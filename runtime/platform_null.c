/* platform_null.c - no window, no sound.  Used for tests and on hosts
 * without a native front end; combine with --frames and --dump. */
#include <time.h>
#include "internal.h"

int plat_init(const char *title, int scale) { (void)title; (void)scale; return 0; }
void plat_shutdown(void) {}
int plat_present(const uint32_t *xrgb) { (void)xrgb; return 0; }
uint32_t plat_buttons(void) { return 0; }
void plat_audio(const int16_t *stereo, int frames, int rate) { (void)stereo; (void)frames; (void)rate; }

uint64_t plat_time_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void plat_sleep_us(uint64_t us) {
    /* a portable busy wait is good enough for a front end that shows nothing */
    uint64_t end = plat_time_us() + us;
    while (plat_time_us() < end) {}
}
