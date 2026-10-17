/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    if ((uintptr_t)p > (uintptr_t)q) {
        while (n) {
            --n;
            p[n] = q[n];
        }
    }
    else while (n--)
        *p++ = *q++;
    return d;
}

void *memset(void *d, int v, size_t n) {
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)v;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    while (n--) {
        if (*p != *q)
            return *p-*q;
        ++p;
        ++q;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

/* Unsigned restoring division: no vendor compiler-runtime dependency. */
uint32_t __udivsi3(uint32_t a, uint32_t b) {
    uint32_t q = 0, r = 0;
    if (!b)
        return 0;
    for (unsigned i = 32; i; --i) {
        uint32_t top = r >> 31;
        r = (r << 1) | ((a >> (i-1))&1u);
        if (top || r >= b) {
            r -= b;
            q |= 1u << (i-1);
        }
    }
    return q;
}

uint32_t __umodsi3(uint32_t a, uint32_t b) {
    return b ? a-__udivsi3(a, b)*b : 0;
}

int32_t __divsi3(int32_t a, int32_t b) {
    uint32_t ua = a < 0 ? 0u-(uint32_t)a:(uint32_t)a;
    uint32_t ub = b < 0 ? 0u-(uint32_t)b:(uint32_t)b;
    uint32_t q = __udivsi3(ua, ub);
    return (int32_t)((a < 0) != (b < 0) ? 0u-q:q);
}

int32_t __modsi3(int32_t a, int32_t b) {
    return b ? (int32_t)((uint32_t)a-(uint32_t)__divsi3(a, b)*(uint32_t)b) : 0;
}
