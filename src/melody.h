/* The melody recipe generator (0.11, style GEN_MEL_RECIPE): one line that calls and answers itself, from a recipe (a
 * type, three options, a phrase, seven knobs and the move flags), a seed and a COMPLEXITY, over the chord under each
 * span. A port of the "Chord Seq final screens" canvas MELODY logic (MG.bar, MG.line, MG.rhythm, MG.contour,
 * MG.transform), generalized to any loop length, key, scale and chord. No host or ALSA code, no heap, no state.
 *
 * Steps are absolute (seq_step_t.abs): the recipe owns every pitch. Notes are folded by octaves into 50..96.
 * Saved: type and option indices are append only, never renumber. */
#pragma once

#include "mel_recipe.h"   /* the recipe: types, knobs, phrases, mel_recipe_t */
#include "seq_core.h"     /* seq_step_t */

/* A chord as pitch classes 0..11: tones[0] is the root (== root_pc), then the third (or sus tone), the fifth and an
 * optional colour tone (7th, 9th); ntones 3..4. seventh: the slot scale's tone 7 (pitch class). A power chord (0.17) is its root
 * and 5th: ntones 2, tones[1] and tones[2] both the 5th. */
typedef struct { int root_pc; int tones[4]; int ntones; int seventh; } mel_chord_t;

typedef struct {
    const mel_recipe_t *r; int seed, complexity;   /* seed 0..65535, complexity 0..100 */
    int bars, chord_len;                           /* the loop (1..16 bars) and its chord length in steps */
    int key, scale;                                /* the project's (theory.h): scale steps and the tonic */
    const mel_chord_t *chord;   /* per span (chord_len steps): the chord sounding; ceil(bars*16 / chord_len) of them */
    const unsigned char *bar_var;   /* 0.16: per bar (SEQ_MAX_BARS), 0 the phrase's bar, else its own variation (NEW BAR):
                                     * that bar's seed picks and cell differ, the rest of the line doesn't; NULL: none */
} mel_params_t;

extern const char *const MEL_TYPE_NAME[MEL_NTYPES];   /* "Arp", "Ostinato", ... "Approach" */
int mel_nopts(int type, int which);                    /* options in row `which` (0..2); 0 if out of range */
const char *mel_opt_name(int type, int which, int i);  /* "Up", "1/8", ... (ASCII, key-free); "" if out of range */
const char *mel_row_name(int type, int which);         /* "Direction", ...; "" if out of range */

/* A type's defaults (plan decision 2): r->type, its default options, its phrasing (VST-12: Arp and Ostinato Loop, Guide
 * tones Loop + Land, Pentatonic 2 + 2 Beat Land, Hook 1 + 1 Beat Q&A, Sequence 3 + 1 Beat Q&A, Approach 1 + 1 1/8 Land
 * Pickup), the seven knob starts and rev/inv/shift 0; *complexity (if not NULL) gets knob 1's start. */
void mel_defaults(int type, mel_recipe_t *r, int *complexity);

/* VST-12 phrasing (r->ph[MP_*]): each setting's count and names ("Loop", "Half", "Q & A", "Pickup"; "" out of range) */
int mel_phrase_n(int which);
const char *mel_phrase_name(int which, int i);
/* The 0.11-0.13 phrase (MEL_PHRASE_*) as phrasing: Form 1 + 1 or 2 + 2, Breath Beat, Cadence Land, Fill Rest; the
 * notes are those of the old phrase exactly. mel_legacy_phrase: the old phrase nearest r's (2 + 2 for Form 2 + 2, else
 * 1 + 1), which the save keeps for older builds; mel_is_legacy: 1 when r's phrasing is exactly an old phrase's. */
void mel_phrase_from_legacy(mel_recipe_t *r, int phrase);
int mel_legacy_phrase(const mel_recipe_t *r);
int mel_is_legacy(const mel_recipe_t *r);

/* Writes steps[0 .. bars*16-1]: on, abs (MIDI note 50..96), vel (100 on a beat, 90 off it), len (to the next onset,
 * wrapping the loop, at most 4); tone/oct are TONE_R/0 placeholders. Off steps are {0, TONE_R, 0, 100, 1, 0}.
 * Steps past the loop are left untouched. Pure: the same params always give the same steps. */
void mel_generate_v1(const mel_params_t *p, seq_step_t *steps);
