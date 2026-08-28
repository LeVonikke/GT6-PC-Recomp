/* pdi_oracle_compare.c -- offline, single-threaded, deterministic A/B test.
 *
 * Loads a GT6_PDI_CAPTURE_SNAPSHOT (gt6/spu_policy_runtime.c) and runs the
 * exact same PDI policy job (LS 0xA00, image_id=24) through both the
 * statically lifted entry and the spu_interp.c interpreter, with byte-for-byte
 * identical initial context -- no threads, no real-time scheduling, so any
 * difference in the outcome is a genuine behavioral divergence between the
 * two engines, not run-to-run noise (see historico_ia.txt, 2026-08-25, for
 * why the live-boot A/B was inconclusive).
 *
 * Not part of the normal build; compiled and run manually. See the compile
 * command in historico_ia.txt / the session that added this file.
 */
#include "../../runtime/spu/spu_context.h"
#include "../../runtime/spu/spu_interp.h"
#include "spu_recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void spu_begin_image(int image_id);
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

static uint8_t g_ls_snapshot[SPU_LS_SIZE];
static uint64_t g_workload_data;
static uint32_t g_poll_status;

static int load_snapshot(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 0; }
    char magic[12];
    if (fread(magic, 1, 12, f) != 12 || memcmp(magic, "GT6PDISNAP1", 12) != 0) {
        fprintf(stderr, "bad snapshot magic\n");
        fclose(f);
        return 0;
    }
    if (fread(&g_workload_data, sizeof g_workload_data, 1, f) != 1 ||
        fread(&g_poll_status, sizeof g_poll_status, 1, f) != 1 ||
        fread(g_ls_snapshot, 1, SPU_LS_SIZE, f) != SPU_LS_SIZE) {
        fprintf(stderr, "short read\n");
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

/* Mirrors spu_run_lifted_pdi_policy's exact initial-context setup
 * (runtime/spu/spu_lifted_job.h), minus the getenv() A/B knobs that don't
 * apply to this offline replay. */
static void setup_ctx(spu_context* ctx)
{
    spu_context_init(ctx, 0);
    ctx->image_id = 24;
    ctx->indirect_branch_budget = 5000u;
    ctx->pc = 0x0A00u;
    memcpy(ctx->ls, g_ls_snapshot, SPU_LS_SIZE);
    ctx->gpr[0]._u32[0] = 0x0808u;
    ctx->gpr[1]._u32[0] = 0x3FFB0u;
    ctx->gpr[3]._u32[0] = 0x0100u;
    ctx->gpr[4]._u32[0] = (uint32_t)(g_workload_data >> 32);
    ctx->gpr[4]._u32[1] = (uint32_t)g_workload_data;
    if ((uint32_t)(g_workload_data >> 32) == 0)
        ctx->gpr[4]._u32[0] = (uint32_t)g_workload_data;
    ctx->gpr[5]._u32[0] = g_poll_status;
}

static void dump_regs(const char* tag, const spu_context* ctx)
{
    fprintf(stderr, "  [%s] pc=%05X stop_code=%u r0=%08X r1=%08X r3=%08X r4=%08X r5=%08X\n",
            tag, ctx->pc, ctx->stop_code,
            ctx->gpr[0]._u32[0], ctx->gpr[1]._u32[0], ctx->gpr[3]._u32[0],
            ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0]);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <snapshot.bin>\n", argv[0]);
        return 64;
    }
    if (!load_snapshot(argv[1]))
        return 1;
    fprintf(stderr, "snapshot loaded: workload_data=%016llX poll_status=%08X\n",
            (unsigned long long)g_workload_data, g_poll_status);

    spu_begin_image(24);
    gt6_pdi_policy_spu_recomp_register();

    spu_context lifted_ctx, interp_ctx;
    setup_ctx(&lifted_ctx);
    setup_ctx(&interp_ctx);

    fprintf(stderr, "running LIFTED path...\n");
    int halted = spu_run_with_halt(gt6_pdi_policy_spu_func_00000A00, &lifted_ctx);
    fprintf(stderr, "lifted: halted=%d\n", halted);
    dump_regs("lifted", &lifted_ctx);

    fprintf(stderr, "running INTERPRETED path...\n");
    interp_ctx.stop_code = spu_interp_run(&interp_ctx, 0x0A00u);
    dump_regs("interp", &interp_ctx);

    /* Compare LS content byte-for-byte. */
    int diffs = 0, first = -1;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) {
        if (lifted_ctx.ls[i] != interp_ctx.ls[i]) {
            if (first < 0) first = (int)i;
            diffs++;
        }
    }
    fprintf(stderr, "\nLS diff: %d/%d bytes differ", diffs, SPU_LS_SIZE);
    if (first >= 0) {
        fprintf(stderr, " (first at 0x%05X: lifted=%02X interp=%02X)",
                first, lifted_ctx.ls[first], interp_ctx.ls[first]);
    }
    fprintf(stderr, "\n");

    int reg_diff = memcmp(lifted_ctx.gpr, interp_ctx.gpr, sizeof lifted_ctx.gpr) != 0;
    fprintf(stderr, "GPR diff: %s\n", reg_diff ? "YES" : "no");
    fprintf(stderr, "pc diff: lifted=%05X interp=%05X (%s)\n",
            lifted_ctx.pc, interp_ctx.pc,
            lifted_ctx.pc == interp_ctx.pc ? "match" : "DIFFER");
    fprintf(stderr, "stop_code diff: lifted=%u interp=%u (%s)\n",
            lifted_ctx.stop_code, interp_ctx.stop_code,
            lifted_ctx.stop_code == interp_ctx.stop_code ? "match" : "DIFFER");

    return 0;
}
