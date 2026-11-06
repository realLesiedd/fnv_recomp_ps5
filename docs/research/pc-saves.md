# PC saves, both directions

The port does not have a save format of its own. A `.fos` file is whatever bytes the game's own code writes through `WriteFile`, and a load is whatever bytes it reads back through `ReadFile`. Nothing in this repository parses a save. A byte-for-byte copy is the whole compatibility story, plus the host differences below, which can change those bytes or the directory entry around them.

**Unconfirmed:** an actual PC save loaded on the PS5 app, and a port save loaded by the PC game. The player page says the format is the PC format and that copying has not been tested widely. v20 says a save written with the gathered writes loads back in the port. v25 says a save and its thumbnail were checked. v27 loaded the Primm save. None of those notes cross a machine.

## What the port does to the bytes

### The file calls are ordinary

`CreateFileA` / `CreateFileW` (`rt/kernel32.c`, `create_file`, around line 679) map the Win32 path onto the host and `open` it. Disposition 1, 2, 4 and 5 become `O_EXCL`, `O_TRUNC`, `O_CREAT` and `O_TRUNC`. Share mode, flags, and the security attributes are ignored. `ReadFile` reads the host descriptor. `SetEndOfFile` truncates at the current offset.

Saves are not a special case. The only `.fos` handling is a log line: `FindFirstFileA` logs the scan when the mask contains `.fos` (around line 865), and `rt/hooks.c` `saves_exist` uses that same mask to decide whether Continue is listed.

Where the game looks: `SHGetFolderPathA` with CSIDL `0x05` returns `C:\Users\Player\Documents` (`rt/misc.c`, around line 25). The virtual filesystem (`rt/api.c`, `host_path`) maps `C:\Users\Player\` onto `FNV_USER_DIR`. On PS5 that is `/app0/user`, so the player page's path `user/Documents/My Games/FalloutNV/Saves/` is the same folder the game enumerates. Copying a PC file into that folder is the right place. The port will not find a save left anywhere else.

### Gathered writes

`WriteFile` (`rt/kernel32.c`, around line 721) keeps a per-handle buffer of up to 256 KB and writes it out in one piece. The comment says the game writes a save as many small pieces, and that those system calls stalled an autosave by about 0.25 s on PS5. `h_flush` (`rt/api.c`) is called from `win_fd`, and `win_fd` is what read, seek, size, flush and set-end-of-file go through. `h_close` flushes before `close`. A sequential write, then a seek back to patch a header, then a close, sees the buffered bytes. v20 says a save written this way loads back.

`rt_exit` (`rt/api.c`, around line 268) flushes C stdio and does not walk the Win32 handles. Exit Game (`h_007D0BF0` in `rt/hooks.c`) runs the original function and then `rt_exit`, and skips the game's teardown because that teardown crashes. A save the game has already closed is on disk. A handle still open at that moment keeps its last buffered chunk. **Unconfirmed** whether quit leaves a save file open. A save from the save menu, which returned to the game afterwards, has been closed.

### What is not translated

There is no endian swap, no compression step, and no rewrite of form ids. The game's code does that, and it is the same code the PC exe runs, recompiled. Two host differences can still change what that code decides to write, or what a later load accepts.

**Character encoding.** `GetACP` returns 1252, but `MultiByteToWideChar` (`rt/kernel32.c`, around line 145) copies each byte to a 16-bit slot, and `WideCharToMultiByte` stores the low 8 bits or `?`. A name or a path with a character above 127 will not match Windows-1252. **Unconfirmed** whether save names in the tested sessions use such characters. An ASCII name is unaffected.

**Who the OS user is.** `GetUserNameA` (`rt/misc.c`) writes `Player`. If the game stores that string in the save, a port save and a PC save differ in that field. **Unconfirmed** whether the game stores it, and whether a load checks it. The character's name is the game's own data, not this API.

**File times, not file bytes.** `FindFirstFileA` fills all three `WIN32_FIND_DATA` times from `st_mtime` (`fill_find`, around line 822) via `unix_to_ft` (around line 120). Creation and access are not distinct. `FileTimeToLocalFileTime` copies the value unchanged, and `GetTimeZoneInformation` returns a zeroed struct. `GetFileTime` / `SetFileTime` are not registered. The sort order and the displayed date of a copied save follow the host file's mtime. An FTP copy that stamps "now" makes that save look newest. That does not change the bytes inside the file. **Unconfirmed** whether the game also refuses a save based on those timestamps. It does use the listing to decide that saves exist (`rt/hooks.c`).

**Directory order.** `FindFirstFileA` returns `readdir` order, then overlay names. It does not sort. **Unconfirmed** whether the game's plugin load order, which a save records and checks, comes from this enumeration or from a plugins list. Nothing in the runtime writes `plugins.txt`. Two copies of the same `Data/` folder can still load plugins in a different order if the game trusts directory order. A save that names its masters will then fail the game's own check on one of the two machines. That is the game rejecting a plugin list, not a corrupt `.fos`.

**Addresses.** v27 ran with `FNV_VM_HIGH=1` (allocations start at 2 GB, `rt/mem.c`). The Primm save loaded, played and saved with every guest allocation above 2 GB. That is evidence the load path does not require the same addresses the save was written at. **Unconfirmed:** loading a save that was *written* under `FNV_VM_HIGH` in a later run without that flag. If a save stored a guest pointer, that second load is the one that would break. The v27 note does not say they did that second load.

**x87.** `rt/x86.h` keeps the x87 stack as `double`, with 80-bit conversions only at `ld80` / `st80`. A long chain the PC computed in 80-bit registers can diverge. A PC save's bytes are not recomputed on load. Divergence would show up in a save the port writes after simulating, compared with a PC save of the same actions, not in the load of a PC file. **Unconfirmed** that any saved quantity actually diverges. No play note attributes a bad save to it.

**Thumbnails.** v25 checked a save and its thumbnail on the port. `D3DXSaveTextureToFileA` returns success and writes nothing (`rt/d3d9.c`), so the thumbnail is not going through that function. The game has another way, and that way worked for a port-made save. **Unconfirmed** that the PC game displays a thumbnail written here, and the other way around. The bytes are still just the game's bytes if the same function wrote them.

## What would break a load

In the code that exists, a load breaks if:

- The file is not in the folder `SHGetFolderPathA` plus the game's own `My Games\FalloutNV\Saves` suffix. The port does not search elsewhere.
- The file was still buffered when the process was killed. `rt_fatal` and `rt_exit` do not flush Win32 handles. A crash mid-save, or quit with the handle open, leaves a short file. The game then rejects it for the same reason a truncated PC save fails.
- The game's own master list does not match. The port does not rewrite that list. A `Data/` folder that is not the one the save was made with fails here exactly as it fails on PC.
- A character the 1252 path mangled is part of a check the game does. **Unconfirmed** that any such check exists.

A load does not break because of the 256 KB gather, for a file that was closed. v20 loaded one. It does not break because the guest addresses moved, for a save that was not itself written under `FNV_VM_HIGH`. v27 loaded the Primm save in a high-address session; the normal-address loads in the other notes are the other direction of that pair, and they are port saves, not PC saves.

## Options

1. **Copy files and see.** Put a PC `.fos` in the saves folder, load it, save, copy that file back to a PC install with the same `Data/` and the same add-ons. No code. This is the test the player page is missing.
2. **Flush Win32 handles in `rt_exit`.** Closes the quit-during-save hole. Does not help the PC round trip. Worth doing if a quit is ever caught with a half-written save. Not indicated by the notes we have.
3. **Implement 1252 and real time zones.** Changes bytes the game writes for non-ASCII text and for any timestamp it converts. Do it only after option 1 shows a name or a date that the other side rejects. An ASCII save does not need it.
4. **A save translator.** There is nothing to translate. The format is the game's. A tool that rewrites form ids or plugin names would be for a save whose masters differ, which is a mod-load-order problem, not a port problem.

## Recommendation

Option 1, on one save made with all six add-ons and one made with the base game only, in both directions. Record the result in the status notes, including the first log line if the game refuses the file. Do not write a parser.

Add option 2 only if a session shows a truncated save after Exit Game. Leave encoding and time zones alone until a refused save is explained by them.

Also run the `FNV_VM_HIGH` save one more time without the variable, as a separate check from the PC copy. That is the pointer question, and it does not need a second machine.

## How to verify

1. From a PC install of 1.4.0.525, copy one `.fos` into `user/Documents/My Games/FalloutNV/Saves/` on the PS5 app (or `FNV_USER_DIR` on Linux). The `Data/` folder must be the same add-ons the save was made with.
2. Load it from the menu. Continue should list it: `saves_exist` in `rt/hooks.c` only checks that a `*.fos` is there, and the game does the real load.
3. Save under a new name. Copy that file back to the PC install's Saves folder and load it there.
4. Pass: both loads reach the loaded cell, and the log has no `rt_fatal`. Fail: record the game's message and the log line. A plugin-mismatch message is the master list, not the file layer.
5. Optional: `FNV_VM_HIGH=1`, save, run again without it, load that save.
6. **Unconfirmed here.** No save file and no second machine were available.
