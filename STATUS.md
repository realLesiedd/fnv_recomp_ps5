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
- **Where it stops.** The game asks d3d9.dll for Direct3DCreate9, gets nothing, and shows its own
  "Failed to initialize renderer" message box, then exits normally (exit code 0).

## Next, in order

1. **Direct3D 9.** Implement IDirect3D9 and IDirect3DDevice9 (about 120 methods) over a native backend.
   Either translate to Vulkan or OpenGL, or marshal to dxvk-native's d3d9, which already speaks D3D9 on Linux.
   d3dx9_38 needs shader assembly/compilation and texture loading: 25 imports.
2. **Game data.** Booting to the main menu needs the player's `Data/` folder: the .esm, the BSAs and the INIs.
   The test container only has the exe, so it has never gotten this far here.
3. **Structured exception handling and C++ throw.** These currently stop with a clear message. They need SEH
   chain dispatch, plus unwinding across host frames (setjmp in functions that catch).
4. **A windowing and input backend** (SDL) in place of the headless USER32. Then audio (DirectSound buffers),
   Bink video (or skipping the intro videos) and libvorbisfile (host libvorbis).
5. **Console target.** Only the runtime (`rt/`) is platform-specific. The generated code is portable C, apart from
   the fixed 4 GiB guest window, which needs a large reserved virtual range.
