# Frame time, per-draw cost, shader compiles, dust and smoke

The steady frame rate and the long frames are different measurements, and the current log only breaks down the long ones. Dust and smoke with hard edges are a third thing: a picture, not a timer. No cause for that picture is in the code.

## Where the time goes

### Steady frames are not broken down

`hb_frame_end` (`rt/hostd3d9.cpp`, around line 587) logs a frame only when `frame_ms > 150`, and only the first 200 such frames. The line splits that time into:

- draws, and the number of draw calls timed (`T_DRAW`)
- creating GPU objects (`T_CREATE`)
- uploads (`T_UPLOAD`)
- other device calls (`T_OTHER`)
- Present
- the remainder, labelled `game`

v18 used this and found the long frames were the game's own code: draws, uploads and Present were a few milliseconds, and a burst of about 400 D3D objects was about 160 ms of which about 130 ms was CPU. Autosave was about 255 ms, about 150 ms of it in file writes, which is the gathered-write change, not the draw path.

A steady 45 fps frame is about 22 ms. It never reaches the 150 ms line. The 45 fps case in the v35 note is invisible to `hb_frame_end`.

What v35 does record, from the five-second `perf:` line (`rt/main.c`, `perf_line`, around line 514): frame time, main-thread CPU, process CPU, game RAM, D3D object counts, and GPU memory from `hb_gpu_mem`. On PS5 the per-thread clocks are forced off (`guest = -1`, around line 555) because they read 0. The overlay (`rt/osd.c`) adds draws per frame, GPU busy share, and a shader-compile count. v35's reading of a long stand-still: about 1,000 draws holds 60 fps, about 2,200 drops to 45 with the main thread at 100%, memory flat, and the pattern matches the idle camera (`fVanityModeDelay` in that note, which says "almost certainly"). The main thread is full. The GPU share for that scene is **not** quoted in the notes.

### What one draw does on the main thread

`hb_draw` (`rt/hostd3d9.cpp`, around line 482) is `T_DRAW`. It calls `sync_state` and then one DXVK draw. `be_draw` in `rt/d3d9.c` only increments counters.

`sync_state` (around line 394) walks render state, samplers and texture stages only when `D->dirty` says the game changed them. It then, on every draw, walks all 20 texture slots and all 16 vertex streams:

- `host_texture` (around line 197) creates the GPU texture on first use and calls `upload_texture` if any dirty bit is set. A clean texture is a flag check and a pointer compare. `SetTexture` runs only when the pointer changed.
- `host_vb` (around line 262) uploads a dirty range, and it pushes `T_UPLOAD` around the lock and the copy. The profiler counts nested sections exclusively (the comment around line 26), so that copy is subtracted from the surrounding `T_DRAW`. A dynamic buffer the game discarded is locked with `D3DLOCK_DISCARD`. That is the v20 particle fix. A particle buffer rewritten every frame shows up as upload, not as draw.

The comment on `hb_frame_end`'s draw bucket says DXVK compiles pipelines on first use inside the draw. Those compiles are not a separate counter in the slow-frame line. They are inside `T_DRAW`.

So a 2,200-draw frame's main-thread time is the sum of three things the log cannot currently separate on a 22 ms frame:

1. The game's own code between draws (the `game` remainder, only printed above 150 ms).
2. `sync_state` plus the DXVK draw call, including a pipeline compile if this is the first use.
3. Uploads of buffers and textures that were dirty, including discarded particle buffers.

**Unconfirmed** how those three split at 2,200 draws. v18 says that on the *stutter* frames the draw bucket was a few milliseconds. That does not say the draw bucket is a few milliseconds when the draw count is 2,200. It may be, and the 45 fps may be entirely the game's code. It may not. The log does not contain the split.

### Shader compiles, separately

The PS5 build sets `DXVK_CONFIG` to `dxvk.numCompilerThreads = 8` in `hb_init` (`rt/hostd3d9.cpp`, around line 64). v14 used 4. v16 moved to 8 and wanted a HUD so stutters could be tied to compiles. The port's overlay replaced that HUD.

`dxvk_fnv_stats` in `ps5/dxvk/dxvk-ps5.patch` writes four counters: GPU idle time, pipeline tasks done, pipeline tasks queued in all, graphics pipelines made. The overlay reads them (`rt/osd.c`, the GPU row). That is the separate signal. It is not in the slow-frame line.

Nothing in the tree sets a DXVK state-cache path. A second visit to an area compiling again, or not, is **unconfirmed**. v16 suspected compiles. v18 then found most logged stutters were object-creation bursts in the game. Both can be true for different frames. A disk cache is **speculation** until a second visit still increments the queued-task counter.

### Dust and smoke

v20 fixed particles flashing. Dynamic buffers locked with `D3DLOCK_DISCARD` now get fresh GPU storage (`host_vb`, the comment around line 276). Before that, a buffer was updated in place while the previous frame's draws still read it. The status note measured frame-to-frame brightness at a Primm fire barrel: 8.2 down to 3.2. That is the flicker.

The "To watch" line is a different report, from a player video: heavy orange sheets with hard edges. No later note names a cause. No code comment after the DISCARD fix describes a remaining particle bug.

What the renderer will not explain by itself:

- Blend, alpha test and samplers are passed through when the game sets them (`D_RS`, `D_SS` in `sync_state`). If the game sets alpha blend, DXVK receives alpha blend. A wrong blend would mean a state the game set and the backend dropped, and the dirty flags cover those states. **Unconfirmed** that any particle state is dropped.
- `prompt_patch` (`rt/glyphs.c`, called from `upload_texture` when level 0 is dirty) only rewrites the UI atlas and the Pip-Boy arm, matched by fingerprint. It does not touch a fire texture unless that texture collides with the fingerprint. **Unconfirmed**, and unlikely: the fingerprints are of specific interface textures.
- The movie path (`hb_movie_overlay`) draws a Bink frame over the whole screen. It is not the world particle path. The intro is the movie that was tested.
- The software rasterizer (`rt/d3d9_swr.inc`) is not the PS5 path. `ps5/build.sh` compiles `rt/hostd3d9.cpp`.

The sheets can still be the game's own cards, drawn large, with no mipmaps in the texture the game uploaded, or with alpha test rather than blend. That is **speculation** about the picture. The repository has no capture of it. `FNV_SHOT_AFTER` (handled in `rt/d3d9.c`) delays test screenshots. It does not compare them to a PC frame.

## Options

1. **Measure the steady frame before changing `sync_state`.** Extend the existing `Prof` buckets to a line on the same five-second cadence as `perf:`, including frames under 150 ms: average draw, create, upload, other, Present, and the game remainder, plus the draw count. Gate it with an environment variable so a normal session stays as quiet as it is now. This is a small runtime change. It is not in this note's branch. Without it, a change to `sync_state` cannot be shown to have moved the 45 fps.
2. **Spend less time in `sync_state` on the guess that the pointer walks are the cost.** The walks are real (20 textures, 16 streams, every draw). v18's stutter data says the draw bucket was small on those frames, so this guess is weak until option 1 says otherwise. Doing it first risks a large diff aimed at the wrong bucket.
3. **A DXVK state cache**, if option 1 or a second-visit log shows the compile counter still climbing on a return to the same cells. Not before.
4. **For the sheets: a paired screenshot, not a blend change.** Same place (the Primm barrel the v20 note used, or the spot in the player video), port and PC, via `FNV_SHOT_AFTER` on the port. If the PC frame has the same hard edges, the port is drawing what the game asked. If the PC frame is soft and the port frame is hard, then trace the blend and alpha-test states and the sampler filter on that draw. There is no such trace today. Adding one is a follow-up, after the pictures disagree.

## Recommendation

Option 1, then stop. The 45 fps scene is main-thread bound by the v35 note, and the code already has the timers. They are thrown away for every frame under 150 ms. Publish them once every five seconds. Read one stand-still at the ~2,200-draw part of the loop.

If the game remainder is almost the whole 22 ms, the next work is the recompiled draw loop, not DXVK. If `T_DRAW` is almost the whole 22 ms and the compile counter is flat, then `sync_state` and the DXVK draw call are the cost, and option 2 is justified. If the compile counter climbs during that scene, the stalls and the average are different problems and option 3 is the stall one.

For the sheets, option 4. Do not change blend or mip code from the video description alone.

## How to verify

1. After the five-second budget line exists: stand still on the v35 save until the draw count swings to ~2,200. The line should sum to about the frame time. Quote one line in the status notes.
2. Shader compiles: enter a cell, leave it, enter it again. The overlay's compile counter, or `dxvk_fnv_stats` tasks-queued, should be recorded for both entries. A second entry that does not queue tasks means the first-visit stutter is one-time. A second entry that queues tasks means a cache is worth trying.
3. Sheets: two screenshots of the same fire, port and PC. Same edges on both means there is nothing to fix in the backend. Different edges is the point a state trace becomes the next note.
4. **Unconfirmed here.** No PS5, no `fnv.log`, and no screenshot were available. The split of a 22 ms frame is not in the existing notes, because the logger does not record it.
