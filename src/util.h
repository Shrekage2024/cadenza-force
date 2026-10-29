/* Small integer helpers shared by the generators, the sequencer and the engine (each file kept its own copy before
 * 0.24.1). static inline: every file that includes it gets its own, as before; nothing to link. */
#ifndef UTIL_H
#define UTIL_H

/* a mod m in 0..m-1 for any sign of a (m > 0) */
static inline int pmod(int a, int m) { return ((a % m) + m) % m; }
/* a / m rounded down (m > 0) */
static inline int fdiv(int a, int m) { return (a - pmod(a, m)) / m; }
static inline int iabs(int x) { return x < 0 ? -x : x; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the murmur3 finalizer */
static inline unsigned fmix32(unsigned h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    return h ^ (h >> 16);
}

/* ascending, in place (insertion sort: the lists are short) */
static inline void sort_ints(int *a, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && a[j - 1] > a[j]; j--) { int x = a[j]; a[j] = a[j - 1]; a[j - 1] = x; }
}

#endif
