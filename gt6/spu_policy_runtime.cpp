/* Runtime bridge for GT6's raw CellSpurs policy module.
 *
 * The title stores this policy as a local-store image, not as an ELF.  It is
 * copied from the loaded PPU image into a fresh SPU local store and run on its
 * own host thread, matching the PS3's concurrent SPU scheduling model. */
#include "../runtime/spu/spu_lifted_job.h"

extern "C" {
void gt6_pdi_policy_spu_func_00000A00(spu_context*);
void gt6_pdi_policy_spu_recomp_register(void);
void spu_begin_image(int image_id);
extern uint8_t* vm_base;
}

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kPdiPolicySize = 0x2F80;
constexpr int kPdiPolicyImageId = 24;
std::atomic_bool g_policy_started{false};

} // namespace

extern "C" void gt6_pdi_policy_start(uint32_t policy_ea, uint32_t policy_size,
                                        uint32_t spurs_ea)
{
    if (!vm_base || policy_size != kPdiPolicySize) {
        std::fprintf(stderr,
            "[GT6 SPURS] policy launch skipped: ea=%08X size=%u\n",
            policy_ea, policy_size);
        return;
    }
    if (g_policy_started.exchange(true))
        return;

    std::vector<uint8_t> image(vm_base + policy_ea,
                               vm_base + policy_ea + policy_size);
    std::thread([image = std::move(image), spurs_ea]() mutable {
        uint8_t local_store[SPU_LS_SIZE] = {};
        std::memcpy(local_store, image.data(), image.size());
        spu_begin_image(kPdiPolicyImageId);
        gt6_pdi_policy_spu_recomp_register();
        std::fprintf(stderr,
            "[GT6 SPURS] starting lifted PDI policy: %u bytes, spurs=%08X\n",
            (unsigned)image.size(), spurs_ea);
        spu_run_lifted_job_img(gt6_pdi_policy_spu_func_00000A00,
                               local_store, spurs_ea, kPdiPolicyImageId);
        std::fprintf(stderr, "[GT6 SPURS] lifted PDI policy returned\n");
    }).detach();
}
