/* A silent DirectShow filter graph for the game's music player (CLSID_FilterGraph).
 * The graph accepts the game's own source filter and reports a playback position that
 * advances in real time while running, so music "plays" (without sound) and ends. */
#include "com.h"
#include <string.h>
#include <time.h>

#define API(n) static void n(Ctx *c)
#define S_OK 0
#define E_NOINTERFACE 0x80004002u
#define TRACK_SECONDS 150.0

typedef struct { int running; double pos, dur, t0; } Graph;
static Graph g_graphs[64];
static int g_ngraphs;
static u32 g_vt_graph, g_vt_control, g_vt_audio, g_vt_position, g_vt_event;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static Graph *G(Ctx *c) { return &g_graphs[M32(COM_THIS + 8) & 63]; }
static double cur(Graph *g) { double p = g->pos + (g->running ? now_s() - g->t0 : 0); return p > g->dur ? g->dur : p; }
static double argd(Ctx *c, int i) { u64 v = (u64)ARG(i) | ((u64)ARG(i + 1) << 32); double d; memcpy(&d, &v, 8); return d; }
static void putd(u32 p, double d) { memcpy(HP(p), &d, 8); }

API(u_AddRef) { RETURN_STD(1, ++M32(COM_THIS + 4)); }
API(u_Release) { u32 r = M32(COM_THIS + 4) ? --M32(COM_THIS + 4) : 0; RETURN_STD(1, r); }
static u32 make(u32 vt, int gid) { u32 o = com_object(vt, 4); M32(o + 8) = gid; return o; }
API(u_QueryInterface) {
  u32 iid = M32(ARG(1)), gid = M32(COM_THIS + 8), out = ARG(2), o = 0;
  switch (iid) {
  case 0x56a8689f: case 0x56a868a9: case 0x36b73882: case 0: o = make(g_vt_graph, gid); break; /* IFilterGraph(2) IGraphBuilder IUnknown */
  case 0x56a868b1: o = make(g_vt_control, gid); break;
  case 0x56a868b3: o = make(g_vt_audio, gid); break;
  case 0x56a868b2: o = make(g_vt_position, gid); break;
  case 0x56a868b6: case 0x56a868c0: o = make(g_vt_event, gid); break;
  }
  M32(out) = o;
  if (!o) rt_log("DirectShow: QueryInterface for unsupported {%08x-...}", iid);
  RETURN_STD(3, o ? S_OK : E_NOINTERFACE);
}
#define UNK(N) {N "::QueryInterface", 3, u_QueryInterface}, {N "::AddRef", 1, u_AddRef}, {N "::Release", 1, u_Release}
#define DISP(N) UNK(N), {N "::GetTypeInfoCount", 2}, {N "::GetTypeInfo", 4}, {N "::GetIDsOfNames", 6}, {N "::Invoke", 9}

static const ComMethod graph_m[] = {
  UNK("IGraphBuilder"), {"IGraphBuilder::AddFilter", 3}, {"IGraphBuilder::RemoveFilter", 2},
  {"IGraphBuilder::EnumFilters", 2}, {"IGraphBuilder::FindFilterByName", 3}, {"IGraphBuilder::ConnectDirect", 4},
  {"IGraphBuilder::Reconnect", 2}, {"IGraphBuilder::Disconnect", 2}, {"IGraphBuilder::SetDefaultSyncSource", 1},
  {"IGraphBuilder::Connect", 3}, {"IGraphBuilder::Render", 2}, {"IGraphBuilder::RenderFile", 3},
  {"IGraphBuilder::AddSourceFilter", 4}, {"IGraphBuilder::SetLogFile", 2}, {"IGraphBuilder::Abort", 1},
  {"IGraphBuilder::ShouldOperationContinue", 1}, {"IFilterGraph2::AddSourceFilterForMoniker", 5},
  {"IFilterGraph2::ReconnectEx", 3}, {"IFilterGraph2::RenderEx", 4},
};
API(mc_Run) { Graph *g = G(c); if (!g->running) { g->running = 1; g->t0 = now_s(); } RETURN_STD(1, S_OK); }
API(mc_Pause) { Graph *g = G(c); g->pos = cur(g); g->running = 0; RETURN_STD(1, S_OK); }
API(mc_Stop) { Graph *g = G(c); g->pos = cur(g); g->running = 0; RETURN_STD(1, S_OK); }
API(mc_GetState) { M32(ARG(2)) = G(c)->running ? 2 : 0; RETURN_STD(3, S_OK); }
static const ComMethod control_m[] = {
  DISP("IMediaControl"), {"IMediaControl::Run", 1, mc_Run}, {"IMediaControl::Pause", 1, mc_Pause},
  {"IMediaControl::Stop", 1, mc_Stop}, {"IMediaControl::GetState", 3, mc_GetState},
  {"IMediaControl::RenderFile", 2}, {"IMediaControl::AddSourceFilter", 3}, {"IMediaControl::get_FilterCollection", 2},
  {"IMediaControl::get_RegFilterCollection", 2}, {"IMediaControl::StopWhenReady", 1},
};
API(ba_get) { M32(ARG(1)) = 0; RETURN_STD(2, S_OK); }
static const ComMethod audio_m[] = {
  DISP("IBasicAudio"), {"IBasicAudio::put_Volume", 2}, {"IBasicAudio::get_Volume", 2, ba_get},
  {"IBasicAudio::put_Balance", 2}, {"IBasicAudio::get_Balance", 2, ba_get},
};
API(mp_get_Duration) { putd(ARG(1), G(c)->dur); RETURN_STD(2, S_OK); }
API(mp_put_CurrentPosition) { Graph *g = G(c); g->pos = argd(c, 1); g->t0 = now_s(); RETURN_STD(3, S_OK); }
API(mp_get_CurrentPosition) { putd(ARG(1), cur(G(c))); RETURN_STD(2, S_OK); }
API(mp_get_StopTime) { putd(ARG(1), G(c)->dur); RETURN_STD(2, S_OK); }
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
  Graph *g = G(c);
  if (g->running && cur(g) >= g->dur) { g->running = 0; M32(ARG(1)) = 1; M32(ARG(2)) = 0; M32(ARG(3)) = 0; RETURN_STD(5, S_OK); } /* EC_COMPLETE */
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
  int gid = g_ngraphs++ & 63;
  memset(&g_graphs[gid], 0, sizeof(Graph));
  g_graphs[gid].dur = TRACK_SECONDS;
  return make(g_vt_graph, gid);
}
