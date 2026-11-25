#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ttf_cmap_remap.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) << 8 | uint16_t(p[1])); }
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
void push16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
void push32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
void wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

uint32_t checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    const size_t full = len & ~size_t(3);
    for (size_t i = 0; i < full; i += 4) sum += rd32(data + i);
    if (full < len) {
        uint8_t tail[4]{};
        std::memcpy(tail, data + full, len - full);
        sum += rd32(tail);
    }
    return sum;
}

std::vector<uint8_t> build_test_ttf(bool include_extended_sets) {
    struct Seg {
        uint16_t start, end, delta;
    };
    std::vector<Seg> segs;
    segs.push_back({0x0041, 0x005A, uint16_t(10 - 0x0041)});
    segs.push_back({0xE0A0, 0xE0AE, uint16_t(100 - 0xE0A0)});
    segs.push_back({0xE0E0, 0xE0F0, uint16_t(200 - 0xE0E0)});
    if (include_extended_sets) segs.push_back({0xE200, 0xE22F, uint16_t(500 - 0xE200)});
    segs.push_back({0xFFFFu, 0xFFFFu, 1});

    const uint16_t seg_count = uint16_t(segs.size());
    const uint16_t f4_len = uint16_t(16 + 8 * seg_count);
    std::vector<uint8_t> f4;
    push16(f4, 4);
    push16(f4, f4_len);
    push16(f4, 0);
    push16(f4, uint16_t(seg_count * 2));
    uint16_t pow2 = 1, sel = 0;
    while (uint16_t(pow2 << 1) <= seg_count) pow2 <<= 1, ++sel;
    push16(f4, uint16_t(pow2 * 2));
    push16(f4, sel);
    push16(f4, uint16_t(seg_count * 2 - pow2 * 2));
    for (const auto& s : segs) push16(f4, s.end);
    push16(f4, 0);
    for (const auto& s : segs) push16(f4, s.start);
    for (const auto& s : segs) push16(f4, s.delta);
    for (size_t i = 0; i < seg_count; ++i) push16(f4, 0);

    std::vector<uint8_t> cmap;
    push16(cmap, 0);
    push16(cmap, 1);
    push16(cmap, 3);
    push16(cmap, 1);
    push32(cmap, 12);
    cmap.insert(cmap.end(), f4.begin(), f4.end());

    std::vector<uint8_t> head(54, 0);
    wr32(head.data(), 0x00010000u);
    wr32(head.data() + 12, 0x5F0F3CF5u);

    std::vector<uint8_t> ttf;
    push32(ttf, 0x00010000u);
    push16(ttf, 2);
    push16(ttf, 32);
    push16(ttf, 1);
    push16(ttf, 0);

    const uint32_t cmap_off = 12 + 32;
    const uint32_t cmap_padded = uint32_t((cmap.size() + 3) & ~3u);
    const uint32_t head_off = cmap_off + cmap_padded;

    ttf.insert(ttf.end(), {'c', 'm', 'a', 'p'});
    push32(ttf, checksum(cmap.data(), cmap.size()));
    push32(ttf, cmap_off);
    push32(ttf, uint32_t(cmap.size()));

    ttf.insert(ttf.end(), {'h', 'e', 'a', 'd'});
    push32(ttf, checksum(head.data(), head.size()));
    push32(ttf, head_off);
    push32(ttf, uint32_t(head.size()));

    ttf.insert(ttf.end(), cmap.begin(), cmap.end());
    while (ttf.size() % 4 != 0) ttf.push_back(0);
    ttf.insert(ttf.end(), head.begin(), head.end());
    while (ttf.size() % 4 != 0) ttf.push_back(0);
    wr32(ttf.data() + head_off + 8, 0xB1B0AFBAu - checksum(ttf.data(), ttf.size()));
    return ttf;
}

uint16_t lookup_format4(const uint8_t* sub, uint16_t cp) {
    const uint16_t seg_count = rd16(sub + 6) / 2;
    const size_t end_off = 14;
    const size_t start_off = 16 + 2 * seg_count;
    const size_t delta_off = 16 + 4 * seg_count;
    const size_t range_off = 16 + 6 * seg_count;
    for (size_t s = 0; s < seg_count; ++s) {
        const uint16_t ec = rd16(sub + end_off + 2 * s);
        const uint16_t sc = rd16(sub + start_off + 2 * s);
        if (cp < sc || cp > ec) continue;
        const uint16_t delta = rd16(sub + delta_off + 2 * s);
        assert(rd16(sub + range_off + 2 * s) == 0);
        return uint16_t((cp + delta) & 0xFFFFu);
    }
    return 0;
}

uint16_t lookup_format12(const uint8_t* sub, uint32_t cp) {
    const uint32_t groups = rd32(sub + 12);
    for (uint32_t g = 0; g < groups; ++g) {
        const uint8_t* p = sub + 16 + g * 12;
        const uint32_t sc = rd32(p), ec = rd32(p + 4), sg = rd32(p + 8);
        if (cp >= sc && cp <= ec) return uint16_t(sg + (cp - sc));
    }
    return 0;
}

void verify_ttf(const std::vector<uint8_t>& ttf,
                const std::vector<std::pair<uint32_t, uint32_t>>& expected_matches) {
    assert(checksum(ttf.data(), ttf.size()) == 0xB1B0AFBAu);
    const uint16_t num = rd16(ttf.data() + 4);
    uint32_t cmap_off = 0, cmap_len = 0, head_off = 0, head_len = 0, cmap_cs = 0;
    for (uint16_t i = 0; i < num; ++i) {
        const uint8_t* rec = ttf.data() + 12 + i * 16;
        if (std::memcmp(rec, "cmap", 4) == 0)
            cmap_cs = rd32(rec + 4), cmap_off = rd32(rec + 8), cmap_len = rd32(rec + 12);
        else if (std::memcmp(rec, "head", 4) == 0)
            head_off = rd32(rec + 8), head_len = rd32(rec + 12);
    }
    assert(cmap_off && cmap_len && head_off && head_len);
    assert(cmap_cs == checksum(ttf.data() + cmap_off, cmap_len));
    const uint8_t* cmap = ttf.data() + cmap_off;
    assert(rd16(cmap + 2) == 3);
    const uint8_t* f4 = nullptr;
    const uint8_t* f12 = nullptr;
    for (int i = 0; i < 3; ++i) {
        const uint8_t* r = cmap + 4 + i * 8;
        const uint32_t off = rd32(r + 4);
        const uint16_t pid = rd16(r), eid = rd16(r + 2);
        if (pid == 3 && eid == 1) f4 = cmap + off;
        if (pid == 3 && eid == 10) f12 = cmap + off;
    }
    assert(f4 && rd16(f4) == 4);
    assert(f12 && rd16(f12) == 12);
    for (const auto& [dst, src] : expected_matches) {
        assert(dst <= 0xFFFE && src <= 0xFFFE);
        const uint16_t g4_dst = lookup_format4(f4, uint16_t(dst));
        const uint16_t g4_src = lookup_format4(f4, uint16_t(src));
        const uint16_t g12_dst = lookup_format12(f12, dst);
        const uint16_t g12_src = lookup_format12(f12, src);
        assert(g4_dst != 0 && g4_dst == g4_src);
        assert(g12_dst == g4_dst && g12_src == g4_src);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::array<int, 16> nintendo_bindings = {1, 0, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    if (argc == 3) {
        std::ifstream in(argv[1], std::ios::binary);
        assert(in);
        std::vector<uint8_t> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(nx::remap_button_font_cmap(ttf, 3, nintendo_bindings));
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        assert(out);
        out.write(reinterpret_cast<const char*>(ttf.data()), std::streamsize(ttf.size()));
        assert(out.good());
        return 0;
    }

    // 1. Legacy font without 0xE200..0xE22F stays untouched.
    std::vector<uint8_t> legacy = build_test_ttf(false);
    const std::vector<uint8_t> legacy_copy = legacy;
    assert(!nx::remap_button_font_cmap(legacy, 3, nintendo_bindings));
    assert(legacy == legacy_copy);

    // 2. PlayStation style with Nintendo layout (A -> East Circle, B -> South Cross).
    std::vector<uint8_t> ps = build_test_ttf(true);
    assert(nx::remap_button_font_cmap(ps, 3, nintendo_bindings));
    verify_ttf(ps, {
        {0xE0E0, 0xE221}, {0xE0A0, 0xE221},  // A -> ps_circle
        {0xE0E1, 0xE220}, {0xE0A1, 0xE220},  // B -> ps_cross
        {0xE0E2, 0xE223},                    // X -> ps_triangle
        {0xE0E3, 0xE222},                    // Y -> ps_square
        {0xE0E4, 0xE224},                    // L -> ps_l1
        {0xE0E6, 0xE226},                    // ZL -> ps_l2
        {0xE0EF, 0xE228},                    // + -> ps_options
        {0xE0F0, 0xE229},                    // - -> ps_create
    });

    // 3. Custom remapping on Xbox style (CtlA -> PhyTriggerL(6), CtlB -> PhyWest(2)).
    std::array<int, 16> custom = nintendo_bindings;
    custom[0] = 6;
    custom[1] = 2;
    std::vector<uint8_t> xb = build_test_ttf(true);
    assert(nx::remap_button_font_cmap(xb, 2, custom));
    verify_ttf(xb, {
        {0xE0E0, 0xE216},  // A -> xbox_lt
        {0xE0E1, 0xE212},  // B -> xbox_west (X)
        {0xE0E4, 0xE214},  // L -> xbox_lb
        {0xE0EF, 0xE218},  // + -> xbox_menu
        {0xE0F0, 0xE219},  // - -> xbox_view
    });
    return 0;
}
