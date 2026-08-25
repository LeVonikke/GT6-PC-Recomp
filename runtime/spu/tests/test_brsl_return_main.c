#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdint.h>

uint8_t* vm_base = 0;
static uint32_t g_out = 0;
static int g_wrote = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    (void)ctx;
    if (channel == SPU_WrOutMbox) { g_out = value._u32[0]; g_wrote = 1; }
}
void spu_indirect_branch(spu_context* ctx) {
    if (spu_host_call_is_return(ctx, ctx->pc))
        return;
    fprintf(stderr, "FAIL: unexpected indirect branch to 0x%X\n", ctx->pc);
}
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void) {
    spu_context ctx;
    spu_context_init(&ctx, 0);
    spu_func_00000000(&ctx);                /* main calls add100 internally */

    const uint32_t kExpected = 42 + 100;
    if (!g_wrote)           { printf("FAIL: no wrch (function never returned to main?)\n"); return 1; }
    if (g_out != kExpected) { printf("FAIL: out = %u (expected %u)\n", g_out, kExpected); return 1; }
    /* r0 was set by brsl; in our lifter brsl emits `gpr[0] = splat(pc+4)`. */
    if (ctx.gpr[0]._u32[0] != 0x8) {
        printf("FAIL: r0 = 0x%X (expected 0x8 -- the address after brsl)\n", ctx.gpr[0]._u32[0]);
        return 1;
    }
    printf("OK: brsl+bi $r0 round-trip ok (out=%u, link=0x%X)\n",
           g_out, ctx.gpr[0]._u32[0]);
    return 0;
}
