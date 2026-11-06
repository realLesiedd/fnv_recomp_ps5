/* Deliberate departures from the original exe. Each function here replaces one game function listed in
 * HOOKS in tools/recomp.py; the original stays callable as o_XXXXXXXX. */
#include "rt.h"
#include <time.h>
#include <dirent.h>

void o_007CBAF0(Ctx *restrict c);
void o_007CFD20(Ctx *restrict c);
void o_00789820(Ctx *restrict c);
void o_0078D080(Ctx *restrict c);
void o_007D0BF0(Ctx *restrict c);

/* Same test the start menu itself makes: any *.fos in the save folder. */
static int saves_exist(void) {
  char hp[2048];
  if (fs_host_path("C:\\Users\\Player\\Documents\\My Games\\FalloutNV\\Saves", hp, sizeof hp)) return 0;
  DIR *d = opendir(hp);
  if (!d) return 0;
  struct dirent *e;
  int found = 0;
  while (!found && (e = readdir(d))) found = rt_wildmatch("*.fos", e->d_name);
  closedir(d);
  return found;
}

/* Start menu option list (BSSimpleArray<Option*>: +4 data, +8 count); Option: +8 action callback,
 * +0xC flags (bit 0 = listed on the main menu, bit 1 = listed in game). */
#define START_MENU_OPTIONS 0x11DAB88u
#define OPTION_CONTINUE    0x7D0440u /* asks "Continue from your last saved game?"; 0x7D0680 is Load */

/* StartMenu init (thiscall: bool inGame, bool forceReload). The original exe lists Continue on the main
 * menu even with no saves (dimmed, then re-enabled by 0x7CFD20). Leave it off the list until a save exists. */
void h_007CBAF0(Ctx *restrict c) {
  static u32 hidden; /* Continue options we unlisted, so they can be listed again */
  int in_game = M8(c->r[4] + 4) != 0;
  if (!in_game) {
    int have = saves_exist();
    u32 data = M32(START_MENU_OPTIONS + 4), n = M32(START_MENU_OPTIONS + 8);
    for (u32 i = 0; i < n && i < 64; i++) {
      u32 opt = M32(data + 4 * i);
      if (!opt || M32(opt + 8) != OPTION_CONTINUE) continue;
      if (!have && (M32(opt + 12) & 1)) { M32(opt + 12) &= ~1u; hidden |= 1u << (i & 31); }
      else if (have && (hidden & (1u << (i & 31)))) { M32(opt + 12) |= 1; hidden &= ~(1u << (i & 31)); }
    }
    rt_log("main menu: %s", have ? "saves found, Continue listed" : "no saves, Continue not listed");
    if (getenv("FNV_DUMP_MENU"))
      for (u32 i = 0; i < n && i < 64; i++) {
        u32 opt = M32(data + 4 * i), w[8];
        for (int k = 0; k < 8; k++) w[k] = M32(opt + 4 * k);
        char txt[64] = "";
        for (int k = 0; k < 8 && !txt[0]; k++) /* the first field that points at printable text */
          if (w[k] > 0x10000 && w[k] < 0xF0000000u) {
            const u8 *t = HP(w[k]);
            int j = 0;
            while (j < 40 && t[j] >= 32 && t[j] < 127) j++;
            if (j >= 3 && !t[j]) snprintf(txt, sizeof txt, "+%d \"%s\"", 4 * k, (const char *)t);
          }
        rt_log("  option %u: callback %08x flags %08x %s", i, M32(opt + 8), M32(opt + 12), txt);
      }
  if (getenv("FNV_DUMP_MENU")) { /* the Settings options (StartMenu::UserOption, 0x30 bytes) */
    u32 data = M32(0x11DAB50u + 4), n = M32(0x11DAB50u + 8);
    for (u32 i = 0; i < n && i < 128; i++) {
      u32 o = M32(data + 4 * i);
      const char *name = M32(o + 4) ? (const char *)HP(M32(o + 4)) : "";
      char vals[160] = "";
      if (M32(o + 0x28) && M32(o + 0x1C) < 2)
        for (u32 k = 0; k < M32(o + 0x20) && k < 6; k++) {
          u32 v = M32(M32(o + 0x28) + 4 * k);
          size_t l = strlen(vals);
          snprintf(vals + l, sizeof vals - l, "%s%s", k ? "|" : "", v ? (const char *)HP(v) : "?");
        }
      rt_log("  user option %u at %08x: vt %08x \"%s\" modes %08x tmpl \"%s\" val %d def %d type %d count %u f24 %u "
             "values [%s] change %08x", i, o, M32(o), name, M32(o + 0xC), M32(o + 0x10) ? (const char *)HP(M32(o + 0x10)) : "",
             (int)M32(o + 0x14), (int)M32(o + 0x18), (int)M32(o + 0x1C), M32(o + 0x20), M32(o + 0x24), vals, M32(o + 0x2C));
    }
  }
  }
  { extern void port_options_install(Ctx *c); port_options_install(c); } /* Settings > Display: Resolution, Frame Rate Cap, Performance Overlay (options.c) */
  o_007CBAF0(c);
}

/* Sets _enabled on the Continue (+0x1C4) and Load (+0x1C0) tiles. With Continue unlisted its tile is NULL;
 * point it at the other tile for the duration of the call. */
void h_007CFD20(Ctx *restrict c) {
  u32 self = c->r[1], cont = M32(self + 0x1C4), load = M32(self + 0x1C0);
  if (cont && load) { o_007CFD20(c); return; }
  if (!cont && !load) { c->r[4] += 4; return; } /* neither tile: just return (plain ret) */
  M32(self + 0x1C4) = cont ? cont : load;
  M32(self + 0x1C0) = load ? load : cont;
  o_007CFD20(c);
  M32(self + 0x1C4) = cont;
  M32(self + 0x1C0) = load;
}

/* Quitting: the loading-screen thread (0x78D490) wakes from its semaphore and calls LoadingMenu::Update on the
 * menu singleton (0x11DA0C0), then UpdateLoadingMenuState, before checking its exit flag. When the game quits it
 * destroys the menu and wakes the thread, which then reads null+0x5BC: the original game's crash on exit, which
 * on Windows usually goes unseen because the process is already being torn down. Skip both calls when there is
 * no menu; the thread then sees its exit flag and ends normally. */
#define LOADING_MENU 0x11DA0C0u
void h_00789820(Ctx *restrict c) { /* thiscall, no stack arguments */
  if (!c->r[1]) { c->r[4] += 4; return; }
  o_00789820(c);
}
void h_0078D080(Ctx *restrict c) { /* no arguments; only the loading-screen thread's call (returns to 0x78D55C) */
  if (M32(c->r[4]) == 0x78D55Cu && !M32(LOADING_MENU)) { c->r[4] += 4; return; }
  o_0078D080(c);
}

/* Exit Game (the Quit confirmation). The original hides the menus, draws a last frame and raises the game's quit
 * flag; the game would then tear itself down, and that teardown has crashes of its own (above). Close the app
 * right away instead, the way FileDen closes: back to the PS5 home screen. */
void h_007D0BF0(Ctx *restrict c) {
  o_007D0BF0(c);
  rt_log("Exit Game chosen: closing the app");
  rt_exit(0);
}

/* Diagnostic, not a change: the player's footsteps (PlayFootstepImpact, cdecl actor, material). In first person
   the game plays them as a 2D sound, in third person as a 3D sound at the feet. The owner hears none in first
   person on PS5 while the Linux build plays them, so each of the first 30 player steps (then every 200th) is
   logged with the view and surface, and the sounds started in the next 0.3 s are logged by dsound.c. */
void o_008391B0(Ctx *restrict c);
void h_008391B0(Ctx *restrict c) {
  u32 actor = M32(c->r[4] + 4), material = M32(c->r[4] + 8), player = M32(0x11DEA3C);
  static unsigned calls;
  if (++calls == 1 || calls % 500 == 0) rt_log("footstep: %u footsteps so far (all actors)", calls);
  if (actor && actor == player) {
    static unsigned steps;
    steps++;
    if (steps <= 30 || steps % 200 == 0) {
      extern double g_snd_trace_until;
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      g_snd_trace_until = ts.tv_sec + ts.tv_nsec / 1e9 + 0.3;
      rt_log("footstep: player step %u, %s person, material %u", steps, M8(player + 0x64B) ? "third" : "first", material);
    }
  }
  o_008391B0(c);
}
