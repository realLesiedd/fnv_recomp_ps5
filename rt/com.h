/* COM objects visible to the guest: the object lives in guest memory, its vtable
 * holds thunk addresses of host functions (stdcall, `this` is the first arg). */
#pragma once
#include "rt.h"

typedef struct { const char *name; int nargs; HostFn fn; } ComMethod; /* nargs includes `this`; fn NULL = stub */

/* builds a vtable once per interface; returns its guest address */
u32 com_vtable(const ComMethod *m, int n);
/* allocates an object: [vtbl][refcount][extra bytes...] */
u32 com_object(u32 vtbl, u32 extra);
#define COM_THIS ARG(0)
#define COM_DATA(obj) ((obj) + 8)
