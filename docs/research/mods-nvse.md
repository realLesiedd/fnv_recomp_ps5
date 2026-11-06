# Mods folder and NVSE

Loose files and plugin lists are a file-layer problem. The script extender is not. `LoadLibraryA` cannot bring in a Windows DLL, and the loader maps one executable. The roadmap already says the extender is rebuilt into the port. This note is what that costs, and what a mods folder can do without it.

## What a mods folder needs, and what already exists

`overlay_add` (`rt/overlay.c`, around line 96) scans a tree that mirrors the game folder. A later overlay replaces an earlier one's file. `fs_host_path` (`rt/api.c`) returns the overlay file for a path inside the game folder. `FindFirstFileA` (`rt/kernel32.c`, around line 830) merges overlay names into the listing and, for an overlay file, reports `overlay_time()` so the game treats it as newer than the BSA. The comment at the top of `rt/overlay.c` cites the game's own rule: a loose file wins when it is newer than the archive (`DirectoryArchive`, `0xB01590` in the decomp, as that comment says).

`write_invalidation` (around line 154) writes `Data/ArchiveInvalidation.txt` as bare filenames, which is the list the game reads (`SInvalidationFile`, `ClearMatchedEntries` `0xAFAD00`, reader `0xAF5AB0`, per that comment). It keeps the game's own list if that file is not inside the overlay being written. `overlay_init` (around line 213) does this only for the built-in assets under `user/assets`, or for `FNV_ASSETS_DIR`. Nothing scans `mods/`.

v21 and v22 are the evidence that this mechanism is not magic. The button prompts were loose files, the invalidation list was written, and the save menu still showed the Xbox glyphs, because the game draws those cells from `textures\interface\interfaceshared0.dds` and never opens the loose button files. The fix was a fingerprint rewrite at upload (`rt/glyphs.c`), not the file layer. A mod that replaces a file the game opens by name is what the overlay can do. A mod that depends on the game noticing a new file inside an archive it does not re-read by name needs the same kind of special case, or it does not work. **Unconfirmed** how many real mods are in the first group. Texture and mesh replacers that the game opens by path are the first group. The atlas is the known member of the second.

### What is not built

- No scan of `mods/<ModName>/`. The roadmap's layout (a folder next to `game/`) is not read anywhere.
- No `plugins.txt`. The name does not occur in the tree. Where the game reads it, and the exact text it accepts, are **unconfirmed**. `FNV_TRACE` on `CreateFileA` during a boot that has plugins is how to see the path. Guessing `Documents\My Games\FalloutNV\plugins.txt` from the PC game would be an invention until that trace exists.
- No on/off switch. `rt/options.c` builds port options as `UserOption`s and stores them in `[fnv_recomp]`, which `rt/firstrun.c` says the game ignores. That is the pattern a mod switch would copy. The restart flag the resolution option already raises (`FLAG_RESTART` in `rt/options.c`) is the "applies after a restart" the roadmap asks for.
- No safe-mode button. `overlay_init` runs in `main` before `plat_init` (`rt/main.c`, around lines 710 and 718). The pad is not open yet when the only overlay is added. The game opens files only after `rt_call_guest` of the entry point, which is after `plat_init`. So the button can be read after `plat_init`, and mod trees can be `overlay_add`'d after that and before the entry point. The assets overlay can stay where it is. Which button is not chosen. The roadmap does not name one.

`FindFirstFileA` returns `readdir` order. If the game loads `.esp` files in enumeration order when no list exists, two filesystems disagree. **Unconfirmed** that it does this. Writing an explicit list, once the path is known, removes that ambiguity.

The game files stay where they are. The overlay does not copy them. A mods folder must not be implemented by writing into `game/Data`.

## Why a Windows DLL cannot load

`k_LoadLibraryA` (`rt/kernel32.c`, around line 381) calls `mod_handle`, which returns a fake address only for a name in `g_mod_names` (around line 353: the system DLLs the runtime itself implements, plus `binkw32.dll`, `libvorbisfile.dll`, `GalaxyWrp.dll`). Any other name, including `nvse.dll` or a plugin, returns 0 and sets error 126. The function does not open the file.

`GetProcAddress` by ordinal returns 0 (`n < 0x10000`, around line 392). By name it calls `rt_get_proc`, which only finds `api_lookup`. A plugin's imports are not in that table.

The loader maps one image. `map_image` (`rt/main.c`) copies the sections of `FalloutNV.exe` to `IMAGE_BASE` (`0x400000`, `rt/rt.h`). `tools/recomp.py` requires that exe and fnv_decomp's `units.json`. It is not a second-image loader. Guest code for a second PE would have to live somewhere else in the 3.75 GB window, under the thunks at `0xF0000000` (`rt/mem.c`), and nothing allocates an image there.

A plugin built for Windows is also an x86 PE that calls Windows and, for an NVSE plugin, the extender's exports. Recompiling the game did not produce a general PE loader. Closed-source plugins cannot be rebuilt, which is the reason the roadmap already excludes them. That conclusion does not depend on the licence text of any particular plugin. There is nothing to compile, and there is nowhere to map the DLL.

**Unconfirmed:** whether the base game ever calls `LoadLibraryA` on a name outside `g_mod_names`. The played sessions did not die in `stub_missing`, and `LoadLibraryA` is registered, so a call on an unknown name would log and return 0 rather than abort. No such log line is stored in the tree.

## What rebuilding NVSE into the port would take

The roadmap's mechanism is the hook list. `HOOKS` in `tools/recomp.py` (around line 1218) is five addresses. The comment says to keep the list short. Each one is emitted as `f_ADDR` forwarding to `h_ADDR` in `rt/hooks.c`, and the original body stays callable as `o_ADDR`. That is enough for a deliberate, small departure. It is the wrong shape for a patch set whose size is not known yet.

What is not in this repository: xNVSE's source, its patch list, or how many of those patches are code overwrites versus script-opcode registrations. **Unconfirmed** how many hooks 1.4.0.525 would need. Treating "rebuild xNVSE" as a small `HOOKS` addition is not supported by anything here.

The pieces that are known, because the port already does each one for something else:

1. **Find the sites.** The console hook (`console_script` in `rt/dualsense.c`, around line 182) already calls the game's script compiler: `Script::CompileAndRun` at `0x5AC400`, with the console manager at `0x71B160`. Those addresses came from the same decomp the recompiler is seeded with. An extender opcode has to be found the same way, one site at a time, against 1.4.0.525. No table of them exists here.
2. **Replace each site with a hook, or teach the recompiler a longer list.** If the list stays "short" as the comment requires, most of xNVSE does not fit. A longer list is a change to that policy, and it has to be explicit. Each hook is a behaviour change that the original `o_ADDR` can still be called from, which is how the quit and menu hooks work.
3. **Script functions need the compiler to accept them.** The console path is the test: `FNV_CONSOLE="seconds:command"` runs a line the way the console does. A new function is done when a line the base game rejects returns "ran" in that log. That does not require the rest of the extender.
4. **SEH.** `RaiseException` and `RtlUnwind` abort (`rt/kernel32.c`). **Speculation:** xNVSE throws. This tree does not show it. If a rebuilt file does throw, the extender waits on the unwinder. That is a separate piece of work, already described as not started.
5. **Plugins published as source.** They are the same kind of port as the extender: C or C++ rebuilt against the port's hook API, not DLLs dropped in `Data/NVSE/Plugins`. That directory is not read. A closed-source `.dll` is refused by returning 0 from `LoadLibraryA`, with the log line that function already writes. The refusal should stay a log line, not a crash.
6. **Loose files a plugin expects** go through the mods overlay, which is why the file layer comes first. An extender that looks for `Data/NVSE/Plugins/SomePlugin.ini` will see it if that path is in an overlay.

What not to do: run `nvse.dll` through `tools/recomp.py`. The tool checks the image against FalloutNV fingerprints when `--image-check` is passed, and the runtime checks `FalloutNV.exe` again in `map_image`. Even without the check, one guest image is mapped, at `0x400000`. A second recompiled DLL is a new loader. That is a larger project than the hooks, and it still would not run a plugin that imports arbitrary kernel32 names.

## Options

1. **Mods folder only, no NVSE.** `overlay_add` each enabled `mods/<Name>/` after `plat_init` and before the entry point. A held button skips that loop. The on/off list lives in `[fnv_recomp]` and is edited with the existing `UserOption` pattern, restart required. Trace `plugins.txt` before writing it. Ship texture and mesh mods, and `.esp` files once the list format is known. Script mods wait.
2. **NVSE as a long hook list, after option 1.** Inventory xNVSE's patches against 1.4.0.525 outside this tree, decide which are hooks, and raise the "keep this list short" limit on purpose. First done state: one opcode the base game does not have, run from `FNV_CONSOLE`. JIP LN, the example the roadmap already gives, is after the extender itself runs, and only the parts of it that are source.
3. **A second guest image for `nvse.dll` and for plugins.** Rejected by the loader as it is. It would also not load a closed-source plugin's missing imports. It does not meet the roadmap's "closed-source plugins can't be supported", because the point of that line is that those binaries cannot be part of the port.

## Recommendation

Option 1, then the first slice of option 2 (one opcode), and not option 3.

The file layer is the part the code already almost does. The order in `main` is the only structural constraint: mods must be added after the pad exists and before the guest opens files. `plugins.txt` waits on one traced path. Do not invent the file format.

NVSE starts as an inventory, not as a port of the whole project. The hook mechanism is proven by the five functions in `rt/hooks.c` and by `FNV_CONSOLE`. The unknown is the length of the patch list. Until that list is written down against 1.4.0.525, there is no estimate of the work that is better than "larger than the current hook table, and blocked on SEH only if a patch actually throws".

## How to verify

Mods:

1. A folder `mods/Example/` containing one loose texture the game opens by path (not an atlas cell). Enable it, start, and confirm the log line `overlay: N files from ...` and that the game loaded the loose file (`FNV_TRACE` on the open, or the picture).
2. Hold the chosen button at startup. The log should not contain that overlay line, and the game file should be back.
3. An `.esp` is not verified until the traced `plugins.txt` path is written and the game logs that it loaded the plugin. A texture test does not prove the plugin test.
4. Put the loose file in `game/Data` as well and confirm the port does not need that. The game folder should stay untouched.

NVSE:

1. The inventory is a list of addresses and a one-line purpose each, checked against 1.4.0.525, stored outside the game binary. It is not verified by anything in this repository today.
2. One new console command, `FNV_CONSOLE`, log line `console: ... (ran)`. The base game must reject that command without the hook, which is the control.
3. Copy any `.dll` into the mods folder and confirm the log shows `LoadLibraryA(...) = 0` and the process does not abort.
4. **Unconfirmed here:** no mod, no xNVSE tree, and no plugin were present. The claim that `LoadLibraryA` will not map them is the code, not a run.
