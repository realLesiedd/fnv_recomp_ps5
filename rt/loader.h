#pragma once
#include "x86.h"

typedef struct { const char *name; u32 va, vsize, raw_off, raw_size, flags; } ImageSection;
typedef struct { u32 iat; const char *dll, *name; } ImageImport;

extern const ImageSection g_sections[];
extern const int g_section_count;
extern const ImageImport g_imports[];
extern const int g_import_count;
extern const u32 g_entry;
extern const u32 g_tls_callbacks[];
extern const u32 g_tls_start, g_tls_end, g_tls_index_addr;
extern const u32 g_image_size;
extern const u32 g_stack_reserve;

extern const u32 g_func_addr[];
extern GuestFn const g_func_ptr[];
extern const u32 g_func_count;
