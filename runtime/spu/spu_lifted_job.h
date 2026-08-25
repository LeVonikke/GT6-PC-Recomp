/* spu_lifted_job.h — run a lifted SPU job with the SPURS task ABI.
 *
 * The bridge between the lv2 SPU-thread-group layer (runtime/syscalls/lv2_register.c,
 * which runs SPU threads as registered PPU-fallbacks) and the lifted-execution layer
 * (a lifted spu_func from spu_lifter). A SPURS task / SPU thread receives its argument
 * (the job/task descriptor effective address) in r3 and runs against its 256 KB local
 * store; this helper sets that up, runs the lifted entry, and bridges the local store.
 *
 * Wiring into lv2: register `spu_lifted_fallback` for an SPU image's entry point with
 * `user` = the lifted entry fn; lv2's spu_fallback_thread_proc then calls it with
 * (tid, args_ea, args_size, user), and we run the lifted job on that thread's LS.
 */
#ifndef SPU_LIFTED_JOB_H
#define SPU_LIFTED_JOB_H

#include <stdlib.h>

#include "spu_context.h"
#include <string.h>
#include <stdio.h>

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* Run a lifted SPU job. `local_store` (256 KB, may be NULL) is the SPU thread's
 * local store; it is brought into the context, the job's arg EA is placed in r3
 * (the SPURS task ABI), the lifted entry runs, and the LS is written back.
 * The caller links the channel ABI (spu_wrch/spu_rdch/...) that the lifted code
 * uses to reach DMA / mailboxes / events. Returns the job's exit code (0). */
/* spurs_task_abi: when nonzero, set up r3 per the SPURS task kernel ABI instead
 * of the simple-job ABI. A SPURS task entry expects r3 (128-bit) = a pair:
 *   word0 = {high half 0x40 (kernel marker), low half = DMA tag}
 *   word1 = eaContext  (the task's context save area; the entry DMAs it in)
 * The simple-job ABI just puts the single arg EA in word0. (Verified against the
 * lifted SPURS entry: it checks (r3.word0 >> 16) == 0x40 then DMAs 64 bytes from
 * r3.word1, tag = r3.word0 & 0xFFFF — see spu_func_00003E68/00003ED8.) */
/* r3_override (optional, 4 words, big-endian/native lane values): when non-NULL
 * and spurs_task_abi is set, the full 128-bit r3 the SPURS kernel hands the task
 * is supplied by the caller (captured race-free at dispatch time from the game's
 * eaContext+0x10 descriptor: {0x40-marker handle, eaContext, queue/lock EA,
 * ...}). The claim CAS computes its atomic EA from r3.word2/3 & 0xFFFFFF80, so a
 * zero there locks address 0 and the runtime finds "no ready task". */
static inline int32_t spu_run_lifted_job_abi(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id,
                                             int spurs_task_abi,
                                             const uint32_t* r3_override)
{
    if (!entry) return -1;
    spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = image_id;     /* select this image's indirect-branch table */
    /* The raw PDI policy has an endless scheduler dispatch loop.  Under MSVC
     * the lifted tail branches are ordinary host calls, so keeping that loop
     * resident eventually exhausts the host thread stack.  Run enough of the
     * genuine policy to initialize its MFC state, then yield to the PPU-facing
     * scheduler tick bridge. */
    if (image_id == 24)
        ctx.indirect_branch_budget = 5000u;
    /* Initialize the SPU stack pointer to the top of local store (the SPU ABI
     * expects r1 = top-16, 16-byte aligned, with a NULL back-chain). Without
     * this it is 0 from spu_context_init, so the first `r1 -= frame` wraps
     * negative -> garbage stack -> null function pointers -> branch to LS 0. */
    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;   /* 0x3FFF0 for a 256KB LS */
    if (local_store) memcpy(ctx.ls, local_store, SPU_LS_SIZE);  /* job's LS in */
    if (spurs_task_abi) {
        if (r3_override) {
            ctx.gpr[3]._u32[0] = r3_override[0];   /* 0x40-marker handle      */
            ctx.gpr[3]._u32[1] = args_ea;          /* eaContext (DMA'd first) */
            ctx.gpr[3]._u32[2] = r3_override[2];   /* queue/lock EA           */
            ctx.gpr[3]._u32[3] = r3_override[3];
        } else if (image_id == 22) {
            /* cri_mpv leaf (func_00003E68) gate is `rotmai(r3.word0, 112) == 64`
             * = an ARITHMETIC RIGHT-SHIFT BY 16 then ceqi 64, i.e. it wants
             * (r3.word0 >> 16) == 0x40  ->  r3.word0 = 0x0040xxxx (0x40 in bits
             * 16-23, low 16 = DMA tag). If it matches it takes the REAL decode
             * path (func_00003ED8, which DMAs the video job); else it halts
             * (func_00003ED0). NOTE: r3.word0 = 0x40 (my earlier misread) shifts
             * to 0 and FAILS the gate -> no decode, no DMA. Use 0x00400000. */
            ctx.gpr[3]._u32[0] = 0x00400000u;   /* >>16 == 64 -> cri real decode path */
            ctx.gpr[3]._u32[1] = args_ea;       /* eaContext -> r3.word1 */
        } else {
            ctx.gpr[3]._u32[0] = 0x00400000u;   /* 0x40 marker (>>16==64), DMA tag 0 */
            ctx.gpr[3]._u32[1] = args_ea;       /* eaContext -> r3.word1 */
        }
    } else {
        ctx.gpr[3]._u32[0] = args_ea;                           /* simple-job arg -> r3 */
    }
    /* The r3_override path already carries the game's 0x0040xxxx marker (which
     * passes the (r3.word0>>16)==0x40 gate), so do NOT clobber it. The earlier
     * forced r3.word0=0x40 here was a misread of the gate (see above) and made
     * the cri task halt at func_00003ED0 instead of decoding -- removed. */
    /* SPURS leaf ABI: the real PM (spursTasksetStartTask) sets r4 = {taskset->args (d0),
     * taskset->spurs EA (d1)}, read from the SpursTasksetContext at LS 0x2700+0x60/0x68
     * (planted by spurs_pm_build_context). Without it the leaf reads a garbage SPURS base
     * and DMAs from a bad address / bails at init. Set for image 22 (cri) when the context
     * carries a non-zero spurs ptr. */
    if (spurs_task_abi && image_id == 22) {
        #define LB(o) (((uint32_t)ctx.ls[(o)]<<24)|((uint32_t)ctx.ls[(o)+1]<<16)|((uint32_t)ctx.ls[(o)+2]<<8)|ctx.ls[(o)+3])
        uint32_t spurs_lo = LB(0x2764);
        if (spurs_lo) {
            ctx.gpr[4]._u32[0] = LB(0x2768);   /* args hi (d0) */
            ctx.gpr[4]._u32[1] = LB(0x276C);   /* args lo      */
            ctx.gpr[4]._u32[2] = LB(0x2760);   /* spurs hi (d1)*/
            ctx.gpr[4]._u32[3] = spurs_lo;     /* spurs lo = CellSpurs EA */
        }
        #undef LB
    }
    { extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
      spu_run_with_halt(entry, &ctx); }                         /* run with halt pad   */
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);  /* LS back out */
    return 0;
}

static inline int32_t spu_run_lifted_job_img(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id)
{
    return spu_run_lifted_job_abi(entry, local_store, args_ea, image_id, 0, 0);
}

/* Run a raw CellSpurs workload policy after its image and SpursKernelContext
 * have been installed in LS by the title bridge.  The real kernel dispatch ABI
 * is r0=kernel-exit, r1=policy stack, r3=kernel context, r4=64-bit workload
 * data and r5=poll status; execution starts at LS 0xA00. */
static inline int32_t spu_run_lifted_pdi_policy(spu_lifted_entry_fn entry,
                                                uint8_t* local_store,
                                                uint64_t workload_data,
                                                uint32_t poll_status)
{
    if (!entry) return -1;
    spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = 24;
    /* Keep the historical bound by default, but allow a controlled Kernel1
     * probe to distinguish a genuine scheduler loop from an artificial host
     * trampoline cutoff.  This is intentionally opt-in: the normal bounded
     * PDI turn must retain its existing cadence until the persistent lane path
     * is verified. */
    ctx.indirect_branch_budget = 5000u;
    {
        const char* budget_text = getenv("GT6_PDI_BRANCH_BUDGET");
        if (budget_text) {
            const unsigned long parsed = strtoul(budget_text, NULL, 10);
            /* Zero has the runtime's documented meaning of unbounded.  It is
             * useful only for the real-Kernel1 persistent-lane experiment;
             * callers keep the 5000-turn default when the variable is absent. */
            if (parsed <= 500000ul)
                ctx.indirect_branch_budget = (uint32_t)parsed;
        }
    }
    ctx.pc = 0x0A00u;
    if (local_store) memcpy(ctx.ls, local_store, SPU_LS_SIZE);

    ctx.gpr[0]._u32[0] = 0x0808u;
    ctx.gpr[1]._u32[0] = 0x3FFB0u;
    ctx.gpr[3]._u32[0] = 0x0100u;
    /* The PDI policy receives CellSpurs' u64 `data` as a doubleword.  GT6's
     * workload records use a 32-bit EA there (high half zero), but the policy
     * reads that EA both after a 4-byte vector shift and directly from the
     * preferred slot when it DMA-loads the worker context.  Preserve a real
     * 64-bit value verbatim; for the title's 32-bit form mirror the EA into
     * the preferred lane as well.  Without this, func 0x26F8 observes zero
     * and skips the very DMA which supplies its PDI worker record. */
    ctx.gpr[4]._u32[0] = (uint32_t)(workload_data >> 32);
    ctx.gpr[4]._u32[1] = (uint32_t)workload_data;
    if (!getenv("GT6_PDI_STRICT_DATA") &&
        (uint32_t)(workload_data >> 32) == 0)
        ctx.gpr[4]._u32[0] = (uint32_t)workload_data;
    /* Diagnostic ABI variant: the raw policy consumes the preferred word for
     * its first DMA, while the hardware dispatcher supplies `arg` as one
     * doubleword.  Keep the normal mirrored form by default; this form lets
     * us verify whether the duplicated low word is contaminating vector math. */
    if (getenv("GT6_PDI_PREFERRED_DATA_ONLY") &&
        (uint32_t)(workload_data >> 32) == 0)
        ctx.gpr[4]._u32[1] = 0;
    ctx.gpr[5]._u32[0] = poll_status;

    { extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
      extern int spu_resume_with_halt(spu_context*);
      extern int spu_system_service_lockwait(spu_context*);
      int halted = spu_run_with_halt(entry, &ctx);
      /* A persistent Kernel1 lane must park system-service work outside the
       * generated C call chain.  Resuming through the dispatcher retains the
       * same context/MFC state but starts with an empty host-call stack, which
       * matches the SPU's independent execution stack.  This mode is entirely
       * opt-in and handles only the real WID-32 idle boundary. */
      if (getenv("GT6_PDI_SYSTEM_SERVICE_LOCKWAIT")) {
          while (halted && ctx.stop_code == 0x7ffeu) {
              if (!spu_system_service_lockwait(&ctx))
                  break;
              ctx.stop_code = 0;
              ctx.status = 0;
              halted = spu_resume_with_halt(&ctx);
          }
      } }
    if (getenv("GT6_PDI_EXITTRACE"))
        fprintf(stderr,
            "[GT6 PDI exit] status=%u pc=%05X r0=%08X r3=%08X r4=%08X r5=%08X "
            "ls1510=%08X ls1520=%08X\n",
            ctx.status, ctx.pc, ctx.gpr[0]._u32[0], ctx.gpr[3]._u32[0],
            ctx.gpr[4]._u32[0], ctx.gpr[5]._u32[0],
            spu_ls_read32(&ctx, 0x1510u), spu_ls_read32(&ctx, 0x1520u));
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);
    return 0;
}

static inline int32_t spu_run_lifted_job(spu_lifted_entry_fn entry,
                                         uint8_t* local_store,
                                         uint32_t args_ea)
{
    return spu_run_lifted_job_img(entry, local_store, args_ea, 0);
}

/* lv2 PPU-fallback wrapper: signature matches spu_ppu_fallback_fn so it can be
 * registered via spu_register_ppu_fallback(entry_point, spu_lifted_fallback, fn).
 * Defined where spu_thread_get_local_store() is available (the lv2 TU); declared
 * here for callers. (uint32_t tid, uint32_t args_ea, uint32_t args_size, void* user) */
int32_t spu_lifted_fallback(uint32_t tid, uint32_t args_ea,
                            uint32_t args_size, void* user);

#endif /* SPU_LIFTED_JOB_H */
