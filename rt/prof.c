/* Built-in sampling profiler for the game's main thread (thread 0), so a log from the console says where its time
 * goes. A helper thread interrupts thread 0 about 200 times a second (SIGUSR1); the handler only stores the
 * interrupted program counter. Once a minute the samples are sorted into four groups and logged:
 *
 *   game code   the recompiled game (gen/, between the lowest and highest f_ function)
 *   port        the rest of the app before gen/ in the link: this runtime, including the Direct3D 9 layer
 *   libraries   what is linked after gen/: DXVK, RADV, SDL, FFmpeg, libc++ (on Linux: the shared libraries)
 *   system      the console's own modules (libkernel, libc): system calls and waiting
 *
 * with the hottest game functions by guest address, and the hottest places in the other groups as offsets from the
 * start of the app (on PS5 those are addresses in build-ps5/llvm-pie.elf; system ones are absolute). Groups are told
 * apart by address alone (no symbol lookup, which on the console fails for most addresses).
 * [fnv_recomp] iProfiler=0 in FalloutPrefs.ini turns it off. */
#define _GNU_SOURCE
#include "rt.h"
#include "loader.h"
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef __PROSPERO__
#include <ps5platform/context.h>
#else
#include <ucontext.h>
#endif

int rt_thread_handle(int t, pthread_t *out); /* main.c */
int rt_setting_int(const char *key, int def); /* firstrun.c */

#define RING 4096
static volatile u64 g_ring[RING];
static volatile u32 g_wr;

static void on_prof(int sig, siginfo_t *si, void *ucv) {
  (void)sig; (void)si;
  ucontext_t *uc = ucv;
#ifdef __PROSPERO__
  u64 pc = uc->uc_mcontext.mc_rip;
#else
  u64 pc = uc->uc_mcontext.gregs[REG_RIP];
#endif
  u32 w = g_wr;
  g_ring[w & (RING - 1)] = pc;
  __atomic_store_n(&g_wr, w + 1, __ATOMIC_RELEASE);
}

/* the f_ functions by host address, to name the guest function a sample fell in */
static struct fn { u64 host; u32 guest; } *g_fns;
static u32 g_nfns;
static u64 g_gen_lo, g_gen_hi;
static int cmp_fn(const void *a, const void *b) {
  u64 x = ((const struct fn *)a)->host, y = ((const struct fn *)b)->host;
  return x < y ? -1 : x > y;
}
static u32 guest_of(u64 pc) {
  u32 lo = 0, hi = g_nfns;
  while (hi - lo > 1) { u32 m = (lo + hi) / 2; if (g_fns[m].host <= pc) lo = m; else hi = m; }
  return g_fns[lo].guest;
}

/* small counting tables for the "top" lists */
#define TOPN 1024
typedef struct { u64 key; u32 n; } Slot;
static void count(Slot *t, u64 key) {
  for (u32 h = (u32)((key * 0x9E3779B97F4A7C15ull) >> 54) & (TOPN - 1), k = 0; k < TOPN; k++, h = (h + 1) & (TOPN - 1)) {
    if (t[h].key == key) { t[h].n++; return; }
    if (!t[h].n) { t[h].key = key; t[h].n = 1; return; }
  }
}
static int top(const Slot *t, int want, int *idx) { /* the want biggest counts, biggest first */
  int got = 0;
  for (int i = 0; i < TOPN; i++) {
    if (!t[i].n) continue;
    int j;
    if (got < want) j = got++;
    else if (t[i].n > t[idx[want - 1]].n) j = want - 1;
    else continue;
    idx[j] = i;
    while (j > 0 && t[idx[j - 1]].n < t[idx[j]].n) { int x = idx[j - 1]; idx[j - 1] = idx[j]; idx[j] = x; j--; }
  }
  return got;
}

/* the app's own image: on PS5 __dso_handle is its first byte and __init_array_start lies past its code; the
   Linux linker defines __executable_start and etext */
#ifdef __PROSPERO__
extern char __dso_handle[], __init_array_start[];
#define APP_LO ((u64)__dso_handle)
#define APP_HI ((u64)__init_array_start)
#else
extern char __executable_start[], etext[];
#define APP_LO ((u64)__executable_start)
#define APP_HI ((u64)etext)
#endif

static void log_top(const char *what, const Slot *t, u32 total, int game_names) {
  int idx[12], k = top(t, game_names ? 12 : 8, idx);
  char b[700];
  int o = snprintf(b, sizeof b, "prof: %s:", what);
  for (int i = 0; i < k && o < (int)sizeof b - 40; i++)
    o += snprintf(b + o, sizeof b - o, game_names ? " %08llx %.1f%%" : " %llx %.1f%%", (unsigned long long)t[idx[i]].key,
                  100.0 * t[idx[i]].n / total);
  rt_log("%s", b);
}

static void *prof_main(void *arg) {
  (void)arg;
  pthread_t main_th;
  while (!rt_thread_handle(0, &main_th)) { struct timespec d = {0, 50000000}; nanosleep(&d, NULL); }
  const u64 app_lo = APP_LO, app_hi = APP_HI;
  enum { GAME, PORT, LIBS, SYS, NGROUP };
  static const char *names[NGROUP] = {"game code", "port runtime", "DXVK/driver/libraries", "system (incl. waiting)"};
  static Slot game[TOPN], port[TOPN], libs[TOPN], sys[TOPN];
  u32 n[NGROUP] = {0}, total = 0, rd = __atomic_load_n(&g_wr, __ATOMIC_ACQUIRE);
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    struct timespec d = {0, 5000000};
    nanosleep(&d, NULL);
    pthread_kill(main_th, SIGUSR1);
    u32 wr = __atomic_load_n(&g_wr, __ATOMIC_ACQUIRE);
    if (wr - rd > RING) rd = wr - RING;
    for (; rd != wr; rd++) { /* by address only: nothing here can fault on an odd pc */
      u64 pc = g_ring[rd & (RING - 1)];
      total++;
      if (pc >= g_gen_lo && pc < g_gen_hi) { n[GAME]++; count(game, guest_of(pc)); }
      else if (pc >= app_lo && pc < g_gen_lo) { n[PORT]++; count(port, (pc - app_lo) & ~0x3Full); }
      else if (pc >= g_gen_hi && pc < app_hi) { n[LIBS]++; count(libs, (pc - app_lo) & ~0x3Full); }
      else { n[SYS]++; count(sys, pc & ~0x3Full); }
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
    if (el < 60 || !total) continue;
    char b[400];
    int o = snprintf(b, sizeof b, "prof: main thread, %.0f s, %u samples:", el, total);
    for (int g = 0; g < NGROUP; g++) o += snprintf(b + o, sizeof b - o, "%s %s %.1f%%", g ? "," : "", names[g], 100.0 * n[g] / total);
    rt_log("%s", b);
    log_top("game code top (guest function)", game, total, 1);
    log_top("port top (offset in app)", port, total, 0);
    log_top("libraries top (offset in app)", libs, total, 0);
    log_top("system top (address)", sys, total, 0);
    memset(game, 0, sizeof game); memset(port, 0, sizeof port); memset(libs, 0, sizeof libs); memset(sys, 0, sizeof sys);
    memset(n, 0, sizeof n); total = 0; t0 = t;
  }
  return NULL;
}

void rt_prof_start(void) {
  if (getenv("FNV_PROF") ? !atoi(getenv("FNV_PROF")) : !rt_setting_int("iProfiler", 1)) return;
  g_nfns = g_func_count;
  g_fns = malloc(g_nfns * sizeof *g_fns);
  if (!g_fns || !g_nfns) return;
  for (u32 i = 0; i < g_nfns; i++) { g_fns[i].host = (u64)g_func_ptr[i]; g_fns[i].guest = g_func_addr[i]; }
  qsort(g_fns, g_nfns, sizeof *g_fns, cmp_fn);
  g_gen_lo = g_fns[0].host;
  g_gen_hi = g_fns[g_nfns - 1].host + 0x1000; /* the last function (a short one): code after it is not the game's */
  struct sigaction sa = {0};
  sa.sa_sigaction = on_prof;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGUSR1, &sa, NULL);
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 << 10);
  if (pthread_create(&th, &at, prof_main, NULL) == 0) pthread_detach(th);
  pthread_attr_destroy(&at);
  rt_log("profiler: sampling the main thread about 200 times a second; a summary every minute (iProfiler=0 turns it off)");
}
