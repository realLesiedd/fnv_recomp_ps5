# fnv_recomp

**Fallout: New Vegas, recompiled to run natively on the PlayStation 5.**

fnv_recomp is an unofficial port of the PC version of Fallout: New Vegas to jailbroken PS5 consoles. It is made by
static recompilation: the game's own executable is translated, function by function, from x86 machine code into C
ahead of time, then compiled for the PS5. Nothing is emulated. The game runs natively, with its original data
files.

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of Fallout: New Vegas for PC,
> version 1.4.0.525. The app does nothing without it.

> [!NOTE]
> **Not released yet.** The port is in testing and there is no public download at the moment. This page describes
> how it will be installed and used.

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Controls](#controls)
- [Settings](#settings)
- [Known issues](#known-issues)
- [FAQ](#faq)
- [Building from source](#building-from-source)
- [Credits](#credits)
- [License and legal](#license-and-legal)

## Features

- **Native 4K.** Renders at 3840×2160 by default, with a Resolution option for 1080p, 1440p and 720p.
- **Frame Rate Cap** of 30 or 60 FPS. It applies straight away and leaves the game's physics alone.
- **Performance overlay** with three levels (off, basic, detailed). The detailed view has a frame-time graph,
  1% lows, CPU load, GPU load, memory use and temperatures. Per-core load is included when the system reports it.
  On PS5 that source (`kern.cp_times`) is not available, so the row is left out.
- **DualSense support.**
  - PlayStation button prompts throughout the menus, HUD and Pip-Boy, drawn in the style of the PS3 version.
  - Adaptive triggers that react to the equipped weapon, with four strength levels.
  - Light bar that shows your health.
- **The full 4 GB of memory** the PC executable can address.
- **The whole game.** Intro movie, music, voices, saving and loading, and all six add-ons when you have them.
- **Tidy files.** Settings, saves and the log stay in the app's own folder.

## Requirements

| | Version |
|---|---|
| fnv_recomp | v35 (testing build) |
| Fallout: New Vegas for PC | 1.4.0.525 |
| PS5 system software | Tested on 13.42 |

### Console

- A jailbroken PS5 that can run homebrew apps from `/data/homebrew/`.
- About 10 GB of free space: roughly 9.5 GB for the game with every add-on, plus the app.
- A DualSense controller.

### Game files

Copy these from your own PC installation of Fallout: New Vegas 1.4.0.525:

| File | Where it comes from | Notes |
|---|---|---|
| `FalloutNV.exe` | The GOG release | The DRM-free 1.4.0.525 executable (16,084,808 bytes). |
| `Data/` | Your installation (GOG or Steam) | The base game and any add-ons you own. Tested with all six add-ons (Ultimate Edition). |
| `Fallout_default.ini`, `low.ini`, `medium.ini`, `high.ini`, `VeryHigh.ini` | Next to the executable | The game's default settings. |

These won't work:

- **Steam's `FalloutNV.exe`.** Steam ships it encrypted, so it can't be used. Your Steam copy's `Data/` folder is
  fine; only the executable has to come from GOG.
- **Any other version** of the executable, or one whose code was changed by a patcher.

The app checks the executable when it starts. If it's the wrong one, the app closes and `fnv.log` says why.

## Installation

1. Download `PPSA97701.zip` from the Releases page (not available yet). Unzip it and copy the `PPSA97701` folder to
   `/data/homebrew/` on your PS5, for example over FTP.
2. Copy your game files into the app's `game` folder:

   ```
   /data/homebrew/PPSA97701/game/FalloutNV.exe
   /data/homebrew/PPSA97701/game/Fallout_default.ini
   /data/homebrew/PPSA97701/game/low.ini, medium.ini, high.ini, VeryHigh.ini
   /data/homebrew/PPSA97701/game/Data/
   ```

3. Start **Fallout: New Vegas (recomp)** from the home screen.

The first start sets the game up for the PS5: the Very High preset at 4K, with a controller. To leave the game, use
Quit in the main menu; it returns to the home screen.

| What | Where |
|---|---|
| Saves | `/data/homebrew/PPSA97701/user/Documents/My Games/FalloutNV/Saves/` |
| Settings | `/data/homebrew/PPSA97701/user/Documents/My Games/FalloutNV/` |
| Log | `/data/homebrew/PPSA97701/fnv.log` (include it with any bug report) |

### Updating

Copy the new `PPSA97701` folder over the old one. Your `game` and `user` folders are kept.

## Controls

The game uses its console controls, with PlayStation buttons in place of the Xbox ones:

| Xbox 360 | DualSense |
|---|---|
| A, B, X, Y | ✕, ○, □, △ |
| LB, RB | L1, R1 |
| LT, RT | L2, R2 |
| Left and right stick click | L3, R3 |
| Start (pause menu) | Options |
| Back (Wait) | Touchpad click |

## Settings

The port adds these to the game's Settings menu:

| Menu | Option | Choices |
|---|---|---|
| Display | Resolution | 1280×720, 1920×1080, 2560×1440, 3840×2160 (applies after a restart) |
| Display | Frame Rate Cap | 30 FPS, 60 FPS |
| Display | Performance Overlay | Off, Basic, Detailed |
| Controls | Adaptive Triggers | Off, On |
| Controls | Trigger Strength | Light, Medium, Strong, Max |
| Controls | Light Bar Health | Off, On |

The PC-only options 360 Controller and Mouse Sensitivity are removed.

## Known issues

- The frame rate can drop below 60 in busy areas.
- Stutters can happen the first time an area or effect is seen, while its shaders are compiled.
- After standing still for a long time the frame rate can dip. A long session in [STATUS.md](STATUS.md) (v35)
  ties that dip to the draw count, which that note reads as the idle camera. Memory stays flat.
- Trophies, a mods folder and NVSE plugins are not supported yet.

Progress notes for each version are in [STATUS.md](STATUS.md).

## FAQ

**Is this emulation?**
No. The game's code has been converted to native PS5 code ahead of time. The PS5 runs it directly.

**Why does it need the GOG executable?**
The converted code matches one exact executable, 1.4.0.525. GOG's copy can be read as it is. Steam's is encrypted
on disk.

**Can I use my PC saves?**
Saves use the PC format. Copy them into the Saves folder above. Saves made with the same add-ons should load, but
this has not been tested widely.

**Can I use mods?**
Not yet. A mods folder with an on/off switch, and a safe mode, are planned.

**Where do I report a bug?**
Open an issue with what happened, what you were doing, and your `fnv.log`.

## Building from source

Building needs your own `FalloutNV.exe`, because the recompiler generates the game's C code from it. See
[docs/BUILDING.md](docs/BUILDING.md) for the recompiler and the Linux build, and [ps5/README.md](ps5/README.md) for
the PS5 app. What is left to build, and the questions still open, are in [STATUS.md](STATUS.md#future-plans) and
[docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md).

## Credits

fnv_recomp builds on many projects. Among them:
- [fnv_decomp](https://github.com/momusdigital/fnv_decomp), whose decompilation guides the recompiler.
- The recompilation work of [N64Recomp](https://github.com/N64Recomp/N64Recomp),
  [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [ReXGlue](https://github.com/rexglue/rexglue-sdk).
- [gta4-ps5](https://github.com/elripalda/gta4-ps5), which showed how a recompiled game runs on the PS5.
- The PS5 homebrew SDKs, drivers and app tooling.
- [DXVK](https://github.com/doitsujin/dxvk), [SDL](https://libsdl.org) and [FFmpeg](https://ffmpeg.org).

The full list is in [CREDITS.md](CREDITS.md).

## License and legal

fnv_recomp's source code is licensed under the [GNU General Public License v3.0 or later](LICENSE). Third-party
components keep their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

These licenses do not cover Fallout: New Vegas. The game's code, artwork, audio and data belong to their owners.
The app contains code generated from the game's executable, ships with no game data, and runs only with files from
your own copy. Do not share your game files.

fnv_recomp is an unofficial fan project. It is not affiliated with or endorsed by Bethesda Softworks, ZeniMax
Media, Microsoft, Obsidian Entertainment or Sony Interactive Entertainment. Fallout and New Vegas are trademarks
of ZeniMax Media Inc. PlayStation, PS5 and DualSense are trademarks of Sony Interactive Entertainment Inc.
