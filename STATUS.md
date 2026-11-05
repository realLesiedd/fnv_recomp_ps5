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

- **Tested on hardware** (app PPSA97701, built by `ps5/build.sh`; see `ps5/README.md`):
  - The app launches and does first-run setup.
  - DXVK on RADV drives the TV at 3840x2160 @ 59 Hz.
  - The main menu runs at 59.9 fps (DXVK HUD on) and the DualSense navigates it.
- **New Game.** v4 crashed to the home screen (CE-108255-1) because the intro movie called the unimplemented
  `BinkOpen`. Movies are now skipped (`rt/misc.c`), and the MMX code that runs right after them is recompiled.
  - On Linux, New Game now reaches the opening scene in Doc Mitchell's house (3D world, wake-up blur, DLC item
    notices).
  - On PS5 (v5): waiting on the test.
- **Continue/Load showing with no saves is vanilla.** The menu dims them when `Saves\*.fos` finds nothing, then
  `0x7CFD20` (called when the menu finishes fading in) sets both back to `_enabled = 1`, so the original exe shows them
  too.
- **Main menu: Continue is left off until a save exists.** This is a deliberate change from the original exe
  (`rt/hooks.c`), at the player's request.
- **Audio** (tested on Linux, recorded with SDL's disk driver):
  - DirectSound 8 mixer (`rt/dsound.c`): static, streaming, duplicate and 3D buffers, the listener and position
    notifications. Output goes through SDL: PulseAudio/ALSA on Linux, `sceAudioOut` on PS5.
  - `.wav` effects and UI sounds: WINMM mmio (`rt/mmio.c`) driving the game's own BSA reader.
  - `.ogg` voices: libvorbisfile calls over stb_vorbis (`rt/vorbis.c`).
  - `.mp3` music: DirectShow (`rt/dshow.c`) reads the track through the game's source filter and decodes it with
    dr_mp3.
- Not yet: Bink video playback (intro movies are skipped).

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
