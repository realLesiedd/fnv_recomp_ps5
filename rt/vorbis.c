/* libvorbisfile for the game's Ogg stream wrapper (0xAF1D80-0xAF22A0), decoded with stb_vorbis
 * (third_party/stb_vorbis.c by Sean Barrett, public domain / MIT).
 *
 * The game embeds the 720-byte OggVorbis_File inside its own stream object and only touches it through
 * these six calls, so we keep our handle in it. At open time the whole compressed stream is pulled in
 * through the game's read callback (voices and effects are small); decoding is incremental. The game passes
 * no close callback, so ov_clear only frees the decoder. */
#include "rt.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"
#pragma clang diagnostic pop

#define API(n) static void n(Ctx *c)
#define OV_MAGIC 0x564F4E46u /* "FNOV" */
#define OV_EREAD (-128)
#define OV_EINVAL (-131)
#define OV_ENOTVORBIS (-132)
#define MAX_STREAMS 256
#define READ_CHUNK (1u << 16)
#define MAX_FILE (64u << 20)

typedef struct { stb_vorbis *v; u8 *data; int channels; } Stream;
static Stream g_st[MAX_STREAMS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static Stream *get(u32 vf) {
  if (!vf || M32(vf) != OV_MAGIC) return NULL;
  u32 i = M32(vf + 4);
  return i < MAX_STREAMS && g_st[i].v ? &g_st[i] : NULL;
}

/* int ov_open_callbacks(void *src, OggVorbis_File *vf, const char *initial, long ibytes,
 *                       ov_callbacks {read, seek, close, tell})   cdecl, callbacks passed by value */
API(ov_open_callbacks) {
  u32 src = ARG(0), vf = ARG(1), initial = ARG(2), ibytes = ARG(3), rd = ARG(4);
  memset(HP(vf), 0, 720);
  size_t cap = READ_CHUNK, len = 0;
  u8 *data = malloc(cap);
  if (initial && ibytes && ibytes < MAX_FILE) {
    while (cap < ibytes) cap *= 2;
    data = realloc(data, cap);
    memcpy(data, HP(initial), ibytes);
    len = ibytes;
  }
  u32 buf = heap_alloc(READ_CHUNK, 0);
  for (;;) {
    u32 a[4] = {buf, 1, READ_CHUNK, src}; /* size_t read(void *ptr, size_t size, size_t n, void *src) */
    u32 n = rt_call_guest(c, rd, 4, a);
    if (!n || n > READ_CHUNK) break;
    if (len + n > cap) { while (len + n > cap) cap *= 2; data = realloc(data, cap); }
    memcpy(data + len, HP(buf), n);
    len += n;
    if (len > MAX_FILE) break;
  }
  heap_free(buf);

  int err = 0;
  stb_vorbis *v = len ? stb_vorbis_open_memory(data, (int)len, &err, NULL) : NULL;
  if (!v) {
    free(data);
    rt_log("ov_open_callbacks: not a Vorbis stream (%zu bytes, decoder error %d)", len, err);
    RETURN_CDECL(len ? OV_ENOTVORBIS : OV_EREAD);
  }
  stb_vorbis_info in = stb_vorbis_get_info(v);
  pthread_mutex_lock(&g_lock);
  u32 i = 0;
  while (i < MAX_STREAMS && g_st[i].v) i++;
  if (i < MAX_STREAMS) g_st[i] = (Stream){v, data, in.channels};
  pthread_mutex_unlock(&g_lock);
  if (i == MAX_STREAMS) {
    stb_vorbis_close(v);
    free(data);
    rt_log("ov_open_callbacks: more than %d streams open", MAX_STREAMS);
    RETURN_CDECL(OV_EREAD);
  }
  M32(vf) = OV_MAGIC;
  M32(vf + 4) = i;
  /* vorbis_info {version, channels, rate, bitrate_upper, _nominal, _lower, _window, codec_setup} for ov_info */
  M32(vf + 0x24) = in.channels;
  M32(vf + 0x28) = in.sample_rate;
  static int logged;
  if (logged < 2) { logged++; rt_log("ogg: opened a %d-channel %u Hz stream (%zu bytes)", in.channels, in.sample_rate, len); }
  RETURN_CDECL(0);
}

/* vorbis_info *ov_info(OggVorbis_File *vf, int link) */
API(ov_info) { RETURN_CDECL(get(ARG(0)) ? ARG(0) + 0x20 : 0); }

/* long ov_seekable(OggVorbis_File *vf) */
API(ov_seekable) { RETURN_CDECL(get(ARG(0)) ? 1 : 0); }

/* ogg_int64_t ov_pcm_total(OggVorbis_File *vf, int i): samples per channel */
API(ov_pcm_total) {
  Stream *s = get(ARG(0));
  if (!s) { c->r[2] = 0xffffffffu; RETURN_CDECL(OV_EINVAL); }
  c->r[2] = 0;
  RETURN_CDECL(stb_vorbis_stream_length_in_samples(s->v));
}

/* long ov_read(OggVorbis_File *vf, char *buf, int length, int bigendianp, int word, int sgned, int *bitstream)
 * The game asks for little-endian signed 16-bit, interleaved. Returns bytes, 0 at the end. */
API(ov_read) {
  Stream *s = get(ARG(0));
  u32 out = ARG(1), length = ARG(2);
  if (!s) RETURN_CDECL(OV_EINVAL);
  if (ARG(3) || ARG(4) != 2 || !ARG(5)) {
    static int warned;
    if (!warned++) rt_log("ov_read: unexpected format (bigendian %u, word %u, signed %u)", ARG(3), ARG(4), ARG(5));
  }
  static __thread short *tmp;
  static __thread u32 tmp_cap;
  int nshort = (int)(length / 2);
  nshort -= nshort % s->channels;
  if (nshort <= 0) RETURN_CDECL(0);
  if ((u32)nshort > tmp_cap) { tmp_cap = nshort; tmp = realloc(tmp, tmp_cap * sizeof(short)); }
  int frames = stb_vorbis_get_samples_short_interleaved(s->v, s->channels, tmp, nshort);
  u32 bytes = (u32)frames * s->channels * 2;
  memcpy(HP(out), tmp, bytes);
  if (ARG(6)) M32(ARG(6)) = 0;
  RETURN_CDECL(bytes);
}

/* int ov_clear(OggVorbis_File *vf) */
API(ov_clear) {
  Stream *s = get(ARG(0));
  if (s) {
    stb_vorbis_close(s->v);
    free(s->data);
    pthread_mutex_lock(&g_lock);
    s->v = NULL;
    pthread_mutex_unlock(&g_lock);
    M32(ARG(0)) = 0;
  }
  RETURN_CDECL(0);
}

static const ApiEntry list[] = {
  {"libvorbisfile.dll", "ov_open_callbacks", ov_open_callbacks},
  {"libvorbisfile.dll", "ov_info", ov_info},
  {"libvorbisfile.dll", "ov_seekable", ov_seekable},
  {"libvorbisfile.dll", "ov_pcm_total", ov_pcm_total},
  {"libvorbisfile.dll", "ov_read", ov_read},
  {"libvorbisfile.dll", "ov_clear", ov_clear},
  {0}};
void vorbisfile_init(void) { api_register(list); }
