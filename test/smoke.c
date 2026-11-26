/* Loads libovrplatform.so like Mono does and makes offline calls through the bridge. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
int main(int c, char **v) {
    void *h = dlopen(c > 1 ? v[1] : "libovrplatform.so", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    void (*reset)(void) = dlsym(h, "ovr_UnityResetTestPlatform");
    void (*globals)(void *) = dlsym(h, "ovr_UnityInitGlobals");
    const char *(*locale)(void) = dlsym(h, "ovr_GetLoggedInUserLocale");
    void *(*pop)(void) = dlsym(h, "ovr_PopMessage");
    uint32_t (*mtype)(void *) = dlsym(h, "ovr_Message_GetType");
    reset(); globals(0);
    printf("locale before init: %s\n", locale() ? locale() : "(null)");
    printf("type of null message (expect 0): %u\n", mtype(0));
    printf("pop message (expect null): %p\n", pop());
    return 0;
}
