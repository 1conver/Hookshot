#include "../gpu/vk/amd_profile.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(name, condition)                                                                     \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__);                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main() {
    using namespace mkwvk::amd_profile;

    constexpr uint32_t kStageVertex = 0x01;
    constexpr uint32_t kStageFragment = 0x10;
    constexpr uint32_t kStageCompute = 0x20;
    constexpr uint32_t kStageTask = 0x40;
    constexpr uint32_t kStageMesh = 0x80;

    // 1. Architecture detection across GCN and RDNA 1..4
    CHECK("non-AMD vendor", detect_arch(0x10DE, 0x2684, "NVIDIA GeForce RTX 4090", 32, true, true) == Arch::NonAmd);
    CHECK("GCN Wave64-only", detect_arch(0x1002, 0x67DF, "Radeon RX 580 Series", 64, false, false) == Arch::GCN);
    CHECK("RDNA 1 RX 5700 XT by PCI ID",
          detect_arch(0x1002, 0x731F, "AMD Radeon RX 5700 XT", 32, false, false) == Arch::RDNA1);
    CHECK("RDNA 1 capability fallback (Wave32 without mesh shaders)",
          detect_arch(0x1002, 0x0001, "AMD Unknown Navi 1x", 32, false, false) == Arch::RDNA1);
    CHECK("RDNA 2 RX 6600 XT by PCI ID",
          detect_arch(0x1002, 0x73FF, "AMD Radeon RX 6600 XT", 32, true, false) == Arch::RDNA2);
    CHECK("RDNA 2 Steam Deck APU",
          detect_arch(0x1002, 0x163F, "AMD Custom GPU 0405", 32, true, false) == Arch::RDNA2);
    CHECK("RDNA 3 RX 7900 XTX by PCI ID",
          detect_arch(0x1002, 0x744C, "AMD Radeon RX 7900 XTX", 32, true, true) == Arch::RDNA3);
    CHECK("RDNA 3 Radeon 780M iGPU",
          detect_arch(0x1002, 0x15BF, "AMD Radeon 780M", 32, true, true) == Arch::RDNA3);
    CHECK("RDNA 4 RX 9070 XT by PCI ID",
          detect_arch(0x1002, 0x7550, "AMD Radeon RX 9070 XT", 32, true, true) == Arch::RDNA4);

    // 2. Per-stage Wave32 pinning vs Wave64->2x32 guest warp partitioning
    // AMD Windows (amdvlk64.dll): maxSubgroupSize = 64, requiredSubgroupSizeStages = compute only (0x20)
    CHECK("AMD Windows compute is pinned to 32",
          !stage_needs_wave64_partition(64, kStageCompute, kStageCompute) &&
              stage_max_subgroup_size(64, kStageCompute, kStageCompute) == 32);
    CHECK("AMD Windows task partitions Wave64 into 2x32",
          stage_needs_wave64_partition(64, kStageCompute, kStageTask) &&
              stage_max_subgroup_size(64, kStageCompute, kStageTask) == 64);
    CHECK("AMD Windows mesh partitions Wave64 into 2x32",
          stage_needs_wave64_partition(64, kStageCompute, kStageMesh) &&
              stage_max_subgroup_size(64, kStageCompute, kStageMesh) == 64);
    CHECK("AMD Windows vertex partitions Wave64 into 2x32",
          stage_needs_wave64_partition(64, kStageCompute, kStageVertex) &&
              stage_max_subgroup_size(64, kStageCompute, kStageVertex) == 64);
    CHECK("AMD Windows fragment partitions Wave64 into 2x32",
          stage_needs_wave64_partition(64, kStageCompute, kStageFragment) &&
              stage_max_subgroup_size(64, kStageCompute, kStageFragment) == 64);

    // Linux RADV on RDNA 2/3: requiredSubgroupSizeStages includes compute, task, and mesh (0xE0)
    constexpr uint32_t kRadvPinned = kStageCompute | kStageTask | kStageMesh;
    CHECK("RADV task and mesh stay Wave32 when pinned",
          !stage_needs_wave64_partition(64, kRadvPinned, kStageTask) &&
              !stage_needs_wave64_partition(64, kRadvPinned, kStageMesh));
    CHECK("RADV unpinned vertex/fragment still partition Wave64",
          stage_needs_wave64_partition(64, kRadvPinned, kStageVertex) &&
              stage_needs_wave64_partition(64, kRadvPinned, kStageFragment));

    // GCN: minSubgroupSize = 64, sg32_stages = 0 -> even compute partitions Wave64
    CHECK("GCN compute partitions Wave64",
          stage_needs_wave64_partition(64, 0, kStageCompute) &&
              stage_max_subgroup_size(64, 0, kStageCompute) == 64);

    // NVIDIA: maxSubgroupSize = 32 -> never partitions
    CHECK("32-lane GPU never partitions",
          !stage_needs_wave64_partition(32, 0, kStageVertex) &&
              stage_max_subgroup_size(32, 0, kStageVertex) == 32);

    // 3. AMD compute optimizer policy, RDNA 1 mesh skip, and TAAU precompile filter
    CHECK("AMD disables optimizer on global-memory pointer-following compute shaders",
          disable_compute_opt(0x1002, false, false, true, false, true));
    CHECK("AMD disables optimizer on software-MMA compute shaders",
          disable_compute_opt(0x1002, false, false, false, true, true));
    CHECK("AMD keeps optimizer on ordinary compute shaders",
          !disable_compute_opt(0x1002, false, false, false, false, true));
    CHECK("AMD keeps optimizer on native-MMA compute shaders without global memory",
          !disable_compute_opt(0x1002, false, false, false, true, false));
    CHECK("RX_VK_COMPUTE_OPT=1 re-enables AMD compute optimizer",
          !disable_compute_opt(0x1002, false, true, true, true, true));
    CHECK("RX_VK_NOOPT=1 disables optimizer on any vendor",
          disable_compute_opt(0x10DE, true, true, false, false, false));

    CHECK("RDNA 1 skips mesh programs when mesh_shader is off",
          skip_mesh_program(true, false) && !skip_mesh_program(true, true) && !skip_mesh_program(false, false));
    CHECK("precompile_run skips stood-in TAAU compute shader",
          should_precompile_compute(true, false) && !should_precompile_compute(true, true) &&
              !should_precompile_compute(false, false));

    // 4. Recommended Quality presets by VRAM & discrete/iGPU
    CHECK("iGPU recommends Minimum (4GB)", recommended_preset(8ull << 30, false) == 1);
    CHECK("4GB GPU recommends Minimum (4GB)", recommended_preset(4ull << 30, true) == 1);
    CHECK("6GB GPU recommends Low (6GB)", recommended_preset(6ull << 30, true) == 2);
    CHECK("8GB GPU (RX 6600 XT / 5700 XT / 7600) recommends Balanced (8GB)",
          recommended_preset(8ull << 30, true) == 3);
    CHECK("12GB GPU (RX 6700 XT / 7700 XT) recommends High (12GB)",
          recommended_preset(12ull << 30, true) == 4);
    CHECK("16GB+ GPU (RX 6800 XT / 7900 XTX / 9070 XT) recommends Ultra (16GB+)",
          recommended_preset(16ull << 30, true) == 5);

    std::puts("amd profile: all checks passed");
    return 0;
}
