/* KERNEL32 for the recompiled game. */
#define _GNU_SOURCE
#include "rt.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sched.h>
#include <setjmp.h>

#define API(n) static void n(Ctx *c)
#define K "KERNEL32.dll"

static char g_cmdline[2048];
static u32 g_cmdline_a, g_cmdline_w;
void api_set_cmdline(const char *s) { snprintf(g_cmdline, sizeof g_cmdline, "%s", s); }

void rt_set_error(Ctx *c, u32 e) { c->last_error = e; M32(c->fs_base + 0x34) = e; }
static u32 errno_to_win(int e) {
  switch (e) { case ENOENT: return 2; case ENOTDIR: return 3; case EACCES: return 5; case EEXIST: return 80; default: return 31; }
}

/* ---- TLS ------------------------------------------------------------------- */
static u8 g_tls_used[64];
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;
#define TLS_SLOT(i) M32(c->fs_base + 0xE10 + 4 * (i))
API(k_TlsAlloc) {
  pthread_mutex_lock(&g_tls_lock);
  u32 r = 0xffffffff;
  for (int i = 0; i < 64; i++) if (!g_tls_used[i]) { g_tls_used[i] = 1; r = i; break; }
  pthread_mutex_unlock(&g_tls_lock);
  RETURN_STD(0, r);
}
API(k_TlsFree) { u32 i = ARG(0); if (i < 64) g_tls_used[i] = 0; RETURN_STD(1, 1); }
API(k_TlsGetValue) { u32 i = ARG(0); rt_set_error(c, 0); RETURN_STD(1, i < 64 ? TLS_SLOT(i) : 0); }
API(k_TlsSetValue) { u32 i = ARG(0); if (i < 64) TLS_SLOT(i) = ARG(1); RETURN_STD(2, i < 64); }

/* ---- critical sections ----------------------------------------------------- */
#define MAX_CS 65536
static pthread_mutex_t *g_cs[MAX_CS];
static int g_ncs = 1;
static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_get(u32 cs) {
  u32 idx = __atomic_load_n((u32 *)HP(cs + 16), __ATOMIC_ACQUIRE);
  if (idx && idx < MAX_CS && g_cs[idx]) return g_cs[idx];
  pthread_mutex_lock(&g_cs_lock);
  idx = M32(cs + 16);
  if (!(idx && idx < MAX_CS && g_cs[idx])) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutex_init(m, &a);
    idx = g_ncs++;
    if (idx >= MAX_CS) rt_fatal(NULL, "too many critical sections");
    g_cs[idx] = m;
    __atomic_store_n((u32 *)HP(cs + 16), idx, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&g_cs_lock);
  return g_cs[idx];
}
static void cs_init(u32 cs) {
  M32(cs) = 0; M32(cs + 4) = 0xffffffff; M32(cs + 8) = 0; M32(cs + 12) = 0; M32(cs + 16) = 0; M32(cs + 20) = 0;
  cs_get(cs);
}
API(k_InitializeCriticalSection) { cs_init(ARG(0)); RETURN_STD(1, 0); }
API(k_InitializeCriticalSectionAndSpinCount) { cs_init(ARG(0)); RETURN_STD(2, 1); }
API(k_DeleteCriticalSection) { RETURN_STD(1, 0); }
static u32 cur_tid(Ctx *c) { return M32(c->fs_base + 0x24); }
API(k_EnterCriticalSection) {
  u32 cs = ARG(0);
  pthread_mutex_lock(cs_get(cs));
  M32(cs + 12) = cur_tid(c); M32(cs + 8) += 1;
  RETURN_STD(1, 0);
}
API(k_TryEnterCriticalSection) {
  u32 cs = ARG(0);
  int ok = pthread_mutex_trylock(cs_get(cs)) == 0;
  if (ok) { M32(cs + 12) = cur_tid(c); M32(cs + 8) += 1; }
  RETURN_STD(1, ok);
}
API(k_LeaveCriticalSection) {
  u32 cs = ARG(0);
  if (M32(cs + 8) && --M32(cs + 8) == 0) M32(cs + 12) = 0;
  pthread_mutex_unlock(cs_get(cs));
  RETURN_STD(1, 0);
}

/* ---- interlocked ----------------------------------------------------------- */
#define A32(a) ((u32 *)HP(a))
API(k_InterlockedIncrement) { RETURN_STD(1, __atomic_add_fetch(A32(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
API(k_InterlockedDecrement) { RETURN_STD(1, __atomic_sub_fetch(A32(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
API(k_InterlockedExchange) { RETURN_STD(2, __atomic_exchange_n(A32(ARG(0)), ARG(1), __ATOMIC_SEQ_CST)); }
API(k_InterlockedExchangeAdd) { RETURN_STD(2, __atomic_fetch_add(A32(ARG(0)), ARG(1), __ATOMIC_SEQ_CST)); }
API(k_InterlockedCompareExchange) {
  u32 exp = ARG(2);
  __atomic_compare_exchange_n(A32(ARG(0)), &exp, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  RETURN_STD(3, exp);
}

/* ---- time ------------------------------------------------------------------ */
static u64 now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + t.tv_nsec; }
API(k_GetTickCount) { RETURN_STD(0, (u32)(now_ns() / 1000000)); }
API(k_QueryPerformanceCounter) { M64(ARG(0)) = now_ns() / 100; RETURN_STD(1, 1); }
API(k_QueryPerformanceFrequency) { M64(ARG(0)) = 10000000; RETURN_STD(1, 1); }
static u64 unix_to_ft(time_t s, long ns) { return ((u64)s + 11644473600ull) * 10000000ull + ns / 100; }
API(k_GetSystemTimeAsFileTime) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); M64(ARG(0)) = unix_to_ft(t.tv_sec, t.tv_nsec); RETURN_STD(1, 0); }
static void fill_systime(u32 p, struct tm *tm, int ms) {
  M16(p) = tm->tm_year + 1900; M16(p + 2) = tm->tm_mon + 1; M16(p + 4) = tm->tm_wday; M16(p + 6) = tm->tm_mday;
  M16(p + 8) = tm->tm_hour; M16(p + 10) = tm->tm_min; M16(p + 12) = tm->tm_sec; M16(p + 14) = ms;
}
API(k_GetLocalTime) { struct timeval tv; gettimeofday(&tv, 0); struct tm tm; localtime_r(&tv.tv_sec, &tm); fill_systime(ARG(0), &tm, tv.tv_usec / 1000); RETURN_STD(1, 0); }
API(k_GetSystemTime) { struct timeval tv; gettimeofday(&tv, 0); struct tm tm; gmtime_r(&tv.tv_sec, &tm); fill_systime(ARG(0), &tm, tv.tv_usec / 1000); RETURN_STD(1, 0); }
API(k_FileTimeToSystemTime) {
  u64 ft = M64(ARG(0));
  time_t s = (time_t)(ft / 10000000ull - 11644473600ull);
  struct tm tm; gmtime_r(&s, &tm); fill_systime(ARG(1), &tm, (ft / 10000) % 1000);
  RETURN_STD(2, 1);
}
API(k_FileTimeToLocalFileTime) { M64(ARG(1)) = M64(ARG(0)); RETURN_STD(2, 1); }
API(k_SystemTimeToTzSpecificLocalTime) { memcpy(HP(ARG(2)), HP(ARG(1)), 16); RETURN_STD(3, 1); }
API(k_CompareFileTime) {
  u64 a = M64(ARG(0)), b = M64(ARG(1));
  RETURN_STD(2, a < b ? (u32)-1 : a > b);
}
API(k_GetTimeZoneInformation) { memset(HP(ARG(0)), 0, 172); RETURN_STD(1, 0); }

/* ---- strings --------------------------------------------------------------- */
API(k_lstrcatA) { strcat((char *)HP(ARG(0)), STR(ARG(1))); RETURN_STD(2, ARG(0)); }
API(k_lstrcpyA) { strcpy((char *)HP(ARG(0)), STR(ARG(1))); RETURN_STD(2, ARG(0)); }
API(k_MultiByteToWideChar) {
  u32 src = ARG(2); s32 n = ARG(3); u32 dst = ARG(4); s32 cap = ARG(5);
  if (n < 0) n = strlen(STR(src)) + 1;
  if (!cap) RETURN_STD(6, n);
  s32 k = n < cap ? n : cap;
  for (s32 i = 0; i < k; i++) M16(dst + 2 * i) = M8(src + i);
  RETURN_STD(6, k);
}
API(k_WideCharToMultiByte) {
  u32 src = ARG(2); s32 n = ARG(3); u32 dst = ARG(4); s32 cap = ARG(5);
  if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
  if (!cap) RETURN_STD(8, n);
  s32 k = n < cap ? n : cap;
  for (s32 i = 0; i < k; i++) { u16 w = M16(src + 2 * i); M8(dst + i) = w < 256 ? w : '?'; }
  RETURN_STD(8, k);
}
API(k_GetACP) { RETURN_STD(0, 1252); }
API(k_GetOEMCP) { RETURN_STD(0, 437); }
API(k_IsValidCodePage) { RETURN_STD(1, 1); }
API(k_GetCPInfo) {
  u32 p = ARG(1);
  memset(HP(p), 0, 20);
  M32(p) = 1; M8(p + 4) = '?';
  RETURN_STD(2, 1);
}
API(k_GetStringTypeW) {
  u32 type = ARG(0), src = ARG(1); s32 n = ARG(2); u32 out = ARG(3);
  if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
  for (s32 i = 0; i < n; i++) {
    u16 ch = M16(src + 2 * i), t = 0;
    if (type == 1 && ch < 256) {
      if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
      if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40;
      if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100;
    }
    M16(out + 2 * i) = t;
  }
  RETURN_STD(4, 1);
}
API(k_GetStringTypeA) {
  u32 type = ARG(1), src = ARG(2); s32 n = ARG(3); u32 out = ARG(4);
  if (n < 0) n = strlen(STR(src)) + 1;
  for (s32 i = 0; i < n; i++) {
    u8 ch = M8(src + i); u16 t = 0;
    if (type == 1 && ch < 128) {
      if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
      if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40;
      if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100;
    }
    M16(out + 2 * i) = t;
  }
  RETURN_STD(5, 1);
}
API(k_LCMapStringW) {
  u32 flags = ARG(1), src = ARG(2); s32 n = ARG(3); u32 dst = ARG(4); s32 cap = ARG(5);
  if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
  if (!cap) RETURN_STD(6, n);
  for (s32 i = 0; i < n && i < cap; i++) {
    u16 ch = M16(src + 2 * i);
    if (ch < 128) ch = (flags & 0x200) ? toupper(ch) : (flags & 0x100) ? tolower(ch) : ch;
    M16(dst + 2 * i) = ch;
  }
  RETURN_STD(6, n < cap ? n : cap);
}
API(k_LCMapStringA) {
  u32 flags = ARG(1), src = ARG(2); s32 n = ARG(3); u32 dst = ARG(4); s32 cap = ARG(5);
  if (n < 0) n = strlen(STR(src)) + 1;
  if (!cap) RETURN_STD(6, n);
  for (s32 i = 0; i < n && i < cap; i++) {
    u8 ch = M8(src + i);
    if (ch < 128) ch = (flags & 0x200) ? toupper(ch) : (flags & 0x100) ? tolower(ch) : ch;
    M8(dst + i) = ch;
  }
  RETURN_STD(6, n < cap ? n : cap);
}
static int cmp_res(int r) { return r < 0 ? 1 : r > 0 ? 3 : 2; }
API(k_CompareStringA) {
  u32 flags = ARG(1); u32 a = ARG(2); s32 na = ARG(3); u32 b = ARG(4); s32 nb = ARG(5);
  if (na < 0) na = strlen(STR(a));
  if (nb < 0) nb = strlen(STR(b));
  int n = na < nb ? na : nb;
  int r = (flags & 1) ? strncasecmp(STR(a), STR(b), n) : strncmp(STR(a), STR(b), n);
  if (!r) r = na - nb;
  RETURN_STD(6, cmp_res(r));
}
API(k_CompareStringW) {
  u32 flags = ARG(1); u32 a = ARG(2); s32 na = ARG(3); u32 b = ARG(4); s32 nb = ARG(5);
  if (na < 0) { na = 0; while (M16(a + 2 * na)) na++; }
  if (nb < 0) { nb = 0; while (M16(b + 2 * nb)) nb++; }
  int r = 0;
  for (int i = 0; i < na && i < nb && !r; i++) {
    int x = M16(a + 2 * i), y = M16(b + 2 * i);
    if (flags & 1) { if (x < 128) x = tolower(x); if (y < 128) y = tolower(y); }
    r = x - y;
  }
  if (!r) r = na - nb;
  RETURN_STD(6, cmp_res(r));
}
static int put_locale_a(u32 buf, s32 cap, const char *v) {
  int n = strlen(v) + 1;
  if (cap && buf) { if (cap < n) return 0; memcpy(HP(buf), v, n); }
  return n;
}
static const char *locale_value(u32 type) {
  switch (type & 0xffff) {
  case 0x1001: return "English"; case 0x1002: return "United States"; case 0x3: return "ENU"; case 0x7: return "USA";
  case 0xb: return "437"; case 0x1004: return "1252"; case 0x1: return "0409"; case 0x59: return "en"; case 0x5a: return "US";
  case 0xe: return "."; case 0xf: return ","; case 0x14: return "$"; case 0x1009: return "6";
  default: return "";
  }
}
API(k_GetLocaleInfoA) { RETURN_STD(4, put_locale_a(ARG(2), ARG(3), locale_value(ARG(1)))); }
API(k_GetLocaleInfoW) {
  const char *v = locale_value(ARG(1));
  u32 buf = ARG(2); s32 cap = ARG(3); int n = strlen(v) + 1;
  if (cap) { if (cap < n) RETURN_STD(4, 0); for (int i = 0; i < n; i++) M16(buf + 2 * i) = (u8)v[i]; }
  RETURN_STD(4, n);
}
API(k_IsValidLocale) { RETURN_STD(2, 1); }
API(k_GetUserDefaultLCID) { RETURN_STD(0, 0x409); }
API(k_EnumSystemLocalesA) { RETURN_STD(2, 1); }

/* ---- environment / process ------------------------------------------------- */
API(k_GetCommandLineA) { if (!g_cmdline_a) g_cmdline_a = guest_strdup(g_cmdline); RETURN_STD(0, g_cmdline_a); }
API(k_GetCommandLineW) {
  if (!g_cmdline_w) {
    int n = strlen(g_cmdline) + 1;
    g_cmdline_w = heap_alloc(n * 2, 0);
    for (int i = 0; i < n; i++) M16(g_cmdline_w + 2 * i) = (u8)g_cmdline[i];
  }
  RETURN_STD(0, g_cmdline_w);
}
API(k_GetEnvironmentStrings) { u32 p = heap_alloc(16, 1); RETURN_STD(0, p); }
API(k_GetEnvironmentStringsW) { u32 p = heap_alloc(16, 1); RETURN_STD(0, p); }
API(k_FreeEnvironmentStringsA) { RETURN_STD(1, 1); }
API(k_FreeEnvironmentStringsW) { RETURN_STD(1, 1); }
API(k_SetEnvironmentVariableA) { RETURN_STD(2, 1); }
API(k_GetStartupInfoA) { u32 p = ARG(0); memset(HP(p), 0, 68); M32(p) = 68; RETURN_STD(1, 0); }
API(k_GetCurrentProcess) { RETURN_STD(0, 0xffffffff); }
API(k_GetCurrentProcessId) { RETURN_STD(0, 0x100); }
API(k_GetCurrentThread) { RETURN_STD(0, 0xfffffffe); }
API(k_GetCurrentThreadId) { RETURN_STD(0, cur_tid(c)); }
API(k_ExitProcess) { rt_log("ExitProcess(%u)", ARG(0)); rt_exit(ARG(0)); }
API(k_TerminateProcess) { rt_log("TerminateProcess(%u)", ARG(1)); rt_exit(ARG(1)); }
API(k_IsDebuggerPresent) { RETURN_STD(0, 0); }
API(k_DebugBreak) { rt_log("DebugBreak()"); RETURN_STD(0, 0); }
API(k_OutputDebugStringA) { rt_log("OutputDebugString: %s", STR(ARG(0))); RETURN_STD(1, 0); }
API(k_GetLastError) { RETURN_STD(0, M32(c->fs_base + 0x34)); }
API(k_SetLastError) { rt_set_error(c, ARG(0)); RETURN_STD(1, 0); }
API(k_SetHandleCount) { RETURN_STD(1, ARG(0)); }
API(k_GetStdHandle) { s32 n = ARG(0); RETURN_STD(1, n == -10 ? 0x10 : n == -11 ? 0x14 : 0x18); }
API(k_SetStdHandle) { RETURN_STD(2, 1); }
API(k_GetConsoleCP) { RETURN_STD(0, 437); }
API(k_GetConsoleOutputCP) { RETURN_STD(0, 437); }
API(k_GetConsoleMode) { RETURN_STD(2, 0); }
API(k_WriteConsoleA) { fwrite(HP(ARG(1)), 1, ARG(2), stderr); if (ARG(3)) M32(ARG(3)) = ARG(2); RETURN_STD(5, 1); }
API(k_WriteConsoleW) { RETURN_STD(5, 0); }
API(k_GetSystemInfo) {
  u32 p = ARG(0); memset(HP(p), 0, 36);
  M32(p + 4) = 0x1000; M32(p + 8) = 0x10000; M32(p + 12) = 0x7ffeffff; M32(p + 16) = 0xf; M32(p + 20) = 4;
  M32(p + 24) = 586; M32(p + 28) = 0x10000; M16(p + 32) = 6;
  RETURN_STD(1, 0);
}
API(k_GlobalMemoryStatus) {
  u32 p = ARG(0); M32(p) = 32; M32(p + 4) = 20; M32(p + 8) = 0x7fffffff; M32(p + 12) = 0x60000000;
  M32(p + 16) = 0x7fffffff; M32(p + 20) = 0x7fffffff; M32(p + 24) = 0x7ffe0000; M32(p + 28) = 0x60000000;
  RETURN_STD(1, 0);
}
API(k_GlobalMemoryStatusEx) {
  u32 p = ARG(0); M32(p) = 64; M32(p + 4) = 20;
  M64(p + 8) = 8ull << 30; M64(p + 16) = 6ull << 30; M64(p + 24) = 16ull << 30; M64(p + 32) = 14ull << 30;
  M64(p + 40) = 0x7ffe0000; M64(p + 48) = 0x60000000; M64(p + 56) = 0;
  RETURN_STD(1, 1);
}

/* ---- memory ---------------------------------------------------------------- */
API(k_VirtualAlloc) {
  u32 r = vm_alloc(ARG(0), ARG(1), ARG(2), ARG(3));
  __atomic_add_fetch(&g_stat_valloc, 1, __ATOMIC_RELAXED);
  if (g_trace > 1) rt_log("VirtualAlloc(%08x, %x, %x) = %08x", ARG(0), ARG(1), ARG(2), r);
  if (!r) rt_set_error(c, 8);
  RETURN_STD(4, r);
}
API(k_VirtualFree) { RETURN_STD(3, vm_free(ARG(0), ARG(1), ARG(2))); }
API(k_VirtualQuery) {
  u32 base, ab, size, state, p = ARG(1);
  if (!vm_query(ARG(0), &base, &ab, &size, &state)) RETURN_STD(3, 0);
  M32(p) = base; M32(p + 4) = ab; M32(p + 8) = 4; M32(p + 12) = size; M32(p + 16) = state;
  M32(p + 20) = state == 0x1000 ? 4 : 1; M32(p + 24) = state == 0x1000 ? 0x20000 : 0;
  RETURN_STD(3, 28);
}
API(k_HeapCreate) { RETURN_STD(3, h_new(H_HEAP)); }
API(k_GetProcessHeap) { static u32 h; if (!h) h = h_new(H_HEAP); RETURN_STD(0, h); }
API(k_HeapAlloc) { u32 r = heap_alloc(ARG(2), ARG(1) & 8); RETURN_STD(3, r); }
API(k_HeapFree) { heap_free(ARG(2)); RETURN_STD(3, 1); }
API(k_HeapSize) { RETURN_STD(3, heap_size(ARG(2))); }
API(k_HeapReAlloc) {
  if (ARG(1) & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
    u32 p = ARG(2), n = ARG(3);
    RETURN_STD(4, heap_size(p) >= n ? p : 0);
  }
  RETURN_STD(4, heap_realloc(ARG(2), ARG(3), ARG(1) & 8));
}
API(k_LocalFree) { heap_free(ARG(0)); RETURN_STD(1, 0); }

/* ---- modules --------------------------------------------------------------- */
static const char *g_mod_names[] = {"kernel32.dll", "user32.dll", "d3d9.dll", "d3dx9_38.dll", "dinput8.dll", "dsound.dll",
                                    "xinput1_3.dll", "ntdll.dll", "advapi32.dll", "ole32.dll", "winmm.dll", "shell32.dll",
                                    "gdi32.dll", "binkw32.dll", "libvorbisfile.dll", "galaxywrp.dll", "wsock32.dll",
                                    "comctl32.dll", NULL};
static u32 mod_handle(const char *name) {
  const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
  char nm[128]; snprintf(nm, sizeof nm, "%s", b);
  if (!strchr(nm, '.')) strcat(nm, ".dll");
  for (int i = 0; g_mod_names[i]; i++)
    if (!strcasecmp(g_mod_names[i], nm)) return 0x70000000u + (i << 16);
  return 0;
}
API(k_GetModuleHandleA) {
  u32 n = ARG(0);
  if (!n) RETURN_STD(1, IMAGE_BASE);
  u32 h = mod_handle(STR(n));
  TRACE("GetModuleHandleA(%s) = %08x", STR(n), h);
  if (!h) rt_set_error(c, 126);
  RETURN_STD(1, h);
}
API(k_GetModuleHandleW) {
  u32 n = ARG(0);
  if (!n) RETURN_STD(1, IMAGE_BASE);
  char s[256]; int i = 0; while (i < 255 && M16(n + 2 * i)) { s[i] = M16(n + 2 * i); i++; } s[i] = 0;
  u32 h = mod_handle(s);
  TRACE("GetModuleHandleW(%s) = %08x", s, h);
  RETURN_STD(1, h);
}
API(k_LoadLibraryA) {
  u32 h = mod_handle(STR(ARG(0)));
  rt_log("LoadLibraryA(%s) = %08x", STR(ARG(0)), h);
  if (!h) rt_set_error(c, 126);
  RETURN_STD(1, h);
}
API(k_FreeLibrary) { RETURN_STD(1, 1); }
API(k_GetProcAddress) {
  u32 h = ARG(0), n = ARG(1);
  const char *dll = NULL;
  if (h >= 0x70000000u && h < 0x70000000u + (18u << 16)) dll = g_mod_names[(h - 0x70000000u) >> 16];
  if (!dll || n < 0x10000) { rt_log("GetProcAddress(%08x, #%u) = 0", h, n); RETURN_STD(2, 0); }
  char dllname[64]; snprintf(dllname, sizeof dllname, "%s", dll);
  u32 r = rt_get_proc(dllname, STR(n));
  rt_log("GetProcAddress(%s, %s) = %08x", dll, STR(n), r);
  if (!r) rt_set_error(c, 127);
  RETURN_STD(2, r);
}
API(k_GetModuleFileNameA) {
  u32 h = ARG(0), buf = ARG(1), n = ARG(2);
  char s[300];
  snprintf(s, sizeof s, "%s\\%s", fs_game_win_dir(), (!h || h == IMAGE_BASE) ? "FalloutNV.exe" : "unknown.dll");
  u32 len = strlen(s);
  if (len >= n) len = n - 1;
  memcpy(HP(buf), s, len); M8(buf + len) = 0;
  RETURN_STD(3, len);
}

/* ---- sync objects ---------------------------------------------------------- */
API(k_CreateEventA) {
  u32 h = h_new(H_EVENT); HObj *o = h_get(h);
  o->manual = ARG(1); o->signaled = ARG(2);
  RETURN_STD(4, h);
}
API(k_SetEvent) {
  HObj *o = h_get(ARG(0));
  pthread_mutex_lock(&g_wait_lock); if (o) o->signaled = 1; sync_signal_all(); pthread_mutex_unlock(&g_wait_lock);
  RETURN_STD(1, o != NULL);
}
API(k_ResetEvent) {
  HObj *o = h_get(ARG(0));
  pthread_mutex_lock(&g_wait_lock); if (o) o->signaled = 0; pthread_mutex_unlock(&g_wait_lock);
  RETURN_STD(1, o != NULL);
}
API(k_CreateSemaphoreA) {
  u32 h = h_new(H_SEM); HObj *o = h_get(h);
  o->count = ARG(1); o->max = ARG(2);
  RETURN_STD(4, h);
}
API(k_ReleaseSemaphore) {
  HObj *o = h_get(ARG(0));
  if (!o) RETURN_STD(3, 0);
  pthread_mutex_lock(&g_wait_lock);
  if (ARG(2)) M32(ARG(2)) = o->count;
  o->count += ARG(1);
  if (o->max && o->count > o->max) o->count = o->max;
  sync_signal_all();
  pthread_mutex_unlock(&g_wait_lock);
  RETURN_STD(3, 1);
}
API(k_CreateMutexA) {
  u32 h = h_new(H_MUTEX); HObj *o = h_get(h);
  if (ARG(1)) { o->owner_thread = pthread_self(); o->owner_valid = 1; o->recursion = 1; }
  RETURN_STD(3, h);
}
static int try_acquire(HObj *o) {
  switch (o->type) {
  case H_EVENT: if (!o->signaled) return 0; if (!o->manual) o->signaled = 0; return 1;
  case H_SEM: if (o->count <= 0) return 0; o->count--; return 1;
  case H_MUTEX:
    if (o->owner_valid && !pthread_equal(o->owner_thread, pthread_self())) return 0;
    o->owner_thread = pthread_self(); o->owner_valid = 1; o->recursion++; return 1;
  case H_THREAD: return o->finished;
  default: return 1;
  }
}
static int is_ready(HObj *o) {
  switch (o->type) {
  case H_EVENT: return o->signaled;
  case H_SEM: return o->count > 0;
  case H_MUTEX: return !o->owner_valid || pthread_equal(o->owner_thread, pthread_self());
  case H_THREAD: return o->finished;
  default: return 1;
  }
}
static u32 wait_objects(Ctx *c, int n, const u32 *hs, int all, u32 ms) {
  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  if (ms != 0xffffffff) {
    dl.tv_sec += ms / 1000; dl.tv_nsec += (ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
  }
  pthread_mutex_lock(&g_wait_lock);
  for (;;) {
    if (all) {
      int ok = 1;
      for (int i = 0; i < n; i++) { HObj *o = h_get(hs[i]); if (o && !is_ready(o)) ok = 0; }
      if (ok) {
        for (int i = 0; i < n; i++) { HObj *o = h_get(hs[i]); if (o) try_acquire(o); }
        pthread_mutex_unlock(&g_wait_lock);
        return 0;
      }
    } else {
      for (int i = 0; i < n; i++) {
        HObj *o = h_get(hs[i]);
        if (!o) { pthread_mutex_unlock(&g_wait_lock); rt_set_error(c, 6); return 0xffffffff; }
        if (try_acquire(o)) { pthread_mutex_unlock(&g_wait_lock); return i; }
      }
    }
    if (ms == 0) break;
    if (ms == 0xffffffff) pthread_cond_wait(&g_wait_cond, &g_wait_lock);
    else if (pthread_cond_timedwait(&g_wait_cond, &g_wait_lock, &dl) == ETIMEDOUT) {
      /* one last check */
      ms = 0;
    }
  }
  pthread_mutex_unlock(&g_wait_lock);
  return 0x102; /* WAIT_TIMEOUT */
}
API(k_WaitForSingleObject) { u32 h = ARG(0); RETURN_STD(2, wait_objects(c, 1, &h, 0, ARG(1))); }
API(k_WaitForMultipleObjects) {
  u32 n = ARG(0), hs[64];
  for (u32 i = 0; i < n && i < 64; i++) hs[i] = M32(ARG(1) + 4 * i);
  RETURN_STD(4, wait_objects(c, n, hs, ARG(2), ARG(3)));
}
API(k_Sleep) {
  u32 ms = ARG(0);
  if (ms == 0) sched_yield(); else usleep(ms * 1000);
  RETURN_STD(1, 0);
}
API(k_CloseHandle) { h_close(ARG(0)); RETURN_STD(1, 1); }

/* ---- threads --------------------------------------------------------------- */
typedef struct { Ctx *c; u32 start, param, h; HObj *o; } ThreadStart;
static u32 g_thread_exit_thunk;
extern void rt_bind_thread(Ctx *c);
static __thread jmp_buf *t_exit_jmp;
#include <setjmp.h>
static __thread jmp_buf t_exit_buf;

static void *thread_main(void *p) {
  ThreadStart *ts = p;
  Ctx *c = ts->c;
  rt_bind_thread(c);
  pthread_mutex_lock(&g_wait_lock);
  while (ts->o->suspended) pthread_cond_wait(&ts->o->resume, &g_wait_lock);
  pthread_mutex_unlock(&g_wait_lock);
  for (int i = 0; g_tls_callbacks[i]; i++) {
    u32 args[3] = {IMAGE_BASE, 2, 0};
    rt_call_guest(c, g_tls_callbacks[i], 3, args);
  }
  TRACE("thread %d starts at %08x(%08x)", c->thread_index, ts->start, ts->param);
  u32 code;
  if (setjmp(t_exit_buf) == 0) {
    t_exit_jmp = &t_exit_buf;
    code = rt_call_guest(c, ts->start, 1, &ts->param);
  } else {
    code = c->r[0];
  }
  pthread_mutex_lock(&g_wait_lock);
  ts->o->exit_code = code;
  ts->o->finished = 1;
  sync_signal_all();
  pthread_mutex_unlock(&g_wait_lock);
  TRACE("thread %d exits (%u)", c->thread_index, code);
  return NULL;
}

u32 rt_thread_create(Ctx *parent, u32 start, u32 param, u32 stack, u32 flags, u32 *tid) {
  if (stack < 0x40000) stack = 0x40000;
  Ctx *c = rt_new_ctx((stack + 0xffff) & ~0xffffu);
  u32 h = h_new(H_THREAD);
  HObj *o = h_get(h);
  o->suspended = (flags & 4) ? 1 : 0;
  ThreadStart *ts = malloc(sizeof *ts);
  ts->c = c; ts->start = start; ts->param = param; ts->h = h; ts->o = o;
  snprintf(o->name, sizeof o->name, "guest %08x", start);
  /* host stack: recompiled frames are larger than guest frames, so ask for a big one, but
     on the PS5 thread stacks come from the small flexible budget: step down until it fits */
  static const size_t sizes[] = {16u << 20, 8u << 20, 4u << 20, 2u << 20, 1u << 20, 0};
  pthread_t th;
  int err = -1;
  size_t used = 0;
  for (int i = 0; err && i < 6; i++) {
    pthread_attr_t at;
    pthread_attr_init(&at);
    if (sizes[i]) pthread_attr_setstacksize(&at, sizes[i]);
    err = pthread_create(&th, &at, thread_main, ts);
    pthread_attr_destroy(&at);
    used = sizes[i];
  }
  if (err) rt_fatal(parent, "CreateThread: the system refused a host thread for %08x (error %d)", start, err);
  pthread_detach(th);
  TRACE("host thread for %08x: %zu KiB stack", start, used >> 10);
  *tid = M32(c->fs_base + 0x24);
  return h;
}
API(k_CreateThread) {
  u32 tid;
  u32 h = rt_thread_create(c, ARG(2), ARG(3), ARG(1), ARG(4), &tid);
  if (ARG(5)) M32(ARG(5)) = tid;
  rt_log("CreateThread(start=%08x, param=%08x, flags=%x) = %08x", ARG(2), ARG(3), ARG(4), h);
  RETURN_STD(6, h);
}
API(k_ResumeThread) {
  HObj *o = h_get(ARG(0));
  u32 prev = 0;
  pthread_mutex_lock(&g_wait_lock);
  if (o && o->type == H_THREAD) { prev = o->suspended; if (o->suspended) o->suspended--; pthread_cond_broadcast(&o->resume); }
  pthread_mutex_unlock(&g_wait_lock);
  RETURN_STD(1, prev);
}
API(k_SuspendThread) { rt_log("SuspendThread ignored"); RETURN_STD(1, 0); }
API(k_ExitThread) {
  c->r[0] = ARG(0);
  if (t_exit_jmp) longjmp(*t_exit_jmp, 1);
  rt_log("ExitThread on main thread");
  rt_exit(ARG(0));
}
API(k_GetExitCodeThread) {
  HObj *o = h_get(ARG(0));
  if (!o) RETURN_STD(2, 0);
  M32(ARG(1)) = o->finished ? o->exit_code : 259;
  RETURN_STD(2, 1);
}
API(k_SetThreadPriority) { RETURN_STD(2, 1); }
API(k_SetThreadIdealProcessor) { RETURN_STD(2, 0); }

/* ---- exceptions (minimal: report and stop) ---------------------------------- */
API(k_RaiseException) {
  u32 code = ARG(0), n = ARG(2), args = ARG(3);
  if (code == 0x406D1388) { /* MSVC SetThreadName: consumed as an attached debugger would */
    if (n >= 2 && M32(args) == 0x1000 && M32(args + 4)) TRACE("thread name: %s", STR(M32(args + 4)));
    RETURN_STD(4, 0);
  }
  if (code == 0xE06D7363 && n >= 3) {
    /* C++ throw: args[1] = object, args[2] = ThrowInfo */
    u32 ti = M32(args + 8);
    u32 cta = ti ? M32(ti + 12) : 0;
    u32 ct = cta ? M32(cta + 4) : 0;
    u32 td = ct ? M32(ct + 4) : 0;
    rt_fatal(c, "C++ exception thrown (type %s), exception unwinding is not implemented yet",
             td ? STR(td + 8) : "?");
  }
  rt_fatal(c, "RaiseException(%08x) - SEH dispatch is not implemented yet", code);
}
API(k_RtlUnwind) { rt_fatal(c, "RtlUnwind not implemented"); }
API(k_UnhandledExceptionFilter) { RETURN_STD(1, 1); }
API(k_SetUnhandledExceptionFilter) { RETURN_STD(1, 0); }

/* ---- files ----------------------------------------------------------------- */
static int win_fd(Ctx *c, u32 h) {
  if (h == 0x14 || h == 0x18) return h == 0x14 ? 1 : 2;
  HObj *o = h_get(h);
  return o && o->type == H_FILE ? o->fd : -1;
}
static u32 create_file(Ctx *c, const char *name, u32 access, u32 disp) {
  char hp[2048];
  fs_host_path(name, hp, sizeof hp);
  int fl = 0;
  if ((access & 0x40000000) && (access & 0x80000000)) fl = O_RDWR;
  else if (access & 0x40000000) fl = O_WRONLY;
  else fl = O_RDONLY;
  if (access & 0x10000000) fl = O_RDWR; /* GENERIC_ALL */
  switch (disp) {
  case 1: fl |= O_CREAT | O_EXCL; break;
  case 2: fl |= O_CREAT | O_TRUNC; break;
  case 4: fl |= O_CREAT; break;
  case 5: fl |= O_TRUNC; break;
  }
  struct stat st;
  int existed = stat(hp, &st) == 0;
  if (existed && S_ISDIR(st.st_mode)) { rt_set_error(c, 5); return 0xffffffff; }
  int fd = open(hp, fl, 0644);
  __atomic_add_fetch(&g_stat_opens, 1, __ATOMIC_RELAXED);
  TRACE("CreateFile(%s -> %s, %x, %u) = %d", name, hp, access, disp, fd);
  if (fd < 0) { rt_set_error(c, errno_to_win(errno)); return 0xffffffff; }
  u32 h = h_new(H_FILE);
  HObj *o = h_get(h);
  o->fd = fd;
  snprintf(o->name, sizeof o->name, "%s", name);
  rt_set_error(c, (existed && (disp == 2 || disp == 4)) ? 183 : 0);
  return h;
}
API(k_CreateFileA) { RETURN_STD(7, create_file(c, STR(ARG(0)), ARG(1), ARG(4))); }
API(k_CreateFileW) {
  char s[1024]; u32 p = ARG(0); int i = 0;
  while (i < 1023 && M16(p + 2 * i)) { s[i] = M16(p + 2 * i); i++; } s[i] = 0;
  RETURN_STD(7, create_file(c, s, ARG(1), ARG(4)));
}
API(k_ReadFile) {
  int fd = win_fd(c, ARG(0));
  if (fd < 0) { rt_set_error(c, 6); RETURN_STD(5, 0); }
  ssize_t r = read(fd, HP(ARG(1)), ARG(2));
  if (r > 0) __atomic_add_fetch(&g_stat_read, (u64)r, __ATOMIC_RELAXED);
  if (ARG(3)) M32(ARG(3)) = r < 0 ? 0 : r;
  RETURN_STD(5, r >= 0);
}
API(k_WriteFile) {
  int fd = win_fd(c, ARG(0));
  if (fd < 0) { rt_set_error(c, 6); RETURN_STD(5, 0); }
  ssize_t r = write(fd, HP(ARG(1)), ARG(2));
  if (ARG(3)) M32(ARG(3)) = r < 0 ? 0 : r;
  RETURN_STD(5, r >= 0);
}
API(k_SetFilePointer) {
  int fd = win_fd(c, ARG(0));
  s64 off = (s32)ARG(1);
  if (ARG(2)) off = (s64)(((u64)M32(ARG(2)) << 32) | ARG(1));
  off_t r = lseek(fd, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) { rt_set_error(c, 131); RETURN_STD(4, 0xffffffff); }
  if (ARG(2)) M32(ARG(2)) = (u32)((u64)r >> 32);
  rt_set_error(c, 0);
  RETURN_STD(4, (u32)r);
}
API(k_SetFilePointerEx) {
  int fd = win_fd(c, ARG(0));
  s64 off = (s64)(((u64)ARG(2) << 32) | ARG(1));
  off_t r = lseek(fd, off, ARG(4) == 0 ? SEEK_SET : ARG(4) == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) RETURN_STD(5, 0);
  if (ARG(3)) M64(ARG(3)) = r;
  RETURN_STD(5, 1);
}
API(k_GetFileSize) {
  struct stat st; int fd = win_fd(c, ARG(0));
  if (fd < 0 || fstat(fd, &st)) RETURN_STD(2, 0xffffffff);
  if (ARG(1)) M32(ARG(1)) = (u32)((u64)st.st_size >> 32);
  RETURN_STD(2, (u32)st.st_size);
}
API(k_GetFileSizeEx) {
  struct stat st; int fd = win_fd(c, ARG(0));
  if (fd < 0 || fstat(fd, &st)) RETURN_STD(2, 0);
  M64(ARG(1)) = st.st_size;
  RETURN_STD(2, 1);
}
API(k_FlushFileBuffers) { RETURN_STD(1, 1); }
API(k_SetEndOfFile) { int fd = win_fd(c, ARG(0)); off_t p = lseek(fd, 0, SEEK_CUR); RETURN_STD(1, ftruncate(fd, p) == 0); }
API(k_GetFileType) { u32 h = ARG(0); RETURN_STD(1, (h == 0x10 || h == 0x14 || h == 0x18) ? 2 : 1); }
API(k_DeleteFileA) { char hp[2048]; fs_host_path(STR(ARG(0)), hp, sizeof hp); RETURN_STD(1, unlink(hp) == 0); }
API(k_MoveFileA) {
  char a[2048], b[2048]; fs_host_path(STR(ARG(0)), a, sizeof a); fs_host_path(STR(ARG(1)), b, sizeof b);
  RETURN_STD(2, rename(a, b) == 0);
}
API(k_CopyFileA) {
  char a[2048], b[2048]; fs_host_path(STR(ARG(0)), a, sizeof a); fs_host_path(STR(ARG(1)), b, sizeof b);
  if (ARG(2) && access(b, F_OK) == 0) RETURN_STD(3, 0);
  FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL;
  if (!in || !out) { if (in) fclose(in); RETURN_STD(3, 0); }
  char buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, in))) fwrite(buf, 1, n, out);
  fclose(in); fclose(out);
  RETURN_STD(3, 1);
}
API(k_CreateDirectoryA) {
  char hp[2048]; fs_host_path(STR(ARG(0)), hp, sizeof hp);
  int r = mkdir(hp, 0755);
  if (r) rt_set_error(c, errno == EEXIST ? 183 : 3);
  RETURN_STD(2, r == 0);
}
static u32 attrs_of(struct stat *st) { return S_ISDIR(st->st_mode) ? 0x10 : 0x80; }
API(k_GetFileAttributesA) {
  char hp[2048]; struct stat st; fs_host_path(STR(ARG(0)), hp, sizeof hp);
  if (stat(hp, &st)) { rt_set_error(c, 2); if (g_trace > 1) rt_log("GetFileAttributes(%s) missing", STR(ARG(0))); RETURN_STD(1, 0xffffffff); }
  RETURN_STD(1, attrs_of(&st));
}
API(k_GetFullPathNameA) {
  const char *n = STR(ARG(0)); u32 cap = ARG(1), buf = ARG(2);
  char s[1024];
  if (n[1] == ':') snprintf(s, sizeof s, "%s", n); else snprintf(s, sizeof s, "%s\\%s", fs_game_win_dir(), n);
  u32 len = strlen(s);
  if (len + 1 > cap) RETURN_STD(4, len + 1);
  memcpy(HP(buf), s, len + 1);
  if (ARG(3)) { char *b = strrchr(s, '\\'); M32(ARG(3)) = b ? buf + (b - s) + 1 : buf; }
  RETURN_STD(4, len);
}
API(k_GetCurrentDirectoryA) {
  const char *s = fs_game_win_dir(); u32 len = strlen(s);
  if (ARG(0) < len + 1) RETURN_STD(2, len + 1);
  memcpy(HP(ARG(1)), s, len + 1);
  RETURN_STD(2, len);
}
API(k_GetDriveTypeA) { RETURN_STD(1, 3); }

static void fill_find(u32 p, const char *name, struct stat *st) {
  memset(HP(p), 0, 320);
  M32(p) = attrs_of(st);
  u64 ft = unix_to_ft(st->st_mtime, 0);
  M64(p + 4) = ft; M64(p + 12) = ft; M64(p + 20) = ft;
  M32(p + 28) = (u32)((u64)st->st_size >> 32); M32(p + 32) = (u32)st->st_size;
  snprintf((char *)HP(p + 44), 260, "%s", name);
}
static int find_next(HObj *o, u32 p) {
  struct dirent *e;
  while ((e = readdir(o->dir))) {
    if (!rt_wildmatch(o->pattern, e->d_name)) continue;
    char full[4096]; struct stat st;
    snprintf(full, sizeof full, "%s/%s", o->dirpath, e->d_name);
    if (stat(full, &st)) continue;
    fill_find(p, e->d_name, &st);
    return 1;
  }
  return 0;
}
API(k_FindFirstFileA) {
  const char *pat = STR(ARG(0));
  char w[2048]; snprintf(w, sizeof w, "%s", pat);
  char *sl = strrchr(w, '\\'); if (!sl) sl = strrchr(w, '/');
  const char *mask = sl ? sl + 1 : w;
  char dirw[2048];
  if (sl) { *sl = 0; snprintf(dirw, sizeof dirw, "%s", w); } else strcpy(dirw, ".");
  char hp[2048]; fs_host_path(dirw, hp, sizeof hp);
  DIR *d = opendir(hp);
  int savescan = rt_strcasestr(mask, ".fos") != NULL;
  if (g_trace > 1 || savescan) rt_log("FindFirstFile(%s) dir=%s%s", pat, hp, d ? "" : " (no such folder)");
  if (!d) { rt_set_error(c, 3); RETURN_STD(2, 0xffffffff); }
  u32 h = h_new(H_FIND); HObj *o = h_get(h);
  o->dir = d; o->pattern = strdup(!strcmp(mask, "*.*") ? "*" : mask); o->dirpath = strdup(hp);
  if (!find_next(o, ARG(1))) { h_close(h); rt_set_error(c, 2); RETURN_STD(2, 0xffffffff); }
  if (savescan) rt_log("  first match: %s", (char *)HP(ARG(1) + 44));
  RETURN_STD(2, h);
}
API(k_FindNextFileA) {
  HObj *o = h_get(ARG(0));
  if (!o || o->type != H_FIND || !find_next(o, ARG(1))) { rt_set_error(c, 18); RETURN_STD(2, 0); }
  RETURN_STD(2, 1);
}
API(k_FindClose) { h_close(ARG(0)); RETURN_STD(1, 1); }

/* ---- private profile (INI) ------------------------------------------------- */
static char *read_all(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc(n + 1); n = fread(b, 1, n, f); b[n] = 0; fclose(f);
  return b;
}
static void trim(char *s) {
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
  size_t k = strspn(s, " \t");
  if (k) memmove(s, s + k, strlen(s + k) + 1);
}
/* returns 1 and copies value if found */
static int ini_get(const char *file, const char *sect, const char *key, char *out, size_t cap) {
  char hp[2048]; fs_host_path(file, hp, sizeof hp);
  char *b = read_all(hp);
  if (!b) return 0;
  int in = 0, found = 0;
  for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) {
    char l[4096]; snprintf(l, sizeof l, "%s", line); trim(l);
    if (l[0] == '[') { char *e = strchr(l, ']'); if (e) *e = 0; in = !strcasecmp(l + 1, sect); continue; }
    if (!in || l[0] == ';') continue;
    char *eq = strchr(l, '='); if (!eq) continue;
    *eq = 0; char *k = l, *v = eq + 1; trim(k); trim(v);
    if (!strcasecmp(k, key)) {
      size_t n = strlen(v);
      if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { v[n - 1] = 0; v++; }
      snprintf(out, cap, "%s", v); found = 1; break;
    }
  }
  free(b);
  return found;
}
API(k_GetPrivateProfileStringA) {
  u32 sect = ARG(0), key = ARG(1), def = ARG(2), buf = ARG(3), cap = ARG(4), file = ARG(5);
  char v[4096];
  if (!sect || !key) { if (cap >= 2) { M8(buf) = 0; M8(buf + 1) = 0; } RETURN_STD(6, 0); }
  if (!ini_get(STR(file), STR(sect), STR(key), v, sizeof v)) snprintf(v, sizeof v, "%s", def ? STR(def) : "");
  u32 n = strlen(v);
  if (cap == 0) RETURN_STD(6, 0);
  if (n >= cap) n = cap - 1;
  memcpy(HP(buf), v, n); M8(buf + n) = 0;
  RETURN_STD(6, n);
}
API(k_GetPrivateProfileIntA) {
  char v[256];
  if (!ini_get(STR(ARG(3)), STR(ARG(0)), STR(ARG(1)), v, sizeof v)) RETURN_STD(4, ARG(2));
  RETURN_STD(4, (u32)strtol(v, NULL, 0));
}
API(k_WritePrivateProfileStringA) {
  if (!ARG(0) || !ARG(1)) RETURN_STD(4, 1);
  const char *sect = STR(ARG(0)), *key = STR(ARG(1)), *val = ARG(2) ? STR(ARG(2)) : NULL;
  char hp[2048]; fs_host_path(STR(ARG(3)), hp, sizeof hp);
  char *b = read_all(hp); if (!b) b = strdup("");
  size_t cap = strlen(b) + strlen(sect) + strlen(key) + (val ? strlen(val) : 0) + 64;
  char *o = malloc(cap); o[0] = 0;
  int in = 0, done = 0, seen = 0;
  char *save;
  for (char *line = strtok_r(b, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char l[4096]; snprintf(l, sizeof l, "%s", line); trim(l);
    if (l[0] == '[') {
      if (in && !done && val) { strcat(o, key); strcat(o, "="); strcat(o, val); strcat(o, "\r\n"); done = 1; }
      char *e = strchr(l, ']'); if (e) *e = 0; in = !strcasecmp(l + 1, sect); if (in) seen = 1;
    } else if (in && !done) {
      char *eq = strchr(l, '=');
      if (eq) { *eq = 0; trim(l); if (!strcasecmp(l, key)) { if (val) { strcat(o, key); strcat(o, "="); strcat(o, val); strcat(o, "\r\n"); } done = 1; continue; } }
    }
    strcat(o, line); strcat(o, "\n");
  }
  if (!done && val) {
    if (!(in && seen)) { strcat(o, "["); strcat(o, sect); strcat(o, "]\r\n"); }
    strcat(o, key); strcat(o, "="); strcat(o, val); strcat(o, "\r\n");
  }
  FILE *f = fopen(hp, "wb");
  if (f) { fputs(o, f); fclose(f); }
  free(o); free(b);
  RETURN_STD(4, f != NULL);
}

API(k_FormatMessageA) {
  u32 flags = ARG(0), buf = ARG(4), cap = ARG(5);
  char s[64]; snprintf(s, sizeof s, "Error %u\r\n", ARG(2));
  u32 n = strlen(s);
  if (flags & 0x100) { u32 p = guest_strdup(s); M32(buf) = p; RETURN_STD(7, n); }
  if (n >= cap) n = cap - 1;
  memcpy(HP(buf), s, n); M8(buf + n) = 0;
  RETURN_STD(7, n);
}

static const ApiEntry k32[] = {
#define E(n) {K, #n, k_##n}
  E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue),
  E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount), E(DeleteCriticalSection),
  E(EnterCriticalSection), E(TryEnterCriticalSection), E(LeaveCriticalSection),
  E(InterlockedIncrement), E(InterlockedDecrement), E(InterlockedExchange), E(InterlockedExchangeAdd),
  E(InterlockedCompareExchange),
  E(GetTickCount), E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(GetSystemTimeAsFileTime),
  E(GetLocalTime), E(GetSystemTime), E(FileTimeToSystemTime), E(FileTimeToLocalFileTime),
  E(SystemTimeToTzSpecificLocalTime), E(CompareFileTime), E(GetTimeZoneInformation),
  E(lstrcatA), E(lstrcpyA), E(MultiByteToWideChar), E(WideCharToMultiByte), E(GetACP), E(GetOEMCP),
  E(IsValidCodePage), E(GetCPInfo), E(GetStringTypeW), E(GetStringTypeA), E(LCMapStringW), E(LCMapStringA),
  E(CompareStringA), E(CompareStringW), E(GetLocaleInfoA), E(GetLocaleInfoW), E(IsValidLocale),
  E(GetUserDefaultLCID), E(EnumSystemLocalesA),
  E(GetCommandLineA), E(GetCommandLineW), E(GetEnvironmentStrings), E(GetEnvironmentStringsW),
  E(FreeEnvironmentStringsA), E(FreeEnvironmentStringsW), E(SetEnvironmentVariableA), E(GetStartupInfoA),
  E(GetCurrentProcess), E(GetCurrentProcessId), E(GetCurrentThread), E(GetCurrentThreadId), E(ExitProcess),
  E(TerminateProcess), E(IsDebuggerPresent), E(DebugBreak), E(OutputDebugStringA), E(GetLastError),
  E(SetLastError), E(SetHandleCount), E(GetStdHandle), E(SetStdHandle), E(GetConsoleCP), E(GetConsoleOutputCP),
  E(GetConsoleMode), E(WriteConsoleA), E(WriteConsoleW), E(GetSystemInfo), E(GlobalMemoryStatus),
  E(GlobalMemoryStatusEx),
  E(VirtualAlloc), E(VirtualFree), E(VirtualQuery), E(HeapCreate), E(GetProcessHeap), E(HeapAlloc), E(HeapFree),
  E(HeapSize), E(HeapReAlloc), E(LocalFree),
  E(GetModuleHandleA), E(GetModuleHandleW), E(LoadLibraryA), E(FreeLibrary), E(GetProcAddress),
  E(GetModuleFileNameA),
  E(CreateEventA), E(SetEvent), E(ResetEvent), E(CreateSemaphoreA), E(ReleaseSemaphore), E(CreateMutexA),
  E(WaitForSingleObject), E(WaitForMultipleObjects), E(Sleep), E(CloseHandle),
  E(CreateThread), E(ResumeThread), E(SuspendThread), E(ExitThread), E(GetExitCodeThread), E(SetThreadPriority),
  E(SetThreadIdealProcessor),
  E(RaiseException), E(RtlUnwind), E(UnhandledExceptionFilter), E(SetUnhandledExceptionFilter),
  E(CreateFileA), E(CreateFileW), E(ReadFile), E(WriteFile), E(SetFilePointer), E(SetFilePointerEx), E(GetFileSize),
  E(GetFileSizeEx), E(FlushFileBuffers), E(SetEndOfFile), E(GetFileType), E(DeleteFileA), E(MoveFileA), E(CopyFileA),
  E(CreateDirectoryA), E(GetFileAttributesA), E(GetFullPathNameA), E(GetCurrentDirectoryA), E(GetDriveTypeA),
  E(FindFirstFileA), E(FindNextFileA), E(FindClose),
  E(GetPrivateProfileStringA), E(GetPrivateProfileIntA), E(WritePrivateProfileStringA), E(FormatMessageA),
  {0}
#undef E
};

void kernel32_init(void) { api_register(k32); }
