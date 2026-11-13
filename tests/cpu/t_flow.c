/* Calls, recursion, switch tables, function pointers and long-lived loops. */
#include "test.h"

__attribute__((noinline)) static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
__attribute__((noinline)) static uint32_t ack(uint32_t m, uint32_t n) {
    if (!m) return n + 1;
    if (!n) return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}

__attribute__((noinline)) static uint32_t sw(uint32_t op, uint32_t a, uint32_t b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a ^ b;
    case 4: return a | b;
    case 5: return a & b;
    case 6: return a << (b & 15);
    case 7: return a >> (b & 15);
    case 8: return ~a;
    case 9: return a + 1;
    case 10: return b - 1;
    case 11: return (a << 16) | (b & 0xFFFF);
    case 13: return a * 3 + b * 5;
    case 200: return 77;
    case 1000: return a;
    default: return op;
    }
}

typedef uint32_t (*binop)(uint32_t, uint32_t);
static uint32_t f_add(uint32_t a, uint32_t b) { return a + b; }
static uint32_t f_mul(uint32_t a, uint32_t b) { return a * b + 1; }
static uint32_t f_xor(uint32_t a, uint32_t b) { return a ^ (b << 1); }
static uint32_t f_rec(uint32_t a, uint32_t b) { return fib((a ^ b) & 15); }
static const binop table[4] = { f_add, f_mul, f_xor, f_rec };

static void sort(uint32_t *v, int lo, int hi, int (*lt)(uint32_t, uint32_t)) {
    int i = lo, j = hi;
    uint32_t p = v[(lo + hi) / 2];
    while (i <= j) {
        while (lt(v[i], p)) i++;
        while (lt(p, v[j])) j--;
        if (i <= j) { uint32_t t = v[i]; v[i] = v[j]; v[j] = t; i++; j--; }
    }
    if (lo < j) sort(v, lo, j, lt);
    if (i < hi) sort(v, i, hi, lt);
}
static int lt_u(uint32_t a, uint32_t b) { return a < b; }
static int lt_s(uint32_t a, uint32_t b) { return (int32_t)a > (int32_t)b; }

static uint32_t arr[500];

__attribute__((noinline)) static uint32_t deep(uint32_t n, uint32_t acc) {
    volatile uint32_t pad[4];
    pad[n & 3] = acc;
    if (!n) return acc;
    return deep(n - 1, acc * 3 + pad[n & 3]) + 1;
}

int main(void) {
    uint32_t s = 0xDEADBEEFu, h = 0;
    int i;
    put_hex("fib", fib(20));
    put_hex("ack", ack(2, 300));
    for (i = 0; i < 5000; i++) {
        uint32_t a = rng(&s), b = rng(&s);
        h = h * 17 + sw(a % 17, a, b) + sw(i == 77 ? 200 : i == 99 ? 1000 : 12, a, b);
        h ^= table[a & 3](a, b);
    }
    put_hex("switch", h);
    for (i = 0; i < 500; i++) arr[i] = rng(&s);
    sort(arr, 0, 499, lt_u);
    for (h = 0, i = 0; i < 500; i++) h = h * 31 + arr[i];
    put_hex("sort.u", h);
    sort(arr, 0, 499, lt_s);
    for (h = 0, i = 0; i < 500; i++) h = h * 31 + arr[i];
    put_hex("sort.s", h);
    put_hex("deep", deep(20000, 1));
    return 0;
}
