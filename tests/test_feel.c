/* Offline test of the Feel page's engine (0.28): chromatic chords, the verbs and their undo, mutes, hand notes, recipes, the save, and
 * src/feel.c's music theory in every scale. Run by tests/run_tests.sh under ASan/UBSan. Exit 1 on failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "seq_core.h"
#include "feel.h"
#include "presets.h"
#include "rhythm.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c) != 0; printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

#define MAX_EV 8192
typedef struct { double beat; int status, note; } ev_t;
static ev_t evs[MAX_EV];
static int nev;
static double at_beat;
static void rec(void *ctx, int status, int note, int vel, int frame) {
    (void)ctx, (void)vel, (void)frame;
    if ((status & 0xF0) == 0x90 && vel && nev < MAX_EV) evs[nev++] = (ev_t){at_beat, status, note};
}

static void play(seq_t *s, double beats) {
    const int frames = 128;
    double step = frames * 120.0 / (60.0 * 44100.0);
    nev = 0;
    for (double ppq = 0; ppq < beats; ppq += step) { at_beat = ppq; seq_block(s, ppq, 120, 1, frames); }
    seq_block(s, -1, 120, 0, frames);
}

static int count_ch(int ch, double from, double to) {   /* MIDI channel 1.. */
    int n = 0;
    for (int i = 0; i < nev; i++) if ((evs[i].status & 15) == ch - 1 && evs[i].beat >= from && evs[i].beat < to) n++;
    return n;
}

static void fresh(seq_t *s) { seq_init(s, rec, NULL); seq_init_preset(s); seq_set_rate(s, 44100); }

static int chords(const seq_t *s, seq_feel_chord_t *c) { return seq_feel_chords(s, c, 64); }

static const char *name_of(const seq_t *s, int slot) {
    static char b[32];
    seq_slot_name(s, slot, b, sizeof b);
    return b;
}

static void test_chromatic(void) {
    int n[THEORY_CHORD_MAX], c;
    char nm[32], nu[16];
    int e7 = theory_chroma_src(4, CQ_DOM), ss = theory_slot_scale(SCALE_MAJOR, e7);
    c = theory_chord_notes(0, ss, 0, COL_7TH, n);
    CHECK(c == 4 && n[0] % 12 == 4 && n[1] % 12 == 8 && n[2] % 12 == 11 && n[3] % 12 == 2, "E7 in C: E G# B D");
    theory_chord_name_c(0, ss, 0, COL_7TH, nm, sizeof nm);
    CHECK(!strcmp(nm, "E7"), "named E7 (got %s)", nm);
    theory_numeral(0, SCALE_MAJOR, ss, 0, nu, sizeof nu);
    CHECK(!strcmp(nu, "III"), "numeral III (got %s)", nu);
    ss = theory_slot_scale(SCALE_MAJOR, theory_chroma_src(8, CQ_DIM));
    theory_chord_name_c(0, ss, 0, COL_7TH, nm, sizeof nm);
    c = theory_chord_notes(0, ss, 0, COL_7TH, n);
    CHECK(!strcmp(nm, "G#dim7") && n[1] - n[0] == 3 && n[2] - n[1] == 3 && n[3] - n[2] == 3, "G#dim7 stacks minor 3rds (got %s)", nm);
    ss = theory_slot_scale(SCALE_MAJOR, theory_chroma_src(10, CQ_DOM));
    theory_numeral(0, SCALE_MAJOR, ss, 0, nu, sizeof nu);
    CHECK(!strcmp(nu, "bVII"), "Bb7 numeral bVII (got %s)", nu);
    theory_chord_name_c(2, theory_slot_scale(SCALE_MINOR, theory_chroma_src(4, CQ_MINOR)), 0, COL_TRIAD, nm, sizeof nm);
    CHECK(!strcmp(nm, "F#m"), "root 4 in D: F#m, moves with the key (got %s)", nm);
    CHECK(theory_borrow_degree(SCALE_MAJOR, e7) == 0 && theory_slot_scale(SCALE_MINOR, 2) == theory_borrow_scale(SCALE_MINOR, 2), "borrows as before");
}

/* The bass follows a chromatic chord: its root sounds in that bar */
static void test_followers(seq_t *s) {
    int e7 = theory_chroma_src(4, CQ_DOM), found = 0;
    seq_feel_chord_t c[64];
    fresh(s);
    CHECK(chords(s, c) == 4 && c[0].steps == 16 && c[1].start == 16, "INIT: 4 chords of a bar");
    seq_feel_pick(s, 1, 0, e7, COL_7TH);
    CHECK(!strcmp(name_of(s, 1), "E7"), "slot 1 names E7 (got %s)", name_of(s, 1));
    play(s, 16);
    for (int i = 0; i < nev; i++)
        if ((evs[i].status & 15) == 1 && evs[i].beat >= 4 && evs[i].beat < 8 && evs[i].note % 12 == 4) found = 1;
    CHECK(found, "the bass plays E under E7");
    seq_set_key(s, 2);
    CHECK(!strcmp(name_of(s, 1), "F#7"), "a key change moves it (got %s)", name_of(s, 1));
    seq_set_scale(s, SCALE_MINOR);
    CHECK(!strcmp(name_of(s, 1), "F#7"), "a scale change keeps it (got %s)", name_of(s, 1));
}

static void test_verbs(seq_t *s) {
    seq_feel_chord_t c[64];
    int n;
    fresh(s);
    seq_feel_pick(s, 2, 1, 0, COL_7TH);
    n = chords(s, c);
    CHECK(n == 4 && c[2].deg == 1 && c[2].colour == COL_7TH && c[2].has_base && c[2].base_deg == 5, "pick: Dm7 over the written Am");
    seq_feel_pick(s, 2, 5, 0, COL_TRIAD);
    n = chords(s, c);
    CHECK(!c[2].has_base && c[2].deg == 5, "back to the written chord: no base");
    seq_undo(s);
    n = chords(s, c);
    CHECK(c[2].deg == 1 && c[2].has_base, "undo brings the pick back");
    seq_undo(s);
    n = chords(s, c);
    CHECK(c[2].deg == 5 && !c[2].has_base, "undo again: as written");

    seq_feel_add(s, 1, 3, 0, COL_TRIAD);
    n = chords(s, c);
    CHECK(n == 5 && c[1].steps == 8 && c[2].steps == 8 && c[2].deg == 3 && seq_chord_len(s) == 8, "add: the chord shares its bar, loop the same");
    CHECK(c[3].start == 32 && c[4].start == 48 && seq_bars(s) == 4, "later chords stay where they were");
    seq_feel_remove(s, 2);
    n = chords(s, c);
    CHECK(n == 4 && c[1].steps == 16 && seq_chord_len(s) == 16, "remove: the time goes back, chord length back to a bar");
    seq_feel_remove(s, 0);
    n = chords(s, c);
    CHECK(n == 3 && c[0].steps == 32 && c[0].deg == 4, "removing the first: the next takes its time");
    seq_undo(s);

    seq_feel_resize(s, 0, 1);
    n = chords(s, c);
    CHECK(c[0].steps == 24 && c[1].start == 24 && c[1].steps == 8 && seq_chord_len(s) == 8, "resize: +half a bar from the next (got %d)", c[0].steps);
    seq_feel_resize(s, 0, 1);
    n = chords(s, c);
    CHECK(c[0].steps == 28 && c[1].steps == 4 && seq_chord_len(s) == 4, "then a quarter when the next has only half a bar (got %d)", c[0].steps);
    seq_feel_resize(s, 0, 1);
    n = chords(s, c);
    CHECK(c[1].steps == 4, "the next keeps a quarter");
    CHECK(seq_feel_align(s, 1), "align: possible");
    n = chords(s, c);
    CHECK(c[1].start == 16 && c[0].steps == 16 && c[1].steps == 16, "align: back on the bar line (start %d)", c[1].start);
    CHECK(!seq_feel_align(s, 1), "align: nothing to do on a bar line");
    n = chords(s, c);
    CHECK(c[0].can_longer && c[0].can_shorter && !c[1].can_align, "the chord list says what resize and align can do");
    seq_feel_resize(s, 0, 1);
    n = chords(s, c);
    CHECK(c[1].can_align, "a chord off the bar line can align");
    seq_undo(s);
    n = chords(s, c);
    int last = n - 1, before = c[0].steps;
    seq_feel_resize(s, last, 1);
    n = chords(s, c);
    CHECK(c[0].steps < before && c[0].start == 0, "the last chord takes from the first, the loop still starts on 0");
    int total = 0;
    for (int i = 0; i < n; i++) total += c[i].steps;
    CHECK(total == seq_bars(s) * 16, "the loop's length never moves (%d)", total);
}

/* A chromatic pick, a pick back to the written chord, then undo: the chromatic chord again, not another */
static void test_undo_chromatic(seq_t *s) {
    seq_feel_chord_t c[64];
    int e7 = theory_chroma_src(4, CQ_DOM);
    fresh(s);
    seq_feel_pick(s, 1, 0, e7, COL_7TH);
    chords(s, c);
    CHECK(c[1].src == e7 && c[1].has_base && c[1].base_deg == 4, "E7 over the written G");
    seq_feel_pick(s, 1, c[1].base_deg, c[1].base_src, c[1].base_colour);
    chords(s, c);
    CHECK(c[1].src == 0 && c[1].deg == 4 && !c[1].has_base, "back to G");
    seq_undo(s);
    chords(s, c);
    CHECK(c[1].src == e7 && c[1].colour == COL_7TH && c[1].has_base, "undo: E7 (%s)", name_of(s, c[1].slot));
}

/* Going through the presets from the JAM page: a turn is no undo step, the loop keeps its length; your own work is one step back */
static void test_browse(seq_t *s) {
    seq_feel_chord_t c[64];
    int hand[3] = {48, 60, 67};
    fresh(s);
    seq_feel_browse_preset(s, 2);
    seq_feel_browse_preset(s, 3);
    CHECK(seq_prog_preset(s) == 3, "preset 3 plays");
    seq_undo(s);
    CHECK(seq_prog_preset(s) != 3 || s->undo_n == 0, "turning from preset to preset fills no undo steps (undo_n %d)", s->undo_n);
    fresh(s);
    seq_feel_browse_preset(s, 15);
    CHECK(seq_bars(s) == 4 && chords(s, c) == 4, "Canon from JAM: the loop stays 4 bars, its first 4 chords");
    fresh(s);
    seq_feel_pick(s, 1, 2, 0, COL_TRIAD);
    seq_feel_browse_preset(s, 2);
    seq_undo(s);
    chords(s, c);
    CHECK(c[1].deg == 2 && c[1].has_base, "leaving a chord of your own: one undo brings it back");
    fresh(s);
    seq_set_voicing_style(s, 3);
    int undos = s->undo_n;
    seq_feel_browse_style(s, 4);
    seq_feel_browse_style(s, 5);
    CHECK(s->undo_n == undos && seq_voicing_style(s) == 5, "turning through the styles fills no undo steps");
    seq_feel_hand(s, 0, 3, hand);
    undos = s->undo_n;
    seq_feel_browse_style(s, 6);
    chords(s, c);
    CHECK(s->undo_n == undos + 1 && c[0].hand == 0, "a style replaces notes placed by hand, undoably");
    seq_undo(s);
    chords(s, c);
    CHECK(c[0].hand == 3 && seq_voicing_style(s) == 5, "undo: the notes and the style back");
}

static void test_mute(seq_t *s) {
    int ch0, ba0, ch1, ba1;
    fresh(s);
    play(s, 16);
    ch0 = count_ch(1, 4, 8), ba0 = count_ch(2, 4, 8);
    seq_feel_mute(s, 1, 1);
    play(s, 16);
    ch1 = count_ch(1, 4, 8), ba1 = count_ch(2, 4, 8);
    CHECK(ch0 > 0 && ch1 == 0, "a muted chord: no Chords notes in its bar (%d, then %d)", ch0, ch1);
    CHECK(ba1 == ba0, "the Bass plays on (%d, %d)", ba0, ba1);
    CHECK(count_ch(1, 0, 4) > 0 && count_ch(1, 8, 16) > 0, "the other chords still play");
    seq_undo(s);
    play(s, 16);
    CHECK(count_ch(1, 4, 8) == ch0, "undo: heard again");
}

static void test_hand(seq_t *s) {
    int n[THEORY_CHORD_MAX], v[3] = {67, 48, 60}, c0;
    seq_feel_chord_t c[64];
    fresh(s);
    seq_set_vknob(s, VK_SPR, 2);
    seq_set_vknob(s, VK_INV, 1);
    seq_feel_hand(s, 0, 3, v);
    c0 = seq_slot_notes(s, 0, -1, n);
    CHECK(c0 == 3 && n[0] == 48 && n[1] == 60 && n[2] == 67, "hand notes play as placed, sorted (%d %d %d)", n[0], n[1], n[2]);
    chords(s, c);
    CHECK(c[0].hand == 3 && c[1].hand == 0, "only that chord");
    CHECK(seq_voicing_edited(s) || seq_voicing_style(s) < 0, "a style reads edited");
    seq_feel_pick(s, 0, 3, 0, COL_TRIAD);
    chords(s, c);
    CHECK(c[0].hand == 0, "a new chord drops the hand notes");
    seq_undo(s);
    chords(s, c);
    CHECK(c[0].hand == 3, "undo restores them");
    seq_feel_shape(s, FS_STR, 4);
    chords(s, c);
    CHECK(c[0].hand == 3, "Roll keeps them: it moves no note");
    seq_feel_shape(s, FS_SPR, 0);
    chords(s, c);
    CHECK(c[0].hand == 0, "Space clears them");
    seq_undo(s);
    chords(s, c);
    CHECK(c[0].hand == 3 && seq_feel_shape_get(s, FS_SPR) == 2, "undo: hand notes and Space both back");
}

static void test_smooth(seq_t *s) {
    int a[THEORY_CHORD_MAX], b[THEORY_CHORD_MAX], na, nb, w0, w1;
    seq_feel_chord_t c[64];
    fresh(s);
    seq_feel_shape(s, FS_MODE, VMODE_CLOSE);
    chords(s, c);
    na = seq_slot_notes(s, c[0].slot, -1, a), nb = seq_slot_notes(s, c[1].slot, -1, b);
    w0 = feel_hand_word(a, na, b, nb);
    seq_feel_smooth(s, 0);
    chords(s, c);
    na = seq_slot_notes(s, c[0].slot, -1, a), nb = seq_slot_notes(s, c[1].slot, -1, b);
    w1 = feel_hand_word(a, na, b, nb);
    CHECK(c[1].hand && w1 <= w0 && w1 == FH_STAY, "smooth: the next chord stays near (%d, then %d)", w0, w1);
    CHECK(c[0].hand == 0 && c[2].hand == 0, "only that change");
}

static void test_recipe(seq_t *s) {
    int roles[3] = {TONE_R, TONE_3, TONE_7}, notes[3] = {48, 64, 71}, n[THEORY_CHORD_MAX], c;
    seq_feel_chord_t r[64];
    fresh(s);
    seq_feel_recipe(s, 3, roles, notes, 1);
    CHECK(seq_feel_recipe_n(s) == 3, "a recipe");
    chords(s, r);
    c = seq_slot_notes(s, r[1].slot, -1, n);   /* G: G B F */
    CHECK(c == 3 && n[0] % 12 == 7 && n[1] % 12 == 11 && n[2] % 12 == 5, "it follows the chord: G B F on G (%d %d %d)", n[0], n[1], n[2]);
    CHECK(n[0] >= 42 && n[0] <= 54 && n[1] >= 58 && n[1] <= 70, "each voice near its reference");
    seq_feel_shape(s, FS_COL, 2);
    CHECK(seq_feel_recipe_n(s) == 0, "Rich replaces it");
    seq_undo(s);
    CHECK(seq_feel_recipe_n(s) == 3, "undo brings it back");
}

static void test_save(seq_t *s) {
    static seq_t t;
    static char a[SEQ_STATE_MAX], b[SEQ_STATE_MAX], plain[SEQ_STATE_MAX];
    int hand[3] = {50, 62, 69}, roles[2] = {TONE_R, TONE_5}, notes[2] = {48, 67}, n[THEORY_CHORD_MAX], m[THEORY_CHORD_MAX], c0, c1;
    seq_feel_chord_t x[64], y[64];
    fresh(s);
    seq_state_save(s, plain, sizeof plain);
    CHECK(!strstr(plain, ".cx=") && !strstr(plain, ".vh=") && !strstr(plain, ".vr="), "no use: no new fields");
    seq_feel_pick(s, 1, 0, theory_chroma_src(10, CQ_DOM), COL_7TH);
    seq_feel_mute(s, 2, 1);
    seq_feel_hand(s, 3, 3, hand);
    seq_feel_resize(s, 0, 1);
    seq_state_save(s, a, sizeof a);
    CHECK(strstr(a, "A.cx=") && strstr(a, "A.vh=") && strstr(a, "A.fm=1"), "saved");
    seq_init(&t, rec, NULL);
    CHECK(seq_state_load(&t, a), "loads");
    int nx = chords(s, x), ny = chords(&t, y);
    CHECK(nx == ny, "as many chords (%d, %d)", nx, ny);
    int same = nx == ny;
    for (int i = 0; same && i < nx; i++)
        same = x[i].start == y[i].start && x[i].steps == y[i].steps && x[i].src == y[i].src && x[i].deg == y[i].deg && x[i].colour == y[i].colour &&
               x[i].mute == y[i].mute && x[i].hand == y[i].hand && x[i].has_base == y[i].has_base && x[i].base_deg == y[i].base_deg;
    CHECK(same, "every chord, length, mute, hand note and base as saved");
    CHECK(t.part[t.cur_part].feel_spans == 1, "the chord length's Bass rule too");
    c0 = seq_slot_notes(s, x[3].slot, -1, n), c1 = seq_slot_notes(&t, y[3].slot, -1, m);
    CHECK(c0 == c1 && !memcmp(n, m, sizeof(int) * (size_t)c0), "the hand notes play the same");
    seq_state_save(&t, b, sizeof b);
    CHECK(!strcmp(a, b), "saves back byte for byte");
    seq_feel_recipe(s, 2, roles, notes, 1);
    seq_state_save(s, a, sizeof a);
    CHECK(seq_state_load(&t, a) && seq_feel_recipe_n(&t) == 2 && strstr(a, "A.vr="), "the recipe round trips");
    CHECK(seq_state_load(&t, "csq2|k=0|s=0|A.len=4|A.cl=16|A.prog=0453|A.voi=1|A1=0:|A2=0:|A3=0:|A4=0:|A.cx=zz|A.vh=,,|A.vr=9") == 1 ||
          seq_state_load(&t, plain), "bad Feel fields never refuse a load");
    /* the worst part: 16 bars of quarter chords, every one muted, chromatic, by hand */
    fresh(s);
    seq_set_bars(s, 16);
    seq_set_chord_len(s, 4);
    for (int i = 0; i < 64; i += 2) {
        int h[6] = {40 + i % 7, 47, 52, 59, 64, 71};
        seq_feel_chord_t k[64];
        int cnt = chords(s, k);
        (void)cnt;
        s->part[s->cur_part].src[i] = (unsigned char)theory_chroma_src(i % 12, i % CQ_COUNT);
        s->part[s->cur_part].prog[i] = 0;
        s->part[s->cur_part].fs[i].mute = 1;
        s->part[s->cur_part].fs[i].hand_n = 6;
        for (int q = 0; q < 6; q++) s->part[s->cur_part].fs[i].hand[q] = (signed char)h[q];
        s->part[s->cur_part].fs[i].pick = 0;
    }
    c0 = seq_state_save(s, a, sizeof a);
    CHECK(c0 > 0 && seq_unsaved_parts(s) == 0, "a worst-case part fits (%d bytes)", c0);
}

/* Uneven lengths store as shorter slots: does that move Bass and Melody where no chord changed? Every genre preset, 8 bars, by channel */
static void test_same_chords_any_length(seq_t *s) {
    static ev_t a[MAX_EV];
    int total[3] = {0, 0, 0}, diff[3] = {0, 0, 0};
    for (int k = 0; k < PRESETS_COUNT; k++) {
        int na;
        fresh(s);
        seq_song_load(s, preset_chunk(k));
        seq_arr_set_form(s, 0);
        play(s, 32);
        na = nev;
        memcpy(a, evs, sizeof a[0] * (size_t)nev);
        fresh(s);
        seq_song_load(s, preset_chunk(k));
        seq_arr_set_form(s, 0);
        s->part[s->cur_part].feel_spans = 1;   /* as a Feel write's */
        seq_set_chord_len(s, 4);
        play(s, 32);
        for (int ch = 0; ch < 3; ch++) {
            int i = 0, j = 0;
            while (i < na || j < nev) {
                while (i < na && (a[i].status & 15) != ch) i++;
                while (j < nev && (evs[j].status & 15) != ch) j++;
                if (i >= na && j >= nev) break;
                total[ch]++;
                if (i >= na || j >= nev || a[i].note != evs[j].note || a[i].beat != evs[j].beat) diff[ch]++;
                if (i < na) i++;
                if (j < nev) j++;
            }
        }
    }
    CHECK(!diff[0] && !diff[1] && !diff[2] && total[0] && total[1] && total[2], "every preset: the same chords at a shorter chord length play the same notes (Chords %d/%d differ, Bass %d/%d, Melody %d/%d)",
          diff[0], total[0], diff[1], total[1], diff[2], total[2]);
}

static const char *runs(const seq_t *s) {
    static char b[512];
    seq_feel_chord_t c[64];
    int n = chords(s, c), w = 0;
    b[0] = 0;
    for (int i = 0; i < n; i++) w += snprintf(b + w, sizeof b - (size_t)w, "%s%d:%d", i ? " " : "", c[i].start, c[i].deg);
    return b;
}

/* The part's chords after a mute of the first point (a point holds no mute: the verb stores the slots), given the points {bar, step, deg} */
static const char *runs_after_verb(seq_t *s, int bars, const int (*pt)[3], int n) {
    seq_feel_chord_t c[64];
    int k = 0, m;
    fresh(s);
    seq_set_bars(s, bars);
    for (int i = 0; i < n; i++)
        CHECK(seq_cp_set(s, pt[i][0], pt[i][1], pt[i][2], 0, 0), "the point at bar %d step %d is set", pt[i][0], pt[i][1]);
    m = chords(s, c);
    while (k + 1 < m && c[k].slot < SEQ_MAX_PROG) k++;
    seq_feel_mute(s, k, 1);
    return runs(s);
}

#define SNAP(want, b, ...) do { \
        static const int P[][3] = {__VA_ARGS__}; \
        const char *got = runs_after_verb(s, b, P, (int)(sizeof P / sizeof P[0])); \
        CHECK(!strcmp(got, want), "%s (got %s)", want, got); \
    } while (0)

/* A Feel verb puts off-beat chords on a quarter and loses none (seq_core.h has the rule) */
static void test_snap_keeps_chords(seq_t *s) {
    seq_feel_chord_t c[64];
    int n;
    /* {start:degree}; INIT's chords are a bar each, degrees 0 4 5 3 */
    SNAP("0:0 12:1 16:4 32:5 48:3", 4, {0, 15, 1});                   /* the 16th before the next chord: both stay */
    SNAP("0:0 4:1 16:4 32:5 48:3", 4, {0, 1, 1});                     /* the 16th after a chord: not lost to it */
    SNAP("0:0 16:4 32:5 48:3 60:1", 4, {3, 15, 1});                   /* the loop's last 16th does not round past its end */
    SNAP("0:0 8:1 12:2 16:4 32:5 48:3", 4, {0, 13, 1}, {0, 15, 2});   /* two in a row, in order */
    SNAP("0:0 4:1 8:2 16:4 32:5 48:3", 4, {0, 4, 1}, {0, 5, 2});      /* on the beat stays; the off-beat one takes the next */
    SNAP("0:0 8:1 16:4 32:5 48:3", 4, {0, 6, 1});                     /* no collision: the nearest quarter, as before */
    SNAP("0:0 12:1 16:4 32:5 48:3", 4, {0, 12, 1});                   /* on the beat: nothing moves */
    /* no quarter between two chords: the one on the beat makes room, and only as far as it must */
    SNAP("0:0 4:1 8:2 16:4 32:5 48:3", 4, {0, 1, 1}, {0, 4, 2});
    SNAP("0:0 4:1 8:2 12:3 16:6 20:4 32:5 48:3", 4, {0, 2, 1}, {0, 4, 2}, {0, 8, 3}, {0, 12, 6});
    /* more chords than quarters: the shortest off the beat goes, its time to the chord before */
    SNAP("0:0 4:2 8:3 12:4", 1, {0, 2, 1}, {0, 4, 2}, {0, 7, 3}, {0, 10, 4});

    fresh(s);
    seq_cp_set(s, 0, 15, 1, 0, 0);
    seq_feel_mute(s, 1, 1);
    seq_feel_pick(s, 3, 1, 0, COL_7TH);
    n = chords(s, c);
    CHECK(n == 5 && c[1].deg == 1 && c[1].start == 12 && c[1].steps == 4 && c[3].colour == COL_7TH, "a pick on another chord keeps the pushed one (%s)", runs(s));
    CHECK(c[n - 1].start + c[n - 1].steps == seq_bars(s) * SEQ_STEPS && seq_bars(s) == 4, "the loop keeps its length");
    seq_undo(s);
    seq_undo(s);
    n = chords(s, c);
    CHECK(n == 5 && c[1].start == 15 && c[1].steps == 1 && c[2].start == 16, "undo: the 16th is back where it was (%s)", runs(s));
}

/* A verb that moves no time (pick, mute, hand, smooth) leaves Tweak's points where they are; one a point cannot hold stores the slots */
static void test_still_verbs_keep_points(seq_t *s) {
    static const char *const AS_TWEAKED = "0:0 15:1 16:4 32:5 48:3";
    seq_feel_chord_t c[64];
    seq_cp_t pt;
    int notes[3] = {48, 52, 55}, n;
    fresh(s);
    seq_cp_set(s, 0, 15, 1, 0, 0);
    seq_feel_mute(s, 0, 1);
    n = chords(s, c);
    CHECK(!strcmp(runs(s), AS_TWEAKED) && c[0].mute && !c[1].mute, "mute keeps the 16th (%s)", runs(s));
    CHECK(seq_cp_get(s, 0, 0, &pt) && pt.step == 15 && n == 5, "the point is still a point");
    seq_feel_mute(s, 0, 0);
    seq_feel_pick(s, 2, 2, 0, COL_7TH);
    n = chords(s, c);
    CHECK(!strcmp(runs(s), "0:0 15:1 16:2 32:5 48:3") && c[2].colour == COL_7TH && c[2].has_base, "a pick on a slot keeps it (%s)", runs(s));
    seq_feel_hand(s, 0, 3, notes);
    seq_feel_smooth(s, 1);
    n = chords(s, c);
    CHECK(!strcmp(runs(s), "0:0 15:1 16:2 32:5 48:3") && c[0].hand == 3 && c[2].hand > 0, "hand notes and smooth keep it (%s)", runs(s));
    seq_feel_pick(s, 1, 6, 0, COL_7TH);
    n = chords(s, c);
    CHECK(!strcmp(runs(s), "0:0 15:6 16:2 32:5 48:3") && c[1].colour == COL_7TH && c[1].slot >= SEQ_MAX_PROG, "a pick on the point changes the point (%s)", runs(s));
    seq_feel_pick(s, 1, 0, 0, c[0].colour);
    CHECK(!strcmp(runs(s), "0:0 16:2 32:5 48:3") && !seq_cp_count(s, 0), "the chord before it again: the point goes (%s)", runs(s));
    for (int i = 0; i < 7; i++) seq_undo(s);
    CHECK(!strcmp(runs(s), AS_TWEAKED) && chords(s, c) == 5 && !c[0].mute, "undo, a step a verb: as Tweak left it (%s)", runs(s));
    seq_feel_hand(s, 1, 3, notes);
    CHECK(!strcmp(runs(s), "0:0 12:1 16:4 32:5 48:3") && !seq_cp_count(s, 0), "hand notes on the point: the slots, the 16th on its quarter (%s)", runs(s));
}

/* A hit held into a Feel chord strikes it there; the rhythm's own rests are reported, and a fix gives the chord a hit */
static int hits_at(double beat) { return count_ch(1, beat - 0.02, beat + 0.1); }

static void paint(seq_t *s, int bar, const char *codes) {
    unsigned char c[16];
    rhythm_parse(codes, c);
    seq_rbar_open(s, LANE_CHORDS, bar);
    for (int i = 0; i < 16; i++) seq_rbar_paint(s, LANE_CHORDS, i, c[i]);
    seq_rbar_done(s, LANE_CHORDS);
}

/* A 1-bar loop painted so, its chord split at beat 3 with a new chord on Feel */
static void two_chords(seq_t *s, const char *codes) {
    fresh(s);
    seq_set_bars(s, 1);
    paint(s, 0, codes);
    seq_feel_add(s, 0, 4, 0, 0);
}

static void test_held_into_a_chord(seq_t *s) {
    static char buf[SEQ_STATE_MAX];
    seq_feel_chord_t c[64];
    unsigned char plays[16], drawn[16];
    int low[THEORY_CHORD_MAX], high[THEORY_CHORD_MAX];

    two_chords(s, "x---------------");
    play(s, 4);
    CHECK(hits_at(0) > 0 && hits_at(2) > 0, "a held hit strikes the new chord (beat 1: %d, beat 3: %d)", hits_at(0), hits_at(2));
    seq_bar_plays(s, LANE_CHORDS, 0, plays);
    seq_bar_codes(s, LANE_CHORDS, 0, drawn);
    CHECK(rhythm_is_hit(plays[8]) && !rhythm_is_hit(drawn[8]), "Tweak shows it as played, and the paint stays as drawn");
    CHECK(seq_step_flags(s, LANE_CHORDS, 0, 8) & SF_ADDED, "it reads as an added hit");
    CHECK(rhythm_is_hit(plays[0]) && plays[7] == RC_HOLD, "the first chord holds up to the change");
    seq_undo(s);
    play(s, 4);
    CHECK(hits_at(2) == 0, "undo: one chord, one hit");

    /* a muted chord stays silent, and the chord after it is heard */
    two_chords(s, "x---------------");
    seq_feel_mute(s, 0, 1);
    play(s, 4);
    CHECK(hits_at(0) == 0 && hits_at(2) > 0, "muted first chord: silent, the next is heard (%d, %d)", hits_at(0), hits_at(2));
    seq_feel_mute(s, 0, 0);
    seq_feel_mute(s, 1, 1);
    play(s, 4);
    CHECK(hits_at(0) > 0 && hits_at(2) == 0, "muted second chord: silent (%d, %d)", hits_at(0), hits_at(2));

    /* the same chord twice is no change: picking it there and back re-renders */
    two_chords(s, "x---------------");
    chords(s, c);
    seq_feel_pick(s, 1, c[0].deg, c[0].src, c[0].colour);
    play(s, 4);
    CHECK(hits_at(2) == 0, "the same chord on both sides: no new hit");
    seq_feel_add(s, 0, 4, 0, 0);
    play(s, 4);
    CHECK(hits_at(2) > 0, "a change there again: heard");

    /* the struck hit plays in the held hit's octave */
    fresh(s);
    seq_set_bars(s, 1);
    paint(s, 0, "x---------------");
    seq_rbar_open(s, LANE_CHORDS, 0);
    seq_rbar_select(s, LANE_CHORDS, 0);
    seq_rbar_hit_oct(s, 2);
    seq_rbar_done(s, LANE_CHORDS);
    seq_part_step_notes(s, seq_part(s), LANE_CHORDS, 0, 0, high);
    seq_feel_add(s, 0, 0, 0, COL_7TH);
    seq_part_step_notes(s, seq_part(s), LANE_CHORDS, 0, 8, low);
    CHECK(low[0] == high[0], "the struck hit keeps the held hit's octave (%d, %d)", low[0], high[0]);

    /* ANTICIPATE's early hit already plays the new chord: no second strike on its tie */
    two_chords(s, "x---....x-------");
    paint(s, 0, "x---....x-------");
    seq_set_anticipate(s, LANE_CHORDS, AN_8);
    play(s, 4);
    CHECK(hits_at(1.5) > 0 && hits_at(2) == 0, "ANTICIPATE's tie: heard early, not twice (%d, %d)", hits_at(1.5), hits_at(2));

    /* a FILL bar plays its own variant, and holds into a chord there too */
    fresh(s);
    seq_set_bars(s, 4);
    paint(s, 3, "x---------------");
    seq_feel_add(s, 3, 4, 0, 0);
    seq_set_rknob(s, LANE_CHORDS, RK_FILL, 1);
    play(s, 16);
    CHECK(hits_at(14) > 0, "a FILL bar: the held hit strikes the new chord");

    /* Rich can make or remove a change between a chord and the same chord with its own colour */
    two_chords(s, "x---------------");
    chords(s, c);
    seq_feel_pick(s, 1, c[0].deg, c[0].src, COL_7TH);
    seq_feel_shape(s, FS_COL, 0);
    seq_bar_plays(s, LANE_CHORDS, 0, plays);
    CHECK(rhythm_is_hit(plays[8]), "Plain against a 7th: a change, struck");
    seq_feel_shape(s, FS_COL, 1);
    seq_bar_plays(s, LANE_CHORDS, 0, plays);
    CHECK(plays[8] == RC_HOLD, "Warm against a 7th: the same chord, so it holds");

    /* the save brings it back */
    two_chords(s, "x---------------");
    seq_state_save(s, buf, sizeof buf);
    fresh(s);
    seq_state_load(s, buf);
    play(s, 4);
    CHECK(hits_at(2) > 0, "saved and loaded: still heard");

    /* a part without Feel's chords keeps its sound: this is an old song's rule */
    fresh(s);
    seq_set_bars(s, 1);
    seq_set_chord_len(s, 8);
    paint(s, 0, "x---------------");
    play(s, 4);
    CHECK(hits_at(2) == 0, "no Feel write: a hit holds through as before");
}

static void test_chord_without_a_hit(seq_t *s) {
    seq_feel_chord_t c[64];
    unsigned char plays[16];
    unsigned before;
    two_chords(s, "A-----x-........");
    CHECK(chords(s, c) == 2 && c[0].hits == 2 && c[1].hits == 0, "the rhythm rests under chord 2 (%d, %d)", c[0].hits, c[1].hits);
    CHECK(c[1].can_hit, "and the fix can give it one");
    play(s, 4);
    CHECK(hits_at(2) == 0, "and it is silent");
    before = seq_feel_hash(s);
    seq_feel_hit(s, 1);
    CHECK(seq_feel_hash(s) != before, "the page sees the new hit");
    chords(s, c);
    CHECK(c[1].hits == 1, "the fix gives it one hit (%d)", c[1].hits);
    play(s, 4);
    CHECK(hits_at(2) > 0, "and it is heard");
    seq_bar_plays(s, LANE_CHORDS, 0, plays);
    CHECK(plays[15] == RC_HOLD, "the hit rings to the chord's end");
    CHECK(plays[0] == RC_ACC && plays[6] == RC_HIT, "the rhythm before it stays");
    seq_undo(s);
    chords(s, c);
    CHECK(c[1].hits == 0, "undo: the rest is back");
    seq_feel_hit(s, 0);
    chords(s, c);
    CHECK(c[0].hits == 2 && !c[0].can_hit, "a chord with a hit on its first step: nothing changes");
    seq_feel_hit(s, 9);
    seq_feel_hit(s, -1);
    chords(s, c);
    CHECK(c[1].hits == 0, "a bad chord: ignored");

    /* a setting takes the drawn hit away: the fix has nothing to add, and says so through can_hit */
    two_chords(s, "x.......x.......");
    seq_set_rknob(s, LANE_CHORDS, RK_BREATH, 3);
    chords(s, c);
    CHECK(c[1].hits == 0 && !c[1].can_hit, "BREATH rests chord 2's hit: hits 0, and no hit to give");
}

static void test_ladder(void) {
    int bad = 0, dup = 0, oor = 0, total = 0;
    for (int scale = 0; scale < SCALE_COUNT; scale++)
        for (int key = 0; key < 12; key++)
            for (int deg = 0; deg < 7; deg++) {
                feel_option_t o[FEEL_OPTIONS_MAX];
                feel_chord_t base = {deg, 0, COL_TRIAD}, prev = {(deg + 6) % 7, 0, COL_TRIAD}, next = {(deg + 3) % 7, 0, COL_TRIAD};
                int n = feel_ladder(key, scale, base, prev, next, o), lv = -1;
                total += n;
                for (int i = 0; i < n; i++) {
                    char nm[32];
                    if (o[i].level < lv) bad++;
                    lv = o[i].level;
                    for (int j = 0; j < i; j++) dup += feel_same(key, scale, o[i].c, o[j].c);
                    if (o[i].c.src < 0 || o[i].c.src > THEORY_SRC_MAX) oor++;
                    theory_chord_name_c(key, theory_slot_scale(scale, o[i].c.src), o[i].c.deg, o[i].c.colour, nm, sizeof nm);
                    if (!nm[0]) bad++;
                }
                if (n < 4 || o[0].level != FEEL_HOME || o[0].kind != FK_WRITTEN) bad++;
            }
    CHECK(!bad && !dup && !oor, "the ladder in all 7 scales, 12 keys, 7 degrees: %d options, in order, no chord twice, in range (%d %d %d)", total, bad, dup, oor);

    feel_chord_t C = {0, 0, 0}, F = {3, 0, 0}, G = {4, 0, 0}, Am = {5, 0, 0}, G7 = {4, 0, COL_7TH};
    feel_option_t o[FEEL_OPTIONS_MAX];
    int n = feel_ladder(0, SCALE_MAJOR, G, C, Am, o), kind, seen = 0;
    char nm[32];
    for (int i = 0; i < n; i++) {
        theory_chord_name_c(0, theory_slot_scale(SCALE_MAJOR, o[i].c.src), o[i].c.deg, o[i].c.colour, nm, sizeof nm);
        if (!strcmp(nm, "E7") && o[i].level == FEEL_TENSE) seen |= 1;     /* V7 of Am */
        if (!strcmp(nm, "G#dim7") && o[i].level == FEEL_TENSE) seen |= 2;
        if (!strcmp(nm, "A#7") && o[i].level == FEEL_OUTSIDE) seen |= 4;  /* tritone sub of Am */
        if (!strcmp(nm, "E") && o[i].level == FEEL_OUTSIDE) seen |= 8;    /* a mediant from C */
        if (!strcmp(nm, "G7") && o[i].level == FEEL_WARM) seen |= 16;
        if (!strcmp(nm, "Fm") && o[i].level == FEEL_COLOURFUL) seen |= 32;
    }
    CHECK(seen == 63, "G between C and Am offers G7, Fm, E7, G#dim7, A#7, E (%x)", seen);
    CHECK(feel_level(0, SCALE_MAJOR, G7, G, C, C, &kind) == FEEL_WARM && kind == FK_RICHER, "G7 sits on Warm");
    CHECK(feel_flow(0, SCALE_MAJOR, G, C) == FF_PULLS && feel_flow(0, SCALE_MAJOR, G7, C) == FF_LANDS_HARD, "G pulls to C, G7 lands hard");
    CHECK(feel_flow(0, SCALE_MAJOR, C, G) == FF_OPENS && feel_flow(0, SCALE_MAJOR, F, C) == FF_SETTLES, "C opens to G, F settles on C");
    CHECK(feel_flow(0, SCALE_MAJOR, F, G) == FF_STEPS && feel_flow(0, SCALE_MAJOR, C, Am) == FF_SHARES && feel_shared(0, SCALE_MAJOR, C, Am) == 2, "steps, shares 2");
    feel_chord_t s0 = feel_suggest(0, SCALE_MAJOR, C, Am);   /* into Am: its 5th is E, the Em of the scale */
    CHECK(s0.deg == 2 && !s0.src, "add between C and Am suggests Em (deg %d)", s0.deg);
    int st[3] = {16, 4, 4};
    CHECK(feel_add_target(st, 3, 0) == 0 && feel_add_target(st, 3, 1) == 0 && st[0] == 16, "add splits the selected chord, else the longest");
    int st2[2] = {4, 4};
    CHECK(feel_add_target(st2, 2, 0) == -1, "nothing left to split");
    int pcs[THEORY_CHORD_MAX];
    feel_chord_t E7 = {0, theory_chroma_src(4, CQ_DOM), COL_7TH};
    CHECK(feel_out_of_key(0, SCALE_MAJOR, E7, pcs) == 1 && pcs[0] == 8, "E7 in C: G# is outside");
    CHECK(feel_role_of(0, SCALE_MAJOR, C, 64) == TONE_3 && feel_role_of(0, SCALE_MAJOR, G7, 65) == TONE_7 && feel_role_of(0, SCALE_MAJOR, C, 62) == TONE_2, "a note's job");
    int low[3] = {40, 44, 60}, clear[3] = {40, 52, 60};
    CHECK(feel_muddy(low, 3) == 0 && feel_muddy(clear, 3) == -1, "low notes too close are called out");
    int a1[3] = {60, 64, 67}, a2[3] = {59, 62, 67}, a3[3] = {71, 74, 79};
    CHECK(feel_hand_word(a1, 3, a2, 3) == FH_STAY && feel_hand_word(a1, 3, a3, 3) == FH_JUMP, "hands stay, hands jump");
    CHECK(feel_styles() == VOICING_NSTYLES && feel_style(0) && !feel_style(VOICING_NSTYLES), "the styles' table");
}

int main(void) {
    static seq_t s;
    test_chromatic();
    test_followers(&s);
    test_verbs(&s);
    test_undo_chromatic(&s);
    test_browse(&s);
    test_mute(&s);
    test_hand(&s);
    test_smooth(&s);
    test_recipe(&s);
    test_save(&s);
    test_same_chords_any_length(&s);
    test_snap_keeps_chords(&s);
    test_still_verbs_keep_points(&s);
    test_held_into_a_chord(&s);
    test_chord_without_a_hit(&s);
    test_ladder();
    printf(fails ? "\nFAILED: %d\n" : "\nPASSED\n", fails);
    return fails != 0;
}
