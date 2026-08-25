/* C linkage bridge for GT6's raw CellSpurs policy module. */
#include "spu_policy/spu_recomp.h"
#include "spu_kernel/spu_recomp.h"
#include "../runtime/spu/spu_lifted_job.h"
#include "../runtime/spu/spu_dma.h"
#include "../runtime/syscalls/sys_event.h"
/* spu_lifted_job.h supplies the image-24 policy ABI, r4=CellSpurs, and its
 * short, bounded bootstrap run guard. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;
void spu_begin_image(int image_id);

#define GT6_PDI_POLICY_SIZE 0x2F80u
#define GT6_PDI_POLICY_LS_BASE 0x0A00u
#define GT6_PDI_POLICY_IMAGE_ID 24
#define GT6_SPURS_KERNEL_IMAGE_ID 25
#define GT6_PDI_WORKER_COUNT 2u
#define GT6_SPURS_KERNEL_CONTEXT_LS 0x0100u
#define GT6_SPURS_KERNEL_CONTEXT_SIZE 0x0190u
#define GT6_SPURS_KERNEL_DMA_TAG 31u
#define GT6_SPURS_KERNEL1_EXIT_LS 0x0808u
#define GT6_SPURS_SELECT_WORKLOAD_LS 0x0290u
/* x1F4 is otherwise unused by the minimal context.  Keep the poll bits here
 * so the HLE for the absent kernel callback at 0x290 can return the same
 * {current wid, poll status} pair that dispatched this policy. */
#define GT6_SPURS_POLL_STATUS_LS 0x01F4u

typedef struct gt6_pdi_policy_job {
    uint32_t policy_ea;
    uint32_t spurs_ea;
    uint64_t workload_data;
    uint32_t workload_id;
    uint32_t ready_queue;
    LONG active_limit;
    uint8_t image[GT6_PDI_POLICY_SIZE];
} gt6_pdi_policy_job;

void gt6_pdi_policy_start(uint32_t policy_ea, uint32_t policy_size,
                          uint32_t spurs_ea, uint32_t ready_queue,
                          uint64_t workload_data, uint32_t workload_id);

static volatile LONG s_policy_started_mask = 0;
/* A PDI scheduler owns two physical SPU lanes.  Keeping a launch bit for
 * every guest workload is useful for coalescing duplicate ReadyCount edges,
 * but it must not turn each published workload into another simultaneous
 * raw-policy instance: those instances share the same CellSpurs lock lines
 * and race the PPU callbacks. */
static volatile LONG s_policy_active_count = 0;
static volatile LONG s_scheduler_tick_started[64];
/* The PDI scheduler is attached first to its bootstrap queue and then moves to
 * the live scheduler queue.  A real SPU port moves with that attachment; it
 * does not keep raising the detached port forever. */
static volatile LONG s_scheduler_tick_queue = 0;
/* Kernel1 keeps one local contention vector per physical SPU.  The guest
 * mirror stores total contention, while each SPU subtracts its own current
 * allocation before selecting again. */
static uint8_t s_kernel_local_contention[GT6_PDI_WORKER_COUNT][16];
static CRITICAL_SECTION s_kernel_select_lock;
static INIT_ONCE s_kernel_select_lock_once = INIT_ONCE_STATIC_INIT;

/* The real SPURS kernel is one of the two tiny SPU ELFs embedded in the
 * user's decrypted libsre.prx.  Its load segment is fixed by the firmware
 * image: file 0x20480 contains the ELF and its LS 0x100..0x87f segment starts
 * at file 0x20500.  The PDI policy owns LS 0x100 as its live KernelContext, so
 * do not copy the kernel text over it: the lifted host code supplies text, and
 * only its constants at LS 0x850..0x87f need materialising.  Keep firmware
 * external instead of embedding its bytes in the executable. */
static int gt6_pdi_load_spurs_kernel(uint8_t* local_store)
{
    static const char* const candidates[] = {
        "fw_spu\\libsre.prx",
        "..\\..\\..\\..\\fw_spu\\libsre.prx",
    };
    const char* configured = getenv("GT6_LIBSRE_PRX");
    const char* path = configured && configured[0] ? configured : NULL;
    FILE* fp = path ? fopen(path, "rb") : NULL;
    for (size_t i = 0; !fp && i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        path = candidates[i];
        fp = fopen(path, "rb");
    }
    if (!fp)
        return 0;

    uint8_t elf_magic[4];
    if (fseek(fp, 0x20480L, SEEK_SET) != 0 ||
        fread(elf_magic, 1, sizeof(elf_magic), fp) != sizeof(elf_magic) ||
        memcmp(elf_magic, "\x7f" "ELF", sizeof(elf_magic)) != 0 ||
        fseek(fp, 0x20C50L, SEEK_SET) != 0 ||
        fread(local_store + 0x850u, 1, 0x30u, fp) != 0x30u) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return 1;
}

static BOOL CALLBACK gt6_pdi_init_kernel_lock(PINIT_ONCE once, PVOID param,
                                              PVOID* context)
{
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&s_kernel_select_lock);
    return TRUE;
}

static uint32_t gt6_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void gt6_write_be32(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint64_t gt6_be64(const uint8_t* p)
{
    return ((uint64_t)gt6_be32(p) << 32) | gt6_be32(p + 4);
}

static void gt6_ls_be16(uint8_t* ls, uint32_t offset, uint16_t value)
{
    ls[offset + 0] = (uint8_t)(value >> 8);
    ls[offset + 1] = (uint8_t)value;
}

static void gt6_ls_be32(uint8_t* ls, uint32_t offset, uint32_t value)
{
    ls[offset + 0] = (uint8_t)(value >> 24);
    ls[offset + 1] = (uint8_t)(value >> 16);
    ls[offset + 2] = (uint8_t)(value >> 8);
    ls[offset + 3] = (uint8_t)value;
}

static void gt6_ls_be64(uint8_t* ls, uint32_t offset, uint64_t value)
{
    gt6_ls_be32(ls, offset, (uint32_t)(value >> 32));
    gt6_ls_be32(ls, offset + 4, (uint32_t)value);
}

/* Build the part of the SDK's SpursKernelContext that a raw workload policy
 * is allowed to consume.  Its fixed layout occupies LS 0x100..0x28F.  The
 * first 0x80 bytes are the kernel's lock-line scratch copy; the remaining
 * fields describe the current CellSpurs instance and workload. */
static uint32_t gt6_pdi_build_kernel_context(uint8_t* ls,
                                             const gt6_pdi_policy_job* job)
{
    const uint32_t spurs = job->spurs_ea;
    const uint32_t wid = job->workload_id;
    const uint32_t spu_num = wid % GT6_PDI_WORKER_COUNT;
    uint32_t poll_status = 0;
    uint16_t runnable = 0;
    uint64_t current_addr = job->policy_ea;
    uint8_t current_unique_id = 0;
    uint16_t signals = 0;

    memset(ls + GT6_SPURS_KERNEL_CONTEXT_LS, 0,
           GT6_SPURS_KERNEL_CONTEXT_SIZE);

    if (vm_base && mfc_ea_range_committed(spurs, 0x2000u)) {
        memcpy(ls + 0x100u, vm_base + spurs, 0x80u);

        for (uint32_t i = 0; i < 16u; ++i) {
            const uint32_t info = spurs + 0xB00u + i * 0x20u;
            const uint8_t unique_id = vm_base[info + 0x14u];

            /* Kernel1 transposes the per-SPU priority into its local
             * per-workload vector and inverts it: SDK priority 1 is the
             * highest priority, whereas the kernel selector assigns it
             * weight 0x0f.  Passing the raw SDK byte here made the raw PDI
             * policy observe the opposite ordering. */
            {
                const uint8_t priority = vm_base[info + 0x18u + spu_num];
                ls[0x1A0u + i] = priority ? (uint8_t)(0x10u - priority) : 0u;
            }
            ls[0x220u + i] = unique_id;
            if (vm_base[spurs + 0x80u + i] == 2u)
                runnable |= (uint16_t)(0x8000u >> i);

            if (i == wid) {
                const uint64_t image_ea = gt6_be64(vm_base + info);
                if (image_ea)
                    current_addr = image_ea;
                current_unique_id = unique_id;
            }
        }

        if (wid < 16u) {
            signals = (uint16_t)
                (((uint16_t)vm_base[spurs + 0x70u] << 8) |
                 vm_base[spurs + 0x71u]);
            if (vm_base[spurs + wid] != 0)
                poll_status |= 1u; /* CELL_SPURS_MODULE_POLL_STATUS_READYCOUNT */
            if (signals & (uint16_t)(0x8000u >> wid))
                poll_status |= 2u; /* CELL_SPURS_MODULE_POLL_STATUS_SIGNAL */
            if (vm_base[spurs + 0x77u] == wid &&
                gt6_be32(vm_base + spurs + 0x68u) == 0)
                poll_status |= 4u; /* CELL_SPURS_MODULE_POLL_STATUS_FLAG */
        }

        /* Keep this opt-in observation beside the context construction: it
         * captures the raw scheduler mirror before any future Kernel1 HLE
         * changes it.  In particular, use the title's state/priority bytes to
         * derive selection rather than assuming that every published WID is
         * runnable forever. */
        if (getenv("GT6_PDI_SCHEDTRACE")) {
            static LONG trace_count = 0;
            if (InterlockedIncrement(&trace_count) <= 12) {
                fprintf(stderr, "[GT6 PDI SCHED] spu=%u current=%u flagValue=%u receiver=%u signal=%04X",
                        spu_num, wid, gt6_be32(vm_base + spurs + 0x6Cu),
                        vm_base[spurs + 0x77u], signals);
                for (uint32_t i = 0; i < 16u; ++i) {
                    const uint32_t info = spurs + 0xB00u + i * 0x20u;
                    fprintf(stderr, " w%u{s=%u,p=%u,r=%u,c=%u,n=%u,m=%u}", i,
                            vm_base[spurs + 0x80u + i],
                            vm_base[info + 0x18u + spu_num],
                            vm_base[spurs + i],
                            vm_base[spurs + 0x20u + i],
                            vm_base[spurs + 0x40u + i],
                            vm_base[spurs + 0x50u + i]);
                }
                fprintf(stderr, "\n");
            }
        }
    }

    /* This host thread exists because the workload was selected for service.
     * If no cause bit is visible yet, use READYCOUNT as a temporary bootstrap
     * cause until the host bridge models a full kernel selection cycle. */
    if (poll_status == 0)
        poll_status = 1u;
    if (wid < 16u) {
        ls[0x180u + wid] = 1u; /* wklLocContention[current] */
        runnable |= (uint16_t)(0x8000u >> wid);
    }

    gt6_ls_be64(ls, 0x1C0u, spurs);
    gt6_ls_be32(ls, 0x1C8u, spu_num);
    gt6_ls_be32(ls, 0x1CCu, GT6_SPURS_KERNEL_DMA_TAG);
    gt6_ls_be64(ls, 0x1D0u, current_addr);
    gt6_ls_be32(ls, 0x1D8u, current_unique_id);
    gt6_ls_be32(ls, 0x1DCu, wid);
    gt6_ls_be32(ls, 0x1E0u, GT6_SPURS_KERNEL1_EXIT_LS);
    gt6_ls_be32(ls, 0x1E4u, GT6_SPURS_SELECT_WORKLOAD_LS);
    gt6_ls_be16(ls, 0x1ECu, runnable);
    gt6_ls_be32(ls, 0x1F0u, 0xF0020000u);
    gt6_ls_be32(ls, GT6_SPURS_POLL_STATUS_LS, poll_status);
    gt6_ls_be32(ls, 0x200u, 0x00020000u);
    gt6_ls_be32(ls, 0x280u, 0x423A3A02u);
    gt6_ls_be32(ls, 0x284u, 0x43F43A82u);
    gt6_ls_be32(ls, 0x288u, 0x43F26502u);
    gt6_ls_be32(ls, 0x28Cu, 0x420EB382u);

    fprintf(stderr,
        "[GT6 SPURS] PDI kernel ABI: spu=%u wid=%u unique=%u "
        "poll=%u context=0x100 entry=0xA00\n",
        spu_num, wid, current_unique_id, poll_status);
    return poll_status;
}

static void gt6_pdi_dump_guest_block(const char* label, uint32_t ea, uint32_t size)
{
    if (!vm_base || !ea || !mfc_ea_range_committed(ea, size)) {
        fprintf(stderr, "[GT6 SPURS] %s ea=%08X unavailable\n", label, ea);
        return;
    }
    fprintf(stderr, "[GT6 SPURS] %s ea=%08X (%u bytes):\n", label, ea, size);
    for (uint32_t i = 0; i < size; i += 16) {
        const uint8_t* p = vm_base + ea + i;
        fprintf(stderr, "  +%03X: %08X %08X %08X %08X\n", i,
                gt6_be32(p), gt6_be32(p + 4), gt6_be32(p + 8), gt6_be32(p + 12));
    }
}

/* Minimal Kernel1 selector used at a raw policy return.  This is the same
 * decision boundary as the SDK kernel: runnable workload, transformed
 * per-SPU priority, contention limit, then ready/signal/flag reason.  It
 * The caller bounds consecutive dispatches to the two physical SPUs, so a
 * selected workload may legitimately be selected again (the normal Kernel1
 * service path) without turning ReadyCount into an unbounded host loop. */
static int gt6_pdi_select_next_workload(const gt6_pdi_policy_job* job,
                                        uint32_t* next_wid,
                                        uint64_t* next_arg)
{
    if (!vm_base || !next_wid || !next_arg ||
        !mfc_ea_range_committed(job->spurs_ea, 0xD00u))
        return 0;

    const uint32_t spurs = job->spurs_ea;
    const uint32_t spu_num = job->workload_id % GT6_PDI_WORKER_COUNT;
    const uint16_t signals = (uint16_t)((vm_base[spurs + 0x70u] << 8) |
                                        vm_base[spurs + 0x71u]);
    const uint32_t flag_value = gt6_be32(vm_base + spurs + 0x6Cu);
    const uint8_t flag_receiver = vm_base[spurs + 0x77u];
    uint32_t selected = 0x10u; /* system-service/no workload */
    uint16_t best_weight = 0;
    uint8_t contention[16];

    InitOnceExecuteOnce(&s_kernel_select_lock_once, gt6_pdi_init_kernel_lock,
                        NULL, NULL);
    EnterCriticalSection(&s_kernel_select_lock);
    for (uint32_t i = 0; i < 16u; ++i)
        contention[i] = (uint8_t)(vm_base[spurs + 0x20u + i] -
                                  s_kernel_local_contention[spu_num][i]);

    for (uint32_t wid = 0; wid < 16u; ++wid) {
        const uint32_t info = spurs + 0xB00u + wid * 0x20u;
        if (vm_base[spurs + 0x80u + wid] != 2u ||
            gt6_be64(vm_base + info) != job->policy_ea)
            continue;

        const uint8_t raw_priority = vm_base[info + 0x18u + spu_num];
        const uint8_t priority = raw_priority ? (uint8_t)(0x10u - raw_priority) : 0u;
        const uint8_t current_contention = contention[wid];
        const uint8_t min_contention = vm_base[spurs + 0x40u + wid];
        const uint8_t max_contention = vm_base[spurs + 0x50u + wid];
        const uint8_t ready_count = vm_base[spurs + wid] > 6u ? 6u : vm_base[spurs + wid];
        const uint8_t idle_count = vm_base[spurs + 0x10u + wid] > 6u
            ? 6u : vm_base[spurs + 0x10u + wid];
        const uint8_t signalled = (signals & (uint16_t)(0x8000u >> wid)) != 0;
        const uint8_t flagged = flag_value == 0u && flag_receiver == wid;

        if (getenv("GT6_PDI_KERNELTRACE")) {
            static LONG trace_count = 0;
            if (InterlockedIncrement(&trace_count) <= 64) {
                fprintf(stderr,
                    "[GT6 Kernel1] spu=%u from=%u candidate=%u state=%u "
                    "prio=%u cont=%u min=%u max=%u ready=%u idle=%u sig=%u flag=%u\n",
                    spu_num, job->workload_id, wid,
                    vm_base[spurs + 0x80u + wid], priority, current_contention,
                    min_contention, max_contention, ready_count, idle_count,
                    signalled, flagged);
            }
        }

        if (!priority || max_contention <= current_contention ||
            !(flagged || signalled || (ready_count != 0u &&
              ready_count + idle_count > current_contention)))
            continue;

        uint16_t weight = (flagged || signalled || ready_count > current_contention) ? 0x8000u : 0u;
        weight |= (uint16_t)(priority & 0x7fu) << 8;
        weight |= wid == job->workload_id ? 0x80u : 0u;
        weight |= current_contention && min_contention > current_contention ? 0x40u : 0u;
        weight |= (uint16_t)((6u - current_contention) & 0x0fu) << 2;
        weight |= 1u;
        if (weight > best_weight) {
            best_weight = weight;
            selected = wid;
        }
    }

    if (selected >= 16u) {
        LeaveCriticalSection(&s_kernel_select_lock);
        return 0;
    }

    /* Kernel1 consumes scheduler causes as part of a non-poll selection.
     * In particular, selecting the workload-flag receiver changes flag from
     * zero to -1.  Leaving it at zero made WID 5 win every synthetic handoff
     * despite having no ReadyCount, repeatedly re-entering its raw policy and
     * eventually corrupting the PPU-side callback graph. */
    {
        uint16_t updated_signals = (uint16_t)(signals &
                                               ~(uint16_t)(0x8000u >> selected));
        vm_base[spurs + 0x70u] = (uint8_t)(updated_signals >> 8);
        vm_base[spurs + 0x71u] = (uint8_t)updated_signals;
        if (flag_receiver == selected)
            gt6_write_be32(vm_base + spurs + 0x6Cu, 0xffffffffu);
    }
    contention[selected]++;
    for (uint32_t i = 0; i < 16u; ++i) {
        vm_base[spurs + 0x20u + i] = contention[i];
        s_kernel_local_contention[spu_num][i] = 0;
    }
    s_kernel_local_contention[spu_num][selected] = 1;
    const uint32_t info = spurs + 0xB00u + selected * 0x20u;
    *next_wid = selected;
    *next_arg = gt6_be64(vm_base + info + 0x08u);
    LeaveCriticalSection(&s_kernel_select_lock);
    return 1;
}

static DWORD WINAPI gt6_pdi_policy_ready_thread(void* opaque)
{
    const uint32_t queue = (uint32_t)(uintptr_t)opaque;
    /* Attaching a runnable workload raises its port once.  The previous
     * periodic host timer fabricated a new SPURS event every 16 ms, making
     * the receiver spin through thousands of empty scheduler passes instead
     * of waiting for the raw policy's actual ReadyCount/signal transitions. */
    if (queue != 0 &&
        (uint32_t)InterlockedCompareExchange(&s_scheduler_tick_queue, 0, 0) == queue &&
        sys_event_queue_push_by_id(queue, 0, 0, 1, 0) == 0) {
        fprintf(stderr, "[GT6 SPURS] lifted policy runnable edge queue=%u\n", queue);
    }
    InterlockedExchange(&s_scheduler_tick_started[queue], 0);
    return 0;
}

/* The PPU's bootstrap queue continues to need policy-service interrupts after
 * the raw image has been associated with PDI's separate workload queue. */
void gt6_pdi_policy_add_scheduler_tick(uint32_t queue)
{
    if (queue == 0 || queue >= 64)
        return;
    /* Replace, rather than duplicate, the old port when CellSpurs attaches
     * the same PDI instance to its live queue.  Leaving both producers alive
     * turned the retired queue's PPU worker into a 60 Hz synthetic-event loop
     * and starved the continuation that creates/runs the boot Fiber. */
    InterlockedExchange(&s_scheduler_tick_queue, (LONG)queue);
    if (InterlockedCompareExchange(&s_scheduler_tick_started[queue], 1, 0) != 0)
        return;
    HANDLE ready = CreateThread(NULL, 0, gt6_pdi_policy_ready_thread,
                                (void*)(uintptr_t)queue, 0, NULL);
    if (ready) {
        CloseHandle(ready);
    } else {
        InterlockedExchange(&s_scheduler_tick_started[queue], 0);
    }
}

static DWORD WINAPI gt6_pdi_policy_thread(void* opaque)
{
    gt6_pdi_policy_job* job = (gt6_pdi_policy_job*)opaque;
    const uint32_t workload_id = job->workload_id;
    const uint32_t workload_context = (uint32_t)job->workload_data;
    if (getenv("GT6_PDI_CTXTRACE") && vm_base) {
        gt6_pdi_dump_guest_block("PDI worker context", workload_context, 0x100);
        if (mfc_ea_range_committed(workload_context, 0x40)) {
            const uint8_t* ctx = vm_base + workload_context;
            /* These are the three non-null pointers in the PPU-created worker
             * context.  They identify the scheduling/control records which a
             * real SPURS kernel preloads into the policy local store. */
            gt6_pdi_dump_guest_block("PDI worker record[0]", gt6_be32(ctx + 0x30), 0x40);
            gt6_pdi_dump_guest_block("PDI worker record[1]", gt6_be32(ctx + 0x34), 0x40);
            gt6_pdi_dump_guest_block("PDI worker record[2]", gt6_be32(ctx + 0x38), 0x40);
        }
    }
    uint8_t* local_store = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (local_store) {
        const int kernel_loaded = gt6_pdi_load_spurs_kernel(local_store);
        memcpy(local_store + GT6_PDI_POLICY_LS_BASE, job->image,
               GT6_PDI_POLICY_SIZE);
        uint32_t poll_status = gt6_pdi_build_kernel_context(local_store, job);
        if (kernel_loaded) {
            spu_begin_image(GT6_SPURS_KERNEL_IMAGE_ID);
            gt6_spurs_kernel_spu_recomp_register();
        }
        spu_begin_image(GT6_PDI_POLICY_IMAGE_ID);
        gt6_pdi_policy_spu_recomp_register();
        fprintf(stderr,
            "[GT6 SPURS] starting lifted PDI policy: %u bytes, spurs=%08X\n",
            GT6_PDI_POLICY_SIZE, job->spurs_ea);
        if (getenv("GT6_PDI_REAL_KERNEL1") && !kernel_loaded)
            fprintf(stderr, "[GT6 SPURS] real Kernel1 requested but libsre.prx was not found\n");
        /* Kernel1 selects again after each policy yield.  A physical GT6 PDI
         * scheduler owns two SPUs, so keep each host turn to two dispatches;
         * subsequent ReadyCount edges create the next bounded turn.  A single
         * dispatch is available only as a lifecycle diagnostic: it isolates
         * host-side re-selection from the raw policy's own first turn. */
        uint32_t dispatch_limit = GT6_PDI_WORKER_COUNT;
        { const char* limit_text = getenv("GT6_PDI_MAX_DISPATCH");
          if (limit_text) {
              const long parsed = strtol(limit_text, NULL, 10);
              if (parsed >= 1 && parsed <= (long)GT6_PDI_WORKER_COUNT)
                  dispatch_limit = (uint32_t)parsed;
          } }
        for (uint32_t dispatch = 0; dispatch < dispatch_limit; ++dispatch) {
            spu_run_lifted_pdi_policy(gt6_pdi_policy_spu_func_00000A00,
                                      local_store, job->workload_data,
                                      poll_status);
            fprintf(stderr, "[GT6 SPURS] lifted PDI policy returned\n");
            /* Optional service cadence for the host bridge. On hardware a
             * policy yield gives the PPU and the other SPU lane time to run;
             * a direct C return otherwise lets this loop monopolize a core. */
            { const char* pace_text = getenv("GT6_PDI_PACE_MS");
              if (pace_text) {
                  const long pace_ms = strtol(pace_text, NULL, 10);
                  if (pace_ms > 0 && pace_ms <= 16)
                      Sleep((DWORD)pace_ms);
              } }
            uint32_t next_wid = 0;
            uint64_t next_arg = 0;
            if (!gt6_pdi_select_next_workload(job, &next_wid, &next_arg))
                break;
            fprintf(stderr,
                "[GT6 SPURS] Kernel1 handoff wid=%u -> wid=%u\n",
                job->workload_id, next_wid);
            job->workload_id = next_wid;
            job->workload_data = next_arg;
            poll_status = gt6_pdi_build_kernel_context(local_store, job);
        }
        free(local_store);
    }
    free(job);
    /* A policy return is a yield to the (not-yet-lifted) SPURS kernel, not a
     * permanent worker shutdown.  Release this worker's launch bit so a later
     * workload publication can run the next scheduler cycle against the
     * updated guest mirror. */
    InterlockedAnd(&s_policy_started_mask, (LONG)~(1u << workload_id));
    InterlockedDecrement(&s_policy_active_count);
    return 0;
}

/* Preserve a ReadyCount edge when both physical lanes are busy. The guest
 * ready byte stays asserted; this thread merely retries the normal bounded
 * dispatch after a real lane returns, instead of opening a third lane. */
static DWORD WINAPI gt6_pdi_policy_deferred_thread(void* opaque)
{
    gt6_pdi_policy_job* job = (gt6_pdi_policy_job*)opaque;
    while (InterlockedCompareExchange(&s_policy_active_count, 0, 0) >=
           job->active_limit)
        Sleep(1);
    gt6_pdi_policy_start(job->policy_ea, GT6_PDI_POLICY_SIZE,
                         job->spurs_ea, job->ready_queue,
                         job->workload_data, job->workload_id);
    free(job);
    return 0;
}

void gt6_pdi_policy_start(uint32_t policy_ea, uint32_t policy_size,
                          uint32_t spurs_ea, uint32_t ready_queue,
                          uint64_t workload_data, uint32_t workload_id)
{
    if (!vm_base || policy_size != GT6_PDI_POLICY_SIZE ||
        !mfc_ea_range_committed(policy_ea, policy_size)) {
        fprintf(stderr,
            "[GT6 SPURS] policy launch skipped: ea=%08X size=%u\n",
            policy_ea, policy_size);
        return;
    }
    /* The initial two policy contexts bootstrap the scheduler, but GT6 adds
     * service workloads (5..7) later using the same raw policy image.  Each
     * needs one bounded policy cycle with its own data/context; limiting this
     * bridge to 0/1 left those entries permanently undispatched. */
    if (workload_id >= 32u)
        return;
    const LONG workload_mask = (LONG)(1u << workload_id);
    if (InterlockedOr(&s_policy_started_mask, workload_mask) & workload_mask)
        return;

    /* The lifted policy shares host-side scheduler mirrors which are not yet
     * equivalent to hardware MFC arbitration.  Serial mode is an opt-in
     * determinism probe; the normal path retains the two physical lanes. */
    const LONG active_limit = getenv("GT6_PDI_SERIAL") ? 1L
                                                     : (LONG)GT6_PDI_WORKER_COUNT;
    if (InterlockedIncrement(&s_policy_active_count) > active_limit) {
        InterlockedDecrement(&s_policy_active_count);
        InterlockedAnd(&s_policy_started_mask, ~workload_mask);
        /* The real Kernel1 route intentionally retains the two physical SPU
         * lanes indefinitely.  A deferred host thread can therefore never
         * acquire a lane: it just sleeps forever for every later ReadyCount
         * edge, while the live kernel already sees that edge in the guest
         * scheduler mirror.  Do not turn those retained guest causes into an
         * unbounded host-thread/memory leak.  The bounded policy bridge below
         * still uses the deferred retry because its lanes really return. */
        if (getenv("GT6_PDI_REAL_KERNEL1")) {
            if (getenv("GT6_PDI_QUEUE_TRACE"))
                fprintf(stderr,
                    "[GT6 SPURS] PDI ready retained by persistent Kernel1 wid=%u active=%ld\n",
                    workload_id, s_policy_active_count);
            return;
        }
        gt6_pdi_policy_job* deferred =
            (gt6_pdi_policy_job*)malloc(sizeof(*deferred));
        if (deferred) {
            deferred->policy_ea = policy_ea;
            deferred->spurs_ea = spurs_ea;
            deferred->workload_data = workload_data;
            deferred->workload_id = workload_id;
            deferred->ready_queue = ready_queue;
            deferred->active_limit = active_limit;
            memcpy(deferred->image, vm_base + policy_ea, GT6_PDI_POLICY_SIZE);
            HANDLE deferred_thread = CreateThread(NULL, 0,
                gt6_pdi_policy_deferred_thread, deferred, 0, NULL);
            if (deferred_thread) {
                CloseHandle(deferred_thread);
                if (getenv("GT6_PDI_QUEUE_TRACE"))
                    fprintf(stderr,
                        "[GT6 SPURS] PDI launch queued wid=%u active=%ld limit=%ld\n",
                        workload_id, s_policy_active_count, active_limit);
                return;
            }
            free(deferred);
        }
        if (getenv("GT6_PDI_QUEUE_TRACE"))
            fprintf(stderr,
                "[GT6 SPURS] PDI launch deferred-but-dropped wid=%u active=%ld limit=%ld\n",
                workload_id, s_policy_active_count, active_limit);
        return;
    }

    gt6_pdi_policy_job* job = (gt6_pdi_policy_job*)malloc(sizeof(*job));
    if (!job) {
        InterlockedAnd(&s_policy_started_mask, ~workload_mask);
        InterlockedDecrement(&s_policy_active_count);
        return;
    }
    job->policy_ea = policy_ea;
    job->spurs_ea = spurs_ea;
    job->workload_data = workload_data;
    job->workload_id = workload_id;
    job->ready_queue = ready_queue;
    job->active_limit = active_limit;
    memcpy(job->image, vm_base + policy_ea, GT6_PDI_POLICY_SIZE);
    /* The lifted policy keeps its guest scheduler loop as a chain of host C
     * calls on MSVC (which has no `musttail` equivalent).  Reserve a dedicated
     * stack for that long-running scheduler instead of letting the default
     * 1 MiB Windows thread stack fault during boot. */
    HANDLE thread = CreateThread(NULL, 32u * 1024u * 1024u,
                                 gt6_pdi_policy_thread, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        free(job);
        InterlockedAnd(&s_policy_started_mask, ~workload_mask);
        InterlockedDecrement(&s_policy_active_count);
    }
}
