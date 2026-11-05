/* Direct3D 9 front end.
 *
 * Every interface the game uses (IDirect3D9, IDirect3DDevice9 and the resource
 * interfaces) is a COM object in guest memory whose vtable points at host
 * functions here.  Resource contents live in guest memory, so Lock/LockRect hand
 * the game a plain guest pointer.  Device state is recorded so Get* calls return
 * what was set.  Drawing goes to the backend hooks at the bottom (currently a null
 * backend that counts work); a GPU backend plugs in there. */
#include "com.h"
#include <string.h>
#include <math.h>

#define API(n) static void n(Ctx *c)
#define S_OK 0
#define E_NOINTERFACE 0x80004002u
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL 0x8876086Cu

static float asf(u32 v) { float f; memcpy(&f, &v, 4); return f; }

/* ---- objects --------------------------------------------------------------- */
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
} Obj;

#define MAXOBJ (1 << 20)
static Obj *g_obj[MAXOBJ];
static int g_nobj = 1;
static pthread_mutex_t g_obj_lock = PTHREAD_MUTEX_INITIALIZER;

static u32 new_obj(u32 vt, int kind, Obj **out) {
  u32 g = com_object(vt, 4);
  Obj *o = calloc(1, sizeof *o);
  o->kind = kind; o->guest = g;
  pthread_mutex_lock(&g_obj_lock);
  int id = g_nobj++;
  if (id >= MAXOBJ) rt_fatal(NULL, "too many D3D objects");
  g_obj[id] = o;
  pthread_mutex_unlock(&g_obj_lock);
  M32(g + 8) = id;
  if (out) *out = o;
  return g;
}
static Obj *obj(u32 g) {
  if (!g) return NULL;
  u32 id = M32(g + 8);
  return id < MAXOBJ ? g_obj[id] : NULL;
}

static void free_storage(Obj *o) {
  for (int f = 0; f < 6; f++)
    for (int l = 0; l < MAXLVL; l++) {
      if (o->data[f][l] && !o->parent) heap_free(o->data[f][l]);
      o->data[f][l] = 0;
    }
  if (o->blob) heap_free(o->blob);
  o->blob = 0;
}

API(m_AddRef) { u32 g = COM_THIS; RETURN_STD(1, __atomic_add_fetch((u32 *)HP(g + 4), 1, __ATOMIC_SEQ_CST)); }
API(m_Release) {
  u32 g = COM_THIS;
  u32 r = __atomic_sub_fetch((u32 *)HP(g + 4), 1, __ATOMIC_SEQ_CST);
  if (r == 0) {
    Obj *o = obj(g);
    if (o && o->kind != K_DEVICE && o->kind != K_D3D) free_storage(o);
  }
  RETURN_STD(1, r);
}
API(m_QueryInterface) { u32 g = COM_THIS; M32(ARG(2)) = g; M32(g + 4)++; RETURN_STD(3, S_OK); }
API(m_QINone) { M32(ARG(2)) = 0; RETURN_STD(3, E_NOINTERFACE); }
static u32 g_device;
API(m_GetDevice) { M32(ARG(1)) = g_device; M32(g_device + 4)++; RETURN_STD(2, S_OK); }

/* ---- formats --------------------------------------------------------------- */
#define FOURCC(a, b, c_, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c_) << 16) | ((u32)(d) << 24))
static int is_dxt(u32 f) { return f == FOURCC('D','X','T','1') || f == FOURCC('D','X','T','2') || f == FOURCC('D','X','T','3') || f == FOURCC('D','X','T','4') || f == FOURCC('D','X','T','5') || f == FOURCC('A','T','I','1') || f == FOURCC('A','T','I','2'); }
static u32 block_bytes(u32 f) { return (f == FOURCC('D','X','T','1') || f == FOURCC('A','T','I','1')) ? 8 : 16; }
static u32 bpp(u32 f) {
  switch (f) {
  case 28: case 50: case 41: return 1;                         /* A8 L8 P8 */
  case 23: case 24: case 25: case 26: case 51: case 80: case 111: case 81: return 2;
  case 113: case 36: case 115: return 8;
  case 116: return 16;
  case 20: return 3;
  default: return 4;
  }
}
static void level_layout(u32 f, u32 w, u32 h, u32 *pitch, u32 *size) {
  if (is_dxt(f)) {
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    *pitch = bw * block_bytes(f);
    *size = *pitch * (bh ? bh : 1);
  } else {
    *pitch = (w * bpp(f) + 3) & ~3u;
    *size = *pitch * h;
  }
}
static u32 mip_count(u32 w, u32 h) { u32 n = 1; while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }

/* ---- backend (null for now) -------------------------------------------------- */
static u64 g_frames, g_draws, g_prims;
static void be_present(void) {
  g_frames++;
  if (g_frames == 1 || (g_frames % 300) == 0)
    rt_log("D3D9: frame %llu presented (%llu draw calls, %llu primitives so far; null renderer)",
           (unsigned long long)g_frames, (unsigned long long)g_draws, (unsigned long long)g_prims);
}
static void be_draw(u32 prims) { g_draws++; g_prims += prims; }

/* ---- surfaces -------------------------------------------------------------- */
static u32 g_vt_surface, g_vt_texture, g_vt_cube, g_vt_volume, g_vt_vb, g_vt_ib, g_vt_vs, g_vt_ps, g_vt_decl, g_vt_query,
    g_vt_sb, g_vt_swap;

static u32 make_surface(u32 w, u32 h, u32 fmt, u32 usage, u32 pool, u32 parent, u32 storage, u32 pitch) {
  Obj *o;
  u32 g = new_obj(g_vt_surface, K_SURFACE, &o);
  o->w = w; o->h = h; o->fmt = fmt; o->usage = usage; o->pool = pool; o->parent = parent; o->type = 1;
  if (storage) { o->data[0][0] = storage; o->pitch[0] = pitch; }
  else {
    u32 sz; level_layout(fmt, w, h, &o->pitch[0], &sz);
    o->data[0][0] = heap_alloc(sz ? sz : 4, 1);
    o->size = sz;
  }
  return g;
}

static void fill_surface_desc(u32 p, Obj *o, u32 w, u32 h) {
  /* D3DSURFACE_DESC: Format Type Usage Pool MultiSampleType MultiSampleQuality Width Height */
  M32(p) = o->fmt; M32(p + 4) = 1; M32(p + 8) = o->usage; M32(p + 12) = o->pool; M32(p + 16) = 0; M32(p + 20) = 0;
  M32(p + 24) = w; M32(p + 28) = h;
}
API(s_GetDesc) { Obj *o = obj(COM_THIS); fill_surface_desc(ARG(1), o, o->w, o->h); RETURN_STD(2, S_OK); }
API(s_LockRect) {
  Obj *o = obj(COM_THIS); u32 lr = ARG(1), rc = ARG(2);
  u32 off = 0;
  if (rc) {
    u32 x = M32(rc), y = M32(rc + 4);
    off = is_dxt(o->fmt) ? (y / 4) * o->pitch[0] + (x / 4) * block_bytes(o->fmt) : y * o->pitch[0] + x * bpp(o->fmt);
  }
  M32(lr) = o->pitch[0]; M32(lr + 4) = o->data[0][0] + off;
  RETURN_STD(4, S_OK);
}
API(s_UnlockRect) { RETURN_STD(1, S_OK); }
API(s_GetContainer) {
  Obj *o = obj(COM_THIS);
  if (!o->parent) { M32(ARG(2)) = 0; RETURN_STD(3, E_NOINTERFACE); }
  M32(ARG(2)) = o->parent; M32(o->parent + 4)++;
  RETURN_STD(3, S_OK);
}
API(s_GetDC) { M32(ARG(1)) = 0x50000; RETURN_STD(2, S_OK); }
API(r_GetType) { Obj *o = obj(COM_THIS); static const u32 t[] = {0, 0, 1, 3, 5, 4, 6, 7}; RETURN_STD(1, o->kind < 8 ? t[o->kind] : 0); }
API(r_GetPriority) { RETURN_STD(1, 0); }

#define RES_METHODS(N) \
  {N "::QueryInterface", 3, m_QueryInterface}, {N "::AddRef", 1, m_AddRef}, {N "::Release", 1, m_Release}, \
  {N "::GetDevice", 2, m_GetDevice}, {N "::SetPrivateData", 5}, {N "::GetPrivateData", 4}, {N "::FreePrivateData", 2}, \
  {N "::SetPriority", 2, r_GetPriority2}, {N "::GetPriority", 1, r_GetPriority}, {N "::PreLoad", 1}, {N "::GetType", 1, r_GetType}
API(r_GetPriority2) { RETURN_STD(2, 0); }

static const ComMethod surface_methods[] = {
  RES_METHODS("IDirect3DSurface9"),
  {"IDirect3DSurface9::GetContainer", 3, s_GetContainer}, {"IDirect3DSurface9::GetDesc", 2, s_GetDesc},
  {"IDirect3DSurface9::LockRect", 4, s_LockRect}, {"IDirect3DSurface9::UnlockRect", 1, s_UnlockRect},
  {"IDirect3DSurface9::GetDC", 2, s_GetDC}, {"IDirect3DSurface9::ReleaseDC", 2},
};

/* ---- textures -------------------------------------------------------------- */
static u32 make_texture(int kind, u32 w, u32 h, u32 d, u32 levels, u32 usage, u32 fmt, u32 pool) {
  Obj *o;
  u32 vt = kind == K_TEXTURE ? g_vt_texture : kind == K_CUBE ? g_vt_cube : g_vt_volume;
  u32 g = new_obj(vt, kind, &o);
  if (!levels) levels = (usage & 0x400) ? 1 : mip_count(w, h); /* AUTOGENMIPMAP keeps one level visible */
  if (levels > MAXLVL) levels = MAXLVL;
  o->w = w; o->h = h; o->d = d; o->levels = levels; o->usage = usage; o->fmt = fmt; o->pool = pool;
  int faces = kind == K_CUBE ? 6 : 1;
  for (int f = 0; f < faces; f++) {
    u32 lw = w, lh = h, ld = d ? d : 1;
    for (u32 l = 0; l < levels; l++) {
      u32 pitch, sz; level_layout(fmt, lw, lh, &pitch, &sz);
      o->pitch[l] = pitch;
      o->data[f][l] = heap_alloc((sz * ld) ? sz * ld : 4, 0);
      lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; ld = ld > 1 ? ld / 2 : 1;
    }
  }
  return g;
}
static u32 lvl_w(Obj *o, u32 l) { u32 w = o->w >> l; return w ? w : 1; }
static u32 lvl_h(Obj *o, u32 l) { u32 h = o->h >> l; return h ? h : 1; }
static u32 child_surface(Obj *o, int face, u32 l) {
  if (!o->sub[face][l]) {
    o->sub[face][l] = make_surface(lvl_w(o, l), lvl_h(o, l), o->fmt, o->usage, o->pool, o->guest, o->data[face][l], o->pitch[l]);
    Obj *s = obj(o->sub[face][l]); s->face = face; s->level = l;
  }
  u32 s = o->sub[face][l];
  M32(s + 4)++;
  return s;
}
static void lock_level(Obj *o, int face, u32 l, u32 lr, u32 rc) {
  u32 off = 0;
  if (rc) {
    u32 x = M32(rc), y = M32(rc + 4);
    off = is_dxt(o->fmt) ? (y / 4) * o->pitch[l] + (x / 4) * block_bytes(o->fmt) : y * o->pitch[l] + x * bpp(o->fmt);
  }
  M32(lr) = o->pitch[l]; M32(lr + 4) = o->data[face][l] + off;
}
API(t_GetLevelCount) { RETURN_STD(1, obj(COM_THIS)->levels); }
API(t_GetLOD) { RETURN_STD(1, 0); }
API(t_SetLOD) { RETURN_STD(2, 0); }
API(t_GetAutoGen) { RETURN_STD(1, 2); }
API(t_GetLevelDesc) {
  Obj *o = obj(COM_THIS); u32 l = ARG(1);
  if (l >= o->levels) RETURN_STD(3, D3DERR_INVALIDCALL);
  fill_surface_desc(ARG(2), o, lvl_w(o, l), lvl_h(o, l));
  M32(ARG(2) + 4) = 1;
  RETURN_STD(3, S_OK);
}
API(t_GetSurfaceLevel) {
  Obj *o = obj(COM_THIS); u32 l = ARG(1);
  if (l >= o->levels) RETURN_STD(3, D3DERR_INVALIDCALL);
  M32(ARG(2)) = child_surface(o, 0, l);
  RETURN_STD(3, S_OK);
}
API(t_LockRect) {
  Obj *o = obj(COM_THIS); u32 l = ARG(1);
  if (l >= o->levels) RETURN_STD(5, D3DERR_INVALIDCALL);
  lock_level(o, 0, l, ARG(2), ARG(3));
  RETURN_STD(5, S_OK);
}
API(t_UnlockRect) { RETURN_STD(2, S_OK); }
API(t_AddDirtyRect) { RETURN_STD(2, S_OK); }
API(cube_GetCubeMapSurface) {
  Obj *o = obj(COM_THIS); u32 f = ARG(1), l = ARG(2);
  if (f > 5 || l >= o->levels) RETURN_STD(4, D3DERR_INVALIDCALL);
  M32(ARG(3)) = child_surface(o, f, l);
  RETURN_STD(4, S_OK);
}
API(cube_LockRect) {
  Obj *o = obj(COM_THIS); u32 f = ARG(1), l = ARG(2);
  if (f > 5 || l >= o->levels) RETURN_STD(6, D3DERR_INVALIDCALL);
  lock_level(o, f, l, ARG(3), ARG(4));
  RETURN_STD(6, S_OK);
}
API(cube_UnlockRect) { RETURN_STD(3, S_OK); }
API(cube_AddDirtyRect) { RETURN_STD(3, S_OK); }
API(vol_GetLevelDesc) {
  Obj *o = obj(COM_THIS); u32 l = ARG(1), p = ARG(2);
  /* D3DVOLUME_DESC: Format Type Usage Pool Width Height Depth */
  M32(p) = o->fmt; M32(p + 4) = 2; M32(p + 8) = o->usage; M32(p + 12) = o->pool;
  M32(p + 16) = lvl_w(o, l); M32(p + 20) = lvl_h(o, l); M32(p + 24) = (o->d >> l) ? o->d >> l : 1;
  RETURN_STD(3, S_OK);
}
API(vol_LockBox) {
  Obj *o = obj(COM_THIS); u32 l = ARG(1), lb = ARG(2);
  u32 slice = o->pitch[l] * (is_dxt(o->fmt) ? (lvl_h(o, l) + 3) / 4 : lvl_h(o, l));
  M32(lb) = o->pitch[l]; M32(lb + 4) = slice; M32(lb + 8) = o->data[0][l];
  RETURN_STD(5, S_OK);
}
API(vol_UnlockBox) { RETURN_STD(2, S_OK); }
API(vol_GetVolumeLevel) { M32(ARG(2)) = 0; RETURN_STD(3, D3DERR_INVALIDCALL); }

#define BASETEX_METHODS(N) \
  RES_METHODS(N), {N "::SetLOD", 2, t_SetLOD}, {N "::GetLOD", 1, t_GetLOD}, {N "::GetLevelCount", 1, t_GetLevelCount}, \
  {N "::SetAutoGenFilterType", 2}, {N "::GetAutoGenFilterType", 1, t_GetAutoGen}, {N "::GenerateMipSubLevels", 1}
static const ComMethod texture_methods[] = {
  BASETEX_METHODS("IDirect3DTexture9"),
  {"IDirect3DTexture9::GetLevelDesc", 3, t_GetLevelDesc}, {"IDirect3DTexture9::GetSurfaceLevel", 3, t_GetSurfaceLevel},
  {"IDirect3DTexture9::LockRect", 5, t_LockRect}, {"IDirect3DTexture9::UnlockRect", 2, t_UnlockRect},
  {"IDirect3DTexture9::AddDirtyRect", 2, t_AddDirtyRect},
};
static const ComMethod cube_methods[] = {
  BASETEX_METHODS("IDirect3DCubeTexture9"),
  {"IDirect3DCubeTexture9::GetLevelDesc", 3, t_GetLevelDesc}, {"IDirect3DCubeTexture9::GetCubeMapSurface", 4, cube_GetCubeMapSurface},
  {"IDirect3DCubeTexture9::LockRect", 6, cube_LockRect}, {"IDirect3DCubeTexture9::UnlockRect", 3, cube_UnlockRect},
  {"IDirect3DCubeTexture9::AddDirtyRect", 3, cube_AddDirtyRect},
};
static const ComMethod volume_methods[] = {
  BASETEX_METHODS("IDirect3DVolumeTexture9"),
  {"IDirect3DVolumeTexture9::GetLevelDesc", 3, vol_GetLevelDesc}, {"IDirect3DVolumeTexture9::GetVolumeLevel", 3, vol_GetVolumeLevel},
  {"IDirect3DVolumeTexture9::LockBox", 5, vol_LockBox}, {"IDirect3DVolumeTexture9::UnlockBox", 2, vol_UnlockBox},
  {"IDirect3DVolumeTexture9::AddDirtyBox", 2},
};

/* ---- buffers --------------------------------------------------------------- */
API(b_Lock) {
  Obj *o = obj(COM_THIS); u32 off = ARG(1);
  M32(ARG(3)) = o->data[0][0] + (off < o->size ? off : 0);
  RETURN_STD(5, S_OK);
}
API(b_Unlock) { RETURN_STD(1, S_OK); }
API(b_GetDesc) {
  Obj *o = obj(COM_THIS); u32 p = ARG(1);
  /* VB: Format Type Usage Pool Size FVF ; IB: Format Type Usage Pool Size */
  M32(p) = o->kind == K_VB ? 100 : o->fmt; M32(p + 4) = o->kind == K_VB ? 6 : 7; M32(p + 8) = o->usage;
  M32(p + 12) = o->pool; M32(p + 16) = o->size;
  if (o->kind == K_VB) M32(p + 20) = o->fvf;
  RETURN_STD(2, S_OK);
}
static const ComMethod vb_methods[] = {
  RES_METHODS("IDirect3DVertexBuffer9"),
  {"IDirect3DVertexBuffer9::Lock", 5, b_Lock}, {"IDirect3DVertexBuffer9::Unlock", 1, b_Unlock},
  {"IDirect3DVertexBuffer9::GetDesc", 2, b_GetDesc},
};
static const ComMethod ib_methods[] = {
  RES_METHODS("IDirect3DIndexBuffer9"),
  {"IDirect3DIndexBuffer9::Lock", 5, b_Lock}, {"IDirect3DIndexBuffer9::Unlock", 1, b_Unlock},
  {"IDirect3DIndexBuffer9::GetDesc", 2, b_GetDesc},
};

/* ---- shaders, declarations, queries, state blocks ---------------------------- */
API(blob_Get) {
  Obj *o = obj(COM_THIS); u32 p = ARG(1), psz = ARG(2);
  if (!p) { M32(psz) = o->blob_size; RETURN_STD(3, S_OK); }
  u32 n = M32(psz) < o->blob_size ? M32(psz) : o->blob_size;
  memcpy(HP(p), HP(o->blob), n);
  RETURN_STD(3, S_OK);
}
static const ComMethod vs_methods[] = {
  {"IDirect3DVertexShader9::QueryInterface", 3, m_QueryInterface}, {"IDirect3DVertexShader9::AddRef", 1, m_AddRef},
  {"IDirect3DVertexShader9::Release", 1, m_Release}, {"IDirect3DVertexShader9::GetDevice", 2, m_GetDevice},
  {"IDirect3DVertexShader9::GetFunction", 3, blob_Get},
};
static const ComMethod ps_methods[] = {
  {"IDirect3DPixelShader9::QueryInterface", 3, m_QueryInterface}, {"IDirect3DPixelShader9::AddRef", 1, m_AddRef},
  {"IDirect3DPixelShader9::Release", 1, m_Release}, {"IDirect3DPixelShader9::GetDevice", 2, m_GetDevice},
  {"IDirect3DPixelShader9::GetFunction", 3, blob_Get},
};
API(decl_Get) {
  Obj *o = obj(COM_THIS); u32 p = ARG(1), pn = ARG(2);
  u32 n = o->blob_size / 8;
  if (p) memcpy(HP(p), HP(o->blob), o->blob_size);
  M32(pn) = n;
  RETURN_STD(3, S_OK);
}
static const ComMethod decl_methods[] = {
  {"IDirect3DVertexDeclaration9::QueryInterface", 3, m_QueryInterface}, {"IDirect3DVertexDeclaration9::AddRef", 1, m_AddRef},
  {"IDirect3DVertexDeclaration9::Release", 1, m_Release}, {"IDirect3DVertexDeclaration9::GetDevice", 2, m_GetDevice},
  {"IDirect3DVertexDeclaration9::GetDeclaration", 3, decl_Get},
};
API(q_GetType) { RETURN_STD(1, obj(COM_THIS)->type); }
API(q_GetDataSize) { u32 t = obj(COM_THIS)->type; RETURN_STD(1, (t == 10 || t == 12) ? 8 : 4); }
API(q_Issue) { RETURN_STD(2, S_OK); }
API(q_GetData) {
  Obj *o = obj(COM_THIS); u32 p = ARG(1), n = ARG(2);
  if (p && n) {
    memset(HP(p), 0, n);
    if (o->type == 8) M32(p) = 1;               /* EVENT: done */
    else if (o->type == 9) M32(p) = 100000;     /* OCCLUSION: visible */
    else if (o->type == 10) M64(p) = g_frames * 16666666ull;
    else if (o->type == 12) M64(p) = 1000000000ull;
    else M32(p) = 1;
  }
  RETURN_STD(4, S_OK);
}
static const ComMethod query_methods[] = {
  {"IDirect3DQuery9::QueryInterface", 3, m_QueryInterface}, {"IDirect3DQuery9::AddRef", 1, m_AddRef},
  {"IDirect3DQuery9::Release", 1, m_Release}, {"IDirect3DQuery9::GetDevice", 2, m_GetDevice},
  {"IDirect3DQuery9::GetType", 1, q_GetType}, {"IDirect3DQuery9::GetDataSize", 1, q_GetDataSize},
  {"IDirect3DQuery9::Issue", 2, q_Issue}, {"IDirect3DQuery9::GetData", 4, q_GetData},
};
static const ComMethod sb_methods[] = {
  {"IDirect3DStateBlock9::QueryInterface", 3, m_QueryInterface}, {"IDirect3DStateBlock9::AddRef", 1, m_AddRef},
  {"IDirect3DStateBlock9::Release", 1, m_Release}, {"IDirect3DStateBlock9::GetDevice", 2, m_GetDevice},
  {"IDirect3DStateBlock9::Capture", 1}, {"IDirect3DStateBlock9::Apply", 1},
};

/* ---- device ---------------------------------------------------------------- */
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
} DevState;
static DevState *D;

static void set_ref(u32 *slot, u32 g) {
  if (g) M32(g + 4)++;
  /* the previous binding is intentionally not released: objects are freed by the game's own Release
     calls, and keeping bound objects alive avoids use-after-free if the game drops them while bound */
  *slot = g;
}
static void get_ref(u32 out, u32 g) { M32(out) = g; if (g) M32(g + 4)++; }

API(d_QueryInterface) { M32(ARG(2)) = 0; RETURN_STD(3, E_NOINTERFACE); }
API(d_TestCooperativeLevel) { RETURN_STD(1, S_OK); }
API(d_GetAvailableTextureMem) { RETURN_STD(1, 0x40000000); }
API(d_EvictManagedResources) { RETURN_STD(1, S_OK); }
static u32 g_d3d;
API(d_GetDirect3D) { M32(ARG(1)) = g_d3d; M32(g_d3d + 4)++; RETURN_STD(2, S_OK); }
static void fill_caps(u32 p);
API(d_GetDeviceCaps) { fill_caps(ARG(1)); RETURN_STD(2, S_OK); }
API(d_GetDisplayMode) { u32 p = ARG(2); M32(p) = D->pp[0]; M32(p + 4) = D->pp[1]; M32(p + 8) = 60; M32(p + 12) = 22; RETURN_STD(3, S_OK); }
API(d_GetCreationParameters) { u32 p = ARG(1); M32(p) = 0; M32(p + 4) = 1; M32(p + 8) = D->hwnd; M32(p + 12) = D->behavior; RETURN_STD(2, S_OK); }
API(d_ShowCursor) { RETURN_STD(2, 0); }
API(d_SetCursorPosition) { RETURN_STD(4, 0); }
API(d_GetNumberOfSwapChains) { RETURN_STD(1, 1); }
static u32 g_swap;
API(d_GetSwapChain) { get_ref(ARG(2), g_swap); RETURN_STD(3, S_OK); }
static void make_backbuffers(void) {
  u32 w = D->pp[0] ? D->pp[0] : 1280, h = D->pp[1] ? D->pp[1] : 720;
  D->pp[0] = w; D->pp[1] = h;
  if (!D->pp[2]) D->pp[2] = 22;
  D->backbuffer = make_surface(w, h, D->pp[2], 1, 0, 0, 0, 0);
  D->rt[0] = D->backbuffer;
  D->depth = D->pp[9] ? make_surface(w, h, D->pp[10], 2, 0, 0, 0, 0) : 0;
  D->ds = D->depth;
  D->viewport[0] = 0; D->viewport[1] = 0; D->viewport[2] = w; D->viewport[3] = h;
  float zn = 0, zf = 1; memcpy(&D->viewport[4], &zn, 4); memcpy(&D->viewport[5], &zf, 4);
  D->scissor[2] = w; D->scissor[3] = h;
}
API(d_Reset) {
  memcpy(D->pp, HP(ARG(1)), 56);
  make_backbuffers();
  memcpy(HP(ARG(1)), D->pp, 56);
  rt_log("D3D9: Reset %ux%u", D->pp[0], D->pp[1]);
  RETURN_STD(2, S_OK);
}
API(d_Present) { be_present(); RETURN_STD(5, S_OK); }
API(d_GetBackBuffer) { get_ref(ARG(4), D->backbuffer); RETURN_STD(5, S_OK); }
API(d_GetRasterStatus) { u32 p = ARG(2); M32(p) = 0; M32(p + 4) = 0; RETURN_STD(3, S_OK); }
API(d_SetGammaRamp) { RETURN_STD(4, 0); }
API(d_GetGammaRamp) { u32 p = ARG(2); for (int i = 0; i < 256; i++) M16(p + 2 * i) = M16(p + 512 + 2 * i) = M16(p + 1024 + 2 * i) = i * 257; RETURN_STD(3, 0); }
API(d_CreateTexture) {
  u32 g = make_texture(K_TEXTURE, ARG(1), ARG(2), 1, ARG(3), ARG(4), ARG(5), ARG(6));
  M32(ARG(7)) = g;
  RETURN_STD(9, S_OK);
}
API(d_CreateVolumeTexture) {
  u32 g = make_texture(K_VOLUME, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7));
  M32(ARG(8)) = g;
  RETURN_STD(10, S_OK);
}
API(d_CreateCubeTexture) {
  u32 g = make_texture(K_CUBE, ARG(1), ARG(1), 1, ARG(2), ARG(3), ARG(4), ARG(5));
  M32(ARG(6)) = g;
  RETURN_STD(8, S_OK);
}
static u32 make_buffer(int kind, u32 len, u32 usage, u32 fmt_or_fvf, u32 pool) {
  Obj *o;
  u32 g = new_obj(kind == K_VB ? g_vt_vb : g_vt_ib, kind, &o);
  o->size = len; o->usage = usage; o->pool = pool;
  if (kind == K_VB) o->fvf = fmt_or_fvf; else o->fmt = fmt_or_fvf;
  o->data[0][0] = heap_alloc(len ? len : 4, 0);
  return g;
}
API(d_CreateVertexBuffer) { M32(ARG(5)) = make_buffer(K_VB, ARG(1), ARG(2), ARG(3), ARG(4)); RETURN_STD(7, S_OK); }
API(d_CreateIndexBuffer) { M32(ARG(5)) = make_buffer(K_IB, ARG(1), ARG(2), ARG(3), ARG(4)); RETURN_STD(7, S_OK); }
API(d_CreateRenderTarget) { M32(ARG(7)) = make_surface(ARG(1), ARG(2), ARG(3), 1, 0, 0, 0, 0); RETURN_STD(9, S_OK); }
API(d_CreateDepthStencilSurface) { M32(ARG(7)) = make_surface(ARG(1), ARG(2), ARG(3), 2, 0, 0, 0, 0); RETURN_STD(9, S_OK); }
API(d_CreateOffscreenPlainSurface) { M32(ARG(5)) = make_surface(ARG(1), ARG(2), ARG(3), 0, ARG(4), 0, 0, 0); RETURN_STD(7, S_OK); }
static void copy_surface(u32 src, u32 dst) {
  Obj *s = obj(src), *d = obj(dst);
  if (!s || !d || s->fmt != d->fmt || s->w != d->w || s->h != d->h) return;
  u32 rows = is_dxt(s->fmt) ? (s->h + 3) / 4 : s->h;
  for (u32 y = 0; y < rows; y++) memcpy(HP(d->data[0][0] + y * d->pitch[0]), HP(s->data[0][0] + y * s->pitch[0]), s->pitch[0] < d->pitch[0] ? s->pitch[0] : d->pitch[0]);
}
API(d_UpdateSurface) { copy_surface(ARG(1), ARG(3)); RETURN_STD(5, S_OK); }
API(d_UpdateTexture) {
  Obj *s = obj(ARG(1)), *d = obj(ARG(2));
  if (s && d && s->fmt == d->fmt && s->w == d->w && s->h == d->h) {
    int faces = s->kind == K_CUBE ? 6 : 1;
    for (int f = 0; f < faces; f++)
      for (u32 l = 0; l < s->levels && l < d->levels; l++) {
        u32 p, sz; level_layout(s->fmt, lvl_w(s, l), lvl_h(s, l), &p, &sz);
        memcpy(HP(d->data[f][l]), HP(s->data[f][l]), sz);
      }
  }
  RETURN_STD(3, S_OK);
}
API(d_GetRenderTargetData) { copy_surface(ARG(1), ARG(2)); RETURN_STD(3, S_OK); }
API(d_GetFrontBufferData) { RETURN_STD(3, S_OK); }
API(d_StretchRect) { RETURN_STD(6, S_OK); }
API(d_ColorFill) { RETURN_STD(4, S_OK); }
API(d_SetRenderTarget) { u32 i = ARG(1); if (i < 4) set_ref(&D->rt[i], ARG(2)); RETURN_STD(3, S_OK); }
API(d_GetRenderTarget) { u32 i = ARG(1); if (i >= 4 || !D->rt[i]) { M32(ARG(2)) = 0; RETURN_STD(3, 0x8876086Cu); } get_ref(ARG(2), D->rt[i]); RETURN_STD(3, S_OK); }
API(d_SetDepthStencilSurface) { set_ref(&D->ds, ARG(1)); RETURN_STD(2, S_OK); }
API(d_GetDepthStencilSurface) { if (!D->ds) { M32(ARG(1)) = 0; RETURN_STD(2, 0x8876086Cu); } get_ref(ARG(1), D->ds); RETURN_STD(2, S_OK); }
API(d_BeginScene) { D->in_scene = 1; RETURN_STD(1, S_OK); }
API(d_EndScene) { D->in_scene = 0; RETURN_STD(1, S_OK); }
API(d_Clear) { RETURN_STD(7, S_OK); }
API(d_SetTransform) { u32 t = ARG(1); if (t < 512) memcpy(D->transform[t], HP(ARG(2)), 64); RETURN_STD(3, S_OK); }
API(d_GetTransform) { u32 t = ARG(1); if (t < 512) memcpy(HP(ARG(2)), D->transform[t], 64); RETURN_STD(3, S_OK); }
API(d_MultiplyTransform) { RETURN_STD(3, S_OK); }
API(d_SetViewport) { memcpy(D->viewport, HP(ARG(1)), 24); RETURN_STD(2, S_OK); }
API(d_GetViewport) { memcpy(HP(ARG(1)), D->viewport, 24); RETURN_STD(2, S_OK); }
API(d_SetMaterial) { memcpy(D->material, HP(ARG(1)), 68); RETURN_STD(2, S_OK); }
API(d_GetMaterial) { memcpy(HP(ARG(1)), D->material, 68); RETURN_STD(2, S_OK); }
API(d_SetLight) { u32 i = ARG(1); if (i < 8) memcpy(D->light[i], HP(ARG(2)), 104); RETURN_STD(3, S_OK); }
API(d_GetLight) { u32 i = ARG(1); if (i < 8) memcpy(HP(ARG(2)), D->light[i], 104); RETURN_STD(3, S_OK); }
API(d_LightEnable) { u32 i = ARG(1); if (i < 8) D->light_on[i] = ARG(2); RETURN_STD(3, S_OK); }
API(d_GetLightEnable) { u32 i = ARG(1); M32(ARG(2)) = i < 8 ? D->light_on[i] : 0; RETURN_STD(3, S_OK); }
API(d_SetClipPlane) { u32 i = ARG(1); if (i < 6) memcpy(D->clip[i], HP(ARG(2)), 16); RETURN_STD(3, S_OK); }
API(d_GetClipPlane) { u32 i = ARG(1); if (i < 6) memcpy(HP(ARG(2)), D->clip[i], 16); RETURN_STD(3, S_OK); }
API(d_SetRenderState) { u32 s = ARG(1); if (s < 256) D->rs[s] = ARG(2); RETURN_STD(3, S_OK); }
API(d_GetRenderState) { u32 s = ARG(1); M32(ARG(2)) = s < 256 ? D->rs[s] : 0; RETURN_STD(3, S_OK); }
API(d_CreateStateBlock) { M32(ARG(2)) = new_obj(g_vt_sb, K_SB, NULL); RETURN_STD(3, S_OK); }
API(d_BeginStateBlock) { RETURN_STD(1, S_OK); }
API(d_EndStateBlock) { M32(ARG(1)) = new_obj(g_vt_sb, K_SB, NULL); RETURN_STD(2, S_OK); }
API(d_SetClipStatus) { RETURN_STD(2, S_OK); }
API(d_GetClipStatus) { memset(HP(ARG(1)), 0, 8); RETURN_STD(2, S_OK); }
static u32 sampler_index(u32 s) { return s >= 257 ? 16 + (s - 257) : s; } /* D3DDMAPSAMPLER, D3DVERTEXTEXTURESAMPLER0..3 */
API(d_GetTexture) { u32 s = sampler_index(ARG(1)); get_ref(ARG(2), s < 20 ? D->tex[s] : 0); RETURN_STD(3, S_OK); }
API(d_SetTexture) { u32 s = sampler_index(ARG(1)); if (s < 20) set_ref(&D->tex[s], ARG(2)); RETURN_STD(3, S_OK); }
API(d_GetTextureStageState) { u32 s = ARG(1), t = ARG(2); M32(ARG(3)) = (s < 8 && t < 33) ? D->tss[s][t] : 0; RETURN_STD(4, S_OK); }
API(d_SetTextureStageState) { u32 s = ARG(1), t = ARG(2); if (s < 8 && t < 33) D->tss[s][t] = ARG(3); RETURN_STD(4, S_OK); }
API(d_GetSamplerState) { u32 s = sampler_index(ARG(1)), t = ARG(2); M32(ARG(3)) = (s < 20 && t < 14) ? D->ss[s][t] : 0; RETURN_STD(4, S_OK); }
API(d_SetSamplerState) { u32 s = sampler_index(ARG(1)), t = ARG(2); if (s < 20 && t < 14) D->ss[s][t] = ARG(3); RETURN_STD(4, S_OK); }
API(d_ValidateDevice) { if (ARG(1)) M32(ARG(1)) = 1; RETURN_STD(2, S_OK); }
API(d_GetCurrentTexturePalette) { M32(ARG(1)) = 0; RETURN_STD(2, S_OK); }
API(d_SetScissorRect) { memcpy(D->scissor, HP(ARG(1)), 16); RETURN_STD(2, S_OK); }
API(d_GetScissorRect) { memcpy(HP(ARG(1)), D->scissor, 16); RETURN_STD(2, S_OK); }
API(d_GetSoftwareVertexProcessing) { RETURN_STD(1, 0); }
API(d_GetNPatchMode) { c->st[(c->top - 1) & 7] = 0; c->top = (c->top - 1) & 7; RETURN_STD(1, 0); } /* returns float in st0 */
static u32 prim_vertices(u32 type, u32 n) {
  switch (type) { case 1: return n; case 2: return n * 2; case 3: return n + 1; case 4: return n * 3; default: return n + 2; }
}
API(d_DrawPrimitive) { be_draw(ARG(3)); RETURN_STD(4, S_OK); }
API(d_DrawIndexedPrimitive) { be_draw(ARG(6)); RETURN_STD(7, S_OK); }
API(d_DrawPrimitiveUP) { (void)prim_vertices; be_draw(ARG(2)); D->stream[0] = 0; RETURN_STD(5, S_OK); }
API(d_DrawIndexedPrimitiveUP) { be_draw(ARG(4)); D->stream[0] = 0; D->indices = 0; RETURN_STD(9, S_OK); }
API(d_CreateVertexDeclaration) {
  u32 p = ARG(1), n = 0;
  while (M8(p + 8 * n) != 0xff && n < 64) n++;
  n++;
  Obj *o;
  u32 g = new_obj(g_vt_decl, K_DECL, &o);
  o->blob_size = n * 8; o->blob = heap_alloc(n * 8, 0);
  memcpy(HP(o->blob), HP(p), n * 8);
  M32(ARG(2)) = g;
  RETURN_STD(3, S_OK);
}
API(d_SetVertexDeclaration) { set_ref(&D->decl, ARG(1)); RETURN_STD(2, S_OK); }
API(d_GetVertexDeclaration) { get_ref(ARG(1), D->decl); RETURN_STD(2, S_OK); }
API(d_SetFVF) { D->fvf = ARG(1); RETURN_STD(2, S_OK); }
API(d_GetFVF) { M32(ARG(1)) = D->fvf; RETURN_STD(2, S_OK); }
static u32 make_shader(int kind, u32 fn) {
  u32 n = 1;
  while (M32(fn + 4 * n) != 0x0000ffff && n < (1 << 20)) {
    u32 tok = M32(fn + 4 * n);
    if ((tok & 0xffff) == 0xfffe) n += 1 + (tok >> 16); /* comment */
    else n++;
  }
  n++;
  Obj *o;
  u32 g = new_obj(kind == K_VS ? g_vt_vs : g_vt_ps, kind, &o);
  o->blob_size = n * 4; o->blob = heap_alloc(n * 4, 0);
  memcpy(HP(o->blob), HP(fn), n * 4);
  return g;
}
API(d_CreateVertexShader) { M32(ARG(2)) = make_shader(K_VS, ARG(1)); RETURN_STD(3, S_OK); }
API(d_SetVertexShader) { set_ref(&D->vs, ARG(1)); RETURN_STD(2, S_OK); }
API(d_GetVertexShader) { get_ref(ARG(1), D->vs); RETURN_STD(2, S_OK); }
API(d_CreatePixelShader) { M32(ARG(2)) = make_shader(K_PS, ARG(1)); RETURN_STD(3, S_OK); }
API(d_SetPixelShader) { set_ref(&D->ps, ARG(1)); RETURN_STD(2, S_OK); }
API(d_GetPixelShader) { get_ref(ARG(1), D->ps); RETURN_STD(2, S_OK); }
#define CONSTS(name, arr, max, esz)                                                                                     \
  API(d_Set##name) { u32 r = ARG(1), n = ARG(3); if (r + n <= max) memcpy(&D->arr[r], HP(ARG(2)), n * esz); RETURN_STD(4, S_OK); } \
  API(d_Get##name) { u32 r = ARG(1), n = ARG(3); if (r + n <= max) memcpy(HP(ARG(2)), &D->arr[r], n * esz); RETURN_STD(4, S_OK); }
CONSTS(VertexShaderConstantF, vsf, 256, 16)
CONSTS(PixelShaderConstantF, psf, 224, 16)
CONSTS(VertexShaderConstantI, vsi, 16, 16)
CONSTS(PixelShaderConstantI, psi, 16, 16)
CONSTS(VertexShaderConstantB, vsb, 16, 4)
CONSTS(PixelShaderConstantB, psb, 16, 4)
API(d_SetStreamSource) {
  u32 i = ARG(1);
  if (i < 16) { set_ref(&D->stream[i], ARG(2)); D->stream_off[i] = ARG(3); D->stream_stride[i] = ARG(4); }
  RETURN_STD(5, S_OK);
}
API(d_GetStreamSource) {
  u32 i = ARG(1);
  get_ref(ARG(2), i < 16 ? D->stream[i] : 0);
  M32(ARG(3)) = i < 16 ? D->stream_off[i] : 0; M32(ARG(4)) = i < 16 ? D->stream_stride[i] : 0;
  RETURN_STD(5, S_OK);
}
API(d_SetStreamSourceFreq) { u32 i = ARG(1); if (i < 16) D->stream_freq[i] = ARG(2); RETURN_STD(3, S_OK); }
API(d_GetStreamSourceFreq) { u32 i = ARG(1); M32(ARG(2)) = i < 16 ? D->stream_freq[i] : 1; RETURN_STD(3, S_OK); }
API(d_SetIndices) { set_ref(&D->indices, ARG(1)); RETURN_STD(2, S_OK); }
API(d_GetIndices) { get_ref(ARG(1), D->indices); RETURN_STD(2, S_OK); }
API(d_CreateQuery) {
  u32 t = ARG(1);
  if (!ARG(2)) RETURN_STD(3, (t >= 8 && t <= 12) ? S_OK : D3DERR_NOTAVAILABLE); /* support check */
  if (t < 8 || t > 12) { M32(ARG(2)) = 0; RETURN_STD(3, D3DERR_NOTAVAILABLE); }
  Obj *o; u32 g = new_obj(g_vt_query, K_QUERY, &o); o->type = t;
  M32(ARG(2)) = g;
  RETURN_STD(3, S_OK);
}

#define DM(name, n, fn) {"IDirect3DDevice9::" name, n, fn}
static const ComMethod device_methods[] = {
  DM("QueryInterface", 3, d_QueryInterface), DM("AddRef", 1, m_AddRef), DM("Release", 1, m_Release),
  DM("TestCooperativeLevel", 1, d_TestCooperativeLevel), DM("GetAvailableTextureMem", 1, d_GetAvailableTextureMem),
  DM("EvictManagedResources", 1, d_EvictManagedResources), DM("GetDirect3D", 2, d_GetDirect3D),
  DM("GetDeviceCaps", 2, d_GetDeviceCaps), DM("GetDisplayMode", 3, d_GetDisplayMode),
  DM("GetCreationParameters", 2, d_GetCreationParameters), DM("SetCursorProperties", 4, NULL),
  DM("SetCursorPosition", 4, d_SetCursorPosition), DM("ShowCursor", 2, d_ShowCursor),
  DM("CreateAdditionalSwapChain", 3, NULL), DM("GetSwapChain", 3, d_GetSwapChain),
  DM("GetNumberOfSwapChains", 1, d_GetNumberOfSwapChains), DM("Reset", 2, d_Reset), DM("Present", 5, d_Present),
  DM("GetBackBuffer", 5, d_GetBackBuffer), DM("GetRasterStatus", 3, d_GetRasterStatus), DM("SetDialogBoxMode", 2, NULL),
  DM("SetGammaRamp", 4, d_SetGammaRamp), DM("GetGammaRamp", 3, d_GetGammaRamp), DM("CreateTexture", 9, d_CreateTexture),
  DM("CreateVolumeTexture", 10, d_CreateVolumeTexture), DM("CreateCubeTexture", 8, d_CreateCubeTexture),
  DM("CreateVertexBuffer", 7, d_CreateVertexBuffer), DM("CreateIndexBuffer", 7, d_CreateIndexBuffer),
  DM("CreateRenderTarget", 9, d_CreateRenderTarget), DM("CreateDepthStencilSurface", 9, d_CreateDepthStencilSurface),
  DM("UpdateSurface", 5, d_UpdateSurface), DM("UpdateTexture", 3, d_UpdateTexture),
  DM("GetRenderTargetData", 3, d_GetRenderTargetData), DM("GetFrontBufferData", 3, d_GetFrontBufferData),
  DM("StretchRect", 6, d_StretchRect), DM("ColorFill", 4, d_ColorFill),
  DM("CreateOffscreenPlainSurface", 7, d_CreateOffscreenPlainSurface), DM("SetRenderTarget", 3, d_SetRenderTarget),
  DM("GetRenderTarget", 3, d_GetRenderTarget), DM("SetDepthStencilSurface", 2, d_SetDepthStencilSurface),
  DM("GetDepthStencilSurface", 2, d_GetDepthStencilSurface), DM("BeginScene", 1, d_BeginScene),
  DM("EndScene", 1, d_EndScene), DM("Clear", 7, d_Clear), DM("SetTransform", 3, d_SetTransform),
  DM("GetTransform", 3, d_GetTransform), DM("MultiplyTransform", 3, d_MultiplyTransform),
  DM("SetViewport", 2, d_SetViewport), DM("GetViewport", 2, d_GetViewport), DM("SetMaterial", 2, d_SetMaterial),
  DM("GetMaterial", 2, d_GetMaterial), DM("SetLight", 3, d_SetLight), DM("GetLight", 3, d_GetLight),
  DM("LightEnable", 3, d_LightEnable), DM("GetLightEnable", 3, d_GetLightEnable), DM("SetClipPlane", 3, d_SetClipPlane),
  DM("GetClipPlane", 3, d_GetClipPlane), DM("SetRenderState", 3, d_SetRenderState),
  DM("GetRenderState", 3, d_GetRenderState), DM("CreateStateBlock", 3, d_CreateStateBlock),
  DM("BeginStateBlock", 1, d_BeginStateBlock), DM("EndStateBlock", 2, d_EndStateBlock),
  DM("SetClipStatus", 2, d_SetClipStatus), DM("GetClipStatus", 2, d_GetClipStatus), DM("GetTexture", 3, d_GetTexture),
  DM("SetTexture", 3, d_SetTexture), DM("GetTextureStageState", 4, d_GetTextureStageState),
  DM("SetTextureStageState", 4, d_SetTextureStageState), DM("GetSamplerState", 4, d_GetSamplerState),
  DM("SetSamplerState", 4, d_SetSamplerState), DM("ValidateDevice", 2, d_ValidateDevice),
  DM("SetPaletteEntries", 3, NULL), DM("GetPaletteEntries", 3, NULL), DM("SetCurrentTexturePalette", 2, NULL),
  DM("GetCurrentTexturePalette", 2, d_GetCurrentTexturePalette), DM("SetScissorRect", 2, d_SetScissorRect),
  DM("GetScissorRect", 2, d_GetScissorRect), DM("SetSoftwareVertexProcessing", 2, NULL),
  DM("GetSoftwareVertexProcessing", 1, d_GetSoftwareVertexProcessing), DM("SetNPatchMode", 2, NULL),
  DM("GetNPatchMode", 1, d_GetNPatchMode), DM("DrawPrimitive", 4, d_DrawPrimitive),
  DM("DrawIndexedPrimitive", 7, d_DrawIndexedPrimitive), DM("DrawPrimitiveUP", 5, d_DrawPrimitiveUP),
  DM("DrawIndexedPrimitiveUP", 9, d_DrawIndexedPrimitiveUP), DM("ProcessVertices", 7, NULL),
  DM("CreateVertexDeclaration", 3, d_CreateVertexDeclaration), DM("SetVertexDeclaration", 2, d_SetVertexDeclaration),
  DM("GetVertexDeclaration", 2, d_GetVertexDeclaration), DM("SetFVF", 2, d_SetFVF), DM("GetFVF", 2, d_GetFVF),
  DM("CreateVertexShader", 3, d_CreateVertexShader), DM("SetVertexShader", 2, d_SetVertexShader),
  DM("GetVertexShader", 2, d_GetVertexShader), DM("SetVertexShaderConstantF", 4, d_SetVertexShaderConstantF),
  DM("GetVertexShaderConstantF", 4, d_GetVertexShaderConstantF), DM("SetVertexShaderConstantI", 4, d_SetVertexShaderConstantI),
  DM("GetVertexShaderConstantI", 4, d_GetVertexShaderConstantI), DM("SetVertexShaderConstantB", 4, d_SetVertexShaderConstantB),
  DM("GetVertexShaderConstantB", 4, d_GetVertexShaderConstantB), DM("SetStreamSource", 5, d_SetStreamSource),
  DM("GetStreamSource", 5, d_GetStreamSource), DM("SetStreamSourceFreq", 3, d_SetStreamSourceFreq),
  DM("GetStreamSourceFreq", 3, d_GetStreamSourceFreq), DM("SetIndices", 2, d_SetIndices), DM("GetIndices", 2, d_GetIndices),
  DM("CreatePixelShader", 3, d_CreatePixelShader), DM("SetPixelShader", 2, d_SetPixelShader),
  DM("GetPixelShader", 2, d_GetPixelShader), DM("SetPixelShaderConstantF", 4, d_SetPixelShaderConstantF),
  DM("GetPixelShaderConstantF", 4, d_GetPixelShaderConstantF), DM("SetPixelShaderConstantI", 4, d_SetPixelShaderConstantI),
  DM("GetPixelShaderConstantI", 4, d_GetPixelShaderConstantI), DM("SetPixelShaderConstantB", 4, d_SetPixelShaderConstantB),
  DM("GetPixelShaderConstantB", 4, d_GetPixelShaderConstantB), DM("DrawRectPatch", 4, NULL), DM("DrawTriPatch", 4, NULL),
  DM("DeletePatch", 2, NULL), DM("CreateQuery", 3, d_CreateQuery),
};

/* IDirect3DSwapChain9 */
API(sc_Present) { be_present(); RETURN_STD(6, S_OK); }
API(sc_GetBackBuffer) { get_ref(ARG(3), D->backbuffer); RETURN_STD(4, S_OK); }
API(sc_GetPresentParameters) { memcpy(HP(ARG(1)), D->pp, 56); RETURN_STD(2, S_OK); }
API(sc_GetDisplayMode) { u32 p = ARG(1); M32(p) = D->pp[0]; M32(p + 4) = D->pp[1]; M32(p + 8) = 60; M32(p + 12) = 22; RETURN_STD(2, S_OK); }
API(sc_GetRasterStatus) { memset(HP(ARG(1)), 0, 8); RETURN_STD(2, S_OK); }
static const ComMethod swap_methods[] = {
  {"IDirect3DSwapChain9::QueryInterface", 3, m_QueryInterface}, {"IDirect3DSwapChain9::AddRef", 1, m_AddRef},
  {"IDirect3DSwapChain9::Release", 1, m_Release}, {"IDirect3DSwapChain9::Present", 6, sc_Present},
  {"IDirect3DSwapChain9::GetFrontBufferData", 2}, {"IDirect3DSwapChain9::GetBackBuffer", 4, sc_GetBackBuffer},
  {"IDirect3DSwapChain9::GetRasterStatus", 2, sc_GetRasterStatus}, {"IDirect3DSwapChain9::GetDisplayMode", 2, sc_GetDisplayMode},
  {"IDirect3DSwapChain9::GetDevice", 2, m_GetDevice}, {"IDirect3DSwapChain9::GetPresentParameters", 2, sc_GetPresentParameters},
};

/* ---- IDirect3D9 ------------------------------------------------------------ */
static const u32 g_modes[][2] = {{800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1600, 900}, {1920, 1080}};
#define NMODES 6
API(i_GetAdapterCount) { RETURN_STD(1, 1); }
API(i_GetAdapterIdentifier) {
  u32 p = ARG(3);
  memset(HP(p), 0, 1100);
  strcpy((char *)HP(p), "nvd3dum.dll");
  strcpy((char *)HP(p + 512), "NVIDIA GeForce 8800 GT (fnv_recomp)");
  strcpy((char *)HP(p + 1024), "\\\\.\\DISPLAY1");
  M32(p + 1056) = 0x00000000; M32(p + 1060) = 0x00080010;
  M32(p + 1064) = 0x10de; M32(p + 1068) = 0x0611; M32(p + 1072) = 0; M32(p + 1076) = 0xa2;
  M32(p + 1080) = 0xd7b71e3e; M32(p + 1084) = 0x11cf4f21; M32(p + 1088) = 0x0000b26b; M32(p + 1092) = 0x35c7c0ff;
  M32(p + 1096) = 1;
  RETURN_STD(4, S_OK);
}
API(i_GetAdapterModeCount) { RETURN_STD(3, (ARG(2) == 22 || ARG(2) == 21) ? NMODES : 0); }
API(i_EnumAdapterModes) {
  u32 m = ARG(3), p = ARG(4);
  if (m >= NMODES) RETURN_STD(5, D3DERR_INVALIDCALL);
  M32(p) = g_modes[m][0]; M32(p + 4) = g_modes[m][1]; M32(p + 8) = 60; M32(p + 12) = ARG(2);
  RETURN_STD(5, S_OK);
}
API(i_GetAdapterDisplayMode) { u32 p = ARG(2); M32(p) = 1920; M32(p + 4) = 1080; M32(p + 8) = 60; M32(p + 12) = 22; RETURN_STD(3, S_OK); }
API(i_CheckDeviceType) { RETURN_STD(6, S_OK); }
static int format_supported(u32 f) {
  switch (f) {
  case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 31: case 32: case 33: case 34: case 35:
  case 36: case 50: case 51: case 71: case 75: case 77: case 80: case 111: case 112: case 113: case 114: case 115: case 116:
    return 1;
  }
  return is_dxt(f) && f != FOURCC('A','T','I','1') && f != FOURCC('A','T','I','2');
}
API(i_CheckDeviceFormat) {
  u32 f = ARG(6);
  int ok = format_supported(f);
  TRACE("CheckDeviceFormat(usage=%x, rtype=%u, fmt=%u/%.4s) = %d", ARG(4), ARG(5), f, (char *)&f, ok);
  RETURN_STD(7, ok ? S_OK : D3DERR_NOTAVAILABLE);
}
API(i_CheckDeviceMultiSampleType) {
  if (ARG(6)) M32(ARG(6)) = 1;
  RETURN_STD(7, ARG(5) <= 1 ? S_OK : D3DERR_NOTAVAILABLE);
}
API(i_CheckDepthStencilMatch) { RETURN_STD(6, S_OK); }
API(i_CheckDeviceFormatConversion) { RETURN_STD(5, S_OK); }
API(i_GetDeviceCaps) { fill_caps(ARG(3)); RETURN_STD(4, S_OK); }
API(i_GetAdapterMonitor) { RETURN_STD(2, 0x60001); }

static void init_vtables(void) {
  if (g_vt_surface) return;
#define VT(v, m) v = com_vtable(m, sizeof m / sizeof *m)
  VT(g_vt_surface, surface_methods); VT(g_vt_texture, texture_methods); VT(g_vt_cube, cube_methods);
  VT(g_vt_volume, volume_methods); VT(g_vt_vb, vb_methods); VT(g_vt_ib, ib_methods); VT(g_vt_vs, vs_methods);
  VT(g_vt_ps, ps_methods); VT(g_vt_decl, decl_methods); VT(g_vt_query, query_methods); VT(g_vt_sb, sb_methods);
  VT(g_vt_swap, swap_methods);
#undef VT
}

API(i_CreateDevice) {
  init_vtables();
  static u32 vt;
  if (!vt) vt = com_vtable(device_methods, sizeof device_methods / sizeof *device_methods);
  if (sizeof device_methods / sizeof *device_methods != 119) rt_fatal(c, "IDirect3DDevice9 vtable has the wrong size");
  D = calloc(1, sizeof *D);
  memcpy(D->pp, HP(ARG(5)), 56);
  D->hwnd = ARG(3); D->behavior = ARG(4);
  for (int s = 0; s < 20; s++) { D->ss[s][1] = D->ss[s][2] = D->ss[s][3] = 1; D->ss[s][5] = D->ss[s][6] = 1; }
  for (int i = 0; i < 16; i++) D->stream_freq[i] = 1;
  D->rs[7] = 1; D->rs[14] = 1; D->rs[22] = 3; D->rs[23] = 4; D->rs[19] = 2; D->rs[20] = 1; D->rs[168] = 0xf; /* ZENABLE ZWRITE CULL ZFUNC SRCBLEND DESTBLEND COLORWRITE */
  make_backbuffers();
  memcpy(HP(ARG(5)), D->pp, 56);
  g_device = new_obj(vt, K_DEVICE, NULL);
  g_swap = new_obj(g_vt_swap, K_SWAP, NULL);
  M32(ARG(6)) = g_device;
  rt_log("D3D9: CreateDevice %ux%u fmt=%u depth=%u windowed=%u behavior=%x -> %08x", D->pp[0], D->pp[1], D->pp[2],
         D->pp[10], D->pp[8], D->behavior, g_device);
  RETURN_STD(7, S_OK);
}

static const ComMethod d3d_methods[] = {
  {"IDirect3D9::QueryInterface", 3, m_QINone}, {"IDirect3D9::AddRef", 1, m_AddRef}, {"IDirect3D9::Release", 1, m_Release},
  {"IDirect3D9::RegisterSoftwareDevice", 2}, {"IDirect3D9::GetAdapterCount", 1, i_GetAdapterCount},
  {"IDirect3D9::GetAdapterIdentifier", 4, i_GetAdapterIdentifier}, {"IDirect3D9::GetAdapterModeCount", 3, i_GetAdapterModeCount},
  {"IDirect3D9::EnumAdapterModes", 5, i_EnumAdapterModes}, {"IDirect3D9::GetAdapterDisplayMode", 3, i_GetAdapterDisplayMode},
  {"IDirect3D9::CheckDeviceType", 6, i_CheckDeviceType}, {"IDirect3D9::CheckDeviceFormat", 7, i_CheckDeviceFormat},
  {"IDirect3D9::CheckDeviceMultiSampleType", 7, i_CheckDeviceMultiSampleType},
  {"IDirect3D9::CheckDepthStencilMatch", 6, i_CheckDepthStencilMatch},
  {"IDirect3D9::CheckDeviceFormatConversion", 5, i_CheckDeviceFormatConversion},
  {"IDirect3D9::GetDeviceCaps", 4, i_GetDeviceCaps}, {"IDirect3D9::GetAdapterMonitor", 2, i_GetAdapterMonitor},
  {"IDirect3D9::CreateDevice", 7, i_CreateDevice},
};

API(Direct3DCreate9) {
  static u32 vt;
  if (!vt) vt = com_vtable(d3d_methods, sizeof d3d_methods / sizeof *d3d_methods);
  init_vtables();
  if (!g_d3d) g_d3d = com_object(vt, 4);
  else M32(g_d3d + 4)++;
  rt_log("Direct3DCreate9(%u) = %08x", ARG(0), g_d3d);
  RETURN_STD(1, g_d3d);
}

/* D3DCAPS9 of a Shader Model 3 class GPU */
static void fill_caps(u32 p) {
  memset(HP(p), 0, 304);
  float f;
  M32(p + 0) = 1;                    /* HAL */
  M32(p + 8) = 0x00020000;           /* Caps: READ_SCANLINE */
  M32(p + 12) = 0xE0020000;          /* Caps2: FULLSCREENGAMMA CANAUTOGENMIPMAP DYNAMICTEXTURES CANCALIBRATEGAMMA */
  M32(p + 16) = 0x000003A0;          /* Caps3 */
  M32(p + 20) = 0x8000000F;          /* PresentationIntervals */
  M32(p + 24) = 1;
  M32(p + 28) = 0x001BBEF0;          /* DevCaps: HWTRANSFORMANDLIGHT, PUREDEVICE, ... */
  M32(p + 32) = 0x002FCEF2;          /* PrimitiveMiscCaps incl. SEPARATEALPHABLEND, MRTINDEPENDENTBITDEPTHS */
  M32(p + 36) = 0x07732191;          /* RasterCaps incl. SCISSORTEST, SLOPESCALEDEPTHBIAS, DEPTHBIAS */
  M32(p + 40) = 0xFF; M32(p + 44) = 0x3FFF; M32(p + 48) = 0x3FFF; M32(p + 52) = 0xFF; M32(p + 56) = 0x00084208;
  M32(p + 60) = 0x0001EC05;          /* TextureCaps */
  M32(p + 64) = 0x03030700; M32(p + 68) = 0x03030300; M32(p + 72) = 0x03030300;
  M32(p + 76) = 0x3F; M32(p + 80) = 0x3F; M32(p + 84) = 0x1F;
  M32(p + 88) = 8192; M32(p + 92) = 8192; M32(p + 96) = 2048; M32(p + 100) = 8192; M32(p + 104) = 8192; M32(p + 108) = 16;
  f = 1e10f; memcpy(HP(p + 112), &f, 4);
  f = -1e8f; memcpy(HP(p + 116), &f, 4); memcpy(HP(p + 120), &f, 4);
  f = 1e8f; memcpy(HP(p + 124), &f, 4); memcpy(HP(p + 128), &f, 4);
  M32(p + 136) = 0x1FF; M32(p + 140) = 0x00180008; M32(p + 144) = 0x03FEFFFF;
  M32(p + 148) = 8; M32(p + 152) = 8; M32(p + 156) = 0x17B; M32(p + 160) = 8; M32(p + 164) = 8; M32(p + 168) = 4;
  M32(p + 172) = 8; f = 8192.0f; memcpy(HP(p + 176), &f, 4);
  M32(p + 180) = 0x00FFFFFF; M32(p + 184) = 0x00FFFFFF; M32(p + 188) = 16; M32(p + 192) = 255;
  M32(p + 196) = 0xFFFE0300; M32(p + 200) = 256; M32(p + 204) = 0xFFFF0300;
  f = 3.4e38f; memcpy(HP(p + 208), &f, 4);
  M32(p + 212) = 0x51; M32(p + 224) = 0; M32(p + 228) = 0; M32(p + 232) = 1;
  M32(p + 236) = 0x37F; M32(p + 240) = 4; M32(p + 244) = 0x03000300;
  M32(p + 248) = 1; M32(p + 252) = 24; M32(p + 256) = 32; M32(p + 260) = 4;
  M32(p + 264) = 0x1F; M32(p + 268) = 24; M32(p + 272) = 32; M32(p + 276) = 4; M32(p + 280) = 512;
  M32(p + 284) = 0x03000300; M32(p + 288) = 65535; M32(p + 292) = 65535; M32(p + 296) = 32768; M32(p + 300) = 32768;
}

static const ApiEntry list[] = {
  {"d3d9.dll", "Direct3DCreate9", Direct3DCreate9},
  {0}};
void d3d9_init(void) { api_register(list); }
