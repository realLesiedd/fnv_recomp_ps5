/* SDL2 platform backend (Linux and PS5). */
#include "platform.h"
#include "rt.h"
#include <SDL2/SDL.h>
#include <string.h>

static SDL_Window *g_win;
static SDL_GameController *g_pad[4];
static uint32_t g_packet;
static uint8_t g_keys[256];
static uint8_t g_keys_latch[256]; /* pressed since the game last read the keyboard: a press shorter than a frame still counts */
static int32_t g_mdx, g_mdy, g_mdz;
static uint8_t g_mbtn[8];
static int g_quit;
static int g_vulkan;
void *plat_window(void) { return g_win; }
int plat_gpu_window(void) { return g_vulkan; }
static pthread_mutex_t g_in_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef __PROSPERO__
/* PS5: the GPU renderer (DXVK) draws straight to the TV through VK_KHR_display, so SDL
   (ps5-payload-dev's port) only provides the DualSense and audio; the "window" is a token. */
static int g_token;
int plat_init(void) {
  if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) rt_log("platform: SDL_Init failed (%s)", SDL_GetError());
  g_win = (SDL_Window *)&g_token;
  g_vulkan = 1;
  for (int i = 0; i < SDL_NumJoysticks() && i < 4; i++)
    if (SDL_IsGameController(i)) { g_pad[i] = SDL_GameControllerOpen(i); rt_log("platform: controller %d connected", i); } /* no name: on PS5 it is the player's account name */
  return 1;
}
#else
int plat_init(void) {
  if (getenv("FNV_HEADLESS")) return 0;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
    rt_log("platform: SDL_Init failed (%s); running headless", SDL_GetError());
    return 0;
  }
  int w = 1280, h = 720;
  /* the GPU renderer (DXVK) presents through Vulkan; the software renderer blits to the window surface */
  extern int hb_wanted(void);
  g_vulkan = hb_wanted();
  g_win = SDL_CreateWindow("Fallout: New Vegas (fnv_recomp)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                           SDL_WINDOW_SHOWN | (g_vulkan ? SDL_WINDOW_VULKAN : 0));
  if (!g_win && g_vulkan) {
    rt_log("platform: no Vulkan window (%s); falling back to the software renderer", SDL_GetError());
    g_vulkan = 0;
    g_win = SDL_CreateWindow("Fallout: New Vegas (fnv_recomp)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_SHOWN);
  }
  if (!g_win) {
    rt_log("platform: SDL_CreateWindow failed (%s); running headless", SDL_GetError());
    return 0;
  }
  rt_log("platform: SDL %s video driver, window %dx%d", SDL_GetCurrentVideoDriver(), w, h);
  return 1;
}

#endif

void plat_present(const uint32_t *bgra, int w, int h, int pitch) {
  if (!g_win || g_vulkan) return;
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

/* typed characters for WM_CHAR, a small ring */
static int g_chars[256];
static unsigned g_char_rd, g_char_wr;
static void push_char(int ch) {
  if (g_char_wr - g_char_rd < 256) g_chars[g_char_wr++ % 256] = ch;
}
int plat_next_char(void) {
  pthread_mutex_lock(&g_in_lock);
  int ch = g_char_rd != g_char_wr ? g_chars[g_char_rd++ % 256] : -1;
  pthread_mutex_unlock(&g_in_lock);
  return ch;
}

void plat_pump(void) {
  if (!g_win) return;
  SDL_Event e;
  pthread_mutex_lock(&g_in_lock);
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_QUIT: g_quit = 1; break;
    case SDL_KEYDOWN: case SDL_KEYUP: {
      int k = dik(e.key.keysym.scancode);
      if (k) g_keys[k] = e.type == SDL_KEYDOWN ? 0x80 : 0;
      if (k && e.type == SDL_KEYDOWN) g_keys_latch[k] = 0x80;
      if (e.type == SDL_KEYDOWN) { /* keys that type a control character */
        SDL_Keycode kc = e.key.keysym.sym;
        if (kc == SDLK_RETURN || kc == SDLK_KP_ENTER) push_char('\r');
        else if (kc == SDLK_BACKSPACE) push_char('\b');
        else if (kc == SDLK_TAB) push_char('\t');
      }
      break;
    }
    case SDL_TEXTINPUT:
      for (const unsigned char *t = (const unsigned char *)e.text.text; *t; t++)
        if (*t < 0x80) push_char(*t); /* ASCII only: the game's console and name entry */
      break;
    case SDL_MOUSEMOTION: g_mdx += e.motion.xrel; g_mdy += e.motion.yrel; break;
    case SDL_MOUSEWHEEL: g_mdz += e.wheel.y * 120; break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
      int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
      g_mbtn[b] = e.type == SDL_MOUSEBUTTONDOWN ? 0x80 : 0; break;
    }
    case SDL_CONTROLLERDEVICEADDED: {
      int i = e.cdevice.which;
      for (int s = 0; s < 4; s++) if (!g_pad[s]) { g_pad[s] = SDL_GameControllerOpen(i); rt_log("platform: controller %d connected", s); break; }
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

void plat_pad_light(uint8_t r, uint8_t g, uint8_t b) {
  if (g_pad[0]) SDL_GameControllerSetLED(g_pad[0], r, g, b);
}

#ifdef __PROSPERO__
/* scePad's trigger effects and light bar reset, which SDL's PS5 driver does not wrap: looked up at run time (a
   system without them only loses the effect), on the controller SDL opened (scePadGetHandle for the user) */
int sceKernelLoadStartModule(const char *, size_t, const void *, unsigned, void *, int *);
int sceKernelDlsym(int, const char *, void **);
static int (*p_get_handle)(int32_t user, int32_t type, int32_t index), (*p_set_trigger)(int32_t, const void *),
    (*p_reset_light)(int32_t), (*p_initial_user)(int32_t *);
static int pad_handle(void) {
  static int done, failures;
  if (!done) {
    done = 1;
    int pad = sceKernelLoadStartModule("libScePad.sprx", 0, 0, 0, 0, 0);
    int us = sceKernelLoadStartModule("libSceUserService.sprx", 0, 0, 0, 0, 0);
    void *f = 0;
    if (pad >= 0 && !sceKernelDlsym(pad, "scePadGetHandle", &f)) p_get_handle = f;
    f = 0;
    if (pad >= 0 && !sceKernelDlsym(pad, "scePadSetTriggerEffect", &f)) p_set_trigger = f;
    f = 0;
    if (pad >= 0 && !sceKernelDlsym(pad, "scePadResetLightBar", &f)) p_reset_light = f;
    f = 0;
    if (us >= 0 && !sceKernelDlsym(us, "sceUserServiceGetInitialUser", &f)) p_initial_user = f;
    rt_log("dualsense: trigger effects %s", p_get_handle && p_set_trigger && p_initial_user ? "available" : "not available");
  }
  if (!p_get_handle || !p_initial_user || failures > 20) return -1;
  int32_t user = -1;
  if (p_initial_user(&user) || user < 0) { failures++; return -1; }
  int h = p_get_handle(user, 0, 0);
  if (h < 0) failures++;
  return h;
}
void plat_pad_light_reset(void) {
  int h = pad_handle();
  if (h >= 0 && p_reset_light) p_reset_light(h);
}
void plat_pad_triggers(const uint8_t param[120]) {
  static int refused;
  if (refused >= 3) return;
  int h = pad_handle();
  if (h < 0 || !p_set_trigger) return;
  int r = p_set_trigger(h, param);
  if (r && ++refused >= 3) rt_log("dualsense: adaptive triggers refused (0x%08x); off", (unsigned)r);
  else if (!r) refused = 0;
}
#else
void plat_pad_light_reset(void) {}
void plat_pad_triggers(const uint8_t param[120]) { (void)param; } /* a PC's DualSense: not done */
#endif
void plat_keys(uint8_t keys[256]) {
  pthread_mutex_lock(&g_in_lock);
  for (int i = 0; i < 256; i++) keys[i] = g_keys[i] | g_keys_latch[i];
  memset(g_keys_latch, 0, sizeof g_keys_latch);
  pthread_mutex_unlock(&g_in_lock);
}
void plat_mouse(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t b[8]) {
  pthread_mutex_lock(&g_in_lock);
  *dx = g_mdx; *dy = g_mdy; *dz = g_mdz; g_mdx = g_mdy = g_mdz = 0; memcpy(b, g_mbtn, 8);
  pthread_mutex_unlock(&g_in_lock);
}

/* ---- audio: PulseAudio/ALSA on Linux, sceAudioOut on PS5 (SDL's PS5 driver) ---- */
static PlatAudioFill g_audio_fill;
static void audio_cb(void *user, Uint8 *stream, int len) { (void)user; g_audio_fill((int16_t *)stream, len / 4); }
int plat_audio_open(PlatAudioFill fill) {
  if (getenv("FNV_NO_AUDIO")) return 0;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { rt_log("audio: SDL audio init failed (%s)", SDL_GetError()); return 0; }
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_cb;
  g_audio_fill = fill;
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!dev) { rt_log("audio: no output device (%s)", SDL_GetError()); return 0; }
  rt_log("audio: %s, %d Hz stereo, %d-frame buffer", SDL_GetCurrentAudioDriver(), have.freq, have.samples);
  SDL_PauseAudioDevice(dev, 0);
  return have.freq;
}
