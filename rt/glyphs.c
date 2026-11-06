/* PlayStation button prompts. The game draws its controller prompts (Pip-Boy, menus, HUD hints) from 64x64 cells of
 * its UI texture atlas, textures\\interface\\interfaceshared0.dds (1024x1024 DXT5, laid out by interfaceshared.tai;
 * that is why the separate general_button_*.dds files are never read). When the game uploads that atlas, it is
 * recognised (its first bytes, then an FNV-1a 64 fingerprint of the player's own copy) and the 22 button cells are
 * overwritten, mipmaps included, with the drawings from tools/make_glyphs.py, built into the executable as DXT5
 * blocks. Nothing in the player's files changes, and any other atlas is left alone. */
#include "rt.h"
#include "d3d9_internal.h"

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

/* called before a texture's level 0 goes to the GPU */
void atlas_patch(Obj *o) {
  if (o->kind != K_TEXTURE || o->w != 1024 || o->h != 1024 || o->fmt != 0x35545844u || !o->data[0][0]) return;
  if (memcmp(HP(o->data[0][0]), g_atlas_head, sizeof g_atlas_head)) return; /* cheap test first */
  if (fnv1a64(HP(o->data[0][0]), 1024u * 1024u) != ATLAS_HASH) return;
  int done = 0;
  for (unsigned i = 0; i < sizeof g_cells / sizeof *g_cells; i++) {
    char path[128];
    snprintf(path, sizeof path, "internal/atlas_glyphs/%s.dxt5", g_cells[i].name);
    const EmbeddedAsset *a = asset(path);
    if (!a) continue;
    const u8 *src = a->data;
    for (u32 l = 0, size = 64; size >= 4 && l < o->levels && l < MAXLVL; l++, size /= 2) {
      u32 nb = size / 4, bytes = nb * nb * 16;
      if ((size_t)(src - a->data) + bytes > a->size) break;
      if (o->data[0][l]) {
        u32 bx = (g_cells[i].x >> l) / 4, by = (g_cells[i].y >> l) / 4;
        for (u32 r = 0; r < nb; r++) memcpy(HP(o->data[0][l] + (by + r) * o->pitch[l] + bx * 16), src + r * nb * 16, nb * 16);
      }
      src += bytes;
    }
    done++;
  }
  rt_log("button prompts: %d PlayStation glyphs written into the UI atlas", done);
}
