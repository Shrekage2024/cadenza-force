/* Offline test of src/gen.c (run by tests/run_tests.sh under ASan/UBSan). Exit 1 on failure. */
#include "../src/gen.h"
#include "../src/theory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static const int PROG[4] = {0, 5, 2, 6};   /* i-VI-III-VII */
static const int P3[3] = {0, 3, 4}, P8[8] = {0, 4, 5, 2, 3, 0, 3, 4};
static const int SEEDS[4] = {0, 1, 777, 65535};
static const int BARS[3] = {1, 4, 16};
static const int CLS[4] = {4, 8, 16, 32};
static const char *NAME[GEN_NSTYLES] = {"MANUAL", "CH_SUSTAIN", "CH_PULSE", "CH_STABS", "BASS_ROOT", "BASS_PUMP", "BASS_WALK",
                                        "MEL_HOOK", "MEL_ARP", "MEL_ANSWER", "MEL_RECIPE", "CH_OFFBEAT", "CH_CHARLESTON",
                                        "CH_BALLAD", "CH_FUNK", "CH_HALFTIME", "CH_PUSH"};

static int lane_of(int style) {
    return style <= GEN_CH_STABS || gen_is_chords_v2(style) ? LANE_CHORDS : style <= GEN_BASS_WALK ? LANE_BASS : style == GEN_MEL_ANSWER ? LANE_MEL_DOWN : LANE_MEL_UP;
}

/* the default test part: E minor, i-VI-III-VII */
static gen_params_t mk(int style, int seed, int c, int bars, int chord_len) {
    gen_params_t p = {style, seed, c, bars, chord_len, PROG, 4, 4, SCALE_MINOR, NULL, lane_of(style),
                      gen_chords_v2_variant(style, seed), NULL};   /* 0.13: the variant GEN stores with this seed */
    return p;
}

/* every (seed, bars, chord_len, c) of the property grid: k % 48 picks seed/bars/chord_len, k / 48 picks c = 0..100 */
#define NCASE (4 * 3 * 4 * 11)
#define C_STRIDE 48   /* case k + 48 is case k at c + 10 */

static gen_params_t case_params(int style, int k) {
    return mk(style, SEEDS[k % 4], (k / C_STRIDE) * 10, BARS[k / 4 % 3], CLS[k / 12 % 4]);
}

static int loop_len(const gen_params_t *p) { return p->bars * SEQ_STEPS; }

/* generate into a lane pre-filled with 0xAB, so untouched steps can be seen */
static void gen(const gen_params_t *p, seq_step_t *st) {
    memset(st, 0xAB, sizeof(seq_step_t) * SEQ_LANE_STEPS);
    gen_lane_v1(p, st);
}

static int count_on(const gen_params_t *p, const seq_step_t *st) {
    int n = 0;
    for (int i = 0; i < loop_len(p); i++) n += st[i].on;
    return n;
}

static int pmod(int a, int m) { return ((a % m) + m) % m; }
static int is_bass(int style) { return style >= GEN_BASS_ROOT; }

static void test_rng(void) {
    gen_rng r;
    gen_seed(&r, 0);
    CHECK(r.s != 0, "gen_seed(0) leaves a nonzero state");
    unsigned a = gen_next(&r), b = gen_next(&r);
    CHECK(a != 0 && b != 0 && a != b, "xorshift32 runs: %u, %u", a, b);
}

static void test_determinism(void) {
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    int same = 1;
    for (int st = 1; st < GEN_V1_STYLES; st++)
        for (int k = 0; k < NCASE; k += 7) {
            gen_params_t p = case_params(st, k);
            gen(&p, a);
            gen(&p, b);
            same &= !memcmp(a, b, sizeof a);
        }
    CHECK(same, "the same params give byte-identical steps");
}

static void test_manual(void) {
    static seq_step_t st[SEQ_LANE_STEPS], ref[SEQ_LANE_STEPS];
    gen_params_t p = mk(GEN_MANUAL, 1, 50, 4, 16);
    gen(&p, st);
    memset(ref, 0xAB, sizeof ref);
    CHECK(!memcmp(st, ref, sizeof st), "GEN_MANUAL leaves the steps untouched");
    seq_step_t sp[SEQ_LANE_STEPS];
    p = mk(GEN_MEL_RECIPE, 1, 50, 4, 16);
    p.lane = LANE_MEL_UP;
    gen(&p, st);
    memset(sp, 0xAB, sizeof sp);
    gen_span_v1(&p, 5, sp);
    CHECK(!memcmp(st, ref, sizeof st) && !memcmp(sp, ref, sizeof sp) && gen_reads_harmony(GEN_MEL_RECIPE) &&
              !gen_transposes(GEN_MEL_RECIPE),
          "GEN_MEL_RECIPE (melody.c's) is not gen_v1's: it writes nothing (lane, span), reads harmony, never transposes");
}

/* onsets at c are all still on at c + 10, and the count never drops */
static void test_nesting(int style) {
    static seq_step_t lo[SEQ_LANE_STEPS], hi[SEQ_LANE_STEPS];
    int lost = 0, fewer = 0;
    for (int k = 0; k + C_STRIDE < NCASE; k++) {
        gen_params_t p = case_params(style, k), q = case_params(style, k + C_STRIDE);
        gen(&p, lo);
        gen(&q, hi);
        for (int i = 0; i < loop_len(&p); i++) lost += lo[i].on && !hi[i].on;
        fewer += count_on(&q, hi) < count_on(&p, lo);
    }
    CHECK(!lost, "%s: every onset at c is still on at c + 10 (%d lost)", NAME[style], lost);
    CHECK(!fewer, "%s: the onset count never drops as c rises (%d drops)", NAME[style], fewer);
}

/* c = 100 has more onsets than c = 0 whenever the span allows: every style once spans are at least half a bar */
static void test_range(int style) {
    static seq_step_t lo[SEQ_LANE_STEPS], hi[SEQ_LANE_STEPS];
    int flat = 0;
    for (int k = 0; k < C_STRIDE; k++) {
        gen_params_t p = case_params(style, k), q = case_params(style, k + 10 * C_STRIDE);
        if (p.chord_len < 8) continue;
        gen(&p, lo);
        gen(&q, hi);
        flat += count_on(&q, hi) <= count_on(&p, lo);
    }
    CHECK(!flat, "%s: c = 100 has more onsets than c = 0 (%d cases flat)", NAME[style], flat);
}

/* one step's ranges: tone in the scale, octave in the lane's range, velocity, length, never past the loop end */
static int step_errors(const gen_params_t *p, const seq_step_t *s, int i) {
    int lo = is_bass(p->style) ? -2 : 0, hi = 0;   /* the walk places approach notes in -2..0 */
    int e = s->tone < TONE_R || s->tone > TONE_7;
    e += s->oct < lo || s->oct > hi;
    e += s->vel < 1 || s->vel > 127;
    e += s->len < 1 || s->len > 16 || i + s->len > loop_len(p);
    return e;
}

/* the walk's last onset in a span (if not its first) approaches the next chord's root by step, or anticipates it */
static int walk_errors(const gen_params_t *p, const seq_step_t *st, int span, int a, int e) {
    int last = e - 1;
    while (last > a && !st[last].on) last--;
    if (last == a) return 0;
    int nspans = (loop_len(p) + p->chord_len - 1) / p->chord_len;
    int d = PROG[(span + 1 == nspans ? 0 : span + 1) % 4] - PROG[span % 4], t = st[last].tone;
    return t != pmod(d - 1, 7) + 1 && t != pmod(d + 1, 7) + 1 && !(t == pmod(d, 7) + 1 && last == e - 1);
}

static int span_errors(const gen_params_t *p, const seq_step_t *st) {
    int errs = 0, n = loop_len(p);
    for (int span = 0, a = 0; a < n; span++, a += p->chord_len) {
        int e = a + p->chord_len < n ? a + p->chord_len : n;
        errs += !st[a].on;
        errs += is_bass(p->style) && st[a].tone != TONE_R;
        if (p->style == GEN_BASS_WALK) errs += walk_errors(p, st, span, a, e);
    }
    return errs;
}

static int rule_errors(const gen_params_t *p, const seq_step_t *st) {
    int errs = span_errors(p, st), n = loop_len(p);
    seq_step_t untouched;
    memset(&untouched, 0xAB, sizeof untouched);
    for (int i = 0; i < n; i++) errs += step_errors(p, &st[i], i);
    for (int i = n; i < SEQ_LANE_STEPS; i++) errs += !!memcmp(&st[i], &untouched, sizeof untouched);
    return errs;
}

static void test_rules(int style) {
    static seq_step_t st[SEQ_LANE_STEPS];
    int errs = 0;
    for (int k = 0; k < NCASE; k++) {
        gen_params_t p = case_params(style, k);
        gen(&p, st);
        errs += rule_errors(&p, st);
    }
    CHECK(!errs, "%s: rules hold at every seed, size and complexity (%d errors)", NAME[style], errs);
}

/* chord_len 32 in a 1-bar loop: one span cut at the loop end; 16 bars of 1/4-bar chords: 64 spans */
static void test_edges(int style) {
    static seq_step_t st[SEQ_LANE_STEPS];
    int errs = 0, starts = 0;
    for (int c = 0; c <= 100; c += 25) {
        gen_params_t cut = mk(style, 9, c, 1, 32), many = mk(style, 9, c, 16, 4);
        gen(&cut, st);
        errs += rule_errors(&cut, st);
        gen(&many, st);
        errs += rule_errors(&many, st);
        for (int i = 0; i < SEQ_LANE_STEPS; i += 4) starts += st[i].on;
    }
    CHECK(!errs && starts == 5 * 64, "%s: a 32-step chord in 1 bar and 64 quarter-bar chords (%d errors, %d/320 starts)",
          NAME[style], errs, starts);
}

static void test_shapes(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    gen_params_t sus = mk(GEN_CH_SUSTAIN, 5, 0, 4, 16), root = mk(GEN_BASS_ROOT, 5, 100, 4, 16);
    gen(&sus, st);
    CHECK(count_on(&sus, st) == 4 && st[0].len == 16, "CH_SUSTAIN at c = 0: one whole-span chord per span");
    gen(&root, st);
    int roots = 1;
    for (int i = 0; i < loop_len(&root); i++) roots &= !st[i].on || st[i].tone == TONE_R;
    CHECK(roots, "BASS_ROOT plays only roots, even at c = 100");
}

static void test_timing(void) {
    static seq_step_t st[SEQ_LANES][SEQ_LANE_STEPS];
    const int styles[SEQ_LANES] = {GEN_CH_STABS, GEN_BASS_WALK, GEN_CH_PULSE, GEN_BASS_PUMP};
    clock_t t0 = clock();
    for (int loop = 0; loop < 100; loop++)
        for (int l = 0; l < SEQ_LANES; l++) {
            gen_params_t p = mk(styles[l], loop * 131 + l, loop % 101, 16, 4 << loop % 4);   /* chord_len 4..32 */
            gen_lane_v1(&p, st[l]);
        }
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 100;
    CHECK(ms < 1.0, "a 4-lane 16-bar regen takes %.4f ms on average (budget 1 ms)", ms);
}

/* Longuet-Higgins & Lee syncopation of a loop: a note on a weaker position followed (before the next note, wrapping)
 * by a rest on a stronger one scores the level difference. Levels: lower = stronger. */
static const int LHL_LEVEL[SEQ_STEPS] = {0, 4, 3, 4, 2, 4, 3, 4, 1, 4, 3, 4, 2, 4, 3, 4};

static int lhl(const seq_step_t *st, int n) {
    int score = 0;
    for (int i = 0; i < n; i++) {
        if (!st[i].on) continue;
        int strongest = 5;
        for (int j = (i + 1) % n; !st[j].on; j = (j + 1) % n)
            if (LHL_LEVEL[j % SEQ_STEPS] < strongest) strongest = LHL_LEVEL[j % SEQ_STEPS];
        if (strongest < LHL_LEVEL[i % SEQ_STEPS]) score += LHL_LEVEL[i % SEQ_STEPS] - strongest;
    }
    return score;
}

static double mean_lhl(int style, int c, int cl) {
    static seq_step_t st[SEQ_LANE_STEPS];
    long sum = 0;
    for (int seed = 0; seed < 200; seed++) {
        gen_params_t p = mk(style, seed, c, 4, cl);
        gen(&p, st);
        sum += lhl(st, loop_len(&p));
    }
    return sum / 200.0;
}

/* none at c = 0 and rising with c for chord lengths 4, 8 and 16; real syncopation at c = 100 for every style but
 * Sustain and Root (whole-bar chords) */
static void test_syncopation(int style, int cl) {
    double m[11];
    int rising = 1;
    for (int k = 0; k <= 10; k++) m[k] = mean_lhl(style, k * 10, cl);
    for (int k = 1; k <= 10; k++) rising &= m[k] >= m[k - 1] - 0.05;
    int want = cl == 16 && style != GEN_CH_SUSTAIN && style != GEN_BASS_ROOT;
    CHECK(m[0] == 0 && rising && (!want || m[10] > 0),
          "%s, chord_len %d: mean LHL syncopation c0 %.2f, c50 %.2f, c100 %.2f, rising", NAME[style], cl, m[0], m[5], m[10]);
}

/* The knob spreads its onsets: whole-bar chords gain nothing from c = 0 to 5, and quarter-bar Pulse gains its one
 * extra onset mid-knob rather than at c = 1. */
static void test_spread(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    int early = 0;
    for (int style = 1; style < GEN_V1_STYLES; style++)
        for (int seed = 0; seed < 20; seed++) {
            gen_params_t p = mk(style, seed, 0, 4, 16), q = mk(style, seed, 5, 4, 16);
            gen(&p, st);
            int n0 = count_on(&p, st);
            gen(&q, st);
            early += count_on(&q, st) != n0;
        }
    CHECK(!early, "chord_len 16: no style's onset count changes from c = 0 to c = 5 (%d changed)", early);
    gen_params_t a = mk(GEN_CH_PULSE, 3, 30, 4, 4), b = mk(GEN_CH_PULSE, 3, 70, 4, 4);
    gen(&a, st);
    int n30 = count_on(&a, st);
    gen(&b, st);
    CHECK(count_on(&b, st) > n30, "chord_len 4: CH_PULSE's onset count changes within c 30..70 (%d -> %d)", n30,
          count_on(&b, st));
}

static int seen_once(unsigned char *seen, unsigned m) {   /* 1 the first time m (16 bits) shows up */
    int fresh = !(seen[m >> 3] >> (m & 7) & 1);
    seen[m >> 3] |= (unsigned char)(1 << (m & 7));
    return fresh;
}

/* 1 if the walk's last onset of the span starting at a approaches its next root from above */
static int approach_up(const seq_step_t *st, int span, int a) {
    int last = a + 15, d = PROG[(span + 1) % 16 % 4] - PROG[span % 4];
    while (last > a && !st[last].on) last--;
    return st[last].tone == pmod(d + 1, 7) + 1;
}

/* Seeds give many variants of each yes/no choice: the walk's anticipations and approach sides over 16 spans. */
static void test_seed_variety(void) {
    static unsigned char ant[65536 / 8], side[65536 / 8];
    static seq_step_t st[SEQ_LANE_STEPS];
    int nant = 0, nside = 0;
    for (int seed = 0; seed < 1000; seed++) {
        gen_params_t hi = mk(GEN_BASS_WALK, seed, 100, 16, 16), lo = mk(GEN_BASS_WALK, seed, 0, 16, 16);
        unsigned m = 0, u = 0;
        gen(&hi, st);
        for (int s = 0; s < 16; s++) m |= (unsigned)st[s * 16 + 15].on << s;
        gen(&lo, st);
        for (int s = 0; s < 16; s++) u |= (unsigned)approach_up(st, s, s * 16) << s;
        nant += seen_once(ant, m);
        nside += seen_once(side, u);
    }
    CHECK(nant >= 50 && nside >= 50, "seeds 0..999: %d walk anticipation patterns, %d approach-side patterns (>= 50)",
          nant, nside);
}

/* |semitones| from the walk's last note in a span to the next root as played; -1 if the span has one note.
 * *ant is set when that note is the anticipated root. */
static int approach_dist(const gen_params_t *p, const seq_step_t *st, int span, int *ant) {
    int a = span * 16, e = a + 16, last = e - 1, cur = PROG[span % 4], next = PROG[(span + 1) % 4];
    while (last > a && !st[last].on) last--;
    if (last == a) return -1;
    int n1 = theory_tone_note(p->key, p->scale, cur, st[last].tone, st[last].oct);
    int n2 = theory_tone_note(p->key, p->scale, next, TONE_R, st[e % 64].oct);
    *ant = st[last].tone == pmod(next - cur, 7) + 1;
    return n1 > n2 ? n1 - n2 : n2 - n1;
}

/* Walk approach notes are a scale step from the next root: 1-2 semitones in the 0.6 scales, 1-3 in the newer ones
 * (harmonic minor has a 3-semitone step); an anticipation is that root's exact pitch. */
static void test_walk_pitch(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    for (int pass = 0; pass < 2; pass++) {
        int lo = pass ? SCALE_V1_COUNT : 0, hi = pass ? SCALE_COUNT : SCALE_V1_COUNT, maxd = pass ? 3 : 2;
        int bad = 0, hist[5] = {0}, ants = 0;
        for (int key = 0; key < 12; key++)
            for (int scale = lo; scale < hi; scale++)
                for (int seed = 0; seed < 50; seed++)
                    for (int c = 40; c <= 80; c += 40) {
                        gen_params_t p = {GEN_BASS_WALK, seed, c, 4, 16, PROG, 4, key, scale, NULL, LANE_BASS, 0, NULL};
                        gen(&p, st);
                        for (int span = 0, ant = 0; span < 4; span++) {
                            int d = approach_dist(&p, st, span, &ant);
                            if (d < 0) continue;
                            bad += ant ? d != 0 : d < 1 || d > maxd;
                            ants += ant;
                            hist[d < 4 ? d : 4]++;
                        }
                    }
        CHECK(!bad, "walk approach notes 1-%d semitones from the next root, %s scales (0:%d 1:%d 2:%d 3:%d 4+:%d, %d anticipations), %d bad",
              maxd, pass ? "new" : "0.6", hist[0], hist[1], hist[2], hist[3], hist[4], ants, bad);
    }
}

/* Root: a step's octave never changes with c; the roots that join above c = 60 sound an octave up. */
static void test_root_octave(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    int bad = 0, high = 0;
    for (int seed = 0; seed < 50; seed++)
        for (int cl = 4; cl <= 32; cl *= 2) {
            signed char oct[64];
            memset(oct, 9, sizeof oct);
            for (int c = 0; c <= 100; c++) {
                gen_params_t p = mk(GEN_BASS_ROOT, seed, c, 4, cl);
                gen(&p, st);
                for (int i = 0; i < 64; i++) {
                    if (!st[i].on) continue;
                    bad += oct[i] != 9 && oct[i] != st[i].oct;
                    bad += c <= 60 && st[i].oct != -1;
                    oct[i] = st[i].oct;
                    high += c == 100 && st[i].oct == 0;
                }
            }
        }
    CHECK(!bad && high > 0, "BASS_ROOT: a step's octave is the same at every c, up only above c = 60 (%d off, %d up)",
          bad, high);
}

/* Pump: low on beats, high on the "&" 8ths and the 16ths, whatever else is on. */
static void test_pump_octave(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    int bad = 0;
    for (int seed = 0; seed < 50; seed++)
        for (int c = 0; c <= 100; c += 5)
            for (int cl = 4; cl <= 16; cl *= 4) {
                gen_params_t p = mk(GEN_BASS_PUMP, seed, c, 4, cl);
                gen(&p, st);
                for (int i = 0; i < loop_len(&p); i++) bad += st[i].on && st[i].oct != (i % 4 ? 0 : -1);
            }
    CHECK(!bad, "BASS_PUMP low on beats, high on the & 8ths and 16ths, at every c (%d off)", bad);
}

/* Sustain and Root at c = 0 with 2-bar chords re-attack each bar, so the loop never falls silent. */
static void test_sustain_cover(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    const int styles[2] = {GEN_CH_SUSTAIN, GEN_BASS_ROOT};
    int gaps = 0;
    for (int s = 0; s < 2; s++)
        for (int seed = 0; seed < 50; seed++) {
            gen_params_t p = mk(styles[s], seed, 0, 4, 32);
            unsigned char cover[64] = {0};
            gen(&p, st);
            for (int i = 0; i < 64; i++)
                for (int j = 0; st[i].on && j < st[i].len; j++) cover[i + j] = 1;
            for (int i = 0; i < 64; i++) gaps += !cover[i];
        }
    CHECK(!gaps, "CH_SUSTAIN and BASS_ROOT at c = 0, 2-bar chords: every step sounds (%d silent)", gaps);
}

static int is_middle(const seq_step_t *st, int i, int cl) {   /* on, not its span's first or last onset */
    int a = i / cl * cl, e = a + cl, j = i + 1;
    while (j < e && !st[j].on) j++;
    return st[i].on && i != a && j < e;
}

/* A middle walk note's tone at c and c + 1 differs only by switching between its chord tone and its passing tone. */
static void test_walk_tone_stable(void) {
    static seq_step_t lo[SEQ_LANE_STEPS], hi[SEQ_LANE_STEPS];
    int bad = 0;
    for (int seed = 0; seed < 50; seed++)
        for (int cl = 8; cl <= 16; cl *= 2)
            for (int c = 0; c < 100; c++) {
                gen_params_t p = mk(GEN_BASS_WALK, seed, c, 4, cl), q = mk(GEN_BASS_WALK, seed, c + 1, 4, cl);
                gen(&p, lo);
                gen(&q, hi);
                for (int i = 0; i < 64; i++)
                    if (is_middle(lo, i, cl) && is_middle(hi, i, cl) && lo[i].tone != hi[i].tone)
                        bad += lo[i].tone % 2 == hi[i].tone % 2;   /* chord tones are odd (R, 3, 5), passing even */
            }
    CHECK(!bad, "BASS_WALK middle tones only switch chord tone <-> passing tone as c rises (%d re-rolls)", bad);
}

static void test_stabs_gate(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    int long_ = 0;
    for (int k = 0; k < NCASE; k++) {
        gen_params_t p = case_params(GEN_CH_STABS, k);
        gen(&p, st);
        for (int i = 0; i < loop_len(&p); i++) long_ += st[i].on && st[i].len > 2;
    }
    CHECK(!long_, "CH_STABS stay short (len <= 2) at every c (%d longer)", long_);
}

/* HARMONY: the degrees each complexity may use, and the chords a progression may end on (bit d = degree d) */
static unsigned prog_tier(int c) { return c < 34 ? 0x39u : c < 67 ? 0x3Fu : 0x7Fu; }   /* {0,3,4,5} +{1,2} +{6} */
static unsigned prog_ends(int c) { return c < 34 ? 0x38u : c < 67 ? 0x3Au : 0x7Au; }   /* {3,4,5} +{1} +{6} */

/* gen_progression_v1 into a buffer pre-filled with -1 past len, so a write past the end can be seen */
static void prog(int seed, int c, int len, int out[9]) {
    for (int i = 0; i < 9; i++) out[i] = -1;
    gen_progression_v1(seed, c, len, out);
}

static int prog_errors(int c, int len, const int *out) {
    int e = out[0] != 0 || out[len] != -1 || !(prog_ends(c) >> out[len - 1] & 1);
    for (int i = 0; i < len; i++) e += out[i] < 0 || out[i] > 6 || !(prog_tier(c) >> out[i] & 1);
    for (int i = 1; i < len && c < 67; i++) e += out[i] == out[i - 1];   /* no chord followed by itself below c 67 */
    return e;
}

/* every seed 0..199, complexity 0..100 and length 2..8: same output twice, tonic first, tier, ending, no repeats */
static void test_prog_rules(void) {
    int a[9], b[9], errs = 0, diff = 0;
    for (int seed = 0; seed < 200; seed++)
        for (int c = 0; c <= 100; c++)
            for (int len = 2; len <= 8; len++) {
                prog(seed, c, len, a);
                prog(seed, c, len, b);
                diff += !!memcmp(a, b, sizeof a);
                errs += prog_errors(c, len, a);
            }
    CHECK(!diff, "gen_progression_v1: the same (seed, c, len) gives the same progression (%d differ)", diff);
    CHECK(!errs, "gen_progression_v1: starts on 0, tier degrees only, allowed ending, no repeat below c 67 (%d errors)",
          errs);
}

static unsigned prog_code(const int *out, int len) {   /* a progression of up to 8 degrees as 3 bits each */
    unsigned m = 0;
    for (int i = 0; i < len; i++) m = m << 3 | (unsigned)out[i];
    return m;
}

static unsigned char seen4[4096 / 8];   /* the 4-chord progressions distinct4 saw last, by prog_code */

static int distinct4(int seeds, int c) {   /* distinct 4-chord progressions over seeds 0..seeds-1 */
    unsigned char *seen = seen4;
    int out[9], n = 0;
    memset(seen, 0, sizeof seen4);
    for (int seed = 0; seed < seeds; seed++) {
        prog(seed, c, 4, out);
        unsigned m = prog_code(out, 4);
        n += !(seen[m >> 3] >> (m & 7) & 1);
        seen[m >> 3] |= (unsigned char)(1 << (m & 7));
    }
    return n;
}

/* Seeds give varied progressions, and every degree of a tier shows up somewhere (the tiers aren't dead weight). */
static void test_prog_variety(void) {
    const int pop[4] = {0, 4, 5, 3};   /* I-V-vi-IV */
    int d0 = distinct4(200, 0), out[9];
    unsigned m = prog_code(pop, 4);
    int has_pop = seen4[m >> 3] >> (m & 7) & 1, d50 = distinct4(200, 50);
    CHECK(d0 >= 12 && has_pop && d50 >= 20,
          "4-chord progressions over 200 seeds: %d distinct at c 0 (>= 12, I-V-vi-IV %s), %d at c 50 (>= 20)", d0,
          has_pop ? "found" : "MISSING", d50);
    const int cs[3] = {0, 50, 100};
    for (int k = 0; k < 3; k++) {
        unsigned used = 0;
        for (int seed = 0; seed < 200; seed++)
            for (int len = 4; len <= 8; len += 4) {
                prog(seed, cs[k], len, out);
                for (int i = 0; i < len; i++) used |= 1u << out[i];
            }
        CHECK(used == prog_tier(cs[k]), "c %d: every degree of the tier is used (mask 0x%02x)", cs[k], used);
    }
}

/* Lengths outside 2..8: 1 is the tonic alone, 0 writes nothing. */
static void test_prog_edges(void) {
    int out[9];
    prog(5, 50, 1, out);
    CHECK(out[0] == 0 && out[1] == -1, "len 1 gives the tonic alone");
    prog(5, 50, 0, out);
    CHECK(out[0] == -1, "len 0 writes nothing");
}

/* every third step's span alone vs the whole loop: *bad counts spans that differ, *outside spans that wrote elsewhere */
static void span_case(const gen_params_t *p, int *bad, int *outside) {
    static seq_step_t whole[SEQ_LANE_STEPS], one[SEQ_LANE_STEPS], ref[SEQ_LANE_STEPS];
    gen(p, whole);
    for (int i = 0; i < loop_len(p); i += 3) {
        int a = i / p->chord_len * p->chord_len, e = a + p->chord_len < loop_len(p) ? a + p->chord_len : loop_len(p);
        memset(one, 0xAB, sizeof one);
        gen_span_v1(p, i, one);
        *bad += memcmp(&one[a], &whole[a], sizeof(seq_step_t) * (size_t)(e - a)) != 0;
        memcpy(ref, one, sizeof ref);
        memset(&ref[a], 0xAB, sizeof(seq_step_t) * (size_t)(e - a));
        memset(one, 0xAB, sizeof one);
        *outside += memcmp(&ref, &one, sizeof one) != 0;
    }
}

/* gen_span_v1 writes exactly gen_lane_v1's steps of the span under the step, and nothing else: over the grid, and over
 * 2- and 8-bar loops, whose phrase ends vary */
static void test_span(void) {
    int bad = 0, outside = 0;
    for (int st = 1; st < GEN_V1_STYLES; st++) {
        for (int k = 0; k < NCASE; k += 5) {
            gen_params_t p = case_params(st, k);
            span_case(&p, &bad, &outside);
        }
        for (int k = 0; k < 2 * 4 * 4 * 3; k++) {
            gen_params_t p = mk(st, SEEDS[k % 4], k / 32 * 50, k / 4 % 2 ? 8 : 2, CLS[k / 8 % 4]);
            span_case(&p, &bad, &outside);
        }
    }
    CHECK(!bad && !outside, "one span alone is the loop's span (%d differ), the rest untouched (%d)", bad, outside);
}

static unsigned bar_rhythm(const seq_step_t *st, int bar) {   /* a bar's onsets as 16 bits */
    unsigned m = 0;
    for (int i = 0; i < SEQ_STEPS; i++) m |= (unsigned)st[bar * SEQ_STEPS + i].on << i;
    return m;
}

static int most_alike(const seq_step_t *st, int group) {   /* the most bars of a 4-bar group sharing one rhythm */
    int best = 0;
    for (int a = 0; a < 4; a++) {
        int n = 0;
        for (int b = 0; b < 4; b++) n += bar_rhythm(st, group * 4 + a) == bar_rhythm(st, group * 4 + b);
        if (n > best) best = n;
    }
    return best;
}

static int distinct_bars(const seq_step_t *st, int bars) {
    int n = 0;
    for (int b = 0; b < bars; b++) {
        int seen = 0;
        for (int j = 0; j < b; j++) seen |= bar_rhythm(st, j) == bar_rhythm(st, b);
        n += !seen;
    }
    return n;
}

/* over 200 seeds, 8 bars of whole-bar chords: *loose counts 4-bar groups with fewer than 3 alike bars; returns the
 * mean number of distinct bar rhythms */
static double repetition(int style, int c, int *loose) {
    static seq_step_t st[SEQ_LANE_STEPS];
    long distinct = 0;
    for (int seed = 0; seed < 200; seed++) {
        gen_params_t p = mk(style, seed, c, 8, 16);
        gen(&p, st);
        distinct += distinct_bars(st, 8);
        for (int g = 0; g < 2; g++) *loose += most_alike(st, g) < 3;
    }
    return distinct / 200.0;
}

/* Bars repeat as a motif: at c = 0 each 4-bar group has 3 alike bars; at c = 50 at most 4 distinct bar rhythms in 8
 * on average, and Stabs and Pump vary at phrase ends. */
static void test_repetition(int style) {
    int loose0 = 0, loose = 0;
    double d0 = repetition(style, 0, &loose0), d50 = repetition(style, 50, &loose), d100 = repetition(style, 100, &loose);
    int busy = style == GEN_CH_PULSE || style == GEN_CH_STABS || style == GEN_BASS_PUMP || style == GEN_BASS_WALK;
    int varies = style == GEN_CH_STABS || style == GEN_BASS_PUMP;
    CHECK(!loose0 && (!busy || d50 <= 4) && (!varies || d50 > 1),
          "%s: distinct bar rhythms per 8 bars c0 %.2f, c50 %.2f, c100 %.2f; 4-bar groups under 3 alike at c0: %d",
          NAME[style], d0, d50, d100, loose0);
}

/* only the styles gen_reads_harmony names change with the key, the scale or the progression: the others regenerate
 * to the same steps, so a key, scale or progression change skips them */
static void test_reads_harmony(void) {
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    CHECK(!gen_reads_harmony(GEN_MANUAL) && gen_reads_harmony(GEN_NSTYLES) && gen_reads_harmony(GEN_NSTYLES + 5),
          "a deny-list: MANUAL ignores harmony, a style id this build doesn't know is assumed to read it");
    for (int st = 1; st < GEN_V1_STYLES; st++) {
        int differ = 0;
        for (int k = 0; k < NCASE; k += 3) {
            gen_params_t p = case_params(st, k), q = p;
            q.key = k % 12;
            q.scale = k / 12 % SCALE_COUNT;
            q.prog = k % 2 ? P3 : P8;
            q.prog_len = k % 2 ? 3 : 8;
            gen(&p, a);
            gen(&q, b);
            differ += memcmp(a, b, sizeof a) != 0;
        }
        CHECK(gen_reads_harmony(st) ? differ > 0 : !differ, "%s: %s the key, scale and progression (%d of %d cases differ)",
              NAME[st], gen_reads_harmony(st) ? "reads" : "ignores", differ, (NCASE + 2) / 3);
    }
}

/* ---- Melodies ---- */

static const int METRIC_GRID[SEQ_STEPS] = {16, 1, 4, 1, 8, 1, 4, 1, 12, 1, 4, 1, 8, 1, 4, 1};

static int is_ct(int tone) { return tone == TONE_R || tone == TONE_3 || tone == TONE_5; }
/* the tonic nearest G4, an octave up on Melody 2 */
static int mel_centre(const gen_params_t *p) {
    int up = p->style == GEN_MEL_ANSWER || p->lane == LANE_MEL_DOWN;
    return 67 + pmod(theory_degree_pc(p->key, p->scale, 0) - 1, 12) - 6 + (up ? 12 : 0);
}
static int mel_range(const gen_params_t *p) {
    return p->style == GEN_MEL_ARP ? 6 + 6 * p->complexity / 100 : 4 + 8 * p->complexity / 100;
}
static int deg_at(const gen_params_t *p, int i) { return p->prog[(i / p->chord_len) % p->prog_len]; }
static int pitch_at(const gen_params_t *p, const seq_step_t *st, int i) {
    return theory_tone_note(p->key, p->scale, deg_at(p, i), st[i].tone, st[i].oct);
}

/* the melody voices under test: Hook on Melody 1, Arp on either lane, Answer on Melody 2 */
#define NVOICE 5
static const int VSTYLE[NVOICE] = {GEN_MEL_HOOK, GEN_MEL_ARP, GEN_MEL_ARP, GEN_MEL_ANSWER, GEN_MEL_ANSWER};
static const int VLANE[NVOICE] = {LANE_MEL_UP, LANE_MEL_UP, LANE_MEL_DOWN, LANE_MEL_DOWN, LANE_MEL_DOWN};
static const int VLEAD[NVOICE] = {0, 0, 0, 1, 0};   /* the Answer with a Hook lead, and alone */
static const char *VNAME[NVOICE] = {"MEL_HOOK", "MEL_ARP (Melody 1)", "MEL_ARP (Melody 2)", "MEL_ANSWER", "MEL_ANSWER (no lead)"};

/* a melody case of the grid: k % 64 picks seed, bars {1,2,4,8}, chord_len and key/scale; k / 64 picks c = 0..100 */
#define MCASE (4 * 4 * 4 * 11)
#define M_STRIDE 64
static const int MBARS[4] = {1, 2, 4, 8};
static seq_step_t mlead[SEQ_LANE_STEPS];

static gen_params_t mel_case(int v, int k) {
    int seed = SEEDS[k % 4], bars = MBARS[k / 4 % 4], cl = CLS[k / 16 % 4], c = k / M_STRIDE * 10, style = VSTYLE[v];
    gen_params_t p = {style, seed, c, bars, cl, PROG, 4, (k * 5) % 12, k % SCALE_COUNT, NULL, VLANE[v], 0, NULL};
    if (VLEAD[v]) {   /* Melody 1's Hook at c 50 with its own seed: fixed across c */
        gen_params_t h = p;
        h.style = GEN_MEL_HOOK, h.seed = seed + 99, h.complexity = 50, h.lane = LANE_MEL_UP;
        gen(&h, mlead);
        p.lead = mlead;
    }
    return p;
}

static int lead_sounds(const gen_params_t *p, int i) {   /* the lead has a note sounding at step i */
    for (int j = 0; p->lead && j <= i; j++)
        if (p->lead[j].on && i < j + (p->lead[j].len < 1 ? 1 : p->lead[j].len)) return 1;
    return 0;
}

static void mel_test_nesting(int v) {
    static seq_step_t lo[SEQ_LANE_STEPS], hi[SEQ_LANE_STEPS];
    int lost = 0;
    for (int k = 0; k + M_STRIDE < MCASE; k++) {
        gen_params_t p = mel_case(v, k), q = mel_case(v, k + M_STRIDE);
        gen(&p, lo);
        gen(&q, hi);   /* the lead is the same for both */
        for (int i = 0; i < loop_len(&p); i++) lost += lo[i].on && !hi[i].on;
    }
    CHECK(!lost, "%s: every onset at c is still on at c + 10 (%d lost)", VNAME[v], lost);
}

/* the chord-change rule: a melody's first onset in each span is R, 3 or 5; it sits on the chord change itself, except
 * for an Answer below c 50 whose lead sounds there */
static int mel_span_errors(const gen_params_t *p, const seq_step_t *st) {
    int errs = 0, n = loop_len(p);
    for (int a = 0; a < n; a += p->chord_len) {
        int e = a + p->chord_len < n ? a + p->chord_len : n, f = a;
        while (f < e && !st[f].on) f++;
        int must = p->style != GEN_MEL_ANSWER || p->complexity >= 50 || !lead_sounds(p, a);
        errs += must && f != a;
        errs += f < e && !is_ct(st[f].tone);
    }
    return errs;
}

/* a non-chord tone: weak position, not the previous pitch, the next onset 1-2 semitones away */
static int nct_errors(const gen_params_t *p, const seq_step_t *st, int i, int prev) {
    int arp7 = p->style == GEN_MEL_ARP && p->complexity > 60 && st[i].tone == TONE_7, n = loop_len(p), j = i + 1;
    if (is_ct(st[i].tone) || arp7) return 0;
    while (j < n && !st[j].on) j++;
    if (j == n) return 1;
    int d = pitch_at(p, st, j) - pitch_at(p, st, i);
    return (METRIC_GRID[i % SEQ_STEPS] >= 8) + (prev == pitch_at(p, st, i)) + (d < -2 || d > 2 || d == 0);
}

/* phrases: a chord span, or a bar under 1-bar chords; pairs (call, answer), a lone last phrase answers */
static int phrase_errors(const gen_params_t *p, const seq_step_t *st) {
    int n = loop_len(p), pl = p->chord_len < 16 ? 16 : p->chord_len, np = (n + pl - 1) / pl, errs = 0;
    for (int ph = 0; ph < np; ph++) {
        int answer = ph % 2 || (ph == np - 1 && np % 2), last = (ph + 1) * pl < n ? (ph + 1) * pl - 1 : n - 1;
        while (last >= ph * pl && !st[last].on) last--;
        errs += answer && last >= ph * pl && !is_ct(st[last].tone);
    }
    return errs;
}

/* Below c 50 an Answer may sound with its lead only as the fallback: on a phrase's first step, as its one onset there
 * (the lead left the phrase no gap) */
static int answer_fallback(const gen_params_t *p, const seq_step_t *st, int i) {
    int pl = p->chord_len < 16 ? 16 : p->chord_len, ps = i / pl * pl, n = 0;
    for (int j = ps; j < ps + pl && j < loop_len(p); j++) n += st[j].on;
    return i == ps && n == 1;
}

static int mel_step_errors(const gen_params_t *p, const seq_step_t *st, int i) {
    int base = p->lane == LANE_MEL_DOWN ? 2 : 1, d = pitch_at(p, st, i) - mel_centre(p);
    int e = st[i].tone < TONE_R || st[i].tone > TONE_7;
    e += st[i].oct < base - 2 || st[i].oct > base + 2;
    e += d < -mel_range(p) || d > mel_range(p);
    e += st[i].vel < 1 || st[i].vel > 127 || st[i].len < 1 || st[i].len > 16 || i + st[i].len > loop_len(p);
    e += p->style == GEN_MEL_ANSWER && p->complexity < 50 && lead_sounds(p, i) && !answer_fallback(p, st, i);
    return e;
}

static int mel_rule_errors(const gen_params_t *p, const seq_step_t *st) {
    int errs = mel_span_errors(p, st) + phrase_errors(p, st), prev = -1, last = -1, n = loop_len(p);
    for (int i = 0; i < n; i++) {
        if (!st[i].on) continue;
        errs += mel_step_errors(p, st, i) + nct_errors(p, st, i, prev);
        prev = pitch_at(p, st, i);
        last = i;
    }
    errs += last >= 0 && !is_ct(st[last].tone);
    seq_step_t untouched;
    memset(&untouched, 0xAB, sizeof untouched);
    for (int i = n; i < SEQ_LANE_STEPS; i++) errs += !!memcmp(&st[i], &untouched, sizeof untouched);
    return errs;
}

static void mel_test_rules(int v) {
    static seq_step_t st[SEQ_LANE_STEPS], again[SEQ_LANE_STEPS];
    int errs = 0, same = 1, ons = 0;
    for (int k = 0; k < MCASE; k++) {
        gen_params_t p = mel_case(v, k);
        gen(&p, st);
        gen(&p, again);
        same &= !memcmp(st, again, sizeof st);
        errs += mel_rule_errors(&p, st);
        ons += count_on(&p, st);
    }
    CHECK(!errs && same && ons > MCASE, "%s: deterministic, and the rules hold at every seed, size, key and complexity "
          "(%d errors, %d onsets)", VNAME[v], errs, ons);
}

/* interval statistics of a melody over the presets x keys x seeds grid (4 bars of whole-bar chords) */
typedef struct { long ivs, steps, unis, leaps, back, loops, run4, wrap7, moves, contra; } mstat_t;

static int lead_pitch(const gen_params_t *p, int i) {   /* the lead's pitch at its latest onset up to step i, -1 none */
    while (i >= 0 && !p->lead[i].on) i--;
    return i < 0 ? -1 : pitch_at(p, p->lead, i);
}

static void mstat_loop(mstat_t *m, const gen_params_t *p, const seq_step_t *st) {
    int a = -1, b = -1, run = 1, most = 1, first = -1, prev_i = 0;
    for (int i = 0; i < loop_len(p); i++) {
        if (!st[i].on) continue;
        int x = pitch_at(p, st, i);
        if (first < 0) first = x;
        if (b >= 0) m->ivs++, m->steps += abs(x - b) >= 1 && abs(x - b) <= 2, m->unis += x == b;
        run = b == x ? run + 1 : 1;
        most = run > most ? run : most;
        if (a >= 0 && abs(b - a) >= 5) m->leaps++, m->back += (x - b) * (b - a) < 0;
        if (p->lead && b >= 0 && lead_pitch(p, i) >= 0 && lead_pitch(p, prev_i) >= 0) {   /* both moved: opposite ways? */
            int dl = lead_pitch(p, i) - lead_pitch(p, prev_i);
            if (dl && x != b) m->moves++, m->contra += (x - b) * dl < 0;
        }
        a = b, b = x, prev_i = i;
    }
    m->loops++, m->run4 += most >= 4, m->wrap7 += b >= 0 && abs(b - first) >= 7;
}

static mstat_t mel_stats(int style, int c) {
    static seq_step_t st[SEQ_LANE_STEPS], lead[SEQ_LANE_STEPS];
    mstat_t m = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int pr = 0; pr < THEORY_NPRESETS; pr++)
        for (int seed = 0; seed < 40; seed++) {
            const theory_preset_t *r = &THEORY_PRESETS[pr];
            gen_params_t h = {GEN_MEL_HOOK, seed + 1, c, 4, 16, r->deg, r->len, (seed * 7 + pr) % 12, (seed + pr) % SCALE_COUNT,
                              NULL, LANE_MEL_UP, 0, NULL}, p = h;
            gen(&h, lead);
            p.style = style, p.seed = seed, p.lane = lane_of(style), p.lead = style == GEN_MEL_ANSWER ? lead : NULL;
            gen(&p, st);
            mstat_loop(&m, &p, st);
        }
    return m;
}

/* Hook at c 50: true steps >= 35%, unisons <= 25%, runs of 4+ equal notes in <= 5% of loops, 80% of leaps of 5+
 * reverse, the loop wraps by a 7th or more in <= 10% of loops; at c 30 runs of 4+ in <= 10% */
static void mel_test_steps(void) {
    mstat_t m = mel_stats(GEN_MEL_HOOK, 50), q = mel_stats(GEN_MEL_HOOK, 30);
    CHECK(m.steps * 100 >= m.ivs * 35 && m.unis * 100 <= m.ivs * 25 && m.run4 * 100 <= m.loops * 5,
          "MEL_HOOK c50: %ld%% steps (>= 35%%), %ld%% unisons (<= 25%%), runs of 4+ in %ld%% of loops (<= 5%%)",
          100 * m.steps / m.ivs, 100 * m.unis / m.ivs, 100 * m.run4 / m.loops);
    CHECK(m.back * 5 >= m.leaps * 4 && m.wrap7 * 10 <= m.loops, "MEL_HOOK c50: %ld of %ld leaps of 5+ reverse (>= 80%%); "
          "the loop wraps by 7+ semitones in %ld%% (<= 10%%)", m.back, m.leaps, 100 * m.wrap7 / m.loops);
    CHECK(q.run4 * 10 <= q.loops, "MEL_HOOK c30: runs of 4+ equal notes in %ld%% of loops (<= 10%%); %ld%% steps, %ld%% "
          "unisons", 100 * q.run4 / q.loops, 100 * q.steps / q.ivs, 100 * q.unis / q.ivs);
    mstat_t w = mel_stats(GEN_MEL_ANSWER, 50);
    CHECK(w.wrap7 * 100 <= w.loops * 12 && w.contra * 100 >= w.moves * 65, "MEL_ANSWER c50 under a c50 Hook: the loop wraps by "
          "7+ semitones in %ld%% (<= 12%%); contrary motion in %ld%% of %ld moves where both move (>= 65%%)",
          100 * w.wrap7 / w.loops, 100 * w.contra / w.moves, w.moves);
}

/* the Answer converses: below c 50 it sounds in every bar the lead leaves 4+ free steps in (c 0), never rings over a
 * lead onset;
 * from c 50 it rarely doubles the lead's pitch class where both sound */
static void mel_test_answer(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    int empty = 0, ring = 0;
    long shared = 0, dbl = 0;
    for (int k = 0; k < MCASE; k++) {
        gen_params_t p = mel_case(3, k);
        gen(&p, st);
        for (int i = 0; p.complexity < 50 && i < loop_len(&p); i++)
            for (int j = i + 1; st[i].on && j < i + st[i].len; j++) ring += p.lead[j].on;
        for (int b = 0; p.complexity == 0 && p.chord_len <= 16 && b < p.bars; b++) {
            int run = 0, room = 0, on = 0;
            for (int i = b * 16; i < b * 16 + 16; i++) run = lead_sounds(&p, i) ? 0 : run + 1, room |= run >= 4, on += st[i].on;
            empty += room && !on;
        }
        for (int i = 0; p.complexity == 50 && i < loop_len(&p); i++)
            if (st[i].on && p.lead[i].on) shared++, dbl += pmod(pitch_at(&p, st, i) - pitch_at(&p, p.lead, i), 12) == 0;
    }
    CHECK(!empty && !ring && dbl * 100 < shared * 15, "MEL_ANSWER: %d silent bars with room at c 0, %d notes ringing over a "
          "lead onset below c 50; at c 50 %ld%% of %ld shared onsets double the lead (< 15%%)", empty, ring,
          shared ? 100 * dbl / shared : 0, shared);
}

/* The Answer is never silent for a whole phrase below c 50: under an Arp lead and under v1's Manual every-other-step
 * lead (no gap of 2 steps anywhere), it falls back to one chord tone on each phrase's chord change; the rules hold */
static void mel_test_fallback(void) {
    static seq_step_t lead[SEQ_LANE_STEPS], st[SEQ_LANE_STEPS];
    int silent = 0, errs = 0, cases = 0;
    for (int k = 0; k < 2 * 4 * 5 * 20; k++) {
        int arp = k % 2, cl = CLS[k / 2 % 4], c = k / 8 % 5 * 10, seed = 1 + k / 40;
        gen_params_t p = {GEN_MEL_ANSWER, seed, c, 4, cl, PROG, 4, seed % 12, seed % SCALE_COUNT, lead, LANE_MEL_DOWN, 0, NULL}, q = p;
        q.style = GEN_MEL_ARP, q.seed = seed * 7, q.lane = LANE_MEL_UP, q.lead = NULL;
        if (arp) gen(&q, lead);
        else
            for (int i = 0; i < SEQ_LANE_STEPS; i++) lead[i] = (seq_step_t){i % 2 == 0, TONE_R, 1, 100, 1, 0};
        gen(&p, st);
        int pl = cl < 16 ? 16 : cl;
        for (int ps = 0; ps < 64; ps += pl) {
            int on = 0;
            for (int i = ps; i < ps + pl; i++) on += st[i].on;
            silent += !on;
        }
        errs += mel_rule_errors(&p, st), cases++;
    }
    CHECK(!silent && !errs, "MEL_ANSWER c 0..40 under an Arp and under v1's every-other-step Manual lead: %d silent phrases, "
          "%d rule errors (%d cases)", silent, errs, cases);
}

/* The default pair at c 30 (Hook c 30 leading the Answer), 16 presets x 40 seeds x chord_len 4, 8, 16: Answer notes of
 * one step <= 40%, bars of only such blips <= 25%, and an Answer note in every bar where the lead leaves 3+ free steps
 * in a row (the brief asks 4+; 3 also covers the 3-step breath under 1/4-bar chords) */
static void mel_test_breath(void) {
    static seq_step_t h[SEQ_LANE_STEPS], st[SEQ_LANE_STEPS];
    long notes = 0, ones = 0, bars = 0, blips = 0, gapbars = 0, mute = 0;
    for (int k = 0; k < THEORY_NPRESETS * 40 * 3; k++) {
        const theory_preset_t *r = &THEORY_PRESETS[k % THEORY_NPRESETS];
        int seed = k / THEORY_NPRESETS % 40, cl = 4 << k / (THEORY_NPRESETS * 40);
        gen_params_t lp = {GEN_MEL_HOOK, seed + 1, 30, 4, cl, r->deg, r->len, (seed * 5 + k) % 12, seed % SCALE_COUNT, NULL,
                           LANE_MEL_UP, 0, NULL}, p = lp;
        gen(&lp, h);
        p.style = GEN_MEL_ANSWER, p.seed = seed, p.lane = LANE_MEL_DOWN, p.lead = h;
        gen(&p, st);
        for (int b = 0; b < 4; b++) {
            int on = 0, one = 0, run = 0, gap = 0;
            for (int i = b * 16; i < b * 16 + 16; i++) {
                on += st[i].on, one += st[i].on && st[i].len == 1;
                run = lead_sounds(&p, i) ? 0 : run + 1;
                gap |= run >= 3;   /* a call's breath under 1/4-bar chords is 3 steps */
            }
            notes += on, ones += one, bars++, blips += on && one == on, gapbars += gap, mute += gap && !on;
        }
    }
    CHECK(ones * 100 <= notes * 40 && blips * 100 <= bars * 25 && !mute, "MEL_ANSWER c30 under a c30 Hook: %ld%% of notes "
          "one step (<= 40%%), %ld%% of bars only blips (<= 25%%), %ld of %ld bars with a 3-step gap silent",
          100 * ones / notes, 100 * blips / bars, mute, gapbars);
}

/* The conversation at the sweet spot, with whole-bar chords (the default): a c 50 Hook leading a c 30 Answer (>= 1.4
 * Answer notes per bar), and a c 50 Answer (<= 10% of bars with 0-1 notes, <= 45% of its onsets on Hook onsets);
 * 16 presets x 40 seeds. Shorter chords add chord changes, where from c 50 both lines must sound (the report has the
 * numbers per chord length). */
static void mel_pair(int hc, int ac, long *notes, long *thin, long *bars, long *on_lead) {
    static seq_step_t h[SEQ_LANE_STEPS], st[SEQ_LANE_STEPS];
    for (int k = 0; k < THEORY_NPRESETS * 40; k++) {
        const theory_preset_t *r = &THEORY_PRESETS[k % THEORY_NPRESETS];
        int seed = k / THEORY_NPRESETS % 40, cl = 16;
        gen_params_t lp = {GEN_MEL_HOOK, seed + 1, hc, 4, cl, r->deg, r->len, (seed * 5 + k) % 12, seed % SCALE_COUNT, NULL,
                           LANE_MEL_UP, 0, NULL}, p = lp;
        gen(&lp, h);
        p.style = GEN_MEL_ANSWER, p.seed = seed, p.complexity = ac, p.lane = LANE_MEL_DOWN, p.lead = h;
        gen(&p, st);
        for (int b = 0; b < 4; b++) {
            int on = 0;
            for (int i = b * 16; i < b * 16 + 16; i++) on += st[i].on, *on_lead += st[i].on && h[i].on;
            *notes += on, *thin += on <= 1, (*bars)++;
        }
    }
}

static void mel_test_sweet(void) {
    long n30 = 0, t30 = 0, b30 = 0, l30 = 0, n50 = 0, t50 = 0, b50 = 0, l50 = 0;
    mel_pair(50, 30, &n30, &t30, &b30, &l30);
    mel_pair(50, 50, &n50, &t50, &b50, &l50);
    CHECK(n30 * 10 >= b30 * 14 && t50 * 10 <= b50 && l50 * 100 <= n50 * 45, "Hook c50 leading: Answer c30 %.2f notes per bar "
          "(>= 1.4); Answer c50 %ld%% of bars with 0-1 notes (<= 10%%), %ld%% of its onsets on Hook onsets (<= 45%%)",
          (double)n30 / b30, 100 * t50 / b50, 100 * l50 / n50);
}

/* Answer GEN varies the rhythm under a fixed Hook: >= 5 distinct Answer rhythms over 100 seeds at c 30 and c 50 */
static void mel_test_answer_seeds(void) {
    static seq_step_t h[SEQ_LANE_STEPS], st[SEQ_LANE_STEPS];
    static unsigned char seen[2][65536 / 8];
    int distinct[2] = {0, 0};
    gen_params_t lp = mk(GEN_MEL_HOOK, 11, 30, 4, 16);
    gen(&lp, h);
    for (int ci = 0; ci < 2; ci++)
        for (int seed = 0; seed < 100; seed++) {
            gen_params_t p = mk(GEN_MEL_ANSWER, seed, 30 + 20 * ci, 4, 16);
            unsigned m = 2166136261u;
            p.lead = h;
            gen(&p, st);
            for (int i = 0; i < 64; i++) m = (m ^ st[i].on) * 16777619u;
            distinct[ci] += seen_once(seen[ci], (m ^ m >> 16) & 0xFFFFu);
        }
    CHECK(distinct[0] >= 5 && distinct[1] >= 5, "MEL_ANSWER under a fixed c30 Hook: %d distinct rhythms over 100 seeds at "
          "c 30, %d at c 50 (>= 5)", distinct[0], distinct[1]);
}

/* a Hook's or Answer's non-chord tone is never left hanging: its next note starts within 2 steps of its end (and an
 * Answer has none below c 20) */
static void mel_test_answer_nct(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    int hanging = 0, ncts = 0;
    for (int v = 0; v <= 4; v += v ? 1 : 3)
        for (int k = 0; k < MCASE; k++) {
            gen_params_t p = mel_case(v, k);
            gen(&p, st);
            for (int i = 0; i < loop_len(&p); i++) {
                if (!st[i].on || is_ct(st[i].tone)) continue;
                int j = i + 1;
                while (j < loop_len(&p) && !st[j].on) j++;
                ncts++, hanging += j - (i + st[i].len) > 2 || (p.style == GEN_MEL_ANSWER && p.complexity < 20);
            }
        }
    CHECK(!hanging, "MEL_HOOK and MEL_ANSWER: %d of %d non-chord tones hang (next note within 2 steps; no Answer NCT "
          "below c 20)", hanging, ncts);
}

/* Arp: on 1/4-bar chords at c 0 its bar lines vary by seed; consecutive notes in a chord never repeat */
static void mel_test_arp(void) {
    static unsigned char seen[65536 / 8];
    static seq_step_t st[SEQ_LANE_STEPS];
    int distinct = 0, repeats = 0;
    for (int seed = 0; seed < 100; seed++) {
        gen_params_t p = mk(GEN_MEL_ARP, seed, 0, 4, 4);
        unsigned h = 2166136261u;
        gen(&p, st);
        for (int i = 0; i < 16; i++) h = (h ^ (unsigned)(st[i].on ? pitch_at(&p, st, i) : 0)) * 16777619u;
        distinct += seen_once(seen, (h ^ h >> 16) & 0xFFFFu);
    }
    for (int k = 0; k < MCASE; k++)
        for (int v = 1; v <= 2; v++) {
            gen_params_t p = mel_case(v, k);
            gen(&p, st);
            for (int i = 0, prev = -1; i < loop_len(&p); i++) {
                if (!st[i].on) continue;
                repeats += prev >= 0 && prev / p.chord_len == i / p.chord_len && pitch_at(&p, st, prev) == pitch_at(&p, st, i);
                prev = i;
            }
        }
    CHECK(distinct >= 10 && !repeats, "MEL_ARP: %d distinct bar lines over 100 seeds at chord_len 4, c 0 (>= 10); %d "
          "repeated notes within a chord", distinct, repeats);
}

/* The Hook's call contour is audible: mean pitch per quarter of a call's sounding part (12 steps; its last beat is the
 * breath below c 75) at c 50 (the contour per seed, as gen.c hashes it) */
static unsigned t_fmix(unsigned h) {
    h ^= h >> 16, h *= 0x85EBCA6Bu, h ^= h >> 13, h *= 0xC2B2AE35u;
    return h ^ (h >> 16);
}

static void mel_test_contour(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    double sum[4][4] = {{0}};
    long n[4][4] = {{0}};
    for (int pr = 0; pr < THEORY_NPRESETS; pr++)
        for (int seed = 0; seed < 100; seed++) {
            int ct = (int)(t_fmix((unsigned)seed * 0x9E3779B1u + 13u * 0xC2B2AE35u) % 4u);   /* SALT_CONTOUR 13, step 0 */
            gen_params_t p = {GEN_MEL_HOOK, seed, 50, 8, 16, THEORY_PRESETS[pr].deg, THEORY_PRESETS[pr].len, (seed + pr) % 12,
                              seed % SCALE_COUNT, NULL, LANE_MEL_UP, 0, NULL};
            gen(&p, st);
            for (int i = 0; i < 128; i++)
                if (st[i].on && i / 16 % 2 == 0 && i % 16 < 12)   /* call bars: quarters of the 12 steps before the breath */
                    sum[ct][i % 16 / 3] += pitch_at(&p, st, i) - mel_centre(&p), n[ct][i % 16 / 3]++;
        }
    double q[4][4];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++) q[c][k] = sum[c][k] / (n[c][k] ? n[c][k] : 1);
    int arch = (q[0][1] + q[0][2]) - (q[0][0] + q[0][3]) >= 4, rise = q[1][3] - q[1][0] >= 2, fall = q[2][0] - q[2][3] >= 2;
    CHECK(arch && rise && fall, "MEL_HOOK c50 contours by quarter: arch %+.1f %+.1f %+.1f %+.1f, rise %+.1f .. %+.1f, "
          "fall %+.1f .. %+.1f (2+ semitones apart)", q[0][0], q[0][1], q[0][2], q[0][3], q[1][0], q[1][3], q[2][0], q[2][3]);
}

/* seeds give many Hook bar-1 lines; at c 0 the Hook's rhythm repeats bar to bar */
static void mel_test_variety(void) {
    static unsigned char seen[65536 / 8];
    static seq_step_t st[SEQ_LANE_STEPS];
    int distinct = 0, loose = 0;
    for (int seed = 0; seed < 1000; seed++) {
        gen_params_t p = mk(GEN_MEL_HOOK, seed, 50, 4, 16);
        unsigned h = 2166136261u;
        gen(&p, st);
        for (int i = 0; i < 16; i++) h = (h ^ (unsigned)(st[i].on ? pitch_at(&p, st, i) : 0)) * 16777619u;
        distinct += seen_once(seen, (h ^ h >> 16) & 0xFFFFu);
    }
    for (int seed = 0; seed < 200; seed++) {
        gen_params_t p = mk(GEN_MEL_HOOK, seed, 0, 8, 16);
        gen(&p, st);
        for (int g = 0; g < 2; g++) loose += most_alike(st, g) < 3;
    }
    static unsigned char rhythms[65536 / 8];
    int nr = 0;
    for (int seed = 0; seed < 100; seed++) {
        gen_params_t p = mk(GEN_MEL_HOOK, seed, 0, 1, 16);
        gen(&p, st);
        nr += seen_once(rhythms, bar_rhythm(st, 0));
    }
    CHECK(distinct >= 50 && !loose && nr >= 3, "MEL_HOOK: %d distinct bar-1 lines over 1000 seeds at c 50 (>= 50); "
          "%d 4-bar groups at c 0 with under 3 alike rhythms; %d c 0 rhythms over 100 seeds (>= 3)", distinct, loose, nr);
}

/* gen_span_v1 for melodies (lead included) equals the loop's span */
static void mel_test_span(void) {
    int bad = 0, outside = 0;
    for (int v = 0; v < NVOICE; v++)
        for (int k = 0; k < MCASE; k += 7) {
            gen_params_t p = mel_case(v, k);
            span_case(&p, &bad, &outside);
        }
    CHECK(!bad && !outside, "melodies: one span alone is the loop's span (%d differ), the rest untouched (%d)", bad, outside);
}

static void mel_test_timing(void) {
    static seq_step_t st[SEQ_LANES][SEQ_LANE_STEPS];
    clock_t t0 = clock();
    for (int loop = 0; loop < 100; loop++) {
        gen_params_t p[SEQ_LANES] = {mk(GEN_CH_STABS, loop, loop % 101, 16, 4 << loop % 4),
                                     mk(GEN_BASS_WALK, loop, loop % 101, 16, 4 << loop % 4),
                                     mk(GEN_MEL_HOOK, loop, loop % 101, 16, 4 << loop % 4),
                                     mk(GEN_MEL_ANSWER, loop, loop % 101, 16, 4 << loop % 4)};
        p[3].lead = st[2];
        for (int l = 0; l < SEQ_LANES; l++) gen_lane_v1(&p[l], st[l]);
    }
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 100;
    CHECK(ms < 1.0, "a 4-lane 16-bar regen with Hook and Answer takes %.4f ms on average (budget 1 ms)", ms);
}

static unsigned fnv(unsigned h, unsigned char b) { return (h ^ b) * 16777619u; }

/* Pin gen_progression_v1: seeds 1..20 x c {0, 50, 100} x len 2..8. Not save-critical (progressions are saved as
 * degrees): an intended change just re-records it. */
#define GEN_V1_GOLDEN_PROG 0x8e3dcb6eu

static unsigned golden_prog(void) {
    unsigned h = 2166136261u;
    int out[9];
    for (int seed = 1; seed <= 20; seed++)
        for (int c = 0; c <= 100; c += 50)
            for (int len = 2; len <= 8; len++) {
                prog(seed, c, len, out);
                for (int i = 0; i < len; i++) h = fnv(h, (unsigned char)out[i]);
            }
    return h;
}

static unsigned hash_lane(unsigned h, const gen_params_t *p) {
    static seq_step_t st[SEQ_LANE_STEPS];
    gen(p, st);
    for (int i = 0; i < loop_len(p); i++) {
        const seq_step_t *s = &st[i];
        unsigned char b[5] = {s->on, s->tone, (unsigned char)s->oct, s->vel, s->len};
        for (int j = 0; j < 5; j++) h = fnv(h, b[j]);
    }
    return h;
}

/* Pin gen_lane_v1's output. Changing it changes the notes of saved generated lanes: follow the version policy in gen.h
 * (new style ids, or a per-lane version field; never bump `gv`). Base: 4 bars of whole-bar chords. Layouts: chord lengths, loop lengths and
 * progression lengths. Keys: keys and scales (the walk's octaves follow pitch). */
#define GEN_V1_GOLDEN_BASE 0xb5b9d45du
#define GEN_V1_GOLDEN_LAYOUT 0x5a23d20eu
#define GEN_V1_GOLDEN_KEYS 0x132b195du

static unsigned golden_base(void) {
    unsigned h = 2166136261u;
    for (int style = 1; style <= GEN_BASS_WALK; style++)
        for (int seed = 1; seed <= 3; seed++)
            for (int c = 0; c <= 100; c += 50) {
                gen_params_t p = mk(style, seed, c, 4, 16);
                h = hash_lane(h, &p);
            }
    return h;
}

static unsigned golden_layout(void) {
    const int cls[3] = {4, 8, 32}, bars[2] = {1, 16}, seeds[2] = {0, 65535};
    unsigned h = 2166136261u;
    for (int k = 0; k < 6 * 3 * 2 * 2 * 2 * 3; k++) {
        int style = 1 + k % 6, pl = k / 36 % 2 ? 8 : 3;
        gen_params_t p = {style, seeds[k / 72 % 2], k / 144 * 50, bars[k / 18 % 2], cls[k / 6 % 3], pl == 8 ? P8 : P3, pl,
                          4, SCALE_MINOR, NULL, lane_of(style), 0, NULL};
        h = hash_lane(h, &p);
    }
    return h;
}

static unsigned golden_keys(void) {
    const int keys[3] = {0, 4, 11};
    unsigned h = 2166136261u;
    for (int style = 1; style <= GEN_BASS_WALK; style++)
        for (int k = 0; k < 3 * SCALE_V1_COUNT; k++)
            for (int c = 0; c <= 100; c += 50) {
                gen_params_t p = {style, 7, c, 4, 8, PROG, 4, keys[k % 3], k / 3, NULL, lane_of(style), 0, NULL};
                h = hash_lane(h, &p);
            }
    return h;
}

/* Pin the melody styles. Voices: Hook, Arp on Melody 1, Arp on Melody 2, Answer with a Hook lead, Answer alone.
 * Layouts: seeds 1..3 x c {0, 50, 100} x {4 bars of whole-bar chords, 2 bars of 1/4, 4 bars of 1/2, 8 bars of 2-bar}
 * in E minor; then keys {0, 4, 11} x the four scales x c {0, 50, 100} at seed 7, 4 bars of 1/2-bar chords. Same version
 * policy as the other goldens. */
#define GEN_V1_GOLDEN_MEL 0xe3105b14u

static unsigned golden_voice(unsigned h, int v, gen_params_t p) {
    static seq_step_t lead[SEQ_LANE_STEPS];
    gen_params_t q = p;
    q.style = GEN_MEL_HOOK, q.seed = p.seed + 10, q.lane = LANE_MEL_UP;
    gen(&q, lead);
    p.style = VSTYLE[v], p.lane = VLANE[v], p.lead = VLEAD[v] ? lead : NULL;
    return hash_lane(h, &p);
}

static unsigned golden_mel(void) {
    const int bars[4] = {4, 2, 4, 8}, cls[4] = {16, 4, 8, 32}, keys[3] = {0, 4, 11};
    unsigned h = 2166136261u;
    for (int k = 0; k < 5 * 3 * 3 * 4; k++)
        h = golden_voice(h, k % 5, mk(GEN_MEL_HOOK, 1 + k / 5 % 3, k / 15 % 3 * 50, bars[k / 45], cls[k / 45]));
    for (int k = 0; k < 5 * 3 * SCALE_V1_COUNT * 3; k++) {
        gen_params_t p = mk(GEN_MEL_HOOK, 7, k / 60 * 50, 4, 8);
        p.key = keys[k / 5 % 3], p.scale = k / 15 % SCALE_V1_COUNT;
        h = golden_voice(h, k % 5, p);
    }
    return h;
}

/* Pins gen_lane_v1 and gen_span_v1 over random params well past the other goldens (odd lengths and chord lengths, keys
 * and complexities out of range, Answers over leads with stray notes, one span). It also freezes inputs the sequencer
 * never produces (chord lengths 0, 1 or 300, out-of-range keys and bars); that is acceptable: it costs a re-record, never
 * a song. Recorded before 0.6's speed work (so the faster code is the same generator); re-recorded for the Answer's
 * fallback note. */
#define GEN_V1_GOLDEN_FUZZ 0x80ab60bau

static unsigned fz_r;
static unsigned fz(unsigned m) { fz_r = fz_r * 1664525u + 1013904223u; return (fz_r >> 8) % m; }

static unsigned fz_hash(unsigned h, const seq_step_t *st) {
    for (int i = 0; i < SEQ_LANE_STEPS; i++) {
        unsigned char b[5] = {st[i].on, st[i].tone, (unsigned char)st[i].oct, st[i].vel, st[i].len};
        for (int j = 0; j < 5; j++) h = (h ^ b[j]) * 16777619u;
    }
    return h;
}

/* One random case: any style (Answers over a generated lead with stray notes), any length, chord length, key and scale,
 * in and out of range; the whole lane, then one span of it */
static unsigned fz_case(unsigned h, int *prog, seq_step_t *lead, seq_step_t *st) {
    static const int cls[7] = {0, 1, 4, 8, 16, 32, 300};
    int pl = 1 + (int)fz(16);
    for (int i = 0; i < pl; i++) prog[i] = (int)fz(7);
    gen_params_t p = {1 + (int)fz(9), (int)fz(65536), (int)fz(101), 1 << fz(5), cls[fz(7)], fz(8) ? prog : NULL, pl,
                      (int)fz(12), (int)fz(4), NULL, (int)fz(4), 0, NULL};
    if (fz(25) == 0) p.complexity = (int)fz(140) - 20, p.bars = (int)fz(20), p.chord_len = (int)fz(70), p.key = (int)fz(40) - 20;
    if (p.style == GEN_MEL_ANSWER && fz(4)) {
        gen_params_t q = p;
        q.style = fz(3) ? GEN_MEL_HOOK : GEN_MEL_ARP, q.seed = (int)fz(65536), q.lane = LANE_MEL_UP;
        memset(lead, 0, sizeof(seq_step_t) * SEQ_LANE_STEPS);
        gen_lane_v1(&q, lead);
        for (int i = (int)fz(8); i > 0; i--) lead[fz(SEQ_LANE_STEPS)] = (seq_step_t){1, (unsigned char)(1 + fz(7)), 0, 100, (unsigned char)(1 + fz(16)), 0};
        p.lead = lead;
    }
    memset(st, 0xAB, sizeof(seq_step_t) * SEQ_LANE_STEPS);
    gen_lane_v1(&p, st);
    h = fz_hash(h, st);
    memset(st, 0xCD, sizeof(seq_step_t) * SEQ_LANE_STEPS);
    gen_span_v1(&p, (int)fz(300) - 20, st);
    return fz_hash(h, st);
}

static unsigned golden_fuzz(void) {
    static seq_step_t lead[SEQ_LANE_STEPS], st[SEQ_LANE_STEPS];
    int prog[16];
    unsigned h = 2166136261u;
    fz_r = 1;
    for (int k = 0; k < 4000; k++) h = fz_case(h, prog, lead, st);
    return h;
}

/* One random change to a cached lane's inputs, in place as a sequencer makes them: mostly key, scale and progression
 * (the cache keeps the rhythm), else style, seed, complexity, length, chord length, lane or the lead's notes. */
static void cache_walk_step(gen_params_t *p, int *prog, seq_step_t *lead, unsigned k) {
    unsigned r = (unsigned)fz(16);
    if (r < 3) p->key = (int)fz(12);
    else if (r < 5) p->scale = (int)fz(SCALE_V1_COUNT);
    else if (r < 7) prog[fz(4)] = (int)fz(7);
    else if (r == 7) p->style = GEN_MEL_HOOK + (int)fz(3);
    else if (r == 8) p->seed = (int)fz(65536);
    else if (r == 9) p->complexity = (int)fz(101);
    else if (r == 10) p->bars = 1 << fz(5);
    else if (r == 11) p->chord_len = 4 << fz(4);
    else if (r == 12) p->lane = LANE_MEL_UP + (int)fz(2);
    else if (r == 13) p->lead = p->lead ? NULL : lead;
    else if (r == 14) lead[fz(SEQ_LANE_STEPS)].on ^= 1;   /* the lead's rhythm moves */
    else lead[fz(SEQ_LANE_STEPS)].tone = (unsigned char)(1 + k % 7);   /* only its pitch does */
}

/* The rhythm cache never changes a step: a random walk of in-place changes, each lane and one span of it generated with
 * the cache and without */
static void test_cache(void) {
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS], lead[SEQ_LANE_STEPS];
    static gen_cache_t c;
    int prog[4] = {0, 5, 2, 6}, bad = 0;
    gen_params_t p = {GEN_MEL_HOOK, 1, 30, 4, 16, prog, 4, 4, SCALE_MINOR, NULL, LANE_MEL_UP, 0, NULL}, q = p;
    q.style = GEN_MEL_HOOK, q.seed = 9;
    gen(&q, lead);
    fz_r = 77;
    for (unsigned k = 0; k < 3000; k++) {
        cache_walk_step(&p, prog, lead, k);
        memset(a, 0xAB, sizeof a), memset(b, 0xAB, sizeof b);
        gen_lane_cached_v1(&p, &c, a);
        gen_lane_v1(&p, b);
        bad += memcmp(a, b, sizeof a) != 0;
        int step = (int)fz(SEQ_LANE_STEPS);
        memset(a, 0xCD, sizeof a), memset(b, 0xCD, sizeof b);
        gen_span_cached_v1(&p, &c, step, a);
        gen_span_v1(&p, step, b);
        bad += memcmp(a, b, sizeof a) != 0;
    }
    CHECK(!bad, "the rhythm cache gives the uncached steps over 3000 random in-place changes (%d differ)", bad);
}

/* A key change moves a melody's notes with the centre (gen_transpose_v1): the same steps as generating in the new key */
static void test_transpose(void) {
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS], l1[SEQ_LANE_STEPS], l2[SEQ_LANE_STEPS];
    int bad = 0, untouched = 1;
    for (int k = 0; k < 3000; k++) {
        int style = GEN_MEL_HOOK + k % 3, k1 = k % 12, k2 = (k * 7 + 5) % 12;
        gen_params_t p = {style, k * 37, k % 101, 1 << (k / 3 % 5), 4 << (k / 15 % 4), k % 2 ? P3 : P8, k % 2 ? 3 : 8, k1,
                          k / 60 % SCALE_COUNT, NULL, style == GEN_MEL_ANSWER ? LANE_MEL_DOWN : LANE_MEL_UP + k % 2, 0, NULL};
        gen_params_t q = p, p2 = p;
        q.style = GEN_MEL_HOOK, q.lane = LANE_MEL_UP, q.seed = k;
        gen(&q, l1);
        q.key = k2;
        gen(&q, l2);
        if (style == GEN_MEL_ANSWER) p.lead = l1, p2.lead = l2;
        p2.key = k2;
        gen(&p, a);
        gen_transpose_v1(&p2, k1, a);
        gen(&p2, b);
        bad += memcmp(a, b, sizeof a) != 0;
    }
    for (int st = GEN_MANUAL; st < GEN_MEL_HOOK; st++) {
        gen_params_t p = mk(st, 1, 50, 4, 16);
        gen(&p, a);
        memcpy(b, a, sizeof b);
        gen_transpose_v1(&p, 9, a);
        untouched &= !gen_transposes(st) && !memcmp(a, b, sizeof a);
    }
    CHECK(!bad && untouched, "melodies transpose to what the new key generates (%d of 3000 differ); other styles don't", bad);
}

static void test_golden(void) {
    unsigned b = golden_base(), l = golden_layout(), k = golden_keys();
    CHECK(b == GEN_V1_GOLDEN_BASE, "gen_lane_v1 golden checksum, base 0x%08x", b);
    CHECK(l == GEN_V1_GOLDEN_LAYOUT, "gen_lane_v1 golden checksum, layouts 0x%08x", l);
    CHECK(k == GEN_V1_GOLDEN_KEYS, "gen_lane_v1 golden checksum, keys 0x%08x", k);
    unsigned m = golden_mel();
    CHECK(m == GEN_V1_GOLDEN_MEL, "gen_lane_v1 golden checksum, melodies 0x%08x", m);
    unsigned g = golden_prog();
    CHECK(g == GEN_V1_GOLDEN_PROG, "gen_progression_v1 golden checksum 0x%08x", g);
    unsigned f = golden_fuzz();
    CHECK(f == GEN_V1_GOLDEN_FUZZ, "gen_lane_v1 and gen_span_v1 golden checksum, random params 0x%08x", f);
}

/* ---- 0.12 chord rhythms (gen_chords_v2) ---- */

/* the on-steps of bar `bar` as a 16-bit mask */
static unsigned bar_mask(const seq_step_t *st, int bar) {
    unsigned m = 0;
    for (int j = 0; j < SEQ_STEPS; j++) m |= (unsigned)st[bar * SEQ_STEPS + j].on << j;
    return m;
}

static unsigned bits(const int *pos, int n) {
    unsigned m = 0;
    for (int i = 0; i < n; i++) m |= 1u << pos[i];
    return m;
}

/* the first seed >= 1 that picks variant v of a 0.12 chord rhythm */
static int variant_seed(int style, int v) {
    int seed = 1;
    while (gen_chords_v2_variant(style, seed) != v) seed++;
    return seed;
}

/* c = 0, 4 bars, variant 0: every bar plays exactly 0.12's template, with its lengths */
static void ch2_test_templates(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    static const int OFF[4] = {2, 6, 10, 14}, CHA[2] = {0, 6}, BAL[1] = {0}, BAL40[2] = {0, 8}, FUN[6] = {0, 3, 6, 10, 12, 14},
                     HAL[2] = {0, 8}, PUS[2] = {0, 14}, PUS50[3] = {0, 6, 14};
    struct { int style, c, cl; unsigned want; const char *what; } T[] = {
        {GEN_CH_OFFBEAT, 0, 16, bits(OFF, 4), "Offbeat c0: 2 6 10 14"},
        {GEN_CH_OFFBEAT, 0, 8, bits(OFF, 4), "Offbeat c0, 1/2-bar chords: 2 6 10 14"},
        {GEN_CH_OFFBEAT, 0, 4, bits(OFF, 4), "Offbeat c0, 1/4-bar chords: 2 6 10 14"},
        {GEN_CH_OFFBEAT, 0, 32, bits(OFF, 4), "Offbeat c0, 2-bar chords: 2 6 10 14 in both bars"},
        {GEN_CH_CHARLESTON, 0, 16, bits(CHA, 2), "Charleston c0: 0 6"},
        {GEN_CH_CHARLESTON, 0, 8, 0x4141u, "Charleston c0, 1/2-bar chords: 0 6 at each chord"},
        {GEN_CH_BALLAD, 0, 16, bits(BAL, 1), "Ballad c0: 0"},
        {GEN_CH_FUNK, 0, 16, bits(FUN, 6), "Funk c0: 0 3 6 10 12 14"},
        {GEN_CH_HALFTIME, 0, 16, bits(HAL, 2), "Half-time c0: 0 8"},
        {GEN_CH_HALFTIME, 0, 4, 0x1111u, "Half-time c0, 1/4-bar chords: each chord's start"},
        {GEN_CH_PUSH, 0, 16, bits(PUS, 2), "Push c0: 0 14"},
        {GEN_CH_PUSH, 0, 32, bits(PUS, 2), "Push c0, 2-bar chords: 0 14 in both bars"},
    };
    for (size_t t = 0; t < sizeof T / sizeof T[0]; t++) {
        int ok = 1;
        for (int seed = 0, n = 0; n < 40; seed++) {   /* 0.13: the seeds that pick variant 0 (0.12's template) */
            if (gen_chords_v2_variant(T[t].style, seed * 1637)) continue;
            gen_params_t p = mk(T[t].style, seed * 1637, T[t].c, 4, T[t].cl);
            gen(&p, st);
            for (int b = 0; b < 4; b++) ok &= bar_mask(st, b) == T[t].want;
            n++;
        }
        CHECK(ok, "%s (40 variant-0 seeds, every bar)", T[t].what);
    }
    /* the template steps above c 0 are still there; Ballad's 8 from c 40, Push's 6 from c 50 */
    int ok40 = 1, ok50 = 1, tmpl = 1;
    for (int seed = 0; seed < 40; seed++) {
        gen_params_t p = mk(GEN_CH_BALLAD, seed, 40, 4, 16), q = mk(GEN_CH_PUSH, seed, 49, 4, 16), r = mk(GEN_CH_PUSH, seed, 50, 4, 16);
        if (!gen_chords_v2_variant(GEN_CH_BALLAD, seed)) {   /* 0.13: variant 0, 0.12's template */
            gen(&p, st);
            ok40 &= (bar_mask(st, 0) & bits(BAL40, 2)) == bits(BAL40, 2);
            p.complexity = 39;
            gen(&p, st);
            ok40 &= st[0].on && !st[8].on;
        }
        if (!gen_chords_v2_variant(GEN_CH_PUSH, seed)) {
            gen(&q, st);
            ok50 &= !st[6].on;
            gen(&r, st);
            ok50 &= (bar_mask(st, 0) & bits(PUS50, 3)) == bits(PUS50, 3);
        }
        for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++) {
            gen_params_t lo = mk(style, seed, 0, 2, 16), hi = mk(style, seed, 100, 2, 16);
            gen(&lo, st);
            unsigned m0 = bar_mask(st, 0);
            gen(&hi, st);
            tmpl &= (bar_mask(st, 0) & m0) == m0;
        }
    }
    CHECK(ok40, "Ballad adds beat 3 (step 8) from c 40");
    CHECK(ok50, "Push adds the & of 2 (step 6) from c 50, not at 49");
    CHECK(tmpl, "every style: c 100 keeps the template's hits");
}

/* 0.13: three templates per style, picked by the seed. c 0, 1-bar chords, 4 bars: every bar plays exactly its variant's
 * hits with their lengths (cut at the next hit and the bar line); the variants differ; each is picked by about a
 * third of seeds; a variant with no hit in a 1/4-bar unit plays variant 0's */
static void ch2_test_variants(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    typedef struct { signed char pos, len; } pl_t;
    static const pl_t V[GEN_CH2_LAST - GEN_CH2_FIRST + 1][3][7] = {
        {{{2, 2}, {6, 2}, {10, 2}, {14, 2}, {-1, 0}}, {{2, 2}, {6, 2}, {10, 2}, {14, 1}, {15, 1}, {-1, 0}}, {{6, 4}, {14, 2}, {-1, 0}}},
        {{{0, 6}, {6, 2}, {-1, 0}}, {{0, 6}, {6, 2}, {12, 2}, {-1, 0}}, {{0, 3}, {3, 3}, {6, 2}, {-1, 0}}},
        {{{0, 16}, {-1, 0}}, {{0, 12}, {12, 4}, {-1, 0}}, {{0, 10}, {10, 6}, {-1, 0}}},
        {{{0, 1}, {3, 1}, {6, 1}, {10, 1}, {12, 1}, {14, 1}, {-1, 0}}, {{0, 1}, {3, 1}, {7, 1}, {10, 1}, {13, 1}, {-1, 0}},
         {{0, 1}, {2, 1}, {6, 1}, {9, 1}, {12, 1}, {14, 1}, {-1, 0}}},
        {{{0, 8}, {8, 8}, {-1, 0}}, {{0, 8}, {8, 6}, {14, 1}, {-1, 0}}, {{0, 6}, {6, 2}, {8, 8}, {-1, 0}}},
        {{{0, 6}, {14, 2}, {-1, 0}}, {{0, 6}, {10, 2}, {14, 2}, {-1, 0}}, {{0, 6}, {7, 2}, {14, 2}, {-1, 0}}},
    };
    for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++) {
        unsigned want[3] = {0, 0, 0};
        int ok = 1, count[3] = {0, 0, 0};
        for (int v = 0; v < 3; v++)
            for (const pl_t *h = V[style - GEN_CH2_FIRST][v]; h->pos >= 0; h++) want[v] |= 1u << h->pos;
        for (int seed = 1; seed <= 300; seed++) {
            int v = gen_chords_v2_variant(style, seed);
            if (v < 0 || v > 2) {
                ok = 0;
                continue;
            }
            count[v]++;
            if (seed > 60) continue;
            gen_params_t p = mk(style, seed, 0, 4, 16);
            gen(&p, st);
            for (int b = 0; b < 4; b++) {
                ok &= bar_mask(st, b) == want[v];
                for (const pl_t *h = V[style - GEN_CH2_FIRST][v]; h->pos >= 0; h++) ok &= st[b * 16 + h->pos].len == h->len;
            }
        }
        CHECK(ok && want[0] != want[1] && want[1] != want[2] && want[0] != want[2],
              "%s: each seed's bars play its variant's template and lengths at c 0; the 3 variants differ", NAME[style]);
        CHECK(count[0] >= 70 && count[1] >= 70 && count[2] >= 70, "%s: each variant is picked by about a third of seeds (%d %d %d)",
              NAME[style], count[0], count[1], count[2]);
    }
    gen_params_t p = mk(GEN_CH_OFFBEAT, variant_seed(GEN_CH_OFFBEAT, 2), 0, 4, 4);
    gen(&p, st);
    CHECK(bar_mask(st, 0) == 0x4444u && bar_mask(st, 3) == 0x4444u,
          "Offbeat's 6/14 variant under 1/4-bar chords plays variant 0's & in each chord (0x%04x)", bar_mask(st, 0));
    p = mk(GEN_CH_OFFBEAT, variant_seed(GEN_CH_OFFBEAT, 2), 100, 4, 16);
    gen(&p, st);
    CHECK(st[6].len == 4 && st[14].len == 2, "Offbeat's 6/14 variant keeps its lengths at c 100 (%d %d)", st[6].len, st[14].len);
    CHECK(gen_chords_v2_variant(GEN_CH_SUSTAIN, 7) == 0 && gen_chords_v2_variant(GEN_MANUAL, 7) == 0,
          "gen_chords_v2_variant: 0 for the other styles");
}

/* lengths (variant 0): Charleston long then short, Ballad held for the bar, Funk short, Half-time long, Push's & of 4 a stab */
static void ch2_test_lengths(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    gen_params_t p = mk(GEN_CH_CHARLESTON, variant_seed(GEN_CH_CHARLESTON, 0), 0, 4, 16);
    gen(&p, st);
    CHECK(st[0].len == 6 && st[6].len == 2, "Charleston: 0 long (6), 6 short (2) (%d %d)", st[0].len, st[6].len);
    p = mk(GEN_CH_BALLAD, variant_seed(GEN_CH_BALLAD, 0), 0, 4, 16);
    gen(&p, st);
    CHECK(st[0].len == 16, "Ballad: 0 held for the bar (%d)", st[0].len);
    p = mk(GEN_CH_BALLAD, variant_seed(GEN_CH_BALLAD, 0), 0, 4, 8);
    gen(&p, st);
    CHECK(st[0].len == 8 && st[8].on && st[8].len == 8, "Ballad, 1/2-bar chords: held to the chord change (%d %d)", st[0].len, st[8].len);
    p = mk(GEN_CH_FUNK, variant_seed(GEN_CH_FUNK, 0), 0, 4, 16);
    gen(&p, st);
    int fl = 1;
    for (int i = 0; i < 64; i++) fl &= !st[i].on || st[i].len == 1;
    CHECK(fl, "Funk: every hit 1 step");
    p = mk(GEN_CH_HALFTIME, variant_seed(GEN_CH_HALFTIME, 0), 0, 4, 16);
    gen(&p, st);
    CHECK(st[0].len == 8 && st[8].len == 8, "Half-time: 0 and 8 held 8 steps (%d %d)", st[0].len, st[8].len);
    p = mk(GEN_CH_PUSH, variant_seed(GEN_CH_PUSH, 0), 0, 4, 16);
    gen(&p, st);
    CHECK(st[14].len == 2 && st[30].len == 2 && st[0].len == 6, "Push: 1 held 6, the & of 4 a 2-step stab to the bar line (%d %d)",
          st[0].len, st[14].len);
}

/* the template steps are the strongest hits: ghost hits are quieter than every template hit of their bar */
static void ch2_test_ghost_vel(void) {
    static seq_step_t lo[SEQ_LANE_STEPS], hi[SEQ_LANE_STEPS];
    int bad = 0, ghosts = 0;
    for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++)
        for (int seed = 0; seed < 30; seed++) {
            gen_params_t p = mk(style, seed, 0, 4, 16), q = mk(style, seed, 100, 4, 16);
            gen(&p, lo);
            gen(&q, hi);
            int minv = 127;
            for (int i = 0; i < 64; i++)
                if (lo[i].on && lo[i].vel < minv) minv = lo[i].vel;
            for (int i = 0; i < 64; i++)
                if (hi[i].on && !lo[i].on && !(style == GEN_CH_BALLAD && i % 16 == 8) && !(style == GEN_CH_PUSH && i % 16 == 6))
                    ghosts++, bad += hi[i].vel >= minv;
        }
    CHECK(ghosts > 0 && !bad, "ghost hits are quieter than the template's (%d ghosts, %d loud)", ghosts, bad);
}

/* COMPLEXITY nests: an onset at c is on at every higher c, and c 100 adds ghost hits */
static void ch2_test_nesting(void) {
    static seq_step_t lo[SEQ_LANE_STEPS], hi[SEQ_LANE_STEPS];
    for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++) {
        int lost = 0, more = 0, cases = 0;
        for (int k = 0; k < NCASE; k++) {
            gen_params_t p = case_params(style, k);
            for (int c = p.complexity + 1; c <= p.complexity + 10 && c <= 100; c++) {
                gen_params_t q = p;
                q.complexity = c;
                gen(&p, lo);
                gen(&q, hi);
                for (int i = 0; i < loop_len(&p); i++) lost += lo[i].on && !hi[i].on;
            }
            if (k < C_STRIDE && p.chord_len >= 8) {
                gen_params_t q = case_params(style, k + 10 * C_STRIDE);
                gen(&p, lo);
                gen(&q, hi);
                cases++;
                more += count_on(&q, hi) > count_on(&p, lo);
            }
        }
        CHECK(!lost, "%s: every onset at c is still on at every higher c (%d lost)", NAME[style], lost);
        CHECK(more == cases, "%s: c 100 has more hits than c 0 (%d of %d cases)", NAME[style], more, cases);
    }
}

/* the seed moves the ghost hits and the velocities, never a variant's template (ch2_test_variants: the variant) */
static void ch2_test_seed(void) {
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++) {
        int on_differ = 0, vel_differ = 0, c0_same = 1, det = 1;
        for (int seed = 1; seed < 30; seed++) {
            gen_params_t p = mk(style, seed, 100, 4, 16), q = mk(style, seed + 100, 100, 4, 16);
            gen(&p, a);
            gen(&q, b);
            int od = 0, vd = 0;
            for (int i = 0; i < 64; i++) od |= a[i].on != b[i].on, vd |= a[i].on && b[i].on && a[i].vel != b[i].vel;
            on_differ += od, vel_differ += vd;
            gen(&p, b);
            det &= !memcmp(a, b, sizeof a);
            p.complexity = q.complexity = 0;
            gen(&p, a);
            gen(&q, b);
            if (gen_chords_v2_variant(style, p.seed) == gen_chords_v2_variant(style, q.seed))   /* 0.13: one template */
                for (int i = 0; i < 64; i++) c0_same &= a[i].on == b[i].on;
        }
        CHECK(on_differ > 10 && vel_differ > 10 && c0_same && det,
              "%s: GEN moves the ghost hits (%d of 29) and velocities (%d), not c 0's template of a variant; deterministic", NAME[style],
              on_differ, vel_differ);
    }
}

/* well-formed steps: chord-relative root at octave 0, vel 1..127, len 1..16, never past the next hit of the span or
 * its chord's end (the loop's end included); steps past the loop are untouched */
static void ch2_test_rules(void) {
    static seq_step_t st[SEQ_LANE_STEPS];
    for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++) {
        int bad = 0, past = 0, cross = 0;
        for (int k = 0; k < NCASE; k++) {
            gen_params_t p = case_params(style, k);
            gen(&p, st);
            int n = loop_len(&p);
            for (int i = 0; i < n; i++) {
                const seq_step_t *x = &st[i];
                bad += x->tone != TONE_R || x->oct != 0 || x->abs || x->len < 1 || x->len > 16 || x->vel < 1 || x->vel > 127 || x->on > 1;
                if (!x->on) continue;
                int e = (i / p.chord_len + 1) * p.chord_len, j = i + 1;
                if (e > n) e = n;
                while (j < e && !st[j].on) j++;
                if (j < e) bad += i + x->len > j;
                else cross += i + x->len > e;
            }
            for (int i = n; i < SEQ_LANE_STEPS; i++) past += st[i].on != 0xAB;
        }
        CHECK(!bad && !past && !cross, "%s: well-formed steps (%d bad, %d crossing a chord change, %d past the loop)",
              NAME[style], bad, cross, past);
    }
}

/* rhythm only: the key, scale and progression change nothing; gen_span_v1 and gen_spans_v1 give the lane's steps */
static void ch2_test_span_harmony(void) {
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++) {
        int differ = 0, span_bad = 0, spans_bad = 0;
        for (int k = 0; k < NCASE; k += 3) {
            gen_params_t p = case_params(style, k), q = p;
            q.key = k % 12, q.scale = k / 12 % SCALE_COUNT, q.prog = k % 2 ? P3 : P8, q.prog_len = k % 2 ? 3 : 8;
            gen(&p, a);
            gen(&q, b);
            differ += memcmp(a, b, sizeof a) != 0;
            int n = loop_len(&p), step = (k * 37) % n, s0 = step / p.chord_len * p.chord_len, s1 = s0 + p.chord_len;
            if (s1 > n) s1 = n;
            memset(b, 0xAB, sizeof b);
            gen_span_v1(&p, step, b);
            for (int i = 0; i < SEQ_LANE_STEPS; i++)
                span_bad += i >= s0 && i < s1 ? memcmp(&a[i], &b[i], sizeof a[i]) != 0 : b[i].on != 0xAB;
            memset(b, 0xAB, sizeof b);
            gen_spans_v1(&p, SEQ_STEPS, b);
            spans_bad += memcmp(a, b, sizeof a[0] * (size_t)(SEQ_STEPS < n ? SEQ_STEPS : n)) != 0;
        }
        CHECK(!differ && !gen_reads_harmony(style) && !gen_transposes(style),
              "%s: reads no harmony, never transposes (%d cases differ)", NAME[style], differ);
        CHECK(!span_bad && !spans_bad, "%s: gen_span_v1 and gen_spans_v1 give the lane's steps (%d, %d bad)", NAME[style],
              span_bad, spans_bad);
    }
}

/* Pins gen_chords_v2: the six styles x seeds 1..2 x c {0, 25, 50, 75, 100} x chord lengths {4, 8, 16, 32} x {1, 4} bars,
 * at an explicit variant. Saved generated lanes keep only their recipe (the variant included, 0.13's `.cv`), so this is
 * save-critical like the v1 goldens. Variant 0 is 0.12's template: its golden is 0.12's v2 golden, unchanged (a 0.12
 * lane has no `.cv` and plays variant 0). 0.13 added variants 1 and 2, pinned by the second golden. */
#define GEN_V2_GOLDEN_CHORDS 0x048fadfdu
#define GEN_V2_GOLDEN_VARIANTS 0xe0ae93a9u

static unsigned golden_chords_v2(int v_lo, int v_hi) {
    unsigned h = 2166136261u;
    const int cls[4] = {4, 8, 16, 32}, bars[2] = {1, 4};
    for (int v = v_lo; v <= v_hi; v++)
        for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++)
            for (int k = 0; k < 2 * 5 * 4 * 2; k++) {
                gen_params_t p = mk(style, 1 + k % 2, k / 2 % 5 * 25, bars[k / 40], cls[k / 10 % 4]);
                p.variant = v;
                h = hash_lane(h, &p);
            }
    return h;
}

/* ---- 0.12 progressions (gen_progression_v2) ---- */

static void prog2_test(void) {
    int deg[9], src[9], ref[9], same = 1, src0 = 1, form = 1, kept = 1, borrowed100 = 0, cases100 = 0, borrowed61 = 0;
    int cases61 = 0, short_b = 0, bsrc[4] = {0};
    for (int scale = 0; scale < SCALE_COUNT; scale++)
        for (int seed = 1; seed <= 400; seed++)
            for (int len = 2; len <= 8; len++)
                for (int c = 0; c <= 100; c += c < 60 ? 15 : 1) {
                    for (int i = 0; i < 9; i++) deg[i] = src[i] = -1;
                    gen_progression_v2(seed, c, len, deg, src, scale);
                    gen_progression_v1(seed, c, len, ref);
                    form &= deg[len] == -1 && src[len] == -1;
                    int nb = 0;
                    for (int i = 0; i < len; i++) {
                        if (!src[i]) { kept &= deg[i] == ref[i]; continue; }
                        nb++;
                        form &= i > 0 && i < len - 1 && src[i] >= 1 && src[i] <= THEORY_NBORROW &&
                                deg[i] == theory_borrow_degree(scale, src[i]);
                        bsrc[src[i]]++;
                    }
                    form &= nb <= 1;
                    if (c <= 60) {
                        same &= !memcmp(deg, ref, sizeof(int) * (size_t)len);
                        src0 &= !nb;
                    }
                    if (len == 2) short_b += nb;
                    if (len >= 3 && c == 100) cases100++, borrowed100 += nb;
                    if (len >= 3 && c == 61) cases61++, borrowed61 += nb;
                }
    CHECK(same && src0, "gen_progression_v2 at HARMONY <= 60: gen_progression_v1's degrees, no borrow");
    CHECK(form && kept, "gen_progression_v2: at most one borrowed slot, never first or last, its degree the borrow's; the "
          "rest is gen_progression_v1's");
    CHECK(!short_b, "gen_progression_v2: 2 chords never borrow");
    CHECK(borrowed100 * 100 > cases100 * 44 && borrowed100 * 100 < cases100 * 56,
          "HARMONY 100 borrows in about half the progressions (%d of %d)", borrowed100, cases100);
    CHECK(borrowed61 * 1000 < cases61 * 30, "HARMONY 61 borrows rarely (%d of %d)", borrowed61, cases61);
    CHECK(bsrc[1] && bsrc[2] && bsrc[3], "all three borrows appear (%d %d %d)", bsrc[1], bsrc[2], bsrc[3]);
    int nest = 1, d2[9], s2[9];
    for (int seed = 1; seed <= 2000; seed++)
        for (int c = 61; c < 100; c++) {
            gen_progression_v2(seed, c, 6, deg, src, SCALE_MAJOR);
            gen_progression_v2(seed, c + 1, 6, d2, s2, SCALE_MAJOR);
            for (int i = 0; i < 6; i++) nest &= !src[i] || (s2[i] == src[i] && d2[i] == deg[i]);
        }
    CHECK(nest, "a borrow at some HARMONY is the same borrow at every higher one (same seed)");
    unsigned h = 2166136261u;
    for (int seed = 1; seed <= 20; seed++)
        for (int c = 0; c <= 50; c += 50)
            for (int len = 2; len <= 8; len++) {
                gen_progression_v2(seed, c, len, deg, src, SCALE_MINOR);
                for (int i = 0; i < len; i++) h = fnv(h, (unsigned char)deg[i]);
            }
    unsigned g = 2166136261u;
    for (int seed = 1; seed <= 20; seed++)
        for (int c = 0; c <= 50; c += 50)
            for (int len = 2; len <= 8; len++) {
                gen_progression_v1(seed, c, len, ref);
                for (int i = 0; i < len; i++) g = fnv(g, (unsigned char)ref[i]);
            }
    CHECK(h == g, "gen_progression_v2 keeps the PROG golden's degrees at c 0 and 50 (0x%08x)", h);
    gen_progression_v2(1, 100, 0, deg, src, 0);
}

static void ch2_test_golden(void) {
    unsigned h = golden_chords_v2(0, 0);
    CHECK(h == GEN_V2_GOLDEN_CHORDS, "gen_chords_v2 golden checksum, variant 0 (0.12's) 0x%08x", h);
    h = golden_chords_v2(1, 2);
    CHECK(h == GEN_V2_GOLDEN_VARIANTS, "gen_chords_v2 golden checksum, variants 1 and 2 0x%08x", h);
}

/* 0.16 NEW BAR on the bass styles (and the classic melodies): a bar's own variation changes that bar, never another */
static void test_bar_var(void) {
    static const int STYLES[3] = {GEN_BASS_ROOT, GEN_BASS_PUMP, GEN_BASS_WALK};
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    unsigned char bv[SEQ_MAX_BARS];
    int local = 1, changed = 0, n = 0;
    for (int si = 0; si < 3; si++)
        for (int seed = 1; seed <= 40; seed++)
            for (int c = 0; c <= 90; c += 30) {   /* c 0: a Root bass's one root per bar must change too */
                gen_params_t p = mk(STYLES[si], seed, c, 4, 16);
                memset(bv, 0, sizeof bv);
                gen_lane_v1(&p, a);
                bv[2] = (unsigned char)(1 + seed % 5);
                p.bar_var = bv;
                gen_lane_v1(&p, b);
                for (int t = 0; t < 7 && !memcmp(a + 32, b + 32, sizeof a[0] * SEQ_STEPS); t++) {   /* NEW BAR's retries */
                    bv[2]++;
                    gen_lane_v1(&p, b);
                }
                for (int i = 0; i < 4 * SEQ_STEPS; i++)
                    if (i / SEQ_STEPS != 2) local &= a[i].on == b[i].on && (!a[i].on || (a[i].tone == b[i].tone && a[i].oct == b[i].oct));
                changed += memcmp(a + 32, b + 32, sizeof a[0] * SEQ_STEPS) != 0;
                n++;
            }
    CHECK(local, "NEW BAR (bass Root, Pump, Walk): bar 3's own variation leaves bars 1, 2 and 4 exactly as they were");
    CHECK(changed == n, "and NEW BAR (with its retries) always changes bar 3 itself (%d of %d cases, c 0 included)", changed, n);
}
int main(void) {
    test_rng();
    test_determinism();
    test_manual();
    for (int style = 1; style <= GEN_BASS_WALK; style++) {   /* Chords and Bass; the melodies have their own below */
        test_nesting(style);
        test_range(style);
        test_rules(style);
        test_edges(style);
        for (int cl = 4; cl <= 16; cl *= 2) test_syncopation(style, cl);
        test_repetition(style);
    }
    test_shapes();
    test_spread();
    test_seed_variety();
    test_walk_pitch();
    test_pump_octave();
    test_root_octave();
    test_sustain_cover();
    test_walk_tone_stable();
    test_stabs_gate();
    test_prog_rules();
    test_prog_variety();
    test_prog_edges();
    test_span();
    test_transpose();
    test_cache();
    for (int v = 0; v < NVOICE; v++) {
        mel_test_nesting(v);
        mel_test_rules(v);
    }
    mel_test_steps();
    mel_test_answer();
    mel_test_fallback();
    mel_test_breath();
    mel_test_sweet();
    mel_test_answer_seeds();
    mel_test_answer_nct();
    mel_test_arp();
    mel_test_contour();
    mel_test_variety();
    mel_test_span();
    mel_test_timing();
    test_reads_harmony();
    test_timing();
    test_golden();
    ch2_test_templates();
    ch2_test_variants();
    ch2_test_lengths();
    ch2_test_ghost_vel();
    ch2_test_nesting();
    ch2_test_seed();
    ch2_test_rules();
    ch2_test_span_harmony();
    ch2_test_golden();
    prog2_test();
    test_bar_var();
    printf(fails ? "\n%d FAILED\n" : "\nall gen tests passed\n", fails);
    return fails ? 1 : 0;
}
