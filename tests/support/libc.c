/* The few library routines the test programs need. */
#include "test.h"

#ifdef __arm__
void sys_write0(const char *s);
void put(const char *s) { sys_write0(s); }

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp < sp) while (n--) *dp++ = *sp++;
    else while (n--) dp[n] = sp[n];
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *dp = d;
    while (n--) *dp++ = (unsigned char)c;
    return d;
}
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }

/* quotient in r0, remainder in r1 */
unsigned long long __aeabi_uidivmod(unsigned n, unsigned d) {
    unsigned q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    return ((unsigned long long)r << 32) | q;
}
unsigned __aeabi_uidiv(unsigned n, unsigned d) { return (unsigned)__aeabi_uidivmod(n, d); }
unsigned long long __aeabi_idivmod(int n, int d) {
    unsigned un = n < 0 ? 0u - (unsigned)n : (unsigned)n, ud = d < 0 ? 0u - (unsigned)d : (unsigned)d;
    unsigned long long qr = __aeabi_uidivmod(un, ud);
    unsigned q = (unsigned)qr, r = (unsigned)(qr >> 32);
    if ((n < 0) != (d < 0)) q = 0u - q;
    if (n < 0) r = 0u - r;
    return ((unsigned long long)r << 32) | q;
}
int __aeabi_idiv(int n, int d) { return (int)(unsigned)__aeabi_idivmod(n, d); }
#else
#include <stdio.h>
void put(const char *s) { fputs(s, stdout); }
#endif

void put_hex(const char *label, uint32_t v) {
    char buf[12];
    int i;
    for (i = 0; i < 8; i++) {
        unsigned d = (v >> (28 - 4 * i)) & 15;
        buf[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    buf[8] = '\n';
    buf[9] = 0;
    put(label);
    put(" ");
    put(buf);
}
