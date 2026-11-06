/* First-run setup normally done by FalloutNVLauncher.exe: copy Fallout_default.ini to
 * My Games\FalloutNV\Fallout.ini and build FalloutPrefs.ini from a quality preset,
 * naming our adapter so the game does not hand off to the launcher. */
#define _GNU_SOURCE
#include "rt.h"
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char *slurp(const char *p, long *n) {
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc(*n + 1); *n = fread(b, 1, *n, f); b[*n] = 0; fclose(f);
  return b;
}

/* set key=value inside [section] of an INI held in a growable buffer */
static char *ini_set(char *s, const char *sect, const char *key, const char *val) {
  char hdr[128]; snprintf(hdr, sizeof hdr, "[%s]", sect);
  char *sec = rt_strcasestr(s, hdr);
  char line[512]; snprintf(line, sizeof line, "%s=%s", key, val);
  size_t n = strlen(s);
  if (!sec) {
    char *o = malloc(n + strlen(hdr) + strlen(line) + 8);
    sprintf(o, "%s\r\n%s\r\n%s\r\n", s, hdr, line);
    free(s); return o;
  }
  char *end = strchr(sec + 1, '['); if (!end) end = s + n;
  size_t kl = strlen(key);
  for (char *p = strchr(sec, '\n'); p && p < end; p = strchr(p + 1, '\n')) {
    char *l = p + 1;
    if (!strncasecmp(l, key, kl) && l[kl] == '=') {
      char *le = strpbrk(l, "\r\n"); if (!le) le = s + n;
      char *o = malloc(n + strlen(line) + 1);
      size_t pre = l - s;
      memcpy(o, s, pre); strcpy(o + pre, line); strcat(o, le);
      free(s); return o;
    }
  }
  char *after = strchr(sec, '\n'); after = after ? after + 1 : s + n;
  char *o = malloc(n + strlen(line) + 4);
  size_t pre = after - s;
  memcpy(o, s, pre); o[pre] = 0; strcat(o, line); strcat(o, "\r\n"); strcat(o, after);
  free(s); return o;
}

static void mkdirs(const char *path) {
  char p[1200]; snprintf(p, sizeof p, "%s", path);
  for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
  mkdir(p, 0755);
}

/* every key=value of the preset file src into the INI buffer b */
static char *apply_preset(char *b, const char *src) {
  long n;
  char *pre = slurp(src, &n);
  if (!pre) { rt_log("settings: %s is missing", src); return b; }
  char sect[128] = "";
  for (char *l = strtok(pre, "\r\n"); l; l = strtok(NULL, "\r\n")) {
    if (l[0] == '[') { char *e = strchr(l, ']'); if (e) { *e = 0; snprintf(sect, sizeof sect, "%s", l + 1); } continue; }
    char *eq = strchr(l, '=');
    if (!eq || !sect[0] || l[0] == ';') continue;
    *eq = 0;
    b = ini_set(b, sect, l, eq + 1);
  }
  free(pre);
  return b;
}

/* Quality: the game's own VeryHigh preset (full-resolution textures, 15x anisotropic filtering, the longest object,
   tree, terrain and grass distances, high shadows and water) without its multisampling, which this port does not
   render. Applied to an existing FalloutPrefs.ini once per SETTINGS_VERSION, so changes made later in the game's
   settings menu stay. */
#define SETTINGS_VERSION "3" /* 3: 4K, chosen once; Settings > Display > Resolution changes it (resolution.c) */
static char *quality(char *b, const char *game_dir) {
  char src[1200];
  snprintf(src, sizeof src, "%s/VeryHigh.ini", game_dir);
  b = apply_preset(b, src);
  b = ini_set(b, "Display", "iSize W", "3840");
  b = ini_set(b, "Display", "iSize H", "2160");
  b = ini_set(b, "Display", "iMultiSample", "1");
  b = ini_set(b, "Display", "iWaterMultisamples", "1");
  b = ini_set(b, "Display", "bTransparencyMultisampling", "0");
  b = ini_set(b, "fnv_recomp", "iSettingsVersion", SETTINGS_VERSION);
  return b;
}

/* The port's own settings, in FalloutPrefs.ini [fnv_recomp] (the game ignores the section). Each is written with
   its default when missing, so it can be found and changed there. */
static const struct { const char *key, *def; } g_own[] = {
  {"iPerfOverlay", "0"},       /* performance overlay: 0 off, 1 basic, 2 detailed (osd.c; Settings > Display) */
  {"iFrameCap", "60"},         /* frame-rate cap: 30 or 60 (osd.c; Settings > Display) */
  {"iAdaptiveTriggers", "1"},  /* DualSense adaptive triggers: 0 off, 1 on (dualsense.c; Settings > Controls) */
  {"iTriggerStrength", "2"},   /* adaptive trigger strength: 0 light, 1 medium, 2 strong, 3 max (dualsense.c) */
  {"iLightBar", "1"},          /* DualSense light bar shows health: 0 off, 1 on (dualsense.c; Settings > Controls) */
  {"iFastMath", "1"},          /* native versions of the game's hottest maths routines: 0 runs the originals (fastmath.c) */
  {"iProfiler", "1"},          /* main-thread profile in fnv.log once a minute: 0 off (prof.c) */
};

static void prefs_path(char *out, size_t n) {
  snprintf(out, n, "%s/Documents/My Games/FalloutNV/FalloutPrefs.ini", g_user_dir);
}

/* an integer setting from [fnv_recomp], or def */
int rt_setting_int(const char *key, int def) {
  char path[1200]; long n;
  prefs_path(path, sizeof path);
  char *b = slurp(path, &n);
  if (!b) return def;
  int v = def;
  char *sec = rt_strcasestr(b, "[fnv_recomp]");
  if (sec) {
    char *end = strchr(sec + 1, '['); if (!end) end = b + n;
    size_t kl = strlen(key);
    for (char *p = strchr(sec, '\n'); p && p < end; p = strchr(p + 1, '\n'))
      if (!strncasecmp(p + 1, key, kl) && p[1 + kl] == '=') { v = atoi(p + 2 + kl); break; }
  }
  free(b);
  return v;
}

/* set key=value in [sect] of FalloutPrefs.ini */
void rt_prefs_set(const char *sect, const char *key, const char *val) {
  char path[1200]; long n;
  prefs_path(path, sizeof path);
  char *b = slurp(path, &n);
  if (!b) return;
  b = ini_set(b, sect, key, val);
  FILE *f = fopen(path, "wb"); if (f) { fputs(b, f); fclose(f); }
  free(b);
}

static void own_settings(void) {
  char path[1200]; long n;
  prefs_path(path, sizeof path);
  char *b = slurp(path, &n);
  if (!b) return;
  int changed = 0;
  for (unsigned i = 0; i < sizeof g_own / sizeof *g_own; i++) {
    char k[128]; snprintf(k, sizeof k, "%s=", g_own[i].key);
    if (!rt_strcasestr(b, k)) { b = ini_set(b, "fnv_recomp", g_own[i].key, g_own[i].def); changed = 1; }
  }
  if (changed) { FILE *f = fopen(path, "wb"); if (f) { fputs(b, f); fclose(f); } }
  free(b);
}

/* Game settings the port keeps at a fixed value on every launch, whatever Fallout.ini says.
   bUseMultiThreadedFaceGen: with FaceGen on the task thread, loading a save can deadlock in three game threads:
   the main thread builds a face (GetOrCreateFaceGenModel) holding one lock (0x11C5CF0) and waits for the FaceGen
   lock (0x11D5A9C); BSTaskManagerThread holds that and waits for a spinlock while freeing memory; and
   BackgroundCloneThread holds the spinlock and waits for the first lock. Reproduced 3 of 3 with a user's Primm save;
   with this off the same save loaded every time. */
static const struct { const char *sect, *key, *val; } g_fixed[] = {
  {"BackgroundLoad", "bUseMultiThreadedFaceGen", "0"},
};
static void fixed_settings(const char *ini) {
  long n;
  char *b = slurp(ini, &n);
  if (!b) return;
  int changed = 0;
  for (size_t i = 0; i < sizeof g_fixed / sizeof *g_fixed; i++) {
    char want[200];
    snprintf(want, sizeof want, "%s=%s", g_fixed[i].key, g_fixed[i].val);
    if (rt_strcasestr(b, want)) continue;
    b = ini_set(b, g_fixed[i].sect, g_fixed[i].key, g_fixed[i].val);
    rt_log("settings: Fallout.ini [%s] %s set to %s", g_fixed[i].sect, g_fixed[i].key, g_fixed[i].val);
    changed = 1;
  }
  if (changed) { FILE *f = fopen(ini, "wb"); if (f) { fputs(b, f); fclose(f); } }
  free(b);
}

void rt_first_run(void) {
  char dir[1100], ini[1200], prefs[1200], src[1200];
  snprintf(dir, sizeof dir, "%s/Documents/My Games/FalloutNV", g_user_dir);
  snprintf(ini, sizeof ini, "%s/Fallout.ini", dir);
  snprintf(prefs, sizeof prefs, "%s/FalloutPrefs.ini", dir);
  mkdirs(dir);
  long n;
  if (access(ini, F_OK) != 0) {
    snprintf(src, sizeof src, "%s/Fallout_default.ini", g_game_dir);
    char *b = slurp(src, &n);
    if (b) { FILE *f = fopen(ini, "wb"); if (f) { fwrite(b, 1, n, f); fclose(f); } free(b); rt_log("first run: created Fallout.ini"); }
    else rt_log("first run: %s is missing - is the game folder complete?", src);
  }
  fixed_settings(ini);
  if (access(prefs, F_OK) != 0) {
    const char *preset = getenv("FNV_PRESET") ? getenv("FNV_PRESET") : "VeryHigh";
    snprintf(src, sizeof src, "%s/%s.ini", g_game_dir, preset);
    char *b = slurp(src, &n);
    if (!b) b = strdup("[Display]\r\n");
    if (!getenv("FNV_PRESET")) b = quality(b, g_game_dir);
    b = ini_set(b, "Display", "sD3DDevice", "\"NVIDIA GeForce 8800 GT (fnv_recomp)\"");
    if (getenv("FNV_PRESET")) { /* tests: the old 720p */
      b = ini_set(b, "Display", "iSize W", "1280");
      b = ini_set(b, "Display", "iSize H", "720");
    }
    b = ini_set(b, "Display", "bFull Screen", "0");
    FILE *f = fopen(prefs, "wb"); if (f) { fputs(b, f); fclose(f); }
    free(b);
    rt_log("first run: created FalloutPrefs.ini from %s.ini", preset);
  } else if (!getenv("FNV_PRESET")) {
    char *b = slurp(prefs, &n);
    char *m = b ? rt_strcasestr(b, "iSettingsVersion=") : NULL;
    if (b && (!m || strncmp(m + 17, SETTINGS_VERSION, strlen(SETTINGS_VERSION)) != 0)) {
      b = quality(b, g_game_dir);
      FILE *f = fopen(prefs, "wb"); if (f) { fputs(b, f); fclose(f); }
      rt_log("settings: FalloutPrefs.ini raised to the VeryHigh preset (textures, filtering, draw distances) at 3840x2160");
    }
    free(b);
  }
  own_settings();
}
