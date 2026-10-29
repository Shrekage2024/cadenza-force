#include "melody2.h"
#include "bassline.h"   /* bass_level: the tension colour band */
#include "theory.h"
#include "util.h"

#define MAXN 32   /* notes in a cell: a block has at most 16, a fill adds 4 */

/* A cell note: step p, scale degree d, length in steps. ped: the PEDAL note, which plays its scale note (never a chord-tone
 * slot) for as long as it moves only by octaves. */
typedef struct { int p, d, len, ped; } mn_t;
typedef struct { int n; mn_t x[MAXN]; } cell_t;
/* A chord as the model sees it: r its root's scale degree in -3..3, pc its root, 3rd and 5th, mi a minor 3rd */
typedef struct { int r, pc[3], mi; } mch_t;

static const char *const COL[MJ_COLS] = {"ARP", "OSTINATO", "HOOK", "LEAD", "RUN", "PEDAL"};
static const char *const BLOCK_NAME[MJ_COLS][MJ_LEVELS] = {
    {"UP", "UP-DOWN", "PINKY", "ALBERTI", "2-OCTAVE"}, {"PULSE", "OCTAVE", "ROOT-5", "CELL", "3+3+2"},
    {"2-NOTE", "MOTIF", "PENTA", "SEQUENCE", "PICKUP"}, {"GUIDE", "HALVES", "PHRASE", "PASSING", "SOAR"},
    {"STEP-IN", "TURN", "RUN-UP", "RUN-DOWN", "CASCADE"}, {"HOLD 5", "HOLD 1", "ALT", "ALT 16", "CLIMB"}};
static const char *const FORM_NAME[MF_COUNT] = {"LOOP", "1+1", "2+2", "3+1", "AAB", "SENT"};
static const char *const ANSWER_NAME[MA_COUNT] = {"ECHO", "SHIFT", "MIRROR", "RHYTHM", "TAIL", "SWAP"};
static const char *const BREATH_NAME[4] = {"None", "1/8", "Beat", "Half"};
static const char *const CAD_NAME[4] = {"OFF", "OPEN", "SOFT", "HOME"};
static const char *const FILL_NAME[4] = {"REST", "PICKUP", "STEP-IN", "RUN"};
static const char *const STRAY_NAME[4] = {"LAST NOTE", "LAST BEAT", "LAST HALF", "ALL"};
static const char *const DENS_NAME[5] = {"SPARSE", "THIN", "AS DRAWN", "FILLED", "BUSY"};
static const char *const RNG_NAME[4] = {"NARROW", "AS DRAWN", "WIDE", "HUGE"};
static const char *const VAR_NAME[4] = {"OFF", "BAR 4", "BARS 2+4", "ALL BARS"};
static const char *const TIME_NAME[3] = {"HALF", "NORMAL", "DOUBLE"};

const char *mjam_col_name(int c) { return COL[clampi(c, 0, MJ_COLS - 1)]; }
const char *mjam_block_name(int c, int l) { return BLOCK_NAME[clampi(c, 0, MJ_COLS - 1)][clampi(l, 0, MJ_LEVELS - 1)]; }
const char *mjam_form_name(int f) { return FORM_NAME[clampi(f, 0, MF_COUNT - 1)]; }
const char *mjam_answer_name(int a) { return ANSWER_NAME[clampi(a, 0, MA_COUNT - 1)]; }
const char *mjam_breath_name(int b) { return BREATH_NAME[clampi(b, 0, 3)]; }
const char *mjam_cad_name(int c) { return CAD_NAME[clampi(c, 0, 3)]; }
const char *mjam_fill_name(int f) { return FILL_NAME[clampi(f, 0, 3)]; }
const char *mjam_stray_name(int s) { return STRAY_NAME[clampi(s, 0, 3)]; }
const char *mjam_dens_name(int d) { return DENS_NAME[clampi(d, -2, 2) + 2]; }
const char *mjam_rng_name(int r) { return RNG_NAME[clampi(r, -1, 2) + 1]; }
const char *mjam_var_name(int v) { return VAR_NAME[clampi(v, 0, 3)]; }
const char *mjam_time_name(int t) { return TIME_NAME[clampi(t, -1, 1) + 1]; }

mjam_set_t mjam_default(void) {
    mjam_set_t s = {MJ_ARP, 0, MF_LOOP, 0, 3, 0, MA_ECHO, 1, 0, 1, MJ_RUN, 2, 0, 0, 0, 0};
    return s;
}

static mjam_set_t sane(const mjam_set_t *s) {
    mjam_set_t o = *s;
    o.c = (signed char)clampi(o.c, 0, MJ_COLS - 1);
    o.l = (signed char)clampi(o.l, 0, MJ_LEVELS - 1);
    o.form = (signed char)clampi(o.form, 0, MF_COUNT - 1);
    o.breath = (signed char)clampi(o.breath, 0, 3);
    o.cad = (signed char)clampi(o.cad, 0, 3);
    o.fill = (signed char)clampi(o.fill, 0, 3);
    o.ans = (signed char)clampi(o.ans, 0, MA_COUNT - 1);
    o.stray = (signed char)clampi(o.stray, 0, 3);
    o.aoct = (signed char)clampi(o.aoct, -1, 1);
    o.reg = (signed char)clampi(o.reg, 0, 2);
    o.sc = (signed char)clampi(o.sc, 0, MJ_COLS - 1);
    o.sl = (signed char)clampi(o.sl, 0, MJ_LEVELS - 1);
    o.dens = (signed char)clampi(o.dens, -2, 2);
    o.rng = (signed char)clampi(o.rng, -1, 2);
    o.var = (signed char)clampi(o.var, 0, 3);
    o.tm = (signed char)clampi(o.tm, -1, 1);
    return o;
}

/* FORMS: each phrase bar's role and the bar (offset in the phrase) it plays */
static const char ROLE[MF_COUNT][5] = {"CCCC", "CACA", "CCAA", "CCCA", "CRAA", "CSFA"};
static const unsigned char SRC[MF_COUNT][4] = {{0, 1, 2, 3}, {0, 0, 2, 2}, {0, 1, 0, 1}, {0, 1, 2, 0}, {0, 0, 0, 1}, {0, 0, 0, 1}};

int mjam_role(int form, int pos, int *src_pos) {
    form = clampi(form, 0, MF_COUNT - 1);
    pos = clampi(pos, 0, 3);
    if (src_pos) *src_pos = SRC[form][pos];
    return ROLE[form][pos];
}

static int role_at(int form, int pos) { return pos < 4 ? ROLE[form][pos] : 0; }

void mjam_chord_of(int key, int scale, int slot_scale, int deg, mjam_chord_t *out) {
    int n[THEORY_CHORD_MAX];
    (void)scale;   /* the slot's own scale names the chord */
    theory_chord_notes(key, slot_scale, deg, COL_TRIAD, n);
    out->root = n[0] % 12;
    out->third = n[1] % 12;
    out->fifth = n[2] % 12;
}

/* ---- chords and pitch ---- */

static int is_ct(int d, const mch_t *ch) { return (0x15 >> pmod(d - ch->r, 7)) & 1; }   /* offset 0, 2 or 4 from r */

static void chord_info(const mjam_loop_in_t *in, const mjam_chord_t *c, mch_t *o) {
    int rel = pmod(c->root - in->key, 12), deg = 6;
    while (deg > 0 && theory_degree_pc(0, in->scale, deg) > rel) deg--;   /* a root outside the scale: the degree below */
    o->r = deg <= 3 ? deg : deg - 7;
    o->pc[0] = pmod(c->root, 12);
    o->pc[1] = c->third >= 0 ? pmod(c->third, 12) : theory_degree_pc(in->key, in->scale, deg + 2);
    o->pc[2] = c->fifth >= 0 ? pmod(c->fifth, 12) : theory_degree_pc(in->key, in->scale, deg + 4);
    o->mi = pmod(o->pc[1] - o->pc[0], 12) == 3;
}

static void bar_chord(const mjam_loop_in_t *in, int b, mch_t *o) { chord_info(in, in->bar[pmod(b, in->bars)].chord[0], o); }

static int scale_midi(const mjam_loop_in_t *in, int d) { return 60 + in->key + 12 * fdiv(d, 7) + theory_degree_pc(0, in->scale, d); }

/* A chord-tone slot of the bar's chord plays that tone of the chord under the note, nearest the slot's scale note */
static int note_midi(const mjam_loop_in_t *in, const mch_t *bar, const mch_t *under, const mn_t *x) {
    int m = scale_midi(in, x->d), k = pmod(x->d - bar->r, 7);
    if (x->ped || !((0x15 >> k) & 1)) return m;
    return m + pmod(under->pc[k / 2] - m + 6, 12) - 6;
}

static int in_key(const mjam_loop_in_t *in, int pc) {
    for (int d = 0; d < 7; d++) if (theory_degree_pc(in->key, in->scale, d) == pc) return 1;
    return 0;
}

/* how a note sits on its chord: 0 a chord tone .. 85 a clash (the reference's tension()) */
static int tension(const mjam_loop_in_t *in, int pc, const mch_t *ch) {
    static const unsigned char T[12] = {0, 85, 26, 70, 70, 40, 40, 0, 55, 18, 34, 44};   /* 5 and 10 on a major chord */
    int iv = pmod(pc - ch->pc[0], 12);
    if (iv == 0 || iv == 7) return 0;
    if (iv == (ch->mi ? 3 : 4)) return 4;
    if (!in_key(in, pc)) return iv == 1 || iv == 6 ? 85 : 60;
    if (ch->mi && (iv == 5 || iv == 10)) return iv == 5 ? 24 : 22;
    return T[iv];
}

static int near(int d, int to) { return d + 7 * fdiv(2 * (to - d) + 7, 14); }   /* d + 7 round((to - d) / 7) */

static int pent(int scale, int k) {
    static const unsigned char MIN_P[5] = {0, 2, 3, 4, 6}, MAJ_P[5] = {0, 1, 2, 4, 5};
    int major = scale == SCALE_MAJOR || scale == SCALE_MIXOLYDIAN || scale == SCALE_LYDIAN;
    return 7 * fdiv(k, 5) + (major ? MAJ_P : MIN_P)[pmod(k, 5)];
}

/* LEAD's guide line: from the 3rd of the phrase's first chord, the chord tone nearest the last note (3rd first on a tie) */
static int guide(const mjam_loop_in_t *in, int b) {
    int pos = pmod(b, 4), base = b - pos, g;
    mch_t ch;
    bar_chord(in, base, &ch);
    g = ch.r + 2;
    for (int i = 1; i <= pos; i++) {
        bar_chord(in, base + i, &ch);
        int o[3] = {near(ch.r + 2, g), near(ch.r + 4, g), near(ch.r, g)}, best = o[0];
        for (int j = 1; j < 3; j++) if (iabs(o[j] - g) < iabs(best - g)) best = o[j];
        g = best;
    }
    return g;
}

/* ---- the 30 blocks: one bar each, notes as a base plus an offset in degrees ---- */

/* bases: R the chord root's degree; G, G2 LEAD's guide note and its partner; PK HOOK's pentatonic index (the offset counts
 * pentatonic steps); NN, UP, DN, C3 RUN's targets from the next chord; P the pedal (degree 4, HOLD 1: 7); U0..U2 the chord
 * tones below P */
enum { B_R, B_G, B_G2, B_PK, B_NN, B_UP, B_DN, B_C3, B_P, B_U0, B_U1, B_U2, B_COUNT };
typedef struct { signed char p, base, off, len; } pat_t;
#define R_(p, o, l) {p, B_R, o, l}
#define P_(p, l) {p, B_P, 0, l}
#define U_(p, u, o, l) {p, B_U0 + (u), o, l}
static const struct { unsigned char n; pat_t x[16]; } BLOCK[MJ_COLS][MJ_LEVELS] = {
    {   /* ARP */
        {8, {R_(0, 0, 2), R_(2, 2, 2), R_(4, 4, 2), R_(6, 7, 2), R_(8, 0, 2), R_(10, 2, 2), R_(12, 4, 2), R_(14, 7, 2)}},
        {8, {R_(0, 0, 2), R_(2, 2, 2), R_(4, 4, 2), R_(6, 7, 2), R_(8, 4, 2), R_(10, 2, 2), R_(12, 0, 2), R_(14, 2, 2)}},
        {8, {R_(0, 0, 2), R_(2, 7, 2), R_(4, 2, 2), R_(6, 7, 2), R_(8, 4, 2), R_(10, 7, 2), R_(12, 2, 2), R_(14, 7, 2)}},
        {16, {R_(0, 0, 1), R_(1, 4, 1), R_(2, 2, 1), R_(3, 4, 1), R_(4, 0, 1), R_(5, 4, 1), R_(6, 2, 1), R_(7, 4, 1),
              R_(8, 0, 1), R_(9, 4, 1), R_(10, 2, 1), R_(11, 4, 1), R_(12, 0, 1), R_(13, 4, 1), R_(14, 2, 1), R_(15, 4, 1)}},
        {16, {R_(0, 0, 1), R_(1, 2, 1), R_(2, 4, 1), R_(3, 7, 1), R_(4, 9, 1), R_(5, 11, 1), R_(6, 14, 1), R_(7, 11, 1),
              R_(8, 9, 1), R_(9, 7, 1), R_(10, 4, 1), R_(11, 2, 1), R_(12, 0, 1), R_(13, 2, 1), R_(14, 4, 1), R_(15, 7, 1)}}},
    {   /* OSTINATO */
        {8, {R_(0, 7, 1), R_(2, 7, 1), R_(4, 7, 1), R_(6, 7, 1), R_(8, 7, 1), R_(10, 7, 1), R_(12, 7, 1), R_(14, 7, 1)}},
        {8, {R_(0, 0, 1), R_(2, 7, 1), R_(4, 0, 1), R_(6, 7, 1), R_(8, 0, 1), R_(10, 7, 1), R_(12, 0, 1), R_(14, 7, 1)}},
        {8, {R_(0, 0, 1), R_(2, 4, 1), R_(4, 7, 1), R_(6, 4, 1), R_(8, 0, 1), R_(10, 4, 1), R_(12, 7, 1), R_(14, 4, 1)}},
        {6, {R_(0, 0, 2), R_(3, 0, 2), R_(6, 4, 2), R_(8, 5, 2), R_(11, 4, 2), R_(14, 0, 1)}},
        {7, {R_(0, 7, 2), R_(3, 4, 2), R_(6, 7, 2), R_(8, 2, 1), R_(10, 3, 1), R_(11, 4, 2), R_(14, 0, 1)}}},
    {   /* HOOK */
        {2, {R_(0, 2, 7), R_(8, 0, 7)}},
        {5, {R_(0, 4, 2), R_(2, 3, 2), R_(4, 2, 4), R_(12, 4, 2), R_(14, 2, 2)}},
        {6, {{0, B_PK, 0, 3}, {3, B_PK, -1, 3}, {6, B_PK, -2, 2}, {8, B_PK, -1, 2}, {10, B_PK, 0, 2}, {12, B_PK, 1, 4}}},
        {8, {R_(0, 0, 2), R_(2, 1, 2), R_(4, 2, 2), R_(6, 0, 2), R_(8, 1, 2), R_(10, 2, 2), R_(12, 3, 2), R_(14, 1, 2)}},
        {8, {R_(0, 4, 2), R_(2, 3, 2), R_(4, 2, 3), R_(7, 1, 1), R_(8, 2, 4), R_(12, 4, 2), R_(14, 5, 1), R_(15, 6, 1)}}},
    {   /* LEAD */
        {1, {{0, B_G, 0, 15}}},
        {2, {{0, B_G, 0, 8}, {8, B_G2, 0, 7}}},
        {4, {R_(0, 2, 4), R_(4, 4, 4), R_(8, 7, 4), R_(12, 4, 3)}},
        {7, {R_(0, 2, 2), R_(2, 3, 2), R_(4, 4, 2), R_(6, 5, 2), R_(8, 7, 4), R_(12, 6, 2), R_(14, 4, 2)}},
        {6, {R_(0, 4, 4), R_(4, 9, 4), R_(8, 8, 2), R_(10, 7, 2), R_(12, 6, 2), R_(14, 4, 2)}}},
    {   /* RUN */
        {4, {R_(0, 2, 12), {13, B_NN, -3, 1}, {14, B_NN, -2, 1}, {15, B_NN, -1, 1}}},
        {5, {R_(0, 2, 8), R_(8, 3, 2), R_(10, 2, 2), R_(12, 1, 2), R_(14, 2, 2)}},
        {7, {R_(0, 0, 4), R_(4, 2, 4), R_(8, 4, 4), {12, B_UP, -4, 1}, {13, B_UP, -3, 1}, {14, B_UP, -2, 1}, {15, B_UP, -1, 1}}},
        {9, {R_(0, 4, 8), {8, B_DN, 8, 1}, {9, B_DN, 7, 1}, {10, B_DN, 6, 1}, {11, B_DN, 5, 1}, {12, B_DN, 4, 1}, {13, B_DN, 3, 1},
             {14, B_DN, 2, 1}, {15, B_DN, 1, 1}}},
        {16, {{0, B_C3, 16, 1}, {1, B_C3, 15, 1}, {2, B_C3, 14, 1}, {3, B_C3, 13, 1}, {4, B_C3, 12, 1}, {5, B_C3, 11, 1},
              {6, B_C3, 10, 1}, {7, B_C3, 9, 1}, {8, B_C3, 8, 1}, {9, B_C3, 7, 1}, {10, B_C3, 6, 1}, {11, B_C3, 5, 1},
              {12, B_C3, 4, 1}, {13, B_C3, 3, 1}, {14, B_C3, 2, 1}, {15, B_C3, 1, 1}}}},
    {   /* PEDAL */
        {1, {P_(0, 15)}},
        {1, {P_(0, 15)}},
        {8, {P_(0, 2), U_(2, 0, 0, 2), P_(4, 2), U_(6, 1, 0, 2), P_(8, 2), U_(10, 2, 0, 2), P_(12, 2), U_(14, 1, 0, 2)}},
        {16, {P_(0, 1), U_(1, 0, 0, 1), P_(2, 1), U_(3, 1, 0, 1), P_(4, 1), U_(5, 2, 0, 1), P_(6, 1), U_(7, 1, 0, 1),
              P_(8, 1), U_(9, 0, 0, 1), P_(10, 1), U_(11, 1, 0, 1), P_(12, 1), U_(13, 2, 0, 1), P_(14, 1), U_(15, 1, 0, 1)}},
        {16, {P_(0, 1), U_(1, 0, 0, 1), P_(2, 1), U_(3, 0, 1, 1), P_(4, 1), U_(5, 0, 2, 1), P_(6, 1), U_(7, 0, 3, 1),
              P_(8, 1), U_(9, 0, 4, 1), P_(10, 1), U_(11, 0, 5, 1), P_(12, 1), U_(13, 0, 6, 1), P_(14, 1), U_(15, 0, 7, 1)}}}};
#undef R_
#undef P_
#undef U_

static void add(cell_t *c, mn_t x) { if (c->n < MAXN) c->x[c->n++] = x; }

/* block c, l on bar b (its chord, and the next bar's for RUN) */
static void block(const mjam_loop_in_t *in, int c, int l, int b, cell_t *o) {
    int v[B_COUNT] = {0};
    mch_t ch, nx;
    bar_chord(in, b, &ch);
    v[B_R] = ch.r;
    if (c == MJ_HOOK) {
        int k = 0;
        while (pent(in->scale, k + 1) <= ch.r + 4) k++;
        while (pent(in->scale, k) > ch.r + 4) k--;
        v[B_PK] = k;
    } else if (c == MJ_LEAD) {
        v[B_G] = guide(in, b);
        v[B_G2] = is_ct(v[B_G] + 2, &ch) ? v[B_G] + 2 : v[B_G] - 2;
    } else if (c == MJ_RUN) {
        bar_chord(in, b + 1, &nx);
        v[B_NN] = near(nx.r, ch.r + 2);
        v[B_UP] = near(nx.r, ch.r + 7);
        v[B_DN] = near(nx.r, ch.r);
        v[B_C3] = near(nx.r + 2, ch.r - 4);
    } else if (c == MJ_PEDAL) {
        int n = 0;
        v[B_P] = l == 1 ? 7 : 4;
        for (int d = v[B_P] - 7; d < v[B_P] && n < 3; d++) if (is_ct(d, &ch)) v[B_U0 + n++] = d;   /* 7 degrees hold 3 */
    }
    o->n = 0;
    for (int i = 0; i < BLOCK[c][l].n; i++) {
        const pat_t *x = &BLOCK[c][l].x[i];
        int d = x->base == B_PK ? pent(in->scale, v[B_PK] + x->off) : v[(int)x->base] + x->off;
        add(o, (mn_t){x->p, d, x->len, x->base == B_P});
    }
}

/* ---- the phrase: transforms on cells ---- */

static void set_d(mn_t *x, int d) {
    if (pmod(d - x->d, 7)) x->ped = 0;
    x->d = d;
}

static void shift(cell_t *c, int k) { for (int i = 0; i < c->n; i++) set_d(&c->x[i], c->x[i].d + k); }

static void mirror(cell_t *c) {   /* upside down about the first note */
    int pv = c->n ? c->x[0].d : 0;
    for (int i = 0; i < c->n; i++) set_d(&c->x[i], 2 * pv - c->x[i].d);
}

/* notes on a strong beat move to the nearest chord tone (one rule for every transform) */
static void snap(cell_t *c, const mch_t *ch) {
    for (int i = 0; i < c->n; i++) {
        mn_t *x = &c->x[i];
        if (x->p % 4 || is_ct(x->d, ch)) continue;
        set_d(x, is_ct(x->d - 1, ch) ? x->d - 1 : x->d + 1);
    }
}

static int last_note(const cell_t *c, int skip) {   /* the first of the latest notes, not skip; -1 if none */
    int L = -1;
    for (int i = 0; i < c->n; i++) if (i != skip && (L < 0 || c->x[i].p > c->x[L].p)) L = i;
    return L;
}

/* the last note moves to the nearest degree deg (-1: none); a note before it on the same degree steps up */
static void end_on(cell_t *c, int deg) {
    int L = last_note(c, -1), prev, best;
    if (L < 0 || deg < 0) return;
    best = c->x[L].d;
    for (int k = 0; k <= 3; k++) {
        if (!pmod(c->x[L].d - k - deg, 7)) { best = c->x[L].d - k; break; }
        if (!pmod(c->x[L].d + k - deg, 7)) { best = c->x[L].d + k; break; }
    }
    prev = last_note(c, L);
    set_d(&c->x[L], best);
    if (prev >= 0 && c->x[prev].d == best) set_d(&c->x[prev], best + 1);
}

static void cut_at(cell_t *c, int cut) {   /* keep the notes before step cut, ending by it */
    int n = 0;
    for (int i = 0; i < c->n; i++)
        if (c->x[i].p < cut) {
            c->x[n] = c->x[i];
            if (c->x[n].len > cut - c->x[n].p) c->x[n].len = cut - c->x[n].p;
            n++;
        }
    c->n = n;
}

/* RHYTHM: the call's rhythm; its highest degree plays the lowest chord tone from the root, and so on */
static void rhythm(cell_t *c, const mch_t *ch) {
    int ds[MAXN], cts[MAXN], nd = 0, nc = 0;
    for (int i = 0; i < c->n; i++) {
        int j = 0;
        while (j < nd && ds[j] != c->x[i].d) j++;
        if (j == nd) ds[nd++] = c->x[i].d;
    }
    sort_ints(ds, nd);
    for (int d = ch->r; nc < nd; d++) if (is_ct(d, ch)) cts[nc++] = d;
    for (int i = 0; i < c->n; i++) {
        int j = 0;
        while (ds[j] != c->x[i].d) j++;
        set_d(&c->x[i], cts[nd - 1 - j]);
    }
}

/* the second half of h, played in the first half and again in the second, each note moved by k degrees */
static void halves(const cell_t *h, int k, cell_t *o) {
    o->n = 0;
    for (int i = 0; i < h->n; i++)
        if (h->x[i].p >= 8) {
            mn_t x = h->x[i];
            x.p -= 8;
            set_d(&x, x.d + k);
            add(o, x);
        }
    for (int i = 0, n = o->n; i < n; i++) {
        mn_t x = o->x[i];
        x.p += 8;
        add(o, x);
    }
}

/* an ANSWER bar b to the call on bar src: the ANSWER transform on the notes STRAY covers, counted from the end */
static void answer(const mjam_loop_in_t *in, const mjam_set_t *s, int src, int b, cell_t *o) {
    static const unsigned char CUT[4] = {15, 12, 8, 0};
    cell_t echo, t, sc;
    mch_t ch, cs;
    bar_chord(in, b, &ch);
    block(in, s->c, s->l, b, &echo);
    t = echo;
    switch (s->ans) {
    case MA_SHIFT: shift(&t, 2); snap(&t, &ch); break;
    case MA_MIRROR: mirror(&t); snap(&t, &ch); break;
    case MA_RHYTHM: rhythm(&t, &ch); break;
    case MA_TAIL:
        bar_chord(in, src, &cs);
        block(in, s->c, s->l, src, &sc);
        halves(&sc, ch.r - cs.r, &t);
        snap(&t, &ch);
        break;
    case MA_SWAP: block(in, s->sc, s->sl, b, &t); break;
    default: break;
    }
    *o = echo;
    if (CUT[(int)s->stray] == 15) {
        int L = last_note(&echo, -1);
        o->n = 0;
        for (int i = 0; i < echo.n; i++) if (i != L) add(o, echo.x[i]);
        if (L >= 0) for (int i = 0; i < t.n; i++) if (t.x[i].p >= echo.x[L].p) add(o, t.x[i]);
    } else {
        cut_at(o, CUT[(int)s->stray]);
        for (int i = 0; i < t.n; i++) if (t.x[i].p >= CUT[(int)s->stray]) add(o, t.x[i]);
    }
}

/* the bar's notes before breath, cadence and fill */
static void raw_bar(const mjam_loop_in_t *in, const mjam_set_t *s, int b, int v, cell_t *o) {
    int pos = pmod(b, 4), src = b - pos + SRC[(int)s->form][pos], role = ROLE[(int)s->form][pos];
    mch_t ch;
    bar_chord(in, b, &ch);
    if (role == 'A') {
        answer(in, s, src, b, o);
        shift(o, 7 * s->aoct);
    } else if (role == 'S') {   /* STEP UP */
        block(in, s->c, s->l, src, o);
        shift(o, 1);
        snap(o, &ch);
    } else if (role == 'F') {   /* BREAK UP */
        cell_t h;
        block(in, s->c, s->l, src, &h);
        halves(&h, 0, o);
        snap(o, &ch);
    } else {
        block(in, s->c, s->l, b, o);
    }
    if (v) {   /* NEW BAR: odd mirrored, even a step up */
        if (v % 2) mirror(o);
        else shift(o, 1);
        snap(o, &ch);
    }
}


/* ---- 0.21 VARIATION, RANGE, DENSITY: the JAM knobs, each on every bar. Each ends with the beat rule on the notes it moved or added ---- */

/* 0.21 the beat rule on the marked notes only (a kept note keeps its pitch), never on a PEDAL note (it holds its scale note) */
static void snap_marked(cell_t *c, const mch_t *ch, const unsigned char *mark) {
    for (int i = 0; i < c->n; i++) {
        mn_t *x = &c->x[i];
        if (!mark[i] || x->ped || x->p % 4 || is_ct(x->d, ch)) continue;
        set_d(x, is_ct(x->d - 1, ch) ? x->d - 1 : x->d + 1);
    }
}

/* the phrase position pos of bar b varies: BAR 4 {3}, BARS 2+4 {1, 3}, ALL {1, 2, 3}; pos 1 a step up, pos 2 mirrored, pos 3 both */
static void variation(const mjam_set_t *s, int b, const mch_t *ch, cell_t *c) {
    int pos = pmod(b, 4), vary = s->var == 1 ? pos == 3 : s->var == 2 ? pos == 1 || pos == 3 : s->var == 3 && pos > 0, was[MAXN];
    unsigned char mark[MAXN];
    if (!vary) return;
    for (int i = 0; i < c->n; i++) was[i] = c->x[i].d;
    if (pos >= 2) mirror(c);
    if (pos != 2) shift(c, 1);
    for (int i = 0; i < c->n; i++) mark[i] = c->x[i].d != was[i];   /* mirror's first note stays: no beat rule on it */
    snap_marked(c, ch, mark);
}

static int earliest(const cell_t *c) {
    int e = -1;
    for (int i = 0; i < c->n; i++) if (e < 0 || c->x[i].p < c->x[e].p) e = i;
    return e;
}

/* RANGE: every note moves to pivot + round((d - pivot) F), F = 1/2, 1, 3/2, 2 (halves away from zero); the earliest note is the pivot */
static void range(const mjam_set_t *s, const mch_t *ch, cell_t *c) {
    int e = earliest(c), pivot, k = 4 + 2 * s->rng, was[MAXN];
    if (s->rng == 0 || e < 0) return;
    pivot = c->x[e].d;
    for (int i = 0; i < c->n; i++) {
        int num = (c->x[i].d - pivot) * k, q = num >= 0 ? (num + 2) / 4 : -((2 - num) / 4);
        was[i] = c->x[i].d;
        set_d(&c->x[i], pivot + q);
    }
    for (int i = 0; i < c->n; i++) {   /* the beat rule in the direction the note moved: outward from the pivot (WIDE, HUGE) or inward */
        mn_t *x = &c->x[i];
        int dir = (was[i] > pivot ? 1 : -1) * (s->rng < 0 ? -1 : 1);
        if (x->d == was[i] || x->ped || x->p % 4 || is_ct(x->d, ch)) continue;   /* 0.21 an unmoved or PEDAL note keeps its pitch */
        for (int j = 1; j <= 4; j++) {
            int to = x->d + (j <= 2 ? dir * j : -dir * (j - 2));
            if (is_ct(to, ch)) { set_d(x, to); break; }
        }
    }
}

int mjam_strength(int p) { return p == 0 ? 4 : p % 8 == 0 ? 3 : p % 4 == 0 ? 2 : p % 2 == 0 ? 1 : 0; }

static void drop(cell_t *c, const unsigned char *kill) {
    int n = 0;
    for (int i = 0; i < c->n; i++) if (!kill[i]) c->x[n++] = c->x[i];
    c->n = n;
}

/* notes in time order (a stable sort by step) */
static void by_step(const cell_t *c, int ord[MAXN]) {
    for (int i = 0; i < c->n; i++) {
        int j = i;
        for (; j > 0 && c->x[ord[j - 1]].p > c->x[i].p; j--) ord[j] = ord[j - 1];
        ord[j] = i;
    }
}

/* THIN: the notes of the lowest metric strength go, but not the earliest; if all are alike, every second note goes */
static void thin(cell_t *c) {
    unsigned char kill[MAXN] = {0};
    int lo = 9, hi = -1, e = earliest(c), ord[MAXN];
    if (c->n < 2) return;
    for (int i = 0; i < c->n; i++) {
        int st = mjam_strength(c->x[i].p);
        lo = st < lo ? st : lo;
        hi = st > hi ? st : hi;
    }
    by_step(c, ord);
    for (int i = 0; i < c->n; i++) kill[i] = lo < hi ? i != e && mjam_strength(c->x[i].p) == lo : 0;
    if (lo == hi) for (int k = 1; k < c->n; k += 2) kill[ord[k]] = 1;
    drop(c, kill);
}

/* FILLED: a note halfway into every span of 2 steps or more, a scale step toward the next note (the last: the upper neighbour) */
static void filled(cell_t *c) {
    int n0 = c->n, ord[MAXN];
    by_step(c, ord);
    for (int k = 0; k < n0; k++) {
        mn_t *x = &c->x[ord[k]];
        int span = (k + 1 < n0 ? c->x[ord[k + 1]].p : SEQ_STEPS) - x->p, len = x->len;
        if (span >= 2) {
            int d = x->d, t = k + 1 < n0 ? c->x[ord[k + 1]].d : d + 1, nd = iabs(t - d) >= 2 ? d + (t > d ? 1 : -1) : t >= d ? d + 1 : d - 1;
            x->len = len < span / 2 ? len : span / 2;
            add(c, (mn_t){x->p + span / 2, nd, len < span - span / 2 ? len : span - span / 2, 0});
        }
    }
}

/* DENSITY: -2 SPARSE and -1 THIN thin the bar twice and once, 1 FILLED and 2 BUSY fill it once and twice */
static void density(const mjam_set_t *s, const mch_t *ch, cell_t *c) {
    unsigned char mark[MAXN] = {0};
    int n0;
    for (int i = 0; i < -s->dens; i++) thin(c);   /* THIN only removes: no beat rule */
    n0 = c->n;
    for (int i = 0; i < s->dens; i++) filled(c);
    for (int i = n0; i < c->n; i++) mark[i] = 1;   /* FILLED's new notes follow the kept ones */
    snap_marked(c, ch, mark);
}

/* 0.21 TIME (Amendment 1): HALF the notes before step 8 at 2p, twice as long (the rest go): the bar's first half at half speed;
 * DOUBLE the notes on even steps at p/2, max(1, len/2) long (odd ones go), then again 8 steps later: the bar's pattern twice. The beat
 * rule on the notes it moved or added (to_line resolves each chord-tone slot against the chord under its new step) */
static void time_k(const mjam_set_t *s, const mch_t *ch, cell_t *c) {
    unsigned char kill[MAXN] = {0}, mark[MAXN] = {0};
    int n;
    if (!s->tm) return;
    for (int i = 0; i < c->n; i++) {
        mn_t *x = &c->x[i];
        kill[i] = s->tm < 0 ? x->p >= 8 : x->p % 2;
        if (kill[i]) continue;
        x->len = s->tm < 0 ? 2 * x->len : x->len / 2 > 1 ? x->len / 2 : 1;
        x->p = s->tm < 0 ? 2 * x->p : x->p / 2;
        if (x->len > SEQ_STEPS - x->p) x->len = SEQ_STEPS - x->p;
    }
    drop(c, kill);
    n = c->n;
    if (s->tm > 0)
        for (int i = 0; i < n; i++) {
            mn_t x = c->x[i];
            x.p += 8;
            add(c, x);
        }
    for (int i = 0; i < c->n; i++) mark[i] = c->x[i].p != 0;   /* every note but the downbeat's moved (or is a copy) */
    snap_marked(c, ch, mark);
}

/* the bar's notes with NEW BAR, VARIATION, RANGE, DENSITY and 0.21 TIME: before breath, cadence, fill and register */
static void shaped_bar(const mjam_loop_in_t *in, const mjam_set_t *s, int b, int v, cell_t *o) {
    mch_t ch;
    bar_chord(in, b, &ch);
    raw_bar(in, s, b, v, o);
    variation(s, b, &ch, o);
    range(s, &ch, o);
    density(s, &ch, o);
    time_k(s, &ch, o);
}

/* LOOP's phrase end: each phrase's bar 4, and the last bar of a loop that is not a multiple of 4 bars (so a 1-, 2-, 3- or 6-bar
 * loop lands home too). bars: the loop's */
static int loop_end(int b, int bars) { return pmod(b, 4) == 3 || (bars % 4 && b == bars - 1); }

/* CADENCE: the last note of a call (before an answer) and of an answer (before the next call); LOOP: its phrase end (loop_end) */
static void cadence(const mjam_set_t *s, int b, int bars, cell_t *c) {
    static const signed char CALL[4] = {-1, 1, 4, 4}, ANS[4] = {-1, 4, 2, 0};
    int pos = pmod(b, 4), role = role_at(s->form, pos), next = role_at(s->form, pos + 1);
    if (s->form == MF_LOOP) { if (loop_end(b, bars)) end_on(c, ANS[(int)s->cad]); }
    else if (role == 'A' && next != 'A') end_on(c, ANS[(int)s->cad]);
    else if (role != 'A' && next == 'A') end_on(c, CALL[(int)s->cad]);
}

/* breath and fill come before each answer (in LOOP: before bar 1 comes round, loop_end) */
static int gap_bar(const mjam_set_t *s, int b, int bars) {
    int pos = pmod(b, 4);
    return s->form == MF_LOOP ? loop_end(b, bars) : role_at(s->form, pos) != 'A' && role_at(s->form, pos + 1) == 'A';
}

/* nf: the next bar's first degree, for FILL (has_nf 0: the next bar is silent); 0.21 odd (if not NULL): 1 when the shaped bar, before
 * breath, cadence and fill, has a note on an odd step (the swing grid's test) */
static void bar_notes(const mjam_loop_in_t *in, const mjam_set_t *s, int b, int v, int has_nf, int nf, cell_t *o, int *odd) {
    static const unsigned char BREATH[4] = {0, 2, 4, 8}, FILL[4] = {0, 1, 3, 4};
    static const signed char FILL_NOTE[4][4][3] = {   /* step, degree from nf, length */
        {{0}}, {{15, -1, 1}}, {{11, -3, 2}, {13, -2, 2}, {15, -1, 2}}, {{12, -4, 1}, {13, -3, 1}, {14, -2, 1}, {15, -1, 1}}};
    int gap = gap_bar(s, b, in->bars), br = BREATH[(int)s->breath], fl = FILL[(int)s->fill], room = s->fill == 2 ? 5 : fl;
    shaped_bar(in, s, b, v, o);
    if (odd) for (int i = 0; i < o->n; i++) *odd |= o->x[i].p % 2;
    if (gap && (br || fl)) cut_at(o, 16 - (br > room ? br : room));
    cadence(s, b, in->bars, o);
    if (gap && has_nf)
        for (int k = 0; k < fl; k++) {
            const signed char *f = FILL_NOTE[(int)s->fill][k];
            add(o, (mn_t){f[0], nf + f[1], f[2], 0});
        }
    shift(o, 7 * (s->reg - 1));
}

/* ---- the line: 16 steps ---- */

static int fold36(int m) {
    while (m > 100) m -= 12;
    while (m < 36) m += 12;
    return m;
}

/* a note's tension on the chord under it at step p: tension() plus 12 for a non-chord tone on a beat */
static int note_tension(const mjam_loop_in_t *in, int m, const mch_t *u, int p) {
    int pc = pmod(m, 12);
    return tension(in, pc, u) + (p % 4 == 0 && pc != u->pc[0] && pc != u->pc[1] && pc != u->pc[2] ? 12 : 0);
}

int mjam_tension(int key, int scale, const mjam_chord_t *ch, int note, int step) {
    mjam_loop_in_t in = {key, scale, 1, mjam_default(), 0, 0};
    mch_t u;
    chord_info(&in, ch, &u);
    return note_tension(&in, note, &u, step);
}

static void to_line(const mjam_loop_in_t *in, int b, const cell_t *c, int fold, mjam_note_t out[SEQ_STEPS]) {
    const mjam_bar_in_t *bi = &in->bar[b];
    int ord[MAXN];
    mch_t bar;
    chord_info(in, bi->chord[0], &bar);
    for (int i = 0; i < SEQ_STEPS; i++) out[i] = (mjam_note_t){0, 0, 0};
    for (int i = 0; i < c->n; i++) {   /* a stable sort by step */
        int j = i;
        for (; j > 0 && c->x[ord[j - 1]].p > c->x[i].p; j--) ord[j] = ord[j - 1];
        ord[j] = i;
    }
    for (int k = 0; k < c->n; k++) {
        const mn_t *x = &c->x[ord[k]];
        mch_t u;
        int m, t, end = x->p + x->len < SEQ_STEPS ? x->p + x->len : SEQ_STEPS;
        if (x->p < 0 || x->p >= SEQ_STEPS) continue;
        chord_info(in, bi->chord[x->p], &u);
        m = note_midi(in, &bar, &u, x);
        t = note_tension(in, m, &u, x->p);
        out[x->p] = (mjam_note_t){(unsigned char)(fold ? fold36(m) : clampi(m, 1, 127)), 0, (unsigned char)t};
        for (int j = x->p + 1; j < end; j++) if (!out[j].note) out[j] = (mjam_note_t){0, 1, (unsigned char)t};
    }
}

static void bar_line(const mjam_loop_in_t *in, int b, const mjam_set_t *set, int fold, mjam_note_t out[SEQ_STEPS], int *odd) {
    mjam_set_t s, live;
    cell_t nx, c;
    int first = -1;
    if (odd) *odd = 0;
    if (in->bars < 1) { for (int i = 0; i < SEQ_STEPS; i++) out[i] = (mjam_note_t){0, 0, 0}; return; }
    b = pmod(b, in->bars);
    s = sane(set);
    live = sane(&in->set);
    if (s.fill && gap_bar(&s, b, in->bars)) {   /* 0.21 only a FILL needs the next bar's first note (setter cost) */
        shaped_bar(in, &live, (b + 1) % in->bars, 0, &nx);
        for (int i = 0; i < nx.n; i++) if (first < 0 || nx.x[i].p < nx.x[first].p) first = i;
    }
    bar_notes(in, &s, b, in->v ? in->v[b] : 0, first >= 0, first >= 0 ? nx.x[first].d : 0, &c, odd);
    to_line(in, b, &c, fold, out);
}

void mjam_bar(const mjam_loop_in_t *in, int b, const mjam_set_t *set, mjam_note_t out[SEQ_STEPS]) { bar_line(in, b, set, 1, out, 0); }
void mjam_bar_ref(const mjam_loop_in_t *in, int b, const mjam_set_t *set, mjam_note_t out[SEQ_STEPS]) { bar_line(in, b, set, 0, out, 0); }
void mjam_bar_odd(const mjam_loop_in_t *in, int b, const mjam_set_t *set, mjam_note_t out[SEQ_STEPS], int *odd) { bar_line(in, b, set, 1, out, odd); }

int mjam_swing16_bar(const mjam_loop_in_t *in, int b) {
    mjam_set_t s = sane(&in->set);
    cell_t c;
    if (in->bars < 1) return 0;
    b = pmod(b, in->bars);
    shaped_bar(in, &s, b, in->v ? in->v[b] : 0, &c);
    for (int i = 0; i < c.n; i++) if (c.x[i].p % 2) return 1;
    return 0;
}

int mjam_swing16(const mjam_loop_in_t *in) {
    for (int b = 0; b < in->bars; b++) if (mjam_swing16_bar(in, b)) return 1;
    return 0;
}

void mjam_block_bar(const mjam_loop_in_t *in, int c, int l, int b, mjam_note_t out[SEQ_STEPS]) {
    cell_t cell;
    if (in->bars < 1) { for (int i = 0; i < SEQ_STEPS; i++) out[i] = (mjam_note_t){0, 0, 0}; return; }
    b = pmod(b, in->bars);
    block(in, clampi(c, 0, MJ_COLS - 1), clampi(l, 0, MJ_LEVELS - 1), b, &cell);
    to_line(in, b, &cell, 1, out);
}

/* ---- dots and score ---- */

static int level(int m) { return m <= 55 ? 0 : clampi((22 * (m - 55) + 34) / 68, 0, 11); }   /* round((m - 55) 11 / 34) */

int mjam_frame(const mjam_note_t line[SEQ_STEPS], int i) {
    if (i < 0 || i >= SEQ_STEPS) return 0;
    if (line[i].note) return 1 + level(line[i].note) * 5 + bass_level(line[i].tension);
    if (!line[i].hold) return 0;
    for (int j = i - 1; j >= 0; j--) if (line[j].note) return 61 + level(line[j].note);
    return 61;
}

int mjam_score(const mjam_note_t line[SEQ_STEPS]) {
    int max = 0, sum = 0, n = 0;
    for (int i = 0; i < SEQ_STEPS; i++) {
        if (!line[i].note) continue;
        if (line[i].tension > max) max = line[i].tension;
        sum += line[i].tension;
        n++;
    }
    if (!n) return 0;
    /* the reference's own floating-point sum and Math.round (exact halves land either way there, so no integer form) */
    double x = 0.6 * max + 0.4 * sum / n;
    int r = (int)x;
    return r + (x - r >= 0.5);
}
