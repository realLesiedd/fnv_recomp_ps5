/* The port's own choices in Settings > Display, after Texture Size: Resolution, Frame Rate Cap, Performance Overlay.
 *
 * Each is added the way the game builds its own options:
 *
 *   StartMenu::UserOption (0x30 bytes): +4 name, +0xC submenus (0x10 = Display), +0x10 template, +0x14 value,
 *   +0x18 default, +0x1C type (0 = a list of named values), +0x20 value count, +0x24 width of the value column,
 *   +0x28 value names, +0x2C change callback. Built with operator new (0x401000) and the UserOption constructor
 *   (0x7D6350, cdecl: this, name, callback, submenus, type, count, width, names...), added with the options
 *   array's Add (0x7CB2E0, thiscall on 0x11DAB50), then moved up after Texture Size.
 *
 * The game calls an option's callback with StartMenu's FLAG_SAVE_OPTIONS set to apply the choice (it does so as the
 * choice changes), and without it to show the current one, like Texture Size's (0x7D1B40).
 *
 * Resolution: the game renders at iSize W x iSize H ([Display] in FalloutPrefs.ini), read once at start-up; on the
 * PC the launcher sets them. The callback sets the iSize settings (0x11C73DC, 0x11C718C; the value at +4), which the
 * game saves itself, and raises the start menu's restart-required flag (0x75F6F0), as Texture Size does.
 * Frame Rate Cap and Performance Overlay (osd.c) apply at once and are kept in [fnv_recomp].
 *
 * Settings > Controls, after Vibration: Adaptive Triggers and Light Bar (dualsense.c), also at once. */
#include "rt.h"
#include "d3d9_internal.h"

#define OPTIONS         0x11DAB50u /* BSSimpleArray<UserOption*>: +4 data, +8 count */
#define START_MENU      0x11DAAC0u /* StartMenu*, while it exists */
#define SM_FLAGS        0x1A8u
#define FLAG_SAVE       32u
#define FLAG_RESTART    0x400000u
#define SET_ISIZE_W     0x11C73DCu
#define SET_ISIZE_H     0x11C718Cu
#define TEXTURE_SIZE_CB 0x7D1B40u
#define VIBRATION_CB    0x7D30E0u

void rt_prefs_set(const char *sect, const char *key, const char *val); /* firstrun.c */
extern int g_osd_mode, g_frame_cap;                                    /* osd.c */
extern int g_ds_triggers, g_ds_lightbar;                               /* dualsense.c */

/* ---- the options ------------------------------------------------------------------------------------ */
static const u32 g_w[] = {1280, 1920, 2560, 3840}, g_h[] = {720, 1080, 1440, 2160};

static int res_get(void) {
  u32 h = M32(SET_ISIZE_H + 4); /* the setting: shows a choice made this session too */
  int best = 0;
  for (int i = 1; i < 4; i++)
    if ((h > g_h[i] ? h - g_h[i] : g_h[i] - h) < (h > g_h[best] ? h - g_h[best] : g_h[best] - h)) best = i;
  return best;
}
static int res_set(int i) { /* returns 1 if a restart is needed */
  if (M32(SET_ISIZE_W + 4) != g_w[i] || M32(SET_ISIZE_H + 4) != g_h[i]) {
    M32(SET_ISIZE_W + 4) = g_w[i];
    M32(SET_ISIZE_H + 4) = g_h[i];
    char w[16], h[16];
    snprintf(w, sizeof w, "%u", g_w[i]); snprintf(h, sizeof h, "%u", g_h[i]);
    rt_prefs_set("Display", "iSize W", w); /* the game writes it too; this way it is there even if it does not */
    rt_prefs_set("Display", "iSize H", h);
    rt_log("options: resolution %ux%u chosen (takes effect when the game is started again)", g_w[i], g_h[i]);
  }
  DevState *D = d3d_dev();
  u32 now_h = D && D->pp[1] ? D->pp[1] : g_h[i]; /* what the game renders at now */
  return g_h[i] != now_h;
}

static int cap_get(void) { return g_frame_cap == 30 ? 0 : 1; }
static int cap_set(int i) {
  int cap = i == 0 ? 30 : 60;
  if (cap != g_frame_cap) {
    g_frame_cap = cap;
    rt_prefs_set("fnv_recomp", "iFrameCap", cap == 30 ? "30" : "60");
    rt_log("options: frame rate cap %d", cap);
  }
  return 0;
}

static int osd_get(void) { return g_osd_mode; }
static int osd_set(int i) {
  if (i != g_osd_mode) {
    g_osd_mode = i;
    char v[4]; snprintf(v, sizeof v, "%d", i);
    rt_prefs_set("fnv_recomp", "iPerfOverlay", v);
    rt_log("options: performance overlay %s", i == 0 ? "off" : i == 1 ? "basic" : "detailed");
  }
  return 0;
}

static int onoff_set(int *v, int i, const char *key, const char *what) {
  if ((i != 0) != *v) {
    *v = i != 0;
    rt_prefs_set("fnv_recomp", key, *v ? "1" : "0");
    rt_log("options: %s %s", what, *v ? "on" : "off");
  }
  return 0;
}
static int trg_get(void) { return g_ds_triggers ? 1 : 0; }
static int trg_set(int i) { return onoff_set(&g_ds_triggers, i, "iAdaptiveTriggers", "adaptive triggers"); }
static int lb_get(void) { return g_ds_lightbar ? 1 : 0; }
static int lb_set(int i) { return onoff_set(&g_ds_lightbar, i, "iLightBar", "light bar"); }

typedef struct {
  const char *name;
  const char *values[4];
  int count, width;
  int (*get)(void);
  int (*set)(int); /* returns 1 if a restart is needed */
  u32 modes;       /* the submenu: 0x10 Display, 0x40 Controls */
  u32 after;       /* the game's option it follows (its callback), or 0 for the one before in this table */
  u32 cb;          /* guest address of its callback */
} PortOption;

static void cb_res(Ctx *c), cb_cap(Ctx *c), cb_osd(Ctx *c), cb_trg(Ctx *c), cb_lb(Ctx *c);
static PortOption g_opts[] = {
  {"Resolution", {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"}, 4, 85, res_get, res_set, 0x10, TEXTURE_SIZE_CB},
  {"Frame Rate Cap", {"30 FPS", "60 FPS"}, 2, 55, cap_get, cap_set, 0x10, 0},
  {"Performance Overlay", {"OFF", "BASIC", "DETAILED"}, 3, 85, osd_get, osd_set, 0x10, 0},
  {"Adaptive Triggers", {"OFF", "ON"}, 2, 0, trg_get, trg_set, 0x40, VIBRATION_CB},
  {"Light Bar Health", {"OFF", "ON"}, 2, 0, lb_get, lb_set, 0x40, 0},
};
#define NOPT (int)(sizeof g_opts / sizeof *g_opts)
static const HostFn g_cbs[NOPT] = {cb_res, cb_cap, cb_osd, cb_trg, cb_lb};

static void changed(Ctx *c, PortOption *o) {
  u32 opt = ARG(0), sm = M32(START_MENU);
  if (sm && (M32(sm + SM_FLAGS) & FLAG_SAVE)) {
    int i = (int)M32(opt + 0x14);
    if (i < 0 || i >= o->count) i = o->count - 1;
    if (o->set(i)) { /* only ever raised: another option (Texture Size) may have raised it too */
      u32 args[2] = {FLAG_RESTART, 1};
      c->r[1] = sm;
      rt_call_guest(c, 0x75F6F0u, 2, args);
    }
  } else {
    M32(opt + 0x14) = (u32)o->get();
    M32(opt + 0x18) = 0;
  }
  RETURN_CDECL(0);
}
static void cb_res(Ctx *c) { changed(c, &g_opts[0]); }
static void cb_cap(Ctx *c) { changed(c, &g_opts[1]); }
static void cb_osd(Ctx *c) { changed(c, &g_opts[2]); }
static void cb_trg(Ctx *c) { changed(c, &g_opts[3]); }
static void cb_lb(Ctx *c) { changed(c, &g_opts[4]); }

/* ---- adding them to the menu ------------------------------------------------------------------------- */
static int index_of_cb(u32 cb) {
  u32 data = M32(OPTIONS + 4), n = M32(OPTIONS + 8);
  for (u32 i = 0; i < n; i++)
    if (M32(M32(data + 4 * i) + 0x2C) == cb) return (int)i;
  return -1;
}

static void add(Ctx *c, PortOption *o, u32 after_cb) {
  if (index_of_cb(o->cb) >= 0) return; /* already there */
  u32 size = 0x30;
  u32 obj = rt_call_guest(c, 0x401000u, 1, &size); /* operator new: the game frees it with its own delete */
  if (!obj) return;
  u32 args[7 + 4] = {obj, guest_strdup(o->name), o->cb, o->modes, 0 /* named values */, (u32)o->count, (u32)o->width};
  for (int i = 0; i < o->count; i++) args[7 + i] = guest_strdup(o->values[i]);
  rt_call_guest(c, 0x7D6350u, 7 + o->count, args);

  u32 slot = heap_alloc(4, 0); /* Add takes the address of the pointer */
  M32(slot) = obj;
  c->r[1] = OPTIONS;
  rt_call_guest(c, 0x7CB2E0u, 1, &slot);
  heap_free(slot);

  /* it went on the end: move it to just after the given option */
  u32 data = M32(OPTIONS + 4), n = M32(OPTIONS + 8);
  int at = index_of_cb(after_cb);
  if (!n || M32(data + 4 * (n - 1)) != obj || at < 0) return;
  for (u32 k = n - 1; k > (u32)at + 1; k--) M32(data + 4 * k) = M32(data + 4 * (k - 1));
  M32(data + 4 * (at + 1)) = obj;
}

/* called when the start menu is set up (from its init hook, before the original runs). The guest calls change
   eax/ecx/edx, which the original init still needs (ecx is its this), so they are put back. */
void port_options_install(Ctx *c) {
  if (M32(SET_ISIZE_W + 8) != 0x1021944u || M32(SET_ISIZE_H + 8) != 0x1021954u) {
    static int said;
    if (!said++) rt_log("options: iSize settings not where expected; no port options in Settings");
    return;
  }
  u32 eax = c->r[0], ecx = c->r[1], edx = c->r[2];
  int added = index_of_cb(g_opts[0].cb ? g_opts[0].cb : 1) < 0;
  u32 after = 0;
  for (int i = 0; i < NOPT; i++) {
    if (!g_opts[i].cb) g_opts[i].cb = rt_thunk(g_cbs[i], g_opts[i].name);
    if (g_opts[i].after) after = g_opts[i].after;
    add(c, &g_opts[i], after);
    after = g_opts[i].cb;
  }
  c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx;
  if (added)
    rt_log("options: Settings > Display: Resolution (now %ux%u), Frame Rate Cap (%d), Performance Overlay (%s); "
           "Settings > Controls: Adaptive Triggers (%s), Light Bar Health (%s) added",
           M32(SET_ISIZE_W + 4), M32(SET_ISIZE_H + 4), g_frame_cap, g_osd_mode == 0 ? "off" : g_osd_mode == 1 ? "basic" : "detailed",
           g_ds_triggers ? "on" : "off", g_ds_lightbar ? "on" : "off");
}
