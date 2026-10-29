/* The melody recipe generator: see melody.h. A port of the canvas MG.bar / MG.line / MG.rhythm / MG.contour /
 * MG.transform, generalized from E minor and 4 one-bar chords to any key, scale, loop and chord length:
 * - "scale steps" are the project key and scale; Pentatonic uses five of its degrees (1 3 4 5 7 in a minor-third scale,
 *   1 2 3 5 6 in a major-third one), and Home note only moves the anchor.
 * - the seed picks note choices too (motif nudges, the Arp start, Guide and Approach ties and first notes), not only
 *   the rhythm, so every type's defaults vary with the seed.
 * - a chord (pitch classes) is voiced in root position, its root nearest 4 semitones below the midpoint of the line's
 *   previous note and the register centre (67, +-12 by REGISTER), so the line follows the chords without drifting.
 * - the 4-bar phrase repeats over the loop (role = bar mod 4); answer bars reuse their call's cell (rhythm and contour).
 *   VST-12: the phrase's Form sets the roles (Loop has none: every bar is a call with its own cell), Breath the rest at a
 *   call's end, Cadence how phrases end (the loop's last note to the tonic; Q & A also ends each call on its chord's 5th
 *   and each earlier answer on the tonic) and Fill a pickup note in the breath, a scale step below the next bar's first.
 * - a bar's chord is the one sounding at its first step; Arp and Guide tones re-read it at each span start, Approach at
 *   each target.
 * Notes fold by octaves into 50..96. The moves (rev, inv, shift) are applied after generation, in that order. */
#include "melody.h"
#include "gen.h"
#include "theory.h"
#include "util.h"
#include <string.h>

#define LO 50
#define HI 96
#define CENTRE 67
#define NONE (-1)

typedef struct { const char *name; int n; const char *opt[4]; int def; } mel_row_t;
typedef struct { mel_row_t row[3]; unsigned char kn[1 + MK_COUNT]; } mel_type_t;   /* kn[0]: COMPLEXITY */

const char *const MEL_TYPE_NAME[MEL_NTYPES] = {"Arp", "Ostinato", "Pentatonic", "Hook", "Sequence", "Guide tones", "Approach"};

/* Saved as indices: append only. */
static const mel_type_t TYPES[MEL_NTYPES] = {
    {{{"Direction", 4, {"Up", "Down", "Up-down", "Converge"}, 0}, {"Rate", 3, {"1/4", "1/8", "1/16"}, 1},
      {"Octaves", 2, {"1", "2"}, 0}}, {30, 40, 20, 10, 10, 30, 50, 40}},
    {{{"Riff notes", 4, {"2", "3", "4", "5"}, 2}, {"Over chords", 2, {"Hold", "Shift"}, 0}, {"Rate", 2, {"1/8", "1/16"}, 0}},
     {40, 25, 15, 40, 20, 20, 50, 50}},
    {{{"Home note", 2, {"minor", "major"}, 0}, {"Shape", 3, {"Rise", "Fall", "Wave"}, 2}, {"Rate", 2, {"1/8", "1/16"}, 0}},
     {50, 50, 35, 45, 20, 50, 60, 60}},
    {{{"Motif", 3, {"3 notes", "4 notes", "5 notes"}, 1}, {"Contour", 4, {"Rise", "Fall", "Arch", "Valley"}, 2},
      {"Starts on", 3, {"Root", "3rd", "5th"}, 2}}, {40, 45, 30, 35, 25, 50, 55, 70}},
    {{{"Motif", 3, {"2 notes", "3 notes", "4 notes"}, 1}, {"Each bar", 3, {"Step down", "Step up", "3rd up"}, 0},
      {"Contour", 2, {"Rise", "Fall"}, 0}}, {45, 50, 25, 25, 25, 40, 55, 60}},
    {{{"Tones", 2, {"3rd + 7th", "3rd only"}, 0}, {"Rhythm", 3, {"Whole", "Half", "Push"}, 1},
      {"Motion", 2, {"Smooth", "Falling"}, 0}}, {20, 20, 10, 15, 40, 30, 45, 90}},
    {{{"From", 3, {"Below", "Above", "Both"}, 0}, {"Step", 2, {"Scale", "Chromatic"}, 0},
      {"Lands on", 3, {"Root", "3rd", "Any tone"}, 1}}, {50, 40, 20, 40, 60, 60, 50, 50}},
};

/* Forms, per bar role (bar mod 4): an answer bar, the call whose cell a bar makes (an answer: its call's), a call's last
 * bar (the breath and Q & A's open end). 1 + 1 and 2 + 2 are the canvas PHRASES. */
static const unsigned char FORM_ANS[MEL_NFORMS][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 1, 1}, {0, 0, 0, 1}};
static const unsigned char FORM_SRC[MEL_NFORMS][4] = {{0, 1, 2, 3}, {0, 0, 2, 2}, {0, 1, 0, 1}, {0, 1, 2, 2}};
static const unsigned char FORM_END[MEL_NFORMS][4] = {{0, 0, 0, 0}, {1, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
static const unsigned char BREATH_STEPS[MEL_NBREATHS] = {0, 2, 4, 8};   /* 16ths rested at a call's end */
static const unsigned char PH_DEFAULT[MEL_NTYPES][MP_COUNT] = {   /* each type's phrasing (mel_defaults) */
    [MEL_ARP] = {MEL_FORM_LOOP, MEL_BREATH_NONE, MEL_CAD_NONE, MEL_FILL_REST},
    [MEL_OST] = {MEL_FORM_LOOP, MEL_BREATH_NONE, MEL_CAD_NONE, MEL_FILL_REST},
    [MEL_PENT] = {MEL_FORM_2_2, MEL_BREATH_BEAT, MEL_CAD_LAND, MEL_FILL_REST},
    [MEL_HOOK] = {MEL_FORM_1_1, MEL_BREATH_BEAT, MEL_CAD_QA, MEL_FILL_REST},
    [MEL_SEQ] = {MEL_FORM_3_1, MEL_BREATH_BEAT, MEL_CAD_QA, MEL_FILL_REST},
    [MEL_GUIDE] = {MEL_FORM_LOOP, MEL_BREATH_NONE, MEL_CAD_LAND, MEL_FILL_REST},
    [MEL_APPR] = {MEL_FORM_1_1, MEL_BREATH_8TH, MEL_CAD_LAND, MEL_FILL_PICKUP}};
static const char *const PH_NAMES[MP_COUNT][4] = {{"Loop", "1 + 1", "2 + 2", "3 + 1"}, {"None", "1/8", "Beat", "Half"},
                                                  {"None", "Land", "Q & A", ""}, {"Rest", "Pickup", "", ""}};
static const int PH_N[MP_COUNT] = {MEL_NFORMS, MEL_NBREATHS, MEL_NCADS, MEL_NFILLS};

static int valid_row(int type, int which) { return type >= 0 && type < MEL_NTYPES && which >= 0 && which < 3; }

int mel_nopts(int type, int which) { return valid_row(type, which) ? TYPES[type].row[which].n : 0; }

const char *mel_row_name(int type, int which) { return valid_row(type, which) ? TYPES[type].row[which].name : ""; }

const char *mel_opt_name(int type, int which, int i) {
    return i >= 0 && i < mel_nopts(type, which) ? TYPES[type].row[which].opt[i] : "";
}

int mel_phrase_n(int which) { return which >= 0 && which < MP_COUNT ? PH_N[which] : 0; }

const char *mel_phrase_name(int which, int i) { return i >= 0 && i < mel_phrase_n(which) ? PH_NAMES[which][i] : ""; }

void mel_phrase_from_legacy(mel_recipe_t *r, int phrase) {
    r->ph[MP_FORM] = phrase == MEL_PHRASE_2_2 ? MEL_FORM_2_2 : MEL_FORM_1_1;
    r->ph[MP_BREATH] = MEL_BREATH_BEAT;
    r->ph[MP_CADENCE] = MEL_CAD_LAND;
    r->ph[MP_FILL] = MEL_FILL_REST;
}

int mel_legacy_phrase(const mel_recipe_t *r) { return r->ph[MP_FORM] == MEL_FORM_2_2 ? MEL_PHRASE_2_2 : MEL_PHRASE_1_1; }

int mel_is_legacy(const mel_recipe_t *r) {
    mel_recipe_t x = *r;
    mel_phrase_from_legacy(&x, mel_legacy_phrase(r));
    return !memcmp(x.ph, r->ph, sizeof x.ph);
}

void mel_defaults(int type, mel_recipe_t *r, int *complexity) {
    if (type < 0 || type >= MEL_NTYPES || !r) return;
    const mel_type_t *t = &TYPES[type];
    r->type = (unsigned char)type;
    for (int w = 0; w < 3; w++) r->opt[w] = (unsigned char)t->row[w].def;
    memcpy(r->ph, PH_DEFAULT[type], sizeof r->ph);
    memcpy(r->kn, t->kn + 1, MK_COUNT);
    r->rev = r->inv = r->shift = 0;
    if (complexity) *complexity = t->kn[0];
}

/* ---- pitch helpers ---- */

static int fold(int m) {
    while (m < LO) m += 12;
    while (m > HI) m -= 12;
    return m;
}

/* A scale as an unbounded ladder of steps: step i is the note key + 12 * (i div n) + iv[i mod n]. */
typedef struct { int key, n, iv[7]; } ladder_t;

static int lad_note(const ladder_t *l, int i) { return l->key + 12 * fdiv(i, l->n) + l->iv[pmod(i, l->n)]; }

/* the step nearest note m (a chromatic note snaps; ties go to the lower step) */
static int lad_idx(const ladder_t *l, int m) {
    int i = fdiv(m - l->key, 12) * l->n, best = i - 1;   /* step i - 1 is below m, step i + n above */
    for (int j = i; j <= i + l->n; j++)
        if (iabs(lad_note(l, j) - m) < iabs(lad_note(l, best) - m)) best = j;
    return best;
}

/* the note `by` steps from the step nearest m */
static int lad_move(const ladder_t *l, int m, int by) { return lad_note(l, lad_idx(l, m) + by); }

static int nearest_in(const int *list, int n, int m) {   /* ties: the earlier */
    int best = list[0];
    for (int i = 1; i < n; i++) if (iabs(list[i] - m) < iabs(best - m)) best = list[i];
    return best;
}

static int nearest_pick(const int *list, int n, int m, unsigned h) {   /* ties: the seed picks */
    int best = iabs(nearest_in(list, n, m) - m), k = 0, ties[8];
    for (int i = 0; i < n && k < 8; i++) if (iabs(list[i] - m) == best) ties[k++] = list[i];
    return ties[h % (unsigned)k];
}

/* ---- the generation context ---- */

typedef struct {
    const mel_params_t *p;
    int type, o[3], kn[MK_COUNT], cx, form, breath, cadence, fill, L, cl, key;
    int novar;               /* 0.16: generate every bar as the phrase's (bar_var ignored) */
    int vtry;                /* 0.16: a bar whose variation came out as the phrase's tries again with this added */
    int reg, centre, home;   /* REGISTER's octave; the register centre; the home note in 60..71 (the tonic), plus reg */
    ladder_t sc, ps;         /* the key's scale; Pentatonic's five notes */
} ctx_t;

/* Pentatonic: five degrees of the project scale, by its third (0-based degrees); Home note picks the anchor degree */
static const int PENT_DEG[2][5] = {{0, 2, 3, 4, 6}, {0, 1, 2, 4, 5}};   /* a minor-third scale, a major-third one */
static const int PENT_HOME[2][2] = {{0, 2}, {5, 0}};                     /* [major third][minor, major] */

static void ctx_ladders(ctx_t *c, int scale) {
    c->sc.key = c->ps.key = c->key;
    c->sc.n = 7;
    for (int d = 0; d < 7; d++) c->sc.iv[d] = pmod(theory_degree_pc(c->key, scale, d) - c->key, 12);
    int major = c->sc.iv[2] == 4;
    c->ps.n = 5;
    for (int i = 0; i < 5; i++) c->ps.iv[i] = c->sc.iv[PENT_DEG[major][i]];
    if (c->type == MEL_PENT) c->home = 60 + pmod(c->key + c->sc.iv[PENT_HOME[major][c->o[0]]], 12) + c->reg;
}

/* The seed's note choices: a hash of the seed, a salt and a place, so each choice is fixed and independent. */
enum { SALT_NUDGE = 1, SALT_ARP, SALT_GUIDE, SALT_APPR };
static unsigned pick(const ctx_t *c, int salt, int where) {
    return fmix32((unsigned)c->p->seed * 0x9E3779B1u ^ fmix32((unsigned)salt * 0x27D4EB2Fu + (unsigned)where * 0x165667B1u));
}

/* 0.16: bar b's own variation (NEW BAR), 0 for the phrase's bar */
static int bar_var(const ctx_t *c, int b) {
    int v = !c->novar && c->p->bar_var && b >= 0 && b < SEQ_MAX_BARS ? c->p->bar_var[b] : 0;
    return v ? v + 37 * c->vtry : 0;
}

/* pick at a place in bar b: a bar with its own variation picks with a salt of its own */
static unsigned pick_in(const ctx_t *c, int salt, int b, int where) { return pick(c, salt + 16 * bar_var(c, b), where); }

static int ctx_init(ctx_t *c, const mel_params_t *p) {
    const mel_recipe_t *r = p->r;
    if (r->type >= MEL_NTYPES) return 0;
    c->p = p;
    c->novar = c->vtry = 0;
    c->type = r->type;
    for (int w = 0; w < 3; w++) c->o[w] = r->opt[w] < mel_nopts(c->type, w) ? r->opt[w] : mel_nopts(c->type, w) - 1;
    for (int k = 0; k < MK_COUNT; k++) c->kn[k] = r->kn[k] > 100 ? 100 : r->kn[k];
    c->cx = clampi(p->complexity, 0, 100);
    c->form = r->ph[MP_FORM] < MEL_NFORMS ? r->ph[MP_FORM] : MEL_FORM_LOOP;
    c->breath = r->ph[MP_BREATH] < MEL_NBREATHS ? BREATH_STEPS[r->ph[MP_BREATH]] : 0;
    c->cadence = r->ph[MP_CADENCE] < MEL_NCADS ? r->ph[MP_CADENCE] : MEL_CAD_NONE;
    c->fill = r->ph[MP_FILL] == MEL_FILL_PICKUP;
    c->L = clampi(p->bars, 1, SEQ_MAX_BARS) * SEQ_STEPS;
    c->cl = p->chord_len < 1 ? SEQ_STEPS : p->chord_len;
    c->key = pmod(p->key, 12);
    c->reg = c->kn[MK_REGISTER] >= 75 ? 12 : c->kn[MK_REGISTER] <= 25 ? -12 : 0;
    c->centre = CENTRE + c->reg;
    c->home = 60 + c->key + c->reg;
    ctx_ladders(c, clampi(p->scale, 0, SCALE_COUNT - 1));
    return 1;
}

/* a chord voiced in root position: t[0] the root, then each tone the first above the one before */
typedef struct { int t[4], n, sev; } voiced_t;

static voiced_t voice_at(const ctx_t *c, int step, int prev) {
    const mel_chord_t *ch = &c->p->chord[pmod(step, c->L) / c->cl];
    int target = (prev + c->centre) / 2 - 4, root = pmod(ch->root_pc, 12);
    int r = root + 12 * fdiv(target - root, 12);   /* at or below target */
    voiced_t v = {{r + 12 * (target - r > r + 12 - target)}, clampi(ch->ntones, 1, 4), 0};
    for (int i = 1; i < v.n; i++) v.t[i] = v.t[i - 1] + pmod(ch->tones[i] - v.t[i - 1], 12);
    v.sev = v.t[0] + pmod(ch->seventh - root, 12);
    return v;
}

static int tone(const voiced_t *v, int i) { return v->t[i < v->n ? i : v->n - 1]; }

static int answer_flips(const ctx_t *c, int answer) { return answer && c->kn[MK_ANSWER] >= 34; }

/* ---- Arp: the chord tones in a pattern ---- */

static void reverse_ints(int *a, int n) {
    for (int i = 0; i < n / 2; i++) { int x = a[i]; a[i] = a[n - 1 - i]; a[n - 1 - i] = x; }
}

static int arp_tones(const ctx_t *c, const voiced_t *v, int *t) {
    int n = 0, has_sev = 0;
    for (int i = 0; i < v->n; i++) { t[n++] = v->t[i]; has_sev |= pmod(v->t[i] - v->sev, 12) == 0; }
    if (c->cx >= 60 && !has_sev) t[n++] = v->sev;
    sort_ints(t, n);
    if (c->o[2] == 1) for (int i = 0; i < n; i++) t[n + i] = t[i] + 12;
    return c->o[2] == 1 ? 2 * n : n;
}

static int arp_seq(const ctx_t *c, const voiced_t *v, int flip, int *seq) {
    int t[10], n = arp_tones(c, v, t), len = 0;
    if (c->o[0] == 0 || c->o[0] == 2) for (int i = 0; i < n; i++) seq[len++] = t[i];
    if (c->o[0] == 1) for (int i = n - 1; i >= 0; i--) seq[len++] = t[i];
    if (c->o[0] == 2) for (int i = n - 2; i >= 1; i--) seq[len++] = t[i];
    if (c->o[0] == 3) for (int i = 0; i < n; i++) seq[len++] = i % 2 ? t[n - 1 - (i >> 1)] : t[i >> 1];
    if (flip) reverse_ints(seq, len);
    return len;
}

/* The pattern runs on through the bar; a new chord (span start) re-voices it near the last note and the seed picks
 * where in the pattern it starts. */
static void bar_arp(const ctx_t *c, int b, int answer, int prev, int *out) {
    static const int RATE[3] = {4, 2, 1};
    int seq[20], len = 0, span = -1, last = prev;
    unsigned start = 0;
    for (int s = 0, i = 0; s < SEQ_STEPS; s += RATE[c->o[1]], i++) {
        int step = b * SEQ_STEPS + s;
        if (step / c->cl != span) {
            voiced_t v = voice_at(c, step, last);
            len = arp_seq(c, &v, answer_flips(c, answer), seq);
            span = step / c->cl;
            start = pick(c, SALT_ARP, span);   /* each chord's own seed-picked start */
            if (bar_var(c, b) && len > 1) start = start % (unsigned)len + 1u + (unsigned)(bar_var(c, b) - 1) % (unsigned)(len - 1);   /* 0.16 */
        }
        last = out[s] = seq[(start + (unsigned)i) % (unsigned)len];
    }
}

/* ---- Guide tones: the 3rd and 7th of each chord, moving smoothly ---- */

/* the octave of m within 6 of prev; a tritone away both fit, and bit `bit` of h picks */
static int fit(int m, int prev, unsigned h, int bit) {
    while (m - prev > 6) m -= 12;
    while (prev - m > 6) m += 12;
    return m - prev == 6 && (h >> bit & 1u) ? m - 12 : m;
}

/* The loop's first pair has no note before it: the seed picks which tone leads and its octave. */
static void guide_pair(const ctx_t *c, const voiced_t *v, int prev, int step, int *first, int *second) {
    unsigned h = pick_in(c, SALT_GUIDE, step / SEQ_STEPS, step);
    int third = fit(tone(v, 1), prev, h, 1), sev = fit(v->sev, prev, h, 2);
    int dt = iabs(third - prev), ds = iabs(sev - prev), lead = step == 0 || dt == ds ? (int)(h & 1u) : ds < dt;
    int f = c->o[0] == 1 ? third : lead ? sev : third;
    if (c->o[2] == 1 && c->o[0] == 0 && sev < f) f = sev;
    *first = f;
    *second = c->o[0] == 1 ? third : f == third ? sev : third;
    if (bar_var(c, step / SEQ_STEPS)) {   /* 0.16: a bar of its own leads with the other tone */
        if (*first != *second) { int x = *first; *first = *second; *second = x; }
        else *first = *second = f + (f > c->centre ? -12 : 12);
    }
    if (step == 0 && (h >> 3 & 1u)) {   /* the first pair an octave toward the centre's other side */
        int d = f > prev ? -12 : 12;
        *first += d, *second += d;
    }
}

static unsigned guide_onsets(const ctx_t *c, int b) {
    unsigned m = 1u | (c->o[1] == 1 ? 1u << 8 : 0) | (c->o[1] == 2 ? 1u << 14 : 0) | (c->cx >= 70 ? 0x1010u : 0);
    for (int s = 1; s < SEQ_STEPS; s++) if ((b * SEQ_STEPS + s) % c->cl == 0) m |= 1u << s;   /* each new chord */
    return m;
}

/* Onsets from step 8 of a chord that started before it play the second tone; Push anticipates the next bar's 3rd. */
static void bar_guide(const ctx_t *c, int b, int prev, int *out) {
    unsigned m = guide_onsets(c, b);
    int first = prev, second = prev, seg = 0, seg_prev = prev, cur = prev;
    for (int s = 0; s < SEQ_STEPS; s++) {
        int step = b * SEQ_STEPS + s;
        if (!(m >> s & 1u)) continue;
        if (s == 0 || step % c->cl == 0) {
            voiced_t v = voice_at(c, step, cur);
            guide_pair(c, &v, cur, step, &first, &second);
            seg = s, seg_prev = cur;
        }
        if (s == 14 && c->o[1] == 2) {
            voiced_t n = voice_at(c, (b + 1) * SEQ_STEPS, seg_prev);
            out[s] = fit(tone(&n, 1), seg_prev, pick_in(c, SALT_GUIDE, b, step), 1);
        } else
            out[s] = s >= 8 && seg < 8 ? second : first;
        cur = out[s];
    }
}

/* ---- Approach: lands on chord tones from a step away ---- */

static int appr_pool(const ctx_t *c, const voiced_t *v, int *pool) {
    int n = 0;
    if (c->o[2] == 0) { pool[n++] = v->t[0]; pool[n++] = v->t[0] + 12; }
    else if (c->o[2] == 1) { pool[n++] = tone(v, 1); pool[n++] = tone(v, 1) - 12; pool[n++] = tone(v, 1) + 12; }
    else {
        for (int i = 0; i < v->n; i++) pool[n++] = v->t[i];
        for (int i = 0; i < v->n; i++) pool[n++] = v->t[i] + 12;
    }
    return n;
}

/* Each target re-reads the chord at its step. The seed picks among equally near tones, the first target (with no note
 * before it: any pool tone within an octave) and which side "Both" starts from. */
static int appr_target(const ctx_t *c, int step, int cur, int flip) {
    voiced_t v = voice_at(c, step, cur);
    int pool[8], np = appr_pool(c, &v, pool), near[8], k = 0;
    unsigned h = pick_in(c, SALT_APPR, step / SEQ_STEPS, step);
    if (step) return nearest_pick(pool, np, cur + (flip ? -2 : 2), h);
    for (int i = 0; i < np; i++) if (iabs(pool[i] - cur) <= 12) near[k++] = pool[i];
    return k ? near[h % (unsigned)k] : nearest_in(pool, np, cur);
}

static void bar_appr(const ctx_t *c, int b, int answer, int prev, int *out) {
    int ntg = c->cx < 35 ? 2 : 4, cur = prev, both = (int)(pick_in(c, SALT_APPR, b, -1) & 1u);
    int chroma = c->o[1] == 1 || c->kn[MK_TENSION] >= 67, flip = answer_flips(c, answer);
    for (int j = 0; j < ntg; j++) {
        int s = j * (SEQ_STEPS / ntg), tgt = appr_target(c, b * SEQ_STEPS + s, cur, flip);
        int from = c->o[0] == 2 ? ((j + both) % 2 ? 1 : -1) : c->o[0] == 0 ? -1 : 1;
        if (bar_var(c, b) % 2) from = -from;   /* 0.16: a bar of its own approaches from the other side */
        if (s > 0) out[s - 1] = chroma ? tgt + from : lad_move(&c->sc, tgt, from);
        out[s] = cur = tgt;
    }
}

/* ---- Motif types (Ostinato, Pentatonic, Hook, Sequence): a rhythm and a contour on a scale ---- */

typedef struct { int n, steps[8], offs[8]; } cell_t;

/* Onsets by tier (downbeat, then weaker); each tier shuffled by the seed. The order never depends on n, so a higher
 * COMPLEXITY keeps every onset a lower one had. */
static int rhythm(unsigned seed, int n, int sync, int grid, int *out) {
    static const int ON[16] = {0, 8, 4, 12, 2, 6, 10, 14, 3, 7, 11, 15, 1, 5, 9, 13}, ON_T[] = {1, 1, 2, 4, 8, 0};
    static const int OFF[16] = {0, 6, 14, 10, 8, 4, 12, 2, 3, 7, 11, 15, 1, 5, 9, 13}, OFF_T[] = {1, 3, 3, 9, 0};
    const int *tiers = sync >= 60 ? OFF_T : ON_T;
    int order[16], k = 0;
    gen_rng r;
    gen_seed(&r, seed);
    memcpy(order, sync >= 60 ? OFF : ON, sizeof order);
    for (int a = 0; tiers[0]; a += *tiers++)
        for (int i = tiers[0] - 1; i > 0; i--) {
            int j = (int)(gen_next(&r) % (unsigned)(i + 1)), x = order[a + i];
            order[a + i] = order[a + j], order[a + j] = x;
        }
    for (int i = 0; i < 16 && k < n; i++) if (order[i] % grid == 0) out[k++] = order[i];
    sort_ints(out, k);
    return k;
}

static int contour(int kind, int len, int leaps, int range, int *out) {   /* kind: Rise, Fall, Arch, Valley */
    static const int BASE[4][5] = {{0, 1, 2, 3, 4}, {0, -1, -2, -3, -4}, {0, 1, 2, 1, 0}, {0, -1, -2, -1, 0}};
    static const int BASE3[4][3] = {{0, 1, 2}, {0, -1, -2}, {0, 2, 1}, {0, -2, -1}};
    for (int i = 0; i < len; i++) {
        int v = (len == 3 ? BASE3[kind][i] : BASE[kind][i]) * (leaps >= 67 ? 2 : 1);
        out[i] = range <= 33 ? fdiv(v + 1, 2) : v;   /* the canvas Math.round(v / 2) */
    }
    return len;
}

static void cell_offs(const ctx_t *c, cell_t *cell) {
    static const int RIFF[5] = {0, 2, 1, 3, 4};
    static const int SHAPE[3][8] = {{0, 1, 2, 3, 4, 5, 6, 7}, {0, -1, -2, -3, -4, -5, -6, -7}, {0, 1, 2, 1, 0, -1, -2, -1}};
    int ct[5], len = 1;
    if (c->type == MEL_HOOK) len = contour(c->o[1], c->o[0] + 3, c->kn[MK_LEAPS], c->kn[MK_RANGE], ct);
    if (c->type == MEL_SEQ) len = contour(c->o[2], c->o[0] + 2, c->kn[MK_LEAPS], c->kn[MK_RANGE], ct);
    for (int i = 0; i < cell->n; i++)
        cell->offs[i] = c->type == MEL_OST ? RIFF[i % (c->o[0] + 2)] : c->type == MEL_PENT ? SHAPE[c->o[1]][i % 8] : ct[i % len];
}

/* The seed nudges about a third of the onsets after the first by one scale step, keyed by the onset's step, so an
 * onset keeps its nudge as COMPLEXITY adds others. Part of the cell: an answer reuses its call's. */
static void cell_nudge(const ctx_t *c, int src, int v, cell_t *cell) {
    for (int i = 1; i < cell->n; i++) {
        unsigned h = pick(c, SALT_NUDGE + 16 * v, src * SEQ_STEPS + cell->steps[i]);
        if (h % 3u == 0) cell->offs[i] += h >> 8 & 1u ? 1 : -1;
    }
}

/* The cell of the call bar with role src (its rhythm seed mixes the seed, the role and its call index): an answer bar
 * makes its call's cell again, exactly. 0.16: v, a bar's own variation (NEW BAR), mixes in a cell of its own. */
static void make_cell(const ctx_t *c, int src, int v, cell_t *cell) {
    int k = c->form == MEL_FORM_1_1 ? src / 2 : src, n = 2 + (c->cx * 6 + 50) / 100;   /* 1 + 1's call index (0.11) */
    int grid = (c->type == MEL_OST || c->type == MEL_PENT) && c->o[2] == 0 ? 2 : 1;
    unsigned seed = fmix32((unsigned)c->p->seed * 0x9E3779B1u + (unsigned)(src * 4 + k + 1) * 0x85EBCA6Bu);
    if (v) seed = fmix32(seed ^ (unsigned)v * 0xC2B2AE35u);
    cell->n = rhythm(seed, n, c->kn[MK_SYNC], grid, cell->steps);
    cell_offs(c, cell);
    cell_nudge(c, src, v, cell);
}

static int motif_anchor(const ctx_t *c, int b, int prev) {
    static const int EACH[3] = {-1, 1, 2};   /* Step down, Step up, 3rd up */
    voiced_t v = voice_at(c, b * SEQ_STEPS, prev);
    if (c->type == MEL_OST) return c->o[1] == 0 ? c->home : nearest_in(v.t, v.n, c->home);
    if (c->type == MEL_PENT) return c->home;
    if (c->type == MEL_HOOK) return tone(&v, c->o[2]);
    return lad_move(&c->sc, c->home, 4 + (b % 4) * EACH[c->o[1]]);   /* Sequence: from the 5th, moved per bar */
}

static void bar_motif(const ctx_t *c, int b, int answer, int prev, int *out) {
    cell_t cell;
    make_cell(c, FORM_SRC[c->form][b % 4], bar_var(c, b), &cell);
    if (answer && c->kn[MK_ANSWER] >= 67) {
        for (int i = 0; i < cell.n; i++) cell.steps[i] = (cell.steps[i] + 2) % SEQ_STEPS;
        sort_ints(cell.steps, cell.n);
    }
    int flip = answer_flips(c, answer) ? -1 : 1, anchor = fold(motif_anchor(c, b, prev));   /* the motif keeps its contour */
    const ladder_t *l = c->type == MEL_PENT ? &c->ps : &c->sc;
    for (int i = 0; i < cell.n; i++) out[cell.steps[i]] = lad_move(l, anchor, flip * cell.offs[i]);
}

/* ---- the line ---- */

static void gen_bar(const ctx_t *c, int b, int prev, int *out) {
    int answer = FORM_ANS[c->form][b % 4];
    for (int s = 0; s < SEQ_STEPS; s++) out[s] = NONE;
    if (c->type == MEL_ARP) bar_arp(c, b, answer, prev, out);
    else if (c->type == MEL_GUIDE) bar_guide(c, b, prev, out);
    else if (c->type == MEL_APPR) bar_appr(c, b, answer, prev, out);
    else bar_motif(c, b, answer, prev, out);
}

/* 0.16: does a varied bar sound like the phrase's? Compared as played (before the rest, folded), but for its last note,
 * which a Cadence may retune */
static int sounds_same(const int *a, const int *b, int rest) {
    int la = -1, lb = -1;
    for (int s = 0; s < rest; s++) la = a[s] != NONE ? s : la, lb = b[s] != NONE ? s : lb;
    if (la != lb) return 0;
    for (int s = 0; s < rest; s++) {
        if (s == la) continue;
        if ((a[s] == NONE) != (b[s] == NONE) || (a[s] != NONE && fold(a[s]) != fold(b[s]))) return 0;
    }
    return 1;
}

/* The bar's last note as the line plays it (rest applied, folded), or prev when it has none */
static int bar_end(const int *out, int rest, int prev) {
    for (int s = 0; s < rest; s++) if (out[s] != NONE) prev = fold(out[s]);
    return prev;
}

/* 0.16: a bar with its own variation is made, but the next bar starts from where the phrase's bar would have ended, so
 * NEW BAR changes only its bar */
static void gen_line(const ctx_t *c, int *notes) {
    ctx_t phrase = *c;
    int prev = c->centre;
    phrase.novar = 1;
    for (int b = 0; b < c->L / SEQ_STEPS; b++) {
        int out[SEQ_STEPS], rest = FORM_END[c->form][b % 4] ? SEQ_STEPS - c->breath : SEQ_STEPS, next;
        gen_bar(c, b, prev, out);
        next = bar_end(out, rest, prev);
        if (bar_var(c, b)) {   /* the phrase's bar: where the next bar starts, and what the variation must differ from */
            int ph[SEQ_STEPS];
            ctx_t again = *c;
            gen_bar(&phrase, b, prev, ph);
            next = bar_end(ph, rest, prev);
            for (again.vtry = 1; again.vtry < 8 && sounds_same(out, ph, rest); again.vtry++) gen_bar(&again, b, prev, out);
        }
        for (int s = 0; s < SEQ_STEPS; s++) notes[b * SEQ_STEPS + s] = out[s] == NONE || s >= rest ? NONE : fold(out[s]);
        prev = next;
    }
}

static int last_onset(const ctx_t *c, const int *notes) {
    for (int i = c->L - 1; i >= 0; i--) if (notes[i] != NONE) return i;
    return -1;
}

static int to_pc(int m, int pc) {   /* the note of pitch class pc nearest m (ties: lower) */
    int down = m - pmod(m - pc, 12);
    return fold(m - down <= down + 12 - m ? down : down + 12);
}

static int bar_last(const int *notes, int b) {   /* bar b's last onset, -1 none */
    for (int i = b * SEQ_STEPS + SEQ_STEPS - 1; i >= b * SEQ_STEPS; i--) if (notes[i] != NONE) return i;
    return -1;
}

/* Cadence: Land, the loop's last note to the nearest tonic. Q & A (not on a Loop) also ends each call on its chord's 5th
 * and each answer before the loop's end on the tonic. */
static void cadence(const ctx_t *c, int *notes) {
    int bars = c->L / SEQ_STEPS, l = last_onset(c, notes);
    for (int b = 0; c->cadence == MEL_CAD_QA && b < bars; b++) {
        int r = b % 4, i = bar_last(notes, b);
        if (i < 0 || i == l) continue;
        if (FORM_END[c->form][r]) notes[i] = to_pc(notes[i], c->p->chord[i / c->cl].tones[2]);
        else if (FORM_ANS[c->form][r] && (r == 3 || !FORM_ANS[c->form][r + 1])) notes[i] = to_pc(notes[i], c->key);
    }
    if (c->cadence != MEL_CAD_NONE && l >= 0) notes[l] = to_pc(notes[l], c->key);
}

/* Fill: a pickup on the breath's 16th 15, a scale step below the next bar's first note (the loop wraps). A call ending
 * the loop with a Cadence keeps its landing: no pickup there. */
static void fill_pickups(const ctx_t *c, int *notes) {
    int bars = c->L / SEQ_STEPS;
    for (int b = 0; c->fill && c->breath && b < bars; b++) {
        if (!FORM_END[c->form][b % 4] || (b == bars - 1 && c->cadence != MEL_CAD_NONE)) continue;
        int nb = (b + 1) % bars * SEQ_STEPS;
        for (int i = nb; i < nb + SEQ_STEPS; i++)
            if (notes[i] != NONE) { notes[b * SEQ_STEPS + 14] = fold(lad_move(&c->sc, notes[i], -1)); break; }
    }
}

/* ---- moves ---- */

static void move_rev(const ctx_t *c, int *notes) {   /* each bar's pitches reversed, its rhythm kept */
    for (int b = 0; b < c->L; b += SEQ_STEPS)
        for (int i = b, j = b + SEQ_STEPS - 1; ; i++, j--) {
            while (i < j && notes[i] == NONE) i++;
            while (i < j && notes[j] == NONE) j--;
            if (i >= j) break;
            int x = notes[i]; notes[i] = notes[j]; notes[j] = x;
        }
}

static void move_inv(const ctx_t *c, int *notes) {   /* mirrored in scale steps around the first note */
    int f = 0;
    while (f < c->L && notes[f] == NONE) f++;
    if (f == c->L) return;
    int i0 = lad_idx(&c->sc, notes[f]);
    for (int i = f; i < c->L; i++)
        if (notes[i] != NONE) notes[i] = fold(lad_note(&c->sc, 2 * i0 - lad_idx(&c->sc, notes[i])));
}

static void move_shift(const ctx_t *c, int *notes, int k) {   /* the loop rotated right by k 16ths */
    int tmp[SEQ_LANE_STEPS];
    memcpy(tmp, notes, c->L * sizeof tmp[0]);
    for (int i = 0; i < c->L; i++) notes[i] = tmp[pmod(i - k, c->L)];
}

static void write_steps(const ctx_t *c, const int *notes, seq_step_t *st) {
    for (int i = 0; i < c->L; i++) {
        int d = 1;
        if (notes[i] == NONE) { st[i] = (seq_step_t){0, TONE_R, 0, 100, 1, 0}; continue; }
        while (d < 4 && notes[(i + d) % c->L] == NONE) d++;
        st[i] = (seq_step_t){1, TONE_R, 0, (unsigned char)(i % 4 ? 90 : 100), (unsigned char)d, (unsigned char)notes[i]};
    }
}

void mel_generate_v1(const mel_params_t *p, seq_step_t *steps) {
    ctx_t c;
    int notes[SEQ_LANE_STEPS];
    if (!p || !p->r || !p->chord || !steps || !ctx_init(&c, p)) return;
    gen_line(&c, notes);
    cadence(&c, notes);
    fill_pickups(&c, notes);
    if (p->r->rev) move_rev(&c, notes);
    if (p->r->inv) move_inv(&c, notes);
    if (p->r->shift % SEQ_STEPS) move_shift(&c, notes, p->r->shift % SEQ_STEPS);
    write_steps(&c, notes, steps);
}
