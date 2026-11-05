/* SDL2 platform backend (Linux and PS5). */
#include "platform.h"
#include "rt.h"
#include <SDL2/SDL.h>
#include <string.h>

static SDL_Window *g_win;
static SDL_GameController *g_pad[4];
static uint32_t g_packet;
static uint8_t g_keys[256];
static int32_t g_mdx, g_mdy, g_mdz;
static uint8_t g_mbtn[8];
static int g_quit;
static pthread_mutex_t g_in_lock = PTHREAD_MUTEX_INITIALIZER;

int plat_init(void) {
  if (getenv("FNV_HEADLESS")) return 0;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
    rt_log("platform: SDL_Init failed (%s); running headless", SDL_GetError());
    return 0;
  }
  int w = 1280, h = 720;
#ifdef __PROSPERO__
  SDL_DisplayMode dm;
  if (SDL_GetCurrentDisplayMode(0, &dm) == 0) { w = dm.w; h = dm.h; }
#endif
  g_win = SDL_CreateWindow("Fallout: New Vegas (fnv_recomp)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                           SDL_WINDOW_SHOWN);
  if (!g_win) {
    rt_log("platform: SDL_CreateWindow failed (%s); running headless", SDL_GetError());
    return 0;
  }
  rt_log("platform: SDL %s video driver, window %dx%d", SDL_GetCurrentVideoDriver(), w, h);
  return 1;
}

void plat_present(const uint32_t *bgra, int w, int h, int pitch) {
  if (!g_win) return;
  SDL_Surface *dst = SDL_GetWindowSurface(g_win);
  if (!dst) return;
  SDL_Surface *src = SDL_CreateRGBSurfaceWithFormatFrom((void *)bgra, w, h, 32, pitch, SDL_PIXELFORMAT_ARGB8888);
  if (!src) return;
  SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
  /* keep the aspect ratio, letterbox the rest */
  SDL_Rect r;
  if ((long)dst->w * h > (long)dst->h * w) { r.h = dst->h; r.w = (int)((long)dst->h * w / h); }
  else { r.w = dst->w; r.h = (int)((long)dst->w * h / w); }
  r.x = (dst->w - r.w) / 2; r.y = (dst->h - r.h) / 2;
  if (r.w == w && r.h == h) SDL_BlitSurface(src, NULL, dst, &r);
  else SDL_BlitScaled(src, NULL, dst, &r);
  SDL_FreeSurface(src);
  SDL_UpdateWindowSurface(g_win);
}

/* SDL scancode -> DirectInput DIK code (the keys the game binds by default) */
static int dik(SDL_Scancode s) {
  if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
    static const uint8_t t[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                  0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    return t[s - SDL_SCANCODE_A];
  }
  if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return 0x02 + (s - SDL_SCANCODE_1);
  if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return 0x3B + (s - SDL_SCANCODE_F1);
  switch (s) {
  case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_ESCAPE: return 0x01;
  case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F; case SDL_SCANCODE_SPACE: return 0x39;
  case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D; case SDL_SCANCODE_GRAVE: return 0x29;
  case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_LCTRL: return 0x1D;
  case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_RALT: return 0xB8;
  case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_DOWN: return 0xD0; case SDL_SCANCODE_LEFT: return 0xCB;
  case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
  case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_COMMA: return 0x33;
  case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_BACKSLASH: return 0x2B;
  case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
  case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_HOME: return 0xC7;
  case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
  default: return 0;
  }
}

void plat_pump(void) {
  if (!g_win) return;
  SDL_Event e;
  pthread_mutex_lock(&g_in_lock);
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_QUIT: g_quit = 1; break;
    case SDL_KEYDOWN: case SDL_KEYUP: { int k = dik(e.key.keysym.scancode); if (k) g_keys[k] = e.type == SDL_KEYDOWN ? 0x80 : 0; break; }
    case SDL_MOUSEMOTION: g_mdx += e.motion.xrel; g_mdy += e.motion.yrel; break;
    case SDL_MOUSEWHEEL: g_mdz += e.wheel.y * 120; break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
      int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
      g_mbtn[b] = e.type == SDL_MOUSEBUTTONDOWN ? 0x80 : 0; break;
    }
    case SDL_CONTROLLERDEVICEADDED: {
      int i = e.cdevice.which;
      for (int s = 0; s < 4; s++) if (!g_pad[s]) { g_pad[s] = SDL_GameControllerOpen(i); rt_log("platform: controller %d connected (%s)", s, SDL_GameControllerName(g_pad[s])); break; }
      break;
    }
    case SDL_CONTROLLERDEVICEREMOVED:
      for (int s = 0; s < 4; s++)
        if (g_pad[s] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad[s])) == e.cdevice.which) { SDL_GameControllerClose(g_pad[s]); g_pad[s] = NULL; }
      break;
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION: g_packet++; break;
    }
  }
  pthread_mutex_unlock(&g_in_lock);
}
int plat_quit_requested(void) { return g_quit; }

int plat_pad(int index, PlatPad *o, uint32_t *packet) {
  if (index < 0 || index > 3) return 0;
  pthread_mutex_lock(&g_in_lock);
  SDL_GameController *p = g_pad[index];
  if (!p) { pthread_mutex_unlock(&g_in_lock); return 0; }
  static const struct { SDL_GameControllerButton b; uint16_t x; } map[] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x1}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x2}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x4},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x8}, {SDL_CONTROLLER_BUTTON_START, 0x10}, {SDL_CONTROLLER_BUTTON_BACK, 0x20},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x40}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x80},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x100}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x200},
    {SDL_CONTROLLER_BUTTON_A, 0x1000}, {SDL_CONTROLLER_BUTTON_B, 0x2000}, {SDL_CONTROLLER_BUTTON_X, 0x4000},
    {SDL_CONTROLLER_BUTTON_Y, 0x8000}, {SDL_CONTROLLER_BUTTON_TOUCHPAD, 0x20}, /* touchpad = Back (Pip-Boy menus) */
  };
  o->buttons = 0;
  for (unsigned i = 0; i < sizeof map / sizeof *map; i++) if (SDL_GameControllerGetButton(p, map[i].b)) o->buttons |= map[i].x;
  int lt = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  o->lt = (uint8_t)(lt * 255 / 32767); o->rt = (uint8_t)(rt * 255 / 32767);
  int ly = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY), ry = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTY);
  o->lx = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
  o->rx = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTX);
  o->ly = (int16_t)(ly == -32768 ? 32767 : -ly); /* XInput: up is positive */
  o->ry = (int16_t)(ry == -32768 ? 32767 : -ry);
  *packet = g_packet;
  pthread_mutex_unlock(&g_in_lock);
  return 1;
}
void plat_rumble(int index, uint16_t low, uint16_t high) {
  if (index >= 0 && index < 4 && g_pad[index]) SDL_GameControllerRumble(g_pad[index], low, high, 250);
}
void plat_keys(uint8_t keys[256]) { pthread_mutex_lock(&g_in_lock); memcpy(keys, g_keys, 256); pthread_mutex_unlock(&g_in_lock); }
void plat_mouse(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t b[8]) {
  pthread_mutex_lock(&g_in_lock);
  *dx = g_mdx; *dy = g_mdy; *dz = g_mdz; g_mdx = g_mdy = g_mdz = 0; memcpy(b, g_mbtn, 8);
  pthread_mutex_unlock(&g_in_lock);
}
