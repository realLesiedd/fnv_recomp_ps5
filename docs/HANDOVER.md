# Handover: running the project without Claude

Written by Claude for FNV HELPER, so the project can continue while Claude is unavailable. It covers what lives
only in Claude's head: how to build, test, debug and ship a test build. [AGENTS.md](../AGENTS.md) has the rules,
[STATUS.md](../STATUS.md) the history, and [BUILDING.md](BUILDING.md) and [ps5/README.md](../ps5/README.md) the
basic build steps. Read those first.

State at handover: **v41** on `main` (`1a9e61e`). The owner tests on a PS5 with system software 13.42.

## 1. Before anything else: the executable

Every build starts from the owner's own `FalloutNV.exe`: the recompiler turns it into `gen/`. Without it nobody can
build, and nothing new reaches the console.

- The owner decides who may use it, and where. It is never committed, never uploaded anywhere public, never put in
  an issue or a pull request, and never passed on.
- The same goes for the game's `Data/`, saves and any login. The owner's Steam login is never used by anyone.
- If you can't build, you can still research, write code in pull requests and review. The owner then waits for a
  build until someone with the executable can make one.

## 2. Toolchain (Ubuntu 24.04), pinned versions

| Part | Source | Pin |
|---|---|---|
| fnv_decomp (function list and executable fingerprints) | github.com/momusdigital/fnv_decomp | `0218762` (`build/units.json`, `build/image_check.json`) |
| ps5-plume-triangle (PS5 SDK, RADV, CRT, SDL2, signer) | github.com/Nazky/ps5-plume-triangle | `d54d9ad` |
| DXVK | github.com/doitsujin/dxvk | tag `v2.7.1`, plus `ps5/dxvk/dxvk-ps5.patch` |
| Python packages | pip | `capstone==5.0.3`, `pefile` |
| Compiler | Ubuntu packages | clang 18, CMake, Ninja, Meson 1.4 or newer |

RADV is built from source. `make` in ps5-plume-triangle builds the sample app (`tools/build.sh`). On a fresh
checkout that script runs `tools/build-radv.sh`, which compiles PS5_Mesa and checks `mesa_revision`. At
`d54d9ad` that revision is `0b2d6d1a61d9bbf89cf8beb88a696144f67c61f8`. `PS5_MESA_FORK` can point the clone
elsewhere. The plume tree also carries a precompiled shader cache. The driver itself is the RADV build.
`wsi_videoout_set_flip_rate` is absent from that Mesa revision. It was added later, in `aa44fa40bc5`. The plan
is in [research/120hz.md](research/120hz.md).

Put the three checkouts next to the repository (`../ext/ps5-plume-triangle`, `../ext/dxvk`) and `ps5/build.sh`
finds them without arguments.

## 3. Making a test build

1. **Generate `gen/` once** (about 90 s):
   `python3 tools/recomp.py --exe <game>/FalloutNV.exe --units <fnv_decomp>/build/units.json --image-check <fnv_decomp>/build/image_check.json --out gen`
2. **After changing `tools/recomp.py`** (for example `HOOKS`): generate into a scratch folder, and copy over only
   the files that differ. Otherwise every file is rebuilt (about 20 CPU-minutes).

       python3 tools/recomp.py ... --out /tmp/gen_new
       for f in /tmp/gen_new/*; do cmp -s "$f" "gen/$(basename "$f")" || cp "$f" gen/; done

3. **Version:** set `FNV_RELEASE` in `rt/main.c` (`"v41"` and so on). It is the first line of `fnv.log`, so every
   log says which build made it.
4. **Build:** `ps5/build.sh`. Output: `build-ps5/PPSA97701/` (eboot.bin and the app folder). Incremental builds
   take under a minute.
5. **Keep `build-ps5/llvm-pie.elf`** for each version you send (outside the repository). Crash addresses in that
   version's logs can only be read against its own ELF.
6. **Send a test build:** zip `eboot.bin` alone when `sce_sys/` and the licence files are unchanged. The owner
   copies it over `/data/homebrew/PPSA97701/eboot.bin`. `ps5/build.sh` rewrites `contentVersion` in the built
   `sce_sys/param.json` from `FNV_RELEASE`. The source `ps5/sce_sys/param.json` is left as it is, so a version
   change still means the built file changed. A release zip is `tools/package_release.sh`. It packs the app
   folder and refuses a `game/` that holds more than the placeholder.
7. **Record it:** a short entry for the version in STATUS.md (what changed and what to test), then commit (see 8).

## 4. Testing on the Linux desktop build

`CC=clang cmake -S . -B build -G Ninja && ninja -C build`, then run from a folder that acts as the user folder
(saves and settings go there):

    cd runX && FNV_GAME_DIR=<game> FNV_RENDERER=software FNV_RENDER_SHOTS_ONLY=1 SDL_AUDIODRIVER=dummy \
      FNV_PADSCRIPT="20:a,24:a,26:a,40:menua,80:menua,112:walk" \
      timeout -s KILL 130 xvfb-run -a -s "-screen 0 1280x720x24" ../build/fnv > ../run.log 2>&1

Put a save in `runX/user/Documents/My Games/FalloutNV/Saves/`. Main menu → Continue → walk is the usual script.
Without a GPU, use `FNV_RENDERER=software`. It's slow (the save loads after about 100 s), and the frame rate means
nothing there.

| Variable | Use |
|---|---|
| `FNV_PADSCRIPT` | A scripted controller: `time:token,...`. Tokens: `a`, `b`, `start`, `up`, `down`, `left`, `right`, `r3`; `walk` (left stick forward, 3 s); `turn` (right stick right, 3 s); `fire` (R2, 0.3 s); `menua` (tap ✕ every 1.5 s for 40 s, only while a menu is open: clears the add-on "items added" boxes). |
| `FNV_PADSCRIPT_EVENTS=1` | Packet numbers change only when the pad state changes, like a real pad. |
| `FNV_INPUT_LOG=1` | Logs what each game reader sees from the pad. |
| `FNV_AUDIO_DEBUG=2` | Logs every one-shot sound, and follows it to its end (volume, pan and pitch changes; how it ended; its loudest sample in the mix). Without the variable, only the first 800 one-shot sounds of a session are traced. That is the default on Linux and on the PS5. |
| `FNV_AUDIO_DUMP=<dir>` | Dumps the raw sound buffers (with `FNV_AUDIO_DEBUG`). |
| `FNV_PROF=0/1` | The built-in sampler of the main thread (on by default; `iProfiler=0` in the prefs turns it off). |
| `FNV_FASTMATH_CHECK=1` | Runs each native maths routine (`rt/fastmath.c`) next to the original and logs any difference. |
| `FNV_CONSOLE="time:line;time:line"` | Runs game console lines from the pad poll at those times in seconds (for example `150:coc Goodsprings`). |
| `FNV_DUMP_MENU=1` | Logs every Settings row (name, submenu bits, values). |
| `FNV_SHOT_EVERY`, `FNV_SHOT_AFTER`, `FNV_RENDER_SHOTS_ONLY` | Screenshots from the software renderer. |
| `FNV_TRACE=1/2` | Logs API calls of interest, or every host API call. Very large. |

Known gap: on Linux the scripted `r3` reaches the game, but the view doesn't switch to third person. It may need a
shorter press. Unconfirmed.

## 5. How the code fits together

- Each guest function `0xADDR` is `void f_ADDR(Ctx *c)` in `gen/`. `c->r[0..7]` are eax, ecx, edx, ebx, esp, ebp,
  esi, edi. x87 is `c->st[]` with `top`. Guest memory: `M32(addr)`, `M8(addr)`, `HP(addr)` (host pointer),
  `STR(addr)`.
- **Hooks:** an address in `HOOKS` in `tools/recomp.py` renames the original to `o_ADDR` and routes calls to
  `h_ADDR` (in `rt/hooks.c`, or `rt/fastmath.c` for maths). A hook must behave like the original: read the
  arguments with `ARG(n)` (from `esp + 4`), leave eax, ecx and edx as the original would, and return with
  `RETURN_STD(nargs, value)` (stdcall / thiscall) or `RETURN_CDECL(value)`. Float results are pushed on the x87
  stack. Call `o_ADDR(c)` to run the original. Keep the list short (AGENTS.md).
- **Calling game code from the runtime:** `rt_call_guest(c, addr, nargs, args)`. For thiscall, set `c->r[1]` to the
  object first. Only from the thread that owns the state, normally thread 0 (the main thread).
- **Windows APIs:** `rt/kernel32.c`, `rt/user32.c`, `rt/dsound.c` (DirectSound and the mixer), `rt/dshow.c` (MP3
  music), `rt/bink.c` (movies), `rt/dinput.c`, `rt/misc.c` (XInput, the shell, the pad script), `rt/d3d9.c` and
  `rt/hostd3d9.cpp` (Direct3D 9 on DXVK).
- **The port's own features:** `rt/options.c` (Settings rows, and the hidden debug menu), `rt/firstrun.c` (first-start settings, the
  `[fnv_recomp]` keys, settings forced every start), `rt/dualsense.c` (triggers, light bar, console lines),
  `rt/osd.c` and `rt/overlay.c` (the overlay), `rt/prof.c` (profiler), `rt/glyphs.c` (button prompts).

### Game addresses in use

| Address | What |
|---|---|
| `0x11DEA3C` | The player. `+0x64B` is non-zero in third person (the game's own check is `0x524D10`). `+0x40` is the cell. |
| `0x11D8A80` | InterfaceManager. `+0xC == 1` outside menu mode. |
| `0x702360` | MenuMode(): non-zero when a menu is up (`rt/dualsense.c`, `rt/options.c`). |
| `0x11DAAC0` | StartMenu (main and pause menu) while it exists. Flags at `+0x1A8`. Last action tick at `0x11DAAB4`. |
| `0x11DAB50` | The Settings rows array. Row constructor `0x7D6350`. |
| `0x11DA0C0` | LoadingMenu while it exists. |
| `0x5AC400` | Script::CompileAndRun (the console runner in `rt/dualsense.c`). |
| `0x8391B0` | PlayFootstepImpact. `rt/hooks.c` logs a player step, then runs the original. The v40 log recorded no player calls on that walk. |
| `0x40FBF0`, `0x4538E0` | RecursiveSpinLock::Lock, and the critical-section wrapper seen in the save-load deadlock. |

## 6. Reading the owner's logs

- `fnv.log` line 1: the version. `perf:` every 5 s gives fps, frame times, main-thread CPU and memory. `slow frame:`
  is a frame over 150 ms, and only the first 200 (`hb_frame_end` in `rt/hostd3d9.cpp`). `prof:` once a minute splits
  main-thread time into game code, port, libraries and system, with the hottest game functions by address.
- **Crashes:** `rt_fatal` prints the registers, the last API, and a host backtrace. It does not walk the guest
  stack. On Linux the `f_` names in that backtrace are guest functions. Host addresses from the stall sampler,
  minus `0x400000`, are addresses in that version's `llvm-pie.elf` (`llvm-symbolizer-18 --obj=llvm-pie.elf 0x...`;
  the note is in `rt/main.c`). `prof:` offsets are already ELF addresses.
- **Hangs:** a critical section held for over 5 s logs who holds it, and both guest stacks (`rt_cs_wait_report`).
- `_d3d9.log` is DXVK's own (`rt/hostd3d9.cpp` sets `DXVK_LOG_PATH` to the app folder). The `PS5 WSI` lines list
  each display mode and the mode picked (`createSurface` in `ps5/dxvk/dxvk-ps5.patch`). The patch prefers
  3840x2160 near 60 Hz. Only 4K at 59.94 Hz has been seen on the owner's console.

## 7. Rules learned the hard way

These are in AGENTS.md. The ones that bite most:

- **PS5 system functions are direct imports.** `sceKernelDlsym` and `dladdr` fail on the console. `dlsym` is a
  different call: the GPU path uses it for Vulkan. A new import the system can't resolve may stop the app from
  starting, so ship it in a build that also tests something else. `sceKernelGetCpuUsage` is refused on 13.42
  (`0x8002004e`). `rt/osd.c` hides the per-core row when that call fails.
- **The main thread is the bottleneck**, at about 70% game code. Anything per frame must be cheap. The frame
  limiter spins, so "main thread CPU" includes waiting. Use the profiler groups to see real work.
- **Keep `bUseMultiThreadedFaceGen=0`.** It is forced every start. On, it deadlocked save loads. The game's other
  threading switches gave no speed-up.
- **Never cap above 60.** The physics break.
- **Never call the console runner from Present**, or while a load is in progress (deadlock, v30). Use the pad poll
  instead.

## 8. Commits and pull requests

- Commits on `main` appear under the owner's GitHub account, with GitHub's private no-reply address. The
  owner gives you the exact identity. No assistant names, trailers or sign-offs.
- No personal information anywhere (AGENTS.md).
- Never commit `gen/`, build output, zips, logs, saves or game files.
- Code is merged only after the owner has tested it on the PS5.

## 9. Open work, in order

| # | Item | State |
|---|---|---|
| #20 | Settings > Audio sliders barely change the sound | Research handed over. Likely also why first-person footsteps are hard to hear. |
| #16 | First-person footsteps quiet | Explained by the v40 log: third person uses a loud 3D sound at the feet. First person uses a 2D sound the game sets to about −15 dB (−10 dB after the owner's slider change). Not lost. Revisit after #20. |
| #14 | Release packaging | On main. `ps5/build.sh` stamps `contentVersion` in the built param.json from `FNV_RELEASE`. `tools/package_release.sh` zips the app. |
| #15 | Debug menu (hidden setting) | On main. Hidden Settings > Controls rows in `rt/options.c` (`bDebugMenu`). |
| #12 | Mods folder | Research and a plan. |
| #13 | Trophies | On hold until the owner says which loader is running. See [research/trophies.md](research/trophies.md). |
| #4 | 120 Hz / 40 fps | Planned: a RADV cherry-pick, then a test on a 120 Hz TV. Parked until the sync-state and x87 work. RADV is built from source by `tools/build-radv.sh` (PS5_Mesa `0b2d6d1a61d`). `wsi_videoout_set_flip_rate` does not exist at that revision. It was added in `aa44fa40bc5`. The plan is in [research/120hz.md](research/120hz.md). |
| #3 | Per-core CPU | Closed as won't fix. 13.42 does not report per-core load. `sceKernelGetCpuUsage` returns `0x8002004e`, and `rt/osd.c` hides that row. |
| Speed | Recompiler: keep x87 values in C locals | The biggest remaining speed-up (x87 through memory is about 2.8× slower than native, per v38). Large job. Touches `tools/recomp.py` and `rt/x86.h`. Verify with `FNV_FASTMATH_CHECK`-style comparisons before any build. |
| Speed | Cheaper per-draw state sync in `rt/d3d9.c` | Needs a change counter so unchanged state isn't looked up every draw. |
| Release | Long play sessions, two or three outside testers, the owner's message to fnv_decomp | See 'Release checklist' in STATUS.md, and README "Known issues". |
