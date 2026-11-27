#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nx {

namespace detail {

inline uint16_t rd16_le(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t rd32_le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint32_t rd32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void wr16_le(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v & 0xFFu);
    p[1] = uint8_t(v >> 8);
}

inline bool unyaz0(const uint8_t* src, size_t len, std::vector<uint8_t>& out) {
    if (len < 16 || std::memcmp(src, "Yaz0", 4) != 0) return false;
    const uint32_t out_sz = rd32_be(src + 4);
    if (out_sz == 0 || out_sz > 64u * 1024u * 1024u) return false;
    out.clear();
    out.reserve(out_sz);
    size_t i = 16;
    uint8_t mask = 0;
    int bits = 0;
    while (out.size() < out_sz && i < len) {
        if (bits == 0) {
            mask = src[i++];
            bits = 8;
        }
        if (mask & 0x80u) {
            if (i >= len) return false;
            out.push_back(src[i++]);
        } else {
            if (i + 1 >= len) return false;
            const uint8_t b1 = src[i++], b2 = src[i++];
            const size_t dist = (size_t(b1 & 0x0Fu) << 8 | b2) + 1u;
            size_t n = b1 >> 4;
            if (n == 0) {
                if (i >= len) return false;
                n = size_t(src[i++]) + 18u;
            } else {
                n += 2u;
            }
            if (dist > out.size()) return false;
            for (size_t k = 0; k < n && out.size() < out_sz; ++k) out.push_back(out[out.size() - dist]);
        }
        mask <<= 1;
        --bits;
    }
    return out.size() == out_sz;
}

inline void wrap_raw_yaz0(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    const size_t n = raw.size();
    const size_t groups = (n + 7u) / 8u;
    out.clear();
    out.reserve(16u + n + groups);
    out.push_back('Y'); out.push_back('a'); out.push_back('z'); out.push_back('0');
    out.push_back(uint8_t((n >> 24) & 0xFFu));
    out.push_back(uint8_t((n >> 16) & 0xFFu));
    out.push_back(uint8_t((n >> 8) & 0xFFu));
    out.push_back(uint8_t(n & 0xFFu));
    for (int z = 0; z < 8; ++z) out.push_back(0);
    for (size_t i = 0; i < n; i += 8) {
        out.push_back(0xFFu);
        const size_t take = std::min<size_t>(8, n - i);
        out.insert(out.end(), raw.begin() + ptrdiff_t(i), raw.begin() + ptrdiff_t(i + take));
    }
}

inline size_t bc4_swizzle_addr(uint32_t bx, uint32_t by, uint32_t bw, uint32_t blk_h_log2) {
    const uint32_t bh = 1u << blk_h_log2;
    const uint32_t gob_h = 8u * bh;
    const uint32_t width_gobs = (bw * 8u + 63u) / 64u;
    const uint32_t xb = bx * 8u;
    const uint32_t gob_x = xb / 64u;
    const uint32_t gob_y = by / gob_h;
    const uint32_t off_in_gob = (((xb % 64u) / 32u) * 256u) + (((by % 8u) / 2u) * 64u) +
                                (((xb % 32u) / 16u) * 32u) + ((by % 2u) * 16u) + (xb % 16u);
    return size_t(gob_y * width_gobs + gob_x) * size_t(512u * bh) +
           size_t((by % gob_h) / 8u) * 512u + size_t(off_in_gob);
}

inline void decode_bc4_block(const uint8_t* blk, uint8_t out[16]) {
    const uint8_t a0 = blk[0], a1 = blk[1];
    uint8_t pal[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) pal[k + 1] = uint8_t(((7 - k) * int(a0) + k * int(a1)) / 7);
    } else {
        for (int k = 1; k <= 4; ++k) pal[k + 1] = uint8_t(((5 - k) * int(a0) + k * int(a1)) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= uint64_t(blk[2 + k]) << (8 * k);
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (i * 3)) & 7u];
}

inline void encode_bc4_block(const uint8_t in[16], uint8_t* blk) {
    uint8_t lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) lo = std::min(lo, in[i]), hi = std::max(hi, in[i]);
    if (lo == hi) {
        blk[0] = hi;
        blk[1] = lo;
        std::memset(blk + 2, 0, 6);
        return;
    }
    blk[0] = hi;
    blk[1] = lo;
    uint8_t pal[8] = {hi, lo, 0, 0, 0, 0, 0, 0};
    for (int k = 1; k <= 6; ++k) pal[k + 1] = uint8_t(((7 - k) * int(hi) + k * int(lo)) / 7);
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, best_d = 256;
        for (int p = 0; p < 8; ++p) {
            int d = std::abs(int(in[i]) - int(pal[p]));
            if (d < best_d) best_d = d, best = p;
        }
        bits |= uint64_t(best) << (i * 3);
    }
    for (int k = 0; k < 6; ++k) blk[2 + k] = uint8_t((bits >> (8 * k)) & 0xFFu);
}

inline float sd_segment(float px, float py, float ax, float ay, float bx, float by) {
    const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    const float h = std::clamp((pax * bax + pay * bay) / std::max(1e-6f, bax * bax + bay * bay), 0.0f, 1.0f);
    const float dx = pax - bax * h, dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

inline float sd_box(float px, float py, float bx, float by) {
    const float dx = std::fabs(px) - bx, dy = std::fabs(py) - by;
    const float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f);
}

inline float sd_char(char ch, float x, float y) {
    const float hw = 4.2f, hh = 6.8f;
    switch (ch) {
        case 'L':
            return std::min(sd_segment(x, y, -hw, -hh, -hw, hh), sd_segment(x, y, -hw, hh, hw, hh));
        case 'R': {
            float d = sd_segment(x, y, -hw, -hh, -hw, hh);
            d = std::min(d, sd_segment(x, y, -hw, -hh, hw * 0.5f, -hh));
            d = std::min(d, sd_segment(x, y, -hw, 0.0f, hw * 0.5f, 0.0f));
            d = std::min(d, sd_segment(x, y, hw * 0.5f, -hh, hw, -hh * 0.5f));
            d = std::min(d, sd_segment(x, y, hw, -hh * 0.5f, hw * 0.5f, 0.0f));
            d = std::min(d, sd_segment(x, y, 0.0f, 0.0f, hw, hh));
            return d;
        }
        case 'B': {
            float d = sd_segment(x, y, -hw, -hh, -hw, hh);
            d = std::min(d, sd_segment(x, y, -hw, -hh, hw * 0.6f, -hh));
            d = std::min(d, sd_segment(x, y, -hw, 0.0f, hw * 0.7f, 0.0f));
            d = std::min(d, sd_segment(x, y, -hw, hh, hw * 0.7f, hh));
            d = std::min(d, sd_segment(x, y, hw * 0.6f, -hh, hw * 0.85f, -hh * 0.5f));
            d = std::min(d, sd_segment(x, y, hw * 0.85f, -hh * 0.5f, hw * 0.6f, 0.0f));
            d = std::min(d, sd_segment(x, y, hw * 0.7f, 0.0f, hw, hh * 0.5f));
            d = std::min(d, sd_segment(x, y, hw, hh * 0.5f, hw * 0.7f, hh));
            return d;
        }
        case 'T':
            return std::min(sd_segment(x, y, -hw, -hh, hw, -hh), sd_segment(x, y, 0.0f, -hh, 0.0f, hh));
        case '1': {
            float d = sd_segment(x, y, 0.5f, -hh, 0.5f, hh);
            d = std::min(d, sd_segment(x, y, -hw * 0.6f, -hh * 0.55f, 0.5f, -hh));
            d = std::min(d, sd_segment(x, y, -hw * 0.7f, hh, hw * 0.8f, hh));
            return d;
        }
        case '2': {
            float d = sd_segment(x, y, -hw * 0.8f, -hh * 0.6f, -hw * 0.2f, -hh);
            d = std::min(d, sd_segment(x, y, -hw * 0.2f, -hh, hw * 0.6f, -hh));
            d = std::min(d, sd_segment(x, y, hw * 0.6f, -hh, hw * 0.9f, -hh * 0.45f));
            d = std::min(d, sd_segment(x, y, hw * 0.9f, -hh * 0.45f, -hw * 0.9f, hh));
            d = std::min(d, sd_segment(x, y, -hw * 0.9f, hh, hw * 0.9f, hh));
            return d;
        }
        default:
            return 100.0f;
    }
}

enum GlyphKind {
    GlyphPsCross,
    GlyphPsCircle,
    GlyphPsSquare,
    GlyphPsTriangle,
    GlyphPillLB,
    GlyphPillRB,
    GlyphPillLT,
    GlyphPillRT,
    GlyphPillL1,
    GlyphPillR1,
    GlyphPillL2,
    GlyphPillR2,
    GlyphCircleMenu,
    GlyphCircleView,
    GlyphCircleTouchpad,
};

// Renders a crisp SDF button glyph inside a vertically-flipped (cell_w+1)x(cell_h+1) BC4 cell.
inline void draw_sdf_glyph_bc4(uint8_t* sheet, uint32_t sheet_w, uint32_t blk_h_log2,
                               uint32_t cell_x, uint32_t cell_y, uint32_t stride_w, uint32_t stride_h, GlyphKind kind) {
    const uint32_t bw = sheet_w / 4u;
    const float cx = float(cell_x) + float(stride_w - 1u) * 0.5f;
    // Original 50px icons sit centered at y=30.0 in the 69px bottom-up cell (leaving bottom descender padding).
    const float cy = float(cell_y) + 30.0f;
    const float outer_r = 24.5f;
    const float ring_thick = 2.8f;

    const uint32_t bx0 = cell_x / 4u, by0 = cell_y / 4u;
    const uint32_t bx1 = (cell_x + stride_w - 1u) / 4u, by1 = (cell_y + stride_h - 1u) / 4u;
    for (uint32_t by = by0; by <= by1; ++by) {
        for (uint32_t bx = bx0; bx <= bx1; ++bx) {
            uint8_t* blk = sheet + bc4_swizzle_addr(bx, by, bw, blk_h_log2);
            uint8_t pxls[16];
            decode_bc4_block(blk, pxls);
            for (uint32_t py = 0; py < 4u; ++py) {
                for (uint32_t px = 0; px < 4u; ++px) {
                    const uint32_t gx = bx * 4u + px, gy = by * 4u + py;
                    if (gx < cell_x || gx >= cell_x + stride_w || gy < cell_y || gy >= cell_y + stride_h) continue;
                    const float x = float(gx) + 0.5f - cx;
                    // Texture sheet is stored bottom-up, so invert y for upright SDF evaluation.
                    const float y = -(float(gy) + 0.5f - cy);
                    const float r = std::sqrt(x * x + y * y);

                    float d = 100.0f;
                    if (kind <= GlyphPsTriangle) {
                        const float d_ring = std::fabs(r - outer_r) - ring_thick * 0.5f;
                        float d_sym = 100.0f;
                        if (kind == GlyphPsCross) {
                            const float s = 9.5f;
                            d_sym = std::min(sd_segment(x, y, -s, -s, s, s), sd_segment(x, y, -s, s, s, -s)) - 2.3f;
                        } else if (kind == GlyphPsCircle) {
                            d_sym = std::fabs(r - 10.5f) - 2.2f;
                        } else if (kind == GlyphPsSquare) {
                            d_sym = std::fabs(sd_box(x, y, 9.5f, 9.5f)) - 2.2f;
                        } else {
                            const float rt = 11.5f;
                            const float ax = 0.0f, ay = -rt;
                            const float bxp = rt * 0.866f, byp = rt * 0.55f;
                            const float cxp = -bxp, cyp = byp;
                            d_sym = std::min({sd_segment(x, y, ax, ay, bxp, byp),
                                              sd_segment(x, y, bxp, byp, cxp, cyp),
                                              sd_segment(x, y, cxp, cyp, ax, ay)}) - 2.2f;
                        }
                        d = std::min(d_ring, d_sym);
                    } else if (kind >= GlyphPillLB && kind <= GlyphPillR2) {
                        // Solid rounded pill with dark cutout 2-char label (matches Switch L/R/ZL/ZR size).
                        const char c0 = (kind == GlyphPillLB || kind == GlyphPillLT || kind == GlyphPillL1 || kind == GlyphPillL2) ? 'L' : 'R';
                        const char c1 = (kind == GlyphPillLB || kind == GlyphPillRB) ? 'B'
                                      : (kind == GlyphPillLT || kind == GlyphPillRT) ? 'T'
                                      : (kind == GlyphPillL1 || kind == GlyphPillR1) ? '1' : '2';
                        const float d_pill = sd_box(x, y, 20.0f, 13.0f) - 6.0f;
                        const float d_txt = std::min(sd_char(c0, x + 7.0f, y), sd_char(c1, x - 7.0f, y)) - 1.75f;
                        d = std::max(d_pill, -d_txt);
                    } else {
                        // Solid circle with cutout Menu / View / Touchpad icon (matches Switch +/- size).
                        const float d_circ = r - outer_r;
                        float d_ico = 100.0f;
                        if (kind == GlyphCircleMenu) {
                            d_ico = std::min({sd_segment(x, y, -10.5f, -7.0f, 10.5f, -7.0f),
                                              sd_segment(x, y, -10.5f, 0.0f, 10.5f, 0.0f),
                                              sd_segment(x, y, -10.5f, 7.0f, 10.5f, 7.0f)}) - 2.0f;
                        } else if (kind == GlyphCircleView) {
                            const float b1 = std::fabs(sd_box(x + 3.5f, y + 3.0f, 6.5f, 5.0f)) - 1.5f;
                            const float b2 = std::fabs(sd_box(x - 3.5f, y - 3.0f, 6.5f, 5.0f)) - 1.5f;
                            d_ico = std::min(b1, b2);
                        } else {
                            d_ico = std::fabs(sd_box(x, y, 11.5f, 7.5f) - 1.5f) - 1.8f;
                        }
                        d = std::max(d_circ, -d_ico);
                    }
                    const float cov = std::clamp(0.75f - d * 0.7f, 0.0f, 1.0f);
                    pxls[py * 4u + px] = uint8_t(std::lround(cov * 255.0f));
                }
            }
            encode_bc4_block(pxls, blk);
        }
    }
}

inline bool patch_bffnt_buttons(uint8_t* b, size_t len, int style, const std::array<int, 16>& action_to_physical) {
    if (len < 0x40 || std::memcmp(b, "FFNT", 4) != 0) return false;

    size_t tglp = 0, cmap = 0;
    for (size_t i = 0; i + 16 < len; ++i) {
        if (!tglp && std::memcmp(b + i, "TGLP", 4) == 0) tglp = i;
        if (std::memcmp(b + i, "CMAP", 4) == 0) {
            if (rd16_le(b + i + 16) == 2) { cmap = i; break; }
        }
    }
    if (!tglp || !cmap || cmap + 28 > len) return false;

    const uint8_t cell_w = b[tglp + 8];
    const uint8_t cell_h = b[tglp + 9];
    const uint16_t cols = rd16_le(b + tglp + 20);
    const uint16_t rows = rd16_le(b + tglp + 22);
    const uint16_t n_ent = rd16_le(b + cmap + 24);
    if (!cols || !rows || cmap + 28 + size_t(n_ent) * 8u > len) return false;

    // 0..3: A,B,X,Y (E0E0..E0E3); 4..7: L,R,ZL,ZR (E0E4..E0E7); 8: + (E0EF), 9: - (E0F0)
    size_t idx_e[10]{};
    bool found[10]{};
    uint16_t orig_gid[10]{};
    for (uint16_t k = 0; k < n_ent; ++k) {
        const size_t eoff = cmap + 28 + size_t(k) * 8u;
        const uint32_t code = rd32_le(b + eoff);
        int slot = -1;
        if (code >= 0xE0E0u && code <= 0xE0E7u) slot = int(code - 0xE0E0u);
        else if (code == 0xE0EFu) slot = 8;
        else if (code == 0xE0F0u) slot = 9;
        if (slot >= 0) {
            idx_e[slot] = eoff;
            orig_gid[slot] = rd16_le(b + eoff + 4);
            found[slot] = true;
        }
    }
    if (!found[0] || !found[1] || !found[2] || !found[3]) return false;

    if (style == 2 || style == 3) {
        size_t bntx = 0, brti = 0, brtd = 0;
        for (size_t i = tglp; i + 16 < len; ++i) {
            if (!bntx && std::memcmp(b + i, "BNTX", 4) == 0) bntx = i;
            else if (bntx && !brti && std::memcmp(b + i, "BRTI", 4) == 0) brti = i;
            else if (bntx && !brtd && std::memcmp(b + i, "BRTD", 4) == 0) { brtd = i; break; }
        }
        if (brti && brtd && brti + 0x38 <= len) {
            const uint32_t fmt = rd32_le(b + brti + 0x1c);
            const uint32_t w = rd32_le(b + brti + 0x24);
            const uint32_t h = rd32_le(b + brti + 0x28);
            const uint32_t arr = rd32_le(b + brti + 0x30);
            const uint32_t blk_log2 = rd32_le(b + brti + 0x34) & 7u;
            const size_t sheet_bytes = size_t(w) * size_t(h) / 2u;
            if (fmt == 0x1d01u && w == 1024u && h == 1024u && brtd + 16u + size_t(arr) * sheet_bytes <= len) {
                const uint32_t per_sheet = uint32_t(cols) * uint32_t(rows);
                const uint32_t stride_x = uint32_t(cell_w) + 1u;
                const uint32_t stride_y = uint32_t(cell_h) + 1u;
                auto draw_at = [&](uint32_t gid, GlyphKind gk) {
                    const uint32_t sh = gid / per_sheet, local = gid % per_sheet;
                    if (sh >= arr) return;
                    const uint32_t col = local % cols, row = local / cols;
                    const uint32_t cell_y = h - (row + 1u) * stride_y;
                    uint8_t* sheet = b + brtd + 16u + size_t(sh) * sheet_bytes;
                    draw_sdf_glyph_bc4(sheet, w, blk_log2, col * stride_x, cell_y, stride_x, stride_y, gk);
                };
                if (style == 3) {
                    draw_at(orig_gid[0], GlyphPsCross);
                    draw_at(orig_gid[1], GlyphPsCircle);
                    draw_at(orig_gid[2], GlyphPsSquare);
                    draw_at(orig_gid[3], GlyphPsTriangle);
                }
                if (found[4]) draw_at(orig_gid[4], style == 2 ? GlyphPillLB : GlyphPillL1);
                if (found[5]) draw_at(orig_gid[5], style == 2 ? GlyphPillRB : GlyphPillR1);
                if (found[6]) draw_at(orig_gid[6], style == 2 ? GlyphPillLT : GlyphPillL2);
                if (found[7]) draw_at(orig_gid[7], style == 2 ? GlyphPillRT : GlyphPillR2);
                if (found[8]) draw_at(orig_gid[8], GlyphCircleMenu);
                if (found[9]) draw_at(orig_gid[9], style == 2 ? GlyphCircleView : GlyphCircleTouchpad);
            }
        }
    }

    uint16_t phys_gid[4]{};
    if (style == 1) {
        phys_gid[0] = orig_gid[1];
        phys_gid[1] = orig_gid[0];
        phys_gid[2] = orig_gid[3];
        phys_gid[3] = orig_gid[2];
    } else {
        phys_gid[0] = orig_gid[0];
        phys_gid[1] = orig_gid[1];
        phys_gid[2] = orig_gid[2];
        phys_gid[3] = orig_gid[3];
    }
    for (int a = 0; a < 4; ++a) {
        const int phys = std::clamp(action_to_physical[size_t(a)], 0, 3);
        wr16_le(b + idx_e[a] + 4, phys_gid[phys]);
    }
    return true;
}

}  // namespace detail

inline bool remap_font_data_szs(const std::vector<uint8_t>& szs_in, int style,
                                const std::array<int, 16>& action_to_physical, std::vector<uint8_t>& szs_out) {
    std::vector<uint8_t> sarc;
    if (!detail::unyaz0(szs_in.data(), szs_in.size(), sarc)) return false;
    if (sarc.size() < 32 || std::memcmp(sarc.data(), "SARC", 4) != 0) return false;
    const uint32_t data_off = detail::rd32_le(sarc.data() + 12);
    const uint16_t cnt = detail::rd16_le(sarc.data() + 26);
    const size_t sfnt_off = 20u + 12u + size_t(cnt) * 16u + 8u;
    if (sfnt_off > sarc.size()) return false;

    bool patched = false;
    for (uint16_t i = 0; i < cnt; ++i) {
        const size_t off = 20u + 12u + size_t(i) * 16u;
        if (off + 16u > sarc.size()) break;
        const uint32_t attr = detail::rd32_le(sarc.data() + off + 4);
        const uint32_t start = detail::rd32_le(sarc.data() + off + 8);
        const uint32_t end = detail::rd32_le(sarc.data() + off + 12);
        const size_t name_off = sfnt_off + size_t(attr & 0xFFFFu) * 4u;
        if (name_off >= sarc.size()) continue;
        const char* name = reinterpret_cast<const char*>(sarc.data() + name_off);
        if (std::strcmp(name, "nintendo_NTLG_DB_002_Switch_50px.bffnt") == 0 &&
            size_t(data_off) + size_t(end) <= sarc.size() && end > start) {
            patched = detail::patch_bffnt_buttons(sarc.data() + data_off + start, size_t(end - start), style,
                                                  action_to_physical);
            break;
        }
    }
    if (!patched) return false;
    detail::wrap_raw_yaz0(sarc, szs_out);
    return true;
}

}  // namespace nx
