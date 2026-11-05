#include "com.h"
#include <string.h>

/* stubs: pop N arguments, return S_OK, log the call once per method */
#define STUB(n) static void stub##n(Ctx *c) { RETURN_STD(n, 0); }
STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9) STUB(10) STUB(11) STUB(12) STUB(13) STUB(14)
static HostFn g_stubs[] = {NULL, stub1, stub2, stub3, stub4, stub5, stub6, stub7, stub8, stub9, stub10, stub11, stub12, stub13, stub14};

u32 com_vtable(const ComMethod *m, int n) {
  u32 vt = heap_alloc(4 * n, 1);
  for (int i = 0; i < n; i++) {
    HostFn f = m[i].fn ? m[i].fn : g_stubs[m[i].nargs];
    M32(vt + 4 * i) = rt_thunk(f, m[i].name);
  }
  return vt;
}

u32 com_object(u32 vtbl, u32 extra) {
  u32 o = heap_alloc(8 + extra, 1);
  M32(o) = vtbl;
  M32(o + 4) = 1;
  return o;
}
