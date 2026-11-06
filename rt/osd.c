/* Performance overlay and frame-rate cap, both changed from Settings > Display (options.c) and applied at once.
 *
 * The overlay is drawn by the port itself (hostd3d9.cpp) over the finished frame, in the style of MSI Afterburner's
 * on-screen display: a label column, values, and a frame-time graph. Off, Basic (one line: frame rate and frame
 * time) or Detailed:
 *   FPS    frame rate over the last half second, and the 1% low (the 99th percentile frame time of ~5 s, as fps)
 *   FRAME  frame time, and a graph of the last 240 frames with 30 and 60 fps lines
 *   CPU    the game's main thread (the usual limit for this game) and the whole process in cores; PS5 CPU temperature
 *   GPU    busy share from DXVK's GPU idle time (our DXVK patch), shaders being compiled (a stutter's usual cause);
 *          PS5 SoC temperature
 *   VRAM   video memory in use and available (VK_EXT_memory_budget)
 *   RAM    the game's memory, and what the console has left (PS5) or the process's size (PC)
 *   VIDEO  the resolution the game renders at, the frame-rate cap, draw calls per frame
 * Text is sized from the picture's height (about 58 px on a 4K picture).
 *
 * The cap waits before Present until 1/cap s has passed since the last frame. The game times each frame itself
 * and moves the world by the time that passed, so a lower cap only means bigger steps: Havok splits them into its
 * usual 1/60 s substeps (fMaxTime), as on the consoles, which ran the game at 30. It never lets the game run faster
 * than 60, where the engine's physics go wrong. At 60 it waits a hair less than 1/60 s, so a 60 Hz TV's own
 * refresh sets the pace and the two do not fight. */
#define _GNU_SOURCE
#include "rt.h"
#include "d3d9_internal.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#ifdef __PROSPERO__
#include <ps5platform/kernel.h>
/* temperatures: looked up at run time, so a system without them only loses the readings */
int sceKernelLoadStartModule(const char *, size_t, const void *, unsigned, void *, int *);
int sceKernelDlsym(int, const char *, void **);
static int (*p_cpu_temp)(int *), (*p_soc_temp)(int, int *);
static void temp_init(void) {
  static int done;
  if (done) return;
  done = 1;
  int handles[4] = {0x2001, -1, -1, -1}; /* 0x2001: libkernel, already loaded in every process */
  static const char *mods[] = {"libkernel_sys.sprx", "libkernel.sprx", "/system/common/lib/libkernel_sys.sprx"};
  for (int i = 0; i < 3; i++) handles[1 + i] = sceKernelLoadStartModule(mods[i], 0, 0, 0, 0, 0);
  for (int i = 0; i < 4 && (!p_cpu_temp || !p_soc_temp); i++) {
    void *f = 0;
    if (handles[i] < 0) continue;
    if (!p_cpu_temp && !sceKernelDlsym(handles[i], "sceKernelGetCpuTemperature", &f) && f) p_cpu_temp = (int (*)(int *))f;
    f = 0;
    if (!p_soc_temp && !sceKernelDlsym(handles[i], "sceKernelGetSocSensorTemperature", &f) && f) p_soc_temp = (int (*)(int, int *))f;
  }
  rt_log("overlay: temperatures %s", p_cpu_temp || p_soc_temp ? "available" : "not available");
}
#endif

int g_osd_mode = 2;   /* 0 off, 1 basic, 2 detailed */
int g_frame_cap = 60; /* 30 or 60 */

int rt_setting_int(const char *key, int def); /* firstrun.c */
u64 d3d_draw_count(void);                     /* d3d9.c */

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

void osd_init(void) {
  g_osd_mode = rt_setting_int("iPerfOverlay", 2);
  g_frame_cap = rt_setting_int("iFrameCap", 60);
  if (g_osd_mode < 0 || g_osd_mode > 2) g_osd_mode = 2;
  if (g_frame_cap != 30) g_frame_cap = 60;
}

/* ---- frame-rate cap -------------------------------------------------------------------------------- */
void osd_frame_limit(void) {
  static double next;
  double period = g_frame_cap == 30 ? 1.0 / 30 - 0.0002 : 1.0 / 60 - 0.0002;
  double t = now_s();
  if (next > 0 && t < next) {
    double wait = next - t;
    if (wait > 0.1) wait = 0.1;
    if (wait > 0.0015) { /* sleep most of it, then spin: sleeps overshoot by up to a millisecond */
      struct timespec ts = {0, (long)((wait - 0.001) * 1e9)};
      nanosleep(&ts, NULL);
    }
    while ((t = now_s()) < next) {}
  }
  next = (next > 0 && t < next + period) ? next + period : t + period; /* late: start again from now */
}

/* ---- statistics -------------------------------------------------------------------------------------- */
#define HIST 240
#define LOWN 512
static float g_hist[HIST]; /* frame times, ms, newest at g_hist_i - 1 */
static int g_hist_i;
static float g_low[LOWN]; /* the last ~5 s of frame times, for the 1% low */
static int g_low_i, g_low_n;
static double g_last, g_acc_ms;
static int g_acc_n;

static struct {
  double fps, ms, low1;
  int main_pct, total_pct, cores, gpu, compiling, temp_cpu, temp_soc, draws;
  unsigned vram_used, vram_total, ram_game, ram_free, ram_proc;
} S = {.gpu = -1, .temp_cpu = -1, .temp_soc = -1};

static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static void sample(double t, double span) {
  S.fps = g_acc_n && g_acc_ms > 0 ? 1000.0 * g_acc_n / g_acc_ms : 0;
  S.ms = g_acc_n ? g_acc_ms / g_acc_n : 0;
  g_acc_ms = 0; g_acc_n = 0;
  if (g_low_n) { /* 1% low */
    static float tmp[LOWN];
    memcpy(tmp, g_low, g_low_n * sizeof *tmp);
    qsort(tmp, g_low_n, sizeof *tmp, cmp_f);
    float p99 = tmp[(int)(g_low_n * 0.99) < g_low_n ? (int)(g_low_n * 0.99) : g_low_n - 1];
    S.low1 = p99 > 0 ? 1000.0 / p99 : 0;
  }
  /* CPU: the presenting thread (the game's main thread in play) and the process */
  static double last_thr = -1, last_proc = -1;
  struct timespec ts;
  double thr = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) ? -1 : ts.tv_sec + ts.tv_nsec / 1e9;
  struct rusage ru;
  double proc = getrusage(RUSAGE_SELF, &ru) ? -1 : ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
  if (span > 0 && last_thr >= 0 && thr >= 0) S.main_pct = (int)(100 * (thr - last_thr) / span + 0.5);
  if (span > 0 && last_proc >= 0 && proc >= 0) S.total_pct = (int)(100 * (proc - last_proc) / span + 0.5);
  last_thr = thr; last_proc = proc;
  if (!S.cores) { long n = sysconf(_SC_NPROCESSORS_ONLN); S.cores = n > 0 ? (int)n : 8; }
  S.gpu = hb_gpu_load(&S.compiling);
  if (!hb_vram_mb(&S.vram_used, &S.vram_total)) S.vram_total = 0;
  S.ram_game = (unsigned)(g_guest_backed >> 20);
#ifdef __PROSPERO__
  size_t flex = 0;
  S.ram_free = sceKernelAvailableFlexibleMemorySize(&flex) == 0 ? (unsigned)(flex >> 20) : 0;
  static double last_temp;
  if (t - last_temp > 2) {
    last_temp = t;
    temp_init();
    int c = 0;
    S.temp_cpu = p_cpu_temp && p_cpu_temp(&c) == 0 && c > 0 && c < 150 ? c : -1;
    c = 0;
    S.temp_soc = p_soc_temp && p_soc_temp(0, &c) == 0 && c > 0 && c < 150 ? c : -1;
  }
#else
  (void)t;
  FILE *f = fopen("/proc/self/statm", "r");
  unsigned long vsz, rss;
  if (f && fscanf(f, "%lu %lu", &vsz, &rss) == 2) S.ram_proc = (unsigned)(rss * 4096 >> 20);
  if (f) fclose(f);
#endif
}

/* ---- drawing ------------------------------------------------------------------------------------------- */
#define C_LABEL 0xFFFFA43Cu
#define C_VALUE 0xFFFFFFFFu
#define C_DIM   0xFFB4B4B4u
#define C_GOOD  0xFF6CFF6Cu
#define C_WARN  0xFFFFE04Au
#define C_BAD   0xFFFF5A5Au
#define C_PANEL 0x9E000000u

static u32 fps_color(double fps) { return fps >= 55 ? C_GOOD : fps >= 28 ? C_WARN : C_BAD; }
static u32 ms_color(float ms) { return ms <= 17.5f ? C_GOOD : ms <= 34.5f ? C_WARN : C_BAD; }

/* one line: label, value (colour), and a dimmer detail; or the graph */
typedef struct { const char *label; char value[32], detail[96]; u32 vcolor, dcolor; int graph; } Row;
#define DETAIL 0.82f /* detail text size, relative */
static float row_width(const Row *r, float size, float col1) {
  if (r->graph) return 0;
  return col1 + hb_osd_text_width(size, r->value) + (r->detail[0] ? size * 0.35f + hb_osd_text_width(size * DETAIL, r->detail) : 0);
}

void osd_frame(void) {
  double t = now_s();
  if (g_last > 0) {
    float ms = (float)((t - g_last) * 1000);
    if (ms > 1000) ms = 1000;
    g_hist[g_hist_i] = ms; g_hist_i = (g_hist_i + 1) % HIST;
    g_low[g_low_i] = ms; g_low_i = (g_low_i + 1) % LOWN; if (g_low_n < LOWN) g_low_n++;
    g_acc_ms += ms; g_acc_n++;
  }
  g_last = t;
  static double last_sample;
  if (t - last_sample >= 0.5) {
    static u64 last_draws, last_frames_for_draws;
    extern u64 g_d3d_frames;
    u64 d = d3d_draw_count(), fr = g_d3d_frames;
    S.draws = fr > last_frames_for_draws ? (int)((d - last_draws) / (fr - last_frames_for_draws)) : 0;
    last_draws = d; last_frames_for_draws = fr;
    sample(t, last_sample > 0 ? t - last_sample : 0);
    last_sample = t;
  }
  if (!g_osd_mode || !hb_active()) return;
  int W, H;
  if (!hb_osd_begin(&W, &H)) return;
  float size = H * 0.027f; /* about 58 px on a 4K picture */
  if (size < 14) size = 14;
  float x0 = size * 1.0f, y0 = size * 0.8f, line = size * 1.22f, col1 = size * 4.6f;

  if (g_osd_mode == 1) { /* basic: one line */
    char v[32], d[32];
    snprintf(v, sizeof v, "%.0f FPS", S.fps);
    snprintf(d, sizeof d, "%.1f ms", S.ms);
    float w = hb_osd_text_width(size, v) + size * 0.5f + hb_osd_text_width(size * DETAIL, d);
    hb_osd_rect(x0 - size * 0.4f, y0 - size * 0.25f, w + size * 0.8f, size * 1.5f, C_PANEL);
    float x = x0 + hb_osd_text(x0, y0, size, v, fps_color(S.fps));
    hb_osd_text(x + size * 0.5f, y0 + size * 0.12f, size * DETAIL, d, C_DIM);
    hb_osd_end();
    return;
  }

  /* detailed */
  Row r[8];
  int n = 0;
  memset(r, 0, sizeof r);
  r[n].label = "FPS"; snprintf(r[n].value, 32, "%.1f", S.fps); r[n].vcolor = fps_color(S.fps);
  snprintf(r[n].detail, 96, "1%% low %.0f", S.low1); r[n].dcolor = S.low1 > 0 ? fps_color(S.low1) : C_DIM; n++;
  r[n].label = "FRAME"; snprintf(r[n].value, 32, "%.1f ms", S.ms); r[n].vcolor = ms_color((float)S.ms);
  snprintf(r[n].detail, 96, "cap %d", g_frame_cap); r[n].dcolor = C_DIM; n++;
  r[n].graph = 1; n++;
  r[n].label = "CPU"; snprintf(r[n].value, 32, "%d%%", S.main_pct);
  r[n].vcolor = S.main_pct >= 95 ? C_BAD : S.main_pct >= 80 ? C_WARN : C_VALUE;
  int o = snprintf(r[n].detail, 96, "main thread; all %.1f of %d cores", S.total_pct / 100.0, S.cores);
  if (S.temp_cpu >= 0) snprintf(r[n].detail + o, 96 - o, "; %d\x80" "C", S.temp_cpu);
  r[n].dcolor = C_DIM; n++;
  r[n].label = "GPU";
  if (S.gpu >= 0) snprintf(r[n].value, 32, "%d%%", S.gpu); else snprintf(r[n].value, 32, "--");
  r[n].vcolor = S.gpu >= 95 ? C_WARN : C_VALUE;
  o = 0;
  if (S.compiling) o += snprintf(r[n].detail, 96, "compiling %d shader%s", S.compiling, S.compiling == 1 ? "" : "s");
  if (S.temp_soc >= 0) snprintf(r[n].detail + o, 96 - o, "%s%d\x80" "C", o ? "; " : "", S.temp_soc);
  r[n].dcolor = S.compiling ? C_WARN : C_DIM; n++;
  r[n].label = "VRAM";
  if (S.vram_total) { snprintf(r[n].value, 32, "%u MB", S.vram_used); snprintf(r[n].detail, 96, "of %u MB", S.vram_total); }
  else snprintf(r[n].value, 32, "--");
  r[n].vcolor = C_VALUE; r[n].dcolor = C_DIM; n++;
  r[n].label = "RAM"; snprintf(r[n].value, 32, "%u MB", S.ram_game);
#ifdef __PROSPERO__
  snprintf(r[n].detail, 96, "game; %u MB free", S.ram_free);
#else
  snprintf(r[n].detail, 96, "game; process %u MB", S.ram_proc);
#endif
  r[n].vcolor = C_VALUE; r[n].dcolor = C_DIM; n++;
  DevState *D = d3d_dev();
  r[n].label = "VIDEO"; snprintf(r[n].value, 32, "%ux%u", D ? D->pp[0] : 0, D ? D->pp[1] : 0);
  snprintf(r[n].detail, 96, "%d draws", S.draws); r[n].vcolor = C_VALUE; r[n].dcolor = C_DIM; n++;

  float width = size * 15, graph_h = size * 3.2f, height = 0;
  for (int i = 0; i < n; i++) {
    float w = row_width(&r[i], size, col1);
    if (w > width) width = w;
    height += r[i].graph ? graph_h : line;
  }
  hb_osd_rect(x0 - size * 0.5f, y0 - size * 0.35f, width + size, height + size * 0.5f, C_PANEL);
  float y = y0;
  for (int i = 0; i < n; i++) {
    if (r[i].graph) { /* frame times, 0-50 ms; lines at 16.7 (60 fps) and 33.3 (30 fps) */
      float gx = x0, gy = y + size * 0.1f, gw = width, gh = graph_h - size * 0.5f, top = 50;
      hb_osd_rect(gx, gy, gw, gh, 0x50000000u);
      hb_osd_rect(gx, gy + gh - gh * 16.7f / top, gw, size * 0.06f + 1, 0x7066FF66u);
      hb_osd_rect(gx, gy + gh - gh * 33.3f / top, gw, size * 0.06f + 1, 0x70FFE04Au);
      float bw = gw / HIST;
      for (int k = 0; k < HIST; k++) {
        float ms = g_hist[(g_hist_i + k) % HIST];
        if (ms <= 0) continue;
        float bh = gh * (ms > top ? top : ms) / top;
        hb_osd_rect(gx + k * bw, gy + gh - bh, bw + 0.6f, bh, ms_color(ms) & 0xE0FFFFFFu);
      }
      y += graph_h;
      continue;
    }
    hb_osd_text(x0, y, size, r[i].label, C_LABEL);
    float w = hb_osd_text(x0 + col1, y, size, r[i].value, r[i].vcolor);
    if (r[i].detail[0]) hb_osd_text(x0 + col1 + w + size * 0.35f, y + size * (1 - DETAIL) * 0.75f, size * DETAIL, r[i].detail, r[i].dcolor);
    y += line;
  }
  hb_osd_end();
}
