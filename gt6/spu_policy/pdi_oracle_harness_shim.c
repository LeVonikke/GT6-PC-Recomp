/* pdi_oracle_harness_shim.c -- minimal per-game-harness symbols for
 * pdi_oracle_compare.c, which links only runtime/spu/*.c (via the archive)
 * and gt6/spu_policy /spu_kernel generated code, not the full gt6/main.cpp
 * + runtime/ppu/ppu_loader.cpp harness. Everything here is a deliberately
 * minimal stand-in: a zeroed guest address space (no real PDIPFS/game data
 * behind it) so DMA/vm_* calls the PDI policy makes don't crash, not an
 * attempt to reproduce the live boot's actual guest memory contents. See
 * pdi_oracle_compare.c's header comment for what this tool is actually
 * comparing (lifted vs. interpreted engine, not "matches real gameplay").
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Same size as gt6/main.cpp's kGuestVmSize. */
#define GUEST_VM_SIZE (0x100010000ull)

uint8_t* vm_base = NULL;
uint32_t ppu_vm_size = (uint32_t)GUEST_VM_SIZE;

__attribute__((constructor))
static void init_vm_base(void)
{
    void* p = mmap(NULL, GUEST_VM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap vm_base");
        exit(1);
    }
    vm_base = (uint8_t*)p;
}

uint32_t vm_read32(uint64_t a)
{
    uint32_t v;
    memcpy(&v, vm_base + (uint32_t)a, 4);
    return __builtin_bswap32(v);
}

uint64_t vm_read64(uint64_t a)
{
    uint64_t v;
    memcpy(&v, vm_base + (uint32_t)a, 8);
    return __builtin_bswap64(v);
}

void vm_write32(uint64_t a, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(vm_base + (uint32_t)a, &v, 4);
}

void vm_write64(uint64_t a, uint64_t v)
{
    v = __builtin_bswap64(v);
    memcpy(vm_base + (uint32_t)a, &v, 8);
}

/* Diagnostic dump used by sys_event.c on a blocked receive; a no-op here
 * since there's no lifted PPU context in this SPU-only test. */
void ppu_dump_guest_stack(void* ctx, const char* tag)
{
    (void)ctx;
    (void)tag;
}
