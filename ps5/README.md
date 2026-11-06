# PS5 build (jailbroken console)

The app is `PPSA97701`, "Fallout: New Vegas (recomp)". It combines four parts:
- the recompiled game and this runtime;
- DXVK, for Direct3D 9 on Vulkan;
- PS5_Mesa's RADV, the GPU driver;
- the PS5 SDL2 port, for the DualSense and audio.

## Build (Ubuntu 24.04)

1. Build Nazky's toolchain once:
   `git clone https://github.com/Nazky/ps5-plume-triangle && make -C ps5-plume-triangle`.
   It needs `rsync`, `glslang-tools`, `python3-mako` and Meson 1.4 or newer (`pip install meson`).
2. Build DXVK 2.7.1 with the PS5 patch:

       git clone --branch v2.7.1 --recurse-submodules https://github.com/doitsujin/dxvk
       cd dxvk && git apply ../fnv_recomp/ps5/dxvk/dxvk-ps5.patch
       printf "[constants]\nsdk = '%s'\n" "$PWD/../ps5-plume-triangle/.deps/native/ps5-payload-sdk" > sdk.ini
       meson setup build.ps5 --cross-file sdk.ini --cross-file ../fnv_recomp/ps5/dxvk/ps5-cross.ini \
         --buildtype=release -Dnative_ps5=true -Dnative_sdl2=disabled -Dnative_sdl3=disabled -Dnative_glfw=disabled \
         -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false
       ninja -C build.ps5

3. Generate `gen/` from your own exe (see [docs/BUILDING.md](../docs/BUILDING.md)), then run
   `ps5/build.sh <ps5-plume-triangle> <dxvk>`.

## Install

1. Copy `build-ps5/PPSA97701` to `/data/homebrew/PPSA97701` on the PS5.
2. Copy your New Vegas folder into `/data/homebrew/PPSA97701/game/`. It needs `FalloutNV.exe` (1.4.0.525, the
   DRM-free GOG executable; the app checks it at start-up), `Data/`, `Fallout_default.ini` and the preset INIs.
3. Launch the app. Settings and saves go to `user/`, and the log to `fnv.log` (DXVK writes its own log there too).

The app folder also gets `LICENSE`, `THIRD_PARTY_NOTICES.md`, `LICENSES/` and `README.md`, so a shared build carries
its license notices.
