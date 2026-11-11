#pragma once
// Discord Rich Presence (Local IPC, discord-ipc-0..9) for Hookshot and The Legend of Zelda: Ocarina of Time.
// Zero external dependencies; runs asynchronously on a low-priority background thread so rendering and shader
// compilation never wait on IPC. Disabled when HOOKSHOT_PREPARE_ONLY=1 or HOOKSHOT_NO_DISCORD=1.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mkwvk {

namespace discord_rpc {

// Default verified Discord Application ID that supports Rich Presence & external media proxy URLs.
// Overridable via HOOKSHOT_DISCORD_APP_ID / HOOKSHOT_DISCORD_GAME_APP_ID / HOOKSHOT_DISCORD_BUILDER_APP_ID.
inline constexpr const char* kDefaultClientId = "1442727160535056384";

// Official Hookshot icon and The Legend of Zelda: Ocarina of Time cover/icon URLs (proxied via Discord mp:external).
inline constexpr const char* kHookshotIconUrl =
    "https://raw.githubusercontent.com/NotRexed/Hookshot/main/assets/hookshot.png";
// [HOOKSHOT-FLAG-DISCORD-OOT-REMAKE-LOGO]
inline constexpr const char* kOcarinaIconUrl =
    "https://files.catbox.moe/uvgrv9.png";

enum class Mode : uint8_t {
    Builder = 0,
    OcarinaOfTime = 1,
};

enum Opcode : uint32_t {
    OP_HANDSHAKE = 0,
    OP_FRAME = 1,
    OP_CLOSE = 2,
    OP_PING = 3,
    OP_PONG = 4,
};

inline std::string json_escape(const char* s) {
    if (!s) return {};
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        const unsigned char c = *p;
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 0x20) out.push_back(static_cast<char>(c));
    }
    return out;
}

inline std::vector<uint8_t> pack_frame(uint32_t opcode, const std::string& json) {
    const uint32_t len = static_cast<uint32_t>(json.size());
    std::vector<uint8_t> frame(8 + len);
    std::memcpy(frame.data(), &opcode, 4);
    std::memcpy(frame.data() + 4, &len, 4);
    if (len) std::memcpy(frame.data() + 8, json.data(), len);
    return frame;
}

inline std::string build_handshake_json(const char* client_id) {
    const char* cid = (client_id && *client_id) ? client_id : kDefaultClientId;
    return std::string("{\"v\":1,\"client_id\":\"") + json_escape(cid) + "\"}";
}

inline std::string build_activity_json(
    Mode mode,
    uint32_t pid,
    int64_t start_epoch,
    const char* details,
    const char* state,
    const char* large_image_override = nullptr,
    const char* small_image_override = nullptr,
    uint64_t nonce = 1)
{
    const bool is_oot = (mode == Mode::OcarinaOfTime);
    const char* activity_name = is_oot ? "The Legend of Zelda: Ocarina of Time" : "Hookshot Builder";
    const char* large_img = (large_image_override && *large_image_override)
                                ? large_image_override
                                : (is_oot ? kOcarinaIconUrl : kHookshotIconUrl);
    const char* large_txt = is_oot ? "The Legend of Zelda: Ocarina of Time" : "Hookshot Builder";
    const char* small_img = (small_image_override && *small_image_override)
                                ? small_image_override
                                : (is_oot ? kHookshotIconUrl : kOcarinaIconUrl);
    const char* small_txt = is_oot ? "Hookshot (Vulkan Native)" : "The Legend of Zelda: Ocarina of Time";

    char num_buf[96];
    std::snprintf(num_buf, sizeof num_buf, "%u", pid);
    std::string out = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
    out += num_buf;
    out += ",\"activity\":{\"name\":\"";
    out += json_escape(activity_name);
    out += "\",\"type\":0";
    if (details && *details) {
        out += ",\"details\":\"";
        out += json_escape(details);
        out += "\"";
    }
    if (state && *state) {
        out += ",\"state\":\"";
        out += json_escape(state);
        out += "\"";
    }
    if (start_epoch > 0) {
        std::snprintf(num_buf, sizeof num_buf, "%lld", static_cast<long long>(start_epoch));
        out += ",\"timestamps\":{\"start\":";
        out += num_buf;
        out += "}";
    }
    out += ",\"assets\":{\"large_image\":\"";
    out += json_escape(large_img);
    out += "\",\"large_text\":\"";
    out += json_escape(large_txt);
    out += "\",\"small_image\":\"";
    out += json_escape(small_img);
    out += "\",\"small_text\":\"";
    out += json_escape(small_txt);
    out += "\"}}}";
    std::snprintf(num_buf, sizeof num_buf, "%llu", static_cast<unsigned long long>(nonce));
    out += ",\"nonce\":\"";
    out += num_buf;
    out += "\"}";
    return out;
}

} // namespace discord_rpc

void discord_start(const char* gpu_name, uint32_t vendor_id, uint32_t device_id, uint32_t max_subgroup_size, bool mesh_shader);
void discord_set_shader_progress(size_t done, size_t total);
void discord_set_in_game();
void discord_set_preset(int preset);
void discord_set_enabled(bool enabled);
void discord_stop();

} // namespace mkwvk
