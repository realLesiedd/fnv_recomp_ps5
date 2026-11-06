/* PlayStation button prompts. The game draws its controller prompts (Pip-Boy, menus, HUD hints) from 64x64 cells of
 * its UI texture atlas, textures\\interface\\interfaceshared0.dds (1024x1024 DXT5, laid out by interfaceshared.tai;
 * that is why the separate general_button_*.dds files are never read).
 *
 * The PS3 version drew its prompts in the same art as the Xbox ones in the PC files, with the PlayStation symbols:
 * "✕)" where the Xbox has "A)" (the arc is the side of a raised button). So when the game uploads that atlas (recognised
 * by its first bytes and an FNV-1a 64 fingerprint of the player's own copy), each of the 22 button cells keeps the
 * game's own art and loses only its letters (the face buttons; the shoulders, triggers, sticks and Start/Back are
 * redrawn whole, in the thin-outline PS3 style, as tools/make_glyphs.py describes): the cell is split into its solid
 * pieces, the one with the largest extent
 * (the arc, the bumper or trigger outline, the stick, the circle) is kept along with any small piece above or below
 * the middle band (a broken-off tip), the rest - the letters - are erased with every pixel of glow nearer to them
 * than to the kept shape, and the symbol drawn by tools/make_glyphs.py (✕ ○ □ △, L1/R1, L2/R2, L3/R3, the DualSense's
 * Options and Create marks) is laid in, with a halo like the game's own on the highlighted cells. The mipmaps of the
 * cell are rebuilt from the result. Nothing in the player's files changes, and any other atlas is left alone.
 *
 * The Pip-Boy arm has the main-tab buttons printed on it, LT and RT (textures\\pipboy3000\\
 * pipboyarm01.dds and the Xbox copies, 1024x1024 DXT1). On the DualSense those are L2 and R2: when that texture is
 * uploaded (recognised the same way), the old letters on the two tags are erased, the gap filled from the tag around
 * them, and L2/R2 painted in the old letters' colour (lettering from tools/make_glyphs.py). The first three mipmap
 * levels are redone from the result; below that the tags are a few pixels across. */
#include "rt.h"
#include "d3d9_internal.h"
#include <math.h>

typedef struct { const char *path; const unsigned char *data; size_t size; } EmbeddedAsset;
extern const EmbeddedAsset g_assets[];
extern const int g_nassets;

#define ATLAS_HASH 0xfc5df0d3e853db38ull
static const u8 g_atlas_head[32] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0xaa, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0xaa, 0xaa};

/* the button cells of interfaceshared0.dds (pixel position of each 64x64 cell, from interfaceshared.tai) */
static const struct { const char *name; u32 x, y; } g_cells[] = {
  {"glow_general_button_rb", 320, 640}, /* 64x64 */
  {"glow_general_button_rs", 896, 640}, /* 64x64 */
  {"glow_general_button_rt", 960, 640}, /* 64x64 */
  {"glow_general_button_start", 320, 704}, /* 64x64 */
  {"glow_general_button_x", 384, 704}, /* 64x64 */
  {"glow_general_button_y", 448, 704}, /* 64x64 */
  {"general_button_rb", 512, 704}, /* 64x64 */
  {"general_button_a", 128, 768}, /* 64x64 */
  {"general_button_b", 192, 768}, /* 64x64 */
  {"general_button_lb", 256, 768}, /* 64x64 */
  {"general_button_ls", 320, 768}, /* 64x64 */
  {"general_button_lt", 384, 768}, /* 64x64 */
  {"general_button_rs", 512, 768}, /* 64x64 */
  {"general_button_rt", 576, 768}, /* 64x64 */
  {"general_button_x", 640, 768}, /* 64x64 */
  {"general_button_y", 704, 768}, /* 64x64 */
  {"glow_general_button_lb", 768, 768}, /* 64x64 */
  {"glow_general_button_a", 960, 768}, /* 64x64 */
  {"glow_general_button_b", 0, 832}, /* 64x64 */
  {"glow_general_button_back", 64, 832}, /* 64x64 */
  {"glow_general_button_ls", 128, 832}, /* 64x64 */
  {"glow_general_button_lt", 192, 832}, /* 64x64 */
};

static u64 fnv1a64(const u8 *p, size_t n) {
  u64 h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

static const EmbeddedAsset *asset(const char *path) {
  for (int k = 0; k < g_nassets; k++)
    if (!strcmp(g_assets[k].path, path)) return &g_assets[k];
  return NULL;
}

/* ---- the Pip-Boy arm's LT/RT tags ---- */
/* the arm copies that show LT/RT: textures\\pipboy3000\\pipboyarm01.dds (the one the game draws) and the Xbox
   folder's (the same in xbox\\english). The game uploads them at 1024 or, with a lower texture setting, from the
   512 level down: first bytes and fingerprint of each */
static const u8 g_arm_head[2][8] = {{0xe2, 0x20, 0x00, 0x00, 0x55, 0xab, 0xab, 0xab}, {0xc2, 0x18, 0x20, 0x08, 0xff, 0xab, 0xab, 0xab}};
static const u64 g_arm_hash[2][2] = {{0xd18d63edeb8363f0ull, 0x63690e859d5f382aull}, {0xc5e01679420beb0bull, 0xd7e73b648c69a004ull}};
/* each tag's old letters: their box in the 1024 image, and the lettering to paint there */
static const struct { const char *label; int x, y, w, h; } g_tags[] = {
  {"internal/arm_labels/l2.a8", 714, 155, 20, 13},
  {"internal/arm_labels/r2.a8", 926, 155, 25, 13},
};

typedef struct { float r, g, b; } Rgb;

static Rgb rgb565(u16 c) { return (Rgb){(float)((c >> 11) * 255 / 31), (float)(((c >> 5) & 63) * 255 / 63), (float)((c & 31) * 255 / 31)}; }
static u16 to565(Rgb c) {
  int r = (int)(c.r * 31 / 255 + 0.5f), g = (int)(c.g * 63 / 255 + 0.5f), b = (int)(c.b * 31 / 255 + 0.5f);
  r = r < 0 ? 0 : r > 31 ? 31 : r; g = g < 0 ? 0 : g > 63 ? 63 : g; b = b < 0 ? 0 : b > 31 ? 31 : b;
  return (u16)(r << 11 | g << 5 | b);
}
static void dxt1_palette(u16 c0, u16 c1, Rgb pal[4]) {
  pal[0] = rgb565(c0); pal[1] = rgb565(c1);
  if (c0 > c1) {
    pal[2] = (Rgb){(2 * pal[0].r + pal[1].r) / 3, (2 * pal[0].g + pal[1].g) / 3, (2 * pal[0].b + pal[1].b) / 3};
    pal[3] = (Rgb){(pal[0].r + 2 * pal[1].r) / 3, (pal[0].g + 2 * pal[1].g) / 3, (pal[0].b + 2 * pal[1].b) / 3};
  } else {
    pal[2] = (Rgb){(pal[0].r + pal[1].r) / 2, (pal[0].g + pal[1].g) / 2, (pal[0].b + pal[1].b) / 2};
    pal[3] = (Rgb){0, 0, 0};
  }
}
static void dxt1_decode(const u8 *blk, Rgb px[16]) {
  Rgb pal[4];
  dxt1_palette((u16)(blk[0] | blk[1] << 8), (u16)(blk[2] | blk[3] << 8), pal);
  u32 bits = (u32)blk[4] | (u32)blk[5] << 8 | (u32)blk[6] << 16 | (u32)blk[7] << 24;
  for (int i = 0; i < 16; i++) px[i] = pal[(bits >> (2 * i)) & 3];
}
static float dist2(Rgb a, Rgb b) { float r = a.r - b.r, g = a.g - b.g, bl = a.b - b.b; return r * r + g * g + bl * bl; }
/* end points from the extremes along the block's main colour axis, then the nearest of the four colours */
static void dxt1_encode(const Rgb px[16], u8 *blk) {
  Rgb m = {0, 0, 0};
  for (int i = 0; i < 16; i++) { m.r += px[i].r / 16; m.g += px[i].g / 16; m.b += px[i].b / 16; }
  float c[6] = {0}; /* covariance rr rg rb gg gb bb */
  for (int i = 0; i < 16; i++) {
    float r = px[i].r - m.r, g = px[i].g - m.g, b = px[i].b - m.b;
    c[0] += r * r; c[1] += r * g; c[2] += r * b; c[3] += g * g; c[4] += g * b; c[5] += b * b;
  }
  float v[3] = {1, 1, 1};
  for (int k = 0; k < 8; k++) {
    float x = c[0] * v[0] + c[1] * v[1] + c[2] * v[2], y = c[1] * v[0] + c[3] * v[1] + c[4] * v[2],
          z = c[2] * v[0] + c[4] * v[1] + c[5] * v[2];
    float n = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y); if (fabsf(z) > n) n = fabsf(z);
    if (n < 1e-6f) break;
    v[0] = x / n; v[1] = y / n; v[2] = z / n;
  }
  int lo = 0, hi = 0; float plo = 1e30f, phi = -1e30f;
  for (int i = 0; i < 16; i++) {
    float p = (px[i].r - m.r) * v[0] + (px[i].g - m.g) * v[1] + (px[i].b - m.b) * v[2];
    if (p < plo) { plo = p; lo = i; }
    if (p > phi) { phi = p; hi = i; }
  }
  u16 c0 = to565(px[hi]), c1 = to565(px[lo]);
  if (c0 < c1) { u16 t = c0; c0 = c1; c1 = t; }
  u32 bits = 0;
  if (c0 != c1) {
    Rgb pal[4];
    dxt1_palette(c0, c1, pal);
    for (int i = 0; i < 16; i++) {
      int best = 0;
      for (int j = 1; j < 4; j++) if (dist2(px[i], pal[j]) < dist2(px[i], pal[best])) best = j;
      bits |= (u32)best << (2 * i);
    }
  }
  blk[0] = (u8)c0; blk[1] = (u8)(c0 >> 8); blk[2] = (u8)c1; blk[3] = (u8)(c1 >> 8);
  blk[4] = (u8)bits; blk[5] = (u8)(bits >> 8); blk[6] = (u8)(bits >> 16); blk[7] = (u8)(bits >> 24);
}

static float luma(Rgb c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }

/* The work is done at 1024 whatever size the game loaded (k = 0 for 1024, 1 for 512): the area is read from the
   texture's top level, each texel repeated 2^k times each way, edited, and each changed block of the first three
   1024-based levels the texture has is averaged back down. Unchanged texels average back to what they were. */
#define AREA 48 /* the area redone around a tag: whole 16-pixel squares, so the first three mip levels get whole blocks */
static void arm_tag(Obj *o, int t, int k) {
  const EmbeddedAsset *a = asset(g_tags[t].label);
  if (!a || a->size < 2 || a->size < 2u + a->data[0] * a->data[1]) return;
  int tx = g_tags[t].x, ty = g_tags[t].y, tw = g_tags[t].w, th = g_tags[t].h;
  if (a->data[0] != tw || a->data[1] != th) return;
  int ax = (tx - 1) & ~15, ay = (ty - 1) & ~15; /* the area: 16-aligned, covering the box and a pixel around it */
  if (tx + tw + 1 > ax + AREA || ty + th + 1 > ay + AREA) return;
  static Rgb px[AREA][AREA], orig[AREA][AREA];
  int f0 = 4 << k; /* level-0 pixels across one block of the top level */
  for (int by = 0; by < AREA / f0; by++)
    for (int bx = 0; bx < AREA / f0; bx++) {
      Rgb b[16];
      dxt1_decode(HP(o->data[0][0] + ((ay >> k) / 4 + by) * o->pitch[0] + ((ax >> k) / 4 + bx) * 8), b);
      for (int y = 0; y < f0; y++)
        for (int x = 0; x < f0; x++) px[by * f0 + y][bx * f0 + x] = b[(y >> k) * 4 + (x >> k)];
    }
  memcpy(orig, px, sizeof px);
  /* the old letters: dark pixels in the box, widened by a pixel (their soft edges) */
  static unsigned char dark[AREA][AREA], known[AREA][AREA];
  memset(dark, 0, sizeof dark);
  Rgb ink = {0, 0, 0}; int nink = 0;
  for (int y = ty; y < ty + th; y++)
    for (int x = tx; x < tx + tw; x++) {
      Rgb c = px[y - ay][x - ax];
      if (luma(c) < 80) dark[y - ay][x - ax] = 1;
      if (luma(c) < 45) { ink.r += c.r; ink.g += c.g; ink.b += c.b; nink++; }
    }
  if (nink < 10) return; /* not the tag we expect */
  ink.r /= nink; ink.g /= nink; ink.b /= nink;
  for (int y = 0; y < AREA; y++)
    for (int x = 0; x < AREA; x++) {
      int gx = x + ax, gy = y + ay, d = 0;
      if (gx >= tx - 1 && gx <= tx + tw && gy >= ty - 1 && gy <= ty + th)
        for (int k = 0; k < 5 && !d; k++) {
          static const int dx[5] = {0, 1, -1, 0, 0}, dy[5] = {0, 0, 0, 1, -1};
          int u = x + dx[k], v = y + dy[k];
          d = u >= 0 && u < AREA && v >= 0 && v < AREA && dark[v][u];
        }
      known[y][x] = !d;
    }
  /* fill them in from the tag around, one ring at a time */
  for (int pass = 0, left = 1; pass < 64 && left; pass++) {
    left = 0;
    static unsigned char now[AREA][AREA];
    memcpy(now, known, sizeof now);
    for (int y = 1; y < AREA - 1; y++)
      for (int x = 1; x < AREA - 1; x++) {
        if (known[y][x]) continue;
        Rgb s = {0, 0, 0}; int n = 0;
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++)
          if (known[y + dy[k]][x + dx[k]]) { Rgb c = px[y + dy[k]][x + dx[k]]; s.r += c.r; s.g += c.g; s.b += c.b; n++; }
        if (n) { px[y][x] = (Rgb){s.r / n, s.g / n, s.b / n}; now[y][x] = 1; } else left = 1;
      }
    memcpy(known, now, sizeof known);
  }
  /* the new lettering */
  for (int y = 0; y < th; y++)
    for (int x = 0; x < tw; x++) {
      float k = a->data[2 + y * tw + x] / 255.0f;
      Rgb *p = &px[ty - ay + y][tx - ax + x];
      p->r += (ink.r - p->r) * k; p->g += (ink.g - p->g) * k; p->b += (ink.b - p->b) * k;
    }
  /* write back the blocks that changed, in the top level and the ones below it, down to the 1024-based level 2 */
  for (u32 l = 0; (int)l + k < 3 && l < o->levels && o->data[0][l]; l++) {
    int f = 4 << (l + k), s = 1 << (l + k); /* a block covers f x f level-0 pixels; each texel s x s */
    for (int by = 0; by < AREA / f; by++)
      for (int bx = 0; bx < AREA / f; bx++) {
        int changed = 0;
        for (int y = by * f; y < by * f + f && !changed; y++)
          for (int x = bx * f; x < bx * f + f && !changed; x++) changed = dist2(px[y][x], orig[y][x]) > 1;
        if (!changed) continue;
        Rgb b[16];
        for (int i = 0; i < 16; i++) {
          Rgb c = {0, 0, 0};
          for (int v = 0; v < s; v++)
            for (int u = 0; u < s; u++) { Rgb q = px[by * f + (i / 4) * s + v][bx * f + (i % 4) * s + u]; c.r += q.r; c.g += q.g; c.b += q.b; }
          b[i] = (Rgb){c.r / (s * s), c.g / (s * s), c.b / (s * s)};
        }
        dxt1_encode(b, HP(o->data[0][l] + ((ay >> (l + k)) / 4 + by) * o->pitch[l] + ((ax >> (l + k)) / 4 + bx) * 8));
      }
  }
}

static void arm_patch(Obj *o) {
  int k = o->w == 1024 ? 0 : 1;
  if (memcmp(HP(o->data[0][0]), g_arm_head[k], sizeof g_arm_head[k])) return;
  u64 h = fnv1a64(HP(o->data[0][0]), (size_t)o->w * o->h / 2);
  if (h != g_arm_hash[k][0] && h != g_arm_hash[k][1]) {
    static int said;
    if (!said++) rt_log("button prompts: an arm-like texture not recognised (%016llx), left as it is", (unsigned long long)h);
    return;
  }
  for (unsigned t = 0; t < sizeof g_tags / sizeof *g_tags; t++) arm_tag(o, t, k);
  rt_log("button prompts: Pip-Boy arm relabelled L2/R2 (%ux%u)", o->w, o->h);
}


/* ---- the UI atlas's button cells ------------------------------------------------------------------------------- */
typedef struct { float r, g, b, a; } Px;

/* DXT5: an alpha block (two end points, eight levels, 3-bit indices) and a colour block (always four colours) */
static void dxt5_decode(const u8 *blk, Px px[16]) {
  float al[8];
  al[0] = blk[0]; al[1] = blk[1];
  if (blk[0] > blk[1]) for (int i = 1; i < 7; i++) al[1 + i] = ((7 - i) * al[0] + i * al[1]) / 7;
  else { for (int i = 1; i < 5; i++) al[1 + i] = ((5 - i) * al[0] + i * al[1]) / 5; al[6] = 0; al[7] = 255; }
  u64 abits = 0;
  for (int i = 0; i < 6; i++) abits |= (u64)blk[2 + i] << (8 * i);
  Rgb pal[4];
  u16 c0 = (u16)(blk[8] | blk[9] << 8), c1 = (u16)(blk[10] | blk[11] << 8);
  pal[0] = rgb565(c0); pal[1] = rgb565(c1);
  pal[2] = (Rgb){(2 * pal[0].r + pal[1].r) / 3, (2 * pal[0].g + pal[1].g) / 3, (2 * pal[0].b + pal[1].b) / 3};
  pal[3] = (Rgb){(pal[0].r + 2 * pal[1].r) / 3, (pal[0].g + 2 * pal[1].g) / 3, (pal[0].b + 2 * pal[1].b) / 3};
  u32 cbits = (u32)blk[12] | (u32)blk[13] << 8 | (u32)blk[14] << 16 | (u32)blk[15] << 24;
  for (int i = 0; i < 16; i++) {
    Rgb c = pal[(cbits >> (2 * i)) & 3];
    px[i] = (Px){c.r, c.g, c.b, al[(abits >> (3 * i)) & 7]};
  }
}
static void dxt5_encode(const Px px[16], u8 *blk) {
  float lo = 255, hi = 0;
  for (int i = 0; i < 16; i++) { if (px[i].a < lo) lo = px[i].a; if (px[i].a > hi) hi = px[i].a; }
  int a0 = (int)(hi + 0.5f), a1 = (int)(lo + 0.5f);
  u64 bits = 0;
  if (a0 == a1) { if (a0 < 255) a0++; else a1--; } /* eight-level mode needs a0 > a1 */
  float al[8];
  al[0] = (float)a0; al[1] = (float)a1;
  for (int i = 1; i < 7; i++) al[1 + i] = ((7 - i) * al[0] + i * al[1]) / 7;
  for (int i = 0; i < 16; i++) {
    int best = 0;
    for (int j = 1; j < 8; j++) if (fabsf(al[j] - px[i].a) < fabsf(al[best] - px[i].a)) best = j;
    bits |= (u64)best << (3 * i);
  }
  blk[0] = (u8)a0; blk[1] = (u8)a1;
  for (int i = 0; i < 6; i++) blk[2 + i] = (u8)(bits >> (8 * i));
  Rgb c[16];
  for (int i = 0; i < 16; i++) c[i] = (Rgb){px[i].r, px[i].g, px[i].b};
  dxt1_encode(c, blk + 8); /* always four colours (c0 >= c1); DXT5 ignores the order */
}

/* the cell's solid pieces (4-connected, alpha above th): label per pixel, and each piece's box and size */
typedef struct { int x0, y0, x1, y1, n; } Piece;
static int pieces(const Px img[64][64], float th, short lab[64][64], Piece *pc, int max) {
  static short stack[4096 * 2];
  int np = 0;
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) lab[y][x] = -1;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      if (img[y][x].a <= th || lab[y][x] >= 0 || np >= max) continue;
      Piece *p = &pc[np];
      *p = (Piece){x, y, x, y, 0};
      int sp = 0;
      stack[sp++] = (short)y; stack[sp++] = (short)x; lab[y][x] = (short)np;
      while (sp) {
        int cx = stack[--sp], cy = stack[--sp];
        p->n++;
        if (cx < p->x0) p->x0 = cx; if (cx > p->x1) p->x1 = cx; if (cy < p->y0) p->y0 = cy; if (cy > p->y1) p->y1 = cy;
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
          int nx = cx + dx[k], ny = cy + dy[k];
          if (nx < 0 || nx > 63 || ny < 0 || ny > 63 || lab[ny][nx] >= 0 || img[ny][nx].a <= th) continue;
          lab[ny][nx] = (short)np;
          stack[sp++] = (short)ny; stack[sp++] = (short)nx;
        }
      }
      np++;
    }
  return np;
}

/* erases the letters and their glow; their box goes in *box. Returns 0 if there were none. */
static int erase_letters(Px img[64][64], int box[4]) {
  static short lab[64][64];
  Piece pc[64];
  int np = 0;
  for (int pass = 0; pass < 2 && np <= 1; pass++) np = pieces(img, pass ? 235.f : 180.f, lab, pc, 64); /* letters may
                                                                     touch the outline through a glow */
  if (np <= 1) return 0;
  int keep = 0;
  for (int i = 1; i < np; i++)
    if ((pc[i].x1 - pc[i].x0 + 1) * (pc[i].y1 - pc[i].y0 + 1) > (pc[keep].x1 - pc[keep].x0 + 1) * (pc[keep].y1 - pc[keep].y0 + 1)) keep = i;
  char letter[64];
  int any = 0;
  box[0] = box[1] = 64; box[2] = box[3] = -1;
  for (int i = 0; i < np; i++) {
    float cy = (pc[i].y0 + pc[i].y1) / 2.0f;
    letter[i] = i != keep && pc[i].n >= 4 && cy >= 18 && cy <= 45; /* a tip above or below the band stays */
    if (!letter[i]) continue;
    any = 1;
    if (pc[i].x0 < box[0]) box[0] = pc[i].x0; if (pc[i].y0 < box[1]) box[1] = pc[i].y0;
    if (pc[i].x1 > box[2]) box[2] = pc[i].x1; if (pc[i].y1 > box[3]) box[3] = pc[i].y1;
  }
  if (!any) return 0;
  /* each visible pixel goes with the nearer of: the letters' solid pixels, the kept shape's */
  static short lp[4096][2], kp[4096][2];
  int nl = 0, nk = 0;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      int l = lab[y][x];
      if (l < 0) continue;
      if (letter[l]) { lp[nl][0] = (short)y; lp[nl][1] = (short)x; nl++; }
      else { kp[nk][0] = (short)y; kp[nk][1] = (short)x; nk++; }
    }
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      if (img[y][x].a <= 0) continue;
      int dl = 1 << 30, dk = 1 << 30;
      for (int i = 0; i < nl; i++) { int d = (lp[i][0] - y) * (lp[i][0] - y) + (lp[i][1] - x) * (lp[i][1] - x); if (d < dl) dl = d; }
      if (dl > 14 * 14) continue; /* far from any letter */
      for (int i = 0; i < nk && dk > dl; i++) { int d = (kp[i][0] - y) * (kp[i][0] - y) + (kp[i][1] - x) * (kp[i][1] - x); if (d < dk) dk = d; }
      if (dl < dk) img[y][x] = (Px){0, 0, 0, 0};
    }
  return 1;
}

static int patch_cell(Obj *o, u32 cx, u32 cy, const EmbeddedAsset *a) {
  if (!a || a->size < 1 + 4096 || ((a->data[0] & 1) && a->size < 1 + 2 * 4096)) return 0;
  int glowing = a->data[0] & 1, whole = a->data[0] & 2; /* whole: the cell is redrawn, not just its letters */
  static Px img[64][64];
  for (int by = 0; by < 16; by++)
    for (int bx = 0; bx < 16; bx++) {
      Px b[16];
      dxt5_decode(HP(o->data[0][0] + (cy / 4 + by) * o->pitch[0] + (cx / 4 + bx) * 16), b);
      for (int i = 0; i < 16; i++) img[by * 4 + i / 4][bx * 4 + i % 4] = b[i];
    }
  int box[4];
  if (whole) memset(img, 0, sizeof img);
  else if (!erase_letters(img, box)) return 0;
  const u8 *body = a->data + 1, *glow = glowing ? a->data + 1 + 4096 : NULL;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      Px *p = &img[y][x];
      if (glow && glow[y * 64 + x] > p->a) { /* the halo: grey, darker where fainter, as the game's own */
        float g = glow[y * 64 + x];
        p->a = g; p->r = p->g = p->b = g * 0.62f;
      }
      float k = body[y * 64 + x] / 255.0f;
      if (k > 0) {
        p->r += (255 - p->r) * k; p->g += (255 - p->g) * k; p->b += (255 - p->b) * k;
        if (body[y * 64 + x] > p->a) p->a = body[y * 64 + x];
      }
    }
  /* write the cell back, and its mipmaps (each an average of the level above) down to 4x4 */
  static Px lvl[64][64];
  memcpy(lvl, img, sizeof lvl);
  for (u32 l = 0, size = 64; size >= 4 && l < o->levels && l < MAXLVL; l++, size /= 2) {
    if (!o->data[0][l]) break;
    if (l) {
      for (u32 y = 0; y < size; y++)
        for (u32 x = 0; x < size; x++) {
          Px s = {0, 0, 0, 0};
          for (int k = 0; k < 4; k++) {
            Px q = lvl[2 * y + (k >> 1)][2 * x + (k & 1)];
            s.r += q.r / 4; s.g += q.g / 4; s.b += q.b / 4; s.a += q.a / 4;
          }
          lvl[y][x] = s;
        }
    }
    for (u32 by = 0; by < size / 4; by++)
      for (u32 bx = 0; bx < size / 4; bx++) {
        Px b[16];
        for (int i = 0; i < 16; i++) b[i] = lvl[by * 4 + i / 4][bx * 4 + i % 4];
        dxt5_encode(b, HP(o->data[0][l] + ((cy >> l) / 4 + by) * o->pitch[l] + ((cx >> l) / 4 + bx) * 16));
      }
  }
  return 1;
}

static void atlas_patch(Obj *o) {
  if (memcmp(HP(o->data[0][0]), g_atlas_head, sizeof g_atlas_head)) return; /* cheap test first */
  if (fnv1a64(HP(o->data[0][0]), 1024u * 1024u) != ATLAS_HASH) return;
  int done = 0;
  for (unsigned i = 0; i < sizeof g_cells / sizeof *g_cells; i++) {
    char path[128];
    snprintf(path, sizeof path, "internal/ps3_cells/%s.cell", g_cells[i].name);
    done += patch_cell(o, g_cells[i].x, g_cells[i].y, asset(path));
  }
  rt_log("button prompts: %d of %d prompts in the UI atlas given PlayStation symbols", done,
         (int)(sizeof g_cells / sizeof *g_cells));
}

/* called before a texture's level 0 goes to the GPU */
void prompt_patch(Obj *o) {
  if (o->kind != K_TEXTURE || o->w != o->h || !o->data[0][0]) return;
  if (o->w == 1024 && o->fmt == 0x35545844u) atlas_patch(o);                       /* DXT5 */
  else if ((o->w == 1024 || o->w == 512) && o->fmt == 0x31545844u) arm_patch(o);   /* DXT1 */
}
