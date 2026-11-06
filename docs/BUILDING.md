# Building fnv_recomp

There are two builds:

- **The PS5 app**, the one players use. See [ps5/README.md](../ps5/README.md).
- **A desktop Linux build** of the same game and runtime, used for development and testing. It runs the game in a
  window (or headless) and is described below.

Both start from C code generated from your own `FalloutNV.exe`. The generated code (`gen/`) is not in this
repository.

## What you need

- Your own copy of Fallout: New Vegas for PC, version 1.4.0.525, with the DRM-free executable from the GOG release
  (Steam's executable is encrypted on disk and cannot be read).
- A local checkout of [fnv_decomp](https://github.com/momusdigital/fnv_decomp), for its `build/units.json` (the
  function list) and `build/image_check.json` (the executable fingerprints).
- Python 3.10 or newer, with `pip install capstone==5.0.3 pefile`.
- clang, CMake and Ninja.

## 1. Generate the C code

    # your own game folder (Data/, the .ini files, FalloutNV.exe)
    ln -s /path/to/Fallout\ New\ Vegas game
    python3 tools/recomp.py --exe game/FalloutNV.exe \
        --units ../fnv_decomp/build/units.json --image-check ../fnv_decomp/build/image_check.json --out gen

This takes about a minute and a half. It checks the executable against the fingerprints, then writes about 370 MB
of C to `gen/`.

## 2. Build and run on Linux

    CC=clang cmake -S . -B build -G Ninja && ninja -C build
    mkdir -p run && cd run && FNV_GAME_DIR=../game ../build/fnv

Compiling `gen/` takes about 20 CPU-minutes.

| Variable | Meaning |
|---|---|
| `FNV_GAME_DIR` | The game folder. The game sees it as `C:\Game`, its working directory. |
| `FNV_USER_DIR` | Where `My Games`, saves and settings go. The game sees it as `C:\Users\Player`. |
| `FNV_TRACE` | `1` logs API calls of interest, `2` logs every host API call. |
| `FNV_RENDERER` | `software` draws on the CPU, for machines without a Vulkan GPU. |

## How it works

| Part | What it does |
|---|---|
| `tools/recomp.py` | Reads the executable and checks it against the fnv_decomp fingerprints. Finds every function (seeded with the decomp's 68,657 known function addresses, then relocations, call targets and jump tables) and writes one C function for each. |
| `rt/x86.h` | x86 semantics. Guest memory is a 4 GiB window at a fixed host address. Registers are C locals, flags are computed lazily, x87 uses a double-precision stack and SSE uses 128-bit unions. |
| `rt/main.c` | The loader. Checks the player's executable, maps its sections, binds imports to host functions, sets up TEB/PEB/TLS, dispatches indirect calls and reports faults with the guest call stack. |
| `rt/mem.c` | VirtualAlloc-style reservations and the heap behind HeapAlloc. |
| `rt/kernel32.c` and the other `rt/*.c` files | The Windows API the game imports: threads, synchronisation, files on a virtual `C:` drive, INI files, DirectSound, DirectShow, XInput and more. |
| `rt/d3d9.c`, `rt/hostd3d9.cpp` | The Direct3D 9 front end and its link to DXVK. |
| `rt/platform_sdl.c` | Window, video output, audio and the DualSense, through SDL2 (on the PS5, its PS5 port). |

Each guest function at `0xADDR` becomes `void f_ADDR(Ctx *c)`. Calls push the real return address on the guest
stack, so the stack layout matches the original, and `ret` returns from the C function.

Progress notes for each version are in [STATUS.md](../STATUS.md). The phased plans are at the end of that file.
Questions the tree does not answer are in [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).
