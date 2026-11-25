// A DualSense's voice coils for the game's HD rumble, through RenHDrum (renhdrum.dll beside the game, loaded at run
// time). See dualsense_haptics.h. The DLL's own DSP turns the game's amplitude and frequency per band into samples,
// which go to the controller's four-channel sound card over its cable (channels 3 and 4 are the voice coils).
#include "dualsense_haptics.h"

#include <SDL3/SDL.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern "C" void rt_log(const char* fmt, ...);  // platform_win.cpp

namespace nx::dualsense {
namespace {

struct Dsp;  // RenHdcDualSenseDsp, opaque

// RenHDrum's C ABI (include/renhdrum.h): any 0.5.x keeps it
constexpr uint32_t kAbi = 0x000500;
constexpr float kFreqLow = 160.0f, kFreqHigh = 320.0f;  // the game's own when it sends none
constexpr int64_t kStaleMs = 250;                       // a side the game stopped sending to goes silent
constexpr int64_t kIdleCloseMs = 30000;                 // the controller is let go of after this long quiet
constexpr int64_t kRetryMs = 3000;

struct Api {
    uint32_t (*version)();
    Dsp* (*create)(int32_t volume, bool crisp);
    void (*destroy)(Dsp*);
    int32_t (*set_half)(Dsp*, bool right, float al, float fl, float ah, float fh);
    int32_t (*fill)(Dsp*, float* out, size_t frames);
};
Api g_api{};
bool g_loaded = false;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_DUALSENSE_HAPTICS");
        return !(e && e[0] == '0');
    }();
    return on;
}

// renhdrum.dll from beside the game, by its full path (no search of PATH or the working folder)
bool load_library() {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) return false;
    std::wstring path(exe, n);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"renhdrum.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        rt_log("vibration: renhdrum.dll is not beside the game: the DualSense plays the plain rumble");
        return false;
    }
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        rt_log("vibration: renhdrum.dll would not load (error %lu)", GetLastError());
        return false;
    }
    auto get = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(m, name)); };
    g_api.version = reinterpret_cast<decltype(Api::version)>(get("renhdrum_version"));
    g_api.create = reinterpret_cast<decltype(Api::create)>(get("renhdrum_dualsense_dsp_create"));
    g_api.destroy = reinterpret_cast<decltype(Api::destroy)>(get("renhdrum_dualsense_dsp_destroy"));
    g_api.set_half = reinterpret_cast<decltype(Api::set_half)>(get("renhdrum_dualsense_dsp_set_half_target"));
    g_api.fill = reinterpret_cast<decltype(Api::fill)>(get("renhdrum_dualsense_dsp_fill_f32"));
    if (!g_api.version || !g_api.create || !g_api.destroy || !g_api.set_half || !g_api.fill) {
        rt_log("vibration: renhdrum.dll lacks the DualSense functions: the plain rumble");
        FreeLibrary(m);
        return false;
    }
    const uint32_t v = g_api.version();
    if ((v & 0xFFFF00u) != (kAbi & 0xFFFF00u)) {
        rt_log("vibration: renhdrum.dll is version %u.%u.%u, this build wants %u.%u: the plain rumble", (v >> 16) & 0xFF,
               (v >> 8) & 0xFF, v & 0xFF, (kAbi >> 16) & 0xFF, (kAbi >> 8) & 0xFF);
        FreeLibrary(m);
        return false;
    }
    // (no Switch 2 mode: Super Mario Bros. Wonder is a Switch 1 game, which is RenHDrum's default)
    rt_log("vibration: RenHDrum %u.%u.%u loaded", (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

// ---- what the game sends (g_lk), and what plays it ------------------------------------------------------------------
std::mutex g_lk;
Dsp* g_dsp = nullptr;                        // (g_lk) the open controller's DSP
uint32_t g_want_id = 0;                      // (g_lk) the controller the game's values are for
bool g_want_open = false;                    // (g_lk) a value above zero was sent
int g_strength = 100;                        // (g_lk) the player's rumble setting, percent
int64_t g_last_loud = 0;                     // (g_lk) when a value above zero was last sent
int64_t g_at[2] = {0, 0};                    // (g_lk) each side's last value (0: none yet)
bool g_quiet[2] = {true, true};              // (g_lk) the side was silenced for want of values
std::atomic<uint32_t> g_owned_id{0};         // the controller whose voice coils are ours
std::atomic<int> g_gain_percent{100};        // the USB samples' gain, after the DSP

bool g_thread_started = false;

void feed(int side, float al, float fl, float ah, float fh) {  // (g_lk held)
    if (g_dsp) g_api.set_half(g_dsp, side == 1, al, fl, ah, fh);
}

// ---- over a cable: the controller's sound card ---------------------------------------------------------------------
SDL_AudioStream* g_stream = nullptr;

void SDLCALL pull(void*, SDL_AudioStream* stream, int additional, int) {
    constexpr int kChunk = 256;
    float buf[kChunk * 4];
    int frames = additional / int(4 * sizeof(float));
    const float gain = float(g_gain_percent.load(std::memory_order_relaxed)) / 100.0f;
    while (frames > 0) {
        const int n = std::min(frames, kChunk);
        bool filled = false;
        {
            std::lock_guard lk(g_lk);
            if (g_dsp) filled = g_api.fill(g_dsp, buf, size_t(n)) >= 0;
        }
        if (!filled) {
            std::memset(buf, 0, size_t(n) * 4 * sizeof(float));
        } else {
            // the strength once, on the samples the DSP made: its response curve stays its own
            for (int i = 0; i < n; ++i)
                for (int ch = 1; ch < 4; ++ch) {
                    const float v = buf[i * 4 + ch] * gain;
                    buf[i * 4 + ch] = std::clamp(v, -1.0f, 1.0f);
                }
        }
        SDL_PutAudioStreamData(stream, buf, n * 4 * int(sizeof(float)));
        frames -= n;
    }
}

bool usb_open() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("vibration: SDL audio unavailable: %s", SDL_GetError());
        return false;
    }
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found = 0;
    int named = 0;
    for (int i = 0; devices && i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        if (!name || (!std::strstr(name, "DualSense") && !std::strstr(name, "Wireless Controller"))) continue;
        ++named;
        SDL_AudioSpec spec;
        int frames = 0;
        if (!found && SDL_GetAudioDeviceFormat(devices[i], &spec, &frames) && spec.channels >= 4) found = devices[i];
    }
    SDL_free(devices);
    if (!found) {
        rt_log("vibration: %d playback devices named for a DualSense, none with four channels: the plain rumble", named);
        return false;
    }
    SDL_AudioSpec want{};
    want.format = SDL_AUDIO_F32;
    want.channels = 4;
    want.freq = 48000;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");  // ~10 ms: the vibration is not to lag
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(found, &want, pull, nullptr);
    SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    if (!stream) {
        rt_log("vibration: the DualSense's sound card would not open: %s", SDL_GetError());
        return false;
    }
    g_stream = stream;
    SDL_ResumeAudioStreamDevice(stream);
    rt_log("vibration: the DualSense's voice coils on its cable (48 kHz, four channels)");
    return true;
}

void usb_close() {
    SDL_AudioStream* stream = g_stream;
    g_stream = nullptr;
    if (stream) SDL_DestroyAudioStream(stream);  // (waits for the callback, which takes g_lk: not held here)
}

// ---- the worker -------------------------------------------------------------------------------------------------------
struct Open {
    uint32_t id = 0;
    bool is_open = false;
} g_open;

void close_all() {
    g_owned_id.store(0);
    usb_close();
    Dsp* dsp;
    {
        std::lock_guard lk(g_lk);
        dsp = g_dsp;
        g_dsp = nullptr;
    }
    if (dsp) g_api.destroy(dsp);
    g_open = {};
}

void worker() {
    SetThreadDescription(GetCurrentThread(), L"dualsense-haptics");
    int64_t retry_at = 0;
    for (;;) {
        const int64_t ms = now_ms();
        uint32_t id;
        bool want_open;
        int strength;
        {
            std::lock_guard lk(g_lk);
            // a side the game stopped sending to: the DSP would hold its last value
            for (int s = 0; s < 2; ++s)
                if (g_at[s] && !g_quiet[s] && ms - g_at[s] > kStaleMs) feed(s, 0.0f, kFreqLow, 0.0f, kFreqHigh), g_quiet[s] = true;
            id = g_want_id, want_open = g_want_open, strength = g_strength;
            if (g_open.is_open && g_last_loud && ms - g_last_loud > kIdleCloseMs) g_want_open = want_open = false;
        }
        if (g_open.is_open && (!want_open || g_open.id != id)) close_all();
        if (want_open && !g_open.is_open && ms >= retry_at && id) {
            Dsp* dsp = g_api.create(100, false);  // (its full gain: the strength is ours, after the DSP)
            if (dsp) {
                {
                    std::lock_guard lk(g_lk);
                    g_dsp = dsp;
                }
                if (usb_open()) {
                    g_open = {id, true};
                    g_owned_id.store(id);
                } else {
                    close_all();
                    retry_at = ms + kRetryMs;
                }
            } else {
                retry_at = ms + kRetryMs;
            }
        }
        if (g_open.is_open) g_gain_percent.store(std::clamp(strength, 0, 100));
        Sleep(10);
    }
}

}  // namespace

void play(uint32_t sdl_id, const float left[4], const float right[4], int strength) {
    if (!enabled() || !sdl_id) return;
    std::lock_guard lk(g_lk);
    if (!g_thread_started) {
        g_thread_started = true;
        g_loaded = load_library();
        if (g_loaded) std::thread(worker).detach();
    }
    if (!g_loaded) return;
    g_want_id = sdl_id;
    g_strength = strength;
    const int64_t ms = now_ms();
    const float* sides[2] = {left, right};
    for (int s = 0; s < 2; ++s) {
        const float* v = sides[s];
        const float al = v[0] > 0.0f ? std::min(v[0], 1.0f) : 0.0f;  // (also a NaN: 0)
        const float ah = v[2] > 0.0f ? std::min(v[2], 1.0f) : 0.0f;
        const float fl = v[1] > 0.0f ? v[1] : kFreqLow, fh = v[3] > 0.0f ? v[3] : kFreqHigh;
        const bool zero = strength <= 0;
        feed(s, zero ? 0.0f : al, fl, zero ? 0.0f : ah, fh);
        g_at[s] = ms, g_quiet[s] = false;
        if (!zero && (al > 0.0f || ah > 0.0f)) g_last_loud = ms, g_want_open = true;
    }
}

bool owns(uint32_t sdl_id) { return sdl_id && g_owned_id.load(std::memory_order_acquire) == sdl_id; }

}  // namespace nx::dualsense
