// Super Mario 3D World + Bowser's Fury (010028600EBDA000, 1.0.0 / 1.2.1): game-specific hooks for Super3D.
//
// High-FPS support (90 / 120 / 144 / 240 FPS) without changing game speed or physics:
// SM3DW's EAD Tokyo `al::` / `sead::` engine is fixed-timestep 60 Hz. Rather than speeding up the
// simulation, we decouple logic from rendering using a 60 Hz fixed-timestep accumulator:
//   - Game simulation (`GameSystem::movement`) and controller polling (`sead::ControllerMgr::calc`)
//     run at exact 60 Hz so gravity, jump arcs, timers, enemies, and audio remain 100% original.
//   - Rendering (`procDraw_` / `drawMain`) runs at the chosen FPS cap (90/120/144/240 Hz), replaying
//     `al::LiveActorKit`'s per-frame graphics bookkeeping on draw-only frames.
//   - Camera (`sead::LookAtCamera`) and 3D model skeletons (`nn::g3d::SkeletonObj` world matrices)
//     are linearly interpolated between the previous and current 60 Hz logic ticks by `alpha`, and
//     restored immediately after rendering so game logic never observes interpolated values.
#include <nx/game.h>
#include <nx/gamelib.h>
#include <nx/module.h>
#include <nx/runtime.h>
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace nx {

namespace nvn {
int frame_cap();
}

namespace {

// Verified main.bin (1.0.0) offsets:
//   al::GameFrameworkNx::procFrame_        : 0x964e80 (virtual, vtable +0xe8)
//   sead::ControllerMgr::calc              : 0x72fff0 (virtual, vtable +0xb0)
//   GameSystem::movement                   : 0x412f00 (virtual, vtable +0x20)
//   al::ModelDrawer*::draw                 : 11 virtual draw methods (all call ModelDrawer::isDraw 0x870510)
//   al::LiveActorKit::clearGraphicsRequest : 0x82c080
//   al::LiveActorKit::updateGraphics       : 0x82c090
//   al::LiveActorKit::preDrawGraphics      : 0x82c0e0
//   nn::g3d::ModelObj::CalculateSkeleton   : 0x6d4e50
//   nn::g3d::ModelObj::CalculateShape      : 0x6d5000
constexpr uint64_t kOffProcFrame = 0x964e80;
constexpr uint64_t kOffControllerMgrCalc = 0x72fff0;
constexpr uint64_t kOffGameSystemMovement = 0x412f00;
constexpr uint64_t kOffModelDrawerDraws[] = {
    0x86ead0, 0x870a20, 0x870d00, 0x870fe0, 0x871180, 0x8717c0,
    0x871c60, 0x872180, 0x8724e0, 0x9755b0, 0x975ba0,
};
constexpr uint64_t kOffLiveActorKitClearGraphics = 0x82c080;
constexpr uint64_t kOffLiveActorKitUpdateGraphics = 0x82c090;
constexpr uint64_t kOffLiveActorKitPreDrawGraphics = 0x82c0e0;
constexpr uint64_t kOffModelCalcSkeleton = 0x6d4e50;
constexpr uint64_t kOffModelCalcShape = 0x6d5000;

constexpr uint64_t kTickNs = 16'666'667ull;  // 60 Hz logic tick
constexpr float kTeleportDistSq = 1000.0f * 1000.0f;  // 10 m in SM3DW world units (100 units = 1 m)
constexpr float kCameraCutMinDot = 0.8660254f;        // cos(30 deg)

uint64_t g_main_base = 0;
Fn g_orig_proc_frame = nullptr;
Fn g_orig_controller_calc = nullptr;
Fn g_orig_game_system_movement = nullptr;
Fn g_orig_model_drawer_draws[std::size(kOffModelDrawerDraws)]{};
Fn g_fn_clear_graphics = nullptr;
Fn g_fn_update_graphics = nullptr;
Fn g_fn_pre_draw_graphics = nullptr;
Fn g_fn_model_calc_skeleton = nullptr;
Fn g_fn_model_calc_shape = nullptr;

bool g_hooks_installed = false;
bool g_run_calc = true;
float g_alpha = 1.0f;
uint32_t g_logic_tick = 1;
uint64_t g_last_scene = 0;
uint64_t g_last_ticked_kit = 0;

uint64_t g_prev_ns = 0;
uint64_t g_accum_ns = 0;

uint64_t g_report_ns = 0;
uint64_t g_report_frames = 0;
uint64_t g_report_ticks = 0;

// ---- Math & Camera Interpolation ----------------------------------------------------------------

struct Vec3 {
    float x, y, z;
};

static inline Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline float vdot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 vcross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline Vec3 vnorm(Vec3 v) {
    const float len = std::sqrt(vdot(v, v));
    return len > 1e-6f ? Vec3{v.x / len, v.y / len, v.z / len} : v;
}
static inline Vec3 vlerp(Vec3 a, Vec3 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// sead::LookAtCamera layout:
//   +0x00: vtable*
//   +0x08: float matrix[3][4] (row-major 3x4 view matrix)
//   +0x38: Vec3 pos
//   +0x44: Vec3 at
//   +0x50: Vec3 up
struct CameraSlot {
    uint64_t cam_addr = 0;
    bool valid = false;
    bool applied = false;
    Vec3 prev_pos{}, prev_at{}, prev_up{};
    Vec3 curr_pos{}, curr_at{}, curr_up{};
    uint8_t saved_state[0x5c - 0x08]{};
};

CameraSlot g_cam_slots[2];

void write_lookat_matrix(uint64_t cam_addr, Vec3 pos, Vec3 at, Vec3 up) {
    const Vec3 z = vnorm(vsub(pos, at));
    const Vec3 x = vnorm(vcross(up, z));
    const Vec3 y = vcross(z, x);
    float m[3][4] = {
        {x.x, x.y, x.z, -vdot(x, pos)},
        {y.x, y.y, y.z, -vdot(y, pos)},
        {z.x, z.y, z.z, -vdot(z, pos)},
    };
    std::memcpy(reinterpret_cast<void*>(cam_addr + 0x08), m, sizeof m);
    std::memcpy(reinterpret_cast<void*>(cam_addr + 0x38), &pos, sizeof pos);
    std::memcpy(reinterpret_cast<void*>(cam_addr + 0x44), &at, sizeof at);
    std::memcpy(reinterpret_cast<void*>(cam_addr + 0x50), &up, sizeof up);
}

void camera_restore() {
    for (CameraSlot& s : g_cam_slots) {
        if (s.applied && s.cam_addr) {
            std::memcpy(reinterpret_cast<void*>(s.cam_addr + 0x08), s.saved_state, sizeof s.saved_state);
            s.applied = false;
        }
    }
}

void camera_reset() {
    camera_restore();
    for (CameraSlot& s : g_cam_slots) s = CameraSlot{};
}

uint64_t get_scene_camera_info(uint64_t kit) {
    if (!kit) return 0;
    const uint64_t cam_dir = ld<uint64_t>(kit + 0x28);
    return cam_dir ? ld<uint64_t>(cam_dir + 0x28) : 0;
}

void camera_capture(uint64_t kit) {
    const uint64_t info = get_scene_camera_info(kit);
    if (!info) return;
    for (int i = 0; i < 2; ++i) {
        const uint64_t cam = ld<uint64_t>(info + 0x20 + uint64_t(i) * 8);
        CameraSlot& s = g_cam_slots[i];
        if (!cam) {
            s = CameraSlot{};
            continue;
        }
        Vec3 pos, at, up;
        std::memcpy(&pos, reinterpret_cast<const void*>(cam + 0x38), sizeof pos);
        std::memcpy(&at, reinterpret_cast<const void*>(cam + 0x44), sizeof at);
        std::memcpy(&up, reinterpret_cast<const void*>(cam + 0x50), sizeof up);

        if (s.valid && s.cam_addr == cam) {
            s.prev_pos = s.curr_pos;
            s.prev_at = s.curr_at;
            s.prev_up = s.curr_up;
        } else {
            s.prev_pos = pos;
            s.prev_at = at;
            s.prev_up = up;
        }
        s.cam_addr = cam;
        s.curr_pos = pos;
        s.curr_at = at;
        s.curr_up = up;
        s.valid = true;
    }
}

void camera_apply(uint64_t kit, float alpha) {
    if (alpha >= 0.999f) return;
    const uint64_t info = get_scene_camera_info(kit);
    if (!info) return;
    for (int i = 0; i < 2; ++i) {
        const uint64_t cam = ld<uint64_t>(info + 0x20 + uint64_t(i) * 8);
        CameraSlot& s = g_cam_slots[i];
        if (!cam || !s.valid || s.cam_addr != cam) continue;

        // Detect camera cut / warp: snap instead of interpolating across a discontinuity.
        const Vec3 dp = vsub(s.curr_pos, s.prev_pos);
        if (vdot(dp, dp) > kTeleportDistSq) continue;
        const Vec3 dir_prev = vnorm(vsub(s.prev_at, s.prev_pos));
        const Vec3 dir_curr = vnorm(vsub(s.curr_at, s.curr_pos));
        if (vdot(dir_prev, dir_curr) < kCameraCutMinDot) continue;

        if (!s.applied) {
            std::memcpy(s.saved_state, reinterpret_cast<const void*>(cam + 0x08), sizeof s.saved_state);
            s.applied = true;
        }
        const Vec3 pos = vlerp(s.prev_pos, s.curr_pos, alpha);
        const Vec3 at = vlerp(s.prev_at, s.curr_at, alpha);
        const Vec3 up = vnorm(vlerp(s.prev_up, s.curr_up, alpha));
        write_lookat_matrix(cam, pos, at, up);
    }
}

// ---- 3D Skeleton / Model Interpolation ----------------------------------------------------------

struct SkelHistory {
    uint32_t tick = 0;
    uint32_t blended_frame = 0;
    uint32_t bones = 0;
    bool has_prev = false;
    bool moving = false;
    bool applied = false;
    uint64_t world_addr = 0;
    std::vector<float> prev;
    std::vector<float> curr;
    std::vector<float> saved;
};

std::unordered_map<uint64_t, SkelHistory> g_skel_history;
std::vector<SkelHistory*> g_blended_skels;
uint32_t g_render_frame = 1;
uint64_t g_prof_move_ns = 0;
uint64_t g_prof_draw_ns = 0;
uint64_t g_prof_interp_ns = 0;

void models_restore() {
    for (SkelHistory* h : g_blended_skels) {
        if (h->applied && h->world_addr) {
            std::memcpy(reinterpret_cast<void*>(h->world_addr), h->saved.data(), h->saved.size() * sizeof(float));
            h->applied = false;
        }
    }
    g_blended_skels.clear();
}

void models_reset() {
    models_restore();
    g_skel_history.clear();
}

void interpolate_model_keeper(CPU& c, uint64_t keeper) {
    if (!keeper) return;
    const uint64_t ctrl = ld<uint64_t>(keeper + 0x18);
    if (!ctrl) return;
    const uint64_t model_obj = ld<uint64_t>(ctrl + 0x08);
    const uint64_t buf_idx_ptr = ld<uint64_t>(ctrl + 0x38);
    if (!model_obj || !buf_idx_ptr) return;
    const uint64_t skel = ld<uint64_t>(model_obj + 0x38);
    if (!skel) return;
    const uint64_t res_skel = ld<uint64_t>(skel);
    const uint64_t world_addr = ld<uint64_t>(skel + 0x20);
    if (!res_skel || !world_addr) return;
    const uint32_t bones = ld<uint16_t>(res_skel + 0x38);
    if (bones == 0 || bones > 512) return;

    const size_t floats = size_t(bones) * 16;
    const float* world = reinterpret_cast<const float*>(world_addr);
    SkelHistory& h = g_skel_history[skel];

    if (h.tick != g_logic_tick) {
        const bool continuous = (h.tick != 0 && h.tick + 1 == g_logic_tick && h.bones == bones);
        if (h.bones != bones) {
            h.prev.resize(floats);
            h.curr.resize(floats);
            h.saved.resize(floats);
            h.bones = bones;
        }
        if (continuous) {
            h.prev.swap(h.curr);
        }
        std::memcpy(h.curr.data(), world, floats * sizeof(float));
        if (!continuous) {
            std::memcpy(h.prev.data(), world, floats * sizeof(float));
            h.has_prev = false;
        } else {
            // Root bone translation is column 3 (indices 12, 13, 14).
            const float dx = h.curr[12] - h.prev[12];
            const float dy = h.curr[13] - h.prev[13];
            const float dz = h.curr[14] - h.prev[14];
            h.has_prev = (dx * dx + dy * dy + dz * dz) <= kTeleportDistSq;
            bool changed = false;
            if (h.has_prev) {
                const float* p = h.prev.data();
                const float* q = h.curr.data();
                for (size_t k = 0; k < floats; ++k) {
                    if (std::fabs(q[k] - p[k]) > 1e-5f) { changed = true; break; }
                }
            }
            h.moving = changed;
        }
        if (!continuous) h.moving = false;
        h.tick = g_logic_tick;
    }

    if (h.moving && g_alpha < 0.999f && h.blended_frame != g_render_frame) {
        float* dst = reinterpret_cast<float*>(world_addr);
        if (!h.applied) {
            std::memcpy(h.saved.data(), dst, floats * sizeof(float));
            h.world_addr = world_addr;
            h.applied = true;
            g_blended_skels.push_back(&h);
        }
        const float a = g_alpha;
        const float* p = h.prev.data();
        const float* q = h.curr.data();
        for (size_t k = 0; k < floats; ++k) dst[k] = p[k] + (q[k] - p[k]) * a;

        const uint32_t buf_idx = ld<uint32_t>(buf_idx_ptr);
        c.x[0] = model_obj;
        c.x[1] = buf_idx;
        g_fn_model_calc_skeleton(c);
        c.x[0] = model_obj;
        c.x[1] = buf_idx;
        g_fn_model_calc_shape(c);
        h.blended_frame = g_render_frame;
    }
}

void interpolate_model_drawer(CPU& c, uint64_t drawer) {
    if (!drawer || ld<int32_t>(drawer + 0x24) <= 0) return;

    const int cap = nvn::frame_cap();
    static const bool env_on = [] {
        const char* e = std::getenv("NX_SM3DW_HIGH_FPS");
        return !(e && e[0] == '0');
    }();
    if (!env_on || !plat::high_fps_interp() || cap <= 60 || !g_fn_model_calc_skeleton || !g_fn_model_calc_shape) {
        return;
    }

    // al::ModelDrawer layout:
    //   drawer + 0x24 -> int32_t m_cnt, + 0x28 -> ModelKeeper** items
    //   drawer + 0x30 -> ModelGroupArray { int32_t count; pad; ModelGroup** items; }
    //   ModelGroup + 0x48 -> int32_t modelCount, + 0x50 -> ModelKeeper** items
    const uint64_t t_interp0 = plat::monotonic_ns();
    const uint64_t saved_x0 = c.x[0], saved_x1 = c.x[1];

    const int32_t direct_cnt = ld<int32_t>(drawer + 0x24);
    const uint64_t direct_items = ld<uint64_t>(drawer + 0x28);
    if (direct_cnt > 0 && direct_cnt <= 4096 && direct_items) {
        for (int32_t m = 0; m < direct_cnt; ++m) {
            interpolate_model_keeper(c, ld<uint64_t>(direct_items + uint64_t(m) * 8));
        }
    }

    const uint64_t grp_arr = ld<uint64_t>(drawer + 0x30);
    const int32_t grp_cnt = grp_arr ? ld<int32_t>(grp_arr) : 0;
    const uint64_t grp_items = grp_arr ? ld<uint64_t>(grp_arr + 8) : 0;

    if (grp_cnt > 0 && grp_cnt <= 256 && grp_items) {
        for (int32_t g = 0; g < grp_cnt; ++g) {
            const uint64_t grp = ld<uint64_t>(grp_items + uint64_t(g) * 8);
            if (!grp) continue;
            const int32_t m_cnt = ld<int32_t>(grp + 0x48);
            const uint64_t m_items = ld<uint64_t>(grp + 0x50);
            if (m_cnt <= 0 || m_cnt > 4096 || !m_items) continue;

            for (int32_t m = 0; m < m_cnt; ++m) {
                interpolate_model_keeper(c, ld<uint64_t>(m_items + uint64_t(m) * 8));
            }
        }
    }
    c.x[0] = saved_x0;
    c.x[1] = saved_x1;
    g_prof_interp_ns += plat::monotonic_ns() - t_interp0;
}

template <size_t I>
void hook_model_drawer_draw(CPU& c) {
    interpolate_model_drawer(c, c.x[0]);
    if (g_orig_model_drawer_draws[I]) g_orig_model_drawer_draws[I](c);
}

constexpr Fn kModelDrawerHooks[] = {
    &hook_model_drawer_draw<0>, &hook_model_drawer_draw<1>, &hook_model_drawer_draw<2>,
    &hook_model_drawer_draw<3>, &hook_model_drawer_draw<4>, &hook_model_drawer_draw<5>,
    &hook_model_drawer_draw<6>, &hook_model_drawer_draw<7>, &hook_model_drawer_draw<8>,
    &hook_model_drawer_draw<9>, &hook_model_drawer_draw<10>,
};

// ---- Core 60 Hz Fixed-Timestep Loop Hooks -------------------------------------------------------

bool high_fps_interp_enabled() {
    return plat::high_fps_interp() && nvn::frame_cap() > 60;
}

void hook_proc_frame(CPU& c) {
    const uint64_t now = plat::monotonic_ns();
    if (!high_fps_interp_enabled()) {
        if (g_last_scene) {
            camera_reset();
            models_reset();
            g_last_scene = 0;
            g_last_ticked_kit = 0;
        }
        g_run_calc = true;
        g_alpha = 1.0f;
        g_prev_ns = now;
        g_accum_ns = 0;
        ++g_render_frame;
    const uint64_t t_proc0 = plat::monotonic_ns();
    g_orig_proc_frame(c);
    g_prof_draw_ns += plat::monotonic_ns() - t_proc0;
        return;
    }

    if (!g_prev_ns) {
        g_prev_ns = now;
        g_report_ns = now;
        g_run_calc = true;
        g_alpha = 1.0f;
    } else {
        uint64_t dt = now > g_prev_ns ? now - g_prev_ns : 0;
        g_prev_ns = now;
        // Clamp large hitches/pauses so we never burst multiple ticks or corrupt the accumulator.
        if (dt > 100'000'000ull) dt = kTickNs;
        g_accum_ns += dt;
        if (g_accum_ns >= kTickNs) {
            g_accum_ns -= kTickNs;
            if (g_accum_ns >= 2 * kTickNs) g_accum_ns = kTickNs;
            g_run_calc = true;
            ++g_logic_tick;
        } else {
            g_run_calc = false;
        }
        g_alpha = float(double(g_accum_ns) / double(kTickNs));
    }

    ++g_render_frame;
    const uint64_t t_proc0 = plat::monotonic_ns();
    g_orig_proc_frame(c);
    g_prof_draw_ns += plat::monotonic_ns() - t_proc0;

    ++g_report_frames;
    if (g_run_calc) ++g_report_ticks;
    static uint64_t speed_ns = 0;
    static uint32_t speed_ticks = 0;
    if (!speed_ns) speed_ns = now;
    if (g_run_calc) ++speed_ticks;
    if (now - speed_ns >= 1'000'000'000ull) {
        const double sec = double(now - speed_ns) / 1e9;
        plat::overlay_game_speed(double(speed_ticks) / sec / 60.0 * 100.0);
        speed_ns = now;
        speed_ticks = 0;
    }
    if (now - g_report_ns >= 5'000'000'000ull) {
        const double sec = double(now - g_report_ns) / 1e9;
        const double render_fps = double(g_report_frames) / sec;
        const double logic_hz = double(g_report_ticks) / sec;
        log("sm3dw: high-fps render=%.1f fps, logic=%.1f Hz (cap=%d, proc=%.2fms, move=%.2fms, interp=%.2fms)",
            render_fps, logic_hz, nvn::frame_cap(),
            g_report_frames ? double(g_prof_draw_ns) / 1e6 / double(g_report_frames) : 0.0,
            g_report_ticks ? double(g_prof_move_ns) / 1e6 / double(g_report_ticks) : 0.0,
            g_report_frames ? double(g_prof_interp_ns) / 1e6 / double(g_report_frames) : 0.0);
        g_prof_draw_ns = 0;
        g_prof_move_ns = 0;
        g_prof_interp_ns = 0;
        g_report_ns = now;
        g_report_frames = 0;
        g_report_ticks = 0;
    }
}

void hook_controller_calc(CPU& c) {
    if (g_run_calc) g_orig_controller_calc(c);
}

void run_pre_draw_graphics(CPU& c, uint64_t kit) {
    if (!g_fn_pre_draw_graphics || !kit) return;
    const uint64_t gfx = ld<uint64_t>(kit + 0x20);
    const uint64_t light_keeper = gfx ? ld<uint64_t>(gfx + 0xb8) : 0;
    const uint64_t light_impl = light_keeper ? ld<uint64_t>(light_keeper + 0x8) : 0;
    const uint32_t saved_light_idx = light_impl ? ld<uint32_t>(light_impl + 0x174c) : 0;

    const uint64_t saved_x0 = c.x[0];
    c.x[0] = kit;
    g_fn_pre_draw_graphics(c);
    c.x[0] = saved_x0;

    if (light_impl) st<uint32_t>(light_impl + 0x174c, saved_light_idx);
}

void hook_game_system_movement(CPU& c) {
    // Always restore true camera and skeleton state before simulation or graphics replay.
    camera_restore();
    models_restore();

    const uint64_t game_sys = c.x[0];
    const uint64_t seq = game_sys ? ld<uint64_t>(game_sys + 0x10) : 0;
    const uint64_t scene = seq ? ld<uint64_t>(seq + 0x78) : 0;
    const bool scene_alive = scene && ld<uint8_t>(scene + 0x80) != 0;
    const uint64_t kit = scene_alive ? ld<uint64_t>(scene + 0x90) : 0;

    if (scene != g_last_scene) {
        camera_reset();
        models_reset();
        g_last_scene = scene;
        g_last_ticked_kit = 0;
    }

    // Never run a draw-only frame on a newly created/entered scene before its first logic tick:
    // static shadows, PrePassLightKeeper, and cameras are only initialized on the first movement tick.
    if (!g_run_calc && kit && kit != g_last_ticked_kit) {
        g_run_calc = true;
        g_accum_ns = 0;
        g_alpha = 1.0f;
    }

    if (g_run_calc) {
        const uint64_t t_m0 = plat::monotonic_ns();
        g_orig_game_system_movement(c);
        g_prof_move_ns += plat::monotonic_ns() - t_m0;
        if (kit) g_last_ticked_kit = kit;
        if (high_fps_interp_enabled() && kit) {
            camera_capture(kit);
            camera_apply(kit, g_alpha);
            // Re-run preDrawGraphics so GPU camera/view UBOs match g_alpha instead of staying at alpha=1.0.
            if (g_alpha < 0.999f) run_pre_draw_graphics(c, kit);
        }
        return;
    }

    if (!kit) return;

    // Draw-only frame: apply interpolated camera, then run preDrawGraphics (so DynamicTextureAllocator
    // frees stay balanced) while preserving PrePassLightKeeper->impl+0x174c. Skip updateGraphics
    // so light/shadow/graphics request lists from the last 60 Hz tick are not cleared or rotated.
    camera_apply(kit, g_alpha);
    run_pre_draw_graphics(c, kit);
}

bool install_hook(const LoadedModule* lm, uint64_t off, Fn hook, Fn& orig) {
    if (!lm || !lm->table || (off & 3) || off >= lm->info->text_size) return false;
    Fn cur = lm->table[off / 4];
    if (!cur) return false;
    orig = cur;
    lm->table[off / 4] = hook;
    return true;
}

void install_high_fps_hooks() {
    const ModuleInfo* main_info = game_lib().main;
    if (!main_info) return;
    const LoadedModule* lm = module_at(main_info->base);
    if (!lm) return;
    g_main_base = lm->base;
    g_fn_clear_graphics = lm->table[kOffLiveActorKitClearGraphics / 4];
    g_fn_update_graphics = lm->table[kOffLiveActorKitUpdateGraphics / 4];
    g_fn_pre_draw_graphics = lm->table[kOffLiveActorKitPreDrawGraphics / 4];
    g_fn_model_calc_skeleton = lm->table[kOffModelCalcSkeleton / 4];
    g_fn_model_calc_shape = lm->table[kOffModelCalcShape / 4];

    const bool ok1 = install_hook(lm, kOffProcFrame, &hook_proc_frame, g_orig_proc_frame);
    const bool ok2 = install_hook(lm, kOffControllerMgrCalc, &hook_controller_calc, g_orig_controller_calc);
    const bool ok3 = install_hook(lm, kOffGameSystemMovement, &hook_game_system_movement, g_orig_game_system_movement);
    bool ok4 = true;
    for (size_t i = 0; i < std::size(kOffModelDrawerDraws); ++i) {
        ok4 &= install_hook(lm, kOffModelDrawerDraws[i], kModelDrawerHooks[i], g_orig_model_drawer_draws[i]);
    }
    g_hooks_installed = ok1 && ok2 && ok3 && ok4;
    log("sm3dw: high-fps hooks %s (procFrame=%d ctrl=%d move=%d model=%d)",
        g_hooks_installed ? "installed" : "FAILED", ok1 ? 1 : 0, ok2 ? 1 : 0, ok3 ? 1 : 0, ok4 ? 1 : 0);
}

}  // namespace

namespace game {

const Info kInfo = {
    "sm3dw",
    "Super Mario 3D World + Bowser's Fury",
    0x010028600EBDA000ull,
    "com.super3d.sm3dw",
    "private/sm3dw",
    0,  // main.npdm META +0x14: system_resource_size = 0
};

void after_init() {
    log("sm3dw: after_init (Super3D)");
    install_high_fps_hooks();
}

void renderer_start() {
    log("sm3dw: renderer_start");
}

void first_frame() {
    log("sm3dw: first_frame presented!");
}

void frame_presented(uint64_t /*now_ns*/, int /*present_interval*/) {}

void file_opened(const char* /*path*/) {}

}  // namespace game

}  // namespace nx
