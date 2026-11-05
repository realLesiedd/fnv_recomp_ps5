/* x86 (32-bit) semantics used by the generated code.
 *
 * Guest memory is a 4 GiB window mapped at a fixed host address, so a guest
 * address becomes a host pointer with a single add.  Integer registers live in
 * C locals inside each generated function (synced to the Ctx at calls and
 * returns); flags are evaluated lazily from the last flag-writing operation so
 * the C compiler can fold them away when the condition is known statically. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>


typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;

/* host address of guest address 0; chosen at startup (rt/mem.c) */
extern u8 *g_guest_mem;
#ifndef GUEST_MEM
#define GUEST_MEM g_guest_mem
#endif

typedef u16 __attribute__((aligned(1), may_alias)) u16u;
typedef u32 __attribute__((aligned(1), may_alias)) u32u;
typedef u64 __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;

#define HP(a) ((u8 *)(GUEST_MEM + (u64)(u32)(a)))
#define M8(a) (*(u8 *)HP(a))
#define M16(a) (*(u16u *)HP(a))
#define M32(a) (*(u32u *)HP(a))
#define M64(a) (*(u64u *)HP(a))
#define MF32(a) (*(f32u *)HP(a))
#define MF64(a) (*(f64u *)HP(a))

typedef union {
  float f[4]; double d[2]; u32 u[4]; s32 s[4]; u16 w[8]; s16 sw[8]; u8 b[16]; u64 q[2];
} XMM;

typedef struct Ctx {
  u32 r[8]; /* eax ecx edx ebx esp ebp esi edi */
  u32 eflags; /* only meaningful across pushfd/popfd and at function boundaries */
  u32 df;
  /* x87 */
  double st[8];
  u32 top;
  u16 fcw, fsw, ftw;
  XMM xmm[8];
  u32 mxcsr;
  u32 fs_base; /* guest address of the TEB */
  u32 last_error;
  int thread_index;
  void *host; /* per-thread runtime data */
} Ctx;

typedef void (*GuestFn)(Ctx *restrict);

/* ---- lazy flags ------------------------------------------------------------ */
enum {
  LF_ADD8, LF_ADD16, LF_ADD32, LF_SUB8, LF_SUB16, LF_SUB32,
  LF_ADC8, LF_ADC16, LF_ADC32, LF_SBB8, LF_SBB16, LF_SBB32,
  LF_LOG8, LF_LOG16, LF_LOG32, LF_INC8, LF_INC16, LF_INC32, LF_DEC8, LF_DEC16, LF_DEC32,
  LF_NEG8, LF_NEG16, LF_NEG32, LF_SHL8, LF_SHL16, LF_SHL32, LF_SHR8, LF_SHR16, LF_SHR32,
  LF_SAR8, LF_SAR16, LF_SAR32,
  LF_EXPL /* lf_r holds the eflags bits directly */
};
#define F_CF 0x001u
#define F_PF 0x004u
#define F_AF 0x010u
#define F_ZF 0x040u
#define F_SF 0x080u
#define F_TF 0x100u
#define F_IF 0x200u
#define F_DF 0x400u
#define F_OF 0x800u

#define LFSZ(op) ((op) % 3 == 0 ? 8 : (op) % 3 == 1 ? 16 : 32)

static inline u32 lf_mask(int sz) { return sz == 8 ? 0xffu : sz == 16 ? 0xffffu : 0xffffffffu; }
static inline u32 lf_sign(int sz) { return sz == 8 ? 0x80u : sz == 16 ? 0x8000u : 0x80000000u; }

/* a,b are the operands, r the (masked) result, ci the carry in / shift count / old CF */
static inline int f_cf(int op, u32 a, u32 b, u32 r, u32 ci) {
  switch (op) {
  case LF_ADD8: case LF_ADD16: case LF_ADD32: return r < a;
  case LF_SUB8: case LF_SUB16: case LF_SUB32: return a < b;
  case LF_ADC8: case LF_ADC16: case LF_ADC32: return ci ? r <= a : r < a;
  case LF_SBB8: case LF_SBB16: case LF_SBB32: return ci ? a <= b : a < b;
  case LF_LOG8: case LF_LOG16: case LF_LOG32: return 0;
  case LF_INC8: case LF_INC16: case LF_INC32: case LF_DEC8: case LF_DEC16: case LF_DEC32: return ci & 1;
  case LF_NEG8: case LF_NEG16: case LF_NEG32: return b != 0;
  case LF_SHL8: case LF_SHL16: case LF_SHL32: return ci <= LFSZ(op) ? (a >> (LFSZ(op) - ci)) & 1 : 0;
  case LF_SHR8: case LF_SHR16: case LF_SHR32: return (a >> (ci - 1)) & 1;
  case LF_SAR8: return ((u32)(s32)(s8)a >> (ci > 8 ? 7 : ci - 1)) & 1;
  case LF_SAR16: return ((u32)(s32)(s16)a >> (ci > 16 ? 15 : ci - 1)) & 1;
  case LF_SAR32: return ((u32)((s32)a >> (ci - 1))) & 1;
  default: return r & F_CF ? 1 : 0;
  }
}
static inline int f_zf(int op, u32 r) { return op == LF_EXPL ? (r & F_ZF) != 0 : r == 0; }
static inline int f_sf(int op, u32 r) { return op == LF_EXPL ? (r & F_SF) != 0 : (r & lf_sign(LFSZ(op))) != 0; }
static inline int f_of(int op, u32 a, u32 b, u32 r, u32 ci) {
  u32 s = op == LF_EXPL ? 0 : lf_sign(LFSZ(op));
  switch (op) {
  case LF_ADD8: case LF_ADD16: case LF_ADD32: case LF_ADC8: case LF_ADC16: case LF_ADC32:
    return ((a ^ r) & (b ^ r) & s) != 0;
  case LF_SUB8: case LF_SUB16: case LF_SUB32: case LF_SBB8: case LF_SBB16: case LF_SBB32:
    return ((a ^ b) & (a ^ r) & s) != 0;
  case LF_LOG8: case LF_LOG16: case LF_LOG32: return 0;
  case LF_INC8: case LF_INC16: case LF_INC32: return r == s;
  case LF_DEC8: case LF_DEC16: case LF_DEC32: return r == s - 1;
  case LF_NEG8: case LF_NEG16: case LF_NEG32: return r == s;
  case LF_SHL8: case LF_SHL16: case LF_SHL32: return (((r & s) != 0) ^ f_cf(op, a, b, r, ci));
  case LF_SHR8: case LF_SHR16: case LF_SHR32: return (a & s) != 0 && ci == 1;
  case LF_SAR8: case LF_SAR16: case LF_SAR32: return 0;
  default: return (r & F_OF) != 0;
  }
}
static inline int f_pf(int op, u32 r) {
  if (op == LF_EXPL) return (r & F_PF) != 0;
  return !__builtin_parity(r & 0xff);
}
static inline int f_af(int op, u32 a, u32 b, u32 r) {
  if (op == LF_EXPL) return (r & F_AF) != 0;
  return ((a ^ b ^ r) & 0x10) != 0;
}
static inline u32 f_all(int op, u32 a, u32 b, u32 r, u32 ci) {
  if (op == LF_EXPL) return r;
  return (f_cf(op, a, b, r, ci) ? F_CF : 0) | (f_pf(op, r) ? F_PF : 0) | (f_af(op, a, b, r) ? F_AF : 0) |
         (f_zf(op, r) ? F_ZF : 0) | (f_sf(op, r) ? F_SF : 0) | (f_of(op, a, b, r, ci) ? F_OF : 0);
}

/* condition codes, x86 numbering: o no b ae e ne be a s ns p np l ge le g */
static inline int f_cond(int cc, int op, u32 a, u32 b, u32 r, u32 ci) {
  /* fast paths for compare-like ops, folded when op is constant */
  if (op == LF_SUB8 || op == LF_SUB16 || op == LF_SUB32) {
    int sz = LFSZ(op);
    s32 sa = sz == 8 ? (s8)a : sz == 16 ? (s16)a : (s32)a;
    s32 sb = sz == 8 ? (s8)b : sz == 16 ? (s16)b : (s32)b;
    switch (cc) {
    case 2: return a < b; case 3: return a >= b; case 4: return a == b; case 5: return a != b;
    case 6: return a <= b; case 7: return a > b;
    case 12: return sa < sb; case 13: return sa >= sb; case 14: return sa <= sb; case 15: return sa > sb;
    }
  }
  if (op == LF_LOG8 || op == LF_LOG16 || op == LF_LOG32) {
    int sz = LFSZ(op);
    s32 sr = sz == 8 ? (s8)r : sz == 16 ? (s16)r : (s32)r;
    switch (cc) {
    case 0: return 0; case 1: return 1; case 2: return 0; case 3: return 1;
    case 4: return r == 0; case 5: return r != 0; case 6: return r == 0; case 7: return r != 0;
    case 8: return sr < 0; case 9: return sr >= 0;
    case 12: return sr < 0; case 13: return sr >= 0; case 14: return sr <= 0; case 15: return sr > 0;
    }
  }
  switch (cc) {
  case 0: return f_of(op, a, b, r, ci);
  case 1: return !f_of(op, a, b, r, ci);
  case 2: return f_cf(op, a, b, r, ci);
  case 3: return !f_cf(op, a, b, r, ci);
  case 4: return f_zf(op, r);
  case 5: return !f_zf(op, r);
  case 6: return f_cf(op, a, b, r, ci) || f_zf(op, r);
  case 7: return !f_cf(op, a, b, r, ci) && !f_zf(op, r);
  case 8: return f_sf(op, r);
  case 9: return !f_sf(op, r);
  case 10: return f_pf(op, r);
  case 11: return !f_pf(op, r);
  case 12: return f_sf(op, r) != f_of(op, a, b, r, ci);
  case 13: return f_sf(op, r) == f_of(op, a, b, r, ci);
  case 14: return f_zf(op, r) || f_sf(op, r) != f_of(op, a, b, r, ci);
  default: return !f_zf(op, r) && f_sf(op, r) == f_of(op, a, b, r, ci);
  }
}

/* ---- x87 ------------------------------------------------------------------- */
#define ST(i) (c->st[(c->top + (i)) & 7])
static inline void fpush(Ctx *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline double fpop(Ctx *c) { double v = c->st[c->top]; c->top = (c->top + 1) & 7; return v; }
/* C0=0x100 C2=0x400 C3=0x4000; TOP in bits 11..13 */
static inline void fcom_set(Ctx *c, double a, double b) {
  u16 cc;
  if (a != a || b != b) cc = 0x4500;
  else if (a > b) cc = 0;
  else if (a < b) cc = 0x100;
  else cc = 0x4000;
  c->fsw = (c->fsw & ~0x4700) | cc;
}
static inline u16 fstsw(Ctx *c) { return (c->fsw & ~0x3800) | ((c->top & 7) << 11); }
static inline double frnd(Ctx *c, double v) {
  switch ((c->fcw >> 10) & 3) {
  case 0: return nearbyint(v);
  case 1: return floor(v);
  case 2: return ceil(v);
  default: return trunc(v);
  }
}
static inline s32 fist32(Ctx *c, double v) {
  v = frnd(c, v);
  if (!(v >= -2147483648.0 && v < 2147483648.0)) return (s32)0x80000000;
  return (s32)v;
}
static inline s16 fist16(Ctx *c, double v) {
  v = frnd(c, v);
  if (!(v >= -32768.0 && v < 32768.0)) return (s16)0x8000;
  return (s16)v;
}
static inline s64 fist64(Ctx *c, double v) {
  v = frnd(c, v);
  if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return (s64)0x8000000000000000ULL;
  return (s64)v;
}
/* 80-bit extended loads/stores (rare: fld tbyte / fstp tbyte) */
static inline double ld80(u32 a) {
  u64 m = M64(a); u16 se = M16(a + 8);
  int e = se & 0x7fff; double s = (se & 0x8000) ? -1.0 : 1.0;
  if (e == 0 && m == 0) return s * 0.0;
  if (e == 0x7fff) return (m << 1) ? NAN : s * INFINITY;
  return s * ldexp((double)m, e - 16383 - 63);
}
static inline void st80(u32 a, double v) {
  u16 se = signbit(v) ? 0x8000 : 0; v = fabs(v);
  u64 m; int e;
  if (v == 0) { m = 0; e = 0; }
  else if (isinf(v)) { m = 0x8000000000000000ULL; e = 0x7fff; }
  else if (isnan(v)) { m = 0xC000000000000000ULL; e = 0x7fff; }
  else { int ex; double fr = frexp(v, &ex); m = (u64)ldexp(fr, 64); e = ex - 1 + 16383; }
  M64(a) = m; M16(a + 8) = se | (u16)e;
}

/* ---- SSE helpers ----------------------------------------------------------- */
static inline float rsqrt_f(float x) { return 1.0f / sqrtf(x); }
static inline float rcp_f(float x) { return 1.0f / x; }
static inline float maxf_x86(float a, float b) { return a > b ? a : b; }
static inline float minf_x86(float a, float b) { return a < b ? a : b; }
static inline u32 cmpf(int pred, float a, float b) {
  int r;
  switch (pred & 7) {
  case 0: r = a == b; break; case 1: r = a < b; break; case 2: r = a <= b; break; case 3: r = a != a || b != b; break;
  case 4: r = !(a == b); break; case 5: r = !(a < b); break; case 6: r = !(a <= b); break; default: r = a == a && b == b;
  }
  return r ? 0xffffffffu : 0;
}
static inline s32 cvt_f2i(Ctx *c, float v, int trunc_) {
  double d = trunc_ ? trunc(v) : nearbyint(v);
  if (!(d >= -2147483648.0 && d < 2147483648.0)) return (s32)0x80000000;
  return (s32)d;
}
static inline s32 cvt_d2i(Ctx *c, double v, int trunc_) {
  double d = trunc_ ? trunc(v) : nearbyint(v);
  if (!(d >= -2147483648.0 && d < 2147483648.0)) return (s32)0x80000000;
  return (s32)d;
}

/* ---- runtime entry points used by generated code --------------------------- */
void rt_call_ind(Ctx *restrict c, u32 target);          /* dispatch to a guest or host function */
void rt_unimpl(Ctx *restrict c, u32 addr, const char *what);
void rt_cpuid(Ctx *restrict c);
u64 rt_rdtsc(void);
void rt_int3(Ctx *restrict c, u32 addr);
u32 rt_jmp_ind_target(Ctx *restrict c, u32 target);
