/*
 * GT6 audio SPURS job bridge.
 *
 * The sixteen binaries registered by GT6 are Sony/Polyphony audio DSP
 * overlays.  They are loaded independently at LS address zero; combining
 * them by guest-EA modulo the local-store size (the previous implementation)
 * corrupts shared LS regions and invokes the wrong entry point.  Running an
 * incomplete lift with a fabricated Job Manager ABI is worse than leaving an
 * effect dry: it corrupts the PPU audio state before the renderer starts.
 *
 * Until their SPU ABI is lifted as independent overlays, acknowledge these
 * jobs synchronously.  CellAudio owns the mix buffers and already supplies a
 * valid silent buffer during bootstrap, so this is a deliberate no-effect
 * implementation rather than a fake DMA transfer.  The SPURS chain still
 * receives its normal completion event from gt6_hle.cpp.
 */

#include "spu_job_executor.h"

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

extern "C" uint8_t* vm_base;

namespace {

struct AudioJob {
    uint32_t binary_ea;
    uint16_t size_units;
    const char* name;
};

/* Values are taken from the live CellSpursJob descriptors.  size_units is in
 * 16-byte units, not bytes. */
constexpr AudioJob k_audio_jobs[] = {
    {0x0144F600u, 0x0FD9u, "socdac3"},
    {0x014A5100u, 0x0D02u, "socisynth"},
    {0x01478A80u, 0x0879u, "socedmix"},
    {0x01492300u, 0x0251u, "socethru"},
    {0x0145F400u, 0x0361u, "soceamp"},
    {0x01481280u, 0x04C2u, "socefilter"},
    {0x01462A80u, 0x0441u, "socecomp"},
    {0x01485F00u, 0x0609u, "socelverb"},
    {0x01466F00u, 0x11B1u, "socedly"},
    {0x014A0000u, 0x0509u, "soceumix"},
    {0x0148C000u, 0x0629u, "socelverbx"},
    {0x01494880u, 0x0B75u, "socetverb"},
    {0x01341000u, 0x0396u, "socepdicomp"},
    {0x01339880u, 0x0776u, "socepdideq"},
    {0x01336400u, 0x0341u, "socepdidist"},
    {0x01330580u, 0x05E8u, "socepdirta"},
};

static uint32_t read_be32(uint32_t ea)
{
    return (static_cast<uint32_t>(vm_base[ea]) << 24) |
           (static_cast<uint32_t>(vm_base[ea + 1]) << 16) |
           (static_cast<uint32_t>(vm_base[ea + 2]) << 8) |
           static_cast<uint32_t>(vm_base[ea + 3]);
}

static uint16_t read_be16(uint32_t ea)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(vm_base[ea]) << 8) |
                                 static_cast<uint16_t>(vm_base[ea + 1]));
}

static const AudioJob* find_audio_job(uint32_t binary_ea)
{
    for (const auto& job : k_audio_jobs)
        if (job.binary_ea == binary_ea)
            return &job;
    return nullptr;
}

std::mutex g_log_lock;
bool g_logged_jobs[sizeof(k_audio_jobs) / sizeof(k_audio_jobs[0])] = {};

/* A real DSP overlay runs asynchronously for roughly one audio block.  The
 * dry HLE completes synchronously, so without a small paced hand-off the PPU
 * audio waiter consumes its own completion event in a tight loop and starves
 * the boot threads.  Keep this host-only and allow diagnostics to turn it
 * off explicitly. */
static unsigned audio_hle_pace_ms()
{
    const char* value = std::getenv("GT6_AUDIO_HLE_PACE_MS");
    if (!value || !*value)
        return 5u;
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 && parsed <= 100 ? static_cast<unsigned>(parsed) : 0u;
}

} // namespace

extern "C" void gt6_spu_job_executor_init(void)
{
    std::fprintf(stderr,
        "[GT6 JOB EXEC] GT6 audio DSP overlays: synchronous dry HLE enabled\n");
}

extern "C" int gt6_execute_spu_job(uint32_t descriptor_ea)
{
    if (!vm_base || descriptor_ea < 0x10000u) {
        std::fprintf(stderr, "[GT6 JOB EXEC] bad descriptor=%08X\n", descriptor_ea);
        return -1;
    }

    const uint32_t binary_ea = read_be32(descriptor_ea + 0x04);
    const uint16_t size_units = read_be16(descriptor_ea + 0x08);
    const AudioJob* job = find_audio_job(binary_ea);
    if (!job) {
        std::fprintf(stderr,
            "[GT6 JOB EXEC] unsupported SPU overlay=%08X units=%04X descriptor=%08X\n",
            binary_ea, static_cast<unsigned>(size_units), descriptor_ea);
        return -1;
    }
    if (size_units != job->size_units) {
        std::fprintf(stderr,
            "[GT6 JOB EXEC] overlay=%08X size mismatch got=%04X expected=%04X\n",
            binary_ea, static_cast<unsigned>(size_units),
            static_cast<unsigned>(job->size_units));
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_log_lock);
    const size_t index = static_cast<size_t>(job - k_audio_jobs);
    if (!g_logged_jobs[index]) {
        g_logged_jobs[index] = true;
        std::fprintf(stderr,
            "[GT6 JOB EXEC] audio HLE %s overlay=%08X bytes=%05X descriptor=%08X\n",
            job->name, binary_ea, static_cast<unsigned>(size_units) << 4,
            descriptor_ea);
    }
    const unsigned pace_ms = audio_hle_pace_ms();
    if (pace_ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms));
    return 0;
}
