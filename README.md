# Super3D

A static recompilation of **Super Mario 3D World + Bowser's Fury** (`010028600EBDA000`) for PC using a native Vulkan / NVN runtime.

> **Disclaimer:** This repository contains **no copyrighted game assets, binaries, shaders, or keys**. You must supply your own legally dumped Nintendo Switch copy (ExeFS & RomFS).

---

## Project Status & Testing

- **Tested Hardware & Performance:** Tested on **NVIDIA RTX 5060 Ti (16 GB VRAM)**:
  - **Linux:** Tested in **World 1** and the first levels of **World Star** at a stable **240 FPS**.
  - **Windows:** Tested up to **World 2** at **200–240 FPS**.
- **Other GPUs:** Other graphics cards (AMD, Intel, and other NVIDIA models) are currently **largely untested** — performance may vary depending on your PC.
- **Game Coverage:** Later worlds and ***Bowser's Fury*** have not been fully tested yet.

## Supported Platforms & Roadmap

- **Currently Supported:** **Windows (x64)** and **Linux (x64)**, with **16:9**, **16:10**, **21:9**, and **32:9** aspect ratios.
- **Planned for Future Releases:**
  - **Online Multiplayer** via **Nextendo**
  - Support for game version **1.2.2**
  - **Toggleable Wii U movement speed & physics** (experimental)

## Built On

Super3D is built on the **ToTK recomp** framework and my other Switch static recompilation project, [**SuperWonders**](https://github.com/vfjoo888/SuperWonders).

---

## Features

- **Native Static Recompilation** — Translates the Switch `main` and `sdk` executables into native x86-64 code (`NX_GAME=super3d`).
- **Vulkan Graphics Backend** — Full NVN-to-Vulkan translation with Maxwell-to-SPIR-V shader translation, GPU ASTC decoding, async pipeline compilation, and internal resolution scaling.
- **SM3DW Renderer & Runtime Support** — Implements cubemap `2D_ARRAY` views, copied `NVNtexture` / `NVNSAMP1` sampler token tracking, `ClearTexture` & `BufferFlushMappedRange` synchronization, `nvnQueueAcquireTexture` token retention, SDK 10/11 `AudioRenderSystem` offsets, and `nn::swkbd::InlineKeyboard` / controller applet hooks.
- **Ultrawide & 16:10 Support (16:10, 21:9 & 32:9)** — Native camera/projection scaling, widened/adjusted render targets, and automatic HUD/UI layout mods.
- **High-FPS Support (up to 240 FPS)** — Fixed 60 Hz simulation timestep with interpolated camera and skeletal animations (`60 / 90 / 120 / 144 / 240 FPS`).
- **Profiles & Emulator Save Import** — Multiple user profiles with automatic save detection (`GameData.bin`) from Eden, Ryujinx, Ryubing, Yuzu, Sudachi, Suyu, and Citron.
- **In-Game Overlay & Settings** — Dear ImGui settings menu (`F1` / controller shortcut) for display, aspect ratio, resolution, VSync, frame cap, audio, controller remapping, deadzones, and performance/speedrun overlays.

---

## Requirements

- **Your own dumped copy** of *Super Mario 3D World + Bowser's Fury* v1.0.0 (`ExeFS` + `RomFS`)
- **OS:** Windows 10/11 or Linux (x86-64, AVX2)
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
