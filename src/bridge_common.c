#include "bridge_common.h"
#include <stdlib.h>
#include <string.h>

void wb_reset(wbuf *w) { w->len = 0; }

void wb_raw(wbuf *w, const void *d, size_t n) {
    if (w->len + n > w->cap) {
        size_t c = w->cap ? w->cap : 256;
        while (c < w->len + n) c *= 2;
        w->p = realloc(w->p, c);
        w->cap = c;
    }
    if (n) memcpy(w->p + w->len, d, n);
    w->len += n;
}
void wb_u32(wbuf *w, uint32_t v) { wb_raw(w, &v, 4); }
void wb_u64(wbuf *w, uint64_t v) { wb_raw(w, &v, 8); }
void wb_f32(wbuf *w, float v) { wb_raw(w, &v, 4); }
void wb_f64(wbuf *w, double v) { wb_raw(w, &v, 8); }

void wb_bytes(wbuf *w, const void *d, size_t n) {
    if (!d) { wb_u32(w, NIL_LEN); return; }
    wb_u32(w, (uint32_t)n);
    wb_raw(w, d, n);
}
void wb_str(wbuf *w, const char *s) { wb_bytes(w, s, s ? strlen(s) + 1 : 0); }

void wb_kvp(wbuf *w, const ovr_kvp *a, size_t n) {
    if (!a) n = 0;
    wb_u32(w, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        wb_str(w, a[i].key);
        wb_u32(w, (uint32_t)a[i].type);
        wb_str(w, a[i].str);
        wb_u32(w, (uint32_t)a[i].i);
        wb_f64(w, a[i].d);
    }
}

void wb_strarr(wbuf *w, const char *const *a, size_t n) {
    if (!a) { wb_u32(w, NIL_LEN); return; }
    wb_u32(w, (uint32_t)n);
    for (size_t i = 0; i < n; i++) wb_str(w, a[i]);
}

void wb_initparams(wbuf *w, const ovr_init_params *p) {
    if (!p) { wb_u32(w, 0); return; }
    wb_u32(w, 1);
    wb_u32(w, (uint32_t)p->sType);
    wb_str(w, p->email);
    wb_str(w, p->password);
    wb_u64(w, p->appId);
    wb_str(w, p->uriPrefixOverride);
}

static int take(rbuf *r, void *out, size_t n) {
    if (r->pos + n > r->len) { memset(out, 0, n); r->pos = r->len; return 0; }
    memcpy(out, r->p + r->pos, n);
    r->pos += n;
    return 1;
}
uint32_t rb_u32(rbuf *r) { uint32_t v; take(r, &v, 4); return v; }
uint64_t rb_u64(rbuf *r) { uint64_t v; take(r, &v, 8); return v; }
float rb_f32(rbuf *r) { float v; take(r, &v, 4); return v; }
double rb_f64(rbuf *r) { double v; take(r, &v, 8); return v; }

uint8_t *rb_bytes(rbuf *r, size_t *n) {
    uint32_t l = rb_u32(r);
    if (l == NIL_LEN || r->pos + l > r->len) { if (n) *n = 0; return NULL; }
    uint8_t *p = r->p + r->pos;
    r->pos += l;
    if (n) *n = l;
    return p;
}
