# Unimplemented Windows imports

What the runtime registers, what is only a stub, and what a played session has already shown is never called. The executable's import table is not in this repository. `tools/import_audit.py` is how to list the gap against a real `FalloutNV.exe`. It is not part of the game build.

## What is known

Imports are bound in `bind_imports` (`rt/main.c`). For each PE import, `api_lookup` (`rt/api.c`) matches the DLL ignoring case and the function name exactly. A miss is stored as `stub_missing`, which stops the process on the first call (`rt/main.c`, `stub_missing`, around line 391): `called %s, which has no implementation yet`. `bind_imports` (just below that) logs how many were missing. The count is not a constant in the source. The names live in `gen/image.c`, which `tools/recomp.py` writes from the player's exe and which `.gitignore` excludes. This environment has no `FalloutNV.exe` and no `gen/`, so the numeric gap is **unconfirmed**.

`api_lookup` walks tables passed to `api_register`. Those tables are the whole implemented surface:

| DLL | Registered from |
|---|---|
| `KERNEL32.dll` | `rt/kernel32.c` (`k32[]`, about line 992) |
| `USER32.dll`, `GDI32.dll`, `COMCTL32.dll` | `rt/user32.c` |
| `ADVAPI32.dll`, `SHELL32.dll`, `ole32.dll`, `WINMM.dll` (`timeGetTime` only), `WSOCK32.dll`, `XINPUT1_3.dll`, `GalaxyWrp.dll`, `d3d9.dll` (`D3DPERF_SetOptions`) | `rt/misc.c` |
| `DSOUND.dll` | `rt/dsound.c`, one entry: ordinal `#11` |
| `DINPUT8.dll` | `rt/dinput.c`, `DirectInput8Create` |
| `d3d9.dll` | `rt/d3d9.c`, `Direct3DCreate9` |
| `d3dx9_38.dll` | `rt/d3dx.c` (math, buffer, shader profiles) and `rt/d3d9.c` (DDS and surface load) |
| `WINMM.dll` mmio | `rt/mmio.c` |
| `libvorbisfile.dll` | `rt/vorbis.c` |
| `binkw32.dll` | `rt/bink.c`, decorated stdcall names (`_BinkOpen@8` and the rest of that table) |

`python3 tools/import_audit.py` against this tree reports 264 names (138 of them kernel32). That is the registered set. It is not the exe's import set.

Seven kernel32 names are also emitted inline at the call site (`INLINE_IMPORTS` in `tools/recomp.py`, around line 211): the interlocked family, `TlsGetValue`, `GetCurrentThreadId`. The runtime copies stay registered for `GetProcAddress` and for any site the recompiler did not inline.

### Reachable, and already exercised

The v13–v35 notes describe a session that reaches the Mojave, plays the intro, mixes sound, saves and loads. `stub_missing` calls `_exit` via `rt_fatal`. A call on that path would be the whole story of the session, and it is not. So every import that is **not** registered was either absent from the import table or present and uncalled on that path. **Unconfirmed** which of those two, until `tools/import_audit.py --exe` is run. What is confirmed is that the current play path does not call them.

These names are registered and the same sessions show they are used: file IO (saves), `FindFirstFileA` (the `*.fos` scan in `rt/hooks.c`), DirectSound, XInput ordinals `#2` and `#3` (`rt/misc.c`), the Bink names (the intro plays), the vorbis names (voices), `Direct3DCreate9`. Sound and the pad working also confirms the exe imports DirectSound and XInput **by those ordinals**. A name-based `DirectSoundCreate8` would not match `"#11"` (`strcmp` on the function name) and the first call would abort.

### Registered, but a stub or a lie

These are implemented names. A missing-import audit will not list them. They matter more than the uncalled gap, because the game can call them today.

Fatal if the game really raises:

- `RaiseException` (`rt/kernel32.c`, around line 649) accepts `0x406D1388` (thread name) and then stops. A C++ throw (`0xE06D7363`) stops with the type name.
- `RtlUnwind` stops. It is registered on `KERNEL32.dll`, not on `ntdll.dll`.

Returns success and does not do the thing:

- `SuspendThread` logs "ignored" and returns 0 (`rt/kernel32.c`, around line 632).
- `SetUnhandledExceptionFilter` returns 0 and stores nothing. `UnhandledExceptionFilter` returns 1.
- `GetAsyncKeyState` returns 0 (`rt/user32.c`). Keys the game sees come from DirectInput's `GetDeviceState`, which copies `plat_keys`.
- `WriteConsoleW` returns 0.
- `SetEnvironmentVariableA` returns 1 and sets nothing. Both `GetEnvironmentStrings` functions hand back an empty 16-byte block.
- `FileTimeToLocalFileTime` copies the value unchanged. `SystemTimeToTzSpecificLocalTime` copies the struct. `GetTimeZoneInformation` zeroes 172 bytes and returns 0 (`rt/kernel32.c`, around line 140).
- Registry (`rt/misc.c`, around line 11): `RegOpenKeyExA` and `RegQueryValueExA` return 2. Creates and sets return success and drop the write. The early status note says WinMain reads the registry and continues, so this failure is tolerated on the startup path.
- `WSOCK32`: `socket` returns `0xffffffff`, `gethostbyname` returns 0 (`rt/misc.c`, around line 73). The comment says networking is not available.
- Galaxy wrapper (`SteamAPI_Init` and the rest in `rt/misc.c`) reports the service is not running.
- `LoadLibraryA` (`rt/kernel32.c`, around line 381) returns a handle only for a name in `g_mod_names`. It does not map a file. `GetProcAddress` by ordinal (`n < 0x10000`, the Windows ordinal convention) returns 0. By name it only resolves `api_lookup`. `ntdll.dll` is in `g_mod_names` and has no registered functions, so a handle for it resolves nothing.
- `D3DXSaveTextureToFileA` (`rt/d3d9.c`) returns success and writes nothing.
- `DirectInput8Create` always logs that the keyboard and mouse report no input (`rt/dinput.c`). `GetDeviceState` does fill them from the platform. The log line is wrong on the Linux SDL build.

`MultiByteToWideChar` (`rt/kernel32.c`, around line 145) copies each byte into a 16-bit slot. It does not implement code page 1252, which is what `GetACP` claims. Characters above 127 in a save name or a path would not round-trip the way Windows 1252 would. **Unconfirmed** whether the played sessions use such characters.

### Not imports: COM methods that return success and do nothing

`rt/com.c` turns a null function pointer into a stub that returns 0. The game reaches these only by calling a vtable the runtime built, so they never show up as unimplemented imports, and a Mojave session does not prove they are uncalled. If the stub is enough, the game keeps going.

Device methods with a null pointer (`rt/d3d9.c`, the `device_methods` table around line 747): `SetCursorProperties`, `CreateAdditionalSwapChain`, `SetDialogBoxMode`, the palette setters, `SetSoftwareVertexProcessing`, `SetNPatchMode`, `ProcessVertices`, `DrawRectPatch`, `DrawTriPatch`, `DeletePatch`. `GetFrontBufferData` and `MultiplyTransform` have functions and still do no work. `IDirect3DStateBlock9::Capture` and `Apply` are null. `q_GetData` returns constants (occlusion is 100000 pixels).

DirectSound (`rt/dsound.c`): `SetFX`, `AcquireResources`, `SetFormat`, `Unlock`, `Restore`, the cone getters and setters, `SetSpeakerConfig`, `CommitDeferredSettings` are null. `gains` never reads doppler or velocity. The played sessions have sound, so the methods that are real (create, lock, play, volume, 3D position) are the ones the game is known to need.

DirectShow (`rt/dshow.c`): most of `IGraphBuilder` is null except `Render`, which decodes MP3 and logs "music stays silent" for anything else. Music in the status notes is MP3, so that path is the one in use.

DirectInput (`rt/dinput.c`): `GetDeviceState` is real. Acquire, properties, and the force-feedback methods are null. The pad the game plays with is XInput, not these.

## Which of these matter

1. **A missing import on an untested path.** Not the current crash queue. The played path would have aborted. A mod, or a quest that calls something the Mojave path does not, still dies at `stub_missing` with the name in the log. That is the list `tools/import_audit.py --exe` produces, crossed with "did this session's log contain `called ..., which has no implementation`".
2. **`RaiseException` / `RtlUnwind`.** Same shape: fatal, not seen in the play notes. Matters the first time something throws.
3. **The lies the startup path already hits.** Registry reads fail open. Time zone conversion is a no-op. `GetUserNameA` returns `"Player"` (`rt/misc.c`). None of these have been tied to a visible bug. They are the first place to look if a PC save's displayed date, or a string, disagrees with Windows. See the save note for the file-time details.
4. **COM stubs.** Matter only if a trace shows the call. Occlusion always reporting "visible" would add draws, not remove them. **Speculation:** the game issues occlusion queries. Nothing in the tree shows it.
5. **`LoadLibraryA` and ordinal `GetProcAddress`.** Do not matter for the base game's own imports. They are the reason a Windows DLL cannot appear later. That is the mods and NVSE question, not this one.

## Options

1. **Diff the exe, then stop.** Run the audit. Record the missing names in the status notes. Implement nothing until a log line names a call. This matches how the port has been choosing work: the played path already selected the imports that matter.
2. **Implement the next fatal name when a session hits it.** The log already contains the DLL, the name, and the guest return address. No speculative kernel32 surface.
3. **Fill the COM stubs that are cheap and wrong** (state-block apply, occlusion readback from DXVK) before a session proves the game calls them. This spends time on methods the Mojave path may never use. `GetFrontBufferData` is in this bucket. Save thumbnails already work (v25), so the game is not depending on that empty function for them.

## Recommendation

Option 1, then option 2. Run `tools/import_audit.py --exe` once and paste the missing list into the status notes so it is not rediscovered. Do not implement a name from that list until `stub_missing` fires or a `FNV_TRACE` log shows the call. Leave the COM stubs until the same kind of evidence exists, except where a session already shows a wrong result (none of the play notes do, for these stubs).

The registry, time-zone, and `GetUserNameA` behaviour should be left unless a save or a string is actually wrong. Changing them is a behaviour change for a path the game already survives.

## How to verify

1. `python3 tools/import_audit.py --exe game/FalloutNV.exe` with the GOG 1.4.0.525 executable. The "not registered" lines are the `stub_missing` set. The script uses the same DLL case rule and `#ordinal` spelling as `tools/recomp.py` and `api_lookup`.
2. Boot that exe to Goodsprings with `FNV_TRACE=1`. The startup line `bound N imports (M not implemented yet...)` should match the script's counts. There should be no `called ... which has no implementation yet` line.
3. A name that appears in both the script's missing list and that fatal line is the one to implement. The return address in the message is the call site.
4. **Unconfirmed here:** both steps. No executable was available in this environment, and no `fnv.log` is stored in the repository.
