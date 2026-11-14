#include "feel.h"
#include "util.h"

static int root_pc(int key, int scale, feel_chord_t c) { return theory_degree_pc(key, theory_slot_scale(scale, c.src), c.deg); }

static unsigned pc_mask(int key, int scale, feel_chord_t c) {
    int n[THEORY_CHORD_MAX], k = theory_chord_notes(key, theory_slot_scale(scale, c.src), c.deg, c.colour, n);
    unsigned m = 0;
    for (int i = 0; i < k; i++) m |= 1u << pmod(n[i], 12);
    return m;
}

int feel_same(int key, int scale, feel_chord_t a, feel_chord_t b) {
    return root_pc(key, scale, a) == root_pc(key, scale, b) && pc_mask(key, scale, a) == pc_mask(key, scale, b);
}

/* A tone's semitones above the chord's root */
static int above(int key, int scale, feel_chord_t c, int tone) {
    int sc = theory_slot_scale(scale, c.src);
    return pmod(theory_tone_note(key, sc, c.deg, tone, 0) - theory_tone_note(key, sc, c.deg, TONE_R, 0), 12);
}

static int quality(int key, int scale, feel_chord_t c) { return theory_triad_quality(key, theory_slot_scale(scale, c.src), c.deg); }
static int is_minor(int key, int scale, feel_chord_t c) { return quality(key, scale, c) == 1; }
static int is_dim(int key, int scale, feel_chord_t c) { return quality(key, scale, c) == 2; }
static int has_seventh(feel_chord_t c) { return c.colour == COL_7TH || c.colour == COL_9TH || c.colour == COL_11TH || c.colour == COL_13TH; }
static int is_dominant(int key, int scale, feel_chord_t c) {
    return has_seventh(c) && above(key, scale, c, TONE_3) == 4 && above(key, scale, c, TONE_7) == 10;
}
static int is_dim7(int key, int scale, feel_chord_t c) { return c.colour == COL_7TH && is_dim(key, scale, c) && above(key, scale, c, TONE_7) == 9; }

/* A chromatic chord, as the scale's own chord when it is one */
static feel_chord_t chroma(int key, int scale, int root, int quality, int colour) {
    feel_chord_t c = {0, theory_chroma_src(root, quality), colour};
    for (int d = 0; d < 7; d++) {
        feel_chord_t t = {d, 0, colour};
        if (feel_same(key, scale, c, t)) return t;
    }
    return c;
}

typedef struct { feel_option_t *o; int n, key, scale; } ladder_t;

static void offer(ladder_t *l, feel_chord_t c, int level, int kind) {
    for (int i = 0; i < l->n; i++)
        if (feel_same(l->key, l->scale, l->o[i].c, c)) return;
    if (l->n < FEEL_OPTIONS_MAX) l->o[l->n++] = (feel_option_t){c, level, kind};
}

/* The degrees that do a degree's job: tonic, subdominant, dominant families */
static const signed char FAMILY[7][3] = {{0, 5, 2}, {1, 3, -1}, {2, 0, 5}, {3, 1, -1}, {4, 6, -1}, {5, 0, 2}, {6, 4, -1}};

static void offer_home(ladder_t *l, feel_chord_t base) {
    offer(l, base, FEEL_HOME, FK_WRITTEN);
    if (base.src) return;
    for (int k = 1; k < 3 && FAMILY[base.deg][k] >= 0; k++) offer(l, (feel_chord_t){FAMILY[base.deg][k], 0, COL_TRIAD}, FEEL_HOME, FK_SAME_JOB);
}

static void offer_warm(ladder_t *l, feel_chord_t base) {
    feel_chord_t c = base;
    c.colour = COL_7TH;
    offer(l, c, FEEL_WARM, FK_RICHER);
    if (is_dim(l->key, l->scale, base)) return;
    c.colour = !base.src && base.deg == 4 && !is_minor(l->key, l->scale, base) ? COL_SUS4 : COL_ADD9;   /* the dominant: its sus, which wants to fall */
    offer(l, c, FEEL_WARM, FK_RICHER);
    c.colour = COL_SUS2;
    offer(l, c, FEEL_WARM, FK_RICHER);
    c.colour = COL_6TH;
    offer(l, c, FEEL_WARM, FK_RICHER);
}

int feel_ladder(int key, int scale, feel_chord_t base, feel_chord_t prev, feel_chord_t next, feel_option_t out[FEEL_OPTIONS_MAX]) {
    ladder_t l = {out, 0, key, scale};
    int nr = pmod(root_pc(key, scale, next) - key, 12), pr = pmod(root_pc(key, scale, prev) - key, 12);
    offer_home(&l, base);
    offer_warm(&l, base);
    for (int b = 1; b <= THEORY_NBORROW; b++) offer(&l, (feel_chord_t){theory_borrow_degree(scale, b), b, COL_TRIAD}, FEEL_COLOURFUL, FK_BORROWED);
    offer(&l, chroma(key, scale, nr + 7, CQ_DOM, COL_7TH), FEEL_TENSE, FK_LEANS);
    offer(&l, chroma(key, scale, nr + 11, CQ_DIM, COL_7TH), FEEL_TENSE, FK_CLIMBS);
    offer(&l, chroma(key, scale, nr + 1, CQ_DOM, COL_7TH), FEEL_OUTSIDE, FK_SLIDES);
    offer(&l, chroma(key, scale, pr + 4, is_minor(key, scale, prev) ? CQ_MINOR : CQ_MAJOR, COL_TRIAD), FEEL_OUTSIDE, FK_LIFT);
    return l.n;
}

int feel_level(int key, int scale, feel_chord_t cur, feel_chord_t base, feel_chord_t prev, feel_chord_t next, int *kind) {
    feel_option_t o[FEEL_OPTIONS_MAX];
    int n = feel_ladder(key, scale, base, prev, next, o), pcs[THEORY_CHORD_MAX];
    for (int i = 0; i < n; i++)
        if (feel_same(key, scale, o[i].c, cur)) {
            if (kind) *kind = o[i].kind;
            return o[i].level;
        }
    if (kind) *kind = FK_KEPT;
    return feel_out_of_key(key, scale, cur, pcs) ? FEEL_TENSE : FEEL_WARM;
}

int feel_shared(int key, int scale, feel_chord_t a, feel_chord_t b) {
    unsigned m = pc_mask(key, scale, a) & pc_mask(key, scale, b);
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

int feel_flow(int key, int scale, feel_chord_t a, feel_chord_t b) {
    int i = pmod(root_pc(key, scale, b) - root_pc(key, scale, a), 12), dom = is_dominant(key, scale, a);
    switch (i) {
    case 0: return FF_HOLDS;
    case 5: return dom ? FF_LANDS_HARD : FF_PULLS;
    case 11: return dom ? FF_SLIDES_IN : FF_FALLS;
    case 1: return is_dim7(key, scale, a) ? FF_CLIMBS_IN : FF_CLIMBS;
    case 7: return root_pc(key, scale, b) == key % 12 ? FF_SETTLES : FF_OPENS;
    case 2:
    case 10: return FF_STEPS;
    default: return feel_shared(key, scale, a, b) ? FF_SHARES : FF_FAR_APART;
    }
}

feel_chord_t feel_suggest(int key, int scale, feel_chord_t here, feel_chord_t next) {
    static const signed char WANT[3] = {7, 2, 10};
    int nr = root_pc(key, scale, next);
    for (int w = 0; w < 3; w++)
        for (int d = 0; d < 6; d++) {
            feel_chord_t c = {d, 0, COL_TRIAD};
            if (root_pc(key, scale, c) == (nr + WANT[w]) % 12 && !feel_same(key, scale, c, here) && !feel_same(key, scale, c, next)) return c;
        }
    for (int d = 0; d < 6; d++) {
        feel_chord_t c = {d, 0, COL_TRIAD};
        if (!feel_same(key, scale, c, here) && !feel_same(key, scale, c, next)) return c;
    }
    return (feel_chord_t){0, 0, COL_TRIAD};
}

int feel_add_target(const int *steps, int n, int sel) {
    int best = -1;
    if (sel >= 0 && sel < n && steps[sel] >= 8) return sel;
    for (int i = 0; i < n; i++)
        if (steps[i] >= 8 && (best < 0 || steps[i] > steps[best])) best = i;
    return best;
}

int feel_out_of_key(int key, int scale, feel_chord_t c, int pcs[THEORY_CHORD_MAX]) {
    int v[THEORY_CHORD_MAX], k = theory_chord_notes(key, theory_slot_scale(scale, c.src), c.deg, c.colour, v), n = 0;
    for (int i = 0; i < k; i++) {
        int pc = pmod(v[i], 12), in = 0;
        for (int d = 0; d < 7; d++) in |= theory_degree_pc(key, scale, d) == pc;
        if (!in) pcs[n++] = pc;
    }
    return n;
}

int feel_role_pc(int key, int scale, feel_chord_t c, int role) {
    return pmod(theory_tone_note(key, theory_slot_scale(scale, c.src), c.deg, theory_colour_tone(c.colour, role), 0), 12);
}

int feel_role_of(int key, int scale, feel_chord_t c, int note) {
    static const signed char ORDER[7] = {TONE_R, TONE_3, TONE_5, TONE_7, TONE_2, TONE_4, TONE_6};
    int pc = pmod(note, 12), s;
    for (int i = 0; i < 7; i++)
        if (feel_role_pc(key, scale, c, ORDER[i]) == pc) return ORDER[i];
    s = pmod(pc - root_pc(key, scale, c), 12);
    return s == 3 || s == 4 ? TONE_3 : s == 10 || s == 11 ? TONE_7 : TONE_5;   /* a note from outside the chord's scale: the job it is nearest */
}

int feel_muddy(const int *notes, int n) {
    for (int i = 0; i + 1 < n; i++)
        if (notes[i] < 52 && notes[i + 1] - notes[i] < 7) return i;
    return -1;
}

int feel_hand_word(const int *a, int na, const int *b, int nb) {
    int m = na < nb ? na : nb, d = 0;
    for (int i = 0; i < m; i++) {   /* from the top note down */
        int x = a[na - 1 - i] - b[nb - 1 - i];
        d += x < 0 ? -x : x;
    }
    if (m < 1) return FH_STAY;
    return d <= 2 * m ? FH_STAY : d <= 4 * m ? FH_MOVE : FH_JUMP;
}

const voicing_style_t *feel_style(int i) { return i >= 0 && i < VOICING_NSTYLES ? &VOICING_STYLES[i] : 0; }
int feel_styles(void) { return VOICING_NSTYLES; }

const char *feel_preset_name(int i) { return i >= 0 && i < THEORY_NPRESETS ? THEORY_PRESETS[i].name : ""; }
int feel_presets(void) { return THEORY_NPRESETS; }
