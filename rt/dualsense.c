/* DualSense light bar and adaptive triggers, from the game's state (Settings > Controls turns either off).
 *
 * Read on the game's main thread at each Present, about 20 times a second, by calling the game's own functions:
 *   the player (PlayerCharacter*, 0x11DEA3C) and its actor values (ActorValueOwner at +0xA4: virtual +0x08
 *   GetActorValue, +0x1C GetPermanentActorValue; 16 = health), its process (+0x68: virtual +0x148 GetWeaponInfo,
 *   the equipped weapon's inventory entry with the form at +8; +0x454 IsWeaponOut), the weapon's type
 *   (TESObjectWEAP +0xF4), whether a menu is up (0x702360, what the MenuMode script condition calls) and whether the
 *   player is in the world at all (its cell, +0x40: at the main menu there is a stand-in player in none).
 *
 * Light bar: the player's health, green when well, through amber, to a pulsing red below a third; New Vegas amber
 * outside the game world.
 *
 * Triggers (the console's trigger modes, as the GTA IV PS5 port drives them): with a gun out, R2 gives way partway
 * like a real trigger (weapon mode), firmer for heavier guns; automatics and heavy weapons push back evenly
 * (feedback). Each shot kicks R2 (vibration mode, briefly): a shot is the game's own firing rumble while R2 is
 * held. L2 (aim) pushes back lightly. Melee, unarmed, thrown weapons, menus and the Pip-Boy: off. */
#include "rt.h"
#include "platform.h"
#include <math.h>
#include <time.h>

#define PLAYER       0x11DEA3Cu
#define MENU_MODE_FN 0x702360u
#define AV_HEALTH    16
#define FORM_WEAPON  0x28

int g_ds_triggers = 1, g_ds_lightbar = 1; /* Settings > Controls (options.c) */
int g_ds_strength = 2;                    /* Trigger Strength: 0 light, 1 medium, 2 strong, 3 max */
double g_ds_rumble_time;                  /* when the game last asked for rumble (misc.c) */
int g_ds_rt;                              /* R2, 0-255, as last read (misc.c) */

int rt_setting_int(const char *key, int def);

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

void ds_init(void) {
  g_ds_triggers = rt_setting_int("iAdaptiveTriggers", 1) != 0;
  g_ds_lightbar = rt_setting_int("iLightBar", 1) != 0;
  g_ds_strength = rt_setting_int("iTriggerStrength", 2);
  if (g_ds_strength < 0 || g_ds_strength > 3) g_ds_strength = 2;
}

/* a vtable must point into the exe's read-only data */
static int is_vtable(u32 v) { return v >= 0x00FD0000u && v < 0x01120000u; }

static u32 vcall(Ctx *c, u32 obj, u32 off, int nargs, u32 *args) {
  u32 vt = M32(obj);
  c->r[1] = obj;
  return rt_call_guest(c, M32(vt + off), nargs, args);
}

typedef struct { int ok, in_game, menu, health, max_health, weapon_type, weapon_out; } GameState;

static void read_state(Ctx *c, GameState *s) {
  memset(s, 0, sizeof *s);
  s->weapon_type = -1;
  u32 player = M32(PLAYER);
  if (!player || !is_vtable(M32(player))) return;
  s->ok = 1;
  s->menu = rt_call_guest(c, MENU_MODE_FN, 0, NULL) & 0xFF;
  u32 av = player + 0xA4;
  if (is_vtable(M32(av))) {
    u32 code = AV_HEALTH;
    s->health = (int)vcall(c, av, 0x08, 1, &code);
    code = AV_HEALTH;
    s->max_health = (int)vcall(c, av, 0x1C, 1, &code);
  }
  u32 proc = M32(player + 0x68);
  if (proc && is_vtable(M32(proc))) {
    u32 info = vcall(c, proc, 0x148, 0, NULL);
    u32 weap = info ? M32(info + 8) : 0;
    if (weap && M8(weap + 4) == FORM_WEAPON) s->weapon_type = M8(weap + 0xF4);
    s->weapon_out = vcall(c, proc, 0x454, 0, NULL) & 0xFF;
  }
  s->in_game = s->max_health > 0 && M32(player + 0x40) != 0; /* in a cell: not the main menu's stand-in player */
}

/* ---- the light bar ---------------------------------------------------------------------------- */
static void light(const GameState *s, double t, uint8_t rgb[3]) {
  if (!s->ok || !s->in_game) { rgb[0] = 255; rgb[1] = 140; rgb[2] = 0; return; } /* New Vegas amber */
  float h = s->health <= 0 ? 0 : (float)s->health / s->max_health;
  if (h > 1) h = 1;
  float r, g, b = 0;
  if (h >= 0.6f) { float k = (h - 0.6f) / 0.4f; r = 255 * (1 - k); g = 150 + 105 * k; b = 40 * k; } /* amber .. green */
  else if (h >= 0.33f) { float k = (h - 0.33f) / 0.27f; r = 255; g = 150 * k; }                      /* red .. amber */
  else { r = 255; g = 0; }
  if (h < 0.33f && s->health > 0) { /* low: pulse */
    float p = 0.3f + 0.7f * (0.5f + 0.5f * (float)sin(t * 2 * M_PI * 1.4));
    r *= p; g *= p; b *= p;
  }
  rgb[0] = (uint8_t)r; rgb[1] = (uint8_t)g; rgb[2] = (uint8_t)b;
}

/* ---- the triggers --------------------------------------------------------------------------------- */
/* ScePadTriggerEffectParam: u8 mask (1 L2, 2 R2), 7 reserved, then per trigger {i32 mode, i32 reserved, u8 data[48]} */
enum { T_OFF, T_FEEDBACK, T_WEAPON, T_VIBRATION };
static void cmd(uint8_t *p, int mode, int a, int b, int c) {
  memset(p, 0, 56);
  p[0] = (uint8_t)mode;
  p[8] = (uint8_t)a; p[9] = (uint8_t)b; p[10] = (uint8_t)c;
}

static int is_gun(int type) { return type >= 3 && type <= 9; }

/* a strength given for Max (1-8), scaled by the Trigger Strength setting. Light is the gentlest the pad
   does (1) and each step about doubles: pistol 1/2/4/7, rifle 1/2/4/8, aim (L2) 1/2/3/5 */
static int str(int max) {
  static const float k[4] = {0.15f, 0.3f, 0.55f, 1.0f};
  int v = (int)(max * k[g_ds_strength] + 0.5f);
  return v < 1 ? 1 : v > 8 ? 8 : v;
}

static void triggers(const GameState *s, double t, uint8_t param[120]) {
  memset(param, 0, 120);
  param[0] = 1 | 2;
  uint8_t *l2 = param + 8, *r2 = param + 64;
  if (!s->ok || !s->in_game || s->menu || !s->weapon_out || !is_gun(s->weapon_type)) return; /* both off */
  int type = s->weapon_type;
  int firing = g_ds_rt > 100 && t - g_ds_rumble_time < 0.12;
  cmd(l2, T_FEEDBACK, 2, str(5), 0); /* aiming: an even push */
  switch (type) { /* weapon mode: resists from the start position, gives way at the end position */
  case 3: cmd(r2, T_WEAPON, 3, 6, str(7)); break;   /* pistol */
  case 4: cmd(r2, T_WEAPON, 3, 6, str(6)); break;   /* energy pistol */
  case 5: cmd(r2, T_WEAPON, 3, 7, str(8)); break;   /* rifle */
  case 6: cmd(r2, T_FEEDBACK, 2, str(6), 0); break; /* automatic */
  case 7: cmd(r2, T_WEAPON, 3, 6, str(7)); break;   /* energy rifle */
  case 8: cmd(r2, T_FEEDBACK, 1, str(8), 0); break; /* heavy weapon held by a handle */
  case 9: cmd(r2, T_WEAPON, 2, 7, str(8)); break;   /* launcher */
  }
  if (firing) { /* the kick */
    int heavy = type == 6 || type == 8;
    cmd(r2, T_VIBRATION, 2, str(8), heavy ? 30 : 45);
    static int said;
    if (said < 3) { said++; rt_log("dualsense: shot (weapon type %d): trigger kick", type); }
  }
}

/* each Present on the game's main thread */
void ds_frame(void) {
  Ctx *c = rt_current();
  if (!c || c->thread_index != 0) return; /* loading screens present from another thread: the game is busy */
  double t = now_s();
  static double last;
  if (t - last < 0.05) return;
  last = t;
  u32 eax = c->r[0], ecx = c->r[1], edx = c->r[2];
  GameState s;
  read_state(c, &s);
  c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx;

  static GameState prev = {.weapon_type = -2};
  if (s.ok != prev.ok || s.in_game != prev.in_game || s.menu != prev.menu || s.weapon_type != prev.weapon_type || s.weapon_out != prev.weapon_out ||
      (s.max_health != prev.max_health))
    rt_log("dualsense: %s, health %d/%d, weapon type %d %s%s", !s.ok ? "no player" : s.in_game ? "in game" : "not in the world", s.health, s.max_health,
           s.weapon_type, s.weapon_out ? "out" : "away", s.menu ? ", menu" : "");
  prev = s;

  static uint8_t last_rgb[3] = {1, 2, 3};
  static int light_on = -1;
  if (g_ds_lightbar) {
    uint8_t rgb[3];
    light(&s, t, rgb);
    if (memcmp(rgb, last_rgb, 3) || light_on != 1) { plat_pad_light(rgb[0], rgb[1], rgb[2]); memcpy(last_rgb, rgb, 3); light_on = 1; }
  } else if (light_on != 0) { plat_pad_light_reset(); light_on = 0; }

  static uint8_t last_param[120];
  static int sent;
  uint8_t param[120];
  triggers(&s, t, param);
  if (!g_ds_triggers) { memset(param, 0, 120); param[0] = 3; }
  if (!sent || memcmp(param, last_param, 120)) {
    plat_pad_triggers(param);
    memcpy(last_param, param, 120);
    sent = 1;
  }
}

/* ---- test hook: FNV_CONSOLE="140:player.moveto X;150:save Name" runs console commands at the given seconds after
   start, on the main thread at the frame's input poll (XInputGetState; inside Present a teleport deadlocks), the
   way the console does (0x71B210): a temporary Script (constructor 0x5AA0F0, then
   0x484490), its text (0x5ABE50), compiled and run by Script::CompileAndRun (0x5AC400) with the console manager's
   compiler (ConsoleManager 0x71B160, its first field) as the console (context 1), no reference selected, then
   destroyed (0x5AA1A0). Used to make test saves (a teleport, a level, a save) where typing would be slow. */
static void console_line(Ctx *c, const char *line) {
  u32 zero = 0;
  u32 cm = rt_call_guest(c, 0x71B160u, 1, &zero);
  if (!cm || !M32(cm)) { rt_log("console: no console manager yet"); return; }
  u32 script = heap_alloc(0x100, 1), text = guest_strdup(line);
  c->r[1] = script; rt_call_guest(c, 0x5AA0F0u, 0, NULL);
  c->r[1] = script; rt_call_guest(c, 0x484490u, 0, NULL);
  c->r[1] = script; rt_call_guest(c, 0x5ABE50u, 1, &text);
  u32 args[3] = {M32(cm), 1, 0};
  c->r[1] = script;
  u32 ok = rt_call_guest(c, 0x5AC400u, 3, args) & 0xFF;
  c->r[1] = script; rt_call_guest(c, 0x5AA1A0u, 0, NULL);
  heap_free(text);
  heap_free(script);
  rt_log("console: %s (%s)", line, ok ? "ran" : "returned 0");
}

void console_script(void) {
  static int n = -1, next;
  static struct { double t; char cmd[200]; } ev[32];
  if (n < 0) {
    n = 0;
    const char *e = getenv("FNV_CONSOLE");
    while (e && *e && n < 32) {
      char *end;
      ev[n].t = strtod(e, &end);
      if (*end != ':') break;
      e = end + 1;
      int k = 0;
      while (*e && *e != ';' && k < 199) ev[n].cmd[k++] = *e++;
      ev[n].cmd[k] = 0;
      n++;
      if (*e == ';') e++;
    }
  }
  if (next >= n) return;
  Ctx *c = rt_current();
  if (!c || c->thread_index != 0) return;
  static double t0;
  double t = now_s();
  if (!t0) t0 = t;
  if (t - t0 < ev[next].t) return;
  u32 eax = c->r[0], ecx = c->r[1], edx = c->r[2];
  console_line(c, ev[next].cmd);
  c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx;
  next++;
}
