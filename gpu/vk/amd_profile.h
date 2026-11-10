/* AMD architecture detection, per-stage Wave32/Wave64 subgroup policy, and compiler flags (gpu/vk). */
#pragma once

#include <cstdint>
#include <cstring>

namespace mkwvk::amd_profile {

enum class Arch { NonAmd, GCN, RDNA1, RDNA2, RDNA3, RDNA4 };

inline const char* arch_name(Arch a) {
    switch (a) {
    case Arch::GCN: return "GCN";
    case Arch::RDNA1: return "RDNA 1";
    case Arch::RDNA2: return "RDNA 2";
    case Arch::RDNA3: return "RDNA 3";
    case Arch::RDNA4: return "RDNA 4";
    default: return "non-AMD";
    }
}

/// Identifies the AMD GPU generation from its PCI ID, name, and hardware capabilities (Wave32 support, mesh shaders,
/// cooperative matrix). Used for startup diagnostics and architecture-aware baseline policy across GCN and RDNA 1..4.
inline Arch detect_arch(uint32_t vendor_id, uint32_t device_id, const char* device_name, uint32_t min_subgroup_size,
                        bool has_mesh_shader, bool has_coop_matrix) {
    if (vendor_id != 0x1002) return Arch::NonAmd;
    const auto has = [&](const char* needle) {
        return device_name && std::strstr(device_name, needle) != nullptr;
    };
    if ((device_id >= 0x7500 && device_id <= 0x75FF) || has("RX 90") || has("Radeon AI PRO R9"))
        return Arch::RDNA4;
    if ((device_id >= 0x7440 && device_id <= 0x74FF) || device_id == 0x15BF || device_id == 0x15C8 ||
        device_id == 0x150E || device_id == 0x1900 || has("RX 7") || has("780M") || has("760M") ||
        has("890M") || has("880M") || has("8060S") || has("8050S"))
        return Arch::RDNA3;
    if ((device_id >= 0x73A0 && device_id <= 0x743F) || device_id == 0x163F || device_id == 0x164C ||
        device_id == 0x1681 || device_id == 0x13C0 || has("RX 6") || has("680M") || has("660M") ||
        has("Custom GPU 0405"))
        return Arch::RDNA2;
    if ((device_id >= 0x7310 && device_id <= 0x736F) || has("RX 5700") || has("RX 5600") ||
        has("RX 5500") || has("RX 5300"))
        return Arch::RDNA1;
    if (min_subgroup_size >= 64) return Arch::GCN;
    if (has_coop_matrix) return Arch::RDNA3;
    if (has_mesh_shader) return Arch::RDNA2;
    return Arch::RDNA1;
}

/// Whether pin_subgroup can hold vk_stage to a 32-lane subgroup via VK_EXT_subgroup_size_control.
inline bool stage_pinned_to_32(uint32_t sg32_stages, uint32_t vk_stage) {
    return vk_stage != 0 && (sg32_stages & vk_stage) != 0;
}

/// NVN2 programs assume 32-lane warps. On AMD Windows (amdvlk64.dll), requiredSubgroupSizeStages is compute-only (0x20),
/// while default subgroupSize is 64 (maxSubgroupSize 64); on GCN, minSubgroupSize is 64 on all stages. Any stage not
/// pinned to 32 lanes on a maxSubgroupSize > 32 GPU must partition 64-lane waves into two 32-lane guest warps.
inline bool stage_needs_wave64_partition(uint32_t max_subgroup_size, uint32_t sg32_stages, uint32_t vk_stage) {
    return max_subgroup_size > 32 && !stage_pinned_to_32(sg32_stages, vk_stage);
}

inline uint32_t stage_max_subgroup_size(uint32_t max_subgroup_size, uint32_t sg32_stages, uint32_t vk_stage) {
    return stage_needs_wave64_partition(max_subgroup_size, sg32_stages, vk_stage) ? max_subgroup_size : 32u;
}

/// AMD's driver optimizer takes 15-23 s on pointer-following compute shaders (foliage culling 53caac5f, ee7764a6) and
/// minutes on unrolled software cooperative-matrix loops (9dc98871). Building those compute pipelines with
/// VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT avoids startup/gameplay freezes, matching task/mesh shaders.
inline bool disable_compute_opt(uint32_t vendor_id, bool noopt_env, bool compute_opt_env,
                                bool uses_global_memory, bool uses_coopmat, bool mma_software) {
    if (noopt_env) return true;
    if (vendor_id == 0x1002 && !compute_opt_env)
        return uses_global_memory || (uses_coopmat && mma_software);
    return false;
}

/// RDNA 1 (RX 5000) and GCN lack VK_EXT_mesh_shader: mesh programs in the seed or pipeline notes must not be translated
/// or compiled with MeshShadingEXT on devices without mesh_shader enabled.
inline bool skip_mesh_program(bool is_mesh_program, bool has_mesh_shader) {
    return is_mesh_program && !has_mesh_shader;
}

/// A compute program replaced at dispatch time by the TAAU stand-in (fd029a31 on software-MMA cards) must not have its
/// full neural-network pipeline compiled during precompile_run.
inline bool should_precompile_compute(bool is_compute, bool is_taau_standin) {
    return is_compute && !is_taau_standin;
}

/// Recommended Insert-menu Quality preset (1..5) for a GPU given its device-local VRAM and whether it is discrete:
///   1: Minimum (4GB / iGPU)
///   2: Low (6GB)
///   3: Balanced (8GB)
///   4: High (12GB)
///   5: Ultra (16GB+)
inline int recommended_preset(uint64_t vram_bytes, bool is_discrete) {
    if (!is_discrete || vram_bytes <= (4ull << 30) + (512ull << 20)) return 1;
    if (vram_bytes <= (6ull << 30) + (512ull << 20)) return 2;
    if (vram_bytes <= (9ull << 30)) return 3;
    if (vram_bytes <= (13ull << 30)) return 4;
    return 5;
}

inline const char* preset_label(int preset) {
    switch (preset) {
    case 1: return "Minimum (4GB)";
    case 2: return "Low (6GB)";
    case 3: return "Balanced (8GB)";
    case 4: return "High (12GB)";
    case 5: return "Ultra (16GB+)";
    default: return "Custom";
    }
}

} // namespace mkwvk::amd_profile
