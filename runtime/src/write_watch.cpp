// Write watches (docs/nvn/android-perf/PLAN.md R4): whether the CPU wrote a range since it was last asked, from page
// protection instead of hashing. A watched range is mapped read-only; the first write into one of its pages faults,
// the crash handler (platform_android.cpp, platform_mac.cpp) hands the address here first, and the page is made
// writable again with the range marked written, so the store retries and succeeds. write_watch_take re-protects the
// range and then reads the mark: a store before the re-protection found the mark already set (its page was writable
// only because a fault set it), one after it faults again.
//
// For Wonder's glyph cache (a 16 MB pitch-linear texture the CPU writes without a flush), which was hashed in full
// every 4th frame (~5 ms on a phone, a spike every 4 frames). Only for game code's stores: the kernel writing into a
// read-only page fails the syscall instead (EFAULT), so it is kept to textures, and dropped with them. POSIX only;
// elsewhere (and with NX_WRITE_WATCH=0) write_watch_add fails and callers hash as before.
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "platform.h"

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nx::plat {

#if !defined(_WIN32)
namespace {

struct Watch {
    std::atomic<uint64_t> addr{0};  // the caller's address (0: a free slot)
    std::atomic<uint64_t> begin{0}, end{0};  // the pages it covers
    std::atomic<bool> written{false};
};
constexpr int kWatches = 16;
Watch g_watches[kWatches];

uint64_t page_size() {
    static const uint64_t p = uint64_t(sysconf(_SC_PAGESIZE));
    return p;
}

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_WRITE_WATCH");
        return !(e && e[0] == '0');
    }();
    return on;
}

}  // namespace

bool write_watch_add(uint64_t addr, uint64_t size) {
    if (!enabled() || !addr || !size) return false;
    uint64_t ps = page_size(), b = addr & ~(ps - 1), e = (addr + size + ps - 1) & ~(ps - 1);
    for (Watch& w : g_watches) {
        uint64_t expected = 0;
        if (!w.addr.compare_exchange_strong(expected, addr)) continue;
        w.begin = b, w.end = e, w.written = true;  // unknown until the first take: written
        if (mprotect(reinterpret_cast<void*>(b), e - b, PROT_READ) != 0) {
            w.addr = 0;
            return false;
        }
        return true;
    }
    return false;
}

void write_watch_remove(uint64_t addr) {
    for (Watch& w : g_watches)
        if (w.addr == addr) {
            mprotect(reinterpret_cast<void*>(w.begin.load()), w.end - w.begin, PROT_READ | PROT_WRITE);
            w.begin = w.end = 0;
            w.addr = 0;
        }
}

bool write_watch_take(uint64_t addr) {
    for (Watch& w : g_watches)
        if (w.addr == addr) {
            mprotect(reinterpret_cast<void*>(w.begin.load()), w.end - w.begin, PROT_READ);
            return w.written.exchange(false);
        }
    return true;
}

bool write_watch_fault(void* at) {
    uint64_t a = reinterpret_cast<uint64_t>(at);
    bool hit = false;
    for (Watch& w : g_watches)
        if (w.addr && a >= w.begin && a < w.end) w.written = true, hit = true;
    if (hit) mprotect(reinterpret_cast<void*>(a & ~(page_size() - 1)), page_size(), PROT_READ | PROT_WRITE);
    return hit;
}
bool write_watch_hw_supported() { return false; }
bool write_watch_hw_reset(uint64_t, uint64_t) { return false; }
int write_watch_hw_poll(uint64_t, uint64_t, void**, size_t, uint64_t*) { return -1; }
#else
namespace {

struct WinWatch {
    std::atomic<uint64_t> addr{0};
    std::atomic<uint64_t> begin{0}, end{0};
    std::atomic<bool> written{false};
};
constexpr int kWinWatches = 16;
WinWatch g_win_watches[kWinWatches];

bool win_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_WRITE_WATCH");
        return !(e && e[0] == '0');
    }();
    return on;
}

}  // namespace

bool write_watch_hw_supported() { return win_enabled(); }

bool write_watch_hw_reset(uint64_t addr, uint64_t size) {
    if (!win_enabled() || !addr || !size) return false;
    uint64_t b = addr & ~0xFFFull;
    uint64_t e = (addr + size + 0xFFF) & ~0xFFFull;
    return ResetWriteWatch(reinterpret_cast<void*>(b), SIZE_T(e - b)) == 0;
}

int write_watch_hw_poll(uint64_t addr, uint64_t size, void** out_pages, size_t max_pages, uint64_t* out_granularity) {
    if (!win_enabled() || !addr || !size || !out_pages || !max_pages) return -1;
    uint64_t b = addr & ~0xFFFull;
    uint64_t e = (addr + size + 0xFFF) & ~0xFFFull;
    ULONG_PTR count = ULONG_PTR(max_pages);
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(b), SIZE_T(e - b), out_pages, &count, &gran) != 0)
        return -1;
    if (out_granularity) *out_granularity = gran;
    // Forward any dirty page overlapping a texture write_watch_add() range so polling a pool never drops texture writes.
    for (ULONG_PTR i = 0; i < count; ++i) {
        uint64_t pa = reinterpret_cast<uint64_t>(out_pages[i]);
        for (WinWatch& w : g_win_watches)
            if (w.addr.load(std::memory_order_relaxed) && pa < w.end.load(std::memory_order_relaxed) &&
                pa + gran > w.begin.load(std::memory_order_relaxed))
                w.written.store(true, std::memory_order_relaxed);
    }
    return int(count);
}

bool write_watch_add(uint64_t addr, uint64_t size) {
    if (!win_enabled() || !addr || !size) return false;
    uint64_t b = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (ResetWriteWatch(reinterpret_cast<void*>(b), SIZE_T(e - b)) != 0) return false;
    for (WinWatch& w : g_win_watches) {
        uint64_t expected = 0;
        if (!w.addr.compare_exchange_strong(expected, addr)) continue;
        w.begin = b, w.end = e, w.written = true;
        return true;
    }
    return false;
}

void write_watch_remove(uint64_t addr) {
    for (WinWatch& w : g_win_watches)
        if (w.addr == addr) {
            w.begin = w.end = 0;
            w.addr = 0;
        }
}

bool write_watch_take(uint64_t addr) {
    for (WinWatch& w : g_win_watches) {
        if (w.addr != addr) continue;
        void* pages[64];
        ULONG_PTR count = 64;
        DWORD gran = 4096;
        uint64_t b = w.begin.load(), e = w.end.load();
        bool dirty = w.written.exchange(false);
        if (b < e && GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(b), SIZE_T(e - b), pages, &count,
                                   &gran) == 0) {
            if (count > 0) dirty = true;
            while (count == 64) {
                count = 64;
                if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(b), SIZE_T(e - b), pages, &count,
                                  &gran) != 0)
                    break;
            }
        } else {
            dirty = true;
        }
        return dirty;
    }
    return true;
}

bool write_watch_fault(void*) { return false; }
#endif

}  // namespace nx::plat
