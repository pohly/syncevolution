#define _GNU_SOURCE 1
#include <dlfcn.h>

static void *(*icaltimezone_get_component_p)(void *zone);
static void *(*icaltzutil_fetch_timezone_p)(const char *location);

static void init()
{
    static int initialized;
    if (!initialized) {
        static void *icaltzutil;
        icaltzutil = dlopen("libsyncevo-icaltz-util.so.0", RTLD_LAZY|RTLD_LOCAL);
        if (icaltzutil) {
            icaltimezone_get_component_p = dlsym(icaltzutil, "icaltimezone_get_component");
            icaltzutil_fetch_timezone_p = dlsym(icaltzutil, "icaltzutil_fetch_timezone");
        } else {
            icaltimezone_get_component_p = dlsym(RTLD_NEXT, "icaltimezone_get_component");
            icaltzutil_fetch_timezone_p = dlsym(RTLD_NEXT, "icaltzutil_fetch_timezone");
        }
        initialized = 1;
    }
}


void *icaltimezone_get_component(void *zone)
{
    init();
    return icaltimezone_get_component_p(zone);
}

void *icaltzutil_fetch_timezone(const char *location)
{
    init();
    return icaltzutil_fetch_timezone_p(location);
}

/*
 * For including the .o file in binaries via -Wl,-usyncevo_fetch_timezone.
 * We cannot use -Wl,-uicaltzutil_fetch_timezone because that gets satisfied by
 * libical itself.
 */
int syncevo_fetch_timezone;
