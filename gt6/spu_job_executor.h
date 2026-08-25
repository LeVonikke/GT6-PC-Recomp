/*
 * gt6/spu_job_executor.h
 *
 * Bridge between the GT6 JobChain host-side scheduler and the statically
 * lifted SPU job binaries.  Each job binary (raw SPU code) was lifted by
 * spu_lifter.py into a C function; this module:
 *
 *   1. Registers every lifted entry point keyed by its guest binary EA.
 *   2. Parses the 128-byte CellSpursJobDescriptor to find the binary EA,
 *      its byte-size, and the context pointer passed in r3.
 *   3. Allocates an spu_context, copies the raw binary into LS at base 0,
 *      then calls the entry function.
 *   4. Provides gt6_execute_spu_job() for use by the JobChain HLE.
 *
 * CellSpursJobDescriptor layout (confirmed from GT6 logs, 20/07/2026):
 *   +0x00  u32  header/reserved (0)
 *   +0x04  u32  binary EA (big-endian u32, guest address)
 *   +0x08  u16  binary size in bytes (hi16 of word@+0x08, big-endian)
 *   +0x0A  u16  padding (0)
 *   ...
 *   +0x1C  u32  flags (0x04000000=default; 0x04000600=has extra DMA)
 *   +0x40  u32/u32 pairs: DMA list (size<<16|tag_id, ea) 0xFFFFFFFF=end
 *   +0x5C  u32  context EA (passed as r3 preferred-slot to job entry)
 *   +0x64  u32  extra pointer (optional, 0 or 0x3FFFE8D0)
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup to register all four lifted job entry points. */
void gt6_spu_job_executor_init(void);

/*
 * Parse the 128-byte JobDescriptor at guest EA descriptor_ea, resolve the
 * binary EA to a lifted function, create a fresh spu_context and run it.
 * Returns 0 on success, -1 on unknown binary or bad descriptor.
 */
int gt6_execute_spu_job(uint32_t descriptor_ea);

#ifdef __cplusplus
}
#endif
