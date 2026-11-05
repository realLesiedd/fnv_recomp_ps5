/* Platform layer: display, input and audio.  One SDL2 backend serves the
 * Linux build and the PS5 build (ps5-payload-dev's SDL2 port: sceVideoOut,
 * DualSense via scePad, AudioOut); platform_null.c is used when SDL is absent. */
#pragma once
#include <stdint.h>

int plat_init(void);                                  /* 0 = headless */
void *plat_window(void);                              /* SDL_Window* (NULL when headless) */
int plat_gpu_window(void);                            /* window was created for Vulkan (GPU renderer) */
void plat_present(const uint32_t *bgra, int w, int h, int pitch_bytes);
void plat_pump(void);                                 /* main thread: poll events */
int plat_quit_requested(void);

/* XINPUT_GAMEPAD-shaped state of the first controller; returns 0 if none is connected */
typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } PlatPad;
int plat_pad(int index, PlatPad *out, uint32_t *packet);
void plat_rumble(int index, uint16_t low, uint16_t high);

/* DirectInput-style keyboard (DIK scancodes, 0x80 = down) and mouse deltas */
void plat_keys(uint8_t keys[256]);
void plat_mouse(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t buttons[8]);

/* Audio out: fill() runs on an audio thread and writes interleaved stereo s16 frames at the returned rate.
 * Returns the rate in Hz, or 0 when there is no audio device (the caller then paces itself). */
typedef void (*PlatAudioFill)(int16_t *out, int frames);
int plat_audio_open(PlatAudioFill fill);
