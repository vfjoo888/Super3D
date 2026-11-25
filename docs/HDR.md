# HDR output

The Windows build can send the picture in HDR (scRGB, `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` on an RGBA16F
swapchain), in the spirit of the RenoDX mods: the game's own tone curve stays in charge of everything up to SDR white,
and the highlights it clips to white (the sun, glows, sparkles, bright skies) are recovered and shown brighter than
paper white, up to the display's peak. Midtones, the 2D art and the UI look the same as in SDR.

## Requirements

- Windows HDR turned on for the display the game runs on (Settings > System > Display > Use HDR). Without it the
  surface offers no scRGB format: the game logs `nvn/hdr: the surface offers no scRGB ...` and runs exactly as with HDR
  off (8-bit window textures, sRGB swapchain, the original shaders).
- A GPU and driver that expose `VK_EXT_swapchain_colorspace` (current NVIDIA, AMD and Intel drivers do).
- The composite replacement in `<data root>/spirv-renodx/352e8b76df96cb1d.spv` (see below). Without it HDR output still
  works, but the picture is the SDR picture at paper white with no highlights.

## Settings

In the F1 menu, Video tab, section HDR (also `pc-settings.ini`):

| Row | ini key | Range | Default | Applies |
|---|---|---|---|---|
| HDR output | `hdr` | 0 / 1 | 0 (off) | after a restart |
| Peak brightness | `hdr_peak` | 400-4000 nits (ini: up to 10000) | 1000 | live |
| Paper white | `hdr_paper_white` | 80-500 nits | 203 | live |
| Highlights | `hdr_highlights` | 0-300 % | 100 | live |
| Highlight start | `hdr_highlight_start` | 50-300 % | 100 | live |

- **Peak brightness**: the display's brightest output; the highlights roll off smoothly into it. Use the value the
  Windows HDR Calibration app found (LG C-series OLEDs: about 1000).
- **Paper white**: the brightness of SDR white: the SDR part of the picture, the menus and text. 203 nits is the
  ITU-R BT.2408 reference; raise it in a bright room.
- **Highlights**: how far past white the recovered highlights go. 0 % shows them clipped, exactly as in SDR.
- **Highlight start**: where the highlight curve begins, in percent of the scene brightness the game's curve maps to
  white (scene value about 1.5). At 100 % the highlights grow smoothly out of white with zero slope. Lower values make
  them rise faster from white; higher values keep more of them plain white, as in SDR. Whatever the setting, nothing at
  or below SDR white ever changes: the start cannot move into the picture the game already shows.

Environment overrides (tests): `NX_HDR=0|1`, `NX_HDR_PEAK`, `NX_HDR_PAPER_WHITE`, `NX_HDR_HIGHLIGHTS`,
`NX_HDR_HIGHLIGHT_START`.

The defaults are chosen to leave the game's look alone: everything the game shows below white (all of the main menu's
2D art, the HUD, the midtones) is bit-for-bit the SDR picture at 203 nits, and the first highlights past white rise
gently (twice the white point's scene brightness reaches about 520 nits, three times about 900, then they settle at
the peak).

## How it works

1. **Swapchain** (`runtime/src/nvn/nvn_vk.cpp`, `make_surface`): with `hdr=1` the instance enables
   `VK_EXT_swapchain_colorspace` and the surface's RGBA16F + `EXTENDED_SRGB_LINEAR` format is taken when it is offered.
   Not with the present pass (`NX_PRESENT_PASS=1`, a mobile path). If the HDR pass cannot be made, the swapchain is
   made again in SDR.
2. **Window textures** (`nvn_queue.cpp`, `nvn_image.cpp`): the textures the game presents (format 0x25, RGBA8) are
   made RGBA16F, so values above 1.0 survive the passes after the composite (a blur, a copy, the UI). They hold
   LINEAR light, UI and scene alike: the game draws into them through sRGB views, where the GPU encodes on write; a
   float image has no such view, so nothing encodes and what the shaders write stays linear. (Measured against the SDR
   picture: a version that decoded these values as gamma-encoded came out as SDR^1.9, far too saturated.)
3. **Composite replacement** (`tools/renodx_wonder.py`): Wonder's tone-mapping program, fragment shader
   `352e8b76df96cb1d`, maps linear scene light `x` per channel with a two-piece rational curve
   `y = (a*x + b) / (c*x + d)` (constants from the c3 uniform buffer, rows 4-6: below 0.3 `0.356x / (0.3 - 0.2x)`,
   above `(0.889x - 0.133) / (0.75x + 0.075)`). `y` is linear display light and reaches 1.0 at `x1 = (d - b) / (a - c)`,
   about 1.5; the RGBA8 target clipped everything above. The replacement returns the game's `y` unchanged wherever it
   is at most 1.0, and above it writes `1 + 0.25 * (min(x / x1, 48) - 1)`: how many times brighter than the white point
   the scene was. `hdr_spirv_override` (`nvn_hdr.cpp`) makes all three SPIR-V loaders (`nvn_shaders.cpp`,
   `nvn_shader_prepare.cpp`, the pipeline prewarm in `nvn_pipeline.cpp`) read it instead of `spirv/<hash>.spv`.
4. **HDR pass** (`nvn_hdr.cpp`, `runtime/shaders/hdr_present.frag`): the blit writes the window texture into an RGBA16F
   image of the swapchain's size, the F1 overlay is drawn on it (so it too sits at paper white), and a full-screen
   pass writes the swapchain: values up to 1.0 are the SDR picture (optionally through the SDR look: the game's sRGB
   encode followed by the display's gamma, 2.2 or 2.4, which is what an SDR display shows); above 1.0 the scene
   brightness past white is recovered and `gain * (t^2 - t0^2)` is added in display-linear light, with `t` the
   distance past the highlight start and `t0` the white point's own distance past it (gain = 2 x Highlights);
   then the brightest channel rolls off exponentially above paper white into the peak (slope 1 at paper white, colour
   scaled as a whole), and the result is scaled by paper white / 80 for scRGB. Peak, paper white, Highlights and
   Highlight start are push constants, read every frame.

FXAA and FSR 1 work on 8-bit images only; with the RGBA16F window textures they step aside and the picture is
upscaled with the bilinear blit. The frame dumps (`NX_DUMP_FRAMES`, `NX_DUMP_PRESENTED`) only write 8-bit images and
skip the float ones in HDR mode.

## Regenerating the replacement

The setup does this itself: its `hdr` step (`tools/setup_game.py`, after `shaders`) runs the generator whenever the
shaders, the generator or the tools change. Releases bring the tools in `game-build/shadertools/`
(`tools/pack_release.py --shader-tools <the Vulkan SDK's Bin>`: `spirv-cross` and `glslc`, Apache-2.0). Without them,
or if the game's shader is not the one the generator knows, the step notes it, removes any old replacement and lets the
setup finish: HDR output then keeps the game's own tone curve. By hand, from a checkout:

    python tools/renodx_wonder.py --data C:/Games/superwonders/data

The tools are looked for in `game-build/shadertools/`, `NX_SHADER_TOOLS`, the Vulkan SDK (`VULKAN_SDK`), then `PATH`.
The output is the same on every run (glslc's debug info names a fixed relative source). It decompiles
`spirv/352e8b76df96cb1d.spv`, wraps the three output stores in `hdr_extend`, and compiles
`spirv-renodx/352e8b76df96cb1d.spv`; `--keep-glsl <file>` also writes the patched GLSL for reading. It stops with an
error if the shader's output stores no longer look as expected. `--scale` must match `kEncodeScale` in `nvn_hdr.cpp`.

## SDR look and scene grading

All of these are rows in F1 > Video > HDR and keys in `pc-settings.ini`; they apply live (HDR output itself needs a
restart). Labels and tooltips are translated into all 13 menu languages (`kHdrGradeText` in `overlay_menu.cpp`); the
new CJK glyphs were added to the Noto menu subsets (`third_party/fonts/NotoFallback`, same recipe, every existing
glyph unchanged) and to `kExtraLocalizedGlyphs`. The rows only show while HDR output is on.

- **SDR look** (`hdr_sdr_eotf`: 0 sRGB / off, 1 gamma 2.2, 2 gamma 2.4; default 1). An SDR display shows the game's
  picture as `gamma(sRGB_encode(light))`: the GPU encodes with the sRGB curve on write, the display decodes with a plain
  power. The linear picture alone lacks the second step and looks flatter, with lifted shadows. Applied in the HDR pass
  to everything up to white, UI included. `NX_HDR_SDR_EOTF` overrides it.
- **Scene grading** (percent, 100 neutral): `hdr_exposure` 25-300, `hdr_contrast` 25-200, `hdr_shadows` 25-175,
  `hdr_saturation` 0-300, `hdr_highlight_saturation` 0-300 (`NX_HDR_EXPOSURE`, `_CONTRAST`, `_SHADOWS`, `_SATURATION`,
  `_HIGHLIGHT_SATURATION` override them). They follow RenoDX's `colorgrade.hlsl` (luminance-only exposure, contrast and
  shadows with the colour kept in proportion; saturation as an OKLab chroma scale) and run inside the composite
  replacement on the scene value before the game's curve, so the curve and the highlight extension follow them. The
  parameters reach the shader through the support buffer's two transform-feedback slots (`0x550`, `0x560`: no Wonder
  shader reads them; `hdr_grading` in `nvn_hdr.cpp` fills them in `nvn_sets.cpp`). Neutral settings write nothing, so
  the buffer is what it was without HDR. Only the 3D scene is graded: the HUD, the menus and the title screen are not
  (checked in game 2026-11-23: the sliders change gameplay, the title screen stays as it is).
- Regenerate the replacement with `tools/renodx_wonder.py` after any change to the generator (the sliders themselves need
  no regeneration).
