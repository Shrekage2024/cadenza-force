/* Offline test of the Bass's Feel and Tweak engine (seq_bass_*, seq_tweak_* on LANE_BASS): notes picked as a tone of the chord. Run by
 * tests/run_tests.sh under ASan/UBSan. Exit 1 on failure. */
#include <stdio.h>
#include <string.h>
#include "seq_core.h"
#include "feel.h"
#include "rhythm.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c) != 0; printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static void rec(void *ctx, int status, int note, int vel, int frame) { (void)ctx, (void)status, (void)note, (void)vel, (void)frame; }

static seq_t a, b;   /* a seq_t is large: never on the stack */
static char state[SEQ_STATE_MAX];

/* C G Am F, a bar each, in C major, Pop Root, the Bass on a rhythm with hits on 0, 4, 8, 12 */
enum { C_, G_, AM, F_, POP_ROOT = 40 };
static void paint_all(seq_t *s, const char *codes) {
    for (int bar = 0; bar < 4; bar++)
        for (int i = 0; i < 16; i++) seq_tweak_paint(s, LANE_BASS, bar, i, codes[i] == 'x' ? RC_HIT : RC_REST);
}
static void fresh(seq_t *s) {
    static const int DEG[4] = {0, 4, 5, 3};
    static const unsigned char SRC[4] = {0}, COL[4] = {COL_TRIAD, COL_TRIAD, COL_TRIAD, COL_TRIAD};
    seq_init(s, rec, NULL);
    seq_init_preset(s);
    seq_set_rate(s, 44100);
    seq_set_key(s, 0);
    seq_set_scale(s, 0);
    seq_set_bars(s, 4);
    seq_set_progression(s, 4, DEG, SRC, COL, 0);
    seq_set_bass_style(s, POP_ROOT);
    paint_all(s, "x...x...x...x...");
}

static const char *NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *line(const seq_t *s, int bar) {   /* "C . . . C . . ." the bar's notes by name */
    static char out[64];
    unsigned char n[16];
    char r[16];
    int o = 0;
    seq_bass_line(s, bar, n, r);
    for (int i = 0; i < 16; i++) o += snprintf(out + o, sizeof out - (size_t)o, "%s%s", i ? " " : "", n[i] ? NAME[n[i] % 12] : ".");
    return out;
}
static int first(const seq_t *s, int bar) {
    unsigned char n[16];
    char r[16];
    seq_bass_line(s, bar, n, r);
    for (int i = 0; i < 16; i++) if (n[i]) return n[i];
    return 0;
}
static int tone_of(const seq_t *s, int i) {
    seq_feel_chord_t c[64];
    int n = seq_feel_chords(s, c, 64);
    return i < n ? c[i].tone : -1;
}

/* What chord i's tone plays (0: as it plays now); the next chord's nearest tone to chord i's note */
static int tone_note(const seq_t *s, int i, int tone) {
    static unsigned char o[64 * BCN_SIZE];
    return i < seq_bass_chord_notes(s, o, 64) ? o[i * BCN_SIZE + tone] : 0;
}
static int step_tone_of(const seq_t *s, int i) { return tone_note(s, i, BCN_STEP); }

/* A chord's picked note: heard on its first hit, kept as a tone of the chord */
static void test_tone(void) {
    fresh(&a);
    CHECK(!strcmp(line(&a, G_), "G . . . G . . . G . . . G . . ."), "a new song plays roots: %s", line(&a, G_));
    CHECK(tone_of(&a, G_) == 0, "no tone saved until one is picked");
    seq_bass_tone_set(&a, G_, TONE_3);
    CHECK(!strcmp(line(&a, G_), "B . . . B . . . B . . . B . . ."), "the 3rd under G is B: %s", line(&a, G_));
    CHECK(!strcmp(line(&a, C_), "C . . . C . . . C . . . C . . ."), "the other chords stay: %s", line(&a, C_));
    CHECK(tone_of(&a, G_) == TONE_3, "the tone reads back");
    seq_undo(&a);
    CHECK(tone_of(&a, G_) == 0 && !strcmp(line(&a, G_), "G . . . G . . . G . . . G . . ."), "one undo puts the root back");

    /* it stays what it is when the key changes */
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_set_key(&a, 2);   /* D major: G becomes A, its 3rd C# */
    CHECK(first(&a, G_) % 12 == 1, "in D the same chord's 3rd is C#: %s", line(&a, G_));
    seq_set_key(&a, 0);

    /* and when the chord changes */
    seq_feel_pick(&a, G_, 1, 0, COL_TRIAD);   /* G to Dm */
    CHECK(first(&a, G_) % 12 == 5, "under Dm the 3rd is F: %s", line(&a, G_));

    /* a 5th and a 7th read the chord's own scale */
    fresh(&a);
    seq_feel_pick(&a, G_, 6, 0, COL_TRIAD);   /* B dim */
    seq_bass_tone_set(&a, G_, TONE_5);
    CHECK(first(&a, G_) % 12 == 5, "the 5th of B dim is F, in key: %s", line(&a, G_));
    seq_bass_tone_set(&a, G_, TONE_7);
    CHECK(first(&a, G_) % 12 == 9, "the 7th of B dim is A: %s", line(&a, G_));

    /* bad values do nothing */
    fresh(&a);
    unsigned long u = (unsigned long)a.undo_pushes;
    seq_bass_tone_set(&a, 99, TONE_3);
    seq_bass_tone_set(&a, -1, TONE_3);
    seq_bass_tone_set(&a, G_, 0);
    seq_bass_tone_set(&a, G_, 8);
    seq_bass_tone_set(&a, G_, TONE_R);   /* it already is */
    CHECK((unsigned long)a.undo_pushes == u && tone_of(&a, G_) == 0, "bad values and the same tone add no undo step");
}

/* The line aims at the note you picked */
static void test_targets(void) {
    fresh(&a);
    seq_set_bknob(&a, BK_APP, 2);   /* half below */
    CHECK(!strcmp(line(&a, C_), "C . . . C . . . C . . . F# . . ."), "half below G: %s", line(&a, C_));
    seq_bass_tone_set(&a, G_, TONE_3);
    CHECK(!strcmp(line(&a, C_), "C . . . C . . . C . . . A# . . ."), "half below the B now under G: %s", line(&a, C_));

    /* Ending's run aims there too */
    fresh(&a);
    seq_bass_tone_set(&a, C_, TONE_3);   /* E under the first chord: bar 4's fill leads to it */
    seq_tweak_set(&a, LANE_BASS, TW_ENDING, 3);   /* Step in */
    unsigned char n[16];
    seq_tweak_notes(&a, F_, n);
    CHECK(n[15] % 12 == 3, "the last note of Step in is a half step below E: %d", n[15]);
    CHECK(n[13] % 12 == 1 && n[14] % 12 == 2, "and climbs by half steps");

    /* Early brings in the picked note */
    fresh(&a);
    paint_all(&a, "x...x...x.......");
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_tweak_set(&a, LANE_BASS, TW_EARLY, AN_8);
    seq_bass_line(&a, C_, n, (char[16]){0});
    CHECK(n[14] % 12 == 11, "an 8th before the change, B: %s", line(&a, C_));
}

/* All roots, a style and Stays on */
static void test_clearing(void) {
    fresh(&a);
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_bass_tone_set(&a, F_, TONE_5);
    seq_bass_tones_clear(&a);
    CHECK(tone_of(&a, G_) == 0 && tone_of(&a, F_) == 0 && first(&a, G_) % 12 == 7, "All roots");
    seq_undo(&a);
    CHECK(tone_of(&a, G_) == TONE_3 && tone_of(&a, F_) == TONE_5 && first(&a, G_) % 12 == 11, "one undo puts every pick back");

    int was = seq_bass_style(&a);
    seq_bass_feel_style(&a, 2);   /* Disco Walk */
    CHECK(seq_bass_style(&a) == 2 && tone_of(&a, G_) == 0, "a style clears each chord's pick");
    seq_undo(&a);
    CHECK(seq_bass_style(&a) == was && tone_of(&a, G_) == TONE_3 && first(&a, G_) % 12 == 11, "one undo puts the style and the picks back");
    seq_bass_feel_style(&a, 99);
    CHECK(seq_bass_style(&a) == was, "a bad style does nothing");

    /* Stays on: the picks wait */
    seq_bass_row(&a, BL_STAYS, 1);
    CHECK(first(&a, G_) % 12 == 0 && tone_of(&a, G_) == TONE_3, "Key note under G, the pick kept: %s", line(&a, G_));
    CHECK(tone_note(&a, G_, BCN_NOW) % 12 == 0 && tone_note(&a, G_, TONE_3) % 12 == 11, "the line reads the pedal, the key its own tone");
    seq_bass_row(&a, BL_STAYS, 0);
    CHECK(first(&a, G_) % 12 == 11, "off again, the pick is back");
}

/* The Line rows: each word sets the engine's own setting, one undo step */
static void test_rows(void) {
    static const int COUNT[BL_COUNT] = {5, 5, 5, 5, 3, 3};
    for (int w = 0; w < BL_COUNT; w++) {
        CHECK(seq_bass_row_words(w) == COUNT[w], "row %d has %d words", w, COUNT[w]);
        for (int v = 0; v < COUNT[w]; v++) {
            fresh(&a);
            int was = seq_bass_row_get(&a, w);
            seq_bass_row(&a, w, v);
            CHECK(seq_bass_row_get(&a, w) == v, "row %d word %d reads back", w, v);
            if (v != was) {
                seq_undo(&a);
                CHECK(seq_bass_row_get(&a, w) == was, "row %d word %d: undo", w, v);
            }
        }
    }
    fresh(&a);
    seq_bass_row(&a, BL_MOVES, 2);
    CHECK(seq_bknob(&a, BK_MOV) == 2, "Moves is MOVEMENT");
    seq_bass_row(&a, BL_LOWEST, 0);
    CHECK(seq_bknob(&a, BK_REG) == -2, "Lowest note's first word is REGISTER -2");
    seq_bass_row(&a, BL_HEIGHT, 0);
    seq_bass_row(&a, BL_STAYS, 2);
    CHECK(seq_bass_edited(&a) == 1 && seq_bass_style(&a) == POP_ROOT, "the rows edit the style, and keep its name");
    unsigned long u = (unsigned long)a.undo_pushes;
    seq_bass_row(&a, BL_MOVES, 99);
    seq_bass_row(&a, 99, 0);
    CHECK((unsigned long)a.undo_pushes == u, "bad rows and words do nothing");
}

/* A kept bar plays as you left it, and no pick is made up for it */
static void test_kept(void) {
    fresh(&a);
    seq_tweak_keep(&a, LANE_BASS, G_);
    char was[64];
    strcpy(was, line(&a, G_));
    seq_bass_tone_set(&a, G_, TONE_3);
    CHECK(!strcmp(line(&a, G_), was), "a pick leaves a kept bar's notes: %s", line(&a, G_));
    seq_set_key(&a, 0);
    seq_rbar_open(&a, LANE_BASS, G_);   /* any render */
    seq_rbar_done(&a, LANE_BASS);
    seq_set_scale(&a, 1);
    seq_set_scale(&a, 0);
    CHECK(!strcmp(line(&a, G_), was), "and after a render too: %s", line(&a, G_));
    CHECK(!seq_bass_pick(&a, G_, 0), "no hit reads as picked");
    seq_tweak_keep(&a, LANE_BASS, G_);
    CHECK(!strcmp(line(&a, G_), "B . . . B . . . B . . . B . . ."), "no longer kept, it follows the page: %s", line(&a, G_));

    fresh(&a);
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_tweak_keep(&a, LANE_BASS, G_);
    strcpy(was, line(&a, G_));
    CHECK(!strcmp(was, "B . . . B . . . B . . . B . . ."), "kept with its pick");
    seq_bass_feel_style(&a, 3);
    CHECK(!strcmp(line(&a, G_), was), "a style leaves it: %s", line(&a, G_));
    seq_bass_tones_clear(&a);
    CHECK(!strcmp(line(&a, G_), was), "All roots leaves it: %s", line(&a, G_));
    seq_undo(&a);
    seq_undo(&a);
    CHECK(tone_of(&a, G_) == TONE_3, "undo brings the tone back");

    /* what it walks into stays as kept too */
    fresh(&a);
    seq_bass_row(&a, BL_STEPS, 2);   /* half below */
    seq_tweak_keep(&a, LANE_BASS, C_);
    strcpy(was, line(&a, C_));
    CHECK(!strcmp(was, "C . . . C . . . C . . . F# . . ."), "kept walking into G: %s", was);
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_set_scale(&a, 1);
    seq_set_scale(&a, 0);
    CHECK(!strcmp(line(&a, C_), was), "the next chord's new note doesn't move it: %s", line(&a, C_));

    /* a save keeps what it holds */
    fresh(&a);
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_tweak_keep(&a, LANE_BASS, G_);
    seq_bass_tones_clear(&a);
    seq_state_save(&a, state, sizeof state);
    CHECK(strstr(state, "2.bf=") != NULL, "a kept bar's tones are saved");
    seq_init(&b, rec, NULL);
    seq_set_rate(&b, 44100);
    CHECK(seq_state_load(&b, state) == 1 && first(&b, G_) % 12 == 11 && tone_of(&b, G_) == 0, "and reload: %s", line(&b, G_));
    fresh(&a);
    seq_tweak_keep(&a, LANE_BASS, G_);
    seq_state_save(&a, state, sizeof state);
    CHECK(!strstr(state, "2.bf="), "a kept bar on roots saves as before");
}

/* Chord edits: each pick stays with its chord */
static void test_chord_edits(void) {
    fresh(&a);
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_bass_tone_set(&a, AM, TONE_5);
    seq_feel_add(&a, G_, 1, 0, COL_TRIAD);   /* a new chord in G's second half */
    CHECK(tone_of(&a, G_) == TONE_3 && tone_of(&a, G_ + 1) == 0 && tone_of(&a, G_ + 2) == TONE_5, "an added chord starts on its root: %d %d %d", tone_of(&a, G_), tone_of(&a, G_ + 1), tone_of(&a, G_ + 2));
    seq_undo(&a);
    CHECK(tone_of(&a, G_) == TONE_3 && tone_of(&a, AM) == TONE_5, "undo");
    seq_feel_resize(&a, G_, -1);
    CHECK(tone_of(&a, G_) == TONE_3 && tone_of(&a, AM) == TONE_5, "resized, each keeps its own");
    seq_undo(&a);
    seq_feel_remove(&a, C_);
    CHECK(tone_of(&a, 0) == TONE_3 && tone_of(&a, 1) == TONE_5, "a removed chord leaves the others': %d %d", tone_of(&a, 0), tone_of(&a, 1));

    /* two equal chords with different tones stay two chords */
    fresh(&a);
    seq_feel_pick(&a, AM, 4, 0, COL_TRIAD);   /* C G G F */
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_feel_chord_t c[64];
    CHECK(seq_feel_chords(&a, c, 64) == 4 && first(&a, G_) % 12 == 11 && first(&a, AM) % 12 == 7, "G on B, G on G");
}

/* Loop length, saving, parts */
static void test_keeping(void) {
    fresh(&a);
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_set_bars(&a, 2);
    seq_set_bars(&a, 8);
    CHECK(tone_of(&a, G_) == TONE_3 && first(&a, G_) % 12 == 11, "a shrink and a grow keep it");
    CHECK(tone_of(&a, G_ + 4) == TONE_3 && first(&a, G_ + 4) % 12 == 11, "the bars it grew into repeat the pick");
    seq_bass_tone_set(&a, G_, TONE_5);
    seq_undo(&a);
    CHECK(tone_of(&a, G_) == TONE_3, "a pick's undo after a grow");

    /* a song with no pick saves as before; one with a pick reloads */
    fresh(&a);
    seq_state_save(&a, state, sizeof state);
    CHECK(!strstr(state, ".bt="), "no pick, no field");
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_bass_tone_set(&a, F_, TONE_7);
    seq_state_save(&a, state, sizeof state);
    CHECK(strstr(state, ".bt=") != NULL, "a pick is saved");
    seq_init(&b, rec, NULL);
    seq_set_rate(&b, 44100);
    CHECK(seq_state_load(&b, state) == 1, "and loads");
    CHECK(tone_of(&b, G_) == TONE_3 && tone_of(&b, F_) == TONE_7 && first(&b, G_) % 12 == 11 && first(&b, F_) % 12 == 4, "and plays: %s", line(&b, F_));

    /* a bad field is skipped, never refused */
    char *f = strstr(state, ".bt=");
    if (f) {
        f[4] = 'z';
        CHECK(seq_state_load(&b, state) == 1, "a bad entry is skipped");
    }

    /* each part keeps its own */
    fresh(&a);
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_select_part(&a, 1);
    seq_bass_tone_set(&a, G_, TONE_5);
    seq_tweak_note(&a, C_, 4, TONE_5, 0);
    seq_select_part(&a, 0);
    CHECK(a.cur_part == 0 && tone_of(&a, G_) == TONE_3 && !seq_bass_pick(&a, C_, 4), "a part's picks stay on it");
    seq_state_save(&a, state, sizeof state);
    seq_init(&b, rec, NULL);
    seq_set_rate(&b, 44100);
    CHECK(seq_state_load(&b, state) == 1, "two parts load");
    seq_select_part(&b, 0);
    CHECK(tone_of(&b, G_) == TONE_3 && !seq_bass_pick(&b, C_, 4), "the first part's");
    seq_select_part(&b, 1);
    CHECK(b.cur_part == 1 && tone_of(&b, G_) == TONE_5 && seq_bass_pick(&b, C_, 4) == bass_pick_code(TONE_5, 0), "and the second's");

    /* the chord's next note */
    fresh(&a);
    CHECK(step_tone_of(&a, C_) == TONE_3, "from C2, B1 is the nearest of G's root, 3rd and 5th");
    CHECK(tone_note(&a, 99, TONE_R) == 0, "a bad chord reads nothing");
}

/* The Tweak rows on the Bass: each word sets its own knobs, one undo step, and Chords' stay as they are */
static void test_tweak_rows(void) {
    static const int COUNT[TW_COUNT] = {5, 6, 5, 5, 5, 3, 4, 4, 6};
    for (int w = 0; w < TW_COUNT; w++) {
        CHECK(seq_tweak_words(LANE_BASS, w) == COUNT[w], "Bass row %d has %d words", w, COUNT[w]);
        fresh(&a);
        for (int v = 0; v < COUNT[w]; v++) {
            int before = seq_tweak_word(&a, LANE_BASS, w), chords = seq_tweak_word(&a, LANE_CHORDS, w);
            seq_tweak_set(&a, LANE_BASS, w, v);
            CHECK(seq_tweak_word(&a, LANE_BASS, w) == v, "Bass row %d word %d reads back", w, v);
            if (w != TW_FOLLOW) CHECK(seq_tweak_word(&a, LANE_CHORDS, w) == chords, "and leaves Chords' row");
            if (v != before) {
                seq_undo(&a);
                CHECK(seq_tweak_word(&a, LANE_BASS, w) == before, "Bass row %d word %d: undo", w, v);
                seq_tweak_set(&a, LANE_BASS, w, v);
            }
        }
    }
    CHECK(seq_tweak_words(LANE_CHORDS, TW_ENDING) == 5 && seq_tweak_words(LANE_MEL_UP, TW_BUSY) == 0, "Chords keep five, a lane with no bars has none");
    static const int FILL[6] = {0, 0, 1, 2, 3, 4}, BREATH[6] = {0, 2, 0, 0, 0, 0};
    fresh(&a);
    for (int v = 0; v < 6; v++) {
        seq_tweak_set(&a, LANE_BASS, TW_ENDING, v);
        CHECK(seq_rknob(&a, LANE_BASS, RK_FILL) == FILL[v] && seq_rknob(&a, LANE_BASS, RK_BREATH) == BREATH[v], "Ending %d is FILL %d, BREATH %d", v, FILL[v], BREATH[v]);
    }
    fresh(&a);
    seq_tweak_set(&a, LANE_BASS, TW_LEAN, 2);
    CHECK(seq_lane_shape(&a, LANE_BASS, BSHAPE_PUSH) == 50, "Lean 2 is the Bass's PUSH 50");
    seq_tweak_set(&a, LANE_BASS, TW_BUSY, 4);
    CHECK(seq_complexity(&a, LANE_BASS) == 88 && seq_complexity(&a, LANE_CHORDS) != 88, "Busy is the Bass's COMPLEXITY");
    unsigned h = seq_tweak_hash(&a, LANE_BASS);
    seq_tweak_note(&a, 0, 0, TONE_3, 0);
    CHECK(seq_tweak_hash(&a, LANE_BASS) != h, "the hash moves with a pick");
    h = seq_tweak_hash(&a, LANE_BASS);
    seq_bass_row(&a, BL_MOVES, 3);
    CHECK(seq_tweak_hash(&a, LANE_BASS) != h, "and with a Line row");
}

/* A hit's pick */
static void test_hit_picks(void) {
    fresh(&a);
    seq_tweak_note(&a, G_, 4, TONE_3, 0);
    CHECK(!strcmp(line(&a, G_), "G . . . B . . . G . . . G . . ."), "the 3rd on one hit: %s", line(&a, G_));
    CHECK(seq_bass_pick(&a, G_, 4) == bass_pick_code(TONE_3, 0) && !seq_bass_pick(&a, G_, 0), "the pick reads back");
    seq_undo(&a);
    CHECK(!strcmp(line(&a, G_), "G . . . G . . . G . . . G . . ."), "one undo");

    /* octaves */
    fresh(&a);
    int root = first(&a, G_);
    seq_tweak_note(&a, G_, 0, TONE_R, 1);
    CHECK(first(&a, G_) == root + 12, "an octave up");
    seq_tweak_note(&a, G_, 0, TONE_R, -1);
    CHECK(first(&a, G_) == root - 12, "an octave down");

    /* it follows the chord */
    fresh(&a);
    seq_tweak_note(&a, G_, 4, TONE_3, 0);
    seq_set_key(&a, 2);
    unsigned char n[16];
    char r[16];
    seq_bass_line(&a, G_, n, r);
    CHECK(n[4] % 12 == 1 && r[4] == 'f', "in D it is C#, role f");
    seq_set_key(&a, 0);
    seq_feel_pick(&a, G_, 1, 0, COL_TRIAD);
    seq_bass_line(&a, G_, n, r);
    CHECK(n[4] % 12 == 5, "under Dm it is F");

    /* half steps aim at the next chord's own note */
    fresh(&a);
    seq_tweak_note(&a, C_, 12, BP_BELOW, 0);
    seq_bass_line(&a, C_, n, r);
    CHECK(n[12] % 12 == 6, "half below G is F#");
    seq_bass_tone_set(&a, G_, TONE_3);
    seq_bass_line(&a, C_, n, r);
    CHECK(n[12] % 12 == 10, "half below the B now under G is A#");
    seq_tweak_note(&a, C_, 12, BP_ABOVE, 0);
    seq_bass_line(&a, C_, n, r);
    CHECK(n[12] % 12 == 0, "half above B is C");

    /* as style, and a rest takes no pick */
    seq_tweak_note(&a, C_, 12, 0, 0);
    CHECK(!seq_bass_pick(&a, C_, 12), "tone 0: back to the style");
    unsigned long u = (unsigned long)a.undo_pushes;
    seq_tweak_note(&a, C_, 1, TONE_3, 0);
    seq_tweak_note(&a, C_, 12, 99, 0);
    seq_tweak_note(&a, C_, 12, TONE_3, 5);
    seq_tweak_note(&a, 99, 0, TONE_3, 0);
    CHECK((unsigned long)a.undo_pushes == u, "a rest, a bad tone, octave and bar do nothing");

    /* it beats Lands and Try another, as a fixed note does */
    fresh(&a);
    seq_tweak_note(&a, F_, 12, TONE_3, 0);
    seq_tweak_set(&a, LANE_BASS, TW_LANDS, 3);
    seq_bass_line(&a, F_, n, r);
    CHECK(n[12] % 12 == 9, "Lands passes over it: %s", line(&a, F_));
    CHECK(n[8] % 12 == 5, "the hit before lands");
    seq_bass_row(&a, BL_MOVES, 3);
    seq_tweak_note(&a, G_, 4, TONE_7, 0);
    for (int i = 0; i < 3; i++) seq_tweak_try(&a, LANE_BASS, G_);
    seq_bass_line(&a, G_, n, r);
    CHECK(n[4] % 12 == 5, "Try another leaves it: %s", line(&a, G_));

    /* copy with notes, not without; Back to style clears it */
    fresh(&a);
    seq_tweak_note(&a, C_, 4, TONE_5, 0);
    seq_tweak_copy(&a, LANE_BASS, C_, 1u << G_, 0);
    CHECK(!seq_bass_pick(&a, G_, 4), "Rhythm only leaves the notes");
    seq_tweak_copy(&a, LANE_BASS, C_, 1u << G_, 1);
    seq_bass_line(&a, G_, n, r);
    CHECK(seq_bass_pick(&a, G_, 4) && n[4] % 12 == 2, "Rhythm + notes: the 5th of G is D");
    seq_tweak_reset(&a, LANE_BASS, C_);
    CHECK(!seq_bass_pick(&a, C_, 4), "Back to style clears it");
    seq_undo(&a);
    CHECK(seq_bass_pick(&a, C_, 4), "undo brings it back");

    /* as style: what the hit plays with no pick */
    seq_tweak_as_style(&a, C_, n);
    CHECK(n[4] % 12 == 0, "as style reads the root under a pick");

    /* a save carries it */
    seq_state_save(&a, state, sizeof state);
    seq_init(&b, rec, NULL);
    seq_set_rate(&b, 44100);
    CHECK(seq_state_load(&b, state) == 1 && seq_bass_pick(&b, C_, 4) == bass_pick_code(TONE_5, 0), "and reloads");
    seq_bass_line(&b, C_, n, r);
    CHECK(n[4] % 12 == 7, "and plays");
    CHECK(strstr(state, "2.ap=") != NULL, "in its own field");
    {   /* a value above MIDI in a fixed note's field stays a bad pair, as it always was */
        char *f = strstr(state, "2.ap=");
        f[3] = 'b';
        CHECK(seq_state_load(&b, state) == 1 && !seq_bass_pick(&b, C_, 4), "2.ab never takes a pick");
    }

    /* Early lands a pick */
    fresh(&a);
    paint_all(&a, "x...x...x.......");
    seq_tweak_note(&a, G_, 0, TONE_5, 0);
    seq_tweak_set(&a, LANE_BASS, TW_EARLY, AN_8);
    seq_bass_line(&a, C_, n, r);
    CHECK(n[14] % 12 == 2, "an 8th early, the picked D: %s", line(&a, C_));
}

/* A Force song's fixed notes: absolute, so they can go stale */
static void test_force_notes(void) {
    fresh(&a);
    CHECK(seq_bass_off_key(&a, G_) == -1, "a new song has none");
    seq_rbar_open(&a, LANE_BASS, G_);
    seq_rbar_select(&a, LANE_BASS, 4);
    seq_rbar_note(&a, 3);   /* D#, fixed */
    seq_rbar_done(&a, LANE_BASS);
    CHECK(seq_bass_off_key(&a, G_) == 4, "D# in C major warns at its step");
    CHECK(!seq_bass_pick(&a, G_, 4), "it isn't a pick");
    seq_set_key(&a, 1);   /* C#: D# is in key now */
    CHECK(seq_bass_off_key(&a, G_) == -1, "in key, no warning");
    seq_set_key(&a, 0);
    seq_tweak_follow(&a, G_);
    unsigned char n[16];
    char r[16];
    seq_bass_line(&a, G_, n, r);
    CHECK(seq_bass_off_key(&a, G_) == -1 && n[4] % 12 == 7, "Make it follow the chord: %s", line(&a, G_));
    seq_undo(&a);
    CHECK(seq_bass_off_key(&a, G_) == 4, "one undo");

    /* a pick replaces it */
    seq_tweak_note(&a, G_, 4, TONE_R, 0);
    CHECK(seq_bass_off_key(&a, G_) == -1 && seq_bass_pick(&a, G_, 4), "a pick takes its place");

    /* a fixed note on the Force replaces a pick */
    seq_rbar_open(&a, LANE_BASS, G_);
    seq_rbar_select(&a, LANE_BASS, 4);
    seq_rbar_note(&a, 5);
    seq_rbar_done(&a, LANE_BASS);
    CHECK(!seq_bass_pick(&a, G_, 4), "the Force's fixed note takes its place");
}

int main(void) {
    test_tone();
    test_targets();
    test_clearing();
    test_rows();
    test_kept();
    test_chord_edits();
    test_keeping();
    test_tweak_rows();
    test_hit_picks();
    test_force_notes();
    printf(fails ? "test_bass_feel: %d FAILED\n" : "test_bass_feel: all passed\n", fails);
    return fails != 0;
}
