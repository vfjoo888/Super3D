"""Generate 16:10, 21:9 and 32:9 UI & HUD layout mods for Super Mario 3D World + Bowser's Fury."""
import argparse
import os
import struct


def unyaz0(data: bytes) -> bytes:
    if data[:4] != b'Yaz0':
        return data
    out_len = struct.unpack_from('>I', data, 4)[0]
    out = bytearray(out_len)
    src = 16
    dst = 0
    mask = 0
    bits = 0
    while dst < out_len:
        if bits == 0:
            mask = data[src]
            src += 1
            bits = 8
        if mask & 0x80:
            out[dst] = data[src]
            src += 1
            dst += 1
        else:
            b1 = data[src]
            b2 = data[src + 1]
            src += 2
            copy_src = dst - (((b1 & 0x0F) << 8) | b2) - 1
            if (b1 >> 4) == 0:
                length = data[src] + 0x12
                src += 1
            else:
                length = (b1 >> 4) + 2
            for _ in range(length):
                out[dst] = out[copy_src]
                copy_src += 1
                dst += 1
        mask = (mask << 1) & 0xFF
        bits -= 1
    return bytes(out)


def yaz0_store(data: bytes) -> bytes:
    """Valid Yaz0 stream using literal 8-byte groups (fast and exact)."""
    n = len(data)
    out = bytearray(b'Yaz0')
    out += struct.pack('>I', n)
    out += b'\x00' * 8
    for i in range(0, n, 8):
        chunk = data[i : i + 8]
        out.append(0xFF)
        out += chunk
    return bytes(out)


def sarc_nodes(sarc: bytes):
    assert sarc[:4] == b'SARC'
    hdr_len = struct.unpack_from('<H', sarc, 4)[0]
    data_off = struct.unpack_from('<I', sarc, 12)[0]
    sfat_off = hdr_len
    node_cnt = struct.unpack_from('<H', sarc, sfat_off + 6)[0]
    sfnt_off = sfat_off + 12 + node_cnt * 16 + 8
    nodes = []
    for i in range(node_cnt):
        no = sfat_off + 12 + i * 16
        name_off = (struct.unpack_from('<I', sarc, no + 4)[0] & 0xFFFFFF) * 4
        s_off, e_off = struct.unpack_from('<II', sarc, no + 8)
        end = sarc.index(0, sfnt_off + name_off)
        name = sarc[sfnt_off + name_off : end].decode('utf-8')
        nodes.append((name, no, data_off + s_off, data_off + e_off))
    return nodes


def sarc_get(sarc: bytes, target: str) -> bytes | None:
    for name, _, s, e in sarc_nodes(sarc):
        if name == target:
            return sarc[s:e]
    return None


def sarc_replace(sarc: bytes, replacements: dict[str, bytes]) -> bytes:
    nodes = sarc_nodes(sarc)
    data_off = struct.unpack_from('<I', sarc, 12)[0]
    header_and_tables = bytearray(sarc[:data_off])
    new_data = bytearray()
    for name, no, s, e in nodes:
        blob = replacements.get(name, sarc[s:e])
        pad = (-len(new_data)) & 7
        if pad:
            new_data += b'\x00' * pad
        start = len(new_data)
        new_data += blob
        end = len(new_data)
        struct.pack_into('<II', header_and_tables, no + 8, start, end)
    out = bytes(header_and_tables) + bytes(new_data)
    out = bytearray(out)
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def flyt_sections(b: bytes):
    assert b[:4] == b'FLYT'
    cnt = struct.unpack_from('<H', b, 0x10)[0]
    off = struct.unpack_from('<H', b, 0x06)[0]
    for _ in range(cnt):
        if off + 8 > len(b):
            break
        tag = b[off : off + 4]
        sz = struct.unpack_from('<I', b, off + 4)[0]
        yield off, tag, sz
        off += sz


def pane_name(b: bytes, off: int) -> str:
    return b[off + 12 : off + 36].split(b'\x00', 1)[0].decode('utf-8', 'replace')


def wrap_panes(b: bytes, targets: tuple[tuple[str, float], ...]) -> bytes:
    """Wrap named panes in a UW_<name> parent pan1 shifted horizontally by `shift`."""
    out = bytearray(b)
    for name, shift in targets:
        sections = list(flyt_sections(out))
        found = [
            (i, o, s)
            for i, (o, t, s) in enumerate(sections)
            if t in (b'pan1', b'pic1', b'prt1', b'wnd1', b'txt1') and pane_name(out, o) == name
        ]
        if len(found) != 1:
            continue
        i, o, _ = found[0]
        j = i + 1
        if j < len(sections) and sections[j][1] == b'usd1':
            j += 1
        if j < len(sections) and sections[j][1] == b'pas1':
            depth = 0
            while j < len(sections):
                typ = sections[j][1]
                if typ == b'pas1':
                    depth += 1
                elif typ == b'pae1':
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            if depth:
                raise ValueError(f'unbalanced pane tree for {name}')
        end = sections[j][0] if j < len(sections) else len(out)
        root_o, _, root_size = next(
            (a, t, c) for a, t, c in sections if t == b'pan1' and pane_name(out, a) == 'RootPane'
        )
        parent = bytearray(out[root_o : root_o + root_size])
        parent[12:36] = ('UW_' + name).encode('utf-8').ljust(24, b'\x00')[:24]
        struct.pack_into('<fff', parent, 0x2C, shift, 0.0, 0.0)
        struct.pack_into('<fff', parent, 0x38, 0.0, 0.0, 0.0)
        struct.pack_into('<ff', parent, 0x44, 1.0, 1.0)
        original = out[o:end]
        out[o:end] = parent + b'pas1' + struct.pack('<I', 8) + original + b'pae1' + struct.pack('<I', 8)
        struct.pack_into('<I', out, 0x0C, len(out))
        struct.pack_into('<H', out, 0x10, struct.unpack_from('<H', out, 0x10)[0] + 3)
    return bytes(out)


def widen_full_panes(b: bytes, factor: float, explicit_names: tuple[str, ...] = ()) -> bytes:
    """Widen centered full-screen background/bar/fade panes to match the target aspect ratio."""
    out = bytearray(b)
    for o, t, _ in flyt_sections(out):
        if t not in (b'pic1', b'wnd1'):
            continue
        nm = pane_name(out, o)
        px = struct.unpack_from('<f', out, o + 0x2C)[0]
        w = struct.unpack_from('<f', out, o + 0x4C)[0]
        if nm in explicit_names or (w >= 1200.0 and abs(px) <= 8.0):
            struct.pack_into('<f', out, o + 0x4C, w * factor)
    return bytes(out)


# Per-archive rules: (bflyt_name, ((pane_name, sign), ...), widen_backgrounds, explicit_widen_panes)
RULES = {
    'CourseSelectSceneLayout.szs': (
        'blyt/CourseSelectSceneLayout.bflyt',
        (('Counters', -1), ('CounterGreenStarTotal', -1), ('World', -1), ('Menu', 1)),
        False,
        (),
    ),
    'StageSceneLayout.szs': (
        'blyt/StageSceneLayout.bflyt',
        (
            ('CounterPlayer', -1),
            ('CounterCoin', -1),
            ('CounterGreenStar', -1),
            ('CounterStamp', -1),
            ('ItemStock', -1),
            ('CounterTime', 1),
            ('CounterScore', 1),
            ('NetworkQuality', 1),
        ),
        False,
        (),
    ),
    'CameraIcon.szs': (
        'blyt/CameraIcon.bflyt',
        (('Normal', 1), ('Brigade', 1)),
        False,
        (),
    ),
    'SingleModeSceneLayout.szs': (
        'blyt/SingleModeSceneLayout.bflyt',
        (
            ('CounterGoalItem', -1),
            ('CounterScenarioShine', -1),
            ('CounterCoin', -1),
            ('ItemStock', -1),
            ('Menu', 1),
        ),
        False,
        (),
    ),
    'TitleLogo.szs': (
        'blyt/TitleLogo.bflyt',
        (('ParButtonLuigiBros', -1),),
        True,
        ('PicBackgroud',),
    ),
    'RCS_TopMenu.szs': (
        'blyt/RCS_TopMenu.bflyt',
        (('AButton', 1),),
        True,
        ('PicBG', 'Pic3DWorld', 'Pic3DWorldSh', 'PicFade', 'PicBackgroud'),
    ),
    'RCS_ControlGuideBarParts.szs': (
        'blyt/RCS_ControlGuideBarParts.bflyt',
        (
            ('TxtInfo', 1),
            ('TxtHostInfo', 1),
            ('TxtPlayerInfo', 1),
            ('Icons', -1),
            ('PicMarioSave_ds', -1),
            ('PicMarioDelete_ds', -1),
        ),
        True,
        ('PicControlBar', 'PicDimmer', 'Banner', 'NtwkBannerLine', 'NtwkBanner'),
    ),
    'PlayerEntry.szs': (
        'blyt/PlayerEntry.bflyt',
        (('NetworkPasscode', 1), ('ParCountdownTimer', 1)),
        True,
        ('PicBG',),
    ),
    'RCS_SaveData.szs': (
        'blyt/RCS_SaveData.bflyt',
        (),
        True,
        ('PicBG',),
    ),
    'RCS_CameraControlParts.szs': (
        'blyt/RCS_CameraControlParts.bflyt',
        (('PicDark', 1), ('Txt1P', 1), ('Txt2P', 1), ('Txt3P', 1), ('Txt4P', 1)),
        True,
        ('BorderWindow',),
    ),
    'RCS_ControlGuideHeaderParts.szs': (
        'blyt/RCS_ControlGuideHeaderParts.bflyt',
        (),
        True,
        ('PicHeader',),
    ),
    'GuideWindowParts.szs': (
        'blyt/GuideWindowParts.bflyt',
        (),
        True,
        ('Header',),
    ),
    'RCS_ControlGuide.szs': (
        'blyt/RCS_ControlGuide.bflyt',
        (),
        True,
        (),
    ),
    'WipeFadeBlack.szs': ('blyt/WipeFadeBlack.bflyt', (), True, ('PicWipeFade',)),
    'WipeFadeWhite.szs': ('blyt/WipeFadeWhite.bflyt', (), True, ('PicWipeFade',)),
    'WipeMiss.szs': ('blyt/WipeMiss.bflyt', (), True, ('PicBase',)),
    'WipeMissSingleMode.szs': ('blyt/WipeMissSingleMode.bflyt', (), True, ('PicBase',)),
    'WipeCasino.szs': ('blyt/WipeCasino.bflyt', (), True, ('WinCasino',)),
    'RCS_NetworkErrorFade.szs': ('blyt/RCS_NetworkErrorFade.bflyt', (), True, ('PicErrorFade',)),
    'PlayerEntryMini.szs': ('blyt/PlayerEntryMini.bflyt', (('Random', 1),), False, ()),
    'WipeCurtainStart.szs': (
        'blyt/WipeCurtainStart.bflyt',
        (
            ('PicCurtainLShadow', -1),
            ('PicCurtainRShadow', 1),
            ('PicKinopioCurtainLineShL', -1),
            ('PicKinopioCurtainLineShR', 1),
            ('PicKinopioCurtainLineL', -1),
            ('PicKinopioCurtainLineR', 1),
        ),
        True,
        ('PicCurtain', 'PicKinopioCurtain', 'PicBaseShadow', 'PicBase'),
    ),
    'WipeCurtainOpening.szs': (
        'blyt/WipeCurtainOpening.bflyt',
        (
            ('PicCurtainLShadow', -1),
            ('PicCurtainRShadow', 1),
            ('PicKinopioCurtainLineShL', -1),
            ('PicKinopioCurtainLineShR', 1),
            ('PicKinopioCurtainLineL', -1),
            ('PicKinopioCurtainLineR', 1),
        ),
        True,
        ('PicCurtain', 'PicKinopioCurtain', 'PicBaseShadow', 'PicBase'),
    ),
    'WipeCurtainDemo.szs': (
        'blyt/WipeCurtainDemo.bflyt',
        (
            ('PicCurtainLShadow', -1),
            ('PicCurtainRShadow', 1),
            ('PicKinopioCurtainLineShL', -1),
            ('PicKinopioCurtainLineShR', 1),
            ('PicKinopioCurtainLineL', -1),
            ('PicKinopioCurtainLineR', 1),
        ),
        True,
        ('PicCurtain', 'PicKinopioCurtain', 'PicBaseShadow', 'PicBase'),
    ),
    'WipeCurtainContinue.szs': (
        'blyt/WipeCurtainContinue.bflyt',
        (
            ('PicCurtainLShadow', -1),
            ('PicCurtainRShadow', 1),
            ('PicKinopioCurtainLineShL', -1),
            ('PicKinopioCurtainLineShR', 1),
            ('PicKinopioCurtainLineL', -1),
            ('PicKinopioCurtainLineR', 1),
        ),
        True,
        ('PicCurtain', 'PicKinopioCurtain', 'PicBaseShadow', 'PicBase'),
    ),
    'WipeCurtainResult.szs': (
        'blyt/WipeCurtainResult.bflyt',
        (
            ('PicCurtainLShadow', -1),
            ('PicCurtainRShadow', 1),
            ('PicKinopioCurtainLineShL', -1),
            ('PicKinopioCurtainLineShR', 1),
            ('PicKinopioCurtainLineL', -1),
            ('PicKinopioCurtainLineR', 1),
        ),
        True,
        ('PicCurtain', 'PicKinopioCurtain', 'PicBaseShadow', 'PicBase'),
    ),
}


def generate(romfs: str, out_dir: str, aspect: float):
    factor = aspect / (16.0 / 9.0)
    shift = 640.0 * (factor - 1.0)
    src_dir = os.path.join(romfs, 'LayoutData')
    dst_dir = os.path.join(out_dir, 'romfs', 'LayoutData')
    os.makedirs(dst_dir, exist_ok=True)
    count = 0
    for fn, (blyt_name, wraps, do_widen, explicit_widen) in RULES.items():
        src_path = os.path.join(src_dir, fn)
        if not os.path.isfile(src_path):
            continue
        outer_sarc = unyaz0(open(src_path, 'rb').read())
        lyarc = sarc_get(outer_sarc, 'layout.lyarc')
        if not lyarc:
            continue
        blyt = sarc_get(lyarc, blyt_name)
        if not blyt:
            continue
        patched = blyt
        if wraps:
            patched = wrap_panes(patched, tuple((nm, sign * shift) for nm, sign in wraps))
        if do_widen:
            patched = widen_full_panes(patched, factor, explicit_widen)
        new_lyarc = sarc_replace(lyarc, {blyt_name: patched})
        new_outer = sarc_replace(outer_sarc, {'layout.lyarc': new_lyarc})
        with open(os.path.join(dst_dir, fn), 'wb') as f:
            f.write(yaz0_store(new_outer))
        count += 1
    print(f'Generated {count} ultrawide layout archives in {dst_dir} (factor={factor:.4f}, shift={shift:.1f})')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', required=True)
    ap.add_argument('--mods', required=True)
    args = ap.parse_args()
    generate(args.romfs, os.path.join(args.mods, '00-uw-map-hud-16-10'), 16.0 / 10.0)
    generate(args.romfs, os.path.join(args.mods, '00-uw-map-hud-21-9'), 2560.0 / 1080.0)
    generate(args.romfs, os.path.join(args.mods, '00-uw-map-hud-32-9'), 32.0 / 9.0)


if __name__ == '__main__':
    main()
