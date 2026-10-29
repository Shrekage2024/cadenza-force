/* 0.17 the Chords lane's named voicing styles: 13 genres of 4, genre-major (index = genre x 4 + position). A style sets the
 * connection mode, COLOUR, INVERSION, SPREAD, STRUM and the Chords lane OCTAVE (seq_set_voicing_style). Pure data, no state. */
#ifndef VOICING_H
#define VOICING_H

typedef struct {
    const char *genre, *name, *feel;   /* name: at most 14 characters (the 238 px tile at 24 px) */
    signed char mode;                  /* VMODE_* (theory.h) */
    signed char col;                   /* COLOUR level -1..4 */
    signed char inv;                   /* INVERSION -2..3 */
    signed char spr;                   /* SPREAD -1..3 */
    signed char str;                   /* STRUM step -8..8 (- is high to low) */
    signed char oct;                   /* the Chords lane OCTAVE -1..1 */
} voicing_style_t;

extern const voicing_style_t VOICING_STYLES[];
extern const int VOICING_NSTYLES;   /* 52 */
extern const int VOICING_NGENRES;   /* 13 */
#define VOICING_DEFAULT 44          /* Pop - Pop Piano: Smooth, all 0, which is 0.16's default sound */

#endif
