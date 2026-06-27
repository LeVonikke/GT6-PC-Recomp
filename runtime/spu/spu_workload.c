/* spu_workload.c — SPU workload / task dispatch registry (see spu_workload.h).
 *
 * Maps a registered SPU image (by FNV-1a-64 content fingerprint) to its
 * pre-lifted native entry, loads the image into a 256 KB local store, and runs
 * it with the SPURS task ABI. cellSpurs's AddWorkload/CreateTask call
 * spu_workload_dispatch(); the registry is populated by the title's lifted set.
 */
#include "spu_workload.h"
#include "spu_lifted_job.h"   /* spu_run_lifted_job */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ---- fingerprint ------------------------------------------------------- */

uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL;          /* FNV offset basis */
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;                     /* FNV prime */
    }
    return h;
}

/* ---- registry ---------------------------------------------------------- */

#ifndef SPU_WORKLOAD_MAX
#define SPU_WORKLOAD_MAX 256
#endif

typedef struct {
    uint64_t            fp;
    spu_lifted_entry_fn fn;
    int                 image_id;
    const char*         name;
} spu_workload_entry;

static spu_workload_entry s_registry[SPU_WORKLOAD_MAX];
static unsigned           s_registry_count = 0;

void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name)
{
    if (!fn) return;
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint) {     /* idempotent on fingerprint */
            s_registry[i].fn       = fn;
            s_registry[i].image_id = image_id;
            s_registry[i].name     = name;
            return;
        }
    }
    if (s_registry_count >= SPU_WORKLOAD_MAX) {
        fprintf(stderr, "[spu_workload] registry full (%u); dropping '%s'\n",
                SPU_WORKLOAD_MAX, name ? name : "?");
        return;
    }
    s_registry[s_registry_count].fp       = fingerprint;
    s_registry[s_registry_count].fn       = fn;
    s_registry[s_registry_count].image_id = image_id;
    s_registry[s_registry_count].name     = name;
    s_registry_count++;
}

void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name)
{
    spu_workload_register_img(fingerprint, fn, 0, name);
}

spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint)
            return s_registry[i].fn;
    return NULL;
}

unsigned spu_workload_count(void) { return s_registry_count; }

/* ---- SPU ELF loader (32-bit big-endian) -------------------------------- */

static uint16_t rd_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out)
{
    if (!image || !ls || image_size < 0x34) return 0;

    /* ELF ident: 0x7F 'E' 'L' 'F', ELFCLASS32 (1), ELFDATA2MSB (2). */
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 /*ELFCLASS32*/ || image[5] != 2 /*ELFDATA2MSB*/)
        return 0;

    uint32_t e_entry     = rd_be32(image + 0x18);
    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    if (e_phentsize < 0x20) e_phentsize = 0x20;

    for (uint16_t i = 0; i < e_phnum; i++) {
        size_t po = (size_t)e_phoff + (size_t)i * e_phentsize;
        if (po + 0x20 > image_size) break;
        const uint8_t* ph = image + po;

        uint32_t p_type   = rd_be32(ph + 0x00);
        if (p_type != 1 /*PT_LOAD*/) continue;
        uint32_t p_offset = rd_be32(ph + 0x04);
        uint32_t p_vaddr  = rd_be32(ph + 0x08);
        uint32_t p_filesz = rd_be32(ph + 0x10);
        uint32_t p_memsz  = rd_be32(ph + 0x14);

        /* bounds: segment must fit in local store and in the image */
        if ((uint64_t)p_vaddr + p_memsz > SPU_LS_SIZE)            return 0;
        if ((uint64_t)p_offset + p_filesz > image_size)          return 0;

        if (p_filesz) memcpy(ls + p_vaddr, image + p_offset, p_filesz);
        if (p_memsz > p_filesz)
            memset(ls + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
    }
    if (entry_out) *entry_out = e_entry;
    return 1;
}

size_t spu_elf_image_size(const uint8_t* image, size_t max_avail)
{
    if (!image || max_avail < 0x34) return 0;
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 || image[5] != 2) return 0;     /* ELFCLASS32, ELFDATA2MSB */

    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint32_t e_shoff     = rd_be32(image + 0x20);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    uint16_t e_shentsize = rd_be16(image + 0x2E);
    uint16_t e_shnum     = rd_be16(image + 0x30);

    uint64_t end = (uint64_t)e_shoff + (uint64_t)e_shnum * e_shentsize;

    for (uint16_t k = 0; k < e_phnum; k++) {           /* program headers */
        size_t po = (size_t)e_phoff + (size_t)k * e_phentsize;
        if (po + 0x14 > max_avail) break;
        uint32_t p_offset = rd_be32(image + po + 0x04);
        uint32_t p_filesz = rd_be32(image + po + 0x10);
        uint64_t e = (uint64_t)p_offset + p_filesz;
        if (e > end) end = e;
    }
    for (uint16_t k = 0; k < e_shnum; k++) {           /* section headers */
        size_t so = (size_t)e_shoff + (size_t)k * e_shentsize;
        if (so + 0x18 > max_avail) break;
        uint32_t sh_type   = rd_be32(image + so + 0x04);
        uint32_t sh_offset = rd_be32(image + so + 0x10);
        uint32_t sh_size   = rd_be32(image + so + 0x14);
        if (sh_type != 8 /*SHT_NOBITS*/) {
            uint64_t e = (uint64_t)sh_offset + sh_size;
            if (e > end) end = e;
        }
    }
    if (end > max_avail) end = max_avail;
    return (size_t)end;
}

/* ---- dispatch ---------------------------------------------------------- */

int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr,
            "[spu_workload] dispatch MISS fp=0x%016llX size=%u "
            "(no lifted SPU binary registered for this image)\n",
            (unsigned long long)fp, image_size);
        return 0;
    }

    /* Load the SPU ELF into a fresh local store, then run the lifted entry with
     * the task arg in r3. 256 KB is heap-allocated (too large for the stack,
     * and spu_run_lifted_job already builds a full spu_context on its stack). */
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return 0;

    uint32_t entry = 0;
    if (!spu_elf_load_to_ls(image, image_size, ls, &entry)) {
        fprintf(stderr, "[spu_workload] dispatch fp=0x%016llX: not a valid SPU ELF\n",
                (unsigned long long)fp);
        free(ls);
        return 0;
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT fp=0x%016llX entry=0x%05X args=0x%08X image=%d -> running\n",
        (unsigned long long)fp, entry, args_ea, image_id);

    spu_run_lifted_job_img(fn, ls, args_ea, image_id);

    free(ls);
    return 1;
}

/* Async dispatch: run the SPU job on its OWN host thread so the PPU caller is
 * not blocked. SPURS service/worker tasks are persistent — they loop waiting on
 * PPU-side signals (DMA, event flags, event queues), so running them inline (as
 * spu_workload_dispatch does) deadlocks: the PPU can never deliver the signal
 * the SPU is waiting for because it is stuck inside the dispatch. Real SPUs run
 * concurrently with the PPU; a detached host thread models that. The image bytes
 * and args live in the shared guest arena (vm_base), so they stay valid. */
typedef struct {
    const uint8_t*      image;
    uint32_t            image_size;
    uint32_t            args_ea;
    spu_lifted_entry_fn fn;
    int                 image_id;
    uint32_t            r3[4];        /* captured race-free at dispatch time */
    int                 have_r3;
} spu_async_job;

static void spu_async_run(spu_async_job* j)
{
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (ls) {
        uint32_t entry = 0;
        if (spu_elf_load_to_ls(j->image, j->image_size, ls, &entry)) {
            /* Async dispatch is the SPURS-task path: the entry expects the SPURS
             * task kernel ABI in r3 ({0x40 marker, eaContext, queue EA, ...}),
             * captured at dispatch time (j->r3) so it doesn't race the PPU
             * overwriting the stack-allocated context. */
            int32_t rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                                1, j->have_r3 ? j->r3 : 0);
            fprintf(stderr, "[spu_workload] async image=%d RETURNED rc=%d "
                    "(job ran to completion, did not loop)\n", j->image_id, rc);
        }
        free(ls);
    }
    free(j);
}

#ifdef _WIN32
static DWORD WINAPI spu_async_thread(LPVOID p) { spu_async_run((spu_async_job*)p); return 0; }
#else
static void* spu_async_thread(void* p) { spu_async_run((spu_async_job*)p); return NULL; }
#endif

int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,
                                uint32_t args_ea)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr, "[spu_workload] async dispatch MISS fp=0x%016llX size=%u\n",
                (unsigned long long)fp, image_size);
        return 0;
    }

    spu_async_job* j = (spu_async_job*)malloc(sizeof(*j));
    if (!j) return 0;
    j->image = image; j->image_size = image_size; j->args_ea = args_ea;
    j->fn = fn; j->image_id = image_id;
    /* Capture the SPURS task r3 NOW (PPU thread, synchronous) from the game's
     * descriptor at eaContext+0x10 = {0x40-marker handle, workload EAs}; the
     * async SPU thread reading it later would race the PPU stack. word1 is
     * overridden to args_ea (eaContext) in spu_run_lifted_job_abi. */
    j->have_r3 = 0;
    if (args_ea) {
        extern uint8_t* vm_base;
        const uint8_t* c = vm_base + args_ea + 0x10;
        for (int k = 0; k < 4; k++)
            j->r3[k] = ((uint32_t)c[k*4]<<24)|((uint32_t)c[k*4+1]<<16)|
                       ((uint32_t)c[k*4+2]<<8)|c[k*4+3];
        if ((j->r3[0] >> 16) == 0x40) j->have_r3 = 1;   /* valid marker */
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT (async) fp=0x%016llX args=0x%08X image=%d -> spawning thread\n",
        (unsigned long long)fp, args_ea, image_id);
    if (args_ea) { extern uint8_t* vm_base; const uint8_t* c = vm_base + args_ea;
        static int _d=0; if (_d++ < 1) {
            /* Dump a larger window of the task context buffer + scan for any word
             * that looks like the LS[0xBEC0] target (i.e. a small LS-range value),
             * to see if the kernel-restored LS data lives here (real game data). */
            /* The policy module restores the task context from a save buffer into
             * LS 0xB200; LS[0xBEC0] = ctxbuf + 0xCC0. Check the heap-EA candidates
             * in the descriptor for a save buffer whose +0xCC0/+0xCC8 holds a
             * small LS pointer (a valid P for LS[0xBEC8]). */
            uint32_t cand[5];
            cand[0]=((uint32_t)c[0x14]<<24)|((uint32_t)c[0x15]<<16)|((uint32_t)c[0x16]<<8)|c[0x17];
            cand[1]=((uint32_t)c[0x1C]<<24)|((uint32_t)c[0x1D]<<16)|((uint32_t)c[0x1E]<<8)|c[0x1F];
            cand[2]=((uint32_t)c[0x98]<<24)|((uint32_t)c[0x99]<<16)|((uint32_t)c[0x9A]<<8)|c[0x9B];
            cand[3]=((uint32_t)c[0xB8]<<24)|((uint32_t)c[0xB9]<<16)|((uint32_t)c[0xBA]<<8)|c[0xBB];
            cand[4]=((uint32_t)c[0x16C]<<24)|((uint32_t)c[0x16D]<<16)|((uint32_t)c[0x16E]<<8)|c[0x16F];
            for (int ci=0; ci<5; ci++) {
                uint32_t ea=cand[ci]; if (!ea || ea>=0x10000000) continue;
                const uint8_t* b = vm_base + ea + 0xCC0;
                fprintf(stderr, "[ctxbuf cand 0x%08X +0xCC0]:", ea);
                for (int k=0;k<0x20;k+=4) {
                    uint32_t w=((uint32_t)b[k]<<24)|((uint32_t)b[k+1]<<16)|((uint32_t)b[k+2]<<8)|b[k+3];
                    fprintf(stderr, " %08X%s", w, (w>0&&w<0x40000)?"<LS":"");
                }
                fprintf(stderr, "\n");
            } } }

#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 1u << 20, spu_async_thread, j, 0, NULL);
    if (!th) { free(j); return 0; }
    CloseHandle(th);   /* detached */
#else
    pthread_t th;
    if (pthread_create(&th, NULL, spu_async_thread, j) != 0) { free(j); return 0; }
    pthread_detach(th);
#endif
    return 1;
}
