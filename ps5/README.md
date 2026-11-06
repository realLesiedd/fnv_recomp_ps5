# PS5 build (jailbroken console)

The app is `PPSA97701`, "Fallout: New Vegas (recomp)". It combines four parts:
- the recompiled game and this runtime;
- DXVK, for Direct3D 9 on Vulkan;
- PS5_Mesa's RADV, the GPU driver;
- the PS5 SDL2 port, for the DualSense and audio.

## Build (Ubuntu 24.04)

Run these from this repository. `ps5/build.sh` looks in `../ext/` when you pass no directories.

1. Build Nazky's toolchain once, at `d54d9ad`:

       git clone https://github.com/Nazky/ps5-plume-triangle ../ext/ps5-plume-triangle
       git -C ../ext/ps5-plume-triangle checkout d54d9ad
       make -C ../ext/ps5-plume-triangle

   `make` builds the sample app. On a fresh checkout `tools/build.sh` runs `tools/build-radv.sh`, which
   compiles RADV from PS5_Mesa source. `mesa_revision` in that script is
   `0b2d6d1a61d9bbf89cf8beb88a696144f67c61f8`. The script's Ubuntu list is `rsync`, `glslang-tools`,
   `meson`, `ninja-build`, `bison`, `flex`, `python3-mako`, `python3-ply` and `python3-yaml`. These notes
   also ask for Meson 1.4 or newer. The script only checks that `meson` is on the path.
2. Build DXVK 2.7.1 with the PS5 patch:

       git clone --branch v2.7.1 --recurse-submodules https://github.com/doitsujin/dxvk ../ext/dxvk
       git -C ../ext/dxvk apply "$PWD/ps5/dxvk/dxvk-ps5.patch"
       printf "[constants]\nsdk = '%s'\n" "$PWD/../ext/ps5-plume-triangle/.deps/native/ps5-payload-sdk" > ../ext/dxvk/sdk.ini
       meson setup --cross-file "$PWD/../ext/dxvk/sdk.ini" --cross-file "$PWD/ps5/dxvk/ps5-cross.ini" \
         --buildtype=release -Dnative_ps5=true -Dnative_sdl2=disabled -Dnative_sdl3=disabled -Dnative_glfw=disabled \
         -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
         ../ext/dxvk/build.ps5 ../ext/dxvk
       ninja -C ../ext/dxvk/build.ps5

3. Generate `gen/` from your own exe (see [docs/BUILDING.md](../docs/BUILDING.md)), then run `ps5/build.sh`.
   `tools/package_release.sh` zips `build-ps5/PPSA97701` as `PPSA97701-<FNV_RELEASE>.zip`. It will not pack a
   `game/` folder that holds more than the placeholder note.

## Install

1. Copy `build-ps5/PPSA97701` to `/data/homebrew/PPSA97701` on the PS5.
2. Copy your New Vegas folder into `/data/homebrew/PPSA97701/game/`. It needs `FalloutNV.exe` (1.4.0.525, the
   DRM-free GOG executable; the app checks it at start-up), `Data/`, `Fallout_default.ini` and the preset INIs.
3. Launch the app. Settings and saves go to `user/`, and the log to `fnv.log`. DXVK writes `_d3d9.log` in the
   same folder (`rt/hostd3d9.cpp` sets `DXVK_LOG_PATH` to the app folder).

The app folder also gets `LICENSE`, `THIRD_PARTY_NOTICES.md`, `LICENSES/` and `README.md`, so a shared build carries
its license notices.
