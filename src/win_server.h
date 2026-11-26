#pragma once
#include "bridge_common.h"

typedef void (*sv_dispatch_fn)(rbuf *r, wbuf *w);
extern const sv_dispatch_fn sv_table[];
extern const unsigned sv_table_len;

void *sv_sym(const char *name);
void *sv_scratch(size_t n);                     /* freed after the current call */
uint64_t sv_size_call(const char *fn, uintptr_t handle);

const char *rb_str(rbuf *r);
void *rb_bytes_dup(rbuf *r, size_t *n);          /* writable, NULL if NULL was sent */
const ovr_kvp *rb_kvp(rbuf *r);
const char *const *rb_strarr(rbuf *r);
const ovr_init_params *rb_initparams(rbuf *r, ovr_init_params *storage);
