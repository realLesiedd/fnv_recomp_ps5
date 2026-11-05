/* API registry, handle table and the virtual filesystem shared by the Win32 layer. */
#define _GNU_SOURCE
#include "rt.h"
#include <string.h>
#include <ctype.h>
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

extern void kernel32_init(void), user32_init(void), misc_init(void), dinput_init(void), d3d9_init(void), d3dx_init(void),
    vorbisfile_init(void), dsound_init(void), mmio_init(void), bink_init(void);
void api_init_all(void) {
  kernel32_init();
  user32_init();
  misc_init();
  bink_init();
  dinput_init();
  d3d9_init();
  d3dx_init();
  vorbisfile_init();
  dsound_init();
  mmio_init();
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

/* ---- portable helpers -------------------------------------------------------- */
char *rt_strcasestr(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; *hay; hay++)
    if (!strncasecmp(hay, needle, n)) return (char *)hay;
  return n ? NULL : (char *)hay;
}

int rt_wildmatch(const char *p, const char *s) {
  const char *star = NULL, *ss = NULL;
  while (*s) {
    if (*p == '*') { star = p++; ss = s; continue; }
    if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) { p++; s++; continue; }
    if (star) { p = star + 1; s = ++ss; continue; }
    return 0;
  }
  while (*p == '*') p++;
  return *p == 0;
}

const char *rt_realpath(const char *p, char *buf, size_t n) {
#ifdef __PROSPERO__
  snprintf(buf, n, "%s", p); /* PS5 paths are absolute already */
#else
  char *r = realpath(p, NULL);
  snprintf(buf, n, "%s", r ? r : p);
  free(r);
#endif
  return buf;
}

#ifdef __PROSPERO__
#include <signal.h>
#include <unistd.h>
int sceKernelLoadStartModule(const char *, size_t, const void *, unsigned, void *, int *);
int sceKernelDlsym(int, const char *, void **);
int sceKernelUsleep(unsigned);
/* Close the app the way FileDen does: ask the system to exit it (sceSystemServiceLoadExec("exit")), which goes
 * back to the home screen without an error report; if that does not take, kill the process. */
static void ps5_close_app(void) {
  static const char *mods[] = {"libSceSystemService.sprx", "/system/common/lib/libSceSystemService.sprx", 0};
  int (*load_exec)(const char *, const char **) = 0;
  for (int i = 0; mods[i] && !load_exec; i++) {
    int h = sceKernelLoadStartModule(mods[i], 0, 0, 0, 0, 0);
    void *f = 0;
    if (h >= 0 && !sceKernelDlsym(h, "sceSystemServiceLoadExec", &f) && f) load_exec = (int (*)(const char *, const char **))f;
  }
  if (load_exec) {
    int r = load_exec("exit", 0);
    for (int i = 0; r == 0 && i < 15; i++) sceKernelUsleep(100000);
  }
  kill(getpid(), SIGKILL);
}
#endif
void rt_exit(int code) {
  rt_log("closing (exit code %d)", code);
  fflush(NULL);
#ifdef __PROSPERO__
  ps5_close_app();
  exit(code); /* only if the system did not close the app; _exit's direct syscall is refused (SIGSYS) */
#else
  _exit(code);
#endif
}
