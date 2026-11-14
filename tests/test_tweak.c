/* Offline test of the Tweak page's engine (seq_tweak_*, seq_bar_fill) and the Warm level's sus2 and 6th. Run by tests/run_tests.sh under
 * ASan/UBSan. Exit 1 on failure. */
#include <stdio.h>
#include <string.h>
#include "seq_core.h"
#include "feel.h"
#include "rhythm.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c) != 0; printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static void rec(void *ctx, int status, int note, int vel, int frame) { (void)ctx, (void)status, (void)note, (void)vel, (void)frame; }
static void fresh(seq_t *s) { seq_init(s, rec, NULL); seq_init_preset(s); seq_set_rate(s, 44100); }

static void codes_str(const unsigned char c[16], char out[17]) {
    static const char T[] = ".xAg-";
    for (int i = 0; i < 16; i++) out[i] = T[c[i]];
    out[16] = 0;
}
static const char *plays(const seq_t *s, int bar) {
    static char b[17];
    unsigned char c[16];
    seq_bar_plays(s, LANE_CHORDS, bar, c);
    codes_str(c, b);
    return b;
}
static const char *fill(const seq_t *s, int bar) {
    static char b[17];
    unsigned char c[16];
    seq_bar_fill(s, LANE_CHORDS, bar, c);
    codes_str(c, b);
    return b;
}
static const char *drawn(const seq_t *s, int bar) {
    static char b[17];
    unsigned char c[16];
    seq_bar_codes(s, LANE_CHORDS, bar, c);
    codes_str(c, b);
    return b;
}

static seq_t a;   /* a seq_t is large: never on the stack */

/* Every word reads back, a word set again adds no undo step, and one undo puts the word before it back */
static void test_words(void) {
    static const int COUNT[TW_COUNT] = {5, 6, 5, 5, 5, 3, 4, 4, 5};
    for (int w = 0; w < TW_COUNT; w++) {
        CHECK(seq_tweak_words(LANE_CHORDS, w) == COUNT[w], "row %d has %d words", w, COUNT[w]);
        fresh(&a);
        int was = seq_tweak_word(&a, LANE_CHORDS, w);
        CHECK(was >= 0, "row %d lights a word on a new song", w);
        for (int v = 0; v < COUNT[w]; v++) {
            int before = seq_tweak_word(&a, LANE_CHORDS, w);
            seq_tweak_set(&a, LANE_CHORDS, w, v);
            CHECK(seq_tweak_word(&a, LANE_CHORDS, w) == v, "row %d word %d reads back", w, v);
            if (v != before) {
                seq_undo(&a);
                CHECK(seq_tweak_word(&a, LANE_CHORDS, w) == before, "row %d word %d: undo puts word %d back", w, v, before);
                seq_tweak_set(&a, LANE_CHORDS, w, v);
            }
        }
    }
    fresh(&a);
    seq_tweak_set(&a, LANE_CHORDS, TW_LOOSE, 2);
    seq_tweak_set(&a, LANE_CHORDS, TW_LOOSE, 2);   /* no second step */
    seq_undo(&a);
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_LOOSE) == 0, "the same word twice is one undo step");
    seq_tweak_set(&a, LANE_CHORDS, TW_LOOSE, 99);
    seq_tweak_set(&a, LANE_CHORDS, 99, 0);
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_LOOSE) == 0 && seq_tweak_word(&a, LANE_CHORDS, 99) == -1, "bad words and rows are ignored");
}

/* The words set the engine's own knobs, so the Force plays what the page says */
static void test_word_values(void) {
    static const int BUSY[5] = {12, 32, 50, 66, 88};
    fresh(&a);
    for (int v = 0; v < 5; v++) {
        seq_tweak_set(&a, LANE_CHORDS, TW_BUSY, v);
        CHECK(seq_complexity(&a, LANE_CHORDS) == BUSY[v], "Busy %d is COMPLEXITY %d", v, BUSY[v]);
    }
    for (int v = 0; v < 6; v++) {
        seq_tweak_set(&a, LANE_CHORDS, TW_OFFBEAT, v);
        CHECK(seq_rknob(&a, LANE_CHORDS, RK_SYNC) == v - 2, "Off beat %d is SYNC %d", v, v - 2);
    }
    seq_tweak_set(&a, LANE_CHORDS, TW_SOFT, 3);
    seq_tweak_set(&a, LANE_CHORDS, TW_LOOSE, 4);
    seq_tweak_set(&a, LANE_CHORDS, TW_LANDS, 3);
    seq_tweak_set(&a, LANE_CHORDS, TW_LEAN, 2);
    seq_tweak_set(&a, LANE_CHORDS, TW_EARLY, 2);
    seq_tweak_set(&a, LANE_CHORDS, TW_FOLLOW, 1);
    CHECK(seq_rknob(&a, LANE_CHORDS, RK_GHOSTS) == 3 && seq_rknob(&a, LANE_CHORDS, RK_HUM) == 4 && seq_rknob(&a, LANE_CHORDS, RK_CAD) == 3, "Soft notes, Loose, Lands");
    CHECK(seq_chord_shape(&a, SHAPE_PUSH) == 50, "Lean 2 is PUSH 50");
    CHECK(seq_anticipate(&a, LANE_CHORDS) == AN_8 && seq_lockwith(&a, LANE_CHORDS) == LW_WITH, "Early and Follow");

    /* a value between the words plays on and lights none, until a word is tapped */
    fresh(&a);
    seq_set_complexity(&a, LANE_CHORDS, 40);
    seq_set_chord_shape(&a, SHAPE_PUSH, 10);
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_BUSY) == -1 && seq_tweak_word(&a, LANE_CHORDS, TW_LEAN) == -1, "a knob between the words lights none");
    CHECK(seq_complexity(&a, LANE_CHORDS) == 40, "and keeps its value");
    seq_tweak_set(&a, LANE_CHORDS, TW_BUSY, 2);
    seq_undo(&a);
    CHECK(seq_complexity(&a, LANE_CHORDS) == 40, "undo of a word puts the value between the words back");
}

/* Ending is FILL and BREATH as one word, one undo step; what the Force saved keeps playing with no word lit */
static void test_ending(void) {
    static const int FILL[5] = {0, 0, 1, 3, 4}, BREATH[5] = {0, 2, 0, 0, 0};
    fresh(&a);
    for (int v = 0; v < 5; v++) {
        seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, v);
        CHECK(seq_rknob(&a, LANE_CHORDS, RK_FILL) == FILL[v] && seq_rknob(&a, LANE_CHORDS, RK_BREATH) == BREATH[v], "Ending %d is FILL %d, BREATH %d", v, FILL[v], BREATH[v]);
    }
    fresh(&a);
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 1);
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 3);
    seq_undo(&a);
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_ENDING) == 1 && seq_rknob(&a, LANE_CHORDS, RK_FILL) == 0 && seq_rknob(&a, LANE_CHORDS, RK_BREATH) == 2, "one undo step puts both back");

    fresh(&a);
    seq_set_rknob(&a, LANE_CHORDS, RK_FILL, 2);
    seq_set_rknob(&a, LANE_CHORDS, RK_BREATH, 1);   /* Fill Busier with Breath Short, from a Force song */
    char was[17];
    strcpy(was, fill(&a, 3));
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_ENDING) == -1, "Fill Busier with Breath Short lights no word");
    CHECK(strcmp(was, plays(&a, 3)) != 0, "and still plays");
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 2);
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_ENDING) == 2, "a tap replaces it");
    seq_undo(&a);
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_ENDING) == -1 && !strcmp(fill(&a, 3), was), "undo brings it back");
}

/* seq_bar_fill: what a phrase's last bar plays with its ending, seq_bar_plays leaving FILL out */
static void test_fill_bar(void) {
    fresh(&a);
    seq_rhythm_pick(&a, LANE_CHORDS, 0);   /* Sustain: x--------------- */
    CHECK(!strcmp(plays(&a, 3), "x---------------"), "Sustain");
    CHECK(seq_bar_fill(&a, LANE_CHORDS, 3, (unsigned char[16]){0}) == 0 && !strcmp(fill(&a, 3), "x---------------"), "no ending: the bar as it plays, 0");
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 1);
    CHECK(!strcmp(fill(&a, 3), "x-----------...."), "Breath rests the last beat: %s", fill(&a, 3));
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 2);
    CHECK(!strcmp(fill(&a, 3), "x-------------xg"), "Pickup: %s", fill(&a, 3));
    CHECK(seq_bar_fill(&a, LANE_CHORDS, 3, (unsigned char[16]){0}) == 1, "a fill bar: 1");
    CHECK(!strcmp(plays(&a, 3), "x---------------"), "seq_bar_plays still leaves FILL out");
    CHECK(seq_bar_fill(&a, LANE_CHORDS, 2, (unsigned char[16]){0}) == 0 && !strcmp(fill(&a, 2), "x---------------"), "bar 3 is no phrase's end");
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 3);
    CHECK(!strcmp(fill(&a, 3), "x-----------xxxA"), "Roll: %s", fill(&a, 3));
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 4);
    CHECK(!strcmp(fill(&a, 3), "x-------........"), "Break: %s", fill(&a, 3));
    seq_tweak_keep(&a, LANE_CHORDS, 3);
    CHECK(seq_bar_fill(&a, LANE_CHORDS, 3, (unsigned char[16]){0}) == 0 && !strcmp(fill(&a, 3), "x---------------"), "a kept bar skips the ending: %s", fill(&a, 3));
    CHECK(seq_bar_fill(&a, LANE_CHORDS, 99, (unsigned char[16]){0}) == 0, "a bad bar reads 0");

    CHECK(seq_phrase_end(&a, 3) && !seq_phrase_end(&a, 2) && !seq_phrase_end(&a, 4) && !seq_phrase_end(&a, -1), "bar 4 ends a phrase; a bar past the loop does not");
    fresh(&a);
    seq_set_bars(&a, 1);
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 2);
    CHECK(seq_bar_fill(&a, LANE_CHORDS, 0, (unsigned char[16]){0}) == 1, "a 1-bar loop: its bar is the phrase's last");
    fresh(&a);
    seq_set_bars(&a, 2);
    seq_tweak_set(&a, LANE_CHORDS, TW_ENDING, 2);
    CHECK(seq_bar_fill(&a, LANE_CHORDS, 0, (unsigned char[16]){0}) == 0 && seq_bar_fill(&a, LANE_CHORDS, 1, (unsigned char[16]){0}) == 1, "a 2-bar loop: bar 2 only");
    CHECK(!seq_phrase_end(&a, 0) && seq_phrase_end(&a, 1), "a 2-bar loop ends on bar 2");
}

/* A bar's edit is one undo step and leaves nothing open */
static void test_bar_verbs(void) {
    fresh(&a);
    seq_rhythm_pick(&a, LANE_CHORDS, 0);
    int view = seq_cview(&a);

    CHECK(seq_tweak_paint(&a, LANE_CHORDS, 1, 8, RC_HIT) == 1, "paint changes the step");
    CHECK(!strcmp(drawn(&a, 1), "x-------x-------"), "paint: %s", drawn(&a, 1));
    CHECK(seq_rbar_bar(&a, LANE_CHORDS) == -1 && seq_cview(&a) == view, "nothing stays open");
    CHECK(seq_tweak_paint(&a, LANE_CHORDS, 1, 8, RC_HIT) == 0, "the same paint: nothing");
    seq_undo(&a);
    CHECK(!strcmp(drawn(&a, 1), "x---------------"), "one undo step");
    CHECK(seq_rbar_bar(&a, LANE_CHORDS) == -1, "undo leaves nothing open");

    seq_tweak_paint(&a, LANE_CHORDS, 1, 0, RC_REST);
    CHECK(!strcmp(drawn(&a, 1), "................"), "a rest cuts the holds after it: %s", drawn(&a, 1));
    CHECK(seq_tweak_paint(&a, LANE_CHORDS, 1, 4, RC_HOLD) == 0, "a hold needs a hit before it");
    seq_undo(&a);

    seq_tweak_paint(&a, LANE_CHORDS, 2, 8, RC_ACC);
    seq_tweak_hit_oct(&a, 2, 8, 1);
    CHECK(seq_hit_oct(&a, 2, 8) == 1, "a hit's octave");
    seq_undo(&a);
    CHECK(seq_hit_oct(&a, 2, 8) == 0 && !strcmp(drawn(&a, 2), "x-------A-------"), "octave undone, the paint stays");
    seq_undo(&a);
    CHECK(!strcmp(drawn(&a, 2), "x---------------"), "then the paint");

    int was = seq_bar_locked(&a, LANE_CHORDS, 2);
    seq_tweak_keep(&a, LANE_CHORDS, 2);
    CHECK(seq_bar_locked(&a, LANE_CHORDS, 2) == !was, "Keep");
    seq_tweak_keep(&a, LANE_CHORDS, 2);
    CHECK(seq_bar_locked(&a, LANE_CHORDS, 2) == was, "Keep again lets go");

    seq_tweak_paint(&a, LANE_CHORDS, 2, 8, RC_HIT);
    seq_tweak_keep(&a, LANE_CHORDS, 2);
    seq_tweak_reset(&a, LANE_CHORDS, 2);
    CHECK(!seq_bar_locked(&a, LANE_CHORDS, 2) && !strcmp(drawn(&a, 2), "x---------------"), "Back to rhythm: the rhythm again, unlocked: %s", drawn(&a, 2));

    CHECK(seq_tweak_try(&a, LANE_CHORDS, 3) == 0 && !strcmp(drawn(&a, 3), "x---------------"), "Sustain has no other version: 0, %s", drawn(&a, 3));
    seq_rhythm_pick(&a, LANE_CHORDS, 20);   /* Funk */
    char before[17];
    strcpy(before, drawn(&a, 3));
    CHECK(seq_tweak_try(&a, LANE_CHORDS, 3) == 1 && strcmp(drawn(&a, 3), before), "Try another changes the bar");
    seq_undo(&a);
    CHECK(!strcmp(drawn(&a, 3), before), "and undoes in one step");
    seq_rhythm_pick(&a, LANE_CHORDS, 0);

    seq_tweak_paint(&a, LANE_CHORDS, 0, 4, RC_HIT);
    seq_tweak_copy(&a, LANE_CHORDS, 0, 1u << 1 | 1u << 2, 0);
    CHECK(!strcmp(drawn(&a, 1), "x---x-----------") && !strcmp(drawn(&a, 2), "x---x-----------"), "Copy to two bars: %s %s", drawn(&a, 1), "");
    CHECK(!strcmp(drawn(&a, 3), "x---------------"), "and no others");
    CHECK(seq_rbar_bar(&a, LANE_CHORDS) == -1, "Copy leaves nothing open");
    seq_undo(&a);
    CHECK(!strcmp(drawn(&a, 1), "x---------------"), "one undo step");
    seq_tweak_keep(&a, LANE_CHORDS, 1);
    seq_tweak_copy(&a, LANE_CHORDS, 0, 1u << 1, 0);
    CHECK(!strcmp(drawn(&a, 1), "x---------------"), "a kept bar takes no copy");
    CHECK(seq_tweak_paint(&a, LANE_CHORDS, 99, 0, RC_HIT) == 0 && seq_tweak_paint(&a, LANE_CHORDS, 0, 99, RC_HIT) == 0, "bad places are ignored");
}

/* Tweak's chord picks: only a hit that does not start a chord makes a change point; sus2 and 6 there too */
static void test_bar_chords(void) {
    fresh(&a);
    seq_rhythm_pick(&a, LANE_CHORDS, 1);   /* Ballad: x-------x------- */
    CHECK(!seq_tweak_can_point(&a, 0, 0), "the hit that starts a chord is Feel's");
    CHECK(seq_tweak_can_point(&a, 0, 8), "a later hit can change the chord");
    int slot = seq_slot_at(&a, 0, 8);
    CHECK(seq_tweak_chord(&a, 0, 8, 4, 0, COL_SUS2), "a different chord from here");
    CHECK(seq_cp_count(&a, 0) == 1 && seq_slot_at(&a, 0, 8) != slot, "a change point");
    seq_cp_t cp;
    seq_cp_get(&a, 0, 0, &cp);
    CHECK(cp.step == 8 && cp.deg == 4 && cp.src == 0, "at that hit, that chord");
    CHECK(seq_slot_colour(&a, seq_slot_at(&a, 0, 8)) == COL_SUS2, "its colour");
    CHECK(seq_rbar_bar(&a, LANE_CHORDS) == -1, "nothing stays open");
    seq_undo(&a);
    CHECK(seq_cp_count(&a, 0) == 0, "one undo step");

    seq_tweak_chord(&a, 0, 8, 4, 0, COL_TRIAD);
    seq_tweak_chord(&a, 0, 8, -1, 0, 0);
    CHECK(seq_cp_count(&a, 0) == 0, "back to the chord as Feel has it");
    CHECK(seq_tweak_chord(&a, 0, 8, 4, 0, COL_COUNT) == 0, "a bad colour: nothing");
    CHECK(seq_tweak_point_ok(0, COL_6TH) && seq_tweak_point_ok(THEORY_NBORROW, COL_TRIAD), "a point holds the scale's and borrowed chords");
    CHECK(!seq_tweak_point_ok(THEORY_NBORROW + 1, COL_TRIAD) && !seq_tweak_point_ok(0, COL_COUNT), "not a chromatic chord or a bad colour");

    fresh(&a);
    seq_rhythm_pick(&a, LANE_CHORDS, 3);   /* Pulse: x---x---x---x--- */
    seq_tweak_chord(&a, 0, 4, 4, 0, COL_TRIAD);
    int after = seq_slot_at(&a, 0, 8);
    CHECK(seq_tweak_can_once(&a, 0, 4) == 1 && seq_tweak_can_once(&a, 0, 12) == RHO_NOT_A_CHANGE && seq_tweak_can_once(&a, 99, 0) == RHO_NOT_A_CHANGE, "what Only this hit would answer");
    CHECK(seq_tweak_once(&a, 0, 4) == 1, "Only this hit");
    CHECK(seq_slot_at(&a, 0, 8) != after, "the next hit goes back to the chord before");
    CHECK(seq_cp_count(&a, 0) == 2, "with a point of its own");
    seq_undo(&a);
    CHECK(seq_cp_count(&a, 0) == 1, "one undo step");
    CHECK(seq_tweak_once(&a, 0, 12) <= 0, "a hit that starts no change: nothing");
    CHECK(seq_rbar_bar(&a, LANE_CHORDS) == -1, "nothing stays open");
}

/* Hear the ending: a start from a bar of the part; a song's held row too */
static void start_at(seq_t *s, int bar) {
    seq_block(s, 0, 120, 1, 128);   /* the start: primes */
    seq_set_part_bar(s, bar);
    for (double ppq = 0.01; ppq < 0.5; ppq += 0.01) seq_block(s, ppq, 120, 1, 128);
}
static void stop(seq_t *s) { seq_block(s, -1, 120, 0, 128); }
static void test_start_at_bar(void) {
    fresh(&a);
    start_at(&a, 2);
    CHECK(seq_cur_bar(&a) == 2, "plays from bar 3: %d", seq_cur_bar(&a));
    stop(&a);
    start_at(&a, 0);
    CHECK(seq_cur_bar(&a) == 0, "bar 0: the top");
    stop(&a);
    start_at(&a, 99);
    CHECK(seq_cur_bar(&a) == 0, "a bar past the loop: the top");
    stop(&a);

    fresh(&a);
    seq_arr_set_form(&a, 0);
    seq_arr_edit(&a, 1);
    int part = seq_part(&a);
    start_at(&a, 1);
    CHECK(seq_part(&a) == part && seq_cur_bar(&a) == 1, "a song's held row: its part, bar 2: part %d, bar %d", seq_part(&a), seq_cur_bar(&a));
    stop(&a);
}

/* The page reads again when anything it shows moved, and not while the song just plays */
static void test_hash(void) {
    fresh(&a);
    unsigned h = seq_tweak_hash(&a, LANE_CHORDS);
    seq_tweak_set(&a, LANE_CHORDS, TW_LOOSE, 2);
    CHECK(seq_tweak_hash(&a, LANE_CHORDS) != h, "a word moves it");
    h = seq_tweak_hash(&a, LANE_CHORDS);
    seq_tweak_paint(&a, LANE_CHORDS, 0, 4, RC_HIT);
    CHECK(seq_tweak_hash(&a, LANE_CHORDS) != h, "a paint moves it");
    h = seq_tweak_hash(&a, LANE_CHORDS);
    seq_tweak_keep(&a, LANE_CHORDS, 0);
    CHECK(seq_tweak_hash(&a, LANE_CHORDS) != h, "Keep moves it");
    h = seq_tweak_hash(&a, LANE_CHORDS);
    seq_tweak_hit_oct(&a, 0, 0, 1);
    CHECK(seq_tweak_hash(&a, LANE_CHORDS) != h, "an octave moves it");
    h = seq_tweak_hash(&a, LANE_CHORDS);
    seq_tweak_set(&a, LANE_CHORDS, TW_LANDS, 3);
    CHECK(seq_tweak_hash(&a, LANE_CHORDS) != h, "Lands moves it");
    h = seq_tweak_hash(&a, LANE_CHORDS);
    for (double ppq = 0; ppq < 8; ppq += 0.05) seq_block(&a, ppq, 120, 1, 128);
    seq_block(&a, -1, 120, 0, 128);
    CHECK(seq_tweak_hash(&a, LANE_CHORDS) == h, "playing does not");
}

/* The Warm level: sus2 and 6 for every chord that can take them */
static void test_warm(void) {
    int roomy = 0;
    for (int scale = 0; scale < SCALE_COUNT; scale++)
        for (int key = 0; key < 12; key++)
            for (int deg = 0; deg < 7; deg++) {
                feel_chord_t base = {deg, 0, COL_TRIAD}, prev = {(deg + 6) % 7, 0, COL_TRIAD}, next = {(deg + 3) % 7, 0, COL_TRIAD};
                feel_option_t o[FEEL_OPTIONS_MAX];
                int n = feel_ladder(key, scale, base, prev, next, o), sus2 = 0, six = 0;
                if (n >= FEEL_OPTIONS_MAX) roomy++;   /* a full ladder may have dropped an option */
                for (int i = 0; i < n; i++)
                    if (o[i].c.deg == deg && o[i].c.src == 0 && o[i].level == FEEL_WARM) {
                        sus2 |= o[i].c.colour == COL_SUS2;
                        six |= o[i].c.colour == COL_6TH;
                    }
                int dim = theory_triad_quality(key, scale, deg) == 2;
                if (sus2 == dim || six == dim) { CHECK(0, "scale %d key %d degree %d: sus2 %d, 6 %d, dim %d", scale, key, deg, sus2, six, dim); return; }
            }
    CHECK(!roomy, "every ladder fits, and every chord but a diminished one offers sus2 and 6 at Warm");

    feel_chord_t c = {0, 0, COL_TRIAD}, x = {4, 0, COL_TRIAD}, y = {3, 0, COL_TRIAD}, sus = {0, 0, COL_SUS2}, six = {0, 0, COL_6TH};
    int kind = -1;
    CHECK(feel_level(0, SCALE_MAJOR, sus, c, x, y, &kind) == FEEL_WARM && kind == FK_RICHER, "Csus2 reads Warm");
    CHECK(feel_level(0, SCALE_MAJOR, six, c, x, y, &kind) == FEEL_WARM && kind == FK_RICHER, "C6 reads Warm");
}

int main(void) {
    test_words();
    test_word_values();
    test_ending();
    test_fill_bar();
    test_bar_verbs();
    test_bar_chords();
    test_start_at_bar();
    test_hash();
    test_warm();
    printf(fails ? "FAILED %d\n" : "PASSED\n", fails);
    return fails != 0;
}
