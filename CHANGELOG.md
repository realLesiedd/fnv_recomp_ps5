# Changelog

What changed in each version, for players. The newest version is at the top. This starts at v22, when the PlayStation button prompts started working. Developer notes, including older versions, are in [STATUS.md](STATUS.md).

## v41

- A hidden debug menu is in testing. It has not been tried on the PS5 yet. It stays off unless `FalloutPrefs.ini` has `bDebugMenu=1` under `[fnv_recomp]`. Settings > Controls then adds rows to pick a place and go there, give a test kit, and turn god mode and noclip on or off.
- Release packaging is in testing. It has not been tried on the PS5 yet. The home screen version follows the build name. This build shows 01.041.000, and the zip is named `PPSA97701-v41.zip`.

## v40

- The log now follows each recorded sound until it ends. On a desktop test, the first-person footsteps play through.
- The v40 log explained why first-person footsteps are quiet.
- On PS5 system software 13.42 the console does not report per-core load, so that overlay row stays hidden. Linux still shows it.

## v39

- First-person footsteps are hard to hear on the PS5. Third person is fine. This version records the sounds the game starts, and which view you are in.
- The detailed performance overlay reads each core's load on the PS5, and only while that view is shown.

## v38

- The busiest maths for what you can see, and for moving things, now runs as native code. A check of 30 million calls matched the old results. `iFastMath=0` under `[fnv_recomp]` in `FalloutPrefs.ini` runs the old code.
- The log lists the picture modes the TV offers. The port still picks 4K near 60 Hz when a 120 Hz mode is listed. The frame cap stays at 60.

## v37

- v36 could close about a minute into play on the PS5. The new performance summary in the log caused it. This version fixes that crash.

## v36

- Loading a save could hang, including Continue. The game's multi-threaded face creation was the link in that hang. The port now turns that off every time it starts, and the same save then loaded. A button tap during a hitch is kept until the game has read it, so a short press is not lost.
- The busiest parts of the game are compiled harder. On a desktop test of a busy view, the main thread fell from about 87% to about 76% while the frame rate stayed at the 60 cap. That figure is from the desktop build, not from a PS5.
- The log gains a short performance summary about once a minute. It is on unless `FalloutPrefs.ini` contains `iProfiler=0` in the `[fnv_recomp]` section.

## v35

- Trigger Strength is spread out, and Light is much lighter. The four choices are about 15%, 30%, 55% and 100% of each weapon's strongest resistance (they were 40%, 65%, 85% and 100%). On a pistol that is 1, 2, 4 or 7 out of 8.
- The app checks `FalloutNV.exe` before it runs. The Steam executable, or any other version, closes the app and says why in `fnv.log`, instead of crashing later.
- The performance overlay is off on a new install. A choice you already made is kept.
- The app has its own icon.
- Standing still for a long time can slow the game down. That follows how busy the picture is, which fits the game's idle camera circling you. It is not a memory leak. A very busy view can fall to about 45 fps because the main thread is full.

## v34

- Menu presses are no longer lost. The main menu, the pause menu, and its Yes/No questions ignore a press that comes within half a second of the last one. Cross soon after leaving a page with Circle, or Yes soon after Continue, used to do nothing. The game's short pause is still there. The press is kept and handed over as soon as the menu is listening.

## v33

- The Back prompt (Wait) is the touchpad click. The PS5 keeps the Create button for itself.
- CPU and chip temperatures in the performance overlay are read directly, so they can show on the console.

## v32

- Button prompts brought closer to the PS3 version. L1 and R1 are a wide rectangle with the name in bold and a thinner outline. L2 and R2 match. The face symbols are larger and sit clear of the button's curve.

## v31

- Button prompts drawn in the PS3 icon style. Face buttons keep the game's raised-button curve with the PlayStation symbol. L1/R1, L2/R2, the sticks, Options and Create are thin outlines at the game's own line weight.

## v30

- Button prompts follow the PS3 version: the game's own button art stays, and only the Xbox letters are replaced with PlayStation symbols. An earlier prompt-size option is gone, and the game's own sizes are used.
- The detailed performance overlay has a CORES row, a bar and a percentage for each CPU core. On the PS5 that row stayed empty, because the console does not answer the reading that version asked for. On Linux it is filled from the system.

## v29

- Button prompts redrawn as solid shapes (a disc, a pill, or a trigger) with the symbol cut out, so they stay readable when the menus tint them, including on a 4K picture. v30 replaces this look.

## v28

- Adaptive triggers and the light bar work on the console. The previous version looked the functions up while the game was running, and the console refused that, so no trigger effect was sent and turning the light bar off could not give it back.
- If the console refuses to reset the light bar, it is set back to the console's blue.
- Settings > Controls has Trigger Strength: Light, Medium, Strong and Max. Strong is the default, and every strength is higher than in the first version.
- 360 Controller and Mouse Sensitivity are gone from Settings > Controls. The controller is the only input. If it had been turned off, it is turned back on.

## v27

- The game can use about 4 GB of memory, as it does on 64-bit Windows. Before this it had 2 GB.
- The DualSense light bar shows your health: green, through amber, to a pulsing red when health is low. Outside the game world it is New Vegas amber.
- Adaptive triggers: with a gun out, R2 resists and gives way partway (firmer for rifles and launchers, a steady push for automatics and heavy weapons) and kicks when you fire. L2 is a light push. Melee, unarmed, thrown weapons, menus and the Pip-Boy turn the effect off.
- Settings > Controls has Adaptive Triggers and Light Bar Health, each on or off at once.

## v26

- Settings > Display has a performance overlay and a frame rate cap. Both apply straight away.
- The overlay is Off, Basic (frame rate and frame time) or Detailed (frame rate with the 1% low, a frame-time graph, CPU, GPU, video memory, the game's memory and what the console has left, resolution, and how much the game is drawing). On the PS5 the detailed view also shows temperatures when the console allows it.
- Frame Rate Cap is 30 or 60. The game's physics stay as they are. 30 only takes bigger steps, and the cap never goes above 60. Until v35 the detailed overlay was the default.

## v25

- The game renders at the resolution in its settings, up to 3840×2160. The TV picture was already 4K. Before this the game itself rendered at 1280×720 and was scaled up.
- A new install, and an existing settings file the first time this version sees it, uses 3840×2160.
- Settings > Display > Resolution: 1280×720, 1920×1080, 2560×1440 and 3840×2160. It applies the next time you start the game.
- At 4K the game uses less of its own memory for the picture. A full-screen image is kept in game memory only if the game reads it.

## v24

- Button prompts are smaller. The first drawings were much bigger than the game's own letters. Face buttons are the symbols on their own, and every prompt is drawn a little smaller in its cell.

## v23

- The tags on the Pip-Boy arm say L2 and R2 instead of LT and RT. The tabs are on the triggers, so L1 and R1 would be wrong here.

## v22

- PlayStation button prompts work in the menus and the Pip-Boy. The game draws those prompts from one shared picture, not from separate button files, which is why earlier versions could not replace them. The port recognises that picture and paints PlayStation buttons over the Xbox ones, including the smaller copies.
