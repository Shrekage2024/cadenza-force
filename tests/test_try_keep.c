/* Offline test of Try and keep's core (iOS): the chord each step plays, the bars whose chord changed, and "chords lead" (the cascade: the Bass's
 * and the Melody's absolute fixes and kept bars go where the chord changed, one undo step with the chord verb). Run by tests/run_tests.sh under
 * ASan/UBSan. Exit 1 on failure. */
#include <stdio.h>
#include <string.h>
#include "seq_core.h"
#include "feel.h"
#include "melody2.h"
#include "bassline.h"
#include "theory.h"
#include "rhythm.h"
#include "jamlib.h"
#include "gen.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c) != 0; printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static void rec(void *ctx, int status, int note, int vel, int frame) { (void)ctx, (void)status, (void)note, (void)vel, (void)frame; }

static seq_t a;   /* a seq_t is large: never on the stack */

/* C G Am F in C major, a chord a bar, 4 bars; the Melody JAM (Hook) */
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

static seq_part_t *playing(seq_t *s) { return &s->part[s->cur_part]; }

/* Undo steps (a joined snapshot is one step with the one before it) */
static int undo_steps(const seq_t *s) {
    int n = 0;
    for (int k = 0; k < s->undo_n; k++) n += !s->undo[(s->undo_top + SEQ_UNDO - 1 - k) % SEQ_UNDO].join;
    return n;
}

static unsigned short before[SEQ_LANE_STEPS], after[SEQ_LANE_STEPS];

static void test_chord_steps(void) {
    fresh(&a);
    seq_chord_steps(&a, playing(&a), before);
    int same = 1;
    for (int i = 1; i < 16; i++) same &= before[i] == before[0];
    CHECK(same && before[0], "bar 1's 16 steps play one chord (%#x)", before[0]);
    CHECK(before[16] && before[16] != before[0], "bar 2's first step plays another (%#x vs %#x)", before[16], before[0]);
    CHECK(before[0] == 1 && before[16] == (7 + 1), "C major reads root C + 1, quality 0; G major root 7 + 1 (%#x, %#x)", before[0], before[16]);
    CHECK(before[32] == ((9 + 1) | 1 << 4), "A minor: root 9 + 1, quality 1 << 4 (%#x)", before[32]);
    CHECK(!before[4 * 16] && !before[SEQ_LANE_STEPS - 1], "0 past the part's bars");
    CHECK(seq_chords_changed(before, before, 4) == 0, "nothing changed: no bar");

    seq_feel_pick(&a, 1, 2, 0, COL_TRIAD);   /* the Feel page's PICK: bar 2's G becomes Em */
    seq_chord_steps(&a, playing(&a), after);
    CHECK(after[16] == ((4 + 1) | 1 << 4), "bar 2 now plays E minor (%#x)", after[16]);
    CHECK(seq_chords_changed(before, after, 4) == 1u << 1, "only bar 2 changed (%#x)", seq_chords_changed(before, after, 4));
    after[3 * 16 + 9] ^= 1;
    CHECK(seq_chords_changed(before, after, 4) == (1u << 1 | 1u << 3), "one step of bar 4 differing marks bar 4 too");
    CHECK(seq_chords_changed(before, after, 3) == 1u << 1, "bars past `bars` are not read");
}

/* seq_chords_diff is seq_chords_changed of the two parts' seq_chord_steps; seq_part_at is the part in place, NULL out of range */
static void test_chords_diff_and_part_at(void) {
    static seq_part_t was;
    fresh(&a);
    CHECK(seq_part_at(&a, a.cur_part) == playing(&a) && seq_part_at(&a, 1) == &a.part[1], "seq_part_at: the part in place");
    CHECK(!seq_part_at(&a, -1) && !seq_part_at(&a, SEQ_PARTS), "and NULL out of range");
    was = *playing(&a);
    CHECK(seq_chords_diff(&a, &was, playing(&a), 4) == 0, "the same chords: no bar");
    seq_feel_pick(&a, 1, 2, 0, COL_TRIAD);   /* bar 2's G becomes Em */
    seq_chord_steps(&a, &was, before);
    seq_chord_steps(&a, playing(&a), after);
    CHECK(seq_chords_diff(&a, &was, playing(&a), 4) == 1u << 1 && seq_chords_diff(&a, &was, playing(&a), 4) == seq_chords_changed(before, after, 4),
          "bar 2 changed, as seq_chords_changed reads it (%#x)", seq_chords_diff(&a, &was, playing(&a), 4));
    CHECK(seq_chords_diff(&a, &was, playing(&a), 1) == 0, "bars past `bars` are not read");
}

static void test_follow_part_drops_only_absolute(void) {
    seq_part_t *p;
    int bpick = bass_pick_code(TONE_3, 0), mpick = mjam_pick_code(TONE_5, 0);
    fresh(&a);
    p = playing(&a);
    p->bass.fix[16 + 0] = 40;
    p->bass.fix[16 + 4] = (unsigned char)bpick;
    p->mjam.fix[16 + 2] = 72;
    p->mjam.fix[16 + 6] = (unsigned char)mpick;
    p->mjam.fix[16 + 8] = MJ_FIX_REST;
    p->mjam.fix[16 + 9] = MJ_FIX_HOLD;
    p->bass.nlock |= 1u << 1;
    p->mjam.nlock |= 1u << 1;
    p->mjam.fland[1][0] = (unsigned char)MJ_LAND_CODE(TONE_3, 0);
    /* bar 1: the same, outside the mask */
    p->bass.fix[0] = 36;
    p->mjam.fix[3] = 67;
    p->bass.nlock |= 1u << 0;
    p->mjam.nlock |= 1u << 0;
    p->mjam.fland[0][0] = (unsigned char)MJ_LAND_CODE(TONE_5, 0);

    CHECK(seq_follow_part(p, 1u << 1) == 1, "bar 2 had absolute fixes and kept bars: changed");
    CHECK(!p->bass.fix[16 + 0] && !p->mjam.fix[16 + 2], "the absolute fixes went (%d, %d)", p->bass.fix[16 + 0], p->mjam.fix[16 + 2]);
    CHECK(p->bass.fix[16 + 4] == bpick && p->mjam.fix[16 + 6] == mpick, "the picks stay");
    CHECK(p->mjam.fix[16 + 8] == MJ_FIX_REST && p->mjam.fix[16 + 9] == MJ_FIX_HOLD, "a painted rest and hold stay");
    CHECK(!(p->bass.nlock >> 1 & 1) && !(p->mjam.nlock >> 1 & 1), "bar 2 is no longer kept, on either lane");
    CHECK(!p->mjam.fland[1][0], "and the Melody's kept picks for it went");
    CHECK(p->bass.fix[0] == 36 && p->mjam.fix[3] == 67 && (p->bass.nlock & 1) && (p->mjam.nlock & 1) && p->mjam.fland[0][0],
          "bar 1, outside the mask, is untouched");
    CHECK(seq_follow_part(p, 1u << 1) == 0, "a second call changes nothing");
    CHECK(seq_follow_part(p, 0) == 0, "an empty mask changes nothing");
    CHECK(seq_follow_part(p, 1u << 31) == 0, "a bar past the last is ignored");
}

static unsigned char bfix[SEQ_LANE_STEPS], mfix[SEQ_LANE_STEPS];

/* The first step of a bar's notes that sounds, -1 none */
static int first_sounding(const unsigned char note[16]) {
    for (int i = 0; i < 16; i++)
        if (note[i]) return i;
    return -1;
}
/* 1 when bar `bar` of the Bass (lane 0) or the Melody (lane 1) plays MIDI note `want` as it renders now */
static int plays(const seq_t *s, int lane, int bar, int want) {
    unsigned char note[16];
    if (lane) seq_mel_notes(s, bar, note);
    else seq_tweak_notes(s, bar, note);
    for (int i = 0; i < 16; i++)
        if (note[i] == want) return 1;
    return 0;
}

static void test_follow_chords_one_undo(void) {
    enum { BASS_NOTE = 39, MEL_NOTE = 73 };   /* D#2 and C#5: off C major, so only a fixed note plays them */
    unsigned char note[16];
    seq_part_t *p;
    int steps, n, bs, ms;
    fresh(&a);
    p = playing(&a);
    seq_tweak_notes(&a, 1, note);
    bs = first_sounding(note);
    seq_mel_notes(&a, 1, note);
    ms = first_sounding(note);
    CHECK(bs >= 0 && ms >= 0, "bar 2 sounds on the Bass (step %d) and the Melody (step %d)", bs + 1, ms + 1);
    if (bs < 0 || ms < 0) return;
    CHECK(!plays(&a, 0, 1, BASS_NOTE) && !plays(&a, 1, 1, MEL_NOTE), "neither plays the off-key note on its own");
    p->bass.fix[16 + bs] = BASS_NOTE;
    p->mjam.fix[16 + ms] = MEL_NOTE;
    p->bass.frozen[1] = p->bass.set;
    p->bass.nlock |= 1u << 1;
    p->mjam.frozen[1] = p->mjam.set;
    p->mjam.nlock |= 1u << 1;
    p->mjam.fland[1][0] = (unsigned char)MJ_LAND_CODE(TONE_3, 0);
    memcpy(bfix, p->bass.fix, sizeof bfix);
    memcpy(mfix, p->mjam.fix, sizeof mfix);
    seq_chord_steps(&a, p, before);
    steps = undo_steps(&a), n = a.undo_n;

    seq_follow_chords(&a, 0, 1);
    CHECK(a.undo_n == n && p->bass.fix[16 + bs] == BASS_NOTE, "no bar: nothing, no undo step");

    seq_feel_pick(&a, 1, 2, 0, COL_TRIAD);
    CHECK(plays(&a, 0, 1, BASS_NOTE) && plays(&a, 1, 1, MEL_NOTE), "after the chord verb bar 2 still plays the absolute fixes");
    seq_chord_steps(&a, p, after);
    unsigned mask = seq_chords_changed(before, after, p->bars);
    seq_follow_chords(&a, mask, 1);
    CHECK(!p->bass.fix[16 + bs] && !p->mjam.fix[16 + ms], "the cascade dropped bar 2's absolute fixes");
    CHECK(!plays(&a, 0, 1, BASS_NOTE) && !plays(&a, 1, 1, MEL_NOTE), "and bar 2 re-rendered: neither plays them now");
    CHECK(!(p->bass.nlock >> 1 & 1) && !(p->mjam.nlock >> 1 & 1) && !p->mjam.fland[1][0], "and unkept bar 2");
    CHECK(undo_steps(&a) == steps + 1, "the chord verb and the cascade are one undo step (%d -> %d)", steps, undo_steps(&a));

    n = a.undo_n;
    seq_follow_chords(&a, mask, 1);
    CHECK(a.undo_n == n, "again: nothing left to drop, no snapshot");

    /* knob turns after the cascade are not undo steps, and its Undo leaves them */
    int mov = (p->bass.set.k[BK_MOV] + 1) % 5, push = (p->mjam.push + 7) % 101;
    seq_set_bknob(&a, BK_MOV, mov);
    seq_set_mel_push(&a, push);
    CHECK(a.undo_n == n && p->bass.set.k[BK_MOV] == mov && p->mjam.push == push, "a Bass knob and the Melody's Push turned, no undo step");

    seq_undo(&a);
    seq_chord_steps(&a, p, after);
    CHECK(!memcmp(before, after, sizeof before), "one Undo: the chord is back");
    CHECK(!memcmp(bfix, p->bass.fix, sizeof bfix) && !memcmp(mfix, p->mjam.fix, sizeof mfix), "and the absolute fixes (%d, %d)",
          p->bass.fix[16 + bs], p->mjam.fix[16 + ms]);
    CHECK(plays(&a, 0, 1, BASS_NOTE) && plays(&a, 1, 1, MEL_NOTE), "and they play again");
    CHECK((p->bass.nlock >> 1 & 1) && (p->mjam.nlock >> 1 & 1) && p->mjam.fland[1][0], "and the kept bars");
    CHECK(p->bass.set.k[BK_MOV] == mov && p->mjam.push == push, "but the knobs turned since stay (Moves %d, Push %d)", p->bass.set.k[BK_MOV], p->mjam.push);
    CHECK(undo_steps(&a) == steps, "nothing else was undone (%d steps, was %d)", undo_steps(&a), steps);
}

/* join 0 (the chord verb pushed no undo step, as a browsed preset): the cascade is an undo step of its own, both lanes in it */
static void test_follow_chords_own_step(void) {
    seq_part_t *p;
    fresh(&a);
    p = playing(&a);
    p->bass.fix[16] = 39;
    p->mjam.fix[16 + 2] = 73;
    seq_chord_steps(&a, p, before);
    a.undo_n = 0;
    seq_feel_pick(&a, 1, 2, 0, COL_TRIAD);   /* bar 2's G becomes Em: its own step */
    seq_chord_steps(&a, p, after);
    seq_follow_chords(&a, seq_chords_changed(before, after, p->bars), 0);
    CHECK(!p->bass.fix[16] && !p->mjam.fix[16 + 2] && undo_steps(&a) == 2, "join 0: the fixes go in a step of their own (%d steps)", undo_steps(&a));
    seq_undo(&a);
    seq_chord_steps(&a, p, before);
    CHECK(p->bass.fix[16] == 39 && p->mjam.fix[16 + 2] == 73 && !memcmp(before, after, sizeof before) && undo_steps(&a) == 1,
          "one Undo: both lanes' fixes back, the chord stays");
    seq_undo(&a);
    CHECK(!a.undo_n, "and the next undoes the chord verb alone");
}

/* ---- the lane view (seq_lane_view on a scratch seq_t) */

static seq_t b;              /* the scratch */
static seq_part_t start;     /* a lane's start, as the app keeps it */

static const int LANES[3] = {LANE_CHORDS, LANE_BASS, LANE_MEL_UP};
static const char *const LANE_NAME[3] = {"Chords", "Bass", "Melody"};

/* 1 when bars [0, bars) of x and y read the same through every getter the app reads a view with */
static int reads_same(const seq_t *x, const seq_t *y, int bars) {
    seq_feel_chord_t cx[SEQ_LANE_STEPS], cy[SEQ_LANE_STEPS];
    int nx = seq_feel_chords(x, cx, SEQ_LANE_STEPS), ny = seq_feel_chords(y, cy, SEQ_LANE_STEPS);
    if (nx != ny || memcmp(cx, cy, sizeof cx[0] * (size_t)nx)) return 0;
    for (int bar = 0; bar < bars; bar++) {
        unsigned char nx16[16], ny16[16];
        mjam_note_t lx[16], ly[16];
        seq_tweak_notes(x, bar, nx16), seq_tweak_notes(y, bar, ny16);
        if (memcmp(nx16, ny16, 16)) return 0;
        seq_mel_line(x, bar, lx), seq_mel_line(y, bar, ly);
        if (memcmp(lx, ly, sizeof lx)) return 0;
        seq_bar_codes(x, LANE_CHORDS, bar, nx16), seq_bar_codes(y, LANE_CHORDS, bar, ny16);
        if (memcmp(nx16, ny16, 16)) return 0;
    }
    return 1;
}

/* A Bass tone and a Melody pick on chord 3 (Am, bar 3), so the per-chord fields are not all 0 */
static void edited(seq_t *s) {
    fresh(s);
    seq_bass_tone_set(s, 2, TONE_5);
    seq_mel_tone_set(s, 2, TONE_3, 0);
}

static void test_view_is_now_when_both_are_now(void) {
    edited(&a);
    seq_scratch_from(&b, &a);
    for (int k = 0; k < 3; k++) {
        seq_lane_view(&b, LANES[k], playing(&a), playing(&a));
        CHECK(reads_same(&a, &b, playing(&a)->bars), "%s over now's chords reads as now", LANE_NAME[k]);
        CHECK(seq_tweak_hash(&b, LANES[k]) == seq_tweak_hash(&a, LANES[k]), "and hashes as now (%#x, %#x)",
              seq_tweak_hash(&b, LANES[k]), seq_tweak_hash(&a, LANES[k]));
    }
}

static unsigned char start_bass[SEQ_MAX_BARS][16];

static void test_view_lane_over_old_chords(void) {
    unsigned char note[16];
    edited(&a);
    start = *playing(&a);
    for (int bar = 0; bar < 4; bar++) seq_tweak_notes(&a, bar, start_bass[bar]);
    seq_feel_pick(&a, 1, 2, 0, COL_TRIAD);   /* bar 2's G becomes Em */
    seq_tweak_notes(&a, 1, note);
    CHECK(memcmp(note, start_bass[1], 16) != 0, "bar 2's Bass moved with the new chord");

    seq_scratch_from(&b, &a);
    seq_lane_view(&b, LANE_BASS, playing(&a), &start);
    seq_tweak_notes(&b, 1, note);
    CHECK(!memcmp(note, start_bass[1], 16), "now's Bass over the start's chords: bar 2 plays as at the start");
    seq_lane_view(&b, LANE_BASS, playing(&a), playing(&a));
    CHECK(reads_same(&a, &b, 4), "over now's chords again: as now (the scratch keeps nothing from the view before)");
}

static void test_view_and_back_with_fewer_bars(void) {
    unsigned char note[16];
    int same = 1, off = 1;
    fresh(&a);
    seq_set_bars(&a, 8);
    seq_bass_tone_set(&a, 6, TONE_5);   /* bar 7: only the 8 bars have it */
    start = *playing(&a);
    for (int bar = 0; bar < 8; bar++) seq_tweak_notes(&a, bar, start_bass[bar]);
    seq_set_bars(&a, 4);

    seq_scratch_from(&b, &a);
    seq_lane_view(&b, LANE_BASS, &start, playing(&a));
    CHECK(playing(&b)->bars == 4, "the start's Bass over now's chords: now's 4 bars (%d)", playing(&b)->bars);
    for (int bar = 0; bar < 4; bar++) {
        seq_tweak_notes(&b, bar, note);
        same &= !memcmp(note, start_bass[bar], 16);
    }
    CHECK(same, "which play as the start's first 4");
    seq_tweak_notes(&b, 4, note);
    CHECK(first_sounding(note) < 0, "bar 5 reads silent");
    for (int t = 4 * 16; t < SEQ_LANE_STEPS; t++) off &= !playing(&b)->lane[LANE_BASS].steps[t].on;
    CHECK(off, "no Bass step past 64 is set");

    seq_lane_view(&b, LANE_BASS, playing(&a), &start);   /* and back: now's Bass over the start's 8 bars */
    same = 1;
    for (int bar = 0; bar < 8; bar++) {
        seq_tweak_notes(&b, bar, note);
        same &= !memcmp(note, start_bass[bar], 16);
    }
    CHECK(playing(&b)->bars == 8 && same, "now's Bass over the start's chords: 8 bars, as the start played them");
}

/* The Feel chord starting at step t, NULL none */
static const seq_feel_chord_t *chord_at(const seq_feel_chord_t *c, int n, int t) {
    for (int i = 0; i < n; i++)
        if (c[i].start == t) return &c[i];
    return NULL;
}

static void test_view_carries_picks_by_time(void) {
    seq_feel_chord_t c[SEQ_LANE_STEPS];
    const seq_feel_chord_t *am, *g;
    int n;
    edited(&a);   /* chord 3 (Am, bar 3, slot 2): Bass tone 5th, Melody pick 3rd */
    start = *playing(&a);
    seq_bass_tones_clear(&a);
    seq_mel_tones_clear(&a);
    seq_set_chord_len(&a, 8);   /* half-bar slots: Am is slots 4 and 5 now, slot 2 is G's first half */

    seq_scratch_from(&b, &a);
    seq_lane_view(&b, LANE_BASS, &start, playing(&a));
    n = seq_feel_chords(&b, c, SEQ_LANE_STEPS);
    am = chord_at(c, n, 32), g = chord_at(c, n, 16);
    CHECK(am && am->tone == TONE_5, "the start's Bass tone stays on Am, at its time (%d)", am ? am->tone : -1);
    CHECK(g && g->tone == 0, "not on slot 2, now G's (%d)", g ? g->tone : -1);
    unsigned char now_n[16], view_n[16];
    mjam_note_t now_l[16], view_l[16];
    seq_tweak_notes(&a, 2, now_n);
    seq_tweak_notes(&b, 2, view_n);
    int fn = first_sounding(now_n), fv = first_sounding(view_n);
    CHECK(fn >= 0 && fv >= 0 && now_n[fn] % 12 == 9 && view_n[fv] % 12 == 4, "and the view renders it: bar 3 starts on E, now on A (%d, %d)",
          fv >= 0 ? view_n[fv] : -1, fn >= 0 ? now_n[fn] : -1);

    seq_lane_view(&b, LANE_MEL_UP, &start, playing(&a));
    seq_mel_line(&a, 2, now_l);
    seq_mel_line(&b, 2, view_l);
    CHECK(memcmp(now_l, view_l, sizeof now_l) != 0, "the Melody's view renders its pick: bar 3 differs from now's");
    n = seq_feel_chords(&b, c, SEQ_LANE_STEPS);
    am = chord_at(c, n, 32), g = chord_at(c, n, 16);
    CHECK(am && am->mtone == TONE_3 && am->moct == 0, "the start's Melody pick stays on Am (%d)", am ? am->mtone : -1);
    CHECK(g && g->mtone == 0, "not on G (%d)", g ? g->mtone : -1);
    CHECK(am && am->tone == 0, "and the Melody's view keeps the chords' own Bass tones (%d)", am ? am->tone : -1);
}

/* A start held from before a key change: each lane's view of it plays in the scratch's key (nothing else changed, so as now) */
static void test_view_in_todays_key(void) {
    int same = 1;
    edited(&a);
    start = *playing(&a);
    seq_set_key(&a, 2);   /* D */
    seq_scratch_from(&b, &a);
    seq_lane_view(&b, LANE_CHORDS, &start, &start);
    for (int slot = 0; slot < playing(&a)->prog_len; slot++) {
        int x[THEORY_CHORD_MAX], y[THEORY_CHORD_MAX], nx = seq_slot_notes(&a, slot, slot, x), ny = seq_slot_notes(&b, slot, slot, y);
        same &= nx > 0 && nx == ny && !memcmp(x, y, sizeof x[0] * (size_t)nx);
    }
    CHECK(same, "the start's chords sound in D, as now's");
    same = 1;
    seq_lane_view(&b, LANE_BASS, &start, &start);
    for (int bar = 0; bar < 4; bar++) {
        unsigned char x[16], y[16];
        seq_tweak_notes(&a, bar, x), seq_tweak_notes(&b, bar, y);
        same &= !memcmp(x, y, 16);
    }
    CHECK(same, "and its Bass");
    same = 1;
    seq_lane_view(&b, LANE_MEL_UP, &start, &start);
    for (int bar = 0; bar < 4; bar++) {
        mjam_note_t x[16], y[16];
        seq_mel_line(&a, bar, x), seq_mel_line(&b, bar, y);
        same &= !memcmp(x, y, sizeof x);
    }
    CHECK(same, "and its Melody");
}

/* A start taken at 4 bars, then the loop grown to 8: the start's lane over now's chords repeats its 4 bars, edits and all, where seq_set_bars put
 * them (bars 5..8 of the start were never filled) */
static void test_view_of_a_start_before_a_grow(void) {
    unsigned char x[16], y[16], e2[16], e6[16];
    mjam_note_t lx[16], ly[16], m2[16], m6[16];
    int same = 1, bstep, mstep, painted;
    fresh(&a);
    seq_tweak_notes(&a, 1, x);
    bstep = first_sounding(x);
    for (painted = 0; painted < 16 && x[painted]; painted++) {}   /* a silent step of bar 2 */
    seq_mel_notes(&a, 1, x);
    mstep = first_sounding(x);
    CHECK(bstep >= 0 && mstep >= 0 && painted < 16, "bar 2 has a Bass hit, a silent Bass step (%d) and a Melody note", painted + 1);
    if (bstep < 0 || mstep < 0 || painted == 16) return;
    CHECK(seq_tweak_paint(&a, LANE_BASS, 1, painted, RC_HIT) == 1, "a Bass hit painted on bar 2");
    seq_mel_note(&a, 1, mstep, TONE_5, 1);   /* and a Melody pick an octave up */
    start = *playing(&a);
    CHECK(start.filled == 4, "the start never filled bars 5..8 (filled %d)", start.filled);
    seq_set_bars(&a, 8);

    seq_scratch_from(&b, &a);
    seq_lane_view(&b, LANE_BASS, &start, playing(&a));
    for (int bar = 0; bar < 8; bar++) {
        seq_tweak_notes(&a, bar, x), seq_tweak_notes(&b, bar, y);
        same &= !memcmp(x, y, 16);
    }
    CHECK(same, "the start's Bass over the grown loop plays as now on all 8 bars");
    seq_tweak_notes(&b, 1, e2), seq_tweak_notes(&b, 5, e6);
    CHECK(e2[painted] && e6[painted], "the painted hit plays on bar 2 and again on bar 6");

    same = 1;
    seq_lane_view(&b, LANE_MEL_UP, &start, playing(&a));
    for (int bar = 0; bar < 8; bar++) {
        seq_mel_line(&a, bar, lx), seq_mel_line(&b, bar, ly);
        same &= !memcmp(lx, ly, sizeof lx);
    }
    CHECK(same, "the start's Melody over the grown loop plays as now on all 8 bars");
    seq_mel_line(&b, 1, m2), seq_mel_line(&b, 5, m6);
    CHECK(m2[mstep].note && m2[mstep].note == m6[mstep].note, "the pick plays on bar 2 and again on bar 6 (%d, %d)", m2[mstep].note, m6[mstep].note);
}

static int emitted;
static void count_emit(void *ctx, int status, int note, int vel, int frame) { (void)ctx, (void)status, (void)note, (void)vel, (void)frame, emitted++; }

/* About two bars of playback at 120 bpm, 512-frame blocks */
static void play(seq_t *s) {
    for (int i = 0; i < 700; i++) seq_block(s, i * 512.0 / 44100.0 * 2.0, 120.0, 1, 512);
}

static void test_scratch_never_emits(void) {
    edited(&a);
    start = *playing(&a);
    a.emit = count_emit;
    play(&a);
    CHECK(emitted > 0, "the seq_t itself plays (%d events)", emitted);
    emitted = 0;
    seq_scratch_from(&b, &a);
    for (int k = 0; k < 3; k++) seq_lane_view(&b, LANES[k], &start, playing(&a));
    play(&b);
    CHECK(emitted == 0, "a scratch copy, its views and its playback send nothing (%d events)", emitted);
}

/* ---- Go back (seq_lane_back) */

/* 1 when lane `lane`'s steps of x and y play the same on bars [0, bars) */
static int steps_same(const seq_t *x, const seq_t *y, int lane, int bars) {
    const seq_step_t *u = x->part[x->cur_part].lane[lane].steps, *v = y->part[y->cur_part].lane[lane].steps;
    for (int t = 0; t < bars * SEQ_STEPS; t++)
        if (u[t].on != v[t].on || (u[t].on && (u[t].tone != v[t].tone || u[t].oct != v[t].oct || u[t].vel != v[t].vel || u[t].len != v[t].len ||
                                               u[t].abs != v[t].abs)))
            return 0;
    return 1;
}

static void test_back_bass(void) {
    unsigned char note[16];
    unsigned h0, h1;
    int silent, steps, words = seq_bass_row_words(BL_MOVES);
    fresh(&a);
    start = *playing(&a);
    h0 = seq_tweak_hash(&a, LANE_BASS);
    seq_tweak_notes(&a, 0, note);
    for (silent = 0; silent < 16 && note[silent]; silent++) {}
    CHECK(silent < 16, "bar 1 has a silent Bass step (%d)", silent + 1);
    if (silent == 16) return;
    seq_bass_tone_set(&a, 1, TONE_3);   /* pick the 3rd on chord 2 */
    CHECK(seq_tweak_paint(&a, LANE_BASS, 0, silent, RC_HIT) == 1, "a step painted");
    seq_bass_row(&a, BL_MOVES, (seq_bass_row_get(&a, BL_MOVES) + 1) % words);   /* a Line row */
    h1 = seq_tweak_hash(&a, LANE_BASS);
    CHECK(h1 != h0, "the Bass changed");
    steps = undo_steps(&a);

    seq_lane_back(&a, LANE_BASS, &start, 0);
    CHECK(seq_tweak_hash(&a, LANE_BASS) == h0, "Go back: the Bass reads as at the start (%#x, %#x)", seq_tweak_hash(&a, LANE_BASS), h0);
    CHECK(playing(&a)->fs[1].tone == 0 && playing(&a)->bass.set.k[BK_MOV] == start.bass.set.k[BK_MOV], "its tone and its row are the start's");
    CHECK(undo_steps(&a) == steps + 1, "one undo step (%d -> %d)", steps, undo_steps(&a));
    seq_undo(&a);
    CHECK(seq_tweak_hash(&a, LANE_BASS) == h1, "one Undo: the edits return (%#x, %#x)", seq_tweak_hash(&a, LANE_BASS), h1);
    CHECK(undo_steps(&a) == steps, "and nothing else was undone");
}

static void test_back_melody(void) {
    unsigned char note[16];
    unsigned h0, h1;
    int ms, steps;
    fresh(&a);
    start = *playing(&a);
    h0 = seq_tweak_hash(&a, LANE_MEL_UP);
    seq_mel_row(&a, ML_ANSWER, (seq_mel_row_get(&a, ML_ANSWER) + 1) % seq_mel_row_words(ML_ANSWER));   /* an answer */
    seq_mel_row(&a, ML_EARLY, (seq_mel_row_get(&a, ML_EARLY) + 1) % seq_mel_row_words(ML_EARLY));      /* Early */
    seq_mel_notes(&a, 1, note);
    ms = first_sounding(note);
    CHECK(ms >= 0, "bar 2 has a Melody note (step %d)", ms + 1);
    if (ms < 0) return;
    seq_mel_note(&a, 1, ms, TONE_5, 1);   /* a step pick */
    CHECK(playing(&a)->mjam.fix[16 + ms] != 0, "the pick is set");
    h1 = seq_tweak_hash(&a, LANE_MEL_UP);
    CHECK(h1 != h0, "the Melody changed");
    steps = undo_steps(&a);

    seq_lane_back(&a, LANE_MEL_UP, &start, 0);
    CHECK(seq_tweak_hash(&a, LANE_MEL_UP) == h0, "Go back: the Melody reads as at the start (%#x, %#x)", seq_tweak_hash(&a, LANE_MEL_UP), h0);
    CHECK(!playing(&a)->mjam.fix[16 + ms] && playing(&a)->mjam.early == start.mjam.early, "the pick and Early are the start's");
    CHECK(undo_steps(&a) == steps + 1, "one undo step (%d -> %d)", steps, undo_steps(&a));
    seq_undo(&a);
    CHECK(seq_tweak_hash(&a, LANE_MEL_UP) == h1, "one Undo: the edits return (%#x, %#x)", seq_tweak_hash(&a, LANE_MEL_UP), h1);
    CHECK(undo_steps(&a) == steps, "and nothing else was undone");
}

/* A preset longer than 4 bars, -1 none */
static int long_preset(void) {
    for (int i = 0; i < THEORY_NPRESETS; i++)
        if (THEORY_PRESETS[i].len > 4) return i;
    return -1;
}

static unsigned short chords_start[SEQ_LANE_STEPS], preset_c[SEQ_LANE_STEPS];
static seq_feel_chord_t fc0[SEQ_LANE_STEPS], fc[SEQ_LANE_STEPS];

static void test_back_chords_brings_picks_back(void) {
    enum { BASS_NOTE = 39, MEL_NOTE = 73, BAR = 3 };   /* bar 4: F, then the preset's Em (bars 1-3 keep their chords) */
    unsigned char note[16];
    seq_part_t *p;
    unsigned mask;
    int bs, ms, nfc, pre = long_preset(), steps, same = 1, nv0[4], v0[4][THEORY_CHORD_MAX], vstyle;
    CHECK(pre >= 0, "a preset longer than 4 bars (%d)", pre);
    if (pre < 0) return;
    fresh(&a);
    p = playing(&a);
    seq_bass_tone_set(&a, BAR, TONE_5);     /* a Bass tone pick on chord 4 (the bar the preset changes) */
    seq_mel_tone_set(&a, BAR, TONE_3, 0);   /* a Melody pick on it */
    seq_tweak_notes(&a, BAR, note);
    bs = first_sounding(note);
    CHECK(bs >= 0, "bar 4 sounds on the Bass (step %d)", bs + 1);
    if (bs < 0) return;
    seq_mel_notes(&a, BAR, note);
    ms = first_sounding(note);
    CHECK(ms >= 0, "and on the Melody (step %d)", ms + 1);
    if (ms < 0) return;
    p->bass.fix[16 * BAR + bs] = BASS_NOTE;   /* an absolute fix (the Force's) */
    p->mjam.fix[16 * BAR + ms] = MEL_NOTE;    /* and the Melody's, its bar kept */
    p->mjam.nlock |= 1u << BAR;
    p->mjam.frozen[BAR] = p->mjam.set;
    start = *p;
    seq_lane_back(&a, LANE_BASS, &start, 0);     /* rendered (and undo steps, which nothing below reads) */
    seq_lane_back(&a, LANE_MEL_UP, &start, 0);
    CHECK(plays(&a, 0, BAR, BASS_NOTE) && plays(&a, 1, BAR, MEL_NOTE), "the fixes play");
    start = *p;
    nfc = seq_feel_chords(&a, fc0, SEQ_LANE_STEPS);
    for (int k = 0; k < 4; k++) nv0[k] = seq_slot_notes(&a, k, k, v0[k]);
    seq_chord_steps(&a, p, chords_start);

    seq_chord_steps(&a, p, before);
    seq_feel_preset(&a, pre);               /* Start from: every chord changes, the loop grows */
    seq_chord_steps(&a, p, after);
    mask = seq_chords_changed(before, after, p->bars);
    seq_follow_chords(&a, mask, 1);         /* the cascade, as the host runs it */
    CHECK(p->bars == 8 && !p->fs[BAR].tone && !p->fs[BAR].mtone, "the preset: 8 bars, the picks wiped (bars %d)", p->bars);
    CHECK(!p->bass.fix[16 * BAR + bs] && !plays(&a, 0, BAR, BASS_NOTE), "and the cascade dropped the fix");
    CHECK(!p->mjam.fix[16 * BAR + ms] && !(p->mjam.nlock >> BAR & 1), "and the Melody's, and unkept its bar");
    seq_bass_tone_set(&a, 0, TONE_3);   /* a Bass tone picked on the preset's first chord: now's, where the start has none */
    vstyle = (start.vstyle + 1) % VOICING_NSTYLES;
    seq_set_voicing_style(&a, vstyle);   /* and another voicing */
    seq_set_mute(&a, LANE_CHORDS, 1);
    seq_chord_steps(&a, p, preset_c);
    a.undo_n = 0;   /* room in the ring: Go back's step is three snapshots */
    steps = undo_steps(&a);

    seq_lane_back(&a, LANE_CHORDS, &start, 0);
    seq_chord_steps(&a, p, after);
    CHECK(p->bars == 4, "Go back on Chords: the start's 4 bars (%d)", p->bars);
    CHECK(!memcmp(after, chords_start, sizeof after), "and its chords");
    CHECK(seq_feel_chords(&a, fc, SEQ_LANE_STEPS) == nfc && (fc[0].tone = 0, !memcmp(fc, fc0, sizeof fc[0] * (size_t)nfc)),
          "the Chords read as at the start (but the tone picked since)");
    for (int k = 0; k < 4; k++) {
        int v[THEORY_CHORD_MAX], nv = seq_slot_notes(&a, k, k, v);
        same &= nv == nv0[k] && !memcmp(v, v0[k], sizeof v[0] * (size_t)nv);
    }
    CHECK(same, "and sound as then");
    CHECK(p->fs[BAR].tone == TONE_5 && p->fs[BAR].mtone == TONE_3 && p->fs[BAR].moct == 0, "the Bass's and the Melody's picks came back");
    CHECK(p->fs[0].tone == TONE_3, "and the tone picked since stays, on the chord at its time (%d)", p->fs[0].tone);
    CHECK(p->bass.fix[16 * BAR + bs] == BASS_NOTE && plays(&a, 0, BAR, BASS_NOTE), "and the absolute fix plays again");
    CHECK(p->mjam.fix[16 * BAR + ms] == MEL_NOTE && plays(&a, 1, BAR, MEL_NOTE) && (p->mjam.nlock >> BAR & 1), "and the Melody's, its bar kept");
    CHECK(seq_voicing_style(&a) == start.vstyle, "the start's voicing (%d)", seq_voicing_style(&a));
    CHECK(p->lane[LANE_CHORDS].mute, "the Chords' mute is left alone");
    CHECK(undo_steps(&a) == steps + 1, "one undo step (%d -> %d)", steps, undo_steps(&a));

    seq_undo(&a);
    seq_chord_steps(&a, p, before);
    CHECK(p->bars == 8, "one Undo: the preset's 8 bars (%d)", p->bars);
    CHECK(!memcmp(before, preset_c, sizeof before) && !p->fs[BAR].tone && !p->bass.fix[16 * BAR + bs], "its chords exactly, no picks, no fix");
    CHECK(!p->mjam.fix[16 * BAR + ms] && !(p->mjam.nlock >> BAR & 1), "nor the Melody's");
    CHECK(seq_voicing_style(&a) == vstyle && p->fs[0].tone == TONE_3, "the voicing and the tone picked before Go back (%d)", seq_voicing_style(&a));
    CHECK(undo_steps(&a) == steps, "and nothing else was undone");
}

/* Ruling J: Go back on Chords brings the Bass's edits back only in the bars whose chord changed; an edit removed since on a bar that kept its chord
 * stays gone */
static void test_back_chords_only_changed_bars(void) {
    enum { BAR1_NOTE = 37, BAR2_NOTE = 39 };
    unsigned char note[16];
    seq_part_t *p;
    int b1, bs;
    fresh(&a);
    p = playing(&a);
    seq_tweak_notes(&a, 0, note);
    b1 = first_sounding(note);
    seq_tweak_notes(&a, 1, note);
    bs = first_sounding(note);
    CHECK(b1 >= 0 && bs >= 0, "bars 1 and 2 sound on the Bass");
    if (b1 < 0 || bs < 0) return;
    seq_bass_tone_set(&a, 0, TONE_5);   /* a tone on chord 1 (bar 1) */
    p->bass.fix[b1] = BAR1_NOTE;        /* absolute fixes in bar 1 and bar 2 */
    p->bass.fix[16 + bs] = BAR2_NOTE;
    p->mjam.nlock |= 1u << 0;           /* and the Melody's bar 1 kept */
    p->mjam.frozen[0] = p->mjam.set;
    start = *p;                         /* the Chords start */

    seq_tweak_follow(&a, 0);             /* the Bass page drops bar 1's fix */
    seq_bass_tone_set(&a, 0, TONE_R);    /* and chord 1's tone */
    p->mjam.nlock &= (unsigned short)~1u;   /* and the Melody's bar 1 is unkept */
    seq_chord_steps(&a, p, before);
    seq_feel_pick(&a, 1, 2, 0, COL_TRIAD);   /* bar 2's chord changes; the cascade drops its fix */
    seq_chord_steps(&a, p, after);
    seq_follow_chords(&a, seq_chords_changed(before, after, p->bars), 1);
    CHECK(!p->bass.fix[b1] && !p->fs[0].tone && !p->bass.fix[16 + bs], "no fix, no tone left");

    seq_lane_back(&a, LANE_CHORDS, &start, 0);
    seq_chord_steps(&a, p, after);
    seq_chord_steps(&a, &start, before);
    CHECK(!memcmp(after, before, sizeof after), "Go back on Chords: the start's chords");
    CHECK(!p->bass.fix[b1] && !plays(&a, 0, 0, BAR1_NOTE), "bar 1 kept its chord: its fix, removed since, stays gone");
    CHECK(!p->fs[0].tone, "and so does chord 1's tone (%d)", p->fs[0].tone);
    CHECK(!(p->mjam.nlock & 1), "and the Melody's bar 1 stays unkept");
    CHECK(p->bass.fix[16 + bs] == BAR2_NOTE && plays(&a, 0, 1, BAR2_NOTE), "bar 2's chord changed: its fix comes back");
}

/* Ruling AG: after a Chords preset (it wipes every pick) Go back on Chords with all_picks brings the start's picks back on every chord where now
 * has none, the bars whose chord stayed too; absolute fixes still only in the bars whose chord changed */
static void test_back_chords_after_preset_all_picks(void) {
    enum { BAR1_NOTE = 37 };
    unsigned char note[16];
    seq_part_t *p;
    int pre = -1, b1;
    for (int i = 0; i < THEORY_NPRESETS && pre < 0; i++)
        if (THEORY_PRESETS[i].deg[0] == 0) pre = i;   /* a preset that keeps chord 1, C */
    CHECK(pre >= 0, "a preset starting on the tonic (%d)", pre);
    if (pre < 0) return;
    for (int all = 0; all < 2; all++) {
        fresh(&a);
        p = playing(&a);
        seq_tweak_notes(&a, 0, note);
        b1 = first_sounding(note);
        CHECK(b1 >= 0, "bar 1 sounds on the Bass");
        if (b1 < 0) return;
        seq_bass_tone_set(&a, 0, TONE_5);       /* a Bass pick on chord 1 */
        seq_mel_tone_set(&a, 0, TONE_3, 0);     /* and a Melody pick */
        p->bass.fix[b1] = BAR1_NOTE;            /* and an absolute fix in bar 1 */
        start = *p;                             /* the Chords start */
        seq_tweak_follow(&a, 0);                /* the Bass page drops the fix by hand */
        seq_chord_steps(&a, p, before);
        seq_feel_preset(&a, pre);               /* Start from */
        seq_chord_steps(&a, p, after);
        seq_follow_chords(&a, seq_chords_changed(before, after, p->bars), 1);
        CHECK(!(seq_chords_changed(before, after, 4) & 1), "bar 1 keeps its chord");
        CHECK(!p->fs[0].tone && !p->fs[0].mtone, "the preset wiped the picks");
        a.undo_n = 0;
        seq_lane_back(&a, LANE_CHORDS, &start, all);
        if (all) {
            CHECK(p->fs[0].tone == TONE_5 && p->fs[0].mtone == TONE_3 && p->fs[0].moct == 0,
                  "all_picks: chord 1's Bass and Melody picks come back though its chord stayed (%d %d)", p->fs[0].tone, p->fs[0].mtone);
            CHECK(!p->bass.fix[b1] && !plays(&a, 0, 0, BAR1_NOTE), "but the fix removed by hand in bar 1 stays gone");
        } else
            CHECK(!p->fs[0].tone && !p->fs[0].mtone, "all_picks 0: only the bars whose chord changed (Ruling J)");
    }
}

static void test_back_bass_after_chord_change(void) {
    enum { BASS_NOTE = 39, BAR1_NOTE = 37, MEL_NOTE = 73 };
    unsigned char note[16];
    seq_part_t *p;
    int bs, b1, ms;
    fresh(&a);
    p = playing(&a);
    seq_tweak_notes(&a, 0, note);
    b1 = first_sounding(note);
    seq_tweak_notes(&a, 1, note);
    bs = first_sounding(note);
    CHECK(bs >= 0 && b1 >= 0, "bars 1 and 2 sound on the Bass");
    if (bs < 0 || b1 < 0) return;
    p->bass.fix[b1] = BAR1_NOTE;
    p->bass.fix[16 + bs] = BASS_NOTE;   /* the start's absolute fixes in bars 1 and 2 */
    start = *p;

    seq_tweak_follow(&a, 0);   /* the Bass page drops bar 1's */
    seq_chord_steps(&a, p, before);
    seq_feel_pick(&a, 1, 2, 0, COL_TRIAD);   /* bar 2's chord changes, and the cascade drops its fix */
    seq_chord_steps(&a, p, after);
    seq_follow_chords(&a, seq_chords_changed(before, after, p->bars), 1);
    CHECK(!p->bass.fix[b1] && !p->bass.fix[16 + bs], "neither fix is left");
    seq_mel_notes(&a, 1, note);
    ms = first_sounding(note);
    CHECK(ms >= 0, "bar 2 sounds on the Melody (step %d)", ms + 1);
    if (ms < 0) return;
    p->mjam.fix[16 + ms] = MEL_NOTE;   /* the Melody's own absolute fix in bar 2, after the chord change */
    p->mjam.nlock |= 1u << 1;

    seq_lane_back(&a, LANE_BASS, &start, 0);
    CHECK(p->bass.fix[b1] == BAR1_NOTE && plays(&a, 0, 0, BAR1_NOTE), "Go back on Bass: bar 1's fix (its chord is the start's) is back");
    CHECK(!p->bass.fix[16 + bs] && !plays(&a, 0, 1, BASS_NOTE), "bar 2's chord changed since: its fix stays gone");
    CHECK(p->mjam.fix[16 + ms] == MEL_NOTE && (p->mjam.nlock >> 1 & 1), "and the Melody's fix and kept bar there are left alone");
}

/* LOCK WITH (lw) is the lane's: Go back sets it, the other lane turns OWN when this one follows it, and both lanes play their LOCK WITH. The
 * lane's mute is left alone */
static void test_back_keeps_mute_and_lockwith(void) {
    seq_part_t *p;
    fresh(&a);
    p = playing(&a);
    seq_set_lockwith(&a, LANE_BASS, LW_WITH);   /* the Bass follows the Chords */
    start = *p;
    seq_scratch_from(&b, &a);                   /* the start as it plays */
    seq_set_lockwith(&a, LANE_CHORDS, LW_WITH); /* the Chords follow the Bass: the Bass turns OWN */
    seq_set_mute(&a, LANE_BASS, 1);
    CHECK(p->lw[DL_BASS] == LW_OWN && p->lw[DL_CHORDS] == LW_WITH, "now the Chords follow the Bass");
    CHECK(!steps_same(&a, &b, LANE_CHORDS, 4), "and play other hits than at the start");

    seq_lane_back(&a, LANE_BASS, &start, 0);
    CHECK(p->lw[DL_BASS] == LW_WITH && p->lw[DL_CHORDS] == LW_OWN, "Go back on Bass: it follows the Chords again, the Chords are OWN");
    CHECK(steps_same(&a, &b, LANE_CHORDS, 4) && steps_same(&a, &b, LANE_BASS, 4), "both lanes play as at the start");
    CHECK(p->lane[LANE_BASS].mute == 1, "the Bass's mute is left alone");
    seq_undo(&a);
    CHECK(p->lw[DL_BASS] == LW_OWN && p->lw[DL_CHORDS] == LW_WITH, "one Undo: the Chords follow the Bass again");
}

static void test_back_cancels_queued_blocks(void) {
    int c = 0, l = 0;
    fresh(&a);
    start = *playing(&a);
    play(&a);
    if (!seq_jam_playing(&a, LANE_BASS, &c, &l)) c = 0, l = 0;   /* no block yet */
    CHECK(seq_jam_queue(&a, LANE_BASS, (c + 1) % JAM_COLS, l) == 1, "a Bass block queued while playing");
    seq_lane_back(&a, LANE_BASS, &start, 0);
    CHECK(!seq_jam_queued(&a, LANE_BASS, NULL, NULL), "Go back on Bass cancels it");
    CHECK(playing(&a)->djam[DL_BASS].c == start.djam[DL_BASS].c && playing(&a)->djam[DL_BASS].l == start.djam[DL_BASS].l, "and its block is the start's");
    seq_mel_queue(&a, (start.mjam.set.c + 1) % MJ_COLS, start.mjam.set.l);
    CHECK(seq_mel_queued(&a, NULL, NULL, NULL), "a Melody block queued");
    seq_lane_back(&a, LANE_MEL_UP, &start, 0);
    CHECK(!seq_mel_queued(&a, NULL, NULL, NULL), "Go back on Melody cancels it");
}

static void test_back_bad_args(void) {
    int n;
    fresh(&a);
    start = *playing(&a);
    seq_bass_tone_set(&a, 1, TONE_3);
    n = a.undo_n;
    seq_lane_back(&a, LANE_MEL_DOWN, &start, 0);
    seq_lane_back(&a, -1, &start, 0);
    seq_lane_back(&a, LANE_BASS, NULL, 0);
    CHECK(a.undo_n == n && playing(&a)->fs[1].tone == TONE_3, "a bad lane or no start: nothing");
}

/* ---- Start over (seq_lane_start_over) */

static seq_t r;   /* the reference: a fresh part with the same block */

/* 1 when the Bass's or Melody's (lane) every row reads the same in x and y */
static int rows_same(const seq_t *x, const seq_t *y, int lane) {
    int tw[] = {TW_BUSY, TW_OFFBEAT, TW_SOFT, TW_LOOSE, TW_LEAN, TW_FOLLOW, TW_EARLY, TW_LANDS, TW_ENDING};
    if (lane == LANE_MEL_UP) {
        for (int w = 0; w < ML_COUNT; w++)
            if (seq_mel_row_get(x, w) != seq_mel_row_get(y, w)) return 0;
        return 1;
    }
    for (int w = 0; w < BL_COUNT; w++)
        if (seq_bass_row_get(x, w) != seq_bass_row_get(y, w)) return 0;
    for (int k = 0; k < (int)(sizeof tw / sizeof tw[0]); k++)
        if (seq_tweak_word(x, lane, tw[k]) != seq_tweak_word(y, lane, tw[k])) return 0;
    return 1;
}

static void test_start_over_bass(void) {
    enum { BASS_NOTE = 39, BC = 2, BL = 3 };
    unsigned char note[16];
    seq_part_t *p;
    unsigned h1;
    int bs, steps, fixes = 0;
    fresh(&r);
    seq_jam_apply_now(&r, LANE_BASS, BC, BL);
    fresh(&a);
    p = playing(&a);
    seq_jam_apply_now(&a, LANE_BASS, BC, BL);
    CHECK(rows_same(&a, &r, LANE_BASS) && seq_tweak_hash(&a, LANE_BASS) == seq_tweak_hash(&r, LANE_BASS), "the Bass plays block %d.%d", BC, BL);
    seq_tweak_notes(&a, 1, note);
    bs = first_sounding(note);
    CHECK(bs >= 0, "bar 2 sounds on the Bass (step %d)", bs + 1);
    if (bs < 0) return;
    p->bass.fix[16 + bs] = BASS_NOTE;                  /* an absolute fix */
    p->bass.nlock |= 1u << 2, p->bass.frozen[2] = p->bass.set;   /* a kept bar */
    seq_bass_tone_set(&a, 3, TONE_5);                  /* a tone pick */
    seq_tweak_set(&a, LANE_BASS, TW_BUSY, 4);          /* Busy */
    seq_tweak_set(&a, LANE_BASS, TW_LEAN, 2);          /* Lean */
    seq_tweak_set(&a, LANE_BASS, TW_EARLY, 1);         /* Early */
    seq_tweak_set(&a, LANE_BASS, TW_SOFT, 2);          /* a RHYTHM knob */
    seq_bass_row(&a, BL_MOVES, (seq_bass_row_get(&a, BL_MOVES) + 1) % seq_bass_row_words(BL_MOVES));
    CHECK(seq_bass_edited(&a) || !rows_same(&a, &r, LANE_BASS), "the Bass is edited");
    h1 = seq_tweak_hash(&a, LANE_BASS);
    steps = undo_steps(&a);

    seq_lane_start_over(&a, LANE_BASS, 0);
    for (int i = 0; i < SEQ_LANE_STEPS; i++) fixes += p->bass.fix[i] != 0;
    CHECK(!seq_bass_edited(&a) && !fixes && !p->bass.nlock, "Start over: no edit, no fix (%d), no kept bar (%#x)", fixes, p->bass.nlock);
    CHECK(!p->fs[3].tone, "no tone pick (%d)", p->fs[3].tone);
    CHECK(rows_same(&a, &r, LANE_BASS), "every row at the block's words");
    CHECK(seq_tweak_hash(&a, LANE_BASS) == seq_tweak_hash(&r, LANE_BASS), "and it plays as the block does in a fresh part");
    CHECK(playing(&a)->djam[DL_BASS].c == BC && playing(&a)->djam[DL_BASS].l == BL, "the block stays");
    CHECK(undo_steps(&a) == steps + 1, "one undo step (%d -> %d)", steps, undo_steps(&a));
    seq_undo(&a);
    CHECK(seq_tweak_hash(&a, LANE_BASS) == h1 && p->bass.fix[16 + bs] == BASS_NOTE && p->fs[3].tone == TONE_5 && (p->bass.nlock >> 2 & 1),
          "one Undo: every edit is back");
    CHECK(seq_tweak_word(&a, LANE_BASS, TW_BUSY) == 4 && seq_tweak_word(&a, LANE_BASS, TW_LEAN) == 2 && seq_tweak_word(&a, LANE_BASS, TW_EARLY) == 1,
          "Busy, Lean and Early too");
    CHECK(undo_steps(&a) == steps, "and nothing else was undone");
}

static void test_start_over_melody(void) {
    unsigned char note[16];
    seq_part_t *p;
    unsigned h1;
    int ms, steps, c, l, fixes = 0, same = 1;
    fresh(&r);
    fresh(&a);
    p = playing(&a);
    c = p->mjam.set.c, l = p->mjam.set.l;
    seq_mel_row(&a, ML_ANSWER, MA_MIRROR);     /* an answer */
    seq_mel_row(&a, ML_EARLY, 2);              /* Early */
    seq_mel_row(&a, ML_LENGTH, 4);             /* Length */
    seq_mel_row(&a, ML_LANDS, 1);              /* Lands */
    seq_mel_tone_set(&a, 2, TONE_3, 1);        /* a chord's pick */
    seq_mel_notes(&a, 1, note);
    ms = first_sounding(note);
    CHECK(ms >= 0, "bar 2 sounds on the Melody (step %d)", ms + 1);
    if (ms < 0) return;
    seq_mel_note(&a, 1, ms, TONE_5, 1);        /* a step pick */
    seq_tweak_keep(&a, LANE_MEL_UP, 3);         /* a kept bar */
    CHECK(p->mjam.nlock >> 3 & 1, "bar 4 kept");
    h1 = seq_tweak_hash(&a, LANE_MEL_UP);
    CHECK(h1 != seq_tweak_hash(&r, LANE_MEL_UP) && !rows_same(&a, &r, LANE_MEL_UP), "the Melody is edited");
    steps = undo_steps(&a);

    seq_lane_start_over(&a, LANE_MEL_UP, 0);
    for (int i = 0; i < SEQ_LANE_STEPS; i++) fixes += p->mjam.fix[i] != 0;
    CHECK(p->mjam.set.c == c && p->mjam.set.l == l, "Start over keeps the block (%d.%d)", p->mjam.set.c, p->mjam.set.l);
    CHECK(!fixes && !p->mjam.nlock && !p->fs[2].mtone && !p->fs[2].moct, "no pick, no fix, no kept bar");
    CHECK(rows_same(&a, &r, LANE_MEL_UP), "every row at the defaults (answer %d, Early %d)", seq_mel_row_get(&a, ML_ANSWER), seq_mel_row_get(&a, ML_EARLY));
    CHECK(p->lane[LANE_MEL_UP].gen.complexity == playing(&r)->lane[LANE_MEL_UP].gen.complexity, "the block's complexity");
    for (int bar = 0; bar < 4; bar++) {
        mjam_note_t x[16], y[16];
        seq_mel_line(&a, bar, x), seq_mel_line(&r, bar, y);
        same &= !memcmp(x, y, sizeof x);
    }
    CHECK(same && seq_tweak_hash(&a, LANE_MEL_UP) == seq_tweak_hash(&r, LANE_MEL_UP), "it plays as the block does in a fresh part");
    CHECK(undo_steps(&a) == steps + 1, "one undo step (%d -> %d)", steps, undo_steps(&a));
    seq_undo(&a);
    CHECK(seq_tweak_hash(&a, LANE_MEL_UP) == h1 && p->mjam.fix[16 + ms] && p->fs[2].mtone == TONE_3 && (p->mjam.nlock >> 3 & 1),
          "one Undo: every edit is back");
    CHECK(seq_mel_row_get(&a, ML_ANSWER) == MA_MIRROR && seq_mel_row_get(&a, ML_EARLY) == 2 && seq_mel_row_get(&a, ML_LENGTH) == 4, "the words too");
    CHECK(undo_steps(&a) == steps, "and nothing else was undone");

    fresh(&a);   /* a Melody that isn't JAM (a Force song's) */
    seq_set_style(&a, LANE_MEL_UP, GEN_MEL_HOOK);
    CHECK(p->lane[LANE_MEL_UP].gen.style == GEN_MEL_HOOK, "the Melody plays a recipe");
    steps = undo_steps(&a);
    seq_lane_start_over(&a, LANE_MEL_UP, 0);
    CHECK(p->lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && rows_same(&a, &r, LANE_MEL_UP), "Start over makes it JAM, at the defaults");
    CHECK(undo_steps(&a) == steps + 1, "one undo step (%d -> %d)", steps, undo_steps(&a));
    seq_undo(&a);
    CHECK(p->lane[LANE_MEL_UP].gen.style == GEN_MEL_HOOK, "one Undo: the recipe again");
}

static unsigned short edited_c[SEQ_LANE_STEPS];

static void test_start_over_chords(void) {
    enum { CC = 1, CL = 2, BASS_NOTE = 39, PRE = 0 };   /* 1-6-3-7 Epic: bar 1 keeps C, bar 2 G -> Am */
    unsigned char note[16], x[16], y[16], codes[4][16];
    seq_part_t *p;
    int bs, steps, same = 1, painted;
    fresh(&r);
    seq_jam_apply_now(&r, LANE_CHORDS, CC, CL);
    seq_feel_preset(&r, PRE);
    fresh(&a);
    p = playing(&a);
    seq_jam_apply_now(&a, LANE_CHORDS, CC, CL);
    seq_tweak_notes(&a, 1, note);
    bs = first_sounding(note);
    CHECK(bs >= 0, "bar 2 sounds on the Bass (step %d)", bs + 1);
    if (bs < 0) return;
    seq_set_bars(&a, 16);
    seq_feel_pick(&a, 0, 5, 0, COL_TRIAD);              /* chord 1 becomes Am */
    seq_set_chord_len(&a, 8);                           /* two chords a bar */
    seq_bar_codes(&a, LANE_CHORDS, 2, x);
    for (painted = 0; painted < 16 && x[painted]; painted++) {}
    CHECK(painted < 16 && seq_tweak_paint(&a, LANE_CHORDS, 2, painted, RC_HIT) == 1, "a Chords hit painted on bar 3");
    seq_tweak_keep(&a, LANE_CHORDS, 1);                 /* bar 2 kept */
    seq_tweak_set(&a, LANE_CHORDS, TW_BUSY, 0);
    seq_tweak_set(&a, LANE_CHORDS, TW_EARLY, 2);
    seq_set_voicing_style(&a, (p->vstyle + 1) % VOICING_NSTYLES);
    p->bass.fix[16 + bs] = BASS_NOTE;                   /* the Bass's absolute fix in bar 2, whose chord the preset changes */
    seq_chord_steps(&a, p, edited_c);
    for (int b = 0; b < 4; b++) seq_bar_codes(&a, LANE_CHORDS, b, codes[b]);
    CHECK(seq_bar_locked(&a, LANE_CHORDS, 1) && p->bars == 16 && p->chord_len == 8, "16 bars, edited chords and rhythm");
    a.undo_n = 0;   /* room in the ring */
    steps = undo_steps(&a);

    seq_lane_start_over(&a, LANE_CHORDS, PRE);
    CHECK(p->bars == THEORY_PRESETS[PRE].len && p->chord_len == 16, "Start over: the preset's %d bars, a chord a bar (%d bars, chord_len %d)",
          THEORY_PRESETS[PRE].len, p->bars, p->chord_len);
    for (int k = 0; k < THEORY_PRESETS[PRE].len; k++) same &= p->prog[k] == THEORY_PRESETS[PRE].deg[k];
    CHECK(same, "its chords");
    seq_chord_steps(&a, p, after);
    seq_chord_steps(&r, playing(&r), before);
    CHECK(!memcmp(after, before, sizeof after), "as Start from plays them");
    same = 1;
    for (int b = 0; b < p->bars; b++) {
        seq_bar_codes(&a, LANE_CHORDS, b, x), seq_bar_codes(&r, LANE_CHORDS, b, y);
        same &= !memcmp(x, y, 16) && !seq_bar_locked(&a, LANE_CHORDS, b);
    }
    CHECK(same, "every bar the block's rhythm, none kept");
    CHECK(steps_same(&a, &r, LANE_CHORDS, p->bars), "the Chords play as the block over the preset in a fresh part");
    CHECK(seq_voicing_style(&a) == -1 && !seq_voicing_edited(&a) && p->vmode == playing(&r)->vmode && p->vspr == playing(&r)->vspr,
          "the block's voicing");
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_BUSY) == 2 && seq_tweak_word(&a, LANE_CHORDS, TW_EARLY) == 0 && seq_tweak_word(&a, LANE_CHORDS, TW_FOLLOW) == 0,
          "Busy As is, Early on time, Follow own");
    CHECK(!p->bass.fix[16 + bs] && !plays(&a, 0, 1, BASS_NOTE), "the Bass follows the new chords: bar 2's absolute fix went");
    CHECK(undo_steps(&a) == steps + 1, "one undo step (%d -> %d)", steps, undo_steps(&a));

    seq_undo(&a);
    seq_chord_steps(&a, p, after);
    CHECK(p->bars == 16 && p->chord_len == 8, "one Undo: 16 bars, two chords a bar (%d, %d)", p->bars, p->chord_len);
    CHECK(!memcmp(after, edited_c, sizeof after), "the edited chords");
    same = 1;
    for (int b = 0; b < 4; b++) seq_bar_codes(&a, LANE_CHORDS, b, x), same &= !memcmp(x, codes[b], 16);
    CHECK(same && seq_bar_locked(&a, LANE_CHORDS, 1), "the edited rhythm and the kept bar");
    CHECK(seq_tweak_word(&a, LANE_CHORDS, TW_BUSY) == 0 && seq_tweak_word(&a, LANE_CHORDS, TW_EARLY) == 2, "Busy and Early");
    CHECK(p->bass.fix[16 + bs] == BASS_NOTE, "and the Bass's fix");
    CHECK(undo_steps(&a) == steps, "and nothing else was undone");
}

static void test_start_over_bad_args(void) {
    int n;
    fresh(&a);
    seq_bass_tone_set(&a, 1, TONE_3);
    n = a.undo_n;
    seq_lane_start_over(&a, LANE_MEL_DOWN, 0);
    seq_lane_start_over(&a, -1, 0);
    seq_lane_start_over(&a, LANE_CHORDS, -1);
    seq_lane_start_over(&a, LANE_CHORDS, THEORY_NPRESETS);
    CHECK(a.undo_n == n && playing(&a)->fs[1].tone == TONE_3 && playing(&a)->bars == 4, "a bad lane or preset: nothing");
}

/* ---- the Melody's answers */

static void test_answer_leaving_repeats_is_all(void) {
    fresh(&a);
    CHECK(seq_mel_row_get(&a, ML_ANSWER) == MA_ECHO && seq_mel_row_get(&a, ML_STRAY) == 1, "a new song: Repeats, Last beat");
    seq_mel_row(&a, ML_ANSWER, MA_TAIL);
    CHECK(seq_mel_row_get(&a, ML_STRAY) == 3, "leaving Repeats: How much is All (%d)", seq_mel_row_get(&a, ML_STRAY));
    seq_undo(&a);
    CHECK(seq_mel_row_get(&a, ML_ANSWER) == MA_ECHO && seq_mel_row_get(&a, ML_STRAY) == 1, "one Undo: Repeats and Last beat");
    seq_mel_row(&a, ML_STRAY, 0);
    seq_mel_row(&a, ML_ANSWER, MA_SHIFT);
    CHECK(seq_mel_row_get(&a, ML_STRAY) == 3, "from Repeats at Last note: All too");
    seq_mel_row(&a, ML_STRAY, 2);
    seq_mel_row(&a, ML_ANSWER, MA_MIRROR);
    CHECK(seq_mel_row_get(&a, ML_STRAY) == 2, "one answer to another: How much stays");
}

/* 1 when bars [0, bars) of the Melody read the same in x and y */
static int lines_same(const seq_t *x, const seq_t *y, int bars) {
    for (int bar = 0; bar < bars; bar++) {
        mjam_note_t lx[16], ly[16];
        seq_mel_line(x, bar, lx), seq_mel_line(y, bar, ly);
        if (memcmp(lx, ly, sizeof lx)) return 0;
    }
    return 1;
}

static void test_view_answer_same_as_repeats(void) {
    unsigned h;
    fresh(&a);
    seq_mel_row(&a, ML_ANSWER, MA_TAIL);   /* Its ending */
    seq_mel_row(&a, ML_STRAY, 2);          /* at Last half */
    h = seq_tweak_hash(&a, LANE_MEL_UP);
    seq_scratch_from(&b, &a);
    seq_mel_view_answer(&b, MA_ECHO);
    CHECK(seq_mel_row_get(&b, ML_ANSWER) == MA_ECHO, "the scratch plays Repeats");
    CHECK(lines_same(&a, &b, 4), "Its ending at Last half sounds the same as Repeats");
    CHECK(seq_tweak_hash(&a, LANE_MEL_UP) == h && seq_mel_row_get(&a, ML_ANSWER) == MA_TAIL, "the part itself is untouched");
    seq_mel_view_answer(&b, MA_TAIL);
    CHECK(lines_same(&a, &b, 4), "and the scratch at Its ending again reads as the part");
    seq_mel_row(&a, ML_STRAY, 3);          /* at All */
    seq_scratch_from(&b, &a);
    seq_mel_view_answer(&b, MA_ECHO);
    CHECK(!lines_same(&a, &b, 4), "at All it differs from Repeats");

    fresh(&a);                              /* Round and round: an answer makes it One and one, as the page's word does */
    seq_scratch_from(&b, &a);
    seq_mel_view_answer(&b, MA_MIRROR);
    CHECK(seq_mel_row_get(&b, ML_FORM) == MF_1_1, "an answer on Round and round: One and one");
    seq_mel_row(&a, ML_ANSWER, MA_MIRROR);
    seq_mel_row(&a, ML_STRAY, 1);           /* the scratch's How much (the view leaves it) */
    CHECK(lines_same(&a, &b, 4), "and it reads as the page's word would play");
    seq_scratch_from(&b, &a);
    h = seq_tweak_hash(&b, LANE_MEL_UP);
    seq_mel_view_answer(&b, -1);
    seq_mel_view_answer(&b, MA_COUNT);
    CHECK(seq_tweak_hash(&b, LANE_MEL_UP) == h, "a bad answer: nothing");
}

int main(void) {
    test_chord_steps();
    test_chords_diff_and_part_at();
    test_follow_part_drops_only_absolute();
    test_follow_chords_one_undo();
    test_follow_chords_own_step();
    test_view_is_now_when_both_are_now();
    test_view_lane_over_old_chords();
    test_view_and_back_with_fewer_bars();
    test_view_carries_picks_by_time();
    test_view_in_todays_key();
    test_view_of_a_start_before_a_grow();
    test_scratch_never_emits();
    test_back_bass();
    test_back_melody();
    test_back_chords_brings_picks_back();
    test_back_chords_only_changed_bars();
    test_back_chords_after_preset_all_picks();
    test_back_bass_after_chord_change();
    test_back_keeps_mute_and_lockwith();
    test_back_cancels_queued_blocks();
    test_back_bad_args();
    test_start_over_bass();
    test_start_over_melody();
    test_start_over_chords();
    test_start_over_bad_args();
    test_answer_leaving_repeats_is_all();
    test_view_answer_same_as_repeats();
    printf(fails ? "test_try_keep: %d FAILED\n" : "test_try_keep: ok\n", fails);
    return fails != 0;
}
