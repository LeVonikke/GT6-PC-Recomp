/*
 * ps3recomp - cellSpurs HLE implementation
 *
 * Provides the SPURS management API so games can call SPU task/workload
 * functions without crashing.  Full SPU execution requires recompiling
 * SPU programs; this layer provides the scheduling and management APIs.
 *
 * Tasks and workloads are tracked.  If a game provides PPU fallback
 * callbacks, those can be invoked through the task submission path.
 */

#include "cellSpurs.h"
#include "spu_workload.h"   /* SPU image -> lifted-entry dispatch (runtime/spu) */
#include "spurs_taskset.h"  /* REAL BE CellSpursTaskset layout builders (fork Option-B) */
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Bridge the real (BE) taskset EA + selected taskId from CreateTask to the image-22
 * SPU dispatch (spu_workload.c), so spurs_pm_build_context can build the leaf's
 * SpursTasksetContext from the real taskset. Set right before dispatch_async (the PPU
 * create path is sequential here). Gated by YDKJ_REAL_TASKSET in the dispatch. */
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid     = 0;
uint32_t g_ydkj_real_spurs_ea   = 0;   /* real CellSpurs instance EA (for the taskset-policy handoff) */
void (*cellspurs_ready_count_observer)(u32 spurs_ea, u32 wid) = NULL;

/* Generic HLE adapter passes GUEST addresses; translate pointer args. CellSpurs
 * is treated opaquely by the game (passed back as a handle), so translating the
 * pointer is enough here. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal workload tracking
 * -----------------------------------------------------------------------*/

typedef struct {
    int         in_use;
    const void* pm;
    u32         sizePm;
    u64         data;
    u8          priority[CELL_SPURS_MAX_SPU];
    u32         minContention;
    u32         maxContention;
    u32         readyCount;
    u8          uniqueId;
} SpursWorkload;

typedef struct {
    int         in_use;
    u32         id;
    int         active;
    int         completed;
    s32         exitCode;
    void*       entryPoint;
    u64         argA;
} SpursTask;

/* Workload bookkeeping is per CellSpurs object.  GT6 creates its PDI
 * scheduler at 0x30100000 and later a separate job-chain scheduler; a single
 * global table caused the latter initialization to erase the former's WIDs
 * 5..7 while their raw policies were still executing. */
#define CELLSPURS_MAX_INSTANCES 8
typedef struct {
    u32 ea;
    SpursWorkload workloads[CELL_SPURS_MAX_WORKLOAD];
} SpursWorkloadInstance;
static SpursWorkloadInstance s_workload_instances[CELLSPURS_MAX_INSTANCES];
static SpursTask     s_tasks[CELL_SPURS_MAX_TASK];
static u32           s_next_task_id = 0;

static SpursWorkload* cellspurs_workloads(u32 spurs_ea, int create)
{
    for (u32 i = 0; i < CELLSPURS_MAX_INSTANCES; ++i) {
        if (s_workload_instances[i].ea == spurs_ea)
            return s_workload_instances[i].workloads;
    }
    if (!create)
        return NULL;
    for (u32 i = 0; i < CELLSPURS_MAX_INSTANCES; ++i) {
        if (s_workload_instances[i].ea == 0) {
            s_workload_instances[i].ea = spurs_ea;
            memset(s_workload_instances[i].workloads, 0,
                   sizeof(s_workload_instances[i].workloads));
            return s_workload_instances[i].workloads;
        }
    }
    return NULL;
}

static void cellspurs_clear_workloads(u32 spurs_ea)
{
    for (u32 i = 0; i < CELLSPURS_MAX_INSTANCES; ++i) {
        if (s_workload_instances[i].ea == spurs_ea) {
            memset(&s_workload_instances[i], 0, sizeof(s_workload_instances[i]));
            return;
        }
    }
}

/* CellSpurs is not a native C object in guest memory.  The SPURS policy DMA
 * reads this scheduler layout directly, including the ready counts at offset
 * zero.  Keep HLE validation on real mirror fields so no private "initialized"
 * marker corrupts workload 0's ready count. */
static int cellspurs_guest_is_initialized(u32 spurs_ea)
{
    if (!vm_base)
        return 0;

    const u8 n_spus = vm_base[spurs_ea + 0x76u];
    return n_spus >= 1 && n_spus <= CELL_SPURS_MAX_SPU;
}

static void cellspurs_guest_initialize(u32 spurs_ea, u32 n_spus)
{
    if (n_spus < 1 || n_spus > CELL_SPURS_MAX_SPU)
        n_spus = 1;

    /* These entry points create SPURS1 (CELL_SPURS_SIZE = 0x1000).  SPURS2's
     * 0x2000-byte InitializeWithAttribute2 path is title/runtime-specific. */
    memset(vm_base + spurs_ea, 0, 0x1000u);
    vm_base[spurs_ea + 0x74u] = 0;            /* SPURS1 / 16 workloads */
    vm_base[spurs_ea + 0x76u] = (u8)n_spus;
    vm_base[spurs_ea + 0x77u] = 0xff;         /* no flag receiver */
    /* CellSpurs::sysSrvPreemptWklId[8].  Zero is a valid workload id; the
     * system service uses 0xff as its required "no pre-empted workload"
     * sentinel before running cleanup. */
    memset(vm_base + spurs_ea + 0xC0u, 0xff, CELL_SPURS_MAX_SPU);
    vm_write32(spurs_ea + 0xB0u, 0x0000FFFFu);
}

/* Policy-module instances that use the same PM image must share a unique ID.
 * Different images take the first ID not already used by a live workload. */
static u8 cellspurs_workload_unique_id(const SpursWorkload* workloads,
                                       const void* pm)
{
    u32 used = 0;

    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
        if (!workloads[i].in_use)
            continue;
        if (workloads[i].pm == pm)
            return workloads[i].uniqueId;
        used |= 1u << workloads[i].uniqueId;
    }

    for (u8 id = 0; id < 32; id++) {
        if ((used & (1u << id)) == 0)
            return id;
    }

    return 0;
}

/* Optional title observer.  A native SPURS implementation is still the owner
 * of workload allocation; a title can attach the corresponding lifted policy
 * only after this function has committed the work item to the guest table. */
void (*cellspurs_workload_observer)(u32 spurs_ea, u32 pm_ea, u32 size_pm,
                                    u32 wid, u64 data) = NULL;

/* ---------------------------------------------------------------------------
 * Event flag sync side table
 *
 * We can't embed OS handles in CellSpursEventFlag (game code controls its
 * layout), so we keep a small side table that maps event flag pointers to
 * their host mutex + condition variable.
 * -----------------------------------------------------------------------*/
#define MAX_EVENT_FLAGS 64

typedef struct {
    CellSpursEventFlag* ef;
#ifdef _WIN32
    CRITICAL_SECTION    cs;
    CONDITION_VARIABLE  cv;
#else
    pthread_mutex_t     mtx;
    pthread_cond_t      cond;
#endif
    int                 initialized;
} EventFlagSync;

static EventFlagSync s_ef_sync[MAX_EVENT_FLAGS];

static EventFlagSync* ef_sync_find(CellSpursEventFlag* ef)
{
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (s_ef_sync[i].initialized && s_ef_sync[i].ef == ef)
            return &s_ef_sync[i];
    }
    return NULL;
}

static EventFlagSync* ef_sync_alloc(CellSpursEventFlag* ef)
{
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (!s_ef_sync[i].initialized) {
            s_ef_sync[i].ef = ef;
            s_ef_sync[i].initialized = 1;
#ifdef _WIN32
            InitializeCriticalSection(&s_ef_sync[i].cs);
            InitializeConditionVariable(&s_ef_sync[i].cv);
#else
            pthread_mutex_init(&s_ef_sync[i].mtx, NULL);
            pthread_cond_init(&s_ef_sync[i].cond, NULL);
#endif
            return &s_ef_sync[i];
        }
    }
    return NULL;
}

static void ef_sync_free(EventFlagSync* sync)
{
    if (!sync) return;
#ifdef _WIN32
    DeleteCriticalSection(&sync->cs);
    /* CONDITION_VARIABLE has no destroy on Windows */
#else
    pthread_mutex_destroy(&sync->mtx);
    pthread_cond_destroy(&sync->cond);
#endif
    sync->ef = NULL;
    sync->initialized = 0;
}

static inline void ef_lock(EventFlagSync* s)
{
#ifdef _WIN32
    EnterCriticalSection(&s->cs);
#else
    pthread_mutex_lock(&s->mtx);
#endif
}

static inline void ef_unlock(EventFlagSync* s)
{
#ifdef _WIN32
    LeaveCriticalSection(&s->cs);
#else
    pthread_mutex_unlock(&s->mtx);
#endif
}

/* Returns 1 if signaled, 0 if it timed out after `ms`. */
static inline int ef_wait_timed(EventFlagSync* s, unsigned ms)
{
#ifdef _WIN32
    return SleepConditionVariableCS(&s->cv, &s->cs, ms) ? 1 : 0;
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&s->cond, &s->mtx, &ts) == 0 ? 1 : 0;
#endif
}

static inline void ef_broadcast(EventFlagSync* s)
{
#ifdef _WIN32
    WakeAllConditionVariable(&s->cv);
#else
    pthread_cond_broadcast(&s->cond);
#endif
}

/* =========================================================================
 * SPURS core
 * =====================================================================*/

s32 cellSpursInitialize(CellSpurs* spurs, s32 nSpus, s32 spuPriority,
                        s32 ppuPriority, u8 exitIfNoWork)
{
    (void)spuPriority; (void)ppuPriority; (void)exitIfNoWork;

    if (!spurs)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u32 spurs_ea = (u32)(uintptr_t)spurs;

    printf("[cellSpurs] Initialize(nSpus=%d)\n", nSpus);

    cellspurs_guest_initialize(spurs_ea, (u32)nSpus);

    {
        SpursWorkload* workloads = cellspurs_workloads(spurs_ea, 1);
        if (!workloads) return CELL_SPURS_CORE_ERROR_NOMEM;
        memset(workloads, 0, sizeof(SpursWorkload) * CELL_SPURS_MAX_WORKLOAD);
    }
    return CELL_OK;
}

s32 cellSpursInitializeWithAttribute(CellSpurs* spurs,
                                     const CellSpursAttribute* attr)
{
    if (!spurs || !attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;

    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    const u32 attr_ea = (u32)(uintptr_t)attr;
    const u32 n_spus = vm_read32(attr_ea + 0x08u);
    if (n_spus < 1 || n_spus > CELL_SPURS_MAX_SPU)
        return CELL_SPURS_CORE_ERROR_INVAL;
    printf("[cellSpurs] InitializeWithAttribute(nSpus=%u)\n", n_spus);
    cellspurs_guest_initialize(spurs_ea, n_spus);

    {
        SpursWorkload* workloads = cellspurs_workloads(spurs_ea, 1);
        if (!workloads) return CELL_SPURS_CORE_ERROR_NOMEM;
        memset(workloads, 0, sizeof(SpursWorkload) * CELL_SPURS_MAX_WORKLOAD);
    }
    return CELL_OK;
}

s32 cellSpursFinalize(CellSpurs* spurs)
{
    if (!spurs)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u32 spurs_ea = (u32)(uintptr_t)spurs;

    if (!cellspurs_guest_is_initialized(spurs_ea))
        return CELL_SPURS_CORE_ERROR_STAT;

    printf("[cellSpurs] Finalize()\n");

    cellspurs_clear_workloads(spurs_ea);
    memset(vm_base + spurs_ea, 0, 0x1000u);
    return CELL_OK;
}

static s32 cellspurs_attribute_initialize_guest(CellSpursAttribute* attr,
                                                u32 revision,
                                                u32 sdkVersion, u32 nSpus,
                                                s32 spuPriority,
                                                s32 ppuPriority,
                                                u8 exitIfNoWork)
{
    if (!attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;

    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (attr_ea & 7u)
        return CELL_SPURS_CORE_ERROR_ALIGN;

    memset(vm_base + attr_ea, 0, 0x200u);
    vm_write32(attr_ea + 0x00u, revision);
    vm_write32(attr_ea + 0x04u, sdkVersion);
    vm_write32(attr_ea + 0x08u, nSpus);
    vm_write32(attr_ea + 0x0Cu, (u32)spuPriority);
    vm_write32(attr_ea + 0x10u, (u32)ppuPriority);
    vm_base[attr_ea + 0x14u] = exitIfNoWork ? 1u : 0u;

    printf("[cellSpurs] AttributeInitialize(revision=%u sdk=0x%08X nSpus=%u)\n",
           revision, sdkVersion, nSpus);
    return CELL_OK;
}

/* Compatibility entry point for callers that expose the five-argument public
 * wrapper directly.  Retail SDK code normally imports the seven-argument
 * internal function below. */
s32 cellSpursAttributeInitialize(CellSpursAttribute* attr, s32 nSpus,
                                 s32 spuPriority, s32 ppuPriority,
                                  u8 exitIfNoWork)
{
    return cellspurs_attribute_initialize_guest(attr, 2u, 0u, (u32)nSpus,
                                                spuPriority, ppuPriority,
                                                exitIfNoWork);
}

/* NID 0x95180230: attr, revision, sdkVersion, nSpus, spuPriority,
 * ppuPriority and exitIfNoWork are passed in guest r3..r9 respectively. */
s32 _cellSpursAttributeInitialize(CellSpursAttribute* attr, u32 revision,
                                  u32 sdkVersion, u32 nSpus,
                                  s32 spuPriority, s32 ppuPriority,
                                  u8 exitIfNoWork)
{
    return cellspurs_attribute_initialize_guest(attr, revision, sdkVersion,
                                                nSpus, spuPriority,
                                                ppuPriority, exitIfNoWork);
}

s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute* attr,
                                    const char* prefix, u32 size)
{
    if (!attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u32 attr_ea = (u32)(uintptr_t)attr;
    const char* prefix_h = GUEST_PTR(prefix, const char*);

    if (prefix_h && size > 0) {
        const u32 copyLen = size < 15u ? size : 15u;
        memcpy(vm_base + attr_ea + 0x15u, prefix_h, copyLen);
        vm_write32(attr_ea + 0x24u, copyLen);
    }

    return CELL_OK;
}

s32 cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute* attr,
                                            s32 type)
{
    (void)type;
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    return CELL_OK;
}

s32 cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute* attr)
{
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    return CELL_OK;
}

s32 cellSpursGetNumSpuThread(const CellSpurs* spurs, u32* nThreads)
{
    if (!spurs || !nThreads)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u32 spurs_ea = (u32)(uintptr_t)spurs;

    if (!cellspurs_guest_is_initialized(spurs_ea))
        return CELL_SPURS_CORE_ERROR_STAT;

    vm_write32((u32)(uintptr_t)nThreads, vm_base[spurs_ea + 0x76u]);
    return CELL_OK;
}

s32 cellSpursSetMaxContention(CellSpurs* spurs, CellSpursWorkloadId wid,
                              u32 maxContention)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    SpursWorkload* workloads = cellspurs_workloads((u32)(uintptr_t)spurs, 0);
    if (!workloads || !workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    workloads[wid].maxContention = maxContention;
    return CELL_OK;
}

s32 cellSpursSetPriorities(CellSpurs* spurs, CellSpursWorkloadId wid,
                           const u8* priorities)
{
    if (!spurs || !priorities) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u8* priorities_h = GUEST_PTR(priorities, const u8*);
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    SpursWorkload* workloads = cellspurs_workloads((u32)(uintptr_t)spurs, 0);
    if (!workloads || !workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    memcpy(workloads[wid].priority, priorities_h, CELL_SPURS_MAX_SPU);
    return CELL_OK;
}

s32 cellSpursAttachLv2EventQueue(CellSpurs* spurs, u32 queue, u8* port,
                                 s32 isDynamic)
{
    (void)queue; (void)isDynamic;

    if (!spurs || !port) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    u8* port_h = GUEST_PTR(port, u8*);

    *port_h = 0; /* give it port 0 */
    printf("[cellSpurs] AttachLv2EventQueue(queue=%u)\n", queue);
    return CELL_OK;
}

s32 cellSpursDetachLv2EventQueue(CellSpurs* spurs, u8 port)
{
    (void)port;
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    printf("[cellSpurs] DetachLv2EventQueue(port=%u)\n", port);
    return CELL_OK;
}

/* =========================================================================
 * Taskset
 * =====================================================================*/

s32 cellSpursCreateTaskset(CellSpurs* spurs, CellSpursTaskset* taskset,
                           u64 args, const u8* priority, u32 maxContention)
{
    (void)args; (void)priority; (void)maxContention;

    /* Capture the GUEST EAs (raw register values) BEFORE host translation -- the real
     * BE taskset builder writes to guest memory at these EAs. */
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    uint32_t spurs_ea   = (uint32_t)(uintptr_t)spurs;

    /* Args arrive as guest effective addresses (ps3_hle_call passes raw guest
     * register values); translate to host before dereferencing. */
    spurs   = GUEST_PTR(spurs, CellSpurs*);
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);

    if (!spurs || !taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!cellspurs_guest_is_initialized(spurs_ea))
        return CELL_SPURS_CORE_ERROR_STAT;

    memset(taskset, 0, sizeof(CellSpursTaskset));
    taskset->initialized = 1;
    taskset->spurs = spurs;

    /* Write the REAL big-endian CellSpursTaskset layout (fork Option-B) so the lifted
     * SPU leaf + spurs_pm_build_context read valid data (the native writes above are
     * little-endian = garbage to the SPU). Overwrites 0x00-0x80 with BE fields. */
    spurs_taskset_init(taskset_ea, spurs_ea, args, /*wid*/0,
                       (uint32_t)sizeof(CellSpursTaskset), /*evf1*/0, /*evf2*/0);
    g_ydkj_real_taskset_ea = taskset_ea;

    g_ydkj_real_spurs_ea = spurs_ea;   /* capture for the taskset-policy handoff (LS[0x1C0]) */
    printf("[cellSpurs] CreateTaskset() ea=0x%08X spurs=0x%08X (real BE layout)\n", taskset_ea, spurs_ea);
    return CELL_OK;
}

s32 cellSpursCreateTasksetWithAttribute(CellSpurs* spurs,
                                        CellSpursTaskset* taskset,
                                        const CellSpursTasksetAttribute* attr)
{
    (void)attr;
    return cellSpursCreateTaskset(spurs, taskset, 0, NULL, 0);
}

s32 cellSpursDestroyTaskset(CellSpursTaskset* taskset)
{
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] DestroyTaskset()\n");
    taskset->initialized = 0;
    return CELL_OK;
}

s32 cellSpursShutdownTaskset(CellSpursTaskset* taskset)
{
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] ShutdownTaskset()\n");
    taskset->shutdownRequested = 1;
    return CELL_OK;
}

s32 cellSpursJoinTaskset(CellSpursTaskset* taskset)
{
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] JoinTaskset()\n");
    /* In a full implementation, wait for all tasks to complete */
    return CELL_OK;
}

s32 cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr)
{
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTasksetAttribute*);
    memset(attr, 0, sizeof(CellSpursTasksetAttribute));
    attr->revision = 1;
    return CELL_OK;
}

s32 cellSpursTasksetAttributeSetName(CellSpursTasksetAttribute* attr,
                                      const char* name)
{
    (void)name;
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    return CELL_OK;
}

/* =========================================================================
 * Task
 * =====================================================================*/

s32 cellSpursCreateTask(CellSpursTaskset* taskset, CellSpursTaskId* taskId,
                        void* elf, void* context, u32 sizeContext,
                        CellSpursTaskAttribute* attr)
{
    (void)context; (void)sizeContext; (void)attr;

    /* Capture guest EAs BEFORE host translation (the real BE taskset builder + the
     * SPU DMA use guest EAs). */
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    uint32_t elf_ea     = (uint32_t)(uintptr_t)elf;
    uint32_t context_ea = (uint32_t)(uintptr_t)context;

    /* taskId/taskset are guest EAs; translate before deref. elf/context stay
     * guest EAs (handled below — elf is translated for load, context kept EA). */
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    CellSpursTaskId* taskId_h = GUEST_PTR(taskId, CellSpursTaskId*);

    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!g_ydkj_real_taskset_ea)   /* real-BE init flag (native ->initialized clobbered by BE layout) */
        return CELL_SPURS_TASK_ERROR_STAT;

    /* Find a free task slot */
    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (!s_tasks[i].in_use) {
            s_tasks[i].in_use = 1;
            s_tasks[i].id = s_next_task_id++;
            s_tasks[i].active = 1;
            s_tasks[i].completed = 0;
            s_tasks[i].exitCode = 0;
            s_tasks[i].entryPoint = elf;

            if (taskId_h) *taskId_h = s_tasks[i].id;
            taskset->taskCount++;

            /* Register the task in the REAL BE taskset: writes task_info[slot]
             * (args/elf/context/ls_pattern) + sets enabled+ready bits so the PM's
             * SELECT_TASK picks it. Slot index i = the SPURS taskId (bitset bit). */
            spurs_taskset_add_task(taskset_ea, i, (uint64_t)elf_ea,
                                   (uint64_t)context_ea, /*arg*/NULL, /*ls_pattern*/NULL);
            /* Bridge to the image-22 dispatch so build_context uses this taskset+task. */
            g_ydkj_real_taskset_ea = taskset_ea;
            g_ydkj_real_taskid     = i;

            printf("[cellSpurs] CreateTask(id=%u, entry=%p) - task logged\n",
                   s_tasks[i].id, elf);

            /* Run the task's SPU program if a lifted build is registered for it.
             * The registry maps the task ELF (by content fingerprint) to its
             * pre-lifted native entry; dispatch loads the ELF into a local store
             * and runs it with the task arg in r3. INERT until the title
             * registers its lifted SPU set: an unregistered image MISSes and
             * returns 0, preserving the prior "track only" behaviour.
             *
             * NOTE: dispatch is synchronous (runs to completion inline). That
             * suits create+join task patterns; a workload/taskset whose SPU job
             * waits on concurrent PPU-side signals will want the async lv2
             * SPU-thread path instead — wired when a title exercises it. */
            if (elf) {
                /* elf/context are guest effective addresses; translate the image
                 * pointer to host memory for fingerprint+load, but keep context
                 * as the guest EA (the SPU job's DMA uses guest EAs / r3). */
                const uint8_t* host_elf = GUEST_PTR(elf, const uint8_t*);
                size_t sz = spu_elf_image_size(host_elf, 2u * 1024 * 1024);
                if (sz)
                    /* Async: SPURS tasks are persistent workers — running them
                     * inline would block this PPU thread forever (deadlock). */
                    spu_workload_dispatch_async(host_elf, (uint32_t)sz,
                                                (uint32_t)(uintptr_t)context);
            }
            return CELL_OK;
        }
    }

    return CELL_SPURS_TASK_ERROR_NOMEM;
}

/* The SDK's versioned task-attribute initializer. ABI (8 GPR args):
 *   r3=attr r4=revision r5=sdkVersion r6=eaElf r7=eaContext r8=sizeContext
 *   r9=lsPattern r10=argument
 * Stash the task ELF EA + context so cellSpursCreateTaskWithAttribute can
 * dispatch the SPU job. */
s32 _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr, u32 revision,
                                      u32 sdkVersion, u64 eaElf, u64 eaContext,
                                      u32 sizeContext, const void* lsPattern,
                                      const void* argument)
{
    (void)sdkVersion; (void)lsPattern; (void)argument;
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTaskAttribute*);
    memset(attr, 0, sizeof(CellSpursTaskAttribute));
    attr->revision    = revision;
    attr->sizeContext = sizeContext;
    attr->eaContext   = eaContext;
    attr->eaElf       = eaElf;
    printf("[cellSpurs] _TaskAttributeInitialize(eaElf=0x%08X ctx=0x%08X szctx=%u)\n",
           (u32)eaElf, (u32)eaContext, sizeContext);
    return CELL_OK;
}

/* Create a task from a pre-initialized attribute (carries ELF EA + context).
 * Forwards to cellSpursCreateTask, which translates taskset/taskId and runs the
 * SPU image through spu_workload_dispatch. */
s32 cellSpursCreateTaskWithAttribute(CellSpursTaskset* taskset,
                                     CellSpursTaskId* taskId,
                                     CellSpursTaskAttribute* attr)
{
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    CellSpursTaskAttribute* attr_h = GUEST_PTR(attr, CellSpursTaskAttribute*);
    /* taskset/taskId forwarded raw (callee translates); elf/context are guest EAs. */
    return cellSpursCreateTask(taskset, taskId,
                               (void*)(uintptr_t)(u32)attr_h->eaElf,
                               (void*)(uintptr_t)(u32)attr_h->eaContext,
                               attr_h->sizeContext, attr);
}

/* The SDK's versioned taskset-attribute initializer. We forward taskset creation
 * through CreateTaskset (which ignores the attribute), so just zero the struct. */
s32 _cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr,
                                         u32 revision, u32 sdkVersion, u64 argTaskset,
                                         u64 priority, u32 maxContention)
{
    (void)sdkVersion; (void)argTaskset; (void)priority; (void)maxContention;
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTasksetAttribute*);
    memset(attr, 0, sizeof(CellSpursTasksetAttribute));
    attr->revision = revision ? revision : 1;
    printf("[cellSpurs] _TasksetAttributeInitialize(rev=%u)\n", revision);
    return CELL_OK;
}

s32 cellSpursJoinTask(CellSpursTaskset* taskset, CellSpursTaskId taskId,
                      s32* exitCode)
{
    (void)taskset;
    s32* exitCode_h = GUEST_PTR(exitCode, s32*);

    printf("[cellSpurs] JoinTask(id=%u)\n", taskId);

    /* Find the task and mark as completed */
    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (s_tasks[i].in_use && s_tasks[i].id == taskId) {
            s_tasks[i].completed = 1;
            s_tasks[i].active = 0;
            if (exitCode_h)
                *exitCode_h = s_tasks[i].exitCode;
            s_tasks[i].in_use = 0;
            return CELL_OK;
        }
    }

    return CELL_SPURS_TASK_ERROR_SRCH;
}

s32 cellSpursSendSignal(CellSpursTaskset* taskset, CellSpursTaskId taskId)
{
    (void)taskset;

    printf("[cellSpurs] SendSignal(id=%u)\n", taskId);

    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (s_tasks[i].in_use && s_tasks[i].id == taskId) {
            /* In a real implementation, signal the task's wait condition */
            return CELL_OK;
        }
    }

    return CELL_SPURS_TASK_ERROR_SRCH;
}

s32 cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr)
{
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTaskAttribute*);
    memset(attr, 0, sizeof(CellSpursTaskAttribute));
    attr->revision = 1;
    return CELL_OK;
}

/* =========================================================================
 * Workload
 * =====================================================================*/

s32 cellSpursAddWorkload(CellSpurs* spurs, CellSpursWorkloadId* wid,
                         const void* pm, u32 sizePm, u64 data,
                         const u8* priority, u32 minContention,
                         u32 maxContention)
{
    if (!spurs || !wid)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    /* spurs/wid/priority are guest EAs; pm stays a guest EA (it's the SPU
     * program address consumed later by the workload dispatch). */
    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    const u8* priority_h = GUEST_PTR(priority, const u8*);

    if (!cellspurs_guest_is_initialized(spurs_ea))
        return CELL_SPURS_CORE_ERROR_STAT;
    SpursWorkload* workloads = cellspurs_workloads(spurs_ea, 1);
    if (!workloads)
        return CELL_SPURS_CORE_ERROR_NOMEM;

    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
        if (!workloads[i].in_use) {
            workloads[i].uniqueId = cellspurs_workload_unique_id(workloads, pm);
            workloads[i].in_use = 1;
            workloads[i].pm = pm;
            workloads[i].sizePm = sizePm;
            workloads[i].data = data;
            workloads[i].minContention = minContention;
            workloads[i].maxContention = maxContention;
            workloads[i].readyCount = 0;

            if (priority_h)
                memcpy(workloads[i].priority, priority_h, CELL_SPURS_MAX_SPU);
            else
                memset(workloads[i].priority, 0, CELL_SPURS_MAX_SPU);

            /* `wid` is guest memory.  The recompiled PPU reads it through
             * vm_read32, so a native host-endian store turns workload 5 into
             * 0x05000000 and makes subsequent SPURS calls reject it. */
            vm_write32((u32)(uintptr_t)wid, i);

            /* Keep the real 0x2000-byte BE CellSpurs scheduler mirror in
             * sync with the host bookkeeping.  Raw policy modules (GT6 PDI
             * included) DMA this table directly; the compact native
             * CellSpurs C struct above is only an HLE convenience and cannot
             * stand in for its guest layout. */
            if (vm_base) {
                const u32 info = spurs_ea + 0xB00u + i * 0x20u;
                vm_write64(info + 0x00, (u64)(uintptr_t)pm);
                vm_write64(info + 0x08, data);
                vm_write32(info + 0x10, sizePm);
                vm_base[info + 0x14] = workloads[i].uniqueId;
                memcpy(vm_base + info + 0x18, workloads[i].priority, 8);

                vm_base[spurs_ea + 0x80u + i] = 2;  /* RUNNABLE */
                vm_base[spurs_ea + 0x40u + i] = (u8)minContention;
                vm_base[spurs_ea + 0x50u + i] = (u8)maxContention;
                vm_write32(spurs_ea + 0xB0u,
                    vm_read32(spurs_ea + 0xB0u) | (1u << (31u - i)));

                /* Publishing a workload is a system-service request, not
                 * merely a descriptor write.  Firmware marks every live SPU
                 * in both sysSrvMsgUpdateWorkload (+0xBD) and sysSrvMessage
                 * (+0x72); the WID-32 service then rebuilds its runnable
                 * mirror before Kernel1 is allowed to dispatch the policy.
                 * Without these two bits the lifted kernel sees an old local
                 * table and has to rely on host-side re-selection workarounds.
                 * This is guest scheduler state, never a synthetic PPU event. */
                {
                    const u8 n_spus = vm_base[spurs_ea + 0x76u];
                    const u8 active_mask = n_spus >= 8u ? 0xffu :
                        n_spus ? (u8)((1u << n_spus) - 1u) : 0u;
                    vm_base[spurs_ea + 0xBDu] |= active_mask;
                    vm_base[spurs_ea + 0x72u] |= active_mask;
                }
            }
            printf("[cellSpurs] AddWorkload(wid=%u, pm=%p, size=%u)\n",
                   i, pm, sizePm);
            if (cellspurs_workload_observer)
                cellspurs_workload_observer(spurs_ea, (u32)(uintptr_t)pm, sizePm,
                                            i, data);
            return CELL_OK;
        }
    }

    return CELL_SPURS_CORE_ERROR_NOMEM;
}

s32 cellSpursAddWorkloadWithAttribute(CellSpurs* spurs,
                                       CellSpursWorkloadId* wid,
                                       const CellSpursWorkloadAttribute* attr)
{
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u32 attr_ea = (u32)(uintptr_t)attr;

    /* Attributes are a documented big-endian guest layout.  Reading them
     * through the native C struct byte-swaps every integer on x86, yielding
     * impossible PM sizes such as 0x80300000 and preventing SPU dispatch. */
    const u64 pm = vm_read64(attr_ea + 0x08);
    const u32 size_pm = vm_read32(attr_ea + 0x10);
    const u64 data = vm_read64(attr_ea + 0x18);
    const u32 min_contention = vm_read32(attr_ea + 0x28);
    const u32 max_contention = vm_read32(attr_ea + 0x2c);

    /* spurs/wid remain guest EAs for cellSpursAddWorkload; only the attribute
     * fields are decoded here. */
    return cellSpursAddWorkload(spurs, wid,
                               (const void*)(uintptr_t)(u32)pm,
                               size_pm, data,
                               (const u8*)(uintptr_t)(attr_ea + 0x20),
                               min_contention, max_contention);
}

s32 cellSpursRemoveWorkload(CellSpurs* spurs, CellSpursWorkloadId wid)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    SpursWorkload* workloads = cellspurs_workloads((u32)(uintptr_t)spurs, 0);
    if (!workloads || !workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    workloads[wid].in_use = 0;
    printf("[cellSpurs] RemoveWorkload(wid=%u)\n", wid);
    return CELL_OK;
}

s32 cellSpursGetWorkloadInfo(CellSpurs* spurs, CellSpursWorkloadId wid,
                              CellSpursWorkloadInfo* info)
{
    if (!spurs || !info)
        return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;

    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    const u32 info_ea = (u32)(uintptr_t)info;

    if ((spurs_ea & 0x7fu) != 0)
        return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;

    const int is_spurs2 = (vm_base[spurs_ea + 0x74u] & 0x40u) != 0;
    const u32 max_workloads = is_spurs2 ? 32u : 16u;
    if (wid >= max_workloads)
        return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;

    if ((vm_read32(spurs_ea + 0xB0u) & (0x80000000u >> wid)) == 0)
        return CELL_SPURS_POLICY_MODULE_ERROR_SRCH;

    if (vm_read32(spurs_ea + 0xD6Cu) != 0)
        return CELL_SPURS_POLICY_MODULE_ERROR_STAT;

    const u32 slot = wid & 0x0fu;
    const u32 workload_ea = spurs_ea + (wid < 16u ? 0xB00u : 0x1000u)
                          + slot * 0x20u;
    const u32 runtime_ea = spurs_ea + (wid < 16u ? 0x100u : 0x1200u)
                         + slot * 0x80u;
    const u32 names_ea = spurs_ea + (wid < 16u ? 0xE00u : 0x1A00u)
                       + slot * 0x10u;
    const u8 packed_contention = vm_base[spurs_ea + 0x20u + slot];
    const u8 packed_max = vm_base[spurs_ea + 0x50u + slot];
    const u16 signals = vm_read16(spurs_ea + (wid < 16u ? 0x70u : 0x78u));
    const u16 signal_mask = (u16)(0x8000u >> slot);

    memset(vm_base + info_ea, 0, sizeof(CellSpursWorkloadInfo));
    vm_write64(info_ea + 0x00u, vm_read64(workload_ea + 0x08u));
    memcpy(vm_base + info_ea + 0x08u, vm_base + workload_ea + 0x18u,
           CELL_SPURS_MAX_SPU);
    vm_write32(info_ea + 0x10u, (u32)vm_read64(workload_ea + 0x00u));
    vm_write32(info_ea + 0x14u, vm_read32(workload_ea + 0x10u));
    vm_write32(info_ea + 0x18u, (u32)vm_read64(names_ea + 0x00u));
    vm_write32(info_ea + 0x1Cu, (u32)vm_read64(names_ea + 0x08u));
    vm_base[info_ea + 0x20u] = !is_spurs2
        ? packed_contention
        : (wid < 16u ? (u8)(packed_contention & 0x0fu)
                     : (u8)(packed_contention >> 4));
    vm_base[info_ea + 0x21u] = is_spurs2
        ? 0
        : vm_base[spurs_ea + 0x40u + slot];
    vm_base[info_ea + 0x22u] = !is_spurs2
        ? packed_max
        : (wid < 16u ? (u8)(packed_max & 0x0fu)
                     : (u8)(packed_max >> 4));
    vm_base[info_ea + 0x23u] = vm_base[spurs_ea
        + (wid < 16u ? 0x00u : 0x10u) + slot];
    vm_base[info_ea + 0x24u] = is_spurs2
        ? 0
        : vm_base[spurs_ea + 0x10u + slot];
    vm_base[info_ea + 0x25u] = (signals & signal_mask) != 0;
    vm_write32(info_ea + 0x28u, (u32)vm_read64(runtime_ea + 0x30u));
    vm_write32(info_ea + 0x2Cu, (u32)vm_read64(runtime_ea + 0x38u));

    return CELL_OK;
}

s32 cellSpursWorkloadAttributeInitialize(CellSpursWorkloadAttribute* attr,
                                         u32 revision, u32 sdkVersion,
                                         const void* pm, u32 sizePm,
                                         u64 data, const u8* priority,
                                         u32 minContention,
                                         u32 maxContention)
{
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursWorkloadAttribute*);
    const u8* priority_h = GUEST_PTR(priority, const u8*);

    memset(attr, 0, sizeof(CellSpursWorkloadAttribute));
    attr->revision = revision;
    attr->sdkVersion = sdkVersion;
    attr->pm = (u64)(uintptr_t)pm;   /* pm kept as guest EA */
    attr->sizePm = sizePm;
    attr->data = data;
    attr->minContention = minContention;
    attr->maxContention = maxContention;

    if (priority_h)
        memcpy(attr->priority, priority_h, CELL_SPURS_MAX_SPU);

    return CELL_OK;
}

s32 cellSpursReadyCountStore(CellSpurs* spurs, CellSpursWorkloadId wid,
                             u32 value)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD || value > 0xffu)
        return CELL_SPURS_CORE_ERROR_INVAL;

    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    SpursWorkload* workloads = cellspurs_workloads(spurs_ea, 0);
    if (!workloads || !workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;
    vm_base[spurs_ea + wid] = (u8)value;
    workloads[wid].readyCount = (u8)value;
    if (getenv("GT6_SPURS_WAKE_TRACE"))
        fprintf(stderr, "[GT6 SPURS WAKE] ready-store spurs=%08X wid=%u value=%u\n",
                spurs_ea, wid, value);
    if (cellspurs_ready_count_observer)
        cellspurs_ready_count_observer(spurs_ea, wid);
    return CELL_OK;
}

s32 cellSpursReadyCountSwap(CellSpurs* spurs, CellSpursWorkloadId wid,
                            u32* old, u32 value)
{
    if (!spurs || !old) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD || value > 0xffu)
        return CELL_SPURS_CORE_ERROR_INVAL;
    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    SpursWorkload* workloads = cellspurs_workloads(spurs_ea, 0);
    if (!workloads || !workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;
    const u32 old_value = vm_base[spurs_ea + wid];
    vm_base[spurs_ea + wid] = (u8)value;
    workloads[wid].readyCount = (u8)value;
    vm_write32((u32)(uintptr_t)old, old_value);
    if (getenv("GT6_SPURS_WAKE_TRACE"))
        fprintf(stderr, "[GT6 SPURS WAKE] ready-swap spurs=%08X wid=%u %u->%u\n",
                spurs_ea, wid, old_value, value);
    if (cellspurs_ready_count_observer)
        cellspurs_ready_count_observer(spurs_ea, wid);
    return CELL_OK;
}

s32 cellSpursReadyCountCompareAndSwap(CellSpurs* spurs,
                                       CellSpursWorkloadId wid,
                                       u32* old, u32 compare, u32 value)
{
    if (!spurs || !old) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD || (compare | value) > 0xffu)
        return CELL_SPURS_CORE_ERROR_INVAL;
    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    SpursWorkload* workloads = cellspurs_workloads(spurs_ea, 0);
    if (!workloads || !workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;
    const u32 old_value = vm_base[spurs_ea + wid];
    if (old_value == compare) {
        vm_base[spurs_ea + wid] = (u8)value;
        workloads[wid].readyCount = (u8)value;
    } else {
        workloads[wid].readyCount = old_value;
    }
    vm_write32((u32)(uintptr_t)old, old_value);
    if (getenv("GT6_SPURS_WAKE_TRACE"))
        fprintf(stderr,
                "[GT6 SPURS WAKE] ready-cas spurs=%08X wid=%u old=%u compare=%u new=%u\n",
                spurs_ea, wid, old_value, compare, value);
    if (old_value == compare && cellspurs_ready_count_observer)
        cellspurs_ready_count_observer(spurs_ea, wid);

    return CELL_OK;
}

s32 cellSpursSendWorkloadSignal(CellSpurs* spurs, CellSpursWorkloadId wid)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    SpursWorkload* workloads = cellspurs_workloads(spurs_ea, 0);
    if (!workloads || !workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    const u32 signal_ea = spurs_ea + 0x70u;
    vm_write16(signal_ea,
               (u16)(vm_read16(signal_ea) | (u16)(0x8000u >> wid)));
    if (getenv("GT6_SPURS_WAKE_TRACE"))
        fprintf(stderr, "[GT6 SPURS WAKE] signal spurs=%08X wid=%u\n",
                (u32)(uintptr_t)spurs, wid);
    return CELL_OK;
}

s32 cellSpursWakeUp(CellSpurs* spurs)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    /* In a full implementation, wake the worker threads */
    if (getenv("GT6_SPURS_WAKE_TRACE"))
        fprintf(stderr, "[GT6 SPURS WAKE] wake spurs=%08X\n",
                (u32)(uintptr_t)spurs);
    return CELL_OK;
}

/* =========================================================================
 * Event flags
 * =====================================================================*/

s32 cellSpursEventFlagInitialize(CellSpursTaskset* taskset,
                                 CellSpursEventFlag* eventFlag,
                                 u32 clearMode, u32 direction)
{
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    (void)taskset;

    if (!eventFlag)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    /* If this event flag was previously initialized, free old sync slot */
    EventFlagSync* old = ef_sync_find(eventFlag);
    if (old) ef_sync_free(old);

    memset(eventFlag, 0, sizeof(CellSpursEventFlag));
    eventFlag->initialized = 1;
    eventFlag->clearMode = (u16)clearMode;
    eventFlag->direction = (u16)direction;
    eventFlag->bits = 0;

    EventFlagSync* sync = ef_sync_alloc(eventFlag);
    if (!sync) {
        printf("[cellSpurs] EventFlagInitialize: no free sync slots!\n");
        return CELL_SPURS_TASK_ERROR_NOMEM;
    }

    printf("[cellSpurs] EventFlagInitialize(clearMode=%u, direction=%u)\n",
           clearMode, direction);
    return CELL_OK;
}

s32 cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag* eventFlag)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    if (!eventFlag) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    printf("[cellSpurs] EventFlagAttachLv2EventQueue()\n");
    return CELL_OK;
}

s32 cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag* eventFlag)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    if (!eventFlag) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    printf("[cellSpurs] EventFlagDetachLv2EventQueue()\n");
    return CELL_OK;
}

s32 cellSpursEventFlagSet(CellSpursEventFlag* eventFlag, u16 bits)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    if (!eventFlag)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!eventFlag->initialized)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_find(eventFlag);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    ef_lock(sync);
    eventFlag->bits |= bits;
    ef_broadcast(sync);
    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagWait(CellSpursEventFlag* eventFlag, u16* bits,
                           u32 mode)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    bits = GUEST_PTR(bits, u16*);
    if (!eventFlag || !bits)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!eventFlag->initialized)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_find(eventFlag);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    u16 pattern = *bits;

    ef_lock(sync);

    /* Block until the requested bit pattern is satisfied */
    /* SPU-completion shim: the bits this wait expects are normally set by an SPU
     * workload via cellSpursEventFlagSet. We don't execute SPU code, so after a
     * short grace period with no signal we force-satisfy the pattern (as if the
     * SPU finished) so the title's boot proceeds. Correct fix is SPU execution. */
    unsigned timeouts = 0;
    for (;;) {
        u16 current = eventFlag->bits;

        if (mode == CELL_SPURS_EVENT_FLAG_AND) {
            if ((current & pattern) == pattern)
                break;
        } else {
            /* OR mode: any requested bit set */
            if ((current & pattern) != 0)
                break;
        }

        if (!ef_wait_timed(sync, 50) && ++timeouts >= 4) {
            fprintf(stderr, "[cellSpurs] EventFlagWait: no SPU signal for pattern "
                            "0x%04X -- force-satisfying (SPU not executed)\n", pattern);
            eventFlag->bits |= pattern;
        }
    }

    *bits = eventFlag->bits;

    /* Auto-clear if configured */
    if (eventFlag->clearMode == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
        eventFlag->bits &= ~pattern;

    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagTryWait(CellSpursEventFlag* eventFlag, u16* bits,
                              u32 mode)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    bits = GUEST_PTR(bits, u16*);
    if (!eventFlag || !bits)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!eventFlag->initialized)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_find(eventFlag);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    u16 pattern = *bits;

    ef_lock(sync);

    u16 current = eventFlag->bits;

    if (mode == CELL_SPURS_EVENT_FLAG_AND) {
        if ((current & pattern) != pattern) {
            ef_unlock(sync);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }
    } else {
        if ((current & pattern) == 0) {
            ef_unlock(sync);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }
    }

    *bits = current;

    if (eventFlag->clearMode == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
        eventFlag->bits &= ~pattern;

    ef_unlock(sync);
    return CELL_OK;
}

s32 cellSpursEventFlagClear(CellSpursEventFlag* eventFlag, u16 bits)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    if (!eventFlag)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!eventFlag->initialized)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_find(eventFlag);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    ef_lock(sync);
    eventFlag->bits &= ~bits;
    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagGetDirection(CellSpursEventFlag* eventFlag,
                                   u32* direction)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    direction = GUEST_PTR(direction, u32*);
    if (!eventFlag || !direction)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    *direction = eventFlag->direction;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle (from RPCS3 audit)
 * -----------------------------------------------------------------------*/

/* _cellSpursEventFlagInitialize — internal init with more parameters */
s32 _cellSpursEventFlagInitialize(void* spurs, void* taskset,
                                    CellSpursEventFlag* eventFlag,
                                    u32 clearMode, u32 direction)
{
    (void)spurs; (void)taskset;
    printf("[cellSpurs] _EventFlagInitialize(clearMode=%u, dir=%u)\n",
           clearMode, direction);
    if (!eventFlag) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    /* Forward raw guest pointers; cellSpursEventFlagInitialize translates them
     * (translating here too would double-translate -> out-of-bounds). */
    return cellSpursEventFlagInitialize((CellSpursTaskset*)taskset, eventFlag, clearMode, direction);
}

/* _cellSpursSendSignal — internal signal delivery */
s32 _cellSpursSendSignal(void* taskset, u32 taskId)
{
    (void)taskset;
    printf("[cellSpurs] _SendSignal(taskId=%u)\n", taskId);
    /* In recomp without SPU execution, signals are no-ops */
    return CELL_OK;
}

/* cellSpursRunJobChain — start a job chain execution */
s32 cellSpursRunJobChain(void* spurs, void* jobChain)
{
    (void)spurs; (void)jobChain;
    printf("[cellSpurs] RunJobChain() — stub (no SPU execution)\n");
    /* Job chains run on SPUs. Without SPU execution, we stub this.
     * Games that depend on job chain completion will need the jobs
     * to be HLE'd or run on host threads. */
    return CELL_OK;
}

/* cellSpursKickJobChain — kick a running job chain */
s32 cellSpursKickJobChain(void* spurs, void* jobChain)
{
    (void)spurs; (void)jobChain;
    return CELL_OK;
}
