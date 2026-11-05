/* WINMM buffered file I/O (mmio*) as the game uses it for .wav sounds: the game supplies its own I/O
 * procedure (0xAF0400, reads from the BSA archives) and always asks for a buffer (MMIO_ALLOCBUF). RIFF chunks
 * are walked with mmioDescend/mmioAscend; sample data is read with mmioRead or straight out of the buffer via
 * mmioGetInfo/mmioAdvance. Behaviour follows the Win32 documentation (and Wine's mmio.c for corner cases). */
#include "rt.h"
#include <pthread.h>
#include <string.h>

#define API(n) static void n(Ctx *c)
enum { MMIOM_READ = 0, MMIOM_WRITE = 1, MMIOM_SEEK = 2, MMIOM_OPEN = 3, MMIOM_CLOSE = 4 };
#define MMIO_ALLOCBUF 0x10000u
#define MMIO_FINDCHUNK 0x10u
#define MMIO_FINDRIFF 0x20u
#define MMIO_FINDLIST 0x40u
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_INVALPARAM 11
#define MMIOERR_CANNOTOPEN 259
#define MMIOERR_CANNOTSEEK 263
#define MMIOERR_CHUNKNOTFOUND 265
#define MMIOERR_UNBUFFERED 266
#define FOURCC_RIFF 0x46464952u
#define FOURCC_LIST 0x5453494Cu
#define DEFAULT_BUFFER 8192u

/* MMIOINFO field offsets (72 bytes); our private copy also carries 16 bytes of scratch after it */
enum { I_FLAGS = 0, I_FCC = 4, I_PROC = 8, I_ERR = 12, I_CCH = 20, I_BUF = 24, I_NEXT = 28, I_ENDREAD = 32,
       I_ENDWRITE = 36, I_BUFOFF = 40, I_DISKOFF = 44, I_INFO = 48, I_HMMIO = 68, I_SIZE = 72, I_SCRATCH = 72 };

#define MAX_MMIO 256
static u32 g_mm[MAX_MMIO]; /* guest MMIOINFO of each open handle (handle = index + 1); 0 = free */
static u8 g_own_buf[MAX_MMIO];
static pthread_mutex_t g_mm_lock = PTHREAD_MUTEX_INITIALIZER;

static u32 info_of(u32 h) { return h && h <= MAX_MMIO ? g_mm[h - 1] : 0; }
static int32_t proc(Ctx *c, u32 i, u32 msg, u32 a, u32 b) {
  u32 args[4] = {i, msg, a, b};
  return (int32_t)rt_call_guest(c, M32(i + I_PROC), 4, args);
}
static int32_t cur_pos(u32 i) {
  return M32(i + I_BUF) ? (int32_t)(M32(i + I_BUFOFF) + (M32(i + I_NEXT) - M32(i + I_BUF))) : (int32_t)M32(i + I_DISKOFF);
}
/* load the buffer with the data at absolute position pos; returns bytes loaded */
static int32_t fill(Ctx *c, u32 i, int32_t pos) {
  if ((int32_t)M32(i + I_DISKOFF) != pos && proc(c, i, MMIOM_SEEK, pos, 0) == -1) return -1;
  M32(i + I_BUFOFF) = pos;
  int32_t n = proc(c, i, MMIOM_READ, M32(i + I_BUF), M32(i + I_CCH));
  if (n < 0) n = 0;
  M32(i + I_NEXT) = M32(i + I_BUF);
  M32(i + I_ENDREAD) = M32(i + I_BUF) + n;
  return n;
}
static int32_t mseek(Ctx *c, u32 i, int32_t off, int origin) {
  if (!M32(i + I_BUF)) return proc(c, i, MMIOM_SEEK, off, origin);
  int32_t target;
  if (origin == 2) {
    target = proc(c, i, MMIOM_SEEK, off, 2);
    if (target == -1) return -1;
    M32(i + I_BUFOFF) = target;
    M32(i + I_NEXT) = M32(i + I_ENDREAD) = M32(i + I_BUF);
    return target;
  }
  target = origin == 1 ? cur_pos(i) + off : off;
  int32_t lo = (int32_t)M32(i + I_BUFOFF), hi = lo + (int32_t)(M32(i + I_ENDREAD) - M32(i + I_BUF));
  if (target >= lo && target <= hi) { M32(i + I_NEXT) = M32(i + I_BUF) + (target - lo); return target; }
  if (proc(c, i, MMIOM_SEEK, target, 0) == -1) return -1;
  M32(i + I_BUFOFF) = target;
  M32(i + I_NEXT) = M32(i + I_ENDREAD) = M32(i + I_BUF);
  return target;
}
static int32_t mread(Ctx *c, u32 i, u32 dst, int32_t n) {
  if (!M32(i + I_BUF)) return proc(c, i, MMIOM_READ, dst, n);
  int32_t done = 0;
  while (done < n) {
    int32_t avail = (int32_t)(M32(i + I_ENDREAD) - M32(i + I_NEXT));
    if (avail <= 0) {
      int32_t pos = (int32_t)(M32(i + I_BUFOFF) + (M32(i + I_ENDREAD) - M32(i + I_BUF)));
      if (fill(c, i, pos) <= 0) break;
      continue;
    }
    int32_t k = avail < n - done ? avail : n - done;
    memmove(HP(dst + done), HP(M32(i + I_NEXT)), k);
    M32(i + I_NEXT) += k;
    done += k;
  }
  return done;
}

/* HMMIO mmioOpenA(LPSTR file, LPMMIOINFO info, DWORD flags) */
API(w_mmioOpenA) {
  u32 fn = ARG(0), ci = ARG(1), flags = ARG(2);
  u32 i = heap_alloc(I_SIZE + 16, 1);
  if (ci) {
    M32(i + I_FCC) = M32(ci + I_FCC);
    M32(i + I_PROC) = M32(ci + I_PROC);
    M32(i + I_CCH) = M32(ci + I_CCH);
    M32(i + I_BUF) = M32(ci + I_BUF);
    memcpy(HP(i + I_INFO), HP(ci + I_INFO), 12);
  }
  M32(i + I_FLAGS) = flags;
  if (!M32(i + I_PROC)) {
    rt_log("mmioOpen(%s): only game-supplied I/O procedures are supported", fn ? (char *)HP(fn) : "");
    heap_free(i);
    if (ci) M32(ci + I_ERR) = MMIOERR_CANNOTOPEN;
    RETURN_STD(3, 0);
  }
  int own = 0;
  if ((flags & MMIO_ALLOCBUF) || M32(i + I_BUF)) {
    if (!M32(i + I_CCH)) M32(i + I_CCH) = DEFAULT_BUFFER;
    if (!M32(i + I_BUF)) { M32(i + I_BUF) = heap_alloc(M32(i + I_CCH), 0); own = 1; }
    M32(i + I_NEXT) = M32(i + I_ENDREAD) = M32(i + I_BUF);
    M32(i + I_ENDWRITE) = M32(i + I_BUF) + M32(i + I_CCH);
  } else M32(i + I_CCH) = 0;
  pthread_mutex_lock(&g_mm_lock);
  u32 h = 0;
  for (u32 k = 0; k < MAX_MMIO; k++) if (!g_mm[k]) { g_mm[k] = i; g_own_buf[k] = own; h = k + 1; break; }
  pthread_mutex_unlock(&g_mm_lock);
  if (!h) { rt_log("mmioOpen: more than %d files open", MAX_MMIO); }
  M32(i + I_HMMIO) = h;
  int32_t r = h ? proc(c, i, MMIOM_OPEN, fn, 0) : MMIOERR_CANNOTOPEN;
  if (r) {
    if (h) { pthread_mutex_lock(&g_mm_lock); g_mm[h - 1] = 0; pthread_mutex_unlock(&g_mm_lock); }
    if (own) heap_free(M32(i + I_BUF));
    heap_free(i);
    if (ci) M32(ci + I_ERR) = r;
    RETURN_STD(3, 0);
  }
  if (ci) M32(ci + I_ERR) = 0;
  static int logged;
  if (logged < 2 && fn) { logged++; rt_log("wav: opened %s", (char *)HP(fn)); }
  RETURN_STD(3, h);
}

/* MMRESULT mmioClose(HMMIO h, UINT flags) */
API(w_mmioClose) {
  u32 h = ARG(0), i = info_of(h);
  if (!i) RETURN_STD(2, MMSYSERR_INVALHANDLE);
  int32_t r = proc(c, i, MMIOM_CLOSE, ARG(1), 0);
  pthread_mutex_lock(&g_mm_lock);
  int own = g_own_buf[h - 1];
  g_mm[h - 1] = 0;
  pthread_mutex_unlock(&g_mm_lock);
  if (own) heap_free(M32(i + I_BUF));
  heap_free(i);
  RETURN_STD(2, r);
}

/* LONG mmioRead(HMMIO h, HPSTR buf, LONG n) */
API(w_mmioRead) {
  u32 i = info_of(ARG(0));
  if (!i) RETURN_STD(3, (u32)-1);
  RETURN_STD(3, (u32)mread(c, i, ARG(1), (int32_t)ARG(2)));
}

/* MMRESULT mmioDescend(HMMIO h, LPMMCKINFO ck, const MMCKINFO *parent, UINT flags)
 * MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags */
API(w_mmioDescend) {
  u32 i = info_of(ARG(0)), ck = ARG(1), parent = ARG(2), flags = ARG(3);
  if (!i || !ck) RETURN_STD(4, MMSYSERR_INVALPARAM);
  int64_t end = parent ? (int64_t)M32(parent + 12) + M32(parent + 4) : INT64_MAX;
  u32 want = 0, wtype = 0;
  if (flags & MMIO_FINDCHUNK) want = M32(ck);
  else if (flags & MMIO_FINDRIFF) { want = FOURCC_RIFF; wtype = M32(ck + 8); }
  else if (flags & MMIO_FINDLIST) { want = FOURCC_LIST; wtype = M32(ck + 8); }
  u32 tmp = i + I_SCRATCH;
  for (;;) {
    int32_t start = cur_pos(i);
    if (start + 8 > end || mread(c, i, tmp, 8) < 8) RETURN_STD(4, MMIOERR_CHUNKNOTFOUND);
    u32 id = M32(tmp), size = M32(tmp + 4), type = 0;
    int form = id == FOURCC_RIFF || id == FOURCC_LIST;
    if (form) { if (mread(c, i, tmp + 8, 4) < 4) RETURN_STD(4, MMIOERR_CHUNKNOTFOUND); type = M32(tmp + 8); }
    if (!want || (id == want && (!wtype || type == wtype))) {
      M32(ck) = id;
      M32(ck + 4) = size;
      M32(ck + 8) = type;
      M32(ck + 12) = start + 8;
      M32(ck + 16) = 0;
      if (mseek(c, i, start + 8 + (form ? 4 : 0), 0) == -1) RETURN_STD(4, MMIOERR_CANNOTSEEK);
      RETURN_STD(4, 0);
    }
    if (mseek(c, i, start + 8 + (int32_t)size + (int32_t)(size & 1), 0) == -1) RETURN_STD(4, MMIOERR_CHUNKNOTFOUND);
  }
}

/* MMRESULT mmioAscend(HMMIO h, LPMMCKINFO ck, UINT flags): move past the end of the chunk */
API(w_mmioAscend) {
  u32 i = info_of(ARG(0)), ck = ARG(1);
  if (!i || !ck) RETURN_STD(3, MMSYSERR_INVALPARAM);
  u32 size = M32(ck + 4);
  if (mseek(c, i, (int32_t)(M32(ck + 12) + size + (size & 1)), 0) == -1) RETURN_STD(3, MMIOERR_CANNOTSEEK);
  RETURN_STD(3, 0);
}

/* MMRESULT mmioGetInfo(HMMIO h, LPMMIOINFO out, UINT flags) */
API(w_mmioGetInfo) {
  u32 i = info_of(ARG(0)), out = ARG(1);
  if (!i || !out) RETURN_STD(3, MMSYSERR_INVALPARAM);
  memcpy(HP(out), HP(i), I_SIZE);
  RETURN_STD(3, 0);
}

/* MMRESULT mmioAdvance(HMMIO h, LPMMIOINFO info, UINT flags): reload the buffer with the next block */
API(w_mmioAdvance) {
  u32 i = info_of(ARG(0)), ci = ARG(1), flags = ARG(2);
  if (!i) RETURN_STD(3, MMSYSERR_INVALHANDLE);
  if (!M32(i + I_BUF)) RETURN_STD(3, MMIOERR_UNBUFFERED);
  if (ci) { M32(i + I_NEXT) = M32(ci + I_NEXT); M32(i + I_ENDREAD) = M32(ci + I_ENDREAD); }
  if (flags == 0) /* MMIO_READ */
    fill(c, i, (int32_t)(M32(i + I_BUFOFF) + (M32(i + I_ENDREAD) - M32(i + I_BUF))));
  if (ci) {
    M32(ci + I_NEXT) = M32(i + I_NEXT);
    M32(ci + I_ENDREAD) = M32(i + I_ENDREAD);
    M32(ci + I_ENDWRITE) = M32(i + I_ENDWRITE);
    M32(ci + I_BUFOFF) = M32(i + I_BUFOFF);
    M32(ci + I_DISKOFF) = M32(i + I_DISKOFF);
  }
  RETURN_STD(3, 0);
}

static const ApiEntry list[] = {
  {"WINMM.dll", "mmioOpenA", w_mmioOpenA}, {"WINMM.dll", "mmioClose", w_mmioClose},
  {"WINMM.dll", "mmioRead", w_mmioRead}, {"WINMM.dll", "mmioDescend", w_mmioDescend},
  {"WINMM.dll", "mmioAscend", w_mmioAscend}, {"WINMM.dll", "mmioGetInfo", w_mmioGetInfo},
  {"WINMM.dll", "mmioAdvance", w_mmioAdvance}, {0}};
void mmio_init(void) { api_register(list); }
