# Third-party notices

fnv_recomp's own source is licensed under the GNU General Public License, version 3 or later (see `LICENSE`).
The components below keep their own licenses. This file lists every one that is part of the PS5 app
(`eboot.bin` and its folder), or bundled in this repository, with its license and the notice it asks to be kept.

None of these licenses, nor this project's, cover Fallout: New Vegas. The game, its code, artwork, audio and
data belong to their owners. No game files are included anywhere in this project.

## Part of the PS5 app

| Component | Version | Used for | License | Source |
|---|---|---|---|---|
| PS5 payload SDK (John Törnblom and contributors) | ps5-payload-dev/sdk | C library, system library stubs, toolchain | GPL-3.0-or-later | https://github.com/ps5-payload-dev/sdk |
| App start-up and C++ runtime, `app_crt.cpp` and `app_cpp_runtime.cpp` (BlackBearReloaded), with Nazky's ps5-plume-triangle | ps5-plume-triangle | Starting the app, C++ support | GPL-3.0-or-later | https://github.com/Nazky/ps5-plume-triangle |
| PS5_PayloadSDK (mihawk-99): `libps5platform`, `libc.prx` | as used by ps5-plume-triangle | Direct-memory heap, threads, the app's C library module | GPL-3.0 | https://github.com/mihawk-99/PS5_PayloadSDK |
| Mesa RADV, PS5 port (PS5_Mesa, mihawk-99; the Mesa authors) | as used by ps5-plume-triangle | Vulkan driver for the PS5 GPU | MIT (core Mesa; see Mesa's `docs/license.rst`) | https://github.com/mihawk-99/PS5_Mesa |
| LLVM libc++, libc++abi, libunwind, compiler-rt builtins | from the payload SDK | C++ standard library for RADV | Apache-2.0 WITH LLVM-exception | https://llvm.org |
| DXVK (Philip Rebohle and contributors) | 2.7.1, with `ps5/dxvk/dxvk-ps5.patch` | Direct3D 9 on Vulkan | zlib | https://github.com/doitsujin/dxvk |
| SDL2, PS5 port (Sam Lantinga and contributors; ps5-payload-dev) | PacBrew package | Video output, DualSense, audio | zlib | https://github.com/ps5-payload-dev/SDL |
| FFmpeg: libavcodec, libavutil (the FFmpeg developers) | 7.0.1, PacBrew package built with `--enable-gpl --enable-version3` | Bink video and audio for the intro movie | GPL-3.0-or-later (as built) | https://ffmpeg.org/releases/ffmpeg-7.0.1.tar.xz |
| GNU libiconv | 1.17, PacBrew package | Character set conversion, linked by SDL2 | LGPL-2.1-or-later | https://www.gnu.org/software/libiconv/ |
| stb_vorbis (Sean Barrett and contributors) | 1.22, in `third_party/` | Ogg Vorbis decoding | Public domain or MIT | https://github.com/nothings/stb |
| dr_mp3 (David Reid; based on minimp3 by lieff) | in `third_party/` | MP3 decoding | Public domain (Unlicense) or MIT-0 | https://github.com/mackron/dr_libs |
| DejaVu Sans Bold and DejaVu Sans Mono Bold (Bitstream, Inc.; DejaVu changes public domain) | fonts-dejavu-core | Rendered into the overlay font (`assets/internal/osd_font.a8`) and the L1/R1/L2/R2/L3/R3 button labels | Bitstream Vera license, see `LICENSES/Bitstream-Vera.txt` | https://dejavu-fonts.github.io/ |

The packages from PacBrew are built by the recipes at https://github.com/ps5-payload-dev/pacbrew-repo.

**Getting the source.** The complete source of this project, including the build script that links the app
(`ps5/build.sh`) and the DXVK changes (`ps5/dxvk/dxvk-ps5.patch`), is in this repository. The source of each
component above is at the address given. For the LGPL and GPL parts you may rebuild the app against your own
modified versions with `ps5/build.sh`.

## Used only to build

These run on the build machine and are not part of the app: Capstone (BSD-3-Clause) and pefile (MIT) in the
recompiler; Pillow (HPND) in the asset tools; clang/LLVM, CMake, Ninja and Meson.

[fnv_decomp](https://github.com/momusdigital/fnv_decomp) is also used only on the build machine: the recompiler
reads its function list and executable fingerprints from the player's own checkout while generating code. No part
of fnv_decomp is included in this repository. fnv_decomp grants no licence for its contents.

## License texts

- GPL-3.0: `LICENSE`.
- LGPL-2.1: `LICENSES/LGPL-2.1.txt`.
- Bitstream Vera (DejaVu fonts): `LICENSES/Bitstream-Vera.txt`.
- Apache-2.0 WITH LLVM-exception: https://llvm.org/LICENSE.txt

### MIT (Mesa)

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

### zlib (DXVK, SDL2)

DXVK: Copyright (c) 2017 Philip Rebohle, (c) 2019 Joshua Ashton, (c) 2019 Robin Kertels, (c) 2023 Jeffrey Ellison.
SDL2: Copyright (C) 1997-2025 Sam Lantinga.

    This software is provided 'as-is', without any express or implied
    warranty. In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.

DXVK as built here is altered: the changes are in `ps5/dxvk/dxvk-ps5.patch`.
