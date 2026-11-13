/* Shared by the guest build and the native reference build of each test. */
#ifndef TEST_H
#define TEST_H
#include <stdint.h>
#include <stddef.h>

void put(const char *s);
void put_hex(const char *label, uint32_t v);

static inline uint32_t rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
#endif
