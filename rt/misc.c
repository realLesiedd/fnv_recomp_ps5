/* ADVAPI32, SHELL32, ole32, WINMM, WSOCK32, XInput, GOG Galaxy wrapper, d3d9 misc. */
#define _GNU_SOURCE
#include "rt.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

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
  static int init; static double t0; static struct { double t; u16 b; } ev[96]; static int n;
  if (!init) {
    init = 1;
    const char *e = getenv("FNV_PADSCRIPT");
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = ts.tv_sec + ts.tv_nsec / 1e9;
    for (; e && *e && n < 96;) {
      double t = strtod(e, (char **)&e); if (*e == ':') e++;
      /* walk / turn: hold the left stick forward / the right stick right for 3 s (no button) */
      u16 b = !strncmp(e, "down", 4) ? 0x2 : !strncmp(e, "up", 2) ? 0x1 : !strncmp(e, "walk", 4) ? 0xA001 :
              !strncmp(e, "turn", 4) ? 0xA002 : !strncmp(e, "fire", 4) ? 0xA003 : !strncmp(e, "a", 1) ? 0x1000 : !strncmp(e, "b", 1) ? 0x2000 :
              !strncmp(e, "start", 5) ? 0x10 : !strncmp(e, "right", 5) ? 0x8 : !strncmp(e, "left", 4) ? 0x4 : 0;
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
    if (ev[i].b < 0xA000 && now >= ev[i].t && now < ev[i].t + 0.25) {
      p->buttons |= ev[i].b;
      if (ev[i].b == 0x2) p->ly = -32000; /* the menus also follow the left stick */
      if (ev[i].b == 0x1) p->ly = 32000;
    }
  }
  *packet = (u32)(now * 10);
  return 1;
}
u64 g_stat_pad;
API(x_XInputGetState) {
  PlatPad p; u32 packet; u32 st = ARG(1);
  g_stat_pad++;
  { extern void console_script(void); console_script(); } /* FNV_CONSOLE test commands, at the frame's input poll */
  if (!(ARG(0) == 0 && pad_script(&p, &packet)) && !plat_pad(ARG(0), &p, &packet)) RETURN_STD(2, 1167); /* ERROR_DEVICE_NOT_CONNECTED */
  M32(st) = packet; M16(st + 4) = p.buttons; M8(st + 6) = p.lt; M8(st + 7) = p.rt;
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
