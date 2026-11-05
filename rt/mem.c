/* Guest address space: VirtualAlloc-style reservations and a heap for HeapAlloc. */
#define _GNU_SOURCE
#include "rt.h"
#include <sys/mman.h>
#include <string.h>

#define GRAN 0x10000u
#define VM_LO 0x00010000u
#define VM_HI 0x7FFF0000u
#define NGRAN (0x80000000u / GRAN)

static pthread_mutex_t g_vm_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_owner[NGRAN];  /* 0 = free, else base address of the reservation (+1 to keep 0 free) */
static u32 g_rsize[NGRAN];  /* reservation size, stored at the base granule */
static u32 g_vm_hint = 0x02000000u;

void mem_init(void) {
  void *p = mmap((void *)GUEST_BASE, 0x100000000ull, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
  if (p != (void *)GUEST_BASE) { perror("mmap guest space"); exit(2); }
  /* catch null-pointer dereferences and stray accesses to the thunk range */
  mprotect((void *)GUEST_BASE, VM_LO, PROT_NONE);
  mprotect((void *)(GUEST_BASE + THUNK_BASE), 0x10000000u, PROT_NONE);
  /* the image range is reserved */
  for (u32 a = IMAGE_BASE; a < IMAGE_BASE + g_image_size; a += GRAN) g_owner[a / GRAN] = IMAGE_BASE + 1;
  g_rsize[IMAGE_BASE / GRAN] = (g_image_size + GRAN - 1) & ~(GRAN - 1);
}

static int range_free(u32 a, u32 n) {
  for (u32 g = a / GRAN; g < (a + n) / GRAN; g++)
    if (g >= NGRAN || g_owner[g]) return 0;
  return 1;
}

static void reserve(u32 a, u32 n) {
  for (u32 g = a / GRAN; g < (a + n) / GRAN; g++) g_owner[g] = a + 1;
  g_rsize[a / GRAN] = n;
}

u32 vm_alloc(u32 addr, u32 size, u32 type, u32 prot) {
  if (!size) return 0;
  pthread_mutex_lock(&g_vm_lock);
  u32 r = 0;
  if (addr) {
    u32 g = addr / GRAN;
    if (g < NGRAN && g_owner[g]) {
      /* commit inside an existing reservation; reserving an address in use fails, as on Windows */
      r = (type & 0x2000) ? 0 : (addr & ~0xfffu);
      goto out;
    }
    u32 a = addr & ~(GRAN - 1);
    u32 n = ((addr + size - a) + GRAN - 1) & ~(GRAN - 1);
    if (a >= VM_LO && a + n <= VM_HI && range_free(a, n)) { reserve(a, n); r = (type & 0x2000) ? a : addr; }
    goto out;
  }
  u32 n = (size + GRAN - 1) & ~(GRAN - 1);
  int top_down = (type & 0x100000) != 0;
  if (top_down) {
    for (u32 a = VM_HI - n; a >= g_vm_hint; a -= GRAN)
      if (range_free(a, n)) { reserve(a, n); r = a; goto out; }
  }
  for (int pass = 0; pass < 2 && !r; pass++) {
    u32 start = pass == 0 ? g_vm_hint : VM_LO;
    for (u32 a = start; a + n <= VM_HI; a += GRAN) {
      if (range_free(a, n)) { reserve(a, n); r = a; if (pass == 0) g_vm_hint = a + n; break; }
    }
  }
out:
  pthread_mutex_unlock(&g_vm_lock);
  return r;
}

int vm_free(u32 addr, u32 size, u32 type) {
  pthread_mutex_lock(&g_vm_lock);
  u32 g = addr / GRAN;
  int ok = 0;
  if (g < NGRAN && g_owner[g]) {
    if (type & 0x8000) { /* MEM_RELEASE */
      u32 base = g_owner[g] - 1, n = g_rsize[base / GRAN];
      madvise(HP(base), n, MADV_DONTNEED);
      for (u32 k = base / GRAN; k < (base + n) / GRAN; k++) g_owner[k] = 0;
      if (base < g_vm_hint) g_vm_hint = base < 0x02000000u ? 0x02000000u : base;
      ok = 1;
    } else { /* MEM_DECOMMIT */
      u32 a = addr & ~0xfffu;
      u32 n = size ? ((addr + size - a + 0xfff) & ~0xfffu) : g_rsize[(g_owner[g] - 1) / GRAN];
      madvise(HP(a), n, MADV_DONTNEED);
      ok = 1;
    }
  }
  pthread_mutex_unlock(&g_vm_lock);
  return ok;
}

int vm_query(u32 addr, u32 *base, u32 *alloc_base, u32 *size, u32 *state) {
  pthread_mutex_lock(&g_vm_lock);
  u32 g = addr / GRAN;
  if (g >= NGRAN) { pthread_mutex_unlock(&g_vm_lock); return 0; }
  if (g_owner[g]) {
    u32 b = g_owner[g] - 1;
    *alloc_base = b;
    *base = addr & ~0xfffu;
    *size = b + g_rsize[b / GRAN] - *base;
    *state = 0x1000; /* MEM_COMMIT */
  } else {
    u32 e = g;
    while (e < NGRAN && !g_owner[e]) e++;
    *alloc_base = 0;
    *base = g * GRAN;
    *size = (e - g) * GRAN;
    *state = 0x10000; /* MEM_FREE */
  }
  pthread_mutex_unlock(&g_vm_lock);
  return 1;
}

/* ---- heap ------------------------------------------------------------------ */
/* Size-class allocator over 1 MiB+ slabs. Block layout: 16-byte header then data. */
#define HDR 16
#define NCLASS 96
#define BIG 0xffffu
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_class_size[NCLASS];
static int g_nclass;
static u32 g_free[NCLASS];
static u32 g_bump[NCLASS], g_bump_end[NCLASS];

static void heap_classes(void) {
  if (g_nclass) return;
  u32 s = 16;
  while (s <= 256 * 1024 && g_nclass < NCLASS) {
    g_class_size[g_nclass++] = s;
    if (s < 512) s += 16;
    else s += s / 4 & ~15u;
  }
}

static int class_of(u32 n) {
  for (int i = 0; i < g_nclass; i++)
    if (g_class_size[i] >= n) return i;
  return -1;
}

u32 heap_alloc(u32 size, int zero) {
  pthread_mutex_lock(&g_heap_lock);
  heap_classes();
  int k = class_of(size ? size : 1);
  u32 p;
  if (k < 0) {
    pthread_mutex_unlock(&g_heap_lock);
    u32 b = vm_alloc(0, size + HDR, 0x3000, 4);
    if (!b) return 0;
    M32(b) = 0x48454150; M32(b + 4) = BIG; M32(b + 8) = size;
    return b + HDR; /* fresh pages are zero */
  }
  u32 cs = g_class_size[k] + HDR;
  if (g_free[k]) {
    p = g_free[k];
    g_free[k] = M32(p + HDR);
  } else {
    if (g_bump[k] + cs > g_bump_end[k]) {
      u32 slab = cs * 64 > 0x100000 ? cs * 64 : 0x100000;
      u32 b = vm_alloc(0, slab, 0x3000, 4);
      if (!b) { pthread_mutex_unlock(&g_heap_lock); return 0; }
      g_bump[k] = b; g_bump_end[k] = b + slab;
    }
    p = g_bump[k];
    g_bump[k] += cs;
  }
  M32(p) = 0x48454150; M32(p + 4) = k; M32(p + 8) = size;
  pthread_mutex_unlock(&g_heap_lock);
  if (zero) memset(HP(p + HDR), 0, g_class_size[k]);
  return p + HDR;
}

void heap_free(u32 ptr) {
  if (!ptr) return;
  u32 p = ptr - HDR;
  if (M32(p) != 0x48454150) { rt_log("heap_free: bad pointer %08x", ptr); return; }
  u32 k = M32(p + 4);
  if (k == BIG) { M32(p) = 0; vm_free(p, 0, 0x8000); return; }
  pthread_mutex_lock(&g_heap_lock);
  M32(p + HDR) = g_free[k];
  g_free[k] = p;
  pthread_mutex_unlock(&g_heap_lock);
}

u32 heap_size(u32 ptr) {
  if (!ptr || M32(ptr - HDR) != 0x48454150) return 0xffffffffu;
  return M32(ptr - HDR + 8);
}

u32 heap_realloc(u32 ptr, u32 size, int zero) {
  if (!ptr) return heap_alloc(size, zero);
  u32 old = heap_size(ptr);
  u32 k = M32(ptr - HDR + 4);
  if (k != BIG && size <= g_class_size[k]) {
    if (zero && size > old) memset(HP(ptr + old), 0, size - old);
    M32(ptr - HDR + 8) = size;
    return ptr;
  }
  u32 n = heap_alloc(size, zero);
  if (!n) return 0;
  memcpy(HP(n), HP(ptr), old < size ? old : size);
  heap_free(ptr);
  return n;
}

u32 guest_strdup(const char *s) {
  u32 n = strlen(s) + 1;
  u32 p = heap_alloc(n, 0);
  memcpy(HP(p), s, n);
  return p;
}
