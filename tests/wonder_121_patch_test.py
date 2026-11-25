#!/usr/bin/env python3
"""Tests for Wonder 1.0.0 / 1.2.1 real-time patching, version checks, and merged RSTB selection."""
import os
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from recomp import nso, wonder_realtime  # noqa: E402
import gamebuild  # noqa: E402
import setup_game  # noqa: E402
import wonder_menu_mod  # noqa: E402


def make_module(spec):
    text_size = 0x1800000
    ro_start = text_size
    data_start = text_size + 0x1000
    bss_start = data_start + 0x1000
    image = bytearray(data_start + 0x1000)
    for b in spec.branches:
        struct.pack_into("<" + "I" * len(spec.original), image, b, *spec.original)
    struct.pack_into("<" + "I" * len(spec.aspect_original), image, spec.aspect_site, *spec.aspect_original)
    return nso.Module(
        name="main",
        image=image,
        text=(0, text_size),
        ro=(ro_start, 0x1000),
        data=(data_start, 0x1000),
        bss=(bss_start, 0x1000),
    )


def test_realtime_patches():
    for spec in wonder_realtime.VERSIONS:
        mod = make_module(spec)
        orig_bss_end = (mod.bss[0] + mod.bss[1] + 0xFFF) & ~0xFFF
        wonder_realtime.patch_wonder_realtime(mod)
        assert mod.frame_step_offset == orig_bss_end
        assert mod.bss == (mod.bss[0], orig_bss_end + 0x1000 - mod.bss[0])
        for b in spec.branches:
            words = struct.unpack_from("<" + "I" * len(spec.original), mod.image, b)
            # Every version keeps adrp x8 at word 0, ldr s0,[x8] at word 1, and two trailing NOPs.
            assert (words[0] & 0x9F00001F) == 0x90000008
            assert words[1] == 0xBD400100
            assert words[-2:] == (0xD503201F, 0xD503201F)
            if spec.version == "1.2.1":
                # Preserve adrp x11 at branch+8 in place and read seconds into s1 before x8 is overwritten at word 5.
                assert words[2] == spec.original[2]
                assert words[3] == 0xBD400501
            else:
                assert words[2] == 0xBD400501
        asp = struct.unpack_from("<4I", mod.image, spec.aspect_site)
        assert (asp[0] & 0x9F00001F) == 0x90000009
        assert asp[1] == spec.aspect_original[1]
        assert asp[2] == wonder_realtime.ASPECT_LOAD
        assert asp[3] == spec.aspect_original[3]

    # Unsupported binary must raise ValueError cleanly.
    bad = nso.Module(name="main", image=bytearray(0x10000), text=(0, 0x8000), ro=(0x8000, 0x1000),
                     data=(0x9000, 0x1000), bss=(0xA000, 0x1000))
    try:
        wonder_realtime.patch_wonder_realtime(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("Expected ValueError for unsupported binary")


def test_exefs_and_romfs_checks():
    def write_nso(path, build_id_hex):
        hdr = bytearray(0x80)
        hdr[:4] = b"NSO0"
        hdr[0x40:0x60] = bytes.fromhex(build_id_hex).ljust(32, b"\0")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "wb").write(hdr)

    with tempfile.TemporaryDirectory() as tmp:
        ex100 = os.path.join(tmp, "ex100")
        write_nso(os.path.join(ex100, "main"), "CD6E42AEE7934F4D8393CD43893AE25EDC83D338")
        write_nso(os.path.join(ex100, "sdk"), "3053B22935E32DDF15EF81997603E5C267534A90")
        i100 = gamebuild.inspect_exefs(ex100)
        assert i100["compatible"] and i100["version"] == "1.0.0" and i100["expected_romfs_product"] == 100

        ex121 = os.path.join(tmp, "ex121")
        write_nso(os.path.join(ex121, "main"), "FF773E90972D544EB79406EAA65396D53C43EFB9")
        write_nso(os.path.join(ex121, "sdk"), "E4BD58CAC28CF13A08FB417AA013CDCB3F9622AF")
        i121 = gamebuild.inspect_exefs(ex121)
        assert i121["compatible"] and i121["version"] == "1.2.1" and i121["expected_romfs_product"] == 120

        ex101 = os.path.join(tmp, "ex101")
        write_nso(os.path.join(ex101, "main"), "F91868B88F60D3D500112233445566778899AABB")
        write_nso(os.path.join(ex101, "sdk"), "3053B22935E32DDF15EF81997603E5C267534A90")
        assert not gamebuild.inspect_exefs(ex101)["compatible"]

        rom_merged = os.path.join(tmp, "rom_merged")
        os.makedirs(os.path.join(rom_merged, "Shader"))
        open(os.path.join(rom_merged, "Shader", "Secred.zs"), "wb").write(b"x")
        res = os.path.join(rom_merged, "System", "Resource")
        os.makedirs(res)
        open(os.path.join(res, "ResourceSizeTable.Product.100.rsizetable.zs"), "wb").write(b"1")
        open(os.path.join(res, "ResourceSizeTable.Product.120.rsizetable.zs"), "wb").write(b"2")
        best, ver = wonder_menu_mod.pick_latest_rstb(rom_merged)
        assert ver == 120 and best.endswith("Product.120.rsizetable.zs")
        ri = setup_game.Setup.inspect_romfs(rom_merged)
        assert ri["valid"] and ri["product"] == 120


if __name__ == "__main__":
    test_realtime_patches()
    test_exefs_and_romfs_checks()
    print("wonder_121_patch_test PASSED")
