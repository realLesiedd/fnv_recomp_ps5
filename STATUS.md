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
