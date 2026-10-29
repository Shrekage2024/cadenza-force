/* 0.22 JAM pages on CHORDS and BASS: the block tables. 6 genre columns (HOUSE TECHNO TRANCE POP ROCK DISCO) x 5 levels
 * (simple to busy) per lane. A block = a name (at most 10 characters), a 16-code rhythm (the RHYTHM page's codes . x A g -)
 * and note settings: a CHORDS block the fields of a voicing style, a BASS block the fields of a bass style. Pure data. */
#ifndef JAMLIB_H
#define JAMLIB_H

enum { JL_CHORDS, JL_BASS, JL_COUNT };
#define JAM_COLS 6
#define JAM_LEVELS 5

typedef struct {
    const char *name, *codes;
    signed char mode, col, inv, spr, str, oct;   /* VMODE_*, COLOUR -1..4, INVERSION -2..3, SPREAD -1..3, STRUM -8..8, OCTAVE -1..1 */
} jam_chords_t;

typedef struct {
    const char *name, *codes;
    signed char pedal, mov, octs, app, reg, octave;   /* pedal 0..2, MOVEMENT 0..4, OCTAVES 0..4, APPROACH 0..4, REGISTER -2..2, octave -1..1 */
} jam_bass_t;

const char *jam_genre_name(int c);                       /* all of these clamp c and l */
const jam_chords_t *jam_chords(int c, int l);
const jam_bass_t *jam_bass(int c, int l);
const char *jam_block_name(int lane, int c, int l);
const char *jam_block_codes(int lane, int c, int l);

int jam_bass_level(int midi);                         /* 0..11: the bass pictures' height of a MIDI note (24..59) */

#endif
