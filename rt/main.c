/* Loader and core runtime: guest address space, image mapping, dispatch, threads. */
#define _GNU_SOURCE
#include "rt.h"
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>
#include <errno.h>
#include <sys/stat.h>

int g_trace;
u32 g_peb;
static __thread Ctx *t_ctx;
static __thread const char *t_last_api;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void rt_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&g_log_lock);
  Ctx *c = t_ctx;
  fprintf(stderr, "[t%d] ", c ? c->thread_index : -1);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  pthread_mutex_unlock(&g_log_lock);
  va_end(ap);
}

static void dump_backtrace(void) {
  void *bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
}

void rt_fatal(Ctx *c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&g_log_lock);
  fprintf(stderr, "\n*** FATAL [t%d]: ", c ? c->thread_index : -1);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  if (c)
    fprintf(stderr, "    eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->r[0], c->r[1],
            c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
  fprintf(stderr, "    host backtrace (f_XXXXXXXX = guest function at that address):\n");
  dump_backtrace();
  pthread_mutex_unlock(&g_log_lock);
  va_end(ap);
  _exit(3);
}

/* ---- thunks ---------------------------------------------------------------- */
static HostFn g_thunk_fn[THUNK_MAX];
static const char *g_thunk_name[THUNK_MAX];
static int g_thunk_count;
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
      g_thunk_fn[i](c);
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

void rt_bind_thread(Ctx *c) { t_ctx = c; }

u32 rt_teb_alloc(Ctx *c, u32 stack_lo, u32 stack_hi, u32 tid) {
  u32 teb = vm_alloc(0, 0x2000, 0x3000, 4);
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
  if (a >= GUEST_BASE && a < GUEST_BASE + 0x100000000ull)
    rt_fatal(c, "guest memory fault (signal %d) at guest address %08x", sig, (u32)(a - GUEST_BASE));
  rt_fatal(c, "host fault (signal %d) at %p", sig, (void *)a);
}

extern void api_set_cmdline(const char *);

int main(int argc, char **argv) {
  const char *exe = getenv("FNV_EXE");
  const char *game = getenv("FNV_GAME_DIR");
  if (getenv("FNV_TRACE")) g_trace = atoi(getenv("FNV_TRACE"));
  if (!game) game = "game";
  snprintf(g_game_dir, sizeof g_game_dir, "%s", realpath(game, NULL) ? realpath(game, NULL) : game);
  static char exe_buf[1100];
  if (!exe) { snprintf(exe_buf, sizeof exe_buf, "%s/FalloutNV.exe", g_game_dir); exe = exe_buf; }
  const char *user = getenv("FNV_USER_DIR");
  if (!user) user = "user";
  mkdir(user, 0755);
  snprintf(g_user_dir, sizeof g_user_dir, "%s", realpath(user, NULL));

  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);

  mem_init();
  map_image(exe);
  api_init_all();
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
