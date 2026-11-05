# Status

## Working (tested with the GOG FalloutNV.exe 1.4.0.525)

- **Recompiler.** Finds 87,340 functions (3.73M instructions) and emits C for them. 209 instruction sites are not
  supported yet: MMX, and a few odd ones in data-like code. All of it compiles with clang -O1.
- **Startup.** The recompiled exe runs through:
  - CRT startup, static constructors and TLS;
  - WinMain: reading the INIs and registry, probing the data files and archives;
  - thread creation (9 game threads start), window class and window creation;
  - DirectInput (keyboard and mouse that report no input), and a DirectSound probe (it reports "no driver");
  - writing RendererInfo.txt.
- **Direct3D 9 front end** (`rt/d3d9.c`).
  - IDirect3D9 reports an SM3 GPU (shown as "NVIDIA GeForce 8800 GT (fnv_recomp)"). IDirect3DDevice9 has all
    119 methods.
  - Textures, cube and volume textures, surfaces, vertex and index buffers, shaders, vertex declarations, queries,
    state blocks and the swap chain are all implemented.
  - Resource memory lives in guest memory, so Lock calls hand back plain pointers.
  - Drawing is a **null renderer**: it counts draw calls and frames but produces no pixels.
- **D3DX math** (`rt/d3dx.c`): matrix and vector helpers, ID3DXBuffer and the shader profile queries.
- **DDS texture loading** (D3DXCreate*TextureFromFileInMemory, D3DXGetImageInfoFromFileInMemory).
- **A silent DirectShow graph** for the music player.
- **Main menu reached (headless).** Tested with a real Steam install (app 22380 with all DLC), with the
  GOG exe swapped in.
  - It loads the 10 plugins (FalloutNV.esm plus all DLC .esm files) and every BSA.
  - It shows the loading screens, then builds the start menu (`Menus\Options\start_menu.xml`, `main_title.dds`,
    `main_background.dds`).
  - It then stays in the main menu loop: over 19,500 frames in 2 minutes, about 19 draw calls per frame, with no
    errors.
  - Nothing is visible yet, because the renderer is the null backend.

### First-run config

Without `My Games\FalloutNV\FalloutPrefs.ini`, the game runs `FalloutNVLauncher.exe` and quits, as it does on
Windows. It also reruns the launcher if `sD3DDevice` doesn't match the adapter name. Minimal file:

    [Display]
    sD3DDevice="NVIDIA GeForce 8800 GT (fnv_recomp)"
    iSize W=1280
    iSize H=720
    bFull Screen=0

## Next, in order

1. **GPU backend for the D3D9 front end.** This needs a translator from SM2/SM3 shader bytecode to GLSL or
   SPIR-V, plus OpenGL or Vulkan draw submission. Every draw call already arrives at `be_draw()` with the full
   device state in `D`. The rest of d3dx9_38 is still to do: the constant table, DDS texture loading and
   D3DXLoadSurfaceFromSurface.
2. **Game data.** Booting to the main menu needs the player's `Data/` folder: the .esm, the BSAs and the INIs.
   The test container only has the exe, so it has never gotten this far here.
3. **Structured exception handling and C++ throw.** These currently stop with a clear message. They need SEH
   chain dispatch, plus unwinding across host frames (setjmp in functions that catch).
4. **A windowing and input backend** (SDL) in place of the headless USER32. Then audio (DirectSound buffers),
   Bink video (or skipping the intro videos) and libvorbisfile (host libvorbis).
5. **Console target.** Only the runtime (`rt/`) is platform-specific. The generated code is portable C, apart from
   the fixed 4 GiB guest window, which needs a large reserved virtual range.

## PS5 (jailbroken, firmware 13.42): progress

- **Playable on hardware (v13):** main menu, New Game through character creation, Goodsprings and the Mojave;
  saving, loading (from the menu and in game) and Continue; Exit Game closes the app back to the home screen.
- **About 60 fps** (the TV's 59 Hz cap) indoors and out at a 1280x720 render upscaled to 4K. Outdoors the main
  thread is at 91-96% CPU, so there is little headroom yet.
- **Sound:** DirectSound mixer, .wav effects (mmio), .ogg voices (stb_vorbis), .mp3 music (dr_mp3).
- **Fixed along the way:**
  - MMX, SSE shifts and x87 save/restore are recompiled.
  - The MP3 decoder is no longer copied, which crashed on music changes.
  - TLS slots are cleared in every thread on TlsAlloc/TlsFree, which crashed save loads.
  - Trivial KERNEL32 calls are inlined, which removes the per-call clock read; that also cured a PS5 load stall.
  - The loading-screen thread no longer crashes on quit.
  - Exit Game closes like FileDen (sceSystemServiceLoadExec("exit")).
  - Continue is not listed until a save exists.
- **Diagnostics in fnv.log:** the release on the first line; a "perf:" line every 5 s (fps, frame times, CPU,
  memory, GPU memory); a stall report naming what each thread is doing if the game stops polling input.
- **v14, memory and CPU:**
  - D3D9 reference counting now follows D3D9. Binding drops the previous binding's reference, and a texture's
    surfaces share its count. Before this, every texture ever drawn stayed alive: in a v11 log, video memory grew
    from 682 to 1837 MB in three minutes of travel.
  - Released D3D objects are freed. The object table grows in 32 KB chunks instead of a fixed 8 MB.
  - DXVK uses 4 shader-compiler threads instead of 16.
  - The perf line reports per-thread CPU, the three busiest threads, and live/created D3D objects.
  - Startup logs the configured and free host (flexible) memory.
- **v15, intro and credits:**
  - The intro movie (FNVIntro.bik, 1280x720, 4 min 50 s, shown on New Game) plays with sound. FFmpeg's Bink decoders
    sit behind the binkw32 calls (`rt/bink.c`) and the movie's sound goes to the mixer. Decoding takes about
    3.5 ms per frame, plus about 4 ms to convert each frame to BGRx (measured locally). A button press skips it, as
    on PC.
  - The game draws movies as fixed-function textured tiles, which came out black through DXVK here. The GPU
    backend instead draws the decoded frame over the whole screen before presenting.
  - The Credits screen (main menu) already worked: scrolling credits over the artwork.
- **v15 on PS5:** 331 MB of host (flexible) memory free, unchanged through play; it was 70 MB in v13. Video memory
  stays around 1.1 GB instead of climbing. D3D objects are freed (2,500 live of 17,800 created). Exit closes
  cleanly. Process CPU is 45% in menus and up to 370% while loading, so no thread spins; per-thread CPU clocks read 0
  on PS5. Three 0.7 s stutters in about 100 s of play (v13's worst was 0.2 s).
- **v16:** 8 shader-compiler threads (from 4), and DXVK's "compiling shaders" HUD indicator to tie stutters to
  shader compiles. The controller's name (the player's account name on PS5) is no longer logged.
- **v17, quality:** FalloutPrefs.ini uses the game's VeryHigh preset: full-resolution textures, 15x anisotropic
  filtering, and the longest object, tree, terrain and grass distances, plus higher shadow and water settings. It
  leaves out the preset's multisampling. An existing medium file is raised once (marker `[fnv_recomp]
  iSettingsVersion`), so later changes in the game's settings menu stay. Test harness: typed text reaches the
  game as WM_CHAR, and key presses shorter than a frame are latched.
- **v16 on PS5 (36 min session):** the intro plays (6.2 ms per frame to decode, skipped at 33 s), no crash, clean
  exit. Memory flat: game RAM 1.0-1.1 GB, video memory 1.9 GB, 331 MB host memory free, about 8,500 of 51,000 D3D
  objects alive. Mostly 60 fps, but single-frame stalls of up to 1 s with the CPU mostly idle, each with about 150
  new D3D objects.
- **v18:** the log reports every frame over 150 ms with a breakdown: draws (pipeline compiles happen there),
  creating GPU objects, uploads, other device calls, Present and game time, plus objects created/freed and MB read.
- **v19:**
  - **PlayStation button prompts.** The game's Xbox glyphs (`textures\interface\shared\button\*.dds`,
    used by barter, dialogue, books, caravan, blackjack and HUD hints) are replaced by original Cross, Circle,
    Square, Triangle, L1/R1, L2/R2, L3/R3, Options and Create drawings (`tools/make_glyphs.py`).
  - **Overlay folders** (`rt/overlay.c`). Folders laid over the game folder: their files replace the game's, and
    their folders merge into directory listings. The game takes a loose file over its archive copy when it finds
    one, and checks every texture and mesh by name. The app's assets are built into the executable
    (`tools/embed_assets.py`) and written to `user/assets` on start, so nothing beyond eboot.bin is uploaded.
    The mods folder can use the same mechanism.
  - **GPU backend:** state is synced only for kinds the game changed since the last draw (render, sampler and
    texture-stage states, constant ranges, transforms, viewport, material). Before this, every state was compared
    on every draw.
  - The DXVK HUD is twice as large (scale 4).
- **v18 on PS5 (28 min):** stutters are almost all the game's own code, not graphics (draws, uploads and Present
  are a few ms). Autosaves take about 255 ms, with 150 ms of that waiting on file writes. About 400 D3D objects
  are created in a single frame, every few seconds (about 160 ms each, of which 130 ms is CPU). Area loads take
  0.4-0.8 s.
- **v20:**
  - **Fire, smoke and dust flicker fixed.** Dynamic vertex buffers locked with D3DLOCK_DISCARD now get fresh GPU
    storage. They used to be written in place with NOOVERWRITE while the previous frame's draws still read them,
    so particles rebuilt every frame jumped between nothing and big bursts. Measured at a Primm fire barrel: the
    average frame-to-frame brightness change dropped from 8.2 to 3.2.
  - **File writes** are gathered per file in 256 KB pieces (saves were thousands of small system writes). A
    save written this way loads back correctly.
  - The slow-frame report lists the kinds and sizes of objects in a burst. FNV_SHOT_AFTER (seconds) starts test
    screenshots late.
  - Seen once locally: a guest null read in the script runner (RunScript 0x5E0D20) just after loading the Primm
    save. Four other loads of the same save did not hit it, so it looks like a timing race in the game.
- **To watch:** stutters in game; frame rate outdoors at the longer draw distances; dust and smoke clouds
  (player video: heavy orange sheets with hard edges, cause not found yet). The old "all threads 100%" figure came from a
  process clock that is not meaningful on PS5; v14 uses getrusage plus per-thread clocks.

## Roadmap (the player's goals, in order)

1. **The game runs reliably on PS5** (current focus): loading a save in the Mojave stalls on PS5 only (v10 logs a
   stall report to find out why); remaining crashes as they are found.
2. **Performance:** a steady 30 fps, then 60. Long-term goal: 4K at 120 fps, which also needs engine fixes for
   frame rates above 60 and a 120 Hz display mode.
3. **Native 4K rendering** and a resolution setting in the options menu.
4. **Trophies** through [RecompTrophy](https://github.com/cptlamarca/RecompTrophy) (lamarking, GPL-3.0), as the
   gta4-ps5 port does, driven by the game's achievement unlocks.
5. **Full DualSense support:** PlayStation-style button prompts (original glyphs, not Sony's assets), haptics,
   adaptive triggers and the light bar where they fit.
6. **Mods in their own folder:** `mods/<ModName>/` next to `game/` on the console, laid over `Data\` by the
   runtime's file layer so the game files stay untouched. Plugins.txt and archive invalidation are generated from the
   enabled mods; an in-game setting turns mods on or off (applies after a restart), and holding a button at start-up
   launches without mods in case one breaks the game.
7. **NVSE:** rebuild the open-source xNVSE into the port, with its code patches done as recompiler/runtime hooks,
   then open-source plugins (such as JIP LN) the same way. Closed-source plugins can't be supported.

## GPU renderer (DXVK): working on Linux

- `rt/hostd3d9.cpp` replays the D3D9 front end's draws on a DXVK device (Direct3D 9 on Vulkan).
  - Resources upload when the game has locked them.
  - State syncs only where it changed.
  - Shaders pass straight through.
  - Render targets live on the GPU and are read back on request.
- Tested with Mesa's lavapipe (software Vulkan) under Xvfb. The main menu renders correctly at about 60 fps (on a
  software Vulkan driver).
- Build: `cmake -DFNV_DXVK_DIR=<dxvk native prefix>`. `FNV_RENDERER=software` forces the CPU renderer.
- Next for PS5:
  1. Build DXVK with the PS5 toolchain against RADV (PS5_Mesa).
  2. Give it a window through `VK_KHR_display`.
  3. Package as a home-screen app like ps5-plume-triangle.
