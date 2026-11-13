/* Arithmetic, logic, shifts, multiplies and comparisons on pseudo-random data. */
#include "test.h"

static uint32_t rotl(uint32_t v, unsigned n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

__attribute__((noinline)) static uint32_t mix32(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a + b;
    r ^= rotl(c, a & 31);
    r -= (b >> (c & 31)) | (a << (b & 31));
    r += (uint32_t)((int32_t)a >> (b & 31));
    r ^= a * b + c;
    r = (r & ~b) | (c & 0x00FF00FFu);
    if ((int32_t)a < (int32_t)b) r += 3;
    if (a < b) r ^= 0x55;
    if ((int32_t)a >= (int32_t)c) r -= 7;
    if (a > c) r = ~r;
    if (a == b || b == c) r++;
    return r;
}

__attribute__((noinline)) static uint64_t mix64(uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    r -= a >> 3;
    r ^= b << 7;
    r += (uint64_t)(uint32_t)a * (uint32_t)b;
    r += (uint64_t)((int64_t)(int32_t)(a >> 32) * (int64_t)(int32_t)b);
    r = (r << 13) | (r >> 51);
    if ((int64_t)a < (int64_t)b) r = 0 - r;
    if (a > b) r += 0x100000001ull;
    r ^= (uint64_t)((int64_t)a >> (b & 63));
    r += a << (r & 63);
    r -= b >> (a & 63);
    return r;
}

__attribute__((noinline)) static uint32_t divs(uint32_t a, uint32_t b, int32_t c, int32_t d) {
    uint32_t r = 0;
    if (b) r += a / b + a % b;
    if (d) r ^= (uint32_t)(c / d) + (uint32_t)(c % d);
    r += a / 10 + a % 10 + (uint32_t)(c / 7) + (uint32_t)(c % 1000);
    return r;
}

static uint32_t bits(uint32_t v) {
    uint32_t n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

__attribute__((noinline)) static uint32_t sat(int32_t a, int32_t b) {
    int64_t s = (int64_t)a + b;
    int32_t lo = a < b ? a : b, hi = a > b ? a : b;
    if (s > 0x7FFFFFFF) s = 0x7FFFFFFF;
    if (s < -0x7FFFFFFF - 1) s = -0x7FFFFFFF - 1;
    return (uint32_t)s ^ ((uint32_t)hi - (uint32_t)lo) ^ (a < 0 ? 0u - (uint32_t)a : (uint32_t)a) ^ bits((uint32_t)b);
}

int main(void) {
    uint32_t s = 0x12345678u, h32 = 0, hd = 0, hs = 0;
    uint64_t h64 = 0;
    int i;
    for (i = 0; i < 20000; i++) {
        uint32_t a = rng(&s), b = rng(&s), c = rng(&s);
        if ((i & 7) == 0) b = a;
        if ((i & 31) == 1) a = 0;
        if ((i & 63) == 2) c = 0x80000000u;
        h32 = rotl(h32, 5) ^ mix32(a, b, c);
        h64 = (h64 << 3 | h64 >> 61) ^ mix64(((uint64_t)a << 32) | b, ((uint64_t)c << 32) | rng(&s));
        hd += divs(a, b >> (c & 31), (int32_t)c, (int32_t)(a >> (b & 31)) - 1000);
        hs = rotl(hs, 3) + sat((int32_t)a, (int32_t)b);
    }
    put_hex("mix32", h32);
    put_hex("mix64.hi", (uint32_t)(h64 >> 32));
    put_hex("mix64.lo", (uint32_t)h64);
    put_hex("div", hd);
    put_hex("sat", hs);
    return 0;
}
