"""Generate experimental widescreen background layouts from the player's data."""
import argparse
import os
import struct
from wonder_menu_mod import unzs, zs, sarc_get, sarc_nodes, flyt_sections, pane_name

EXPECTED_WIDTHS = (16.0, 17.2, 1920.0, 1984.0, 2000.0, 2100.0)

PANES = {
    'AccountSelect': ('P_Header', 'P_Footer'),
    'BadgeChallengeIntro': ('P_BG',),
    'BadgeSelect01': (
        'P_PtnBG', 'P_Header', 'P_ColorLineH01', 'P_ColorLineH02', 'P_ColorLineH03',
        'P_Footer', 'P_ShuffleHeader', 'P_ShuffleFooter',
    ),
    'BadgeShop': ('W_ListBG', 'P_HeaderEdge', 'P_Header', 'P_Footer'),
    'Boot': ('P_BG',),
    'CharacterSelect': (
        'P_BG', 'P_CapEasyBG', 'P_Header', 'P_ColorLineH01', 'P_ColorLineH02', 'P_ColorLineH03', 'P_Footer',
    ),
    'CmnBlurBG': ('P_CapUse01', 'P_CapUse'),
    'CmnErrorBlurBG': ('P_CapUse',),
    'CmnLobbyBG': ('P_BG', 'P_CapUse'),
    'CmnOnUIBlurBg': ('P_CapUse', 'P_Dot'),
    'ControlGuide': ('P_BG', 'P_Header', 'P_Footer'),
    'CourseIn': ('P_BG', 'P_Ground', 'P_PtnJagStatic', 'P_PtnJag'),
    'CourseInWindow': ('P_CapUse',),
    'CourseList': (
        'P_PtnBGCourseList', 'P_Header', 'P_ColorLineH01', 'P_ColorLineH02', 'P_ColorLineH03', 'P_Footer',
    ),
    'CourseMiddleDemo': ('P_Header', 'P_HeaderFlash', 'P_Footer', 'P_FooterFlash'),
    'CourseOut': ('P_BG_00',),
    'FadeBlack': ('P_Fade',),
    'FadeBlackDemo': ('P_Fade',),
    'FadeWhite': ('P_Fade',),
    'FadeWhiteDemo': ('P_Fade',),
    'GameOver': ('P_BG', 'P_Ground', 'P_PtnJagStatic'),
    'GetMotherSeed': (
        'P_Header', 'P_ColorLineH01', 'P_ColorLineH02', 'P_ColorLineH03', 'P_ColorLineH04',
        'P_Footer', 'P_ColorLineF01', 'P_ColorLineF02', 'P_ColorLineF03', 'P_ColorLineF04',
        'P_ColorBarH00', 'P_ColorBarH01', 'P_ColorBarH02', 'P_ColorBarH03', 'P_ColorBarH04',
        'P_ColorBarF00', 'P_ColorBarF01', 'P_ColorBarF02', 'P_ColorBarF03', 'P_ColorBarF04',
    ),
    'GrandWorldMap': ('P_GrandWorldMap',),
    'KakashiCollection': (
        'P_TCap', 'P_Header', 'P_ColorLineH01', 'P_ColorLineH02', 'P_ColorLineH03', 'P_Footer',
    ),
    'KakashiGet': ('P_CapUse', 'P_Dot'),
    'MainMenu': (
        'P_Header', 'P_Header_00', 'P_ColorLineH01', 'P_ColorLineH02', 'P_ColorLineH03',
        'P_Footer', 'P_Footer_00', 'P_CapUse', 'P_Dot',
    ),
    'MemberList': (
        'P_PtnBGCourseList', 'P_Header', 'P_ColorLineH01', 'P_ColorLineH02', 'P_ColorLineH03', 'P_Footer',
    ),
    'MissZanki': ('P_BG',),
    'Option': ('P_BG', 'P_Header', 'P_Footer'),
    'PlayerNumSelect': ('P_Header', 'P_Footer'),
    'QuizStart': ('P_BG',),
    'RaceFinish': ('P_BG', 'P_Header', 'P_Footer'),
    'RaceFrame': ('P_Header', 'P_Footer'),
    'RescueFrame': ('W_Frame',),
    'Result': (
        'P_Header', 'P_ColorLineH01', 'P_ColorLineH02', 'P_ColorLineH03', 'P_ColorLineH04',
        'P_Footer', 'P_ColorLineF01', 'P_ColorLineF02', 'P_ColorLineF03', 'P_ColorLineF04',
        'P_ColorBarH00', 'P_ColorBarH01', 'P_ColorBarH02', 'P_ColorBarH03', 'P_ColorBarH04',
        'P_ColorBarF00', 'P_ColorBarF01', 'P_ColorBarF02', 'P_ColorBarF03', 'P_ColorBarF04',
    ),
    'TalkFlowerCutIn': ('W_Glow',),
    'Title': ('P_Ptn', 'P_Gradation', 'P_PtnComplete', 'P_BG'),
    'TitleBlurBG': ('P_CapUse', 'P_Mask'),
    'Wipe': ('W_Wipe',),
    'WipeBadgeChallenge': ('W_Wipe',),
    'WipeCourseIn': ('P_CapUse', 'W_Wipe'),
    'WipeMiss': ('W_Wipe',),
    'WipeToDown': ('P_Capture', 'P_WipeToDown'),
    'WipeToLeft': ('P_Capture', 'P_WipeToLeft'),
    'WipeToRight': ('P_Capture', 'P_WipeToLeft'),
    'WipeToUp': ('P_Capture', 'P_WipeToUp'),
    'WipeWarp': ('P_Capture', 'P_WipeToUp'),
    'WipeWonder': ('W_Wipe_00', 'W_Wipe_01', 'W_Wipe_02'),
    'WorldName': ('P_BG',),
}


def patch_layout(name, layout_bytes, factor):
    """Widen configured background/bar panes in-place inside a bflyt buffer."""
    panes = PANES.get(name, ())
    if not panes and name not in ('Boot', 'Title'):
        return layout_bytes
    layout = bytearray(layout_bytes)
    found = set()
    shift = 960.0 * (factor - 1.0)
    for off, tag, _ in flyt_sections(layout):
        if tag not in (b'pic1', b'wnd1'):
            continue
        pane = pane_name(layout, off)
        if pane in panes:
            width = struct.unpack_from('<f', layout, off + 0x4c)[0]
            if not any(abs(width - exp) <= 0.1 for exp in EXPECTED_WIDTHS):
                raise ValueError(f'{name}/{pane}: unexpected width {width}')
            struct.pack_into('<f', layout, off + 0x4c, width * factor)
            # Left-anchored full-width bars (origin_x == 1, e.g. QuizStart/WorldName P_BG)
            # must also shift left by half the added width so both screen edges stay covered.
            origin_x = layout[off + 9] & 3
            if origin_x == 1:
                x = struct.unpack_from('<f', layout, off + 0x2c)[0]
                struct.pack_into('<f', layout, off + 0x2c, x - shift)
            elif origin_x == 2:
                x = struct.unpack_from('<f', layout, off + 0x2c)[0]
                struct.pack_into('<f', layout, off + 0x2c, x + shift)
            found.add(pane)
    if found != set(panes):
        raise ValueError(f'{name}: missing panes {set(panes) - found}')
    if name == 'Boot':
        for off, tag, _ in flyt_sections(layout):
            if tag == b'prt1' and pane_name(layout, off) == 'L_WonderFlowerMorphing':
                x = struct.unpack_from('<f', layout, off + 0x2c)[0]
                struct.pack_into('<f', layout, off + 0x2c, x + shift)
    if name == 'Title':
        for off, tag, _ in flyt_sections(layout):
            pane = pane_name(layout, off) if tag in (b'pic1', b'txt1') else ''
            if pane == 'P_Logo' or pane in ('T_QuitGame', 'T_QuitGameUS'):
                x = struct.unpack_from('<f', layout, off + 0x2c)[0]
                struct.pack_into('<f', layout, off + 0x2c, x + (-shift if pane == 'P_Logo' else shift))
    return bytes(layout)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', required=True)
    ap.add_argument('--menu-mod', required=True, help='active recomp-menu romfs')
    ap.add_argument('--out', required=True, help='new mod directory')
    ap.add_argument('--aspect', type=float, default=32 / 9)
    ap.add_argument('--skip', nargs='*', default=(), help='layout names handled by map-hud mod')
    args = ap.parse_args(argv)
    factor = args.aspect / (16 / 9)
    if not 1.0 <= factor <= 3.0:
        raise ValueError('unsupported aspect')
    skipped = set(args.skip)
    for name, panes in PANES.items():
        if name in skipped:
            continue
        filename = name + '.Nin_NX_NVN.blarc.zs'
        root = args.menu_mod if name == 'Title' else args.romfs
        archive = unzs(os.path.join(root, 'Layout', filename))
        layout_name = 'blyt/' + name + '.bflyt'
        patched = patch_layout(name, sarc_get(archive, layout_name), factor)
        result = bytearray(archive)
        nodes = [(start, end) for entry, _, start, end in sarc_nodes(archive) if entry == layout_name]
        if len(nodes) != 1 or nodes[0][1] - nodes[0][0] != len(patched):
            raise ValueError(f'{name}: layout size changed')
        start, end = nodes[0]
        result[start:end] = patched
        target = os.path.join(args.out, 'romfs', 'Layout', filename)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'wb') as f:
            f.write(zs(bytes(result)))
        print(name, ', '.join(panes))


if __name__ == '__main__':
    main()
