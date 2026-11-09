"""AMD GPU baseline & RDNA 1..4 / GCN profile (gpu/vk/amd_profile.h and its wiring in vk_main.cpp, vk_shader.cpp, and
vk_draw.cpp), no game or GPU required.

python -I -B tests/test_amd_profile.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_sources():
    vk = ROOT / "gpu" / "vk"
    main_src = (vk / "vk_main.cpp").read_text(encoding="utf-8")
    shader_src = (vk / "vk_shader.cpp").read_text(encoding="utf-8")
    draw_src = (vk / "vk_draw.cpp").read_text(encoding="utf-8")

    for need, where in (
        ('#include "amd_profile.h"', "vk_main.cpp includes amd_profile.h"),
        ("dev.vk12_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES}", "vk_main.cpp queries Vulkan 1.2 properties"),
        ("dev.min_subgroup_size = sc.minSubgroupSize", "vk_main.cpp records minSubgroupSize"),
        ("dev.max_subgroup_size = sc.maxSubgroupSize", "vk_main.cpp records maxSubgroupSize"),
        ("dev.subgroup_ops = sg.supportedOperations", "vk_main.cpp records subgroup operations"),
        ("amd_profile::detect_arch(", "vk_main.cpp detects AMD architecture"),
    ):
        if need not in main_src:
            raise SystemExit(f"vk_main.cpp missing {need!r} ({where})")

    for need, where in (
        ('#include "amd_profile.h"', "vk_shader.cpp includes amd_profile.h"),
        ("amd_profile::stage_needs_wave64_partition(dev.max_subgroup_size, dev.sg32_stages, (uint32_t)vk_stage)", "make_profile partitions unpinned Wave64 stages"),
        ("amd_profile::stage_max_subgroup_size(dev.max_subgroup_size, dev.sg32_stages, (uint32_t)vk_stage)", "make_profile sets per-stage max_subgroup_size"),
        ("dev.vk12.shaderStorageImageArrayNonUniformIndexing", "make_profile wires storage image nonuniform indexing"),
        ("dev.vk12.shaderUniformTexelBufferArrayNonUniformIndexing", "make_profile wires uniform texel buffer nonuniform indexing"),
        ("dev.vk12.shaderStorageTexelBufferArrayNonUniformIndexing", "make_profile wires storage texel buffer nonuniform indexing"),
        ("amd_profile::skip_mesh_program(by_stage[7] != nullptr, dev.mesh_shader)", "seed_program skips mesh programs when mesh_shader is off"),
        ("profile(VK_SHADER_STAGE_COMPUTE_BIT)", "translate_into passes compute stage to profile"),
        ("profile(VK_SHADER_STAGE_TASK_BIT_EXT)", "translate_mesh_into passes task stage to profile"),
        ("profile(VK_SHADER_STAGE_MESH_BIT_EXT)", "translate_mesh_into passes mesh stage to profile"),
    ):
        if need not in shader_src:
            raise SystemExit(f"vk_shader.cpp missing {need!r} ({where})")

    for need, where in (
        ('#include "amd_profile.h"', "vk_draw.cpp includes amd_profile.h"),
        ("amd_profile::disable_compute_opt(", "compute_pipeline_flags uses amd_profile::disable_compute_opt"),
        ("amd_profile::skip_mesh_program(p->mesh, dev.mesh_shader)", "prepare_run skips mesh graphics libraries when mesh_shader is off"),
        ("amd_profile::should_precompile_compute(p.compute, taau_key(p.key))", "precompile_run skips stood-in TAAU compute shader"),
        ("amd_profile::skip_mesh_program(p->mesh, dev.mesh_shader)", "precompile_run skips mesh notes when mesh_shader is off"),
    ):
        if need not in draw_src:
            raise SystemExit(f"vk_draw.cpp missing {need!r} ({where})")

    if draw_src.count("compute_pipeline_flags(") < 5:
        raise SystemExit("vk_draw.cpp: compute_pipeline_flags must be wired into all 4 compute pipeline build sites")
    if draw_src.count("if (pcache_built_since_save.load() > 0) pipeline_cache_save(true);") < 2:
        raise SystemExit("vk_draw.cpp: prepare_run and precompile_run must persist pcache when pipelines were built")

    build_src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    for need, where in (
        ('("vk_shader.cpp", "vk_state.h", "vk_api.h", "amd_profile.h")', "build.py includes amd_profile.h in HOOKSHOT_SHADER_STAMP"),
        ('[shader_stamp_def] if os.path.basename(src) == "vk_shader.cpp" else []', "build.py scopes HOOKSHOT_SHADER_STAMP to vk_shader.cpp"),
    ):
        if need not in build_src:
            raise SystemExit(f"nx2/build.py missing {need!r} ({where})")


def check_make_profile(cxx: str):
    shader_src = (ROOT / "gpu" / "vk" / "vk_shader.cpp").read_text(encoding="utf-8")
    start = shader_src.index("Shader::Profile make_profile(")
    end = shader_src.index("\n}\n", start) + 3
    make_profile_fn = shader_src[start:end]

    fixture = f"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "{(ROOT / 'gpu' / 'vk' / 'amd_profile.h').as_posix()}"
#include "{(ROOT / 'gpu' / 'vk' / 'eden' / 'shader_recompiler' / 'profile.h').as_posix()}"

using VkBool32 = uint32_t;
using VkFlags = uint32_t;
constexpr VkBool32 VK_FALSE = 0;
constexpr VkBool32 VK_TRUE = 1;
constexpr uint32_t VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES = 1000196000u;
constexpr uint32_t VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE = 2u;
constexpr uint32_t VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY = 0u;
constexpr uint32_t VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT = 0x20u;
constexpr uint32_t VK_SUBGROUP_FEATURE_QUAD_BIT = 0x80u;

enum VkShaderStageFlagBits : uint32_t {{
    VK_SHADER_STAGE_VERTEX_BIT = 0x01,
    VK_SHADER_STAGE_FRAGMENT_BIT = 0x10,
    VK_SHADER_STAGE_COMPUTE_BIT = 0x20,
    VK_SHADER_STAGE_TASK_BIT_EXT = 0x40,
    VK_SHADER_STAGE_MESH_BIT_EXT = 0x80,
    VK_SHADER_STAGE_ALL = 0x7FFFFFFF,
}};

struct StubDevice {{
    struct {{
        uint32_t sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        uint32_t denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY;
        uint32_t roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY;
        VkBool32 shaderDenormPreserveFloat16 = VK_TRUE;
        VkBool32 shaderDenormPreserveFloat32 = VK_TRUE;
        VkBool32 shaderDenormFlushToZeroFloat16 = VK_TRUE;
        VkBool32 shaderDenormFlushToZeroFloat32 = VK_TRUE;
        VkBool32 shaderSignedZeroInfNanPreserveFloat16 = VK_TRUE;
        VkBool32 shaderSignedZeroInfNanPreserveFloat32 = VK_TRUE;
        VkBool32 shaderSignedZeroInfNanPreserveFloat64 = VK_TRUE;
    }} vk12_props;
    struct {{
        VkBool32 shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        VkBool32 shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
        VkBool32 shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
    }} vk12;
    struct {{
        struct {{
            uint32_t minStorageBufferOffsetAlignment = 16;
        }} limits;
    }} props;
    uint32_t max_subgroup_size = 64;
    uint32_t sg32_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    uint32_t subgroup_ops = VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
    bool subgroup_quad_all_stages = true;
}} dev;

using namespace mkwvk;

{make_profile_fn}

#define CHECK(cond) do {{ if (!(cond)) {{ std::fprintf(stderr, "FAIL %d: %s\\n", __LINE__, #cond); return 1; }} }} while (0)

int main() {{
    // RDNA 2 on Windows (amdvlk64.dll): compute is pinned to Wave32, graphics/mesh run in Wave64
    const Shader::Profile cs = make_profile(VK_SHADER_STAGE_COMPUTE_BIT);
    CHECK(!cs.warp_size_potentially_larger_than_guest);
    CHECK(cs.max_subgroup_size == 32);
    CHECK(cs.support_shuffle_relative);
    CHECK(cs.support_quad_shuffles);
    CHECK(cs.support_storage_image_array_nonuniform_indexing);
    CHECK(cs.support_uniform_texel_buffer_array_nonuniform_indexing);
    CHECK(cs.support_storage_texel_buffer_array_nonuniform_indexing);
    CHECK(cs.support_fp16_denorm_preserve && cs.support_fp16_denorm_flush);

    for (VkShaderStageFlagBits st : {{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                      VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT}}) {{
        const Shader::Profile gp = make_profile(st);
        CHECK(gp.warp_size_potentially_larger_than_guest);
        CHECK(gp.max_subgroup_size == 64);
        CHECK(!gp.support_shuffle_relative);
        CHECK(gp.support_quad_shuffles);
    }}

    return 0;
}}
"""
    with tempfile.TemporaryDirectory(prefix="amd-make-profile-") as tmp:
        cpp = Path(tmp) / "make_profile_test.cpp"
        exe = Path(tmp) / "make_profile_test.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run(
            [cxx, "-std=c++20", "-static", "-Wall", "-Wextra", "-Werror",
             f"-I{ROOT / 'gpu' / 'vk' / 'compat'}", str(cpp), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="amd-profile-") as tmp:
        exe = Path(tmp) / "amd-profile.exe"
        subprocess.run(
            [args.cxx, "-std=c++20", "-static", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "tests" / "test_amd_profile.cpp"), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    check_make_profile(args.cxx)
    check_presets(args.cxx)


def check_presets(cxx: str):
    menu_src = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    start = menu_src.index("const char* tx_preset(int v) {")
    end = menu_src.index("\n}\n", menu_src.index("void apply_preset(int p) {")) + 3
    preset_fns = menu_src[start:end]

    fixture = f"""
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "{(ROOT / 'gpu' / 'vk' / 'amd_profile.h').as_posix()}"

struct Options {{
    int framegen = 0, pace = 0, resolution = 0, upscale = 0, sharpness = 0, realtime = 1, handheld = 0;
    int preset = 0, vram_mb = 0, mirror_mb = 0;
}} opts;

{preset_fns}

#define CHECK(cond) do {{ if (!(cond)) {{ std::fprintf(stderr, "FAIL %d: %s\\n", __LINE__, #cond); return 1; }} }} while (0)

int main() {{
    // Preset 1: Minimum (4GB / iGPU) - no framegen or heavy Lanczos upscaling
    apply_preset(1);
    CHECK(opts.resolution == 4 && opts.upscale == 0 && opts.sharpness == 0);
    CHECK(opts.framegen == 0 && opts.pace == 0 && opts.handheld == 1);
    CHECK(opts.vram_mb == 2200 && opts.mirror_mb == 256);

    // Preset 2: Low (6GB) - Catmull-Rom upscaling, no framegen
    apply_preset(2);
    CHECK(opts.resolution == 2 && opts.upscale == 1 && opts.sharpness == 25);
    CHECK(opts.framegen == 0 && opts.pace == 1 && opts.handheld == 1);
    CHECK(opts.vram_mb == 3600 && opts.mirror_mb == 512);

    // Preset 3: Balanced (8GB) - Auto dynamic resolution, Catmull-Rom, 2x framegen, docked
    apply_preset(3);
    CHECK(opts.resolution == 0 && opts.upscale == 1 && opts.sharpness == 25);
    CHECK(opts.framegen == 2 && opts.pace == 1 && opts.handheld == 0);
    CHECK(opts.vram_mb == 4800 && opts.mirror_mb == 1024);

    // Preset 4: High (12GB) - 100% resolution, Lanczos, 2x framegen
    apply_preset(4);
    CHECK(opts.resolution == 1 && opts.upscale == 2 && opts.sharpness == 30);
    CHECK(opts.framegen == 2 && opts.pace == 1 && opts.handheld == 0);
    CHECK(opts.vram_mb == 7500 && opts.mirror_mb == 2048);

    // Preset 5: Ultra (16GB+) - 100% resolution, Lanczos, 3x framegen
    apply_preset(5);
    CHECK(opts.resolution == 1 && opts.upscale == 2 && opts.sharpness == 40);
    CHECK(opts.framegen == 3 && opts.pace == 1 && opts.handheld == 0);
    CHECK(opts.vram_mb == 11000 && opts.mirror_mb == 3072);

    for (int p = 0; p <= 5; p++) {{
        CHECK(std::strcmp(tx_preset(p), mkwvk::amd_profile::preset_label(p)) == 0);
    }}
    return 0;
}}
"""
    with tempfile.TemporaryDirectory(prefix="amd-presets-") as tmp:
        cpp = Path(tmp) / "presets_test.cpp"
        exe = Path(tmp) / "presets_test.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run(
            [cxx, "-std=c++20", "-static", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
