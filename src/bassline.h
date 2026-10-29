/* 0.19 BASS page: the bass note model. Pure: no host, no allocation. Decides which note each bass hit plays from the
 * rhythm, the chord, the next chord, the note settings and the bar's variation. It follows the canvas barLine
 * (tests/ref/bass_ref.js); tests/test_bassline.c checks it against that file's sweep. */
#ifndef BASSLINE_H
#define BASSLINE_H

enum { BK_MOV, BK_OCTS, BK_APP, BK_REG, BK_COUNT };   /* MOV, OCTS, APP 0..4; REG -2..2 (0 = Natural) */

typedef struct {
    signed char style;       /* 0..BASS_STYLE_COUNT-1, or -1 for none (Custom) */
    signed char pedal;       /* 0 off, 1 tonic, 2 dominant */
    signed char octave;      /* -1..1 */
    signed char k[BK_COUNT];
} bass_set_t;

#define BASS_STYLE_COUNT 48
#define BASS_GENRES 12

const char *bass_style_name(int i);
const char *bass_style_feel(int i);
int bass_style_genre(int i);             /* i / 4 */
const char *bass_genre_name(int g);
bass_set_t bass_style_set(int i);
int bass_set_edited(const bass_set_t *s);   /* 1 when pedal, octave or a k differs from the style's; 0 for style -1 */

typedef struct { int root, third, minor; } bass_chord_t;   /* pitch classes of the root and the 3rd; minor 0/1 */
typedef struct { int tonic; int scale[7]; } bass_key_t;    /* pitch classes: the tonic, and the scale from the tonic */

/* The nearest scale pitch class going down / up by semitones from pc, not pc itself. */
int bass_scale_below(const bass_key_t *k, int pc);
int bass_scale_above(const bass_key_t *k, int pc);

typedef struct {
    const unsigned char *codes;      /* the span's rhythm codes as they play (RC_*), length span, 1..16 */
    int span;
    const bass_chord_t *chord, *next;
    const bass_key_t *key;
    bass_set_t set;
    int v;                           /* the bar's NEW BAR variation */
    const unsigned char *fix;        /* the span's fixed notes: 0 none, else MIDI */
} bass_span_in_t;

/* Writes span entries. note: MIDI 1..127, or 0 for silent. role: 1 root, o root up an octave, 3, 5, p scale step,
 * c half step, P pedal, f fixed, - held, . rest. */
void bass_span(const bass_span_in_t *in, unsigned char note[], char role[]);

/* 0.24 the note pitch class pc plays as a root in settings s (REGISTER's fold and OCTAVE, no octave jump): MIDI 1..127 */
int bass_root_note(const bass_set_t *s, int pc);
/* 0.24 BASS FILL f's notes walking into `to` (MIDI, the next bar's first root), on rhythm_bass_fill's steps: PICKUP the half step below on
 * step 15; STEP-IN 3 chromatic notes up to it on 13-15; RUN 4 (-7, -5, -3, -1 semitones) on 12-15; each 1..127. NONE and BREAK write none */
void bass_fill_notes(int f, int to, unsigned char note[16]);

int bass_tension(int pc, const bass_chord_t *c, const bass_key_t *k);   /* 0..85 */
/* round half up (0.6 max + 0.4 mean) of the tension of the n steps' notes, each under chord_of_step[i]; 0 if none sound */
int bass_score(const unsigned char note[], int n, const bass_chord_t *const chord_of_step[], const bass_key_t *k);
int bass_level(int score);               /* 0..4 at 0 / 12 / 28 / 50 / 72 */
const char *bass_level_word(int level);  /* plain warm colourful tense clash */

#endif
