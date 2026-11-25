// Per-submit upload reuse (docs/nvn/DEBUG.md #30). A draw's buffers are copied from guest memory into the upload
// ring when it is replayed; the same camera block, scene constants and vertex buffers were copied again for every
// draw that bound them (~130 KB per draw, 50-70 MB per frame). Within one submit guest memory only changes through
// inline uniform updates (UpdateUniformBuffer, which invalidates what it overlaps) and compute results (ranges a
// dispatch writes are read from their written copy, nvn_written.cpp, and invalidated here): a copy made once can
// serve every later draw of the submit that binds the same bytes.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "nvn_draw_state.h"
#include "platform.h"

namespace nx::nvn::replay {

using namespace nx::vk;

namespace {

// ---- Cross-frame DEVICE_LOCAL static VB/IB cache ---------------------------------------------------------------
// Vertex and index buffers in CPU_CACHED (0x4) or CPU_NO_ACCESS (0x1) memory pools do not change on the CPU without
// nvnMemoryPoolFlushMappedRange (or MemoryPoolFinalize / UpdateUniformBuffer / compute writes). Uploading them once
// into DEVICE_LOCAL VRAM slabs avoids re-copying ~10+ MB/frame through the host upload ring and PCIe bus.
// NX_STATIC_BUF_CACHE=0 disables it (bisecting / A-B benchmarking).
constexpr uint64_t kStaticSlabSize = 16ull << 20;  // 16 MiB per DEVICE_LOCAL slab
constexpr uint32_t kStaticMaxSlabs = 16;           // 256 MiB max VRAM budget
constexpr uint32_t kStaticTableSize = 32768;
constexpr uint32_t kStaticPageBuckets = 8192;

struct StaticSlab {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t size = 0, used = 0, live_bytes = 0;
    uint64_t last_serial = 0;
};

struct StaticSlot {
    const uint8_t* p = nullptr;
    uint64_t size = 0, padded = 0, offset = 0;
    uint32_t slab = 0;
    uint32_t generation = 0;
    bool valid = false;
};

struct StaticPageBucket {
    uint32_t generation = 0;
    std::vector<uint32_t> slots;
};

std::vector<StaticSlab> g_slabs;
StaticSlot g_static_table[kStaticTableSize];
StaticPageBucket g_static_pages[kStaticPageBuckets];
uint32_t g_static_gen = 1;
uint32_t g_static_used = 0;
uint64_t g_static_hits = 0, g_static_misses = 0, g_static_saved_bytes = 0, g_static_uploaded_bytes = 0;

std::mutex g_inval_lock;
std::vector<std::pair<uintptr_t, uint64_t>> g_inval_queue;
std::atomic<uint32_t> g_inval_pending{0};

// Hardware write-watch tracking for CPU_UNCACHED (0x2) pools on Windows (MEM_WRITE_WATCH / GetWriteWatch).
struct WatchedPool {
    uint64_t base = 0, size = 0;
    uint64_t added_submit = 0;
    bool broken = false;
};
std::vector<WatchedPool> g_watched_pools;
uint64_t g_static_submit = 0;

constexpr uint32_t kHotPageSlots = 4096;
struct HotPage {
    uint64_t page = 0;
    uint64_t dirty_submit = 0;
};
HotPage g_hot_pages[kHotPageSlots];

void static_mark_hot_page(uint64_t addr) {
    uint64_t pg = addr >> 12;
    HotPage& h = g_hot_pages[(pg ^ (pg >> 12)) & (kHotPageSlots - 1)];
    h.page = pg;
    h.dirty_submit = g_static_submit;
}

bool static_range_is_hot(const uint8_t* p, uint64_t size) {
    uint64_t p0 = uintptr_t(p) >> 12;
    uint64_t p1 = (uintptr_t(p) + size - 1) >> 12;
    for (uint64_t pg = p0; pg <= p1; ++pg) {
        const HotPage& h = g_hot_pages[(pg ^ (pg >> 12)) & (kHotPageSlots - 1)];
        if (h.page == pg && g_static_submit - h.dirty_submit <= 3) return true;
    }
    return false;
}

bool static_buf_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_STATIC_BUF_CACHE");
        return !(e && e[0] == '0') && std::getenv("NX_NO_DEDUP") == nullptr;
    }();
    return on;
}

inline bool static_pool_flush_tracked(uint32_t pool_flags) {
    // NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS_BIT = 0x1, CPU_UNCACHED_BIT = 0x2, CPU_CACHED_BIT = 0x4
    return (pool_flags & (0x1u | 0x4u)) != 0 && (pool_flags & 0x2u) == 0;
}

uint32_t static_hash(const uint8_t* p) {
    uintptr_t v = uintptr_t(p);
    return uint32_t(((v >> 4) ^ ((v >> 14) * 0x9E3779B9u)) & (kStaticTableSize - 1));
}

void static_register_pages(const uint8_t* p, uint64_t size, uint32_t idx) {
    const uintptr_t p0 = uintptr_t(p) >> 16;
    const uintptr_t p1 = uintptr_t(p + (size ? size - 1 : 0)) >> 16;
    const uintptr_t span = std::min<uintptr_t>(p1 - p0, kStaticPageBuckets - 1);
    for (uintptr_t k = 0; k <= span; ++k) {
        StaticPageBucket& b = g_static_pages[(p0 + k) & (kStaticPageBuckets - 1)];
        if (b.generation != g_static_gen) {
            b.generation = g_static_gen;
            b.slots.clear();
        }
        b.slots.push_back(idx);
    }
}

void static_invalidate_range_now(uintptr_t addr, uint64_t size) {
    if (!addr || !size || !g_static_used) return;
    const uintptr_t end = addr + size;
    const uintptr_t p0 = addr >> 16;
    const uintptr_t p1 = (end - 1) >> 16;
    const uintptr_t span = std::min<uintptr_t>(p1 - p0, kStaticPageBuckets - 1);
    for (uintptr_t k = 0; k <= span; ++k) {
        StaticPageBucket& b = g_static_pages[(p0 + k) & (kStaticPageBuckets - 1)];
        if (b.generation != g_static_gen) continue;
        for (uint32_t idx : b.slots) {
            StaticSlot& s = g_static_table[idx];
            if (s.generation != g_static_gen || !s.valid) continue;
            uintptr_t sp = uintptr_t(s.p);
            if (sp < end && addr < sp + s.size) {
                s.valid = false;
                if (s.slab < g_slabs.size() && g_slabs[s.slab].live_bytes >= s.padded)
                    g_slabs[s.slab].live_bytes -= s.padded;
            }
        }
    }
}

void static_drain_invalidations() {
    if (g_inval_pending.load(std::memory_order_acquire) == 0) return;
    std::vector<std::pair<uintptr_t, uint64_t>> batch;
    {
        std::lock_guard lk(g_inval_lock);
        batch.swap(g_inval_queue);
        g_inval_pending.store(0, std::memory_order_release);
    }
    for (const auto& [addr, size] : batch) {
        static_invalidate_range_now(addr, size);
        // If an entire watched pool was finalized/freed, drop it from hardware write-watch polling.
        g_watched_pools.erase(
            std::remove_if(g_watched_pools.begin(), g_watched_pools.end(),
                           [&](const WatchedPool& wp) {
                               return addr <= wp.base && addr + size >= wp.base + wp.size;
                           }),
            g_watched_pools.end());
    }
}

void static_evict_slab(uint32_t slab_idx) {
    for (uint32_t i = 0; i < kStaticTableSize; ++i) {
        StaticSlot& s = g_static_table[i];
        if (s.generation == g_static_gen && s.valid && s.slab == slab_idx) s.valid = false;
    }
    g_slabs[slab_idx].used = 0;
    g_slabs[slab_idx].live_bytes = 0;
}

bool static_alloc_slab(uint64_t padded, uint64_t align, uint32_t& out_slab, uint64_t& out_offset) {
    uint64_t completed = frame_completed();
    for (uint32_t i = 0; i < g_slabs.size(); ++i) {
        StaticSlab& sl = g_slabs[i];
        if (sl.used > 0 && sl.live_bytes == 0 && sl.last_serial <= completed) sl.used = 0;
        uint64_t off = (sl.used + align - 1) & ~(align - 1);
        if (off + padded <= sl.size) {
            sl.used = off + padded;
            sl.live_bytes += padded;
            sl.last_serial = frame_recording_serial();
            out_slab = i;
            out_offset = off;
            return true;
        }
    }
    // Create a new DEVICE_LOCAL slab if within VRAM budget.
    if (g_slabs.size() < kStaticMaxSlabs && vk_device()) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kStaticSlabSize;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buf = VK_NULL_HANDLE;
        if (p_vkCreateBuffer(vk_device(), &bi, nullptr, &buf) == VK_SUCCESS) {
            VkMemoryRequirements req{};
            p_vkGetBufferMemoryRequirements(vk_device(), buf, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (p_vkAllocateMemory(vk_device(), &ai, nullptr, &mem) == VK_SUCCESS &&
                p_vkBindBufferMemory(vk_device(), buf, mem, 0) == VK_SUCCESS) {
                StaticSlab sl;
                sl.buffer = buf;
                sl.memory = mem;
                sl.size = kStaticSlabSize;
                sl.used = padded;
                sl.live_bytes = padded;
                sl.last_serial = frame_recording_serial();
                out_slab = uint32_t(g_slabs.size());
                out_offset = 0;
                g_slabs.push_back(sl);
                return true;
            }
            if (mem) p_vkFreeMemory(vk_device(), mem, nullptr);
            p_vkDestroyBuffer(vk_device(), buf, nullptr);
        }
    }
    // Budget full: recycle the least-recently-used retired slab (untouched for >= 60 submits).
    uint32_t victim = UINT32_MAX;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < g_slabs.size(); ++i) {
        if (g_slabs[i].last_serial <= completed && g_slabs[i].last_serial + 60 < completed &&
            g_slabs[i].last_serial < oldest) {
            oldest = g_slabs[i].last_serial;
            victim = i;
        }
    }
    if (victim != UINT32_MAX) {
        static_evict_slab(victim);
        StaticSlab& sl = g_slabs[victim];
        sl.used = padded;
        sl.live_bytes = padded;
        sl.last_serial = frame_recording_serial();
        out_slab = victim;
        out_offset = 0;
        return true;
    }
    return false;
}

struct Entry {
    uint64_t size = 0, padded = 0;
    Upload upload;
};

constexpr uint32_t kTableSize = 16384;
constexpr uint32_t kPageBuckets = 4096;

struct Slot {
    const uint8_t* p = nullptr;
    uint64_t generation = 0;
    bool valid = false;
    Entry entry;
};

struct PageBucket {
    uint64_t generation = 0;
    std::vector<uint32_t> slots;
};

Slot g_table[kTableSize];
PageBucket g_pages[kPageBuckets];
uint64_t g_generation = 1;
uint32_t g_table_used = 0;
uint64_t g_saved = 0, g_copied = 0;

uint32_t slot_hash(const uint8_t* p) {
    uintptr_t v = uintptr_t(p);
    return uint32_t(((v >> 4) ^ ((v >> 14) * 0x9E3779B9u)) & (kTableSize - 1));
}

void register_pages(const uint8_t* p, uint64_t size, uint32_t idx) {
    const uintptr_t p0 = uintptr_t(p) >> 16;
    const uintptr_t p1 = uintptr_t(p + (size ? size - 1 : 0)) >> 16;
    const uintptr_t span = std::min<uintptr_t>(p1 - p0, kPageBuckets - 1);
    for (uintptr_t k = 0; k <= span; ++k) {
        PageBucket& b = g_pages[(p0 + k) & (kPageBuckets - 1)];
        if (b.generation != g_generation) {
            b.generation = g_generation;
            b.slots.clear();
        }
        b.slots.push_back(idx);
    }
}

}  // namespace

Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    if (!p || !size) {
        Upload u = upload_alloc(padded, align);
        std::memset(u.ptr, 0, padded);
        return u;
    }
    static const bool off = std::getenv("NX_NO_DEDUP") != nullptr;  // bisecting: a fresh copy every time
    uint32_t target_idx = UINT32_MAX;
    if (!off) {
        if (g_table_used >= kTableSize / 2) dedup_reset();
        const uint32_t h = slot_hash(p);
        for (uint32_t probe = 0; probe < kTableSize; ++probe) {
            const uint32_t idx = (h + probe) & (kTableSize - 1);
            Slot& s = g_table[idx];
            if (s.generation != g_generation) {
                if (target_idx == UINT32_MAX) target_idx = idx;
                break;
            }
            if (!s.valid) {
                if (target_idx == UINT32_MAX) target_idx = idx;
                continue;
            }
            if (s.p == p) {
                if (s.entry.size == size && s.entry.padded >= padded && s.entry.upload.offset % align == 0) {
                    g_saved += padded;
                    return s.entry.upload;
                }
                target_idx = idx;
                break;
            }
        }
    }
    Upload u = upload_alloc(padded, align);
    std::memcpy(u.ptr, p, size);
    if (padded > size) std::memset(u.ptr + size, 0, padded - size);
    if (!off && target_idx != UINT32_MAX) {
        Slot& s = g_table[target_idx];
        if (s.generation != g_generation) ++g_table_used;
        s = {p, g_generation, true, {size, padded, u}};
        register_pages(p, size, target_idx);
    }
    g_copied += padded;
    return u;
}

void dedup_reset() {
    ++g_generation;
    g_table_used = 0;
}

void dedup_invalidate(const uint8_t* p, uint64_t size) {
    if (!p || !size) return;
    static_buf_invalidate(reinterpret_cast<uint64_t>(p), size);
    const uintptr_t p0 = uintptr_t(p) >> 16;
    const uintptr_t p1 = uintptr_t(p + size - 1) >> 16;
    const uintptr_t span = std::min<uintptr_t>(p1 - p0, kPageBuckets - 1);
    for (uintptr_t k = 0; k <= span; ++k) {
        PageBucket& b = g_pages[(p0 + k) & (kPageBuckets - 1)];
        if (b.generation != g_generation) continue;
        for (uint32_t idx : b.slots) {
            Slot& s = g_table[idx];
            if (s.generation == g_generation && s.valid && s.p < p + size && p < s.p + s.entry.size)
                s.valid = false;
        }
    }
}

// c[0], the NVN driver's constants, as far as shaders read them (docs/nvn/TRANSLATOR.md): buffer descriptors
// {u64 GPU address; u32 size} of what is bound, which compute shaders search to find the buffer an address falls in
// (they wrote garbage with a zero table), and the maximum point size.
std::array<uint8_t, 0x1000> driver_constants(ShaderStage stage, int st) {
    std::array<uint8_t, 0x1000> c0{};
    auto put = [&](uint32_t at, const Range& r) {
        if (at + 12 > c0.size()) return;
        std::memcpy(c0.data() + at, &r.address, 8);
        uint32_t size = uint32_t(std::min<uint64_t>(r.size, 0xFFFFFFFFu));
        std::memcpy(c0.data() + at + 8, &size, 4);
    };
    if (stage == ShaderStage::Compute) {
        for (int n = 0; n < 16; n++) put(0x210 + 0x10 * n, g_state.uniforms[st][n]);
        for (int n = 0; n < 16; n++) put(0x310 + 0x10 * n, g_state.storage[st][n]);
    } else {
        int hw = stage == ShaderStage::Fragment ? 4 : 0;  // Maxwell stage numbers: vertex 0, fragment 4
        for (int n = 0; n < 16; n++) put(0x110 + 0x100 * hw + 0x10 * n, g_state.storage[st][n]);
        float max_point = 64.0f;
        std::memcpy(c0.data() + 0x610, &max_point, 4);
    }
    return c0;
}

void static_buf_begin_submit() {
    ++g_static_submit;
    static_drain_invalidations();
    // Poll hardware dirty pages for any watched CPU_UNCACHED pools (zero-fault on Windows MEM_WRITE_WATCH).
    if (!g_watched_pools.empty()) {
        void* pages[1024];
        for (WatchedPool& wp : g_watched_pools) {
            if (wp.broken) continue;
            for (;;) {
                uint64_t gran = 4096;
                int n = plat::write_watch_hw_poll(wp.base, wp.size, pages, 1024, &gran);
                if (n < 0) {
                    wp.broken = true;
                    static_invalidate_range_now(wp.base, wp.size);
                    break;
                }
                for (int i = 0; i < n; ++i) {
                    uintptr_t pa = reinterpret_cast<uintptr_t>(pages[i]);
                    static_invalidate_range_now(pa, gran);
                    static_mark_hot_page(pa);
                }
                if (n < 1024) break;
            }
        }
    }
    uint64_t completed = frame_completed();
    for (StaticSlab& sl : g_slabs)
        if (sl.used > 0 && sl.live_bytes == 0 && sl.last_serial <= completed) sl.used = 0;
    if (g_static_used >= kStaticTableSize / 2) {
        // Compact/reset table only when all slabs are idle; otherwise keep serving existing hits until evicted.
        bool all_idle = true;
        for (const StaticSlab& sl : g_slabs)
            if (sl.last_serial > completed) all_idle = false;
        if (all_idle) {
            ++g_static_gen;
            g_static_used = 0;
            for (StaticSlab& sl : g_slabs) sl.used = sl.live_bytes = 0;
        }
    }
}

bool static_buf_upload(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align,
                       const PoolSlice& slice, Upload& out) {
    if (!static_buf_on() || !p || !size || padded > kStaticSlabSize) return false;
    bool hw_watched = false;
    if (!static_pool_flush_tracked(slice.flags)) {
        if (!plat::write_watch_hw_supported() || !slice.base || !slice.size) return false;
        hw_watched = true;
        // Find or register this physical pool for hardware write-watch dirty-page polling.
        WatchedPool* wp = nullptr;
        for (WatchedPool& w : g_watched_pools) {
            if (w.base == slice.base && w.size == slice.size) {
                wp = &w;
                break;
            }
        }
        if (!wp) {
            bool ok = plat::write_watch_hw_reset(slice.base, slice.size);
            g_watched_pools.push_back({slice.base, slice.size, g_static_submit, !ok});
            return false;  // wait 1 submit so GetWriteWatch separates dynamic pages from static pages
        }
        if (wp->broken || g_static_submit <= wp->added_submit) return false;
    }
    static_drain_invalidations();
    const uint32_t h = static_hash(p);
    uint32_t target_idx = UINT32_MAX;
    for (uint32_t probe = 0; probe < kStaticTableSize; ++probe) {
        const uint32_t idx = (h + probe) & (kStaticTableSize - 1);
        StaticSlot& s = g_static_table[idx];
        if (s.generation != g_static_gen) {
            if (target_idx == UINT32_MAX) target_idx = idx;
            break;
        }
        if (!s.valid) {
            if (target_idx == UINT32_MAX) target_idx = idx;
            continue;
        }
        if (s.p == p) {
            if (s.size == size && s.padded >= padded && (s.offset % align) == 0) {
                StaticSlab& sl = g_slabs[s.slab];
                sl.last_serial = frame_recording_serial();
                out = {sl.buffer, s.offset, nullptr};
                g_static_hits++;
                g_static_saved_bytes += padded;
                return true;
            }
            // Same address, different size/alignment: replace this slot.
            if (s.slab < g_slabs.size() && g_slabs[s.slab].live_bytes >= s.padded)
                g_slabs[s.slab].live_bytes -= s.padded;
            s.valid = false;
            target_idx = idx;
            break;
        }
    }
    // Dynamic per-frame buffers live on recently dirtied pages: skip VRAM slab promotion and use dedup_upload.
    if (hw_watched && static_range_is_hot(p, size)) return false;
    if (target_idx == UINT32_MAX || g_static_used >= kStaticTableSize - 64) return false;
    uint32_t slab_idx = 0;
    uint64_t slab_off = 0;
    if (!static_alloc_slab(padded, std::max<uint64_t>(align, 16), slab_idx, slab_off)) return false;
    // Record staging copy into DEVICE_LOCAL VRAM outside any open render pass (only on first-time miss).
    draw_end_rendering(cb, 2);
    Upload stg = upload_alloc(padded, 16);
    std::memcpy(stg.ptr, p, size);
    if (padded > size) std::memset(stg.ptr + size, 0, padded - size);
    VkBufferCopy copy{stg.offset, slab_off, padded};
    p_vkCmdCopyBuffer(cb, stg.buffer, g_slabs[slab_idx].buffer, 1, &copy);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                       VK_ACCESS_SHADER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &mb, 0, nullptr, 0, nullptr);
    StaticSlot& s = g_static_table[target_idx];
    if (s.generation != g_static_gen) ++g_static_used;
    s = {p, size, padded, slab_off, slab_idx, g_static_gen, true};
    static_register_pages(p, size, target_idx);
    out = {g_slabs[slab_idx].buffer, slab_off, nullptr};
    g_static_misses++;
    g_static_uploaded_bytes += padded;
    return true;
}

bool static_buf_upload_immutable(VkCommandBuffer cb, const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align,
                                 Upload& out) {
    // Host-side immutable data (ShaderInfo::constants, zero_stream): never invalidated, cached permanently in VRAM.
    static constexpr PoolSlice kImmutableSlice{0, 0, 0x1u};  // CPU_NO_ACCESS_BIT
    return static_buf_upload(cb, p, size, padded, align, kImmutableSlice, out);
}

void dedup_log() {
    if (g_copied + g_saved)
        log("nvn: uploads copied %.1f MB, reused %.1f MB", double(g_copied) / 1e6, double(g_saved) / 1e6);
    if (g_static_hits + g_static_misses)
        log("nvn: static vb/ib cache: %llu hits (%.1f MB saved), %llu misses (%.1f MB to VRAM), %zu slabs",
            (unsigned long long)g_static_hits, double(g_static_saved_bytes) / 1e6,
            (unsigned long long)g_static_misses, double(g_static_uploaded_bytes) / 1e6, g_slabs.size());
    g_copied = g_saved = 0;
    g_static_hits = g_static_misses = g_static_saved_bytes = g_static_uploaded_bytes = 0;
}

}  // namespace nx::nvn::replay

namespace nx::nvn {

void static_buf_invalidate(uint64_t host_addr, uint64_t size) {
    if (!host_addr || !size) return;
    std::lock_guard lk(replay::g_inval_lock);
    replay::g_inval_queue.emplace_back(uintptr_t(host_addr), size);
    replay::g_inval_pending.fetch_add(1, std::memory_order_release);
}

}  // namespace nx::nvn
