#!/usr/bin/env python3
"""HDR replacement of Wonder's composite shader, for the runtime's HDR output (hdr=1 in pc-settings.ini). See docs/HDR.md.

Wonder's last tone-mapping program (fragment shader 352e8b76df96cb1d: the scene, the bloom and the depth fog in, the
picture out) turns linear scene light x into linear display light with a two-piece rational curve per channel, its
constants from the c3 uniform buffer (rows 4, 5 and 6):

    y = (a*x + b) / (c*x + d)      x < 0.3: 0.356x / (0.3 - 0.2x)        x >= 0.3: (0.889x - 0.133) / (0.75x + 0.075)

That curve reaches 1.0 (SDR white) at x1 = (d - b) / (a - c), about 1.5, and the RGBA8 window texture clips everything
brighter. The copy this script makes keeps the game's result, unchanged, wherever it is at most 1.0. Above that it writes

    1 + SCALE * (min(x / x1, XMAX) - 1)

a value that tells how far past the white point the scene was (x / x1 = 2: twice as bright as SDR white). Nothing at
or below 1.0 changes, so the UI and the 2D art drawn after or through this pass look as they always did. The window
textures are RGBA16F in HDR mode, so these values survive the game's later passes (a blur, a copy, the UI), and the
runtime's HDR pass (runtime/shaders/hdr_present.frag) turns them into light above paper white with the player's
Highlights / Highlight start settings (live, in the F1 menu) and rolls them off to the display's peak.

    python tools/renodx_wonder.py --data <data root> [--scale 0.25] [--xmax 48]

writes <data root>/spirv-renodx/352e8b76df96cb1d.spv (the runtime loads it instead of spirv/352e8b76df96cb1d.spv when HDR
output is on). Needs spirv-cross and glslc: the Vulkan SDK (VULKAN_SDK, or on PATH). Run it again after the game's
shaders are rebuilt. SCALE must match kEncodeScale in runtime/src/nvn/nvn_hdr.cpp.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

SHADER = "352e8b76df96cb1d"


def tool_dirs():
    """Where spirv-cross and glslc are looked for: a release's game-build/shadertools/ (tools/pack_release.py
    --shader-tools), NX_SHADER_TOOLS, the Vulkan SDK, then PATH (None)."""
    here = os.path.dirname(os.path.abspath(__file__))
    dirs = [os.path.join(here, os.pardir, "shadertools"), os.environ.get("NX_SHADER_TOOLS")]
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        dirs += [os.path.join(sdk, "Bin"), os.path.join(sdk, "bin")]
    return [d for d in dirs if d and os.path.isdir(d)] + [None]


def find_tool(name):
    for d in tool_dirs():
        p = shutil.which(name, path=d) if d else shutil.which(name)
        if p:
            return p
    return None


def tool(name):
    p = find_tool(name)
    if not p:
        sys.exit(f"{name} not found: install the Vulkan SDK (or set VULKAN_SDK / NX_SHADER_TOOLS)")
    return p


EXTEND = """
// ---- HDR (tools/renodx_wonder.py, docs/HDR.md) ----
const float HDR_SCALE = %(scale)s;  // encoded units per multiple of the white point (runtime: kEncodeScale)
const float HDR_XMAX = %(xmax)s;    // brightest scene value kept, in multiples of the white point
// y: the game's result (a*x + b) / (c*x + d). At most 1.0 it is returned as it is; above, how far x is past x1, the
// scene value where the curve reaches 1.0.
// The composite's result (y) is LINEAR display light: the game's final copy goes through an sRGB view, which
// encodes it on write. In this mode the window texture is a float image nothing encodes into, so it stays linear and
// the HDR pass reads it as it is.
float hdr_extend(float y, float x, float a, float b, float c, float d)
{
    if (!(y > 1.0))
    {
        return y;  // linear already: measured against the SDR picture, a decode here squared it (HDR = SDR^1.9)
    }
    float slope = a - c;
    if (!(slope > 1e-6))
    {
        return 1.0;  // a curve that does not cross 1.0 from below: SDR white, as the RGBA8 texture would store it
    }
    float x1 = (d - b) / slope;
    if (!(x1 > 0.0))
    {
        return 1.0;
    }
    return 1.0 + HDR_SCALE * (min(x / x1, HDR_XMAX) - 1.0);
}
"""

# Scene grading, after RenoDX's colorgrade.hlsl (UserColorGrading): exposure, contrast and shadows on luminance with
# the colour kept in proportion, saturation and highlight saturation as an OKLab chroma scale. It runs on the scene
# value x before the game's curve, so the curve and the highlight extension both follow it. The parameters are words
# of the support buffer's two transform-feedback slots (runtime: hdr_grading in nvn_hdr.cpp); all zero (neutral
# settings) returns x untouched, bit for bit.
GRADE = """
// ---- scene grading (tools/renodx_wonder.py, after RenoDX colorgrade.hlsl) ----
vec3 hdr_oklab(vec3 c)
{
    vec3 lms = vec3(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b,
                    0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b,
                    0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    lms = pow(max(lms, vec3(0.0)), vec3(1.0 / 3.0));
    return vec3(0.2104542553 * lms.x + 0.7936177850 * lms.y - 0.0040720468 * lms.z,
                1.9779984951 * lms.x - 2.4285922050 * lms.y + 0.4505937099 * lms.z,
                0.0259040371 * lms.x + 0.7827717662 * lms.y - 0.8086757660 * lms.z);
}
vec3 hdr_unoklab(vec3 l)
{
    vec3 lms = vec3(l.x + 0.3963377774 * l.y + 0.2158037573 * l.z,
                    l.x - 0.1055613458 * l.y - 0.0638541728 * l.z,
                    l.x - 0.0894841775 * l.y - 1.2914855480 * l.z);
    lms = lms * lms * lms;
    return vec3(4.0767416621 * lms.x - 3.3077115913 * lms.y + 0.2309699292 * lms.z,
                -1.2684380046 * lms.x + 2.6097574011 * lms.y - 0.3413193965 * lms.z,
                -0.0041960863 * lms.x - 0.7034186147 * lms.y + 1.7076147010 * lms.z);
}
vec3 hdr_grade(vec3 x)
{
    float on = intBitsToFloat(support_buffer._m6.x);
    if (!(on > 0.5))
    {
        return x;
    }
    float exposure = intBitsToFloat(support_buffer._m6.y);
    float contrast = intBitsToFloat(support_buffer._m6.z);
    float shadows_exp = intBitsToFloat(support_buffer._m6.w);  // 2 - Shadows: below 1 lifts, above 1 lowers
    float saturation = intBitsToFloat(support_buffer._m7.x);
    float highlight_saturation = intBitsToFloat(support_buffer._m7.y);
    const float MID = 0.18;
    x = max(x, vec3(0.0)) * exposure;
    float y = dot(x, vec3(0.2126, 0.7152, 0.0722));
    if (y > 1e-6)
    {
        float yc = MID * pow(y / MID, contrast);
        // Shadows: RenoDX's curve below mid grey, faded out above it so nothing else moves.
        float ys = mix(yc, MID * pow(yc / MID, shadows_exp), 1.0 - smoothstep(0.0, 0.5, yc));
        x *= ys / y;
    }
    if (saturation != 1.0 || highlight_saturation != 1.0)
    {
        float yn = dot(x, vec3(0.2126, 0.7152, 0.0722));
        vec3 lab = hdr_oklab(x);
        // Highlight saturation: the chroma scale moves toward its own value as the scene gets brighter.
        lab.yz *= saturation * mix(1.0, highlight_saturation, smoothstep(0.35, 1.5, yn));
        x = max(hdr_unoklab(lab), vec3(0.0));
    }
    return x;
}
"""

DECL = r"^(?P<ind>\s*)float {x} = (?P<e>.+);$"

STORE = re.compile(
    r"^(?P<ind>\s*)_34\.(?P<c>[xyz]) = fma\((?P<x>_\d+), (?P<a>_\d+), (?P<b>_\d+)\) \* "
    r"\(1\.0 / fma\((?P=x), (?P<cc>_\d+), (?P<d>_\d+)\)\);$", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, help="the game's data root (holds spirv/)")
    ap.add_argument("--scale", default="0.25")
    ap.add_argument("--xmax", default="48.0")
    ap.add_argument("--keep-glsl", help="also write the patched GLSL here (to read it)")
    a = ap.parse_args()
    src = os.path.join(a.data, "spirv", SHADER + ".spv")
    if not os.path.isfile(src):
        sys.exit(f"{src} not found")
    glsl = subprocess.run([tool("spirv-cross"), "--vulkan-semantics", src], check=True, capture_output=True,
                          text=True).stdout
    stores = list(STORE.finditer(glsl))
    if sorted(m.group("c") for m in stores) != ["x", "y", "z"]:
        sys.exit("the program does not have the expected output stores: has the game's shader changed?")
    for m in reversed(stores):  # from the end, so the earlier offsets stay valid
        g = m.groupdict()
        y = f"fma({g['x']}, {g['a']}, {g['b']}) * (1.0 / fma({g['x']}, {g['cc']}, {g['d']}))"
        new = f"{g['ind']}_34.{g['c']} = hdr_extend({y}, {g['x']}, {g['a']}, {g['b']}, {g['cc']}, {g['d']});"
        glsl = glsl[:m.start()] + new + glsl[m.end():]
    # Grading: the support buffer's last member is an int in the game's declaration; the grading reads a vec4 there.
    tail = "    int _m7;\n} support_buffer_1;"
    if tail not in glsl:
        sys.exit("the support buffer is not declared as expected: has the game's shader changed?")
    glsl = glsl.replace(tail, "    ivec4 _m7;\n} support_buffer_1;", 1)
    # The runtime finds this buffer by the VARIABLE's name (nvn_spirv_reflect.cpp: "support_buffer"). spirv-cross calls the
    # instance support_buffer_1 (the block type takes the plain name), and glslc drops names unless -g: rename the block
    # type and give the instance the name the game's shader had. Without this the runtime does not bind the buffer.
    glsl = glsl.replace("uniform support_buffer\n", "uniform support_block\n", 1).replace("support_buffer_1", "support_buffer")
    # The three scene values (the x of each store) are declared once; they are graded as one vec3 after the last one.
    names = {m.group("c"): m.group("x") for m in stores}
    decls = []
    for c in "xyz":
        d = re.search(DECL.format(x=names[c]), glsl, re.M)
        if not d:
            sys.exit(f"the declaration of {names[c]} was not found: has the game's shader changed?")
        decls.append(d)
    for d in sorted(decls, key=lambda d: d.start(), reverse=True):
        last = d is max(decls, key=lambda d: d.start())
        name = names["xyz"[decls.index(d)]]
        new = f"{d.group('ind')}float {name}_raw = {d.group('e')};"
        if last:
            raw = [f"{names[c]}_raw" for c in "xyz"]
            new += (f"\n{d.group('ind')}vec3 hdr_graded = hdr_grade(vec3({raw[0]}, {raw[1]}, {raw[2]}));"
                    + "".join(f"\n{d.group('ind')}float {names[c]} = hdr_graded.{c};" for c in "xyz"))
        glsl = glsl[:d.start()] + new + glsl[d.end():]
    glsl = glsl.replace("void main()", GRADE + EXTEND % dict(scale=a.scale, xmax=a.xmax) + "\nvoid main()", 1)
    if a.keep_glsl:
        with open(a.keep_glsl, "w") as f:
            f.write(glsl)
    out_dir = os.path.join(a.data, "spirv-renodx")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, SHADER + ".spv")
    with tempfile.TemporaryDirectory() as tmp:
        fp = os.path.join(tmp, SHADER + ".frag")
        with open(fp, "w") as f:
            f.write(glsl)
        # A relative source name from the temporary folder: -g records it, so the output is the same on every run.
        subprocess.run([tool("glslc"), "-fshader-stage=frag", "--target-env=vulkan1.0", "-O", "-g", SHADER + ".frag",
                        "-o", os.path.abspath(out)], check=True, cwd=tmp)
    print(f"wrote {out} (scale {a.scale}, xmax {a.xmax})")


if __name__ == "__main__":
    main()
