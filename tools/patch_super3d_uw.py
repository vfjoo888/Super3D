#!/usr/bin/env python3
"""Patch Super Mario 3D World + Bowser's Fury (v1.0.0) recompiled main sources for dynamic 21:9 / 32:9 ultrawide.

Based on the verified v1.0.0 aspect ratio sites (0x8FADE0, 0x9692D0, 0x91CFC8, 0x8B6C1C),
using runtime nx::uw_aspect_scale() / nx::uw_aspect_bits() so 16:9, 21:9, and 32:9 all work
from the F1 menu / pc-settings.ini without rebuilding.
"""
import os
import sys

DECL = "namespace nx { float uw_aspect_scale(); uint32_t uw_aspect_bits(); }\n"

PATCHES = [
    (
        "code_181.cpp",
        "main_008B6250 (0x8b6c1c aspect constant)",
        '    c.x[30] = NXB + 0x8b6c1cull; main_00765BC0(c); // bl #0x765bc0\n'
        '    c.x[8] = uint32_t(0x8e39ull); // mov w8, #0x8e39\n'
        '    c.x[8] = uint32_t((uint32_t(c.x[8]) & 0xffffull) | 0x3fe30000ull); // movk w8, #0x3fe3, lsl #16',
        '    c.x[30] = NXB + 0x8b6c1cull; main_00765BC0(c); // bl #0x765bc0\n'
        '    c.x[8] = nx::uw_aspect_bits(); // sm3dw uw patch (0x8b6c1c)',
    ),
    (
        "code_187.cpp",
        "main_008FADD0 (0x8fade0 viewport aspect)",
        '    { float r_ = float((c.v[0].f32[0]) / (c.v[1].f32[0])); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[0] = t_; } // fdiv s0, s0, s1\n'
        '    { uint64_t a = c.x[0] + 0x124ull; nx::st<uint32_t>(a, c.v[0].u32[0]); } // str s0, [x0, #0x124]\n'
        '    if (c.x[2] == 0) goto L_8FAE00; // cbz x2, #0x8fae00\n'
        '    { uint64_t a = c.x[2] + 0x10ull; auto v0 = nx::ld<uint32_t>(a + 0); auto v1 = nx::ld<uint32_t>(a + 4); { nx::V128 t_ = nx::vzero(); t_.u32[0] = v0; c.v[0] = t_; } { nx::V128 t_ = nx::vzero(); t_.u32[0] = v1; c.v[1] = t_; } } // ldp s0, s1, [x2, #0x10]\n'
        '    { uint64_t a = c.x[2] + 0x8ull; auto v0 = nx::ld<uint32_t>(a + 0); auto v1 = nx::ld<uint32_t>(a + 4); { nx::V128 t_ = nx::vzero(); t_.u32[0] = v0; c.v[2] = t_; } { nx::V128 t_ = nx::vzero(); t_.u32[0] = v1; c.v[3] = t_; } } // ldp s2, s3, [x2, #8]\n'
        '    { float r_ = float((c.v[1].f32[0]) - (c.v[3].f32[0])); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[1] = t_; } // fsub s1, s1, s3\n'
        '    { float r_ = float((c.v[0].f32[0]) - (c.v[2].f32[0])); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[0] = t_; } // fsub s0, s0, s2\n'
        '    { float r_ = float((c.v[0].f32[0]) / (c.v[1].f32[0])); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[0] = t_; } // fdiv s0, s0, s1',
        '    { float r_ = float((c.v[0].f32[0]) / (c.v[1].f32[0])) * nx::uw_aspect_scale(); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[0] = t_; } // sm3dw uw patch (0x8fade0)\n'
        '    { uint64_t a = c.x[0] + 0x124ull; nx::st<uint32_t>(a, c.v[0].u32[0]); } // str s0, [x0, #0x124]\n'
        '    if (c.x[2] == 0) goto L_8FAE00; // cbz x2, #0x8fae00\n'
        '    { uint64_t a = c.x[2] + 0x10ull; auto v0 = nx::ld<uint32_t>(a + 0); auto v1 = nx::ld<uint32_t>(a + 4); { nx::V128 t_ = nx::vzero(); t_.u32[0] = v0; c.v[0] = t_; } { nx::V128 t_ = nx::vzero(); t_.u32[0] = v1; c.v[1] = t_; } } // ldp s0, s1, [x2, #0x10]\n'
        '    { uint64_t a = c.x[2] + 0x8ull; auto v0 = nx::ld<uint32_t>(a + 0); auto v1 = nx::ld<uint32_t>(a + 4); { nx::V128 t_ = nx::vzero(); t_.u32[0] = v0; c.v[2] = t_; } { nx::V128 t_ = nx::vzero(); t_.u32[0] = v1; c.v[3] = t_; } } // ldp s2, s3, [x2, #8]\n'
        '    { float r_ = float((c.v[1].f32[0]) - (c.v[3].f32[0])); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[1] = t_; } // fsub s1, s1, s3\n'
        '    { float r_ = float((c.v[0].f32[0]) - (c.v[2].f32[0])); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[0] = t_; } // fsub s0, s0, s2\n'
        '    { float r_ = float((c.v[0].f32[0]) / (c.v[1].f32[0])) * nx::uw_aspect_scale(); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[0] = t_; } // sm3dw uw patch (0x8fade0b)',
    ),
    (
        "code_190.cpp",
        "main_0091CF30 (0x91cfc8 camera aspect)",
        '    c.x[9] = uint64_t(0x8e39ull); // mov x9, #0x8e39\n'
        '    c.x[9] = uint64_t((c.x[9] & 0xffffffff0000ffffull) | 0x3fe30000ull); // movk x9, #0x3fe3, lsl #16\n'
        '    c.x[9] = uint64_t((c.x[9] & 0xffffffffffffull) | 0x41f0000000000000ull); // movk x9, #0x41f0, lsl #48',
        '    c.x[9] = uint64_t(nx::uw_aspect_bits()) | 0x41f0000000000000ull; // sm3dw uw patch (0x91cfc8)',
    ),
    (
        "code_196.cpp",
        "main_00969280 (0x9692d0 projection aspect)",
        '    { float r_ = float((c.v[0].f32[0]) / (c.v[1].f32[0])); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[3] = t_; } // fdiv s3, s0, s1\n'
        '    c.x[8] = uint64_t(NXB + 0xd9f000ull); // adrp x8, #0xd9f000\n'
        '    { uint64_t a = c.x[8] + 0x1e0ull; { nx::V128 t_ = nx::vzero(); t_.u32[0] = nx::ld<uint32_t>(a); c.v[2] = t_; } } // ldr s2, [x8, #0x1e0]',
        '    { float r_ = float((c.v[0].f32[0]) / (c.v[1].f32[0])) * nx::uw_aspect_scale(); nx::V128 t_ = nx::vzero(); t_.f32[0] = r_; c.v[3] = t_; } // sm3dw uw patch (0x9692d0)\n'
        '    c.x[8] = uint64_t(NXB + 0xd9f000ull); // adrp x8, #0xd9f000\n'
        '    { uint64_t a = c.x[8] + 0x1e0ull; { nx::V128 t_ = nx::vzero(); t_.u32[0] = nx::ld<uint32_t>(a); c.v[2] = t_; } } // ldr s2, [x8, #0x1e0]',
    ),
]


def main():
    gen_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join("private", "sm3dw", "gen", "main")
    for fname, label, old, new in PATCHES:
        path = os.path.join(gen_dir, fname)
        with open(path, "r", encoding="utf-8", newline="") as f:
            raw = f.read()
        crlf = "\r\n" in raw
        text = raw.replace("\r\n", "\n")
        if new in text:
            print(f"already patched: {fname} ({label})")
            continue
        count = text.count(old)
        if count != 1:
            raise RuntimeError(f"{fname} ({label}): expected 1 match, found {count}")
        if DECL not in text:
            text = text.replace('#include "funcs.h"\n', '#include "funcs.h"\n' + DECL, 1)
        text = text.replace(old, new, 1)
        if crlf:
            text = text.replace("\n", "\r\n")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print(f"patched: {fname} ({label})")


if __name__ == "__main__":
    main()
