#pragma once
#include "bridge_common.h"

wbuf *cl_begin(uint32_t func_id);  /* locks the bridge; returns the request buffer */
rbuf *cl_call(wbuf *w);            /* sends, waits for the response (empty on failure) */
void cl_end(rbuf *r);              /* unlocks */

void rb_bytes_into(rbuf *r, void *dst, size_t n);
const char *rb_str_keep(rbuf *r);   /* copy kept alive until 256 later returns */
const void *rb_blob_keep(rbuf *r);
