"""Discord Rich Presence (Hookshot Builder <-> The Legend of Zelda: Ocarina of Time) checks, game-free.

python -I -B tests/test_discord_rpc.py [--cxx g++]
"""
import argparse
import ast
import json
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_sources():
    vk_main = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
    vk_menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    cs_rpc = (ROOT / "builder" / "HookshotBuilder" / "DiscordRpc.cs").read_text(encoding="utf-8")
    cs_win = (ROOT / "builder" / "HookshotBuilder" / "MainWindow.xaml.cs").read_text(encoding="utf-8")
    lx_builder = (ROOT / "builder" / "linux" / "hookshot_builder.py").read_text(encoding="utf-8")

    for need, where in (
        ('#include "vk_discord.h"', "vk_main.cpp includes vk_discord.h"),
        ("discord_start(dev.props.deviceName, dev.props.vendorID, dev.props.deviceID, dev.max_subgroup_size, dev.mesh_shader)",
         "vk_main.cpp starts Discord Rich Presence for Ocarina of Time"),
        ("discord_set_shader_progress(done, total)", "vk_main.cpp reports shader preparation progress to Discord"),
        ("discord_set_in_game()", "vk_main.cpp switches Discord status to in-game after precompile"),
    ):
        if need not in vk_main:
            raise SystemExit(f"vk_main.cpp missing {need!r} ({where})")

    for need, where in (
        ('#include "vk_discord.h"', "vk_menu.cpp includes vk_discord.h"),
        ("discord_set_preset(opts.preset)", "vk_menu.cpp updates Discord preset label"),
    ):
        if need not in vk_menu:
            raise SystemExit(f"vk_menu.cpp missing {need!r} ({where})")

    for need, where in (
        ("The Legend of Zelda: Ocarina of Time", "DiscordRpc.cs sets Ocarina of Time activity name"),
        ("Hookshot Builder", "DiscordRpc.cs sets Hookshot Builder activity name"),
        ("https://raw.githubusercontent.com/NotRexed/Hookshot/main/assets/hookshot.png", "DiscordRpc.cs has Hookshot icon"),
        ("https://upload.wikimedia.org/wikipedia/en/5/57/The_Legend_of_Zelda_Ocarina_of_Time.jpg", "DiscordRpc.cs has Ocarina of Time icon"),
        ("SetPlayingOcarinaOfTime", "DiscordRpc.cs switches to Ocarina of Time when game runs"),
    ):
        if need not in cs_rpc:
            raise SystemExit(f"DiscordRpc.cs missing {need!r} ({where})")

    for need, where in (
        ("DiscordRpc.SetBuilderStatus(", "MainWindow.xaml.cs updates Builder status in Discord"),
        ("DiscordRpc.SetBuilderProgress(", "MainWindow.xaml.cs updates build progress in Discord"),
        ("DiscordRpc.SetPlayingOcarinaOfTime(", "MainWindow.xaml.cs switches Discord to Ocarina of Time when launching game"),
        ("DiscordRpc.Shutdown()", "MainWindow.xaml.cs shuts down Discord RPC on close"),
    ):
        if need not in cs_win:
            raise SystemExit(f"MainWindow.xaml.cs missing {need!r} ({where})")

    for need, where in (
        ("class DiscordPresence:", "hookshot_builder.py defines DiscordPresence"),
        ("DISCORD.set_playing_oot(", "hookshot_builder.py switches to Ocarina of Time in play()"),
        ("DISCORD.set_builder(", "hookshot_builder.py sets Hookshot Builder status"),
        ("DISCORD.set_progress(", "hookshot_builder.py updates build progress in Discord"),
    ):
        if need not in lx_builder:
            raise SystemExit(f"hookshot_builder.py missing {need!r} ({where})")


def check_python_payloads():
    source = (ROOT / "builder" / "linux" / "hookshot_builder.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    cls_node = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "DiscordPresence")
    assigns = [n for n in tree.body if isinstance(n, ast.Assign) and any(
        isinstance(t, ast.Name) and t.id.startswith("DISCORD_") for t in n.targets)]
    mod = ast.Module(body=assigns + [cls_node], type_ignores=[])
    ns = {"json": json, "os": __import__("os"), "socket": __import__("socket"),
          "struct": struct, "threading": __import__("threading"), "time": __import__("time")}
    exec(compile(mod, "hookshot_builder.py", "exec"), ns)
    dp = ns["DiscordPresence"]

    builder_payload = dp.build_activity_payload(False, 1234, 1700000000, "Configuring Game Files", "Ready", 1)
    act_b = builder_payload["args"]["activity"]
    if act_b["name"] != "Hookshot Builder":
        raise SystemExit(f"expected Hookshot Builder name, got {act_b['name']!r}")
    if "hookshot.png" not in act_b["assets"]["large_image"]:
        raise SystemExit(f"expected Hookshot icon in large_image, got {act_b['assets']['large_image']!r}")

    oot_payload = dp.build_activity_payload(True, 5678, 1700000100, "Playing The Legend of Zelda: Ocarina of Time", "Hookshot · Balanced", 2)
    act_o = oot_payload["args"]["activity"]
    if act_o["name"] != "The Legend of Zelda: Ocarina of Time":
        raise SystemExit(f"expected Ocarina of Time name, got {act_o['name']!r}")
    if "Ocarina_of_Time" not in act_o["assets"]["large_image"] or "hookshot.png" not in act_o["assets"]["small_image"]:
        raise SystemExit(f"unexpected Ocarina of Time assets: {act_o['assets']!r}")

    frame = dp.pack_frame(1, oot_payload)
    op, length = struct.unpack("<II", frame[:8])
    if op != 1 or length != len(frame) - 8:
        raise SystemExit("invalid IPC frame header")


def check_cpp_rpc(cxx: str):
    fixture = f"""
#include <cstdio>
#include <cstring>
#include "{(ROOT / 'gpu' / 'vk' / 'vk_discord.h').as_posix()}"

#define CHECK(cond) do {{ if (!(cond)) {{ std::fprintf(stderr, "FAIL %d: %s\\n", __LINE__, #cond); return 1; }} }} while (0)

int main() {{
    using namespace mkwvk::discord_rpc;
    const std::string hs = build_handshake_json(nullptr);
    CHECK(hs.find("\\"v\\":1") != std::string::npos);
    CHECK(hs.find(kDefaultClientId) != std::string::npos);

    const std::string b_json = build_activity_json(Mode::Builder, 1001, 1700000000, "Building Ocarina of Time (54%)", "Compiling changed files", nullptr, nullptr, 1);
    CHECK(b_json.find("\\"name\\":\\"Hookshot Builder\\"") != std::string::npos);
    CHECK(b_json.find(kHookshotIconUrl) != std::string::npos);
    CHECK(b_json.find("Building Ocarina of Time (54%)") != std::string::npos);

    const std::string o_json = build_activity_json(Mode::OcarinaOfTime, 2002, 1700000050, "Playing The Legend of Zelda: Ocarina of Time", "AMD Radeon RX 6600 XT (RDNA 2) · Balanced", nullptr, nullptr, 2);
    CHECK(o_json.find("\\"name\\":\\"The Legend of Zelda: Ocarina of Time\\"") != std::string::npos);
    CHECK(o_json.find(kOcarinaIconUrl) != std::string::npos);
    CHECK(o_json.find("\\"small_image\\":\\"https://raw.githubusercontent.com/NotRexed/Hookshot/main/assets/hookshot.png\\"") != std::string::npos);
    CHECK(o_json.find("AMD Radeon RX 6600 XT (RDNA 2)") != std::string::npos);

    const auto framed = pack_frame(OP_FRAME, o_json);
    CHECK(framed.size() == 8 + o_json.size());
    uint32_t op = 0, len = 0;
    std::memcpy(&op, framed.data(), 4);
    std::memcpy(&len, framed.data() + 4, 4);
    CHECK(op == OP_FRAME && len == o_json.size());
    return 0;
}}
"""
    with tempfile.TemporaryDirectory(prefix="discord-rpc-") as tmp:
        cpp = Path(tmp) / "test_discord.cpp"
        exe = Path(tmp) / "test_discord.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-static", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_sources()
    check_python_payloads()
    check_cpp_rpc(args.cxx)
    print("PASS: Discord Rich Presence (Hookshot Builder <-> The Legend of Zelda: Ocarina of Time) verified")


if __name__ == "__main__":
    main()
