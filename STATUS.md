# Status

Later notes override earlier ones where they disagree. Three early plan lists are marked as of v35: [Next, in order](#next-in-order), the [roadmap](#roadmap-the-players-goals-in-order), and [Next for PS5](#gpu-renderer-dxvk-working-on-linux) under the GPU renderer. [Future plans](#future-plans) is the list to work from. Questions this tree does not answer are in [docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md).

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

Superseded as a plan. Written before the PS5 build played, and left as it was. As of v35: the GPU backend is `rt/hostd3d9.cpp` on DXVK, on Linux and on PS5. Booting with the player's `Data/` is how the PS5 app reaches the Mojave. SDL input is in `rt/platform_sdl.c` (a window on Linux; the controller and events on PS5, with the picture coming from DXVK). The DirectSound mixer, Bink intro and Vorbis voices are in `rt/dsound.c`, `rt/bink.c` and `rt/vorbis.c`. The console target is the `PPSA97701` app. What this list still asks for is SEH and C++ unwinding: `rt/kernel32.c` stops the process on `RaiseException` (other than the thread-name code) and on `RtlUnwind`. DDS loading and `D3DXLoadSurfaceFromSurface` are in `rt/d3d9.c` now. The D3DX constant table is not in `rt/d3dx.c`. The "209 unsupported instruction sites" in the recompiler bullet above is that first pass, including MMX. The commit that added the roadmap records the count falling from 132 to 119, and describes the remainder as bytes in data that the disassembler treated as code. `tools/recomp.py` still prints the count when it runs. `gen/` is not in this repository, so a newer count is not stored here. The emitter still rejects some mnemonics (`pcmpgtb`, MMX beyond `movq` / `paddw` / `psubw` / `pcmpgtw` / bitwise, unknown x87 and SSE, `gs`, nested `enter`, `hlt`).

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
- **v21, main thread speed:**
  - **Waits.** Every Win32 wait (events, semaphores, threads) used one shared condition variable, so each
    SetEvent or ReleaseSemaphore woke every waiting game thread. Worker threads spent a fifth of their time in
    kernel wake-ups, and the main thread waited on them. Each waiting thread now has its own condition variable,
    linked into the objects it waits on, and only those wake. Measured CPU-only (no rendering) at Goodsprings,
    VeryHigh: 52-63 fps became 89-97 fps, and process CPU fell from about 200% to 145%.
  - **Indirect calls** use a 6 MB table indexed by address / 16 (85% of functions start on a 16-byte boundary)
    plus a small hash table, instead of a per-thread cache that fell back to a binary search.
  - Loading, saving, menus and sound were checked with these changes.
  - Button prompts: the overlay now writes Data\ArchiveInvalidation.txt (the game's own list of archive files to
    take loose), but the save menu still shows the Xbox glyphs. Not solved yet.
- **v22, PlayStation button prompts (working):** the game draws its controller prompts from 64x64 cells of its UI
  atlas, textures\interface\interfaceshared0.dds (1024x1024 DXT5, laid out by interfaceshared.tai). It never
  reads the separate general_button_*.dds files, which is why the loose-file overlay and the archive-read swap
  could not reach them. When the atlas is uploaded it is recognised by fingerprint, and its 22 button cells (mipmaps
  included) are overwritten with the drawings from tools/make_glyphs.py. Seen in the save menu (Delete triangle,
  Back circle) and the Pip-Boy (General square).
- **v38, native maths for the hottest culling and transform routines:**
  - The owner's v37 PS5 profile (10 min, clean exit): in play the main thread was ~70% game code, ~13% port
    runtime (sync_state, the D3D9 front end, rt_call_ind ~2%), ~6% DXVK/driver, ~10% waiting; mostly 56-58.5 fps
    with the main thread at 100%. A render thread would only take the ~6%. The hottest game functions were small
    x87 routines compiled without optimisation: culling (sphere against plane 0x4B6100, plane side 0x49DA80, the
    fade-node and frustum walkers that call them) and transform maths (Transform*Transform 0x62C250,
    Matrix3*Matrix3 0x43F8D0, Matrix3*Vec3 0x4B4500, Vec3 +, -, scale, constructor, dot, plane distance).
  - Those 11 are now native (rt/fastmath.c, listed in HOOKS): the same operations in the same order in double, rounded
    to float where the original stores a float, the same eax/ecx/edx and stack effect, results through a temporary.
    FNV_FASTMATH_CHECK=1 ran original and native side by side on the owner's Primm save: 30 million calls, no
    difference in registers, x87 stack or memory. Same busy view, same build: those functions took 8.9% of the main
    thread as recompiled code and 3.2% native (2.8x). [fnv_recomp] iFastMath=0 runs the originals.
  - Lesson for the next step: recompiled x87 code is ~2.8x slower than native, and the rest of the profile is flat
    (top function ~3%). Keeping x87 values in C locals inside the recompiler (instead of c->st) would speed up all
    of the game's floating-point code at once. The frame cap spins while it waits, so "main thread CPU" barely moves
    when a 60 fps scene gets cheaper; the profiler's groups and per-function samples are the measure.
  - For #4 (120 Hz research): the PS5 WSI logs every display mode with its refresh in millihertz, and picks 3840x2160
    near 60 Hz rather than the first 4K mode (a 119.88 Hz mode is listed first when 120 Hz is enabled).
  - The profiler counts 64-byte spots, so its offsets name the exact function. iFastMath and iProfiler are written
    into FalloutPrefs.ini with their defaults.
- **v37:** v36 crashed at exactly 60 s on both of the owner's launches, in the profiler's own thread right after its
  first summary (SIGBUS in libc's strstr): on the console dladdr fails for most addresses, and the profiler then
  read the name out of an uninitialised Dl_info, which the first summary's snprintf had left non-null. The same
  failure had put every non-game sample under "system". The profiler now sorts samples by address alone: the app
  image runs from __dso_handle to __init_array_start on PS5 (runtime, then gen/, then DXVK, RADV, SDL, FFmpeg),
  and everything else is the system. Its offsets are addresses in build-ps5/llvm-pie.elf. Before the crash, v36
  ran at 56-60 fps with the main thread at 83-100%.
- **v36, a save-load hang fixed, and the first speed-up:**
  - Loading a save could hang (the owner hit it pressing Continue on v35): three game threads in a lock cycle,
    with the game's threaded FaceGen as the link. Main held critical section 0x11C5CF0 building a face
    (GetOrCreateFaceGenModel) and waited for the FaceGen lock 0x11D5A9C; BSTaskManagerThread held that and spun on
    a game spinlock (0x40FBF0) while freeing memory; BackgroundCloneThread held the spinlock and waited for
    0x11C5CF0. Reproduced 3 of 3 with the owner's Primm save. [BackgroundLoad] bUseMultiThreadedFaceGen=0 is now
    forced on every launch (rt/firstrun.c), and the same save loaded every time. A lock wait over 5 s is now
    logged with the holder and both guest stacks (cs wait lines).
  - Button presses are latched until the game has read them twice, so a tap during a hitch is not lost.
  - The 30 gen/ files that held ~80% of the main thread's game-code time in a Primm profile are built at -O2
    (tools/hot_gen_files.txt; the rest stay -O1). Same busy Primm view (~1,700 draws a frame, Linux, software
    renderer): main thread 87.4% -> 76.1% at the 60 fps cap. No faults, hangs or lock waits in that run.
  - A built-in profiler (rt/prof.c) samples the main thread ~200 times a second and logs a split once a minute:
    game code, port runtime (this runtime and the D3D9 layer), DXVK/driver/libraries, system (incl. waiting),
    with the hottest game functions and offsets for the rest. On by default; [fnv_recomp] iProfiler=0 turns it off.
    On Linux the link puts the runtime after gen/, so it is counted under libraries there; the PS5 link order
    (runtime, then gen/, then DXVK and the driver) is what the groups assume.
  - The game's optional threading switches (threaded AI, morpher, particles, blood, audio) were measured: no
    change in main-thread load in that view (87.0% vs 87.4%), ~15% more CPU on other cores. Left off.
- **v35:** Trigger Strength spread out, since Light still felt strong: the scale on each
  weapon's Max strength is 0.15 / 0.3 / 0.55 / 1.0 (was 0.4 / 0.65 / 0.85 / 1.0), so a pistol's resistance is
  1 / 2 / 4 / 7 out of 8 for Light / Medium / Strong / Max.
  The app now checks the player's FalloutNV.exe before running it (FNV-1a 64 of .text, and of .rdata+.data): the
  Steam executable (encrypted) or another version stops with a clear line in fnv.log instead of crashing somewhere
  random. Release preparation: README rewritten for players (requirements and versions, install, controls,
  settings, known issues, FAQ), GPL-3.0-or-later LICENSE (the payload SDK and the app start-up code linked into
  the app are GPL-3.0), THIRD_PARTY_NOTICES.md with LICENSES/, CREDITS.md corrected, developer build moved to
  docs/BUILDING.md; the PS5 package now carries the license files.
  The performance overlay is off by default on new installs (existing settings are kept), and the app has an
  original icon (tools/make_icon.py: a Mojave highway at dusk, no game artwork).
  The user's v34 log (44 min, mostly standing still) explains the stand-still slowdown: no leak (game RAM, D3D
  objects and VRAM flat throughout). The frame rate follows the scene's draw calls, which swing between ~800 and
  ~2,200 per frame on a ~54 s loop, almost certainly the game's idle camera (fVanityModeDelay) circling the player:
  ~1,000 draws holds 60 fps, ~2,200 drops to 45 with the main thread at 100%. The 6-minute pattern was that loop
  seen through one-minute samples. So busy views are bound by main-thread cost per draw call; that is the
  performance work to do. The overlay's per-core row is empty on the console (kern.cp_times is not available
  there). The menu fix held 16 quick presses in that session and delivered all of them.
- **v34, menu presses no longer lost:** the start menu (main and pause menu, and its own Yes/No prompts such as
  "Continue from your last saved game?") ignores a click or Back that comes within 500 ms of its last action (its
  click handler 0x7CE9B0 and back handler 0x7CF5E0 compare GetTickCount with 0x11DAAB4). Found by tracing
  InterfaceManager::Update down to the click: the path was identical for a lost and a taken press until that
  check. So ✕ pressed soon after leaving a page with ○, or Yes soon after Continue, had to be pressed again. The
  game's pause is kept; a ✕ or ○ that would be dropped is held back (rt/misc.c, menu_press_hold) and given to the
  game once the menu takes input again (the log says "menu: cross held N ms ..."). The long stand-still on the
  Primm save (27 min, CPU-only) kept the main thread at 53-71% with no upward trend, so the creep in the user's
  log is not in the game logic; it is more likely on the rendering side.
- **v33:** the Back prompt (Wait) is the touchpad (the PS5 keeps the Create button for itself; the port maps the
  touchpad click to Back); the temperature functions are imported directly (the run-time lookup fails on the console,
  as it did for the pad's trigger functions). The user's v28 log of a long stand-still showed the main thread rising
  from ~81% to 100% over 10-15 minutes with the scene unchanged (frames at ~1170 draws throughout), then dips to
  43-47 fps; being reproduced on the Primm save.
- **v32, prompts matched to a PS3 screenshot** (the barter menu, from the user): L1/R1 a rectangle about 4:3 with
  the name in bold filling it, a thinner outline (2.4); L2/R2 to match; the face symbols larger (about 55-60% of the
  arc's height, as on the PS3) and moved left to stay 2 pixels clear of the arc (its inner edge per cell, ARC in
  make_glyphs.py).
- **v31, prompts in the PS3 icon-set style:** after a PS3-style icon mod for Fallout 3 on PC (the user's screenshot):
  the face buttons keep the game's arc with the PlayStation symbol (as v30); the shoulders, triggers, sticks and
  Start/Back are drawn whole in thin outlines at the game's line weight: L1/R1 a flat rectangle, L2/R2 a taller
  rounded box, L3/R3 a stick with its letter in the cap, OPTIONS and CREATE in rectangles. Cell flag 2 marks a
  whole-cell redraw. Checked on Settings > Controls > Action Mapping.
- **v30, PS3-style prompts, per-core CPU, console test hook:**
  - **Prompts as the PS3 drew them.** Frames of PS3 gameplay (YouTube thumbnails) show the PS3 version's prompts in
    the same art as the Xbox ones the PC files carry, with the PlayStation symbols: "✕)" where the Xbox has "A)" (the
    arc is the side of a raised button). So the port now keeps the game's own icons and replaces only their letters
    (rt/glyphs.c): each cell is split into its solid pieces, the one with the largest extent (arc, bumper or trigger
    outline, stick, circle) is kept with any small piece above or below the middle band (a broken-off tip), the rest
    (the letters) is erased with every glow pixel nearer to it, and our symbol (tools/make_glyphs.py: ✕ ○ □ △, bold
    L1/R1, L2/R2, L3/R3 squeezed to the old letters' box, Options and Create marks) is laid in, with a halo like the
    game's own on highlighted cells (grey, about 0.62 x its opacity, as the originals). Mipmaps rebuilt from the
    result. Only letter boxes (coordinates) and our drawings are stored. iButtonPromptSize is gone (the game's own
    sizes).
  - **Per-core CPU in the overlay** (Detailed): a CORES block with each core's load as a bar and number
    (kern.cp_times on the PS5, /proc/stat on Linux; the log says if the PS5 refuses).
  - **FNV_CONSOLE="seconds:command;..."** runs console commands the way the console does (a temporary Script,
    CompileAndRun 0x5AC400 with the console manager's compiler), at the frame's input poll (inside Present a
    teleport deadlocks). Used to make a Strip test save from the Primm save: cow TheStripWorldNew 0 1 (the Lucky 38
    cell; the Strip cells read from the ESM: StripLucky38 0,1, StripGomorrah -1,1, StripTops -1,0, StripUltraLuxe
    0,-1, StripFSGate 0,2, the busiest 0,0), player.setlevel 30, 5000 caps, save StripTest.
- **v29, new button prompts:** redrawn in the style of the console's own monochrome prompts: each button a solid
  shape (disc for the face buttons and sticks, a low pill for L1/R1, a trigger shape for L2/R2, an upright pill for
  Options and Create) with its symbol or name cut out, so the menus' tint fills the button and the symbol reads
  clearly. Soft edges: the old hard-edged glyphs (from when they had to fit in the archive) showed steps when the
  game draws the 64-pixel cells large on a 4K picture. The highlighted versions' glow stays outside the button so
  the cut-out stays clear. The PC game files have no PS3 prompts to use: the PS3 build's interface files were not
  shipped on PC (only the Pip-Boy arm, terminal screens and a legal screen have PS3 copies).
- **v28, DualSense fixes:** v27's run-time lookup of the pad functions failed on the console ("trigger effects not
  available"), so no trigger effect was ever sent and turning Light Bar Health off could not hand the light bar
  back. scePadSetTriggerEffect and scePadResetLightBar are now imported directly (the pad library SDL already
  imports from), on the handle of the first logged-in user's controller (sceUserServiceGetLoginUserIdList and
  scePadGetHandle, as SDL opens it); if the reset is refused the light bar is set to the console's blue. Settings >
  Controls gets Trigger Strength (Light, Medium, Strong, Max; [fnv_recomp] iTriggerStrength, default Strong), and
  all strengths are higher. "360 Controller" and "Mouse Sensitivity" are taken off the Controls list (the controller
  is the only input); if the controller had been turned off there it is turned back on first.
- **v27, 4 GB of game memory, DualSense light bar and adaptive triggers:**
  - **Memory.** The exe is large-address-aware (on 64-bit Windows it gets 4 GB), but the port gave it 2 GB. Its
    address space now goes up to 0xEFFF0000, under the host-function thunks at 0xF0000000 (about 3.75 GB);
    GetSystemInfo and GlobalMemoryStatus say so. Allocations still go bottom-up, as on Windows. FNV_VM_HIGH=1 starts
    them at 2 GB to test high addresses: the Primm save loaded, played and saved, and two minutes of walking around
    Goodsprings ran clean, with every game, runtime and D3D object above 2 GB.
  - **The script-runner crash after loading Primm (pre-v21, about 1 load in 5)** did not happen in 15 loads (three of
    them with FNV_VM_HIGH); v21's thread wake-up rework most likely removed the race. Kept an eye on.
  - **DualSense** (rt/dualsense.c), on the main thread at Present, ~20 times a second, through the game's own
    functions: the player (0x11DEA3C), health (ActorValueOwner at +0xA4, virtual +0x08 and +0x1C, actor value 16),
    the equipped weapon and whether it is out (process at +0x68, virtual +0x148 and +0x454; weapon type at
    TESObjectWEAP +0xF4), menu mode (0x702360) and the player's cell (+0x40; none at the main menu).
    - Light bar: health, green through amber to a pulsing red below a third; New Vegas amber outside the world.
    - Adaptive triggers (scePadSetTriggerEffect, the layout the gta4-ps5 port uses, looked up at run time): with a
      gun out, R2 in weapon mode (gives way partway), firmer for rifles and launchers, even push for automatics and
      heavy weapons; a kick on each shot (the game's firing rumble while R2 is held); L2 a light push. Off for
      melee, unarmed, thrown, menus and the Pip-Boy. Checked: health 205/205 read on the Primm save, rifle (5) and
      pistol (3) recognised, out/away and the Pip-Boy seen, a shot with the Goodsprings pistol kicked the trigger.
    - Settings > Controls: Adaptive Triggers and Light Bar Health, on/off at once ([fnv_recomp] iAdaptiveTriggers,
      iLightBar).
- **v26, performance overlay and frame-rate cap, both in Settings > Display and applied at once:**
  - The overlay (rt/osd.c, drawn by hostd3d9.cpp over the finished frame) replaces DXVK's HUD, which could not be
    switched while running. Off, Basic (fps and frame time) or Detailed, in the style of MSI Afterburner: fps with
    the 1% low, frame time and a 240-frame graph with 30/60 lines, CPU (main thread, and the process in cores, CPU
    temperature on the PS5), GPU busy share from DXVK's GPU idle time and shaders being compiled (our DXVK patch
    exports dxvk_fnv_stats; SoC temperature on the PS5), video memory in use of the budget, the game's memory and
    what the console has left, resolution and draw calls. Drawn in one triangle list with its own font
    (tools/make_osd_font.py); the game's device state is captured in a state block and put back, with its render
    targets.
  - Frame Rate Cap 30 or 60 (osd.c): waits before Present until 1/cap s has passed. The game times each frame
    itself, so a 30 cap only means larger steps (Havok substeps at its usual 1/60 s, as on the consoles, which ran
    at 30); never above 60, where the engine's physics go wrong. Checked: from 42 fps in the menu to a steady 30.0.
  - rt/options.c (was resolution.c) holds the three port options as a table.
  - [fnv_recomp] iPerfOverlay (0-2, default 2) and iFrameCap (30/60, default 60) keep the choices.
- **v25, 4K and a Resolution option:** the game renders at iSize W x iSize H from FalloutPrefs.ini (read once at
  start-up); the PS5's TV output was already 4K (DXVK's PS5 display code picks the TV's 3840x2160 mode) with the
  game's 1280x720 image scaled up to it. Now:
  - 3840x2160 by default: set once by the settings upgrade (SETTINGS_VERSION 3, firstrun.c), and on a first run.
  - Settings > Display > Resolution (rt/resolution.c), next to Texture Size: 1280x720, 1920x1080, 2560x1440,
    3840x2160. Built like the game's own options (UserOption 0x30 bytes, constructor 0x7D6350, added to the options
    array 0x11DAB50 from the start menu's init hook); its callback sets the iSize settings (0x11C73DC, 0x11C718C),
    which the game saves, and raises the start menu's restart-required flag. Takes effect at the next start.
  - The game is told the screen is 3840x2160 (GetSystemMetrics, the adapter's display mode, two more modes).
  - GPU render targets and depth buffers no longer get a copy in game memory until the game locks one or reads it
    back (d3d_storage in d3d9.c): at 4K a screen-sized target is 33-66 MB of the game's 2 GB address space, and on
    the PS5 of real memory. Checked: 4K main menu and Goodsprings, a save and its thumbnail, screenshots.
  - The prompt size from v25's first half: iButtonPromptSize in [fnv_recomp] (50, 60, 70, 80, 100; 60 by default).
- **v24, smaller prompts:** the first drawings put the face symbols inside a ring about 42 px across, against the
  game's own letters at about 23x34 in the same 64 px cells, so they looked far too big. Face buttons are now the
  bare symbols (as on the PS3), and every glyph is drawn at 80% about the cell centre (SCALE in make_glyphs.py).
- **v23, Pip-Boy arm says L2/R2:** the arm model's texture has LT and RT printed on the two tags beside the tab
  buttons (textures\pipboy3000\pipboyarm01.dds; the Xbox copy is the same). When it is uploaded (at 1024, or 512
  with a lower texture setting) it is recognised by fingerprint, the old letters are erased and filled from the tag
  around them, and L2/R2 is painted in their colour; the first mip levels are rebuilt from the result. The game's own
  PS3 arm says L1/R1 (the PS3 had the tabs on the shoulder buttons), which would be wrong here: on the DualSense the
  tabs are on L2/R2. The platform-name table (0x1189038: PC, XBOX, PS3, PS3O) would switch every controller texture
  to the PS3 set if ever wanted.
- **To watch:** stutters in game; frame rate outdoors at the longer draw distances; dust and smoke clouds
  (player video: heavy orange sheets with hard edges, cause not found yet). The old "all threads 100%" figure came from a
  process clock that is not meaningful on PS5; v14 uses getrusage plus per-thread clocks.

## Roadmap (the player's goals, in order)

Where this list stands as of v35, without changing the goals. (1) Play on PS5 goes through the main menu, character creation, Goodsprings and the Mojave, with saving, loading and a clean exit. The v10 Mojave load stall named here was already past by the v13 notes. Crashes are still fixed as they show up, and the [To watch](#ps5-jailbroken-firmware-1342-progress) list is still open. (2) Busy views still drop under 60. 4K at 120 fps is still the long-term goal, and `rt/osd.c` still refuses a cap above 60. (3) Done in v25: the game renders at the `iSize` in `FalloutPrefs.ini`, 3840x2160 by default, with Settings > Display > Resolution. (4) Not started. Nothing in the tree calls a trophy service. (5) Prompts, adaptive triggers and the light bar are in (v22 through v35). The game's own vibration reaches the pad through `XInputSetState` and `plat_rumble`. A haptic effect separate from that vibration and from the adaptive triggers is not in `rt/dualsense.c`. (6) and (7) Not started. `rt/overlay.c` can lay an extra folder over the game files and is used for the built-in assets only. [Future plans](#future-plans) breaks up what is left. [docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md) lists the unknowns.

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

  Done, and left as written. DXVK 2.7.1 is built with `ps5/dxvk/dxvk-ps5.patch` against PS5_Mesa's RADV, presents on `VK_KHR_display`, and `ps5/build.sh` packages the home-screen app `PPSA97701`. The Linux software-Vulkan note above is still how a machine without a GPU can check the same backend.

  1. Build DXVK with the PS5 toolchain against RADV (PS5_Mesa).
  2. Give it a window through `VK_KHR_display`.
  3. Package as a home-screen app like ps5-plume-triangle.

## Future plans

These add to the roadmap above. They do not replace it. Each item says why it is here, what it depends on, and what done means. Paths are files in this repository. A line marked speculation is a guess about approach or impact. The code and the notes do not establish it.

### Near term

Reliable play, then the frame time. This is roadmap items 1 and 2, narrowed to what the v18–v35 notes and the code already point at.

1. **Main-thread time per draw.** v35's long session ties the frame rate to the draw count: about 1,000 draws holds 60 fps, about 2,200 drops to 45 with the main thread at 100%. That note reads the pattern as the idle camera (`fVanityModeDelay`) and names the main-thread cost per draw as the work. That cost is the game's own draw path plus `hb_draw` and `sync_state` in `rt/hostd3d9.cpp`. `be_draw` in `rt/d3d9.c` only counts. v18's slow-frame log is a different measurement: on those long frames, draws, uploads and Present were a few milliseconds, and the time was the game creating objects, with DXVK pipeline compiles counted inside the draw bucket (`hb_frame_end`). Depends on that breakdown and on the overlay's draw counter (`rt/osd.c`), so a 45 fps scene can be split into game time and draw time. Done when a repeat of that stand-still, same preset (VeryHigh, the resolution under test), keeps the 60 cap through the ~2,200-draw part of the loop. Speculation: fewer DXVK calls inside `sync_state` is the first place to look on the runtime side. Render state, samplers and texture stages are walked only when `D->dirty` says they changed. The texture and stream slots are still compared on every draw. A batching change in the game's draw loop is a different, larger change, and this item does not assume it.

2. **Shader-compile stutters, measured apart from the game's stalls.** The player page lists a stutter the first time an area or effect is seen, while shaders compile. v16 turned on eight compiler threads (`DXVK_CONFIG` `dxvk.numCompilerThreads` in `hb_init`) and the overlay reads compile counters from `dxvk_fnv_stats` in `ps5/dxvk/dxvk-ps5.patch`. v18 then found most long frames were game code. Depends on item 1's logging, because the two are easy to mix up. Done when one area is visited twice and the log or the overlay says whether the second visit still queues pipeline compiles. Speculation: a DXVK state cache on disk would only be worth doing if that second visit still compiles. Nothing in the tree sets a cache path today.

3. **Dust and smoke sheets.** The v20 note fixed particles flashing, by giving `D3DLOCK_DISCARD` buffers fresh GPU storage (`rt/hostd3d9.cpp`). The "To watch" line is a different symptom, from a player video: heavy orange sheets with hard edges, cause not found. Depends on a capture of the same spot (the v20 note used a Primm fire barrel, and `FNV_SHOT_AFTER` delays test screenshots). Done when a later note names the cause and the picture matches a capture from the PC game at the same place, or names a concrete difference in the port's path (the fixed-function movie path and the software rasterizer are the two special cases already in the tree: `hb_movie_overlay` in `rt/hostd3d9.cpp`, and `rt/d3d9_swr.inc`). Speculation about a blend or mipmap bug, before that capture, would be a guess.

4. **The one-shot script fault, only if it comes back.** v20 saw one guest null read in `RunScript` (`0x5E0D20`) after a Primm load. Four other loads of that save did not. v27's note says 15 loads after the v21 wake-up change, three of them with `FNV_VM_HIGH`, did not hit it. Depends on a new log, not on a change made blind. Done when either a new hit names the racing threads in the stall report (`rt/main.c`) and a fix keeps that save loading, or a later session record closes the watch.

5. **Write down the imports the game actually calls.** `rt/main.c` binds every missing import to `stub_missing`, which stops the process on the first call, and logs the count at startup. The count is not in the source, because the import list is generated into `gen/` from the player's executable. `FNV_TRACE` logs each unimplemented name. Depends on one Linux or PS5 boot with the game data. Done when a Goodsprings session's log is quoted here: the "not implemented yet" count, and either no `stub_missing` fatal or the name of each import that fired. That list is the queue for the mid-term API work below.

6. **PC saves, tried both ways.** The player page says saves are the PC format and that copying them across has not been tested widely. The port writes saves through the same Win32 file calls (`rt/kernel32.c`, gathered in 256 KB pieces since v20). Depends on a 1.4.0.525 save made with the same add-ons. Done when a PC save loads on the PS5 app and a save made by the port loads in the PC game, or the log shows the first point that rejects one of them.

7. **Startup text that disagrees with the code.** `DirectInput8Create` in `rt/dinput.c` always logs that the keyboard and mouse report no input. `dev_GetDeviceState` fills them from `plat_keys` and `plat_mouse`, and the Linux SDL backend does feed those (`rt/platform_sdl.c`). The PS5 backend initializes SDL for the controller and events only. Depends on nothing else. Done when the log line matches the backend that is running.

### Mid term

These are the rest of the player's list, plus gaps the code already contains that a full game does not appear to hit on the path played so far. They wait on the near-term measurements where the item says so.

1. **Mods in their own folder.** Roadmap item 6. `overlay_add` and `overlay_find` (`rt/overlay.c`) already replace or add files and merge directory listings, and `write_invalidation` writes `Data/ArchiveInvalidation.txt` the way the game reads it (`SInvalidationFile`, bare names, plus the game's own list if one exists). `overlay_init` only adds the built-in assets under `user/assets` (or `FNV_ASSETS_DIR`). Nothing scans `mods/<ModName>/`, nothing writes `plugins.txt`, and `rt/options.c` has no mod switch. The startup button that launches without mods is not read anywhere. Depends on the file layer above, on the Settings pattern in `rt/options.c` (a `UserOption` plus a `[fnv_recomp]` key, as the overlay and the frame cap already use), and on item 5 if a mod hits an unimplemented import. The path and format of `plugins.txt` are not recorded in this repository; that is an open question, not a format to invent. Done when a loose file under `mods/<ModName>/` replaces the game's copy, an enabled plugin is loaded by the game from a list the app wrote, the on/off choice survives in `[fnv_recomp]` and applies after a restart, and holding the start-up button (which button is not chosen yet) reaches the menu with those mods absent. The game files stay untouched. This repository does not gain any of the game's assets to test it.

2. **SEH and C++ unwinding.** `k_RaiseException` in `rt/kernel32.c` accepts the MSVC thread-name code (`0x406D1388`) and then stops the process. A C++ throw (`0xE06D7363`) stops with the type name. `k_RtlUnwind` stops. `SetUnhandledExceptionFilter` stores nothing. The played path (menu, character creation, Goodsprings, Mojave, save and load) has no note of that message, so this is a landmine for an untested quest or a mod, not the current crash queue. Depends on the recompiler: the original "Next" list already says catching functions need `setjmp`, and unwinding has to cross host frames. `tools/recomp.py` does not emit that. Done when a throw the game catches returns into that handler, with a log line naming it, and `RtlUnwind` returns. Until a real call site shows up in a log, a synthetic throw in a small guest function is enough to show the mechanism. Speculation: NVSE's own code may throw. Nothing in this repository shows that.

3. **Audio behaviour the mixer stores but does not use.** The base game has sound: DirectSound, WAV through mmio, Ogg voices (`rt/vorbis.c`, stb_vorbis), MP3 music (`rt/dshow.c`, dr_mp3), and the intro's first Bink audio track (`rt/bink.c`). The gaps, for anything past that:
   - `gains` in `rt/dsound.c` applies volume, pan, distance and the listener basis. `l_SetDopplerFactor` and the velocity setters store their values. `gains` never reads them.
   - The sound-cone methods on the 3D buffer have no function pointer, so `rt/com.c` returns success and does nothing.
   - `SetFX` and the other effect methods are the same kind of empty entry. `b_GetObjectInPath` returns `E_NOINTERFACE`.
   - A DirectShow stream that is not MP3 logs "music stays silent" and still returns success.
   - `bk_BinkOpen` plays audio track 0 only, and refuses an open by file handle.
   Depends on a `FNV_TRACE` session that shows the game, or a mod, actually calling these. Done when either a captured mix shows doppler pitch shift for a source whose velocity was set, or a note here says a playthrough's 3D buffers never set a non-zero velocity. Cones, effects, non-MP3 music and further Bink tracks stay as they are until a trace names a call.

4. **D3D9 results that do not come from the GPU.** `q_GetData` in `rt/d3d9.c` returns constants: an event query is done, an occlusion query reports 100000 pixels, timestamps are derived from the frame counter. `IDirect3DStateBlock9::Capture` and `Apply` have no function, so they succeed and do nothing. The overlay's own state block is a DXVK object inside `rt/hostd3d9.cpp`, which is a different path. `D3DXSaveTextureToFileA` returns success and writes nothing. `rt/d3dx.c` has the matrix helpers, `ID3DXBuffer` and the `vs_3_0` / `ps_3_0` profile strings, and no constant table and no shader compiler. `GetAvailableTextureMem` returns a fixed 1 GB. The game as played gets far enough that these may be unused. Depends on the trace from the near-term import item, plus a check of whether `GetData` and `Apply` run during a Mojave session (they are on the vtable, so they would not show up as missing imports). Done when that session's log says whether those calls happen. If they do, occlusion and state-block results come from the DXVK device the rest of the draw already uses. Speculation: a constant "visible" occlusion result would make the game draw more, not less. Nothing here shows that the game issues occlusion queries.

5. **Multisample, after the frame time allows it.** `rt/firstrun.c` applies the game's VeryHigh preset and then forces `iMultiSample` and `iWaterMultisamples` to 1 and `bTransparencyMultisampling` to 0, with the comment that this port does not render multisampling. `hb` creates render targets and depth surfaces with `D3DMULTISAMPLE_NONE` (`rt/hostd3d9.cpp`). Depends on item 1 in the near term: 4K is already the default, and the main thread is the limit the notes describe, so turning samples on before that cost is understood spends budget that is not measured. Done when a prefs value above 1 creates the host surfaces with that count and a screenshot of an edge differs from the same scene at 1. The settings upgrade must not overwrite a choice the player made afterwards (`SETTINGS_VERSION` in `rt/firstrun.c` is the existing mechanism).

6. **Trophies.** Roadmap item 4. The only mentions of trophies in the tree are that roadmap line and the player page's known issues. No function reads an achievement or calls the system trophy service. [RecompTrophy](https://github.com/cptlamarca/RecompTrophy) is the named library, GPL-3.0, used the way the roadmap says the gta4-ps5 port uses it. Depends on finding the game's unlock site in the recompiled code (fnv_decomp's names are the way the rest of this tree finds sites; no address is known here), on that library, and on whatever the jailbreak actually exposes for trophies. The last of those is not described in this repository. Done when one achievement the base game grants creates one trophy, and earning it again does not create a second. The mapping from the game's achievements to trophy ids is part of the work. It is not listed here, because it is not in the tree.

### Long term

1. **Frame rates above 60, then 4K at 120.** Roadmap item 2's long-term half. `osd_init` keeps `iFrameCap` at 30 or 60, and the comment in `rt/osd.c` says the game steps the world by the frame time while Havok substeps at `fMaxTime` (1/60 s), and that the engine's physics go wrong above 60. The PS5 display code in `ps5/dxvk/dxvk-ps5.patch` prefers a 3840x2160 mode and records that mode's refresh. It does not ask for 120 Hz. Depends on finding the engine sites the `rt/osd.c` comment is describing. Those sites are not named in this repository. Also depends on the TV mode list RADV returns; the patch logs the mode it chose, and no such log is stored here. Done when the cap can be raised, a timed walk matches the distance of a 60 fps run of the same input closely enough that the note can state the tolerance, physics stays stable for that run, and a TV that offers 120 Hz shows that refresh in the WSI log. Speculation about which globals to change, before those sites are found, does not belong in a patch.

2. **NVSE, rebuilt into the port.** Roadmap item 7. The hook list in `tools/recomp.py` (`HOOKS`, five functions, implemented in `rt/hooks.c`) is the mechanism the roadmap points at, and the comment there says to keep the list short. `k_LoadLibraryA` in `rt/kernel32.c` only returns a handle for a name already in `g_mod_names`. It does not map a DLL from disk, and `GetProcAddress` only resolves the host APIs `api_lookup` knows. A plugin binary built for Windows cannot be dropped into the mods folder and run. The roadmap already says the open-source extender and open-source plugins are rebuilt in, and that closed-source plugins cannot be supported. Depends on an inventory of xNVSE's patches against 1.4.0.525 (which of them are hooks, which need the recompiler), on the mods folder for loose files, and possibly on SEH if a rebuilt plugin throws (speculation: this tree does not show that xNVSE throws). Done when one script function the base game does not have runs from `FNV_CONSOLE` (the console hook in `rt/dualsense.c`), and a closed-source plugin is refused with a log line. JIP LN is the example the roadmap already gives. It is a later step, after the extender itself runs.

3. **The PS5 DXVK patch, kept buildable.** `ps5/README.md` pins DXVK 2.7.1 and `ps5/build.sh` links `build.ps5/src/d3d9/libdxvk_d3d9.a` plus every static library in that build. The patch adds the PS5 WSI, static d3d9, `dxvk_fnv_stats`, and the Vulkan loader shortcut. A newer DXVK will not apply that patch unchanged. Depends on nothing in the game. It matters when a bug fix or a device feature is only in a later DXVK. Done when either the patch applies to a named newer tag and the PS5 app still reaches the menu, or a note here says why 2.7.1 stays. Speculation: later DXVK shader-compiler changes might move the stutter work. That is not a reason to upgrade on its own.

4. **Guest memory pressure, as a limit rather than a bug.** The guest window is 4 GiB reserved and about 3.75 GiB usable, up to `0xEFFF0000`, under the thunks at `0xF0000000` (`rt/mem.c`, `rt/rt.h`). On PS5 those commits are direct memory, because the flexible budget is too small (`rt/mem.c`). Running out calls `exit(2)`. v16's session sat around 1.0–1.1 GB of game RAM with 331 MB of flexible memory free, which is a different pool. Depends on the perf line (`game RAM` is `g_guest_backed`). Done when a long session, all add-ons, 4K, records the high-water mark, and the log distinguishes "close to `vm_top`" from "direct memory refused". No change is called for until a session approaches either limit. `FNV_VM_HIGH` remains the test that addresses above 2 GB work.
