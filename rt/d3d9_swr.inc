/* Software rasterizer backend for the D3D9 front end (included from d3d9.c).
 *
 * Shaders (vs/ps 1.1-3.0 bytecode) are decoded once into an instruction list and
 * interpreted per vertex and per pixel.  Triangles are clipped against the near
 * plane, rasterized with perspective-correct interpolation and written into the
 * render target's own storage (guest memory), so render-to-texture, StretchRect
 * and LockRect on render targets all see the same pixels. */

/* ---------------------------------------------------------------- shaders */
enum { R_TEMP, R_INPUT, R_CONST, R_ADDR, R_RASTOUT, R_ATTROUT, R_OUTPUT, R_CONSTINT, R_COLOROUT, R_DEPTHOUT,
       R_SAMPLER, R_CONST2, R_CONST3, R_CONST4, R_CONSTBOOL, R_LOOP, R_TEMPF16, R_MISC, R_LABEL, R_PRED };
typedef struct { u8 type, mod, rel, rel_type, rel_comp; u8 swz[4]; u16 num, rel_num; } Src;
typedef struct { u8 type, mask, sat; s8 shift; u16 num; } Dst;
typedef struct { u16 op; u8 ctrl, nsrc, pred, pred_neg, pred_swz; u8 has_dst; Dst d; Src s[4]; float imm[4]; int jump; } Ins;
typedef struct {
  Ins *ins; int n; int ps, major, minor;
  u8 in_usage[16], in_index[16], in_valid[16];   /* dcl v# */
  u8 out_usage[12], out_index[12], out_valid[12]; /* SM3 dcl o# */
  u8 samp_type[16];
  int labels[2048];
  int warned;
} Shader;

static const int sm1_len[] = {
  /*0 nop*/0, 2, 3, 3, 4, 3, 2, 2, 3, 3, 3, 3, 3, 3, 2, 2, /*16 lit*/2, 3, 4, 2, 3, 3, 3, 3, 3,
};

static int tok_src(u32 t, Src *s, const u32 *p, int *k, int sm_major) {
  s->num = t & 0x7ff;
  s->type = ((t >> 28) & 7) | ((t >> 8) & 0x18);
  for (int i = 0; i < 4; i++) s->swz[i] = (t >> (16 + 2 * i)) & 3;
  s->mod = (t >> 24) & 0xf;
  s->rel = (t >> 13) & 1;
  s->rel_type = R_ADDR; s->rel_num = 0; s->rel_comp = 0;
  if (s->rel && sm_major >= 2) {
    u32 r = p[(*k)++];
    s->rel_type = ((r >> 28) & 7) | ((r >> 8) & 0x18);
    s->rel_num = r & 0x7ff;
    s->rel_comp = (r >> 16) & 3;
  }
  return 1;
}
static void tok_dst(u32 t, Dst *d) {
  d->num = t & 0x7ff;
  d->type = ((t >> 28) & 7) | ((t >> 8) & 0x18);
  d->mask = (t >> 16) & 0xf;
  d->sat = (t >> 20) & 1;
  int sh = (t >> 24) & 0xf;
  d->shift = sh >= 8 ? sh - 16 : sh;
}

static Shader *shader_decode(u32 blob, u32 size) {
  Shader *sh = calloc(1, sizeof *sh);
  const u32 *p = (const u32 *)HP(blob);
  u32 n = size / 4;
  u32 ver = p[0];
  sh->ps = (ver >> 16) == 0xffff;
  sh->major = (ver >> 8) & 0xff; sh->minor = ver & 0xff;
  sh->ins = calloc(n, sizeof(Ins));
  for (int i = 0; i < 2048; i++) sh->labels[i] = -1;
  int stack[64], sp = 0;
  u32 k = 1;
  while (k < n) {
    u32 t = p[k];
    if (t == 0x0000ffff) break;
    u16 op = t & 0xffff;
    if (op == 0xfffe) { k += 1 + ((t >> 16) & 0x7fff); continue; }
    u32 len;
    if (sh->major >= 2) len = (t >> 24) & 0xf;
    else if (op < sizeof sm1_len / sizeof *sm1_len) len = sm1_len[op];
    else if (op == 31) len = 2;
    else if (op == 81) len = 5;
    else if (op == 80 || op == 88) len = 4;
    else if (op == 64 || op == 65 || op == 66) len = (sh->ps && sh->major == 1 && sh->minor >= 4 && op != 65) ? 2 : 1;
    else if (op == 78 || op == 79) len = 2;
    else len = 3;
    const u32 *a = p + k + 1;
    k += 1 + len;
    Ins *in = &sh->ins[sh->n];
    memset(in, 0, sizeof *in);
    in->op = op; in->ctrl = (t >> 16) & 0xff; in->jump = -1;
    int j = 0;
    if (op == 31) { /* dcl usage, reg */
      u32 u = a[0]; Dst d; tok_dst(a[1], &d);
      if (d.type == R_INPUT && d.num < 16) { sh->in_usage[d.num] = u & 0x1f; sh->in_index[d.num] = (u >> 16) & 0xf; sh->in_valid[d.num] = 1; }
      if (d.type == R_OUTPUT && d.num < 12) { sh->out_usage[d.num] = u & 0x1f; sh->out_index[d.num] = (u >> 16) & 0xf; sh->out_valid[d.num] = 1; }
      if (d.type == R_SAMPLER && d.num < 16) sh->samp_type[d.num] = (u >> 27) & 0xf;
      continue;
    }
    if (op == 81 || op == 48 || op == 47) { /* def defi defb */
      tok_dst(a[0], &in->d); in->has_dst = 1;
      if (op == 47) { u32 v = a[1]; in->imm[0] = v ? 1.0f : 0.0f; }
      else if (op == 48) for (int c4 = 0; c4 < 4; c4++) in->imm[c4] = (float)(s32)a[1 + c4];
      else memcpy(in->imm, a + 1, 16);
      sh->n++;
      continue;
    }
    if (op == 30) { /* label */
      Src s; int kk = 1; tok_src(a[0], &s, a, &kk, sh->major);
      if (s.num < 2048) sh->labels[s.num] = sh->n;
      continue;
    }
    if (t & 0x10000000) { /* predicated: predicate source token comes after dst */ in->pred = 1; }
    /* destination? */
    int has_dst = !(op == 0 || op == 25 || op == 26 || op == 27 || op == 28 || op == 29 || op == 38 || op == 39 ||
                    op == 40 || op == 41 || op == 42 || op == 43 || op == 44 || op == 45 || op == 96 ||
                    (op == 65));
    if (has_dst && len) { tok_dst(a[j++], &in->d); in->has_dst = 1; }
    if (in->pred && j < (int)len) {
      Src ps; int kk = j + 1; tok_src(a[j], &ps, a, &kk, sh->major); j = kk;
      in->pred_neg = ps.mod == 1; in->pred_swz = ps.swz[0];
    }
    int jj = j;
    while (jj < (int)len && in->nsrc < 4) { int kk = jj + 1; tok_src(a[jj], &in->s[in->nsrc], a, &kk, sh->major); jj = kk; in->nsrc++; }
    /* control flow matching */
    if (op == 40 || op == 41 || op == 27 || op == 38) stack[sp++] = sh->n;
    else if (op == 42) { sh->ins[stack[sp - 1]].jump = sh->n; stack[sp - 1] = sh->n; }
    else if (op == 43 || op == 29 || op == 39) {
      int o = stack[--sp];
      sh->ins[o].jump = sh->n;
      in->jump = o; /* endloop/endrep -> loop head; endif -> if/else */
    }
    sh->n++;
  }
  return sh;
}

/* ---------------------------------------------------------------- textures */
typedef struct { float r, g, b, a; } F4;
static inline float h2f(u16 h) {
  u32 s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float f;
  if (e == 0) f = ldexpf((float)m, -24);
  else if (e == 31) f = m ? NAN : INFINITY;
  else f = ldexpf((float)(m | 1024), (int)e - 25);
  return s ? -f : f;
}
static inline u16 f2h(float f) {
  u32 x; memcpy(&x, &f, 4);
  u32 s = (x >> 16) & 0x8000; int e = ((x >> 23) & 255) - 127 + 15; u32 m = x & 0x7fffff;
  if (e <= 0) return s;
  if (e >= 31) return s | 0x7c00;
  return s | (e << 10) | (m >> 13);
}
static void dxt_color(u32 blk, u32 c[4], int dxt1) {
  u16 c0 = M16(blk), c1 = M16(blk + 2);
  u32 r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
  u32 r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
  c[0] = 0xff000000u | (r0 << 16) | (g0 << 8) | b0;
  c[1] = 0xff000000u | (r1 << 16) | (g1 << 8) | b1;
  if (c0 > c1 || !dxt1) {
    c[2] = 0xff000000u | (((2 * r0 + r1) / 3) << 16) | (((2 * g0 + g1) / 3) << 8) | ((2 * b0 + b1) / 3);
    c[3] = 0xff000000u | (((r0 + 2 * r1) / 3) << 16) | (((g0 + 2 * g1) / 3) << 8) | ((b0 + 2 * b1) / 3);
  } else {
    c[2] = 0xff000000u | (((r0 + r1) / 2) << 16) | (((g0 + g1) / 2) << 8) | ((b0 + b1) / 2);
    c[3] = 0;
  }
}
/* decode a whole DXT level into BGRA8 */
static u32 *dxt_decode(u32 fmt, u32 src, u32 w, u32 h, u32 pitch) {
  u32 *out = malloc((size_t)w * h * 4);
  int kind = fmt == FOURCC('D','X','T','1') ? 1 : (fmt == FOURCC('D','X','T','2') || fmt == FOURCC('D','X','T','3')) ? 3 : 5;
  for (u32 by = 0; by < (h + 3) / 4; by++)
    for (u32 bx = 0; bx < (w + 3) / 4; bx++) {
      u32 blk = src + by * pitch + bx * (kind == 1 ? 8 : 16);
      u32 cb = kind == 1 ? blk : blk + 8;
      u32 col[4]; dxt_color(cb, col, kind == 1);
      u32 idx = M32(cb + 4);
      u8 alpha[16];
      if (kind == 3) { u64 a = M64(blk); for (int i = 0; i < 16; i++) alpha[i] = ((a >> (4 * i)) & 15) * 17; }
      else if (kind == 5) {
        u8 a0 = M8(blk), a1 = M8(blk + 1); u64 bits = M64(blk) >> 16; u8 pal[8];
        pal[0] = a0; pal[1] = a1;
        if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
        for (int i = 0; i < 16; i++) alpha[i] = pal[(bits >> (3 * i)) & 7];
      }
      for (int py = 0; py < 4; py++)
        for (int px = 0; px < 4; px++) {
          u32 x = bx * 4 + px, y = by * 4 + py;
          if (x >= w || y >= h) continue;
          int i = py * 4 + px;
          u32 cc = col[(idx >> (2 * i)) & 3];
          if (kind != 1) cc = (cc & 0xffffff) | ((u32)alpha[i] << 24);
          out[y * w + x] = cc;
        }
    }
  return out;
}

typedef struct { u32 data, pitch, w, h, d, fmt; u32 *cache; } Level;

static F4 texel(const Level *L, int x, int y, int z) {
  F4 r = {0, 0, 0, 1};
  if (L->cache) {
    u32 v = L->cache[(u32)y * L->w + x];
    r.r = ((v >> 16) & 255) / 255.f; r.g = ((v >> 8) & 255) / 255.f; r.b = (v & 255) / 255.f; r.a = (v >> 24) / 255.f;
    return r;
  }
  u32 slice = L->pitch * L->h;
  u32 a = L->data + (u32)z * slice + (u32)y * L->pitch;
  switch (L->fmt) {
  case 21: case 22: { u32 v = M32(a + 4 * x); r.r = ((v >> 16) & 255) / 255.f; r.g = ((v >> 8) & 255) / 255.f; r.b = (v & 255) / 255.f; r.a = L->fmt == 22 ? 1 : (v >> 24) / 255.f; break; }
  case 32: case 33: { u32 v = M32(a + 4 * x); r.r = (v & 255) / 255.f; r.g = ((v >> 8) & 255) / 255.f; r.b = ((v >> 16) & 255) / 255.f; r.a = L->fmt == 33 ? 1 : (v >> 24) / 255.f; break; }
  case 23: { u16 v = M16(a + 2 * x); r.r = (v >> 11) / 31.f; r.g = ((v >> 5) & 63) / 63.f; r.b = (v & 31) / 31.f; break; }
  case 24: case 25: { u16 v = M16(a + 2 * x); r.r = ((v >> 10) & 31) / 31.f; r.g = ((v >> 5) & 31) / 31.f; r.b = (v & 31) / 31.f; r.a = L->fmt == 24 ? 1 : (v >> 15); break; }
  case 26: { u16 v = M16(a + 2 * x); r.r = ((v >> 8) & 15) / 15.f; r.g = ((v >> 4) & 15) / 15.f; r.b = (v & 15) / 15.f; r.a = (v >> 12) / 15.f; break; }
  case 28: r.r = r.g = r.b = 0; r.a = M8(a + x) / 255.f; break;
  case 50: r.r = r.g = r.b = M8(a + x) / 255.f; break;
  case 51: r.r = r.g = r.b = M8(a + 2 * x) / 255.f; r.a = M8(a + 2 * x + 1) / 255.f; break;
  case 111: r.r = h2f(M16(a + 2 * x)); r.g = r.b = 1; break;
  case 112: r.r = h2f(M16(a + 4 * x)); r.g = h2f(M16(a + 4 * x + 2)); r.b = 1; break;
  case 113: r.r = h2f(M16(a + 8 * x)); r.g = h2f(M16(a + 8 * x + 2)); r.b = h2f(M16(a + 8 * x + 4)); r.a = h2f(M16(a + 8 * x + 6)); break;
  case 114: r.r = MF32(a + 4 * x); r.g = r.b = 1; break;
  case 115: r.r = MF32(a + 8 * x); r.g = MF32(a + 8 * x + 4); r.b = 1; break;
  case 116: r.r = MF32(a + 16 * x); r.g = MF32(a + 16 * x + 4); r.b = MF32(a + 16 * x + 8); r.a = MF32(a + 16 * x + 12); break;
  case 34: r.r = M16(a + 4 * x) / 65535.f; r.g = M16(a + 4 * x + 2) / 65535.f; r.b = 1; break;
  case 36: r.r = M16(a + 8 * x) / 65535.f; r.g = M16(a + 8 * x + 2) / 65535.f; r.b = M16(a + 8 * x + 4) / 65535.f; r.a = M16(a + 8 * x + 6) / 65535.f; break;
  case 31: case 35: { u32 v = M32(a + 4 * x); float c0 = (v & 1023) / 1023.f, c1 = ((v >> 10) & 1023) / 1023.f, c2 = ((v >> 20) & 1023) / 1023.f;
    if (L->fmt == 31) { r.r = c0; r.g = c1; r.b = c2; } else { r.b = c0; r.g = c1; r.r = c2; } r.a = (v >> 30) / 3.f; break; }
  case 75: case 77: case 71: r.r = r.g = r.b = r.a = MF32(a + 4 * x); break;
  case 80: r.r = r.g = r.b = r.a = M16(a + 2 * x) / 65535.f; break;
  default: r.r = 1; r.g = 0; r.b = 1; break;
  }
  return r;
}

static int addr_wrap(int i, int n, u32 mode) {
  switch (mode) {
  case 1: i %= n; return i < 0 ? i + n : i;                      /* WRAP */
  case 2: { int p = 2 * n; i %= p; if (i < 0) i += p; return i < n ? i : p - 1 - i; } /* MIRROR */
  case 4: return -1;                                              /* BORDER */
  default: return i < 0 ? 0 : i >= n ? n - 1 : i;                /* CLAMP, MIRRORONCE */
  }
}

typedef struct { Obj *t; int sampler; } SampCtx;
static void level_for(Obj *t, int face, u32 l, Level *L) {
  if (l >= t->levels) l = t->levels - 1;
  L->w = lvl_w(t, l); L->h = lvl_h(t, l); L->d = t->kind == K_VOLUME ? ((t->d >> l) ? t->d >> l : 1) : 1;
  L->fmt = t->fmt; L->pitch = t->pitch[l]; L->data = t->data[face][l]; L->cache = NULL;
  if (is_dxt(t->fmt)) {
    if (!t->cache[face][l]) t->cache[face][l] = dxt_decode(t->fmt, L->data, L->w, L->h, L->pitch);
    L->cache = t->cache[face][l];
  }
}
static void level_for_surface(Obj *s, Level *L) {
  L->w = s->w; L->h = s->h; L->d = 1; L->fmt = s->fmt; L->pitch = s->pitch[0]; L->data = s->data[0][0]; L->cache = NULL;
  if (is_dxt(s->fmt)) {
    if (!s->cache[0][0]) s->cache[0][0] = dxt_decode(s->fmt, L->data, L->w, L->h, L->pitch);
    L->cache = s->cache[0][0];
  }
}
static int guest_is_target(u32 g) {
  if (!D || !g) return 0;
  for (int i = 0; i < 4; i++) if (D->rt[i] == g) return 1;
  return D->ds == g;
}
static void invalidate_cache(Obj *o) {
  if (!o) return;
  for (int f = 0; f < 6; f++) o->dirty[f] = 0xffffffffu; /* GPU backend: re-upload */
  /* The texture walk uploads sampler textures. A level bound as a target is uploaded from host_surface
     on the target walk, so that walk has to run too when this object is one of the current targets. */
  if (D && (o->kind == K_SURFACE || o->kind == K_TEXTURE || o->kind == K_CUBE || o->kind == K_VOLUME)) {
    D->dirty |= D_TEX;
    if (o->kind == K_SURFACE) {
      if (guest_is_target(o->guest)) D->dirty |= D_RT;
    } else {
      for (int f = 0; f < 6 && !(D->dirty & D_RT); f++)
        for (int l = 0; l < MAXLVL; l++)
          if (guest_is_target(o->sub[f][l])) { D->dirty |= D_RT; break; }
    }
  }
  for (int f = 0; f < 6; f++) for (int l = 0; l < MAXLVL; l++) if (o->cache[f][l]) { free(o->cache[f][l]); o->cache[f][l] = NULL; }
}

static F4 sample_level(const Level *L, float u, float v, float w, int linear, const u32 *ss) {
  F4 border = {((ss[4] >> 16) & 255) / 255.f, ((ss[4] >> 8) & 255) / 255.f, (ss[4] & 255) / 255.f, (ss[4] >> 24) / 255.f};
  int z = 0;
  if (L->d > 1) { z = (int)floorf(w * L->d); z = addr_wrap(z, L->d, ss[3]); if (z < 0) z = 0; }
  if (!linear) {
    int x = addr_wrap((int)floorf(u * L->w), L->w, ss[1]), y = addr_wrap((int)floorf(v * L->h), L->h, ss[2]);
    if (x < 0 || y < 0) return border;
    return texel(L, x, y, z);
  }
  float fx = u * L->w - 0.5f, fy = v * L->h - 0.5f;
  int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
  float ax = fx - x0, ay = fy - y0;
  F4 t[4];
  for (int k = 0; k < 4; k++) {
    int x = addr_wrap(x0 + (k & 1), L->w, ss[1]), y = addr_wrap(y0 + (k >> 1), L->h, ss[2]);
    t[k] = (x < 0 || y < 0) ? border : texel(L, x, y, z);
  }
  F4 r;
  r.r = (t[0].r * (1 - ax) + t[1].r * ax) * (1 - ay) + (t[2].r * (1 - ax) + t[3].r * ax) * ay;
  r.g = (t[0].g * (1 - ax) + t[1].g * ax) * (1 - ay) + (t[2].g * (1 - ax) + t[3].g * ax) * ay;
  r.b = (t[0].b * (1 - ax) + t[1].b * ax) * (1 - ay) + (t[2].b * (1 - ax) + t[3].b * ax) * ay;
  r.a = (t[0].a * (1 - ax) + t[1].a * ax) * (1 - ay) + (t[2].a * (1 - ax) + t[3].a * ax) * ay;
  return r;
}

static F4 tex_sample(int s, const float *uv, float lod) {
  F4 black = {0, 0, 0, 1};
  if (s >= 20) return black;
  Obj *t = obj(D->tex[s]);
  if (!t) return black;
  const u32 *ss = D->ss[s];
  int linear = ss[5] >= 2 || ss[6] >= 2;
  u32 l = 0;
  if (ss[7] && lod > 0) { l = (u32)(lod + 0.5f); }
  l += ss[9]; /* MAXMIPLEVEL */
  Level L;
  if (t->kind == K_CUBE) {
    float x = uv[0], y = uv[1], z = uv[2], ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) { ma = ax; face = x > 0 ? 0 : 1; sc = x > 0 ? -z : z; tc = -y; }
    else if (ay >= az) { ma = ay; face = y > 0 ? 2 : 3; sc = x; tc = y > 0 ? z : -z; }
    else { ma = az; face = z > 0 ? 4 : 5; sc = z > 0 ? x : -x; tc = -y; }
    if (ma == 0) return black;
    level_for(t, face, l, &L);
    u32 cs[14]; memcpy(cs, ss, sizeof cs); cs[1] = cs[2] = 3;
    return sample_level(&L, (sc / ma + 1) * 0.5f, (tc / ma + 1) * 0.5f, 0, linear, cs);
  }
  if (t->kind == K_SURFACE) level_for_surface(t, &L); else level_for(t, 0, l, &L);
  return sample_level(&L, uv[0], uv[1], uv[2], linear, ss);
}

/* ---------------------------------------------------------------- interpreter */
typedef struct {
  float r[32][4], v[16][4], o[16][4], t[8][4], oc[4][4], c[256][4];
  int ci[16][4]; int cb[16];
  int a0[4], aL; int p[4];
  float misc[2][4];
  float depth; int kill;
} Regs;

static inline float *reg_ptr(Regs *R, int type, int num) {
  static float zero[4];
  switch (type) {
  case R_TEMP: return num < 32 ? R->r[num] : zero;
  case R_INPUT: return num < 16 ? R->v[num] : zero;
  case R_CONST: return (num >= 0 && num < 256) ? R->c[num] : zero;
  case 3: return num < 8 ? R->t[num] : zero; /* ps: t#, vs: a0 handled separately */
  case R_OUTPUT: return num < 16 ? R->o[num] : zero;
  case R_COLOROUT: return num < 4 ? R->oc[num] : zero;
  case R_MISC: return num < 2 ? R->misc[num] : zero;
  default: return zero;
  }
}
static void read_src(Regs *R, const Shader *sh, const Src *s, int off, float out[4]) {
  float tmp[4];
  const float *p;
  int num = s->num + off;
  if (s->rel) {
    int a;
    if (s->rel_type == R_LOOP) a = R->aL; else a = R->a0[s->rel_comp];
    num += a;
  }
  switch (s->type) {
  case R_CONSTINT: for (int i = 0; i < 4; i++) tmp[i] = num < 16 ? (float)R->ci[num][i] : 0; p = tmp; break;
  case R_CONSTBOOL: tmp[0] = tmp[1] = tmp[2] = tmp[3] = num < 16 ? (float)R->cb[num] : 0; p = tmp; break;
  case R_LOOP: tmp[0] = tmp[1] = tmp[2] = tmp[3] = (float)R->aL; p = tmp; break;
  case R_PRED: for (int i = 0; i < 4; i++) tmp[i] = (float)R->p[i]; p = tmp; break;
  case 3:
    if (!sh->ps) { for (int i = 0; i < 4; i++) tmp[i] = (float)R->a0[i]; p = tmp; }
    else p = reg_ptr(R, 3, num);
    break;
  default: p = reg_ptr(R, s->type, num);
  }
  for (int i = 0; i < 4; i++) out[i] = p[s->swz[i]];
  switch (s->mod) {
  case 1: for (int i = 0; i < 4; i++) out[i] = -out[i]; break;
  case 2: for (int i = 0; i < 4; i++) out[i] -= 0.5f; break;
  case 3: for (int i = 0; i < 4; i++) out[i] = -(out[i] - 0.5f); break;
  case 4: for (int i = 0; i < 4; i++) out[i] = 2 * out[i] - 1; break;
  case 5: for (int i = 0; i < 4; i++) out[i] = -(2 * out[i] - 1); break;
  case 6: for (int i = 0; i < 4; i++) out[i] = 1 - out[i]; break;
  case 7: for (int i = 0; i < 4; i++) out[i] *= 2; break;
  case 8: for (int i = 0; i < 4; i++) out[i] *= -2; break;
  case 9: { float z = out[2] ? out[2] : 1; out[0] /= z; out[1] /= z; break; }
  case 10: { float w = out[3] ? out[3] : 1; out[0] /= w; out[1] /= w; break; }
  case 11: for (int i = 0; i < 4; i++) out[i] = fabsf(out[i]); break;
  case 12: for (int i = 0; i < 4; i++) out[i] = -fabsf(out[i]); break;
  case 13: for (int i = 0; i < 4; i++) out[i] = !out[i]; break;
  }
}
static void write_dst(Regs *R, const Shader *sh, const Dst *d, int off, const float v[4]) {
  float *p;
  int num = d->num + off;
  switch (d->type) {
  case R_RASTOUT: p = num == 0 ? R->o[15] : num == 1 ? R->o[14] : R->o[13]; break; /* oPos oFog oPts */
  case R_ATTROUT: p = num == 0 ? R->o[12] : R->o[11]; break;                     /* oD0 oD1 */
  case R_OUTPUT: p = sh->ps ? NULL : (sh->major >= 3 ? R->o[num & 15] : R->o[num & 7]); break;
  case R_DEPTHOUT: R->depth = v[0]; return;
  case 3:
    if (!sh->ps) { for (int i = 0; i < 4; i++) if (d->mask & (1 << i)) R->a0[i] = (int)floorf(v[i] + (sh->major >= 2 ? 0.5f : 0.0f)); return; }
    p = R->t[num & 7]; break;
  case R_PRED: for (int i = 0; i < 4; i++) if (d->mask & (1 << i)) R->p[i] = v[i] != 0; return;
  default: p = reg_ptr(R, d->type, num);
  }
  if (!p) return;
  for (int i = 0; i < 4; i++)
    if (d->mask & (1 << i)) {
      float x = v[i];
      if (d->shift) x = ldexpf(x, d->shift);
      if (d->sat) x = x < 0 ? 0 : x > 1 ? 1 : x;
      p[i] = x;
    }
}
static int cmpc(int ctrl, float a, float b) {
  switch (ctrl & 7) { case 1: return a > b; case 2: return a == b; case 3: return a >= b; case 4: return a < b; case 5: return a != b; case 6: return a <= b; }
  return 0;
}
static float dp(const float *a, const float *b, int n) { float s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }

/* scalar sources: a replicate swizzle picks the component; an identity swizzle (vs_1_x) means .w */
#define SCALAR(i, v) ((in->s[i].swz[0] == 0 && in->s[i].swz[1] == 1 && in->s[i].swz[2] == 2 && in->s[i].swz[3] == 3) ? (v)[3] : (v)[0])
static void run_shader(const Shader *sh, Regs *R) {
  int lstack[8][3], lsp = 0; /* loop: count remaining, saved aL, step */
  int cstack[16], csp = 0;
  for (int pc = 0; pc < sh->n; pc++) {
    const Ins *in = &sh->ins[pc];
    float a[4] = {0}, b[4] = {0}, c3[4] = {0}, r[4] = {0};
    if (in->op == 81 || in->op == 48 || in->op == 47) {
      if (in->op == 81) memcpy(R->c[in->d.num & 255], in->imm, 16);
      else if (in->op == 48) for (int i = 0; i < 4; i++) R->ci[in->d.num & 15][i] = (int)in->imm[i];
      else R->cb[in->d.num & 15] = in->imm[0] != 0;
      continue;
    }
    if (in->pred) { int pv = R->p[in->pred_swz]; if (in->pred_neg) pv = !pv; if (!pv) continue; }
    if (in->nsrc > 0 && in->op != 66 && in->op != 95 && in->op != 93) read_src(R, sh, &in->s[0], 0, a);
    if (in->nsrc > 1 && !(in->op >= 20 && in->op <= 24)) read_src(R, sh, &in->s[1], 0, b);
    if (in->nsrc > 2) read_src(R, sh, &in->s[2], 0, c3);
    switch (in->op) {
    case 0: continue;
    case 1: memcpy(r, a, 16); break;
    case 46: memcpy(r, a, 16); break;
    case 2: for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; break;
    case 3: for (int i = 0; i < 4; i++) r[i] = a[i] - b[i]; break;
    case 4: for (int i = 0; i < 4; i++) r[i] = a[i] * b[i] + c3[i]; break;
    case 5: for (int i = 0; i < 4; i++) r[i] = a[i] * b[i]; break;
    case 6: { float v = SCALAR(0, a); r[0] = r[1] = r[2] = r[3] = v == 0 ? INFINITY : 1.0f / v; break; }
    case 7: { float v = fabsf(SCALAR(0, a)); r[0] = r[1] = r[2] = r[3] = v == 0 ? INFINITY : 1.0f / sqrtf(v); break; }
    case 8: r[0] = r[1] = r[2] = r[3] = dp(a, b, 3); break;
    case 9: r[0] = r[1] = r[2] = r[3] = dp(a, b, 4); break;
    case 10: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; break;
    case 11: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? a[i] : b[i]; break;
    case 12: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.f : 0.f; break;
    case 13: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.f : 0.f; break;
    case 14: case 78: { float v = SCALAR(0, a); r[0] = r[1] = r[2] = r[3] = exp2f(v); break; }
    case 15: case 79: { float v = fabsf(SCALAR(0, a)); r[0] = r[1] = r[2] = r[3] = v == 0 ? -INFINITY : log2f(v); break; }
    case 16: { r[0] = 1; r[3] = 1; r[1] = a[0] > 0 ? a[0] : 0; float pw = a[3] < -127.99f ? -127.99f : a[3] > 127.99f ? 127.99f : a[3];
      r[2] = (a[0] > 0 && a[1] > 0) ? powf(a[1], pw) : 0; break; }
    case 17: r[0] = 1; r[1] = a[1] * b[1]; r[2] = a[2]; r[3] = b[3]; break;
    case 18: for (int i = 0; i < 4; i++) r[i] = a[i] * (b[i] - c3[i]) + c3[i]; break;
    case 19: for (int i = 0; i < 4; i++) r[i] = a[i] - floorf(a[i]); break;
    case 20: case 21: case 22: case 23: case 24: {
      int rows = in->op == 20 ? 4 : in->op == 21 ? 3 : in->op == 22 ? 4 : in->op == 23 ? 3 : 2;
      int cols = (in->op == 20 || in->op == 21) ? 4 : 3;
      for (int k = 0; k < rows; k++) { float m[4]; read_src(R, sh, &in->s[1], k, m); r[k] = dp(a, m, cols); }
      Dst d = in->d; d.mask &= (1 << rows) - 1;
      write_dst(R, sh, &d, 0, r);
      continue;
    }
    case 25: case 26: {
      int lab = in->s[in->op == 25 ? 0 : 0].num;
      if (in->op == 26) { float bv[4]; read_src(R, sh, &in->s[1], 0, bv); if (!bv[0]) continue; }
      if (lab < 2048 && sh->labels[lab] >= 0 && csp < 16) { cstack[csp++] = pc; pc = sh->labels[lab] - 1; }
      continue;
    }
    case 28: if (csp) { pc = cstack[--csp]; continue; } return;
    case 27: { /* loop aL, i# */
      float iv[4]; read_src(R, sh, &in->s[1], 0, iv);
      int cnt = (int)iv[0];
      if (cnt <= 0 || lsp >= 8) { pc = in->jump; continue; }
      lstack[lsp][0] = cnt; lstack[lsp][1] = R->aL; lstack[lsp][2] = (int)iv[2]; lsp++;
      R->aL = (int)iv[1];
      continue;
    }
    case 38: {
      float iv[4]; read_src(R, sh, &in->s[0], 0, iv);
      int cnt = (int)iv[0];
      if (cnt <= 0 || lsp >= 8) { pc = in->jump; continue; }
      lstack[lsp][0] = cnt; lstack[lsp][1] = R->aL; lstack[lsp][2] = 0; lsp++;
      continue;
    }
    case 29: case 39:
      if (lsp && --lstack[lsp - 1][0] > 0) { R->aL += lstack[lsp - 1][2]; pc = in->jump; }
      else if (lsp) { lsp--; R->aL = lstack[lsp][1]; }
      continue;
    case 44: case 45: case 96: {
      int brk = 1;
      if (in->op == 45) brk = cmpc(in->ctrl, a[0], b[0]);
      if (in->op == 96) { float pv[4]; read_src(R, sh, &in->s[0], 0, pv); brk = pv[0] != 0; }
      if (!brk) continue;
      /* jump to the matching endloop/endrep, then leave the loop */
      int depth = 0, q = pc + 1;
      for (; q < sh->n; q++) {
        int o = sh->ins[q].op;
        if (o == 27 || o == 38) depth++;
        else if (o == 29 || o == 39) { if (!depth) break; depth--; }
      }
      if (lsp) { lsp--; R->aL = lstack[lsp][1]; }
      pc = q;
      continue;
    }
    case 40: if (!a[0]) pc = in->jump; continue;
    case 41: if (!cmpc(in->ctrl, a[0], b[0])) pc = in->jump; continue;
    case 42: pc = in->jump; continue;
    case 43: continue;
    case 32: { float x = SCALAR(0, a); float y = SCALAR(1, b); r[0] = r[1] = r[2] = r[3] = powf(fabsf(x), y); break; }
    case 33: r[0] = a[1] * b[2] - a[2] * b[1]; r[1] = a[2] * b[0] - a[0] * b[2]; r[2] = a[0] * b[1] - a[1] * b[0]; break;
    case 34: for (int i = 0; i < 4; i++) r[i] = a[i] > 0 ? 1.f : a[i] < 0 ? -1.f : 0.f; break;
    case 35: for (int i = 0; i < 4; i++) r[i] = fabsf(a[i]); break;
    case 36: { float l = sqrtf(dp(a, a, 3)); float s = l ? 1 / l : 0; for (int i = 0; i < 4; i++) r[i] = a[i] * s; break; }
    case 37: { float x = SCALAR(0, a); r[0] = cosf(x); r[1] = sinf(x); break; }
    case 80: for (int i = 0; i < 4; i++) r[i] = a[i] > 0.5f ? b[i] : c3[i]; break;
    case 88: for (int i = 0; i < 4; i++) r[i] = a[i] >= 0 ? b[i] : c3[i]; break;
    case 90: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + c3[0]; break;
    case 91: case 92: break;
    case 94: for (int i = 0; i < 4; i++) r[i] = cmpc(in->ctrl, a[i], b[i]); break;
    case 65: { float k[4]; read_src(R, sh, &in->s[0], 0, k); if (k[0] < 0 || k[1] < 0 || k[2] < 0 || (sh->major >= 2 && k[3] < 0)) R->kill = 1; continue; }
    case 66: case 95: case 93: {
      if (sh->major < 2) { /* ps_1_x tex t#: sample with texcoord t# */
        int n = in->d.num & 7; F4 s = tex_sample(n, R->t[n], 0);
        R->t[n][0] = s.r; R->t[n][1] = s.g; R->t[n][2] = s.b; R->t[n][3] = s.a; continue;
      }
      float uv[4]; read_src(R, sh, &in->s[0], 0, uv);
      if (in->op == 66 && (in->ctrl & 1) && uv[3]) { uv[0] /= uv[3]; uv[1] /= uv[3]; uv[2] /= uv[3]; }
      float lod = in->op == 95 ? uv[3] : 0;
      F4 s = tex_sample(in->s[1].num, uv, lod);
      float v[4] = {s.r, s.g, s.b, s.a}, sw[4];
      for (int i = 0; i < 4; i++) sw[i] = v[in->s[1].swz[i]];
      memcpy(r, sw, 16);
      break;
    }
    case 64: { memcpy(r, R->t[in->d.num & 7], 16); break; }
    default:
      if (!((Shader *)sh)->warned++) rt_log("swr: shader opcode %u not implemented (%s %d.%d)", in->op, sh->ps ? "ps" : "vs", sh->major, sh->minor);
      continue;
    }
    if (in->has_dst) write_dst(R, sh, &in->d, 0, r);
  }
}

/* ---------------------------------------------------------------- vertex fetch */
#define NVARY 11 /* COLOR0 COLOR1 TEXCOORD0-7 FOG */
typedef struct { float pos[4]; float vary[NVARY][4]; } Vtx;
static int vary_slot(int usage, int index) {
  if (usage == 10) return index < 2 ? index : -1;
  if (usage == 5) return index < 8 ? 2 + index : -1;
  if (usage == 11) return 10;
  return -1;
}
static void fetch_elem(u32 a, int type, float out[4]) {
  out[0] = out[1] = out[2] = 0; out[3] = 1;
  switch (type) {
  case 0: out[0] = MF32(a); break;
  case 1: out[0] = MF32(a); out[1] = MF32(a + 4); break;
  case 2: out[0] = MF32(a); out[1] = MF32(a + 4); out[2] = MF32(a + 8); break;
  case 3: out[0] = MF32(a); out[1] = MF32(a + 4); out[2] = MF32(a + 8); out[3] = MF32(a + 12); break;
  case 4: { u32 v = M32(a); out[0] = ((v >> 16) & 255) / 255.f; out[1] = ((v >> 8) & 255) / 255.f; out[2] = (v & 255) / 255.f; out[3] = (v >> 24) / 255.f; break; }
  case 5: for (int i = 0; i < 4; i++) out[i] = M8(a + i); break;
  case 6: out[0] = (s16)M16(a); out[1] = (s16)M16(a + 2); break;
  case 7: for (int i = 0; i < 4; i++) out[i] = (s16)M16(a + 2 * i); break;
  case 8: for (int i = 0; i < 4; i++) out[i] = M8(a + i) / 255.f; break;
  case 9: out[0] = (s16)M16(a) / 32767.f; out[1] = (s16)M16(a + 2) / 32767.f; break;
  case 10: for (int i = 0; i < 4; i++) out[i] = (s16)M16(a + 2 * i) / 32767.f; break;
  case 11: out[0] = M16(a) / 65535.f; out[1] = M16(a + 2) / 65535.f; break;
  case 12: for (int i = 0; i < 4; i++) out[i] = M16(a + 2 * i) / 65535.f; break;
  case 15: out[0] = h2f(M16(a)); out[1] = h2f(M16(a + 2)); break;
  case 16: for (int i = 0; i < 4; i++) out[i] = h2f(M16(a + 2 * i)); break;
  default: break;
  }
}
/* FVF -> declaration elements (stream 0) */
typedef struct { u16 stream, off; u8 type, usage, index; } Elem;
static int fvf_elems(u32 fvf, Elem *e, u32 *stride) {
  int n = 0; u32 off = 0;
  u32 pos = fvf & 0x400e;
  if (pos == 2) { e[n++] = (Elem){0, off, 2, 0, 0}; off += 12; }
  else if (pos == 4) { e[n++] = (Elem){0, off, 3, 9, 0}; off += 16; }
  else if (pos >= 6 && pos <= 0xe) { e[n++] = (Elem){0, off, 2, 0, 0}; off += 12 + ((pos - 4) / 2) * 4; }
  if (fvf & 0x10) { e[n++] = (Elem){0, off, 2, 3, 0}; off += 12; }
  if (fvf & 0x20) off += 4;
  if (fvf & 0x40) { e[n++] = (Elem){0, off, 4, 10, 0}; off += 4; }
  if (fvf & 0x80) { e[n++] = (Elem){0, off, 4, 10, 1}; off += 4; }
  int ntex = (fvf >> 8) & 15;
  for (int i = 0; i < ntex; i++) {
    int f = (fvf >> (16 + 2 * i)) & 3;
    int comps = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
    e[n++] = (Elem){0, off, (u8)(comps - 1), 5, (u8)i}; off += 4 * comps;
  }
  *stride = off;
  return n;
}

/* ---------------------------------------------------------------- render targets */
static void px_load(Obj *s, int x, int y, float c[4]) {
  Level L; level_for_surface(s, &L);
  F4 v = texel(&L, x, y, 0);
  c[0] = v.r; c[1] = v.g; c[2] = v.b; c[3] = v.a;
}
static inline u32 to8(float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (u32)(v * 255.f + 0.5f); }
static void px_store(Obj *s, int x, int y, const float c[4], u32 wmask) {
  u32 a = s->data[0][0] + (u32)y * s->pitch[0];
  switch (s->fmt) {
  case 21: case 22: {
    u32 old = M32(a + 4 * x), v = old;
    if (wmask & 1) v = (v & ~0xff0000u) | (to8(c[0]) << 16);
    if (wmask & 2) v = (v & ~0xff00u) | (to8(c[1]) << 8);
    if (wmask & 4) v = (v & ~0xffu) | to8(c[2]);
    if (wmask & 8) v = (v & ~0xff000000u) | (to8(c[3]) << 24);
    M32(a + 4 * x) = v; break;
  }
  case 113: for (int i = 0; i < 4; i++) if (wmask & (1 << i)) M16(a + 8 * x + 2 * i) = f2h(c[i]); break;
  case 116: for (int i = 0; i < 4; i++) if (wmask & (1 << i)) MF32(a + 16 * x + 4 * i) = c[i]; break;
  case 114: if (wmask & 1) MF32(a + 4 * x) = c[0]; break;
  case 111: if (wmask & 1) M16(a + 2 * x) = f2h(c[0]); break;
  case 112: if (wmask & 1) M16(a + 4 * x) = f2h(c[0]); if (wmask & 2) M16(a + 4 * x + 2) = f2h(c[1]); break;
  case 115: if (wmask & 1) MF32(a + 8 * x) = c[0]; if (wmask & 2) MF32(a + 8 * x + 4) = c[1]; break;
  case 23: M16(a + 2 * x) = (to8(c[0]) >> 3 << 11) | (to8(c[1]) >> 2 << 5) | (to8(c[2]) >> 3); break;
  case 34: M16(a + 4 * x) = (u16)(to8(c[0]) * 257); M16(a + 4 * x + 2) = (u16)(to8(c[1]) * 257); break;
  case 36: for (int i = 0; i < 4; i++) { float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i]; M16(a + 8 * x + 2 * i) = (u16)(v * 65535); } break;
  case 31: case 35: { u32 r = (u32)(fminf(fmaxf(c[0], 0), 1) * 1023), g = (u32)(fminf(fmaxf(c[1], 0), 1) * 1023), b = (u32)(fminf(fmaxf(c[2], 0), 1) * 1023), al = (u32)(fminf(fmaxf(c[3], 0), 1) * 3);
    M32(a + 4 * x) = s->fmt == 31 ? (r | (g << 10) | (b << 20) | (al << 30)) : (b | (g << 10) | (r << 20) | (al << 30)); break; }
  case 28: M8(a + x) = to8(c[3]); break;
  case 50: M8(a + x) = to8(c[0]); break;
  default: break;
  }
}
static float *zptr(Obj *z, int x, int y) { return (float *)HP(z->data[0][0] + (u32)y * z->pitch[0] + 4 * x); }
static int zfmt_float(Obj *z) { return z && bpp(z->fmt) == 4; }

static float blend_factor(u32 f, const float *src, const float *dst, int ch, const float *bf) {
  switch (f) {
  case 1: return 0; case 2: return 1;
  case 3: return src[ch]; case 4: return 1 - src[ch];
  case 5: return src[3]; case 6: return 1 - src[3];
  case 7: return dst[3]; case 8: return 1 - dst[3];
  case 9: return dst[ch]; case 10: return 1 - dst[ch];
  case 11: { if (ch == 3) return 1; float a = src[3], b = 1 - dst[3]; return a < b ? a : b; }
  case 14: return bf[ch]; case 15: return 1 - bf[ch];
  default: return 1;
  }
}
static float blend_op(u32 op, float s, float d) {
  switch (op) { case 2: return s - d; case 3: return d - s; case 4: return s < d ? s : d; case 5: return s > d ? s : d; default: return s + d; }
}
static int cmp_func(u32 f, float a, float b) {
  switch (f) { case 1: return 0; case 2: return a < b; case 3: return a == b; case 4: return a <= b; case 5: return a > b; case 6: return a != b; case 7: return a >= b; default: return 1; }
}

/* ---------------------------------------------------------------- draw */
static Shader *get_shader(u32 g) {
  Obj *o = obj(g);
  if (!o) return NULL;
  if (!o->sh) o->sh = shader_decode(o->blob, o->blob_size);
  return o->sh;
}
static u64 g_swr_pixels;
static int g_render_on = 1;

typedef struct {
  Shader *vs, *ps;
  Elem el[32]; int nel;
  u32 stream_base[16], stream_stride[16];
  Regs *VR, *PR;
  int vs_in_map[16]; /* v# -> element */
  int ps_in_slot[16];
} DrawCtx;

static void setup_vs_consts(Regs *R, const Shader *vs) {
  memcpy(R->c, D->vsf, sizeof R->c);
  memcpy(R->ci, D->vsi, sizeof R->ci);
  for (int i = 0; i < 16; i++) R->cb[i] = D->vsb[i] != 0;
}
static void setup_ps_consts(Regs *R) {
  memcpy(R->c, D->psf, 224 * 16);
  memcpy(R->ci, D->psi, sizeof R->ci);
  for (int i = 0; i < 16; i++) R->cb[i] = D->psb[i] != 0;
}

static void ff_transform(const float in[4], float out[4], int pretransformed) {
  if (pretransformed) { memcpy(out, in, 16); return; }
  float t[4], u[4];
  const float *W = D->transform[256], *V = D->transform[2], *P = D->transform[3];
  float v4[4] = {in[0], in[1], in[2], 1};
  for (int j = 0; j < 4; j++) t[j] = v4[0] * W[j] + v4[1] * W[4 + j] + v4[2] * W[8 + j] + v4[3] * W[12 + j];
  for (int j = 0; j < 4; j++) u[j] = t[0] * V[j] + t[1] * V[4 + j] + t[2] * V[8 + j] + t[3] * V[12 + j];
  for (int j = 0; j < 4; j++) out[j] = u[0] * P[j] + u[1] * P[4 + j] + u[2] * P[8 + j] + u[3] * P[12 + j];
}

static void process_vertex(DrawCtx *dc, u32 index, Vtx *out) {
  memset(out, 0, sizeof *out);
  out->vary[0][0] = out->vary[0][1] = out->vary[0][2] = out->vary[0][3] = 1;
  if (!dc->vs) {
    /* fixed function */
    float pos[4] = {0, 0, 0, 1}; int pret = 0;
    for (int e = 0; e < dc->nel; e++) {
      Elem *el = &dc->el[e];
      float v[4]; fetch_elem(dc->stream_base[el->stream] + index * dc->stream_stride[el->stream] + el->off, el->type, v);
      if (el->usage == 0) memcpy(pos, v, 16);
      else if (el->usage == 9) { memcpy(pos, v, 16); pret = 1; }
      else { int s = vary_slot(el->usage, el->index); if (s >= 0) memcpy(out->vary[s], v, 16); }
    }
    if (pret) {
      /* XYZRHW: screen space -> clip space for the common path */
      const u32 *vp = D->viewport;
      float w = pos[3] ? 1.0f / pos[3] : 1;
      out->pos[0] = ((pos[0] - vp[0]) / (vp[2] * 0.5f) - 1) * w;
      out->pos[1] = (1 - (pos[1] - vp[1]) / (vp[3] * 0.5f)) * w;
      out->pos[2] = pos[2] * w; out->pos[3] = w;
    } else ff_transform(pos, out->pos, 0);
    return;
  }
  Regs *R = dc->VR;
  memset(R->r, 0, sizeof R->r); memset(R->o, 0, sizeof R->o);
  for (int v = 0; v < 16; v++) {
    int e = dc->vs_in_map[v];
    if (e < 0) continue;
    Elem *el = &dc->el[e];
    fetch_elem(dc->stream_base[el->stream] + index * dc->stream_stride[el->stream] + el->off, el->type, R->v[v]);
  }
  run_shader(dc->vs, R);
  const Shader *vs = dc->vs;
  if (vs->major >= 3) {
    int got_pos = 0;
    for (int o = 0; o < 12; o++) {
      if (!vs->out_valid[o]) continue;
      if (vs->out_usage[o] == 0 || vs->out_usage[o] == 9) { memcpy(out->pos, R->o[o], 16); got_pos = 1; continue; }
      int s = vary_slot(vs->out_usage[o], vs->out_index[o]);
      if (s >= 0) memcpy(out->vary[s], R->o[o], 16);
    }
    if (!got_pos) memcpy(out->pos, R->o[0], 16);
  } else {
    memcpy(out->pos, R->o[15], 16);
    memcpy(out->vary[0], R->o[12], 16);
    memcpy(out->vary[1], R->o[11], 16);
    for (int t = 0; t < 8; t++) memcpy(out->vary[2 + t], R->o[t], 16);
    out->vary[10][0] = R->o[14][0];
  }
}

static int ff_warned;
static void shade_pixel(DrawCtx *dc, int x, int y, const float vary[NVARY][4], float z, int front, Obj **rts, Obj *zs) {
  float col[4][4];
  int nout = 1;
  float depth = z;
  if (dc->ps) {
    Regs *R = dc->PR;
    const Shader *ps = dc->ps;
    R->kill = 0; R->depth = z;
    memset(R->oc, 0, sizeof R->oc);
    if (ps->major >= 3) {
      for (int v = 0; v < 10; v++) {
        if (!ps->in_valid[v]) continue;
        int s = dc->ps_in_slot[v];
        if (s >= 0) memcpy(R->v[v], vary[s], 16); else memset(R->v[v], 0, 16);
      }
      R->misc[0][0] = x; R->misc[0][1] = y; R->misc[1][0] = front ? 1.f : -1.f;
    } else {
      memcpy(R->v[0], vary[0], 16); memcpy(R->v[1], vary[1], 16);
      for (int t = 0; t < 8; t++) memcpy(R->t[t], vary[2 + t], 16);
      if (ps->major == 1) { for (int t = 0; t < 4; t++) memcpy(R->t[t], vary[2 + t], 16); }
    }
    run_shader(ps, R);
    if (R->kill) return;
    if (ps->major == 1) memcpy(col[0], R->r[0], 16);
    else for (int i = 0; i < 4; i++) memcpy(col[i], R->oc[i], 16);
    nout = 4;
    depth = R->depth;
  } else {
    /* fixed function: stage 0 = texture * diffuse (or diffuse) */
    memcpy(col[0], vary[0], 16);
    if (D->tex[0] && D->tss[0][1] != 1) {
      F4 t = tex_sample(0, vary[2], 0);
      u32 op = D->tss[0][1];
      if (op == 2) { col[0][0] = t.r; col[0][1] = t.g; col[0][2] = t.b; }
      else { col[0][0] *= t.r; col[0][1] *= t.g; col[0][2] *= t.b; }
      u32 aop = D->tss[0][4];
      if (aop == 2) col[0][3] = t.a; else if (aop != 3) col[0][3] *= t.a;
    }
    if (!ff_warned++) rt_log("swr: fixed-function draw (approximate stage-0 texturing)");
  }
  /* alpha test */
  if (D->rs[15]) {
    float ref = (D->rs[24] & 255) / 255.f;
    if (!cmp_func(D->rs[25], col[0][3], ref)) return;
  }
  /* depth test */
  if (zs && D->rs[7]) {
    float *zp = zptr(zs, x, y);
    if (!cmp_func(D->rs[23], depth, *zp)) return;
    if (D->rs[14]) *zp = depth;
  }
  float bf[4] = {((D->rs[193] >> 16) & 255) / 255.f, ((D->rs[193] >> 8) & 255) / 255.f, (D->rs[193] & 255) / 255.f, (D->rs[193] >> 24) / 255.f};
  for (int i = 0; i < nout && i < 4; i++) {
    Obj *rt = rts[i];
    if (!rt) continue;
    u32 wmask = i == 0 ? D->rs[168] : i == 1 ? D->rs[190] : i == 2 ? D->rs[191] : D->rs[192];
    float *s = col[i];
    if (D->rs[27]) {
      float d[4]; px_load(rt, x, y, d);
      float o[4];
      int sep = D->rs[206];
      for (int ch = 0; ch < 4; ch++) {
        u32 sf = (ch == 3 && sep) ? D->rs[207] : D->rs[19], df = (ch == 3 && sep) ? D->rs[208] : D->rs[20];
        u32 op = (ch == 3 && sep) ? D->rs[209] : D->rs[171];
        o[ch] = blend_op(op, s[ch] * blend_factor(sf, s, d, ch, bf), d[ch] * blend_factor(df, s, d, ch, bf));
      }
      px_store(rt, x, y, o, wmask);
    } else px_store(rt, x, y, s, wmask);
  }
  g_swr_pixels++;
}

typedef struct { float x, y, z, iw; float vary[NVARY][4]; } SV; /* screen vertex: vary pre-divided by w */

static void raster_tri(DrawCtx *dc, const SV *a, const SV *b, const SV *c, Obj **rts, Obj *zs, int clip[4]) {
  float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
  if (area == 0) return;
  int cw = area > 0;
  u32 cull = D->rs[22];
  if ((cull == 2 && cw) || (cull == 3 && !cw)) return;
  int front = cull == 3 ? cw : !cw;
  float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
  float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
  int x0 = (int)ceilf(minx), x1 = (int)floorf(maxx), y0 = (int)ceilf(miny), y1 = (int)floorf(maxy);
  if (x0 < clip[0]) x0 = clip[0]; if (y0 < clip[1]) y0 = clip[1];
  if (x1 >= clip[2]) x1 = clip[2] - 1; if (y1 >= clip[3]) y1 = clip[3] - 1;
  if (x0 > x1 || y0 > y1) return;
  float inv = 1.0f / area;
  float vary[NVARY][4];
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      float px = x, py = y;
      float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) * inv;
      float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) * inv;
      float w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) {
        /* include exact edges only on one side to avoid double coverage */
        if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
      }
      float iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
      float rw = iw ? 1.0f / iw : 0;
      for (int s = 0; s < NVARY; s++)
        for (int k = 0; k < 4; k++) vary[s][k] = (w0 * a->vary[s][k] + w1 * b->vary[s][k] + w2 * c->vary[s][k]) * rw;
      float z = w0 * a->z + w1 * b->z + w2 * c->z;
      shade_pixel(dc, x, y, (const float(*)[4])vary, z, front, rts, zs);
    }
}

static void to_screen(const Vtx *v, SV *s) {
  const u32 *vp = D->viewport;
  float minz, maxz; memcpy(&minz, &vp[4], 4); memcpy(&maxz, &vp[5], 4);
  float iw = 1.0f / v->pos[3];
  s->x = vp[0] + (1 + v->pos[0] * iw) * vp[2] * 0.5f;
  s->y = vp[1] + (1 - v->pos[1] * iw) * vp[3] * 0.5f;
  s->z = minz + v->pos[2] * iw * (maxz - minz);
  s->iw = iw;
  for (int i = 0; i < NVARY; i++) for (int k = 0; k < 4; k++) s->vary[i][k] = v->vary[i][k] * iw;
}
static void lerp_vtx(const Vtx *a, const Vtx *b, float t, Vtx *o) {
  for (int k = 0; k < 4; k++) o->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t;
  for (int i = 0; i < NVARY; i++) for (int k = 0; k < 4; k++) o->vary[i][k] = a->vary[i][k] + (b->vary[i][k] - a->vary[i][k]) * t;
}
static void draw_triangle(DrawCtx *dc, const Vtx *v0, const Vtx *v1, const Vtx *v2, Obj **rts, Obj *zs, int clip[4]) {
  /* clip against z >= 0 (D3D near plane) and w > eps */
  Vtx in[8], out[8];
  int n = 3;
  in[0] = *v0; in[1] = *v1; in[2] = *v2;
  for (int plane = 0; plane < 2; plane++) {
    int m = 0;
    for (int i = 0; i < n; i++) {
      const Vtx *p = &in[i], *q = &in[(i + 1) % n];
      float dp_ = plane == 0 ? p->pos[2] : p->pos[3] - 1e-5f, dq = plane == 0 ? q->pos[2] : q->pos[3] - 1e-5f;
      if (dp_ >= 0) out[m++] = *p;
      if ((dp_ >= 0) != (dq >= 0) && m < 8) lerp_vtx(p, q, dp_ / (dp_ - dq), &out[m++]);
    }
    n = m;
    if (n < 3) return;
    memcpy(in, out, sizeof(Vtx) * n);
  }
  SV s[8];
  for (int i = 0; i < n; i++) to_screen(&in[i], &s[i]);
  for (int i = 1; i + 1 < n; i++) raster_tri(dc, &s[0], &s[i], &s[i + 1], rts, zs, clip);
}

static void swr_draw(u32 type, u32 base_vertex, u32 min_index, u32 num_vertices, u32 start, u32 prims,
                     u32 idx_data, u32 idx_fmt, u32 up_data, u32 up_stride) {
  if (!g_render_on || type < 4 || !prims) return;
  DrawCtx dc; memset(&dc, 0, sizeof dc);
  dc.vs = D->vs ? get_shader(D->vs) : NULL;
  dc.ps = D->ps ? get_shader(D->ps) : NULL;
  /* vertex layout */
  Obj *decl = obj(D->decl);
  if (decl && (dc.vs || !D->fvf)) {
    for (u32 k = 0; k + 1 < decl->blob_size / 8 && dc.nel < 32; k++) {
      u32 e = decl->blob + 8 * k;
      dc.el[dc.nel++] = (Elem){M16(e), M16(e + 2), M8(e + 4), M8(e + 6), M8(e + 7)};
    }
  } else {
    u32 stride; dc.nel = fvf_elems(D->fvf, dc.el, &stride);
  }
  for (int s = 0; s < 16; s++) {
    Obj *vb = obj(D->stream[s]);
    dc.stream_base[s] = vb ? vb->data[0][0] + D->stream_off[s] : 0;
    dc.stream_stride[s] = D->stream_stride[s];
  }
  if (up_data) { dc.stream_base[0] = up_data; dc.stream_stride[0] = up_stride; }
  static __thread Regs *VR, *PR;
  if (!VR) { VR = calloc(1, sizeof *VR); PR = calloc(1, sizeof *PR); }
  dc.VR = VR; dc.PR = PR;
  if (dc.vs) {
    setup_vs_consts(VR, dc.vs);
    for (int v = 0; v < 16; v++) {
      dc.vs_in_map[v] = -1;
      if (!dc.vs->in_valid[v]) continue;
      for (int e = 0; e < dc.nel; e++)
        if (dc.el[e].usage == dc.vs->in_usage[v] && dc.el[e].index == dc.vs->in_index[v]) { dc.vs_in_map[v] = e; break; }
      if (dc.vs_in_map[v] < 0 && dc.vs->in_usage[v] == 0)
        for (int e = 0; e < dc.nel; e++) if (dc.el[e].usage == 9) { dc.vs_in_map[v] = e; break; }
    }
    /* run the shader's def instructions once into the constant file */
  }
  if (dc.ps) {
    setup_ps_consts(PR);
    for (int v = 0; v < 16; v++) dc.ps_in_slot[v] = dc.ps->in_valid[v] ? vary_slot(dc.ps->in_usage[v], dc.ps->in_index[v]) : -1;
  }
  /* targets */
  Obj *rts[4];
  for (int i = 0; i < 4; i++) rts[i] = obj(D->rt[i]);
  if (!rts[0]) return;
  Obj *zs = D->rs[7] ? obj(D->ds) : NULL;
  if (zs && !zfmt_float(zs)) zs = NULL;
  int clip[4] = {(int)D->viewport[0], (int)D->viewport[1], (int)(D->viewport[0] + D->viewport[2]), (int)(D->viewport[1] + D->viewport[3])};
  if (clip[2] > (int)rts[0]->w) clip[2] = rts[0]->w;
  if (clip[3] > (int)rts[0]->h) clip[3] = rts[0]->h;
  if (D->rs[174]) {
    if (clip[0] < (int)D->scissor[0]) clip[0] = D->scissor[0];
    if (clip[1] < (int)D->scissor[1]) clip[1] = D->scissor[1];
    if (clip[2] > (int)D->scissor[2]) clip[2] = D->scissor[2];
    if (clip[3] > (int)D->scissor[3]) clip[3] = D->scissor[3];
  }
  /* vertices */
  u32 nidx = type == 4 ? prims * 3 : prims + 2;
  u32 vcount = num_vertices ? num_vertices : nidx + start;
  Vtx *cache = malloc(sizeof(Vtx) * (vcount + 1));
  u8 *done = calloc(vcount + 1, 1);
  Obj *ib = obj(D->indices);
  u32 ibase = idx_data ? idx_data : ib ? ib->data[0][0] : 0;
  int i32 = idx_data ? idx_fmt == 102 : (ib && ib->fmt == 102);
  int indexed = idx_data || (num_vertices && ib);
  Vtx tmpv[3];
  const Vtx *tri[3];
  for (u32 p = 0; p < prims; p++) {
    for (int k = 0; k < 3; k++) {
      u32 seq;
      if (type == 4) seq = p * 3 + k;
      else if (type == 5) seq = (p & 1) ? p + (k == 0 ? 1 : k == 1 ? 0 : 2) : p + k;
      else seq = k == 0 ? 0 : p + k;
      u32 vi;
      if (indexed) {
        u32 ii = start + seq;
        vi = i32 ? M32(ibase + 4 * ii) : M16(ibase + 2 * ii);
        vi += base_vertex;
        u32 slot = vi - min_index - base_vertex;
        if (slot < vcount) {
          if (!done[slot]) { process_vertex(&dc, vi, &cache[slot]); done[slot] = 1; }
          tri[k] = &cache[slot];
        } else { process_vertex(&dc, vi, &tmpv[k]); tri[k] = &tmpv[k]; }
      } else {
        vi = start + seq;
        u32 slot = seq;
        if (slot < vcount) {
          if (!done[slot]) { process_vertex(&dc, vi, &cache[slot]); done[slot] = 1; }
          tri[k] = &cache[slot];
        } else { process_vertex(&dc, vi, &tmpv[k]); tri[k] = &tmpv[k]; }
      }
    }
    draw_triangle(&dc, tri[0], tri[1], tri[2], rts, zs, clip);
  }
  free(cache); free(done);
}

static void swr_clear(u32 count, u32 rects, u32 flags, u32 color, float z) {
  if (!g_render_on) return;
  Obj *rt = obj(D->rt[0]), *zs = obj(D->ds);
  int x0 = D->viewport[0], y0 = D->viewport[1], x1 = x0 + D->viewport[2], y1 = y0 + D->viewport[3];
  if (D->rs[174]) {
    if (x0 < (int)D->scissor[0]) x0 = D->scissor[0]; if (y0 < (int)D->scissor[1]) y0 = D->scissor[1];
    if (x1 > (int)D->scissor[2]) x1 = D->scissor[2]; if (y1 > (int)D->scissor[3]) y1 = D->scissor[3];
  }
  float c[4] = {((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, (color >> 24) / 255.f};
  int nr = count && rects ? count : 1;
  for (int r = 0; r < nr; r++) {
    int rx0 = x0, ry0 = y0, rx1 = x1, ry1 = y1;
    if (count && rects) {
      u32 p = rects + 16 * r;
      if ((s32)M32(p) > rx0) rx0 = M32(p); if ((s32)M32(p + 4) > ry0) ry0 = M32(p + 4);
      if ((s32)M32(p + 8) < rx1) rx1 = M32(p + 8); if ((s32)M32(p + 12) < ry1) ry1 = M32(p + 12);
    }
    if ((flags & 1) && rt) {
      for (int i = 0; i < 4; i++) {
        Obj *t = obj(D->rt[i]);
        if (!t) continue;
        int ex = rx1 < (int)t->w ? rx1 : (int)t->w, ey = ry1 < (int)t->h ? ry1 : (int)t->h;
        for (int y = ry0 < 0 ? 0 : ry0; y < ey; y++) for (int x = rx0 < 0 ? 0 : rx0; x < ex; x++) px_store(t, x, y, c, 0xf);
      }
    }
    if ((flags & 2) && zs && zfmt_float(zs)) {
      int ex = rx1 < (int)zs->w ? rx1 : (int)zs->w, ey = ry1 < (int)zs->h ? ry1 : (int)zs->h;
      for (int y = ry0 < 0 ? 0 : ry0; y < ey; y++) for (int x = rx0 < 0 ? 0 : rx0; x < ex; x++) *zptr(zs, x, y) = z;
    }
  }
}

static void swr_stretch(u32 src, u32 srect, u32 dst, u32 drect, u32 filter) {
  if (!g_render_on) return;
  Obj *s = obj(src), *d = obj(dst);
  if (!s || !d) return;
  int sx0 = 0, sy0 = 0, sx1 = s->w, sy1 = s->h, dx0 = 0, dy0 = 0, dx1 = d->w, dy1 = d->h;
  if (srect) { sx0 = M32(srect); sy0 = M32(srect + 4); sx1 = M32(srect + 8); sy1 = M32(srect + 12); }
  if (drect) { dx0 = M32(drect); dy0 = M32(drect + 4); dx1 = M32(drect + 8); dy1 = M32(drect + 12); }
  if (bpp(d->fmt) == 4 && (d->fmt == 75 || d->fmt == 77 || d->fmt == 71)) { /* depth copy */
    for (int y = dy0; y < dy1 && y < (int)d->h; y++) for (int x = dx0; x < dx1 && x < (int)d->w; x++) {
      int sx = sx0 + (x - dx0) * (sx1 - sx0) / (dx1 - dx0), sy = sy0 + (y - dy0) * (sy1 - sy0) / (dy1 - dy0);
      if (s->fmt == d->fmt && sx < (int)s->w && sy < (int)s->h) *zptr(d, x, y) = *zptr(s, sx, sy);
    }
    return;
  }
  Level L; level_for_surface(s, &L);
  u32 ss[14] = {0, 3, 3, 3, 0, 0, 0};
  for (int y = dy0; y < dy1 && y < (int)d->h; y++)
    for (int x = dx0; x < dx1 && x < (int)d->w; x++) {
      float u = (sx0 + (x - dx0 + 0.5f) * (float)(sx1 - sx0) / (dx1 - dx0)) / s->w;
      float v = (sy0 + (y - dy0 + 0.5f) * (float)(sy1 - sy0) / (dy1 - dy0)) / s->h;
      F4 t = sample_level(&L, u, v, 0, filter >= 2, ss);
      float c[4] = {t.r, t.g, t.b, t.a};
      px_store(d, x, y, c, 0xf);
    }
}
static void swr_colorfill(u32 surf, u32 rect, u32 color) {
  Obj *s = obj(surf);
  if (!s) return;
  int x0 = 0, y0 = 0, x1 = s->w, y1 = s->h;
  if (rect) { x0 = M32(rect); y0 = M32(rect + 4); x1 = M32(rect + 8); y1 = M32(rect + 12); }
  float c[4] = {((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, (color >> 24) / 255.f};
  for (int y = y0; y < y1 && y < (int)s->h; y++) for (int x = x0; x < x1 && x < (int)s->w; x++) px_store(s, x, y, c, 0xf);
}

/* ---------------------------------------------------------------- PNG frame dumps */
static u32 crc_tab[256];
static u32 crc32_(const u8 *p, size_t n, u32 c) {
  if (!crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crc_tab[i] = v; }
  c = ~c; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 255] ^ (c >> 8); return ~c;
}
static void put32(FILE *f, u32 v) { u8 b[4] = {v >> 24, v >> 16, v >> 8, v}; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *t, const u8 *d, u32 n) {
  put32(f, n); fwrite(t, 1, 4, f); if (n) fwrite(d, 1, n, f);
  u32 c = crc32_((const u8 *)t, 4, 0); c = crc32_(d, n, c); put32(f, c);
}
static void write_png(const char *path, Obj *s) {
  u32 w = s->w, h = s->h;
  size_t raw = (size_t)(w * 3 + 1) * h;
  u8 *img = malloc(raw);
  for (u32 y = 0; y < h; y++) {
    img[y * (w * 3 + 1)] = 0;
    for (u32 x = 0; x < w; x++) {
      float c[4]; px_load(s, x, y, c);
      u8 *p = img + y * (w * 3 + 1) + 1 + 3 * x;
      p[0] = to8(c[0]); p[1] = to8(c[1]); p[2] = to8(c[2]);
    }
  }
  /* zlib stream with stored blocks */
  size_t nblk = (raw + 65534) / 65535;
  size_t zn = 2 + raw + nblk * 5 + 4;
  u8 *z = malloc(zn), *q = z;
  *q++ = 0x78; *q++ = 0x01;
  u32 a1 = 1, a2 = 0;
  for (size_t i = 0; i < raw; i++) { a1 = (a1 + img[i]) % 65521; a2 = (a2 + a1) % 65521; }
  for (size_t off = 0; off < raw; off += 65535) {
    u32 n = raw - off > 65535 ? 65535 : raw - off;
    *q++ = off + n >= raw; *q++ = n & 255; *q++ = n >> 8; *q++ = ~n & 255; *q++ = (~n >> 8) & 255;
    memcpy(q, img + off, n); q += n;
  }
  u32 ad = (a2 << 16) | a1; *q++ = ad >> 24; *q++ = ad >> 16; *q++ = ad >> 8; *q++ = ad;
  FILE *f = fopen(path, "wb");
  if (f) {
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, q - z);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
  }
  free(z); free(img);
}
