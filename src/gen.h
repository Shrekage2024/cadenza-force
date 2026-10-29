/* The generator: writes a lane's steps from a style, a seed and a COMPLEXITY. No host or ALSA code, no heap, no state.
 *
 * Each step gets fixed random numbers from a hash of the seed and the step's place in a repeating motif cell (so bars
 * repeat, with a varied bar at phrase ends); COMPLEXITY only moves thresholds, so the onsets at complexity c are all
 * still there at any higher c (develop, not re-roll) and turning the knob back restores the earlier result.
 * gen_lane_v1 is pinned by golden checksums (tests/test_gen.c, tests/test_core.c): saved generated lanes keep only
 * their recipe and locked steps, so changing its output changes the notes of saved songs.
 *
 * Version policy: never bump the global `gv` (0.5 refuses gv != 1, and a refused load lets MPC save defaults over the
 * song). New behaviour is a new style id. A style whose output must change gets an optional per-lane version field,
 * which older builds ignore (unknown fields are skipped), and keeps v1 for lanes without it.
 * The PROG golden is not save-critical: progressions are saved as degrees, so gen_progression_v1 may evolve; re-record
 * its golden. */
#pragma once
#include "seq_core.h"   /* seq_step_t, SEQ_STEPS, SEQ_LANE_STEPS */

typedef struct { unsigned s; } gen_rng;
void gen_seed(gen_rng *r, unsigned seed);          /* never leaves s == 0 */
unsigned gen_next(gen_rng *r);                     /* xorshift32 */

/* Ids are saved: append only, never renumber. */
enum {
    GEN_MANUAL = 0, GEN_CH_SUSTAIN, GEN_CH_PULSE, GEN_CH_STABS, GEN_BASS_ROOT, GEN_BASS_PUMP, GEN_BASS_WALK,
    GEN_MEL_HOOK, GEN_MEL_ARP, GEN_MEL_ANSWER,
    GEN_MEL_RECIPE,   /* 0.11: Melody 1's recipe line (melody.h, mel_generate_v1); gen_v1 writes nothing for it */
    /* 0.12 chord rhythms (gen_chords_v2, through gen_lane_v1 and the span calls): a fixed template per bar, or per span
     * start under 1-bar chords, plus ranked ghost hits from COMPLEXITY */
    GEN_CH_OFFBEAT, GEN_CH_CHARLESTON, GEN_CH_BALLAD, GEN_CH_FUNK, GEN_CH_HALFTIME, GEN_CH_PUSH,
    /* 0.18: the Chords lane's drawn bars (seq_part_t.rb), rendered by seq_core; the generator writes nothing for it */
    GEN_CH_DRAWN = 17,
    /* 0.19: the Bass lane's drawn bars (seq_part_t.rb) and note model (bassline.h), rendered by seq_core; the generator writes
     * nothing for it */
    GEN_BASS_DRAWN = 18,
    /* 0.20: the Melody lane's JAM block and phrase model (melody2.h), rendered by seq_core; the generator writes nothing for it */
    GEN_MEL_JAM = 19,
    GEN_NSTYLES
};
#define GEN_V1_STYLES GEN_MEL_RECIPE   /* the styles gen_v1 generates: 1 .. GEN_V1_STYLES - 1 */
#define GEN_CH2_FIRST GEN_CH_OFFBEAT   /* the 0.12 chord rhythms: GEN_CH2_FIRST .. GEN_CH2_LAST, Chords lane only */
#define GEN_CH2_LAST GEN_CH_PUSH
int gen_is_chords_v2(int style);       /* 1 for GEN_CH2_FIRST .. GEN_CH2_LAST */
int gen_chords_v2_variant(int style, int seed);   /* 0.13: the template (0..2) a new seed picks (GEN, NEW RHYTHM, NEW
                                                  * IDEA); 0 for other styles. Stored: gen_params_t.variant */

typedef struct {
    int style, seed, complexity;      /* style GEN_*; seed 0..65535; complexity 0..100 */
    int bars, chord_len;              /* the part's loop and chord length (steps) */
    const int *prog; int prog_len;    /* scale degrees of the part's progression */
    int key, scale;                   /* the project's (theory.h): the walk and the melodies place notes by pitch */
    const seq_step_t *lead;           /* Answer: Melody 1's steps (bars*16), or NULL for a standalone line; else NULL */
    int lane;                         /* LANE_*: a melody style on Melody 2 sits an octave above Melody 1 */
    int variant;                      /* 0.13: a 0.12 chord rhythm's template, 0..2 (saved with the lane; else 0) */
    const unsigned char *bar_var;     /* 0.16: per bar (SEQ_MAX_BARS), 0 the phrase's bar, else that bar's own variation (NEW
                                       * BAR): its motif index takes another cell, so only that bar changes; NULL: none */
} gen_params_t;

/* Writes steps[0 .. bars*16-1] (on, tone, oct, vel, len) for a Chords (CH_*), Bass (BASS_*) or Melody (MEL_*) style.
 * Steps past the loop are left untouched. GEN_MANUAL, GEN_MEL_RECIPE and unknown ids write nothing; the 0.12 chord
 * rhythms (GEN_CH2_FIRST .. GEN_CH2_LAST) write their own generator's steps (gen.c, gen_chords_v2), which their own
 * goldens pin.
 * Pure: the same params always give the same steps.
 * Melodies: onsets nest as complexity rises like every style's, but a note's pitch can change when a new onset appears
 * before it, since each pitch is chosen from the notes before it (voice-leading). */
void gen_lane_v1(const gen_params_t *p, seq_step_t *steps);

/* gen_lane_v1 and gen_span_v1 with a melody's rhythm cache: the same steps. A melody's rhythm (onsets, lengths,
 * velocities and what its pitches read of the weights) depends on everything but the key, scale and progression; the
 * cache keeps it with all those inputs (an Answer's: its lead's onsets and lengths) and is used only when every one
 * matches, else made again. Other styles ignore it. Not thread-safe: one cache per lane, used by one caller at a time. */
typedef seq_gen_cache_t gen_cache_t;
void gen_lane_cached_v1(const gen_params_t *p, gen_cache_t *c, seq_step_t *steps);
void gen_span_cached_v1(const gen_params_t *p, gen_cache_t *c, int step, seq_step_t *steps);

/* gen_lane_v1's steps for the one span (chord) holding `step`, the rest left untouched: what a tap on a generated lane
 * needs. Writes nothing for GEN_MANUAL or a step past the loop. Pure. Chords and Bass generate the span alone; a
 * melody's pitch chain crosses spans, so for melody styles it generates the whole loop's rhythm and the pitches up to
 * the span's end internally (exact: no pitch depends on a later one). */
void gen_span_v1(const gen_params_t *p, int step, seq_step_t *steps);

/* gen_lane_v1's steps for the spans from the loop's first up to the one holding step to - 1 (to past the loop: all of
 * them), the rest left untouched: a label's first bar without the whole loop. One set-up for all the spans, so cheaper
 * than gen_span_v1 per span. Chords and Bass only; a melody style generates the whole loop. Pure. */
void gen_spans_v1(const gen_params_t *p, int to, seq_step_t *steps);

/* 1 if a style's steps may depend on the key, the scale or the progression (gen_params_t's prog, key, scale).
 * A deny-list: MANUAL, the Chords styles, Root and Pump are known not to; every other id (the walk, which places its
 * approach notes by pitch, and any later style) counts as reading them. The others skip regenerating when only those
 * change. */
int gen_reads_harmony(int style);

/* 1 if a style follows a key change by transposition (gen_transpose_v1): the melodies, whose choices are all made on
 * pitches relative to the key's tonic, so in another key each note keeps its tone and moves with the centre. */
int gen_transposes(int style);

/* Turns `steps` that are gen_lane_v1's output for p in key `from_key` (all else as p) into its output for p, without
 * generating: only on-steps' octaves change. For a style gen_transposes names; else it writes nothing. An Answer's lead
 * (p->lead) must itself be generated output that moved the same way (a lead with hand-edited notes moves by its chords'
 * roots instead, and the Answer must regenerate). */
void gen_transpose_v1(const gen_params_t *p, int from_key, seq_step_t *steps);

/* A progression of `len` (2..8) scale degrees from `seed` and HARMONY `complexity` (0..100): starts on 0 (tonic) and
 * ends on a chord that leads home as the loop wraps (degree 3, 4 or 5; also 1 from complexity 34, 6 from 67).
 * Diatonic only in v1 (degrees 0..6).
 * Writes out[0..len-1] (len 1 writes the tonic alone, len < 1 nothing). Pure. Its golden checksum guards against
 * accidental changes only: progressions are saved as degrees, so it may evolve (re-record the golden). */
void gen_progression_v1(int seed, int complexity, int len, int out[]);

/* 0.12: gen_progression_v1's degrees into deg[0..len-1] with src[0..len-1] = 0, then above HARMONY 60, by the seed with
 * chance (harmony - 60) * 1.25 % (50 % at 100), one slot neither first nor last (len >= 3) becomes the scale's borrowed
 * chord b (1..3, by the seed): deg = theory_borrow_degree(scale, b), src = b. At harmony <= 60 deg is exactly
 * gen_progression_v1's. Pure; len < 1 writes nothing. */
void gen_progression_v2(int seed, int harmony, int len, int deg[], int src[], int scale);
