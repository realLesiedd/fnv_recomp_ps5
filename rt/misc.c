/* ADVAPI32, SHELL32, ole32, WINMM, WSOCK32, XInput, GOG Galaxy wrapper, d3d9 misc. */
#define _GNU_SOURCE
#include "rt.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

#define API(n) static void n(Ctx *c)

/* registry: nothing installed; writes are accepted and dropped */
API(a_RegOpenKeyExA) { TRACE("RegOpenKeyExA(%s)", ARG(1) ? STR(ARG(1)) : ""); RETURN_STD(5, 2); }
API(a_RegCreateKeyExA) { if (ARG(7)) M32(ARG(7)) = 0x50001; if (ARG(8)) M32(ARG(8)) = 1; RETURN_STD(9, 0); }
API(a_RegQueryValueExA) { RETURN_STD(6, 2); }
API(a_RegSetValueExA) { RETURN_STD(6, 0); }
API(a_RegCloseKey) { RETURN_STD(1, 0); }
API(a_GetUserNameA) { snprintf((char *)HP(ARG(0)), M32(ARG(1)), "Player"); M32(ARG(1)) = 7; RETURN_STD(2, 1); }

/* shell */
static void mkdirs(const char *win) {
  char hp[2048]; fs_host_path(win, hp, sizeof hp);
  for (char *p = hp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(hp, 0755); *p = '/'; }
  mkdir(hp, 0755);
}
API(s_SHGetFolderPathA) {
  u32 csidl = ARG(1) & 0xff, out = ARG(4);
  const char *p;
  switch (csidl) {
  case 0x05: p = "C:\\Users\\Player\\Documents"; break;
  case 0x1a: p = "C:\\Users\\Player\\AppData\\Roaming"; break;
  case 0x1c: p = "C:\\Users\\Player\\AppData\\Local"; break;
  default: p = "C:\\Users\\Player"; break;
  }
  mkdirs(p);
  strcpy((char *)HP(out), p);
  TRACE("SHGetFolderPathA(%#x) = %s", csidl, p);
  RETURN_STD(5, 0);
}
API(s_ShellExecuteA) { rt_log("ShellExecuteA(%s, %s, %s)", ARG(1) ? STR(ARG(1)) : "", ARG(2) ? STR(ARG(2)) : "", ARG(3) ? STR(ARG(3)) : ""); RETURN_STD(6, 2); }
API(s_CommandLineToArgvW) {
  /* single argument: the program */
  u32 arr = heap_alloc(16 + 64, 1);
  const char *nm = "FalloutNV.exe";
  M32(arr) = arr + 8; M32(arr + 4) = 0;
  for (int i = 0; nm[i]; i++) M16(arr + 8 + 2 * i) = nm[i];
  if (ARG(1)) M32(ARG(1)) = 1;
  RETURN_STD(2, arr);
}

/* COM */
API(o_CoInitialize) { RETURN_STD(1, 0); }
API(o_CoInitializeEx) { RETURN_STD(2, 0); }
API(o_CoUninitialize) { RETURN_STD(0, 0); }
API(o_CoFreeUnusedLibraries) { RETURN_STD(0, 0); }
API(o_CoTaskMemAlloc) { RETURN_STD(1, heap_alloc(ARG(0), 0)); }
API(o_CoTaskMemFree) { heap_free(ARG(0)); RETURN_STD(1, 0); }
extern u32 dshow_create(u32);
API(o_CoCreateInstance) {
  u32 g = ARG(0);
  u32 obj = dshow_create(M32(g));
  if (obj) { TRACE("CoCreateInstance(FilterGraph) -> silent graph"); M32(ARG(4)) = obj; RETURN_STD(5, 0); }
  rt_log("CoCreateInstance({%08x-...}) -> class not registered", M32(g));
  if (ARG(4)) M32(ARG(4)) = 0;
  RETURN_STD(5, 0x80040154);
}

/* WINMM */
API(w_timeGetTime) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  RETURN_STD(0, (u32)(t.tv_sec * 1000 + t.tv_nsec / 1000000));
}

/* WSOCK32: networking is not available */
API(n_WSAStartup) { u32 p = ARG(1); memset(HP(p), 0, 400); M16(p) = ARG(0); M16(p + 2) = 0x0202; RETURN_STD(2, 0); }
API(n_WSAGetLastError) { RETURN_STD(0, 10050); }
API(n_socket) { RETURN_STD(3, 0xffffffff); }
API(n_closesocket) { RETURN_STD(1, 0); }
API(n_gethostname) { snprintf((char *)HP(ARG(0)), ARG(1), "localhost"); RETURN_STD(2, 0); }
API(n_gethostbyname) { RETURN_STD(1, 0); }
API(n_inet_addr) { RETURN_STD(1, 0xffffffff); }
API(n_htons) { u32 v = ARG(0) & 0xffff; RETURN_STD(1, ((v & 0xff) << 8) | (v >> 8)); }
API(n_ntohs) { u32 v = ARG(0) & 0xffff; RETURN_STD(1, ((v & 0xff) << 8) | (v >> 8)); }

/* XInput: no controller */
#include "platform.h"
/* test hook (FNV_PADSCRIPT="30:down,31:a"): scripted presses on controller 0 at the given
   seconds after start, for reproducing menu flows in automated runs */
static int pad_script(PlatPad *p, u32 *packet) {
  static int init; static double t0; static struct { double t; u16 b; int polls; } ev[96]; static int n;
  if (!init) {
    init = 1;
    const char *e = getenv("FNV_PADSCRIPT");
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = ts.tv_sec + ts.tv_nsec / 1e9;
    for (; e && *e && n < 96;) {
      double t = strtod(e, (char **)&e); if (*e == ':') e++;
      /* walk / turn: hold the left stick forward / the right stick right for 3 s (no button) */
      /* menua: for 40 s, tap cross every 1.5 s while a menu is open (clears message boxes, never acts in play) */
      u16 b = !strncmp(e, "menua", 5) ? 0xA004 : !strncmp(e, "down", 4) ? 0x2 : !strncmp(e, "up", 2) ? 0x1 : !strncmp(e, "walk", 4) ? 0xA001 :
              !strncmp(e, "turn", 4) ? 0xA002 : !strncmp(e, "fire", 4) ? 0xA003 : !strncmp(e, "a", 1) ? 0x1000 : !strncmp(e, "b", 1) ? 0x2000 :
              !strncmp(e, "start", 5) ? 0x10 : !strncmp(e, "right", 5) ? 0x8 : !strncmp(e, "left", 4) ? 0x4 :
              !strncmp(e, "r3", 2) ? 0x80 : 0;
      ev[n].t = t; ev[n].b = b; n++;
      while (*e && *e != ',') e++; if (*e == ',') e++;
    }
  }
  if (!n) return 0;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  double now = ts.tv_sec + ts.tv_nsec / 1e9 - t0;
  /* connected from the start: the game only notices a newly connected pad about 10 s later */
  memset(p, 0, sizeof *p);
  for (int i = 0; i < n; i++) {
    if (ev[i].b == 0xA001 && now >= ev[i].t && now < ev[i].t + 3) p->ly = 32767;
    if (ev[i].b == 0xA002 && now >= ev[i].t && now < ev[i].t + 3) p->rx = 32767;
    if (ev[i].b == 0xA003 && now >= ev[i].t && now < ev[i].t + 0.3) p->rt = 255;
    if (ev[i].b == 0xA004 && now >= ev[i].t && now < ev[i].t + 40 && fmod(now - ev[i].t, 1.5) < 0.3) {
      u32 im = M32(0x11D8A80); /* InterfaceManager; +0xC == 1 outside menu mode */
      if (im && M32(im + 0xC) != 1) p->buttons |= 0x1000;
    }
    if (ev[i].b < 0xA000 && now >= ev[i].t && (now < ev[i].t + 0.25 || ev[i].polls < 4)) { /* seen by 4 reads at least */
      ev[i].polls++;
      p->buttons |= ev[i].b;
      if (ev[i].b == 0x2) p->ly = -32000; /* the menus also follow the left stick */
      if (ev[i].b == 0x1) p->ly = 32000;
    }
  }
  if (getenv("FNV_PADSCRIPT_EVENTS")) { /* like a real pad: the packet number changes only when the state does */
    static PlatPad last; static u32 pk;
    if (memcmp(&last, p, sizeof last)) { pk++; last = *p; }
    *packet = pk;
  } else *packet = (u32)(now * 10);
  return 1;
}
u64 g_stat_pad;
/* The start menu (main and pause menu, with its own Yes/No prompts) drops a ✕ or ○ that comes within
   500 ms of its last action: its click handler (0x7CE9B0) and back handler (0x7CF5E0) compare
   GetTickCount with the time of the last action (0x11DAAB4). So ✕ pressed soon after backing out with ○,
   or Yes pressed soon after Continue, did nothing and had to be pressed again. The game's own pause
   stays as it is; a press that would be dropped is held back and given to the game once the menu
   takes input again. */
static int start_menu_waiting(void) {
  u32 sm = M32(0x11DAAC0), im = M32(0x11D8A80);
  if (!sm || !im || M32(im + 0xC) == 1) return 0; /* no start menu, or the game is not in menu mode */
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  u32 now = (u32)(((u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec) / 1000000); /* the game's GetTickCount */
  return now - M32(0x11DAAB4) <= 520;
}
static void menu_press_hold(PlatPad *p) {
  static u16 prev, held, sent; static double since; static int sent_polls;
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  double now = t.tv_sec + t.tv_nsec / 1e9;
  u16 down = p->buttons & ~prev;
  prev = p->buttons;
  if (held && (down & ~held)) held = 0; /* another press came first: the held one is dropped as before */
  if (!held && (down & 0x3000) && start_menu_waiting()) { held = down & 0x3000; since = now; }
  if (held && (!start_menu_waiting() || now - since > 1.0)) {
    rt_log("menu: %s held %.0f ms until the menu took input", held & 0x1000 ? "cross" : "circle", (now - since) * 1000);
    sent = held; sent_polls = 0; held = 0;
  }
  if (held) p->buttons &= ~held;
  if (sent) { p->buttons |= sent; if (++sent_polls >= 4) sent = 0; } /* down for two frames (two readers per frame) */
}
API(x_XInputGetState) {
  PlatPad p; u32 packet; u32 st = ARG(1);
  g_stat_pad++;
  { extern void console_script(void); console_script(); } /* FNV_CONSOLE test commands, at the frame's input poll */
  { extern void debug_menu_pump(void); debug_menu_pump(); } /* queued debug-menu lines (options.c); idle returns at once */
  if (!(ARG(0) == 0 && pad_script(&p, &packet)) && !plat_pad(ARG(0), &p, &packet)) RETURN_STD(2, 1167); /* ERROR_DEVICE_NOT_CONNECTED */
  if (ARG(0) == 0) { /* menu_press_hold; the packet number changes whenever the state it reports does */
    static u16 last_out; static u32 last_pk, extra;
    menu_press_hold(&p);
    if (p.buttons != last_out && packet == last_pk) extra++;
    last_out = p.buttons; last_pk = packet; packet += extra;
  }
  M32(st) = packet; M16(st + 4) = p.buttons; M8(st + 6) = p.lt; M8(st + 7) = p.rt;
  if (getenv("FNV_INPUT_LOG")) { /* who reads the pad, and what it sees when the buttons change */
    static u32 last_ret[8], last_btn[8], nret;
    u32 caller = M32(c->r[5] + 4); /* the game's wrapper 0x9F996E is a frameless thunk: its caller's return address */
    int k = 0;
    while (k < (int)nret && last_ret[k] != caller) k++;
    if (k == (int)nret && nret < 8) { last_ret[nret] = caller; last_btn[nret] = 0xFFFF; nret++; rt_log("input: new reader %08x", caller); }
    if (k < 8 && last_btn[k] != p.buttons) { rt_log("input: reader %08x sees buttons %04x (packet %u)", caller, p.buttons, packet); last_btn[k] = p.buttons; }
  }
  if (ARG(0) == 0) { extern int g_ds_rt; g_ds_rt = p.rt; } /* dualsense.c: a shot is rumble while R2 is held */
  M16(st + 8) = p.lx; M16(st + 10) = p.ly; M16(st + 12) = p.rx; M16(st + 14) = p.ry;
  RETURN_STD(2, 0);
}
API(x_XInputSetState) {
  PlatPad p; u32 packet;
  if (!(ARG(0) == 0 && getenv("FNV_PADSCRIPT")) && !plat_pad(ARG(0), &p, &packet)) RETURN_STD(2, 1167);
  plat_rumble(ARG(0), M16(ARG(1)), M16(ARG(1) + 2));
  if (ARG(0) == 0 && (M16(ARG(1)) || M16(ARG(1) + 2))) { /* dualsense.c */
    extern double g_ds_rumble_time;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    g_ds_rumble_time = ts.tv_sec + ts.tv_nsec / 1e9;
  }
  RETURN_STD(2, 0);
}

/* GOG Galaxy's Steam-API compatible wrapper (cdecl): report that the service is not running */
API(gx_SteamAPI_Init) { rt_log("SteamAPI_Init (GOG Galaxy wrapper) -> false"); RETURN_CDECL(0); }
API(gx_SteamAPI_IsSteamRunning) { RETURN_CDECL(0); }
API(gx_SteamAPI_Shutdown) { RETURN_CDECL(0); }
API(gx_SteamAPI_RunCallbacks) { RETURN_CDECL(0); }
API(gx_SteamAPI_RegisterCallback) { RETURN_CDECL(0); }
API(gx_SteamAPI_UnregisterCallback) { RETURN_CDECL(0); }
API(gx_null) { RETURN_CDECL(0); }

API(d_D3DPERF_SetOptions) { RETURN_STD(1, 0); }


static const ApiEntry list[] = {
  {"ADVAPI32.dll", "RegOpenKeyExA", a_RegOpenKeyExA}, {"ADVAPI32.dll", "RegCreateKeyExA", a_RegCreateKeyExA},
  {"ADVAPI32.dll", "RegQueryValueExA", a_RegQueryValueExA}, {"ADVAPI32.dll", "RegSetValueExA", a_RegSetValueExA},
  {"ADVAPI32.dll", "RegCloseKey", a_RegCloseKey}, {"ADVAPI32.dll", "GetUserNameA", a_GetUserNameA},
  {"SHELL32.dll", "SHGetFolderPathA", s_SHGetFolderPathA}, {"SHELL32.dll", "ShellExecuteA", s_ShellExecuteA},
  {"SHELL32.dll", "CommandLineToArgvW", s_CommandLineToArgvW},
  {"ole32.dll", "CoInitialize", o_CoInitialize}, {"ole32.dll", "CoInitializeEx", o_CoInitializeEx},
  {"ole32.dll", "CoUninitialize", o_CoUninitialize}, {"ole32.dll", "CoFreeUnusedLibraries", o_CoFreeUnusedLibraries},
  {"ole32.dll", "CoTaskMemAlloc", o_CoTaskMemAlloc}, {"ole32.dll", "CoTaskMemFree", o_CoTaskMemFree},
  {"ole32.dll", "CoCreateInstance", o_CoCreateInstance},
  {"WINMM.dll", "timeGetTime", w_timeGetTime}, /* mmio* live in mmio.c */
  {"WSOCK32.dll", "WSAStartup", n_WSAStartup}, {"WSOCK32.dll", "WSAGetLastError", n_WSAGetLastError},
  {"WSOCK32.dll", "socket", n_socket}, {"WSOCK32.dll", "closesocket", n_closesocket},
  {"WSOCK32.dll", "gethostname", n_gethostname}, {"WSOCK32.dll", "gethostbyname", n_gethostbyname},
  {"WSOCK32.dll", "inet_addr", n_inet_addr}, {"WSOCK32.dll", "htons", n_htons}, {"WSOCK32.dll", "ntohs", n_ntohs},
  {"XINPUT1_3.dll", "#2", x_XInputGetState}, {"XINPUT1_3.dll", "#3", x_XInputSetState},
  {"GalaxyWrp.dll", "SteamAPI_Init", gx_SteamAPI_Init}, {"GalaxyWrp.dll", "SteamAPI_IsSteamRunning", gx_SteamAPI_IsSteamRunning},
  {"GalaxyWrp.dll", "SteamAPI_Shutdown", gx_SteamAPI_Shutdown}, {"GalaxyWrp.dll", "SteamAPI_RunCallbacks", gx_SteamAPI_RunCallbacks},
  {"GalaxyWrp.dll", "SteamAPI_RegisterCallback", gx_SteamAPI_RegisterCallback},
  {"GalaxyWrp.dll", "SteamAPI_UnregisterCallback", gx_SteamAPI_UnregisterCallback},
  {"GalaxyWrp.dll", "SteamFriends", gx_null}, {"GalaxyWrp.dll", "SteamUser", gx_null},
  {"GalaxyWrp.dll", "SteamUserStats", gx_null}, {"GalaxyWrp.dll", "SteamApps", gx_null},
  {"GalaxyWrp.dll", "SteamUtils", gx_null},
  {"d3d9.dll", "D3DPERF_SetOptions", d_D3DPERF_SetOptions},
  {0}};

void misc_init(void) { api_register(list); }
