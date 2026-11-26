// Scripted input for unattended runs: NX_INPUT_SCRIPT="<seconds>:<button>:<hold seconds>;..." presses buttons at
// fixed times after the first pad read, e.g. "70:A:0.2;74:A:0.2;80:RIGHT:5" (buttons: A B X Y L R ZL ZR PLUS MINUS
// LEFT RIGHT UP DOWN; the directions also push the left stick). Added on top of the real pad and keyboard.
//
// NX_INPUT_RECORD=1: what the player does is written in that same format, to <data root>/input_record.txt
// (a path instead is the frame recorder's, play.cpp): every press of a button, and the stick past half way as a direction, from the first
// pad read. Played back through NX_INPUT_SCRIPT, a test run follows a real player's run instead of a blind script.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "platform.h"

namespace nx::plat {

namespace {

struct Press {
    double at = 0, hold = 0;
    uint64_t bits = 0;
    int lx = 0, ly = 0;
};

std::vector<Press> parse(const char* s) {
    struct Name {
        const char* name;
        uint64_t bits;
        int lx, ly;
    };
    static const Name names[] = {
        {"A", kA, 0, 0},          {"B", kB, 0, 0},          {"X", kX, 0, 0},          {"Y", kY, 0, 0},
        {"L", kL, 0, 0},          {"R", kR, 0, 0},          {"ZL", kZL, 0, 0},        {"ZR", kZR, 0, 0},
        {"PLUS", kPlus, 0, 0},    {"MINUS", kMinus, 0, 0},  {"LEFT", kLeft, -kStickMax, 0},
        {"RIGHT", kRight, kStickMax, 0}, {"UP", kUp, 0, kStickMax}, {"DOWN", kDown, 0, -kStickMax},
    };
    std::vector<Press> v;
    std::string all = s;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(';', pos);
        if (end == std::string::npos) end = all.size();
        std::string item = all.substr(pos, end - pos);
        pos = end + 1;
        size_t a = item.find(':'), b = item.find(':', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        Press p;
        p.at = std::atof(item.substr(0, a).c_str());
        p.hold = std::atof(item.substr(b + 1).c_str());
        std::string button = item.substr(a + 1, b - a - 1);
        for (const Name& n : names)
            if (button == n.name) p.bits = n.bits, p.lx = n.lx, p.ly = n.ly;
        if (p.bits) v.push_back(p);
    }
    return v;
}

// The recorder: per button, when it went down; a release writes "<down>:<name>:<held>;" (flushed at once, so a
// killed app keeps what was played).
std::mutex g_rec_mu;
std::atomic<bool> g_recording{false};
std::atomic<bool> g_rec_env_init{false};
FILE* g_rec_file = nullptr;
uint64_t g_rec_start_ns = 0;
double g_rec_down[14] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

std::atomic<bool> g_high_fps_interp{true};
std::atomic<bool> g_high_fps_env_init{false};

void init_rec_env() {
    if (g_rec_env_init.exchange(true)) return;
    const char* e = std::getenv("NX_INPUT_RECORD");
    if (e && std::strcmp(e, "1") == 0) {
        std::lock_guard lk(g_rec_mu);
        if (!g_rec_file) {
            for (double& d : g_rec_down) d = -1;
            g_rec_start_ns = monotonic_ns();
            g_rec_file = fopen_utf8(data_root() + "/input_record.txt", "w");
        }
        g_recording.store(g_rec_file != nullptr);
    }
}

void record(const PadState& p, uint64_t now_ns) {
    static const char* names[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "RIGHT", "UP", "DOWN"};
    static const uint64_t bits[] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kLeft, kRight, kUp, kDown};
    std::lock_guard lk(g_rec_mu);
    if (!g_recording.load() || !g_rec_file) return;
    double t = double(now_ns - g_rec_start_ns) / 1e9;
    constexpr int32_t half = kStickMax / 2;
    uint64_t now = p.buttons;
    if (p.lx < -half) now |= kLeft;
    if (p.lx > half) now |= kRight;
    if (p.ly > half) now |= kUp;
    if (p.ly < -half) now |= kDown;
    for (int k = 0; k < 14; k++) {
        bool held = now & bits[k];
        if (held && g_rec_down[k] < 0) g_rec_down[k] = t;
        if (!held && g_rec_down[k] >= 0) {
            std::fprintf(g_rec_file, "%.2f:%s:%.2f;", g_rec_down[k], names[k], std::max(0.05, t - g_rec_down[k]));
            std::fflush(g_rec_file);
            g_rec_down[k] = -1;
        }
    }
}

}  // namespace

void set_input_recording(bool on) {
    init_rec_env();
    std::lock_guard lk(g_rec_mu);
    if (on == g_recording.load()) return;
    if (on) {
        for (double& d : g_rec_down) d = -1;
        g_rec_start_ns = monotonic_ns();
        if (g_rec_file) {
            std::fclose(g_rec_file);
            g_rec_file = nullptr;
        }
        g_rec_file = fopen_utf8(data_root() + "/input_record.txt", "w");
        g_recording.store(g_rec_file != nullptr);
    } else {
        g_recording.store(false);
        for (double& d : g_rec_down) d = -1;
        if (g_rec_file) {
            std::fflush(g_rec_file);
            std::fclose(g_rec_file);
            g_rec_file = nullptr;
        }
    }
}

bool input_recording() {
    init_rec_env();
    return g_recording.load();
}

void set_high_fps_interp(bool on) {
    g_high_fps_env_init.store(true);
    g_high_fps_interp.store(on);
}

bool high_fps_interp() {
    if (!g_high_fps_env_init.exchange(true)) {
        const char* e = std::getenv("NX_SM3DW_HIGH_FPS");
        if (e && e[0] == '0') g_high_fps_interp.store(false);
    }
    return g_high_fps_interp.load();
}

void input_script(PadState& p) {
    static const char* env = std::getenv("NX_INPUT_SCRIPT");
    init_rec_env();
    const bool recording = g_recording.load();
    if (!env && !recording) return;
    const uint64_t now_ns = monotonic_ns();
    if (recording) record(p, now_ns);  // what the player did, before the script adds to it
    if (!env) return;
    static const uint64_t start = monotonic_ns();
    double t = double(now_ns - start) / 1e9;
    static const std::vector<Press> script = parse(env);
    for (const Press& s : script)
        if (t >= s.at && t < s.at + s.hold) {
            p.buttons |= s.bits;
            if (s.lx) p.lx = s.lx;
            if (s.ly) p.ly = s.ly;
        }
}

}  // namespace nx::plat
