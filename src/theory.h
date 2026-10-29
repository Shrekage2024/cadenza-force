/* Keys, scales and chords as small fixed tables: what a chord-relative step means in a key. No state, no host code.
 * Degrees are 0-based scale degrees (0 = the tonic) and wrap by octave; tones are 1-based (TONE_R = the chord root,
 * TONE_3 = the third above it in the scale, 9 = the ninth). */
#pragma once

enum { SCALE_MAJOR, SCALE_MINOR, SCALE_DORIAN, SCALE_MIXOLYDIAN, SCALE_PHRYGIAN, SCALE_LYDIAN, SCALE_HARM_MINOR, SCALE_COUNT };
#define SCALE_V1_COUNT 4 /* the 0.6 scales: gen_v1 goldens are pinned on these */
enum { TONE_R = 1, TONE_2, TONE_3, TONE_4, TONE_5, TONE_6, TONE_7 };

int theory_degree_pc(int key, int scale, int deg);   /* pitch class 0..11 of a scale degree in key (0 = C) */

/* MIDI note of `tone` above the diatonic chord on `deg`, the chord root in octave 3 (C3 = 48) plus `octave` octaves:
 * tone 3 of degree 0 in E minor is G3 (55). */
int theory_tone_note(int key, int scale, int deg, int tone, int octave);

int theory_chord_name(int key, int scale, int deg, char *buf, int len);   /* "Em", "C", "F#dim"; returns the length */
int theory_note_name(int note, char *buf, int len);                       /* "E4"; returns the length */

/* A progression preset: up to 8 chords, as 0-based scale degrees. */
typedef struct { const char *name; int len; int deg[8]; } theory_preset_t;
extern const theory_preset_t THEORY_PRESETS[];
extern const int THEORY_NPRESETS;                            /* 16 */

const char *theory_key_name(int key);                        /* "C".."B" */
const char *theory_scale_name(int scale);                    /* "Major", "Minor", "Dorian", "Mixolydian" */

/* Close-voiced triad notes of degree `deg` (root position, root in octave 3), into out[3] ascending. */
void theory_triad(int key, int scale, int deg, int out[3]);

/* The voicing of `deg` that moves least from `prev` (3 ascending notes): try the 3 inversions, each shifted by
 * -12/0/+12, keep those whose lowest note is in 43..62, and pick the smallest sum of |out[i]-prev[i]|. Ties go to the
 * lower voicing. out ascending.
 * Changing what this returns changes how saved smooth projects sound (tests/test_core.c pins it with a checksum):
 * bump A.voi instead. */
void theory_voice_next(int key, int scale, int deg, const int prev[3], int out[3]);

/* 0.9 chord qualities. A colour changes the notes of a slot's chord; Triad is exactly theory_triad. */
enum { COL_TRIAD, COL_7TH, COL_SUS2, COL_SUS4, COL_ADD9, COL_POWER, COL_9TH, COL_11TH, COL_13TH, COL_6TH, COL_COUNT };   /* append only: saved as digits */
#define THEORY_NBORROW 3
#define THEORY_CHORD_MAX 6   /* notes in a chord: a 9th, 11th or 13th has 5, and CLUSTER adds one */
extern const char *const THEORY_COLOUR_NAME[COL_COUNT];   /* "Triad","7th","sus2","sus4","add9","Power","9th","11th","13th","6th" */

/* The scale and degree of a project scale's borrowed chord b (1..3): the same degree from another scale. */
int theory_borrow_scale(int scale, int b);
int theory_borrow_degree(int scale, int b);
/* The scale a slot's chord comes from: src 0 = the project scale, 1..3 = theory_borrow_scale(scale, src) */
int theory_slot_scale(int scale, int src);
/* The tone a lane plays for `tone` on a chord of colour c: sus2/sus4 turn tone 3 into 2/4; else tone */
int theory_colour_tone(int colour, int tone);
/* MIDI notes of the chord in root position, root in octave 3 (like theory_triad), ascending; returns the count (2..5).
 * Tones of the chord's own scale: Power 1-5; Triad 1-3-5; 7th 1-3-5-7; 9th 1-3-5-7-9; 11th 1-3-7-9-11; 13th 1-3-7-9-13; 6th 1-3-5-6;
 * add9 1-3-5-9; sus2 / sus4 swap the 3 */
int theory_chord_notes(int key, int scale, int deg, int colour, int out[THEORY_CHORD_MAX]);
/* The COLOUR knob's level -1..4 as a colour: Power, Triad, 7th, 9th, 11th, 13th; anything else Triad */
int theory_colour_of_level(int level);
/* "Em7", "F#m7b5", "Asus4", "Cadd9", "E5", "Em9", "Cmaj13", "D11", "Em6" (ASCII); returns the length */
int theory_chord_name_c(int key, int scale, int deg, int colour, char *buf, int len);
/* The numeral of a chord on deg from `slot_scale`, in a project in `scale`: "i", "bII", "IV", "ii\xC2\xB0". Case and the
 * dim sign (0.13: THEORY_DIM, the UTF-8 degree sign MPC's font draws; 0.12 "o") or + come from the chord's triad; b/# when
 * its root is a semitone below/above the project scale's same degree. Returns the length in bytes. */
#define THEORY_DIM "\xC2\xB0"
int theory_numeral(int key, int scale, int slot_scale, int deg, char *buf, int len);
/* 0.13: characters in UTF-8 text (the degree sign and the middle dot count one each), for the card and pin widths */
int theory_text_width(const char *text);
/* The chord as a degree (the slot labels, the move text): the numeral, then a 7th as a suffix, "iv7", "Imaj7", "iim7b5",
 * "vii\xC2\xB07", "III+maj7" (a 7th names its own seventh: on a diminished triad the dim sign becomes m7b5 or stays for
 * the full diminished 7th, an augmented one keeps its +), or 0.13 a sus or add9 after a space, the numeral whole:
 * "vi\xC2\xB0 sus4", "IV add9". Returns the full length in bytes, as snprintf. */
int theory_degree_c(int key, int scale, int slot_scale, int deg, int colour, char *buf, int len);
/* The IDEAS cards' compact chord (fd_, pn_). Degrees (0.13): the numeral whole, its colour in brackets right after it,
 * "(7)" a minor or dominant 7th, "(M7)" a major 7th, "(\xC2\xB07)" a diminished 7th, "(9)" add9, "(s2)", "(s4)":
 * "i(7)", "vi\xC2\xB0(s4)", "IV(9)", "I(M7)"; at most 9 characters. Names (as 0.12): the triad's name with "7", "M7",
 * "9", "sus2", "sus4" (sus drops the m/dim; a diminished triad's 7th "m7b5" or "o7"): "Am7", "CM7", "C9", "F#m7b5"; at
 * most 8. Returns the full length in bytes, as snprintf. */
int theory_chord_card(int key, int scale, int slot_scale, int deg, int colour, int names, char *buf, int len);
/* The voicing of an n-note chord (notes from theory_chord_notes) that moves least from prev (np notes): every inversion,
 * each shifted by -12/0/+12, lowest note in 43..62, smallest sum over i of |out[i] - prev[min(i, np-1)]|, ties to the
 * lower (lowest note, then the sum of notes). out ascending. For n == 3 and np == 3 on a triad, the result equals
 * theory_voice_next's (the pinned checksum). Returns n. */
int theory_voice_chord(const int notes[THEORY_CHORD_MAX], int n, const int prev[THEORY_CHORD_MAX], int np, int out[THEORY_CHORD_MAX]);

/* 0.17 voicing modes: how a chord is led from the one before */
enum { VMODE_CLOSE, VMODE_SMOOTH, VMODE_TOPLINE, VMODE_ROOTLESS, VMODE_COUNT };
/* CLOSE: the notes as given. SMOOTH: theory_voice_chord. TOPLINE: as SMOOTH, scored |top - prev's top| x 100 + the sum, so the
 * top note moves least. ROOTLESS: notes[0] dropped when n >= 4, then SMOOTH. out ascending; returns the count. */
int theory_voice_mode(const int notes[THEORY_CHORD_MAX], int n, const int prev[THEORY_CHORD_MAX], int np, int mode, int out[THEORY_CHORD_MAX]);

/* 0.17 drift: how far a chord sits from the key, 0..100. +30 borrowed (slot_scale != scale), +15 a diminished triad, the colour
 * (7th 10, sus2 / sus4 / add9 / 6th 12, 9th 22, 11th 38, 13th 50), +20 when a 9th, 11th, 13th or 6th is a semitone above the
 * triad's root, 3rd or 5th (the avoid note, the 5th counted even if the voicing omits it), +22 `cluster`, +2 per step of
 * |inversion|. */
int theory_drift(int key, int scale, int slot_scale, int deg, int colour, int cluster, int inversion);
/* 0.18: 1 when the colour's 9th, 11th, 13th or 6th sits a semitone above the triad's root, 3rd or 5th (the avoid note): theory_drift's
 * +20 (COMPLEXITY's Tension layer skips these chords) */
int theory_avoid(int key, int scale, int slot_scale, int deg, int colour);
/* 0 close (< 16), 1 colourful (< 40), 2 tense (< 65), 3 dissonant */
int theory_drift_level(int score);
