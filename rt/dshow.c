/* DirectShow filter graph for the game's music player (CLSID_FilterGraph).
 * The game adds its own MP3 source filter and renders that filter's output pin. We read the whole file through
 * the pin's IAsyncReader (so the game's own code does the file I/O), decode it with dr_mp3
 * (third_party/dr_mp3.h, David Reid, public domain / MIT-0) and mix it into the DirectSound output.
 * Position, duration, seeking, volume and the end-of-track event follow the decoder. If a pin cannot be read,
 * the graph falls back to a silent clock so the game's music logic still advances. */
#include "com.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../third_party/dr_mp3.h"
#pragma clang diagnostic pop

#define API(n) static void n(Ctx *c)
#define S_OK 0
#define E_NOINTERFACE 0x80004002u
#define SILENT_TRACK_SECONDS 150.0 /* only when there is no decoder */
#define MAX_GRAPHS 64
#define CHUNK 2048
#define MAX_FILE (256u << 20)

typedef struct {
  int used, refs, running, done, ended_sent, have;
  double pos, t0;                /* silent fallback: wall-clock position */
  drmp3 *mp3;                    /* heap: dr_mp3 keeps a pointer to itself, so it must never be copied */
  u8 *data;
  u64 frames, frame;             /* total, and next frame to decode (source rate) */
  int volume;                    /* IBasicAudio, hundredths of a dB */
  double frac;                   /* resampler phase */
  int16_t prev[2], cur[2];
  int16_t chunk[2 * CHUNK];
  int chunk_n, chunk_i;
} Graph;
static Graph g_graphs[MAX_GRAPHS];
static int g_next;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_vt_graph, g_vt_control, g_vt_audio, g_vt_position, g_vt_event;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static Graph *G(Ctx *c) { return &g_graphs[M32(COM_THIS + 8) % MAX_GRAPHS]; }
static double duration(Graph *g) { return g->have ? (double)g->frames / g->mp3->sampleRate : SILENT_TRACK_SECONDS; }
static double position(Graph *g) {
  if (g->have) return (double)g->frame / g->mp3->sampleRate;
  double p = g->pos + (g->running ? now_s() - g->t0 : 0);
  return p > SILENT_TRACK_SECONDS ? SILENT_TRACK_SECONDS : p;
}
static double argd(Ctx *c, int i) { u64 v = (u64)ARG(i) | ((u64)ARG(i + 1) << 32); double d; memcpy(&d, &v, 8); return d; }
static void putd(u32 p, double d) { memcpy(HP(p), &d, 8); }

static void free_graph(Graph *g) { /* g_lock held */
  if (g->have) { drmp3_uninit(g->mp3); free(g->mp3); free(g->data); }
  memset(g, 0, sizeof *g);
}
static void seek_frames(Graph *g, u64 f) { /* g_lock held */
  if (f > g->frames) f = g->frames;
  drmp3_seek_to_pcm_frame(g->mp3, f);
  g->frame = f;
  g->chunk_n = g->chunk_i = 0;
  g->frac = 1.0;
  g->prev[0] = g->prev[1] = g->cur[0] = g->cur[1] = 0;
  g->done = g->ended_sent = 0;
}

/* ---- mixing (called by the DirectSound mixer on the audio thread) ---- */
static int next_frame(Graph *g, int16_t out[2]) {
  if (g->chunk_i >= g->chunk_n) {
    int16_t tmp[2 * CHUNK];
    int ch = g->mp3->channels > 1 ? 2 : 1;
    int n = (int)drmp3_read_pcm_frames_s16(g->mp3, CHUNK, tmp);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) { g->chunk[2 * i] = tmp[i * ch]; g->chunk[2 * i + 1] = tmp[i * ch + ch - 1]; }
    g->chunk_n = n;
    g->chunk_i = 0;
  }
  out[0] = g->chunk[2 * g->chunk_i];
  out[1] = g->chunk[2 * g->chunk_i + 1];
  g->chunk_i++;
  g->frame++;
  return 1;
}
void dshow_mix(float *acc, int frames, int out_rate) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_GRAPHS; i++) {
    Graph *g = &g_graphs[i];
    if (!g->used || !g->have || !g->running || g->done) continue;
    float gain = g->volume <= -10000 ? 0.f : powf(10.f, g->volume / 2000.f);
    double step = (double)g->mp3->sampleRate / out_rate;
    for (int k = 0; k < frames && !g->done; k++) {
      while (g->frac >= 1.0) {
        g->prev[0] = g->cur[0];
        g->prev[1] = g->cur[1];
        if (!next_frame(g, g->cur)) { g->done = 1; break; }
        g->frac -= 1.0;
      }
      if (g->done) break;
      float t = (float)g->frac;
      acc[2 * k] += (g->prev[0] + (g->cur[0] - g->prev[0]) * t) * gain;
      acc[2 * k + 1] += (g->prev[1] + (g->cur[1] - g->prev[1]) * t) * gain;
      g->frac += step;
    }
  }
  pthread_mutex_unlock(&g_lock);
}

/* ---- reading the game's source filter ---- */
static u32 vcall(Ctx *c, u32 obj, int slot, int nargs, u32 *args) { return rt_call_guest(c, M32(M32(obj) + 4 * slot), nargs, args); }

/* read the whole stream behind an output pin via IAsyncReader {56A868AA-0AD4-11CE-B03A-0020AF0BA770} */
static u8 *read_pin(Ctx *c, u32 pin, size_t *len) {
  static const u8 iid[16] = {0xAA, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11, 0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
  u32 s = heap_alloc(48, 1);
  memcpy(HP(s), iid, 16);
  u32 qa[3] = {pin, s, s + 16};
  u32 hr = vcall(c, pin, 0, 3, qa); /* QueryInterface */
  u32 rd = M32(s + 16);
  u8 *data = NULL;
  *len = 0;
  if (hr || !rd) { rt_log("music: the game's source pin has no IAsyncReader (%08x); music stays silent", hr); heap_free(s); return NULL; }
  u32 la[3] = {rd, s + 24, s + 32};
  hr = vcall(c, rd, 8, 3, la); /* Length(total, available) */
  u64 total = M64(s + 24);
  if (!(hr & 0x80000000u) && total > 0 && total < MAX_FILE) {
    data = malloc(total);
    u32 buf = heap_alloc(1 << 16, 0);
    size_t got = 0;
    while (got < total) {
      u32 n = total - got < (1 << 16) ? (u32)(total - got) : (1u << 16);
      u32 ra[5] = {rd, (u32)got, (u32)((u64)got >> 32), n, buf};
      hr = vcall(c, rd, 7, 5, ra); /* SyncRead(LONGLONG pos, LONG len, BYTE *buf) */
      if (hr & 0x80000000u) break;
      memcpy(data + got, HP(buf), n);
      got += n;
      if (hr == 1) break; /* S_FALSE: end of stream */
    }
    heap_free(buf);
    *len = got;
  } else rt_log("music: IAsyncReader::Length failed (%08x, %llu bytes)", hr, (unsigned long long)total);
  u32 ra[1] = {rd};
  vcall(c, rd, 2, 1, ra); /* Release */
  heap_free(s);
  return data;
}

/* ---- IUnknown: per-object counts; the graph lives while any of its objects does ---- */
static u32 make(u32 vt, int gid) {
  u32 o = com_object(vt, 4);
  M32(o + 8) = gid;
  g_graphs[gid].refs++;
  return o;
}
API(u_AddRef) {
  pthread_mutex_lock(&g_lock);
  G(c)->refs++;
  u32 r = ++M32(COM_THIS + 4);
  pthread_mutex_unlock(&g_lock);
  RETURN_STD(1, r);
}
API(u_Release) {
  u32 o = COM_THIS, r = 0;
  pthread_mutex_lock(&g_lock);
  Graph *g = G(c);
  if (M32(o + 4)) {
    r = --M32(o + 4);
    if (g->refs && --g->refs == 0) free_graph(g);
  }
  pthread_mutex_unlock(&g_lock);
  if (!r) heap_free(o);
  RETURN_STD(1, r);
}
API(u_QueryInterface) {
  u32 iid = M32(ARG(1)), gid = M32(COM_THIS + 8) % MAX_GRAPHS, out = ARG(2), o = 0;
  pthread_mutex_lock(&g_lock);
  switch (iid) {
  case 0x56a8689f: case 0x56a868a9: case 0x36b73882: case 0: o = make(g_vt_graph, gid); break; /* IFilterGraph(2) IGraphBuilder IUnknown */
  case 0x56a868b1: o = make(g_vt_control, gid); break;
  case 0x56a868b3: o = make(g_vt_audio, gid); break;
  case 0x56a868b2: o = make(g_vt_position, gid); break;
  case 0x56a868b6: case 0x56a868c0: o = make(g_vt_event, gid); break;
  }
  pthread_mutex_unlock(&g_lock);
  M32(out) = o;
  if (!o) rt_log("DirectShow: QueryInterface for unsupported {%08x-...}", iid);
  RETURN_STD(3, o ? S_OK : E_NOINTERFACE);
}
#define UNK(N) {N "::QueryInterface", 3, u_QueryInterface}, {N "::AddRef", 1, u_AddRef}, {N "::Release", 1, u_Release}
#define DISP(N) UNK(N), {N "::GetTypeInfoCount", 2}, {N "::GetTypeInfo", 4}, {N "::GetIDsOfNames", 6}, {N "::Invoke", 9}

/* IGraphBuilder::Render(IPin *out): load and decode the track behind the game's source pin */
API(gb_Render) {
  u32 pin = ARG(1);
  size_t len = 0;
  u8 *data = pin ? read_pin(c, pin, &len) : NULL;
  drmp3 *mp3 = data ? malloc(sizeof *mp3) : NULL;
  if (data && !drmp3_init_memory(mp3, data, len, NULL)) {
    rt_log("music: %zu-byte stream is not MP3; music stays silent", len);
    free(mp3);
    free(data);
    data = NULL;
  }
  if (data) {
    u64 frames = drmp3_get_pcm_frame_count(mp3);
    drmp3_seek_to_pcm_frame(mp3, 0);
    pthread_mutex_lock(&g_lock);
    Graph *g = G(c);
    if (g->have) { drmp3_uninit(g->mp3); free(g->mp3); free(g->data); }
    g->mp3 = mp3;
    g->data = data;
    g->frames = frames;
    g->have = 1;
    seek_frames(g, 0);
    pthread_mutex_unlock(&g_lock);
    static int logged;
    if (logged < 3) {
      logged++;
      rt_log("music: %.0f s of MP3, %u Hz %u ch", (double)frames / mp3->sampleRate, mp3->sampleRate, mp3->channels);
    }
  }
  RETURN_STD(2, S_OK);
}
static const ComMethod graph_m[] = {
  UNK("IGraphBuilder"), {"IGraphBuilder::AddFilter", 3}, {"IGraphBuilder::RemoveFilter", 2},
  {"IGraphBuilder::EnumFilters", 2}, {"IGraphBuilder::FindFilterByName", 3}, {"IGraphBuilder::ConnectDirect", 4},
  {"IGraphBuilder::Reconnect", 2}, {"IGraphBuilder::Disconnect", 2}, {"IGraphBuilder::SetDefaultSyncSource", 1},
  {"IGraphBuilder::Connect", 3}, {"IGraphBuilder::Render", 2, gb_Render}, {"IGraphBuilder::RenderFile", 3},
  {"IGraphBuilder::AddSourceFilter", 4}, {"IGraphBuilder::SetLogFile", 2}, {"IGraphBuilder::Abort", 1},
  {"IGraphBuilder::ShouldOperationContinue", 1}, {"IFilterGraph2::AddSourceFilterForMoniker", 5},
  {"IFilterGraph2::ReconnectEx", 3}, {"IFilterGraph2::RenderEx", 4},
};

API(mc_Run) {
  pthread_mutex_lock(&g_lock);
  Graph *g = G(c);
  if (!g->running) { g->running = 1; g->t0 = now_s(); }
  pthread_mutex_unlock(&g_lock);
  RETURN_STD(1, S_OK);
}
static void halt(Ctx *c) {
  pthread_mutex_lock(&g_lock);
  Graph *g = G(c);
  if (!g->have) g->pos = position(g);
  g->running = 0;
  pthread_mutex_unlock(&g_lock);
}
API(mc_Pause) { halt(c); RETURN_STD(1, S_OK); }
API(mc_Stop) { halt(c); RETURN_STD(1, S_OK); }
API(mc_GetState) { M32(ARG(2)) = G(c)->running ? 2 : 0; RETURN_STD(3, S_OK); }
static const ComMethod control_m[] = {
  DISP("IMediaControl"), {"IMediaControl::Run", 1, mc_Run}, {"IMediaControl::Pause", 1, mc_Pause},
  {"IMediaControl::Stop", 1, mc_Stop}, {"IMediaControl::GetState", 3, mc_GetState},
  {"IMediaControl::RenderFile", 2}, {"IMediaControl::AddSourceFilter", 3}, {"IMediaControl::get_FilterCollection", 2},
  {"IMediaControl::get_RegFilterCollection", 2}, {"IMediaControl::StopWhenReady", 1},
};

API(ba_put_Volume) {
  int v = (int)ARG(1);
  v = v < -10000 ? -10000 : v > 0 ? 0 : v;
  Graph *g = G(c);
  if (v != g->volume) rt_log("audio: music %d -> %d", g->volume, v);
  g->volume = v;
  RETURN_STD(2, S_OK);
}
API(ba_get_Volume) { M32(ARG(1)) = (u32)G(c)->volume; RETURN_STD(2, S_OK); }
API(ba_get_Balance) { M32(ARG(1)) = 0; RETURN_STD(2, S_OK); }
static const ComMethod audio_m[] = {
  DISP("IBasicAudio"), {"IBasicAudio::put_Volume", 2, ba_put_Volume}, {"IBasicAudio::get_Volume", 2, ba_get_Volume},
  {"IBasicAudio::put_Balance", 2}, {"IBasicAudio::get_Balance", 2, ba_get_Balance},
};

API(mp_get_Duration) { putd(ARG(1), duration(G(c))); RETURN_STD(2, S_OK); }
API(mp_put_CurrentPosition) {
  double sec = argd(c, 1);
  pthread_mutex_lock(&g_lock);
  Graph *g = G(c);
  if (g->have) seek_frames(g, sec <= 0 ? 0 : (u64)(sec * g->mp3->sampleRate));
  else { g->pos = sec; g->t0 = now_s(); }
  pthread_mutex_unlock(&g_lock);
  RETURN_STD(3, S_OK);
}
API(mp_get_CurrentPosition) { putd(ARG(1), position(G(c))); RETURN_STD(2, S_OK); }
API(mp_get_StopTime) { putd(ARG(1), duration(G(c))); RETURN_STD(2, S_OK); }
API(mp_get0) { putd(ARG(1), 0); RETURN_STD(2, S_OK); }
API(mp_get_Rate) { putd(ARG(1), 1.0); RETURN_STD(2, S_OK); }
API(mp_put) { RETURN_STD(3, S_OK); }
API(mp_can) { M32(ARG(1)) = 0xffffffff; RETURN_STD(2, S_OK); }
static const ComMethod position_m[] = {
  DISP("IMediaPosition"), {"IMediaPosition::get_Duration", 2, mp_get_Duration},
  {"IMediaPosition::put_CurrentPosition", 3, mp_put_CurrentPosition},
  {"IMediaPosition::get_CurrentPosition", 2, mp_get_CurrentPosition}, {"IMediaPosition::get_StopTime", 2, mp_get_StopTime},
  {"IMediaPosition::put_StopTime", 3, mp_put}, {"IMediaPosition::get_PrerollTime", 2, mp_get0},
  {"IMediaPosition::put_PrerollTime", 3, mp_put}, {"IMediaPosition::put_Rate", 3, mp_put},
  {"IMediaPosition::get_Rate", 2, mp_get_Rate}, {"IMediaPosition::CanSeekForward", 2, mp_can},
  {"IMediaPosition::CanSeekBackward", 2, mp_can},
};

API(me_GetEventHandle) { M32(ARG(1)) = 0; RETURN_STD(2, S_OK); }
API(me_GetEvent) {
  pthread_mutex_lock(&g_lock);
  Graph *g = G(c);
  int ended = g->running && (g->have ? g->done && !g->ended_sent : position(g) >= SILENT_TRACK_SECONDS);
  if (ended) { g->running = 0; g->ended_sent = 1; }
  pthread_mutex_unlock(&g_lock);
  if (ended) { M32(ARG(1)) = 1; M32(ARG(2)) = 0; M32(ARG(3)) = 0; RETURN_STD(5, S_OK); } /* EC_COMPLETE */
  RETURN_STD(5, 0x80004004u); /* E_ABORT: no event */
}
API(me_WaitForCompletion) { M32(ARG(2)) = 0; RETURN_STD(3, 0x80040227u); }
static const ComMethod event_m[] = {
  DISP("IMediaEvent"), {"IMediaEvent::GetEventHandle", 2, me_GetEventHandle}, {"IMediaEvent::GetEvent", 5, me_GetEvent},
  {"IMediaEvent::WaitForCompletion", 3, me_WaitForCompletion}, {"IMediaEvent::CancelDefaultHandling", 2},
  {"IMediaEvent::RestoreDefaultHandling", 2}, {"IMediaEvent::FreeEventParams", 4},
  {"IMediaEventEx::SetNotifyWindow", 4}, {"IMediaEventEx::SetNotifyFlags", 2}, {"IMediaEventEx::GetNotifyFlags", 2},
};

/* called from CoCreateInstance */
u32 dshow_create(u32 clsid_first) {
  if (clsid_first != 0xe436ebb3) return 0;
#define VT(v, m) if (!v) v = com_vtable(m, sizeof m / sizeof *m)
  VT(g_vt_graph, graph_m); VT(g_vt_control, control_m); VT(g_vt_audio, audio_m); VT(g_vt_position, position_m);
  VT(g_vt_event, event_m);
#undef VT
  pthread_mutex_lock(&g_lock);
  int gid = -1;
  for (int k = 0; k < MAX_GRAPHS && gid < 0; k++) if (!g_graphs[(g_next + k) % MAX_GRAPHS].used) gid = (g_next + k) % MAX_GRAPHS;
  if (gid < 0) gid = g_next % MAX_GRAPHS; /* all in use (never released): recycle the oldest */
  g_next = gid + 1;
  free_graph(&g_graphs[gid]);
  g_graphs[gid].used = 1;
  u32 o = make(g_vt_graph, gid);
  pthread_mutex_unlock(&g_lock);
  return o;
}
