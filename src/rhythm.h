/* 0.18 RHYTHM page: the 24-rhythm library and the 16-step rhythm codes. Pure: no host, no allocation.
 * A bar is 16 codes, one per 16th: a rest, a hit (plain, accent or ghost), or a hold (the hit before rings on). */
#ifndef RHYTHM_H
#define RHYTHM_H
#include "seq_core.h"   /* seq_step_t */
#include "jamlib.h"   /* JAM_LEVELS */

enum { RC_REST, RC_HIT, RC_ACC, RC_GHOST, RC_HOLD };   /* strings: . x A g - */

#define RHYTHM_COUNT 24
#define RHYTHM_GROUPS 6

const char *rhythm_name(int i);
const char *rhythm_feel(int i);
int rhythm_group(int i);                 /* 0..RHYTHM_GROUPS-1; 4 rhythms each, in library order */
const char *rhythm_group_name(int g);
int rhythm_is_new(int i);                /* 1 for the 15 rhythms new in 0.18 */
void rhythm_codes(int i, unsigned char out[16]);

/* The libraries: the 0.18 Chords one (rhythm_name and the rest above mean RLIB_CHORDS) and the 0.19 Bass one. Both have
 * RHYTHM_COUNT rhythms in RHYTHM_GROUPS groups of 4 consecutive entries. */
enum { RLIB_CHORDS, RLIB_BASS };
const char *rhythm_lib_name(int lib, int i);
const char *rhythm_lib_feel(int lib, int i);
int rhythm_lib_group(int lib, int i);
const char *rhythm_lib_group_name(int lib, int g);
int rhythm_lib_is_new(int lib, int i);
void rhythm_lib_codes(int lib, int i, unsigned char out[16]);
/* 0.22 JAM index: a bar picked from a JAM block has from = RJAM_BASE + genre*JAM_LEVELS + level (64..93), in the lane's own
 * table (JL_CHORDS / JL_BASS = RLIB_CHORDS / RLIB_BASS). The lookups above take it: the block's name and codes, no group. */
#define RJAM_BASE 64
#define RJAM_END (RJAM_BASE + 7 * JAM_LEVELS)   /* exclusive: 64..98 is reserved for JAM indexes (a 7th genre fits) */
_Static_assert(RHYTHM_COUNT <= RJAM_BASE && RJAM_END <= 99 && JAM_COLS * JAM_LEVELS <= RJAM_END - RJAM_BASE,
               "JAM indexes sit above the library and below 99");
int rhythm_is_jam(int from);   /* 1 for 64..93 */
int rhythm_lib_find(int lib, const unsigned char c[16]);   /* the library index with these codes, else -1 */

int rhythm_parse(const char *s16, unsigned char out[16]);   /* "A x g - ." only, 16 chars; 0 on a bad string */
void rhythm_format(const unsigned char c[16], char out[17]);

/* Hit velocities: ACC 112, HIT 100, GHOST 60. A hit's len is 1 plus the HOLDs after it. Tone TONE_R, oct 0. */
void rhythm_to_steps(const unsigned char c[16], seq_step_t out[16]);
/* vel >= 108 ACC, <= 70 GHOST, else HIT; len - 1 steps after a hit become HOLD (cut at the next hit and the bar end). */
void rhythm_from_steps(const seq_step_t st[16], unsigned char out[16]);
int rhythm_step_code(const seq_step_t st[16], int k);   /* rhythm_from_steps' out[k] alone (a polled cell's one step) */
int rhythm_find(const unsigned char c[16]);   /* the library index with these codes, else -1 */
int rhythm_is_hit(int code);                  /* HIT, ACC or GHOST */
void rhythm_cut_holds(unsigned char c[16], int i);   /* the HOLDs right after step i become rests */

/* Bar transforms. Each reads in[16] and writes out[16] (they may not alias). */
int rhythm_weight(int step);   /* 16,1,4,2,8,1,4,2,12,1,4,2,8,1,4,2 */
/* COMPLEXITY's hits: below 50 keep max(1, round(1 + (n-1)*cx/50)) of the n hits (most important first, ACC +6, GHOST -6,
 * ties to the lower step; a removed hit clears its HOLDs); above 50 turn round(r*(cx-50)/50) of the r rests into HIT. */
void rhythm_hits(const unsigned char in[16], int cx, unsigned char out[16]);
/* SYNC, GHOSTS and FILL may land on a rest or a held step: a hit placed on a hold cuts the hit before it there and keeps
 * the holds after it. rhythm_hits (above 50) adds to rests only. */
void rhythm_sync(const unsigned char in[16], int k, unsigned char out[16]);     /* k -2..+3 */
void rhythm_ghosts(const unsigned char in[16], int g, unsigned char out[16]);   /* g 0..4: none, 1, 2, 4, all */
void rhythm_fill(const unsigned char in[16], int f, unsigned char out[16]);     /* f 0..4 */
/* 0.24 BASS FILL f (1 PICKUP, 2 STEP-IN, 3 RUN, 4 BREAK; the Chords keep rhythm_fill): a HIT on step 15 / on 13-15 / on 12-15 (bassline.h
 * bass_fill_notes gives them their notes), BREAK steps 8-15 rests; 0 (or any other) a copy */
void rhythm_bass_fill(const unsigned char in[16], int f, unsigned char out[16]);
/* 0.24 BREATH: the last n steps (0..16) rests (a hold there too: the hit before is cut at the gap) */
void rhythm_breath(const unsigned char in[16], int n, unsigned char out[16]);   /* in and out may alias */
/* 0.22 JAM: TIME tm -1 HALF (the first 8 codes each at 2i, the step after held if the code sounds, else a rest), 0 copy,
 * 1 DOUBLE (the even codes into the first half, copied into the second); a hold after a rest becomes a rest. VARIATION pos 0
 * copy, 1 SYNC +1, 2 the bar rotated 2 steps later, 3 FILL at fill_level. */
void rhythm_time(const unsigned char in[16], int tm, unsigned char out[16]);
void rhythm_vary(const unsigned char in[16], int pos, int fill_level, unsigned char out[16]);
int rhythm_lhl(const unsigned char c[16]);   /* Longuet-Higgins & Lee syncopation score of the (looping) bar */
/* 0.24 CHORDS NEW BAR: version v of a bar (v 0 copy). Odd v: each hit on steps 4, 8, 12 (beats 2-4) moves 2 steps earlier when both
 * steps before it are rests in `in` (the holds after it clear); even v: each rest on steps 2, 6, 10, 14 (the empty 8ths) becomes HIT */
void rhythm_newbar(const unsigned char in[16], int v, unsigned char out[16]);
/* 0.24 LOCK WITH's IN THE GAPS: a HIT on each 8th (steps 0, 2, .., 14) where `other` is a rest (a hold is not silent); a beat with none
 * gets one HIT on its first rest 16th; every other step a rest */
void rhythm_gaps(const unsigned char other[16], unsigned char out[16]);

/* COMPLEXITY layers: signed offsets in each target knob's own steps. Positive adds toward complex, negative moves
 * toward the knob's simple end (COL Power, SPR Close, SYNC -2, STRUM 0, GHOSTS 0, HUM 0) and stops there. `col` is
 * Colour 1 and 2; `tension` the Tension layer's COLOUR steps (11ths and 13ths), kept apart because it acts only on chords
 * where it makes no avoid note (theory_avoid). */
enum { CXT_COL, CXT_SPR, CXT_SYNC, CXT_STRUM, CXT_GHOSTS, CXT_HUM, CXT_COUNT };
typedef struct { int col, spr, sync, strum, ghosts, hum, smooth, tension; } cx_offsets_t;
cx_offsets_t rhythm_cx_offsets(int cx);
void rhythm_init(void);   /* makes rhythm_cx_offsets' table once (seq_init calls it); safe to call again, from any thread */
/* 1 when the Colour and Tension offsets are one over cx-2..cx+2 (clipped to 0..100), the range seq_core staggers a chord's
 * COMPLEXITY over by its rank */
int rhythm_cx_colour_flat(int cx);
/* The knob's effective value: user + offset, a negative offset stopping at the simple end (from either side), clamped
 * to the knob's range (COL -1..4, SPR -1..3, SYNC -2..3, STRUM -8..8, GHOSTS 0..4, HUM 0..4). A positive STRUM offset
 * moves away from 0: up when user >= 0, down when user < 0. */
int rhythm_cx_apply(int target, int user, int offset);
#endif
