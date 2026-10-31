#include "seq_core.h"
#include "gen.h"
#include "jamlib.h"
#include "melody.h"
#include "rhythm.h"
#include "theory.h"
#include "util.h"
#include "voicing.h"
#include <math.h>
#include <pthread.h>   /* VST-33: the lookup tables' one build (pthread_once) */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GATE_TAIL 0.025   /* a note ends 10% of a 1/16 before its last step ends: one step = 0.225 beats, as in v1 */
#define VELOCITY 100
#define EPS 1e-9

static const int V1_PROG[4] = {0, 5, 2, 6};   /* E minor i-VI-III-VII: Em C G D */
static const unsigned char ARP[3] = {TONE_R, TONE_3, TONE_5};
static const signed char LANE_OCT[SEQ_LANES] = {0, -1, 1, 2};   /* v1's octave per lane */
static const seq_gen_t MANUAL_GEN = {GEN_MANUAL, 1, 30, 0};       /* a style chosen later starts like a new instance's */
/* Each lane's neutral SHAPE row: the Chords 0.8's sound, the Bass 0.9's, Melody unused */
static const int LSHAPE_NEUTRAL[SEQ_ACTIVE_LANES][LSHAPE_COUNT] = {{0, 50, 0, 50, 0, 50, 0}, {0, 0, 0, 50, 0, 50, 50, 0, 0}, {0}};
static const int LSHAPE_N[SEQ_ACTIVE_LANES] = {SHAPE_COUNT, BSHAPE_COUNT, 0};   /* values per lane's row */
/* A lane's PUSH, GATE, DYNAMICS (the Bass's ACCENT) and OCTAVE in its row, -1 none */
static const signed char L_PUSH[SEQ_ACTIVE_LANES] = {SHAPE_PUSH, BSHAPE_PUSH, -1};
static const signed char L_GATE[SEQ_ACTIVE_LANES] = {SHAPE_GATE, BSHAPE_GATE, -1};
static const signed char L_DYN[SEQ_ACTIVE_LANES] = {SHAPE_DYN, BSHAPE_ACCENT, -1};
static const signed char L_OCT[SEQ_ACTIVE_LANES] = {SHAPE_OCT, BSHAPE_OCT, -1};

const char *const SEQ_PART_NAME[SEQ_NPARTS] = {"Verse", "Intro", "Pre", "Chorus", "Bridge", "Outro"};
const int SEQ_PART_SCREEN[SEQ_NPARTS] = {1, 0, 2, 3, 4, 5};

static long floor_mod(long a, long m) { return ((a % m) + m) % m; }

static int seq_channel(const seq_t *s, int lane) { return s->chan[lane]; }

static seq_part_t *cur(seq_t *s) { return &s->part[s->cur_part]; }

/* 0.19.1 the drawn lanes, by their index (DL_*) into seq_part_t.rb, rk, rfill, rfill_abs and seq_t.hum: each one's lane, the tab
 * its RHYTHM views are on, the rhythm library (rhythm.h RLIB_*) its bars come from, its DRAWN style, its bars' library rhythm in a
 * loaded part before its `.rb` entry, its save fields' prefix (`<c><fp>.rb=`) and their find_fields indices, whether the moves
 * still apply to it DRAWN, and its own parts: convert (a loaded lane of another style becomes DRAWN), notes (after a bar's steps
 * render: its notes, NULL for none), repeat (a grow repeats bar a's own state into bar b), save_extra (its fields after `.g`,
 * before `.rb`) and load_extra. Defined below its hooks (after convert_bass). */
struct out;
struct fields;
static const struct dlane {
    int lane, tab, rlib, style, rload;
    const char *fp;
    int frb, frz, moves;
    void (*convert)(seq_t *s, seq_part_t *p);
    void (*notes)(const seq_t *s, seq_part_t *p, int b, const unsigned char c[16], const bass_key_t *k);
    void (*repeat)(seq_part_t *p, int b, int a);
    void (*save_extra)(struct out *o, char c, const seq_part_t *p, const seq_lane_t *ln);
    void (*load_extra)(seq_part_t *p, const struct fields *f);
} DLANE[DL_COUNT];

/* A drawn lane's index (DL_*), -1 for any other lane */
static int dl_of(int lane) {
    for (int d = 0; d < DL_COUNT; d++)
        if (DLANE[d].lane == lane) return d;
    return -1;
}

/* 0.20 the tab a transaction on `lane` is shown on (-1 for a lane none is open on). The Melody lane has no DLANE row (dl_of -1):
 * its transactions are on TAB_MEL, and nothing here indexes DLANE by it */
static int tab_of_lane(int lane) {
    return lane == LANE_CHORDS ? TAB_CHORDS : lane == LANE_BASS ? TAB_BASS : lane == LANE_MEL_UP ? TAB_MEL : -1;
}

static const seq_part_t *ccur(const seq_t *s) { return &s->part[s->cur_part]; }

/* 0.12: a setter that changes something ends the last move's feedback (the moves set it after) */
static void act(seq_t *s) { s->move_text[0] = 0, s->lw_bumped = 0; }   /* 0.24 a LOCK WITH bump's flag lasts until the next setter */

/* PUSH forgets its step; 0.24 and ANTICIPATE its change, and every lane's next step chases a tied step (nothing sounds into it now) */
static void forget_pushes(seq_t *s) {
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) s->pushed_k[l] = -1, s->early_k[l] = -1;
    s->chase = (1u << SEQ_ACTIVE_LANES) - 1;
}

/* Lane `lane`'s value at index idx[lane] of its SHAPE row, or dflt when the lane has none */
static int lshape(const seq_part_t *p, int lane, const signed char *idx, int dflt) {
    return lane < 0 || lane >= SEQ_ACTIVE_LANES || idx[lane] < 0 ? dflt : p->lane_shape[lane][idx[lane]];
}

/* A SHAPE value's range: OCTAVE -1..1, the Bass's APPROACH 0..1, the rest 0..100. 0: no such value. */
static int lshape_range(int lane, int what, int *lo, int *hi) {
    if (lane < 0 || lane >= SEQ_ACTIVE_LANES || what < 0 || what >= LSHAPE_N[lane]) return 0;
    *lo = what == L_OCT[lane] ? -1 : 0;
    *hi = what == L_OCT[lane] ? 1 : lane == LANE_BASS && what == BSHAPE_APPROACH ? 1 : 100;
    return 1;
}

static void txn_cancel(seq_t *s);      /* 0.17: the open picker or browser, put back */
static void txn_done(seq_t *s);        /* 0.17: kept */
static void txn_unstale(seq_t *s);
static int txn_live(seq_t *s, int lane, int kind);
static void strip_clamp(seq_t *s);     /* 0.17: the CHORDS strip's page and centre inside the loop (0.19: every tab's) */
static void strip_hold_open(seq_t *s, int tab, int kind); /* 0.17: a picker or browser opens: the strip holds what it shows */
static void strip_follow_off(seq_t *s);
static void paging_off(seq_t *s);      /* 0.19: every tab's paging pause ends */

/* A progression to read: a part's own */
typedef struct { const int *prog; const unsigned char *src, *colour; int len; uint64_t lock; int hseed, filled, phrase, rank; } prog_view_t;

static prog_view_t part_view(const seq_part_t *p) {
    return (prog_view_t){p->prog, p->src, p->colour, p->prog_len, p->slot_lock, p->harmony_seed, p->filled_slots, p->phrase,
                         p->rank_len};
}

/* v1's fixed patterns as recipes: the tone, octave, velocity and length of every step in one bar of a lane. The
 * melodies arp one chord tone per on step, restarting each bar, so a toggle re-applies the recipe to its bar. */
static void apply_recipe(seq_part_t *p, int lane, int bar) {
    seq_step_t *st = &p->lane[lane].steps[bar * SEQ_STEPS];
    int n = 0;
    for (int i = 0; i < SEQ_STEPS; i++) {
        st[i].vel = VELOCITY;
        st[i].len = 1;
        st[i].oct = LANE_OCT[lane];
        st[i].tone = lane == LANE_MEL_UP ? ARP[n % 3] : lane == LANE_MEL_DOWN ? ARP[2 - n % 3] : TONE_R;
        n += st[i].on;
    }
}

static int v1_default_on(int lane, int i) {
    return lane == LANE_CHORDS ? i == 0 : lane == LANE_BASS ? (i == 0 || i == 8) : i % 2 == 0;
}

static void v1_default_pat(unsigned char pat[SEQ_LANES][SEQ_STEPS]) {
    for (int l = 0; l < SEQ_LANES; l++)
        for (int i = 0; i < SEQ_STEPS; i++) pat[l][i] = (unsigned char)v1_default_on(l, i);
}

/* 0.20 a part's JAM Melody state before any edit: ARP · UP, LOOP, CADENCE HOME (mjam_default), SWITCH AT BAR, PUSH 0, DYNAMICS 50;
 * no lock, no fix */
static void mjam_init(mjam_state_t *m) {
    memset(m, 0, sizeof *m);
    m->set = mjam_default();
    m->sw = 1;
    m->dyn = 50;
    m->acc = 1;   /* 0.21 ACCENT LIGHT */
}

/* 0.22 a part's drawn lanes' JAM state before any tap (new instances, INIT, a load without `.dj`): no block, SWITCH AT BAR, the knobs neutral;
 * 0.24 LOCK WITH OWN, ANTICIPATE OFF (a load without `.la`) */
static void djam_init(seq_part_t *p) {
    for (int d = 0; d < DL_COUNT; d++) p->djam[d] = (djam_t){-1, 0, 1, 0, 0, 0, 0, 0}, p->lw[d] = LW_OWN, p->an[d] = AN_OFF;   /* 0.24 */
}

/* 0.11: a part's melody recipe before one is picked: the Hook's defaults (VST-12: its phrasing too) */
static void mel_default(mel_recipe_t *r) {
    memset(r, 0, sizeof *r);
    mel_defaults(MEL_HOOK, r, NULL);
}

enum { RHYTHM_PULSE = 3 };   /* 0.18: Pulse's index in rhythm.h's library */
enum { BASS_POP_ROOT = 40 };  /* 0.19: Pop Root's index in bassline.h's styles: new instances', INIT's and loads' Bass notes */
enum { BASS_OCTAVE_BOUNCE = 1, BASS_UPRIGHT_WALK = 29 };   /* 0.19: the styles an old Pump and Walk Bass load as (convert_bass) */

/* 0.18: every drawn bar of a drawn lane (0.19: Chords or Bass) its library's rhythm i, unlocked and not edited; its RHYTHM knobs
 * neutral. 0.19: lane must be Chords or Bass (dl_of >= 0), as for every helper that indexes [dl_of(lane)] unchecked */
static void rbars_init(seq_part_t *p, int lane, int i) {
    int d = dl_of(lane);
    memset(p->rb[d], 0, sizeof p->rb[d]);
    memset(p->rk[d], 0, sizeof p->rk[d]);
    for (int b = 0; b < SEQ_MAX_BARS; b++) {
        rhythm_lib_codes(DLANE[d].rlib, i, p->rb[d][b].c);
        p->rb[d][b].from = (signed char)i;
    }
}

static const seq_cp_t CP_NONE = {-1, 0, 0, 0};   /* 0.24 an empty point entry, the same bytes everywhere: equal parts compare equal */

/* 0.24 no change point in any bar (seq_part_t.cp) */
static void cp_init(seq_part_t *p) {
    for (int b = 0; b < SEQ_MAX_BARS; b++)
        for (int k = 0; k < SEQ_CP_MAX; k++) p->cp[b][k] = CP_NONE;
}

/* v1's 4-bar song with each lane's 16-step pattern in every bar (0.18: and Sustain in every drawn bar) */
static void v1_part(seq_part_t *p, const unsigned char pat[SEQ_LANES][SEQ_STEPS]) {
    memset(p, 0, sizeof *p);
    p->bars = p->filled = 4;
    p->chord_len = SEQ_STEPS;
    p->prog_len = p->filled_slots = p->phrase = p->rank_len = 4;
    p->voicing = VOICING_CLOSE;
    p->vstyle = -1;   /* Custom; vmode, vcol, vinv, vspr and vstrdir 0 */
    p->harmony = 30;
    p->harmony_seed = 1;
    memcpy(p->lane_shape, LSHAPE_NEUTRAL, sizeof LSHAPE_NEUTRAL);
    memcpy(p->prog, V1_PROG, sizeof V1_PROG);
    mel_default(&p->mel);
    for (int d = 0; d < DL_COUNT; d++) rbars_init(p, DLANE[d].lane, 0);
    p->bass.set = bass_style_set(BASS_POP_ROOT);   /* 0.19: the Bass notes' settings (no bar note-locked, no fixed note) */
    mjam_init(&p->mjam);   /* 0.20: the JAM Melody's (no bar locked, no fix) */
    djam_init(p);   /* 0.22: no JAM block on the Chords and Bass lanes */
    cp_init(p);   /* 0.24: no change point */
    for (int l = 0; l < SEQ_LANES; l++) {
        p->lane[l].gen = MANUAL_GEN;
        for (int b = 0; b < p->bars; b++) {
            for (int i = 0; i < SEQ_STEPS; i++) p->lane[l].steps[b * SEQ_STEPS + i].on = pat[l][i];
            apply_recipe(p, l, b);
        }
    }
}

/* The scale a slot's chord comes from */
static int slot_scale(const seq_t *s, const seq_part_t *p, int i) { return theory_slot_scale(s->scale, p->src[i]); }

/* 0.14 the slot list (seq_part_t.prog): a chord per slot of the loop */
#define SLOT_BIT(i) ((uint64_t)1 << (i))

static int slots_of(int bars, int chord_len) { int n = bars * SEQ_STEPS / chord_len; return n < 1 ? 1 : n; }

static int part_slots(const seq_part_t *p) { return slots_of(p->bars, p->chord_len); }

/* 0.24 a slot's lock: a real slot's bit (a pick); a change point's virtual slot is always locked (its type sets its colour).
 * Every test of slot_lock goes through it, so a virtual index never shifts a uint64_t by 64 or more */
static int slot_locked(const seq_part_t *p, int slot) { return slot >= SEQ_MAX_PROG || (p->slot_lock >> slot & 1); }

/* 0.24 the chord types a pick and a change point take (seq_rbar_type's 0 Knobs .. 4 6th), as colours. 0.24.2 (append only: saved as
 * digits) 5..9 the own colours no TYPE picks, which only a point made of a sounding chord takes (slot_chord: THIS HIT ONLY) */
enum { PICK_TYPES = 5, CP_TYPES = 10 };
static const unsigned char PICK_TYPE[CP_TYPES] = {COL_TRIAD, COL_SUS2, COL_SUS4, COL_ADD9, COL_6TH, COL_7TH, COL_POWER, COL_9TH, COL_11TH, COL_13TH};

/* 0.24 1 when bar b's step 0 starts a slot: a point there would be a slot change (none is kept) */
static int bar_starts_slot(const seq_part_t *p, int b) { return b * SEQ_STEPS % p->chord_len == 0; }

/* 0.24 point k of bar b removed, the later ones moved down */
static void cp_drop(seq_part_t *p, int b, int k) {
    for (; k < SEQ_CP_MAX - 1; k++) p->cp[b][k] = p->cp[b][k + 1];
    p->cp[b][SEQ_CP_MAX - 1] = CP_NONE;
}

/* 0.24 after every change to the points, the chord length or a load: a step-0 point of a bar that starts a slot dropped, then the
 * virtual slots (prog, src, colour from the type) mirrored from cp, empty entries zeroed. revoice runs it (its voices and labels) */
static void cp_sync(seq_part_t *p) {
    for (int b = 0; b < SEQ_MAX_BARS; b++) {
        while (p->cp[b][0].step == 0 && bar_starts_slot(p, b)) cp_drop(p, b, 0);
        for (int k = 0; k < SEQ_CP_MAX; k++) {
            const seq_cp_t *c = &p->cp[b][k];
            int v = SEQ_MAX_PROG + b * SEQ_CP_MAX + k, on = c->step >= 0;
            p->prog[v] = on ? c->deg : 0;
            p->src[v] = (unsigned char)(on ? c->src : 0);
            p->colour[v] = on ? PICK_TYPE[c->type] : 0;
        }
    }
}

/* 0.24 a change point's virtual slot in a bar of the loop (the getters read it; the setters never take one) */
static int vslot_live(const seq_part_t *p, int slot) {
    int b = (slot - SEQ_MAX_PROG) / SEQ_CP_MAX;
    return slot >= SEQ_MAX_PROG && slot < SEQ_SLOTS_ALL && b < p->bars && p->cp[b][(slot - SEQ_MAX_PROG) % SEQ_CP_MAX].step >= 0;
}

/* 1 if slots q..n-1 repeat the slots q before them: chord, borrow, colour and lock */
static int repeats(const int *prog, const unsigned char *src, const unsigned char *col, uint64_t lock, int q, int n) {
    for (int i = q; i < n; i++)
        if (prog[i] != prog[i - q] || src[i] != src[i - q] || col[i] != col[i - q] || (lock >> i & 1) != (lock >> (i - q) & 1))
            return 0;
    return 1;
}

static int part_repeats(const seq_part_t *p, int q, int n) { return repeats(p->prog, p->src, p->colour, p->slot_lock, q, n); }

/* 1 if the loop repeats its phrase (always when the phrase is longer than the loop: the loop is its start) */
static int view_loops(const prog_view_t *v) {
    return v->phrase > v->len || repeats(v->prog, v->src, v->colour, v->lock, v->phrase, v->len);
}

/* The slots that make the loop: its phrase when it repeats one, else every slot */
static int view_cycle(const prog_view_t *v) { return view_loops(v) ? v->phrase : v->len; }

static int slot_cycle(const seq_part_t *p) { prog_view_t v = part_view(p); return view_cycle(&v); }

/* The slots that sound as the loop: its cycle when the COLOUR ranks repeat with it, else every slot (voiced one by one) */
static int sound_cycle(const seq_part_t *p) { int n = slot_cycle(p); return p->rank_len > 0 && n % p->rank_len ? p->prog_len : n; }

/* The loop's period: the smallest of 16 or less that divides the slot count, else 16 (the loop saves with `.ch` then) */
static int min_period(const seq_part_t *p, int n) {
    for (int q = 1; q <= SEQ_LEGACY_PROG && q <= n; q++)
        if (n % q == 0 && part_repeats(p, q, n)) return q;
    return SEQ_LEGACY_PROG;
}

/* After any change to the slots or the loop's length (setters and loads): prog_len the slot count, nothing past
 * filled_slots (so equal slot lists compare equal byte for byte), and the phrase kept while the slots still repeat it
 * (or, longer than the loop, while it is all real: filled), else the loop's period; the COLOUR ranks' period kept, or
 * (none dealt, or out of range) the phrase */
static void settle_slots(seq_part_t *p) {
    int n = part_slots(p), h = p->phrase, f;
    p->prog_len = n;
    if (p->filled_slots < n) p->filled_slots = n;
    f = p->filled_slots;
    for (int i = f; i < SEQ_MAX_PROG; i++) p->prog[i] = 0, p->src[i] = p->colour[i] = 0;
    if (f < SEQ_MAX_PROG) p->slot_lock &= SLOT_BIT(f) - 1;
    if (h < 1 || h > SEQ_LEGACY_PROG || (h <= n ? !part_repeats(p, h, n) : h > f)) h = min_period(p, n);
    p->phrase = h;
    if (p->rank_len < 1 || p->rank_len > SEQ_LEGACY_PROG) p->rank_len = h;
}

/* Slot `from` into slot i, its lock bit too */
static void copy_slot(seq_part_t *p, int i, int from) {
    p->prog[i] = p->prog[from];
    p->src[i] = p->src[from];
    p->colour[i] = p->colour[from];
    p->slot_lock = (p->slot_lock & ~SLOT_BIT(i)) | (uint64_t)slot_locked(p, from) << i;
}

/* What a loop of n slots repeats into slots it never filled: its phrase while it repeats one (so a 0.13 progression goes
 * on as 0.13 plays it, prog[span % len]), else slots 0..n-1 */
static int grow_period(const seq_part_t *p, int n) {
    int h = p->phrase;
    return h >= 1 && (h > n ? h <= p->filled_slots : part_repeats(p, h, n)) ? h : n;
}

/* A grow from n slots to `to`: the slots past filled_slots repeat the loop, chords and locks, as repeat_bars does for
 * steps. Slots a shrink hid (below filled_slots) come back as they were. */
static void grow_slots(seq_part_t *p, int n, int to) {
    int q = grow_period(p, n);
    for (int i = p->filled_slots; i < to; i++) copy_slot(p, i, i % q);
    if (p->filled_slots < to) p->filled_slots = to;
}

/* A phrase of n chords (1..16; src, colour NULL: 0) written into every slot of the loop by repetition, locked slots
 * unchanged, and on into the hidden slots a shrink left (so a grow shows the phrase there, a hidden locked slot as it
 * was). A phrase longer than the loop goes on into hidden slots too (real, as a 0.13 progression longer than its loop). */
static void fill_phrase(seq_part_t *p, int n, const int *deg, const unsigned char *src, const unsigned char *colour) {
    int slots = part_slots(p), to = n > slots ? n : slots;
    if (to < p->filled_slots) to = p->filled_slots;
    for (int i = 0; i < to; i++) {
        int j = i % n, b = src ? src[j] : 0, c = colour ? colour[j] : 0;
        if (slot_locked(p, i)) continue;
        p->prog[i] = pmod(deg[j], 7);   /* a borrow's degree: revoice */
        p->src[i] = (unsigned char)(b > THEORY_NBORROW ? 0 : b);
        p->colour[i] = (unsigned char)(c >= COL_COUNT ? 0 : c);
    }
    p->filled_slots = to;
    p->phrase = n;
}

/* A chord-length change (chord_len set, old_cl the length before): new slot i takes the slot sounding at its first step,
 * (i x new / old) % old slots, with its src, colour and lock. Hidden slots remap too (the slots past filled_slots read as
 * a grow would fill them), so a shrunk loop's hidden chords stay at their times. */
static void remap_slots(seq_part_t *p, int old_cl) {
    int prog[SEQ_MAX_PROG];   /* a setter's own (several instances set at once on iOS, VST-33): 384 bytes of stack */
    unsigned char src[SEQ_MAX_PROG], col[SEQ_MAX_PROG];
    uint64_t lock = 0;
    int cl = p->chord_len, n = slots_of(p->bars, old_cl), filled = p->filled_slots, q = grow_period(p, n), h = p->phrase, r = p->rank_len;
    int f = (filled * old_cl + cl - 1) / cl;
    f = f < part_slots(p) ? part_slots(p) : f > SEQ_MAX_PROG ? SEQ_MAX_PROG : f;
    for (int i = 0; i < SEQ_MAX_PROG; i++) {
        int t = i * cl / old_cl, j = t < filled ? t : t % q;
        prog[i] = p->prog[j], src[i] = p->src[j], col[i] = p->colour[j];
        if (i < f) lock |= (uint64_t)slot_locked(p, j) << i;
    }
    memcpy(p->prog, prog, sizeof prog);
    memcpy(p->src, src, sizeof src);
    memcpy(p->colour, col, sizeof col);
    p->slot_lock = lock;
    p->filled_slots = f;
    p->phrase = h * old_cl % cl ? 0 : h * old_cl / cl;   /* the phrase at the new length, if whole (settle_slots checks it) */
    p->rank_len = r * old_cl % cl ? 0 : r * old_cl / cl;   /* the ranks' period too */
}

/* 0.14: slot i's position in the phrase dealt (rank_len), as 0.13 ranked its progression's slots */
static int rank_pos(const seq_part_t *p, int i) { return p->rank_len > 0 ? i % p->rank_len : i; }

/* The COLOUR knob's fixed rank of slot i, 0..99: a Knuth multiplicative hash of (harmony_seed, the slot's position in the
 * phrase), its top bits mod 100. Fixed per progression, so turning the knob up colours one more slot at a time and never
 * recolours one. */
static int colour_rank(const seq_part_t *p, int i) {
    unsigned h;
    i = rank_pos(p, i);
    h = ((unsigned)p->harmony_seed * 16u + (unsigned)i) * 2654435761u;
    return (int)((h >> 16) % 100u);
}

/* 0.19: a lane that plays its drawn bars, rendered into its steps, and whose bars the RHYTHM views edit: a drawn lane (DLANE) at
 * its DRAWN style (an old project's generated Bass lane has none to edit) */
static int lane_drawn(const seq_part_t *p, int lane) {
    int d = dl_of(lane);
    return d >= 0 && p->lane[lane].gen.style == DLANE[d].style;
}

/* 0.18 a Chords lane of drawn bars (seq_part_t.rb), which the setters render into its steps */
static int drawn_lane(const seq_part_t *p) { return lane_drawn(p, LANE_CHORDS); }

/* 0.18: the COMPLEXITY the harmony and RHYTHM layers read: a DRAWN Chords lane's; any other Chords lane (a 0.17 project's
 * generated or Manual lane, whose complexity means its generator's) plays as neutral, so it keeps its sound */
static int chords_cx(const seq_part_t *p) { return drawn_lane(p) ? p->lane[LANE_CHORDS].gen.complexity : CHORDS_CX_NEUTRAL; }

/* 0.19 a Bass lane of drawn bars and the note model (seq_part_t.bass.set), which the setters render into its steps */
static int bass_drawn(const seq_part_t *p) { return lane_drawn(p, LANE_BASS); }

/* 0.20 a JAM Melody lane: the melody model (seq_part_t.mjam), which the setters render into its steps (render_mel) */
static int mel_jam(const seq_part_t *p) { return p->lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM; }
static int jam_lane(const seq_part_t *p, int lane) { return lane == LANE_MEL_UP && mel_jam(p); }

/* 0.19: the COMPLEXITY a drawn lane's RHYTHM layers read (chords_cx for the Chords lane): a lane not drawn plays as neutral */
static int lane_rcx(const seq_part_t *p, int lane) { return lane_drawn(p, lane) ? p->lane[lane].gen.complexity : CHORDS_CX_NEUTRAL; }

static int cx_clip(int c) { return c < 0 ? 0 : c > 100 ? 100 : c; }

/* The COMPLEXITY a slot's colour layers read: the knob shifted -2..+2 by the slot's place in its phrase's COLOUR rank order
 * (spread evenly over the shifts), so in each colour window the chords take their step a turn or two apart, the mean drift
 * moving a little a turn, and a slot never loses one turning up; 0..100. At 48..52 every colour layer is 0, so 50 colours as
 * the knobs say. */
static int colour_cx(const seq_part_t *p, int i) {
    int m = p->rank_len > 0 ? p->rank_len : p->prog_len, j = rank_pos(p, i), r = colour_rank(p, i), order = 0;
    for (int q = 0; q < m; q++) {
        int rq = colour_rank(p, q);
        order += rq < r || (rq == r && q < j);
    }
    return cx_clip(chords_cx(p) + (m > 0 ? (2 * order + 1) * 5 / (2 * m) : 2) - 2);   /* centred: one chord alone shifts 0 */
}

/* 0.18 perf: where the colour layers are flat (rhythm_cx_colour_flat) a chord's level is one per COMPLEXITY and user COLOUR, so
 * it is looked up: lv_flat the level before the Tension step, lv_ten with it (LV_NONE: no Tension step), for every cx 0..100 and
 * COLOUR -1..4; lv_ok[cx] 1 where flat. Made once from rhythm's offsets by seq_init (level_table_init), read only after; until
 * then chord_level works the level out. VST-33: made under pthread_once, so two instances created on two threads at once
 * (an iOS host's) both wait for the one build, and every thread that gets an instance after its seq_init reads a made table. */
#define LV_NONE (-128)
static signed char lv_flat[101][6], lv_ten[101][6], lv_ok[101];
static int lv_state;   /* 0 empty, 2 made */
static pthread_once_t lv_once = PTHREAD_ONCE_INIT;
static void level_table_build(void) {
    for (int cx = 0; cx <= 100; cx++) {
        cx_offsets_t o = rhythm_cx_offsets(cx);
        lv_ok[cx] = (signed char)rhythm_cx_colour_flat(cx);
        for (int v = -1; v <= 4; v++) {
            int lv = rhythm_cx_apply(CXT_COL, v, o.col);
            lv_flat[cx][v + 1] = (signed char)lv;
            lv_ten[cx][v + 1] = (signed char)(o.tension > 0 ? rhythm_cx_apply(CXT_COL, lv, o.tension) : LV_NONE);
        }
    }
    __atomic_store_n(&lv_state, 2, __ATOMIC_RELEASE);
}
static void level_table_init(void) { pthread_once(&lv_once, level_table_build); }

/* The COLOUR level a Knobs chord (own colour Triad) on degree deg of scale sc plays in slot i: vcol under COMPLEXITY's colour
 * layers. The Tension layer (cx_offsets_t.tension) adds its step only where it makes no avoid note (theory_avoid; coupling
 * rule 2). */
static int chord_level(const seq_t *s, const seq_part_t *p, int i, int deg, int sc) {
    cx_offsets_t o;
    int lv, t, cx = chords_cx(p);
    if (cx == CHORDS_CX_NEUTRAL) return p->vcol;
    if (cx >= 0 && cx <= 100 && p->vcol >= -1 && p->vcol <= 4 && __atomic_load_n(&lv_state, __ATOMIC_RELAXED) == 2 && lv_ok[cx]) {
        lv = lv_flat[cx][p->vcol + 1], t = lv_ten[cx][p->vcol + 1];
        return t == LV_NONE ? lv : theory_avoid(s->key, s->scale, sc, deg, theory_colour_of_level(t)) ? lv : t;
    }
    /* 0.18 perf: flat, no rank count. 0.24: a change point's virtual slot never takes a rank (as one chord alone: shift 0) */
    o = rhythm_cx_offsets(rhythm_cx_colour_flat(cx) || i >= SEQ_MAX_PROG ? cx_clip(cx) : colour_cx(p, i));
    lv = rhythm_cx_apply(CXT_COL, p->vcol, o.col);
    if (o.tension <= 0) return lv;
    t = rhythm_cx_apply(CXT_COL, lv, o.tension);
    return theory_avoid(s->key, s->scale, sc, deg, theory_colour_of_level(t)) ? lv : t;
}

static int knob_level(const seq_t *s, const seq_part_t *p, int i) { return chord_level(s, p, i, p->prog[i], slot_scale(s, p, i)); }

/* The colour slot i plays: its own, or on a Triad slot the COLOUR knob's level (0.17: vcol; the 0.9 rank rule no longer acts;
 * 0.18: under COMPLEXITY, knob_level). A locked slot plays its own (ruling 25: a lock at level Triad stays Triad). 0.24 a change
 * point's virtual slot (locked) plays its type's colour; Knobs, as a Knobs pick's lock takes, the COLOUR knob's level (no rank) */
static int eff_colour(const seq_t *s, const seq_part_t *p, int i) {
    return p->colour[i] != COL_TRIAD || (i < SEQ_MAX_PROG && slot_locked(p, i)) ? p->colour[i] : theory_colour_of_level(knob_level(s, p, i));
}

/* 0.18: the connection mode that plays: CLOSE becomes SMOOTH under COMPLEXITY's voice-leading layer */
static int eff_vmode(const seq_part_t *p) {
    return p->vmode == VMODE_CLOSE && rhythm_cx_offsets(chords_cx(p)).smooth ? VMODE_SMOOTH : p->vmode;
}

/* A part's connection mode, and the `voicing` that saves it: Close is VOICING_CLOSE, the others VOICING_SMOOTH (`.voi`) */
static void set_vmode(seq_part_t *p, int mode) {
    p->vmode = mode;
    p->voicing = mode == VMODE_CLOSE ? VOICING_CLOSE : VOICING_SMOOTH;
}

/* A part's voicing as seq_snap_t.vx holds it: style, mode, COLOUR, INVERSION, SPREAD, STRUM direction, STRUM, OCTAVE. vox_write
 * leaves the revoicing to the caller. */
static void vox_read(const seq_part_t *p, int vx[8]) {
    vx[0] = p->vstyle, vx[1] = p->vmode, vx[2] = p->vcol, vx[3] = p->vinv, vx[4] = p->vspr, vx[5] = p->vstrdir;
    vx[6] = p->lane_shape[LANE_CHORDS][SHAPE_STRUM];
    vx[7] = p->lane_shape[LANE_CHORDS][SHAPE_OCT];
}

static void vox_write(seq_part_t *p, const int vx[8]) {
    p->vstyle = vx[0];
    set_vmode(p, vx[1]);
    p->vcol = vx[2], p->vinv = vx[3], p->vspr = vx[4], p->vstrdir = vx[5];
    p->lane_shape[LANE_CHORDS][SHAPE_STRUM] = vx[6];
    p->lane_shape[LANE_CHORDS][SHAPE_OCT] = vx[7];
}

/* 0.17: a Knobs chord (deg, src) that gets locked in a slot keeps the COLOUR knob's level as its own colour (an unlock leaves
 * it); 0.18: the level it plays there under COMPLEXITY */
static int lock_colour(const seq_t *s, const seq_part_t *p, int slot, int deg, int src, int colour) {
    return colour == COL_TRIAD ? theory_colour_of_level(chord_level(s, p, slot, deg, theory_slot_scale(s->scale, src))) : colour;
}

/* A borrowed slot's degree is its borrow's, in the project scale now (a scale change or an older snapshot moves it) */
static void borrow_degrees(const seq_t *s, seq_part_t *p) {
    for (int i = 0; i < SEQ_MAX_PROG; i++)   /* 0.14: hidden slots too (they save) */
        if (p->src[i]) p->prog[i] = theory_borrow_degree(s->scale, p->src[i]);
    for (int b = 0; b < SEQ_MAX_BARS; b++)   /* 0.24 and the change points' (cp_sync mirrors them) */
        for (int k = 0; k < SEQ_CP_MAX; k++)
            if (p->cp[b][k].step >= 0 && p->cp[b][k].src) p->cp[b][k].deg = (signed char)theory_borrow_degree(s->scale, p->cp[b][k].src);
}

/* Slot i's chord in root position (theory_chord_notes); returns the count */
static int slot_notes(const seq_t *s, const seq_part_t *p, int i, int out[THEORY_CHORD_MAX]) {
    return theory_chord_notes(s->key, slot_scale(s, p, i), p->prog[i], eff_colour(s, p, i), out);
}

/* Slots 1.. voiced in the part's mode, each from the slot before it (different rows: theory_voice_mode never works in place). */
static void voice_chain(const seq_t *s, seq_part_t *p, int n) {
    int notes[THEORY_CHORD_MAX], mode = eff_vmode(p);
    for (int i = 1; i < n; i++)
        p->voice_n[i] = theory_voice_mode(notes, slot_notes(s, p, i, notes), p->voice[i - 1], p->voice_n[i - 1], mode, p->voice[i]);
}

/* The Chords lane's notes for every progression slot; run by the setters and the loads, never in seq_block. Close: the
 * chords as theory_chord_notes gives them. The other modes (0.17: Smooth, Top line, Rootless): a first pass from the
 * root-position chord, then slot 0 is led from the last slot (the loop wraps to it) and the chain re-run from there; one
 * chord stays in root position, but Rootless drops its root. Triads with no borrow get 0.8's notes in Smooth
 * (theory_voice_chord equals theory_voice_next on them).
 * 0.14: voiced over the loop's cycle (its phrase when it repeats one, as 0.13 voiced its progression), the repeats copied;
 * every slot when the COLOUR ranks do not repeat with it (sound_cycle) */
static void voice_slots(const seq_t *s, seq_part_t *p) {
    int n = sound_cycle(p), notes[THEORY_CHORD_MAX], mode = eff_vmode(p);
    if (mode == VMODE_CLOSE) {
        for (int i = 0; i < n; i++) p->voice_n[i] = slot_notes(s, p, i, p->voice[i]);
    } else {
        p->voice_n[0] = slot_notes(s, p, 0, p->voice[0]);
        if (n > 1) {
            voice_chain(s, p, n);
            p->voice_n[0] = theory_voice_mode(notes, slot_notes(s, p, 0, notes), p->voice[n - 1], p->voice_n[n - 1], mode, p->voice[0]);
            voice_chain(s, p, n);
        } else if (mode == VMODE_ROOTLESS) {   /* one chord: root position, its root left out */
            memcpy(notes, p->voice[0], sizeof notes);
            p->voice_n[0] = theory_voice_mode(notes, p->voice_n[0], notes, p->voice_n[0], mode, p->voice[0]);
        }
    }
    for (int i = n; i < p->prog_len; i++) {
        memcpy(p->voice[i], p->voice[i % n], sizeof p->voice[i]);
        p->voice_n[i] = p->voice_n[i % n];
    }
    for (int i = n > p->prog_len ? n : p->prog_len; i < SEQ_MAX_PROG; i++) {   /* none past them: equal parts compare equal */
        memset(p->voice[i], 0, sizeof p->voice[i]);
        p->voice_n[i] = 0;
    }
}

/* The slot labels the getters copy (seq_part_t.slot_text, slot_num): everything they read is what the voices read, so they
 * are made together. Zero-filled, so equal parts compare equal byte for byte. */
static void name_slots(const seq_t *s, seq_part_t *p) {
    int n = sound_cycle(p);
    memset(p->slot_text, 0, sizeof p->slot_text);
    memset(p->slot_num, 0, sizeof p->slot_num);
    for (int i = 0; i < SEQ_MAX_PROG; i++) {
        if (i >= p->prog_len) {
            p->slot_text[i][0] = p->slot_num[i][0] = ' ';
            continue;
        }
        if (i >= n) {   /* 0.14: a repeat of the cycle's slot */
            memcpy(p->slot_text[i], p->slot_text[i % n], sizeof p->slot_text[i]);
            memcpy(p->slot_num[i], p->slot_num[i % n], sizeof p->slot_num[i]);
            continue;
        }
        theory_chord_name_c(s->key, slot_scale(s, p, i), p->prog[i], eff_colour(s, p, i), p->slot_text[i], sizeof p->slot_text[i]);
        theory_numeral(s->key, s->scale, slot_scale(s, p, i), p->prog[i], p->slot_num[i], sizeof p->slot_num[i]);
    }
}

static int slot_at(const seq_part_t *p, int bar, int step);

/* 0.24 the change points of the loop's bars voiced and named, after the real slots (whose voices a point never changes: Claude's
 * call, so adding one leaves every other chord as it was). Each is led, by the voicing mode's rule, from the chord sounding at the
 * step before it (a point before it in the bar, the slot, or on step 0 the bar before's last), voiced first in time order (bar 1
 * starts slot 0, so its step 0 never holds one). Hidden bars' points: empty voices and labels, until a grow shows them */
static void voice_points(const seq_t *s, seq_part_t *p) {
    int mode = eff_vmode(p), notes[THEORY_CHORD_MAX];
    for (int b = 0; b < SEQ_MAX_BARS; b++)
        for (int k = 0; k < SEQ_CP_MAX; k++) {
            int v = SEQ_MAX_PROG + b * SEQ_CP_MAX + k, t = p->cp[b][k].step, w;
            memset(p->voice[v], 0, sizeof p->voice[v]);
            p->voice_n[v] = 0;
            if (t < 0 || b >= p->bars) continue;
            w = t ? slot_at(p, b, t - 1) : slot_at(p, b + p->bars - 1, SEQ_STEPS - 1);
            if (mode == VMODE_CLOSE) p->voice_n[v] = slot_notes(s, p, v, p->voice[v]);
            else p->voice_n[v] = theory_voice_mode(notes, slot_notes(s, p, v, notes), p->voice[w], p->voice_n[w], mode, p->voice[v]);
            theory_chord_name_c(s->key, slot_scale(s, p, v), p->prog[v], eff_colour(s, p, v), p->slot_text[v], sizeof p->slot_text[v]);
            theory_numeral(s->key, s->scale, slot_scale(s, p, v), p->prog[v], p->slot_num[v], sizeof p->slot_num[v]);
        }
}

/* The voices and the labels, after anything they read changed (setters and loads only); 0.14: the slot list settled first.
 * 0.24: the change points mirrored (cp_sync), then voiced after the slots */
static void revoice(const seq_t *s, seq_part_t *p) {
    borrow_degrees(s, p);
    settle_slots(p);
    cp_sync(p);
    voice_slots(s, p);
    name_slots(s, p);
    voice_points(s, p);
}

/* buf gets text, cut to len; returns strlen(text), as snprintf "%s" would, without its cost (the getters run on the audio
 * thread) */
static int copy_text(char *buf, int len, const char *text) {
    int n = (int)strlen(text), k = n < len - 1 ? n : len - 1;
    if (len <= 0) return n;
    memcpy(buf, text, (size_t)k);
    buf[k] = 0;
    return n;
}

static int locked(const seq_lane_t *ln, int i) { return ln->lock[i >> 3] >> (i & 7) & 1; }

/* 1 if the lane's steps follow Melody 1's: an Answer on Melody 2 (whatever Melody 1's style, Manual included) */
static int follows_mel1(const seq_part_t *p, int lane) {
    return lane == LANE_MEL_DOWN && p->lane[lane].gen.style == GEN_MEL_ANSWER;
}

static gen_params_t gen_params(const seq_t *s, const seq_part_t *p, int lane) {
    const seq_gen_t *g = &p->lane[lane].gen;
    const seq_step_t *lead = follows_mel1(p, lane) ? p->lane[LANE_MEL_UP].steps : NULL;
    return (gen_params_t){g->style, g->seed, g->complexity, p->bars, p->chord_len, p->prog, p->prog_len, s->key, s->scale,
                          lead, lane, g->variant, p->lane[lane].bar_var};
}

/* A melody lane's rhythm cache (gen_lane_cached_v1); NULL for Chords and Bass */
static gen_cache_t *lane_cache(seq_t *s, int lane) { return lane >= LANE_MEL_UP ? &s->gen_cache[lane - LANE_MEL_UP] : NULL; }

/* 0.10 Bass transforms (seq_core.h): each pure, the generator's steps of one loop (n steps) in, the shaped steps out */
enum { SALT_MOVE = 1, SALT_SYNC, SALT_OCTS, SALT_HUM, SALT_HUMV };   /* 0.18: HUMANIZE's delay and velocity (append only) */

/* A step's fixed rank 0..99 for one transform: a fixed integer hash (murmur3's finaliser) of the lane's seed and the step */
static int xf_rank(int seed, int i, unsigned salt) {
    return (int)(fmix32((unsigned)seed * 0x9E3779B1u ^ (unsigned)i * 0x85EBCA6Bu ^ salt * 0xC2B2AE35u) % 100u);
}

static int slot_first(const seq_part_t *p, int i) { return i % p->chord_len == 0; }   /* a chord change */

/* A chord-relative tone's pitch at step i, in its slot's chord (before REGISTER and OCTAVE) */
static int tone_pitch(const seq_t *s, const seq_part_t *p, int i, int tone, int oct) {
    int slot = slot_at(p, 0, i);
    return theory_tone_note(s->key, slot_scale(s, p, slot), p->prog[slot], theory_colour_tone(eff_colour(s, p, slot), tone), oct);
}

/* The next (dir 1) or previous (dir -1) onset from step i, wrapping round the loop; i itself when it is the only one */
static int onset_from(const seq_step_t *st, int n, int i, int dir) {
    for (int k = 1; k <= n; k++) {
        int j = (int)floor_mod(i + dir * k, n);
        if (st[j].on) return j;
    }
    return i;
}

static void xf_movement(const seq_t *s, const seq_part_t *p, seq_step_t *st, int n, int v, int seed) {
    seq_step_t in[SEQ_LANE_STEPS];
    if (!v) return;
    memcpy(in, st, sizeof in[0] * (size_t)n);   /* the next onset's pitch is the generator's, so a pick never depends on v */
    for (int i = 0; i < n; i++) {
        if (!in[i].on || slot_first(p, i) || xf_rank(seed, i, SALT_MOVE) >= v) continue;
        int j = onset_from(in, n, i, 1), target = tone_pitch(s, p, j, in[j].tone, in[j].oct), best = 1000;
        for (int t = TONE_2; t <= TONE_5; t++) {   /* ties: the lower tone */
            int d = iabs(tone_pitch(s, p, i, t, in[i].oct) - target);
            if (d < best) best = d, st[i].tone = (unsigned char)t;
        }
    }
}

/* Backwards, so a moved note is never moved again (targets are odd steps, sources even ones). Locks are not seen here:
 * a move onto a step the user locked is lost when regen merges the lock, as intended (the user's locked step wins). */
static void xf_sync(const seq_part_t *p, seq_step_t *st, int n, int v, int seed) {
    for (int i = n - 2; v && i >= 0; i--) {
        int pos = i % SEQ_STEPS, band = pos == 2 || pos == 6 || pos == 10 || pos == 14 ? 0 : pos == 4 || pos == 12 ? 50 : -1;
        if (band < 0 || !st[i].on || slot_first(p, i) || slot_first(p, i + 1) || st[i + 1].on) continue;
        if (band + xf_rank(seed, i, SALT_SYNC) % 50 >= v) continue;
        st[i + 1] = st[i];
        if (st[i + 1].len > 1) st[i + 1].len--;
        st[i].on = 0;
    }
}

static void xf_octaves(const seq_part_t *p, seq_step_t *st, int n, int v, int seed) {
    for (int i = 0; v && i < n; i++)
        if (st[i].on && !slot_first(p, i) && xf_rank(seed, i, SALT_OCTS) < v)
            st[i].oct = (signed char)(st[i].oct > LANE_OCT[LANE_BASS] ? st[i].oct - 1 : st[i].oct + 1);
}

/* The octave -4..4 at which step i's slot root (in progression v) is nearest `note` (ties: the lower) */
static int root_oct(const seq_t *s, const prog_view_t *v, int chord_len, int i, int note) {
    int slot = (i / chord_len) % v->len, sc = theory_slot_scale(s->scale, v->src[slot]), best = -4;
    for (int o = -3; o <= 4; o++)
        if (iabs(theory_tone_note(s->key, sc, v->prog[slot], TONE_R, o) - note) <
            iabs(theory_tone_note(s->key, sc, v->prog[slot], TONE_R, best) - note))
            best = o;
    return best;
}

/* An absolute step: its note; its tone and oct the fixed placeholder ABS_TONE, ABS_OCT, never a pitch (seq_step_t.abs) */
#define ABS_TONE TONE_R
#define ABS_OCT 0
static void set_abs(seq_step_t *x, int note) {
    x->abs = (unsigned char)note;
    x->tone = ABS_TONE;
    x->oct = ABS_OCT;
}

/* The colour the lanes that read chord tones (the recipe melody's chords, the Bass's chord-tone test) see for a slot's colour:
 * a 6th, 9th, 11th or 13th chord is its 7th chord (root, 3rd, 5th, 7th, the 5th too even where the voicing leaves it out); the
 * others as they are. The recipe reads a Power chord as its root and 5th (mel_chords). */
static int mel_chord_colour(int colour) {
    return colour == COL_6TH || colour == COL_9TH || colour == COL_11TH || colour == COL_13TH ? COL_7TH : colour;
}

static int chord_tone(const seq_t *s, const seq_part_t *p, int slot, int tone) {
    return tone == TONE_R || tone == TONE_3 || tone == TONE_5 || (tone == TONE_7 && mel_chord_colour(eff_colour(s, p, slot)) == COL_7TH);
}

/* The note of pitch class pc nearest `near` (ties: the lower) */
static int nearest_pc(int pc, int near) {
    int lo = near - (int)floor_mod(near - pc, 12);
    return near - lo <= lo + 12 - near ? lo : lo + 12;
}

static void xf_approach(const seq_t *s, const seq_part_t *p, seq_step_t *st, int n) {
    seq_step_t in[SEQ_LANE_STEPS];
    memcpy(in, st, sizeof in[0] * (size_t)n);   /* the onsets before are as the other transforms left them */
    for (int a = 0; a < n; a += p->chord_len) {
        int e = a + p->chord_len < n ? a + p->chord_len : n, last = -1;
        for (int i = a; i < e; i++)
            if (in[i].on) last = i;
        if (last < 0 || chord_tone(s, p, slot_at(p, 0, last), in[last].tone)) continue;
        int j = onset_from(in, n, last, -1), near = tone_pitch(s, p, j, in[j].tone, in[j].oct), nx = slot_at(p, 0, e % n);
        int root = theory_degree_pc(s->key, slot_scale(s, p, nx), p->prog[nx]);
        int lo = nearest_pc(root + 11, near), hi = nearest_pc(root + 1, near), note = iabs(lo - near) <= iabs(hi - near) ? lo : hi;
        set_abs(&st[last], note < 1 ? 1 : note > 127 ? 127 : note);
    }
}

static int bass_shaped(const seq_part_t *p) {
    const int *b = p->lane_shape[LANE_BASS];
    return b[BSHAPE_MOVEMENT] || b[BSHAPE_SYNC] || b[BSHAPE_OCTAVES] ||
           (b[BSHAPE_APPROACH] && p->lane[LANE_BASS].gen.style == GEN_BASS_WALK);
}

/* MOVEMENT, SYNC, OCTAVES, then the walk's APPROACH on steps 0..n-1, the loop (n = bars x 16) */
static void shape_bass(const seq_t *s, const seq_part_t *p, seq_step_t *st, int n) {
    const int *b = p->lane_shape[LANE_BASS];
    int seed = p->lane[LANE_BASS].gen.seed;
    xf_movement(s, p, st, n, b[BSHAPE_MOVEMENT], seed);
    xf_sync(p, st, n, b[BSHAPE_SYNC], seed);
    xf_octaves(p, st, n, b[BSHAPE_OCTAVES], seed);
    if (b[BSHAPE_APPROACH] && p->lane[LANE_BASS].gen.style == GEN_BASS_WALK) xf_approach(s, p, st, n);
}

/* 0.13: how many of a generated Bass's unlocked steps MOVEMENT, SYNC and OCTAVES each change, in shape_bass's order on the
 * generator's steps (a moved SYNC note counts once); a MANUAL Bass: none. Setters only: it generates the loop. */
void seq_part_bass_shape_counts(const seq_t *s, int part, int out[3]) {
    const seq_part_t *p = &s->part[part < 0 || part >= SEQ_NPARTS ? s->cur_part : part];
    const seq_lane_t *ln = &p->lane[LANE_BASS];
    const int *b = p->lane_shape[LANE_BASS];
    seq_step_t was[SEQ_LANE_STEPS], st[SEQ_LANE_STEPS];
    int n = p->bars * SEQ_STEPS;
    out[0] = out[1] = out[2] = 0;
    if (ln->gen.style == GEN_MANUAL || bass_drawn(p)) return;   /* 0.19: a DRAWN Bass has no generator's steps */
    gen_params_t gp = gen_params(s, p, LANE_BASS);
    gen_lane_cached_v1(&gp, NULL, st);
    for (int x = 0; x < 3; x++) {
        memcpy(was, st, sizeof st[0] * (size_t)n);
        if (x == 0) xf_movement(s, p, st, n, b[BSHAPE_MOVEMENT], ln->gen.seed);
        else if (x == 1) xf_sync(p, st, n, b[BSHAPE_SYNC], ln->gen.seed);
        else xf_octaves(p, st, n, b[BSHAPE_OCTAVES], ln->gen.seed);
        for (int i = 0; i < n; i++)
            if (!locked(ln, i))
                out[x] += x == 0 ? st[i].on && st[i].tone != was[i].tone : x == 1 ? st[i].on && !was[i].on : st[i].on && st[i].oct != was[i].oct;
    }
}

/* 0.11: Melody 1 on the RECIPE style (mel_generate_v1 from the part's recipe) */
static int is_recipe(const seq_part_t *p, int lane) { return lane == LANE_MEL_UP && p->lane[lane].gen.style == GEN_MEL_RECIPE; }

/* The chord sounding over each span of the loop (chord_len steps) as the recipe reads it: its slot's real tones (the
 * slot's scale, so a borrow's, and its effective colour, mel_chord_colour), root, and the slot scale's tone 7 */
static void mel_chords(const seq_t *s, const seq_part_t *p, mel_chord_t *ch, int n) {
    for (int j = 0; j < n; j++) {
        int slot = j % p->prog_len, sc = slot_scale(s, p, slot), notes[THEORY_CHORD_MAX], colour = eff_colour(s, p, slot);
        int k = theory_chord_notes(s->key, sc, p->prog[slot], mel_chord_colour(colour), notes);
        mel_chord_t *c = &ch[j];
        memset(c, 0, sizeof *c);
        c->root_pc = theory_degree_pc(s->key, sc, p->prog[slot]);
        for (int i = 0; i < k; i++) c->tones[i] = (int)floor_mod(notes[i], 12);
        c->ntones = k;
        if (colour == COL_POWER) c->tones[2] = c->tones[1];
        c->seventh = theory_degree_pc(s->key, sc, p->prog[slot] + 6);
    }
}

/* The recipe's first `bars` bars (the loop's: p->bars) as if the loop were that long, over the loop's chords */
static void mel_steps(const seq_t *s, const seq_part_t *p, int bars, seq_step_t *out) {
    mel_chord_t ch[SEQ_LANE_STEPS / 4];
    const seq_gen_t *g = &p->lane[LANE_MEL_UP].gen;
    int n = (bars * SEQ_STEPS + p->chord_len - 1) / p->chord_len;
    mel_chords(s, p, ch, n);
    mel_params_t mp = {&p->mel, g->seed, g->complexity, bars, p->chord_len, s->key, s->scale, ch, p->lane[LANE_MEL_UP].bar_var};
    mel_generate_v1(&mp, out);
}

/* 1 if a lane is generated whole (a step or bar 1 alone cannot be): a shaped Bass, a recipe melody */
static int whole_loop(const seq_part_t *p, int lane) { return (lane == LANE_BASS && bass_shaped(p)) || is_recipe(p, lane); }

/* What the generator writes for a lane of part p (steps 0 .. bars*16-1), the Bass shaped by its transforms, a recipe
 * Melody 1 by mel_generate_v1. Setters only: never on the audio thread. */
static void gen_steps(seq_t *s, const seq_part_t *p, int lane, seq_step_t *out) {
    gen_params_t gp = gen_params(s, p, lane);
    if (jam_lane(p, lane)) {   /* 0.20: no generator: its steps as rendered (render_mel) */
        memcpy(out, p->lane[lane].steps, sizeof out[0] * (size_t)(p->bars * SEQ_STEPS));
        return;
    }
    if (is_recipe(p, lane)) {
        mel_steps(s, p, p->bars, out);
        return;
    }
    gen_lane_cached_v1(&gp, lane_cache(s, lane), out);
    if (lane == LANE_BASS && bass_shaped(p)) shape_bass(s, p, out, p->bars * SEQ_STEPS);
}

/* gen_steps' step i (at least the span holding it) into st: a tap's, a pick's and an unlock's. A shaped Bass makes
 * the whole loop (MOVEMENT reads the next chord's onset), and so does a recipe melody; else only the span, as 0.9. */
static void gen_at(seq_t *s, const seq_part_t *p, int lane, int i, seq_step_t *st) {
    gen_params_t gp = gen_params(s, p, lane);
    if (whole_loop(p, lane) || jam_lane(p, lane)) gen_steps(s, p, lane, st);
    else gen_span_cached_v1(&gp, lane_cache(s, lane), i, st);
}

/* 0.19 the note model's key: the project's tonic and scale (a borrowed chord's root is still read against it) */
static bass_key_t bass_key(const seq_t *s) {
    bass_key_t k;
    k.tonic = theory_degree_pc(s->key, s->scale, 0);
    for (int d = 0; d < 7; d++) k.scale[d] = theory_degree_pc(s->key, s->scale, d);
    return k;
}

/* 0.19 the chord the Bass reads at loop step i (wrapping): its slot's triad root and 3rd, in the slot's scale (a borrow's own) */
static bass_chord_t bass_chord_at(const seq_t *s, const seq_part_t *p, int i) {
    int slot = slot_at(p, 0, i % (p->bars * SEQ_STEPS)), sc = slot_scale(s, p, slot);
    int r = theory_degree_pc(s->key, sc, p->prog[slot]), t = theory_degree_pc(s->key, sc, p->prog[slot] + 2);
    return (bass_chord_t){r, t, floor_mod(t - r, 12) == 3};
}

/* 0.19 what a DRAWN Bass bar's notes read besides its codes and settings: the key; the spans, the steps of one chord in the bar (the
 * chord length, 16 at most; 0.24 split at each change point: n spans, span j from step at[j] to at[j + 1], at[n] 16); each span's
 * chord, ch[j] span j's and ch[j + 1] its next (the one at the step after it: in a 2-bar chord's first bar the same chord). Made once
 * a bar for every pass over its notes */
typedef struct {
    const bass_key_t *key;
    int n;
    unsigned char at[SEQ_STEPS + 1];
    bass_chord_t ch[SEQ_STEPS + 1];
} bar_harm_t;

static void bar_harm(const seq_t *s, const seq_part_t *p, int b, const bass_key_t *k, bar_harm_t *h) {
    int span = p->chord_len < SEQ_STEPS ? p->chord_len : SEQ_STEPS;
    h->key = k;
    h->n = 0;
    for (int a = 0; a < SEQ_STEPS; a++)
        if (a % span == 0 || slot_at(p, b, a) != slot_at(p, b, a - 1)) h->at[h->n] = (unsigned char)a, h->ch[h->n++] = bass_chord_at(s, p, b * SEQ_STEPS + a);
    h->at[h->n] = SEQ_STEPS;
    h->ch[h->n] = bass_chord_at(s, p, b * SEQ_STEPS + SEQ_STEPS);
}

/* 0.18: a RHYTHM knob's value v in its range (SYNC -2..3, GHOSTS, HUMANIZE and FILL 0..4; 0.24 CADENCE and BREATH 0..3) */
static int rk_clamp(int what, int v) {
    int lo = what == RK_SYNC ? -2 : 0, hi = what == RK_SYNC ? 3 : what >= RK_CAD ? 3 : 4;
    return v < lo ? lo : v > hi ? hi : v;
}

/* 0.19: a Bass note knob's value v in its range (BK_*: REGISTER -2..2, the others 0..4) */
static int bk_clamp(int k, int v) { return k == BK_REG ? clampi(v, -2, 2) : clampi(v, 0, 4); }

/* 0.18: a RHYTHM knob (RK_*) as it plays at COMPLEXITY cx: SYNC (COMPLEXITY's part at most +2), GHOSTS and HUMANIZE under their
 * layers, FILL (0.24 CADENCE, BREATH) as set. A drawn lane only (rbars_init) */
static int rk_at(const seq_part_t *p, int lane, int what, int cx) {
    const signed char *rk = p->rk[dl_of(lane)];
    cx_offsets_t o = rhythm_cx_offsets(cx);
    switch (what) {
    case RK_SYNC: return rhythm_cx_apply(CXT_SYNC, rk[RK_SYNC], o.sync < 2 ? o.sync : 2);
    case RK_GHOSTS: return rhythm_cx_apply(CXT_GHOSTS, rk[RK_GHOSTS], o.ghosts);
    case RK_HUM: return rhythm_cx_apply(CXT_HUM, rk[RK_HUM], o.hum);
    case RK_FILL: case RK_CAD: case RK_BREATH: return rk[what];   /* 0.24 CADENCE and BREATH as set too */
    }
    return 0;
}

/* 0.22 drawn lane d's bar b is locked for JAM: its rhythm locked, or on the Bass its notes. A block, TIME and VARIATION leave it */
static int jam_locked(const seq_part_t *p, int d, int b) { return p->rb[d][b].lock || (d == DL_BASS && p->bass.nlock >> b & 1); }

/* 0.24 LOCK WITH: drawn lane d's bar b follows the other lane (its LW_WITH or LW_GAPS, else 0) when the lane does and the bar is neither
 * painted (ed) nor locked for JAM */
static int bar_follows(const seq_part_t *p, int d, int b) { return p->lw[d] != LW_OWN && !p->rb[d][b].ed && !jam_locked(p, d, b) ? p->lw[d] : 0; }

/* 0.24 drawn lane d's bar b codes as drawn: when it follows, the other lane's own drawn codes (as they are, or their gaps: never the other's
 * LOCK WITH, so the lanes never read each other in a loop), else its own */
static void drawn_codes(const seq_part_t *p, int d, int b, unsigned char out[16]) {
    int f = bar_follows(p, d, b);
    const unsigned char *o = p->rb[DL_COUNT - 1 - d][b].c;
    if (f == LW_GAPS) rhythm_gaps(o, out);
    else memcpy(out, f ? o : p->rb[d][b].c, 16);
}

/* 0.22 bar b's VARIATION phrase position (b mod 4) when it varies, as MELODY's: BAR 4 {3}, BARS 2+4 {1, 3}, ALL {1, 2, 3}; 0 when it
 * doesn't or is locked for JAM (a loop under 4 bars varies only the positions it has). Read on the audio thread: arithmetic only.
 * jam_var_at: at VARIATION var (dj_weak's neutral render) */
static int jam_var_at(const seq_part_t *p, int d, int b, int var) {
    int pos = b % 4;
    if (!var || !pos || jam_locked(p, d, b)) return 0;
    return var == 3 || pos == 3 || (var == 2 && pos == 1) ? pos : 0;
}
static int jam_var_pos(const seq_part_t *p, int d, int b) { return jam_var_at(p, d, b, p->djam[d].var); }

/* 0.22 drawn lane d's bar b codes as COMPLEXITY reads them: TIME, then VARIATION's rhythm (pos 1 SYNC +1, 2 an 8th later, 3 the FILL
 * variant at 3), on a bar not locked for JAM; else as drawn. jam_codes_at: at VARIATION var and TIME tm (dj_weak's neutral renders) */
static void jam_codes_at(const seq_part_t *p, int d, int b, int var, int tm, unsigned char out[16]) {
    int pos = jam_var_at(p, d, b, var);
    unsigned char c[16], t[16];
    drawn_codes(p, d, b, c);   /* 0.24 LOCK WITH's when the bar follows */
    if ((!tm && !pos) || jam_locked(p, d, b)) {
        memcpy(out, c, 16);
        return;
    }
    rhythm_time(c, tm, t);
    rhythm_vary(t, pos, 3, out);
}
static void jam_codes(const seq_part_t *p, int d, int b, unsigned char out[16]) { jam_codes_at(p, d, b, p->djam[d].var, p->djam[d].tm, out); }

/* 0.24 a phrase's last bar, where CADENCE and BREATH act: bar 4, 8, 12, 16 (b % 4 == 3), or the last bar of a loop under 4 bars (the
 * loop lengths are 1, 2, 4, 8, 16: MELODY's loop_end on them) */
static int phrase_end(const seq_part_t *p, int b) { return b % 4 == 3 || (p->bars < 4 && b == p->bars - 1); }

/* 0.24 BREATH's steps of rest (RK_BREATH: None, 1/8, Beat, Half) */
static const unsigned char BREATH_STEPS[4] = {0, 2, 4, 8};

/* 0.24 THE ORDER IN A BAR (spec "Order in a bar", pinned by tests/test_core.c test_24_bar_order), the one list of every stage a drawn
 * lane's bar goes through, each where it runs:
 *   1. drawn: the bar's own paint, else LOCK WITH's (drawn_codes), else the RHYTHM pick; NEW BAR's version (the Chords' is painted in, the
 *      Bass's is a note variation, stage 10)                                                 render_bar_at > jam_codes_at > drawn_codes
 *   2. JAM's transforms (0.22's order: TIME, then VARIATION's rhythm)                       render_bar_at > jam_codes_at
 *   3. COMPLEXITY  4. SYNC  5. GHOSTS                                                      render_bar_at
 *   6. BREATH: the phrase's last bar's last 0 / 2 / 4 / 8 steps rest                        render_bar_at
 *   7. CADENCE: the bar's last hit after BREATH (cad_step) on the chord's 5th / 3rd / root: the Bass's note (bass_cadence, in stage 10's
 *      render: the hit is picked from what 1-6 made, before FILL and ANTICIPATE), the Chords' voicing (chord_cadence, a play-time offset as
 *      INVERSION's, in part_step_notes). When ANTICIPATE (9) moves that hit, its early hit keeps the cadence (ruling 17: the Chords'
 *      voicing, the Bass's note in anticipate_notes, a fixed note still winning)
 *   8. FILL: the fill variant (rfill) from 6's codes (the Chords rhythm_fill, the Bass rhythm_bass_fill + bass_fill_notes)  render_bar_at
 *   9. ANTICIPATE (Task 3) over the loop's codes and fill variants                          lane_bars > anticipate
 *  10. the notes: the chord or bass note under each hit, fixed notes, octaves (hit octaves; the Bass note model)  bar_steps, anticipate_notes
 *  11. output: PUSH, SWING, GATE, DYNAMICS, HUMANIZE                                         seq_block (play time)
 * A locked bar skips 2-9 (it plays as drawn: render_bar_at's lock; CADENCE's lock checks; ANTICIPATE's jam_locked). Every existing stage
 * kept 0.23's order: BREATH and CADENCE slot in between GHOSTS and FILL, so a project without them renders byte for byte as before.
 *
 * A drawn lane's bar b as it plays (out: stages 1-6) and on a fill bar (fill: out with FILL, stage 8, which COMPLEXITY does not move). A
 * locked bar plays as drawn in both. A drawn lane only (rbars_init). render_bar_at: at VARIATION var and TIME tm (dj_weak's renders) */
static void render_bar_at(const seq_part_t *p, int lane, int b, int var, int tm, unsigned char out[16], unsigned char fill[16]) {
    const rbar_t *r = &p->rb[dl_of(lane)][b];
    const signed char *rk = p->rk[dl_of(lane)];
    int cx = p->lane[lane].gen.complexity;
    unsigned char codes[16], hits[16], sync[16];
    if (r->lock) {
        memcpy(out, r->c, 16);
        memcpy(fill, r->c, 16);
        return;
    }
    jam_codes_at(p, dl_of(lane), b, var, tm, codes);   /* 1, 2 */
    rhythm_hits(codes, cx, hits);                      /* 3 */
    rhythm_sync(hits, rk_at(p, lane, RK_SYNC, cx), sync);
    rhythm_ghosts(sync, rk_at(p, lane, RK_GHOSTS, cx), out);
    if (rk[RK_BREATH] && phrase_end(p, b)) rhythm_breath(out, BREATH_STEPS[rk[RK_BREATH]], out);   /* 6 (in place: memmove) */
    if (lane == LANE_BASS) rhythm_bass_fill(out, rk[RK_FILL], fill);   /* 8: 0.24 the Bass's own FILL; its notes in bass_notes */
    else rhythm_fill(out, rk[RK_FILL], fill);
}
static void render_bar(const seq_part_t *p, int lane, int b, unsigned char out[16], unsigned char fill[16]) {
    const djam_t *j = &p->djam[dl_of(lane)];
    render_bar_at(p, lane, b, j->var, j->tm, out, fill);
}

/* 0.19 the settings DRAWN Bass bar b's notes come from: its own frozen ones when it is note-locked, else `set` (NULL: the part's) */
static const bass_set_t *bar_set(const seq_part_t *p, int b, const bass_set_t *set) {
    return p->bass.nlock >> b & 1 ? &p->bass.frozen[b] : set ? set : &p->bass.set;
}

/* 0.19 the notes and roles of DRAWN Bass bar b, whose codes as they play are c: the note model (bass_span) per span of h, with the
 * bar's settings (bar_set), its NEW BAR variation (0.22 + its JAM VARIATION position pos, jam_var_pos) and its fixed notes */
static void bass_bar_notes_at(const seq_part_t *p, int b, const bass_set_t *set, const bar_harm_t *h, const unsigned char c[16], int pos,
                              unsigned char note[16], char role[16]) {
    int v = p->lane[LANE_BASS].bar_var[b] + pos;
    set = bar_set(p, b, set);
    for (int j = 0; j < h->n; j++) {
        int a = h->at[j], i = b * SEQ_STEPS + a;
        bass_span_in_t in = {c + a, h->at[j + 1] - a, &h->ch[j], &h->ch[j + 1], h->key, *set, v, &p->bass.fix[i]};
        bass_span(&in, note + a, role + a);
    }
}
static void bass_bar_notes(const seq_part_t *p, int b, const bass_set_t *set, const bar_harm_t *h, const unsigned char c[16],
                           unsigned char note[16], char role[16]) {
    bass_bar_notes_at(p, b, set, h, c, jam_var_pos(p, DL_BASS, b), note, role);
}

/* 0.24 CADENCE's hit in drawn lane d's bar b (stage 7 of THE ORDER IN A BAR), read back from its rendered steps (on) and flags: the bar's
 * last hit of stages 1-6 (FILL's hits live in rfill, never in the steps); on the Bass a fixed note is passed over (the one before it
 * lands). An early hit (SF_EARLY) is not one of the bar's own (ANTICIPATE added it for a change after it); a TIED step is the bar's own
 * hit that ANTICIPATE moved (fix round 1, ruling 17): it is still the cadence hit, and its early hit (cad_at, from the step it ties over)
 * plays the cadence. -1 none */
static int cad_step(const seq_part_t *p, int d, int b) {
    const seq_step_t *st = &p->lane[DLANE[d].lane].steps[b * SEQ_STEPS];
    const unsigned char *f = &p->sflag[d][b * SEQ_STEPS];
    for (int j = SEQ_STEPS - 1; j >= 0; j--) {
        if (d == DL_BASS && p->bass.fix[b * SEQ_STEPS + j]) continue;   /* a fixed note never lands (moved or not) */
        if ((f[j] & SF_TIED) || (st[j].on && !(f[j] & SF_EARLY))) return j;
    }
    return -1;
}

/* 0.24 CADENCE (RK_CAD, 0 none) on drawn lane d's part step i: i holds its phrase-end bar's cadence hit (cad_step; a TIED step: the hit
 * ANTICIPATE moved from there), the lane DRAWN, the bar not locked for JAM (the Chords: its rhythm lock; the Bass: its rhythm or notes) */
static int cad_at(const seq_part_t *p, int d, int i) {
    int b = i / SEQ_STEPS, cad = p->rk[d][RK_CAD];
    if (!cad || !lane_drawn(p, DLANE[d].lane) || !phrase_end(p, b) || jam_locked(p, d, b) || cad_step(p, d, b) != i % SEQ_STEPS) return 0;
    return cad;
}

static const char CAD_ROLE[4] = {'1', '5', '3', '1'};   /* 0.24 a Bass cadence note's role: OPEN the 5th, SOFT the 3rd, HOME the root */

/* 0.24 CADENCE's chord tone (RK_CAD 1 OPEN the 5th, 2 SOFT the 3rd, 3 HOME the root) of the chord at the part's step i (a change point's
 * too), as a pitch class: its scale's degrees 4, 2, 0 above the root (a borrowed chord's in its own scale; a diminished 5th as it is) */
static int cad_pc(const seq_t *s, const seq_part_t *p, int i, int cad) {
    static const signed char DEG[4] = {0, 4, 2, 0};
    int slot = slot_at(p, 0, i % (p->bars * SEQ_STEPS));
    return theory_degree_pc(s->key, slot_scale(s, p, slot), p->prog[slot] + DEG[cad < 0 || cad > 3 ? 0 : cad]);
}

/* 0.24 CADENCE on DRAWN Bass bar b's notes and roles (rendered steps: cad_step): on the phrase's last bar, not locked for JAM (its rhythm or
 * notes), the last hit's note moves to the chord's 5th / 3rd / root in the octave nearest it (role 5, 3, 1). Returns that step, -1 none */
static int bass_cadence(const seq_t *s, const seq_part_t *p, int b, unsigned char note[16], char role[16]) {
    int k, cad;
    if (!p->rk[DL_BASS][RK_CAD] || (k = cad_step(p, DL_BASS, b)) < 0 || !note[k] || !(cad = cad_at(p, DL_BASS, b * SEQ_STEPS + k)))
        return -1;   /* a TIED cadence hit has no note here (a hold): anticipate_notes lands its early hit */
    note[k] = (unsigned char)clampi(nearest_pc(cad_pc(s, p, b * SEQ_STEPS + k, cad), note[k]), 1, 127);
    role[k] = CAD_ROLE[cad];
    return k;
}

/* 0.19 DRAWN Bass bar b's notes (its DLANE notes), its steps rendered from its codes as they play (c): each hit's abs and brole (0.24 with
 * CADENCE, bass_cadence), and its fill variant's (rfill[DL_BASS]) into rfill_abs, in key k (NULL: made here). 0.24 BASS FILL: PICKUP,
 * STEP-IN and RUN play the bar's own notes (CADENCE's included) and bass_fill_notes on their steps, walking into the next bar's first
 * root (the root of the chord on its step 1, as that bar's settings play a root); BREAK's (0.19's rule, kept for old projects) are the
 * note model's on its codes, with CADENCE's note where it still sounds. A locked bar never fills (play_step): its own notes */
static void bass_notes(const seq_t *s, seq_part_t *p, int b, const unsigned char c[16], const bass_key_t *k) {
    unsigned char note[16], *fill = p->rfill[DL_BASS][b], *fnote = p->rfill_abs[DL_BASS][b];
    char role[16], *br = &p->brole[b * SEQ_STEPS];
    seq_step_t *st = &p->lane[LANE_BASS].steps[b * SEQ_STEPS];
    int f = p->rk[DL_BASS][RK_FILL], ck, nb = (b + 1) % p->bars;
    bass_key_t own;
    bar_harm_t h;
    if (!k) own = bass_key(s), k = &own;
    bar_harm(s, p, b, k, &h);
    bass_bar_notes(p, b, NULL, &h, c, note, br);
    ck = bass_cadence(s, p, b, note, br);
    for (int i = 0; i < SEQ_STEPS; i++)
        if (st[i].on) set_abs(&st[i], note[i]);
    if (f != 4 || !memcmp(c, fill, 16)) {   /* NONE, a locked bar or a BREAK that drops nothing: the same notes */
        memcpy(fnote, note, 16);
        if (f > 0 && f < 4 && !p->rb[DL_BASS][b].lock)
            bass_fill_notes(f, bass_root_note(bar_set(p, nb, NULL), bass_chord_at(s, p, nb * SEQ_STEPS).root), fnote);
        return;
    }
    bass_bar_notes(p, b, NULL, &h, fill, fnote, role);
    if (ck >= 0 && rhythm_is_hit(fill[ck])) fnote[ck] = note[ck];
}

static int chord_change_at(const seq_t *s, const seq_part_t *p, int bar, int step);
static void djq_own(seq_t *s, seq_part_t *p, int d);
static void djq_follow(seq_t *s, seq_part_t *p, int d);

/* 0.24 ANTICIPATE's steps (AN_*) */
static const signed char AN_STEPS[4] = {0, 1, 2, 4};

/* 0.24 drawn lane d's bar b flags before ANTICIPATE: SF_ADDED on each hit of its codes as they play (c) that its drawn codes lack */
static void bar_flags(seq_part_t *p, int d, int b, const unsigned char c[16]) {
    unsigned char dr[16], *f = &p->sflag[d][b * SEQ_STEPS];
    drawn_codes(p, d, b, dr);
    for (int i = 0; i < SEQ_STEPS; i++) f[i] = rhythm_is_hit(c[i]) && !rhythm_is_hit(dr[i]) ? SF_ADDED : 0;
}

/* A drawn lane's bar b from its codes as they play (c; its fill variant in rfill): its steps, the Chords' hit octaves (0.24 PHRASE's: the
 * step's oct, which chord_notes moves 12 an octave), then its notes (DLANE notes: 0.19 a DRAWN Bass's) in key k (NULL: made there).
 * 0.24 final review (ruling 32): a hit octave belongs to a DRAWN hit (the bar's drawn codes, LOCK WITH's): a hit the knobs add (GHOSTS,
 * SYNC, COMPLEXITY) plays at the part's octave, and a step whose drawn code is no longer a hit (a RHYTHM pick or try, a JAM block, LOCK
 * WITH, a copy, NEW BAR: every one renders here) loses its octave, so it never comes back on a later hit. Every caller that renders
 * here keeps the octaves in its undo step or CANCEL (seq_snap_t.hoct for both drawn lanes, seq_txn_t.hoct / sub_hoct, djq_t.hoct) */
static void bar_steps(const seq_t *s, seq_part_t *p, int lane, int b, const unsigned char c[16], const bass_key_t *k) {
    int d = dl_of(lane);
    seq_step_t *st = &p->lane[lane].steps[b * SEQ_STEPS];
    rhythm_to_steps(c, st);
    if (lane == LANE_CHORDS) {
        unsigned char dr[16];
        drawn_codes(p, d, b, dr);
        for (int i = 0; i < SEQ_STEPS; i++) {
            if (!rhythm_is_hit(dr[i])) p->hoct[b][i] = 0;
            if (st[i].on) st[i].oct = p->hoct[b][i];
        }
    }
    if (DLANE[d].notes) DLANE[d].notes(s, p, b, c, k);
}

/* A drawn lane's bar b: its fill variant from its codes and the knobs, its flags, and its steps when the lane plays its bars (lane_drawn),
 * then its notes, in key k (NULL: made there). ANTICIPATE OFF only: on, every bar renders at once (lane_bars) */
static void render_one_k(const seq_t *s, seq_part_t *p, int lane, int b, const bass_key_t *k) {
    unsigned char c[16];
    render_bar(p, lane, b, c, p->rfill[dl_of(lane)][b]);
    bar_flags(p, dl_of(lane), b, c);
    if (lane_drawn(p, lane)) bar_steps(s, p, lane, b, c, k);
}

/* 0.24 drawn lane d's bar b may play its FILL variant (rfill): FILL on, not locked, a bar fill_due can pick (every 4th of a loop of 4 bars
 * or more, any of a shorter one) */
static int fillable(const seq_part_t *p, int d, int b) { return p->rk[d][RK_FILL] && !p->rb[d][b].lock && (p->bars < 4 || b % 4 == 3); }

/* 0.24 ANTICIPATE on drawn lane d's codes as they play (c, and the FILL variants in rfill) over the loop: at each step j where a chord
 * starts (chord_change_at) and the lane hits, with the n steps before it rests (wrapping into the bar before; in a bar that may play its
 * fill, its fill variant's too) and neither bar locked for JAM, the hit moves to j - n and holds over j, which becomes a hold (the fill
 * variants alike where they may play, when j's has the hit: a fill variant without it stays as it is): SF_EARLY | SF_ADDED there, SF_TIED
 * at j. A hit already moved never moves again. Deterministic, in
 * step order; no allocation */
static void anticipate(const seq_t *s, seq_part_t *p, int d, unsigned char c[][16]) {
    int n = AN_STEPS[p->an[d]], len = p->bars * SEQ_STEPS;
    for (int j = 0; j < len; j++) {
        int b = j / SEQ_STEPS, st = j % SEQ_STEPS, e = (j - n + len) % len, eb = e / SEQ_STEPS, ok = 1, code = c[b][st], fhit;
        if (!rhythm_is_hit(code) || (p->sflag[d][j] & SF_EARLY) || jam_locked(p, d, b) || jam_locked(p, d, eb) || !chord_change_at(s, p, b, st))
            continue;
        for (int t = 0; t < n && ok; t++) {
            int i = (e + t) % len, ib = i / SEQ_STEPS;
            ok = c[ib][i % SEQ_STEPS] == RC_REST && (!fillable(p, d, ib) || p->rfill[d][ib][i % SEQ_STEPS] == RC_REST);
        }
        if (!ok) continue;
        fhit = !fillable(p, d, b) || rhythm_is_hit(p->rfill[d][b][st]);   /* j's fill variant has the hit to move (BREAK may have none) */
        for (int t = 0; t <= n; t++) {
            int i = (e + t) % len, ib = i / SEQ_STEPS, w = t ? RC_HOLD : code;
            c[ib][i % SEQ_STEPS] = (unsigned char)w;
            if (fhit && fillable(p, d, ib)) p->rfill[d][ib][i % SEQ_STEPS] = (unsigned char)w;
        }
        p->sflag[d][e] = SF_EARLY | SF_ADDED;
        p->sflag[d][j] = SF_TIED;
    }
}

/* 0.24 after ANTICIPATE's bars rendered (their codes c): each early hit at e (tying over j = e + n) holds over the bar line (its len with
 * the next bar's leading holds, 16 at most) and plays the change's chord: on the Chords j's hit octave (part_step_notes reads its slot at
 * j), on the Bass j's fixed note, else the root of the chord at j nearest its note (its FILL variant's too). 0.24.2 in the bars of mask */
static void anticipate_notes(const seq_t *s, seq_part_t *p, int lane, unsigned char c[][16], unsigned mask) {
    int d = dl_of(lane), n = AN_STEPS[p->an[d]], len = p->bars * SEQ_STEPS;
    for (int e = 0; e < len; e++) {
        seq_step_t *x = &p->lane[lane].steps[e];
        int j = (e + n) % len, eb = e / SEQ_STEPS, h = 0;
        if (!(mask >> eb & 1) || !(p->sflag[d][e] & SF_EARLY) || !x->on) continue;
        if (e % SEQ_STEPS + x->len == SEQ_STEPS)
            while (h < SEQ_STEPS && c[(eb + 1) % p->bars][h] == RC_HOLD) h++;
        x->len = (unsigned char)(x->len + h > SEQ_STEPS ? SEQ_STEPS : x->len + h);
        if (lane == LANE_CHORDS) x->oct = p->hoct[j / SEQ_STEPS][j % SEQ_STEPS];
        else {
            /* 0.24 fix round 1 (ruling 17): j's fixed note, else the cadence note when j was its bar's cadence hit, else the new root */
            int near = x->abs ? x->abs : 36, cad = p->bass.fix[j] ? 0 : cad_at(p, DL_BASS, j);
            int note = p->bass.fix[j] ? p->bass.fix[j] : nearest_pc(cad ? cad_pc(s, p, j, cad) : bass_chord_at(s, p, j).root, near);
            set_abs(x, clampi(note, 1, 127));
            p->brole[e] = p->bass.fix[j] ? 'f' : cad ? CAD_ROLE[cad] : '1';
            if (rhythm_is_hit(p->rfill[d][eb][e % SEQ_STEPS])) p->rfill_abs[d][eb][e % SEQ_STEPS] = x->abs;
        }
    }
}

/* 0.24 drawn lane `lane`'s bars of mask (bit b, below filled) rendered in key k; with ANTICIPATE on every bar's codes and flags at once,
 * then the pass over the loop. 0.24.2 the pass moves a hit at most a bar back: a bar's early hit and tie read its own codes and the next
 * bar's (anticipate_notes: the next bar's leading holds, its cadence), as a Bass bar's FILL reads the next bar's root (bass_notes; a bar
 * past the loop's next is in the loop), so a change to the bars of mask reaches only them, the bar after each in the loop and the bars
 * whose next one they are; their steps and notes render, the others' are as a render of every bar leaves them (none for mask 0) */
static void lane_bars(const seq_t *s, seq_part_t *p, int lane, unsigned mask, const bass_key_t *k) {
    int d = dl_of(lane);
    unsigned char c[SEQ_MAX_BARS][16];
    unsigned near = mask;
    if (!p->an[d] || !lane_drawn(p, lane)) {
        for (int b = 0; b < p->filled; b++)
            if (mask >> b & 1) render_one_k(s, p, lane, b, k);
        return;
    }
    if (!mask) return;
    for (int b = 0; b < p->filled; b++) {
        int nb = (b + 1) % p->bars;
        if (mask >> nb & 1) near |= 1u << b;
        if (b < p->bars && (mask >> b & 1)) near |= 1u << nb;
    }
    for (int b = 0; b < p->filled; b++) {
        render_bar(p, lane, b, c[b], p->rfill[d][b]);
        bar_flags(p, d, b, c[b]);
    }
    anticipate(s, p, d, c);
    for (int b = 0; b < p->filled; b++)
        if (near >> b & 1) bar_steps(s, p, lane, b, c[b], k);
    anticipate_notes(s, p, lane, c, near);
}

/* A drawn lane's bars of mask rendered (lane_bars), the key made once; with `follow` the other lane's too when it follows this one (LOCK
 * WITH reads its drawn codes: a knob that only moves how this lane plays passes 0). Setters only (0.22: and a queued JAM block's scratch
 * render, djq_render); then the blocks queued on them again (djq_own, djq_follow) */
static void render_mask(seq_t *s, seq_part_t *p, int lane, unsigned mask, int follow) {
    int d = dl_of(lane), o = DL_COUNT - 1 - d;
    bass_key_t k = bass_key(s);
    lane_bars(s, p, lane, mask, &k);
    if (follow && p->lw[o] != LW_OWN) {
        lane_bars(s, p, DLANE[o].lane, mask, &k);
        djq_own(s, p, o);
    }
    djq_follow(s, p, d);
}

/* render_mask's one bar, and every bar below filled (render_rhythm) */
static void render_one(seq_t *s, seq_part_t *p, int lane, int b) { render_mask(s, p, lane, 1u << b, 1); }
static void render_rhythm(seq_t *s, seq_part_t *p, int lane) { render_mask(s, p, lane, (1u << p->filled) - 1, 1); }

/* 0.24.1 both drawn lanes' every bar below filled, each rendered once, drawn lane `first`'s first, then their blocks once (djq_own, both
 * lanes): render_rhythm on each renders a lane that follows the other twice (it reads only the other's drawn codes and the harmony) and
 * the queued blocks again. The callers pass the lane render_rhythm on each would have rendered first for the last time */
static void render_pair(seq_t *s, seq_part_t *p, int first) {
    bass_key_t k = bass_key(s);
    lane_bars(s, p, DLANE[first].lane, (1u << p->filled) - 1, &k);
    lane_bars(s, p, DLANE[DL_COUNT - 1 - first].lane, (1u << p->filled) - 1, &k);
    for (int d = 0; d < DL_COUNT; d++) djq_own(s, p, d);
}
/* 0.24.1 the part-wide renders' order: the lane that follows the other last (Chords first when neither does) */
static int pair_first(const seq_part_t *p) { return p->lw[DL_CHORDS] != LW_OWN ? DL_BASS : DL_CHORDS; }
static int both_drawn(const seq_part_t *p) { return lane_drawn(p, LANE_CHORDS) && lane_drawn(p, LANE_BASS); }

/* 0.19 *dst becomes src but for the knobs (knobs 2-5) of mask `keep` (bit BK_*), which stay as they are */
static void bset_keep(bass_set_t *dst, const bass_set_t *src, unsigned keep) {
    bass_set_t b = *src;
    for (int k = 0; k < BK_COUNT; k++)
        if (keep >> k & 1) b.k[k] = dst->k[k];
    *dst = b;
}

/* 0.19.1 a Bass snapshot's notes back in place: all but the note settings, and of them what `keep` says (seq_snap_t.bkeep) */
static void bass_back(bass_state_t *dst, const bass_state_t *src, unsigned keep) {
    bass_set_t now = dst->set;
    *dst = *src;
    dst->set = now;
    if (keep & BKEEP_KNOBS) bset_keep(&dst->set, &src->set, keep / BKEEP_TURNED);
    else if (keep & BKEEP_PO) dst->set.pedal = src->set.pedal, dst->set.octave = src->set.octave;   /* PEDAL, OCTAVE: only themselves */
}

/* 0.20 a Melody snapshot's JAM notes back in place: the locks, frozen settings and fixes; the settings, SWITCH AT, PUSH and
 * DYNAMICS stay (knob turns and block switches are not undo steps), 0.21 SWING and ACCENT too */
static void mjam_back(mjam_state_t *dst, const mjam_state_t *src) {
    mjam_state_t now = *dst;
    *dst = *src;
    dst->set = now.set, dst->sw = now.sw, dst->push = now.push, dst->dyn = now.dyn;
    dst->swing = now.swing, dst->acc = now.acc, dst->weak = now.weak, dst->grid16 = now.grid16;   /* 0.21 */
}

/* 0.19 the Bass note settings (bass.set) changed: every bar that plays them renders (not a note-locked one: its own are frozen) */
static void bass_notes_changed(seq_t *s, seq_part_t *p) {
    if (bass_drawn(p)) render_mask(s, p, LANE_BASS, ~(unsigned)p->bass.nlock & ((1u << p->filled) - 1), 0);   /* 0.22 its queue too */
}

/* 1 when the voices read the same at COMPLEXITY a and b: every slot's colour layers (the -2..+2 rank shifts) and the
 * voice-leading layer (SPREAD and STRUM are read where they play) */
static int harmony_same(int a, int b) {
    if (rhythm_cx_offsets(a).smooth != rhythm_cx_offsets(b).smooth) return 0;
    for (int d = -2; d <= 2; d++) {
        cx_offsets_t x = rhythm_cx_offsets(cx_clip(a + d)), y = rhythm_cx_offsets(cx_clip(b + d));
        if (x.col != y.col || x.tension != y.tension) return 0;
    }
    return 1;
}

static void colour_changed(seq_t *s, seq_part_t *p);

/* 0.18: a DRAWN lane's COMPLEXITY moved from `was` (the CPU rule): the Chords voices only when a harmony layer moved, and
 * only the unlocked bars whose hits changed (every one when SYNC or GHOSTS moved) render. 0.22: hits of the codes JAM leaves (jam_codes) */
static void cx_turned(seq_t *s, seq_part_t *p, int lane, int was) {
    const rbar_t *rb = p->rb[dl_of(lane)];
    int now = p->lane[lane].gen.complexity;
    int all = rk_at(p, lane, RK_SYNC, was) != rk_at(p, lane, RK_SYNC, now) ||
              rk_at(p, lane, RK_GHOSTS, was) != rk_at(p, lane, RK_GHOSTS, now);
    unsigned char a[16], b[16], c[16];
    unsigned m = 0;
    if (lane == LANE_CHORDS && !harmony_same(was, now)) colour_changed(s, p);   /* the voices read the Chords lane's */
    for (int bar = 0; bar < p->filled; bar++) {
        if (rb[bar].lock) continue;
        if (!all) {
            jam_codes(p, dl_of(lane), bar, c);
            rhythm_hits(c, was, a);
            rhythm_hits(c, now, b);
            if (!memcmp(a, b, sizeof a)) continue;
        }
        m |= 1u << bar;
    }
    render_mask(s, p, lane, m, 0);   /* 0.22 a queued JAM block: once */
}

/* 0.20 the chords a JAM Melody's model reads: every slot's triad in its own scale (mjam_chord_of: never COLOUR's extensions), and
 * per bar of the loop the chord under each step (0.24 a change point's from its step) */
typedef struct { mjam_chord_t ch[SEQ_SLOTS_ALL]; mjam_bar_in_t bar[SEQ_MAX_BARS]; } mel_harm_t;

static void mel_harm(const seq_t *s, const seq_part_t *p, mel_harm_t *h) {
    for (int k = 0; k < SEQ_SLOTS_ALL; k++)
        if (k < p->prog_len || vslot_live(p, k)) mjam_chord_of(s->key, s->scale, slot_scale(s, p, k), p->prog[k], &h->ch[k]);
    for (int b = 0; b < p->bars; b++) {
        h->bar[b].key = s->key, h->bar[b].scale = s->scale;
        for (int i = 0; i < SEQ_STEPS; i++) h->bar[b].chord[i] = &h->ch[slot_at(p, b, i)];
    }
}

/* 0.20 a JAM bar's fixes (mjam_state_t.fix) on its model line, bar its chords: a note sets the step's note (the holds after it
 * stay, now its own), REST silences the step, HOLD continues the note before. A hold with no note before it in the bar is silent;
 * a hold whose run a fix changed takes its note's tension (the model's own holds keep theirs) */
static void mel_fix_line(const seq_t *s, const unsigned char *fix, const mjam_bar_in_t *bar, mjam_note_t line[SEQ_STEPS]) {
    int changed = 0;
    for (int i = 0; i < SEQ_STEPS; i++) {
        int f = fix[i], prev = i > 0 && (line[i - 1].note || line[i - 1].hold);
        if (f >= 1 && f <= 127) line[i] = (mjam_note_t){(unsigned char)f, 0, (unsigned char)mjam_tension(s->key, s->scale, bar->chord[i], f, i)};
        else if (f == MJ_FIX_REST) line[i] = (mjam_note_t){0, 0, 0};
        else if (f == MJ_FIX_HOLD) line[i] = (mjam_note_t){0, 1, 0};
        else if (line[i].note) changed = 0;
        if (f) changed = 1;
        if (line[i].hold && !prev) line[i] = (mjam_note_t){0, 0, 0};
        else if (line[i].hold && changed) line[i].tension = line[i - 1].tension;
    }
}

/* 0.20 a JAM bar's line into its 16 steps: each note an absolute step held to the end of its holds; 0.21 its velocity ACCENT's,
 * 96 + (strength - 2) x a (acc EVEN, LIGHT, MEDIUM, HARD: a 0, 4, 8, 12; mjam_strength: 4 the downbeat, 3 beat 3, 2 beats 2 and 4, 1
 * the other 8ths, 0 the 16ths): HARD 120, 108, 96, 84, 72 */
static void mel_line_steps(const mjam_note_t *line, seq_step_t *st, int acc) {
    static const unsigned char A[4] = {0, 4, 8, 12};
    for (int i = 0; i < SEQ_STEPS; i++) {
        int str = mjam_strength(i), len = 1;
        st[i] = (seq_step_t){0, ABS_TONE, ABS_OCT, VELOCITY, 1, 0};
        if (!line[i].note) continue;
        while (i + len < SEQ_STEPS && line[i + len].hold) len++;
        st[i] = (seq_step_t){1, ABS_TONE, ABS_OCT, (unsigned char)(96 + (str - 2) * A[acc & 3]), (unsigned char)len, line[i].note};
    }
}

/* 0.20 a JAM Melody lane's bar b (below filled) as it plays: the melody model with in's settings (a locked bar's frozen ones) and the
 * bar's NEW BAR variation, then its fixes; h: mel_harm's chords. The one render of a bar: render_mel_into and mel_bar_line both use it.
 * 0.21 odd (if not NULL): mjam_swing16's test of the bar, from the render itself on an unlocked bar (a locked one shapes it again with
 * the live settings, as the grid reads them) */
static void mel_render_bar(const seq_t *s, const seq_part_t *p, const mjam_loop_in_t *in, const mel_harm_t *h, int b, mjam_note_t line[SEQ_STEPS],
                           int *odd) {
    int locked = p->mjam.nlock >> b & 1;
    mjam_bar_odd(in, b, locked ? &p->mjam.frozen[b] : &in->set, line, locked ? NULL : odd);
    if (odd && locked) *odd = mjam_swing16_bar(in, b);
    mel_fix_line(s, &p->mjam.fix[b * SEQ_STEPS], &h->bar[b % p->bars], line);
}

/* 0.20 a JAM Melody lane's lines and steps with settings `set`, every bar of the loop (mel_render_bar); 0.21 grid16 (if not NULL): the
 * loop's swing grid (mjam_swing16, from the bars as they render); steps NULL: the lines only; h: mel_harm's chords, once per setter */
static void render_mel_into(const seq_t *s, const seq_part_t *p, const mel_harm_t *h, const mjam_set_t *set, mjam_note_t *lines,
                            seq_step_t *steps, int *grid16) {
    mjam_loop_in_t in = {s->key, s->scale, p->bars, *set, h->bar, p->lane[LANE_MEL_UP].bar_var};
    int g = 0;
    for (int b = 0; b < p->bars; b++) {   /* the visible loop: hidden bars (bars..filled-1) are re-rendered by regen_part when it grows */
        int odd = 0;
        mel_render_bar(s, p, &in, h, b, &lines[b * SEQ_STEPS], grid16 && !g ? &odd : NULL);
        g |= odd;
        if (steps) mel_line_steps(&lines[b * SEQ_STEPS], &steps[b * SEQ_STEPS], p->mjam.acc);
    }
    if (grid16) *grid16 = g;   /* 0.21 */
}

/* 0.21 "weak here": x changed of n is fewer than 1 in 8 */
static int mel_few(int x, int n) { return x * 8 < n; }

/* 0.21 the steps of n where lines a and b sound differently: the sounding pitch (a held step its note's, within its bar; 0 silence) or
 * a note start (the knob audit's count, tests/test_knob_audit.c) */
static int mel_differ(const mjam_note_t *a, const mjam_note_t *b, int n) {
    int d = 0, pa = 0, pb = 0;
    for (int i = 0; i < n; i++) {
        if (i % SEQ_STEPS == 0) pa = pb = 0;
        pa = a[i].note ? a[i].note : a[i].hold ? pa : 0;
        pb = b[i].note ? b[i].note : b[i].hold ? pb : 0;
        d += pa != pb || !a[i].note != !b[i].note;
    }
    return d;
}

/* 0.21 SWING's weak bit for a loop's lines on swing grid g16: SWING off 50% with fewer than 1 in 8 of the notes on the swung positions
 * (grid16 odd steps, else step mod 4 = 2). Amendment 1: ACCENT left the knobs (its bit is TIME's, mel_weak) */
static unsigned mel_weak_notes(const seq_part_t *p, const mjam_note_t *lines, int g16) {
    int notes = 0, swung = 0;
    for (int i = 0; i < p->bars * SEQ_STEPS; i++) {
        if (!lines[i].note) continue;
        notes++;
        swung += g16 ? i & 1 : i % 4 == 2;
    }
    return p->mjam.swing && mel_few(swung, notes) ? MW_SWING : 0u;
}

/* 0.21 weak flags w with SWING's bit counted again (a SWING turn: nothing renders) */
static unsigned char mel_reweigh(unsigned w, const seq_part_t *p, const mjam_note_t *lines, int g16) {
    return (unsigned char)((w & (MW_DENS | MW_RNG | MW_VAR | MW_TIME)) | mel_weak_notes(p, lines, g16));
}

/* 0.21 the MW_* bits of a loop rendered with `set` (its lines and grid g16): DENSITY, RANGE, VARIATION and (Amendment 1) TIME off
 * neutral each render the lines again with that field at 0 (lines only) and are weak when fewer than 1 in 8 of the loop's steps sound
 * different; a neutral knob renders nothing. Setters only (render_mel, render_queued; h: their mel_harm) */
static unsigned mel_weak(const seq_t *s, const seq_part_t *p, const mel_harm_t *h, const mjam_set_t *set, const mjam_note_t *lines, int g16) {
    static const unsigned char BIT[4] = {MW_DENS, MW_RNG, MW_VAR, MW_TIME};
    mjam_note_t alt[SEQ_LANE_STEPS];
    unsigned w = mel_weak_notes(p, lines, g16);
    for (int k = 0; k < 4; k++) {
        mjam_set_t z = *set;
        signed char *f = k == 0 ? &z.dens : k == 1 ? &z.rng : k == 2 ? &z.var : &z.tm;
        if (!*f) continue;
        *f = 0;
        render_mel_into(s, p, h, &z, alt, NULL, NULL);
        if (mel_few(mel_differ(lines, alt, p->bars * SEQ_STEPS), p->bars * SEQ_STEPS)) w |= BIT[k];
    }
    return w;
}

/* 0.20 a block queued for part p (seq_mel_queue): the whole loop with it into mj_next and mj_next_line, which seq_block copies in at
 * the boundary; 0.21 with its swing grid and weak flags (mj_next_grid16, mj_next_weak); h: mel_harm's chords (NULL: worked out here) */
static void render_queued(seq_t *s, const seq_part_t *p, const mel_harm_t *h) {
    mjam_set_t q = p->mjam.set;
    mel_harm_t own;
    if (s->mjq_c < 0 || p != &s->part[s->mjq_part]) return;
    if (!h) mel_harm(s, p, &own), h = &own;
    q.c = s->mjq_c, q.l = s->mjq_l;
    {
        int g = 0;
        render_mel_into(s, p, h, &q, s->mj_next_line, s->mj_next, &g);
        s->mj_next_grid16 = (unsigned char)g;
        s->mj_next_weak = (unsigned char)mel_weak(s, p, h, &q, s->mj_next_line, g);
    }
}

/* 0.20 a JAM Melody lane's steps and lines (mline), and a block queued for the part, so the one that takes over has every change
 * made since it was queued. Setters only: never on the audio thread. 0.21 its swing grid and weak flags; one mel_harm for every render */
static void render_mel(seq_t *s, seq_part_t *p) {
    mel_harm_t h;
    int g = 0;
    mel_harm(s, p, &h);
    render_mel_into(s, p, &h, &p->mjam.set, p->mline, p->lane[LANE_MEL_UP].steps, &g);
    p->mjam.grid16 = (unsigned char)g;
    p->mjam.weak = (unsigned char)mel_weak(s, p, &h, &p->mjam.set, p->mline, g);
    render_queued(s, p, &h);
}

static void jam_apply_part(seq_part_t *p, int d, int c, int l);

/* 0.22 the block queued on drawn lane d (seq_jam_queue) applied to a copy of its part (jam_tmp) and rendered there with the part-level
 * renders only (revoice, never colour_changed; every bar with render_rhythm, the Bass's notes in it), then what the lane plays from
 * copied into djq[d] (djq_t), which seq_block installs at the boundary (djq_commit). Setters only: never on the audio thread */
static void djq_render(seq_t *s, int d) {
    djq_t *q = &s->djq[d];
    seq_part_t *t = &s->jam_tmp;
    int lane = DLANE[d].lane;
    *t = s->part[q->part];
    jam_apply_part(t, d, q->c, q->l);
    if (d == DL_CHORDS) revoice(s, t);
    render_rhythm(s, t, lane);
    memcpy(q->steps, t->lane[lane].steps, sizeof q->steps);
    memcpy(q->rb, t->rb[d], sizeof q->rb);
    memcpy(q->rfill, t->rfill[d], sizeof q->rfill);
    memcpy(q->rfill_abs, t->rfill_abs[d], sizeof q->rfill_abs);
    memcpy(q->sflag, t->sflag[d], sizeof q->sflag);   /* 0.24 */
    memcpy(q->hoct, t->hoct, sizeof q->hoct);   /* 0.24 final review: a hit the block takes away loses its octave (bar_steps) */
    q->grid16 = t->djam[d].grid16;
    q->f.fon = t->lw[DL_COUNT - 1 - d] != LW_OWN && lane_drawn(t, DLANE[DL_COUNT - 1 - d].lane);   /* 0.24 the lane that follows: rendered with it */
    if (q->f.fon) {
        int o = DL_COUNT - 1 - d;
        memcpy(q->f.steps, t->lane[DLANE[o].lane].steps, sizeof q->f.steps);
        memcpy(q->f.rfill, t->rfill[o], sizeof q->f.rfill);
        memcpy(q->f.rfill_abs, t->rfill_abs[o], sizeof q->f.rfill_abs);
        memcpy(q->f.sflag, t->sflag[o], sizeof q->f.sflag);
        memcpy(q->f.brole, t->brole, sizeof q->f.brole);
    }
    if (d == DL_BASS) {
        memcpy(q->brole, t->brole, sizeof q->brole);
        q->bset = t->bass.set, q->nlock = t->bass.nlock;
        memcpy(q->frozen, t->bass.frozen, sizeof q->frozen);
    } else {
        q->vstyle = t->vstyle, q->vmode = t->vmode, q->vcol = t->vcol, q->vinv = t->vinv, q->vspr = t->vspr, q->vstrdir = t->vstrdir;
        q->strum = t->lane_shape[LANE_CHORDS][SHAPE_STRUM], q->oct = t->lane_shape[LANE_CHORDS][SHAPE_OCT], q->voicing = t->voicing;
        memcpy(q->voice, t->voice, sizeof q->voice);
        memcpy(q->voice_n, t->voice_n, sizeof q->voice_n);
        memcpy(q->slot_text, t->slot_text, sizeof q->slot_text);
        memcpy(q->slot_num, t->slot_num, sizeof q->slot_num);
    }
    s->djq_renders++;
}

/* 0.22 a setter rendered drawn lane d (-1: none) of part p: its derived JAM state (dj_derive: the swing grid, the weak flags), then a block
 * queued for it renders again, so the one that takes over has every change made since the tap (as MELODY's render_queued). Every
 * setter that revoices renders the Chords lane after (regen_harmony, regen_part); the ones that only revoice change what a block sets
 * anyway (the voicing) or nothing the voices read (a slot lock that keeps the phrase). The scratch part's renders: no recursion */
static void dj_derive(const seq_t *s, seq_part_t *p, int d);
static void djq_own(seq_t *s, seq_part_t *p, int d) {
    if (d < 0) return;
    dj_derive(s, p, d);
    if (s->djq[d].c >= 0 && p == &s->part[s->djq[d].part]) djq_render(s, d);
}
/* 0.24 djq_own, and the other lane's block again when this lane follows it (or its render held this lane: djq_t.f), as it renders this
 * lane with the block's codes */
static void djq_follow(seq_t *s, seq_part_t *p, int d) {
    int o = DL_COUNT - 1 - d;
    djq_own(s, p, d);
    if (d >= 0 && s->djq[o].c >= 0 && p == &s->part[s->djq[o].part] && (s->djq[o].f.fon || p->lw[d] != LW_OWN)) djq_render(s, o);
}

/* 0.20 JAM Melody bars [from, to) repeat a `bars`-bar loop's NEW BAR variations, locks, frozen settings and fixes: what a grow puts
 * in bars never filled (as a DRAWN Bass's, bass_repeat_bar) */
static void mel_repeat(seq_part_t *p, int from, int to, int bars) {
    for (int b = from; b < to; b++) {
        int a = b % bars;
        p->lane[LANE_MEL_UP].bar_var[b] = p->lane[LANE_MEL_UP].bar_var[a];
        p->mjam.frozen[b] = p->mjam.frozen[a];
        p->mjam.nlock = (unsigned short)((p->mjam.nlock & ~(1u << b)) | (unsigned)(p->mjam.nlock >> a & 1) << b);
        memcpy(&p->mjam.fix[b * SEQ_STEPS], &p->mjam.fix[a * SEQ_STEPS], SEQ_STEPS);
    }
}

/* A generated lane takes the generator's steps, except where a hand edit locked them; a MANUAL lane is left alone. 0.18: a
 * DRAWN lane renders its bars; 0.20 a JAM Melody its model. */
static void regen(seq_t *s, seq_part_t *p, int lane) {
    seq_lane_t *ln = &p->lane[lane];
    seq_step_t st[SEQ_LANE_STEPS];
    if (lane_drawn(p, lane)) {
        render_rhythm(s, p, lane);
        return;
    }
    if (jam_lane(p, lane)) {   /* 0.20: the melody model */
        render_mel(s, p);
        return;
    }
    if (ln->gen.style == GEN_MANUAL) return;
    gen_steps(s, p, lane, st);
    for (int i = 0; i < p->bars * SEQ_STEPS; i++)
        if (!locked(ln, i)) ln->steps[i] = st[i];
}

/* After Melody 1's steps changed by any path (a regeneration, a tap, an undo, Manual or not): an Answer on Melody 2
 * regenerates over them. The part-wide loops need no call: they run the lanes in order, Melody 2 after Melody 1. */
static void follow(seq_t *s, seq_part_t *p, int lane) {
    if (lane == LANE_MEL_UP && follows_mel1(p, LANE_MEL_DOWN)) regen(s, p, LANE_MEL_DOWN);
}

/* One lane's regeneration, and Melody 2 after it if it follows */
static void regen_lane(seq_t *s, seq_part_t *p, int lane) {
    regen(s, p, lane);
    follow(s, p, lane);
}

/* The lanes of mask m (bit l: lane l) regenerated in lane order; 0.24.1 both drawn lanes in m: once each (render_pair) */
static void regen_lanes(seq_t *s, seq_part_t *p, unsigned m) {
    int pair = (m & 3u) == 3u && both_drawn(p);   /* LANE_CHORDS 0, LANE_BASS 1 */
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++)
        if (pair && l == LANE_CHORDS) render_pair(s, p, pair_first(p));
        else if (m >> l & 1 && !(pair && l == LANE_BASS)) regen(s, p, l);
}

static void regen_part(seq_t *s, seq_part_t *p) {   /* in lane order: an Answer sees Melody 1's new steps */
    regen_lanes(s, p, (1u << SEQ_ACTIVE_LANES) - 1);
}

/* A lane's steps depend on the key, scale or progression: its style reads them (gen_reads_harmony), or it is a shaped
 * Bass (MOVEMENT and the approach place tones by pitch and read the chords' colours) */
static int reads_harmony(const seq_part_t *p, int lane) {
    return gen_reads_harmony(p->lane[lane].gen.style) || (lane == LANE_BASS && p->lane[lane].gen.style != GEN_MANUAL && bass_shaped(p));
}

/* After a key, scale or progression change: only the lanes that read them change (reads_harmony: the style's
 * gen_reads_harmony, or a shaped Bass, whose transforms read the chords). The
 * setters run while the audio thread waits, so the others are not regenerated to the same steps. */
static void regen_harmony(seq_t *s, seq_part_t *p) {
    unsigned m = 0;
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) m |= (unsigned)reads_harmony(p, l) << l;
    regen_lanes(s, p, m);
}

/* Melody 1 moved with the key as generated output does: a generated melody with no hand-edited note sounding (those
 * keep their tones over new roots). An Answer over it may then transpose too. */
static int lead_transposed(const seq_part_t *p) {
    const seq_lane_t *ln = &p->lane[LANE_MEL_UP];
    if (!gen_transposes(ln->gen.style)) return 0;
    for (int i = 0; i < p->bars * SEQ_STEPS; i++)
        if (locked(ln, i) && ln->steps[i].on) return 0;
    return 1;
}

/* A melody's unlocked steps from key `from` to the current key: the same steps a regeneration writes (gen_transpose_v1) */
static void transpose_lane(const seq_t *s, seq_part_t *p, int lane, int from) {
    seq_lane_t *ln = &p->lane[lane];
    seq_step_t st[SEQ_LANE_STEPS];
    gen_params_t gp = gen_params(s, p, lane);
    memcpy(st, ln->steps, sizeof st);
    gen_transpose_v1(&gp, from, st);
    for (int i = 0; i < p->bars * SEQ_STEPS; i++)
        if (!locked(ln, i)) ln->steps[i] = st[i];
}

/* After a key change: the melodies transpose, in lane order (an Answer after its lead); the walk, and an Answer over
 * hand-edited lead notes, regenerate. */
static void rekey(seq_t *s, seq_part_t *p, int from) {
    int pair = both_drawn(p);   /* 0.24.1 both drawn lanes once each (regen_lanes); both read harmony, neither transposes */
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) {
        int style = p->lane[l].gen.style;
        if (!reads_harmony(p, l) || (pair && l == LANE_BASS)) continue;
        if (pair && l == LANE_CHORDS) render_pair(s, p, pair_first(p));
        else if (gen_transposes(style) && (!follows_mel1(p, l) || lead_transposed(p))) transpose_lane(s, p, l, from);
        else regen(s, p, l);
    }
}

/* A part brought up to the key and scale: voiced again and its generated lanes regenerated, which is what rekey's
 * transposes write too (test_gen_key_transposes). On the UI thread only: a whole part is too slow for seq_block. */
static void refresh(seq_t *s, int part) {
    revoice(s, &s->part[part]);
    regen_part(s, &s->part[part]);
    s->stale_mask &= ~(1 << part);
}

/* After a key or scale change, already applied to the playing part: a queued part catches up now (seq_block may switch
 * to it at any bar line), the other parts in use when next selected (seq_select_part). The setters run while the audio
 * thread waits, so they do not regenerate parts that may never play. */
static void others_follow(seq_t *s) {
    s->stale_mask |= s->used_mask & ~(1 << s->cur_part);
    if (s->queued >= 0) refresh(s, s->queued);
}

/* 0.20 the part's Melody lane JAM at level 1 with GATE 50 (new instances, INIT); its melody state as mjam_init left it */
static void jam_default(seq_part_t *p) {
    p->lane[LANE_MEL_UP].gen = (seq_gen_t){GEN_MEL_JAM, 1, MEL_JAM_CX, 0};
    p->mel.kn[MK_GATE] = 50;
}

/* Melody 2 is retired: empty and Manual, whatever a saved state or v1_part put there */
static void retire(seq_part_t *p) {
    memset(&p->lane[LANE_MEL_DOWN], 0, sizeof p->lane[0]);
    p->lane[LANE_MEL_DOWN].gen = MANUAL_GEN;
}

void seq_init(seq_t *s, seq_emit_fn emit, void *ctx) {
    unsigned char pat[SEQ_LANES][SEQ_STEPS];
    rhythm_init();
    level_table_init();
    memset(s, 0, sizeof *s);
    s->emit = emit;
    s->ctx = ctx;
    s->rate = 44100.0;
    s->cur_k = -1;
    forget_pushes(s);
    s->queued = -1;
    s->mjq_c = -1;   /* 0.20: no JAM block queued */
    for (int d = 0; d < DL_COUNT; d++) s->djq[d].c = -1;   /* 0.22: on the drawn lanes neither */
    s->used_mask = 1;   /* the Verse */
    s->view_bar = 0;
    s->follow = 1;   /* on by default (owner, after 0.16); off saves MPC's screen thread (+12.6 points on PLAY, about 17 on FIX) */
    s->fix_prev = -1;
    s->key = 4;
    s->scale = SCALE_MINOR;
    for (int l = 0; l < SEQ_LANES; l++) s->chan[l] = l + 1;
    s->gate = s->dyn = 50;
    v1_default_pat(pat);
    for (int p = 0; p < SEQ_PARTS; p++) {
        v1_part(&s->part[p], pat);
        set_vmode(&s->part[p], VMODE_SMOOTH);   /* v1_part is close, so load_v1 stays close; new instances are smooth */
        s->part[p].vstyle = VOICING_DEFAULT;   /* Pop Piano: Smooth and every knob 0, 0.16's sound */
    }
    cur(s)->lane[LANE_CHORDS].gen = (seq_gen_t){GEN_CH_DRAWN, 1, CHORDS_CX_NEUTRAL, 0};   /* 0.18: Sustain bars (v1_part) */
    cur(s)->lane[LANE_BASS].gen = (seq_gen_t){GEN_BASS_DRAWN, 1, BASS_CX_NEUTRAL, 0};   /* 0.19: Root bars, Pop Root (v1_part) */
    jam_default(cur(s));   /* 0.20: Melody JAM, ARP · UP (v1_part's mjam_init) */
    for (int p = 0; p < SEQ_PARTS; p++) retire(&s->part[p]);
    revoice(s, cur(s));   /* the other parts are unused: their first use copies the Verse */
    regen_part(s, cur(s));
}

static void render_drawn_lanes(seq_t *s, seq_part_t *p);

void seq_reset_v1(seq_t *s) {
    unsigned char pat[SEQ_LANES][SEQ_STEPS];
    v1_default_pat(pat);
    act(s);
    v1_part(cur(s), pat);   /* every lane MANUAL, close voicing */
    retire(cur(s));
    s->undo_n = 0;
    revoice(s, cur(s));
    render_drawn_lanes(s, cur(s));   /* 0.18 (R19): v1's Chords steps as drawn bars (0.19: and its Bass steps) */
}

static int part_idx(const seq_t *s, long k);

static int part_index(const seq_t *s) { return s->cur_k < 0 ? -1 : part_idx(s, s->cur_k); }   /* the playing step, or -1 */

int seq_cur_step(const seq_t *s) { int i = part_index(s); return i < 0 ? -1 : i % SEQ_STEPS; }

int seq_cur_bar(const seq_t *s) { int i = part_index(s); return i < 0 ? 0 : i / SEQ_STEPS; }

int seq_cur_slot(const seq_t *s) {
    int i = part_index(s);
    return i < 0 ? -1 : (i / ccur(s)->chord_len) % ccur(s)->prog_len;
}

/* 0.24 (ruling 31) the step of bar-step t where the real slot holding it starts in its bar: a slot shorter than a bar starts on a
 * multiple of chord_len (which divides 16); a bar's or a 2-bar slot's (its second bar too) on step 0 */
static int slot_start_in_bar(const seq_part_t *p, int t) { return p->chord_len < SEQ_STEPS ? t - t % p->chord_len : 0; }

/* The slot under a step (bar and step may run past the bar: a loop step, wrapping). 0.24: the last change point of its bar at or
 * before the step and at or after the start of the real slot that holds the step (ruling 31: a point lasts to the next slot's start,
 * so at 1/2- and 1/4-bar chords a later slot of the bar sounds its own chord), its virtual slot; else the real slot. Read on the audio
 * thread: a scan of 4 entries */
static int slot_at(const seq_part_t *p, int bar, int step) {
    int i = bar * SEQ_STEPS + step, n = p->bars * SEQ_STEPS, v = -1, t = i % SEQ_STEPS, s0 = slot_start_in_bar(p, t);
    const seq_cp_t *c = p->cp[i % n / SEQ_STEPS];
    for (int k = 0; k < SEQ_CP_MAX && c[k].step >= 0 && c[k].step <= t; k++)
        if (c[k].step >= s0) v = k;
    return v >= 0 ? SEQ_MAX_PROG + i % n / SEQ_STEPS * SEQ_CP_MAX + v : (i / p->chord_len) % p->prog_len;
}

int seq_slot_at(const seq_t *s, int bar, int step) { return slot_at(ccur(s), bar, step); }

/* the slots a loop plays: 0.14 every slot of the list (a 2-bar chord in 1 bar still plays: 1) */
static int slots_reached(const seq_part_t *p) { return p->prog_len; }

/* 0.24 a slot a getter reads: one the loop reaches, or a live change point's */
static int slot_read_ok(const seq_part_t *p, int slot) { return (slot >= 0 && slot < slots_reached(p)) || vslot_live(p, slot); }

int seq_slot_name(const seq_t *s, int slot, char *buf, int len) {
    if (!slot_read_ok(ccur(s), slot)) return copy_text(buf, len, " ");
    return seq_slot_label(s, slot, buf, len);
}

/* 0.24 a point into bar b at its step (kept sorted), or the chord of the one already there replaced; 0 when the bar is full */
static int cp_put(seq_part_t *p, int b, int step, int deg, int src, int type) {
    seq_cp_t *c = p->cp[b];
    int k = 0;
    while (k < SEQ_CP_MAX && c[k].step >= 0 && c[k].step < step) k++;
    if (k == SEQ_CP_MAX || (c[k].step != step && c[SEQ_CP_MAX - 1].step >= 0)) return 0;
    if (c[k].step != step) memmove(c + k + 1, c + k, (size_t)(SEQ_CP_MAX - 1 - k) * sizeof *c);
    c[k] = (seq_cp_t){(signed char)step, (signed char)deg, (signed char)src, (signed char)type};
    return 1;
}

/* 0.24 the change-point setters on part p, each revoiced (cp_sync, then the points' voices); the caller renders the lanes and pushes
 * any undo step. cp_set: a chord deg (src 0) or a borrow (src 1..3, its degree) of type 0..4 at a step of a bar of the loop; 0 for a
 * bad value, step 0 of a bar that starts a slot (a slot change there) or a full bar. cp_clear: the point at the step (none: nothing);
 * cp_clear_bar: every point of the bar */
static int cp_set(const seq_t *s, seq_part_t *p, int bar, int step, int deg, int src, int type) {
    if (bar < 0 || bar >= p->bars || step < 0 || step >= SEQ_STEPS || (!step && bar_starts_slot(p, bar)) || src < 0 || src > THEORY_NBORROW ||
        (!src && (deg < 0 || deg > 6)) || type < 0 || type >= CP_TYPES)
        return 0;
    if (src) deg = theory_borrow_degree(s->scale, src);
    if (!cp_put(p, bar, step, deg, src, type)) return 0;
    revoice(s, p);
    return 1;
}

static int cp_clear(const seq_t *s, seq_part_t *p, int bar, int step) {
    for (int k = 0; bar >= 0 && bar < SEQ_MAX_BARS && step >= 0 && k < SEQ_CP_MAX; k++)
        if (p->cp[bar][k].step == step) {
            cp_drop(p, bar, k);
            revoice(s, p);
            return 1;
        }
    return 0;
}

static int cp_clear_bar(const seq_t *s, seq_part_t *p, int bar) {
    if (bar < 0 || bar >= SEQ_MAX_BARS || p->cp[bar][0].step < 0) return 0;
    for (int k = 0; k < SEQ_CP_MAX; k++) p->cp[bar][k] = CP_NONE;
    revoice(s, p);
    return 1;
}

/* 0.24 final review: a point that plays what its step would play without it (the point before it in its real slot, else the slot:
 * deg, borrow and the colour heard, chord_change_at's test) changes nothing and is removed, so no label reads `X·X@9` and PUSH
 * and ANTICIPATE never take it for a change. A slot pick under it or a change to the point before it makes one; the caller's undo
 * step holds it (its snapshot has the points). After each the slots settled and the points mirrored (the tests read no voices), voiced
 * once at the end (0.24.1); 1 when one went */
static int cp_prune(const seq_t *s, seq_part_t *p) {
    int any = 0;
    for (int b = 0; b < p->bars; b++)
        for (int k = 0; k < SEQ_CP_MAX && p->cp[b][k].step >= 0;) {
            int t = p->cp[b][k].step, v = SEQ_MAX_PROG + b * SEQ_CP_MAX + k;
            int u = k > 0 && p->cp[b][k - 1].step >= slot_start_in_bar(p, t) ? v - 1 : (b * SEQ_STEPS + t) / p->chord_len % p->prog_len;
            if (p->prog[v] != p->prog[u] || p->src[v] != p->src[u] || eff_colour(s, p, v) != eff_colour(s, p, u)) {
                k++;
                continue;
            }
            cp_drop(p, b, k);
            borrow_degrees(s, p), settle_slots(p), cp_sync(p);   /* revoice's steps before the voices */
            any = 1;
        }
    if (any) revoice(s, p);
    return any;
}

int seq_cp_set(seq_t *s, int bar, int step, int deg, int src, int type) {
    if (type >= PICK_TYPES || !cp_set(s, cur(s), bar, step, deg, src, type)) return 0;
    act(s);
    regen_harmony(s, cur(s));
    return 1;
}

void seq_cp_clear(seq_t *s, int bar, int step) {
    if (cp_clear(s, cur(s), bar, step)) act(s), regen_harmony(s, cur(s));
}

void seq_cp_clear_bar(seq_t *s, int bar) {
    if (cp_clear_bar(s, cur(s), bar)) act(s), regen_harmony(s, cur(s));
}

int seq_cp_count(const seq_t *s, int bar) {
    int n = 0;
    while (bar >= 0 && bar < SEQ_MAX_BARS && n < SEQ_CP_MAX && ccur(s)->cp[bar][n].step >= 0) n++;
    return n;
}

int seq_cp_get(const seq_t *s, int bar, int k, seq_cp_t *out) {
    if (k < 0 || k >= seq_cp_count(s, bar)) return 0;
    if (out) *out = ccur(s)->cp[bar][k];
    return 1;
}

/* 0.24 seq_chord_change_at on part p (ANTICIPATE's pass renders scratch parts too) */
static int chord_change_at(const seq_t *s, const seq_part_t *p, int bar, int step) {
    int i = bar * SEQ_STEPS + step, n = p->bars * SEQ_STEPS, v, w;
    if (bar < 0 || bar >= p->bars || step < 0 || step >= SEQ_STEPS) return 0;
    v = slot_at(p, bar, step);
    if (v >= SEQ_MAX_PROG) return p->cp[bar][(v - SEQ_MAX_PROG) % SEQ_CP_MAX].step == step;   /* a point (one before: no change) */
    if (i % p->chord_len && step) return 0;   /* 0.24.2 a bar's step 0 too: a point before it ends at the bar line (2-bar chords) */
    w = slot_at(p, 0, (i + n - 1) % n);
    return p->prog[v] != p->prog[w] || p->src[v] != p->src[w] || eff_colour(s, p, v) != eff_colour(s, p, w);
}
int seq_chord_change_at(const seq_t *s, int bar, int step) { return chord_change_at(s, ccur(s), bar, step); }

static void chord_word(const seq_t *s, int deg, int src, int colour, char *buf, int len);

int seq_chord_label(const seq_t *s, int bar, char *buf, int len) {
    const seq_part_t *p = ccur(s);
    char t[SEQ_STEPS * 24], w[16];
    int n = 0;
    t[0] = 0;
    for (int i = 0; bar >= 0 && bar < p->bars && i < SEQ_STEPS; i++) {
        int v = slot_at(p, bar, i);
        if (i && !seq_chord_change_at(s, bar, i)) continue;
        chord_word(s, p->prog[v], p->src[v], eff_colour(s, p, v), w, sizeof w);
        if (i) memcpy(t + n, "\xC2\xB7", 2), n += 2;
        n += copy_text(t + n, (int)sizeof t - n, w);
        if (!i) continue;
        t[n++] = '@';
        if (i >= 9) t[n++] = '1';
        t[n++] = (char)('0' + (i + 1) % 10);
        t[n] = 0;
    }
    return copy_text(buf, len, t);
}

int seq_bars(const seq_t *s) { return ccur(s)->bars; }

int seq_chord_len(const seq_t *s) { return ccur(s)->chord_len; }


int seq_view_bar(const seq_t *s) { return s->view_bar; }

int seq_grid_bar(const seq_t *s) { return s->follow && s->cur_k >= 0 ? seq_cur_bar(s) : s->view_bar; }

int seq_strip_bar(const seq_t *s) { return seq_grid_bar(s); }

int seq_fix_bar(const seq_t *s) {
    if (s->follow && s->cur_k >= 0 && s->fix_prev >= 0 && s->fix_prev < ccur(s)->bars &&
        s->last_ppq >= s->fix_ppq && s->last_ppq - s->fix_ppq < 0.5)   /* a host loop back into the bar is not a bar change */
        return s->fix_prev;
    return seq_grid_bar(s);
}

int seq_key(const seq_t *s) { return s->key; }

int seq_scale(const seq_t *s) { return s->scale; }

int seq_voicing(const seq_t *s) { return ccur(s)->voicing; }

/* 0.14: the preset whose phrase is the loop's (its length the phrase's, its chords every slot's, a longer phrase's hidden
 * slots included): what 0.13's equal progression was */
static int preset_of(const prog_view_t *v) {
    int n = v->phrase > v->len ? v->phrase : v->len;
    for (int i = 0; i < n; i++)
        if (v->src[i] || v->colour[i]) return -1;   /* a borrowed or coloured slot is no preset's */
    for (int i = 0; i < THEORY_NPRESETS; i++) {
        const theory_preset_t *r = &THEORY_PRESETS[i];
        int k = 0;
        if (r->len != v->phrase) continue;
        while (k < n && v->prog[k] == r->deg[k % r->len]) k++;
        if (k == n) return i;
    }
    return -1;
}

int seq_prog_preset(const seq_t *s) {
    prog_view_t v = part_view(ccur(s));
    return preset_of(&v);
}

/* Setting what is already set changes nothing, so it does no work (a host re-sends values it already sent). */
void seq_set_key(seq_t *s, int key) {
    int from = s->key;
    if (key < 0 || key > 11 || key == from) return;
    act(s);
    s->key = key;
    revoice(s, cur(s));
    rekey(s, cur(s), from);
    others_follow(s);
}

void seq_set_scale(seq_t *s, int scale) {
    if (scale < 0 || scale >= SCALE_COUNT || scale == s->scale) return;
    act(s);
    s->scale = scale;
    for (int p = 0; p < SEQ_PARTS; p++) borrow_degrees(s, &s->part[p]);   /* every part saves its borrows' new degrees */
    revoice(s, cur(s));
    regen_harmony(s, cur(s));
    others_follow(s);
}

static void push_undo(seq_t *s, int lane);
static void colour_changed(seq_t *s, seq_part_t *p);

/* Leaving a Custom progression (HARMONY GEN's, matching no preset) pushes an undo snapshot; preset to preset doesn't.
 * A preset replaces every slot: borrows, colours and locks go (an explicit choice). */
void seq_set_prog_preset(seq_t *s, int i) {
    seq_part_t *p = cur(s);
    prog_view_t y = part_view(ccur(s));
    int was = preset_of(&y);
    if (i < 0 || i >= THEORY_NPRESETS || (was == i && !y.lock)) return;   /* a re-send */
    seq_txn_done(s);
    act(s);
    if (was < 0 || p->slot_lock) push_undo(s, -1);   /* borrows, colours (was < 0) or locks go: undoable */
    memset(p->src, 0, sizeof p->src);
    memset(p->colour, 0, sizeof p->colour);
    p->slot_lock = 0;
    fill_phrase(p, THEORY_PRESETS[i].len, THEORY_PRESETS[i].deg, NULL, NULL);   /* 0.14: the phrase over the loop */
    p->rank_len = THEORY_PRESETS[i].len;   /* a deal: the ranks follow the new phrase */
    revoice(s, p);
    regen_harmony(s, p);
}

void seq_set_voicing(seq_t *s, int v) {
    if (v != VOICING_CLOSE && v != VOICING_SMOOTH) return;
    seq_set_vmode(s, v == VOICING_CLOSE ? VMODE_CLOSE : VMODE_SMOOTH);
}

void seq_set_vmode(seq_t *s, int mode) {
    if (mode < 0 || mode >= VMODE_COUNT) return;
    if (mode != cur(s)->vmode) act(s);
    set_vmode(cur(s), mode);
    revoice(s, cur(s));
}

/* REGISTER, one-sided so it moves smoothly: below 50 a note above the ceiling 60 - (50 - v) x 24/50 (59.5 at 49, 36 at 0)
 * folds down by octaves to at or below it; above 50 a note below the floor 28 + (v - 50) x 24/50 (28.5 at 51, E3 52 at
 * 100) folds up to at or above it; 50 leaves it alone. Lowering v never raises a note, raising it never lowers one. */
static int fold_register(int note, int v) {
    if (v < 50)
        for (double c = 60.0 - (50 - v) * 24 / 50.0; note > c;) note -= 12;
    else if (v > 50)
        for (double f = 28.0 + (v - 50) * 24 / 50.0; note < f;) note += 12;
    return note;
}

/* CLUSTER's note: the slot scale's 2nd above the chord's root, where the root is in the chord (its lowest), else where it
 * would sit at or below the lowest note. v ascending, n >= 1. */
static int cluster_note(const seq_t *s, const seq_part_t *p, int slot, const int *v, int n) {
    int sc = slot_scale(s, p, slot), deg = p->prog[slot], root = theory_degree_pc(s->key, sc, deg), r = v[0] - (int)floor_mod(v[0] - root, 12);
    for (int i = n - 1; i >= 0; i--)
        if (floor_mod(v[i], 12) == root) r = v[i];
    return r + (int)floor_mod(theory_degree_pc(s->key, sc, deg + 1) - root, 12);
}

/* A slot's Chords-lane notes at a step octave `oct`, ascending: its voicing under INVERSION `inv` (each step raises the lowest note
 * an octave, or drops the highest one), SPREAD `spread` (Cluster adds the 2nd above the root, Open lifts the 2nd note from the
 * bottom an octave, Split drops the lowest one, Wide does both) and OCTAVE (part_step_notes). Returns voice_n, one more with
 * Cluster. */
static int spread_notes(const seq_t *s, const seq_part_t *p, int slot, int oct, int inv, int spread, int out[THEORY_CHORD_MAX]) {
    int n = p->voice_n[slot];
    oct += lshape(p, LANE_CHORDS, L_OCT, 0);
    for (int i = 0; i < n; i++) out[i] = p->voice[slot][i];
    for (int k = 0; n > 0 && k < iabs(inv); k++) {
        if (inv > 0) out[0] += 12;
        else out[n - 1] -= 12;
        sort_ints(out, n);
    }
    if (spread < 0 && n > 0 && n < THEORY_CHORD_MAX) {
        int c = cluster_note(s, p, slot, out, n), dup = 0;
        for (int i = 0; i < n; i++) dup |= out[i] == c;
        if (!dup) out[n++] = c;
    }
    if ((spread == 1 || spread == 3) && n > 1) out[1] += 12;
    if (spread >= 2 && n > 0) out[0] -= 12;
    for (int i = 0; i < n; i++) out[i] += 12 * oct;
    sort_ints(out, n);   /* ascending again, so STRUM runs low to high */
    return n;
}

/* 0.18: SPREAD as it plays, under COMPLEXITY's spread layer */
static int eff_spread(const seq_part_t *p) {
    int cx = chords_cx(p);
    return cx == CHORDS_CX_NEUTRAL ? p->vspr : rhythm_cx_apply(CXT_SPR, p->vspr, rhythm_cx_offsets(cx).spr);
}

/* 1 when two of the (ascending) notes are a 2nd or less apart, the lower below E3 (52): the low interval limit */
static int low_second(const int *v, int n) {
    for (int i = 1; i < n; i++)
        if (v[i - 1] < 52 && v[i] - v[i - 1] <= 2) return 1;
    return 0;
}

/* 0.22 JAM VARIATION's INVERSION steps on loop bar `bar` of the Chords lane (-1: none, an audition): +1 / +2 / -1 at phrase position
 * 1 / 2 / 3 when the bar varies (jam_var_pos). Arithmetic only: it is read where the chord plays, on the audio thread */
static const signed char JAM_INV[4] = {0, 1, 2, -1};
static int jam_inv(const seq_part_t *p, int bar) { return bar < 0 ? 0 : JAM_INV[jam_var_pos(p, DL_CHORDS, bar)]; }

/* A slot's Chords-lane notes at a step octave `oct` (spread_notes at the effective SPREAD) in loop bar `bar` (-1 none: 0.22, its JAM
 * VARIATION's INVERSION steps, jam_inv). 0.18 coupling rule 1, on a DRAWN
 * lane: a 9th, 11th or 13th whose notes break the low interval limit plays one SPREAD step wider (never when the user's
 * SPREAD is Cluster, ruling R17, which asks for the 2nd). Read where it plays (the
 * INVERSION, SPREAD and OCTAVE knobs never revoice), so it is never stale. */
static int chord_notes_inv(const seq_t *s, const seq_part_t *p, int slot, int inv, int oct, int out[THEORY_CHORD_MAX]) {
    int spread = eff_spread(p), n = spread_notes(s, p, slot, oct, inv, spread, out), c;
    if (!drawn_lane(p) || p->vspr < 0 || spread >= 3 || !low_second(out, n)) return n;   /* R17: not on the user's Cluster */
    c = eff_colour(s, p, slot);
    return c == COL_9TH || c == COL_11TH || c == COL_13TH ? spread_notes(s, p, slot, oct, inv, spread + 1, out) : n;
}
static int chord_notes(const seq_t *s, const seq_part_t *p, int slot, int bar, int oct, int out[THEORY_CHORD_MAX]) {
    return chord_notes_inv(s, p, slot, p->vinv + jam_inv(p, bar), oct, out);   /* 0.22 chord_notes_inv: at INVERSION inv (dj_same) */
}

/* 0.22 a drawn lane's derived JAM state (djam_t grid16, weak), worked out where the setters render the lane (djq_follow) */

/* 0.22 drawn lane d's swing grid: 1 (16ths) when a bar of the loop not locked for JAM hits an odd step in its codes after TIME and
 * VARIATION (jam_codes: before COMPLEXITY's layers and FILL, so their 16ths never flip it), else 0 (8ths). A locked bar plays straight */
static int dj_grid16(const seq_part_t *p, int d) {
    unsigned char c[16];
    for (int b = 0; b < p->bars; b++) {
        if (jam_locked(p, d, b)) continue;
        jam_codes(p, d, b, c);
        for (int i = 1; i < SEQ_STEPS; i += 2)
            if (rhythm_is_hit(c[i])) return 1;
    }
    return 0;
}

/* 0.22 SWING's weak bit of drawn lane d (MELODY's mel_weak_notes): SWING off 50% with fewer than 1 in 8 of the hits of the loop's bars not
 * locked for JAM (a locked bar plays straight) on the swung positions (grid16: odd steps, else step mod 4 = 2). No render: a SWING turn
 * counts it again */
static unsigned dj_weak_swing(const seq_part_t *p, int d) {
    const seq_step_t *st = p->lane[DLANE[d].lane].steps;
    int hits = 0, swung = 0, g16 = p->djam[d].grid16;
    for (int i = 0; i < p->bars * SEQ_STEPS; i++) {
        if (!st[i].on || jam_locked(p, d, i / SEQ_STEPS)) continue;
        hits++;
        swung += g16 ? i & 1 : i % 4 == 2;
    }
    return p->djam[d].swing && mel_few(swung, hits) ? DW_SWING : 0u;
}

/* 0.22 drawn lane d's steps of the loop at VARIATION var and TIME tm into st (steps only: no FILL, no roles; a Bass hit's note), pos each
 * bar's VARIATION position (the Chords' INVERSION steps). A bar they leave as it plays (locked for JAM, or the same position at the same
 * TIME) is the lane's own: only the bars they move render. Setters only (dj_weak), the key k made once */
static void dj_steps_at(const seq_t *s, const seq_part_t *p, int d, int var, int tm, const bass_key_t *k, seq_step_t *st, unsigned char *pos) {
    int lane = DLANE[d].lane;
    for (int b = 0; b < p->bars; b++) {
        unsigned char c[16], fill[16], note[16];
        char role[16];
        bar_harm_t h;
        seq_step_t *x = &st[b * SEQ_STEPS];
        pos[b] = (unsigned char)jam_var_at(p, d, b, var);
        if (jam_locked(p, d, b) || (tm == p->djam[d].tm && pos[b] == jam_var_pos(p, d, b))) {
            memcpy(x, &p->lane[lane].steps[b * SEQ_STEPS], SEQ_STEPS * sizeof *x);
            continue;
        }
        render_bar_at(p, lane, b, var, tm, c, fill);
        rhythm_to_steps(c, x);
        if (d != DL_BASS) continue;
        bar_harm(s, p, b, k, &h);
        bass_bar_notes_at(p, b, NULL, &h, c, pos[b], note, role);
        for (int i = 0; i < SEQ_STEPS; i++)
            if (x[i].on) set_abs(&x[i], note[i]);
    }
}

/* 0.22 per step of a loop of n steps, the hit sounding there (its step; -1 silence): a hit sounds its len steps within its bar, until the
 * next one */
static void dj_spans(const seq_step_t *st, int n, short *at) {
    for (int i = 0, cur = -1, end = 0; i < n; i++) {
        if (i % SEQ_STEPS == 0) cur = -1;
        if (st[i].on) cur = i, end = i + st[i].len;
        at[i] = (short)(cur >= 0 && i < end ? cur : -1);
    }
}

/* 0.22 1 when hit x (step ja, its bar's VARIATION position pa) sounds what hit y (jb, pb) of the same bar does: a Bass hit's note; a Chords
 * hit's voicing (chord_notes_inv at its slot, the INVERSION steps of its position, its octave), worked out only when those differ; memo
 * (dj_differ's) keeps the answer per slot and position pair at one octave */
static int dj_same(const seq_t *s, const seq_part_t *p, int d, const seq_step_t *x, int ja, int pa, const seq_step_t *y, int jb, int pb,
                   signed char memo[SEQ_SLOTS_ALL][4][4]) {
    int bar = ja / SEQ_STEPS, sa = slot_at(p, bar, ja % SEQ_STEPS), sb = slot_at(p, bar, jb % SEQ_STEPS), na, u[THEORY_CHORD_MAX], v[THEORY_CHORD_MAX];
    signed char *m = sa == sb && x->oct == y->oct ? &memo[sa][pa][pb] : NULL;
    if (d == DL_BASS) return x->abs == y->abs;
    if (sa == sb && pa == pb && x->oct == y->oct) return 1;
    if (m && *m >= 0) return *m;
    na = chord_notes_inv(s, p, sa, p->vinv + JAM_INV[pa], x->oct, u);
    na = na == chord_notes_inv(s, p, sb, p->vinv + JAM_INV[pb], y->oct, v) && !memcmp(u, v, sizeof(int) * (size_t)na);
    if (m) *m = (signed char)na;
    return na;
}

/* 0.22 the loop's steps where drawn lane d sounds differently in steps a and b (VARIATION positions pa, pb per bar): a note start, silence,
 * or other notes (as MELODY's mel_differ; tests/test_knob_audit.c counts the same). A held step reuses its hits' answer */
static int dj_differ(const seq_t *s, const seq_part_t *p, int d, const seq_step_t *a, const unsigned char *pa, const seq_step_t *b,
                     const unsigned char *pb) {
    short sa[SEQ_LANE_STEPS], sb[SEQ_LANE_STEPS];
    signed char memo[SEQ_SLOTS_ALL][4][4];
    int n = p->bars * SEQ_STEPS, x = 0, la = -1, lb = -1, same = 1;
    dj_spans(a, n, sa), dj_spans(b, n, sb);
    memset(memo, -1, sizeof memo);
    for (int i = 0; i < n; i++) {
        int ja = sa[i], jb = sb[i];
        if (!a[i].on != !b[i].on || (ja < 0) != (jb < 0)) x++;
        else if (ja >= 0) {
            if (ja != la || jb != lb) la = ja, lb = jb, same = dj_same(s, p, d, &a[ja], ja, pa[i / SEQ_STEPS], &b[jb], jb, pb[i / SEQ_STEPS], memo);
            x += !same;
        }
    }
    return x;
}

/* 0.22 drawn lane d's DW_* bits as it plays (MELODY's mel_weak): VARIATION and TIME off neutral each render the loop's steps again with that
 * knob at neutral (dj_steps_at: the bars it moves) and are weak when fewer than 1 in 8 of the loop's steps sound different; SWING counts
 * its hits (dj_weak_swing). A neutral knob renders nothing; a lane not DRAWN has none */
static unsigned dj_weak(const seq_t *s, const seq_part_t *p, int d) {
    const djam_t *j = &p->djam[d];
    seq_step_t alt[SEQ_LANE_STEPS];
    unsigned char pos[SEQ_MAX_BARS], apos[SEQ_MAX_BARS];
    unsigned w;
    bass_key_t k;
    if (!lane_drawn(p, DLANE[d].lane)) return 0;
    w = dj_weak_swing(p, d);
    if (!j->var && !j->tm) return w;
    k = bass_key(s);
    for (int b = 0; b < p->bars; b++) pos[b] = (unsigned char)jam_var_pos(p, d, b);
    for (int f = 0; f < 2; f++) {   /* VARIATION OFF, TIME NORMAL */
        if (!(f ? j->tm : j->var)) continue;
        dj_steps_at(s, p, d, f ? j->var : 0, f ? 0 : j->tm, &k, alt, apos);
        if (mel_few(dj_differ(s, p, d, p->lane[DLANE[d].lane].steps, pos, alt, apos), p->bars * SEQ_STEPS)) w |= f ? DW_TIME : DW_VAR;
    }
    return w;
}

/* 0.22 a setter rendered drawn lane d of part p: its swing grid, and its weak flags but on the scratch part (a queued block gets none:
 * djq_render keeps its grid, which djq_commit installs with the flags at 0) */
static void dj_derive(const seq_t *s, seq_part_t *p, int d) {
    p->djam[d].grid16 = (unsigned char)dj_grid16(p, d);
    p->djam[d].weak = p == &s->jam_tmp ? 0 : (unsigned char)dj_weak(s, p, d);
}

/* 0.24 the slot the Chords lane's step plays: ANTICIPATE's early hit (SF_EARLY) the one at the change it ties over, else slot_at's */
static int step_slot(const seq_part_t *p, int bar, int step) {
    int early = lane_drawn(p, LANE_CHORDS) && (p->sflag[DL_CHORDS][bar * SEQ_STEPS + step] & SF_EARLY);
    return slot_at(p, bar, step + (early ? AN_STEPS[p->an[DL_CHORDS]] : 0));
}

/* 0.24 CADENCE on the Chords (stage 7 of THE ORDER IN A BAR), a play-time voicing offset as INVERSION's: on the phrase's last bar of a DRAWN
 * lane, not locked, the hit cad_step picks (the bar's last before FILL and ANTICIPATE; when ANTICIPATE moved it, its early hit, which may
 * sit in the bar before: fix round 1, ruling 17) plays its chord with the chord tone (5th OPEN, 3rd SOFT, root HOME) on top: the highest note of that pitch class moved up the fewest octaves that put it above the rest (re-sorted). On top
 * already, absent from the voicing (a power chord's 3rd, a sus chord's) or past MIDI 127: as it is. A fill pass's hit on that step plays
 * it too; FILL's own hits and ANTICIPATE's other early hits never. The notes v (n, ascending) in place; returns n. Audio thread: arithmetic only */
static int chord_cadence(const seq_t *s, const seq_part_t *p, int bar, int step, int *v, int n) {
    int cad = p->rk[DL_CHORDS][RK_CAD], pc, k = -1, i = bar * SEQ_STEPS + step;
    if (!cad || n < 2 || !drawn_lane(p)) return n;
    if (p->sflag[DL_CHORDS][i] & SF_EARLY) i = (i + AN_STEPS[p->an[DL_CHORDS]]) % (p->bars * SEQ_STEPS);   /* ruling 17: an early hit */
    if (!(cad = cad_at(p, DL_CHORDS, i))) return n;   /* is the cadence when the step it ties over is (it plays that chord: step_slot) */
    pc = cad_pc(s, p, i, cad);
    if (floor_mod(v[n - 1], 12) == pc) return n;
    for (int i = n - 2; i >= 0 && k < 0; i--)
        if (floor_mod(v[i], 12) == pc) k = i;
    if (k < 0) return n;
    int m = v[k] + 12 * ((v[n - 1] - v[k]) / 12 + 1);
    if (m > 127) return n;
    v[k] = m;
    sort_ints(v, n);
    return n;
}

/* The notes a lane's step plays (whether or not it is on), ascending: the slot's voicing for Chords, under INVERSION, SPREAD
 * and OCTAVE (chord_notes); else one tone through the slot's chord
 * (its scale, and a sus colour's 2 or 4 for the 3), folded by the Bass's REGISTER, then moved by the lane's OCTAVE, or an
 * absolute step's note moved by OCTAVE (on a generated Bass's unlocked step, the walk's approach, folded by REGISTER
 * first; a picked note, locked or on a MANUAL lane, never). Returns the count. */
static int part_step_notes(const seq_t *s, const seq_part_t *p, int lane, int bar, int step, int out[THEORY_CHORD_MAX]) {
    const seq_step_t *x = &p->lane[lane].steps[bar * SEQ_STEPS + step];
    int slot = slot_at(p, bar, step);
    if (lane == LANE_CHORDS)   /* 0.24 an early hit: the change's chord; CADENCE's hit its voicing (chord_cadence) */
        return chord_cadence(s, p, bar, step, out, chord_notes(s, p, step_slot(p, bar, step), bar, x->oct, out));
    if (lane_drawn(p, lane) || jam_lane(p, lane)) {   /* 0.19: a DRAWN lane's step (the Bass's; 0.20 a JAM Melody's) plays its note as
                                                       * rendered: no REGISTER, no OCTAVE */
        out[0] = x->abs;
        return 1;
    }
    if (x->abs) {   /* 0.10: an absolute step plays its note; REGISTER folds only a generated one, OCTAVE moves any */
        const seq_lane_t *ln = &p->lane[lane];
        int n = x->abs, i = bar * SEQ_STEPS + step;
        if (lane == LANE_BASS && ln->gen.style != GEN_MANUAL && !locked(ln, i))
            n = fold_register(n, p->lane_shape[LANE_BASS][BSHAPE_REGISTER]);
        n += 12 * lshape(p, lane, L_OCT, 0);
        out[0] = n < 0 ? 0 : n > 127 ? 127 : n;
        return 1;
    }
    out[0] = theory_tone_note(s->key, slot_scale(s, p, slot), p->prog[slot], theory_colour_tone(eff_colour(s, p, slot), x->tone), x->oct);
    if (lane == LANE_BASS) out[0] = fold_register(out[0], p->lane_shape[LANE_BASS][BSHAPE_REGISTER]);
    out[0] += 12 * lshape(p, lane, L_OCT, 0);   /* after REGISTER, so OCTAVE always moves the note an octave */
    return 1;
}

static int step_notes(const seq_t *s, int lane, int bar, int step, int out[THEORY_CHORD_MAX]) { return part_step_notes(s, ccur(s), lane, bar, step, out); }

/* KEYS: the keys a chord lights. A note outside the window folds by octaves to the nearest octave inside; the keys notes
 * sit on themselves are set first, so a fold never turns one into a folded key */
seq_keys_t seq_keys_of(const int *n, int c) {
    seq_keys_t k = {0, 0, {0, 0}};
    uint64_t own;
    for (int i = 0; i < c; i++)
        if (n[i] >= SEQ_KEY_LO && n[i] <= SEQ_KEY_HI) k.lit |= 1ull << (n[i] - SEQ_KEY_LO);
    own = k.lit;
    for (int i = 0; i < c; i++) {
        int m = n[i], low = m < SEQ_KEY_LO;
        if (!low && m <= SEQ_KEY_HI) continue;
        m += low ? 12 * ((SEQ_KEY_LO - m + 11) / 12) : -12 * ((m - SEQ_KEY_HI + 11) / 12);
        k.edge[low ? 0 : 1] = 1;
        k.lit |= 1ull << (m - SEQ_KEY_LO);
        k.fold |= (1ull << (m - SEQ_KEY_LO)) & ~own;
    }
    return k;
}

static int step_ok(const seq_part_t *p, int bar, int step) { return bar >= 0 && bar < p->bars && step >= 0 && step < SEQ_STEPS; }

int seq_part_step_notes(const seq_t *s, int part, int lane, int bar, int step, int out[THEORY_CHORD_MAX]) {
    if (part < 0 || part >= SEQ_NPARTS || lane < 0 || lane >= SEQ_ACTIVE_LANES || !step_ok(&s->part[part], bar, step)) return 0;
    return part_step_notes(s, &s->part[part], lane, bar, step, out);
}

int seq_part_step_note(const seq_t *s, int part, int lane, int bar, int step) {
    int n[THEORY_CHORD_MAX];
    if (part < 0 || part >= SEQ_NPARTS || lane < 0 || lane >= SEQ_ACTIVE_LANES || !step_ok(&s->part[part], bar, step)) return -1;
    part_step_notes(s, &s->part[part], lane, bar, step, n);
    return n[0];
}

int seq_step_on(const seq_t *s, int lane, int step) {
    return ccur(s)->lane[lane].steps[seq_grid_bar(s) * SEQ_STEPS + step].on;
}

int seq_cell_label(const seq_t *s, int lane, int step, char *buf, int len) {
    int bar = seq_grid_bar(s), n[THEORY_CHORD_MAX];
    if (!seq_step_on(s, lane, step)) return copy_text(buf, len, " ");
    if (lane == LANE_CHORDS) return seq_slot_name(s, step_slot(ccur(s), bar, step), buf, len);
    step_notes(s, lane, bar, step, n);
    return theory_note_name(n[0], buf, len);
}

/* The frame of the block being played that song position `at` falls on: 0 outside a block (a setter's event) or before the
 * block's interval (a late note-off is sent at once), the last frame at or past its end. */
static int frame_of(const seq_t *s, double at) {
    if (!s->in_block || s->blk_frames <= 0 || !(s->blk_end > s->blk_start) || at <= s->blk_start) return 0;
    if (at >= s->blk_end) return s->blk_frames - 1;
    return (int)((at - s->blk_start) / (s->blk_end - s->blk_start) * s->blk_frames);
}

static void emit_at(seq_t *s, double at, int status, int note, int vel) { s->emit(s->ctx, status, note, vel, frame_of(s, at)); }

static void release(seq_t *s, int i) {
    emit_at(s, s->pending[i].off_ppq, s->pending[i].status_off, s->pending[i].note, 0);
    s->pending[i] = s->pending[--s->npending];
}

/* Send the note-offs due before `before`, plus every one on `status_off`'s channel (-1: none). */
static void release_where(seq_t *s, double before, int status_off) {
    for (int i = 0; i < s->npending;)
        if (s->pending[i].off_ppq < before || s->pending[i].status_off == status_off) release(s, i);
        else i++;
}

/* Every held note ends, and nothing early is left to come: STRUM's queued notes are dropped and PUSH forgets its step.
 * Stop, resync (loop wrap, locate), a part switch, ALL NOTES OFF and the loads all come through here. */
static void release_all(seq_t *s) {
    while (s->npending) release(s, s->npending - 1);
    s->nstrum = 0;
    for (int d = 0; d < DL_COUNT; d++) s->hum[d].on = 0;
    forget_pushes(s);
}

/* GATE's curve: x0.2 at 0, exactly 1 at 50, x2 at 100 */
static double gate_curve(int g) { return g <= 50 ? 0.2 + 0.8 * g / 50.0 : 1.0 + (g - 50) / 50.0; }

/* A note-on on the lane's channel at song position t, its note-off queued for `end` */
static void sound(seq_t *s, int lane, int note, int vel, double t, double end) {
    int off = 0x80 | (seq_channel(s, lane) - 1);
    for (int i = 0; i < s->npending; i++)   /* a retrigger ends the held note first */
        if (s->pending[i].status_off == off && s->pending[i].note == note) { release(s, i); break; }
    if (s->npending == SEQ_MAX_PENDING) release(s, 0);   /* never leave a note hanging */
    emit_at(s, t, 0x90 | (off & 0x0f), note, vel);
    s->pending[s->npending++] = (seq_pending_t){off, note, end};
}

/* GATE and DYNAMICS; gf scales the length first (the Chords lane's own GATE, else 1). At 50 both factors are exactly 1,
 * so the note-off time and velocity are 0.6's to the bit. */
static void note_on(seq_t *s, int lane, int note, int vel, double t, int len, double gf) {
    double f = gate_curve(s->gate), end = t + len * 0.25 * gf * f - GATE_TAIL;
    long v = lround(vel * (40.0 + 1.2 * s->dyn) / 100.0);
    if (end < t + 0.02) end = t + 0.02;
    sound(s, lane, note, v < 1 ? 1 : v > 127 ? 127 : (int)v, t, end);
}

/* 0.12 audition: the queue goes out at t (the block's time on the pending queue's clock), 4 steps long, velocity 100 */
#define AUDITION_VEL 100
#define AUDITION_BEATS (4 * 0.25 - GATE_TAIL)
static void audition_due(seq_t *s, double t) {
    for (int i = 0; i < s->naud; i++) sound(s, s->aud[i].lane, s->aud[i].note, AUDITION_VEL, t, t + AUDITION_BEATS);
    s->naud = 0;
}

void seq_audition_note(seq_t *s, int lane, int note) {
    if (lane < 0 || lane >= SEQ_ACTIVE_LANES || note < 1 || note > 127 || s->naud == SEQ_MAX_AUDITION) return;
    s->aud[s->naud++] = (seq_audition_t){(unsigned char)lane, (unsigned char)note};
}

/* 0.24 a slot's chord heard at step octave oct (PHRASE: a hit's octave) */
static void audition_chord_at(seq_t *s, int slot, int oct) {
    int n[THEORY_CHORD_MAX], c;
    if (!slot_read_ok(ccur(s), slot)) return;   /* 0.24 a change point's too */
    c = chord_notes(s, ccur(s), slot, -1, oct, n);
    if (s->naud + c > SEQ_MAX_AUDITION) return;   /* all or nothing */
    for (int i = 0; i < c; i++)
        if (n[i] >= 1 && n[i] <= 127) s->aud[s->naud++] = (seq_audition_t){LANE_CHORDS, (unsigned char)n[i]};
}
void seq_audition_chord(seq_t *s, int slot) { audition_chord_at(s, slot, LANE_OCT[LANE_CHORDS]); }

int seq_slot_notes(const seq_t *s, int slot, int bar, int out[THEORY_CHORD_MAX]) {   /* chord_notes holds the part's OCTAVE */
    const seq_part_t *p = ccur(s);
    return slot_read_ok(p, slot) ? chord_notes(s, p, slot, bar, LANE_OCT[LANE_CHORDS], out) : 0;
}

/* A step's event time: SWING delays odd 16ths by up to 1/12 beat (a triplet feel at 100). Under 1/4 beat, so steps
 * stay in order. */
static double step_time(const seq_t *s, long k) {
    return k / 4.0 + ((k & 1) ? s->swing / 100.0 * (0.25 / 3.0) : 0.0);
}

/* STRUM's queued notes of a lane are dropped: a new chord (or a mute) supersedes them, so a late note never sounds over
 * the next chord. 0.22 those due at or after `from` (a mute: every one): a swung chord drops only what would sound over it, the chord
 * before keeps the notes due before it sounds (an unswung one's are all played by then: from is its own time) */
static void drop_strum(seq_t *s, int lane, double from) {
    int n = 0;
    for (int i = 0; i < s->nstrum; i++)
        if (s->strum[i].lane != lane || s->strum[i].on_ppq < from) s->strum[n++] = s->strum[i];
    s->nstrum = n;
}

/* STRUM's queued notes due before `before` start now, through note_on (their note-offs are queued from their own on) */
static void strum_due(seq_t *s, double before) {
    int n = 0;
    for (int i = 0; i < s->nstrum; i++) {
        const seq_strum_t *q = &s->strum[i];
        if (q->on_ppq < before) note_on(s, q->lane, q->note, q->vel, q->on_ppq, q->len, q->gf);
        else s->strum[n++] = *q;
    }
    s->nstrum = n;
}

static int part_vstr(const seq_part_t *p);

/* STRUM as it plays: the gap between a chord's notes in beats (SHAPE_STRUM / 100 x 1/16), and `down` high to low (vstrdir);
 * 0.18: under COMPLEXITY's strum layer, the effective step's, as seq_set_vknob would set it */
static double strum_gap(const seq_part_t *p, int *down) {
    int cx = chords_cx(p), off = cx == CHORDS_CX_NEUTRAL ? 0 : rhythm_cx_offsets(cx).strum, k;
    *down = p->vstrdir;
    if (!off) return p->lane_shape[LANE_CHORDS][SHAPE_STRUM] / 100.0 * 0.0625;
    k = rhythm_cx_apply(CXT_STRUM, part_vstr(p), off);
    *down = k < 0;
    return lround(iabs(k) * 12.5) / 100.0 * 0.0625;
}

/* 0.18 FILL's bars: every 4th bar of the loop (4, 8, 12, 16) in a loop of 4 bars or more, else every 4th song bar */
static int fill_due(int bars, int loop_bar, long song_bar) { return bars >= 4 ? loop_bar % 4 == 3 : floor_mod(song_bar, 4) == 3; }

/* The step a lane plays at song step k (bar, step of the part): on a DRAWN lane's fill bar (not locked) its fill
 * variant's (0.19: with its note from rfill_abs where it has one, a DRAWN Bass's), else the lane's own. Reads rfill only: a
 * 16-step decode on the stack, no render. */
static seq_step_t play_step(const seq_part_t *p, int lane, int bar, int step, long k) {
    int d = dl_of(lane);
    if (lane_drawn(p, lane) && p->rk[d][RK_FILL] && !p->rb[d][bar].lock &&
        fill_due(p->bars, bar, (k - floor_mod(k, SEQ_STEPS)) / SEQ_STEPS)) {
        seq_step_t f[SEQ_STEPS];
        rhythm_to_steps(p->rfill[d][bar], f);
        if (f[step].on && p->rfill_abs[d][bar][step]) set_abs(&f[step], p->rfill_abs[d][bar][step]);
        if (f[step].on && step + f[step].len == SEQ_STEPS && (p->sflag[d][bar * SEQ_STEPS + step] & SF_EARLY))   /* 0.24 the moved hit (the fill variant */
            f[step].len = p->lane[lane].steps[bar * SEQ_STEPS + step].len;   /* holds it: its steps were rests) held over the bar line */
        return f[step];
    }
    return p->lane[lane].steps[bar * SEQ_STEPS + step];
}

/* A hit's notes at event time t, velocity v (SHAPE's DYNAMICS applied), len steps: the lane's GATE (a recipe or JAM melody's MK_GATE)
 * before PLAY's; on the Chords lane STRUM's notes after the first i x 1/16 beat x strum/100 later, low to high or (0.17) high
 * to low (queued; a full queue drops them, never blocks). note: 0.19, a DRAWN Bass hit's own note (a fill bar's), else 0: the
 * step's notes. late: 0.21 a swung JAM Melody step's delay past t (lane_swing_late; 0.22 a JAM Chords or Bass step's too), its notes
 * queued as STRUM's (spacing kept), else 0. */
static void sound_hit(seq_t *s, int lane, int bar, int step, int note, int len, int v, double t, double late) {
    const seq_part_t *p = ccur(s);
    int n[THEORY_CHORD_MAX], c = note ? (n[0] = note, 1) : step_notes(s, lane, bar, step, n);
    double gf = gate_curve(is_recipe(p, lane) || jam_lane(p, lane) ? p->mel.kn[MK_GATE] : lshape(p, lane, L_GATE, 50)), dt = 0;
    if (lane == LANE_CHORDS) {
        int down;
        dt = strum_gap(p, &down);
        drop_strum(s, LANE_CHORDS, t + late);
        if (dt > 0 && down)   /* 0.17: high to low */
            for (int i = 0; i < c / 2; i++) { int w = n[i]; n[i] = n[c - 1 - i]; n[c - 1 - i] = w; }
    }
    if (lane == LANE_CHORDS) s->keys = seq_keys_of(n, c);   /* the whole chord at once, strummed notes too */
    for (int i = 0; i < c; i++)
        if ((i == 0 || dt <= 0) && late <= 0) note_on(s, lane, n[i], v, t, len, gf);
        else if (s->nstrum < SEQ_MAX_STRUM) s->strum[s->nstrum++] = (seq_strum_t){lane, n[i], v, len, t + late + i * dt, gf};
}

/* HUMANIZE's waiting hit of drawn lane d (DL_*) plays now (0.22 a swung one waits from its swung time: fire_lane) */
static void hum_play(seq_t *s, int d) {
    seq_hum_t h = s->hum[d];
    s->hum[d].on = 0;
    sound_hit(s, DLANE[d].lane, h.bar, h.step, h.note, h.len, h.vel, h.on_ppq, 0);
}

/* HUMANIZE's waiting hits, each when due before `before`: the strum notes due before it first (they belong to the chord before) */
static void hum_due(seq_t *s, double before) {
    for (int d = 0; d < DL_COUNT; d++) {
        if (!s->hum[d].on || s->hum[d].on_ppq >= before) continue;
        strum_due(s, s->hum[d].on_ppq);
        hum_play(s, d);
    }
}

/* 0.18 HUMANIZE on a DRAWN lane's hit at event time t, velocity *v: h (under COMPLEXITY's layer) delays it
 * xf_rank(seed, i, SALT_HUM) x h x 4 / 100 ms (0..16 ms at 4) at the block's tempo and moves *v by -3h..+3h by a second rank:
 * the same on every loop pass (0.19: the lane's own seed). Delay only: a hit still waiting plays first (at any h), so none sounds
 * before the hit before it. 1 when the hit waits (s->hum, the lane's), 0 when it plays now. */
static int humanize(seq_t *s, const seq_part_t *p, int lane, int bar, int step, int note, int len, int *v, double t) {
    int dl = dl_of(lane), h = lane_drawn(p, lane) ? rk_at(p, lane, RK_HUM, lane_rcx(p, lane)) : 0, i = bar * SEQ_STEPS + step,
        seed = p->lane[lane].gen.seed;
    if (s->hum[dl].on) {   /* at any h (also one just turned to 0): the waiting hit, its chord's strum notes before it, first */
        strum_due(s, s->hum[dl].on_ppq);
        hum_play(s, dl);
    }
    if (!h) return 0;
    *v += xf_rank(seed, i, SALT_HUMV) * (6 * h + 1) / 100 - 3 * h;
    *v = *v < 1 ? 1 : *v > 127 ? 127 : *v;
    double d = xf_rank(seed, i, SALT_HUM) * h * 4 / 100.0 * s->bpm / 60000.0;
    if (d <= 0) return 0;
    s->hum[dl] = (seq_hum_t){1, bar, step, len, *v, note, t + d};
    return 1;
}

/* 0.20 a lane's PUSH and DYNAMICS as it plays: a JAM Melody's PUSH is the part's mjam.push (as the Bass's SHAPE value), any other lane's
 * its SHAPE row's (none: 0 and 50); 0.21 a JAM Melody's DYNAMICS is neutral (50): ACCENT bakes its velocities (mel_line_steps), so
 * mjam.dyn (jdyn) no longer plays */
static int lane_push(const seq_part_t *p, int lane) { return jam_lane(p, lane) ? p->mjam.push : lshape(p, lane, L_PUSH, 0); }
static int lane_dyn(const seq_part_t *p, int lane) { return jam_lane(p, lane) ? 50 : lshape(p, lane, L_DYN, 50); }

/* 0.21 a JAM Melody step's SWING (0.22 a JAM Chords or Bass step's: djam swing and grid16, a lane DRAWN, never on a bar locked for JAM:
 * locked bars keep everything, so they play straight): on the loop's swing grid (grid16: an odd step, else step mod 4 = 2) it sounds
 * (S - 0.5) x 2 x grid
 * late, S = 0.50 + 0.04 x swing, grid 1/4 or 1/2 beat (70%: 0.1 and 0.2 beat); PLAY's SWING on an odd 16th (in step_time already)
 * counts toward it, so the larger of the two plays, never their sum. The delay past step_time(k), 0 off the grid. Never on a pushed
 * step (PUSH: chord starts, on beats). At most 0.2 beat: the note still sounds before the next step's. Final review: on the 8th grid a
 * note on the bar's next step (FILL's 16ths, a fixed note) caps it at 1/8 beat before that note (read from the steps the lane plays: no
 * render); on the 16th grid it never binds (0.1 beat at most) */
static double lane_swing_late(const seq_t *s, const seq_part_t *p, int lane, int bar, int step, long k) {
    int d = dl_of(lane), dj = d >= 0 && lane_drawn(p, lane), sw = jam_lane(p, lane) ? p->mjam.swing : dj ? p->djam[d].swing : 0,
        g16 = dj ? p->djam[d].grid16 : p->mjam.grid16;
    double m, g;
    if (!sw || (g16 ? !(step & 1) : step % 4 != 2) || (dj && jam_locked(p, d, bar))) return 0;   /* reads only: the audio thread */
    m = 0.04 * sw * 2 * (g16 ? 0.25 : 0.5);
    if (step < SEQ_STEPS - 1 && play_step(p, lane, bar, step + 1, k + 1).on && m > step_time(s, k + 1) - 0.125 - k / 4.0)   /* the same bar's */
        m = step_time(s, k + 1) - 0.125 - k / 4.0;
    g = step_time(s, k) - k / 4.0;
    return m > g ? m - g : 0;
}

/* A lane's step at event time t (song step k), with its SHAPE: DYNAMICS (the Bass's ACCENT) around 64, then sound_hit (0.18:
 * a DRAWN lane's fill variant on a fill bar, and HUMANIZE; 0.19: a DRAWN lane's note as play_step gives it, a Bass's; 0.21 a JAM
 * Melody's SWING, queued; 0.22 a JAM Chords or Bass lane's, SWING first and HUMANIZE's delay on top: a humanized hit waits from its
 * swung time, so hum_play keeps the swing). Neutral (50: both factors exactly 1): 0.9's events. */
static void fire_lane(seq_t *s, int lane, int bar, int step, long k, double t) {
    const seq_part_t *p = ccur(s);
    seq_step_t x = play_step(p, lane, bar, step, k);
    if (!x.on || p->lane[lane].mute) return;
    if (dl_of(lane) >= 0 && (p->sflag[dl_of(lane)][bar * SEQ_STEPS + step] & SF_EARLY)) s->early_k[lane] = k + AN_STEPS[p->an[dl_of(lane)]];   /* 0.24 */
    /* a DRAWN Chords step's abs is always 0 (only rhythm_to_steps writes its steps), so x.abs is safe: a DRAWN Bass's note or 0 */
    int v = 64 + (x.vel - 64) * lane_dyn(p, lane) / 50, note = lane_drawn(p, lane) ? x.abs : 0;
    double late = lane_swing_late(s, p, lane, bar, step, k);
    v = v < 1 ? 1 : v > 127 ? 127 : v;
    if (dl_of(lane) >= 0 && humanize(s, p, lane, bar, step, note, x.len, &v, t + late)) return;
    sound_hit(s, lane, bar, step, note, x.len, v, t, late);
}

static int part_idx(const seq_t *s, long k) { return (int)floor_mod(k - s->part_origin_k, ccur(s)->bars * SEQ_STEPS); }

/* 0.24 a lane's first step after a start, a resync that ended its notes, a part switch or a JAM block taking over (seq_t.chase), on a tied
 * step (SF_TIED): the early hit that ties over it never sounded, so it plays here for the rest of its length, the change's chord / note */
static void fire_tied(seq_t *s, int lane, int bar, int step, long k, double t) {
    const seq_part_t *p = ccur(s);
    int d = dl_of(lane), len = p->bars * SEQ_STEPS, i = bar * SEQ_STEPS + step, n, e, v;
    seq_step_t x;
    if (d < 0 || !lane_drawn(p, lane) || !(p->sflag[d][i] & SF_TIED) || p->lane[lane].mute || s->early_k[lane] == k) return;
    n = AN_STEPS[p->an[d]], e = (i - n + len) % len;
    x = play_step(p, lane, e / SEQ_STEPS, e % SEQ_STEPS, k - n);
    if (!x.on || !(p->sflag[d][e] & SF_EARLY) || x.len <= n) return;
    v = clampi(64 + (x.vel - 64) * lane_dyn(p, lane) / 50, 1, 127);
    sound_hit(s, lane, e / SEQ_STEPS, e % SEQ_STEPS, x.abs, x.len - n, v, t, 0);
}

static void fire(seq_t *s, long k) {
    int was = s->cur_k >= 0 ? seq_cur_bar(s) : -1;   /* the bar before this step, -1 after a start or a part switch */
    s->cur_k = k;
    int step = seq_cur_step(s), bar = seq_cur_bar(s);
    if (bar != was) {   /* a bar change (a loop wrap included): seq_fix_bar */
        s->fix_prev = was;
        s->fix_ppq = k / 4.0;   /* a bar's first step is even: never swung */
    }
    for (int lane = 0; lane < SEQ_ACTIVE_LANES; lane++) {
        if (s->early_k[lane] >= 0 && k > s->early_k[lane]) s->early_k[lane] = -1;   /* 0.24 past the change it tied over */
        if (s->pushed_k[lane] == k) {
            s->pushed_k[lane] = -1;   /* PUSH played it; skipped once, so a later locate back to k plays it again */
            continue;
        }
        if (s->chase >> lane & 1) fire_tied(s, lane, bar, step, k, step_time(s, k));
        fire_lane(s, lane, bar, step, k, step_time(s, k));
    }
    s->chase = 0;
}

/* 0.20 the steps between SWITCH AT boundaries of part p, counted from its bar 1: BEAT, BAR, 2 BARS, PHRASE; at most the loop (a
 * shorter loop switches at its wrap). 0.22: SWITCH AT sw of any lane (switch_q); the Melody's its own (mjam.sw) */
static long switch_q(const seq_part_t *p, int sw) {
    static const int Q[4] = {4, 16, 32, 64};
    int q = Q[sw & 3], len = p->bars * SEQ_STEPS;
    return q < len ? q : len;
}
static long mel_switch_q(const seq_part_t *p) { return switch_q(p, p->mjam.sw); }

/* 0.20 1 when a JAM block is queued and step k is its boundary */
static int mel_switch_due(const seq_t *s, long k) {
    return s->mjq_c >= 0 && floor_mod(k - s->part_origin_k, mel_switch_q(ccur(s))) == 0;
}

/* 0.20 the queued JAM block takes over: a copy of what the setters rendered (the audio thread: no render, no wait), which the
 * screen side's seq_mel_poll sees. A lane no longer JAM only drops it. */
static void mel_commit(seq_t *s) {
    seq_part_t *p = cur(s);
    if (mel_jam(p) && s->mjq_part == s->cur_part) {
        size_t n = (size_t)p->filled * SEQ_STEPS;
        memcpy(p->lane[LANE_MEL_UP].steps, s->mj_next, n * sizeof *s->mj_next);
        memcpy(p->mline, s->mj_next_line, n * sizeof *s->mj_next_line);
        p->mjam.grid16 = s->mj_next_grid16, p->mjam.weak = s->mj_next_weak;   /* 0.21 */
        p->mjam.set.c = s->mjq_c, p->mjam.set.l = s->mjq_l;
        s->mj_switched = 1;
    }
    s->mjq_c = -1;
}

/* 0.22 1 when a JAM block is queued on drawn lane d and step k is its boundary (the lane's own SWITCH AT). PUSH reads it (push_next) so
 * the old block's step is never pushed into the boundary. A window shared with MELODY's (mel_switch_due): a step PUSH already played
 * early, before a tap made it the boundary, stays the old block's (fire skips the pushed step once, at most 1/8 beat of the old
 * chord / note); and a host jump back onto a boundary keeps a pushed old step (keep_pushed). Expected on the device test */
static int djq_due(const seq_t *s, int d, long k) {
    return s->djq[d].c >= 0 && floor_mod(k - s->part_origin_k, switch_q(ccur(s), ccur(s)->djam[d].sw)) == 0;
}

/* 0.24 the other lane following drawn lane d (LOCK WITH) as the block taking over on d makes it (djq_t.f): plain copies */
static void djq_follower(seq_t *s, int d) {
    const djq_t *q = &s->djq[d];
    seq_part_t *p = cur(s);
    int o = DL_COUNT - 1 - d, lane = DLANE[o].lane;
    if (!q->f.fon || p->lw[o] == LW_OWN || !lane_drawn(p, lane)) return;
    memcpy(p->lane[lane].steps, q->f.steps, (size_t)p->filled * SEQ_STEPS * sizeof *q->f.steps);
    memcpy(p->rfill[o], q->f.rfill, sizeof q->f.rfill);
    memcpy(p->rfill_abs[o], q->f.rfill_abs, sizeof q->f.rfill_abs);
    memcpy(p->sflag[o], q->f.sflag, sizeof q->f.sflag);
    if (o == DL_BASS) memcpy(p->brole, q->f.brole, (size_t)p->filled * SEQ_STEPS);
}

/* 0.22 the block queued on drawn lane d takes over: plain copies of what the setters rendered (djq_render; the audio thread: no render,
 * no wait), which seq_jam_poll then reports. Its swing grid comes with it (djq_render's); its weak flags read 0 until the next setter
 * renders the lane (dj_derive). A lane no longer DRAWN only drops it */
static void djq_commit(seq_t *s, int d) {
    djq_t *q = &s->djq[d];
    seq_part_t *p = cur(s);
    int lane = DLANE[d].lane;
    if (q->part == s->cur_part && lane_drawn(p, lane)) {
        size_t n = (size_t)p->filled * SEQ_STEPS;
        memcpy(p->lane[lane].steps, q->steps, n * sizeof *q->steps);
        memcpy(p->rb[d], q->rb, sizeof q->rb);
        memcpy(p->rfill[d], q->rfill, sizeof q->rfill);
        memcpy(p->rfill_abs[d], q->rfill_abs, sizeof q->rfill_abs);
        memcpy(p->sflag[d], q->sflag, sizeof q->sflag);   /* 0.24 */
        if (d == DL_CHORDS || q->f.fon) memcpy(p->hoct, q->hoct, sizeof q->hoct);   /* 0.24 final review: the Chords' (they or their follower) */
        p->lw[d] = LW_OWN;   /* 0.24 the tap's: its block brings its own rhythm */
        p->djam[d].c = q->c, p->djam[d].l = q->l, p->djam[d].grid16 = q->grid16, p->djam[d].weak = 0;
        djq_follower(s, d);
        if (d == DL_BASS) {
            memcpy(p->brole, q->brole, n);
            p->bass.set = q->bset, p->bass.nlock = q->nlock;
            memcpy(p->bass.frozen, q->frozen, sizeof q->frozen);
        } else {
            p->vstyle = q->vstyle, p->vmode = q->vmode, p->vcol = q->vcol, p->vinv = q->vinv, p->vspr = q->vspr, p->vstrdir = q->vstrdir;
            p->lane_shape[LANE_CHORDS][SHAPE_STRUM] = q->strum, p->lane_shape[LANE_CHORDS][SHAPE_OCT] = q->oct, p->voicing = q->voicing;
            memcpy(p->voice, q->voice, sizeof q->voice);
            memcpy(p->voice_n, q->voice_n, sizeof q->voice_n);
            memcpy(p->slot_text, q->slot_text, sizeof q->slot_text);
            memcpy(p->slot_num, q->slot_num, sizeof q->slot_num);
        }
        s->dj_switched |= 1 << d;
    }
    q->c = -1;
}

/* 0.22 every drawn lane's queue dropped (a part switch, a load, INIT); a tap's undo entries leave at the next UNDO or tap (djq_sweep) */
static void djq_drop_all(seq_t *s) {
    for (int d = 0; d < DL_COUNT; d++) {
        if (s->djq[d].c >= 0) s->djq_drop[d] = s->djq[d].tag;
        s->djq[d].c = -1;
    }
}

/* PUSH: each lane's next step, when it begins a chord (the first step of a progression slot), up to 1/8 beat early (the
 * lane's push/100 x 0.125), played once its pushed time is in this block. Never into a queued part switch's bar line (the
 * switch ends held notes and the part changes), 0.20 nor the Melody into a queued JAM block's boundary (the old block's step
 * would play; 0.22 nor a drawn lane into its own), and never before the step before it (fired already: 1/8 < 1/4 - SWING's 1/12). */
/* 0.24 (ruling 8) a step PUSH pushes: a real slot's first step that no change point covers, or a change point's step; 0.24.2 or a bar's
 * step 0 inside a slot after a bar that ends on a point (2-bar chords: the slot's chord again). A scan of a bar's 4 points: the audio thread */
static int push_change(const seq_part_t *p, int idx) {
    int b = idx / SEQ_STEPS, st = idx % SEQ_STEPS, v = slot_at(p, b, st);
    if (v >= SEQ_MAX_PROG) return p->cp[b][(v - SEQ_MAX_PROG) % SEQ_CP_MAX].step == st;
    return idx % p->chord_len == 0 || (!st && slot_at(p, b + p->bars - 1, SEQ_STEPS - 1) >= SEQ_MAX_PROG);
}

static void push_next(seq_t *s, double end) {
    const seq_part_t *p = ccur(s);
    long k = s->next_k;
    int idx = part_idx(s, k);
    if (!push_change(p, idx) || (s->queued >= 0 && idx % SEQ_STEPS == 0)) return;
    for (int lane = 0; lane < SEQ_ACTIVE_LANES; lane++) {
        int push = lane_push(p, lane);
        if (!push || s->pushed_k[lane] == k || (lane == LANE_MEL_UP && mel_switch_due(s, k)) || (dl_of(lane) >= 0 && djq_due(s, dl_of(lane), k)) ||
            !play_step(p, lane, idx / SEQ_STEPS, idx % SEQ_STEPS, k).on || p->lane[lane].mute) continue;
        double t = step_time(s, k) - push / 100.0 * 0.125;
        if (t >= end - EPS) continue;
        s->pushed_k[lane] = k;
        fire_lane(s, lane, idx / SEQ_STEPS, idx % SEQ_STEPS, k, t);
    }
}

/* A jump back (a host loop wrap) onto the step PUSH already played, the same step of the part: each lane pushed there
 * keeps sounding, as it would have without the wrap. Its note-offs (by its channel) and its strum's queued notes move
 * with the song position (by the jump); everything else ends, as at any resync, and fire() skips those lanes on the
 * step. A host loop that is not a multiple of the part lands elsewhere and releases everything: the notes pushed before
 * the wrap were the wrong ones, for at most 1/8 beat (PUSH cannot know the host's loop end). Returns 0 when nothing was
 * kept. */
static unsigned keep_pushed(seq_t *s, long k) {
    int keep[SEQ_ACTIVE_LANES], push[SEQ_ACTIVE_LANES], n = 0;
    unsigned any = 0;
    double d[SEQ_ACTIVE_LANES];
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) {
        /* 0.24 or ANTICIPATE's early hit sounding into the change step: kept as a pushed one (its step is a hold) */
        long at = s->early_k[l] >= 0 && part_idx(s, k) == part_idx(s, s->early_k[l]) ? s->early_k[l] : -1;
        push[l] = s->pushed_k[l] >= 0 && part_idx(s, k) == part_idx(s, s->pushed_k[l]);
        if (push[l]) at = s->pushed_k[l];
        keep[l] = at >= 0;
        d[l] = keep[l] ? step_time(s, at) - step_time(s, k) : 0;   /* same parity (an even-length part): no swing gap */
        any |= (unsigned)keep[l] << l;
    }
    if (!any) return 0;
    for (int i = 0; i < s->npending;) {
        int l = 0;
        while (l < SEQ_ACTIVE_LANES && !(keep[l] && s->pending[i].status_off == (0x80 | (seq_channel(s, l) - 1)))) l++;
        if (l < SEQ_ACTIVE_LANES) s->pending[i++].off_ppq -= d[l];
        else release(s, i);
    }
    for (int i = 0; i < s->nstrum; i++)
        if (keep[s->strum[i].lane]) {
            s->strum[n] = s->strum[i];
            s->strum[n++].on_ppq -= d[s->strum[i].lane];
        }
    s->nstrum = n;
    for (int h = 0; h < DL_COUNT; h++) {   /* HUMANIZE's waiting hits (the pushed ones) move too */
        if (s->hum[h].on && keep[DLANE[h].lane]) s->hum[h].on_ppq -= d[DLANE[h].lane];
        else s->hum[h].on = 0;
    }
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) {
        s->early_k[l] = keep[l] && !push[l] ? k : -1;
        s->pushed_k[l] = push[l] ? k : -1;
    }
    s->chase = ~any & ((1u << SEQ_ACTIVE_LANES) - 1);   /* 0.24 the lanes whose notes ended */
    return any;
}

/* The first step whose event time is at or after ppq: the boundary at or after it, or the swung odd step just before */
static long first_k_at(const seq_t *s, double ppq) {
    long k = (long)ceil(ppq * 4.0 - EPS);
    return s->swing && ((k - 1) & 1) && step_time(s, k - 1) >= ppq - EPS ? k - 1 : k;
}

/* The queued part takes over (its catch-up ran when it was queued): only indices change here, on the audio thread. */
static void switch_part(seq_t *s, long origin_k) {
    release_all(s);   /* as a loop wrap does */
    s->cur_part = s->queued;
    s->queued = -1;
    s->mjq_c = -1;   /* 0.20: a JAM block queued on the old part is dropped */
    djq_drop_all(s);   /* 0.22: and the drawn lanes' */
    s->move_text[0] = 0;   /* the old part's move feedback does not carry over */
    s->part_origin_k = origin_k;
    if (cur(s)->lane[LANE_CHORDS].mute) memset(&s->keys, 0, sizeof s->keys);   /* 0.26 the mutes are the new part's */
    if (s->txn.kind) s->txn.stale = 1;   /* 0.17: an open picker or browser is the old part's: the next setter cancels it */
    paging_off(s);   /* and a paging pause ends (ruling 18) */
    s->cur_k = -1;   /* no step of the new part fired yet (fire() follows at once while playing) */
    if (s->view_bar >= cur(s)->bars) s->view_bar = 0;
    s->display_rev++;   /* MPC re-reads the new part's values (its knobs); a plain count, fine on the audio thread */
}

/* The beats a block of `frames` frames spans at bpm and the host's sample rate (seq_set_rate) */
static double block_beats(const seq_t *s, double bpm, int frames) { return frames * bpm / (60.0 * s->rate); }

/* Stopped: the pending queue (only auditions: a stop ended the rest) runs on idle_ppq, beats of stopped blocks at the
 * host tempo (120 when it gives none), so their note-offs come on time. */
static void idle_block(seq_t *s, double bpm, int frames) {
    s->blk_start = s->idle_ppq;
    s->blk_end = s->idle_ppq + block_beats(s, bpm > 0 ? bpm : 120.0, frames);
    audition_due(s, s->idle_ppq);
    s->idle_ppq = s->blk_end;
    release_where(s, s->idle_ppq, -1);
    if (!s->npending) s->idle_ppq = 0;   /* nothing timed on it: keep it small */
}

void seq_set_rate(seq_t *s, double hz) { if (hz > 0) s->rate = hz; }

static void block(seq_t *s, double ppq, double bpm, int playing, int frames);

void seq_block(seq_t *s, double ppq, double bpm, int playing, int frames) {
    s->in_block = 1;
    s->blk_frames = frames;
    block(s, ppq, bpm, playing, frames);
    s->in_block = 0;
}

static void block(seq_t *s, double ppq, double bpm, int playing, int frames) {
    s->bpm = bpm;
    if (!playing || ppq < 0 || bpm <= 0) {
        if (s->playing) {
            release_all(s);
            memset(&s->keys, 0, sizeof s->keys);
            s->naud = 0;   /* a stop ends the auditions too */
            s->idle_ppq = 0;
            s->playing = 0;
            s->cur_k = -1;
            if (s->queued >= 0) switch_part(s, 0);   /* as a tap while stopped would */
            if (s->mjq_c >= 0) mel_commit(s);         /* 0.20: a queued JAM block too (a part switch dropped it) */
            for (int d = 0; d < DL_COUNT; d++)
                if (s->djq[d].c >= 0) djq_commit(s, d);   /* 0.22: and the drawn lanes' */
            s->part_origin_k = 0;   /* 0.16: the next start sets it (the part's bar 1 where it starts) */
        }
        idle_block(s, bpm, frames);
        return;
    }
    if (!s->playing) {   /* start: the first boundary at or after the playhead is the first step */
        long kp = (long)floor(ppq * 4.0 + EPS);
        s->playing = 1;
        s->part_origin_k = kp - floor_mod(kp, SEQ_STEPS);   /* 0.16: the part's bar 1 is the MPC bar the start is in (a
                                                             * clip launch or PLAY starts the plugin's loop, not the song's) */
        forget_pushes(s);
        for (int i = 0; i < s->npending; i++) s->pending[i].off_ppq += ppq - s->idle_ppq;   /* auditions: onto the song clock */
        s->idle_ppq = 0;
        s->last_ppq = ppq;
        s->next_k = first_k_at(s, ppq);
        s->blk_start = ppq;   /* the auditions at the block's first frame */
        s->blk_end = ppq + block_beats(s, bpm, frames);
        audition_due(s, ppq);
        return;
    }
    /* This block plays the interval since the previous block, [last_ppq, ppq): notes go out one block (~2.9 ms) after
     * their boundary, a constant, never extrapolated past the host position. */
    double start = s->last_ppq, end = ppq;
    if (end < start && start - end < 1.0) {
        end = start;   /* a tempo change stepped the position back a little: hold the high-water mark, play nothing twice */
    } else if (end < start || end - start > 1.0) {
        int back = end < start;   /* loop wrap, locate back, or a jump forward: resync without flooding */
        start = end - block_beats(s, bpm, frames);
        s->next_k = first_k_at(s, start);
        if (!back || !keep_pushed(s, s->next_k)) release_all(s);
    }
    s->blk_start = start;   /* frame_of: the events of [start, end) at their frames of this block */
    s->blk_end = end;
    audition_due(s, end);
    if (s->move_text[0] && (s->move_secs -= frames / s->rate) <= 0) s->move_text[0] = 0;   /* 4 s of playback (any tempo) */
    release_where(s, end, -1);
    /* In order, each once: next_k only moves forward. Start and resync put step_time(next_k) at or after `start`, so every
     * step reached fires; one whose time moved before `start` (SWING lowered while it was pending) fires late, not never. */
    while (step_time(s, s->next_k) < end - EPS) {
        long k = s->next_k++;
        hum_due(s, step_time(s, k));   /* 0.18: HUMANIZE's waiting hit, then */
        strum_due(s, step_time(s, k));   /* the last chord's strum before this step (and before a switch drops it) */
        if (s->queued >= 0 && floor_mod(k - s->part_origin_k, SEQ_STEPS) == 0) switch_part(s, k);   /* a bar line */
        if (mel_switch_due(s, k)) mel_commit(s);   /* 0.20: a queued JAM block at its SWITCH AT boundary (after a part switch's drop) */
        for (int d = 0; d < DL_COUNT; d++)
            if (djq_due(s, d, k)) {   /* 0.22: the drawn lanes' each at its own */
                djq_commit(s, d);
                for (int l = 0; l < DL_COUNT; l++)   /* 0.24 a tied step its new steps start on chases (nothing sounds into it) */
                    if (s->early_k[DLANE[l].lane] < 0) s->chase |= 1u << DLANE[l].lane;
            }
        fire(s, k);
    }
    hum_due(s, end);
    strum_due(s, end);
    push_next(s, end);
    s->last_ppq = end;
}


void seq_all_notes_off(seq_t *s) {
    act(s);
    s->naud = 0;
    release_all(s);
    for (int lane = 0; lane < SEQ_ACTIVE_LANES; lane++) s->emit(s->ctx, 0xB0 | (seq_channel(s, lane) - 1), 123, 0, 0);
}

static void tap_generated(seq_t *s, seq_part_t *p, int lane, int i, int on);

/* 0.18 PLAY's tap on a DRAWN lane, whose row shows the bar as it plays: a silent step draws a HIT, a playing one
 * clears the drawn hit there and the holds after it (a hit the knobs added has none: nothing changes). One undo step, after an
 * open bar or browser is closed as DONE (its CANCEL never takes back this tap). */
static void tap_drawn(seq_t *s, seq_part_t *p, int lane, int bar, int step, int on) {
    rbar_t *r = &p->rb[dl_of(lane)][bar];
    unsigned char c[16];
    drawn_codes(p, dl_of(lane), bar, c);   /* 0.24 a LOCK WITH bar's: the tap starts from them and makes it its own */
    if (p->lane[lane].steps[bar * SEQ_STEPS + step].on == on || rhythm_is_hit(c[step]) == on) return;   /* a re-send; nothing to draw or clear */
    seq_txn_done(s);
    push_undo(s, lane);
    memcpy(r->c, c, 16);
    r->c[step] = on ? RC_HIT : RC_REST;
    if (!on) rhythm_cut_holds(r->c, step);
    r->ed = 1;
    render_rhythm(s, p, lane);
}

/* 0.20 the note PLAY's tap fixes on a silent JAM step: the root of the chord under it nearest the bar's notes (their mean; a silent
 * bar: the tonic at the bar's REGISTER), in 36..100 */
static int mel_tap_note(const seq_t *s, const seq_part_t *p, int bar, int step) {
    const mjam_note_t *line = &p->mline[bar * SEQ_STEPS];
    const mjam_set_t *set = p->mjam.nlock >> bar & 1 ? &p->mjam.frozen[bar] : &p->mjam.set;
    int sum = 0, n = 0, ref, slot = slot_at(p, bar, step), m;
    mjam_chord_t ch;
    for (int i = 0; i < SEQ_STEPS; i++)
        if (line[i].note) sum += line[i].note, n++;
    ref = n ? (sum + n / 2) / n : 60 + s->key + 12 * (set->reg - 1);
    mjam_chord_of(s->key, s->scale, slot_scale(s, p, slot), p->prog[slot], &ch);
    m = nearest_pc(ch.root, ref);
    while (m > 100) m -= 12;
    while (m < 36) m += 12;
    return m;
}

/* 0.20 PLAY's tap on a JAM Melody lane: a silent step takes a note fix (mel_tap_note), a sounding one a REST fix. One undo step;
 * an open CHORDS or BASS transaction stays open (the tap is another tab's edit, as on a classic Melody lane), an open Melody picker
 * or editor closes as DONE first (its CANCEL must not take the tap back) */
static void tap_mel(seq_t *s, seq_part_t *p, int bar, int step, int on) {
    int i = bar * SEQ_STEPS + step;
    if (p->lane[LANE_MEL_UP].steps[i].on == on) return;   /* a re-send */
    txn_unstale(s);
    if (s->txn.kind && s->txn.lane == LANE_MEL_UP) txn_done(s);
    push_undo(s, LANE_MEL_UP);
    p->mjam.fix[i] = on ? (unsigned char)mel_tap_note(s, p, bar, step) : MJ_FIX_REST;
    render_mel(s, p);
}

static void set_step(seq_t *s, int lane, int bar, int step, int on) {
    seq_part_t *p = cur(s);
    if (lane_drawn(p, lane)) {
        tap_drawn(s, p, lane, bar, step, on ? 1 : 0);
        return;
    }
    if (jam_lane(p, lane)) {
        tap_mel(s, p, bar, step, on ? 1 : 0);
        return;
    }
    if (p->lane[lane].gen.style != GEN_MANUAL) {
        tap_generated(s, p, lane, bar * SEQ_STEPS + step, on ? 1 : 0);
        return;
    }
    seq_step_t *x = &p->lane[lane].steps[bar * SEQ_STEPS + step];
    if (lane == LANE_MEL_UP || lane == LANE_MEL_DOWN) {   /* the arp order depends on the bar's other steps */
        x->on = on ? 1 : 0;
        x->abs = 0;   /* a tap is chord-relative */
        apply_recipe(p, lane, bar);
        follow(s, p, lane);
    } else if (on) {   /* Chords, Bass: only the tapped step, as the recipe has it (a generated bar keeps the rest) */
        *x = (seq_step_t){1, TONE_R, LANE_OCT[lane], VELOCITY, 1, 0};
    } else {
        x->on = 0;
        x->abs = 0;
    }
}

void seq_set_step(seq_t *s, int lane, int bar, int step, int on) {
    seq_part_t *p = cur(s);
    if (lane < 0 || lane >= SEQ_ACTIVE_LANES || bar < 0 || bar >= p->bars || step < 0 || step >= SEQ_STEPS) return;
    act(s);
    set_step(s, lane, bar, step, on);
}

int seq_channel_of(const seq_t *s, int lane) {
    return lane < 0 || lane >= SEQ_LANES ? 0 : s->chan[lane];
}

void seq_set_channel(seq_t *s, int lane, int ch) {
    if (lane < 0 || lane >= SEQ_LANES || ch < 1 || ch > 16 || ch == s->chan[lane]) return;
    act(s);
    release_where(s, -INFINITY, 0x80 | (s->chan[lane] - 1));   /* held notes end on the channel they began on */
    s->chan[lane] = ch;
}

static void set_out(seq_t *s, int *x, int v) {
    if (v < 0 || v > 100 || v == *x) return;
    act(s);
    *x = v;
}
void seq_set_swing(seq_t *s, int v) { set_out(s, &s->swing, v); }
void seq_set_gate(seq_t *s, int v) { set_out(s, &s->gate, v); }
void seq_set_dynamics(seq_t *s, int v) { set_out(s, &s->dyn, v); }
int seq_swing(const seq_t *s) { return s->swing; }
int seq_gate(const seq_t *s) { return s->gate; }
int seq_dynamics(const seq_t *s) { return s->dyn; }
/* 0.20 the playing part's JAM Melody PUSH and DYNAMICS (lane_push, lane_dyn): knob turns, no undo step */
static void set_mel_knob(seq_t *s, unsigned char *x, int v) {
    if (v < 0 || v > 100 || v == *x) return;
    act(s);
    *x = (unsigned char)v;
}
void seq_set_mel_push(seq_t *s, int v) { set_mel_knob(s, &cur(s)->mjam.push, v); }
void seq_set_mel_dyn(seq_t *s, int v) { set_mel_knob(s, &cur(s)->mjam.dyn, v); }
/* 0.21 SWING 0..5 and ACCENT 0..3: clamped, no undo step; ACCENT re-renders (render_mel: the velocities are baked in the steps).
 * Amendment 1: no knob sets ACCENT (the engine never calls seq_set_mel_acc; it stays for the tests) */
void seq_set_mel_swing(seq_t *s, int v) {
    seq_part_t *p = cur(s);
    v = clampi(v, 0, 5);
    if (v == p->mjam.swing) return;
    act(s);
    p->mjam.swing = (unsigned char)v;
    if (mel_jam(p)) {   /* 0.21 its weak bit, no render (SWING moves notes in time only): the playing block's and a queued one's */
        p->mjam.weak = mel_reweigh(p->mjam.weak, p, p->mline, p->mjam.grid16);
        if (s->mjq_c >= 0 && p == &s->part[s->mjq_part]) s->mj_next_weak = mel_reweigh(s->mj_next_weak, p, s->mj_next_line, s->mj_next_grid16);
    }
}
void seq_set_mel_acc(seq_t *s, int v) {
    seq_part_t *p = cur(s);
    v = clampi(v, 0, 3);
    if (v == p->mjam.acc) return;
    act(s);
    p->mjam.acc = (unsigned char)v;
    if (mel_jam(p)) render_mel(s, p);
}
int seq_mel_swing(const seq_t *s) { return ccur(s)->mjam.swing; }
int seq_mel_acc(const seq_t *s) { return ccur(s)->mjam.acc; }
int seq_mel_grid16(const seq_t *s) { return ccur(s)->mjam.grid16; }
unsigned seq_mel_weak(const seq_t *s) { return ccur(s)->mjam.weak; }
int seq_mel_push(const seq_t *s) { return ccur(s)->mjam.push; }
int seq_mel_dyn(const seq_t *s) { return ccur(s)->mjam.dyn; }

/* 0.20 switching JAM blocks (seq_core.h). COMPLEXITY (cx_3) follows the block's level */
static int mel_block_cx(int l) { return 20 * l + MEL_JAM_CX; }

static void push_recipe_snap(seq_t *s, int part, int lane);

/* 0.20 (owner, 2026-10-20: old projects load as they are, no melody conversion) the first JAM action (seq_mel_queue, seq_mjam_set,
 * seq_mel_swap, seq_set_mel_switch, opening the picker or the editor) on a playing Melody lane that isn't JAM (an old project's
 * recipe, 0.10 style or MANUAL notes) first makes it a new instance's JAM lane (JAM at level 1, mjam_default, GATE 50; its steps,
 * locks and NEW BAR variations dropped), one undo step that puts the old lane back; an open transaction closes as DONE first */
static void mel_jam_now(seq_t *s) {
    seq_part_t *p = cur(s);
    seq_lane_t *ln = &p->lane[LANE_MEL_UP];
    unsigned char mute;
    if (mel_jam(p)) return;
    seq_txn_done(s);
    act(s);
    push_recipe_snap(s, s->cur_part, LANE_MEL_UP);
    mute = ln->mute;
    memset(ln, 0, sizeof *ln);
    ln->mute = mute;
    mjam_init(&p->mjam);
    jam_default(p);
    render_mel(s, p);
}

/* 0.20 1 when playing with a part switch queued whose bar line is at or before the next boundary q steps apart (0.22: any lane's) */
static int switch_blocked(const seq_t *s, long q) {
    long bar_k, k;
    if (!s->playing || s->queued < 0) return 0;
    bar_k = s->next_k + floor_mod(s->part_origin_k - s->next_k, SEQ_STEPS);   /* the bar line the part switch takes */
    k = s->next_k + floor_mod(s->part_origin_k - s->next_k, q);
    return k >= bar_k;
}

int seq_mel_blocked(const seq_t *s) { return switch_blocked(s, mel_switch_q(ccur(s))); }

void seq_mel_queue(seq_t *s, int c, int l) {
    seq_part_t *p = cur(s);
    if (c < 0 || c >= MJ_COLS || l < 0 || l >= MJ_LEVELS || (!mel_jam(p) && c == p->mjam.set.c && l == p->mjam.set.l)) return;
    mel_jam_now(s);   /* the shown block (a host echo) leaves a lane that isn't JAM as it is */
    act(s);
    s->mjq_c = -1;
    if (c == p->mjam.set.c && l == p->mjam.set.l) return;   /* the playing block: a cancel */
    if (!s->playing) {   /* stopped: at once */
        p->mjam.set.c = (signed char)c, p->mjam.set.l = (signed char)l;
        p->lane[LANE_MEL_UP].gen.complexity = mel_block_cx(l);
        render_mel(s, p);
        return;
    }
    if (seq_mel_blocked(s)) return;   /* a part switch queued takes over first and would drop it (the screen says so) */
    s->mjq_c = (signed char)c, s->mjq_l = (signed char)l, s->mjq_part = s->cur_part;
    render_queued(s, p, NULL);
}

int seq_mel_queued(const seq_t *s, int *c, int *l, int *at_bar) {
    const seq_part_t *p = ccur(s);
    long k;
    if (s->mjq_c < 0 || !mel_jam(p)) return 0;   /* the lane left JAM: nothing will switch */
    k = s->next_k + floor_mod(s->part_origin_k - s->next_k, mel_switch_q(p));   /* the first boundary at or after the next step */
    if (c) *c = s->mjq_c;
    if (l) *l = s->mjq_l;
    if (at_bar) *at_bar = (int)(floor_mod(k - s->part_origin_k, p->bars * SEQ_STEPS) / SEQ_STEPS);
    return 1;
}

void seq_set_mel_switch(seq_t *s, int sw) {
    if (sw < 0 || sw > 3 || sw == cur(s)->mjam.sw) return;
    mel_jam_now(s);
    act(s);
    cur(s)->mjam.sw = (unsigned char)sw;
}

int seq_mel_switch(const seq_t *s) { return ccur(s)->mjam.sw; }

int seq_mel_poll(seq_t *s) {
    seq_part_t *p = &s->part[s->mjq_part];   /* the part switched (mel_commit's) */
    if (!s->mj_switched) return 0;
    s->mj_switched = 0;
    if (mel_jam(p)) p->lane[LANE_MEL_UP].gen.complexity = mel_block_cx(p->mjam.set.l);
    return 1;
}

void seq_set_mute(seq_t *s, int lane, int muted) {
    if (lane < 0 || lane >= SEQ_ACTIVE_LANES) return;
    if (cur(s)->lane[lane].mute != (muted ? 1 : 0)) act(s);
    cur(s)->lane[lane].mute = muted ? 1 : 0;
    if (!muted) return;
    if (lane == LANE_CHORDS) memset(&s->keys, 0, sizeof s->keys);
    release_where(s, -INFINITY, 0x80 | (seq_channel(s, lane) - 1));   /* the sequence keeps running; only its sound stops */
    drop_strum(s, lane, -INFINITY);
    if (dl_of(lane) >= 0) s->hum[dl_of(lane)].on = 0;   /* and HUMANIZE's waiting hit */
}

int seq_mute(const seq_t *s, int lane) { return lane >= 0 && lane < SEQ_ACTIVE_LANES && ccur(s)->lane[lane].mute; }

#define BAR_LOCK (SEQ_STEPS / 8)   /* lock bytes per bar */

/* Bars [from, to) repeat a `bars`-bar loop, steps and locks: what a grow puts in bars never filled. */
static void repeat_bars(seq_lane_t *ln, int from, int to, int bars) {
    for (int b = from; b < to; b++) {
        memcpy(&ln->steps[b * SEQ_STEPS], &ln->steps[(b % bars) * SEQ_STEPS], SEQ_STEPS * sizeof(seq_step_t));
        memcpy(&ln->lock[b * BAR_LOCK], &ln->lock[(b % bars) * BAR_LOCK], BAR_LOCK);
    }
}

/* 0.19 DRAWN Bass bar b repeats bar a's notes (its DLANE repeat): its NEW BAR variation, note lock and fixed notes */
static void bass_repeat_bar(seq_part_t *p, int b, int a) {
    p->lane[LANE_BASS].bar_var[b] = p->lane[LANE_BASS].bar_var[a];
    p->bass.frozen[b] = p->bass.frozen[a];
    p->bass.nlock = (unsigned short)((p->bass.nlock & ~(1u << b)) | (unsigned)(p->bass.nlock >> a & 1) << b);
    memcpy(&p->bass.fix[b * SEQ_STEPS], &p->bass.fix[a * SEQ_STEPS], SEQ_STEPS);
}

/* 0.24 DRAWN Chords bar b repeats bar a's own state (its DLANE repeat): its hit octaves and NEW BAR version */
static void chords_repeat_bar(seq_part_t *p, int b, int a) {
    memcpy(p->hoct[b], p->hoct[a], sizeof p->hoct[b]);
    p->lane[LANE_CHORDS].bar_var[b] = p->lane[LANE_CHORDS].bar_var[a];
}

/* 0.18: a drawn lane's bars the same way (a drawn lane only: rbars_init). 0.19: a DRAWN lane's own state too (DLANE repeat: a
 * DRAWN Bass's notes) */
static void repeat_rbars(seq_part_t *p, int lane, int from, int to, int bars) {
    int d = dl_of(lane);
    rbar_t *rb = p->rb[d];
    for (int b = from; b < to; b++) {
        int a = b % bars;
        rb[b] = rb[a];
        if (DLANE[d].repeat && lane_drawn(p, lane)) DLANE[d].repeat(p, b, a);
    }
}

void seq_set_bars(seq_t *s, int bars) {
    seq_part_t *p = cur(s);
    if ((bars != 1 && bars != 2 && bars != 4 && bars != 8 && bars != 16) || bars == p->bars) return;
    for (int d = 0; d < DL_COUNT; d++)
        if (txn_live(s, DLANE[d].lane, CVIEW_BAR) && s->txn.slot >= bars) txn_cancel(s);   /* 0.18: the open bar leaves the loop */
    if (txn_live(s, LANE_MEL_UP, TXN_MEDIT) && s->txn.slot >= bars) txn_cancel(s);   /* 0.20: and the Melody's */
    seq_txn_done(s);   /* 0.17 (ruling 24): CANCEL never undoes the length */
    act(s);
    /* Never-filled bars repeat the loop (and its locks), so it sounds the same. A bar a shrink hid keeps its steps
     * (filled >= bars). A generated lane then regenerates over the new length, its hand edits kept. */
    int from = p->prog_len, to;
    for (int l = 0; l < SEQ_LANES; l++) repeat_bars(&p->lane[l], p->filled, bars, p->bars);
    for (int d = 0; d < DL_COUNT; d++) repeat_rbars(p, DLANE[d].lane, p->filled, bars, p->bars);
    if (mel_jam(p)) mel_repeat(p, p->filled, bars, p->bars);
    for (int b = p->filled; b < bars; b++) memcpy(p->cp[b], p->cp[b % p->bars], sizeof p->cp[b]);   /* 0.24 the change points too */
    p->bars = bars;
    if (p->filled < bars) p->filled = bars;
    /* 0.14: the slots the same way */
    to = part_slots(p);
    grow_slots(p, from, to);
    if (s->view_bar >= bars) s->view_bar = 0;
    strip_clamp(s);
    revoice(s, p);
    regen_part(s, p);
}

/* 0.14: the slots remapped by time, one undo step (its snapshot holds the length) */
void seq_set_chord_len(seq_t *s, int steps) {
    seq_part_t *p = cur(s);
    int old = p->chord_len;
    if ((steps != 4 && steps != 8 && steps != 16 && steps != 32) || steps == old) return;
    seq_txn_done(s);
    act(s);
    push_undo(s, -1);
    p->chord_len = steps;
    strip_clamp(s);
    remap_slots(p, old);
    revoice(s, p);
    regen_part(s, p);
}

void seq_set_follow(seq_t *s, int on) {
    if (s->follow != (on != 0)) act(s);
    if (!on && s->follow && s->cur_k >= 0) s->view_bar = seq_cur_bar(s);   /* stay on the bar being played */
    if (!on && s->follow) strip_follow_off(s);
    s->follow = on != 0;
}

int seq_follow(const seq_t *s) { return s->follow; }

int seq_preset_loaded(const seq_t *s) { return s->preset; }
void seq_set_preset_loaded(seq_t *s, int n) {
    if (n >= 0 && n <= SEQ_PRESETS_MAX) s->preset = n;
}

/* A pick of the bar already shown changes nothing: the host echoes back what the getter returned. */
void seq_set_view(seq_t *s, int bar) {
    int b = bar >= 0 && bar < cur(s)->bars ? bar : 0;
    if (b == s->view_bar) return;
    act(s);
    s->view_bar = b;
}

/* Bounded appends: o->n stops counting once the buffer is full, so a save that does not fit is refused, not cut. */
typedef struct out { char *buf; int len, n; } out_t;

static void outf(out_t *o, const char *fmt, ...) {
    va_list ap;
    if (o->n >= o->len) return;
    va_start(ap, fmt);
    o->n += vsnprintf(o->buf + o->n, (size_t)(o->len - o->n), fmt, ap);
    va_end(ap);
}

static const char HEX[] = "0123456789abcdef";

/* One 7-character step record, as outf's "%02x%d%d%02x%x" would write it: written by hand because a 16-bar part
 * has up to 1024 of them and the wrapper holds its lock (the audio thread waits) while the state saves. */
static void out_step(out_t *o, int i, const seq_step_t *x) {
    if (o->n >= o->len) return;
    char *d = o->buf + o->n;
    o->n += 7;
    if (o->n >= o->len) return;   /* no room for the record and its NUL: the save is refused */
    d[0] = HEX[(i >> 4) & 15];
    d[1] = HEX[i & 15];
    d[2] = (char)('0' + x->tone);
    d[3] = (char)('0' + x->oct + 5);
    d[4] = HEX[(x->vel >> 4) & 15];
    d[5] = HEX[x->vel & 15];
    d[6] = HEX[(x->len - 1) & 15];
    d[7] = 0;
}

static int any_lock(const seq_lane_t *ln, int nsteps) {
    for (int i = 0; i < nsteps / 8; i++)
        if (ln->lock[i]) return 1;
    return 0;
}

/* 0.16: a bar with its own variation (NEW BAR) */
static int any_bar_var(const seq_lane_t *ln) {
    for (int b = 0; b < SEQ_MAX_BARS; b++)
        if (ln->bar_var[b]) return 1;
    return 0;
}

/* 0.16 `|<c><n>.bv=`: 32 hex digits, two per bar (bars 1-16, hidden bars too), each bar's variation; only when one is set */
static void out_bar_var(out_t *o, char c, int lane, const seq_lane_t *ln) {
    char d[2 * SEQ_MAX_BARS + 1];
    for (int b = 0; b < SEQ_MAX_BARS; b++) d[2 * b] = HEX[ln->bar_var[b] >> 4], d[2 * b + 1] = HEX[ln->bar_var[b] & 15];
    d[sizeof d - 1] = 0;
    outf(o, "|%c%d.bv=%s", c, lane + 1, d);
}

/* `|A<n>.lk=` (part letter c): 64 hex digits, one per 4 steps from step 1, the lowest step in the lowest bit; none past the loop */
static void out_locks(out_t *o, char c, int lane, const seq_lane_t *ln, int nsteps) {
    char d[2 * sizeof ln->lock + 1];
    for (int i = 0; i < (int)sizeof ln->lock; i++) {
        int b = i * 8 < nsteps ? ln->lock[i] : 0;
        d[2 * i] = HEX[b & 15];
        d[2 * i + 1] = HEX[b >> 4];
    }
    d[sizeof d - 1] = 0;
    outf(o, "|%c%d.lk=%s", c, lane + 1, d);
}

/* 0.18 `|<c>.rb=`: a DRAWN Chords lane's bars up to filled, comma-separated, each `L` when locked, then the library index
 * it was picked from (none for a bar drawn from nothing), then, when edited, its 16 codes (A x g - .): `3` is Pulse,
 * `L3x---x...` a locked bar edited from Pulse. `|<c>.rz=` SYNC,GHOSTS,HUMANIZE,FILL, only when one is not 0.
 * 0.19: drawn lane dl's (DL_*), with its DLANE prefix: a DRAWN Bass lane's as `|<c>2.rb=` (its index in the Bass library) and
 * `|<c>2.rz=`. */
static void out_rbars(out_t *o, char c, const seq_part_t *p, int dl) {
    char d[SEQ_MAX_BARS * 20 + 1], *w = d;   /* an entry: L, 2 digits, 16 codes, a comma */
    const char *n = DLANE[dl].fp;
    unsigned char lib[16];
    for (int b = 0; b < p->filled; b++) {
        const rbar_t *r = &p->rb[dl][b];
        int from = r->from >= 0 && (r->from < RHYTHM_COUNT || rhythm_is_jam(r->from)) ? r->from : -1;   /* 0.22: a JAM index too */
        if (from >= 0) rhythm_lib_codes(DLANE[dl].rlib, from, lib);
        if (b) *w++ = ',';
        if (r->lock) *w++ = 'L';
        if (from >= 10) *w++ = (char)('0' + from / 10);
        if (from >= 0) *w++ = (char)('0' + from % 10);
        if (from < 0 || r->ed || memcmp(lib, r->c, 16)) {
            rhythm_format(r->c, w);
            w += 16;
        }
    }
    *w = 0;
    outf(o, "|%c%s.rb=%s", c, n, d);
    const signed char *rk = p->rk[dl];
    if (rk[RK_CAD] || rk[RK_BREATH])   /* 0.24: CADENCE and BREATH appended, only when one is set (else 0.23's 4 values, byte for byte) */
        outf(o, "|%c%s.rz=%d,%d,%d,%d,%d,%d", c, n, rk[RK_SYNC], rk[RK_GHOSTS], rk[RK_HUM], rk[RK_FILL], rk[RK_CAD], rk[RK_BREATH]);
    else if (rk[RK_SYNC] || rk[RK_GHOSTS] || rk[RK_HUM] || rk[RK_FILL])
        outf(o, "|%c%s.rz=%d,%d,%d,%d", c, n, rk[RK_SYNC], rk[RK_GHOSTS], rk[RK_HUM], rk[RK_FILL]);
}

/* 0.22 `|<c>1.dj=` (Chords) / `|<c>2.dj=` (Bass): drawn lane dl's JAM state c,l,sw,var,swing,tm (djam_t), only once JAM was used (a
 * block played, or a setting not a new instance's): a project that never jammed saves as 0.21 did */
static void out_djam(out_t *o, char c, const seq_part_t *p, int dl) {
    const djam_t *j = &p->djam[dl];
    if (j->c < 0 && j->sw == 1 && !j->var && !j->swing && !j->tm) return;
    outf(o, "|%c%d.dj=%d,%d,%d,%d,%d,%d", c, DLANE[dl].lane + 1, j->c, j->l, j->sw, j->var, j->swing, j->tm);
}

/* 0.24 `|<c>1.la=` (Chords) / `|<c>2.la=` (Bass): drawn lane dl's LOCK WITH and ANTICIPATE (lw,an), only when not OWN and OFF */
static void out_la(out_t *o, char c, const seq_part_t *p, int dl) {
    if (p->lw[dl] || p->an[dl]) outf(o, "|%c%d.la=%d,%d", c, DLANE[dl].lane + 1, p->lw[dl], p->an[dl]);
}

/* 0.19 note settings as saved: style,pedal,octave,mov,octs,app,reg with separator sep */
static void out_bset(out_t *o, const bass_set_t *b, char sep) {
    outf(o, "%d%c%d%c%d%c%d%c%d%c%d%c%d", b->style, sep, b->pedal, sep, b->octave, sep, b->k[BK_MOV], sep, b->k[BK_OCTS], sep,
         b->k[BK_APP], sep, b->k[BK_REG]);
}

/* 0.19 a DRAWN Bass lane's fields (its DLANE save_extra): ln's `.bv` (as today), then its notes: `|<c>2.ab=` its fixed notes (bass.fix,
 * below filled, sounding or not) as <step hex2><note hex2> pairs, only when there is one; `|<c>2.bx=` the note settings (out_bset),
 * only when not Pop Root's; `|<c>2.bl=` each note-locked bar below filled as bar:style.pedal.octave.mov.octs.app.reg, `;`-separated,
 * only when there is one */
static void out_bass(out_t *o, char c, const seq_part_t *p, const seq_lane_t *ln) {
    const bass_set_t pop = bass_style_set(BASS_POP_ROOT);
    int n = 0;
    if (any_bar_var(ln)) out_bar_var(o, c, LANE_BASS, ln);
    for (int i = 0; i < p->filled * SEQ_STEPS; i++) {
        if (!p->bass.fix[i]) continue;
        if (!n++) outf(o, "|%c2.ab=", c);
        outf(o, "%02x%02x", i, p->bass.fix[i]);
    }
    if (memcmp(&p->bass.set, &pop, sizeof pop)) {
        outf(o, "|%c2.bx=", c);
        out_bset(o, &p->bass.set, ',');
    }
    n = 0;
    for (int b = 0; b < p->filled; b++) {
        if (!(p->bass.nlock >> b & 1)) continue;
        if (n++) outf(o, ";%d:", b);
        else outf(o, "|%c2.bl=%d:", c, b);
        out_bset(o, &p->bass.frozen[b], '.');
    }
}

/* 0.20 a JAM set's 12 saved values, in order (mset_fields): c l form breath cad fill ans stray aoct reg sc sl, each one base-36
 * character, its value less its LO (aoct + 1); HI: each one's top */
enum { MSET_N = 12 };
static const signed char MSET_LO[MSET_N] = {0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0},
                         MSET_HI[MSET_N] = {MJ_COLS - 1, MJ_LEVELS - 1, MF_COUNT - 1, 3, 3, 3, MA_COUNT - 1, 3, 1, 2, MJ_COLS - 1, MJ_LEVELS - 1};
static const char B36[] = "0123456789abcdefghijklmnopqrstuvwxyz";

static void mset_fields(mjam_set_t *m, signed char *f[MSET_N]) {
    signed char *x[MSET_N] = {&m->c, &m->l, &m->form, &m->breath, &m->cad, &m->fill, &m->ans, &m->stray, &m->aoct, &m->reg, &m->sc, &m->sl};
    memcpy(f, x, sizeof x);
}

static void out_mset(out_t *o, mjam_set_t m) {
    signed char *f[MSET_N];
    char d[MSET_N + 1];
    mset_fields(&m, f);
    for (int k = 0; k < MSET_N; k++) d[k] = B36[clampi(*f[k] - MSET_LO[k], 0, 35)];
    d[MSET_N] = 0;
    outf(o, "%s", d);
}

/* 0.21 a set's DENSITY, RANGE, VARIATION and (Amendment 1) TIME as MSET-style characters (value - LO, base 36) */
static void mnew_chars(const mjam_set_t *m, char d[5]) {
    d[0] = B36[clampi(m->dens + 2, 0, 35)], d[1] = B36[clampi(m->rng + 1, 0, 35)], d[2] = B36[clampi(m->var, 0, 35)];
    d[3] = B36[clampi(m->tm + 1, 0, 35)], d[4] = 0;
}

/* 0.21 the fields 0.20 loaders do not know, each only when not the default: `|<c>3.jy=` DENSITY, RANGE, VARIATION, TIME (4 characters),
 * `|<c>3.jv=` SWING, ACCENT (2), `|<c>3.jm=` bar:<4 characters>, `;`-separated, the same of each locked bar's frozen settings */
static void out_mnew(out_t *o, char c, const seq_part_t *p) {
    const mjam_state_t *m = &p->mjam;
    char d[5], dflt[5];
    int n = 0;
    mjam_set_t z = mjam_default();
    mnew_chars(&z, dflt);
    mnew_chars(&m->set, d);
    if (strcmp(d, dflt)) outf(o, "|%c3.jy=%s", c, d);
    if (m->swing || m->acc != 1) outf(o, "|%c3.jv=%c%c", c, B36[m->swing], B36[m->acc]);
    for (int b = 0; b < p->filled; b++) {
        if (!(m->nlock >> b & 1)) continue;
        mnew_chars(&m->frozen[b], d);
        if (!strcmp(d, dflt)) continue;
        if (n++) outf(o, ";%d:%s", b, d);
        else outf(o, "|%c3.jm=%d:%s", c, b, d);
    }
}

/* 0.20 a JAM Melody lane's fields, after its `3.g` (no step records, locks or recipe): ln's `.bv` (as today), then each only when not
 * a new instance's: `|<c>3.jx=` the settings (out_mset), `|<c>3.jw=` SWITCH AT 0..3, `|<c>3.jk=` PUSH,GATE,DYNAMICS, `|<c>3.jl=` each
 * locked bar below filled as bar:<its frozen settings>, `;`-separated, `|<c>3.jf=` the fixed steps below filled as <step hex2><value
 * hex2> pairs (a note 01..7f, 80 a rest, 81 a hold). A queued block is never saved: the settings hold the playing one. */
static void out_mjam(out_t *o, char c, const seq_part_t *p, const seq_lane_t *ln) {
    const mjam_state_t *m = &p->mjam;
    const mjam_set_t d = mjam_default();
    int n = 0, gate = p->mel.kn[MK_GATE];
    if (any_bar_var(ln)) out_bar_var(o, c, LANE_MEL_UP, ln);
    mjam_set_t x = m->set;
    x.dens = x.rng = x.var = x.tm = 0;   /* 0.21: not in jx (0.20 loaders need exactly 12 characters) */
    if (memcmp(&x, &d, sizeof d)) {
        outf(o, "|%c3.jx=", c);
        out_mset(o, m->set);
    }
    if (m->sw != 1) outf(o, "|%c3.jw=%d", c, m->sw);
    if (m->push || gate != 50 || m->dyn != 50) outf(o, "|%c3.jk=%d,%d,%d", c, m->push, gate, m->dyn);
    for (int b = 0; b < p->filled; b++) {
        if (!(m->nlock >> b & 1)) continue;
        if (n++) outf(o, ";%d:", b);
        else outf(o, "|%c3.jl=%d:", c, b);
        out_mset(o, m->frozen[b]);
    }
    n = 0;
    for (int i = 0; i < p->filled * SEQ_STEPS; i++) {
        if (!m->fix[i]) continue;
        if (!n++) outf(o, "|%c3.jf=", c);
        outf(o, "%02x%02x", i, m->fix[i]);
    }
    out_mnew(o, c, p);
}

/* A MANUAL lane saves every on-step; a generated one its recipe (g), its locks (lk) and only its locked on-steps, the
 * rest regenerating on load. g is left out for a MANUAL lane at MANUAL_GEN, so an old project saves as it did. 0.10: a
 * saved absolute step's record is its root at the octave nearest its note (in v, the progression saved), and its note
 * follows in `<c><n>.ab=` (<step hex2><note hex2> pairs), only when there is one (0.9 ignores it). */
static void save_lane(out_t *o, const seq_t *s, char c, const seq_part_t *p, const prog_view_t *v, int lane, int mute,
                      const seq_lane_t *ln) {
    const seq_gen_t *g = &ln->gen;
    int gen = g->style != GEN_MANUAL, nsteps = p->bars * SEQ_STEPS, nab = 0, dl = dl_of(lane);
    outf(o, "|%c%d=%d:", c, lane + 1, mute);
    if (dl >= 0 && g->style == DLANE[dl].style) {   /* 0.18: no records, the bars in `.rb` (0.19: `2.rb`, after the notes' fields) */
        outf(o, "|%c%d.g=%d,%d,%d", c, lane + 1, g->style, g->seed, g->complexity);
        if (DLANE[dl].save_extra) DLANE[dl].save_extra(o, c, p, ln);
        out_rbars(o, c, p, dl);
        out_djam(o, c, p, dl);   /* 0.22 */
        out_la(o, c, p, dl);     /* 0.24 */
        return;
    }
    if (lane == LANE_MEL_UP && g->style == GEN_MEL_JAM) {   /* 0.20: no records, its notes in `3.jx` .. `3.jf` */
        outf(o, "|%c3.g=%d,%d,%d", c, g->style, g->seed, g->complexity);
        out_mjam(o, c, p, ln);
        return;
    }
    for (int i = 0; i < nsteps; i++) {
        seq_step_t x = ln->steps[i];
        if (!x.on || (gen && !locked(ln, i))) continue;
        if (x.abs) x.tone = TONE_R, x.oct = (signed char)root_oct(s, v, p->chord_len, i, x.abs), nab++;
        out_step(o, i, &x);
    }
    if (gen || g->seed != MANUAL_GEN.seed || g->complexity != MANUAL_GEN.complexity)   /* a recipe: 0.10's Hook (.mr) */
        outf(o, "|%c%d.g=%d,%d,%d", c, lane + 1, g->style == GEN_MEL_RECIPE ? GEN_MEL_HOOK : g->style, g->seed, g->complexity);
    if (gen && g->variant) outf(o, "|%c%d.cv=%d", c, lane + 1, g->variant);   /* 0.13; absent: 0 (0.12's template) */
    if (gen && any_lock(ln, nsteps)) out_locks(o, c, lane, ln, nsteps);
    if (gen && any_bar_var(ln)) out_bar_var(o, c, lane, ln);
    if (!nab) return;
    outf(o, "|%c%d.ab=", c, lane + 1);
    for (int i = 0; i < nsteps; i++)
        if (ln->steps[i].on && ln->steps[i].abs && (!gen || locked(ln, i))) outf(o, "%02x%02x", i, ln->steps[i].abs);
}

/* 0.14 `|<c>.ch=`: every slot of the loop, 4 characters each, <degree 0-6><src 0-3><colour 0-4><lock 0|1> */
static void out_slot_chords(out_t *o, char c, const prog_view_t *v) {
    char d[4 * SEQ_MAX_PROG + 1];
    for (int i = 0; i < v->len; i++) {
        d[4 * i] = (char)('0' + v->prog[i]);
        d[4 * i + 1] = (char)('0' + v->src[i]);
        d[4 * i + 2] = (char)('0' + v->colour[i]);
        d[4 * i + 3] = (char)('0' + (int)(v->lock >> i & 1));
    }
    d[4 * v->len] = 0;
    outf(o, "|%c.ch=%s", c, d);
}

/* 0.24 `|<c>.cp=` the change points of the bars up to filled, comma-separated, 5 characters each: bar and step in hex, degree, src,
 * type (seq_cp_t), as `58200` (bar 6 step 9, III, Knobs); only when one exists (a state without saves as 0.23 did) */
static void out_cp(out_t *o, char c, const seq_part_t *p) {
    static const char HEX[] = "0123456789abcdef";
    char d[SEQ_MAX_BARS * SEQ_CP_MAX * 6 + 1], *w = d;
    for (int b = 0; b < p->filled; b++)
        for (int k = 0; k < SEQ_CP_MAX && p->cp[b][k].step >= 0; k++) {
            const seq_cp_t *x = &p->cp[b][k];
            if (w > d) *w++ = ',';
            *w++ = HEX[b], *w++ = HEX[x->step], *w++ = (char)('0' + x->deg), *w++ = (char)('0' + x->src), *w++ = (char)('0' + x->type);
        }
    *w = 0;
    if (w > d) outf(o, "|%c.cp=%s", c, d);
}

/* 0.24 a hit octave as one `.ho` character: HO_CH[k x 16 + step], k 0..3 the octave -2, -1, +1, +2 */
static const char HO_CH[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.";

/* 0.24 `|<c>.ho=` the hit octaves (ruling 13: sparse), only of the bars up to filled with any, comma-separated: the bar in hex, then one
 * character per step with an octave (HO_CH), as `5jA` (bar 6: step 4 +1, step 1 -2); only when one is set */
static void out_hoct(out_t *o, char c, const seq_part_t *p) {
    char d[SEQ_MAX_BARS * 18 + 1], *w = d;
    for (int b = 0; b < p->filled; b++) {
        char *at = w;
        if (w > d) *w++ = ',';
        *w++ = "0123456789abcdef"[b];
        for (int i = 0; i < SEQ_STEPS; i++)
            if (p->hoct[b][i]) *w++ = HO_CH[(p->hoct[b][i] + (p->hoct[b][i] < 0 ? 2 : 1)) * 16 + i];
        if (w - at == 1 + (at > d)) w = at;   /* no octave in the bar: no entry */
    }
    *w = 0;
    if (w > d) outf(o, "|%c.ho=%s", c, d);
}

/* A part's own settings, `|<c>.len=` to `|<c>.voi=`, then 0.9's only when not neutral (a 0.8-sounding part saves as 0.8
 * did; 0.8 ignores them and plays prog, diatonic triads): `.pq=` <src><colour> per slot, `.sl=` the slot locks in hex,
 * `.cs=` the SHAPE values in enum order. 0.17: `.vx=` (after `.voi`) the Chords voicing, only when something is not what `.voi` and a
 * new part give (Close and Smooth are `.voi`'s): style,mode,colour level,inversion,spread,strum direction (STRUM's magnitude
 * is `.cs`'s).
 * 0.14: `.prog`, `.pq` and `.sl` hold the loop's phrase (its first `phrase` slots, at most 16, hidden ones included when the
 * phrase is longer than the loop), exactly as 0.13 wrote its progression: a loop filled from a phrase saves byte for byte
 * as 0.13 did. A loop with no period of 16 or less (settle_slots) writes its first 16 slots there (what a 0.13 build then
 * plays, repeating) and every slot in `.ch` after `.cs`. */
/* v: the progression saved */
static void save_part_head(out_t *o, char c, const seq_part_t *p, const prog_view_t *v) {
    int pq = 0, cs = memcmp(p->lane_shape[LANE_CHORDS], LSHAPE_NEUTRAL[LANE_CHORDS], sizeof LSHAPE_NEUTRAL[0]) != 0, n = v->phrase;
    unsigned sl = (unsigned)(v->lock & (SLOT_BIT(n) - 1));
    outf(o, "|%c.len=%d|%c.cl=%d|%c.h=%d,%d|%c.prog=", c, p->bars, c, p->chord_len, c, p->harmony, v->hseed, c);
    for (int i = 0; i < n; i++) {
        outf(o, "%d", v->prog[i]);
        pq |= v->src[i] | v->colour[i];
    }
    outf(o, "|%c.voi=%d", c, p->voicing);
    if (p->vstyle != -1 || p->vmode >= VMODE_TOPLINE || p->vcol || p->vinv || p->vspr || p->vstrdir)
        outf(o, "|%c.vx=%d,%d,%d,%d,%d,%d", c, p->vstyle, p->vmode, p->vcol, p->vinv, p->vspr, p->vstrdir);
    if (pq) {
        outf(o, "|%c.pq=", c);
        for (int i = 0; i < n; i++) outf(o, i ? ",%d%d" : "%d%d", v->src[i], v->colour[i]);
    }
    if (sl) outf(o, "|%c.sl=%x", c, sl);
    if (cs) {
        outf(o, "|%c.cs=", c);
        for (int i = 0; i < SHAPE_COUNT; i++) outf(o, i ? ",%d" : "%d", p->lane_shape[LANE_CHORDS][i]);
    }
    if (!view_loops(v)) out_slot_chords(o, c, v);
    if (v->rank != n) outf(o, "|%c.rk=%d", c, v->rank);   /* never for a phrase as dealt (a 0.13 project's) */
    out_cp(o, c, p);
    out_hoct(o, c, p);   /* 0.24 */
}

/* 0.11: `|<c>3.mr=` a recipe Melody 1's recipe (its .g says GEN_MEL_HOOK, which 0.10 plays), its phrase the old one
 * nearest the phrasing (0.11-0.13 read it); VST-12: then, unless the phrasing is exactly that old phrase's, `|<c>3.mp=`
 * the phrasing, form,breath,cadence,fill */
static void out_mel(out_t *o, char c, const mel_recipe_t *r) {
    outf(o, "|%c3.mr=%d,%d,%d,%d,%d", c, r->type, r->opt[0], r->opt[1], r->opt[2], mel_legacy_phrase(r));
    for (int k = 0; k < MK_COUNT; k++) outf(o, ",%d", r->kn[k]);
    outf(o, ",%d,%d,%d", r->rev, r->inv, r->shift);
    if (!mel_is_legacy(r))   /* an old phrase's phrasing saves as 0.13 did, byte for byte */
        outf(o, "|%c3.mp=%d,%d,%d,%d", c, r->ph[MP_FORM], r->ph[MP_BREATH], r->ph[MP_CADENCE], r->ph[MP_FILL]);
}

/* A part's lanes; mutes 0 writes its mutes as 0 (0.26: parts B-F when every part has A's, which a load gives them). 0.10:
 * after the Bass's fields, `<c>2.bs=` its SHAPE values in BSHAPE order, only when not neutral (0.9 ignores it). */
static void save_part_lanes(out_t *o, const seq_t *s, char c, const seq_part_t *p, const prog_view_t *v, int mutes) {
    for (int l = 0; l < SEQ_LANES; l++) {
        const seq_lane_t *ln = &p->lane[l];
        save_lane(o, s, c, p, v, l, mutes && ln->mute, ln);
        if (l == LANE_MEL_UP && ln->gen.style == GEN_MEL_RECIPE) out_mel(o, c, &p->mel);
        if (l != LANE_BASS) continue;
        const int *sh = p->lane_shape[l];
        if (!memcmp(sh, LSHAPE_NEUTRAL[l], sizeof LSHAPE_NEUTRAL[0])) continue;
        outf(o, "|%c2.bs=", c);
        for (int i = 0; i < BSHAPE_COUNT; i++) outf(o, i ? ",%d" : "%d", sh[i]);
    }
}

/* 0.26 1 if a part in use mutes other lanes than the Verse */
static int mutes_differ(const seq_t *s) {
    for (int p = 1; p < SEQ_NPARTS; p++) {
        if (!seq_part_used(s, p)) continue;
        for (int l = 0; l < SEQ_LANES; l++)
            if (s->part[p].lane[l].mute != s->part[0].lane[l].mute) return 1;
    }
    return 0;
}

/* The Verse is part A, whatever plays, with the project's settings between its head and its lanes (as 0.3-0.7 wrote it).
 * Each other part in use follows as B-F (index + 'A'), whole or not at all: one that does not fit is left out, the
 * later ones still tried, and named in unsaved_mask until a save fits them all. */
static int save_state(seq_t *s, char *buf, int len) {
    out_t o = {buf, len, 0};
    s->unsaved_mask = 0;
    outf(&o, "csq2|gv=1|k=%d|s=%d", s->key, s->scale);
    prog_view_t v = part_view(&s->part[0]);
    save_part_head(&o, 'A', &s->part[0], &v);
    outf(&o, "|ch=%d,%d,%d", s->chan[0], s->chan[1], s->chan[2]);
    if (s->swing) outf(&o, "|sw=%d", s->swing);   /* the output stage only when not neutral: a 0.6-sounding state saves as before */
    if (s->gate != 50) outf(&o, "|gt=%d", s->gate);
    if (s->dyn != 50) outf(&o, "|dy=%d", s->dyn);
    if (!s->follow) outf(&o, "|fo=0");   /* only when off: a state with FOLLOW on saves as 0.16 did */
    if (s->preset) outf(&o, "|gn=%d", s->preset);   /* 0.23 only when set: a state without a preset saves as 0.22 did */
    int pm = mutes_differ(s);
    if (pm) outf(&o, "|pm=1");   /* 0.26 only then: a state whose parts share the Verse's mutes saves as 0.25 did */
    save_part_lanes(&o, s, 'A', &s->part[0], &v, 1);
    if (o.n >= len) {
        s->unsaved_mask = s->used_mask;
        return 0;
    }
    for (int p = 1; p < SEQ_NPARTS; p++) {
        if (!seq_part_used(s, p)) continue;
        int at = o.n;
        v = part_view(&s->part[p]);
        save_part_head(&o, (char)('A' + p), &s->part[p], &v);
        save_part_lanes(&o, s, (char)('A' + p), &s->part[p], &v, pm);
        if (o.n < len) continue;
        o.n = at;   /* the part is cut off: undone whole */
        buf[at] = 0;
        s->unsaved_mask |= 1 << p;
    }
    return o.n;
}

/* 0.17: an open picker or browser's tries are not saved: the part is written as it was at open (the transaction's backup swapped
 * in for the call; the engine's lock holds the audio thread off) */
static void txn_swap(seq_t *s);

int seq_state_save(seq_t *s, char *buf, int len) {
    int n;
    txn_swap(s);
    n = save_state(s, buf, len);
    txn_swap(s);
    return n;
}

int seq_unsaved_parts(const seq_t *s) { return s->unsaved_mask; }

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int hex2(const char *p) { int a = hexv(p[0]), b = a < 0 ? -1 : hexv(p[1]); return b < 0 ? -1 : a * 16 + b; }

static int num(const char *v, int n) {   /* 1-3 decimal digits, else -1 */
    int x = 0;
    if (n < 1 || n > 3) return -1;
    for (int i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') return -1;
        x = x * 10 + v[i] - '0';
    }
    return x;
}

static int load_lane(seq_lane_t *ln, const char *v, int n, int bars) {
    if (n < 2 || (v[0] != '0' && v[0] != '1') || v[1] != ':' || (n - 2) % 7) return 0;
    memset(ln, 0, sizeof *ln);
    ln->gen = MANUAL_GEN;   /* 0.3/0.4 lanes are MANUAL */
    ln->mute = v[0] == '1';
    for (const char *r = v + 2; r < v + n; r += 7) {
        int step = hex2(r), tone = r[2] - '0', oct = r[3] - '5', vel = hex2(r + 4), len = hexv(r[6]);
        if (step < 0 || step >= bars * SEQ_STEPS || tone < TONE_R || tone > TONE_7 || oct < -4 || oct > 4 || vel < 1 ||
            vel > 127 || len < 0)
            return 0;
        ln->steps[step] = (seq_step_t){1, (unsigned char)tone, (signed char)oct, (unsigned char)vel, (unsigned char)(len + 1), 0};
    }
    return 1;
}

static int load_prog(seq_part_t *p, const char *v, int n) {
    if (n < 1 || n > SEQ_LEGACY_PROG) return 0;   /* the phrase (0.14: `.ch` holds a longer loop) */
    for (int i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '6') return 0;
        p->prog[i] = v[i] - '0';
    }
    p->prog_len = n;
    return 1;
}

/* `count` comma-separated decimals of 1-5 digits, filling all n characters; else 0 */
static int num_list(const char *v, int n, int *out, int count) {
    int k = 0, digits = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        if (v[i] == ',') {
            if (!digits || ++k == count) return 0;
            out[k] = 0;
            digits = 0;
        } else if (v[i] >= '0' && v[i] <= '9' && digits < 5) {
            out[k] = out[k] * 10 + v[i] - '0';
            digits++;
        } else {
            return 0;
        }
    }
    return digits && k == count - 1;
}

static int style_fits(int lane, int style);

/* `A<n>.g=<style>,<seed>,<complexity>`; v is NULL when absent: the lane keeps MANUAL_GEN. A well-formed g naming a style
 * this build lacks for the lane (a later build's, or one for another lane) is not refused (a refused load leaves
 * defaults, which MPC then saves over the song): the lane stays MANUAL at MANUAL_GEN with its records as its notes.
 * Returns 0 refused, 1 loaded, 2 loaded as MANUAL (the lane's lk is then ignored). */
static int load_gen(seq_lane_t *ln, int lane, const char *v, int n) {
    int g[3], d = dl_of(lane);
    if (!v) return 1;
    if (!num_list(v, n, g, 3) || g[1] > 65535 || g[2] > 100) return 0;
    if (!style_fits(lane, g[0]) && !(d >= 0 && g[0] == DLANE[d].style) && !(lane == LANE_MEL_UP && g[0] == GEN_MEL_JAM)) return 2;
    ln->gen = (seq_gen_t){g[0], g[1], g[2], 0};
    return 1;
}

/* `A<n>.lk=`, as out_locks writes it: a generated lane only. Absent: no locks. Locks past the loop are dropped, not
 * refused (a refused load leaves defaults, which MPC then saves over the song); so is a record at an unlocked step,
 * which settle_loaded regenerates over. */
static int load_locks(seq_lane_t *ln, const char *v, int n, int nsteps) {
    if (!v) return 1;
    if (ln->gen.style == GEN_MANUAL || n != 2 * (int)sizeof ln->lock) return 0;
    for (int i = 0; i < n; i++) {
        int x = hexv(v[i]);
        if (x < 0) return 0;
        if (i * 4 < nsteps) ln->lock[i / 2] |= (unsigned char)(x << (i % 2 * 4));
    }
    return 1;
}

/* `A.h=<harmony>,<harmony_seed>`; v is NULL when the state predates 0.5: 30, 1 */
static int load_harmony(seq_part_t *p, const char *v, int n) {
    int h[2] = {30, 1};
    if (v && (!num_list(v, n, h, 2) || h[0] > 100 || h[1] > 65535)) return 0;
    p->harmony = h[0];
    p->harmony_seed = h[1];
    return 1;
}

/* `ch=<c1>,<c2>,<c3>`; v is NULL when the state predates 0.7: 2, 3, 4 */
static int load_channels(int *ch, const char *v, int n) {
    int c[3] = {2, 3, 4};
    if (v && (!num_list(v, n, c, 3) || c[0] < 1 || c[0] > 16 || c[1] < 1 || c[1] > 16 || c[2] < 1 || c[2] > 16)) return 0;
    memcpy(ch, c, sizeof c);
    return 1;
}

/* `sw=`, `gt=`, `dy=`: 0..100 each; v is NULL when absent (neutral, or saved before 0.7): `dflt` */
static int load_out(int *x, const char *v, int n, int dflt) {
    *x = v ? num(v, n) : dflt;
    return *x >= 0 && *x <= 100;
}

/* The fields of a csq2 line for one part letter, found but not yet checked: the first F_REQUIRED must all be present; the
 * rest may be absent (A.voi before 0.4; gv, A.h, A<n>.g and A<n>.lk before 0.5; ch before 0.7; sw, gt, dy when neutral).
 * A name in NAME starting with a lower-case letter is the project's; the others follow the part's letter. */
enum { F_REQUIRED = 5 + SEQ_LANES, F_VOI = F_REQUIRED, F_GV, F_H, F_G, F_LK = F_G + SEQ_LANES, F_CH = F_LK + SEQ_LANES, F_SW, F_GT,
       F_DY, F_PQ, F_SL, F_CS, F_BS, F_AB, F_MR = F_AB + SEQ_LANES, F_CV, F_SCH = F_CV + SEQ_LANES, F_RK, F_MP, F_BV,
       F_FO = F_BV + SEQ_LANES, F_VX, F_RB, F_RZ, F_BRB, F_BRZ, F_BX, F_BL, F_JX, F_JW, F_JK, F_JL, F_JF, F_JY, F_JV, F_JM,
       F_DJ, F_GN = F_DJ + DL_COUNT, F_CP, F_HO, F_LA, F_PM = F_LA + DL_COUNT, F_COUNT };
/* pq, sl, cs: 0.9, bs, ab: 0.10, when not neutral; mr: 0.11, a recipe melody; cv: 0.13, a chord rhythm's variant;
 * .ch (F_SCH): 0.14, every slot of a loop with no period of 16 or less; .rk: 0.14, the COLOUR ranks' period; mp: VST-12, a
 * recipe melody's phrasing; bv: 0.16, a generated lane's bars with their own variation; fo: after 0.16, FOLLOW off; vx: 0.17, the Chords voicing, when not a new part's;
 * rb, rz: 0.18, a DRAWN Chords lane's bars and RHYTHM knobs; 2.rb, 2.rz, 2.bx, 2.bl: 0.19, a DRAWN Bass lane's bars, RHYTHM knobs,
 * note settings and note-locked bars (its fixed notes in 2.ab); 3.jx, 3.jw, 3.jk, 3.jl, 3.jf: 0.20, a JAM Melody lane's settings,
 * SWITCH AT, PUSH/GATE/DYNAMICS, locked bars and fixed steps (out_mjam); 3.jy, 3.jv, 3.jm: 0.21, DENSITY/RANGE/VARIATION/TIME, SWING/ACCENT
 * and the locked bars' DENSITY/RANGE/VARIATION/TIME (out_mnew), when not the defaults; 1.dj, 2.dj (F_DJ + DL_*): 0.22, a drawn lane's
 * JAM state (out_djam), once used; gn: 0.23, the genre preset last loaded, when set; cp: 0.24, the change points (out_cp), when any; ho:
 * 0.24, the hit octaves (out_hoct), when any; 1.la, 2.la (F_LA + DL_*): 0.24, a drawn lane's LOCK WITH and ANTICIPATE (out_la), when not
 * the defaults; pm: 0.26, the parts keep their own mutes (else every part takes A's) */
typedef struct fields { const char *v[F_COUNT]; int n[F_COUNT]; } fields_t;

static void find_fields(const char *str, char letter, fields_t *f) {
    static const char *const NAME[F_COUNT] = {"k", "s", ".len", ".cl", ".prog", "1", "2", "3", "4", ".voi", "gv", ".h",
                                              "1.g", "2.g", "3.g", "4.g", "1.lk", "2.lk", "3.lk", "4.lk", "ch", "sw",
                                              "gt", "dy", ".pq", ".sl", ".cs", "2.bs", "1.ab", "2.ab", "3.ab", "4.ab", "3.mr",
                                              "1.cv", "2.cv", "3.cv", "4.cv", ".ch", ".rk", "3.mp",
                                              "1.bv", "2.bv", "3.bv", "4.bv", "fo", ".vx", ".rb", ".rz", "2.rb", "2.rz", "2.bx",
                                              "2.bl", "3.jx", "3.jw", "3.jk", "3.jl", "3.jf", "3.jy", "3.jv", "3.jm",
                                              "1.dj", "2.dj", "gn", ".cp", ".ho", "1.la", "2.la", "pm"};
    memset(f, 0, sizeof *f);
    for (const char *p = str; *p;) {
        const char *end = strchr(p, '|'), *eq;
        if (!end) end = p + strlen(p);
        eq = memchr(p, '=', (size_t)(end - p));
        for (int i = 0; eq && i < F_COUNT; i++) {
            int part = NAME[i][0] < 'a' || NAME[i][0] > 'z';
            const char *name = p + part;
            if (part && p[0] != letter) continue;
            if ((size_t)(eq - name) == strlen(NAME[i]) && !strncmp(name, NAME[i], (size_t)(eq - name))) {
                f->v[i] = eq + 1;
                f->n[i] = (int)(end - eq - 1);
            }
        }
        p = *end ? end + 1 : end;
    }
}

static int load_voicing(seq_part_t *p, const char *v, int n) {   /* v is NULL when the state predates 0.4: close */
    int x = v ? (n == 1 ? v[0] - '0' : -1) : VOICING_CLOSE;
    if (x != VOICING_CLOSE && x != VOICING_SMOOTH) return 0;
    set_vmode(p, x == VOICING_CLOSE ? VMODE_CLOSE : VMODE_SMOOTH);
    return 1;
}

/* `<n>.ab=`: <step hex2><note hex2> pairs, each an on-step's record and a note 1..127. Absent or malformed: ignored whole
 * (the records play, chord-relative), never refused. */
static void load_abs(seq_lane_t *ln, const char *v, int n, int nsteps) {
    if (!v || !n || n % 4) return;
    for (int i = 0; i < n; i += 4) {
        int st = hex2(v + i), note = hex2(v + i + 2);
        if (st < 0 || st >= nsteps || !ln->steps[st].on || note < 1 || note > 127) return;
    }
    for (int i = 0; i < n; i += 4) set_abs(&ln->steps[hex2(v + i)], hex2(v + i + 2));   /* the record was the 0.9 fallback */
}

/* 0.13 `<n>.cv=`: a chord rhythm's variant, 1 or 2 (0 is never written). Absent or bad: 0, never refused (a 0.12 lane
 * plays 0.12's template). */
static void load_variant(seq_lane_t *ln, const char *v, int n) {
    ln->gen.variant = v && n == 1 && v[0] >= '0' && v[0] <= '2' ? v[0] - '0' : 0;
}

/* A lane's steps, recipe, locks, variant and absolute pitches */
/* 0.16 `<n>.bv=`, as out_bar_var writes it: 32 hex digits; anything else is ignored (every bar the phrase's) */
static void load_bar_var(seq_lane_t *ln, const char *v, int n) {
    unsigned char x[SEQ_MAX_BARS];
    if (!v || n != 2 * SEQ_MAX_BARS || ln->gen.style == GEN_MANUAL) return;
    for (int b = 0; b < SEQ_MAX_BARS; b++) {
        int hi = hexv(v[2 * b]), lo = hexv(v[2 * b + 1]);
        if (hi < 0 || lo < 0) return;
        x[b] = (unsigned char)(hi * 16 + lo);
    }
    memcpy(ln->bar_var, x, sizeof x);
}

static int load_lane_fields(seq_part_t *p, int l, const fields_t *f) {
    seq_lane_t *ln = &p->lane[l];
    int g;
    if (!load_lane(ln, f->v[5 + l], f->n[5 + l], p->bars)) return 0;
    load_abs(ln, f->v[F_AB + l], f->n[F_AB + l], p->bars * SEQ_STEPS);
    g = load_gen(ln, l, f->v[F_G + l], f->n[F_G + l]);
    load_variant(ln, f->v[F_CV + l], f->n[F_CV + l]);
    load_bar_var(ln, f->v[F_BV + l], f->n[F_BV + l]);
    return g == 2 || (g && load_locks(ln, f->v[F_LK + l], f->n[F_LK + l], p->bars * SEQ_STEPS));
}

/* `.pq=`: <src><colour> per slot, one per slot of prog_len. Bad or absent: every slot as it was before 0.9 (the field is
 * ignored, never refused: the part's prog is a full 0.8 part). */
static void load_quality(seq_part_t *p, const char *v, int n) {
    if (!v || n != 3 * p->prog_len - 1) return;
    for (int i = 0; i < p->prog_len; i++) {
        int b = v[3 * i] - '0', c = v[3 * i + 1] - '0';
        if (b < 0 || b > THEORY_NBORROW || c < 0 || c >= COL_COUNT || (i + 1 < p->prog_len && v[3 * i + 2] != ',')) {
            memset(p->src, 0, sizeof p->src);
            memset(p->colour, 0, sizeof p->colour);
            return;
        }
        p->src[i] = (unsigned char)b;
        p->colour[i] = (unsigned char)c;
    }
}

/* `.sl=`: 1-4 hex digits; locks past prog_len are dropped. Bad: no locks. */
static void load_slot_locks(seq_part_t *p, const char *v, int n) {
    int x = 0;
    if (!v || n < 1 || n > 4) return;
    for (int i = 0; i < n; i++) {
        if (hexv(v[i]) < 0) return;
        x = x * 16 + hexv(v[i]);
    }
    p->slot_lock = (uint64_t)(x & ((1 << p->prog_len) - 1));
}

/* 0.14 the loop's slots, once the part's length, chord length and legacy phrase are read. `.ch` (4 characters a slot, as
 * many as the loop has, every one valid) gives every slot; else the phrase (`.prog`, `.pq`, `.sl`: prog_len slots)
 * unrolls across the loop, a lock to every repeat of its slot, and past it when longer (hidden slots, as 0.13 kept a
 * progression longer than its loop). Never refused: a bad `.ch` is ignored. */
static void load_slots(seq_part_t *p, const char *v, int n) {
    int len = p->prog_len, slots = part_slots(p), to = len > slots ? len : slots, ok = v && n == 4 * slots;
    for (int i = 0; ok && i < n; i += 4)
        ok = v[i] >= '0' && v[i] <= '6' && v[i + 1] >= '0' && v[i + 1] <= '0' + THEORY_NBORROW && v[i + 2] >= '0' &&
             v[i + 2] < '0' + COL_COUNT && (v[i + 3] == '0' || v[i + 3] == '1');
    p->phrase = len;
    if (p->rank_len < 1 || p->rank_len > SEQ_LEGACY_PROG) p->rank_len = len;
    if (ok) {
        p->slot_lock = 0;
        for (int i = 0; i < slots; i++) {
            p->prog[i] = v[4 * i] - '0';
            p->src[i] = (unsigned char)(v[4 * i + 1] - '0');
            p->colour[i] = (unsigned char)(v[4 * i + 2] - '0');
            p->slot_lock |= (uint64_t)(v[4 * i + 3] - '0') << i;
        }
        to = slots;
    } else {
        for (int i = len; i < to; i++) copy_slot(p, i, i % len);
    }
    p->filled_slots = to;
    settle_slots(p);
}

/* `count` comma-separated integers of 1-3 digits, a leading - allowed, filling all n characters (none after the last), into x;
 * else 0. NULL v: 0 */
static int signed_list(const char *v, int n, int *x, int count) {
    int k = 0, i = 0;
    if (!v) return 0;
    while (k < count && i < n) {
        int neg = v[i] == '-', d = 0, val = 0;
        i += neg;
        for (; i < n && v[i] >= '0' && v[i] <= '9' && d < 3; i++, d++) val = val * 10 + v[i] - '0';
        if (!d || (i < n && v[i++] != ',')) return 0;
        x[k++] = neg ? -val : val;
    }
    return k == count && i == n && v[n - 1] != ',';
}

/* A lane's SHAPE row: `.cs=` (Chords) or `2.bs=` (Bass), its LSHAPE_N comma-separated values in range (lshape_range).
 * Bad: the row stays neutral. */
static void load_shape(seq_part_t *p, int lane, const char *v, int n) {
    int x[LSHAPE_COUNT], lo, hi;
    if (!signed_list(v, n, x, LSHAPE_N[lane])) return;
    for (int k = 0; k < LSHAPE_N[lane]; k++) {
        lshape_range(lane, k, &lo, &hi);
        if (x[k] < lo || x[k] > hi) return;
    }
    memcpy(p->lane_shape[lane], x, sizeof x[0] * (size_t)LSHAPE_N[lane]);
}

/* 0.17 `.vx=`: style (-1..51), mode (VMODE_*), colour level -1..4, inversion -2..3, spread -1..3, strum direction 0..1, six
 * comma-separated values in range. Absent or bad: ignored whole, the part keeps `.voi`'s mode and the knobs at 0, never refused. */
static void load_vx(seq_part_t *p, const char *v, int n) {
    static const int LO[6] = {-1, 0, -1, -2, -1, 0}, HI[6] = {51, VMODE_COUNT - 1, 4, 3, 3, 1};
    int x[6];
    if (!signed_list(v, n, x, 6)) return;
    for (int k = 0; k < 6; k++)
        if (x[k] < LO[k] || x[k] > HI[k]) return;
    p->vstyle = x[0];
    set_vmode(p, x[1]);
    p->vcol = x[2], p->vinv = x[3], p->vspr = x[4], p->vstrdir = x[5];
}

/* 0.18 one `.rb` entry (out_rbars) into r: `L`, an index 0..RHYTHM_COUNT-1 (0.22: or a JAM index RJAM_BASE+), 16 codes, in that order, the index or the codes
 * there. Anything else leaves r as it is. 0.19: the index in library lib (RLIB_*) */
static void load_rbar(rbar_t *r, int lib, const char *e, int n) {
    int i = n > 0 && e[0] == 'L', lock = i, from = -1, ed = 0;
    char s16[17];
    unsigned char c[16];
    for (int d = 0; d < 2 && i < n && e[i] >= '0' && e[i] <= '9'; d++, i++) from = (from < 0 ? 0 : from * 10) + e[i] - '0';
    if ((from >= RHYTHM_COUNT && (from < RJAM_BASE || from >= RJAM_END)) || (i == n && from < 0)) return;
    if (from >= RJAM_BASE && !rhythm_is_jam(from)) {   /* 0.22 an unknown JAM index: the saved codes if there, else a rest bar */
        memset(c, RC_REST, 16);
        if (i < n) {
            if (n - i != 16) return;
            memcpy(s16, e + i, 16);
            s16[16] = 0;
            if (!rhythm_parse(s16, c)) return;
        }
        from = -1;
        ed = 1;   /* from < 0 implies ed */
    } else if (i == n) {
        rhythm_lib_codes(lib, from, c);
    } else {
        if (n - i != 16) return;
        memcpy(s16, e + i, 16);
        s16[16] = 0;
        if (!rhythm_parse(s16, c)) return;
    }
    memcpy(r->c, c, 16);
    r->from = (signed char)from;
    r->ed = i < n || ed;
    r->lock = (unsigned char)lock;
}

/* 0.18 `.rb=`: a DRAWN Chords lane's bars from bar 1, at most SEQ_MAX_BARS entries (the rest ignored). A bad entry leaves its
 * bar as parse_part set it (Pulse); never refused. 0.19: a DRAWN Bass lane's `2.rb=` the same way (a bad entry: Root). */
static void load_rbars(seq_part_t *p, int lane, const char *v, int n) {
    int d = dl_of(lane);
    if (!v) return;
    const char *end = v + n;
    for (int b = 0; b < SEQ_MAX_BARS && v <= end; b++) {
        const char *x = memchr(v, ',', (size_t)(end - v));
        if (!x) x = end;
        load_rbar(&p->rb[d][b], DLANE[d].rlib, v, (int)(x - v));
        v = x + 1;
    }
}

/* 0.18 `.rz=`: 4 comma-separated integers (1-3 digits, a leading - allowed), each clamped to its knob's range (SYNC -2..3,
 * the others 0..4). Absent or malformed: ignored (the knobs 0), never refused. 0.19: a DRAWN Bass lane's `2.rz=` too. 0.24: 6 (CADENCE and
 * BREATH after them, 0..3); 4 load with those 0 (a 0.18-0.23 project's) */
static void load_rz(seq_part_t *p, int lane, const char *v, int n) {
    int x[RK_COUNT] = {0};
    if (!signed_list(v, n, x, RK_COUNT) && !signed_list(v, n, x, 4)) return;
    for (int k = 0; k < RK_COUNT; k++) p->rk[dl_of(lane)][k] = (signed char)rk_clamp(k, x[k]);
}

/* 0.22 `1.dj=` / `2.dj=`, as out_djam writes it: 6 comma-separated integers c,l,sw,var,swing,tm into drawn lane d's JAM state, clamped
 * (sw 0..3, var 0..3, swing 0..5, tm -1..1); a block outside the table reads none. Another count or a bad character: ignored whole (a
 * new instance's state); never refused */
static void load_dj(seq_part_t *p, int d, const char *v, int n) {
    int x[6], ok;
    djam_t *j = &p->djam[d];
    if (!signed_list(v, n, x, 6)) return;
    ok = x[0] >= 0 && x[0] < JAM_COLS && x[1] >= 0 && x[1] < JAM_LEVELS;
    j->c = (signed char)(ok ? x[0] : -1), j->l = (signed char)(ok ? x[1] : 0);
    j->sw = (signed char)clampi(x[2], 0, 3), j->var = (signed char)clampi(x[3], 0, 3);
    j->swing = (signed char)clampi(x[4], 0, 5), j->tm = (signed char)clampi(x[5], -1, 1);
}

/* 0.24 `1.la=` / `2.la=`, as out_la writes it: 2 comma-separated integers lw,an into drawn lane d (LW_OWN..LW_GAPS, AN_OFF..AN_BEAT). Another
 * count, a bad character or a value out of range: ignored whole (OWN, OFF); never refused */
static void load_la(seq_part_t *p, int d, const char *v, int n) {
    int x[2];
    if (!signed_list(v, n, x, 2) || x[0] < LW_OWN || x[0] > LW_GAPS || x[1] < AN_OFF || x[1] > AN_BEAT) return;
    p->lw[d] = (signed char)x[0], p->an[d] = (signed char)x[1];
}

/* 0.19 note settings from x (style, pedal, octave, mov, octs, app, reg), each clamped to its range (style -1..BASS_STYLE_COUNT-1,
 * pedal 0..2, octave -1..1, mov, octs and app 0..4, reg -2..2): the note model checks none */
static bass_set_t bset_clamped(const int x[7]) {
    bass_set_t b = {(signed char)clampi(x[0], -1, BASS_STYLE_COUNT - 1), (signed char)clampi(x[1], 0, 2), (signed char)clampi(x[2], -1, 1), {0}};
    for (int k = 0; k < BK_COUNT; k++) b.k[k] = (signed char)bk_clamp(k, x[3 + k]);   /* style, pedal, octave, then knobs 2-5 */
    return b;
}

/* 0.19 `2.bx=`, as out_bass writes it: 7 comma-separated integers, clamped. Absent or malformed: Pop Root (parse_part's). */
static void load_bx(seq_part_t *p, const char *v, int n) {
    int x[7];
    if (signed_list(v, n, x, 7)) p->bass.set = bset_clamped(x);
}

/* 0.19 one `2.bl` entry, bar:style.pedal.octave.mov.octs.app.reg (out_bass), on a bar of the loop: that bar note-locked with those
 * settings, clamped. Anything else leaves the bar unlocked. */
static void load_bl_entry(seq_part_t *p, const char *e, int n) {
    const char *colon = memchr(e, ':', (size_t)n);
    char d[40];
    int x[7], at = colon ? (int)(colon - e) : -1, bar = at > 0 ? num(e, at) : -1, m = n - at - 1;
    if (bar < 0 || bar >= p->bars || m < 1 || m >= (int)sizeof d || memchr(colon + 1, ',', (size_t)m)) return;
    for (int i = 0; i < m; i++) d[i] = colon[1 + i] == '.' ? ',' : colon[1 + i];
    if (!signed_list(d, m, x, 7)) return;
    p->bass.frozen[bar] = bset_clamped(x);
    p->bass.nlock |= (unsigned short)(1u << bar);
}

/* 0.19 `2.bl=`: `;`-separated entries (load_bl_entry); a bad one is skipped, never refused */
static void load_bl(seq_part_t *p, const char *v, int n) {
    if (!v) return;
    for (const char *end = v + n; v <= end;) {
        const char *x = memchr(v, ';', (size_t)(end - v));
        if (!x) x = end;
        load_bl_entry(p, v, (int)(x - v));
        v = x + 1;
    }
}

/* 0.19 a DRAWN Bass lane's `2.ab=`: <step hex2><note hex2> pairs, each a fixed note 1..127 on a step of the loop. A bad pair is
 * skipped (that step has none), a trailing part ignored; never refused. */
static void load_bfix(seq_part_t *p, const char *v, int n) {
    for (int i = 0; v && i + 4 <= n; i += 4) {
        int st = hex2(v + i), note = hex2(v + i + 2);
        if (st >= 0 && st < p->bars * SEQ_STEPS && note >= 1 && note <= 127) p->bass.fix[st] = (unsigned char)note;
    }
}

/* 0.19 a DRAWN Bass lane's fields (its DLANE load_extra): `2.bx`, `2.bl` and its fixed notes in `2.ab` */
static void load_bass(seq_part_t *p, const fields_t *f) {
    load_bx(p, f->v[F_BX], f->n[F_BX]);
    load_bl(p, f->v[F_BL], f->n[F_BL]);
    load_bfix(p, f->v[F_AB + LANE_BASS], f->n[F_AB + LANE_BASS]);
}

/* `3.mr=`, as out_mel writes it: 15 values in range. On a Melody 1 loaded as GEN_MEL_HOOK (what a 0.11 save writes in
 * .g) it makes the lane GEN_MEL_RECIPE with that recipe, its phrase as phrasing (mel_phrase_from_legacy: the same notes);
 * absent, bad or on any other style: ignored, never refused. */
static void load_mel(seq_part_t *p, const char *v, int n) {
    int x[15];
    mel_recipe_t r;
    if (!v || p->lane[LANE_MEL_UP].gen.style != GEN_MEL_HOOK || !num_list(v, n, x, 15)) return;
    if (x[0] >= MEL_NTYPES || x[4] >= MEL_NPHRASES || x[12] > 1 || x[13] > 1 || x[14] > 15) return;
    for (int w = 0; w < 3; w++)
        if (x[1 + w] >= mel_nopts(x[0], w)) return;
    for (int k = 0; k < MK_COUNT; k++)
        if (x[5 + k] > 100) return;
    memset(&r, 0, sizeof r);
    r.type = (unsigned char)x[0];
    mel_phrase_from_legacy(&r, x[4]);
    for (int w = 0; w < 3; w++) r.opt[w] = (unsigned char)x[1 + w];
    for (int k = 0; k < MK_COUNT; k++) r.kn[k] = (unsigned char)x[5 + k];
    r.rev = (unsigned char)x[12], r.inv = (unsigned char)x[13], r.shift = (unsigned char)x[14];
    p->mel = r;
    p->lane[LANE_MEL_UP].gen.style = GEN_MEL_RECIPE;
}

/* VST-12 `3.mp=`, as out_mel writes it: the phrasing of a recipe loaded from `.mr`, 4 values in range; else ignored */
static void load_mp(seq_part_t *p, const char *v, int n) {
    int x[MP_COUNT];
    if (!v || p->lane[LANE_MEL_UP].gen.style != GEN_MEL_RECIPE || !num_list(v, n, x, MP_COUNT)) return;
    for (int i = 0; i < MP_COUNT; i++)
        if (x[i] >= mel_phrase_n(i)) return;
    for (int i = 0; i < MP_COUNT; i++) p->mel.ph[i] = (unsigned char)x[i];
}

static int b36v(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : -1; }

/* 0.20 a JAM set as out_mset writes it: 12 base-36 characters, each value clamped to its range; else 0 and out as it was */
static int load_mset(const char *v, int n, mjam_set_t *out) {
    mjam_set_t m = mjam_default();
    signed char *f[MSET_N];
    if (!v || n != MSET_N) return 0;
    mset_fields(&m, f);
    for (int k = 0; k < MSET_N; k++) {
        int x = b36v(v[k]);
        if (x < 0) return 0;
        *f[k] = (signed char)clampi(x + MSET_LO[k], MSET_LO[k], MSET_HI[k]);
    }
    *out = m;
    return 1;
}

/* 0.20 `3.jl=`: `;`-separated entries bar:<12 characters> (out_mjam), each on a bar of the loop: that bar locked with those settings.
 * A bad entry leaves its bar unlocked; never refused. */
static void load_jl(seq_part_t *p, const char *v, int n) {
    if (!v) return;
    for (const char *end = v + n; v <= end;) {
        const char *x = memchr(v, ';', (size_t)(end - v)), *colon;
        if (!x) x = end;
        colon = memchr(v, ':', (size_t)(x - v));
        int at = colon ? (int)(colon - v) : -1, bar = at > 0 ? num(v, at) : -1;
        if (bar >= 0 && bar < p->bars && load_mset(colon + 1, (int)(x - colon - 1), &p->mjam.frozen[bar])) p->mjam.nlock |= (unsigned short)(1u << bar);
        v = x + 1;
    }
}

/* 0.21 a set's DENSITY, RANGE, VARIATION, TIME from 4 characters (mnew_chars), each clamped; 3 (the first 0.21.0 build): TIME NORMAL;
 * else 0 and out as it was */
static int load_mnew(const char *v, int n, mjam_set_t *out) {
    int x[4] = {0, 0, 0, 1};
    if (!v || (n != 3 && n != 4)) return 0;
    for (int k = 0; k < n; k++)
        if ((x[k] = b36v(v[k])) < 0) return 0;
    out->dens = (signed char)clampi(x[0] - 2, -2, 2), out->rng = (signed char)clampi(x[1] - 1, -1, 2), out->var = (signed char)clampi(x[2], 0, 3);
    out->tm = (signed char)clampi(x[3] - 1, -1, 1);
    return 1;
}

/* 0.21 `3.jm=`: `;`-separated bar:<4 or 3 characters> (out_mnew), each on a bar locked by `3.jl`. A bad entry is skipped; never refused. */
static void load_jm(seq_part_t *p, const char *v, int n) {
    if (!v) return;
    for (const char *end = v + n; v <= end;) {
        const char *x = memchr(v, ';', (size_t)(end - v)), *colon;
        if (!x) x = end;
        colon = memchr(v, ':', (size_t)(x - v));
        int at = colon ? (int)(colon - v) : -1, bar = at > 0 ? num(v, at) : -1;
        if (bar >= 0 && bar < p->bars && p->mjam.nlock >> bar & 1) load_mnew(colon + 1, (int)(x - colon - 1), &p->mjam.frozen[bar]);
        v = x + 1;
    }
}

/* 0.20 `3.jf=`: <step hex2><value hex2> pairs, each a fix 1..129 (MJ_FIX_REST, MJ_FIX_HOLD) on a step of the loop. A bad pair is
 * skipped, a trailing part ignored; never refused. */
static void load_jf(seq_part_t *p, const char *v, int n) {
    for (int i = 0; v && i + 4 <= n; i += 4) {
        int st = hex2(v + i), x = hex2(v + i + 2);
        if (st >= 0 && st < p->bars * SEQ_STEPS && x >= 1 && x <= MJ_FIX_HOLD) p->mjam.fix[st] = (unsigned char)x;
    }
}

/* 0.20 a JAM Melody lane's fields (out_mjam), over mjam_init's state and GATE 50. Absent or bad: that field's default; values clamped */
static void load_mjam(seq_part_t *p, const fields_t *f) {
    mjam_state_t *m = &p->mjam;
    const char *w = f->v[F_JW], *v = f->v[F_JV];
    int k[3];
    p->mel.kn[MK_GATE] = 50;
    load_mset(f->v[F_JX], f->n[F_JX], &m->set);
    if (w && f->n[F_JW] == 1 && w[0] >= '0' && w[0] <= '9') m->sw = (unsigned char)clampi(w[0] - '0', 0, 3);
    if (signed_list(f->v[F_JK], f->n[F_JK], k, 3)) {
        m->push = (unsigned char)clampi(k[0], 0, 100);
        p->mel.kn[MK_GATE] = (unsigned char)clampi(k[1], 0, 100);
        m->dyn = (unsigned char)clampi(k[2], 0, 100);
    }
    load_jl(p, f->v[F_JL], f->n[F_JL]);
    load_jf(p, f->v[F_JF], f->n[F_JF]);
    load_mnew(f->v[F_JY], f->n[F_JY], &m->set);   /* 0.21 */
    /* 0.21 Amendment 1: ACCENT stays LIGHT (3.jv's second character is checked, not loaded) */
    if (v && f->n[F_JV] == 2 && b36v(v[0]) >= 0 && b36v(v[1]) >= 0) m->swing = (unsigned char)clampi(b36v(v[0]), 0, 5);
    load_jm(p, f->v[F_JM], f->n[F_JM]);
}

/* 0.24 `.cp=` (out_cp): each entry of 5 characters, in any order; a bad one, or one past a full bar, is skipped (cp_put sorts them;
 * revoice then drops a step-0 point of a bar that starts a slot); never refused */
static void load_cp(seq_part_t *p, const char *v, int n) {
    if (!v) return;
    for (const char *end = v + n; v < end;) {
        const char *x = memchr(v, ',', (size_t)(end - v));
        if (!x) x = end;
        if (x - v == 5) {
            int b = hexv(v[0]), st = hexv(v[1]), d = v[2] - '0', sr = v[3] - '0', t = v[4] - '0';
            if (b >= 0 && st >= 0 && d >= 0 && d <= 6 && sr >= 0 && sr <= THEORY_NBORROW && t >= 0 && t < CP_TYPES) cp_put(p, b, st, d, sr, t);
        }
        v = x + 1;
    }
}

/* 0.24 `.ho=` (out_hoct): each entry a bar up to filled in hex (final review: the bars out_hoct saves; parse_part sets filled to the
 * loop's bars, and the bars past it repeat the loop, chords_repeat_bar) and one HO_CH character or more; a bad one (a bad bar, no
 * octave, a character not in HO_CH) is skipped whole; never refused */
static void load_hoct(seq_part_t *p, const char *v, int n) {
    if (!v) return;
    for (const char *end = v + n; v < end;) {
        const char *x = memchr(v, ',', (size_t)(end - v));
        int b, ok;
        if (!x) x = end;
        b = hexv(v[0]), ok = b >= 0 && b < p->filled && x - v >= 2;
        for (const char *q = v + 1; ok && q < x; q++) ok = *q && strchr(HO_CH, *q) != NULL;
        for (const char *q = v + 1; ok && q < x; q++) {
            int k = (int)(strchr(HO_CH, *q) - HO_CH);
            p->hoct[b][k % 16] = (signed char)(k / 16 - (k < 32 ? 2 : 1));
        }
        v = x + 1;
    }
}

/* A part's fields (letter A-F), into p; 0 if one is missing or bad (0.9's optional fields are never refused) */
static int parse_part(const fields_t *f, seq_part_t *p) {
    for (int i = 2; i < F_REQUIRED; i++)
        if (!f->v[i]) return 0;
    memset(p, 0, sizeof *p);
    cp_init(p);   /* 0.24: `.cp` (load_cp, below) adds the points */
    p->vstyle = -1;
    p->bars = num(f->v[2], f->n[2]);
    p->chord_len = num(f->v[3], f->n[3]);
    if (!load_prog(p, f->v[4], f->n[4])) return 0;
    if (p->bars != 1 && p->bars != 2 && p->bars != 4 && p->bars != 8 && p->bars != 16) return 0;
    p->filled = p->bars;
    memcpy(p->lane_shape, LSHAPE_NEUTRAL, sizeof LSHAPE_NEUTRAL);
    mel_default(&p->mel);
    load_quality(p, f->v[F_PQ], f->n[F_PQ]);
    load_slot_locks(p, f->v[F_SL], f->n[F_SL]);
    load_shape(p, LANE_CHORDS, f->v[F_CS], f->n[F_CS]);
    load_shape(p, LANE_BASS, f->v[F_BS], f->n[F_BS]);
    if (!load_voicing(p, f->v[F_VOI], f->n[F_VOI]) || !load_harmony(p, f->v[F_H], f->n[F_H])) return 0;
    load_vx(p, f->v[F_VX], f->n[F_VX]);
    if (p->chord_len != 4 && p->chord_len != 8 && p->chord_len != 16 && p->chord_len != 32) return 0;
    p->rank_len = f->v[F_RK] ? num(f->v[F_RK], f->n[F_RK]) : 0;   /* bad or absent: the phrase's (settle_slots) */
    load_slots(p, f->v[F_SCH], f->n[F_SCH]);
    load_cp(p, f->v[F_CP], f->n[F_CP]);   /* 0.24 */
    load_hoct(p, f->v[F_HO], f->n[F_HO]);
    for (int l = 0; l < SEQ_LANES; l++)
        if (!load_lane_fields(p, l, f)) return 0;
    for (int d = 0; d < DL_COUNT; d++)   /* 0.18: a DRAWN lane's bars without an `.rb` entry (0.19: the Bass's, a new instance's) */
        rbars_init(p, DLANE[d].lane, DLANE[d].rload);
    p->bass.set = bass_style_set(BASS_POP_ROOT);
    mjam_init(&p->mjam);
    djam_init(p);
    for (int d = 0; d < DL_COUNT; d++) {   /* settle_loaded converts a lane of any other style */
        const struct dlane *dl = &DLANE[d];
        if (!lane_drawn(p, dl->lane)) continue;
        load_rbars(p, dl->lane, f->v[dl->frb], f->n[dl->frb]);
        load_rz(p, dl->lane, f->v[dl->frz], f->n[dl->frz]);
        if (dl->load_extra) dl->load_extra(p, f);
        load_dj(p, d, f->v[F_DJ + d], f->n[F_DJ + d]);   /* 0.22 */
        load_la(p, d, f->v[F_LA + d], f->n[F_LA + d]);   /* 0.24 */
    }
    if (p->lw[DL_CHORDS] && p->lw[DL_BASS]) p->lw[DL_BASS] = LW_OWN;   /* 0.24 never both following each other (a set's rule) */
    load_mel(p, f->v[F_MR], f->n[F_MR]);
    load_mp(p, f->v[F_MP], f->n[F_MP]);
    if (mel_jam(p)) load_mjam(p, f);   /* any other Melody lane loads as 0.19 loads it */
    return 1;
}

/* The project's fields and part A */
static int parse_v2(const char *str, int *key, int *scale, int *ch, int *out, seq_part_t *p) {
    int gn;
    fields_t f;
    find_fields(str, 'A', &f);
    if (!f.v[0] || !f.v[1]) return 0;
    if (f.v[F_GV] && (f.n[F_GV] != 1 || f.v[F_GV][0] != '1')) return 0;   /* the generator is gen_v1: gv=1, or absent */
    *key = num(f.v[0], f.n[0]);
    *scale = num(f.v[1], f.n[1]);
    if (*key < 0 || *key > 11 || *scale < 0 || *scale >= SCALE_COUNT) return 0;
    if (!load_channels(ch, f.v[F_CH], f.n[F_CH])) return 0;
    if (!load_out(&out[0], f.v[F_SW], f.n[F_SW], 0) || !load_out(&out[1], f.v[F_GT], f.n[F_GT], 50) ||
        !load_out(&out[2], f.v[F_DY], f.n[F_DY], 50))
        return 0;
    out[3] = !(f.v[F_FO] && f.n[F_FO] == 1 && f.v[F_FO][0] == '0');   /* FOLLOW: off only for `fo=0`; never refuses a load */
    gn = f.v[F_GN] ? num(f.v[F_GN], f.n[F_GN]) : 0;
    out[4] = gn > 0 && gn <= SEQ_PRESETS_MAX ? gn : 0;   /* 0.23 the preset: absent or bad is none; never refuses a load */
    out[5] = f.v[F_PM] && f.n[F_PM] == 1 && f.v[F_PM][0] == '1';   /* 0.26 per-part mutes: only `pm=1`; never refuses a load */
    return parse_part(&f, p);
}

/* A loaded state starts as the Verse alone: the Verse plays (from the song position, as at a start), nothing queued, the
 * rest unused until a csq2 load finds them */
static void only_verse(seq_t *s) {
    s->cur_part = 0;
    s->queued = -1;
    s->mjq_c = -1;   /* 0.20: a queued JAM block too */
    djq_drop_all(s);   /* 0.22: and the drawn lanes' */
    s->used_mask = 1;
    s->stale_mask = 0;
    s->part_origin_k = 0;
    s->unsaved_mask = 0;
}

static int load_v1(seq_t *s, const char *str) {
    const int grid = SEQ_LANES * SEQ_STEPS;
    unsigned char pat[SEQ_LANES][SEQ_STEPS];
    if ((int)strlen(str) != 5 + grid + 1 + SEQ_LANES || str[5 + grid] != ':') return 0;
    for (int i = 0; i < grid + 1 + SEQ_LANES; i++)
        if (i != grid && str[5 + i] != '0' && str[5 + i] != '1') return 0;
    for (int i = 0; i < grid; i++) pat[i / SEQ_STEPS][i % SEQ_STEPS] = str[5 + i] == '1';
    txn_cancel(s);   /* a valid state: an open picker or browser is the old project's */
    paging_off(s);   /* a pause is not saved (ruling 18) */
    release_all(s);
    memset(&s->keys, 0, sizeof s->keys);
    only_verse(s);
    v1_part(cur(s), pat);
    retire(cur(s));
    for (int l = 0; l < SEQ_LANES; l++) cur(s)->lane[l].mute = l < SEQ_ACTIVE_LANES && str[6 + grid + l] == '1';
    s->key = 4;
    s->scale = SCALE_MINOR;
    for (int l = 0; l < SEQ_LANES; l++) s->chan[l] = l + 2;
    s->swing = 0;
    s->gate = s->dyn = 50;
    revoice(s, cur(s));
    render_drawn_lanes(s, cur(s));   /* 0.18 (R19): a v1 song's Chords steps as drawn bars (0.19: and its Bass steps) */
    if (s->view_bar >= cur(s)->bars) s->view_bar = 0;
    s->undo_n = 0;   /* the snapshots belong to the project the load replaced */
    return 1;
}

/* A loaded generated lane regenerates, then its locks overlay it: a locked on-step is its record, a locked off-step the
 * generator's step turned off, as a tap off leaves it. */
static void settle_gen(seq_t *s, seq_part_t *p, int lane) {
    seq_lane_t *ln = &p->lane[lane];
    seq_step_t st[SEQ_LANE_STEPS];
    if (ln->gen.style == GEN_MANUAL) return;
    if (jam_lane(p, lane)) {   /* 0.20: rendered from the melody model */
        render_mel(s, p);
        return;
    }
    gen_steps(s, p, lane, st);
    for (int i = 0; i < p->bars * SEQ_STEPS; i++) {
        if (locked(ln, i) && ln->steps[i].on) continue;
        ln->steps[i] = st[i];
        if (locked(ln, i)) ln->steps[i].on = ln->steps[i].abs = 0;
    }
}

static int old_rhythm(int style);

/* 0.18: a loaded 0.12-0.17 Chords lane (an old style, or MANUAL) becomes DRAWN bars that sound as it did: its steps as 0.17
 * loads them (generated under its locks), each bar up to filled read back as codes (ruling R2: velocity classes, holds cut
 * at the next hit and the bar end). An old style's bars are from its library rhythm, edited where they differ from it (the
 * RHYTHM field names the style, and style_1 reads the style the host saved, so its restore after the chunk changes
 * nothing); a MANUAL lane's bar is the library rhythm it equals, else drawn from nothing. COMPLEXITY 50 plays them as
 * drawn; no bar or step locks; the RHYTHM knobs 0. */
static void convert_chords(seq_t *s, seq_part_t *p) {
    seq_lane_t *ln = &p->lane[LANE_CHORDS];
    int was = old_rhythm(ln->gen.style);
    unsigned char lib[16];
    if (was >= 0) rhythm_codes(was, lib);
    settle_gen(s, p, LANE_CHORDS);
    for (int b = 0; b < p->filled; b++) {
        rbar_t *r = &p->rb[DL_CHORDS][b];
        rhythm_from_steps(&ln->steps[b * SEQ_STEPS], r->c);
        r->from = (signed char)(was >= 0 ? was : rhythm_find(r->c));
        r->ed = was >= 0 ? memcmp(r->c, lib, 16) != 0 : r->from < 0;
        r->lock = 0;
    }
    ln->gen = (seq_gen_t){GEN_CH_DRAWN, ln->gen.seed, CHORDS_CX_NEUTRAL, 0};
    memset(p->rk[DL_CHORDS], 0, sizeof p->rk[DL_CHORDS]);
    memset(ln->lock, 0, sizeof ln->lock);
    memset(ln->bar_var, 0, sizeof ln->bar_var);
}

/* 0.19 an old SHAPE value (0..100) at the nearest of a knob's 5 steps (0/25/50/75/100: 0..4) */
static int shape_step(int v) { return (v * 4 + 50) / 100; }

/* 0.19 the note settings an old Bass lane of style `style` with SHAPE row sh loads with: the nearest style (Root and MANUAL Pop
 * Root, Pump Octave Bounce, Walk Upright Walk), whose MOVEMENT, OCTAVES and APPROACH a generated lane's old row raises to its own
 * (MOVEMENT and OCTAVES at their nearest step, APPROACH Chromatic Half below: those only ever added to the old generator's line; they
 * never changed a MANUAL lane's notes), REGISTER at its nearest step (-2..2, 50 Natural), OCT the octave */
static bass_set_t old_bass_set(int style, const int *sh) {
    bass_set_t b = bass_style_set(style == GEN_BASS_PUMP ? BASS_OCTAVE_BOUNCE : style == GEN_BASS_WALK ? BASS_UPRIGHT_WALK : BASS_POP_ROOT);
    int up[3] = {shape_step(sh[BSHAPE_MOVEMENT]), shape_step(sh[BSHAPE_OCTAVES]), sh[BSHAPE_APPROACH] ? 2 : 0};
    for (int k = BK_MOV; style != GEN_MANUAL && k <= BK_APP; k++)
        if (b.k[k] < up[k]) b.k[k] = (signed char)up[k];
    b.k[BK_REG] = (signed char)(shape_step(sh[BSHAPE_REGISTER]) - 2);
    b.octave = (signed char)sh[BSHAPE_OCT];
    return b;
}

/* 0.19: a loaded 0.18-or-earlier Bass lane (Root, Pump, Walk or MANUAL) becomes DRAWN bars that keep its rhythm exactly: its steps as
 * 0.18 loads them (generated under its locks), each bar up to filled read back as codes (as convert_chords), from the Bass library
 * rhythm they equal, else edited. Its notes come from the note model (old_bass_set); SYNC is dropped, and the SHAPE values the
 * settings took leave the row (PUSH, GATE and ACCENT, now DYNAMICS, stay). A FIX pick (a locked step's note; any note on a MANUAL
 * lane) becomes a fixed note, at the octave the old OCT played it (a fixed note is absolute); `.bv` is kept. COMPLEXITY 50, no bar,
 * note or step locks, the RHYTHM knobs 0. */
static void convert_bass(seq_t *s, seq_part_t *p) {
    static const int GONE[6] = {BSHAPE_MOVEMENT, BSHAPE_SYNC, BSHAPE_OCTAVES, BSHAPE_REGISTER, BSHAPE_OCT, BSHAPE_APPROACH};
    seq_lane_t *ln = &p->lane[LANE_BASS];
    int *sh = p->lane_shape[LANE_BASS], gen = ln->gen.style != GEN_MANUAL;
    settle_gen(s, p, LANE_BASS);
    for (int b = 0; b < p->filled; b++) {
        rbar_t *r = &p->rb[DL_BASS][b];
        rhythm_from_steps(&ln->steps[b * SEQ_STEPS], r->c);
        r->from = (signed char)rhythm_lib_find(RLIB_BASS, r->c);
        r->ed = r->from < 0;
        r->lock = 0;
    }
    for (int i = 0; i < p->filled * SEQ_STEPS; i++) {
        int n = ln->steps[i].abs + 12 * sh[BSHAPE_OCT];
        if (ln->steps[i].on && ln->steps[i].abs && (!gen || locked(ln, i))) p->bass.fix[i] = (unsigned char)(n < 1 ? 1 : n > 127 ? 127 : n);
    }
    p->bass.set = old_bass_set(ln->gen.style, sh);
    p->bass.nlock = 0;
    for (int k = 0; k < 6; k++) sh[GONE[k]] = LSHAPE_NEUTRAL[LANE_BASS][GONE[k]];
    ln->gen = (seq_gen_t){GEN_BASS_DRAWN, ln->gen.seed, BASS_CX_NEUTRAL, 0};
    memset(p->rk[DL_BASS], 0, sizeof p->rk[DL_BASS]);
    memset(ln->lock, 0, sizeof ln->lock);
}

/* 0.19.1 the drawn lanes (struct dlane, at the top): the Chords lane (0.18) and the Bass lane (0.19) */
static const struct dlane DLANE[DL_COUNT] = {
    {LANE_CHORDS, TAB_CHORDS, RLIB_CHORDS, GEN_CH_DRAWN, RHYTHM_PULSE, "", F_RB, F_RZ, 1, convert_chords, NULL, chords_repeat_bar, NULL, NULL},
    {LANE_BASS, TAB_BASS, RLIB_BASS, GEN_BASS_DRAWN, 0, "2", F_BRB, F_BRZ, 0, convert_bass, bass_notes, bass_repeat_bar, out_bass,
     load_bass}};

/* 0.18: the part's drawn lane `lane` DRAWN (converted when it is not) and rendered: what every load and v1 reset ends with */
static void drawn_now(seq_t *s, seq_part_t *p, int lane) {
    if (!lane_drawn(p, lane)) DLANE[dl_of(lane)].convert(s, p);
    render_rhythm(s, p, lane);
}

/* 0.18 the part's Chords lane drawn_now (0.19: and its Bass lane): a v1 reset or song */
static void render_drawn_lanes(seq_t *s, seq_part_t *p) {
    if (both_drawn(p)) render_pair(s, p, pair_first(p));   /* 0.24.1 none to convert: each once */
    else
        for (int d = 0; d < DL_COUNT; d++) drawn_now(s, p, DLANE[d].lane);
}

/* A loaded lane made ready: a generated one regenerated under its locks; 0.18 the Chords lane DRAWN and rendered (drawn_now);
 * 0.19 the Bass lane too */
static void settle_loaded(seq_t *s, seq_part_t *p, int lane) {
    if (dl_of(lane) >= 0) drawn_now(s, p, lane);
    else settle_gen(s, p, lane);
}

/* A loaded part made ready to play: Melody 2 retired, voiced, its generated lanes regenerated under their locks. Here, on
 * the load, so seq_block only ever switches to a part that is ready. */
static void ready_loaded(seq_t *s, seq_part_t *p) {
    retire(p);
    revoice(s, p);
    for (int l = 0; l < SEQ_LANES; l++) settle_loaded(s, p, l);
}

/* Part B-F of a csq2 line: in use if all its fields are there and good, else left unused (the load still succeeds) */
static void load_other_part(seq_t *s, const char *str, int part) {
    fields_t f;
    find_fields(str, (char)('A' + part), &f);
    if (!parse_part(&f, &s->load_tmp)) return;
    s->part[part] = s->load_tmp;
    ready_loaded(s, &s->part[part]);
    s->used_mask |= 1 << part;
}

/* song (0.23 seq_song_load): a csq2 state's song only, the MIDI channels and FOLLOW kept */
static int state_load(seq_t *s, const char *str, int song) {
    int key, scale, ch[3], out[6];   /* out: SWING, GATE, DYNAMICS, FOLLOW, 0.23 the preset, 0.26 per-part mutes */
    if (!str) return 0;
    if (!strncmp(str, "csq1:", 5)) {
        if (!load_v1(s, str)) return 0;
        act(s);
        s->follow = 1;   /* a v1 state has no `fo`: FOLLOW on */
        s->preset = 0;   /* 0.23 nor a preset */
        s->naud = 0;     /* as ALL NOTES OFF: the loaded song starts clean */
        strip_clamp(s);
        s->display_rev++;   /* MPC re-reads the loaded values */
        return 1;
    }
    if (strncmp(str, "csq2|", 5) || !parse_v2(str + 5, &key, &scale, ch, out, &s->load_tmp)) return 0;
    txn_cancel(s);
    paging_off(s);
    act(s);
    if (!song) s->follow = out[3];
    s->preset = out[4];
    s->naud = 0;     /* as ALL NOTES OFF: the loaded song starts clean */
    release_all(s);
    memset(&s->keys, 0, sizeof s->keys);
    s->key = key;
    s->scale = scale;
    if (!song) memcpy(s->chan, ch, sizeof ch);
    s->swing = out[0];
    s->gate = out[1];
    s->dyn = out[2];
    only_verse(s);
    *cur(s) = s->load_tmp;
    retire(cur(s));
    ready_loaded(s, cur(s));
    if (s->view_bar >= cur(s)->bars) s->view_bar = 0;
    for (int p = 1; p < SEQ_NPARTS; p++) load_other_part(s, str + 5, p);
    for (int p = 1; !out[5] && p < SEQ_NPARTS; p++)   /* 0.26 a state without `pm`: A's mutes were every part's */
        for (int l = 0; l < SEQ_LANES; l++) s->part[p].lane[l].mute = s->part[0].lane[l].mute;
    s->undo_n = 0;
    strip_clamp(s);
    s->display_rev++;
    return 1;
}

int seq_state_load(seq_t *s, const char *str) { return state_load(s, str, 0); }
int seq_song_load(seq_t *s, const char *str) { return !str || !strncmp(str, "csq2|", 5) ? state_load(s, str, 1) : 0; }

#define VOICING_INIT 48   /* Film - As Written (test_17_init_style checks the name) */

/* A voicing's mode, COLOUR, INVERSION, SPREAD, STRUM (-8..8) and Chords OCTAVE onto part p (the caller revoices): a style's, 0.22 a
 * JAM block's */
static void part_vox(seq_part_t *p, int mode, int col, int inv, int spr, int str, int oct) {
    set_vmode(p, mode);
    p->vcol = col, p->vinv = inv, p->vspr = spr;
    p->lane_shape[LANE_CHORDS][SHAPE_STRUM] = (int)lround(iabs(str) * 12.5);
    p->vstrdir = str < 0;
    p->lane_shape[LANE_CHORDS][SHAPE_OCT] = oct;
}

/* Style i's mode, COLOUR, INVERSION, SPREAD, STRUM and Chords OCTAVE onto part p (the caller revoices) */
static void part_style(seq_part_t *p, int i) {
    const voicing_style_t *v = &VOICING_STYLES[i];
    p->vstyle = i;
    part_vox(p, v->mode, v->col, v->inv, v->spr, v->str, v->oct);
}

/* INIT's part: v1's 4-bar part with the 1-5-6-4 Pop preset and the INIT lanes (0.20: Melody JAM, ARP · UP at level 1) */
static void init_part(seq_part_t *p, const unsigned char pat[SEQ_LANES][SEQ_STEPS]) {
    const theory_preset_t *pop = &THEORY_PRESETS[1];
    v1_part(p, pat);
    part_style(p, VOICING_INIT);   /* Film - As Written: Close, every knob 0, INIT's root-position triads */
    memcpy(p->prog, pop->deg, sizeof(int) * (size_t)pop->len);
    p->prog_len = pop->len;
    mel_defaults(MEL_ARP, &p->mel, NULL);
    p->mel.opt[0] = 2;   /* Up-down */
    p->lane[LANE_CHORDS].gen = (seq_gen_t){GEN_CH_DRAWN, 1, CHORDS_CX_NEUTRAL, 0};   /* 0.18: Sustain bars, 0.17's INIT sound */
    p->lane[LANE_BASS].gen = (seq_gen_t){GEN_BASS_DRAWN, 1, BASS_CX_NEUTRAL, 0};   /* 0.19: Root bars, Pop Root: 0.18's onsets */
    jam_default(p);
    retire(p);
}

void seq_init_preset(seq_t *s) {
    unsigned char pat[SEQ_LANES][SEQ_STEPS];
    txn_cancel(s);
    paging_off(s);
    act(s);
    s->naud = 0;
    release_all(s);
    memset(&s->keys, 0, sizeof s->keys);
    s->key = 0;
    s->scale = SCALE_MAJOR;
    s->swing = 0;
    s->gate = s->dyn = 50;
    s->preset = 0;   /* 0.23 */
    only_verse(s);
    v1_default_pat(pat);
    for (int p = 0; p < SEQ_PARTS; p++) init_part(&s->part[p], pat);
    revoice(s, cur(s));   /* the other parts are unused: their first use copies the Verse */
    regen_part(s, cur(s));
    s->view_bar = 0;
    s->undo_n = 0;
    strip_clamp(s);
    s->display_rev++;
}

/* The generator: styles, complexity, GEN, hand-edit locks, undo, and HARMONY's progressions. */

static int style_fits(int lane, int style) {
    if (lane < 0 || lane >= SEQ_ACTIVE_LANES) return 0;
    if (style == GEN_MANUAL) return 1;
    if (lane == LANE_CHORDS) return (style >= GEN_CH_SUSTAIN && style <= GEN_CH_STABS) || gen_is_chords_v2(style);
    if (lane == LANE_BASS) return style >= GEN_BASS_ROOT && style <= GEN_BASS_WALK;
    if (lane == LANE_MEL_UP && style == GEN_MEL_RECIPE) return 1;   /* 0.11: Melody 1 only */
    return style == GEN_MEL_ARP || style == (lane == LANE_MEL_UP ? GEN_MEL_HOOK : GEN_MEL_ANSWER);
}

/* A new seed from the old one and a per-instance counter, so GEN rarely repeats itself (16-bit seed); persisted only as
 * the seed */
static int next_seed(seq_t *s, int old) {
    s->gen_counter++;
    return (int)(((unsigned)old * 1103515245u + 12345u + s->gen_counter) & 0xFFFFu);
}

/* Snapshot a lane of the playing part (lane >= 0) or its progression (lane -1); the 9th push drops the oldest. join: one
 * undo step with the snapshot before it. When the ring drops a group's first snapshot the rest of it goes too: no undo is
 * ever half a group. */
static void push_snap_part(seq_t *s, int part, int lane, int join) {
    const seq_part_t *p = &s->part[part];
    seq_snap_t *u = &s->undo[s->undo_top];
    u->part = part;
    u->lane = lane;
    u->join = join;
    u->bars = p->bars;
    u->filled = p->filled;
    u->recipe = 0;
    if (lane >= 0) memcpy(&u->ln, &p->lane[lane], sizeof u->ln);
    u->mel = p->mel;
    if (lane >= 0 && lane < SEQ_ACTIVE_LANES) memcpy(u->shape, p->lane_shape[lane], sizeof u->shape);
    memcpy(u->prog, p->prog, sizeof u->prog);
    u->prog_len = p->prog_len;
    u->harmony_seed = p->harmony_seed;
    memcpy(u->src, p->src, sizeof u->src);
    memcpy(u->colour, p->colour, sizeof u->colour);
    u->slot_lock = p->slot_lock;
    memcpy(u->cp, p->cp, sizeof u->cp);   /* 0.24 */
    u->chord_len = p->chord_len;
    u->filled_slots = p->filled_slots;
    u->phrase = p->phrase;
    u->rank_len = p->rank_len;
    u->vox = 0;
    u->txn = 0;
    u->knobs = 0;
    u->bkeep = 0;
    u->jq = 0;   /* 0.22 */
    u->prog_too = 0;   /* 0.24 */
    if (dl_of(lane) >= 0) {   /* 0.19: the snapshot lane's drawn bars and knobs; 0.22 its playing JAM block */
        memcpy(u->rb, p->rb[dl_of(lane)], sizeof u->rb);
        memcpy(u->rk, p->rk[dl_of(lane)], sizeof u->rk);
        u->djc = p->djam[dl_of(lane)].c, u->djl = p->djam[dl_of(lane)].l;
        memcpy(u->lw, p->lw, sizeof u->lw), u->an = p->an[dl_of(lane)];   /* 0.24 */
    }
    if (lane == LANE_BASS) u->bass = p->bass;   /* 0.19: the Bass lane's notes */
    if (dl_of(lane) >= 0) memcpy(u->hoct, p->hoct, sizeof u->hoct);   /* 0.24 the Chords lane's hit octaves (final review, ruling 32: a Bass
                                                                          * step's too: the Chords following it lose those of hits it takes) */
    if (lane == LANE_MEL_UP) u->mjam = p->mjam;   /* 0.20: the JAM Melody's */
    s->undo_top = (s->undo_top + 1) % SEQ_UNDO;
    if (s->undo_n < SEQ_UNDO) s->undo_n++;
    s->undo_pushes++;
    while (s->undo_n && s->undo[(s->undo_top + SEQ_UNDO - s->undo_n) % SEQ_UNDO].join) s->undo_n--;
}

static void push_snap(seq_t *s, int lane, int join) { push_snap_part(s, s->cur_part, lane, join); }

static void push_undo(seq_t *s, int lane) { push_snap(s, lane, 0); }

/* A recipe lane's snapshot whose undo restores its complexity and SHAPE row too (a KEEP, a move) */
static void push_recipe_snap(seq_t *s, int part, int lane) {
    push_snap_part(s, part, lane, 0);
    s->undo[(s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO].recipe = 1;
}

/* A progression snapshot whose undo restores the voicing (style, knobs, mode, octave) too */
static void push_voicing_snap(seq_t *s) {
    const seq_part_t *p = cur(s);
    seq_snap_t *u;
    push_undo(s, -1);
    u = &s->undo[(s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO];
    u->vox = 1;
    vox_read(p, u->vx);
}

/* The generated pattern under the tap gives the step's values; the generator's off steps play the root. */
static void tap_generated(seq_t *s, seq_part_t *p, int lane, int i, int on) {
    seq_lane_t *ln = &p->lane[lane];
    seq_step_t st[SEQ_LANE_STEPS];
    if (ln->steps[i].on == on) return;   /* a host re-sending the state it has is not an edit */
    push_undo(s, lane);
    ln->lock[i >> 3] |= (unsigned char)(1u << (i & 7));
    if (on) {
        gen_at(s, p, lane, i, st);   /* the chord under the tap (a shaped Bass or a melody: the whole lane, exact) */
        ln->steps[i] = st[i].on ? st[i] : (seq_step_t){1, TONE_R, LANE_OCT[lane], VELOCITY, 1, 0};
    } else {
        ln->steps[i].on = 0;
        ln->steps[i].abs = 0;
    }
    follow(s, p, lane);
}

/* MANUAL: the step set directly (its velocity and length kept when on); generated: the generator's velocity and length */
/* 0.16 per bar: a lane that has bar variations (a generated Bass or Melody 1) and a bar of the loop */
static int bar_lane_ok(const seq_part_t *p, int lane, int bar) {
    return (lane == LANE_BASS || lane == LANE_MEL_UP) && bar >= 0 && bar < p->bars && p->lane[lane].gen.style != GEN_MANUAL;
}

static int bar_locks(const seq_lane_t *ln, int bar) {
    int n = 0;
    for (int i = bar * SEQ_STEPS; i < (bar + 1) * SEQ_STEPS; i++) n += locked(ln, i);
    return n;
}

static void bar_edit(seq_t *s, int lane, int bar, int reset) {
    seq_part_t *p = cur(s);
    if (!bar_lane_ok(p, lane, bar)) return;
    seq_lane_t *ln = &p->lane[lane];
    if (reset && !ln->bar_var[bar] && !bar_locks(ln, bar)) return;   /* already the phrase's: no edit */
    act(s);
    push_undo(s, lane);
    if (reset) {
        ln->bar_var[bar] = 0;
        memset(&ln->lock[bar * SEQ_STEPS / 8], 0, SEQ_STEPS / 8);
        regen_lane(s, p, lane);
    } else {   /* the next variation that sounds different (a sparse line can give the same notes): up to 8 tries */
        seq_step_t was[SEQ_STEPS];
        memcpy(was, &ln->steps[bar * SEQ_STEPS], sizeof was);
        for (int t = 0; t < 8; t++) {
            ln->bar_var[bar] = (unsigned char)(ln->bar_var[bar] == 255 ? 1 : ln->bar_var[bar] + 1);
            regen_lane(s, p, lane);
            if (memcmp(was, &ln->steps[bar * SEQ_STEPS], sizeof was)) break;
        }
    }
}

void seq_lane_new_bar(seq_t *s, int lane, int bar) { bar_edit(s, lane, bar, 0); }

void seq_lane_reset_bar(seq_t *s, int lane, int bar) { bar_edit(s, lane, bar, 1); }

int seq_lane_bar_var(const seq_t *s, int lane, int bar) {
    const seq_part_t *p = ccur(s);
    return lane >= 0 && lane < SEQ_LANES && bar >= 0 && bar < SEQ_MAX_BARS ? p->lane[lane].bar_var[bar] : 0;
}

int seq_lane_bar_locks(const seq_t *s, int lane, int bar) {
    return lane >= 0 && lane < SEQ_LANES && bar >= 0 && bar < SEQ_MAX_BARS ? bar_locks(&ccur(s)->lane[lane], bar) : 0;
}

void seq_set_step_note(seq_t *s, int lane, int bar, int step, int note) {
    seq_part_t *p = cur(s);
    seq_step_t st[SEQ_LANE_STEPS];
    if ((lane != LANE_BASS && lane != LANE_MEL_UP) || !step_ok(p, bar, step) || note < 0 || note > 127 || lane_drawn(p, lane) ||
        jam_lane(p, lane))
        return;   /* 0.19: a DRAWN Bass's fixed notes are its own (bass.fix); 0.20 a JAM Melody's (mjam.fix) */
    seq_lane_t *ln = &p->lane[lane];
    int i = bar * SEQ_STEPS + step, gen = ln->gen.style != GEN_MANUAL;
    seq_step_t x = ln->steps[i], g = x;
    if ((!gen || locked(ln, i)) && x.on == (note > 0) && x.abs == note) return;   /* a re-send is no edit */
    act(s);
    if (note) {
        if (gen) {
            gen_at(s, p, lane, i, st);
            g = st[i];
        }
        x.on = 1;
        x.vel = g.on ? g.vel : VELOCITY;
        x.len = g.on ? g.len : 1;
        set_abs(&x, note);
    } else {
        x.on = x.abs = 0;
    }
    push_undo(s, lane);
    ln->steps[i] = x;
    if (gen) ln->lock[i >> 3] |= (unsigned char)(1u << (i & 7));
    follow(s, p, lane);
}

void seq_toggle_step_lock(seq_t *s, int lane, int bar, int step) {
    seq_part_t *p = cur(s);
    seq_step_t st[SEQ_LANE_STEPS];
    if (lane < 0 || lane >= SEQ_ACTIVE_LANES || !step_ok(p, bar, step)) return;
    if (p->lane[lane].gen.style == GEN_MANUAL || lane_drawn(p, lane) || jam_lane(p, lane)) return;   /* 0.18: bars lock whole (0.20:
                                                                                                    * a JAM Melody's too) */
    act(s);
    seq_lane_t *ln = &p->lane[lane];
    int i = bar * SEQ_STEPS + step;
    push_undo(s, lane);
    ln->lock[i >> 3] ^= (unsigned char)(1u << (i & 7));
    if (locked(ln, i)) return;   /* locked as it is */
    gen_at(s, p, lane, i, st);   /* unlocked: the generator's step again (an absolute pitch goes, unless the walk makes it) */
    ln->steps[i] = st[i];
    follow(s, p, lane);
}

/* 0.16's nine chord rhythms in the 0.18 library (rhythm.h) */
static const struct { int style, rhythm; } OLD_RHYTHM[9] = {
    {GEN_CH_SUSTAIN, 0}, {GEN_CH_BALLAD, 1}, {GEN_CH_HALFTIME, 2}, {GEN_CH_PULSE, RHYTHM_PULSE}, {GEN_CH_PUSH, 5},
    {GEN_CH_STABS, 6}, {GEN_CH_OFFBEAT, 8}, {GEN_CH_FUNK, 20}, {GEN_CH_CHARLESTON, 21},
};

/* The library rhythm of an old chord style, else -1 (MANUAL, other lanes' styles) */
static int old_rhythm(int style) {
    for (int k = 0; k < 9; k++)
        if (OLD_RHYTHM[k].style == style) return OLD_RHYTHM[k].rhythm;
    return -1;
}

/* A DRAWN lane's style: the old style a library rhythm is, else Pulse */
static int old_style(int rhythm) {
    for (int k = 0; k < 9; k++)
        if (OLD_RHYTHM[k].rhythm == rhythm) return OLD_RHYTHM[k].style;
    return GEN_CH_PULSE;
}

/* 0.18: the lane's library rhythm ri (ed 0, from ri) in its unlocked bars [lo, hi); with `apply` 0 it only tells. 1 when a bar
 * changes. A drawn lane only (rbars_init). */
static int fill_rhythm(seq_part_t *p, int lane, int lo, int hi, int ri, int apply) {
    unsigned char c[16];
    int changed = 0;
    rhythm_lib_codes(DLANE[dl_of(lane)].rlib, ri, c);
    for (int b = lo; b < hi; b++) {
        rbar_t *r = &p->rb[dl_of(lane)][b];
        if (r->lock || (!memcmp(r->c, c, 16) && r->from == ri && !r->ed)) continue;
        changed = 1;
        if (!apply) continue;
        memcpy(r->c, c, 16);
        r->from = (signed char)ri;
        r->ed = 0;
    }
    return changed;
}

/* style_1 on a DRAWN lane: the old style's rhythm in every unlocked bar of the loop; one undo step, none when they have it.
 * An open bar or browser is closed as DONE first. MANUAL and other styles: nothing. */
static void pick_old_rhythm(seq_t *s, int style) {
    seq_part_t *p = cur(s);
    int ri = old_rhythm(style);
    if (ri < 0 || !fill_rhythm(p, LANE_CHORDS, 0, p->bars, ri, 0)) return;
    seq_txn_done(s);
    act(s);
    push_undo(s, LANE_CHORDS);
    fill_rhythm(p, LANE_CHORDS, 0, p->bars, ri, 1);
    render_rhythm(s, p, LANE_CHORDS);
}

void seq_set_style(seq_t *s, int lane, int style) {
    if (lane == LANE_CHORDS && drawn_lane(cur(s))) {
        pick_old_rhythm(s, style);
        return;
    }
    if (!style_fits(lane, style) || cur(s)->lane[lane].gen.style == style) return;
    act(s);
    seq_lane_t *ln = &cur(s)->lane[lane];
    push_undo(s, lane);
    ln->gen.style = style;
    if (style == GEN_MANUAL) memset(ln->lock, 0, sizeof ln->lock);   /* the steps stay, now the user's */
    else regen_lane(s, cur(s), lane);
}

/* The Q-Link path: Melody 2 follows only if Melody 1's notes moved (a Manual Melody 1's never do; a generated one's
 * stay put on about a third of the knob's steps). */
void seq_set_complexity(seq_t *s, int lane, int c) {
    seq_part_t *p = cur(s);
    seq_step_t was[SEQ_LANE_STEPS];
    int n = p->bars * SEQ_STEPS;
    if (lane < 0 || lane >= SEQ_ACTIVE_LANES || c < 0 || c > 100 || c == p->lane[lane].gen.complexity) return;
    if (jam_lane(p, lane)) {   /* 0.20: the playing block's level, in 5 bands, queued as a block tap (no classic regeneration) */
        act(s);
        p->lane[lane].gen.complexity = c;
        seq_mel_queue(s, p->mjam.set.c, c / 20 > MJ_LEVELS - 1 ? MJ_LEVELS - 1 : c / 20);
        return;
    }
    act(s);
    int old = p->lane[lane].gen.complexity;
    p->lane[lane].gen.complexity = c;
    if (lane != LANE_MEL_UP) {
        if (lane_drawn(p, lane)) cx_turned(s, p, lane, old);
        else regen(s, p, lane);
        return;
    }
    memcpy(was, p->lane[lane].steps, sizeof(seq_step_t) * (size_t)n);
    regen(s, p, lane);
    if (memcmp(was, p->lane[lane].steps, sizeof(seq_step_t) * (size_t)n)) follow(s, p, lane);
}

/* A new seed for a lane (GEN, NEW IDEA, NEW RHYTHM). On the Chords lane it also picks the chord rhythm's variant
 * (gen_chords_v2_variant), stored with the lane, so a saved lane keeps its template whatever the formula. */
static void new_seed(seq_t *s, seq_lane_t *ln, int lane) {
    ln->gen.seed = next_seed(s, ln->gen.seed);
    if (lane == LANE_CHORDS) ln->gen.variant = gen_chords_v2_variant(ln->gen.style, ln->gen.seed);
}

/* join: one undo step with the snapshot before it; returns whether the lane was generated */
static int generate(seq_t *s, int lane, int join) {
    if (lane < 0 || lane >= SEQ_ACTIVE_LANES) return 0;
    if (cur(s)->lane[lane].gen.style == GEN_MANUAL || lane_drawn(cur(s), lane) || jam_lane(cur(s), lane)) return 0;   /* 0.18: bars
                                                                                                   * stay; 0.20: a JAM Melody too */
    act(s);
    push_snap(s, lane, join);
    new_seed(s, &cur(s)->lane[lane], lane);
    regen_lane(s, cur(s), lane);
    return 1;
}

static void said(seq_t *s, int lane, const char *text);

/* NEW RHYTHM's family order: Sustain -> Ballad -> Half-time -> Pulse -> Offbeat -> Charleston -> Push -> Funk -> Stabs ->
 * Sustain, with the name the move text shows */
static const struct { int style; const char *name; } RHYTHMS[9] = {
    {GEN_CH_SUSTAIN, "Sustain"}, {GEN_CH_BALLAD, "Ballad"}, {GEN_CH_HALFTIME, "Half-time"}, {GEN_CH_PULSE, "Pulse"},
    {GEN_CH_OFFBEAT, "Offbeat"}, {GEN_CH_CHARLESTON, "Charleston"}, {GEN_CH_PUSH, "Push"}, {GEN_CH_FUNK, "Funk"},
    {GEN_CH_STABS, "Stabs"},
};

static int rhythm_at(int style) {
    for (int k = 0; k < 9; k++)
        if (RHYTHMS[k].style == style) return k;
    return -1;
}

int seq_next_rhythm(int style) {
    int k = rhythm_at(style);
    return k < 0 ? style : RHYTHMS[(k + 1) % 9].style;
}

const char *seq_rhythm_name(int style) {
    int k = rhythm_at(style);
    return k < 0 ? "" : RHYTHMS[k].name;
}

#define NEW_RHYTHM_CAP 12   /* chords-lane regenerations per press: the UI thread holds the wrapper's lock throughout */
#define NEW_RHYTHM_SEEDS 2  /* new seeds on the pressed rhythm, after its other variants */

static int all_locked(const seq_lane_t *ln, int nsteps) {
    for (int i = 0; i < nsteps; i++)
        if (!locked(ln, i)) return 0;
    return 1;
}

/* One press's tries: the steps before it and the regenerations left */
typedef struct { seq_t *s; seq_part_t *p; const seq_step_t *was; int left; } nr_tries_t;

/* One regeneration of the chords lane (none when the cap is spent): 1 if the rhythm (which steps play, and how long)
 * differs from the steps before the press. A velocity alone is not a new rhythm. */
static int nr_try(nr_tries_t *t) {
    if (t->left <= 0) return 0;
    t->left--;
    regen_lane(t->s, t->p, LANE_CHORDS);
    const seq_step_t *st = t->p->lane[LANE_CHORDS].steps;
    for (int i = 0; i < t->p->bars * SEQ_STEPS; i++)
        if (st[i].on != t->was[i].on || (st[i].on && st[i].len != t->was[i].len)) return 1;
    return 0;
}

/* A 0.12 rhythm's other 2 templates at the lane's seed: 1 at the first that moves the rhythm (the lane left on it) */
static int nr_variants(nr_tries_t *t) {
    seq_gen_t *g = &t->p->lane[LANE_CHORDS].gen;
    if (!gen_is_chords_v2(g->style)) return 0;
    for (int k = 1, v = g->variant; k < 3; k++) {
        g->variant = (v + k) % 3;
        if (nr_try(t)) return 1;
    }
    return 0;
}

static int nr_seeds(nr_tries_t *t, int n) {
    for (int k = 0; k < n && t->left > 0; k++) {
        new_seed(t->s, &t->p->lane[LANE_CHORDS], LANE_CHORDS);
        if (nr_try(t)) return 1;
    }
    return 0;
}

/* The rhythms after the lane's in the family order, each at the seed and variant the earlier tries left, then (a 0.12
 * rhythm) its other variants; until the cap: the RHYTHMS index of the first that moves the rhythm (the lane left on
 * it), or -1. No new seeds here: a rhythm whose first try and variants change nothing is stuck (a v1 rhythm at low
 * COMPLEXITY, short chords), and a new seed rarely helps it. */
static int nr_walk(nr_tries_t *t) {
    seq_lane_t *ln = &t->p->lane[LANE_CHORDS];
    for (int k = 1, at = rhythm_at(ln->gen.style); at >= 0 && k < 9 && t->left > 0; k++) {
        ln->gen.style = RHYTHMS[(at + k) % 9].style;
        if (nr_try(t) || nr_variants(t)) return (at + k) % 9;
    }
    return -1;
}

/* NEW RHYTHM (GEN on the Chords lane): always changes the rhythm, or says why not. A 0.12 rhythm tries its other 2
 * variants, then (any rhythm) up to 2 new seeds; when none moves the rhythm, the next rhythm in the family order
 * (nr_walk). At most NEW_RHYTHM_CAP regenerations a press; the cap reached with no change (never in the tests' grid)
 * leaves the lane as it was, with no undo step. One undo step restores the style, seed, variant and steps. Every loop
 * step locked: nothing (no undo step). */
static void new_rhythm(seq_t *s) {
    seq_step_t *before = s->nr_before;   /* the instance's scratch (seq_t, not static: several instances set at once on iOS, VST-33) */
    seq_part_t *p = cur(s);
    seq_lane_t *ln = &p->lane[LANE_CHORDS];
    nr_tries_t t = {s, p, before, NEW_RHYTHM_CAP};
    char text[48];
    if (drawn_lane(p)) return;   /* 0.18: NEW RHYTHM answers as a no-op on drawn bars */
    if (ln->gen.style == GEN_MANUAL) {
        said(s, LANE_CHORDS, "NEW RHYTHM: pick a rhythm first");
        return;
    }
    if (all_locked(ln, p->bars * SEQ_STEPS)) {
        said(s, LANE_CHORDS, "NEW RHYTHM: all hits locked");
        return;
    }
    act(s);
    seq_gen_t g = ln->gen;
    int top = s->undo_top, undo_n = s->undo_n;
    memcpy(before, ln->steps, sizeof s->nr_before);
    s->nr_over = s->undo[top];
    push_snap(s, LANE_CHORDS, 0);
    if (nr_variants(&t) || nr_seeds(&t, NEW_RHYTHM_SEEDS)) {
        said(s, LANE_CHORDS, "NEW RHYTHM: new variation");
        return;
    }
    int k = nr_walk(&t);
    if (k >= 0) {
        snprintf(text, sizeof text, "NEW RHYTHM: %s", RHYTHMS[k].name);
        said(s, LANE_CHORDS, text);
        return;
    }
    ln->gen = g;   /* the cap spent, nothing moved: as it was, no undo step */
    memcpy(ln->steps, before, sizeof s->nr_before);
    s->undo[top] = s->nr_over;   /* the push undone exactly, a full ring's oldest step included */
    s->undo_top = top;
    s->undo_n = undo_n;
    said(s, LANE_CHORDS, "NEW RHYTHM: nothing new here");
}

/* GEN: a new seed; on the Chords lane, NEW RHYTHM (new_rhythm) */
void seq_generate(seq_t *s, int lane) {
    seq_txn_done(s);
    if (lane == LANE_CHORDS) {
        new_rhythm(s);
        return;
    }
    if (!generate(s, lane, 0)) return;
}

void seq_generate_all(seq_t *s) {
    int done = 0;
    seq_txn_done(s);
    for (int lane = 0; lane < SEQ_ACTIVE_LANES; lane++) done |= generate(s, lane, done);
}

/* The SHAPE values a recipe snapshot (a Bass KEEP or move) restores: those ideas and moves change */
static const int RECIPE_SHAPE[5] = {BSHAPE_MOVEMENT, BSHAPE_SYNC, BSHAPE_OCTAVES, BSHAPE_REGISTER, BSHAPE_PUSH};

/* A lane snapshot back in place, its complexity left as it is now (knob moves are not undoable); a recipe snapshot
 * restores its complexity (with style and seed) and RECIPE_SHAPE too. Melody 1's restores the part's melody recipe, its
 * knobs only from a recipe snapshot. Bars a
 * grow filled since the snapshot repeat it, as the grow repeated the lane then; a generated lane regenerates (at
 * today's complexity, and over today's length and chords). */
static void restore_lane(seq_t *s, seq_part_t *p, const seq_snap_t *u) {
    seq_lane_t *ln = &p->lane[u->lane];
    int complexity = ln->gen.complexity, cx = chords_cx(p), d = dl_of(u->lane), o = DL_COUNT - 1 - d, olw = d >= 0 ? p->lw[o] : 0;
    unsigned char mute = ln->mute;
    memcpy(ln, &u->ln, sizeof u->ln);
    ln->mute = mute;   /* 0.26 a mute is not an edit: the lane's stays */
    if (dl_of(u->lane) >= 0) {   /* 0.18: the drawn bars; the RHYTHM knobs, like the complexity, only from RESET KNOBS's */
        memcpy(p->rb[dl_of(u->lane)], u->rb, sizeof u->rb);
        if (u->knobs == 2) memcpy(p->rk[dl_of(u->lane)], u->rk, sizeof u->rk);   /* RHYTHM's RESET KNOBS only, never CHORDS' */
        p->djam[dl_of(u->lane)].c = u->djc, p->djam[dl_of(u->lane)].l = u->djl;   /* 0.22 the block then; the JAM knobs stay */
        memcpy(p->lw, u->lw, sizeof p->lw), p->an[d] = u->an;   /* 0.24 both lanes' LOCK WITH (a set may have turned the other OWN) */
    }
    if (d >= 0) memcpy(p->hoct, u->hoct, sizeof p->hoct);   /* 0.24 (final review: either drawn lane's step, as push_snap_part) */
    if (u->lane == LANE_BASS) bass_back(&p->bass, &u->bass, u->bkeep);   /* 0.19: the note settings only from bkeep */
    if (u->lane == LANE_MEL_UP && u->ln.gen.style != GEN_MEL_JAM) p->mjam = u->mjam;   /* 0.20: a JAM conversion's (mel_jam_now): all of it */
    else if (u->lane == LANE_MEL_UP) mjam_back(&p->mjam, &u->mjam);                    /* 0.20: not the settings (knob turns) */
    if (u->lane == LANE_MEL_UP) {   /* the recipe; its knobs, like the complexity, only from a recipe snapshot */
        unsigned char kn[MK_COUNT];
        memcpy(kn, p->mel.kn, sizeof kn);
        p->mel = u->mel;
        if (!u->recipe) memcpy(p->mel.kn, kn, sizeof kn);
    }
    if (u->knobs)   /* 0.17.1 RESET KNOBS: the complexity (as the snapshot's lane) and the whole SHAPE row come back */
        memcpy(p->lane_shape[u->lane], u->shape, sizeof u->shape);
    else if (u->recipe)   /* a KEEP or a move: what ideas and moves change comes back; GATE, ACCENT, OCT, APPROACH stay */
        for (int k = 0; k < (int)(sizeof RECIPE_SHAPE / sizeof RECIPE_SHAPE[0]); k++)
            p->lane_shape[u->lane][RECIPE_SHAPE[k]] = u->shape[RECIPE_SHAPE[k]];
    if (!u->knobs && !u->recipe) ln->gen.complexity = complexity;
    repeat_bars(ln, u->filled, p->filled, u->bars);
    if (dl_of(u->lane) >= 0) repeat_rbars(p, u->lane, u->filled, p->filled, u->bars);
    if (jam_lane(p, u->lane)) mel_repeat(p, u->filled, p->filled, u->bars);
    if (d >= 0 && p->lw[o] != olw && both_drawn(p))   /* 0.24 the other lane's LOCK WITH back; 0.24.1 both lanes once each, as below */
        render_pair(s, p, p->lw[d] != LW_OWN ? o : d);
    else {
        regen_lane(s, p, u->lane);   /* a Manual Melody 1 restored moves an Answer too */
        if (d >= 0 && p->lw[o] != olw && lane_drawn(p, DLANE[o].lane)) render_rhythm(s, p, DLANE[o].lane);   /* 0.24 the other lane's LOCK WITH back */
    }
    if (u->lane == LANE_CHORDS && !harmony_same(cx, chords_cx(p))) colour_changed(s, p);   /* 0.18: RESET KNOBS's COMPLEXITY back */
}

/* A progression snapshot back in place: the slots, the chord length (0.14, a chord-length change's undo) and the phrase.
 * Slots a grow filled since the snapshot repeat it, as the grow repeated the loop then (as restore_lane does for bars). */
static void restore_prog(seq_t *s, seq_part_t *p, const seq_snap_t *u) {
    int cl = p->chord_len;
    memcpy(p->prog, u->prog, sizeof u->prog);   /* 0.24 the real slots; the change points from cp (revoice mirrors them) */
    p->harmony_seed = u->harmony_seed;
    memcpy(p->src, u->src, sizeof u->src);
    memcpy(p->colour, u->colour, sizeof u->colour);
    memcpy(p->cp, u->cp, sizeof p->cp);
    p->slot_lock = u->slot_lock;
    p->chord_len = u->chord_len;
    p->filled_slots = u->filled_slots;
    p->phrase = u->phrase;
    p->rank_len = u->rank_len;
    if (u->vox) vox_write(p, u->vx);   /* 0.24.1 colour_changed's work is all below: the revoice, and its Bass and recipe Melody read harmony */
    grow_slots(p, slots_of(u->bars, u->chord_len), part_slots(p));
    for (int b = u->filled; b < p->filled; b++) memcpy(p->cp[b], p->cp[b % u->bars], sizeof p->cp[b]);   /* 0.24 as seq_set_bars */
    strip_clamp(s);
    revoice(s, p);   /* a borrow takes its degree in today's scale */
    if (p->chord_len != cl) regen_part(s, p);
    else regen_harmony(s, p);
}

static int rbar_undo(seq_t *s);
static int rbar_own_top(const seq_t *s);
static void djq_sweep(seq_t *s);
static int djq_undo(seq_t *s);

/* Restores the last snapshot: a lane, or the progression (revoiced, its generated lanes regenerated); a joined group,
 * newest first. 0.18: with a bar open, its own steps one at a time. */
void seq_undo(seq_t *s) {
    const seq_snap_t *u;
    if (rbar_undo(s)) return;   /* 0.18: the open bar's COPY TO... or RHYTHMS closed */
    /* 0.18: an open bar's own step is undone below, the bar staying open */
    if (s->txn.kind && !rbar_own_top(s)) {   /* 0.17: with a picker or browser open UNDO is its CANCEL (as a try's BACK); a stale one is not shown, so UNDO goes on */
        int shown = !s->txn.stale;
        txn_cancel(s);
        if (shown) return;
    }
    if (!s->txn.kind) djq_sweep(s);   /* 0.22: a tap a part switch dropped leaves no step (an open bar's ring copy waits: its CANCEL) */
    if (!rbar_own_top(s) && djq_undo(s)) return;   /* 0.22: a JAM block waiting: UNDO cancels it (the latest) and nothing else */
    if (!s->undo_n) return;
    act(s);
    do {
        if (!s->undo_n) break;
        s->undo_top = (s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO;
        s->undo_n--;
        u = &s->undo[s->undo_top];
        seq_part_t *p = &s->part[u->part];
        if (u->lane >= 0) {
            restore_lane(s, p, u);
            if (u->prog_too) restore_prog(s, p, u);   /* 0.24 a PHRASE edit's slots, picks and points */
            continue;
        }
        restore_prog(s, p, u);
    } while (u->join);
}

void seq_set_harmony(seq_t *s, int c) {
    if (c < 0 || c > 100 || c == cur(s)->harmony) return;
    act(s);
    cur(s)->harmony = c;
}

/* Locked slots keep their chord, borrow and colour; the others are the generator's (gen_progression_v2: above HARMONY
 * 60 one may be borrowed), Triads */
void seq_generate_progression(seq_t *s) {
    seq_part_t *p = cur(s);
    int len, deg[SEQ_MAX_PROG], src[SEQ_MAX_PROG];
    unsigned char bsrc[SEQ_MAX_PROG];
    seq_txn_done(s);
    act(s);
    len = slot_cycle(p);   /* 0.14: the loop's phrase, as 0.13's progression */
    len = len < 2 ? 2 : len > 8 ? 8 : len;
    push_undo(s, -1);
    p->harmony_seed = next_seed(s, p->harmony_seed);
    gen_progression_v2(p->harmony_seed, p->harmony, len, deg, src, s->scale);
    for (int i = 0; i < len; i++) bsrc[i] = (unsigned char)src[i];
    fill_phrase(p, len, deg, bsrc, NULL);   /* Triads; locked slots keep theirs */
    p->rank_len = len;   /* a deal */
    revoice(s, p);
    regen_harmony(s, p);
}

/* 0.9: slots, locks and SHAPE */

/* A pick that changes nothing (the host re-sending it) is no edit and pushes no undo */
void seq_set_slot(seq_t *s, int slot, int deg, int src, int colour) {
    seq_part_t *p = cur(s);
    prog_view_t y = part_view(ccur(s));
    if (slot < 0 || slot >= y.len || src < 0 || src > THEORY_NBORROW || (!src && (deg < 0 || deg > 6)) || colour < 0 ||
        colour >= COL_COUNT)
        return;
    if (src) deg = theory_borrow_degree(s->scale, src);
    if (y.prog[slot] == deg && y.src[slot] == src && (y.lock >> slot & 1) &&
        (y.colour[slot] == colour || y.colour[slot] == lock_colour(s, p, slot, deg, src, colour)))
        return;   /* a re-send (a locked Triad slot is as it is stored) */
    colour = lock_colour(s, p, slot, deg, src, colour);   /* it locks the slot */
    act(s);
    push_undo(s, -1);
    p->prog[slot] = deg;
    p->src[slot] = (unsigned char)src;
    p->colour[slot] = (unsigned char)colour;
    p->slot_lock |= SLOT_BIT(slot);
    revoice(s, p);
    cp_prune(s, p);   /* 0.24 final review: a point now playing its slot's chord goes, in this undo step */
    regen_harmony(s, p);
}

void seq_toggle_slot_lock(seq_t *s, int slot) {
    seq_part_t *p = cur(s);
    int phrase, colour;
    if (slot < 0 || slot >= part_view(ccur(s)).len) return;
    act(s);
    p->slot_lock ^= SLOT_BIT(slot);
    phrase = p->phrase;
    colour = p->colour[slot];
    if (slot_locked(p, slot)) p->colour[slot] = (unsigned char)lock_colour(s, p, slot, p->prog[slot], p->src[slot], colour);
    revoice(s, p);   /* 0.14: a lock that breaks the loop's phrase (or restores it) changes the phrase */
    if (p->phrase != phrase || p->colour[slot] != colour) regen_harmony(s, p);   /* the smooth voicing's chain follows the phrase (the ranks stay) */
}

/* 0.24 the getters read a live change point's virtual slot too (slot_read_ok), the setters above never */
int seq_slot_locked(const seq_t *s, int slot) { return slot_read_ok(ccur(s), slot) && slot_locked(ccur(s), slot); }
int seq_slot_deg(const seq_t *s, int slot) { return slot_read_ok(ccur(s), slot) ? ccur(s)->prog[slot] : 0; }
int seq_slot_src(const seq_t *s, int slot) { return slot_read_ok(ccur(s), slot) ? ccur(s)->src[slot] : 0; }
int seq_slot_colour(const seq_t *s, int slot) { return slot_read_ok(ccur(s), slot) ? ccur(s)->colour[slot] : 0; }
int seq_prog_len(const seq_t *s) { return ccur(s)->prog_len; }
int seq_slots(const seq_t *s) { return part_slots(ccur(s)); }

/* Into part p, revoiced and its harmony lanes regenerated; no undo, no BACK, no new field. Locks are kept. n = the slot
 * count: the whole loop, every slot written; else (1..16) a phrase over the loop, locked slots unchanged (fill_phrase).
 * Out-of-range values clamp (degrees wrap by octave, as the theory functions read them). The COLOUR ranks' period: n on
 * a deal; else kept (an idea or move rewrites the loop, or its phrase at a length the ranks divide), n for a phrase of a
 * new length. */
static void set_prog(seq_t *s, seq_part_t *p, int n, const int *deg, const unsigned char *src, const unsigned char *colour,
                     int deal) {
    int rank = p->rank_len, whole = n == part_slots(p);
    if (deal || rank < 1 || (!whole && (n != p->phrase || n % rank))) rank = n <= SEQ_LEGACY_PROG ? n : 0;
    if (whole) {   /* the loop, nothing hidden; its length the phrase when 0.13 could save it so */
        for (int i = 0; i < n; i++) {
            int b = src ? src[i] : 0, c = colour ? colour[i] : 0;
            if (slot_locked(p, i)) continue;   /* a locked slot keeps its chord (the repeats of a locked phrase slot too) */
            p->src[i] = (unsigned char)(b > THEORY_NBORROW ? 0 : b);
            p->colour[i] = (unsigned char)(c >= COL_COUNT ? 0 : c);
            p->prog[i] = pmod(deg[i], 7);   /* a borrow's degree: revoice */
        }
        if (p->filled_slots < n) p->filled_slots = n;   /* slots a shrink hid keep theirs */
        p->phrase = n <= SEQ_LEGACY_PROG ? n : 0;
    } else {
        fill_phrase(p, n, deg, src, colour);
    }
    p->rank_len = rank;
    revoice(s, p);
    regen_harmony(s, p);
}

void seq_set_progression(seq_t *s, int n, const int *deg, const unsigned char *src, const unsigned char *colour, int undo) {
    act(s);
    if (undo) push_undo(s, -1);
    if (n != seq_slots(s)) n = n < 1 ? 1 : n > 8 ? 8 : n;   /* a phrase */
    set_prog(s, cur(s), n, deg, src, colour, 1);
}

/* The Bass's MOVEMENT, SYNC, OCTAVES and APPROACH change its steps (regenerated here); the others act at playback (0.17: the
 * Chords' SHAPE_COLOUR and SHAPE_SPREAD only keep their values, vcol and vspr sound) */
void seq_set_lane_shape(seq_t *s, int lane, int what, int v) {
    seq_part_t *p = cur(s);
    int lo, hi;
    if (!lshape_range(lane, what, &lo, &hi)) return;
    v = v < lo ? lo : v > hi ? hi : v;
    if (v == p->lane_shape[lane][what]) return;
    act(s);
    p->lane_shape[lane][what] = v;
    if (lane == LANE_BASS && (what == BSHAPE_MOVEMENT || what == BSHAPE_SYNC || what == BSHAPE_OCTAVES || what == BSHAPE_APPROACH)) {
        regen_lane(s, p, LANE_BASS);
    }
}

int seq_lane_shape(const seq_t *s, int lane, int what) {
    int lo, hi;
    return lshape_range(lane, what, &lo, &hi) ? ccur(s)->lane_shape[lane][what] : 0;
}

void seq_set_chord_shape(seq_t *s, int what, int v) { seq_set_lane_shape(s, LANE_CHORDS, what, v); }

int seq_chord_shape(const seq_t *s, int what) { return seq_lane_shape(s, LANE_CHORDS, what); }

/* A knob's range, 0 for a bad `what` */
static int vknob_range(int what, int *lo, int *hi) {
    static const signed char RANGE[VK_COUNT][2] = {{-1, 4}, {-2, 3}, {-1, 3}, {-8, 8}};
    if (what < 0 || what >= VK_COUNT) return 0;
    *lo = RANGE[what][0];
    *hi = RANGE[what][1];
    return 1;
}

/* STRUM's step on a part: the step nearest SHAPE_STRUM, signed by the direction */
static int part_vstr(const seq_part_t *p) {
    int k = (int)lround(p->lane_shape[LANE_CHORDS][SHAPE_STRUM] / 12.5);
    return p->vstrdir ? -k : k;
}

int seq_vknob(const seq_t *s, int what) {
    const seq_part_t *p = ccur(s);
    switch (what) {
    case VK_COL: return p->vcol;
    case VK_INV: return p->vinv;
    case VK_SPR: return p->vspr;
    case VK_STR: return part_vstr(p);
    }
    return 0;
}

/* 0.18: the knobs as they play under COMPLEXITY (seq_core.h) */
int seq_part_vknob_eff(const seq_t *s, int part, int what) {
    const seq_part_t *p = &s->part[part >= 0 && part < SEQ_NPARTS ? part : s->cur_part];
    int cx = chords_cx(p);
    cx_offsets_t o = rhythm_cx_offsets(cx);
    if (cx == CHORDS_CX_NEUTRAL) o.col = o.tension = o.strum = 0;
    switch (what) {
    case VK_COL: return rhythm_cx_apply(CXT_COL, p->vcol, o.col + o.tension);
    case VK_INV: return p->vinv;
    case VK_SPR: return eff_spread(p);
    case VK_STR: return rhythm_cx_apply(CXT_STRUM, part_vstr(p), o.strum);
    }
    return 0;
}

int seq_vknob_eff(const seq_t *s, int what) { return seq_part_vknob_eff(s, s->cur_part, what); }

int seq_rknob_eff(const seq_t *s, int lane, int what) {
    const seq_part_t *p = ccur(s);
    return dl_of(lane) < 0 || what < RK_SYNC || what >= RK_COUNT ? 0 : rk_at(p, lane, what, lane_rcx(p, lane));
}

int seq_vmode_eff(const seq_t *s) { return eff_vmode(ccur(s)); }

/* The chords' colours changed: the voices and labels, and the lanes that read the colours (a shaped Bass, a recipe melody; 0.24.2 a drawn
 * lane with ANTICIPATE on: a colour can make or remove a chord change, chord_change_at) */
static void colour_changed(seq_t *s, seq_part_t *p) {
    int shaped = p->lane[LANE_BASS].gen.style != GEN_MANUAL && bass_shaped(p);
    revoice(s, p);
    if (shaped) regen_lane(s, p, LANE_BASS);   /* a shaped Bass reads the chords' colours (the generator does not) */
    if (is_recipe(p, LANE_MEL_UP)) regen_lane(s, p, LANE_MEL_UP);   /* so does a recipe melody (absolute notes) */
    for (int d = 0; d < DL_COUNT; d++)
        if (p->an[d] && lane_drawn(p, DLANE[d].lane) && !(d == DL_BASS && shaped)) render_mask(s, p, DLANE[d].lane, (1u << p->filled) - 1, 0);
}

void seq_set_vknob(seq_t *s, int what, int v) {
    seq_part_t *p = cur(s);
    int lo, hi;
    if (!vknob_range(what, &lo, &hi)) return;
    v = v < lo ? lo : v > hi ? hi : v;
    if (v == seq_vknob(s, what)) return;
    act(s);
    if (what == VK_COL) p->vcol = v;
    else if (what == VK_INV) p->vinv = v;
    else if (what == VK_SPR) p->vspr = v;
    else {
        p->lane_shape[LANE_CHORDS][SHAPE_STRUM] = (int)lround(iabs(v) * 12.5);
        p->vstrdir = v < 0;
    }
    if (what == VK_COL) colour_changed(s, p);
}

static int voicing_edited(const seq_t *s);

/* 1 when the 8 Chords knobs are as RESET KNOBS leaves them */
static int chord_knobs_fresh(const seq_t *s) {
    const seq_part_t *p = ccur(s);
    const int *n = LSHAPE_NEUTRAL[LANE_CHORDS];
    int voicing = p->vstyle >= 0 ? !voicing_edited(s) : !p->vcol && !p->vinv && !p->vspr && !part_vstr(p);
    return voicing && p->lane[LANE_CHORDS].gen.complexity == CHORDS_CX_NEUTRAL && p->lane_shape[LANE_CHORDS][SHAPE_PUSH] == n[SHAPE_PUSH] &&
           p->lane_shape[LANE_CHORDS][SHAPE_GATE] == n[SHAPE_GATE] && p->lane_shape[LANE_CHORDS][SHAPE_DYN] == n[SHAPE_DYN];
}

void seq_reset_chord_knobs(seq_t *s) {
    seq_part_t *p = cur(s);
    const int *n = LSHAPE_NEUTRAL[LANE_CHORDS];
    if (chord_knobs_fresh(s)) return;
    seq_txn_done(s);   /* ruling 20: an open picker or browser is kept first */
    act(s);
    push_voicing_snap(s);
    push_snap(s, LANE_CHORDS, 1);   /* one step with the voicing's */
    s->undo[(s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO].knobs = 1;
    if (p->vstyle >= 0) part_style(p, p->vstyle);
    else {
        p->vcol = p->vinv = p->vspr = 0;
        p->lane_shape[LANE_CHORDS][SHAPE_STRUM] = 0, p->vstrdir = 0;
    }
    p->lane_shape[LANE_CHORDS][SHAPE_PUSH] = n[SHAPE_PUSH];
    p->lane_shape[LANE_CHORDS][SHAPE_GATE] = n[SHAPE_GATE];
    p->lane_shape[LANE_CHORDS][SHAPE_DYN] = n[SHAPE_DYN];
    colour_changed(s, p);
    seq_set_complexity(s, LANE_CHORDS, CHORDS_CX_NEUTRAL);   /* renders the chord rhythm when it changes */
}

/* 1 when RHYTHM's RESET KNOBS has nothing to reset on the drawn lane */
static int rhythm_knobs_fresh(const seq_t *s, int lane) {
    const seq_part_t *p = ccur(s);
    const signed char *rk = p->rk[dl_of(lane)];
    const int *n = LSHAPE_NEUTRAL[lane], *sh = p->lane_shape[lane];
    return !rk[RK_SYNC] && !rk[RK_GHOSTS] && !rk[RK_HUM] && !rk[RK_FILL] && !rk[RK_CAD] && !rk[RK_BREATH] && p->lane[lane].gen.complexity == CHORDS_CX_NEUTRAL &&
           sh[L_PUSH[lane]] == n[L_PUSH[lane]] && sh[L_GATE[lane]] == n[L_GATE[lane]] && sh[L_DYN[lane]] == n[L_DYN[lane]];
}

void seq_reset_rhythm_knobs(seq_t *s, int lane) {
    seq_part_t *p = cur(s);
    const int *n;
    if (dl_of(lane) < 0 || rhythm_knobs_fresh(s, lane)) return;
    n = LSHAPE_NEUTRAL[lane];
    seq_txn_done(s);
    act(s);
    push_snap(s, lane, 0);
    s->undo[(s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO].knobs = 2;   /* undo restores the complexity, SHAPE row and rk */
    memset(p->rk[dl_of(lane)], 0, sizeof p->rk[dl_of(lane)]);
    p->lane_shape[lane][L_PUSH[lane]] = n[L_PUSH[lane]];
    p->lane_shape[lane][L_GATE[lane]] = n[L_GATE[lane]];
    p->lane_shape[lane][L_DYN[lane]] = n[L_DYN[lane]];
    int cx = chords_cx(p);
    p->lane[lane].gen.complexity = CHORDS_CX_NEUTRAL;
    if (lane == LANE_CHORDS && !harmony_same(cx, chords_cx(p))) colour_changed(s, p);   /* the voices read the Chords lane's */
    regen(s, p, lane);   /* every bar: the RHYTHM knobs changed (a COMPLEXITY turn renders only by its own layers) */
}

void seq_set_rknob(seq_t *s, int lane, int what, int v) {
    seq_part_t *p = cur(s);
    signed char *rk;
    if (dl_of(lane) < 0 || what < RK_SYNC || what >= RK_COUNT) return;
    rk = p->rk[dl_of(lane)];
    v = rk_clamp(what, v);
    if (v == rk[what]) return;
    act(s);
    rk[what] = (signed char)v;
    if (lane_drawn(p, lane)) render_rhythm(s, p, lane);   /* every bar, whatever COMPLEXITY's gate would render */
}

int seq_rknob(const seq_t *s, int lane, int what) {
    return dl_of(lane) < 0 || what < RK_SYNC || what >= RK_COUNT ? 0 : ccur(s)->rk[dl_of(lane)][what];
}

/* 1 if the part is a style and any of the eight values differ from the style's */
static int part_edited(const seq_part_t *p) {
    const voicing_style_t *v;
    if (p->vstyle < 0 || p->vstyle >= VOICING_NSTYLES) return 0;
    v = &VOICING_STYLES[p->vstyle];
    return p->vmode != v->mode || p->vcol != v->col || p->vinv != v->inv || p->vspr != v->spr || part_vstr(p) != v->str ||
           p->lane_shape[LANE_CHORDS][SHAPE_OCT] != v->oct;
}
static int voicing_edited(const seq_t *s) { return part_edited(ccur(s)); }

int seq_voicing_edited(const seq_t *s) { return voicing_edited(s); }

int seq_part_voicing_edited(const seq_t *s, int part) { return part_edited(&s->part[part >= 0 && part < SEQ_NPARTS ? part : s->cur_part]); }

int seq_voicing_style(const seq_t *s) { return ccur(s)->vstyle; }

/* Style i onto the playing part, no undo; 0 when it is already so (the style, not edited) or bad */
static int voicing_apply(seq_t *s, int i) {
    seq_part_t *p = cur(s);
    if (i < 0 || i >= VOICING_NSTYLES) return 0;
    if (p->vstyle == i && !voicing_edited(s)) return 0;   /* already this style as it is */
    act(s);
    part_style(p, i);
    colour_changed(s, p);
    return 1;
}

void seq_set_chord_octave(seq_t *s, int o) {
    if (o < -1 || o > 1 || o == ccur(s)->lane_shape[LANE_CHORDS][SHAPE_OCT]) return;
    seq_txn_done(s);
    act(s);
    push_voicing_snap(s);   /* the snapshot holds the octave (vox_read) */
    seq_set_chord_shape(s, SHAPE_OCT, o);
}

void seq_set_voicing_style(seq_t *s, int i) {
    if (i < 0 || i >= VOICING_NSTYLES || (cur(s)->vstyle == i && !voicing_edited(s))) return;
    act(s);
    push_voicing_snap(s);
    voicing_apply(s, i);
}

/* 0.17 the PROGRESSIONS / VOICING browsers (0.24.1 the FIX chord picker, CVIEW_PICK, is gone). One transaction at a time
 * (s->txn, kind CVIEW_PROG / VOICE, none CVIEW_CHORDS = 0). Open saves what CANCEL puts back and a copy of the undo ring (a push
 * may drop its oldest snapshot). seq_block cannot cancel: switch_part flags the transaction stale, the getters read a stale one
 * as closed, and the next UI-thread setter (txn_live) cancels it: its part is not touched since, as selecting it would have cancelled it. Its own
 * pushes (a pick, a browser's open) are marked (seq_snap_t.txn): a cancel puts the ring back whole when nothing else pushed since,
 * else drops only those, so another edit's steps stay. 0.19: one transaction at a time across the tabs: one opening on `lane` closes
 * another lane's as DONE first (a stale one is cancelled) and puts back its own lane's (0.19.1: here, for every open) */

/* 0.19.1 before `lane` opens a transaction: a stale one cancelled, another lane's kept (DONE), the lane's own cancelled */
static void txn_close_for(seq_t *s, int lane) {
    txn_unstale(s);
    if (s->txn.kind && s->txn.lane != lane) txn_done(s);
    txn_cancel(s);
}

static void txn_begin(seq_t *s, int lane, int kind, int slot) {
    seq_txn_t *t = &s->txn;
    const seq_part_t *p = cur(s);
    txn_close_for(s, lane);
    strip_hold_open(s, tab_of_lane(lane), kind);   /* before the kind is set: the strip as it shows now */
    t->kind = kind, t->lane = lane, t->stale = 0, t->part = s->cur_part, t->slot = slot;
    t->pushes0 = s->undo_pushes, t->own = 0;
    for (int i = 0; i < SEQ_UNDO; i++) s->undo[i].txn = 0;   /* an earlier transaction's, kept by its DONE */
    t->undo_n = s->undo_n, t->undo_top = s->undo_top;
    memcpy(t->ring, s->undo, sizeof t->ring);
    memcpy(t->prog, p->prog, sizeof t->prog);
    memcpy(t->src, p->src, sizeof t->src);
    memcpy(t->colour, p->colour, sizeof t->colour);
    t->lock = p->slot_lock;
    t->prog_len = p->prog_len, t->filled_slots = p->filled_slots, t->phrase = p->phrase, t->rank_len = p->rank_len;
    vox_read(p, t->vx);
    if (dl_of(lane) >= 0) {   /* 0.18: what an open bar's or RHYTHMS browser's CANCEL puts back (0.24 and its LOCK WITH) */
        memcpy(t->rb, p->rb[dl_of(lane)], sizeof t->rb);
        t->lw = p->lw[dl_of(lane)];
        memcpy(t->hoct, p->hoct, sizeof t->hoct);   /* 0.24 final review (ruling 32): a try that takes a hit away takes its octave */
    }
    t->sub = 0, t->copy_mask = 0;
    t->bset = p->bass.set;   /* 0.19: what the STYLE browser's CANCEL puts back */
    t->bturned = 0;
    if (kind == CVIEW_BAR) {   /* 0.24 what an open PHRASE bar's CANCEL puts back besides the bars and the progression */
        memcpy(t->cp, p->cp, sizeof t->cp);
        memcpy(t->bvars, p->lane[lane].bar_var, sizeof t->bvars);
        t->bass = p->bass;
        t->copy_what = 0;
    }
    t->mset = p->mjam.set;   /* 0.20: what the ANSWER BLOCK picker's CANCEL puts back (its SWAP block) */
    if (kind == TXN_MEDIT) {   /* 0.20: the Melody bar editor's bar, what its CANCEL puts back */
        t->bvar = p->lane[LANE_MEL_UP].bar_var[slot];
        t->bnlock = (unsigned char)(p->mjam.nlock >> slot & 1);
        t->mfrozen = p->mjam.frozen[slot];
        memcpy(t->bfix, &p->mjam.fix[slot * SEQ_STEPS], SEQ_STEPS);
    }
}

/* 0.20 the Melody bar editor's bar exchanged with what it was at open: its NEW BAR variation, lock, frozen settings and fixes */
static void medit_swap(seq_part_t *p, seq_txn_t *t) {
    int b = t->slot;
    unsigned char v = p->lane[LANE_MEL_UP].bar_var[b], l = (unsigned char)(p->mjam.nlock >> b & 1), f[SEQ_STEPS];
    mjam_set_t z = p->mjam.frozen[b];
    memcpy(f, &p->mjam.fix[b * SEQ_STEPS], SEQ_STEPS);
    p->lane[LANE_MEL_UP].bar_var[b] = t->bvar, t->bvar = v;
    p->mjam.nlock = (unsigned short)((p->mjam.nlock & ~(1u << b)) | (unsigned)t->bnlock << b), t->bnlock = l;
    p->mjam.frozen[b] = t->mfrozen, t->mfrozen = z;
    memcpy(&p->mjam.fix[b * SEQ_STEPS], t->bfix, SEQ_STEPS);
    memcpy(t->bfix, f, SEQ_STEPS);
}

/* 0.18 the RHYTHM page's transactions: an open bar (CVIEW_BAR) and the RHYTHMS browser on its own (CVIEW_RBROWSE). 0.19: on
 * either drawn lane (the kinds are the same; t->lane says whose bars) */
static int rhythm_txn(const seq_txn_t *t) { return dl_of(t->lane) >= 0 && (t->kind == CVIEW_BAR || t->kind == CVIEW_RBROWSE); }

static void swap_i(int *a, int *b) { int x = *a; *a = *b, *b = x; }
static void swap_b(unsigned char *a, unsigned char *b) { unsigned char x = *a; *a = *b, *b = x; }

/* 0.24 n bytes of a and b exchanged */
static void swap_mem(void *a, void *b, size_t n) {
    unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) swap_b(&x[i], &y[i]);
}

/* 0.24 an open PHRASE bar's part fields besides its drawn bars exchanged with what they were at open (twice: as it was): the real
 * slots and their picks, the change points (mirrored, no revoicing), hit octaves, the lane's NEW BAR versions and the Bass's fixed
 * notes, note locks and frozen settings (its settings stay: knob turns are kept). 1 when the slots, picks or points differed */
static int phrase_swap(seq_part_t *p, seq_txn_t *t) {
    uint64_t lock = p->slot_lock;
    int d = memcmp(p->prog, t->prog, sizeof t->prog) || memcmp(p->src, t->src, sizeof t->src) || memcmp(p->colour, t->colour, sizeof t->colour) ||
            lock != t->lock || memcmp(p->cp, t->cp, sizeof t->cp);
    for (int i = 0; i < SEQ_MAX_PROG; i++) {
        swap_i(&p->prog[i], &t->prog[i]);
        swap_b(&p->src[i], &t->src[i]);
        swap_b(&p->colour[i], &t->colour[i]);
    }
    p->slot_lock = t->lock, t->lock = lock;
    swap_mem(p->cp, t->cp, sizeof t->cp);
    swap_mem(p->hoct, t->hoct, sizeof t->hoct);
    swap_mem(p->lane[t->lane].bar_var, t->bvars, sizeof t->bvars);
    swap_mem(p->bass.fix, t->bass.fix, sizeof t->bass.fix);
    swap_mem(p->bass.frozen, t->bass.frozen, sizeof t->bass.frozen);
    swap_mem(&p->bass.nlock, &t->bass.nlock, sizeof t->bass.nlock);
    cp_sync(p);
    return d;
}

/* 0.24 final review (ruling 32): a RHYTHM transaction's part back on the bars it opened on gets their hit octaves back (h; its tries
 * cleared those of the hits they took away), the Chords re-rendered when they differ */
static void hoct_back(seq_t *s, signed char h[][16]) {
    seq_part_t *p = &s->part[s->txn.part];
    if (!memcmp(p->hoct, h, sizeof p->hoct)) return;
    memcpy(p->hoct, h, sizeof p->hoct);
    if (lane_drawn(p, LANE_CHORDS)) render_rhythm(s, p, LANE_CHORDS);
}

/* The part's fields a cancel restores, exchanged with the saved ones (twice: as it was); no revoicing */
static void txn_swap(seq_t *s) {
    seq_txn_t *t = &s->txn;
    seq_part_t *p = &s->part[t->part];
    int vx[8];
    uint64_t lock = p->slot_lock;
    if (!t->kind) return;
    if (rhythm_txn(t)) {   /* 0.18: the drawn bars (a DRAWN lane saves only them) */
        rbar_t *rb = p->rb[dl_of(t->lane)];
        signed char lw = p->lw[dl_of(t->lane)];
        for (int b = 0; b < SEQ_MAX_BARS; b++) {
            rbar_t r = rb[b];
            rb[b] = t->rb[b], t->rb[b] = r;
        }
        p->lw[dl_of(t->lane)] = t->lw, t->lw = lw;   /* 0.24 */
        if (t->kind == CVIEW_BAR) phrase_swap(p, t);   /* 0.24 (the hit octaves with it) */
        else swap_mem(p->hoct, t->hoct, sizeof t->hoct);   /* 0.24 final review: the RHYTHMS browser's, as its bars */
        return;
    }
    if (t->kind == CVIEW_VOICE) {
        vox_read(p, vx);
        vox_write(p, t->vx);
        memcpy(t->vx, vx, sizeof vx);
        return;
    }
    if (t->kind == TXN_BSTYLE) {   /* 0.19: the note settings, the knobs turned in the browser kept as they are */
        bass_set_t b = p->bass.set;
        bset_keep(&p->bass.set, &t->bset, t->bturned);
        t->bset = b;
        return;
    }
    if (t->kind == TXN_MPICK) {   /* 0.20: the SWAP block (any other setting turned meanwhile stays) */
        mjam_set_t m = p->mjam.set;
        p->mjam.set.sc = t->mset.sc, p->mjam.set.sl = t->mset.sl;
        t->mset = m;
        return;
    }
    if (t->kind == TXN_MEDIT) {   /* 0.20: the Melody editor's bar */
        medit_swap(p, t);
        return;
    }
    for (int i = 0; i < SEQ_MAX_PROG; i++) {
        swap_i(&p->prog[i], &t->prog[i]);
        swap_b(&p->src[i], &t->src[i]);
        swap_b(&p->colour[i], &t->colour[i]);
    }
    p->slot_lock = t->lock, t->lock = lock;
    swap_i(&p->phrase, &t->phrase);
    swap_i(&p->prog_len, &t->prog_len);
    swap_i(&p->filled_slots, &t->filled_slots);
    swap_i(&p->rank_len, &t->rank_len);
}

/* The pushes since `pushes` (seq_t.undo_pushes then) are the transaction's own */
static void txn_mine(seq_t *s, int pushes) {
    int n = s->undo_pushes - pushes;
    for (int k = 0; k < n && k < s->undo_n; k++) s->undo[(s->undo_top + SEQ_UNDO - 1 - k) % SEQ_UNDO].txn = 1;
    s->txn.own += n;
}

/* The transaction's own snapshots leave the ring, which closes up (what other edits pushed since stays) */
static void txn_drop_own(seq_t *s) {
    int base = s->undo_top + SEQ_UNDO - s->undo_n, w = 0;
    for (int i = 0; i < s->undo_n; i++) {
        const seq_snap_t *u = &s->undo[(base + i) % SEQ_UNDO];
        if (u->txn) continue;
        if (w != i) s->undo[(base + w) % SEQ_UNDO] = *u;
        w++;
    }
    s->undo_n = w;
    s->undo_top = (base + w) % SEQ_UNDO;
}

static void txn_ring_back(seq_t *s) {
    memcpy(s->undo, s->txn.ring, sizeof s->undo);
    s->undo_n = s->txn.undo_n, s->undo_top = s->txn.undo_top;
}

/* The transaction's undo steps go: the ring as at open (a push may have dropped its oldest) when only they were pushed, else
 * just them */
static void txn_unpush(seq_t *s) {
    if (s->undo_pushes - s->txn.pushes0 == s->txn.own) txn_ring_back(s);
    else txn_drop_own(s);
}

/* The saved progression back, revoiced */
static void txn_restore_prog(seq_t *s) {
    const seq_txn_t *t = &s->txn;
    seq_part_t *p = &s->part[t->part];
    memcpy(p->prog, t->prog, sizeof t->prog);   /* 0.24 the real slots (the change points' are cp's) */
    memcpy(p->src, t->src, sizeof t->src);
    memcpy(p->colour, t->colour, sizeof t->colour);
    p->slot_lock = t->lock;
    p->prog_len = t->prog_len, p->filled_slots = t->filled_slots, p->rank_len = t->rank_len;
    p->phrase = t->phrase;
    revoice(s, p);
    regen_harmony(s, p);
}

static void txn_cancel(seq_t *s) {
    seq_txn_t *t = &s->txn;
    seq_part_t *p = &s->part[t->part];
    if (!t->kind) return;
    if (t->kind == CVIEW_VOICE) {
        vox_write(p, t->vx);
        colour_changed(s, p);
    } else if (rhythm_txn(t)) {   /* 0.18: every drawn bar and lock as at open (the knobs stay); 0.24 an open bar's PHRASE edits too */
        memcpy(p->rb[dl_of(t->lane)], t->rb, sizeof t->rb);
        p->lw[dl_of(t->lane)] = t->lw;   /* 0.24 a pick's or try's OWN undone */
        if (t->kind != CVIEW_BAR) memcpy(p->hoct, t->hoct, sizeof p->hoct);   /* 0.24 final review: the octaves its tries took (an open bar's: phrase_swap) */
        if (t->kind == CVIEW_BAR && phrase_swap(p, t)) {   /* the chords: the lanes that follow them (the Bass's own notes render below) */
            revoice(s, p);
            regen_harmony(s, p);
        }
        if (lane_drawn(p, t->lane)) render_rhythm(s, p, t->lane);
    } else if (t->kind == TXN_BSTYLE) {   /* 0.19: the note settings as at open */
        txn_swap(s);
        bass_notes_changed(s, p);
    } else if (t->kind == TXN_MPICK || t->kind == TXN_MEDIT) {   /* 0.20: the SWAP block, or the editor's bar, as at open */
        txn_swap(s);
        if (mel_jam(p)) render_mel(s, p);
    } else txn_restore_prog(s);
    txn_unpush(s);
    t->kind = 0, t->stale = 0, t->sub = 0;
}

/* A stale transaction is cancelled (UI thread) */
static void txn_unstale(seq_t *s) {
    if (s->txn.kind && s->txn.stale) txn_cancel(s);
}

/* The open transaction of `kind` on `lane` (0.19: kinds are told apart by their lane), after cancelling a stale one (UI thread) */
static int txn_live(seq_t *s, int lane, int kind) {
    txn_unstale(s);
    return s->txn.kind == kind && s->txn.lane == lane;
}

/* 1 when the playing part is as the browser found it */
static int txn_unchanged(const seq_t *s) {
    const seq_txn_t *t = &s->txn;
    const seq_part_t *p = ccur(s);
    int vx[8];
    vox_read(p, vx);
    return !memcmp(vx, t->vx, sizeof vx) && !memcmp(p->prog, t->prog, sizeof t->prog) && !memcmp(p->src, t->src, sizeof t->src) &&
           !memcmp(p->colour, t->colour, sizeof t->colour) && p->slot_lock == t->lock && p->phrase == t->phrase && p->rank_len == t->rank_len &&
           p->filled_slots == t->filled_slots;
}

static void rsub_close(seq_t *s, int done);

/* 0.19 the STYLE browser's DONE keeps its undo step (its open's): that step's UNDO keeps the knobs turned in the browser. 0.19.1:
 * the step at txn.bstep, while it is in the ring and still the browser's own (its only Bass step that restores the knobs) */
static void bstyle_keep_turns(seq_t *s) {
    seq_snap_t *u = &s->undo[s->txn.bstep];
    if ((s->undo_top + SEQ_UNDO - 1 - s->txn.bstep) % SEQ_UNDO < s->undo_n && u->txn && u->lane == LANE_BASS && u->bkeep & BKEEP_KNOBS)
        u->bkeep |= (unsigned char)(s->txn.bturned * BKEEP_TURNED);
}

/* DONE: what was picked or tried stays; a browser that changed nothing leaves no undo step. 0.18: an open bar keeps every
 * step it made (its RHYTHMS browser closed as DONE first) */
static void txn_done(seq_t *s) {
    if (s->txn.kind == CVIEW_BAR && rhythm_txn(&s->txn)) rsub_close(s, 1);   /* 0.20: rhythm_txn, not a Melody lane's (no DLANE row) */
    else if (s->txn.kind == CVIEW_RBROWSE && rhythm_txn(&s->txn)) {
        if (!memcmp(ccur(s)->rb[dl_of(s->txn.lane)], s->txn.rb, sizeof s->txn.rb)) {
            hoct_back(s, s->txn.hoct);   /* 0.24 final review: back to the bars it opened on, so their octaves too (no undo step holds them) */
            txn_unpush(s);
        }
    } else if (s->txn.kind == TXN_BSTYLE) {   /* 0.19: as a browser; knob turns are no change (an undo would keep them) */
        bass_set_t at = ccur(s)->bass.set;
        bset_keep(&at, &s->txn.bset, s->txn.bturned);
        if (!memcmp(&ccur(s)->bass.set, &at, sizeof at)) txn_unpush(s);
        else bstyle_keep_turns(s);
    } else if (s->txn.kind != TXN_MPICK && s->txn.kind != TXN_MEDIT && txn_unchanged(s))
        txn_unpush(s);   /* the editors keep their steps; the ANSWER BLOCK picker has none */
    s->txn.kind = 0;
}

/* Ruling 24: an edit elsewhere keeps an open picker or browser first (DONE); a stale one is cancelled */
void seq_txn_done(seq_t *s) {
    txn_unstale(s);
    if (s->txn.kind) txn_done(s);
}

static int txn_shown(const seq_t *s, int tab);

int seq_cview(const seq_t *s) { return txn_shown(s, TAB_CHORDS) ? (s->txn.sub ? s->txn.sub : s->txn.kind) : s->tv[TAB_CHORDS].view; }

/* 0.17 the CHORDS strip: 4 chords a page. FOLLOW running shows the playing slot's page and slot (slot 0 stopped); a paging pause
 * (paging) holds hold_page and csel; an open picker or browser holds what the strip showed when it opened (snap_*), and its
 * close needs nothing: with the transaction gone the getters read paging or running again. FOLLOW off: page_hand and csel.
 * 0.18 (ruling R22): the RHYTHM views (the user's RHYTHM view, an open bar, the RHYTHMS browser) page by bars, 4 a page; the others
 * by chord slots. A view change converts page_hand and hold_page by the page's first bar; an opening picker or browser holds the
 * page in its own view's units. 0.19: per tab (seq_t.tv): a transaction is shown on, and holds, its own lane's tab only. */
#define STRIP_N 4

static int tab_ok(int tab) { return tab >= 0 && tab < TAB_COUNT; }   /* a tab the strip accessors take (TAB_*) */

/* The open transaction is shown on `tab` (its lane's); a stale one reads as closed */
static int txn_shown(const seq_t *s, int tab) { return s->txn.kind && !s->txn.stale && tab_of_lane(s->txn.lane) == tab; }

/* 1 when the view of an open transaction `kind` on `tab` (0: none, the tab's user view) pages by bars */
static int bar_paged(const seq_t *s, int tab, int kind) {
    if (tab == TAB_BASS || tab == TAB_MEL) return 1;   /* 0.19: the BASS tab's strip is bars on both its pages (0.20: the MELODY tab's too) */
    return kind ? kind == CVIEW_BAR || kind == CVIEW_RBROWSE : s->tv[tab].view == CVIEW_RHYTHM;
}

int seq_strip_by_bars(const seq_t *s, int tab) {
    return !tab_ok(tab) ? 0 : bar_paged(s, tab, txn_shown(s, tab) ? s->txn.kind : 0);
}

/* A page in bars (to_bars) or chord slots from one in the other's units, by its first bar */
static int page_convert(const seq_t *s, int page, int from_bars, int to_bars) {
    int cl = ccur(s)->chord_len, bar = from_bars ? page * STRIP_N : page * STRIP_N * cl / SEQ_STEPS;
    return to_bars ? bar / STRIP_N : bar * SEQ_STEPS / cl / STRIP_N;
}

int seq_strip_pages(const seq_t *s, int tab) {
    int n = seq_strip_by_bars(s, tab) ? ccur(s)->bars : seq_slots(s);
    return clampi((n + STRIP_N - 1) / STRIP_N, 1, SEQ_MAX_PROG);
}

static int run_slot(const seq_t *s) {
    int k = seq_cur_slot(s);
    return clampi(k, 0, seq_slots(s) - 1);   /* stopped: -1, slot 0 */
}

/* 0.19 (final review): the BASS tab's centre is a bar (its strip is bars; a slot would put a 2-bar chord's second bar out of
 * reach), the CHORDS tab's a chord slot: how many there are, and the playing one (stopped: bar 1, slot 0) */
static int bar_centred(int tab) { return tab == TAB_BASS || tab == TAB_MEL; }   /* 0.20: the MELODY tab's centre is a bar too */
static int centre_count(const seq_t *s, int tab) { return bar_centred(tab) ? ccur(s)->bars : seq_slots(s); }
static int run_centre(const seq_t *s, int tab) { return bar_centred(tab) ? seq_cur_bar(s) : run_slot(s); }

int seq_strip_page(const seq_t *s, int tab) {
    const seq_tabview_t *v;
    int run, p;
    if (!tab_ok(tab)) return 0;
    v = &s->tv[tab];
    run = seq_strip_by_bars(s, tab) ? seq_cur_bar(s) : run_slot(s);   /* stopped: bar 1, slot 0 */
    p = !s->follow ? v->page_hand : txn_shown(s, tab) ? v->snap_page : v->paging ? v->hold_page : run / STRIP_N;
    return clampi(p, 0, seq_strip_pages(s, tab) - 1);
}

int seq_centre_slot(const seq_t *s, int tab) {
    const seq_tabview_t *v;
    int c;
    if (!tab_ok(tab)) return 0;
    v = &s->tv[tab];
    c = !s->follow ? v->csel : txn_shown(s, tab) ? v->snap_centre : v->paging ? v->csel : run_centre(s, tab);
    return clampi(c, 0, centre_count(s, tab) - 1);
}

int seq_follow_paused(const seq_t *s, int tab) {
    return !tab_ok(tab) || !s->follow ? 0 : txn_shown(s, tab) ? 1 : s->tv[tab].paging ? 2 : 0;
}

static void strip_clamp(seq_t *s) {
    for (int tab = 0; tab < TAB_COUNT; tab++) {
        seq_tabview_t *v = &s->tv[tab];
        int last = seq_strip_pages(s, tab) - 1, slot = centre_count(s, tab) - 1;
        v->page_hand = clampi(v->page_hand, 0, last), v->hold_page = clampi(v->hold_page, 0, last);
        v->snap_page = clampi(v->snap_page, 0, last);
        v->csel = clampi(v->csel, 0, slot), v->snap_centre = clampi(v->snap_centre, 0, slot);
        v->cbar = clampi(v->cbar, 0, ccur(s)->bars - 1), v->snap_cbar = clampi(v->snap_cbar, 0, ccur(s)->bars - 1);   /* 0.24 */
    }
}

static void strip_hold_open(seq_t *s, int tab, int kind) {
    seq_tabview_t *v;
    if (!tab_ok(tab)) return;   /* a lane with no tab (tab_of_lane -1) */
    v = &s->tv[tab];
    v->snap_page = page_convert(s, seq_strip_page(s, tab), seq_strip_by_bars(s, tab), bar_paged(s, tab, kind));
    v->snap_centre = seq_centre_slot(s, tab);
    v->snap_cbar = seq_centre_bar(s, tab);   /* 0.24 */
}

/* FOLLOW off keeps the page and centre on show, and ends a paging pause (every tab) */
static void strip_follow_off(seq_t *s) {
    for (int tab = 0; tab < TAB_COUNT; tab++) {
        seq_tabview_t *v = &s->tv[tab];
        v->page_hand = seq_strip_page(s, tab), v->csel = seq_centre_slot(s, tab), v->cbar = seq_centre_bar(s, tab);
        v->paging = 0;
    }
}

static void paging_off(seq_t *s) {
    for (int tab = 0; tab < TAB_COUNT; tab++) s->tv[tab].paging = 0;
}

void seq_strip_turn(seq_t *s, int tab, int delta) {
    seq_tabview_t *v;
    int n, p;
    if (!tab_ok(tab)) return;
    v = &s->tv[tab];
    n = seq_strip_pages(s, tab);
    txn_unstale(s);   /* a stale picker or browser is cancelled first */
    if (n < 2 || (s->follow && txn_shown(s, tab))) return;
    p = pmod(seq_strip_page(s, tab) + delta, n);
    if (!s->follow) v->page_hand = p;
    else {
        if (!v->paging) v->csel = seq_centre_slot(s, tab), v->cbar = seq_centre_bar(s, tab);   /* the pause starts on the chord shown */
        v->paging = 1, v->hold_page = p;
    }
}

void seq_back_to_playing(seq_t *s, int tab) {
    if (tab_ok(tab)) s->tv[tab].paging = 0;
}

void seq_set_centre(seq_t *s, int tab, int slot) {
    if (!tab_ok(tab) || slot < 0 || slot >= centre_count(s, tab)) return;
    s->tv[tab].csel = slot;
    if (!bar_centred(tab)) s->tv[tab].cbar = clampi(slot * ccur(s)->chord_len / SEQ_STEPS, 0, ccur(s)->bars - 1);   /* 0.24 its first bar */
}

/* 0.24 (ruling 18) seq_core.h: the CHORDS centre as a bar, read where seq_centre_slot reads the slot */
int seq_centre_bar(const seq_t *s, int tab) {
    const seq_tabview_t *v;
    int c;
    if (!tab_ok(tab)) return 0;
    if (bar_centred(tab)) return seq_centre_slot(s, tab);
    v = &s->tv[tab];
    c = !s->follow ? v->cbar : txn_shown(s, tab) ? v->snap_cbar : v->paging ? v->cbar : seq_cur_bar(s);
    return clampi(c, 0, ccur(s)->bars - 1);
}

void seq_set_centre_bar(seq_t *s, int tab, int bar) {
    if (!tab_ok(tab) || bar < 0 || bar >= ccur(s)->bars) return;
    if (bar_centred(tab)) {
        seq_set_centre(s, tab, bar);
        return;
    }
    s->tv[tab].csel = clampi(bar * SEQ_STEPS / ccur(s)->chord_len, 0, seq_slots(s) - 1);
    s->tv[tab].cbar = bar;
}

void seq_strip_show_bar(seq_t *s, int tab, int bar) {
    seq_tabview_t *v;
    int page, c;
    if (!tab_ok(tab) || bar < 0 || bar >= ccur(s)->bars) return;
    txn_unstale(s);
    if (s->follow && txn_shown(s, tab)) return;
    v = &s->tv[tab];
    page = page_convert(s, bar / STRIP_N, 1, seq_strip_by_bars(s, tab));
    c = bar_centred(tab) ? bar : clampi(bar * SEQ_STEPS / ccur(s)->chord_len, 0, seq_slots(s) - 1);
    if (!s->follow) v->page_hand = page;
    else v->paging = 1, v->hold_page = page;
    v->csel = c, v->cbar = bar;   /* 0.24 (ruling 18): the bar itself on CHORDS too */
    strip_clamp(s);
}

void seq_set_cview(seq_t *s, int v) {
    seq_tabview_t *t = &s->tv[TAB_CHORDS];
    int was;
    if (v != CVIEW_CHORDS && v != CVIEW_RHYTHM && v != CVIEW_JAM) return;   /* 0.22 JAM; 0.24 FIX retired */
    if (txn_live(s, LANE_CHORDS, CVIEW_BAR)) txn_done(s);   /* ruling R32: an open bar (its COPY TO... or RHYTHMS too) is kept, as DONE */
    if (s->txn.lane == LANE_CHORDS) txn_cancel(s);   /* 0.19: the CHORDS tab's; the BASS tab's stays open */
    was = seq_strip_by_bars(s, TAB_CHORDS);
    t->view = v;
    if (seq_strip_by_bars(s, TAB_CHORDS) == was) return;
    t->page_hand = page_convert(s, t->page_hand, was, !was), t->hold_page = page_convert(s, t->hold_page, was, !was);
    strip_clamp(s);
}

int seq_key_lit(const seq_t *s, int midi) {
    const seq_keys_t *k = &s->keys;
    if (midi < SEQ_KEY_LO || midi > SEQ_KEY_HI) return 0;
    return (k->lit >> (midi - SEQ_KEY_LO) & 1) ? 1 + (int)(k->fold >> (midi - SEQ_KEY_LO) & 1) : 0;
}

int seq_key_edge(const seq_t *s, int side) { return side == 0 || side == 1 ? s->keys.edge[side] : 0; }

/* A browser's open pushes the one undo snapshot that DONE keeps and CANCEL removes; its tries push none */
void seq_browse_open(seq_t *s, int kind) {
    const seq_part_t *p;
    if (kind != CVIEW_PROG && kind != CVIEW_VOICE) return;
    act(s);
    txn_begin(s, LANE_CHORDS, kind, -1);
    p = cur(s);
    if (kind == CVIEW_PROG) push_undo(s, -1);
    else {
        push_voicing_snap(s);
        if (p->vstyle >= 0) s->tv[TAB_CHORDS].browse_genre = p->vstyle / (VOICING_NSTYLES / VOICING_NGENRES);
    }
    txn_mine(s, s->txn.pushes0);
}

static int bar_sub(seq_t *s, int lane, int sub);

/* 0.19: the RHYTHMS browser closes on either drawn lane (one transaction at a time: the open one's lane) */
void seq_browse_done(seq_t *s) {
    int l;
    txn_unstale(s);
    l = s->txn.lane;
    if (txn_live(s, LANE_CHORDS, CVIEW_PROG) || txn_live(s, LANE_CHORDS, CVIEW_VOICE) || txn_live(s, l, CVIEW_RBROWSE)) txn_done(s);
    else if (bar_sub(s, l, CVIEW_RBROWSE)) rsub_close(s, 1);
}

void seq_browse_cancel(seq_t *s) {
    int l;
    txn_unstale(s);
    l = s->txn.lane;
    if (txn_live(s, LANE_CHORDS, CVIEW_PROG) || txn_live(s, LANE_CHORDS, CVIEW_VOICE) || txn_live(s, l, CVIEW_RBROWSE)) txn_cancel(s);
    else if (bar_sub(s, l, CVIEW_RBROWSE)) rsub_close(s, 0);
}

/* Preset i's phrase in the unlocked slots (borrows and colours go), revoiced */
static void prog_fill(seq_t *s, seq_part_t *p, int i) {
    fill_phrase(p, THEORY_PRESETS[i].len, THEORY_PRESETS[i].deg, NULL, NULL);
    p->rank_len = THEORY_PRESETS[i].len;
    revoice(s, p);
    regen_harmony(s, p);
}

void seq_prog_try(seq_t *s, int i) {
    seq_part_t *p = cur(s);
    if (!txn_live(s, LANE_CHORDS, CVIEW_PROG) || i < 0 || i >= THEORY_NPRESETS) return;
    act(s);
    prog_fill(s, p, i);
    for (int k = 0; k < p->prog_len; k++)   /* a tap is heard, stopped too: the first slot it changed */
        if (!slot_locked(p, k)) {
            seq_audition_chord(s, k);
            break;
        }
}

/* Ruling 23: PROGRESSION's arrows outside the browser, a try that stays: locks kept, one undo step */
void seq_prog_step(seq_t *s, int i) {
    prog_view_t y = part_view(ccur(s));
    int k = 0;
    while (k < y.len && (y.lock >> k & 1)) k++;
    if (i < 0 || i >= THEORY_NPRESETS || k == y.len) return;   /* every slot locked: nothing to fill */
    seq_txn_done(s);
    act(s);
    push_undo(s, -1);
    prog_fill(s, cur(s), i);
}

int seq_prog_preset_shown(const seq_t *s) { return seq_part_prog_preset_shown(s, s->cur_part); }

int seq_part_prog_preset_shown(const seq_t *s, int part) {
    prog_view_t v = part_view(&s->part[part >= 0 && part < SEQ_NPARTS ? part : s->cur_part]);
    int n = v.phrase > v.len ? v.phrase : v.len, free_slots = 0;
    for (int i = 0; i < n; i++) free_slots += !(v.lock >> i & 1);
    if (!free_slots) return -1;
    for (int pass = 0; pass < 2; pass++)   /* the preset whose length is the phrase's first */
        for (int i = 0; i < THEORY_NPRESETS; i++) {
            const theory_preset_t *r = &THEORY_PRESETS[i];
            int k = 0;
            if ((r->len == v.phrase) != (pass == 0)) continue;
            while (k < n && ((v.lock >> k & 1) || (v.prog[k] == r->deg[k % r->len] && !v.src[k] && !v.colour[k]))) k++;
            if (k == n) return i;
        }
    return -1;
}

void seq_voice_try(seq_t *s, int style) {
    if (!txn_live(s, LANE_CHORDS, CVIEW_VOICE) || style < 0 || style >= VOICING_NSTYLES) return;
    voicing_apply(s, style);
    seq_audition_chord(s, 0);   /* heard, stopped too */
}

int seq_browse_genre(const seq_t *s) { return s->tv[TAB_CHORDS].browse_genre; }

void seq_set_browse_genre(seq_t *s, int g) {
    if (g >= 0 && g < VOICING_NGENRES) s->tv[TAB_CHORDS].browse_genre = g;
}

/* 0.18 RHYTHM's bar editing (seq_core.h). The bar is a transaction of kind CVIEW_BAR (slot: the bar); its COPY TO... picker and
 * RHYTHMS browser are sub-views of it (txn.sub). Its paints, locks, copies and picks are its own undo steps (txn_mine), so CANCEL
 * (txn_cancel: rb as at open) drops them and UNDO steps them back. Every change renders the bars it touched. 0.19: on either drawn
 * lane (txn.lane), each with its own bars (rb[dl_of(lane)]) and library. */

/* The open bar on `lane`, its sub-view `sub` (0: the bar itself), after cancelling a stale one */
static int bar_sub(seq_t *s, int lane, int sub) { return txn_live(s, lane, CVIEW_BAR) && s->txn.sub == sub; }

/* A drawn lane's undo step; with a bar or RHYTHMS browser open (the same lane's), its own */
static void push_rhythm(seq_t *s, int lane) {
    int pushes = s->undo_pushes;
    act(s);
    push_undo(s, lane);
    if (rhythm_txn(&s->txn)) txn_mine(s, pushes);
}

/* The transaction's newest undo step goes when it is its own (the bar's RHYTHMS browser's open) */
static void txn_pop_own(seq_t *s) {
    int top = (s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO;
    if (!s->undo_n || !s->undo[top].txn) return;
    s->undo_top = top;
    s->undo_n--;
}

/* The open bar's sub-view closes. Its RHYTHMS browser: DONE keeps the bar (its open's undo step only when the bar changed),
 * CANCEL puts the bar back as the browser found it. COPY TO...: nothing to put back. */
static void rsub_close(seq_t *s, int done) {
    seq_txn_t *t = &s->txn;
    seq_part_t *p = &s->part[t->part];
    rbar_t *r = &p->rb[dl_of(t->lane)][t->slot];
    if (t->sub == CVIEW_RBROWSE) {
        int same = !memcmp(r, &t->sub_rb, sizeof *r);
        if (!done) {
            *r = t->sub_rb;
            memcpy(p->hoct[t->slot], t->sub_hoct, sizeof t->sub_hoct);   /* 0.24 final review: the octaves its tries took */
            render_one(s, p, t->lane, t->slot);
        } else if (same) {
            signed char h[SEQ_MAX_BARS][16];
            memcpy(h, p->hoct, sizeof h);
            memcpy(h[t->slot], t->sub_hoct, sizeof t->sub_hoct);
            hoct_back(s, h);   /* the bar it opened on: its octaves too (its undo step goes) */
        }
        if (!done || same) txn_pop_own(s);
    }
    t->sub = 0, t->copy_mask = 0;
}

/* The newest undo step is the open bar's own (UNDO steps it back, the bar stays open); 0.20: or the Melody bar editor's */
static int rbar_own_top(const seq_t *s) {
    return (s->txn.kind == CVIEW_BAR || s->txn.kind == TXN_MEDIT) && !s->txn.stale && !s->txn.sub && s->undo_n && s->undo[(s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO].txn;
}

/* UNDO with the bar's COPY TO... or RHYTHMS open is that view's CANCEL; 1 when it was */
static int rbar_undo(seq_t *s) {
    if (s->txn.kind != CVIEW_BAR || s->txn.stale || !s->txn.sub) return 0;
    rsub_close(s, 0);
    return 1;
}

/* The tab's strip holds the bar's page (ruling R22: by bars), and as the centre the bar's first slot (0.19.1: on the BASS tab, whose
 * centre is a bar, the bar) */
static void strip_hold_bar(seq_t *s, int tab, int bar) {
    int slot = clampi(bar * SEQ_STEPS / ccur(s)->chord_len, 0, seq_slots(s) - 1);
    s->tv[tab].snap_page = bar / STRIP_N, s->tv[tab].snap_centre = bar_centred(tab) ? bar : slot;
    s->tv[tab].snap_cbar = bar;   /* 0.24 (ruling 18) */
}

static const seq_step_t *bass_step(const seq_part_t *p, int bar, int step) { return &p->lane[LANE_BASS].steps[bar * SEQ_STEPS + step]; }

/* 0.24 PHRASE (seq_core.h): the open bar's selected step (txn.bsel); on the Bass a sounding step's note sets the octave (txn.boct) */
static void rbar_select(seq_t *s, int step) {
    const seq_step_t *x = bass_step(cur(s), s->txn.slot, step);
    s->txn.bsel = step;
    if (s->txn.lane == LANE_BASS && x->on) s->txn.boct = clampi(x->abs / 12 - 1, 1, 4);
}

void seq_rbar_open(seq_t *s, int lane, int bar) {
    unsigned char c[16];
    int i = 0;
    if (bar < 0 || bar >= cur(s)->bars || !lane_drawn(cur(s), lane)) return;
    seq_txn_done(s);
    act(s);
    txn_begin(s, lane, CVIEW_BAR, bar);
    strip_hold_bar(s, DLANE[dl_of(lane)].tab, bar);
    drawn_codes(cur(s), dl_of(lane), bar, c);   /* 0.24 what plays: LOCK WITH's when it follows */
    while (i < SEQ_STEPS - 1 && !rhythm_is_hit(c[i])) i++;
    s->txn.boct = 1;
    rbar_select(s, rhythm_is_hit(c[i]) ? i : 0);   /* 0.24 its first hit */
}

int seq_rbar_bar(const seq_t *s, int lane) {
    return s->txn.kind == CVIEW_BAR && s->txn.lane == lane && !s->txn.stale ? s->txn.slot : -1;
}

int seq_rbar_sel(const seq_t *s, int lane) { return seq_rbar_bar(s, lane) >= 0 ? s->txn.bsel : -1; }

void seq_rbar_select(seq_t *s, int lane, int step) {
    if (step >= 0 && step < SEQ_STEPS && bar_sub(s, lane, 0)) rbar_select(s, step);
}

/* 0.24 an open PHRASE bar's edit: its undo step, its own. PH_LANE the lane's snapshot, PH_PROG the progression's; both: the lane's,
 * whose undo restores the progression too (prog_too) */
enum { PH_LANE = 1, PH_PROG = 2 };
static void phrase_push(seq_t *s, int what) {
    int pushes = s->undo_pushes;
    act(s);
    push_snap(s, what & PH_LANE ? s->txn.lane : -1, 0);
    s->undo[(s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO].prog_too = (unsigned char)(what == (PH_LANE | PH_PROG));
    txn_mine(s, pushes);
}

int seq_rbar_paint(seq_t *s, int lane, int step, int code) {
    seq_part_t *p = cur(s);
    rbar_t *r;
    unsigned char c[16];
    int was;
    if (step < 0 || step >= SEQ_STEPS || code < RC_REST || code > RC_HOLD || !bar_sub(s, lane, 0)) return 0;
    r = &p->rb[dl_of(lane)][s->txn.slot];
    drawn_codes(p, dl_of(lane), s->txn.slot, c);   /* 0.24 a LOCK WITH bar's: the paint starts from them (and makes it ed) */
    was = c[step];
    if (code == RC_HOLD && was != RC_HOLD && (!step || c[step - 1] == RC_REST)) return 0;   /* a HOLD needs a hit or hold before */
    if (was != code) {   /* 0.24: the paint a step has only selects it */
        phrase_push(s, PH_LANE);
        memcpy(r->c, c, 16);
        r->c[step] = (unsigned char)code;
        if (code == RC_REST) rhythm_cut_holds(r->c, step);   /* no hold after a rest */
        if (lane == LANE_CHORDS && !rhythm_is_hit(code)) p->hoct[s->txn.slot][step] = 0;   /* 0.24 a cleared hit's octave (its point and fix stay) */
        r->ed = 1;
        render_one(s, p, lane, s->txn.slot);
    }
    rbar_select(s, step);
    return was != code;
}

/* 0.24 the real slots that start in bar b (bit i: slot i) */
static uint64_t bar_slot_bits(const seq_part_t *p, int b) {
    uint64_t m = 0;
    for (int i = (b * SEQ_STEPS + p->chord_len - 1) / p->chord_len; i < p->prog_len && i * p->chord_len < (b + 1) * SEQ_STEPS; i++)
        m |= SLOT_BIT(i);
    return m;
}

/* 0.24 the slots of mask picked (slot_lock; a Knobs one keeps the COLOUR level it plays, as seq_toggle_slot_lock), revoiced */
static void pick_slots(seq_t *s, seq_part_t *p, uint64_t mask) {
    for (int i = 0; i < p->prog_len; i++)
        if ((mask >> i & 1) && !slot_locked(p, i)) {
            p->colour[i] = (unsigned char)lock_colour(s, p, i, p->prog[i], p->src[i], p->colour[i]);
            p->slot_lock |= SLOT_BIT(i);
        }
    revoice(s, p);
    regen_harmony(s, p);
}

void seq_rbar_lock(seq_t *s, int lane) {
    seq_part_t *p = cur(s);
    rbar_t *r;
    int b;
    if (!bar_sub(s, lane, 0)) return;
    b = s->txn.slot;
    r = &p->rb[dl_of(lane)][b];
    phrase_push(s, PH_LANE | PH_PROG);
    if (r->lock) {   /* UNLOCK: the rhythm and note locks only (picked slots stay) */
        r->lock = 0;
        if (lane == LANE_BASS) p->bass.nlock = (unsigned short)(p->bass.nlock & ~(1u << b));
    } else {
        if (bar_follows(p, dl_of(lane), b)) drawn_codes(p, dl_of(lane), b, r->c), r->ed = 1;   /* 0.24 LOCK freezes LOCK WITH's codes: its own */
        r->lock = 1;
        if (lane == LANE_BASS && !(p->bass.nlock >> b & 1)) {   /* the settings it plays now, frozen */
            p->bass.frozen[b] = p->bass.set;
            p->bass.nlock |= (unsigned short)(1u << b);
        }
        if (bar_slot_bits(p, b) & ~p->slot_lock) pick_slots(s, p, bar_slot_bits(p, b));
    }
    render_one(s, p, lane, b);
}

int seq_bar_picked(const seq_t *s, int lane, int bar) {
    const seq_part_t *p = ccur(s);
    int d = dl_of(lane);
    return d >= 0 && bar >= 0 && bar < p->bars && !jam_locked(p, d, bar) && (p->slot_lock & bar_slot_bits(p, bar)) != 0;
}

/* The lane's bars of mask (bit b: bar b) in the loop other than the source bar src, locked (locked 1) or not (0): with locked 0 the
 * bars a copy from src may replace. 0.24 locked as for JAM (jam_locked): its rhythm lock, or on the Bass its note lock alone. A drawn
 * lane only (rbars_init) */
static unsigned copy_bars(const seq_t *s, int lane, int src, unsigned mask, int locked) {
    const seq_part_t *p = ccur(s);
    unsigned m = 0;
    for (int b = 0; b < p->bars; b++)
        if ((mask >> b & 1) && b != src && !jam_locked(p, dl_of(lane), b) == !locked) m |= 1u << b;
    return m;
}
static unsigned copy_targets(const seq_t *s, unsigned mask) { return copy_bars(s, s->txn.lane, s->txn.slot, mask, 0); }

/* 0.24 what a copy with + CHORDS / + NOTES writes into bar b from bar a besides the rhythm: the Chords' change points and hit octaves,
 * the Bass's fixed notes; `apply` 0 only tells (1 when bar b's differ) */
static int more_differs(const seq_part_t *p, int lane, int b, int a) {
    if (lane == LANE_CHORDS) return memcmp(p->cp[b], p->cp[a], sizeof p->cp[b]) || memcmp(p->hoct[b], p->hoct[a], sizeof p->hoct[b]);
    return memcmp(&p->bass.fix[b * SEQ_STEPS], &p->bass.fix[a * SEQ_STEPS], SEQ_STEPS) != 0;
}
static int copy_more(seq_part_t *p, int lane, int b, int a, int apply) {
    int d = more_differs(p, lane, b, a);
    if (!apply) return d;
    if (lane == LANE_CHORDS) memcpy(p->cp[b], p->cp[a], sizeof p->cp[b]), memcpy(p->hoct[b], p->hoct[a], sizeof p->hoct[b]);
    else memcpy(&p->bass.fix[b * SEQ_STEPS], &p->bass.fix[a * SEQ_STEPS], SEQ_STEPS);
    return d;
}

/* The open bar's codes, from and ed (not its lock) over the bars of mask it may replace (0.24 with copy_what its copy_more too); one
 * undo step when one changes */
static void rbar_copy(seq_t *s, unsigned mask) {
    seq_part_t *p = cur(s);
    int lane = s->txn.lane, more = s->txn.copy_what, a = s->txn.slot, d = dl_of(lane), ed;
    rbar_t *rb = p->rb[d];
    const rbar_t *src = &rb[a];
    unsigned char c[16];
    unsigned t = copy_targets(s, mask), m = 0;
    drawn_codes(p, d, a, c);   /* 0.24 what the bar plays: a LOCK WITH bar's codes go out as the targets' own (ed) */
    ed = src->ed || bar_follows(p, d, a);
    for (int b = 0; b < SEQ_MAX_BARS; b++) {
        const rbar_t *r = &rb[b];
        if ((t >> b & 1) && (memcmp(r->c, c, 16) || r->from != src->from || r->ed != ed || (more && copy_more(p, lane, b, a, 0))))
            m |= 1u << b;
    }
    if (!m) return;
    phrase_push(s, more && lane == LANE_CHORDS ? PH_LANE | PH_PROG : PH_LANE);
    for (int b = 0; b < SEQ_MAX_BARS; b++) {
        if (!(m >> b & 1)) continue;
        memcpy(rb[b].c, c, 16);
        rb[b].from = src->from, rb[b].ed = (unsigned char)ed;
        if (more) copy_more(p, lane, b, a, 1);
    }
    if (more && lane == LANE_CHORDS) {   /* the points: revoiced, the lanes that follow the chords rendered */
        revoice(s, p);
        regen_harmony(s, p);
    }
    render_mask(s, p, lane, m, 1);   /* 0.22 a queued JAM block: once */
}

void seq_rbar_copy(seq_t *s, int lane, unsigned mask) {
    if (txn_live(s, lane, CVIEW_BAR) && s->txn.sub != CVIEW_RBROWSE) rbar_copy(s, mask);
}

int seq_rbar_copy_counts(const seq_t *s, int lane, unsigned mask, int *replaced_differing, int *skipped_locked) {
    const seq_part_t *p = ccur(s);
    int bar = seq_rbar_bar(s, lane), n = 0, rep = 0, skip = 0, more = seq_rcopy_what(s, lane);
    if (bar >= 0) {   /* no bar open: no counts */
        unsigned char a[16], c[16];
        unsigned to = copy_bars(s, lane, bar, mask, 0), locked = copy_bars(s, lane, bar, mask, 1);
        drawn_codes(p, dl_of(lane), bar, a);   /* 0.24 as they play (LOCK WITH's) */
        for (int b = 0; b < p->bars; b++) {
            n += to >> b & 1, skip += locked >> b & 1;
            drawn_codes(p, dl_of(lane), b, c);
            rep += (to >> b & 1) && (memcmp(c, a, 16) || (more && more_differs(p, lane, b, bar)));   /* release fix: + CHORDS / + NOTES too */
        }
    }
    if (replaced_differing) *replaced_differing = rep;
    if (skipped_locked) *skipped_locked = skip;
    return n;
}

void seq_rbar_cancel(seq_t *s, int lane) {
    if (txn_live(s, lane, CVIEW_BAR)) txn_cancel(s);
}

void seq_rbar_done(seq_t *s, int lane) {
    if (txn_live(s, lane, CVIEW_BAR)) txn_done(s);
}

void seq_rcopy_open(seq_t *s, int lane) {
    if (bar_sub(s, lane, 0)) s->txn.sub = CVIEW_COPY, s->txn.copy_mask = 0;
}

void seq_rcopy_toggle(seq_t *s, int lane, int bar) {
    if (bar >= 0 && bar < SEQ_MAX_BARS && bar_sub(s, lane, CVIEW_COPY)) s->txn.copy_mask ^= copy_targets(s, 1u << bar);
}

void seq_rcopy_set_all(seq_t *s, int lane, int on) {
    if (bar_sub(s, lane, CVIEW_COPY)) s->txn.copy_mask = on ? copy_targets(s, ~0u) : 0;
}

unsigned seq_rcopy_mask(const seq_t *s, int lane) {
    return seq_rbar_bar(s, lane) >= 0 && s->txn.sub == CVIEW_COPY ? s->txn.copy_mask : 0;
}

void seq_rcopy_go(seq_t *s, int lane) {
    if (!bar_sub(s, lane, CVIEW_COPY) || !s->txn.copy_mask) return;   /* PICK BARS: nothing */
    rbar_copy(s, s->txn.copy_mask);
    rsub_close(s, 1);
}

void seq_rcopy_close(seq_t *s, int lane) {
    if (bar_sub(s, lane, CVIEW_COPY)) rsub_close(s, 0);
}

void seq_rcopy_set_what(seq_t *s, int lane, int what) {
    if ((what == 0 || what == 1) && txn_live(s, lane, CVIEW_BAR)) s->txn.copy_what = what;
}

int seq_rcopy_what(const seq_t *s, int lane) { return seq_rbar_bar(s, lane) >= 0 ? s->txn.copy_what : 0; }

unsigned seq_rcopy_same_place(const seq_t *s, int lane) {
    int bar = seq_rbar_bar(s, lane);
    unsigned m = 0;
    for (int b = bar % 4; bar >= 0 && b < SEQ_MAX_BARS; b += 4) m |= 1u << b;
    return bar >= 0 ? copy_bars(s, lane, bar, m, 0) : 0;
}

/* 0.24 PHRASE's CHORDS picker (seq_core.h) on the open Chords bar's selected step */

/* A chord as a slot holds it (deg, src) and its type (seq_rbar_type's: a point's own, else the slot colour's, Knobs on a Triad; 0.24.2 a
 * colour no TYPE picks: its own type, so a point made of it plays it) */
typedef struct { int deg, src, type; } ph_chord_t;
static ph_chord_t slot_chord(const seq_part_t *p, int slot) {
    ph_chord_t c = {p->prog[slot], p->src[slot], 0};
    if (slot >= SEQ_MAX_PROG) c.type = p->cp[(slot - SEQ_MAX_PROG) / SEQ_CP_MAX][(slot - SEQ_MAX_PROG) % SEQ_CP_MAX].type;
    else
        for (int t = 1; t < CP_TYPES; t++)
            if (p->colour[slot] == PICK_TYPE[t]) c.type = t;
    return c;
}

/* The point of bar b at its step, -1 none */
static int cp_at(const seq_part_t *p, int b, int step) {
    for (int k = 0; k < SEQ_CP_MAX; k++)
        if (p->cp[b][k].step == step) return k;
    return -1;
}

/* The real slot a pick on step `step` of bar b sets: from the start of a slot that starts in the bar up to and including its first
 * hit (the drawn codes, 0.24 LOCK WITH's), no point between; else -1 (a later step: a change point) */
static int pick_slot_at(const seq_part_t *p, int b, int step) {
    int i = b * SEQ_STEPS + step, r = i / p->chord_len, st = r * p->chord_len - b * SEQ_STEPS, end = st + p->chord_len;
    unsigned char c[16];
    if (st < 0) return -1;   /* a 2-bar slot's second bar */
    drawn_codes(p, DL_CHORDS, b, c);   /* 0.24 what plays: LOCK WITH's when it follows */
    for (int k = st; k <= step; k++)
        if (cp_at(p, b, k) >= 0) return -1;
    for (int k = st; k < step && k < end; k++)
        if (rhythm_is_hit(c[k])) return -1;   /* its first hit is before the step */
    return r % p->prog_len;
}

/* The chord at step `step` of bar b without the step's own point (point k): the point before it in the same real slot (ruling 31: a
 * point in an earlier slot of the bar has ended), else the slot */
static ph_chord_t chord_before(const seq_part_t *p, int b, int step, int k) {
    if (k > 0 && p->cp[b][k - 1].step >= slot_start_in_bar(p, step)) return slot_chord(p, SEQ_MAX_PROG + b * SEQ_CP_MAX + k - 1);
    if (k >= 0) return slot_chord(p, (b * SEQ_STEPS + step) / p->chord_len % p->prog_len);
    return slot_chord(p, slot_at(p, b, step));
}

/* The selected step's chord heard once, at its hit's octave */
static void phrase_audition(seq_t *s) {
    const seq_part_t *p = ccur(s);
    audition_chord_at(s, slot_at(p, s->txn.slot, s->txn.bsel), LANE_OCT[LANE_CHORDS] + p->hoct[s->txn.slot][s->txn.bsel]);
}

/* The real slot's pick (sets, picks, one undo step, none when it is so), heard */
static void phrase_slot(seq_t *s, int slot, int deg, int src, int colour) {
    int pushes = s->undo_pushes;
    seq_set_slot(s, slot, deg, src, colour);
    txn_mine(s, pushes);
    phrase_audition(s);
}

/* The selected step's own point: chord c (one undo step), or none (c NULL); the lanes that follow it rendered, heard */
static void phrase_point(seq_t *s, const ph_chord_t *c) {
    seq_part_t *p = cur(s);
    int b = s->txn.slot, st = s->txn.bsel;
    phrase_push(s, PH_PROG);
    if (c) cp_set(s, p, b, st, c->deg, c->src, c->type);
    else cp_clear(s, p, b, st);
    cp_prune(s, p);   /* 0.24 final review: a later point now playing the chord before it goes, in this undo step */
    regen_harmony(s, p);
    phrase_audition(s);
}

/* The open Chords bar with a step selected (any sub-view closed), else NULL */
static seq_part_t *phrase_chords(seq_t *s) {
    return bar_sub(s, LANE_CHORDS, 0) && s->txn.bsel >= 0 ? cur(s) : NULL;
}

/* 0.24 (Task 7) each returns what it did (RBC_*), for the message line */
int seq_rbar_chord(seq_t *s, int deg, int src) {
    seq_part_t *p = phrase_chords(s);
    int b, st, k, slot;
    ph_chord_t was, now;
    if (!p || src < 0 || src > THEORY_NBORROW || (!src && (deg < 0 || deg > 6))) return RBC_NONE;
    if (src) deg = theory_borrow_degree(s->scale, src);
    b = s->txn.slot, st = s->txn.bsel, k = cp_at(p, b, st), slot = k < 0 ? pick_slot_at(p, b, st) : -1;
    if (slot >= 0) {
        phrase_slot(s, slot, deg, src, p->colour[slot]);
        return RBC_SLOT;
    }
    was = chord_before(p, b, st, k);
    now = k >= 0 ? slot_chord(p, SEQ_MAX_PROG + b * SEQ_CP_MAX + k) : was;
    if (now.deg == deg && now.src == src) {   /* the chord it plays already: only heard */
        phrase_audition(s);
        return RBC_HEARD;
    }
    if (was.deg == deg && was.src == src) {   /* the chord that sounds there without its point: the point goes */
        phrase_point(s, NULL);
        return RBC_GONE;
    }
    if (k < 0 && p->cp[b][SEQ_CP_MAX - 1].step >= 0) return RBC_FULL;   /* a full bar */
    now.deg = deg, now.src = src;
    if (k < 0) now.type = 0;   /* ruling 12: a new point plays as the knobs say until a TYPE is picked; a point keeps its own */
    phrase_point(s, &now);
    return RBC_POINT;
}

int seq_rbar_type(seq_t *s, int type) {
    seq_part_t *p = phrase_chords(s);
    int b, st, k, slot;
    ph_chord_t now;
    if (!p || type < 0 || type > 4) return RBC_NONE;
    b = s->txn.slot, st = s->txn.bsel, k = cp_at(p, b, st), slot = k < 0 ? pick_slot_at(p, b, st) : -1;
    if (slot >= 0) {
        phrase_slot(s, slot, p->prog[slot], p->src[slot], PICK_TYPE[type]);
        return RBC_SLOT;
    }
    now = slot_chord(p, k >= 0 ? SEQ_MAX_PROG + b * SEQ_CP_MAX + k : slot_at(p, b, st));
    if (now.type == type) {   /* as it is: only heard */
        phrase_audition(s);
        return RBC_HEARD;
    }
    if (k < 0 && p->cp[b][SEQ_CP_MAX - 1].step >= 0) return RBC_FULL;   /* a full bar */
    now.type = type;
    phrase_point(s, &now);
    return RBC_POINT;
}

void seq_rbar_hit_oct(seq_t *s, int o) {
    seq_part_t *p = phrase_chords(s);
    unsigned char c[16];
    signed char *h;
    if (!p || o < -2 || o > 2) return;
    drawn_codes(p, DL_CHORDS, s->txn.slot, c);   /* 0.24 a hit as it plays (LOCK WITH's) */
    if (!rhythm_is_hit(c[s->txn.bsel])) return;
    h = &p->hoct[s->txn.slot][s->txn.bsel];
    if (*h != o) {
        phrase_push(s, PH_LANE);
        *h = (signed char)o;
        render_one(s, p, LANE_CHORDS, s->txn.slot);
    }
    phrase_audition(s);
}

int seq_hit_oct(const seq_t *s, int bar, int step) {
    return bar >= 0 && bar < SEQ_MAX_BARS && step >= 0 && step < SEQ_STEPS ? ccur(s)->hoct[bar][step] : 0;
}

int seq_rbar_this_hit_only(seq_t *s) {
    seq_part_t *p = phrase_chords(s);
    unsigned char c[16];
    int b, st, j, end;
    ph_chord_t was;
    if (!p) return RHO_NOT_A_CHANGE;
    b = s->txn.slot, st = s->txn.bsel;
    end = slot_start_in_bar(p, st) + p->chord_len;   /* 0.24 final review (ruling 31): the change lasts to its slot's end at most */
    if (end > SEQ_STEPS) end = SEQ_STEPS;
    drawn_codes(p, DL_CHORDS, b, c);   /* 0.24 the hits as drawn (LOCK WITH's) */
    if (!rhythm_is_hit(c[st]) || !seq_chord_change_at(s, b, st)) return RHO_NOT_A_CHANGE;
    for (j = st + 1; j < end && !rhythm_is_hit(c[j]) && !chord_change_at(s, p, b, j); j++) {}
    if (j == end) return RHO_NO_LATER_HIT;   /* no hit before the next slot (it plays its own chord) */
    if (cp_at(p, b, j) >= 0 || chord_change_at(s, p, b, j)) return RHO_LATER_HAS_OWN;   /* a change before the hit or on it: ended */
    if (p->cp[b][SEQ_CP_MAX - 1].step >= 0) return RHO_FULL;
    was = slot_chord(p, st ? slot_at(p, b, st - 1) : slot_at(p, b + p->bars - 1, SEQ_STEPS - 1));   /* the chord before the change */
    phrase_push(s, PH_PROG);
    cp_set(s, p, b, j, was.deg, was.src, was.type);
    regen_harmony(s, p);
    return 1;
}

/* 0.24 PHRASE's NEW BAR and RESET BAR on the open bar of either lane */

/* Bar b's Bass notes as it plays (0 silent) */
static void bar_notes(const seq_part_t *p, int b, unsigned char out[SEQ_STEPS]) {
    for (int i = 0; i < SEQ_STEPS; i++) out[i] = (unsigned char)seq_abs_note(bass_step(p, b, i));
}

/* 0.16's rule on the Bass: the next variation whose notes differ (up to 8 tries); 1 when found. 0.24.2 with ANTICIPATE on, each try
 * renders the lane on a copy of the part (jam_tmp, as djq_render), so it compares the notes as they play after the pass, as `was` */
static int bass_new_bar(seq_t *s, seq_part_t *p, int b) {
    unsigned char was[SEQ_STEPS], now[SEQ_STEPS], c[16], fill[16], *v = &p->lane[LANE_BASS].bar_var[b];
    char role[SEQ_STEPS];
    bass_key_t k = bass_key(s);
    bar_harm_t h;
    seq_part_t *tmp = &s->jam_tmp;
    int v0 = *v, t, found, an = p->an[DL_BASS] != 0;
    bar_notes(p, b, was);
    render_bar(p, LANE_BASS, b, c, fill);   /* the codes every try reads: a variation moves notes only */
    bar_harm(s, p, b, &k, &h);
    if (an) *tmp = *p;
    for (t = 0; t < 8; t++) {
        *v = (unsigned char)(*v == 255 ? 1 : *v + 1);
        if (an) {
            tmp->lane[LANE_BASS].bar_var[b] = *v;
            lane_bars(s, tmp, LANE_BASS, 1u << b, &k);
            bar_notes(tmp, b, now);
        } else {
            bass_bar_notes(p, b, NULL, &h, c, now, role);   /* the notes alone (0 off a hit): what a render would play */
            bass_cadence(s, p, b, now, role);               /* 0.24 with CADENCE, as the render's */
        }
        if (memcmp(was, now, sizeof was)) break;
    }
    found = *v;
    *v = (unsigned char)v0;
    if (t == 8) return 0;   /* no variation sounds different: nothing rendered, as it was */
    phrase_push(s, PH_LANE);
    *v = (unsigned char)found;
    return 1;
}

/* The Chords: rhythm_newbar at the next version (bar_var) whose codes differ (up to 8 tries), painted in; a hit moved 2 steps earlier
 * takes its octave along, a step no longer a hit loses its own; 1 when found */
static int chords_new_bar(seq_t *s, seq_part_t *p, int b) {
    rbar_t *r = &p->rb[DL_CHORDS][b];
    unsigned char in[16], out[16], *v = &p->lane[LANE_CHORDS].bar_var[b];
    int nv = *v, t;
    drawn_codes(p, DL_CHORDS, b, in);   /* 0.24 from what plays (LOCK WITH's), painted in as its own */
    for (t = 0; t < 8; t++) {
        nv = nv == 255 ? 1 : nv + 1;
        rhythm_newbar(in, nv, out);
        if (memcmp(out, in, 16)) break;
    }
    if (t == 8) return 0;
    phrase_push(s, PH_LANE);
    for (int i = 0; i < SEQ_STEPS; i++) {
        if (!rhythm_is_hit(in[i]) || rhythm_is_hit(out[i])) continue;
        if (i >= 2 && !rhythm_is_hit(in[i - 2]) && rhythm_is_hit(out[i - 2])) p->hoct[b][i - 2] = p->hoct[b][i];
        p->hoct[b][i] = 0;
    }
    memcpy(r->c, out, 16);
    r->ed = 1;
    *v = (unsigned char)nv;
    return 1;
}

int seq_rbar_new_bar(seq_t *s, int lane) {
    seq_part_t *p = cur(s);
    int b;
    if (!bar_sub(s, lane, 0)) return 0;
    b = s->txn.slot;
    if (!(lane == LANE_BASS ? bass_new_bar(s, p, b) : chords_new_bar(s, p, b))) return 0;
    render_one(s, p, lane, b);
    return 1;
}

void seq_rbar_reset_bar(seq_t *s, int lane) {
    static const signed char none[16];
    seq_part_t *p = cur(s);
    rbar_t *r;
    unsigned char lib[16];
    uint64_t picked;
    int b, d = dl_of(lane), more;
    if (!bar_sub(s, lane, 0)) return;
    b = s->txn.slot, r = &p->rb[d][b], picked = p->slot_lock & bar_slot_bits(p, b);
    memcpy(lib, r->c, 16);
    if (r->from >= 0) rhythm_lib_codes(DLANE[d].rlib, r->from, lib);   /* a bar drawn from nothing keeps its codes */
    more = lane == LANE_CHORDS ? p->cp[b][0].step >= 0 || memcmp(p->hoct[b], none, sizeof none) || picked
                               : (p->bass.nlock >> b & 1) || seq_bass_fixed_count(s, b);
    if (!memcmp(lib, r->c, 16) && !r->ed && !r->lock && !p->lane[lane].bar_var[b] && !more) return;   /* already the part's */
    phrase_push(s, lane == LANE_CHORDS ? PH_LANE | PH_PROG : PH_LANE);
    memcpy(r->c, lib, 16);
    r->ed = 0, r->lock = 0;
    p->lane[lane].bar_var[b] = 0;
    if (lane == LANE_CHORDS) {   /* its slots keep their chords, unpicked: the next PROGRESSION pick fills them */
        memset(p->hoct[b], 0, sizeof p->hoct[b]);
        p->slot_lock &= ~picked;
        if (!cp_clear_bar(s, p, b)) revoice(s, p);
        regen_harmony(s, p);
    } else {
        p->bass.nlock = (unsigned short)(p->bass.nlock & ~(1u << b));
        memset(&p->bass.fix[b * SEQ_STEPS], 0, SEQ_STEPS);
    }
    render_one(s, p, lane, b);
}

/* The lane's RHYTHMS browser is open: on its own, or in the open bar */
static int rbrowse_live(seq_t *s, int lane) { return txn_live(s, lane, CVIEW_RBROWSE) || bar_sub(s, lane, CVIEW_RBROWSE); }

/* The lane's library rhythm ri in what a pick fills (the open bar, else every unlocked bar of the loop), rendered; `push`: one undo
 * step when it changes a bar. 1 when it changed one */
static int rhythm_fill_now(seq_t *s, int lane, int ri, int push) {
    seq_part_t *p = cur(s);
    int bar = seq_rbar_bar(s, lane), lo = bar >= 0 ? bar : 0, hi = bar >= 0 ? bar + 1 : p->bars, d = dl_of(lane);
    int follows = bar >= 0 ? bar_follows(p, d, bar) != 0 : p->lw[d] != LW_OWN;   /* 0.24 LOCK WITH would hide the pick */
    if (!fill_rhythm(p, lane, lo, hi, ri, 0) && !follows) return 0;
    if (push) push_rhythm(s, lane);
    else act(s);
    fill_rhythm(p, lane, lo, hi, ri, 1);
    if (follows && bar >= 0) p->rb[d][bar].ed = 1;   /* 0.24 ruling 16: in the open bar the pick makes the bar its own, LOCK WITH stays */
    else if (follows) {   /* 0.24 a lane-wide pick (or try) turns the lane OWN, so it is heard: every bar renders */
        p->lw[d] = LW_OWN;
        lo = 0, hi = p->filled;
    }
    render_mask(s, p, lane, ((1u << hi) - 1) & ~((1u << lo) - 1), 1);   /* 0.22 a queued JAM block: once */
    return 1;
}

int seq_rhythm_pick(seq_t *s, int lane, int ri) {
    if (ri < 0 || ri >= RHYTHM_COUNT || !lane_drawn(cur(s), lane)) return 0;
    if (rbrowse_live(s, lane)) return rhythm_fill_now(s, lane, ri, 0);   /* the field's arrows while RHYTHMS is open: a try */
    if (!txn_live(s, lane, CVIEW_BAR)) seq_txn_done(s);
    return rhythm_fill_now(s, lane, ri, 1);
}

/* Its open pushes the one undo step DONE keeps; the chip is the shown bar's feel */
void seq_rbrowse_open(seq_t *s, int lane) {
    seq_part_t *p = cur(s);
    const rbar_t *rb;
    int bar;
    if (!lane_drawn(p, lane) || rbrowse_live(s, lane)) return;
    rb = p->rb[dl_of(lane)];
    if (txn_live(s, lane, CVIEW_BAR)) {
        s->txn.sub = CVIEW_RBROWSE, s->txn.copy_mask = 0;
        s->txn.sub_rb = rb[s->txn.slot];
        memcpy(s->txn.sub_hoct, cur(s)->hoct[s->txn.slot], sizeof s->txn.sub_hoct);   /* 0.24 final review: what its CANCEL puts back */
    } else {
        act(s);
        txn_begin(s, lane, CVIEW_RBROWSE, -1);
    }
    bar = s->txn.slot >= 0 ? s->txn.slot : 0;
    if (rb[bar].from >= 0 && !rhythm_is_jam(rb[bar].from)) s->tv[DLANE[dl_of(lane)].tab].rgroup = rhythm_lib_group(DLANE[dl_of(lane)].rlib, rb[bar].from);
    push_rhythm(s, lane);
}

void seq_rbrowse_try(seq_t *s, int lane, int ri) {
    if (ri >= 0 && ri < RHYTHM_COUNT && rbrowse_live(s, lane)) rhythm_fill_now(s, lane, ri, 0);
}

int seq_rbrowse_group(const seq_t *s, int lane) { return dl_of(lane) < 0 ? 0 : s->tv[DLANE[dl_of(lane)].tab].rgroup; }

void seq_rbrowse_set_group(seq_t *s, int lane, int g) {
    int i = 0, d = dl_of(lane);
    if (d < 0 || g < 0 || g >= RHYTHM_GROUPS) return;
    s->tv[DLANE[d].tab].rgroup = g;
    while (i < RHYTHM_COUNT - 1 && rhythm_lib_group(DLANE[d].rlib, i) != g) i++;
    seq_rbrowse_try(s, lane, i);
}

/* 0.19 the BASS tab: its views, the note settings and the STYLE browser (kind TXN_BSTYLE; 0.24 the bar editor is PHRASE's open bar),
 * on the playing part (seq_core.h). The note edits are Bass snapshots (push_snap_part keeps the notes; bkeep: the settings too);
 * the two transactions follow the VOICING browser's and the RHYTHM bar's rules (txn_*). */

int seq_bview(const seq_t *s) {
    if (txn_shown(s, TAB_BASS)) {
        switch (s->txn.kind) {
        case TXN_BSTYLE: return BVIEW_STYLE;
        case CVIEW_BAR: return s->txn.sub == CVIEW_COPY ? BVIEW_COPY : s->txn.sub == CVIEW_RBROWSE ? BVIEW_RBROWSE : BVIEW_BAR;
        case CVIEW_RBROWSE: return BVIEW_RBROWSE;
        }
    }
    return s->tv[TAB_BASS].view;
}

void seq_set_bview(seq_t *s, int v) {
    if (v != BVIEW_BASS && v != BVIEW_RHYTHM && v != BVIEW_JAM) return;   /* 0.22 JAM */
    txn_unstale(s);
    if (s->txn.kind && s->txn.lane == LANE_BASS) txn_done(s);   /* the BASS tab's is kept; the CHORDS tab's stays open */
    s->tv[TAB_BASS].view = v;
}

int seq_bmode(const seq_t *s) {
    int v = seq_bview(s);
    return v == BVIEW_BASS || v == BVIEW_EDIT || v == BVIEW_STYLE ? 0 : v == BVIEW_JAM ? 2 : 1;   /* 0.22 JAM 2 */
}

int seq_bknob(const seq_t *s, int k) { return k >= 0 && k < BK_COUNT ? ccur(s)->bass.set.k[k] : 0; }

void seq_set_bknob(seq_t *s, int k, int v) {
    seq_part_t *p = cur(s);
    if (k < 0 || k >= BK_COUNT) return;
    v = bk_clamp(k, v);
    if (v == p->bass.set.k[k]) return;
    act(s);
    p->bass.set.k[k] = (signed char)v;
    if (s->txn.kind == TXN_BSTYLE && !s->txn.stale) s->txn.bturned |= (unsigned char)(1u << k);   /* kept by CANCEL and UNDO */
    bass_notes_changed(s, p);
}

/* A Bass undo step (keep: what of the note settings it restores, seq_snap_t.bkeep); with the bar editor or STYLE browser open, its own */
static void push_bass(seq_t *s, unsigned keep) {
    int pushes = s->undo_pushes;
    act(s);
    push_undo(s, LANE_BASS);
    s->undo[(s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO].bkeep = (unsigned char)keep;
    if (s->txn.kind == TXN_BSTYLE) txn_mine(s, pushes);
}

/* The note settings `to` as one undo step (none when they are already: a re-send closes nothing; keep as push_bass's); an open
 * transaction is kept first (ruling 24) */
static void bset_edit(seq_t *s, bass_set_t to, unsigned keep) {
    seq_part_t *p = cur(s);
    if (!memcmp(&to, &p->bass.set, sizeof to)) return;
    seq_txn_done(s);
    push_bass(s, keep);
    p->bass.set = to;
    bass_notes_changed(s, p);
}

/* Style i's settings with no undo step (a browser's try): the style sets knobs 2-5, so no knob counts as turned any more */
static void bstyle_apply(seq_t *s, int i) {
    seq_part_t *p = cur(s);
    bass_set_t to = bass_style_set(i);
    s->txn.bturned = 0;
    if (!memcmp(&to, &p->bass.set, sizeof to)) return;
    act(s);
    p->bass.set = to;
    bass_notes_changed(s, p);
}

void seq_set_bass_pedal(seq_t *s, int pd) {
    bass_set_t to = cur(s)->bass.set;
    if (pd < 0 || pd > 2) return;
    to.pedal = (signed char)pd;
    bset_edit(s, to, BKEEP_PO);
}

void seq_set_bass_octave(seq_t *s, int o) {
    bass_set_t to = cur(s)->bass.set;
    if (o < -1 || o > 1) return;
    to.octave = (signed char)o;
    bset_edit(s, to, BKEEP_PO);
}

void seq_set_bass_style(seq_t *s, int i) {
    if (i < 0 || i >= BASS_STYLE_COUNT) return;
    if (txn_live(s, LANE_BASS, TXN_BSTYLE)) bstyle_apply(s, i);   /* the field's arrows while STYLE is open: a try */
    else bset_edit(s, bass_style_set(i), BKEEP_PO | BKEEP_KNOBS);
}

int seq_bass_style(const seq_t *s) { return ccur(s)->bass.set.style; }

int seq_bass_edited(const seq_t *s) { return bass_set_edited(&ccur(s)->bass.set); }

/* The settings RESET KNOBS gives: the style's own, Custom's all 0 */
static bass_set_t bass_own(const bass_set_t *b) {
    bass_set_t z = {b->style, 0, 0, {0}};
    return b->style >= 0 && b->style < BASS_STYLE_COUNT ? bass_style_set(b->style) : z;
}

/* 1 when the BASS page's RESET KNOBS has nothing to reset */
static int bass_knobs_fresh(const seq_t *s) {
    const seq_part_t *p = ccur(s);
    const int *n = LSHAPE_NEUTRAL[LANE_BASS], *sh = p->lane_shape[LANE_BASS];
    bass_set_t o = bass_own(&p->bass.set);
    return !memcmp(&o, &p->bass.set, sizeof o) && p->lane[LANE_BASS].gen.complexity == BASS_CX_NEUTRAL && sh[BSHAPE_PUSH] == n[BSHAPE_PUSH] &&
           sh[BSHAPE_GATE] == n[BSHAPE_GATE] && sh[BSHAPE_ACCENT] == n[BSHAPE_ACCENT];
}

void seq_reset_bass_knobs(seq_t *s) {
    seq_part_t *p = cur(s);
    const int *n = LSHAPE_NEUTRAL[LANE_BASS];
    if (bass_knobs_fresh(s)) return;
    seq_txn_done(s);
    push_bass(s, BKEEP_PO | BKEEP_KNOBS);
    s->undo[(s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO].knobs = 1;   /* undo restores the complexity and the SHAPE row too */
    p->bass.set = bass_own(&p->bass.set);
    p->lane_shape[LANE_BASS][BSHAPE_PUSH] = n[BSHAPE_PUSH];
    p->lane_shape[LANE_BASS][BSHAPE_GATE] = n[BSHAPE_GATE];
    p->lane_shape[LANE_BASS][BSHAPE_ACCENT] = n[BSHAPE_ACCENT];
    p->lane[LANE_BASS].gen.complexity = BASS_CX_NEUTRAL;
    regen(s, p, LANE_BASS);   /* every bar: the settings and COMPLEXITY */
}

/* The STYLE browser: its open pushes the one undo step DONE keeps (txn_done drops it when nothing changed) */
void seq_bstyle_open(seq_t *s) {
    seq_part_t *p = cur(s);
    if (!bass_drawn(p) || txn_live(s, LANE_BASS, TXN_BSTYLE)) return;
    act(s);
    txn_begin(s, LANE_BASS, TXN_BSTYLE, -1);
    if (p->bass.set.style >= 0) s->tv[TAB_BASS].browse_genre = bass_style_genre(p->bass.set.style);
    push_bass(s, BKEEP_PO | BKEEP_KNOBS);
    s->txn.bstep = (s->undo_top + SEQ_UNDO - 1) % SEQ_UNDO;   /* the step DONE keeps */
}

void seq_bstyle_try(seq_t *s, int i) {
    if (i >= 0 && i < BASS_STYLE_COUNT && txn_live(s, LANE_BASS, TXN_BSTYLE)) bstyle_apply(s, i);
}

int seq_bstyle_genre(const seq_t *s) { return s->tv[TAB_BASS].browse_genre; }

void seq_bstyle_set_genre(seq_t *s, int g) {
    if (g < 0 || g >= BASS_GENRES) return;
    s->tv[TAB_BASS].browse_genre = g;
    seq_bstyle_try(s, g * (BASS_STYLE_COUNT / BASS_GENRES));   /* the genre's first style (bass_style_genre) */
}

void seq_bstyle_done(seq_t *s) {
    if (txn_live(s, LANE_BASS, TXN_BSTYLE)) txn_done(s);
}

void seq_bstyle_cancel(seq_t *s) {
    if (txn_live(s, LANE_BASS, TXN_BSTYLE)) txn_cancel(s);
}

/* 0.24 PHRASE's BASS picker (seq_core.h) on the open Bass bar's selected step, when it sounds */

/* The open Bass bar with a sounding step selected, else NULL */
static seq_part_t *phrase_bass(seq_t *s) {
    return bar_sub(s, LANE_BASS, 0) && s->txn.bsel >= 0 && bass_step(cur(s), s->txn.slot, s->txn.bsel)->on ? cur(s) : NULL;
}

/* The selected step fixed at `note` (0: unfixed), one undo step (none when it already is), and heard once */
static void phrase_fix(seq_t *s, seq_part_t *p, int note) {
    int i = s->txn.slot * SEQ_STEPS + s->txn.bsel;
    if (p->bass.fix[i] != note) {
        phrase_push(s, PH_LANE);
        p->bass.fix[i] = (unsigned char)note;
        render_one(s, p, LANE_BASS, s->txn.slot);
    }
    seq_audition_note(s, LANE_BASS, bass_step(p, s->txn.slot, s->txn.bsel)->abs);
}

void seq_rbar_note(seq_t *s, int pc) {
    seq_part_t *p = phrase_bass(s);
    if (p && pc >= 0 && pc <= 11) phrase_fix(s, p, (s->txn.boct + 1) * 12 + pc);
}

void seq_rbar_note_oct(seq_t *s, int o) {
    seq_part_t *p;
    if (o < 1 || o > 4 || !bar_sub(s, LANE_BASS, 0)) return;
    s->txn.boct = o;
    if ((p = phrase_bass(s)) != NULL) phrase_fix(s, p, (o + 1) * 12 + bass_step(p, s->txn.slot, s->txn.bsel)->abs % 12);   /* the note in the new octave */
}

int seq_rbar_oct(const seq_t *s) { return seq_rbar_bar(s, LANE_BASS) >= 0 ? s->txn.boct : 1; }

void seq_rbar_follow_chord(seq_t *s) {
    seq_part_t *p = phrase_bass(s);
    if (p) phrase_fix(s, p, 0);
}

/* 0.22 JAM on the Chords and Bass lanes (seq_core.h): the lane JAM state (seq_part_t.djam), a block applied, its knobs. Rendering
 * reads it in render_bar (jam_codes), bass_bar_notes and chord_notes (jam_var_pos). */

/* 0.22 a Bass block's note settings, reading Custom */
static bass_set_t jam_bass_set(int c, int l) {
    const jam_bass_t *j = jam_bass(c, l);
    return (bass_set_t){-1, j->pedal, j->octave, {j->mov, j->octs, j->app, j->reg}};
}

/* 0.22 block (c, l) onto drawn lane d of part p, pure on the part (no seq_t: Task 4 applies it to a scratch part; the caller renders):
 * a Bass bar with only its rhythm locked is note-locked first at the settings it plays (frozen), so it keeps everything; every bar
 * of the loop not locked for JAM then takes the block's rhythm as its JAM index (ed 0); the lane's note settings become the block's,
 * reading Custom (the Chords voicing is per part: a rhythm-locked bar plays it too) */
static void jam_apply_part(seq_part_t *p, int d, int c, int l) {
    int from = RJAM_BASE + c * JAM_LEVELS + l;
    for (int b = 0; b < p->bars; b++) {
        rbar_t *r = &p->rb[d][b];
        if (d == DL_BASS && r->lock && !(p->bass.nlock >> b & 1)) {
            p->bass.frozen[b] = p->bass.set;
            p->bass.nlock |= (unsigned short)(1u << b);
        }
        if (jam_locked(p, d, b)) continue;
        rhythm_lib_codes(DLANE[d].rlib, from, r->c);
        r->from = (signed char)from, r->ed = 0;
    }
    if (d == DL_CHORDS) {
        const jam_chords_t *j = jam_chords(c, l);
        p->vstyle = -1;
        part_vox(p, j->mode, j->col, j->inv, j->spr, j->str, j->oct);
    } else
        p->bass.set = jam_bass_set(c, l);
    p->djam[d].c = (signed char)c, p->djam[d].l = (signed char)l;
    p->lw[d] = LW_OWN;   /* 0.24 a JAM tap turns its lane's LOCK WITH OWN: the block brings its own rhythm */
}

/* 0.22 1 when drawn lane d differs from the block it plays (none: 0): its voicing / note settings, or a bar not locked for JAM whose
 * codes or index aren't the block's */
static int jam_differs(const seq_part_t *p, int d) {
    const djam_t *j = &p->djam[d];
    int from = RJAM_BASE + j->c * JAM_LEVELS + j->l;
    unsigned char k[16];
    if (j->c < 0) return 0;
    if (p->lw[d] != LW_OWN) return 1;   /* 0.24 a lane that follows the other plays its rhythm, not the block's */
    rhythm_lib_codes(DLANE[d].rlib, from, k);
    for (int b = 0; b < p->bars; b++)
        if (!jam_locked(p, d, b) && (p->rb[d][b].from != from || memcmp(p->rb[d][b].c, k, 16))) return 1;
    if (d == DL_CHORDS) {
        const jam_chords_t *v = jam_chords(j->c, j->l);
        return p->vstyle != -1 || p->vmode != v->mode || p->vcol != v->col || p->vinv != v->inv || p->vspr != v->spr || part_vstr(p) != v->str ||
               p->lane_shape[LANE_CHORDS][SHAPE_OCT] != v->oct;
    }
    bass_set_t b = jam_bass_set(j->c, j->l);
    return memcmp(&b, &p->bass.set, sizeof b) != 0;
}

/* 0.22 a JAM tap's one undo step on drawn lane d: the Chords voicing's and bars' (as RESET KNOBS: two joined entries); the Bass bars, note
 * settings and note locks (the auto ones too) */
static void jam_push(seq_t *s, int d) {
    if (d == DL_CHORDS) {
        push_voicing_snap(s);
        push_snap(s, LANE_CHORDS, 1);
    } else
        push_bass(s, BKEEP_PO | BKEEP_KNOBS);
}

/* 0.22 the undo entries a queued tap pushed (seq_snap_t.jq == tag, 0 none) leave the ring, which closes up (as txn_drop_own) */
static void djq_unpush(seq_t *s, int tag) {
    int base = s->undo_top + SEQ_UNDO - s->undo_n, w = 0;
    if (!tag) return;
    for (int i = 0; i < s->undo_n; i++) {
        const seq_snap_t *u = &s->undo[(base + i) % SEQ_UNDO];
        if (u->jq == tag) continue;
        if (w != i) s->undo[(base + w) % SEQ_UNDO] = *u;
        w++;
    }
    s->undo_n = w;
    s->undo_top = (base + w) % SEQ_UNDO;
}

/* 0.22 drawn lane d's queue cancelled, its tap's undo step with it */
static void djq_cancel(seq_t *s, int d) {
    if (s->djq[d].c < 0) return;
    djq_unpush(s, s->djq[d].tag);
    s->djq[d].c = -1;
}

/* 0.22 the undo steps of taps whose queue a part switch dropped (djq_drop) leave the ring: the screen side, before a tap or an UNDO */
static void djq_sweep(seq_t *s) {
    for (int d = 0; d < DL_COUNT; d++) {
        djq_unpush(s, s->djq_drop[d]);
        s->djq_drop[d] = 0;
    }
}

/* 0.22 UNDO while a JAM block waits: the latest queue (the higher tag) cancelled; 1 when there was one */
static int djq_undo(seq_t *s) {
    int d = s->djq[DL_BASS].c >= 0 && (s->djq[DL_CHORDS].c < 0 || s->djq[DL_BASS].tag > s->djq[DL_CHORDS].tag) ? DL_BASS : DL_CHORDS;
    if (s->djq[d].c < 0) return 0;
    act(s);
    djq_cancel(s, d);
    return 1;
}

void seq_jam_apply_now(seq_t *s, int lane, int c, int l) {
    seq_part_t *p = cur(s);
    int d = dl_of(lane), echo;
    if (d < 0 || c < 0 || c >= JAM_COLS || l < 0 || l >= JAM_LEVELS || !lane_drawn(p, lane)) return;
    echo = c == p->djam[d].c && l == p->djam[d].l && !jam_differs(p, d);
    if (echo && s->djq[d].c < 0) return;   /* the block as it plays (a host echo) */
    seq_txn_done(s);   /* an open transaction is kept first (ruling 24); 0.22: before any ring edit, so its CANCEL can't put one back */
    act(s);
    djq_cancel(s, d);   /* 0.22: a block queued on the lane gives way */
    if (echo) return;
    jam_push(s, d);
    jam_apply_part(p, d, c, l);
    if (d == DL_CHORDS) colour_changed(s, p);   /* the voices, labels and the lanes that read the colours */
    render_rhythm(s, p, lane);
}

int seq_jam_queue(seq_t *s, int lane, int c, int l) {
    seq_part_t *p = cur(s);
    int d = dl_of(lane), before;
    djq_t *q;
    if (d < 0 || c < 0 || c >= JAM_COLS || l < 0 || l >= JAM_LEVELS || !lane_drawn(p, lane)) return 0;
    q = &s->djq[d];
    if (!s->playing) {   /* stopped: at once */
        seq_jam_apply_now(s, lane, c, l);
        return 0;
    }
    if (q->c == c && q->l == l) return 1;   /* the block queued (a host echo) */
    if (c == p->djam[d].c && l == p->djam[d].l && q->c < 0 && !jam_differs(p, d)) return 0;   /* the playing block, nothing queued: an echo */
    /* An open transaction is kept first (ruling 24), before any undo ring edit: its CANCEL puts back the ring as at its open
     * (txn_ring_back), which would bring a removed tap's entries back */
    seq_txn_done(s);
    act(s);
    djq_sweep(s);
    if (c == p->djam[d].c && l == p->djam[d].l && q->c >= 0) {   /* the playing block with a block queued: a cancel */
        djq_cancel(s, d);
        return 0;
    }
    djq_cancel(s, d);   /* a second tap replaces the queue */
    if (switch_blocked(s, switch_q(p, p->djam[d].sw))) return 0;   /* a part switch queued takes over first and would drop it */
    before = s->undo_pushes;
    jam_push(s, d);   /* the tap's undo step, its entries marked with the queue's tag */
    q->tag = before + 1;
    for (int k = 0; k < s->undo_pushes - before && k < s->undo_n; k++) s->undo[(s->undo_top + SEQ_UNDO - 1 - k) % SEQ_UNDO].jq = q->tag;
    q->c = (signed char)c, q->l = (signed char)l, q->part = s->cur_part;
    djq_render(s, d);
    return 1;
}

int seq_jam_queued(const seq_t *s, int lane, int *c, int *l) {
    int d = dl_of(lane);
    if (d < 0 || s->djq[d].c < 0 || s->djq[d].part != s->cur_part || !lane_drawn(ccur(s), lane)) return 0;
    if (c) *c = s->djq[d].c;
    if (l) *l = s->djq[d].l;
    return 1;
}

/* 0.22 seq_mel_queued's at_bar for a drawn lane's queue: the first boundary of its SWITCH AT at or after the next step */
int seq_jam_at_bar(const seq_t *s, int lane) {
    const seq_part_t *p = ccur(s);
    long k;
    if (!seq_jam_queued(s, lane, NULL, NULL)) return -1;
    k = s->next_k + floor_mod(s->part_origin_k - s->next_k, switch_q(p, p->djam[dl_of(lane)].sw));
    return (int)(floor_mod(k - s->part_origin_k, p->bars * SEQ_STEPS) / SEQ_STEPS);
}

int seq_jam_poll(seq_t *s) {
    int m = s->dj_switched;
    s->dj_switched = 0;
    return m;
}

int seq_jam_playing(const seq_t *s, int lane, int *c, int *l) {
    int d = dl_of(lane);
    if (d < 0 || ccur(s)->djam[d].c < 0) return 0;
    if (c) *c = ccur(s)->djam[d].c;
    if (l) *l = ccur(s)->djam[d].l;
    return 1;
}

int seq_jam_edited(const seq_t *s, int lane) { return dl_of(lane) >= 0 && jam_differs(ccur(s), dl_of(lane)); }

/* 0.22 a lane JAM setting's field (DJ_*) and range */
static signed char *dj_field(djam_t *j, int f) {
    signed char *at[DJ_COUNT] = {&j->var, &j->swing, &j->tm, &j->sw};
    return f >= 0 && f < DJ_COUNT ? at[f] : NULL;
}
static const signed char DJ_LO[DJ_COUNT] = {0, 0, -1, 0}, DJ_HI[DJ_COUNT] = {3, 5, 1, 3};

int seq_djam_grid16(const seq_t *s, int lane) { return dl_of(lane) >= 0 ? ccur(s)->djam[dl_of(lane)].grid16 : 0; }
unsigned seq_djam_weak(const seq_t *s, int lane) { return dl_of(lane) >= 0 ? ccur(s)->djam[dl_of(lane)].weak : 0u; }

int seq_djam_get(const seq_t *s, int lane, int field) {
    djam_t j;
    const signed char *x;
    if (dl_of(lane) < 0) return 0;
    j = ccur(s)->djam[dl_of(lane)];
    x = dj_field(&j, field);
    return x ? *x : 0;
}

void seq_djam_set(seq_t *s, int lane, int field, int v) {
    seq_part_t *p = cur(s);
    signed char *x = dl_of(lane) >= 0 ? dj_field(&p->djam[dl_of(lane)], field) : NULL;
    if (!x) return;
    v = clampi(v, DJ_LO[field], DJ_HI[field]);
    if (v == *x) return;   /* a host echo */
    act(s);
    *x = (signed char)v;
    if ((field == DJ_VAR || field == DJ_TIME) && lane_drawn(p, lane)) render_rhythm(s, p, lane);   /* every bar; the Chords' INVERSION steps play where read */
    if (field == DJ_SWING) {   /* 0.22 its weak bit, no render (SWING moves hits in time only) */
        djam_t *j = &p->djam[dl_of(lane)];
        j->weak = (unsigned char)((j->weak & ~DW_SWING) | (lane_drawn(p, lane) ? dj_weak_swing(p, dl_of(lane)) : 0u));
    }
}

/* 0.20 the MELODY tab: its views, the JAM settings, the ANSWER BLOCK picker (kind TXN_MPICK) and the bar editor (kind TXN_MEDIT,
 * slot the bar), on the playing part's JAM lane (seq_core.h). The editor's edits are Melody snapshots (push_snap_part keeps the
 * JAM notes); the two transactions follow the BASS tab's rules (txn_*). */

int seq_mview(const seq_t *s) {
    if (txn_shown(s, TAB_MEL)) {
        if (s->txn.kind == TXN_MPICK) return MVIEW_PICK;
        if (s->txn.kind == TXN_MEDIT) return MVIEW_EDIT;
    }
    return s->tv[TAB_MEL].view;
}

void seq_set_mview(seq_t *s, int v) {
    if (v != MVIEW_JAM && v != MVIEW_PHRASE) return;
    txn_unstale(s);
    if (s->txn.kind && s->txn.lane == LANE_MEL_UP) txn_done(s);   /* the MELODY tab's is kept; another tab's stays open */
    s->tv[TAB_MEL].view = v;
}

int seq_mmode(const seq_t *s) { return seq_mview(s) == MVIEW_JAM ? 0 : 1; }

/* A JAM setting's field: its place in mjam_set_t and its range */
static signed char *mjs_field(mjam_set_t *m, int f) {
    signed char *at[MJS_COUNT] = {&m->form, &m->breath, &m->cad, &m->fill, &m->ans, &m->stray, &m->aoct, &m->reg, &m->dens, &m->rng, &m->var,
                                  &m->tm};
    return f >= 0 && f < MJS_COUNT ? at[f] : NULL;
}

static const signed char MJS_LO[MJS_COUNT] = {0, 0, 0, 0, 0, 0, -1, 0, -2, -1, 0, -1};
static const signed char MJS_HI[MJS_COUNT] = {MF_COUNT - 1, 3, 3, 3, MA_COUNT - 1, 3, 1, 2, 2, 2, 3, 1};

int seq_mjam_get(const seq_t *s, int field) {
    mjam_set_t m = ccur(s)->mjam.set;
    const signed char *x = mjs_field(&m, field);
    return x ? *x : 0;
}

void seq_mjam_set(seq_t *s, int field, int v) {
    seq_part_t *p = cur(s);
    signed char *x = mjs_field(&p->mjam.set, field);
    if (!x) return;
    v = clampi(v, MJS_LO[field], MJS_HI[field]);
    if (v == *x) return;   /* nothing changes (a host echo): a lane that isn't JAM stays as it is */
    mel_jam_now(s);
    act(s);
    *x = (signed char)v;
    render_mel(s, p);   /* every bar (a locked one plays its frozen settings) and a queued block */
}

/* The SWAP block (c, l) set and rendered, no undo step; 0 when refused: a bad block, the call's own, or a lane that isn't JAM */
static int mel_swap_set(seq_t *s, int c, int l) {
    seq_part_t *p = cur(s);
    mjam_set_t *m = &p->mjam.set;
    if (c < 0 || c >= MJ_COLS || l < 0 || l >= MJ_LEVELS || !mel_jam(p) || (c == m->c && l == m->l)) return 0;
    if (c == m->sc && l == m->sl) return 1;
    act(s);
    m->sc = (signed char)c, m->sl = (signed char)l;
    render_mel(s, p);
    return 1;
}

void seq_mel_swap(seq_t *s, int c, int l) {
    const mjam_set_t *m = &cur(s)->mjam.set;
    if (c < 0 || c >= MJ_COLS || l < 0 || l >= MJ_LEVELS || (!mel_jam(cur(s)) && c == m->sc && l == m->sl)) return;
    mel_jam_now(s);   /* the shown SWAP block (a host echo) leaves a lane that isn't JAM as it is */
    mel_swap_set(s, c, l);
}

/* The ANSWER BLOCK picker: no undo step (the SWAP block is a setting); its CANCEL puts sc and sl back (txn_swap) */
void seq_mpick_open(seq_t *s) {
    if (txn_live(s, LANE_MEL_UP, TXN_MPICK)) return;
    mel_jam_now(s);
    seq_txn_done(s);
    act(s);
    txn_begin(s, LANE_MEL_UP, TXN_MPICK, -1);
}

void seq_mpick_try(seq_t *s, int c, int l) {
    if (txn_live(s, LANE_MEL_UP, TXN_MPICK)) mel_swap_set(s, c, l);
}

void seq_mpick_done(seq_t *s) {
    if (txn_live(s, LANE_MEL_UP, TXN_MPICK)) txn_done(s);
}

void seq_mpick_cancel(seq_t *s) {
    if (txn_live(s, LANE_MEL_UP, TXN_MPICK)) txn_cancel(s);
}

/* The bar editor */

static const mjam_note_t *medit_note(const seq_t *s, int step) { return &ccur(s)->mline[s->txn.slot * SEQ_STEPS + step]; }

/* The editor's step: selected; a sounding one sets the octave (3..7) to its note's */
static void medit_pick(seq_t *s, int step) {
    int note = medit_note(s, step)->note;
    s->txn.bsel = step;
    if (note) s->txn.boct = clampi(note / 12 - 1, 3, 7);
}

void seq_medit_open(seq_t *s, int bar) {
    seq_part_t *p = cur(s);
    int first = 0;
    if (bar < 0 || bar >= p->bars) return;
    mel_jam_now(s);
    seq_txn_done(s);
    act(s);
    txn_begin(s, LANE_MEL_UP, TXN_MEDIT, bar);
    strip_hold_bar(s, TAB_MEL, bar);
    s->txn.boct = 4;
    while (first < SEQ_STEPS - 1 && !medit_note(s, first)->note) first++;   /* the first sounding step (none: step 1) */
    medit_pick(s, medit_note(s, first)->note ? first : 0);
}

int seq_medit_bar(const seq_t *s) { return s->txn.kind == TXN_MEDIT && !s->txn.stale ? s->txn.slot : -1; }

/* The editor is open on a lane still JAM (a style set meanwhile leaves its edits nothing to render into) */
static int medit_live(seq_t *s) { return txn_live(s, LANE_MEL_UP, TXN_MEDIT) && mel_jam(cur(s)); }

int seq_medit_sel(const seq_t *s) { return seq_medit_bar(s) >= 0 ? s->txn.bsel : -1; }

int seq_medit_select(seq_t *s, int step) {
    if (step < 0 || step >= SEQ_STEPS || !medit_live(s)) return 0;
    medit_pick(s, step);
    return 1;
}

int seq_medit_oct(const seq_t *s) { return seq_medit_bar(s) >= 0 ? s->txn.boct : 4; }

/* A Melody undo step; with the bar editor open, its own */
static void push_mel(seq_t *s) {
    int pushes = s->undo_pushes;
    act(s);
    push_undo(s, LANE_MEL_UP);
    if (s->txn.kind == TXN_MEDIT) txn_mine(s, pushes);
}

/* The selected step fixed as `fix` (a MIDI note, MJ_FIX_REST or MJ_FIX_HOLD): one undo step, none when it already is */
static void medit_fix(seq_t *s, int fix) {
    seq_part_t *p = cur(s);
    unsigned char *f = &p->mjam.fix[s->txn.slot * SEQ_STEPS + s->txn.bsel];
    if (*f == fix) return;
    push_mel(s);
    *f = (unsigned char)fix;
    render_mel(s, p);
}

void seq_medit_set_oct(seq_t *s, int o) {
    int note;
    if (o < 3 || o > 7 || !medit_live(s)) return;
    s->txn.boct = o;
    note = medit_note(s, s->txn.bsel)->note;
    if (!note) return;
    note = (o + 1) * 12 + note % 12;   /* the sounding note in the new octave, heard (as the Bass editor's) */
    medit_fix(s, note);
    seq_audition_note(s, LANE_MEL_UP, note);
}

void seq_medit_note(seq_t *s, int pc) {
    int note;
    if (pc < 0 || pc > 11 || !medit_live(s)) return;
    note = (s->txn.boct + 1) * 12 + pc;
    medit_fix(s, note);
    seq_audition_note(s, LANE_MEL_UP, note);
}

void seq_medit_rest(seq_t *s) {
    if (medit_live(s)) medit_fix(s, MJ_FIX_REST);
}

void seq_medit_hold(seq_t *s) {
    if (medit_live(s)) medit_fix(s, MJ_FIX_HOLD);
}

/* Bar b's line as render_mel would make it now (mel_render_bar: its settings, NEW BAR variation and fixes) */
static void mel_bar_line(const seq_t *s, const seq_part_t *p, int b, mjam_note_t line[SEQ_STEPS]) {
    mel_harm_t h;
    mjam_loop_in_t in = {s->key, s->scale, p->bars, p->mjam.set, h.bar, p->lane[LANE_MEL_UP].bar_var};
    mel_harm(s, p, &h);
    mel_render_bar(s, p, &in, &h, b, line, NULL);
}

/* 0.16's rule: NEW BAR always changes the bar when it can (the next variation whose notes differ, up to 8 tries) */
int seq_medit_new_bar(seq_t *s) {
    seq_part_t *p = cur(s);
    mjam_note_t was[SEQ_STEPS], now[SEQ_STEPS];
    unsigned char *v;
    int b, v0, t, found;
    if (!medit_live(s)) return 0;
    b = s->txn.slot;
    v = &p->lane[LANE_MEL_UP].bar_var[b];
    v0 = *v;
    memcpy(was, &p->mline[b * SEQ_STEPS], sizeof was);
    for (t = 0; t < 8; t++) {
        *v = (unsigned char)(*v == 255 ? 1 : *v + 1);
        mel_bar_line(s, p, b, now);
        if (memcmp(was, now, sizeof was)) break;
    }
    found = *v;
    *v = (unsigned char)v0;
    if (t == 8) return 0;   /* no variation sounds different: nothing rendered, as it was */
    push_mel(s);
    *v = (unsigned char)found;
    render_mel(s, p);
    return 1;
}

void seq_medit_reset_bar(seq_t *s) {
    seq_part_t *p = cur(s);
    int b;
    if (!medit_live(s)) return;
    b = s->txn.slot;
    if (!p->lane[LANE_MEL_UP].bar_var[b] && !(p->mjam.nlock >> b & 1) && !seq_mel_fixed_count(s, b)) return;   /* already */
    push_mel(s);
    p->lane[LANE_MEL_UP].bar_var[b] = 0;
    p->mjam.nlock = (unsigned short)(p->mjam.nlock & ~(1u << b));
    memset(&p->mjam.fix[b * SEQ_STEPS], 0, SEQ_STEPS);
    render_mel(s, p);
}

void seq_medit_lock(seq_t *s) {
    seq_part_t *p = cur(s);
    int b;
    if (!medit_live(s)) return;
    b = s->txn.slot;
    push_mel(s);
    if (!(p->mjam.nlock >> b & 1)) p->mjam.frozen[b] = p->mjam.set;   /* the settings it plays now, frozen */
    p->mjam.nlock ^= (unsigned short)(1u << b);
    render_mel(s, p);
}

void seq_medit_cancel(seq_t *s) {
    if (txn_live(s, LANE_MEL_UP, TXN_MEDIT)) txn_cancel(s);
}

void seq_medit_done(seq_t *s) {
    if (txn_live(s, LANE_MEL_UP, TXN_MEDIT)) txn_done(s);
}

int seq_slot_label(const seq_t *s, int slot, char *buf, int len) {
    return copy_text(buf, len, slot_read_ok(ccur(s), slot) ? ccur(s)->slot_text[slot] : " ");
}

int seq_slot_numeral(const seq_t *s, int slot, char *buf, int len) {
    return copy_text(buf, len, slot_read_ok(ccur(s), slot) ? ccur(s)->slot_num[slot] : " ");
}

int seq_part_bar_changes(const seq_t *s, int part, int bar, int slots[SEQ_STEPS], int at[SEQ_STEPS]) {
    const seq_part_t *p = part >= 0 && part < SEQ_NPARTS ? &s->part[part] : NULL;
    int n = 0;
    for (int i = 0; p && bar >= 0 && bar < p->bars && i < SEQ_STEPS; i++)
        if (!i || chord_change_at(s, p, bar, i)) {
            if (at) at[n] = i;
            slots[n++] = slot_at(p, bar, i);
        }
    return n;
}

int seq_part_eff_colour(const seq_t *s, int part, int slot) {
    if (part < 0 || part >= SEQ_NPARTS || !slot_read_ok(&s->part[part], slot)) return COL_TRIAD;
    return eff_colour(s, &s->part[part], slot);
}



int seq_gate_pct(int g) { return (int)lround(gate_curve(g) * 100.0); }

int seq_style(const seq_t *s, int lane) {
    if (lane == LANE_CHORDS && drawn_lane(ccur(s))) return old_style(ccur(s)->rb[DL_CHORDS][0].from);   /* 0.18: style_1 reads bar 1's */
    return lane >= 0 && lane < SEQ_LANES ? ccur(s)->lane[lane].gen.style : GEN_MANUAL;
}

/* 0.18 the RHYTHM page's bars (0.19: a drawn lane's; any other lane reads as a bad bar) */
static const rbar_t *rbar_at(const seq_t *s, int lane, int bar) {
    return dl_of(lane) >= 0 && bar >= 0 && bar < SEQ_MAX_BARS ? &ccur(s)->rb[dl_of(lane)][bar] : NULL;
}

void seq_bar_codes(const seq_t *s, int lane, int bar, unsigned char out[16]) {
    if (rbar_at(s, lane, bar)) drawn_codes(ccur(s), dl_of(lane), bar, out);   /* 0.24 LOCK WITH's when it follows */
    else memset(out, RC_REST, 16);
}

/* 0.24 the steps at the start of bar `bar` (of the loop) ANTICIPATE's early hit in the bar before holds over the bar line, 0 none */
static int held_in(const seq_part_t *p, int lane, int bar) {
    int d = dl_of(lane), pb = (bar + p->bars - 1) % p->bars;
    for (int i = SEQ_STEPS - 4; bar < p->bars && i < SEQ_STEPS; i++) {
        const seq_step_t *x = &p->lane[lane].steps[pb * SEQ_STEPS + i];
        if (x->on && (p->sflag[d][pb * SEQ_STEPS + i] & SF_EARLY) && i + x->len > SEQ_STEPS) return i + x->len - SEQ_STEPS;
    }
    return 0;
}

void seq_bar_plays(const seq_t *s, int lane, int bar, unsigned char out[16]) {   /* the rendered steps read back: a getter never renders */
    const seq_step_t *st;
    if (!rbar_at(s, lane, bar)) {
        memset(out, RC_REST, 16);
        return;
    }
    st = &ccur(s)->lane[lane].steps[bar * SEQ_STEPS];
    rhythm_from_steps(st, out);
    for (int i = 0, h = held_in(ccur(s), lane, bar); i < h && !st[i].on; i++) out[i] = RC_HOLD;   /* 0.24 an early hit's tie */
}

int seq_bar_plays_at(const seq_t *s, int lane, int bar, int step) {   /* seq_bar_plays' step alone */
    const seq_step_t *st;
    int c;
    if (!rbar_at(s, lane, bar) || step < 0 || step >= SEQ_STEPS) return RC_REST;
    st = &ccur(s)->lane[lane].steps[bar * SEQ_STEPS];
    c = rhythm_step_code(st, step);
    if (c == RC_REST && step < held_in(ccur(s), lane, bar)) {   /* 0.24 an early hit's tie, up to the bar's first hit */
        int i = 0;
        while (i < step && !st[i].on) i++;
        if (i == step) c = RC_HOLD;
    }
    return c;
}

int seq_step_flags(const seq_t *s, int lane, int bar, int step) {
    return rbar_at(s, lane, bar) && step >= 0 && step < SEQ_STEPS ? ccur(s)->sflag[dl_of(lane)][bar * SEQ_STEPS + step] : 0;
}

int seq_bar_follows(const seq_t *s, int lane, int bar) { return rbar_at(s, lane, bar) && lane_drawn(ccur(s), lane) ? bar_follows(ccur(s), dl_of(lane), bar) : 0; }

int seq_lockwith(const seq_t *s, int lane) { return dl_of(lane) >= 0 ? ccur(s)->lw[dl_of(lane)] : LW_OWN; }
int seq_anticipate(const seq_t *s, int lane) { return dl_of(lane) >= 0 ? ccur(s)->an[dl_of(lane)] : AN_OFF; }
int seq_anticipate_steps(const seq_t *s, int lane) { return AN_STEPS[clampi(seq_anticipate(s, lane), 0, 3)]; }
int seq_lockwith_bumped(const seq_t *s) { return s->lw_bumped; }

/* 0.24 a drawn lane's LOCK WITH or ANTICIPATE set (an open transaction kept first): one undo step (its snapshot holds both lanes' LOCK WITH),
 * none when it is so already. WITH or GAPS while the other lane follows this one turns the other OWN (lw_bumped) */
void seq_set_lockwith(seq_t *s, int lane, int v) {
    seq_part_t *p = cur(s);
    int d = dl_of(lane), o = DL_COUNT - 1 - d;
    if (d < 0 || v < LW_OWN || v > LW_GAPS || !lane_drawn(p, lane) || p->lw[d] == v) return;
    seq_txn_done(s);
    act(s);
    push_undo(s, lane);
    p->lw[d] = (signed char)v;
    if (v != LW_OWN && p->lw[o] != LW_OWN) {
        p->lw[o] = LW_OWN;
        s->lw_bumped = 1;
        if (lane_drawn(p, DLANE[o].lane)) {   /* 0.24.1 the other lane, then this one following it: each once */
            render_pair(s, p, o);
            return;
        }
        render_rhythm(s, p, DLANE[o].lane);
    }
    render_rhythm(s, p, lane);
}

void seq_set_anticipate(seq_t *s, int lane, int v) {
    seq_part_t *p = cur(s);
    int d = dl_of(lane);
    if (d < 0 || v < AN_OFF || v > AN_BEAT || !lane_drawn(p, lane) || p->an[d] == v) return;
    seq_txn_done(s);
    act(s);
    push_undo(s, lane);
    p->an[d] = (signed char)v;
    render_mask(s, p, lane, (1u << p->filled) - 1, 0);   /* how it plays only: a lane following it reads its drawn codes */
}

int seq_bar_rhythm(const seq_t *s, int lane, int bar, int *edited) {
    const rbar_t *r = rbar_at(s, lane, bar);
    if (edited) *edited = r ? r->ed : 0;
    return r ? r->from : -1;
}

int seq_bar_locked(const seq_t *s, int lane, int bar) { const rbar_t *r = rbar_at(s, lane, bar); return r ? r->lock : 0; }

int seq_bar_hits(const seq_t *s, int lane, int bar) {
    int n = 0;
    if (!rbar_at(s, lane, bar)) return 0;
    for (int i = 0; i < SEQ_STEPS; i++) n += ccur(s)->lane[lane].steps[bar * SEQ_STEPS + i].on;
    return n;
}

/* 0.19 the BASS page's reads (seq_core.h) */
int seq_abs_note(const seq_step_t *x) { return x->on ? x->abs : 0; }

int seq_lane_drawn(const seq_t *s, int lane) { return lane_drawn(ccur(s), lane); }

void seq_bass_line(const seq_t *s, int bar, unsigned char note[16], char role[16]) {
    const seq_part_t *p = ccur(s);
    int ok = bass_drawn(p) && bar >= 0 && bar < p->bars;
    memcpy(role, seq_bass_roles(s, bar), 16);
    for (int i = 0; i < SEQ_STEPS; i++) note[i] = ok ? (unsigned char)seq_abs_note(bass_step(p, bar, i)) : 0;
}

const char *seq_bass_roles(const seq_t *s, int bar) {
    const seq_part_t *p = ccur(s);
    return bass_drawn(p) && bar >= 0 && bar < p->bars ? &p->brole[bar * SEQ_STEPS] : "................";
}

int seq_bass_bar_score(const seq_t *s, int bar) { return seq_part_bass_bar_score(s, s->cur_part, bar); }

int seq_part_bass_bar_score(const seq_t *s, int part, int bar) {
    const seq_part_t *p = &s->part[part >= 0 && part < SEQ_NPARTS ? part : s->cur_part];
    unsigned char note[SEQ_STEPS];
    bass_chord_t ch[SEQ_STEPS];
    const bass_chord_t *of[SEQ_STEPS];
    bass_key_t k;
    if (!bass_drawn(p) || bar < 0 || bar >= p->bars) return 0;
    bar_notes(p, bar, note);
    k = bass_key(s);
    for (int i = 0; i < SEQ_STEPS; i++) {   /* the chord under each step, worked out once a slot */
        ch[i] = i && slot_at(p, bar, i) == slot_at(p, bar, i - 1) ? ch[i - 1] : bass_chord_at(s, p, bar * SEQ_STEPS + i);
        of[i] = &ch[i];
    }
    return bass_score(note, SEQ_STEPS, of, &k);
}

void seq_bass_style_roles(const seq_t *s, int style, int n, int bar, char role[][16]) {
    const seq_part_t *p = ccur(s);
    unsigned char c[16], fill[16], note[16];
    bass_key_t k;
    bar_harm_t h;
    for (int j = 0; j < n; j++) memset(role[j], '.', 16);
    if (!bass_drawn(p) || bar < 0 || bar >= p->bars) return;
    k = bass_key(s);
    render_bar(p, LANE_BASS, bar, c, fill);   /* the codes and chords once for every style */
    bar_harm(s, p, bar, &k, &h);
    for (int j = 0; j < n; j++) {
        bass_set_t set;
        if (style + j < 0 || style + j >= BASS_STYLE_COUNT) continue;
        set = bass_style_set(style + j);   /* a note-locked bar keeps its own (bar_set: styles leave it) */
        bass_bar_notes(p, bar, &set, &h, c, note, role[j]);
    }
}

int seq_bass_tension_at(const seq_t *s, int bar, int step, int midi) {
    const seq_part_t *p = ccur(s);
    bass_key_t k;
    bass_chord_t c;
    if (bar < 0 || bar >= p->bars || step < 0 || step >= SEQ_STEPS || midi <= 0) return 0;
    k = bass_key(s);
    c = bass_chord_at(s, p, bar * SEQ_STEPS + step);
    return bass_tension(midi % 12, &c, &k);
}

int seq_bass_fixed_count(const seq_t *s, int bar) {
    int n = 0;
    if (bar < 0 || bar >= ccur(s)->bars) return 0;
    for (int i = 0; i < SEQ_STEPS; i++) n += ccur(s)->bass.fix[bar * SEQ_STEPS + i] != 0;
    return n;
}

/* 0.20 the JAM Melody lane's reads (seq_core.h): the line render_mel cached, or NULL for a bar it has none of */
static const mjam_note_t *mel_line_at(const seq_t *s, int bar) {
    const seq_part_t *p = ccur(s);
    return mel_jam(p) && bar >= 0 && bar < p->bars ? &p->mline[bar * SEQ_STEPS] : NULL;
}

void seq_mel_line(const seq_t *s, int bar, mjam_note_t out[16]) {
    const mjam_note_t *l = mel_line_at(s, bar);
    if (l) memcpy(out, l, sizeof(mjam_note_t) * SEQ_STEPS);
    else memset(out, 0, sizeof(mjam_note_t) * SEQ_STEPS);
}

int seq_mel_frame(const seq_t *s, int bar, int step) {
    const mjam_note_t *l = mel_line_at(s, bar);
    return l ? mjam_frame(l, step) : 0;
}

int seq_mel_score(const seq_t *s, int bar) {
    const mjam_note_t *l = mel_line_at(s, bar);
    return l ? mjam_score(l) : 0;
}

int seq_mel_fixed_count(const seq_t *s, int bar) {
    int n = 0;
    if (!mel_line_at(s, bar)) return 0;
    for (int i = 0; i < SEQ_STEPS; i++) n += ccur(s)->mjam.fix[bar * SEQ_STEPS + i] != 0;
    return n;
}

int seq_mel_level(const seq_t *s) {
    int l = ccur(s)->lane[LANE_MEL_UP].gen.complexity / 20;
    return l > 4 ? 4 : l;
}

int seq_complexity(const seq_t *s, int lane) { return lane >= 0 && lane < SEQ_LANES ? ccur(s)->lane[lane].gen.complexity : 0; }

int seq_harmony(const seq_t *s) { return ccur(s)->harmony; }

int seq_can_undo(const seq_t *s) { return s->undo_n > 0; }

/* Parts */

int seq_part(const seq_t *s) { return s->cur_part; }

int seq_queued_part(const seq_t *s) { return s->queued; }

int seq_part_used(const seq_t *s, int part) { return part >= 0 && part < SEQ_NPARTS && (s->used_mask >> part & 1); }

/* On the UI thread: the part's first use copies the Verse (stale with it), and a stale part catches up here, so seq_block
 * only ever switches to a part that is ready. */
void seq_select_part(seq_t *s, int part) {
    if (part < 0 || part >= SEQ_NPARTS) return;
    if (part == s->cur_part || (s->playing && part == s->queued)) {   /* stopped, nothing is ever queued */
        if (s->queued >= 0) act(s);   /* a cancel */
        s->queued = -1;
        return;
    }
    txn_cancel(s);   /* 0.17: an open picker or browser is the part being left's */
    act(s);
    if (!seq_part_used(s, part)) {
        s->part[part] = s->part[0];
        s->used_mask |= 1 << part;
        s->stale_mask = (s->stale_mask & ~(1 << part)) | (s->stale_mask & 1) << part;
    }
    if (s->stale_mask >> part & 1) refresh(s, part);
    s->queued = part;
    if (!s->playing) switch_part(s, 0);
}

/* 0.12 move feedback */

int seq_last_move_text(const seq_t *s, char *buf, int len) { return copy_text(buf, len, s->move_text); }

void seq_set_names(seq_t *s, int on) {
    if (s->names == (on != 0)) return;
    act(s);
    s->names = on != 0;
}

int seq_names(const seq_t *s) { return s->names; }

static void said(seq_t *s, int lane, const char *text) {   /* lane: the move's own (its tag shows the text) */
    copy_text(s->move_text, sizeof s->move_text, text);
    s->move_lane = lane;
    s->move_secs = 4.0;
}

int seq_last_move_lane(const seq_t *s) { return s->move_text[0] ? s->move_lane : -1; }

/* A slot's chord as the move text names it, with its own colour: "iv7", "bVI", "Isus4", "vi(add9)" (theory_degree_c);
 * names: "Am7" */
static void chord_word(const seq_t *s, int deg, int src, int colour, char *buf, int len) {
    int sc = theory_slot_scale(s->scale, src);
    if (s->names) theory_chord_name_c(s->key, sc, deg, colour, buf, len);
    else theory_degree_c(s->key, s->scale, sc, deg, colour, buf, len);
}

/* 0.11 the melody RECIPE style on Melody 1 */

/* The type the RECIPE tab shows for a Melody 1 of generator g and recipe mel: the recipe's, a classic style's (Hook or
 * Arp), -1 for Manual */
static int mel_type_of(const seq_gen_t *g, const mel_recipe_t *mel) {
    return g->style == GEN_MEL_RECIPE ? mel->type : g->style == GEN_MEL_HOOK ? MEL_HOOK : g->style == GEN_MEL_ARP ? MEL_ARP : -1;
}

static int mel_shown_type(const seq_part_t *p) { return mel_type_of(&p->lane[LANE_MEL_UP].gen, &p->mel); }

/* mel_base on a generator and recipe: the recipe, or a classic or Manual lane's shown defaults */
static int mel_base_of(const seq_gen_t *g, const mel_recipe_t *mel, mel_recipe_t *r, int *cx) {
    int t = mel_type_of(g, mel);
    *r = *mel;
    *cx = g->complexity;
    if (g->style == GEN_MEL_RECIPE) return 0;
    mel_defaults(t < 0 ? MEL_HOOK : t, r, cx);
    return 1;
}

/* The recipe and complexity a tap edits: the lane's own, or on a classic or Manual lane the shown type's defaults (Manual:
 * Hook's; VST-12: its phrasing too), which the tap converts it to; returns 1 for a conversion */
static int mel_base(const seq_part_t *p, mel_recipe_t *r, int *cx) { return mel_base_of(&p->lane[LANE_MEL_UP].gen, &p->mel, r, cx); }

enum { MEL_NO_UNDO, MEL_UNDO, MEL_UNDO_KNOBS };   /* MEL_UNDO_KNOBS: the undo restores the knobs and complexity too */

static int mel_same(const seq_part_t *p, const mel_recipe_t *r, int cx) {
    return is_recipe(p, LANE_MEL_UP) && !memcmp(r, &p->mel, sizeof *r) && cx == p->lane[LANE_MEL_UP].gen.complexity;
}

/* Recipe r at complexity cx on Melody 1, which becomes GEN_MEL_RECIPE and regenerates (locked steps stay; a GATE turn
 * alone acts at playback, so no regeneration). Nothing changes: no-op, no undo. A conversion always pushes a recipe
 * snapshot, whatever `undo` says (its undo restores the classic or Manual lane, complexity included); on a recipe lane
 * `undo` picks the snapshot. */
static void mel_apply(seq_t *s, const mel_recipe_t *r, int cx, int undo) {
    seq_part_t *p = cur(s);
    seq_lane_t *ln = &p->lane[LANE_MEL_UP];
    mel_recipe_t was = p->mel;
    int conv = ln->gen.style != GEN_MEL_RECIPE;
    if (mel_same(p, r, cx)) return;
    if (conv || undo == MEL_UNDO_KNOBS) push_recipe_snap(s, s->cur_part, LANE_MEL_UP);
    else if (undo) push_undo(s, LANE_MEL_UP);
    was.kn[MK_GATE] = r->kn[MK_GATE];
    int same = !conv && cx == ln->gen.complexity && !memcmp(&was, r, sizeof was);
    p->mel = *r;
    ln->gen.style = GEN_MEL_RECIPE;
    ln->gen.complexity = cx;
    if (!same) regen_lane(s, p, LANE_MEL_UP);
}

/* One RECIPE-tab edit on the recipe mel_base gives (a classic or Manual lane's shown defaults): writes r and cx and
 * returns 1, or 0 when the edit is ignored (out of range, or not for this lane) */
typedef int (*mel_edit_fn)(const seq_part_t *p, mel_recipe_t *r, int *cx, int a, int b, int c);

static int edit_type(const seq_part_t *p, mel_recipe_t *r, int *cx, int t, int b, int c) {
    (void)p, (void)b, (void)c;
    if (t < 0 || t >= MEL_NTYPES) return 0;
    mel_defaults(t, r, cx);   /* VST-12: the type's phrasing too */
    return 1;
}

static int edit_opt(const seq_part_t *p, mel_recipe_t *r, int *cx, int which, int i, int c) {
    (void)p, (void)cx, (void)c;
    if (which < 0 || which > 2 || i < 0 || i >= mel_nopts(r->type, which)) return 0;
    r->opt[which] = (unsigned char)i;
    return 1;
}

static int edit_phrase(const seq_part_t *p, mel_recipe_t *r, int *cx, int ph, int b, int c) {
    (void)p, (void)cx, (void)b, (void)c;
    if (ph < 0 || ph >= MEL_NPHRASES) return 0;
    mel_phrase_from_legacy(r, ph);
    return 1;
}

static int edit_phrasing(const seq_part_t *p, mel_recipe_t *r, int *cx, int which, int v, int c) {
    (void)p, (void)cx, (void)c;
    if (v < 0 || v >= mel_phrase_n(which)) return 0;
    r->ph[which] = (unsigned char)v;
    return 1;
}

static int edit_flags(const seq_part_t *p, mel_recipe_t *r, int *cx, int rev, int inv, int shift) {
    (void)p, (void)cx;
    if (rev < 0 || rev > 1 || inv < 0 || inv > 1 || shift < 0 || shift > 15) return 0;
    r->rev = (unsigned char)rev, r->inv = (unsigned char)inv, r->shift = (unsigned char)shift;
    return 1;
}

/* A knob: a Manual lane has no recipe to turn (ignored); a classic one converts first */
static int edit_knob(const seq_part_t *p, mel_recipe_t *r, int *cx, int knob, int v, int c) {
    (void)cx, (void)c;
    if (knob < 0 || knob >= MK_COUNT || v < 0 || v > 100 || mel_shown_type(p) < 0) return 0;
    r->kn[knob] = (unsigned char)v;
    return 1;
}

static int edit_reset(const seq_part_t *p, mel_recipe_t *r, int *cx, int a, int b, int c) {
    mel_recipe_t d = *r;
    (void)p, (void)a, (void)b, (void)c;
    mel_defaults(r->type, &d, cx);
    memcpy(r->kn, d.kn, sizeof r->kn);
    return 1;
}

static int edit_reset_phrasing(const seq_part_t *p, mel_recipe_t *r, int *cx, int a, int b, int c) {
    mel_recipe_t d = *r;
    int dcx;
    (void)p, (void)cx, (void)a, (void)b, (void)c;
    mel_defaults(r->type, &d, &dcx);
    memcpy(r->ph, d.ph, sizeof r->ph);
    return 1;
}

static int mel_edited(const seq_part_t *p, mel_edit_fn edit, int a, int b, int c, mel_recipe_t *r, int *cx) {
    mel_base(p, r, cx);
    return edit(p, r, cx, a, b, c);
}

/* A RECIPE-tab tap: an ignored or re-sent one does nothing */
static void mel_tap(seq_t *s, mel_edit_fn edit, int a, int b, int c, int undo) {
    mel_recipe_t r;
    int cx;
    if (mel_jam(cur(s))) return;   /* 0.20: a JAM lane has no recipe (its RECIPE cells are retired) */
    if (!mel_edited(cur(s), edit, a, b, c, &r, &cx) || mel_same(cur(s), &r, cx)) return;
    act(s);
    mel_apply(s, &r, cx, undo);
}

void seq_mel_set_type(seq_t *s, int type) { mel_tap(s, edit_type, type, 0, 0, MEL_UNDO_KNOBS); }

void seq_mel_set_opt(seq_t *s, int which, int i) { mel_tap(s, edit_opt, which, i, 0, MEL_UNDO); }

void seq_mel_set_phrase(seq_t *s, int phrase) { mel_tap(s, edit_phrase, phrase, 0, 0, MEL_UNDO); }

void seq_mel_set_phrasing(seq_t *s, int which, int v) { mel_tap(s, edit_phrasing, which, v, 0, MEL_UNDO); }

void seq_mel_reset_phrasing(seq_t *s) { mel_tap(s, edit_reset_phrasing, 0, 0, 0, MEL_UNDO); }

void seq_mel_set_flags(seq_t *s, int rev, int inv, int shift, int undo) {
    mel_tap(s, edit_flags, rev, inv, shift, undo ? MEL_UNDO : MEL_NO_UNDO);
}

/* 0.20: a JAM lane's GATE (mgat) is its part's MK_GATE, a knob turn with no undo step (as PUSH and DYNAMICS); its other knobs are retired */
void seq_mel_set_knob(seq_t *s, int knob, int v) {
    if (mel_jam(cur(s))) {
        if (knob == MK_GATE) set_mel_knob(s, &cur(s)->mel.kn[MK_GATE], v);
        return;
    }
    mel_tap(s, edit_knob, knob, v, 0, MEL_NO_UNDO);
}

void seq_mel_reset_knobs(seq_t *s) { mel_tap(s, edit_reset, 0, 0, 0, MEL_UNDO_KNOBS); }

int seq_mel_type(const seq_t *s) { return mel_shown_type(ccur(s)); }

int seq_mel_is_classic(const seq_t *s) {
    int style = ccur(s)->lane[LANE_MEL_UP].gen.style;
    return style == GEN_MEL_HOOK || style == GEN_MEL_ARP;
}

int seq_mel_opt(const seq_t *s, int which) {
    mel_recipe_t r;
    int cx;
    mel_base(ccur(s), &r, &cx);
    return which >= 0 && which < 3 ? r.opt[which] : 0;
}

int seq_mel_phrase(const seq_t *s) {
    mel_recipe_t r;
    int cx;
    mel_base(ccur(s), &r, &cx);
    return mel_is_legacy(&r) ? mel_legacy_phrase(&r) : -1;
}

int seq_part_mel_phrasing(const seq_t *s, int part, int which) {
    mel_recipe_t r;
    int cx;
    if (part < 0 || part >= SEQ_NPARTS || which < 0 || which >= MP_COUNT) return 0;
    mel_base(&s->part[part], &r, &cx);
    return r.ph[which];
}

int seq_mel_phrasing(const seq_t *s, int which) { return seq_part_mel_phrasing(s, s->cur_part, which); }

int seq_mel_knob(const seq_t *s, int knob) { return seq_part_mel_knob(s, s->cur_part, knob); }

int seq_part_mel_knob(const seq_t *s, int part, int knob) {
    mel_recipe_t r;
    int cx;
    if (part < 0 || part >= SEQ_NPARTS) part = s->cur_part;
    if (mel_jam(&s->part[part]) && knob == MK_GATE) return s->part[part].mel.kn[MK_GATE];   /* 0.20: the JAM lane's GATE */
    mel_base(&s->part[part], &r, &cx);
    return knob >= 0 && knob < MK_COUNT ? r.kn[knob] : 0;
}

int seq_part_mel_type(const seq_t *s, int part) { return mel_shown_type(&s->part[part < 0 || part >= SEQ_NPARTS ? s->cur_part : part]); }

void seq_mel_flags(const seq_t *s, int *rev, int *inv, int *shift) {
    mel_recipe_t r;
    int cx;
    mel_base(ccur(s), &r, &cx);
    if (rev) *rev = r.rev;
    if (inv) *inv = r.inv;
    if (shift) *shift = r.shift;
}
