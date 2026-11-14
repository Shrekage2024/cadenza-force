/* 0.28 the Feel page's music theory: which chords fit where, and how one chord moves to the next, in any key and scale. Pure, no state:
 * the UIs ask, and the words are theirs. A chord is (deg, src, colour) as a slot holds it (theory.h). */
#ifndef FEEL_H
#define FEEL_H

#include "theory.h"
#include "voicing.h"

typedef struct { int deg, src, colour; } feel_chord_t;

/* The ladder's rungs */
enum { FEEL_HOME, FEEL_WARM, FEEL_COLOURFUL, FEEL_TENSE, FEEL_OUTSIDE };
/* Why an option is on the ladder (the UI says what it will hear) */
enum { FK_WRITTEN, FK_SAME_JOB, FK_RICHER, FK_BORROWED, FK_LEANS, FK_CLIMBS, FK_SLIDES, FK_LIFT, FK_KEPT };
typedef struct { feel_chord_t c; int level, kind; } feel_option_t;
#define FEEL_OPTIONS_MAX 16

/* Every chord that fits between prev and next, lowest level first, no chord twice: Home the written chord and the degrees that do its
 * job; Warm it richer; Colourful the scale's borrows; Tense the V7 and the diminished 7th of next; Outside next's tritone sub and a
 * chromatic mediant from prev. A chromatic result that is really a chord of the scale comes back as that chord. Returns the count. */
int feel_ladder(int key, int scale, feel_chord_t base, feel_chord_t prev, feel_chord_t next, feel_option_t out[FEEL_OPTIONS_MAX]);
/* cur's place on that ladder: its level and, in *kind, why. Not on it (kept from before a neighbour changed): Tense when it has notes outside
 * the scale, else Warm, FK_KEPT */
int feel_level(int key, int scale, feel_chord_t cur, feel_chord_t base, feel_chord_t prev, feel_chord_t next, int *kind);
/* 1 when the two are the same root and the same notes */
int feel_same(int key, int scale, feel_chord_t a, feel_chord_t b);

/* How a moves to b */
enum { FF_HOLDS, FF_PULLS, FF_LANDS_HARD, FF_SLIDES_IN, FF_FALLS, FF_CLIMBS_IN, FF_CLIMBS, FF_SETTLES, FF_OPENS, FF_STEPS, FF_SHARES, FF_FAR_APART };
int feel_flow(int key, int scale, feel_chord_t a, feel_chord_t b);
int feel_shared(int key, int scale, feel_chord_t a, feel_chord_t b);   /* notes they have in common */

/* A chord of the scale (not its degree 7) that leads from here into next: + uses it */
feel_chord_t feel_suggest(int key, int scale, feel_chord_t here, feel_chord_t next);
/* Which chord + splits: sel when it lasts at least half a bar, else the longest, -1 when none can (steps[n], in 16ths) */
int feel_add_target(const int *steps, int n, int sel);

/* The chord's notes outside the project scale, as pitch classes; returns the count */
int feel_out_of_key(int key, int scale, feel_chord_t c, int pcs[THEORY_CHORD_MAX]);
/* A note's job in the chord (TONE_R, TONE_3, TONE_5, TONE_7, TONE_2 the 9th, TONE_4, TONE_6), and a job's pitch class in it */
int feel_role_of(int key, int scale, feel_chord_t c, int note);
int feel_role_pc(int key, int scale, feel_chord_t c, int role);

/* Two low notes too close for the ear to part (ascending notes; below E3, under a 5th apart): the lower's index, -1 none */
int feel_muddy(const int *notes, int n);

/* How far the hands travel from a to b (ascending notes): 0 stay, 1 move, 2 jump */
enum { FH_STAY, FH_MOVE, FH_JUMP };
int feel_hand_word(const int *a, int na, const int *b, int nb);

/* A voicing style of the engine's table, 0..VOICING_NSTYLES - 1 (NULL past it) */
const voicing_style_t *feel_style(int i);
int feel_styles(void);
/* A progression preset's name, 0..feel_presets() - 1 ("" past it) */
const char *feel_preset_name(int i);
int feel_presets(void);

#endif
