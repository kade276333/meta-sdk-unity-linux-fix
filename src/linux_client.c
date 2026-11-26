/* Linux side: libovrplatform.so. Starts ovrbridge.exe under Wine on first use and
 * forwards every call to it over a pipe pair.
 *
 * Files are looked up in (first match wins):
 *   $OVRPLATFORM_LINUX_DIR
 *   <dir of this .so>/ovrplatform-linux
 *   ~/.local/share/ovrplatform-linux
 * which must contain ovrbridge.exe and LibOVRPlatformImpl64_1.dll (+ its LibOVR*.dll deps).
 *
 * Wine prefix: $OVRPLATFORM_WINEPREFIX, else <that dir>/prefix.
 * Wine binary: $OVRPLATFORM_WINE, else "wine" from PATH.
 * OVRPLATFORM_LINUX_DEBUG=1 lets Wine's own debug output through to stderr. */
#define _GNU_SOURCE
#include "linux_client.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int to_fd = -1, from_fd = -1, dead, warned;
static pid_t child;
static wbuf req;
static rbuf resp;
static uint8_t *resp_mem;
static size_t resp_cap;

#define LOG(...) fprintf(stderr, "[ovrplatform-linux] " __VA_ARGS__)

static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static int find_dir(char *out, size_t n) {
    char cand[PATH_MAX], probe[PATH_MAX + 64];
    const char *env = getenv("OVRPLATFORM_LINUX_DIR");
    Dl_info info;
    const char *dirs[3] = {0};
    char self_dir[PATH_MAX] = "", home_dir[PATH_MAX] = "";
    if (dladdr((void *)find_dir, &info) && info.dli_fname) {
        snprintf(cand, sizeof cand, "%s", info.dli_fname);
        char *slash = strrchr(cand, '/');
        if (slash) { *slash = 0; snprintf(self_dir, sizeof self_dir, "%s/ovrplatform-linux", cand); }
    }
    if (getenv("HOME")) snprintf(home_dir, sizeof home_dir, "%s/.local/share/ovrplatform-linux", getenv("HOME"));
    dirs[0] = env; dirs[1] = self_dir; dirs[2] = home_dir;
    for (int i = 0; i < 3; i++) {
        if (!dirs[i] || !dirs[i][0]) continue;
        snprintf(probe, sizeof probe, "%s/ovrbridge.exe", dirs[i]);
        if (is_file(probe)) { snprintf(out, n, "%s", dirs[i]); return 1; }
    }
    return 0;
}

static int write_all(int fd, const void *d, size_t n) {
    const uint8_t *p = d;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}
static int read_all(int fd, void *d, size_t n) {
    uint8_t *p = d;
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

static void kill_bridge(void) {
    if (to_fd >= 0) close(to_fd);
    if (from_fd >= 0) close(from_fd);
    to_fd = from_fd = -1;
    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, WNOHANG); child = 0; }
}

static int start_bridge(void) {
    char dir[PATH_MAX], exe[PATH_MAX + 32], dll[PATH_MAX + 64], prefix[PATH_MAX + 16];
    if (!find_dir(dir, sizeof dir)) {
        LOG("ovrbridge.exe not found (set OVRPLATFORM_LINUX_DIR). Platform SDK calls will return nothing.\n");
        return 0;
    }
    snprintf(exe, sizeof exe, "%s/ovrbridge.exe", dir);
    snprintf(dll, sizeof dll, "%s/LibOVRPlatformImpl64_1.dll", dir);
    if (getenv("OVRPLATFORM_WINEPREFIX")) snprintf(prefix, sizeof prefix, "%s", getenv("OVRPLATFORM_WINEPREFIX"));
    else snprintf(prefix, sizeof prefix, "%s/prefix", dir);
    const char *wine = getenv("OVRPLATFORM_WINE") ? getenv("OVRPLATFORM_WINE") : "wine";
    int debug = getenv("OVRPLATFORM_LINUX_DEBUG") && atoi(getenv("OVRPLATFORM_LINUX_DEBUG"));

    int a[2], b[2];
    if (pipe2(a, O_CLOEXEC) || pipe2(b, O_CLOEXEC)) return 0;

    /* environment: inherit, override the Wine bits */
    size_t ne = 0;
    while (environ[ne]) ne++;
    char **envp = calloc(ne + 4, sizeof *envp);
    size_t k = 0;
    for (size_t i = 0; i < ne; i++)
        if (strncmp(environ[i], "WINEPREFIX=", 11) && strncmp(environ[i], "WINEDEBUG=", 10) &&
            strncmp(environ[i], "WINEDLLOVERRIDES=", 17))
            envp[k++] = environ[i];
    char e1[PATH_MAX + 32];
    snprintf(e1, sizeof e1, "WINEPREFIX=%s", prefix);
    envp[k++] = e1;
    envp[k++] = debug ? "WINEDEBUG=err+all,fixme-all" : "WINEDEBUG=-all";
    envp[k++] = "WINEDLLOVERRIDES=mscoree,mshtml=";
    envp[k] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, a[0], 0);
    posix_spawn_file_actions_adddup2(&fa, b[1], 1);
    if (!debug) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none; sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    sigset_t def; sigemptyset(&def); sigaddset(&def, SIGPIPE); sigaddset(&def, SIGCHLD);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    char *argv[] = {(char *)wine, exe, dll, NULL};
    int rc = posix_spawnp(&child, wine, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    free(envp);
    close(a[0]); close(b[1]);
    if (rc) { LOG("could not start %s: %s\n", wine, strerror(rc)); close(a[1]); close(b[0]); child = 0; return 0; }
    to_fd = a[1]; from_fd = b[0];

    /* handshake: magic, function-table size, load status, message */
    uint32_t hs[3];
    if (!read_all(from_fd, hs, sizeof hs) || hs[0] != BRIDGE_MAGIC) {
        LOG("Wine helper did not start (run with OVRPLATFORM_LINUX_DEBUG=1 for Wine output).\n");
        kill_bridge();
        return 0;
    }
    if (!hs[2]) {
        char msg[512] = "";
        uint32_t l = 0;
        read_all(from_fd, &l, 4);
        if (l < sizeof msg) read_all(from_fd, msg, l);
        LOG("Meta DLL failed to load in Wine: %s\n", msg);
        kill_bridge();
        return 0;
    }
    extern const unsigned cl_table_len;
    if (hs[1] != cl_table_len) {
        LOG("ovrbridge.exe / libovrplatform.so version mismatch (%u vs %u); rebuild both.\n", hs[1], cl_table_len);
        kill_bridge();
        return 0;
    }
    LOG("bridge up: real Meta Platform SDK running under Wine (pid %d)\n", (int)child);
    return 1;
}

wbuf *cl_begin(uint32_t id) {
    pthread_mutex_lock(&mu);
    wb_reset(&req);
    wb_u32(&req, 0);   /* length placeholder */
    wb_u32(&req, id);
    return &req;
}

rbuf *cl_call(wbuf *w) {
    resp.len = resp.pos = 0;
    resp.p = resp_mem;
    if (dead) return &resp;
    if (to_fd < 0 && !start_bridge()) { dead = 1; return &resp; }

    uint32_t len = (uint32_t)(w->len - 4);
    memcpy(w->p, &len, 4);

    /* don't let a dead pipe raise SIGPIPE in Unity */
    sigset_t pipe_set, old;
    sigemptyset(&pipe_set); sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    int ok = write_all(to_fd, w->p, w->len);
    if (!ok) {
        struct timespec zero = {0, 0};
        sigtimedwait(&pipe_set, NULL, &zero);
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    uint32_t rl = 0;
    if (ok) ok = read_all(from_fd, &rl, 4);
    if (ok && rl > resp_cap) { resp_mem = realloc(resp_mem, rl); resp_cap = rl; }
    if (ok) ok = read_all(from_fd, resp_mem, rl);
    if (!ok) {
        if (!warned++) LOG("lost connection to the Wine helper; Platform SDK calls now return nothing.\n");
        kill_bridge();
        dead = 1;
        return &resp;
    }
    resp.p = resp_mem;
    resp.len = rl;
    return &resp;
}

void cl_end(rbuf *r) { (void)r; pthread_mutex_unlock(&mu); }

void rb_bytes_into(rbuf *r, void *dst, size_t n) {
    size_t got;
    uint8_t *p = rb_bytes(r, &got);
    if (dst && p) memcpy(dst, p, got < n ? got : n);
}

/* Returned strings/blobs must outlive the call; C# copies them immediately. */
static void *ring[256];
static unsigned ring_i;
static void *keep(const uint8_t *p, size_t n) {
    if (!p) return NULL;
    void *c = malloc(n ? n : 1);
    memcpy(c, p, n);
    free(ring[ring_i]);
    ring[ring_i] = c;
    ring_i = (ring_i + 1) % 256;
    return c;
}
const char *rb_str_keep(rbuf *r) { size_t n; uint8_t *p = rb_bytes(r, &n); return keep(p, n); }
const void *rb_blob_keep(rbuf *r) { size_t n; uint8_t *p = rb_bytes(r, &n); return keep(p, n); }
