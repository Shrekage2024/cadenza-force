#include "gen.h"
#include "theory.h"
#include "util.h"
#include <string.h>

/* Metric strength of each 16th in a bar: the downbeat, then beat 3, beats 2 and 4, the other 8ths, the 16ths. */
static const int METRIC[SEQ_STEPS] = {16, 1, 4, 1, 8, 1, 4, 1, 12, 1, 4, 1, 8, 1, 4, 1};

/* Longuet-Higgins & Lee levels (lower = stronger): a note before a rest on a stronger level is syncopated. */
static const int LEVEL[SEQ_STEPS] = {0, 4, 3, 4, 2, 4, 3, 4, 1, 4, 3, 4, 2, 4, 3, 4};

/* Onset density in thousandths of a span's 16ths, at complexity 0 and 100 (linear between) and at most `max`.
 * lo == 0 means one onset per 16 steps of the span (rounded up) at complexity 0. */
typedef struct { short lo, hi, max; } gen_density_t;
static const gen_density_t DENSITY[GEN_NSTYLES] = {
    [GEN_CH_SUSTAIN] = {0, 250, 1000}, [GEN_CH_PULSE] = {250, 500, 1000}, [GEN_CH_STABS] = {125, 600, 1000},
    [GEN_BASS_ROOT] = {0, 250, 1000},  [GEN_BASS_PUMP] = {500, 750, 1000}, [GEN_BASS_WALK] = {250, 500, 875},
    [GEN_MEL_HOOK] = {187, 600, 1000}, [GEN_MEL_ARP] = {250, 750, 1000},   [GEN_MEL_ANSWER] = {125, 500, 1000},
};

/* Pushes: a pushed (weak, strong) pair lifts the weak step into the strong step's class and drops the strong step below
 * every class, so the weak note sounds while the stronger position rests (syncopation) until the span is nearly full.
 * Joining the class (rather than topping it) makes syncopation grow steadily as that class fills with complexity.
 * Bar-relative pairs in priority order (a step joins one pair at most): 8ths onto beats 2 and 4, then 16ths onto 8ths.
 * Beat 3 and the downbeat are never pushed. PUSH_PCT: per style, the chance per pair for 8th pairs and 16th pairs. */
#define NPUSH 6
static const signed char PUSH[NPUSH][2] = {{2, 4}, {10, 12}, {1, 2}, {5, 6}, {9, 10}, {13, 14}};
static const unsigned char PUSH_PCT[GEN_NSTYLES][2] = {
    [GEN_CH_PULSE] = {0, 25}, [GEN_CH_STABS] = {40, 40}, [GEN_BASS_PUMP] = {0, 40}, [GEN_BASS_WALK] = {0, 40},
    [GEN_MEL_HOOK] = {25, 35}, [GEN_MEL_ARP] = {0, 25},   [GEN_MEL_ANSWER] = {25, 35},
};

static const unsigned char WALK_CHORD[3] = {TONE_R, TONE_3, TONE_5};

/* Independent per-step random streams: each decision hashes (seed, step, its own salt). */
enum {
    SALT_WEIGHT = 1, SALT_PUSH, SALT_VEL, SALT_TONE, SALT_SIDE, SALT_ANTIC, SALT_PASS, SALT_PDIR, SALT_PROG, SALT_VARY,
    SALT_MPITCH, SALT_NCT, SALT_CONTOUR, SALT_ATYPE, SALT_ARP, SALT_GAP,
    SALT_GHOST, SALT_CVEL, SALT_BORROW,   /* 0.12: chord rhythms, progression v2 */
    SALT_CVAR                            /* 0.13: a chord rhythm's template variant */
};

#define MEL_MAXC 24   /* melodies: scale notes in the widest range (25 semitones) */
#define GEN_MEMO 8      /* spans pick_onsets remembers, so a span the motif repeats copies its order (memo_find) */
#define GEN_MEMO_LEN 32
/* A candidate note: its tone, octave and pitch; whether it is R, 3 or 5; and the centre's pull on it (2 * range + 2 less
 * its distance from the centre) */
typedef struct { unsigned char tone; signed char oct; short pitch; unsigned char ct, pull; } mel_cand_t;

typedef struct {
    const gen_params_t *p;
    int style, c, n, cl, nspans;      /* complexity clamped to 0..100; loop length, span length, spans in the loop */
    int cl_sh, pl, pl_sh, np;         /* log2 cl (-1: not a power of 2); the phrase length, its log2, phrases in the loop */
    short sdeg[SEQ_LANE_STEPS];       /* each span's chord (span_deg) */
    int key, scale;
    unsigned w[SEQ_LANE_STEPS];       /* each step's weight: who gets an onset first */
    short mi[SEQ_LANE_STEPS];         /* each step's motif index: what its rhythm, velocity and tone randoms hash */
    unsigned char pre[SEQ_LANE_STEPS];   /* the current span's complexity-0 onsets */
    short ord[SEQ_LANE_STEPS];        /* the current span's steps, strongest first (the outranks order) */
    short rank[SEQ_LANE_STEPS];       /* a step's place in that order among the span's steps outside the prefix */
    short kc[101];                    /* span_k at each complexity for spans of kc_len steps (Root's octaves); 0: none */
    int kc_len;
    short memo_ord[GEN_MEMO][GEN_MEMO_LEN];   /* a remembered span's order, relative to its start */
    int memo_a[GEN_MEMO], memo_len[GEN_MEMO], nmemo, memo_next;
    unsigned char elig[SEQ_LANE_STEPS];   /* steps the onset order may take (lead_mask); the rest join from c 50 */
    unsigned char held[SEQ_LANE_STEPS];   /* an Answer's lead sounds here (onset or held note) */
    short pit[SEQ_LANE_STEPS];        /* melodies: each onset's pitch (MIDI), for voice-leading and answers */
    short nxt[SEQ_LANE_STEPS];        /* melodies: the next onset after each step, n if none */
    mel_cand_t mc[7][MEL_MAXC];       /* melodies: the scale notes in range above each degree's root, ascending */
    int nmc[7], contour, centre;      /* how many; the call contour (per seed); the centre pitch */
    int lo;                           /* the lowest pitch in range */
    unsigned ctbits[7];               /* each degree's chord tones in range: bit pitch - lo + 2 (ct_near) */
    unsigned char cti[7][MEL_MAXC];   /* each degree's chord tones, as indices into mc */
    int ncti[7], row;                 /* how many; the PROX row */
} gen_ctx_t;

typedef struct { int i, n, last, e, span; } gen_onset_t;   /* onset at step i, the n-th in its span ending at e */

void gen_seed(gen_rng *r, unsigned seed) { r->s = seed ? seed : 0x9E3779B9u; }

unsigned gen_next(gen_rng *r) {
    unsigned x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return r->s = x;
}

static unsigned mix(int seed, int i, unsigned salt) {
    return fmix32((unsigned)seed * 0x9E3779B1u + (unsigned)i * 0x85EBCA6Bu + salt * 0xC2B2AE35u);
}

/* Depends on the seed, the step and the decision only, never on complexity, so raising it only moves thresholds. */
static unsigned hash(const gen_ctx_t *g, int i, unsigned salt) { return mix(g->p->seed, i, salt); }

/* The same for a step's motif: steps at the same place in the cell share it (see motif_init). */
static unsigned mhash(const gen_ctx_t *g, int i, unsigned salt) { return mix(g->p->seed, g->mi[i], salt); }

/* Motif and development: rhythm comes from a cell of max(16, chord_len) steps (clipped to the loop) that repeats, so
 * bars repeat. A step's motif index is its place in the cell, except at phrase ends: the last bar of each 4-bar group
 * (of a 2-bar loop: bar 2) uses the seed's variant v (1..3; 0, no variation, for a quarter of seeds) as cell * v + place.
 * Spans stay independent: a step's index depends only on the layout and the seed. The walk's approach side and
 * anticipation still hash the step itself, so they differ span by span. 0.16: a bar with its own variation n (NEW BAR) takes
 * cell * (3 + n) + place instead, a cell no other bar uses. */
static void motif_init(gen_ctx_t *g) {
    int cell = g->cl > SEQ_STEPS ? g->cl : SEQ_STEPS, bars = g->n / SEQ_STEPS;
    int group = bars >= 4 ? 4 : bars >= 2 ? 2 : 0, v = (int)(mix(g->p->seed, 0, SALT_VARY) % 4u);
    if (cell > g->n) cell = g->n;
    for (int bar = 0, pos = 0; bar < bars; bar++) {
        int base = group && (bar & (group - 1)) == group - 1 ? v * cell : 0;   /* group is 2 or 4 */
        if (g->p->bar_var && g->p->bar_var[bar]) base = (3 + g->p->bar_var[bar]) * cell;   /* 0.16: the bar's own (v <= 3) */
        for (int j = 0; j < SEQ_STEPS; j++, pos = pos + 1 == cell ? 0 : pos + 1)
            g->mi[bar * SEQ_STEPS + j] = (short)(base + pos);
    }
}

/* i % cl, without a division when cl is a power of 2 (always, from the sequencer): armv7 divides in software. */
static int span_pos(const gen_ctx_t *g, int i) { return g->cl & (g->cl - 1) ? i % g->cl : i & (g->cl - 1); }

/* i / cl and i / the phrase length (i >= 0), the same way */
static int span_of(const gen_ctx_t *g, int i) { return g->cl_sh < 0 ? i / g->cl : i >> g->cl_sh; }
static int phrase_of(const gen_ctx_t *g, int i) { return g->pl_sh < 0 ? i / g->pl : i >> g->pl_sh; }

static int pushable(const gen_ctx_t *g, int p, int q) {
    int sp = span_pos(g, p), sq = span_pos(g, q);
    return q < g->n && p - sp == q - sq && sp && sq;
}

static void apply_pushes(gen_ctx_t *g) {
    for (int bar = 0; bar < g->n; bar += SEQ_STEPS) {
        unsigned used = 0;   /* bar-relative steps already in a pushed pair */
        for (int k = 0; k < NPUSH; k++) {
            int p = bar + PUSH[k][0], q = bar + PUSH[k][1];
            unsigned bits = 1u << PUSH[k][0] | 1u << PUSH[k][1], pct = PUSH_PCT[g->style][PUSH[k][0] % 2];
            if ((used & bits) || !pushable(g, p, q) || mhash(g, p, SALT_PUSH) % 100u >= pct) continue;
            g->w[p] = (unsigned)METRIC[q % SEQ_STEPS] * 1000u + g->w[p] % 1000u;
            g->w[q] %= 1000u;
            used |= bits;
        }
    }
}

/* The Hook breathes: the last beat of every phrase (a phrase is a chord span, or a bar under shorter chords) ranks
 * below every other step, so phrases keep one rhythm; in a call phrase (phrases pair as call and answer, a lone last
 * one answers) it is a rest until c 50. Chord changes are never part of it. */
static void hook_breath(gen_ctx_t *g) {
    int pl = g->pl, np = g->np;
    for (int ph = 0; ph < np; ph++) {
        int pe = (ph + 1) * pl < g->n ? (ph + 1) * pl : g->n, call = ph % 2 == 0 && ph + 1 < np;
        for (int i = pe - 4; i < pe; i++)
            if (span_pos(g, i)) g->w[i] %= 1000u, g->elig[i] = !call;
    }
}

/* An Answer's starts in the lead's gap [a, e): every other step from a seed-chosen phase, leaving 2+ free steps in the
 * chord (not the gap's or the chord's last step), so its notes have room and the seed decides where they fall. The
 * phase is the seed's only where both phases fit as many notes. Fixed for a seed and lead, so the onsets nest. */
static void answer_gap(gen_ctx_t *g, int a, int e) {
    int phase = (e - a) % 2 && e - a > 2 ? (int)(mix(g->p->seed, a, SALT_GAP) % 2u) : 0;
    for (int i = a + phase; i + 1 < e; i += 2)
        if (span_pos(g, i + 1) && !(i > a && g->elig[i - 1]) && !g->elig[i + 1]) g->elig[i] = 1;
}

/* A phrase the lead leaves no start in (an Arp, v1's every-other-step Manual) is not left silent: its chord change, a
 * chord change at every complexity, becomes a start (one chord tone with the lead; the pitch weights still keep it off
 * the lead's pitch class). Fixed for a seed and lead, so the onsets nest. */
static void answer_fallback(gen_ctx_t *g) {
    for (int ps = 0; ps < g->n; ps += g->pl) {
        int any = 0;
        for (int i = ps; i < ps + g->pl && i < g->n; i++) any |= g->elig[i];
        if (!any) g->elig[ps] = 1;
    }
}

/* Where a melody may start notes at every complexity (more join from c 50, late_onsets; a fixed set, so the onsets
 * nest): a Hook skips its call breaths; an Answer with a lead starts on a chord change its lead leaves free, or in the
 * lead's gaps (no onset, no note held) as answer_gap places them. Every other style takes every step. */
static void lead_mask(gen_ctx_t *g) {
    memset(g->elig, 1, sizeof g->elig);
    memset(g->held, 0, sizeof g->held);
    if (g->style == GEN_MEL_HOOK) hook_breath(g);
    if (g->style != GEN_MEL_ANSWER || !g->p->lead) return;
    for (int i = 0; i < g->n; i++) {
        const seq_step_t *x = &g->p->lead[i];
        for (int j = i; x->on && j < i + (x->len < 1 ? 1 : x->len) && j < g->n; j++) g->held[j] = 1;
    }
    for (int i = 0; i < g->n; i++) g->elig[i] = !g->held[i] && span_pos(g, i) == 0;
    for (int a = 0, e = 0; a < g->n; a = e + 1) {
        while (a < g->n && g->held[a]) a++;
        for (e = a; e < g->n && !g->held[e]; e++) {}
        answer_gap(g, a, e);
    }
    answer_fallback(g);
}

static int log2_or(int v) {   /* log2 v for a power of 2, else -1 */
    int k = 0;
    while (1 << k < v) k++;
    return 1 << k == v ? k : -1;
}

/* The spans and phrases of the loop, and each span's chord, once per lane: the per-onset work then neither divides
 * (armv7 divides in software) nor looks the chord up through the progression's length. */
static void layout_init(gen_ctx_t *g) {
    const gen_params_t *p = g->p;
    g->nspans = (g->n + g->cl - 1) / g->cl;
    g->cl_sh = log2_or(g->cl);
    g->pl = g->cl < SEQ_STEPS ? SEQ_STEPS : g->cl;
    g->pl_sh = log2_or(g->pl);
    g->np = (g->n + g->pl - 1) / g->pl;
    for (int span = 0; span < g->nspans; span++)
        g->sdeg[span] = (short)(!p->prog || p->prog_len < 1 ? 0 : p->prog[span % p->prog_len]);
}

/* The params, clamped, and the layout: all gen_transpose_v1 needs */
static void ctx_layout(gen_ctx_t *g, const gen_params_t *p) {
    g->p = p;
    g->style = p->style;
    g->c = clampi(p->complexity, 0, 100);
    g->n = clampi(p->bars, 1, SEQ_MAX_BARS) * SEQ_STEPS;
    g->cl = p->chord_len < 1 || p->chord_len > g->n ? g->n : p->chord_len;
    layout_init(g);
    g->key = pmod(p->key, 12);
    g->scale = p->scale < 0 || p->scale >= SCALE_COUNT ? 0 : p->scale;
}

/* Weight: METRIC * 1000 plus the motif's random 0..999 (ties within a class), +100000 on a chord change, then the pushes.
 * A step whose motif index names an earlier step carrying that index copies its random instead of hashing again. */
static void ctx_init(gen_ctx_t *g, const gen_params_t *p) {
    ctx_layout(g, p);
    g->kc_len = 0;
    g->nmemo = g->memo_next = 0;
    motif_init(g);
    for (int i = 0; i < g->n; i++) {
        int m = g->mi[i];
        unsigned r = m < i && g->mi[m] == m ? g->w[m] % 1000u : mhash(g, i, SALT_WEIGHT) % 1000u;
        g->w[i] = (unsigned)METRIC[i % SEQ_STEPS] * 1000u + r + (span_pos(g, i) ? 0u : 100000u);
    }
    apply_pushes(g);
    lead_mask(g);
}

/* The scale degree of a span's chord; the span after the last is the loop's first again. */
static int span_deg(const gen_ctx_t *g, int span) { return g->sdeg[span >= g->nspans ? 0 : span]; }

static int note(const gen_ctx_t *g, int deg, int tone, int oct) { return theory_tone_note(g->key, g->scale, deg, tone, oct); }

static int outranks(const gen_ctx_t *g, int j, int i) { return g->w[j] > g->w[i] || (g->w[j] == g->w[i] && j < i); }

/* Onsets in a span of len steps at complexity c: density * len rounded to nearest (halves up), at least 1, never
 * dropping as c rises. Rounding to nearest spreads the added onsets over the knob instead of adding one at c = 1. */
static int span_k(const gen_ctx_t *g, int len, int c) {
    const gen_density_t *d = &DENSITY[g->style];
    long lo = d->lo ? (long)d->lo * len : 1000L * ((len + 15) / 16), hi = (long)d->hi * len;   /* onsets x 1000 */
    long k = (lo * (100 - c) + hi * c + 50000) / 100000, cap = (long)d->max * len / 1000;
    if (k > cap) k = cap;
    return k < 1 ? 1 : (int)k;
}

/* Would an onset at p sound before a rest on a stronger level (with the span's complexity-0 onsets so far)? An Answer
 * with a lead plays in the lead's gaps, wherever they fall, and holds its notes to the gap's end: no guard. */
static int syncopates(const gen_ctx_t *g, int p, int e) {
    if (g->style == GEN_MEL_ANSWER && g->p->lead) return 0;
    for (int j = p + 1; j < e && !g->pre[j]; j++)
        if (LEVEL[j % SEQ_STEPS] < LEVEL[p % SEQ_STEPS]) return 1;
    return 0;
}

/* The span's steps in the outranks order, by insertion sort (a span is at most a loop): one sort serves the prefix
 * and the ranks, where asking each step how many outrank it cost a pass over the span per step. */
static void sort_span(gen_ctx_t *g, int a, int e) {
    for (int i = a; i < e; i++) {
        int j = i - a;
        for (; j > 0 && outranks(g, i, g->ord[j - 1]); j--) g->ord[j] = g->ord[j - 1];
        g->ord[j] = (short)i;
    }
}

/* The complexity-0 onsets: the chord change, then the best by weight that don't syncopate, so c = 0 is never
 * syncopated. Adding a note never makes an earlier one syncopate, and the strongest free level always qualifies.
 * Each pick is the first qualifying step in the span's order (a step passed over may qualify after a later pick). */
static void pick_prefix(gen_ctx_t *g, int a, int e, int k0) {
    for (int i = a; i < e; i++) g->pre[i] = i == a && g->elig[a];
    for (int n = g->pre[a]; n < k0; n++) {   /* an Answer's chord change may be its lead's: then k0 picks from the rest */
        int r = 0;
        while (r < e - a && (g->pre[g->ord[r]] || !g->elig[g->ord[r]] || syncopates(g, g->ord[r], e))) r++;
        if (r == e - a) return;
        g->pre[g->ord[r]] = 1;
    }
}

/* Above c = 70 the walk may add an onset on the span's last 16th that anticipates the next root. It depends on c only
 * through the threshold, so it nests too. */
static int anticipates(const gen_ctx_t *g, int a, int e) {
    return g->style == GEN_BASS_WALK && g->c > 70 && e - 1 > a && hash(g, e - 1, SALT_ANTIC) % 2u;
}

/* A span earlier in the loop whose order, prefix and ranks this one shares: the same length and place in the bar (the
 * syncopation levels), and the same weights and eligibility step for step (all that sort_span and pick_prefix read).
 * The motif repeats, so most spans find one. -1 if none. */
static int memo_find(const gen_ctx_t *g, int a, int len) {
    for (int k = 0; k < g->nmemo; k++) {
        int b = g->memo_a[k];
        if (g->memo_len[k] == len && (a - b) % SEQ_STEPS == 0 && !memcmp(&g->w[a], &g->w[b], sizeof g->w[0] * (size_t)len) &&
            !memcmp(&g->elig[a], &g->elig[b], (size_t)len))
            return k;
    }
    return -1;
}

/* The order, prefix and ranks of remembered span k, moved to the span at a (its pre and rank are still in place). */
static void memo_copy(gen_ctx_t *g, int k, int a, int len) {
    int b = g->memo_a[k];
    for (int r = 0; r < len; r++) g->ord[r] = (short)(g->memo_ord[k][r] + a);
    memcpy(&g->pre[a], &g->pre[b], (size_t)len);
    memcpy(&g->rank[a], &g->rank[b], sizeof g->rank[0] * (size_t)len);
}

/* Sort the span, pick its prefix, rank the rest, and remember it (the oldest of GEN_MEMO goes). */
static void order_span(gen_ctx_t *g, int a, int e, int k0) {
    int f = 0, k = g->memo_next;
    sort_span(g, a, e);
    pick_prefix(g, a, e, k0);
    for (int r = 0; r < e - a; r++)
        if (!g->pre[g->ord[r]] && g->elig[g->ord[r]]) g->rank[g->ord[r]] = (short)f++;
    if (e - a < 8 || e - a > GEN_MEMO_LEN) return;   /* a short span sorts faster than it is found */
    for (int r = 0; r < e - a; r++) g->memo_ord[k][r] = (short)(g->ord[r] - a);
    g->memo_a[k] = a;
    g->memo_len[k] = e - a;
    g->memo_next = (k + 1) % GEN_MEMO;
    if (g->nmemo < GEN_MEMO) g->nmemo++;
}

/* The complexity-0 onsets plus the next k - k0 by weight: a fixed order, so the onsets nest as c rises. */
static void pick_onsets(gen_ctx_t *g, int a, int e, seq_step_t *st) {
    int k0 = span_k(g, e - a, 0), k = span_k(g, e - a, g->c), hit = e - a < 8 ? -1 : memo_find(g, a, e - a);
    if (hit >= 0) memo_copy(g, hit, a, e - a);
    else order_span(g, a, e, k0);
    for (int i = a; i < e; i++) st[i].on = g->pre[i] || (g->elig[i] && g->rank[i] < k - k0);
    if (anticipates(g, a, e)) st[e - 1].on = 1;
}

static int next_onset(const seq_step_t *st, int i, int e) {
    do i++; while (i < e && !st[i].on);
    return i;
}

/* Until the next onset or the span end, at most 16. Stabs stay short (1 step, 2 from c = 50); Pump shortens with c. */
static int onset_len(const gen_ctx_t *g, const seq_step_t *st, int i, int e) {
    int gap = next_onset(st, i, e) - i;
    if (gap > 16) gap = 16;
    if (g->style == GEN_CH_STABS && gap > (g->c < 50 ? 1 : 2)) gap = g->c < 50 ? 1 : 2;
    if (g->style == GEN_BASS_PUMP) gap = gap * (100 - g->c) / 100;
    if (g->style >= GEN_MEL_HOOK) gap = (gap > 4 ? 4 : gap) * (200 - g->c) / 200;   /* melodies: see mel_rhythm */
    return gap < 1 ? 1 : gap;
}

/* A step's metric class after the pushes (from its weight): a pushed step has its new class, a dropped one 0. */
static int step_class(const gen_ctx_t *g, int i) { return (int)(g->w[i] % 100000u / 1000u); }

/* 100, plus an accent on beat-class steps (pushed ones included) and a random spread, both growing with complexity. */
static int onset_vel(const gen_ctx_t *g, int i) {
    int accent = step_class(g, i) >= 8 ? 15 * g->c / 100 : 0;
    int spread = ((int)(mhash(g, i, SALT_VEL) % 41u) - 20) * g->c / 100;
    return clampi(100 + accent + spread, 1, 127);
}

/* The octave (-2..0) that puts `tone` above `deg` nearest to the pitch `target`. */
static int nearest_oct(const gen_ctx_t *g, int deg, int tone, int target) {
    int best = -2;
    for (int o = -1; o <= 0; o++) {
        int d = note(g, deg, tone, o) - target, db = note(g, deg, tone, best) - target;
        if (d * d < db * db) best = o;
    }
    return best;
}

/* The walk's last onset (not the first): the scale step below or above the next chord's root, within 2 semitones of
 * that root as the bass plays it (octave -1); on the span's last 16th above c = 70, the next root itself. */
static void walk_edge(const gen_ctx_t *g, const gen_onset_t *o, seq_step_t *s) {
    int cur = span_deg(g, o->span), next = span_deg(g, o->span + 1), d = next - cur;
    int side = hash(g, o->i, SALT_SIDE) % 2u ? 1 : -1;
    int tone = o->i == o->e - 1 && g->c > 70 ? pmod(d, 7) + 1 : pmod(d + side, 7) + 1;
    s->tone = (unsigned char)tone;
    s->oct = (signed char)nearest_oct(g, cur, tone, note(g, next, TONE_R, -1));
}

/* span_k at every complexity for spans of len steps, for entry_c: once per span length, not per onset and c. */
static void fill_kc(gen_ctx_t *g, int len) {
    for (int c = 0; c <= 100; c++) g->kc[c] = (short)span_k(g, len, c);
    g->kc_len = len;
}

/* The lowest complexity at which step i of the current span sounds: 0 for the complexity-0 onsets, else the first c
 * whose onset count reaches its place in the fixed order. A property of the step alone, so it never changes with c.
 * Needs the span's kc (fill_kc). */
static int entry_c(const gen_ctx_t *g, int i) {
    if (g->pre[i]) return 0;
    int need = g->kc[0] + g->rank[i] + 1, c = 0;
    while (c < 100 && g->kc[c] < need) c++;
    return c;
}

static void onset_tone(const gen_ctx_t *g, const gen_onset_t *o, seq_step_t *s) {
    s->tone = TONE_R;
    s->oct = -1;
    switch (g->style) {
    case GEN_BASS_ROOT:   /* the roots that only sound above c = 60 jump up an octave, at every c */
        s->oct = entry_c(g, o->i) > 60 ? 0 : -1;
        if (g->p->bar_var && g->p->bar_var[o->i / SEQ_STEPS]) {   /* 0.16: a bar of its own: up an octave, or the 5th */
            if (g->p->bar_var[o->i / SEQ_STEPS] % 2) s->oct = (signed char)(s->oct + 1);
            else s->tone = TONE_5;
        }
        break;
    case GEN_BASS_PUMP: s->oct = o->i % 4 ? 0 : -1; break;               /* low on beats, high on the & and 16ths */
    case GEN_BASS_WALK:
        if (o->last && o->n > 0) walk_edge(g, o, s);
        else if (o->n > 0) s->tone = WALK_CHORD[mhash(g, o->i, SALT_TONE) % 3u];
        break;
    default: s->oct = 0; break;                                          /* chords: the voicing supplies the notes */
    }
}

/* Above c = 50 a middle walk note on a weak position may become a passing tone (2/4/6, a step from its chord tone),
 * by its own random against a threshold rising with c, when the next note is a step (1-2 semitones) away.
 * Runs backwards so the next note is final; a step's tone is only ever its chord tone or its passing tone. */
static void walk_passing(const gen_ctx_t *g, int span, int a, int e, seq_step_t *st) {
    int deg = span_deg(g, span), pct = g->c > 50 ? g->c - 50 : 0;
    for (int i = e - 1, next = e; i > a; i--) {
        if (!st[i].on) continue;
        int ct = st[i].tone, pt = ct == TONE_R ? TONE_2 : ct + (mhash(g, i, SALT_PDIR) % 2u ? 1 : -1);
        if (next < e && METRIC[i % SEQ_STEPS] <= 4 && mhash(g, i, SALT_PASS) % 100u < (unsigned)pct) {
            int d = note(g, deg, pt, -1) - note(g, deg, st[next].tone, st[next].oct);
            if (d && d >= -2 && d <= 2) st[i].tone = (unsigned char)pt;
        }
        next = i;
    }
}

static void gen_span(gen_ctx_t *g, int span, int a, int e, seq_step_t *st) {
    for (int i = a; i < e; i++) st[i] = (seq_step_t){0, TONE_R, g->style >= GEN_BASS_ROOT ? -1 : 0, 100, 1, 0};
    pick_onsets(g, a, e, st);
    if (g->style == GEN_BASS_ROOT && g->kc_len != e - a) fill_kc(g, e - a);
    for (int i = a, n = 0; i < e; i = next_onset(st, i, e), n++) {
        gen_onset_t o = {i, n, next_onset(st, i, e) == e, e, span};
        st[i].len = (unsigned char)onset_len(g, st, i, e);
        st[i].vel = (unsigned char)onset_vel(g, i);
        onset_tone(g, &o, &st[i]);
    }
    if (g->style == GEN_BASS_WALK) walk_passing(g, span, a, e, st);
}

/* ---- Melodies: Hook, Arp, Answer ----
 * Rhythm comes from the same weights, pushes and motif cell as the other styles (an Answer's from the steps its lead
 * leaves free); then one pass picks every onset's pitch from the notes before it. Phrases are chord spans (bars under
 * 1-bar chords), paired as call and answer; a lone last phrase answers. */

/* Proximity to the previous pitch, 64 * exp(-d^2 / 2 sigma^2) for d = 0..24 semitones, one row per sigma 2.5, 3.1,
 * 3.75, 4.4, 5; row c*c/2500: 2.5 below c 50, 3.1 for c 50..70, 3.75 for 71..86, 4.4 for 87..99, 5 at 100. Melodies
 * move mostly by small intervals (Huron 2006; von Hippel & Huron 2000; Temperley 2007). */
static const unsigned char PROX[5][25] = {
    {64, 59, 46, 31, 18, 9, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {64, 61, 52, 40, 28, 18, 10, 5, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {64, 62, 56, 46, 36, 26, 18, 11, 7, 4, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {64, 62, 58, 51, 42, 33, 25, 18, 12, 8, 5, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {64, 63, 59, 53, 46, 39, 31, 24, 18, 13, 9, 6, 4, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
};
#define MEL_REVERSAL 16  /* after a leap of 5+ semitones, turning back: gap fill (Narmour; Schellenberg 1997) */
#define MEL_CONTOUR 6     /* toward the call's contour */
#define MEL_BLEND 6       /* the proximity origin: 6/8 of the way from the previous note to the contour */
#define MEL_DOUBLE 32     /* an Answer note doubling its sounding lead's pitch class: w / 32 + 1 */
#define MEL_CONTRARY 6    /* an Answer from c 50, against its lead's direction */
typedef struct {
    int prev, prev2, nct, first;          /* the last two pitches and the loop's first (-1: none yet); prev was an NCT */
    int lead_prev, lead_dir, lead_at, lead_until;   /* the lead's latest pitch, its direction, scan position; sounding
                                                     * until this step */
    int lead_ref;                         /* the lead's pitch at the previous onset (contrary motion) */
    int apos, phrase;                     /* Arp: place in its pattern, counted from the phrase start */
} mel_state_t;
typedef struct {
    int i, deg, ps, first, forced, nct_ok, target, contour;   /* step, chord, phrase start; must it be R/3/5; may it be
                                                                * an NCT; the answer's target (-1: none); contour pitch */
} mel_onset_t;

static int is_mel(int style) { return style >= GEN_MEL_HOOK && style <= GEN_MEL_ANSWER; }
static int is_ct(int tone) { return tone == TONE_R || tone == TONE_3 || tone == TONE_5; }

/* Melody 2 (an Answer, or any melody style on lane 4) sits an octave above Melody 1. */
static int mel_upper(const gen_ctx_t *g) { return g->style == GEN_MEL_ANSWER || g->p->lane == LANE_MEL_DOWN; }
static int mel_centre(const gen_ctx_t *g) { return g->centre; }
static int mel_base_oct(const gen_ctx_t *g) { return mel_upper(g) ? 2 : 1; }

/* The centre in `key`: its tonic nearest G4 (E4 in E, C5 in C; 61..72), an octave up on Melody 2. */
static int centre_in(const gen_ctx_t *g, int key) {
    return 67 + pmod(theory_degree_pc(key, g->scale, 0) - 7 + 6, 12) - 6 + (mel_upper(g) ? 12 : 0);
}

static int tonic_centre(const gen_ctx_t *g) { return centre_in(g, g->key); }

/* Pitches stay within centre +- range: 4 semitones at c 0 to 12 at 100; an Arp spans an octave at 0, two at 100. */
static int mel_range(const gen_ctx_t *g) { return g->style == GEN_MEL_ARP ? 6 + 6 * g->c / 100 : 4 + 8 * g->c / 100; }

/* The complexity where the steps lead_mask held back open: c 50 for an Answer; c 75 for a Hook's breaths, so the Hook
 * still leaves its Answer room at the sweet spot (35-65). */
static int mel_open(const gen_ctx_t *g) { return g->style == GEN_MEL_HOOK ? 75 : 50; }

/* strong: a beat on the grid, or a step pushed into the beat class */
static int mel_strong(const gen_ctx_t *g, int i) { return METRIC[i % SEQ_STEPS] >= 8 || step_class(g, i) >= 8; }

/* A degree's chord tones (bits for ct_near, indices for hook_note) and each candidate's pull, from its candidates. */
static void mel_cache_cts(gen_ctx_t *g, int deg, int lo, int room) {
    g->ctbits[deg] = 0;
    g->ncti[deg] = 0;
    for (int k = 0; k < g->nmc[deg]; k++) {
        mel_cand_t *x = &g->mc[deg][k];
        x->ct = (unsigned char)is_ct(x->tone);
        x->pull = (unsigned char)(room - iabs(x->pitch - mel_centre(g)));
        if (!x->ct) continue;
        g->ctbits[deg] |= 1u << (x->pitch - lo + 2);
        g->cti[deg][g->ncti[deg]++] = (unsigned char)k;
    }
}

/* Every degree's scale notes in range, ascending, once per lane (a pitch is linear in the octave). */
static void mel_cache(gen_ctx_t *g) {
    g->centre = tonic_centre(g);
    int lo = mel_centre(g) - mel_range(g), hi = mel_centre(g) + mel_range(g), pc[14];
    for (int d = 0; d < 14; d++) pc[d] = theory_degree_pc(g->key, g->scale, d);
    for (int deg = 0; deg < 7; deg++) {
        int at0[7], n = 0;
        for (int t = 0; t < 7; t++) at0[t] = 48 + pc[deg] + pmod(pc[deg + t] - pc[deg], 12);   /* note(g, deg, t + 1, 0) */
        for (int oct = -1; oct <= 4; oct++)
            for (int t = 0; t < 7 && n < MEL_MAXC; t++) {
                int p = at0[t] + 12 * oct;
                if (p >= lo && p <= hi) g->mc[deg][n++] = (mel_cand_t){(unsigned char)(t + 1), (signed char)oct, (short)p, 0, 0};
            }
        g->nmc[deg] = n;
        mel_cache_cts(g, deg, lo, 2 * mel_range(g) + 2);
    }
    g->lo = lo;
    g->row = g->c * g->c / 2500;
    g->contour = (int)(mix(g->p->seed, 0, SALT_CONTOUR) % 4u);
}

/* The scale notes of chord `deg` in range, ascending. */
static const mel_cand_t *mel_cands(const gen_ctx_t *g, int deg, int *n) {
    *n = g->nmc[pmod(deg, 7)];
    return g->mc[pmod(deg, 7)];
}

/* The pitch of chord `deg`'s R, 3 or 5 in range nearest to p, within `near` semitones (0: any distance) but not p
 * itself when `near` is set; -1 if none. */
static int nearest_ct(const gen_ctx_t *g, int deg, int p, int near) {
    int n, best = -1;
    const mel_cand_t *c = mel_cands(g, deg, &n);
    for (int k = 0; k < n; k++) {
        int d = iabs(c[k].pitch - p);
        if (!is_ct(c[k].tone) || (near && (d == 0 || d > near))) continue;
        if (best < 0 || d < iabs(best - p)) best = c[k].pitch;
    }
    return best;
}

/* nearest_ct(g, deg, p, 2) >= 0 for an in-range pitch p: a chord tone of `deg` 1 or 2 semitones away, from the bits
 * mel_cache keeps (p - 2, p - 1, p + 1, p + 2: 0x1B), not a scan of the candidates. */
static int ct_near(const gen_ctx_t *g, int deg, int p) { return (g->ctbits[pmod(deg, 7)] >> (p - g->lo) & 0x1Bu) != 0; }

/* The Answer's lead: its pitch at its latest onset up to step i, and the direction it last moved (-1, 0, 1). */
static void lead_follow(const gen_ctx_t *g, mel_state_t *m, int i) {
    for (; g->p->lead && m->lead_at <= i; m->lead_at++) {
        const seq_step_t *x = &g->p->lead[m->lead_at];
        if (!x->on) continue;
        int p = x->abs ? x->abs : note(g, span_deg(g, span_of(g, m->lead_at)), x->tone, x->oct);   /* an absolute step (0.10: a FIX pick) */
        if (m->lead_prev >= 0) m->lead_dir = p > m->lead_prev ? 1 : p < m->lead_prev ? -1 : 0;
        m->lead_prev = p;
        m->lead_until = m->lead_at + (x->len < 1 ? 1 : x->len);
    }
}

/* The call's contour, one per seed: arch, rise, fall or wave across the `len` steps it sounds in (the phrase, less a
 * Hook call's breath), within half the range of the centre. It looks an eighth ahead, since each note moves only part
 * of the way toward it. */
static int contour_target(const gen_ctx_t *g, const mel_onset_t *o, int len) {
    int r = mel_range(g), f = (o->i - o->ps) * 64 / len + 8, t;
    f = f > 63 ? 63 : f;
    switch (g->contour) {
    case 0: t = 32 - iabs(f - 32); break;
    case 1: t = f / 2; break;
    case 2: t = 32 - f / 2; break;
    default: t = 2 * (16 - iabs(f % 32 - 16)); break;
    }
    return mel_centre(g) - r / 2 + r * t / 32;
}

/* An answer onset's target from its call (the phrase before), at the same place: echo (the call's pitch), sequence
 * (the call's shape restarted on the answer chord's chord tone nearest the call's first note), mirror (the call's
 * intervals inverted around its first note). Echo always; sequence from c 34, mirror from 67 (pitch only). -1: none. */
static int answer_target(const gen_ctx_t *g, const seq_step_t *st, const mel_onset_t *o) {
    int pl = g->pl, cs = o->ps - pl, j = cs + (o->i - o->ps), f = cs;
    if (cs < 0) return -1;
    while (j >= cs && !st[j].on) j--;
    while (f < o->ps && !st[f].on) f++;
    if (j < cs || f == o->ps) return -1;
    int types = g->c >= 67 ? 3 : g->c >= 34 ? 2 : 1, type = (int)(mix(g->p->seed, phrase_of(g, o->ps), SALT_ATYPE) % (unsigned)types);
    int p = g->pit[j], p0 = g->pit[f];
    if (type == 1) return p + nearest_ct(g, span_deg(g, span_of(g, o->ps)), p0, 0) - p0;
    return type == 2 ? 2 * p0 - p : p;
}

/* What the pitch of onset i must be: R/3/5 on a span's first onset, an answer phrase's last and the loop's last; a
 * non-chord tone only on a weak position, by a chance rising with c (60% to 100%), never right after one, and only when
 * the next note follows within 2 steps of its end (no passing tone left hanging); an Answer's only from c 20. */
static void mel_describe(const gen_ctx_t *g, const seq_step_t *st, const mel_state_t *m, mel_onset_t *o) {
    int pl = g->pl, ph = phrase_of(g, o->i), np = g->np, pe = (ph + 1) * pl < g->n ? (ph + 1) * pl : g->n;
    int answer = ph % 2 || (ph == np - 1 && np % 2);
    o->deg = span_deg(g, span_of(g, o->i));
    o->ps = ph * pl;
    o->forced = o->first || g->nxt[o->i] == g->n || (answer && g->nxt[o->i] >= pe);
    o->nct_ok = !o->forced && !m->nct && !mel_strong(g, o->i) && mhash(g, o->i, SALT_NCT) % 100u < (unsigned)(60 + 40 * g->c / 100);
    o->nct_ok &= g->nxt[o->i] - (o->i + st[o->i].len) <= 2;   /* no dangling: the next note follows within 2 steps */
    if (g->style == GEN_MEL_ANSWER) o->nct_ok &= g->c >= 20;
    o->target = answer && g->style != GEN_MEL_ARP ? answer_target(g, st, o) : -1;
    o->contour = contour_target(g, o, g->style == GEN_MEL_HOOK && !answer && g->c < mel_open(g) ? pl - 4 : pl);
}

/* What every candidate of one onset shares, worked out once per onset rather than once per candidate (hook_note) */
typedef struct {
    int prev, prev2, first, from;  /* the last two pitches and the loop's first (-1: none); the proximity origin */
    int ctmul, nct, after_nct;     /* a chord tone's factor (x6 on a strong position); a non-chord tone may sound here;
                                    * the note before was one */
    int far_nct;                   /* a non-chord tone may be reached by leap (c 75+, or no note before) */
    int nct_deg;                   /* the next onset's chord, where a non-chord tone resolves */
    int target, dbl, last;         /* the answer's target (-1: none); the lead pitch not to double (-1: none); the loop's
                                    * last onset */
    unsigned bias[3];              /* mel_bias's factors but the target's, by the move from prev: down, none, up */
} mel_pick_t;

/* Shape: toward the answer's target, else along the call's contour; turning back after a leap; for an Answer from
 * c 50, against the way its lead moved since the Answer's previous note (contrary motion). All but the target's depend
 * only on the direction of the move from the previous note (none when there is none), so they are worked out per
 * direction: bias[dir + 1]. */
static void mel_bias(const gen_ctx_t *g, const mel_state_t *m, const mel_onset_t *o, mel_pick_t *k) {
    int dl = m->lead_ref >= 0 && m->lead_prev != m->lead_ref ? m->lead_prev - m->lead_ref : m->lead_dir;
    int leap = m->prev2 >= 0 && iabs(m->prev - m->prev2) >= 5 ? m->prev - m->prev2 : 0;
    int against = g->style == GEN_MEL_ANSWER && g->c >= 50 ? dl : 0;
    for (int dir = -1; dir <= 1; dir++) {
        unsigned b = 1;
        if (o->target < 0 && (dir * (o->contour - m->prev) > 0 || (dir == 0 && o->contour == m->prev))) b *= MEL_CONTOUR;
        if (dir * leap < 0) b *= MEL_REVERSAL;
        if (dir * against < 0) b *= MEL_CONTRARY;
        k->bias[dir + 1] = b;
    }
}

static void pick_init(const gen_ctx_t *g, const mel_state_t *m, const mel_onset_t *o, mel_pick_t *k) {
    k->prev = m->prev;
    k->prev2 = m->prev2;
    k->first = m->first;
    k->from = m->prev < 0 ? (o->target >= 0 ? o->target : o->contour)
              : o->target >= 0 ? m->prev : m->prev + (o->contour - m->prev) * MEL_BLEND / 8;
    k->ctmul = mel_strong(g, o->i) ? 6 : 1;
    k->nct = !m->nct && o->nct_ok;
    k->after_nct = m->nct;
    k->far_nct = g->c >= 75 || m->prev < 0;
    k->nct_deg = span_deg(g, span_of(g, g->nxt[o->i]));
    k->target = o->target;
    k->dbl = g->p->lead && m->lead_until > o->i && m->lead_prev >= 0 ? m->lead_prev : -1;
    k->last = g->nxt[o->i] == g->n && m->first >= 0;
    mel_bias(g, m, o, k);
}

/* A candidate may sound: after a non-chord tone only a chord tone a step (1-2 semitones) away; a non-chord tone only
 * where allowed, not repeating the note before, reached by step below c 75 (a passing or neighbour tone), and with a
 * chord tone of the next onset's chord a step away. */
static int cand_allowed(const gen_ctx_t *g, const mel_pick_t *k, const mel_cand_t *x) {
    int d = iabs(x->pitch - k->prev);
    if (k->after_nct) return x->ct && d >= 1 && d <= 2;
    if (x->ct) return 1;
    if (!k->nct || x->pitch == k->prev || (!k->far_nct && d > 2)) return 0;
    return ct_near(g, k->nct_deg, x->pitch);
}

/* Repeats: a unison 3/8 as likely as its proximity says, a third note in a row 1/64. An Answer avoids doubling its
 * sounding lead's pitch class (w / MEL_DOUBLE + 1). The loop's last note leans toward the first, so the loop wraps
 * smoothly. */
static unsigned mel_shape(const mel_pick_t *k, const mel_cand_t *x, unsigned w) {
    if (k->prev >= 0 && x->pitch == k->prev) w = k->prev == k->prev2 ? w / 64 : w * 3 / 8;
    if (k->dbl >= 0 && pmod(x->pitch - k->dbl, 12) == 0) w = w / MEL_DOUBLE + 1;
    if (k->last) {
        int d = iabs(x->pitch - k->first);
        w = (w / 16 + 1) * PROX[0][d > 24 ? 24 : d];   /* scaled so the total stays far below 2^32 */
    }
    return w;
}

/* Proximity to the note before (the first note: to its target or contour); the centre's pull; chord tones x6 on strong
 * positions, and an allowed non-chord tone x4 on a weak one (passing and neighbour tones keep the line stepwise between
 * chord tones); then the bias (the target's by distance, x8 on it, x3 a step away) and the shape. */
static unsigned cand_weight(const gen_ctx_t *g, const mel_pick_t *k, const mel_cand_t *x) {
    if (!cand_allowed(g, k, x)) return 0;
    int d = iabs(x->pitch - k->from), dir = k->prev < 0 ? 0 : (x->pitch > k->prev) - (x->pitch < k->prev);
    unsigned w = PROX[g->row][d > 24 ? 24 : d], b = k->bias[dir + 1];
    w *= x->pull;
    w *= !x->ct ? 4u : (unsigned)k->ctmul;
    if (k->target >= 0) {
        int dt = iabs(x->pitch - k->target);
        b *= dt == 0 ? 8u : dt <= 2 ? 3u : 1u;
    }
    return mel_shape(k, x, w * b);
}

/* Hook and Answer: a weighted pick among the candidates, by the onset's motif random. */
static void hook_note(gen_ctx_t *g, const mel_state_t *m, const mel_onset_t *o, seq_step_t *s) {
    unsigned w[MEL_MAXC], total = 0;
    int n, k = 0;
    mel_pick_t pk;
    const mel_cand_t *c = mel_cands(g, o->deg, &n);
    pick_init(g, m, o, &pk);
    if (pk.nct) {
        for (int j = 0; j < n; j++) total += w[j] = cand_weight(g, &pk, &c[j]);
    } else {   /* every non-chord tone weighs 0 (cand_allowed): weigh the chord tones alone */
        const unsigned char *ct = g->cti[pmod(o->deg, 7)];
        memset(w, 0, sizeof w);
        for (int j = 0; j < g->ncti[pmod(o->deg, 7)]; j++) total += w[ct[j]] = cand_weight(g, &pk, &c[ct[j]]);
    }
    for (int j = 0; !total && j < n; j++) total += w[j] = (unsigned)is_ct(c[j].tone);   /* never: see cand_allowed */
    unsigned r = mhash(g, o->i, SALT_MPITCH) % total;
    while (r >= w[k]) r -= w[k++];
    s->tone = c[k].tone;
    s->oct = c[k].oct;
    g->pit[o->i] = c[k].pitch;
}

/* The pattern's index at place `pos` among n tones: up, down or up-down. */
static int arp_at(int order, int pos, int n) {
    if (order == 0 || n < 2) return pos % n;
    if (order == 1) return n - 1 - pos % n;
    return pos % (2 * n - 2) < n ? pos % (2 * n - 2) : 2 * n - 2 - pos % (2 * n - 2);
}

/* Where the rules want R/3/5 but the pattern is on a 7th (index k): the pattern's next tone, else the nearest of k's
 * neighbours, whichever is R/3/5 and not the note just played. */
static int arp_forced(const mel_cand_t *c, int n, int k, int next, int prev) {
    int opt[5] = {next, k - 1, k + 1, k - 2, k + 2};
    for (int pass = 0; pass < 2; pass++)
        for (int j = 0; j < 5; j++)
            if (opt[j] >= 0 && opt[j] < n && is_ct(c[opt[j]].tone) && (pass || c[opt[j]].pitch != prev)) return opt[j];
    return k;   /* never: a 7th has R above and 5 below it */
}

/* Arp: the chord's R, 3, 5 (and 7 above c 60) in range, one per onset, up, down or up-down by seed, from a place in the
 * pattern chosen by seed at each phrase start (a bar under short chords, so the pattern runs across their changes; the
 * index wraps on each chord's own tones). Where the rules want R/3/5, a 7th gives way (arp_forced) and the pattern goes
 * on from there. */
static void arp_note(gen_ctx_t *g, mel_state_t *m, const mel_onset_t *o, seq_step_t *s) {
    mel_cand_t c[MEL_MAXC];
    int all, n = 0, k, order = (int)(mix(g->p->seed, 0, SALT_ARP) % 3u);
    const mel_cand_t *in = mel_cands(g, o->deg, &all);
    for (k = 0; k < all; k++)
        if (is_ct(in[k].tone) || (in[k].tone == TONE_7 && g->c > 60)) c[n++] = in[k];
    if (m->phrase != o->ps) m->phrase = o->ps, m->apos = (int)(mix(g->p->seed, 1, SALT_ARP) % 6u);
    k = arp_at(order, m->apos++, n);
    if (o->forced && !is_ct(c[k].tone)) k = arp_forced(c, n, k, arp_at(order, m->apos++, n), m->prev);
    s->tone = c[k].tone;
    s->oct = c[k].oct;
    g->pit[o->i] = c[k].pitch;
}

/* From mel_open the steps lead_mask held back join by weight: an Answer's (and an onset on every chord change), a
 * quarter of them by c 100; a Hook's breaths, at the span's own density. A fixed order and a count rising with c, so the onsets
 * nest. */
static void late_onsets(const gen_ctx_t *g, int a, int e, seq_step_t *st) {
    int n = 0, m;
    if (g->c < mel_open(g) || (g->style != GEN_MEL_ANSWER && g->style != GEN_MEL_HOOK)) return;
    for (int i = a; i < e; i++) n += !g->elig[i];
    if (g->style == GEN_MEL_ANSWER) st[a].on = 1, m = (n * (g->c - 50) + 100) / 200;
    else m = (2 * n * span_k(g, e - a, g->c) + e - a) / (2 * (e - a));
    for (int r = 0; r < e - a && m > 0; r++)
        if (!g->elig[g->ord[r]] && !st[g->ord[r]].on) st[g->ord[r]].on = 1, m--;
}

/* Below mel_open: an Answer note fills its gap (to its next note, at most a quarter) and stops where its lead sounds
 * again (a fallback note, which starts with the lead, at the lead's next onset); a Hook note stops before its breath.
 * Lengths only, so the onsets still nest. */
static int mel_len(const gen_ctx_t *g, const seq_step_t *st, int i, int e) {
    int len = onset_len(g, st, i, e), next = next_onset(st, i, e), with = g->held[i];
    if (g->c >= mel_open(g)) return len;
    if (g->style == GEN_MEL_ANSWER && g->p->lead) len = next - i > 4 ? 4 : next - i;
    for (int j = i + 1; j < i + len; j++)
        if ((with ? g->p->lead[j].on : g->held[j]) || (g->style == GEN_MEL_HOOK && !g->elig[j] && span_pos(g, j))) return j - i;
    return len;
}

/* A melody note lasts until the next one, at most a quarter note, and shortens to half that by c 100 (below c 50,
 * mel_len). */
static void mel_rhythm(gen_ctx_t *g, int a, int e, seq_step_t *st) {
    for (int i = a; i < e; i++) st[i] = (seq_step_t){0, TONE_R, (signed char)mel_base_oct(g), 100, 1, 0};
    pick_onsets(g, a, e, st);
    late_onsets(g, a, e, st);
    for (int i = a; i < e; i++) {
        if (!st[i].on) continue;
        st[i].len = (unsigned char)mel_len(g, st, i, e);
        st[i].vel = (unsigned char)onset_vel(g, i);
    }
}

/* The pitches in order before step `stop`: each from the notes before it (and the rhythm after it), never from a later
 * pitch, so a stop leaves the pitches before it as the whole loop has them. */
static void mel_pitches(gen_ctx_t *g, seq_step_t *st, int stop) {
    mel_state_t m = {-1, -1, 0, -1, -1, 0, 0, 0, -1, 0, -1};
    for (int i = g->n - 1, next = g->n; i >= 0; i--) {
        g->nxt[i] = (short)next;
        if (st[i].on) next = i;
    }
    for (int i = 0, span = -1, nth = 0; i < stop; i++) {
        if (!st[i].on) continue;
        nth = span_of(g, i) == span ? nth + 1 : 0;
        span = span_of(g, i);
        mel_onset_t o = {.i = i, .first = nth == 0};
        mel_describe(g, st, &m, &o);
        lead_follow(g, &m, i);
        if (g->style == GEN_MEL_ARP) arp_note(g, &m, &o, &st[i]);
        else hook_note(g, &m, &o, &st[i]);
        if (m.first < 0) m.first = g->pit[i];
        m.lead_ref = m.lead_prev;
        m.nct = g->style != GEN_MEL_ARP && !is_ct(st[i].tone);
        m.prev2 = m.prev;
        m.prev = g->pit[i];
    }
}

/* Was the cache made from these inputs? Every one the rhythm reads: the style, seed, complexity, length, chord length,
 * lane, and an Answer's lead's onsets and lengths (raw, unclamped: a near miss only costs a regeneration). */
static int cache_hit(const gen_cache_t *c, const gen_params_t *p, int n) {
    if (!c->valid || c->style != p->style || c->seed != p->seed || c->complexity != p->complexity || c->bars != p->bars ||
        c->chord_len != p->chord_len || c->lane != p->lane || c->lead != (p->lead != NULL))
        return 0;
    for (int b = 0; b < SEQ_MAX_BARS; b++)
        if (c->bar_var[b] != (p->bar_var ? p->bar_var[b] : 0)) return 0;
    for (int i = 0; p->lead && i < n; i++)
        if (c->lead_on[i] != p->lead[i].on || c->lead_len[i] != p->lead[i].len) return 0;
    return 1;
}

static void cache_store(gen_cache_t *c, const gen_ctx_t *g, const gen_params_t *p, const seq_step_t *st) {
    c->valid = 1;
    c->style = p->style, c->seed = p->seed, c->complexity = p->complexity;
    c->bars = p->bars, c->chord_len = p->chord_len, c->lane = p->lane, c->lead = p->lead != NULL;
    for (int b = 0; b < SEQ_MAX_BARS; b++) c->bar_var[b] = p->bar_var ? p->bar_var[b] : 0;
    for (int i = 0; p->lead && i < g->n; i++) c->lead_on[i] = p->lead[i].on, c->lead_len[i] = p->lead[i].len;
    memcpy(c->w, g->w, sizeof g->w[0] * (size_t)g->n);
    memcpy(c->mi, g->mi, sizeof g->mi[0] * (size_t)g->n);
    memcpy(c->st, st, sizeof st[0] * (size_t)g->n);
}

/* A melody up to its pitches: the rhythm of every span (with what the pitch pass reads of the weights and motif), from
 * the cache when it was made from the same inputs, else made and kept there (c NULL: made); then the key's candidates. */
static void mel_prepare(gen_ctx_t *g, const gen_params_t *p, gen_cache_t *c, seq_step_t *st) {
    ctx_layout(g, p);
    if (c && cache_hit(c, p, g->n)) {
        memcpy(g->w, c->w, sizeof g->w[0] * (size_t)g->n);
        memcpy(g->mi, c->mi, sizeof g->mi[0] * (size_t)g->n);
        memcpy(st, c->st, sizeof st[0] * (size_t)g->n);
    } else {
        ctx_init(g, p);
        for (int a = 0; a < g->n; a += g->cl) mel_rhythm(g, a, a + g->cl < g->n ? a + g->cl : g->n, st);
        if (c) cache_store(c, g, p, st);
    }
    mel_cache(g);
}

/* ---- 0.12 chord rhythms: Offbeat, Charleston, Ballad, Funk, Half-time, Push ----
 * Not gen_v1's: a fixed template per unit (a bar, or a span under 1-bar chords; a 2-bar span is two bar units), hits
 * at unit positions with a velocity, a length and the complexity they start at, plus ghost hits. A unit's ghost
 * candidates (its mask's positions that hold no template hit) are ordered by a hash of the seed and the position, and
 * the first round(c * candidates * ghost_pct / 10000) sound: a fixed order, so the hits nest as c rises. Every unit of
 * a lane plays the same pattern (a truncated last unit its first steps). A note lasts its length, cut at the next hit
 * of its span and at the span's end (the chord change).
 * 0.13: each style has 3 templates, chosen by p->variant (saved with the lane, so a 0.12 lane, which has none, plays
 * variant 0, 0.12's template). A new seed picks it (gen_chords_v2_variant), so GEN and NEW RHYTHM can move the
 * template's hits too. A variant with no hit inside the unit (Offbeat's 6/14 under 1/4-bar chords) plays variant 0.
 * Velocities: the template's +-10 and ghosts 52..68, by the seed and the position. Reads no harmony. */
typedef struct { unsigned char pos, vel, len, from_c; } ch2_hit_t;
typedef struct { unsigned char n, ghost_pct, ghost_len; unsigned short ghost_mask; ch2_hit_t hit[6]; } ch2_style_t;
#define CH2_VARIANTS 3
static const ch2_style_t CH2[GEN_CH2_LAST - GEN_CH2_FIRST + 1][CH2_VARIANTS] = {
    {   /* Offbeat: the &s; ghosts on the 16ths */
        {4, 30, 1, 0xAAAA, {{2, 104, 2, 0}, {6, 100, 2, 0}, {10, 104, 2, 0}, {14, 100, 2, 0}}},
        /* plus a soft 1-step pickup on the last 16th, into the next bar */
        {5, 30, 1, 0xAAAA, {{2, 104, 2, 0}, {6, 100, 2, 0}, {10, 104, 2, 0}, {14, 100, 1, 0}, {15, 88, 1, 0}}},
        /* only the & of 2 and the & of 4, held longer (an 8th and a half, the stab to the bar line); no ghost inside
         * them, so they keep their lengths */
        {2, 30, 1, 0x282A, {{6, 104, 4, 0}, {14, 104, 2, 0}}},
    },
    {   /* Charleston: 1 long, the & of 2 short; ghosts from the & of 2's 16th on (and 1's last 16th), so 1 stays long */
        {2, 30, 1, 0xFF88, {{0, 112, 6, 0}, {6, 100, 2, 0}}},
        /* plus a short answer on beat 4 */
        {3, 30, 1, 0xFF88, {{0, 112, 6, 0}, {6, 100, 2, 0}, {12, 96, 2, 0}}},
        /* the 3-3-2 tresillo: 1 and the a of 1 a dotted 8th each, the & of 2 short; ghosts also on the 16th before the
         * a of 1 and the & of 2 (a half-bar unit then has 3 candidates, so c 100 adds one) */
        {3, 30, 1, 0xFFA4, {{0, 112, 3, 0}, {3, 100, 3, 0}, {6, 104, 2, 0}}},
    },
    {   /* Ballad: 1 held for the bar, 3 from c 40; soft 8th ghosts */
        {2, 25, 2, 0x5555, {{0, 100, 16, 0}, {8, 88, 8, 40}}},
        /* 1 held to beat 4, a soft beat 4 ringing to the bar line */
        {2, 25, 2, 0x5555, {{0, 100, 12, 0}, {12, 84, 4, 0}}},
        /* 1 held, a soft push on the & of 3 ringing to the bar line */
        {2, 25, 2, 0x5555, {{0, 100, 10, 0}, {10, 84, 6, 0}}},
    },
    {   /* Funk: 16th syncopation, all short */
        {6, 40, 1, 0xFFFF, {{0, 112, 1, 0}, {3, 92, 1, 0}, {6, 104, 1, 0}, {10, 100, 1, 0}, {12, 92, 1, 0}, {14, 100, 1, 0}}},
        {5, 40, 1, 0xFFFF, {{0, 112, 1, 0}, {3, 92, 1, 0}, {7, 104, 1, 0}, {10, 100, 1, 0}, {13, 96, 1, 0}}},
        {6, 40, 1, 0xFFFF, {{0, 112, 1, 0}, {2, 88, 1, 0}, {6, 104, 1, 0}, {9, 96, 1, 0}, {12, 100, 1, 0}, {14, 92, 1, 0}}},
    },
    {   /* Half-time: 1 and 3, long; 8th ghosts */
        {2, 30, 2, 0x5555, {{0, 108, 8, 0}, {8, 100, 8, 0}}},
        /* plus a short, softer & of 4 */
        {3, 30, 2, 0x5555, {{0, 108, 8, 0}, {8, 100, 6, 0}, {14, 92, 1, 0}}},
        /* a short pickup on the & of 2 into a long 3 */
        {3, 30, 2, 0x5555, {{0, 108, 6, 0}, {6, 92, 2, 0}, {8, 104, 8, 0}}},
    },
    {   /* Push: 1, an accented stab on the & of 4 that ends at the bar line, the & of 2 from c 50; no ghost on the last
         * 16th, so the stab keeps its length */
        {3, 30, 1, 0x7FFF, {{0, 104, 6, 0}, {6, 96, 2, 50}, {14, 112, 2, 0}}},
        /* the & of 3 and the & of 4 both pushed */
        {3, 30, 1, 0x7FFF, {{0, 104, 6, 0}, {10, 96, 2, 0}, {14, 112, 2, 0}}},
        /* the e of 2's 16th ahead of beat 3, then the & of 4 */
        {3, 30, 1, 0x7FFF, {{0, 104, 6, 0}, {7, 100, 2, 0}, {14, 112, 2, 0}}},
    },
};

int gen_chords_v2_variant(int style, int seed) {
    return gen_is_chords_v2(style) ? (int)(mix(seed, 0, SALT_CVAR) % CH2_VARIANTS) : 0;
}

int gen_is_chords_v2(int style) { return style >= GEN_CH2_FIRST && style <= GEN_CH2_LAST; }

static const ch2_hit_t *ch2_hit(const ch2_style_t *t, int pos) {
    for (int h = 0; h < t->n; h++)
        if (t->hit[h].pos == pos) return &t->hit[h];
    return NULL;
}

/* The hits of a unit of u steps at complexity c, as a mask of unit positions */
static unsigned ch2_unit(const ch2_style_t *t, int seed, int c, int u) {
    unsigned on = 0, tmpl = 0;
    int cand[SEQ_STEPS], nc = 0;
    for (int h = 0; h < t->n; h++) {
        if (t->hit[h].pos >= u) continue;
        tmpl |= 1u << t->hit[h].pos;
        if (c >= t->hit[h].from_c) on |= 1u << t->hit[h].pos;
    }
    for (int j = 0; j < u && j < SEQ_STEPS; j++)
        if ((t->ghost_mask >> j & 1) && !(tmpl >> j & 1)) cand[nc++] = j;
    for (int g = (c * nc * t->ghost_pct + 5000) / 10000; g > 0; g--) {   /* the next candidate by hash, ties to the earlier */
        int best = -1;
        unsigned bh = 0;
        for (int k = 0; k < nc; k++) {
            unsigned hv = mix(seed, cand[k], SALT_GHOST);
            if (!(on >> cand[k] & 1) && (best < 0 || hv > bh)) best = cand[k], bh = hv;
        }
        on |= 1u << best;
    }
    return on;
}

/* One span [a, e) of units of u steps, whose hits are `on` (ch2_unit) */
static void ch2_span(const gen_params_t *p, const ch2_style_t *t, unsigned on, int a, int e, int u, seq_step_t *st) {
    for (int i = a; i < e; i++) st[i] = (seq_step_t){0, TONE_R, 0, 100, 1, 0};
    for (int u0 = a; u0 < e; u0 += u)
        for (int j = 0; j < u && u0 + j < e; j++) {
            if (!(on >> j & 1)) continue;
            const ch2_hit_t *h = ch2_hit(t, j);
            int r = (int)(mix(p->seed, j, SALT_CVEL) % 21u) - 10;
            st[u0 + j] = (seq_step_t){1, TONE_R, 0, (unsigned char)(h ? clampi(h->vel + r, 1, 127) : 60 + r * 8 / 10),
                                      h ? h->len : t->ghost_len, 0};
        }
    for (int i = a; i < e; i++) {
        if (!st[i].on) continue;
        int j = i + 1;
        while (j < e && !st[j].on) j++;
        if (st[i].len > j - i) st[i].len = (unsigned char)(j - i);   /* the next hit, or the span's end */
    }
}

/* The spans that hold any of steps from .. to - 1 (clipped to the loop) */
static void gen_chords_v2(const gen_params_t *p, int from, int to, seq_step_t *st) {
    int n = clampi(p->bars, 1, SEQ_MAX_BARS) * SEQ_STEPS, cl = p->chord_len < 1 || p->chord_len > n ? n : p->chord_len;
    int c = clampi(p->complexity, 0, 100), u = cl < SEQ_STEPS ? cl : SEQ_STEPS;
    const ch2_style_t *t = &CH2[p->style - GEN_CH2_FIRST][p->variant >= 0 && p->variant < CH2_VARIANTS ? p->variant : 0];
    if (t->hit[0].pos >= u) t = &CH2[p->style - GEN_CH2_FIRST][0];   /* no hit in the unit (hits are in order) */
    unsigned on = ch2_unit(t, p->seed, c, u);   /* every unit plays the same hits */
    if (from < 0) from = 0;
    for (int a = from / cl * cl; a < n && a < to; a += cl) ch2_span(p, t, on, a, a + cl < n ? a + cl : n, u, st);
}

static void gen_lane(const gen_params_t *p, gen_cache_t *c, seq_step_t *steps) {
    if (gen_is_chords_v2(p->style)) {
        gen_chords_v2(p, 0, SEQ_LANE_STEPS, steps);
        return;
    }
    if (p->style <= GEN_MANUAL || p->style >= GEN_V1_STYLES) return;
    gen_ctx_t g;
    if (is_mel(p->style)) {
        mel_prepare(&g, p, c, steps);
        mel_pitches(&g, steps, g.n);
        return;
    }
    ctx_init(&g, p);
    for (int span = 0, a = 0; a < g.n; span++, a += g.cl) gen_span(&g, span, a, a + g.cl < g.n ? a + g.cl : g.n, steps);
}

void gen_lane_v1(const gen_params_t *p, seq_step_t *steps) { gen_lane(p, NULL, steps); }

void gen_lane_cached_v1(const gen_params_t *p, gen_cache_t *c, seq_step_t *steps) { gen_lane(p, c, steps); }

/* Chords and Bass spans are generated independently (a span reads only the weights, its own steps and the chords), so
 * one alone is the same as in the whole loop. A melody's pitches follow from the notes before, across spans: it
 * generates the loop's rhythm and the pitches up to the span's end, and copies the span. */
static void gen_one_span(const gen_params_t *p, gen_cache_t *c, int step, seq_step_t *steps) {
    if (gen_is_chords_v2(p->style)) {
        if (step >= 0) gen_chords_v2(p, step, step + 1, steps);
        return;
    }
    if (p->style <= GEN_MANUAL || p->style >= GEN_V1_STYLES) return;
    gen_ctx_t g;
    ctx_layout(&g, p);
    if (step < 0 || step >= g.n) return;
    int span = step / g.cl, a = span * g.cl, e = a + g.cl < g.n ? a + g.cl : g.n;
    if (is_mel(p->style)) {
        seq_step_t all[SEQ_LANE_STEPS];
        mel_prepare(&g, p, c, all);
        mel_pitches(&g, all, e);
        memcpy(&steps[a], &all[a], sizeof(seq_step_t) * (size_t)(e - a));
        return;
    }
    ctx_init(&g, p);
    gen_span(&g, span, a, e, steps);
}

void gen_span_v1(const gen_params_t *p, int step, seq_step_t *steps) { gen_one_span(p, NULL, step, steps); }

void gen_span_cached_v1(const gen_params_t *p, gen_cache_t *c, int step, seq_step_t *steps) { gen_one_span(p, c, step, steps); }

void gen_spans_v1(const gen_params_t *p, int to, seq_step_t *steps) {
    if (gen_is_chords_v2(p->style)) {
        gen_chords_v2(p, 0, to, steps);
        return;
    }
    if (p->style <= GEN_MANUAL || p->style >= GEN_V1_STYLES) return;
    if (is_mel(p->style)) {
        gen_lane(p, NULL, steps);
        return;
    }
    gen_ctx_t g;
    ctx_init(&g, p);
    for (int span = 0, a = 0; a < g.n && a < to; span++, a += g.cl) gen_span(&g, span, a, a + g.cl < g.n ? a + g.cl : g.n, steps);
}

int gen_transposes(int style) { return is_mel(style); }

/* Each note keeps its tone and moves with the centre; its octave is counted again from its chord's root in the new key
 * (the move is a whole number of octaves from there, as the candidates are the same scale notes around the centre). */
void gen_transpose_v1(const gen_params_t *p, int from_key, seq_step_t *steps) {
    gen_ctx_t g;
    if (!gen_transposes(p->style)) return;
    ctx_layout(&g, p);
    int from = pmod(from_key, 12), shift = tonic_centre(&g) - centre_in(&g, from);
    for (int i = 0; i < g.n; i++) {
        seq_step_t *x = &steps[i];
        if (!x->on) continue;
        int deg = span_deg(&g, span_of(&g, i)), pitch = theory_tone_note(from, g.scale, deg, x->tone, x->oct) + shift;
        x->oct = (signed char)((pitch - note(&g, deg, x->tone, 0)) / 12);
    }
}

/* A deny-list: the styles known to ignore key, scale and progression; any other id (a later build's) is assumed to
 * read them, so a missed regen can't happen. */
int gen_reads_harmony(int style) {
    return !(style == GEN_MANUAL || (style >= GEN_CH_SUSTAIN && style <= GEN_CH_STABS) || style == GEN_BASS_ROOT ||
             style == GEN_BASS_PUMP || gen_is_chords_v2(style));
}

/* HARMONY. Root-motion weights from one diatonic degree (row) to the next (column), after de Clercq & Temperley's rock
 * corpus: I, IV, V and vi carry most of it; up a fourth (d + 3) and down a fourth (d + 4) lead, then seconds, then
 * thirds; the pop cycle I-V-vi-IV (vi-IV-I-V) is strong. Tonic -> predominant (ii, IV, vi) -> dominant (V, vii) ->
 * tonic. Self-moves are 0: holding a chord is PROG_HOLD, from c 67. */
static const unsigned char PROG_MOVE[7][7] = {
    /*  I  ii iii  IV   V  vi vii */
    {   0,  8,  4, 35, 30, 20,  2},   /* I */
    {  10,  0, 10, 10, 50,  8,  5},   /* ii */
    {  10, 10,  0, 35,  8, 35,  2},   /* iii */
    {  35,  8,  6,  0, 35,  8,  8},   /* IV */
    {  35,  3,  5, 20,  0, 30,  2},   /* V */
    {   8, 20,  8, 40, 20,  0, 10},   /* vi */
    {  35,  2, 15, 25,  5, 20,  0},   /* vii (VII in minor) */
};
#define PROG_HOLD 3
static const unsigned char PROG_END[7] = {0, 1, 0, 3, 4, 2, 1};   /* how much each allowed ending is preferred */

/* Bit d = degree d: {I, IV, V, vi}, then ii and iii from c 34, vii from c 67. Endings: anything that leads back to I
 * as the loop wraps (I-V-vi-IV, I-vi-IV-V): {IV, V, vi}, then ii from c 34, vii from c 67. */
static unsigned prog_tier(int c) { return c < 34 ? 0x39u : c < 67 ? 0x3Fu : 0x7Fu; }
static unsigned prog_ends(int c) { return c < 34 ? 0x38u : c < 67 ? 0x3Au : 0x7Au; }

/* The move a -> b at complexity c: 0 outside the tier; ii and iii grow with c inside it. vii stays rare: the weights
 * don't know the scale, and it is VII in minor but diminished in major. */
static unsigned prog_move(int a, int b, int c) {
    if (!(prog_tier(c) >> b & 1)) return 0;
    if (a == b) return c >= 67 ? PROG_HOLD : 0;
    return b == 1 || b == 2 ? PROG_MOVE[a][b] * (unsigned)(50 + c) / 50u : PROG_MOVE[a][b];
}

/* How strongly degree d leads into an allowed ending: the penultimate chord sets up the cadence. */
static unsigned prog_cadence(int d, int c) {
    unsigned w = 0;
    for (int e = 0; e < 7; e++) w += (prog_ends(c) >> e & 1) * prog_move(d, e, c) * PROG_END[e];
    return w;
}

/* The weight of degree d in slot i (after out[0..i-1]): the move from the previous chord, a quarter for going straight
 * back (I-V-I-V ping-pong), three times for the tonic half-way through 6+ chords (two phrases), an allowed ending in
 * the last slot, and a chord that can reach one in the slot before. */
static unsigned prog_weight(const int *out, int i, int len, int d, int c) {
    unsigned w = prog_move(out[i - 1], d, c);
    if (i >= 2 && d == out[i - 2]) w = (w + 3) / 4;
    if (len >= 6 && i == len / 2 && d == 0) w *= 3;
    if (i == len - 1) return w * PROG_END[d] * (prog_ends(c) >> d & 1);
    return i == len - 2 ? w * prog_cadence(d, c) : w;
}

static int prog_pick(int seed, const int *out, int i, int len, int c) {
    unsigned w[7], total = 0;
    for (int d = 0; d < 7; d++) total += w[d] = prog_weight(out, i, len, d, c);
    if (!total) return 4;   /* never: every tier chord reaches an allowed ending, which the slot before ensures */
    unsigned r = mix(seed, i, SALT_PROG) % total;
    int d = 0;
    while (r >= w[d]) r -= w[d++];
    return d;
}

void gen_progression_v1(int seed, int complexity, int len, int out[]) {
    int c = clampi(complexity, 0, 100);
    if (len < 1) return;
    out[0] = 0;
    for (int i = 1; i < len; i++) out[i] = prog_pick(seed, out, i, len, c);
}

/* Chance (harmony - 60) in 80 (1.25 % a point, 50 % at 100); the slot (1 .. len - 2) and the borrow (1..3) from other
 * bits of the same draw, which depends on the seed and the length only, so a borrow at some harmony stays the same
 * borrow at every higher one. */
void gen_progression_v2(int seed, int harmony, int len, int deg[], int src[], int scale) {
    int c = clampi(harmony, 0, 100);
    if (len < 1) return;
    gen_progression_v1(seed, c, len, deg);
    for (int i = 0; i < len; i++) src[i] = 0;
    unsigned h = mix(seed, len, SALT_BORROW);
    if (c <= 60 || len < 3 || (int)(h % 80u) >= c - 60) return;
    int slot = 1 + (int)((h >> 8) % (unsigned)(len - 2)), b = 1 + (int)((h >> 20) % THEORY_NBORROW);
    src[slot] = b;
    deg[slot] = theory_borrow_degree(scale < 0 || scale >= SCALE_COUNT ? 0 : scale, b);
}
