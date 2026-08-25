#include "ppu_recomp.h"
#include "cellFs.h"
#include "spu_job_executor.h"

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <array>

extern "C" {
extern uint8_t* vm_base;
void ps3_hle_register_ctx(uint32_t nid, const char* name,
                          void (*fn)(ppu_context*));
extern void (*g_ps3_hle_before_real_import)(uint32_t, ppu_context*);
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size);
int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                               uint64_t data1, uint64_t data2, uint64_t data3);
extern "C" uint32_t g_pdi_workload_data = 0;
void gt6_pdi_policy_start(uint32_t policy_ea, uint32_t policy_size,
                          uint32_t spurs_ea, uint32_t ready_queue,
                          uint64_t workload_data, uint32_t workload_id);
void gt6_pdi_policy_add_scheduler_tick(uint32_t queue);
void ps3_indirect_call(ppu_context* ctx);
int cellSpursAddWorkload(void* spurs, void* wid, const void* pm,
                          u32 size_pm, u64 data, const u8* priority,
                          u32 min_contention, u32 max_contention);
extern void (*cellspurs_workload_observer)(u32 spurs_ea, u32 pm_ea, u32 size_pm,
                                           u32 wid, u64 data);
extern void (*cellspurs_ready_count_observer)(u32 spurs_ea, u32 wid);
}

namespace {

constexpr uint64_t kGuestAddressSpace = 0x100000000ull;
constexpr uint32_t kPdiWorkerCount = 2;

/* The retail SDK keeps SPURS's scheduler objects in guest memory.  We cannot
 * pass their EAs through the generic native-ABI bridge, but retaining the
 * scheduler relation here gives us a real lifecycle: attach -> create -> run
 * -> complete -> join/shutdown.  The guest-facing objects remain deliberately
 * small and opaque; application code must not depend on host pointers. */
struct SpursState {
    uint32_t event_queue = 0;
    uint8_t next_port = 0;
    /* Initial PDI records are a bootstrap only.  GT6 binds another LV2 queue
     * to the same CellSpurs later; that must not restart every policy from
     * its entry point while the first set has already returned to Kernel1. */
    bool pdi_bootstrap_dispatched = false;
};

struct JobChainState {
    uint32_t spurs = 0;
    uint32_t attribute = 0;
    uint32_t event_queue = 0;
    uint32_t entry = 0;
    uint16_t descriptor_size = 0;
    uint16_t max_grabbed_jobs = 0;
    uint32_t generation = 0;
    bool running = false;
    bool completed = false;
};

struct JobChainAttributeState {
    uint32_t jm_revision = 0;
    uint32_t sdk_revision = 0;
    uint32_t entry = 0;
    uint16_t descriptor_size = 0;
    uint16_t max_grabbed_jobs = 0;
    uint32_t priorities = 0;
    uint32_t max_contention = 0;
    uint32_t tag1 = 0;
    uint32_t tag2 = 0;
    uint32_t max_descriptor_size = 0;
    uint32_t initial_spu_count = 0;
    bool auto_spu_count = false;
    bool fixed_memory = false;
};

struct JobGuardState {
    uint32_t chain = 0;
    bool notified = false;
    bool auto_reset = false;
};

struct PdiWorkload {
    uint32_t pm = 0;
    uint32_t size = 0;
    uint32_t wid = 0;
    uint64_t data = 0;
};

std::mutex g_spurs_lock;
/* These tables are tiny, guest-keyed registries.  std::map deliberately avoids
 * unordered_map's dynamically resized bucket vector: a damaged guest command
 * list must not be able to turn a scheduler bookkeeping update into the host
 * CRT's "vector<T> too long" termination. */
std::map<uint32_t, SpursState> g_spurs;
std::map<uint32_t, JobChainAttributeState> g_job_chain_attributes;
std::map<uint32_t, JobChainState> g_job_chains;
std::map<uint32_t, JobGuardState> g_job_guards;
std::map<uint32_t, bool> g_skipped_job_log;
uint32_t g_last_spurs_event_queue = 0;
uint32_t g_pending_pdi_spurs = 0;
/* The policy ABI has a 16-bit workload id and GT6 only publishes ids 0..15.
 * Keep the bootstrap records in fixed storage: guest-driven scheduling must
 * never be able to grow a host std::vector with corrupted metadata. */
std::array<PdiWorkload, 16> g_pending_pdi_workloads{};
uint32_t g_pending_pdi_workload_count = 0;

bool skip_spurs_jobs()
{
    static const bool enabled = [] {
        const char* value = std::getenv("GT6_SPURS_SKIP_JOBS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

bool guest_span(uint64_t address, size_t size)
{
    return vm_base && address < kGuestAddressSpace &&
           size <= kGuestAddressSpace - address;
}

bool guest_cstr(uint64_t address, char* output, size_t output_size)
{
    if (!output || output_size == 0 || !guest_span(address, 1))
        return false;

    for (size_t i = 0; i < output_size; ++i) {
        if (!guest_span(address + i, 1))
            return false;
        output[i] = static_cast<char>(vm_base[static_cast<uint32_t>(address + i)]);
        if (output[i] == '\0')
            return true;
    }
    output[output_size - 1] = '\0';
    return false;
}

bool guest_zero(uint64_t address, size_t size)
{
    if (!guest_span(address, size))
        return false;
    std::memset(vm_base + static_cast<uint32_t>(address), 0, size);
    return true;
}

uint32_t guest_read_be32(uint64_t address)
{
    const auto ea = static_cast<uint32_t>(address);
    return (static_cast<uint32_t>(vm_base[ea]) << 24) |
           (static_cast<uint32_t>(vm_base[ea + 1]) << 16) |
           (static_cast<uint32_t>(vm_base[ea + 2]) << 8) |
           static_cast<uint32_t>(vm_base[ea + 3]);
}

void guest_write_be32(uint64_t address, uint32_t value)
{
    const auto ea = static_cast<uint32_t>(address);
    vm_base[ea]     = static_cast<uint8_t>(value >> 24);
    vm_base[ea + 1] = static_cast<uint8_t>(value >> 16);
    vm_base[ea + 2] = static_cast<uint8_t>(value >> 8);
    vm_base[ea + 3] = static_cast<uint8_t>(value);
}

void set_result(ppu_context* ctx, s32 result)
{
    ctx->gpr[3] = static_cast<uint64_t>(static_cast<int64_t>(result));
}

template <typename T>
bool guest_write(uint64_t address, const T& value)
{
    if (!guest_span(address, sizeof(value)))
        return false;
    std::memcpy(vm_base + static_cast<uint32_t>(address), &value, sizeof(value));
    return true;
}

void hle_cell_fs_open(ppu_context* ctx)
{
    char path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (!guest_cstr(ctx->gpr[3], path, sizeof(path))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    CellFsFd fd = -1;
    const s32 result = cellFsOpen(path, static_cast<s32>(ctx->gpr[4]), &fd,
                                  nullptr, ctx->gpr[6]);
    if (std::getenv("GT6_FS_TRACE")) {
        static unsigned trace_count = 0;
        if (trace_count++ < 128) {
            std::fprintf(stderr,
                         "[GT6 FS] open path=%s flags=%08X result=%08X fd=%d out=%08X\n",
                         path, static_cast<u32>(ctx->gpr[4]),
                         static_cast<u32>(result), static_cast<int>(fd),
                         static_cast<u32>(ctx->gpr[5]));
        }
    }
    if (result == CELL_OK && !guest_write(ctx->gpr[5], fd)) {
        cellFsClose(static_cast<CellFsFd>(__builtin_bswap32(static_cast<uint32_t>(fd))));
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

void hle_cell_fs_close(ppu_context* ctx)
{
    set_result(ctx, cellFsClose(static_cast<CellFsFd>(ctx->gpr[3])));
}

void hle_cell_fs_read(ppu_context* ctx)
{
    const uint64_t size = ctx->gpr[5];
    if (!guest_span(ctx->gpr[4], static_cast<size_t>(size))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    u64 read = 0;
    const s32 result = cellFsRead(static_cast<CellFsFd>(ctx->gpr[3]),
                                  vm_base + static_cast<uint32_t>(ctx->gpr[4]),
                                  size, &read);
    if (result == CELL_OK && !guest_write(ctx->gpr[6], read)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

void hle_cell_fs_write(ppu_context* ctx)
{
    const uint64_t size = ctx->gpr[5];
    if (!guest_span(ctx->gpr[4], static_cast<size_t>(size))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    u64 written = 0;
    const s32 result = cellFsWrite(static_cast<CellFsFd>(ctx->gpr[3]),
                                   vm_base + static_cast<uint32_t>(ctx->gpr[4]),
                                   size, &written);
    if (result == CELL_OK && !guest_write(ctx->gpr[6], written)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

void hle_cell_fs_lseek(ppu_context* ctx)
{
    u64 position = 0;
    const s32 result = cellFsLseek(static_cast<CellFsFd>(ctx->gpr[3]),
                                   static_cast<s64>(ctx->gpr[4]),
                                   static_cast<s32>(ctx->gpr[5]), &position);
    if (result == CELL_OK && !guest_write(ctx->gpr[6], position)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

/* cellFsReadWithOffset(fd, offset, buf, size, nread).  GT.VOL uses this
 * positional form heavily; emulate it through the existing descriptor layer
 * while restoring the sequential file position visible to other callers. */
void hle_cell_fs_read_with_offset(ppu_context* ctx)
{
    const auto fd = static_cast<CellFsFd>(ctx->gpr[3]);
    const auto offset = static_cast<s64>(ctx->gpr[4]);
    const uint64_t size = ctx->gpr[6];
    if (!guest_span(ctx->gpr[5], static_cast<size_t>(size))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    u64 saved_position = 0;
    u64 ignored_position = 0;
    u64 read = 0;
    s32 result = cellFsLseek(fd, 0, CELL_FS_SEEK_CUR, &saved_position);
    if (result == CELL_OK)
        result = cellFsLseek(fd, offset, CELL_FS_SEEK_SET, &ignored_position);
    if (result == CELL_OK)
        result = cellFsRead(fd, vm_base + static_cast<uint32_t>(ctx->gpr[5]),
                            size, &read);
    /* cellFsLseek returns its output in guest byte order because generic HLE
     * callers write it straight back to PS3 memory.  Convert it before using
     * it as the native offset for the restoration seek. */
    const s64 host_saved_position = static_cast<s64>(__builtin_bswap64(saved_position));
    const s32 restore_result = cellFsLseek(fd, host_saved_position,
                                           CELL_FS_SEEK_SET, &ignored_position);
    if (result == CELL_OK && restore_result != CELL_OK)
        result = restore_result;
    if (result == CELL_OK && !guest_write(ctx->gpr[7], read)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

void hle_cell_fs_stat(ppu_context* ctx)
{
    char path[CELL_FS_MAX_FS_PATH_LENGTH];
    CellFsStat stat{};
    if (!guest_cstr(ctx->gpr[3], path, sizeof(path))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    const s32 result = cellFsStat(path, &stat);
    if (result == CELL_OK && !guest_write(ctx->gpr[4], stat)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

void hle_cell_fs_fstat(ppu_context* ctx)
{
    CellFsStat stat{};
    const s32 result = cellFsFstat(static_cast<CellFsFd>(ctx->gpr[3]), &stat);
    if (result == CELL_OK && !guest_write(ctx->gpr[4], stat)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

void hle_cell_fs_opendir(ppu_context* ctx)
{
    char path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (!guest_cstr(ctx->gpr[3], path, sizeof(path))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    CellFsDir dir = -1;
    const s32 result = cellFsOpendir(path, &dir);
    if (result == CELL_OK && !guest_write(ctx->gpr[4], dir)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

void hle_cell_fs_readdir(ppu_context* ctx)
{
    CellFsDirectoryEntry entry{};
    u64 count = 0;
    const s32 result = cellFsReaddir(static_cast<CellFsDir>(ctx->gpr[3]),
                                     &entry, &count);
    if (result == CELL_OK &&
        (!guest_write(ctx->gpr[4], entry) || !guest_write(ctx->gpr[5], count))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, result);
}

void hle_cell_fs_closedir(ppu_context* ctx)
{
    set_result(ctx, cellFsClosedir(static_cast<CellFsDir>(ctx->gpr[3])));
}

void hle_cell_fs_unlink(ppu_context* ctx)
{
    char path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (!guest_cstr(ctx->gpr[3], path, sizeof(path))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    /* Startup removes stale cache markers.  A successful no-op is deliberate:
     * never let a smoke test delete files from the user's RPCS3 storage. */
    set_result(ctx, CELL_OK);
}

/* The GT6 EMAIN links against the newer four-argument GCM mapping export.
 * The extra flags only describe the mapping policy; the existing core already
 * maintains the EA<->IO table, so its three-argument implementation is the
 * correct state-changing operation here. */
void hle_cell_gcm_map_ea_io_with_flags(ppu_context* ctx)
{
    set_result(ctx, cellGcmMapEaIoAddress(static_cast<u32>(ctx->gpr[3]),
                                          static_cast<u32>(ctx->gpr[4]),
                                          static_cast<u32>(ctx->gpr[5])));
}

/* The generic NID table contains a historical duplicate for this export, and
 * its raw C handler receives the PPU EA as a host pointer.  Keep the mutex
 * state in its big-endian guest structure (+0 owner, +0x0c recursion count). */
void hle_sys_lwmutex_unlock(ppu_context* ctx)
{
    const u32 mutex = static_cast<u32>(ctx->gpr[3]);
    if (!guest_span(mutex, 0x10)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    const u32 recursion = guest_read_be32(mutex + 0x0c);
    if (recursion != 0)
        guest_write_be32(mutex + 0x0c, recursion - 1);
    if (recursion <= 1)
        guest_write_be32(mutex, 0);
    set_result(ctx, CELL_OK);
}

void hle_cell_syscache_mount(ppu_context* ctx)
{
    constexpr char kCachePath[] = "/dev_hdd1/caches";
    const u32 path = static_cast<u32>(ctx->gpr[3]);
    if (!path || !guest_span(path, sizeof(kCachePath))) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    std::memcpy(vm_base + path, kCachePath, sizeof(kCachePath));
    set_result(ctx, CELL_OK);
}

void hle_cell_audio_out_get_device_info(ppu_context* ctx)
{
    /* CellAudioOutDeviceInfo is six header bytes plus 16 four-byte modes.
     * It contains only byte fields, so it can be populated directly in guest
     * order without passing a guest EA to the native cellAvconfExt routine. */
    const u32 info = static_cast<u32>(ctx->gpr[5]);
    if (!info || !guest_zero(info, 70)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    vm_base[info + 0] = 0; /* HDMI */
    vm_base[info + 1] = 1; /* one available mode */
    vm_base[info + 2] = 2; /* connected */
    vm_base[info + 6] = 0; /* LPCM */
    vm_base[info + 7] = 2; /* stereo */
    vm_base[info + 8] = 4; /* 48 kHz */
    set_result(ctx, CELL_OK);
}

/* These SPURS calls are SDK helpers around structures stored in PS3 memory.
 * They cannot go through the generic native-ABI bridge because their pointer
 * arguments are guest EAs.  The fields below match CellSpursWorkloadAttribute
 * (revision, SDK version, PM EA/size, data, priorities, contention). */
void hle_cell_spurs_workload_attribute_initialize(ppu_context* ctx)
{
    const u32 attr = static_cast<u32>(ctx->gpr[3]);
    if (!guest_zero(attr, 0xB0)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    const u32 revision = static_cast<u32>(ctx->gpr[4]);
    const u32 sdk_version = static_cast<u32>(ctx->gpr[5]);
    const u64 pm = ctx->gpr[6];
    const u32 pm_size = static_cast<u32>(ctx->gpr[7]);
    const u64 data = ctx->gpr[8];
    const u32 priority = static_cast<u32>(ctx->gpr[9]);
    const u32 min_contention = static_cast<u32>(ctx->gpr[10]);
    /* PPC64 passes the ninth parameter after r3..r10 in the caller save
     * area.  Losing this value left every guest wklMaxContention entry zero;
     * a real Kernel1 selector must then reject every otherwise-runnable
     * workload before it can make its first dispatch. */
    const u32 max_contention = static_cast<u32>(
        vm_read64(ctx->gpr[1] + 0x70));

    auto put32 = [](u32 ea, u32 value) {
        vm_base[ea + 0] = static_cast<uint8_t>(value >> 24);
        vm_base[ea + 1] = static_cast<uint8_t>(value >> 16);
        vm_base[ea + 2] = static_cast<uint8_t>(value >> 8);
        vm_base[ea + 3] = static_cast<uint8_t>(value);
    };
    auto put64 = [&put32](u32 ea, u64 value) {
        put32(ea, static_cast<u32>(value >> 32));
        put32(ea + 4, static_cast<u32>(value));
    };

    put32(attr + 0x00, revision);
    put32(attr + 0x04, sdk_version);
    put64(attr + 0x08, pm);
    put32(attr + 0x10, pm_size);
    put64(attr + 0x18, data);
    if (guest_span(priority, 8))
        std::memcpy(vm_base + attr + 0x20, vm_base + priority, 8);
    put32(attr + 0x28, min_contention);
    put32(attr + 0x2C, max_contention);
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_get_info(ppu_context* ctx)
{
    /* CellSpursGetInfo writes a diagnostic snapshot to r4.  GT6 only needs a
     * valid, zero-initialized snapshot during startup before workloads run. */
    if (ctx->gpr[4] && !guest_zero(ctx->gpr[4], 0x40)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_request_idle_spu(ppu_context* ctx)
{
    (void)ctx;
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_workload_attribute_set_name(ppu_context* ctx)
{
    /* Workload names are diagnostic metadata; no scheduler state changes. */
    (void)ctx;
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_set_exception_event_handler(ppu_context* ctx)
{
    /* Keep the handler registration non-failing.  No SPU exception has been
     * delivered by this runner yet, so invoking a guest callback would be wrong. */
    (void)ctx;
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_initialize_with_attribute2(ppu_context* ctx)
{
    const u32 spurs = static_cast<u32>(ctx->gpr[3]);
    const u32 attribute = static_cast<u32>(ctx->gpr[4]);

    /* CellSpursAttribute is a 0x200-byte BE guest object.  NID 0x95180230
     * receives revision in r4 and writes the actual nSpus at +0x08. */
    u32 n_spus = 0;
    if (attribute && guest_span(attribute, 0x0cu))
        n_spus = guest_read_be32(attribute + 0x08u);

    /* CellSpurs is a 0x2000-byte, 128-byte-aligned BE scheduler object.  The
     * former 0x140-byte compatibility stub left its workload tables and
     * control line uninitialised.  PDI's raw policy DMA-loads those exact
     * fields, then attempts GETLLAR/PUTLLC against the resulting context. */
    if (!spurs || (spurs & 0x7f) != 0 || !n_spus ||
        !guest_zero(spurs, 0x2000)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    /* Populate the fixed core fields in their documented guest-endian
     * locations (RPCS3's CellSpurs layout): flags1/nSpus/flag receiver, then
     * the revision area.  Workload descriptors are filled by AddWorkload. */
    vm_base[spurs + 0x74] = 0x40;    /* SPURS2 / 32-workload layout */
    vm_base[spurs + 0x76] = static_cast<uint8_t>(n_spus);
    vm_base[spurs + 0x77] = 0xff;    /* no workload-flag receiver yet */
    guest_write_be32(spurs + 0xB0, 0x0000FFFFu); /* SPURS1 enabled sentinel */
    guest_write_be32(spurs + 0xDA0, 1);
    guest_write_be32(spurs + 0xDA4, attribute);
    std::fprintf(stderr,
                 "[GT6 SPURS] initialize spurs=%08X attribute=%08X nSpus=%u\n",
                 spurs, attribute, n_spus);
    std::lock_guard<std::mutex> lock(g_spurs_lock);
    g_spurs[spurs] = {};
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_attach_lv2_event_queue(ppu_context* ctx)
{
    const u32 spurs = static_cast<u32>(ctx->gpr[3]);
    const u32 queue = static_cast<u32>(ctx->gpr[4]);
    const u32 port = static_cast<u32>(ctx->gpr[5]);
    if (!spurs || !queue || !port || !guest_span(port, 1)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    std::array<PdiWorkload, 16> start_pdi_workloads{};
    uint32_t start_pdi_workload_count = 0;
    uint8_t assigned_port = 0;
    bool is_pdi_scheduler = false;
    {
        std::lock_guard<std::mutex> lock(g_spurs_lock);
        auto& state = g_spurs[spurs];
        state.event_queue = queue;
        g_last_spurs_event_queue = queue;
        assigned_port = state.next_port++;
        vm_base[port] = assigned_port;
        /* PDI's policy reads the attached LV2 endpoint directly from the
         * CellSpurs object, not from this host-side lifecycle map. */
        guest_write_be32(spurs + 0xD5C, queue);
        guest_write_be32(spurs + 0xD60, assigned_port);
        is_pdi_scheduler = g_pending_pdi_spurs == spurs;
        if (g_pending_pdi_spurs == spurs && !state.pdi_bootstrap_dispatched) {
            start_pdi_workloads = g_pending_pdi_workloads;
            start_pdi_workload_count = g_pending_pdi_workload_count;
            state.pdi_bootstrap_dispatched = true;
        }
    }
    std::fprintf(stderr, "[GT6 SPURS] spurs=%08X attached queue=%u port=%u\n",
                 spurs, queue, assigned_port);
    if (start_pdi_workload_count != 0) {
        std::fprintf(stderr,
                     "[GT6 SPURS] PDI scheduler ready; launching %u policy workers on queue=%u\n",
                     start_pdi_workload_count, queue);
        for (uint32_t i = 0; i < start_pdi_workload_count; ++i) {
            const auto& workload = start_pdi_workloads[i];
            /* Diagnostic topology mode: the physical PDI scheduler has two
             * SPU lanes.  Let its own Kernel1 handoff select the remaining
             * workloads instead of speculatively launching five host policy
             * instances against the same guest scheduler state. */
            if (std::getenv("GT6_PDI_TWO_LANE_BOOT") &&
                workload.wid >= kPdiWorkerCount) {
                std::fprintf(stderr,
                             "[GT6 SPURS] PDI two-lane boot defers wid=%u\n",
                             workload.wid);
                continue;
            }
            gt6_pdi_policy_start(workload.pm, workload.size, spurs, queue,
                                 workload.data, workload.wid);
        }
    }
    /* A real SPU raises the attached LV2 port while its policy is runnable.
     * PDI reattaches its scheduler from the bootstrap queue to q=6 after the
     * initial resources are loaded; keep that PPU service loop supplied on
     * each endpoint rather than leaving the second receiver to time out. */
    if (is_pdi_scheduler)
        gt6_pdi_policy_add_scheduler_tick(queue);
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_job_chain_attribute_initialize(ppu_context* ctx)
{
    /* Firmware ABI (14 arguments):
     *   r3=jmRevision, r4=sdkRevision, r5=attribute, r6=entry,
     *   r7=descriptorSize, r8=maxGrabbedJobs, r9=priorities,
     *   r10=maxContention; the remaining five values start at sp+0x70.
     * The old bridge treated r3 as the output pointer, silently left the real
     * attribute uninitialised, and discarded the command-list address. */
    const u32 jm_revision = static_cast<u32>(ctx->gpr[3]);
    const u32 sdk_revision = static_cast<u32>(ctx->gpr[4]);
    const u32 attribute = static_cast<u32>(ctx->gpr[5]);
    const u32 entry = static_cast<u32>(ctx->gpr[6]);
    const u16 descriptor_size = static_cast<u16>(ctx->gpr[7]);
    const u16 max_grabbed_jobs = static_cast<u16>(ctx->gpr[8]);
    const u32 priorities = static_cast<u32>(ctx->gpr[9]);
    const u32 max_contention = static_cast<u32>(ctx->gpr[10]);
    const u32 auto_spu_count = static_cast<u32>(vm_read64(ctx->gpr[1] + 0x70));
    const u32 tag1 = static_cast<u32>(vm_read64(ctx->gpr[1] + 0x78));
    const u32 tag2 = static_cast<u32>(vm_read64(ctx->gpr[1] + 0x80));
    const u32 fixed_memory = static_cast<u32>(vm_read64(ctx->gpr[1] + 0x88));
    const u32 max_descriptor_size = static_cast<u32>(vm_read64(ctx->gpr[1] + 0x90));
    const u32 initial_spu_count = static_cast<u32>(vm_read64(ctx->gpr[1] + 0x98));

    if (!attribute || !entry || !priorities ||
        !guest_span(priorities, 8) || !guest_zero(attribute, 0x40)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    vm_write32(attribute + 0x00, jm_revision);
    vm_write32(attribute + 0x04, sdk_revision);
    vm_write32(attribute + 0x08, entry);
    vm_write16(attribute + 0x0c, descriptor_size);
    vm_write16(attribute + 0x0e, max_grabbed_jobs);
    std::memcpy(vm_base + attribute + 0x10, vm_base + priorities, 8);
    vm_write32(attribute + 0x18, max_contention);
    vm_write8(attribute + 0x1c, auto_spu_count != 0);
    vm_write32(attribute + 0x20, tag1);
    vm_write32(attribute + 0x24, tag2);
    vm_write8(attribute + 0x28, fixed_memory != 0);
    vm_write32(attribute + 0x2c, max_descriptor_size);
    vm_write32(attribute + 0x30, initial_spu_count);

    JobChainAttributeState state{};
    state.jm_revision = jm_revision;
    state.sdk_revision = sdk_revision;
    state.entry = entry;
    state.descriptor_size = descriptor_size;
    state.max_grabbed_jobs = max_grabbed_jobs;
    state.priorities = priorities;
    state.max_contention = max_contention;
    state.tag1 = tag1;
    state.tag2 = tag2;
    state.max_descriptor_size = max_descriptor_size;
    state.initial_spu_count = initial_spu_count;
    state.auto_spu_count = auto_spu_count != 0;
    state.fixed_memory = fixed_memory != 0;
    {
        std::lock_guard<std::mutex> lock(g_spurs_lock);
        g_job_chain_attributes[attribute] = state;
    }

    if (std::getenv("GT6_JOBCHAIN_TRACE")) {
        static uint32_t trace_count = 0;
        if (trace_count++ < 64) {
            std::fprintf(stderr,
                         "[GT6 JOB] attr=%08X rev=%u sdk=%08X entry=%08X "
                         "size=%u grab=%u contention=%u auto=%u tags=%u/%u "
                         "fixed=%u max=%u initial=%u commands:",
                         attribute, jm_revision, sdk_revision, entry,
                         descriptor_size, max_grabbed_jobs, max_contention,
                         auto_spu_count != 0, tag1, tag2, fixed_memory != 0,
                         max_descriptor_size, initial_spu_count);
            for (u32 i = 0; i < 12 && guest_span(entry + i * 8, 8); ++i)
                std::fprintf(stderr, " %016llX",
                             static_cast<unsigned long long>(vm_read64(entry + i * 8)));
            std::fputc('\n', stderr);

            /* A command whose low three bits are zero is a job-descriptor
             * pointer (zero itself is NOP).  Print the complete maximum-sized
             * descriptor once per traced attribute so we can identify its
             * job binary and DMA list without guessing the private layout. */
            for (u32 i = 0; i < 12 && guest_span(entry + i * 8, 8); ++i) {
                const u64 command = vm_read64(entry + i * 8);
                const u32 descriptor = static_cast<u32>(command);
                if (descriptor >= 0x10000u && (command & 7u) == 0 &&
                    guest_span(descriptor, max_descriptor_size)) {
                    std::fprintf(stderr, "[GT6 JOB] descriptor=%08X words:", descriptor);
                    const u32 words = std::min<u32>(max_descriptor_size / 4, 64);
                    for (u32 word = 0; word < words; ++word)
                        std::fprintf(stderr, " %08X", vm_read32(descriptor + word * 4));
                    std::fputc('\n', stderr);
                }
            }
        }
    }
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_job_chain_attribute_set_name(ppu_context* ctx)
{
    const u32 attribute = static_cast<u32>(ctx->gpr[3]);
    if (!attribute || !guest_span(attribute + 0x38, 4)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    vm_write32(attribute + 0x38, static_cast<u32>(ctx->gpr[4]));
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_create_job_chain_with_attribute(ppu_context* ctx)
{
    const u32 spurs = static_cast<u32>(ctx->gpr[3]);
    const u32 chain = static_cast<u32>(ctx->gpr[4]);
    const u32 attribute = static_cast<u32>(ctx->gpr[5]);
    if (!spurs || !chain || !attribute || !guest_zero(chain, 0x110)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }

    std::lock_guard<std::mutex> lock(g_spurs_lock);
    const auto scheduler = g_spurs.find(spurs);
    const uint32_t queue = scheduler == g_spurs.end()
        ? g_last_spurs_event_queue : scheduler->second.event_queue;
    JobChainAttributeState attr{};
    const auto attr_it = g_job_chain_attributes.find(attribute);
    if (attr_it != g_job_chain_attributes.end())
        attr = attr_it->second;
    else if (guest_span(attribute, 0x40)) {
        attr.jm_revision = vm_read32(attribute + 0x00);
        attr.sdk_revision = vm_read32(attribute + 0x04);
        attr.entry = vm_read32(attribute + 0x08);
        attr.descriptor_size = vm_read16(attribute + 0x0c);
        attr.max_grabbed_jobs = vm_read16(attribute + 0x0e);
        attr.max_contention = vm_read32(attribute + 0x18);
        attr.auto_spu_count = vm_read8(attribute + 0x1c) != 0;
        attr.tag1 = vm_read32(attribute + 0x20);
        attr.tag2 = vm_read32(attribute + 0x24);
        attr.fixed_memory = vm_read8(attribute + 0x28) != 0;
        attr.max_descriptor_size = vm_read32(attribute + 0x2c);
        attr.initial_spu_count = vm_read32(attribute + 0x30);
    }

    /* Preserve the guest-visible fields used by libspurs callers. */
    vm_write32(chain + 0x00, attr.entry);
    vm_write8(chain + 0x23, 0);
    vm_write8(chain + 0x24, attr.auto_spu_count);
    vm_write8(chain + 0x28, static_cast<u8>(attr.initial_spu_count));
    vm_write8(chain + 0x2a, static_cast<u8>(attr.tag1));
    vm_write8(chain + 0x2b, static_cast<u8>(attr.tag2));
    const u8 size_class = attr.max_descriptor_size >= 0x100
        ? static_cast<u8>(((attr.max_descriptor_size - 0x100) / 128) & 7) : 0;
    vm_write8(chain + 0x2c,
              static_cast<u8>((attr.fixed_memory ? 0x80 : 0) | (size_class << 4)));
    vm_write8(chain + 0x2d, static_cast<u8>(attr.jm_revision));
    vm_write16(chain + 0x70, attr.max_grabbed_jobs);
    vm_write16(chain + 0x72, attr.descriptor_size);
    vm_write64(chain + 0x78, spurs);
    vm_write32(chain + 0x90, attr.sdk_revision);

    JobChainState state{};
    state.spurs = spurs;
    state.attribute = attribute;
    state.event_queue = queue;
    state.entry = attr.entry;
    state.descriptor_size = attr.descriptor_size;
    state.max_grabbed_jobs = attr.max_grabbed_jobs;
    g_job_chains[chain] = state;
    std::fprintf(stderr,
                 "[GT6 SPURS] create chain=%08X spurs=%08X queue=%u "
                 "attribute=%08X entry=%08X size=%u\n",
                 chain, spurs, queue, attribute, attr.entry,
                 attr.descriptor_size);
    set_result(ctx, CELL_OK);
}

/* The PDI loader installs the policy image through CellSpurs' ordinary
 * workload API before it starts the short bootstrap JobChain.  Preserve the
 * generic implementation (it owns the workload table and guest WID), then
 * launch the lifted image using that same scheduler EA. */
void gt6_observe_spurs_workload(u32 spurs, u32 pm, u32 size_pm, u32 wid, u64 data)
{
    if (pm != 0x0144C400u || size_pm != 0x2F80u)
        return;

    uint32_t queue = 0;
    bool start_now = false;
    {
        std::lock_guard<std::mutex> lock(g_spurs_lock);
        g_pending_pdi_spurs = spurs;
        /* The raw kernel schedules from every runnable initial workload, not
         * merely one entry per physical SPU.  The old bootstrap kept only 0/1,
         * so 2..4 could never receive even their first policy dispatch after
         * a kernel return.  Retain each pre-attach policy record; the lifted
         * policy itself still derives its logical SPU from wid % 2. */
        if (wid < g_pending_pdi_workloads.size()) {
            bool already_recorded = false;
            for (uint32_t i = 0; i < g_pending_pdi_workload_count; ++i)
                already_recorded |= g_pending_pdi_workloads[i].wid == wid;
            if (!already_recorded &&
                g_pending_pdi_workload_count < g_pending_pdi_workloads.size()) {
                g_pending_pdi_workloads[g_pending_pdi_workload_count++] =
                    {pm, size_pm, wid, data};
            }
        }
        const auto it = g_spurs.find(spurs);
        if (it != g_spurs.end()) {
            queue = it->second.event_queue;
            start_now = queue != 0;
        }
    }
    std::fprintf(stderr,
                 "[GT6 SPURS] PDI policy workload: spurs=%08X queue=%u wid=%u data=%08X%08X\n",
                 spurs, queue, wid, static_cast<u32>(data >> 32),
                 static_cast<u32>(data));
    /* PDI registers its policy before attaching its LV2 service queue.  Wait
     * for that attach so the raw policy reads a complete scheduler object. */
    if (start_now && wid < kPdiWorkerCount) {
        gt6_pdi_policy_start(pm, size_pm, spurs, queue, data, wid);
    } else if (start_now) {
        std::fprintf(stderr,
                     "[GT6 SPURS] PDI workload %u published; launching its policy cycle\n",
                     wid);
        gt6_pdi_policy_start(pm, size_pm, spurs, queue, data, wid);
    }
}

/* ReadyCount is the PPU-to-kernel scheduling edge.  The generic CellSpurs HLE
 * mirrors it in guest memory, but a raw, statically lifted policy has no
 * kernel thread watching that byte.  Re-enter only the workload explicitly
 * made ready; `gt6_pdi_policy_start` coalesces an already-running cycle. */
void gt6_observe_spurs_ready_count(u32 spurs, u32 wid)
{
    PdiWorkload workload{};
    uint32_t queue = 0;
    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(g_spurs_lock);
        if (g_pending_pdi_spurs != spurs || wid >= 16u ||
            !guest_span(spurs + wid, 1) || vm_base[spurs + wid] == 0)
            return;
        const auto scheduler = g_spurs.find(spurs);
        if (scheduler == g_spurs.end() || scheduler->second.event_queue == 0)
            return;
        uint32_t workload_index = g_pending_pdi_workload_count;
        for (uint32_t i = 0; i < g_pending_pdi_workload_count; ++i)
            if (g_pending_pdi_workloads[i].wid == wid) {
                workload_index = i;
                break;
            }
        if (workload_index == g_pending_pdi_workload_count)
            return;
        workload = g_pending_pdi_workloads[workload_index];
        queue = scheduler->second.event_queue;
        dispatch = true;
    }
    if (dispatch) {
        std::fprintf(stderr,
                     "[GT6 SPURS] PDI ready wid=%u queue=%u; dispatching policy cycle\n",
                     wid, queue);
        gt6_pdi_policy_start(workload.pm, workload.size, spurs, queue,
                             workload.data, workload.wid);
        /* A ReadyCount is a real PPU->SPU scheduling edge.  The attached
         * service receiver may already be asleep after the bootstrap edge;
         * hardware raises the same scheduler-service notification when this
         * newly-ready workload is made visible.  Keep the bridge opt-in while
         * validating the exact lifecycle: one queue wake per observed guest
         * ReadyCount, never a timer and never a fabricated completion. */
        if (std::getenv("GT6_PDI_READY_QUEUE_EDGE")) {
            const int pushed = sys_event_queue_push_by_id(queue, 0, 0, 1, 0);
            std::fprintf(stderr,
                         "[GT6 SPURS] PDI ReadyCount edge wid=%u queue=%u (%s)\n",
                         wid, queue, pushed == 0 ? "queued" : "queue unavailable");
        }
    }
}

void hle_before_real_spurs_import(uint32_t nid, ppu_context* ctx)
{
    if (nid == 0x69726AA2u) /* cellSpursAddWorkload */
        gt6_observe_spurs_workload(static_cast<u32>(ctx->gpr[3]),
                                   static_cast<u32>(ctx->gpr[5]),
                                   static_cast<u32>(ctx->gpr[6]), 0,
                                   ctx->gpr[7]);
}

void hle_cell_spurs_add_workload(ppu_context* ctx)
{
    const u32 spurs = static_cast<u32>(ctx->gpr[3]);
    const u32 wid = static_cast<u32>(ctx->gpr[4]);
    const u32 pm = static_cast<u32>(ctx->gpr[5]);
    const u32 size_pm = static_cast<u32>(ctx->gpr[6]);
    const s32 result = cellSpursAddWorkload(
        reinterpret_cast<void*>(static_cast<uintptr_t>(spurs)),
        reinterpret_cast<void*>(static_cast<uintptr_t>(wid)),
        reinterpret_cast<const void*>(static_cast<uintptr_t>(pm)),
        size_pm, ctx->gpr[7],
        reinterpret_cast<const u8*>(static_cast<uintptr_t>(ctx->gpr[8])),
        static_cast<u32>(ctx->gpr[9]), static_cast<u32>(ctx->gpr[10]));

    if (result == CELL_OK && pm == 0x0144C400u && size_pm == 0x2F80u) {
        uint32_t queue = 0;
        {
            std::lock_guard<std::mutex> lock(g_spurs_lock);
            const auto it = g_spurs.find(spurs);
            if (it != g_spurs.end()) queue = it->second.event_queue;
        }
        std::fprintf(stderr,
                     "[GT6 SPURS] PDI policy workload: spurs=%08X queue=%u wid@%08X data=%08X%08X\n",
                     spurs, queue, wid, static_cast<u32>(ctx->gpr[7] >> 32),
                     static_cast<u32>(ctx->gpr[7]));
        /* The raw workers are launched by the generic observer after their
         * workload IDs and `data` contexts have been committed. */
    }
    set_result(ctx, result);
}

void complete_job_chain_locked(uint32_t chain, JobChainState& state)
{
    if (!state.running || state.completed)
        return;

    state.running = false;
    state.completed = true;
    const auto scheduler = g_spurs.find(state.spurs);
    const uint32_t queue = state.event_queue != 0 ? state.event_queue
        : (scheduler == g_spurs.end() ? g_last_spurs_event_queue
                                      : scheduler->second.event_queue);
    if (queue == 0)
        return;

    /* LV2 receives the same readiness/completion shape observed on an actual
     * local SPURS port: the selector is data2=1 and the remaining fields are
     * zero.  Unlike the old diagnostic escape hatch, this is queued through
     * the scheduler's attached LV2 queue and only after a recorded Run. */
    const int pushed = sys_event_queue_push_by_id(queue,
                                                  0, 0, 1, 0);
    std::fprintf(stderr,
                 "[GT6 SPURS] completion chain=%08X generation=%u queue=%u (%s)\n",
                 chain, state.generation, queue,
                 pushed == 0 ? "queued" : "queue unavailable");
}

/* Execute the resident job-chain command list beginning at entry_ea.
 * The list format (one 8-byte word per slot, big-endian EA in low32):
 *   low7==0x0f : GUARD – CellSpursJobGuard pointer at (cmd & ~0x7f)
 *   low3==0 , addr>=0x10000 : job descriptor pointer
 *   value==2 : SYNC  – memory barrier between job and completion
 *   low3==3 : NEXT  – loop back (resident chain; we stop after first pass)
 *   value==0 : NOP
 *
 * Runs synchronously on the calling thread (PPU side).  Completion is
 * sent to the SPURS event queue only after the actual job finishes. */
static void run_job_chain_list(u32 chain, JobChainState& state)
{
    const u32 entry = state.entry;
    if (!entry || !guest_span(entry, 8)) {
        std::fprintf(stderr,
            "[GT6 SPURS] chain=%08X bad entry=%08X\n", chain, entry);
        return;
    }

    uint32_t gen = state.generation;

    /* A completion belongs to a fully executed resident-list pass.  In
     * particular, an initial Run commonly reaches its JobGuard before the
     * PPU's first Notify; that is a blocked pass, not a completed job. */
    bool pass_finished = false;

    /* Walk at most 64 slots to prevent runaway. */
    for (int slot = 0; slot < 64; ++slot) {
        if (!guest_span(entry + slot * 8u, 8))
            break;

        /* Each slot is a 64-bit big-endian value; we only use the low32
         * (the EA / command tag).  The high32 is flags/reserved. */
        const u32 cmd = guest_read_be32(entry + slot * 8u + 4); /* low32 */
        const u32 type = cmd & 7u;
        const u32 opcode = cmd & 0x7fu;

        if (cmd == 0u) {
            /* NOP – skip */
            continue;
        } else if (cmd == 0x2u) {
            /* SYNC – barrier.  Ordering already guaranteed by single-thread
             * execution; nothing to do host-side. */
            std::fprintf(stderr,
                "[GT6 SPURS] chain=%08X gen=%u SYNC\n", chain, gen);
        } else if ((cmd & 7u) == 3u) {
            /* NEXT – wrap around.  This is a resident chain; we treat one
             * complete pass through GUARD→JOB→SYNC→NEXT as one generation. */
            std::fprintf(stderr,
                "[GT6 SPURS] chain=%08X gen=%u NEXT (end of pass)\n", chain, gen);
            pass_finished = true;
            break;
        } else if (opcode == 0x0fu) {
            /* GUARD commands use a seven-bit opcode; the remaining bits are
             * the 128-byte-aligned CellSpursJobGuard EA. */
            const u32 guard_ea = cmd & ~0x7fu;
            if (!guest_span(guard_ea, 0x24)) {
                std::fprintf(stderr,
                    "[GT6 SPURS] chain=%08X GUARD=%08X bad EA\n", chain, guard_ea);
                break;
            }
            /* Current notify_count is in the guest structure at +0x00;
             * initial (reset) count is at +0x04.  The GUARD blocks the
             * list until all PPU notifiers have decremented to zero. */
            const u32 cur = guest_read_be32(guard_ea + 0x00);
            std::fprintf(stderr,
                "[GT6 SPURS] chain=%08X gen=%u GUARD=%08X count=%u\n",
                chain, gen, guard_ea, cur);
            /* Count may already be 0 if all Notify calls arrived before Run. */
            if (cur != 0) {
                /* Still blocked – do not execute the job this pass.  The
                 * Notify path will decrement and call us again if needed. */
                break;
            }
        } else if (type == 0u && cmd >= 0x10000u) {
            /* Job descriptor pointer. */
            std::fprintf(stderr,
                "[GT6 SPURS] chain=%08X gen=%u JOB descriptor=%08X\n",
                chain, gen, cmd);
            if (skip_spurs_jobs()) {
                if (g_skipped_job_log.emplace(cmd, true).second) {
                    std::fprintf(stderr,
                        "[GT6 SPURS] diagnostic skip JOB descriptor=%08X\n",
                        cmd);
                }
            } else {
                gt6_execute_spu_job(cmd);
            }
        } else {
            std::fprintf(stderr,
                "[GT6 SPURS] chain=%08X gen=%u unknown cmd=%08X\n",
                chain, gen, cmd);
        }
    }

    /* Do not put a completion in the one-slot LV2 queue merely because Run
     * observed a closed JobGuard.  The real SPU has not acquired or executed
     * the job yet; JobGuardNotify will re-enter this list when it opens. */
    if (!pass_finished)
        return;

    /* Emit the completion event only after the job and SYNC completed. */
    complete_job_chain_locked(chain, state);

    /* Resident chain with autoReset guard rearms immediately for the next
     * generation driven by the next Notify cycle. */
    state.running  = true;
    state.completed = false;
}

void hle_cell_spurs_run_job_chain(ppu_context* ctx)
{
    /* cellSpursRunJobChain takes only the JobChain pointer in r3.  The owning
     * CellSpurs and event queue were captured by CreateJobChainWithAttribute. */
    const u32 chain = static_cast<u32>(ctx->gpr[3]);
    std::lock_guard<std::mutex> lock(g_spurs_lock);
    const auto it = g_job_chains.find(chain);
    if (!chain || it == g_job_chains.end()) {
        std::fprintf(stderr, "[GT6 SPURS] run unknown chain=%08X\n", chain);
        set_result(ctx, static_cast<s32>(0x80410A0Fu)); /* JOB_ERROR_STAT */
        return;
    }
    auto& state = it->second;
    state.running = true;
    state.completed = false;
    ++state.generation;
    std::fprintf(stderr,
                 "[GT6 SPURS] run chain=%08X spurs=%08X queue=%u generation=%u\n",
                 chain, state.spurs, state.event_queue, state.generation);
    run_job_chain_list(chain, state);
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_job_guard_initialize(ppu_context* ctx)
{
    /* ABI: r3=JobChain, r4=JobGuard.  Reversing these zeroed the first 0x20
     * bytes of the live chain and keyed Notify under the wrong address. */
    const u32 chain = static_cast<u32>(ctx->gpr[3]);
    const u32 guard = static_cast<u32>(ctx->gpr[4]);
    const u32 notify_count = static_cast<u32>(ctx->gpr[5]);
    const u32 request_spus = static_cast<u32>(ctx->gpr[6]);
    const bool auto_reset = static_cast<u32>(ctx->gpr[7]) != 0;
    if (!guard || !chain || !guest_zero(guard, 0x80)) {
        set_result(ctx, CELL_EFAULT);
        return;
    }
    guest_write_be32(guard + 0x00, notify_count);
    guest_write_be32(guard + 0x04, notify_count);
    guest_write_be32(guard + 0x0c, chain);
    guest_write_be32(guard + 0x10, request_spus);
    guest_write_be32(guard + 0x20, auto_reset ? 1u : 0u);
    std::lock_guard<std::mutex> lock(g_spurs_lock);
    g_job_guards[guard] = {chain, false, auto_reset};
    std::fprintf(stderr,
                 "[GT6 SPURS] guard=%08X chain=%08X initialized autoReset=%u\n",
                 guard, chain, auto_reset ? 1u : 0u);
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_job_guard_notify(ppu_context* ctx)
{
    const u32 guard = static_cast<u32>(ctx->gpr[3]);
    std::lock_guard<std::mutex> lock(g_spurs_lock);
    const auto guard_it = g_job_guards.find(guard);
    if (guard_it == g_job_guards.end()) {
        std::fprintf(stderr, "[GT6 SPURS] notify unknown guard=%08X\n", guard);
        set_result(ctx, CELL_OK);
        return;
    }

    /* Decrement the live notify_count in guest memory (+0x00).  Only when
     * it reaches zero does the GUARD release and the job list execute.
     * The initial count (+0x04) is used to re-arm after autoReset. */
    u32 cur = 0;
    if (guest_span(guard, 0x08)) {
        cur = guest_read_be32(guard + 0x00);
        if (cur > 0) {
            cur -= 1;
            guest_write_be32(guard + 0x00, cur);
        }
    }
    std::fprintf(stderr, "[GT6 SPURS] notify guard=%08X chain=%08X remaining=%u\n",
                 guard, guard_it->second.chain, cur);

    if (cur != 0) {
        /* GUARD is still blocked – more Notify calls are expected before
         * the job list can proceed.  Return without executing the chain. */
        set_result(ctx, CELL_OK);
        return;
    }

    /* notify_count hit zero: execute the full GUARD→job→SYNC→NEXT pass. */
    guard_it->second.notified = true;
    const auto chain_it = g_job_chains.find(guard_it->second.chain);
    if (chain_it != g_job_chains.end()) {
        /* run_job_chain_list handles completion and re-arm internally. */
        run_job_chain_list(chain_it->first, chain_it->second);

        /* After execution, reset the GUARD count for the next cycle. */
        if (guard_it->second.auto_reset && guest_span(guard, 0x08)) {
            const u32 initial = guest_read_be32(guard + 0x04);
            guest_write_be32(guard + 0x00, initial);
            guard_it->second.notified = false;
        }
    }
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_join_job_chain(ppu_context* ctx)
{
    const u32 chain = static_cast<u32>(ctx->gpr[3]);
    std::lock_guard<std::mutex> lock(g_spurs_lock);
    const auto it = g_job_chains.find(chain);
    if (it != g_job_chains.end())
        complete_job_chain_locked(chain, it->second);
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_shutdown_job_chain(ppu_context* ctx)
{
    const u32 chain = static_cast<u32>(ctx->gpr[3]);
    std::lock_guard<std::mutex> lock(g_spurs_lock);
    const auto it = g_job_chains.find(chain);
    if (it != g_job_chains.end()) {
        it->second.running = false;
        it->second.completed = true;
    }
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_get_workload_flag(ppu_context* ctx)
{
    /* cellSpursGetWorkloadFlag(spurs, out): return the address of the
     * scheduler-owned 16-byte CellSpursWorkloadFlag at +0x60.  The previous
     * bridge read r5 and returned a null pointer, so PDI could not arm the
     * workload-flag receiver installed immediately afterwards. */
    const u32 spurs = static_cast<u32>(ctx->gpr[3]);
    const u32 out = static_cast<u32>(ctx->gpr[4]);
    if (!spurs || !out || !guest_span(spurs + 0x60, 0x10) || !guest_span(out, 4)) {
        set_result(ctx, static_cast<s32>(0x80410801u)); /* POLICY_MODULE_NULL_POINTER */
        return;
    }
    guest_write_be32(out, spurs + 0x60);
    set_result(ctx, CELL_OK);
}

void hle_cell_spurs_workload_flag_receiver2(ppu_context* ctx)
{
    /* _cellSpursWorkloadFlagReceiver2(spurs, wid, is_set, debug).  The
     * workload IDs produced by the generic SPURS HLE used to be written in
     * host byte order; accept either representation while old callers are
     * still in flight.  CellSpurs stores the selected receiver at +0x77. */
    const u32 spurs = static_cast<u32>(ctx->gpr[3]);
    u32 wid = static_cast<u32>(ctx->gpr[4]);
    if (wid >= 32)
        wid = __builtin_bswap32(wid);
    const bool is_set = static_cast<u32>(ctx->gpr[5]) != 0;

    if (!spurs || wid >= 32 || !guest_span(spurs + 0x77, 1)) {
        set_result(ctx, static_cast<s32>(0x80410802u)); /* POLICY_MODULE_INVAL */
        return;
    }

    const uint8_t current = vm_base[spurs + 0x77];
    if (is_set) {
        if (current != 0xff) {
            set_result(ctx, static_cast<s32>(0x8041080Au)); /* BUSY */
            return;
        }
        vm_base[spurs + 0x77] = static_cast<uint8_t>(wid);
    } else if (current == wid) {
        vm_base[spurs + 0x77] = 0xff;
    }

    std::fprintf(stderr, "[GT6 SPURS] workload flag receiver=%u (%s)\n",
                 wid, is_set ? "set" : "cleared");
    set_result(ctx, CELL_OK);
}

void hle_cell_sysutil_avconf_ext_unknown(ppu_context* ctx)
{
    static unsigned calls = 0;
    if (calls++ == 0) {
        std::fprintf(stderr,
            "[GT6 HLE] cellSysutilAvconfExt NID 0xFAA275A4 r3..r6=%08X %08X %08X %08X\n",
            static_cast<u32>(ctx->gpr[3]), static_cast<u32>(ctx->gpr[4]),
            static_cast<u32>(ctx->gpr[5]), static_cast<u32>(ctx->gpr[6]));
    }
    set_result(ctx, CELL_OK);
}

struct GuestFiber {
    uint32_t scheduler = 0;
    uint32_t fiber_ea = 0;
    uint32_t entry = 0;
    uint64_t arg = 0;
    uint32_t priority = 0;
    uint32_t stack_ea = 0;
    uint32_t stack_size = 0;
    uint32_t attribute = 0;
    uint32_t state = 0;
};

std::mutex g_guest_fiber_mutex;
std::map<uint32_t, GuestFiber> g_guest_fibers;

/* GT6 imports the scheduler-style CellFiber API, not the small opaque-handle
 * variant in the legacy HLE header.  The guest ABI is:
 * CreateFiber(scheduler, fiber, entry, arg, priority, stack, stackSize, attr).
 * All pointers are guest EAs and must remain guest EAs until dispatched. */
void hle_cell_fiber_scheduler_attribute_initialize(ppu_context* ctx)
{
    const uint32_t attribute = static_cast<uint32_t>(ctx->gpr[3]);
    if (!guest_span(attribute, 0x100u)) {
        set_result(ctx, static_cast<s32>(0x8041080Au));
        return;
    }
    guest_zero(attribute, 0x100u);
    set_result(ctx, CELL_OK);
}

void hle_cell_fiber_initialize_scheduler(ppu_context* ctx)
{
    const uint32_t scheduler = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t attribute = static_cast<uint32_t>(ctx->gpr[4]);
    if (!guest_span(scheduler, 0x200u) || (scheduler & 0x7fu) != 0 ||
        !guest_span(attribute, 0x100u)) {
        set_result(ctx, static_cast<s32>(0x8041080Au));
        return;
    }
    guest_zero(scheduler, 0x200u);
    std::fprintf(stderr, "[GT6 Fiber] scheduler=%08X initialized attr=%08X\n",
                 scheduler, attribute);
    set_result(ctx, CELL_OK);
}

void hle_cell_fiber_attribute_initialize(ppu_context* ctx)
{
    const uint32_t attribute = static_cast<uint32_t>(ctx->gpr[3]);
    if (!guest_span(attribute, 0x100u)) {
        set_result(ctx, static_cast<s32>(0x8041080Au));
        return;
    }
    guest_zero(attribute, 0x100u);
    set_result(ctx, CELL_OK);
}

void hle_cell_fiber_create(ppu_context* ctx)
{
    const uint32_t scheduler = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t fiber_ea = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t entry = static_cast<uint32_t>(ctx->gpr[5]);
    const uint64_t arg = ctx->gpr[6];
    const uint32_t priority = static_cast<uint32_t>(ctx->gpr[7]);
    const uint32_t stack_ea = static_cast<uint32_t>(ctx->gpr[8]);
    const uint32_t stack_size = static_cast<uint32_t>(ctx->gpr[9]);
    const uint32_t attribute = static_cast<uint32_t>(ctx->gpr[10]);
    if (!guest_span(scheduler, 0x200u) || (scheduler & 0x7fu) != 0 ||
        !guest_span(fiber_ea, 0x380u) || (fiber_ea & 0x7fu) != 0 ||
        !entry || priority > 3u || !guest_span(attribute, 0x100u) ||
        (stack_ea && !guest_span(stack_ea, stack_size))) {
        set_result(ctx, static_cast<s32>(0x8041080Au));
        return;
    }
    guest_zero(fiber_ea, 0x380u);
    {
        std::lock_guard<std::mutex> lock(g_guest_fiber_mutex);
        g_guest_fibers[fiber_ea] = GuestFiber{scheduler, fiber_ea, entry, arg,
                                              priority, stack_ea, stack_size,
                                              attribute, 0};
    }
    std::fprintf(stderr,
                 "[GT6 Fiber] create scheduler=%08X fiber=%08X entry=%08X "
                 "arg=%08X%08X priority=%u stack=%08X/%X\n",
                 scheduler, fiber_ea, entry, static_cast<uint32_t>(arg >> 32),
                 static_cast<uint32_t>(arg), priority, stack_ea, stack_size);
    set_result(ctx, CELL_OK);
}

void hle_cell_fiber_run_fibers(ppu_context* ctx)
{
    const uint32_t scheduler = static_cast<uint32_t>(ctx->gpr[3]);
    GuestFiber fiber{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_guest_fiber_mutex);
        for (auto& [fiber_ea, candidate] : g_guest_fibers) {
            if (candidate.scheduler == scheduler && candidate.state == 0) {
                candidate.state = 1;
                fiber = candidate;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        set_result(ctx, CELL_OK);
        return;
    }

    /* `entry` is an ELFv1 OPD in GT6 (0x01590F68 in the observed boot).
     * ps3_indirect_call resolves the descriptor and preserves its guest TOC
     * before entering the actual recompiled callback. */
    std::fprintf(stderr, "[GT6 Fiber] run scheduler=%08X fiber=%08X entry=%08X\n",
                 scheduler, fiber.fiber_ea, fiber.entry);
    ctx->gpr[3] = fiber.arg;
    ctx->ctr = fiber.entry;
    ps3_indirect_call(ctx);
    {
        std::lock_guard<std::mutex> lock(g_guest_fiber_mutex);
        const auto it = g_guest_fibers.find(fiber.fiber_ea);
        if (it != g_guest_fibers.end())
            it->second.state = 3;
    }
    set_result(ctx, CELL_OK);
}

void hle_cell_fiber_check_flags(ppu_context* ctx)
{
    static unsigned calls = 0;
    if (calls++ < 8) {
        std::fprintf(stderr, "[GT6 Fiber] check-flags scheduler=%08X\n",
                     static_cast<uint32_t>(ctx->gpr[3]));
    }
    /* The observed scheduler wrapper invokes this before RunFibers.  No flag
     * producer has yet been delivered here, so preserving the successful SDK
     * return is the non-invasive behavior while exposing that ordering. */
    set_result(ctx, CELL_OK);
}

void hle_cell_fiber_switch(ppu_context* ctx)
{
    const uint32_t id = static_cast<uint32_t>(ctx->gpr[3]);
    GuestFiber fiber;
    {
        std::lock_guard<std::mutex> lock(g_guest_fiber_mutex);
        const auto it = g_guest_fibers.find(id);
        if (it == g_guest_fibers.end()) {
            set_result(ctx, static_cast<s32>(0x80410802u));
            return;
        }
        fiber = it->second;
        it->second.state = 1;
    }
    /* A guest fiber entry is PPU code, never a host function pointer. Execute
     * it through the normal dispatcher with the real guest argument. This is
     * synchronous until a proper non-local yield is required, but it preserves
     * the original callback rather than inventing a completion. */
    ctx->gpr[3] = fiber.arg;
    ctx->ctr = fiber.entry;
    ps3_indirect_call(ctx);
    set_result(ctx, CELL_OK);
}

void hle_cell_fiber_yield(ppu_context* ctx)
{
    /* Yield returns control to the current SwitchFiber invocation.  Until the
     * title requests a resumed continuation, keeping the guest call on the
     * same PPU stack is the faithful non-destructive fallback. */
    set_result(ctx, CELL_OK);
}

} // namespace

extern "C" void gt6_register_hle(void)
{
    /* Register lifted SPU job entry points before any JobChain runs. */
    gt6_spu_job_executor_init();

    g_ps3_hle_before_real_import = hle_before_real_spurs_import;
    cellspurs_workload_observer = gt6_observe_spurs_workload;
    cellspurs_ready_count_observer = gt6_observe_spurs_ready_count;
    /* Digital GT6 installation supplied for this port.  The more-specific
     * /dev_bdvd/PS3_GAME/ mapping lets retail paths work without duplicating
     * the installed game directory on the host. */
    cellfs_set_root_path("E:/Emulation/storage/rpcs3");
    cellfs_add_path_mapping("/dev_hdd0/", "dev_hdd0/");
    cellfs_add_path_mapping("/dev_bdvd/PS3_GAME/", "dev_hdd0/game/NPUA81049/");

    ps3_hle_register_ctx(0x718BF5F8u, "cellFsOpen", hle_cell_fs_open);
    ps3_hle_register_ctx(0x2CB51F0Du, "cellFsClose", hle_cell_fs_close);
    ps3_hle_register_ctx(0x4D5FF8E2u, "cellFsRead", hle_cell_fs_read);
    ps3_hle_register_ctx(0xECDCF2ABu, "cellFsWrite", hle_cell_fs_write);
    ps3_hle_register_ctx(0xA397D042u, "cellFsLseek", hle_cell_fs_lseek);
    ps3_hle_register_ctx(0x0D5B4A14u, "cellFsReadWithOffset",
                         hle_cell_fs_read_with_offset);
    ps3_hle_register_ctx(0xEF3EFA34u, "cellFsFstat", hle_cell_fs_fstat);
    ps3_hle_register_ctx(0x7DE6DCEDu, "cellFsStat", hle_cell_fs_stat);
    ps3_hle_register_ctx(0x3F61245Cu, "cellFsOpendir", hle_cell_fs_opendir);
    ps3_hle_register_ctx(0x5C74903Du, "cellFsReaddir", hle_cell_fs_readdir);
    ps3_hle_register_ctx(0xFF42DCC3u, "cellFsClosedir", hle_cell_fs_closedir);
    ps3_hle_register_ctx(0x7F4677A8u, "cellFsUnlink", hle_cell_fs_unlink);

    ps3_hle_register_ctx(0x626E8518u, "cellGcmMapEaIoAddressWithFlags",
                         hle_cell_gcm_map_ea_io_with_flags);
    ps3_hle_register_ctx(0x1BC200F4u, "sys_lwmutex_unlock",
                         hle_sys_lwmutex_unlock);
    ps3_hle_register_ctx(0x1E7BFF94u, "cellSysCacheMount",
                         hle_cell_syscache_mount);
    ps3_hle_register_ctx(0x7663E368u, "cellAudioOutGetDeviceInfo",
                         hle_cell_audio_out_get_device_info);
    ps3_hle_register_ctx(0xEFEB2679u, "_cellSpursWorkloadAttributeInitialize",
                         hle_cell_spurs_workload_attribute_initialize);
    ps3_hle_register_ctx(0x1F402F8Fu, "cellSpursGetInfo", hle_cell_spurs_get_info);
    ps3_hle_register_ctx(0x182D9890u, "cellSpursRequestIdleSpu",
                         hle_cell_spurs_request_idle_spu);
    ps3_hle_register_ctx(0x4A5EAB63u, "cellSpursWorkloadAttributeSetName",
                         hle_cell_spurs_workload_attribute_set_name);
    ps3_hle_register_ctx(0xD2E23FA9u, "cellSpursSetExceptionEventHandler",
                         hle_cell_spurs_set_exception_event_handler);
    ps3_hle_register_ctx(0x30AA96C4u, "cellSpursInitializeWithAttribute2",
                         hle_cell_spurs_initialize_with_attribute2);
    ps3_hle_register_ctx(0xB9BC6207u, "cellSpursAttachLv2EventQueue",
                         hle_cell_spurs_attach_lv2_event_queue);
    ps3_hle_register_ctx(0x69726AA2u, "cellSpursAddWorkload",
                         hle_cell_spurs_add_workload);
    ps3_hle_register_ctx(0x3548F483u, "_cellSpursJobChainAttributeInitialize",
                         hle_cell_spurs_job_chain_attribute_initialize);
    ps3_hle_register_ctx(0x9FEF70C2u, "cellSpursJobChainAttributeSetName",
                         hle_cell_spurs_job_chain_attribute_set_name);
    ps3_hle_register_ctx(0x303C19CDu, "cellSpursCreateJobChainWithAttribute",
                         hle_cell_spurs_create_job_chain_with_attribute);
    ps3_hle_register_ctx(0xF31731BBu, "cellSpursRunJobChain",
                         hle_cell_spurs_run_job_chain);
    ps3_hle_register_ctx(0x68AAEBA9u, "cellSpursJobGuardInitialize",
                         hle_cell_spurs_job_guard_initialize);
    ps3_hle_register_ctx(0xD5D0B256u, "cellSpursJobGuardNotify",
                         hle_cell_spurs_job_guard_notify);
    ps3_hle_register_ctx(0xA7C066DEu, "cellSpursJoinJobChain",
                         hle_cell_spurs_join_job_chain);
    ps3_hle_register_ctx(0x738E40E6u, "cellSpursShutdownJobChain",
                         hle_cell_spurs_shutdown_job_chain);
    ps3_hle_register_ctx(0xC765B995u, "cellSpursGetWorkloadFlag",
                         hle_cell_spurs_get_workload_flag);
    ps3_hle_register_ctx(0x2DDBCC0Au, "_cellSpursWorkloadFlagReceiver2",
                         hle_cell_spurs_workload_flag_receiver2);
    ps3_hle_register_ctx(0xFAA275A4u, "cellSysutilAvconfExt::unknown_0xFAA275A4",
                         hle_cell_sysutil_avconf_ext_unknown);
    /* GT6's CellFiber imports are the scheduler ABI.  The first three NIDs
     * below are identified from their observed argument shapes at boot:
     * SchedulerAttributeInitialize(attr, sdk), InitializeScheduler(s, attr),
     * and AttributeInitialize(attr, sdk), followed by Create/Run. */
    ps3_hle_register_ctx(0x9E25C72Du, "_cellFiberPpuSchedulerAttributeInitialize",
                         hle_cell_fiber_scheduler_attribute_initialize);
    ps3_hle_register_ctx(0xEE3B604Du, "cellFiberPpuInitializeScheduler",
                         hle_cell_fiber_initialize_scheduler);
    ps3_hle_register_ctx(0xC11F8056u, "_cellFiberPpuAttributeInitialize",
                         hle_cell_fiber_attribute_initialize);
    ps3_hle_register_ctx(0x7C2F4034u, "cellFiberPpuCreateFiber", hle_cell_fiber_create);
    ps3_hle_register_ctx(0x12B1ACF0u, "cellFiberPpuRunFibers", hle_cell_fiber_run_fibers);
    ps3_hle_register_ctx(0xF6C6900Cu, "cellFiberPpuCheckFlags",
                         hle_cell_fiber_check_flags);
    ps3_hle_register_ctx(0x68504C7Au, "cellFiberPpuSwitchFiber", hle_cell_fiber_switch);
    ps3_hle_register_ctx(0x42D0C465u, "cellFiberPpuYieldFiber", hle_cell_fiber_yield);

    std::fprintf(stderr, "[GT6 VFS] /dev_hdd0 and /dev_bdvd/PS3_GAME mounted from RPCS3 storage\n");
}

extern "C" void hook_spurs_init(uint64_t spurs, uint64_t attr) { fprintf(stderr, "[debug] cellSpursInitializeWithAttribute(%llX, %llX)\n", spurs, attr); }
