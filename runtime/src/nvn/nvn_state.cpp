// NVN command buffers (docs/NVN.md, N6): binds, state objects and draws, recorded for nvn_draw.cpp to replay.
//
// State objects are plain structs of the SDK driver; they are decoded here, while recording, through its getters
// (the game reuses its structs right after binding them). Signatures from the game's call sites (nvn_trace.txt):
//   BindProgram(cmd, program, stageBits)            BindUniformBuffer(cmd, stage, index, address, size)
//   BindStorageBuffer(cmd, stage, index, address, size)   BindTexture(cmd, stage, index, handle)
//   BindVertexBuffer(cmd, index, address, size)     BindVertexAttribState(cmd, count, const state*)  (4 bytes each)
//   BindVertexStreamState(cmd, count, const state*) (8 bytes each)   Bind{Blend,Color,...}State(cmd, const state*)
//   UpdateUniformBuffer(cmd, address, size, offset, dataSize, const void* data)
//   DrawArrays(cmd, mode, first, count)             DrawElements(cmd, mode, indexType, count, indexAddress)
//   DrawElementsBaseVertex(..., baseVertex)         DrawElementsInstanced(..., baseVertex, baseInstance, instances)
//   DrawArraysInstanced(cmd, mode, first, count, baseInstance, instances)
// Shader stages are numbered 0 vertex, 1 fragment (the pairs the game binds), 5 compute.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "nvn.h"
#include "kernel.h"
#include "nvn_shaders.h"
#include "platform.h"

namespace nx::nvn {

namespace {

// NX_NVN_STATES=1: log each distinct decoded state once (to check the NVN enum values against what the game uses).
template <typename T>
void log_state(const char* what, const T& s) {
    static const bool on = std::getenv("NX_NVN_STATES") != nullptr;
    if (!on) return;
    static std::mutex lock;
    static std::set<std::string> seen;
    std::string key(reinterpret_cast<const char*>(&s), sizeof s);
    std::lock_guard lk(lock);
    if (!seen.insert(key).second || seen.size() > 200) return;
    std::string words;
    for (size_t k = 0; k + 4 <= sizeof s; k += 4) {
        uint32_t w;
        std::memcpy(&w, reinterpret_cast<const uint8_t*>(&s) + k, 4);
        char b[16];
        std::snprintf(b, sizeof b, " %x", w);
        words += b;
    }
    log("nvn state %s:%s", what, words.c_str());
}

// ---- shaders and resources --------------------------------------------------------------------------------------

void BindProgram(CPU& c) {
    Cmd cmd{Cmd::BindProgram};
    cmd.a[0] = c.x[1];
    cmd.mask = uint32_t(c.x[2]);
    record(c.x[0], cmd);
    static const bool trace = std::getenv("NX_TRACE_PROGRAMS") != nullptr;  // binds of skinned programs (input 6)
    static int logged = 0;
    if (const auto* sh = trace && logged < 40 ? program_shaders(c.x[1]) : nullptr)
        for (const ShaderInfo* s : *sh)
            for (const ShaderIo& in : s->refl.inputs)
                if (s->stage == ShaderStage::Vertex && in.location == 6 && logged++ < 40)
                    log("nvn: skinned program %llx (vs %s) bound into cmd %llx", (unsigned long long)c.x[1],
                        s->hash.c_str(), (unsigned long long)c.x[0]);
}

void BindUniformBuffer(CPU& c) {
    Cmd cmd{Cmd::BindUniformBuffer};
    cmd.i[0] = int32_t(c.x[1]), cmd.i[1] = int32_t(c.x[2]);
    cmd.a[0] = c.x[3], cmd.a[1] = c.x[4];
    static const char* terrain_trigger = std::getenv("NX_TERRAIN_BIND_TRACE");
    static int terrain_logs = 0;
    if (terrain_trigger && terrain_logs < 8 && cmd.i[0] == 0 && cmd.i[1] == 4 && cmd.a[1] == 57408) {
        if (FILE* terrain_file = std::fopen(terrain_trigger, "rb")) {
            std::fclose(terrain_file);
            ++terrain_logs;
            log("terrain bind single: address=%llx size=%llu lr=%s", (unsigned long long)cmd.a[0],
                (unsigned long long)cmd.a[1], describe(c.x[30]).c_str());
            backtrace(c);
        }
    }
    record(c.x[0], cmd);
}

void BindStorageBuffer(CPU& c) {
    Cmd cmd{Cmd::BindStorageBuffer};
    cmd.i[0] = int32_t(c.x[1]), cmd.i[1] = int32_t(c.x[2]);
    cmd.a[0] = c.x[3], cmd.a[1] = c.x[4];
    record(c.x[0], cmd);
}

void BindTexture(CPU& c) {
    Cmd cmd{Cmd::BindTexture};
    cmd.i[0] = int32_t(c.x[1]), cmd.i[1] = int32_t(c.x[2]);
    cmd.a[0] = c.x[3];
    record(c.x[0], cmd);
}

void BindVertexBuffer(CPU& c) {
    Cmd cmd{Cmd::BindVertexBuffer};
    cmd.i[0] = int32_t(c.x[1]);
    cmd.a[0] = c.x[2], cmd.a[1] = c.x[3];
    record(c.x[0], cmd);
}

void SetTexturePool(CPU& c) {
    Cmd cmd{Cmd::SetTexturePool};
    cmd.a[0] = c.x[1];
    record(c.x[0], cmd);
}

void SetSamplerPool(CPU& c) {
    Cmd cmd{Cmd::SetSamplerPool};
    cmd.a[0] = c.x[1];
    record(c.x[0], cmd);
}

void UpdateUniformBuffer(CPU& c) {  // the data is copied now: the game's buffer is on its stack
    Cmd cmd{Cmd::UpdateUniformBuffer};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[2], cmd.a[2] = c.x[3];
    size_t n = size_t(c.x[4]);
    record(c.x[0], cmd, reinterpret_cast<const void*>(c.x[5]), n);
}

// ---- state objects ------------------------------------------------------------------------------------------------
// NVN state objects are plain 4-byte or 8-byte value structs. Caching their decoded fields by raw value avoids
// calling into the guest SDK driver getters (and real_proc map lookups) thousands of times per frame.
template <typename Raw, typename Val, size_t N = 256>
struct StateCache {
    struct Entry {
        Raw raw = 0;
        bool valid = false;
        Val val{};
    };
    Entry slots[N]{};
    static size_t index(Raw r) {
        uint64_t x = uint64_t(r) * 0x9E3779B97F4A7C15ull;
        return size_t((x ^ (x >> 32)) & (N - 1));
    }
    template <typename Decode>
    const Val& get(Raw raw, Decode&& decode) {
        Entry& e = slots[index(raw)];
        if (e.valid && e.raw == raw) return e.val;
        e.raw = raw;
        e.val = decode();
        e.valid = true;
        return e.val;
    }
};

void BindVertexAttribState(CPU& c) {
    uint32_t n = std::min<uint32_t>(uint32_t(c.x[1]), 32);
    VertexAttrib v[32];
    thread_local StateCache<uint32_t, VertexAttrib, 512> cache;
    uint64_t tmp = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t state = c.x[2] + 4 * k;
        uint32_t raw = ld<uint32_t>(state);
        v[k] = cache.get(raw, [&] {
            if (!tmp) tmp = scratch(c, 16);
            VertexAttrib a{};
            driver(c, "nvnVertexAttribStateGetFormat", {state, tmp, tmp + 8});
            a.format = ld<uint32_t>(tmp);
            a.offset = uint32_t(ld<uint64_t>(tmp + 8));
            a.stream = uint32_t(driver(c, "nvnVertexAttribStateGetStreamIndex", {state}));
            return a;
        });
    }
    for (uint32_t k = 0; k < n; k++) log_state("attrib", v[k]);
    Cmd cmd{Cmd::BindVertexAttribs};
    cmd.n = n;
    record(c.x[0], cmd, v, n * sizeof(VertexAttrib));
}

void BindVertexStreamState(CPU& c) {
    uint32_t n = std::min<uint32_t>(uint32_t(c.x[1]), 32);
    VertexStream v[32];
    thread_local StateCache<uint64_t, VertexStream, 256> cache;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t state = c.x[2] + 8 * k;
        uint64_t raw = ld<uint64_t>(state);
        v[k] = cache.get(raw, [&] {
            VertexStream s{};
            s.stride = uint32_t(driver(c, "nvnVertexStreamStateGetStride", {state}));
            s.divisor = uint32_t(driver(c, "nvnVertexStreamStateGetDivisor", {state}));
            return s;
        });
    }
    Cmd cmd{Cmd::BindVertexStreams};
    cmd.n = n;
    record(c.x[0], cmd, v, n * sizeof(VertexStream));
}

void BindBlendState(CPU& c) {
    uint64_t state = c.x[1];
    uint64_t raw = ld<uint64_t>(state);
    thread_local StateCache<uint64_t, BlendTarget, 256> cache;
    const BlendTarget& b = cache.get(raw, [&] {
        uint64_t tmp = scratch(c, 16);
        BlendTarget out;
        out.target = uint32_t(driver(c, "nvnBlendStateGetBlendTarget", {state}));
        driver(c, "nvnBlendStateGetBlendFunc", {state, tmp, tmp + 4, tmp + 8, tmp + 12});
        out.src_rgb = ld<uint32_t>(tmp), out.dst_rgb = ld<uint32_t>(tmp + 4);
        out.src_a = ld<uint32_t>(tmp + 8), out.dst_a = ld<uint32_t>(tmp + 12);
        driver(c, "nvnBlendStateGetBlendEquation", {state, tmp, tmp + 4});
        out.eq_rgb = ld<uint32_t>(tmp), out.eq_a = ld<uint32_t>(tmp + 4);
        return out;
    });
    Cmd cmd{Cmd::BindBlendState};
    log_state("blend", b);
    record(c.x[0], cmd, &b, sizeof b);
}

void BindColorState(CPU& c) {
    uint64_t state = c.x[1];
    uint32_t raw = ld<uint32_t>(state);
    thread_local StateCache<uint32_t, ColorStateInfo, 256> cache;
    const ColorStateInfo& s = cache.get(raw, [&] {
        ColorStateInfo out;
        for (uint32_t k = 0; k < 8; k++) out.blend_enable[k] = uint8_t(driver(c, "nvnColorStateGetBlendEnable", {state, k}) & 1);
        out.alpha_test = uint32_t(driver(c, "nvnColorStateGetAlphaTest", {state}));
        out.logic_op = uint32_t(driver(c, "nvnColorStateGetLogicOp", {state}));
        return out;
    });
    Cmd cmd{Cmd::BindColorState};
    log_state("color", s);
    record(c.x[0], cmd, &s, sizeof s);
}

void BindChannelMaskState(CPU& c) {
    uint64_t state = c.x[1];
    uint32_t raw = ld<uint32_t>(state);
    thread_local StateCache<uint32_t, ChannelMaskInfo, 256> cache;
    const ChannelMaskInfo& s = cache.get(raw, [&] {
        uint64_t tmp = scratch(c, 16);
        ChannelMaskInfo out;
        for (uint32_t k = 0; k < 8; k++) {
            for (int ch = 0; ch < 4; ch++) st<uint32_t>(tmp + 4 * ch, 0);
            driver(c, "nvnChannelMaskStateGetChannelMask", {state, k, tmp, tmp + 4, tmp + 8, tmp + 12});
            out.mask[k] = uint8_t((ld<uint8_t>(tmp) & 1) | (ld<uint8_t>(tmp + 4) & 1) << 1 | (ld<uint8_t>(tmp + 8) & 1) << 2 |
                                  (ld<uint8_t>(tmp + 12) & 1) << 3);
        }
        return out;
    });
    Cmd cmd{Cmd::BindChannelMaskState};
    record(c.x[0], cmd, &s, sizeof s);
}

void BindDepthStencilState(CPU& c) {
    uint64_t state = c.x[1];
    uint64_t raw = ld<uint64_t>(state);
    thread_local StateCache<uint64_t, DepthStencilInfo, 256> cache;
    const DepthStencilInfo& s = cache.get(raw, [&] {
        uint64_t tmp = scratch(c, 16);
        DepthStencilInfo out;
        out.depth_test = uint8_t(driver(c, "nvnDepthStencilStateGetDepthTestEnable", {state}) & 1);
        out.depth_write = uint8_t(driver(c, "nvnDepthStencilStateGetDepthWriteEnable", {state}) & 1);
        out.depth_func = uint32_t(driver(c, "nvnDepthStencilStateGetDepthFunc", {state}));
        out.stencil_test = uint8_t(driver(c, "nvnDepthStencilStateGetStencilTestEnable", {state}) & 1);
        for (uint32_t face = 0; face < 2; face++) {  // NVNface: 1 front, 2 back
            out.stencil_func[face] = uint32_t(driver(c, "nvnDepthStencilStateGetStencilFunc", {state, face + 1}));
            driver(c, "nvnDepthStencilStateGetStencilOp", {state, face + 1, tmp, tmp + 4, tmp + 8});
            out.stencil_fail[face] = ld<uint32_t>(tmp), out.depth_fail[face] = ld<uint32_t>(tmp + 4);
            out.depth_pass[face] = ld<uint32_t>(tmp + 8);
        }
        return out;
    });
    Cmd cmd{Cmd::BindDepthStencilState};
    log_state("depth", s);
    record(c.x[0], cmd, &s, sizeof s);
}

void BindPolygonState(CPU& c) {
    uint64_t state = c.x[1];
    uint32_t raw = ld<uint32_t>(state);
    thread_local StateCache<uint32_t, PolygonInfo, 256> cache;
    const PolygonInfo& s = cache.get(raw, [&] {
        PolygonInfo out;
        out.cull = uint32_t(driver(c, "nvnPolygonStateGetCullFace", {state}));
        out.front = uint32_t(driver(c, "nvnPolygonStateGetFrontFace", {state}));
        out.mode = uint32_t(driver(c, "nvnPolygonStateGetPolygonMode", {state}));
        out.offset_enables = uint32_t(driver(c, "nvnPolygonStateGetPolygonOffsetEnables", {state}));
        return out;
    });
    Cmd cmd{Cmd::BindPolygonState};
    log_state("polygon", s);
    record(c.x[0], cmd, &s, sizeof s);
}

void BindMultisampleState(CPU& c) {
    uint64_t state = c.x[1];
    Cmd cmd{Cmd::BindMultisampleState};
    static uint64_t fn = 0;
    if (state) {
        if (!fn) fn = real_proc(c, "nvnMultisampleStateGetAlphaToCoverageEnable");
        if (fn) cmd.n = uint32_t(guest_call(c, fn, {state}) & 1);
    }
    log_state("multisample", cmd.n);
    record(c.x[0], cmd);
}

void SetBlendColor(CPU& c) {
    Cmd cmd{Cmd::SetBlendColor};
    if (c.x[1]) std::memcpy(cmd.f, reinterpret_cast<const void*>(c.x[1]), 16);
    record(c.x[0], cmd);
}

void SetDepthRange(CPU& c) {  // (cmd, float near [s0], float far [s1])
    Cmd cmd{Cmd::SetDepthRange};
    cmd.f[0] = c.v[0].f32[0], cmd.f[1] = c.v[1].f32[0];
    record(c.x[0], cmd);
}

std::atomic<uint64_t> g_recorded_draws{0};

// ---- draws --------------------------------------------------------------------------------------------------------

void DrawArrays(CPU& c) {
    g_recorded_draws++;
    Cmd cmd{Cmd::DrawArrays};
    cmd.n = uint32_t(c.x[1]);                                    // mode
    cmd.i[0] = int32_t(c.x[2]), cmd.i[1] = int32_t(c.x[3]);      // first, count
    cmd.i[2] = 0, cmd.i[3] = 1;                                  // base instance, instances
    record(c.x[0], cmd);
}

void DrawArraysInstanced(CPU& c) {
    Cmd cmd{Cmd::DrawArrays};
    cmd.n = uint32_t(c.x[1]);
    cmd.i[0] = int32_t(c.x[2]), cmd.i[1] = int32_t(c.x[3]);
    cmd.i[2] = int32_t(c.x[4]), cmd.i[3] = int32_t(c.x[5]);
    record(c.x[0], cmd);
}

void draw_elements(CPU& c, int32_t base_vertex, int32_t base_instance, int32_t instances) {
    g_recorded_draws++;
    Cmd cmd{Cmd::DrawElements};
    cmd.n = uint32_t(c.x[1]);        // mode
    cmd.mask = uint32_t(c.x[2]);     // index type: 0 u8, 1 u16, 2 u32
    cmd.i[0] = int32_t(c.x[3]);      // count
    cmd.a[0] = c.x[4];               // index buffer address
    cmd.i[1] = base_vertex, cmd.i[2] = base_instance, cmd.i[3] = instances;
    record(c.x[0], cmd);
}

void DrawElements(CPU& c) { draw_elements(c, 0, 0, 1); }
void DrawElementsBaseVertex(CPU& c) { draw_elements(c, int32_t(c.x[5]), 0, 1); }
void DrawElementsInstanced(CPU& c) { draw_elements(c, int32_t(c.x[5]), int32_t(c.x[6]), int32_t(c.x[7])); }

void ignore(CPU&) {}

// ReportCounter(cmd, NVNcounterType, NVNbufferAddress): the GPU writes {u64 value, u64 timestamp} there when it gets
// to this point. Engines read these to know how far the GPU is (Wonder skips drawing the scene when they never
// arrive), so the replay writes them.
void ReportCounter(CPU& c) {
    Cmd cmd{Cmd::ReportCounter};
    cmd.n = uint32_t(c.x[1]);
    cmd.a[0] = c.x[2];
    record(c.x[0], cmd);
}

// Our GPU timestamps are host nanoseconds: the conversions are identities.
void GetTimestampInNanoseconds(CPU& c) { c.x[0] = c.x[1] ? ld<uint64_t>(c.x[1] + 8) : 0; }  // (device, counterData*)
void GetCurrentTimestampInNanoseconds(CPU& c) { c.x[0] = plat::monotonic_ns(); }


}  // namespace

uint64_t recorded_draws() { return g_recorded_draws; }

NX_HLE("nvnCommandBufferBindProgram", BindProgram);
NX_HLE("nvnCommandBufferBindUniformBuffer", BindUniformBuffer);
NX_HLE("nvnCommandBufferBindStorageBuffer", BindStorageBuffer);
NX_HLE("nvnCommandBufferBindTexture", BindTexture);
NX_HLE("nvnCommandBufferBindVertexBuffer", BindVertexBuffer);
NX_HLE("nvnCommandBufferSetTexturePool", SetTexturePool);
NX_HLE("nvnCommandBufferSetSamplerPool", SetSamplerPool);
NX_HLE("nvnCommandBufferUpdateUniformBuffer", UpdateUniformBuffer);
NX_HLE("nvnCommandBufferBindVertexAttribState", BindVertexAttribState);
NX_HLE("nvnCommandBufferBindVertexStreamState", BindVertexStreamState);
NX_HLE("nvnCommandBufferBindBlendState", BindBlendState);
NX_HLE("nvnCommandBufferBindColorState", BindColorState);
NX_HLE("nvnCommandBufferBindChannelMaskState", BindChannelMaskState);
NX_HLE("nvnCommandBufferBindDepthStencilState", BindDepthStencilState);
NX_HLE("nvnCommandBufferBindPolygonState", BindPolygonState);
NX_HLE("nvnCommandBufferBindMultisampleState", BindMultisampleState);
NX_HLE("nvnCommandBufferSetBlendColor", SetBlendColor);
NX_HLE("nvnCommandBufferSetDepthRange", SetDepthRange);
NX_HLE("nvnCommandBufferDrawArrays", DrawArrays);
NX_HLE("nvnCommandBufferDrawArraysInstanced", DrawArraysInstanced);
NX_HLE("nvnCommandBufferDrawElements", DrawElements);
NX_HLE("nvnCommandBufferDrawElementsBaseVertex", DrawElementsBaseVertex);
NX_HLE("nvnCommandBufferDrawElementsInstanced", DrawElementsInstanced);
void Barrier(CPU& c) {
    Cmd cmd{Cmd::Barrier};
    cmd.mask = uint32_t(c.x[1]);
    record(c.x[0], cmd);
}

NX_HLE("nvnCommandBufferBarrier", Barrier);
NX_HLE("nvnCommandBufferReportCounter", ReportCounter);
NX_HLE("nvnDeviceGetTimestampInNanoseconds", GetTimestampInNanoseconds);
NX_HLE("nvnDeviceGetCurrentTimestampInNanoseconds", GetCurrentTimestampInNanoseconds);  // every replayed command is serialized for now

}  // namespace nx::nvn
