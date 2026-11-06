# Credits

fnv_recomp stands on the work of these projects and people. Thank you.

## The game
- **Fallout: New Vegas** is by Obsidian Entertainment and Bethesda Softworks (ZeniMax/Microsoft). This project
  contains no game code or data. Players use their own copy.

## Reverse engineering
- **[fnv_decomp](https://github.com/momusdigital/fnv_decomp)** (momusdigital and the fnvdecomp.com community).
  Its matching decompilation supplies the 68,657 function addresses that seed the recompiler, the fingerprints that
  identify the right executable, and the names used to read crash reports.

## Static recompilation, the idea
- **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** and **[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp)**
  (hedge-dev), **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** (Mr-Wiseguy): the "recomp" approach this follows.
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**, **[Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp)**
  (OZORDI), **[LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid)** (vaduur) and
  **[gta4-ps5](https://github.com/elripalda/gta4-ps5)** (elripalda). Their PS5 port showed how a recompiled game lives
  on a jailbroken PS5: guest memory in direct memory, Vulkan on RADV, `VK_KHR_display` output, app packaging.

## PS5 homebrew
- **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)**: John Törnblom and contributors. The payload SDK and toolchain.
- **[ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL)** and **[pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)**:
  the PS5 SDL2 port (video out, DualSense, audio) and prebuilt libraries. The platform layer here follows its
  `sceVideoOut` usage.
- **[PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)**, **[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)**,
  **[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)** (mihawk-99): RADV on the PS5's GPU, the link recipe, the
  `libc.prx` builder, the signer and libps5platform's direct-memory heap.
- **[ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle)**, **[plume-ps5](https://github.com/Nazky/plume-ps5)** and
  **[UnleashedRecomp-Prospero](https://github.com/Nazky/UnleashedRecomp-Prospero)** (Nazky), with **[plume](https://github.com/renderbag/plume)**
  (Darío Samo): the PS5 Vulkan app setup and packaging.

## Libraries and tools
- **[Capstone](https://www.capstone-engine.org/)** (x86 disassembly in the recompiler), **[pefile](https://github.com/erocarrera/pefile)**.
- **[SDL2](https://libsdl.org)** (Sam Lantinga and contributors).
- **[DXVK](https://github.com/doitsujin/dxvk)** (Philip Rebohle and contributors, zlib license): Direct3D 9 on Vulkan, the GPU renderer (`rt/hostd3d9.cpp` drives its native d3d9 build). `ps5/dxvk/dxvk-ps5.patch` adds a PS5 display
  backend and `dxvk_fnv_stats` (GPU idle time and shader-compile counters, for the performance overlay).
- **[SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD)** for downloading the player's own copy.
- **[stb_vorbis](https://github.com/nothings/stb)** (Sean Barrett and contributors, public domain or MIT): decodes the
  game's Ogg Vorbis voices and effects behind the libvorbisfile calls (`rt/vorbis.c`). Bundled unmodified in
  `third_party/`.
- **[dr_mp3](https://github.com/mackron/dr_libs)** (David Reid, public domain or MIT-0; based on
  [minimp3](https://github.com/lieff/minimp3) by lieff): decodes the MP3 music behind DirectShow (`rt/dshow.c`).
  Bundled unmodified in `third_party/`.
- **[FFmpeg](https://ffmpeg.org/)** (the FFmpeg developers, LGPL 2.1 or later): its Bink video and Bink audio
  decoders play the intro movie behind the binkw32 calls (`rt/bink.c`). Linked as libraries: on PS5 the static
  libavcodec/libavutil from the ps5-payload-dev PacBrew packages, on Linux the system's shared libraries. The
  Bink container is parsed in `rt/bink.c`. Under the LGPL you may relink the app against your own FFmpeg build:
  the full source of everything else is in this repository and `ps5/build.sh` does the link.
- **[DejaVu Fonts](https://dejavu-fonts.github.io/)** (Bitstream Vera license): DejaVu Sans Bold letters the
  L1/R1/L2/R2/L3/R3 glyphs drawn by `tools/make_glyphs.py`, and DejaVu Sans Mono Bold is the performance overlay's
  font (`tools/make_osd_font.py`). The PlayStation-style button prompts (`assets/`) are original drawings made for
  this project, not Sony artwork.
- **[Wine](https://www.winehq.org/)** (LGPL): its documentation of DirectSound and WINMM mmio behaviour was the
  reference for corner cases in `rt/dsound.c` and `rt/mmio.c` (no Wine code is used).

Each project keeps its own license. Code taken or adapted from another project carries that project's notice where
it is used.
