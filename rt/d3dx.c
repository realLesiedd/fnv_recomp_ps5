/* d3dx9_38: the math helpers, ID3DXBuffer and shader profile queries.
 * D3DX matrices are row-major, vectors are rows (v' = v * M). */
#include "com.h"
#include <string.h>
#include <math.h>

#define API(n) static void n(Ctx *c)
typedef float M44[16];

static void ld(M44 m, u32 p) { memcpy(m, HP(p), 64); }
static void st(u32 p, const M44 m) { memcpy(HP(p), m, 64); }
static float argf(Ctx *c, int i) { u32 v = ARG(i); float f; memcpy(&f, &v, 4); return f; }
static void mul(M44 o, const M44 a, const M44 b) {
  M44 r;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
  memcpy(o, r, 64);
}
static void transpose(M44 o, const M44 a) { M44 r; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r[i * 4 + j] = a[j * 4 + i]; memcpy(o, r, 64); }

API(MatrixMultiply) { M44 a, b, r; ld(a, ARG(1)); ld(b, ARG(2)); mul(r, a, b); st(ARG(0), r); RETURN_STD(3, ARG(0)); }
API(MatrixMultiplyTranspose) { M44 a, b, r; ld(a, ARG(1)); ld(b, ARG(2)); mul(r, a, b); transpose(r, r); st(ARG(0), r); RETURN_STD(3, ARG(0)); }
API(MatrixTranspose) { M44 a; ld(a, ARG(1)); transpose(a, a); st(ARG(0), a); RETURN_STD(2, ARG(0)); }
API(MatrixInverse) {
  M44 m, inv; ld(m, ARG(2));
  double d[16], r[16];
  for (int i = 0; i < 16; i++) d[i] = m[i];
  r[0] = d[5]*d[10]*d[15] - d[5]*d[11]*d[14] - d[9]*d[6]*d[15] + d[9]*d[7]*d[14] + d[13]*d[6]*d[11] - d[13]*d[7]*d[10];
  r[4] = -d[4]*d[10]*d[15] + d[4]*d[11]*d[14] + d[8]*d[6]*d[15] - d[8]*d[7]*d[14] - d[12]*d[6]*d[11] + d[12]*d[7]*d[10];
  r[8] = d[4]*d[9]*d[15] - d[4]*d[11]*d[13] - d[8]*d[5]*d[15] + d[8]*d[7]*d[13] + d[12]*d[5]*d[11] - d[12]*d[7]*d[9];
  r[12] = -d[4]*d[9]*d[14] + d[4]*d[10]*d[13] + d[8]*d[5]*d[14] - d[8]*d[6]*d[13] - d[12]*d[5]*d[10] + d[12]*d[6]*d[9];
  r[1] = -d[1]*d[10]*d[15] + d[1]*d[11]*d[14] + d[9]*d[2]*d[15] - d[9]*d[3]*d[14] - d[13]*d[2]*d[11] + d[13]*d[3]*d[10];
  r[5] = d[0]*d[10]*d[15] - d[0]*d[11]*d[14] - d[8]*d[2]*d[15] + d[8]*d[3]*d[14] + d[12]*d[2]*d[11] - d[12]*d[3]*d[10];
  r[9] = -d[0]*d[9]*d[15] + d[0]*d[11]*d[13] + d[8]*d[1]*d[15] - d[8]*d[3]*d[13] - d[12]*d[1]*d[11] + d[12]*d[3]*d[9];
  r[13] = d[0]*d[9]*d[14] - d[0]*d[10]*d[13] - d[8]*d[1]*d[14] + d[8]*d[2]*d[13] + d[12]*d[1]*d[10] - d[12]*d[2]*d[9];
  r[2] = d[1]*d[6]*d[15] - d[1]*d[7]*d[14] - d[5]*d[2]*d[15] + d[5]*d[3]*d[14] + d[13]*d[2]*d[7] - d[13]*d[3]*d[6];
  r[6] = -d[0]*d[6]*d[15] + d[0]*d[7]*d[14] + d[4]*d[2]*d[15] - d[4]*d[3]*d[14] - d[12]*d[2]*d[7] + d[12]*d[3]*d[6];
  r[10] = d[0]*d[5]*d[15] - d[0]*d[7]*d[13] - d[4]*d[1]*d[15] + d[4]*d[3]*d[13] + d[12]*d[1]*d[7] - d[12]*d[3]*d[5];
  r[14] = -d[0]*d[5]*d[14] + d[0]*d[6]*d[13] + d[4]*d[1]*d[14] - d[4]*d[2]*d[13] - d[12]*d[1]*d[6] + d[12]*d[2]*d[5];
  r[3] = -d[1]*d[6]*d[11] + d[1]*d[7]*d[10] + d[5]*d[2]*d[11] - d[5]*d[3]*d[10] - d[9]*d[2]*d[7] + d[9]*d[3]*d[6];
  r[7] = d[0]*d[6]*d[11] - d[0]*d[7]*d[10] - d[4]*d[2]*d[11] + d[4]*d[3]*d[10] + d[8]*d[2]*d[7] - d[8]*d[3]*d[6];
  r[11] = -d[0]*d[5]*d[11] + d[0]*d[7]*d[9] + d[4]*d[1]*d[11] - d[4]*d[3]*d[9] - d[8]*d[1]*d[7] + d[8]*d[3]*d[5];
  r[15] = d[0]*d[5]*d[10] - d[0]*d[6]*d[9] - d[4]*d[1]*d[10] + d[4]*d[2]*d[9] + d[8]*d[1]*d[6] - d[8]*d[2]*d[5];
  double det = d[0] * r[0] + d[1] * r[4] + d[2] * r[8] + d[3] * r[12];
  if (ARG(1)) { float f = (float)det; memcpy(HP(ARG(1)), &f, 4); }
  if (det == 0) RETURN_STD(3, 0);
  for (int i = 0; i < 16; i++) inv[i] = (float)(r[i] / det);
  st(ARG(0), inv);
  RETURN_STD(3, ARG(0));
}
API(MatrixRotationYawPitchRoll) {
  float y = argf(c, 1), p = argf(c, 2), r = argf(c, 3);
  float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(r), sr = sinf(r);
  M44 m = {cr * cy + sr * sp * sy, sr * cp, sr * sp * cy - cr * sy, 0,
           cr * sp * sy - sr * cy, cr * cp, sr * sy + cr * sp * cy, 0,
           cp * sy, -sp, cp * cy, 0,
           0, 0, 0, 1};
  st(ARG(0), m);
  RETURN_STD(4, ARG(0));
}
static void vload(float *v, u32 p, int n) { memcpy(v, HP(p), 4 * n); }
API(Vec3TransformCoord) {
  float v[3]; M44 m; vload(v, ARG(1), 3); ld(m, ARG(2));
  float o[4];
  for (int j = 0; j < 4; j++) o[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + m[12 + j];
  float w = o[3] ? o[3] : 1;
  float r[3] = {o[0] / w, o[1] / w, o[2] / w};
  memcpy(HP(ARG(0)), r, 12);
  RETURN_STD(3, ARG(0));
}
API(Vec3TransformNormal) {
  float v[3]; M44 m; vload(v, ARG(1), 3); ld(m, ARG(2));
  float r[3];
  for (int j = 0; j < 3; j++) r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j];
  memcpy(HP(ARG(0)), r, 12);
  RETURN_STD(3, ARG(0));
}
API(Vec4Transform) {
  float v[4]; M44 m; vload(v, ARG(1), 4); ld(m, ARG(2));
  float r[4];
  for (int j = 0; j < 4; j++) r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
  memcpy(HP(ARG(0)), r, 16);
  RETURN_STD(3, ARG(0));
}
API(Vec3Normalize) {
  float v[3]; vload(v, ARG(1), 3);
  float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l) { v[0] /= l; v[1] /= l; v[2] /= l; } else v[0] = v[1] = v[2] = 0;
  memcpy(HP(ARG(0)), v, 12);
  RETURN_STD(2, ARG(0));
}
API(PlaneNormalize) {
  float p[4]; vload(p, ARG(1), 4);
  float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
  if (l) for (int i = 0; i < 4; i++) p[i] /= l; else memset(p, 0, 16);
  memcpy(HP(ARG(0)), p, 16);
  RETURN_STD(2, ARG(0));
}
API(PlaneTransform) {
  float p[4]; M44 m; vload(p, ARG(1), 4); ld(m, ARG(2));
  float r[4];
  for (int j = 0; j < 4; j++) r[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + p[3] * m[12 + j];
  memcpy(HP(ARG(0)), r, 16);
  RETURN_STD(3, ARG(0));
}

/* ID3DXBuffer: [vtbl][ref][ptr][size] */
static void buf_ptr(Ctx *c) { RETURN_STD(1, M32(COM_THIS + 8)); }
static void buf_size(Ctx *c) { RETURN_STD(1, M32(COM_THIS + 12)); }
static void buf_addref(Ctx *c) { RETURN_STD(1, ++M32(COM_THIS + 4)); }
static void buf_release(Ctx *c) {
  u32 o = COM_THIS; u32 r = --M32(o + 4);
  if (!r) heap_free(M32(o + 8));
  RETURN_STD(1, r);
}
static void buf_qi(Ctx *c) { M32(ARG(2)) = COM_THIS; M32(COM_THIS + 4)++; RETURN_STD(3, 0); }
static const ComMethod buf_methods[] = {
  {"ID3DXBuffer::QueryInterface", 3, buf_qi}, {"ID3DXBuffer::AddRef", 1, buf_addref}, {"ID3DXBuffer::Release", 1, buf_release},
  {"ID3DXBuffer::GetBufferPointer", 1, buf_ptr}, {"ID3DXBuffer::GetBufferSize", 1, buf_size},
};
u32 d3dx_buffer(u32 size) {
  static u32 vt;
  if (!vt) vt = com_vtable(buf_methods, 5);
  u32 o = com_object(vt, 8);
  M32(o + 8) = heap_alloc(size ? size : 4, 1);
  M32(o + 12) = size;
  return o;
}
API(CreateBuffer) { M32(ARG(1)) = d3dx_buffer(ARG(0)); RETURN_STD(2, 0); }
API(GetVertexShaderProfile) { static u32 s; if (!s) s = guest_strdup("vs_3_0"); RETURN_STD(1, s); }
API(GetPixelShaderProfile) { static u32 s; if (!s) s = guest_strdup("ps_3_0"); RETURN_STD(1, s); }

static const ApiEntry list[] = {
#define E(n) {"d3dx9_38.dll", "D3DX" #n, n}
  E(MatrixMultiply), E(MatrixMultiplyTranspose), E(MatrixTranspose), E(MatrixInverse), E(MatrixRotationYawPitchRoll),
  E(Vec3TransformCoord), E(Vec3TransformNormal), E(Vec4Transform), E(Vec3Normalize), E(PlaneNormalize),
  E(PlaneTransform), E(CreateBuffer), E(GetVertexShaderProfile), E(GetPixelShaderProfile),
#undef E
  {0}};
void d3dx_init(void) { api_register(list); }
