/* DirectInput8: keyboard and mouse devices fed by the platform layer. DirectSound lives in dsound.c. */
#include "com.h"
#include <string.h>

#define API(n) static void n(Ctx *c)

API(m_AddRef) { u32 o = COM_THIS; RETURN_STD(1, ++M32(o + 4)); }
API(m_Release) { u32 o = COM_THIS; u32 r = M32(o + 4) ? --M32(o + 4) : 0; RETURN_STD(1, r); }
API(m_QueryInterface) { u32 o = COM_THIS; M32(ARG(2)) = o; M32(o + 4)++; RETURN_STD(3, 0); }

#include "platform.h"
/* IDirectInputDevice8A; object extra data: [8] = 1 keyboard, 2 mouse */
API(dev_GetDeviceState) {
  u32 o = COM_THIS, n = ARG(1), p = ARG(2);
  memset(HP(p), 0, n);
  if (M32(o + 8) == 1) { u8 k[256]; plat_keys(k); memcpy(HP(p), k, n < 256 ? n : 256); }
  else if (M32(o + 8) == 2 && n >= 16) {
    int32_t dx, dy, dz; u8 b[8]; plat_mouse(&dx, &dy, &dz, b);
    M32(p) = dx; M32(p + 4) = dy; M32(p + 8) = dz;
    memcpy(HP(p + 12), b, n >= 20 ? 8 : 4);
  }
  RETURN_STD(3, 0);
}
API(dev_GetDeviceData) { if (ARG(3)) M32(ARG(3)) = 0; RETURN_STD(5, 0); }
API(dev_GetCapabilities) {
  u32 p = ARG(1); u32 n = M32(p); memset(HP(p + 4), 0, n - 4);
  M32(p + 4) = 1; M32(p + 8) = 0x12; M32(p + 12) = 3; M32(p + 16) = 3; RETURN_STD(2, 0);
}
static const ComMethod dev_methods[] = {
  {"IDirectInputDevice8::QueryInterface", 3, m_QueryInterface}, {"IDirectInputDevice8::AddRef", 1, m_AddRef},
  {"IDirectInputDevice8::Release", 1, m_Release}, {"IDirectInputDevice8::GetCapabilities", 2, dev_GetCapabilities},
  {"IDirectInputDevice8::EnumObjects", 4}, {"IDirectInputDevice8::GetProperty", 3},
  {"IDirectInputDevice8::SetProperty", 3}, {"IDirectInputDevice8::Acquire", 1}, {"IDirectInputDevice8::Unacquire", 1},
  {"IDirectInputDevice8::GetDeviceState", 3, dev_GetDeviceState},
  {"IDirectInputDevice8::GetDeviceData", 5, dev_GetDeviceData}, {"IDirectInputDevice8::SetDataFormat", 2},
  {"IDirectInputDevice8::SetEventNotification", 2}, {"IDirectInputDevice8::SetCooperativeLevel", 3},
  {"IDirectInputDevice8::GetObjectInfo", 4}, {"IDirectInputDevice8::GetDeviceInfo", 2},
  {"IDirectInputDevice8::RunControlPanel", 3}, {"IDirectInputDevice8::Initialize", 4},
  {"IDirectInputDevice8::CreateEffect", 5}, {"IDirectInputDevice8::EnumEffects", 4},
  {"IDirectInputDevice8::GetEffectInfo", 3}, {"IDirectInputDevice8::GetForceFeedbackState", 2},
  {"IDirectInputDevice8::SendForceFeedbackCommand", 2}, {"IDirectInputDevice8::EnumCreatedEffectObjects", 4},
  {"IDirectInputDevice8::Escape", 2}, {"IDirectInputDevice8::Poll", 1}, {"IDirectInputDevice8::SendDeviceData", 5},
  {"IDirectInputDevice8::EnumEffectsInFile", 5}, {"IDirectInputDevice8::WriteEffectToFile", 5},
  {"IDirectInputDevice8::BuildActionMap", 4}, {"IDirectInputDevice8::SetActionMap", 4},
  {"IDirectInputDevice8::GetImageInfo", 2},
};
static u32 g_dev_vt;

/* IDirectInput8A */
API(di_CreateDevice) {
  if (!g_dev_vt) g_dev_vt = com_vtable(dev_methods, sizeof dev_methods / sizeof *dev_methods);
  u32 o = com_object(g_dev_vt, 4);
  u32 g = M32(ARG(1));
  M32(o + 8) = g == 0x6F1D2B61 ? 1 : g == 0x6F1D2B60 ? 2 : 0; /* GUID_SysKeyboard / GUID_SysMouse */
  M32(ARG(2)) = o;
  RETURN_STD(4, 0);
}
API(di_EnumDevices) { RETURN_STD(5, 0); }
static const ComMethod di_methods[] = {
  {"IDirectInput8::QueryInterface", 3, m_QueryInterface}, {"IDirectInput8::AddRef", 1, m_AddRef},
  {"IDirectInput8::Release", 1, m_Release}, {"IDirectInput8::CreateDevice", 4, di_CreateDevice},
  {"IDirectInput8::EnumDevices", 5, di_EnumDevices}, {"IDirectInput8::GetDeviceStatus", 2},
  {"IDirectInput8::RunControlPanel", 3}, {"IDirectInput8::Initialize", 3}, {"IDirectInput8::FindDevice", 4},
  {"IDirectInput8::EnumDevicesBySemantics", 6}, {"IDirectInput8::ConfigureDevices", 5},
};
API(DirectInput8Create) {
  static u32 vt;
  if (!vt) vt = com_vtable(di_methods, sizeof di_methods / sizeof *di_methods);
  M32(ARG(3)) = com_object(vt, 0);
  rt_log("DirectInput8Create: keyboard/mouse devices report no input (headless)");
  RETURN_STD(5, 0);
}

static const ApiEntry list[] = {
  {"DINPUT8.dll", "DirectInput8Create", DirectInput8Create},
  {0}};
void dinput_init(void) { api_register(list); }
