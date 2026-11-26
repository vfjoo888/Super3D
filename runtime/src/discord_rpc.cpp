// Native Discord Rich Presence client over local IPC (Windows named pipe / Linux Unix socket).
#include "discord_rpc.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <nx/runtime.h>

namespace nx::discord {

namespace {

enum Opcode : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2 };

std::mutex g_lock;
std::condition_variable g_cv;
std::string g_app_id;
std::string g_details = "In Game";
std::string g_state;
int64_t g_start_time = 0;
bool g_dirty = false;
bool g_enabled = true;
bool g_running = false;
bool g_started = false;
uint64_t g_nonce = 1;
std::thread g_thread;

#if defined(_WIN32)
HANDLE g_pipe = INVALID_HANDLE_VALUE;
#else
int g_sock = -1;
#endif

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\'), out.push_back(char(c));
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 0x20) out.push_back(char(c));
    }
    return out;
}

bool ipc_connected() {
#if defined(_WIN32)
    return g_pipe != INVALID_HANDLE_VALUE;
#else
    return g_sock >= 0;
#endif
}

void ipc_close() {
#if defined(_WIN32)
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
#else
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
#endif
}

bool ipc_write_all(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < size) {
#if defined(_WIN32)
        DWORD n = 0;
        if (!WriteFile(g_pipe, p + sent, DWORD(size - sent), &n, nullptr) || n == 0) return false;
        sent += n;
#else
        ssize_t n = send(g_sock, p + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += size_t(n);
#endif
    }
    return true;
}

bool ipc_read_exact(void* data, size_t size) {
    uint8_t* p = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < size) {
#if defined(_WIN32)
        DWORD n = 0;
        if (!ReadFile(g_pipe, p + got, DWORD(size - got), &n, nullptr) || n == 0) return false;
        got += n;
#else
        ssize_t n = recv(g_sock, p + got, size - got, 0);
        if (n <= 0) return false;
        got += size_t(n);
#endif
    }
    return true;
}

bool ipc_send(Opcode op, const std::string& json) {
    uint32_t hdr[2] = {uint32_t(op), uint32_t(json.size())};
    return ipc_write_all(hdr, sizeof hdr) && (json.empty() || ipc_write_all(json.data(), json.size()));
}

bool ipc_drain_reply() {
#if defined(_WIN32)
    DWORD avail = 0;
    while (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= 8) {
        uint32_t hdr[2] = {};
        if (!ipc_read_exact(hdr, sizeof hdr)) return false;
        if (hdr[1] > 65536) return false;
        std::string buf(hdr[1], '\0');
        if (hdr[1] && !ipc_read_exact(buf.data(), hdr[1])) return false;
        if (hdr[0] == kClose) return false;
    }
    return true;
#else
    for (;;) {
        uint32_t hdr[2] = {};
        ssize_t n = recv(g_sock, hdr, sizeof hdr, MSG_DONTWAIT | MSG_PEEK);
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
        if (n == 0) return false;
        if (size_t(n) < sizeof hdr) return true;
        if (!ipc_read_exact(hdr, sizeof hdr)) return false;
        if (hdr[1] > 65536) return false;
        std::string buf(hdr[1], '\0');
        if (hdr[1] && !ipc_read_exact(buf.data(), hdr[1])) return false;
        if (hdr[0] == kClose) return false;
    }
#endif
}

bool ipc_connect(const std::string& app_id) {
    if (app_id.empty()) return false;
    for (int i = 0; i < 10; ++i) {
#if defined(_WIN32)
        char name[64];
        std::snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        g_pipe = h;
#else
        const char* bases[3] = {std::getenv("XDG_RUNTIME_DIR"), std::getenv("TMPDIR"), "/tmp"};
        int fd = -1;
        for (const char* base : bases) {
            if (!base || !*base) continue;
            const char* suffixes[2] = {"", "/app/com.discordapp.Discord"};
            for (const char* suf : suffixes) {
                std::string path = std::string(base) + suf + "/discord-ipc-" + std::to_string(i);
                if (path.size() >= sizeof(sockaddr_un::sun_path)) continue;
                int s = socket(AF_UNIX, SOCK_STREAM, 0);
                if (s < 0) continue;
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
                if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
                    fd = s;
                    break;
                }
                close(s);
            }
            if (fd >= 0) break;
        }
        if (fd < 0) continue;
        g_sock = fd;
#endif
        const std::string hs = "{\"v\":1,\"client_id\":\"" + json_escape(app_id) + "\"}";
        uint32_t hdr[2] = {};
        if (!ipc_send(kHandshake, hs) || !ipc_read_exact(hdr, sizeof hdr) || hdr[1] > 65536) {
            ipc_close();
            continue;
        }
        std::string reply(hdr[1], '\0');
        if (hdr[1] && !ipc_read_exact(reply.data(), hdr[1])) {
            ipc_close();
            continue;
        }
        if (hdr[0] != kFrame) {
            ipc_close();
            continue;
        }
        nx::log("discord: connected (ipc-%d)", i);
        return true;
    }
    return false;
}

bool send_activity_locked() {
    if (!ipc_connected()) return false;
#if defined(_WIN32)
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    std::string activity;
    if (g_enabled) {
#if defined(NX_GAME_NAME)
        const bool is_sm3dw = std::strcmp(NX_GAME_NAME, "sm3dw") == 0;
#else
        const bool is_sm3dw = false;
#endif
        const char* img = is_sm3dw ? "super3d" : "wonder";
        const char* txt = is_sm3dw ? "Super Mario 3D World + Bowser's Fury" : "Super Mario Bros. Wonder";
        activity = "\"activity\":{"
                   "\"details\":\"" + json_escape(g_details) + "\","
                   + (!g_state.empty() ? "\"state\":\"" + json_escape(g_state) + "\"," : "") +
                   "\"timestamps\":{\"start\":" + std::to_string(g_start_time) + "},"
                   "\"assets\":{\"large_image\":\"" + img + "\",\"large_text\":\"" + txt + "\"}"
                   "}";
    } else {
        activity = "\"activity\":null";
    }
    const std::string payload =
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + "," + activity +
        "},\"nonce\":\"" + std::to_string(g_nonce++) + "\"}";
    if (!ipc_send(kFrame, payload) || !ipc_drain_reply()) {
        ipc_close();
        return false;
    }
    return true;
}

void worker() {
    for (;;) {
        std::string app_id;
        bool should_send = false;
        bool enabled = false;
        {
            std::unique_lock lk(g_lock);
            g_cv.wait_for(lk, std::chrono::seconds(5), [] { return !g_running || g_dirty; });
            if (!g_running) break;
            app_id = g_app_id;
            enabled = g_enabled;
            should_send = g_dirty;
        }
        if (!enabled) {
            if (ipc_connected()) {
                std::lock_guard lk(g_lock);
                send_activity_locked();
                ipc_send(kClose, "{}");
                ipc_close();
            }
            std::lock_guard lk(g_lock);
            g_dirty = false;
            continue;
        }
        if (!ipc_connected()) {
            if (!ipc_connect(app_id)) {
                std::unique_lock lk(g_lock);
                g_cv.wait_for(lk, std::chrono::seconds(10), [] { return !g_running; });
                continue;
            }
            should_send = true;
        }
        if (should_send) {
            std::lock_guard lk(g_lock);
            if (send_activity_locked()) g_dirty = false;
        } else if (!ipc_drain_reply()) {
            ipc_close();
        }
    }
    if (ipc_connected()) {
        ipc_send(kClose, "{}");
        ipc_close();
    }
}

}  // namespace

void init(const char* default_app_id) {
    std::lock_guard lk(g_lock);
    if (g_started) return;
    const char* env_id = std::getenv("NX_DISCORD_APP_ID");
    g_app_id = (env_id && *env_id) ? env_id : (default_app_id ? default_app_id : "");
    if (g_app_id.empty()) return;
    g_start_time = static_cast<int64_t>(std::time(nullptr));
    g_running = true;
    g_started = true;
    g_dirty = true;
    g_thread = std::thread(worker);
}

void set_enabled(bool enabled) {
    std::lock_guard lk(g_lock);
    if (g_enabled == enabled) return;
    g_enabled = enabled;
    g_dirty = true;
    g_cv.notify_all();
}

void set_activity(const char* details, const char* state) {
    std::lock_guard lk(g_lock);
    const std::string d = details ? details : "";
    const std::string s = state ? state : "";
    if (g_details == d && g_state == s) return;
    g_details = d;
    g_state = s;
    g_dirty = true;
    g_cv.notify_all();
}

void shutdown() {
    {
        std::lock_guard lk(g_lock);
        if (!g_started) return;
        g_running = false;
        g_cv.notify_all();
    }
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard lk(g_lock);
    g_started = false;
}

}  // namespace nx::discord
