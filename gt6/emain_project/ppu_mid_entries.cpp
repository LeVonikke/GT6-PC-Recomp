#include "ppu_recomp.h"

/* Thread-local storage keyword, portable between MSVC and GCC/Clang (mirrors
 * SPU_TLS in runtime/spu/spu_channels.c). ppu_recomp.h keeps its own copy of
 * ppu_context rather than including runtime/ppu/ppu_context.h, so this can't
 * be picked up from there -- defined locally instead. */
#if defined(_MSC_VER)
#  define PPU_TLS __declspec(thread)
#else
#  define PPU_TLS __thread
#endif

/* These three addresses are valid bctr targets in func_009A79D4's callback
 * table.  They are internal basic-block entries, not independent functions,
 * so the original lift did not register them.  On PPU a bctr to one of them
 * continues with the live 009A79D4 stack frame; reproduce those blocks here
 * and restore exactly that frame at the shared epilogue. */
extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" PPU_TLS void (*g_trampoline_fn)(void*);

static inline uint32_t gt6_u32(uint64_t value) {
    return (uint32_t)value;
}

static inline void gt6_set_cmp_signed(ppu_context* ctx, unsigned shift,
                                      int32_t a, int32_t b) {
    const uint32_t cr = (a < b) ? 8u : (a > b) ? 4u : 2u;
    ctx->cr = (ctx->cr & ~(0xFu << shift)) | (cr << shift);
}

static inline void gt6_set_cmp_unsigned(ppu_context* ctx, unsigned shift,
                                        uint32_t a, uint32_t b) {
    const uint32_t cr = (a < b) ? 8u : (a > b) ? 4u : 2u;
    ctx->cr = (ctx->cr & ~(0xFu << shift)) | (cr << shift);
}

static inline void gt6_drain(ppu_context* ctx) {
    while (g_trampoline_fn) {
        void (*next)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        next(ctx);
    }
}

static void gt6_9a7b78_epilogue(ppu_context* ctx) {
    ctx->gpr[3] = gt6_u32(vm_read32(gt6_u32(ctx->gpr[30]) + 0x24u));
    const uint64_t lr = vm_read64(gt6_u32(ctx->gpr[1]) + 0xC0u);
    const uint32_t saved_cr = vm_read32(gt6_u32(ctx->gpr[1]) + 0xB8u);
    ctx->gpr[26] = vm_read64(gt6_u32(ctx->gpr[1]) + 0x80u);
    ctx->lr = lr;
    ctx->gpr[27] = vm_read64(gt6_u32(ctx->gpr[1]) + 0x88u);
    ctx->cr = (ctx->cr & 0xFFFF0FFFu) | (saved_cr & 0x0000F000u);
    ctx->gpr[28] = vm_read64(gt6_u32(ctx->gpr[1]) + 0x90u);
    ctx->gpr[29] = vm_read64(gt6_u32(ctx->gpr[1]) + 0x98u);
    ctx->gpr[30] = vm_read64(gt6_u32(ctx->gpr[1]) + 0xA0u);
    ctx->gpr[31] = vm_read64(gt6_u32(ctx->gpr[1]) + 0xA8u);
    ctx->gpr[1] += 0xB0;
}

static void gt6_9a7a14(ppu_context* ctx) {
    const uint32_t state = gt6_u32(ctx->gpr[30]);
    const uint32_t cursor = gt6_u32(ctx->gpr[31]);
    const uint32_t r0 = vm_read32(cursor + 4u);
    const uint32_t end = vm_read32(cursor + 8u);
    gt6_set_cmp_unsigned(ctx, 28, end - r0 + 3u, 6u);
    if (!((ctx->cr >> 28) & 4u)) {
        gt6_9a7b78_epilogue(ctx);
        return;
    }

    const uint32_t object = vm_read32(state + 0x68u);
    vm_write32(state + 0x24u, 5u);
    ctx->gpr[3] = object;
    uint32_t opd = vm_read32(object) + 0x14u;
    opd = gt6_u32(opd);
    opd = vm_read32(opd);
    ctx->ctr = vm_read32(opd);
    ctx->gpr[2] = vm_read32(opd + 4u);
    ps3_indirect_call(ctx);
    gt6_drain(ctx);
    ctx->gpr[3] = gt6_u32(ctx->gpr[3]) & 0xFFu;
    gt6_set_cmp_signed(ctx, 28, (int32_t)ctx->gpr[3], 0);
    if (!((ctx->cr >> 28) & 2u)) {
        gt6_9a7b78_epilogue(ctx);
        return;
    }

    const uint32_t selector = vm_read32(state + 0x24u);
    const uint32_t table = 0x014D0000u - 27348u;
    ctx->ctr = vm_read32(table + (selector << 2));
    ps3_indirect_call(ctx);
    gt6_drain(ctx);
}

extern "C" void func_009A7A28(ppu_context* ctx) {
    gt6_9a7a14(ctx);
}

extern "C" void func_009A7A84(ppu_context* ctx) {
    ctx->gpr[3] = gt6_u32(ctx->gpr[28]);
    func_009A6140(ctx);
    gt6_drain(ctx);
    if (vm_read32(gt6_u32(ctx->gpr[31]) + 4u) != 0u)
        gt6_9a7a14(ctx);
    else
        gt6_9a7b78_epilogue(ctx);
}

extern "C" void func_009A7A9C(ppu_context* ctx) {
    ctx->gpr[29] = gt6_u32(ctx->gpr[28]);
    for (;;) {
        ctx->gpr[3] = gt6_u32(ctx->gpr[29]);
        func_009A60D4(ctx);
        gt6_drain(ctx);
        gt6_set_cmp_signed(ctx, 12, (int32_t)ctx->gpr[3], 0);
        if ((ctx->cr >> 12) & 2u) {
            ctx->gpr[3] = gt6_u32(ctx->gpr[29]);
            func_009A6140(ctx);
            gt6_drain(ctx);
            const uint32_t r0 = vm_read32(gt6_u32(ctx->gpr[31]) + 4u);
            if (r0 == 0u) {
                gt6_9a7b78_epilogue(ctx);
                return;
            }
            const uint32_t end = vm_read32(gt6_u32(ctx->gpr[31]) + 8u);
            gt6_set_cmp_unsigned(ctx, 28, end - r0 + 3u, 6u);
            if (!((ctx->cr >> 28) & 4u)) {
                gt6_9a7a14(ctx);
                return;
            }
            const uint32_t object = vm_read32(gt6_u32(ctx->gpr[30]) + 0x68u);
            ctx->gpr[29] = gt6_u32(ctx->gpr[28]) + 0x20u;
            ctx->gpr[27] = gt6_u32(ctx->gpr[1]) + 0x70u;
            ctx->gpr[26] = gt6_u32(ctx->gpr[29]);
            ctx->gpr[3] = object;
            ctx->gpr[4] = gt6_u32(ctx->gpr[26]);
            uint32_t opd = gt6_u32(vm_read32(object) + 0x10u);
            opd = vm_read32(opd);
            ctx->ctr = vm_read32(opd);
            ctx->gpr[2] = vm_read32(opd + 4u);
            ps3_indirect_call(ctx);
            gt6_drain(ctx);
            ctx->gpr[3] = gt6_u32(ctx->gpr[27]);
            func_0062880C(ctx);
            gt6_drain(ctx);
            if ((int32_t)ctx->gpr[29] != (int32_t)ctx->gpr[27]) {
                ctx->gpr[4] = vm_read32(gt6_u32(ctx->gpr[1]) + 0x70u);
                ctx->gpr[3] = gt6_u32(ctx->gpr[26]);
                ctx->gpr[4] = gt6_u32(ctx->gpr[4]);
                func_00997798(ctx);
                gt6_drain(ctx);
            }
            ctx->gpr[3] = gt6_u32(ctx->gpr[27]);
            func_0099770C(ctx);
            gt6_drain(ctx);
            gt6_9a7a14(ctx);
            return;
        }

        ctx->gpr[3] = gt6_u32(ctx->gpr[29]);
        func_009A6140(ctx);
        gt6_drain(ctx);
        const uint32_t r0 = vm_read32(gt6_u32(ctx->gpr[31]) + 4u);
        if (r0 == 0u) {
            gt6_9a7b78_epilogue(ctx);
            return;
        }
        const uint32_t end = vm_read32(gt6_u32(ctx->gpr[31]) + 8u);
        gt6_set_cmp_unsigned(ctx, 28, end - r0 + 3u, 6u);
        if ((ctx->cr >> 28) & 4u)
            continue;
        gt6_9a7a14(ctx);
        return;
    }
}
