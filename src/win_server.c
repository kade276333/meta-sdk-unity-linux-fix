/* ovrbridge.exe: runs under Wine, loads Meta's real LibOVRPlatformImpl64_1.dll and
 * executes the calls libovrplatform.so sends over stdin, answering on stdout.
 * Usage: ovrbridge.exe <path to LibOVRPlatformImpl64_1.dll> */
#include "win_server.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HMODULE dll;
static HANDLE hin, hout;

void *sv_sym(const char *name) { return dll ? (void *)GetProcAddress(dll, name) : NULL; }

/* per-call scratch allocations */
static void **scratch;
static size_t nscratch, capscratch;
void *sv_scratch(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (nscratch == capscratch) {
        capscratch = capscratch ? capscratch * 2 : 32;
        scratch = realloc(scratch, capscratch * sizeof *scratch);
    }
    scratch[nscratch++] = p;
    return p;
}
static void scratch_free(void) {
    for (size_t i = 0; i < nscratch; i++) free(scratch[i]);
    nscratch = 0;
}

uint64_t sv_size_call(const char *fn, uintptr_t h) {
    void *f = sv_sym(fn);
    if (!f) return 0;
    if (strstr(fn, "Length")) return ((uint32_t (*)(uintptr_t))f)(h);  /* unsigned int */
    return ((size_t (*)(uintptr_t))f)(h);                               /* size_t */
}

const char *rb_str(rbuf *r) { return (const char *)rb_bytes(r, NULL); }

void *rb_bytes_dup(rbuf *r, size_t *n) {
    size_t len;
    uint8_t *p = rb_bytes(r, &len);
    if (n) *n = p ? len : 0;
    if (!p) return NULL;
    void *c = sv_scratch(len);
    memcpy(c, p, len);
    return c;
}

const ovr_kvp *rb_kvp(rbuf *r) {
    uint32_t n = rb_u32(r);
    ovr_kvp *a = sv_scratch(sizeof *a * (n ? n : 1));
    for (uint32_t i = 0; i < n; i++) {
        a[i].key = rb_str(r);
        a[i].type = (int32_t)rb_u32(r);
        a[i].str = rb_str(r);
        a[i].i = (int32_t)rb_u32(r);
        a[i].d = rb_f64(r);
    }
    return a;
}

const char *const *rb_strarr(rbuf *r) {
    uint32_t n = rb_u32(r);
    if (n == NIL_LEN) return NULL;
    const char **a = sv_scratch(sizeof *a * (n + 1));
    for (uint32_t i = 0; i < n; i++) a[i] = rb_str(r);
    return a;
}

const ovr_init_params *rb_initparams(rbuf *r, ovr_init_params *s) {
    if (!rb_u32(r)) return NULL;
    s->sType = (int32_t)rb_u32(r);
    s->email = rb_str(r);
    s->password = rb_str(r);
    s->appId = rb_u64(r);
    s->uriPrefixOverride = rb_str(r);
    return s;
}

static int read_all(void *d, DWORD n) {
    uint8_t *p = d;
    while (n) {
        DWORD k = 0;
        if (!ReadFile(hin, p, n, &k, NULL) || !k) return 0;
        p += k; n -= k;
    }
    return 1;
}
static int write_all(const void *d, DWORD n) {
    const uint8_t *p = d;
    while (n) {
        DWORD k = 0;
        if (!WriteFile(hout, p, n, &k, NULL) || !k) return 0;
        p += k; n -= k;
    }
    return 1;
}

int main(int argc, char **argv) {
    /* Private copies: freopen() below closes the original stdout handle. */
    HANDLE self = GetCurrentProcess();
    DuplicateHandle(self, GetStdHandle(STD_INPUT_HANDLE), self, &hin, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(self, GetStdHandle(STD_OUTPUT_HANDLE), self, &hout, 0, FALSE, DUPLICATE_SAME_ACCESS);
    /* Keep the protocol pipe private: anything the DLL prints goes to NUL. */
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    SetStdHandle(STD_OUTPUT_HANDLE, nul);
    freopen("NUL", "w", stdout);

    char err[400] = "";
    fprintf(stderr, "[ovrbridge] loading %s\n", argc >= 2 ? argv[1] : "?"); fflush(stderr);
    if (argc >= 2) {
        dll = LoadLibraryExA(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) snprintf(err, sizeof err, "LoadLibrary(%s) failed, error %lu", argv[1], GetLastError());
    } else {
        snprintf(err, sizeof err, "no DLL path given");
    }
    fprintf(stderr, "[ovrbridge] load result %p %s\n", (void *)dll, err); fflush(stderr);
    uint32_t hs[3] = {BRIDGE_MAGIC, sv_table_len, dll != NULL};
    write_all(hs, sizeof hs);
    if (!dll) {
        uint32_t l = (uint32_t)strlen(err) + 1;
        write_all(&l, 4);
        write_all(err, l);
        return 1;
    }

    rbuf r = {0};
    size_t rcap = 0;
    wbuf w = {0};
    for (;;) {
        uint32_t len;
        if (!read_all(&len, 4) || len < 4) break;
        if (len > rcap) { r.p = realloc(r.p, len); rcap = len; }
        if (!read_all(r.p, len)) break;
        r.len = len; r.pos = 0;
        uint32_t id = rb_u32(&r);
        wb_reset(&w);
        wb_u32(&w, 0);
        if (id && id < sv_table_len) sv_table[id](&r, &w);
        uint32_t out = (uint32_t)(w.len - 4);
        memcpy(w.p, &out, 4);
        if (!write_all(w.p, (DWORD)w.len)) break;
        scratch_free();
    }
    return 0;
}
