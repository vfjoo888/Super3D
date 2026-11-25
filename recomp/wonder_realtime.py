"""Wonder 1.0.0 and 1.2.1 frame-step patch, based on verified AArch64 instructions.

Wonder normally turns the NVN present interval (1 or 2) into its simulation step, so
the game runs at the wrong speed if it is rendered at a rate other than 60/30 FPS.
Replace those calculations with loads from a runtime-owned page. The runtime updates
that page from the measured elapsed time between presents.
"""
from dataclasses import dataclass
import struct


ADRP8, LOAD_RATIO, LOAD_SECONDS, NOP = -1, -2, -3, -4

# Backwards-compatible exports for the 1.0.0 layout.
BRANCHES = (0xC9528, 0xC9588)
SITE_WORDS = 8
ORIGINAL = (
    0xBD4262C0, 0x52A84E08, 0xAA1F03E9, 0x1E270101,
    0x91022288, 0x52AFF80A, 0x5E21D800, 0x1E211801,
)
REPLACEMENT = (ADRP8, LOAD_RATIO, LOAD_SECONDS, 2, 4, 5, NOP, NOP)

# The perspective config builder copies the aspect pointer's float
# into the projection config. Read a runtime-owned aspect instead.
# This is the same kind of camera-input patch used by TOTK Recomp, not a shader edit.
ASPECT_SITE = 0x160DEA4
ASPECT_ORIGINAL = (0xF9402009, 0xAA0803E0, 0xBD400120, 0xBD005900)
ASPECT_LOAD = 0xBD400920  # ldr s0, [x9, #8]


@dataclass(frozen=True)
class VersionSpec:
    version: str
    branches: tuple[int, ...]
    original: tuple[int, ...]
    replacement: tuple[int, ...]
    aspect_site: int
    aspect_original: tuple[int, ...]


VERSIONS = (
    VersionSpec(
        version="1.0.0",
        branches=BRANCHES,
        original=ORIGINAL,
        replacement=REPLACEMENT,
        aspect_site=ASPECT_SITE,
        aspect_original=ASPECT_ORIGINAL,
    ),
    # 1.2.1 inserts `adrp x11, ...` at index 2 (used by `ldr s0, [x11, #0xe28]` right after).
    # Keep index 2 at its original PC so `adrp x11` resolves the same page, and load both
    # runtime floats into s0/s1 before index 5 overwrites x8 (`add x8, x20, #0x88`).
    VersionSpec(
        version="1.2.1",
        branches=(0xC5A28, 0xC5A8C),
        original=(
            0xBD426300, 0x52A84E08, 0xB001514B, 0x1E270101, 0xAA1F03E9,
            0x91022288, 0x52AFF80A, 0x5E21D800, 0x1E211801,
        ),
        replacement=(ADRP8, LOAD_RATIO, 2, LOAD_SECONDS, 4, 5, 6, NOP, NOP),
        aspect_site=0x173339C,
        aspect_original=ASPECT_ORIGINAL,
    ),
)


def _adrp(rd, pc, target):
    pages = (target >> 12) - (pc >> 12)
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError("Wonder real-time frame-step page is outside ADRP range")
    imm = pages & 0x1FFFFF
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd


def _matches_spec(module, spec):
    t_start, t_size = module.text
    t_end = t_start + t_size
    site_bytes = len(spec.original) * 4
    for branch in spec.branches:
        if branch < t_start or branch + site_bytes > t_end:
            return False
        actual = struct.unpack_from("<" + "I" * len(spec.original), module.image, branch)
        if actual != spec.original:
            return False
    aspect_bytes = len(spec.aspect_original) * 4
    if spec.aspect_site < t_start or spec.aspect_site + aspect_bytes > t_end:
        return False
    actual = struct.unpack_from("<" + "I" * len(spec.aspect_original), module.image, spec.aspect_site)
    return actual == spec.aspect_original


def patch_wonder_realtime(module):
    """Patch main's two present-interval conversions and reserve one guest data page."""
    if module.name != "main":
        return

    bss_start, bss_size = module.bss
    page = (bss_start + bss_size + 0xFFF) & ~0xFFF
    if page < module.text[0] + module.text[1] and page + 0x1000 > module.text[0]:
        raise ValueError("Wonder real-time frame-step page overlaps main text")

    spec = next((s for s in VERSIONS if _matches_spec(module, s)), None)
    if spec is None:
        raise ValueError(
            "Wonder real-time frame-step patch supports game versions 1.0.0 and 1.2.1; "
            "neither version's expected instructions matched main"
        )

    for branch in spec.branches:
        words = []
        for i, item in enumerate(spec.replacement):
            if item == ADRP8:
                word = _adrp(8, branch + i * 4, page)
            elif item == LOAD_RATIO:
                word = 0xBD400100  # ldr s0, [x8]
            elif item == LOAD_SECONDS:
                word = 0xBD400501  # ldr s1, [x8, #4]
            elif item == NOP:
                word = 0xD503201F
            else:
                word = spec.original[item]
            words.append(word)
        struct.pack_into("<" + "I" * len(words), module.image, branch, *words)

    aspect_words = (
        _adrp(9, spec.aspect_site, page),
        spec.aspect_original[1],  # preserve x0 = projection config
        ASPECT_LOAD,
        spec.aspect_original[3],
    )
    struct.pack_into("<" + "I" * len(aspect_words), module.image, spec.aspect_site, *aspect_words)

    # load_modules() commits through the end of BSS. Put the page at the end so the
    # runtime can find it through the optional ModuleInfo offset.
    module.bss = (bss_start, page + 0x1000 - bss_start)
    module.frame_step_offset = page
    print(f"wonder_realtime: patched Wonder {spec.version} frame-step and perspective aspect")
