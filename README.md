# Super3D

A static recompilation of **Super Mario 3D World + Bowser's Fury** (`010028600EBDA000`) for PC using a native Vulkan / NVN runtime.

> **Disclaimer:** This repository contains **no copyrighted game assets, binaries, shaders, or keys**. You must supply your own legally dumped Nintendo Switch copy (ExeFS & RomFS).

---

## Project Status & Testing

- **Tested Hardware & Performance:** Tested on **NVIDIA RTX 5060 Ti (16 GB VRAM)** in **World 1** at **200–240 FPS**.
- **Other GPUs:** Other graphics cards (AMD, Intel, and other NVIDIA models) are currently **untested** — performance may vary depending on your PC.
- **Game Coverage:** Testing so far has only been done in **World 1** of *Super Mario 3D World*; later worlds and ***Bowser's Fury*** have not been tested yet.

## Supported Platforms & Roadmap

- **Currently Supported:** **Windows (x64)** only.
- **Planned for Upcoming Releases:**
  - **Linux** support
  - **16:10** aspect ratio support

## Built On

Super3D is built on the **ToTK recomp** framework and my other Switch static recompilation project, [**SuperWonders**](https://github.com/vfjoo888/SuperWonders).

---

## Features

- **Native Static Recompilation** — Translates the Switch `main` and `sdk` executables into native x86-64 code (`NX_GAME=super3d`).
- **Vulkan Graphics Backend** — Full NVN-to-Vulkan translation with Maxwell-to-SPIR-V shader translation, GPU ASTC decoding, async pipeline compilation, and internal resolution scaling.
- **SM3DW Renderer & Runtime Support** — Implements cubemap `2D_ARRAY` views, copied `NVNtexture` / `NVNSAMP1` sampler token tracking, `ClearTexture` & `BufferFlushMappedRange` synchronization, `nvnQueueAcquireTexture` token retention, SDK 10/11 `AudioRenderSystem` offsets, and `nn::swkbd::InlineKeyboard` / controller applet hooks.
- **Ultrawide Support (21:9 & 32:9)** — Native `Hor+` camera projection and widened render targets with 16:9-safe UI scaling.
- **In-Game Overlay & Settings** — Dear ImGui settings menu (`Tab` / controller shortcut) for display, aspect ratio, resolution, VSync, frame cap, audio, controller remapping, deadzones, and performance/speedrun overlays.

---

## Requirements

- **Your own dumped copy** of *Super Mario 3D World + Bowser's Fury* (`ExeFS` + `RomFS`)
- **OS:** Windows 10/11 (x86-64, AVX2) *(Linux support planned for upcoming releases)*
- **GPU:** Vulkan 1.3 capable GPU with up-to-date drivers
- **Build Toolchain (building from source):**
  - CMake `>= 3.25` & Ninja
  - Clang / LLVM-MinGW (Windows) or Clang (Linux)
  - Vulkan SDK (`glslc` / `glslangValidator`)
  - Python 3 (`capstone`, `lz4`, `zstandard`, `fontTools`) + .NET SDK (for `spirv_harness`)

---

## Building from Source

Configure and build with `NX_GAME=super3d`:

```bash
cmake -B build-super3d -G Ninja -DNX_GAME=super3d -DCMAKE_BUILD_TYPE=Release
cmake --build build-super3d -j
```

Then prepare your game data (ExeFS + RomFS) using `tools/setup_game.py` or the built-in launcher UI.

---

## Project Layout

| Path | Description |
| :--- | :--- |
| [`games/super3d/`](games/super3d/) | Game-specific entry point & hooks (`super3d`) |
| [`runtime/`](runtime/) | HLE kernel, OS/SDK services, audio, input, overlay, and Vulkan NVN backend |
| [`recomp/`](recomp/) | Static AArch64-to-C++ recompiler & `nso2bin` |
| [`tools/`](tools/) | Game setup (`setup_game.py`), Maxwell shader extraction/translation, and packaging scripts |
| [`docs/`](docs/) | Technical documentation for the runtime, NVN backend, and shader translator |

---

## License

This project is licensed under the **MIT License** — see [LICENSE](LICENSE) for details.
