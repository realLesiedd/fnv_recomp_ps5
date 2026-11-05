/* API registry, handle table and the virtual filesystem shared by the Win32 layer. */
#define _GNU_SOURCE
#include "rt.h"
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- registry of implemented functions ------------------------------------- */
#define MAX_LISTS 64
static const ApiEntry *g_lists[MAX_LISTS];
static int g_nlists;

void api_register(const ApiEntry *list) { g_lists[g_nlists++] = list; }

HostFn api_lookup(const char *dll, const char *name) {
  for (int i = 0; i < g_nlists; i++)
    for (const ApiEntry *e = g_lists[i]; e->dll; e++)
      if (!strcasecmp(e->dll, dll) && !strcmp(e->name, name)) return e->fn;
  return NULL;
}

u32 rt_get_proc(const char *dll, const char *name) {
  HostFn f = api_lookup(dll, name);
  if (!f) return 0;
  char *nm = malloc(strlen(dll) + strlen(name) + 2);
  sprintf(nm, "%s!%s", dll, name);
  /* the same (fn, name) pair is deduplicated by pointer, so keep one string per lookup site */
  for (int i = 0; i < g_nlists; i++)
    for (const ApiEntry *e = g_lists[i]; e->dll; e++)
      if (e->fn == f) { free(nm); return rt_thunk(f, e->name); }
  return rt_thunk(f, nm);
}

extern void kernel32_init(void), user32_init(void), misc_init(void), dinput_init(void);
void api_init_all(void) {
  kernel32_init();
  user32_init();
  misc_init();
  dinput_init();
}

/* ---- handles --------------------------------------------------------------- */
#define MAX_H 16384
static HObj *g_h[MAX_H];
static pthread_mutex_t g_h_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_wait_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_wait_cond = PTHREAD_COND_INITIALIZER;

u32 h_new(int type) {
  pthread_mutex_lock(&g_h_lock);
  for (int i = 1; i < MAX_H; i++)
    if (!g_h[i]) {
      HObj *o = calloc(1, sizeof(HObj));
      o->type = type; o->refs = 1; o->fd = -1;
      pthread_cond_init(&o->resume, NULL);
      g_h[i] = o;
      pthread_mutex_unlock(&g_h_lock);
      return 0x1000 + i * 4;
    }
  pthread_mutex_unlock(&g_h_lock);
  rt_fatal(NULL, "out of handles");
}

HObj *h_get(u32 h) {
  if (h < 0x1000) return NULL;
  u32 i = (h - 0x1000) / 4;
  if (i >= MAX_H) return NULL;
  return g_h[i];
}

void h_close(u32 h) {
  pthread_mutex_lock(&g_h_lock);
  u32 i = (h - 0x1000) / 4;
  if (h >= 0x1000 && i < MAX_H && g_h[i]) {
    HObj *o = g_h[i];
    if (o->type == H_FILE && o->fd >= 0) close(o->fd);
    if (o->type == H_FIND && o->dir) closedir(o->dir);
    free(o->pattern);
    free(o->dirpath);
    /* thread objects stay alive while the thread runs; it holds its own pointer */
    if (o->type != H_THREAD || o->finished) free(o);
    g_h[i] = NULL;
  }
  pthread_mutex_unlock(&g_h_lock);
}

void sync_signal_all(void) { pthread_cond_broadcast(&g_wait_cond); }

/* ---- virtual filesystem ---------------------------------------------------- */
/* C:\Game\...  -> the game folder (FNV_GAME_DIR)
 * C:\Users\Player\... -> FNV_USER_DIR
 * relative paths -> the game folder (the game's working directory)
 * names are matched without regard to case, like on Windows */
char g_game_dir[1024];
char g_user_dir[1024];
const char *fs_game_win_dir(void) { return "C:\\Game"; }

static void resolve_case(char *path) {
  /* path is absolute host path; fix each component that does not exist as spelled */
  char out[2048] = "";
  char *save, *tok;
  char tmp[2048];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
    if (!strcmp(tok, ".")) continue;
    if (!strcmp(tok, "..")) { char *s = strrchr(out, '/'); if (s) *s = 0; continue; }
    char cand[2048];
    snprintf(cand, sizeof cand, "%s/%s", out, tok);
    struct stat st;
    if (stat(cand, &st) != 0) {
      DIR *d = opendir(out[0] ? out : "/");
      if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
          if (!strcasecmp(e->d_name, tok)) { snprintf(cand, sizeof cand, "%s/%s", out, e->d_name); break; }
        closedir(d);
      }
    }
    snprintf(out, sizeof out, "%s", cand);
  }
  strcpy(path, out[0] ? out : "/");
}

int fs_host_path(const char *w, char *out, size_t n) {
  char p[2048];
  snprintf(p, sizeof p, "%s", w);
  for (char *q = p; *q; q++) if (*q == '\\') *q = '/';
  const char *rest = p;
  char buf[2048];
  if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') {
    rest = p + 2;
    if (!strncasecmp(rest, "/Game", 5) && (rest[5] == '/' || !rest[5]))
      snprintf(buf, sizeof buf, "%s/%s", g_game_dir, rest + 5);
    else if (!strncasecmp(rest, "/Users/Player", 13) && (rest[13] == '/' || !rest[13]))
      snprintf(buf, sizeof buf, "%s/%s", g_user_dir, rest + 13);
    else
      snprintf(buf, sizeof buf, "%s/drive_%c/%s", g_user_dir, p[0] | 32, rest);
  } else if (p[0] == '/') {
    snprintf(buf, sizeof buf, "%s/%s", g_game_dir, p);
  } else {
    snprintf(buf, sizeof buf, "%s/%s", g_game_dir, p);
  }
  resolve_case(buf);
  snprintf(out, n, "%s", buf);
  return 0;
}
