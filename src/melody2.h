/* 0.20 MELODY: the JAM block and phrase model. Pure: no host, no allocation. A bar's 16 notes come from the part's melody
 * settings, the chords under the loop's steps and the bar's NEW BAR variation. It follows the canvas script
 * (tests/ref/melody_ref.js); tests/test_melody2.c checks it against that file's three sweeps.
 * Degrees: 0 = the key's tonic in octave 4. A pattern note whose offset from the bar's chord root degree is 0, 2 or 4
 * (mod 7) is a chord-tone slot: it plays the root, 3rd or 5th of the chord under the note, nearest its scale note. */
#ifndef MELODY2_H
#define MELODY2_H

#ifndef SEQ_STEPS
#define SEQ_STEPS 16   /* as seq_core.h (which this header must not include: seq_core.h will include it) */
#endif

enum { MJ_ARP, MJ_OST, MJ_HOOK, MJ_LEAD, MJ_RUN, MJ_PEDAL, MJ_COLS };
#define MJ_LEVELS 5
enum { MF_LOOP, MF_1_1, MF_2_2, MF_3_1, MF_AAB, MF_SENT, MF_COUNT };
enum { MA_ECHO, MA_SHIFT, MA_MIRROR, MA_RHYTHM, MA_TAIL, MA_SWAP, MA_COUNT };

/* c 0..5, l 0..4 the block; form 0..5; breath, cad, fill 0..3; ans 0..5; stray 0..3; aoct -1..1 (answers' octave);
 * reg 0..2 (Low, Middle, High); sc 0..5 / sl 0..4 the SWAP block. Out-of-range values are clamped.
 * 0.21 (0 = neutral, so a zeroed set sounds as 0.20): dens -2 SPARSE .. 2 BUSY, rng -1 NARROW .. 2 HUGE, var 0 OFF .. 3 ALL;
 * tm -1 HALF, 0 NORMAL, 1 DOUBLE (TIME, Amendment 1). */
typedef struct { signed char c, l, form, breath, cad, fill, ans, stray, aoct, reg, sc, sl, dens, rng, var, tm; } mjam_set_t;
mjam_set_t mjam_default(void);   /* ARP · UP, LOOP, breath None, CADENCE HOME, FILL REST, ECHO, LAST BEAT, 0, Middle, RUN · RUN-UP */

const char *mjam_col_name(int c);          /* ARP OSTINATO HOOK LEAD RUN PEDAL */
const char *mjam_block_name(int c, int l); /* UP, MOTIF, HOLD 5 ... */
const char *mjam_form_name(int f);         /* LOOP 1+1 2+2 3+1 AAB SENT */
const char *mjam_answer_name(int a);       /* ECHO SHIFT MIRROR RHYTHM TAIL SWAP */
const char *mjam_breath_name(int b);       /* None 1/8 Beat Half */
const char *mjam_cad_name(int c);          /* OFF OPEN SOFT HOME */
const char *mjam_fill_name(int f);         /* REST PICKUP STEP-IN RUN */
const char *mjam_stray_name(int s);        /* LAST NOTE, LAST BEAT, LAST HALF, ALL */
const char *mjam_dens_name(int d);         /* SPARSE THIN AS DRAWN FILLED BUSY for -2..2 */
const char *mjam_rng_name(int r);          /* NARROW AS DRAWN WIDE HUGE for -1..2 */
const char *mjam_var_name(int v);          /* OFF BAR 4 BARS 2+4 ALL BARS */
const char *mjam_time_name(int t);         /* HALF NORMAL DOUBLE for -1..1 */

typedef struct { int root, third, fifth; } mjam_chord_t;   /* pitch classes; -1 = none (the scale's note plays there) */
typedef struct { int key, scale; const mjam_chord_t *chord[SEQ_STEPS]; } mjam_bar_in_t;   /* the chord under each step */
/* bar[b] for b < bars; v[b] the bar's NEW BAR variation (v may be NULL: all 0). The model reads key and scale from here;
 * a bar's key and scale must equal them. */
typedef struct { int key, scale, bars; mjam_set_t set; const mjam_bar_in_t *bar; const unsigned char *v; } mjam_loop_in_t;
/* note: MIDI, 0 = silent; hold 1 = a held step (continues the note before); tension 0..97 (a held step: its note's) */
typedef struct { unsigned char note, hold, tension; } mjam_note_t;

/* One bar with `set` (a locked bar passes its frozen set), notes folded into 36..100. FILL's target, the next bar's first
 * note, comes from bar (b + 1) % bars with in->set and v 0. */
void mjam_bar(const mjam_loop_in_t *in, int b, const mjam_set_t *set, mjam_note_t out[SEQ_STEPS]);
/* As mjam_bar without the fold into 36..100: the reference's own notes (clipped to 1..127). Tests only. */
void mjam_bar_ref(const mjam_loop_in_t *in, int b, const mjam_set_t *set, mjam_note_t out[SEQ_STEPS]);
/* 1 if any bar of the loop under in->set (and in->v) has a note on an odd step before BREATH, CADENCE and FILL (the 16th swing grid) */
int mjam_swing16(const mjam_loop_in_t *in);
/* 0.21 mjam_swing16's test of bar b alone; mjam_bar_odd: as mjam_bar, and *odd the same test of the bar under `set` (with set equal to
 * in->set: mjam_swing16_bar's answer without shaping the bar again) */
int mjam_swing16_bar(const mjam_loop_in_t *in, int b);
void mjam_bar_odd(const mjam_loop_in_t *in, int b, const mjam_set_t *set, mjam_note_t out[SEQ_STEPS], int *odd);
/* The raw block c, l on bar b: no phrase rules, REGISTER Middle (the JAM pictures). */
void mjam_block_bar(const mjam_loop_in_t *in, int c, int l, int b, mjam_note_t out[SEQ_STEPS]);

/* The tension 0..97 of MIDI note `note` at bar step `step` (0..15) over chord ch in the key: as the model scores its own notes
 * (a fixed note's) */
int mjam_tension(int key, int scale, const mjam_chord_t *ch, int note, int step);
int mjam_frame(const mjam_note_t line[SEQ_STEPS], int i);   /* the dot frame 0..72: 0 rest, 1 + level * 5 + band, 61 + level */
int mjam_score(const mjam_note_t line[SEQ_STEPS]);          /* round half up (0.6 max + 0.4 mean) of the notes' tension; 0 if none */
int mjam_role(int form, int pos, int *src_pos);   /* 'C' 'A' 'R' 'S' 'F' of phrase bar pos 0..3; src_pos: the bar it plays */
/* 0.21 the metric strength of bar step p 0..15 (DENSITY's THIN, the JAM velocities): 4 the downbeat, 3 beat 3, 2 beats 2 and 4, 1 the
 * other 8ths, 0 the 16ths */
int mjam_strength(int p);   /* metric strength of bar step p: 0 -> 4, mod 8 -> 3, mod 4 -> 2, mod 2 -> 1, else 0 */
/* The chord's real root, 3rd and 5th (its triad), never COLOUR's extensions */
void mjam_chord_of(int key, int scale, int slot_scale, int deg, mjam_chord_t *out);

#endif
