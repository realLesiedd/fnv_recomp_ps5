/* DirectSound 8: secondary buffers (static, streaming, duplicates), 3D buffers and the listener, position
 * notifications. Everything is mixed in software to stereo s16 and played through the platform audio output
 * (SDL: PulseAudio/ALSA on Linux, sceAudioOut on PS5). Like DirectSound on Vista and later, no hardware
 * voices are reported. Sample data lives in guest memory so Lock() can hand the game guest pointers. */
#include "com.h"
#include "platform.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define API(n) static void n(Ctx *c)
#define DS_OK 0
#define E_NOINTERFACE 0x80004002u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_OUTOFMEMORY 0x8007000Eu
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_BADFORMAT 0x88780064u
#define DSBCAPS_PRIMARYBUFFER 0x1
#define DSBCAPS_CTRL3D 0x10
#define DSBCAPS_MUTE3DATMAXDISTANCE 0x20000
#define DSBPLAY_LOOPING 1
#define DSBLOCK_FROMWRITECURSOR 1
#define DSBLOCK_ENTIREBUFFER 2
#define DS3DMODE_HEADRELATIVE 1
#define DS3DMODE_DISABLE 2
#define DSBPN_OFFSETSTOP 0xFFFFFFFFu
#define MAX_BUFS 4096
#define MAX_NOTIFY 32

typedef struct { u32 guest, bytes; int refs; } Data;
typedef struct {
  int used, refs, primary;            /* refs: interfaces with a nonzero count of their own */
  u32 obj, obj3d, objnotify;         /* guest interface objects ([vtbl][refs][buffer index]) */
  Data *data;
  u32 flags;
  int channels, bits, block, rate;   /* format */
  u32 frames;                        /* buffer length in frames */
  u32 freq;
  int volume, pan;                   /* hundredths of a dB */
  int playing, looping;
  double pos;                        /* play position in frames */
  float p[3], v[3], mind, maxd;
  u32 mode;
  int nnotify;
  u32 n_off[MAX_NOTIFY], n_ev[MAX_NOTIFY];
} Buf;

static Buf g_buf[MAX_BUFS];
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_rate = 48000;
static int g_master = 0; /* primary buffer volume */
static struct { float p[3], v[3], front[3], top[3], dist, rolloff, doppler; } g_lis = {
  {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1, 1, 1};
static u32 g_vt_ds, g_vt_buf, g_vt_3d, g_vt_lis, g_vt_notify, g_ds_obj, g_lis_obj;
static volatile int g_voices; /* playing secondary buffers, for the heartbeat */
void dshow_mix(float *acc, int frames, int out_rate);
void bink_mix(float *acc, int frames, int out_rate);

static float argf(Ctx *c, int i) { u32 v = ARG(i); float f; memcpy(&f, &v, 4); return f; }
static float rdf(u32 a) { u32 v = M32(a); float f; memcpy(&f, &v, 4); return f; }
static void wrf(u32 a, float f) { u32 v; memcpy(&v, &f, 4); M32(a) = v; }
static void rdv(u32 a, float v[3]) { v[0] = rdf(a); v[1] = rdf(a + 4); v[2] = rdf(a + 8); }
static void wrv(u32 a, const float v[3]) { wrf(a, v[0]); wrf(a + 4, v[1]); wrf(a + 8, v[2]); }
static Buf *B(Ctx *c) { u32 i = M32(COM_THIS + 8); return i < MAX_BUFS && g_buf[i].used ? &g_buf[i] : NULL; }
static u32 make_obj(u32 vt, int i) { u32 o = com_object(vt, 4); M32(o + 8) = i; return o; }
static u32 cursor_bytes(Buf *b) { u32 p = (u32)b->pos * b->block; return b->data && p >= b->data->bytes ? 0 : p; }

static int alloc_buf(void) {
  for (int i = 0; i < MAX_BUFS; i++)
    if (!g_buf[i].used) {
      memset(&g_buf[i], 0, sizeof g_buf[i]);
      Buf *b = &g_buf[i];
      b->used = b->refs = 1;
      b->mind = 1;
      b->maxd = 1e9f;
      return i;
    }
  return -1;
}
static void notify_stop(Buf *b) {
  for (int k = 0; k < b->nnotify; k++)
    if (b->n_off[k] == DSBPN_OFFSETSTOP) rt_event_set(b->n_ev[k]);
}
static void free_buf(Buf *b) { /* g_mx held */
  if (b->data && --b->data->refs == 0) { heap_free(b->data->guest); free(b->data); }
  if (b->obj) heap_free(b->obj);
  if (b->obj3d) heap_free(b->obj3d);
  if (b->objnotify) heap_free(b->objnotify);
  b->used = 0;
}

/* ---- mixer ------------------------------------------------------------------------------------------ */
static float db100(int v) { return v <= -10000 ? 0.f : powf(10.f, v / 2000.f); }
static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void gains(Buf *b, float *gl, float *gr) {
  float vol = db100(b->volume), l = b->pan > 0 ? db100(-b->pan) : 1.f, r = b->pan < 0 ? db100(b->pan) : 1.f;
  if ((b->flags & DSBCAPS_CTRL3D) && b->mode != DS3DMODE_DISABLE) {
    float rel[3], right[3] = {1, 0, 0};
    if (b->mode == DS3DMODE_HEADRELATIVE) memcpy(rel, b->p, sizeof rel);
    else {
      for (int k = 0; k < 3; k++) rel[k] = b->p[k] - g_lis.p[k];
      const float *t = g_lis.top, *f = g_lis.front; /* right = top x front (left-handed) */
      right[0] = t[1] * f[2] - t[2] * f[1];
      right[1] = t[2] * f[0] - t[0] * f[2];
      right[2] = t[0] * f[1] - t[1] * f[0];
      float n = sqrtf(dot(right, right));
      if (n > 1e-6f) for (int k = 0; k < 3; k++) right[k] /= n;
    }
    float dist = sqrtf(dot(rel, rel)), d = dist < b->maxd ? dist : b->maxd;
    float att = d <= b->mind ? 1.f : b->mind / (b->mind + g_lis.rolloff * (d - b->mind));
    if ((b->flags & DSBCAPS_MUTE3DATMAXDISTANCE) && dist > b->maxd) att = 0;
    float x = dist > 1e-4f ? dot(rel, right) / dist : 0.f; /* -1 left .. +1 right */
    l *= att * (x > 0 ? 1.f - 0.8f * x : 1.f);
    r *= att * (x < 0 ? 1.f + 0.8f * x : 1.f);
  }
  *gl = vol * l;
  *gr = vol * r;
}

static inline float sample(const u8 *d, const Buf *b, u32 frame, int ch) {
  const u8 *p = d + (size_t)frame * b->block + ch * (b->bits / 8);
  if (b->bits == 16) { int16_t v; memcpy(&v, p, 2); return v; }
  return (float)((int)*p - 128) * 256.f;
}

/* fire notifications whose offsets lie in [from, to) bytes, wrapping at the buffer end */
static void notify_range(Buf *b, u32 from, u32 to) {
  for (int k = 0; k < b->nnotify; k++) {
    u32 o = b->n_off[k];
    if (o == DSBPN_OFFSETSTOP) continue;
    if (from <= to ? (o >= from && o < to) : (o >= from || o < to)) rt_event_set(b->n_ev[k]);
  }
}

static void mix(int16_t *out, int frames) {
  static float *acc;
  static int cap;
  if (frames > cap) { cap = frames; acc = realloc(acc, sizeof(float) * 2 * cap); }
  memset(acc, 0, sizeof(float) * 2 * frames);
  int voices = 0;
  pthread_mutex_lock(&g_mx);
  for (int i = 0; i < MAX_BUFS; i++) {
    Buf *b = &g_buf[i];
    if (!b->used || !b->playing || b->primary || !b->data || !b->frames) continue;
    voices++;
    float gl, gr;
    gains(b, &gl, &gr);
    const u8 *d = HP(b->data->guest);
    double step = (double)b->freq / g_rate, pos = b->pos;
    u32 from = cursor_bytes(b);
    int stereo = b->channels == 2;
    for (int k = 0; k < frames; k++) {
      u32 i0 = (u32)pos, i1 = i0 + 1 < b->frames ? i0 + 1 : (b->looping ? 0 : i0);
      float t = (float)(pos - i0);
      float s0 = sample(d, b, i0, 0), s1 = sample(d, b, i1, 0), sl = s0 + (s1 - s0) * t, sr = sl;
      if (stereo) { s0 = sample(d, b, i0, 1); s1 = sample(d, b, i1, 1); sr = s0 + (s1 - s0) * t; }
      acc[2 * k] += sl * gl;
      acc[2 * k + 1] += sr * gr;
      pos += step;
      if (pos >= b->frames) {
        if (b->looping) { while (pos >= b->frames) pos -= b->frames; }
        else { b->playing = 0; pos = 0; break; }
      }
    }
    b->pos = pos;
    if (b->nnotify) {
      if (b->playing) notify_range(b, from, cursor_bytes(b));
      else { notify_range(b, from, b->data->bytes); notify_stop(b); }
    }
  }
  static int debug = -1;
  static double next_dump;
  if (debug < 0) debug = getenv("FNV_AUDIO_DEBUG") != NULL;
  if (debug) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (now >= next_dump) {
      next_dump = now + 5;
      for (int i = 0; i < MAX_BUFS; i++) {
        Buf *b = &g_buf[i];
        if (!b->used || !b->playing || b->primary || !b->data) continue;
        const u8 *d = HP(b->data->guest);
        int peak = 0;
        for (u32 f = 0; f < b->frames; f += 7) { int v = (int)sample(d, b, f, 0); v = v < 0 ? -v : v; if (v > peak) peak = v; }
        float gl, gr;
        gains(b, &gl, &gr);
        if (getenv("FNV_AUDIO_DUMP")) {
          char fn[256];
          snprintf(fn, sizeof fn, "%s/buf%03d_%uch_%uhz_%ub_%u.raw", getenv("FNV_AUDIO_DUMP"), i, b->channels, b->rate, b->bits,
                   b->data->bytes);
          FILE *f = fopen(fn, "wb");
          if (f) { fwrite(d, 1, b->data->bytes, f); fclose(f); }
        }
        rt_log("audio: buf %d flags %x %dch %dHz %dbit %u bytes freq %u vol %d pan %d %s pos %u mode %u "
               "p(%.0f,%.0f,%.0f) min %.0f max %.0f gain %.2f/%.2f data peak %d notify %d",
               i, b->flags, b->channels, b->rate, b->bits, b->data->bytes, b->freq, b->volume, b->pan,
               b->looping ? "loop" : "once", (u32)b->pos, b->mode, b->p[0], b->p[1], b->p[2], b->mind, b->maxd,
               gl, gr, peak, b->nnotify);
      }
    }
  }
  pthread_mutex_unlock(&g_mx);
  dshow_mix(acc, frames, g_rate); /* music (dshow.c) */
  bink_mix(acc, frames, g_rate);  /* movie sound (bink.c) */
  g_voices = voices;
  float m = db100(g_master);
  for (int k = 0; k < 2 * frames; k++) {
    float v = acc[k] * m;
    out[k] = v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int16_t)v;
  }
}

/* no audio device: keep play cursors moving in real time so the game's streams still advance */
static void *pace_thread(void *arg) {
  (void)arg;
  int16_t scratch[2 * 480];
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  unsigned long long done = 0;
  for (;;) {
    usleep(10000);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    unsigned long long due = (unsigned long long)(((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9) * g_rate);
    while (done < due) { int n = due - done > 480 ? 480 : (int)(due - done); mix(scratch, n); done += n; }
  }
  return NULL;
}
int dsound_voices(void) { return g_voices; }

/* ---- IDirectSoundBuffer8 -----------------------------------------------------------------------------
 * Each interface (buffer, 3D, notify) keeps its own reference count in its object, as in DirectSound: the
 * game releases each one in a loop until it reaches 0. The buffer goes away when all of them have. */
static u32 iface_addref(Buf *b, u32 obj) { /* g_mx held */
  if (M32(obj + 4)++ == 0) b->refs++;
  return M32(obj + 4);
}
API(b_AddRef) { pthread_mutex_lock(&g_mx); Buf *b = B(c); u32 r = b ? iface_addref(b, COM_THIS) : 0; pthread_mutex_unlock(&g_mx); RETURN_STD(1, r); }
API(b_Release) {
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  u32 r = 0, obj = COM_THIS;
  if (b && M32(obj + 4)) {
    r = --M32(obj + 4);
    if (!r && --b->refs == 0) free_buf(b);
  }
  pthread_mutex_unlock(&g_mx);
  RETURN_STD(1, r);
}
API(b_QueryInterface) {
  u32 iid = M32(ARG(1)), out = ARG(2), o = 0;
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  if (b) {
    int i = (int)(b - g_buf);
    switch (iid) {
    case 0: case 0x279AFA85: case 0x6825A449: o = b->obj; break; /* IUnknown, IDirectSoundBuffer(8) */
    case 0x279AFA86: if (!b->obj3d) { b->obj3d = make_obj(g_vt_3d, i); M32(b->obj3d + 4) = 0; } o = b->obj3d; break;
    case 0xB0210783: if (!b->objnotify) { b->objnotify = make_obj(g_vt_notify, i); M32(b->objnotify + 4) = 0; } o = b->objnotify; break;
    case 0x279AFA84: o = g_lis_obj; break; /* IDirectSound3DListener, from the primary buffer */
    }
    if (o && o != g_lis_obj) iface_addref(b, o);
  }
  pthread_mutex_unlock(&g_mx);
  if (out) M32(out) = o;
  if (!o) {
    static u32 seen[16];
    int k = 0;
    while (k < 16 && seen[k] && seen[k] != iid) k++;
    if (k < 16 && !seen[k]) { seen[k] = iid; rt_log("DirectSound: buffer QueryInterface for unsupported {%08x-...}", iid); }
  }
  RETURN_STD(3, o ? DS_OK : E_NOINTERFACE);
}
API(b_GetCaps) {
  Buf *b = B(c);
  u32 p = ARG(1);
  if (!b || !p) RETURN_STD(2, DSERR_INVALIDPARAM);
  M32(p + 4) = b->flags;
  M32(p + 8) = b->data ? b->data->bytes : 0;
  M32(p + 12) = 0;
  M32(p + 16) = 0;
  RETURN_STD(2, DS_OK);
}
API(b_GetCurrentPosition) {
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  u32 pos = b && !b->primary ? cursor_bytes(b) : 0;
  pthread_mutex_unlock(&g_mx);
  if (ARG(1)) M32(ARG(1)) = pos;
  if (ARG(2)) M32(ARG(2)) = pos;
  RETURN_STD(3, b ? DS_OK : DSERR_INVALIDCALL);
}
API(b_GetFormat) {
  Buf *b = B(c);
  u32 w = ARG(1), size = ARG(2);
  if (ARG(3)) M32(ARG(3)) = 18;
  if (b && w && size >= 16) {
    M16(w) = 1;
    M16(w + 2) = b->channels;
    M32(w + 4) = b->rate;
    M32(w + 8) = b->rate * b->block;
    M16(w + 12) = b->block;
    M16(w + 14) = b->bits;
    if (size >= 18) M16(w + 16) = 0;
  }
  RETURN_STD(4, DS_OK);
}
API(b_GetVolume) { Buf *b = B(c); if (ARG(1)) M32(ARG(1)) = b ? (u32)(b->primary ? g_master : b->volume) : 0; RETURN_STD(2, DS_OK); }
API(b_GetPan) { Buf *b = B(c); if (ARG(1)) M32(ARG(1)) = b ? (u32)b->pan : 0; RETURN_STD(2, DS_OK); }
API(b_GetFrequency) { Buf *b = B(c); if (ARG(1)) M32(ARG(1)) = b ? b->freq : 0; RETURN_STD(2, DS_OK); }
API(b_GetStatus) {
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  u32 s = b && b->playing ? 1u | (b->looping ? 4u : 0u) : 0u;
  pthread_mutex_unlock(&g_mx);
  if (ARG(1)) M32(ARG(1)) = s;
  RETURN_STD(2, DS_OK);
}
API(b_Lock) {
  u32 off = ARG(1), n = ARG(2), p1 = ARG(3), n1 = ARG(4), p2 = ARG(5), n2 = ARG(6), flags = ARG(7);
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  if (!b || !b->data) { pthread_mutex_unlock(&g_mx); RETURN_STD(8, DSERR_INVALIDCALL); }
  u32 total = b->data->bytes, base = b->data->guest;
  if (flags & DSBLOCK_ENTIREBUFFER) n = total;
  if (flags & DSBLOCK_FROMWRITECURSOR) off = cursor_bytes(b);
  pthread_mutex_unlock(&g_mx);
  if (off >= total || n > total || !p1 || !n1) RETURN_STD(8, DSERR_INVALIDPARAM);
  u32 first = n < total - off ? n : total - off;
  M32(p1) = base + off;
  M32(n1) = first;
  if (p2) M32(p2) = first < n ? base : 0;
  if (n2) M32(n2) = n - first;
  RETURN_STD(8, DS_OK);
}
API(b_Play) {
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  if (b && !b->primary) {
    if (b->pos >= b->frames) b->pos = 0;
    b->playing = 1;
    b->looping = ARG(3) & DSBPLAY_LOOPING;
  }
  pthread_mutex_unlock(&g_mx);
  RETURN_STD(4, b ? DS_OK : DSERR_INVALIDCALL);
}
API(b_SetCurrentPosition) {
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  if (b && b->block) b->pos = (double)(ARG(1) / b->block) < b->frames ? (double)(ARG(1) / b->block) : 0;
  pthread_mutex_unlock(&g_mx);
  RETURN_STD(2, DS_OK);
}
API(b_SetVolume) {
  int v = (int)ARG(1);
  v = v < -10000 ? -10000 : v > 0 ? 0 : v;
  Buf *b = B(c);
  if (b) { if (b->primary) g_master = v; else b->volume = v; }
  RETURN_STD(2, DS_OK);
}
API(b_SetPan) { int v = (int)ARG(1); Buf *b = B(c); if (b) b->pan = v < -10000 ? -10000 : v > 10000 ? 10000 : v; RETURN_STD(2, DS_OK); }
API(b_SetFrequency) {
  u32 f = ARG(1);
  Buf *b = B(c);
  if (b) b->freq = f ? (f < 100 ? 100 : f > 200000 ? 200000 : f) : (u32)b->rate;
  RETURN_STD(2, DS_OK);
}
API(b_Stop) {
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  if (b && b->playing) { b->playing = 0; notify_stop(b); }
  pthread_mutex_unlock(&g_mx);
  RETURN_STD(1, DS_OK);
}
API(b_GetObjectInPath) { if (ARG(4)) M32(ARG(4)) = 0; RETURN_STD(5, E_NOINTERFACE); }

static const ComMethod buf_methods[] = {
  {"IDirectSoundBuffer8::QueryInterface", 3, b_QueryInterface}, {"IDirectSoundBuffer8::AddRef", 1, b_AddRef},
  {"IDirectSoundBuffer8::Release", 1, b_Release}, {"IDirectSoundBuffer8::GetCaps", 2, b_GetCaps},
  {"IDirectSoundBuffer8::GetCurrentPosition", 3, b_GetCurrentPosition},
  {"IDirectSoundBuffer8::GetFormat", 4, b_GetFormat}, {"IDirectSoundBuffer8::GetVolume", 2, b_GetVolume},
  {"IDirectSoundBuffer8::GetPan", 2, b_GetPan}, {"IDirectSoundBuffer8::GetFrequency", 2, b_GetFrequency},
  {"IDirectSoundBuffer8::GetStatus", 2, b_GetStatus}, {"IDirectSoundBuffer8::Initialize", 3},
  {"IDirectSoundBuffer8::Lock", 8, b_Lock}, {"IDirectSoundBuffer8::Play", 4, b_Play},
  {"IDirectSoundBuffer8::SetCurrentPosition", 2, b_SetCurrentPosition},
  {"IDirectSoundBuffer8::SetFormat", 2}, {"IDirectSoundBuffer8::SetVolume", 2, b_SetVolume},
  {"IDirectSoundBuffer8::SetPan", 2, b_SetPan}, {"IDirectSoundBuffer8::SetFrequency", 2, b_SetFrequency},
  {"IDirectSoundBuffer8::Stop", 1, b_Stop}, {"IDirectSoundBuffer8::Unlock", 5},
  {"IDirectSoundBuffer8::Restore", 1}, {"IDirectSoundBuffer8::SetFX", 4},
  {"IDirectSoundBuffer8::AcquireResources", 4}, {"IDirectSoundBuffer8::GetObjectInPath", 5, b_GetObjectInPath},
};

/* ---- IDirectSound3DBuffer8 --------------------------------------------------------------------------- */
API(b3_GetAllParameters) {
  Buf *b = B(c);
  u32 p = ARG(1);
  if (b && p) {
    wrv(p + 4, b->p); wrv(p + 16, b->v);
    M32(p + 28) = 360; M32(p + 32) = 360;
    float o[3] = {0, 0, 1};
    wrv(p + 36, o);
    M32(p + 48) = 0;
    wrf(p + 52, b->mind); wrf(p + 56, b->maxd);
    M32(p + 60) = b->mode;
  }
  RETURN_STD(2, DS_OK);
}
API(b3_GetMaxDistance) { Buf *b = B(c); if (b && ARG(1)) wrf(ARG(1), b->maxd); RETURN_STD(2, DS_OK); }
API(b3_GetMinDistance) { Buf *b = B(c); if (b && ARG(1)) wrf(ARG(1), b->mind); RETURN_STD(2, DS_OK); }
API(b3_GetMode) { Buf *b = B(c); if (b && ARG(1)) M32(ARG(1)) = b->mode; RETURN_STD(2, DS_OK); }
API(b3_GetPosition) { Buf *b = B(c); if (b && ARG(1)) wrv(ARG(1), b->p); RETURN_STD(2, DS_OK); }
API(b3_GetVelocity) { Buf *b = B(c); if (b && ARG(1)) wrv(ARG(1), b->v); RETURN_STD(2, DS_OK); }
API(b3_SetAllParameters) {
  Buf *b = B(c);
  u32 p = ARG(1);
  if (b && p) {
    rdv(p + 4, b->p); rdv(p + 16, b->v);
    b->mind = rdf(p + 52); b->maxd = rdf(p + 56);
    b->mode = M32(p + 60);
  }
  RETURN_STD(3, DS_OK);
}
API(b3_SetMaxDistance) { Buf *b = B(c); if (b) b->maxd = argf(c, 1); RETURN_STD(3, DS_OK); }
API(b3_SetMinDistance) { Buf *b = B(c); if (b) b->mind = argf(c, 1); RETURN_STD(3, DS_OK); }
API(b3_SetMode) { Buf *b = B(c); if (b) b->mode = ARG(1); RETURN_STD(3, DS_OK); }
API(b3_SetPosition) { Buf *b = B(c); if (b) { b->p[0] = argf(c, 1); b->p[1] = argf(c, 2); b->p[2] = argf(c, 3); } RETURN_STD(5, DS_OK); }
API(b3_SetVelocity) { Buf *b = B(c); if (b) { b->v[0] = argf(c, 1); b->v[1] = argf(c, 2); b->v[2] = argf(c, 3); } RETURN_STD(5, DS_OK); }

static const ComMethod b3_methods[] = {
  {"IDirectSound3DBuffer8::QueryInterface", 3, b_QueryInterface}, {"IDirectSound3DBuffer8::AddRef", 1, b_AddRef},
  {"IDirectSound3DBuffer8::Release", 1, b_Release},
  {"IDirectSound3DBuffer8::GetAllParameters", 2, b3_GetAllParameters},
  {"IDirectSound3DBuffer8::GetConeAngles", 3}, {"IDirectSound3DBuffer8::GetConeOrientation", 2},
  {"IDirectSound3DBuffer8::GetConeOutsideVolume", 2},
  {"IDirectSound3DBuffer8::GetMaxDistance", 2, b3_GetMaxDistance},
  {"IDirectSound3DBuffer8::GetMinDistance", 2, b3_GetMinDistance}, {"IDirectSound3DBuffer8::GetMode", 2, b3_GetMode},
  {"IDirectSound3DBuffer8::GetPosition", 2, b3_GetPosition}, {"IDirectSound3DBuffer8::GetVelocity", 2, b3_GetVelocity},
  {"IDirectSound3DBuffer8::SetAllParameters", 3, b3_SetAllParameters},
  {"IDirectSound3DBuffer8::SetConeAngles", 4}, {"IDirectSound3DBuffer8::SetConeOrientation", 5},
  {"IDirectSound3DBuffer8::SetConeOutsideVolume", 3},
  {"IDirectSound3DBuffer8::SetMaxDistance", 3, b3_SetMaxDistance},
  {"IDirectSound3DBuffer8::SetMinDistance", 3, b3_SetMinDistance}, {"IDirectSound3DBuffer8::SetMode", 3, b3_SetMode},
  {"IDirectSound3DBuffer8::SetPosition", 5, b3_SetPosition}, {"IDirectSound3DBuffer8::SetVelocity", 5, b3_SetVelocity},
};

/* ---- IDirectSoundNotify ------------------------------------------------------------------------------ */
API(n_SetNotificationPositions) {
  u32 n = ARG(1), arr = ARG(2);
  pthread_mutex_lock(&g_mx);
  Buf *b = B(c);
  if (b) {
    b->nnotify = n < MAX_NOTIFY ? (int)n : MAX_NOTIFY;
    for (int k = 0; k < b->nnotify; k++) { b->n_off[k] = M32(arr + 8 * k); b->n_ev[k] = M32(arr + 8 * k + 4); }
  }
  pthread_mutex_unlock(&g_mx);
  if (n > MAX_NOTIFY) rt_log("DirectSound: %u notification positions, keeping %d", n, MAX_NOTIFY);
  RETURN_STD(3, b ? DS_OK : DSERR_INVALIDCALL);
}
static const ComMethod notify_methods[] = {
  {"IDirectSoundNotify::QueryInterface", 3, b_QueryInterface}, {"IDirectSoundNotify::AddRef", 1, b_AddRef},
  {"IDirectSoundNotify::Release", 1, b_Release},
  {"IDirectSoundNotify::SetNotificationPositions", 3, n_SetNotificationPositions},
};

/* ---- IDirectSound3DListener8 (one per device; never freed) ------------------------------------------- */
API(l_AddRef) { RETURN_STD(1, 1); }
API(l_Release) { RETURN_STD(1, 1); }
API(l_QueryInterface) {
  u32 iid = M32(ARG(1));
  u32 o = iid == 0x279AFA84 || iid == 0 ? g_lis_obj : 0;
  if (ARG(2)) M32(ARG(2)) = o;
  RETURN_STD(3, o ? DS_OK : E_NOINTERFACE);
}
API(l_GetAllParameters) {
  u32 p = ARG(1);
  if (p) {
    wrv(p + 4, g_lis.p); wrv(p + 16, g_lis.v);
    wrf(p + 28, g_lis.dist); wrf(p + 32, g_lis.rolloff); wrf(p + 36, g_lis.doppler);
    wrv(p + 40, g_lis.front); wrv(p + 52, g_lis.top);
  }
  RETURN_STD(2, DS_OK);
}
API(l_GetDistanceFactor) { if (ARG(1)) wrf(ARG(1), g_lis.dist); RETURN_STD(2, DS_OK); }
API(l_GetDopplerFactor) { if (ARG(1)) wrf(ARG(1), g_lis.doppler); RETURN_STD(2, DS_OK); }
API(l_GetOrientation) { if (ARG(1)) wrv(ARG(1), g_lis.front); if (ARG(2)) wrv(ARG(2), g_lis.top); RETURN_STD(3, DS_OK); }
API(l_GetPosition) { if (ARG(1)) wrv(ARG(1), g_lis.p); RETURN_STD(2, DS_OK); }
API(l_GetRolloffFactor) { if (ARG(1)) wrf(ARG(1), g_lis.rolloff); RETURN_STD(2, DS_OK); }
API(l_GetVelocity) { if (ARG(1)) wrv(ARG(1), g_lis.v); RETURN_STD(2, DS_OK); }
API(l_SetAllParameters) {
  u32 p = ARG(1);
  if (p) {
    pthread_mutex_lock(&g_mx);
    rdv(p + 4, g_lis.p); rdv(p + 16, g_lis.v);
    g_lis.dist = rdf(p + 28); g_lis.rolloff = rdf(p + 32); g_lis.doppler = rdf(p + 36);
    rdv(p + 40, g_lis.front); rdv(p + 52, g_lis.top);
    pthread_mutex_unlock(&g_mx);
  }
  RETURN_STD(3, DS_OK);
}
API(l_SetDistanceFactor) { g_lis.dist = argf(c, 1); RETURN_STD(3, DS_OK); }
API(l_SetDopplerFactor) { g_lis.doppler = argf(c, 1); RETURN_STD(3, DS_OK); }
API(l_SetOrientation) {
  pthread_mutex_lock(&g_mx);
  for (int k = 0; k < 3; k++) { g_lis.front[k] = argf(c, 1 + k); g_lis.top[k] = argf(c, 4 + k); }
  pthread_mutex_unlock(&g_mx);
  RETURN_STD(8, DS_OK);
}
API(l_SetPosition) {
  pthread_mutex_lock(&g_mx);
  for (int k = 0; k < 3; k++) g_lis.p[k] = argf(c, 1 + k);
  pthread_mutex_unlock(&g_mx);
  RETURN_STD(5, DS_OK);
}
API(l_SetRolloffFactor) { g_lis.rolloff = argf(c, 1); RETURN_STD(3, DS_OK); }
API(l_SetVelocity) { for (int k = 0; k < 3; k++) g_lis.v[k] = argf(c, 1 + k); RETURN_STD(5, DS_OK); }

static const ComMethod lis_methods[] = {
  {"IDirectSound3DListener8::QueryInterface", 3, l_QueryInterface}, {"IDirectSound3DListener8::AddRef", 1, l_AddRef},
  {"IDirectSound3DListener8::Release", 1, l_Release},
  {"IDirectSound3DListener8::GetAllParameters", 2, l_GetAllParameters},
  {"IDirectSound3DListener8::GetDistanceFactor", 2, l_GetDistanceFactor},
  {"IDirectSound3DListener8::GetDopplerFactor", 2, l_GetDopplerFactor},
  {"IDirectSound3DListener8::GetOrientation", 3, l_GetOrientation},
  {"IDirectSound3DListener8::GetPosition", 2, l_GetPosition},
  {"IDirectSound3DListener8::GetRolloffFactor", 2, l_GetRolloffFactor},
  {"IDirectSound3DListener8::GetVelocity", 2, l_GetVelocity},
  {"IDirectSound3DListener8::SetAllParameters", 3, l_SetAllParameters},
  {"IDirectSound3DListener8::SetDistanceFactor", 3, l_SetDistanceFactor},
  {"IDirectSound3DListener8::SetDopplerFactor", 3, l_SetDopplerFactor},
  {"IDirectSound3DListener8::SetOrientation", 8, l_SetOrientation},
  {"IDirectSound3DListener8::SetPosition", 5, l_SetPosition},
  {"IDirectSound3DListener8::SetRolloffFactor", 3, l_SetRolloffFactor},
  {"IDirectSound3DListener8::SetVelocity", 5, l_SetVelocity},
  {"IDirectSound3DListener8::CommitDeferredSettings", 1},
};

/* ---- IDirectSound8 ------------------------------------------------------------------------------------ */
API(ds_QueryInterface) {
  u32 iid = M32(ARG(1));
  u32 o = iid == 0 || iid == 0x279AFA83 || iid == 0xC50A7E93 ? g_ds_obj : 0; /* IUnknown, IDirectSound(8) */
  if (ARG(2)) M32(ARG(2)) = o;
  if (!o) rt_log("DirectSound: device QueryInterface for unsupported {%08x-...}", iid);
  RETURN_STD(3, o ? DS_OK : E_NOINTERFACE);
}
API(ds_AddRef) { RETURN_STD(1, 1); }
API(ds_Release) { RETURN_STD(1, 1); }
API(ds_CreateSoundBuffer) {
  u32 d = ARG(1), out = ARG(2);
  if (!d || !out) RETURN_STD(4, DSERR_INVALIDPARAM);
  u32 flags = M32(d + 4), bytes = M32(d + 8), w = M32(d + 16);
  int primary = flags & DSBCAPS_PRIMARYBUFFER;
  int ch = 2, bits = 16, rate = g_rate;
  if (!primary) {
    if (!w || !bytes) RETURN_STD(4, DSERR_INVALIDPARAM);
    u32 tag = M16(w);
    ch = M16(w + 2); rate = M32(w + 4); bits = M16(w + 14);
    if ((tag != 1 && tag != 0xFFFE) || (bits != 8 && bits != 16) || ch < 1 || ch > 2 || rate < 100) {
      rt_log("DirectSound: unsupported buffer format (tag %u, %d ch, %d Hz, %d bit)", tag, ch, rate, bits);
      RETURN_STD(4, DSERR_BADFORMAT);
    }
  }
  pthread_mutex_lock(&g_mx);
  int i = alloc_buf();
  if (i < 0) { pthread_mutex_unlock(&g_mx); rt_log("DirectSound: more than %d buffers", MAX_BUFS); RETURN_STD(4, DSERR_OUTOFMEMORY); }
  Buf *b = &g_buf[i];
  b->primary = primary;
  b->flags = flags;
  b->channels = ch; b->bits = bits; b->rate = rate; b->block = ch * bits / 8; b->freq = rate;
  if (!primary) {
    Data *dt = malloc(sizeof *dt);
    dt->bytes = bytes;
    dt->refs = 1;
    dt->guest = heap_alloc(bytes, 0);
    memset(HP(dt->guest), bits == 8 ? 0x80 : 0, bytes);
    b->data = dt;
    b->frames = bytes / b->block;
  }
  b->obj = make_obj(g_vt_buf, i);
  M32(out) = b->obj;
  pthread_mutex_unlock(&g_mx);
  if (g_trace > 1) rt_log("DirectSound: buffer %d flags %x, %u bytes, %d ch %d Hz %d bit", i, flags, bytes, ch, rate, bits);
  RETURN_STD(4, DS_OK);
}
API(ds_GetCaps) {
  u32 p = ARG(1);
  if (!p) RETURN_STD(2, DSERR_INVALIDPARAM);
  memset(HP(p + 4), 0, 92);
  M32(p + 4) = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800; /* mono/stereo 8/16-bit, continuous rate */
  M32(p + 8) = 100;
  M32(p + 12) = 200000;
  M32(p + 16) = 1;
  RETURN_STD(2, DS_OK);
}
API(ds_DuplicateSoundBuffer) {
  u32 orig = ARG(1), out = ARG(2);
  if (!orig || !out) RETURN_STD(3, DSERR_INVALIDPARAM);
  pthread_mutex_lock(&g_mx);
  u32 oi = M32(orig + 8);
  Buf *o = oi < MAX_BUFS && g_buf[oi].used ? &g_buf[oi] : NULL;
  int i = o && !o->primary ? alloc_buf() : -1;
  if (i < 0) { pthread_mutex_unlock(&g_mx); RETURN_STD(3, DSERR_INVALIDCALL); }
  Buf *b = &g_buf[i];
  b->flags = o->flags;
  b->channels = o->channels; b->bits = o->bits; b->block = o->block; b->rate = o->rate; b->freq = o->freq;
  b->frames = o->frames;
  b->volume = o->volume; b->pan = o->pan;
  b->data = o->data;
  b->data->refs++;
  b->obj = make_obj(g_vt_buf, i);
  M32(out) = b->obj;
  pthread_mutex_unlock(&g_mx);
  RETURN_STD(3, DS_OK);
}
API(ds_GetSpeakerConfig) { if (ARG(1)) M32(ARG(1)) = 4; RETURN_STD(2, DS_OK); } /* DSSPEAKER_STEREO */
API(ds_VerifyCertification) { if (ARG(1)) M32(ARG(1)) = 0; RETURN_STD(2, DS_OK); }

static const ComMethod ds_methods[] = {
  {"IDirectSound8::QueryInterface", 3, ds_QueryInterface}, {"IDirectSound8::AddRef", 1, ds_AddRef},
  {"IDirectSound8::Release", 1, ds_Release}, {"IDirectSound8::CreateSoundBuffer", 4, ds_CreateSoundBuffer},
  {"IDirectSound8::GetCaps", 2, ds_GetCaps}, {"IDirectSound8::DuplicateSoundBuffer", 3, ds_DuplicateSoundBuffer},
  {"IDirectSound8::SetCooperativeLevel", 3}, {"IDirectSound8::Compact", 1},
  {"IDirectSound8::GetSpeakerConfig", 2, ds_GetSpeakerConfig}, {"IDirectSound8::SetSpeakerConfig", 2},
  {"IDirectSound8::Initialize", 2}, {"IDirectSound8::VerifyCertification", 2, ds_VerifyCertification},
};
#define N(a) (int)(sizeof a / sizeof *a)

/* HRESULT DirectSoundCreate8(LPCGUID device, LPDIRECTSOUND8 *out, LPUNKNOWN outer)   ordinal 11 */
API(DirectSoundCreate8) {
  if (!g_ds_obj) {
    g_vt_ds = com_vtable(ds_methods, N(ds_methods));
    g_vt_buf = com_vtable(buf_methods, N(buf_methods));
    g_vt_3d = com_vtable(b3_methods, N(b3_methods));
    g_vt_lis = com_vtable(lis_methods, N(lis_methods));
    g_vt_notify = com_vtable(notify_methods, N(notify_methods));
    g_ds_obj = com_object(g_vt_ds, 0);
    g_lis_obj = com_object(g_vt_lis, 0);
    int rate = plat_audio_open(mix);
    if (rate) g_rate = rate;
    else {
      rt_log("audio: no output device; mixing silently in real time");
      pthread_t t;
      pthread_attr_t a;
      pthread_attr_init(&a);
      pthread_attr_setstacksize(&a, 256 << 10);
      pthread_create(&t, &a, pace_thread, NULL);
      pthread_attr_destroy(&a);
    }
  }
  if (ARG(1)) M32(ARG(1)) = g_ds_obj;
  RETURN_STD(3, DS_OK);
}

static const ApiEntry list[] = {{"DSOUND.dll", "#11", DirectSoundCreate8}, {0}};
void dsound_init(void) { api_register(list); }
