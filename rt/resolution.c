/* Resolution: a choice in Settings > Display, next to Texture Size.
 *
 * The game renders at iSize W x iSize H ([Display] in FalloutPrefs.ini), read once at start-up; on the PC the
 * launcher sets them. The PS5 shows the picture on the TV at the TV's own mode (4K on a 4K set, see the DXVK PS5
 * display code), scaling the game's image to it. This adds a Settings option the way the game builds its own:
 *
 *   StartMenu::UserOption (0x30 bytes): +4 name, +0xC submenus (0x10 = Display), +0x10 template, +0x14 value,
 *   +0x18 default, +0x1C type (0 = a list of named values), +0x20 value count, +0x24 width of the value column,
 *   +0x28 value names, +0x2C change callback. Built with operator new (0x401000) and the UserOption constructor
 *   (0x7D6350, cdecl: this, name, callback, submenus, type, count, width, names...), added with the options
 *   array's Add (0x7CB2E0, thiscall on 0x11DAB50).
 *
 * The callback is called with StartMenu's FLAG_SAVE_OPTIONS set to apply the choice, and without it to show the
 * current one, like Texture Size's (0x7D1B40): it sets the iSize settings (0x11C73DC, 0x11C718C; the value at +4),
 * which the game writes to FalloutPrefs.ini itself, and raises FLAG_RESTART_REQUIRED (StartMenu 0x75F6F0) so the
 * game says a restart is needed. */
#include "rt.h"
#include "d3d9_internal.h"

#define OPTIONS        0x11DAB50u /* BSSimpleArray<UserOption*>: +4 data, +8 count */
#define START_MENU     0x11DAAC0u /* StartMenu*, while it exists */
#define SM_FLAGS       0x1A8u
#define FLAG_SAVE      32u
#define FLAG_RESTART   0x400000u
#define SET_ISIZE_W    0x11C73DCu
#define SET_ISIZE_H    0x11C718Cu
#define TEXTURE_SIZE_CB 0x7D1B40u

static const u32 g_w[] = {1280, 1920, 2560, 3840}, g_h[] = {720, 1080, 1440, 2160};
#define NRES 4
static u32 g_cb; /* guest address of our callback */

static int nearest(u32 h) {
  int best = 0;
  for (int i = 1; i < NRES; i++)
    if ((h > g_h[i] ? h - g_h[i] : g_h[i] - h) < (h > g_h[best] ? h - g_h[best] : g_h[best] - h)) best = i;
  return best;
}

void rt_prefs_set(const char *sect, const char *key, const char *val); /* firstrun.c */

static void option_changed(Ctx *c) {
  u32 opt = ARG(0), sm = M32(START_MENU);
  DevState *D = d3d_dev();
  u32 now_h = D && D->pp[1] ? D->pp[1] : M32(SET_ISIZE_H + 4); /* what the game renders at now */
  if (sm && (M32(sm + SM_FLAGS) & FLAG_SAVE)) {
    int i = (int)M32(opt + 0x14);
    if (i < 0 || i >= NRES) i = NRES - 1;
    if (M32(SET_ISIZE_W + 4) != g_w[i] || M32(SET_ISIZE_H + 4) != g_h[i]) {
      M32(SET_ISIZE_W + 4) = g_w[i];
      M32(SET_ISIZE_H + 4) = g_h[i];
      char w[16], h[16];
      snprintf(w, sizeof w, "%u", g_w[i]); snprintf(h, sizeof h, "%u", g_h[i]);
      rt_prefs_set("Display", "iSize W", w); /* the game writes it too; this way it is there even if it does not */
      rt_prefs_set("Display", "iSize H", h);
      rt_log("resolution: %ux%u chosen (takes effect when the game is started again)", g_w[i], g_h[i]);
    }
    if (g_h[i] != now_h) { /* only ever raised: another option (Texture Size) may have raised it too */
      u32 args[2] = {FLAG_RESTART, 1};
      c->r[1] = sm;
      rt_call_guest(c, 0x75F6F0u, 2, args);
    }
  } else {
    M32(opt + 0x14) = (u32)nearest(M32(SET_ISIZE_H + 4)); /* the setting: shows a choice made this session too */
    M32(opt + 0x18) = 0;
  }
  RETURN_CDECL(0);
}

static void install(Ctx *c);
/* called when the start menu is set up (from its init hook, before the original runs): adds the option once,
   after Texture Size. The guest calls below change eax/ecx/edx, which the original init still needs (ecx is its
   this), so they are put back. */
void resolution_option_install(Ctx *c) {
  u32 eax = c->r[0], ecx = c->r[1], edx = c->r[2];
  install(c);
  c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx;
}

static void install(Ctx *c) {
  if (M32(SET_ISIZE_W + 8) != 0x1021944u || M32(SET_ISIZE_H + 8) != 0x1021954u) {
    static int said;
    if (!said++) rt_log("resolution: iSize settings not where expected; no Resolution option");
    return;
  }
  if (!g_cb) g_cb = rt_thunk(option_changed, "Resolution option");
  u32 data = M32(OPTIONS + 4), n = M32(OPTIONS + 8);
  for (u32 i = 0; i < n; i++)
    if (M32(M32(data + 4 * i) + 0x2C) == g_cb) return; /* already there */

  u32 size = 0x30;
  u32 obj = rt_call_guest(c, 0x401000u, 1, &size); /* operator new: the game frees it with its own delete */
  if (!obj) return;
  static u32 name, values[NRES];
  if (!name) {
    name = guest_strdup("Resolution");
    for (int i = 0; i < NRES; i++) {
      char t[32]; snprintf(t, sizeof t, "%u x %u", g_w[i], g_h[i]);
      values[i] = guest_strdup(t);
    }
  }
  u32 args[7 + NRES] = {obj, name, g_cb, 0x10 /* Display */, 0 /* named values */, NRES, 85 /* value column */};
  for (int i = 0; i < NRES; i++) args[7 + i] = values[i];
  rt_call_guest(c, 0x7D6350u, 7 + NRES, args);

  u32 slot = heap_alloc(4, 0); /* Add takes the address of the pointer */
  M32(slot) = obj;
  c->r[1] = OPTIONS;
  rt_call_guest(c, 0x7CB2E0u, 1, &slot);
  heap_free(slot);

  /* it went on the end: move it to just after Texture Size */
  data = M32(OPTIONS + 4); n = M32(OPTIONS + 8);
  if (!n || M32(data + 4 * (n - 1)) != obj) return;
  for (u32 i = 0; i + 1 < n; i++)
    if (M32(M32(data + 4 * i) + 0x2C) == TEXTURE_SIZE_CB) {
      for (u32 k = n - 1; k > i + 1; k--) M32(data + 4 * k) = M32(data + 4 * (k - 1));
      M32(data + 4 * (i + 1)) = obj;
      break;
    }
  rt_log("resolution: Settings > Display > Resolution added (now %ux%u)", M32(SET_ISIZE_W + 4), M32(SET_ISIZE_H + 4));
}
