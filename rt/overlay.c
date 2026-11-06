/* Overlay folders laid over the game folder: their files replace or add to the game's files, and their folders
 * merge into directory listings. A folder in an overlay mirrors the game folder (Data\Textures\...).
 *
 * The game takes a loose file over its copy in a .bsa archive when the loose file is newer than the archive: when
 * it opens an archive it walks Data with FindFirstFileA and compares dates (DirectoryArchive, 0xB01590 in the
 * decomp). Overlay files are reported with the time the app started, so they always count as newer.
 *
 * The first overlay is the app's own assets folder (files built into the executable, tools/embed_assets.py); a mods
 * folder can be another. Paths are matched without regard to case. */
#define _GNU_SOURCE
#include "rt.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>

typedef struct {
  char *rel;    /* lower case, '/' separated, relative to the game folder: "data/textures/interface" */
  char *name;   /* last component as spelled on disk */
  char *host;   /* full host path */
  int dir;
  unsigned parent_hash;
} OvEnt;

static OvEnt *g_ov;
static int g_nov, g_cap;
static int *g_tab; /* open-addressing hash table of indexes into g_ov, by rel */
static int g_tabn;
static time_t g_ov_time;

static unsigned hash_str(const char *s, size_t n) {
  unsigned h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}
static unsigned parent_of(const char *rel) {
  const char *sl = strrchr(rel, '/');
  return hash_str(rel, sl ? (size_t)(sl - rel) : 0);
}

static int find_index(const char *rel) {
  if (!g_tabn) return -1;
  unsigned h = hash_str(rel, strlen(rel));
  for (int i = 0; i < g_tabn; i++) {
    int k = g_tab[(h + i) & (g_tabn - 1)];
    if (k < 0) return -1;
    if (!strcmp(g_ov[k].rel, rel)) return k;
  }
  return -1;
}

static void add(const char *rel, const char *name, const char *host, int dir) {
  for (int i = 0; i < g_nov; i++) /* a later overlay replaces an earlier one's file (only at start-up) */
    if (!strcmp(g_ov[i].rel, rel)) { free(g_ov[i].host); g_ov[i].host = strdup(host); return; }
  if (g_nov == g_cap) { g_cap = g_cap ? 2 * g_cap : 256; g_ov = realloc(g_ov, g_cap * sizeof *g_ov); }
  OvEnt *e = &g_ov[g_nov++];
  e->rel = strdup(rel); e->name = strdup(name); e->host = strdup(host); e->dir = dir;
  e->parent_hash = parent_of(rel);
}

static void scan(const char *host, const char *rel) {
  DIR *d = opendir(host);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.') continue;
    char h[2048], r[2048];
    snprintf(h, sizeof h, "%s/%s", host, de->d_name);
    snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
    for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    struct stat st;
    if (stat(h, &st)) continue;
    int dir = S_ISDIR(st.st_mode);
    add(r, de->d_name, h, dir);
    if (dir) scan(h, r);
  }
  closedir(d);
}

static void build_table(void) {
  free(g_tab);
  g_tabn = 16;
  while (g_tabn < 2 * g_nov) g_tabn *= 2;
  g_tab = malloc(g_tabn * sizeof *g_tab);
  for (int i = 0; i < g_tabn; i++) g_tab[i] = -1;
  for (int k = 0; k < g_nov; k++) {
    unsigned h = hash_str(g_ov[k].rel, strlen(g_ov[k].rel));
    int i = 0;
    while (g_tab[(h + i) & (g_tabn - 1)] >= 0) i++;
    g_tab[(h + i) & (g_tabn - 1)] = k;
  }
}

/* adds a folder that mirrors the game folder; later folders win */
void overlay_add(const char *root) {
  int before = g_nov;
  scan(root, "");
  build_table();
  int files = 0;
  for (int i = before; i < g_nov; i++) files += !g_ov[i].dir;
  if (g_nov > before) rt_log("overlay: %d files from %s", files, root);
}

/* the app's own assets, built into the executable (tools/embed_assets.py) */
typedef struct { const char *path; const unsigned char *data; size_t size; } EmbeddedAsset;
extern const EmbeddedAsset g_assets[];
extern const int g_nassets;

/* a built-in asset's bytes, or NULL */
const unsigned char *rt_asset(const char *path, size_t *size) {
  for (int i = 0; i < g_nassets; i++)
    if (!strcmp(g_assets[i].path, path)) { if (size) *size = g_assets[i].size; return g_assets[i].data; }
  return NULL;
}

static void mkdirs_for(const char *file) {
  char p[2048];
  snprintf(p, sizeof p, "%s", file);
  for (char *q = p + 1; *q; q++)
    if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
}

/* writes the built-in assets to dir, skipping files already there with the same contents */
static void extract_assets(const char *dir) {
  int written = 0;
  for (int i = 0; i < g_nassets; i++) {
    if (!strncmp(g_assets[i].path, "internal/", 9)) continue; /* used from memory (glyphs.c), not as files */
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, g_assets[i].path);
    FILE *f = fopen(path, "rb");
    if (f) {
      int same = 1;
      unsigned char buf[16384];
      size_t off = 0, n;
      while (same && (n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (off + n > g_assets[i].size || memcmp(buf, g_assets[i].data + off, n)) same = 0;
        off += n;
      }
      fclose(f);
      if (same && off == g_assets[i].size) continue;
    }
    mkdirs_for(path);
    f = fopen(path, "wb");
    if (!f) { rt_log("overlay: cannot write %s", path); continue; }
    fwrite(g_assets[i].data, 1, g_assets[i].size, f);
    fclose(f);
    written++;
  }
  if (written) rt_log("overlay: wrote %d built-in assets to %s", written, dir);
}

/* The game only takes a loose file over its archive copy when the archive entry is invalidated: at start-up it
   reads Data\ArchiveInvalidation.txt (SInvalidationFile), where a bare file name (no backslash) invalidates that
   file in every archive folder that also exists as a loose folder (ClearMatchedEntries, 0xAFAD00; the list is
   read by 0xAF5AB0, one name per CRLF line). Our list: the game's own file, if any, plus every overlay file name. */
static void collect_names(const char *host, FILE *out) {
  DIR *d = opendir(host);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.') continue;
    char h[2048];
    snprintf(h, sizeof h, "%s/%s", host, de->d_name);
    struct stat st;
    if (stat(h, &st)) continue;
    if (S_ISDIR(st.st_mode)) collect_names(h, out);
    else if (strcasecmp(de->d_name, "ArchiveInvalidation.txt")) fprintf(out, "%s\r\n", de->d_name);
  }
  closedir(d);
}
static void write_invalidation(const char *root) {
  char path[2048], data[2048], game[2048];
  snprintf(data, sizeof data, "%s/Data", root);
  if (access(data, F_OK)) return;
  snprintf(path, sizeof path, "%s/ArchiveInvalidation.txt", data);
  FILE *out = fopen(path, "wb");
  if (!out) return;
  if (!fs_host_path("Data\\ArchiveInvalidation.txt", game, sizeof game) && strncmp(game, root, strlen(root))) {
    FILE *in = fopen(game, "rb"); /* the game folder's own list (mods), kept */
    if (in) {
      char buf[4096]; size_t n;
      while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
      fclose(in);
      fputs("\r\n", out);
    }
  }
  collect_names(data, out);
  fclose(out);
}

/* files in the assets folder that this build does not ship (left by an older version) are removed */
static void prune(const char *root, const char *host, const char *rel) {
  DIR *d = opendir(host);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.') continue;
    char h[2048], r[2048];
    snprintf(h, sizeof h, "%s/%s", host, de->d_name);
    snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
    struct stat st;
    if (stat(h, &st)) continue;
    if (S_ISDIR(st.st_mode)) { prune(root, h, r); rmdir(h); continue; } /* rmdir only succeeds when empty */
    int keep = 0;
    for (int i = 0; i < g_nassets && !keep; i++) keep = !strcmp(g_assets[i].path, r);
    if (!keep && unlink(h) == 0) rt_log("overlay: removed %s (not in this build)", r);
  }
  closedir(d);
}

void overlay_init(void) {
  g_ov_time = time(NULL);
  const char *dir = getenv("FNV_ASSETS_DIR"); /* a folder to use instead of the built-in assets (testing) */
  char buf[1100];
  if (!dir && g_nassets) {
    snprintf(buf, sizeof buf, "%s/assets", g_user_dir);
    prune(buf, buf, "");
    extract_assets(buf);
    dir = buf;
  }
  if (dir && access(dir, F_OK) == 0) {
    write_invalidation(dir);
    overlay_add(dir);
  }
}

/* host path of the overlay file or folder at rel (lower case, '/' separated), or NULL */
const char *overlay_find(const char *rel, int *is_dir) {
  int k = find_index(rel);
  if (k < 0) return NULL;
  if (is_dir) *is_dir = g_ov[k].dir;
  return g_ov[k].host;
}

/* entries directly inside folder rel; the callback gets the name as spelled, the host path, and whether it is a
   folder */
void overlay_list(const char *rel, void (*cb)(void *ctx, const char *name, const char *host, int dir), void *ctx) {
  unsigned ph = hash_str(rel, strlen(rel));
  size_t rl = strlen(rel);
  for (int i = 0; i < g_nov; i++) {
    const OvEnt *e = &g_ov[i];
    if (e->parent_hash != ph) continue;
    const char *sl = strrchr(e->rel, '/');
    if ((size_t)(sl ? sl - e->rel : 0) != rl || strncmp(e->rel, rel, rl)) continue;
    cb(ctx, e->name, e->host, e->dir);
  }
}

time_t overlay_time(void) { return g_ov_time; }
int overlay_active(void) { return g_nov > 0; }
