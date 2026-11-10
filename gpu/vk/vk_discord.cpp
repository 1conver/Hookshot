#include "vk_discord.h"
#include "amd_profile.h"
#include "vk_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace mkwvk {

namespace {

struct DiscordState {
    std::mutex mu;
    std::condition_variable cv;
    bool started = false;
    bool stopping = false;
    bool enabled = true;
    bool dirty = false;
    uint64_t nonce = 1;
    int64_t start_epoch = 0;
    std::string gpu_summary;
    std::string details = "Starting The Legend of Zelda: Ocarina of Time";
    std::string state_line = "Hookshot (Vulkan Native)";
    int current_preset = 3;
    std::chrono::steady_clock::time_point last_shader_update{};
    std::thread worker;
};

DiscordState& state() {
    static DiscordState s;
    return s;
}

bool discord_disabled() {
    if (std::getenv("HOOKSHOT_PREPARE_ONLY")) return true;
    const char* off = std::getenv("HOOKSHOT_NO_DISCORD");
    return off && *off && std::strcmp(off, "0") != 0;
}

uint32_t current_pid() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

// [HOOKSHOT-FLAG-DISCORD-NO-PRESET]
std::string make_state_line(const std::string& gpu_summary, int /*preset*/) {
    if (gpu_summary.empty()) return "Hookshot (Vulkan Native)";
    return gpu_summary;
}

#ifdef _WIN32
using PipeHandle = HANDLE;
const PipeHandle kInvalidPipe = INVALID_HANDLE_VALUE;

void close_pipe(PipeHandle& h) {
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
}

bool write_all(PipeHandle h, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        DWORD wrote = 0;
        if (!WriteFile(h, p, static_cast<DWORD>(len), &wrote, nullptr) || wrote == 0) return false;
        p += wrote;
        len -= wrote;
    }
    return true;
}

bool read_exact(PipeHandle h, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, static_cast<DWORD>(len), &got, nullptr) || got == 0) return false;
        p += got;
        len -= got;
    }
    return true;
}

bool drain_incoming(PipeHandle h) {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail < 8) return true;
        uint32_t hdr[2]{};
        if (!read_exact(h, hdr, 8)) return false;
        const uint32_t op = hdr[0];
        const uint32_t len = hdr[1];
        if (len > (1u << 20)) return false;
        std::string payload(len, '\0');
        if (len && !read_exact(h, payload.data(), len)) return false;
        if (op == discord_rpc::OP_CLOSE) return false;
        if (op == discord_rpc::OP_PING) {
            const auto pong = discord_rpc::pack_frame(discord_rpc::OP_PONG, payload);
            if (!write_all(h, pong.data(), pong.size())) return false;
        }
    }
}

PipeHandle connect_discord_ipc() {
    const char* cid = std::getenv("HOOKSHOT_DISCORD_GAME_APP_ID");
    if (!cid || !*cid) cid = std::getenv("HOOKSHOT_DISCORD_APP_ID");
    const std::string hs_json = discord_rpc::build_handshake_json(cid);
    const auto hs_frame = discord_rpc::pack_frame(discord_rpc::OP_HANDSHAKE, hs_json);

    for (int i = 0; i < 10; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (!write_all(h, hs_frame.data(), hs_frame.size())) {
            CloseHandle(h);
            continue;
        }
        uint32_t hdr[2]{};
        if (!read_exact(h, hdr, 8) || hdr[0] != discord_rpc::OP_FRAME || hdr[1] == 0 || hdr[1] > (1u << 20)) {
            CloseHandle(h);
            continue;
        }
        std::string reply(hdr[1], '\0');
        if (!read_exact(h, reply.data(), hdr[1]) || reply.find("\"READY\"") == std::string::npos) {
            CloseHandle(h);
            continue;
        }
        return h;
    }
    return INVALID_HANDLE_VALUE;
}
#else
using PipeHandle = int;
constexpr PipeHandle kInvalidPipe = -1;

void close_pipe(PipeHandle& h) {
    if (h >= 0) {
        close(h);
        h = -1;
    }
}

bool write_all(PipeHandle h, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        const ssize_t n = send(h, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool read_exact(PipeHandle h, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        const ssize_t n = recv(h, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool drain_incoming(PipeHandle h) {
    for (;;) {
        uint32_t hdr[2]{};
        const ssize_t peek = recv(h, hdr, 8, MSG_DONTWAIT | MSG_PEEK);
        if (peek == 0) return false;
        if (peek < 8) return true;
        if (!read_exact(h, hdr, 8)) return false;
        const uint32_t op = hdr[0];
        const uint32_t len = hdr[1];
        if (len > (1u << 20)) return false;
        std::string payload(len, '\0');
        if (len && !read_exact(h, payload.data(), len)) return false;
        if (op == discord_rpc::OP_CLOSE) return false;
        if (op == discord_rpc::OP_PING) {
            const auto pong = discord_rpc::pack_frame(discord_rpc::OP_PONG, payload);
            if (!write_all(h, pong.data(), pong.size())) return false;
        }
    }
}

PipeHandle connect_discord_ipc() {
    const char* cid = std::getenv("HOOKSHOT_DISCORD_GAME_APP_ID");
    if (!cid || !*cid) cid = std::getenv("HOOKSHOT_DISCORD_APP_ID");
    const std::string hs_json = discord_rpc::build_handshake_json(cid);
    const auto hs_frame = discord_rpc::pack_frame(discord_rpc::OP_HANDSHAKE, hs_json);

    const char* dirs[] = {
        std::getenv("XDG_RUNTIME_DIR"),
        std::getenv("TMPDIR"),
        std::getenv("TMP"),
        std::getenv("TEMP"),
        "/tmp",
    };
    const char* subdirs[] = {"", "/app/com.discordapp.Discord", "/snap.discord"};

    for (const char* base : dirs) {
        if (!base || !*base) continue;
        for (const char* sub : subdirs) {
            for (int i = 0; i < 10; ++i) {
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s%s/discord-ipc-%d", base, sub, i);
                int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) continue;
                if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
                    close(fd);
                    continue;
                }
                if (!write_all(fd, hs_frame.data(), hs_frame.size())) {
                    close(fd);
                    continue;
                }
                uint32_t hdr[2]{};
                if (!read_exact(fd, hdr, 8) || hdr[0] != discord_rpc::OP_FRAME || hdr[1] == 0 || hdr[1] > (1u << 20)) {
                    close(fd);
                    continue;
                }
                std::string reply(hdr[1], '\0');
                if (!read_exact(fd, reply.data(), hdr[1]) || reply.find("\"READY\"") == std::string::npos) {
                    close(fd);
                    continue;
                }
                return fd;
            }
        }
    }
    return -1;
}
#endif

void worker_loop() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    PipeHandle pipe = kInvalidPipe;
    const uint32_t pid = current_pid();
    const char* oot_img = std::getenv("HOOKSHOT_DISCORD_OOT_IMAGE");
    const char* icon_img = std::getenv("HOOKSHOT_DISCORD_ICON_URL");

    for (;;) {
        std::string details, state_line;
        int64_t start_epoch = 0;
        uint64_t nonce = 1;
        bool should_send = false;
        bool is_enabled = true;

        {
            std::unique_lock<std::mutex> lk(state().mu);
            state().cv.wait_for(lk, std::chrono::seconds(pipe == kInvalidPipe ? 5 : 2), [&] {
                return state().stopping || state().dirty;
            });
            if (state().stopping) break;
            is_enabled = state().enabled;
            if (is_enabled && (state().dirty || pipe == kInvalidPipe)) {
                details = state().details;
                state_line = state().state_line;
                start_epoch = state().start_epoch;
                nonce = state().nonce++;
                should_send = true;
            }
        }

        if (!is_enabled) {
            if (pipe != kInvalidPipe) {
                char clear_json[128];
                std::snprintf(clear_json, sizeof clear_json, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%u,\"activity\":null},\"nonce\":\"c\"}", pid);
                const auto frame = discord_rpc::pack_frame(discord_rpc::OP_FRAME, clear_json);
                write_all(pipe, frame.data(), frame.size());
                close_pipe(pipe);
                pipe = kInvalidPipe;
            }
            std::unique_lock<std::mutex> lk(state().mu);
            state().cv.wait(lk, [&] { return state().stopping || state().enabled; });
            if (state().stopping) break;
            continue;
        }

        if (pipe != kInvalidPipe && !drain_incoming(pipe)) {
            close_pipe(pipe);
        }

        if (pipe == kInvalidPipe) {
            pipe = connect_discord_ipc();
            if (pipe == kInvalidPipe) continue;
            should_send = true;
            logf("vk: discord rich presence connected (The Legend of Zelda: Ocarina of Time)");
        }

        if (should_send && pipe != kInvalidPipe) {
            const std::string json = discord_rpc::build_activity_json(
                discord_rpc::Mode::OcarinaOfTime,
                pid,
                start_epoch,
                details.c_str(),
                state_line.c_str(),
                oot_img,
                icon_img,
                nonce);
            const auto frame = discord_rpc::pack_frame(discord_rpc::OP_FRAME, json);
            if (!write_all(pipe, frame.data(), frame.size()) || !drain_incoming(pipe)) {
                close_pipe(pipe);
            } else {
                std::lock_guard<std::mutex> lk(state().mu);
                state().dirty = false;
            }
        }
    }

    close_pipe(pipe);
}

} // namespace

void discord_start(const char* gpu_name, uint32_t vendor_id, uint32_t device_id, uint32_t max_subgroup_size, bool mesh_shader) {
    if (discord_disabled()) return;
    auto& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.started) return;
    st.started = true;
    st.stopping = false;
    st.start_epoch = static_cast<int64_t>(std::time(nullptr));

    std::string gpu = gpu_name ? gpu_name : "";
    if (vendor_id == 0x1002) {
        const auto arch = amd_profile::detect_arch(vendor_id, device_id, gpu_name, max_subgroup_size, mesh_shader, false);
        if (arch != amd_profile::Arch::NonAmd) {
            gpu += " (";
            gpu += amd_profile::arch_name(arch);
            gpu += ")";
        }
    }
    st.gpu_summary = std::move(gpu);
    st.details = "Playing The Legend of Zelda: Ocarina of Time";
    st.state_line = make_state_line(st.gpu_summary, st.current_preset);
    st.dirty = true;
    st.worker = std::thread(worker_loop);
    st.worker.detach();
}

void discord_set_shader_progress(size_t done, size_t total) {
    if (discord_disabled() || !total) return;
    auto& st = state();
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(st.mu);
    if (!st.started) return;
    if (done < total && now - st.last_shader_update < std::chrono::seconds(2)) return;
    st.last_shader_update = now;
    const size_t pct = (done * 100) / total;
    char buf[96];
    std::snprintf(buf, sizeof buf, "Preparing Shaders (%zu%%)", pct);
    if (st.details != buf) {
        st.details = buf;
        st.dirty = true;
        st.cv.notify_one();
    }
}

void discord_set_in_game() {
    if (discord_disabled()) return;
    auto& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    if (!st.started) return;
    st.details = "Playing The Legend of Zelda: Ocarina of Time";
    st.state_line = make_state_line(st.gpu_summary, st.current_preset);
    st.dirty = true;
    st.cv.notify_one();
}

void discord_set_preset(int preset) {
    if (discord_disabled()) return;
    auto& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    st.current_preset = preset;
    if (!st.started) return;
    const std::string next_state = make_state_line(st.gpu_summary, preset);
    if (st.state_line != next_state) {
        st.state_line = next_state;
        st.dirty = true;
        st.cv.notify_one();
    }
}

void discord_set_enabled(bool enabled) {
    auto& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.enabled == enabled) return;
    st.enabled = enabled;
    st.dirty = true;
    st.cv.notify_all();
}

void discord_stop() {
    auto& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    if (!st.started) return;
    st.stopping = true;
    st.cv.notify_all();
}

} // namespace mkwvk
