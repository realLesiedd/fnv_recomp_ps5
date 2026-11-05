/* ADVAPI32, SHELL32, ole32, WINMM, WSOCK32, XInput, GOG Galaxy wrapper, d3d9 misc. */
#define _GNU_SOURCE
#include "rt.h"
#include <string.h>
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
API(s_ShellExecuteA) { RETURN_STD(6, 2); }
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
API(o_CoCreateInstance) {
  u32 g = ARG(0);
  rt_log("CoCreateInstance({%08x-...}) -> class not registered", M32(g));
  if (ARG(4)) M32(ARG(4)) = 0;
  RETURN_STD(5, 0x80040154);
}

/* WINMM */
API(w_timeGetTime) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  RETURN_STD(0, (u32)(t.tv_sec * 1000 + t.tv_nsec / 1000000));
}
API(w_mmioOpenA) { RETURN_STD(3, 0); }
API(w_mmioClose) { RETURN_STD(2, 0); }
API(w_mmioDescend) { RETURN_STD(4, 1); }
API(w_mmioAscend) { RETURN_STD(3, 1); }
API(w_mmioRead) { RETURN_STD(3, (u32)-1); }
API(w_mmioGetInfo) { RETURN_STD(3, 1); }
API(w_mmioAdvance) { RETURN_STD(3, 1); }

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
API(x_XInputGetState) { RETURN_STD(2, 1167); }
API(x_XInputSetState) { RETURN_STD(2, 1167); }

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
  {"WINMM.dll", "timeGetTime", w_timeGetTime}, {"WINMM.dll", "mmioOpenA", w_mmioOpenA},
  {"WINMM.dll", "mmioClose", w_mmioClose}, {"WINMM.dll", "mmioDescend", w_mmioDescend},
  {"WINMM.dll", "mmioAscend", w_mmioAscend}, {"WINMM.dll", "mmioRead", w_mmioRead},
  {"WINMM.dll", "mmioGetInfo", w_mmioGetInfo}, {"WINMM.dll", "mmioAdvance", w_mmioAdvance},
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
