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
