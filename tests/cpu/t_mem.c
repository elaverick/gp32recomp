/* Loads and stores of every width, signed and unsigned, with every addressing style. */
#include "test.h"

struct rec { int8_t a; uint8_t b; int16_t c; uint16_t d; int32_t e; uint32_t f; int8_t g[3]; uint16_t h; };

static struct rec recs[64];
static uint8_t bytes[1024];
static uint16_t halves[256];
static uint32_t words[256];
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);

__attribute__((noinline)) static uint32_t sum_rec(const struct rec *r, int n) {
    uint32_t h = 0;
    while (n--) {
        h = h * 31 + (uint32_t)r->a + r->b + (uint32_t)r->c + r->d + (uint32_t)r->e + r->f + (uint32_t)r->g[1] + r->h;
        r++;
    }
    return h;
}

__attribute__((noinline)) static void fill(uint32_t *s) {
    int i;
    for (i = 0; i < 64; i++) {
        struct rec *r = &recs[i];
        uint32_t v = rng(s);
        r->a = (int8_t)v; r->b = (uint8_t)(v >> 8); r->c = (int16_t)(v >> 16); r->d = (uint16_t)(v >> 3);
        r->e = (int32_t)rng(s); r->f = rng(s);
        r->g[0] = (int8_t)(v >> 5); r->g[1] = (int8_t)(v >> 11); r->g[2] = (int8_t)(v >> 19);
        r->h = (uint16_t)(v >> 7);
    }
    for (i = 0; i < 1024; i++) bytes[i] = (uint8_t)rng(s);
    for (i = 0; i < 256; i++) { halves[i] = (uint16_t)rng(s); words[i] = rng(s); }
}

__attribute__((noinline)) static uint32_t walk(void) {
    uint32_t h = 0;
    const uint8_t *p = bytes;
    const int8_t *sp = (const int8_t *)bytes + 1023;
    int i;
    for (i = 0; i < 1024; i++) h = (h << 1 | h >> 31) + *p++ + (uint32_t)(int32_t)*sp--;
    for (i = 255; i >= 0; i--) h ^= halves[i] + (uint32_t)(int32_t)(int16_t)halves[255 - i] + words[i] * (uint32_t)(i + 1);
    for (i = 0; i < 250; i += 3) { words[i] += words[i + 1]; halves[i] ^= (uint16_t)words[i + 2]; bytes[i * 4 + 1] = (uint8_t)(h >> i % 24); }
    return h;
}

__attribute__((noinline)) static uint32_t unaligned(void) {
    uint32_t h = 0, v;
    uint16_t w;
    int i;
    for (i = 0; i < 1000; i++) {
        memcpy(&v, bytes + i, 4);
        memcpy(&w, bytes + i + 1, 2);
        h = h * 33 + v + w;
        v ^= h;
        memcpy(bytes + ((i * 7) % 1000), &v, 4);
    }
    memmove(bytes + 10, bytes, 500);
    memmove(bytes + 300, bytes + 317, 400);
    for (i = 0; i < 1024; i++) h = h * 131 + bytes[i];
    return h;
}

int main(void) {
    uint32_t s = 0xCAFEF00Du;
    fill(&s);
    put_hex("rec", sum_rec(recs, 64));
    put_hex("walk", walk());
    put_hex("walk2", walk());
    put_hex("unaligned", unaligned());
    put_hex("rec2", sum_rec(recs + 3, 50));
    return 0;
}
