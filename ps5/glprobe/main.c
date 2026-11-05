/* GPU probe for jailbroken PS5s: does Sony's OpenGL ES (libSceGLSlimVSH) work from a
 * homebrew payload?  Draws offscreen, reads the pixels back and times a draw loop.
 * Results: /data/fnv_recomp/glprobe.log and a notification on screen.
 *
 * Build: make -C ps5/glprobe   (needs PS5_PAYLOAD_SDK)
 * Run:   send glprobe.elf to the ELF loader (port 9021), e.g. with socat or a payload sender app. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int sceNotificationSend(int userId, bool isLogged, const char *payload);

static FILE *g_log;
static char g_summary[256] = "GL probe: started";

static void logf_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  fputc('\n', g_log);
  fflush(g_log);
  va_end(ap);
}

static void notify(const char *msg) {
  char buf[1024];
  snprintf(buf, sizeof buf,
           "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
           "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
           "\"viewData\":{\"message\":{\"body\":\"%s\"}},\"platformViews\":{\"previewDisabled\":{\"viewData\":"
           "{\"message\":{\"body\":\"%s\"}}}}},\"createdDateTime\":\"2024-01-01T00:00:00.000Z\",\"expirationDuration\":0,"
           "\"queueId\":0,\"localNotificationId\":0,\"appendTimestamp\":false}",
           msg, msg);
  sceNotificationSend(0xFE, true, buf);
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  char log[1024] = "";
  glGetShaderInfoLog(s, sizeof log, NULL, log);
  logf_("compile %s shader: %s %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", ok ? "OK" : "FAILED", log);
  return ok ? s : 0;
}

static int probe(void) {
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  logf_("eglGetDisplay = %p", (void *)dpy);
  EGLint maj = 0, min = 0;
  if (!eglInitialize(dpy, &maj, &min)) { logf_("eglInitialize failed: 0x%x", eglGetError()); snprintf(g_summary, sizeof g_summary, "GL probe: eglInitialize failed 0x%x", eglGetError()); return 1; }
  logf_("EGL %d.%d vendor=%s version=%s apis=%s", maj, min, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION),
        eglQueryString(dpy, EGL_CLIENT_APIS));
  logf_("EGL extensions: %s", eglQueryString(dpy, EGL_EXTENSIONS));
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cattr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                          EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!eglChooseConfig(dpy, cattr, &cfg, 1, &n) || n < 1) {
    logf_("eglChooseConfig: no pbuffer ES2 config (0x%x); trying window configs", eglGetError());
    const EGLint c2[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    if (!eglChooseConfig(dpy, c2, &cfg, 1, &n) || n < 1) { snprintf(g_summary, sizeof g_summary, "GL probe: no EGL config"); return 1; }
  }
  EGLint total = 0;
  eglGetConfigs(dpy, NULL, 0, &total);
  logf_("%d EGL configs", total);
  const EGLint pattr[] = {EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE};
  EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pattr);
  logf_("eglCreatePbufferSurface = %p (0x%x)", (void *)surf, eglGetError());
  const EGLint xattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xattr);
  logf_("eglCreateContext = %p (0x%x)", (void *)ctx, eglGetError());
  if (ctx == EGL_NO_CONTEXT) { snprintf(g_summary, sizeof g_summary, "GL probe: eglCreateContext failed"); return 1; }
  if (!eglMakeCurrent(dpy, surf, surf, ctx)) { logf_("eglMakeCurrent failed 0x%x", eglGetError()); snprintf(g_summary, sizeof g_summary, "GL probe: eglMakeCurrent failed"); return 1; }
  logf_("GL_VENDOR=%s", glGetString(GL_VENDOR));
  logf_("GL_RENDERER=%s", glGetString(GL_RENDERER));
  logf_("GL_VERSION=%s", glGetString(GL_VERSION));
  logf_("GL_SHADING_LANGUAGE_VERSION=%s", glGetString(GL_SHADING_LANGUAGE_VERSION));
  logf_("GL_EXTENSIONS=%s", glGetString(GL_EXTENSIONS));

  /* if the pbuffer failed, render into a framebuffer object instead */
  GLuint fbo = 0, tex = 0;
  if (surf == EGL_NO_SURFACE) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    logf_("FBO status 0x%x", glCheckFramebufferStatus(GL_FRAMEBUFFER));
  }
  GLuint vs = compile(GL_VERTEX_SHADER, "attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }");
  GLuint fs = compile(GL_FRAGMENT_SHADER, "precision mediump float; void main(){ gl_FragColor = vec4(1.0, 0.5, 0.25, 1.0); }");
  if (!vs || !fs) { snprintf(g_summary, sizeof g_summary, "GL probe: context OK, shader compile FAILED (see log)"); return 1; }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glBindAttribLocation(prog, 0, "p");
  glLinkProgram(prog);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  logf_("link: %s", ok ? "OK" : "FAILED");
  glUseProgram(prog);
  static const float tri[] = {-1, -1, 3, -1, -1, 3};
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
  glEnableVertexAttribArray(0);
  glViewport(0, 0, 256, 256);
  glClearColor(0, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glFinish();
  unsigned char px[4] = {0};
  glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  logf_("glReadPixels center = %u %u %u %u (expected 255 128 64 255), glGetError=0x%x", px[0], px[1], px[2], px[3], glGetError());
  int drew = px[0] > 200 && px[1] > 100 && px[1] < 160 && px[2] < 100;
  double t0 = now();
  for (int i = 0; i < 2000; i++) glDrawArrays(GL_TRIANGLES, 0, 3);
  glFinish();
  double dt = now() - t0;
  logf_("2000 full-target draws in %.3f s (%.1f us per draw)", dt, dt * 1e6 / 2000);
  snprintf(g_summary, sizeof g_summary, "GL probe: %s, %s, %.1f us/draw", drew ? "GPU DRAW OK" : "draw result WRONG",
           (const char *)glGetString(GL_RENDERER), dt * 1e6 / 2000);
  return drew ? 0 : 1;
}

int main(void) {
  mkdir("/data/fnv_recomp", 0755);
  g_log = fopen("/data/fnv_recomp/glprobe.log", "w");
  if (!g_log) g_log = stderr;
  notify("GL probe: running");
  int r = probe();
  logf_("%s", g_summary);
  notify(g_summary);
  fclose(g_log);
  return r;
}
