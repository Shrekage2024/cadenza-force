/* Offline test of src/melody.c (run by tests/run_tests.sh under ASan/UBSan). Exit 1 on failure. */
#include "../src/melody.h"
#include "../src/theory.h"
#include "../src/gen.h"   /* gen_rng: the fuzz draws the same cases on every libc */
#include <stdio.h>
#include <string.h>

static int fails;

#define WHY 96   /* check_case's reason buffer */

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

#define MAX_SPANS (SEQ_LANE_STEPS / 4)
static const int BARS[5] = {1, 2, 4, 8, 16};
static const int CLS[4] = {4, 8, 16, 32};
static const int PROG[4] = {0, 5, 2, 6};   /* i-VI-III-VII */
static const int MOTIF[4] = {MEL_OST, MEL_PENT, MEL_HOOK, MEL_SEQ};

static int pmod(int a, int m) { return ((a % m) + m) % m; }

static mel_chord_t diatonic(int key, int scale, int deg) {
    mel_chord_t c = {theory_degree_pc(key, scale, deg),
                     {theory_degree_pc(key, scale, deg), theory_degree_pc(key, scale, deg + 2), theory_degree_pc(key, scale, deg + 4), 0},
                     3, theory_degree_pc(key, scale, deg + 6)};
    return c;
}

/* the chords of every span: PROG cycling, one degree per span */
static void fill_prog(int key, int scale, mel_chord_t *ch) {
    for (int i = 0; i < MAX_SPANS; i++) ch[i] = diatonic(key, scale, PROG[i % 4]);
}

typedef struct { mel_recipe_t r; int cx; mel_params_t p; mel_chord_t ch[MAX_SPANS]; } tcase_t;

/* E minor, i-VI-III-VII, `type` at its defaults with 0.13's 1 + 1 phrase, which the tests before VST-12 pin (test_phrasing
 * sets its own) */
static void tc_init(tcase_t *t, int type, int bars, int cl) {
    memset(t, 0, sizeof *t);
    mel_defaults(type, &t->r, &t->cx);
    mel_phrase_from_legacy(&t->r, MEL_PHRASE_1_1);
    fill_prog(4, SCALE_MINOR, t->ch);
    t->p = (mel_params_t){&t->r, 123, t->cx, bars, cl, 4, SCALE_MINOR, t->ch, NULL};
}

static void gen(const tcase_t *t, seq_step_t *st) {
    mel_params_t p = t->p;
    p.r = &t->r;
    p.chord = t->ch;
    mel_generate_v1(&p, st);
}

static int last_on(const seq_step_t *st, int L) {
    for (int i = L - 1; i >= 0; i--) if (st[i].on) return i;
    return -1;
}

static int in_scale(int key, int scale, int note) {
    for (int d = 0; d < 7; d++) if (theory_degree_pc(key, scale, d) == note % 12) return 1;
    return 0;
}

/* the types that move in scale steps: all but Arp, and Approach only with Step Scale below TENSION 67 */
static int scale_steps(const mel_recipe_t *r) {
    return r->type != MEL_ARP && !(r->type == MEL_APPR && (r->opt[1] == 1 || r->kn[MK_TENSION] >= 67));
}

/* Every check of one generation: determinism, range, nothing past the loop, every bar sounding, vel/len, last tonic. */
static int check_case(const tcase_t *t, char *why) {
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    int L = t->p.bars * SEQ_STEPS;
    memset(a, 0xAB, sizeof a);
    memset(b, 0xCD, sizeof b);
    gen(t, a);
    gen(t, b);
    for (int i = L; i < SEQ_LANE_STEPS; i++)
        if (a[i].on != 0xAB || a[i].abs != 0xAB) return snprintf(why, WHY, "step %d past the loop written", i), 0;
    if (memcmp(a, b, L * sizeof a[0])) return snprintf(why, WHY, "not deterministic"), 0;
    for (int bar = 0; bar < t->p.bars && !(t->r.shift % SEQ_STEPS); bar++) {   /* SHIFT rotates the loop: it may empty a bar */
        int n = 0;
        for (int s = 0; s < SEQ_STEPS; s++) n += a[bar * SEQ_STEPS + s].on;
        if (!n) return snprintf(why, WHY, "bar %d empty", bar), 0;
    }
    for (int i = 0; i < L; i++) {
        if (a[i].on > 1) return snprintf(why, WHY, "step %d on=%d", i, a[i].on), 0;
        if (!a[i].on) continue;
        if (a[i].abs < 50 || a[i].abs > 96) return snprintf(why, WHY, "step %d note %d out of range", i, a[i].abs), 0;
        if (a[i].len < 1 || a[i].len > 4) return snprintf(why, WHY, "step %d len %d", i, a[i].len), 0;
        if (a[i].vel != (i % 4 ? 90 : 100)) return snprintf(why, WHY, "step %d vel %d", i, a[i].vel), 0;
        if (a[i].tone != TONE_R || a[i].oct != 0) return snprintf(why, WHY, "step %d placeholder", i), 0;
    }
    if (scale_steps(&t->r)) {
        unsigned ok = 0;   /* the project scale, plus every tone of the loop's chords (a borrowed chord's) */
        for (int d = 0; d < 7; d++) ok |= 1u << theory_degree_pc(t->p.key, t->p.scale, d);
        for (int i = 0; i < (L + t->p.chord_len - 1) / t->p.chord_len; i++) {
            for (int k = 0; k < t->ch[i].ntones; k++) ok |= 1u << t->ch[i].tones[k];
            ok |= 1u << t->ch[i].seventh;
        }
        for (int i = 0; i < L; i++)
            if (a[i].on && !(ok >> (a[i].abs % 12) & 1u)) return snprintf(why, WHY, "step %d note %d out of key", i, a[i].abs), 0;
    }
    int l = last_on(a, L);
    if (!t->r.rev && !t->r.inv && !t->r.shift && t->r.ph[MP_CADENCE] != MEL_CAD_NONE && a[l].abs % 12 != t->p.key)
        return snprintf(why, WHY, "last note %d is not a tonic", a[l].abs), 0;
    return 1;
}

/* every type x option x phrasing (the old two, the type's own and three more) x 1/2/4/8/16 bars x 4 chord lengths, at
 * complexity 0, default and 100 */
static void test_grid(void) {
    static const unsigned char PH[4][MP_COUNT] = {{3, 3, 2, 1}, {0, 0, 0, 0}, {2, 1, 2, 1}, {0, 2, 1, 0}};
    static tcase_t t;
    char why[WHY] = "";
    int ok = 1, n = 0;
    for (int type = 0; type < MEL_NTYPES && ok; type++) {
        int n0 = mel_nopts(type, 0), n1 = mel_nopts(type, 1), n2 = mel_nopts(type, 2);
        for (int o = 0; o < n0 * n1 * n2 && ok; o++)
            for (int ph = 0; ph < 7 && ok; ph++)
                for (int bi = 0; bi < 5 && ok; bi++)
                    for (int ci = 0; ci < 4 && ok; ci++)
                        for (int cx = 0; cx < 3 && ok; cx++) {
                            tc_init(&t, type, BARS[bi], CLS[ci]);
                            t.r.opt[0] = (unsigned char)(o % n0);
                            t.r.opt[1] = (unsigned char)(o / n0 % n1);
                            t.r.opt[2] = (unsigned char)(o / n0 / n1);
                            if (ph < MEL_NPHRASES) mel_phrase_from_legacy(&t.r, ph);
                            else if (ph == MEL_NPHRASES) mel_defaults(type, &t.r, NULL);
                            else memcpy(t.r.ph, PH[ph - MEL_NPHRASES - 1], MP_COUNT);
                            t.r.opt[0] = (unsigned char)(o % n0);
                            t.r.opt[1] = (unsigned char)(o / n0 % n1);
                            t.r.opt[2] = (unsigned char)(o / n0 / n1);
                            if (cx != 1) t.p.complexity = cx ? 100 : 0;
                            ok = check_case(&t, why);
                            n++;
                            if (!ok) printf("  type %d opts %d %d %d phrasing %d bars %d cl %d cx %d: %s\n", type, t.r.opt[0],
                                            t.r.opt[1], t.r.opt[2], ph, BARS[bi], CLS[ci], t.p.complexity, why);
                        }
    }
    CHECK(ok, "grid: %d generations deterministic, in 50..96, in key, nothing past the loop, no empty bar, last note a tonic"
              " (with a cadence)", n);
}

/* 0..m-1 from the test's own xorshift (gen_rng), not libc rand(), so every platform runs the same cases */
static int rnd(gen_rng *r, int m) { return (int)(gen_next(r) % (unsigned)m); }

/* random recipes, knobs, moves, seeds, keys, scales and coloured / borrowed-looking chords */
static void test_fuzz(void) {
    static tcase_t t;
    char why[WHY] = "";
    int ok = 1;
    gen_rng r;
    gen_seed(&r, 7);
    for (int k = 0; k < 20000 && ok; k++) {
        int type = rnd(&r, MEL_NTYPES);
        tc_init(&t, type, BARS[rnd(&r, 5)], CLS[rnd(&r, 4)]);
        for (int w = 0; w < 3; w++) t.r.opt[w] = (unsigned char)(rnd(&r, mel_nopts(type, w)));
        for (int w = 0; w < MP_COUNT; w++) t.r.ph[w] = (unsigned char)(rnd(&r, mel_phrase_n(w)));
        for (int j = 0; j < MK_COUNT; j++) t.r.kn[j] = (unsigned char)(rnd(&r, 101));
        t.r.rev = (unsigned char)(rnd(&r, 2));
        t.r.inv = (unsigned char)(rnd(&r, 2));
        t.r.shift = (unsigned char)(rnd(&r, 16));
        t.p.seed = rnd(&r, 65536);
        t.p.complexity = rnd(&r, 101);
        t.p.key = rnd(&r, 12);
        t.p.scale = rnd(&r, SCALE_COUNT);
        for (int i = 0; i < MAX_SPANS; i++) {
            t.ch[i] = diatonic(t.p.key, t.p.scale, rnd(&r, 7));
            if (rnd(&r, 3) == 0) t.ch[i].tones[1] = (t.ch[i].tones[1] + 1) % 12;   /* a borrowed third */
            if (rnd(&r, 2)) t.ch[i].tones[t.ch[i].ntones++] = t.ch[i].seventh;
        }
        ok = check_case(&t, why);
        if (!ok) printf("  case %d type %d: %s\n", k, type, why);
    }
    CHECK(ok, "fuzz: 20000 random recipes (phrasing too), keys, scales and chords pass every check (in key or a chord's tone)");
}

static unsigned onset_mask(const seq_step_t *st, int bar) {
    unsigned m = 0;
    for (int s = 0; s < SEQ_STEPS; s++) if (st[bar * SEQ_STEPS + s].on) m |= 1u << s;
    return m;
}

/* motif types: every onset at complexity c is still there at c + 1 */
static void test_nesting(void) {
    static tcase_t t;
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    for (int m = 0; m < 4; m++) {
        int ok = 1, grew = 0;
        for (int seed = 0; seed < 20 && ok; seed++)
            for (int ans = 0; ans < 3 && ok; ans++) {
                tc_init(&t, MOTIF[m], 4, 16);
                t.p.seed = seed * 977;
                t.r.kn[MK_ANSWER] = (unsigned char)(ans * 40);
                t.r.kn[MK_SYNC] = (unsigned char)(seed % 2 ? 80 : 20);
                for (int c = 0; c < 100 && ok; c++) {
                    t.p.complexity = c;
                    gen(&t, a);
                    t.p.complexity = c + 1;
                    gen(&t, b);
                    for (int bar = 0; bar < 4; bar++) {
                        unsigned ma = onset_mask(a, bar), mb = onset_mask(b, bar);
                        if ((ma & mb) != ma) ok = 0;
                        if (mb != ma) grew = 1;
                    }
                }
            }
        CHECK(ok && grew, "%s: onsets nest as complexity rises (and do grow)", MEL_TYPE_NAME[MOTIF[m]]);
    }
}

/* 2 + 2: bars 2 and 3 (answers) reuse bars 0 and 1's cells; 1 + 1: bar 1 reuses bar 0's (bar 0 breathes from 12);
 * the phrase repeats every 4 bars */
static void test_phrase_reuse(void) {
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    for (int m = 0; m < 4; m++) {
        int ok = 1;
        for (int seed = 0; seed < 30 && ok; seed++) {
            tc_init(&t, MOTIF[m], 8, 16);
            t.p.seed = seed * 131 + 5;
            t.r.kn[MK_ANSWER] = 0;
            mel_phrase_from_legacy(&t.r, MEL_PHRASE_2_2);
            gen(&t, st);
            for (int b = 0; b < 8; b++)   /* bar 1 (the second call) breathes from 12; its answer, bar 3, does not */
                if ((onset_mask(st, b) & (b % 2 ? 0x0FFFu : 0xFFFFu)) != onset_mask(st, b % 2)) ok = 0;
            mel_phrase_from_legacy(&t.r, MEL_PHRASE_1_1);
            gen(&t, st);
            if ((onset_mask(st, 1) & 0x0FFFu) != onset_mask(st, 0)) ok = 0;
            if ((onset_mask(st, 3) & 0x0FFFu) != onset_mask(st, 2)) ok = 0;
            for (int b = 4; b < 8; b++) if (onset_mask(st, b) != onset_mask(st, b - 4)) ok = 0;
        }
        CHECK(ok, "%s: answer bars reuse their call's rhythm cell; the phrase repeats every 4 bars", MEL_TYPE_NAME[MOTIF[m]]);
    }
    /* Pentatonic's anchor is fixed, so an echoed answer is its call note for note */
    tc_init(&t, MEL_PENT, 4, 16);
    t.r.kn[MK_ANSWER] = 0;
    mel_phrase_from_legacy(&t.r, MEL_PHRASE_2_2);
    gen(&t, st);
    int same = 1;
    for (int s = 0; s < SEQ_STEPS; s++) same &= st[32 + s].on == st[s].on && (!st[s].on || st[32 + s].abs == st[s].abs);
    CHECK(same, "Pentatonic 2 + 2, ANSWER 0: bar 3 echoes bar 1 note for note");
}

/* the last note lands on a tonic in every key and scale */
static void test_last_tonic(void) {
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    int ok = 1;
    for (int type = 0; type < MEL_NTYPES; type++)
        for (int key = 0; key < 12; key++)
            for (int sc = 0; sc < SCALE_COUNT; sc++) {
                tc_init(&t, type, 4, 16);
                t.p.key = key;
                t.p.scale = sc;
                fill_prog(key, sc, t.ch);
                gen(&t, st);
                int l = last_on(st, 64);
                if (l < 0 || st[l].abs % 12 != key) ok = 0;
            }
    CHECK(ok, "every type, key and scale: the loop's last note is a tonic");
}

/* Review Focus 3: Guide tones over a borrowed IV in E minor (A major) plays C#; over A7 it plays C# and G */
static void test_guide_borrowed(void) {
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    mel_chord_t A = {9, {9, 1, 4, 0}, 3, 7}, A7 = {9, {9, 1, 4, 7}, 4, 7};
    tc_init(&t, MEL_GUIDE, 4, 16);
    t.ch[1] = A;
    t.r.opt[0] = 1;   /* 3rd only */
    gen(&t, st);
    int ok = 1, n = 0;
    for (int s = 16; s < 32; s++) if (st[s].on) { n++; ok &= st[s].abs % 12 == 1; }
    CHECK(ok && n, "Guide tones, 3rd only, over a borrowed A major in E minor: every note is C# (%d notes)", n);
    t.ch[1] = A7;
    t.r.opt[0] = 0;
    t.r.opt[1] = 1;   /* Half */
    gen(&t, st);
    int third = 0, sev = 0, other = 0;
    for (int s = 16; s < 32; s++)
        if (st[s].on) { int pc = st[s].abs % 12; third += pc == 1; sev += pc == 7; other += pc != 1 && pc != 7; }
    CHECK(third && sev && !other, "Guide tones, 3rd + 7th, Half, over A7: C# and G only (%d, %d, %d)", third, sev, other);
    /* a half-bar chord change is followed: bar 2's second half is over the A chord */
    tc_init(&t, MEL_GUIDE, 4, 8);
    t.r.opt[0] = 1;
    t.ch[3] = A;
    gen(&t, st);
    ok = 1, n = 0;
    for (int s = 24; s < 32; s++) if (st[s].on) { n++; ok &= st[s].abs % 12 == 1; }
    CHECK(ok && n, "Guide tones re-read a half-bar chord: the A major half plays C# (%d notes)", n);
}

static int pent_index(int note) {   /* E minor pentatonic steps from C-1: E G A B D */
    int i = 0;
    for (int m = 0; m < note; m++) i += m % 12 == 4 || m % 12 == 7 || m % 12 == 9 || m % 12 == 11 || m % 12 == 2;
    return i;
}

/* ups among the cyclic moves between steps 0, 2, 4 and 6 of a bar */
static int ups(const seq_step_t *st, int bar) {
    int n = 0;
    for (int s = 0; s < 6; s += 2) n += st[bar * 16 + s + 2].abs > st[bar * 16 + s].abs;
    return n;
}

/* ANSWER 34..66 flips the answer's contour; 67..100 also moves its rhythm by 2 sixteenths */
static void test_answer(void) {
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    int ok = 1;
    for (int seed = 0; seed < 30; seed++) {
        tc_init(&t, MEL_PENT, 4, 16);
        t.r.opt[1] = 0;   /* Rise */
        mel_phrase_from_legacy(&t.r, MEL_PHRASE_2_2);
        t.p.seed = seed * 71;
        t.r.kn[MK_ANSWER] = 50;
        gen(&t, st);
        if (onset_mask(st, 2) != onset_mask(st, 0)) ok = 0;
        int f = -1;   /* mirrored in pentatonic steps around the first note (which is never nudged) */
        for (int s = 0; s < SEQ_STEPS; s++) {
            if (!st[s].on) continue;
            if (f < 0) { f = s; ok &= st[32 + s].abs == st[s].abs; continue; }
            ok &= pent_index(st[s].abs) - pent_index(st[f].abs) == pent_index(st[32 + f].abs) - pent_index(st[32 + s].abs);
        }
    }
    CHECK(ok, "ANSWER 50: the answer keeps the call's rhythm and flips its contour");
    ok = 1;
    for (int seed = 0; seed < 30; seed++) {
        tc_init(&t, MEL_HOOK, 4, 16);
        mel_phrase_from_legacy(&t.r, MEL_PHRASE_2_2);
        t.p.seed = seed * 71;
        t.r.kn[MK_ANSWER] = 80;
        gen(&t, st);
        unsigned c = onset_mask(st, 0), want = ((c << 2) | (c >> 14)) & 0xFFFFu;
        if (onset_mask(st, 2) != want) ok = 0;
    }
    CHECK(ok, "ANSWER 80: the answer's rhythm is the call's moved 2 sixteenths (wrapping)");
    tc_init(&t, MEL_ARP, 2, 16);
    t.r.opt[1] = 1;   /* 1/8, Up */
    for (int i = 0; i < MAX_SPANS; i++) t.ch[i] = diatonic(4, SCALE_MINOR, 0);
    t.r.kn[MK_ANSWER] = 50;
    gen(&t, st);
    ok = 1;
    for (int seed = 0; seed < 8; seed++) {
        t.p.seed = seed;
        gen(&t, st);
        ok &= ups(st, 0) == 2 && ups(st, 1) == 1;   /* a 3-note cycle: up, up, back down / down, down, back up */
    }
    CHECK(ok, "Arp Up, ANSWER 50: the call climbs, the answer falls (from any seed-picked start)");
}

static int sc_index(int key, int scale, int note) {   /* scale steps from C-1 (note in the scale) */
    int i = 0;
    for (int m = 0; m < note; m++) i += in_scale(key, scale, m);
    return i;
}

/* rev reverses each bar's pitches, inv mirrors them in scale steps around the first note, shift rotates the loop */
static void test_moves(void) {
    static tcase_t t;
    static seq_step_t base[SEQ_LANE_STEPS], st[SEQ_LANE_STEPS];
    int okr = 1, oki = 1, oks = 1;
    for (int type = 0; type < MEL_NTYPES; type++)
        for (int seed = 0; seed < 10; seed++) {
            tc_init(&t, type, 4, 16);
            t.p.seed = seed * 37;
            gen(&t, base);
            t.r.rev = 1;
            gen(&t, st);
            for (int b = 0; b < 4; b++) {
                int on[16], n = 0;
                for (int s = 0; s < 16; s++) if (base[b * 16 + s].on) on[n++] = b * 16 + s;
                for (int j = 0; j < n; j++) okr &= st[on[j]].on && st[on[j]].abs == base[on[n - 1 - j]].abs;
                okr &= onset_mask(st, b) == onset_mask(base, b);
            }
            t.r.rev = 0;
            t.r.inv = 1;
            gen(&t, st);
            int f = -1;
            for (int i = 0; i < 64; i++) {
                if (!base[i].on) continue;
                if (!st[i].on) { oki = 0; continue; }
                if (!in_scale(4, SCALE_MINOR, st[i].abs)) oki = 0;
                if (f < 0) { f = i; oki &= st[i].abs == base[i].abs || !in_scale(4, SCALE_MINOR, base[i].abs); continue; }
                if (!in_scale(4, SCALE_MINOR, base[i].abs) || !in_scale(4, SCALE_MINOR, base[f].abs)) continue;
                int d0 = sc_index(4, SCALE_MINOR, base[i].abs) - sc_index(4, SCALE_MINOR, base[f].abs);
                int d1 = sc_index(4, SCALE_MINOR, st[i].abs) - sc_index(4, SCALE_MINOR, st[f].abs);
                oki &= pmod(d0 + d1, 7) == 0;   /* mirrored, up to the octave fold into 50..96 */
            }
            t.r.inv = 0;
            for (int k = 1; k < 16; k += 5) {
                t.r.shift = (unsigned char)k;
                gen(&t, st);
                for (int i = 0; i < 64; i++) {
                    const seq_step_t *o = &base[pmod(i - k, 64)];
                    oks &= st[i].on == o->on && (!o->on || (st[i].abs == o->abs && st[i].len == o->len));
                }
            }
        }
    CHECK(okr, "rev: each bar's pitches reversed, rhythm kept");
    CHECK(oki, "inv: pitches mirrored in scale steps around the first note");
    CHECK(oks, "shift: the loop rotated right by `shift` 16ths");
}

/* Ruling A: at every type's own defaults, the seed changes the pitches */
static void test_seed_defaults(void) {
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    for (int type = 0; type < MEL_NTYPES; type++) {
        unsigned char seen[8][64];
        int distinct = 0;
        for (int seed = 0; seed < 8; seed++) {
            tc_init(&t, type, 4, 16);
            t.p.seed = seed * 1009 + 3;
            gen(&t, st);
            for (int i = 0; i < 64; i++) seen[seed][i] = st[i].on ? st[i].abs : 0;
            int fresh = 1;
            for (int k = 0; k < seed; k++) fresh &= memcmp(seen[k], seen[seed], 64) != 0;
            distinct += fresh;
        }
        CHECK(distinct >= 2, "%s at its defaults: 8 seeds give %d different lines", MEL_TYPE_NAME[type], distinct);
    }
}

/* Ruling B: Approach re-reads the chord at each target: with 1/4-bar chords every target is its own span's 3rd */
static void test_appr_spans(void) {
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    int ok = 1, n = 0;
    for (int seed = 0; seed < 20; seed++) {
        tc_init(&t, MEL_APPR, 4, 4);   /* Lands on 3rd, complexity 50: targets at 0, 4, 8, 12 */
        t.p.seed = seed;
        gen(&t, st);
        int l = last_on(st, 64);
        for (int i = 0; i < 64; i += 4) {
            if (!st[i].on || i == l) continue;   /* a breath (step 12 of bars 1, 3), or the last note (the tonic) */
            n++;
            ok &= st[i].abs % 12 == t.ch[i / 4].tones[1];
        }
    }
    CHECK(ok && n == 260, "Approach, 1/4-bar chords: each of %d targets lands on its own span's 3rd", n);
}

/* Pentatonic from the scale: E minor uses E G A B D, Home note major starts on G; C major uses C D E G A, minor on A */
static void test_pentatonic(void) {
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    static const int SET[2][5] = {{4, 7, 9, 11, 2}, {0, 2, 4, 7, 9}}, HOME[2][2] = {{4, 7}, {9, 0}};
    int ok = 1;
    for (int k = 0; k < 2; k++)
        for (int home = 0; home < 2; home++)
            for (int seed = 0; seed < 10; seed++) {
                tc_init(&t, MEL_PENT, 4, 16);
                t.p.key = k ? 0 : 4;
                t.p.scale = k ? SCALE_MAJOR : SCALE_MINOR;
                fill_prog(t.p.key, t.p.scale, t.ch);
                t.r.opt[0] = (unsigned char)home;
                t.p.seed = seed;
                gen(&t, st);
                ok &= st[0].on && st[0].abs % 12 == HOME[k][home];
                for (int i = 0; i < 63; i++) {   /* the last note is the tonic, in the set anyway */
                    int pc = st[i].abs % 12, in = 0;
                    for (int j = 0; j < 5; j++) in |= pc == SET[k][j];
                    ok &= !st[i].on || in;
                }
            }
    CHECK(ok, "Pentatonic: five notes of the scale (E minor E G A B D, C major C D E G A); Home note moves the anchor");
}

/* The seed shuffles the onsets within each tier (canvas MG.rhythm), so it shows when a bar's onset count splits a tier
 * (complexity 60: 6 onsets); at 4 onsets (Hook, Ostinato defaults) the rhythm is the same for every seed. */
static void test_seed_variety(void) {
    static tcase_t t;
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    for (int m = 0; m < 4; m++) {
        int diff = 0;
        for (int seed = 1; seed < 10; seed++) {
            tc_init(&t, MOTIF[m], 4, 16);
            t.p.complexity = 60;
            t.p.seed = 0;
            gen(&t, a);
            t.p.seed = seed;
            gen(&t, b);
            diff += memcmp(a, b, 64 * sizeof a[0]) != 0;
        }
        CHECK(diff >= 7, "%s: a new seed gives new notes (%d of 9)", MEL_TYPE_NAME[MOTIF[m]], diff);
    }
}

/* lengths run to the next onset (wrapping), capped at 4 */
static void test_len(void) {
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    int ok = 1;
    for (int type = 0; type < MEL_NTYPES; type++)
        for (int bars = 1; bars <= 4; bars++) {
            tc_init(&t, type, bars, 16);
            int L = bars * 16;
            gen(&t, st);
            for (int i = 0; i < L; i++) {
                if (!st[i].on) continue;
                int d = 1;
                while (d < 4 && !st[(i + d) % L].on) d++;
                ok &= st[i].len == d;
            }
        }
    CHECK(ok, "length: to the next onset, wrapping the loop, at most 4");
}

static int ascii(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) return 0;
    return 1;
}

static void test_names_defaults(void) {
    int ok = 1;
    for (int type = 0; type < MEL_NTYPES; type++) {
        ok &= ascii(MEL_TYPE_NAME[type]);
        for (int w = 0; w < 3; w++) {
            ok &= ascii(mel_row_name(type, w)) && mel_nopts(type, w) >= 2 && mel_nopts(type, w) <= 4;
            for (int i = 0; i < mel_nopts(type, w); i++) ok &= ascii(mel_opt_name(type, w, i));
            ok &= !*mel_opt_name(type, w, mel_nopts(type, w));
        }
    }
    ok &= !mel_nopts(MEL_NTYPES, 0) && !mel_nopts(0, 3) && !*mel_row_name(-1, 0) && !*mel_opt_name(0, 0, -1);
    CHECK(ok, "names: every type, row and option has an ASCII name; out of range is empty");
    CHECK(!strcmp(mel_opt_name(MEL_PENT, 0, 0), "minor") && !strcmp(mel_opt_name(MEL_PENT, 0, 1), "major") &&
          !strcmp(mel_row_name(MEL_ARP, 0), "Direction") && !strcmp(mel_opt_name(MEL_GUIDE, 0, 0), "3rd + 7th") &&
          !strcmp(MEL_TYPE_NAME[MEL_GUIDE], "Guide tones"), "names: Home note minor/major, Direction, 3rd + 7th, Guide tones");
    mel_recipe_t r;
    memset(&r, 0xFF, sizeof r);
    int cx = -1;
    mel_defaults(MEL_HOOK, &r, &cx);
    static const unsigned char kn[MK_COUNT] = {45, 30, 35, 25, 50, 55, 70}, ph[MP_COUNT] = {1, 2, 2, 0};
    CHECK(r.type == MEL_HOOK && r.opt[0] == 1 && r.opt[1] == 2 && r.opt[2] == 2 && !memcmp(r.kn, kn, sizeof kn) &&
          cx == 40 && !memcmp(r.ph, ph, sizeof ph) && !r.rev && !r.inv && !r.shift,
          "defaults: Hook 4 notes, Arch, 5th, 1 + 1 Beat Q&A, knobs 40 45 30 35 25 50 55 70");
    mel_defaults(MEL_GUIDE, &r, NULL);
    CHECK(r.type == MEL_GUIDE && r.opt[0] == 0 && r.opt[1] == 1 && r.opt[2] == 0 && r.kn[MK_GATE] == 90,
          "defaults: Guide tones 3rd + 7th, Half, Smooth, GATE 90 (complexity may be NULL)");
}

/* VST-12: the old phrases as phrasing (mel_phrase_from_legacy) play the notes 0.11-0.13 played, over a grid of every type,
 * loop length, chord length, three keys, seeds, complexities, ANSWER settings, moves and options. The fingerprints are
 * 0.13's output (measured before the phrasing rewrite). */
static void test_legacy_fingerprint(void) {
    static const int KS[3][2] = {{4, 1}, {0, 0}, {2, 2}}, SEEDS[3] = {123, 7, 40000}, CX[3] = {0, 50, 100}, ANS[3] = {10, 50, 80};
    static const int FL[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 3}};
    static const unsigned WANT[MEL_NPHRASES] = {0x8000d4cfu, 0xf76ddbd6u};
    static mel_chord_t ch[MAX_SPANS];
    static seq_step_t st[SEQ_LANE_STEPS];
    for (int ph = 0; ph < MEL_NPHRASES; ph++) {
        unsigned h = 2166136261u;
        for (int t = 0; t < MEL_NTYPES; t++) for (int b = 0; b < 5; b++) for (int c = 0; c < 4; c++) for (int k = 0; k < 3; k++)
        for (int sd = 0; sd < 3; sd++) for (int x = 0; x < 3; x++) for (int a = 0; a < 3; a++) for (int f = 0; f < 3; f++) {
            mel_recipe_t r;
            int cx;
            memset(&r, 0, sizeof r);
            mel_defaults(t, &r, &cx);
            mel_phrase_from_legacy(&r, ph);
            for (int w = 0; w < 3; w++) r.opt[w] = (unsigned char)((sd + w + b) % mel_nopts(t, w));
            r.kn[MK_ANSWER] = (unsigned char)ANS[a];
            r.rev = (unsigned char)FL[f][0], r.inv = (unsigned char)FL[f][1], r.shift = (unsigned char)FL[f][2];
            for (int i = 0; i < MAX_SPANS; i++) ch[i] = diatonic(KS[k][0], KS[k][1], PROG[i % 4]);
            mel_params_t p = {&r, SEEDS[sd], CX[x], BARS[b], CLS[c], KS[k][0], KS[k][1], ch, NULL};
            memset(st, 0, sizeof st);
            mel_generate_v1(&p, st);
            for (int i = 0; i < BARS[b] * SEQ_STEPS; i++) {
                unsigned char v[4] = {st[i].on, st[i].abs, st[i].vel, st[i].len};
                for (int j = 0; j < 4; j++) h = (h ^ v[j]) * 16777619u;
            }
        }
        CHECK(h == WANT[ph], "the old %s phrase as phrasing plays 0.13's notes exactly (34020 cases, 0x%08x)",
              ph ? "2 + 2" : "1 + 1", h);
    }
    mel_recipe_t r;
    memset(&r, 0, sizeof r);
    mel_defaults(MEL_HOOK, &r, NULL);
    int was = mel_is_legacy(&r);
    mel_phrase_from_legacy(&r, MEL_PHRASE_2_2);
    CHECK(!was && mel_is_legacy(&r) && mel_legacy_phrase(&r) == MEL_PHRASE_2_2 && r.ph[MP_BREATH] == MEL_BREATH_BEAT &&
          r.ph[MP_CADENCE] == MEL_CAD_LAND && r.ph[MP_FILL] == MEL_FILL_REST, "the old 2 + 2: Form 2 + 2, Beat, Land, Rest");
}

static int onsets_in(const seq_step_t *st, int bar, int from, int to) {
    int n = 0;
    for (int s = from; s < to; s++) n += st[bar * SEQ_STEPS + s].on;
    return n;
}

static int bar_last_on(const seq_step_t *st, int bar) {
    for (int s = SEQ_STEPS - 1; s >= 0; s--) if (st[bar * SEQ_STEPS + s].on) return bar * SEQ_STEPS + s;
    return -1;
}

/* VST-12 phrasing: each type's defaults, Loop, the breath lengths, Q & A, the pickup, the names */
static void test_phrasing(void) {
    static const unsigned char DEF[MEL_NTYPES][MP_COUNT] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {2, 2, 1, 0}, {1, 2, 2, 0}, {3, 2, 2, 0},
                                                           {0, 0, 1, 0}, {1, 1, 1, 1}};
    static tcase_t t;
    static seq_step_t st[SEQ_LANE_STEPS];
    int ok = 1;
    for (int ty = 0; ty < MEL_NTYPES; ty++) {
        mel_recipe_t d;
        memset(&d, 0, sizeof d);
        mel_defaults(ty, &d, NULL);
        ok &= !memcmp(d.ph, DEF[ty], MP_COUNT);
    }
    CHECK(ok, "each type's phrasing: Arp, Ostinato Loop; Guide Loop Land; Pentatonic 2 + 2 Beat Land; Hook 1 + 1 Beat Q&A; "
              "Sequence 3 + 1 Beat Q&A; Approach 1 + 1 1/8 Land Pickup");
    ok = 1;
    for (int b = 0; b < 5; b++) {   /* the Arp as a Loop: every 8th of every bar, any loop length */
        tc_init(&t, MEL_ARP, BARS[b], 16);
        mel_defaults(MEL_ARP, &t.r, NULL);
        gen(&t, st);
        for (int bar = 0; bar < BARS[b]; bar++) ok &= onsets_in(st, bar, 0, SEQ_STEPS) == 8;
    }
    CHECK(ok, "Loop: the Arp plays every 8th of every bar, at every loop length");
    ok = 1;
    for (int br = 0; br < MEL_NBREATHS; br++) {   /* Arp 1 + 1: the call bars rest for the breath, the answers don't */
        static const int REST[MEL_NBREATHS] = {0, 2, 4, 8};
        tc_init(&t, MEL_ARP, 4, 16);
        t.r.ph[MP_FORM] = MEL_FORM_1_1, t.r.ph[MP_BREATH] = (unsigned char)br;
        gen(&t, st);
        for (int bar = 0; bar < 4; bar++)
            ok &= onsets_in(st, bar, 0, SEQ_STEPS) == (bar % 2 ? 8 : 8 - REST[br] / 2) &&
                  onsets_in(st, bar, SEQ_STEPS - REST[br], SEQ_STEPS) == (bar % 2 ? REST[br] / 2 : 0);
    }
    CHECK(ok, "Breath None, 1/8, Beat, 1/2 bar: the call bars rest for 0, 2, 4, 8 16ths at their end, the answers play on");
    ok = 1;
    for (int f = MEL_FORM_1_1; f < MEL_NFORMS; f++) {   /* Q & A: each call ends on its chord's 5th, the answers on E */
        static const unsigned char END[MEL_NFORMS][4] = {{0}, {1, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        static const unsigned char ANS_END[MEL_NFORMS][4] = {{0}, {0, 1, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        for (int ty = 0; ty < MEL_NTYPES; ty++) {
            tc_init(&t, ty, 4, 16);
            t.r.ph[MP_FORM] = (unsigned char)f, t.r.ph[MP_CADENCE] = MEL_CAD_QA, t.r.ph[MP_FILL] = MEL_FILL_REST;
            gen(&t, st);
            for (int bar = 0; bar < 4; bar++) {
                int i = bar_last_on(st, bar);
                if (i < 0) continue;
                if (END[f][bar]) ok &= st[i].abs % 12 == t.ch[bar].tones[2];
                if (ANS_END[f][bar]) ok &= st[i].abs % 12 == 4;
            }
        }
    }
    CHECK(ok, "Q & A: at every Form and type, each call ends on its chord's 5th and each answer's end on the tonic");
    ok = 1;
    for (int f = MEL_FORM_1_1; f < MEL_NFORMS; f++) {   /* Pickup: on the breath's 16th 15, a step below the next bar's first */
        static const unsigned char END[MEL_NFORMS][4] = {{0}, {1, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        tc_init(&t, MEL_APPR, 4, 16);
        t.r.ph[MP_FORM] = (unsigned char)f, t.r.ph[MP_BREATH] = MEL_BREATH_BEAT, t.r.ph[MP_CADENCE] = MEL_CAD_NONE;
        t.r.ph[MP_FILL] = MEL_FILL_PICKUP;
        gen(&t, st);
        for (int bar = 0; bar < 4; bar++) {
            if (!END[f][bar]) continue;
            const seq_step_t *pk = &st[bar * SEQ_STEPS + 14];
            int nb = (bar + 1) % 4 * SEQ_STEPS, first = nb;
            while (!st[first].on) first++;
            ok &= pk->on && onsets_in(st, bar, 12, SEQ_STEPS) == 1 && in_scale(4, SCALE_MINOR, pk->abs) &&
                  pk->abs < st[first].abs && st[first].abs - pk->abs <= 2;
        }
    }
    CHECK(ok, "Pickup: the breath holds one note on its 16th 15, a scale step below the answer's first note");
    tc_init(&t, MEL_APPR, 2, 16);   /* 2 + 2 over 2 bars: the call ends the loop */
    t.r.ph[MP_FORM] = MEL_FORM_2_2, t.r.ph[MP_BREATH] = MEL_BREATH_BEAT, t.r.ph[MP_FILL] = MEL_FILL_PICKUP;
    t.r.ph[MP_CADENCE] = MEL_CAD_LAND;
    gen(&t, st);
    int l = last_on(st, 32);
    CHECK(l >= 0 && l < 28 && st[l].abs % 12 == 4, "a call ending the loop with a Cadence keeps its landing: no pickup there");
    CHECK(mel_phrase_n(MP_FORM) == 4 && mel_phrase_n(MP_FILL) == 2 && mel_phrase_n(MP_COUNT) == 0 &&
          !strcmp(mel_phrase_name(MP_FORM, 3), "3 + 1") && !strcmp(mel_phrase_name(MP_BREATH, 3), "Half") &&
          !strcmp(mel_phrase_name(MP_CADENCE, 2), "Q & A") && !strcmp(mel_phrase_name(MP_CADENCE, 3), "") &&
          ascii(mel_phrase_name(MP_FILL, 1)), "the phrasing names (ASCII, empty out of range)");
}

/* 0.16 NEW BAR on the recipe types: a bar's own variation changes that bar; the others stay, but a pickup leading into it */
static void test_bar_var(void) {
    static tcase_t t;
    static seq_step_t a[SEQ_LANE_STEPS], b[SEQ_LANE_STEPS];
    unsigned char bv[SEQ_MAX_BARS];
    int local = 1, changed = 0, n = 0;
    for (int ty = 0; ty < MEL_NTYPES; ty++)
        for (int f = 0; f < MEL_NFORMS; f++)
            for (int seed = 1; seed <= 12; seed++) {
                tc_init(&t, ty, 4, 16);
                mel_defaults(ty, &t.r, NULL);
                t.r.ph[MP_FORM] = (unsigned char)f;
                t.p.seed = seed * 101;
                gen(&t, a);
                memset(bv, 0, sizeof bv);
                bv[2] = (unsigned char)(1 + seed % 3);
                t.p.bar_var = bv;
                gen(&t, b);
                for (int i = 0; i < 4 * SEQ_STEPS; i++)
                    if (i / SEQ_STEPS != 2 && i != SEQ_STEPS + 14) local &= a[i].on == b[i].on && (!a[i].on || a[i].abs == b[i].abs);
                changed += memcmp(a + 32, b + 32, sizeof a[0] * SEQ_STEPS) != 0;
                n++;
            }
    CHECK(local, "NEW BAR (every type and Form): bar 3's own variation leaves bars 1, 2 and 4 as they were (but a pickup into it)");
    CHECK(changed == n, "and always changes bar 3 itself (%d of %d cases)", changed, n);
}
int main(void) {
    test_names_defaults();
    test_grid();
    test_fuzz();
    test_nesting();
    test_phrase_reuse();
    test_last_tonic();
    test_guide_borrowed();
    test_answer();
    test_moves();
    test_seed_variety();
    test_seed_defaults();
    test_appr_spans();
    test_pentatonic();
    test_len();
    test_legacy_fingerprint();
    test_phrasing();
    test_bar_var();
    printf(fails ? "\nFAILED: %d\n" : "\nPASSED\n", fails);
    return fails ? 1 : 0;
}
