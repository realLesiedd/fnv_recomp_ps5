# Credits

fnv_recomp is built on the work of these projects and people. Thank you.

Licenses for everything that ships in the PS5 app are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## The game
- **Fallout: New Vegas** is by Obsidian Entertainment and Bethesda Softworks (ZeniMax Media, a Microsoft company).
  This repository contains no game code or data. The app is built from code generated from the game's executable
  and runs only with the player's own copy of the game.

## Reverse engineering
- **[fnv_decomp](https://github.com/momusdigital/fnv_decomp)** (momusdigital and the fnvdecomp.com community).
  Its matching decompilation supplies the 68,657 function addresses that seed the recompiler and the fingerprints
  that identify the right executable. Its function names help read crash reports. The recompiler reads these from a
  local checkout while generating code. Nothing from fnv_decomp is copied into this repository, and fnv_decomp
  grants no licence for its contents.

## Static recompilation
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** (Mr-Wiseguy), **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)**
  and **[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp)** (hedge-dev): the recompilation approach
  this project follows.
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**, **[Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp)**
  (OZORDI), **[LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid)** (vaduur) and
  **[gta4-ps5](https://github.com/elripalda/gta4-ps5)** (elripalda). The GTA IV PS5 port showed how a recompiled
  game runs on a jailbroken PS5: guest memory in direct memory, Vulkan on RADV, `VK_KHR_display` output and app
  packaging. Its PS5 input driver documents the console's adaptive-trigger layout (after ProsperoEden's
  `ps5_pad.hpp`), which `rt/platform_sdl.c` follows. No code is copied from it.

## PS5 homebrew
- **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)** (John Törnblom and contributors): the payload
  SDK and toolchain.
- **[ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL)** and **[pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)**:
  the PS5 SDL2 port (video output, DualSense, audio) and the prebuilt libraries. The platform layer follows its
  `sceVideoOut` usage.
- **[PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)**, **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** and
  **[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)** (mihawk-99): RADV on the PS5's GPU, the link
  recipe, the `libc.prx` builder, the signer and libps5platform's direct-memory heap.
- **[ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle)**, **[plume-ps5](https://github.com/Nazky/plume-ps5)**
  and **[UnleashedRecomp-Prospero](https://github.com/Nazky/UnleashedRecomp-Prospero)** (Nazky), with
  **[plume](https://github.com/renderbag/plume)** (Darío Samo): the PS5 Vulkan app setup and packaging. The app's
  start-up and C++ runtime (`app_crt.cpp`, `app_cpp_runtime.cpp`) are by BlackBearReloaded.

## Libraries
- **[DXVK](https://github.com/doitsujin/dxvk)** (Philip Rebohle and contributors): Direct3D 9 on Vulkan, the renderer.
  `rt/hostd3d9.cpp` drives its native d3d9 build. `ps5/dxvk/dxvk-ps5.patch` adds a PS5 display backend and
  `dxvk_fnv_stats` (GPU idle time and shader-compile counters for the performance overlay).
- **[SDL2](https://libsdl.org)** (Sam Lantinga and contributors).
- **[FFmpeg](https://ffmpeg.org/)** (the FFmpeg developers): its Bink video and audio decoders play the intro movie
  behind the binkw32 calls (`rt/bink.c`, which also parses the Bink container).
- **[stb_vorbis](https://github.com/nothings/stb)** (Sean Barrett and contributors): Ogg Vorbis voices and effects
  behind the libvorbisfile calls (`rt/vorbis.c`). Bundled unmodified in `third_party/`.
- **[dr_mp3](https://github.com/mackron/dr_libs)** (David Reid, based on [minimp3](https://github.com/lieff/minimp3)
  by lieff): the MP3 music behind DirectShow (`rt/dshow.c`). Bundled unmodified in `third_party/`.
- **[GNU libiconv](https://www.gnu.org/software/libiconv/)**, and LLVM's libc++, libc++abi and libunwind, linked
  into the app by SDL2 and RADV.
- **[Capstone](https://www.capstone-engine.org/)** and **[pefile](https://github.com/erocarrera/pefile)**: x86
  disassembly and PE parsing in the recompiler.

## Art and fonts
- **[DejaVu Fonts](https://dejavu-fonts.github.io/)**: DejaVu Sans Bold letters the L1/R1/L2/R2/L3/R3 prompts
  drawn by `tools/make_glyphs.py`, and DejaVu Sans Mono Bold is the performance overlay's font
  (`tools/make_osd_font.py`).
- The app icon (`ps5/sce_sys/icon0.png`, drawn by `tools/make_icon.py`) is original artwork made for this project,
  lettered in DejaVu Sans Bold. It uses no artwork or logo from the game.
- The PlayStation-style button prompts in `assets/` are original drawings made for this project in the style of the
  PS3 version's prompts. They are not Sony artwork.

## References
- **[Wine](https://www.winehq.org/)**: its documentation of DirectSound and WINMM mmio behaviour was the reference
  for corner cases in `rt/dsound.c` and `rt/mmio.c`. No Wine code is used.

Each project keeps its own license. Code taken or adapted from another project carries that project's notice where
it is used.
