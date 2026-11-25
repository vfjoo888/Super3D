#!/usr/bin/env python3
"""Check translated overlay and launcher characters against atlas ranges and bundled fonts."""
import re
from pathlib import Path

from fontTools.ttLib import TTFont

root = Path(__file__).resolve().parents[1]
overlay = root / "runtime/src/overlay"
launcher = root / "runtime/src/launcher"
sources = list(overlay.rglob("*.cpp")) + list(launcher.rglob("*.cpp"))


def translated(text):
    return {ord(c) for c in text if any(lo <= ord(c) <= hi for lo, hi in
            ((0x100, 0x52F), (0x600, 0x6FF), (0x3040, 0x30FF), (0x4E00, 0x9FFF), (0xAC00, 0xD7A3)))}


required = set().union(*(translated(path.read_text(encoding="utf-8")) for path in sources))
ui = (overlay / "overlay_ui.cpp").read_text(encoding="utf-8")
ranges = set()
for name in ("kLocalizedGlyphs", "kAdditionalLocalizedGlyphs", "kLauncherLocalizedGlyphs", "kKoreanHelpGlyphs", "kAdditionalHelpGlyphs"):
    match = re.search(r'const char ' + name + r'\[\] = "([^"]*)";', ui)
    if not match:
        raise SystemExit(f"Missing glyph list {name}")
    ranges |= translated(match.group(1))
fonts = (root / "third_party/fonts/NotoFallback").glob("*.ttf")
coverage = set().union(*(set(TTFont(path).getBestCmap()) for path in fonts))
for name, present in (("atlas", ranges), ("bundled fonts", coverage)):
    missing = required - present
    if missing:
        raise SystemExit(f"Missing {name} glyphs: {''.join(chr(c) for c in sorted(missing))}")
print(f"Overlay coverage: {len(required)} characters in atlas and bundled fonts")
