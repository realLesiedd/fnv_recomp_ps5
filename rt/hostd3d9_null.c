/* GPU backend stub for builds without DXVK: everything goes to the software renderer. */
#include "d3d9_internal.h"
int hb_wanted(void) { return 0; }
int hb_active(void) { return 0; }
int hb_init(void) { return 0; }
void hb_create_device(void) {}
void hb_reset(void) {}
void hb_draw(u32 type, u32 base, u32 min_index, u32 num_vertices, u32 start, u32 prims, u32 idx_data, u32 idx_fmt,
             u32 up_data, u32 up_stride, int indexed) {}
void hb_clear(u32 count, u32 rects, u32 flags, u32 color, float z, u32 stencil) {}
void hb_stretch(u32 src, u32 srect, u32 dst, u32 drect, u32 filter) {}
void hb_colorfill(u32 surf, u32 rect, u32 color) {}
void hb_present(void) {}
void hb_movie_overlay(void) {}
void hb_readback(Obj *src, Obj *dst) {}
void hb_release(Obj *o) { o->host = 0; }
int hb_gpu_mem(char *out, int cap) { (void)out; (void)cap; return 0; }
