/* Loader and core runtime: guest address space, image mapping, dispatch, threads. */
#define _GNU_SOURCE
#include "rt.h"
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#ifdef __linux__
#include <execinfo.h>
#endif
#include <dlfcn.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#include <pthread.h>
#ifdef __PROSPERO__
#include <ps5platform/context.h> /* the console's ucontext layout (registers for the stall sampler) */
#include <ps5platform/kernel.h>  /* sceKernelAvailableFlexibleMemorySize, for the perf line */
#else
#include <ucontext.h>
#endif

#define FNV_RELEASE "v19" /* printed at the top of the log so a log shows which build produced it */

int g_trace;
u32 g_peb;
static __thread Ctx *t_ctx;
static __thread const char *t_last_api;
#define MAXTHR 64
static const char *volatile g_in_api[MAXTHR];
static volatile double g_in_api_since[MAXTHR];
/* stall diagnostics: what every thread last asked of the system, and from where in the game */
static const char *volatile g_last_api[MAXTHR];
static volatile u32 g_last_ret[MAXTHR];
static volatile u64 g_api_calls[MAXTHR];
static pthread_t g_pth[MAXTHR];
/* refreshed once a second by the heartbeat: system calls note when they started without reading the clock */
static volatile double g_coarse_now;
static volatile int g_pth_set[MAXTHR];
static double mono_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
u64 g_stat_valloc, g_stat_opens, g_stat_read;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void rt_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&g_log_lock);
  Ctx *c = t_ctx;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  static double t0;
  double now = ts.tv_sec + ts.tv_nsec / 1e9;
  if (t0 == 0) t0 = now;
  fprintf(stderr, "[%8.3f t%d] ", now - t0, c ? c->thread_index : -1);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  pthread_mutex_unlock(&g_log_lock);
  va_end(ap);
}

static void dump_backtrace(void) {
#ifdef __linux__
  void *bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
#else
  /* frame-pointer walk (the PS5 build keeps frame pointers); dladdr names exported f_XXXXXXXX symbols */
  void **fp = __builtin_frame_address(0);
  for (int i = 0; i < 48 && fp && ((u64)fp & 7) == 0; i++) {
    void *ret = fp[1];
    if (!ret) break;
    Dl_info di;
    if (dladdr(ret, &di) && di.dli_sname) fprintf(stderr, "  %s+0x%lx\n", di.dli_sname, (long)((char *)ret - (char *)di.dli_saddr));
    else fprintf(stderr, "  %p\n", ret);
    void **next = fp[0];
    if (next <= fp) break;
    fp = next;
  }
#endif
}

void rt_fatal(Ctx *c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&g_log_lock);
  fprintf(stderr, "\n*** FATAL [t%d]: ", c ? c->thread_index : -1);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  if (c) {
    fprintf(stderr, "    eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->r[0], c->r[1],
            c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    for (int k = 0; k < 8; k++) {
      const char *tn = c->r[k] >= THUNK_BASE ? rt_thunk_name(c->r[k]) : NULL;
      if (tn) fprintf(stderr, "    register %d holds the address of %s\n", k, tn);
    }
    fprintf(stderr, "    last API called on this thread: %s\n", t_last_api ? t_last_api : "(none)");
  }
  fprintf(stderr, "    host backtrace (f_XXXXXXXX = guest function at that address; rt_fatal is at %p):\n", (void *)&rt_fatal);
  dump_backtrace();
  fflush(stderr);
  pthread_mutex_unlock(&g_log_lock);
  va_end(ap);
#ifdef __PROSPERO__
  /* hand the trap to the console's own crash report, not back to on_fault */
  signal(SIGILL, SIG_DFL); signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL); signal(SIGFPE, SIG_DFL);
  __builtin_trap();
#endif
  _exit(3);
}

/* ---- thunks ---------------------------------------------------------------- */
static HostFn g_thunk_fn[THUNK_MAX];
static const char *g_thunk_name[THUNK_MAX];
static int g_thunk_count;
static u64 g_thunk_calls[THUNK_MAX]; /* per host function, for FNV_API_STATS */
static pthread_mutex_t g_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

u32 rt_thunk(HostFn f, const char *name) {
  pthread_mutex_lock(&g_thunk_lock);
  for (int i = 0; i < g_thunk_count; i++)
    if (g_thunk_fn[i] == f && g_thunk_name[i] == name) { pthread_mutex_unlock(&g_thunk_lock); return THUNK_BASE + i * 16; }
  int i = g_thunk_count++;
  if (i >= THUNK_MAX) rt_fatal(NULL, "out of thunks");
  g_thunk_fn[i] = f;
  g_thunk_name[i] = name;
  pthread_mutex_unlock(&g_thunk_lock);
  return THUNK_BASE + i * 16;
}

const char *rt_thunk_name(u32 a) {
  if (a < THUNK_BASE) return NULL;
  u32 i = (a - THUNK_BASE) / 16;
  return i < (u32)g_thunk_count ? g_thunk_name[i] : NULL;
}

/* ---- dispatch -------------------------------------------------------------- */
GuestFn rt_find_guest(u32 a) {
  u32 lo = 0, hi = g_func_count;
  while (lo < hi) {
    u32 mid = (lo + hi) / 2;
    if (g_func_addr[mid] < a) lo = mid + 1; else hi = mid;
  }
  if (lo < g_func_count && g_func_addr[lo] == a) return g_func_ptr[lo];
  return NULL;
}

#define DCACHE 4096
static __thread struct { u32 a; GuestFn f; } g_dcache[DCACHE]; /* per thread: no torn reads */

void rt_call_ind(Ctx *restrict c, u32 t) {
  if (t >= THUNK_BASE) {
    u32 i = (t - THUNK_BASE) / 16;
    if (i < (u32)g_thunk_count && (t & 15) == 0) {
      if (g_trace > 1) rt_log("-> %s (ret %08x)", g_thunk_name[i], M32(c->r[4]));
      t_last_api = g_thunk_name[i];
      int ti = c->thread_index & (MAXTHR - 1);
      g_last_api[ti] = g_thunk_name[i];
      g_last_ret[ti] = M32(c->r[4]);
      g_api_calls[ti]++;
      g_thunk_calls[i]++;
      const char *outer = g_in_api[ti];
      double outer_since = g_in_api_since[ti];
      if (!outer) { g_in_api_since[ti] = g_coarse_now; g_in_api[ti] = g_thunk_name[i]; }
      if (!g_thunk_fn[i]) rt_fatal(c, "call to %s: no host implementation", g_thunk_name[i]);
      g_thunk_fn[i](c);
      g_in_api[ti] = outer; g_in_api_since[ti] = outer_since; /* host calls can nest (window procs) */
      return;
    }
  }
  u32 h = (t >> 2) & (DCACHE - 1);
  GuestFn f;
  if (g_dcache[h].a == t && g_dcache[h].f) f = g_dcache[h].f;
  else {
    f = rt_find_guest(t);
    if (!f) rt_fatal(c, "call/jump to %08x: no recompiled code there (return address %08x)", t, M32(c->r[4]));
    g_dcache[h].a = t; g_dcache[h].f = f;
  }
  f(c);
}

static u32 g_host_return;
static void host_return_marker(Ctx *c) { rt_fatal(c, "returned into host marker"); }

u32 rt_call_guest(Ctx *c, u32 addr, int nargs, const u32 *args) {
  u32 saved = c->r[4];
  for (int k = nargs - 1; k >= 0; k--) { c->r[4] -= 4; M32(c->r[4]) = args[k]; }
  c->r[4] -= 4;
  M32(c->r[4]) = g_host_return;
  u32 save_regs[3] = {c->r[3], c->r[6], c->r[7]};
  u32 save_ebp = c->r[5];
  rt_call_ind(c, addr);
  c->r[4] = saved;
  c->r[3] = save_regs[0]; c->r[6] = save_regs[1]; c->r[7] = save_regs[2]; c->r[5] = save_ebp;
  return c->r[0];
}

void rt_unimpl(Ctx *restrict c, u32 addr, const char *what) {
  rt_fatal(c, "unsupported instruction at %08x: %s", addr, what);
}
void rt_int3(Ctx *restrict c, u32 addr) {
  rt_log("int3 at %08x (debug break) - ignored", addr);
}
void rt_cpuid(Ctx *restrict c) {
  u32 leaf = c->r[0];
  switch (leaf) {
  case 0: c->r[0] = 1; c->r[3] = 0x756e6547; c->r[2] = 0x49656e69; c->r[1] = 0x6c65746e; break; /* GenuineIntel */
  case 1:
    c->r[0] = 0x000006f6; c->r[3] = 0x00010800;
    c->r[1] = 0;            /* no SSE3+ */
    c->r[2] = 0x02008001 | (1u << 23) | (1u << 15) | (1u << 4); /* fpu tsc cmov mmx sse (no sse2: CRT keeps x87 paths) */
    break;
  default: c->r[0] = c->r[1] = c->r[2] = c->r[3] = 0;
  }
}
u64 rt_rdtsc(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 3000000000ull + (u64)ts.tv_nsec * 3;
}
u32 rt_jmp_ind_target(Ctx *restrict c, u32 t) { return t; }

/* ---- contexts, TEB --------------------------------------------------------- */
static int g_thread_counter;
Ctx *rt_current(void) { return t_ctx; }

Ctx *rt_new_ctx(u32 stack_size) {
  Ctx *c = calloc(1, sizeof(Ctx));
  c->fcw = 0x27f; /* MSVC default: 53-bit precision, round to nearest, exceptions masked */
  c->mxcsr = 0x1f80;
  c->thread_index = __atomic_fetch_add(&g_thread_counter, 1, __ATOMIC_SEQ_CST);
  u32 stack = vm_alloc(0, stack_size, 0x3000, 4);
  if (!stack) rt_fatal(NULL, "no memory for a guest stack");
  c->r[4] = stack + stack_size - 16;
  c->fs_base = rt_teb_alloc(c, stack, stack + stack_size, 0x1000 + c->thread_index * 4);
  return c;
}

/* FNV_STACK_STATS (Linux test runs): each thread paints its unused stack at start; the heartbeat reports the
   deepest point any thread has reached, to size host stacks with a known margin */
static u8 *volatile g_stk_lo[MAXTHR], *volatile g_stk_hi[MAXTHR];
static int stack_stats(void) { static int on = -1; if (on < 0) on = getenv("FNV_STACK_STATS") != NULL; return on; }
static void stack_paint(int ti) {
#ifdef __linux__
  pthread_attr_t a;
  void *base;
  size_t size;
  if (pthread_getattr_np(pthread_self(), &a)) return;
  pthread_attr_getstack(&a, &base, &size);
  pthread_attr_destroy(&a);
  u8 *lo = (u8 *)base + 64 * 1024, *sp = (u8 *)__builtin_frame_address(0) - 16 * 1024; /* skip guard, stay off our frame */
  if (sp > lo) memset(lo, 0xA5, sp - lo);
  g_stk_lo[ti] = lo;
  g_stk_hi[ti] = (u8 *)base + size;
#else
  (void)ti;
#endif
}
static void stack_report(void) {
  char b[700];
  int o = snprintf(b, sizeof b, "stack use (KB, peak):");
  for (int t = 0; t < MAXTHR && o < (int)sizeof b - 24; t++) {
    u8 *lo = g_stk_lo[t], *hi = g_stk_hi[t];
    if (!lo) continue;
    u8 *p = lo;
    while (p < hi && *p == 0xA5) p++;
    o += snprintf(b + o, sizeof b - o, " t%d:%ld", t, (long)((hi - p) >> 10));
  }
  rt_log("%s", b);
}
void rt_bind_thread(Ctx *c) {
  t_ctx = c;
  int ti = c->thread_index & (MAXTHR - 1);
  g_pth[ti] = pthread_self();
  g_pth_set[ti] = 1;
  if (stack_stats()) stack_paint(ti);
}

/* every TEB ever created (they are never freed), so TLS slots can be cleared in all threads */
#define MAX_TEBS 4096
static u32 g_tebs[MAX_TEBS];
static int g_nteb;
static pthread_mutex_t g_teb_lock = PTHREAD_MUTEX_INITIALIZER;
void rt_tls_clear_all(u32 slot) {
  pthread_mutex_lock(&g_teb_lock);
  for (int i = 0; i < g_nteb; i++) M32(g_tebs[i] + 0xE10 + 4 * slot) = 0;
  pthread_mutex_unlock(&g_teb_lock);
}

u32 rt_teb_alloc(Ctx *c, u32 stack_lo, u32 stack_hi, u32 tid) {
  u32 teb = vm_alloc(0, 0x2000, 0x3000, 4);
  pthread_mutex_lock(&g_teb_lock);
  if (g_nteb < MAX_TEBS) g_tebs[g_nteb++] = teb;
  pthread_mutex_unlock(&g_teb_lock);
  M32(teb + 0x00) = 0xffffffff; /* SEH chain end */
  M32(teb + 0x04) = stack_hi;
  M32(teb + 0x08) = stack_lo;
  M32(teb + 0x18) = teb;
  M32(teb + 0x20) = 0x100; /* pid */
  M32(teb + 0x24) = tid;
  M32(teb + 0x30) = g_peb;
  /* implicit TLS (__declspec(thread)): one module, index 0 */
  if (g_tls_end > g_tls_start) {
    u32 sz = g_tls_end - g_tls_start;
    u32 block = heap_alloc(sz + 16, 1);
    memcpy(HP(block), HP(g_tls_start), sz);
    u32 arr = teb + 0x1800;
    M32(arr) = block;
    M32(teb + 0x2c) = arr;
  }
  return teb;
}

/* ---- image ----------------------------------------------------------------- */
static void map_image(const char *exe) {
  int fd = open(exe, O_RDONLY);
  if (fd < 0) { perror(exe); exit(2); }
  struct stat st;
  fstat(fd, &st);
  u8 *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  /* headers */
  memcpy(HP(IMAGE_BASE), file, 0x1000);
  for (int i = 0; i < g_section_count; i++) {
    const ImageSection *s = &g_sections[i];
    u32 n = s->raw_size < s->vsize ? s->raw_size : s->vsize;
    if (s->raw_off + n > (u64)st.st_size) { fprintf(stderr, "%s: truncated (section %s)\n", exe, s->name); exit(2); }
    memcpy(HP(s->va), file + s->raw_off, n);
  }
  munmap(file, st.st_size);
}

static void stub_missing(Ctx *c) {
  /* name is recovered from the thunk table by the caller's target address */
  rt_fatal(c, "called %s, which has no implementation yet (return address %08x)", t_last_api, M32(c->r[4]));
}

typedef struct { const char *dll, *name; } MissingInfo;

static void bind_imports(void) {
  int missing = 0;
  for (int i = 0; i < g_import_count; i++) {
    const ImageImport *im = &g_imports[i];
    HostFn f = api_lookup(im->dll, im->name);
    char *nm = malloc(strlen(im->dll) + strlen(im->name) + 2);
    sprintf(nm, "%s!%s", im->dll, im->name);
    if (!f) { f = stub_missing; missing++; TRACE("unimplemented import %s", nm); }
    M32(im->iat) = rt_thunk(f, nm);
  }
  rt_log("bound %d imports (%d not implemented yet; they abort only if called)", g_import_count, missing);
}

/* catch guest faults and report them with the guest call stack */
static void on_fault(int sig, siginfo_t *si, void *uc) {
  u64 a = (u64)si->si_addr;
  Ctx *c = t_ctx;
  u64 gb = (u64)g_guest_mem;
  if (a >= gb && a < gb + 0x100000000ull)
    rt_fatal(c, "guest memory fault (signal %d) at guest address %08x", sig, (u32)(a - gb));
  rt_fatal(c, "host fault (signal %d) at %p", sig, (void *)a);
}

extern void api_set_cmdline(const char *);

/* stall sampler: SIGUSR2 makes a thread record where it is (host pc and frame-pointer chain); the
   addresses are symbolized offline against the build (PS5: llvm-pie.elf, minus 0x400000) */
static volatile u64 g_sample[12];
static volatile int g_sample_n = -1;
static void on_sample(int sig, siginfo_t *si, void *ucv) {
  (void)sig; (void)si;
  ucontext_t *uc = ucv;
#ifdef __PROSPERO__
  u64 pc = uc->uc_mcontext.mc_rip, fp = uc->uc_mcontext.mc_rbp;
#else
  u64 pc = uc->uc_mcontext.gregs[REG_RIP], fp = uc->uc_mcontext.gregs[REG_RBP];
#endif
  int n = 0;
  g_sample[n++] = pc;
  u64 lo = (u64)&n, hi = lo + (16u << 20); /* only walk frames on this thread's stack */
  while (n < 12 && fp > lo && fp < hi && !(fp & 7)) {
    u64 next = ((u64 *)fp)[0], ret = ((u64 *)fp)[1];
    if (!ret) break;
    g_sample[n++] = ret;
    if (next <= fp) break;
    fp = next;
  }
  __atomic_store_n(&g_sample_n, n, __ATOMIC_RELEASE);
}
static void sample_thread(int t) {
  if (!g_pth_set[t]) return;
  __atomic_store_n(&g_sample_n, -1, __ATOMIC_RELEASE);
  if (pthread_kill(g_pth[t], SIGUSR2)) return;
  for (int k = 0; k < 50 && __atomic_load_n(&g_sample_n, __ATOMIC_ACQUIRE) < 0; k++) usleep(2000);
  int n = __atomic_load_n(&g_sample_n, __ATOMIC_ACQUIRE);
  if (n <= 0) { rt_log("  t%d sample: no answer", t); return; }
  char b[400];
  int o = snprintf(b, sizeof b, "  t%d sample (host):", t);
  for (int k = 0; k < n && o < (int)sizeof b - 20; k++) o += snprintf(b + o, sizeof b - o, " %llx", (unsigned long long)g_sample[k]);
  rt_log("%s", b);
}
/* the game loop stopped polling the controller: say what every thread is doing */
static void stall_report(double stalled_for) {
  static u64 prev_calls[MAXTHR];
  double now = mono_now();
  rt_log("stall: the game loop has not polled input for %.0f s (fnv_recomp %s; rt_fatal at %p). Threads:", stalled_for,
         FNV_RELEASE, (void *)&rt_fatal);
  for (int t = 0; t < MAXTHR; t++) {
    if (!g_pth_set[t]) continue;
    const char *in = g_in_api[t], *last = g_last_api[t];
    u64 calls = g_api_calls[t], d = calls - prev_calls[t];
    prev_calls[t] = calls;
    if (in) rt_log("  t%d: in %s for %.1f s (called from %08x), %llu calls since last report", t, in, now - g_in_api_since[t],
                   g_last_ret[t], (unsigned long long)d);
    else rt_log("  t%d: running game code; last call %s from %08x, %llu calls since last report", t, last ? last : "(none)",
                g_last_ret[t], (unsigned long long)d);
  }
  sample_thread(0);
  for (int t = 1; t < MAXTHR; t++)
    if (g_pth_set[t] && !g_in_api[t] && g_last_api[t]) sample_thread(t);
}

/* ---- performance: frame timing, CPU and memory; one "perf:" line per heartbeat ------------------- */
#define PERF_MAX 4096
static float g_ft[PERF_MAX]; /* frame-to-frame intervals (ms) in the current window */
static int g_ftn;
static double g_last_frame, g_present_sum, g_t0_cpu = -1, g_t0_cpu_wall;
static int g_present_n;
static pthread_mutex_t g_perf_lock = PTHREAD_MUTEX_INITIALIZER;
static double cpu_clock(clockid_t id) { struct timespec t; return clock_gettime(id, &t) ? -1 : t.tv_sec + t.tv_nsec / 1e9; }
void rt_perf_frame(double present_ms) {
  double now = mono_now();
  double cpu = t_ctx && t_ctx->thread_index == 0 ? cpu_clock(CLOCK_THREAD_CPUTIME_ID) : -1;
  pthread_mutex_lock(&g_perf_lock);
  if (g_last_frame > 0 && g_ftn < PERF_MAX) g_ft[g_ftn++] = (float)((now - g_last_frame) * 1000.0);
  g_last_frame = now;
  g_present_sum += present_ms;
  g_present_n++;
  if (cpu >= 0) { g_t0_cpu = cpu; g_t0_cpu_wall = now; }
  pthread_mutex_unlock(&g_perf_lock);
}
/* CPU used by each guest thread since the last perf line (pthread_getcpuclockid), and the whole process
   (getrusage), so a busy thread shows up by name */
#include <sys/resource.h>
static double thread_cpu(int t) {
  clockid_t id;
  struct timespec ts;
  if (!g_pth_set[t] || pthread_getcpuclockid(g_pth[t], &id) || clock_gettime(id, &ts)) return -1;
  return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double process_cpu(void) {
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru)) return -1;
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static void perf_line(void) {
  static float ft[PERF_MAX];
  static double prev_t0 = -1, prev_t0_wall, prev_proc = -1, prev_wall;
  pthread_mutex_lock(&g_perf_lock);
  int n = g_ftn;
  memcpy(ft, g_ft, n * sizeof *ft);
  g_ftn = 0;
  double present = g_present_n ? g_present_sum / g_present_n : 0, t0 = g_t0_cpu, t0w = g_t0_cpu_wall;
  g_present_sum = 0;
  g_present_n = 0;
  pthread_mutex_unlock(&g_perf_lock);
  char b[1024];
  int o = 0;
  if (n) {
    qsort(ft, n, sizeof *ft, cmp_float);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += ft[i];
    double avg = sum / n;
    o += snprintf(b + o, sizeof b - o, "%.1f fps; frame ms avg %.1f, p99 %.1f, worst %.1f; in Present %.1f ms", 1000.0 / avg,
                  avg, ft[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1], ft[n - 1], present);
  } else o += snprintf(b + o, sizeof b - o, "no frames");
  if (t0 >= 0 && prev_t0 >= 0 && t0w > prev_t0_wall)
    o += snprintf(b + o, sizeof b - o, "; main thread CPU %.0f%%", 100.0 * (t0 - prev_t0) / (t0w - prev_t0_wall));
  if (t0w > prev_t0_wall) { prev_t0 = t0; prev_t0_wall = t0w; }
  double pc = process_cpu(), wall = mono_now(), span = wall - prev_wall;
  static double prev_t[MAXTHR];
  double guest = 0, pct[MAXTHR];
  int busiest[3] = {-1, -1, -1};
  for (int t = 0; t < MAXTHR; t++) {
    double now_t = thread_cpu(t);
    pct[t] = now_t >= 0 && prev_t[t] > 0 && span > 0 ? 100.0 * (now_t - prev_t[t]) / span : -1;
    if (now_t >= 0) prev_t[t] = now_t;
    if (pct[t] < 0) continue;
    guest += pct[t];
    for (int k = 0; k < 3; k++)
      if (busiest[k] < 0 || pct[t] > pct[busiest[k]]) {
        for (int j = 2; j > k; j--) busiest[j] = busiest[j - 1];
        busiest[k] = t;
        break;
      }
  }
#ifdef __PROSPERO__
  guest = -1; /* per-thread CPU clocks read 0 on PS5 (v15 logs): only the process total means anything */
#endif
  if (pc >= 0 && prev_proc >= 0 && span > 0 && guest < 0)
    o += snprintf(b + o, sizeof b - o, ", process %.0f%%", 100.0 * (pc - prev_proc) / span);
  else if (pc >= 0 && prev_proc >= 0 && span > 0) {
    double all = 100.0 * (pc - prev_proc) / span;
    o += snprintf(b + o, sizeof b - o, ", process %.0f%% (game threads %.0f%%, other threads %.0f%%; busiest", all, guest,
                  all - guest > 0 ? all - guest : 0);
    for (int k = 0; k < 3 && busiest[k] >= 0; k++) o += snprintf(b + o, sizeof b - o, " t%d %.0f%%", busiest[k], pct[busiest[k]]);
    o += snprintf(b + o, sizeof b - o, ")");
  }
  prev_proc = pc;
  prev_wall = wall;
  o += snprintf(b + o, sizeof b - o, "; game RAM %llu MB", (unsigned long long)(g_guest_backed >> 20));
#ifdef __PROSPERO__
  size_t flex = 0;
  if (sceKernelAvailableFlexibleMemorySize(&flex) == 0) o += snprintf(b + o, sizeof b - o, ", host memory free %zu MB", flex >> 20);
#else
  FILE *f = fopen("/proc/self/statm", "r");
  unsigned long vsz, rss;
  if (f && fscanf(f, "%lu %lu", &vsz, &rss) == 2) o += snprintf(b + o, sizeof b - o, ", process RSS %lu MB", rss * 4096 >> 20);
  if (f) fclose(f);
#endif
  extern int g_d3d_live, d3d_objects_created(void);
  o += snprintf(b + o, sizeof b - o, "; D3D objects %d live, %d created", g_d3d_live, d3d_objects_created());
  char g[320];
  if (hb_gpu_mem(g, sizeof g) > 0) o += snprintf(b + o, sizeof b - o, "; GPU %s", g);
  rt_log("perf: %s", b);
}

/* FNV_API_STATS: every 30 s, the 20 most called system functions and their calls per second */
static void api_stats(double interval) {
  static u64 *prev, *d; /* allocated on first use: too big for the heartbeat stack, and unused unless enabled */
  if (!prev) { prev = calloc(THUNK_MAX, sizeof *prev); d = calloc(THUNK_MAX, sizeof *d); }
  int n = g_thunk_count, top[20], nt = 0;
  u64 total = 0;
  for (int i = 0; i < n; i++) { d[i] = g_thunk_calls[i] - prev[i]; prev[i] = g_thunk_calls[i]; total += d[i]; }
  for (int k = 0; k < 20; k++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      int used = 0;
      for (int j = 0; j < nt; j++) used |= top[j] == i;
      if (!used && d[i] && (best < 0 || d[i] > d[best])) best = i;
    }
    if (best < 0) break;
    top[nt++] = best;
  }
  rt_log("api stats: %.0f calls/s in total; busiest:", total / interval);
  for (int k = 0; k < nt; k++) rt_log("  %9.0f/s  %s", d[top[k]] / interval, g_thunk_name[top[k]]);
}

/* heartbeat: every 5 s, one log line with progress counters and any thread that has been
   inside a single system call for more than 2 s (where a stall is) */
static void *heartbeat(void *arg) {
  (void)arg;
  u64 last_frames = 0, last_read = 0;
  for (int n = 0;; n++) {
    struct timespec d = {1, 0};
    nanosleep(&d, NULL);
    g_coarse_now = mono_now();
    fflush(stderr);
    if (n % 5 != 4) continue;
    u64 fr = g_d3d_frames, rd = g_stat_read;
    char stuck[512] = "";
    double now = mono_now();
    for (int i = 0; i < MAXTHR; i++) {
      const char *a = g_in_api[i];
      double since = g_in_api_since[i];
      /* idle workers park in waits for good; report waits only on the main thread */
      int idle = a && (strstr(a, "WaitFor") || strstr(a, "!Sleep") || strstr(a, "ExitThread"));
      if (a && since && now - since > 2.0 && (!idle || i == 0) && strlen(stuck) < 400) {
        char b[96];
        snprintf(b, sizeof b, " t%d:%s(%.0fs)", i, a, now - since);
        strcat(stuck, b);
      }
    }
    extern u64 g_stat_pad;
    extern int dsound_voices(void);
    rt_log("heartbeat: %llu frames (+%llu), %llu file opens, %.1f MB read (+%.1f), %llu memory commits, %llu pad reads, "
           "%d sounds playing%s%s",
           (unsigned long long)fr, (unsigned long long)(fr - last_frames), (unsigned long long)g_stat_opens,
           rd / 1048576.0, (rd - last_read) / 1048576.0, (unsigned long long)g_stat_valloc, (unsigned long long)g_stat_pad,
           dsound_voices(), stuck[0] ? "; waiting in" : "", stuck);
    perf_line();
    static int stats = -1;
    if (stats < 0) stats = getenv("FNV_API_STATS") != NULL;
    if (stats && n % 30 == 29) api_stats(30.0);
    if (stack_stats() && n % 30 == 29) stack_report();
    last_frames = fr; last_read = rd;
    /* 15 s without a controller poll while frames were being drawn: report (at most every 30 s, 6 times) */
    static u64 last_pad;
    static int quiet_beats, reports;
    static double last_report;
    if (fr && g_stat_pad == last_pad) quiet_beats++; else quiet_beats = 0;
    last_pad = g_stat_pad;
    static int need = -1;
    if (need < 0) { const char *e = getenv("FNV_STALL_SECONDS"); need = e && atoi(e) >= 5 ? atoi(e) / 5 : 3; }
    if (quiet_beats >= need && reports < 6 && now - last_report >= 30) { stall_report(quiet_beats * 5.0); reports++; last_report = now; }
  }
  return NULL;
}
void rt_heartbeat_start(void) {
  g_coarse_now = mono_now();
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 << 10);
  if (pthread_create(&th, &at, heartbeat, NULL) == 0) pthread_detach(th);
  pthread_attr_destroy(&at);
}

int main(int argc, char **argv) {
  const char *exe = getenv("FNV_EXE");
  const char *game = getenv("FNV_GAME_DIR");
  if (getenv("FNV_TRACE")) g_trace = atoi(getenv("FNV_TRACE"));
#ifdef __PROSPERO__
  /* PS5 app: /app0 is the app's folder, /data/homebrew/<title id>/ on the console.
     The game's files go in game/, settings and saves land in user/, the log in fnv.log. */
  if (!game) game = "/app0/game";
  if (!getenv("FNV_USER_DIR")) setenv("FNV_USER_DIR", "/app0/user", 1);
  if (!freopen("/app0/fnv.log", "w", stderr)) perror("log file");
  setvbuf(stderr, NULL, _IOFBF, 1 << 16); /* written in blocks; the heartbeat flushes every second */
  if (!getenv("FNV_TRACE")) g_trace = 1;
#endif
  if (!game) game = "game";
  rt_realpath(game, g_game_dir, sizeof g_game_dir);
  static char exe_buf[1100];
  if (!exe) { snprintf(exe_buf, sizeof exe_buf, "%s/FalloutNV.exe", g_game_dir); exe = exe_buf; }
  const char *user = getenv("FNV_USER_DIR");
  if (!user) user = "user";
  mkdir(user, 0755);
  rt_realpath(user, g_user_dir, sizeof g_user_dir);

  rt_log("fnv_recomp %s", FNV_RELEASE);
#ifdef __PROSPERO__
  size_t flex_total = 0, flex_free = 0;
  sceKernelConfiguredFlexibleMemorySize(&flex_total);
  sceKernelAvailableFlexibleMemorySize(&flex_free);
  rt_log("host (flexible) memory: %zu MB configured, %zu MB free at start", flex_total >> 20, flex_free >> 20);
#endif
  struct sigaction ss = {0};
  ss.sa_sigaction = on_sample;
  ss.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGUSR2, &ss, NULL);
  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);

  rt_heartbeat_start();
  overlay_init(); /* the app's assets over the game folder (button prompts) */
  { extern void rt_first_run(void); rt_first_run(); }
  mem_init();
  map_image(exe);
  api_init_all();
  {
    extern int plat_init(void);
    plat_init();
  }
  g_host_return = rt_thunk(host_return_marker, "<host return>");

  /* PEB */
  g_peb = vm_alloc(0, 0x1000, 0x3000, 4);
  M32(g_peb + 0x08) = IMAGE_BASE;
  bind_imports();
  if (g_tls_index_addr) M32(g_tls_index_addr) = 0;

  char cmd[2048] = "\"FalloutNV.exe\"";
  for (int i = 1; i < argc; i++) { strcat(cmd, " "); strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 2); }
  api_set_cmdline(cmd);

  Ctx *c = rt_new_ctx(g_stack_reserve < 0x100000 ? 0x100000 : g_stack_reserve);
  rt_bind_thread(c);
  rt_log("image mapped, entry %08x, %u recompiled functions", g_entry, g_func_count);

  for (int i = 0; g_tls_callbacks[i]; i++) {
    u32 args[3] = {IMAGE_BASE, 1, 0};
    TRACE("TLS callback %08x", g_tls_callbacks[i]);
    rt_call_guest(c, g_tls_callbacks[i], 3, args);
  }
  u32 r = rt_call_guest(c, g_entry, 0, NULL);
  rt_log("entry point returned %u", r);
  return (int)r;
}
