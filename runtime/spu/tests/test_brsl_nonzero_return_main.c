#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdint.h>
#include <stdio.h>

uint8_t* vm_base = 0;
static uint32_t g_out = 0;
static int g_write_count = 0;
static int g_unexpected_branch = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch)
{
    (void)ctx; (void)ch; return spu_zero();
}

uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch)
{
    (void)ctx; (void)ch; return 1;
}

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    (void)ctx;
    if (channel == SPU_WrOutMbox) {
        g_out = value._u32[0];
        ++g_write_count;
    }
}

void spu_indirect_branch(spu_context* ctx)
{
    if (spu_host_call_is_return(ctx, ctx->pc))
        return;
    if ((ctx->pc & SPU_LS_MASK) == 0x08) {
        spu_func_00000008(ctx);
        return;
    }
    g_unexpected_branch = 1;
}

void spu_register_function(uint32_t addr, void (*fn)(spu_context*))
{
    (void)addr; (void)fn;
}

void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void)
{
    spu_context ctx;
    spu_context_init(&ctx, 0);
    spu_func_00000000(&ctx);

    if (g_write_count != 1 || g_out != 142 || g_unexpected_branch ||
        ctx.host_call_depth != 0) {
        printf("FAIL: writes=%d out=%u unexpected=%d depth=%u\n",
               g_write_count, g_out, g_unexpected_branch, ctx.host_call_depth);
        return 1;
    }
    printf("OK: brsl r4 + bi r4 ran continuation once (out=%u)\n", g_out);
    return 0;
}
