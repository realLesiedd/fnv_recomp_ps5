/* Internal types of the D3D9 front end, shared with the GPU backend (hostd3d9.cpp). */
#pragma once
#include "com.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { K_D3D, K_DEVICE, K_SURFACE, K_TEXTURE, K_CUBE, K_VOLUME, K_VB, K_IB, K_VS, K_PS, K_DECL, K_QUERY, K_SB, K_SWAP };
#define MAXLVL 16
typedef struct Obj {
  int kind;
  u32 guest;
  u32 fmt, w, h, d, levels, usage, pool, size, fvf, type;
  u32 data[6][MAXLVL];   /* guest storage per face/level (textures), [0][0] for buffers/surfaces */
  u32 pitch[MAXLVL];
  u32 sub[6][MAXLVL];    /* child surface objects */
  u32 parent;            /* surface -> owning texture object (guest), or 0 */
  int face, level;
  u32 blob, blob_size;   /* shader bytecode / declaration */
  void *sh;              /* decoded shader (software renderer) */
  u32 *cache[6][MAXLVL]; /* decoded DXT levels (software renderer) */
  void *host;            /* GPU backend object (IDirect3D*9), created on first use */
  u32 dirty[6];          /* GPU backend: levels to upload, one bit per level and face */
  u32 dirty_lo, dirty_hi;/* GPU backend: buffer byte range to upload */
} Obj;

typedef struct {
  u32 pp[14];                 /* D3DPRESENT_PARAMETERS */
  u32 backbuffer, depth;
  u32 rt[4], ds;
  u32 rs[256];
  u32 tss[8][33];
  u32 ss[20][14];
  u32 tex[20];
  u32 stream[16], stream_off[16], stream_stride[16], stream_freq[16];
  u32 indices, vs, ps, decl, fvf;
  u32 viewport[6];
  u32 scissor[4];
  float transform[512][16];
  float vsf[256][4], psf[224][4];
  s32 vsi[16][4], psi[16][4];
  u32 vsb[16], psb[16];
  u32 clip[6][4];
  u32 material[17], light[8][26], light_on[8];
  u32 hwnd, behavior;
  int in_scene;
  u8 rs_set[256], ss_set[20][14], tss_set[8][33]; /* which states the game has set (GPU backend sync) */
} DevState;

Obj *d3d_obj(u32 guest);
DevState *d3d_dev(void);
int d3d_is_dxt(u32 fmt);
void d3d_level_layout(u32 fmt, u32 w, u32 h, u32 *pitch, u32 *size);

/* GPU backend (DXVK on Vulkan). hb_active() is 0 when the software renderer is used. */
int hb_active(void);
int hb_init(void);
void hb_create_device(void);
void hb_reset(void);
void hb_draw(u32 type, u32 base, u32 min_index, u32 num_vertices, u32 start, u32 prims, u32 idx_data, u32 idx_fmt,
             u32 up_data, u32 up_stride, int indexed);
void hb_clear(u32 count, u32 rects, u32 flags, u32 color, float z, u32 stencil);
void hb_stretch(u32 src, u32 srect, u32 dst, u32 drect, u32 filter);
void hb_colorfill(u32 surf, u32 rect, u32 color);
void hb_present(void);
void hb_readback(Obj *src, Obj *dst); /* host render target -> guest storage of dst (or of src when dst is NULL) */
void hb_release(Obj *o);

#ifdef __cplusplus
}
#endif
