# fnv_recomp

A static recompiler for Fallout: New Vegas (PC 1.4.0.525, the plain/GOG executable). It turns the player's own
`FalloutNV.exe` into C, compiles that natively for 64-bit Linux, and runs it on a small Win32 runtime. The goal is a
recomp-style port that can later be moved to other platforms by replacing the runtime layer.

**Nothing from the game is in this repository.** Players supply their own legally owned copy. The generated C
(`gen/`) is derived from the player's executable and must not be committed or shared. `.gitignore` excludes it.

## How it works

| Part | What it does |
|---|---|
| `tools/recomp.py` | Reads the exe. Checks it against the [fnv_decomp](https://github.com/momusdigital/fnv_decomp) fingerprints. Finds every function (seeded with the decomp's 68,657 known function addresses, plus relocations, call targets and jump tables). Emits one C function per guest function. |
| `rt/x86.h` | x86 semantics. Guest memory is a 4 GiB window at a fixed host address. Registers are C locals. Flags are lazy (the compiler folds them). x87 uses a double-precision stack. SSE uses 128-bit unions. |
| `rt/main.c` | Loader: maps the exe's sections, binds imports to host thunks, sets up TEB/PEB/TLS, dispatches indirect calls, and reports faults with the guest call stack (host frames are named `f_<guest address>`). |
| `rt/mem.c` | VirtualAlloc-style reservations and the heap behind HeapAlloc. |
| `rt/kernel32.c` and others | The Win32 surface the game imports: threads, sync objects, files mapped onto a virtual `C:` drive, INI files, and so on. |

Each guest function `0xADDR` becomes `void f_ADDR(Ctx *c)`. Calls push the real return address on the guest stack,
so the stack layout matches the original. `ret` returns from the C function.

## Build

You need Python 3.10+ (`pip install capstone==5.0.3 pefile`), clang, cmake and ninja.

    # 1. your own game folder (Data/, the .ini files, FalloutNV.exe)
    ln -s /path/to/Fallout\ New\ Vegas game
    # 2. generate C from your exe (about 1.5 minutes)
    python3 tools/recomp.py --exe game/FalloutNV.exe \
        --units ../fnv_decomp/build/units.json --image-check ../fnv_decomp/build/image_check.json --out gen
    # 3. compile (~370 MB of C: about 20 CPU-minutes)
    CC=clang cmake -S . -B build -G Ninja && ninja -C build
    # 4. run from a scratch folder
    mkdir -p run && cd run && FNV_GAME_DIR=../game FNV_TRACE=1 ../build/fnv

Environment variables:

- `FNV_GAME_DIR`: the game folder. It appears to the game as `C:\Game`, which is also its working directory.
- `FNV_USER_DIR`: where `My Games`, saves and INIs go. It appears to the game as `C:\Users\Player`.
- `FNV_TRACE=1`: logs API calls of interest. `FNV_TRACE=2` logs every host API call.

## Status

See `STATUS.md`.
