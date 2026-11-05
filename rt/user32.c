/* USER32 / GDI32 / COMCTL32: a headless window layer for now.  Windows are
 * records; messages sent at creation go to the guest window procedure.  A real
 * windowing backend (SDL) replaces this once rendering exists. */
#define _GNU_SOURCE
#include "rt.h"
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#define API(n) static void n(Ctx *c)
#define U "USER32.dll"

typedef struct { u32 wndproc; char cls[64]; u32 style, exstyle; s32 x, y, w, h; u32 hwnd; u32 longs[32]; } Wnd;
typedef struct { char name[64]; u32 wndproc, style; } WClass;
static Wnd g_wnd[64];
static int g_nwnd;
static WClass g_cls[64];
static int g_ncls;

static Wnd *wnd(u32 h) {
  for (int i = 0; i < g_nwnd; i++) if (g_wnd[i].hwnd == h) return &g_wnd[i];
  return NULL;
}

API(u_RegisterClassA) {
  u32 wc = ARG(0);
  WClass *k = &g_cls[g_ncls++];
  k->style = M32(wc); k->wndproc = M32(wc + 4);
  u32 nm = M32(wc + 36);
  if (nm >= 0x10000) snprintf(k->name, sizeof k->name, "%s", STR(nm)); else snprintf(k->name, sizeof k->name, "#%u", nm);
  rt_log("RegisterClassA(%s, wndproc=%08x)", k->name, k->wndproc);
  RETURN_STD(1, 0xC000 + g_ncls);
}

static u32 send(Ctx *c, Wnd *w, u32 msg, u32 wp, u32 lp) {
  if (!w || !w->wndproc) return 0;
  u32 args[4] = {w->hwnd, msg, wp, lp};
  return rt_call_guest(c, w->wndproc, 4, args);
}

API(u_CreateWindowExA) {
  u32 cls = ARG(1);
  char cn[64];
  if (cls >= 0x10000) snprintf(cn, sizeof cn, "%s", STR(cls)); else snprintf(cn, sizeof cn, "#%u", cls);
  Wnd *w = &g_wnd[g_nwnd++];
  memset(w, 0, sizeof *w);
  w->hwnd = 0x00010000 + g_nwnd * 4;
  snprintf(w->cls, sizeof w->cls, "%s", cn);
  w->exstyle = ARG(0); w->style = ARG(3); w->x = ARG(4); w->y = ARG(5); w->w = ARG(6); w->h = ARG(7);
  for (int i = 0; i < g_ncls; i++) if (!strcasecmp(g_cls[i].name, cn)) w->wndproc = g_cls[i].wndproc;
  rt_log("CreateWindowExA(class=%s, title=%s, %dx%d) = %08x", cn, ARG(2) ? STR(ARG(2)) : "", w->w, w->h, w->hwnd);
  /* WM_NCCREATE/WM_CREATE with a CREATESTRUCT */
  u32 cs = heap_alloc(48, 1);
  M32(cs) = ARG(11); M32(cs + 16) = ARG(3); M32(cs + 20) = ARG(6); M32(cs + 24) = ARG(7);
  M32(cs + 28) = ARG(4); M32(cs + 32) = ARG(5); M32(cs + 36) = ARG(3); M32(cs + 40) = ARG(2); M32(cs + 44) = cls;
  u32 hwnd = w->hwnd;
  u32 esp_save = c->r[4];
  send(c, w, 0x0081, 0, cs);
  send(c, w, 0x0001, 0, cs);
  c->r[4] = esp_save;
  RETURN_STD(12, hwnd);
}
API(u_DefWindowProcA) { RETURN_STD(4, ARG(1) == 0x0081 ? 1 : 0); }
API(u_ShowWindow) { RETURN_STD(2, 0); }
API(u_UpdateWindow) { RETURN_STD(1, 1); }
API(u_DestroyWindow) { RETURN_STD(1, 1); }
API(u_SetWindowPos) { RETURN_STD(7, 1); }
API(u_GetClientRect) {
  Wnd *w = wnd(ARG(0)); u32 r = ARG(1);
  M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = w ? w->w : 1280; M32(r + 12) = w ? w->h : 720;
  RETURN_STD(2, 1);
}
API(u_AdjustWindowRect) { RETURN_STD(3, 1); }
API(u_AdjustWindowRectEx) { RETURN_STD(4, 1); }
API(u_GetSystemMetrics) {
  u32 i = ARG(0);
  u32 v = i == 0 ? 1280 : i == 1 ? 720 : i == 43 ? 3 : i == 80 ? 1 : 0;
  RETURN_STD(1, v);
}
#include "platform.h"
API(u_PeekMessageA) {
  plat_pump();
  if (plat_quit_requested()) {
    static int sent;
    if (!sent) { u32 m = ARG(0); memset(HP(m), 0, 28); M32(m + 4) = 0x12; if (ARG(4) & 1) sent = 1; RETURN_STD(5, 1); } /* WM_QUIT */
  }
  /* typed text as WM_CHAR to the game window (its console reads text this way) */
  static int pending = -1;
  if (pending < 0) pending = plat_next_char();
  if (pending >= 0 && g_nwnd && (ARG(2) == 0 || (ARG(2) <= 0x102 && ARG(3) >= 0x102))) {
    u32 m = ARG(0);
    memset(HP(m), 0, 28);
    M32(m) = g_wnd[0].hwnd; M32(m + 4) = 0x102; M32(m + 8) = (u32)pending; M32(m + 12) = 1;
    if (ARG(4) & 1) pending = -1; /* PM_REMOVE */
    RETURN_STD(5, 1);
  }
  RETURN_STD(5, 0);
}
API(u_TranslateMessage) { RETURN_STD(1, 0); }
API(u_DispatchMessageA) {
  u32 m = ARG(0);
  Wnd *w = wnd(M32(m));
  u32 r = w ? send(c, w, M32(m + 4), M32(m + 8), M32(m + 12)) : 0;
  RETURN_STD(1, r);
}
API(u_SendMessageA) { Wnd *w = wnd(ARG(0)); u32 a[4] = {ARG(0), ARG(1), ARG(2), ARG(3)}; u32 r = w && w->wndproc ? rt_call_guest(c, w->wndproc, 4, a) : 0; RETURN_STD(4, r); }
API(u_LoadIconA) { RETURN_STD(2, 0x20001); }
API(u_LoadCursorA) { RETURN_STD(2, 0x20002); }
API(u_ShowCursor) { RETURN_STD(1, 0); }
API(u_GetWindow) { RETURN_STD(2, 0); }
API(u_GetDoubleClickTime) { RETURN_STD(0, 500); }
API(u_SwapMouseButton) { RETURN_STD(1, 0); }
API(u_SetWindowsHookExA) { RETURN_STD(4, 0x30001); }
API(u_UnhookWindowsHookEx) { RETURN_STD(1, 1); }
API(u_CallNextHookEx) { RETURN_STD(4, 0); }
API(u_GetWindowTextA) { if (ARG(2)) M8(ARG(1)) = 0; RETURN_STD(3, 0); }
API(u_SetWindowTextA) { RETURN_STD(2, 1); }
API(u_GetClassNameA) { Wnd *w = wnd(ARG(0)); snprintf((char *)HP(ARG(1)), ARG(2), "%s", w ? w->cls : ""); RETURN_STD(3, strlen(STR(ARG(1)))); }
API(u_SendInput) { RETURN_STD(3, ARG(0)); }
API(u_EnumChildWindows) { RETURN_STD(3, 1); }
API(u_GetWindowLongA) { Wnd *w = wnd(ARG(0)); s32 i = ARG(1); RETURN_STD(2, w ? (i == -16 ? w->style : i == -20 ? w->exstyle : i == -4 ? w->wndproc : 0) : 0); }
API(u_GetClassLongA) { RETURN_STD(2, 0); }
API(u_EnumDisplayDevicesA) {
  u32 dev = ARG(1), p = ARG(2);
  if (dev > 0) RETURN_STD(4, 0);
  u32 cb = M32(p);
  memset(HP(p + 4), 0, cb - 4);
  snprintf((char *)HP(p + 4), 32, "\\\\.\\DISPLAY1");
  snprintf((char *)HP(p + 36), 128, "Recompiled Display Adapter");
  M32(p + 164) = 5; /* attached to desktop | primary */
  RETURN_STD(4, 1);
}
API(u_MessageBoxA) {
  rt_log("MessageBox: [%s] %s", ARG(2) ? STR(ARG(2)) : "", ARG(1) ? STR(ARG(1)) : "");
  RETURN_STD(4, 1); /* IDOK */
}
API(u_GetActiveWindow) { RETURN_STD(0, g_nwnd ? g_wnd[0].hwnd : 0); }
API(u_SetForegroundWindow) { RETURN_STD(1, 1); }
API(u_FindWindowA) { RETURN_STD(2, 0); }
API(u_GetAsyncKeyState) { RETURN_STD(1, 0); }

API(g_GetStockObject) { RETURN_STD(1, 0x40000 + ARG(0)); }
API(cc_InitCommonControlsEx) { RETURN_STD(1, 1); }
API(cc_ImageList_Destroy) { RETURN_STD(1, 1); }
API(cc_ImageList_LoadImageA) { RETURN_STD(7, 0); }

static const ApiEntry list[] = {
#define E(n) {U, #n, u_##n}
  E(RegisterClassA), E(CreateWindowExA), E(DefWindowProcA), E(ShowWindow), E(UpdateWindow), E(DestroyWindow),
  E(SetWindowPos), E(GetClientRect), E(AdjustWindowRect), E(AdjustWindowRectEx), E(GetSystemMetrics),
  E(PeekMessageA), E(TranslateMessage), E(DispatchMessageA), E(SendMessageA), E(LoadIconA), E(LoadCursorA),
  E(ShowCursor), E(GetWindow), E(GetDoubleClickTime), E(SwapMouseButton), E(SetWindowsHookExA),
  E(UnhookWindowsHookEx), E(CallNextHookEx), E(GetWindowTextA), E(SetWindowTextA), E(GetClassNameA), E(SendInput),
  E(EnumChildWindows), E(GetWindowLongA), E(GetClassLongA), E(EnumDisplayDevicesA), E(MessageBoxA),
  E(GetActiveWindow), E(SetForegroundWindow), E(FindWindowA), E(GetAsyncKeyState),
#undef E
  {"GDI32.dll", "GetStockObject", g_GetStockObject},
  {"COMCTL32.dll", "InitCommonControlsEx", cc_InitCommonControlsEx},
  {"COMCTL32.dll", "ImageList_Destroy", cc_ImageList_Destroy},
  {"COMCTL32.dll", "ImageList_LoadImageA", cc_ImageList_LoadImageA},
  {0}};

void user32_init(void) { api_register(list); }
