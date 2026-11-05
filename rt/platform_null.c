/* Headless platform backend (no SDL). */
#include "platform.h"
#include <string.h>
int plat_init(void) { return 0; }
void plat_present(const uint32_t *bgra, int w, int h, int pitch) { (void)bgra; (void)w; (void)h; (void)pitch; }
void plat_pump(void) {}
int plat_quit_requested(void) { return 0; }
int plat_pad(int index, PlatPad *out, uint32_t *packet) { (void)index; (void)out; (void)packet; return 0; }
void plat_rumble(int index, uint16_t low, uint16_t high) { (void)index; (void)low; (void)high; }
void plat_keys(uint8_t keys[256]) { memset(keys, 0, 256); }
void plat_mouse(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t b[8]) { *dx = *dy = *dz = 0; memset(b, 0, 8); }
void *plat_window(void) { return 0; }
int plat_gpu_window(void) { return 0; }
