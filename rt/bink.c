/* binkw32 (RAD Bink 1 video), played with FFmpeg's Bink decoders: the intro movie (FNVIntro.bik, started by the
 * master file's script on New Game).
 *
 * The game's movie player (around 0xEC0C00) opens the movie, then each frame: BinkWait until the frame is due,
 * BinkDoFrame, BinkCopyToBufferRect (32-bit BGRx into a locked texture, possibly in several rectangles),
 * BinkNextFrame. It reads Width (+0), Height (+4) and Frames (+8) from the BINK struct and stops when its own
 * frame count reaches Frames. Bink plays the sound itself, so the movie's audio goes straight to our mixer
 * (dsound.c calls bink_mix).
 *
 * The container is parsed here (a header, a frame index, and per frame: one audio packet per track, then the
 * video packet), so only the two decoders are linked from FFmpeg. */
#include "rt.h"
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define API(n) static void n(Ctx *c)

#ifdef __PROSPERO__
/* the PS5 toolchain's static FFmpeg: name the decoders directly so the codec list (and with it every codec
   FFmpeg was built with) is not linked in */
extern const AVCodec ff_bink_decoder, ff_binkaudio_dct_decoder, ff_binkaudio_rdft_decoder;
#define BINK_VIDEO_DECODER (&ff_bink_decoder)
#define BINK_AUDIO_DECODER(dct) ((dct) ? &ff_binkaudio_dct_decoder : &ff_binkaudio_rdft_decoder)
/* libavcodec's own lookups (allcodecs.o) list every codec it was built with, x264 included; these stand in for
   them, so that object is never linked */
static const AVCodec *const g_codecs[] = {&ff_bink_decoder, &ff_binkaudio_dct_decoder, &ff_binkaudio_rdft_decoder};
const AVCodec *av_codec_iterate(void **opaque) {
  uintptr_t i = (uintptr_t)*opaque;
  if (i >= sizeof g_codecs / sizeof *g_codecs) return NULL;
  *opaque = (void *)(i + 1);
  return g_codecs[i];
}
const AVCodec *avcodec_find_decoder(enum AVCodecID id) {
  for (unsigned i = 0; i < sizeof g_codecs / sizeof *g_codecs; i++)
    if (g_codecs[i]->id == id) return g_codecs[i];
  return NULL;
}
const AVCodec *avcodec_find_decoder_by_name(const char *name) {
  for (unsigned i = 0; name && i < sizeof g_codecs / sizeof *g_codecs; i++)
    if (!strcmp(g_codecs[i]->name, name)) return g_codecs[i];
  return NULL;
}
const AVCodec *avcodec_find_encoder(enum AVCodecID id) { (void)id; return NULL; }
const AVCodec *avcodec_find_encoder_by_name(const char *name) { (void)name; return NULL; }
#else
#define BINK_VIDEO_DECODER avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO)
#define BINK_AUDIO_DECODER(dct) avcodec_find_decoder((dct) ? AV_CODEC_ID_BINKAUDIO_DCT : AV_CODEC_ID_BINKAUDIO_RDFT)
#endif

#define AUD_USEDCT 0x1000
#define AUD_STEREO 0x2000
#define RING (48000 * 4) /* stereo frames of decoded sound held ahead of playback: 4 s */

typedef struct {
  u32 guest;                      /* the BINK struct the game sees */
  FILE *f;
  u32 tag, frames, w, h, fps_num, fps_den;
  u32 *index;                     /* frames + 1 file offsets */
  u32 cur;                        /* frame BinkDoFrame decodes next (0-based) */
  int ntracks, rate, channels;
  AVCodecContext *vdec, *adec;
  AVFrame *vf, *af;
  AVPacket *pkt;
  u32 *rgb;                       /* the current picture as BGRx, w x h */
  unsigned serial;                /* bumped for every new picture */
  double t0, paused_at;           /* playback clock: frame n is due at t0 + n / fps */
  int started, paused;
  int16_t *ring;                  /* sound, stereo, int16 */
  u32 rd, wr;                     /* frames read / written (wrap at RING) */
  double frac;
  int16_t prev[2], cur_s[2];
  u8 *buf; u32 bufsz;
  u64 decode_us, frames_done;
} Movie;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Movie *g_mov[8];

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static Movie *find(u32 g) {
  for (int i = 0; i < 8; i++)
    if (g_mov[i] && g_mov[i]->guest == g) return g_mov[i];
  return NULL;
}

static void close_movie(Movie *m) {
  if (!m) return;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < 8; i++)
    if (g_mov[i] == m) g_mov[i] = NULL;
  pthread_mutex_unlock(&g_lock);
  avcodec_free_context(&m->vdec);
  avcodec_free_context(&m->adec);
  av_frame_free(&m->vf);
  av_frame_free(&m->af);
  av_packet_free(&m->pkt);
  if (m->f) fclose(m->f);
  if (m->guest) heap_free(m->guest);
  free(m->index);
  free(m->ring);
  free(m->buf);
  free(m->rgb);
  free(m);
}

static AVCodecContext *open_decoder(const AVCodec *codec, Movie *m, int audio, int flags) {
  if (!codec) return NULL;
  AVCodecContext *c = avcodec_alloc_context3(codec);
  if (!c) return NULL;
  c->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
  c->extradata_size = 4;
  if (audio) {
    /* as FFmpeg's demuxer: the video codec tag (the decoder checks it for revision 'b') */
    memcpy(c->extradata, &m->tag, 4);
    c->sample_rate = m->rate;
    av_channel_layout_default(&c->ch_layout, (flags & AUD_STEREO) ? 2 : 1);
  } else {
    memcpy(c->extradata, &flags, 4); /* the video flags from the header */
    c->codec_tag = m->tag;           /* 'BIKi': the decoder takes the revision from it */
    c->width = m->w;
    c->height = m->h;
  }
  c->thread_count = 1;
  if (avcodec_open2(c, codec, NULL) < 0) { avcodec_free_context(&c); return NULL; }
  return c;
}

/* ---- sound: decoded ahead into a ring, played by the mixer --------------------------------------------- */
static void push_audio(Movie *m, const AVFrame *fr) {
  int n = fr->nb_samples, ch = fr->ch_layout.nb_channels;
  int planar = av_sample_fmt_is_planar(fr->format);
  for (int i = 0; i < n; i++) {
    float l, r;
    if (planar) {
      l = ((const float *)fr->extended_data[0])[i];
      r = ((const float *)fr->extended_data[ch > 1 ? 1 : 0])[i];
    } else {
      l = ((const float *)fr->extended_data[0])[i * ch];
      r = ((const float *)fr->extended_data[0])[i * ch + (ch > 1)];
    }
    l = l > 1.f ? 1.f : l < -1.f ? -1.f : l;
    r = r > 1.f ? 1.f : r < -1.f ? -1.f : r;
    pthread_mutex_lock(&g_lock);
    if (m->wr - m->rd >= RING) m->rd++; /* full: drop the oldest */
    m->ring[2 * (m->wr % RING)] = (int16_t)(l * 32767.f);
    m->ring[2 * (m->wr % RING) + 1] = (int16_t)(r * 32767.f);
    m->wr++;
    pthread_mutex_unlock(&g_lock);
  }
}

void bink_mix(float *acc, int frames, int out_rate) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < 8; i++) {
    Movie *m = g_mov[i];
    if (!m || !m->ring || !m->started || m->paused) continue;
    double step = (double)m->rate / out_rate;
    for (int k = 0; k < frames; k++) {
      while (m->frac >= 1.0) {
        if (m->rd == m->wr) goto next; /* nothing decoded yet: silence */
        m->prev[0] = m->cur_s[0];
        m->prev[1] = m->cur_s[1];
        m->cur_s[0] = m->ring[2 * (m->rd % RING)];
        m->cur_s[1] = m->ring[2 * (m->rd % RING) + 1];
        m->rd++;
        m->frac -= 1.0;
      }
      float t = (float)m->frac;
      acc[2 * k] += m->prev[0] + (m->cur_s[0] - m->prev[0]) * t;
      acc[2 * k + 1] += m->prev[1] + (m->cur_s[1] - m->prev[1]) * t;
      m->frac += step;
    }
  next:;
  }
  pthread_mutex_unlock(&g_lock);
}

/* ---- the API ------------------------------------------------------------------------------------------- */
API(bk_BinkOpen) {
  const char *name = ARG(0) ? STR(ARG(0)) : "";
  u32 flags = ARG(1);
  char hp[2048];
  Movie *m = calloc(1, sizeof *m);
  u8 h[44];
  if (flags & 0x00800000) { rt_log("Bink: open by file handle is not supported (%s)", name); goto fail; }
  fs_host_path(name, hp, sizeof hp);
  if (!(m->f = fopen(hp, "rb")) || fread(h, 1, 44, m->f) != 44) { rt_log("Bink: cannot open %s", hp); goto fail; }
  m->tag = rd32(h);
  if ((m->tag & 0xFFFFFF) != 0x4B4942) { rt_log("Bink: %s is not a Bink 1 movie", name); goto fail; } /* "BIK" */
  m->frames = rd32(h + 8);
  m->w = rd32(h + 20);
  m->h = rd32(h + 24);
  m->fps_num = rd32(h + 28);
  m->fps_den = rd32(h + 32);
  u32 vflags = rd32(h + 36);
  m->ntracks = (int)rd32(h + 40);
  if (!m->frames || m->frames > 1000000 || !m->fps_num || !m->fps_den || m->ntracks > 256) goto fail;
  if ((m->tag >> 24) == 'k') fseek(m->f, 4, SEEK_CUR); /* an extra header field in revision 'k' */
  int aflags = 0;
  if (m->ntracks) {
    u8 t[4];
    fseek(m->f, 4 * m->ntracks, SEEK_CUR); /* max decoded sizes */
    for (int i = 0; i < m->ntracks; i++) {
      if (fread(t, 1, 4, m->f) != 4) goto fail;
      if (i == 0) { m->rate = t[0] | t[1] << 8; aflags = t[2] | t[3] << 8; } /* only the first track is played */
    }
    fseek(m->f, 4 * m->ntracks, SEEK_CUR); /* track ids */
  }
  m->index = malloc(4 * (m->frames + 1));
  if (fread(m->index, 4, m->frames, m->f) != m->frames) goto fail;
  fseek(m->f, 0, SEEK_END);
  m->index[m->frames] = (u32)ftell(m->f);
  m->vdec = open_decoder(BINK_VIDEO_DECODER, m, 0, (int)vflags);
  if (!m->vdec) { rt_log("Bink: no video decoder"); goto fail; }
  if (m->ntracks && m->rate) {
    m->adec = open_decoder(BINK_AUDIO_DECODER(aflags & AUD_USEDCT), m, 1, aflags);
    if (m->adec) m->ring = calloc(RING, 4);
    else rt_log("Bink: no audio decoder, playing without sound");
  }
  m->vf = av_frame_alloc();
  m->af = av_frame_alloc();
  m->pkt = av_packet_alloc();
  /* BINK: Width, Height, Frames, FrameNum, LastFrameNum, FrameRate, FrameRateDiv, ReadError, OpenFlags, ... */
  m->guest = heap_alloc(0x400, 1);
  M32(m->guest + 0) = m->w;
  M32(m->guest + 4) = m->h;
  M32(m->guest + 8) = m->frames;
  M32(m->guest + 12) = 1;
  M32(m->guest + 20) = m->fps_num;
  M32(m->guest + 24) = m->fps_den;
  M32(m->guest + 32) = flags;
  pthread_mutex_lock(&g_lock);
  int slot = -1;
  for (int i = 0; i < 8 && slot < 0; i++)
    if (!g_mov[i]) slot = i;
  if (slot >= 0) g_mov[slot] = m;
  pthread_mutex_unlock(&g_lock);
  if (slot < 0) goto fail;
  rt_log("Bink: playing %s: %ux%u, %u frames at %.2f fps (%.0f s), sound %d Hz %s%s", name, m->w, m->h, m->frames,
         (double)m->fps_num / m->fps_den, (double)m->frames * m->fps_den / m->fps_num, m->rate,
         (aflags & AUD_STEREO) ? "stereo" : "mono", m->adec ? "" : " (not played)");
  RETURN_STD(2, m->guest);
fail:
  if (m->guest) { heap_free(m->guest); m->guest = 0; }
  close_movie(m);
  RETURN_STD(2, 0);
}

static void start_clock(Movie *m) {
  if (m->started) return;
  m->started = 1;
  m->t0 = now_s();
}

/* the decoded picture (YUV 4:2:0) as BGRx: BT.601, limited range (Bink 1 before revision 'k', which is full range) */
static inline u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }
static void to_bgrx(Movie *m) {
  AVFrame *f = m->vf;
  if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVA420P) {
    static int warned;
    if (!warned++) rt_log("Bink: unexpected picture format %d", f->format);
    return;
  }
  if (!m->rgb) m->rgb = malloc((size_t)m->w * m->h * 4);
  if (!m->rgb) return;
  int full = (m->tag >> 24) == 'k';
  int ky = full ? 256 : 298, kv_r = full ? 359 : 409, ku_g = full ? 88 : 100, kv_g = full ? 183 : 208, ku_b = full ? 454 : 516;
  int yoff = full ? 0 : 16;
  u32 w = m->w < (u32)f->width ? m->w : (u32)f->width, h = m->h < (u32)f->height ? m->h : (u32)f->height;
  for (u32 y = 0; y < h; y++) {
    const u8 *Y = f->data[0] + (size_t)y * f->linesize[0];
    const u8 *U = f->data[1] + (size_t)(y >> 1) * f->linesize[1];
    const u8 *V = f->data[2] + (size_t)(y >> 1) * f->linesize[2];
    u8 *d = (u8 *)(m->rgb + (size_t)y * m->w);
    for (u32 x = 0; x < w; x++) {
      int c = (Y[x] - yoff) * ky, u = U[x >> 1] - 128, v = V[x >> 1] - 128;
      d[4 * x + 0] = clamp8((c + ku_b * u + 128) >> 8);
      d[4 * x + 1] = clamp8((c - ku_g * u - kv_g * v + 128) >> 8);
      d[4 * x + 2] = clamp8((c + kv_r * v + 128) >> 8);
      d[4 * x + 3] = 255;
    }
  }
  m->serial++;
}

/* decode frame m->cur: its sound packets, then its picture */
API(bk_BinkDoFrame) {
  Movie *m = find(ARG(0));
  if (!m || m->cur >= m->frames) RETURN_STD(1, 0);
  start_clock(m);
  double t = now_s();
  u32 pos = m->index[m->cur] & ~1u, end = m->index[m->cur + 1] & ~1u;
  if (end <= pos || end - pos > 64u << 20) RETURN_STD(1, 0);
  u32 size = end - pos;
  if (size + AV_INPUT_BUFFER_PADDING_SIZE > m->bufsz) {
    m->bufsz = size + AV_INPUT_BUFFER_PADDING_SIZE;
    m->buf = realloc(m->buf, m->bufsz);
  }
  fseek(m->f, pos, SEEK_SET);
  if (fread(m->buf, 1, size, m->f) != size) { M32(m->guest + 28) = 1; RETURN_STD(1, 0); }
  memset(m->buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  u32 off = 0;
  for (int i = 0; i < m->ntracks && off + 4 <= size; i++) {
    u32 asz = rd32(m->buf + off);
    off += 4;
    if (asz > size - off) break;
    if (i == 0 && m->adec && asz >= 4) {
      av_packet_unref(m->pkt);
      if (av_new_packet(m->pkt, (int)asz) == 0) {
        memcpy(m->pkt->data, m->buf + off, asz);
        if (avcodec_send_packet(m->adec, m->pkt) == 0)
          while (avcodec_receive_frame(m->adec, m->af) == 0) push_audio(m, m->af);
      }
    }
    off += asz;
  }
  av_packet_unref(m->pkt);
  if (off < size && av_new_packet(m->pkt, (int)(size - off)) == 0) {
    memcpy(m->pkt->data, m->buf + off, size - off);
    if ((m->index[m->cur] & 1) || m->cur == 0) m->pkt->flags |= AV_PKT_FLAG_KEY;
    if (avcodec_send_packet(m->vdec, m->pkt) == 0 && avcodec_receive_frame(m->vdec, m->vf) == 0) to_bgrx(m);
  }
  m->decode_us += (u64)((now_s() - t) * 1e6);
  m->frames_done++;
  if (m->frames_done % 300 == 0)
    rt_log("Bink: frame %u/%u, decoding %.1f ms per frame, %.1f s of sound buffered", m->cur + 1, m->frames,
           m->decode_us / 1e3 / m->frames_done, (double)(m->wr - m->rd) / (m->rate ? m->rate : 1));
  RETURN_STD(1, 0);
}

API(bk_BinkNextFrame) {
  Movie *m = find(ARG(0));
  if (m && m->cur < m->frames) {
    m->cur++;
    M32(m->guest + 16) = M32(m->guest + 12);
    M32(m->guest + 12) = m->cur + 1 <= m->frames ? m->cur + 1 : m->frames;
  }
  RETURN_STD(1, 0);
}

/* nonzero: the next frame is not due yet */
API(bk_BinkWait) {
  Movie *m = find(ARG(0));
  if (!m || m->paused) RETURN_STD(1, m ? 1 : 0);
  start_clock(m);
  double due = m->t0 + (double)m->cur * m->fps_den / m->fps_num;
  RETURN_STD(1, now_s() < due ? 1 : 0);
}

API(bk_BinkPause) {
  Movie *m = find(ARG(0));
  int p = ARG(1) != 0;
  if (m && p != m->paused) {
    if (p) m->paused_at = now_s();
    else if (m->started) m->t0 += now_s() - m->paused_at; /* resume where it stopped */
    m->paused = p;
  }
  RETURN_STD(2, p);
}

API(bk_BinkClose) {
  Movie *m = find(ARG(0));
  if (m) {
    rt_log("Bink: closed after %u of %u frames", m->cur, m->frames);
    close_movie(m);
  }
  RETURN_STD(1, 0);
}

/* (bink, dest, destpitch, destheight, destx, desty, srcx, srcy, srcw, srch, flags): the source rectangle of the
   current picture to dest at (destx, desty), as 32-bit BGRx (surface type 3, the only one the game asks for; the
   game tiles the movie into 256x256 textures). Returns 0 when copied. */
API(bk_BinkCopyToBufferRect) {
  Movie *m = find(ARG(0));
  u32 dest = ARG(1), pitch = ARG(2), dh = ARG(3), dx = ARG(4), dy = ARG(5);
  u32 sx = ARG(6), sy = ARG(7), sw = ARG(8), sh = ARG(9), type = ARG(10) & 0xF;
  if (!m || !m->rgb || !dest) RETURN_STD(11, 1);
  static int warned;
  if (type != 3 && type != 4 && !warned++) rt_log("Bink: copy to surface type %u treated as 32-bit BGRx", type);
  if (sx >= m->w || sy >= m->h) RETURN_STD(11, 0);
  if (sx + sw > m->w) sw = m->w - sx;
  if (sy + sh > m->h) sh = m->h - sy;
  if (dy + sh > dh) sh = dh > dy ? dh - dy : 0;
  for (u32 y = 0; y < sh; y++) memcpy(HP(dest + (dy + y) * pitch + dx * 4), m->rgb + (size_t)(sy + y) * m->w + sx, (size_t)sw * 4);
  RETURN_STD(11, 0);
}

/* The playing movie's newest picture, for the GPU backend, which draws it over the whole frame before presenting
   (the game's own textured tiles came out black on the GPU path). 0: no movie showing. */
int bink_picture(const u32 **px, int *w, int *h, unsigned *serial) {
  for (int i = 0; i < 8; i++) {
    Movie *m = g_mov[i];
    if (m && m->rgb && m->started) { *px = m->rgb; *w = (int)m->w; *h = (int)m->h; *serial = m->serial; return 1; }
  }
  return 0;
}

API(bk_BinkSetSoundSystem) { RETURN_STD(2, 1); }
API(bk_BinkOpenDirectSound) { RETURN_STD(1, 1); }

static const ApiEntry list[] = {
  {"binkw32.dll", "_BinkOpen@8", bk_BinkOpen}, {"binkw32.dll", "_BinkSetSoundSystem@8", bk_BinkSetSoundSystem},
  {"binkw32.dll", "_BinkOpenDirectSound@4", bk_BinkOpenDirectSound}, {"binkw32.dll", "_BinkClose@4", bk_BinkClose},
  {"binkw32.dll", "_BinkNextFrame@4", bk_BinkNextFrame}, {"binkw32.dll", "_BinkWait@4", bk_BinkWait},
  {"binkw32.dll", "_BinkPause@8", bk_BinkPause}, {"binkw32.dll", "_BinkDoFrame@4", bk_BinkDoFrame},
  {"binkw32.dll", "_BinkCopyToBufferRect@44", bk_BinkCopyToBufferRect},
  {0}};
void bink_init(void) { api_register(list); }
