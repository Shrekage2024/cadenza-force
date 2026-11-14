/* Offline test of the Melody's Feel and Tweak engine: picks that follow the chord, Lands that can follow it, Early, Loose, the page's verbs. Run by
 * tests/run_tests.sh under ASan/UBSan. Exit 1 on failure. */
#include <stdio.h>
#include <string.h>
#include "seq_core.h"
#include "feel.h"
#include "melody2.h"
#include "rhythm.h"
#include "gen.h"
#include "theory.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c) != 0; printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static const char *NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const int MAJOR[7] = {0, 2, 4, 5, 7, 9, 11};
static int in_c(int m) { for (int i = 0; i < 7; i++) if (MAJOR[i] == m % 12) return 1; return 0; }

/* ---- the model alone: C G Am F (or any loop) in C major, a chord a bar ---- */
static mjam_chord_t CH[8];
static mjam_bar_in_t BAR[8];
static unsigned char LAND[8][SEQ_STEPS];
static void loop_of(const int *deg, int n) {
    memset(LAND, 0, sizeof LAND);
    for (int b = 0; b < n; b++) {
        mjam_chord_of(0, SCALE_MAJOR, SCALE_MAJOR, deg[b], &CH[b]);
        BAR[b].key = 0, BAR[b].scale = SCALE_MAJOR;
        for (int i = 0; i < SEQ_STEPS; i++) BAR[b].chord[i] = &CH[b];
    }
}
static mjam_loop_in_t in_of(int bars, mjam_set_t set, int land) {
    mjam_loop_in_t in = {0, SCALE_MAJOR, bars, set, BAR, 0, land ? (const unsigned char (*)[SEQ_STEPS])LAND : NULL};
    return in;
}
static const char *notes(const mjam_note_t *l) {
    static char out[128];
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < SEQ_STEPS; i++)
        if (l[i].note) o += snprintf(out + o, sizeof out - (size_t)o, "%s%s%d", o ? " " : "", NAME[l[i].note % 12], l[i].note / 12 - 1);
    return out;
}

static void test_picks_move_the_chords_notes(void) {
    static const int POP[4] = {0, 4, 5, 3};
    mjam_set_t s = mjam_default();
    mjam_note_t l[SEQ_STEPS];
    loop_of(POP, 4);
    s.cad = 0;
    mjam_loop_in_t plain = in_of(4, s, 0);
    mjam_bar(&plain, 0, &s, l);
    CHECK(!strcmp(notes(l), "C4 E4 G4 C5 C4 E4 G4 C5"), "Arp Up on C as written: %s", notes(l));
    LAND[0][0] = MJ_LAND_CODE(TONE_3, 0);
    mjam_loop_in_t in = in_of(4, s, 1);
    mjam_bar(&in, 0, &s, l);
    CHECK(!strcmp(notes(l), "E4 G4 C5 E5 E4 G4 C5 E5"), "the 3rd picked: the same idea, starting on E: %s", notes(l));
    mjam_bar(&in, 1, &s, l);
    mjam_note_t w[SEQ_STEPS];
    mjam_bar(&plain, 1, &s, w);
    CHECK(!memcmp(l, w, sizeof l), "a bar with no pick plays as before");

    LAND[0][0] = MJ_LAND_CODE(TONE_3, 1);
    mjam_bar(&in, 0, &s, l);
    CHECK(l[0].note == 76, "an octave up: E5 (%d)", l[0].note);
    LAND[0][0] = MJ_LAND_CODE(TONE_2, 0);
    mjam_bar(&in, 0, &s, l);
    CHECK(l[0].note % 12 == 2, "a 2nd lands too: it stays off the chord on purpose (%s)", NAME[l[0].note % 12]);
    LAND[0][0] = 0;

    static const unsigned char TUNE[4] = {TONE_3, TONE_5, TONE_3, TONE_5};
    s.c = MJ_HOOK, s.l = 1;
    for (int b = 0; b < 4; b++) LAND[b][0] = (unsigned char)MJ_LAND_CODE(TUNE[b], 0);
    in = in_of(4, s, 1);
    static const char *WANT[4] = {"E", "D", "C", "C"};
    for (int b = 0; b < 4; b++) {
        mjam_bar(&in, b, &s, l);
        CHECK(!strcmp(NAME[l[0].note % 12], WANT[b]), "Hook Motif, bar %d lands on %s (%s)", b + 1, WANT[b], NAME[l[0].note % 12]);
    }
}

/* Every block but Pedal, every form and answer, every tone: nothing leaves the key, every first note is the pick, every note on beats 2-4 is on the chord */
static void test_sweep(void) {
    static const int POP[4] = {0, 4, 5, 3};
    int bars = 0, out = 0, missed = 0, off = 0;
    loop_of(POP, 4);
    for (int t = TONE_R; t <= TONE_7; t++) {
        for (int b = 0; b < 4; b++) LAND[b][0] = (unsigned char)MJ_LAND_CODE(t, 0);
        for (int c = 0; c < MJ_PEDAL; c++)
            for (int l = 0; l < MJ_LEVELS; l++)
                for (int form = 0; form < MF_COUNT; form++)
                    for (int ans = 0; ans < (form ? MA_COUNT : 1); ans++) {
                        mjam_set_t s = mjam_default();
                        mjam_note_t n[SEQ_STEPS];
                        s.c = (signed char)c, s.l = (signed char)l, s.form = (signed char)form, s.ans = (signed char)ans, s.stray = 2, s.cad = 0;
                        mjam_loop_in_t in = in_of(4, s, 1);
                        for (int b = 0; b < 4; b++) {
                            int first = -1;
                            mjam_chord_t *ch = &CH[b];
                            mjam_bar(&in, b, &s, n);
                            bars++;
                            for (int i = 0; i < SEQ_STEPS; i++) {
                                if (!n[i].note) continue;
                                if (first < 0) first = i;
                                out += !in_c(n[i].note);
                                if (i % 4 == 0 && i != first) {
                                    int pc = n[i].note % 12;
                                    off += pc != ch->root && pc != ch->third && pc != ch->fifth;
                                }
                            }
                            if (first >= 0) missed += n[first].note % 12 != ch->tone[t - 1];
                        }
                    }
    }
    CHECK(bars > 20000, "the sweep ran: %d bars", bars);
    CHECK(!out, "no note left the key (%d)", out);
    CHECK(!missed, "every first note is the pick (%d)", missed);
    CHECK(!off, "with Lands off, every note on a beat after the first is on the chord (%d)", off);
}

static void test_edges(void) {
    static const int POP[4] = {0, 4, 5, 3};
    mjam_note_t a[SEQ_STEPS], b[SEQ_STEPS];
    mjam_set_t s = mjam_default();
    loop_of(POP, 4);
    s.c = MJ_PEDAL, s.l = 2;
    for (int i = 0; i < 4; i++) LAND[i][0] = MJ_LAND_CODE(TONE_3, 1);
    mjam_loop_in_t off = in_of(4, s, 0), on = in_of(4, s, 1);
    mjam_bar(&off, 0, &s, a);
    mjam_bar(&on, 0, &s, b);
    CHECK(!memcmp(a, b, sizeof a), "a Pedal block ignores picks");

    memset(LAND, 0, sizeof LAND);
    s = mjam_default();
    off = in_of(4, s, 0), on = in_of(4, s, 1);
    mjam_bar(&off, 3, &s, a);
    mjam_bar(&on, 3, &s, b);
    CHECK(!memcmp(a, b, sizeof a), "an empty table plays as no table");

    /* a pick on steps past every note, and on a bar that breath cuts to nothing */
    s.c = MJ_LEAD, s.l = 0, s.breath = 3;
    LAND[1][15] = MJ_LAND_CODE(TONE_3, 0);
    LAND[3][0] = MJ_LAND_CODE(TONE_5, 0);
    on = in_of(4, s, 1), off = in_of(4, s, 0);
    mjam_bar(&off, 1, &s, a);
    mjam_bar(&on, 1, &s, b);
    CHECK(!memcmp(a, b, sizeof a), "a chord with no note in its span: nothing moves");
    mjam_bar(&on, 3, &s, b);
    CHECK(b[0].note % 12 == CH[3].fifth, "a one-note bar with a breath: the note is still the pick (%s)", NAME[b[0].note % 12]);
}

static void test_lands(void) {
    static const int CGFG[4] = {0, 4, 3, 4};
    mjam_set_t s = mjam_default();
    mjam_note_t l[SEQ_STEPS];
    int last = -1;
    loop_of(CGFG, 4);
    s.c = MJ_HOOK, s.l = 1;
    mjam_loop_in_t in = in_of(4, s, 0);
    mjam_bar(&in, 3, &s, l);
    for (int i = 0; i < SEQ_STEPS; i++) if (l[i].note) last = i;
    CHECK(l[last].note % 12 == 0, "Home over G, as today: the key's C (%s)", NAME[l[last].note % 12]);
    s.cadc = 1;
    in = in_of(4, s, 0);
    mjam_bar(&in, 3, &s, l);
    CHECK(l[last].note % 12 == CH[3].root || l[last].note % 12 == CH[3].third || l[last].note % 12 == CH[3].fifth, "tapped: a note of G (%s)", NAME[l[last].note % 12]);
    static const int POP[4] = {0, 4, 5, 3};
    loop_of(POP, 4);
    in = in_of(4, s, 0);
    mjam_bar(&in, 3, &s, l);
    CHECK(l[last].note % 12 == 0, "F holds the key's C, so it stays (%s)", NAME[l[last].note % 12]);

    /* a pick beats Lands on the same note */
    s = mjam_default();
    s.c = MJ_LEAD, s.l = 0;
    LAND[3][0] = MJ_LAND_CODE(TONE_3, 0);
    in = in_of(4, s, 1);
    mjam_bar(&in, 3, &s, l);
    CHECK(l[0].note % 12 == CH[3].third, "a one-note bar: the pick, not Lands (%s)", NAME[l[0].note % 12]);
}

static void test_land_notes(void) {
    static const int POP[4] = {0, 4, 5, 3};
    mjam_set_t s = mjam_default();
    unsigned char o[MJ_LAND_NOTES];
    mjam_note_t l[SEQ_STEPS];
    loop_of(POP, 4);
    mjam_loop_in_t in = in_of(4, s, 0);
    mjam_land_notes(&in, 0, &s, 0, SEQ_STEPS, o);
    CHECK(o[0] == 60, "as the tune: C4 (%d)", o[0]);
    for (int t = TONE_R; t <= TONE_7; t++)
        for (int oct = -1; oct <= 1; oct++) {
            LAND[0][0] = (unsigned char)MJ_LAND_CODE(t, oct);
            mjam_loop_in_t on = in_of(4, s, 1);
            mjam_bar(&on, 0, &s, l);
            CHECK(l[0].note == o[MJ_LAND_AT(t, oct)], "tone %d octave %d plays what the table says: %d, %d", t, oct, l[0].note, o[MJ_LAND_AT(t, oct)]);
        }
}


/* ---- the core: C G Am F, a bar each, in C major ---- */
typedef struct { int status, note, vel; long at; } ev_t;
static ev_t EV[20000];
static int nev;
static long NOW;
static void rec(void *ctx, int status, int note, int vel, int frame) {
    (void)ctx;
    if (nev < 20000) EV[nev++] = (ev_t){status, note, vel, NOW + frame};
}

static seq_t a, b;   /* a seq_t is large: never on the stack */
static char state[SEQ_STATE_MAX];

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
    seq_mel_queue(s, MJ_HOOK, 1);
}
static const char *lineof(const seq_t *s, int bar) {
    static char out[128];
    mjam_note_t l[16];
    int o = 0;
    out[0] = 0;
    seq_mel_line(s, bar, l);
    for (int i = 0; i < 16; i++) o += snprintf(out + o, sizeof out - (size_t)o, "%s%s", i ? " " : "", l[i].note ? NAME[l[i].note % 12] : l[i].hold ? "-" : ".");
    return out;
}
static int first_note(const seq_t *s, int bar) {
    mjam_note_t l[16];
    seq_mel_line(s, bar, l);
    for (int i = 0; i < 16; i++) if (l[i].note) return l[i].note;
    return 0;
}
static void all_lines(const seq_t *s, mjam_note_t *out) { for (int b_ = 0; b_ < seq_bars(s); b_++) seq_mel_line(s, b_, &out[b_ * 16]); }
static int same_lines(const seq_t *x, const seq_t *y) {
    static mjam_note_t p[256], q[256];
    all_lines(x, p);
    all_lines(y, q);
    return !memcmp(p, q, sizeof(mjam_note_t) * 16 * (size_t)seq_bars(x));
}

static void test_core_picks(void) {
    static const unsigned char TONES[4] = {TONE_3, TONE_5, TONE_3, TONE_5};
    static const char *W[4] = {"E", "D", "C", "C"};
    seq_feel_chord_t c[64];
    int before;
    fresh(&a);
    before = first_note(&a, 0);
    CHECK(before % 12 == 7, "Hook Motif starts on G (%s)", NAME[before % 12]);
    for (int i = 0; i < 4; i++) seq_mel_tone_set(&a, i, TONES[i], 0);
    for (int i = 0; i < 4; i++) CHECK(!strcmp(NAME[first_note(&a, i) % 12], W[i]), "chord %d lands on %s (%s)", i, W[i], NAME[first_note(&a, i) % 12]);
    seq_feel_chords(&a, c, 64);
    CHECK(c[1].mtone == TONE_5 && c[1].moct == 0, "the chord reads its pick");

    /* it's a job, not a pitch */
    seq_set_key(&a, 2);
    CHECK(first_note(&a, 0) % 12 == 6, "the key goes to D: the 3rd of its first chord is F# (%s)", NAME[first_note(&a, 0) % 12]);
    seq_set_key(&a, 0);
    CHECK(first_note(&a, 0) % 12 == 4, "back in C: E");

    /* a Pedal block makes them wait, another brings them back */
    seq_mel_queue(&a, MJ_PEDAL, 2);
    CHECK(first_note(&a, 0) % 12 != 4, "a Pedal block: the pick waits (%s)", NAME[first_note(&a, 0) % 12]);
    seq_mel_queue(&a, MJ_ARP, 0);
    CHECK(first_note(&a, 0) % 12 == 4, "a block changed: the pick stays");

    /* one undo step each; Clear all */
    seq_mel_tone_set(&a, 0, TONE_5, 1);
    CHECK(first_note(&a, 0) % 12 == 7, "the 5th, an octave up");
    seq_undo(&a);
    CHECK(first_note(&a, 0) % 12 == 4, "undo: the 3rd again");
    seq_mel_tones_clear(&a);
    CHECK(first_note(&a, 0) % 12 == 0, "Clear all: Arp Up starts on C");
    seq_undo(&a);
    CHECK(first_note(&a, 0) % 12 == 4, "undo: the picks are back");
    int n = a.undo_n;
    seq_mel_tone_set(&a, 0, TONE_3, 0);
    CHECK(a.undo_n == n, "the same pick again: no undo step");

    /* a chord two bars long has one pick */
    fresh(&a);
    static const int TWO[2] = {0, 3};
    static const unsigned char S2[2] = {0}, C2[2] = {COL_TRIAD, COL_TRIAD};
    seq_set_chord_len(&a, 32);
    seq_set_progression(&a, 2, TWO, S2, C2, 0);
    seq_mel_tone_set(&a, 0, TONE_3, 0);
    CHECK(first_note(&a, 0) % 12 == 4, "a long chord lands where it starts");
    CHECK(first_note(&a, 1) % 12 == 7, "and bar 2 plays as the tune: G (%s)", NAME[first_note(&a, 1) % 12]);
}

static void test_core_step_picks(void) {
    fresh(&a);
    mjam_note_t l[16];
    seq_mel_note(&a, 0, 12, TONE_5, 0);
    seq_mel_line(&a, 0, l);
    CHECK(l[12].note % 12 == 7 && seq_mel_pick(&a, 0, 12), "a step's pick: the 5th of C");
    seq_mel_note(&a, 0, 12, TONE_5, 1);
    seq_mel_line(&a, 0, l);
    int up = l[12].note;
    seq_mel_note(&a, 0, 12, TONE_5, 0);
    seq_mel_line(&a, 0, l);
    CHECK(up == l[12].note + 12, "its octave");
    seq_mel_note(&a, 0, 12, MP_BELOW, 0);
    seq_mel_line(&a, 0, l);
    CHECK(l[12].note % 12 == (first_note(&a, 1) + 11) % 12, "half step below the next chord's first note (%s, next %s)", NAME[l[12].note % 12], NAME[first_note(&a, 1) % 12]);
    seq_mel_note(&a, 0, 12, MP_ABOVE, 0);
    seq_mel_line(&a, 0, l);
    CHECK(l[12].note % 12 == (first_note(&a, 1) + 1) % 12, "half step above it");
    seq_mel_note(&a, 0, 12, MP_BLUE, 0);
    seq_mel_line(&a, 0, l);
    CHECK(l[12].note % 12 == 3, "blue: the half step under C's 3rd (%s)", NAME[l[12].note % 12]);
    seq_mel_note(&a, 0, 12, 0, 0);
    CHECK(!seq_mel_pick(&a, 0, 12), "back to the tune");

    /* a step's pick follows the key */
    seq_mel_note(&a, 0, 12, TONE_3, 0);
    seq_set_key(&a, 2);
    seq_mel_line(&a, 0, l);
    CHECK(l[12].note % 12 == 6, "in D the pick is F# (%s)", NAME[l[12].note % 12]);
    seq_set_key(&a, 0);

    /* a Force note */
    fresh(&a);
    seq_medit_open(&a, 1);
    seq_medit_select(&a, 4);
    seq_medit_note(&a, 1);
    seq_medit_done(&a);
    CHECK(seq_mel_off_key(&a, 1) == 4 && seq_mel_fixed(&a, 1, 4) > 0, "a C# fixed on the Force warns");
    CHECK(seq_mel_off_key(&a, 0) == -1, "bar 1 has none");
    seq_mel_follow(&a, 1);
    CHECK(seq_mel_off_key(&a, 1) == -1 && !seq_mel_fixed(&a, 1, 4), "Make it follow the chord");
    seq_undo(&a);
    CHECK(seq_mel_off_key(&a, 1) == 4, "undo brings it back");

    /* paint */
    fresh(&a);
    CHECK(seq_tweak_paint(&a, LANE_MEL_UP, 0, 0, RC_REST) == 1, "Rest paints");
    seq_mel_line(&a, 0, l);
    CHECK(!l[0].note && !l[0].hold, "a rest");
    CHECK(seq_tweak_paint(&a, LANE_MEL_UP, 0, 0, RC_HOLD) == 0, "a hold with nothing before it is refused");
    CHECK(seq_tweak_paint(&a, LANE_MEL_UP, 0, 0, RC_HIT) == 1, "Note paints");
    seq_mel_line(&a, 0, l);
    CHECK(l[0].note % 12 == 0 && seq_mel_pick(&a, 0, 0), "the chord's root, as a pick");
    CHECK(seq_tweak_paint(&a, LANE_MEL_UP, 0, 3, RC_HOLD) == 1, "Hold paints after a note");
    seq_undo(&a);
    seq_undo(&a);
    seq_undo(&a);
    b = a;
    fresh(&a);
    CHECK(same_lines(&a, &b), "three undos: the bar as it was");

    /* Try another, Back to the tune, Keep */
    fresh(&a);
    char was[128];
    strcpy(was, lineof(&a, 1));
    CHECK(seq_tweak_try(&a, LANE_MEL_UP, 1) == 1 && strcmp(was, lineof(&a, 1)), "Try another: another line");
    seq_tweak_reset(&a, LANE_MEL_UP, 1);
    CHECK(!strcmp(was, lineof(&a, 1)), "Back to the tune");
    CHECK(seq_medit_bar(&a) < 0, "no bar left open");
    seq_tweak_keep(&a, LANE_MEL_UP, 1);
    CHECK(seq_mel_bar_locked(&a, 1), "Keep");
    seq_mel_queue(&a, MJ_ARP, 0);
    CHECK(!strcmp(was, lineof(&a, 1)), "a kept bar stays through a block");
    seq_tweak_keep(&a, LANE_MEL_UP, 1);
    CHECK(!seq_mel_bar_locked(&a, 1), "and lets go");
}

static void test_core_copy(void) {
    fresh(&a);
    seq_mel_tone_set(&a, 0, TONE_3, 0);
    seq_tweak_paint(&a, LANE_MEL_UP, 0, 4, RC_REST);
    seq_tweak_keep(&a, LANE_MEL_UP, 3);
    char kept[128];
    strcpy(kept, lineof(&a, 3));
    seq_tweak_copy(&a, LANE_MEL_UP, 0, 0xE, 0);
    mjam_note_t l[16], s0[16];
    seq_mel_line(&a, 0, s0);
    seq_mel_line(&a, 2, l);
    CHECK(l[0].note % 12 == 0, "Am takes the same job, its 3rd: C (%s)", NAME[l[0].note % 12]);
    CHECK(!l[4].note, "the rest copies");
    CHECK(!strcmp(kept, lineof(&a, 3)), "a kept bar is skipped");
    seq_undo(&a);
    CHECK(!seq_mel_pick(&a, 2, 0), "undo takes the copy back");
}

static void test_core_rows(void) {
    fresh(&a);
    for (int what = 0; what < ML_COUNT; what++) {
        int words = seq_mel_row_words(what);
        CHECK(words >= 3 && words <= 6, "row %d has %d words", what, words);
        for (int w = 0; w < words; w++) {
            int was = seq_mel_row_get(&a, what), n = a.undo_n;
            seq_mel_row(&a, what, w);
            CHECK(seq_mel_row_get(&a, what) == w, "row %d word %d reads back (%d)", what, w, seq_mel_row_get(&a, what));
            if (was == w && what != ML_LANDS) CHECK(a.undo_n == n, "row %d word %d again: no undo step", what, w);
            if (was != w) {
                seq_undo(&a);
                CHECK(seq_mel_row_get(&a, what) == was, "row %d undo brings %d back (%d)", what, was, seq_mel_row_get(&a, what));
                seq_mel_row(&a, what, w);
            }
        }
    }
    fresh(&a);
    CHECK(seq_mel_row_get(&a, ML_ANSWER) == 0 && seq_mel_row_get(&a, ML_FORM) == 0 && seq_mel_row_get(&a, ML_STRAY) == 1, "a new song: Repeats, Round and round, Last beat");
    seq_mel_row(&a, ML_ANSWER, MA_MIRROR);
    CHECK(seq_mel_row_get(&a, ML_FORM) == MF_1_1, "an answer on Round and round: One and one");
    CHECK(seq_mel_row_get(&a, ML_STRAY) == 3, "leaving Repeats: All");
    seq_mel_row(&a, ML_STRAY, 0);
    seq_mel_row(&a, ML_ANSWER, MA_SHIFT);
    CHECK(seq_mel_row_get(&a, ML_STRAY) == 0, "already left: How much stays");
    seq_mel_queue(&a, MJ_RUN, 2);
    seq_mel_row(&a, ML_ANSWER, MA_SWAP);
    CHECK(seq_mjam_get(&a, MJS_ANS) == MA_SWAP && (a.part[0].mjam.set.sc != MJ_RUN || a.part[0].mjam.set.sl != 2), "Another idea when it's the call's own: another block");
    int n = a.undo_n;
    seq_mel_answer_block(&a, MJ_HOOK, 0);
    CHECK(a.part[0].mjam.set.sc == MJ_HOOK && a.part[0].mjam.set.sl == 0 && a.undo_n == n + 1, "the answer's block, one undo step");
    seq_undo(&a);
    CHECK(a.part[0].mjam.set.sc != MJ_HOOK, "undo");

    /* Lands: a tap marks the part, even on the word lit */
    fresh(&a);
    CHECK(!a.part[0].mjam.set.cadc && seq_mel_row_get(&a, ML_LANDS) == 3, "Home, not tapped");
    n = a.undo_n;
    seq_mel_row(&a, ML_LANDS, 3);
    CHECK(a.part[0].mjam.set.cadc && a.undo_n == n + 1, "tapped");
    n = a.undo_n;
    seq_mel_row(&a, ML_LANDS, 3);
    CHECK(a.undo_n == n, "again: nothing");

    /* a recipe lane turns JAM first: that's the undo step */
    seq_init(&a, rec, NULL);
    seq_init_preset(&a);
    seq_set_style(&a, LANE_MEL_UP, GEN_MEL_HOOK);
    int was_jam = a.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM;
    n = a.undo_n;
    seq_mel_row(&a, ML_BREATH, 2);
    CHECK(!was_jam && a.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && a.undo_n == n + 1, "a word turns a recipe lane JAM, one step");
    seq_undo(&a);
    CHECK(a.part[0].lane[LANE_MEL_UP].gen.style != GEN_MEL_JAM, "undo brings the recipe back");

    /* Smooth */
    fresh(&a);
    seq_mel_smooth(&a);
    for (int i = 1; i < 4; i++) {
        int d = first_note(&a, i) - first_note(&a, i - 1);
        CHECK((d < 0 ? -d : d) <= 4, "Smooth: bar %d is within a skip of the one before (%d)", i + 1, d);
    }
    seq_undo(&a);
    seq_feel_chord_t c[64];
    seq_feel_chords(&a, c, 64);
    CHECK(!c[1].mtone && !c[2].mtone, "one undo takes it all back");
    fresh(&a);
    unsigned char t[4 * MCN_SIZE];
    CHECK(seq_mel_chord_notes(&a, t, 4) == 4, "four chords");
    CHECK(t[MCN_NOW] == t[MCN_TUNE] && t[MCN_TUNE] % 12 == 7, "plays as the tune: G");
    seq_mel_tone_set(&a, 1, t[MCN_STEP_TONE], t[MCN_STEP_OCT] - 1);
    CHECK(first_note(&a, 1) == t[MCN_SIZE + MCN_TABLE + MJ_LAND_AT(t[MCN_STEP_TONE], t[MCN_STEP_OCT] - 1) - 1], "the next chord's nearest stop plays what the table says");
}

static void test_save(void) {
    fresh(&a);
    int n0 = (int)seq_state_save(&a, state, sizeof state);
    static const char *const NEW[] = {".mt=", "3.jp=", "3.jt=", "3.je=", "3.jc="};
    for (int i = 0; i < 5; i++) CHECK(!strstr(state, NEW[i]), "a song that uses nothing doesn't write %s (%d bytes)", NEW[i], n0);
    seq_mel_tone_set(&a, 0, TONE_3, 1);
    seq_mel_tone_set(&a, 2, TONE_7, -1);
    seq_mel_note(&a, 1, 4, TONE_6, 1);
    seq_tweak_paint(&a, LANE_MEL_UP, 1, 8, RC_REST);
    seq_tweak_keep(&a, LANE_MEL_UP, 0);
    seq_mel_row(&a, ML_EARLY, 2);
    seq_mel_row(&a, ML_LOOSE, 3);
    seq_mel_row(&a, ML_LANDS, 2);
    seq_tweak_keep(&a, LANE_MEL_UP, 3);
    seq_state_save(&a, state, sizeof state);
    for (int i = 0; i < 5; i++) CHECK(strstr(state, NEW[i]) != NULL, "%s is written", NEW[i]);
    const char *jf = strstr(state, "3.jf=");
    CHECK(jf != NULL, "the rest is in 3.jf");
    for (const char *e = jf ? jf + 5 : NULL; e && e[0] && e[1] && e[2] && e[3] && e[0] != '|'; e += 4) {
        int hi = e[2] <= '9' ? e[2] - '0' : e[2] - 'a' + 10, lo = e[3] <= '9' ? e[3] - '0' : e[3] - 'a' + 10;
        CHECK(hi * 16 + lo <= MJ_FIX_HOLD, "3.jf holds no pick (%d)", hi * 16 + lo);
    }
    seq_init(&b, rec, NULL);
    seq_state_load(&b, state);
    CHECK(same_lines(&a, &b), "the same notes after a reload");
    CHECK(b.part[0].mjam.early == 2 && b.part[0].mjam.loose == 3 && b.part[0].mjam.set.cadc == 1, "Early, Loose, Lands");
    CHECK(b.part[0].mjam.frozen[3].cadc == 1 && b.part[0].mjam.frozen[0].cadc == 0, "kept bars keep their Lands");
    CHECK(!memcmp(b.part[0].mjam.fland[0], a.part[0].mjam.fland[0], 16) && b.part[0].mjam.fland[0][0], "a kept bar keeps its picks");

    /* a chord's pick changing doesn't move a kept bar, also after a reload */
    char was[128];
    strcpy(was, lineof(&b, 0));
    seq_mel_tone_set(&b, 0, TONE_5, 0);
    CHECK(!strcmp(was, lineof(&b, 0)), "the kept bar stays as it was");
}

static void test_early(void) {
    mjam_note_t l[16], m[16];
    fresh(&a);
    seq_mel_row(&a, ML_EARLY, 2);
    seq_mel_line(&a, 0, l);
    seq_mel_line(&a, 1, m);
    b = a;
    CHECK(l[14].note && !m[0].note && m[0].hold, "8th: bar 2's first note starts at step 15 of bar 1: %s | %s", lineof(&a, 0), lineof(&b, 1));
    fresh(&b);
    CHECK(l[14].note == first_note(&b, 1), "the note it was (%s)", NAME[l[14].note % 12]);
    const seq_step_t *st = &a.part[0].lane[LANE_MEL_UP].steps[14];
    CHECK(st->on && st->len > 2, "it rings past the bar line (%d steps)", st->len);
    mjam_note_t wrap[16];
    seq_mel_line(&a, 3, wrap);
    CHECK(wrap[14].note == first_note(&b, 0), "at the wrap, the first chord's note ends the last bar");

    /* a note in the window: no move */
    fresh(&a);
    seq_mel_queue(&a, MJ_ARP, 3);   /* 16ths */
    seq_mel_row(&a, ML_EARLY, 2);
    seq_mel_line(&a, 1, m);
    CHECK(m[0].note != 0, "a note starts between its new step and the chord: nothing moves");

    /* a kept bar stays */
    fresh(&a);
    seq_tweak_keep(&a, LANE_MEL_UP, 1);
    seq_mel_row(&a, ML_EARLY, 2);
    seq_mel_line(&a, 1, m);
    CHECK(m[0].note != 0, "a kept bar keeps its first note");

    /* Early off: as it was */
    fresh(&a);
    fresh(&b);
    seq_mel_row(&a, ML_EARLY, 2);
    seq_mel_row(&a, ML_EARLY, 0);
    CHECK(same_lines(&a, &b), "Early back off: the lines as they were");
}

/* ---- playing: the host's song position through the core; the Melody alone ---- */
static void play_from(seq_t *s, double ppq0, double beats) {
    const int frames = 128;
    const double dp = frames * 120.0 / 60.0 / 44100.0;
    nev = 0;
    s->part[0].lane[LANE_CHORDS].mute = 1;
    s->part[0].lane[LANE_BASS].mute = 1;
    NOW = 0;
    for (double ppq = ppq0; ppq < ppq0 + beats; ppq += dp, NOW += frames) seq_block(s, ppq, 120, 1, frames);
    seq_block(s, -1, 120, 0, frames);
}
static int ons(int *note, long *at, int max) {
    int n = 0;
    for (int i = 0; i < nev && n < max; i++)
        if ((EV[i].status & 0xF0) == 0x90 && EV[i].vel > 0) note[n] = EV[i].note, at[n] = EV[i].at, n++;
    return n;
}

static void test_play(void) {
    static int n0[512], n1[512], n2[512];
    static long t0[512], t1[512], t2[512];
    fresh(&a);
    play_from(&a, 0, 16);
    int c0 = ons(n0, t0, 512);
    CHECK(c0 > 10, "the tune plays: %d notes", c0);

    fresh(&a);
    seq_mel_row(&a, ML_LOOSE, 4);
    play_from(&a, 0, 16);
    int c1 = ons(n1, t1, 512), same = c1 == c0, late = 1, moved = 0;
    for (int i = 0; i < c0 && same; i++) {
        long d = t1[i] - t0[i];
        same = n1[i] == n0[i];
        late = late && d >= -128 && d <= 16 * 44100 / 1000 + 128;
        moved |= d > 128;
    }
    CHECK(same, "Loose: the same notes (%d, %d)", c0, c1);
    CHECK(late && moved, "Loose: up to 16 ms late, and some are");
    fresh(&b);
    seq_mel_row(&b, ML_LOOSE, 4);
    play_from(&b, 0, 16);
    int c2 = ons(n2, t2, 512), again = c2 == c1;
    for (int i = 0; i < c1 && again; i++) again = t2[i] == t1[i];
    CHECK(again, "and the same way on every run");

    fresh(&a);
    seq_mel_row(&a, ML_LOOSE, 4);
    seq_mel_row(&a, ML_LOOSE, 0);
    play_from(&a, 0, 16);
    c1 = ons(n1, t1, 512);
    same = c1 == c0;
    for (int i = 0; i < c0 && same; i++) same = n1[i] == n0[i] && t1[i] == t0[i];
    CHECK(same, "Loose back off: every note where it was");

    /* Early: the song's first note still sounds, the next chord's comes ahead of the bar line */
    fresh(&a);
    int want = first_note(&a, 0), next = first_note(&a, 1);
    seq_mel_row(&a, ML_EARLY, 2);
    play_from(&a, 0, 8);
    c1 = ons(n1, t1, 512);
    CHECK(c1 > 0 && n1[0] == want && t1[0] < 44100 / 10, "the song's first note still sounds from the start (%s at %ld)", c1 ? NAME[n1[0] % 12] : "-", c1 ? t1[0] : -1);
    long bar = 44100 * 2, sixteenth = bar / 16;
    int ahead = 0;
    for (int i = 0; i < c1; i++) ahead |= n1[i] == next && t1[i] >= bar - 2 * sixteenth - 256 && t1[i] <= bar - 2 * sixteenth + 256;
    CHECK(ahead, "bar 2's first note comes an 8th ahead of the bar line");
    /* its note-off comes after the line */
    int off_after = 0;
    for (int i = 0; i < nev; i++)
        if (((EV[i].status & 0xF0) == 0x80 || ((EV[i].status & 0xF0) == 0x90 && !EV[i].vel)) && EV[i].note == next && EV[i].at > bar && EV[i].at < 2 * bar) off_after = 1;
    CHECK(off_after, "and rings past it");
}

/* ---- the codes ---- */
static void test_codes(void) {
    for (int t = TONE_R; t <= MP_BLUE; t++)
        for (int oct = -1; oct <= 1; oct++) {
            int c = mjam_pick_code(t, oct);
            CHECK(mjam_is_pick(c) && c > MJ_FIX_HOLD && c < 256 && mjam_pick_tone(c) == t && mjam_pick_oct(c) == oct, "pick %d %d: code %d", t, oct, c);
        }
    CHECK(!mjam_is_pick(127) && !mjam_is_pick(MJ_FIX_REST) && !mjam_is_pick(MJ_FIX_HOLD) && !mjam_is_pick(0), "a note, a rest and a hold are not picks");
}

int main(void) {
    test_picks_move_the_chords_notes();
    test_sweep();
    test_edges();
    test_lands();
    test_land_notes();
    test_codes();
    test_core_picks();
    test_core_step_picks();
    test_core_copy();
    test_core_rows();
    test_save();
    test_early();
    test_play();
    printf(fails ? "test_melody_feel: %d FAILED\n" : "test_melody_feel: ok\n", fails);
    return fails != 0;
}
