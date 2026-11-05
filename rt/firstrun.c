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
  if (access(prefs, F_OK) != 0) {
    const char *preset = getenv("FNV_PRESET") ? getenv("FNV_PRESET") : "medium";
    snprintf(src, sizeof src, "%s/%s.ini", g_game_dir, preset);
    char *b = slurp(src, &n);
    if (!b) b = strdup("[Display]\r\n");
    b = ini_set(b, "Display", "sD3DDevice", "\"NVIDIA GeForce 8800 GT (fnv_recomp)\"");
    b = ini_set(b, "Display", "iSize W", "1280");
    b = ini_set(b, "Display", "iSize H", "720");
    b = ini_set(b, "Display", "bFull Screen", "0");
    FILE *f = fopen(prefs, "wb"); if (f) { fputs(b, f); fclose(f); }
    free(b);
    rt_log("first run: created FalloutPrefs.ini from %s.ini", preset);
  }
}
