/* Native versions of the game's hottest small maths routines (v38).
 *
 * The PS5 profile (v37) put the main thread's game-code time mostly in culling and transform maths: tiny x87
 * functions, compiled by the original developers without optimisation, called thousands of times a frame. As
 * recompiled code every x87 operation is a load and a store through the context, so a hand-written version is
 * several times faster.
 *
 * Each function here is listed in HOOKS in tools/recomp.py and replaces that game function; the original stays
 * callable as o_XXXXXXXX. They compute exactly what the recompiled code computes: the same operations in the same
 * order, in double precision (the runtime's x87 model), rounded to float at the same points (every fstp dword).
 * They also leave eax, ecx and edx as the original does, pop the same arguments, and push a float result on the
 * x87 stack where the original returns one. Results written through a pointer are computed into a temporary
 * first and copied at the end, as the originals do, so a result may alias an input.
 *
 * FNV_FASTMATH_CHECK=1 runs the original and the native version on the same input and logs any difference in
 * registers, the x87 stack or the bytes written. [fnv_recomp] iFastMath=0 in FalloutPrefs.ini runs the
 * originals only. */
#include "rt.h"
#include <string.h>
#include <stdlib.h>

#pragma clang fp contract(off) /* a*b+c must stay two roundings, as in the recompiled code */

int rt_setting_int(const char *key, int def); /* firstrun.c */

static int g_fm = -1;     /* 1: native versions on */
static int g_check;       /* FNV_FASTMATH_CHECK */
static void fm_init(void) {
  g_check = getenv("FNV_FASTMATH_CHECK") && atoi(getenv("FNV_FASTMATH_CHECK"));
  g_fm = rt_setting_int("iFastMath", 1) != 0;
  rt_log("fastmath: native culling and transform maths %s%s", g_fm ? "on" : "off (iFastMath=0)",
         g_check ? ", checked against the originals" : "");
}

static inline float f32(u32 a) { return MF32(a); }
static inline double d32(u32 a) { return (double)MF32(a); }
static inline u32 bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

/* thiscall/stdcall return: pop the return address and n bytes of arguments */
static inline void ret_n(Ctx *c, u32 n) { c->r[4] += 4 + n; }

/* ---- the native versions ------------------------------------------------------------------------------ */

/* 0x416870 Vec3(float x, float y, float z): this = {x, y, z} */
static void n_00416870(Ctx *c) {
  u32 t = c->r[1], esp = c->r[4];
  float x = (float)d32(esp + 4), y = (float)d32(esp + 8), z = (float)d32(esp + 12);
  MF32(t) = x; MF32(t + 4) = y; MF32(t + 8) = z;
  c->r[0] = c->r[1] = c->r[2] = t;
  ret_n(c, 12);
}

/* 0x439E90 / 0x439EF0 Vec3 operator+ / operator-: *result = this +/- *other (z, y, x computed in that order) */
static void vec_addsub(Ctx *c, int sub) {
  u32 a = c->r[1], esp = c->r[4], res = M32(esp + 4), b = M32(esp + 8);
  float z = sub ? (float)(d32(a + 8) - d32(b + 8)) : (float)(d32(a + 8) + d32(b + 8));
  float y = sub ? (float)(d32(a + 4) - d32(b + 4)) : (float)(d32(a + 4) + d32(b + 4));
  float x = sub ? (float)(d32(a) - d32(b)) : (float)(d32(a) + d32(b));
  MF32(res) = x; MF32(res + 4) = y; MF32(res + 8) = z;
  c->r[0] = c->r[1] = c->r[2] = res;
  ret_n(c, 8);
}
static void n_00439E90(Ctx *c) { vec_addsub(c, 0); }
static void n_00439EF0(Ctx *c) { vec_addsub(c, 1); }

/* 0x4A3760 (cdecl) Vec3 operator*(Vec3 *result, float s, const Vec3 *v): *result = s * *v */
static void n_004A3760(Ctx *c) {
  u32 esp = c->r[4], res = M32(esp + 4), v = M32(esp + 12);
  double s = d32(esp + 8);
  float z = (float)(s * d32(v + 8)), y = (float)(s * d32(v + 4)), x = (float)(s * d32(v));
  MF32(res) = x; MF32(res + 4) = y; MF32(res + 8) = z;
  c->r[0] = c->r[1] = c->r[2] = res;
  ret_n(c, 0);
}

/* row r of a 3x3 float matrix times a vector: ((m[r][0]*v0 + m[r][1]*v1) + m[r][2]*v2), in that order */
static inline double row_dot(u32 m, u32 v) {
  double s = d32(m) * d32(v);
  double t = d32(m + 4) * d32(v + 4);
  s = s + t;
  t = d32(m + 8) * d32(v + 8);
  return s + t;
}

/* 0x4B4500 Matrix3 operator*(Vec3 *result, const Vec3 *v): rows 2, 1, 0 */
static void n_004B4500(Ctx *c) {
  u32 m = c->r[1], esp = c->r[4], res = M32(esp + 4), v = M32(esp + 8);
  float z = (float)row_dot(m + 0x18, v), y = (float)row_dot(m + 0xC, v), x = (float)row_dot(m, v);
  MF32(res) = x; MF32(res + 4) = y; MF32(res + 8) = z;
  c->r[0] = c->r[1] = c->r[2] = res;
  ret_n(c, 8);
}

/* A (row-major 3x3) times column j of B: ((A[i][0]*B[0][j] + A[i][1]*B[1][j]) + A[i][2]*B[2][j]) */
static inline float mm_elem(u32 a, u32 b, int i, int j) {
  double s = d32(a + i * 12) * d32(b + j * 4);
  double t = d32(a + i * 12 + 4) * d32(b + 12 + j * 4);
  s = s + t;
  t = d32(a + i * 12 + 8) * d32(b + 24 + j * 4);
  return (float)(s + t);
}
static void matmul(u32 a, u32 b, float out[9]) {
  for (int j = 0; j < 3; j++) /* column by column, as the original (order does not change the values) */
    for (int i = 0; i < 3; i++) out[i * 3 + j] = mm_elem(a, b, i, j);
}

/* 0x43F8D0 Matrix3 operator*(Matrix3 *result, const Matrix3 *b) */
static void n_0043F8D0(Ctx *c) {
  u32 a = c->r[1], esp = c->r[4], res = M32(esp + 4), b = M32(esp + 8);
  float m[9];
  matmul(a, b, m);
  for (int k = 0; k < 9; k++) MF32(res + k * 4) = m[k];
  c->r[0] = res; c->r[1] = 0; c->r[2] = b; /* ecx: rep movsd ran out; edx: b */
  ret_n(c, 8);
}

/* 0x62C250 Transform operator*(Transform *result, const Transform *t2), Transform = {Matrix3 rot; Vec3 trans;
   float scale}: rot = rot1*rot2; trans = trans1 + scale1*(rot1*trans2); scale = scale1*scale2 */
static void n_0062C250(Ctx *c) {
  u32 t1 = c->r[1], esp = c->r[4], res = M32(esp + 4), t2 = M32(esp + 8);
  float out[13];
  out[12] = (float)(d32(t1 + 0x30) * d32(t2 + 0x30));
  matmul(t1, t2, out);
  u32 v2 = t2 + 0x24;
  float vz = (float)row_dot(t1 + 0x18, v2), vy = (float)row_dot(t1 + 0xC, v2), vx = (float)row_dot(t1, v2);
  double s = (double)(float)d32(t1 + 0x30); /* pushed through an fstp dword: unchanged */
  float wz = (float)(s * (double)vz), wy = (float)(s * (double)vy), wx = (float)(s * (double)vx);
  out[11] = (float)(d32(t1 + 0x2C) + (double)wz);
  out[10] = (float)(d32(t1 + 0x28) + (double)wy);
  out[9] = (float)(d32(t1 + 0x24) + (double)wx);
  for (int k = 0; k < 13; k++) MF32(res + k * 4) = out[k];
  c->r[0] = res; c->r[1] = 0; c->r[2] = bits(out[10]); /* edx: the new translation's y, as loaded by the original */
  ret_n(c, 8);
}

/* 0x4B6190 dot(this, p), returned on the x87 stack after an fstp dword */
static inline float dot3f(u32 a, u32 b) { return (float)row_dot(a, b); }
static void n_004B6190(Ctx *c) {
  u32 n = c->r[1], esp = c->r[4], p = M32(esp + 4);
  fpush(c, (double)dot3f(n, p));
  c->r[0] = p; c->r[1] = n; c->r[2] = p;
  ret_n(c, 4);
}

/* 0x4B6160 Plane::Distance(p) = dot(normal, p) - constant, rounded to float */
static inline float plane_dist(u32 pl, u32 p) { return (float)((double)dot3f(pl, p) - d32(pl + 0xC)); }
static void n_004B6160(Ctx *c) {
  u32 pl = c->r[1], esp = c->r[4], p = M32(esp + 4);
  fpush(c, (double)plane_dist(pl, p));
  c->r[0] = p; c->r[1] = pl; c->r[2] = p;
  ret_n(c, 4);
}

/* 0x4B6100 Sphere::Side(plane): 2 if the sphere is entirely behind it, 1 if entirely in front, 0 otherwise */
static void n_004B6100(Ctx *c) {
  u32 sp = c->r[1], esp = c->r[4], pl = M32(esp + 4);
  double d = (double)plane_dist(pl, sp), r = d32(sp + 0xC);
  u32 side = d <= -r ? 2 : r <= d ? 1 : 0; /* NaN: 0, as the original's compares */
  c->r[0] = side; c->r[1] = sp; c->r[2] = sp;
  ret_n(c, 4);
}

/* 0x49DA80 Plane::WhichSide(p): 2 behind, 1 in front, 0 on it (distance compared with the 0.0 at 0x1012060) */
static void n_0049DA80(Ctx *c) {
  u32 pl = c->r[1], esp = c->r[4], p = M32(esp + 4);
  double d = (double)(float)(row_dot(pl, p) - d32(pl + 0xC)), k = MF64(0x1012060);
  c->r[0] = d < k ? 2 : d > k ? 1 : 0;
  c->r[1] = pl; c->r[2] = p;
  ret_n(c, 4);
}

/* ---- dispatch and the side-by-side check ---------------------------------------------------------------- */

static void check(Ctx *c, u32 addr, void (*orig)(Ctx *), void (*nat)(Ctx *), u32 out_arg, u32 out_len, int fret) {
  static int reported;
  u32 out = out_arg == 1 ? c->r[1] : out_arg ? M32(c->r[4] + out_arg) : 0;
  u8 before[64], after_o[64];
  if (out_len > 64) out_len = 64;
  Ctx saved = *c;
  if (out) memcpy(before, HP(out), out_len);
  orig(c);
  Ctx o = *c;
  if (out) { memcpy(after_o, HP(out), out_len); memcpy(HP(out), before, out_len); }
  *c = saved;
  nat(c);
  int bad = 0;
  for (int i = 0; i < 8; i++) if (c->r[i] != o.r[i]) bad |= 1;
  if (c->top != o.top) bad |= 2;
  if (fret && memcmp(&c->st[c->top & 7], &o.st[o.top & 7], 8)) bad |= 4;
  if (out && memcmp(HP(out), after_o, out_len)) bad |= 8;
  if (bad && reported < 40) {
    reported++;
    rt_log("fastmath check: %08x differs (%s%s%s%s): eax %08x/%08x ecx %08x/%08x edx %08x/%08x esp %08x/%08x st0 %.9g/%.9g",
           addr, bad & 1 ? "registers " : "", bad & 2 ? "x87 top " : "", bad & 4 ? "x87 value " : "", bad & 8 ? "memory" : "",
           c->r[0], o.r[0], c->r[1], o.r[1], c->r[2], o.r[2], c->r[4], o.r[4], c->st[c->top & 7], o.st[o.top & 7]);
  }
  static u64 calls;
  if (++calls % 5000000 == 0) rt_log("fastmath check: %llu calls checked, %d differences reported", (unsigned long long)calls, reported);
}

#define HOOK(A, out_arg, out_len, fret)                                         \
  void o_##A(Ctx *restrict c);                                                  \
  void h_##A(Ctx *restrict c) {                                                 \
    if (__builtin_expect(g_fm < 0, 0)) fm_init();                               \
    if (!g_fm) { o_##A(c); return; }                                            \
    if (__builtin_expect(g_check, 0)) { check(c, 0x##A, o_##A, n_##A, out_arg, out_len, fret); return; } \
    n_##A(c);                                                                   \
  }

/* out_arg: where the result pointer is (1 = this, 4/8 = the stack slot), and how many bytes are written */
HOOK(00416870, 1, 12, 0)
HOOK(00439E90, 4, 12, 0)
HOOK(00439EF0, 4, 12, 0)
HOOK(004A3760, 4, 12, 0)
HOOK(004B4500, 4, 12, 0)
HOOK(0043F8D0, 4, 36, 0)
HOOK(0062C250, 4, 52, 0)
HOOK(004B6190, 0, 0, 1)
HOOK(004B6160, 0, 0, 1)
HOOK(004B6100, 0, 0, 0)
HOOK(0049DA80, 0, 0, 0)
