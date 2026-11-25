import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from wonder_menu_mod import unzs, zs, sarc_get, sarc_replace, flyt_sections, pane_name, rstb_grow
from wonder_uw_screen_backgrounds import PANES, patch_layout

# Edge-anchored HUD and menu panes: sign -1 shifts left (-shift), +1 shifts right (+shift).
HUD_WRAPS = {
    'WorldMapDisplay': (('N_Header01', -1), ('N_Footer01', 1), ('L_Pass', 1)),
    'GameDisplay': (('N_BigRandomCoin00', 1),),
    'Pause': (('N_Left', -1), ('N_Right', 1)),
    'FloorNum': (('N_Pos', 1),),
    'MusicTitle': (('W_BG', 1),),
    'NowSaving': (('N_Pos', 1),),
    'NowConnectingCmn': (('N_Pos', -1),),
    'LoadingIcon': (('L_WonderFlowerMorphing', 1),),
    'Wipe': (('L_WonderFlowerMorphing', 1),),
    'CourseOut': (('N_LoadIcon', 1),),
    'Option': (('N_LoadIcon', 1),),
    'AccountSelect': (('N_DeleteSaveData', 1),),
    'MainMenu': (
        ('T_HeaderTitle', -1),
        ('T_HeaderTitleSub', -1),
        ('L_CharaSelectBtn', -1),
        ('L_KakashiCollectionBtn', 1),
        ('L_PlayerCard', 1),
        ('N_ScreenshotPos', -1),
    ),
    'BadgeSelect01': (('P_ZigZagBG', -1), ('T_HeaderTitle', -1), ('T_HeaderTitleSub', -1)),
    'CharacterSelect': (('P_ZigZagBG', -1), ('T_HeaderTitle', -1), ('T_HeaderTitleSub', -1)),
    'CourseList': (
        ('P_ZigZagBG', -1),
        ('T_HeaderTitle', -1),
        ('T_HeaderTitleSub', -1),
        ('L_GrandWorldMapBtn', 1),
    ),
    'KakashiCollection': (('P_ZigZagBG', -1), ('T_HeaderTitle', -1), ('T_HeaderTitleSub', -1)),
    'CourseIn': (('L_BadgeReccomend', -1),),
    'CourseInCourseName': (('N_Pos', -1),),
    'TalkFlowerCutIn': (('N_Anchor00', 1), ('N_Anchor01', -1), ('N_Anchor02', 1), ('N_Anchor03', -1)),
    'Result': (('N_FlashA', -1), ('N_FlashB', 1)),
    'MemberList': (('P_ZigZagBG', -1), ('T_HeaderTitle', -1), ('T_Title', -1)),
    'MemberListS': (('N_MemberList', -1),),
    'GameDisplayOverlay': (('N_State', -1), ('N_StateHertPoint', -1)),
    'RaceTimer': (('N_All', 1),),
    'StaffCreditsDisplay': (
        ('N_OverlapArea00', -1),
        ('N_Cap', -1),
        ('P_CapUse', -1),
        ('N_OverlapArea01', 1),
        ('N_BestScore', 1),
    ),
}


def wrap(data, targets):
    out = bytearray(data)
    for name, shift in targets:
        sections = list(flyt_sections(out))
        found = [
            (i, o, s)
            for i, (o, t, s) in enumerate(sections)
            if t in (b'pan1', b'pic1', b'prt1', b'wnd1', b'txt1') and pane_name(out, o) == name
        ]
        if len(found) != 1:
            raise ValueError('pane not unique: ' + name)
        i, o, size = found[0]
        # Include user data and the complete original child tree under the new parent.
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
                raise ValueError('unbalanced pane tree')
        end = sections[j][0] if j < len(sections) else len(out)
        root_o, _, root_size = next(
            (a, b, c) for a, b, c in sections if b == b'pan1' and pane_name(out, a) == 'RootPane'
        )
        parent = bytearray(out[root_o : root_o + root_size])
        # Clone the original pane to preserve its format; remove its transform and use the requested translation.
        parent[12:36] = ('UW_' + name).encode().ljust(24, b'\0')[:24]
        struct.pack_into('<fff', parent, 0x2C, shift, 0.0, 0.0)
        struct.pack_into('<fff', parent, 0x38, 0.0, 0.0, 0.0)
        struct.pack_into('<ff', parent, 0x44, 1.0, 1.0)
        original = out[o:end]
        out[o:end] = parent + b'pas1' + struct.pack('<I', 8) + original + b'pae1' + struct.pack('<I', 8)
        struct.pack_into('<I', out, 0xC, len(out))
        struct.pack_into('<H', out, 0x10, struct.unpack_from('<H', out, 0x10)[0] + 3)
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--aspect', type=float, default=32 / 9)
    ap.add_argument('--rstb', required=True)
    args = ap.parse_args(argv)
    if not 16 / 9 <= args.aspect <= 64 / 9:
        raise ValueError('unsupported aspect')
    factor = args.aspect / (16 / 9)
    shift = 960.0 * (factor - 1.0)
    dest = os.path.join(args.out, 'romfs', 'Layout')
    os.makedirs(dest, exist_ok=True)
    rstb_updates = {}
    for layout_name, targets in HUD_WRAPS.items():
        arc_base = f'{layout_name}.Nin_NX_NVN.blarc'
        source = os.path.join(args.romfs, 'Layout', arc_base + '.zs')
        archive = unzs(source)
        entry_name = f'blyt/{layout_name}.bflyt'
        bflyt = sarc_get(archive, entry_name)
        if layout_name in PANES:
            bflyt = patch_layout(layout_name, bflyt, factor)
        modified = wrap(bflyt, [(pane, sign * shift) for pane, sign in targets])
        new_archive = sarc_replace(archive, {entry_name: modified})
        with open(os.path.join(dest, arc_base + '.zs'), 'wb') as f:
            f.write(zs(new_archive))
        rstb_updates[f'Layout/{arc_base}'] = len(new_archive) - len(archive) + 0x2000
    # Start from the active menu mod's table so its own enlarged resources remain allocated.
    table = unzs(args.rstb)
    table = rstb_grow(table, rstb_updates)
    table_dir = os.path.join(args.out, 'romfs', 'System', 'Resource')
    os.makedirs(table_dir, exist_ok=True)
    with open(os.path.join(table_dir, os.path.basename(args.rstb)), 'wb') as f:
        f.write(zs(table))
    print(f'Created ultrawide HUD layouts ({len(HUD_WRAPS)} archives); offset {shift:.2f}')


if __name__ == '__main__':
    main()
