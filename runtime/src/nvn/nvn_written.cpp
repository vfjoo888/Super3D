// Compute results within a submit (docs/nvn/GLITCH-PLAN.md G5). A storage buffer a compute shader writes is a copy in
// the upload buffer, written back to guest memory once the submit completed. Each binding used to get a copy of its
// own: later draws and dispatches of the submit read the guest bytes from before the dispatch, two dispatches writing
// one buffer overwrote each other at write-back, and indirect dispatch arguments were read before the GPU wrote them.
// Here the submit keeps one copy per written guest range (the ranges are disjoint, keyed by host address). Writers and
// readers of a range bind that copy; ranges that meet are merged into a new copy on the GPU (vkCmdCopyBuffer, so in
// command order); each range is written back once. NX_COMPUTE_SEPARATE=1: the previous behaviour.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "nvn_draw_state.h"

namespace nx::nvn::replay {

using namespace nx::vk;

namespace {

constexpr uint64_t kAlign = 256;          // the largest buffer offset alignment Vulkan allows (all descriptor kinds)
constexpr uint64_t kTail = 0x10000 + 16;  // zeros after a range: storage bindings are padded to 64 KB (nvn_draw.cpp)

struct Region {
    const uint8_t* p = nullptr;
    uint64_t size = 0;  // guest bytes from p
    uint64_t cap = 0;   // bytes of the copy from up.offset (size, then zeros)
    Upload up;          // the copy of p's first byte
    std::shared_ptr<std::vector<uint8_t>> before;  // the guest bytes copied in (what the GPU did not change)
};
std::vector<Region> g_regions;  // disjoint, sorted by Region::p (capacity kept across submits)
const uint8_t* g_min_addr = nullptr;
const uint8_t* g_max_addr = nullptr;
bool g_read = false;  // a copy was bound for reading since the last write_after_read
uint64_t g_binds = 0;  // written_bind calls, ever (nvn_frames.cpp: did a dispatch write storage after the split)
uint64_t g_shared = 0, g_merged = 0, g_composed = 0, g_indirect = 0;  // for written_log

// First index i in g_regions with g_regions[i].p > p.
size_t upper_idx(const uint8_t* p) {
    size_t lo = 0, hi = g_regions.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_regions[mid].p <= p)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

bool has_overlap_fast(const uint8_t* p, uint64_t size) {
    if (!p || !size || g_regions.empty() || p + size <= g_min_addr || p >= g_max_addr) return false;
    size_t idx = upper_idx(p);
    if (idx > 0 && g_regions[idx - 1].p + g_regions[idx - 1].size > p) return true;
    return idx < g_regions.size() && g_regions[idx].p < p + size;
}

// Contiguous index range [lo, hi) in g_regions overlapping [p, p + size), in address order (zero heap allocs).
std::pair<size_t, size_t> overlapping_range(const uint8_t* p, uint64_t size) {
    if (!p || !size || g_regions.empty() || p + size <= g_min_addr || p >= g_max_addr) return {0, 0};
    size_t idx = upper_idx(p);
    size_t lo = idx;
    if (idx > 0 && g_regions[idx - 1].p + g_regions[idx - 1].size > p) lo = idx - 1;
    size_t hi = idx;
    const uint8_t* end = p + size;
    while (hi < g_regions.size() && g_regions[hi].p < end) ++hi;
    return {lo, hi};
}

// `bytes` of upload memory whose byte `anchor` is kAlign aligned (so it can start a binding).
Upload alloc_anchored(uint64_t bytes, uint64_t anchor) {
    Upload u = upload_alloc(bytes + kAlign, kAlign);
    uint64_t shift = (kAlign - anchor % kAlign) % kAlign;
    u.offset += shift, u.ptr += shift;
    return u;
}

// The parts of g_regions[lo..hi) inside [p, p + size) copied on the GPU into `to` (whose first byte is p's), then made
// visible to everything after. What the dispatches recorded so far wrote is there when the copy runs: dispatch()
// ends each one with a barrier to all later commands. Outside render passes.
void copy_regions(VkCommandBuffer cb, size_t rlo, size_t rhi, const uint8_t* p, uint64_t size, const Upload& to) {
    for (size_t i = rlo; i < rhi; ++i) {
        const Region& r = g_regions[i];
        const uint8_t* lo = std::max(r.p, p);
        const uint8_t* hi = std::min(r.p + r.size, p + size);
        if (lo >= hi) continue;
        VkBufferCopy c{r.up.offset + uint64_t(lo - r.p), to.offset + uint64_t(lo - p), uint64_t(hi - lo)};
        p_vkCmdCopyBuffer(cb, r.up.buffer, to.buffer, 1, &c);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
}

// A dispatch about to write a copy that earlier commands of the submit read (draws binding it as vertices, uniforms,
// storage): they finish first. Earlier writes are already ordered by dispatch()'s barrier.
void write_after_read(VkCommandBuffer cb) {
    if (!g_read) return;
    g_read = false;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 0, nullptr);
}

// The device's buffer descriptor offset alignment (kAlign at most, by the Vulkan spec).
uint64_t bind_align() {
    static const uint64_t a = [] {
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        return std::max<uint64_t>({props.limits.minStorageBufferOffsetAlignment,
                                   props.limits.minUniformBufferOffsetAlignment, 16});
    }();
    return a;
}

Upload at(size_t idx, uint64_t off) {
    const Region& r = g_regions[idx];
    return {r.up.buffer, r.up.offset + off, r.up.ptr + off};
}

// [p, p + size) as a sub-range of one region, bindable at `align` (at most the device's descriptor alignment; the
// callers ask 256 for descriptors) with `padded` bytes behind it: else SIZE_MAX. A copy is anchored at its first writer,
// so two pointers aligned in guest memory are aligned in the copy.
size_t containing(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, size_t rlo, size_t rhi) {
    if (rhi - rlo != 1) return SIZE_MAX;
    const Region& r = g_regions[rlo];
    uint64_t off = uint64_t(p - r.p);
    if (p < r.p || off + size > r.size || off + padded > r.cap) return SIZE_MAX;
    if ((r.up.offset + off) % std::clamp<uint64_t>(align, 1, bind_align())) return SIZE_MAX;
    return rlo;
}

}  // namespace

bool written_separate() {
    static const bool on = std::getenv("NX_COMPUTE_SEPARATE") != nullptr;
    return on;
}

uint64_t written_bind_count() { return g_binds; }
void written_count_bind() { g_binds++; }

Upload written_bind(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded) {
    g_binds++;
    auto [rlo, rhi] = overlapping_range(p, size);
    if (size_t idx = containing(p, size, padded, bind_align(), rlo, rhi); idx != SIZE_MAX) {
        write_after_read(cb);
        g_shared++;
        return at(idx, uint64_t(p - g_regions[idx].p));
    }
    // A new range, or one meeting others (or misaligned in its copy): one copy covering all of them, anchored at p.
    const uint8_t* lo = p;
    const uint8_t* hi = p + size;
    for (size_t i = rlo; i < rhi; ++i)
        lo = std::min(lo, g_regions[i].p), hi = std::max(hi, g_regions[i].p + g_regions[i].size);
    uint64_t n = uint64_t(hi - lo), off = uint64_t(p - lo);
    Region r;
    r.p = lo;
    r.size = n;
    r.cap = std::max(n + kTail, off + padded);
    r.up = alloc_anchored(r.cap, off);
    std::memcpy(r.up.ptr, lo, n);
    std::memset(r.up.ptr + n, 0, r.cap - n);
    r.before = std::make_shared<std::vector<uint8_t>>(lo, lo + n);
    for (size_t i = rlo; i < rhi; ++i)  // their parts: as they were before their own GPU writes
        if (g_regions[i].before)
            std::memcpy(r.before->data() + (g_regions[i].p - lo), g_regions[i].before->data(),
                        g_regions[i].before->size());
    if (rlo < rhi) {
        copy_regions(cb, rlo, rhi, lo, n, r.up);  // dispatch(): no render pass is open
        g_regions.erase(g_regions.begin() + ptrdiff_t(rlo), g_regions.begin() + ptrdiff_t(rhi));
        g_merged++;
    }
    dedup_invalidate(lo, n);  // copies of these bytes made earlier in the submit are not what later work reads
    if (!g_min_addr || lo < g_min_addr) g_min_addr = lo;
    if (!g_max_addr || hi > g_max_addr) g_max_addr = hi;
    size_t insert_at = upper_idx(lo);
    g_regions.insert(g_regions.begin() + ptrdiff_t(insert_at), std::move(r));
    return at(insert_at, off);
}

bool written_read(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align, Upload& out) {
    if (!p || !size || g_regions.empty() || p + size <= g_min_addr || p >= g_max_addr || written_separate()) return false;
    auto [rlo, rhi] = overlapping_range(p, size);
    if (rlo == rhi) return false;
    if (size_t idx = containing(p, size, padded, align, rlo, rhi); idx != SIZE_MAX) {
        g_shared++, g_read = true;
        out = at(idx, uint64_t(p - g_regions[idx].p));
        return true;
    }
    // Partly written, or not bindable in place: a copy of its own, the written parts copied in on the GPU. This is
    // what the range holds at this point of the submit (later dispatches do not change it).
    draw_end_rendering(cb, 2);
    out = upload_alloc(std::max(padded, size), std::max<uint64_t>(align, 16));
    std::memcpy(out.ptr, p, size);
    if (padded > size) std::memset(out.ptr + size, 0, padded - size);
    copy_regions(cb, rlo, rhi, p, size, out);
    g_composed++;
    return true;
}

Upload read_upload(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    Upload u;
    if (written_read(cb, p, size, padded, align, u)) return u;
    return dedup_upload(p, size, padded, align);
}

bool written_overlaps(const uint8_t* p, uint64_t size) {
    return has_overlap_fast(p, size);
}

bool written_indirect(VkCommandBuffer cb, const uint8_t* p) {
    Upload u;
    if (!written_read(cb, p, 12, 16, 4, u)) return false;
    p_vkCmdDispatchIndirect(cb, u.buffer, u.offset);  // {x, y, z} as the earlier dispatches left them
    g_indirect++;
    return true;
}

bool draw_inputs(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in) {
    // Vertex buffers: the bound ranges of the streams the vertex shader reads (the pipeline declares no others).
    for (const ShaderIo& io : vs->refl.inputs)
        if (io.location < g_state.attribs.size() && g_state.attribs[io.location].stream < 15)
            in.used |= 1u << g_state.attribs[io.location].stream;
    for (uint32_t s = 0; s < 15; s++) {
        if (!(in.used >> s & 1)) continue;
        const Range& r = g_state.vertex_buffers[s];
        PoolSlice slice{};
        const uint8_t* p = r.address ? guest_bytes_pool(r.address, &slice) : nullptr;
        uint64_t size = p ? std::min<uint64_t>(r.size, 64ull << 20) : 0;
        uint64_t padded = std::max<uint64_t>((size + 15) & ~15ull, 16);
        if (!written_read(cb, p, size, padded, 16, in.vertex[s]) &&
            !static_buf_upload(cb, p, size, padded, 16, slice, in.vertex[s]))
            in.vertex[s] = dedup_upload(p, size, padded, 16);
    }
    if (!indexed) return true;
    uint32_t count = uint32_t(c.i[0]);
    PoolSlice slice{};
    const uint8_t* p = guest_bytes_pool(c.a[0], &slice);
    if (!p) return false;
    in.index_type = vk_index_type(c.mask);
    if (in.index_type == VK_INDEX_TYPE_UINT8_EXT) {
        // 8-bit indices: Metal has none, and MoltenVK converts them on the GPU at every draw (that was most of the
        // frame's GPU time), so they are widened here, from guest memory (a dispatch writing them is only logged).
        static int logs = 0;
        if (written_overlaps(p, count) && logs++ < 16) log("nvn: 8-bit indices written by a dispatch: stale");
        in.index = upload_alloc(uint64_t(count) * 2, 16);
        auto* d = reinterpret_cast<uint16_t*>(in.index.ptr);
        for (uint32_t k = 0; k < count; k++) d[k] = p[k];
        in.index_type = VK_INDEX_TYPE_UINT16;
        return true;
    }
    uint64_t bytes = uint64_t(count) * index_size(c.mask);
    if (!written_read(cb, p, bytes, bytes, index_size(c.mask), in.index) &&
        !static_buf_upload(cb, p, bytes, bytes, 16, slice, in.index))
        in.index = dedup_upload(p, bytes, bytes, 16);
    return true;
}

void check_vertex_ranges(const ShaderInfo* vs, const ShaderInfo* fs, bool indexed, const Cmd& c, const DrawInputs& in) {
    static const bool on = std::getenv("NX_CHECK_VERTS") != nullptr;
    static uint64_t found = 0;
    if (!on || !vs) return;
    // The vertices and instances the draw fetches.
    uint64_t lo = 0, hi = 0;  // vertex indices [lo, hi]
    uint32_t count = uint32_t(indexed ? c.i[0] : c.i[1]);
    if (!count) return;
    if (indexed) {
        const uint8_t* p = guest_bytes(c.a[0]);
        if (!p) return;
        uint32_t isz = index_size(c.mask), restart = isz == 1 ? 0xFF : isz == 2 ? 0xFFFF : 0xFFFFFFFFu;
        uint32_t mn = UINT32_MAX, mx = 0;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t v = isz == 1 ? p[k] : isz == 2 ? reinterpret_cast<const uint16_t*>(p)[k] : reinterpret_cast<const uint32_t*>(p)[k];
            if (v == restart) continue;
            mn = std::min(mn, v), mx = std::max(mx, v);
        }
        if (mn == UINT32_MAX) return;
        lo = uint64_t(int64_t(mn) + c.i[1]), hi = uint64_t(int64_t(mx) + c.i[1]);
    } else {
        lo = uint32_t(c.i[0]), hi = uint64_t(uint32_t(c.i[0])) + count - 1;
    }
    uint64_t inst_hi = uint64_t(uint32_t(c.i[2])) + uint32_t(std::max(1, c.i[3])) - 1;
    for (const ShaderIo& io : vs->refl.inputs) {
        if (io.location >= g_state.attribs.size()) continue;
        const VertexAttrib& va = g_state.attribs[io.location];
        if (va.stream >= 15 || va.stream >= g_state.streams.size() || !(in.used >> va.stream & 1)) continue;
        const VertexStream& st = g_state.streams[va.stream];
        const Range& r = g_state.vertex_buffers[va.stream];
        uint64_t last = st.divisor ? inst_hi / st.divisor : hi;
        uint64_t end = last * st.stride + va.offset + vk_block_bytes(vk_vertex_format(va.format));
        uint64_t copied = r.address && guest_bytes(r.address) ? std::min<uint64_t>(r.size, 64ull << 20) : 0;
        if (end <= copied) continue;
        found++;
        static std::map<std::string, uint64_t> seen;
        std::string key = vs->hash + (fs ? fs->hash : std::string("-")) + char('a' + va.stream);
        uint64_t n = ++seen[key];
        if (n <= 3 || (n & (n - 1)) == 0)
            log("nvn: vertex fetch past the bound range (%llu so far; #%llu of vs %s fs %s): stream %u loc %u fmt %#x off %u "
                "stride %u div %u, vertices %llu..%llu instances ..%llu, needs %llu bytes, bound %llu at %llx (copied %llu)",
                (unsigned long long)found, (unsigned long long)n, vs->hash.c_str(), fs ? fs->hash.c_str() : "-", va.stream,
                io.location, va.format, va.offset, st.stride, st.divisor, (unsigned long long)lo, (unsigned long long)hi,
                (unsigned long long)inst_hi, (unsigned long long)end, (unsigned long long)r.size,
                (unsigned long long)r.address, (unsigned long long)copied);
        break;
    }
}

void written_cpu_write(const uint8_t* p, uint64_t size) {
    // UpdateUniformBuffer / ReportCounter into a range a dispatch of this submit writes: the copy misses the bytes
    // (they reach guest memory at replay time) and the write-back replaces them. Not handled: logged.
    static int logs = 0;
    if (written_overlaps(p, size) && logs++ < 16)
        log("nvn: guest write into a compute-written range (%llu bytes): lost at write-back", (unsigned long long)size);
}

std::vector<WriteBack> written_take() {
    std::vector<WriteBack> v;
    v.reserve(g_regions.size());
    for (auto& r : g_regions) v.push_back({r.up.ptr, const_cast<uint8_t*>(r.p), r.size, r.before});
    g_regions.clear();
    g_min_addr = g_max_addr = nullptr;
    return v;
}

void written_reset() { g_regions.clear(), g_min_addr = g_max_addr = nullptr, g_read = false; }

void written_log() {
    if (g_shared + g_merged + g_composed + g_indirect)
        log("nvn: compute ranges: %llu bindings of written copies, %llu merged, %llu reads composed on the GPU, %llu indirect",
            (unsigned long long)g_shared, (unsigned long long)g_merged, (unsigned long long)g_composed,
            (unsigned long long)g_indirect);
    g_shared = g_merged = g_composed = g_indirect = 0;
}

}  // namespace nx::nvn::replay

namespace nx::nvn {

// A bound storage range is often larger than what the shader writes, and the game's recording thread writes the
// rest of those buffers while the submit is replayed. Copying the whole range back put stale bytes over them: meshes
// exploded into beams and a band of stretched triangles at the horizon, for the rest of the run (DEBUG.md #56). Only
// the words the GPU changed go back. NX_WRITE_BACK_WHOLE=1: the old whole-range copy.
void apply_write_back(const WriteBack& w) {
    static const bool whole = std::getenv("NX_WRITE_BACK_WHOLE") != nullptr;
    if (whole || !w.before || w.before->size() < w.size) {
        std::memcpy(w.to, w.from, w.size);
        return;
    }
    const uint8_t* was = w.before->data();
    uint64_t k = 0;
    for (; k + 8 <= w.size; k += 8) {
        if (std::memcmp(w.from + k, was + k, 8) == 0) continue;
        for (uint64_t b = k; b < k + 8; b += 4)
            if (std::memcmp(w.from + b, was + b, 4) != 0) std::memcpy(w.to + b, w.from + b, 4);
    }
    for (; k < w.size; k++)
        if (w.from[k] != was[k]) w.to[k] = w.from[k];
}

}  // namespace nx::nvn
