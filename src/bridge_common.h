/* Wire format shared by libovrplatform.so (Linux) and ovrbridge.exe (runs under Wine).
 * request  = u32 len | u32 func_id | args...
 * response = u32 len | out-args... | return value
 * u64/f32/f64 raw little endian; str/bytes = u32 len (NIL_LEN = NULL) + data
 * (strings carry their NUL terminator). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define EXPORT
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#define BRIDGE_MAGIC 0x4252564Fu
#define NIL_LEN 0xFFFFFFFFu

/* Same layout as the SDK's C# ovrKeyValuePair / OculusInitParams. */
typedef struct { const char *key; int32_t type; const char *str; int32_t i; double d; } ovr_kvp;
typedef struct { int32_t sType; const char *email; const char *password; uint64_t appId; const char *uriPrefixOverride; } ovr_init_params;

typedef struct { uint8_t *p; size_t len, cap; } wbuf;
typedef struct { uint8_t *p; size_t len, pos; } rbuf;

void wb_reset(wbuf *w);
void wb_raw(wbuf *w, const void *d, size_t n);
void wb_u32(wbuf *w, uint32_t v);
void wb_u64(wbuf *w, uint64_t v);
void wb_f32(wbuf *w, float v);
void wb_f64(wbuf *w, double v);
void wb_str(wbuf *w, const char *s);
void wb_bytes(wbuf *w, const void *d, size_t n);
void wb_kvp(wbuf *w, const ovr_kvp *a, size_t n);
void wb_strarr(wbuf *w, const char *const *a, size_t n);
void wb_initparams(wbuf *w, const ovr_init_params *p);

uint32_t rb_u32(rbuf *r);
uint64_t rb_u64(rbuf *r);
float rb_f32(rbuf *r);
double rb_f64(rbuf *r);
uint8_t *rb_bytes(rbuf *r, size_t *n); /* pointer into buffer or NULL */
