/* GPU backend: the game's D3D9 calls, already recorded by the front end (d3d9.c),
 * replayed on a real IDirect3DDevice9 provided by DXVK (doitsujin/dxvk, zlib
 * license), which runs Direct3D 9 on Vulkan - RADV on the PS5, any Vulkan driver
 * on Linux.
 *
 * The front end stays the source of truth: resources live in guest memory and are
 * uploaded when the game has locked them; device state is synced before each draw
 * from the front end's copy, only where it changed. Render targets live on the GPU
 * and are read back when the game (or a screenshot) asks for their pixels. */
#include <d3d9.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include "d3d9_internal.h"
#include "platform.h"
}

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static int g_active = -1;
static bool g_in_scene;
static IDirect3DSurface9 *g_host_bb, *g_host_depth;

extern "C" int hb_wanted(void) {
  const char *r = getenv("FNV_RENDERER");
  if (r && !strcmp(r, "software")) return 0;
  return 1;
}

extern "C" int hb_active(void) { return g_active == 1; }

extern "C" int hb_init(void) {
  if (g_active >= 0) return g_active;
  g_active = 0;
  if (!hb_wanted() || !plat_gpu_window()) {
    rt_log("GPU backend: off (%s); using the software renderer", hb_wanted() ? "no Vulkan window" : "FNV_RENDERER=software");
    return 0;
  }
#ifdef __PROSPERO__
  setenv("DXVK_WSI_DRIVER", "PS5", 0);
  /* 16 shader-compiler threads by default: each costs host (flexible) memory, more so during compile bursts */
  setenv("DXVK_CONFIG", "dxvk.numCompilerThreads = 4", 0);
  setenv("DXVK_LOG_PATH", "/app0", 0);
  /* performance overlay for now: fps, frame times, GPU load (scaled for a 4K TV) */
  setenv("DXVK_HUD", "fps,frametimes,gpuload,scale=2", 0);
#else
  setenv("DXVK_WSI_DRIVER", "SDL2", 0);
#endif
  g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!g_d3d) { rt_log("GPU backend: DXVK Direct3DCreate9 failed; using the software renderer"); return 0; }
  D3DADAPTER_IDENTIFIER9 id;
  if (SUCCEEDED(g_d3d->GetAdapterIdentifier(0, 0, &id))) rt_log("GPU backend: DXVK on %s", id.Description);
  g_active = 1;
  return 1;
}

/* ---------------------------------------------------------------- device */
static void fill_pp(D3DPRESENT_PARAMETERS *pp) {
  DevState *D = d3d_dev();
  memset(pp, 0, sizeof *pp);
  pp->BackBufferWidth = D->pp[0];
  pp->BackBufferHeight = D->pp[1];
  pp->BackBufferFormat = D3DFMT_X8R8G8B8;
  pp->BackBufferCount = 1;
  pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp->hDeviceWindow = (HWND)plat_window();
  pp->Windowed = TRUE;
  pp->EnableAutoDepthStencil = D->pp[9] ? TRUE : FALSE;
  pp->AutoDepthStencilFormat = (D3DFORMAT)(D->pp[9] ? D->pp[10] : 0);
  pp->PresentationInterval = getenv("FNV_VSYNC") ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

struct Shadow;
static void shadow_reset(void);

extern "C" void hb_create_device(void) {
  if (!hb_init()) return;
  D3DPRESENT_PARAMETERS pp;
  fill_pp(&pp);
  HRESULT hr = g_d3d->CreateDevice(0, D3DDEVTYPE_HAL, (HWND)plat_window(),
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &g_dev);
  if (FAILED(hr)) {
    rt_log("GPU backend: CreateDevice failed (0x%08x); using the software renderer", (unsigned)hr);
    g_active = 0;
    return;
  }
  g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_host_bb);
  g_dev->GetDepthStencilSurface(&g_host_depth);
  shadow_reset();
  rt_log("GPU backend: device %ux%u ready", pp.BackBufferWidth, pp.BackBufferHeight);
}

static void movie_release(void);
extern "C" void hb_reset(void) {
  if (!g_dev) return;
  movie_release(); /* a D3DPOOL_DEFAULT surface: must go before Reset */
  if (g_host_bb) { g_host_bb->Release(); g_host_bb = NULL; }
  if (g_host_depth) { g_host_depth->Release(); g_host_depth = NULL; }
  D3DPRESENT_PARAMETERS pp;
  fill_pp(&pp);
  HRESULT hr = g_dev->Reset(&pp);
  if (FAILED(hr)) rt_log("GPU backend: Reset failed (0x%08x)", (unsigned)hr);
  g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_host_bb);
  g_dev->GetDepthStencilSurface(&g_host_depth);
  shadow_reset();
}

static void begin_scene(void) {
  if (!g_in_scene) { g_dev->BeginScene(); g_in_scene = true; }
}

/* ---------------------------------------------------------------- resources */
static D3DPOOL pool_for(Obj *o) {
  if (o->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)) return D3DPOOL_DEFAULT;
  return D3DPOOL_MANAGED;
}

static void upload_rows(void *dst, int dpitch, u32 src, u32 spitch, u32 rows) {
  u32 n = (u32)dpitch < spitch ? (u32)dpitch : spitch;
  for (u32 y = 0; y < rows; y++) memcpy((char *)dst + (size_t)y * dpitch, HP(src + y * spitch), n);
}

static u32 level_rows(Obj *o, u32 l) {
  u32 h = o->h >> l; if (!h) h = 1;
  return d3d_is_dxt(o->fmt) ? (h + 3) / 4 : h;
}

static void upload_texture(Obj *o) {
  if (o->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) { for (int f = 0; f < 6; f++) o->dirty[f] = 0; return; }
  DWORD lockf = (o->usage & D3DUSAGE_DYNAMIC) ? D3DLOCK_DISCARD : 0;
  if (o->kind == K_TEXTURE) {
    IDirect3DTexture9 *t = (IDirect3DTexture9 *)o->host;
    for (u32 l = 0; l < o->levels; l++) {
      if (!(o->dirty[0] & (1u << l))) continue;
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(t->LockRect(l, &lr, NULL, l ? 0 : lockf))) {
        upload_rows(lr.pBits, lr.Pitch, o->data[0][l], o->pitch[l], level_rows(o, l));
        t->UnlockRect(l);
      }
    }
  } else if (o->kind == K_CUBE) {
    IDirect3DCubeTexture9 *t = (IDirect3DCubeTexture9 *)o->host;
    for (int f = 0; f < 6; f++)
      for (u32 l = 0; l < o->levels; l++) {
        if (!(o->dirty[f] & (1u << l))) continue;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, NULL, 0))) {
          upload_rows(lr.pBits, lr.Pitch, o->data[f][l], o->pitch[l], level_rows(o, l));
          t->UnlockRect((D3DCUBEMAP_FACES)f, l);
        }
      }
  } else if (o->kind == K_VOLUME) {
    IDirect3DVolumeTexture9 *t = (IDirect3DVolumeTexture9 *)o->host;
    for (u32 l = 0; l < o->levels; l++) {
      if (!(o->dirty[0] & (1u << l))) continue;
      D3DLOCKED_BOX lb;
      if (SUCCEEDED(t->LockBox(l, &lb, NULL, 0))) {
        u32 d = o->d >> l; if (!d) d = 1;
        u32 rows = level_rows(o, l), slice = o->pitch[l] * rows;
        for (u32 z = 0; z < d; z++)
          upload_rows((char *)lb.pBits + (size_t)z * lb.SlicePitch, lb.RowPitch, o->data[0][l] + z * slice, o->pitch[l], rows);
        t->UnlockBox(l);
      }
    }
  }
  for (int f = 0; f < 6; f++) o->dirty[f] = 0;
}

static IDirect3DBaseTexture9 *host_texture(Obj *o) {
  if (!o) return NULL;
  if (!o->host) {
    HRESULT hr = E_FAIL;
    DWORD usage = o->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC | D3DUSAGE_AUTOGENMIPMAP);
    D3DPOOL pool = pool_for(o);
    UINT levels = (o->usage & D3DUSAGE_AUTOGENMIPMAP) ? 0 : o->levels;
    if (o->kind == K_TEXTURE) {
      IDirect3DTexture9 *t = NULL;
      hr = g_dev->CreateTexture(o->w, o->h, levels, usage, (D3DFORMAT)o->fmt, pool, &t, NULL);
      o->host = t;
    } else if (o->kind == K_CUBE) {
      IDirect3DCubeTexture9 *t = NULL;
      hr = g_dev->CreateCubeTexture(o->w, levels, usage, (D3DFORMAT)o->fmt, pool, &t, NULL);
      o->host = t;
    } else if (o->kind == K_VOLUME) {
      IDirect3DVolumeTexture9 *t = NULL;
      hr = g_dev->CreateVolumeTexture(o->w, o->h, o->d, levels, usage, (D3DFORMAT)o->fmt, pool, &t, NULL);
      o->host = t;
    }
    if (FAILED(hr) || !o->host) {
      static int warned;
      if (warned++ < 20) rt_log("GPU backend: texture %ux%u fmt %u usage %x failed (0x%08x)", o->w, o->h, o->fmt, o->usage, (unsigned)hr);
      o->host = NULL;
      return NULL;
    }
    for (int f = 0; f < 6; f++) o->dirty[f] = 0xffffffffu;
  }
  if (o->dirty[0] | o->dirty[1] | o->dirty[2] | o->dirty[3] | o->dirty[4] | o->dirty[5]) upload_texture(o);
  return (IDirect3DBaseTexture9 *)o->host;
}

static IDirect3DSurface9 *host_surface(Obj *s) {
  if (!s) return NULL;
  DevState *D = d3d_dev();
  if (s->guest == D->backbuffer) return g_host_bb;
  if (s->guest == D->depth) return g_host_depth;
  if (!s->host) {
    HRESULT hr = E_FAIL;
    IDirect3DSurface9 *surf = NULL;
    if (s->parent) {
      Obj *t = d3d_obj(s->parent);
      IDirect3DBaseTexture9 *bt = host_texture(t);
      if (!bt) return NULL;
      if (t->kind == K_CUBE) hr = ((IDirect3DCubeTexture9 *)bt)->GetCubeMapSurface((D3DCUBEMAP_FACES)s->face, s->level, &surf);
      else hr = ((IDirect3DTexture9 *)bt)->GetSurfaceLevel(s->level, &surf);
    } else if (s->usage & D3DUSAGE_RENDERTARGET) {
      hr = g_dev->CreateRenderTarget(s->w, s->h, (D3DFORMAT)s->fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &surf, NULL);
    } else if (s->usage & D3DUSAGE_DEPTHSTENCIL) {
      hr = g_dev->CreateDepthStencilSurface(s->w, s->h, (D3DFORMAT)s->fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &surf, NULL);
    } else {
      hr = g_dev->CreateOffscreenPlainSurface(s->w, s->h, (D3DFORMAT)s->fmt, D3DPOOL_SYSTEMMEM, &surf, NULL);
    }
    if (FAILED(hr)) {
      static int warned;
      if (warned++ < 20) rt_log("GPU backend: surface %ux%u fmt %u usage %x failed (0x%08x)", s->w, s->h, s->fmt, s->usage, (unsigned)hr);
      return NULL;
    }
    s->host = surf;
  }
  return (IDirect3DSurface9 *)s->host;
}

static IDirect3DVertexBuffer9 *host_vb(Obj *o) {
  if (!o) return NULL;
  if (!o->host) {
    IDirect3DVertexBuffer9 *b = NULL;
    DWORD usage = (o->usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_POINTS));
    if (FAILED(g_dev->CreateVertexBuffer(o->size, usage, 0, (o->usage & D3DUSAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &b, NULL))) return NULL;
    o->host = b; o->dirty_lo = 0; o->dirty_hi = o->size;
  }
  IDirect3DVertexBuffer9 *b = (IDirect3DVertexBuffer9 *)o->host;
  if (o->dirty_hi > o->dirty_lo) {
    void *p;
    bool whole = o->dirty_lo == 0 && o->dirty_hi >= o->size;
    DWORD f = (o->usage & D3DUSAGE_DYNAMIC) ? (whole ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE) : 0;
    if (SUCCEEDED(b->Lock(o->dirty_lo, o->dirty_hi - o->dirty_lo, &p, f))) {
      memcpy(p, HP(o->data[0][0] + o->dirty_lo), o->dirty_hi - o->dirty_lo);
      b->Unlock();
    }
    o->dirty_lo = o->dirty_hi = 0;
  }
  return b;
}

static IDirect3DIndexBuffer9 *host_ib(Obj *o) {
  if (!o) return NULL;
  if (!o->host) {
    IDirect3DIndexBuffer9 *b = NULL;
    DWORD usage = (o->usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY));
    if (FAILED(g_dev->CreateIndexBuffer(o->size, usage, (D3DFORMAT)o->fmt, (o->usage & D3DUSAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &b, NULL))) return NULL;
    o->host = b; o->dirty_lo = 0; o->dirty_hi = o->size;
  }
  IDirect3DIndexBuffer9 *b = (IDirect3DIndexBuffer9 *)o->host;
  if (o->dirty_hi > o->dirty_lo) {
    void *p;
    bool whole = o->dirty_lo == 0 && o->dirty_hi >= o->size;
    DWORD f = (o->usage & D3DUSAGE_DYNAMIC) ? (whole ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE) : 0;
    if (SUCCEEDED(b->Lock(o->dirty_lo, o->dirty_hi - o->dirty_lo, &p, f))) {
      memcpy(p, HP(o->data[0][0] + o->dirty_lo), o->dirty_hi - o->dirty_lo);
      b->Unlock();
    }
    o->dirty_lo = o->dirty_hi = 0;
  }
  return b;
}

static IDirect3DVertexShader9 *host_vs(Obj *o) {
  if (!o) return NULL;
  if (!o->host) {
    IDirect3DVertexShader9 *s = NULL;
    if (FAILED(g_dev->CreateVertexShader((const DWORD *)HP(o->blob), &s))) { rt_log("GPU backend: vertex shader rejected"); return NULL; }
    o->host = s;
  }
  return (IDirect3DVertexShader9 *)o->host;
}
static IDirect3DPixelShader9 *host_ps(Obj *o) {
  if (!o) return NULL;
  if (!o->host) {
    IDirect3DPixelShader9 *s = NULL;
    if (FAILED(g_dev->CreatePixelShader((const DWORD *)HP(o->blob), &s))) { rt_log("GPU backend: pixel shader rejected"); return NULL; }
    o->host = s;
  }
  return (IDirect3DPixelShader9 *)o->host;
}
static IDirect3DVertexDeclaration9 *host_decl(Obj *o) {
  if (!o) return NULL;
  if (!o->host) {
    IDirect3DVertexDeclaration9 *d = NULL;
    if (FAILED(g_dev->CreateVertexDeclaration((const D3DVERTEXELEMENT9 *)HP(o->blob), &d))) return NULL;
    o->host = d;
  }
  return (IDirect3DVertexDeclaration9 *)o->host;
}

extern "C" void hb_release(Obj *o) {
  if (!o->host) return;
  DevState *D = d3d_dev();
  if (D && (o->guest == D->backbuffer || o->guest == D->depth)) { o->host = NULL; return; }
  ((IUnknown *)o->host)->Release();
  o->host = NULL;
}

/* ---------------------------------------------------------------- state sync */
struct Shadow {
  u32 rs[256];
  u32 ss[20][14];
  u32 tss[8][33];
  void *tex[20];
  void *stream[16]; u32 off[16], stride[16], freq[16];
  void *ib, *vs, *ps, *decl; u32 fvf;
  void *rt[4], *ds;
  float vsf[256][4], psf[224][4];
  s32 vsi[16][4], psi[16][4];
  u32 vsb[16], psb[16];
  float transform[512][16];
  u32 valid;
};
static Shadow *S;

static void shadow_reset(void) {
  if (!S) S = (Shadow *)calloc(1, sizeof(Shadow));
  memset(S, 0xff, sizeof(Shadow)); /* everything "unknown": the first draw sets it all */
  S->valid = 1;
}

static DWORD sampler_stage(int s) { return s < 16 ? (DWORD)s : s == 16 ? D3DDMAPSAMPLER : (DWORD)(D3DVERTEXTEXTURESAMPLER0 + (s - 17)); }

template <typename T, typename F>
static void sync_consts(T (*cur)[4], T (*sh)[4], int n, F set) {
  int i = 0;
  while (i < n) {
    if (!memcmp(cur[i], sh[i], sizeof cur[i])) { i++; continue; }
    int j = i;
    while (j < n && memcmp(cur[j], sh[j], sizeof cur[j])) j++;
    set(i, &cur[i][0], j - i);
    memcpy(sh[i], cur[i], sizeof cur[i] * (j - i));
    i = j;
  }
}

static void sync_state(void) {
  DevState *D = d3d_dev();
  /* render targets first: SetRenderTarget resets the viewport */
  for (int i = 0; i < 4; i++) {
    IDirect3DSurface9 *s = D->rt[i] ? host_surface(d3d_obj(D->rt[i])) : NULL;
    if (i == 0 && !s) s = g_host_bb;
    if (s != S->rt[i]) { g_dev->SetRenderTarget(i, s); S->rt[i] = s; }
  }
  IDirect3DSurface9 *ds = D->ds ? host_surface(d3d_obj(D->ds)) : NULL;
  if (ds != S->ds) { g_dev->SetDepthStencilSurface(ds); S->ds = ds; }
  D3DVIEWPORT9 vp;
  memcpy(&vp, D->viewport, sizeof vp);
  g_dev->SetViewport(&vp);
  RECT sc = {(LONG)D->scissor[0], (LONG)D->scissor[1], (LONG)D->scissor[2], (LONG)D->scissor[3]};
  g_dev->SetScissorRect(&sc);

  for (int s = 0; s < 256; s++)
    if (D->rs_set[s] && D->rs[s] != S->rs[s]) { g_dev->SetRenderState((D3DRENDERSTATETYPE)s, D->rs[s]); S->rs[s] = D->rs[s]; }
  for (int s = 0; s < 20; s++)
    for (int t = 1; t < 14; t++)
      if (D->ss_set[s][t] && D->ss[s][t] != S->ss[s][t]) { g_dev->SetSamplerState(sampler_stage(s), (D3DSAMPLERSTATETYPE)t, D->ss[s][t]); S->ss[s][t] = D->ss[s][t]; }
  for (int s = 0; s < 8; s++)
    for (int t = 1; t < 33; t++)
      if (D->tss_set[s][t] && D->tss[s][t] != S->tss[s][t]) { g_dev->SetTextureStageState(s, (D3DTEXTURESTAGESTATETYPE)t, D->tss[s][t]); S->tss[s][t] = D->tss[s][t]; }
  for (int s = 0; s < 20; s++) {
    Obj *t = D->tex[s] ? d3d_obj(D->tex[s]) : NULL;
    IDirect3DBaseTexture9 *ht = (t && t->kind != K_SURFACE) ? host_texture(t) : NULL;
    if (ht != S->tex[s]) { g_dev->SetTexture(sampler_stage(s), ht); S->tex[s] = ht; }
  }
  for (int i = 0; i < 16; i++) {
    IDirect3DVertexBuffer9 *vb = D->stream[i] ? host_vb(d3d_obj(D->stream[i])) : NULL;
    if (vb != S->stream[i] || D->stream_off[i] != S->off[i] || D->stream_stride[i] != S->stride[i]) {
      g_dev->SetStreamSource(i, vb, D->stream_off[i], D->stream_stride[i]);
      S->stream[i] = vb; S->off[i] = D->stream_off[i]; S->stride[i] = D->stream_stride[i];
    }
    if (D->stream_freq[i] != S->freq[i]) { g_dev->SetStreamSourceFreq(i, D->stream_freq[i]); S->freq[i] = D->stream_freq[i]; }
  }
  IDirect3DIndexBuffer9 *ib = D->indices ? host_ib(d3d_obj(D->indices)) : NULL;
  if (ib != S->ib) { g_dev->SetIndices(ib); S->ib = ib; }
  IDirect3DVertexShader9 *vs = D->vs ? host_vs(d3d_obj(D->vs)) : NULL;
  if (vs != S->vs) { g_dev->SetVertexShader(vs); S->vs = vs; }
  IDirect3DPixelShader9 *ps = D->ps ? host_ps(d3d_obj(D->ps)) : NULL;
  if (ps != S->ps) { g_dev->SetPixelShader(ps); S->ps = ps; }
  IDirect3DVertexDeclaration9 *decl = D->decl ? host_decl(d3d_obj(D->decl)) : NULL;
  if (decl) {
    if (decl != S->decl) { g_dev->SetVertexDeclaration(decl); S->decl = decl; S->fvf = 0xffffffffu; }
  } else if (D->fvf != S->fvf) { g_dev->SetFVF(D->fvf); S->fvf = D->fvf; S->decl = NULL; }

  sync_consts(D->vsf, S->vsf, 256, [](int r, float *v, int n) { g_dev->SetVertexShaderConstantF(r, v, n); });
  sync_consts(D->psf, S->psf, 224, [](int r, float *v, int n) { g_dev->SetPixelShaderConstantF(r, v, n); });
  sync_consts(D->vsi, S->vsi, 16, [](int r, s32 *v, int n) { g_dev->SetVertexShaderConstantI(r, (const int *)v, n); });
  sync_consts(D->psi, S->psi, 16, [](int r, s32 *v, int n) { g_dev->SetPixelShaderConstantI(r, (const int *)v, n); });
  for (int i = 0; i < 16; i++) {
    if (D->vsb[i] != S->vsb[i]) { BOOL b = D->vsb[i] != 0; g_dev->SetVertexShaderConstantB(i, &b, 1); S->vsb[i] = D->vsb[i]; }
    if (D->psb[i] != S->psb[i]) { BOOL b = D->psb[i] != 0; g_dev->SetPixelShaderConstantB(i, &b, 1); S->psb[i] = D->psb[i]; }
  }
  if (!vs || !ps) { /* fixed function bits */
    static const int ts[] = {2, 3, 256, 16, 17, 18, 19, 20, 21, 22, 23};
    for (unsigned k = 0; k < sizeof ts / sizeof *ts; k++) {
      int t = ts[k];
      if (memcmp(D->transform[t], S->transform[t], 64)) {
        g_dev->SetTransform((D3DTRANSFORMSTATETYPE)t, (const D3DMATRIX *)D->transform[t]);
        memcpy(S->transform[t], D->transform[t], 64);
      }
    }
    g_dev->SetMaterial((const D3DMATERIAL9 *)D->material);
  }
}

/* ---------------------------------------------------------------- drawing */
extern "C" void hb_draw(u32 type, u32 base, u32 min_index, u32 num_vertices, u32 start, u32 prims, u32 idx_data,
                        u32 idx_fmt, u32 up_data, u32 up_stride, int indexed) {
  if (!g_dev || !prims) return;
  sync_state();
  begin_scene();
  if (up_data) {
    if (idx_data)
      g_dev->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)type, min_index, num_vertices, prims, HP(idx_data), (D3DFORMAT)idx_fmt,
                                    HP(up_data), up_stride);
    else
      g_dev->DrawPrimitiveUP((D3DPRIMITIVETYPE)type, prims, HP(up_data), up_stride);
    /* UP draws unbind stream 0 and the index buffer on the device */
    S->stream[0] = (void *)~(uintptr_t)0; S->ib = (void *)~(uintptr_t)0;
    return;
  }
  if (indexed) g_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)type, (INT)base, min_index, num_vertices, start, prims);
  else g_dev->DrawPrimitive((D3DPRIMITIVETYPE)type, start, prims);
}

extern "C" void hb_clear(u32 count, u32 rects, u32 flags, u32 color, float z, u32 stencil) {
  if (!g_dev) return;
  sync_state();
  begin_scene();
  DevState *D = d3d_dev();
  /* without a depth surface bound, D3D9 rejects a depth clear */
  if (!D->ds) flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
  g_dev->Clear(count, rects ? (const D3DRECT *)HP(rects) : NULL, flags, color, z, stencil);
}

static RECT *rect_from(u32 p, RECT *r) {
  if (!p) return NULL;
  r->left = (s32)M32(p); r->top = (s32)M32(p + 4); r->right = (s32)M32(p + 8); r->bottom = (s32)M32(p + 12);
  return r;
}

extern "C" void hb_stretch(u32 src, u32 srect, u32 dst, u32 drect, u32 filter) {
  if (!g_dev) return;
  IDirect3DSurface9 *s = host_surface(d3d_obj(src)), *d = host_surface(d3d_obj(dst));
  if (!s || !d) return;
  RECT a, b;
  g_dev->StretchRect(s, rect_from(srect, &a), d, rect_from(drect, &b), (D3DTEXTUREFILTERTYPE)filter);
}

extern "C" void hb_colorfill(u32 surf, u32 rect, u32 color) {
  if (!g_dev) return;
  IDirect3DSurface9 *s = host_surface(d3d_obj(surf));
  if (!s) return;
  RECT r;
  g_dev->ColorFill(s, rect_from(rect, &r), color);
}

/* A Bink movie (bink.c) is drawn over the whole frame here, scaled to fit, before presenting: the game draws it as
   fixed-function textured tiles, which came out black through this backend. */
extern "C" int bink_picture(const u32 **px, int *w, int *h, unsigned *serial);
static IDirect3DSurface9 *g_movie;
static int g_movie_w, g_movie_h;
static unsigned g_movie_serial;
static void movie_release(void) {
  if (g_movie) { g_movie->Release(); g_movie = NULL; }
}
static void movie_overlay(void) {
  const u32 *px;
  int w, h;
  unsigned serial;
  if (!bink_picture(&px, &w, &h, &serial)) { movie_release(); return; }
  if (!g_movie || w != g_movie_w || h != g_movie_h) {
    movie_release();
    if (FAILED(g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_movie, NULL))) {
      static int warned;
      if (!warned++) rt_log("GPU backend: no surface for the movie");
      g_movie = NULL;
      return;
    }
    g_movie_w = w; g_movie_h = h; g_movie_serial = serial - 1;
  }
  if (serial != g_movie_serial) {
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_movie->LockRect(&lr, NULL, 0))) {
      for (int y = 0; y < h; y++) memcpy((char *)lr.pBits + (size_t)y * lr.Pitch, px + (size_t)y * w, (size_t)w * 4);
      g_movie->UnlockRect();
    }
    g_movie_serial = serial;
  }
  D3DSURFACE_DESC bd;
  if (!g_host_bb || FAILED(g_host_bb->GetDesc(&bd))) return;
  /* fit, keeping the movie's shape; the game has already cleared the frame to black */
  LONG W = bd.Width, H = bd.Height, dw = W, dh = (LONG)((long long)W * h / w);
  if (dh > H) { dh = H; dw = (LONG)((long long)H * w / h); }
  RECT dst = {(W - dw) / 2, (H - dh) / 2, (W - dw) / 2 + dw, (H - dh) / 2 + dh};
  g_dev->StretchRect(g_movie, NULL, g_host_bb, &dst, D3DTEXF_LINEAR);
}

/* called before the frame is presented (and before a test screenshot) */
extern "C" void hb_movie_overlay(void) {
  if (!g_dev) return;
  if (g_in_scene) { g_dev->EndScene(); g_in_scene = false; }
  movie_overlay();
}

extern "C" void hb_present(void) {
  if (!g_dev) return;
  if (g_in_scene) { g_dev->EndScene(); g_in_scene = false; }
  g_dev->Present(NULL, NULL, NULL, NULL);
}

/* copy a GPU render target into the guest storage of dst (or of src) */
extern "C" void hb_readback(Obj *src, Obj *dst) {
  if (!g_dev || !src) return;
  if (!dst) dst = src;
  IDirect3DSurface9 *s = host_surface(src);
  if (!s) return;
  D3DSURFACE_DESC desc;
  s->GetDesc(&desc);
  IDirect3DSurface9 *sys = NULL;
  if (FAILED(g_dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))) return;
  if (g_in_scene) { g_dev->EndScene(); g_in_scene = false; }
  if (SUCCEEDED(g_dev->GetRenderTargetData(s, sys))) {
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
      u32 rows = d3d_is_dxt(dst->fmt) ? (dst->h + 3) / 4 : dst->h;
      if (rows > desc.Height) rows = desc.Height;
      u32 n = (u32)lr.Pitch < dst->pitch[0] ? (u32)lr.Pitch : dst->pitch[0];
      for (u32 y = 0; y < rows; y++) memcpy(HP(dst->data[0][0] + y * dst->pitch[0]), (char *)lr.pBits + (size_t)y * lr.Pitch, n);
      sys->UnlockRect();
    }
  }
  sys->Release();
}

/* ---- GPU memory use per Vulkan heap, for the perf log line ---------------------------------------
 * DXVK's interop interface (ID3D9VkInteropInterface) gives the Vulkan instance and physical device; the
 * driver's VK_EXT_memory_budget reports usage and budget per heap. The few Vulkan structures needed are
 * declared here with the layouts and values of vulkan_core.h. */
#include <stdint.h>
typedef void *VkInst_;
typedef void *VkPhys_;
struct VkMemType_ { uint32_t flags, heap; };
struct VkMemHeap_ { uint64_t size; uint32_t flags; };
struct VkMemProps_ { uint32_t type_count; VkMemType_ types[32]; uint32_t heap_count; VkMemHeap_ heaps[16]; };
struct VkMemProps2_ { uint32_t stype; void *next; VkMemProps_ props; };          /* ..._MEMORY_PROPERTIES_2 */
struct VkMemBudget_ { uint32_t stype; void *next; uint64_t budget[16], usage[16]; }; /* ..._MEMORY_BUDGET_PROPERTIES_EXT */
typedef void (*VkVoidFn_)(void);
typedef VkVoidFn_ (*VkGipa_)(VkInst_, const char *);
typedef void (*VkMemProps2Fn_)(VkPhys_, VkMemProps2_ *);
static_assert(sizeof(VkMemProps_) == 520, "VkPhysicalDeviceMemoryProperties layout");

struct VkInterop_ : public IUnknown { /* ID3D9VkInteropInterface */
  virtual void STDMETHODCALLTYPE GetInstanceHandle(VkInst_ *instance) = 0;
  virtual void STDMETHODCALLTYPE GetPhysicalDeviceHandle(UINT adapter, VkPhys_ *phys) = 0;
};
static const GUID IID_VkInterop_ = {0x3461a81b, 0xce41, 0x485b, {0xb6, 0xb5, 0xfc, 0xf0, 0x8b, 0xa6, 0xa6, 0xbd}};

#ifdef __PROSPERO__
extern "C" VkVoidFn_ vk_icdGetInstanceProcAddr(VkInst_ instance, const char *name); /* RADV, linked in */
static VkGipa_ vk_gipa(void) { return vk_icdGetInstanceProcAddr; }
#else
#include <dlfcn.h>
static VkGipa_ vk_gipa(void) {
  void *h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
  if (!h) h = dlopen("libvulkan.so.1", RTLD_NOW);
  return h ? (VkGipa_)dlsym(h, "vkGetInstanceProcAddr") : NULL;
}
#endif

extern "C" int hb_gpu_mem(char *out, int cap) {
  static VkMemProps2Fn_ props2;
  static VkPhys_ phys;
  static int tried;
  if (!g_d3d || g_active != 1) return 0;
  if (!tried) {
    tried = 1;
    VkInterop_ *it = NULL;
    VkInst_ inst = NULL;
    if (SUCCEEDED(g_d3d->QueryInterface(IID_VkInterop_, (void **)&it)) && it) {
      it->GetInstanceHandle(&inst);
      it->GetPhysicalDeviceHandle(0, &phys);
      it->Release();
    }
    VkGipa_ gipa = vk_gipa();
    if (gipa && inst && phys) {
      props2 = (VkMemProps2Fn_)gipa(inst, "vkGetPhysicalDeviceMemoryProperties2");
      if (!props2) props2 = (VkMemProps2Fn_)gipa(inst, "vkGetPhysicalDeviceMemoryProperties2KHR");
    }
    if (!props2) rt_log("perf: GPU memory figures unavailable (no Vulkan memory query)");
  }
  if (!props2) return 0;
  VkMemBudget_ b;
  VkMemProps2_ p;
  memset(&b, 0, sizeof b);
  memset(&p, 0, sizeof p);
  b.stype = 1000237000;
  p.stype = 1000059006;
  p.next = &b;
  props2(phys, &p);
  int o = 0;
  for (uint32_t h = 0; h < p.props.heap_count && h < 16 && o < cap - 64; h++)
    o += snprintf(out + o, cap - o, "%sheap%u %s %llu/%llu MB", h ? ", " : "", h,
                  (p.props.heaps[h].flags & 1) ? "(VRAM)" : "(system)", (unsigned long long)(b.usage[h] >> 20),
                  (unsigned long long)(b.budget[h] >> 20));
  return o;
}
