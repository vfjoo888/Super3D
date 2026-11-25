#!/usr/bin/env python3
"""Generate both widescreen layout profiles from the player's own game files."""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import tempfile
import uuid

import wonder_uw_map_hud
import wonder_uw_screen_backgrounds

PROFILES = (("21-9", "2.37037037037", "00-uw-map-hud-21-9", "00-uw-screen-test-21-9"),
            ("32-9", "3.55555555556", "00-uw-map-hud-32-9", "00-uw-screen-test"))
HERE = os.path.dirname(os.path.abspath(__file__))


def publish_file(source, target):
    # Create the replacement in its final directory so it inherits that
    # directory's permissions (TemporaryDirectory is private on Windows).
    os.makedirs(os.path.dirname(target), exist_ok=True)
    temporary = target + '.new-' + uuid.uuid4().hex
    try:
        with open(source, 'rb') as src, open(temporary, 'xb') as dst:
            shutil.copyfileobj(src, dst)
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def ensure(data):
    rom = os.path.join(data, 'extract', 'romfs')
    mods = os.path.join(data, 'mods')
    menu = os.path.join(mods, 'recomp-menu', 'romfs')
    tables = glob.glob(os.path.join(glob.escape(menu), 'System', 'Resource', 'ResourceSizeTable.Product.*.rsizetable.zs'))
    if not tables:
        raise RuntimeError('The recomp-menu resource table is missing. Run game setup first.')

    def rstb_ver(path):
        m = re.search(r'Product\.(\d+)\.rsizetable\.zs$', os.path.basename(path))
        return int(m.group(1)) if m else -1

    table = max(tables, key=rstb_ver)
    required = [os.path.join(HERE, x) for x in ('wonder_menu_mod.py', 'wonder_uw_map_hud.py',
                'wonder_uw_screen_backgrounds.py', 'wonder_uw_prepare.py')]
    required += [table, os.path.join(menu, 'Layout', 'Title.Nin_NX_NVN.blarc.zs')]
    names = set(wonder_uw_screen_backgrounds.PANES) | set(wonder_uw_map_hud.HUD_WRAPS)
    required += [os.path.join(rom, 'Layout', n + '.Nin_NX_NVN.blarc.zs') for n in sorted(names) if n != 'Title']
    digest = hashlib.sha256()
    for path in required:
        digest.update(os.path.basename(path).encode('utf-8') + b'\0')
        with open(path, 'rb') as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(chunk)
    signature = digest.hexdigest()
    state = os.path.join(data, 'ultrawide-layouts.json')
    destinations = []
    for _, aspect, map_name, screen_name in PROFILES:
        for name in (map_name, screen_name):
            active, disabled = os.path.join(mods, name), os.path.join(mods, '-' + name)
            if os.path.exists(active) and os.path.exists(disabled):
                raise RuntimeError('Both enabled and disabled copies exist for ' + name)
            destinations.append(disabled if os.path.isdir(disabled) else active)
    try:
        with open(state, encoding='utf8') as stream:
            record = json.load(stream)
        if record.get('signature') == signature and all(os.path.isfile(os.path.join(destinations[index], rel))
                                                        for index, rel in record['files']):
            print('Ultrawide layouts are up to date.')
            return
    except (OSError, ValueError, KeyError, TypeError):
        pass
    print('Preparing ultrawide layouts from your game files...', flush=True)
    os.makedirs(mods, exist_ok=True)
    files = []
    with tempfile.TemporaryDirectory(prefix='uw-layouts-', dir=data) as work:
        for index, (_, aspect, map_name, screen_name) in enumerate(PROFILES):
            map_out, screen_out = os.path.join(work, map_name), os.path.join(work, screen_name)
            wonder_uw_map_hud.main(['--romfs', rom, '--rstb', table, '--out', map_out, '--aspect', aspect])
            wonder_uw_screen_backgrounds.main([
                '--romfs', rom, '--menu-mod', menu, '--out', screen_out, '--aspect', aspect,
                '--skip', *sorted(wonder_uw_map_hud.HUD_WRAPS),
            ])
            for subindex, source in enumerate((map_out, screen_out)):
                dest_index = index * 2 + subindex
                for base, dirs, names in os.walk(source):
                    dirs.sort()
                    for name in sorted(names):
                        original = os.path.join(base, name)
                        rel = os.path.relpath(original, source)
                        target = os.path.join(destinations[dest_index], rel)
                        publish_file(original, target)
                        files.append([dest_index, rel])
        temporary = os.path.join(work, 'state.json')
        with open(temporary, 'w', encoding='utf8') as stream:
            json.dump({'signature': signature, 'files': files}, stream)
        publish_file(temporary, state)
    print('Ultrawide layouts are ready.', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--data-root', required=True)
    args = parser.parse_args()
    ensure(os.path.abspath(args.data_root))

if __name__ == '__main__':
    main()
