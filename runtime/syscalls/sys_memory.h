/*
 * ps3recomp - Memory management syscalls
 */

#ifndef SYS_MEMORY_H
#define SYS_MEMORY_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../memory/vm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Page sizes */
#define SYS_MEMORY_PAGE_SIZE_1M   0x400
#define SYS_MEMORY_PAGE_SIZE_64K  0x200

/* Maximum tracked allocations */
#define SYS_MEMORY_ALLOC_MAX      1024
#define SYS_MEMORY_CONTAINER_MAX  16
#define SYS_MMAPPER_SHARED_MAX    64
#define SYS_MMAPPER_PF_NOTIFY_MAX 16

/* Allocation record */
typedef struct sys_mem_alloc_info {
    int      active;
    uint32_t addr;
    uint32_t size;
    int32_t  container_id;  /* 0 = main pool */
    uint32_t page_size;     /* 0x100000 (1M) or 0x10000 (64K) from the alloc flags */
} sys_mem_alloc_info;

/* Container */
typedef struct sys_mem_container_info {
    int      active;
    uint32_t total_size;
    uint32_t used_size;
} sys_mem_container_info;

/* Shared memory object */
typedef struct sys_mmapper_shared_info {
    int      active;
    uint32_t size;
    uint32_t addr;   /* mapped address, 0 if not mapped */
    uint64_t key;
} sys_mmapper_shared_info;

/* sys_mmapper_enable_page_fault_notification: a region registered this way
 * expects the FIRST real touch of any page inside it to behave like a real
 * PS3 page fault -- deliver an event to event_queue_id instead of (or as
 * well as) silently becoming readable/writable. See historico_ia.txt
 * 2026-08-30 (RPCS3 issue #2066, filed specifically against GT6, confirms
 * this exact syscall/address range is load-bearing for this title). */
typedef struct sys_mmapper_pf_notify_info {
    int      active;
    uint32_t start_addr;
    uint32_t size;           /* looked up from the matching allocate_address call */
    uint32_t event_queue_id;
} sys_mmapper_pf_notify_info;

extern sys_mem_alloc_info          g_sys_mem_allocs[SYS_MEMORY_ALLOC_MAX];
extern sys_mem_container_info      g_sys_mem_containers[SYS_MEMORY_CONTAINER_MAX];
extern sys_mmapper_shared_info     g_sys_mmapper_shared[SYS_MMAPPER_SHARED_MAX];
extern sys_mmapper_pf_notify_info  g_sys_mmapper_pf_notify[SYS_MMAPPER_PF_NOTIFY_MAX];
extern uint32_t                    g_sys_mem_bump_ptr;  /* bump allocator pointer */

/* Syscall handlers */
int64_t sys_memory_allocate(ppu_context* ctx);
int64_t sys_memory_free(ppu_context* ctx);
int64_t sys_memory_get_user_memory_size(ppu_context* ctx);
int64_t sys_memory_get_page_attribute(ppu_context* ctx);
int64_t sys_memory_container_create(ppu_context* ctx);
int64_t sys_memory_container_destroy(ppu_context* ctx);
int64_t sys_memory_container_get_size(ppu_context* ctx);
int64_t sys_mmapper_allocate_address(ppu_context* ctx);
int64_t sys_mmapper_free_address(ppu_context* ctx);
int64_t sys_mmapper_allocate_shared_memory(ppu_context* ctx);
int64_t sys_mmapper_map_shared_memory(ppu_context* ctx);
int64_t sys_mmapper_enable_page_fault_notification(ppu_context* ctx);

/* Public helper for gt6_posix_segv_handler (main.cpp): does fault_addr fall
 * inside a region with page-fault notification enabled? Returns the
 * event_queue_id to notify, or 0 if this address isn't one of ours. */
uint32_t sys_mmapper_pf_notify_lookup(uint32_t fault_addr);

/* Registration */
void sys_memory_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_MEMORY_H */
