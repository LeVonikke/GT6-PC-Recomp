/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_dma.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Clean SPU job abort (longjmp). The `br .` halt idiom and other terminal
 * spins can't be escaped by setting a status flag (lifted code never checks
 * it), so spu_halt() longjmps back to spu_run_with_halt() in the dispatcher.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_TLS __declspec(thread)
#else
#  define SPU_TLS __thread
#endif
static SPU_TLS jmp_buf s_spu_halt_env;
static SPU_TLS int     s_spu_halt_armed = 0;
#define GT6_PDI_SYSTEM_IDLE_STOP 0x7ffeu

/* The system-service HLE is placed before the reservation implementation in
 * this translation unit, but it must make the same lock-line update atomic as
 * the lifted Kernel1 selector. */
static void resv_lock(void);
static void resv_unlock(void);

static uint32_t spu_debug_be32(const spu_context* ctx, uint32_t address)
{
    const uint32_t a = address & SPU_LS_MASK;
    return ((uint32_t)ctx->ls[a] << 24) |
           ((uint32_t)ctx->ls[(a + 1u) & SPU_LS_MASK] << 16) |
           ((uint32_t)ctx->ls[(a + 2u) & SPU_LS_MASK] << 8) |
           (uint32_t)ctx->ls[(a + 3u) & SPU_LS_MASK];
}

static uint32_t spu_guest_be32(const uint8_t* address)
{
    return ((uint32_t)address[0] << 24) |
           ((uint32_t)address[1] << 16) |
           ((uint32_t)address[2] << 8) |
           (uint32_t)address[3];
}

static void spu_guest_write_be32(uint8_t* address, uint32_t value)
{
    address[0] = (uint8_t)(value >> 24);
    address[1] = (uint8_t)(value >> 16);
    address[2] = (uint8_t)(value >> 8);
    address[3] = (uint8_t)value;
}

static void spu_write_be16(spu_context* ctx, uint32_t address, uint16_t value)
{
    const uint32_t a = address & SPU_LS_MASK;
    ctx->ls[a] = (uint8_t)(value >> 8);
    ctx->ls[(a + 1u) & SPU_LS_MASK] = (uint8_t)value;
}

/* This is CellSpursModulePollStatus as used by the system workload.  It is a
 * poll, rather than a kernel dispatch: it records a pending context switch and
 * tells the system module to leave only when another workload is eligible.
 * The subsequent real Kernel1 entry at 0x808 performs the non-poll selection
 * and dispatch.  Keeping these two decisions separate is essential; returning
 * to 0x808 after every service message made both lanes select WID 32 again and
 * starved the later PDI workloads. */
static int spu_system_service_poll(spu_context* ctx, uint32_t spurs,
                                   uint32_t spu_num)
{
    const uint32_t current = spu_debug_be32(ctx, 0x1DCu);
    const uint16_t runnable = (uint16_t)(((uint16_t)ctx->ls[0x1ECu] << 8) |
                                         ctx->ls[0x1EDu]);
    uint8_t contention[16];
    uint8_t pending[16];
    uint32_t selected = 32u;

    resv_lock();
    {
        const uint16_t signals = (uint16_t)(((uint16_t)vm_base[spurs + 0x70u] << 8) |
                                             vm_base[spurs + 0x71u]);
        const uint32_t flag = spu_guest_be32(vm_base + spurs + 0x6Cu);
        const uint8_t flag_receiver = vm_base[spurs + 0x77u];
        uint16_t best_weight = 0;

        for (uint32_t wid = 0; wid < 16u; ++wid) {
            contention[wid] = (uint8_t)(vm_base[spurs + 0x20u + wid] -
                                        ctx->ls[0x180u + wid]);
            pending[wid] = (uint8_t)(vm_base[spurs + 0x30u + wid] -
                                    ctx->ls[0x190u + wid]);
            if (wid != current)
                contention[wid] = (uint8_t)(contention[wid] + pending[wid]);
        }

        /* The system service has priority.  As it is the current workload,
         * consuming its own message does not request a context switch. */
        if (vm_base[spurs + 0x72u] & (uint8_t)(1u << (spu_num & 7u))) {
            vm_base[spurs + 0x72u] &= (uint8_t)~(1u << (spu_num & 7u));
            ctx->ls[0x1EBu] = 0u;
        } else {
            for (uint32_t wid = 0; wid < 16u; ++wid) {
                const uint8_t priority = ctx->ls[0x1A0u + wid];
                const uint8_t max_contention = vm_base[spurs + 0x50u + wid];
                const uint8_t min_contention = vm_base[spurs + 0x40u + wid];
                const uint8_t ready = vm_base[spurs + wid] > 8u ? 8u : vm_base[spurs + wid];
                const uint8_t idle = vm_base[spurs + 0x10u + wid] > 8u ?
                    8u : vm_base[spurs + 0x10u + wid];
                const uint8_t signalled = (signals & (uint16_t)(0x8000u >> wid)) != 0;
                const uint8_t flagged = flag == 0u && flag_receiver == wid;
                const uint8_t cont = contention[wid];

                if (!(runnable & (uint16_t)(0x8000u >> wid)) || !priority ||
                    max_contention <= cont ||
                    !(flagged || signalled || (ready != 0u &&
                      (uint8_t)(ready + idle) > cont)))
                    continue;

                uint16_t weight = (flagged || signalled || ready > cont) ? 0x8000u : 0u;
                weight |= (uint16_t)(priority & 0x7fu) << 8;
                weight |= wid == current ? 0x80u : 0u;
                weight |= cont && min_contention > cont ? 0x40u : 0u;
                weight |= (uint16_t)((8u - (cont > 8u ? 8u : cont)) & 0x0fu) << 2;
                weight |= ctx->ls[0x220u + wid] == ctx->ls[0x1DBu] ? 0x02u : 0u;
                weight |= 1u;
                if (weight > best_weight) {
                    selected = wid;
                    best_weight = weight;
                }
            }
            ctx->ls[0x1EBu] = selected == 32u ? 1u : 0u;
        }

        if (selected != current) {
            if (selected < 16u)
                pending[selected]++;
            for (uint32_t wid = 0; wid < 16u; ++wid) {
                vm_base[spurs + 0x30u + wid] = pending[wid];
                ctx->ls[0x190u + wid] = 0u;
            }
            if (selected < 16u)
                ctx->ls[0x190u + selected] = 1u;
        } else {
            for (uint32_t wid = 0; wid < 16u; ++wid) {
                vm_base[spurs + 0x30u + wid] = (uint8_t)(vm_base[spurs + 0x30u + wid] -
                                                         ctx->ls[0x190u + wid]);
                ctx->ls[0x190u + wid] = 0u;
            }
        }
        memcpy(ctx->ls + 0x100u, vm_base + spurs, 0x80u);

        if (getenv("GT6_PDI_SYSTEM_SERVICE_TRACE")) {
            static SPU_TLS unsigned trace_count;
            if (trace_count++ < 32u)
                fprintf(stderr,
                        "[GT6 Kernel1] service-poll spu=%u current=%u selected=%u r5=%u r6=%u r7=%u c5=%u c6=%u c7=%u\n",
                        spu_num, current, selected,
                        vm_base[spurs + 5u], vm_base[spurs + 6u], vm_base[spurs + 7u],
                        contention[5], contention[6], contention[7]);
        }
    }
    resv_unlock();
    return selected != current;
}

/* SPU->PPU outbound-mailbox delivery hook. The SPU writing WrOutMbox /
 * WrOutIntrMbox must wake PPU code blocked on the SPURS event queue bound to
 * the SPU thread group (e.g. cellSpursInitialize). lv2_register.c installs a
 * handler that maps spu_group_id -> connected event queue and pushes an event.
 * NULL until installed (plain SPU jobs with no PPU listener stay a no-op). */
void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                            int is_intr, uint32_t value) = 0;

void spu_halt(spu_context* ctx)
{
    (void)ctx;
    if (s_spu_halt_armed) { s_spu_halt_armed = 0; longjmp(s_spu_halt_env, 1); }
}

/* Diagnostic: dump the taskset-policy scheduler's working tables (LS 0x2700..)
 * at func_00000E60 entry, to reverse why it computes "no runnable task". Env
 * YDKJ_E60. Called from the lifted taskset policy. */
void spu_dbg_e60(spu_context* ctx)
{
    static int s_e = -1; if (s_e < 0) s_e = getenv("YDKJ_E60") ? 1 : 0;
    if (!s_e) return;
    static int _d = 0; if (_d++ >= 6) return;
    const uint8_t* L = ctx->ls;
    #define RD(o) (((uint32_t)L[(o)]<<24)|((uint32_t)L[(o)+1]<<16)|((uint32_t)L[(o)+2]<<8)|L[(o)+3])
    fprintf(stderr, "[E60] r20=%08X r24=%08X | run2700=%08X ready2710=%08X pend2720=%08X en2730=%08X sig2740=%08X wait2750=%08X x2770=%08X\n",
            ctx->gpr[20]._u32[0], ctx->gpr[24]._u32[0],
            RD(0x2700), RD(0x2710), RD(0x2720), RD(0x2730), RD(0x2740), RD(0x2750), RD(0x2770));
    #undef RD
    fflush(stderr);
}

/* SPU `stop` / stop-and-signal. A real SPU stop HALTS the core; the previous
 * lifted emission only did `status=...; return;`, which unwound ONE frame and
 * let the caller's service loop keep running -- so a SPURS policy doing
 * stop-and-signal in a loop spun millions of times instead of yielding. Halt the
 * host SPU thread (longjmp to spu_run_with_halt) so the job stops AT the stop,
 * with ctx->status + the outbound mailbox value available for the dispatcher to
 * service. Env YDKJ_STOP_NOHALT restores the old (looping) behavior for A/B. */
void spu_stop(spu_context* ctx)
{
    /* A lifted `stop` sets status and RETURNS to its caller -- for a SPURS
     * policy this is stop-and-signal: it returns up into the kernel/policy
     * service loop, which continues (i.e. the SPU "resumes" past the stop). That
     * resume-by-return behavior is correct; the previous spin was caused by the
     * lack of (a) a PPU listener for the outbound mailbox and (b) mailbox
     * backpressure -- both handled in the channel write path, not here. So by
     * default DO NOT halt. Env YDKJ_STOP_HALT forces a hard halt (longjmp) for
     * A/B experiments (it makes the kernel terminate, which the game restarts). */
    static int s_halt = -1;
    if (s_halt < 0) s_halt = getenv("YDKJ_STOP_HALT") ? 1 : 0;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    if (s_halt) spu_halt(ctx);
}

/* Run a lifted SPU entry with a halt landing pad. Returns 1 if the job halted
 * (via spu_halt), 0 if it returned normally. */
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx)
{
    int halted = 0;
    s_spu_halt_armed = 1;
    if (setjmp(s_spu_halt_env) != 0) {
        halted = 1;   /* came back via longjmp */
    } else {
        ctx->trampoline_fn = entry;
        spu_drain_trampoline(ctx);
    }
    s_spu_halt_armed = 0;
    /* A longjmp bypasses the generated brsl/bisl pop operations.  The run has
     * ended, so discard any host-call markers before this context is reused. */
    ctx->host_call_depth = 0;
    return halted;
}

/* Resume an already prepared SPU context after a host-side idle wait.  Unlike
 * spu_run_with_halt this deliberately preserves trampoline_fn, registers and
 * the MFC slot associated with ctx; this is required for a Kernel1 system
 * workload to wait without rebuilding its physical SPU lane. */
int spu_resume_with_halt(spu_context* ctx)
{
    int halted = 0;
    s_spu_halt_armed = 1;
    if (setjmp(s_spu_halt_env) != 0) {
        halted = 1;
    } else {
        spu_drain_trampoline(ctx);
    }
    s_spu_halt_armed = 0;
    ctx->host_call_depth = 0;
    return halted;
}

/* WID 32 is the SPURS system workload.  Its idle path owns the CellSpurs
 * lock-line: it marks this SPU idle and sleeps until the PPU changes the
 * protected 0x80-byte scheduler header.  This deliberately waits for an
 * observed guest transition instead of repeatedly invoking Kernel1 on a
 * timer.  The remaining service requests are still handled by the title's
 * normal PPU bridge; this is only the firmware-equivalent idle boundary. */
int spu_system_service_lockwait(spu_context* ctx)
{
    if (!ctx || !vm_base)
        return 0;

    const uint32_t spurs = spu_ls_read32(ctx, 0x1C4u);
    const uint32_t spu_num = spu_ls_read32(ctx, 0x1C8u);
    if ((spu_num >= 8u) || !spurs || !mfc_ea_range_committed(spurs, 0x80u))
        return 0;

    const uint8_t bit = (uint8_t)(1u << spu_num);
    uint8_t lock_line[0x80];

    /* SpursKernelContext carries its own service lifecycle.  Initialise it
     * once per physical SPU, exactly as the firmware service entry does.
     * Keeping only the guest sysSrvOnSpu bit made the next Kernel1 poll see a
     * stale local state. */
    if (ctx->ls[0x1EAu] == 0u) {
        ctx->ls[0x1EAu] = 1u; /* sysSrvInitialised */
        if (mfc_ea_range_committed(spurs + 0xC8u, 1u))
            vm_base[spurs + 0xC8u] |= bit;
    }

    for (;;) {
    /* Process the scheduler request that caused Kernel1 to select WID 32.
     * AddWorkload publishes sysSrvMsgUpdateWorkload and sysSrvMessage for
     * each live SPU.  The system workload must rebuild the *local* Kernel1
     * mirror before it polls again: status ownership in guest RAM alone leaves
     * wklRunnable1, priority and unique-id stale in the PDI SPU context. */
    const int has_service_message = (vm_base[spurs + 0x72u] & bit) != 0;
    const int has_workload_update = mfc_ea_range_committed(spurs + 0xBEu, 1u) &&
        (vm_base[spurs + 0xBDu] & bit) != 0;
    if (has_service_message || has_workload_update) {
        vm_base[spurs + 0x72u] &= (uint8_t)~bit;
        if (has_workload_update) {
            vm_base[spurs + 0xBDu] &= (uint8_t)~bit;
            uint16_t runnable = 0;

            /* Firmware first DMA-copies WorkloadInfo1 to LS 0x30000.  The
             * guest mirror uses the same 16 x 0x20-byte packed records. */
            memcpy(ctx->ls + 0x30000u, vm_base + spurs + 0xB00u, 0x200u);
            for (uint32_t wid = 0; wid < 16u; ++wid) {
                const uint32_t info = spurs + 0xB00u + wid * 0x20u;
                const uint8_t priority = vm_base[info + 0x18u + spu_num];

                /* SpursKernelContext: priority[16], wklUniqueId[16], then
                 * wklRunnable1.  These are the offsets already used by the
                 * raw PDI context builder and selector. */
                ctx->ls[0x1A0u + wid] = priority ? (uint8_t)(0x10u - priority) : 0u;
                ctx->ls[0x220u + wid] = vm_base[info + 0x14u];
                if (vm_base[spurs + 0x80u + wid] == 2u)
                    vm_base[spurs + 0x90u + wid] |= bit;
                else
                    vm_base[spurs + 0x90u + wid] &= (uint8_t)~bit;
                if (vm_base[spurs + 0x80u + wid] == 2u)
                    runnable |= (uint16_t)(0x8000u >> wid);
            }
            spu_write_be16(ctx, 0x1ECu, runnable);

            /* The service retains the post-update state copy used by its
             * next selection pass.  It is not an event or a fabricated
             * completion; it is the same scheduler snapshot the firmware
             * materializes after processing sysSrvMsgUpdateWorkload. */
            memcpy(ctx->ls + 0x2D80u, vm_base + spurs + 0x80u, 0x80u);
        }
        vm_base[spurs + 0x73u] &= (uint8_t)~bit;
        ctx->ls[0x1EBu] = 0u; /* SpursKernelContext::spuIdling */
        memcpy(ctx->ls + 0x100u, vm_base + spurs, 0x80u);
        if (getenv("GT6_PDI_SYSTEM_STATE_TRACE") && has_workload_update) {
            static SPU_TLS unsigned state_trace_count;
            if (state_trace_count++ < 16u) {
                fprintf(stderr,
                        "[GT6 Kernel1] service-state spu=%u current=%u runnable=%04X",
                        spu_num, spu_debug_be32(ctx, 0x1DCu),
                        (unsigned)((uint16_t)ctx->ls[0x1ECu] << 8 | ctx->ls[0x1EDu]));
                for (uint32_t wid = 0; wid < 8u; ++wid) {
                    const uint32_t info = spurs + 0xB00u + wid * 0x20u;
                    fprintf(stderr, " w%u{s=%u,p=%u,r=%u,c=%u,q=%u,t=%02X}",
                            wid, vm_base[spurs + 0x80u + wid],
                            vm_base[info + 0x18u + spu_num], vm_base[spurs + wid],
                            vm_base[spurs + 0x20u + wid], vm_base[spurs + 0x30u + wid],
                            vm_base[spurs + 0x90u + wid]);
                }
                fputc('\n', stderr);
            }
        }
        if (getenv("GT6_PDI_REAL_KERNEL_TRACE"))
            fprintf(stderr,
                    "[GT6 Kernel1] system-service spu=%u processed update=%d message=%d\n",
                    spu_num, has_workload_update, has_service_message);
    }

    /* The service must not leave merely because it processed a message.  Its
     * real loop polls the scheduler first and exits to Kernel1 only if that
     * poll selected another workload. */
    if (spu_system_service_poll(ctx, spurs, spu_num))
        return 1;

    /* No transition is currently eligible.  Model the reservation-lost wait
     * and return to the poll loop after a real PPU lock-line modification. */
    vm_base[spurs + 0x73u] |= bit;
    ctx->ls[0x1EBu] = 1u; /* SpursKernelContext::spuIdling */
    memcpy(lock_line, vm_base + spurs, sizeof(lock_line));
    memcpy(ctx->ls + 0x100u, lock_line, sizeof(lock_line));

    for (;;) {
#ifdef _WIN32
        Sleep(1);
#endif
        if (memcmp(lock_line, vm_base + spurs, sizeof(lock_line)) != 0) {
            memcpy(ctx->ls + 0x100u, vm_base + spurs, sizeof(lock_line));
            vm_base[spurs + 0x73u] &= (uint8_t)~bit;
            ctx->ls[0x1EBu] = 0u;
            break;
        }
    }
    }
}

/* ===========================================================================
 * Per-context MFC engine registry
 * ===========================================================================*/
#define SPU_MAX_CONTEXTS 8

typedef struct {
    spu_context* ctx;
    mfc_engine   mfc;
} spu_mfc_slot;

static spu_mfc_slot s_mfc_slots[SPU_MAX_CONTEXTS];

static mfc_engine* mfc_for(spu_context* ctx)
{
    spu_mfc_slot* free_slot = NULL;
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++) {
        if (s_mfc_slots[i].ctx == ctx)
            return &s_mfc_slots[i].mfc;
        if (!free_slot && s_mfc_slots[i].ctx == NULL)
            free_slot = &s_mfc_slots[i];
    }
    if (free_slot) {
        free_slot->ctx = ctx;
        mfc_engine_init(&free_slot->mfc);
        return &free_slot->mfc;
    }
    /* Out of slots: fall back to a shared engine (correct for single-SPU). */
    static mfc_engine fallback;
    static int fallback_init = 0;
    if (!fallback_init) { mfc_engine_init(&fallback); fallback_init = 1; }
    return &fallback;
}

/* ===========================================================================
 * Atomic reservation (GETLLAR / PUTLLC / PUTLLUC) -- real lock-line semantics
 *
 * Multiple SPU kernel threads (the SPURS workload runtime runs several SPUs on
 * one shared lock-free queue) issue GETLLAR/PUTLLC on the SAME 128-byte lines.
 * Without honoring the reservation, two SPUs both "claim" the same slot and the
 * queue corrupts (observed: the 2nd claim returns garbage [1,1,1,1] -> the SPU
 * traps). PUTLLC must FAIL when the line changed since GETLLAR. We implement the
 * compare-and-swap under one global lock across all SPU host threads.
 * ===========================================================================*/
extern uint8_t* vm_base;

/* Global spinlock guarding all atomic line ops. _InterlockedExchange is a
 * clang-cl/MSVC intrinsic (no runtime library symbol needed); GCC/Clang on
 * non-Windows targets use the equivalent C11 atomic exchange instead. */
#if defined(_MSC_VER)
#include <intrin.h>
static volatile long g_resv_lock = 0;
static void resv_lock(void)   { while (_InterlockedExchange(&g_resv_lock, 1)) { } }
static void resv_unlock(void) { _InterlockedExchange(&g_resv_lock, 0); }
#else
#include <stdatomic.h>
static atomic_int g_resv_lock = 0;
static void resv_lock(void)   { while (atomic_exchange(&g_resv_lock, 1)) { } }
static void resv_unlock(void) { atomic_exchange(&g_resv_lock, 0); }
#endif

/* Returns 1 if `cmd` is an atomic line op and was handled here, else 0. */
static int spu_mfc_atomic(spu_context* ctx, uint32_t cmd)
{
    /* Reject ordinary DMA commands before inspecting their EA.  The
     * uncommitted-line guard below belongs only to reservation commands; when
     * it ran first it also swallowed a normal PUT/GET whose target page had
     * not been committed yet, incorrectly reporting it as "spu-atomic". */
    if (cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD &&
        cmd != MFC_PUTLLUC_CMD && cmd != MFC_PUTQLLUC_CMD)
        return 0;

    uint32_t ea  = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK;
    uint8_t* ls  = &ctx->ls[lsa];
    uint8_t* mem = vm_base + ea;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_lastea = 0;
        if ((++s_n % 2000000) == 0 || ea != s_lastea) {
          if ((s_n % 2000000) == 0)
            fprintf(stderr, "[atomcnt] %llu atomic ops; last cmd=0x%X ea=0x%08X\n",
                    (unsigned long long)s_n, cmd, ea);
          s_lastea = ea; } } }
    { static int s_at = -1; if (s_at < 0) s_at = getenv("YDKJ_ATOMTRACE") ? 1 : 0;
      if (s_at) { static int _a=0; if (_a++ < 40)
        fprintf(stderr, "[atom] cmd=0x%02X ea=0x%08X (img=%d)\n", cmd, ea, ctx->image_id); } }
    /* cri task (img22) atomic on the taskset: dump the loaded bitset line so we can
     * see if the task reads MY taskset (0x4005E000) with my READY bit, or elsewhere. */
    { static int s_ct=-1; if(s_ct<0) s_ct=getenv("YDKJ_ATOMTRACE")?1:0;
      if(s_ct && ctx->image_id==22 && cmd==0xD0 && mfc_ea_range_committed(ea,16)) {
        static int _c=0; if(_c++<24){
          uint8_t* m=vm_base+ea;
          #define BW(o) (((uint32_t)m[o]<<24)|((uint32_t)m[o+1]<<16)|((uint32_t)m[o+2]<<8)|m[o+3])
          fprintf(stderr,"[cri-atom] GETLLAR ea=0x%08X line[0..0x30]: %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X\n",
            ea, BW(0),BW(4),BW(8),BW(0xC), BW(0x10),BW(0x14),BW(0x18),BW(0x1C), BW(0x20),BW(0x24),BW(0x28),BW(0x2C));
          #undef BW
        } } }
    /* YDKJ_CRI_R4: dump the CellSpursTaskset bitsets when the policy atomically
     * touches my taskset (0x0F000000), to watch the task-activation state machine
     * (why task0 isn't selected+first-run). running@0 ready@0x10 pending@0x20
     * enabled@0x30 signalled@0x40 waiting@0x50 (each 16B; word0 = MSB, task0=bit127). */
    { static int s_td = -1; if (s_td < 0) s_td = (getenv("YDKJ_CRI_CHAIN") && getenv("YDKJ_ATOMTRACE")) ? 1 : 0;
      if (s_td && ea >= 0x0F000000u && ea < 0x0F001900u) {
        extern uint8_t* vm_base;
        static int _t=0; if (vm_base && _t++ < 24) {
            uint8_t* t = vm_base + 0x0F000000u;
            #define TW(o) (((uint32_t)t[o]<<24)|((uint32_t)t[o+1]<<16)|((uint32_t)t[o+2]<<8)|t[o+3])
            fprintf(stderr, "[tset] %s run=%08X rdy=%08X pnd=%08X ena=%08X sig=%08X wait=%08X | wid=%08X last=%02X\n",
                    cmd==0xD0?"GET":cmd==0xB4?"PUT":"?", TW(0x00), TW(0x10), TW(0x20), TW(0x30), TW(0x40), TW(0x50), TW(0x74), t[0x73]);
            #undef TW
        }
      } }

    /* Guard atomic line ops against an uncommitted/garbage EA (e.g. a SPURS
     * policy computing a lock-line address from an incomplete instance context).
     * Same rationale as the DMA EA guard: a bad guest atomic must not segfault
     * the host. GETLLAR returns a zeroed line (no reservation); PUTLLC fails. */
    if (!mfc_ea_range_committed(ea, MFC_ATOMIC_LINE)) {
        static int s_w = 0;
        if (s_w++ < 16)
            fprintf(stderr, "[spu-atomic] cmd=0x%X ea=0x%08X uncommitted -- skipped\n", cmd, ea);
        if (cmd == MFC_GETLLAR_CMD) {
            memset(ls, 0, MFC_ATOMIC_LINE);
            ctx->resv_ea = ea; ctx->resv_valid = 0; ctx->atomic_stat = 0;
        } else {
            ctx->atomic_stat = 1;   /* PUTLLC failure (line "moved") */
        }
        return 1;
    }

    switch (cmd) {
    case MFC_GETLLAR_CMD:
        resv_lock();
        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */
        memcpy(ctx->resv_line, mem, MFC_ATOMIC_LINE);  /* snapshot for compare */
        ctx->resv_ea = ea; ctx->resv_valid = 1; ctx->atomic_stat = 0;
        resv_unlock();
        /* Kernel1 keeps priority/identity/runnable vectors outside the
         * 128-byte reservation line.  GT6 adds PDI workloads 5..7 after the
         * two physical lanes have begun, so the initial local-store snapshot
         * legitimately predates those records.  Refresh only those derived
         * read-only mirrors before the authentic selector consumes its newly
         * fetched ReadyCounts.  This neither posts an event nor writes guest
         * CellSpurs memory; it is the missing DMA-visible half of the same
         * scheduler context. */
        if (getenv("GT6_PDI_REAL_KERNEL1") && ctx->image_id == 25 &&
            lsa == 0x100u && mfc_ea_range_committed(ea, 0xD00u)) {
            const uint32_t spu_num = spu_debug_be32(ctx, 0x1C8u) & 1u;
            uint16_t runnable = 0;
            for (uint32_t wid = 0; wid < 16u; ++wid) {
                const uint32_t info = ea + 0xB00u + wid * 0x20u;
                const uint8_t priority = vm_base[info + 0x18u + spu_num];
                ctx->ls[0x1A0u + wid] = priority ?
                    (uint8_t)(0x10u - priority) : 0u;
                ctx->ls[0x220u + wid] = vm_base[info + 0x14u];
                if (vm_base[ea + 0x80u + wid] == 2u)
                    runnable |= (uint16_t)(0x8000u >> wid);
            }
            ctx->ls[0x1ECu] = (uint8_t)(runnable >> 8);
            ctx->ls[0x1EDu] = (uint8_t)runnable;
        }
        /* The real GT6 Kernel1 polls this CellSpurs reservation line while
         * the PPU publishes ReadyCount edges.  Log only content changes for
         * its real kernel image: this proves whether those edges reach the
         * SPU before diagnosing the selector itself. */
        if (getenv("GT6_PDI_LOCKLINE_TRACE") && ctx->image_id == 25 &&
            lsa == 0x100u) {
            static SPU_TLS uint64_t s_gt6_last_lockline = UINT64_MAX;
            static SPU_TLS unsigned s_gt6_lockline_changes;
            uint64_t signature = ((uint64_t)ls[0] << 56) |
                                 ((uint64_t)ls[1] << 48) |
                                 ((uint64_t)ls[2] << 40) |
                                 ((uint64_t)ls[3] << 32) |
                                 ((uint64_t)ls[4] << 24) |
                                 ((uint64_t)ls[5] << 16) |
                                 ((uint64_t)ls[6] << 8) |
                                 (uint64_t)ls[7];
            if (signature != s_gt6_last_lockline &&
                s_gt6_lockline_changes++ < 32) {
                s_gt6_last_lockline = signature;
                fprintf(stderr,
                        "[GT6 Kernel1 lockline] ea=%08X ready=%02X%02X%02X%02X%02X%02X%02X%02X state=%02X%02X%02X%02X current=%02X%02X%02X%02X flag=%08X receiver=%02X flags1=%02X spus=%u\n",
                        ea, ls[0], ls[1], ls[2], ls[3], ls[4], ls[5], ls[6], ls[7],
                        ls[0x20], ls[0x21], ls[0x22], ls[0x23],
                        ls[0x40], ls[0x41], ls[0x42], ls[0x43],
                        spu_guest_be32(ls + 0x6Cu), ls[0x77],
                        ls[0x74], ls[0x76]);
            }
        }
        return 1;

    case MFC_PUTLLC_CMD:
        resv_lock();
        if (ctx->resv_valid && ctx->resv_ea == ea &&
            memcmp(mem, ctx->resv_line, MFC_ATOMIC_LINE) == 0) {
            memcpy(mem, ls, MFC_ATOMIC_LINE);          /* commit local store */
            ctx->atomic_stat = 0;                      /* PUTLLC_SUCCESS */
        } else {
            ctx->atomic_stat = 1;                      /* PUTLLC_FAILURE -> retry */
        }
        ctx->resv_valid = 0;                           /* reservation consumed */
        if (getenv("GT6_PDI_LOCKLINE_COMMIT_TRACE") && ctx->image_id == 25 &&
            lsa == 0x100u) {
            static SPU_TLS unsigned s_gt6_lockline_commit_trace;
            if (s_gt6_lockline_commit_trace++ < 64u)
                fprintf(stderr,
                        "[GT6 Kernel1 commit] ea=%08X %s flag=%08X recv=%02X flags1=%02X spus=%u r5=%u r6=%u r7=%u i5=%u i6=%u i7=%u c5=%u c6=%u c7=%u p5=%u p6=%u p7=%u\n",
                        ea, ctx->atomic_stat == 0 ? "ok" : "retry",
                        spu_guest_be32(ls + 0x6Cu), ls[0x77], ls[0x74], ls[0x76],
                        ls[5], ls[6], ls[7],
                        ls[0x15], ls[0x16], ls[0x17],
                        ls[0x25], ls[0x26], ls[0x27],
                        ls[0x35], ls[0x36], ls[0x37]);
        }
        resv_unlock();
        return 1;

    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        resv_lock();
        memcpy(mem, ls, MFC_ATOMIC_LINE);              /* unconditional store */
        ctx->resv_valid = 0; ctx->atomic_stat = 0;
        resv_unlock();
        return 1;

    default:
        return 0;
    }
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

/* ===========================================================================
 * Channel write
 * ===========================================================================*/
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    if (channel_is_mfc(channel)) {
        /* A raw PDI policy should issue GETLLAR (0xD0) / PUTLLC (0xB4)
         * while it claims its SPURS scheduler line.  Seeing command zero here
         * means that the lifted path lost the command register before the MFC
         * operation, which is more fundamental than an event-queue wakeup.
         * This is also the runtime TU that owns the inline job ABI helpers. */
        { static int s_gt6 = -1; if (s_gt6 < 0) s_gt6 = getenv("GT6_PDI_MFCTRACE") ? 1 : 0;
          if (s_gt6 && ctx->image_id == 24 && channel == MFC_Cmd) {
              static int n = 0; if (n++ < 48)
                  fprintf(stderr,
                      "[gt6-pdi-mfc] cmd=%02X lsa=%05X ea=%08X size=%u tag=%u pc=%05X r13=%08X r14=%08X\n",
                      v, ctx->mfc_lsa, ctx->mfc_eal, ctx->mfc_size,
                      ctx->mfc_tag, ctx->pc, ctx->gpr[13]._u32[0],
                      ctx->gpr[14]._u32[0]);
              if (v == MFC_PUT_CMD && n <= 48 && ctx->mfc_size <= 128) {
                  const uint8_t* p = ctx->ls + (ctx->mfc_lsa & SPU_LS_MASK);
                  fprintf(stderr,
                      "[gt6-pdi-put] LS[%05X]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                      ctx->mfc_lsa, p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                      p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
              }
          } }
        /* Atomic line ops (GETLLAR/PUTLLC/...) need real reservation semantics,
         * not the plain GET/PUT the DMA engine would do. */
        if (channel == MFC_Cmd && spu_mfc_atomic(ctx, v))
            return;
        { static int s_gt6_tag_trace = -1;
          if (s_gt6_tag_trace < 0) s_gt6_tag_trace = getenv("GT6_PDI_TAGTRACE") ? 1 : 0;
          if (s_gt6_tag_trace && ctx->image_id == 24 &&
              (channel == MFC_TagID || channel == MFC_Cmd ||
               channel == MFC_WrTagMask || channel == MFC_WrTagUpdate)) {
              static int n = 0;
              if (n++ < 96)
                  fprintf(stderr, "[gt6-pdi-tag] wr ch=%u val=%08X pc=%05X tag=%u mask=%08X completed=%08X\n",
                          channel, v, ctx->pc & SPU_LS_MASK, ctx->mfc_tag,
                          ctx->mfc_tag_mask, mfc_for(ctx)->tag_completed);
          } }
        mfc_channel_write(mfc_for(ctx), ctx, channel, v);
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:
        spu_channel_write(&ctx->ch_out_mbox, v);
        { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] OUT  grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 0, v);
        break;
    case SPU_WrOutIntrMbox:
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] INTR grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 1, v);
        break;
    case SPU_WrDec:          ctx->decrementer = v;                          break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break; /* WrEventMask */
    case SPU_WrEventAck:     ctx->event_status &= ~v;                       break;
    case SPU_WrSRR0:         ctx->srr0 = v;                                 break;
    default:
        /* Unknown / unhandled channel write -- ignore (matches a no-op SPU). */
        break;
    }
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    uint32_t v = 0;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_c[10] = {0}; static uint64_t s_tot = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdSigNotify1)?1:(channel==SPU_RdSigNotify2)?2:
                (channel==SPU_RdDec)?3:(channel==SPU_RdEventStat)?4:(channel==SPU_RdEventMask)?5:
                (channel==MFC_RdTagStat)?6:(channel==MFC_RdAtomicStat)?7:(channel==SPU_RdMachStat)?8:9;
        s_c[b]++;
        if ((++s_tot % 2000000) == 0)
          fprintf(stderr, "[rdch] InMbox=%llu Sig1=%llu Sig2=%llu Dec=%llu EvStat=%llu EvMask=%llu TagStat=%llu AtomStat=%llu MachStat=%llu other=%llu\n",
            (unsigned long long)s_c[0],(unsigned long long)s_c[1],(unsigned long long)s_c[2],(unsigned long long)s_c[3],
            (unsigned long long)s_c[4],(unsigned long long)s_c[5],(unsigned long long)s_c[6],(unsigned long long)s_c[7],
            (unsigned long long)s_c[8],(unsigned long long)s_c[9]); } }

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(mfc_for(ctx), ctx, channel);
        { static int s_gt6_tag_trace = -1;
          if (s_gt6_tag_trace < 0) s_gt6_tag_trace = getenv("GT6_PDI_TAGTRACE") ? 1 : 0;
          if (s_gt6_tag_trace && ctx->image_id == 24 && channel == MFC_RdTagStat) {
              static int n = 0;
              if (n++ < 96)
                  fprintf(stderr, "[gt6-pdi-tag] rd status=%08X pc=%05X tag=%u mask=%08X completed=%08X\n",
                          v, ctx->pc & SPU_LS_MASK, ctx->mfc_tag,
                          ctx->mfc_tag_mask, mfc_for(ctx)->tag_completed);
          } }
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:      v = spu_channel_read(&ctx->ch_in_mbox);     break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    case SPU_RdDec:         v = ctx->decrementer;                       break;
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:   v = ctx->event_status;                      break;
    case SPU_RdMachStat:    v = (ctx->status == SPU_STATUS_RUNNING) ? 1 : 0; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_cnt[8] = {0}; static uint64_t s_total = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdEventStat)?1:(channel==SPU_RdSigNotify1)?2:
                (channel==SPU_RdSigNotify2)?3:(channel==MFC_RdTagStat)?4:(channel==SPU_WrOutMbox)?5:
                (channel==SPU_WrOutIntrMbox)?6:7;
        s_cnt[b]++;
        if ((++s_total % 2000000) == 0)
          fprintf(stderr, "[pollcnt] InMbox=%llu EvStat=%llu Sig1=%llu Sig2=%llu TagStat=%llu OutMbox=%llu OutIntr=%llu other=%llu (ch last=%u)\n",
                  (unsigned long long)s_cnt[0],(unsigned long long)s_cnt[1],(unsigned long long)s_cnt[2],
                  (unsigned long long)s_cnt[3],(unsigned long long)s_cnt[4],(unsigned long long)s_cnt[5],
                  (unsigned long long)s_cnt[6],(unsigned long long)s_cnt[7], channel); } }
    switch (channel) {
    case SPU_RdInMbox:       return ctx->ch_in_mbox.count;                 /* readable */
    case SPU_WrOutMbox:      return SPU_MBOX_DEPTH - ctx->ch_out_mbox.count; /* free slots */
    case SPU_WrOutIntrMbox:  return SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count;
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    default:                 return 1;  /* default: channel ready */
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
} spu_reg_entry;

#define SPU_FN_REGISTRY_MAX 65536
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    /* Mask to LS addr: guest EA (0x0144F618) -> LS (0xF618) matching ctx->pc */
    addr &= SPU_LS_MASK;
    if (s_registry_count < SPU_FN_REGISTRY_MAX) {
        s_registry[s_registry_count].addr = addr;
        s_registry[s_registry_count].fn = fn;
        s_registry[s_registry_count].image_id = s_reg_image;
        s_registry_count++;
    }
}

static spu_fn spu_lookup(uint32_t addr, int image_id)
{
    /* Linear scan is fine for the small per-image tables. Match the context's
     * active image; image_id 0 (context or entry) matches any, for back-compat
     * with single-image contexts. */
    for (uint32_t i = 0; i < s_registry_count; i++)
        if (s_registry[i].addr == addr &&
            (image_id == 0 || s_registry[i].image_id == 0 ||
             s_registry[i].image_id == image_id))
            return s_registry[i].fn;
    return NULL;
}

/* HLE of the taskset Policy Module's task-syscall entry (LS 0xA70). A SPURS task
 * (e.g. the cri_mpv task, image 22) reads syscallAddr from its SpursTasksetContext
 * (LS 0x27C4) and branches to it to perform a task syscall (EXIT/YIELD/WAIT/POLL).
 * The real kernel has the PM code resident at 0xA70; we don't, so we plant 0xA70 as
 * syscallAddr (in the cri dispatch) and INTERCEPT a branch to it here to HLE the
 * syscall. num = r3&0xF (0x10 bit = the "2" variant), args in r4. Adopted from the
 * JonathanDC64/ps3recomp fork (aaea4158) which uses this to run SPURS tasks clean. */
#define YDKJ_TASKSET_PM_SYSCALL_ADDR 0xA70u
#define GT6_SPURS_KERNEL1_EXIT_ADDR 0x808u
#define GT6_SPURS_SELECT_WORKLOAD_ADDR 0x290u
#define GT6_SPURS_CURRENT_WID_LS 0x1DCu
#define GT6_SPURS_POLL_STATUS_LS 0x1F4u
/* Generated locally from the user's firmware, linked by GT6MainRecomp. */
extern void gt6_spurs_kernel_spu_func_00000808(spu_context* ctx);
extern void gt6_spurs_kernel_spu_func_00000814(spu_context* ctx);
extern void gt6_spurs_kernel_spu_func_00000818(spu_context* ctx);
extern void gt6_spurs_kernel_spu_func_00000844(spu_context* ctx);
extern void gt6_spurs_kernel_spu_func_00000290(spu_context* ctx);
/* Generated from the second policy module in the user's local libsre.prx.
 * This is distinct from GT6's raw PDI workload policy (image 24).
 * Non-static: also called directly by the pure interpreter (spu_interp.c,
 * upstream 2026-08-14 fold) when it reaches this LS address itself. */
void spu_spurs_taskset_syscall(spu_context* ctx)
{
    uint32_t raw = ctx->gpr[3]._u32[0];
    uint32_t num = raw & 0x0F;
    { static int _n = 0; if (_n++ < 24)
        fprintf(stderr, "[spu] SPURS taskset syscall num=%u (raw=0x%X args=0x%08X) image=%d link/r0=0x%05X\n",
                num, raw, ctx->gpr[4]._u32[0], ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
    /* NOTE (YDKJ cri_mpv): the cri task's BOOTSTRAP (func_00003040) calls the
     * task-API syscall and EXPECTS IT TO RETURN, then branches to the real task
     * entry (0x3050). Halting on num=0 here kills the task at bootstrap before it
     * runs. So for image 22 we DON'T halt on num=0 -- we return so the bootstrap
     * continues to the decode entry. (A genuine end-of-task EXIT would re-enter and
     * spin; if that happens, gate a real halt after the task has done work.)
     * For non-cri images keep the fork's EXIT=halt semantics. Env YDKJ_CRI_EXIT_HALT
     * forces the old halt behaviour for comparison. */
    if (num == 0 && (ctx->image_id != 22 || getenv("YDKJ_CRI_EXIT_HALT"))) {
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);          /* longjmp out to spu_run_with_halt; post-run writes exit code */
        return;
    }
    /* EXIT(0, cri bootstrap)/YIELD(1)/WAIT_SIGNAL(2)/POLL(3)/RECV_WKL_FLAG(4):
     * report success and resume (return -> lifted caller continues at its link). */
    ctx->gpr[3]._u32[0] = 0;
}

void spu_indirect_branch(spu_context* ctx)
{
    /* Real SPU bi/bisl mask the target to the 256 KB local store; the high bits
     * of a computed pointer (e.g. a packed handle like 0x7a028803) are ignored.
     * Without this, any indirect branch through such a value fails the lookup
     * and falls into branch-to-0. All lifted funcs live below SPU_LS_SIZE, so
     * masking is a no-op for already-valid targets. */
    ctx->pc &= SPU_LS_MASK;
    /* The lifted caller already owns the continuation after a brsl/bisl.  If
     * this `bi` targets its guest return PC, unwind to that C caller; dispatching
     * the continuation here would run it once now and once again after the host
     * call returns. */
    if (spu_host_call_is_return(ctx, ctx->pc)) {
        /* Kernel1's selector returns through the synthetic host-call frame at
         * 0x814.  Capture its actual result before the following firmware
         * helper transforms it into the next workload context.  This is
         * observational only: it distinguishes a real WID-5 selection from
         * an unexpected system/no-work result without changing the lock line. */
        if (getenv("GT6_PDI_KERNEL_SELECT_RESULT_TRACE") &&
            ctx->image_id == 25 && ctx->pc == 0x814u) {
            static SPU_TLS unsigned s_gt6_kernel_select_result_trace;
            if (s_gt6_kernel_select_result_trace++ < 64u)
                fprintf(stderr,
                        "[GT6 Kernel1 select-result] selected=%u current=%u flag=%08X recv=%02X\n",
                        ctx->gpr[3]._u32[0],
                        spu_debug_be32(ctx, GT6_SPURS_CURRENT_WID_LS),
                        spu_guest_be32(ctx->ls + 0x16Cu), ctx->ls[0x177u]);
        }
        /* The local firmware Kernel1 selector has just made a non-poll
         * selection at this return boundary.  The SDK contract is explicit:
         * selecting the workload-flag receiver pulls wklFlag.flag to -1.
         * The lifted path returns the correct WID but loses that one local
         * lock-line write, leaving WID 5 eligible forever.  Reconcile only
         * this already-proven selector effect, and only when opted in.  Both
         * the guest line and the selector's local snapshot are updated so a
         * later PUTLLC cannot restore the stale zero. */
        if (getenv("GT6_PDI_KERNEL1_RECONCILE_FLAG") &&
            ctx->image_id == 25 && ctx->pc == 0x814u) {
            const uint32_t selected = ctx->gpr[3]._u32[0];
            const uint32_t spurs = spu_debug_be32(ctx, 0x1C4u);
            if (selected < 16u && spurs && vm_base &&
                mfc_ea_range_committed(spurs, 0x80u) &&
                vm_base[spurs + 0x77u] == selected &&
                spu_guest_be32(vm_base + spurs + 0x6Cu) == 0u) {
                resv_lock();
                spu_guest_write_be32(vm_base + spurs + 0x6Cu, 0xffffffffu);
                spu_guest_write_be32(ctx->ls + 0x16Cu, 0xffffffffu);
                resv_unlock();
                if (getenv("GT6_PDI_REAL_KERNEL_TRACE"))
                    fprintf(stderr,
                            "[GT6 Kernel1] reconciled flag receiver=%u after firmware selection\n",
                            selected);
            }
        }
        return;
    }
    /* Kernel1's post-selector continuation is entered by a computed branch
     * after the raw policy returns.  Keep that dispatch explicit for the
     * overlapping PDI images: it avoids depending on whichever image most
     * recently populated the generic indirect-branch registry. */
    if (ctx->image_id == 25 && getenv("GT6_PDI_REAL_KERNEL1")) {
        if (ctx->pc == 0x814u) {
            ctx->trampoline_fn = (void*)gt6_spurs_kernel_spu_func_00000814;
            return;
        }
        if (ctx->pc == 0x818u) {
            ctx->trampoline_fn = (void*)gt6_spurs_kernel_spu_func_00000818;
            return;
        }
        if (ctx->pc == 0x844u) {
            ctx->trampoline_fn = (void*)gt6_spurs_kernel_spu_func_00000844;
            return;
        }
    }
    /* The real firmware Kernel1 resumes a selected raw PDI policy at 0xA00.
     * The two images overlap in LS, so switch the dispatcher table exactly at
     * this kernel-to-policy handoff.  It is opt-in until its full lifecycle is
     * validated against GT6's boot path. */
    if (ctx->image_id == 25 && ctx->pc == 0x0A00u &&
        getenv("GT6_PDI_REAL_KERNEL1")) {
        const uint32_t wid = spu_debug_be32(ctx, GT6_SPURS_CURRENT_WID_LS);
        const uint32_t spurs = spu_debug_be32(ctx, 0x1C4u);
        /* Kernel1's WID 0x20 is its no-work/system-service selection.  The
         * PDI bridge only has the raw workload-policy image at LS 0xA00; if
         * it treats this kernel service as PDI, the policy consumes a stale
         * context and immediately issues atomic DMA to EA 0.  Keep the sleep
         * boundary opt-in while validating that the next real ReadyCount can
         * launch a properly addressed workload lane. */
        if (wid >= 16u && getenv("GT6_PDI_SYSTEM_SERVICE_LOCKWAIT")) {
            /* Do not recurse through 0x808 from the current lifted branch.
             * Yield to the runner, which performs the system workload's real
             * lock-line wait before resuming this kernel context. */
            if (getenv("GT6_PDI_REAL_KERNEL_TRACE")) {
                static SPU_TLS unsigned s_gt6_system_idle_trace;
                if (s_gt6_system_idle_trace++ < 16)
                    fprintf(stderr,
                            "[GT6 Kernel1] system-service wid=%u yielded for lock-line wait\n",
                            wid);
            }
            ctx->image_id = 25;
            ctx->pc = GT6_SPURS_KERNEL1_EXIT_ADDR;
            ctx->trampoline_fn = (void*)gt6_spurs_kernel_spu_func_00000808;
            ctx->stop_code = GT6_PDI_SYSTEM_IDLE_STOP;
            ctx->status = SPU_STATUS_STOPPED_BY_STOP;
            spu_halt(ctx);
            return;
        }
        if (wid >= 16u && getenv("GT6_PDI_SKIP_SYSTEM_WID")) {
            if (getenv("GT6_PDI_REAL_KERNEL_TRACE"))
                fprintf(stderr, "[GT6 Kernel1] system-service wid=%u parked before PDI handoff\n", wid);
            ctx->status = SPU_STATUS_STOPPED_BY_STOP;
            spu_halt(ctx);
            return;
        }
        if (getenv("GT6_PDI_REAL_KERNEL_TRACE")) {
            static SPU_TLS unsigned s_gt6_kernel_to_policy_trace;
            static SPU_TLS uint32_t s_gt6_kernel_last_to_policy_wid = UINT32_MAX;
            if (wid != s_gt6_kernel_last_to_policy_wid &&
                s_gt6_kernel_to_policy_trace++ < 32) {
                const uint16_t signals = spurs && mfc_ea_range_committed(spurs + 0x70u, 2u)
                    ? (uint16_t)(((uint16_t)vm_base[spurs + 0x70u] << 8) |
                                 vm_base[spurs + 0x71u]) : 0u;
                s_gt6_kernel_last_to_policy_wid = wid;
                fprintf(stderr,
                        "[GT6 Kernel1] -> policy pc=%05X wid=%u poll=%u rdy=%u sig=%04X gc=%u gp=%u lc=%u lp=%u pr=%u max=%u r3=%08X:%08X:%08X:%08X\n",
                        ctx->pc, wid,
                        spu_debug_be32(ctx, GT6_SPURS_POLL_STATUS_LS),
                        spurs && wid < 16u ? vm_base[spurs + wid] : 0u,
                        signals,
                        spurs && wid < 16u ? vm_base[spurs + 0x20u + wid] : 0u,
                        spurs && wid < 16u ? vm_base[spurs + 0x30u + wid] : 0u,
                        wid < 16u ? ctx->ls[0x180u + wid] : 0u,
                        wid < 16u ? ctx->ls[0x190u + wid] : 0u,
                        wid < 16u ? ctx->ls[0x1A0u + wid] : 0u,
                        spurs && wid < 16u ? vm_base[spurs + 0x50u + wid] : 0u,
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                        ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3]);
            }
        }
        /* Firmware dispatch must pass wklInfo.arg as r4 and the selector's
         * poll status as r5.  The lifted Kernel1 reaches 0xA00 with the
         * selected WID but drops r4, causing every selected PDI policy to
         * receive a null worker-context argument.  Rebuild that documented
         * dispatch ABI from the original guest descriptor, not host state.
         * Keep it opt-in until the PDI continuation proves the handoff. */
        if (getenv("GT6_PDI_KERNEL1_DISPATCH_ABI") && wid < 16u && spurs &&
            vm_base && mfc_ea_range_committed(spurs + 0xB00u + wid * 0x20u,
                                               0x20u)) {
            const uint8_t* info = vm_base + spurs + 0xB00u + wid * 0x20u;
            const uint32_t arg_hi = spu_guest_be32(info + 0x08u);
            const uint32_t arg_lo = spu_guest_be32(info + 0x0Cu);
            const uint32_t poll = spu_debug_be32(ctx, GT6_SPURS_POLL_STATUS_LS);
            ctx->gpr[4]._u32[0] = arg_hi;
            ctx->gpr[4]._u32[1] = arg_lo;
            if (arg_hi == 0u)
                ctx->gpr[4]._u32[0] = arg_lo;
            ctx->gpr[5]._u32[0] = poll;
            if (getenv("GT6_PDI_REAL_KERNEL_TRACE"))
                fprintf(stderr,
                        "[GT6 Kernel1] restored dispatch ABI wid=%u arg=%08X:%08X poll=%u\n",
                        wid, arg_hi, arg_lo, poll);
        }
        ctx->image_id = 24;
    }
    /* Taskset PM task-syscall entry (LS 0xA70): HLE it instead of branching into
     * (absent) PM code. The cri task (image 22) reaches here via syscallAddr. */
    if (ctx->pc == YDKJ_TASKSET_PM_SYSCALL_ADDR && ctx->image_id == 22) {
        spu_spurs_taskset_syscall(ctx); return;
    }
    /* The raw GT6 PDI image is only the workload policy at LS 0xA00; its two
     * kernel callbacks live outside that image.  0x808 exits/yields back to
     * the SPURS kernel, so end this bounded host dispatch cleanly.  0x290 is
     * SelectWorkload: until the full kernel scheduler is lifted, report the
     * currently dispatched wid and its cached poll bits.  The policy consumes
     * this as r3={wid,pollStatus,...} and can continue without an unresolved
     * branch.  This deliberately does not perform a workload context switch. */
    if (ctx->image_id == 24 && ctx->pc == GT6_SPURS_KERNEL1_EXIT_ADDR) {
        if (getenv("GT6_PDI_REAL_KERNEL1")) {
            /* 0x808 is a real entry in the firmware kernel (not a policy
             * stop).  The regenerated image includes this formerly missing
             * boundary; execute it through the normal trampoline so nested
             * kernel->policy calls preserve the same SPU context. */
            if (getenv("GT6_PDI_REAL_KERNEL_TRACE")) {
                static SPU_TLS unsigned s_gt6_policy_to_kernel_trace;
                static SPU_TLS uint32_t s_gt6_kernel_last_to_kernel_wid = UINT32_MAX;
                const uint32_t wid = spu_debug_be32(ctx, GT6_SPURS_CURRENT_WID_LS);
                if (wid != s_gt6_kernel_last_to_kernel_wid &&
                    s_gt6_policy_to_kernel_trace++ < 32) {
                    s_gt6_kernel_last_to_kernel_wid = wid;
                    fprintf(stderr,
                            "[GT6 Kernel1] <- policy pc=%05X wid=%u poll=%u r0=%08X:%08X:%08X:%08X r3=%08X:%08X:%08X:%08X\n",
                            ctx->pc, wid,
                            spu_debug_be32(ctx, GT6_SPURS_POLL_STATUS_LS),
                            ctx->gpr[0]._u32[0], ctx->gpr[0]._u32[1],
                            ctx->gpr[0]._u32[2], ctx->gpr[0]._u32[3],
                            ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                            ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3]);
                }
            }
            ctx->image_id = 25;
            ctx->trampoline_fn = (void*)gt6_spurs_kernel_spu_func_00000808;
            return;
        }
        static int s_gt6_exit = 0;
        if (s_gt6_exit++ < 4)
            fprintf(stderr, "[GT6 SPURS] PDI yielded to kernel exit 0x808\n");
        if (getenv("GT6_PDI_EXITTRACE")) {
            static int s_exit_trace = 0;
            if (s_exit_trace++ < 12) {
                fprintf(stderr,
                    "[GT6 PDI EXIT] r3=%08X r4=%08X:%08X r5=%08X r6=%08X "
                    "r7=%08X r13=%08X r14=%08X r15=%08X ev=%08X mask=%08X\n",
                    ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                    ctx->gpr[4]._u32[1], ctx->gpr[5]._u32[0],
                    ctx->gpr[6]._u32[0], ctx->gpr[7]._u32[0],
                    ctx->gpr[13]._u32[0], ctx->gpr[14]._u32[0],
                    ctx->gpr[15]._u32[0], ctx->event_status, ctx->event_mask);
                /* r3 is the policy's local scheduler-workload record on its
                 * return to Kernel1 (currently LS 0xB00).  Capture it before
                 * the host bridge halts this dispatch: it is the concrete
                 * input needed to model the next workload selection, rather
                 * than inventing an LV2 queue payload. */
                const uint32_t record = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
                if (record + 0x40u <= SPU_LS_SIZE) {
                    fprintf(stderr, "[GT6 PDI EXIT] record@%05X:", record);
                    for (uint32_t off = 0; off < 0x40u; off += 4u) {
                        const uint32_t word = ((uint32_t)ctx->ls[record + off] << 24) |
                            ((uint32_t)ctx->ls[record + off + 1] << 16) |
                            ((uint32_t)ctx->ls[record + off + 2] << 8) |
                            (uint32_t)ctx->ls[record + off + 3];
                        fprintf(stderr, " %08X", word);
                    }
                    fputc('\n', stderr);
                }
            }
        }
        /* Diagnostic continuation of one real Kernel1 service call.  The
         * lifted policy links this call to 0x36A4 and immediately invokes
         * SelectWorkload (0x290); returning the KernelContext temporary area
         * lets that existing policy code consume the select result.  Keep it
         * opt-in and once-per-host-worker while the full persistent Kernel1
         * state machine is still being reconstructed. */
        if (getenv("GT6_PDI_KERNEL_CONTINUE")) {
            static SPU_TLS unsigned s_gt6_kernel_continue_count = 0;
            unsigned continue_limit = 1;
            const char* continue_limit_text = getenv("GT6_PDI_KERNEL_CONTINUE_LIMIT");
            if (continue_limit_text) {
                const long parsed = strtol(continue_limit_text, NULL, 10);
                if (parsed >= 1 && parsed <= 8)
                    continue_limit = (unsigned)parsed;
            }
            if (s_gt6_kernel_continue_count++ < continue_limit) {
                memset(&ctx->gpr[3], 0, sizeof(ctx->gpr[3]));
                ctx->gpr[3]._u32[0] = 0x100u; /* SpursKernelContext.tempArea */
                if (getenv("GT6_PDI_EXITTRACE"))
                    fprintf(stderr, "[GT6 PDI EXIT] continuing Kernel1 return %u/%u via tempArea=0x100\n",
                            s_gt6_kernel_continue_count, continue_limit);
                return;
            }
        }
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);
        return;
    }
    if (ctx->image_id == 24 && ctx->pc == GT6_SPURS_SELECT_WORKLOAD_ADDR) {
        /* Exercise the firmware's selector independently from the larger
         * kernel handoff.  This keeps the established PDI continuation and
         * asks only the authentic 0x290 code to produce its result vector and
         * lock-line updates.  It is intentionally opt-in while GT6's two-lane
         * lifecycle is still under validation. */
        if (getenv("GT6_PDI_LIFTED_SELECTOR")) {
            gt6_spurs_kernel_spu_func_00000290(ctx);
            /* The lifted entry schedules its real body through trampoline_fn
             * (first at 0x324 for the atomic lock-line). Calling only the C
             * entry returned the pre-call registers and falsely diagnosed the
             * firmware selector as zero. Drain that genuine continuation just
             * like a normal lifted brsl chain; this remains opt-in because it
             * can now update the real SPURS reservation line. */
            spu_drain_trampoline(ctx);
            if (getenv("GT6_PDI_LIFTED_SELECTOR_TRACE")) {
                static SPU_TLS unsigned s_gt6_lifted_selector_trace = 0;
                if (s_gt6_lifted_selector_trace++ < 16)
                    fprintf(stderr,
                        "[GT6 PDI LIFTED SELECT] r3=%08X:%08X:%08X:%08X r0=%05X r4=%08X:%08X:%08X:%08X pc=%05X\n",
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                        ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3],
                        ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                        ctx->gpr[4]._u32[0], ctx->gpr[4]._u32[1],
                        ctx->gpr[4]._u32[2], ctx->gpr[4]._u32[3],
                        ctx->pc & SPU_LS_MASK);
            }
            return;
        }
        /* The complete selector below is retained behind an explicit probe
         * while its vector return convention is being reconciled with this
         * lifter.  The proven path only refreshes the three poll causes from
         * the live lock line; it must remain the default until the complete
         * return is byte-for-byte compatible with the raw policy. */
        if (!getenv("GT6_PDI_KERNEL1_SELECTOR")) {
            const uint8_t* widp = ctx->ls + GT6_SPURS_CURRENT_WID_LS;
            const uint8_t* pollp = ctx->ls + GT6_SPURS_POLL_STATUS_LS;
            const uint32_t wid = ((uint32_t)widp[0] << 24) |
                                 ((uint32_t)widp[1] << 16) |
                                 ((uint32_t)widp[2] << 8) | widp[3];
            const uint32_t cached_poll = ((uint32_t)pollp[0] << 24) |
                                         ((uint32_t)pollp[1] << 16) |
                                         ((uint32_t)pollp[2] << 8) | pollp[3];
            uint32_t poll = cached_poll;
            const uint8_t* sp = ctx->ls + 0x1C0u;
            const uint32_t spurs = ((uint32_t)sp[4] << 24) |
                                   ((uint32_t)sp[5] << 16) |
                                   ((uint32_t)sp[6] << 8) | sp[7];
            if (vm_base && spurs && wid < 16u) {
                const uint16_t signals = (uint16_t)(((uint16_t)vm_base[spurs + 0x70u] << 8) |
                                                     vm_base[spurs + 0x71u]);
                poll = 0;
                if (vm_base[spurs + wid] != 0)
                    poll |= 1u;
                if (signals & (uint16_t)(0x8000u >> wid))
                    poll |= 2u;
                if (spu_guest_be32(vm_base + spurs + 0x6Cu) == 0u &&
                    vm_base[spurs + 0x77u] == wid)
                    poll |= 4u;
            }
            memset(&ctx->gpr[3], 0, sizeof(ctx->gpr[3]));
            ctx->gpr[3]._u32[0] = wid;
            ctx->gpr[3]._u32[1] = poll;
            /* The raw policy subsequently rotates this value and compares it
             * against KernelContext[0x1D0].  Probe the complete context-vector
             * form without changing the established scalar fallback: word 2
             * is currentAddr.low and word 3 is the current unique id. */
            if (getenv("GT6_PDI_SELECT_CONTEXT_VECTOR")) {
                ctx->gpr[3]._u32[2] = ((uint32_t)ctx->ls[0x1D4u] << 24) |
                                      ((uint32_t)ctx->ls[0x1D5u] << 16) |
                                      ((uint32_t)ctx->ls[0x1D6u] << 8) | ctx->ls[0x1D7u];
                ctx->gpr[3]._u32[3] = ((uint32_t)ctx->ls[0x1D8u] << 24) |
                                      ((uint32_t)ctx->ls[0x1D9u] << 16) |
                                      ((uint32_t)ctx->ls[0x1DAu] << 8) | ctx->ls[0x1DBu];
            }
            if (getenv("GT6_PDI_EXITTRACE")) {
                static SPU_TLS unsigned s_gt6_select_trace_count = 0;
                if (s_gt6_select_trace_count++ < 12)
                    fprintf(stderr, "[GT6 PDI SELECT] current=%u poll=%u cached=%u mode=live\n",
                            wid, poll, cached_poll);
            }
            return;
        }
        /* This is cellSpursModulePollStatus, not a request to report the
         * current WID.  The raw PDI module calls it with r3=1 after a Kernel1
         * yield.  The previous bridge returned that WID with a cached status,
         * which made a completed workload select itself indefinitely.  Mirror
         * Kernel1's real decision using the live 128-byte SPURS lock-line and
         * the per-SPU fields already materialised in SpursKernelContext. */
        const uint8_t* widp = ctx->ls + GT6_SPURS_CURRENT_WID_LS;
        const uint8_t* sp = ctx->ls + 0x1C0u;
        const uint32_t current = ((uint32_t)widp[0] << 24) |
                                 ((uint32_t)widp[1] << 16) |
                                 ((uint32_t)widp[2] << 8) | widp[3];
        const uint32_t spurs = ((uint32_t)sp[4] << 24) |
                               ((uint32_t)sp[5] << 16) |
                               ((uint32_t)sp[6] << 8) | sp[7];
        const uint32_t is_poll = ctx->gpr[3]._u32[0] != 0;
        const uint16_t runnable = (uint16_t)(((uint16_t)ctx->ls[0x1ECu] << 8) |
                                              ctx->ls[0x1EDu]);
        uint32_t selected = 32u; /* system service / no runnable workload */
        uint32_t poll = 0;
        uint16_t best_weight = 0;
        uint8_t contention[16];
        uint8_t pending[16];

        if (vm_base && spurs && mfc_ea_range_committed(spurs, 0x80u)) {
            resv_lock();
            const uint16_t signals = (uint16_t)(((uint16_t)vm_base[spurs + 0x70u] << 8) |
                                                 vm_base[spurs + 0x71u]);
            const uint32_t flag = spu_guest_be32(vm_base + spurs + 0x6Cu);
            const uint8_t flag_receiver = vm_base[spurs + 0x77u];
            const uint32_t spu_num = ((uint32_t)ctx->ls[0x1C8u] << 24) |
                                     ((uint32_t)ctx->ls[0x1C9u] << 16) |
                                     ((uint32_t)ctx->ls[0x1CAu] << 8) | ctx->ls[0x1CBu];

            for (uint32_t i = 0; i < 16u; ++i) {
                contention[i] = (uint8_t)(vm_base[spurs + 0x20u + i] -
                                           ctx->ls[0x180u + i]);
                pending[i] = (uint8_t)(vm_base[spurs + 0x30u + i] -
                                       ctx->ls[0x190u + i]);
                if (is_poll && i != current)
                    contention[i] = (uint8_t)(contention[i] + pending[i]);
            }

            if (vm_base[spurs + 0x72u] & (uint8_t)(1u << (spu_num & 7u))) {
                /* The system service wins. In poll mode it only consumes its
                 * message when it is already the current workload. */
                if (!is_poll || current == 32u)
                    vm_base[spurs + 0x72u] &= (uint8_t)~(1u << (spu_num & 7u));
                ctx->ls[0x1EBu] = 0; /* SpursKernelContext::spuIdling */
            } else {
                for (uint32_t wid = 0; wid < 16u; ++wid) {
                    const uint8_t priority = ctx->ls[0x1A0u + wid];
                    const uint8_t max_contention = vm_base[spurs + 0x50u + wid];
                    const uint8_t min_contention = vm_base[spurs + 0x40u + wid];
                    const uint8_t ready = vm_base[spurs + wid] > 8u ? 8u : vm_base[spurs + wid];
                    const uint8_t idle = vm_base[spurs + 0x10u + wid] > 8u ? 8u : vm_base[spurs + 0x10u + wid];
                    const uint8_t signalled = (signals & (uint16_t)(0x8000u >> wid)) != 0;
                    const uint8_t flagged = flag == 0u && flag_receiver == wid;
                    const uint8_t cont = contention[wid];

                    if (!(runnable & (uint16_t)(0x8000u >> wid)) || !priority ||
                        max_contention <= cont ||
                        !(flagged || signalled || (ready != 0u && (uint8_t)(ready + idle) > cont)))
                        continue;

                    uint16_t weight = (flagged || signalled || ready > cont) ? 0x8000u : 0u;
                    weight |= (uint16_t)(priority & 0x7fu) << 8;
                    weight |= wid == current ? 0x80u : 0u;
                    weight |= cont && min_contention > cont ? 0x40u : 0u;
                    weight |= (uint16_t)((8u - (cont > 8u ? 8u : cont)) & 0x0fu) << 2;
                    weight |= 1u;
                    if (weight > best_weight) {
                        selected = wid;
                        best_weight = weight;
                        poll = (ready > cont ? 1u : 0u) |
                               (signalled ? 2u : 0u) |
                               (flagged ? 4u : 0u);
                    }
                }

                ctx->ls[0x1EBu] = selected == 32u ? 1u : 0u;
                if ((!is_poll || selected == current) && selected < 16u) {
                    const uint16_t updated = (uint16_t)(signals &
                        ~(uint16_t)(0x8000u >> selected));
                    vm_base[spurs + 0x70u] = (uint8_t)(updated >> 8);
                    vm_base[spurs + 0x71u] = (uint8_t)updated;
                    if (flag_receiver == selected)
                        spu_guest_write_be32(vm_base + spurs + 0x6Cu, 0xffffffffu);
                }
            }

            if (!is_poll) {
                if (selected < 16u)
                    contention[selected]++;
                for (uint32_t i = 0; i < 16u; ++i) {
                    vm_base[spurs + 0x20u + i] = contention[i];
                    vm_base[spurs + 0x30u + i] = (uint8_t)(pending[i] - ctx->ls[0x190u + i]);
                    ctx->ls[0x180u + i] = 0;
                    ctx->ls[0x190u + i] = 0;
                }
                if (selected < 16u)
                    ctx->ls[0x180u + selected] = 1u;
                ctx->ls[0x1DCu + 3u] = (uint8_t)selected;
            } else if (selected != current) {
                if (selected < 16u)
                    pending[selected]++;
                for (uint32_t i = 0; i < 16u; ++i) {
                    vm_base[spurs + 0x30u + i] = pending[i];
                    ctx->ls[0x190u + i] = 0;
                }
                if (selected < 16u)
                    ctx->ls[0x190u + selected] = 1u;
            } else {
                for (uint32_t i = 0; i < 16u; ++i) {
                    vm_base[spurs + 0x30u + i] = (uint8_t)(pending[i] - ctx->ls[0x190u + i]);
                    ctx->ls[0x190u + i] = 0;
                }
            }

            /* Kernel1 leaves a current snapshot of its lock line in the
             * context. This keeps the lifted PDI code and the next callback
             * on the same observable scheduler state. */
            memcpy(ctx->ls + 0x100u, vm_base + spurs, 0x80u);
            resv_unlock();
        }

        memset(&ctx->gpr[3], 0, sizeof(ctx->gpr[3]));
        ctx->gpr[3]._u32[0] = selected;
        ctx->gpr[3]._u32[1] = poll;
        if (getenv("GT6_PDI_EXITTRACE")) {
            static SPU_TLS unsigned s_gt6_select_trace_count = 0;
            if (s_gt6_select_trace_count++ < 12)
                fprintf(stderr, "[GT6 PDI SELECT] current=%u selected=%u poll=%u mode=%s runnable=%04X\n",
                        current, selected, poll, is_poll ? "poll" : "kernel", runnable);
        }
        return;
    }
    /* YDKJ_CRI_R4: the taskset policy entry (LS 0xA00, image 23) writes r4 into
     * SpursTasksetContext.taskset @LS 0x27B8 (per RPCS3 cellSpursSpu.cpp). Our
     * kernel->policy handoff doesn't convey the taskset EA, so the policy DMAs
     * the taskset from garbage -> waiting!=0 -> wrong resume path -> savedContextLr=0.
     * Inject r4 = taskset EA (0x0F000000) at the policy entry dispatch (this is the
     * exact point before the entry reads r4, after the kernel's arg setup). */
    if (ctx->image_id == 23 && !getenv("YDKJ_NO_CRI_R4")) {
        static int s_r4 = -1; if (s_r4 < 0) s_r4 = getenv("YDKJ_CRI_CHAIN") ? 1 : 0;
        if (s_r4) {
            /* Force ctxt->taskset @LS 0x27B8 = the REAL game taskset EA on every
             * image-23 branch, so the policy's atomic reads + context-EA computation
             * use the actual taskset (the r4 handoff sets it to garbage 0x0000FFFF via
             * a path we can't intercept). Was hardcoded 0x0F000000, which mismatched
             * the game's real taskset (0x45F1B000) -> policy DMA'd garbage. */
            extern uint32_t g_ydkj_real_taskset_ea;
            uint32_t ts = g_ydkj_real_taskset_ea ? g_ydkj_real_taskset_ea : 0x0F000000u;
            ctx->ls[0x27B8]=0x00; ctx->ls[0x27B9]=0x00; ctx->ls[0x27BA]=0x00; ctx->ls[0x27BB]=0x00;
            ctx->ls[0x27BC]=(uint8_t)(ts>>24); ctx->ls[0x27BD]=(uint8_t)(ts>>16); ctx->ls[0x27BE]=(uint8_t)(ts>>8); ctx->ls[0x27BF]=(uint8_t)ts;
            if (ctx->pc == 0xA00u) { static int _n=0; if (_n++ < 4)
                fprintf(stderr, "[cri-r4] policy entry pc=0xA00: forced ctxt->taskset LS[0x27B8]=0x0F000000\n"); }
        }
    }
    { static int s_ib = -1; if (s_ib < 0) s_ib = getenv("YDKJ_IBTRACE") ? 1 : 0;
      if (s_ib && ctx->image_id == 23) { static int _i = 0; if (_i++ < 60)
        fprintf(stderr, "[ib23] target=0x%05X lr=0x%05X\n",
                ctx->pc, ctx->gpr[0]._u32[0] & 0x3FFFF); } }
    /* The GT6 PDI policy is a raw SPURS module (image 24).  Its bootstrap is
     * driven by internal SPU event bits rather than an outbound mailbox, so
     * record its first dispatches on demand.  Keep this bounded: it is only a
     * bootstrap-state diagnostic, not a permanent instruction trace. */
    { static int s_gt6 = -1; if (s_gt6 < 0) s_gt6 = getenv("GT6_PDI_IBTRACE") ? 1 : 0;
      if (s_gt6 && ctx->image_id == 24) { static int _i = 0; if (_i++ < 96)
        fprintf(stderr, "[gt6-pdi-ib] target=0x%05X lr=0x%05X ev=%08X mask=%08X\n",
                ctx->pc, ctx->gpr[0]._u32[0] & 0x3FFFF,
                ctx->event_status, ctx->event_mask); } }
    /* The PDI policy's idle/dispatch transition is selected by small fields in
     * its local state (not by the MFC list-stall event itself).  Capture the
     * actual values at its branch boundaries so the next correction can follow
     * the guest state rather than guessing a completion or callback. */
    { static int s_gt6_loop = -1;
      if (s_gt6_loop < 0) s_gt6_loop = getenv("GT6_PDI_LOOP_STATE_TRACE") ? 1 : 0;
      if (s_gt6_loop && ctx->image_id == 24 &&
          (ctx->pc == 0x02C58u || ctx->pc == 0x02C60u || ctx->pc == 0x02C70u ||
           ctx->pc == 0x02C80u || ctx->pc == 0x02CA0u || ctx->pc == 0x02CC0u ||
           ctx->pc == 0x02CD8u || ctx->pc == 0x02CE8u || ctx->pc == 0x02CF0u ||
           ctx->pc == 0x02D00u || ctx->pc == 0x02D18u || ctx->pc == 0x02D20u ||
           ctx->pc == 0x02D38u || ctx->pc == 0x02D58u || ctx->pc == 0x02D68u ||
           ctx->pc == 0x02DB0u)) {
        static SPU_TLS unsigned s_gt6_loop_count;
        if (s_gt6_loop_count++ < 192u) {
            fprintf(stderr,
                    "[GT6 PDI loop] pc=%05X wid=%u 12a0=%08X 12f0=%08X 12c0=%08X 1340=%08X 1430=%08X 1510=%08X 1520=%08X 1530=%08X r3=%08X r4=%08X r5=%08X\n",
                    ctx->pc, spu_debug_be32(ctx, GT6_SPURS_CURRENT_WID_LS),
                    spu_debug_be32(ctx, 0x12A0u), spu_debug_be32(ctx, 0x12F0u),
                    spu_debug_be32(ctx, 0x12C0u), spu_debug_be32(ctx, 0x1340u),
                    spu_debug_be32(ctx, 0x1430u), spu_debug_be32(ctx, 0x1510u),
                    spu_debug_be32(ctx, 0x1520u), spu_debug_be32(ctx, 0x1530u),
                    ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0]);
        }
      } }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("YDKJ_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_last = 0; static uint64_t s_run = 0;
        if (ctx->pc == s_last) s_run++; else { s_last = ctx->pc; s_run = 1; }
        if ((++s_n % 2000000) == 0)
          fprintf(stderr, "[ibranch] %llu indirect branches; current target=0x%05X run=%llu\n",
                  (unsigned long long)s_n, ctx->pc, (unsigned long long)s_run); } }
    /* Policy-entry trace: the SPURS policy at LS 0xA00 branches on
     * r8 = word at LS[r6] (must be 32 for the path that sets the dispatch ptr
     * LS[0x780]). Log r3..r6 + the word the kernel handed it, to see why the
     * wrong branch is taken. Env YDKJ_POLTRACE. */
    if (ctx->pc == 0xA00u) {
        static int64_t pt=-2; if (pt==-2){ const char* e=getenv("YDKJ_POLTRACE"); pt=e?1:0; }
        if (pt) { static int _p=0; if (_p++ < 8) {
            /* re-lifted policy entry uses r80 (kernel-set context base): r44=LS[r80+0xC0] */
            uint32_t r80=ctx->gpr[80]._u32[0] & SPU_LS_MASK;
            const uint8_t* q = ctx->ls + ((r80+0xC0)&SPU_LS_MASK);
            uint32_t ctxw = ((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
            fprintf(stderr, "[POLTRACE] policy@0xA00 r3=%08X r4=%08X r80=%08X  LS[r80+0xC0]=%08X\n",
                ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], r80, ctxw);
            fflush(stderr);
        } }
    }
    spu_fn fn = spu_lookup(ctx->pc, ctx->image_id);
    if (fn) {
        ctx->trampoline_fn = (void*)fn;
        return;
    }
    { static int _bt0=0; if (_bt0++ < 12)
        fprintf(stderr, "[SPU] BRANCH-TO-0 unresolved pc=0x%05X image=%d lr=0x%05X\n",
                ctx->pc, ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
    { static int _n=0; if (_n++ < 2) {
        fprintf(stderr, "[SPU] branch-to-0 lr=0x%05X r1=0x%05X\n",
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->gpr[1]._u32[0] & SPU_LS_MASK);
#ifdef _WIN32
        void* frames[24]; unsigned short fn = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        char* base = (char*)GetModuleHandleA(NULL);
        fprintf(stderr, "[SPU] host bt RVAs:");
        for (unsigned short i = 0; i < fn; i++)
            fprintf(stderr, " 0x%zX", (size_t)((char*)frames[i] - base));
        fprintf(stderr, "\n");
#endif
        /* State-diff oracle: dump full LS + all GPRs at the branch-to-0 so it can
         * be compared byte-for-byte against the RPCS3 savestate LS of the same
         * (cri_mpv) task. Path from YDKJ_SPU_LSDUMP, else ./recomp_spu_ls.bin. */
        const char* dp = getenv("YDKJ_SPU_LSDUMP");
        if (!dp || !*dp) dp = "recomp_spu_ls.bin";
        FILE* lf = fopen(dp, "wb");
        if (lf) { fwrite(ctx->ls, 1, SPU_LS_SIZE, lf); fclose(lf);
                  fprintf(stderr, "[SPU] dumped 256KB LS -> %s\n", dp); }
        fprintf(stderr, "[SPU] image_id=%d  GPR dump (r0..r127, hi64:lo64 of each quadword, preferred slot = _u32[0]):\n", ctx->image_id);
        for (int g = 0; g < 128; g++) {
            fprintf(stderr, " r%-3d=%08X %08X %08X %08X", g,
                    ctx->gpr[g]._u32[0], ctx->gpr[g]._u32[1],
                    ctx->gpr[g]._u32[2], ctx->gpr[g]._u32[3]);
            if ((g & 1) == 1) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        /* echo the dispatch chain values the way func_00026DE0 computes them */
        { uint32_t bec0 = ctx->ls[0xBEC0]<<24 | ctx->ls[0xBEC1]<<16 | ctx->ls[0xBEC2]<<8 | ctx->ls[0xBEC3];
          fprintf(stderr, "[SPU] LS[0xBEC0].w0=0x%08X  LS[0x2d4e0:16]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
            bec0,
            ctx->ls[0x2d4e0],ctx->ls[0x2d4e1],ctx->ls[0x2d4e2],ctx->ls[0x2d4e3],
            ctx->ls[0x2d4e4],ctx->ls[0x2d4e5],ctx->ls[0x2d4e6],ctx->ls[0x2d4e7],
            ctx->ls[0x2d4e8],ctx->ls[0x2d4e9],ctx->ls[0x2d4ea],ctx->ls[0x2d4eb],
            ctx->ls[0x2d4ec],ctx->ls[0x2d4ed],ctx->ls[0x2d4ee],ctx->ls[0x2d4ef]); }
    } }
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* ===========================================================================
 * Execution trace (for §3 validation: diff vs RPCS3 SPU interpreter)
 *
 * When the lifter is invoked with --trace, every emitted instruction is
 * surrounded by spu_trace_pc(ctx, PC) before execution and spu_trace_rt(
 * ctx, RT) after, for instructions whose destination is the rt slot. The
 * output is one line per event:
 *
 *     <PC-5hex>                          - PC about to execute
 *       r<rt> <hi-64hex> <lo-64hex>      - register written, post-state
 *
 * Direct to stderr by default; call spu_trace_init(path) once at startup
 * to redirect to a file. The format is intentionally minimal and stable
 * so a small converter can line it up against an RPCS3.log SPU trace.
 * ===========================================================================*/
static FILE* s_trace_fp = NULL;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

void spu_trace_pc(spu_context* ctx, uint32_t pc)
{
    (void)ctx;
    if (!s_trace_fp) s_trace_fp = stderr;
    fprintf(s_trace_fp, "%05X\n", pc & SPU_LS_MASK);
}

void spu_trace_rt(spu_context* ctx, uint32_t rt)
{
    if (!s_trace_fp) s_trace_fp = stderr;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif
