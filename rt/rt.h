/* Runtime internals shared by the loader and the Win32 layer. */
#pragma once
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define IMAGE_BASE 0x400000u
#define THUNK_BASE 0xF0000000u   /* host functions live at fake guest addresses here */
#define THUNK_MAX 65536

/* stdcall helpers for host implementations of guest-callable functions */
#define ESP (c->r[4])
#define ARG(n) M32(ESP + 4 + 4 * (n))
#define RETURN_STD(nargs, val) do { u32 _v = (u32)(val); c->r[0] = _v; ESP += 4 + 4 * (nargs); return; } while (0)
#define RETURN_CDECL(val) do { c->r[0] = (u32)(val); ESP += 4; return; } while (0)
#define STR(p) ((const char *)HP(p))
#define WSTR(p) ((const u16 *)HP(p))

typedef void (*HostFn)(Ctx *c);

/* logging */
extern int g_trace;
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
#define TRACE(...) do { if (g_trace) rt_log(__VA_ARGS__); } while (0)

/* host thunks */
u32 rt_thunk(HostFn f, const char *name);
const char *rt_thunk_name(u32 addr);

/* guest functions */
GuestFn rt_find_guest(u32 addr);
/* call a guest function with stdcall/cdecl args from host code; returns eax */
u32 rt_call_guest(Ctx *c, u32 addr, int nargs, const u32 *args);

/* memory */
void mem_init(void);
u32 vm_alloc(u32 addr, u32 size, u32 type, u32 prot);  /* VirtualAlloc */
int vm_free(u32 addr, u32 size, u32 type);
int vm_query(u32 addr, u32 *base, u32 *alloc_base, u32 *size, u32 *state);
u32 heap_alloc(u32 size, int zero);
void heap_free(u32 p);
u32 heap_size(u32 p);
u32 heap_realloc(u32 p, u32 size, int zero);
u32 guest_strdup(const char *s);

/* threads / TEB */
typedef struct HostThread HostThread;
Ctx *rt_new_ctx(u32 stack_size);
Ctx *rt_current(void);
u32 rt_teb_alloc(Ctx *c, u32 stack_lo, u32 stack_hi, u32 tid);
void rt_tls_clear_all(u32 slot); /* TlsAlloc/TlsFree: Windows zeroes the slot in every thread */
extern u32 g_peb;
u32 rt_thread_create(Ctx *parent, u32 start, u32 param, u32 stack, u32 flags, u32 *tid);

/* handles */
enum { H_NONE, H_FILE, H_EVENT, H_MUTEX, H_SEM, H_THREAD, H_FIND, H_HEAP, H_PROCESS, H_STDIO, H_MODULE };
typedef struct HObj {
  int type;
  int refs;
  /* sync objects */
  int signaled, manual, count, max;
  pthread_t owner_thread; int owner_valid; int recursion;
  /* threads */
  u32 exit_code; int finished; int suspended;
  pthread_cond_t resume;
  /* files */
  int fd; void *dir; char *pattern; char *dirpath;
  char name[64];
} HObj;
u32 h_new(int type);
HObj *h_get(u32 h);
void h_close(u32 h);
extern pthread_mutex_t g_wait_lock;
extern pthread_cond_t g_wait_cond;
void sync_signal_all(void);

/* filesystem */
extern char g_game_dir[1024];
extern char g_user_dir[1024];
int fs_host_path(const char *winpath, char *out, size_t outsz); /* 0 ok */
const char *fs_game_win_dir(void);

/* registration of API implementations */
typedef struct { const char *dll; const char *name; HostFn fn; } ApiEntry;
void api_register(const ApiEntry *list);
HostFn api_lookup(const char *dll, const char *name);
void api_init_all(void);
u32 rt_get_proc(const char *dll, const char *name); /* GetProcAddress for emulated DLLs */
void rt_set_error(Ctx *c, u32 e);

/* portable helpers: libc extensions the PS5 SDK declares are not all usable by an app
   (a missing one is called through NULL), so the runtime brings its own */
char *rt_strcasestr(const char *hay, const char *needle);
void rt_event_set(u32 handle);                                /* SetEvent from host code */
int rt_wildmatch(const char *pat, const char *s);            /* '*' and '?', ignoring case; 1 = match */
const char *rt_realpath(const char *p, char *buf, size_t n); /* absolute path, or p itself */
void rt_exit(int code) __attribute__((noreturn));

/* counters for the heartbeat log line */
extern u64 g_stat_valloc, g_stat_opens, g_stat_read;
extern u64 g_d3d_frames;
void rt_heartbeat_start(void);
