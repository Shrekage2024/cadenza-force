/* Offline test of src/seq_core.c (run by tests/run_tests.sh under ASan/UBSan). Exit 1 on failure. */
#include "../src/seq_core.h"
#include "../src/gen.h"
#include "../src/theory.h"
#include "../src/voicing.h"
#include "../src/melody.h"
#include "../src/rhythm.h"
#include "../src/jamlib.h"
#include "../src/presets.h"
#include "../src/presets_data.h"   /* 0.24 test_24_load_old_unchanged: the genre presets' chunks (PRESET_CHUNK) */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXEV 20000
typedef struct { int status, note, vel, k; double ppq; int part; } ev_t;   /* k: the 16th last fired, ppq: the block's song
                                                                          * position, part: the playing part */
static ev_t evs[MAXEV];
static int nev, fails;
static const seq_t *ev_seq;   /* set during play: the sequencer and block position emit records */
static double ev_ppq;

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static void emit(void *ctx, int status, int note, int vel, int frame) {
    (void)ctx; (void)frame;
    if (nev < MAXEV) evs[nev++] = (ev_t){status, note, vel, ev_seq ? (int)ev_seq->cur_k : -1, ev_ppq, ev_seq ? ev_seq->cur_part : -1};
}

static int is_on(const ev_t *e) { return (e->status & 0xf0) == 0x90 && e->vel > 0; }
static int is_off(const ev_t *e) { return (e->status & 0xf0) == 0x80 || ((e->status & 0xf0) == 0x90 && e->vel == 0); }

static int count_on(int ch) {
    int n = 0;
    for (int i = 0; i < nev; i++) n += is_on(&evs[i]) && (evs[i].status & 15) == ch - 1;
    return n;
}

static int count_off(int ch) {
    int n = 0;
    for (int i = 0; i < nev; i++) n += is_off(&evs[i]) && (evs[i].status & 15) == ch - 1;
    return n;
}

/* every note-on has its note-off on the same channel and note */
static int balanced(void) {
    int open[16][128];
    memset(open, 0, sizeof open);
    for (int i = 0; i < nev; i++) {
        int ch = evs[i].status & 15, n = evs[i].note;
        if (is_on(&evs[i])) open[ch][n]++;
        else if (is_off(&evs[i])) open[ch][n]--;
    }
    for (int c = 0; c < 16; c++)
        for (int n = 0; n < 128; n++)
            if (open[c][n]) return 0;
    return 1;
}

/* most notes sounding at once on one channel, over the whole run: a 90% gate means a step's notes end before the next step */
static int max_open(int ch) {
    int open = 0, most = 0;
    for (int i = 0; i < nev; i++) {
        if ((evs[i].status & 15) != ch - 1) continue;
        if (is_on(&evs[i])) open++;
        else if (is_off(&evs[i])) open--;
        if (open > most) most = open;
    }
    return most;
}

static double bpm = 120.0;

/* a new sequencer and an empty event log; all_mel_up turns on every Melody 1 step (one note per 16th) */
/* a new sequencer and an empty event log; all_mel_up turns on every Melody 1 step in each of the 4 bars */
/* 0.19: v1's MANUAL Bass lane (a root on steps 1 and 9 of each of its 4 bars, octave -1), which a v1 load or reset now converts to
 * DRAWN bars: the tests of the MANUAL Bass put it back (no undo step) */
static void v1_bass(seq_t *s) {
    seq_lane_t *ln = &s->part[s->cur_part].lane[LANE_BASS];
    memset(ln, 0, sizeof *ln);
    ln->gen = (seq_gen_t){GEN_MANUAL, 1, 30, 0};
    for (int i = 0; i < 4 * SEQ_STEPS; i++) ln->steps[i] = (seq_step_t){i % 16 == 0 || i % 16 == 8, TONE_R, -1, 100, 1, 0};
}

static void fresh(seq_t *s, int all_mel_up) {
    nev = 0;
    seq_init(s, emit, NULL);
    seq_reset_v1(s);                     /* the v1 tests pin v1's MANUAL lanes; new instances default to generated ones */
    v1_bass(s);                          /* 0.19: the reset makes the Bass DRAWN bars */
    seq_set_voicing(s, VOICING_CLOSE);   /* the v1 tests pin close voicing; new instances default to smooth */
    for (int l = 0; l < 3; l++) seq_set_channel(s, l, l + 2);   /* and v1's channels 2-4; new instances use 1-3 */
    for (int b = 0; all_mel_up && b < 4; b++)
        for (int i = 0; i < SEQ_STEPS; i++) seq_set_step(s, LANE_MEL_UP, b, i, 1);
}
static double blk(void) { return 128 * bpm / (60.0 * 44100.0); }

/* play from `ppq` for `beats`, 128-frame blocks, optional loop of `loop` beats from 0 */
static double play(seq_t *s, double ppq, double beats, double loop) {
    double done = 0;
    ev_seq = s;
    while (done < beats) {
        ev_ppq = ppq;
        seq_block(s, ppq, bpm, 1, 128);
        ppq += blk();
        done += blk();
        if (loop > 0 && ppq >= loop) ppq -= loop;
    }
    ev_seq = NULL;
    return ppq;
}

static seq_lane_t *cur_lane(seq_t *s, int l) { return &s->part[s->cur_part].lane[l]; }

static int any_on(const seq_lane_t *ln) {
    for (int i = 0; i < SEQ_LANE_STEPS; i++)
        if (ln->steps[i].on) return 1;
    return 0;
}

/* 0.7: Melody 2 is retired. The lane stays in the state (A4 is a required field) but never plays, never generates, and
 * is empty after every load. */
static void test_melody2_retired(void) {
    static seq_t s;
    char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    CHECK(cur_lane(&s, 3)->gen.style == GEN_MANUAL, "a new instance has no Melody 2 recipe");
    nev = 0;
    seq_set_step(&s, 3, 0, 0, 1);
    play(&s, 0.0, 8.0, 0);
    CHECK(count_on(5) == 0, "Melody 2 never plays");
    /* a 0.6 state with an Answer on Melody 2 loads without it */
    const char *st = "csq2|gv=1|k=4|s=1|A.len=1|A.cl=16|A.h=30,1|A.prog=0526|A.voi=1|A1=0:|A1.g=2,1,30|A2=0:|A2.g=4,1,30"
                     "|A3=0:|A3.g=7,1,30|A4=0:|A4.g=9,1,30";
    CHECK(seq_state_load(&s, st), "0.6 state loads");
    CHECK(cur_lane(&s, 3)->gen.style == GEN_MANUAL && !any_on(cur_lane(&s, 3)), "Melody 2 dropped on load");
    CHECK(seq_state_save(&s, buf, sizeof buf) && strstr(buf, "|A4=0:") && !strstr(buf, "A4.g"), "saved without it");
}

static void test_default_pattern(void) {
    static seq_t s;
    fresh(&s, 0);
    play(&s, 0, 16.0, 0);   /* 4 bars */
    seq_block(&s, 16.0, bpm, 0, 128);   /* stop */
    CHECK(count_on(2) == 12, "chords: 3 notes x 1 step x 4 bars = 12 (got %d)", count_on(2));
    CHECK(count_on(3) == 8, "bass: 2 steps x 4 bars = 8 (got %d)", count_on(3));
    CHECK(count_on(4) == 32 && count_on(5) == 0, "melody 1: 8 steps x 4 bars; Melody 2 is retired (got %d, %d)", count_on(4), count_on(5));
    CHECK(balanced(), "every note-on has a note-off after stop");
    CHECK(max_open(2) == 3 && max_open(4) == 1, "notes end within their 1/16 (chords at most 3 at once, melody 1)");

    int bass[4] = {0}, nb = 0, mel1[8] = {0}, n1 = 0, chord[3] = {0}, nc = 0;
    for (int i = 0; i < nev; i++) {
        if (!is_on(&evs[i])) continue;
        int ch = (evs[i].status & 15) + 1;
        if (ch == 3 && nb < 4 && (nb == 0 || bass[nb - 1] != evs[i].note)) bass[nb++] = evs[i].note;
        if (ch == 4 && n1 < 8) mel1[n1++] = evs[i].note;
        if (ch == 2 && nc < 3) chord[nc++] = evs[i].note;
    }
    CHECK(chord[0] == 52 && chord[1] == 55 && chord[2] == 59, "bar 1 chord is Em = E3 G3 B3");
    CHECK(bass[0] == 40 && bass[1] == 36 && bass[2] == 43 && bass[3] == 38, "bass roots E2 C2 G2 D2 (got %d %d %d %d)",
          bass[0], bass[1], bass[2], bass[3]);
    CHECK(mel1[0] == 64 && mel1[1] == 67 && mel1[2] == 71 && mel1[3] == 64, "melody 1 arps up: E4 G4 B4 E4");
}

/* MIDI_TIMING.md "Test it": 50 loops of 4 beats with a loop length that is not a multiple of the block size */
static void test_loops(void) {
    static seq_t s;
    fresh(&s, 1);
    double ppq = play(&s, 0, 4.0 * 5, 4.0);
    int before = count_on(4);
    play(&s, ppq, 4.0 * 45, 4.0);
    int got = count_on(4) - before;
    CHECK(got >= 719 && got <= 721, "loops 6-50: 720 steps within 1 (got %d)", got);
    seq_block(&s, 0, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes after 50 loop wraps");
}

static void test_start_mid_bar(void) {
    static seq_t s;
    fresh(&s, 1);
    seq_block(&s, 3.3, bpm, 1, 128);
    while (count_on(4) == 0) seq_block(&s, s.last_ppq + blk(), bpm, 1, 128);
    CHECK(seq_cur_step(&s) == 14, "a start at ppq 3.3 plays step 15 (3.5 beats) first (got step %d)", seq_cur_step(&s) + 1);
}

/* tempo changes step ppq back a little and lurch it ahead: no 16th may fire twice, and the pattern step follows the song position */
static void test_tempo_jitter(void) {
    static seq_t s;
    fresh(&s, 1);
    double ppq = 0;
    long last_k = -1;
    int doubled = 0, out_of_phase = 0;
    for (int b = 1; b < 20000; b++) {
        seq_block(&s, ppq, bpm, 1, 128);
        if (s.cur_k >= 0) {
            if (s.cur_k < last_k) doubled++;
            if (seq_cur_step(&s) != (int)(s.cur_k % 16)) out_of_phase++;
            if (s.cur_k > last_k) last_k = s.cur_k;
        }
        ppq += blk();
        if (b % 37 == 0) ppq -= 0.12;
        if (b % 53 == 0) ppq += 0.4;
        if (b % 400 == 0) bpm = bpm == 120.0 ? 160.0 : (bpm == 160.0 ? 90.0 : 120.0);
    }
    bpm = 120.0;
    CHECK(doubled == 0, "tempo jitter: no step fired twice (%d)", doubled);
    CHECK(out_of_phase == 0, "pattern step always equals song position mod 16");
    int fires = count_on(4);
    CHECK(fires > 0 && fires <= (int)(last_k + 1), "fired %d steps, at most one per 16th (%ld)", fires, last_k + 1);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes after tempo jitter");
}

static void test_mute_and_panic(void) {
    static seq_t s;
    fresh(&s, 0);
    double ppq = play(&s, 0, 2.0, 0);
    seq_set_mute(&s, LANE_MEL_UP, 1);
    int before = count_on(4);
    ppq = play(&s, ppq, 4.0, 0);
    CHECK(count_on(4) == before, "muted melody 1 sends no note-ons");
    CHECK(count_on(3) > 0, "the bass keeps playing");
    seq_all_notes_off(&s);
    int cc = 0;
    for (int i = 0; i < nev; i++) cc += (evs[i].status & 0xf0) == 0xB0 && evs[i].note == 123;
    CHECK(cc == SEQ_ACTIVE_LANES && cc == 3, "ALL NOTES OFF sends CC 123 on the 3 lane channels");
    CHECK(balanced(), "no stuck notes after mute and ALL NOTES OFF");
}

/* the playing part of two sequencers holds the same length, chord length, mutes and on-steps */
/* 0.18 Task 6: a step's length as it sounds: to its own end, the next hit (which re-strikes the chord) or the bar end */
static int sounding_len(const seq_step_t *bar, int i) {
    int e = i + 1;
    while (e < SEQ_STEPS && !bar[e].on) e++;
    return bar[i].len < e - i ? bar[i].len : e - i;
}

static int vel_class(int v) { return v >= 108 ? RC_ACC : v <= 70 ? RC_GHOST : RC_HIT; }

/* R2: a bar sounds as another when every step's on, sounding length and velocity class (ACC / HIT / GHOST) match */
static int bar_sounds_as(const seq_step_t *got, const seq_step_t *want) {
    for (int i = 0; i < SEQ_STEPS; i++) {
        if (got[i].on != want[i].on) return 0;
        if (got[i].on && (sounding_len(got, i) != sounding_len(want, i) || vel_class(got[i].vel) != vel_class(want[i].vel))) return 0;
    }
    return 1;
}

/* 0.18: b is a's Chords lane loaded: -1 when that is no conversion (a already DRAWN, or b not), else 1 when b is a's 0.17
 * lane (generated or MANUAL) converted to DRAWN bars at COMPLEXITY 50 that sound as a did, bar by bar (R2), 0 when not */
static int chords_loaded_as(const seq_lane_t *a, const seq_lane_t *b, int bars) {
    if (a->gen.style == GEN_CH_DRAWN || b->gen.style != GEN_CH_DRAWN) return -1;
    if (b->gen.complexity != CHORDS_CX_NEUTRAL) return 0;
    for (int i = 0; i < bars; i++)
        if (!bar_sounds_as(&b->steps[i * SEQ_STEPS], &a->steps[i * SEQ_STEPS])) return 0;
    return 1;
}

/* 0.19: b is a's Bass lane loaded: -1 when that is no conversion (a already DRAWN, or b not), else 1 when b is a's 0.18 lane
 * (generated or MANUAL) converted to DRAWN bars at COMPLEXITY 50 that keep its rhythm, bar by bar (R2; the notes come from the
 * note model), 0 when not */
static int bass_loaded_as(const seq_lane_t *a, const seq_lane_t *b, int bars) {
    if (a->gen.style == GEN_BASS_DRAWN || b->gen.style != GEN_BASS_DRAWN) return -1;
    if (b->gen.complexity != BASS_CX_NEUTRAL) return 0;
    for (int i = 0; i < bars; i++)
        if (!bar_sounds_as(&b->steps[i * SEQ_STEPS], &a->steps[i * SEQ_STEPS])) return 0;
    return 1;
}

static int same_part(const seq_t *a, const seq_t *b) {
    const seq_part_t *p = &a->part[0], *q = &b->part[0];
    if (p->bars != q->bars || p->chord_len != q->chord_len) return 0;
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) {   /* Melody 2 is retired and empty */
        if (p->lane[l].mute != q->lane[l].mute) return 0;
        int conv = l == LANE_CHORDS ? chords_loaded_as(&p->lane[l], &q->lane[l], p->bars)   /* 0.18: a 0.17 lane converted */
                   : l == LANE_BASS ? bass_loaded_as(&p->lane[l], &q->lane[l], p->bars) : -1;   /* 0.19: a 0.18 lane */
        if (conv >= 0) {
            if (!conv) return 0;
            continue;
        }
        for (int i = 0; i < p->bars * SEQ_STEPS; i++) {
            const seq_step_t *x = &p->lane[l].steps[i], *y = &q->lane[l].steps[i];
            if (x->on != y->on || (x->on && (x->tone != y->tone || x->oct != y->oct || x->vel != y->vel || x->len != y->len)))
                return 0;
        }
    }
    return 1;
}

/* 0.18: every lane MANUAL but the Chords lane, which a load makes DRAWN bars (0.19: and the Bass lane) */
static int manual_but_drawn(const seq_t *s) {
    for (int l = LANE_MEL_UP; l < SEQ_LANES; l++)
        if (seq_style(s, l) != GEN_MANUAL) return 0;
    return s->part[s->cur_part].lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && s->part[s->cur_part].lane[LANE_BASS].gen.style == GEN_BASS_DRAWN;
}

/* 0.18: t, loaded, saves back byte for byte (a 0.17 Chords lane saves differently once, as DRAWN bars: save, load, save; 0.19 a
 * 0.18 Bass lane too) */
static int resaves_same(seq_t *t) {
    static seq_t u;
    static char a[SEQ_STATE_MAX], b[SEQ_STATE_MAX];
    int n = seq_state_save(t, a, sizeof a);
    seq_init(&u, emit, NULL);
    return n > 0 && seq_state_load(&u, a) && seq_state_save(&u, b, sizeof b) == n && !strcmp(a, b);
}

/* 0.18: a state's text without its Chords lanes' fields (`<P>1=`, `<P>1.*`, `<P>.rb`, `<P>.rz`), which 0.18 saves as DRAWN
 * bars, and 0.19 its Bass lanes' (`<P>2=`, `<P>2.*`, its SHAPE row `2.bs` too): the rest of a 0.17 state still saves back byte
 * for byte */
static void strip_drawn(char *buf) {
    for (char *f = strchr(buf, '|'); f; f = strchr(f, '|')) {
        int part = f[1] >= 'A' && f[1] <= 'F';
        if (part && (((f[2] == '1' || f[2] == '2') && (f[3] == '=' || f[3] == '.')) || !strncmp(f + 2, ".rb=", 4) || !strncmp(f + 2, ".rz=", 4))) {
            char *e = strchr(f + 1, '|');
            memmove(f, e ? e : f + strlen(f), strlen(e ? e : f + strlen(f)) + 1);
        } else {
            f++;
        }
    }
}

/* 0.18: the state s saves is `want` but for the Chords lanes (0.19: and the Bass lanes; strip_drawn) */
static int saves_but_drawn(seq_t *s, const char *want) {
    static char a[SEQ_STATE_MAX], b[SEQ_STATE_MAX];
    if (seq_state_save(s, a, sizeof a) <= 0) return 0;
    snprintf(b, sizeof b, "%s", want);
    strip_drawn(a);
    strip_drawn(b);
    return !strcmp(a, b);
}

static void test_labels_and_state(void) {
    static seq_t s, t;
    char buf[16];
    static char st[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_reset_v1(&s);   /* v1's lanes, as this test always had */
    seq_cell_label(&s, LANE_CHORDS, 0, buf, sizeof buf);
    CHECK(!strcmp(buf, "Em"), "chords cell in bar 1 reads Em (got %s)", buf);
    seq_cell_label(&s, LANE_MEL_UP, 2, buf, sizeof buf);
    CHECK(!strcmp(buf, "G4"), "melody 1 step 3 reads G4 (got %s)", buf);
    seq_cell_label(&s, LANE_BASS, 1, buf, sizeof buf);
    CHECK(!strcmp(buf, " "), "an off cell is blank");

    seq_set_step(&s, LANE_BASS, 0, 5, 1);
    seq_set_mute(&s, LANE_MEL_DOWN, 1);
    seq_state_save(&s, st, sizeof st);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && same_part(&s, &t),
          "state round trip: grid and mutes restored");
    CHECK(!seq_state_load(&t, "garbage") && !seq_state_load(&t, NULL), "invalid state is refused");
}

static void test_per_bar_edit(void) {
    static seq_t s;
    fresh(&s, 0);
    seq_set_step(&s, LANE_BASS, 1, 4, 1);   /* bar 2, step 5 only */
    play(&s, 0, 16.0, 0);
    seq_block(&s, 16.0, bpm, 0, 128);
    CHECK(count_on(3) == 9, "one extra bass note, in bar 2 only: 2 x 4 + 1 = 9 (got %d)", count_on(3));
    CHECK(balanced(), "no stuck notes");
}

/* note-ons on channel `ch`, in order */
static int ons(int ch, int *out, int max) {
    int n = 0;
    for (int i = 0; i < nev && n < max; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1) out[n++] = evs[i].note;
    return n;
}

static void test_long_loop(void) {
    static seq_t s;
    fresh(&s, 0);
    seq_set_bars(&s, 16);
    play(&s, 0, 63.0, 0);
    CHECK(seq_cur_bar(&s) == 15, "a 16-bar loop reaches bar 16 (got %d)", seq_cur_bar(&s) + 1);
    fresh(&s, 0);
    seq_set_bars(&s, 16);
    play(&s, 0, 127.9, 64.0);   /* two passes of the 64-beat loop */
    seq_block(&s, 63.9, bpm, 0, 128);
    CHECK(count_on(3) == 64, "bass: 2 notes x 16 bars x 2 passes = 64 (got %d)", count_on(3));
    CHECK(balanced(), "no stuck notes across the 16-bar loop wrap");
}

static void test_grow_copies(void) {
    static seq_t s;
    fresh(&s, 0);
    seq_set_step(&s, LANE_BASS, 2, 4, 1);   /* bar 3 */
    seq_set_bars(&s, 8);
    const seq_step_t *st = s.part[0].lane[LANE_BASS].steps;
    CHECK(seq_bars(&s) == 8 && st[6 * SEQ_STEPS + 4].on && !st[5 * SEQ_STEPS + 4].on,
          "growing 4 -> 8 bars repeats bars 1-4 (bar 7 = bar 3)");
    seq_set_bars(&s, 3);
    CHECK(seq_bars(&s) == 8, "3 bars is refused");
    seq_set_view(&s, 7);
    seq_set_bars(&s, 4);
    CHECK(seq_view_bar(&s) == 0, "shrinking past the bar on view shows bar 1");
    seq_set_view(&s, 9);
    CHECK(seq_view_bar(&s) == 0, "a bar past the loop shows bar 1");
}

/* shrinking the loop keeps the hidden bars; growing it back brings them back instead of copies */
static void test_shrink_grow_keeps_edits(void) {
    static seq_t s;
    fresh(&s, 0);
    seq_set_bars(&s, 16);
    seq_set_step(&s, LANE_BASS, 12, 4, 1);   /* bar 13, step 5 */
    seq_set_bars(&s, 8);
    seq_set_bars(&s, 16);
    CHECK(s.part[0].lane[LANE_BASS].steps[12 * SEQ_STEPS + 4].on, "16 -> 8 -> 16 bars keeps the edit in bar 13");
}

static void test_chord_len(void) {
    static seq_t s;
    int n[16], c;
    static const int V1[4] = {0, 5, 2, 6};
    fresh(&s, 0);
    seq_set_chord_len(&s, 8);
    play(&s, 0, 16.0, 0);
    seq_block(&s, 16.0, bpm, 0, 128);
    c = ons(3, n, 16);
    CHECK(c == 8 && n[0] == 40 && n[1] == 40 && n[2] == 36 && n[3] == 36 && n[4] == 43 && n[6] == 38,
          "half-bar chords (0.14: remapped by time): bass steps 1 and 9 play E2 E2 C2 C2 G2 G2 D2 D2 (got %d notes)", c);
    seq_set_progression(&s, 4, V1, NULL, NULL, 0);   /* the phrase picked at the new length: 0.13's half-bar loop */
    nev = 0;
    play(&s, 0, 16.0, 0);
    seq_block(&s, 16.0, bpm, 0, 128);
    c = ons(3, n, 16);
    CHECK(c == 8 && n[0] == 40 && n[1] == 36 && n[2] == 43 && n[3] == 38 && n[4] == 40,
          "the 4-chord phrase over half-bar chords: E2 C2 G2 D2 E2 ... (got %d notes)", c);
    fresh(&s, 0);
    seq_set_chord_len(&s, 32);
    play(&s, 0, 16.0, 0);
    seq_block(&s, 16.0, bpm, 0, 128);
    c = ons(3, n, 16);
    CHECK(c == 8 && n[0] == 40 && n[3] == 40 && n[4] == 43 && n[7] == 43,
          "2-bar chords in a 4-bar loop (0.14: each span keeps its first chord): E2 x4, then G2 x4");
    seq_set_chord_len(&s, 12);
    CHECK(seq_chord_len(&s) == 32, "a chord length of 12 steps is refused");
}

static void test_shrink_while_playing(void) {
    static seq_t s;
    fresh(&s, 0);
    seq_set_bars(&s, 16);
    double ppq = play(&s, 0, 38.0, 0);   /* bar 10 */
    seq_set_bars(&s, 1);
    ppq = play(&s, ppq, 8.0, 0);
    CHECK(seq_cur_bar(&s) == 0, "after shrinking to 1 bar while playing, bar 1 loops");
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes after shrinking while playing");
}

static void test_theory(void) {
    char b[8];
    CHECK(theory_tone_note(4, SCALE_MINOR, 0, TONE_R, 0) == 52 && theory_tone_note(4, SCALE_MINOR, 0, TONE_3, 0) == 55 &&
          theory_tone_note(4, SCALE_MINOR, 0, TONE_5, 0) == 59, "Em in E minor: E3 G3 B3");
    CHECK(theory_tone_note(4, SCALE_MINOR, 5, TONE_3, 0) == 52, "tone 3 of C (degree VI) is E3, a major third");
    CHECK(theory_tone_note(4, SCALE_MINOR, 6, TONE_3, 0) == 54, "tone 3 of D (degree VII) is F#3");
    CHECK(theory_tone_note(4, SCALE_MINOR, 2, TONE_5, 1) == 74, "tone 5 of G an octave up is D5");
    CHECK(theory_tone_note(4, SCALE_MINOR, 0, 9, 0) == 66, "tone 9 of Em is F#4, an octave above tone 2");
    CHECK(theory_tone_note(4, SCALE_MINOR, 0, TONE_R, -1) == 40, "octave -1 puts the root at E2");
    theory_chord_name(4, SCALE_MINOR, 0, b, sizeof b);
    CHECK(!strcmp(b, "Em"), "degree i in E minor is Em (got %s)", b);
    theory_chord_name(4, SCALE_MINOR, 1, b, sizeof b);
    CHECK(!strcmp(b, "F#dim"), "degree ii in E minor is F#dim (got %s)", b);
    theory_chord_name(0, SCALE_MAJOR, 4, b, sizeof b);
    CHECK(!strcmp(b, "G"), "degree V in C major is G (got %s)", b);
    CHECK(theory_note_name(64, b, sizeof b) == 2 && !strcmp(b, "E4"), "MIDI 64 is E4 (got %s)", b);
}

static void test_theory_04(void) {
    int v[3] = {52, 55, 59}, w[3], lo_ok = 1, i, rep;
    const int *p0 = THEORY_PRESETS[0].deg;
    CHECK(theory_degree_pc(2, SCALE_DORIAN, 5) == 11, "degree 6 of D Dorian is B");
    CHECK(theory_degree_pc(7, SCALE_MIXOLYDIAN, 6) == 5, "degree 7 of G Mixolydian is F");
    CHECK(THEORY_NPRESETS == 16, "16 progression presets (got %d)", THEORY_NPRESETS);
    CHECK(THEORY_PRESETS[0].len == 4 && p0[0] == 0 && p0[1] == 5 && p0[2] == 2 && p0[3] == 6, "preset 0 is 1-6-3-7");
    CHECK(THEORY_PRESETS[15].len == 8, "preset 15 has 8 chords");
    CHECK(!strcmp(THEORY_PRESETS[10].name, "2-5-1-1 Jazz") && THEORY_PRESETS[10].len == 4 && THEORY_PRESETS[10].deg[0] == 1 &&
          THEORY_PRESETS[10].deg[1] == 4 && THEORY_PRESETS[10].deg[2] == 0 && THEORY_PRESETS[10].deg[3] == 0,
          "preset 10 is 2-5-1-1 Jazz, four chords");
    CHECK(!strcmp(theory_scale_name(SCALE_MIXOLYDIAN), "Mixolydian"), "scale name Mixolydian");
    CHECK(!strcmp(theory_key_name(1), "C#"), "key 1 is C#");
    theory_voice_next(4, SCALE_MINOR, 5, v, w);
    CHECK(w[0] == 52 && w[1] == 55 && w[2] == 60, "Em to C moves one note: 52 55 60 (got %d %d %d)", w[0], w[1], w[2]);
    memcpy(v, w, sizeof v);
    theory_voice_next(4, SCALE_MINOR, 2, v, w);
    CHECK(w[0] == 50 && w[1] == 55 && w[2] == 59, "C to G: 50 55 59 (got %d %d %d)", w[0], w[1], w[2]);
    memcpy(v, w, sizeof v);
    theory_voice_next(4, SCALE_MINOR, 6, v, w);
    CHECK(w[0] == 50 && w[1] == 54 && w[2] == 57, "G to D: 50 54 57 (got %d %d %d)", w[0], w[1], w[2]);
    v[0] = 52; v[1] = 55; v[2] = 59;
    for (rep = 0; rep < 50; rep++)
        for (i = 0; i < THEORY_PRESETS[15].len; i++) {
            theory_voice_next(4, SCALE_MINOR, THEORY_PRESETS[15].deg[i], v, w);
            memcpy(v, w, sizeof v);
            if (v[0] < 43 || v[0] > 62) lo_ok = 0;
        }
    CHECK(lo_ok, "smooth voicing does not drift out of 43..62 over 400 chords");
}

static void sort3(int *g) {
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (g[j] < g[i]) { int t = g[i]; g[i] = g[j]; g[j] = t; }
}

static void test_default_smooth(void) {
    static const int WANT[4][3] = {{52, 55, 59}, {52, 55, 60}, {50, 55, 59}, {50, 54, 57}};
    static seq_t s;
    int got[16], ok = 1;
    nev = 0;
    seq_init(&s, emit, NULL);
    CHECK(seq_voicing(&s) == VOICING_SMOOTH, "a new instance voices smooth");
    seq_reset_v1(&s);   /* v1's one chord per bar (a new instance's Chords are generated), voiced smooth */
    seq_set_voicing(&s, VOICING_SMOOTH);
    play(&s, 0, 15.9, 0);
    seq_block(&s, 15.9, bpm, 0, 128);
    CHECK(ons(1, got, 16) == 12, "4 bars of chords = 12 note-ons");
    for (int c = 0; c < 4; c++) {
        sort3(got + 3 * c);
        for (int i = 0; i < 3; i++) ok &= got[3 * c + i] == WANT[c][i];
    }
    CHECK(ok, "smooth chords: Em, C (52 55 60), G (50 55 59), D (50 54 57)");
}

static int wrap_move(int (*v)[THEORY_CHORD_MAX], int n) {   /* sum of |voice[0] - voice[n-1]|: the move from the last chord back to the first */
    int sum = 0;
    for (int i = 0; i < 3; i++) sum += v[0][i] > v[n - 1][i] ? v[0][i] - v[n - 1][i] : v[n - 1][i] - v[0][i];
    return sum;
}

/* 0.4.0's single pass: slot 0 the root-position triad, each later slot voiced smooth from the one before */
static void single_pass(int key, int scale, const theory_preset_t *r, int (*v)[THEORY_CHORD_MAX]) {
    theory_triad(key, scale, r->deg[0], v[0]);
    for (int i = 1; i < r->len; i++) theory_voice_next(key, scale, r->deg[i], v[i - 1], v[i]);
}

/* smooth voicing leads slot 0 from the last slot too: over every key, scale and preset the wrap never moves more */
static void test_wrap_voicing(void) {
    static seq_t s;
    int old[8][THEORY_CHORD_MAX], worse = 0, better = 0;
    fresh(&s, 0);
    seq_set_voicing(&s, VOICING_SMOOTH);
    for (int k = 0; k < 12; k++)
        for (int sc = 0; sc < SCALE_COUNT; sc++)
            for (int p = 0; p < THEORY_NPRESETS; p++) {
                const theory_preset_t *r = &THEORY_PRESETS[p];
                if (r->len < 2) continue;
                seq_set_key(&s, k);
                seq_set_scale(&s, sc);
                seq_set_prog_preset(&s, p);
                single_pass(k, sc, r, old);
                worse += wrap_move(s.part[0].voice, r->len) > wrap_move(old, r->len);
                better += wrap_move(s.part[0].voice, r->len) < wrap_move(old, r->len);
            }
    CHECK(worse == 0 && better > 0, "the wrap move is never larger than a single pass's, and smaller in %d (larger in %d)", better, worse);
    seq_set_key(&s, 4);
    seq_set_scale(&s, SCALE_MINOR);
    seq_set_prog_preset(&s, 0);
    CHECK(s.part[0].voice[0][0] == 52 && s.part[0].voice[0][1] == 55 && s.part[0].voice[0][2] == 59,
          "E minor 1-6-3-7: slot 0 stays root-position Em, 52 55 59");
}

/* Every smooth voice table (12 keys x 4 scales x 16 presets) folded into one FNV-1a checksum, a note per byte.
 * Changing this value changes how saved smooth projects sound: bump A.voi instead. */
static void test_voicing_golden(void) {
    static seq_t s;
    unsigned h = 2166136261u;
    fresh(&s, 0);
    seq_set_voicing(&s, VOICING_SMOOTH);
    for (int k = 0; k < 12; k++)
        for (int sc = 0; sc < SCALE_V1_COUNT; sc++)
            for (int p = 0; p < THEORY_NPRESETS; p++) {
                seq_set_key(&s, k);
                seq_set_scale(&s, sc);
                seq_set_prog_preset(&s, p);
                for (int i = 0; i < THEORY_PRESETS[p].len; i++)
                    for (int j = 0; j < 3; j++) h = (h ^ (unsigned char)s.part[0].voice[i][j]) * 16777619u;
            }
    CHECK(h == 0xedf82197u, "smooth voicing checksum over every key, scale and preset (got 0x%08x)", h);
}

static void test_key_scale_prog(void) {
    static seq_t s;
    char nm[8];
    fresh(&s, 0);
    seq_set_key(&s, 0);
    seq_set_scale(&s, SCALE_MAJOR);
    seq_set_prog_preset(&s, 1);
    CHECK(seq_key(&s) == 0 && seq_scale(&s) == SCALE_MAJOR && seq_prog_preset(&s) == 1, "key, scale and preset read back");
    seq_slot_name(&s, 0, nm, sizeof nm);
    CHECK(!strcmp(nm, "C"), "slot 0 is C (got %s)", nm);
    seq_slot_name(&s, 1, nm, sizeof nm);
    CHECK(!strcmp(nm, "G"), "slot 1 is G (got %s)", nm);
    seq_slot_name(&s, 2, nm, sizeof nm);
    CHECK(!strcmp(nm, "Am"), "slot 2 is Am (got %s)", nm);
    seq_slot_name(&s, 3, nm, sizeof nm);
    CHECK(!strcmp(nm, "F"), "slot 3 is F (got %s)", nm);
    seq_set_key(&s, 12);
    seq_set_scale(&s, 7);
    seq_set_prog_preset(&s, 16);
    seq_set_voicing(&s, 2);
    CHECK(seq_key(&s) == 0 && seq_scale(&s) == SCALE_MAJOR && seq_prog_preset(&s) == 1 && seq_voicing(&s) == VOICING_CLOSE,
          "invalid key, scale, preset and voicing change nothing");
}

static void test_eight_chords_quarter_bar(void) {
    static seq_t s;
    int slot[8], ok = 1;
    fresh(&s, 0);
    seq_set_prog_preset(&s, 15);
    seq_set_chord_len(&s, 4);
    seq_set_bars(&s, 1);
    for (int beat = 0; beat < 32; beat++) {
        play(&s, beat, 1.0, 0);
        if (beat < 4) slot[beat] = seq_cur_slot(&s);
    }
    for (int i = 0; i < 4; i++) ok &= slot[i] == i;
    CHECK(ok, "4 chords per bar: slots 0,1,2,3 across bar 1 (got %d %d %d %d)", slot[0], slot[1], slot[2], slot[3]);
    seq_block(&s, 31.9, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes over 8 bars of 8-chord progression");
}

/* named slots out of 8, and whether exactly the first `want` are named */
static int named_slots(const seq_t *s, int want) {
    char nm[8];
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        seq_slot_name(s, i, nm, sizeof nm);
        ok &= (i < want) == (strcmp(nm, " ") != 0);
    }
    return ok;
}

/* a slot the loop never reaches (past bars * 16 / chord_len chords) has a blank tile */
static void test_unreached_slots_blank(void) {
    static seq_t s;
    fresh(&s, 0);
    seq_set_prog_preset(&s, 15);   /* Canon (8) */
    CHECK(named_slots(&s, 4), "Canon in 4 bars of 1-bar chords: slots 1-4 named, 5-8 blank");
    seq_set_bars(&s, 8);
    CHECK(named_slots(&s, 8), "in 8 bars all 8 slots are named");
    seq_set_bars(&s, 4);
    seq_set_chord_len(&s, 8);
    CHECK(named_slots(&s, 8), "in 4 bars of 1/2-bar chords all 8 slots are named");
    seq_set_bars(&s, 1);
    seq_set_chord_len(&s, 32);
    CHECK(named_slots(&s, 1), "a 2-bar chord in a 1-bar loop: slot 1 named, the rest blank");
}

static void test_song_change_while_playing(void) {
    static seq_t s;
    double ppq;
    fresh(&s, 0);
    seq_set_voicing(&s, VOICING_SMOOTH);
    ppq = play(&s, 0, 4.1, 0);   /* the chord fired at beat 4 is still sounding */
    seq_set_key(&s, 2);
    ppq = play(&s, ppq, 1.9, 0);
    seq_set_prog_preset(&s, 15);
    ppq = play(&s, ppq, 2.0, 0);
    seq_set_voicing(&s, VOICING_CLOSE);
    ppq = play(&s, ppq, 2.0, 0);
    seq_set_scale(&s, SCALE_DORIAN);
    ppq = play(&s, ppq, 2.0, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "changing key, progression, voicing and scale while playing leaves no stuck notes");
    CHECK(max_open(2) <= 3, "never more than 3 chord notes open (got %d)", max_open(2));
}

/* a state loaded into a smooth-by-default instance comes back close-voiced: the chords are the triads old projects played */
static void test_loads_keep_close(void) {
    static seq_t s;
    static const char *const LOADS[2] = {
        "csq1:1000000000000000" "1000000010000000" "1010101010101010" "1010101010101010" ":0000",
        "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:0015640" "1015640" "2015640" "3015640|A2=0:|A3=0:|A4=0:"};
    static const int WANT[6] = {52, 55, 59, 48, 52, 55};
    for (int k = 0; k < 2; k++) {
        int got[16], ok = 1;
        nev = 0;
        seq_init(&s, emit, NULL);
        CHECK(seq_state_load(&s, LOADS[k]), "%s state loads", k ? "csq2 without A.voi" : "csq1");
        CHECK(seq_voicing(&s) == VOICING_CLOSE, "%s loads as close voicing", k ? "csq2 without A.voi" : "csq1");
        play(&s, 0, 7.9, 0);
        seq_block(&s, 7.9, bpm, 0, 128);
        CHECK(ons(2, got, 16) == 6, "bars 1-2 play 6 chord notes");
        for (int i = 0; i < 6; i++) ok &= got[i] == WANT[i];
        CHECK(ok, "close triads: Em 52 55 59 then C 48 52 55 (got %d %d %d %d %d %d)", got[0], got[1], got[2], got[3], got[4], got[5]);
    }
}

static void test_setters_rebuild_voicing(void) {
    static seq_t s;
    int got[16], g[3];
    fresh(&s, 0);
    seq_set_key(&s, 0);
    seq_set_scale(&s, SCALE_MAJOR);
    seq_set_prog_preset(&s, 1);   /* C G Am F */
    play(&s, 0, 3.9, 0);
    CHECK(ons(2, got, 16) == 3 && got[0] == 48 && got[1] == 52 && got[2] == 55, "bar 1 plays C3 E3 G3 (got %d %d %d)", got[0], got[1], got[2]);
    seq_set_voicing(&s, VOICING_SMOOTH);
    play(&s, 3.9, 4.0, 0);
    seq_block(&s, 7.9, bpm, 0, 128);
    CHECK(ons(2, got, 16) == 6, "bar 2 plays 3 more chord notes");
    memcpy(g, got + 3, sizeof g);
    sort3(g);
    /* smooth from C {48,52,55}: G's inversions {55,59,62}, {59,62,67}, {62,67,71}, each shifted -12/0/+12 with the lowest in
     * 43..62; {59,62,67} - 12 = {47,50,55} moves 1+2+0 = 3, the least, not the close G triad (55,59,62) */
    CHECK(g[0] == 47 && g[1] == 50 && g[2] == 55, "bar 2 is G voiced smooth from C: 47 50 55 (got %d %d %d)", g[0], g[1], g[2]);
}

static void test_state_v2(void) {
    static seq_t s, t;   /* static: two 48 KB sequencers */
    static char buf[SEQ_STATE_MAX];
    fresh(&s, 0);
    seq_set_bars(&s, 8);
    seq_set_chord_len(&s, 8);
    seq_set_step(&s, LANE_MEL_DOWN, 7, 15, 1);
    seq_set_mute(&s, LANE_BASS, 1);
    int n = seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    CHECK(n > 0 && !strncmp(buf, "csq2|", 5), "state saves as csq2 (%d bytes)", n);
    CHECK(seq_state_load(&t, buf) && same_part(&s, &t), "csq2 round trip: length, chord length, steps and mutes");
    fresh(&s, 0);
    seq_set_bars(&s, 16);
    for (int l = 0; l < SEQ_LANES; l++)
        for (int b = 0; b < 16; b++)
            for (int i = 0; i < SEQ_STEPS; i++) seq_set_step(&s, l, b, i, 1);
    n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && n < SEQ_STATE_MAX, "16 bars with every step on: %d bytes fits the wrapper's %d", n, SEQ_STATE_MAX);
    CHECK(seq_state_load(&t, buf) && same_part(&s, &t), "and loads back");
    CHECK(resaves_same(&t), "save -> load -> save gives the same string (0.18: once loaded, the Chords lane DRAWN bars)");
    CHECK(seq_state_save(&s, buf, 100) == 0, "a buffer too small saves nothing (no truncated state)");
}

/* voicing and the four scales survive a save and a load; a bad A.voi refuses the whole state */
static void test_state_voicing(void) {
    static seq_t s, t, before;
    static char buf[SEQ_STATE_MAX];
    static const char *const BAD[] = {"2", "", "x", "01", "-1", NULL};
    static const char HEAD[] = "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A.voi=";
    static const char TAIL[] = "|A1=0:|A2=0:|A3=0:|A4=0:";
    char bad[160];
    fresh(&s, 0);
    seq_set_key(&s, 2);
    seq_set_scale(&s, SCALE_DORIAN);
    seq_set_prog_preset(&s, 8);
    seq_set_voicing(&s, VOICING_SMOOTH);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|A.prog=") && strstr(buf, "|A.voi=1|ch="), "smooth voicing saves as A.voi=1");
    seq_init(&t, emit, NULL);
    seq_set_voicing(&t, VOICING_CLOSE);
    CHECK(seq_state_load(&t, buf), "a state with A.voi loads");
    CHECK(t.key == 2 && seq_scale(&t) == SCALE_DORIAN && seq_prog_preset(&t) == 8 && seq_voicing(&t) == VOICING_SMOOTH,
          "D Dorian, preset 8 and smooth voicing round trip");
    CHECK(!memcmp(s.part[0].voice, t.part[0].voice, sizeof s.part[0].voice), "and so does the voice table");
    for (int sc = 0; sc < SCALE_COUNT; sc++) {
        seq_set_scale(&s, sc);
        seq_state_save(&s, buf, sizeof buf);
        CHECK(seq_state_load(&t, buf) && seq_scale(&t) == sc, "scale %d round trips", sc);
    }
    fresh(&s, 0);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A.voi=0|ch="), "close voicing saves as A.voi=0");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_voicing(&t) == VOICING_CLOSE, "A.voi=0 loads close into a smooth-by-default instance");
    CHECK(seq_state_load(&t, "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0123|A1=0:|A2=0:|A3=0:|A4=0:") && seq_prog_preset(&t) == -1,
          "a progression that is no preset loads, and reports no preset");
    fresh(&s, 0);
    seq_set_key(&s, 7);
    seq_set_voicing(&s, VOICING_SMOOTH);
    before = s;
    for (int i = 0; BAD[i]; i++) {
        snprintf(bad, sizeof bad, "%s%s%s", HEAD, BAD[i], TAIL);
        CHECK(!seq_state_load(&s, bad), "refused: A.voi=%s", BAD[i]);
    }
    CHECK(!memcmp(&before.part[0], &s.part[0], sizeof s.part[0]) && before.key == s.key && before.scale == s.scale,
          "refused voicing states change nothing");
    snprintf(bad, sizeof bad, "csq2|k=4|s=7|A.len=4|A.cl=16|A.prog=0526|A.voi=0%s", TAIL);
    CHECK(!seq_state_load(&s, bad), "refused: scale 7");
}

static void test_state_v1(void) {
    static seq_t s, t;
    /* v1's default grid with Melody 1 muted, as 0.1 and 0.2 saved it */
    const char *v1 = "csq1:1000000000000000" "1000000010000000" "1010101010101010" "1010101010101010" ":0010";
    fresh(&s, 0);
    seq_set_mute(&s, LANE_MEL_UP, 1);
    seq_init(&t, emit, NULL);
    seq_set_bars(&t, 16);   /* something else first */
    CHECK(seq_state_load(&t, v1) && same_part(&s, &t), "a v1 (csq1) state loads as v1's 4-bar song");
    CHECK(manual_but_drawn(&t), "with every lane MANUAL (the instance had generated ones; 0.18, R19: the Chords lane DRAWN bars; 0.19 "
          "the Bass too)");
    seq_set_harmony(&t, 77);
    seq_generate_progression(&t);
    CHECK(seq_state_load(&t, v1) && seq_harmony(&t) == 30 && t.part[0].harmony_seed == 1, "a csq1 state gives HARMONY 30, seed 1");
    nev = 0;
    play(&t, 0, 16.0, 0);
    seq_block(&t, 16.0, bpm, 0, 128);
    CHECK(count_on(2) == 12 && count_on(3) == 8 && count_on(4) == 0 && count_on(5) == 0,
          "and plays as v1 did less the retired Melody 2, Melody 1 muted (%d %d %d %d)", count_on(2), count_on(3), count_on(4), count_on(5));
}

static void test_state_invalid(void) {
    static seq_t s, before;
    static const char *bad[] = {
        "garbage", "csq3|k=4",
        "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:",                          /* a lane missing */
        "csq2|k=4|s=1|A.len=1|A.cl=16|A.prog=0526|A1=0:1014640|A2=0:|A3=0:|A4=0:",             /* step 17 of 16 */
        "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:0084640|A2=0:|A3=0:|A4=0:",             /* tone 8 */
        "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:001464|A2=0:|A3=0:|A4=0:",              /* a cut-off record */
        "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:0014000|A2=0:|A3=0:|A4=0:",             /* velocity 0 */
        "csq2|k=4|s=1|A.len=3|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:|A4=0:",                    /* 3 bars */
        "csq2|k=12|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:|A4=0:",                   /* key 12 */
        "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=|A1=0:|A2=0:|A3=0:|A4=0:",                        /* no chords */
        "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0527|A1=0:|A2=0:|A3=0:|A4=0:",                    /* degree 7 */
        NULL};
    fresh(&s, 0);
    seq_set_bars(&s, 2);
    before = s;
    for (int i = 0; bad[i]; i++) CHECK(!seq_state_load(&s, bad[i]), "refused: %s", bad[i]);
    CHECK(!seq_state_load(&s, NULL), "refused: NULL");
    CHECK(!memcmp(&before.part[0], &s.part[0], sizeof s.part[0]) && before.key == s.key, "a refused state changes nothing");
    CHECK(seq_state_load(&s, "csq2|k=4|s=1|zz=9|A.len=4|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:|A4=0:"),
          "an unknown field is ignored (a later version's extra setting)");
}

static void test_note_length(void) {
    static seq_t s;
    static char st[400];
    /* Melody 1 (0.19: was the Bass, which a load now makes DRAWN bars), 1-bar part: the root held 4 steps at step 1, the 5th for 1
     * step at step 3 */
    const char *two = "csq2|k=4|s=1|A.len=1|A.cl=16|A.prog=0526|A.voi=0|A1=0:|A2=0:|A3=0:00146430254640|A4=0:";
    /* 0.18: the empty MANUAL Chords lane loads as one DRAWN bar drawn from nothing, all rests; 0.19 the empty Bass lane one DRAWN bar
     * drawn from nothing too */
    const char *two5 = "csq2|gv=1|k=4|s=1|A.len=1|A.cl=16|A.h=30,1|A.prog=0526|A.voi=0|ch=2,3,4|A1=0:|A1.g=17,1,50|A.rb=................"
                       "|A2=0:|A2.g=18,1,50|A2.rb=................|A3=0:00146430254640|A4=0:";
    fresh(&s, 0);
    CHECK(seq_state_load(&s, two), "a state with note lengths loads");
    CHECK(seq_state_save(&s, st, sizeof st) > 0 && !strcmp(st, two5), "and saves back byte for byte, plus gv and A.h (got %s)", st);
    play(&s, 0, 16.0, 0);
    seq_block(&s, 16.0, bpm, 0, 128);
    CHECK(count_on(4) == 8 && max_open(4) == 2, "a 4-step root overlaps the 5th (got %d notes, %d at once)", count_on(4), max_open(4));
    int p = snprintf(st, sizeof st, "csq2|k=4|s=1|A.len=1|A.cl=16|A.prog=0526|A1=0:|A2=0:");
    for (int i = 0; i < SEQ_STEPS; i++) p += snprintf(st + p, sizeof st - p, "%02x1464f", i);
    snprintf(st + p, sizeof st - p, "|A3=0:|A4=0:");
    fresh(&s, 0);
    CHECK(seq_state_load(&s, st), "16 held roots load");
    play(&s, 0, 4.0, 0);
    seq_block(&s, 4.0, bpm, 0, 128);
    CHECK(max_open(3) == 1 && balanced(), "a held note retriggered by the next step ends first (%d at once)", max_open(3));
}

/* 0.5: generated lanes */

/* what the generator writes for a lane of the playing part, from the lane's style, seed and complexity; an Answer on
 * Melody 2 follows Melody 1's steps as they are now */
static void gen_ref(const seq_t *s, int lane, seq_step_t *out) {
    const seq_part_t *p = &s->part[0];
    const seq_gen_t *g = &p->lane[lane].gen;
    const seq_step_t *lead = lane == LANE_MEL_DOWN && g->style == GEN_MEL_ANSWER ? p->lane[LANE_MEL_UP].steps : NULL;
    gen_params_t gp = {g->style, g->seed, g->complexity, p->bars, p->chord_len, p->prog, p->prog_len, s->key, s->scale, lead, lane, g->variant, p->lane[lane].bar_var};
    gen_lane_v1(&gp, out);
}

static int locked(const seq_t *s, int lane, int i) { return s->part[0].lane[lane].lock[i / 8] >> (i % 8) & 1; }

/* the lane holds what the generator writes, except at locked steps */
static int lane_is_gen(const seq_t *s, int lane) {
    static seq_step_t ref[SEQ_LANE_STEPS];
    gen_ref(s, lane, ref);
    for (int i = 0; i < s->part[0].bars * SEQ_STEPS; i++)
        if (!locked(s, lane, i) && memcmp(&s->part[0].lane[lane].steps[i], &ref[i], sizeof ref[i])) return 0;
    return 1;
}

static int same_lane(const seq_t *s, int lane, const seq_lane_t *ln) { return !memcmp(&s->part[0].lane[lane], ln, sizeof *ln); }

/* the notes the loop plays are the same (the steps only, not the generator settings) */
static int same_steps(const seq_t *s, int lane, const seq_lane_t *ln) {
    return !memcmp(s->part[0].lane[lane].steps, ln->steps, sizeof(seq_step_t) * (size_t)(s->part[0].bars * SEQ_STEPS));
}

/* 0.18: a new instance's Chords lane is drawn bars (GEN_CH_DRAWN). The tests of the generator's chord rhythms, which old
 * projects still load with, put back 0.17's new-instance lane: Pulse c30, seed 1, generated (no undo step). */
static void old_chords(seq_t *s) {
    s->part[s->cur_part].lane[LANE_CHORDS].gen = (seq_gen_t){GEN_CH_PULSE, 1, 30, 0};
    seq_set_complexity(s, LANE_CHORDS, 31);
    seq_set_complexity(s, LANE_CHORDS, 30);   /* regenerates */
}

/* 0.19: a new instance's Bass lane is drawn bars with the note model (GEN_BASS_DRAWN). The tests of the Bass generator (its
 * styles, locks, GEN, SHAPE transforms, FIX and moves), which old projects still load with, put back 0.18's lane: Root at
 * COMPLEXITY cx (a new instance's 30, INIT's 0), seed 1, generated (no undo step). */
static void old_bass(seq_t *s, int cx) {
    s->part[s->cur_part].lane[LANE_BASS].gen = (seq_gen_t){GEN_BASS_ROOT, 1, cx + 1, 0};
    seq_set_complexity(s, LANE_BASS, cx);   /* regenerates */
}

/* 0.20: a new instance's Melody lane is JAM (GEN_MEL_JAM, the melody model). The tests of the classic Melody (Hook, Arp, the
 * recipe), which old projects still load with, put back 0.19's lane: Hook at COMPLEXITY 30, seed 1, generated, the Hook recipe
 * (no undo step) */
static void old_mel(seq_t *s) {
    seq_part_t *p = &s->part[s->cur_part];
    memset(&p->mel, 0, sizeof p->mel);
    mel_defaults(MEL_HOOK, &p->mel, NULL);
    p->lane[LANE_MEL_UP].gen = (seq_gen_t){GEN_MEL_HOOK, 1, 31, 0};
    seq_set_complexity(s, LANE_MEL_UP, 30);   /* regenerates */
}

/* 0.20: INIT's Melody is JAM (test_20_init_mel); 0.19's INIT Melody put back: the Up-down Arp recipe at its COMPLEXITY 30 and
 * GATE 40 (no undo step) */
static void old_init_mel(seq_t *s) {
    seq_part_t *p = &s->part[s->cur_part];
    mel_defaults(MEL_ARP, &p->mel, NULL);
    p->mel.opt[0] = 2;   /* Up-down */
    p->lane[LANE_MEL_UP].gen = (seq_gen_t){GEN_MEL_RECIPE, 1, 31, 0};
    seq_set_complexity(s, LANE_MEL_UP, 30);   /* regenerates */
}

static int mel_is_model(const seq_t *s);   /* 0.20 Task 3's: the JAM lane plays the melody model */

/* 0.18: drawn bar `bar` is library rhythm `rhythm` */
static int bar_is(const seq_t *s, int bar, int rhythm) {
    unsigned char c[16], want[16];
    seq_bar_codes(s, LANE_CHORDS, bar, c);
    rhythm_codes(rhythm, want);
    return !memcmp(c, want, 16);
}

/* Renders every bar of the Chords lane after a test pokes rk or a bar lock (0.18 Task 4: a COMPLEXITY turn renders only the
 * bars whose hits change): a key change and back re-renders a drawn lane (gen_reads_harmony) */
static void render_now(seq_t *s) {
    int k = seq_key(s);
    seq_set_key(s, (k + 1) % 12);
    seq_set_key(s, k);
}

/* The bar's steps as the drawn codes render with no knob */
static int bar_plays_drawn(const seq_t *s, int bar) {
    seq_step_t want[16];
    unsigned char c[16];
    seq_bar_codes(s, LANE_CHORDS, bar, c);
    rhythm_to_steps(c, want);
    return !memcmp(&s->part[s->cur_part].lane[LANE_CHORDS].steps[bar * SEQ_STEPS], want, sizeof want);
}

/* plays on from ppq up to (not including) `until`; returns the first bass note-on, 0 if none */
static int play_to(seq_t *s, double *ppq, double until) {
    int at = nev;
    for (; *ppq < until - 1e-9; *ppq += blk()) seq_block(s, *ppq, bpm, 1, 128);
    for (int i = at; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == 1) return evs[i].note;
    return 0;
}

static void test_gen_default(void) {
    static seq_t s;
    int first[4], ok;
    double ppq = 0;
    nev = 0;
    seq_init(&s, emit, NULL);
    CHECK(s.part[0].lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_style(&s, LANE_CHORDS) == GEN_CH_SUSTAIN &&
          seq_complexity(&s, LANE_CHORDS) == 50 && seq_style(&s, LANE_BASS) == GEN_BASS_DRAWN && seq_complexity(&s, LANE_BASS) == 50 &&
          s.part[0].lane[LANE_BASS].gen.seed == 1, "a new instance: Chords drawn Sustain c50 (0.18), Bass drawn c50 (0.19), seed 1");
    const seq_lane_t *m1 = &s.part[0].lane[LANE_MEL_UP], *m2 = &s.part[0].lane[LANE_MEL_DOWN];
    CHECK(m1->gen.style == GEN_MEL_JAM && m1->gen.complexity == 10 && m1->gen.seed == 1 && m2->gen.style == GEN_MANUAL,
          "Melody 1 JAM c10 (0.20), seed 1; Melody 2 retired (Manual)");
    CHECK(bar_plays_drawn(&s, 0) && bar_plays_drawn(&s, 3) && seq_bar_hits(&s, LANE_BASS, 0) == 1 && seq_bar_hits(&s, LANE_BASS, 3) == 1 &&
          mel_is_model(&s), "and the three active lanes hold their steps: the Chords and Bass their drawn bars, the Melody the melody model's");
    int n1 = 0;
    for (int i = 0; i < 64; i++) n1 += m1->steps[i].on;
    CHECK(n1 > 0 && !any_on(m2), "Melody 1 has %d notes, Melody 2 none", n1);
    CHECK(!seq_can_undo(&s) && seq_harmony(&s) == 30 && s.part[0].harmony_seed == 1, "nothing to undo; HARMONY 30, seed 1");
    static char st[SEQ_STATE_MAX];
    CHECK(seq_state_save(&s, st, sizeof st) > 0 && strstr(st, "|A.h=30,1|"), "and it saves as A.h=30,1");
    for (int b = 0; b < 4; b++) first[b] = play_to(&s, &ppq, 4.0 * (b + 1));
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(count_on(3) >= 4, "4 bars play %d bass note-ons (at least 4)", count_on(3));
    ok = first[0] == 28 && first[1] == 36 && first[2] == 31 && first[3] == 38;
    CHECK(ok, "each bar's first bass note is its root (0.19: REGISTER Natural): E1 C2 G1 D2 (got %d %d %d %d)", first[0], first[1], first[2], first[3]);
    CHECK(balanced(), "no stuck notes");
}

static void test_gen_styles(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    old_chords(&s);
    old_bass(&s, 30);
    old_mel(&s);
    seq_set_style(&s, LANE_MEL_UP, GEN_CH_PULSE);
    seq_set_style(&s, LANE_CHORDS, GEN_BASS_ROOT);
    seq_set_style(&s, LANE_BASS, GEN_CH_STABS);
    seq_set_style(&s, LANE_BASS, GEN_NSTYLES);
    seq_set_style(&s, 4, GEN_MANUAL);
    seq_set_style(&s, LANE_MEL_UP, GEN_MEL_ANSWER);
    seq_set_style(&s, LANE_MEL_DOWN, GEN_MEL_HOOK);
    seq_set_style(&s, LANE_CHORDS, GEN_MEL_HOOK);
    CHECK(seq_style(&s, LANE_MEL_UP) == GEN_MEL_HOOK && seq_style(&s, LANE_MEL_DOWN) == GEN_MANUAL && seq_style(&s, LANE_CHORDS) == GEN_CH_PULSE &&
          seq_style(&s, LANE_BASS) == GEN_BASS_ROOT, "a style from another lane (Answer on Melody 1, anything on retired Melody 2) or an unknown one is ignored");
    CHECK(!seq_can_undo(&s), "and pushes nothing");
    seq_set_complexity(&s, LANE_BASS, 101);
    seq_set_complexity(&s, LANE_BASS, -1);
    CHECK(seq_complexity(&s, LANE_BASS) == 30, "complexity outside 0..100 is ignored");
    for (int st = GEN_CH_SUSTAIN; st <= GEN_CH_STABS; st++) {
        seq_set_style(&s, LANE_CHORDS, st);
        CHECK(seq_style(&s, LANE_CHORDS) == st && lane_is_gen(&s, LANE_CHORDS), "Chords style %d regenerates the lane", st);
    }
    for (int st = GEN_BASS_ROOT; st <= GEN_BASS_WALK; st++) {
        seq_set_style(&s, LANE_BASS, st);
        CHECK(seq_style(&s, LANE_BASS) == st && lane_is_gen(&s, LANE_BASS), "Bass style %d regenerates the lane", st);
    }
}

/* complexity develops the lane and turning it back restores it exactly */
static void test_gen_develop(void) {
    static seq_t s;
    static seq_lane_t before[2];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    old_bass(&s, 30);
    for (int l = 0; l < 2; l++) {
        memcpy(&before[l], &s.part[0].lane[l], sizeof before[l]);
        seq_set_complexity(&s, l, 80);
        CHECK(!same_steps(&s, l, &before[l]) && lane_is_gen(&s, l), "lane %d at complexity 80 is regenerated", l + 1);
        seq_set_complexity(&s, l, 30);
        CHECK(same_lane(&s, l, &before[l]), "lane %d: complexity 30 -> 80 -> 30 restores identical steps", l + 1);
    }
    CHECK(!seq_can_undo(&s), "complexity changes push no undo");
}

static void test_gen_locks(void) {
    static seq_t s;
    static seq_lane_t before;
    static seq_step_t ref[SEQ_LANE_STEPS];
    seq_step_t x;
    seq_init(&s, emit, NULL);
    old_bass(&s, 30);
    const seq_step_t *st = s.part[0].lane[LANE_BASS].steps;
    seq_set_step(&s, LANE_BASS, 0, 0, 1);   /* already on: nothing changes */
    CHECK(!locked(&s, LANE_BASS, 0) && !seq_can_undo(&s), "tapping a step to the state it has locks nothing, pushes nothing");
    CHECK(!st[2].on, "bass step 3 of bar 1 is off in Root c30");
    seq_set_step(&s, LANE_BASS, 0, 2, 1);
    x = st[2];
    CHECK(x.on && x.tone == TONE_R && x.oct == -1 && x.vel == 100 && x.len == 1 && locked(&s, LANE_BASS, 2),
          "a tap turns it on as the root, octave -1, vel 100, length 1, and locks it");
    seq_generate(&s, LANE_BASS);
    CHECK(!memcmp(&st[2], &x, sizeof x) && locked(&s, LANE_BASS, 2) && lane_is_gen(&s, LANE_BASS), "GEN keeps step 3 and its lock");
    seq_set_complexity(&s, LANE_BASS, 90);
    CHECK(!memcmp(&st[2], &x, sizeof x) && locked(&s, LANE_BASS, 2) && lane_is_gen(&s, LANE_BASS), "so does a complexity change");
    seq_set_step(&s, LANE_BASS, 1, 0, 0);   /* bar 2's downbeat, a generated onset */
    CHECK(!st[16].on && locked(&s, LANE_BASS, 16), "a tap turns a generated onset off and locks it");
    seq_set_complexity(&s, LANE_BASS, 30);
    CHECK(!st[16].on, "and it stays off through a regeneration");
    gen_ref(&s, LANE_BASS, ref);
    seq_set_step(&s, LANE_BASS, 1, 0, 1);
    CHECK(!memcmp(&st[16], &ref[16], sizeof ref[16]), "turned back on, it takes the generated step's values");
    seq_set_step(&s, LANE_CHORDS, 0, 1, 1);
    CHECK(s.part[0].lane[LANE_CHORDS].steps[1].oct == 0, "a tapped Chords step sits at octave 0");
    memcpy(&before, &s.part[0].lane[LANE_BASS], sizeof before);
    seq_set_style(&s, LANE_BASS, GEN_MANUAL);
    int nolock = 1;
    for (int i = 0; i < SEQ_LANE_STEPS / 8; i++) nolock &= s.part[0].lane[LANE_BASS].lock[i] == 0;
    CHECK(seq_style(&s, LANE_BASS) == GEN_MANUAL && nolock && !memcmp(st, before.steps, sizeof before.steps),
          "switching to MANUAL clears the locks and keeps the steps");
    seq_set_bars(&s, 8);
    CHECK(!memcmp(&st[64], before.steps, 64 * sizeof *st), "a MANUAL lane is never regenerated (grow repeats it)");
}

/* a MANUAL tap on Chords or Bass changes only its step: a generated bar switched to MANUAL keeps the rest */
static void test_manual_tap_one_step(void) {
    static seq_t s;
    static seq_lane_t before;
    seq_init(&s, emit, NULL);
    seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
    seq_set_complexity(&s, LANE_BASS, 80);
    seq_set_style(&s, LANE_BASS, GEN_MANUAL);
    memcpy(&before, &s.part[0].lane[LANE_BASS], sizeof before);
    const seq_step_t *st = s.part[0].lane[LANE_BASS].steps;
    int i = 0, kept = 1;
    while (st[i].on) i++;   /* an off step in bar 1 */
    seq_set_step(&s, LANE_BASS, 0, i, 1);
    for (int j = 0; j < SEQ_STEPS; j++)
        if (j != i) kept &= !memcmp(&st[j], &before.steps[j], sizeof st[j]);
    CHECK(i < SEQ_STEPS && kept, "a MANUAL tap on bass step %d keeps every other step's tone, octave, velocity and length", i + 1);
    CHECK(st[i].on && st[i].tone == TONE_R && st[i].oct == -1 && st[i].vel == 100 && st[i].len == 1,
          "and turns its step on as the root, octave -1, vel 100, length 1");
    int on = 0;   /* a generated onset in bar 1 that is not what v1's recipe would write */
    while (on < SEQ_STEPS && (!st[on].on || on == i || (st[on].tone == TONE_R && st[on].vel == 100 && st[on].len == 1))) on++;
    CHECK(on < SEQ_STEPS, "bar 1 has a generated onset unlike the recipe (step %d)", on + 1);
    seq_step_t was = st[on];
    seq_set_step(&s, LANE_BASS, 0, on, 0);
    was.on = 0;
    CHECK(!memcmp(&st[on], &was, sizeof was), "turning a step off only clears it");
    seq_set_step(&s, LANE_BASS, 0, on, 1);
    CHECK(st[on].tone == TONE_R && st[on].oct == -1 && st[on].vel == 100 && st[on].len == 1, "turned back on, it is the root again");
}

/* a lock follows the bars a grow repeats */
static void test_gen_grow_locks(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    old_bass(&s, 30);
    seq_set_step(&s, LANE_BASS, 2, 5, 1);   /* bar 3 */
    seq_set_bars(&s, 8);
    CHECK(locked(&s, LANE_BASS, 2 * 16 + 5) && locked(&s, LANE_BASS, 6 * 16 + 5) && !locked(&s, LANE_BASS, 5 * 16 + 5),
          "growing 4 -> 8 bars repeats the lock (bar 7 = bar 3)");
    CHECK(s.part[0].lane[LANE_BASS].steps[6 * 16 + 5].on && lane_is_gen(&s, LANE_BASS), "and the tapped note, the rest regenerated");
}

static void test_gen_undo(void) {
    static seq_t s, before_all;
    static seq_lane_t lane[11];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    old_bass(&s, 30);
    memcpy(&before_all, &s, sizeof s);
    seq_undo(&s);
    CHECK(!memcmp(&before_all.part[0], &s.part[0], sizeof s.part[0]) && !seq_can_undo(&s), "undo with nothing pushed changes nothing");
    memcpy(&lane[0], &s.part[0].lane[LANE_BASS], sizeof lane[0]);
    seq_generate(&s, LANE_BASS);
    CHECK(seq_can_undo(&s) && !same_steps(&s, LANE_BASS, &lane[0]) && lane_is_gen(&s, LANE_BASS), "GEN pushes and makes new notes");
    seq_set_mute(&s, LANE_BASS, 1);
    seq_undo(&s);
    CHECK(!seq_can_undo(&s) && seq_mute(&s, LANE_BASS), "undo keeps the lane's mute");
    seq_set_mute(&s, LANE_BASS, 0);
    CHECK(same_lane(&s, LANE_BASS, &lane[0]), "GEN then UNDO restores the previous lane exactly");
    for (int i = 1; i <= 10; i++) {
        seq_generate(&s, LANE_BASS);
        memcpy(&lane[i], &s.part[0].lane[LANE_BASS], sizeof lane[i]);
    }
    int ok = 1;
    for (int k = 1; k <= 10; k++) {
        seq_undo(&s);
        ok &= same_lane(&s, LANE_BASS, &lane[k <= 8 ? 10 - k : 2]);
    }
    CHECK(ok && !seq_can_undo(&s), "10 GENs then 10 UNDOs: 8 restore the states before, the last 2 change nothing");
    memcpy(&lane[0], &s.part[0].lane[LANE_BASS], sizeof lane[0]);
    seq_set_step(&s, LANE_BASS, 0, 2, 1);
    seq_undo(&s);
    CHECK(same_lane(&s, LANE_BASS, &lane[0]), "a tap on a generated lane undoes, lock and all");
    memcpy(&lane[0], &s.part[0].lane[LANE_CHORDS], sizeof lane[0]);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_STABS);
    seq_undo(&s);
    CHECK(same_lane(&s, LANE_CHORDS, &lane[0]) && seq_style(&s, LANE_CHORDS) == GEN_CH_PULSE, "a style change undoes");
    seq_set_style(&s, LANE_MEL_UP, GEN_MANUAL);   /* Melody 1 is a new instance's Hook */
    int pushed = s.undo_n;
    seq_set_step(&s, LANE_MEL_UP, 0, 1, 1);
    seq_generate(&s, LANE_MEL_UP);
    CHECK(s.undo_n == pushed, "a tap on a MANUAL lane and GEN on a MANUAL lane push nothing");
    seq_generate(&s, LANE_BASS);
    seq_state_load(&s, "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:|A4=0:");
    CHECK(!seq_can_undo(&s), "a state load clears the undo history");
}

/* NEW IDEA (seq_generate_all) is one undo step for all three lanes; the ring keeps whole ideas only */
static void test_new_idea_one_undo(void) {
    static seq_t s, before, idea[3];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    old_bass(&s, 30);
    old_mel(&s);
    memcpy(&before, &s, sizeof s);
    seq_generate_all(&s);
    CHECK(!same_steps(&s, LANE_CHORDS, &before.part[0].lane[LANE_CHORDS]) && !same_steps(&s, LANE_BASS, &before.part[0].lane[LANE_BASS]) &&
          !same_steps(&s, LANE_MEL_UP, &before.part[0].lane[LANE_MEL_UP]), "NEW IDEA makes new Chords, Bass and Melody");
    seq_undo(&s);
    CHECK(!memcmp(&before.part[0], &s.part[0], sizeof s.part[0]) && !seq_can_undo(&s),
          "one UNDO restores Chords, Bass and Melody exactly, and leaves nothing to undo");
    for (int i = 0; i < 3; i++) {   /* 9 snapshots: the ring's 8 drop the first idea's head, so its rest goes too */
        seq_generate_all(&s);
        memcpy(&idea[i], &s, sizeof s);
    }
    seq_undo(&s);
    int ok = !memcmp(&idea[1].part[0], &s.part[0], sizeof s.part[0]);
    seq_undo(&s);
    ok &= !memcmp(&idea[0].part[0], &s.part[0], sizeof s.part[0]);
    CHECK(ok && !seq_can_undo(&s), "3 ideas: 2 undo whole, the first (cut by the ring) is gone, not half undone");
    seq_generate(&s, LANE_BASS);
    seq_generate_all(&s);
    seq_undo(&s);
    seq_undo(&s);
    CHECK(!memcmp(&idea[0].part[0], &s.part[0], sizeof s.part[0]), "a lane GEN before an idea is its own undo step");
    seq_set_style(&s, LANE_BASS, GEN_MANUAL);
    seq_set_style(&s, LANE_CHORDS, GEN_MANUAL);
    int pushed = s.undo_n;
    seq_generate_all(&s);
    CHECK(s.undo_n == pushed + 1, "Manual lanes are skipped (one snapshot for Melody alone)");
    seq_undo(&s);
    CHECK(s.undo_n == pushed, "and one UNDO takes it back");
}

/* UNDO restores the lane's locks, seed and style but keeps its complexity, as it keeps mute: knob moves aren't undoable */
static void test_gen_undo_keeps_complexity(void) {
    static seq_t s;
    static seq_step_t ref[SEQ_LANE_STEPS];
    seq_init(&s, emit, NULL);
    old_bass(&s, 30);
    seq_set_step(&s, LANE_BASS, 0, 2, 1);   /* a tap at c30 */
    seq_set_complexity(&s, LANE_BASS, 70);
    seq_undo(&s);
    int nolock = 1;
    for (int i = 0; i < SEQ_LANE_STEPS / 8; i++) nolock &= s.part[0].lane[LANE_BASS].lock[i] == 0;
    const seq_part_t *p = &s.part[0];
    gen_params_t gp = {GEN_BASS_ROOT, 1, 70, p->bars, p->chord_len, p->prog, p->prog_len, s.key, s.scale, NULL, LANE_BASS, 0, NULL};
    gen_lane_v1(&gp, ref);
    CHECK(seq_complexity(&s, LANE_BASS) == 70 && nolock && seq_style(&s, LANE_BASS) == GEN_BASS_ROOT &&
          s.part[0].lane[LANE_BASS].gen.seed == 1, "tap at c30, knob to 70, UNDO: complexity stays 70, the tap's lock is gone");
    CHECK(!memcmp(s.part[0].lane[LANE_BASS].steps, ref, sizeof(seq_step_t) * 64), "and the lane is a fresh generation at c70");
}

static int bar_ons(const seq_t *s, int lane, int bar) {
    int n = 0;
    for (int i = 0; i < SEQ_STEPS; i++) n += s->part[0].lane[lane].steps[bar * SEQ_STEPS + i].on;
    return n;
}

/* an undo after a grow repeats the restored lane into the bars the grow added, as the grow itself does */
static void test_gen_undo_across_grow(void) {
    static seq_t s;
    static const char *const V1 = "csq1:1000000000000000100000001000000010101010101010101010101010101010:0000";
    const seq_lane_t *ln = &s.part[0].lane[LANE_BASS];
    int silent = 0;
    seq_init(&s, emit, NULL);
    seq_state_load(&s, V1);
    v1_bass(&s);   /* 0.19: the load makes it DRAWN bars */
    seq_set_style(&s, LANE_BASS, GEN_BASS_PUMP);
    seq_set_bars(&s, 8);
    seq_undo(&s);
    for (int b = 0; b < 8; b++) silent += bar_ons(&s, LANE_BASS, b) == 0;
    CHECK(seq_style(&s, LANE_BASS) == GEN_MANUAL && seq_bars(&s) == 8, "csq1, Bass Pump, 4 -> 8 bars, UNDO: Bass is MANUAL, 8 bars");
    CHECK(!memcmp(&ln->steps[64], ln->steps, 64 * sizeof(seq_step_t)) && !silent, "bars 5-8 repeat bars 1-4 (%d silent bars)", silent);
    seq_init(&s, emit, NULL);
    old_bass(&s, 30);
    seq_set_step(&s, LANE_BASS, 0, 2, 1);   /* a lock in bar 1 */
    seq_generate(&s, LANE_BASS);            /* the snapshot holds it */
    seq_set_bars(&s, 8);
    seq_generate(&s, LANE_BASS);
    seq_undo(&s);
    seq_undo(&s);
    CHECK(locked(&s, LANE_BASS, 2) && locked(&s, LANE_BASS, 4 * 16 + 2) && ln->steps[4 * 16 + 2].on && lane_is_gen(&s, LANE_BASS),
          "a generated snapshot's lock repeats into the bars added since, the rest regenerated");
}

/* an old project's lane switched to a style starts where a new instance does */
static void test_manual_gen_defaults(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_state_load(&s, "csq1:1000000000000000100000001000000010101010101010101010101010101010:0000");
    v1_bass(&s);   /* 0.19: the load makes it DRAWN bars */
    seq_set_style(&s, LANE_BASS, GEN_BASS_ROOT);
    CHECK(seq_complexity(&s, LANE_BASS) == 30 && s.part[0].lane[LANE_BASS].gen.seed == 1, "csq1 -> Bass Root: c30, seed 1 (got c%d, seed %d)",
          seq_complexity(&s, LANE_BASS), s.part[0].lane[LANE_BASS].gen.seed);
    seq_state_load(&s, "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:|A4=0:");
    seq_set_style(&s, LANE_CHORDS, GEN_CH_PULSE);
    CHECK(s.part[0].lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_complexity(&s, LANE_CHORDS) == 50 &&
          s.part[0].lane[LANE_CHORDS].gen.seed == 1 && bar_is(&s, 0, 3) && bar_is(&s, 3, 3),
          "csq2 -> Chords Pulse: DRAWN Pulse bars at c50 (0.18: a load converts the MANUAL lane), seed 1");
    seq_reset_v1(&s);
    CHECK(seq_complexity(&s, LANE_MEL_UP) == 30 && s.part[0].lane[LANE_MEL_UP].gen.seed == 1, "v1's lanes carry c30, seed 1 too");
}

static int prog_is(const seq_t *s, const int *want, int n) {
    return s->part[0].prog_len == n && !memcmp(s->part[0].prog, want, sizeof(int) * (size_t)n);
}

static void test_gen_harmony(void) {
    static seq_t s;
    int old[4], ok = 1, first2;
    double ppq;
    nev = 0;
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);   /* the walk's approach notes follow the progression */
    seq_set_complexity(&s, LANE_BASS, 80);
    seq_set_harmony(&s, 0);
    seq_set_harmony(&s, 101);
    CHECK(seq_harmony(&s) == 0, "HARMONY outside 0..100 is ignored");
    memcpy(old, s.part[0].prog, sizeof old);
    seq_generate_progression(&s);
    const seq_part_t *p = &s.part[0];
    for (int i = 0; i < p->prog_len; i++) ok &= p->prog[i] == 0 || p->prog[i] == 3 || p->prog[i] == 4 || p->prog[i] == 5;
    CHECK(p->prog_len == 4 && p->prog[0] == 0 && ok, "HARMONY 0: 4 chords from I IV V vi, starting on I (%d %d %d %d)",
          p->prog[0], p->prog[1], p->prog[2], p->prog[3]);
    CHECK(lane_is_gen(&s, LANE_BASS) && lane_is_gen(&s, LANE_CHORDS), "the generated lanes regenerate over the new progression");
    ppq = 0;
    play_to(&s, &ppq, 4.0);
    first2 = play_to(&s, &ppq, 8.0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(first2 == theory_tone_note(4, SCALE_MINOR, p->prog[1], TONE_R, -1), "bar 2's first bass note is the root of the new chord 2 (%d)", first2);
    seq_undo(&s);
    CHECK(prog_is(&s, old, 4) && lane_is_gen(&s, LANE_BASS) && seq_style(&s, LANE_BASS) == GEN_BASS_WALK,
          "UNDO restores the progression and regenerates over it");
    seq_state_load(&s, "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0|A1=0:|A2=0:|A3=0:|A4=0:");
    seq_generate_progression(&s);
    CHECK(s.part[0].phrase == 2 && s.part[0].prog_len == 4 && s.part[0].prog[0] == 0, "a 1-chord progression generates 2 chords");
    seq_state_load(&s, "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0123456012345601|A1=0:|A2=0:|A3=0:|A4=0:");
    seq_generate_progression(&s);
    CHECK(s.part[0].phrase == 8, "a 16-chord progression generates 8 chords");
}

/* picking a preset over a Custom (generated) progression can be undone; browsing presets pushes nothing */
static void test_prog_preset_undo(void) {
    static seq_t s;
    int gen[SEQ_MAX_PROG], n, tries = 0;
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_prog_preset(&s, 1);
    seq_set_prog_preset(&s, 2);
    CHECK(!seq_can_undo(&s) && seq_prog_preset(&s) == 2, "preset -> preset pushes no undo");
    do seq_generate_progression(&s);
    while (seq_prog_preset(&s) >= 0 && ++tries < 20);
    n = s.part[0].prog_len;
    memcpy(gen, s.part[0].prog, sizeof gen);
    seq_set_prog_preset(&s, 0);
    CHECK(seq_prog_preset(&s) == 0, "HARMONY GEN gave a Custom progression; a preset replaced it");
    seq_undo(&s);
    CHECK(prog_is(&s, gen, n) && seq_prog_preset(&s) < 0 && lane_is_gen(&s, LANE_CHORDS),
          "UNDO restores the generated progression and regenerates over it");
}

/* every input of a generated lane regenerates it */
static void test_gen_regen_on_change(void) {
    static seq_t s;
    const seq_step_t *st = s.part[0].lane[LANE_BASS].steps;
    seq_init(&s, emit, NULL);
    old_chords(&s);
    old_bass(&s, 30);
    seq_set_complexity(&s, LANE_BASS, 50);
    seq_set_chord_len(&s, 8);
    CHECK(st[8].on && lane_is_gen(&s, LANE_BASS), "c50, 1/2-bar chords: a bass onset on step 9, the chord change");
    seq_set_chord_len(&s, 4);
    CHECK(st[0].on && st[4].on && st[8].on && st[12].on && lane_is_gen(&s, LANE_BASS), "1/4-bar chords: an onset on each of the 4 changes");
    seq_set_bars(&s, 2);
    CHECK(lane_is_gen(&s, LANE_BASS) && lane_is_gen(&s, LANE_CHORDS), "a length change regenerates");
    seq_set_bars(&s, 16);
    CHECK(lane_is_gen(&s, LANE_BASS) && lane_is_gen(&s, LANE_CHORDS), "so does a grow to 16 bars");
    seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
    seq_set_complexity(&s, LANE_BASS, 90);
    seq_set_prog_preset(&s, 5);
    CHECK(lane_is_gen(&s, LANE_BASS), "a progression preset regenerates");
    seq_set_key(&s, 9);
    CHECK(lane_is_gen(&s, LANE_BASS), "a key change regenerates");
    seq_set_scale(&s, SCALE_DORIAN);
    CHECK(lane_is_gen(&s, LANE_BASS), "a scale change regenerates");
}

static unsigned rnd(unsigned *r, unsigned m) { *r = *r * 1664525u + 1013904223u; return (*r >> 8) % m; }

/* random generated (or MANUAL) lanes over a random loop, chords, key and scale, with taps that lock steps */
static void random_gen_state(seq_t *s, unsigned *r) {
    seq_init(s, emit, NULL);
    old_chords(s);
    seq_set_bars(s, 1 << rnd(r, 5));
    seq_set_chord_len(s, 4 << rnd(r, 4));
    seq_set_prog_preset(s, (int)rnd(r, THEORY_NPRESETS));
    seq_set_key(s, (int)rnd(r, 12));
    seq_set_scale(s, (int)rnd(r, SCALE_COUNT));
    seq_set_style(s, LANE_CHORDS, (int)rnd(r, 4));
    seq_set_style(s, LANE_BASS, rnd(r, 4) ? GEN_BASS_ROOT + (int)rnd(r, 3) : GEN_MANUAL);
    for (int l = 0; l < 2; l++) {
        seq_set_complexity(s, l, (int)rnd(r, 101));
        for (int g = (int)rnd(r, 3); g > 0; g--) seq_generate(s, l);
        for (int t = (int)rnd(r, 4); t > 0; t--) seq_set_step(s, l, (int)rnd(r, (unsigned)seq_bars(s)), (int)rnd(r, 16), (int)rnd(r, 2));
    }
}

/* op 0..4: a key, a scale, a preset, a generated progression, a generated progression undone */
static void harmony_change(seq_t *s, int op, unsigned *r) {
    if (op == 0) seq_set_key(s, (seq_key(s) + 1 + (int)rnd(r, 11)) % 12);
    else if (op == 1) seq_set_scale(s, (seq_scale(s) + 1 + (int)rnd(r, SCALE_COUNT - 1)) % SCALE_COUNT);
    else if (op == 2) seq_set_prog_preset(s, (int)rnd(r, THEORY_NPRESETS));
    else {
        seq_set_harmony(s, (int)rnd(r, 101));
        seq_generate_progression(s);
        if (op == 4) seq_undo(s);
    }
}

/* A key, scale or progression change regenerates only the lanes whose style reads them (the walk). Over random states,
 * every generated lane still holds exactly what a full regeneration writes, with its locked steps as they were, and a
 * MANUAL lane is untouched. */
static void test_gen_selective_regen(void) {
    static seq_t s;
    static seq_lane_t before[2];
    unsigned r = 12345;
    int stale = 0, moved = 0, walks = 0;
    for (int t = 0; t < 500; t++) {
        random_gen_state(&s, &r);
        memcpy(before, s.part[0].lane, sizeof before);
        harmony_change(&s, t % 5, &r);
        for (int l = 0; l < 2; l++) {
            const seq_lane_t *ln = &s.part[0].lane[l];
            walks += ln->gen.style == GEN_BASS_WALK;
            if (ln->gen.style == GEN_MANUAL) { moved += !same_lane(&s, l, &before[l]); continue; }
            stale += !lane_is_gen(&s, l);
            for (int i = 0; i < s.part[0].bars * SEQ_STEPS; i++)
                moved += locked(&s, l, i) && memcmp(&ln->steps[i], &before[l].steps[i], sizeof ln->steps[i]);
        }
    }
    CHECK(!stale && !moved && walks > 50, "500 random key/scale/progression changes: %d lanes differ from a full regeneration, "
          "%d locked or MANUAL steps moved (%d walks)", stale, moved, walks);
}

/* a random melody lane over random_gen_state's: style (MANUAL too), complexity, seed and hand edits (Melody 2 is retired) */
static void random_mel_state(seq_t *s, unsigned *r) {
    static const int up[3] = {GEN_MANUAL, GEN_MEL_HOOK, GEN_MEL_ARP};
    random_gen_state(s, r);
    seq_set_style(s, LANE_MEL_UP, up[rnd(r, 3)]);
    seq_set_complexity(s, LANE_MEL_UP, (int)rnd(r, 101));
    if (rnd(r, 2)) seq_generate(s, LANE_MEL_UP);
    for (int t = (int)rnd(r, 8); t > 0; t--) seq_set_step(s, LANE_MEL_UP, (int)rnd(r, (unsigned)seq_bars(s)), (int)rnd(r, 16), (int)rnd(r, 2));
}

/* A key change transposes the melody instead of regenerating it. Over random states every generated lane holds exactly what a full regeneration writes, its locked steps as
 * they were, and a MANUAL lane is untouched. */
static void test_gen_key_transposes(void) {
    static seq_t s;
    static seq_lane_t before[SEQ_LANES];
    unsigned r = 4242;
    int stale = 0, moved = 0;
    for (int t = 0; t < 1000; t++) {
        random_mel_state(&s, &r);
        memcpy(before, s.part[0].lane, sizeof before);
        seq_set_key(&s, (seq_key(&s) + 1 + (int)rnd(&r, 11)) % 12);
        for (int l = 0; l < SEQ_LANES; l++) {
            const seq_lane_t *ln = &s.part[0].lane[l];
            if (ln->gen.style == GEN_MANUAL) { moved += !same_lane(&s, l, &before[l]); continue; }
            stale += !lane_is_gen(&s, l);
            for (int i = 0; i < s.part[0].bars * SEQ_STEPS; i++)
                moved += locked(&s, l, i) && memcmp(&ln->steps[i], &before[l].steps[i], sizeof ln->steps[i]);
        }
    }
    CHECK(!stale && !moved, "1000 random key changes over melodies: %d lanes differ from a full regeneration, "
          "%d locked or MANUAL steps moved", stale, moved);
}

/* a setter given the value it already has does nothing: no regeneration, no undo snapshot */
static void test_gen_noop_setters(void) {
    static seq_t s, was;
    unsigned r = 99;
    int changed = 0;
    for (int t = 0; t < 50; t++) {
        random_gen_state(&s, &r);
        memcpy(&was, &s, sizeof s);
        seq_set_key(&s, seq_key(&s));
        seq_set_scale(&s, seq_scale(&s));
        if (seq_prog_preset(&s) >= 0) seq_set_prog_preset(&s, seq_prog_preset(&s));
        seq_set_bars(&s, seq_bars(&s));
        seq_set_chord_len(&s, seq_chord_len(&s));
        for (int l = 0; l < 2; l++) seq_set_complexity(&s, l, seq_complexity(&s, l));
        changed += memcmp(&s, &was, sizeof s) != 0;
    }
    CHECK(!changed, "re-setting key, scale, preset, length, chord length and complexity changes nothing (%d of 50 did)", changed);
}

/* setters run while the audio thread waits: the slowest (a key change, both lanes 16 bars at c 100) stays cheap */
static void test_gen_regen_time(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_chord_len(&s, 4);
    seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
    seq_set_complexity(&s, LANE_BASS, 100);
    seq_set_complexity(&s, LANE_CHORDS, 100);
    clock_t t0 = clock();
    for (int i = 0; i < 100; i++) seq_set_key(&s, i % 12);
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 100;
    CHECK(ms < 1.0, "a key change regenerating every lane takes %.4f ms (budget 1 ms); sizeof(seq_t) %zu", ms, sizeof s);
}

/* 0.5 state: generated lanes save their recipe (g), their locks (lk) and only their locked on-steps */

/* Chords Stabs c65 and Bass Walk c80 on new seeds, a HARMONY 70 progression, and locks: a Chords on-lock, a Bass off-lock */
static void gen_project(seq_t *s) {
    const seq_lane_t *ch = &s->part[0].lane[LANE_CHORDS], *ba = &s->part[0].lane[LANE_BASS];
    int on = 32, off = 17;
    seq_init(s, emit, NULL);
    old_chords(s);
    seq_set_chord_len(s, 8);
    seq_set_harmony(s, 70);
    seq_generate_progression(s);
    seq_set_style(s, LANE_CHORDS, GEN_CH_STABS);
    seq_set_complexity(s, LANE_CHORDS, 65);
    seq_generate(s, LANE_CHORDS);
    seq_set_style(s, LANE_BASS, GEN_BASS_WALK);
    seq_set_complexity(s, LANE_BASS, 80);
    seq_generate(s, LANE_BASS);
    while (ch->steps[on].on) on++;     /* bar 3's first off chord step */
    while (!ba->steps[off].on) off++;  /* bar 2's first bass onset after the downbeat */
    seq_set_step(s, LANE_CHORDS, on / 16, on % 16, 1);
    seq_set_step(s, LANE_BASS, off / 16, off % 16, 0);
    seq_set_step(s, LANE_BASS, 0, 3, !ba->steps[3].on);   /* a third lock, whichever way */
}

/* a generated Chords and Bass with locks, new seeds, complexity and HARMONY save and load back to the same lanes */
static void test_state_gen_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    int rec;
    gen_project(&s);
    int n = seq_state_save(&s, buf, sizeof buf);
    const char *a1 = strstr(buf, "|A1=0:"), *a1g = strstr(buf, "|A1.g=");
    rec = a1 && a1g ? (int)(a1g - a1 - 6) / 7 : -1;
    CHECK(n > 0 && strstr(buf, "csq2|gv=1|") && strstr(buf, "|A.h=70,") && strstr(buf, "|A1.lk=") && strstr(buf, "|A2.lk="),
          "a generated state saves gv=1, A.h and the lock bitmaps (%d bytes)", n);
    CHECK(rec == 1, "the Chords lane saves 1 step record, its locked on-step (got %d)", rec);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf), "and loads");
    CHECK(chords_loaded_as(&s.part[0].lane[LANE_CHORDS], &t.part[0].lane[LANE_CHORDS], s.part[0].bars) == 1,
          "the Chords lane (0.18): DRAWN bars at c50 that sound as the generated lane and its lock did");
    CHECK(bass_loaded_as(&s.part[0].lane[LANE_BASS], &t.part[0].lane[LANE_BASS], s.part[0].bars) == 1 &&
          t.part[0].lane[LANE_BASS].gen.seed == s.part[0].lane[LANE_BASS].gen.seed,
          "the Bass lane (0.19): DRAWN bars at c50 that keep the generated lane's rhythm and its locks, its seed kept");
    CHECK(same_part(&s, &t) && !memcmp(s.part[0].voice, t.part[0].voice, sizeof s.part[0].voice), "the melodies and the voice table too");
    CHECK(t.part[0].harmony == 70 && t.part[0].harmony_seed == s.part[0].harmony_seed && t.part[0].prog_len == s.part[0].prog_len &&
          !memcmp(t.part[0].prog, s.part[0].prog, sizeof t.part[0].prog), "HARMONY, its seed and the progression too");
    CHECK(resaves_same(&t), "save -> load -> save gives the same string (0.18: once loaded, the Chords lane DRAWN bars)");
    CHECK(!seq_can_undo(&t), "a load leaves nothing to undo");
}

/* 0.12 chord rhythms: the Chords lane only; GEN, a lock, undo; saved as .g and loaded back to the same lane */
static void test_chord_rhythms(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], want[32];
    static seq_lane_t was;
    int fits = 1;
    seq_init(&s, emit, NULL);
    old_chords(&s);
    for (int st = GEN_CH2_FIRST; st <= GEN_CH2_LAST; st++) {
        seq_set_style(&s, LANE_CHORDS, st);
        seq_set_style(&s, LANE_BASS, st);
        seq_set_style(&s, LANE_MEL_UP, st);
        seq_set_style(&s, LANE_MEL_DOWN, st);
        fits &= seq_style(&s, LANE_CHORDS) == st && seq_style(&s, LANE_BASS) != st && seq_style(&s, LANE_MEL_UP) != st &&
                seq_style(&s, LANE_MEL_DOWN) != st && lane_is_gen(&s, LANE_CHORDS);
    }
    CHECK(fits, "the six chord rhythms fit the Chords lane only, and the lane plays the generator's steps");
    seq_set_chord_len(&s, 8);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_PUSH);
    seq_set_complexity(&s, LANE_CHORDS, 70);
    was = s.part[0].lane[LANE_CHORDS];
    int n0 = s.undo_n;
    seq_generate(&s, LANE_CHORDS);
    CHECK(s.undo_n == n0 + 1 && lane_is_gen(&s, LANE_CHORDS), "GEN on a chord rhythm: a new seed, one undo step");
    seq_undo(&s);
    CHECK(same_lane(&s, LANE_CHORDS, &was), "undo restores the lane");
    seq_generate(&s, LANE_CHORDS);
    int off = 0;
    while (s.part[0].lane[LANE_CHORDS].steps[off].on) off++;
    seq_set_step(&s, LANE_CHORDS, off / 16, off % 16, 1);   /* a locked hand-placed hit */
    seq_set_complexity(&s, LANE_CHORDS, 20);
    CHECK(s.part[0].lane[LANE_CHORDS].steps[off].on && locked(&s, LANE_CHORDS, off) && lane_is_gen(&s, LANE_CHORDS),
          "a lock survives a regeneration");
    int n = seq_state_save(&s, buf, sizeof buf);
    snprintf(want, sizeof want, "|A1.g=%d,", GEN_CH_PUSH);
    CHECK(n > 0 && strstr(buf, want), "the style saves in A1.g (%s)", want);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && chords_loaded_as(&s.part[0].lane[LANE_CHORDS], &t.part[0].lane[LANE_CHORDS], s.part[0].bars) == 1,
          "and loads (0.18) as DRAWN bars at c50 that sound as the lane and its lock did");
    CHECK(resaves_same(&t), "save -> load -> save gives the same string (0.18: once loaded)");
}

/* 0.12 NEW PROGRESSION: above HARMONY 60 one slot may be borrowed (src, the borrow's degree, a Triad); locks kept;
 * one undo step each; at 60 and below never */
static void test_progression_borrow(void) {
    static seq_t s;
    int borrowed = 0, bad = 0, kept = 1, steps = 1, lo = 0;
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_harmony(&s, 100);
    seq_toggle_slot_lock(&s, 2);
    int k2 = s.part[0].prog[2];
    for (int k = 0; k < 60; k++) {
        int n0 = s.undo_n;
        seq_generate_progression(&s);
        const seq_part_t *p = &s.part[0];
        steps &= s.undo_n == n0 + 1 || s.undo_n == n0;   /* the ring may be full */
        kept &= p->prog[2] == k2 && !p->src[2] && p->prog_len == 4;
        for (int i = 0; i < p->prog_len; i++) {
            if (i == 2) continue;
            bad += p->colour[i] != 0 || (p->src[i] && (i == 0 || i == 3 || p->prog[i] != theory_borrow_degree(s.scale, p->src[i])));
            borrowed += p->src[i] != 0;
        }
        bad += !lane_is_gen(&s, LANE_CHORDS);
    }
    CHECK(borrowed > 10 && !bad && kept && steps, "HARMONY 100: %d of 60 borrow a slot (never first or last), the locked "
          "slot stays, %d errors", borrowed, bad);
    int pv[SEQ_MAX_PROG];
    unsigned char sv[SEQ_MAX_PROG];
    do {
        memcpy(pv, s.part[0].prog, sizeof pv);
        memcpy(sv, s.part[0].src, sizeof sv);
        seq_generate_progression(&s);
    } while (!s.part[0].src[1] && ++lo < 100);
    seq_undo(&s);
    CHECK(lo < 100 && !memcmp(pv, s.part[0].prog, sizeof pv) && !memcmp(sv, s.part[0].src, sizeof sv) &&
              lane_is_gen(&s, LANE_CHORDS), "UNDO takes a borrowing NEW PROGRESSION back in one step");
    lo = 0;
    seq_set_harmony(&s, 60);
    for (int k = 0; k < 60; k++) {
        seq_generate_progression(&s);
        for (int i = 0; i < 4; i++) lo += i != 2 && s.part[0].src[i];
    }
    CHECK(!lo, "HARMONY 60: never borrows (%d)", lo);
}

/* turns every step of a lane on and locks it: off then on, so the steps the generator has on lock too */
static void lock_all_on(seq_t *s, int lane) {
    for (int b = 0; b < seq_bars(s); b++)
        for (int i = 0; i < SEQ_STEPS; i++) {
            seq_set_step(s, lane, b, i, 0);
            seq_set_step(s, lane, b, i, 1);
        }
}

/* every field at its longest: 16 chords, key 11, HARMONY 100 seed 65535, 5-digit seeds, complexity 100; 0.17 (final review) the
 * longest `.vx` (style 51, Rootless, COLOUR Power, INVERSION -2, SPREAD Cluster, STRUM down) on all six parts, in use (the Verse's
 * copies) */
static void longest_fields(seq_t *s) {
    seq_state_load(s, "csq2|k=11|s=3|A.len=16|A.cl=32|A.h=100,65535|A.prog=0123456012345601|A1=0:|A2=0:|A3=0:|A4=0:");
    for (int l = 0; l < SEQ_LANES; l++) {
        s->part[0].lane[l].gen.seed = 65535;
        s->part[0].lane[l].mute = 1;
        seq_set_complexity(s, l, 100);
    }
    for (int p = SEQ_NPARTS - 1; p >= 0; p--) {
        seq_select_part(s, p);
        seq_set_voicing_style(s, VOICING_NSTYLES - 1);
        seq_set_vmode(s, VMODE_ROOTLESS);
        seq_set_vknob(s, VK_COL, -1);
        seq_set_vknob(s, VK_INV, -2);
        seq_set_vknob(s, VK_SPR, -1);
        seq_set_vknob(s, VK_STR, -8);
    }
}

/* the save has the longest `.vx` on every part, and left none out */
static int longest_vx_saved(const seq_t *s, const char *buf) {
    char want[32];
    for (int p = 0; p < SEQ_NPARTS; p++) {
        snprintf(want, sizeof want, "|%c.vx=%d,3,-1,-2,-1,1|", 'A' + p, VOICING_NSTYLES - 1);
        if (!strstr(buf, want)) return 0;
    }
    return !seq_unsaved_parts(s);
}

static void test_state_gen_size(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_bars(&s, 16);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_STABS);
    s.part[0].lane[LANE_CHORDS].gen.seed = 65535;
    seq_set_complexity(&s, LANE_CHORDS, 100);
    seq_state_save(&s, buf, sizeof buf);
    const char *a = strstr(buf, "|A1="), *b = strstr(buf, "|A2=");
    CHECK(a && b && b - a < 40, "a 16-bar generated lane at c100 with no locks saves in %d bytes (< 40)", a && b ? (int)(b - a) : -1);
    longest_fields(&s);
    for (int l = 0; l < SEQ_LANES; l++)
        for (int i = 0; i < 16 * SEQ_STEPS; i++) seq_set_step(&s, l, i / 16, i % 16, 1);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && n < SEQ_STATE_MAX, "worst MANUAL case, 16 bars with every step on and every field longest: %d bytes (< %d)", n, SEQ_STATE_MAX);
    CHECK(longest_vx_saved(&s, buf), "with the longest .vx on all six parts, every part saved (%d bytes, unsaved mask 0x%x)", n,
          seq_unsaved_parts(&s));
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && same_part(&s, &t) && manual_but_drawn(&t) && seq_complexity(&t, LANE_MEL_UP) == 100 &&
          t.part[0].lane[LANE_MEL_UP].gen.seed == 65535,
          "and loads back, a MANUAL lane's seed and complexity too (0.18: the Chords lane DRAWN, longest_fields' load converts it; 0.19 the Bass too)");
    longest_fields(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_STABS);
    seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
    for (int l = 0; l < SEQ_LANES; l++) lock_all_on(&s, l);
    n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && n < SEQ_STATE_MAX, "worst generated case, Chords and Bass every step locked on, melodies every step on: %d bytes", n);
    CHECK(longest_vx_saved(&s, buf), "and every part saved with its .vx (%d bytes, unsaved mask 0x%x)", n, seq_unsaved_parts(&s));
    CHECK(seq_state_load(&t, buf) && same_lane(&t, LANE_CHORDS, &s.part[0].lane[LANE_CHORDS]) &&
          bass_loaded_as(&s.part[0].lane[LANE_BASS], &t.part[0].lane[LANE_BASS], 16) == 1 && same_part(&s, &t),
          "and loads back (0.19: the Bass as DRAWN bars that keep its rhythm)");
}

/* a 0.4 state (no gv, g, lk or A.h) loads as it always did: every lane MANUAL, the same steps, the same notes */
static void test_state_04(void) {
    static seq_t s, t;
    static ev_t was[MAXEV];
    /* saved by the 0.4 code (seq_state_save unchanged from 0a52e4f to c5913e1): v1's song cut to 1 bar, D minor,
     * smooth, Melody 1 muted, a bass step added on step 6 */
    static const char OLD[] = "csq2|k=2|s=1|A.len=1|A.cl=16|A.prog=0526|A.voi=1|A1=0:0015640|A2=0:001464005146400814640"
                              "|A3=1:001664002366400456640061664008366400a566400c166400e36640"
                              "|A4=0:005764002376400417640065764008376400a176400c576400e37640";
    int n;
    fresh(&s, 0);
    seq_set_bars(&s, 1);
    seq_set_key(&s, 2);
    seq_set_voicing(&s, VOICING_SMOOTH);
    seq_set_mute(&s, LANE_MEL_UP, 1);
    seq_set_step(&s, LANE_BASS, 0, 5, 1);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, OLD) && same_part(&s, &t) && manual_but_drawn(&t),
          "a 0.4 state loads its steps with every lane MANUAL (0.18: but the Chords lane, DRAWN bars that sound the same; 0.19 the Bass "
          "lane, DRAWN bars that keep its rhythm)");
    CHECK(t.part[0].harmony == 30 && t.part[0].harmony_seed == 1, "no A.h: HARMONY 30, seed 1");
    nev = 0;
    play(&s, 0, 8.0, 0);
    seq_block(&s, 8.0, bpm, 0, 128);
    n = nev;
    memcpy(was, evs, sizeof(ev_t) * (size_t)n);
    nev = 0;
    play(&t, 0, 8.0, 0);
    seq_block(&t, 8.0, bpm, 0, 128);
    CHECK(n > 0 && nev == n && !memcmp(was, evs, sizeof(ev_t) * (size_t)n), "and plays the same %d events as before", n);
}

/* everything a load could change, except its scratch (load_tmp) */
static int same_seq(const seq_t *a, const seq_t *b) {
    return !memcmp(a->part, b->part, sizeof a->part) && a->key == b->key && a->scale == b->scale && !memcmp(a->chan, b->chan, sizeof a->chan) && a->view_bar == b->view_bar &&
           a->undo_n == b->undo_n && a->undo_top == b->undo_top && !memcmp(a->undo, b->undo, sizeof a->undo);
}

#define ST_HEAD "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526"
#define ST_MEL "|A3=0:|A4=0:"
#define Z16 "0000000000000000"
#define LK1 "1000000000000000" Z16 Z16 Z16   /* step 1 locked */

static void test_state_gen_refused(void) {
    static seq_t s, before;
    static const char *const bad[] = {
        ST_HEAD "|gv=2|A1=0:|A2=0:" ST_MEL,
        ST_HEAD "|A1=0:|A2=0:|A2.g=4,1,101" ST_MEL,                         /* complexity out of range */
        ST_HEAD "|A1=0:|A1.g=1,1,30|A1.lk=100000000000000" Z16 Z16 Z16 "|A2=0:" ST_MEL,   /* 63 digits */
        ST_HEAD "|A1=0:|A1.lk=" LK1 "|A2=0:" ST_MEL,                        /* locks on a MANUAL lane */
        ST_HEAD "|A1=0:|A1.g=1,70000,30|A2=0:" ST_MEL,                      /* seed */
        ST_HEAD "|gv=|A1=0:|A2=0:" ST_MEL, ST_HEAD "|gv=01|A1=0:|A2=0:" ST_MEL,
        ST_HEAD "|A1=0:|A1.g=0,1,30|A1.lk=" LK1 "|A2=0:" ST_MEL,            /* MANUAL said outright, with locks */
        ST_HEAD "|A1=0:|A1.g=1,1,30|A1.lk=X000000000000000" Z16 Z16 Z16 "|A2=0:" ST_MEL,     /* not hex */
        ST_HEAD "|A1=0:|A1.g=1,1|A2=0:" ST_MEL, ST_HEAD "|A1=0:|A1.g=1,1,30,4|A2=0:" ST_MEL,
        ST_HEAD "|A1=0:|A1.g=1,,30|A2=0:" ST_MEL, ST_HEAD "|A1=0:|A1.g=-1,1,30|A2=0:" ST_MEL,
        ST_HEAD "|A1=0:|A1.g=1,1,30,|A2=0:" ST_MEL, ST_HEAD "|A1=0:|A1.g=|A2=0:" ST_MEL,
        ST_HEAD "|A.h=101,1|A1=0:|A2=0:" ST_MEL, ST_HEAD "|A.h=30,65536|A1=0:|A2=0:" ST_MEL, ST_HEAD "|A.h=30|A1=0:|A2=0:" ST_MEL,
        NULL};
    static const char GOOD[] =
        ST_HEAD "|gv=1|A.h=100,65535|A1=0:0015640|A1.g=1,65535,100|A1.lk=" LK1 "|A2=0:|A2.g=4,0,0|A3=0:|A3.g=0,9,9|A4=0:";
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, GOOD) && seq_style(&s, LANE_CHORDS) == GEN_CH_SUSTAIN && s.part[0].lane[LANE_BASS].gen.style == GEN_BASS_DRAWN &&
          s.part[0].bass.set.style == 40 && seq_complexity(&s, LANE_MEL_UP) == 9 && s.part[0].lane[LANE_CHORDS].steps[0].on &&
          seq_harmony(&s) == 100,
          "the valid state the refused ones change one field of loads (0.18: Chords DRAWN, its locked record kept; 0.19: the Root "
          "Bass DRAWN, Pop Root)");
    seq_set_step(&s, LANE_BASS, 0, 2, !s.part[0].lane[LANE_BASS].steps[2].on);   /* undo history a refusal must keep */
    seq_set_bars(&s, 2);
    before = s;
    for (int i = 0; bad[i]; i++) CHECK(!seq_state_load(&s, bad[i]) && same_seq(&s, &before), "refused, nothing changed: %s", bad[i]);
}

/* a style id this build lacks for the lane (a later build's, or another lane's) is not a refusal: the lane loads as
 * MANUAL at the MANUAL default, its records as its notes, its lk ignored */
static void test_state_unknown_style(void) {
    static seq_t s;
    static const char U3[] = ST_HEAD "|A1=0:|A2=0:|A3=0:0215640" "0a15640" "|A3.g=9,5,40|A3.lk=" LK1 "|A4=0:";
    static const char U1[] = ST_HEAD "|A1=0:0015640" "0315640|A1.g=5,1,30|A2=0:" ST_MEL;
    static const char U2[] = ST_HEAD "|A1=0:|A2=0:0015640|A2.g=7,1,30" ST_MEL;
    static const char U4[] = ST_HEAD "|A1=0:|A2=0:|A3=0:|A4=0:0415640|A4.g=1,1,30|A4.lk=" LK1;
    static const char *const bad[] = {ST_HEAD "|A1=0:|A1.g=1,1|A2=0:" ST_MEL, ST_HEAD "|A1=0:|A1.g=1,70000,30|A2=0:" ST_MEL,
                                      ST_HEAD "|A1=0:|A1.g=9,70000,30|A2=0:" ST_MEL, ST_HEAD "|A1=0:|A1.g=9,1,101|A2=0:" ST_MEL,
                                      ST_HEAD "|A1=0:|A1.g=9,-1,30|A2=0:" ST_MEL, ST_HEAD "|A1=0:|A1.g=9,1|A2=0:" ST_MEL, NULL};
    static seq_t before;
    const seq_lane_t *ln;
    int n = 0;
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, U3), "style 9 on Melody 1, with a lock bitmap, loads");
    ln = &s.part[0].lane[LANE_MEL_UP];
    CHECK(!lane_is_gen(&s, LANE_MEL_UP) && ln->gen.style == GEN_MANUAL && ln->gen.seed == 1 && ln->gen.complexity == 30,
          "Melody 1 is MANUAL at {MANUAL, 1, 30}");
    CHECK(ln->steps[2].on && ln->steps[10].on && ln->steps[2].tone == 1 && ln->steps[10].tone == 1, "its two records are its notes");
    CHECK(!locked(&s, LANE_MEL_UP, 0), "its lk is ignored");
    for (int i = 0; i < 64; i++) n += ln->steps[i].on;
    CHECK(n == 2, "exactly two on-steps (%d)", n);
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, U1) && s.part[0].lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && s.part[0].rb[DL_CHORDS][0].from == -1 &&
          s.part[0].lane[LANE_CHORDS].steps[0].on && s.part[0].lane[LANE_CHORDS].steps[3].on,
          "a Bass style on Chords loads Chords as MANUAL with its records (0.18: converted to DRAWN bars, drawn from nothing)");
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, U2) && s.part[0].lane[LANE_BASS].gen.style == GEN_BASS_DRAWN && s.part[0].rb[DL_BASS][0].c[0] == RC_HIT &&
          s.part[0].rb[DL_BASS][0].from == -1 && s.part[0].lane[LANE_BASS].steps[0].on,
          "style 7 on Bass loads Bass as MANUAL with its record (0.19: converted to DRAWN bars, the record a hit drawn from nothing)");
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, U4) && seq_style(&s, LANE_MEL_DOWN) == GEN_MANUAL && !any_on(cur_lane(&s, LANE_MEL_DOWN)),
          "a Chords style on Melody 2 loads, and the retired lane is dropped (MANUAL, empty)");
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, U3), "a known lane beside it still loads");
    before = s;
    for (int i = 0; bad[i]; i++) CHECK(!seq_state_load(&s, bad[i]) && same_seq(&s, &before), "malformed g still refused: %s", bad[i]);
}

/* what our saver never writes but a load must not refuse (a refused load leaves defaults that MPC saves over the song):
 * a lock past the loop and a record at an unlocked generated step are dropped, the rest of the state loads */
static void test_state_gen_lenient(void) {
    static seq_t s, t;
    static const char CLEAN[] = ST_HEAD "|A1=0:0115640|A1.g=1,1,30|A1.lk=2000000000000000" Z16 Z16 Z16 "|A2=0:|A2.g=4,5,60" ST_MEL;
    static const char *const STRAY[] = {
        ST_HEAD "|A1=0:0115640|A1.g=1,1,30|A1.lk=2000000000000000" Z16 "1000000000000000" Z16 "|A2=0:|A2.g=4,5,60" ST_MEL,
        ST_HEAD "|A1=0:0115640|A1.g=1,1,30|A1.lk=2000000000000000" Z16 Z16 Z16 "|A2=0:0b14640|A2.g=4,5,60" ST_MEL, NULL};
    static const char *const WHAT[] = {"a lock past the loop", "a record at an unlocked generated step"};
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, CLEAN), "the clean state loads");
    for (int i = 0; STRAY[i]; i++) {
        seq_init(&s, emit, NULL);
        CHECK(seq_state_load(&s, STRAY[i]) && !memcmp(&s.part[0], &t.part[0], sizeof s.part[0]) &&
              s.part[0].lane[LANE_BASS].gen.style == GEN_BASS_DRAWN &&   /* 0.19: the generated Bass, as DRAWN bars */
              s.part[0].lane[LANE_CHORDS].steps[1].on,   /* 0.18: the Chords lane's locked record, as a DRAWN bar */
              "%s: loads as the clean state, the stray part dropped", WHAT[i]);
    }
}

/* the realistic path to locks past the loop: locks in bar 8, shrink to 4 bars, save, load */
static void test_state_shrunk_locks(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    old_bass(&s, 30);
    seq_set_bars(&s, 8);
    for (int l = LANE_CHORDS; l <= LANE_BASS; l++) {
        seq_set_step(&s, l, 7, 3, !s.part[0].lane[l].steps[7 * 16 + 3].on);
        seq_set_step(&s, l, 7, 9, !s.part[0].lane[l].steps[7 * 16 + 9].on);
        seq_set_step(&s, l, 1, 5, !s.part[0].lane[l].steps[16 + 5].on);   /* and one inside the loop to come */
    }
    seq_set_bars(&s, 4);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && seq_state_load(&t, buf), "8 bars with locks in bar 8, shrunk to 4: saves and loads");
    CHECK(bass_loaded_as(&s.part[0].lane[LANE_BASS], &t.part[0].lane[LANE_BASS], 4) == 1 &&
          t.part[0].lane[LANE_BASS].steps[16 + 5].on == s.part[0].lane[LANE_BASS].steps[16 + 5].on,
          "the Bass lane (0.19) DRAWN bars that keep its rhythm within the loop, its bar-2 lock's step kept");
    CHECK(chords_loaded_as(&s.part[0].lane[LANE_CHORDS], &t.part[0].lane[LANE_CHORDS], 4) == 1 &&
          t.part[0].lane[LANE_CHORDS].steps[16 + 5].on == s.part[0].lane[LANE_CHORDS].steps[16 + 5].on,
          "the Chords lane (0.18) DRAWN bars that sound as it did within the loop, its bar-2 lock's step kept");
}

static unsigned fnv(unsigned h, unsigned char b) { return (h ^ b) * 16777619u; }

static unsigned steps_hash(const seq_t *s) {
    unsigned h = 2166136261u;
    for (int l = 0; l < SEQ_LANES; l++)
        for (int i = 0; i < s->part[0].bars * SEQ_STEPS; i++) {
            const seq_step_t *x = &s->part[0].lane[l].steps[i];
            unsigned char b[5] = {x->on, x->tone, (unsigned char)x->oct, x->vel, x->len};
            for (int j = 0; j < 5; j++) h = fnv(h, b[j]);
        }
    return h;
}

/* Pins load + gen v1 together: a gv=1 state must load to these steps for as long as gv=1 states exist. 0.18: STATE_GOLDEN
 * stays the steps 0.17 loaded (gen v1 under the locks, rebuilt here from gen_lane_v1), STATE_GOLDEN_18 the steps 0.18 loaded,
 * its Chords lane converted to DRAWN bars (class velocities, ruling R2) that sound as those did (0.19: rebuilt here with the
 * 0.17 Bass), STATE_GOLDEN_19 the steps a load now plays, its Bass lane converted to DRAWN bars too: the same rhythm, its notes
 * the note model's (Walk as Upright Walk). */
#define STATE_GOLDEN 0x3b9dd26au
#define STATE_GOLDEN_18 0x1ce3facdu
#define STATE_GOLDEN_19 0x6600d677u
static void test_state_golden(void) {
    static seq_t s, r;
    static char buf[SEQ_STATE_MAX];
    static const char G[] = "csq2|gv=1|k=4|s=1|A.len=2|A.cl=8|A.h=55,321|A.prog=0352|A.voi=1|ch=2,3,4"   /* 0.7: ch field */
                            "|A1=0:0515640|A1.g=3,4242,55|A1.lk=0200000000000000" Z16 Z16 Z16
                            "|A2=0:0354501|A2.g=6,777,80|A2.lk=8000100000000000" Z16 Z16 Z16 "|A3=0:|A4=0:";
    seq_init(&s, emit, NULL);
    const seq_step_t *ch = s.part[0].lane[LANE_CHORDS].steps, *ba = s.part[0].lane[LANE_BASS].steps;
    CHECK(seq_state_load(&s, G), "the golden gv=1 state loads");
    CHECK(s.part[0].lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && s.part[0].lane[LANE_BASS].gen.style == GEN_BASS_DRAWN && ch[5].on &&
          ch[5].tone == TONE_R && ba[3].on && ba[3].vel == 100 && ba[3].len == 1 && !ba[16].on && s.part[0].bass.set.style == 29,
          "Stabs converted (0.18), Walk converted as Upright Walk (0.19); Chords step 6 and Bass step 4 from their records (a HIT, "
          "cut at the next), Bass step 17 locked off");
    r = s;
    seq_lane_t *was = &r.part[0].lane[LANE_CHORDS], *wasb = &r.part[0].lane[LANE_BASS];
    unsigned char bv[SEQ_MAX_BARS] = {0};
    gen_params_t gp = {GEN_CH_STABS, 4242, 55, 2, 8, s.part[0].prog, s.part[0].prog_len, 4, 1, NULL, LANE_CHORDS, 0, bv};
    gen_params_t gb = {GEN_BASS_WALK, 777, 80, 2, 8, s.part[0].prog, s.part[0].prog_len, 4, 1, NULL, LANE_BASS, 0, bv};
    gen_lane_v1(&gb, wasb->steps);   /* 0.19: the Bass as 0.17 and 0.18 loaded it */
    wasb->steps[3] = (seq_step_t){1, TONE_5, -1, 0x50, 2, 0};   /* the locked record */
    wasb->steps[16].on = wasb->steps[16].abs = 0;                /* the locked off-step */
    wasb->gen = (seq_gen_t){GEN_BASS_WALK, 777, 80, 0};
    unsigned h18 = steps_hash(&r);   /* 0.18's load: the Chords converted, the Bass generated */
    gen_lane_v1(&gp, was->steps);
    was->steps[5] = (seq_step_t){1, TONE_R, 0, 0x64, 1, 0};   /* the locked record */
    was->gen = (seq_gen_t){GEN_CH_STABS, 4242, 55, 0};
    CHECK(steps_hash(&r) == STATE_GOLDEN, "0.17's steps (gen v1 under the locks) hash to 0x%08x (got 0x%08x)", STATE_GOLDEN, steps_hash(&r));
    CHECK(h18 == STATE_GOLDEN_18, "0.18's (the Chords lane DRAWN) to 0x%08x (got 0x%08x)", STATE_GOLDEN_18, h18);
    CHECK(chords_loaded_as(was, &s.part[0].lane[LANE_CHORDS], 2) == 1, "the converted Chords bars sound as they did");
    CHECK(bass_loaded_as(wasb, &s.part[0].lane[LANE_BASS], 2) == 1, "the converted Bass bars keep the rhythm they had");
    unsigned h = steps_hash(&s);
    CHECK(h == STATE_GOLDEN_19, "the loaded steps hash to 0x%08x (got 0x%08x)", STATE_GOLDEN_19, h);
    CHECK(saves_but_drawn(&s, G) && seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|A1=0:|A1.g=17,4242,50|A.rb=") &&
          strstr(buf, "|A2=0:|A2.g=18,777,50|"), "and it saves back byte for byte, but the Chords and Bass lanes as DRAWN bars");
    snprintf(buf, sizeof buf, "csq2|%s", G + strlen("csq2|gv=1|"));
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, buf) && steps_hash(&s) == h, "without gv (absent means 1) it loads the same steps");
}

/* 0.6: generated melodies (0.7: Melody 2 and its Answer are retired; the Answer's tests live in test_gen.c) */

static const int MEL1_STYLES[3] = {GEN_MANUAL, GEN_MEL_HOOK, GEN_MEL_ARP}, MEL2_STYLES[3] = {GEN_MANUAL, GEN_MEL_ANSWER, GEN_MEL_ARP};

/* an old project: v1 (csq1), and 0.5 (generated Chords and Bass, Manual melodies with notes) */
static const char *const OLD_STATES[2] = {
    "csq1:1000000000000000100000001000000010101010101010101010101010101010:0000",
    "csq2|gv=1|k=4|s=1|A.len=2|A.cl=8|A.h=55,321|A.prog=0352|A.voi=1|A1=0:|A1.g=3,4242,55|A2=0:|A2.g=6,777,80"
    "|A3=0:0015640" "0635640" "1215640|A4=0:0435640"};

/* one random edit: op 0..17 */
static void mel_op(seq_t *s, int op, unsigned *r) {
    static char buf[SEQ_STATE_MAX];
    int lane = LANE_MEL_UP + (int)rnd(r, 2);
    if (op <= 3) seq_set_step(s, op < 3 ? LANE_MEL_UP : LANE_MEL_DOWN, (int)rnd(r, (unsigned)seq_bars(s)), (int)rnd(r, 16), (int)rnd(r, 2));
    else if (op == 4) seq_set_style(s, LANE_MEL_UP, MEL1_STYLES[rnd(r, 3)]);
    else if (op == 5) seq_set_style(s, LANE_MEL_DOWN, MEL2_STYLES[rnd(r, 3)]);
    else if (op == 6) seq_set_complexity(s, lane, (int)rnd(r, 101));
    else if (op == 7) seq_generate(s, lane);
    else if (op == 8) seq_undo(s);
    else if (op == 9) seq_set_key(s, (int)rnd(r, 12));
    else if (op == 10) seq_set_scale(s, (int)rnd(r, SCALE_COUNT));
    else if (op == 11) seq_set_prog_preset(s, (int)rnd(r, THEORY_NPRESETS));
    else if (op == 12) seq_set_bars(s, 1 << rnd(r, 5));
    else if (op == 13) seq_set_chord_len(s, 4 << rnd(r, 4));
    else if (op == 14 && seq_state_save(s, buf, sizeof buf) > 0) seq_state_load(s, buf);
    else if (op == 15) {
        seq_set_harmony(s, (int)rnd(r, 101));
        seq_generate_progression(s);
    } else if (op == 16) seq_state_load(s, OLD_STATES[rnd(r, 2)]);
    else if (op == 17) seq_set_mute(s, (int)rnd(r, SEQ_LANES), (int)rnd(r, 2));
}

/* The melody rhythm caches never go stale: over random edit sequences (taps, undo, loads, length, chord length,
 * complexity, style, GEN, key, scale, progressions; edits aimed at the retired Melody 2 included) every generated lane equals an uncached
 * generation (gen_lane_v1) at its unlocked steps after every edit. */
static void test_gen_cache_differential(void) {
    static seq_t s;
    unsigned r = 777;
    int stale = 0, kept = 0;
    for (int run = 0; run < 100; run++) {
        seq_init(&s, emit, NULL);
        old_chords(&s);
        old_bass(&s, 30);
        old_mel(&s);
        for (int k = 0; k < 60; k++) {
            mel_op(&s, (int)rnd(&r, 18), &r);
            for (int l = 0; l < SEQ_LANES; l++)   /* 0.18: a load makes the Chords lane DRAWN bars, rendered, not generated (0.19: the Bass too) */
                stale += seq_style(&s, l) != GEN_MANUAL && s.part[0].lane[l].gen.style != GEN_CH_DRAWN &&
                         s.part[0].lane[l].gen.style != GEN_BASS_DRAWN && !lane_is_gen(&s, l);
            kept += s.gen_cache[0].valid;
        }
    }
    CHECK(!stale && kept > 3000, "6000 random edits with the rhythm caches: %d lanes stale (Melody 1's cache held %d times)", stale, kept);
}

/* generated melodies with locks save as recipes and come back the same */
static void test_state_mel_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    const seq_step_t *m1 = s.part[0].lane[LANE_MEL_UP].steps;
    int on = 0, off = 0;
    seq_init(&s, emit, NULL);
    old_mel(&s);
    seq_set_complexity(&s, LANE_MEL_UP, 60);
    seq_generate(&s, LANE_MEL_UP);
    while (m1[on].on) on++;
    while (!m1[off].on) off++;
    seq_set_step(&s, LANE_MEL_UP, on / 16, on % 16, 1);
    seq_set_step(&s, LANE_MEL_UP, off / 16, off % 16, 0);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "|A3.g=7,") && strstr(buf, "|A3.lk=") && strstr(buf, "|A4=0:") && !strstr(buf, "A4.g"),
          "a generated melody saves its recipe and locks, the retired Melody 2 as an empty lane (%d bytes)", n);
    seq_init(&t, emit, NULL);
    seq_set_style(&t, LANE_MEL_UP, GEN_MANUAL);
    CHECK(seq_state_load(&t, buf), "and load");
    CHECK(same_lane(&t, LANE_MEL_UP, &s.part[0].lane[LANE_MEL_UP]), "melody 1: steps, locks, style, seed and complexity equal");
    CHECK(seq_state_save(&t, again, sizeof again) == n && !strcmp(buf, again), "save -> load -> save gives the same string");
}

/* the worst generated state still fits: 16 bars, every lane generated with every step locked on */
static void test_state_mel_size(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    longest_fields(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_STABS);
    seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
    seq_set_style(&s, LANE_MEL_UP, GEN_MEL_HOOK);
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) lock_all_on(&s, l);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(longest_vx_saved(&s, buf), "every part saved with its .vx (%d bytes, unsaved mask 0x%x)", n, seq_unsaved_parts(&s));
    seq_init(&t, emit, NULL);
    CHECK(n > 0 && n < SEQ_STATE_MAX && seq_state_load(&t, buf) && same_part(&s, &t),
          "3 generated lanes, 16 bars, every step locked on: %d bytes (< %d), and loads back", n, SEQ_STATE_MAX);
}

/* ms of one complexity change on `lane`, sweeping the knob as a Q-Link does: the fastest of 5 sweeps, so a busy
 * machine (parallel runs) slows no single reading into a false failure */
static double knob_ms(seq_t *s, int lane) {
    double best = 1e9;
    for (int k = 0; k < 5; k++) {
        clock_t t0 = clock();
        for (int c = 0; c <= 100; c++) seq_set_complexity(s, lane, k % 2 ? 100 - c : c);
        double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 101;
        if (ms < best) best = ms;
    }
    return best;
}

/* A Melody 1 knob step stays well inside the budget; the 0.5 Bass (Walk) knob step is printed for scale only: short
 * windows on a busy machine land on fast or slow cores, and the device bench is the authority on setter cost. */
static void test_mel_setter_time(void) {
    static seq_t s;
    for (int k = 0; k < 2; k++) {   /* the default loop, then 16 bars of 1/4-bar chords */
        seq_init(&s, emit, NULL);
        if (k) {
            seq_set_bars(&s, 16);
            seq_set_chord_len(&s, 4);
        }
        seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
        double bass = knob_ms(&s, LANE_BASS), mel = knob_ms(&s, LANE_MEL_UP);
        CHECK(mel < 1.0, "%s: a Melody 1 knob step %.4f ms (budget 1 ms)", k ? "16 bars, 1/4-bar chords" : "default 4 bars", mel);
        printf("info a Bass Walk knob step %.4f ms (x%.2f)\n", bass, mel / bass);
    }
}

static void test_new_scales(void) {
    char b[16];
    CHECK(SCALE_COUNT == 7 && SCALE_V1_COUNT == 4, "seven scales, four from 0.6");
    theory_chord_name(4, SCALE_PHRYGIAN, 1, b, sizeof b);   /* E phrygian, degree 2: F major */
    CHECK(!strcmp(b, "F"), "E phrygian II is F, got %s", b);
    theory_chord_name(0, SCALE_LYDIAN, 1, b, sizeof b);     /* C lydian, degree 2: D major */
    CHECK(!strcmp(b, "D"), "C lydian II is D, got %s", b);
    theory_chord_name(9, SCALE_HARM_MINOR, 2, b, sizeof b); /* A harmonic minor, degree 3: C augmented */
    CHECK(!strcmp(b, "C+"), "A harmonic minor III is C+, got %s", b);
    theory_chord_name(9, SCALE_HARM_MINOR, 4, b, sizeof b); /* degree 5: E major */
    CHECK(!strcmp(b, "E"), "A harmonic minor V is E, got %s", b);
}

static void test_channels(void) {
    static seq_t s;
    char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    CHECK(seq_channel_of(&s, 0) == 1 && seq_channel_of(&s, 1) == 2 && seq_channel_of(&s, 2) == 3, "new: 1, 2, 3");
    seq_set_channel(&s, 2, 10);
    nev = 0;
    play(&s, 0.0, 4.0, 0);
    CHECK(count_on(10) > 0 && count_on(3) == 0, "Melody plays on its new channel");
    CHECK(seq_state_save(&s, buf, sizeof buf) && strstr(buf, "|ch=1,2,10"), "saved: %s", buf);
    const char *v06 = "csq2|gv=1|k=4|s=1|A.len=1|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:|A4=0:";
    CHECK(seq_state_load(&s, v06) && seq_channel_of(&s, 0) == 2 && seq_channel_of(&s, 2) == 4, "0.6 keeps 2, 3, 4");
    seq_set_channel(&s, 1, 0);
    CHECK(seq_channel_of(&s, 1) == 3, "0 is refused");
}

/* ch= round trips; a bad list refuses the whole state */
static void test_channel_state(void) {
    static seq_t s;
    char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_set_channel(&s, 2, 10);
    static seq_t t;
    seq_init(&t, emit, NULL);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|ch=1,2,10") && seq_state_load(&t, buf) &&
          seq_channel_of(&t, 0) == 1 && seq_channel_of(&t, 1) == 2 && seq_channel_of(&t, 2) == 10, "ch=1,2,10 round trips");
    static const char *const bad[] = {"0,2,3", "17,2,3", "2,3", "2,3,4,5", ""};
    for (int i = 0; i < 5; i++) {
        snprintf(buf, sizeof buf, "csq2|gv=1|k=4|s=1|A.len=1|A.cl=16|A.prog=0526|ch=%s|A1=0:|A2=0:|A3=0:|A4=0:", bad[i]);
        CHECK(!seq_state_load(&t, buf) && seq_channel_of(&t, 2) == 10, "refused: ch=%s", bad[i]);
    }
}

static void test_channel_change_releases(void) {
    static seq_t s;
    nev = 0;
    seq_init(&s, emit, NULL);
    play(&s, 0.0, 0.1, 0);   /* step 0 fires: notes held on channels 1-3 */
    int offs = count_off(1);
    seq_set_channel(&s, 0, 9);
    CHECK(count_off(1) > offs, "held Chords notes end on the old channel");
    play(&s, 0.1, 4.0, 0);
    seq_block(&s, 4.0, bpm, 0, 128);   /* stop: the notes still held end */
    CHECK(balanced(), "no hanging notes");
}

/* 0.7 output stage: FNV-1a over the logged events */
static unsigned events_hash(void) {
    unsigned h = 2166136261u;
    for (int i = 0; i < nev; i++) {
        const unsigned char *b = (const unsigned char *)&evs[i];
        for (size_t j = 0; j < sizeof evs[i]; j++) h = (h ^ b[j]) * 16777619u;
    }
    return h;
}

static int any_on_at_step(const seq_t *s, int step) {
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && evs[i].k % (seq_bars(s) * SEQ_STEPS) == step) return 1;
    return 0;
}

/* Melody 1 (channel 4) note-ons at a step of a 1-bar loop: one per time the step fires */
static int count_step_fires(int step) {
    int n = 0;
    for (int i = 0; i < nev; i++) n += is_on(&evs[i]) && (evs[i].status & 15) == 3 && evs[i].k % SEQ_STEPS == step;
    return n;
}

static int max_vel(void) {
    int most = 0;
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && evs[i].vel > most) most = evs[i].vel;
    return most;
}

/* the shortest note, from each note-on to the next note-off on its channel and note */
static double min_note_len_beats(void) {
    double least = 1e9;
    for (int i = 0; i < nev; i++)
        for (int j = i + 1; is_on(&evs[i]) && j < nev; j++)
            if (is_off(&evs[j]) && (evs[j].status & 15) == (evs[i].status & 15) && evs[j].note == evs[i].note) {
                if (evs[j].ppq - evs[i].ppq < least) least = evs[j].ppq - evs[i].ppq;
                break;
            }
    return least;
}

static void test_neutral_output_is_06(void) {   /* defaults: byte-identical MIDI to 0.6 */
    static seq_t a, b;
    fresh(&a, 0);
    fresh(&b, 0);
    CHECK(seq_swing(&a) == 0 && seq_gate(&a) == 50 && seq_dynamics(&a) == 50, "defaults: swing 0, gate 50, dynamics 50");
    seq_set_swing(&b, 30); seq_set_gate(&b, 80); seq_set_dynamics(&b, 10);
    seq_set_swing(&b, 0); seq_set_gate(&b, 50); seq_set_dynamics(&b, 50);
    nev = 0; play(&a, 0.0, 16.0, 0); int na = nev; unsigned ha = events_hash();
    nev = 0; play(&b, 0.0, 16.0, 0);
    CHECK(na > 0 && nev == na && events_hash() == ha, "neutral output stage changes nothing");
}

/* Scope decision 5 (0.7): FOLLOW lights the playing bar in the bar strip (seq_strip_bar). 0.12 (plan decision 2): the grid
 * follows too (seq_grid_bar); the pick (seq_view_bar) is kept for when playback stops or FOLLOW goes off. After 0.16 FOLLOW is
 * a GLOBAL setting: a pick never turns it off */
static void test_follow(void) {
    static seq_t s;
    fresh(&s, 0);
    seq_set_bars(&s, 4);
    seq_set_view(&s, 2);
    CHECK(seq_view_bar(&s) == 2 && seq_strip_bar(&s) == 2, "held bar");
    seq_set_follow(&s, 1);
    play(&s, 0.0, 5.0, 0);   /* in bar 2 (0-based 1) */
    CHECK(seq_strip_bar(&s) == 1 && seq_grid_bar(&s) == 1 && seq_view_bar(&s) == 2,
          "follow shows the playing bar in the strip and the grid; the pick is kept");
    seq_set_view(&s, 3);
    CHECK(seq_follow(&s) && seq_view_bar(&s) == 3 && seq_strip_bar(&s) == 1, "picking a bar keeps FOLLOW: the strip stays on the playing bar");

    seq_set_follow(&s, 1);
    seq_set_view(&s, seq_view_bar(&s));   /* the host echoing back the grid's bar */
    CHECK(seq_follow(&s) && seq_strip_bar(&s) == 1, "writing back the grid's bar keeps FOLLOW on");
    seq_set_view(&s, 1);
    CHECK(seq_follow(&s) && seq_view_bar(&s) == 1, "picking the playing bar is a pick too, FOLLOW kept");
    seq_set_view(&s, 3);
    seq_set_follow(&s, 0);
    CHECK(!seq_follow(&s) && seq_view_bar(&s) == 1, "FOLLOW off while playing: the grid joins the playing bar");
}

static void test_swing_delays_odd_steps_once(void) {
    static seq_t s;
    fresh(&s, 1);   /* every Melody 1 step on: v1's pattern has nothing on odd steps */
    seq_set_bars(&s, 1);
    seq_set_swing(&s, 100);
    /* step 1 is at 0.25; with full swing it is due at 0.25 + 1/12 */
    nev = 0; play(&s, 0.0, 0.30, 0);
    CHECK(any_on_at_step(&s, 0) && !any_on_at_step(&s, 1), "step 1 not yet fired at 0.30");
    play(&s, 0.30, 0.10, 0);
    CHECK(any_on_at_step(&s, 1), "step 1 fired by 0.40");
    /* loop wrap: 4 beats then back to 0; the swung step 15 fires once per pass */
    seq_block(&s, 0.40, bpm, 0, 128);
    nev = 0; play(&s, 0.0, 12.0, 4.0);
    CHECK(count_step_fires(15) == 3 && count_step_fires(0) == 3, "step 15 fires once per loop pass (got %d)", count_step_fires(15));
    seq_block(&s, 0.0, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");
    /* a resync just past an even step still fires the swung odd step before it */
    nev = 0; play(&s, 0.26, 0.2, 0);
    CHECK(any_on_at_step(&s, 1) && !any_on_at_step(&s, 2), "a start at 0.26 still plays the swung step 1 (due 0.333)");
    seq_block(&s, 0.0, bpm, 0, 128);
    /* tempo jitter with swing: steps in order, never twice (Melody 1 plays one note per step: at most one note-on per k) */
    static int fired[1024];
    long last_k = -1;
    int doubled = 0, twice = 0;
    double ppq = 0;
    nev = 0;
    ev_seq = &s;
    for (int b = 1; b < 5000; b++) {
        seq_block(&s, ppq, bpm, 1, 128);
        if (s.cur_k >= 0 && s.cur_k < last_k) doubled++;
        if (s.cur_k > last_k) last_k = s.cur_k;
        ppq += blk();
        if (b % 37 == 0) ppq -= 0.12;
    }
    ev_seq = NULL;
    memset(fired, 0, sizeof fired);
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == 3 && evs[i].k >= 0 && evs[i].k < 1024) twice += ++fired[evs[i].k] > 1;
    CHECK(doubled == 0 && twice == 0 && last_k > 40, "swing under tempo jitter: no step fired twice (%d, %d note-ons twice)",
          doubled, twice);
}

/* lowering SWING while a swung odd step is still pending fires it at once, exactly once */
static void test_swing_lowered_mid_step(void) {
    static seq_t s;
    fresh(&s, 1);
    seq_set_bars(&s, 1);
    seq_set_swing(&s, 100);
    nev = 0; play(&s, 0.0, 0.30, 0);   /* step 1 (0.25) is due at 0.333: not yet */
    CHECK(count_step_fires(1) == 0, "step 1 not yet fired at 0.30");
    seq_set_swing(&s, 0);
    play(&s, 0.30, 0.20, 0);
    double at = -1;
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == 3 && evs[i].k == 1 && at < 0) at = evs[i].ppq;
    CHECK(count_step_fires(1) == 1 && at >= 0 && at - 0.25 <= 1.0 / 12 + 1e-9,
          "SWING 100 -> 0 at 0.30: step 1 fires once, %.3f beat late", at - 0.25);
    seq_block(&s, 0.5, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");
}

static void test_gate_and_dynamics(void) {
    static seq_t s;
    fresh(&s, 0);
    seq_set_gate(&s, 100);
    seq_set_dynamics(&s, 0);
    nev = 0; play(&s, 0.0, 4.0, 0);
    CHECK(max_vel() == 40, "dynamics 0: velocity 100 -> 40 (got %d)", max_vel());
    CHECK(min_note_len_beats() > 0.4, "gate 100: notes about twice as long (got %.3f)", min_note_len_beats());
    fresh(&s, 0);
    seq_set_gate(&s, 0);
    seq_set_dynamics(&s, 100);
    nev = 0; play(&s, 0.0, 4.0, 0);
    CHECK(max_vel() == 127, "dynamics 100: velocity clamps at 127");
    CHECK(min_note_len_beats() < 0.1, "gate 0: short notes (got %.3f)", min_note_len_beats());
    seq_block(&s, 4.0, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");
    seq_set_gate(&s, 101);
    seq_set_swing(&s, -1);
    CHECK(seq_gate(&s) == 0 && seq_swing(&s) == 0, "out of range is ignored");
}

static void test_state_output_stage(void) {
    static seq_t s;
    char buf[SEQ_STATE_MAX];
    fresh(&s, 0);
    CHECK(seq_state_save(&s, buf, sizeof buf) && !strstr(buf, "sw=") && !strstr(buf, "gt=") && !strstr(buf, "dy="),
          "neutral values are not saved");
    seq_set_swing(&s, 40); seq_set_gate(&s, 75); seq_set_dynamics(&s, 100);
    CHECK(seq_state_save(&s, buf, sizeof buf) && strstr(buf, "|ch=2,3,4|sw=40|gt=75|dy=100|A1="), "saved: %s", buf);
    static seq_t t;
    fresh(&t, 0);
    CHECK(seq_state_load(&t, buf) && seq_swing(&t) == 40 && seq_gate(&t) == 75 && seq_dynamics(&t) == 100, "and loads back");
    const char *v06 = "csq2|gv=1|k=4|s=1|A.len=1|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:|A4=0:";
    CHECK(seq_state_load(&t, v06) && seq_swing(&t) == 0 && seq_gate(&t) == 50 && seq_dynamics(&t) == 50, "absent: neutral");
    static const char *const bad[] = {"|sw=101", "|gt=", "|dy=x", "|sw=1000", "|gt=-1"};
    for (int i = 0; i < 5; i++) {
        snprintf(buf, sizeof buf, "%s%s", v06, bad[i]);
        CHECK(!seq_state_load(&s, buf), "refused: %s", bad[i]);
    }
    CHECK(seq_swing(&s) == 40, "a refused state changes nothing");
    seq_state_load(&s, "csq1:1000000000000000100000001000000010101010101010101010101010101010:0000");
    CHECK(seq_swing(&s) == 0 && seq_gate(&s) == 50 && seq_dynamics(&s) == 50, "a v1 load is neutral");
}

/* 0.8: parts as scenes */

/* note-ons on channel ch fired at 16th k, logged from event `from` on: the count, the lowest note in *low, and in *part
 * the part that played them */
static int ons_from(int from, int ch, long k, int *low, int *part) {
    int n = 0;
    *low = 128;
    *part = -1;
    for (int i = from; i < nev; i++) {
        if (!is_on(&evs[i]) || (evs[i].status & 15) != ch - 1 || evs[i].k != k) continue;
        n++;
        if (evs[i].note < *low) *low = evs[i].note;
        *part = evs[i].part;
    }
    return n;
}

static int ons_at(int ch, long k, int *low, int *part) { return ons_from(0, ch, k, low, part); }

/* stopped: the Chorus (index 3) made 2 bars of 1-5-6-4 (Em Bm C Am), then back to the 4-bar Verse; Melody 1 on every step */
static void chorus_setup(seq_t *s) {
    fresh(s, 1);
    seq_select_part(s, 3);
    seq_set_bars(s, 2);
    seq_set_prog_preset(s, 1);
    seq_select_part(s, 0);
}

static void test_part_switch_at_bar_line(void) {
    static seq_t s;
    int low, part, low48, low64, bad = 0, once = 1;
    chorus_setup(&s);
    CHECK(seq_part(&s) == 0 && seq_bars(&s) == 4 && s.part[3].bars == 2, "the Verse plays (4 bars), the Chorus has 2");
    double ppq = play(&s, 0, 9.5, 0);
    seq_select_part(&s, 3);
    CHECK(seq_part(&s) == 0 && seq_queued_part(&s) == 3, "a tap at beat 9.5 (bar 3) queues the Chorus");
    ev_seq = &s;
    for (; ppq < 12.0 - 1e-9; ppq += blk()) seq_block(&s, ppq, bpm, 1, 128);
    for (int i = 0; i < nev; i++) bad += is_on(&evs[i]) && evs[i].part != 0;
    CHECK(!bad && seq_part(&s) == 0 && seq_queued_part(&s) == 3, "no Chorus note before beat 12 (%d)", bad);
    for (; ppq < 21.0; ppq += blk()) seq_block(&s, ppq, bpm, 1, 128);
    ev_seq = NULL;
    CHECK(seq_part(&s) == 3 && seq_queued_part(&s) == -1 && seq_bars(&s) == 2, "the Chorus plays, nothing queued");
    CHECK(ons_at(2, 47, &low, &part) == 0 && ons_at(4, 47, &low, &part) == 1 && part == 0, "step 47: the Verse's last 16th");
    CHECK(ons_at(2, 48, &low48, &part) == 3 && part == 3 && low48 == 52, "beat 12: the Chorus's bar 1 Em, once (low %d)", low48);
    CHECK(ons_at(2, 64, &low64, &part) == 3 && part == 3 && low64 != 52, "beat 16: its bar 2 (Bm, low %d)", low64);
    CHECK(ons_at(2, 80, &low, &part) == 3 && part == 3 && low == low48, "beat 20: its bar 1 again");
    for (long k = 36; k < 84; k++) once &= ons_at(4, k, &low, &part) == 1;
    CHECK(once, "every 16th from beat 9 to 21 fired exactly once across the switch");
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");
}

/* after a switch at beat 12 a host loop back to 0 keeps the Chorus, on its own 2 bars counted from the switch */
static void test_switch_then_loop_wrap(void) {
    static seq_t s;
    int low, part, low_b1, low_b2, bad = 0, max_bar = 0;
    chorus_setup(&s);
    double ppq = play(&s, 0, 9.5, 0);
    seq_select_part(&s, 3);
    ppq = play(&s, ppq, 8.0, 0);   /* to beat 17.5: Chorus bar 1 at 12, bar 2 at 16 */
    ons_at(2, 48, &low_b1, &part);
    ons_at(2, 64, &low_b2, &part);
    int at = nev;
    ev_seq = &s;
    for (ppq = 0; ppq < 8.0; ppq += blk()) {
        seq_block(&s, ppq, bpm, 1, 128);
        if (seq_cur_bar(&s) > max_bar) max_bar = seq_cur_bar(&s);
    }
    ev_seq = NULL;
    for (int i = at; i < nev; i++) bad += is_on(&evs[i]) && evs[i].part != 3;
    CHECK(!bad && seq_part(&s) == 3 && max_bar == 1, "the loop back plays only the Chorus, bars 1-2 (%d, max bar %d)", bad, max_bar + 1);
    CHECK(ons_from(at, 2, 0, &low, &part) == 3 && low == low_b2, "song beat 0 is its bar 2 (4 bars after the switch, mod 2)");
    CHECK(ons_from(at, 2, 16, &low, &part) == 3 && low == low_b1, "song beat 4 its bar 1");
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");
}

static void test_part_cancel_and_stopped_select(void) {
    static seq_t s;
    fresh(&s, 0);
    double ppq = play(&s, 0, 1.0, 0);
    seq_select_part(&s, 3);
    seq_select_part(&s, 3);
    CHECK(seq_queued_part(&s) == -1 && seq_part(&s) == 0, "tapping the queued part again cancels");
    seq_select_part(&s, 3);
    seq_select_part(&s, 0);
    CHECK(seq_queued_part(&s) == -1 && seq_part(&s) == 0, "tapping the playing part cancels too");
    seq_select_part(&s, 3);
    seq_select_part(&s, 4);
    CHECK(seq_queued_part(&s) == 4, "tapping another part queues that one instead");
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(seq_part(&s) == 4 && seq_queued_part(&s) == -1, "a stop starts the queued part at once");
    seq_select_part(&s, 2);
    CHECK(seq_part(&s) == 2 && seq_queued_part(&s) == -1, "stopped: a tap switches at once");
    seq_select_part(&s, SEQ_NPARTS);
    seq_select_part(&s, -1);
    CHECK(seq_part(&s) == 2 && seq_queued_part(&s) == -1, "out of range is ignored");
    nev = 0;
    play(&s, 0, 4.0, 0);
    int low, part;
    CHECK(ons_at(2, 0, &low, &part) == 3 && part == 2, "and plays from the song position");
    CHECK(!strcmp(SEQ_PART_NAME[0], "Verse") && !strcmp(SEQ_PART_NAME[SEQ_PART_SCREEN[0]], "Intro") &&
          !strcmp(SEQ_PART_NAME[5], "Outro") && SEQ_PART_SCREEN[1] == 0, "names and screen order");
}

static void test_part_first_use_copies_verse(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    old_bass(&s, 30);
    seq_generate_all(&s);   /* the Verse differs from a new instance's */
    seq_select_part(&s, 3);
    CHECK(seq_part(&s) == 3 && !memcmp(&s.part[3], &s.part[0], sizeof s.part[0]), "a part's first use is a copy of the Verse");
    seq_generate(&s, LANE_BASS);
    int seed = s.part[3].lane[LANE_BASS].gen.seed;
    seq_select_part(&s, 0);
    seq_select_part(&s, 3);
    CHECK(s.part[3].lane[LANE_BASS].gen.seed == seed && seed != s.part[0].lane[LANE_BASS].gen.seed, "later uses keep its own");
}

static void test_part_used_mask(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    int others = 0;
    seq_init(&s, emit, NULL);
    for (int p = 1; p < SEQ_NPARTS; p++) others += seq_part_used(&s, p);
    CHECK(seq_part_used(&s, 0) && !others, "a new instance uses only the Verse");
    seq_select_part(&s, 2);
    CHECK(seq_part_used(&s, 2) && !seq_part_used(&s, 1), "a selected part is in use");
    play(&s, 0, 1.0, 0);
    seq_select_part(&s, 5);
    CHECK(seq_part_used(&s, 5) && seq_part(&s) == 2, "so is a queued one, before it plays");
    seq_select_part(&s, 5);   /* cancelled: still in use (it is the Verse's copy) */
    CHECK(seq_part_used(&s, 5), "a cancelled queue leaves it in use");
    seq_block(&s, 1.0, bpm, 0, 128);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(seq_state_load(&s, buf) && seq_part(&s) == 0 && seq_queued_part(&s) == -1 && seq_part_used(&s, 0) &&
          seq_part_used(&s, 2) && seq_part_used(&s, 5) && !seq_part_used(&s, 1), "a load plays the Verse, the saved parts in use");
}

/* 0.26 note-ons on channel ch logged from `from` while part `part` played */
static int ons_in(int from, int ch, int part) {
    int n = 0;
    for (int i = from; i < nev; i++) n += is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].part == part;
    return n;
}

/* 0.26 each part keeps its own mutes: a first use copies the Verse's, a switch brings the new part's in (at the bar line
 * when queued), undo and an unmute elsewhere leave them, a save keeps them (`pm=1`), and a state without `pm` gives every
 * part A's, as the global mutes before 0.26 played */
static void test_mutes_per_part(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    fresh(&s, 0);
    seq_set_mute(&s, LANE_BASS, 1);
    seq_select_part(&s, 3);
    CHECK(seq_mute(&s, LANE_BASS) && s.part[3].lane[LANE_BASS].mute, "a first use copies the Verse's mutes");
    seq_set_mute(&s, LANE_BASS, 0);
    CHECK(!seq_mute(&s, LANE_BASS) && s.part[0].lane[LANE_BASS].mute, "unmuted in the Chorus, the Verse keeps it muted");
    seq_select_part(&s, 0);
    CHECK(seq_mute(&s, LANE_BASS), "back in the Verse, the Bass is muted");
    seq_select_part(&s, 3);
    CHECK(!seq_mute(&s, LANE_BASS), "and in the Chorus not");
    seq_select_part(&s, 0);
    nev = 0;
    double ppq = play(&s, 0, 9.5, 0);
    seq_select_part(&s, 3);
    ev_seq = &s;
    play_to(&s, &ppq, 12.0);
    CHECK(seq_part(&s) == 0 && ons_in(0, 3, 0) == 0 && count_on(2) > 0, "playing the Verse: no Bass (%d), Chords play",
          ons_in(0, 3, 0));
    play_to(&s, &ppq, 16.0);
    int from = nev;
    CHECK(seq_part(&s) == 3 && ons_in(0, 3, 3) > 0, "the queued Chorus starts at the bar line with its Bass (%d)", ons_in(0, 3, 3));
    seq_set_mute(&s, LANE_MEL_UP, 1);
    seq_select_part(&s, 0);
    play_to(&s, &ppq, 24.0);
    CHECK(seq_part(&s) == 0 && seq_mute(&s, LANE_BASS) && !seq_mute(&s, LANE_MEL_UP) && ons_in(from, 3, 0) == 0,
          "the Verse back at its bar line: Bass muted again, Melody 1 (muted in the Chorus) not");
    seq_block(&s, ppq, bpm, 0, 128);
    ev_seq = NULL;
    CHECK(balanced(), "no stuck notes across the switches");
    seq_generate(&s, LANE_BASS);
    seq_set_mute(&s, LANE_BASS, 0);
    seq_undo(&s);
    CHECK(!seq_mute(&s, LANE_BASS), "undo leaves the mute");
    seq_set_mute(&s, LANE_BASS, 1);
    CHECK(seq_state_save(&s, buf, sizeof buf) && strstr(buf, "|pm=1|") && strstr(buf, "|A2=1:") && strstr(buf, "|D2=0:") &&
          strstr(buf, "|A3=0:") && strstr(buf, "|D3=1:"), "a save writes each part's mutes, and pm=1");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_mute(&t, LANE_BASS) && !seq_mute(&t, LANE_MEL_UP) && !t.part[3].lane[LANE_BASS].mute &&
          t.part[3].lane[LANE_MEL_UP].mute, "a load gives each part its own");
    CHECK(resaves_same(&t), "and saves back the same");
    seq_select_part(&s, 3);
    seq_set_mute(&s, LANE_BASS, 1);
    seq_set_mute(&s, LANE_MEL_UP, 0);
    CHECK(seq_state_save(&s, buf, sizeof buf) && !strstr(buf, "|pm=") && strstr(buf, "|A2=1:") && strstr(buf, "|D2=0:"),
          "the parts' mutes alike: no pm, B-F write 0, as 0.25 saved");
    CHECK(seq_state_load(&t, buf) && t.part[3].lane[LANE_BASS].mute && t.part[0].lane[LANE_BASS].mute,
          "a state without pm: every part takes A's mutes");
}

/* key and scale changes regenerate the playing part (and a queued one); the others catch up when next selected */
static void test_lazy_regen_on_key_change(void) {
    static seq_t s, r;
    static seq_part_t old;
    seq_init(&s, emit, NULL);
    seq_select_part(&s, 3);
    seq_generate_all(&s);   /* a Chorus of its own */
    seq_select_part(&s, 0);
    double ppq = play(&s, 0, 2.0, 0);
    old = s.part[3];
    seq_set_key(&s, 7);
    CHECK((s.stale_mask >> 3 & 1) && !(s.stale_mask & 1), "a key change while the Verse plays leaves the Chorus stale");
    CHECK(!(s.stale_mask & ~s.used_mask), "only parts in use go stale");
    ppq = play(&s, ppq, 8.0, 0);
    CHECK(!memcmp(&old, &s.part[3], sizeof old), "playing never regenerates it");
    seq_select_part(&s, 3);   /* queued: it catches up in the tap */
    seq_init(&r, emit, NULL);   /* the same Chorus made in the new key */
    seq_set_key(&r, 7);
    seq_select_part(&r, 3);
    seq_generate_all(&r);
    CHECK(!(s.stale_mask >> 3 & 1) && !memcmp(&r.part[3], &s.part[3], sizeof r.part[3]),
          "queueing it regenerates it to what a fresh generation in the new key gives");
    seq_set_scale(&s, SCALE_DORIAN);
    seq_set_scale(&r, SCALE_DORIAN);
    CHECK(!(s.stale_mask >> 3 & 1) && !memcmp(&r.part[3], &s.part[3], sizeof r.part[3]), "a queued part follows a change at once");
    play(&s, ppq, 4.0, 0);
    seq_set_key(&s, 2);
    CHECK(seq_part(&s) == 3 && (s.stale_mask & 1) && !(s.stale_mask >> 3 & 1), "the Chorus plays; a key change leaves the Verse stale");
    seq_block(&s, ppq + 4.0, bpm, 0, 128);
}

/* GEN in the Chorus, back to the Verse, UNDO: the Chorus is restored, the Verse untouched and still playing */
static void test_undo_across_parts(void) {
    static seq_t s;
    static seq_part_t chorus, verse;
    seq_init(&s, emit, NULL);
    seq_select_part(&s, 3);
    old_bass(&s, 30);   /* 0.19: GEN leaves a DRAWN Bass; 0.18's generated Root lane */
    chorus = s.part[3];
    int n = s.undo_n;
    seq_generate(&s, LANE_BASS);
    CHECK(s.undo_n == n + 1 && memcmp(&chorus, &s.part[3], sizeof chorus), "GEN in the Chorus changes it: one undo step");
    seq_select_part(&s, 0);
    verse = s.part[0];
    double ppq = play(&s, 0, 2.0, 0);
    seq_undo(&s);
    CHECK(!memcmp(&chorus, &s.part[3], sizeof chorus) && !memcmp(&verse, &s.part[0], sizeof verse) && seq_part(&s) == 0 &&
          seq_queued_part(&s) == -1, "undo restores the Chorus, leaves the Verse, does not switch");
    seq_block(&s, ppq, bpm, 0, 128);
}

/* the same on-steps in the first n (an off step's other fields are not saved) */
static int same_on_steps(const seq_step_t *x, const seq_step_t *y, int n) {
    for (int i = 0; i < n; i++)
        if (x[i].on != y[i].on || (x[i].on && memcmp(&x[i], &y[i], sizeof x[i]))) return 0;
    return 1;
}

/* part pa of a and part pb of b: the same loop, progression, voicing, HARMONY and lanes (steps in the loop, recipes, locks).
 * 0.14: the loop's slots (hidden slots a shrink left past the saved phrase do not save) */
static int same_part_at(const seq_t *a, int pa, const seq_t *b, int pb) {
    const seq_part_t *p = &a->part[pa], *q = &b->part[pb];
    if (p->bars != q->bars || p->chord_len != q->chord_len || p->prog_len != q->prog_len || p->voicing != q->voicing ||
        p->harmony != q->harmony || p->harmony_seed != q->harmony_seed ||
        memcmp(p->prog, q->prog, sizeof p->prog[0] * (size_t)p->prog_len) || memcmp(p->voice, q->voice, sizeof p->voice))
        return 0;
    for (int l = 0; l < SEQ_LANES; l++) {
        const seq_lane_t *x = &p->lane[l], *y = &q->lane[l];
        int conv = l == LANE_CHORDS ? chords_loaded_as(x, y, p->bars)   /* 0.18: a 0.17 lane converted */
                   : l == LANE_BASS ? bass_loaded_as(x, y, p->bars) : -1;   /* 0.19: a 0.18 lane */
        if (conv >= 0) {
            if (!conv) return 0;
            continue;
        }
        if (memcmp(&x->gen, &y->gen, sizeof x->gen) || memcmp(x->lock, y->lock, sizeof x->lock) ||
            !same_on_steps(x->steps, y->steps, p->bars * SEQ_STEPS))
            return 0;
    }
    return 1;
}

/* three parts in use: the Verse, an Intro (2 bars, 1/2-bar chords, new seeds, a lock, a Manual Melody 1 with notes) and a
 * Chorus (another progression, close voicing, a new Bass seed); Bass muted in all three (0.26: as 0.8's global mute) */
static void three_parts(seq_t *s) {
    static const int V1[4] = {0, 5, 2, 6};
    seq_init(s, emit, NULL);
    old_chords(s);
    old_bass(s, 30);
    old_mel(s);
    seq_select_part(s, 1);
    seq_set_bars(s, 2);
    seq_set_chord_len(s, 8);
    seq_set_progression(s, 4, V1, NULL, NULL, 0);   /* 0.14: the Verse's chords again at the new length, as 0.13 kept them */
    seq_generate_all(s);
    seq_set_step(s, LANE_BASS, 1, 5, !s->part[1].lane[LANE_BASS].steps[16 + 5].on);
    seq_set_style(s, LANE_MEL_UP, GEN_MANUAL);
    seq_set_step(s, LANE_MEL_UP, 0, 3, 1);
    seq_select_part(s, 3);
    seq_set_prog_preset(s, 2);
    seq_set_voicing(s, VOICING_CLOSE);
    seq_generate(s, LANE_BASS);
    seq_select_part(s, 0);
    seq_set_mute(s, LANE_BASS, 1);
    s->part[1].lane[LANE_BASS].mute = s->part[3].lane[LANE_BASS].mute = 1;
}

static void test_state_parts_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    three_parts(&s);
    int n = seq_state_save(&s, buf, sizeof buf);
    const char *b = strstr(buf, "|B.len=2|B.cl=8|B.h="), *d = strstr(buf, "|D.len=4|D.cl=16|D.h=");
    CHECK(n > 0 && b && d && b < d && strstr(buf, "|A2=1:") && strstr(buf, "|B2=0:") && strstr(buf, "|D2=0:") &&
          strstr(buf, "|B2.lk=") && strstr(buf, "|B.voi=1") && strstr(buf, "|D.voi=0") && !strstr(buf, "|C.") &&
          !strstr(buf, "|E.") && !strstr(buf, "|F.") && !seq_unsaved_parts(&s),
          "the parts in use save as A, B, D; B-F lanes write mute 0 (0.26: the parts' mutes alike); nothing left out (%d bytes)", n);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf), "and load");
    CHECK(seq_part_used(&t, 0) && seq_part_used(&t, 1) && !seq_part_used(&t, 2) && seq_part_used(&t, 3) &&
          !seq_part_used(&t, 4) && !seq_part_used(&t, 5) && seq_part(&t) == 0 && !t.stale_mask,
          "the Verse plays, Intro and Chorus in use, the rest unused");
    CHECK(same_part_at(&s, 0, &t, 0) && same_part_at(&s, 1, &t, 1) && same_part_at(&s, 3, &t, 3),
          "every part loads back: steps, recipes, locks, progression, voicing");
    CHECK(!strstr(buf, "|pm=") && seq_mute(&t, LANE_BASS) && !seq_mute(&t, LANE_CHORDS) && t.part[1].lane[LANE_BASS].mute &&
          t.part[3].lane[LANE_BASS].mute, "0.26 every part takes A's mutes (no pm)");
    CHECK(resaves_same(&t), "save -> load -> save gives the same string (0.18: once loaded, the Chords lanes DRAWN bars)");
    seq_select_part(&t, 1);
    CHECK(seq_part(&t) == 1 && same_part_at(&s, 1, &t, 1), "a loaded part plays as saved, no first-use copy of the Verse");
}

/* a 0.7 state (no B-F) is the Verse alone, and saves back byte for byte */
static void test_state_07_is_verse(void) {
    static seq_t s;
    static const char S07[] = "csq2|gv=1|k=9|s=0|A.len=2|A.cl=8|A.h=55,321|A.prog=0352|A.voi=1|ch=1,2,3|sw=40|gt=70|dy=20"
                              "|A1=0:|A1.g=3,4242,55|A2=1:|A2.g=6,777,80|A3=0:0015640|A4=0:";
    seq_init(&s, emit, NULL);
    seq_select_part(&s, 2);
    CHECK(seq_state_load(&s, S07), "a 0.7 state loads");
    int others = 0;
    for (int p = 1; p < SEQ_NPARTS; p++) others += seq_part_used(&s, p);
    CHECK(seq_part(&s) == 0 && !others && seq_mute(&s, LANE_BASS) && !seq_mute(&s, LANE_CHORDS), "as the Verse, nothing else in use, A2's mute the Verse's");
    CHECK(saves_but_drawn(&s, S07), "and saves back byte for byte (0.18: but its Chords lane, as DRAWN bars)");
}

/* a part with a missing or malformed field loads as unused; the rest of the state loads */
static void test_state_bad_part_dropped(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], bad[2 * SEQ_STATE_MAX];   /* room for the appended fields */
    three_parts(&s);
    seq_select_part(&s, 2);   /* the Pre: a copy of the Verse */
    seq_set_bars(&s, 8);
    seq_select_part(&s, 0);
    seq_state_save(&s, buf, sizeof buf);
    const char *c = strstr(buf, "|C.prog=");
    CHECK(c != NULL, "the Pre saves as C");
    int at = c ? (int)(c - buf) + 8 : 0;
    snprintf(bad, sizeof bad, "%.*s9%s", at, buf, buf + at + 1);   /* C.prog's first degree 9: malformed */
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, bad) && seq_part_used(&t, 1) && !seq_part_used(&t, 2) && seq_part_used(&t, 3) &&
          same_part_at(&s, 1, &t, 1) && same_part_at(&s, 3, &t, 3) && same_part_at(&s, 0, &t, 0),
          "a malformed C.prog drops only the Pre");
    const char *c2 = strstr(buf, "|C2=");
    const char *c2e = c2 ? strchr(c2 + 1, '|') : NULL;
    snprintf(bad, sizeof bad, "%.*s%s", c2 ? (int)(c2 - buf) : 0, buf, c2e ? c2e : "");
    seq_init(&t, emit, NULL);
    CHECK(c2e && seq_state_load(&t, bad) && !seq_part_used(&t, 2) && seq_part_used(&t, 1) && seq_part_used(&t, 3),
          "a missing C2 drops only the Pre");
    snprintf(bad, sizeof bad, "%s|G.len=4|G.cl=16|G.prog=0|G1=0:|G2=0:|G3=0:|G4=0:|C.len=3", buf);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, bad) && !seq_part_used(&t, 2) && seq_part_used(&t, 3) && seq_part_used(&t, 1),
          "an unknown letter is ignored; a bad later C.len drops the Pre too");
}

/* Review Focus 4: six parts with three dense hand-drawn lanes cannot all fit the wrapper's 8192 bytes */
static void dense_parts(seq_t *s, int bars) {
    fresh(s, 0);
    seq_set_bars(s, bars);
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++)
        for (int i = 0; i < bars * SEQ_STEPS; i++) seq_set_step(s, l, i / 16, i % 16, 1);
    for (int p = 1; p < SEQ_NPARTS; p++) seq_select_part(s, p);
    seq_select_part(s, 0);
}

static void test_state_overflow(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    dense_parts(&s, 16);
    int n = seq_state_save(&s, buf, sizeof buf);
    /* 0.18 (R19): the Chords lane saves as compact drawn bars, so the Intro fits beside the Verse and 8 bars fit four parts */
    CHECK(n > 0 && n < SEQ_STATE_MAX && strstr(buf, "|A.len=16") && strstr(buf, "|A3=0:") && strstr(buf, "|B.") && !strstr(buf, "|C.") &&
          seq_unsaved_parts(&s) == 0x3c, "16 bars: the Verse and Intro save (%d bytes), C-F left out whole and named (mask 0x%x)", n,
          seq_unsaved_parts(&s));
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && same_part_at(&s, 0, &t, 0) && same_part_at(&s, 1, &t, 1) && !seq_part_used(&t, 2),
          "the Verse and Intro load back alone");
    dense_parts(&s, 8);
    n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && n < SEQ_STATE_MAX && strstr(buf, "|D.len=8") && !strstr(buf, "|E.") && !strstr(buf, "|F.") &&
          seq_unsaved_parts(&s) == 0x30 && buf[n] == 0 && (int)strlen(buf) == n,
          "8 bars: A-D fit (%d bytes), the Bridge and Outro are left out (mask 0x%x)", n, seq_unsaved_parts(&s));
    seq_init(&t, emit, NULL);
    int ok = seq_state_load(&t, buf);
    for (int p = 0; p < 4; p++) ok &= seq_part_used(&t, p) && same_part_at(&s, p, &t, p);
    CHECK(ok && !seq_part_used(&t, 4) && !seq_part_used(&t, 5), "and A-D load back");
    for (int p = 3; p < SEQ_NPARTS; p++) {   /* the Chorus too: 8 dense bars leave a 1-bar part no room */
        seq_select_part(&s, p);
        seq_set_bars(&s, 1);
    }
    n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "|F.len=1") && !seq_unsaved_parts(&s), "a 1-bar Chorus, Bridge and Outro fit: the next save clears the mask (%d bytes)", n);
}

/* a save while the Chorus plays still writes the Verse as A */
static void test_save_while_chorus_plays(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    chorus_setup(&s);
    double ppq = play(&s, 0, 9.5, 0);
    seq_select_part(&s, 3);
    ppq = play(&s, ppq, 4.0, 0);
    CHECK(seq_part(&s) == 3, "the Chorus plays");
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|A.len=4|") && strstr(buf, "|D.len=2|"),
          "the save writes the Verse (4 bars) as A, the Chorus (2 bars) as D");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_part(&t) == 0 && same_part_at(&s, 0, &t, 0) && same_part_at(&s, 3, &t, 3),
          "and loads as the Verse, the Chorus as it was");
    seq_block(&s, ppq, bpm, 0, 128);
}

/* an 8-bar Verse viewing bar 7 (FOLLOW off) switches to a 2-bar Chorus with 1/4-bar chords, swing 100 */
static void test_switch_view_and_grid(void) {
    static seq_t s;
    int low, part, once = 1;
    fresh(&s, 1);
    seq_set_bars(&s, 8);
    seq_select_part(&s, 3);
    seq_set_bars(&s, 2);
    seq_set_chord_len(&s, 4);
    seq_select_part(&s, 0);
    seq_set_swing(&s, 100);
    seq_set_follow(&s, 0);
    seq_set_view(&s, 6);
    CHECK(!seq_follow(&s) && seq_view_bar(&s) == 6, "the Verse's bar 7 on view, FOLLOW off");
    double ppq = play(&s, 0, 30.0, 0);   /* bar 8 */
    seq_select_part(&s, 3);
    ev_seq = &s;
    for (; ppq < 33.1; ppq += blk()) seq_block(&s, ppq, bpm, 1, 128);   /* the switch at beat 32 (16th 128) */
    ev_seq = NULL;
    CHECK(seq_part(&s) == 3 && seq_view_bar(&s) == 0 && seq_strip_bar(&s) < 2, "the Chorus plays; view %d, strip %d",
          seq_view_bar(&s), seq_strip_bar(&s));
    CHECK(s.cur_k == 132 && seq_cur_slot(&s) == 1, "16th 132 is the Chorus's step 5: its 2nd chord (slot %d)", seq_cur_slot(&s));
    ppq = play(&s, ppq, 3.0, 0);
    for (long k = 112; k < 140; k++) once &= ons_at(4, k, &low, &part) == 1;
    CHECK(once, "every 16th from beat 28 to 35 fired exactly once across the switch, swung");
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");
}


/* 0.9: chord quality per slot, slot locks, chord shaping, optional save fields */

/* FNV-1a over the logged events' fields (not their bytes: padding), the block position to a millionth of a beat */
static unsigned fold_events(unsigned h) {
    for (int i = 0; i < nev; i++) {
        long x[6] = {evs[i].status, evs[i].note, evs[i].vel, evs[i].k, evs[i].part, (long)(evs[i].ppq * 1e6 + 0.5)};
        for (int j = 0; j < 6; j++) h = (h ^ (unsigned)x[j]) * 16777619u;
    }
    return h;
}

static void three_parts(seq_t *s);

/* Every scale x preset with generated lanes of every style, both voicings, 1/2-bar chords, a non-neutral output stage,
 * played as a 4-bar host loop twice; then three parts with a switch. Pinned on the 0.8 build (Review Focus 1). */
static unsigned capture_neutral(void) {
    static seq_t s;
    unsigned h = 2166136261u;
    for (int sc = 0; sc < SCALE_COUNT; sc++)
        for (int p = 0; p < THEORY_NPRESETS; p++) {
            seq_init(&s, emit, NULL);
            old_chords(&s);
            old_bass(&s, 30);
            old_mel(&s);
            seq_set_scale(&s, sc);
            if (p % 4 == 2) seq_set_chord_len(&s, 8);   /* 0.14: before the preset (a chord-length change remaps by time) */
            seq_set_prog_preset(&s, p);
            seq_set_style(&s, LANE_CHORDS, GEN_CH_SUSTAIN + p % 3);
            seq_set_style(&s, LANE_BASS, GEN_BASS_ROOT + (p / 3) % 3);
            seq_set_style(&s, LANE_MEL_UP, p & 4 ? GEN_MEL_ARP : GEN_MEL_HOOK);
            if (p & 1) seq_set_voicing(&s, VOICING_CLOSE);
            if (sc == 2) { seq_set_swing(&s, 40); seq_set_gate(&s, 20); seq_set_dynamics(&s, 90); }
            nev = 0;
            play(&s, 0.0, 32.0, 16.0);
            seq_block(&s, 0.0, bpm, 0, 128);
            h = fold_events(h);
        }
    three_parts(&s);
    nev = 0;
    double ppq = play(&s, 0.0, 6.0, 0);
    seq_select_part(&s, 1);
    ppq = play(&s, ppq, 12.0, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    return fold_events(h);
}

#define NEUTRAL_GOLDEN 0x9241c97au

static void strip_field(char *buf, const char *name);

/* the text three parts and a default instance save, folded: pinned on the 0.8 build (0.17's `.vx` field is left out) */
static unsigned saves_hash(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    unsigned h = 2166136261u;
    for (int i = 0; i < 2; i++) {
        if (i) three_parts(&s);
        else {
            seq_init(&s, emit, NULL);
            old_chords(&s);
            old_bass(&s, 30);
            old_mel(&s);
        }
        seq_state_save(&s, buf, sizeof buf);
        strip_field(buf, "vx");
        for (int j = 0; buf[j]; j++) h = (h ^ (unsigned char)buf[j]) * 16777619u;
    }
    return h;
}

#define SAVES_GOLDEN 0xf2213770u

static void test_09_neutral_is_08(void) {
    unsigned h = capture_neutral();
    CHECK(h == NEUTRAL_GOLDEN, "neutral 0.9 plays 0.8's MIDI: every scale, preset, style and voicing, a loop wrap, a part switch (got 0x%08x)", h);
    h = saves_hash();
    CHECK(h == SAVES_GOLDEN, "and saves 0.8's text (got 0x%08x)", h);
}

/* 0.8's revoice, copied: the voice table a progression of triads with no borrowed slot must still get */
static void revoice_08(int key, int scale, const int *prog, int n, int voicing, int v[][3]) {
    if (voicing != VOICING_SMOOTH) {
        for (int i = 0; i < n; i++) theory_triad(key, scale, prog[i], v[i]);
        return;
    }
    theory_triad(key, scale, prog[0], v[0]);
    if (n < 2) return;
    for (int i = 1; i < n; i++) theory_voice_next(key, scale, prog[i], v[i - 1], v[i]);
    theory_voice_next(key, scale, prog[0], v[n - 1], v[0]);
    for (int i = 1; i < n; i++) theory_voice_next(key, scale, prog[i], v[i - 1], v[i]);
}

/* the playing part's voice table equals 0.8's for its progression, every slot 3 notes (0.14: 0.8's progression is the
 * loop's phrase; each slot plays its phrase position's voice) */
static int voices_as_08(const seq_t *s) {
    const seq_part_t *p = &s->part[s->cur_part];
    int v[SEQ_MAX_PROG][3], n = p->phrase;
    revoice_08(s->key, s->scale, p->prog, n, p->voicing, v);
    for (int i = 0; i < p->prog_len; i++)
        if (p->voice_n[i] != 3 || memcmp(p->voice[i], v[i % n], sizeof v[i])) return 0;
    return 1;
}

static void test_09_triads_voice_as_08(void) {
    static seq_t s;
    int bad = 0;
    seq_init(&s, emit, NULL);
    for (int vo = 0; vo < 2; vo++)
        for (int k = 0; k < 12; k++)
            for (int sc = 0; sc < SCALE_COUNT; sc++)
                for (int p = 0; p < THEORY_NPRESETS; p++) {
                    seq_set_voicing(&s, vo);
                    seq_set_key(&s, k);
                    seq_set_scale(&s, sc);
                    seq_set_prog_preset(&s, p);
                    bad += !voices_as_08(&s);
                }
    CHECK(!bad, "triad slots with no borrow voice exactly as 0.8 (both voicings, 12 keys, every scale and preset; %d differ)", bad);
}

/* the first note-on on channel ch at or after event `from`, -1 if none */
static int first_on(int from, int ch) {
    for (int i = from; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1) return evs[i].note;
    return -1;
}

/* Review Focus 2: E minor, slot 1 = Borrow 1 (IV, A major): Chords A C# E, the Bass root A, Melody tone 3 C# */
static void test_09_borrowed_in_every_lane(void) {
    static seq_t s;
    char buf[16], num[16];
    int got[16];
    fresh(&s, 0);   /* v1's Manual lanes, close voicing: Chords bar 1 step 1, Bass steps 1 and 9, Melody 1 arps R 3 5 */
    seq_set_slot(&s, 0, 0, 1, COL_TRIAD);
    seq_slot_label(&s, 0, buf, sizeof buf);
    seq_slot_numeral(&s, 0, num, sizeof num);
    CHECK(seq_slot_deg(&s, 0) == 3 && seq_slot_src(&s, 0) == 1 && seq_slot_colour(&s, 0) == COL_TRIAD &&
          seq_slot_locked(&s, 0) && seq_can_undo(&s) && !strcmp(buf, "A") && !strcmp(num, "IV"),
          "FIX slot 1 to Borrow 1: degree 3 from Major, locked, an undo step, \"A\" \"IV\" (got %s %s)", buf, num);
    nev = 0;
    play(&s, 0.0, 3.9, 0);
    seq_block(&s, 3.9, bpm, 0, 128);
    CHECK(ons(2, got, 3) == 3 && got[0] == 57 && got[1] == 61 && got[2] == 64, "Chords play A3 C#4 E4 (%d %d %d)", got[0], got[1], got[2]);
    CHECK(first_on(0, 3) == 45, "the Bass root is A2 (%d)", first_on(0, 3));
    CHECK(ons(4, got, 3) == 3 && got[0] == 69 && got[1] == 73 && got[2] == 76, "Melody 1's tone 3 is C# (%d %d %d)", got[0], got[1], got[2]);
    /* sus4 on slot 3 (G): Melody 1's tone 3 becomes tone 4 (C) */
    seq_set_slot(&s, 2, 2, 0, COL_SUS4);
    play(&s, 0.0, 8.0, 0);   /* 0.16: a start plays the part's bar 1, so bar 3 is reached by playing through */
    nev = 0;
    play(&s, 8.0, 3.9, 0);
    seq_block(&s, 11.9, bpm, 0, 128);
    CHECK(ons(2, got, 3) == 3 && got[0] == 55 && got[1] == 60 && got[2] == 62 && ons(4, got, 3) == 3 && got[1] == 72,
          "a sus4 slot: Chords G C D, Melody 1's tone 3 plays C (%d)", got[1]);
    /* a scale change re-derives a borrowed slot's degree in every part; minor's Borrow 3 is bII (degree 1), major's bVII (6) */
    seq_set_slot(&s, 1, 0, 3, COL_TRIAD);
    seq_slot_numeral(&s, 1, num, sizeof num);
    CHECK(seq_slot_deg(&s, 1) == 1 && !strcmp(num, "bII"), "Borrow 3 in minor: degree 1, bII (got %s)", num);
    seq_select_part(&s, 3);
    seq_select_part(&s, 0);
    seq_set_scale(&s, SCALE_MAJOR);
    seq_slot_numeral(&s, 1, num, sizeof num);
    CHECK(s.part[0].prog[1] == 6 && s.part[3].prog[1] == 6 && s.part[0].src[1] == 3 && !strcmp(num, "bVII"),
          "to major: degree 6 (bVII) in the Verse and the Chorus (got %d %d %s)", s.part[0].prog[1], s.part[3].prog[1], num);
}

/* bit i: slot i plays more than 3 notes */
static int coloured_mask(const seq_t *s) {
    int m = 0;
    for (int i = 0; i < s->part[s->cur_part].prog_len; i++) m |= (s->part[s->cur_part].voice_n[i] > 3) << i;
    return m;
}

static void test_09_colours(void) {
    static seq_t s;
    char buf[16];
    int got[8];
    fresh(&s, 0);
    seq_set_slot(&s, 0, 0, 0, COL_7TH);
    seq_slot_label(&s, 0, buf, sizeof buf);
    nev = 0;
    play(&s, 0.0, 1.0, 0);
    CHECK(s.part[0].voice_n[0] == 4 && !strcmp(buf, "Em7") && ons(2, got, 8) == 4 && got[0] == 52 && got[3] == 62,
          "a 7th slot plays 4 notes, E G B D, \"Em7\" (got %s)", buf);
    seq_set_bars(&s, 8);
    seq_set_prog_preset(&s, 15);   /* Canon: 8 slots (0.14: on an 8-bar loop), all Triad */
    CHECK(seq_slot_colour(&s, 0) == COL_TRIAD && !seq_slot_locked(&s, 0) && coloured_mask(&s) == 0, "a preset clears colours and locks");
    /* retired in 0.17: the 0.9 rank rule (COLOUR adds colour slot by slot, add9 from 67) no longer sounds; the COLOUR level does */
    seq_set_vknob(&s, VK_COL, 1);
    seq_slot_label(&s, 0, buf, sizeof buf);
    CHECK(coloured_mask(&s) == 0xff && !strcmp(buf, "Em7"), "COLOUR +1 makes every Triad slot a 7th (%s)", buf);
    seq_set_vknob(&s, VK_COL, 3);
    seq_slot_label(&s, 0, buf, sizeof buf);
    CHECK(coloured_mask(&s) == 0xff && !strcmp(buf, "Em11") && s.part[0].voice_n[0] == 5, "COLOUR +3: 11ths of 5 notes (%s)", buf);
    seq_set_vknob(&s, VK_COL, -1);
    seq_slot_label(&s, 0, buf, sizeof buf);
    CHECK(coloured_mask(&s) == 0 && !strcmp(buf, "E5") && s.part[0].voice_n[0] == 2, "COLOUR -1: Power chords of 2 notes (%s)", buf);
    seq_set_vknob(&s, VK_COL, 0);
    CHECK(coloured_mask(&s) == 0 && voices_as_08(&s), "back at 0: 0.8's voices");
    seq_set_chord_shape(&s, SHAPE_COLOUR, 150);
    seq_set_chord_shape(&s, SHAPE_OCT, 5);
    seq_set_chord_shape(&s, SHAPE_COUNT, 7);
    CHECK(seq_chord_shape(&s, SHAPE_COLOUR) == 100 && seq_chord_shape(&s, SHAPE_OCT) == 1 && coloured_mask(&s) == 0,
          "SHAPE clamps (100, octave 1); SHAPE_COLOUR no longer colours");
    seq_set_vknob(&s, VK_COL, 9);
    seq_set_vknob(&s, VK_INV, -9);
    seq_set_vknob(&s, VK_SPR, 9);
    seq_set_vknob(&s, VK_STR, -9);
    seq_set_vknob(&s, VK_COUNT, 1);
    CHECK(seq_vknob(&s, VK_COL) == 4 && seq_vknob(&s, VK_INV) == -2 && seq_vknob(&s, VK_SPR) == 3 && seq_vknob(&s, VK_STR) == -8 &&
          seq_vknob(&s, VK_COUNT) == 0, "the 0.17 knobs clamp (COLOUR 4, INVERSION -2, SPREAD 3, STRUM -8)");
    seq_set_chord_shape(&s, SHAPE_OCT, -5);
    seq_set_chord_shape(&s, SHAPE_SPREAD, -3);
    CHECK(seq_chord_shape(&s, SHAPE_OCT) == -1 && seq_chord_shape(&s, SHAPE_SPREAD) == 0 && seq_chord_shape(&s, -1) == 0,
          "octave -1, spread 0");
}

/* channel ch's events (status, note, velocity, block position), each hashed and summed: the order within a block (which
 * the other lanes' note-offs shuffle) does not count */
static unsigned chan_hash(int ch) {
    unsigned h = 0;
    for (int i = 0; i < nev; i++)
        if ((evs[i].status & 15) == ch - 1) {
            unsigned x = ((unsigned)evs[i].status * 131u + (unsigned)evs[i].note) * 131u + (unsigned)evs[i].vel;
            x = (x ^ (unsigned)(long)(evs[i].ppq * 1e6 + 0.5)) * 2654435761u;
            h += x ^ x >> 15;
        }
    return h;
}

/* play from 0 for `beats`, then stop */
static void play_stop(seq_t *s, double beats) {
    double ppq = play(s, 0.0, beats, 0);
    seq_block(s, ppq, bpm, 0, 128);
}

/* the lowest note-on on channel ch logged from `from` with a block position in (a, b]; 128 if none */
static int low_between(int from, int ch, double a, double b) {
    int low = 128;
    for (int i = from; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].ppq > a && evs[i].ppq <= b && evs[i].note < low) low = evs[i].note;
    return low;
}

/* SPREAD, OCTAVE, GATE and DYNAMICS on the Chords lane only */
static void test_09_shape_chords_lane(void) {
    static seq_t s;
    int got[8];
    unsigned bass, chords;
    fresh(&s, 0);   /* close Em 52 55 59, vel 100 */
    nev = 0; play_stop(&s, 16.0);
    bass = chan_hash(3);
    seq_set_chord_shape(&s, SHAPE_SPREAD, 100);   /* retired in 0.17: SHAPE_SPREAD no longer sounds, vspr does (test_17_spread) */
    nev = 0; play_stop(&s, 1.0);
    CHECK(ons(2, got, 8) == 3 && got[0] == 52 && got[1] == 55 && got[2] == 59, "SHAPE_SPREAD leaves the voicing alone (%d %d %d)", got[0], got[1], got[2]);
    seq_set_chord_shape(&s, SHAPE_SPREAD, 0);
    seq_set_vknob(&s, VK_SPR, 1);
    nev = 0; play_stop(&s, 1.0);
    CHECK(ons(2, got, 8) == 3 && got[0] == 52 && got[1] == 59 && got[2] == 67, "SPREAD +1: the middle note up an octave (%d %d %d)", got[0], got[1], got[2]);
    seq_set_vknob(&s, VK_SPR, 3);
    nev = 0; play_stop(&s, 1.0);
    CHECK(ons(2, got, 8) == 3 && got[0] == 40 && got[1] == 59 && got[2] == 67, "SPREAD +3: and the lowest down one (%d)", got[0]);
    seq_set_vknob(&s, VK_SPR, 0);
    seq_set_chord_shape(&s, SHAPE_OCT, 1);
    nev = 0; play_stop(&s, 1.0);
    CHECK(ons(2, got, 8) == 3 && got[0] == 64 && got[2] == 71, "OCTAVE +1 (%d %d)", got[0], got[2]);
    seq_set_chord_shape(&s, SHAPE_OCT, 0);
    seq_set_chord_shape(&s, SHAPE_DYN, 0);
    nev = 0; play_stop(&s, 16.0);
    int flat = 1;
    for (int i = 0; i < nev; i++) flat &= !is_on(&evs[i]) || (evs[i].status & 15) != 1 || evs[i].vel == 64;
    CHECK(flat && chan_hash(3) == bass, "DYNAMICS 0: every chord at 64, the Bass untouched");
    seq_set_chord_shape(&s, SHAPE_DYN, 100);
    nev = 0; play_stop(&s, 1.0);
    CHECK(first_on(0, 2) == 52 && evs[0].vel == 127, "DYNAMICS 100: 64 + 36 x 2, clamped to 127 (%d)", evs[0].vel);
    seq_set_chord_shape(&s, SHAPE_DYN, 50);
    seq_set_chord_shape(&s, SHAPE_GATE, 0);
    nev = 0; play_stop(&s, 16.0);
    chords = chan_hash(2);
    CHECK(chan_hash(3) == bass, "GATE 0 leaves the Bass alone");
    seq_set_chord_shape(&s, SHAPE_GATE, 50);
    seq_set_gate(&s, 0);
    nev = 0; play_stop(&s, 16.0);
    CHECK(chan_hash(2) == chords, "and shortens the Chords as PLAY's GATE 0 does");
}

/* note-ons on channel ch whose block ended in (t, t + 1 block]: the notes emitted for an event time t */
static int ons_near(int from, int ch, double t) {
    int n = 0;
    for (int i = from; i < nev; i++)
        n += is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].ppq > t - 1e-9 && evs[i].ppq <= t + blk() + 1e-9;
    return n;
}

static int ons_between(int from, int ch, double a, double b) {
    int n = 0;
    for (int i = from; i < nev; i++) n += is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].ppq > a && evs[i].ppq <= b;
    return n;
}

/* Review Focus 5: PUSH at a loop wrap, a queued part switch, a stop and a locate */
static void test_09_push(void) {
    static seq_t s;
    fresh(&s, 0);   /* one chord per bar, Chords on step 1 of each bar */
    seq_set_chord_shape(&s, SHAPE_PUSH, 100);
    nev = 0;
    play(&s, 0.0, 15.95, 0);
    seq_block(&s, 15.95, bpm, 0, 128);
    CHECK(count_on(2) == 12 + 3 && ons_near(0, 2, 3.875) == 3 && ons_near(0, 2, 7.875) == 3 && ons_near(0, 2, 11.875) == 3 &&
          ons_near(0, 2, 15.875) == 3 && ons_between(0, 2, 3.9, 4.1) == 0, "PUSH 100: each chord change 1/8 beat early, once");
    CHECK(ons_near(0, 3, 4.0) == 1 && ons_between(0, 3, 3.8, 3.99) == 0 && balanced(), "the Bass stays on the beat; no hung notes");
    seq_set_chord_shape(&s, SHAPE_PUSH, 50);
    nev = 0;
    play(&s, 0.0, 4.1, 0);
    seq_block(&s, 4.1, bpm, 0, 128);
    CHECK(ons_near(0, 2, 3.9375) == 3, "PUSH 50: 1/16 beat early");
    /* a 4-bar host loop: the chord pushed before the wrap is bar 1's, and bar 1 plays again from the wrap */
    seq_set_chord_shape(&s, SHAPE_PUSH, 100);
    nev = 0;
    double ppq = play(&s, 0.0, 15.95, 16.0);
    int at = nev, low, part;
    int pushed = nev;   /* the events from the push on */
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == 1 && evs[i].ppq > 15.8) { pushed = i; break; }
    ppq = play(&s, ppq, 0.2, 16.0);
    low = low_between(0, 2, 15.8, 16.0);
    CHECK(ons_near(0, 2, 15.875) == 3 && low == 52, "at a loop wrap the push plays bar 1's chord (Em, low %d)", low);
    int offs = 0, cut = 0;
    for (int i = pushed; i < nev; i++)
        if (is_off(&evs[i]) && (evs[i].status & 15) == 1) {
            offs++;
            cut += evs[i].ppq > 1.0 || evs[i].ppq < 0.05;   /* ended at the wrap, not 0.225 beat after its push */
        }
    CHECK(ons_between(at, 2, 0.0, 0.2) == 0 && ons_between(pushed, 2, -1.0, 100.0) == 3 && offs == 3 && !cut,
          "and keeps sounding across the wrap: each note on once, off once at its own length, no strike at the wrap");
    ppq = play(&s, ppq, 4.0, 16.0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ons_near(at, 2, 3.875) == 3 && balanced(), "the next change pushes as usual; no hung notes over the wrap");
    /* a 3-bar host loop over the 4-bar part: the wrap lands on another step, so the pushed chord ends there */
    nev = 0;
    ppq = play(&s, 0.0, 36.0, 12.0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced() && max_open(2) <= 3, "a host loop that is not a multiple of the part: no hung notes");
    /* a locate back after a push: the step plays its push again (no stale pushed_k) */
    nev = 0;
    play(&s, 0.0, 3.9, 0);
    at = nev;
    ppq = play(&s, 2.0, 2.5, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ons_near(at, 2, 3.875) == 3 && ons_between(at, 2, 3.9, 4.5) == 0 && balanced(), "a locate back: the push again, once");
    /* past a played push, a locate back to just before the same step: the chord plays again, once (no stale pushed_k) */
    nev = 0;
    play(&s, 0.0, 5.5, 0);
    at = nev;
    ppq = play(&s, 3.8, 0.7, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ons_between(at, 2, 3.8, 4.5) == 3 && ons_near(at, 2, 3.875) == 3 && balanced(),
          "played past a push, located back before its step: the chord sounds once after the locate");
    /* a stop after a push, then a start just before the step: it plays on the step, not skipped */
    nev = 0;
    play(&s, 0.0, 3.9, 0);
    seq_block(&s, 3.9, bpm, 0, 128);
    at = nev;
    ppq = play(&s, 3.95, 0.3, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ons_between(at, 2, 3.95, 4.0 + blk() + 1e-9) == 3 && balanced(), "a stop and a start at 3.95, past the push time: the chord is pushed on the first block after the start, once");
    /* a queued part switch: no push into the bar line it switches at; the Chorus pushes its own changes */
    static seq_t c;
    chorus_setup(&c);
    seq_set_chord_shape(&c, SHAPE_PUSH, 100);
    seq_select_part(&c, 3);
    seq_set_chord_shape(&c, SHAPE_PUSH, 100);
    seq_select_part(&c, 0);
    nev = 0;
    ppq = play(&c, 0.0, 9.5, 0);
    seq_select_part(&c, 3);
    ppq = play(&c, ppq, 8.0, 0);
    seq_block(&c, ppq, bpm, 0, 128);
    ons_at(2, 48, &low, &part);
    CHECK(ons_near(0, 2, 7.875) == 3 && ons_between(0, 2, 11.8, 12.0) == 0 && ons_near(0, 2, 12.0) == 3 && part == 3 &&
          ons_near(0, 2, 15.875) == 3 && balanced(), "a queued switch: no push into its bar line; the Chorus's bar 2 pushed");
    /* queued after the push already played: the switch ends it and the Chorus's chord plays on the line */
    chorus_setup(&c);
    seq_set_chord_shape(&c, SHAPE_PUSH, 100);
    nev = 0;
    ppq = play(&c, 0.0, 11.9, 0);
    seq_select_part(&c, 3);
    ppq = play(&c, ppq, 1.0, 0);
    seq_block(&c, ppq, bpm, 0, 128);
    ons_at(2, 48, &low, &part);
    CHECK(ons_near(0, 2, 11.875) == 3 && ons_near(0, 2, 12.0) == 3 && part == 3 && balanced(),
          "queued after a push: the Chorus's chord still plays on the bar line");
}

/* Review Focus 5: STRUM 100 with 1/4-bar chords, and ALL NOTES OFF mid-strum */
static void test_09_strum(void) {
    static seq_t s;
    int got[8];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_SUSTAIN);
    seq_set_chord_len(&s, 4);
    seq_set_prog_preset(&s, 15);   /* Canon: 8 chords, 1/4 bar each */
    seq_set_chord_shape(&s, SHAPE_STRUM, 100);
    nev = 0;
    double ppq = play(&s, 0.0, 16.0, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    int up = 1;
    for (int c = 1; c < 16; c++) {
        int n0 = ons_near(0, 1, c), n1 = ons_near(0, 1, c + 0.0625), n2 = ons_near(0, 1, c + 0.125);
        up &= n0 == 1 && n1 == 1 && n2 == 1;
    }
    ons(1, got, 3);
    CHECK(up && got[0] < got[1] && got[1] < got[2], "STRUM 100: a chord's notes 1/16 beat apart, low to high");
    CHECK(balanced() && max_open(1) <= 6, "no hung notes (at most %d sounding)", max_open(1));
    nev = 0;
    ppq = play(&s, 0.0, 1.07, 0);   /* the chord at beat 1: two notes out, the third due at 1.125 */
    seq_all_notes_off(&s);
    int at = nev;
    ppq = play(&s, ppq, 0.5, 0);
    CHECK(ons_between(at, 1, 1.07, 1.5) == 0, "ALL NOTES OFF mid-strum: the queued note never plays");
    ppq = play(&s, ppq, 2.0, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced() && ons_near(0, 1, 2.0) == 1, "every on has its off; the next chord strums again");
    /* a 4-note chord on a swung 16th: its last note would come after the next step's chord, which supersedes it */
    static seq_t w;
    fresh(&w, 0);
    seq_set_swing(&w, 100);
    seq_set_slot(&w, 0, 0, 0, COL_7TH);
    seq_set_step(&w, LANE_CHORDS, 0, 0, 0);
    seq_set_step(&w, LANE_CHORDS, 0, 1, 1);
    seq_set_step(&w, LANE_CHORDS, 0, 2, 1);
    seq_set_chord_shape(&w, SHAPE_STRUM, 100);
    nev = 0;
    play_stop(&w, 0.9);
    CHECK(ons_between(0, 2, 0.0, 0.5) == 3 && ons_between(0, 2, 0.5, 0.9) == 4 && balanced(),
          "a chord's late strum notes are dropped when the next chord starts first");
    /* a stop and a locate clear it too */
    nev = 0;
    ppq = play(&s, 0.0, 1.07, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    at = nev;
    ppq = play(&s, 1.07, 0.3, 0);
    CHECK(ons_between(at, 1, 1.07, 1.37) == 0, "a stop mid-strum drops the rest");
    nev = 0;
    ppq = play(&s, 0.0, 1.07, 0);
    at = nev;
    ppq = play(&s, 3.5, 0.3, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ons_between(at, 1, 3.5, 3.8) == 0 && balanced(), "so does a locate");
}

/* a state text without its `|<P>.<name>=...` fields, for every part letter */
static void strip_field(char *buf, const char *name) {
    char pat[8];
    for (char c = 'A'; c <= 'F'; c++) {
        snprintf(pat, sizeof pat, "|%c.%s=", c, name);
        char *f = strstr(buf, pat);
        if (!f) continue;
        char *e = strchr(f + 1, '|');
        memmove(f, e ? e : f + strlen(f), strlen(e ? e : f + strlen(f)) + 1);
    }
}

static int neutral_09(const seq_part_t *p) {
    static const int N[SHAPE_COUNT] = {0, 50, 0, 50, 0, 50, 0};
    for (int i = 0; i < SEQ_MAX_PROG; i++)
        if (p->src[i] || p->colour[i]) return 0;
    return !p->slot_lock && !memcmp(p->lane_shape[LANE_CHORDS], N, sizeof N);
}

static void test_09_state(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX], bad[SEQ_STATE_MAX], plain[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, ".pq=") && !strstr(buf, ".sl=") && !strstr(buf, ".cs=") && neutral_09(&s.part[0]), "neutral: no 0.9 fields");
    seq_set_slot(&s, 1, 0, 1, COL_7TH);
    seq_set_slot(&s, 3, 4, 0, COL_SUS2);
    seq_set_chord_shape(&s, SHAPE_SPREAD, 70);
    seq_set_chord_shape(&s, SHAPE_OCT, -1);
    seq_select_part(&s, 3);
    seq_set_chord_shape(&s, SHAPE_STRUM, 40);
    seq_select_part(&s, 0);
    seq_state_save(&s, buf, sizeof buf);
    strcpy(plain, buf);
    strip_field(plain, "vx");   /* 0.17 writes its `.vx` between .voi and .pq */
    CHECK(strstr(plain, "|A.voi=1|A.pq=00,11,00,02|A.sl=a|A.cs=0,70,0,50,0,50,-1|ch=") &&
          strstr(plain, "|D.voi=1|D.pq=00,11,00,02|D.sl=a|D.cs=0,70,0,50,40,50,-1|D1="), "0.9 fields after .voi, for A and D");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_state_save(&t, again, sizeof again) && !strcmp(buf, again) &&
          same_part_at(&s, 0, &t, 0) && same_part_at(&s, 3, &t, 3) && !memcmp(t.part[0].src, s.part[0].src, sizeof s.part[0].src) &&
          !memcmp(t.part[3].colour, s.part[3].colour, sizeof s.part[3].colour) && t.part[0].slot_lock == 0xa &&
          !memcmp(t.part[3].lane_shape[LANE_CHORDS], s.part[3].lane_shape[LANE_CHORDS], sizeof s.part[3].lane_shape[0]),
          "a 0.9 state round-trips byte for byte");
    strip_field(buf, "pq");
    strip_field(buf, "sl");
    strip_field(buf, "cs");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && neutral_09(&t.part[0]) && neutral_09(&t.part[3]) && t.part[0].prog[1] == 3 &&
          voices_as_08(&t), "without them (a 0.8 build's view): diatonic triads, the borrowed slot on its degree");
    static const char *const BAD[][2] = {{"|A.pq=00,11,00,02", "|A.pq=00,41,00,02"}, {"|A.pq=00,11,00,02", "|A.pq=00,11,00"},
                                         {"|A.pq=00,11,00,02", "|A.pq=00,1a,00,02"}, {"|A.sl=a", "|A.sl=xz"},
                                         {"|A.cs=0,70,0,50,0,50,-1", "|A.cs=0,70"}, {"|A.cs=0,70,0,50,0,50,-1", "|A.cs=0,70,0,50,0,50,2"},
                                         {"|A.cs=0,70,0,50,0,50,-1", "|A.cs=0,70,0,50,0,101,0"}};
    int ok = 1;
    for (int i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++) {
        seq_state_save(&s, buf, sizeof buf);
        char *f = strstr(buf, BAD[i][0]);
        snprintf(bad, sizeof bad, "%.*s%s%s", (int)(f - buf), buf, BAD[i][1], f + strlen(BAD[i][0]));
        seq_init(&t, emit, NULL);
        int loaded = seq_state_load(&t, bad), pq = BAD[i][0][3] == 'p', sl = BAD[i][0][3] == 's', cs = BAD[i][0][3] == 'c';
        const seq_part_t *p = &t.part[0];
        ok &= loaded && seq_part_used(&t, 3) && t.part[3].lane_shape[LANE_CHORDS][SHAPE_STRUM] == 40;
        ok &= pq ? !p->src[1] && !p->colour[1] && !p->colour[3] : p->src[1] == 1 && p->colour[3] == COL_SUS2;
        ok &= sl ? !p->slot_lock : p->slot_lock == 0xa;
        const int *cs_row = p->lane_shape[LANE_CHORDS];
        ok &= cs ? cs_row[SHAPE_SPREAD] == 50 && !cs_row[SHAPE_OCT] : cs_row[SHAPE_SPREAD] == 70;
    }
    CHECK(ok, "a bad pq, sl or cs is ignored: neutral for that field, the rest of the part and the other parts load");
    /* the worst case still fits: 16 bars, every step on, 8 borrowed coloured locked slots, every SHAPE value off neutral */
    int deg[8] = {0};
    unsigned char src[8] = {3, 3, 3, 3, 3, 3, 3, 3}, col[8] = {4, 4, 4, 4, 4, 4, 4, 4};
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) seq_set_style(&s, l, GEN_MANUAL);
    for (int l = 0; l < SEQ_LANES; l++)
        for (int i = 0; i < 16 * SEQ_STEPS; i++) seq_set_step(&s, l, i / 16, i % 16, 1);
    seq_set_progression(&s, 8, deg, src, col, 0);
    for (int i = 0; i < 8; i++) seq_toggle_slot_lock(&s, i);
    for (int w = 0; w < SHAPE_COUNT; w++) seq_set_chord_shape(&s, w, w == SHAPE_OCT ? -1 : 100);
    int n = seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    CHECK(n > 0 && n < SEQ_STATE_MAX && seq_state_load(&t, buf) && t.part[0].slot_lock == 0xff && t.part[0].src[7] == 3,
          "the worst case with every 0.9 field saves in %d bytes (< %d) and loads", n, SEQ_STATE_MAX);
}

/* a literal state saved by the 0.8 build: parts A and C, hand-edit locks, g fields, a Manual melody */
static void test_09_literal_08_state(void) {
    static seq_t s;
    static const char S08[] =
        "csq2|gv=1|k=4|s=1|A.len=1|A.cl=16|A.h=30,1|A.prog=0526|A.voi=1|ch=1,2,3|A1=0:|A1.g=2,1,30|A2=0:0514640|A2.g=4,1,30"
        "|A2.lk=0200000000000000000000000000000000000000000000000000000000000000|A3=0:|A3.g=7,1,30|A4=0:|C.len=1|C.cl=8"
        "|C.h=30,1|C.prog=03|C.voi=1|C1=0:0915640|C1.g=3,1,30|C1.lk=0020000000000000000000000000000000000000000000000000000000000000"
        "|C2=0:0514640|C2.g=4,1,30|C2.lk=0200000000000000000000000000000000000000000000000000000000000000"
        "|C3=0:00166400236640035664008166400a36640|C4=0:";
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, S08) && seq_part_used(&s, 2) && neutral_09(&s.part[0]) && neutral_09(&s.part[2]),
          "a 0.8 state (A and C, locks, g) loads neutral");
    CHECK(saves_but_drawn(&s, S08), "and saves back byte for byte (0.18: but its Chords lanes, as DRAWN bars)");
}

static void test_09_slots_locks_undo(void) {
    static seq_t s;
    int deg[8] = {1, 2, 3, 4, 5, 6, 0, 1};
    unsigned char src[8] = {0, 2, 0, 0, 0, 0, 0, 0}, col[8] = {0, 0, 1, 0, 0, 0, 0, 0};
    seq_init(&s, emit, NULL);   /* 0526 */
    int n0 = s.undo_n;
    seq_set_slot(&s, 2, 4, 0, COL_7TH);
    CHECK(s.part[0].prog[2] == 4 && seq_slot_colour(&s, 2) == COL_7TH && seq_slot_locked(&s, 2) && s.undo_n == n0 + 1, "FIX: one undo step");
    seq_set_slot(&s, 4, 0, 0, 0);
    seq_set_slot(&s, 0, 7, 0, 0);
    seq_set_slot(&s, 0, 0, 4, 0);
    seq_set_slot(&s, 0, 0, 0, COL_COUNT);
    CHECK(s.undo_n == n0 + 1 && s.part[0].prog[0] == 0, "out of range: ignored");
    seq_undo(&s);
    CHECK(s.part[0].prog[2] == 2 && !seq_slot_colour(&s, 2) && !seq_slot_locked(&s, 2) && voices_as_08(&s), "undo: chord, colour and lock back");
    seq_toggle_slot_lock(&s, 0);
    CHECK(seq_slot_locked(&s, 0), "toggle locks");
    seq_toggle_slot_lock(&s, 0);
    CHECK(!seq_slot_locked(&s, 0) && seq_prog_len(&s) == 4, "and unlocks");
    seq_set_slot(&s, 1, 0, 2, COL_SUS4);   /* minor's Borrow 2: V (degree 4, Major) */
    int same = 1, moved = 0, was[SEQ_MAX_PROG];
    memcpy(was, s.part[0].prog, sizeof was);
    for (int i = 0; i < 8; i++) {
        seq_generate_progression(&s);
        same &= s.part[0].prog[1] == 4 && s.part[0].src[1] == 2 && s.part[0].colour[1] == COL_SUS4 && seq_slot_locked(&s, 1);
        moved += s.part[0].prog[0] != was[0] || s.part[0].prog[2] != was[2] || s.part[0].prog[3] != was[3];
    }
    CHECK(same && moved, "HARMONY GEN keeps the locked slot (the others change)");
    seq_set_prog_preset(&s, 1);
    CHECK(!s.part[0].slot_lock && !s.part[0].src[1] && !s.part[0].colour[1] && seq_prog_preset(&s) == 1, "a preset clears every slot");
    seq_toggle_slot_lock(&s, 2);
    int top = s.undo_top;
    seq_set_prog_preset(&s, 1);   /* the same preset: only the lock goes */
    CHECK(!s.part[0].slot_lock && s.undo_top == (top + 1) % SEQ_UNDO, "re-picking the preset clears a lock as an undo step");
    seq_undo(&s);
    CHECK(seq_slot_locked(&s, 2) && seq_prog_preset(&s) == 1, "and undo brings the lock back");
    seq_toggle_slot_lock(&s, 2);
    n0 = s.undo_top;
    seq_set_progression(&s, 12, deg, src, col, 1);
    n0 = (n0 + 1) % SEQ_UNDO;
    CHECK(s.part[0].phrase == 8 && seq_prog_len(&s) == 4 && s.undo_top == n0 && s.part[0].prog[1] == 4 && s.part[0].src[1] == 2 &&
          seq_slot_colour(&s, 2) == COL_7TH && seq_prog_preset(&s) < 0, "set_progression: n clamped to 8, a borrow on its degree, one undo");
    seq_set_progression(&s, 0, deg, src, col, 0);
    CHECK(s.part[0].phrase == 1 && seq_prog_len(&s) == 4 && s.undo_top == n0, "n 0 clamps to 1 (a phrase over the loop); undo 0 pushes nothing");
    seq_undo(&s);
    CHECK(seq_prog_len(&s) == 4 && seq_prog_preset(&s) == 1 && voices_as_08(&s), "undo goes back to the preset");
}

/* 0.10: the Bass lane's SHAPE (per-lane table). Neutral: 0.9's MIDI and save text; MOVEMENT, SYNC, OCTAVES and APPROACH
 * transform only generated steps (test_10_bass_movement and after) */
static const int BASS_NEUTRAL[BSHAPE_COUNT] = {0, 0, 0, 50, 0, 50, 50, 0, 0};

static void test_10_lane_shape_neutral(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX], ref[SEQ_STATE_MAX];
    int ok = 1;
    seq_init(&s, emit, NULL);
    for (int i = 0; i < BSHAPE_COUNT; i++) ok &= seq_lane_shape(&s, LANE_BASS, i) == BASS_NEUTRAL[i];
    for (int i = 0; i < SHAPE_COUNT; i++) ok &= seq_lane_shape(&s, LANE_CHORDS, i) == seq_chord_shape(&s, i);
    seq_state_save(&s, ref, sizeof ref);
    CHECK(ok && !strstr(ref, ".bs="), "a new instance: Bass shaping neutral (0,0,0,50,0,50,50,0,0), no .bs field");
    fresh(&s, 1);
    play_stop(&s, 16.0);
    unsigned h = fold_events(2166136261u);
    for (int i = 0; i < BSHAPE_COUNT; i++) seq_set_lane_shape(&s, LANE_BASS, i, i == BSHAPE_REGISTER ? 0 : 1);
    for (int i = 0; i < BSHAPE_COUNT; i++) seq_set_lane_shape(&s, LANE_BASS, i, BASS_NEUTRAL[i]);
    nev = 0;
    play_stop(&s, 16.0);
    CHECK(fold_events(2166136261u) == h, "set off neutral and back: the same MIDI");
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_MOVEMENT, 70);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_SYNC, 40);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCTAVES, 90);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1);
    nev = 0;
    play_stop(&s, 16.0);
    CHECK(fold_events(2166136261u) == h, "on a MANUAL Bass, MOVEMENT, SYNC, OCTAVES and APPROACH change nothing: the same MIDI");
    seq_init(&s, emit, NULL);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, 20);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, 50);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strcmp(buf, ref), "and saves the same text");
    /* clamping, bad lanes and indices */
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, 5);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 3);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER, 130);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_PUSH, -4);
    CHECK(seq_lane_shape(&s, LANE_BASS, BSHAPE_OCT) == 1 && seq_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH) == 1 &&
          seq_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER) == 100 && seq_lane_shape(&s, LANE_BASS, BSHAPE_PUSH) == 0,
          "Bass clamps: octave -1..1, approach 0..1, the rest 0..100");
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, -5);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_COUNT, 9);
    seq_set_lane_shape(&s, LANE_BASS, -1, 9);
    seq_set_lane_shape(&s, SEQ_ACTIVE_LANES, 0, 9);
    seq_set_lane_shape(&s, -1, 0, 9);
    seq_set_lane_shape(&s, LANE_CHORDS, SHAPE_COUNT, 9);
    seq_set_lane_shape(&s, LANE_MEL_UP, 0, 9);
    ok = seq_lane_shape(&s, LANE_BASS, BSHAPE_OCT) == -1 && seq_lane_shape(&s, LANE_BASS, BSHAPE_COUNT) == 0 &&
         seq_lane_shape(&s, LANE_BASS, -1) == 0 && seq_lane_shape(&s, SEQ_ACTIVE_LANES, 0) == 0 &&
         seq_lane_shape(&s, -1, 0) == 0 && seq_lane_shape(&s, LANE_CHORDS, SHAPE_COUNT) == 0 &&
         seq_lane_shape(&s, LANE_MEL_UP, 0) == 0;
    seq_set_lane_shape(&s, LANE_CHORDS, SHAPE_SPREAD, 70);
    seq_set_chord_shape(&s, SHAPE_STRUM, 30);
    CHECK(ok && seq_chord_shape(&s, SHAPE_SPREAD) == 70 && seq_lane_shape(&s, LANE_CHORDS, SHAPE_STRUM) == 30,
          "bad lanes and indices are ignored (read 0); Melody has no SHAPE yet; the 0.9 chord calls are lane 0's");
}

/* v1's Bass: steps 1 and 9 of each bar, the root at octave -1: E2 40, C2 36, G2 43, D2 38 */
static int bass_notes(int *got) { return ons(3, got, 8); }

/* the Bass's note-ons over 16 beats at REGISTER v and OCTAVE oct, from 0 then stopped; returns the count */
static int bass_at(seq_t *s, int v, int oct, int *got, int max) {
    seq_set_lane_shape(s, LANE_BASS, BSHAPE_REGISTER, v);
    seq_set_lane_shape(s, LANE_BASS, BSHAPE_OCT, oct);
    nev = 0;
    play_stop(s, 16.0);
    return ons(3, got, max);
}

/* REGISTER: below 50 a ceiling 60 - (50 - v) x 24/50, notes above it fold down; above 50 a floor 28 + (v - 50) x 24/50,
 * notes below it fold up; OCTAVE after it */
static void test_10_bass_register(void) {
    static seq_t s;
    int got[64], ref[64], prev[64];
    char lbl[8];
    fresh(&s, 1);
    play_stop(&s, 16.0);
    unsigned chords = chan_hash(2), mel = chan_hash(4);
    bass_at(&s, 0, 0, got, 8);   /* ceiling 36 */
    CHECK(got[0] == 28 && got[2] == 36 && got[4] == 31 && got[6] == 26,
          "REGISTER 0: E2 C2 G2 D2 folded down to at most 36 (%d %d %d %d)", got[0], got[2], got[4], got[6]);
    CHECK(chan_hash(2) == chords && chan_hash(4) == mel, "the Chords and Melody untouched");
    bass_at(&s, 50, 0, got, 8);
    CHECK(got[0] == 40 && got[2] == 36 && got[4] == 43 && got[6] == 38, "REGISTER 50: nothing folded");
    bass_at(&s, 25, 0, got, 8);   /* ceiling 48: all already under it */
    CHECK(got[0] == 40 && got[2] == 36 && got[4] == 43 && got[6] == 38, "REGISTER 25: nothing above 48");
    bass_at(&s, 100, 0, got, 8);   /* floor 52 */
    CHECK(got[0] == 52 && got[2] == 60 && got[4] == 55 && got[6] == 62,
          "REGISTER 100: folded up to at least 52 (%d %d %d %d)", got[0], got[2], got[4], got[6]);
    seq_cell_label(&s, LANE_BASS, 0, lbl, sizeof lbl);
    CHECK(!strcmp(lbl, "E3") && chan_hash(2) == chords, "the cell shows the note played (%s)", lbl);
    /* 49 and 51 barely move it: no octave jump either side of neutral */
    int n49 = bass_at(&s, 49, 0, got, 64), n50 = bass_at(&s, 50, 0, ref, 64), n51 = bass_at(&s, 51, 0, prev, 64);
    int same = n49 == n50 && n51 == n50;
    for (int i = 0; same && i < n50; i++) same &= got[i] == ref[i] && prev[i] == ref[i];
    CHECK(same, "REGISTER 49 and 51 play as 50 here (the window edge is at 59.5 and 28.5)");
    /* OCTAVE always moves the result an octave */
    int oct_ok = 1;
    for (int v = 0; v <= 100; v += 100) {
        int n = bass_at(&s, v, 0, ref, 64);
        oct_ok &= bass_at(&s, v, 1, got, 64) == n;
        for (int i = 0; i < n; i++) oct_ok &= got[i] == ref[i] + 12;
        oct_ok &= bass_at(&s, v, -1, got, 64) == n;
        for (int i = 0; i < n; i++) oct_ok &= got[i] == ref[i] - 12;
    }
    CHECK(oct_ok, "OCTAVE +1 is exactly +12 (and -1 exactly -12) at REGISTER 0 and at 100");
    /* monotonic: down from 50 no note rises, up from 50 none falls; v1's Bass and a generated Walk, every octave */
    int mono = 1;
    for (int g = 0; g < 2; g++) {
        if (g) seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
        for (int oct = -1; oct <= 1; oct++)
            for (int dir = -1; dir <= 1; dir += 2) {
                int n = bass_at(&s, 50, oct, prev, 64);
                for (int v = 50 + dir; v >= 0 && v <= 100; v += dir) {
                    mono &= bass_at(&s, v, oct, got, 64) == n;
                    for (int i = 0; i < n; i++) mono &= dir < 0 ? got[i] <= prev[i] : got[i] >= prev[i];
                    memcpy(prev, got, sizeof got);
                }
            }
    }
    CHECK(mono, "REGISTER is monotonic: 50 -> 0 never raises a note, 50 -> 100 never lowers one");
}

static void test_10_bass_gate_accent_octave(void) {
    static seq_t s;
    int got[8], flat = 1;
    fresh(&s, 1);
    play_stop(&s, 16.0);
    unsigned chords = chan_hash(2), mel = chan_hash(4);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT, 0);
    nev = 0;
    play_stop(&s, 16.0);
    for (int i = 0; i < nev; i++) flat &= !is_on(&evs[i]) || (evs[i].status & 15) != 2 || evs[i].vel == 64;
    CHECK(flat && chan_hash(2) == chords && chan_hash(4) == mel, "ACCENT 0: every Bass note at 64, the others untouched");
    int v75 = 1, v100 = 1;
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT, 75);
    nev = 0;
    play_stop(&s, 4.0);
    for (int i = 0; i < nev; i++) v75 &= !is_on(&evs[i]) || (evs[i].status & 15) != 2 || evs[i].vel == 118;
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT, 100);
    nev = 0;
    play_stop(&s, 4.0);
    for (int i = 0; i < nev; i++) v100 &= !is_on(&evs[i]) || (evs[i].status & 15) != 2 || evs[i].vel == 127;
    CHECK(v75 && v100, "ACCENT 75: 64 + 36 x 1.5 = 118; 100: clamped to 127");
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT, 50);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, 0);
    nev = 0;
    play_stop(&s, 16.0);
    unsigned bass = chan_hash(3);
    CHECK(chan_hash(2) == chords && chan_hash(4) == mel && balanced(), "GATE 0 leaves the Chords and Melody alone");
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, 50);
    seq_set_gate(&s, 0);
    nev = 0;
    play_stop(&s, 16.0);
    CHECK(chan_hash(3) == bass, "and shortens the Bass as PLAY's GATE 0 does");
    seq_set_gate(&s, 50);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, 100);
    seq_set_gate(&s, 100);
    nev = 0;
    play_stop(&s, 16.0);
    bass = chan_hash(3);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, 50);
    nev = 0;
    play_stop(&s, 16.0);
    CHECK(chan_hash(3) != bass && balanced(), "GATE 100 lengthens on top of PLAY's");
    seq_set_gate(&s, 50);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, -1);
    nev = 0;
    play_stop(&s, 4.0);
    CHECK(bass_notes(got) == 2 && got[0] == 28 && got[1] == 28, "OCTAVE -1: E1 (%d)", got[0]);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, 1);
    nev = 0;
    play_stop(&s, 16.0);
    CHECK(bass_notes(got) == 8 && got[0] == 52 && got[2] == 48 && chan_hash(2) == chords && chan_hash(4) == mel,
          "OCTAVE +1: E3 C3 (%d %d), the others untouched", got[0], got[2]);
}

/* consecutive note-ons on channel ch, by block position: two less than `gap` apart, not across a jump back, nor across a
 * part switch (a switch queued after a push ends the pushed note and plays the new part on its bar line, as in 0.9) */
static int close_strikes(int ch, double gap) {
    double last = -100.0;
    int n = 0, part = -1;
    for (int i = 0; i < nev; i++) {
        if (!is_on(&evs[i]) || (evs[i].status & 15) != ch - 1) continue;
        double d = evs[i].ppq - last;
        n += d >= 0 && d < gap && evs[i].part == part;
        last = evs[i].ppq;
        part = evs[i].part;
    }
    return n;
}

/* Review Focus 5 for 0.10: the Chords and the Bass pushed at one chord change; a loop wrap, a queued switch, a locate */
static void test_10_push_both_lanes(void) {
    static seq_t s, c;
    fresh(&s, 0);   /* Chords on step 1, Bass on steps 1 and 9 of each bar */
    seq_set_chord_shape(&s, SHAPE_PUSH, 100);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_PUSH, 100);
    nev = 0;
    play(&s, 0.0, 15.95, 0);
    seq_block(&s, 15.95, bpm, 0, 128);
    CHECK(count_on(2) == 15 && count_on(3) == 9 && ons_near(0, 3, 3.875) == 1 && ons_near(0, 3, 15.875) == 1 &&
          ons_near(0, 2, 3.875) == 3 && ons_between(0, 3, 3.9, 4.1) == 0,
          "PUSH 100 on both: the Bass's chord-change onsets 1/8 beat early with the chord, once");
    CHECK(ons_near(0, 3, 2.0) == 1 && ons_between(0, 3, 1.8, 1.99) == 0 && balanced() && !close_strikes(3, 1.0),
          "the Bass's onset inside a chord stays on the beat; no hung notes, no double strike");
    /* Bass alone at 50: the Chords on the beat */
    seq_set_chord_shape(&s, SHAPE_PUSH, 0);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_PUSH, 50);
    nev = 0;
    play_stop(&s, 4.1);
    CHECK(ons_near(0, 3, 3.9375) == 1 && ons_near(0, 2, 4.0) == 3 && ons_between(0, 2, 3.8, 3.99) == 0 && balanced(),
          "Bass PUSH 50 alone: the Bass 1/16 beat early, the Chords on the beat");
    /* a 4-bar host loop: both pushes are bar 1's and keep sounding across the wrap */
    seq_set_chord_shape(&s, SHAPE_PUSH, 100);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_PUSH, 100);
    nev = 0;
    double ppq = play(&s, 0.0, 15.95, 16.0);
    int at = nev, low;
    ppq = play(&s, ppq, 0.2, 16.0);
    low = low_between(0, 3, 15.8, 16.0);
    CHECK(ons_near(0, 3, 15.875) == 1 && low == 40 && ons_near(0, 2, 15.875) == 3 && ons_between(at, 3, 0.0, 0.2) == 0 &&
          ons_between(at, 2, 0.0, 0.2) == 0, "at a loop wrap both play bar 1's notes early (Bass %d), no strike at the wrap", low);
    ppq = play(&s, ppq, 8.0, 16.0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced() && !close_strikes(3, 1.0) && ons_near(at, 3, 3.875) == 1, "and over the wrap: no hung notes, no double strike");
    /* the Bass pushed, the Chords not: at the wrap the Bass is kept, the Chords strike on the line */
    seq_set_chord_shape(&s, SHAPE_PUSH, 0);
    nev = 0;
    ppq = play(&s, 0.0, 15.95, 16.0);
    at = nev;
    ppq = play(&s, ppq, 1.0, 16.0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ons_near(0, 3, 15.875) == 1 && ons_between(at, 3, 0.0, 0.2) == 0 && ons_between(at, 2, 0.0, 0.2) == 3 && balanced(),
          "only the Bass pushed: it is kept over the wrap, the Chords play on bar 1");
    seq_set_chord_shape(&s, SHAPE_PUSH, 100);
    /* a 3-bar host loop over the 4-bar part */
    nev = 0;
    ppq = play(&s, 0.0, 36.0, 12.0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced() && max_open(3) <= 1, "a host loop that is not a multiple of the part: no hung notes");
    /* a locate back after both pushed: both again, once */
    nev = 0;
    play(&s, 0.0, 3.9, 0);
    at = nev;
    ppq = play(&s, 2.0, 2.5, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ons_near(at, 3, 3.875) == 1 && ons_between(at, 3, 3.9, 4.5) == 0 && ons_near(at, 2, 3.875) == 3 && balanced(),
          "a locate back: both pushes again, once");
    nev = 0;
    play(&s, 0.0, 5.5, 0);
    at = nev;
    ppq = play(&s, 3.8, 0.7, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ons_between(at, 3, 3.8, 4.5) == 1 && ons_between(at, 2, 3.8, 4.5) == 3 && balanced(),
          "played past both pushes, located back before the step: each sounds once");
    /* a queued switch: no push into its bar line on either lane; the Chorus pushes its own */
    chorus_setup(&c);
    seq_set_chord_shape(&c, SHAPE_PUSH, 100);
    seq_set_lane_shape(&c, LANE_BASS, BSHAPE_PUSH, 100);
    seq_select_part(&c, 3);
    seq_set_chord_shape(&c, SHAPE_PUSH, 100);
    seq_set_lane_shape(&c, LANE_BASS, BSHAPE_PUSH, 100);
    seq_select_part(&c, 0);
    nev = 0;
    ppq = play(&c, 0.0, 9.5, 0);
    seq_select_part(&c, 3);
    ppq = play(&c, ppq, 8.0, 0);
    seq_block(&c, ppq, bpm, 0, 128);
    int part;
    ons_at(3, 48, &low, &part);
    CHECK(ons_near(0, 3, 7.875) == 1 && ons_between(0, 3, 11.8, 12.0) == 0 && ons_between(0, 2, 11.8, 12.0) == 0 &&
          ons_near(0, 3, 12.0) == 1 && part == 3 && ons_near(0, 3, 15.875) == 1 && balanced() && !close_strikes(3, 1.0),
          "a queued switch: neither lane pushed into its bar line; the Chorus's bar 2 pushed on both");
    /* queued after both pushed: the switch ends them, the Chorus plays on the line */
    chorus_setup(&c);
    seq_set_chord_shape(&c, SHAPE_PUSH, 100);
    seq_set_lane_shape(&c, LANE_BASS, BSHAPE_PUSH, 100);
    nev = 0;
    ppq = play(&c, 0.0, 11.9, 0);
    seq_select_part(&c, 3);
    ppq = play(&c, ppq, 1.0, 0);
    seq_block(&c, ppq, bpm, 0, 128);
    ons_at(3, 48, &low, &part);
    CHECK(ons_near(0, 3, 11.875) == 1 && ons_near(0, 3, 12.0) == 1 && part == 3 && balanced(),
          "queued after both pushed: the Chorus's Bass still plays on the bar line");
}

/* fuzz: random PUSH and GATE per lane, SWING, chord lengths, host loops and queued switches; never a hung note or a double strike */
static void test_10_push_fuzz(void) {
    static seq_t s;
    static const double LOOPS[4] = {0.0, 16.0, 12.0, 8.0};
    unsigned r = 77;
    int ok = 1, strikes = 0;
    for (int it = 0; it < 60; it++) {
        chorus_setup(&s);
        for (int p = 0; p < 2; p++) {
            if (p) seq_select_part(&s, 3);
            seq_set_chord_shape(&s, SHAPE_PUSH, (int)rnd(&r, 101));
            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_PUSH, (int)rnd(&r, 101));
            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, (int)rnd(&r, 101));
            if (rnd(&r, 3) == 0) seq_set_chord_len(&s, 4 << rnd(&r, 3));
            if (p) seq_select_part(&s, 0);
        }
        seq_set_swing(&s, (int)rnd(&r, 101));
        if (rnd(&r, 2)) seq_set_channel(&s, LANE_BASS, 2);   /* the Bass on the Chords' channel */
        double loop = LOOPS[rnd(&r, 4)];
        nev = 0;
        double ppq = play(&s, 0.0, 3.0 + rnd(&r, 12), loop);
        if (rnd(&r, 2)) seq_select_part(&s, 3);
        ppq = play(&s, ppq, 3.0 + rnd(&r, 12), loop);
        if (rnd(&r, 2)) seq_select_part(&s, 0);
        ppq = play(&s, ppq, 20.0, loop);
        seq_block(&s, ppq, bpm, 0, 128);
        ok &= balanced() && s.npending == 0 && s.nstrum == 0;
        if (s.chan[LANE_BASS] != 2) strikes += close_strikes(3, 1.0);   /* v1's Bass: onsets 2 beats apart */
    }
    CHECK(ok && !strikes, "fuzz: 60 runs of PUSH on both lanes over wraps and switches, no hung notes, no double strike (%d)", strikes);
}

/* `<P>2.bs=`: the Bass's nine values in BSHAPE order, when not neutral; a bad one loads neutral, the rest of the part loads */
static void test_10_bs_state(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX], bad[SEQ_STATE_MAX];
    static const int V[BSHAPE_COUNT] = {10, 20, 30, 40, 55, 60, 70, -1, 1};
    seq_init(&s, emit, NULL);
    for (int i = 0; i < BSHAPE_COUNT; i++) seq_set_lane_shape(&s, LANE_BASS, i, V[i]);
    seq_set_chord_shape(&s, SHAPE_SPREAD, 70);
    seq_select_part(&s, 3);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER, 0);
    seq_select_part(&s, 0);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A.cs=0,70,0,50,0,50,0|ch=") && strstr(buf, "|A2.bs=10,20,30,40,55,60,70,-1,1|A3=") &&
          strstr(buf, "|D2.bs=10,20,30,0,55,60,70,-1,1|D3="), ".bs after the Bass's fields, for A and D; .cs as 0.9 wrote it");
    seq_init(&t, emit, NULL);
    int ok = seq_state_load(&t, buf) && seq_state_save(&t, again, sizeof again) && !strcmp(buf, again);
    ok &= !memcmp(t.part[0].lane_shape, s.part[0].lane_shape, sizeof s.part[0].lane_shape) &&
          !memcmp(t.part[3].lane_shape, s.part[3].lane_shape, sizeof s.part[3].lane_shape);
    CHECK(ok, "round-trips byte for byte, every value back");
    static const char *const BAD[] = {"|A2.bs=10,20", "|A2.bs=10,20,30,40,55,60,70,-1,2", "|A2.bs=10,20,30,40,55,60,70,-2,1",
                                      "|A2.bs=10,20,30,40,101,60,70,-1,1", "|A2.bs=10,20,30,40,55,60,70,-1,1,", "|A2.bs=10,20,30,40,55,60,70,-1,1,0",
                                      "|A2.bs=", "|A2.bs=a,20,30,40,55,60,70,-1,1"};
    const char *good = "|A2.bs=10,20,30,40,55,60,70,-1,1";
    ok = 1;
    for (int i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++) {
        char *f = strstr(buf, good);
        snprintf(bad, sizeof bad, "%.*s%s%s", (int)(f - buf), buf, BAD[i], f + strlen(good));
        seq_init(&t, emit, NULL);
        int loaded = seq_state_load(&t, bad);
        for (int k = 0; k < BSHAPE_COUNT; k++) ok &= t.part[0].lane_shape[LANE_BASS][k] == BASS_NEUTRAL[k];
        ok &= loaded && t.part[0].lane_shape[LANE_CHORDS][SHAPE_SPREAD] == 70 && seq_part_used(&t, 3) &&
              t.part[3].lane_shape[LANE_BASS][BSHAPE_PUSH] == 55 && t.part[0].lane[LANE_BASS].gen.style == s.part[0].lane[LANE_BASS].gen.style;
    }
    CHECK(ok, "a bad .bs is ignored: the Bass neutral, the rest of the part and the other parts load");
    /* back to neutral: no field */
    for (int i = 0; i < BSHAPE_COUNT; i++) seq_set_lane_shape(&s, LANE_BASS, i, BASS_NEUTRAL[i]);
    seq_select_part(&s, 3);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER, 50);
    for (int i = 0; i < BSHAPE_COUNT; i++) seq_set_lane_shape(&s, LANE_BASS, i, BASS_NEUTRAL[i]);
    seq_select_part(&s, 0);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, ".bs="), "neutral again: no .bs field");
}

/* 0.10 Task 2: the Bass's step transforms (MOVEMENT, SYNC, OCTAVES), the walk's chromatic approach, absolute-pitch steps
 * and the FIX setters */

static seq_step_t *bass_steps(seq_t *s) { return s->part[s->cur_part].lane[LANE_BASS].steps; }

static int bass_locked(const seq_t *s, int i) { return s->part[s->cur_part].lane[LANE_BASS].lock[i / 8] >> (i % 8) & 1; }

/* A new instance with a generated Bass of style, seed and complexity over 4 bars of cl-step chords; no undo */
static void bass_lane(seq_t *s, int style, int seed, int c, int cl) {
    seq_init(s, emit, NULL);
    seq_set_chord_len(s, cl);
    seq_set_style(s, LANE_BASS, style);
    s->part[0].lane[LANE_BASS].gen.seed = seed;
    s->part[0].lane[LANE_BASS].gen.complexity = -1;   /* so the setter regenerates */
    seq_set_complexity(s, LANE_BASS, c);
    s->undo_n = 0;
}

/* A chord-relative step's pitch in its own chord (diatonic triads: a new instance's progression) */
static int bass_pitch(const seq_t *s, int i, const seq_step_t *x) {
    const seq_part_t *p = &s->part[s->cur_part];
    return theory_tone_note(s->key, s->scale, p->prog[(i / p->chord_len) % p->prog_len], x->tone, x->oct);
}

static int next_on_wrap(const seq_step_t *st, int n, int i) {
    for (int k = 1; k <= n; k++)
        if (st[(i + k) % n].on) return (i + k) % n;
    return i;
}

static int prev_on_wrap(const seq_step_t *st, int n, int i) {
    for (int k = 1; k <= n; k++)
        if (st[(i - k + n) % n].on) return (i - k + n) % n;
    return i;
}

static int same_step(const seq_step_t *a, const seq_step_t *b) { return !memcmp(a, b, sizeof *a); }

static int dist(int a, int b) { return a > b ? a - b : b - a; }

/* MOVEMENT: a ranked onset that is not a slot's first becomes tone 2..5, nearest the next onset's pitch, at its octave */
static void test_10_bass_movement(void) {
    static seq_t s;
    static seq_step_t base[SEQ_LANE_STEPS], prev[SEQ_LANE_STEPS];
    int id = 1, nest = 1, first = 1, tone = 1, all_root = 1, at50 = 0, at100 = 0;
    for (int run = 0; run < 9; run++) {
        int cl = run % 2 ? 8 : 16, n = 4 * SEQ_STEPS;
        bass_lane(&s, GEN_BASS_ROOT + run % 3, 100 + run * 37, 60 + run * 4, cl);
        seq_step_t *st = bass_steps(&s);
        memcpy(base, st, sizeof base);
        memcpy(prev, st, sizeof prev);
        for (int v = 0; v <= 100; v += 10) {
            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_MOVEMENT, v);
            for (int i = 0; i < n; i++) {
                int changed = !same_step(&st[i], &base[i]);
                if (!same_step(&prev[i], &base[i])) nest &= same_step(&st[i], &prev[i]);
                if (v == 0) id &= !changed;
                if (v == 50) at50 += changed;
                if (v == 100) at100 += changed;
                if (v == 100 && run % 3 == 0 && base[i].on && i % cl) all_root &= changed;   /* Root: tone 1 is never 2..5 */
                if (!changed) continue;
                first &= i % cl != 0;
                int j = next_on_wrap(base, n, i), target = bass_pitch(&s, j, &base[j]), best = 99;
                for (int t = TONE_2; t <= TONE_5; t++) {
                    seq_step_t x = base[i];
                    x.tone = (unsigned char)t;
                    if (dist(bass_pitch(&s, i, &x), target) < best) best = dist(bass_pitch(&s, i, &x), target);
                }
                tone &= st[i].on && st[i].tone >= TONE_2 && st[i].tone <= TONE_5 && st[i].oct == base[i].oct &&
                        st[i].vel == base[i].vel && st[i].len == base[i].len && !st[i].abs &&
                        dist(bass_pitch(&s, i, &st[i]), target) == best;
            }
            memcpy(prev, st, sizeof prev);
        }
    }
    CHECK(id, "MOVEMENT 0: the generated steps exactly");
    CHECK(first && tone, "a moved onset: never a slot's first, tone 2..5 nearest the next onset's pitch, same octave");
    CHECK(nest, "nesting: an onset changed at v is changed the same at every higher v");
    CHECK(at50 > 0 && at100 > at50 && all_root, "50 moves some (%d), 100 more (%d): every Root onset off a chord change", at50, at100);
}

/* SYNC: off-beat 8ths first (ranks 0..49), then beats 2 and 4 (50..99), one 16th later into a free step of the same slot */
static void test_10_bass_sync(void) {
    static seq_t s;
    static seq_step_t base[SEQ_LANE_STEPS], prev[SEQ_LANE_STEPS];
    int id = 1, exact50 = 1, exact100 = 1, nest = 1, moved = 1, some = 0, part = 0;
    for (int run = 0; run < 12; run++) {
        int cl = (int[]){16, 8, 4}[run % 3], n = 4 * SEQ_STEPS;
        bass_lane(&s, GEN_BASS_ROOT + run % 3, 200 + run * 53, 50 + run * 4, cl);
        seq_step_t *st = bass_steps(&s);
        memcpy(base, st, sizeof base);
        memcpy(prev, st, sizeof prev);
        for (int v = 0; v <= 100; v += 5) {
            int cnt = 0;
            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_SYNC, v);
            for (int i = 0; i < n; i++) {
                int pos = i % SEQ_STEPS, band = pos == 2 || pos == 6 || pos == 10 || pos == 14 ? 0 : pos == 4 || pos == 12 ? 1 : -1;
                int cand = band >= 0 && base[i].on && i % cl && (i + 1) % cl && !base[i + 1].on;
                int mv = cand && !st[i].on;
                int was = cand && !prev[i].on;
                if (v == 0) id &= same_step(&st[i], &base[i]);
                if (v == 50) exact50 &= mv == (cand && band == 0);
                if (v == 100) exact100 &= mv == cand;
                if (was) nest &= mv;
                cnt += mv;
                if (mv) {
                    seq_step_t x = base[i];
                    x.len = x.len > 1 ? x.len - 1 : 1;
                    moved &= same_step(&st[i + 1], &x);
                } else if (!(i > 0 && !st[i - 1].on && st[i].on && !base[i].on)) {
                    moved &= same_step(&st[i], &base[i]);   /* every step neither moved nor a target is untouched */
                }
            }
            if (v == 100) some += cnt;
            if (v == 25 && cnt) part++;
            memcpy(prev, st, sizeof prev);
        }
    }
    CHECK(id, "SYNC 0: the generated steps exactly");
    CHECK(exact50 && exact100, "SYNC 50 moves every free off-beat 8th, 100 beats 2 and 4 too; never a slot's first or across a slot");
    CHECK(moved && some > 0, "a moved onset is the same note one 16th later, a step shorter (%d moved at 100)", some);
    CHECK(nest && part > 0, "nesting: moved at v, moved at every higher v; 25 moves some");
    /* a move onto a step the user locked (a rest) is lost: the locked step wins */
    int src = -1;
    for (int run = 0; src < 0 && run < 12; run++) {
        bass_lane(&s, GEN_BASS_PUMP, 200 + run * 53, 70, 16);
        memcpy(base, bass_steps(&s), sizeof base);
        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_SYNC, 100);
        for (int i = 0; src < 0 && i < 63; i++)
            if (base[i].on && !bass_steps(&s)[i].on) src = i;
    }
    seq_set_step_note(&s, LANE_BASS, (src + 1) / 16, (src + 1) % 16, 0);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_SYNC, 99);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_SYNC, 100);
    CHECK(src >= 0 && !bass_steps(&s)[src].on && !bass_steps(&s)[src + 1].on, "a sync onto a locked rest is lost (step %d)", src);
}

/* OCTAVES: a ranked onset that is not a slot's first goes up an octave, or down when above the lane's octave (-1) */
static void test_10_bass_octaves(void) {
    static seq_t s;
    static seq_step_t base[SEQ_LANE_STEPS], prev[SEQ_LANE_STEPS];
    int id = 1, all = 1, nest = 1, at50 = 0, at100 = 0;
    for (int run = 0; run < 9; run++) {
        int cl = run % 2 ? 8 : 16, n = 4 * SEQ_STEPS;
        bass_lane(&s, GEN_BASS_ROOT + run % 3, 300 + run * 11, 70, cl);
        seq_step_t *st = bass_steps(&s);
        memcpy(base, st, sizeof base);
        memcpy(prev, st, sizeof prev);
        for (int v = 0; v <= 100; v += 10) {
            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCTAVES, v);
            for (int i = 0; i < n; i++) {
                seq_step_t x = base[i];
                x.oct = (signed char)(x.oct > -1 ? x.oct - 1 : x.oct + 1);
                int changed = !same_step(&st[i], &base[i]);
                if (!same_step(&prev[i], &base[i])) nest &= same_step(&st[i], &prev[i]);
                if (v == 0) id &= !changed;
                if (changed) all &= base[i].on && i % cl && same_step(&st[i], &x);
                if (v == 100 && base[i].on && i % cl) all &= changed;
                at50 += v == 50 && changed;
                at100 += v == 100 && changed;
            }
            memcpy(prev, st, sizeof prev);
        }
    }
    CHECK(id, "OCTAVES 0: the generated steps exactly");
    CHECK(all, "OCTAVES: only onsets off a chord change, +1 octave (-1 above the lane's); at 100 every one");
    CHECK(nest && at50 > 0 && at100 > at50, "nesting; 50 changes some (%d), 100 more (%d)", at50, at100);
}

/* MOVEMENT, then SYNC, then OCTAVES: all three at 100 on a Root lane */
static void test_10_bass_transform_order(void) {
    static seq_t s;
    static seq_step_t ms[SEQ_LANE_STEPS];
    bass_lane(&s, GEN_BASS_ROOT, 909, 80, 16);
    seq_step_t *st = bass_steps(&s);
    int ons0 = 0, ons1 = 0, ok = 1;
    for (int i = 0; i < 64; i++) ons0 += st[i].on;
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_MOVEMENT, 100);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_SYNC, 100);
    memcpy(ms, st, sizeof ms);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCTAVES, 100);
    for (int i = 0; i < 64; i++) {
        ons1 += st[i].on;
        if (!st[i].on || i % 16 == 0) continue;
        ok &= st[i].tone >= TONE_2 && st[i].tone <= TONE_5 && ms[i].on && st[i].oct == (ms[i].oct > -1 ? ms[i].oct - 1 : ms[i].oct + 1);
    }
    CHECK(ons0 == ons1 && ok, "a synced note keeps its passing tone and OCTAVES ranks it where it landed; no onset lost");
}

/* The nearest note of pitch class pc to P (ties: the lower) */
static int near_pc(int pc, int P) {
    int lo = P - ((P - pc) % 12 + 12) % 12;
    return P - lo <= lo + 12 - P ? lo : lo + 12;
}

/* APPROACH 1 (Chromatic), Walk only: a slot's last onset that is not a chord tone becomes the semitone below or above the
 * next slot's root, whichever is nearer the onset before it, as an absolute pitch */
static void test_10_walk_chromatic(void) {
    static seq_t s;
    static seq_step_t base[SEQ_LANE_STEPS];
    char lbl[8], want[8];
    int exact = 1, rest = 1, made = 0, label = 1, other = 1;
    for (int run = 0; run < 16; run++) {
        int cl = run % 2 ? 8 : 16, n = 64;
        bass_lane(&s, GEN_BASS_WALK, 400 + run * 29, 40 + run * 4, cl);
        seq_step_t *st = bass_steps(&s);
        memcpy(base, st, sizeof base);
        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1);
        const seq_part_t *p = &s.part[0];
        for (int a = 0; a < n; a += cl) {
            int last = -1;
            for (int i = a; i < a + cl; i++)
                if (base[i].on) last = i;
            for (int i = a; i < a + cl; i++)
                if (i != last) rest &= same_step(&st[i], &base[i]);
            if (last < 0) continue;
            int t = base[last].tone;
            if (t == TONE_R || t == TONE_3 || t == TONE_5) {
                exact &= same_step(&st[last], &base[last]);
                continue;
            }
            int j = prev_on_wrap(base, n, last), P = bass_pitch(&s, j, &base[j]);
            int root = theory_degree_pc(s.key, s.scale, p->prog[((a + cl) % n / cl) % p->prog_len]);
            int lo = near_pc((root + 11) % 12, P), hi = near_pc((root + 1) % 12, P), e = dist(lo, P) <= dist(hi, P) ? lo : hi;
            exact &= st[last].on && st[last].abs == e && st[last].vel == base[last].vel && st[last].len == base[last].len;
            made++;
            if (last < SEQ_STEPS) {
                seq_cell_label(&s, LANE_BASS, last, lbl, sizeof lbl);
                theory_note_name(e, want, sizeof want);
                label &= !strcmp(lbl, want);
            }
        }
        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 0);
        other &= !memcmp(st, base, sizeof base);
    }
    CHECK(made > 10 && exact, "Chromatic: %d approach notes, each the semitone by the next root nearer the onset before", made);
    CHECK(rest && label && other, "every other step untouched; the cell names the absolute note; Diatonic again: the walk as before");
    bass_lane(&s, GEN_BASS_PUMP, 5, 70, 16);
    memcpy(base, bass_steps(&s), sizeof base);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1);
    CHECK(!memcmp(bass_steps(&s), base, sizeof base), "APPROACH acts on a Walk only (Pump unchanged)");
}

/* An absolute step plays its note: REGISTER never folds it, OCTAVE moves it */
static void test_10_abs_playback(void) {
    static seq_t s;
    int got[64];
    char lbl[8];
    bass_lane(&s, GEN_BASS_ROOT, 1, 0, 16);
    seq_set_step_note(&s, LANE_BASS, 0, 4, 73);   /* C#5: far above any REGISTER window */
    nev = 0;
    play_stop(&s, 4.0);
    int n = ons(2, got, 64), hit = 0;
    for (int i = 0; i < n; i++) hit += got[i] == 73;
    seq_cell_label(&s, LANE_BASS, 4, lbl, sizeof lbl);
    CHECK(hit == 1 && !strcmp(lbl, "C#5"), "the step plays 73 and shows C#5 (%s)", lbl);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER, 0);
    nev = 0;
    play_stop(&s, 4.0);
    n = ons(2, got, 64), hit = 0;
    for (int i = 0; i < n; i++) hit += got[i] == 73;
    CHECK(hit == 1, "REGISTER 0 does not fold a locked pick (73)");
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, 1);
    nev = 0;
    play_stop(&s, 4.0);
    n = ons(2, got, 64), hit = 0;
    for (int i = 0; i < n; i++) hit += got[i] == 85;
    seq_cell_label(&s, LANE_BASS, 4, lbl, sizeof lbl);
    CHECK(hit == 1 && !strcmp(lbl, "C#6"), "OCTAVE +1 moves it an octave (%s)", lbl);
}

/* A generated approach note is folded by REGISTER (then OCTAVE) as the onsets around it; the cell names the folded note */
static void test_10_approach_register(void) {
    static seq_t s;
    int got[256], ok = 1, made = 0, folded[2] = {0, 0};
    char lbl[8], want[8];
    for (int v = 0; v <= 100; v += 100)
        for (int oct = -1; oct <= 1; oct++) {
            bass_lane(&s, GEN_BASS_WALK, 401, 70, 8);
            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1);
            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER, v);
            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, oct);
            nev = 0;
            play_stop(&s, 16.0);
            int n = ons(2, got, 256), k = 0;
            const seq_step_t *st = bass_steps(&s);
            for (int i = 0; i < 64; i++) {
                if (!st[i].on) continue;
                if (st[i].abs) {
                    /* fold_register as seq_core's: below 50 down under 60 - (50 - v) x 24/50, above 50 up over 28 + .. */
                    int e = st[i].abs;
                    if (v < 50) while (e > 60.0 - (50 - v) * 24 / 50.0) e -= 12;
                    if (v > 50) while (e < 28.0 + (v - 50) * 24 / 50.0) e += 12;
                    e += 12 * oct;
                    ok &= k < n && got[k] == e;
                    folded[v > 50] += e != st[i].abs + 12 * oct;
                    if (i < 16) {
                        seq_cell_label(&s, LANE_BASS, i, lbl, sizeof lbl);
                        theory_note_name(e, want, sizeof want);
                        ok &= !strcmp(lbl, want);
                    }
                    made++;
                }
                k++;
            }
        }
    CHECK(made > 0 && ok && folded[0] && folded[1],
          "REGISTER 0 and 100, OCTAVE -1..1: each approach note plays folded, then moved (%d notes, %d and %d folded)", made,
          folded[0], folded[1]);
}

/* Review Focus 2: FIX-locked steps (absolute, a rest, chord-relative) survive every transform, style, complexity, seed,
 * key, scale and chord-length change */
static void test_10_locks_survive(void) {
    static seq_t s;
    static const int at[3] = {3, 21, 32};
    seq_step_t keep[3];
    int ok = 1, ops = 0;
    bass_lane(&s, GEN_BASS_WALK, 77, 60, 16);
    seq_set_step_note(&s, LANE_BASS, 0, 3, 61);
    seq_set_step_note(&s, LANE_BASS, 1, 5, 0);
    seq_toggle_step_lock(&s, LANE_BASS, 2, 0);
    seq_step_t *st = bass_steps(&s);
    for (int k = 0; k < 3; k++) keep[k] = st[at[k]];
    ok &= keep[0].on && keep[0].abs == 61 && !keep[1].on && keep[2].on && !keep[2].abs;
#define SURVIVES(op) do { op; ops++; st = bass_steps(&s); for (int k = 0; k < 3; k++) ok &= same_step(&st[at[k]], &keep[k]) && bass_locked(&s, at[k]); } while (0)
    for (int w = BSHAPE_MOVEMENT; w <= BSHAPE_OCTAVES; w++)
        for (int v = 0; v <= 100; v += 50) SURVIVES(seq_set_lane_shape(&s, LANE_BASS, w, v));
    SURVIVES(seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1));
    for (int st2 = GEN_BASS_ROOT; st2 <= GEN_BASS_WALK; st2++) SURVIVES(seq_set_style(&s, LANE_BASS, st2));
    for (int c = 0; c <= 100; c += 50) SURVIVES(seq_set_complexity(&s, LANE_BASS, c));
    SURVIVES(seq_generate(&s, LANE_BASS));
    SURVIVES(seq_generate_all(&s));
    SURVIVES(seq_set_key(&s, 9));
    SURVIVES(seq_set_scale(&s, SCALE_DORIAN));
    SURVIVES(seq_set_prog_preset(&s, 3));
    SURVIVES(seq_set_chord_len(&s, 8));
    SURVIVES(seq_undo(&s));
#undef SURVIVES
    CHECK(ok, "an absolute step, a rest and a chord-relative lock survive %d changes", ops);
}

/* seq_set_step_note / seq_toggle_step_lock: one undo step each; a re-send is no edit */
static void test_10_fix_setters(void) {
    static seq_t s;
    static seq_step_t base[SEQ_LANE_STEPS];
    static seq_lane_t was;
    bass_lane(&s, GEN_BASS_PUMP, 33, 60, 16);
    seq_step_t *st = bass_steps(&s);
    memcpy(base, st, sizeof base);
    int on = 0, off = 1;
    while (on < 16 && !base[on].on) on++;
    while (off < 16 && base[off].on) off++;
    memcpy(&was, &s.part[0].lane[LANE_BASS], sizeof was);
    seq_set_step_note(&s, LANE_BASS, 0, on, 50);
    CHECK(st[on].on && st[on].abs == 50 && bass_locked(&s, on) && st[on].vel == base[on].vel && st[on].len == base[on].len &&
          s.undo_n == 1, "a note on a generated onset: on, locked, absolute 50, the generator's velocity and length, one undo");
    seq_set_step_note(&s, LANE_BASS, 0, on, 50);
    CHECK(s.undo_n == 1, "the same note again pushes nothing");
    seq_set_step_note(&s, LANE_BASS, 0, off, 47);
    CHECK(st[off].on && st[off].abs == 47 && st[off].vel == 100 && st[off].len == 1 && s.undo_n == 2,
          "a note on a generated rest: velocity 100, length 1");
    seq_set_step_note(&s, LANE_BASS, 1, 0, 0);
    CHECK(!st[16].on && !st[16].abs && bass_locked(&s, 16) && s.undo_n == 3, "note 0: the step off and locked (a rest)");
    seq_set_step_note(&s, LANE_BASS, 1, 0, 0);
    CHECK(s.undo_n == 3, "a rest again pushes nothing");
    seq_toggle_step_lock(&s, LANE_BASS, 0, off);
    CHECK(!bass_locked(&s, off) && same_step(&st[off], &base[off]) && s.undo_n == 4, "unlock: regenerated, absolute cleared");
    seq_toggle_step_lock(&s, LANE_BASS, 2, 0);
    CHECK(bass_locked(&s, 32) && same_step(&st[32], &base[32]) && s.undo_n == 5, "lock: the step kept, one undo");
    for (int k = 0; k < 5; k++) seq_undo(&s);
    CHECK(!memcmp(&s.part[0].lane[LANE_BASS], &was, sizeof was) && !s.undo_n, "five undos: the lane exactly as it was");
    /* bad calls */
    seq_set_step_note(&s, LANE_CHORDS, 0, 0, 60);
    seq_set_step_note(&s, LANE_BASS, 0, 16, 60);
    seq_set_step_note(&s, LANE_BASS, 4, 0, 60);
    seq_set_step_note(&s, LANE_BASS, 0, 0, 128);
    seq_set_step_note(&s, LANE_BASS, 0, 0, -1);
    seq_toggle_step_lock(&s, LANE_BASS, -1, 0);
    CHECK(!memcmp(&s.part[0].lane[LANE_BASS], &was, sizeof was) && !s.undo_n, "bad lane, step, bar or note: ignored");
    /* a Melody lane may take notes too (0.11); a MANUAL lane has nothing to lock */
    old_mel(&s);   /* 0.20: a JAM lane's fixes are its own */
    seq_set_step_note(&s, LANE_MEL_UP, 0, 1, 76);
    CHECK(s.part[0].lane[LANE_MEL_UP].steps[1].abs == 76 && s.undo_n == 1, "Melody 1 takes a note");
    seq_set_style(&s, LANE_BASS, GEN_MANUAL);
    int n0 = s.undo_n;
    seq_set_step_note(&s, LANE_BASS, 0, 7, 45);
    int nolock = 1;
    for (int i = 0; i < (int)sizeof s.part[0].lane[1].lock; i++) nolock &= !s.part[0].lane[LANE_BASS].lock[i];
    CHECK(st[7].on && st[7].abs == 45 && nolock && s.undo_n == n0 + 1, "a MANUAL Bass: set directly, no lock, one undo");
    seq_toggle_step_lock(&s, LANE_BASS, 0, 7);
    CHECK(s.undo_n == n0 + 1 && !bass_locked(&s, 7), "a MANUAL lane's lock toggle does nothing");
    seq_set_step(&s, LANE_BASS, 0, 7, 0);
    seq_set_step(&s, LANE_BASS, 0, 7, 1);
    CHECK(st[7].on && !st[7].abs, "a tap off and on: chord-relative again");
}

/* `str` with field `name` (e.g. "|A2.ab=") set to val, or removed (val NULL) */
static void set_field(const char *str, const char *name, const char *val, char *out, int len) {
    const char *a = strstr(str, name), *e;
    if (!a) {
        snprintf(out, len, "%s", str);
        return;
    }
    e = strchr(a + 1, '|');
    if (!e) e = a + strlen(a);
    if (val) snprintf(out, len, "%.*s%s%s%s", (int)(a - str), str, name, val, e);
    else snprintf(out, len, "%.*s%s", (int)(a - str), str, e);
}

/* Review Focus 4: `.ab=` round trip; a 0.9 load (no .ab, no .bs) plays the fallback root; bad fields ignored; a key change
 * keeps absolute steps */
static void test_10_abs_state(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], buf2[SEQ_STATE_MAX], mod[SEQ_STATE_MAX];
    char lbl[8];
    bass_lane(&s, GEN_BASS_WALK, 77, 60, 16);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_MOVEMENT, 40);
    seq_set_step_note(&s, LANE_BASS, 0, 3, 61);    /* Em: root E, 61 nearest E4 64 (oct +1) */
    seq_set_step_note(&s, LANE_BASS, 1, 2, 31);    /* C: 31 nearest C2 36 (oct -1) */
    seq_state_save(&s, buf, sizeof buf);
    const char *r = strstr(buf, "|A2=0:");
    CHECK(strstr(buf, "|A2.ab=033d121f|A2.bs=40,") && r && strstr(r, "0316") && strstr(r, "1214"),
          "saved: .ab pairs before .bs, the records the root at the nearest octave");
    CHECK(seq_state_load(&t, buf) && bass_steps(&t)[3].abs == 61 && bass_steps(&t)[18].abs == 31 && t.part[0].bass.fix[3] == 61 &&
          t.part[0].bass.fix[18] == 31, "a 0.10 load restores the pitches (0.19: as the DRAWN Bass's fixed notes)");
    seq_state_save(&t, buf2, sizeof buf2);
    CHECK(strstr(buf2, "|A2.g=18,77,50|") && strstr(buf2, "|A2.ab=033d121f") && resaves_same(&t),
          "and saves them back as 2.ab, saved back byte for byte from then on (0.19: as DRAWN bars)");
    seq_cell_label(&t, LANE_BASS, 3, lbl, sizeof lbl);
    CHECK(!strcmp(lbl, "C#4"), "the cell shows C#4 (%s)", lbl);
    /* a 0.9 build: ignores .ab and .bs */
    set_field(buf, "|A2.ab=", NULL, buf2, sizeof buf2);
    set_field(buf2, "|A2.bs=", NULL, mod, sizeof mod);
    CHECK(seq_state_load(&t, mod) && !t.part[0].bass.fix[3] && bass_steps(&t)[3].on && t.part[0].lane[LANE_BASS].gen.style == GEN_BASS_DRAWN,
          "without .ab: the record (0.19: a hit of the DRAWN Bass with no fixed note, its note the note model's)");
    /* malformed: ignored, the rest loads */
    static const char *const BAD[] = {"033d12", "033", "0300", "03ff", "033g", "ff3d", "013d", "", "033d121f00"};
    int bad = 1;
    for (int k = 0; k < (int)(sizeof BAD / sizeof BAD[0]); k++) {
        set_field(buf, "|A2.ab=", BAD[k], mod, sizeof mod);
        bad &= seq_state_load(&t, mod) && !t.part[0].bass.fix[3] && !t.part[0].bass.fix[18] && bass_steps(&t)[3].on &&   /* 0.19: DRAWN */
               t.part[0].bass.set.style == 29 && t.part[0].bass.set.k[BK_MOV] == 4;   /* the Walk as Upright Walk, its MOVEMENT 40 under the style's */
        if (!bad) printf("     bad .ab variant %d (%s) not ignored\n", k, BAD[k]);
    }
    CHECK(bad, "a malformed .ab is ignored: the steps stay chord-relative (0.19: no fixed note), the rest loads");
    /* a key or scale change keeps them, generated (Walk) and MANUAL */
    for (int manual = 0; manual < 2; manual++) {
        bass_lane(&s, GEN_BASS_WALK, 77, 60, 16);
        if (manual) seq_set_style(&s, LANE_BASS, GEN_MANUAL);
        seq_set_step_note(&s, LANE_BASS, 0, 3, 61);
        seq_set_key(&s, 7);
        seq_set_scale(&s, SCALE_MAJOR);
        seq_cell_label(&s, LANE_BASS, 3, lbl, sizeof lbl);
        CHECK(bass_steps(&s)[3].abs == 61 && !strcmp(lbl, "C#4"), "%s: a key and scale change keep the absolute step",
              manual ? "MANUAL" : "Walk");
    }
    /* Final review Minor 2: in memory the record is a fixed placeholder, never a stale fallback; the save computes it */
    bass_lane(&s, GEN_BASS_WALK, 77, 60, 16);
    seq_set_step_note(&s, LANE_BASS, 0, 3, 61);
    seq_set_key(&s, 1);
    seq_set_prog_preset(&s, 2);
    seq_set_chord_len(&s, 8);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(seq_state_load(&t, buf) && !memcmp(&bass_steps(&t)[3], &bass_steps(&s)[3], sizeof(seq_step_t)) &&
          bass_steps(&s)[3].tone == TONE_R && bass_steps(&s)[3].oct == 0 && strstr(buf, "|A2.ab=033d"),
          "an absolute step's record in memory is the placeholder (R, 0), the same after a reload");
}

/* Review Focus 1: the transforms at neutral are the identity, and off neutral and back gives 0.9's steps, MIDI and save */
static void test_10_transforms_neutral(void) {
    static seq_t s;
    static char ref[SEQ_STATE_MAX], buf[SEQ_STATE_MAX];
    static seq_step_t base[SEQ_LANE_STEPS];
    int ok = 1;
    for (int style = GEN_BASS_ROOT; style <= GEN_BASS_WALK; style++) {
        bass_lane(&s, style, 1234, 85, 8);
        memcpy(base, bass_steps(&s), sizeof base);
        seq_state_save(&s, ref, sizeof ref);
        nev = 0;
        play_stop(&s, 16.0);
        unsigned h = fold_events(2166136261u);
        for (int w = BSHAPE_MOVEMENT; w <= BSHAPE_OCTAVES; w++) seq_set_lane_shape(&s, LANE_BASS, w, 70);
        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1);
        for (int w = BSHAPE_MOVEMENT; w <= BSHAPE_OCTAVES; w++) seq_set_lane_shape(&s, LANE_BASS, w, 0);
        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 0);
        nev = 0;
        play_stop(&s, 16.0);
        seq_state_save(&s, buf, sizeof buf);
        ok &= !memcmp(bass_steps(&s), base, sizeof base) && fold_events(2166136261u) == h && !strcmp(buf, ref) &&
              !strstr(buf, ".ab=") && !strstr(buf, ".bs=");
    }
    CHECK(ok, "Root, Pump, Walk: transforms off neutral and back give the same steps, MIDI and save text (no .ab, no .bs)");
}

/* 0.11: the melody RECIPE style (GEN_MEL_RECIPE) on Melody 1 */

static seq_step_t *mel_st(seq_t *s) { return s->part[s->cur_part].lane[LANE_MEL_UP].steps; }

static const seq_part_t *mel_part(const seq_t *s) { return &s->part[s->cur_part]; }

static int mel_lk(const seq_t *s, int i) { return mel_part(s)->lane[LANE_MEL_UP].lock[i / 8] >> (i % 8) & 1; }

/* What the recipe lane should hold, built here from theory.h (each span's slot chord: its scale, borrow and own colour;
 * the COLOUR knob at 0) and mel_generate_v1 */
static void mel_ref(const seq_t *s, seq_step_t *out) {
    const seq_part_t *p = mel_part(s);
    const seq_gen_t *g = &p->lane[LANE_MEL_UP].gen;
    mel_chord_t ch[SEQ_LANE_STEPS / 4];
    int n = (p->bars * SEQ_STEPS + p->chord_len - 1) / p->chord_len;
    for (int j = 0; j < n; j++) {
        int slot = j % p->prog_len, sc = theory_slot_scale(s->scale, p->src[slot]), deg = p->prog[slot], notes[THEORY_CHORD_MAX];
        int k = theory_chord_notes(s->key, sc, deg, p->colour[slot], notes);
        if (k > 4) k = 4;
        memset(&ch[j], 0, sizeof ch[j]);
        ch[j].root_pc = theory_degree_pc(s->key, sc, deg);
        for (int i = 0; i < k; i++) ch[j].tones[i] = notes[i] % 12;
        ch[j].ntones = k;
        ch[j].seventh = theory_degree_pc(s->key, sc, deg + 6);
    }
    mel_params_t mp = {&p->mel, g->seed, g->complexity, p->bars, p->chord_len, s->key, s->scale, ch, p->lane[LANE_MEL_UP].bar_var};
    memset(out, 0, sizeof(seq_step_t) * SEQ_LANE_STEPS);
    mel_generate_v1(&mp, out);
}

/* the lane is the recipe's line at every unlocked step */
static int mel_is_ref(seq_t *s) {
    static seq_step_t ref[SEQ_LANE_STEPS];
    mel_ref(s, ref);
    for (int i = 0; i < mel_part(s)->bars * SEQ_STEPS; i++)
        if (!mel_lk(s, i) && memcmp(&mel_st(s)[i], &ref[i], sizeof ref[i])) return 0;
    return 1;
}

static unsigned mel_hash(const seq_t *s) {   /* Melody 1's steps in every part in use (as the 0.10 build hashed them) */
    unsigned h = 2166136261u;
    for (int p = 0; p < SEQ_NPARTS; p++) {
        if (!(s->used_mask >> p & 1)) continue;
        const unsigned char *b = (const void *)s->part[p].lane[LANE_MEL_UP].steps;
        for (size_t i = 0; i < sizeof s->part[p].lane[LANE_MEL_UP].steps; i++) h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* Saved by the 0.10 build (ebc6799 + Task 1): a Hook Melody 1 at c64 with a FIX pick (A), an Arp (C), a Manual one
 * with an absolute note (D), a borrowed 7th, a shaped Walk, key A. Its Melody 1 steps after the load hash to 0x0642faab
 * in that build. */
static const char L010[] =
    "csq2|gv=1|k=9|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0326|A.voi=1|A.pq=00,11,00,00|A.sl=2|ch=1,2,3|A1=0:|A1.g=2,1,30"
    "|A2=0:|A2.g=6,1,30|A2.bs=0,30,0,50,0,50,50,0,0|A3=0:1417640|A3.g=7,1,64"
    "|A3.lk=0000010000000000000000000000000000000000000000000000000000000000|A3.ab=1449|A4=0:|C.len=2|C.cl=16|C.h=30,1"
    "|C.prog=0326|C.voi=1|C.pq=00,11,00,00|C.sl=2|C1=0:|C1.g=2,1,30|C2=0:|C2.g=6,1,30|C2.bs=0,30,0,50,0,50,50,0,0"
    "|C3=0:1417640|C3.g=8,1,64|C3.lk=0000010000000000000000000000000000000000000000000000000000000000|C3.ab=1449|C4=0:"
    "|D.len=4|D.cl=16|D.h=30,1|D.prog=0326|D.voi=1|D.pq=00,11,00,00|D.sl=2|D1=0:|D1.g=2,1,30|D2=0:|D2.g=6,1,30"
    "|D2.bs=0,30,0,50,0,50,50,0,0|D3=0:00166d0021661003365900526620061663008367400a166f110556d01216610135559014176401565620"
    "165563018167401a556f120556d0225561023155902525620263563028167402a556f130166a031755c0331670034556f036656e038556d03a166c1"
    "|D3.g=0,1,64|D3.ab=00461449|D4=0:";

/* Review Focus 1: a 0.10 project loads to the same Melody 1 steps, plays its classic styles, and saves byte for byte */
static void test_11_literal_010_state(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, L010) && mel_hash(&s) == 0x0642faabu, "a 0.10 state loads to 0.10's Melody 1 steps (0x%08x)",
          mel_hash(&s));
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && saves_but_drawn(&s, L010) && !strstr(buf, ".mr="),
          "and saves back byte for byte, no .mr (0.18: but its Chords lanes, as DRAWN bars)");
    int ok = seq_style(&s, LANE_MEL_UP) == GEN_MEL_HOOK && seq_mel_type(&s) == MEL_HOOK && seq_mel_is_classic(&s);
    seq_select_part(&s, 2);
    ok &= seq_style(&s, LANE_MEL_UP) == GEN_MEL_ARP && seq_mel_type(&s) == MEL_ARP && seq_mel_is_classic(&s);
    seq_select_part(&s, 3);
    ok &= seq_style(&s, LANE_MEL_UP) == GEN_MANUAL && seq_mel_type(&s) == -1 && !seq_mel_is_classic(&s);
    CHECK(ok, "classic Hook shows Hook, Arp shows Arp, Manual no type");
    /* a classic lane ignores the part's recipe: its GATE, its type */
    seq_select_part(&s, 0);
    unsigned h = mel_hash(&s);
    s.part[0].mel.kn[MK_GATE] = 100;
    s.part[0].mel.type = MEL_APPR;
    CHECK(mel_hash(&s) == h && seq_mel_knob(&s, MK_GATE) == 70 && seq_mel_type(&s) == MEL_HOOK,
          "a classic lane plays and shows the Hook's defaults, whatever the part's recipe holds");
    CHECK(saves_but_drawn(&s, L010), "and still saves as 0.10 (0.18: but its Chords lanes)");
    static seq_t a, b;
    fresh(&a, 1);
    fresh(&b, 1);
    seq_set_style(&a, LANE_MEL_UP, GEN_MEL_HOOK);
    seq_set_style(&b, LANE_MEL_UP, GEN_MEL_HOOK);
    b.part[0].mel.kn[MK_GATE] = 0;
    nev = 0;
    play_stop(&a, 16.0);
    unsigned ha = fold_events(2166136261u);
    nev = 0;
    play_stop(&b, 16.0);
    CHECK(fold_events(2166136261u) == ha, "a classic Hook plays the same MIDI whatever the part's recipe GATE");
}

/* Each type: its defaults on the lane, the recipe's line, one undo step back to the classic Hook */
static void test_11_types(void) {
    static seq_t s;
    static seq_lane_t was;
    int ok = 1, undo = 1;
    for (int t = 0; t < MEL_NTYPES; t++) {
        mel_recipe_t d;
        int cx;
        seq_init(&s, emit, NULL);
        old_mel(&s);
        memcpy(&was, &s.part[0].lane[LANE_MEL_UP], sizeof was);
        memset(&d, 0, sizeof d);
        mel_defaults(t, &d, &cx);
        seq_mel_set_type(&s, t);
        ok &= seq_style(&s, LANE_MEL_UP) == GEN_MEL_RECIPE && seq_complexity(&s, LANE_MEL_UP) == cx && seq_mel_type(&s) == t &&
              !seq_mel_is_classic(&s) && !memcmp(s.part[0].mel.kn, d.kn, sizeof d.kn) && !memcmp(s.part[0].mel.ph, d.ph, sizeof d.ph) &&
              s.undo_n == 1 && mel_is_ref(&s) && any_on(&s.part[0].lane[LANE_MEL_UP]);
        for (int w = 0; w < 3; w++) ok &= seq_mel_opt(&s, w) == d.opt[w];
        for (int k = 0; k < MK_COUNT; k++) ok &= seq_mel_knob(&s, k) == d.kn[k];
        seq_undo(&s);
        undo &= same_lane(&s, LANE_MEL_UP, &was) && seq_style(&s, LANE_MEL_UP) == GEN_MEL_HOOK;
    }
    CHECK(ok, "every type: GEN_MEL_RECIPE at its defaults (options, phrasing, knobs, complexity), the recipe's line, one undo step");
    CHECK(undo, "and its undo gives back the classic Hook lane exactly (style, complexity, steps)");
    seq_init(&s, emit, NULL);
    old_mel(&s);
    seq_mel_set_type(&s, -1);
    seq_mel_set_type(&s, MEL_NTYPES);
    CHECK(seq_style(&s, LANE_MEL_UP) == GEN_MEL_HOOK && !s.undo_n, "a bad type is ignored");
    seq_set_style(&s, LANE_MEL_UP, GEN_MEL_RECIPE);
    CHECK(seq_style(&s, LANE_MEL_UP) == GEN_MEL_RECIPE && mel_is_ref(&s), "GEN_MEL_RECIPE fits Melody 1 (the part's recipe)");
    seq_set_style(&s, LANE_BASS, GEN_MEL_RECIPE);
    seq_set_style(&s, LANE_CHORDS, GEN_MEL_RECIPE);
    CHECK(seq_style(&s, LANE_BASS) == GEN_BASS_DRAWN && seq_style(&s, LANE_CHORDS) == GEN_CH_SUSTAIN, "and no other lane");
}

/* Each setter's undo: what it changed comes back; knob turns are not undone (as the complexity) */
static void test_11_setters_undo(void) {
    static seq_t s;
    static seq_lane_t was;
    int rev, inv, shift;
    seq_init(&s, emit, NULL);
    old_mel(&s);   /* 0.20: the recipe setters leave a JAM lane alone */
    seq_mel_set_type(&s, MEL_HOOK);
    CHECK(s.undo_n == 1 && seq_style(&s, LANE_MEL_UP) == GEN_MEL_RECIPE && seq_complexity(&s, LANE_MEL_UP) == 40,
          "Hook on the classic Hook: converted (complexity 40), one undo step");
    seq_mel_set_type(&s, MEL_HOOK);
    CHECK(s.undo_n == 1, "the same type at its defaults: nothing changes, no undo");
    seq_mel_set_knob(&s, MK_RANGE, 90);
    seq_set_complexity(&s, LANE_MEL_UP, 77);
    CHECK(s.undo_n == 1 && seq_mel_knob(&s, MK_RANGE) == 90 && mel_is_ref(&s), "a knob turn regenerates, no undo");
    seq_mel_set_type(&s, MEL_HOOK);
    CHECK(s.undo_n == 2 && seq_mel_knob(&s, MK_RANGE) == 45 && seq_complexity(&s, LANE_MEL_UP) == 40,
          "the same type again resets its knobs and complexity: one undo step");
    seq_undo(&s);
    CHECK(seq_mel_knob(&s, MK_RANGE) == 90 && seq_complexity(&s, LANE_MEL_UP) == 77 && mel_is_ref(&s),
          "its undo brings the knobs and complexity back");
    int n = s.undo_n;
    seq_mel_set_opt(&s, 1, 0);
    CHECK(s.undo_n == n + 1 && seq_mel_opt(&s, 1) == 0 && mel_is_ref(&s), "an option: one undo step, regenerated");
    seq_mel_set_opt(&s, 1, 0);
    seq_mel_set_opt(&s, 3, 0);
    seq_mel_set_opt(&s, -1, 0);
    seq_mel_set_opt(&s, 1, 4);   /* Contour has 4 */
    seq_mel_set_opt(&s, 0, 3);   /* Motif has 3 */
    seq_mel_set_opt(&s, 2, -1);
    CHECK(s.undo_n == n + 1 && seq_mel_opt(&s, 1) == 0 && seq_mel_opt(&s, 0) == 1, "a re-send and out-of-range options: ignored");
    seq_mel_set_knob(&s, MK_SYNC, 5);
    seq_undo(&s);
    CHECK(seq_mel_opt(&s, 1) == 2 && seq_mel_knob(&s, MK_SYNC) == 5 && seq_mel_knob(&s, MK_RANGE) == 90 && mel_is_ref(&s),
          "its undo: the option back, the knobs turned since stay");
    n = s.undo_n;
    unsigned char ph0[MP_COUNT];
    memcpy(ph0, s.part[0].mel.ph, sizeof ph0);
    seq_mel_set_phrase(&s, MEL_PHRASE_2_2);
    seq_mel_set_phrase(&s, MEL_NPHRASES);
    seq_mel_set_phrase(&s, -1);
    CHECK(s.undo_n == n + 1 && seq_mel_phrase(&s) == MEL_PHRASE_2_2 && mel_is_ref(&s), "a phrase: one undo step; bad: ignored");
    seq_undo(&s);
    CHECK(!memcmp(s.part[0].mel.ph, ph0, sizeof ph0) && mel_is_ref(&s), "its undo: the phrasing back");
    for (int k = 0; k < MK_COUNT; k++) seq_mel_set_knob(&s, k, 11 + k);
    seq_mel_set_knob(&s, MK_COUNT, 50);
    seq_mel_set_knob(&s, MK_RANGE, 101);
    seq_mel_set_knob(&s, MK_RANGE, -1);
    seq_set_complexity(&s, LANE_MEL_UP, 9);
    n = s.undo_n;
    seq_mel_reset_knobs(&s);
    int dflt = seq_complexity(&s, LANE_MEL_UP) == 40 && seq_mel_knob(&s, MK_RANGE) == 45 && seq_mel_knob(&s, MK_GATE) == 70;
    CHECK(s.undo_n == n + 1 && dflt && mel_is_ref(&s), "RESET: the Hook's knobs and complexity, one undo step");
    seq_mel_reset_knobs(&s);
    CHECK(s.undo_n == n + 1, "RESET at the defaults: no undo");
    seq_undo(&s);
    CHECK(seq_complexity(&s, LANE_MEL_UP) == 9 && seq_mel_knob(&s, MK_RANGE) == 11 && seq_mel_knob(&s, MK_GATE) == 17,
          "its undo: the turned knobs and complexity back (bad knob values were ignored)");
    n = s.undo_n;
    seq_mel_set_flags(&s, 1, 1, 3, 1);
    seq_mel_flags(&s, &rev, &inv, &shift);
    CHECK(s.undo_n == n + 1 && rev && inv && shift == 3 && mel_is_ref(&s), "the move flags: one undo step");
    seq_mel_set_flags(&s, 2, 0, 0, 1);
    seq_mel_set_flags(&s, 0, 0, 16, 1);
    seq_mel_set_flags(&s, 0, -1, 0, 1);
    seq_mel_flags(&s, NULL, NULL, &shift);
    CHECK(s.undo_n == n + 1 && shift == 3, "bad flags: ignored");
    seq_undo(&s);
    seq_mel_flags(&s, &rev, &inv, &shift);
    CHECK(!rev && !inv && !shift && mel_is_ref(&s), "its undo: no flags");
    seq_mel_set_flags(&s, 0, 0, 5, 0);
    seq_mel_flags(&s, NULL, NULL, &shift);
    CHECK(s.undo_n == n && shift == 5 && mel_is_ref(&s), "flags with undo 0: applied, no undo");
    /* conversions: Arp keeps its type, Manual counts as Hook; their undo is the classic lane again */
    seq_init(&s, emit, NULL);
    seq_set_style(&s, LANE_MEL_UP, GEN_MEL_ARP);
    seq_set_complexity(&s, LANE_MEL_UP, 64);
    memcpy(&was, &s.part[0].lane[LANE_MEL_UP], sizeof was);
    n = s.undo_n;
    CHECK(seq_mel_opt(&s, 0) == 0 && seq_mel_opt(&s, 1) == 1 && seq_mel_knob(&s, MK_GATE) == 40, "a classic Arp shows Arp's defaults");
    seq_mel_set_opt(&s, 0, 2);
    CHECK(s.undo_n == n + 1 && seq_mel_type(&s) == MEL_ARP && seq_mel_opt(&s, 0) == 2 && seq_mel_opt(&s, 1) == 1 &&
              seq_complexity(&s, LANE_MEL_UP) == 30 && seq_mel_knob(&s, MK_GATE) == 40 && mel_is_ref(&s),
          "an option on a classic Arp: an Arp recipe at its defaults (complexity 30) with that option, one undo step");
    seq_undo(&s);
    CHECK(same_lane(&s, LANE_MEL_UP, &was), "its undo: the classic Arp at c64 exactly");
    seq_set_style(&s, LANE_MEL_UP, GEN_MANUAL);
    seq_set_step(&s, LANE_MEL_UP, 0, 1, 1);
    memcpy(&was, &s.part[0].lane[LANE_MEL_UP], sizeof was);
    n = s.undo_n;
    seq_mel_set_phrase(&s, MEL_PHRASE_2_2);
    CHECK(s.undo_n == n + 1 && seq_mel_type(&s) == MEL_HOOK && seq_mel_phrase(&s) == MEL_PHRASE_2_2 &&
              seq_complexity(&s, LANE_MEL_UP) == 40 && mel_is_ref(&s),
          "a phrase on a Manual lane: a Hook recipe with that phrase, one undo step");
    seq_undo(&s);
    CHECK(same_lane(&s, LANE_MEL_UP, &was) && seq_mel_phrasing(&s, MP_CADENCE) == MEL_CAD_QA,
          "its undo: the Manual lane exactly (showing the Hook's phrasing)");
    seq_mel_reset_knobs(&s);
    CHECK(seq_style(&s, LANE_MEL_UP) == GEN_MEL_RECIPE && seq_mel_type(&s) == MEL_HOOK && s.undo_n == n + 1,
          "RESET on a Manual lane converts too (decision 7)");
    seq_undo(&s);
    seq_mel_set_flags(&s, 0, 1, 0, 1);
    CHECK(seq_style(&s, LANE_MEL_UP) == GEN_MEL_RECIPE && s.undo_n == n + 1, "and so do the move flags");
    seq_undo(&s);
    CHECK(same_lane(&s, LANE_MEL_UP, &was), "each undone back to the Manual lane");
}

/* The pitch classes Melody 1 plays in bar b */
static unsigned bar_pcs(seq_t *s, int b) {
    unsigned m = 0;
    for (int i = 0; i < SEQ_STEPS; i++)
        if (mel_st(s)[b * SEQ_STEPS + i].on) m |= 1u << (mel_st(s)[b * SEQ_STEPS + i].abs % 12);
    return m;
}

/* Review Focus 3: a borrowed IV in E minor plays its major third, a 7th chord its 7th, through the core */
static void test_11_borrowed(void) {
    static seq_t s;
    static const int deg[4] = {0, 3, 0, 6};
    static const unsigned char bor[4] = {0, 1, 0, 0}, dia[4] = {0}, col7[4] = {COL_7TH, COL_7TH, 0, 0};
    seq_init(&s, emit, NULL);   /* E minor, 4 bars, a chord a bar */
    old_mel(&s);   /* 0.20: the recipe setters leave a JAM lane alone */
    seq_set_progression(&s, 4, deg, bor, dia, 0);
    seq_mel_set_type(&s, MEL_GUIDE);
    unsigned iv = bar_pcs(&s, 1);
    CHECK(seq_slot_src(&s, 1) == 1 && iv && (iv & (1u << 1)) && !(iv & ~(1u << 1 | 1u << 8)) && mel_is_ref(&s),
          "Guide tones over the borrowed IV (A major, from E major): C# and G# only, C# played (0x%03x)", iv);
    seq_set_progression(&s, 4, deg, dia, dia, 0);
    iv = bar_pcs(&s, 1);
    CHECK(iv && !(iv & ~(1u << 0 | 1u << 7)), "the diatonic iv (Am): C and G (0x%03x)", iv);
    seq_mel_set_type(&s, MEL_ARP);
    seq_set_progression(&s, 4, deg, bor, col7, 0);
    unsigned i7 = bar_pcs(&s, 0), a7 = bar_pcs(&s, 1), tri = bar_pcs(&s, 2);
    CHECK((i7 & 1u << 2) && !(i7 & ~(1u << 4 | 1u << 7 | 1u << 11 | 1u << 2)) && (a7 & 1u << 8) && (a7 & 1u << 1) &&
              !(a7 & ~(1u << 9 | 1u << 1 | 1u << 4 | 1u << 8)) && !(tri & ~(1u << 4 | 1u << 7 | 1u << 11)) && mel_is_ref(&s),
          "Arp: Em7 plays its D, the borrowed A7 (Amaj7) its C# and G#, the Em triad no 7th (0x%03x 0x%03x 0x%03x)", i7, a7, tri);
}

/* Review Focus 4: a FIX pick survives every type, option, phrase, knob, move flag, GEN and harmony change, a save and
 * a reload, and a pick on the classic lane survives the conversion */
static void test_11_locks(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    old_mel(&s);
    seq_set_step_note(&s, LANE_MEL_UP, 0, 6, 71);   /* on the classic Hook */
    seq_mel_set_type(&s, MEL_APPR);
    seq_set_step_note(&s, LANE_MEL_UP, 1, 5, 77);   /* on the recipe */
    seq_set_step(&s, LANE_MEL_UP, 2, 3, !mel_st(&s)[35].on);   /* a tap locks too */
    seq_step_t tap = mel_st(&s)[35];
    int ok = 1, step = 0;
#define PICKS_OK() (mel_st(&s)[6].abs == 71 && mel_st(&s)[6].on && mel_lk(&s, 6) && mel_st(&s)[21].abs == 77 && \
                    mel_st(&s)[21].on && mel_lk(&s, 21) && !memcmp(&mel_st(&s)[35], &tap, sizeof tap) && mel_lk(&s, 35))
    for (int k = 0; k < 40; k++) {
        int op = k % 20;
        if (op < MEL_NTYPES) seq_mel_set_type(&s, (op + k) % MEL_NTYPES);
        else if (op == 7) seq_mel_set_opt(&s, k % 3, 1);
        else if (op == 8) seq_mel_set_phrase(&s, k % 2);
        else if (op == 9) seq_mel_set_knob(&s, k % MK_COUNT, k * 7 % 101);
        else if (op == 10) seq_mel_reset_knobs(&s);
        else if (op == 11) seq_mel_set_flags(&s, k & 1, k >> 1 & 1, k % 16, 1);
        else if (op == 12) seq_generate(&s, LANE_MEL_UP);
        else if (op == 13) seq_set_complexity(&s, LANE_MEL_UP, k * 13 % 101);
        else if (op == 14) seq_set_key(&s, k % 12);
        else if (op == 15) seq_set_scale(&s, k % SCALE_COUNT);
        else if (op == 16) seq_set_slot(&s, 2, 1, k % 2 ? 2 : 0, COL_7TH);
        else if (op == 17) seq_set_chord_len(&s, k % 3 ? 8 : 16);
        else if (op == 18) seq_set_prog_preset(&s, k % 5);
        else seq_undo(&s);
        if (!PICKS_OK() || !mel_is_ref(&s)) ok = 0, step = k;
        if (!ok) break;
    }
    CHECK(ok, "the picks and the tap stay (absolute, locked) and the rest is the recipe's line, over 40 edits (failed at %d)", step);
    seq_set_vknob(&s, VK_COL, 4);
    ok = PICKS_OK();
    seq_set_vknob(&s, VK_COL, 0);
    seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    seq_state_load(&t, buf);
    int same = 1;   /* what plays (a locked off-step's other fields are the generator's after a load, as in 0.10) */
    for (int i = 0; i < mel_part(&s)->bars * SEQ_STEPS; i++)
        same &= mel_st(&t)[i].on == mel_st(&s)[i].on && (!mel_st(&s)[i].on || !memcmp(&mel_st(&t)[i], &mel_st(&s)[i], sizeof tap));
    CHECK(ok && same && mel_lk(&t, 6) && mel_lk(&t, 21) && mel_lk(&t, 35),
          "and through the COLOUR knob, a save and a reload");
#undef PICKS_OK
}

/* Decision 8: the save writes .g as the Hook and the recipe in .mr; a reload is the same; 0.10 (no .mr) plays the Hook */
static void test_11_state(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], buf2[SEQ_STATE_MAX], mod[SEQ_STATE_MAX];
    char want[96];
    seq_init(&s, emit, NULL);
    old_mel(&s);   /* 0.20: the recipe setters leave a JAM lane alone */
    seq_mel_set_type(&s, MEL_SEQ);
    seq_mel_set_opt(&s, 0, 2);
    seq_mel_set_opt(&s, 1, 2);
    seq_mel_set_opt(&s, 2, 1);
    seq_mel_set_phrase(&s, MEL_PHRASE_2_2);
    for (int k = 0; k < MK_COUNT; k++) seq_mel_set_knob(&s, k, 10 * k + 5);
    seq_mel_set_flags(&s, 1, 0, 7, 1);
    seq_set_complexity(&s, LANE_MEL_UP, 66);
    seq_generate(&s, LANE_MEL_UP);
    seq_set_step_note(&s, LANE_MEL_UP, 2, 9, 80);
    int seed = s.part[0].lane[LANE_MEL_UP].gen.seed;
    seq_select_part(&s, 2);
    seq_mel_set_type(&s, MEL_APPR);
    seq_select_part(&s, 0);
    seq_state_save(&s, buf, sizeof buf);
    snprintf(want, sizeof want, "|A3.g=%d,%d,66|A3.lk=", GEN_MEL_HOOK, seed);
    CHECK(strstr(buf, want) && strstr(buf, "|A3.ab=2950|A3.mr=4,2,2,1,1,5,15,25,35,45,55,65,1,0,7|A4=0:") &&
              strstr(buf, "|C3.g=7,") && strstr(buf, "|C3.mr=6,0,0,1,0,40,20,40,60,60,50,50,0,0,0|C3.mp=1,1,1,1|C4=0:"),
          "saved: .g the Hook with the seed and complexity, .mr the recipe in order (its phrase the nearest old one), .mp the"
          " phrasing when it isn't an old phrase's, per part (C: a copy of the Verse, at Approach's own phrasing)");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_style(&t, LANE_MEL_UP) == GEN_MEL_RECIPE &&
              !memcmp(&t.part[0].mel, &s.part[0].mel, sizeof s.part[0].mel) &&
              !memcmp(&t.part[2].mel, &s.part[2].mel, sizeof s.part[2].mel) && t.part[2].lane[LANE_MEL_UP].gen.style == GEN_MEL_RECIPE &&
              !memcmp(mel_st(&t), mel_st(&s), sizeof(seq_step_t) * SEQ_LANE_STEPS) && mel_st(&t)[41].abs == 80 && mel_lk(&t, 41),
          "a reload: both recipes, the same steps, the pick");
    CHECK(seq_state_save(&t, buf2, sizeof buf2) > 0 && !strcmp(buf, buf2), "and saves back byte for byte");
    /* what 0.10 reads: the same text without .mr (or VST-12's .mp) */
    set_field(buf, "|A3.mr=", NULL, buf2, sizeof buf2);
    set_field(buf2, "|C3.mr=", NULL, mod, sizeof mod);
    set_field(mod, "|A3.mp=", NULL, buf2, sizeof buf2);
    set_field(buf2, "|C3.mp=", NULL, mod, sizeof mod);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, mod) && seq_style(&t, LANE_MEL_UP) == GEN_MEL_HOOK && t.part[0].lane[LANE_MEL_UP].gen.seed == seed &&
              seq_complexity(&t, LANE_MEL_UP) == 66 && mel_st(&t)[41].abs == 80 && mel_lk(&t, 41) && lane_is_gen(&t, LANE_MEL_UP) &&
              t.part[2].lane[LANE_MEL_UP].gen.style == GEN_MEL_HOOK,
          "without .mr (0.10): a classic Hook from the same seed and complexity, the pick kept");
    seq_state_save(&t, buf2, sizeof buf2);
    CHECK(!strcmp(buf2, mod), "which saves as that text again");
    /* a bad .mr: ignored, the Hook stays */
    static const char *const BAD[] = {"", "4,2,2,1,1,5,15,25,35,45,55,65,1,0", "4,2,2,1,1,5,15,25,35,45,55,65,1,0,7,0",
                                      "7,0,0,0,0,5,15,25,35,45,55,65,1,0,7", "4,3,2,1,1,5,15,25,35,45,55,65,1,0,7",
                                      "4,2,3,1,1,5,15,25,35,45,55,65,1,0,7", "4,2,2,2,1,5,15,25,35,45,55,65,1,0,7",
                                      "4,2,2,1,2,5,15,25,35,45,55,65,1,0,7", "4,2,2,1,1,101,15,25,35,45,55,65,1,0,7",
                                      "4,2,2,1,1,5,15,25,35,45,55,65,2,0,7", "4,2,2,1,1,5,15,25,35,45,55,65,1,2,7",
                                      "4,2,2,1,1,5,15,25,35,45,55,65,1,0,16", "4,2,2,1,1,5,15,25,35,45,55,65,1,0,x",
                                      "4,2,2,1,1,5,15,25,35,45,55,65,1,,7", "-4,2,2,1,1,5,15,25,35,45,55,65,1,0,7"};
    int bad = 1;
    for (int k = 0; k < (int)(sizeof BAD / sizeof BAD[0]); k++) {
        set_field(buf, "|A3.mr=", BAD[k], mod, sizeof mod);
        seq_init(&t, emit, NULL);
        bad &= seq_state_load(&t, mod) && seq_style(&t, LANE_MEL_UP) == GEN_MEL_HOOK && t.part[0].lane[LANE_MEL_UP].gen.seed == seed &&
               t.part[2].lane[LANE_MEL_UP].gen.style == GEN_MEL_RECIPE && lane_is_gen(&t, LANE_MEL_UP);
        if (!bad) printf("     bad .mr variant %d (%s) not ignored\n", k, BAD[k]);
    }
    CHECK(bad, "a bad .mr is ignored: the Hook from .g, the other part's recipe still loads");
    /* .mr on another style than the Hook: ignored */
    set_field(buf, "|A3.g=", "8,1,30", mod, sizeof mod);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, mod) && seq_style(&t, LANE_MEL_UP) == GEN_MEL_ARP, ".mr beside an Arp .g: ignored (the Arp)");
}

/* GATE: a recipe melody's note length is len x gate_curve(MK_GATE); the step and its event timing are unchanged */
static double mel_first_len(seq_t *s, int g, int *len) {
    double ppq = 0;
    seq_mel_set_knob(s, MK_GATE, g);
    seq_set_mute(s, LANE_CHORDS, 1);
    seq_set_mute(s, LANE_BASS, 1);
    nev = 0;
    ev_seq = s;
    while (ppq < 16.0) {
        seq_block(s, ppq, bpm, 1, 128);
        ppq += blk();
        for (int i = 0; i < s->npending; i++)
            if (s->pending[i].status_off == 0x82) {
                int k = (int)s->cur_k;
                *len = mel_st(s)[k % (SEQ_STEPS * mel_part(s)->bars)].len;
                double d = s->pending[i].off_ppq - k / 4.0;
                ev_seq = NULL;
                seq_block(s, ppq, bpm, 0, 128);
                return d;
            }
    }
    ev_seq = NULL;
    seq_block(s, ppq, bpm, 0, 128);
    return -1;
}

static void test_11_gate(void) {
    static seq_t s;
    int len = 0, n;
    seq_init(&s, emit, NULL);
    old_mel(&s);   /* 0.20: the recipe setters leave a JAM lane alone */
    seq_mel_set_type(&s, MEL_GUIDE);   /* long notes: Half, so len 4 */
    n = s.undo_n;
    unsigned h = mel_hash(&s);
    double d50 = mel_first_len(&s, 50, &len), d0, d100;
    int l50 = len;
    d100 = mel_first_len(&s, 100, &len);
    d0 = mel_first_len(&s, 0, &len);
    CHECK(l50 > 1 && fabs(d50 - (l50 * 0.25 - 0.025)) < 1e-9 && fabs(d100 - (l50 * 0.5 - 0.025)) < 1e-9 &&
              fabs(d0 - (l50 * 0.05 - 0.025)) < 1e-9,
          "GATE 50: len x 1/16 (%.4f), 100: x2 (%.4f), 0: x0.2 (%.4f), len %d", d50, d100, d0, l50);
    CHECK(mel_hash(&s) == h && s.undo_n == n, "GATE changes no step and pushes no undo");
    seq_set_gate(&s, 100);
    double both = mel_first_len(&s, 100, &len);
    CHECK(fabs(both - (l50 * 1.0 - 0.025)) < 1e-9, "and multiplies with PLAY's GATE (%.4f)", both);
}

/* Ruling 6: key, scale, progression, slot, chord length, length and COLOUR changes regenerate the recipe: the same
 * steps as the recipe picked after them */
static void test_11_harmony_follows(void) {
    static seq_t a, b;
    int ok = 1, moved = 0;
    for (int op = 0; op < 7; op++) {
        seq_init(&a, emit, NULL);
        seq_init(&b, emit, NULL);
        old_mel(&a);   /* 0.20: the recipe setters leave a JAM lane alone */
        old_mel(&b);
        seq_mel_set_type(&a, MEL_ARP);
        seq_mel_set_flags(&a, 0, 0, 0, 1);
        seq_set_complexity(&a, LANE_MEL_UP, 70);
        seq_step_t before[SEQ_LANE_STEPS];
        memcpy(before, mel_st(&a), sizeof before);
        for (int k = 0; k < 2; k++) {
            seq_t *x = k ? &b : &a;
            if (op == 0) seq_set_key(x, 9);
            else if (op == 1) seq_set_scale(x, SCALE_DORIAN);
            else if (op == 2) seq_set_prog_preset(x, 3);
            else if (op == 3) seq_set_slot(x, 1, 0, 2, COL_7TH);
            else if (op == 4) seq_set_chord_len(x, 8);
            else if (op == 5) seq_set_bars(x, 8);
            else seq_set_vknob(x, VK_COL, -1);   /* Power: the recipe reads a root and a 5th */
            if (k) {
                seq_mel_set_type(&b, MEL_ARP);
                seq_set_complexity(&b, LANE_MEL_UP, 70);
            }
        }
        ok &= !memcmp(mel_st(&a), mel_st(&b), sizeof before) && seq_style(&a, LANE_MEL_UP) == GEN_MEL_RECIPE;
        moved += memcmp(mel_st(&a), before, sizeof before) != 0;
        if (op < 6) ok &= mel_is_ref(&a);
    }
    CHECK(ok && moved == 7, "key, scale, preset, FIX slot, chord length, length and COLOUR: the recipe follows (%d of 7 moved)", moved);
}

/* Cleanup ruling: a knob turn on a classic lane converts it (the shown type's defaults, one recipe snapshot) and then
 * turns the knob; a Manual lane ignores it. set_flags with undo 0 still pushes a snapshot when it converts. */
static void test_11_classic_knob(void) {
    static seq_t s;
    static seq_lane_t was;
    seq_init(&s, emit, NULL);
    seq_set_style(&s, LANE_MEL_UP, GEN_MEL_ARP);
    seq_set_complexity(&s, LANE_MEL_UP, 64);
    memcpy(&was, &s.part[0].lane[LANE_MEL_UP], sizeof was);
    int n = s.undo_n;
    seq_mel_set_knob(&s, MK_RANGE, 101);
    seq_mel_set_knob(&s, MK_COUNT, 5);
    CHECK(same_lane(&s, LANE_MEL_UP, &was) && s.undo_n == n, "a bad knob on a classic Arp: ignored, no conversion");
    seq_mel_set_knob(&s, MK_RANGE, 77);
    CHECK(seq_style(&s, LANE_MEL_UP) == GEN_MEL_RECIPE && seq_mel_type(&s) == MEL_ARP && seq_mel_knob(&s, MK_RANGE) == 77 &&
              seq_mel_knob(&s, MK_LEAPS) == 20 && seq_mel_opt(&s, 0) == 0 && seq_complexity(&s, LANE_MEL_UP) == 30 &&
              s.undo_n == n + 1 && mel_is_ref(&s),
          "a knob on a classic Arp: an Arp recipe at its defaults (c30) with that knob, the conversion one undo step");
    seq_mel_set_knob(&s, MK_LEAPS, 60);
    CHECK(s.undo_n == n + 1 && seq_mel_knob(&s, MK_LEAPS) == 60 && mel_is_ref(&s), "later turns: no undo");
    seq_undo(&s);
    CHECK(same_lane(&s, LANE_MEL_UP, &was), "the undo: the classic Arp at c64 exactly");
    seq_set_style(&s, LANE_MEL_UP, GEN_MANUAL);
    memcpy(&was, &s.part[0].lane[LANE_MEL_UP], sizeof was);
    n = s.undo_n;
    for (int k = 0; k < MK_COUNT; k++) seq_mel_set_knob(&s, k, 33);
    CHECK(same_lane(&s, LANE_MEL_UP, &was) && s.undo_n == n && seq_mel_type(&s) == -1, "a Manual lane: knob turns ignored");
    seq_mel_set_flags(&s, 1, 0, 0, 0);
    CHECK(seq_style(&s, LANE_MEL_UP) == GEN_MEL_RECIPE && s.undo_n == n + 1, "flags with undo 0 on a Manual lane: converted, one snapshot");
    seq_undo(&s);
    CHECK(same_lane(&s, LANE_MEL_UP, &was), "whose undo is the Manual lane exactly");
    seq_mel_set_type(&s, MEL_GUIDE);
    unsigned h = mel_hash(&s);
    n = s.undo_n;
    seq_mel_set_knob(&s, MK_GATE, 10);
    CHECK(mel_hash(&s) == h && s.undo_n == n && seq_mel_knob(&s, MK_GATE) == 10, "GATE on a recipe: no step change, no undo");
}

/* ---- 0.12 (plan decisions 2, 3, 7) ---- */

/* one stopped block (the host gives no position while stopped: ppq -1, but a tempo) */
static void idle(seq_t *s, int blocks, double tempo) {
    ev_seq = s;
    for (int i = 0; i < blocks; i++) seq_block(s, -1.0, tempo, 0, 128);
    ev_seq = NULL;
}

/* the first note-on of `note` on channel ch at or after event index from, or -1 */
static int on_of(int from, int ch, int note) {
    for (int i = from; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].note == note) return i;
    return -1;
}

static int off_of(int from, int ch, int note) {
    for (int i = from; i < nev; i++)
        if (is_off(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].note == note) return i;
    return -1;
}

/* Review Focus 1 (after 0.16: FOLLOW on by default, saved as `fo=0` when off, kept by INIT, a v1 reset and picks); the grid (seq_grid_bar, seq_step_on, seq_cell_label)
 * shows the playing bar while following and playing, the picked bar otherwise, and never a bar past the part's length */
static void test_12_follow(void) {
    static seq_t s, t;
    char buf[SEQ_STATE_MAX], a[16], b[16];
    int ok = 1, bad = -1;
    seq_init(&s, emit, NULL);
    CHECK(seq_follow(&s) && seq_grid_bar(&s) == 0, "a new instance starts with FOLLOW on");
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, "|fo="), "FOLLOW on saves no field (a state saves as 0.16 did)");
    seq_init(&t, emit, NULL);
    seq_set_follow(&t, 0);
    CHECK(seq_state_load(&t, buf) && seq_follow(&t), "a load without fo turns FOLLOW on");
    seq_set_follow(&s, 0);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|fo=0") != NULL, "FOLLOW off saves fo=0");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && !seq_follow(&t), "and loads off");
    seq_reset_v1(&t);
    CHECK(!seq_follow(&t), "a reset to v1's song keeps it");
    seq_init_preset(&t);
    CHECK(!seq_follow(&t), "so does INIT");
    seq_set_follow(&s, 1);
    seq_state_save(&s, buf, sizeof buf);
    seq_audition_note(&t, LANE_BASS, 40);
    CHECK(seq_state_load(&t, buf) && seq_follow(&t) && t.naud == 0, "a load empties the audition queue");
    seq_set_follow(&t, 0);
    int v1 = seq_state_load(&t, "csq1:1000000000000000100000001000000010101010101010101010101010101010:0000");
    CHECK(v1 && seq_follow(&t), "and a v1 load turns it on (loaded %d)", v1);

    /* each bar of a Manual Bass different: bar b has only step b on */
    fresh(&s, 0);
    CHECK(seq_follow(&s), "FOLLOW on after fresh()");
    for (int bar = 0; bar < 4; bar++)
        for (int i = 0; i < SEQ_STEPS; i++) seq_set_step(&s, LANE_BASS, bar, i, i == bar);
    seq_set_view(&s, 2);   /* a pick: FOLLOW stays */
    CHECK(seq_follow(&s) && seq_grid_bar(&s) == 2 && seq_step_on(&s, LANE_BASS, 2), "stopped, a pick shows that bar (FOLLOW kept)");
    double ppq = play(&s, 0.0, 5.0, 0);   /* bar 2 (0-based 1) */
    seq_cell_label(&s, LANE_BASS, 1, a, sizeof a);
    CHECK(seq_grid_bar(&s) == 1 && seq_view_bar(&s) == 2 && seq_strip_bar(&s) == 1 && seq_step_on(&s, LANE_BASS, 1) &&
          !seq_step_on(&s, LANE_BASS, 2) && strcmp(a, " "),
          "playing bar 2: the grid shows it (step 2 on, '%s'); the pick is kept", a);
    /* through a loop wrap (a 16-beat host loop on the 4-bar part): the grid is the playing bar at every block */
    ev_seq = &s;
    for (double done = 0; done < 40.0; done += blk()) {
        ev_ppq = ppq;
        seq_block(&s, ppq, bpm, 1, 128);
        if (seq_grid_bar(&s) != seq_cur_bar(&s) || seq_grid_bar(&s) >= seq_bars(&s)) ok = 0;
        if (!seq_step_on(&s, LANE_BASS, seq_grid_bar(&s))) ok = 0;
        ppq += blk();
        if (ppq >= 16.0) ppq -= 16.0;
    }
    ev_seq = NULL;
    CHECK(ok, "across loop wraps the grid is the playing bar at every block");
    seq_block(&s, ppq, bpm, 0, 128);   /* stop */
    CHECK(seq_grid_bar(&s) == 2 && seq_follow(&s), "stopped: the grid is the picked bar again, FOLLOW still on");
    ppq = play(&s, 4.0, 0.5, 0);       /* start again in song bar 2: 0.16, the part's bar 1 */
    CHECK(seq_grid_bar(&s) == 0, "started again: it follows (bar %d, the part's bar 1)", seq_grid_bar(&s) + 1);
    seq_set_view(&s, 3);
    CHECK(seq_follow(&s) && seq_grid_bar(&s) == 0 && seq_view_bar(&s) == 3, "picking a bar while playing: FOLLOW kept, the grid stays on the playing bar");
    seq_set_follow(&s, 0);
    CHECK(seq_grid_bar(&s) == 0 && seq_view_bar(&s) == 0, "FOLLOW off while playing: the grid holds the playing bar");
    seq_set_view(&s, 3);
    CHECK(seq_grid_bar(&s) == 3 && seq_step_on(&s, LANE_BASS, 3), "FOLLOW off: a pick shows that bar while playing");
    seq_set_follow(&s, 1);
    CHECK(seq_grid_bar(&s) == 0, "FOLLOW on again: the playing bar");
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");

    /* a part switch while following: an 8-bar Verse in bar 7 queues a 2-bar Chorus; the grid never shows a bar past
     * the playing part's length */
    fresh(&s, 1);
    seq_set_bars(&s, 8);
    seq_select_part(&s, 3);
    seq_set_bars(&s, 2);
    seq_select_part(&s, 0);
    seq_set_view(&s, 6);
    seq_set_follow(&s, 1);
    ppq = play(&s, 0, 26.0, 0);   /* bar 7 */
    CHECK(seq_grid_bar(&s) == 6, "the Verse's bar 7 plays and shows");
    seq_select_part(&s, 3);
    ok = 1;
    ev_seq = &s;
    for (double end = ppq + 12.0; ppq < end; ppq += blk()) {
        ev_ppq = ppq;
        seq_block(&s, ppq, bpm, 1, 128);
        if (seq_grid_bar(&s) >= seq_bars(&s) || seq_grid_bar(&s) != seq_cur_bar(&s)) ok = 0, bad = seq_grid_bar(&s);
    }
    ev_seq = NULL;
    CHECK(ok && seq_part(&s) == 3 && seq_view_bar(&s) == 0, "after the switch the grid is the Chorus's playing bar, never past "
          "its 2 bars (bad %d); the pick past its length went to bar 1", bad);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(seq_grid_bar(&s) == 0 && balanced(), "stopped in the Chorus: bar 1, no stuck notes");
    seq_cell_label(&s, LANE_MEL_UP, 0, b, sizeof b);
    CHECK(strcmp(b, " ") != 0, "its cells label (Melody 1 step 1 '%s')", b);
}

/* Task 1's finding, fixed: a FIX edit while following lands on the playing bar (the engine passes seq_grid_bar), and
 * sounds when that step comes round in the same bar */
static void test_12_fix_follows(void) {
    static seq_t s;
    int note = 45, i, bar0;
    fresh(&s, 0);
    seq_set_follow(&s, 1);
    bar0 = s.part[0].lane[LANE_BASS].steps[12].on;
    play(&s, 0.0, 4.5, 0);   /* bar 2, beat 1.5: the view is bar 1 */
    CHECK(seq_view_bar(&s) == 0 && seq_grid_bar(&s) == 1, "playing bar 2 with bar 1 picked, following");
    int from = nev;
    seq_set_step_note(&s, LANE_BASS, seq_grid_bar(&s), 12, note);
    CHECK(s.part[0].lane[LANE_BASS].steps[SEQ_STEPS + 12].abs == note && s.part[0].lane[LANE_BASS].steps[12].on == bar0 &&
          !s.part[0].lane[LANE_BASS].steps[12].abs, "the edit is on bar 2, step 13; bar 1 untouched");
    double ppq = play(&s, 4.5, 3.0, 0);
    i = on_of(from, 3, note);
    CHECK(i >= 0 && evs[i].ppq >= 7.0 && evs[i].ppq < 7.0 + 2 * blk(), "it sounds at beat 7.0, the same bar (at %.3f)",
          i >= 0 ? evs[i].ppq : -1.0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");
}

/* Review Focus 2: auditions stopped and playing, on a muted lane, across a channel change, a full queue, ALL NOTES OFF,
 * a stop and a retrigger; every note ends */
static void test_12_audition(void) {
    static seq_t s;
    int i, j, n[4], c = 0, want[8], nw = 0;
    double beat = 44100.0 * 60.0 / bpm / 128;   /* blocks a beat */
    fresh(&s, 0);   /* channels 2, 3, 4 */
    seq_audition_note(&s, LANE_BASS, 40);
    CHECK(nev == 0 && s.naud == 1, "a queued audition waits for seq_block");
    idle(&s, 1, bpm);
    CHECK(nev == 1 && is_on(&evs[0]) && (evs[0].status & 15) == 2 && evs[0].note == 40 && evs[0].vel == 100,
          "stopped: the next block plays it on the Bass channel at velocity 100");
    idle(&s, (int)(0.95 * beat), bpm);
    CHECK(off_of(0, 3, 40) < 0, "still held at 0.95 beat");
    idle(&s, (int)(0.1 * beat) + 1, bpm);
    CHECK(off_of(0, 3, 40) > 0 && balanced(), "ended after 1 beat, stopped");
    nev = 0;
    seq_audition_note(&s, LANE_BASS, 41);
    ev_seq = &s;
    for (i = 0; i < (int)(0.55 * 44100 / 128); i++) seq_block(&s, -1.0, 0.0, 0, 128);   /* no tempo from the host: 120 */
    ev_seq = NULL;
    CHECK(nev == 2 && balanced(), "with no host tempo it ends after 1 beat at 120 BPM");
    seq_audition_note(&s, LANE_BASS, 0);
    seq_audition_note(&s, LANE_BASS, 128);
    seq_audition_note(&s, 3, 60);
    CHECK(s.naud == 0, "a REST, a bad note and Melody 2 queue nothing");

    /* a chord: the slot's voicing under INVERSION, SPREAD and OCTAVE, as the Chords lane plays it */
    seq_set_vknob(&s, VK_SPR, 3);
    seq_set_chord_shape(&s, SHAPE_OCT, 1);
    seq_set_step(&s, LANE_CHORDS, 0, 0, 1);
    nev = 0;
    double ppq = play(&s, 0.0, 0.1, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    for (i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == 1) want[nw++] = evs[i].note;
    nev = 0;
    seq_set_mute(&s, LANE_CHORDS, 1);
    seq_audition_chord(&s, 0);
    idle(&s, 1, bpm);
    for (i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == 1 && evs[i].vel == 100) n[c < 4 ? c++ : 3] = evs[i].note;
    int same = nw == c && nw >= 3;
    for (i = 0; same && i < c; i++) same = n[i] == want[i];
    CHECK(same, "the slot's chord as the lane plays it (%d notes, %d played), on a muted lane", c, nw);
    idle(&s, (int)(1.1 * beat), bpm);
    CHECK(balanced(), "and it ends");
    seq_set_mute(&s, LANE_CHORDS, 0);
    seq_audition_chord(&s, s.part[0].prog_len);
    seq_audition_chord(&s, -1);
    CHECK(s.naud == 0, "a slot past the progression queues nothing");

    /* a full queue: 8 notes, the rest dropped; a chord that does not fit is dropped whole */
    nev = 0;
    for (i = 0; i < 10; i++) seq_audition_note(&s, LANE_MEL_UP, 60 + i);
    CHECK(s.naud == SEQ_MAX_AUDITION, "the queue holds 8");
    idle(&s, 1, bpm);
    CHECK(count_on(4) == 8, "8 play, 2 dropped (%d)", count_on(4));
    for (i = 0; i < 6; i++) seq_audition_note(&s, LANE_BASS, 30 + i);
    seq_audition_chord(&s, 0);
    CHECK(s.naud == 6, "a chord that does not fit the 2 left is dropped whole");
    seq_all_notes_off(&s);
    CHECK(s.naud == 0 && s.npending == 0, "ALL NOTES OFF empties the queue and ends the held auditions");
    idle(&s, 2, bpm);
    CHECK(count_on(3) == 0 && balanced(), "nothing queued plays after it");

    /* a retrigger ends the held note first */
    nev = 0;
    seq_audition_note(&s, LANE_BASS, 40);
    idle(&s, 10, bpm);
    seq_audition_note(&s, LANE_BASS, 40);
    idle(&s, 1, bpm);
    i = on_of(0, 3, 40), j = off_of(0, 3, 40);
    CHECK(i >= 0 && j > i && on_of(i + 1, 3, 40) > j && max_open(3) == 1, "the same note again ends the held one first");
    idle(&s, (int)(1.1 * beat), bpm);
    CHECK(balanced(), "and both end");

    /* a channel change: a held audition ends on the old channel, a queued one plays on the new */
    nev = 0;
    seq_audition_note(&s, LANE_BASS, 50);
    idle(&s, 1, bpm);
    seq_audition_note(&s, LANE_BASS, 51);
    seq_set_channel(&s, LANE_BASS, 9);
    CHECK(off_of(0, 3, 50) > 0, "the held audition ends on channel 3 at the change");
    idle(&s, 1, bpm);
    CHECK(on_of(0, 9, 51) >= 0, "the queued one plays on channel 9");
    idle(&s, (int)(1.1 * beat), bpm);
    CHECK(off_of(0, 9, 51) > 0 && balanced(), "and ends there");
    seq_set_channel(&s, LANE_BASS, 3);

    /* playing: offs by the song position */
    nev = 0;
    ppq = play(&s, 0.0, 1.0, 0);
    int from = nev;
    seq_audition_note(&s, LANE_BASS, 70);
    ppq = play(&s, ppq, 0.9, 0);
    i = on_of(from, 3, 70);
    CHECK(i >= 0 && evs[i].ppq <= 1.0 + blk() && off_of(i, 3, 70) < 0, "playing: it plays at the next block, held at 0.9 beat");
    ppq = play(&s, ppq, 0.2, 0);
    CHECK(off_of(i, 3, 70) > 0, "ended after 1 beat of song position");
    /* a stop ends a held audition and empties the queue */
    seq_audition_note(&s, LANE_BASS, 71);
    ppq = play(&s, ppq, 0.1, 0);
    seq_audition_note(&s, LANE_BASS, 72);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(on_of(from, 3, 72) < 0 && off_of(from, 3, 71) > 0 && s.naud == 0, "a stop ends the held audition and drops the queued one");
    /* stopped into playing: a held audition's note-off moves onto the song clock */
    nev = 0;
    seq_audition_note(&s, LANE_BASS, 73);
    idle(&s, (int)(0.5 * beat), bpm);
    ppq = play(&s, 100.0, 0.3, 0);
    CHECK(off_of(0, 3, 73) < 0, "started 0.5 beat in: still held");
    ppq = play(&s, ppq, 0.3, 0);
    i = off_of(0, 3, 73);
    CHECK(i > 0 && evs[i].ppq < 100.6 + blk(), "ends half a beat into playback (song %.3f)", i > 0 ? evs[i].ppq : -1.0);
    seq_audition_note(&s, LANE_BASS, 74);   /* across a loop wrap: ended there, as every held note */
    ppq = play(&s, ppq, 0.2, 0);
    play(&s, 0.0, 0.2, 0);
    CHECK(off_of(0, 3, 74) > 0, "a loop wrap ends a held audition");
    seq_block(&s, 0.4, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes, ever");
}

/* one setter with a move before it: the text clears */
typedef void (*setter_fn)(seq_t *s);
static void st_swing(seq_t *s) { seq_set_swing(s, 30); }
static void st_key(seq_t *s) { seq_set_key(s, 7); }
static void st_scale(seq_t *s) { seq_set_scale(s, SCALE_DORIAN); }
static void st_bars(seq_t *s) { seq_set_bars(s, 8); }
static void st_chord_len(seq_t *s) { seq_set_chord_len(s, 8); }
static void st_view(seq_t *s) { seq_set_view(s, 2); }
static void st_follow(seq_t *s) { seq_set_follow(s, !seq_follow(s)); }
static void st_mute(seq_t *s) { seq_set_mute(s, LANE_BASS, 1); }
static void st_chan(seq_t *s) { seq_set_channel(s, LANE_BASS, 9); }
static void st_step(seq_t *s) { seq_set_step(s, LANE_BASS, 0, 3, !s->part[s->cur_part].lane[LANE_BASS].steps[3].on); }
static void st_style(seq_t *s) { seq_set_style(s, LANE_BASS, GEN_BASS_WALK); }
static void st_cx(seq_t *s) { seq_set_complexity(s, LANE_BASS, 77); }
static void st_gen(seq_t *s) { seq_generate(s, LANE_BASS); }
static void st_gen_all(seq_t *s) { seq_generate_all(s); }
static void st_undo(seq_t *s) { seq_undo(s); }
static void st_harmony(seq_t *s) { seq_set_harmony(s, 90); }
static void st_new_prog(seq_t *s) { seq_generate_progression(s); }
static void st_slot(seq_t *s) { seq_set_slot(s, 0, 3, 0, COL_7TH); }
static void st_slot_lock(seq_t *s) { seq_toggle_slot_lock(s, 1); }
static void st_shape(seq_t *s) { seq_set_lane_shape(s, LANE_BASS, BSHAPE_SYNC, 60); }
static void st_note(seq_t *s) { seq_set_step_note(s, LANE_BASS, 0, 5, 33); }
static void st_lock(seq_t *s) { seq_toggle_step_lock(s, LANE_BASS, 0, 6); }
static void st_part(seq_t *s) { seq_select_part(s, 3); }
static void st_mel(seq_t *s) { seq_mjam_set(s, MJS_FORM, MF_2_2); }   /* 0.20: the MELODY tab's (a JAM lane's) */
static void st_names(seq_t *s) { seq_set_names(s, 1); }
static void st_panic(seq_t *s) { seq_all_notes_off(s); }
static void st_voicing(seq_t *s) { seq_set_voicing(s, VOICING_CLOSE); }
static void st_preset(seq_t *s) { seq_set_prog_preset(s, 3); }

/* Decision 7: each move describes what it did; any other setter that changes something, or 4 beats of playback,
 * clears it */

/* 0.13 NEW RHYTHM: the rhythm (on, len) of the chords lane's loop */
static int same_rhythm(const seq_t *s, const seq_step_t *was) {
    const seq_part_t *p = &s->part[s->cur_part];
    for (int i = 0; i < p->bars * SEQ_STEPS; i++)
        if (p->lane[LANE_CHORDS].steps[i].on != was[i].on || (was[i].on && p->lane[LANE_CHORDS].steps[i].len != was[i].len))
            return 0;
    return 1;
}

static const int NR_STYLES[9] = {GEN_CH_SUSTAIN, GEN_CH_PULSE, GEN_CH_STABS, GEN_CH_OFFBEAT, GEN_CH_CHARLESTON,
                                 GEN_CH_BALLAD, GEN_CH_FUNK, GEN_CH_HALFTIME, GEN_CH_PUSH};

/* A Sustain at COMPLEXITY 0 that no seed changes (0.12's "same at this complexity"): NEW RHYTHM moves to the next
 * rhythm, Ballad, says so, and keeps one undo step */
static void test_12_new_rhythm_same(void) {
    static seq_t s;
    static seq_step_t was[SEQ_LANE_STEPS];
    char t[64];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_SUSTAIN);
    seq_set_complexity(&s, LANE_CHORDS, 0);
    memcpy(was, s.part[0].lane[LANE_CHORDS].steps, sizeof was);
    int n = s.undo_n;
    seq_generate(&s, LANE_CHORDS);
    seq_last_move_text(&s, t, sizeof t);
    CHECK(s.undo_n == n + 1 && !strcmp(t, "NEW RHYTHM: Ballad") && seq_last_move_lane(&s) == LANE_CHORDS &&
              s.part[0].lane[LANE_CHORDS].gen.style == GEN_CH_BALLAD && !same_rhythm(&s, was),
          "NEW RHYTHM on a Sustain no seed changes moves to Ballad, one undo step ('%s')", t);
}

/* Every rhythm x COMPLEXITY 0/30/70/100 x chord lengths: each of 3 presses in a row changes the rhythm, pushes one undo
 * step and says what it did, on the Chords lane */
static void test_13_new_rhythm_changes(void) {
    static seq_t s;
    static seq_step_t was[SEQ_LANE_STEPS];
    static const int CX[4] = {0, 30, 70, 100}, CL[4] = {4, 8, 16, 32};
    int bad = 0, cases = 0, walked = 0, capped = 0;
    char t[64], why[96] = "";
    for (int k = 0; k < 9; k++)
        for (int c = 0; c < 4; c++)
            for (int l = 0; l < 4; l++) {
                seq_init(&s, emit, NULL);
                old_chords(&s);
                seq_set_chord_len(&s, CL[l]);
                seq_set_style(&s, LANE_CHORDS, NR_STYLES[k]);
                seq_set_complexity(&s, LANE_CHORDS, CX[c]);
                for (int press = 0; press < 3; press++) {
                    const seq_lane_t *ln = &s.part[0].lane[LANE_CHORDS];
                    int n = s.undo_n, style = ln->gen.style;
                    char want[48];
                    memcpy(was, ln->steps, sizeof was);
                    seq_generate(&s, LANE_CHORDS);
                    seq_last_move_text(&s, t, sizeof t);
                    snprintf(want, sizeof want, "NEW RHYTHM: %s", seq_rhythm_name(ln->gen.style));
                    int ok = !same_rhythm(&s, was) && (s.undo_n == n + 1 || (n == SEQ_UNDO && s.undo_n == n)) &&
                             seq_last_move_lane(&s) == LANE_CHORDS &&
                             !strcmp(t, ln->gen.style == style ? "NEW RHYTHM: new variation" : want);
                    walked += ln->gen.style != style;
                    capped += !strcmp(t, "NEW RHYTHM: nothing new here");
                    cases++;
                    if (!ok && !bad++)
                        snprintf(why, sizeof why, "%s c%d cl%d press %d: '%s'", seq_rhythm_name(NR_STYLES[k]), CX[c], CL[l], press, t);
                }
            }
    CHECK(!bad && !capped, "NEW RHYTHM changes the rhythm on every press: 9 rhythms x c 0/30/70/100 x 4 chord lengths x 3 "
                           "(%d of %d bad%s%s; %d moved to the next rhythm; %d reached the 12-regeneration cap)",
          bad, cases, bad ? ", first " : "", why, walked, capped);
}

/* Manual: nothing changes, no undo step, "pick a rhythm first" */
static void test_13_new_rhythm_manual(void) {
    static seq_t s;
    static seq_step_t was[SEQ_LANE_STEPS];
    char t[64];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_MANUAL);
    seq_set_step(&s, LANE_CHORDS, 0, 3, 1);
    memcpy(was, s.part[0].lane[LANE_CHORDS].steps, sizeof was);
    int n = s.undo_n, seed = s.part[0].lane[LANE_CHORDS].gen.seed;
    seq_generate(&s, LANE_CHORDS);
    seq_last_move_text(&s, t, sizeof t);
    CHECK(s.undo_n == n && !memcmp(was, s.part[0].lane[LANE_CHORDS].steps, sizeof was) &&
              s.part[0].lane[LANE_CHORDS].gen.style == GEN_MANUAL && s.part[0].lane[LANE_CHORDS].gen.seed == seed &&
              !strcmp(t, "NEW RHYTHM: pick a rhythm first") && seq_last_move_lane(&s) == LANE_CHORDS,
          "NEW RHYTHM on Manual changes nothing, pushes no undo, says pick a rhythm first ('%s')", t);
}

/* The family walk: from each rhythm where no seed changes the rhythm, NEW RHYTHM moves to the next in
 * Sustain -> Ballad -> Half-time -> Pulse -> Offbeat -> Charleston -> Push -> Funk -> Stabs -> Sustain (or further when
 * the next one changes nothing either), and names it */
static void test_13_new_rhythm_walk(void) {
    static seq_t s;
    static const int ORDER[9] = {GEN_CH_SUSTAIN, GEN_CH_BALLAD, GEN_CH_HALFTIME, GEN_CH_PULSE, GEN_CH_OFFBEAT,
                                 GEN_CH_CHARLESTON, GEN_CH_PUSH, GEN_CH_FUNK, GEN_CH_STABS};
    static const int CL[4] = {4, 8, 16, 32};
    int bad = 0, seen = 0, from[9] = {0};
    char t[64], why[96] = "";
    for (int k = 0; k < 9; k++)
        for (int l = 0; l < 4; l++) {
            seq_init(&s, emit, NULL);
            old_chords(&s);
            seq_set_chord_len(&s, CL[l]);
            seq_set_style(&s, LANE_CHORDS, ORDER[k]);
            seq_set_complexity(&s, LANE_CHORDS, 0);
            seq_generate(&s, LANE_CHORDS);
            int got = s.part[0].lane[LANE_CHORDS].gen.style;
            if (got == ORDER[k]) continue;   /* a new variation */
            seq_last_move_text(&s, t, sizeof t);
            int at = 1;   /* the first rhythm after this one in the order that changes something: at least the next */
            while (at < 9 && ORDER[(k + at) % 9] != got) at++;
            char want[48];
            snprintf(want, sizeof want, "NEW RHYTHM: %s", seq_rhythm_name(got));
            seen++, from[k] = 1;
            if (at == 9 || strcmp(t, want)) {
                if (!bad++) snprintf(why, sizeof why, "%s cl%d -> %s '%s'", seq_rhythm_name(ORDER[k]), CL[l], seq_rhythm_name(got), t);
            }
        }
    CHECK(!bad && seen && from[0], "the family walk goes to the next rhythm in the order and names it (%d walks%s%s)", seen,
          bad ? ", first bad " : "", why);
    /* Sustain at c 0, 1-bar chords: Ballad, then (Ballad changes with a new seed) a new variation */
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_SUSTAIN);
    seq_set_complexity(&s, LANE_CHORDS, 0);
    seq_generate(&s, LANE_CHORDS);
    seq_last_move_text(&s, t, sizeof t);
    int ok = !strcmp(t, "NEW RHYTHM: Ballad");
    seq_generate(&s, LANE_CHORDS);
    seq_last_move_text(&s, t, sizeof t);
    CHECK(ok && !strcmp(t, "NEW RHYTHM: new variation") && s.part[0].lane[LANE_CHORDS].gen.style == GEN_CH_BALLAD,
          "Sustain c 0 -> 'NEW RHYTHM: Ballad', then a new variation of Ballad ('%s')", t);
    int order = 1;
    for (int k = 0; k < 9; k++) order &= seq_next_rhythm(ORDER[k]) == ORDER[(k + 1) % 9];
    CHECK(order && seq_next_rhythm(GEN_MANUAL) == GEN_MANUAL && seq_next_rhythm(GEN_BASS_WALK) == GEN_BASS_WALK,
          "seq_next_rhythm: Sustain Ballad Half-time Pulse Offbeat Charleston Push Funk Stabs, then Sustain");
}

/* Every step locked: nothing regenerates, no undo step, "all hits locked" */
static void test_13_new_rhythm_locked(void) {
    static seq_t s;
    static seq_step_t was[SEQ_LANE_STEPS];
    char t[64];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_FUNK);
    seq_set_complexity(&s, LANE_CHORDS, 50);
    for (int i = 0; i < s.part[0].bars * SEQ_STEPS; i++) seq_toggle_step_lock(&s, LANE_CHORDS, i / SEQ_STEPS, i % SEQ_STEPS);
    const seq_lane_t *ln = &s.part[0].lane[LANE_CHORDS];
    int n = s.undo_n, seed = ln->gen.seed;
    memcpy(was, ln->steps, sizeof was);
    seq_generate(&s, LANE_CHORDS);
    seq_last_move_text(&s, t, sizeof t);
    CHECK(s.undo_n == n && ln->gen.style == GEN_CH_FUNK && ln->gen.seed == seed && !memcmp(was, ln->steps, sizeof was) &&
              !strcmp(t, "NEW RHYTHM: all hits locked"),
          "NEW RHYTHM with every step locked: unchanged, no undo step, says all hits locked ('%s')", t);
}

/* The chords lane's steps as gen_lane_v1 makes them at an explicit variant */
static void chords_at(const seq_t *s, int variant, seq_step_t *out) {
    const seq_part_t *p = &s->part[0];
    const seq_gen_t *g = &p->lane[LANE_CHORDS].gen;
    gen_params_t gp = {g->style, g->seed, g->complexity, p->bars, p->chord_len, p->prog, p->prog_len, s->key, s->scale,
                       NULL, LANE_CHORDS, variant, NULL};
    gen_lane_v1(&gp, out);
}

static int first_seed(int style, int v) {
    int seed = 1;
    while (gen_chords_v2_variant(style, seed) != v) seed++;
    return seed;
}

/* A literal 0.12 state (no .cv) with each 0.12 rhythm at a seed whose hash picks variant 1 or 2: it plays variant 0,
 * 0.12's template, exactly */
static void test_13_variant_012_load(void) {
    static seq_t s;
    static seq_step_t v0[SEQ_LANE_STEPS], vn[SEQ_LANE_STEPS];
    char str[160];
    int bad = 0, differ = 0;
    for (int style = GEN_CH2_FIRST; style <= GEN_CH2_LAST; style++)
        for (int v = 1; v <= 2; v++) {
            snprintf(str, sizeof str, "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:|A1.g=%d,%d,0|A2=0:|A3=0:|A4=0:", style,
                     first_seed(style, v));
            seq_init(&s, emit, NULL);
            if (!seq_state_load(&s, str)) {
                bad++;
                continue;
            }
            s.part[0].lane[LANE_CHORDS].gen = (seq_gen_t){style, first_seed(style, v), 0, 0};   /* 0.18: the saved recipe */
            chords_at(&s, 0, v0);
            chords_at(&s, v, vn);
            int n = 4 * SEQ_STEPS * (int)sizeof(seq_step_t), same = 1;
            for (int b = 0; b < 4; b++) same &= bar_sounds_as(&s.part[0].lane[LANE_CHORDS].steps[b * SEQ_STEPS], &v0[b * SEQ_STEPS]);
            bad += !same;   /* 0.18: converted to DRAWN bars that sound as variant 0 */
            differ += memcmp(v0, vn, (size_t)n) != 0;
        }
    CHECK(!bad && differ == 12, "a 0.12 state at seeds that pick variant 1 or 2 loads as variant 0, 0.12's template "
                                "(%d bad; %d of 12 would differ)", bad, differ);
}

/* `.cv` round trip: written only when not 0, loaded back with the same steps; a bad one loads as 0, never refused */
static void test_13_variant_save(void) {
    static seq_t s, t;
    static char buf[8192];
    char want[16];
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_FUNK);
    seq_state_save(&s, buf, sizeof buf);
    int none = !strstr(buf, ".cv=");
    for (int i = 0; i < 20 && s.part[0].lane[LANE_CHORDS].gen.variant == 0; i++) seq_generate(&s, LANE_CHORDS);
    int v = s.part[0].lane[LANE_CHORDS].gen.variant;
    seq_state_save(&s, buf, sizeof buf);
    snprintf(want, sizeof want, "|A1.cv=%d", v);
    seq_init(&t, emit, NULL);
    int ok = v && strstr(buf, want) && seq_state_load(&t, buf) &&
             chords_loaded_as(&s.part[0].lane[LANE_CHORDS], &t.part[0].lane[LANE_CHORDS], seq_bars(&s)) == 1;
    CHECK(none && ok, "a variant-0 lane saves no .cv; variant %d saves %s and loads back (0.18: DRAWN bars that sound the same)", v, want);
    int bad_ok = 1;
    static const char *const BAD[3] = {"|A1.cv=7", "|A1.cv=", "|A1.cv=12"};
    for (int k = 0; k < 3; k++) {
        snprintf(buf, sizeof buf, "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:|A1.g=%d,5,0%s|A2=0:|A3=0:|A4=0:", GEN_CH_FUNK,
                 BAD[k]);
        seq_init(&t, emit, NULL);
        bad_ok &= seq_state_load(&t, buf) && t.part[0].lane[LANE_CHORDS].gen.variant == 0;
    }
    CHECK(bad_ok, "a bad .cv loads as variant 0, never refused");
}

/* NEW RHYTHM on a 0.12 rhythm at c 0 (no ghosts, velocities aside): a new variation is another of its templates, at
 * the same seed (its other variants are tried first), stored with the lane */
static void test_13_new_rhythm_variant(void) {
    static seq_t s;
    static seq_step_t want[SEQ_LANE_STEPS];
    char t[64];
    int ok = 1, moved = 0;
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_OFFBEAT);
    seq_set_complexity(&s, LANE_CHORDS, 0);
    for (int press = 0; press < 6; press++) {
        const seq_gen_t *g = &s.part[0].lane[LANE_CHORDS].gen;
        int v = g->variant, seed = g->seed;
        seq_generate(&s, LANE_CHORDS);
        seq_last_move_text(&s, t, sizeof t);
        chords_at(&s, g->variant, want);
        ok &= g->style == GEN_CH_OFFBEAT && !strcmp(t, "NEW RHYTHM: new variation") && g->variant != v && g->seed == seed &&
              !memcmp(want, s.part[0].lane[LANE_CHORDS].steps, sizeof(seq_step_t) * 4 * SEQ_STEPS);
        moved++;
    }
    CHECK(ok && moved == 6, "NEW RHYTHM on Offbeat c 0: each press another variant at the same seed, stored with the lane");
}

/* One UNDO takes back the whole press: style, seed and steps, a variation or a move to the next rhythm */
static void test_13_new_rhythm_undo(void) {
    static seq_t s;
    static seq_step_t was[SEQ_LANE_STEPS];
    static const int ST[3] = {GEN_CH_SUSTAIN, GEN_CH_FUNK, GEN_CH_OFFBEAT}, CX[3] = {0, 100, 0};   /* Offbeat c 0: a new variant */
    for (int k = 0; k < 3; k++) {
        seq_init(&s, emit, NULL);
        old_chords(&s);
        seq_set_style(&s, LANE_CHORDS, ST[k]);
        seq_set_complexity(&s, LANE_CHORDS, CX[k]);
        const seq_lane_t *ln = &s.part[0].lane[LANE_CHORDS];
        int seed = ln->gen.seed, variant = ln->gen.variant, n = s.undo_n;
        memcpy(was, ln->steps, sizeof was);
        seq_generate(&s, LANE_CHORDS);
        int moved = ln->gen.style != ST[k], reseeded = ln->gen.seed != seed || ln->gen.variant != variant;
        seq_undo(&s);
        CHECK(ln->gen.style == ST[k] && ln->gen.seed == seed && ln->gen.variant == variant &&
                  !memcmp(was, ln->steps, sizeof was) && s.undo_n == n &&
                  (k ? reseeded && !moved : moved),
              "%s c%d: one UNDO restores the style, seed, variant and steps of a NEW RHYTHM (%s)", seq_rhythm_name(ST[k]), CX[k],
              moved ? "moved to the next rhythm" : "a new variation: another variant or seed");
    }
}

/* The move text (0.12; NEW RHYTHM's since IDEAS's moves left): 4 seconds of playback clear it, stopped it stays, and any other setter
 * that changes something clears it */
static void test_12_move_text(void) {
    static seq_t s;
    char t[64];
    static const setter_fn SETTERS[] = {st_swing, st_key, st_scale, st_bars, st_chord_len, st_view, st_follow, st_mute,
        st_chan, st_step, st_style, st_cx, st_gen, st_gen_all, st_undo, st_harmony, st_new_prog, st_slot, st_slot_lock,
        st_shape, st_note, st_lock, st_part, st_mel, st_names, st_panic, st_voicing, st_preset};
    int n = (int)(sizeof SETTERS / sizeof SETTERS[0]), cleared = 0, fail_at = -1;
    seq_init(&s, emit, NULL);
    CHECK(!seq_names(&s) && seq_last_move_text(&s, t, sizeof t) == 0 && !t[0], "no text and degrees on a new instance");

    /* it clears: 4 seconds of playback whatever the tempo (3.9 s keeps it); stopped it stays */
    old_chords(&s);
    seq_generate(&s, LANE_CHORDS);
    nev = 0;
    double ppq = play(&s, 0.0, 3.9 * bpm / 60.0, 0);
    CHECK(seq_last_move_text(&s, t, sizeof t) > 0, "kept for 3.9 s of playback");
    ppq = play(&s, ppq, 0.2 * bpm / 60.0, 0);
    CHECK(seq_last_move_text(&s, t, sizeof t) == 0, "cleared after 4 s");
    seq_block(&s, ppq, bpm, 0, 128);
    seq_generate(&s, LANE_CHORDS);
    idle(&s, 3000, bpm);
    CHECK(seq_last_move_text(&s, t, sizeof t) > 0, "stopped, it stays");
    seq_set_swing(&s, 0);   /* a re-send */
    seq_set_view(&s, 0);
    seq_set_key(&s, seq_key(&s));
    seq_set_mute(&s, LANE_BASS, 0);
    seq_set_names(&s, 0);
    seq_audition_chord(&s, 0);
    CHECK(seq_last_move_text(&s, t, sizeof t) > 0, "re-sends that change nothing, and an audition, keep it");
    for (int i = 0; i < n; i++) {
        seq_init(&s, emit, NULL);
        old_bass(&s, 30);
        seq_generate(&s, LANE_BASS);   /* something to undo */
        old_chords(&s);
        seq_generate(&s, LANE_CHORDS);
        SETTERS[i](&s);
        if (seq_last_move_text(&s, t, sizeof t) == 0) cleared++;
        else if (fail_at < 0) fail_at = i;
    }
    CHECK(cleared == n, "every other setter clears it (%d of %d; first left: #%d)", cleared, n, fail_at);
    seq_init(&s, emit, NULL);
    old_chords(&s);
    seq_generate(&s, LANE_CHORDS);
    seq_last_move_text(&s, t, 4);
    CHECK(!strcmp(t, "NEW"), "cut to the buffer");
}

/* 0.14: a chord for every slot of the loop (bars x 16 / chord length, at least 1, up to 64). Presets, NEW PROGRESSION and
 * ideas fill it by repeating a phrase; a chord-length change remaps it by time; a loop-length change grows and hides it
 * as bars do; a loop that repeats with a period of 16 or less saves as 0.13 did, else with `.ch` too. */

/* The 0.13 core's play hash (FNV-1a over each event's status, note, velocity and 16th): the expected values below are a
 * 0.13 build's (git show feat/0.13-feedback2:src/... into a scratch dir), playing the same state from song position 0 */
static unsigned long long ev_hash(void) {
    unsigned long long h = 1469598103934665603ull;
    for (int i = 0; i < nev; i++) {
        unsigned v[4] = {(unsigned)evs[i].status, (unsigned)evs[i].note, (unsigned)evs[i].vel, (unsigned)evs[i].k};
        for (int j = 0; j < 4; j++) h = (h ^ v[j]) * 1099511628211ull;
    }
    return h;
}

/* A field's value in a state string (name with its part letter, "A.prog"), into out; its length, -1 when absent */
static int st_field(const char *st, const char *name, char *out, int len) {
    char key[24];
    const char *p, *e;
    snprintf(key, sizeof key, "|%s=", name);
    if (!(p = strstr(st, key))) return -1;
    p += strlen(key);
    e = strchr(p, '|');
    int n = e ? (int)(e - p) : (int)strlen(p);
    snprintf(out, (size_t)len, "%.*s", n, p);
    return n;
}

static int slot_is(const seq_t *s, int i, int deg, int src, int col, int lock) {
    const seq_part_t *p = &s->part[s->cur_part];
    return p->prog[i] == deg && p->src[i] == src && p->colour[i] == col && (int)(p->slot_lock >> i & 1) == lock;
}

/* The playing part's slots, as the remap and undo tests compare them: every slot (hidden ones too) and the counts */
static int same_slots(const seq_part_t *a, const seq_part_t *b) {
    return !memcmp(a->prog, b->prog, sizeof a->prog) && !memcmp(a->src, b->src, sizeof a->src) &&
           !memcmp(a->colour, b->colour, sizeof a->colour) && a->slot_lock == b->slot_lock && a->prog_len == b->prog_len &&
           a->chord_len == b->chord_len && a->filled_slots == b->filled_slots && a->phrase == b->phrase && a->bars == b->bars;
}

static void test_14_slot_count(void) {
    static const int BARS[5] = {1, 2, 4, 8, 16}, CL[4] = {32, 16, 8, 4};
    static seq_t s;
    int ok = 1;
    seq_init(&s, emit, NULL);
    CHECK(seq_slots(&s) == 4 && seq_prog_len(&s) == 4, "a new instance: 4 slots (4 bars of 1-bar chords)");
    for (int b = 0; b < 5; b++)
        for (int c = 0; c < 4; c++) {
            int want = BARS[b] * SEQ_STEPS / CL[c];
            seq_set_bars(&s, BARS[b]);
            seq_set_chord_len(&s, CL[c]);
            want = want < 1 ? 1 : want;
            if (seq_slots(&s) != want || seq_prog_len(&s) != want) {
                printf("     %d bars, chord length %d: %d slots, prog_len %d, want %d\n", BARS[b], CL[c], seq_slots(&s), seq_prog_len(&s), want);
                ok = 0;
            }
        }
    CHECK(ok, "slots = max(1, bars x 16 / chord length) = prog_len, for every length (2-bar chords on 1 bar: 1)");
    CHECK(SEQ_MAX_PROG == 64 && seq_slots(&s) == 64, "16 bars of 1/4-bar chords: 64 slots, SEQ_MAX_PROG");
    seq_set_prog_preset(&s, 15);
    CHECK(seq_prog_len(&s) == 64, "a preset fills the loop: prog_len stays the slot count");
    seq_generate_progression(&s);
    CHECK(seq_prog_len(&s) == 64, "so does NEW PROGRESSION");
    seq_undo(&s);
    CHECK(seq_prog_len(&s) == 64, "and an undo");
}

/* Review Focus 1: literal 0.13 saves load as the whole loop, play as 0.13 plays them and save again byte for byte */
typedef struct { const char *name, *st; int slots; unsigned long long hash; int nev; } old14_t;
static const old14_t OLD14[] = {
    {"4 chords, 16 bars of 1-bar chords",
     "csq2|gv=1|k=4|s=1|A.len=16|A.cl=16|A.h=30,1|A.prog=0526|A.voi=1|ch=1,2,3|A1=0:|A1.g=2,1,30|A2=0:|A2.g=4,1,30|A3=0:"
     "|A3.g=7,1,30|A4=0:", 16, 0x1d1292d63151a70bull, 704},
    {"4 chords with a borrow, colours, locks and COLOUR 55",
     "csq2|gv=1|k=4|s=1|A.len=16|A.cl=16|A.h=30,1|A.prog=0424|A.voi=1|A.pq=00,21,00,03|A.sl=a|A.cs=55,50,0,50,0,50,0|ch=1,2,3"
     "|A1=0:|A1.g=2,1,30|A2=0:|A2.g=4,1,30|A3=0:|A3.g=7,1,30|A4=0:", 16, 0x92759de95f438a4bull, 784},
    {"Canon (8), 16 bars of 1/4-bar chords",
     "csq2|gv=1|k=4|s=1|A.len=16|A.cl=4|A.h=30,1|A.prog=04523034|A.voi=1|ch=1,2,3|A1=0:|A1.g=2,1,30|A2=0:|A2.g=4,1,30|A3=0:"
     "|A3.g=7,1,30|A4=0:", 64, 0x3774a94e1722756bull, 640},
    {"Canon (8) at 1/4 bar with an add9, locks and COLOUR 80",
     "csq2|gv=1|k=4|s=1|A.len=16|A.cl=4|A.h=30,1|A.prog=04523014|A.voi=1|A.pq=00,00,00,00,00,00,04,00|A.sl=44"
     "|A.cs=80,50,0,50,0,50,0|ch=1,2,3|A1=0:|A1.g=2,1,30|A2=0:|A2.g=4,1,30|A3=0:|A3.g=7,1,30|A4=0:", 64, 0x7e89f9d8da23b80full, 752},
    {"Canon (8) on a 4-bar loop (the second half never reached)",
     "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=04523034|A.voi=1|ch=1,2,3|A1=0:|A1.g=2,1,30|A2=0:|A2.g=4,1,30|A3=0:"
     "|A3.g=7,1,30|A4=0:", 4, 0x9d3da92f457c8095ull, 176},
    {"12 chords over 32 slots (a period that does not divide the loop), a recipe melody, COLOUR 60",
     "csq2|gv=1|k=9|s=0|A.len=16|A.cl=8|A.h=30,7|A.prog=012345601234|A.voi=1|A.cs=60,50,0,50,0,50,0|ch=1,2,3|A1=0:"
     "|A1.g=2,1,30|A2=0:|A2.g=4,1,30|A3=0:|A3.g=7,1,40|A3.mr=1,2,0,0,0,25,15,40,20,20,50,50,0,0,0|A4=0:", 32,
     0x245345132124989cull, 896},
};

/* 0.18: the logged events are `was`'s but for the velocity of the Chords channel's note-ons (ruling R2's classes) */
static int same_but_chord_vel(const ev_t *was, int n, int chan) {
    if (nev != n) return 0;
    for (int i = 0; i < n; i++) {
        const ev_t *a = &was[i], *b = &evs[i];
        int chord_on = (a->status & 0xF0) == 0x90 && (a->status & 15) == chan - 1 && a->vel;
        if (a->status != b->status || a->note != b->note || a->k != b->k || fabs(a->ppq - b->ppq) > 1e-9 || (!chord_on && a->vel != b->vel) ||
            (chord_on && !b->vel))
            return 0;
    }
    return 1;
}

static void test_14_old_states(void) {
    static seq_t t, r, r13;
    static ev_t was[MAXEV];
    char buf[SEQ_STATE_MAX], prog[24], pq[64], sl[8];
    int n;
    for (int k = 0; k < (int)(sizeof OLD14 / sizeof OLD14[0]); k++) {
        const old14_t *o = &OLD14[k];
        int L = st_field(o->st, "A.prog", prog, sizeof prog), npq = st_field(o->st, "A.pq", pq, sizeof pq), ok = 1;
        unsigned lk = st_field(o->st, "A.sl", sl, sizeof sl) > 0 ? (unsigned)strtoul(sl, NULL, 16) : 0;
        seq_init(&t, emit, NULL);
        CHECK(seq_state_load(&t, o->st) && seq_prog_len(&t) == o->slots && seq_slots(&t) == o->slots, "0.13 %s: loads as %d slots",
              o->name, o->slots);
        for (int i = 0; i < o->slots; i++) {
            int j = i % L, src = npq > 0 ? pq[3 * j] - '0' : 0, col = npq > 0 ? pq[3 * j + 1] - '0' : 0;
            ok &= slot_is(&t, i, prog[j] - '0', src, col, (int)(lk >> j & 1));
        }
        CHECK(ok, "  every slot is the phrase's chord there (degree, borrow, colour, lock)");
        CHECK(seq_state_save(&t, buf, sizeof buf) > 0 && saves_but_drawn(&t, o->st), "  re-saves byte for byte (0.18: but the Chords lane)%s%s",
              strcmp(buf, o->st) ? ": " : "", strcmp(buf, o->st) ? buf : "");
        r = t;
        old_chords(&r);   /* 0.18: the lane as 0.13-0.17 loaded it (A1.g=2,1,30 is old_chords' Pulse c30, seed 1) */
        r13 = r;
        old_bass(&r13, 30);   /* 0.19: and the Bass as 0.13-0.18 loaded it (A2.g=4,1,30 is old_bass's Root c30, seed 1) */
        nev = 0;
        play(&r, 0.0, seq_bars(&r) * 4.0, 0);
        n = nev;
        memcpy(was, evs, sizeof(ev_t) * (size_t)n);
        nev = 0;
        play(&t, 0.0, seq_bars(&t) * 4.0, 0);
        CHECK(same_but_chord_vel(was, n, t.chan[LANE_CHORDS]), "  the converted Chords bars play the same notes at the same times, velocities classed (R2)");
        if (strstr(o->st, "|A.cs="))   /* retired in 0.17: the saved COLOUR knob (0.9's rank rule) no longer sounds, so these play differently */
            continue;
        nev = 0;
        play(&r13, 0.0, seq_bars(&r13) * 4.0, 0);
        CHECK(ev_hash() == o->hash && nev == o->nev, "  plays as 0.13 does with 0.13's Chords and Bass steps (%d events, hash %016llx)", nev, ev_hash());
    }
}

/* Fix round 1: a 0.13 lock on a phrase slot is on its repeats too (0.24.1 COLOUR's rank-rule count and steps, test-only, are gone) */
static void test_14_locked_try(void) {
    static seq_t s;
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=%d|A.cl=16|A.h=30,1|A.prog=0526|A.voi=1|A.cs=55,50,0,50,0,50,0%s|ch=1,2,3|A1=0:|A1.g=2,1,30|A2=0:|A2.g=4,1,30|A3=0:|A3.g=7,1,30|A4=0:";
    char st[600];
    snprintf(st, sizeof st, HEAD, 8, "|A.sl=2");
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, st) && seq_slots(&s) == 8 && seq_slot_locked(&s, 1) && seq_slot_locked(&s, 5), "0.13 lock on a phrase slot: slots 1 and 5");
}

/* Review Focus 2: an edit in bar 8 changes bar 8 only; it saves the first 16 slots in the legacy fields (all a 16-slot
 * loop has), and a longer loop adds `.ch`. Without `.ch` (what a 0.13 build reads), the first 16 slots repeat. */
static void test_14_per_bar_edit(void) {
    static seq_t s, t;
    static char bad[SEQ_STATE_MAX + 16];
    char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX], f[300], *ch;
    int ok = 1;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_prog_preset(&s, 0);   /* 1-6-3-7 Epic: 0526 */
    seq_set_slot(&s, 7, 3, 0, COL_TRIAD);   /* bar 8: iv */
    for (int i = 0; i < 16; i++) ok &= slot_is(&s, i, i == 7 ? 3 : "0526"[i % 4] - '0', 0, 0, i == 7);
    CHECK(ok, "16 bars of 1-bar chords: the edit in bar 8 changes bar 8 only (bars 4, 12, 16 keep VII)");
    CHECK(seq_part_step_note(&s, 0, LANE_CHORDS, 7, 0) != seq_part_step_note(&s, 0, LANE_CHORDS, 3, 0) &&
          seq_part_step_note(&s, 0, LANE_CHORDS, 11, 0) == seq_part_step_note(&s, 0, LANE_CHORDS, 3, 0), "and plays it in bar 8 only");
    seq_state_save(&s, buf, sizeof buf);
    CHECK(st_field(buf, "A.prog", f, sizeof f) == 16 && !strcmp(f, "0526052305260526") && st_field(buf, "A.sl", f, sizeof f) > 0 &&
          !strcmp(f, "80") && !strstr(buf, "|A.ch="), "16 slots: the legacy fields hold the whole loop (no .ch)");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && same_slots(&t.part[0], &s.part[0]) && seq_state_save(&t, again, sizeof again) > 0 &&
          !strcmp(again, buf), "and loads back exactly, re-saving byte for byte");
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_chord_len(&s, 4);   /* 64 slots */
    seq_set_prog_preset(&s, 0);
    seq_set_slot(&s, 28, 3, 0, COL_TRIAD);   /* bar 8 beat 1 */
    ok = 1;
    for (int i = 0; i < 64; i++) ok &= slot_is(&s, i, i == 28 ? 3 : "0526"[i % 4] - '0', 0, 0, i == 28);
    CHECK(ok, "64 slots: the edit in bar 8 beat 1 changes that slot only");
    seq_state_save(&s, buf, sizeof buf);
    CHECK(st_field(buf, "A.prog", f, sizeof f) == 16 && !strcmp(f, "0526052605260526") && st_field(buf, "A.sl", f, sizeof f) < 0,
          "the legacy fields hold the first 16 slots (its .sl from them: none locked)");
    ok = st_field(buf, "A.ch", f, sizeof f) == 256;
    for (int i = 0; ok && i < 64; i++) {
        char want[5];
        snprintf(want, sizeof want, "%c00%c", i == 28 ? '3' : "0526"[i % 4], i == 28 ? '1' : '0');
        ok &= !strncmp(f + 4 * i, want, 4);
    }
    CHECK(ok, "and .ch every slot as <degree><src><colour><lock>");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && same_slots(&t.part[0], &s.part[0]) && seq_state_save(&t, again, sizeof again) > 0 &&
          !strcmp(again, buf), "the long form loads back exactly and re-saves byte for byte");
    ch = strstr(buf, "|A.ch=");
    memmove(ch, ch + 6 + 256, strlen(ch + 6 + 256) + 1);   /* what a 0.13 build reads */
    seq_init(&t, emit, NULL);
    ok = seq_state_load(&t, buf) && seq_prog_len(&t) == 64;
    for (int i = 0; ok && i < 64; i++) ok &= slot_is(&t, i, "0526"[i % 4] - '0', 0, 0, 0);
    CHECK(ok, "without .ch the legacy fields load: the first 16 slots repeat");
    seq_init(&t, emit, NULL);
    snprintf(again, sizeof again, "%s", buf);
    ch = strstr(again, "|A.prog=");
    memcpy(ch + 8, "0526052305260526", 16);   /* a bad .ch (wrong length) is ignored: the legacy phrase unrolls */
    snprintf(bad, sizeof bad, "%s|A.ch=3001", again);
    CHECK(seq_state_load(&t, bad) && slot_is(&t, 7, 3, 0, 0, 0) && slot_is(&t, 23, 3, 0, 0, 0) && slot_is(&t, 28, 0, 0, 0, 0),
          "a .ch of the wrong length is ignored (the legacy phrase unrolls), never refused");
}

/* Review Focus 3: a chord-length change remaps the slots by time (locks and colours move with their chord), one undo step */
static void test_14_remap(void) {
    static seq_t s;
    static seq_part_t before;
    int ok = 1, n0;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_prog_preset(&s, 0);
    seq_set_slot(&s, 5, 1, 0, COL_7TH);
    seq_set_slot(&s, 6, 0, 2, COL_TRIAD);   /* a borrow */
    before = s.part[0];
    n0 = s.undo_n;
    seq_set_chord_len(&s, 8);
    for (int i = 0; i < 32; i++)
        ok &= slot_is(&s, i, before.prog[i / 2], before.src[i / 2], before.colour[i / 2], (int)(before.slot_lock >> (i / 2) & 1));
    CHECK(ok && seq_prog_len(&s) == 32, "1 bar -> 1/2 bar: new slot i is old slot i/2 (each chord twice; locks and colours move)");
    CHECK(s.undo_n == n0 + 1, "one undo step");
    seq_undo(&s);
    CHECK(same_slots(&s.part[0], &before) && seq_chord_len(&s) == 16, "undo restores the slots and the chord length exactly");
    seq_set_chord_len(&s, 32);
    ok = seq_prog_len(&s) == 8;
    for (int i = 0; i < 8; i++)
        ok &= slot_is(&s, i, before.prog[2 * i], before.src[2 * i], before.colour[2 * i], (int)(before.slot_lock >> (2 * i) & 1));
    CHECK(ok, "1 bar -> 2 bars: new slot i is old slot 2i (the first chord of each new span)");
    seq_undo(&s);
    CHECK(same_slots(&s.part[0], &before), "undo restores it exactly");
    seq_set_chord_len(&s, 4);
    seq_set_chord_len(&s, 16);
    ok = seq_prog_len(&s) == 16;
    for (int i = 0; i < 16; i++)
        ok &= slot_is(&s, i, before.prog[i], before.src[i], before.colour[i], (int)(before.slot_lock >> i & 1));
    CHECK(ok, "1 bar -> 1/4 bar -> 1 bar: every chord back at its time");
    seq_undo(&s);
    seq_undo(&s);
    CHECK(same_slots(&s.part[0], &before), "two undos: exactly as before");
    nev = 0;
    seq_set_chord_len(&s, 4);
    play(&s, 0.0, 64.0, 0);
    seq_all_notes_off(&s);
    CHECK(nev > 0 && balanced(), "the remapped loop plays balanced");
}

/* Review Focus 3: a grow fills never-filled slots by repeating the loop; a shrink hides; a grow back shows them as they were */
static void test_14_grow_shrink(void) {
    static seq_t s, t;
    char buf[SEQ_STATE_MAX], f[40];
    int ok = 1;
    seq_init(&s, emit, NULL);   /* 4 bars of 1-bar chords, 0526 */
    seq_set_slot(&s, 2, 4, 0, COL_TRIAD);   /* bar 3: v, locked */
    seq_set_bars(&s, 16);
    for (int i = 0; i < 16; i++) ok &= slot_is(&s, i, "0546"[i % 4] - '0', 0, 0, i % 4 == 2);
    CHECK(ok && seq_prog_len(&s) == 16, "4 -> 16 bars: the new slots repeat the loop, edit and lock included");
    seq_set_slot(&s, 11, 1, 0, COL_TRIAD);   /* bar 12 */
    seq_set_bars(&s, 4);
    ok = seq_prog_len(&s) == 4;
    for (int i = 0; i < 4; i++) ok &= slot_is(&s, i, "0546"[i] - '0', 0, 0, i == 2);
    CHECK(ok, "16 -> 4 bars hides slots 5-16");
    seq_state_save(&s, buf, sizeof buf);
    CHECK(st_field(buf, "A.prog", f, sizeof f) == 16 && !strcmp(f, "0546054605410546"),
          "a shrunk loop saves its hidden slots as the legacy phrase (as 0.13 saved a progression longer than its loop): %s", f);
    seq_set_slot(&s, 1, 3, 0, COL_TRIAD);   /* bar 2, while shrunk */
    seq_set_bars(&s, 16);
    ok = 1;
    for (int i = 0; i < 16; i++) {
        int d = i == 1 ? 3 : i == 11 ? 1 : "0546"[i % 4] - '0';
        ok &= slot_is(&s, i, d, 0, 0, i % 4 == 2 || i == 1 || i == 11);
    }
    CHECK(ok, "4 -> 16 bars again: the hidden slots come back as they were (bar 12's edit; bar 2's edit not repeated)");
    seq_undo(&s);
    CHECK(slot_is(&s, 1, 5, 0, 0, 0) && slot_is(&s, 11, 1, 0, 0, 1) && seq_prog_len(&s) == 16, "undo takes back bar 2's edit only");
    seq_init(&t, emit, NULL);
    seq_state_load(&t, buf);   /* the 4-bar save with its hidden slots */
    seq_set_bars(&t, 16);
    CHECK(slot_is(&t, 11, 1, 0, 0, 1) && slot_is(&t, 2, 4, 0, 0, 1) && slot_is(&t, 6, 4, 0, 0, 1),
          "the saved hidden slots (and their locks, in .sl) come back on a grow after a load");
    seq_init(&s, emit, NULL);
    seq_set_slot(&s, 2, 4, 0, COL_TRIAD);   /* the snapshot: 4 slots */
    seq_set_bars(&s, 16);
    seq_undo(&s);
    ok = seq_prog_len(&s) == 16 && s.part[0].slot_lock == 0;
    for (int i = 0; i < 16; i++) ok &= slot_is(&s, i, "0526"[i % 4] - '0', 0, 0, 0);
    CHECK(ok, "undo of an edit made before a grow: the loop restored, the grown slots repeating it");
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    seq_set_bars(&s, 1);
    seq_set_bars(&s, 16);
    ok = seq_prog_len(&s) == 16;
    for (int i = 0; i < 16; i++) ok &= slot_is(&s, i, "0526"[i % 4] - '0', 0, 0, 0);
    CHECK(ok, "4 -> 8 -> 1 -> 16 bars: the phrase continues (0.13 plays prog[span %% len])");
}

/* Locks past slot 31 (a 64-bit mask), saved in .ch; NEW PROGRESSION keeps them, a preset clears them */
static void test_14_locks64(void) {
    static seq_t s, t;
    char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX], f[300];
    uint64_t want = (uint64_t)1 << 40 | (uint64_t)1 << 50 | (uint64_t)1 << 63;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_chord_len(&s, 4);
    seq_set_prog_preset(&s, 15);
    seq_toggle_slot_lock(&s, 40);
    seq_toggle_slot_lock(&s, 63);
    seq_set_slot(&s, 50, 2, 0, COL_SUS2);
    seq_toggle_slot_lock(&s, 64);   /* past the loop: ignored */
    CHECK(s.part[0].slot_lock == want && seq_slot_locked(&s, 40) && seq_slot_locked(&s, 63) && !seq_slot_locked(&s, 39) &&
          !seq_slot_locked(&s, 41), "locks on slots 40, 50, 63");
    seq_state_save(&s, buf, sizeof buf);
    CHECK(st_field(buf, "A.ch", f, sizeof f) == 256 && f[40 * 4 + 3] == '1' && f[63 * 4 + 3] == '1' && !strncmp(f + 50 * 4, "2021", 4),
          "saved in .ch");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && t.part[0].slot_lock == want && same_slots(&t.part[0], &s.part[0]) &&
          seq_state_save(&t, again, sizeof again) > 0 && !strcmp(again, buf), "and loaded back");
    int d40 = s.part[0].prog[40], d63 = s.part[0].prog[63];
    seq_generate_progression(&s);
    CHECK(s.part[0].slot_lock == want && s.part[0].prog[40] == d40 && s.part[0].prog[63] == d63 && slot_is(&s, 50, 2, 0, COL_SUS2, 1),
          "NEW PROGRESSION keeps the locked slots past 31");
    seq_set_prog_preset(&s, 0);
    CHECK(s.part[0].slot_lock == 0, "a preset clears every lock");
}

/* Generation and playback with 64 slots: every lane reads prog[span % 64] (gen_params passes prog_len 64) */
static void test_14_gen64(void) {
    static seq_t s;
    int lo = 999;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_chord_len(&s, 4);
    seq_set_prog_preset(&s, 15);
    seq_mel_set_type(&s, 1);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_MOVEMENT, 60);
    seq_set_slot(&s, 28, 6, 0, COL_7TH);   /* bar 8 beat 1: VII7 (the phrase has IV there) */
    CHECK(seq_slot_at(&s, 7, 0) == 28 && seq_slot_at(&s, 15, 12) == 63 && seq_slot_at(&s, 3, 0) == 12, "slot_at reaches every slot");
    CHECK(seq_part_step_note(&s, 0, LANE_CHORDS, 7, 0) != seq_part_step_note(&s, 0, LANE_CHORDS, 3, 0), "bar 8 beat 1 sounds its own chord");
    nev = 0;
    play(&s, 0.0, 64.0, 0);
    seq_all_notes_off(&s);
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == 0 && evs[i].k == 112 && evs[i].note < lo) lo = evs[i].note;
    CHECK(nev > 0 && balanced(), "64 slots generate and play balanced (%d events)", nev);
    CHECK(lo == 999 || lo == seq_part_step_note(&s, 0, LANE_CHORDS, 7, 0), "the Chords at bar 8 beat 1 play slot 28 (lowest %d)", lo);
}

/* seq_set_progression: n = the slot count is the whole loop; shorter, a phrase repeated with the locks kept */
static void test_14_set_progression(void) {
    static seq_t s;
    int deg[16] = {0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1}, d3[3] = {4, 2, 6}, ok = 1;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_progression(&s, 16, deg, NULL, NULL, 1);
    for (int i = 0; i < 16; i++) ok &= slot_is(&s, i, deg[i], 0, 0, 0);
    CHECK(ok, "n = the slot count: the arrays are the whole loop");
    seq_toggle_slot_lock(&s, 5);
    seq_set_progression(&s, 3, d3, NULL, NULL, 1);
    ok = 1;
    for (int i = 0; i < 16; i++) ok &= i == 5 ? slot_is(&s, i, deg[5], 0, 0, 1) : slot_is(&s, i, d3[i % 3], 0, 0, 0);
    CHECK(ok, "n shorter: a phrase repeated over the loop, the locked slot kept");
    seq_set_progression(&s, 12, deg, NULL, NULL, 1);
    ok = 1;
    for (int i = 0; i < 16; i++) ok &= i == 5 ? slot_is(&s, i, deg[5], 0, 0, 1) : slot_is(&s, i, deg[i % 8], 0, 0, 0);
    CHECK(ok, "any other n: a phrase of at most 8");
}

/* Final review Important 1: with COLOUR up, a FIX edit or a lock that breaks the loop's repeat leaves every other slot's
 * colour as it was (the ranks keep the phrase dealt, seq_part_t.rank_len), after a save and reload too; a 0.13 state
 * writes no `.rk` (test_14_old_states: byte for byte) */
static int same_eff(const seq_t *s, const int *want, int skip) {
    int ok = 1;
    for (int i = 0; i < seq_slots(s); i++)
        if (i != skip && seq_part_eff_colour(s, 0, i) != want[i]) ok = 0;
    return ok;
}

static void test_14_colour_rank(void) {
    static seq_t s, t;
    static const char *const BAD[] = {"|A.rk=0", "|A.rk=17", "|A.rk=x", "|A.rk="};
    char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX], bad[SEQ_STATE_MAX + 16], *q;
    int e0[SEQ_MAX_PROG], mixed = 0;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_prog_preset(&s, 0);   /* Epic, 4 chords */
    seq_set_chord_shape(&s, SHAPE_COLOUR, 50);
    for (int i = 0; i < 16; i++) e0[i] = seq_part_eff_colour(&s, 0, i), mixed |= 1 << e0[i];
    CHECK(mixed == 1 << COL_TRIAD, "16 bars of Epic at SHAPE_COLOUR 50: all Triads (retired in 0.17: the rank rule no longer colours)");
    seq_set_slot(&s, 7, 3, 0, COL_TRIAD);   /* bar 8: iv */
    CHECK(same_eff(&s, e0, 7), "FIX bar 8: every other slot keeps its colour");
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A.rk=4") != NULL, "  the save keeps the ranks' phrase (.rk=4)");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && same_eff(&t, e0, 7) && seq_part_eff_colour(&t, 0, 7) == seq_part_eff_colour(&s, 0, 7),
          "  reloaded: the same colours");
    seq_state_save(&t, again, sizeof again);
    CHECK(!strcmp(buf, again), "  and re-saves byte for byte");
    int d3[3] = {0, 4, 5};
    seq_set_progression(&s, 3, d3, NULL, NULL, 1);   /* a deal: ranks of 3 */
    seq_undo(&s);
    CHECK(s.part[0].rank_len == 4 && same_eff(&s, e0, 7), "  a deal's UNDO brings the ranks back");
    for (int k = 0; (q = strstr(buf, "|A.rk=4")) && k < (int)(sizeof BAD / sizeof BAD[0]); k++) {
        snprintf(bad, sizeof bad, "%.*s%s%s", (int)(q - buf), buf, BAD[k], q + 7);
        seq_init(&t, emit, NULL);
        CHECK(seq_state_load(&t, bad) && seq_prog_len(&t) == 16, "  a bad '%s' is ignored, never refused", BAD[k] + 1);
    }
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_prog_preset(&s, 0);
    seq_set_chord_shape(&s, SHAPE_COLOUR, 50);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, ".rk="), "the phrase as dealt writes no .rk");
    seq_toggle_slot_lock(&s, 1);   /* bar 2 locked: the slots stop repeating */
    CHECK(seq_slot_locked(&s, 1) && same_eff(&s, e0, -1), "a lock alone: every slot keeps its colour");
    seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && same_eff(&t, e0, -1), "  after a save and reload too");
    seq_toggle_slot_lock(&s, 1);
    CHECK(same_eff(&s, e0, -1), "  and unlocked");
    /* ranks of 3 on a loop repeating 8 (8 is no multiple of 3): voiced and named slot by slot */
    static const char R3[] = "csq2|gv=1|k=4|s=1|A.len=16|A.cl=16|A.h=30,1|A.prog=01234560|A.voi=1|A.cs=50,50,0,50,0,50,0"
                             "|A.rk=3|ch=1,2,3|A1=0:|A1.g=2,1,30|A2=0:|A2.g=4,1,30|A3=0:|A3.g=7,1,30|A4=0:";
    int ok, sevens = 0;
    seq_init(&s, emit, NULL);
    ok = seq_state_load(&s, R3) && s.part[0].phrase == 8 && s.part[0].rank_len == 3;
    for (int i = 0; i < 16; i++) {
        char lab[16];
        int e = seq_part_eff_colour(&s, 0, i);
        seq_slot_label(&s, i, lab, sizeof lab);
        sevens += e == COL_7TH;
        ok &= e == seq_part_eff_colour(&s, 0, i % 3) && (s.part[0].voice_n[i] == 4) == (e == COL_7TH) &&
              (strchr(lab, '7') != NULL) == (e == COL_7TH);
    }
    CHECK(ok && sevens == 0, "ranks of 3 on a loop repeating 8: voiced and named slot by slot, as they sound "
          "(%d 7ths; retired in 0.17: the rank rule no longer colours)", sevens);
    CHECK(saves_but_drawn(&s, R3), "  and re-saves byte for byte (0.18: but the Chords lane, as DRAWN bars)");
}

/* Fix round 1: seq_fix_bar. A FIX pick edits the bar on screen; following and playing, one within 0.5 beat after a bar
 * change edits the bar before it (MPC's redraw lags) */
static void test_12_fix_bar(void) {
    static seq_t s;
    double ppq;
    fresh(&s, 0);
    seq_set_view(&s, 3);
    CHECK(seq_fix_bar(&s) == 3, "stopped: the picked bar (view_bar)");
    seq_set_follow(&s, 1);
    CHECK(seq_fix_bar(&s) == 3, "stopped, following: still view_bar");
    ppq = play(&s, 0.0, 4.2, 0);   /* 0.2 beat into bar 2 */
    CHECK(seq_grid_bar(&s) == 1 && seq_fix_bar(&s) == 0, "0.2 beat after the bar line: the previous bar (grid %d, fix %d)",
          seq_grid_bar(&s), seq_fix_bar(&s));
    seq_set_step_note(&s, LANE_BASS, seq_fix_bar(&s), 9, 47);
    CHECK(s.part[0].lane[LANE_BASS].steps[9].abs == 47 && !s.part[0].lane[LANE_BASS].steps[SEQ_STEPS + 9].abs,
          "the pick lands on bar 1");
    ppq = play(&s, ppq, 0.6, 0);   /* 0.8 beat in */
    CHECK(seq_fix_bar(&s) == 1, "0.8 beat after it: the new bar (%d)", seq_fix_bar(&s));
    ppq = play(&s, ppq, 1.2, 0);   /* mid-bar */
    CHECK(seq_fix_bar(&s) == 1 && seq_grid_bar(&s) == 1, "mid-bar: the playing bar");
    ppq = play(&s, ppq, 10.1, 16.0);   /* past the loop wrap at 16 */
    CHECK(seq_grid_bar(&s) == 0 && seq_fix_bar(&s) == 3, "0.1 beat after a loop wrap: the loop's last bar (fix %d)", seq_fix_bar(&s));
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(seq_fix_bar(&s) == seq_view_bar(&s), "stopped again: view_bar");
    ppq = play(&s, 0.0, 0.2, 0);
    CHECK(seq_fix_bar(&s) == 0, "0.2 beat after a start: the bar playing (nothing was shown before it)");
    seq_block(&s, ppq, bpm, 0, 128);
    /* a part switch: the new part's first bar, never the old part's bar */
    fresh(&s, 0);
    seq_select_part(&s, 3);
    seq_set_bars(&s, 2);
    seq_select_part(&s, 0);
    ppq = play(&s, 0.0, 15.0, 0);   /* the Verse's bar 4 */
    seq_select_part(&s, 3);
    ppq = play(&s, ppq, 1.2, 0);    /* 0.2 beat into the Chorus */
    CHECK(seq_part(&s) == 3 && seq_fix_bar(&s) == 0, "0.2 beat after a part switch: the new part's bar 1 (fix %d)", seq_fix_bar(&s));
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(balanced(), "no stuck notes");
}

/* The notes (ascending) that went on, on channel ch, in 16th k; returns the count */
static int notes_at(int ch, int k, int *out) {
    int n = 0;
    for (int i = 0; i < nev && n < 8; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].k == k) out[n++] = evs[i].note;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j - 1] > out[j]; j--) { int x = out[j]; out[j] = out[j - 1]; out[j - 1] = x; }
    return n;
}

/* 0.15 INIT: the whole instance as a safe start. C major, I-V-vi-IV, 4 bars of 1-bar chords: one held root-position triad
 * per bar on Chords, the root an octave below it once per bar on Bass, (0.20) the JAM block ARP UP on Melody, landing home. The
 * MIDI channels stay; held notes end; every part is the Verse alone again; UNDO is cleared. */
static void test_15_init_preset(void) {
    static const int ROOT[4] = {0, 7, 9, 5};   /* C G Am F */
    /* the Melody's 8ths (0.20 ARP UP, LOOP, CADENCE HOME): root, 3rd, 5th, octave of each chord; the loop's last note lands home
     * on C5, the note before it on the same degree stepping up to D5 (the reference's endOn) */
    static const int ARP[4][8] = {{60, 64, 67, 72, 60, 64, 67, 72}, {55, 59, 62, 67, 55, 59, 62, 67},
                                  {57, 60, 64, 69, 57, 60, 64, 69}, {65, 69, 72, 77, 65, 69, 74, 72}};
    static seq_t s;
    char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_set_channel(&s, LANE_BASS, 7);
    seq_set_mute(&s, LANE_MEL_UP, 1);
    seq_select_part(&s, 3);
    nev = 0;
    play(&s, 0.0, 6.0, 0);   /* notes held when INIT comes */
    seq_init_preset(&s);
    CHECK(balanced(), "INIT ends every held note");
    CHECK(seq_key(&s) == 0 && seq_scale(&s) == SCALE_MAJOR && seq_bars(&s) == 4 && seq_chord_len(&s) == SEQ_STEPS &&
          seq_voicing(&s) == VOICING_CLOSE && s.cur_part == 0 && s.used_mask == 1 && s.undo_n == 0,
          "INIT: C major, 4 bars of 1-bar chords, close voicing, the Verse alone, no undo");
    CHECK(seq_prog_preset(&s) == 1, "INIT plays the 1-5-6-4 Pop progression (preset %d)", seq_prog_preset(&s));
    CHECK(seq_channel_of(&s, LANE_BASS) == 7 && !seq_mute(&s, LANE_MEL_UP), "INIT keeps the channels and clears the mutes");
    CHECK(s.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && s.part[0].mjam.set.c == MJ_ARP && s.part[0].mjam.set.l == 0 &&
          s.part[0].mjam.set.form == MF_LOOP && s.part[0].mjam.set.cad == 3, "INIT's melody is JAM ARP UP, LOOP, CADENCE HOME (0.20)");
    nev = 0;
    play(&s, 16.0, 16.0, 0);   /* 4 bars, from bar 1 of the song's 5th bar */
    int ok_ch = 1, ok_bass = 1, ok_arp = 1;
    for (int b = 0; b < 4; b++) {
        int c[8], bs[8], k0 = 64 + b * SEQ_STEPS, r = 48 + ROOT[b];
        int want[3] = {r, r + (b == 2 ? 3 : 4), r + 7};
        ok_ch &= notes_at(1, k0, c) == 3 && c[0] == want[0] && c[1] == want[1] && c[2] == want[2];
        for (int i = 1; i < SEQ_STEPS; i++) ok_ch &= notes_at(1, k0 + i, c) == 0;
        ok_bass &= notes_at(7, k0, bs) == 1 && bs[0] % 12 == ROOT[b] && bs[0] >= 28 && bs[0] <= 39;   /* 0.19: E1-D#2 */
        for (int i = 1; i < SEQ_STEPS; i++) ok_bass &= notes_at(7, k0 + i, bs) == 0;
        for (int i = 0; i < SEQ_STEPS; i++) {
            int x[8], n = notes_at(3, k0 + i, x), want_m = i % 2 ? 0 : ARP[b][i / 2];
            if (n != (want_m > 0) || (n && x[0] != want_m)) {
                if (ok_arp) printf("     melody bar %d step %d: %d notes (%d), want %d\n", b + 1, i, n, n ? x[0] : 0, want_m);
                ok_arp = 0;
            }
        }
    }
    CHECK(ok_ch, "INIT Chords: one root-position triad per bar on beat 1 (C, G, Am, F)");
    CHECK(ok_bass, "INIT Bass: the chord's root, once per bar (0.19: in E1-D#2, REGISTER Natural)");
    CHECK(ok_arp, "INIT Melody: every 8th up each chord, the loop landing home on C5");
    CHECK(balanced(), "INIT plays balanced notes");
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0, "INIT saves");
    static seq_t t;
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_key(&t) == 0 && t.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM &&
          !memcmp(t.part[0].mline, s.part[0].mline, sizeof s.part[0].mline) && seq_prog_preset(&t) == 1, "INIT's state reloads as INIT");
}

/* VST-12 phrasing: each setting is one undo step (bad values ignored); a save writes .mp and, in .mr, the old phrase
 * nearest it; a reload restores it; a 0.13 state (no .mp) opens at its old phrase's phrasing and plays the same; a bad
 * .mp, or one without a recipe, is ignored */
static void test_vst12_phrasing_state(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], buf2[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    old_mel(&s);   /* 0.20: the recipe setters leave a JAM lane alone */
    seq_mel_set_type(&s, MEL_HOOK);
    int n = s.undo_n;
    seq_mel_set_phrasing(&s, MP_FORM, MEL_FORM_3_1);
    seq_mel_set_phrasing(&s, MP_FORM, MEL_NFORMS);
    seq_mel_set_phrasing(&s, MP_BREATH, -1);
    seq_mel_set_phrasing(&s, MP_COUNT, 0);
    seq_mel_set_phrasing(&s, MP_FORM, MEL_FORM_3_1);   /* a re-send */
    CHECK(s.undo_n == n + 1 && seq_mel_phrasing(&s, MP_FORM) == MEL_FORM_3_1 && seq_mel_phrase(&s) == -1 && mel_is_ref(&s),
          "a phrasing setting: one undo step; bad values and a re-send: ignored; not an old phrase (-1)");
    seq_undo(&s);
    CHECK(seq_mel_phrasing(&s, MP_FORM) == MEL_FORM_1_1 && seq_mel_phrasing(&s, MP_CADENCE) == MEL_CAD_QA && mel_is_ref(&s),
          "its undo: the Hook's 1 + 1 Q&A again");
    seq_mel_set_phrasing(&s, MP_FORM, MEL_FORM_2_2);
    seq_mel_set_phrasing(&s, MP_BREATH, MEL_BREATH_HALF);
    seq_mel_set_phrasing(&s, MP_FILL, MEL_FILL_PICKUP);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A3.mr=3,1,2,2,1,") && strstr(buf, "|A3.mp=2,3,2,1"), "saved: .mr's phrase the nearest old one (2 + 2), .mp the phrasing");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && !memcmp(&t.part[0].mel, &s.part[0].mel, sizeof s.part[0].mel) &&
              !memcmp(mel_st(&t), mel_st(&s), sizeof(seq_step_t) * SEQ_LANE_STEPS), "a reload: the phrasing and the same steps");
    /* a 0.13 save: no .mp, phrase 1 + 1 */
    set_field(buf, "|A3.mp=", NULL, buf2, sizeof buf2);
    set_field(buf2, "|A3.mr=", "3,1,2,2,0,45,30,35,25,50,55,70,0,0,0", buf, sizeof buf);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_mel_phrase(&t) == MEL_PHRASE_1_1 && seq_mel_phrasing(&t, MP_BREATH) == MEL_BREATH_BEAT &&
              seq_mel_phrasing(&t, MP_CADENCE) == MEL_CAD_LAND && seq_mel_phrasing(&t, MP_FILL) == MEL_FILL_REST,
          "a 0.13 state (no .mp) opens at its phrase's phrasing: 1 + 1, Beat, Land, Rest");
    seq_state_save(&t, buf2, sizeof buf2);
    CHECK(strstr(buf2, "|A3.mr=3,1,2,2,0,") && !strstr(buf2, "|A3.mp="), "and saves that phrase back as 0.13 did (no .mp)");
    set_field(buf2, "|A3.mr=", "3,1,2,2,0,45,30,35,25,50,55,70,0,0,0|A3.mp=1,2,1,0", buf, sizeof buf);   /* bad .mp tests below */
    memcpy(buf2, buf, sizeof buf2);
    static const char *const BAD[] = {"4,0,0,0", "0,4,0,0", "0,0,3,0", "0,0,0,2", "1,2,1", "1,2,1,0,0", ""};
    int ok = 1;
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        set_field(buf2, "|A3.mp=", BAD[i], buf, sizeof buf);
        seq_init(&t, emit, NULL);
        ok &= seq_state_load(&t, buf) && seq_mel_phrase(&t) == MEL_PHRASE_1_1;
    }
    CHECK(ok, "a bad .mp is ignored: the .mr phrase's phrasing stays");
    set_field(buf2, "|A3.mr=", NULL, buf, sizeof buf);   /* .mp alone, on a classic Hook */
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_style(&t, LANE_MEL_UP) == GEN_MEL_HOOK, ".mp without a recipe (.mr) is ignored");
}

/* 0.16 notes for every bar: NEW BAR changes only its bar and is one undo step; RESET BAR returns the bar to the phrase and
 * unlocks its fixed notes; saved as .bv only when a bar has a variation; reloads; a bad .bv is ignored; a Manual lane ignores both */
static void test_16_notes_per_bar(void) {
    static seq_t s, t;
    static seq_lane_t was;
    static char buf[SEQ_STATE_MAX], buf2[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
    seq_set_complexity(&s, LANE_BASS, 60);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, ".bv="), "no bar of its own: no .bv in the save");
    memcpy(&was, cur_lane(&s, LANE_BASS), sizeof was);
    int n = s.undo_n;
    seq_lane_new_bar(&s, LANE_BASS, 5);
    const seq_lane_t *ln = cur_lane(&s, LANE_BASS);
    int other = 1;
    for (int i = 0; i < 8 * SEQ_STEPS; i++)
        if (i / SEQ_STEPS != 5) other &= !memcmp(&ln->steps[i], &was.steps[i], sizeof was.steps[i]);
    CHECK(seq_lane_bar_var(&s, LANE_BASS, 5) == 1 && s.undo_n == n + 1 && other &&
              memcmp(&ln->steps[80], &was.steps[80], sizeof was.steps[0] * SEQ_STEPS),
          "NEW BAR on bar 6: only bar 6 changes, one undo step");
    seq_lane_new_bar(&s, LANE_BASS, 5);
    CHECK(seq_lane_bar_var(&s, LANE_BASS, 5) == 2, "again: another variation");
    seq_set_step_note(&s, LANE_BASS, 5, 0, 40);
    CHECK(seq_lane_bar_locks(&s, LANE_BASS, 5) == 1, "a note fixed in bar 6 locks");
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A2.bv=00000000000200000000000000000000") != NULL, "saved: .bv with bar 6's variation");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && bass_loaded_as(cur_lane(&s, LANE_BASS), cur_lane(&t, LANE_BASS), 8) == 1 &&
              seq_lane_bar_var(&t, LANE_BASS, 5) == 2 && t.part[0].bass.fix[80] == 40 && resaves_same(&t),
          "a reload (0.19: DRAWN bars that keep the rhythm): bar 6's variation and fixed note kept, saved back byte for byte from then on");
    set_field(buf, "|A2.bv=", "0000000000020000000000000000000", buf2, sizeof buf2);   /* 31 digits */
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf2) && !seq_lane_bar_var(&t, LANE_BASS, 5), "a bad .bv is ignored: every bar the phrase's");
    n = s.undo_n;
    seq_lane_reset_bar(&s, LANE_BASS, 5);
    ln = cur_lane(&s, LANE_BASS);
    CHECK(!seq_lane_bar_var(&s, LANE_BASS, 5) && !seq_lane_bar_locks(&s, LANE_BASS, 5) && s.undo_n == n + 1 &&
              !memcmp(ln->steps, was.steps, sizeof was.steps), "RESET BAR: bar 6 back to the phrase, its note unlocked, one undo step");
    seq_lane_reset_bar(&s, LANE_BASS, 5);
    CHECK(s.undo_n == n + 1, "RESET on a bar that is the phrase's: no edit");
    seq_undo(&s);
    CHECK(seq_lane_bar_var(&s, LANE_BASS, 5) == 2 && seq_lane_bar_locks(&s, LANE_BASS, 5) == 1, "UNDO: bar 6's own line and its fixed note back");
    seq_lane_new_bar(&s, LANE_BASS, 8);   /* past the loop */
    seq_set_style(&s, LANE_MEL_UP, GEN_MANUAL);
    n = s.undo_n;
    seq_lane_new_bar(&s, LANE_MEL_UP, 0);
    seq_lane_new_bar(&s, LANE_CHORDS, 0);
    CHECK(s.undo_n == n && !seq_lane_bar_var(&s, LANE_MEL_UP, 0) && !seq_lane_bar_var(&s, LANE_BASS, 8),
          "ignored: a bar past the loop, a Manual lane, the Chords lane");
    seq_mel_set_type(&s, MEL_HOOK);   /* a recipe melody */
    memcpy(&was, cur_lane(&s, LANE_MEL_UP), sizeof was);
    seq_lane_new_bar(&s, LANE_MEL_UP, 2);
    ln = cur_lane(&s, LANE_MEL_UP);
    other = 1;
    for (int i = 0; i < 8 * SEQ_STEPS; i++)
        if (i / SEQ_STEPS != 2 && i != SEQ_STEPS + 14) other &= !memcmp(&ln->steps[i], &was.steps[i], sizeof was.steps[i]);
    CHECK(other && seq_lane_bar_var(&s, LANE_MEL_UP, 2) == 1, "a recipe Melody: NEW BAR on bar 3 leaves the other bars");
}
/* The chord onsets on channel ch in the event log, in order: each onset's lowest note; returns the count */
static int chord_lows(int ch, int *out, int max) {
    int n = 0;
    for (int i = 0; i < nev && n < max; i++) {
        if (!is_on(&evs[i]) || (evs[i].status & 15) != ch - 1 || (i && is_on(&evs[i - 1]) && (evs[i - 1].status & 15) == ch - 1 &&
                                                                    evs[i - 1].k == evs[i].k))
            continue;
        int low = evs[i].note;
        for (int j = i; j < nev && evs[j].k == evs[i].k; j++)
            if (is_on(&evs[j]) && (evs[j].status & 15) == ch - 1 && evs[j].note < low) low = evs[j].note;
        out[n++] = low;
    }
    return n;
}

/* 0.16: a start (the clip on the plugin's track launched, or PLAY) plays the part from its bar 1, at the MPC bar the playhead
 * is in: the loop is the clip's, not the song's. A loop of the whole 128-bar sequence wraps with the part in step; a stop and
 * a start elsewhere begins at bar 1 again. Init: 4 bars, C G Am F, one chord a bar on channel 1 (lowest notes 48, 55, 57, 53) */
static void test_16_start_bar1(void) {
    static seq_t s;
    int lows[16], n;
    seq_init(&s, emit, NULL);
    seq_init_preset(&s);
    nev = 0;
    double ppq = play(&s, 8.0, 8.0, 0);   /* start at song bar 3, two bars */
    n = chord_lows(1, lows, 16);
    CHECK(n == 2 && lows[0] == 48 && lows[1] == 55, "a start at song bar 3 plays the part's bars 1 and 2 (C, G), not 3 and 4 (%d: %d)",
          n, n ? lows[0] : -1);
    seq_block(&s, ppq, bpm, 0, 128);   /* stop */
    nev = 0;
    ppq = play(&s, 42.0, 6.0, 0);   /* start at song bar 11, beat 3: the part's bar 1 from its beat 3, then bar 2 */
    n = chord_lows(1, lows, 16);
    CHECK(n >= 1 && lows[0] == 55, "a start mid-bar (bar 11, beat 3) is in the part's bar 1: the next chord is bar 2's G (got %d)",
          n ? lows[0] : -1);
    seq_block(&s, ppq, bpm, 0, 128);
    nev = 0;
    play(&s, 500.0, 24.0, 512.0);   /* start at song bar 126, then a loop of the whole 128 bars back to bar 1 */
    n = chord_lows(1, lows, 16);
    static const int WANT[6] = {48, 55, 57, 53, 48, 55};   /* bars 126-128: C G Am; after the wrap: F C G */
    int ok = n == 6;
    for (int i = 0; ok && i < 6; i++) ok &= lows[i] == WANT[i];
    CHECK(ok, "a loop of the whole 128-bar sequence wraps with the part in step: C G Am | F C G (%d chords)", n);
}

/* 0.17: a fresh instance's first 4 bars, every event's status, note, velocity and block position folded into one FNV-1a
 * checksum: the CHORDS page's knobs and styles at their defaults must not change what 0.16 played */
/* 0.18: the 4 bars without the Chords lane's events (channel 1), recorded on 0.17.1, which played 0.16.1's (0x618c9577 with
 * them): a new instance's Chords lane is now drawn Sustain, its Bass and Melody play as 0.16's did */
#define CK_016_DEFAULT_NO_CHORDS 0xd1b12051u
static unsigned events_ck_without(int ch) {   /* ch 1..16 left out; 0 none */
    unsigned h = 2166136261u;
    for (int i = 0; i < nev; i++) {
        if ((evs[i].status & 15) == ch - 1) continue;
        long ppq = lround(evs[i].ppq * 1e6);
        unsigned char b[8] = {(unsigned char)evs[i].status, (unsigned char)evs[i].note, (unsigned char)evs[i].vel, (unsigned char)evs[i].k,
                              (unsigned char)ppq, (unsigned char)(ppq >> 8), (unsigned char)(ppq >> 16), (unsigned char)(ppq >> 24)};
        for (int j = 0; j < 8; j++) h = fnv(h, b[j]);
    }
    return h;
}

static unsigned events_ck(void) { return events_ck_without(0); }

static void test_17_defaults_sound_as_016(void) {
    static seq_t s;
    int lows[8];
    seq_init(&s, emit, NULL);
    old_bass(&s, 30);   /* 0.19: a new instance's Bass is DRAWN (test_19_init_bass); 0.18's Root c30 put back */
    old_mel(&s);        /* 0.20: its Melody is JAM (test_20_init_mel); 0.19's Hook c30 put back */
    nev = 0;
    play(&s, 0, 16.0, 0);
    seq_block(&s, 16.0, bpm, 0, 128);   /* stop */
    CHECK(events_ck_without(1) == CK_016_DEFAULT_NO_CHORDS && nev > 0,
          "a fresh instance (0.19: with 0.18's Bass; 0.20: and 0.19's Melody) plays 4 bars of Bass and Melody as 0.16 did (got 0x%08x, %d events)",
          events_ck_without(1), nev);
    CHECK(chord_lows(1, lows, 8) == 4 && lows[0] == 52 && balanced(), "0.18: its Chords lane plays Sustain, one chord a bar (Em first)");
}

/* Em close (52 55 59) at bar 1 step 0 of a v1 instance, played for 1 beat: the Chords lane's note-ons, in order */
static int em_notes(seq_t *s, int *got) {
    nev = 0;
    play_stop(s, 1.0);
    return ons(2, got, THEORY_CHORD_MAX);
}

static int same_notes(const int *got, int n, int a, int b, int c, int d) {
    const int want[4] = {a, b, c, d};
    for (int i = 0; i < n; i++)
        if (i >= 4 || got[i] != want[i]) return 0;
    return 1;
}

static void test_17_spread(void) {
    static seq_t s;
    int got[THEORY_CHORD_MAX], n;
    fresh(&s, 0);   /* close Em 52 55 59 */
    n = em_notes(&s, got);
    CHECK(n == 3 && same_notes(got, n, 52, 55, 59, 0), "SPREAD 0 Close: 52 55 59");
    seq_set_vknob(&s, VK_SPR, 1);
    n = em_notes(&s, got);
    CHECK(n == 3 && same_notes(got, n, 52, 59, 67, 0), "SPREAD +1 Open: 52 59 67 (got %d: %d %d %d)", n, got[0], got[1], got[2]);
    seq_set_vknob(&s, VK_SPR, 2);
    n = em_notes(&s, got);
    CHECK(n == 3 && same_notes(got, n, 40, 55, 59, 0), "SPREAD +2 Split: 40 55 59 (got %d: %d %d %d)", n, got[0], got[1], got[2]);
    seq_set_vknob(&s, VK_SPR, 3);
    n = em_notes(&s, got);
    CHECK(n == 3 && same_notes(got, n, 40, 59, 67, 0), "SPREAD +3 Wide: 40 59 67 (got %d: %d %d %d)", n, got[0], got[1], got[2]);
    seq_set_vknob(&s, VK_SPR, -1);
    n = em_notes(&s, got);
    CHECK(n == 4 && same_notes(got, n, 52, 54, 55, 59), "SPREAD -1 Cluster: the scale's 2nd above the root, 52 54 55 59 (got %d: %d %d %d %d)",
          n, got[0], got[1], got[2], got[3]);
    seq_set_vknob(&s, VK_INV, 1);   /* the 2nd goes above the root where the inversion left it: G B E' F#' */
    n = em_notes(&s, got);
    CHECK(n == 4 && same_notes(got, n, 55, 59, 64, 66), "Cluster on the 1st inversion: 55 59 64 66 (got %d: %d %d %d %d)", n, got[0], got[1], got[2], got[3]);
    seq_set_vknob(&s, VK_INV, 0);
    seq_set_chord_shape(&s, SHAPE_OCT, 1);
    n = em_notes(&s, got);
    CHECK(n == 4 && same_notes(got, n, 64, 66, 67, 71), "OCTAVE +1 moves the Cluster too: 64 66 67 71");
}

static void test_17_inversion(void) {
    static seq_t s;
    static const int WANT[][4] = {{43, 47, 52}, {47, 52, 55}, {52, 55, 59}, {55, 59, 64}, {59, 64, 67}, {64, 67, 71}};   /* -2 .. 3 */
    int got[THEORY_CHORD_MAX], ok = 1;
    fresh(&s, 0);
    for (int v = -2; v <= 3; v++) {
        seq_set_vknob(&s, VK_INV, v);
        int n = em_notes(&s, got);
        ok &= n == 3 && same_notes(got, n, WANT[v + 2][0], WANT[v + 2][1], WANT[v + 2][2], 0);
        CHECK(n == 3 && same_notes(got, n, WANT[v + 2][0], WANT[v + 2][1], WANT[v + 2][2], 0), "INVERSION %+d: %d %d %d (got %d %d %d)", v,
              WANT[v + 2][0], WANT[v + 2][1], WANT[v + 2][2], got[0], got[1], got[2]);
    }
    CHECK(ok && seq_vknob(&s, VK_INV) == 3, "INVERSION runs -2 to 3");
    seq_set_vknob(&s, VK_INV, 1);
    seq_set_vknob(&s, VK_SPR, 1);   /* INVERSION first, then SPREAD: 55 59 64 -> Open lifts the 2nd note: 55 64 71 */
    int n = em_notes(&s, got);
    CHECK(n == 3 && same_notes(got, n, 55, 64, 71, 0), "INVERSION +1 then SPREAD Open: 55 64 71 (got %d %d %d)", got[0], got[1], got[2]);
}

/* the note-ons of channel ch, each with the block position it came out in */
static int on_times(int ch, int *notes, double *ppq, int max) {
    int n = 0;
    for (int i = 0; i < nev && n < max; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1) notes[n] = evs[i].note, ppq[n++] = evs[i].ppq;
    return n;
}

static void test_17_strum_down(void) {
    static seq_t s;
    int got[THEORY_CHORD_MAX], n, ok = 1;
    double at[THEORY_CHORD_MAX];
    fresh(&s, 0);
    seq_set_vknob(&s, VK_STR, 4);
    nev = 0;
    play_stop(&s, 1.0);
    n = on_times(2, got, at, THEORY_CHORD_MAX);
    CHECK(n == 3 && same_notes(got, n, 52, 55, 59, 0), "STRUM +4: low to high (%d %d %d)", got[0], got[1], got[2]);
    seq_set_vknob(&s, VK_STR, -4);
    CHECK(seq_chord_shape(&s, SHAPE_STRUM) == 50 && seq_vknob(&s, VK_STR) == -4, "STRUM -4: SHAPE_STRUM 50, reads back -4");
    nev = 0;
    play_stop(&s, 1.0);
    n = on_times(2, got, at, THEORY_CHORD_MAX);
    CHECK(n == 3 && same_notes(got, n, 59, 55, 52, 0), "STRUM -4: high to low (%d %d %d)", got[0], got[1], got[2]);
    for (int i = 0; i < n; i++) ok &= fabs(at[i] - i * 0.03125) <= 2 * blk();
    CHECK(ok, "  0.03125 beat apart (blocks at %.4f %.4f %.4f)", at[0], at[1], at[2]);
    seq_set_vknob(&s, VK_STR, 0);
    nev = 0;
    play_stop(&s, 1.0);
    n = on_times(2, got, at, THEORY_CHORD_MAX);
    CHECK(n == 3 && same_notes(got, n, 52, 55, 59, 0) && at[2] == at[0] && seq_vknob(&s, VK_STR) == 0, "STRUM 0 Block: all at once, low to high");
}

/* Review Focus 4: a 6-note chord (13th plus Cluster) strums whole in both directions, one chord after another */
static void test_17_six_notes_strum(void) {
    static seq_t s;
    int got[THEORY_CHORD_MAX + 2], n, ok = 1, mx = 0;
    double at[THEORY_CHORD_MAX + 2];
    fresh(&s, 0);
    seq_set_vknob(&s, VK_COL, 4);
    seq_set_vknob(&s, VK_SPR, -1);
    seq_set_vknob(&s, VK_STR, 8);
    CHECK(SEQ_MAX_STRUM >= 5 * 2 && s.part[0].voice_n[0] == 5, "a 13th is 5 voiced notes; the strum queue holds %d", SEQ_MAX_STRUM);
    nev = 0;
    ev_seq = &s;
    for (double ppq = 0; ppq < 16.0; ppq += blk()) {   /* 4 bars: the queue never holds more than 5, and every note comes out */
        ev_ppq = ppq;
        seq_block(&s, ppq, bpm, 1, 128);
        mx = s.nstrum > mx ? s.nstrum : mx;
    }
    ev_seq = NULL;
    seq_block(&s, 16.0, bpm, 0, 128);
    CHECK(count_on(2) == 4 * 6 && balanced() && mx == 5, "STRUM +8: 6 notes a chord x 4 bars, none dropped (%d note-ons, at most %d queued)", count_on(2), mx);
    nev = 0;
    play_stop(&s, 2.0);
    n = on_times(2, got, at, 8);
    for (int i = 1; i < 6; i++) ok &= got[i] > got[i - 1];
    for (int i = 0; i < 6; i++) ok &= fabs(at[i] - i * 0.0625) <= 2 * blk();
    CHECK(n == 6 && ok, "  low to high, 1/16 beat apart (%d notes, last at %.4f)", n, at[n - 1]);
    seq_set_vknob(&s, VK_STR, -8);
    nev = 0;
    play_stop(&s, 2.0);
    n = on_times(2, got, at, 8);
    ok = n == 6;
    for (int i = 1; i < 6; i++) ok &= got[i] < got[i - 1];
    for (int i = 0; i < 6; i++) ok &= fabs(at[i] - i * 0.0625) <= 2 * blk();
    CHECK(ok && balanced(), "STRUM -8: the same six, high to low");
}

static void test_17_colour_levels(void) {
    static seq_t s;
    static const char *const NAME[] = {"E5", "Em", "Em7", "Em9", "Em11", "Em13"};
    static const int COUNT[] = {2, 3, 4, 5, 5, 5};
    char buf[16];
    int got[THEORY_CHORD_MAX], ok = 1;
    fresh(&s, 0);
    for (int v = -1; v <= 4; v++) {
        seq_set_vknob(&s, VK_COL, v);
        seq_slot_label(&s, 0, buf, sizeof buf);
        int n = em_notes(&s, got);
        ok &= n == COUNT[v + 1] && !strcmp(buf, NAME[v + 1]);
        CHECK(n == COUNT[v + 1] && !strcmp(buf, NAME[v + 1]), "COLOUR %+d: %s, %d notes (got %s, %d)", v, NAME[v + 1], COUNT[v + 1], buf, n);
    }
    seq_set_vknob(&s, VK_COL, 2);
    seq_set_slot(&s, 0, 0, 0, COL_SUS4);   /* a slot's own colour wins; the others take the knob's */
    seq_slot_label(&s, 0, buf, sizeof buf);
    CHECK(!strcmp(buf, "Esus4") && s.part[0].voice_n[0] == 3 && s.part[0].voice_n[1] == 5, "an own colour (sus4) stays at COLOUR +2 (%s)", buf);
    seq_set_slot(&s, 0, 0, 0, COL_TRIAD);
    seq_slot_label(&s, 0, buf, sizeof buf);
    CHECK(!strcmp(buf, "Em9") && seq_slot_colour(&s, 0) == COL_9TH, "a Triad slot takes the knob's (0.17: a lock writes the level as its own colour)");
}

static void test_17_knob_api(void) {
    static seq_t s;
    fresh(&s, 0);
    CHECK(s.part[0].vstyle == -1 && s.part[0].vmode == VMODE_CLOSE && s.part[0].vcol == 0 && s.part[0].vinv == 0 && s.part[0].vspr == 0 &&
          s.part[0].vstrdir == 0, "a v1 part: Custom, Close, every knob 0");
    seq_init(&s, emit, NULL);
    CHECK(s.part[0].vstyle == VOICING_DEFAULT && s.part[0].vmode == VMODE_SMOOTH && s.part[0].voicing == VOICING_SMOOTH && s.part[0].vcol == 0 &&
          s.part[0].vinv == 0 && s.part[0].vspr == 0 && s.part[0].vstrdir == 0, "a new instance: Pop Piano, Smooth, every knob 0");
    seq_set_voicing(&s, VOICING_CLOSE);
    CHECK(s.part[0].vmode == VMODE_CLOSE && seq_voicing(&s) == VOICING_CLOSE, "seq_set_voicing Close sets vmode Close");
    seq_set_voicing(&s, VOICING_SMOOTH);
    CHECK(s.part[0].vmode == VMODE_SMOOTH && seq_voicing(&s) == VOICING_SMOOTH, "  and Smooth sets vmode Smooth");
    for (int m = VMODE_CLOSE; m < VMODE_COUNT; m++) {
        seq_set_vmode(&s, m);
        CHECK(s.part[0].vmode == m && seq_voicing(&s) == (m == VMODE_CLOSE ? VOICING_CLOSE : VOICING_SMOOTH), "vmode %d keeps voicing %d (.voi)", m, seq_voicing(&s));
    }
    seq_set_vmode(&s, 9);
    seq_set_vmode(&s, -1);
    CHECK(s.part[0].vmode == VMODE_ROOTLESS, "a bad mode is ignored");
    seq_set_vmode(&s, VMODE_SMOOTH);
    int want[] = {0, 13, 25, 38, 50, 63, 75, 88, 100};   /* round(|v| x 12.5) */
    for (int v = 0; v <= 8; v++) {
        seq_set_vknob(&s, VK_STR, v);
        CHECK(seq_chord_shape(&s, SHAPE_STRUM) == want[v] && seq_vknob(&s, VK_STR) == v && !s.part[0].vstrdir, "STRUM %+d: SHAPE_STRUM %d, reads %+d", v, want[v], v);
        seq_set_vknob(&s, VK_STR, -v);
        CHECK(seq_chord_shape(&s, SHAPE_STRUM) == want[v] && seq_vknob(&s, VK_STR) == -v && s.part[0].vstrdir == (v > 0), "STRUM %+d: reads %+d", -v, -v);
    }
    seq_set_vknob(&s, VK_COL, 3);
    seq_set_vknob(&s, VK_INV, -1);
    seq_set_vknob(&s, VK_SPR, 2);
    seq_set_vknob(&s, VK_STR, -5);
    CHECK(!seq_can_undo(&s), "the knobs have no undo");
    seq_reset_chord_knobs(&s);
    CHECK(seq_vknob(&s, VK_COL) == 0 && seq_vknob(&s, VK_INV) == 0 && seq_vknob(&s, VK_SPR) == 0 && seq_vknob(&s, VK_STR) == 0 &&
          seq_chord_shape(&s, SHAPE_STRUM) == 0 && !s.part[0].vstrdir, "RESET KNOBS: COLOUR, INVERSION, SPREAD and STRUM to Pop Piano's (0)");
    seq_undo(&s);
    CHECK(seq_vknob(&s, VK_COL) == 3 && seq_vknob(&s, VK_INV) == -1 && seq_vknob(&s, VK_SPR) == 2 && seq_vknob(&s, VK_STR) == -5 &&
          !seq_can_undo(&s), "0.17.1: RESET KNOBS is one undo step");
}

/* the semitones the top note travels round the 4-chord loop */
static int top_travel(const seq_t *s) {
    const seq_part_t *p = &s->part[0];
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += abs(p->voice[i][p->voice_n[i] - 1] - p->voice[(i + 1) % 4][p->voice_n[(i + 1) % 4] - 1]);
    return sum;
}

static void test_17_vmodes(void) {
    static seq_t s;
    int ok = 1, top_s, top_t;
    seq_init(&s, emit, NULL);
    seq_set_prog_preset(&s, 0);   /* Epic: Em C G D */
    seq_set_vknob(&s, VK_COL, 1);   /* 7ths: four notes, so Rootless drops the root */
    seq_set_vmode(&s, VMODE_CLOSE);
    for (int i = 0; i < 4; i++) ok &= s.part[0].voice_n[i] == 4 && s.part[0].voice[i][0] % 12 == theory_degree_pc(4, SCALE_MINOR, s.part[0].prog[i]);
    CHECK(ok, "Close: each 7th as theory_chord_notes gives it, root in the bass");
    seq_set_vmode(&s, VMODE_ROOTLESS);
    ok = 1;
    for (int i = 0; i < 4; i++) {
        int root = theory_degree_pc(4, SCALE_MINOR, s.part[0].prog[i]);
        ok &= s.part[0].voice_n[i] == 3;
        for (int k = 0; k < s.part[0].voice_n[i]; k++) ok &= s.part[0].voice[i][k] % 12 != root;
    }
    CHECK(ok && seq_voicing(&s) == VOICING_SMOOTH, "Rootless: 3 notes a 7th chord, the root left out");
    seq_set_vknob(&s, VK_COL, 0);
    for (int i = 0; i < 4; i++) ok &= s.part[0].voice_n[i] == 3;
    CHECK(ok, "  a triad keeps its root");
    seq_set_prog_preset(&s, 14);   /* Drone: one chord, root position */
    seq_set_vknob(&s, VK_COL, 1);
    CHECK(s.part[0].voice_n[0] == 3 && s.part[0].voice[0][0] % 12 != 4, "  one chord alone: its root left out too");
    seq_set_prog_preset(&s, 0);
    seq_set_vknob(&s, VK_COL, 0);
    seq_set_vmode(&s, VMODE_SMOOTH);
    top_s = top_travel(&s);
    seq_set_vmode(&s, VMODE_TOPLINE);
    top_t = top_travel(&s);
    CHECK(top_t <= top_s && seq_voicing(&s) == VOICING_SMOOTH, "Top line moves the top note no more than Smooth does (%d against %d semitones round the loop)", top_t, top_s);
}

/* the Chords lane's voicing state, as `.vx` saves it, equal in two instances' part p */
static int same_vx(const seq_t *a, const seq_t *b, int p) {
    const seq_part_t *x = &a->part[p], *y = &b->part[p];
    return x->vstyle == y->vstyle && x->vmode == y->vmode && x->voicing == y->voicing && x->vcol == y->vcol && x->vinv == y->vinv &&
           x->vspr == y->vspr && x->vstrdir == y->vstrdir;
}

static void test_17_vx_roundtrip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|A.vx=44,1,0,0,0,0|") && strstr(buf, "|A.voi=1|A.vx="), "a fresh instance writes Pop Piano's .vx");
    s.part[0].vstyle = 5;
    seq_set_vmode(&s, VMODE_TOPLINE);
    seq_set_vknob(&s, VK_COL, 2);
    seq_set_vknob(&s, VK_INV, -1);
    seq_set_vknob(&s, VK_SPR, -1);
    seq_set_vknob(&s, VK_STR, -3);
    seq_set_slot(&s, 2, 4, 0, COL_6TH);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|A.vx=5,2,2,-1,-1,1|") && strstr(buf, "|A.voi=1|") &&
          strstr(buf, "|A.cs=") && !strstr(buf, "|B.vx="), "saves A.vx=style,mode,col,inv,spr,strdir, .voi 1 and .cs for the STRUM magnitude");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && same_vx(&s, &t, 0) && t.part[0].vstyle == 5 && t.part[0].vmode == VMODE_TOPLINE && t.part[0].vcol == 2 &&
          t.part[0].vinv == -1 && t.part[0].vspr == -1 && t.part[0].vstrdir == 1 && seq_vknob(&t, VK_STR) == -3 &&
          seq_slot_colour(&t, 2) == COL_6TH, "loads into a new instance: all six, STRUM -3, the 6th on slot 3");
    CHECK(seq_state_save(&t, again, sizeof again) > 0 && !strcmp(buf, again), "and saves back byte for byte");
    s.part[0].vstyle = -1;   /* Close on part B (.voi 0 as well), Rootless on C: each part has its own */
    seq_set_vmode(&s, VMODE_SMOOTH);
    seq_reset_chord_knobs(&s);
    seq_select_part(&s, 1);
    seq_set_vmode(&s, VMODE_CLOSE);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|B.voi=0|") && !strstr(buf, ".vx="), "Close alone is .voi's: no .vx (a 0.8 close part saves as it did)");
    seq_set_vmode(&s, VMODE_ROOTLESS);
    seq_set_vknob(&s, VK_SPR, 3);
    seq_select_part(&s, 5);
    seq_set_vmode(&s, VMODE_CLOSE);
    seq_set_vknob(&s, VK_INV, 3);
    seq_select_part(&s, 0);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && !strstr(buf, "|A.vx=") && strstr(buf, "|B.vx=-1,3,0,0,3,0|") && strstr(buf, "|F.vx=-1,0,0,3,0,0|"),
          "parts B and F save their own .vx, A (all defaults) none");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_part_used(&t, 1) && seq_part_used(&t, 5) && same_vx(&s, &t, 0) && same_vx(&s, &t, 1) && same_vx(&s, &t, 5) &&
          t.part[1].vmode == VMODE_ROOTLESS && t.part[5].vinv == 3, "parts B and F load their own");
}

/* a bad .vx is ignored whole: the part keeps what .voi says and the defaults, and the load is never refused */
static void test_17_vx_bad(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], bad[SEQ_STATE_MAX];
    static const char *const BAD[] = {"99,9,9,9,9,9", "-1,1,0,0,0", "-1,1,0,0,0,0,0", "-1,1,5,0,0,0", "-1,1,0,-3,0,0", "-1,1,0,0,4,0", "-1,1,0,0,0,2",
                                      "52,1,0,0,0,0", "-2,1,0,0,0,0", "-1,4,0,0,0,0", "-1,1,0,0,0,x", "-1,,0,0,0,0", "", "5,2,1,1,1,1,"};
    seq_init(&s, emit, NULL);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|A.voi=1|"), "a fresh state saves");
    strip_field(buf, "vx");   /* so the bad one is the part's only */
    s.part[0].vstyle = -1;   /* what a part without .vx loads as */
    for (int i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++) {
        char *at = strstr(buf, "|A.voi=1|") + strlen("|A.voi=1");
        snprintf(bad, sizeof bad, "%.*s|A.vx=%s%s", (int)(at - buf), buf, BAD[i], at);
        seq_init(&t, emit, NULL);
        seq_set_vknob(&t, VK_INV, 2);   /* a load replaces it */
        CHECK(seq_state_load(&t, bad) && same_vx(&s, &t, 0), "a bad .vx=%s loads with the defaults", BAD[i]);
    }
}

/* Review Focus 1: a 0.16 state (its .cs row, no .vx) loads, takes .voi's mode and the knobs' neutral values, and plays */
static void test_17_old_state_loads(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    static const char CS[] = "|A.cs=40,51,0,50,40,50,0", VOI[] = "|A.voi=1";
    char *at;
    seq_init(&s, emit, NULL);
    seq_select_part(&s, 1);
    seq_select_part(&s, 0);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, VOI) && strstr(buf, "|B.voi=1") && strstr(buf, ".vx="), "a new instance saves, with parts A and B");
    strip_field(buf, "vx");   /* 0.16 wrote none */
    at = strstr(buf, VOI) + strlen(VOI);
    memmove(at + strlen(CS), at, strlen(at) + 1);   /* 0.16 wrote the .cs row of seven values */
    memcpy(at, CS, strlen(CS));
    seq_init(&s, emit, NULL);
    CHECK(seq_state_load(&s, buf), "a 0.16 state loads");
    CHECK(s.part[0].vstyle == -1 && s.part[0].vmode == VMODE_SMOOTH && s.part[0].vcol == 0 && s.part[0].vinv == 0 && s.part[0].vspr == 0 &&
          s.part[0].vstrdir == 0 && seq_chord_shape(&s, SHAPE_STRUM) == 40, "part A: Custom, Smooth from .voi, knobs 0, STRUM from .cs");
    nev = 0;
    play_stop(&s, 16.0);
    CHECK(nev > 0 && balanced(), "it plays 4 bars, every note off");
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && !strstr(buf, "|A.vx=") && seq_state_load(&s, buf), "and saves and loads again without a .vx");
}

/* 0.17 the 52 voicing styles: 13 genres of 4, every value in range, every name fits the 238 px tile at 24 px */
static void test_17_styles_table(void) {
    int ok = 1, fits = 1, seen[VMODE_COUNT] = {0};
    CHECK(VOICING_NSTYLES == 52 && VOICING_NGENRES == 13, "52 styles in 13 genres (got %d in %d)", VOICING_NSTYLES, VOICING_NGENRES);
    for (int i = 0; i < VOICING_NSTYLES; i++) {
        const voicing_style_t *v = &VOICING_STYLES[i];
        ok &= v->genre && v->name && v->feel && v->mode >= 0 && v->mode < VMODE_COUNT && v->col >= -1 && v->col <= 4 && v->inv >= -2 &&
              v->inv <= 3 && v->spr >= -1 && v->spr <= 3 && v->str >= -8 && v->str <= 8 && v->oct >= -1 && v->oct <= 1;
        ok &= !strcmp(v->genre, VOICING_STYLES[i / 4 * 4].genre);   /* four to a genre */
        if (ok) seen[(int)v->mode] = 1;
        if (!v->name || theory_text_width(v->name) > 14) fits = 0;
    }
    CHECK(ok, "every style has its texts, a connection mode and values in range, four to a genre");
    CHECK(fits, "every name is at most 14 characters");
    CHECK(seen[VMODE_CLOSE] && seen[VMODE_SMOOTH] && seen[VMODE_TOPLINE] && seen[VMODE_ROOTLESS], "all four connection modes are used");
    CHECK(!strcmp(VOICING_STYLES[44].genre, "Pop") && !strcmp(VOICING_STYLES[44].name, "Pop Piano") && VOICING_DEFAULT == 44,
          "Pop Piano is the default, 44");
    CHECK(!strcmp(VOICING_STYLES[0].genre, "House") && !strcmp(VOICING_STYLES[51].name, "Music Box") && !strcmp(VOICING_STYLES[24].name, "Lo-fi Keys") &&
          !strcmp(VOICING_STYLES[16].name, "Wide 9ths") && !strcmp(VOICING_STYLES[18].name, "Supersaws"), "genre-major order, Supersaws renamed");
}

static void test_17_default_style(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    CHECK(seq_voicing_style(&s) == VOICING_DEFAULT && seq_voicing_edited(&s) == 0 && seq_voicing(&s) == VOICING_SMOOTH, "a new instance is Pop Piano, not edited, Smooth");
    CHECK(s.part[0].vmode == VMODE_SMOOTH && s.part[3].vstyle == VOICING_DEFAULT && seq_vknob(&s, VK_COL) == 0 && seq_vknob(&s, VK_STR) == 0 &&
          seq_chord_shape(&s, SHAPE_OCT) == 0, "every part starts there, all knobs 0, octave 0");
    seq_set_voicing_style(&s, 52);
    seq_set_voicing_style(&s, -1);
    CHECK(seq_voicing_style(&s) == VOICING_DEFAULT, "a bad style index is ignored");
    seq_set_vknob(&s, VK_SPR, 2);
    CHECK(seq_voicing_style(&s) == VOICING_DEFAULT && seq_voicing_edited(&s) == 1, "turning a knob keeps the style and reads edited");
    s.part[0].vstyle = -1;
    CHECK(seq_voicing_edited(&s) == 0, "Custom is never edited");
}

static void test_17_style_apply_edited(void) {
    static seq_t s;
    const int lofi = 24;
    seq_init(&s, emit, NULL);
    seq_set_voicing_style(&s, lofi);
    CHECK(seq_voicing_style(&s) == lofi && s.part[0].vmode == VMODE_SMOOTH && seq_vknob(&s, VK_COL) == 1 && seq_vknob(&s, VK_INV) == 0 &&
          seq_vknob(&s, VK_SPR) == 1 && seq_vknob(&s, VK_STR) == 3 && seq_chord_shape(&s, SHAPE_OCT) == 0 && seq_voicing(&s) == VOICING_SMOOTH,
          "Lo-fi Keys: Smooth, colour +1, inversion 0, spread +1, strum +3");
    CHECK(seq_chord_shape(&s, SHAPE_STRUM) == 38 && s.part[0].vstrdir == 0, "STRUM +3 is SHAPE_STRUM 37.5 rounded, low to high");
    CHECK(seq_voicing_edited(&s) == 0, "a style as applied is not edited");
    seq_set_vknob(&s, VK_INV, 1);
    CHECK(seq_voicing_edited(&s) == 1 && seq_voicing_style(&s) == lofi, "turning INVERSION reads edited, the style stays");
    seq_set_vknob(&s, VK_INV, 0);
    CHECK(seq_voicing_edited(&s) == 0, "turned back, it is not");
    seq_set_vmode(&s, VMODE_CLOSE);
    CHECK(seq_voicing_edited(&s) == 1, "the connection mode counts");
    seq_set_voicing_style(&s, lofi);
    CHECK(seq_voicing_edited(&s) == 0 && s.part[0].vmode == VMODE_SMOOTH, "applying it again restores the style");
    seq_set_chord_shape(&s, SHAPE_OCT, 1);
    CHECK(seq_voicing_edited(&s) == 1, "so does the octave");
    seq_set_voicing_style(&s, lofi);
    seq_set_vknob(&s, VK_SPR, 3);
    seq_reset_chord_knobs(&s);
    CHECK(seq_voicing_edited(&s) == 0 && seq_voicing_style(&s) == lofi && seq_vknob(&s, VK_COL) == 1,
          "0.17.1: RESET KNOBS puts Lo-fi's own knobs back (COLOUR +1): not edited");
    seq_set_voicing_style(&s, 36);   /* Dusty Keys (Boom Bap): STRUM +2 */
    CHECK(seq_vknob(&s, VK_STR) == 2 && seq_voicing_edited(&s) == 0, "another style sets the knobs it names");
}

static void test_17_style_strum_octave(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_voicing_style(&s, 26);   /* Tape Pad: STRUM -2, high to low */
    CHECK(seq_vknob(&s, VK_STR) == -2 && s.part[0].vstrdir == 1 && seq_voicing_edited(&s) == 0, "a negative strum sets the direction");
    seq_set_voicing_style(&s, 4);   /* Minor 7 Stab: no strum */
    CHECK(seq_vknob(&s, VK_STR) == 0 && s.part[0].vstrdir == 0 && seq_chord_shape(&s, SHAPE_STRUM) == 0, "and the next style's 0 clears it");
    seq_set_voicing_style(&s, 0);   /* Piano House: octave +1 */
    CHECK(seq_chord_shape(&s, SHAPE_OCT) == 1 && seq_voicing_edited(&s) == 0, "a style sets the Chords octave");
    seq_set_voicing_style(&s, 7);   /* Warm Pad: octave 0 */
    CHECK(seq_chord_shape(&s, SHAPE_OCT) == 0, "and the next one resets it");
}

/* 0.17.1 (owner): RESET KNOBS puts all 8 Chords knobs to a fresh start: the VOICING style's own COLOUR, INVERSION, SPREAD, STRUM
 * (and octave), a new instance's COMPLEXITY, PUSH, GATE and DYNAMICS; one undo step brings all 8 back; nothing to reset, no step */
static void test_171_reset_knobs(void) {
    static seq_t s;
    seq_lane_t was;
    const seq_part_t *p = &s.part[0];
    seq_init(&s, emit, NULL);
    seq_reset_chord_knobs(&s);
    CHECK(!seq_can_undo(&s), "a new instance: nothing to reset, no undo step");
    seq_set_voicing_style(&s, 0);   /* Piano House: octave +1 */
    seq_set_vknob(&s, VK_INV, 2);
    seq_set_vknob(&s, VK_STR, 4);
    seq_set_chord_shape(&s, SHAPE_OCT, -1);
    seq_set_complexity(&s, LANE_CHORDS, 80);
    seq_set_chord_shape(&s, SHAPE_PUSH, 40);
    seq_set_chord_shape(&s, SHAPE_GATE, 10);
    seq_set_chord_shape(&s, SHAPE_DYN, 90);
    was = p->lane[LANE_CHORDS];
    seq_reset_chord_knobs(&s);
    CHECK(seq_voicing_style(&s) == 0 && !seq_voicing_edited(&s) && seq_vknob(&s, VK_INV) == 0 && seq_vknob(&s, VK_STR) == 0 &&
          seq_chord_shape(&s, SHAPE_OCT) == 1, "the style's own knobs and octave back: Piano House, not edited");
    CHECK(p->lane[LANE_CHORDS].gen.complexity == 50 && seq_chord_shape(&s, SHAPE_PUSH) == 0 && seq_chord_shape(&s, SHAPE_GATE) == 50 &&
          seq_chord_shape(&s, SHAPE_DYN) == 50, "COMPLEXITY 50 (0.18), PUSH 0, GATE 50, DYNAMICS 50: a new instance's");
    seq_undo(&s);
    CHECK(seq_vknob(&s, VK_INV) == 2 && seq_vknob(&s, VK_STR) == 4 && seq_chord_shape(&s, SHAPE_OCT) == -1 && seq_voicing_edited(&s) &&
          p->lane[LANE_CHORDS].gen.complexity == 80 && seq_chord_shape(&s, SHAPE_PUSH) == 40 && seq_chord_shape(&s, SHAPE_GATE) == 10 &&
          seq_chord_shape(&s, SHAPE_DYN) == 90 && !memcmp(p->lane[LANE_CHORDS].steps, was.steps, sizeof was.steps),
          "one UNDO brings all 8 back, and the chord rhythm with COMPLEXITY");
    seq_undo(&s);
    CHECK(seq_voicing_style(&s) == 44, "the step before it is the style's (Pop Piano before Piano House)");
    s.part[0].vstyle = -1;   /* Custom: the four voicing knobs to 0 */
    seq_set_vknob(&s, VK_COL, 2);
    seq_reset_chord_knobs(&s);
    CHECK(seq_voicing_style(&s) == -1 && seq_vknob(&s, VK_COL) == 0, "Custom: COLOUR back to 0");
}

static void test_17_style_undo(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_vknob(&s, VK_INV, 2);   /* an edit of Pop Piano */
    seq_set_voicing_style(&s, 0);   /* Piano House: Close, colour +1, octave +1 */
    CHECK(seq_voicing_style(&s) == 0 && s.part[0].vmode == VMODE_CLOSE && seq_vknob(&s, VK_INV) == 0, "the style is applied");
    seq_undo(&s);
    CHECK(seq_voicing_style(&s) == VOICING_DEFAULT && s.part[0].vmode == VMODE_SMOOTH && seq_vknob(&s, VK_INV) == 2 && seq_vknob(&s, VK_COL) == 0 &&
          seq_chord_shape(&s, SHAPE_OCT) == 0 && seq_voicing(&s) == VOICING_SMOOTH && seq_voicing_edited(&s) == 1, "one UNDO restores the style and every knob");
    seq_set_voicing_style(&s, 24);
    seq_set_voicing_style(&s, 26);
    seq_undo(&s);
    CHECK(seq_voicing_style(&s) == 24, "each application is one step");
    seq_set_slot(&s, 1, 4, 0, COL_6TH);
    seq_set_vknob(&s, VK_COL, 3);
    seq_undo(&s);   /* the slot edit */
    CHECK(seq_vknob(&s, VK_COL) == 3 && seq_slot_colour(&s, 1) != COL_6TH, "undoing a slot edit leaves the knobs as they are");
}

static void test_17_style_saves(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_set_voicing_style(&s, 26);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|A.vx=26,1,1,0,3,1|"), "a style part writes .vx");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_voicing_style(&t) == 26 && seq_vknob(&t, VK_STR) == -2 && seq_voicing_edited(&t) == 0 &&
          seq_state_save(&t, again, sizeof again) > 0 && !strcmp(buf, again), "a style loads and saves back byte for byte, not edited");
}

/* 0.17 the chord picker and the PROGRESSIONS / VOICING browsers */

/* a slot's chord, colour and lock as one comparable number */
static int slot_key(const seq_t *s, int i) {
    return seq_slot_deg(s, i) | seq_slot_src(s, i) << 4 | seq_slot_colour(s, i) << 8 | seq_slot_locked(s, i) << 16;
}

static void test_17_knobs_lock(void) {   /* 0.24.1 the FIX picker's part left with it */
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_vknob(&s, VK_COL, 2);   /* 9th */
    seq_set_slot(&s, 2, 4, 0, COL_TRIAD);
    CHECK(seq_slot_colour(&s, 2) == COL_9TH && seq_slot_locked(&s, 2), "a Knobs set writes the knob's level as its own colour");
    seq_set_vknob(&s, VK_COL, 0);
    CHECK(seq_part_eff_colour(&s, 0, 2) == COL_9TH, "so it stays when the knob moves");
    seq_toggle_slot_lock(&s, 2);
    CHECK(!seq_slot_locked(&s, 2) && seq_slot_colour(&s, 2) == COL_9TH, "unlocking leaves it");
    seq_set_vknob(&s, VK_COL, 1);   /* 7th */
    seq_toggle_slot_lock(&s, 2);
    CHECK(seq_slot_locked(&s, 2) && seq_slot_colour(&s, 2) == COL_9TH, "a locked chord with its own colour keeps it");
    seq_toggle_slot_lock(&s, 3);
    CHECK(seq_slot_locked(&s, 3) && seq_slot_colour(&s, 3) == COL_7TH, "seq_toggle_slot_lock on a Knobs chord writes the level");
    seq_toggle_slot_lock(&s, 3);
    CHECK(!seq_slot_locked(&s, 3) && seq_slot_colour(&s, 3) == COL_7TH, "and unlocking leaves it");
    seq_set_vknob(&s, VK_COL, 3);   /* 11th */
    seq_set_slot(&s, 0, 4, 0, COL_TRIAD);
    CHECK(seq_slot_colour(&s, 0) == COL_11TH && seq_slot_locked(&s, 0), "seq_set_slot (FIX) with Knobs writes the level");
    seq_set_slot(&s, 1, 4, 0, COL_SUS2);
    CHECK(seq_slot_colour(&s, 1) == COL_SUS2, "an own colour is kept");
    seq_set_vknob(&s, VK_COL, 0);
    seq_set_slot(&s, 1, 4, 0, COL_TRIAD);
    CHECK(seq_slot_colour(&s, 1) == COL_TRIAD && seq_slot_locked(&s, 1), "at level Triad a Knobs lock stays Triad");
    seq_set_vknob(&s, VK_COL, -1);
    seq_toggle_slot_lock(&s, 1);
    seq_toggle_slot_lock(&s, 1);
    CHECK(seq_slot_colour(&s, 1) == COL_POWER, "Power");
}

/* Review Focus 2: a full undo ring (0.24.1 the FIX picker's wrap left with it) */
static void test_17_cancel_on_full_ring(void) {
    static seq_t s;
    static seq_snap_t ring[SEQ_UNDO];
    int top;
    seq_init(&s, emit, NULL);
    for (int i = 0; i < 11; i++) seq_set_slot(&s, i % 4, i % 7, 0, i % 3 ? COL_TRIAD : COL_7TH);
    CHECK(s.undo_n == SEQ_UNDO, "the ring is full");
    memcpy(ring, s.undo, sizeof ring);
    top = s.undo_top;
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 3);
    seq_browse_cancel(&s);
    CHECK(s.undo_n == SEQ_UNDO && s.undo_top == top && !memcmp(ring, s.undo, sizeof ring), "a browser open on a full ring, cancelled: the same");
    for (int i = 0; i < SEQ_UNDO; i++) seq_undo(&s);
    CHECK(!seq_can_undo(&s), "and the 8 steps undo");
}

static void test_17_prog_try_keeps_locks(void) {
    static seq_t s;
    int before[4], phrase, rank, filled, top;
    seq_init(&s, emit, NULL);
    seq_set_prog_preset(&s, 1);   /* Pop 1 5 6 4 */
    seq_toggle_slot_lock(&s, 2);
    for (int i = 0; i < 4; i++) before[i] = slot_key(&s, i);
    phrase = s.part[0].phrase, rank = s.part[0].rank_len, filled = s.part[0].filled_slots, top = s.undo_top;
    CHECK(seq_prog_preset_shown(&s) == 1, "Pop is shown");
    s.naud = 0;
    seq_prog_try(&s, 2);
    CHECK(seq_slot_deg(&s, 0) == 0 && seq_slot_deg(&s, 1) == 4 && s.naud == 0, "a try outside a browser does nothing");
    seq_browse_open(&s, CVIEW_PROG);
    CHECK(seq_cview(&s) == CVIEW_PROG && s.undo_n == 1, "open: the PROGRESSIONS view, one undo snapshot");
    s.naud = 0;
    seq_prog_try(&s, 2);   /* Sad Pop 6 4 1 5 */
    CHECK(s.naud == s.part[0].voice_n[0] && s.naud >= 3, "a tap plays the first unlocked slot's new chord (%d notes)", s.naud);
    CHECK(seq_slot_deg(&s, 0) == 5 && seq_slot_deg(&s, 1) == 3 && seq_slot_deg(&s, 3) == 4, "the unlocked slots are Sad Pop's");
    CHECK(seq_slot_deg(&s, 2) == 5 && seq_slot_locked(&s, 2), "the locked slot stays");
    CHECK(seq_prog_preset_shown(&s) == 2 && seq_prog_preset(&s) == -1, "the unlocked slots show Sad Pop; the loop is Custom");
    CHECK(s.undo_n == 1 && s.part[0].rank_len == 4 && s.part[0].phrase == 4, "a try pushes no undo step");
    seq_prog_try(&s, 0);
    CHECK(seq_slot_deg(&s, 0) == 0 && seq_slot_deg(&s, 1) == 5 && seq_slot_deg(&s, 3) == 6 && seq_slot_deg(&s, 2) == 5, "another try");
    seq_prog_try(&s, 16);
    seq_prog_try(&s, -1);
    CHECK(seq_slot_deg(&s, 0) == 0 && seq_slot_deg(&s, 1) == 5, "a bad preset is ignored");
    seq_browse_cancel(&s);
    for (int i = 0; i < 4; i++) CHECK(slot_key(&s, i) == before[i], "cancel: slot %d as before", i);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && s.part[0].phrase == phrase && s.part[0].rank_len == rank && s.part[0].filled_slots == filled &&
              s.undo_n == 0 && s.undo_top == top && seq_prog_preset_shown(&s) == 1,
          "and the phrase, the ranks, the filled slots and the undo ring");
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    seq_prog_try(&s, 3);
    seq_browse_done(&s);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && seq_slot_deg(&s, 0) == 0 && seq_slot_deg(&s, 1) == 5 && s.undo_n == 1, "DONE keeps it, as one undo step");
    seq_undo(&s);
    for (int i = 0; i < 4; i++) CHECK(slot_key(&s, i) == before[i], "one UNDO: slot %d back to before the browser", i);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 4);
    seq_prog_try(&s, 1);
    seq_browse_done(&s);
    CHECK(s.undo_n == 0, "DONE with nothing changed leaves no undo step");
    seq_undo(&s);
    seq_set_prog_preset(&s, 1);
    CHECK(seq_prog_preset_shown(&s) == 1, "all-unlocked Pop");
    s.part[0].colour[1] = COL_7TH;
    CHECK(seq_prog_preset_shown(&s) == -1, "a coloured unlocked slot: none");
}

static void test_17_voice_try_cancel(void) {
    static seq_t s;
    static seq_part_t before;
    int top;
    seq_init(&s, emit, NULL);
    seq_set_vknob(&s, VK_INV, 2);
    before = s.part[0];
    top = s.undo_top;
    CHECK(seq_browse_genre(&s) == 0, "the chip starts at 0");
    s.naud = 0;
    seq_voice_try(&s, 26);
    CHECK(seq_voicing_style(&s) == VOICING_DEFAULT && s.naud == 0, "a try outside a browser does nothing");
    seq_browse_open(&s, CVIEW_VOICE);
    CHECK(seq_cview(&s) == CVIEW_VOICE && s.undo_n == 1 && seq_browse_genre(&s) == VOICING_DEFAULT / 4, "open: the VOICING view on the style's genre");
    seq_set_browse_genre(&s, 3);
    seq_set_browse_genre(&s, 13);
    seq_set_browse_genre(&s, -1);
    CHECK(seq_browse_genre(&s) == 3, "the chip moves; a bad one is ignored");
    s.naud = 0;
    seq_voice_try(&s, 26);
    CHECK(s.naud >= 3, "a tap plays the first chord in the new voicing (%d notes)", s.naud);
    CHECK(seq_voicing_style(&s) == 26 && seq_vknob(&s, VK_INV) == 0 && seq_vknob(&s, VK_STR) == -2 && s.undo_n == 1, "a try applies the style with no undo step");
    seq_voice_try(&s, 0);
    seq_voice_try(&s, 52);
    CHECK(seq_voicing_style(&s) == 0, "another; a bad style is ignored");
    seq_browse_cancel(&s);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && seq_voicing_style(&s) == VOICING_DEFAULT && seq_vknob(&s, VK_INV) == 2 && seq_voicing_edited(&s) == 1 &&
              !memcmp(&s.part[0].lane_shape, &before.lane_shape, sizeof before.lane_shape) && s.part[0].vmode == before.vmode &&
              s.part[0].vcol == before.vcol && s.part[0].vspr == before.vspr && s.part[0].vstrdir == before.vstrdir,
          "cancel puts the style and every knob back");
    CHECK(s.undo_n == 0 && s.undo_top == top, "and the undo ring");
    seq_browse_open(&s, CVIEW_VOICE);
    seq_voice_try(&s, 24);
    seq_voice_try(&s, 26);
    seq_browse_done(&s);
    CHECK(seq_voicing_style(&s) == 26 && s.undo_n == 1, "DONE keeps it as one step");
    seq_undo(&s);
    CHECK(seq_voicing_style(&s) == VOICING_DEFAULT && seq_vknob(&s, VK_INV) == 2 && seq_voicing_edited(&s) == 1, "and one UNDO goes back to before");
    seq_browse_open(&s, CVIEW_VOICE);
    seq_voice_try(&s, 26);
    seq_voice_try(&s, VOICING_DEFAULT);
    seq_voice_try(&s, 26);
    seq_voice_try(&s, 0);
    seq_voice_try(&s, VOICING_DEFAULT);
    seq_voice_try(&s, 26);
    seq_voice_try(&s, VOICING_DEFAULT);
    seq_browse_done(&s);
    CHECK(seq_voicing_style(&s) == VOICING_DEFAULT && seq_vknob(&s, VK_INV) == 0 && s.undo_n == 1, "back to the default style: its own knobs, not the edit");
    seq_undo(&s);
    s.part[0].vstyle = -1;
    seq_set_browse_genre(&s, 5);
    seq_browse_open(&s, CVIEW_VOICE);
    CHECK(seq_browse_genre(&s) == 5, "a Custom voicing keeps the chip where it was");
    seq_browse_cancel(&s);
}

/* Review Focus 5: a part switch, a load, INIT and UNDO close a transaction as cancel; a queued switch only flags it */
static void test_17_browser_closed_by_switch(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    int before[4];
    double ppq;
    seq_init(&s, emit, NULL);
    for (int i = 0; i < 4; i++) before[i] = slot_key(&s, i);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    seq_select_part(&s, 1);
    CHECK(seq_part(&s) == 1 && seq_cview(&s) == CVIEW_CHORDS, "stopped: the switch closes the browser");
    seq_select_part(&s, 0);
    for (int i = 0; i < 4; i++) CHECK(slot_key(&s, i) == before[i], "and cancelled it: slot %d", i);
    CHECK(s.undo_n == 0, "undo ring as at open");
    /* playing: queue, open, the bar line switches on the audio thread */
    seq_init(&s, emit, NULL);
    ev_seq = &s;
    ppq = play(&s, 0.0, 0.5, 0);
    seq_select_part(&s, 1);
    CHECK(seq_queued_part(&s) == 1, "part 2 is queued");
    {
        int prog0[SEQ_MAX_PROG], prog1[SEQ_MAX_PROG];
        memcpy(prog0, s.part[0].prog, sizeof prog0);
        seq_browse_open(&s, CVIEW_PROG);   /* 0.24.1 the FIX picker is gone: the browser is the CHORDS transaction */
        seq_prog_try(&s, 2);
        CHECK(seq_cview(&s) == CVIEW_PROG, "the browser opens while the switch waits");
        ppq = play(&s, ppq, 4.0, 0);
        CHECK(seq_part(&s) == 1 && seq_cview(&s) == CVIEW_CHORDS, "after the bar line the getters read it closed");
        CHECK(memcmp(s.part[0].prog, prog0, sizeof prog0) && s.txn.kind == CVIEW_PROG && s.txn.stale, "(the old part still holds the try until a setter)");
        memcpy(prog1, s.part[1].prog, sizeof prog1);
        seq_prog_try(&s, 3);
        CHECK(!memcmp(s.part[0].prog, prog0, sizeof prog0) && s.txn.kind == CVIEW_CHORDS, "the next browser setter cancels it on the old part");
        CHECK(seq_part(&s) == 1 && !memcmp(s.part[1].prog, prog1, sizeof prog1), "and does not touch the playing part");
    }
    ev_seq = NULL;
    /* a stop starts the queued part: the same */
    seq_init(&s, emit, NULL);
    ev_seq = &s;
    ppq = play(&s, 0.0, 0.5, 0);
    seq_select_part(&s, 2);
    seq_browse_open(&s, CVIEW_VOICE);
    seq_voice_try(&s, 26);
    idle(&s, 2, bpm);
    CHECK(seq_part(&s) == 2 && seq_cview(&s) == CVIEW_CHORDS && s.part[0].vstyle == 26, "a stop switched it too");
    seq_set_cview(&s, CVIEW_JAM);   /* 0.24 FIX is retired (seq_set_cview refuses it): another user view */
    CHECK(s.part[0].vstyle == VOICING_DEFAULT && seq_cview(&s) == CVIEW_JAM, "the CVIEW setter cancels it");
    ev_seq = NULL;
    /* a load, INIT and UNDO */
    seq_init(&t, emit, NULL);
    seq_state_save(&t, buf, sizeof buf);
    seq_init(&s, emit, NULL);
    seq_set_slot(&s, 0, 2, 0, COL_TRIAD);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    CHECK(seq_state_load(&s, buf) && seq_cview(&s) == CVIEW_CHORDS && s.txn.kind == CVIEW_CHORDS, "a load closes the browser");
    seq_browse_open(&s, CVIEW_VOICE);
    seq_voice_try(&s, 0);
    seq_init_preset(&s);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && s.txn.kind == CVIEW_CHORDS && s.undo_n == 0, "INIT closes the browser");
    seq_set_slot(&s, 0, 2, 0, COL_TRIAD);
    seq_set_slot(&s, 1, 3, 0, COL_TRIAD);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    seq_undo(&s);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && seq_slot_deg(&s, 0) == 2 && seq_slot_deg(&s, 1) == 3 && s.undo_n == 2, "UNDO with a browser open is its cancel");
    seq_undo(&s);
    CHECK(seq_slot_deg(&s, 1) != 3, "and the next UNDO is the step before");
}

static void test_17_transactions_exclusive(void) {
    static seq_t s;
    int before[4];
    seq_init(&s, emit, NULL);
    for (int i = 0; i < 4; i++) before[i] = slot_key(&s, i);
    seq_browse_open(&s, CVIEW_PROG);
    CHECK(seq_cview(&s) == CVIEW_PROG && s.undo_n == 1, "PROGRESSIONS opens");
    seq_prog_try(&s, 3);
    seq_browse_open(&s, CVIEW_VOICE);
    CHECK(seq_cview(&s) == CVIEW_VOICE && slot_key(&s, 0) == before[0] && seq_slot_deg(&s, 1) == before[1] % 16 && s.undo_n == 1, "the VOICING browser cancels PROGRESSIONS");
    seq_voice_try(&s, 3);
    seq_browse_open(&s, CVIEW_FIX);
    seq_browse_open(&s, CVIEW_PICK);   /* 0.24.1 retired kinds */
    CHECK(seq_cview(&s) == CVIEW_VOICE && seq_voicing_style(&s) == 3, "a bad browser kind opens nothing");
    seq_browse_open(&s, CVIEW_PROG);
    CHECK(seq_cview(&s) == CVIEW_PROG && seq_voicing_style(&s) == VOICING_DEFAULT && s.undo_n == 1, "PROGRESSIONS cancels VOICING");
    seq_browse_done(&s);
    seq_set_cview(&s, CVIEW_RHYTHM);
    CHECK(seq_cview(&s) == CVIEW_RHYTHM, "the user's view");
    seq_set_cview(&s, CVIEW_PICK);
    seq_set_cview(&s, CVIEW_PROG);
    seq_set_cview(&s, CVIEW_JAM + 1);
    seq_set_cview(&s, CVIEW_FIX);   /* 0.24 retired */
    CHECK(seq_cview(&s) == CVIEW_RHYTHM, "only CHORDS and RHYTHM (0.22: and JAM; 0.24 FIX is refused) from the user");
    seq_set_cview(&s, CVIEW_JAM);
    CHECK(seq_cview(&s) == CVIEW_JAM, "0.22 JAM is the user's");
    seq_set_cview(&s, CVIEW_RHYTHM);
    seq_browse_open(&s, CVIEW_VOICE);
    CHECK(seq_cview(&s) == CVIEW_VOICE, "an open browser shows");
    seq_set_cview(&s, CVIEW_CHORDS);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && s.txn.kind == CVIEW_CHORDS, "a view change cancels it");
}

/* a stale transaction: a queued switch took the part away; the getters read it closed, UNDO is a real undo, the cancelled
 * browser's step is not left in the ring (0.24.1 the browser: the FIX picker is gone) */
static void test_17_stale_cancel(void) {
    static seq_t s;
    int deg0, deg2, n;
    double ppq;
    for (int how = 0; how < 2; how++) {
        seq_init(&s, emit, NULL);
        ev_seq = &s;
        deg0 = seq_slot_deg(&s, 0), deg2 = seq_slot_deg(&s, 2);
        seq_set_slot(&s, 0, (deg0 + 1) % 7, 0, COL_TRIAD);   /* the step before the browser */
        n = s.undo_n;
        ppq = play(&s, 0.0, 0.5, 0);
        seq_select_part(&s, 1);
        seq_browse_open(&s, CVIEW_PROG);
        seq_prog_try(&s, 2);
        seq_prog_try(&s, 3);
        CHECK(s.undo_n == n + 1, "its open, one step; the tries none");
        ppq = play(&s, ppq, 4.0, 0);
        ev_seq = NULL;
        CHECK(seq_part(&s) == 1 && seq_cview(&s) == CVIEW_CHORDS && s.txn.kind == CVIEW_PROG && s.txn.stale, "the switch left it stale");
        if (how == 0) {
            seq_undo(&s);   /* not swallowed: the cancel, then the real undo */
            CHECK(s.txn.kind == CVIEW_CHORDS && s.part[0].prog[2] == deg2, "UNDO cancels the stale browser: the old part's slot is as before");
            CHECK(s.part[0].prog[0] == deg0 && s.undo_n == 0, "and goes on to undo the step before it");
        } else {
            seq_set_slot(&s, 1, 3, 0, COL_TRIAD);   /* the playing part's own edit while it waits */
            CHECK(s.undo_n == n + 2, "(an edit of part 2 on top)");
            seq_prog_try(&s, 1);   /* any browser setter cancels it */
            CHECK(s.txn.kind == CVIEW_CHORDS && s.part[0].prog[2] == deg2, "a setter cancels it: the slot back on the old part");
            CHECK(s.undo_n == n + 1 && s.undo[(s.undo_top + SEQ_UNDO - 1) % SEQ_UNDO].part == 1, "its step left the ring, part 2's stayed");
            seq_undo(&s);
            CHECK(s.part[1].prog[1] != 3 || seq_slot_locked(&s, 1) == 0, "UNDO undoes part 2's edit first");
            seq_select_part(&s, 0);
            seq_undo(&s);
            CHECK(s.part[0].prog[0] == deg0 && s.part[0].prog[2] == deg2 && s.undo_n == 0, "then the step before the browser, never a cancelled one");
        }
    }
}

/* a save writes what the transaction found at open, not its tries */
static void test_17_save_hides_tries(void) {
    static seq_t s, t;
    static char before[SEQ_STATE_MAX], during[SEQ_STATE_MAX], after[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_set_prog_preset(&s, 1);
    seq_toggle_slot_lock(&s, 1);
    seq_state_save(&s, before, sizeof before);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    CHECK(seq_state_save(&s, during, sizeof during) > 0 && !strcmp(before, during), "PROGRESSIONS open: the save is the progression before it");
    CHECK(seq_slot_deg(&s, 0) == 5 && seq_prog_preset_shown(&s) == 2, "and the tried progression is still there");
    seq_browse_done(&s);
    seq_state_save(&s, after, sizeof after);
    CHECK(strcmp(before, after), "after DONE it is saved");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, after) && seq_slot_deg(&t, 0) == 5, "(and loads)");
    seq_state_save(&s, before, sizeof before);
    seq_browse_open(&s, CVIEW_VOICE);
    seq_voice_try(&s, 26);
    CHECK(seq_state_save(&s, during, sizeof during) > 0 && !strcmp(before, during) && seq_voicing_style(&s) == 26, "VOICING open: the save is the voicing before it");
    seq_browse_done(&s);
    seq_state_save(&s, after, sizeof after);
    CHECK(strcmp(before, after) && strstr(after, ".vx=26,"), "after DONE it is saved");
    seq_state_save(&s, before, sizeof before);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 3);
    seq_state_save(&s, during, sizeof during);
    seq_browse_cancel(&s);
    seq_state_save(&s, after, sizeof after);
    CHECK(!strcmp(before, during) && !strcmp(before, after), "and after a cancel");
}

static void test_17_init_style(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_voicing_style(&s, 0);
    seq_init_preset(&s);
    CHECK(!strcmp(VOICING_STYLES[48].name, "As Written") && VOICING_STYLES[48].mode == VMODE_CLOSE, "style 48 is Film - As Written, Close");
    CHECK(seq_voicing_style(&s) == 48 && seq_voicing_edited(&s) == 0 && seq_voicing(&s) == VOICING_CLOSE && s.part[3].vstyle == 48,
          "INIT: every part is a named style (Close, all 0), not Custom");
}

static void test_17_resend_and_bad_load(void) {
    static seq_t s;
    int n;
    seq_init(&s, emit, NULL);
    seq_set_slot(&s, 1, 3, 0, COL_TRIAD);   /* knob at Triad: locked Triad */
    n = s.undo_n;
    seq_set_vknob(&s, VK_COL, 2);
    seq_set_slot(&s, 1, 3, 0, COL_TRIAD);
    CHECK(seq_slot_colour(&s, 1) == COL_TRIAD && s.undo_n == n, "FIX re-sending a locked Triad slot leaves it and pushes nothing");
    seq_set_slot(&s, 2, 4, 0, COL_TRIAD);
    n = s.undo_n;
    seq_set_slot(&s, 2, 4, 0, COL_TRIAD);
    CHECK(seq_slot_colour(&s, 2) == COL_9TH && s.undo_n == n, "nor a slot stored as the knob's level");
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    CHECK(!seq_state_load(&s, "csq1:nope") && seq_cview(&s) == CVIEW_PROG, "a bad v1 state does not close the browser");
    CHECK(!seq_state_load(&s, "csq2|nope") && seq_cview(&s) == CVIEW_PROG, "nor a bad v2 state");
    seq_browse_cancel(&s);
}

/* the Melody 1 recipe's pitch classes must belong to the chord the Chords lane colours: Power is its root and 5th, and a 6th,
 * 9th, 11th or 13th is read as its 7th chord */
static void test_17_melody_reads_colours(void) {
    static seq_t s;
    static const int LEVEL[] = {-1, 2, 3, 4};
    int ok = 1;
    for (int k = 0; k < 5; k++) {
        int notes[THEORY_CHORD_MAX], pcs[THEORY_CHORD_MAX], np, cnt = 0, bad = 0;
        seq_init(&s, emit, NULL);
        seq_set_prog_preset(&s, 0);   /* Epic: Em C G D, one chord a bar */
        old_mel(&s);   /* 0.20: the recipe setters leave a JAM lane alone */
        seq_mel_set_type(&s, MEL_ARP);
        seq_mel_set_flags(&s, 0, 0, 0, 1);
        seq_set_complexity(&s, LANE_MEL_UP, 30);
        if (k < 4) seq_set_vknob(&s, VK_COL, LEVEL[k]);
        else for (int i = 0; i < 4; i++) seq_set_slot(&s, i, s.part[0].prog[i], 0, COL_6TH);
        for (int bar = 0; bar < 4; bar++) {
            int deg = s.part[0].prog[bar], colour = k == 0 ? COL_POWER : COL_7TH, last = -1;
            np = theory_chord_notes(4, SCALE_MINOR, deg, colour, notes);
            for (int i = 0; i < np; i++) pcs[i] = notes[i] % 12;
            for (int st = 0; st < SEQ_STEPS; st++)
                if (mel_st(&s)[bar * SEQ_STEPS + st].on) last = st;   /* bar 3's is the loop's last */
            for (int st = 0; st < SEQ_STEPS; st++) {
                int note, in = 0;
                if (!mel_st(&s)[bar * SEQ_STEPS + st].on || (bar == 3 && st == last)) continue;   /* the loop's last note is the cadence's tonic */
                note = seq_part_step_note(&s, 0, LANE_MEL_UP, bar, st);
                for (int i = 0; i < np; i++) in |= note % 12 == pcs[i];
                cnt++;
                bad += !in;
            }
        }
        ok &= cnt > 8 && !bad;
        CHECK(cnt > 8 && !bad, "Melody 1 Arp over %s: %d notes, %d off the chord tones", k == 0 ? "Power" : k < 4 ? "9th/11th/13th" : "6th", cnt, bad);
    }
    CHECK(ok, "the recipe reads a Power chord as root and 5th, and 6th to 13th chords as 7th chords");
}

/* 0.17 the CHORDS strip's paging, the FOLLOW pauses and the centre chord */

/* 16 bars of 1-bar chords: 16 slots, 4 pages */
static void strip_16(seq_t *s) {
    seq_init(s, emit, NULL);
    seq_set_bars(s, 16);
    seq_set_chord_len(s, 16);
}

static void test_17_strip_follows(void) {
    static seq_t s;
    double ppq;
    strip_16(&s);
    CHECK(seq_slots(&s) == 16 && seq_strip_pages(&s, TAB_CHORDS) == 4, "16 slots, 4 pages");
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 0 && seq_centre_slot(&s, TAB_CHORDS) == 0 && seq_follow_paused(&s, TAB_CHORDS) == 0, "stopped: slot 0, running");
    ppq = play(&s, 0.0, 20.5, 0);   /* into bar 6 */
    CHECK(seq_cur_slot(&s) == 5, "bar 6 plays slot 5");
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 1 && seq_centre_slot(&s, TAB_CHORDS) == 5 && seq_follow_paused(&s, TAB_CHORDS) == 0, "page 1, centre slot 5");
    ppq = play(&s, ppq, 12.0, 0);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 2 && seq_centre_slot(&s, TAB_CHORDS) == 8, "and on with playback: slot 8, page 2");
    (void)ppq;
}

static void test_17_turn_pauses(void) {
    static seq_t s;
    double ppq;
    strip_16(&s);
    ppq = play(&s, 0.0, 20.5, 0);
    seq_strip_turn(&s, TAB_CHORDS, 1);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 2 && seq_follow_paused(&s, TAB_CHORDS) == 2 && seq_centre_slot(&s, TAB_CHORDS) == 5, "turn +1: page 2, paused, the centre stays");
    ppq = play(&s, ppq, 32.0, 0);   /* 8 more bars: slot 13 */
    CHECK(seq_cur_slot(&s) == 13 && seq_strip_page(&s, TAB_CHORDS) == 2 && seq_follow_paused(&s, TAB_CHORDS) == 2 && seq_centre_slot(&s, TAB_CHORDS) == 5, "held across 8 bars");
    seq_strip_turn(&s, TAB_CHORDS, 2);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 0 && seq_follow_paused(&s, TAB_CHORDS) == 2, "wraps forward");
    seq_strip_turn(&s, TAB_CHORDS, -1);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 3, "and back");
    seq_back_to_playing(&s, TAB_CHORDS);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == 3 && seq_centre_slot(&s, TAB_CHORDS) == 13, "back: the playing slot 13, page 3");
    ppq = play(&s, ppq, 12.0, 0);   /* the 16-bar loop wraps in MPC; here the position runs on past it, slot 0 again */
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == seq_cur_slot(&s) / 4, "and follows again");
    /* a new instance has 4 slots, one page: the arrows do nothing */
    seq_init(&s, emit, NULL);
    seq_strip_turn(&s, TAB_CHORDS, 1);
    CHECK(seq_strip_pages(&s, TAB_CHORDS) == 1 && seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == 0, "one page: the arrows do nothing");
    /* stopped with more pages: slot 0 running, and the arrows pause */
    strip_16(&s);
    seq_strip_turn(&s, TAB_CHORDS, 1);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2 && seq_strip_page(&s, TAB_CHORDS) == 1 && seq_centre_slot(&s, TAB_CHORDS) == 0, "stopped: pages and pauses too");
    seq_back_to_playing(&s, TAB_CHORDS);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == 0, "and back");
}

static void test_17_txn_pause_keeps_paging(void) {
    static seq_t s;
    double ppq;
    strip_16(&s);
    ppq = play(&s, 0.0, 20.5, 0);
    /* a browser with no paging: holds, then runs again (0.24.1 the FIX picker is gone) */
    seq_browse_open(&s, CVIEW_PROG);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 1 && seq_centre_slot(&s, TAB_CHORDS) == 5 && seq_strip_page(&s, TAB_CHORDS) == 1, "browser: paused 1, the centre and page on show");
    ppq = play(&s, ppq, 12.0, 0);
    CHECK(seq_cur_slot(&s) == 8 && seq_strip_page(&s, TAB_CHORDS) == 1 && seq_follow_paused(&s, TAB_CHORDS) == 1, "held while it is open");
    seq_browse_cancel(&s);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == 2 && seq_centre_slot(&s, TAB_CHORDS) == 8, "cancel: running, playback's page and centre");
    /* a browser, then done */
    seq_browse_open(&s, CVIEW_VOICE);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 1 && seq_centre_slot(&s, TAB_CHORDS) == 8, "browser: paused 1, the centre shown");
    ppq = play(&s, ppq, 8.0, 0);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 2 && seq_centre_slot(&s, TAB_CHORDS) == 8, "and holds");
    seq_browse_done(&s);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_centre_slot(&s, TAB_CHORDS) == 10, "done: running");
    /* paged first: the pause stays 2 with the same page */
    seq_strip_turn(&s, TAB_CHORDS, -1);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2 && seq_strip_page(&s, TAB_CHORDS) == 1, "paged to page 1");
    seq_browse_open(&s, CVIEW_VOICE);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 1 && seq_strip_page(&s, TAB_CHORDS) == 1, "open: 1");
    seq_browse_done(&s);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2 && seq_strip_page(&s, TAB_CHORDS) == 1, "done: still paused 2, the same page");
    seq_browse_open(&s, CVIEW_PROG);
    seq_browse_cancel(&s);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2 && seq_strip_page(&s, TAB_CHORDS) == 1, "a browser's cancel too");
    seq_browse_open(&s, CVIEW_VOICE);
    seq_strip_turn(&s, TAB_CHORDS, 1);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 1, "the arrows wait while a browser is open");
    seq_select_part(&s, 1);   /* queued: the switch cancels it at the bar line */
    ppq = play(&s, ppq, 8.0, 0);
    CHECK(seq_part(&s) == 1 && seq_cview(&s) == CVIEW_CHORDS && seq_follow_paused(&s, TAB_CHORDS) == 0, "an automatic close ends 1, and the part switch ends 2 (ruling 18)");
    (void)ppq;
}

static void test_17_page_after_shrink(void) {
    static seq_t s;
    strip_16(&s);
    seq_strip_turn(&s, TAB_CHORDS, -1);   /* page 3 */
    seq_set_centre(&s, TAB_CHORDS, 15);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 3 && seq_centre_slot(&s, TAB_CHORDS) == 15, "last page, last slot");
    seq_set_chord_len(&s, 32);   /* 8 slots */
    CHECK(seq_slots(&s) == 8 && seq_strip_page(&s, TAB_CHORDS) == 1 && seq_centre_slot(&s, TAB_CHORDS) == 7, "a longer chord: clamped to the last page and slot");
    seq_set_chord_len(&s, 16);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 1 && seq_centre_slot(&s, TAB_CHORDS) == 7, "and stays there when it grows back");
    seq_strip_turn(&s, TAB_CHORDS, 1);
    seq_set_bars(&s, 2);   /* 2 slots */
    CHECK(seq_strip_pages(&s, TAB_CHORDS) == 1 && seq_strip_page(&s, TAB_CHORDS) == 0 && seq_centre_slot(&s, TAB_CHORDS) == 1, "fewer bars too");
    seq_set_centre(&s, TAB_CHORDS, 5);
    CHECK(seq_centre_slot(&s, TAB_CHORDS) == 1, "a slot past the loop is ignored");
}

static void test_17_follow_off_pages_by_hand(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    double ppq;
    strip_16(&s);
    ppq = play(&s, 0.0, 20.5, 0);
    seq_set_follow(&s, 0);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == 1 && seq_centre_slot(&s, TAB_CHORDS) == 5, "FOLLOW off keeps what was on show");
    seq_strip_turn(&s, TAB_CHORDS, 1);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 2 && seq_follow_paused(&s, TAB_CHORDS) == 0, "the arrows page by hand, no pause");
    seq_set_centre(&s, TAB_CHORDS, 9);
    ppq = play(&s, ppq, 12.0, 0);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 2 && seq_centre_slot(&s, TAB_CHORDS) == 9 && seq_follow_paused(&s, TAB_CHORDS) == 0, "playback moves neither");
    seq_browse_open(&s, CVIEW_VOICE);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_centre_slot(&s, TAB_CHORDS) == 9 && seq_strip_page(&s, TAB_CHORDS) == 2, "a browser: still no pause");
    seq_browse_cancel(&s);
    CHECK(seq_centre_slot(&s, TAB_CHORDS) == 9, "closed: the last tap again");
    seq_strip_turn(&s, TAB_CHORDS, -3);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 3, "wraps");
    seq_set_follow(&s, 1);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == seq_cur_slot(&s) / 4 && seq_centre_slot(&s, TAB_CHORDS) == seq_cur_slot(&s), "FOLLOW on: running");
    /* a pause does not survive FOLLOW off and on, and is not saved */
    seq_strip_turn(&s, TAB_CHORDS, 1);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2, "paused");
    seq_set_follow(&s, 0);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0, "0 with FOLLOW off");
    seq_set_follow(&s, 1);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0, "running after FOLLOW back on");
    seq_strip_turn(&s, TAB_CHORDS, 1);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && !strstr(buf, "page") && !strstr(buf, "paus"), "a pause is not saved");
    (void)ppq;
}

/* the keys lit, MIDI 24..83, as a bitset of the pitches; -1 when a key reads outside 0..2 */
static uint64_t lit_set(const seq_t *s) {
    uint64_t m = 0;
    for (int k = SEQ_KEY_LO; k <= SEQ_KEY_HI; k++) {
        int v = seq_key_lit(s, k);
        if (v < 0 || v > 2) return ~0ull;
        if (v) m |= 1ull << (k - SEQ_KEY_LO);
    }
    return m;
}
#define KEY(n) (1ull << ((n) - SEQ_KEY_LO))

static void test_17_keys_latch(void) {
    static seq_t s;
    int got[THEORY_CHORD_MAX], n;
    uint64_t want = 0;
    fresh(&s, 0);   /* Em close, then C (bar 2) */
    CHECK(lit_set(&s) == 0 && !seq_key_edge(&s, 0) && !seq_key_edge(&s, 1), "nothing lit before a chord plays");
    play(&s, 0.0, 1.0, 0);   /* playing, in bar 1 */
    CHECK(lit_set(&s) == (KEY(52) | KEY(55) | KEY(59)), "Em: keys 52 55 59 lit (got 0x%llx)", (unsigned long long)lit_set(&s));
    CHECK(seq_key_lit(&s, 52) == 1 && seq_key_lit(&s, 53) == 0 && seq_key_lit(&s, 23) == 0 && seq_key_lit(&s, 84) == 0, "lit is 1; outside the window 0");
    seq_block(&s, 1.0, bpm, 0, 128);
    CHECK(lit_set(&s) == 0, "a stop clears the latch");
    nev = 0;
    play(&s, 0.0, 4.0 + 1.0, 0);   /* into bar 2: C */
    n = ons(2, got, THEORY_CHORD_MAX);
    for (int i = n - 3; i < n; i++) want |= KEY(got[i]);
    CHECK(n == 6 && lit_set(&s) == want && !(lit_set(&s) & KEY(59)),
          "after the next chord fires only its notes are lit, the old chord's not (got 0x%llx, want 0x%llx)", (unsigned long long)lit_set(&s),
          (unsigned long long)want);
}

static void test_17_keys_fold(void) {
    static seq_t s;
    uint64_t lit;
    int ok = 1, any_fold = 0;
    fresh(&s, 0);
    seq_set_vknob(&s, VK_SPR, 3);   /* Wide */
    seq_set_vknob(&s, VK_COL, 4);   /* 13th */
    seq_set_chord_shape(&s, SHAPE_OCT, 1);
    play(&s, 0.0, 1.0, 0);
    lit = lit_set(&s);
    CHECK(lit != ~0ull && lit != 0 && !(lit >> (SEQ_KEY_HI - SEQ_KEY_LO + 1)), "OCTAVE +1, Wide, 13th: every lit key is inside 24..83 (0x%llx)", (unsigned long long)lit);
    for (int k = SEQ_KEY_LO; k <= SEQ_KEY_HI; k++) any_fold |= seq_key_lit(&s, k) == 2;
    ok = seq_key_edge(&s, 1) == 1 && seq_key_edge(&s, 0) == 0;
    CHECK(ok && any_fold, "a note past the top folds in (value 2) and sets the high edge only (edges %d %d)", seq_key_edge(&s, 0), seq_key_edge(&s, 1));
    /* below the window: INVERSION -2 (43 47 52) under Wide and OCTAVE -1 puts the lowest note at 19 */
    seq_block(&s, 1.0, bpm, 0, 128);
    seq_set_chord_shape(&s, SHAPE_OCT, -1);
    seq_set_vknob(&s, VK_INV, -2);
    seq_set_vknob(&s, VK_COL, 0);
    play(&s, 0.0, 1.0, 0);
    CHECK(seq_key_edge(&s, 0) == 1 && seq_key_edge(&s, 1) == 0, "a note below the window sets the low edge only (edges %d %d)", seq_key_edge(&s, 0), seq_key_edge(&s, 1));
    lit = lit_set(&s);
    CHECK(lit != ~0ull && lit != 0 && !(lit >> (SEQ_KEY_HI - SEQ_KEY_LO + 1)), "and folds inside, nearest octave (0x%llx)", (unsigned long long)lit);
    CHECK(seq_key_edge(&s, 2) == 0 && seq_key_edge(&s, -1) == 0, "a bad side reads 0");
}

static void test_17_keys_mute_clears(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    fresh(&s, 0);
    play(&s, 0.0, 1.0, 0);
    seq_set_mute(&s, LANE_BASS, 1);
    CHECK(lit_set(&s) != 0, "muting another lane keeps the latch");
    seq_set_mute(&s, LANE_CHORDS, 1);
    CHECK(lit_set(&s) == 0, "muting the Chords lane clears it");
    play(&s, 1.0, 4.0, 0);
    CHECK(lit_set(&s) == 0, "and a muted lane fires nothing, so nothing relights");
    seq_set_mute(&s, LANE_CHORDS, 0);
    play(&s, 5.0, 4.0, 0);
    CHECK(lit_set(&s) != 0, "unmuted: the next chord lights");
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && seq_state_load(&s, buf) && lit_set(&s) == 0, "a load clears it");
    play(&s, 0.0, 1.0, 0);
    seq_init_preset(&s);
    CHECK(lit_set(&s) == 0, "INIT clears it");
}

/* final review: a paging pause is not saved; a load (csq2 and v1), INIT and a part switch end it (ruling 18) */
static void test_17_pause_ends(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    static const char V1[] = "csq1:1000000000000000100000001000000010101010101010101010101010101010:0000";
    double ppq;
    for (int how = 0; how < 5; how++) {
        strip_16(&s);
        seq_state_save(&s, buf, sizeof buf);
        ppq = play(&s, 0.0, 20.5, 0);
        if (how == 4) seq_select_part(&s, 1);   /* queued first (a copy of the Verse); the switch comes at the bar line */
        seq_strip_turn(&s, TAB_CHORDS, 1);
        CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2, "(%d) paged: paused 2", how);
        if (how == 0) seq_state_load(&s, buf);
        else if (how == 1) seq_state_load(&s, V1);
        else if (how == 2) seq_init_preset(&s);
        else if (how == 3) seq_select_part(&s, 1);   /* playing: queued */
        if (how >= 3) ppq = play(&s, ppq, 4.0, 0);
        CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && !s.tv[TAB_CHORDS].paging, "%s ends the pause", (const char *[]){"a load", "a v1 load", "INIT", "a part switch",
              "a part switch queued before the paging"}[how]);
        CHECK(how < 3 || (seq_part(&s) == 1 && seq_strip_page(&s, TAB_CHORDS) == seq_cur_slot(&s) / 4), "and the strip follows the new part (page %d)",
              seq_strip_page(&s, TAB_CHORDS));
    }
    /* into a part with fewer slots: stopped, at once; the page and centre clamp to its 1 page, no pause */
    strip_16(&s);
    seq_select_part(&s, 2);
    seq_set_bars(&s, 2);   /* the Pre: 2 slots */
    seq_select_part(&s, 0);
    seq_strip_turn(&s, TAB_CHORDS, -1);   /* page 3 */
    seq_set_centre(&s, TAB_CHORDS, 14);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2 && seq_strip_page(&s, TAB_CHORDS) == 3, "the Verse paged to its last page");
    seq_select_part(&s, 2);
    CHECK(seq_part(&s) == 2 && seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_pages(&s, TAB_CHORDS) == 1 && seq_strip_page(&s, TAB_CHORDS) == 0 && seq_centre_slot(&s, TAB_CHORDS) == 0,
          "the switch: no pause, page 0 of 1, the centre slot 0 (%d)", seq_centre_slot(&s, TAB_CHORDS));
    seq_set_follow(&s, 0);
    seq_select_part(&s, 0);
    seq_select_part(&s, 2);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 0 && seq_centre_slot(&s, TAB_CHORDS) <= 1, "FOLLOW off: the hand page and centre clamp too (%d)", seq_centre_slot(&s, TAB_CHORDS));
}

/* final review (ruling 24): a length, chord-length, GEN, NEW PROGRESSION or preset change closes an open picker or browser as
 * DONE first, so its CANCEL cannot put a short loop's chords over the new bars or delete the change's undo step */
static void test_17_edits_close_txn(void) {
    static seq_t s;
    int before[16], n, ok;
    /* the PROGRESSIONS browser open, LOOP LENGTH 4 -> 16, CANCEL: the grown bars stay as the grow made them */
    seq_init(&s, emit, NULL);
    seq_set_prog_preset(&s, 0);
    n = s.undo_n;
    seq_browse_open(&s, CVIEW_PROG);
    seq_set_bars(&s, 16);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && seq_bars(&s) == 16 && seq_slots(&s) == 16, "a length change closes the browser");
    for (int i = 0; i < 16; i++) before[i] = slot_key(&s, i);
    seq_browse_cancel(&s);
    ok = 1;
    for (int i = 0; i < 16; i++) ok &= slot_key(&s, i) == before[i];
    CHECK(ok && s.part[0].prog_len == 16 && s.undo_n == n, "CANCEL after it changes nothing; the unchanged browser left no undo step");
    for (int i = 4; i < 16; i++) ok &= seq_slot_deg(&s, i) == seq_slot_deg(&s, i % 4);
    CHECK(ok, "bars 5-16 repeat the loop, not chord i");
    /* PROGRESSIONS with a try, CHORD LENGTH 8 -> 16, CANCEL: the chord-length step is kept and undoes (0.24.1 the browser: the FIX
     * picker is gone) */
    seq_init(&s, emit, NULL);
    seq_set_chord_len(&s, 8);
    n = s.undo_n;
    for (int i = 0; i < 8; i++) before[i] = slot_key(&s, i);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    seq_set_chord_len(&s, 16);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && seq_chord_len(&s) == 16 && s.undo_n == n + 2, "a chord-length change keeps the try (DONE): two steps");
    seq_browse_cancel(&s);
    CHECK(s.undo_n == n + 2 && seq_chord_len(&s) == 16, "CANCEL after it does nothing");
    seq_undo(&s);
    CHECK(seq_chord_len(&s) == 8 && slot_key(&s, 0) != before[0], "UNDO: the chord length back, the try still there");
    seq_undo(&s);
    ok = s.undo_n == n;
    for (int i = 0; i < 8; i++) ok &= slot_key(&s, i) == before[i];
    CHECK(ok, "then the try");
    /* GEN, GEN ALL, NEW PROGRESSION, a preset: each keeps an open browser first (its open's step stays) */
    for (int how = 0; how < 4; how++) {
        seq_init(&s, emit, NULL);
        seq_browse_open(&s, CVIEW_PROG);
        seq_prog_try(&s, 2);
        for (int i = 0; i < 4; i++) before[i] = slot_key(&s, i);
        if (how == 0) seq_generate(&s, LANE_BASS);
        else if (how == 1) seq_generate_all(&s);
        else if (how == 2) seq_generate_progression(&s);
        else seq_set_prog_preset(&s, 5);
        seq_browse_cancel(&s);
        /* the open's step kept; NEW PROGRESSION pushes its own, GEN on a DRAWN Bass and preset to preset none */
        ok = s.txn.kind == CVIEW_CHORDS && s.undo_n == (how == 2 ? 2 : 1);
        if (how == 0)
            for (int i = 0; i < 4; i++) ok &= slot_key(&s, i) == before[i];
        if (how == 3) ok &= seq_slot_deg(&s, 1) == THEORY_PRESETS[5].deg[1];
        CHECK(ok, "%s closes the browser as DONE: the try stays (%d steps)", (const char *[]){"GEN", "GEN ALL", "NEW PROGRESSION", "a preset"}[how], s.undo_n);
    }
    /* a stale one is cancelled as before */
    seq_init(&s, emit, NULL);
    for (int i = 0; i < 4; i++) before[i] = s.part[0].prog[i];
    play(&s, 0.0, 0.5, 0);
    seq_select_part(&s, 1);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    play(&s, 0.5, 4.0, 0);
    seq_set_bars(&s, 8);
    ok = s.txn.kind == CVIEW_CHORDS && seq_bars(&s) == 8;
    for (int i = 0; i < 4; i++) ok &= s.part[0].prog[i] == before[i];
    CHECK(ok, "a stale browser: the length change cancels it (the old part's slots back), then acts");
}

/* final review (ruling 24): a CANCEL (or UNDO as CANCEL) with other edits pushed since drops only its own undo steps */
static void test_17_cancel_keeps_other_steps(void) {
    static seq_t s;
    int deg1, n, mel;
    for (int how = 0; how < 2; how++) {
        seq_init(&s, emit, NULL);
        old_bass(&s, 30);
        deg1 = seq_slot_deg(&s, 1);
        seq_generate(&s, LANE_BASS);   /* a step before */
        n = s.undo_n;
        seq_browse_open(&s, CVIEW_PROG);   /* 0.24.1 the browser (the FIX picker is gone): its open is its step */
        seq_prog_try(&s, 2);
        seq_toggle_step_lock(&s, LANE_BASS, 0, 0);   /* another tab's edits (one undo step each) */
        mel = s.part[0].lane[LANE_MEL_UP].steps[5].on;
        seq_set_step(&s, LANE_MEL_UP, 0, 5, !mel);
        CHECK(s.undo_n == n + 3, "(%d) an open browser and two other edits: three steps", how);
        if (how == 0) seq_undo(&s);
        else seq_browse_cancel(&s);
        CHECK(seq_cview(&s) == CVIEW_CHORDS && seq_slot_deg(&s, 1) == deg1, "%s: the slot as it was",
              how ? "CANCEL" : "UNDO with the browser open");
        CHECK(s.undo_n == n + 2 && s.part[0].lane[LANE_MEL_UP].steps[5].on == !mel, "only the browser's step left the ring (%d)", s.undo_n - n);
        seq_undo(&s);
        CHECK(s.part[0].lane[LANE_MEL_UP].steps[5].on == mel, "the next UNDO takes the Melody step back");
        seq_undo(&s);
        seq_undo(&s);
        CHECK(s.undo_n == n - 1 && seq_slot_deg(&s, 1) == deg1, "then the lock, then the GEN before: none lost");
    }
    /* a browser: its open snapshot goes, another edit's stays; DONE with nothing changed the same */
    for (int how = 0; how < 2; how++) {
        int before[4];
        seq_init(&s, emit, NULL);
        for (int i = 0; i < 4; i++) before[i] = slot_key(&s, i);
        n = s.undo_n;
        seq_browse_open(&s, CVIEW_PROG);
        if (!how) seq_prog_try(&s, 2);
        seq_set_step(&s, LANE_MEL_UP, 0, 5, !s.part[0].lane[LANE_MEL_UP].steps[5].on);
        if (!how) seq_browse_cancel(&s);
        else seq_browse_done(&s);
        int ok = s.undo_n == n + 1 && s.undo[(s.undo_top + SEQ_UNDO - 1) % SEQ_UNDO].lane == LANE_MEL_UP;
        for (int i = 0; i < 4; i++) ok &= slot_key(&s, i) == before[i];
        CHECK(ok, "%s: the progression as it was, the Melody step's undo kept", how ? "an unchanged browser's DONE" : "a browser's CANCEL");
    }
    /* a full ring a foreign push wraps: the cancel still drops only its own */
    seq_init(&s, emit, NULL);
    old_bass(&s, 30);   /* 0.19: a tap on a DRAWN Bass closes the browser as DONE first */
    for (int i = 0; i < SEQ_UNDO; i++) seq_set_step(&s, LANE_MEL_UP, 0, i, !s.part[0].lane[LANE_MEL_UP].steps[i].on);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    seq_set_step(&s, LANE_BASS, 0, 1, !s.part[0].lane[LANE_BASS].steps[1].on);
    seq_browse_cancel(&s);
    n = 0;
    for (int i = 0; i < s.undo_n; i++) n += s.undo[(s.undo_top + SEQ_UNDO - s.undo_n + i) % SEQ_UNDO].lane < 0;
    CHECK(s.undo_n == SEQ_UNDO - 1 && !n && s.undo[(s.undo_top + SEQ_UNDO - 1) % SEQ_UNDO].lane == LANE_BASS,
          "a full ring: the browser's step gone (%d left), the newest the Bass edit", s.undo_n);
}

/* final review (ruling 25): a chord locked while COLOUR is at Triad stays a triad when the knob turns */
static void test_17_lock_at_triad(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_slot(&s, 1, 3, 0, COL_TRIAD);
    CHECK(seq_slot_colour(&s, 1) == COL_TRIAD && seq_slot_locked(&s, 1), "a Knobs set at level Triad: own colour Triad, locked");
    seq_set_vknob(&s, VK_COL, 1);   /* 7th */
    CHECK(seq_part_eff_colour(&s, 0, 1) == COL_TRIAD && seq_part_eff_colour(&s, 0, 0) == COL_7TH,
          "COLOUR 7th: the locked chord stays Triad (%d), an unlocked one is a 7th", seq_part_eff_colour(&s, 0, 1));
    s.naud = 0;
    seq_audition_chord(&s, 1);
    CHECK(s.part[0].voice_n[1] == 3 && s.naud == 3 && s.part[0].voice_n[0] == 4, "it plays 3 notes (%d), the 7th 4", s.naud);
    seq_toggle_slot_lock(&s, 1);
    CHECK(seq_part_eff_colour(&s, 0, 1) == COL_7TH && s.part[0].voice_n[1] == 4, "unlocked, it follows the knob again");
    seq_toggle_slot_lock(&s, 1);
    CHECK(seq_slot_colour(&s, 1) == COL_7TH && seq_part_eff_colour(&s, 0, 1) == COL_7TH, "locked again at 7th: it keeps the 7th");
    seq_set_vknob(&s, VK_COL, 0);
    CHECK(seq_part_eff_colour(&s, 0, 1) == COL_7TH, "and the knob back at Triad leaves it");
}

/* final review (ruling 23): PROGRESSION's arrows outside the browser keep the locked chords, one undo step a tap */
static void test_17_prog_step_keeps_locks(void) {
    static seq_t s;
    int before[4], n, ok;
    seq_init(&s, emit, NULL);
    seq_set_prog_preset(&s, 1);   /* Pop 1 5 6 4 */
    seq_set_slot(&s, 2, 1, 0, COL_TRIAD);   /* slot 2: ii, locked */
    for (int i = 0; i < 4; i++) before[i] = slot_key(&s, i);
    n = s.undo_n;
    seq_prog_step(&s, 2);   /* Sad Pop 6 4 1 5 */
    CHECK(seq_slot_deg(&s, 2) == 1 && seq_slot_locked(&s, 2), "the locked slot stays");
    CHECK(seq_slot_deg(&s, 0) == 5 && seq_slot_deg(&s, 1) == 3 && seq_slot_deg(&s, 3) == 4 && seq_prog_preset_shown(&s) == 2,
          "the others are Sad Pop's");
    CHECK(s.undo_n == n + 1, "one undo step");
    seq_prog_step(&s, 3);
    CHECK(s.undo_n == n + 2 && seq_slot_locked(&s, 2) && seq_prog_preset_shown(&s) == 3, "every tap one step");
    seq_undo(&s);
    seq_undo(&s);
    ok = s.undo_n == n;
    for (int i = 0; i < 4; i++) ok &= slot_key(&s, i) == before[i];
    CHECK(ok, "two UNDOs: as before the arrows");
    for (int i = 0; i < 4; i++)
        if (!seq_slot_locked(&s, i)) seq_toggle_slot_lock(&s, i);
    for (int i = 0; i < 4; i++) before[i] = slot_key(&s, i);
    n = s.undo_n;
    seq_prog_step(&s, 4);
    ok = s.undo_n == n;
    for (int i = 0; i < 4; i++) ok &= slot_key(&s, i) == before[i];
    CHECK(ok, "every slot locked: nothing, no step");
    seq_prog_step(&s, -1);
    seq_prog_step(&s, THEORY_NPRESETS);
    CHECK(s.undo_n == n, "a bad preset: nothing");
}

/* 0.18 RHYTHM page: the Chords lane is drawn bars (GEN_CH_DRAWN), rendered into its steps by the setters */
#define CK_017_INIT 0xa5434bbeu   /* INIT's first 4 bars, recorded on 0.17.1 (Sustain at COMPLEXITY 0) before the 0.18 core */

static void test_18_init_sustain(void) {
    static seq_t s;
    int ok = 1, ed = 1;
    seq_init(&s, emit, NULL);
    for (int b = 0; b < 4; b++) ok &= bar_is(&s, b, 0) && seq_bar_rhythm(&s, LANE_CHORDS, b, &ed) == 0 && !ed && !seq_bar_locked(&s, LANE_CHORDS, b);
    CHECK(s.part[0].lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_complexity(&s, LANE_CHORDS) == CHORDS_CX_NEUTRAL && ok,
          "a new instance: Chords DRAWN at COMPLEXITY 50, Sustain in every bar");
    seq_init_preset(&s);
    ok = 1;
    for (int b = 0; b < 4; b++)
        ok &= bar_is(&s, b, 0) && seq_bar_rhythm(&s, LANE_CHORDS, b, &ed) == 0 && !ed && seq_bar_hits(&s, LANE_CHORDS, b) == 1 && bar_plays_drawn(&s, b);
    CHECK(s.part[0].lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_complexity(&s, LANE_CHORDS) == CHORDS_CX_NEUTRAL && ok,
          "INIT: Chords DRAWN at COMPLEXITY 50, Sustain in every bar, 1 hit a bar");
    old_bass(&s, 0);   /* 0.19: INIT's Bass is DRAWN (test_19_init_bass); 0.18's Root c0 put back for 0.17.1's hash */
    old_init_mel(&s);  /* 0.20: INIT's Melody is JAM (test_20_init_mel); 0.19's Up-down Arp put back */
    nev = 0;
    play(&s, 0, 16.0, 0);
    seq_block(&s, 16.0, bpm, 0, 128);
    CHECK(events_ck() == CK_017_INIT && balanced(), "INIT plays its 4 bars as 0.17.1's INIT did: one held chord a bar (0x%08x)", events_ck());
}

static void test_18_render_follows_knobs(void) {
    static seq_t s;
    unsigned char c[16], pulse[16], f[16];
    seq_step_t want[16];
    int ok = 1;
    seq_init(&s, emit, NULL);
    seq_part_t *p = &s.part[0];
    const seq_step_t *st = p->lane[LANE_CHORDS].steps;
    seq_set_style(&s, LANE_CHORDS, GEN_CH_PULSE);
    for (int b = 0; b < 4; b++) ok &= bar_is(&s, b, 3) && seq_bar_hits(&s, LANE_CHORDS, b) == 4 && bar_plays_drawn(&s, b);
    CHECK(ok && p->lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_style(&s, LANE_CHORDS) == GEN_CH_PULSE && s.undo_n == 1,
          "a Pulse pick: every bar Pulse, the lane stays DRAWN and reads Pulse, one undo step");
    seq_set_complexity(&s, LANE_CHORDS, 0);
    ok = 1;
    for (int b = 0; b < 4; b++) ok &= seq_bar_hits(&s, LANE_CHORDS, b) == 1 && st[b * SEQ_STEPS].on && st[b * SEQ_STEPS].len == 4 && bar_is(&s, b, 3);
    CHECK(ok && s.undo_n == 1, "COMPLEXITY 0: 1 hit a bar (beat 1, held to beat 2); the drawn bars stay Pulse, no undo step");
    seq_set_complexity(&s, LANE_CHORDS, 50);
    ok = 1;
    for (int b = 0; b < 4; b++) ok &= seq_bar_hits(&s, LANE_CHORDS, b) == 4 && bar_plays_drawn(&s, b);
    CHECK(ok, "COMPLEXITY 50 plays the drawn 4 hits");
    p->rk[DL_CHORDS][RK_SYNC] = 1;
    render_now(&s);
    seq_bar_plays(&s, LANE_CHORDS, 1, c);
    CHECK(!st[SEQ_STEPS].on && st[SEQ_STEPS + 14].on && seq_bar_hits(&s, LANE_CHORDS, 1) == 4 && c[0] == RC_REST && c[14] == RC_HIT && bar_is(&s, 1, 3),
          "SYNC +1: beat 1's hit moves to step 14 (an 8th early), and what is drawn stays");
    p->rk[DL_CHORDS][RK_FILL] = 1;
    p->rb[DL_CHORDS][2].lock = 1;
    render_now(&s);
    seq_set_complexity(&s, LANE_CHORDS, 0);
    rhythm_codes(3, pulse);
    rhythm_to_steps(pulse, want);
    CHECK(seq_bar_locked(&s, LANE_CHORDS, 2) && !memcmp(&st[2 * SEQ_STEPS], want, sizeof want) && !memcmp(p->rfill[DL_CHORDS][2], pulse, 16) &&
          seq_bar_hits(&s, LANE_CHORDS, 1) == 1, "a locked bar plays as drawn: COMPLEXITY, SYNC and FILL leave it");
    seq_bar_plays(&s, LANE_CHORDS, 1, c);
    rhythm_fill(c, 1, f);
    CHECK(!memcmp(p->rfill[DL_CHORDS][1], f, 16), "an unlocked bar's fill variant is its rendered bar with FILL");
    p->rb[DL_CHORDS][0].from = 4;   /* Eighths, new in 0.18 */
    CHECK(seq_style(&s, LANE_CHORDS) == GEN_CH_PULSE, "bar 1 a 0.18 rhythm: the style reads Pulse");
    p->rb[DL_CHORDS][0].from = 21;
    CHECK(seq_style(&s, LANE_CHORDS) == GEN_CH_CHARLESTON, "bar 1 Charleston: the style reads Charleston");
}

static void test_18_new_idea_keeps_bars(void) {
    static seq_t s;
    unsigned char was[16], c[16];
    seq_step_t steps[SEQ_LANE_STEPS];
    seq_init(&s, emit, NULL);
    old_mel(&s);   /* 0.20: a JAM Melody is left too: a generated lane for NEW IDEA to change */
    seq_set_step(&s, LANE_CHORDS, 2, 8, 1);   /* a hit drawn on bar 3 */
    seq_bar_codes(&s, LANE_CHORDS, 2, was);
    memcpy(steps, s.part[0].lane[LANE_CHORDS].steps, sizeof steps);
    int n = s.undo_n, chords = 0;
    seq_generate_all(&s);
    for (int k = 0; k < s.undo_n - n; k++) chords += s.undo[(s.undo_top + SEQ_UNDO - 1 - k) % SEQ_UNDO].lane == LANE_CHORDS;
    seq_bar_codes(&s, LANE_CHORDS, 2, c);
    CHECK(s.undo_n > n && !chords && !memcmp(c, was, 16) && !memcmp(steps, s.part[0].lane[LANE_CHORDS].steps, sizeof steps),
          "NEW IDEA leaves the drawn bars (bar 3's hit kept) and pushes no Chords snapshot");
    n = s.undo_n;
    seq_generate(&s, LANE_CHORDS);
    seq_bar_codes(&s, LANE_CHORDS, 2, c);
    CHECK(s.undo_n == n && !memcmp(c, was, 16) && !memcmp(steps, s.part[0].lane[LANE_CHORDS].steps, sizeof steps),
          "NEW RHYTHM (GEN on Chords) is a no-op on drawn bars: no undo step");
}

static void test_18_play_tap(void) {
    static seq_t s;
    unsigned char c[16], sus[16];
    int ed = 0;
    seq_init(&s, emit, NULL);
    const seq_step_t *st = &s.part[0].lane[LANE_CHORDS].steps[2 * SEQ_STEPS];
    rhythm_codes(0, sus);
    seq_set_step(&s, LANE_CHORDS, 2, 4, 1);   /* silent (held) step 5 of bar 3 */
    seq_bar_codes(&s, LANE_CHORDS, 2, c);
    CHECK(c[4] == RC_HIT && c[0] == RC_HIT && c[3] == RC_HOLD && c[5] == RC_HOLD && seq_bar_rhythm(&s, LANE_CHORDS, 2, &ed) == 0 && ed == 1 &&
          st[0].len == 4 && st[4].on && st[4].len == 12 && s.undo_n == 1, "a tap on a silent step draws a HIT there: edited, one undo step");
    seq_set_step(&s, LANE_CHORDS, 2, 4, 1);
    CHECK(s.undo_n == 1, "a re-send is no edit");
    seq_set_step(&s, LANE_CHORDS, 2, 0, 0);
    seq_bar_codes(&s, LANE_CHORDS, 2, c);
    CHECK(c[0] == RC_REST && c[1] == RC_REST && c[3] == RC_REST && c[4] == RC_HIT && !st[0].on && s.undo_n == 2,
          "a tap on a playing step clears its drawn hit and the holds after it: one undo step");
    seq_undo(&s);
    seq_bar_codes(&s, LANE_CHORDS, 2, c);
    CHECK(c[0] == RC_HIT && c[4] == RC_HIT && st[0].on && st[0].len == 4, "UNDO puts the cleared hit back");
    seq_undo(&s);
    CHECK(bar_is(&s, 2, 0) && seq_bar_rhythm(&s, LANE_CHORDS, 2, &ed) == 0 && !ed && bar_plays_drawn(&s, 2), "UNDO again: Sustain, not edited");
    seq_set_style(&s, LANE_CHORDS, GEN_CH_STABS);
    seq_set_complexity(&s, LANE_CHORDS, 100);   /* every rest of x.......x....... becomes a hit */
    int n = s.undo_n;
    seq_set_step(&s, LANE_CHORDS, 2, 2, 0);
    CHECK(s.undo_n == n && bar_is(&s, 2, 6) && st[2].on, "a hit the knobs added: nothing drawn there, it keeps playing, no undo step");
}

static void test_18_loop_grow_repeats(void) {
    static seq_t s;
    unsigned char a[16], b[16];
    int ed = 0;
    seq_init(&s, emit, NULL);
    seq_set_step(&s, LANE_CHORDS, 1, 8, 1);   /* bar 2 edited */
    seq_set_bars(&s, 8);
    seq_bar_codes(&s, LANE_CHORDS, 1, a);
    seq_bar_codes(&s, LANE_CHORDS, 5, b);
    CHECK(!memcmp(a, b, 16) && seq_bar_rhythm(&s, LANE_CHORDS, 5, &ed) == 0 && ed && bar_plays_drawn(&s, 5) && bar_is(&s, 4, 0) && bar_plays_drawn(&s, 4),
          "a grow to 8 bars repeats the loop: bar 6 is bar 2, edited too");
    seq_init(&s, emit, NULL);
    seq_set_step(&s, LANE_CHORDS, 2, 8, 1);   /* bar 3 edited */
    seq_bar_codes(&s, LANE_CHORDS, 2, a);
    seq_set_bars(&s, 2);
    seq_set_bars(&s, 4);
    seq_bar_codes(&s, LANE_CHORDS, 2, b);
    CHECK(!memcmp(a, b, 16) && seq_bar_rhythm(&s, LANE_CHORDS, 2, &ed) == 0 && ed && bar_plays_drawn(&s, 2) && bar_is(&s, 3, 0),
          "shrinking to 2 and growing back to 4 keeps bars 3 and 4");
}

/* R3: RESET KNOBS on CHORDS keeps its 0.17.1 knobs (COMPLEXITY to 50 now), RHYTHM's resets SYNC..FILL and the shared 1, 6, 7, 8 */
static void test_18_reset_knobs(void) {
    static seq_t s;
    static const signed char RK[4] = {2, 3, 1, 4};
    seq_init(&s, emit, NULL);
    seq_part_t *p = &s.part[0];
    seq_set_complexity(&s, LANE_CHORDS, 20);
    memcpy(p->rk[DL_CHORDS], RK, sizeof RK);
    seq_set_chord_shape(&s, SHAPE_PUSH, 40);
    seq_set_chord_shape(&s, SHAPE_GATE, 10);
    seq_set_chord_shape(&s, SHAPE_DYN, 90);
    int n = s.undo_n;
    seq_reset_chord_knobs(&s);
    CHECK(seq_complexity(&s, LANE_CHORDS) == 50 && !memcmp(p->rk[DL_CHORDS], RK, sizeof RK) && seq_chord_shape(&s, SHAPE_PUSH) == 0,
          "CHORDS' RESET KNOBS: COMPLEXITY 50, the RHYTHM knobs left");
    seq_undo(&s);
    CHECK(s.undo_n == n && seq_complexity(&s, LANE_CHORDS) == 20 && seq_chord_shape(&s, SHAPE_PUSH) == 40, "and one UNDO brings it back");
    seq_reset_rhythm_knobs(&s, LANE_CHORDS);
    CHECK(seq_complexity(&s, LANE_CHORDS) == 50 && !p->rk[DL_CHORDS][RK_SYNC] && !p->rk[DL_CHORDS][RK_GHOSTS] && !p->rk[DL_CHORDS][RK_HUM] && !p->rk[DL_CHORDS][RK_FILL] &&
          seq_chord_shape(&s, SHAPE_PUSH) == 0 && seq_chord_shape(&s, SHAPE_GATE) == 50 && seq_chord_shape(&s, SHAPE_DYN) == 50 &&
          s.undo_n == n + 1 && bar_plays_drawn(&s, 0), "RHYTHM's RESET KNOBS: SYNC..FILL 0, COMPLEXITY 50, PUSH 0, GATE 50, DYNAMICS 50, one undo step");
    seq_undo(&s);
    CHECK(s.undo_n == n && seq_complexity(&s, LANE_CHORDS) == 20 && !memcmp(p->rk[DL_CHORDS], RK, sizeof RK) && seq_chord_shape(&s, SHAPE_GATE) == 10 &&
          seq_chord_shape(&s, SHAPE_DYN) == 90, "and one UNDO brings all of them back");
    seq_reset_rhythm_knobs(&s, LANE_CHORDS);
    seq_reset_rhythm_knobs(&s, LANE_CHORDS);
    CHECK(s.undo_n == n + 1, "nothing to reset: no undo step");
}

/* 0.18 Task 4: a new instance (Chords DRAWN at COMPLEXITY 50) on E minor Epic (Em C G D), Close, every voicing knob 0 */
static void cx_epic(seq_t *s) {
    seq_init(s, emit, NULL);
    seq_set_key(s, 4);
    seq_set_scale(s, SCALE_MINOR);
    seq_set_prog_preset(s, 0);
    seq_set_vmode(s, VMODE_CLOSE);
    for (int k = 0; k < VK_COUNT; k++) seq_set_vknob(s, k, 0);
}

/* 1 when every slot plays `want`, slot `except` (-1 none) `other` */
static int slots_play(const seq_t *s, int want, int except, int other) {
    int ok = seq_prog_len(s) > 0;
    for (int i = 0; i < seq_prog_len(s); i++) ok &= seq_part_eff_colour(s, 0, i) == (i == except ? other : want);
    return ok;
}

static void test_18_cx_harmony(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    int ok = 1;
    cx_epic(&s);
    seq_toggle_slot_lock(&s, 1);   /* C, locked at Triad */
    seq_set_complexity(&s, LANE_CHORDS, 75);
    CHECK(slots_play(&s, COL_7TH, 1, COL_TRIAD) && s.part[0].voice_n[0] == 4, "COMPLEXITY 75: every unlocked chord a 7th, the locked one a triad");
    CHECK(seq_vknob_eff(&s, VK_SPR) == 1 && seq_vknob(&s, VK_SPR) == 0 && seq_vmode_eff(&s) == VMODE_SMOOTH && s.part[0].vmode == VMODE_CLOSE,
          "  SPREAD plays Open, the connection Smooth; the user's Close and SPREAD 0 stay (%d %d)", seq_vknob_eff(&s, VK_SPR), seq_vmode_eff(&s));
    seq_set_complexity(&s, LANE_CHORDS, 90);
    CHECK(slots_play(&s, COL_9TH, 1, COL_TRIAD) && seq_vknob_eff(&s, VK_COL) == 2, "COMPLEXITY 90: 9ths, the locked chord a triad");
    CHECK(seq_vknob_eff(&s, VK_STR) == 2 && seq_vknob(&s, VK_STR) == 0, "  STRUM plays +2 (%d), the knob reads 0", seq_vknob_eff(&s, VK_STR));
    int got[THEORY_CHORD_MAX], spread = 1;
    double at[THEORY_CHORD_MAX];
    nev = 0;
    play_stop(&s, 1.0);
    int k = on_times(1, got, at, THEORY_CHORD_MAX);
    for (int i = 1; i < k; i++) spread &= got[i] > got[i - 1] && fabs(at[i] - at[0] - i * 0.015625) <= 2 * blk();
    CHECK(k == 5 && spread, "  and plays it: 5 notes low to high, 1/64 beat apart (%d notes)", k);
    seq_set_complexity(&s, LANE_CHORDS, 100);
    for (int i = 0; i < 4; i++) {
        int want = i == 1 ? COL_TRIAD : theory_avoid(4, SCALE_MINOR, SCALE_MINOR, s.part[0].prog[i], COL_11TH) ? COL_9TH : COL_11TH;
        ok &= seq_part_eff_colour(&s, 0, i) == want;
    }
    CHECK(ok && seq_part_eff_colour(&s, 0, 0) == COL_11TH && seq_part_eff_colour(&s, 0, 2) == COL_9TH && seq_part_eff_colour(&s, 0, 3) == COL_9TH,
          "COMPLEXITY 100: Em11 (no clash); G and D stay 9th (the 11th over a major 3rd); C locked at Triad");
    CHECK(seq_vknob(&s, VK_COL) == 0 && seq_vknob_eff(&s, VK_COL) == 3, "  COLOUR still reads the user's Triad; its effective value 11th");
    CHECK(seq_rknob_eff(&s, LANE_CHORDS, RK_SYNC) == 2 && seq_rknob_eff(&s, LANE_CHORDS, RK_GHOSTS) == 3 && seq_rknob_eff(&s, LANE_CHORDS, RK_HUM) == 1 && seq_rknob_eff(&s, LANE_CHORDS, RK_FILL) == 0,
          "  RHYTHM knobs at 0 play SYNC +2, GHOSTS 3, HUMANIZE 1, FILL 0 (%d %d %d)", seq_rknob_eff(&s, LANE_CHORDS, RK_SYNC), seq_rknob_eff(&s, LANE_CHORDS, RK_GHOSTS),
          seq_rknob_eff(&s, LANE_CHORDS, RK_HUM));
    s.part[0].rk[DL_CHORDS][RK_SYNC] = 1;
    CHECK(seq_rknob_eff(&s, LANE_CHORDS, RK_SYNC) == 3, "  SYNC 1 plays 3 (COMPLEXITY adds at most +2, clamped to the knob)");
    s.part[0].rk[DL_CHORDS][RK_SYNC] = 0;
    seq_set_complexity(&s, LANE_CHORDS, 90);
    int n = seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    CHECK(n > 0 && seq_state_load(&t, buf) && slots_play(&t, COL_9TH, 1, COL_TRIAD), "a save at 90 loads with its 9ths");
    seq_reset_chord_knobs(&s);
    CHECK(seq_complexity(&s, LANE_CHORDS) == 50 && slots_play(&s, COL_TRIAD, -1, 0) && seq_vmode_eff(&s) == s.part[0].vmode,
          "RESET KNOBS: 50, triads, the style's own mode");
    seq_undo(&s);
    CHECK(seq_complexity(&s, LANE_CHORDS) == 90 && slots_play(&s, COL_9TH, 1, COL_TRIAD), "its UNDO: 90 and the 9ths again");
    seq_set_complexity(&s, LANE_CHORDS, 0);
    CHECK(slots_play(&s, COL_POWER, 1, COL_TRIAD) && seq_rknob_eff(&s, LANE_CHORDS, RK_SYNC) == -2 && seq_vknob_eff(&s, VK_STR) == 0,
          "COMPLEXITY 0: Power chords (the locked one a triad), SYNC -2, STRUM block");
    seq_set_complexity(&s, LANE_CHORDS, 50);
    seq_toggle_slot_lock(&s, 1);
    seq_set_vknob(&s, VK_COL, 1);
    seq_set_complexity(&s, LANE_CHORDS, 90);
    seq_toggle_slot_lock(&s, 2);
    seq_set_complexity(&s, LANE_CHORDS, 50);
    CHECK(slots_play(&s, COL_7TH, 2, COL_11TH), "a chord locked while COMPLEXITY plays it (7th + 2 = 11th) keeps what it played");
}

/* the low interval limit: an effective 9th or more with a 2nd whose lower note is below E3 (52) plays one SPREAD step wider */
static void test_18_low_interval(void) {
    static seq_t s;
    int got[THEORY_CHORD_MAX], n;
    cx_epic(&s);
    seq_set_vknob(&s, VK_INV, -2);
    seq_set_vknob(&s, VK_COL, 1);   /* Em7, two down: B2 D3 E3 G3 */
    nev = 0;
    play_stop(&s, 1.0);
    n = ons(1, got, THEORY_CHORD_MAX);
    CHECK(n == 4 && same_notes(got, n, 47, 50, 52, 55), "a 7th keeps its D3-E3 2nd: 47 50 52 55 (got %d: %d %d %d %d)", n, got[0], got[1], got[2], got[3]);
    seq_set_vknob(&s, VK_COL, 2);   /* Em9, two down: D3 E3 F#3 G3 B3; Open lifts E3 */
    nev = 0;
    play_stop(&s, 1.0);
    n = ons(1, got, THEORY_CHORD_MAX);
    CHECK(n == 5 && same_notes(got, 4, 50, 54, 55, 59) && got[4] == 64 && seq_vknob_eff(&s, VK_SPR) == 0,
          "a 9th with D3-E3 plays Open: 50 54 55 59 64 (got %d: %d %d %d %d %d)", n, got[0], got[1], got[2], got[3], got[4]);
    seq_set_vknob(&s, VK_INV, 0);   /* Em9 in root position: E3 G3 B3 D4 F#4, no low 2nd */
    nev = 0;
    play_stop(&s, 1.0);
    n = ons(1, got, THEORY_CHORD_MAX);
    CHECK(n == 5 && same_notes(got, 4, 52, 55, 59, 62) && got[4] == 66, "  no 2nd below E3: as voiced (got %d %d %d %d %d)", got[0], got[1], got[2], got[3], got[4]);
    seq_set_vknob(&s, VK_INV, -2);
    seq_set_vknob(&s, VK_SPR, -1);   /* R17: the user's Cluster asks for its 2nd */
    nev = 0;
    play_stop(&s, 1.0);
    n = ons(1, got, THEORY_CHORD_MAX);
    CHECK(n == 5 && same_notes(got, 4, 50, 52, 54, 55) && got[4] == 59, "a Cluster is left as it is (got %d: %d %d %d %d %d)", n, got[0], got[1], got[2],
          got[3], got[4]);
    /* the 9th from COMPLEXITY: user Triad at 90 plays Em9 with SPREAD Open; one chord (Drone) a low octave, two down */
    cx_epic(&s);
    seq_set_prog_preset(&s, 14);
    seq_set_chord_shape(&s, SHAPE_OCT, -1);
    seq_set_vknob(&s, VK_INV, -2);
    seq_set_complexity(&s, LANE_CHORDS, 90);
    nev = 0;
    play_stop(&s, 1.0);
    n = ons(1, got, THEORY_CHORD_MAX);
    CHECK(seq_part_eff_colour(&s, 0, 0) == COL_9TH && seq_vknob_eff(&s, VK_SPR) == 1 && n == 5 && same_notes(got, 4, 26, 40, 42, 43) && got[4] == 47,
          "user Triad at 90: Em9 Open (38 42 43 47 52) keeps F#2-G2 low, so it plays Split: 26 40 42 43 47 (got %d: %d %d %d %d %d)", n, got[0], got[1],
          got[2], got[3], got[4]);
}

/* R15: a Chords lane that is not DRAWN (a 0.17 project's generated lane) plays no COMPLEXITY layer and no coupling rule */
static void test_18_old_lane_unmoved(void) {
    static seq_t s;
    int got[THEORY_CHORD_MAX], n;
    cx_epic(&s);
    old_chords(&s);   /* Pulse, complexity 30 */
    seq_set_vknob(&s, VK_INV, -2);
    seq_set_vknob(&s, VK_COL, 2);
    seq_set_complexity(&s, LANE_CHORDS, 100);
    nev = 0;
    play_stop(&s, 1.0);
    n = ons(1, got, THEORY_CHORD_MAX);
    CHECK(slots_play(&s, COL_9TH, -1, 0) && seq_vmode_eff(&s) == VMODE_CLOSE && seq_vknob_eff(&s, VK_COL) == 2 && seq_vknob_eff(&s, VK_SPR) == 0 &&
          seq_vknob_eff(&s, VK_STR) == 0 && seq_rknob_eff(&s, LANE_CHORDS, RK_SYNC) == 0 && seq_rknob_eff(&s, LANE_CHORDS, RK_GHOSTS) == 0,
          "a generated Chords lane at COMPLEXITY 100: the knobs play as set, Close, every chord the COLOUR knob's 9th");
    CHECK(n >= 5 && same_notes(got, 4, 50, 52, 54, 55) && got[4] == 59, "  and its low D3-E3 2nd stays (got %d: %d %d %d %d %d)", n, got[0], got[1],
          got[2], got[3], got[4]);
}

/* Any RHYTHM knob change renders: RESET KNOBS from COMPLEXITY 49 with SYNC 1 and FILL 2 plays the bars as drawn again */
static void test_18_rhythm_reset_renders(void) {
    static seq_t s;
    unsigned char c[16], f[16];
    int ok = 1;
    seq_init(&s, emit, NULL);
    seq_part_t *p = &s.part[0];
    const seq_step_t *st = p->lane[LANE_CHORDS].steps;
    seq_set_style(&s, LANE_CHORDS, GEN_CH_PULSE);
    seq_set_complexity(&s, LANE_CHORDS, 49);
    p->rk[DL_CHORDS][RK_SYNC] = 1;
    p->rk[DL_CHORDS][RK_FILL] = 2;
    render_now(&s);
    CHECK(!st[SEQ_STEPS].on && st[SEQ_STEPS + 14].on, "COMPLEXITY 49, SYNC 1: bar 2's beat 1 plays on step 14");
    seq_reset_rhythm_knobs(&s, LANE_CHORDS);
    for (int b = 0; b < 4; b++) {
        seq_bar_codes(&s, LANE_CHORDS, b, c);
        rhythm_fill(c, 0, f);
        ok &= bar_plays_drawn(&s, b) && !memcmp(p->rfill[DL_CHORDS][b], f, 16);
    }
    CHECK(ok && seq_complexity(&s, LANE_CHORDS) == 50 && !p->rk[DL_CHORDS][RK_SYNC] && !p->rk[DL_CHORDS][RK_FILL], "RESET KNOBS: every bar and fill plays as drawn");
    p->rk[DL_CHORDS][RK_FILL] = 2;   /* FILL alone, at 50 */
    render_now(&s);
    seq_reset_rhythm_knobs(&s, LANE_CHORDS);
    ok = 1;
    for (int b = 0; b < 4; b++) {
        seq_bar_codes(&s, LANE_CHORDS, b, c);
        rhythm_fill(c, 0, f);
        ok &= !memcmp(p->rfill[DL_CHORDS][b], f, 16);
    }
    CHECK(ok && !p->rk[DL_CHORDS][RK_FILL], "  FILL alone at 50: the fills render without it too");
}

/* The progression's mean drift as it plays (theory_drift of each slot's effective colour; SPREAD and INVERSION 0) */
static double mean_drift(const seq_t *s) {
    const seq_part_t *p = &s->part[0];
    int sum = 0, n = seq_prog_len(s);
    for (int i = 0; i < n; i++)
        sum += theory_drift(s->key, s->scale, theory_slot_scale(s->scale, p->src[i]), p->prog[i], seq_part_eff_colour(s, 0, i), 0, 0);
    return (double)sum / n;
}

static void test_18_drift_rises(void) {
    static seq_t s;
    static const int KEYS[4][2] = {{0, SCALE_MAJOR}, {4, SCALE_MINOR}, {7, SCALE_MAJOR}, {9, SCALE_MINOR}};
    static double sum[6][101];
    for (int u = 0; u < 6; u++)   /* the user's COLOUR, Power (-1) .. 13th (4) */
        for (int k = 0; k < 4; k++)
            for (int pr = 0; pr < THEORY_NPRESETS; pr++) {
                cx_epic(&s);
                seq_set_key(&s, KEYS[k][0]);
                seq_set_scale(&s, KEYS[k][1]);
                seq_set_prog_preset(&s, pr);
                seq_set_vknob(&s, VK_COL, u - 1);
                for (int cx = 0; cx <= 100; cx++) {
                    seq_set_complexity(&s, LANE_CHORDS, cx);
                    sum[u][cx] += mean_drift(&s) / (4 * THEORY_NPRESETS);
                }
            }
    for (int u = 0; u < 6; u++) {   /* R16: above 50 a turn never takes more than 1 point off (Tension may swap an avoid-note 11th for a clean 13th) */
        int up = 1, down = 1;
        double big = 0, fall = 0;
        for (int cx = 0; cx < 100; cx++) {
            double d = sum[u][cx + 1] - sum[u][cx];
            if (cx >= 50) up &= d >= -1, fall = d < fall ? d : fall;
            else down &= d >= 0;
            big = fabs(d) > big ? fabs(d) : big;
        }
        CHECK(up && (u == 5 ? sum[u][100] == sum[u][50] : sum[u][100] > sum[u][50]),
              "COLOUR %s: from 50 to 100 the mean drift %s (%.2f, %.2f at 75, %.2f), a turn takes at most %.2f off",
              THEORY_COLOUR_NAME[theory_colour_of_level(u - 1)], u == 5 ? "stays (nothing above a 13th)" : "rises", sum[u][50], sum[u][75], sum[u][100],
              fall < 0 ? -fall : 0.0);
        CHECK(down, "  from 50 to 0 it never rises (%.2f, %.2f at 25, %.2f at 0)", sum[u][50], sum[u][25], sum[u][0]);
        CHECK(big <= 6, "  no 1-step turn moves it more than 6 (largest %.2f)", big);
    }
    CHECK(sum[2][0] < sum[2][50], "COLOUR 7th: COMPLEXITY 0 takes its colour away (%.2f -> %.2f)", sum[2][50], sum[2][0]);
}

/* quarter-bar chords, Pulse, COMPLEXITY 0: one hit a bar, so chords 2-4 of every bar are silent */
static void test_18_cx0_quarter_chords(void) {
    static seq_t s;
    int got[64], ok = 1;
    double at[64];
    seq_init(&s, emit, NULL);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_PULSE);
    seq_set_chord_len(&s, 4);
    seq_set_complexity(&s, LANE_CHORDS, 0);
    nev = 0;
    play(&s, 0, 8.0, 0);
    seq_block(&s, 8.0, bpm, 0, 128);
    int n = on_times(1, got, at, 64);
    for (int i = 0; i < n; i++) ok &= fabs(at[i]) <= 2 * blk() || fabs(at[i] - 4.0) <= 2 * blk();
    CHECK(n > 0 && ok && balanced() && seq_bar_hits(&s, LANE_CHORDS, 0) == 1 && seq_bar_hits(&s, LANE_CHORDS, 1) == 1,
          "2 bars of 1/4-bar chords at COMPLEXITY 0: note-ons only on beat 1 of each bar (%d note-ons), balanced", n);
    int first = 0;
    for (int i = 0; i < n; i++) first += fabs(at[i]) <= 2 * blk();
    CHECK(first > 0 && n == 2 * first, "  one chord in each bar, the same size (%d of %d)", first, n);
}

/* the CPU rule: a COMPLEXITY turn revoices only when the harmony layers change, and renders only the bars whose hits change */
static void test_18_cx_cpu_rule(void) {
    static seq_t s;
    cx_epic(&s);
    seq_part_t *p = &s.part[0];
    seq_step_t *st = p->lane[LANE_CHORDS].steps;
    seq_set_step(&s, LANE_CHORDS, 1, 0, 0);   /* bar 2 cleared: 16 rests, which COMPLEXITY above 50 fills */
    p->voice[0][0] = 1;   /* markers, kept unless revoiced or rendered: */
    st[5].vel = 7;   /* bar 1's held step (Sustain: no rest, so its hits never change above 50) */
    st[SEQ_STEPS + 5].vel = 7;   /* bar 2's rest */
    seq_set_complexity(&s, LANE_CHORDS, 51);
    CHECK(p->voice[0][0] == 1 && st[5].vel == 7 && st[SEQ_STEPS + 5].vel == 7, "50 -> 51 changes no layer and no bar's hits: no revoice, no render");
    seq_set_complexity(&s, LANE_CHORDS, 55);
    CHECK(p->voice[0][0] == 1 && st[5].vel == 7 && st[SEQ_STEPS + 5].vel != 7 && seq_bar_hits(&s, LANE_CHORDS, 1) == 2,
          "51 -> 55: bar 2 gains 2 hits and renders; bar 1 and the voices stay");
    seq_set_complexity(&s, LANE_CHORDS, 75);
    CHECK(p->voice[0][0] != 1 && st[5].vel != 7, "55 -> 75: the 7ths revoice (Smooth now), GHOSTS renders every bar");
}

/* 0.18 Task 5: the RHYTHM knobs' setter (seq_set_rknob): clamped, renders every bar, no undo step */
static void test_18_rknob_set(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_PULSE);
    seq_step_t *st = s.part[0].lane[LANE_CHORDS].steps;
    int n = s.undo_n, ok;
    seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, 9);
    ok = seq_rknob(&s, LANE_CHORDS, RK_SYNC) == 3;
    seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, -9);
    ok &= seq_rknob(&s, LANE_CHORDS, RK_SYNC) == -2;
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 7);
    ok &= seq_rknob(&s, LANE_CHORDS, RK_GHOSTS) == 4;
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, -1);
    ok &= seq_rknob(&s, LANE_CHORDS, RK_GHOSTS) == 0;
    seq_set_rknob(&s, LANE_CHORDS, RK_HUM, 5);
    ok &= seq_rknob(&s, LANE_CHORDS, RK_HUM) == 4;
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, -3);
    ok &= seq_rknob(&s, LANE_CHORDS, RK_FILL) == 0;
    seq_set_rknob(&s, LANE_CHORDS, RK_COUNT, 1);   /* 0.24: 4 is CADENCE now; RK_COUNT the first bad one */
    seq_set_rknob(&s, LANE_CHORDS, -1, 1);
    ok &= seq_rknob(&s, LANE_CHORDS, RK_COUNT) == 0 && seq_rknob(&s, LANE_CHORDS, -1) == 0;
    CHECK(ok, "SYNC clamps to -2..3, GHOSTS, HUMANIZE and FILL to 0..4; a bad knob is ignored and reads 0");
    seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, 0);
    seq_set_rknob(&s, LANE_CHORDS, RK_HUM, 0);
    seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, 1);
    CHECK(!st[SEQ_STEPS].on && st[SEQ_STEPS + 14].on && bar_is(&s, 1, 3), "SYNC +1 renders at once: beat 1's hit moves to step 15");
    st[5].vel = 7;   /* a marker on bar 1's rest, kept unless the bar renders */
    st[3 * SEQ_STEPS + 5].vel = 7;
    seq_set_rknob(&s, LANE_CHORDS, RK_HUM, 1);   /* changes no step, yet every bar renders (no COMPLEXITY gate) */
    CHECK(st[5].vel != 7 && st[3 * SEQ_STEPS + 5].vel != 7, "a knob turn renders every bar");
    CHECK(s.undo_n == n, "knob turns push no undo step");
    seq_browse_open(&s, CVIEW_PROG);   /* an open transaction (0.24.1 the browser: the chord picker is gone) */
    int kind = s.txn.kind;
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 2);
    CHECK(kind && s.txn.kind == kind && seq_rknob(&s, LANE_CHORDS, RK_GHOSTS) == 2, "a knob turn leaves an open transaction open");
    seq_browse_cancel(&s);
}

/* Task 3's review: UNDO of CHORDS' RESET KNOBS leaves the RHYTHM knobs (only RHYTHM's RESET restores them) */
static void test_18_reset_undo_keeps_rk(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_vknob(&s, VK_COL, 2);
    seq_set_complexity(&s, LANE_CHORDS, 20);
    seq_reset_chord_knobs(&s);
    seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, 2);
    seq_undo(&s);
    CHECK(seq_rknob(&s, LANE_CHORDS, RK_SYNC) == 2 && seq_vknob(&s, VK_COL) == 2 && seq_complexity(&s, LANE_CHORDS) == 20,
          "CHORDS RESET, SYNC 2, UNDO: SYNC stays 2, COLOUR and COMPLEXITY come back");
    seq_reset_rhythm_knobs(&s, LANE_CHORDS);
    seq_undo(&s);
    CHECK(seq_rknob(&s, LANE_CHORDS, RK_SYNC) == 2, "RHYTHM's RESET then UNDO: SYNC back to 2");
}

/* each song bar's Chords note-on steps (bit = step) from the event log */
static void chord_masks(const seq_t *s, unsigned *m, int nbars) {
    memset(m, 0, nbars * sizeof *m);
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == s->chan[LANE_CHORDS] - 1 && evs[i].k >= 0 && evs[i].k / 16 < nbars)
            m[evs[i].k / 16] |= 1u << (evs[i].k % 16);
}

/* 1 when song bars [from, to) play Sustain, with the pickup (steps 15 and 16) on those `fill` says */
static int fills_on(const unsigned *m, int from, int to, int (*fill)(int)) {
    int ok = 1;
    for (int b = from; b < to; b++) ok &= m[b] == (fill(b) ? 0xC001u : 0x0001u);
    return ok;
}
static int fill_4th(int b) { return b % 4 == 3; }
static int fill_4th_not8(int b) { return b % 4 == 3 && b != 7; }
static int fill_none(int b) { (void)b; return 0; }

static void test_18_fill_bars(void) {
    static seq_t s;
    unsigned m[16];
    seq_init(&s, emit, NULL);   /* Sustain: one hit a bar */
    seq_set_bars(&s, 16);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 1);   /* pickup */
    nev = 0;
    play(&s, 0, 64.0, 0);
    seq_block(&s, 64.0, bpm, 0, 128);
    chord_masks(&s, m, 16);
    CHECK(fills_on(m, 0, 16, fill_4th), "a 16-bar loop, FILL 1: the pickup plays on bars 4, 8, 12 and 16 only");
    s.part[0].rb[DL_CHORDS][7].lock = 1;
    render_now(&s);
    nev = 0;
    play(&s, 0, 64.0, 0);
    seq_block(&s, 64.0, bpm, 0, 128);
    chord_masks(&s, m, 16);
    CHECK(fills_on(m, 0, 16, fill_4th_not8), "a locked bar 8 plays no fill; bars 4, 12 and 16 still do");
    seq_set_bars(&s, 1);
    nev = 0;
    play(&s, 0, 48.0, 0);
    seq_block(&s, 48.0, bpm, 0, 128);
    chord_masks(&s, m, 12);
    CHECK(fills_on(m, 0, 12, fill_4th), "a 1-bar loop: the pickup on song bars 3, 7 and 11 (every 4th time round)");
    nev = 0;
    play(&s, 20.0, 16.0, 0);   /* song bars 5..8 (0-based) */
    seq_block(&s, 36.0, bpm, 0, 128);
    chord_masks(&s, m, 9);
    CHECK(fills_on(m, 5, 9, fill_4th), "started at song bar 5: counted from the song position (bar 7), not from the start");
    seq_set_bars(&s, 2);
    nev = 0;
    play(&s, 0, 32.0, 0);
    seq_block(&s, 32.0, bpm, 0, 128);
    chord_masks(&s, m, 8);
    CHECK(fills_on(m, 0, 8, fill_4th), "a 2-bar loop: on song bars 3 and 7");
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 0);
    nev = 0;
    play(&s, 0, 32.0, 0);
    seq_block(&s, 32.0, bpm, 0, 128);
    chord_masks(&s, m, 8);
    CHECK(fills_on(m, 0, 8, fill_none), "FILL 0: no fill");
}

/* plays from `ppq` for `beats` in blocks of `frames`, then stops */
static void play_fine(seq_t *s, double ppq, double beats, int frames) {
    double b = frames * bpm / (60.0 * 44100.0), end = ppq + beats;
    ev_seq = s;
    for (; ppq < end - 1e-9; ppq += b) {
        ev_ppq = ppq;
        seq_block(s, ppq, bpm, 1, frames);
    }
    ev_seq = NULL;
    seq_block(s, ppq, bpm, 0, frames);
}

typedef struct { double ppq; int note, vel, k; } hit_t;   /* a Chords note-on: its block's song position, the step last fired */

static int chord_ons(const seq_t *s, hit_t *h, int max) {
    int n = 0;
    for (int i = 0; i < nev && n < max; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == s->chan[LANE_CHORDS] - 1)
            h[n++] = (hit_t){evs[i].ppq, evs[i].note, evs[i].vel, evs[i].k};
    return n;
}

#define HUM_FRAMES 8
#define HUM_MAX 600
/* Pulse (4 hits a bar) over 2 passes of the 4-bar loop at HUMANIZE hum, 8-frame blocks; tap15: a hit drawn on every bar's
 * step 16; swing and push: SWING and the Chords PUSH */
static void hum_setup(seq_t *s, int hum, int tap15, int swing, int push) {
    seq_init(s, emit, NULL);
    seq_set_style(s, LANE_CHORDS, GEN_CH_PULSE);
    for (int b = 0; tap15 && b < 4; b++) seq_set_step(s, LANE_CHORDS, b, 15, 1);
    seq_set_swing(s, swing);
    seq_set_chord_shape(s, SHAPE_PUSH, push);
    seq_set_rknob(s, LANE_CHORDS, RK_HUM, hum);
    nev = 0;
}
static int hum_run(seq_t *s, int hum, int tap15, int swing, int push, hit_t *h) {
    hum_setup(s, hum, tap15, swing, push);
    play_fine(s, 0, 32.0, HUM_FRAMES);
    return chord_ons(s, h, HUM_MAX);
}

static double beats_ms(double b) { return b * 60000.0 / bpm; }

/* hum_run(4, tap15, SWING 100, PUSH 100), HUMANIZE turned to 0 in the first block where the j-th step-16 hit waits; *late counts
 * the runs where that hit waited past the next bar's pushed chord (PUSH 100: 1/8 beat before the bar) */
static int hum_drop_run(seq_t *s, int j, hit_t *h, int *late) {
    double b = HUM_FRAMES * bpm / (60.0 * 44100.0), ppq = 0;
    int seen = 0, was = 0;
    hum_setup(s, 4, 1, 100, 100);
    ev_seq = s;
    for (; ppq < 32.0 - 1e-9; ppq += b) {
        ev_ppq = ppq;
        seq_block(s, ppq, bpm, 1, HUM_FRAMES);
        int now = s->hum[DL_CHORDS].on && s->hum[DL_CHORDS].step == 15;
        if (now && !was && seen++ == j) {
            *late += s->hum[DL_CHORDS].on_ppq > ceil(s->hum[DL_CHORDS].on_ppq / 4.0) * 4.0 - 0.125;
            seq_set_rknob(s, LANE_CHORDS, RK_HUM, 0);
        }
        was = now;
    }
    ev_seq = NULL;
    seq_block(s, ppq, bpm, 0, HUM_FRAMES);
    return chord_ons(s, h, HUM_MAX);
}

static void test_18_humanize(void) {
    static seq_t s;
    static hit_t h0[HUM_MAX], h4[HUM_MAX], h2[HUM_MAX];
    double blk_ms = beats_ms(HUM_FRAMES * bpm / (60.0 * 44100.0)), most = 0, d;
    int n0 = hum_run(&s, 0, 0, 0, 0, h0), ok = n0 > 0, same = 1, moved = 0, ordered;
    for (int i = 0; i < n0; i++) ok &= h0[i].ppq >= h0[i].k / 4.0 - 1e-9 && beats_ms(h0[i].ppq - h0[i].k / 4.0) < blk_ms + 1e-6;
    CHECK(ok && n0 % 2 == 0, "HUMANIZE 0: every Chords note-on goes out in its step's block, as 0.17 (%d)", n0);
    int n4 = hum_run(&s, 4, 0, 0, 0, h4);
    ok = n4 == n0;
    for (int i = 0; ok && i < n0; i++) {
        d = beats_ms(h4[i].ppq - h0[i].ppq);
        ok &= h4[i].note == h0[i].note && d > -blk_ms && d < 16.0 + blk_ms && abs(h4[i].vel - h0[i].vel) <= 12;
        most = d > most ? d : most;
        moved |= h4[i].vel != h0[i].vel;
        if (i >= n0 / 2)   /* pass 2 against pass 1: the same delay and velocity */
            same &= fabs(beats_ms(h4[i].ppq - h4[i - n0 / 2].ppq) - beats_ms(h0[i].ppq - h0[i - n0 / 2].ppq)) < blk_ms + 1e-6 &&
                    h4[i].vel == h4[i - n0 / 2].vel;
    }
    CHECK(ok && most > 8.0 && moved, "HUMANIZE 4: the same notes, each delayed 0..16 ms (most %.1f ms), velocity within +-12", most);
    CHECK(same, "the same offsets on every loop pass");
    int n2 = hum_run(&s, 2, 0, 0, 0, h2);
    ok = n2 == n0;
    for (int i = 0; ok && i < n0; i++) ok &= beats_ms(h2[i].ppq - h0[i].ppq) < 8.0 + blk_ms && abs(h2[i].vel - h0[i].vel) <= 6;
    CHECK(ok, "HUMANIZE 2: at most 8 ms, velocity within +-6");
    /* 300 BPM, SWING 100, PUSH 100, a hit on every bar's step 16: the next bar's pushed chord comes 1/24 beat (8.3 ms) after
     * it, less than HUMANIZE 4's delay */
    bpm = 300.0;
    blk_ms = beats_ms(HUM_FRAMES * bpm / (60.0 * 44100.0));
    n4 = hum_run(&s, 4, 1, 100, 0, h4);
    n0 = hum_run(&s, 0, 1, 100, 0, h0);
    most = 0;
    for (int i = 0; n4 == n0 && i < n0; i++)
        if (h0[i].k % 16 == 15) most = fmax(most, beats_ms(h4[i].ppq - h0[i].ppq));
    CHECK(n4 == n0 && most > 8.3 + blk_ms, "a step-16 hit delayed past the pushed chord's time (%.1f ms)", most);
    n0 = hum_run(&s, 0, 1, 100, 100, h0);
    n4 = hum_run(&s, 4, 1, 100, 100, h4);
    ordered = n4 == n0 && n0 > 0;
    for (int i = 0; ordered && i < n0; i++)   /* the notes in HUMANIZE 0's order (the chords differ by bar), none earlier than there */
        ordered &= h4[i].note == h0[i].note && h4[i].ppq >= h0[i].ppq - 1e-9 && beats_ms(h4[i].ppq - h0[i].ppq) < 16.0 + blk_ms;
    CHECK(ordered, "with PUSH there, every chord still plays in step order: none before the hit before it");
    int late = 0;
    ordered = 1;
    for (int j = 0; j < 8; j++) {   /* each step-16 hit in turn: HUMANIZE to 0 while it waits */
        int n = hum_drop_run(&s, j, h4, &late);
        for (int i = 0; i < n0; i++) ordered &= n == n0 && h4[i].note == h0[i].note;
    }
    CHECK(ordered && late, "HUMANIZE to 0 while a delayed hit waits (%d of 8 past the pushed chord's time): it still sounds first", late);
    bpm = 120.0;
}

/* edited, locked and library bars and the RHYTHM knobs save and load back the same, with no Chords step records */
static void test_18_save_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    seq_part_t *p = &s.part[0];
    seq_init(&s, emit, NULL);
    seq_set_style(&s, LANE_CHORDS, GEN_CH_PULSE);              /* every bar library Pulse (3) */
    seq_set_step(&s, LANE_CHORDS, 1, 3, 1);                   /* bar 2 edited from Pulse */
    rhythm_parse("A.g-x..-Axg.x-..", p->rb[DL_CHORDS][2].c);            /* bar 3 drawn from nothing */
    p->rb[DL_CHORDS][2].from = -1, p->rb[DL_CHORDS][2].ed = 1;
    rhythm_codes(6, p->rb[DL_CHORDS][3].c);                              /* bar 4 Stabs, locked */
    p->rb[DL_CHORDS][3].from = 6, p->rb[DL_CHORDS][3].lock = 1;
    seq_set_complexity(&s, LANE_CHORDS, 62);
    seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, -1);
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 2);
    seq_set_rknob(&s, LANE_CHORDS, RK_HUM, 3);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 4);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "|A1=0:|A1.g=17,1,62|") && !strstr(buf, "A1.lk") && strstr(buf, "|A.rz=-1,2,3,4"),
          "a DRAWN lane saves .g=17,seed,cx and no step records; the knobs in .rz");
    CHECK(strstr(buf, "|A.rb=3,3") != NULL, "an unedited bar saves its library index");
    seq_init(&t, emit, NULL);
    const seq_part_t *q = &t.part[0];
    CHECK(seq_state_load(&t, buf), "and loads");
    CHECK(!memcmp(q->rb[DL_CHORDS], p->rb[DL_CHORDS], 4 * sizeof p->rb[DL_CHORDS][0]) && !memcmp(q->rk[DL_CHORDS], p->rk[DL_CHORDS], sizeof p->rk[DL_CHORDS]),
          "every bar's codes, rhythm, edited and lock flags, and SYNC..FILL come back");
    CHECK(!memcmp(q->lane[LANE_CHORDS].steps, p->lane[LANE_CHORDS].steps, 4 * SEQ_STEPS * sizeof(seq_step_t)) &&
          !memcmp(q->rfill[DL_CHORDS], p->rfill[DL_CHORDS], 4 * sizeof p->rfill[DL_CHORDS][0]) && q->lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN &&
          seq_complexity(&t, LANE_CHORDS) == 62, "and the lane renders the same steps and fills, DRAWN at COMPLEXITY 62");
    CHECK(seq_state_save(&t, again, sizeof again) == n && !strcmp(buf, again), "it saves back byte for byte");
    seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, 0), seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 0), seq_set_rknob(&s, LANE_CHORDS, RK_HUM, 0), seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 0);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, ".rz="), "the knobs at 0 save no .rz");
}

/* the biggest project: 6 parts x 16 dense edited bars with the RHYTHM knobs set saves every part */
static void test_18_full_save_fits(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    unsigned char c[16];
    int ok = 1;
    rhythm_parse("AxgxAxgxAxgxAxgx", c);
    seq_init(&s, emit, NULL);
    for (int part = 0; part < SEQ_NPARTS; part++) {
        seq_select_part(&s, part);
        seq_set_bars(&s, 16);
        for (int b = 0; b < SEQ_MAX_BARS; b++) {
            memcpy(s.part[part].rb[DL_CHORDS][b].c, c, 16);
            s.part[part].rb[DL_CHORDS][b].from = 3, s.part[part].rb[DL_CHORDS][b].ed = 1;   /* edited from Pulse: the longest entry */
        }
        seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, -2), seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 4), seq_set_rknob(&s, LANE_CHORDS, RK_HUM, 4), seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 4);
        render_now(&s);
    }
    seq_select_part(&s, 0);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && !seq_unsaved_parts(&s), "6 parts x 16 edited bars of AxgxAxgxAxgxAxgx, knobs set: every part saved in %d bytes (unsaved 0x%x)", n,
          seq_unsaved_parts(&s));
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf), "and it loads");
    for (int part = 0; part < SEQ_NPARTS; part++)
        ok &= seq_part_used(&t, part) && !memcmp(t.part[part].rb[DL_CHORDS], s.part[part].rb[DL_CHORDS], sizeof s.part[part].rb[DL_CHORDS]) &&
              !memcmp(t.part[part].rk[DL_CHORDS], s.part[part].rk[DL_CHORDS], sizeof s.part[part].rk[DL_CHORDS]) &&
              !memcmp(t.part[part].lane[LANE_CHORDS].steps, s.part[part].lane[LANE_CHORDS].steps, sizeof s.part[part].lane[LANE_CHORDS].steps);
    CHECK(ok, "every part's bars, knobs and Chords steps come back");
}

/* no step lock on the Chords lane */
static int any_lock_bits(const seq_part_t *p) {
    for (int i = 0; i < (int)sizeof p->lane[LANE_CHORDS].lock; i++)
        if (p->lane[LANE_CHORDS].lock[i]) return 1;
    return 0;
}

/* 0.12-0.17 Chords lanes (every old style, variant and chord length, a locked step in bar 1) and a MANUAL one load as
 * DRAWN bars at COMPLEXITY 50 that sound as the 0.17 load did (R2: on, sounding length, velocity class) */
static void test_18_old_styles_convert(void) {
    static const int STYLES[] = {GEN_CH_SUSTAIN, GEN_CH_PULSE, GEN_CH_STABS, GEN_CH_OFFBEAT, GEN_CH_CHARLESTON,
                                 GEN_CH_BALLAD, GEN_CH_FUNK, GEN_CH_HALFTIME, GEN_CH_PUSH};
    static const int OLD_RB[] = {0, 3, 6, 8, 21, 1, 20, 2, 5};   /* their library rhythms (rhythm.h) */
    static const int CL[4] = {32, 16, 8, 4};
    static seq_t t;
    static char st[SEQ_STATE_MAX];
    static seq_step_t want[SEQ_LANE_STEPS];
    int loads = 0, sounds = 0, drawn = 0, named = 0, runs = 0;
    for (int k = 0; k < 9; k++)
        for (int v = 0; v < 3; v++)
            for (int c = 0; c < 4; c++) {
                char cv[32] = "";
                if (v) snprintf(cv, sizeof cv, "|A1.cv=%d", v);
                snprintf(st, sizeof st,
                         "csq2|gv=1|k=4|s=1|A.len=4|A.cl=%d|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:0215461|A1.g=%d,7,30"
                         "|A1.lk=4" "000000000000000" Z16 Z16 Z16 "%s|A2=0:|A3=0:|A4=0:", CL[c], STYLES[k], cv);
                seq_init(&t, emit, NULL);
                runs++;
                if (!seq_state_load(&t, st)) continue;
                loads++;
                const seq_part_t *q = &t.part[0];
                unsigned char bv[SEQ_MAX_BARS] = {0};
                gen_params_t gp = {STYLES[k], 7, 30, 4, CL[c], q->prog, q->prog_len, 4, 1, NULL, LANE_CHORDS, v, bv};
                memset(want, 0, sizeof want);
                gen_lane_v1(&gp, want);
                want[2] = (seq_step_t){1, TONE_R, 0, 0x46, 2, 0};   /* the locked record: a ghost, 2 steps */
                int same = 1;
                for (int b = 0; b < 4; b++) same &= bar_sounds_as(&q->lane[LANE_CHORDS].steps[b * SEQ_STEPS], &want[b * SEQ_STEPS]);
                sounds += same;
                drawn += q->lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_complexity(&t, LANE_CHORDS) == CHORDS_CX_NEUTRAL &&
                         !q->rk[DL_CHORDS][0] && !q->rk[DL_CHORDS][1] && !q->rk[DL_CHORDS][2] && !q->rk[DL_CHORDS][3] && !q->rb[DL_CHORDS][0].lock && !any_lock_bits(q);
                int from = 1;
                for (int b = 0; b < 4; b++) {   /* every bar from the old style's rhythm, edited where it differs */
                    unsigned char lib[16];
                    rhythm_codes(q->rb[DL_CHORDS][b].from, lib);
                    from &= q->rb[DL_CHORDS][b].from == OLD_RB[k] && q->rb[DL_CHORDS][b].ed == (memcmp(lib, q->rb[DL_CHORDS][b].c, 16) != 0);
                }
                named += from && seq_style(&t, LANE_CHORDS) == STYLES[k] && q->rb[DL_CHORDS][0].ed;   /* bar 1's locked ghost: edited */
            }
    CHECK(loads == runs, "every old style x variant x chord length loads (%d of %d)", loads, runs);
    CHECK(sounds == runs, "and sounds as 0.17 loaded it: on, sounding length, velocity class (%d of %d)", sounds, runs);
    CHECK(drawn == runs, "as DRAWN bars at COMPLEXITY 50, RHYTHM knobs 0, no bar or step locks (%d of %d)", drawn, runs);
    CHECK(named == runs, "every bar is from the old style's rhythm, edited where it differs; style_1 reads the style (%d of %d)", named, runs);

    /* a MANUAL Chords lane: from its steps, the same way */
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, "csq2|gv=1|k=4|s=1|A.len=2|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:0015703" "0415640"
                             "0a15461" "1015460" "1315641|A2=0:|A3=0:|A4=0:"),
          "a MANUAL Chords lane loads");
    seq_step_t m[2 * SEQ_STEPS] = {{0}};
    m[0] = (seq_step_t){1, TONE_R, 0, 0x70, 4, 0}, m[4] = (seq_step_t){1, TONE_R, 0, 0x64, 1, 0};
    m[10] = (seq_step_t){1, TONE_R, 0, 0x46, 2, 0}, m[16] = (seq_step_t){1, TONE_R, 0, 0x46, 1, 0}, m[19] = (seq_step_t){1, TONE_R, 0, 0x64, 2, 0};
    const seq_part_t *q = &t.part[0];
    CHECK(q->lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_complexity(&t, LANE_CHORDS) == 50 &&
          bar_sounds_as(q->lane[LANE_CHORDS].steps, m) && bar_sounds_as(&q->lane[LANE_CHORDS].steps[SEQ_STEPS], &m[SEQ_STEPS]) &&
          q->rb[DL_CHORDS][0].from == -1 && q->rb[DL_CHORDS][0].ed, "  becomes DRAWN at 50 and sounds the same; its bars are drawn from nothing (edited)");
}

/* `.rb` entries that are not an index 0-23 or a 16-step string become Pulse; `.rz` clamps; nothing refuses the load */
static void test_18_bad_rb(void) {
    static seq_t t;
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50";
    static const char *const BAD[] = {"3,zzz,L99,A..x", ",,,", "", "L", "3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3", "24,-1,3x,AxAxAxAxAxAxAxAxA",
                                      "7AxgxAxgxAxgxAxg", "LL3,3L,99999999999,A.g-x..-Axg.x-..x"};
    static char st[512];
    unsigned char pulse[16];
    rhythm_codes(3, pulse);
    int ok = 1;
    for (int i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++) {
        snprintf(st, sizeof st, "%s|A.rb=%s|A2=0:|A3=0:|A4=0:", HEAD, BAD[i]);
        seq_init(&t, emit, NULL);
        ok &= seq_state_load(&t, st);
        for (int b = 1; b < 4; b++) ok &= !memcmp(t.part[0].rb[DL_CHORDS][b].c, pulse, 16) && t.part[0].rb[DL_CHORDS][b].from == 3 && !t.part[0].rb[DL_CHORDS][b].ed && !t.part[0].rb[DL_CHORDS][b].lock;
    }
    CHECK(ok, "bad .rb entries load as unedited, unlocked Pulse");
    snprintf(st, sizeof st, "%s|A.rb=3,zzz,L99,A..x|A2=0:|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && bar_is(&t, 0, 3) && t.part[0].rb[DL_CHORDS][0].from == 3, "`3` is Pulse, the others Pulse");
    snprintf(st, sizeof st, "%s|A2=0:|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    ok = seq_state_load(&t, st);
    for (int b = 0; b < SEQ_MAX_BARS; b++) ok &= bar_is(&t, b, 3) && !t.part[0].rb[DL_CHORDS][b].ed;
    CHECK(ok && bar_plays_drawn(&t, 0), "a DRAWN lane without .rb: Pulse in every bar");
    snprintf(st, sizeof st, "%s|A.rb=L2,Axgx............|A.rz=9,-9,2,7|A2=0:|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    const seq_part_t *q = &t.part[0];
    CHECK(seq_state_load(&t, st) && q->rk[DL_CHORDS][RK_SYNC] == 3 && q->rk[DL_CHORDS][RK_GHOSTS] == 0 && q->rk[DL_CHORDS][RK_HUM] == 2 && q->rk[DL_CHORDS][RK_FILL] == 4,
          ".rz=9,-9,2,7 clamps to SYNC 3, GHOSTS 0, HUM 2, FILL 4");
    CHECK(q->rb[DL_CHORDS][0].lock && q->rb[DL_CHORDS][0].from == 2 && !q->rb[DL_CHORDS][0].ed && bar_is(&t, 0, 2) && q->rb[DL_CHORDS][1].from == -1 && q->rb[DL_CHORDS][1].ed,
          "`L2` is Half-time locked; a bare string is a bar drawn from nothing");
    static const char *const BAD_RZ[] = {"1,x,1,1", "1,1,1", "1,1,1,1,1", "", "1,,1,1", "-,1,1,1", "1,1,1,1,", "1234567,1,1,1"};
    ok = 1;
    for (int i = 0; i < (int)(sizeof BAD_RZ / sizeof BAD_RZ[0]); i++) {
        snprintf(st, sizeof st, "%s|A.rz=%s|A2=0:|A3=0:|A4=0:", HEAD, BAD_RZ[i]);
        seq_init(&t, emit, NULL);
        ok &= seq_state_load(&t, st) && !q->rk[DL_CHORDS][0] && !q->rk[DL_CHORDS][1] && !q->rk[DL_CHORDS][2] && !q->rk[DL_CHORDS][3];
    }
    CHECK(ok, "a malformed .rz is ignored (the knobs 0), never refused");
}

/* R19: a csq1 (v1) state and seq_reset_v1 leave the Chords lane DRAWN: its v1 steps as drawn bars (R2), COMPLEXITY 50, rk 0 */
static void test_18_v1_chords_drawn(void) {
    static seq_t t;
    static const char V1[] = "csq1:1000100010001010" "1000000010000000" "1010101010101010" "1010101010101010" ":0000";
    seq_step_t want[SEQ_STEPS] = {{0}};
    for (int i = 0; i < SEQ_STEPS; i++)
        if (V1[5 + i] == '1') want[i] = (seq_step_t){1, TONE_R, 0, 100, 1, 0};   /* v1_part's MANUAL step */
    seq_init(&t, emit, NULL);
    seq_set_rknob(&t, LANE_CHORDS, RK_FILL, 2);
    const seq_part_t *q = &t.part[0];
    int ok = seq_state_load(&t, V1) && q->lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_complexity(&t, LANE_CHORDS) == 50 &&
             !q->rk[DL_CHORDS][0] && !q->rk[DL_CHORDS][1] && !q->rk[DL_CHORDS][2] && !q->rk[DL_CHORDS][3];
    for (int b = 0; b < 4; b++) ok &= bar_sounds_as(&q->lane[LANE_CHORDS].steps[b * SEQ_STEPS], want) && bar_plays_drawn(&t, b) &&
                                     q->rb[DL_CHORDS][b].from == rhythm_find(q->rb[DL_CHORDS][b].c) && q->rb[DL_CHORDS][b].ed == (q->rb[DL_CHORDS][b].from < 0);
    CHECK(ok, "a csq1 state loads its Chords lane as DRAWN bars at c50, rk 0, that sound as its v1 steps (from rhythm_find)");
    seq_init(&t, emit, NULL);
    seq_set_rknob(&t, LANE_CHORDS, RK_SYNC, 1);
    seq_reset_v1(&t);
    want[4] = want[8] = want[12] = want[14] = (seq_step_t){0};   /* v1's default: one chord a bar on step 1 */
    ok = q->lane[LANE_CHORDS].gen.style == GEN_CH_DRAWN && seq_complexity(&t, LANE_CHORDS) == 50 && !q->rk[DL_CHORDS][RK_SYNC];
    for (int b = 0; b < 4; b++) ok &= bar_sounds_as(&q->lane[LANE_CHORDS].steps[b * SEQ_STEPS], want) && bar_plays_drawn(&t, b);
    CHECK(ok, "seq_reset_v1 leaves a DRAWN lane at c50, rk 0, playing v1's one chord a bar");
}

/* 0.18 Task 7: bar `bar` drawn as the 16-character string want (A x g - .) */
static int bar_reads(const seq_t *s, int bar, const char *want) {
    unsigned char c[16];
    char str[17];
    seq_bar_codes(s, LANE_CHORDS, bar, c);
    rhythm_format(c, str);
    return !strcmp(str, want);
}

/* Bar b gets library rhythm ri with no undo step (a test's setup), rendered */
static void bar_set(seq_t *s, int b, int ri) {
    rbar_t *r = &s->part[s->cur_part].rb[DL_CHORDS][b];
    rhythm_codes(ri, r->c);
    r->from = (signed char)ri, r->ed = 0;
    render_now(s);
}

/* Locks bar b through the API: open, LOCK, DONE */
static void bar_lock(seq_t *s, int b) {
    seq_rbar_open(s, LANE_CHORDS, b);
    seq_rbar_lock(s, LANE_CHORDS);
    seq_rbar_done(s, LANE_CHORDS);
}

static void test_18_paint_rules(void) {
    static seq_t s;
    int ed = 0, n, r;
    seq_init(&s, emit, NULL);   /* Sustain: x--------------- */
    r = seq_rbar_paint(&s, LANE_CHORDS, 0, RC_REST);
    CHECK(!r && bar_reads(&s, 1, "x---------------"), "no bar open: a paint does nothing");
    seq_rbar_open(&s, LANE_CHORDS, 1);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == 1 && seq_cview(&s) == CVIEW_BAR, "bar 2 open: seq_cview CVIEW_BAR");
    n = s.undo_n;
    r = seq_rbar_paint(&s, LANE_CHORDS, 0, RC_REST);
    CHECK(r == 1 && bar_reads(&s, 1, "................") && s.undo_n == n + 1 &&
          seq_bar_rhythm(&s, LANE_CHORDS, 1, &ed) == 0 && ed && bar_plays_drawn(&s, 1), "REST on the hit clears it and its holds: one undo step, edited");
    r = seq_rbar_paint(&s, LANE_CHORDS, 0, RC_HOLD) + seq_rbar_paint(&s, LANE_CHORDS, 5, RC_HOLD) + seq_rbar_paint(&s, LANE_CHORDS, 3, RC_REST);
    CHECK(!r && bar_reads(&s, 1, "................") && s.undo_n == n + 1, "HOLD at step 1 and after a rest is refused, a rest on a rest is none");
    seq_rbar_paint(&s, LANE_CHORDS, 4, RC_HIT);
    seq_rbar_paint(&s, LANE_CHORDS, 5, RC_HOLD);
    seq_rbar_paint(&s, LANE_CHORDS, 6, RC_HOLD);
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_ACC);
    seq_rbar_paint(&s, LANE_CHORDS, 12, RC_GHOST);
    CHECK(bar_reads(&s, 1, "....x--.A...g...") && s.undo_n == n + 6 && bar_plays_drawn(&s, 1), "HIT, HOLD, HOLD, ACCENT, GHOST: one undo each");
    r = seq_rbar_paint(&s, LANE_CHORDS, 4, RC_ACC);
    CHECK(r == 1 && bar_reads(&s, 1, "....A--.A...g..."), "another hit on a hit: its holds stay");
    r = seq_rbar_paint(&s, LANE_CHORDS, 8, RC_ACC);
    CHECK(!r && bar_reads(&s, 1, "....A--.A...g..."), "0.24: the paint a step has only selects it (0.18 cleared it to rest)");
    r = seq_rbar_paint(&s, LANE_CHORDS, 8, RC_REST);
    CHECK(r == 1 && bar_reads(&s, 1, "....A--.....g..."), "REST clears it");
    r = seq_rbar_paint(&s, LANE_CHORDS, 6, RC_HIT) + seq_rbar_paint(&s, LANE_CHORDS, 7, RC_HOLD);
    CHECK(r == 2 && bar_reads(&s, 1, "....A-x-....g..."),
          "a hit on a hold: the holds after it are its own");
    r = seq_rbar_paint(&s, LANE_CHORDS, 4, RC_REST);
    CHECK(r == 1 && bar_reads(&s, 1, "......x-....g..."), "a cleared hit clears its holds (up to the next hit)");
    seq_rbar_paint(&s, LANE_CHORDS, 4, RC_HIT);
    seq_rbar_paint(&s, LANE_CHORDS, 5, RC_HOLD);
    r = seq_rbar_paint(&s, LANE_CHORDS, 6, RC_HOLD);
    CHECK(r == 1 && bar_reads(&s, 1, "....x---....g..."), "HOLD over a hit: the hit before rings on");
    r = seq_rbar_paint(&s, LANE_CHORDS, 5, RC_REST);
    CHECK(r == 1 && bar_reads(&s, 1, "....x.......g..."), "a hold cleared: the holds after it go too");
    r = seq_rbar_paint(&s, LANE_CHORDS, 16, RC_HIT) + seq_rbar_paint(&s, LANE_CHORDS, -1, RC_HIT) + seq_rbar_paint(&s, LANE_CHORDS, 2, 5) + seq_rbar_paint(&s, LANE_CHORDS, 2, -1);
    CHECK(!r && bar_reads(&s, 1, "....x.......g..."), "a bad step or code does nothing");
    n = s.undo_n;
    seq_undo(&s);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == 1 && bar_reads(&s, 1, "....x---....g...") && s.undo_n == n - 1 && bar_plays_drawn(&s, 1),
          "UNDO steps back one paint, the bar stays open");
    seq_rbar_done(&s, LANE_CHORDS);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && seq_cview(&s) == CVIEW_CHORDS && bar_reads(&s, 1, "....x---....g...") && s.undo_n == n - 1,
          "DONE keeps the paints and their undo steps");
    seq_rbar_open(&s, LANE_CHORDS, 4);
    r = seq_rbar_paint(&s, LANE_CHORDS, 0, RC_REST);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && !r, "bar 5 of a 4-bar loop does not open");
}

static void test_18_copy(void) {
    static seq_t s;
    int rep = -1, skip = -1, n, ok = 1;
    rbar_t at[SEQ_MAX_BARS];
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    bar_set(&s, 4, 3);
    bar_set(&s, 9, 6);
    bar_set(&s, 7, 3);
    bar_lock(&s, 7);
    seq_rbar_open(&s, LANE_CHORDS, 2);
    n = seq_rbar_copy_counts(&s, LANE_CHORDS, 0xFFFF, &rep, &skip);
    CHECK(n == 14 && rep == 2 && skip == 1, "COPY TO ALL from bar 3: replaces 14 bars, 2 of them different, skips 1 locked (%d, %d, %d)",
          n, rep, skip);
    memcpy(at, s.part[0].rb[DL_CHORDS], sizeof at);
    n = s.undo_n;
    seq_rbar_copy(&s, LANE_CHORDS, 0xFFFF);
    for (int b = 0; b < 16; b++)
        ok &= b == 7 ? bar_is(&s, 7, 3) && seq_bar_locked(&s, LANE_CHORDS, 7) : bar_is(&s, b, 0) && !seq_bar_locked(&s, LANE_CHORDS, b) && bar_plays_drawn(&s, b);
    CHECK(ok && s.undo_n == n + 1 && seq_rbar_bar(&s, LANE_CHORDS) == 2, "every unlocked bar is bar 3's, locked bar 8 kept; one undo step, the bar open");
    seq_undo(&s);
    CHECK(!memcmp(at, s.part[0].rb[DL_CHORDS], sizeof at) && s.undo_n == n && seq_rbar_bar(&s, LANE_CHORDS) == 2 && bar_plays_drawn(&s, 4) && bar_plays_drawn(&s, 9),
          "one UNDO puts every copied bar back");
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_ACC);   /* bar 3: x-------A------- */
    s.part[0].rb[DL_CHORDS][2].from = 5;
    n = seq_rbar_copy_counts(&s, LANE_CHORDS, 1u << 2 | 1u << 10 | 1u << 14 | 1u << 7, &rep, &skip);
    CHECK(n == 2 && rep == 2 && skip == 1, "a mask counts only its own bars, not the source (%d, %d, %d)", n, rep, skip);
    /* COPY TO... */
    seq_rcopy_open(&s, LANE_CHORDS);
    CHECK(seq_cview(&s) == CVIEW_COPY && seq_rbar_bar(&s, LANE_CHORDS) == 2 && seq_rcopy_mask(&s, LANE_CHORDS) == 0 && seq_follow_paused(&s, TAB_CHORDS) == 1,
          "COPY TO... opens in the bar: CVIEW_COPY, nothing picked");
    n = s.undo_n;
    seq_rcopy_go(&s, LANE_CHORDS);
    CHECK(seq_cview(&s) == CVIEW_COPY && s.undo_n == n, "COPY with nothing picked does nothing");
    seq_rcopy_toggle(&s, LANE_CHORDS, 2);   /* the source */
    seq_rcopy_toggle(&s, LANE_CHORDS, 7);   /* locked */
    seq_rcopy_toggle(&s, LANE_CHORDS, 16);
    seq_rcopy_toggle(&s, LANE_CHORDS, -1);
    CHECK(seq_rcopy_mask(&s, LANE_CHORDS) == 0, "the source, a locked bar and bars outside the loop are not picked");
    seq_rcopy_set_all(&s, LANE_CHORDS, 1);
    CHECK(seq_rcopy_mask(&s, LANE_CHORDS) == (0xFFFFu & ~(1u << 2 | 1u << 7)), "ALL: every bar but the source and the locked one");
    seq_rcopy_set_all(&s, LANE_CHORDS, 0);
    CHECK(seq_rcopy_mask(&s, LANE_CHORDS) == 0, "NONE");
    seq_rcopy_toggle(&s, LANE_CHORDS, 10);
    seq_rcopy_toggle(&s, LANE_CHORDS, 14);
    seq_rcopy_toggle(&s, LANE_CHORDS, 5);
    seq_rcopy_toggle(&s, LANE_CHORDS, 5);
    seq_rcopy_toggle(&s, LANE_CHORDS, 1);
    CHECK(seq_rcopy_mask(&s, LANE_CHORDS) == (1u << 1 | 1u << 10 | 1u << 14), "taps pick and unpick");
    memcpy(at, s.part[0].rb[DL_CHORDS], sizeof at);
    seq_rcopy_go(&s, LANE_CHORDS);
    ok = 1;
    for (int b = 0; b < 16; b++) {
        const rbar_t *r = &s.part[0].rb[DL_CHORDS][b], *src = &s.part[0].rb[DL_CHORDS][2];
        if (b == 1 || b == 10 || b == 14) ok &= !memcmp(r->c, src->c, 16) && r->from == 5 && r->ed == 1 && !r->lock && bar_plays_drawn(&s, b);
        else ok &= !memcmp(r, &at[b], sizeof *r);
    }
    CHECK(ok && s.undo_n == n + 1 && seq_cview(&s) == CVIEW_BAR && seq_rbar_bar(&s, LANE_CHORDS) == 2,
          "COPY TO 3 BARS copies the whole bar (codes, from, edited) to those only, one undo step, and stays in the bar");
    n = s.undo_n;
    seq_rbar_copy(&s, LANE_CHORDS, 0);
    seq_rbar_copy(&s, LANE_CHORDS, 1u << 2 | 1u << 7);
    seq_rbar_copy(&s, LANE_CHORDS, 1u << 1);   /* already the same */
    CHECK(s.undo_n == n && !memcmp(&s.part[0].rb[DL_CHORDS][7], &at[7], sizeof at[7]), "an empty mask, the source, a locked bar or a same bar: nothing");
    seq_rcopy_open(&s, LANE_CHORDS);
    seq_rcopy_toggle(&s, LANE_CHORDS, 3);
    seq_rcopy_close(&s, LANE_CHORDS);
    CHECK(seq_cview(&s) == CVIEW_BAR && seq_rcopy_mask(&s, LANE_CHORDS) == 0 && s.undo_n == n, "COPY TO...'s CANCEL closes it, copying nothing");
    seq_rcopy_open(&s, LANE_CHORDS);
    seq_undo(&s);
    CHECK(seq_cview(&s) == CVIEW_BAR && s.undo_n == n, "UNDO with COPY TO... open closes it");
    seq_rbar_lock(&s, LANE_CHORDS);   /* a locked source copies its codes, not its lock */
    seq_rbar_copy(&s, LANE_CHORDS, 1u << 0);
    CHECK(!memcmp(s.part[0].rb[DL_CHORDS][0].c, s.part[0].rb[DL_CHORDS][2].c, 16) && !s.part[0].rb[DL_CHORDS][0].lock && seq_bar_locked(&s, LANE_CHORDS, 2), "a copy never copies the lock");
    seq_rbar_done(&s, LANE_CHORDS);
    n = seq_rbar_copy_counts(&s, LANE_CHORDS, 0xFFFF, &rep, &skip);
    CHECK(n == 0 && rep == 0 && skip == 0, "no bar open: no counts");
}

static void test_18_cancel_restores_copies(void) {
    static seq_t s;
    rbar_t at[SEQ_MAX_BARS];
    seq_step_t steps[SEQ_LANE_STEPS];
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    bar_set(&s, 3, 6);
    bar_lock(&s, 9);
    seq_set_step(&s, LANE_BASS, 0, 3, 1);   /* undo steps from before */
    int n = s.undo_n;
    memcpy(at, s.part[0].rb[DL_CHORDS], sizeof at);
    seq_rbar_open(&s, LANE_CHORDS, 6);
    seq_rbar_paint(&s, LANE_CHORDS, 4, RC_ACC);
    seq_rbar_lock(&s, LANE_CHORDS);
    seq_rbar_copy(&s, LANE_CHORDS, 0xFFFF);
    seq_rbar_open(&s, LANE_CHORDS, 6);   /* reopened: the edits are kept by its DONE, and the new transaction starts from them */
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(seq_bar_locked(&s, LANE_CHORDS, 6) && bar_reads(&s, 0, "x---A-----------") && s.undo_n == n + 3, "a reopen's cancel keeps the earlier DONE's edits");
    for (int k = 0; k < 3; k++) seq_undo(&s);
    CHECK(!memcmp(at, s.part[0].rb[DL_CHORDS], sizeof at) && s.undo_n == n, "and three UNDOs step them back");
    seq_rbar_open(&s, LANE_CHORDS, 6);
    seq_rbar_paint(&s, LANE_CHORDS, 4, RC_ACC);
    seq_rbar_lock(&s, LANE_CHORDS);
    seq_rbar_copy(&s, LANE_CHORDS, 0xFFFF);
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 2);
    seq_set_complexity(&s, LANE_CHORDS, 70);
    CHECK(seq_bar_locked(&s, LANE_CHORDS, 6) && bar_reads(&s, 3, "x---A-----------") && seq_bar_locked(&s, LANE_CHORDS, 9) && s.undo_n == n + 3, "painted, locked, copied");
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(!memcmp(at, s.part[0].rb[DL_CHORDS], sizeof at) && !seq_bar_locked(&s, LANE_CHORDS, 6) && seq_bar_locked(&s, LANE_CHORDS, 9), "CANCEL: every bar and lock as at open");
    CHECK(s.undo_n == n && s.undo[(s.undo_top + SEQ_UNDO - 1) % SEQ_UNDO].lane == LANE_BASS, "and the undo depth as before it opened");
    CHECK(seq_rknob(&s, LANE_CHORDS, RK_GHOSTS) == 2 && seq_complexity(&s, LANE_CHORDS) == 70, "knob turns made meanwhile keep their values");
    memcpy(steps, s.part[0].lane[LANE_CHORDS].steps, sizeof steps);
    render_now(&s);
    CHECK(!memcmp(steps, s.part[0].lane[LANE_CHORDS].steps, sizeof steps), "and the lane is rendered from the restored bars");
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(!memcmp(at, s.part[0].rb[DL_CHORDS], sizeof at) && s.undo_n == n, "a second CANCEL is a no-op");
    /* a full undo ring: a cancel gives back the snapshots the bar's pushes dropped */
    for (int k = 0; k < SEQ_UNDO + 2; k++) seq_set_step(&s, LANE_BASS, 1, 0, k & 1);
    seq_snap_t ring[SEQ_UNDO];
    memcpy(ring, s.undo, sizeof ring);
    seq_rbar_open(&s, LANE_CHORDS, 1);
    for (int k = 0; k < 4; k++) seq_rbar_paint(&s, LANE_CHORDS, 4 + k, RC_HIT);
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(s.undo_n == SEQ_UNDO && !memcmp(ring, s.undo, sizeof ring) && bar_is(&s, 1, 0), "a full ring comes back whole");
}

/* Task 5's review: a knob turn with a bar open pushes no undo step and leaves it open; CANCEL keeps it */
static void test_18_rknob_no_undo(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_rbar_open(&s, LANE_CHORDS, 2);
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    int n = s.undo_n;
    seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, 2);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 3);
    seq_set_complexity(&s, LANE_CHORDS, 20);
    CHECK(s.undo_n == n && seq_rbar_bar(&s, LANE_CHORDS) == 2 && seq_cview(&s) == CVIEW_BAR, "RHYTHM knobs and COMPLEXITY: no undo step, the bar stays open");
    seq_undo(&s);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == 2 && bar_is(&s, 2, 0) && seq_rknob(&s, LANE_CHORDS, RK_SYNC) == 2 && seq_complexity(&s, LANE_CHORDS) == 20,
          "UNDO steps back the paint, not the knobs");
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(bar_is(&s, 2, 0) && seq_rknob(&s, LANE_CHORDS, RK_SYNC) == 2 && seq_rknob(&s, LANE_CHORDS, RK_FILL) == 3 && seq_complexity(&s, LANE_CHORDS) == 20,
          "CANCEL keeps the knob values");
    seq_rbar_open(&s, LANE_CHORDS, 2);
    seq_undo(&s);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && seq_complexity(&s, LANE_CHORDS) == 20, "UNDO with none of its own steps left is the bar's CANCEL");
}

/* Task 5's review: a bar locked through the API never plays its fill variant */
static void test_18_locked_bar_no_fill(void) {
    static seq_t s;
    unsigned m[16];
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 1);
    bar_lock(&s, 7);
    nev = 0;
    play(&s, 0, 64.0, 0);
    seq_block(&s, 64.0, bpm, 0, 128);
    chord_masks(&s, m, 16);
    CHECK(seq_bar_locked(&s, LANE_CHORDS, 7) && fills_on(m, 0, 16, fill_4th_not8), "bar 8 locked with LOCK: no fill there; bars 4, 12 and 16 fill");
}

static void test_18_pick_targets(void) {
    static seq_t s;
    int ed = 1, n;
    seq_init(&s, emit, NULL);
    bar_lock(&s, 1);
    seq_rbar_open(&s, LANE_CHORDS, 3);
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    n = s.undo_n;
    int got = seq_rhythm_pick(&s, LANE_CHORDS, 5);
    CHECK(got == 1 && bar_is(&s, 3, 5) && seq_bar_rhythm(&s, LANE_CHORDS, 3, &ed) == 5 && !ed && bar_plays_drawn(&s, 3) && bar_is(&s, 0, 0) &&
          bar_is(&s, 2, 0) && s.undo_n == n + 1 && seq_rbar_bar(&s, LANE_CHORDS) == 3,
          "a bar open: a pick fills that bar only (not edited), one undo step, the bar open; returns 1");
    got = seq_rhythm_pick(&s, LANE_CHORDS, 5) | seq_rhythm_pick(&s, LANE_CHORDS, RHYTHM_COUNT) | seq_rhythm_pick(&s, LANE_CHORDS, -1);
    CHECK(!got && s.undo_n == n + 1 && bar_is(&s, 3, 5), "the same pick or a bad one: nothing, returns 0");
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(bar_is(&s, 3, 0) && s.undo_n == n - 1, "CANCEL takes the pick back with the paint");
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rhythm_pick(&s, LANE_CHORDS, 7);
    CHECK(bar_is(&s, 1, 0) && s.undo_n == n - 1, "a locked open bar keeps its own");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rhythm_pick(&s, LANE_CHORDS, 7);
    CHECK(bar_is(&s, 0, 7) && bar_is(&s, 1, 0) && bar_is(&s, 2, 7) && bar_is(&s, 3, 7) && s.undo_n == n && seq_rbar_bar(&s, LANE_CHORDS) == -1 &&
          bar_plays_drawn(&s, 2), "none open: every unlocked bar, one undo step");
    seq_undo(&s);
    CHECK(bar_is(&s, 0, 0) && bar_is(&s, 2, 0) && bar_is(&s, 3, 0), "and one UNDO takes it back");
    seq_browse_open(&s, CVIEW_PROG);
    seq_rhythm_pick(&s, LANE_CHORDS, 2);
    CHECK(s.txn.kind == CVIEW_CHORDS && bar_is(&s, 0, 2), "a pick closes an open PROGRESSIONS browser (DONE) first");
}

static void test_18_browser(void) {
    static seq_t s;
    rbar_t at[SEQ_MAX_BARS];
    seq_init(&s, emit, NULL);
    bar_lock(&s, 2);
    bar_set(&s, 0, 9);   /* Garage Skip, Dance */
    int n = s.undo_n;
    memcpy(at, s.part[0].rb[DL_CHORDS], sizeof at);
    seq_rbrowse_open(&s, LANE_CHORDS);
    CHECK(seq_cview(&s) == CVIEW_RBROWSE && seq_follow_paused(&s, TAB_CHORDS) == 1 && s.undo_n == n + 1 && seq_rbrowse_group(&s, LANE_CHORDS) == 2 &&
          seq_rbar_bar(&s, LANE_CHORDS) == -1, "RHYTHMS opens: CVIEW_RBROWSE, FOLLOW paused, one undo step, the chip bar 1's feel (Dance)");
    seq_rbrowse_try(&s, LANE_CHORDS, 3);
    seq_rbrowse_try(&s, LANE_CHORDS, 12);
    seq_rbrowse_try(&s, LANE_CHORDS, 20);
    CHECK(bar_is(&s, 0, 20) && bar_is(&s, 1, 20) && bar_is(&s, 3, 20) && bar_is(&s, 2, 0) && s.undo_n == n + 1 && bar_plays_drawn(&s, 3),
          "a try fills every unlocked bar at once, with no undo step");
    seq_browse_cancel(&s);
    CHECK(!memcmp(at, s.part[0].rb[DL_CHORDS], sizeof at) && s.undo_n == n && seq_cview(&s) == CVIEW_CHORDS && bar_plays_drawn(&s, 0),
          "CANCEL after 3 tries: as before, no undo step");
    seq_rbrowse_open(&s, LANE_CHORDS);
    seq_rbrowse_set_group(&s, LANE_CHORDS, 4);
    CHECK(seq_rbrowse_group(&s, LANE_CHORDS) == 4 && bar_is(&s, 0, 16) && bar_is(&s, 3, 16), "a chip tap applies its group's first rhythm (Skank)");
    seq_rbrowse_set_group(&s, LANE_CHORDS, RHYTHM_GROUPS);
    seq_rhythm_pick(&s, LANE_CHORDS, 17);   /* the field's arrows while it is open: a try */
    CHECK(seq_rbrowse_group(&s, LANE_CHORDS) == 4 && bar_is(&s, 1, 17) && s.undo_n == n + 1, "a bad chip is ignored; a pick while open is a try");
    seq_browse_done(&s);
    CHECK(bar_is(&s, 1, 17) && seq_cview(&s) == CVIEW_CHORDS && s.undo_n == n + 1, "DONE keeps it: one undo step");
    seq_undo(&s);
    CHECK(!memcmp(at, s.part[0].rb[DL_CHORDS], sizeof at), "which UNDO takes back");
    seq_rbrowse_open(&s, LANE_CHORDS);
    seq_rbrowse_try(&s, LANE_CHORDS, 4);
    seq_rbrowse_try(&s, LANE_CHORDS, 9);
    seq_rbrowse_try(&s, LANE_CHORDS, 0);
    seq_rbrowse_try(&s, LANE_CHORDS, 9);
    s.part[0].rb[DL_CHORDS][1] = at[1], s.part[0].rb[DL_CHORDS][3] = at[3];
    seq_browse_done(&s);
    CHECK(s.undo_n == n, "DONE with nothing changed leaves no undo step");
    seq_rbrowse_open(&s, LANE_CHORDS);
    seq_undo(&s);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && s.undo_n == n, "UNDO with it open is its CANCEL");
    /* in an open bar: it fills that bar, and closes back to the bar */
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    seq_rbrowse_open(&s, LANE_CHORDS);
    CHECK(seq_cview(&s) == CVIEW_RBROWSE && seq_rbar_bar(&s, LANE_CHORDS) == 1 && seq_rbrowse_group(&s, LANE_CHORDS) == 0 && s.undo_n == n + 2,
          "RHYTHMS in bar 2: its own undo step, the chip bar 2's feel");
    seq_rbrowse_try(&s, LANE_CHORDS, 4);
    CHECK(bar_is(&s, 1, 4) && bar_is(&s, 0, 9) && bar_is(&s, 3, 0), "a try fills the open bar only");
    seq_browse_cancel(&s);
    CHECK(bar_reads(&s, 1, "x-------x-------") && seq_cview(&s) == CVIEW_BAR && s.undo_n == n + 1, "CANCEL: the bar as the browser found it, still open");
    seq_rbrowse_open(&s, LANE_CHORDS);
    seq_rbrowse_try(&s, LANE_CHORDS, 4);
    seq_browse_done(&s);
    CHECK(bar_is(&s, 1, 4) && seq_cview(&s) == CVIEW_BAR && s.undo_n == n + 2, "DONE: kept, one undo step, back in the bar");
    seq_rbrowse_open(&s, LANE_CHORDS);
    seq_undo(&s);
    CHECK(seq_cview(&s) == CVIEW_BAR && bar_is(&s, 1, 4) && s.undo_n == n + 2, "UNDO in the bar's browser is the browser's CANCEL");
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(!memcmp(at, s.part[0].rb[DL_CHORDS], sizeof at) && s.undo_n == n && seq_cview(&s) == CVIEW_CHORDS, "the bar's CANCEL takes it all back");
}

static void test_18_rbar_closed_by_switch(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    rbar_t at[SEQ_MAX_BARS];
    for (int how = 0; how < 4; how++) {
        double ppq = 0;
        seq_init(&s, emit, NULL);
        seq_state_save(&s, buf, sizeof buf);
        if (how == 3) {   /* playing: the switch queued first, the bar opened on the part being left */
            ppq = play(&s, 0.0, 1.0, 0);
            seq_select_part(&s, 1);
        }
        seq_rbar_open(&s, LANE_CHORDS, 1);
        seq_rbar_paint(&s, LANE_CHORDS, 0, RC_REST);
        if (how == 0) seq_select_part(&s, 1);
        else if (how == 1) seq_state_load(&s, buf);
        else if (how == 2) seq_init_preset(&s);
        else {   /* the switch at the bar line flags it: read as closed at once, cancelled by the next setter */
            play(&s, ppq, 4.0, 0);
            CHECK(s.cur_part == 1 && s.txn.kind && seq_rbar_bar(&s, LANE_CHORDS) == -1 && seq_cview(&s) == CVIEW_CHORDS, "a queued switch: read as closed");
            seq_rbar_paint(&s, LANE_CHORDS, 4, RC_HIT);
        }
        CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && seq_cview(&s) == CVIEW_CHORDS && !s.txn.kind && bar_is(&s, 1, 0),
              "%s ends the bar (cancelled)", (const char *[]){"a part switch", "a load", "INIT", "a queued switch"}[how]);
        if (how == 0 || how == 3) CHECK(s.cur_part == 1 && s.part[0].rb[DL_CHORDS][1].c[0] == RC_HIT, "(%d) the part left has its bar as before", how);
        memcpy(at, s.part[s.cur_part].rb[DL_CHORDS], sizeof at);
        int n = s.undo_n;
        seq_rbar_cancel(&s, LANE_CHORDS);
        seq_rbar_done(&s, LANE_CHORDS);
        CHECK(!memcmp(at, s.part[s.cur_part].rb[DL_CHORDS], sizeof at) && s.undo_n == n, "(%d) a later CANCEL or DONE is a no-op", how);
    }
}

/* Ruling R32: a view change (CHORDS / RHYTHM / FIX) with a bar open closes it as DONE: its edits and their undo steps stay, from the
 * bar itself, its COPY TO... or its RHYTHMS browser; a browser or picker opened on its own is still cancelled */
static void test_18_rbar_kept_by_view(void) {
    static seq_t s;
    int n;
    for (int sub = 0; sub < 3; sub++) {
        seq_init(&s, emit, NULL);   /* Sustain: x--------------- */
        seq_set_cview(&s, CVIEW_RHYTHM);
        n = s.undo_n;
        seq_rbar_open(&s, LANE_CHORDS, 1);
        seq_rbar_paint(&s, LANE_CHORDS, 4, RC_HIT);
        seq_rbar_paint(&s, LANE_CHORDS, 8, RC_ACC);
        seq_rbar_paint(&s, LANE_CHORDS, 12, RC_GHOST);
        if (sub == 1) seq_rcopy_open(&s, LANE_CHORDS), seq_rcopy_toggle(&s, LANE_CHORDS, 2);
        if (sub == 2) seq_rbrowse_open(&s, LANE_CHORDS), seq_rbrowse_try(&s, LANE_CHORDS, 4);
        seq_set_cview(&s, sub == 1 ? CVIEW_JAM : CVIEW_CHORDS);   /* 0.24 JAM in FIX's place (retired) */
        CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && !s.txn.kind && seq_cview(&s) == (sub == 1 ? CVIEW_JAM : CVIEW_CHORDS),
              "(%d) the view change closes the bar", sub);
        if (sub == 2) {
            CHECK(bar_is(&s, 1, 4) && s.undo_n == n + 4, "with RHYTHMS open: its try kept as DONE, 3 paints + the browser's step");
            seq_undo(&s);
        }
        CHECK(bar_reads(&s, 1, "x---x---A---g---") && s.undo_n == n + 3 && bar_is(&s, 2, 0),
              "(%d) the 3 paints kept (%d undo steps), COPY TO... not applied", sub, s.undo_n - n);
        seq_undo(&s);
        CHECK(bar_reads(&s, 1, "x---x---A-------"), "(%d) UNDO steps the last paint back", sub);
        seq_undo(&s);
        seq_undo(&s);
        CHECK(bar_reads(&s, 1, "x---------------") && s.undo_n == n, "(%d) and the others, one at a time", sub);
    }
    seq_init(&s, emit, NULL);
    seq_set_cview(&s, CVIEW_RHYTHM);
    n = s.undo_n;
    seq_rbrowse_open(&s, LANE_CHORDS);
    seq_rbrowse_try(&s, LANE_CHORDS, 4);
    seq_set_cview(&s, CVIEW_CHORDS);
    CHECK(bar_is(&s, 1, 0) && s.undo_n == n && !s.txn.kind, "the RHYTHMS browser on its own: still cancelled");
    int deg = seq_slot_deg(&s, 1);
    seq_browse_open(&s, CVIEW_PROG);   /* 0.24.1 the chord picker is gone */
    seq_prog_try(&s, 2);
    seq_set_cview(&s, CVIEW_JAM);   /* 0.24 FIX retired */
    CHECK(seq_slot_deg(&s, 1) == deg && s.undo_n == n && !s.txn.kind, "the PROGRESSIONS browser: still cancelled");
}

/* As a picker's pick: an open bar's edits are saved once it closes (the state as at open is written meanwhile) */
static void test_18_rbar_not_saved(void) {
    static seq_t s;
    static char a[SEQ_STATE_MAX], b[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_state_save(&s, a, sizeof a);
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rbar_paint(&s, LANE_CHORDS, 0, RC_ACC);
    seq_rbar_lock(&s, LANE_CHORDS);
    seq_state_save(&s, b, sizeof b);
    CHECK(!strcmp(a, b) && bar_reads(&s, 1, "A---------------") && seq_bar_locked(&s, LANE_CHORDS, 1) && seq_rbar_bar(&s, LANE_CHORDS) == 1,
          "a save with a bar open writes the bars as at open; the bar keeps its edits");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_state_save(&s, b, sizeof b);
    CHECK(strcmp(a, b) && strstr(b, ".rb=0,L0A---------------,0,0"), "after DONE they are saved");
}

static void test_18_rbar_shrink(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    int n = s.undo_n;
    seq_rbar_open(&s, LANE_CHORDS, 12);
    seq_rbar_paint(&s, LANE_CHORDS, 0, RC_REST);
    seq_set_bars(&s, 8);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && !s.txn.kind && bar_is(&s, 12, 0) && s.undo_n == n && seq_bars(&s) == 8,
          "bar 13 open, the loop shrinks to 8: cancelled, bar 13 as it was, no undo step left");
    seq_rbar_open(&s, LANE_CHORDS, 4);
    seq_rbar_paint(&s, LANE_CHORDS, 0, RC_REST);
    seq_set_bars(&s, 4);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && bar_is(&s, 4, 0) && s.undo_n == n, "bar 5 open, a shrink to 4 bars: cancelled");
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rbar_paint(&s, LANE_CHORDS, 0, RC_REST);
    seq_set_bars(&s, 2);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && bar_reads(&s, 1, "................") && s.undo_n == n + 1, "a shrink that keeps the bar: DONE");
    seq_rbar_open(&s, LANE_CHORDS, 0);
    seq_rbar_paint(&s, LANE_CHORDS, 0, RC_REST);
    seq_set_chord_len(&s, 8);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) == -1 && bar_reads(&s, 0, "................"), "a chord-length change: DONE");
}

static void test_18_follow_pause(void) {
    static seq_t s;
    double ppq;
    strip_16(&s);
    ppq = play(&s, 0.0, 20.5, 0);   /* bar 6 */
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == 1, "playing bar 6: running, page 2");
    seq_rbar_open(&s, LANE_CHORDS, 1);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 1 && seq_strip_page(&s, TAB_CHORDS) == 0, "bar 2 open: paused, the strip holds its page");
    ppq = play(&s, ppq, 16.0, 0);   /* bar 10 */
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 1 && seq_strip_page(&s, TAB_CHORDS) == 0, "held while open");
    ppq = play(&s, ppq, 8.0, 0);   /* bar 12 */
    seq_rbar_done(&s, LANE_CHORDS);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_strip_page(&s, TAB_CHORDS) == 2 && seq_strip_page(&s, TAB_CHORDS) == seq_cur_slot(&s) / 4,
          "DONE: running again, at the playing page (%d)", seq_strip_page(&s, TAB_CHORDS));
    seq_strip_turn(&s, TAB_CHORDS, 1);
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2, "opened while paged: CANCEL goes back to paging");
    (void)ppq;
}

/* Task 8 carry: a Chords step tap (PLAY) and a style_1 pick change rb only after closing an open bar as DONE: the core is safe by
 * itself, whoever calls it */
static void test_18_core_closes_bar(void) {
    static seq_t s;
    int n;
    seq_init(&s, emit, NULL);   /* Sustain: x--------------- */
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rbar_paint(&s, LANE_CHORDS, 4, RC_HIT);
    seq_set_step(&s, LANE_CHORDS, 2, 8, 1);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) < 0 && bar_reads(&s, 1, "x---x-----------") && bar_reads(&s, 2, "x-------x-------"),
          "a PLAY tap on Chords closes the open bar as DONE (its paint kept), then draws its hit");
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rbar_paint(&s, LANE_CHORDS, 0, RC_ACC);
    n = s.undo_n;
    seq_set_style(&s, LANE_CHORDS, GEN_CH_PULSE);
    CHECK(seq_rbar_bar(&s, LANE_CHORDS) < 0 && s.undo_n == n + 1 && seq_bar_rhythm(&s, LANE_CHORDS, 1, NULL) == seq_bar_rhythm(&s, LANE_CHORDS, 3, NULL) &&
          seq_bar_rhythm(&s, LANE_CHORDS, 1, NULL) >= 0, "a style_1 pick closes it as DONE too, then fills every bar (one undo step)");
    seq_undo(&s);
    CHECK(bar_reads(&s, 1, "A---x-----------"), "UNDO takes the pick back to the bar as DONE kept it");
}

/* Ruling R22: on the RHYTHM views the strip pages by bars (4 a page), not chord slots; FOLLOW off's page converts on a view change */
static void test_18_strip_by_bars(void) {
    static seq_t s;
    double ppq;
    strip_16(&s);
    seq_set_chord_len(&s, 4);   /* 1/4-bar chords: 64 slots, 16 slot pages */
    CHECK(!seq_strip_by_bars(&s, TAB_CHORDS) && seq_strip_pages(&s, TAB_CHORDS) == 16, "CHORDS: 16 pages of chords");
    seq_set_cview(&s, CVIEW_RHYTHM);
    CHECK(seq_strip_by_bars(&s, TAB_CHORDS) && seq_strip_pages(&s, TAB_CHORDS) == 4 && seq_strip_page(&s, TAB_CHORDS) == 0, "RHYTHM: 4 pages of bars");
    ppq = play(&s, 0.0, 20.5, 0);   /* bar 6 */
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 1, "playing bar 6: page 2 (bars 5-8), got %d", seq_strip_page(&s, TAB_CHORDS));
    seq_rbar_open(&s, LANE_CHORDS, 9);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 2 && seq_strip_by_bars(&s, TAB_CHORDS), "bar 10 open: its page (bars 9-12)");
    ppq = play(&s, ppq, 8.0, 0);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 2, "held while open");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_set_chord_len(&s, 32);   /* 2-bar chords: 8 slots, 2 slot pages, still 4 bar pages */
    seq_rbar_open(&s, LANE_CHORDS, 13);
    CHECK(seq_strip_pages(&s, TAB_CHORDS) == 4 && seq_strip_page(&s, TAB_CHORDS) == 3, "2-bar chords: bar 14 open on page 4 of 4 (got %d)", seq_strip_page(&s, TAB_CHORDS));
    seq_rbar_cancel(&s, LANE_CHORDS);
    seq_set_follow(&s, 0);     /* the page shown (bar 8's) stays */
    seq_strip_turn(&s, TAB_CHORDS, 1);     /* by hand: bars 9-12 */
    seq_set_cview(&s, CVIEW_CHORDS);
    CHECK(!seq_strip_by_bars(&s, TAB_CHORDS) && seq_strip_pages(&s, TAB_CHORDS) == 2 && seq_strip_page(&s, TAB_CHORDS) == 1, "to CHORDS: the page of bar 9's chord (got %d)",
          seq_strip_page(&s, TAB_CHORDS));
    seq_set_cview(&s, CVIEW_RHYTHM);
    CHECK(seq_strip_page(&s, TAB_CHORDS) == 2, "and back: bars 9-12 (got %d)", seq_strip_page(&s, TAB_CHORDS));
    (void)ppq;
}

/* 0.19 Task 3: the Bass lane's drawn bars and RHYTHM knobs are its own: the CHORDS rhythm ops (paint, COPY TO ALL, a pick, a
 * CANCEL, RESET KNOBS, UNDO) never touch them, nor the Bass lane's steps */
static void test_19_bass_rb_independent(void) {
    static seq_t s;
    static rbar_t sentinel[SEQ_MAX_BARS];
    static const signed char RK[4] = {-1, 2, 3, 4};
    seq_step_t bass[SEQ_LANE_STEPS];
    unsigned char before[16], after[16];
    int ok = 1, n;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    for (int b = 0; b < SEQ_MAX_BARS; b++) {
        memset(sentinel[b].c, RC_GHOST, 16);
        sentinel[b].from = 7, sentinel[b].ed = 1, sentinel[b].lock = (unsigned char)(b & 1);
    }
    for (int q = 0; q < SEQ_PARTS; q++) {
        memcpy(s.part[q].rb[DL_BASS], sentinel, sizeof sentinel);
        memcpy(s.part[q].rk[DL_BASS], RK, sizeof RK);
    }
    memcpy(bass, s.part[0].lane[LANE_BASS].steps, sizeof bass);
    n = s.undo_n;
    seq_bar_codes(&s, LANE_CHORDS, 1, before);
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rbar_paint(&s, LANE_CHORDS, 4, RC_ACC);   /* paint */
    seq_rbar_copy(&s, LANE_CHORDS, 0xFFFF);       /* COPY TO ALL */
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rhythm_pick(&s, LANE_CHORDS, 5);           /* a pick into every unlocked bar */
    seq_rbar_open(&s, LANE_CHORDS, 2);
    seq_rbar_paint(&s, LANE_CHORDS, 2, RC_HIT);
    seq_rbar_cancel(&s, LANE_CHORDS);              /* CANCEL puts the Chords bars back */
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 2);
    seq_reset_rhythm_knobs(&s, LANE_CHORDS);       /* RESET KNOBS */
    seq_bar_codes(&s, LANE_CHORDS, 1, after);
    CHECK(memcmp(before, after, 16) && s.undo_n > n, "the CHORDS ops changed the Chords bars (%d undo steps)", s.undo_n - n);
    for (int q = 0; q < SEQ_PARTS; q++)
        ok &= !memcmp(s.part[q].rb[DL_BASS], sentinel, sizeof sentinel) && !memcmp(s.part[q].rk[DL_BASS], RK, sizeof RK);
    CHECK(ok && !memcmp(bass, s.part[0].lane[LANE_BASS].steps, sizeof bass), "the Bass bars, knobs and steps are untouched");
    while (s.undo_n > n) seq_undo(&s);             /* UNDO back through all of it */
    seq_bar_codes(&s, LANE_CHORDS, 1, after);
    for (int q = 0; q < SEQ_PARTS; q++)
        ok &= !memcmp(s.part[q].rb[DL_BASS], sentinel, sizeof sentinel) && !memcmp(s.part[q].rk[DL_BASS], RK, sizeof RK);
    CHECK(ok && !memcmp(before, after, 16) && !memcmp(bass, s.part[0].lane[LANE_BASS].steps, sizeof bass),
          "UNDO restores the Chords bars and leaves the Bass ones");
}

/* 0.19 Task 3 (Review Focus 3, part 1): one transaction at a time across both tabs. A Bass bar opening closes the CHORDS
 * browser as DONE (0.24.1 the browser: the picker is gone); the transaction is then the Bass lane's, and a CHORDS CANCEL does nothing to it */
static void test_19_txn_cross_tab(void) {
    static seq_t s;
    rbar_t rb[SEQ_MAX_BARS];
    int n, deg;
    seq_init(&s, emit, NULL);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 2);
    deg = s.part[0].prog[1];
    CHECK(seq_cview(&s) == CVIEW_PROG && seq_follow_paused(&s, TAB_CHORDS) == 1, "the PROGRESSIONS browser is open, FOLLOW paused there");
    seq_rbar_open(&s, LANE_BASS, 2);
    CHECK(seq_cview(&s) == CVIEW_CHORDS && s.part[0].prog[1] == deg && s.undo_n == 1,
          "a Bass bar opening closes the browser as DONE (the try and its step stay)");
    CHECK(s.txn.kind == CVIEW_BAR && s.txn.lane == LANE_BASS && seq_rbar_bar(&s, LANE_BASS) == 2 && seq_rbar_bar(&s, LANE_CHORDS) == -1,
          "the open transaction is the Bass lane's bar 3");
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_follow_paused(&s, TAB_BASS) == 1 && !seq_strip_by_bars(&s, TAB_CHORDS) &&
          seq_strip_by_bars(&s, TAB_BASS), "it pauses the Bass tab's FOLLOW, not the CHORDS tab's");
    n = s.undo_n;
    memcpy(rb, s.part[0].rb[DL_BASS], sizeof rb);
    seq_rbar_cancel(&s, LANE_CHORDS);
    seq_rbar_done(&s, LANE_CHORDS);
    CHECK(s.txn.kind == CVIEW_BAR && s.txn.lane == LANE_BASS && seq_rbar_bar(&s, LANE_BASS) == 2 && s.undo_n == n &&
          !memcmp(rb, s.part[0].rb[DL_BASS], sizeof rb), "seq_rbar_cancel(LANE_CHORDS) and _done are no-ops");
    seq_rbar_paint(&s, LANE_BASS, 4, RC_HIT);
    seq_rbar_cancel(&s, LANE_BASS);
    CHECK(!s.txn.kind && s.undo_n == n && !memcmp(rb, s.part[0].rb[DL_BASS], sizeof rb), "seq_rbar_cancel(LANE_BASS) closes it, bars back");
}

/* 0.19 Task 4: the drawn Bass lane (GEN_BASS_DRAWN). Bass library 0 is Root, A---------------; Eighths is 3 */
#define BASS_POP_ROOT 40
#define BASS_OCTAVE_BOUNCE 1

/* 1 when the part's Bass lane is DRAWN at COMPLEXITY 50 with Pop Root's settings and Root in every bar of the loop */
static int bass_default(const seq_t *s, int part) {
    const seq_part_t *p = &s->part[part];
    bass_set_t pop = bass_style_set(BASS_POP_ROOT);
    int ok = p->lane[LANE_BASS].gen.style == GEN_BASS_DRAWN && p->lane[LANE_BASS].gen.complexity == BASS_CX_NEUTRAL &&
             !memcmp(&p->bass.set, &pop, sizeof pop);
    for (int b = 0; b < p->bars; b++) ok &= p->rb[DL_BASS][b].from == 0 && !p->rb[DL_BASS][b].ed && !p->rb[DL_BASS][b].lock;
    return ok;
}

static void test_19_init_bass(void) {
    static seq_t s;
    seq_step_t old[SEQ_LANE_STEPS];
    int same = 1, pcs = 1, range = 1;
    seq_init(&s, emit, NULL);
    CHECK(bass_default(&s, 0), "a new instance: Bass DRAWN at COMPLEXITY 50, Pop Root, Root in every bar");
    seq_init_preset(&s);
    CHECK(bass_default(&s, 0), "INIT: Bass DRAWN at COMPLEXITY 50, Pop Root, Root in every bar");
    const seq_part_t *p = &s.part[0];
    gen_params_t gp = {.style = GEN_BASS_ROOT, .seed = 1, .complexity = 0, .bars = p->bars, .chord_len = p->chord_len,
                       .prog = p->prog, .prog_len = p->prog_len, .key = s.key, .scale = s.scale, .lane = LANE_BASS};
    gen_lane_v1(&gp, old);   /* 0.18's INIT Bass: Root at COMPLEXITY 0 */
    for (int i = 0; i < p->bars * SEQ_STEPS; i++) {
        const seq_step_t *x = &p->lane[LANE_BASS].steps[i];
        int slot = seq_slot_at(&s, i / SEQ_STEPS, i % SEQ_STEPS);
        same &= x->on == old[i].on && (!x->on || x->len == old[i].len);
        if (!x->on) continue;
        pcs &= x->abs % 12 == theory_degree_pc(s.key, s.scale, p->prog[slot]) &&
               seq_part_step_note(&s, 0, LANE_BASS, i / SEQ_STEPS, i % SEQ_STEPS) == x->abs;
        range &= x->abs >= 28 && x->abs <= 39;
    }
    CHECK(same, "INIT's Bass onsets and lengths are 0.18's (Root at COMPLEXITY 0: one note held a bar)");
    CHECK(pcs, "its pitch classes are the chord roots (C G A F)");
    CHECK(range, "its notes are in E1-D#2 (REGISTER Natural)");
    static const int WANT[4] = {36, 31, 33, 29};   /* C2 G1 A1 F1 */
    int exact = p->bars == 4;
    for (int b = 0; b < 4 && exact; b++) {
        const seq_step_t *x = &p->lane[LANE_BASS].steps[b * SEQ_STEPS];
        exact &= x->on && x->abs == WANT[b];
    }
    CHECK(exact, "INIT's Bass notes exactly: C2 36, G1 31, A1 33, F1 29");
}

/* The reference's spot line (node tests/ref/bass_ref.js): Octave Bounce on Eighths over E minor Em C G D, bar 0 */
static void test_19_render_style(void) {
    static seq_t s;
    static const unsigned char WANT0[16] = {28, 0, 40, 0, 28, 0, 40, 0, 28, 0, 40, 0, 28, 0, 40, 0};
    unsigned char note[16];
    char role[16];
    int ok1 = 1;
    seq_init(&s, emit, NULL);   /* E minor, Em C G D */
    s.part[0].bass.set = bass_style_set(BASS_OCTAVE_BOUNCE);
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths in every bar: renders */
    seq_bass_line(&s, 0, note, role);
    CHECK(!memcmp(note, WANT0, 16) && !memcmp(role, "1.o.1.o.1.o.1.o.", 16), "bar 0 plays E1 E2 E1 E2 ... (1.o.1.o.)");
    seq_bass_line(&s, 1, note, role);
    for (int i = 0; i < 16; i++) ok1 &= note[i] == (i % 4 == 0 ? 36 : i % 4 == 2 ? 48 : 0);
    CHECK(ok1, "bar 1 plays C2 C3 C2 C3 ...");
    CHECK(seq_bass_bar_score(&s, 0) == 0, "roots and octaves: score 0");
}

/* The core render splits a DRAWN Bass bar into spans of one chord (bar_harm). Rolling (.xxx a beat) with APPROACH Half below over a
 * loop whose neighbouring chords differ (but at 1/2-bar chords the last and the first, both I): at 1/2- and 1/4-bar chords each span
 * plays its own chord's root and its last hit is a half step below the next span's root (none before the same chord); at 2-bar
 * chords a chord's first bar is all its root (its next chord is itself: APPROACH skipped) and its second bar approaches the next */
static void test_19_span_split(void) {
    static const int LEN[3] = {8, 4, 32}, WANT_APP[3] = {7, 16, 2};
    static const char *const NAME[3] = {"1/2-bar", "1/4-bar", "2-bar"};
    static seq_t s;
    for (int t = 0; t < 3; t++) {
        const seq_part_t *p = &s.part[0];
        int ok = 1, apps = 0, first_bars = 1, span = LEN[t] < SEQ_STEPS ? LEN[t] : SEQ_STEPS, loop;
        seq_init(&s, emit, NULL);   /* E minor, 4 bars */
        seq_set_chord_len(&s, LEN[t]);
        for (int k = 0; k < seq_slots(&s); k++) seq_set_slot(&s, k, k * 3 % 7, 0, COL_TRIAD);   /* neighbours 3 degrees apart */
        seq_rhythm_pick(&s, LANE_BASS, 5);   /* Rolling */
        seq_set_bknob(&s, BK_APP, 2);        /* Half below */
        loop = p->bars * SEQ_STEPS;
        for (int a = 0; a < loop; a += span) {
            int root = theory_degree_pc(s.key, s.scale, p->prog[seq_slot_at(&s, a / SEQ_STEPS, a % SEQ_STEPS)]);
            int nx = (a + span) % loop, next = theory_degree_pc(s.key, s.scale, p->prog[seq_slot_at(&s, nx / SEQ_STEPS, nx % SEQ_STEPS)]);
            int last = -1, app;
            for (int i = a; i < a + span; i++)
                if (p->lane[LANE_BASS].steps[i].on) last = i;
            app = last >= 0 && next != root && 16 * (last - a) >= 10 * span;
            for (int i = a; i < a + span; i++) {
                const seq_step_t *x = &p->lane[LANE_BASS].steps[i];
                int want = i == last && app ? (next + 11) % 12 : root;
                if (x->on && (x->abs % 12 != want || p->brole[i] != (i == last && app ? 'c' : '1'))) {
                    printf("     %s: step %d plays %d (%c), want pc %d\n", NAME[t], i, x->abs, p->brole[i], want);
                    ok = 0;
                }
            }
            apps += app;
            if (LEN[t] > SEQ_STEPS && a / SEQ_STEPS % 2 == 0) first_bars &= !app && !memchr(&p->brole[a], 'c', (size_t)span);
        }
        CHECK(ok && apps == WANT_APP[t] && first_bars, "%s chords: each span its own chord's root, its last hit half below the next "
              "chord (%d approaches)%s", NAME[t], apps, LEN[t] > SEQ_STEPS ? "; none in a chord's first bar" : "");
    }
}

/* Final review: the BASS tab's centre is a bar (any bar of the loop, an odd one at 2-bar chords), the CHORDS tab's a chord slot;
 * each clamps to its own count when the loop shrinks */
static void test_19_bass_centre_bars(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);   /* 4 bars */
    seq_set_follow(&s, 0);
    seq_set_chord_len(&s, 32);  /* 2 slots */
    seq_set_centre(&s, TAB_BASS, 3);
    seq_set_centre(&s, TAB_CHORDS, 3);
    CHECK(seq_centre_slot(&s, TAB_BASS) == 3 && seq_centre_slot(&s, TAB_CHORDS) == 0, "2-bar chords: the BASS centre is bar 4, the "
          "CHORDS centre a slot (3 is past its 2: ignored)");
    seq_set_bars(&s, 2);
    CHECK(seq_centre_slot(&s, TAB_BASS) == 1 && seq_centre_slot(&s, TAB_CHORDS) == 0, "2 bars: the BASS centre clamps to bar 2");
}

/* What seq_block plays on the Bass channel is seq_bass_line; the old SHAPE row's REGISTER and OCT do nothing on a drawn lane */
static int bass_plays_line(seq_t *s, int ch) {
    int ok = 1;
    nev = 0;
    play(s, 0.0, 16.0, 0);
    seq_block(s, 16.0, bpm, 0, 128);
    for (int k = 0; k < 64; k++) {
        unsigned char note[16];
        char role[16];
        int x[8], n = notes_at(ch, k, x);
        seq_bass_line(s, k / SEQ_STEPS, note, role);
        ok &= n == (note[k % SEQ_STEPS] > 0) && (!n || x[0] == note[k % SEQ_STEPS]);
    }
    return ok && balanced();
}

static void test_19_play_abs(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    s.part[0].bass.set = bass_style_set(36);   /* Funk Thirds: 3rds, 5ths and octaves */
    seq_rhythm_pick(&s, LANE_BASS, 12);    /* Funk */
    CHECK(bass_plays_line(&s, seq_channel_of(&s, LANE_BASS)), "the Bass plays seq_bass_line's notes, balanced");
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER, 100);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, 1);
    CHECK(bass_plays_line(&s, seq_channel_of(&s, LANE_BASS)), "the old REGISTER and OCT change nothing on a drawn lane");
    seq_set_rknob(&s, LANE_BASS, RK_HUM, 4);
    CHECK(bass_plays_line(&s, seq_channel_of(&s, LANE_BASS)), "HUMANIZE 4 (the Bass lane's own waiting hit): the same notes");
}

/* PLAY's tap on a DRAWN Bass (0.18's rule): a silent step draws a HIT, a playing one clears the drawn hit; one undo step each */
static void test_19_bass_tap(void) {
    static seq_t s;
    const seq_step_t *st = s.part[0].lane[LANE_BASS].steps;
    int n;
    seq_init(&s, emit, NULL);
    n = s.undo_n;
    seq_set_step(&s, LANE_BASS, 0, 8, 1);
    CHECK(st[8].on && st[8].abs == 28 && st[0].len == 8 && s.part[0].rb[DL_BASS][0].c[8] == RC_HIT && s.part[0].rb[DL_BASS][0].ed &&
          s.undo_n == n + 1 && s.undo[(s.undo_top + SEQ_UNDO - 1) % SEQ_UNDO].lane == LANE_BASS,
          "a tap on a silent step draws a HIT, rendered (E1), the held root cut there; one Bass undo step");
    seq_set_step(&s, LANE_BASS, 0, 8, 1);
    CHECK(s.undo_n == n + 1, "a re-send changes nothing");
    seq_set_step(&s, LANE_BASS, 0, 8, 0);
    CHECK(!st[8].on && s.part[0].rb[DL_BASS][0].c[8] == RC_REST && s.undo_n == n + 2, "a tap on it clears the drawn hit");
    seq_undo(&s);
    seq_undo(&s);
    CHECK(st[0].len == 16 && !st[8].on && !s.part[0].rb[DL_BASS][0].ed && s.undo_n == n, "two UNDOs: Root again");
}

static void test_19_fill_notes(void) {
    static seq_t s;
    int x[8], n = 0, ok = 1;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_rknob(&s, LANE_BASS, RK_FILL, 1);
    nev = 0;
    play(&s, 0.0, 20.0, 0);   /* bars 1-5 */
    seq_block(&s, 20.0, bpm, 0, 128);
    for (int k = 48; k < 64; k++)
        if (notes_at(seq_channel_of(&s, LANE_BASS), k, x)) {
            n++;
            ok &= x[0] > 0 && x[0] == s.part[0].rfill_abs[DL_BASS][3][k - 48];
        }
    CHECK(n == 2 && ok && notes_at(seq_channel_of(&s, LANE_BASS), 63, x) && x[0] == 27,
          "FILL 1 (0.24 PICKUP): bar 4 plays its own note and the half step below bar 5's Em on step 16, D#1 (%d hits)", n);
    CHECK(balanced(), "balanced");
}

/* Review Focus 2: a fixed note on a step COMPLEXITY cuts is kept, silent and counted, and sounds again when the hit returns */
static void test_19_fix_survives_cut(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_part_t *p = &s.part[0];
    p->bass.fix[6] = 31;   /* G1 */
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths */
    CHECK(p->lane[LANE_BASS].steps[6].on && p->lane[LANE_BASS].steps[6].abs == 31 && p->brole[6] == 'f' &&
          seq_bass_fixed_count(&s, 0) == 1, "step 7 of bar 1 plays its fixed G1");
    seq_set_complexity(&s, LANE_BASS, 0);
    CHECK(!p->lane[LANE_BASS].steps[6].on && p->bass.fix[6] == 31 && seq_bass_fixed_count(&s, 0) == 1,
          "COMPLEXITY 0: the step is silent, the fix kept and counted");
    seq_set_complexity(&s, LANE_BASS, 50);
    CHECK(p->lane[LANE_BASS].steps[6].on && p->lane[LANE_BASS].steps[6].abs == 31, "COMPLEXITY 50: it sounds at G1 again");
}

static int bass_snaps(const seq_t *s, int newest) {   /* Bass snapshots among the newest undo steps */
    int n = 0;
    for (int i = 1; i <= newest; i++) n += s->undo[(s->undo_top + SEQ_UNDO - i) % SEQ_UNDO].lane == LANE_BASS;
    return n;
}

static void test_19_gen_leaves_bass(void) {
    static seq_t s;
    static rbar_t rb[SEQ_MAX_BARS];
    static seq_step_t st[SEQ_LANE_STEPS];
    int n;
    seq_init(&s, emit, NULL);
    seq_rbar_open(&s, LANE_BASS, 1);
    seq_rbar_paint(&s, LANE_BASS, 4, RC_HIT);
    seq_rbar_done(&s, LANE_BASS);
    memcpy(rb, s.part[0].rb[DL_BASS], sizeof rb);
    memcpy(st, s.part[0].lane[LANE_BASS].steps, sizeof st);
    CHECK(s.part[0].rb[DL_BASS][1].ed && st[SEQ_STEPS + 4].on, "a painted Bass bar plays its hit");
    n = s.undo_n;
    seq_generate_all(&s);
    CHECK(!memcmp(rb, s.part[0].rb[DL_BASS], sizeof rb) && !memcmp(st, s.part[0].lane[LANE_BASS].steps, sizeof st) &&
          bass_snaps(&s, s.undo_n - n) == 0, "NEW IDEA leaves the drawn Bass lane, with no Bass undo");
    n = s.undo_n;
    seq_generate(&s, LANE_BASS);
    CHECK(!memcmp(rb, s.part[0].rb[DL_BASS], sizeof rb) && !memcmp(st, s.part[0].lane[LANE_BASS].steps, sizeof st) && s.undo_n == n,
          "GEN on the Bass leaves it, with no undo");
}

static void test_19_harmony_rerender_basic(void) {
    static seq_t s;
    unsigned char note[16];
    char role[16];
    seq_init(&s, emit, NULL);   /* E minor: Em C G D */
    seq_bass_line(&s, 1, note, role);
    CHECK(note[0] % 12 == 0, "bar 2's root is C");
    seq_set_slot(&s, 1, 3, 0, COL_TRIAD);   /* Am */
    seq_bass_line(&s, 1, note, role);
    CHECK(note[0] % 12 == 9 && role[0] == '1', "slot 2 to Am: bar 2's root is A");
}

/* 0.19 Task 5: the BASS page's note settings, STYLE browser, bar editor and the Bass tab's views */

/* The note model run directly (bassline.h) for bar b of a loop of 1-bar triads in the project scale, as it plays, with
 * settings `set`: the reference seq_bass_line is checked against */
static void ref_bar(const seq_t *s, int b, const bass_set_t *set, unsigned char note[16]) {
    const seq_part_t *p = &s->part[s->cur_part];
    unsigned char c[16];
    char role[16];
    int n[THEORY_CHORD_MAX];
    bass_key_t k;
    bass_chord_t ch[2];
    k.tonic = theory_degree_pc(s->key, s->scale, 0);
    for (int d = 0; d < 7; d++) k.scale[d] = theory_degree_pc(s->key, s->scale, d);
    for (int j = 0; j < 2; j++) {
        theory_chord_notes(s->key, s->scale, p->prog[(b + j) % p->bars], COL_TRIAD, n);
        int r = ((n[0] % 12) + 12) % 12, t = ((n[1] % 12) + 12) % 12;
        ch[j] = (bass_chord_t){r, t, ((t - r) % 12 + 12) % 12 == 3};
    }
    seq_bar_plays(s, LANE_BASS, b, c);
    bass_span_in_t in = {c, 16, &ch[0], &ch[1], &k, *set, p->lane[LANE_BASS].bar_var[b], &p->bass.fix[b * SEQ_STEPS]};
    bass_span(&in, note, role);
}

/* Bar b plays as the reference says, with its own settings (frozen when note-locked) */
static int bass_bar_ref(const seq_t *s, int b) {
    const seq_part_t *p = &s->part[s->cur_part];
    unsigned char got[16], want[16];
    char role[16];
    seq_bass_line(s, b, got, role);
    ref_bar(s, b, p->bass.nlock >> b & 1 ? &p->bass.frozen[b] : &p->bass.set, want);
    return !memcmp(got, want, 16);
}

static int bass_all_ref(const seq_t *s) {
    int ok = 1;
    for (int b = 0; b < s->part[s->cur_part].bars; b++) ok &= bass_bar_ref(s, b);
    return ok;
}

static void bline(const seq_t *s, int bar, unsigned char note[16]) {
    char role[16];
    seq_bass_line(s, bar, note, role);
}

/* Every sounding note of bar b is `want` with role r */
static int bass_bar_all(const seq_t *s, int b, int want, char r) {
    unsigned char note[16];
    char role[16];
    int n = 0, ok = 1;
    seq_bass_line(s, b, note, role);
    for (int i = 0; i < 16; i++)
        if (note[i]) n++, ok &= note[i] == want && role[i] == r;
    return ok && n > 0;
}

static int bset_is(const seq_t *s, bass_set_t want) { return !memcmp(&s->part[s->cur_part].bass.set, &want, sizeof want); }

static int bbar_reads(const seq_t *s, int bar, const char *want) {
    unsigned char c[16];
    char str[17];
    seq_bar_codes(s, LANE_BASS, bar, c);
    rhythm_format(c, str);
    return !strcmp(str, want);
}

static int bbar_is(const seq_t *s, int bar, int ri) {
    unsigned char c[16], want[16];
    seq_bar_codes(s, LANE_BASS, bar, c);
    rhythm_lib_codes(RLIB_BASS, ri, want);
    return !memcmp(c, want, 16);
}

/* Note-locks bar b through the editor: open, LOCK, DONE */
static void bass_nlock(seq_t *s, int b) {
    seq_rbar_open(s, LANE_BASS, b);
    seq_rbar_lock(s, LANE_BASS);
    seq_rbar_done(s, LANE_BASS);
}

static void test_19_bknobs(void) {
    static seq_t s;
    static const int LO[BK_COUNT] = {0, 0, 0, -2}, HI[BK_COUNT] = {4, 4, 4, 2};
    bass_set_t want = bass_style_set(BASS_POP_ROOT);
    unsigned char before[16], after[16];
    int n, ok = 1, changed = 0;
    seq_init(&s, emit, NULL);   /* E minor: Em C G D */
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths */
    n = s.undo_n;
    for (int k = 0; k < BK_COUNT; k++)
        for (int v = LO[k]; v <= HI[k]; v++) {
            bline(&s, 0, before);
            seq_set_bknob(&s, k, v);
            want.k[k] = (signed char)v;
            bline(&s, 0, after);
            changed += memcmp(before, after, 16) != 0;
            ok &= seq_bknob(&s, k) == v && bset_is(&s, want) && bass_all_ref(&s);
        }
    CHECK(ok && changed >= 8, "MOVEMENT, OCTAVES, APPROACH and REGISTER: every value plays as the reference (%d turns changed bar 1)",
          changed);
    CHECK(s.undo_n == n && seq_bass_style(&s) == BASS_POP_ROOT && seq_bass_edited(&s), "no undo step; the style stays, (edited)");
    seq_set_bknob(&s, BK_MOV, 9);
    seq_set_bknob(&s, BK_REG, -5);
    seq_set_bknob(&s, BK_COUNT, 1);
    seq_set_bknob(&s, -1, 1);
    CHECK(seq_bknob(&s, BK_MOV) == 4 && seq_bknob(&s, BK_REG) == -2 && seq_bknob(&s, BK_COUNT) == 0 && s.undo_n == n,
          "values clamp to their ranges, a bad knob is ignored");
    seq_set_bknob(&s, BK_MOV, 0);
    seq_set_bknob(&s, BK_OCTS, 0);
    seq_set_bknob(&s, BK_REG, 2);
    bline(&s, 0, after);
    CHECK(after[0] == 40 && after[2] == 40, "REGISTER Highest: E sounds as E2 (A1-G#2)");
    bass_nlock(&s, 1);
    n = s.undo_n;
    bline(&s, 1, before);
    seq_set_bknob(&s, BK_OCTS, 4);
    seq_set_bknob(&s, BK_REG, -1);
    bline(&s, 1, after);
    CHECK(!memcmp(before, after, 16) && bass_bar_ref(&s, 1) && bass_all_ref(&s) && s.undo_n == n,
          "a note-locked bar ignores knob turns; the others follow");
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_set_bknob(&s, BK_MOV, 1);
    CHECK(seq_rbar_bar(&s, LANE_BASS) == 2 && s.undo_n == n, "a knob turn leaves an open editor open");
    seq_rbar_done(&s, LANE_BASS);
}

static void test_19_style_apply(void) {
    static seq_t s;
    bass_set_t pop = bass_style_set(BASS_POP_ROOT);
    int n, ok = 1;
    seq_init(&s, emit, NULL);   /* E minor */
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths */
    for (int i = 0; i < BASS_STYLE_COUNT; i++) {
        seq_set_bass_style(&s, i);
        ok &= seq_bass_style(&s) == i && !seq_bass_edited(&s) && bset_is(&s, bass_style_set(i)) && bass_all_ref(&s);
    }
    CHECK(ok, "all 48 styles apply: the style, not edited, every bar as the reference");
    seq_init(&s, emit, NULL);   /* an undo ring with room */
    seq_rhythm_pick(&s, LANE_BASS, 3);
    bass_nlock(&s, 2);
    n = s.undo_n;
    seq_set_bass_style(&s, 9);   /* Pedal Drone: TONIC, REGISTER Low */
    CHECK(bass_bar_all(&s, 0, 28, 'P') && bass_bar_all(&s, 1, 28, 'P') && bass_bar_all(&s, 3, 28, 'P') && bass_bar_all(&s, 2, 31, '1') &&
          s.undo_n == n + 1, "Pedal Drone: every unlocked bar plays the E pedal (E1); locked bar 3 keeps G1; one undo step");
    CHECK(seq_bass_style(&s) == 9 && !seq_bass_edited(&s), "the style is Pedal Drone, not edited");
    seq_set_bass_style(&s, 9);
    CHECK(s.undo_n == n + 1, "the same style again: nothing");
    seq_set_bknob(&s, BK_OCTS, 1);
    CHECK(seq_bass_edited(&s) && s.undo_n == n + 1, "a knob turn: (edited), no undo step");
    seq_undo(&s);
    CHECK(bset_is(&s, pop) && bass_bar_all(&s, 0, 28, '1') && bass_bar_all(&s, 1, 36, '1') && s.undo_n == n,
          "one UNDO restores the previous settings (Pop Root)");
    seq_set_bass_pedal(&s, 2);
    CHECK(bass_bar_all(&s, 0, 35, 'P') && bass_bar_all(&s, 1, 35, 'P') && bass_bar_all(&s, 2, 31, '1') && s.undo_n == n + 1 &&
          seq_bass_edited(&s), "PEDAL DOMINANT: B1 under every unlocked bar, one undo step, (edited)");
    seq_rbar_open(&s, LANE_BASS, 0);
    seq_set_bass_pedal(&s, 2);
    seq_set_bass_pedal(&s, 3);
    seq_set_bass_octave(&s, 2);
    CHECK(s.undo_n == n + 1 && s.part[0].bass.set.pedal == 2 && !s.part[0].bass.set.octave && seq_rbar_bar(&s, LANE_BASS) == 0,
          "a re-send or a bad value: nothing (an open editor stays open)");
    seq_rbar_done(&s, LANE_BASS);
    seq_set_bass_octave(&s, 1);
    CHECK(bass_bar_all(&s, 0, 47, 'P') && s.undo_n == n + 2, "OCTAVE +1: B2, one undo step");
    seq_undo(&s);
    seq_undo(&s);
    CHECK(bset_is(&s, pop) && bass_bar_all(&s, 0, 28, '1') && s.undo_n == n, "two UNDOs: back");
}

static void test_19_style_browser(void) {
    static seq_t s;
    bass_set_t b0;
    unsigned char line0[16], line[16];
    int n;
    seq_init(&s, emit, NULL);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    b0 = s.part[0].bass.set;
    bline(&s, 0, line0);
    n = s.undo_n;
    seq_bstyle_open(&s);
    CHECK(seq_bview(&s) == BVIEW_STYLE && seq_bmode(&s) == 0 && seq_follow_paused(&s, TAB_BASS) == 1 &&
          seq_follow_paused(&s, TAB_CHORDS) == 0 && s.undo_n == n + 1 && seq_bstyle_genre(&s) == 10,
          "STYLE opens: BVIEW_STYLE, FOLLOW paused on BASS, one undo step, the chip Pop / Rock");
    seq_bstyle_try(&s, 3);
    seq_bstyle_try(&s, 9);
    seq_bstyle_try(&s, 3);
    CHECK(bset_is(&s, bass_style_set(3)) && bass_all_ref(&s) && s.undo_n == n + 1, "a try applies at once, with no undo step");
    seq_bstyle_cancel(&s);
    bline(&s, 0, line);
    CHECK(bset_is(&s, b0) && !memcmp(line, line0, 16) && s.undo_n == n && seq_bview(&s) == BVIEW_BASS &&
          seq_follow_paused(&s, TAB_BASS) == 0, "CANCEL after 3 tries: as before, no undo step, back on BASS");
    seq_bstyle_open(&s);
    seq_bstyle_set_genre(&s, 2);
    CHECK(seq_bstyle_genre(&s) == 2 && bset_is(&s, bass_style_set(8)), "a genre chip applies its first style (Techno: Root Drone)");
    seq_bstyle_set_genre(&s, BASS_GENRES);
    seq_set_bass_style(&s, 13);   /* the field's arrows while it is open: a try */
    CHECK(seq_bstyle_genre(&s) == 2 && bset_is(&s, bass_style_set(13)) && s.undo_n == n + 1, "a bad chip is ignored; a step while open is a try");
    seq_bstyle_done(&s);
    CHECK(bset_is(&s, bass_style_set(13)) && seq_bview(&s) == BVIEW_BASS && s.undo_n == n + 1, "DONE keeps it: one undo step");
    seq_undo(&s);
    CHECK(bset_is(&s, b0), "which UNDO takes back");
    seq_bstyle_open(&s);
    seq_bstyle_try(&s, 5);
    seq_bstyle_try(&s, BASS_POP_ROOT);
    seq_bstyle_done(&s);
    CHECK(s.undo_n == n && bset_is(&s, b0), "DONE with nothing changed leaves no undo step");
    seq_bstyle_open(&s);
    seq_bstyle_try(&s, 5);
    seq_undo(&s);
    CHECK(seq_bview(&s) == BVIEW_BASS && bset_is(&s, b0) && s.undo_n == n, "UNDO with it open is its CANCEL");
    seq_bstyle_try(&s, 5);
    seq_bstyle_done(&s);
    seq_bstyle_cancel(&s);
    CHECK(bset_is(&s, b0) && s.undo_n == n, "closed: try, DONE and CANCEL do nothing");
}

static void test_19_bedit(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    unsigned char line0[16], line[16];
    char role[16];
    int n0, n, r;
    seq_init(&s, emit, NULL);   /* E minor: Em C G D */
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths: A.x.A.x.A.x.A.x. */
    bline(&s, 2, line0);
    n0 = s.undo_n;
    CHECK(seq_rbar_bar(&s, LANE_BASS) == -1 && seq_rbar_sel(&s, LANE_BASS) == -1, "closed: no bar, no step");
    seq_rbar_open(&s, LANE_BASS, 2);
    CHECK(seq_rbar_bar(&s, LANE_BASS) == 2 && seq_bview(&s) == BVIEW_BAR && seq_bmode(&s) == 1 && seq_follow_paused(&s, TAB_BASS) == 1 &&
          seq_rbar_sel(&s, LANE_BASS) == 0 && seq_rbar_oct(&s) == 1 && s.undo_n == n0,
          "bar 3 opens (0.24: PHRASE's bar, BVIEW_BAR), FOLLOW paused, its first note selected (G1: octave 1), no undo step");
    seq_rbar_select(&s, LANE_BASS, 4);
    CHECK(seq_rbar_sel(&s, LANE_BASS) == 4 && seq_rbar_oct(&s) == 1, "step 5 selected (G1)");
    s.naud = 0;
    seq_rbar_note_oct(&s, 2);
    CHECK(p->bass.fix[2 * 16 + 4] == 43 && seq_rbar_oct(&s) == 2 && s.undo_n == n0 + 1 && s.naud == 1 && s.aud[0].note == 43 &&
          s.aud[0].lane == LANE_BASS, "OCTAVE 2 re-fixes it at G2, one undo step, heard");
    seq_rbar_note(&s, 0);
    seq_bass_line(&s, 2, line, role);
    CHECK(p->bass.fix[2 * 16 + 4] == 36 && line[4] == 36 && role[4] == 'f' && s.undo_n == n0 + 2 && s.naud == 2 && s.aud[1].note == 36 &&
          seq_bass_fixed_count(&s, 2) == 1, "C on the piano: step 5 fixed at C2 (role f), one undo step, heard");
    seq_rbar_note(&s, 0);
    seq_rbar_note(&s, 12);
    seq_rbar_note_oct(&s, 5);
    CHECK(s.undo_n == n0 + 2 && seq_rbar_oct(&s) == 2, "the same note again, a bad note or octave: no undo step");
    seq_undo(&s);
    CHECK(p->bass.fix[2 * 16 + 4] == 43 && seq_rbar_bar(&s, LANE_BASS) == 2 && s.undo_n == n0 + 1, "UNDO steps back the fix (G2), the editor open");
    seq_undo(&s);
    bline(&s, 2, line);
    CHECK(!p->bass.fix[2 * 16 + 4] && !memcmp(line, line0, 16) && seq_rbar_bar(&s, LANE_BASS) == 2 && s.undo_n == n0, "and the octave's: G1 again");
    seq_rbar_note(&s, 0);
    n = s.undo_n;
    r = seq_rbar_new_bar(&s, LANE_BASS);
    bline(&s, 2, line);
    CHECK(r == 1 && p->lane[LANE_BASS].bar_var[2] == 1 && memcmp(line, line0, 16) && line[4] == 36 && s.undo_n == n + 1,
          "NEW BAR: its own line (v 1: hit 2's octave flips), the fix kept, one undo step");
    seq_rbar_lock(&s, LANE_BASS);
    CHECK((p->bass.nlock >> 2 & 1) && !memcmp(&p->bass.frozen[2], &p->bass.set, sizeof p->bass.set) && s.undo_n == n + 2, "LOCK freezes the settings: one undo step");
    bline(&s, 2, line0);
    seq_set_bknob(&s, BK_OCTS, 2);
    seq_set_bass_style(&s, 9);   /* closes the editor as DONE first (an edit elsewhere) */
    bline(&s, 2, line);
    CHECK(!memcmp(line, line0, 16) && seq_rbar_bar(&s, LANE_BASS) == -1 && s.undo_n == n + 3, "LOCK then a knob turn and a style: the bar keeps its notes");
    seq_undo(&s);   /* the style */
    seq_rbar_open(&s, LANE_BASS, 2);
    n = s.undo_n;
    seq_rbar_reset_bar(&s, LANE_BASS);
    CHECK(!p->lane[LANE_BASS].bar_var[2] && !seq_bass_fixed_count(&s, 2) && !(p->bass.nlock >> 2 & 1) && s.undo_n == n + 1 &&
          bass_bar_ref(&s, 2), "RESET BAR: v 0, no fixed note, unlocked; one undo step");
    seq_rbar_reset_bar(&s, LANE_BASS);
    CHECK(s.undo_n == n + 1, "RESET BAR on a fresh bar: nothing");
    seq_rbar_done(&s, LANE_BASS);
    /* CANCEL after all of these puts the bar back */
    seq_rbar_open(&s, LANE_BASS, 1);
    seq_rbar_select(&s, LANE_BASS, 2);
    seq_rbar_note(&s, 2);
    seq_rbar_lock(&s, LANE_BASS);
    seq_rbar_open(&s, LANE_BASS, 1);   /* reopened: the edits are kept by its DONE */
    n = s.undo_n;
    int pushes = s.undo_pushes;
    static seq_snap_t ring[SEQ_UNDO];
    memcpy(ring, s.undo, sizeof ring);
    unsigned char fix0[16], var0 = p->lane[LANE_BASS].bar_var[1];
    unsigned short lock0 = p->bass.nlock;
    bass_set_t fro0 = p->bass.frozen[1];
    memcpy(fix0, &p->bass.fix[16], 16);
    bline(&s, 1, line0);
    seq_rbar_select(&s, LANE_BASS, 6);    /* C3 */
    seq_rbar_note(&s, 4);      /* E3 */
    seq_rbar_note_oct(&s, 3);   /* E3 already: no step */
    r = seq_rbar_new_bar(&s, LANE_BASS);  /* only hit 2's octave varies (MOVEMENT None), and it is fixed: nothing */
    seq_rbar_reset_bar(&s, LANE_BASS);
    r = r * 2 + seq_rbar_new_bar(&s, LANE_BASS);
    seq_rbar_select(&s, LANE_BASS, 8);
    seq_rbar_note(&s, 11);
    seq_rbar_lock(&s, LANE_BASS);
    seq_set_bknob(&s, BK_APP, 3);   /* a knob turn meanwhile: kept */
    CHECK(r == 1 && s.undo_pushes - pushes == 5, "edited: NEW BAR over a fixed hit 2 refused, then after RESET BAR taken; %d undo steps",
          s.undo_pushes - pushes);
    seq_rbar_cancel(&s, LANE_BASS);
    bline(&s, 1, line);
    CHECK(seq_rbar_bar(&s, LANE_BASS) == -1 && !memcmp(fix0, &p->bass.fix[16], 16) && p->lane[LANE_BASS].bar_var[1] == var0 && p->bass.nlock == lock0 &&
          !memcmp(&fro0, &p->bass.frozen[1], sizeof fro0) && !memcmp(line, line0, 16) && s.undo_n == n && !memcmp(ring, s.undo, sizeof ring) &&
          seq_bknob(&s, BK_APP) == 3 &&
          seq_bview(&s) == BVIEW_BASS, "CANCEL: v, fixes, lock and frozen settings as at open, the undo depth too; the knob kept");
    seq_rbar_cancel(&s, LANE_BASS);
    CHECK(s.undo_n == n && seq_rbar_bar(&s, LANE_BASS) == -1, "a second CANCEL is a no-op");
    seq_rbar_open(&s, LANE_BASS, 1);
    seq_undo(&s);
    CHECK(seq_rbar_bar(&s, LANE_BASS) == -1 && s.undo_n == n, "UNDO with none of its own steps left is the editor's CANCEL");
    /* NEW BAR on one hit under a pedal: no variation changes it */
    seq_rbar_open(&s, LANE_BASS, 3);
    seq_rhythm_pick(&s, LANE_BASS, 0);   /* Root: one note */
    seq_rbar_done(&s, LANE_BASS);
    seq_set_bass_pedal(&s, 1);
    seq_rbar_open(&s, LANE_BASS, 3);
    n = s.undo_n;
    bline(&s, 3, line0);
    r = seq_rbar_new_bar(&s, LANE_BASS);
    bline(&s, 3, line);
    CHECK(!r && !p->lane[LANE_BASS].bar_var[3] && !memcmp(line, line0, 16) && s.undo_n == n, "NEW BAR on one hit under a pedal: returns 0, nothing changes");
    seq_rbar_done(&s, LANE_BASS);
    seq_rbar_open(&s, LANE_BASS, 4);
    CHECK(seq_rbar_bar(&s, LANE_BASS) == -1, "bar 5 of a 4-bar loop does not open");
}

/* 0.20 the MELODY tab (TAB_MEL): its strip pages by bars and follows playback like TAB_BASS's; its pause is its own */
static void test_20_tab_mel_strip(void) {
    static seq_t s;
    double ppq;
    strip_16(&s);
    CHECK(seq_strip_by_bars(&s, TAB_MEL) && seq_strip_pages(&s, TAB_MEL) == 4 && seq_strip_page(&s, TAB_MEL) == 0 &&
          seq_centre_slot(&s, TAB_MEL) == 0 && seq_follow_paused(&s, TAB_MEL) == 0, "stopped: the MELODY strip is bars, 4 pages, bar 1 running");
    ppq = play(&s, 0.0, 20.5, 0);   /* into bar 6 */
    CHECK(seq_strip_page(&s, TAB_MEL) == seq_strip_page(&s, TAB_BASS) && seq_strip_page(&s, TAB_MEL) == 1 &&
          seq_centre_slot(&s, TAB_MEL) == seq_centre_slot(&s, TAB_BASS) && seq_centre_slot(&s, TAB_MEL) == 5,
          "playing: page 1, centre bar 6, as the BASS tab");
    seq_strip_turn(&s, TAB_MEL, 1);
    CHECK(seq_follow_paused(&s, TAB_MEL) == 2 && seq_strip_page(&s, TAB_MEL) == 2 && seq_centre_slot(&s, TAB_MEL) == 5, "turn +1: MELODY paused on page 2");
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_follow_paused(&s, TAB_BASS) == 0 && seq_strip_page(&s, TAB_CHORDS) == 1 &&
          seq_strip_page(&s, TAB_BASS) == 1, "the CHORDS and BASS tabs still follow");
    ppq = play(&s, ppq, 12.0, 0);
    CHECK(seq_strip_page(&s, TAB_MEL) == 2 && seq_strip_page(&s, TAB_CHORDS) == 2 && seq_strip_page(&s, TAB_BASS) == 2, "held while the others move on");
    seq_back_to_playing(&s, TAB_MEL);
    CHECK(seq_follow_paused(&s, TAB_MEL) == 0 && seq_strip_page(&s, TAB_MEL) == seq_strip_page(&s, TAB_BASS), "back to playing");
    seq_set_centre(&s, TAB_MEL, 3);
    CHECK(s.tv[TAB_MEL].csel == 3 && s.tv[TAB_BASS].csel == 0 && s.tv[TAB_CHORDS].csel == 0, "the centre is MELODY's own");
    (void)ppq;
}

/* 0.20 a transaction on the Melody lane has no drawn-lane row (dl_of -1): the strip getters read it as MELODY's, no other tab's */
static void test_20_txn_mel_safe(void) {
    static seq_t s;
    strip_16(&s);
    s.txn.kind = CVIEW_BAR, s.txn.lane = LANE_MEL_UP, s.txn.stale = 0, s.txn.slot = 5;
    CHECK(seq_follow_paused(&s, TAB_MEL) == 1 && seq_follow_paused(&s, TAB_CHORDS) == 0 && seq_follow_paused(&s, TAB_BASS) == 0,
          "an open Melody transaction pauses MELODY's FOLLOW only");
    CHECK(seq_strip_by_bars(&s, TAB_MEL) && !seq_strip_by_bars(&s, TAB_CHORDS) && seq_strip_by_bars(&s, TAB_BASS), "no tab's paging by bars reads the wrong row");
    CHECK(seq_strip_page(&s, TAB_MEL) == 0 && seq_centre_slot(&s, TAB_MEL) >= 0, "MELODY's strip reads");
    s.txn.stale = 1;
    CHECK(seq_follow_paused(&s, TAB_MEL) == 0, "stale: closed");
    s.txn.kind = 0;
}

/* 0.20 Task 3: the JAM Melody lane (GEN_MEL_JAM), checked against the melody model (melody2.h) called directly */

/* every slot's chord as the model reads it (its triad in its own scale), and per bar the chord under each step */
typedef struct { mjam_chord_t ch[SEQ_MAX_PROG]; mjam_bar_in_t bar[SEQ_MAX_BARS]; } mharm_t;

static void mharm(const seq_t *s, mharm_t *h) {
    const seq_part_t *p = &s->part[s->cur_part];
    for (int k = 0; k < p->prog_len; k++) mjam_chord_of(s->key, s->scale, theory_slot_scale(s->scale, p->src[k]), p->prog[k], &h->ch[k]);
    for (int b = 0; b < p->bars; b++) {
        h->bar[b].key = s->key, h->bar[b].scale = s->scale;
        for (int i = 0; i < SEQ_STEPS; i++) h->bar[b].chord[i] = &h->ch[seq_slot_at(s, b, i)];
    }
}

/* the model's bar b of the playing part (its settings, a locked bar's frozen ones, its NEW BAR variation), before any fix */
static void model_bar(const seq_t *s, int b, mjam_note_t out[SEQ_STEPS]) {
    static mharm_t h;
    const seq_part_t *p = &s->part[s->cur_part];
    mharm(s, &h);
    mjam_loop_in_t in = {s->key, s->scale, p->bars, p->mjam.set, h.bar, p->lane[LANE_MEL_UP].bar_var};
    mjam_bar(&in, b, p->mjam.nlock >> b & 1 ? &p->mjam.frozen[b] : &p->mjam.set, out);
}

/* 0.21 ACCENT: a JAM Melody note's velocity at step i of a bar, 96 + (strength - 2) x a (a 0, 4, 8, 12 for EVEN..HARD); strength 4
 * the downbeat, 3 beat 3, 2 beats 2 and 4, 1 the other 8ths, 0 the 16ths */
static int acc_vel(int i, int acc) {
    static const int A[4] = {0, 4, 8, 12};
    int st = i == 0 ? 4 : i % 8 == 0 ? 3 : i % 4 == 0 ? 2 : i % 2 == 0 ? 1 : 0;
    return 96 + (st - 2) * A[acc & 3];
}

/* 1 when every bar plays the model's line (no fixes): seq_mel_line is it, the lane's steps start its notes (abs, the length to the
 * end of its holds, 0.21 the part's ACCENT velocity, acc_vel) and every note is in 36..100 */
static int mel_is_model(const seq_t *s) {
    const seq_part_t *p = &s->part[s->cur_part];
    int ok = p->lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM;
    for (int b = 0; b < p->bars; b++) {
        mjam_note_t want[SEQ_STEPS], got[SEQ_STEPS];
        model_bar(s, b, want);
        seq_mel_line(s, b, got);
        ok &= !memcmp(want, got, sizeof want);
        for (int i = 0; i < SEQ_STEPS; i++) {
            const seq_step_t *x = &p->lane[LANE_MEL_UP].steps[b * SEQ_STEPS + i];
            int len = 1;
            while (i + len < SEQ_STEPS && want[i + len].hold) len++;
            ok &= x->on == (want[i].note != 0);
            if (x->on) ok &= x->abs == want[i].note && x->len == len && x->vel == acc_vel(i, p->mjam.acc) && x->abs >= 36 && x->abs <= 100;
        }
    }
    return ok;
}

/* a key change and back: every setter path that renders the lane, for a test that wrote the part's melody settings itself */
static void mel_rerender(seq_t *s) {
    int k = seq_key(s);
    seq_set_key(s, (k + 1) % 12);
    seq_set_key(s, k);
}

/* the Melody channel's note-ons of one pass over the loop from 0 (stopped after), by step: got[k] the note (0 none), 1 if no
 * step had two and each velocity is 0.21 the part's ACCENT velocity (acc_vel) */
static int mel_played(seq_t *s, int got[SEQ_LANE_STEPS]) {
    int ch = seq_channel_of(s, LANE_MEL_UP), ok = 1, n = seq_bars(s) * SEQ_STEPS;
    memset(got, 0, sizeof(int) * SEQ_LANE_STEPS);
    nev = 0;
    play_stop(s, n / 4.0 - 0.05);
    for (int i = 0; i < nev; i++) {
        if (!is_on(&evs[i]) || (evs[i].status & 15) != ch - 1 || evs[i].k < 0 || evs[i].k >= n) continue;
        ok &= !got[evs[i].k] && evs[i].vel == acc_vel(evs[i].k % SEQ_STEPS, seq_mel_acc(s));
        got[evs[i].k] = evs[i].note;
    }
    return ok && balanced();
}

/* 1 when one pass of playback plays seq_mel_line exactly */
static int mel_plays_line(seq_t *s) {
    int got[SEQ_LANE_STEPS], ok = mel_played(s, got);
    for (int b = 0; b < seq_bars(s); b++) {
        mjam_note_t line[SEQ_STEPS];
        seq_mel_line(s, b, line);
        for (int i = 0; i < SEQ_STEPS; i++) ok &= got[b * SEQ_STEPS + i] == line[i].note;
    }
    return ok;
}

static int mel_default_lane(const seq_t *s) {
    const seq_part_t *p = &s->part[s->cur_part];
    mjam_set_t d = mjam_default();
    int fixes = 0;
    for (int i = 0; i < SEQ_LANE_STEPS; i++) fixes += p->mjam.fix[i] != 0;
    return p->lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && p->lane[LANE_MEL_UP].gen.complexity == 10 && seq_mel_level(s) == 0 &&
           !memcmp(&p->mjam.set, &d, sizeof d) && p->mjam.sw == 1 && !p->mjam.nlock && !fixes && seq_mel_push(s) == 0 &&
           seq_mel_dyn(s) == 50 && p->mel.kn[MK_GATE] == 50;
}

static void test_20_init_mel(void) {
    static seq_t s;
    mjam_note_t line[SEQ_STEPS];
    seq_init(&s, emit, NULL);
    CHECK(mel_default_lane(&s) && mel_is_model(&s), "a new instance: Melody JAM, ARP UP, LOOP, HOME, COMPLEXITY 10 (level 1), "
          "SWITCH AT BAR, PUSH 0, GATE 50, DYNAMICS 50; it plays the model's lines");
    seq_init_preset(&s);
    CHECK(mel_default_lane(&s) && mel_is_model(&s), "INIT: the same, over INIT's C major 1-5-6-4");
    seq_mel_line(&s, 3, line);
    CHECK(line[14].note && line[14].note % 12 == 0, "INIT's loop ends home: bar 4's last note is a C (CADENCE HOME)");
    CHECK(mel_plays_line(&s), "through seq_block the Melody channel plays seq_mel_line, once a step, 0.21 at ACCENT LIGHT's velocities");
    seq_set_key(&s, 4);
    seq_set_scale(&s, SCALE_MINOR);
    CHECK(mel_is_model(&s) && mel_plays_line(&s), "in E minor: the model's lines (mjam_bar directly), played");
}

/* The reference's show() (tests/test_melody2.c): note names, - held, . rest, then the dots and the score */
static void mel_show(const seq_t *s, int bar, char *buf, int len) {
    mjam_note_t line[SEQ_STEPS];
    int n = 0;
    seq_mel_line(s, bar, line);
    for (int i = 0; i < SEQ_STEPS; i++) {
        char nm[8];
        if (line[i].note) theory_note_name(line[i].note, nm, sizeof nm);
        else strcpy(nm, line[i].hold ? "-" : ".");
        n += snprintf(buf + n, (size_t)(len - n), "%s%s", i ? " " : "", nm);
    }
    n += snprintf(buf + n, (size_t)(len - n), " | dots ");
    for (int i = 0; i < SEQ_STEPS; i++) n += snprintf(buf + n, (size_t)(len - n), "%s%d", i ? "," : "", seq_mel_frame(s, bar, i));
    snprintf(buf + n, (size_t)(len - n), " | score %d", seq_mel_score(s, bar));
}

/* HOOK MOTIF 1+1, BREATH Beat, CADENCE HOME, FILL PICKUP, ECHO, LAST BEAT (the reference's spot settings) */
static mjam_set_t mel_spot_set(void) {
    mjam_set_t m = mjam_default();
    m.c = MJ_HOOK, m.l = 1, m.form = MF_1_1, m.breath = 2, m.cad = 3, m.fill = 1, m.ans = MA_ECHO, m.stray = 1;
    return m;
}

static void test_20_render_matches_model(void) {
    static seq_t s;
    static const char *const SPOT[4] = {
        "B4 - A4 - B4 - - - . . . . . . . F#4 | dots 26,66,27,66,26,66,66,66,0,0,0,0,0,0,0,22 | score 21",
        "G4 - F#4 - E4 - - - . . . . G4 - E4 - | dots 21,65,23,65,16,64,64,64,0,0,0,0,21,65,16,64 | score 28",
        "D5 - C5 - B4 - - - . . . . . . . G4 | dots 31,67,33,67,26,66,66,66,0,0,0,0,0,0,0,21 | score 28",
        "A4 - G4 - F#4 - - - . . . . A4 - E4 - | dots 26,66,23,65,21,65,65,65,0,0,0,0,26,66,17,64 | score 30"};
    seq_init(&s, emit, NULL);   /* E minor, Em C G D, 1-bar chords */
    s.part[0].mjam.set = mel_spot_set();
    mel_rerender(&s);
    for (int b = 0; b < 4; b++) {
        char got[256];
        mel_show(&s, b, got, sizeof got);
        CHECK(!strcmp(got, SPOT[b]), "HOOK MOTIF 1+1 bar %d is the reference's spot line: %s", b + 1, got);
    }
    CHECK(mel_is_model(&s) && mel_plays_line(&s), "and the lane plays it");
    CHECK(seq_mel_frame(&s, 4, 0) == 0 && seq_mel_score(&s, -1) == 0 && seq_mel_frame(&s, 0, 16) == 0, "bars and steps out of range read 0");
}

/* Review Focus 3: 1- and 2-bar loops with FORM 3+1 and SENT play the first roles: each bar as bar b of a 4-bar loop of the same
 * chords repeated; every block renders and plays (ASan) */
static void test_20_short_loops(void) {
    static seq_t s;
    static mharm_t h;
    static const int FORMS[2] = {MF_3_1, MF_SENT};
    int same = 1, plays = 1;
    for (int n = 1; n <= 2; n++)
        for (int f = 0; f < 2; f++) {
            seq_init(&s, emit, NULL);
            seq_set_bars(&s, n);
            for (int c = 0; c < MJ_COLS; c++)
                for (int l = 0; l < MJ_LEVELS; l++) {
                    mjam_bar_in_t four[4];
                    mjam_set_t m = mjam_default();
                    m.c = (signed char)c, m.l = (signed char)l, m.form = (signed char)FORMS[f], m.breath = 1, m.fill = 1;
                    s.part[0].mjam.set = m;
                    mel_rerender(&s);
                    mharm(&s, &h);
                    for (int b = 0; b < 4; b++) four[b] = h.bar[b % n];
                    mjam_loop_in_t in = {s.key, s.scale, 4, m, four, NULL};
                    for (int b = 0; b < n; b++) {
                        mjam_note_t want[SEQ_STEPS], got[SEQ_STEPS];
                        mjam_bar(&in, b, &m, want);
                        seq_mel_line(&s, b, got);
                        same &= !memcmp(want, got, sizeof want);
                    }
                    if (l == 4) plays &= mel_plays_line(&s);
                }
        }
    CHECK(same, "1- and 2-bar loops, FORM 3+1 and SENT, every block: bar b plays bar b of a 4-bar loop of its chords (the first roles)");
    CHECK(plays, "and plays it, no note twice, no hung note");
}

/* Review Focus 5: key, scale, a borrowed chord, COLOUR 13th and 1/2-bar chords each render every bar again; COLOUR moves nothing */
static void test_20_harmony_rerender(void) {
    static seq_t s;
    mjam_note_t triad[SEQ_LANE_STEPS];
    seq_init(&s, emit, NULL);
    s.part[0].mjam.set = mel_spot_set();
    s.part[0].mjam.set.c = MJ_LEAD, s.part[0].mjam.set.l = 3;   /* LEAD PASSING: chord tones and passing notes */
    mel_rerender(&s);
    CHECK(mel_is_model(&s), "LEAD PASSING 1+1 in E minor plays the model");
    seq_set_key(&s, 7);
    CHECK(mel_is_model(&s), "a key change (G minor): every bar again");
    seq_set_scale(&s, SCALE_DORIAN);
    CHECK(mel_is_model(&s), "a scale change (Dorian): every bar again");
    seq_set_slot(&s, 2, 0, 1, COL_TRIAD);
    CHECK(s.part[0].src[2] == 1 && mel_is_model(&s), "slot 3 borrowed: every bar again");
    memcpy(triad, s.part[0].mline, sizeof triad);
    seq_set_vknob(&s, VK_COL, 4);
    CHECK(mel_is_model(&s) && !memcmp(triad, s.part[0].mline, sizeof triad), "the COLOUR knob at 13th: the same notes");
    for (int k = 0; k < seq_slots(&s); k++) seq_set_slot(&s, k, s.part[0].prog[k], s.part[0].src[k], COL_13TH);
    CHECK(s.part[0].colour[0] == COL_13TH && mel_is_model(&s) && !memcmp(triad, s.part[0].mline, sizeof triad),
          "every slot COLOUR 13th: rendered again, the same notes as Triad");
    seq_set_chord_len(&s, 8);
    CHECK(seq_slots(&s) == 8 && mel_is_model(&s), "1/2-bar chords: every bar again, its 2nd half's chord tones from the chord under them");
    CHECK(mel_plays_line(&s), "and it plays");
}

/* GEN, NEW IDEA and seq_generate(LANE_MEL_UP) leave a JAM lane, with no undo step */
static void test_20_gen_leaves_mel(void) {
    static seq_t s;
    static seq_lane_t was;
    static mjam_note_t line[SEQ_LANE_STEPS];
    seq_init(&s, emit, NULL);
    seq_init_preset(&s);
    was = s.part[0].lane[LANE_MEL_UP];
    memcpy(line, s.part[0].mline, sizeof line);
    seq_generate(&s, LANE_MEL_UP);
    seq_generate_all(&s);
    CHECK(!memcmp(&was, &s.part[0].lane[LANE_MEL_UP], sizeof was) && !memcmp(line, s.part[0].mline, sizeof line) && !seq_can_undo(&s),
          "GEN and NEW IDEA: the JAM lane as it was (seed too), no undo step");
}

/* PUSH moves a JAM lane's chord-change notes early as the Bass's PUSH does; DYNAMICS scales its velocities; both are the part's */
static void test_20_push_dyn(void) {
    static seq_t s;
    int got[SEQ_LANE_STEPS];
    seq_init(&s, emit, NULL);
    seq_init_preset(&s);   /* Melody on channel 3, Bass on 2: each has a note on every bar's step 1 */
    seq_set_mel_push(&s, 100);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_PUSH, 100);
    nev = 0;
    play_stop(&s, 15.95);
    CHECK(seq_mel_push(&s) == 100 && ons_near(0, 3, 3.875) == 1 && ons_between(0, 3, 3.9, 4.1) == 0 && ons_near(0, 2, 3.875) == 1,
          "PUSH 100: bar 2's first Melody note 1/8 beat early, as the Bass's");
    CHECK(ons_near(0, 3, 4.5) == 1 && ons_between(0, 3, 4.3, 4.49) == 0 && balanced(), "a note inside a chord stays on the beat");
    seq_set_mel_push(&s, 0);
    seq_set_mel_push(&s, 101);
    nev = 0;
    play_stop(&s, 4.2);
    CHECK(seq_mel_push(&s) == 0 && ons_near(0, 3, 4.0) == 1 && ons_between(0, 3, 3.8, 3.99) == 0, "PUSH 0 (101 ignored): on the beat");
    seq_set_mel_dyn(&s, 0);
    CHECK(seq_mel_dyn(&s) == 0 && mel_played(&s, got), "0.21 DYNAMICS 0 (jdyn) is a no-op: ACCENT LIGHT's velocities");
    seq_set_mel_dyn(&s, 50);
    CHECK(mel_played(&s, got), "DYNAMICS 50: ACCENT LIGHT's velocities");
    seq_set_mel_push(&s, 30);
    seq_select_part(&s, 1);   /* stopped: at once; its first use copies the Verse's */
    CHECK(seq_part(&s) == 1 && seq_mel_push(&s) == 30, "the Intro starts with the Verse's PUSH 30");
    seq_set_mel_push(&s, 70);
    seq_set_mel_dyn(&s, 20);
    seq_select_part(&s, 0);
    CHECK(seq_mel_push(&s) == 30 && seq_mel_dyn(&s) == 50, "PUSH and DYNAMICS are the part's: the Verse keeps 30 and 50");
    seq_select_part(&s, 1);
    CHECK(seq_mel_push(&s) == 70 && seq_mel_dyn(&s) == 20, "and the Intro its own 70 and 20");
    seq_set_step(&s, LANE_MEL_UP, 0, 1, 1);   /* an undo step, then a knob turn */
    seq_set_mel_push(&s, 90);
    seq_undo(&s);
    CHECK(seq_mel_push(&s) == 90 && seq_mel_dyn(&s) == 20 && !s.part[1].mjam.fix[1], "UNDO takes the tap back, not the knobs");
}

/* PLAY's tap on a JAM lane fixes steps: a silent step a note (the chord root nearest the bar's notes), a sounding one a rest; one
 * undo step each. A note fix keeps the holds after it, a rest silences them, HOLD continues the note before (silent with none) */
static void test_20_tap_fix(void) {
    static seq_t s;
    mjam_note_t line[SEQ_STEPS], was[SEQ_STEPS];
    seq_init(&s, emit, NULL);   /* E minor; ARP UP: notes on even steps, each held one step */
    seq_mel_line(&s, 1, was);   /* bar 2, C: C4 E4 G4 C5 ... */
    seq_set_step(&s, LANE_MEL_UP, 1, 3, 1);
    seq_mel_line(&s, 1, line);
    CHECK(s.part[0].mjam.fix[16 + 3] == 60 && line[3].note == 60 && !line[3].hold && line[2].note == was[2].note && seq_mel_fixed_count(&s, 1) == 1 &&
          s.undo_n == 1 && s.part[0].lane[LANE_MEL_UP].steps[16 + 2].len == 1, "a tap on silent step 4 of bar 2 fixes C4, the root nearest the bar's notes; one undo step");
    seq_set_step(&s, LANE_MEL_UP, 1, 4, 0);
    seq_mel_line(&s, 1, line);
    CHECK(s.part[0].mjam.fix[16 + 4] == MJ_FIX_REST && !line[4].note && !line[5].hold && !line[5].note && s.undo_n == 2 &&
          !s.part[0].lane[LANE_MEL_UP].steps[16 + 4].on, "a tap on sounding step 5: a REST fix, its hold silent too");
    seq_set_step(&s, LANE_MEL_UP, 1, 4, 0);
    CHECK(s.undo_n == 2, "a re-send: no edit");
    s.part[0].mjam.fix[16 + 6] = MJ_FIX_HOLD;    /* after the rest: silent (and the hold after it) */
    s.part[0].mjam.fix[16 + 10] = 70;            /* a note over a note: its hold after it stays, now its own */
    s.part[0].mjam.fix[16 + 12] = MJ_FIX_HOLD;   /* after that hold: held on, and the hold after it */
    mel_rerender(&s);
    seq_mel_line(&s, 1, line);
    CHECK(!line[6].note && !line[6].hold && !line[7].note && !line[7].hold && line[8].note == was[8].note && seq_mel_frame(&s, 1, 6) == 0,
          "HOLD after a rest: silent, a rest dot (the hold after it too)");
    CHECK(line[10].note == 70 && line[11].hold && line[12].hold && !line[12].note && line[13].hold && line[13].tension == line[10].tension &&
          s.part[0].lane[LANE_MEL_UP].steps[16 + 10].len == 4 && !s.part[0].lane[LANE_MEL_UP].steps[16 + 12].on && seq_mel_fixed_count(&s, 1) == 5,
          "a note fix plays its note, the hold after it stays; HOLD after it holds it on (4 steps), its tension the note's");
    seq_undo(&s);
    seq_mel_line(&s, 1, line);
    CHECK(s.part[0].mjam.fix[16 + 4] == 0 && s.part[0].mjam.fix[16 + 3] == 60 && line[4].note == was[4].note, "undo: the REST fix goes");
    seq_undo(&s);
    seq_mel_line(&s, 1, line);
    CHECK(!memcmp(line, was, sizeof was) && seq_mel_fixed_count(&s, 1) == 0, "undo again: bar 2 as it was");
}

/* 0.20 Task 4: JAM blocks switch in time at the SWITCH AT boundary */

static void quiet(void *ctx, int status, int note, int vel, int frame) { (void)ctx, (void)status, (void)note, (void)vel, (void)frame; }

/* plays 128-frame blocks from `ppq` (a start first, when stopped) until step k - 1 has fired; returns the position reached */
static double play_to_k(seq_t *s, double ppq, long k) {
    ev_seq = s;
    while (!s->playing || s->next_k < k) {
        ev_ppq = ppq;
        seq_block(s, ppq, bpm, 1, 128);
        ppq += blk();
    }
    ev_seq = NULL;
    return ppq;
}

#define MJ_KMAX 512
/* the Melody channel's note-ons logged since nev = 0, by the step that fired them: got[k] the note (0 none); 1 when no step had
 * two */
static int mel_ons(const seq_t *s, int got[MJ_KMAX]) {
    static int n[MJ_KMAX];
    int ch = seq_channel_of(s, LANE_MEL_UP), ok = 1;
    memset(got, 0, sizeof(int) * MJ_KMAX);
    memset(n, 0, sizeof n);
    for (int i = 0; i < nev; i++) {
        if (!is_on(&evs[i]) || (evs[i].status & 15) != ch - 1 || evs[i].k < 0 || evs[i].k >= MJ_KMAX) continue;
        got[evs[i].k] = evs[i].note;
        ok &= ++n[evs[i].k] == 1;
    }
    return ok;
}

/* 1 when each step k in [from, to) played old's note before `at` and nw's from it (len: the loop's steps) */
static int mel_switched_at(const int got[MJ_KMAX], long from, long to, long at, const mjam_note_t *old, const mjam_note_t *nw, int len) {
    int ok = 1;
    for (long k = from; k < to; k++) ok &= got[k] == (k < at ? old : nw)[k % len].note;
    return ok;
}

/* the model's lines of the playing part with block c, l (its other settings, NEW BAR variations and locks; no fixes), every bar */
static void model_lines(const seq_t *s, int c, int l, mjam_note_t out[SEQ_LANE_STEPS]) {
    static mharm_t h;
    const seq_part_t *p = &s->part[s->cur_part];
    mjam_set_t m = p->mjam.set;
    m.c = (signed char)c, m.l = (signed char)l;
    mharm(s, &h);
    mjam_loop_in_t in = {s->key, s->scale, p->bars, m, h.bar, p->lane[LANE_MEL_UP].bar_var};
    for (int b = 0; b < p->bars; b++) mjam_bar(&in, b, p->mjam.nlock >> b & 1 ? &p->mjam.frozen[b] : &m, &out[b * SEQ_STEPS]);
}

/* a copy of s, nothing queued, its playing part's block c, l rendered afresh by the setters (fixes included) */
static const seq_part_t *rendered_with(const seq_t *s, int c, int l) {
    static seq_t r;
    r = *s;
    r.emit = quiet;
    r.mjq_c = -1;
    r.part[r.cur_part].mjam.set.c = (signed char)c, r.part[r.cur_part].mjam.set.l = (signed char)l;
    mel_rerender(&r);
    return &r.part[r.cur_part];
}

/* 1 when line and st hold p's Melody lines and steps, every bar of the loop (hidden bars are rendered when it grows) */
static int same_lines(const seq_part_t *p, const mjam_note_t *line, const seq_step_t *st) {
    int n = p->bars * SEQ_STEPS;
    return !memcmp(p->mline, line, sizeof *line * (size_t)n) && !memcmp(p->lane[LANE_MEL_UP].steps, st, sizeof *st * (size_t)n);
}

/* the queued block's buffers are a fresh render of it */
static int queue_is_fresh(const seq_t *s) { return same_lines(rendered_with(s, s->mjq_c, s->mjq_l), s->mj_next_line, s->mj_next); }

/* the playing lane is a fresh render of the part's block */
static int lane_is_fresh(const seq_t *s) {
    const seq_part_t *p = &s->part[s->cur_part];
    return same_lines(rendered_with(s, p->mjam.set.c, p->mjam.set.l), p->mline, p->lane[LANE_MEL_UP].steps);
}

/* A 16-bar loop at 120 BPM, ARP PINKY queued at bar 2 step 5: each SWITCH AT's boundary from the part's bar 1 */
static void test_20_switch_points(void) {
    static seq_t s;
    static mjam_note_t old[SEQ_LANE_STEPS], nw[SEQ_LANE_STEPS];
    static int got[MJ_KMAX];
    static const long AT[4] = {24, 32, 32, 64};   /* bar 2 beat 3, bar 3, bar 3 (2 bars from bar 1), bar 5 */
    static const char *const NAME[4] = {"BEAT", "BAR", "2 BARS", "PHRASE"};
    for (int sw = 0; sw < 4; sw++) {
        int c = -1, l = -1, at = -1, differ = 0, once, polled, again;
        double ppq;
        seq_init(&s, emit, NULL);
        seq_set_bars(&s, 16);
        seq_set_mel_switch(&s, sw);
        model_lines(&s, MJ_ARP, 0, old);
        model_lines(&s, MJ_ARP, 2, nw);
        nev = 0;
        ppq = play_to_k(&s, 0, 21);   /* bar 2 step 5 has played */
        seq_mel_queue(&s, MJ_ARP, 2);
        polled = seq_mel_poll(&s);   /* (CHECK reads its condition twice: no call that changes state in it) */
        CHECK(seq_mel_switch(&s) == sw && seq_mel_queued(&s, &c, &l, &at) && c == MJ_ARP && l == 2 && at == AT[sw] / 16 &&
              !polled, "SWITCH AT %s: ARP PINKY queued at bar 2 step 5, its boundary in bar %d", NAME[sw], at + 1);
        ppq = play_to_k(&s, ppq, AT[sw] + 32);
        seq_block(&s, ppq, bpm, 0, 128);
        once = mel_ons(&s, got);
        for (long k = AT[sw]; k < AT[sw] + 16; k++) differ |= old[k].note != nw[k].note;
        CHECK(differ && nw[AT[sw]].note && once && balanced() && mel_switched_at(got, 0, AT[sw] + 32, AT[sw], old, nw, 256),
              "SWITCH AT %s: the new block's first note at step %ld of bar %ld, every step before it the old block's, none twice",
              NAME[sw], AT[sw] % 16 + 1, AT[sw] / 16 + 1);
        CHECK(!seq_mel_queued(&s, NULL, NULL, NULL) && s.part[0].mjam.set.c == MJ_ARP && s.part[0].mjam.set.l == 2 && lane_is_fresh(&s),
              "SWITCH AT %s: then nothing queued; the part's block is ARP PINKY, its lane as rendered", NAME[sw]);
        polled = seq_mel_poll(&s);
        again = seq_mel_poll(&s);
        CHECK(polled == 1 && seq_complexity(&s, LANE_MEL_UP) == 50 && seq_mel_level(&s) == 2 && !again,
              "SWITCH AT %s: one screen poll sees the switch: COMPLEXITY 50 (level 3)", NAME[sw]);
    }
}

/* PUSH never plays the old block's note early into the bar line a block switches at: the new block's note plays on the beat */
static void test_20_switch_push(void) {
    static seq_t s;
    static mjam_note_t nw[SEQ_LANE_STEPS];
    static int got[MJ_KMAX];
    int once, ch;
    double ppq;
    seq_init(&s, emit, NULL);   /* 1-bar chords: every bar line begins a chord */
    seq_set_mel_push(&s, 100);
    ch = seq_channel_of(&s, LANE_MEL_UP);
    model_lines(&s, MJ_ARP, 2, nw);
    nev = 0;
    ppq = play_to_k(&s, 0, 21);
    seq_mel_queue(&s, MJ_ARP, 2);
    ppq = play_to_k(&s, ppq, 40);
    seq_block(&s, ppq, bpm, 0, 128);
    once = mel_ons(&s, got);
    CHECK(ons_near(0, ch, 3.875) == 1 && ons_between(0, ch, 7.8, 7.99) == 0 && ons_near(0, ch, 8.0) == 1 && got[32] == nw[32].note &&
          once && balanced(), "PUSH 100: bar 2's note 1/8 early, but not bar 3's, where ARP PINKY takes over: its note on the beat");
}

/* Final review: a block tap is refused when a part switch is queued and takes the bar line first; a shrunk loop renders only its
 * visible bars, and a grow back shows the hidden bars with their own fixes */
static void test_20_final_review(void) {
    static seq_t s;
    static mjam_note_t keep[SEQ_STEPS];
    static seq_step_t keep_st[SEQ_STEPS];
    int c = -1, l = -1;
    seq_init(&s, emit, NULL);   /* 4 bars, SWITCH AT BAR */
    nev = 0;
    play_to_k(&s, 0, 21);
    seq_select_part(&s, 1);   /* queued: takes over at bar 2 */
    CHECK(seq_queued_part(&s) == 1 && seq_mel_blocked(&s), "a part switch queued at bar 1 step 5: a SWITCH AT BAR block is blocked");
    seq_mel_queue(&s, MJ_HOOK, 1);
    CHECK(!seq_mel_queued(&s, &c, &l, NULL), "the block tap is refused (nothing queued)");
    seq_set_mel_switch(&s, 0);   /* BEAT: its boundary (step 24) is before the bar line */
    CHECK(!seq_mel_blocked(&s), "SWITCH AT BEAT: the beat comes before the part's bar line");
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_medit_open(&s, 9);
    seq_medit_note(&s, 2);
    seq_medit_done(&s);
    memcpy(keep, &s.part[0].mline[9 * SEQ_STEPS], sizeof keep);
    memcpy(keep_st, &s.part[0].lane[LANE_MEL_UP].steps[9 * SEQ_STEPS], sizeof keep_st);
    seq_set_bars(&s, 4);
    seq_medit_open(&s, 1);
    seq_medit_note(&s, 2);
    seq_medit_done(&s);
    seq_set_bars(&s, 16);
    CHECK(memcmp(keep, &s.part[0].mline[9 * SEQ_STEPS], sizeof keep) == 0 &&
          memcmp(keep_st, &s.part[0].lane[LANE_MEL_UP].steps[9 * SEQ_STEPS], sizeof keep_st) == 0 && seq_mel_fixed_count(&s, 9) == 1 &&
          seq_mel_fixed_count(&s, 1) == 1, "16 -> 4 -> 16 with a visible bar edited: bar 10 renders again with its own fix");
}

/* The playing block cancels a queued one, another replaces it; stopped, a tap switches at once (a stop switches a queued one) */
static void test_20_switch_cancel_replace_stopped(void) {
    static seq_t s;
    static mjam_note_t old[SEQ_LANE_STEPS], run[SEQ_LANE_STEPS];
    static int got[MJ_KMAX];
    int c = -1, l = -1, once, polled;
    double ppq;
    seq_init(&s, emit, NULL);   /* 4 bars, SWITCH AT BAR */
    model_lines(&s, MJ_ARP, 0, old);
    model_lines(&s, MJ_RUN, 2, run);
    nev = 0;
    ppq = play_to_k(&s, 0, 21);
    seq_mel_queue(&s, MJ_HOOK, 1);
    seq_mel_queue(&s, MJ_ARP, 0);
    CHECK(!seq_mel_queued(&s, &c, &l, NULL), "a tap on the playing block (ARP UP) cancels the queued HOOK MOTIF");
    ppq = play_to_k(&s, ppq, 50);
    once = mel_ons(&s, got);
    polled = seq_mel_poll(&s);
    CHECK(once && mel_switched_at(got, 0, 50, 50, old, old, 64) && s.part[0].mjam.set.c == MJ_ARP && s.part[0].mjam.set.l == 0 &&
          !polled, "the old block plays on past the bar line");
    seq_mel_queue(&s, MJ_HOOK, 1);
    seq_mel_queue(&s, MJ_RUN, 2);
    CHECK(seq_mel_queued(&s, &c, &l, NULL) && c == MJ_RUN && l == 2 && queue_is_fresh(&s), "a second tap replaces it: RUN RUN-UP queued");
    ppq = play_to_k(&s, ppq, 80);
    once = mel_ons(&s, got);
    polled = seq_mel_poll(&s);
    CHECK(once && mel_switched_at(got, 50, 80, 64, old, run, 64) && s.part[0].mjam.set.c == MJ_RUN && s.part[0].mjam.set.l == 2 &&
          polled && seq_complexity(&s, LANE_MEL_UP) == 50, "RUN RUN-UP takes over at the next bar line (the wrap)");
    seq_block(&s, ppq, bpm, 0, 128);
    seq_mel_queue(&s, MJ_LEAD, 3);
    polled = seq_mel_poll(&s);
    CHECK(!seq_mel_queued(&s, NULL, NULL, NULL) && s.part[0].mjam.set.c == MJ_LEAD && s.part[0].mjam.set.l == 3 && mel_is_model(&s) &&
          seq_complexity(&s, LANE_MEL_UP) == 70 && !polled && !seq_can_undo(&s),
          "stopped, a tap switches at once: LEAD PASSING plays, COMPLEXITY 70, no undo step");
    ppq = play_to_k(&s, 0, 5);
    seq_mel_queue(&s, MJ_OST, 1);
    seq_block(&s, ppq, bpm, 0, 128);
    polled = seq_mel_poll(&s);
    CHECK(!seq_mel_queued(&s, NULL, NULL, NULL) && s.part[0].mjam.set.c == MJ_OST && s.part[0].mjam.set.l == 1 && mel_is_model(&s) &&
          polled && seq_complexity(&s, LANE_MEL_UP) == 30, "a stop before the boundary: the queued block takes over at once");
    seq_mel_queue(&s, MJ_COLS, 0);
    seq_mel_queue(&s, 0, MJ_LEVELS);
    seq_mel_queue(&s, -1, 0);
    seq_set_mel_switch(&s, 4);
    seq_set_mel_switch(&s, -1);
    CHECK(s.part[0].mjam.set.c == MJ_OST && s.part[0].mjam.set.l == 1 && seq_mel_switch(&s) == 1, "a block or SWITCH AT out of range: ignored");
    seq_set_style(&s, LANE_MEL_UP, GEN_MEL_RECIPE);
    ppq = play_to_k(&s, 0, 5);
    seq_mel_queue(&s, MJ_HOOK, 2);
    CHECK(seq_mel_queued(&s, NULL, NULL, NULL) && s.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && s.part[0].mjam.set.c == MJ_ARP &&
          s.part[0].mjam.set.l == 0, "a Melody lane that isn't JAM (Task 6 ruling): first a new instance's JAM lane (ARP UP), then HOOK queued");
    seq_block(&s, ppq, bpm, 0, 128);
}

/* Review Focus 1: a 4-bar loop, SWITCH AT PHRASE, queued in bar 4: the switch lands on the wrap. With a part queued for the same bar
 * line the part switch wins, the queue is dropped and the new part's Melody plays its own block */
static void test_20_switch_at_wrap_and_part(void) {
    static seq_t s;
    static mjam_note_t old[SEQ_LANE_STEPS], nw[SEQ_LANE_STEPS], intro[SEQ_LANE_STEPS];
    static int got[MJ_KMAX];
    int at = -1, once, polled;
    double ppq;
    seq_init(&s, emit, NULL);
    seq_set_mel_switch(&s, 3);
    model_lines(&s, MJ_ARP, 0, old);
    model_lines(&s, MJ_ARP, 2, nw);
    nev = 0;
    ppq = play_to_k(&s, 0, 53);   /* bar 4 step 5 */
    seq_mel_queue(&s, MJ_ARP, 2);
    CHECK(seq_mel_queued(&s, NULL, NULL, &at) && at == 0, "a 4-bar loop, SWITCH AT PHRASE, queued in bar 4: its boundary is bar 1 (the wrap)");
    ppq = play_to_k(&s, ppq, 96);
    seq_block(&s, ppq, bpm, 0, 128);
    once = mel_ons(&s, got);
    CHECK(once && balanced() && mel_switched_at(got, 0, 96, 64, old, nw, 64) && s.part[0].mjam.set.l == 2,
          "the switch lands on the wrap: bar 4 plays the old block, bar 1 the new, nothing twice");

    seq_init(&s, emit, NULL);
    seq_set_mel_switch(&s, 3);
    seq_select_part(&s, 1);          /* stopped: at once; its first use copies the Verse */
    seq_mel_queue(&s, MJ_OST, 1);    /* stopped: the Intro's block at once */
    seq_select_part(&s, 0);
    model_lines(&s, MJ_ARP, 0, old);
    nev = 0;
    ppq = play_to_k(&s, 0, 53);
    seq_mel_queue(&s, MJ_ARP, 2);
    seq_select_part(&s, 1);
    memcpy(intro, s.part[1].mline, sizeof intro);
    CHECK(seq_mel_queued(&s, NULL, NULL, &at) && at == 0 && seq_queued_part(&s) == 1 && s.mjq_part == 0 && queue_is_fresh(&s),
          "the Verse's ARP PINKY and the Intro both queued for the same bar line (the Intro's render left the Verse's queue as it was)");
    ppq = play_to_k(&s, ppq, 96);
    seq_block(&s, ppq, bpm, 0, 128);
    once = mel_ons(&s, got);
    polled = seq_mel_poll(&s);
    CHECK(seq_part(&s) == 1 && !seq_mel_queued(&s, NULL, NULL, NULL) && !polled && s.part[0].mjam.set.l == 0 &&
          s.part[1].mjam.set.c == MJ_OST && s.part[1].mjam.set.l == 1, "the part switch wins: the melody queue is dropped, the Verse keeps ARP UP");
    CHECK(once && balanced() && mel_switched_at(got, 0, 96, 64, old, intro, 64), "the Intro plays its own OSTINATO OCTAVE from its bar 1, nothing twice");
}

/* Review Focus 2: FORM, a fix, a chord and the key change after the queue, before the boundary: the block that takes over has them */
static void test_20_queue_follows_edits(void) {
    static seq_t s;
    static mjam_note_t was[SEQ_LANE_STEPS];
    static int got[MJ_KMAX];
    int ok = 1, moved = 1, once;
    double ppq;
    seq_init(&s, emit, NULL);   /* 4 bars, SWITCH AT BAR */
    nev = 0;
    ppq = play_to_k(&s, 0, 21);
    seq_mel_queue(&s, MJ_HOOK, 1);
    ok &= queue_is_fresh(&s);
    memcpy(was, s.mj_next_line, sizeof was);
    s.part[0].mjam.set.form = MF_1_1;   /* FORM: written here, rendered by the next setter (as Task 5's setter will) */
    seq_set_step(&s, LANE_MEL_UP, 2, 6, !s.part[0].lane[LANE_MEL_UP].steps[2 * SEQ_STEPS + 6].on);   /* a fix in bar 3 */
    ok &= queue_is_fresh(&s) && s.part[0].mjam.fix[2 * SEQ_STEPS + 6];
    moved &= memcmp(was, s.mj_next_line, sizeof was) != 0;
    memcpy(was, s.mj_next_line, sizeof was);
    seq_set_slot(&s, 3, 3, 0, COL_TRIAD);   /* bar 4's chord */
    ok &= queue_is_fresh(&s);
    moved &= memcmp(was, s.mj_next_line, sizeof was) != 0;
    memcpy(was, s.mj_next_line, sizeof was);
    seq_set_key(&s, 7);
    ok &= queue_is_fresh(&s);
    moved &= memcmp(was, s.mj_next_line, sizeof was) != 0;
    CHECK(ok && moved, "FORM 1+1 and a fix, a chord edit, a key change while HOOK MOTIF is queued: each renders the queued block again");
    ppq = play_to_k(&s, ppq, 64);
    seq_block(&s, ppq, bpm, 0, 128);
    once = mel_ons(&s, got);
    CHECK(s.part[0].mjam.set.c == MJ_HOOK && s.part[0].mjam.set.form == MF_1_1 && seq_key(&s) == 7 && lane_is_fresh(&s),
          "after the switch the lane is a fresh render of HOOK MOTIF with all of them");
    CHECK(once && balanced() && mel_switched_at(got, 32, 64, 32, s.part[0].mline, s.part[0].mline, 64), "and plays it from the bar line");
}

/* Review Focus 3: a loop shorter than the boundary switches at the wrap */
static void test_20_switch_short_loop(void) {
    static seq_t s;
    static mjam_note_t old[SEQ_LANE_STEPS], nw[SEQ_LANE_STEPS];
    static int got[MJ_KMAX];
    static const int BARS[3] = {2, 1, 1}, SW[3] = {3, 3, 2};
    for (int t = 0; t < 3; t++) {
        int at = -1, len = BARS[t] * SEQ_STEPS, once;
        double ppq;
        seq_init(&s, emit, NULL);
        seq_set_bars(&s, BARS[t]);
        seq_set_mel_switch(&s, SW[t]);
        model_lines(&s, MJ_ARP, 0, old);
        model_lines(&s, MJ_HOOK, 3, nw);
        nev = 0;
        ppq = play_to_k(&s, 0, 5);
        seq_mel_queue(&s, MJ_HOOK, 3);
        CHECK(seq_mel_queued(&s, NULL, NULL, &at) && at == 0, "a %d-bar loop, SWITCH AT %s: the boundary is bar 1 (the wrap)", BARS[t],
              SW[t] == 3 ? "PHRASE" : "2 BARS");
        ppq = play_to_k(&s, ppq, len + 32);
        seq_block(&s, ppq, bpm, 0, 128);
        once = mel_ons(&s, got);
        CHECK(once && balanced() && mel_switched_at(got, 0, len + 32, len, old, nw, len) && s.part[0].mjam.set.l == 3,
              "a %d-bar loop: HOOK SEQUENCE takes over at the wrap (step %d), nothing twice", BARS[t], len);
    }
}

/* 10 000 512-frame blocks at random tempos with random queues, SWITCH AT, loop lengths, parts and keys (ASan, UBSan): every switch is a copy
 * of a fresh render, no step plays twice, no note hangs */
static void test_20_switch_audio_safe(void) {
    static seq_t s;
    static const int BARS[5] = {1, 2, 4, 8, 16};
    static int n[4096];
    unsigned r = 20201020u;
    double ppq = 0;
    int fresh_ok = 1, once = 1, switches = 0, ch;
    seq_init(&s, emit, NULL);
    ch = seq_channel_of(&s, LANE_MEL_UP);
    memset(n, 0, sizeof n);
    nev = 0;
    for (int i = 0; i < 10000; i++) {
        unsigned x;
        double tempo;
        r = r * 1103515245u + 12345u;
        x = r >> 8;
        tempo = 60 + x % 141;
        if (x % 29 == 0) seq_mel_queue(&s, (int)(x / 29 % MJ_COLS), (int)(x / 97 % MJ_LEVELS));
        if (x % 101 == 0) seq_set_mel_switch(&s, (int)(x / 101 % 4));
        if (x % 211 == 0) seq_set_bars(&s, BARS[x / 211 % 5]);
        if (x % 307 == 0) seq_select_part(&s, (int)(x / 307 % 3));
        if (x % 401 == 0) seq_set_key(&s, (int)(x / 401 % 12));
        ev_seq = &s;
        ev_ppq = ppq;
        seq_block(&s, ppq, tempo, 1, 512);
        ev_seq = NULL;
        ppq += 512 * tempo / (60.0 * 44100.0);
        if (seq_mel_poll(&s)) {
            switches++;
            fresh_ok &= lane_is_fresh(&s);
        }
        if (s.mjq_c >= 0 && x % 7 == 0) fresh_ok &= queue_is_fresh(&s);
    }
    seq_block(&s, ppq, 120, 0, 128);
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].k >= 0 && evs[i].k < 4096) once &= ++n[evs[i].k] == 1;
    CHECK(switches > 20 && fresh_ok, "10 000 blocks, random tempos and queues: %d switches, each the lane a fresh render (the queue too)", switches);
    CHECK(once && balanced() && nev < MAXEV, "no Melody step played twice, no note hung (%d events)", nev);
}

/* 0.20 Task 5: the PHRASE page's settings, the ANSWER BLOCK picker, the bar editor and the MELODY views */

/* FNV-1a of the playing part's Melody lines, every bar */
static unsigned mel_lines_hash(const seq_t *s) {
    const seq_part_t *p = &s->part[s->cur_part];
    const unsigned char *b = (const unsigned char *)p->mline;
    unsigned h = 2166136261u;
    for (size_t i = 0; i < sizeof(mjam_note_t) * (size_t)(p->bars * SEQ_STEPS); i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* the playing part's SWAP block is (c, l) */
static int swap_is(const seq_t *s, int c, int l) { return s->part[s->cur_part].mjam.set.sc == c && s->part[s->cur_part].mjam.set.sl == l; }

/* Each setting renders the model at once (a locked bar keeps its own, a queued block follows); no undo step; clamped */
static void test_20_settings(void) {
    static seq_t s;
    static const int LO[MJS_COUNT] = {0, 0, 0, 0, 0, 0, -1, 0}, HI[MJS_COUNT] = {5, 3, 3, 3, 5, 3, 1, 2};
    static const char *const NAME[MJS_COUNT] = {"FORM", "BREATH", "CADENCE", "FILL", "ANSWER", "STRAY", "OCTAVE", "REGISTER"};
    mjam_note_t was[SEQ_STEPS], line[SEQ_STEPS];
    mjam_set_t m;
    char got[256];
    unsigned h;
    double ppq;
    int n;
    seq_init(&s, emit, NULL);   /* E minor, Em C G D */
    seq_mel_queue(&s, MJ_ARP, 2);   /* stopped: ARP PINKY at once */
    seq_mjam_set(&s, MJS_FORM, MF_2_2);
    seq_mjam_set(&s, MJS_BREATH, 1);
    seq_mjam_set(&s, MJS_FILL, 3);
    mel_show(&s, 1, got, sizeof got);
    CHECK(!strcmp(got, "C4 - C5 - E4 - C5 - G4 - B4 - C4 D4 E4 F#4 | dots 11,63,31,67,16,64,31,67,21,65,28,66,11,12,16,23 | score 31") &&
          seq_mjam_get(&s, MJS_FORM) == MF_2_2 && seq_mjam_get(&s, MJS_BREATH) == 1 && seq_mjam_get(&s, MJS_FILL) == 3,
          "ARP PINKY, FORM 2+2, BREATH 1/8, FILL RUN: bar 2 is the reference's spot line: %s", got);
    CHECK(mel_is_model(&s) && mel_plays_line(&s) && s.undo_n == 0, "every bar the model's, played; knob turns: no undo step");
    s.part[0].mjam.set = mel_spot_set();   /* HOOK MOTIF 1+1: a form with answers */
    s.part[0].mjam.set.ans = MA_MIRROR;   /* an answer STRAY acts on (ECHO changes nothing) */
    mel_rerender(&s);
    for (int f = 0; f < MJS_REG + 1; f++) {   /* 0.21: the three new fields are test_21_settings's */
        int model = 1, moved = 0, v0 = seq_mjam_get(&s, f);
        unsigned h0 = mel_lines_hash(&s);
        for (int v = LO[f]; v <= HI[f]; v++) {
            seq_mjam_set(&s, f, v);
            model &= seq_mjam_get(&s, f) == v && mel_is_model(&s);
            moved += mel_lines_hash(&s) != h0;
        }
        seq_mjam_set(&s, f, v0);
        CHECK(model && moved && mel_lines_hash(&s) == h0 && s.undo_n == 0,
              "%s: each value renders the model's lines (%d of %d differ from %d's), turning back restores them, no undo step", NAME[f], moved,
              HI[f] - LO[f] + 1, v0);
    }
    seq_mjam_set(&s, MJS_FORM, 9);
    seq_mjam_set(&s, MJS_AOCT, -4);
    seq_mjam_set(&s, MJS_REG, 7);
    m = s.part[0].mjam.set;
    seq_mjam_set(&s, MJS_COUNT, 1);
    seq_mjam_set(&s, -1, 1);
    CHECK(seq_mjam_get(&s, MJS_FORM) == MF_SENT && seq_mjam_get(&s, MJS_AOCT) == -1 && seq_mjam_get(&s, MJS_REG) == 2 && mel_is_model(&s) &&
          !memcmp(&m, &s.part[0].mjam.set, sizeof m) && seq_mjam_get(&s, MJS_COUNT) == 0 && seq_mjam_get(&s, -1) == 0,
          "out of range: clamped (FORM SENT, OCTAVE -1, REGISTER High); a bad field: ignored, reads 0");
    /* a locked bar keeps the settings it had */
    s.part[0].mjam.set = mel_spot_set();
    mel_rerender(&s);
    seq_medit_open(&s, 1);
    seq_medit_lock(&s);
    seq_medit_done(&s);
    n = s.undo_n;
    seq_mel_line(&s, 1, was);
    h = mel_lines_hash(&s);
    seq_mjam_set(&s, MJS_FORM, MF_LOOP);
    seq_mjam_set(&s, MJS_REG, 2);
    seq_mjam_set(&s, MJS_BREATH, 3);
    seq_mel_line(&s, 1, line);
    CHECK(!memcmp(was, line, sizeof was) && mel_lines_hash(&s) != h && mel_is_model(&s) && s.undo_n == n,
          "a locked bar (2) keeps its line; the other bars move; no undo step");
    /* while a block is queued, and with the editor open */
    ppq = play_to_k(&s, 0.0, 5);
    seq_mel_queue(&s, MJ_LEAD, 1);
    seq_medit_open(&s, 2);
    seq_mjam_set(&s, MJS_FORM, MF_AAB);
    CHECK(s.mjq_c == MJ_LEAD && queue_is_fresh(&s) && lane_is_fresh(&s) && seq_medit_bar(&s) == 2,
          "LEAD HALVES queued: its buffer renders FORM AAB too; the open editor stays open");
    seq_block(&s, ppq, bpm, 0, 128);
    /* a lane that isn't JAM */
    seq_init(&s, emit, NULL);
    old_mel(&s);
    m = s.part[0].mjam.set;
    seq_mjam_set(&s, MJS_FORM, MF_2_2);
    seq_mel_swap(&s, MJ_ARP, 4);
    m.form = MF_2_2, m.sc = MJ_ARP, m.sl = 4;
    CHECK(!memcmp(&m, &s.part[0].mjam.set, sizeof m) && s.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && s.undo_n == 1,
          "a Melody lane that isn't JAM (Task 6 ruling): the first setter makes it a new instance's JAM lane (one undo step), then both apply");
}

/* The ANSWER BLOCK picker: a try plays at once, CANCEL restores the SWAP block, DONE keeps it; the call's own block is refused */
static void test_20_swap_picker(void) {
    static seq_t s;
    static mjam_note_t before[SEQ_LANE_STEPS];
    unsigned h0;
    int n, calls, answers;
    seq_init(&s, emit, NULL);   /* E minor, Em C G D */
    s.part[0].mjam.set = mel_spot_set();   /* HOOK MOTIF 1+1 */
    seq_mjam_set(&s, MJS_ANS, MA_SWAP);
    n = s.undo_n;
    h0 = mel_lines_hash(&s);
    memcpy(before, s.part[0].mline, sizeof before);
    CHECK(swap_is(&s, MJ_RUN, 2) && seq_mview(&s) == MVIEW_JAM && seq_mmode(&s) == 0 && mel_is_model(&s),
          "ANSWER SWAP plays RUN RUN-UP (the default); a new instance shows JAM (mode 0)");
    seq_set_mview(&s, MVIEW_PHRASE);
    CHECK(seq_mview(&s) == MVIEW_PHRASE && seq_mmode(&s) == 1, "PHRASE: mode 1");
    seq_mpick_open(&s);
    CHECK(seq_mview(&s) == MVIEW_PICK && seq_mmode(&s) == 1 && seq_follow_paused(&s, TAB_MEL) == 1 && seq_follow_paused(&s, TAB_BASS) == 0 &&
          seq_follow_paused(&s, TAB_CHORDS) == 0, "the picker opens: MVIEW_PICK, MELODY's FOLLOW paused (only)");
    seq_mpick_try(&s, MJ_RUN, 1);
    calls = !memcmp(before, s.part[0].mline, sizeof(mjam_note_t) * 16) && !memcmp(&before[32], &s.part[0].mline[32], sizeof(mjam_note_t) * 16);
    answers = memcmp(&before[16], &s.part[0].mline[16], sizeof(mjam_note_t) * 16) && memcmp(&before[48], &s.part[0].mline[48], sizeof(mjam_note_t) * 16);
    CHECK(swap_is(&s, MJ_RUN, 1) && calls && answers && mel_is_model(&s) && s.undo_n == n,
          "try RUN TURN: the answer bars (2, 4) play it at once, the calls stay; no undo step");
    seq_mpick_cancel(&s);
    CHECK(swap_is(&s, MJ_RUN, 2) && mel_lines_hash(&s) == h0 && seq_mview(&s) == MVIEW_PHRASE && seq_follow_paused(&s, TAB_MEL) == 0 && s.undo_n == n,
          "CANCEL: RUN RUN-UP again, the lines as they were, back on PHRASE");
    seq_mpick_open(&s);
    seq_mpick_try(&s, MJ_HOOK, 1);
    CHECK(swap_is(&s, MJ_RUN, 2) && mel_lines_hash(&s) == h0, "trying the call's own block (HOOK MOTIF): refused, nothing changes");
    seq_mpick_try(&s, MJ_RUN, 1);
    seq_mpick_try(&s, MJ_COLS, 0);
    seq_mpick_try(&s, MJ_ARP, MJ_LEVELS);
    seq_mpick_try(&s, -1, 0);
    CHECK(swap_is(&s, MJ_RUN, 1), "out-of-range blocks: ignored");
    seq_mpick_done(&s);
    CHECK(swap_is(&s, MJ_RUN, 1) && seq_mview(&s) == MVIEW_PHRASE && s.undo_n == n && mel_is_model(&s), "try again, DONE keeps RUN TURN");
    seq_mpick_cancel(&s);
    seq_mpick_try(&s, MJ_ARP, 0);
    CHECK(swap_is(&s, MJ_RUN, 1), "closed: a CANCEL or a try is inert");
    seq_mpick_open(&s);
    seq_mpick_try(&s, MJ_ARP, 0);
    seq_undo(&s);
    CHECK(swap_is(&s, MJ_RUN, 1) && seq_mview(&s) == MVIEW_PHRASE && s.undo_n == n, "UNDO with the picker open is its CANCEL");
    seq_mel_swap(&s, MJ_PEDAL, 2);
    CHECK(swap_is(&s, MJ_PEDAL, 2) && s.undo_n == n && mel_is_model(&s), "seq_mel_swap (the field's arrows): PEDAL ALT, at once, no undo step");
    seq_mel_swap(&s, MJ_HOOK, 1);
    seq_mel_swap(&s, MJ_COLS, 1);
    CHECK(swap_is(&s, MJ_PEDAL, 2), "the call's own block or a bad one: refused");
    seq_mpick_open(&s);
    seq_mjam_set(&s, MJS_STRAY, 3);
    seq_mpick_try(&s, MJ_ARP, 0);
    seq_mpick_cancel(&s);
    CHECK(swap_is(&s, MJ_PEDAL, 2) && seq_mjam_get(&s, MJS_STRAY) == 3 && mel_is_model(&s), "a knob turned with the picker open stays after its CANCEL");
}

/* 1 when bar b of the playing part has its NEW BAR variation, lock, frozen settings and fixes as given */
static int medit_bar_is(const seq_t *s, int b, unsigned char var, int lock, const mjam_set_t *fro, const unsigned char *fix) {
    const seq_part_t *p = &s->part[s->cur_part];
    return p->lane[LANE_MEL_UP].bar_var[b] == var && (p->mjam.nlock >> b & 1) == lock && !memcmp(fro, &p->mjam.frozen[b], sizeof *fro) &&
           !memcmp(fix, &p->mjam.fix[b * SEQ_STEPS], SEQ_STEPS);
}

static void test_20_medit(void) {
    static seq_t s;
    static seq_snap_t ring[SEQ_UNDO];
    seq_part_t *p = &s.part[0];
    const seq_step_t *st = p->lane[LANE_MEL_UP].steps;
    mjam_note_t line0[SEQ_STEPS], line[SEQ_STEPS];
    unsigned char fix0[SEQ_STEPS], var0;
    mjam_set_t fro0;
    int n0, n, r, pushes, lock0;
    seq_init(&s, emit, NULL);   /* E minor, Em C G D */
    seq_mel_queue(&s, MJ_OST, 0);   /* OSTINATO PULSE: a note on every even step, a rest between */
    seq_mel_line(&s, 1, line0);
    n0 = s.undo_n;
    CHECK(seq_medit_bar(&s) == -1 && seq_medit_sel(&s) == -1, "closed: no bar, no step");
    seq_medit_open(&s, 1);
    CHECK(seq_medit_bar(&s) == 1 && seq_mview(&s) == MVIEW_EDIT && seq_mmode(&s) == 1 && seq_follow_paused(&s, TAB_MEL) == 1 &&
          seq_centre_slot(&s, TAB_MEL) == 1 && seq_medit_sel(&s) == 0 && line0[0].note && seq_medit_oct(&s) == line0[0].note / 12 - 1 &&
          s.undo_n == n0, "bar 2 opens: EDIT, FOLLOW paused on it, its first note selected, the octave that note's, no undo step");
    r = seq_medit_select(&s, 3);
    CHECK(r == 1 && seq_medit_sel(&s) == 3 && !line0[3].note && !line0[3].hold, "a rest step selects");
    seq_medit_set_oct(&s, 5);
    CHECK(seq_medit_oct(&s) == 5 && s.undo_n == n0 && !p->mjam.fix[16 + 3], "OCTAVE 5 with a rest selected: the octave only");
    s.naud = 0;
    seq_medit_note(&s, 2);
    seq_mel_line(&s, 1, line);
    CHECK(p->mjam.fix[16 + 3] == 74 && line[3].note == 74 && st[16 + 3].on && st[16 + 3].abs == 74 && s.undo_n == n0 + 1 && s.naud == 1 &&
          s.aud[0].note == 74 && s.aud[0].lane == LANE_MEL_UP && seq_mel_fixed_count(&s, 1) == 1,
          "D on the piano: the rest step 4 fixed at D5, it sounds; one undo step, heard");
    seq_medit_note(&s, 2);
    seq_medit_note(&s, 12);
    seq_medit_set_oct(&s, 8);
    seq_medit_set_oct(&s, 2);
    r = seq_medit_select(&s, 16) + seq_medit_select(&s, -1);
    CHECK(s.undo_n == n0 + 1 && seq_medit_oct(&s) == 5 && seq_medit_sel(&s) == 3 && !r, "the same note again, a bad note, octave or step: nothing");
    s.naud = 0;
    seq_medit_set_oct(&s, 4);
    CHECK(p->mjam.fix[16 + 3] == 62 && s.undo_n == n0 + 2 && s.naud == 1 && s.aud[0].note == 62 && s.aud[0].lane == LANE_MEL_UP,
          "OCTAVE 4 re-fixes the sounding step at D4, heard (as the Bass editor)");
    seq_medit_select(&s, 4);
    CHECK(seq_medit_oct(&s) == line0[4].note / 12 - 1, "a sounding step selected: its note's octave");
    seq_medit_rest(&s);
    seq_mel_line(&s, 1, line);
    CHECK(p->mjam.fix[16 + 4] == MJ_FIX_REST && !line[4].note && !st[16 + 4].on && s.undo_n == n0 + 3, "REST on sounding step 5 silences it; one undo step");
    seq_medit_rest(&s);
    CHECK(s.undo_n == n0 + 3, "REST again: nothing");
    seq_medit_select(&s, 1);
    seq_medit_hold(&s);
    seq_mel_line(&s, 1, line);
    CHECK(p->mjam.fix[16 + 1] == MJ_FIX_HOLD && line[1].hold && st[16].len == 2 && s.undo_n == n0 + 4, "HOLD on the rest after step 1's note: extends it (2 steps)");
    seq_undo(&s);
    CHECK(!p->mjam.fix[16 + 1] && st[16].len == 1 && seq_medit_bar(&s) == 1 && s.undo_n == n0 + 3, "UNDO steps back the HOLD, the editor open");
    seq_medit_hold(&s);
    n = s.undo_n;
    seq_mel_line(&s, 1, line0);
    r = seq_medit_new_bar(&s);
    seq_mel_line(&s, 1, line);
    CHECK(r == 1 && p->lane[LANE_MEL_UP].bar_var[1] >= 1 && memcmp(line, line0, sizeof line) && line[3].note == 62 && !line[4].note && line[1].hold &&
          s.undo_n == n + 1, "NEW BAR: its own line (v %d), the fixes kept, one undo step", p->lane[LANE_MEL_UP].bar_var[1]);
    seq_medit_lock(&s);
    CHECK((p->mjam.nlock >> 1 & 1) && !memcmp(&p->mjam.frozen[1], &p->mjam.set, sizeof p->mjam.set) && s.undo_n == n + 2, "LOCK freezes the settings: one undo step");
    seq_mel_line(&s, 1, line0);
    seq_mjam_set(&s, MJS_REG, 2);
    seq_mjam_set(&s, MJS_FORM, MF_1_1);
    seq_mel_line(&s, 1, line);
    CHECK(!memcmp(line, line0, sizeof line) && seq_medit_bar(&s) == 1 && s.undo_n == n + 2, "LOCK, then REGISTER High and FORM 1+1: the bar keeps its line");
    seq_medit_reset_bar(&s);
    CHECK(!p->lane[LANE_MEL_UP].bar_var[1] && !seq_mel_fixed_count(&s, 1) && !(p->mjam.nlock >> 1 & 1) && s.undo_n == n + 3 && mel_is_model(&s),
          "RESET BAR: v 0, no fix, unlocked (the model's line, REGISTER High); one undo step");
    seq_medit_reset_bar(&s);
    CHECK(s.undo_n == n + 3, "RESET BAR on a fresh bar: nothing");
    seq_medit_done(&s);
    CHECK(seq_medit_bar(&s) == -1 && seq_mview(&s) == MVIEW_JAM && s.undo_n == n + 3, "DONE keeps its steps; back on the user's view (JAM)");
    /* CANCEL after all of these puts the bar back */
    seq_medit_open(&s, 2);
    seq_medit_select(&s, 1);
    seq_medit_note(&s, 4);
    seq_medit_lock(&s);
    seq_medit_done(&s);
    seq_medit_open(&s, 2);   /* reopened: the edits are kept by its DONE */
    n = s.undo_n;
    pushes = s.undo_pushes;
    memcpy(ring, s.undo, sizeof ring);
    var0 = p->lane[LANE_MEL_UP].bar_var[2];
    lock0 = p->mjam.nlock >> 2 & 1;
    fro0 = p->mjam.frozen[2];
    memcpy(fix0, &p->mjam.fix[32], 16);
    seq_mel_line(&s, 2, line0);
    seq_medit_select(&s, 3);
    seq_medit_note(&s, 7);
    seq_medit_select(&s, 4);
    seq_medit_rest(&s);
    seq_medit_select(&s, 5);
    seq_medit_hold(&s);
    r = seq_medit_new_bar(&s);
    seq_medit_lock(&s);   /* unlocked */
    seq_mjam_set(&s, MJS_FILL, 2);   /* a knob turn meanwhile: kept */
    seq_medit_reset_bar(&s);
    seq_medit_lock(&s);
    CHECK(r == 1 && lock0 && s.undo_pushes - pushes == 7, "edited: a note, REST, HOLD, NEW BAR, UNLOCK, RESET BAR, LOCK: %d undo steps", s.undo_pushes - pushes);
    seq_medit_cancel(&s);
    seq_mel_line(&s, 2, line);
    CHECK(seq_medit_bar(&s) == -1 && medit_bar_is(&s, 2, var0, lock0, &fro0, fix0) && !memcmp(line, line0, sizeof line) && s.undo_n == n &&
          !memcmp(ring, s.undo, sizeof ring) && seq_mjam_get(&s, MJS_FILL) == 2 && seq_mview(&s) == MVIEW_JAM,
          "CANCEL: v, fixes, lock and frozen settings as at open, the undo ring too; the knob kept");
    seq_medit_cancel(&s);
    CHECK(s.undo_n == n && seq_medit_bar(&s) == -1, "a second CANCEL is a no-op");
    seq_medit_open(&s, 2);
    seq_undo(&s);
    CHECK(seq_medit_bar(&s) == -1 && s.undo_n == n, "UNDO with none of its own steps left is the editor's CANCEL");
    /* NEW BAR on a bar every step of which is fixed: no variation changes it */
    for (int i = 0; i < SEQ_STEPS; i++) p->mjam.fix[48 + i] = MJ_FIX_REST;
    mel_rerender(&s);
    seq_medit_open(&s, 3);
    n = s.undo_n;
    seq_mel_line(&s, 3, line0);
    CHECK(seq_medit_sel(&s) == 0 && seq_medit_oct(&s) == 4, "a silent bar opens on step 1 at octave 4");
    r = seq_medit_new_bar(&s);
    seq_mel_line(&s, 3, line);
    CHECK(!r && !p->lane[LANE_MEL_UP].bar_var[3] && !memcmp(line, line0, sizeof line) && s.undo_n == n, "NEW BAR on a fully fixed bar: returns 0, nothing changes");
    seq_medit_done(&s);
    seq_medit_open(&s, 4);
    CHECK(seq_medit_bar(&s) == -1, "bar 5 of a 4-bar loop does not open");
    seq_medit_open(&s, 1);
    old_mel(&s);   /* the lane leaves JAM with the editor open */
    {
        static seq_step_t was[SEQ_LANE_STEPS];
        static mjam_state_t mj;
        memcpy(was, st, sizeof was);
        mj = p->mjam;
        n = s.undo_n;
        seq_medit_select(&s, 2);
        seq_medit_note(&s, 0);
        seq_medit_rest(&s);
        seq_medit_lock(&s);
        r = seq_medit_new_bar(&s);
        CHECK(!r && !memcmp(was, st, sizeof was) && !memcmp(&mj, &p->mjam, sizeof mj) && s.undo_n == n,
              "the lane left JAM with the editor open: its edits do nothing");
        seq_medit_cancel(&s);
        CHECK(!memcmp(was, st, sizeof was) && seq_medit_bar(&s) == -1, "and its CANCEL renders nothing over the lane");
    }
    seq_medit_open(&s, 1);
    seq_mpick_open(&s);
    CHECK(seq_medit_bar(&s) == -1 && seq_mview(&s) == MVIEW_PICK && s.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM,
          "a Melody lane that isn't JAM (Task 6 ruling): the editor's open makes it JAM and opens; the picker's then closes it and opens");
}

/* Review Focus 4: fixes that stop matching. A HOLD whose note before it becomes a rest plays silent (a rest dot); a fixed note in a
 * locked bar survives block switches and NEW BAR */
static void test_20_fix_edge_cases(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    const seq_step_t *st = p->lane[LANE_MEL_UP].steps;
    mjam_note_t was[SEQ_STEPS], line[SEQ_STEPS];
    double ppq;
    int r;
    seq_init(&s, emit, NULL);   /* E minor; ARP UP: a note on every even step, held one step */
    seq_medit_open(&s, 0);
    seq_medit_select(&s, 2);
    seq_medit_hold(&s);
    seq_medit_done(&s);
    seq_mel_line(&s, 0, line);
    CHECK(line[2].hold && !line[2].note && st[0].len == 4 && seq_mel_frame(&s, 0, 2) != 0, "ARP UP: a HOLD fix on step 3 holds step 1's note on (4 steps)");
    seq_mel_queue(&s, MJ_PEDAL, 0);   /* stopped: at once */
    seq_mel_line(&s, 0, line);
    CHECK(line[2].hold && st[0].on && st[0].len == 15 && p->mjam.fix[2] == MJ_FIX_HOLD, "PEDAL HOLD 5: the pedal holds through it");
    seq_mel_queue(&s, MJ_OST, 0);
    seq_mel_line(&s, 0, line);
    CHECK(!line[1].note && !line[2].note && !line[2].hold && seq_mel_frame(&s, 0, 2) == 0 && !st[2].on && st[0].len == 1 && mel_plays_line(&s),
          "OSTINATO PULSE: the step before it a rest, the HOLD plays silent, a rest dot (frame 0)");
    seq_mel_queue(&s, MJ_ARP, 0);
    seq_mel_line(&s, 0, line);
    CHECK(line[2].hold && st[0].len == 4 && p->mjam.fix[2] == MJ_FIX_HOLD, "ARP UP again: it holds on again (the fix kept through the switches)");
    /* a fixed note in a locked bar */
    seq_medit_open(&s, 1);
    seq_medit_select(&s, 5);
    seq_medit_set_oct(&s, 5);
    seq_medit_note(&s, 11);
    seq_medit_lock(&s);
    seq_medit_done(&s);
    seq_mel_line(&s, 1, was);
    seq_mel_queue(&s, MJ_LEAD, 4);
    seq_mel_line(&s, 1, line);
    CHECK(was[5].note == 83 && !memcmp(was, line, sizeof was) && p->mjam.set.c == MJ_LEAD && p->mjam.frozen[1].c == MJ_ARP,
          "a fixed B5 in a locked bar: the bar keeps its line and the fix through a block switch (LEAD SOAR)");
    ppq = play_to_k(&s, 0.0, 5);
    seq_mel_queue(&s, MJ_RUN, 3);
    ppq = play_to_k(&s, ppq, 17);
    seq_mel_line(&s, 1, line);
    r = seq_mel_poll(&s);   /* CHECK evaluates its condition twice: a state change goes before it */
    CHECK(r == 1 && p->mjam.set.c == MJ_RUN && p->mjam.set.l == 3, "a block switched in time (RUN RUN-DOWN at bar 2)");
    CHECK(!memcmp(was, line, sizeof was) && lane_is_fresh(&s), "the locked bar again kept its line and fix");
    seq_block(&s, ppq, bpm, 0, 128);
    seq_medit_open(&s, 1);
    r = seq_medit_new_bar(&s);
    seq_medit_done(&s);
    seq_mel_line(&s, 1, line);
    CHECK(r == 1 && line[5].note == 83 && p->mjam.fix[16 + 5] == 83 && (p->mjam.nlock >> 1 & 1) && memcmp(was, line, sizeof was),
          "NEW BAR: a new line, the fixed B5 kept, still locked");
}

/* An open picker or editor ends on a part switch, a load, INIT and seq_set_mview; a later CANCEL is inert. A PLAY tap on the JAM
 * lane closes it as DONE first, so its CANCEL can't take the tap back */
static void test_20_mel_txn_closed_by_switch(void) {
    static seq_t s;
    static char st[SEQ_STATE_MAX];
    static const char *const HOW[6] = {"a part switch", "a load", "INIT", "seq_set_mview(JAM)", "seq_set_mview(PHRASE)", "a PLAY tap"};
    double ppq;
    for (int how = 0; how < 6; how++)
        for (int kind = 0; kind < 2; kind++) {
            static mjam_state_t after;
            const char *what = kind ? "editor" : "picker";
            int view = how == 3 ? MVIEW_JAM : how == 4 ? MVIEW_PHRASE : kind ? MVIEW_JAM : MVIEW_PHRASE, kept, undo;
            seq_init(&s, emit, NULL);
            seq_state_save(&s, st, sizeof st);
            if (kind) {
                seq_medit_open(&s, 1);
                seq_medit_select(&s, 3);
                seq_medit_set_oct(&s, 5);   /* a held step: the octave only */
                seq_medit_note(&s, 2);   /* D5 */
            } else {
                seq_set_mview(&s, MVIEW_PHRASE);
                seq_mpick_open(&s);
                seq_mpick_try(&s, MJ_RUN, 1);
            }
            switch (how) {
            case 0: seq_select_part(&s, 1); break;   /* stopped: at once */
            case 1: seq_state_load(&s, st); break;
            case 2: seq_init_preset(&s); break;
            case 3: seq_set_mview(&s, MVIEW_JAM); break;
            case 4: seq_set_mview(&s, MVIEW_PHRASE); break;
            default: seq_set_step(&s, LANE_MEL_UP, 1, 6, 0); break;   /* ARP UP's note on bar 2 step 7: a REST fix */
            }
            CHECK(seq_mview(&s) == view && seq_medit_bar(&s) == -1 && seq_follow_paused(&s, TAB_MEL) == 0,
                  "%s ends an open %s: the user's view (%s), FOLLOW running", HOW[how], what, view ? "PHRASE" : "JAM");
            kept = kind ? s.part[0].mjam.fix[16 + 3] == 74 : swap_is(&s, MJ_RUN, 1);
            if (how < 3)
                CHECK(!kept && (kind ? !s.part[0].mjam.fix[16 + 3] : swap_is(&s, MJ_RUN, 2)), "%s: the Verse as at open (cancelled)", HOW[how]);
            else
                CHECK(kept && (how < 5 || (s.part[0].mjam.fix[16 + 6] == MJ_FIX_REST && s.undo_n == kind + 1)), "%s keeps the %s's edit (DONE)%s",
                      HOW[how], what, how == 5 ? ", and the tap after it" : "");
            after = s.part[s.cur_part].mjam;
            undo = s.undo_n;
            seq_mpick_cancel(&s);
            seq_medit_cancel(&s);
            CHECK(!memcmp(&after, &s.part[s.cur_part].mjam, sizeof after) && s.undo_n == undo, "%s: a later CANCEL is inert", HOW[how]);
        }
    /* playing: a part queued before the editor opened takes over at the bar line; the stale editor reads closed, the next setter
     * cancels it */
    seq_init(&s, emit, NULL);
    ppq = play_to_k(&s, 0.0, 5);
    seq_select_part(&s, 1);   /* queued for bar 2 */
    seq_medit_open(&s, 1);
    seq_medit_select(&s, 3);
    seq_medit_note(&s, 2);
    ppq = play_to_k(&s, ppq, 17);
    CHECK(seq_part(&s) == 1 && seq_medit_bar(&s) == -1 && seq_mview(&s) == MVIEW_JAM && seq_follow_paused(&s, TAB_MEL) == 0,
          "playing: the queued Intro takes over at bar 2; the editor reads closed");
    seq_medit_cancel(&s);
    CHECK(!s.part[0].mjam.fix[16 + 3] && !s.txn.kind, "the next setter cancels it: the Verse's bar as at open");
    seq_block(&s, ppq, bpm, 0, 128);
    /* Task 6 (Task 5 review): the loop length and the chord length */
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    int undo = s.undo_n;
    seq_medit_open(&s, 11);
    seq_medit_note(&s, 2);
    int fixed = seq_mel_fixed_count(&s, 11);
    seq_set_bars(&s, 8);
    CHECK(fixed == 1 && seq_medit_bar(&s) == -1 && seq_mview(&s) == MVIEW_JAM && !s.part[0].mjam.fix[11 * SEQ_STEPS + 1] && s.undo_n == undo &&
          s.part[0].bars == 8, "the editor on bar 12 of 16, a note fixed, the loop to 8: cancelled (the fix gone, the undo depth back)");
    seq_medit_open(&s, 2);
    seq_medit_note(&s, 2);
    seq_set_bars(&s, 16);
    CHECK(seq_medit_bar(&s) == -1 && seq_mview(&s) == MVIEW_JAM && seq_mel_fixed_count(&s, 2) == 1 && s.undo_n == undo + 1 && s.part[0].bars == 16,
          "the editor on bar 3, a note fixed, the loop grows to 16: closed as DONE (the fix and its undo step kept)");
    seq_medit_open(&s, 3);
    seq_medit_note(&s, 2);
    seq_set_chord_len(&s, 32);
    CHECK(seq_medit_bar(&s) == -1 && seq_mview(&s) == MVIEW_JAM && seq_mel_fixed_count(&s, 3) == 1 && s.part[0].chord_len == 32,
          "the editor on bar 4, a note fixed, the chord length to 2 bars: closed as DONE (the fix kept)");
    for (int how = 0; how < 2; how++) {
        seq_set_mview(&s, MVIEW_PHRASE);
        seq_mpick_open(&s);
        seq_mpick_try(&s, MJ_RUN, how + 3);
        if (how) seq_set_chord_len(&s, 16);
        else seq_set_bars(&s, 4);
        CHECK(!s.txn.kind && seq_mview(&s) == MVIEW_PHRASE && swap_is(&s, MJ_RUN, how + 3), "the picker open, %s: closed as DONE (the SWAP block kept)",
              how ? "a chord length change" : "a loop shrink");
    }
}

/* cx_3 on a JAM lane is the playing block's level in 5 bands (0-19 level 1 ... 80-100 level 5), queued as a tap */
static void test_20_cx3_bands(void) {
    static seq_t s;
    static const int CX[5] = {0, 19, 20, 99, 100}, LV[5] = {0, 0, 1, 4, 4};
    double ppq;
    seq_init(&s, emit, NULL);
    seq_mel_queue(&s, MJ_HOOK, 2);   /* stopped: HOOK PENTA at once */
    CHECK(seq_complexity(&s, LANE_MEL_UP) == 50 && seq_mel_level(&s) == 2, "HOOK PENTA: cx_3 50, level 3");
    ppq = play_to_k(&s, 0.0, 5);
    for (int i = 0; i < 5; i++) {
        int c = -1, l = -1;
        seq_set_complexity(&s, LANE_MEL_UP, CX[i]);
        CHECK(seq_mel_queued(&s, &c, &l, NULL) && c == MJ_HOOK && l == LV[i] && seq_complexity(&s, LANE_MEL_UP) == CX[i] && seq_mel_level(&s) == LV[i] &&
              s.part[0].mjam.set.c == MJ_HOOK && s.part[0].mjam.set.l == 2 && queue_is_fresh(&s) && lane_is_fresh(&s),
              "cx_3 %d queues HOOK level %d; HOOK PENTA plays on, a JAM render", CX[i], LV[i] + 1);
    }
    seq_set_complexity(&s, LANE_MEL_UP, 50);
    CHECK(!seq_mel_queued(&s, NULL, NULL, NULL) && lane_is_fresh(&s) && s.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM,
          "cx_3 50 (the playing level): the queue cancelled; the lane still JAM");
    seq_block(&s, ppq, bpm, 0, 128);
    seq_set_complexity(&s, LANE_MEL_UP, 85);
    CHECK(s.part[0].mjam.set.c == MJ_HOOK && s.part[0].mjam.set.l == 4 && seq_complexity(&s, LANE_MEL_UP) == 90 && !seq_mel_queued(&s, NULL, NULL, NULL) &&
          lane_is_fresh(&s) && !s.undo_n, "stopped: cx_3 85 switches to HOOK level 5 at once (cx_3 90), no undo step");
}

/* The old RECIPE setters leave a JAM lane alone; a queue reads empty once the lane leaves JAM */
static void test_20_jam_guards(void) {
    static seq_t s;
    static seq_part_t was;
    double ppq;
    seq_init(&s, emit, NULL);
    was = s.part[0];
    seq_mel_set_type(&s, MEL_ARP);
    seq_mel_set_opt(&s, 0, 1);
    seq_mel_set_phrase(&s, 1);
    seq_mel_set_phrasing(&s, 0, 1);
    seq_mel_reset_phrasing(&s);
    seq_mel_set_flags(&s, 1, 1, 3, 1);
    seq_mel_set_knob(&s, MK_RANGE, 80);
    seq_mel_reset_knobs(&s);
    CHECK(!memcmp(&was, &s.part[0], sizeof was) && !s.undo_n, "the old RECIPE setters on a JAM lane: no-ops (no conversion, no undo step)");
    seq_mel_set_knob(&s, MK_GATE, 80);   /* mgat: the JAM lane's GATE */
    CHECK(s.part[0].mel.kn[MK_GATE] == 80 && seq_mel_knob(&s, MK_GATE) == 80 && seq_part_mel_knob(&s, 0, MK_GATE) == 80 &&
          s.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && !s.undo_n, "GATE on a JAM lane: its MK_GATE (80), read back, no undo step");
    seq_mel_set_knob(&s, MK_GATE, 101);
    CHECK(seq_mel_knob(&s, MK_GATE) == 80, "a GATE past 100 is ignored");
    ppq = play_to_k(&s, 0.0, 5);
    seq_mel_queue(&s, MJ_RUN, 0);
    CHECK(seq_mel_queued(&s, NULL, NULL, NULL), "RUN STEP-IN queued");
    old_mel(&s);
    CHECK(!seq_mel_queued(&s, NULL, NULL, NULL), "the lane leaves JAM: nothing reads queued");
    seq_block(&s, ppq, bpm, 0, 128);
}

static void test_19_bar_on_phrase(void) {   /* 0.24.1 the retired editor's EDIT THIS BAR'S RHYTHM left the code: PHRASE's bar */
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    seq_set_bview(&s, BVIEW_RHYTHM);
    seq_rbar_open(&s, LANE_BASS, 3);
    seq_rbar_note(&s, 7);   /* step 1 (D2, octave 2): G2 */
    CHECK(seq_bview(&s) == BVIEW_BAR && seq_bmode(&s) == 1 && seq_rbar_bar(&s, LANE_BASS) == 3 &&
          s.part[0].bass.fix[3 * 16] == 43, "PHRASE (RHYTHM): bar 4 open, the fix kept");
    CHECK(seq_follow_paused(&s, TAB_BASS) == 1 && seq_strip_by_bars(&s, TAB_BASS) && seq_strip_page(&s, TAB_BASS) == 0, "FOLLOW paused, the strip on bar 4's page");
    seq_rbar_done(&s, LANE_BASS);
    CHECK(seq_bview(&s) == BVIEW_RHYTHM && seq_bmode(&s) == 1, "DONE: the RHYTHM page");
    seq_set_bview(&s, BVIEW_BASS);
    seq_rbar_open(&s, LANE_BASS, 5);
    CHECK(seq_strip_page(&s, TAB_BASS) == 1 && seq_strip_pages(&s, TAB_BASS) == 2, "the BASS page's strip pages by bars: bar 6's page is 2 of 2");
    seq_rbar_done(&s, LANE_BASS);
    seq_set_bview(&s, BVIEW_EDIT);
    seq_set_bview(&s, BVIEW_BAR);
    CHECK(seq_bview(&s) == BVIEW_BASS, "seq_set_bview takes BASS and RHYTHM only");
}

/* Review Focus 3, part 2: a part switch, a state load, INIT and seq_set_bview end every open Bass transaction; a later cancel is
 * inert */
static void bass_txn_open(seq_t *s, int kind) {
    switch (kind) {
    case 0: seq_rbar_open(s, LANE_BASS, 1); seq_rbar_note(s, 2); break;                    /* the bar editor: step 1 fixed at D2 */
    case 1: seq_bstyle_open(s); seq_bstyle_try(s, 9); break;                       /* the STYLE browser */
    case 2: seq_rbar_open(s, LANE_BASS, 1); seq_rbar_paint(s, LANE_BASS, 4, RC_HIT); break;   /* a RHYTHM bar */
    default: seq_rbrowse_open(s, LANE_BASS); seq_rbrowse_try(s, LANE_BASS, 3); break;   /* the RHYTHMS browser */
    }
}

static void bass_txn_cancel(seq_t *s, int kind) {
    switch (kind) {
    case 0: seq_rbar_cancel(s, LANE_BASS); break;
    case 1: seq_bstyle_cancel(s); break;
    case 2: seq_rbar_cancel(s, LANE_BASS); break;
    default: seq_browse_cancel(s); break;
    }
}

static int bass_txn_kept(const seq_t *s, int kind) {
    const seq_part_t *p = &s->part[s->cur_part];
    switch (kind) {
    case 0: return p->bass.fix[16] == 38;
    case 1: return p->bass.set.style == 9;
    case 2: return p->rb[DL_BASS][1].c[4] == RC_HIT;
    default: return p->rb[DL_BASS][0].from == 3;
    }
}

static void test_19_bass_txn_closed_by_switch(void) {
    static seq_t s;
    static seq_part_t parts[SEQ_PARTS];
    static const char *const BY[4] = {"a part switch", "a state load", "INIT", "seq_set_bview(BASS)"};
    static const char *const WHAT[4] = {"bar editor", "STYLE browser", "RHYTHM bar", "RHYTHMS browser"};
    static char state[SEQ_STATE_MAX];
    for (int by = 0; by < 4; by++)
        for (int kind = 0; kind < 4; kind++) {
            int n, closed, kept;
            seq_init(&s, emit, NULL);
            seq_state_save(&s, state, sizeof state);
            bass_txn_open(&s, kind);
            CHECK(seq_bview(&s) != BVIEW_BASS && seq_follow_paused(&s, TAB_BASS) == 1 && bass_txn_kept(&s, kind), "the %s is open", WHAT[kind]);
            if (by == 0) seq_select_part(&s, 1);
            else if (by == 1) seq_state_load(&s, state);
            else if (by == 2) seq_init_preset(&s);
            else seq_set_bview(&s, BVIEW_BASS);
            closed = !s.txn.kind && seq_bview(&s) == BVIEW_BASS && seq_follow_paused(&s, TAB_BASS) == 0;
            kept = bass_txn_kept(&s, kind);
            memcpy(parts, s.part, sizeof parts);
            n = s.undo_n;
            bass_txn_cancel(&s, kind);
            CHECK(closed && kept == (by == 3) && !memcmp(parts, s.part, sizeof parts) && s.undo_n == n,
                  "%s ends the %s (%s); a later CANCEL is inert", BY[by], WHAT[kind], by == 3 ? "DONE" : "cancelled");
        }
}

/* Review Focus 1: a key and a progression change with the editor open on bar 3 and bar 2 note-locked */
static void test_19_harmony_rerender(void) {
    static seq_t s;
    const seq_part_t *p = &s.part[0];
    unsigned char note[16];
    char role[16];
    int thirds = 0, ok = 1;
    seq_init(&s, emit, NULL);   /* E minor: Em C G D */
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths */
    seq_set_bass_style(&s, 36);           /* Funk Thirds */
    bass_nlock(&s, 1);
    seq_set_bass_style(&s, BASS_OCTAVE_BOUNCE);
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_note(&s, 9);   /* step 1: A1 */
    seq_set_key(&s, 2);      /* D minor: Dm Bb F C */
    seq_set_slot(&s, 1, 3, 0, COL_TRIAD);   /* slot 2: Gm */
    CHECK(bass_all_ref(&s), "every bar re-renders against the new chords");
    seq_bass_line(&s, 1, note, role);
    for (int i = 0; i < 16; i++) thirds += role[i] == '3';
    CHECK(note[0] % 12 == 7 && thirds > 0 && !memcmp(&p->bass.frozen[1], &(bass_set_t){36, 0, 0, {3, 1, 0, 0}}, sizeof p->bass.frozen[1]),
          "locked bar 2 keeps Funk Thirds (3rds) over its new root G");
    seq_bass_line(&s, 0, note, role);
    for (int i = 0; i < 16; i++) ok &= role[i] == '.' || role[i] == '1' || role[i] == 'o';
    CHECK(ok && note[0] % 12 == 2, "bar 1 plays Octave Bounce from D");
    CHECK(seq_rbar_bar(&s, LANE_BASS) == 2 && seq_bview(&s) == BVIEW_BAR && p->bass.fix[2 * 16] == 33, "the editor (0.24 PHRASE's bar) is still on bar 3, its fix (A1) kept");
    seq_rbar_done(&s, LANE_BASS);
    CHECK(seq_rbar_bar(&s, LANE_BASS) == -1 && seq_bview(&s) == BVIEW_BASS && p->bass.fix[2 * 16] == 33, "DONE works");
}

static void test_19_reset_bass_knobs(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    bass_set_t walk = bass_style_set(29);   /* Upright Walk */
    int n;
    seq_init(&s, emit, NULL);
    n = s.undo_n;
    seq_reset_bass_knobs(&s);
    CHECK(s.undo_n == n, "fresh: RESET KNOBS is a no-op");
    seq_set_bass_style(&s, 29);
    seq_set_bknob(&s, BK_MOV, 1);
    seq_set_bknob(&s, BK_REG, 2);
    seq_set_bass_pedal(&s, 2);
    seq_set_bass_octave(&s, 1);
    seq_set_complexity(&s, LANE_BASS, 20);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_PUSH, 30);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, 70);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT, 80);
    seq_set_rknob(&s, LANE_BASS, RK_FILL, 2);
    n = s.undo_n;
    seq_reset_bass_knobs(&s);
    CHECK(bset_is(&s, walk) && seq_complexity(&s, LANE_BASS) == 50 && seq_lane_shape(&s, LANE_BASS, BSHAPE_PUSH) == 0 &&
          seq_lane_shape(&s, LANE_BASS, BSHAPE_GATE) == 50 && seq_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT) == 50 && s.undo_n == n + 1 &&
          seq_rknob(&s, LANE_BASS, RK_FILL) == 2 && bass_all_ref(&s),
          "BASS's RESET KNOBS: knobs 2-5, PEDAL and OCTAVE the style's, COMPLEXITY 50, PUSH 0, GATE 50, DYNAMICS 50; FILL stays; one undo step");
    seq_reset_bass_knobs(&s);
    CHECK(s.undo_n == n + 1, "again: nothing");
    seq_undo(&s);
    CHECK(bset_is(&s, (bass_set_t){29, 2, 1, {1, 0, 1, 2}}) && seq_complexity(&s, LANE_BASS) == 20 &&
          seq_lane_shape(&s, LANE_BASS, BSHAPE_PUSH) == 30 && seq_lane_shape(&s, LANE_BASS, BSHAPE_GATE) == 70 &&
          seq_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT) == 80 && s.undo_n == n && bass_all_ref(&s), "one UNDO puts every one back");
    p->bass.set = (bass_set_t){-1, 1, -1, {2, 3, 4, -1}};   /* Custom */
    seq_reset_bass_knobs(&s);
    CHECK(bset_is(&s, (bass_set_t){-1, 0, 0, {0, 0, 0, 0}}), "Custom: knobs 2-5, PEDAL and OCTAVE to zero");
    seq_set_bass_style(&s, 29);
    seq_set_bknob(&s, BK_MOV, 1);
    seq_set_rknob(&s, LANE_BASS, RK_SYNC, 2);
    n = s.undo_n;
    seq_reset_rhythm_knobs(&s, LANE_BASS);
    CHECK(!seq_rknob(&s, LANE_BASS, RK_SYNC) && !seq_rknob(&s, LANE_BASS, RK_FILL) && seq_complexity(&s, LANE_BASS) == 50 &&
          seq_bknob(&s, BK_MOV) == 1 && s.undo_n == n + 1, "RHYTHM's RESET KNOBS: SYNC..FILL 0 and the shared knobs; the note knobs stay; one undo");
    seq_undo(&s);
    CHECK(seq_rknob(&s, LANE_BASS, RK_SYNC) == 2 && seq_rknob(&s, LANE_BASS, RK_FILL) == 2 && seq_bknob(&s, BK_MOV) == 1, "and its UNDO");
}

/* The 0.18 RHYTHM editing rules on the Bass lane; the Chords bars never move */
static void test_19_bass_rhythm_rules(void) {
    static seq_t s;
    static rbar_t chords[SEQ_MAX_BARS], at[SEQ_MAX_BARS];
    seq_part_t *p = &s.part[0];
    int ed = 0, n, r, ok = 1, rep = -1, skip = -1;
    seq_init(&s, emit, NULL);   /* Bass Root: A--------------- */
    memcpy(chords, p->rb[DL_CHORDS], sizeof chords);
    seq_set_bview(&s, BVIEW_RHYTHM);
    CHECK(seq_bview(&s) == BVIEW_RHYTHM && seq_bmode(&s) == 1 && seq_cview(&s) == CVIEW_CHORDS, "the Bass tab on RHYTHM, CHORDS untouched");
    r = seq_rbar_paint(&s, LANE_BASS, 0, RC_REST);
    CHECK(!r && bbar_reads(&s, 1, "A---------------"), "no bar open: a paint does nothing");
    seq_rbar_open(&s, LANE_BASS, 1);
    CHECK(seq_rbar_bar(&s, LANE_BASS) == 1 && seq_bview(&s) == BVIEW_BAR && seq_cview(&s) == CVIEW_CHORDS, "bar 2 open: BVIEW_BAR");
    n = s.undo_n;
    r = seq_rbar_paint(&s, LANE_BASS, 0, RC_REST);
    CHECK(r == 1 && bbar_reads(&s, 1, "................") && s.undo_n == n + 1 && seq_bar_rhythm(&s, LANE_BASS, 1, &ed) == 0 && ed &&
          !p->lane[LANE_BASS].steps[16].on, "REST on the hit clears it and its holds: one undo step, edited, silent");
    r = seq_rbar_paint(&s, LANE_BASS, 0, RC_HOLD) + seq_rbar_paint(&s, LANE_BASS, 5, RC_HOLD);
    CHECK(!r && s.undo_n == n + 1, "HOLD at step 1 and after a rest is refused");
    seq_rbar_paint(&s, LANE_BASS, 4, RC_HIT);
    seq_rbar_paint(&s, LANE_BASS, 5, RC_HOLD);
    seq_rbar_paint(&s, LANE_BASS, 8, RC_ACC);
    seq_rbar_paint(&s, LANE_BASS, 12, RC_GHOST);
    CHECK(bbar_reads(&s, 1, "....x-..A...g...") && s.undo_n == n + 5 && p->lane[LANE_BASS].steps[20].on &&
          p->lane[LANE_BASS].steps[20].len == 2 && p->lane[LANE_BASS].steps[20].abs == 36 && bass_bar_ref(&s, 1),
          "HIT, HOLD, ACCENT, GHOST: one undo each; step 5 plays C2 for 2 steps");
    seq_undo(&s);
    CHECK(seq_rbar_bar(&s, LANE_BASS) == 1 && bbar_reads(&s, 1, "....x-..A.......") && s.undo_n == n + 4, "UNDO steps back one paint, the bar open");
    seq_rbar_done(&s, LANE_BASS);
    CHECK(seq_bview(&s) == BVIEW_RHYTHM && bbar_reads(&s, 1, "....x-..A......."), "DONE keeps the paints: back on RHYTHM");
    /* COPY TO ALL with a locked bar, COPY TO..., and CANCEL putting copies back */
    seq_set_bars(&s, 16);
    seq_rbar_open(&s, LANE_BASS, 7);
    seq_rbar_lock(&s, LANE_BASS);
    seq_rbar_done(&s, LANE_BASS);
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_paint(&s, LANE_BASS, 8, RC_ACC);   /* bar 3: A-------A------- */
    n = seq_rbar_copy_counts(&s, LANE_BASS, 0xFFFF, &rep, &skip);
    CHECK(n == 14 && rep == 14 && skip == 1, "COPY TO ALL from bar 3: 14 bars, all different, 1 locked skipped (%d %d %d)", n, rep, skip);
    memcpy(at, p->rb[DL_BASS], sizeof at);
    n = s.undo_n;
    seq_rbar_copy(&s, LANE_BASS, 0xFFFF);
    for (int b = 0; b < 16; b++) ok &= b == 7 ? seq_bar_locked(&s, LANE_BASS, 7) && bbar_reads(&s, 7, "A---------------")
                                              : bbar_reads(&s, b, "A-------A-------") && bass_bar_ref(&s, b);
    CHECK(ok && s.undo_n == n + 1 && seq_rbar_bar(&s, LANE_BASS) == 2, "every unlocked bar is bar 3's, locked bar 8 kept; one undo step");
    seq_undo(&s);
    CHECK(!memcmp(at, p->rb[DL_BASS], sizeof at) && s.undo_n == n, "one UNDO puts every copied bar back");
    seq_rcopy_open(&s, LANE_BASS);
    CHECK(seq_bview(&s) == BVIEW_COPY && seq_bmode(&s) == 1, "COPY TO...: BVIEW_COPY");
    seq_rcopy_toggle(&s, LANE_BASS, 10);
    seq_rcopy_go(&s, LANE_BASS);
    CHECK(seq_bview(&s) == BVIEW_BAR && bbar_reads(&s, 10, "A-------A-------") && bbar_reads(&s, 11, "A---------------"), "COPY TO 1 BAR, back in the bar");
    seq_rbar_cancel(&s, LANE_BASS);
    CHECK(seq_bview(&s) == BVIEW_RHYTHM && bbar_reads(&s, 2, "A---------------") && bbar_reads(&s, 10, "A---------------") &&
          s.undo_n == n - 1, "CANCEL puts the copies and the paint back, and the undo depth");
    /* picks */
    seq_rbar_open(&s, LANE_BASS, 3);
    r = seq_rhythm_pick(&s, LANE_BASS, 5);
    CHECK(r == 1 && bbar_is(&s, 3, 5) && bbar_is(&s, 4, 0) && bass_bar_ref(&s, 3), "a bar open: a pick fills that bar only (Rolling)");
    seq_rbar_done(&s, LANE_BASS);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    ok = 1;
    for (int b = 0; b < 16; b++) ok &= b == 7 ? bbar_is(&s, 7, 0) : bbar_is(&s, b, 3);
    CHECK(ok && bass_all_ref(&s), "none open: every unlocked bar (Eighths)");
    /* the RHYTHMS browser */
    n = s.undo_n;
    memcpy(at, p->rb[DL_BASS], sizeof at);
    seq_rbrowse_open(&s, LANE_BASS);
    CHECK(seq_bview(&s) == BVIEW_RBROWSE && seq_follow_paused(&s, TAB_BASS) == 1 && s.undo_n == n + 1 && seq_rbrowse_group(&s, LANE_BASS) == 0,
          "RHYTHMS opens: BVIEW_RBROWSE, FOLLOW paused, one undo step, the chip Basic");
    seq_rbrowse_try(&s, LANE_BASS, 12);
    CHECK(bbar_is(&s, 0, 12) && bbar_is(&s, 7, 0) && s.undo_n == n + 1, "a try fills every unlocked bar (Funk), no undo step");
    seq_browse_cancel(&s);
    CHECK(!memcmp(at, p->rb[DL_BASS], sizeof at) && s.undo_n == n && seq_bview(&s) == BVIEW_RHYTHM, "CANCEL: as before");
    seq_rbrowse_open(&s, LANE_BASS);
    seq_rbrowse_set_group(&s, LANE_BASS, 1);
    seq_browse_done(&s);
    CHECK(bbar_is(&s, 0, 4) && bbar_is(&s, 15, 4) && s.undo_n == n + 1 && seq_rbrowse_group(&s, LANE_BASS) == 1,
          "a chip applies its feel's first rhythm (Offbeat); DONE keeps it, one undo step");
    /* FILL and COMPLEXITY */
    seq_set_rknob(&s, LANE_BASS, RK_FILL, 2);
    CHECK(memcmp(p->rfill[DL_BASS][3], p->rb[DL_BASS][3].c, 16) && !memcmp(p->rfill[DL_BASS][7], p->rb[DL_BASS][7].c, 16),
          "FILL: a bar's fill variant differs, a locked bar's is as drawn");
    seq_rhythm_pick(&s, LANE_BASS, 3);
    seq_set_complexity(&s, LANE_BASS, 0);
    ok = 1;
    for (int b = 0; b < 16; b++) ok &= seq_bar_hits(&s, LANE_BASS, b) == 1;
    CHECK(ok && bass_all_ref(&s), "COMPLEXITY 0: one hit a bar");
    CHECK(!memcmp(chords, p->rb[DL_CHORDS], sizeof chords), "the Chords bars never moved");
}

/* Task 5 review: knob turns are never undone or cancelled. PEDAL and OCTAVE's UNDO put back only themselves; the STYLE browser's
 * CANCEL (and the UNDO of its kept step) keep a knob turned while it was open */
static void test_19_knob_turns_kept(void) {
    static seq_t s;
    bass_set_t b0, want;
    seq_init(&s, emit, NULL);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    seq_set_bass_pedal(&s, 1);
    seq_set_bknob(&s, BK_MOV, 3);
    seq_set_bass_octave(&s, 1);
    seq_set_bknob(&s, BK_OCTS, 2);
    seq_undo(&s);
    CHECK(s.part[0].bass.set.pedal == 1 && !s.part[0].bass.set.octave && seq_bknob(&s, BK_MOV) == 3 && seq_bknob(&s, BK_OCTS) == 2 &&
          bass_all_ref(&s), "OCTAVE's UNDO puts back the octave only: MOVEMENT and OCTAVES turned since stay");
    seq_undo(&s);
    CHECK(!s.part[0].bass.set.pedal && seq_bknob(&s, BK_MOV) == 3 && seq_bknob(&s, BK_OCTS) == 2 && bass_all_ref(&s),
          "PEDAL's UNDO puts back PEDAL only");
    seq_set_bass_style(&s, BASS_POP_ROOT);
    b0 = s.part[0].bass.set;
    seq_bstyle_open(&s);
    seq_bstyle_try(&s, 36);         /* Funk Thirds: MOVEMENT 3, OCTAVES 1 */
    seq_set_bknob(&s, BK_APP, 2);   /* turned while open */
    seq_bstyle_cancel(&s);
    want = b0;
    want.k[BK_APP] = 2;
    CHECK(bset_is(&s, want) && bass_all_ref(&s), "STYLE's CANCEL: the settings as at open, APPROACH as turned");
    seq_bstyle_open(&s);
    seq_set_bknob(&s, BK_REG, -1);
    seq_bstyle_try(&s, 36);         /* the try sets REGISTER: the style's, not a turn */
    seq_set_bknob(&s, BK_MOV, 1);
    seq_bstyle_cancel(&s);
    want.k[BK_MOV] = 1;
    CHECK(bset_is(&s, want), "a try after a turn takes that knob back (the style set it); a turn after it stays");
    int n = s.undo_n;
    seq_bstyle_open(&s);
    seq_set_bknob(&s, BK_OCTS, 3);
    seq_bstyle_done(&s);
    CHECK(s.undo_n == n, "only a knob turned in the browser: DONE leaves no undo step");
    seq_bstyle_open(&s);
    seq_bstyle_try(&s, 9);          /* Pedal Drone */
    seq_set_bknob(&s, BK_APP, 4);
    seq_bstyle_done(&s);
    want = s.part[0].bass.set;
    seq_undo(&s);
    CHECK(s.part[0].bass.set.style == BASS_POP_ROOT && !s.part[0].bass.set.pedal && seq_bknob(&s, BK_APP) == 4 && seq_bknob(&s, BK_OCTS) == 3 &&
          seq_bknob(&s, BK_REG) == 0 && bass_all_ref(&s), "the UNDO of a kept STYLE step keeps the knob turned in the browser");
}

/* 0.19 Task 6: q's DRAWN Bass lane is p's: bars, knobs, note settings, note locks, fixed notes, variations and rendered steps */
static int bass_same(const seq_part_t *p, const seq_part_t *q) {
    int bars = p->bars;
    return q->lane[LANE_BASS].gen.style == GEN_BASS_DRAWN && q->lane[LANE_BASS].gen.complexity == p->lane[LANE_BASS].gen.complexity &&
           !memcmp(q->rb[DL_BASS], p->rb[DL_BASS], (size_t)bars * sizeof p->rb[DL_BASS][0]) &&
           !memcmp(q->rk[DL_BASS], p->rk[DL_BASS], sizeof p->rk[DL_BASS]) && !memcmp(&q->bass.set, &p->bass.set, sizeof p->bass.set) &&
           q->bass.nlock == p->bass.nlock && !memcmp(q->bass.frozen, p->bass.frozen, sizeof p->bass.frozen) && !memcmp(q->bass.fix, p->bass.fix, sizeof p->bass.fix) &&
           !memcmp(q->lane[LANE_BASS].bar_var, p->lane[LANE_BASS].bar_var, sizeof p->lane[LANE_BASS].bar_var) &&
           !memcmp(q->lane[LANE_BASS].steps, p->lane[LANE_BASS].steps, (size_t)bars * SEQ_STEPS * sizeof(seq_step_t)) &&
           !memcmp(q->rfill[DL_BASS], p->rfill[DL_BASS], (size_t)bars * sizeof p->rfill[DL_BASS][0]);
}

/* edited, locked and library Bass bars, the RHYTHM knobs, note settings, two note-locked bars, fixed notes (one silent) and a
 * variation save as `2.rb/2.rz/2.bx/2.bl/2.ab/2.bv` with no Bass step records, and load back the same */
static void test_19_save_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    seq_part_t *p = &s.part[0];
    int rest = -1;
    seq_init(&s, emit, NULL);
    seq_rhythm_pick(&s, LANE_BASS, 3);                   /* every bar Eighths (3) */
    seq_rbar_open(&s, LANE_BASS, 1);
    seq_rbar_paint(&s, LANE_BASS, 5, RC_HIT);            /* bar 2 edited from Eighths */
    seq_rbar_done(&s, LANE_BASS);
    rhythm_parse("A.g-x..-Axg.x-..", p->rb[DL_BASS][2].c);   /* bar 3 drawn from nothing */
    p->rb[DL_BASS][2].from = -1, p->rb[DL_BASS][2].ed = 1;
    rhythm_lib_codes(RLIB_BASS, 12, p->rb[DL_BASS][3].c);    /* bar 4 Funk, locked: plays as drawn */
    p->rb[DL_BASS][3].from = 12, p->rb[DL_BASS][3].lock = 1;
    for (int i = 0; i < 16 && rest < 0; i++)
        if (p->rb[DL_BASS][3].c[i] == RC_REST) rest = i;
    seq_set_complexity(&s, LANE_BASS, 62);
    seq_set_rknob(&s, LANE_BASS, RK_SYNC, 2), seq_set_rknob(&s, LANE_BASS, RK_GHOSTS, 1);
    seq_set_rknob(&s, LANE_BASS, RK_HUM, 3), seq_set_rknob(&s, LANE_BASS, RK_FILL, 2);
    seq_set_bass_style(&s, 36);   /* Funk Thirds */
    seq_set_bknob(&s, BK_REG, -1);
    seq_set_bass_octave(&s, -1);
    p->bass.nlock = 1 << 0 | 1 << 2;
    p->bass.frozen[0] = bass_style_set(BASS_OCTAVE_BOUNCE);
    p->bass.frozen[2] = (bass_set_t){-1, 2, 1, {4, 3, 2, -2}};   /* Custom */
    p->bass.fix[1] = 33, p->bass.fix[SEQ_STEPS + 4] = 47, p->bass.fix[3 * SEQ_STEPS + rest] = 45;
    p->lane[LANE_BASS].bar_var[1] = 5;
    render_now(&s);
    CHECK(rest >= 0 && !p->lane[LANE_BASS].steps[3 * SEQ_STEPS + rest].on && p->lane[LANE_BASS].steps[SEQ_STEPS + 4].abs == 47,
          "set up: bar 4 step %d is silent, its fix kept; bar 2 step 5 plays its fixed B2", rest + 1);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "|A2=0:|A2.g=18,1,62|") && !strstr(buf, "A2.lk") && strstr(buf, "|A2.rz=2,1,3,2"),
          "a DRAWN Bass saves 2.g=18,seed,cx and no step records; its knobs in 2.rz");
    CHECK(strstr(buf, "|A2.rb=3,3") && strstr(buf, "|A2.bx=36,0,-1,3,1,0,-1") && strstr(buf, "|A2.bl=0:1.0.0.0.2.0.0;2:-1.2.1.4.3.2.-2") &&
          strstr(buf, "|A2.bv=") && strstr(buf, "|A2.ab="), "2.rb, 2.bx (style,pedal,octave,mov,octs,app,reg), 2.bl, 2.bv and 2.ab are there");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf), "and it loads");
    CHECK(bass_same(p, &t.part[0]), "bars, knobs, settings, note locks, fixes (the silent one too), variation and steps come back");
    CHECK(seq_state_save(&t, again, sizeof again) == n && !strcmp(buf, again), "it saves back byte for byte");
    p->bass.set = bass_style_set(BASS_POP_ROOT);
    p->bass.nlock = 0;
    memset(p->bass.fix, 0, sizeof p->bass.fix);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, "2.bx=") && !strstr(buf, "2.bl=") && !strstr(buf, "2.ab="), "Pop Root, no note lock, no fix: no 2.bx, 2.bl or 2.ab");
}

/* Final review: seeded random edit sequences (both tabs' transactions, knobs, styles, the bar editor, the RHYTHM bars, parts, undo;
 * no IDEAS try, bars inside the loop), then a save and a load: every saved, current part's Bass steps, notes, roles, fill notes and
 * settings and its CHORDS steps, bars and progression come back; and a re-render of the playing part (a key change and back)
 * changes nothing */
enum { RT_WHY = 96 };   /* what differed */
static unsigned rt_seed;
static int rt_rnd(int n) { rt_seed = rt_seed * 1103515245u + 12345u; return (int)((rt_seed >> 16) % (unsigned)n); }

static int rt_same(const seq_part_t *a, const seq_part_t *b, char *why) {
    for (int i = 0; i < a->bars * SEQ_STEPS; i++) {
        const seq_step_t *x = &a->lane[LANE_BASS].steps[i], *y = &b->lane[LANE_BASS].steps[i];
        const seq_step_t *u = &a->lane[LANE_CHORDS].steps[i], *v = &b->lane[LANE_CHORDS].steps[i];
        if (x->on != y->on || (x->on && (x->abs != y->abs || x->len != y->len || x->vel != y->vel)) || a->brole[i] != b->brole[i])
            return snprintf(why, RT_WHY, "Bass step %d: on %d/%d abs %d/%d role %c/%c", i, x->on, y->on, x->abs, y->abs, a->brole[i],
                            b->brole[i]), 0;
        if (u->on != v->on || (u->on && (u->len != v->len || u->vel != v->vel))) return snprintf(why, RT_WHY, "Chords step %d", i), 0;
    }
    for (int bar = 0; bar < a->bars; bar++)
        for (int i = 0; i < SEQ_STEPS; i++)
            if (a->rfill[DL_BASS][bar][i] != b->rfill[DL_BASS][bar][i] ||
                (rhythm_is_hit(a->rfill[DL_BASS][bar][i]) && a->rfill_abs[DL_BASS][bar][i] != b->rfill_abs[DL_BASS][bar][i]))
                return snprintf(why, RT_WHY, "Bass fill bar %d step %d", bar, i), 0;
    if (memcmp(a->rb[DL_CHORDS], b->rb[DL_CHORDS], sizeof(rbar_t) * (size_t)a->bars)) return snprintf(why, RT_WHY, "Chords bars"), 0;
    int slots = a->bars * SEQ_STEPS / a->chord_len;   /* the loop's slots (a 2-bar chord in a 1-bar loop: 1) */
    if (memcmp(a->prog, b->prog, sizeof a->prog[0] * (size_t)(slots > 1 ? slots : 1))) return snprintf(why, RT_WHY, "progression"), 0;
    if (memcmp(&a->bass.set, &b->bass.set, sizeof a->bass.set) || ((a->bass.nlock ^ b->bass.nlock) & ((1u << a->bars) - 1)))
        return snprintf(why, RT_WHY, "Bass settings or note locks"), 0;
    return 1;
}

static void rt_op(seq_t *s) {
    int bars = seq_bars(s);
    switch (rt_rnd(45)) {
    case 0: seq_set_key(s, rt_rnd(12)); break;
    case 1: seq_set_scale(s, rt_rnd(7)); break;
    case 2: seq_set_slot(s, rt_rnd(seq_slots(s)), rt_rnd(7), 0, 0); break;
    case 3: seq_set_chord_len(s, (int[]){4, 8, 16, 32}[rt_rnd(4)]); break;
    case 4: seq_set_bars(s, (int[]){1, 2, 4, 8, 16}[rt_rnd(5)]); break;
    case 5: seq_set_bknob(s, rt_rnd(4), rt_rnd(7) - 2); break;
    case 6: seq_set_bass_pedal(s, rt_rnd(3)); break;
    case 7: seq_set_bass_octave(s, rt_rnd(3) - 1); break;
    case 8: seq_set_bass_style(s, rt_rnd(BASS_STYLE_COUNT)); break;
    case 9: seq_rbar_open(s, LANE_BASS, rt_rnd(bars)); break;
    case 10: seq_rbar_select(s, LANE_BASS, rt_rnd(16)); seq_rbar_note(s, rt_rnd(12)); break;
    case 11: seq_rbar_new_bar(s, LANE_BASS); break;
    case 12: seq_rbar_lock(s, LANE_BASS); break;
    case 13: seq_rbar_reset_bar(s, LANE_BASS); break;
    case 14: if (rt_rnd(2)) seq_rbar_cancel(s, LANE_BASS); else seq_rbar_done(s, LANE_BASS); break;
    case 15: seq_rbar_open(s, LANE_BASS, rt_rnd(bars)); break;
    case 16: seq_rbar_paint(s, LANE_BASS, rt_rnd(16), rt_rnd(5)); break;
    case 17: seq_rbar_lock(s, LANE_BASS); break;
    case 18: seq_rbar_copy(s, LANE_BASS, (unsigned)rt_rnd(65536) & ((1u << bars) - 1)); break;
    case 19: if (rt_rnd(2)) seq_rbar_cancel(s, LANE_BASS); else seq_rbar_done(s, LANE_BASS); break;
    case 20: seq_set_rknob(s, LANE_BASS, rt_rnd(4), rt_rnd(7) - 2); break;
    case 21: seq_set_complexity(s, LANE_BASS, rt_rnd(101)); break;
    case 22: seq_undo(s); break;
    case 23: seq_select_part(s, rt_rnd(SEQ_NPARTS)); break;
    case 24: seq_set_prog_preset(s, rt_rnd(16)); break;
    case 25: seq_bstyle_open(s); break;
    case 26: seq_bstyle_try(s, rt_rnd(BASS_STYLE_COUNT)); break;
    case 27: if (rt_rnd(2)) seq_bstyle_cancel(s); else seq_bstyle_done(s); break;
    case 28: seq_set_step(s, LANE_BASS, rt_rnd(bars), rt_rnd(16), rt_rnd(2)); break;
    case 29: seq_rhythm_pick(s, LANE_BASS, rt_rnd(RHYTHM_COUNT)); break;
    case 30: seq_rbrowse_open(s, LANE_BASS); break;
    case 31: seq_rbrowse_try(s, LANE_BASS, rt_rnd(RHYTHM_COUNT)); break;
    case 32: seq_reset_bass_knobs(s); break;
    case 33: seq_reset_rhythm_knobs(s, LANE_BASS); break;
    case 34: seq_rbar_note_oct(s, 1 + rt_rnd(4)); break;
    case 35: seq_rbar_select(s, LANE_CHORDS, rt_rnd(16)); break;   /* 0.24.1 PHRASE's CHORDS picks (the FIX picker is gone) */
    case 36: seq_rbar_chord(s, rt_rnd(7), 0); break;
    case 37: seq_rbar_type(s, rt_rnd(5)); break;
    case 38: seq_rbar_open(s, LANE_CHORDS, rt_rnd(bars)); break;
    case 39: seq_rbar_paint(s, LANE_CHORDS, rt_rnd(16), rt_rnd(5)); break;
    case 40: if (rt_rnd(2)) seq_rbar_cancel(s, LANE_CHORDS); else seq_rbar_done(s, LANE_CHORDS); break;
    case 41: seq_browse_open(s, rt_rnd(2) ? CVIEW_PROG : CVIEW_VOICE); break;
    case 42: seq_prog_try(s, rt_rnd(16)); seq_voice_try(s, rt_rnd(40)); break;
    case 43: if (rt_rnd(2)) seq_browse_cancel(s); else seq_browse_done(s); break;
    case 44:
        seq_set_cview(s, (int[]){CVIEW_CHORDS, CVIEW_JAM, CVIEW_RHYTHM}[rt_rnd(3)]);   /* 0.24 JAM in FIX's place (retired) */
        seq_set_bview(s, rt_rnd(2) ? BVIEW_BASS : BVIEW_RHYTHM);
        break;
    }
}

static void test_19_random_round_trip(void) {
    enum { RUNS = 100 };
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    char why[RT_WHY] = "", first[RT_WHY + 24] = "";
    int fails = 0, checks = 0, parts = 0;
    for (int r = 0; r < RUNS; r++) {
        int ok;
        rt_seed = 1000u + (unsigned)r;
        seq_init(&s, emit, NULL);
        seq_init_preset(&s);
        for (int i = 5 + rt_rnd(60); i > 0; i--) rt_op(&s);
        seq_txn_done(&s);
        if (seq_state_save(&s, buf, sizeof buf) <= 0) continue;
        seq_init(&t, emit, NULL);
        if (!(ok = seq_state_load(&t, buf))) snprintf(why, RT_WHY, "load refused");
        for (int q = 0; ok && q < SEQ_NPARTS; q++) {
            if ((q && !(s.used_mask >> q & 1)) || (s.stale_mask | s.unsaved_mask) >> q & 1) continue;
            ok = rt_same(&s.part[q], &t.part[q], why);
            parts++;
        }
        t = s;
        render_now(&t);
        if (ok && !rt_same(&s.part[s.cur_part], &t.part[t.cur_part], why)) ok = 0, strncat(why, " (re-render)", RT_WHY - strlen(why) - 1);
        checks++;
        if (!ok && !fails++) snprintf(first, sizeof first, "; run %d: %s", r, why);
    }
    CHECK(!fails && checks > RUNS / 2, "%d seeded random edit sequences: save and load bring back %d parts' Bass and CHORDS, and a "
          "re-render changes nothing (%d failed%s)", checks, parts, fails, first);
}

/* the biggest project: 6 parts x 16 bars, both lanes' bars dense and edited, every Bass field set: every part saved */
static void test_19_full_save_fits(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    unsigned char c[16], b[16];
    int ok = 1;
    rhythm_parse("AxgxAxgxAxgxAxgx", c);
    rhythm_parse("AgxgAgxgAgxgAgx-", b);
    seq_init(&s, emit, NULL);
    for (int part = 0; part < SEQ_NPARTS; part++) {
        seq_select_part(&s, part);
        seq_set_bars(&s, 16);
        seq_part_t *p = &s.part[part];
        for (int bar = 0; bar < SEQ_MAX_BARS; bar++) {
            memcpy(p->rb[DL_CHORDS][bar].c, c, 16);
            p->rb[DL_CHORDS][bar].from = 3, p->rb[DL_CHORDS][bar].ed = 1;
            memcpy(p->rb[DL_BASS][bar].c, b, 16);   /* edited from Funk, locked: the longest entry */
            p->rb[DL_BASS][bar].from = 12, p->rb[DL_BASS][bar].ed = 1, p->rb[DL_BASS][bar].lock = 1;
            p->bass.frozen[bar] = (bass_set_t){-1, 2, -1, {4, 4, 4, -2}};   /* the longest .bl entry */
            p->bass.fix[bar * SEQ_STEPS] = 40, p->bass.fix[bar * SEQ_STEPS + 7] = 52;
            p->lane[LANE_BASS].bar_var[bar] = (unsigned char)(bar + 1);
        }
        p->bass.nlock = 0xffff;
        p->bass.set = (bass_set_t){-1, 2, -1, {4, 4, 4, -2}};
        seq_set_rknob(&s, LANE_CHORDS, RK_SYNC, -2), seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 4), seq_set_rknob(&s, LANE_CHORDS, RK_HUM, 4), seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 4);
        seq_set_rknob(&s, LANE_BASS, RK_SYNC, -2), seq_set_rknob(&s, LANE_BASS, RK_GHOSTS, 4), seq_set_rknob(&s, LANE_BASS, RK_HUM, 4), seq_set_rknob(&s, LANE_BASS, RK_FILL, 4);
        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_PUSH, 100), seq_set_lane_shape(&s, LANE_BASS, BSHAPE_GATE, 100), seq_set_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT, 100);
        seq_set_complexity(&s, LANE_BASS, 100);
        render_now(&s);
    }
    seq_select_part(&s, 0);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && !seq_unsaved_parts(&s), "6 parts x 16 dense edited bars on both lanes, every Bass field set: every part saved in %d bytes (unsaved 0x%x)",
          n, seq_unsaved_parts(&s));
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf), "and it loads");
    for (int part = 0; part < SEQ_NPARTS; part++)
        ok &= seq_part_used(&t, part) && !memcmp(t.part[part].rb[DL_CHORDS], s.part[part].rb[DL_CHORDS], sizeof s.part[part].rb[DL_CHORDS]) &&
              !memcmp(t.part[part].lane[LANE_CHORDS].steps, s.part[part].lane[LANE_CHORDS].steps, sizeof s.part[part].lane[LANE_CHORDS].steps) &&
              bass_same(&s.part[part], &t.part[part]);
    CHECK(ok, "every part's Chords and Bass lanes come back");
}

/* 0.18 Bass lanes (Root, Pump, Walk with APPROACH Chromatic, Manual) x chord length 16/8 x a 2- and a 16-bar loop, with a FIX
 * pick and a variation, load as DRAWN bars at COMPLEXITY 50 that keep their rhythm exactly (Review Focus 5) */
static void test_19_old_bass_converts(void) {
    static const int STYLES[4] = {GEN_BASS_ROOT, GEN_BASS_PUMP, GEN_BASS_WALK, GEN_MANUAL}, MAPPED[4] = {40, 1, 29, 40};
    static const int CL[2] = {16, 8}, BARS[2] = {2, 16};
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    static unsigned char want[SEQ_MAX_BARS][16];
    int runs = 0, old = 0, loads = 0, codes = 0, sounds = 0, fixed = 0, kept = 0, drawn = 0, mapped = 0;
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < 2; c++)
            for (int n = 0; n < 2; n++) {
                seq_init(&s, emit, NULL);
                seq_set_bars(&s, BARS[n]);
                seq_set_chord_len(&s, CL[c]);
                seq_part_t *p = &s.part[0];
                seq_lane_t *ln = &p->lane[LANE_BASS];
                if (STYLES[k] == GEN_MANUAL) {   /* a hand-drawn lane: a held accent, a hit and a ghost in every bar */
                    p->lane_shape[LANE_BASS][BSHAPE_MOVEMENT] = 100;   /* it never moved a MANUAL lane's notes */
                    memset(ln, 0, sizeof *ln);
                    ln->gen = (seq_gen_t){GEN_MANUAL, 1, 30, 0};
                    for (int bar = 0; bar < BARS[n]; bar++) {
                        ln->steps[bar * SEQ_STEPS] = (seq_step_t){1, TONE_R, -1, 112, 4, 0};
                        ln->steps[bar * SEQ_STEPS + 6] = (seq_step_t){1, TONE_5, -1, 100, 2, 0};
                        ln->steps[bar * SEQ_STEPS + 11] = (seq_step_t){1, TONE_R, 0, 60, 1, 0};
                    }
                } else {
                    ln->gen = (seq_gen_t){STYLES[k], 7, 41, 0};
                    ln->bar_var[1] = 2;
                    if (STYLES[k] == GEN_BASS_ROOT) seq_set_lane_shape(&s, LANE_BASS, BSHAPE_SYNC, 40), seq_set_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT, 70);
                    if (STYLES[k] == GEN_BASS_PUMP)
                        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCTAVES, 100), seq_set_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER, 25),
                            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, 1);
                    if (STYLES[k] == GEN_BASS_WALK) seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1);
                    seq_set_complexity(&s, LANE_BASS, 40);   /* regenerates */
                }
                seq_set_step_note(&s, LANE_BASS, 1, 0, 45);   /* a FIX pick: bar 2 step 1 at A2 (locked on a generated lane) */
                for (int bar = 0; bar < BARS[n]; bar++) rhythm_from_steps(&ln->steps[bar * SEQ_STEPS], want[bar]);
                runs++;
                old += seq_state_save(&s, buf, sizeof buf) > 0 && !strstr(buf, "A2.rb") && !strstr(buf, "A2.g=18") &&
                       (STYLES[k] != GEN_MANUAL || strstr(buf, "|A2.bs=100,"));
                seq_init(&t, emit, NULL);
                if (!seq_state_load(&t, buf)) continue;
                loads++;
                const seq_part_t *q = &t.part[0];
                int same = 1, snd = 1;
                for (int bar = 0; bar < BARS[n]; bar++) {
                    const rbar_t *r = &q->rb[DL_BASS][bar];
                    int from = rhythm_lib_find(RLIB_BASS, want[bar]);
                    same &= !memcmp(r->c, want[bar], 16) && r->from == from && r->ed == (from < 0) && !r->lock;
                    snd &= bar_sounds_as(&q->lane[LANE_BASS].steps[bar * SEQ_STEPS], &ln->steps[bar * SEQ_STEPS]);
                }
                codes += same;
                sounds += snd;
                int fix = STYLES[k] == GEN_BASS_PUMP ? 57 : 45;   /* the Pump's OCT +1 played it at A3 */
                fixed += q->bass.fix[SEQ_STEPS] == fix && q->lane[LANE_BASS].steps[SEQ_STEPS].abs == fix;
                kept += q->lane[LANE_BASS].bar_var[1] == (STYLES[k] == GEN_MANUAL ? 0 : 2);
                drawn += q->lane[LANE_BASS].gen.style == GEN_BASS_DRAWN && q->lane[LANE_BASS].gen.complexity == BASS_CX_NEUTRAL &&
                         !q->rk[DL_BASS][0] && !q->rk[DL_BASS][1] && !q->rk[DL_BASS][2] && !q->rk[DL_BASS][3] && !q->bass.nlock;
                bass_set_t w = bass_style_set(MAPPED[k]);
                if (STYLES[k] == GEN_BASS_PUMP) w.k[BK_OCTS] = 4, w.k[BK_REG] = -1, w.octave = 1;
                if (STYLES[k] == GEN_BASS_WALK) w.k[BK_APP] = 2;
                const int *sh = q->lane_shape[LANE_BASS];
                mapped += !memcmp(&q->bass.set, &w, sizeof w) && sh[BSHAPE_SYNC] == 0 && sh[BSHAPE_OCTAVES] == 0 && sh[BSHAPE_REGISTER] == 50 &&
                          sh[BSHAPE_OCT] == 0 && sh[BSHAPE_APPROACH] == 0 && sh[BSHAPE_ACCENT] == (STYLES[k] == GEN_BASS_ROOT ? 70 : 50);
            }
    CHECK(old == runs, "every case saves as a 0.18 Bass lane (%d of %d)", old, runs);
    CHECK(loads == runs, "and loads (%d of %d)", loads, runs);
    CHECK(codes == runs, "each bar's codes are rhythm_from_steps of the old bar; from its Bass library index, else edited (%d of %d)", codes, runs);
    CHECK(sounds == runs, "so every bar keeps its rhythm: on, sounding length, velocity class (%d of %d)", sounds, runs);
    CHECK(fixed == runs, "the FIX pick is a fixed note at the octave it played (OCT +1: A3), and plays (%d of %d)", fixed, runs);
    CHECK(kept == runs, "the variation is kept (%d of %d)", kept, runs);
    CHECK(drawn == runs, "the lane is DRAWN at COMPLEXITY 50, RHYTHM knobs 0, no note lock (%d of %d)", drawn, runs);
    CHECK(mapped == runs, "Root/Manual -> Pop Root, Pump -> Octave Bounce, Walk -> Upright Walk; OCTAVES, REGISTER, OCT and Chromatic "
          "mapped; SYNC dropped; ACCENT kept; a MANUAL lane's MOVEMENT 100 not (%d of %d)", mapped, runs);
    /* a v1 (csq1) song and a v1 reset: the MANUAL Bass as DRAWN bars, its rhythm kept */
    static const char V1S[] = "csq1:1000000000000000100000001000000010101010101010101010101010101010:0000";
    static seq_t v;
    fresh(&v, 0);   /* v1's MANUAL Bass (v1_bass) */
    for (int r = 0; r < 2; r++) {
        seq_init(&t, emit, NULL);
        if (r) seq_reset_v1(&t);
        const seq_part_t *q = &t.part[0];
        int same = (r || seq_state_load(&t, V1S)) && bass_loaded_as(&v.part[0].lane[LANE_BASS], &q->lane[LANE_BASS], 4) == 1 &&
                   q->bass.set.style == 40 && !q->bass.set.k[BK_MOV];
        for (int bar = 0; bar < 4; bar++) {
            unsigned char want[16];
            rhythm_from_steps(&v.part[0].lane[LANE_BASS].steps[bar * SEQ_STEPS], want);
            same &= !memcmp(q->rb[DL_BASS][bar].c, want, 16) && q->rb[DL_BASS][bar].from == rhythm_lib_find(RLIB_BASS, want);
        }
        CHECK(same && bass_all_ref(&t), "%s: the MANUAL Bass loads as DRAWN Pop Root bars that keep its rhythm, rendered", r ? "seq_reset_v1" : "a csq1 load");
    }
}

/* bad Bass fields load with their defaults, values out of range clamped; nothing refuses the load */
static void test_19_bad_fields(void) {
    static seq_t t;
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50|A2=0:|A2.g=18,1,50";
    static char st[512];
    const seq_part_t *q = &t.part[0];
    snprintf(st, sizeof st, "%s|A2.rb=zz|A2.bx=99,9,9|A2.bl=40:x;99:1.1.1.1.1.1.1|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && bass_default(&t, 0) && !q->bass.nlock, "2.rb=zz, 2.bx=99,9,9, 2.bl=40:x;99:... load as the defaults");
    snprintf(st, sizeof st, "%s|A2.bx=99,9,9,9,9,9,9|A2.bl=1:99.9.-9.9.9.9.9;x:1;3:1.1;;2:|A2.rz=9,-9,2,7|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    bass_set_t hi = {47, 2, 1, {4, 4, 4, 2}}, lo = {47, 2, -1, {4, 4, 4, 2}};
    CHECK(seq_state_load(&t, st) && !memcmp(&q->bass.set, &hi, sizeof hi) && q->bass.nlock == 1 << 1 && !memcmp(&q->bass.frozen[1], &lo, sizeof lo),
          "values out of range clamp (style 47, pedal 2, octave -1..1, knobs 0..4, REGISTER 2); a bad .bl entry leaves its bar unlocked");
    CHECK(q->rk[DL_BASS][RK_SYNC] == 3 && q->rk[DL_BASS][RK_GHOSTS] == 0 && q->rk[DL_BASS][RK_HUM] == 2 && q->rk[DL_BASS][RK_FILL] == 4,
          "2.rz=9,-9,2,7 clamps to SYNC 3, GHOSTS 0, HUM 2, FILL 4");
    snprintf(st, sizeof st, "%s|A2.ab=0021zz00ff7f01800225f|A2.bx=1,2|A2.bl=0:-2.-1.0.-1.0.0.-9|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    int others = 1;
    for (int i = 0; i < SEQ_LANE_STEPS; i++) others &= i == 0 || i == 2 || !q->bass.fix[i];
    lo = (bass_set_t){-1, 0, 0, {0, 0, 0, -2}};
    CHECK(seq_state_load(&t, st) && q->bass.fix[0] == 33 && q->bass.fix[2] == 37 && others && bass_all_ref(&t),
          "2.ab keeps its good pairs (step 1 A1, step 3 C#2), drops bad ones, one past the loop and a note of 128");
    CHECK(q->bass.nlock == 1 && !memcmp(&q->bass.frozen[0], &lo, sizeof lo), "a note-locked bar's -2 style is Custom (-1), its values clamped");
    snprintf(st, sizeof st, "%s|A2.rb=L12,Axgx............,3,99|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    unsigned char funk[16];
    rhythm_lib_codes(RLIB_BASS, 12, funk);
    CHECK(seq_state_load(&t, st) && q->rb[DL_BASS][0].lock && q->rb[DL_BASS][0].from == 12 && !memcmp(q->rb[DL_BASS][0].c, funk, 16) &&
          q->rb[DL_BASS][1].from == -1 && q->rb[DL_BASS][1].ed && q->rb[DL_BASS][2].from == 3 && q->rb[DL_BASS][3].from == 0 && !q->rb[DL_BASS][3].ed,
          "2.rb reads the Bass library: L12 is Funk locked, a string is drawn from nothing, 99 stays Root");
}

/* 0.20 Task 6: a JAM Melody saves `3.g=19,seed,cx` (no step records), then `3.bv`, `3.jx` (its settings), `3.jw` (SWITCH AT), `3.jk`
 * (PUSH, GATE, DYNAMICS), `3.jl` (locked bars) and `3.jf` (fixes), each only when not the default */

/* p's and q's JAM Melody lanes are the same: style, seed, COMPLEXITY, variations, settings, SWITCH AT, PUSH, GATE, DYNAMICS, locks
 * with their frozen settings, fixes, steps and lines (bars of p's loop) */
static int mjam_same(const seq_part_t *p, const seq_part_t *q) {
    const mjam_state_t *a = &p->mjam, *b = &q->mjam;
    int n = p->bars * SEQ_STEPS, ok = q->bars == p->bars && !memcmp(&p->lane[LANE_MEL_UP].gen, &q->lane[LANE_MEL_UP].gen, sizeof(seq_gen_t)) &&
                                    !memcmp(p->lane[LANE_MEL_UP].bar_var, q->lane[LANE_MEL_UP].bar_var, sizeof p->lane[0].bar_var) &&
                                    !memcmp(&a->set, &b->set, sizeof a->set) && a->sw == b->sw && a->push == b->push && a->dyn == b->dyn &&
                                    p->mel.kn[MK_GATE] == q->mel.kn[MK_GATE] && a->nlock == b->nlock && !memcmp(a->fix, b->fix, (size_t)n) &&
                                    !memcmp(p->lane[LANE_MEL_UP].steps, q->lane[LANE_MEL_UP].steps, (size_t)n * sizeof(seq_step_t)) &&
                                    !memcmp(p->mline, q->mline, (size_t)n * sizeof(mjam_note_t));
    for (int bar = 0; bar < p->bars; bar++)
        if (a->nlock >> bar & 1) ok &= !memcmp(&a->frozen[bar], &b->frozen[bar], sizeof a->frozen[bar]);
    return ok;
}

/* 1 when the playing part's JAM lane is as a render makes it (a key change and back on a copy changes nothing): its fixes included */
static int mel_jam_rendered(const seq_t *s) {
    static seq_t c;
    c = *s;
    mel_rerender(&c);
    const seq_part_t *p = &s->part[s->cur_part], *q = &c.part[c.cur_part];
    return !memcmp(p->lane[LANE_MEL_UP].steps, q->lane[LANE_MEL_UP].steps, sizeof p->lane[0].steps) && !memcmp(p->mline, q->mline, sizeof p->mline);
}

static void test_20_save_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    seq_part_t *p = &s.part[0];
    double ppq;
    seq_init(&s, emit, NULL);
    seq_mel_queue(&s, MJ_LEAD, 4);   /* stopped: at once */
    seq_mjam_set(&s, MJS_FORM, MF_AAB), seq_mjam_set(&s, MJS_BREATH, 2), seq_mjam_set(&s, MJS_CAD, 1), seq_mjam_set(&s, MJS_FILL, 3);
    seq_mjam_set(&s, MJS_ANS, MA_SWAP), seq_mjam_set(&s, MJS_STRAY, 2), seq_mjam_set(&s, MJS_AOCT, -1), seq_mjam_set(&s, MJS_REG, 2);
    seq_mel_swap(&s, MJ_PEDAL, 3);
    seq_set_mel_switch(&s, 3);   /* PHRASE */
    seq_set_mel_push(&s, 30), seq_set_mel_dyn(&s, 70);
    p->mel.kn[MK_GATE] = 80;
    seq_medit_open(&s, 1);   /* bar 2 locked at AAB, F4 fixed inside it */
    seq_medit_lock(&s);
    seq_medit_select(&s, 2);
    seq_medit_set_oct(&s, 4);
    seq_medit_note(&s, 5);
    seq_medit_done(&s);
    seq_mjam_set(&s, MJS_FORM, MF_SENT);
    seq_medit_open(&s, 3);   /* bar 4 locked at SENT: a rest and a hold */
    seq_medit_lock(&s);
    seq_medit_select(&s, 0);
    seq_medit_rest(&s);
    seq_medit_select(&s, 5);
    seq_medit_hold(&s);
    seq_medit_done(&s);
    seq_medit_open(&s, 2);   /* bar 3 its own variation */
    seq_medit_new_bar(&s);
    seq_medit_done(&s);
    seq_medit_open(&s, 0);   /* bar 1 C4 */
    seq_medit_select(&s, 7);
    seq_medit_set_oct(&s, 4);
    seq_medit_note(&s, 0);
    seq_medit_done(&s);
    CHECK(p->mjam.nlock == (1 << 1 | 1 << 3) && p->mjam.frozen[1].form == MF_AAB && p->mjam.fix[16 + 2] == 65 && p->mjam.fix[48] == MJ_FIX_REST &&
          p->mjam.fix[48 + 5] == MJ_FIX_HOLD && p->mjam.fix[7] == 60 && p->lane[LANE_MEL_UP].bar_var[2] && mel_jam_rendered(&s),
          "set up: LEAD 5, every setting changed, bars 2 and 4 locked, a note fix inside locked bar 2, a rest, a hold, a variation");
    ppq = play_to_k(&s, 0.0, 5);
    seq_mel_queue(&s, MJ_HOOK, 0);
    CHECK(seq_mel_queued(&s, NULL, NULL, NULL), "playing: HOOK 2-NOTE queued");
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "|A3=0:|A3.g=19,1,90|A3.bv=") && !strstr(buf, "A3.lk") && !strstr(buf, "A3.ab") && !strstr(buf, "A3.mr"),
          "a JAM Melody saves 3.g=19,seed,cx and 3.bv, no step records, locks or recipe");
    CHECK(strstr(buf, "|A3.jx=345213520253|A3.jw=3|A3.jk=30,80,70|A3.jl=1:344213520253;3:345213520253|A3.jf=073c124130803581") != NULL,
          "3.jx (LEAD 5, SENT, Beat, OPEN, RUN, SWAP, LAST HALF, -1, High, PEDAL 4), 3.jw PHRASE, 3.jk 30,80,70, 3.jl, 3.jf; not the queued block");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && !seq_mel_queued(&t, NULL, NULL, NULL), "it loads, nothing queued");
    CHECK(mjam_same(p, &t.part[0]), "settings, SWITCH AT, PUSH, GATE, DYNAMICS, locks and their settings, fixes, variation, steps and lines come back");
    CHECK(seq_state_save(&t, again, sizeof again) == n && !strcmp(buf, again), "it saves back byte for byte");
    seq_block(&s, ppq, bpm, 0, 128);
    seq_init(&s, emit, NULL);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A3=0:|A3.g=19,1,10|A4=0:") != NULL, "the default JAM lane: 3.g alone");
}

/* the spec's realistic heavy case: 6 parts x 16 bars, Chords and Bass with 8 edited bars each and every 0.19 field set, the Melody with
 * every setting changed, 4 locked bars and 16 fixed steps: every part saved, and it comes back. Then (printed, no check) 0.19's full worst
 * case with a fully edited Melody too */
static void heavy_part(seq_t *s, int part, int edited, int mlocked, int mfixed) {
    unsigned char c[16], b[16];
    mjam_set_t m = {MJ_RUN, 4, MF_SENT, 3, 2, 3, MA_SWAP, 3, 1, 0, MJ_PEDAL, 4, 2, 2, 3, 1};   /* 0.21: DENSITY, RANGE, VARIATION, TIME too */
    rhythm_parse("AxgxAxgxAxgxAxgx", c);
    rhythm_parse("AgxgAgxgAgxgAgx-", b);
    seq_select_part(s, part);
    seq_set_bars(s, 16);
    seq_part_t *p = &s->part[part];
    for (int bar = 0; bar < edited; bar++) {
        memcpy(p->rb[DL_CHORDS][bar].c, c, 16);
        p->rb[DL_CHORDS][bar].from = 3, p->rb[DL_CHORDS][bar].ed = 1;
        memcpy(p->rb[DL_BASS][bar].c, b, 16);
        p->rb[DL_BASS][bar].from = 12, p->rb[DL_BASS][bar].ed = 1, p->rb[DL_BASS][bar].lock = 1;
        p->bass.frozen[bar] = (bass_set_t){-1, 2, -1, {4, 4, 4, -2}};
        p->bass.nlock |= (unsigned short)(1u << bar);
        p->bass.fix[bar * SEQ_STEPS] = 40, p->bass.fix[bar * SEQ_STEPS + 7] = 52;
        p->lane[LANE_BASS].bar_var[bar] = (unsigned char)(bar + 1);
    }
    p->bass.set = (bass_set_t){-1, 2, -1, {4, 4, 4, -2}};
    seq_set_rknob(s, LANE_CHORDS, RK_SYNC, -2), seq_set_rknob(s, LANE_CHORDS, RK_GHOSTS, 4), seq_set_rknob(s, LANE_CHORDS, RK_HUM, 4), seq_set_rknob(s, LANE_CHORDS, RK_FILL, 4);
    seq_set_rknob(s, LANE_BASS, RK_SYNC, -2), seq_set_rknob(s, LANE_BASS, RK_GHOSTS, 4), seq_set_rknob(s, LANE_BASS, RK_HUM, 4), seq_set_rknob(s, LANE_BASS, RK_FILL, 4);
    seq_set_lane_shape(s, LANE_BASS, BSHAPE_PUSH, 100), seq_set_lane_shape(s, LANE_BASS, BSHAPE_GATE, 100), seq_set_lane_shape(s, LANE_BASS, BSHAPE_ACCENT, 100);
    seq_set_complexity(s, LANE_BASS, 100);
    if (mlocked < 0) {   /* the Melody as a new instance's */
        render_now(s);
        return;
    }
    p->mjam.set = m;
    p->mjam.sw = 3, p->mjam.push = 100, p->mjam.dyn = 100, p->mel.kn[MK_GATE] = 100;
    p->mjam.swing = 5;   /* 0.21 (Amendment 1: ACCENT stays LIGHT, a load keeps it so) */
    for (int bar = 0; bar < mlocked; bar++) {
        p->mjam.frozen[bar] = m;
        p->mjam.nlock |= (unsigned short)(1u << bar);
    }
    for (int i = 0; i < mfixed; i++) p->mjam.fix[i * SEQ_LANE_STEPS / mfixed] = (unsigned char)(i % 3 ? 100 + i % 28 : MJ_FIX_REST);
    for (int bar = 0; bar < SEQ_MAX_BARS; bar++) p->lane[LANE_MEL_UP].bar_var[bar] = (unsigned char)(bar + 1);
    render_now(s);
}

static void test_20_save_fits(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    int ok = 1;
    seq_init(&s, emit, NULL);
    for (int part = 0; part < SEQ_NPARTS; part++) heavy_part(&s, part, 8, 4, 16);
    seq_select_part(&s, 0);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && !seq_unsaved_parts(&s), "6 parts: Chords and Bass 8 edited bars each, every 0.19 field set; the Melody every setting changed, 4 "
          "locked bars, 16 fixed steps: every part saved in %d bytes of %d (unsaved 0x%x)", n, SEQ_STATE_MAX, seq_unsaved_parts(&s));
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf), "and it loads");
    for (int part = 0; part < SEQ_NPARTS; part++)
        ok &= seq_part_used(&t, part) && !memcmp(t.part[part].rb[DL_CHORDS], s.part[part].rb[DL_CHORDS], sizeof s.part[part].rb[DL_CHORDS]) &&
              !memcmp(t.part[part].lane[LANE_CHORDS].steps, s.part[part].lane[LANE_CHORDS].steps, sizeof s.part[part].lane[LANE_CHORDS].steps) &&
              bass_same(&s.part[part], &t.part[part]) && mjam_same(&s.part[part], &t.part[part]);
    CHECK(ok, "every part's Chords, Bass and Melody lanes come back");
    seq_init(&s, emit, NULL);
    for (int part = 0; part < SEQ_NPARTS; part++) heavy_part(&s, part, 16, 16, SEQ_LANE_STEPS);
    seq_select_part(&s, 0);
    n = seq_state_save(&s, buf, sizeof buf);
    printf("     (no check) 0.19's full worst case plus a fully edited Melody (16 locked bars, all 256 steps fixed) in 6 parts: %d bytes, "
           "unsaved parts 0x%x\n", n, seq_unsaved_parts(&s));
    seq_init(&s, emit, NULL);
    for (int part = 0; part < SEQ_NPARTS; part++) heavy_part(&s, part, 16, -1, 0);
    seq_select_part(&s, 0);
    n = seq_state_save(&s, buf, sizeof buf);
    printf("     (no check) 0.19's full worst case alone (default Melody) in 6 parts: %d bytes, unsaved parts 0x%x\n", n, seq_unsaved_parts(&s));
}

/* Owner (2026-10-20): old projects are throwaway work, no melody conversion. A 0.19-or-earlier project loads (never refused) with its
 * Melody lane as 0.19 loads it (a recipe, a 0.10 style, MANUAL notes), its Chords and Bass as before. The first JAM action on such a
 * lane makes it a new instance's JAM lane (no variation, lock or fix), then acts: one undo step, whose undo puts the old lane back */
static int mel_lane_same(const seq_part_t *p, const seq_part_t *q) {
    const seq_lane_t *a = &p->lane[LANE_MEL_UP], *b = &q->lane[LANE_MEL_UP];
    const mjam_state_t *x = &p->mjam, *y = &q->mjam;
    return !memcmp(&a->gen, &b->gen, sizeof a->gen) && !memcmp(a->bar_var, b->bar_var, sizeof a->bar_var) && !memcmp(&p->mel, &q->mel, sizeof p->mel) &&
           !memcmp(a->steps, b->steps, (size_t)p->bars * SEQ_STEPS * sizeof(seq_step_t)) && !memcmp(&x->set, &y->set, sizeof x->set) &&
           !memcmp(x->frozen, y->frozen, sizeof x->frozen) && x->nlock == y->nlock && !memcmp(x->fix, y->fix, sizeof x->fix) && x->sw == y->sw &&
           x->push == y->push && x->dyn == y->dyn;
}

static void test_20_old_project_loads(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    static const char *const ACT[6] = {"a block queued (stopped: at once)", "FORM set", "the SWAP block set", "SWITCH AT set", "the picker opened",
                                       "the editor opened"};
    seq_init(&s, emit, NULL);
    old_init_mel(&s);   /* 0.19's recipe (Up-down Arp) */
    s.part[0].lane[LANE_MEL_UP].bar_var[1] = 3;
    render_now(&s);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "|A3.g=7,1,30|A3.bv=") && strstr(buf, "|A3.mr=0,2,"), "set up: a 0.19 save with a recipe Melody (3.mr) and a variation");
    seq_init(&t, emit, NULL);
    const seq_part_t *p = &s.part[0], *q = &t.part[0];
    CHECK(seq_state_load(&t, buf) && q->lane[LANE_MEL_UP].gen.style == GEN_MEL_RECIPE && mel_lane_same(p, q),
          "it loads: its Melody the same recipe lane (not JAM), variation, recipe and steps as before");
    CHECK(!memcmp(q->rb[DL_CHORDS], p->rb[DL_CHORDS], 4 * sizeof p->rb[0][0]) &&
          !memcmp(q->lane[LANE_CHORDS].steps, p->lane[LANE_CHORDS].steps, sizeof p->lane[0].steps) && bass_same(p, q),
          "its Chords and Bass as before");
    CHECK(seq_state_save(&t, again, sizeof again) == n && !strcmp(buf, again), "it saves back byte for byte");
    seq_mel_queue(&t, MJ_ARP, 0), seq_mjam_set(&t, MJS_FORM, MF_LOOP), seq_mel_swap(&t, MJ_RUN, 2), seq_set_mel_switch(&t, 1);
    CHECK(q->lane[LANE_MEL_UP].gen.style == GEN_MEL_RECIPE && mel_lane_same(p, q) && !t.undo_n,
          "the shown values set again (a host echo: ARP UP, LOOP, RUN-UP, BAR): the old lane stays as it is");
    for (int a = 0; a < 6; a++) {
        seq_init(&t, emit, NULL);
        seq_state_load(&t, buf);
        switch (a) {
        case 0: seq_mel_queue(&t, MJ_HOOK, 1); break;
        case 1: seq_mjam_set(&t, MJS_FORM, MF_1_1); break;
        case 2: seq_mel_swap(&t, MJ_PEDAL, 0); break;
        case 3: seq_set_mel_switch(&t, 3); break;
        case 4: seq_mpick_open(&t); break;
        default: seq_medit_open(&t, 2); break;
        }
        mjam_set_t want = mjam_default();
        if (a == 0) want.c = MJ_HOOK, want.l = 1;
        if (a == 1) want.form = MF_1_1;
        if (a == 2) want.sc = MJ_PEDAL, want.sl = 0;
        const mjam_state_t *m = &q->mjam;
        int fixes = 0;
        for (int i = 0; i < SEQ_LANE_STEPS; i++) fixes += m->fix[i] != 0;
        CHECK(q->lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && q->lane[LANE_MEL_UP].gen.complexity == (a ? 10 : 30) && !memcmp(&m->set, &want, sizeof want) &&
              m->sw == (a == 3 ? 3 : 1) && !m->nlock && !fixes && !q->lane[LANE_MEL_UP].bar_var[1] && q->mel.kn[MK_GATE] == 50 && mel_is_model(&t) &&
              t.undo_n == 1 && seq_mview(&t) == (a == 4 ? MVIEW_PICK : a == 5 ? MVIEW_EDIT : MVIEW_JAM),
              "%s on the old lane: first a new instance's JAM lane (no variation), then the action; one undo step", ACT[a]);
        seq_txn_done(&t);
        seq_undo(&t);
        CHECK(q->lane[LANE_MEL_UP].gen.style == GEN_MEL_RECIPE && mel_lane_same(p, q), "%s: UNDO puts the recipe lane back as loaded (its melody state too)", ACT[a]);
        if (a > 3) continue;
        switch (a) {   /* the same action again: not an echo, it converts again */
        case 0: seq_mel_queue(&t, MJ_HOOK, 1); break;
        case 1: seq_mjam_set(&t, MJS_FORM, MF_1_1); break;
        case 2: seq_mel_swap(&t, MJ_PEDAL, 0); break;
        default: seq_set_mel_switch(&t, 3); break;
        }
        CHECK(q->lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && !memcmp(&q->mjam.set, &want, sizeof want) && q->mjam.sw == (a == 3 ? 3 : 1) &&
              t.undo_n == 1, "%s again after the UNDO: JAM again, the action applied", ACT[a]);
    }
}

/* bad JAM fields load (never refused, nothing read past their ends): a bad field falls back to its default, values are clamped */
static void test_20_bad_fields(void) {
    static seq_t t;
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50|A2=0:|A2.g=18,1,50"
                               "|A3=0:|A3.g=19,1,10";
    static char st[512];
    const seq_part_t *q = &t.part[0];
    snprintf(st, sizeof st, "%s|A3.jx=zz|A3.jl=99:!!|A3.jf=ffff|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && mel_default_lane(&t) && mel_is_model(&t), "3.jx=zz, 3.jl=99:!!, 3.jf=ffff: the JAM default");
    snprintf(st, sizeof st, "%s|A3.jx=zzzzzzzzzzzz|A3.jw=9|A3.jk=999,-5,40|A3.jl=1:zzzzzzzzzzzz;2:!!!!!!!!!!!!;x:1;3:;0:1234|A3.jf=0081zz0002800fffff7f014503820400|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    mjam_set_t hi = {5, 4, 5, 3, 3, 3, 5, 3, 1, 2, 5, 4, 0, 0, 0, 0};
    int others = 1;
    for (int i = 3; i < SEQ_LANE_STEPS; i++) others &= !q->mjam.fix[i];
    CHECK(seq_state_load(&t, st) && !memcmp(&q->mjam.set, &hi, sizeof hi) && q->mjam.sw == 3 && q->mjam.push == 100 && q->mel.kn[MK_GATE] == 0 &&
          q->mjam.dyn == 40, "3.jx of z's clamps every setting to its top, 3.jw=9 to PHRASE, 3.jk=999,-5,40 to 100,0,40");
    CHECK(q->mjam.nlock == 1 << 1 && !memcmp(&q->mjam.frozen[1], &hi, sizeof hi), "3.jl: bar 2 locked (clamped); bad entries leave their bars unlocked");
    CHECK(q->mjam.fix[0] == MJ_FIX_HOLD && q->mjam.fix[1] == 69 && q->mjam.fix[2] == MJ_FIX_REST && others && mel_jam_rendered(&t),
          "3.jf keeps its good pairs (a hold, A4, a rest) and drops bad ones, one past the loop, values 0, 255 and 130; rendered");
    snprintf(st, sizeof st, "%s|A3.jx=34521352025|A3.jw=|A3.jk=1,2|A3.jf=008|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && mel_default_lane(&t), "a short 3.jx, an empty 3.jw, a short 3.jk and an odd 3.jf: the defaults");
}

/* 0.21 Task 2: DENSITY, RANGE and VARIATION (MJS_DENS, MJS_RNG, MJS_VAR), Amendment 1 TIME (MJS_TIME), are settings as 0.20's: clamped,
 * a set equal to the value returns early (a lane that isn't JAM stays as it is), the first change on one turns it JAM; one render, no
 * undo step */
static void test_21_settings(void) {
    static seq_t s;
    static const int LO[4] = {-2, -1, 0, -1}, HI[4] = {2, 2, 3, 1};
    static const char *const NAME[4] = {"DENSITY", "RANGE", "VARIATION", "TIME"};
    unsigned h0;
    seq_init(&s, emit, NULL);
    for (int i = 0; i < 4; i++) {
        int f = MJS_DENS + i, ok = 1;
        for (int v = LO[i]; v <= HI[i]; v++) {
            seq_mjam_set(&s, f, v);
            ok &= seq_mjam_get(&s, f) == v && mel_is_model(&s);
        }
        seq_mjam_set(&s, f, LO[i] - 7);
        ok &= seq_mjam_get(&s, f) == LO[i];
        seq_mjam_set(&s, f, HI[i] + 7);
        ok &= seq_mjam_get(&s, f) == HI[i];
        seq_mjam_set(&s, f, 0);
        CHECK(ok && s.undo_n == 0 && seq_mjam_get(&s, f) == 0, "%s: %d..%d each renders the model's lines, out of range clamps, no undo step", NAME[i], LO[i], HI[i]);
    }
    CHECK(MJS_TIME == MJS_VAR + 1 && MJS_COUNT == MJS_TIME + 1 && seq_mjam_get(&s, MJS_COUNT) == 0, "TIME follows VARIATION, MJS_COUNT TIME; past it reads 0");
    seq_mjam_set(&s, MJS_DENS, 2);
    h0 = mel_lines_hash(&s);
    s.part[0].mline[3].note ^= 1;   /* a render would put it back; an early return leaves it */
    seq_mjam_set(&s, MJS_DENS, 2);
    CHECK(mel_lines_hash(&s) != h0, "a set equal to the value returns early (no render)");
    seq_init(&s, emit, NULL);
    old_mel(&s);
    seq_mjam_set(&s, MJS_DENS, 0);
    seq_mjam_set(&s, MJS_VAR, 0);
    CHECK(s.part[0].lane[LANE_MEL_UP].gen.style != GEN_MEL_JAM && s.undo_n == 0, "a set equal to the value on a lane that isn't JAM leaves it as it is");
    seq_mjam_set(&s, MJS_DENS, 1);
    CHECK(s.part[0].lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM && seq_mjam_get(&s, MJS_DENS) == 1 && s.undo_n == 1 && mel_is_model(&s),
          "DENSITY on a lane that isn't JAM turns it JAM first (one undo step), then applies");
}

/* SWING 0..5 and ACCENT 0..3: per part, clamped, no undo step; ACCENT re-renders; a bad value is ignored; and an UNDO of a melody edit
 * keeps both (as PUSH and DYNAMICS) */
static void test_21_undo_keeps_swing_acc(void) {
    static seq_t s;
    unsigned h0;
    seq_init(&s, emit, NULL);
    CHECK(seq_mel_swing(&s) == 0 && seq_mel_acc(&s) == 1 && seq_mel_weak(&s) == 0, "a new JAM lane: SWING 50%% (0), ACCENT LIGHT (1), no weak flag");
    seq_set_mel_swing(&s, 3);
    seq_set_mel_acc(&s, 3);
    CHECK(seq_mel_swing(&s) == 3 && seq_mel_acc(&s) == 3 && s.undo_n == 0, "SWING 3 and ACCENT 3 set, no undo step");
    seq_set_mel_swing(&s, 9);
    seq_set_mel_acc(&s, -1);
    CHECK(seq_mel_swing(&s) == 5 && seq_mel_acc(&s) == 0, "out of range clamps (5, 0)");
    seq_set_mel_swing(&s, 3);
    seq_set_mel_acc(&s, 3);
    h0 = mel_lines_hash(&s);
    seq_medit_open(&s, 0);
    seq_medit_select(&s, 7);
    seq_medit_set_oct(&s, 4);
    seq_medit_note(&s, 0);
    seq_medit_done(&s);
    seq_set_mel_swing(&s, 5);   /* after the edit: UNDO must keep these, not restore the snapshot's 0 and 1 */
    seq_set_mel_acc(&s, 0);
    CHECK(s.part[0].mjam.fix[7] == 60 && s.undo_n == 1 && mel_lines_hash(&s) != h0, "a melody edit (C4 fixed on step 8): one undo step");
    seq_undo(&s);
    CHECK(s.part[0].mjam.fix[7] == 0 && seq_mel_swing(&s) == 5 && seq_mel_acc(&s) == 0, "SWING and ACCENT changed after the edit: UNDO takes the edit back and keeps them");
    seq_select_part(&s, 1);
    seq_set_mel_swing(&s, 1);
    seq_select_part(&s, 0);
    CHECK(seq_mel_swing(&s) == 5 && seq_mel_acc(&s) == 0, "another part's SWING is its own");
}

/* the pinned 0.20 chunk of test_20_save_round_trip, as 0.20 wrote it */
#define CHUNK_020 "|A3.jx=345213520253|A3.jw=3|A3.jk=30,80,70|A3.jl=1:344213520253;3:345213520253|A3.jf=073c124130803581"

static void test_21_save_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    seq_part_t *p = &s.part[0];
    int n;
    seq_init(&s, emit, NULL);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, "A3.jy=") && !strstr(buf, "A3.jv=") && !strstr(buf, "A3.jm="), "defaults write none of 3.jy, 3.jv, 3.jm");
    seq_set_mel_swing(&s, 3);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A3.jv=31") && !strstr(buf, "A3.jy="), "SWING alone: 3.jv=31 (swing 3, acc 1), no 3.jy");
    seq_init(&s, emit, NULL);
    seq_mjam_set(&s, MJS_DENS, 1), seq_mjam_set(&s, MJS_RNG, -1), seq_mjam_set(&s, MJS_VAR, 2), seq_mjam_set(&s, MJS_TIME, 1);
    seq_set_mel_swing(&s, 3);
    n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "|A3.jy=3022|") && strstr(buf, "|A3.jv=31") && !strstr(buf, "A3.jm=") && !strstr(buf, "A3.jx="),
          "DENSITY 1, RANGE -1, VARIATION 2, TIME DOUBLE -> 3.jy=3022 (value - LO, base 36); SWING 3 -> 3.jv=31; 3.jx stays out (its 12 are default)");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && mjam_same(p, &t.part[0]) && seq_mel_swing(&t) == 3 && seq_mel_acc(&t) == 1 && seq_mjam_get(&t, MJS_DENS) == 1 &&
          seq_mjam_get(&t, MJS_RNG) == -1 && seq_mjam_get(&t, MJS_VAR) == 2 && seq_mjam_get(&t, MJS_TIME) == 1 && mel_is_model(&t),
          "it loads: all five and the lines come back");
    CHECK(seq_state_save(&t, again, sizeof again) == n && !strcmp(buf, again), "it saves back byte for byte");
    seq_init(&s, emit, NULL);
    seq_mjam_set(&s, MJS_TIME, -1);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A3.jy=2100|") && !strstr(buf, "A3.jv="), "TIME HALF alone: 3.jy=2100");
}

/* 0.21 Amendment 1: a 3-character 3.jy or 3.jm (the first 0.21.0 build) loads with TIME NORMAL; ACCENT left the knobs, so a saved
 * 3.jv ACCENT loads as LIGHT (its velocities fixed); nothing but the internal seq_set_mel_acc changes it */
static void test_21_time_load_3char(void) {
    static seq_t t;
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50|A2=0:|A2.g=18,1,50"
                               "|A3=0:|A3.g=19,1,10";
    static char st[512], buf[SEQ_STATE_MAX];
    const mjam_state_t *m = &t.part[0].mjam;
    snprintf(st, sizeof st, "%s|A3.jy=302|A3.jv=33|A3.jl=1:345213520253|A3.jm=1:310|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && m->set.dens == 1 && m->set.rng == -1 && m->set.var == 2 && m->set.tm == 0 && m->nlock == 1 << 1 &&
          m->frozen[1].dens == 1 && m->frozen[1].tm == 0 && seq_mel_swing(&t) == 3 && seq_mel_acc(&t) == 1 && mel_jam_rendered(&t),
          "3.jy=302 and 3.jm=1:310 load DENSITY, RANGE, VARIATION with TIME NORMAL; 3.jv=33: SWING 3, ACCENT LIGHT (%d)", seq_mel_acc(&t));
    seq_state_save(&t, buf, sizeof buf);
    CHECK(strstr(buf, "|A3.jy=3021|") && strstr(buf, "|A3.jv=31") && strstr(buf, "|A3.jm=1:3101"), "it saves back as 4 characters (3021, 1:3101), ACCENT 1");
}

/* a locked bar freezes the new fields with the rest; a later knob turn leaves it; saved in 3.jm */
static void test_21_lock_keeps_new_fields(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    mjam_note_t was[SEQ_STEPS], line[SEQ_STEPS];
    unsigned h;
    char *at;
    seq_init(&s, emit, NULL);
    seq_mjam_set(&s, MJS_DENS, 1);
    seq_mjam_set(&s, MJS_TIME, 1);   /* 0.21 Amendment 1: TIME freezes with the rest */
    seq_medit_open(&s, 1);   /* bar 2 */
    seq_medit_lock(&s);
    seq_medit_done(&s);
    seq_mel_line(&s, 1, was);
    h = mel_lines_hash(&s);
    seq_mjam_set(&s, MJS_DENS, -1);
    seq_mjam_set(&s, MJS_TIME, -1);
    seq_mel_line(&s, 1, line);
    CHECK(s.part[0].mjam.frozen[1].dens == 1 && s.part[0].mjam.frozen[1].tm == 1 && !memcmp(was, line, sizeof was) && mel_lines_hash(&s) != h &&
          mel_is_model(&s), "bar 2 locked at DENSITY FILLED, TIME DOUBLE keeps playing them after DENSITY THIN, TIME HALF; the other bars move");
    seq_state_save(&s, buf, sizeof buf);
    at = strstr(buf, "|A3.jm=");
    CHECK(strstr(buf, "|A3.jy=1100") && at && !strncmp(at, "|A3.jm=1:3102", 13) && (at[13] == '|' || !at[13]),
          "saved: 3.jy=1100 (THIN, AS DRAWN, OFF, HALF) and 3.jm=1:3102 (bar 2: FILLED, AS DRAWN, OFF, DOUBLE)");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && t.part[0].mjam.frozen[1].dens == 1 && t.part[0].mjam.frozen[1].tm == 1 && seq_mjam_get(&t, MJS_DENS) == -1 &&
          seq_mjam_get(&t, MJS_TIME) == -1 && mjam_same(&s.part[0], &t.part[0]), "it loads: the frozen DENSITY and TIME and the current ones come back");
}

/* a 0.20 project (no 3.jy, 3.jv, 3.jm) loads with the new fields at their defaults: the same notes, ACCENT LIGHT, SWING 50%, and saves
 * back without them */
static void test_21_load_020_chunk(void) {
    static seq_t t;
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50|A2=0:|A2.g=18,1,50"
                               "|A3=0:|A3.g=19,1,90";
    static char st[512], buf[SEQ_STATE_MAX];
    const mjam_state_t *m = &t.part[0].mjam;
    snprintf(st, sizeof st, "%s" CHUNK_020 "|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && m->set.dens == 0 && m->set.rng == 0 && m->set.var == 0 && m->set.tm == 0 && m->frozen[1].dens == 0 &&
          m->frozen[3].var == 0 && m->frozen[3].tm == 0 &&
          seq_mel_swing(&t) == 0 && seq_mel_acc(&t) == 1 && m->nlock == (1 << 1 | 1 << 3) && m->set.reg == 2 && mel_jam_rendered(&t),
          "a 0.20 chunk loads: DENSITY, RANGE, VARIATION neutral (the lines are the model's), SWING 50%%, ACCENT LIGHT, locks and fixes as saved");
    seq_state_save(&t, buf, sizeof buf);
    CHECK(strstr(buf, CHUNK_020) && !strstr(buf, "A3.jy=") && !strstr(buf, "A3.jv=") && !strstr(buf, "A3.jm="), "it saves back as the 0.20 chunk, no new keys");
}

/* 3.jy of 2 or 5 characters (0.21 Amendment 1: 3 and 4 load) and 3.jv of 1 character are ignored whole; 3.jm entries on unlocked bars
 * or of the wrong length are skipped; the top character clamps */
static void test_21_bad_fields(void) {
    static seq_t t;
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50|A2=0:|A2.g=18,1,50"
                               "|A3=0:|A3.g=19,1,10";
    static char st[512];
    const mjam_state_t *m = &t.part[0].mjam;
    snprintf(st, sizeof st, "%s|A3.jy=11|A3.jv=3|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && mel_default_lane(&t) && seq_mel_swing(&t) == 0 && seq_mel_acc(&t) == 1, "3.jy of 2 characters and 3.jv of 1: ignored, the defaults");
    snprintf(st, sizeof st, "%s|A3.jy=11021|A3.jv=312|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && mel_default_lane(&t) && seq_mel_swing(&t) == 0 && seq_mel_acc(&t) == 1, "3.jy of 5 characters and 3.jv of 3: ignored, the defaults");
    snprintf(st, sizeof st, "%s|A3.jy=1!1|A3.jv=!3|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && mel_default_lane(&t) && seq_mel_swing(&t) == 0 && seq_mel_acc(&t) == 1, "a bad character: ignored whole");
    snprintf(st, sizeof st, "%s|A3.jy=zzzz|A3.jv=zz|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && m->set.dens == 2 && m->set.rng == 2 && m->set.var == 3 && m->set.tm == 1 && seq_mel_swing(&t) == 5 &&
          seq_mel_acc(&t) == 1 && mel_is_model(&t), "z's clamp to the top of every range (DENSITY 2, RANGE 2, VARIATION 3, TIME 1, SWING 5); ACCENT LIGHT");
    snprintf(st, sizeof st, "%s|A3.jl=1:345213520253;3:345213520253|A3.jm=1:zzz;2:333;0:1;x:333;1:33;3:zzzz;3:33333|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && m->nlock == (1 << 1 | 1 << 3) && m->frozen[1].dens == 2 && m->frozen[1].rng == 2 && m->frozen[1].var == 3 &&
          m->frozen[1].tm == 0 && m->frozen[3].tm == 1 && m->frozen[3].var == 3 && mel_jam_rendered(&t),
          "3.jm: a locked bar takes its entry (clamped; 3 characters TIME NORMAL, 4 TIME); an unlocked bar, a bad bar and a wrong length are skipped");
    snprintf(st, sizeof st, "%s|A3.jm=1:333|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && mel_default_lane(&t) && !m->nlock, "3.jm with no 3.jl: nothing locks");
}

/* 0.21 Amendment 1: TIME's weak flag (MW_TIME, ACCENT's old bit) as DENSITY's: one render with TIME NORMAL, weak when fewer than 1 in 8
 * of the loop's sounding steps differ. ARP UP: HALF and DOUBLE change most steps; PEDAL HOLD 5 at HALF only each bar's step 16 */
static void test_21_time_weak(void) {
    static seq_t s;
    int ok;
    seq_init(&s, emit, NULL);
    CHECK(MW_TIME == 16, "MW_TIME takes ACCENT's bit (16)");
    seq_mjam_set(&s, MJS_TIME, -1);
    ok = !seq_mel_weak(&s);
    seq_mjam_set(&s, MJS_TIME, 1);
    ok &= !seq_mel_weak(&s) && seq_mel_grid16(&s);
    CHECK(ok, "ARP UP: TIME HALF and DOUBLE are not weak; DOUBLE swings on 16ths");
    seq_mjam_set(&s, MJS_TIME, 0);
    seq_mel_queue(&s, MJ_PEDAL, 0);   /* stopped: at once */
    seq_mjam_set(&s, MJS_TIME, -1);
    CHECK(seq_mel_weak(&s) == MW_TIME, "PEDAL HOLD 5 at TIME HALF: weak (MW_TIME alone: 0x%02x)", seq_mel_weak(&s));
    seq_mjam_set(&s, MJS_TIME, 0);
    CHECK(seq_mel_weak(&s) == 0, "back to NORMAL: no flag");
}

/* the swing grid of a block that takes over while playing is the new block's (nothing renders at the switch) */
static void test_21_queued_grid16(void) {
    static seq_t s;
    int found = 0, l8 = -1, l16 = -1;
    double ppq;
    seq_init(&s, emit, NULL);
    for (int l = 0; l < MJ_LEVELS; l++) {   /* stopped: each block at once */
        seq_mel_queue(&s, MJ_ARP, l);
        if (seq_mel_grid16(&s)) l16 = l;
        else l8 = l;
    }
    found = l8 >= 0 && l16 >= 0;
    CHECK(found, "ARP has a block on 8ths (%d) and one on odd 16ths (%d)", l8, l16);
    if (!found) return;
    seq_mel_queue(&s, MJ_ARP, l8);
    CHECK(seq_mel_grid16(&s) == 0, "playing ARP block %d: 8ths", l8);
    ppq = play_to_k(&s, 0, 21);
    seq_mel_queue(&s, MJ_ARP, l16);
    CHECK(seq_mel_grid16(&s) == 0, "queued: still the playing block's grid");
    ppq = play_to_k(&s, ppq, 40);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(seq_mel_grid16(&s) == 1, "after the switch, with no setter between: the new block's 16th grid");
}

/* the weak flags of a block that takes over while playing are the new block's (nothing renders at the switch), and a knob turned
 * while it is queued counts in them: DENSITY (a render) and SWING (no render: its bit counted again) */
static void test_21_queued_weak(void) {
    static seq_t s;
    int lw = -1, ls = -1, cw = -1, cl = -1;
    double ppq;
    seq_init(&s, emit, NULL);
    seq_set_mel_swing(&s, 3);
    for (int c = 0; c < MJ_COLS && cw < 0; c++)   /* stopped: each block at once */
        for (int l = 0; l < MJ_LEVELS && cw < 0; l++) {
            seq_mel_queue(&s, c, l);
            if (seq_mel_weak(&s) & MW_SWING) cw = c, cl = l;
        }
    seq_set_mel_swing(&s, 0);
    CHECK(cw >= 0, "SWING 3: a block with almost no notes on its swung positions (%s %s)", cw < 0 ? "-" : mjam_col_name(cw),
          cw < 0 ? "-" : mjam_block_name(cw, cl));
    seq_mjam_set(&s, MJS_DENS, 1);   /* FILLED */
    for (int l = 0; l < MJ_LEVELS; l++) {   /* stopped: each block at once */
        seq_mel_queue(&s, MJ_ARP, l);
        if (seq_mel_weak(&s) & MW_DENS) lw = l;
        else ls = l;
    }
    CHECK(lw >= 0 && ls >= 0, "DENSITY FILLED: ARP has a block it fills (%d) and one it barely changes (%d)", ls, lw);
    if (lw < 0 || ls < 0) return;
    seq_mjam_set(&s, MJS_DENS, 0);
    seq_mel_queue(&s, MJ_ARP, ls);
    CHECK(seq_mel_weak(&s) == 0, "playing ARP block %d at DENSITY AS DRAWN: no weak flag", ls);
    ppq = play_to_k(&s, 0, 21);
    seq_mel_queue(&s, MJ_ARP, lw);
    seq_mjam_set(&s, MJS_DENS, 1);
    CHECK(seq_mel_weak(&s) == 0, "block %d queued, then DENSITY FILLED: still the playing block's flags (FILLED fills it)", lw);
    ppq = play_to_k(&s, ppq, 40);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(!seq_mel_queued(&s, NULL, NULL, NULL) && seq_mel_weak(&s) == MW_DENS, "after the switch, with no setter between: block %d's DENSITY weak flag", lw);
    if (cw < 0) return;
    seq_mjam_set(&s, MJS_DENS, 0);
    ppq = play_to_k(&s, ppq, 44);
    seq_mel_queue(&s, cw, cl);
    seq_set_mel_swing(&s, 3);
    CHECK(seq_mel_queued(&s, NULL, NULL, NULL) && seq_mel_weak(&s) == 0, "%s %s queued, then SWING 3: the playing ARP block swings (no flag)",
          mjam_col_name(cw), mjam_block_name(cw, cl));
    ppq = play_to_k(&s, ppq, 72);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(!seq_mel_queued(&s, NULL, NULL, NULL) && seq_mel_weak(&s) == MW_SWING, "after the switch: the new block's SWING weak flag");
}

/* a block queued while playing takes the new knobs at the switch */
static void test_21_queued_gets_knobs(void) {
    static seq_t s;
    static mjam_note_t nw[SEQ_LANE_STEPS], nw0[SEQ_LANE_STEPS];
    static int got[MJ_KMAX];
    int once, ok = 1, differs = 0;
    double ppq;
    seq_init(&s, emit, NULL);
    model_lines(&s, MJ_ARP, 2, nw0);
    nev = 0;
    ppq = play_to_k(&s, 0, 21);
    seq_mel_queue(&s, MJ_ARP, 2);
    seq_mjam_set(&s, MJS_DENS, 1);
    model_lines(&s, MJ_ARP, 2, nw);
    ppq = play_to_k(&s, ppq, 40);
    seq_block(&s, ppq, bpm, 0, 128);
    once = mel_ons(&s, got);
    for (int k = 32; k < 40; k++) {
        ok &= got[k] == nw[k].note;
        differs += nw[k].note != nw0[k].note;
    }
    for (int k = 32; k < 64; k++) differs += nw[k].note != nw0[k].note;
    CHECK(once && ok && differs && balanced(), "ARP PINKY queued, then DENSITY FILLED: the block that takes over plays FILLED (%d notes differ from AS DRAWN)", differs);
}

/* 0.21 the Melody channel's note-ons logged since nev = 0, by the step that fired them (vel 0: none): its note, velocity, block
 * position (ppq) and the beats to its note-off's block (-1: none logged); 1 when no step had two */
typedef struct { int note, vel; double on, len; } mel_ev_t;
static int mel_evs(const seq_t *s, mel_ev_t e[MJ_KMAX]) {
    static int n[MJ_KMAX];
    int ch = seq_channel_of(s, LANE_MEL_UP), ok = 1;
    memset(e, 0, sizeof(mel_ev_t) * MJ_KMAX);
    memset(n, 0, sizeof n);
    for (int i = 0; i < nev; i++) {
        int k = evs[i].k;
        if (!is_on(&evs[i]) || (evs[i].status & 15) != ch - 1 || k < 0 || k >= MJ_KMAX) continue;
        ok &= ++n[k] == 1;
        e[k] = (mel_ev_t){evs[i].note, evs[i].vel, evs[i].ppq, -1};
        for (int j = i + 1; j < nev; j++)
            if (is_off(&evs[j]) && (evs[j].status & 15) == ch - 1 && evs[j].note == evs[i].note) { e[k].len = evs[j].ppq - evs[i].ppq; break; }
    }
    return ok;
}

/* 1 when block position x is the block an event at time t went out in: (t, t + 1 block] */
static int at_t(double x, double t) { return x > t - 1e-9 && x <= t + blk() + 1e-9; }

/* 0.21 ACCENT: velocities 96 + (strength - 2) x a, baked in the steps; PLAY's DYNAMICS still scales them in note_on */
static void test_21_accent(void) {
    static seq_t s;
    static mel_ev_t e[MJ_KMAX];
    static const int STEP[4] = {0, 8, 4, 2}, HARD[4] = {120, 108, 96, 84};
    mjam_note_t line[SEQ_STEPS];
    int ok = 1, n = 0;
    seq_init(&s, emit, NULL);   /* ARP UP: a note on every even step */
    seq_set_mel_acc(&s, 3);
    nev = 0;
    play_stop(&s, 3.95);
    ok = mel_evs(&s, e);
    for (int i = 0; i < 4; i++) ok &= e[STEP[i]].vel == HARD[i] && s.part[0].lane[LANE_MEL_UP].steps[STEP[i]].vel == HARD[i];
    CHECK(ok, "ACCENT HARD: steps 1, 9, 5, 3 at 120, 108, 96, 84 (%d %d %d %d), baked in the steps", e[0].vel, e[8].vel, e[4].vel, e[2].vel);
    seq_set_mel_acc(&s, 0);
    nev = 0;
    play_stop(&s, 3.95);
    ok = mel_evs(&s, e);
    for (int k = 0; k < SEQ_STEPS; k++)
        if (e[k].vel) ok &= e[k].vel == 96, n++;
    CHECK(ok && n == 8, "ACCENT EVEN: all 96 (%d notes)", n);
    seq_mel_queue(&s, MJ_ARP, 3);   /* ALBERTI (stopped: at once), with 16th notes */
    seq_set_mel_acc(&s, 3);
    seq_mel_line(&s, 0, line);
    nev = 0;
    play_stop(&s, 3.95);
    ok = mel_evs(&s, e);
    CHECK(ok && line[0].note && line[1].note && e[1].vel == 72 && e[0].vel == 120, "ALBERTI at HARD: the 16th note on step 2 at 72 (%d)", e[1].vel);
    seq_set_dynamics(&s, 0);
    nev = 0;
    play_stop(&s, 3.95);
    mel_evs(&s, e);
    CHECK(e[0].vel == 48, "PLAY's DYNAMICS 0 still scales it in note_on: 120 x 0.4 = 48 (%d)", e[0].vel);
}

/* 0.21 one pass of the loop (and a beat after, so every note-off comes) at Melody SWING sw, against SWING 0: 1 when the loop's grid is
 * g16, each note on the grid (g16: an odd step, else p mod 4 = 2) goes out d beats after its step and every other note on its step,
 * with its note, velocity and length (within 2 blocks) as at SWING 0, and none hangs; a late note the next step retriggers (its pitch
 * again) ends there, as any retrigger does; *moved, *kept: how many of each */
static int swing_pass(seq_t *s, int sw, int g16, double d, int *moved, int *kept) {
    static mel_ev_t ref[MJ_KMAX], e[MJ_KMAX];
    int n = seq_bars(s) * SEQ_STEPS, ok;
    *moved = *kept = 0;
    seq_set_mel_swing(s, 0);
    nev = 0;
    play_stop(s, n / 4.0 + 1.0);
    ok = mel_evs(s, ref);
    seq_set_mel_swing(s, sw);
    nev = 0;
    play_stop(s, n / 4.0 + 1.0);
    ok &= mel_evs(s, e) && balanced() && seq_mel_grid16(s) == g16 && seq_mel_swing(s) == sw;
    for (int k = 0; k < n; k++) {
        int late = g16 ? k & 1 : k % 4 == 2;
        if (!ref[k].vel) { ok &= !e[k].vel; continue; }
        ok &= e[k].note == ref[k].note && e[k].vel == ref[k].vel && at_t(e[k].on, k / 4.0 + (late ? d : 0)) && e[k].len > 0 &&
              (fabs(e[k].len - ref[k].len) < 2 * blk() || (e[k + 1].note == e[k].note && at_t(e[k].on + e[k].len, (k + 1) / 4.0)));
        *moved += late, *kept += !late;
    }
    return ok;
}

/* 0.21 SWING on a 16th loop (ALBERTI): at 70% each odd step's note-on 0.1 beat late (queued: it really sounds late), the even steps
 * where they were; lengths and velocities unchanged */
static void test_21_swing_16(void) {
    static seq_t s;
    int moved, kept, ok;
    seq_init(&s, emit, NULL);
    seq_mel_queue(&s, MJ_ARP, 3);
    ok = swing_pass(&s, 5, 1, 0.1, &moved, &kept);
    CHECK(ok && moved >= 8 && kept >= 8, "ALBERTI, SWING 70%%: %d odd steps 0.1 beat late, %d even ones on time, lengths and velocities unchanged", moved, kept);
    ok = swing_pass(&s, 2, 1, 0.04, &moved, &kept);
    CHECK(ok && moved >= 8, "SWING 58%%: 0.04 beat late");
}

/* 0.21 SWING on an 8th loop (ARP UP, no odd notes): at 70% steps with p mod 4 = 2 0.2 beat late, the others where they were */
static void test_21_swing_8(void) {
    static seq_t s;
    int moved, kept, ok;
    seq_init(&s, emit, NULL);
    ok = swing_pass(&s, 5, 0, 0.2, &moved, &kept);
    CHECK(ok && moved >= 8 && kept >= 8, "ARP UP, SWING 70%%: %d off-beat 8ths 0.2 beat late, %d beats on time", moved, kept);
}

/* 0.21 final review: on the 8th grid a swung note never comes nearer than 1/8 beat to the line's next note. ARP UP with FILL RUN (LOOP: the
 * last bar's steps 13-16 are 16th notes, which never set the grid) at SWING 70%: bar 4 step 15 at song time 15.625, not 15.7 (step 16 is at 15.75); the
 * other off-beat 8ths still 0.2 beat late */
static void test_21_swing_8_cap(void) {
    static seq_t s;
    static mel_ev_t e[MJ_KMAX];
    mjam_note_t line[SEQ_STEPS];
    int n, ok, late = 0, b = 3;
    seq_init(&s, emit, NULL);
    seq_mjam_set(&s, MJS_FILL, 3);
    seq_set_mel_swing(&s, 5);
    n = seq_bars(&s) * SEQ_STEPS;
    seq_mel_line(&s, b, line);
    nev = 0;
    play_stop(&s, n / 4.0 + 1.0);
    ok = mel_evs(&s, e) && balanced() && seq_mel_grid16(&s) == 0 && seq_bars(&s) == 4 && line[12].note && line[13].note && line[14].note && line[15].note;
    for (int k = 0; k < b * SEQ_STEPS; k++)
        if (e[k].vel && k % 4 == 2) ok &= at_t(e[k].on, k / 4.0 + 0.2), late++;
    CHECK(ok && late >= 8 && at_t(e[62].on, 15.625) && at_t(e[63].on, 15.75) && e[63].on - e[62].on >= 0.125 - blk() - 1e-9,
          "ARP UP + FILL RUN, SWING 70%% (8th grid): bar 4 step 15 at %.3f, 1/8 beat before step 16 at %.3f (not 15.700); %d off-beat 8ths 0.2 late",
          e[62].on, e[63].on, late);
}

/* 0.21 Review Focus: with PLAY's SWING on, a Melody odd 16th takes the larger offset, never their sum; with PUSH 100 the note-ons stay in
 * time order across the bar: step 16 at 3.85 sounds before bar 2's pushed first note at 3.875 */
static void test_21_swing_global(void) {
    static seq_t s;
    mjam_note_t b1[SEQ_STEPS], b2[SEQ_STEPS];
    double want[SEQ_STEPS + 1];
    int moved, kept, ok, ch, nw = 0, i = 0;
    seq_init(&s, emit, NULL);
    seq_mel_queue(&s, MJ_ARP, 3);
    seq_set_swing(&s, 100);
    ok = swing_pass(&s, 1, 1, 1.0 / 12, &moved, &kept);
    CHECK(ok && moved >= 8, "PLAY SWING 100 (1/12 beat), Melody 54%% (0.02): odd 16ths at +1/12, the larger (%d)", moved);
    ok = swing_pass(&s, 5, 1, 0.1, &moved, &kept);
    CHECK(ok && moved >= 8, "PLAY SWING 100, Melody 70%% (0.1): odd 16ths at +0.1, not the sum 0.183 (%d)", moved);
    seq_set_mel_push(&s, 100);
    seq_mel_line(&s, 0, b1);
    seq_mel_line(&s, 1, b2);
    for (int k = 0; k < SEQ_STEPS; k++)
        if (b1[k].note) want[nw++] = k / 4.0 + (k & 1 ? 0.1 : 0);
    if (b2[0].note) want[nw++] = 4.0 - 0.125;
    ch = seq_channel_of(&s, LANE_MEL_UP);
    nev = 0;
    play_stop(&s, 3.95);
    ok = b1[15].note && b2[0].note && balanced();
    for (int e = 0; e < nev; e++)
        if (is_on(&evs[e]) && (evs[e].status & 15) == ch - 1) ok &= i < nw && at_t(evs[e].ppq, want[i++]);
    CHECK(ok && i == nw, "and PUSH 100: bar 1's %d Melody note-ons in time order, each at its time, step 16 at 3.85 before bar 2's pushed note at 3.875", i);
}

/* 0.21 Melody note-ons logged since nev = 0 by the step that fired them */
static void mel_count_k(const seq_t *s, int cnt[MJ_KMAX]) {
    int ch = seq_channel_of(s, LANE_MEL_UP);
    memset(cnt, 0, sizeof(int) * MJ_KMAX);
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && evs[i].k >= 0 && evs[i].k < MJ_KMAX) cnt[evs[i].k]++;
}

/* 0.21 Review Focus: a swung note waiting in the queue (step j fired, its note due 0.1 beat later) is dropped at a host loop wrap
 * before it, a mute, a stop and a locate, never played late or twice; a part switch and a host loop of a 1-bar part with PUSH play
 * every step once. No note hangs: every note-on has its note-off (balanced, per channel and pitch). */
static void test_21_swing_edges(void) {
    static seq_t s;
    static int cnt[MJ_KMAX];
    static mel_ev_t e[MJ_KMAX];
    mjam_note_t line[SEQ_STEPS];
    int j = 0, ok = 1, q = 0, n = 0, total = 0;
    double L, ppq;
    seq_init(&s, emit, NULL);
    seq_mel_queue(&s, MJ_ARP, 3);   /* ALBERTI: 16th notes */
    seq_set_mel_swing(&s, 5);
    seq_mel_line(&s, 0, line);
    for (int i = 5; i < SEQ_STEPS && !j; i += 2)
        if (line[i].note) j = i;
    CHECK(j && line[15].note, "ALBERTI has a note on an odd step from step 6 on (step %d) and on step 16", j + 1);
    if (!j || !line[15].note) return;
    L = j / 4.0 + 0.05;   /* a host loop ending 0.05 beat after step j fires, before its note is due */
    nev = 0;
    ppq = play(&s, 0.0, 3 * L - 2 * blk(), L);
    seq_block(&s, ppq, bpm, 0, 128);
    mel_count_k(&s, cnt);
    for (int i = 0; i < j; i++) ok &= cnt[i] == (line[i].note ? 3 : 0);
    CHECK(ok && cnt[j] == 0 && balanced(), "a host loop wrap at %.2f beats: step %d's waiting note is dropped each pass, steps 1..%d once a pass, none hangs", L, j + 1, j);
    nev = 0;
    ppq = play_to_k(&s, 0.0, j + 1);   /* step j fired: its note waits */
    for (int i = 0; i < s.nstrum; i++) q += s.strum[i].lane == LANE_MEL_UP;
    seq_set_mute(&s, LANE_MEL_UP, 1);
    ppq = play(&s, ppq, 1.0, 0);
    seq_set_mute(&s, LANE_MEL_UP, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    mel_count_k(&s, cnt);
    CHECK(q == 1 && cnt[j] == 0 && balanced(), "a mute while step %d's note waits in the queue (%d) drops it; none hangs", j + 1, q);
    nev = 0;
    ppq = play_to_k(&s, 0.0, j + 1);
    seq_block(&s, ppq, bpm, 0, 128);
    mel_count_k(&s, cnt);
    CHECK(cnt[j] == 0 && s.nstrum == 0 && balanced(), "a stop while it waits drops it");
    nev = 0;
    ppq = play_to_k(&s, 0.0, j + 1);
    ppq = play(&s, 8.0, 1.0, 0);   /* a locate forward: resync */
    seq_block(&s, ppq, bpm, 0, 128);
    ok = mel_evs(&s, e);
    mel_count_k(&s, cnt);
    CHECK(ok && cnt[j] == 0 && (!line[0].note || at_t(e[0].on, 0)) && balanced(), "a locate forward while it waits drops it (no late note); bar 3 plays on");
    nev = 0;
    ppq = play_to_k(&s, 0.0, 6);
    seq_select_part(&s, 1);   /* playing: queued, at bar 2 */
    ppq = play(&s, ppq, 6.0, 0);
    seq_block(&s, ppq, bpm, 0, 128);
    ok = mel_evs(&s, e);
    CHECK(ok && seq_part(&s) == 1 && at_t(e[15].on, 3.85) && e[16].vel && balanced(),
          "a part switch at bar 2: step 16's note at 3.85 before it, no step twice, none hangs");
    seq_init(&s, emit, NULL);
    seq_mel_queue(&s, MJ_ARP, 3);
    seq_set_bars(&s, 1);
    seq_set_mel_swing(&s, 5);
    seq_set_mel_push(&s, 100);
    seq_mel_line(&s, 0, line);
    for (int i = 0; i < SEQ_STEPS; i++) n += line[i].note != 0;
    nev = 0;
    ppq = play(&s, 0.0, 12.0 - 2 * blk(), 4.0);   /* a host loop of the 1-bar part: PUSH's first note kept over each wrap */
    seq_block(&s, ppq, bpm, 0, 128);
    mel_count_k(&s, cnt);
    for (int k = 0; k < MJ_KMAX; k++) total += cnt[k];
    CHECK(line[0].note && total == 3 * n + 1 && balanced(), "a 4-beat host loop of a 1-bar part, PUSH 100, SWING 70%%: %d note-ons (3 x %d + the next pass's pushed first), none hangs",
          total, n);
}

/* 0.19.1 the drawn-lane table (DLANE) is a refactor: every old project, a v1 song, a 0.19 save with every Bass field and the
 * hostile fields above load, re-save, render and grow to 16 bars exactly as 0.19.0 did. DLANE_LOAD_FNV was computed at the
 * commit before the table (FNV-1a over each fixture's saved string and its Chords and Bass steps, after the load and again after
 * the grow). */
#define DLANE_LOAD_FNV 0x672be066u
static unsigned dlane_hash(unsigned h, seq_t *t) {
    static char buf[SEQ_STATE_MAX];
    int n = seq_state_save(t, buf, sizeof buf);
    for (int i = 0; i < n; i++) h = fnv(h, (unsigned char)buf[i]);
    for (int l = LANE_CHORDS; l <= LANE_BASS; l++)
        for (int i = 0; i < t->part[0].bars * SEQ_STEPS; i++) {
            const seq_step_t *x = &t->part[0].lane[l].steps[i];
            unsigned char b[6] = {x->on, x->tone, (unsigned char)x->oct, x->vel, x->len, x->abs};
            for (int j = 0; j < 6; j++) h = fnv(h, b[j]);
        }
    return h;
}
static unsigned dlane_load(unsigned h, const char *st) {
    static seq_t t;
    seq_init(&t, emit, NULL);
    h = fnv(h, (unsigned char)seq_state_load(&t, st));
    h = dlane_hash(h, &t);
    seq_set_bars(&t, 16);
    return dlane_hash(h, &t);
}
static void test_191_dlane_load_same(void) {
    static const int STYLES[4] = {GEN_BASS_ROOT, GEN_BASS_PUMP, GEN_BASS_WALK, GEN_MANUAL}, CL[2] = {16, 8}, BARS[2] = {2, 16};
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50|A2=0:|A2.g=18,1,50";
    static const char *const TAILS[4] = {"|A2.rb=zz|A2.bx=99,9,9|A2.bl=40:x;99:1.1.1.1.1.1.1|A3=0:|A4=0:",
                                         "|A2.bx=99,9,9,9,9,9,9|A2.bl=1:99.9.-9.9.9.9.9;x:1;3:1.1;;2:|A2.rz=9,-9,2,7|A3=0:|A4=0:",
                                         "|A2.ab=0021zz00ff7f01800225f|A2.bx=1,2|A2.bl=0:-2.-1.0.-1.0.0.-9|A3=0:|A4=0:",
                                         "|A2.rb=L12,Axgx............,3,99|A3=0:|A4=0:"};
    static const char G[] = "csq2|gv=1|k=4|s=1|A.len=2|A.cl=8|A.h=55,321|A.prog=0352|A.voi=1|ch=2,3,4"
                            "|A1=0:0515640|A1.g=3,4242,55|A1.lk=0200000000000000" Z16 Z16 Z16
                            "|A2=0:0354501|A2.g=6,777,80|A2.lk=8000100000000000" Z16 Z16 Z16 "|A3=0:|A4=0:";
    static seq_t s;
    static char buf[SEQ_STATE_MAX], st[512];
    unsigned h = 2166136261u;
    int n = 0;
    for (int k = 0; k < 4; k++)   /* test_19_old_bass_converts's 0.18 Bass lanes */
        for (int c = 0; c < 2; c++)
            for (int b = 0; b < 2; b++) {
                seq_init(&s, emit, NULL);
                old_mel(&s);   /* 0.20: the Melody as before the drawn-lane table (0.19's Hook) */
                seq_set_bars(&s, BARS[b]);
                seq_set_chord_len(&s, CL[c]);
                seq_lane_t *ln = &s.part[0].lane[LANE_BASS];
                if (STYLES[k] == GEN_MANUAL) {
                    s.part[0].lane_shape[LANE_BASS][BSHAPE_MOVEMENT] = 100;
                    memset(ln, 0, sizeof *ln);
                    ln->gen = (seq_gen_t){GEN_MANUAL, 1, 30, 0};
                    for (int bar = 0; bar < BARS[b]; bar++) {
                        ln->steps[bar * SEQ_STEPS] = (seq_step_t){1, TONE_R, -1, 112, 4, 0};
                        ln->steps[bar * SEQ_STEPS + 6] = (seq_step_t){1, TONE_5, -1, 100, 2, 0};
                        ln->steps[bar * SEQ_STEPS + 11] = (seq_step_t){1, TONE_R, 0, 60, 1, 0};
                    }
                } else {
                    ln->gen = (seq_gen_t){STYLES[k], 7, 41, 0};
                    ln->bar_var[1] = 2;
                    if (STYLES[k] == GEN_BASS_ROOT) seq_set_lane_shape(&s, LANE_BASS, BSHAPE_SYNC, 40), seq_set_lane_shape(&s, LANE_BASS, BSHAPE_ACCENT, 70);
                    if (STYLES[k] == GEN_BASS_PUMP)
                        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCTAVES, 100), seq_set_lane_shape(&s, LANE_BASS, BSHAPE_REGISTER, 25),
                            seq_set_lane_shape(&s, LANE_BASS, BSHAPE_OCT, 1);
                    if (STYLES[k] == GEN_BASS_WALK) seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1);
                    seq_set_complexity(&s, LANE_BASS, 40);
                }
                seq_set_step_note(&s, LANE_BASS, 1, 0, 45);
                n += seq_state_save(&s, buf, sizeof buf) > 0;
                h = dlane_load(h, buf);
            }
    h = dlane_load(h, G);   /* STATE_GOLDEN's */
    h = dlane_load(h, OLD_STATES[1]);   /* 0.5 */
    h = dlane_load(h, "csq1:1000100010001010" "1000000010000000" "1010101010101010" "1010101010101010" ":0110");
    for (int k = 0; k < 4; k++) {   /* test_19_bad_fields's hostile 0.19 fields */
        snprintf(st, sizeof st, "%s%s", HEAD, TAILS[k]);
        h = dlane_load(h, st);
    }
    seq_init(&s, emit, NULL);   /* a 0.19 save: both lanes' bars, knobs and every Bass field */
    old_mel(&s);
    seq_set_bars(&s, 4);
    seq_part_t *p = &s.part[0];
    rhythm_parse("AgxgAgxgAgxgAgx-", p->rb[DL_BASS][1].c);
    p->rb[DL_BASS][1].ed = 1, p->rb[DL_BASS][2].lock = 1, p->rb[DL_CHORDS][3].from = 5;
    rhythm_lib_codes(RLIB_CHORDS, 5, p->rb[DL_CHORDS][3].c);
    p->bass.frozen[2] = (bass_set_t){-1, 2, -1, {4, 4, 4, -2}};
    p->bass.nlock = 1 << 2;
    p->bass.fix[SEQ_STEPS + 4] = 40, p->bass.fix[3 * SEQ_STEPS] = 52;
    p->lane[LANE_BASS].bar_var[3] = 2;
    p->bass.set = (bass_set_t){12, 1, 1, {3, 2, 1, -1}};
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 3), seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 2);
    seq_set_rknob(&s, LANE_BASS, RK_SYNC, 2), seq_set_rknob(&s, LANE_BASS, RK_HUM, 3), seq_set_rknob(&s, LANE_BASS, RK_FILL, 4);
    render_now(&s);
    n += seq_state_save(&s, buf, sizeof buf) > 0;
    h = dlane_load(h, buf);
    CHECK(n == 17 && h == DLANE_LOAD_FNV, "16 old Bass lanes, STATE_GOLDEN's, a 0.5 and a v1 song, 4 hostile and a full 0.19 save load, "
          "re-save and grow as before the drawn-lane table: 0x%08x (got 0x%08x)", DLANE_LOAD_FNV, h);
}

/* 0.19.1 (spec 6) a BASS RHYTHM bar or the bar editor holds the BASS tab's centre on the bar (the tab's centre is a bar): at 2-bar and
 * at 1/4-bar chords too (0.19.0 stored the bar's first chord slot); DONE leaves it */
static void test_191_snap_centre_bar(void) {
    static seq_t s;
    static const int CL[2] = {32, 4};
    for (int c = 0; c < 2; c++) {
        seq_init(&s, emit, NULL);
        seq_set_bars(&s, 8);
        seq_set_chord_len(&s, CL[c]);
        seq_rbar_open(&s, LANE_BASS, 5);
        int held = s.tv[TAB_BASS].snap_centre == 5 && seq_centre_slot(&s, TAB_BASS) == 5;
        seq_rbar_done(&s, LANE_BASS);
        seq_rbar_open(&s, LANE_BASS, 6);
        int edit = s.tv[TAB_BASS].snap_centre == 6;
        seq_rbar_done(&s, LANE_BASS);
        CHECK(held && edit && s.tv[TAB_BASS].snap_centre == 6, "%s chords: BASS RHYTHM bar 6 open, the BASS centre is bar 6; the editor "
              "on bar 7, bar 7; DONE leaves it (snap_centre %d)", c ? "1/4-bar" : "2-bar", s.tv[TAB_BASS].snap_centre);
    }
}

/* 0.19.1 (0.19.0 review) UNDO with the STYLE browser open after two tries and a knob turn: the browser closes as CANCEL (the settings as
 * at open, no undo step), the turned knob as turned */
static void test_191_style_undo_as_cancel(void) {
    static seq_t s;
    bass_set_t want;
    seq_init(&s, emit, NULL);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    want = s.part[0].bass.set;
    int n = s.undo_n;
    seq_bstyle_open(&s);
    seq_bstyle_try(&s, 36);   /* Funk Thirds */
    seq_bstyle_try(&s, 5);
    seq_set_bknob(&s, BK_OCTS, 3);
    want.k[BK_OCTS] = 3;
    seq_undo(&s);
    CHECK(seq_bview(&s) == BVIEW_BASS && seq_follow_paused(&s, TAB_BASS) == 0 && bset_is(&s, want) && bass_all_ref(&s) && s.undo_n == n,
          "UNDO after 2 tries and a turn: closed as CANCEL, the settings as at open, OCTAVES as turned (3), no undo step");
}

/* 0.19.1 (0.19.0 review) the bar editor on bar 12 of 16 is cancelled when the loop shrinks to 8 (its fixed note goes); open on bar 3,
 * a chord length change closes it as DONE (its fixed note stays) */
static int bar_fixed(const seq_t *s, int bar) {
    int n = 0;
    for (int i = 0; i < SEQ_STEPS; i++) n += s->part[0].bass.fix[bar * SEQ_STEPS + i] != 0;
    return n;
}
static void test_191_editor_closes(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths */
    seq_set_bars(&s, 16);
    seq_rbar_open(&s, LANE_BASS, 11);
    seq_rbar_note(&s, 2);
    int fixed = bar_fixed(&s, 11);
    seq_set_bars(&s, 8);
    CHECK(fixed == 1 && seq_rbar_bar(&s, LANE_BASS) < 0 && seq_bview(&s) == BVIEW_BASS && !bar_fixed(&s, 11) && s.part[0].bars == 8,
          "the editor on bar 12 of 16, a note fixed, the loop to 8: cancelled (the note gone)");
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_note(&s, 2);
    fixed = bar_fixed(&s, 2);
    seq_set_chord_len(&s, 32);
    CHECK(fixed == 1 && seq_rbar_bar(&s, LANE_BASS) < 0 && seq_bview(&s) == BVIEW_BASS && bar_fixed(&s, 2) == 1 && s.part[0].chord_len == 32,
          "the editor on bar 3, a note fixed, the chord length to 2 bars: closed as DONE (the note kept)");
}

/* 0.19.1 (0.19.0 review) hostile `2.bl`, `2.rb` and `2.ab` strings load (never refused) and read nothing past their ends (ASan): a
 * `2.bl` entry over 40 characters, "5:" and "3:" (no settings), ":1.1.1" (no bar); 20 `2.rb` entries; an odd-length `2.ab` */
static void test_191_hostile_strings(void) {
    static seq_t t;
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50|A2=0:|A2.g=18,1,50";
    static char st[1024];
    const seq_part_t *q = &t.part[0];
    snprintf(st, sizeof st, "%s|A2.bl=1:1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1.1;5:;3:;:1.1.1;2:3.1.0.1.1.1.0|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && q->bass.nlock == 1 << 2 && q->bass.frozen[2].style == 3, "2.bl: a 45-character entry, '5:', '3:' "
          "and ':1.1.1' are skipped; bar 3's good entry after them loads (nlock 0x%x)", q->bass.nlock);
    snprintf(st, sizeof st, "%s|A2.rb=3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,5,5,5,5|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    int all = seq_state_load(&t, st);
    for (int b = 0; b < 4; b++) all &= q->rb[DL_BASS][b].from == 3;
    CHECK(all, "20 2.rb entries load: the loop's 4 bars Eighths (the entries past 16 ignored)");
    snprintf(st, sizeof st, "%s|A2.ab=0021022|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && q->bass.fix[0] == 33 && !q->bass.fix[2], "an odd-length 2.ab (7 characters) loads its whole pair "
          "(step 1 A1) and ignores the rest");
}

/* 0.22 a JAM index (RJAM_BASE + genre*5 + level) saves and loads as an index, per lane; an unknown one keeps its saved codes or
 * becomes a rest bar; the lookups by `from` give the block's name */
static void test_22_jam_index(void) {
    static seq_t t;
    static char st[512], buf[8192];
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50";
    unsigned char want[16];
    int ok = 1, ed;
    for (int from = RJAM_BASE; from < RJAM_BASE + 30; from++) {
        snprintf(st, sizeof st, "%s|A.rb=%d,L%d,%d,%d|A2=0:|A3=0:|A4=0:", HEAD, from, from, from, from);
        seq_init(&t, emit, NULL);
        ok &= seq_state_load(&t, st);
        rhythm_lib_codes(RLIB_CHORDS, from, want);
        const rbar_t *r = t.part[0].rb[DL_CHORDS];
        ok &= r[0].from == from && !r[0].ed && !memcmp(r[0].c, want, 16) && r[1].lock && r[1].from == from;
        ok &= seq_bar_rhythm(&t, LANE_CHORDS, 0, &ed) == from && !ed;
        ok &= seq_state_save(&t, buf, sizeof buf) > 0;
        char exp[64];
        snprintf(exp, sizeof exp, "|A.rb=%d,L%d,%d,%d", from, from, from, from);
        ok &= strstr(buf, exp) != NULL;
    }
    CHECK(ok, "JAM indexes 64..93 load, read back and save as an index");
    CHECK(strcmp(rhythm_lib_name(RLIB_CHORDS, RJAM_BASE), jam_block_name(JL_CHORDS, 0, 0)) == 0 &&
          strcmp(rhythm_lib_name(RLIB_BASS, RJAM_BASE + 7), jam_block_name(JL_BASS, 1, 2)) == 0, "the library lookups give the block's name per lane");
    snprintf(st, sizeof st, "%s|A.rb=94,L95,96x...x...x...x...,97A---------------|A2=0:|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    const rbar_t *r = t.part[0].rb[DL_CHORDS];
    unsigned char rest[16] = {0};
    CHECK(seq_state_load(&t, st) && r[0].from == -1 && r[0].ed && !memcmp(r[0].c, rest, 16) && r[1].from == -1 && r[1].ed && r[1].lock && !memcmp(r[1].c, rest, 16),
          "an unknown JAM index without codes loads a rest bar");
    snprintf(st, sizeof st, "%s|A.rb=94x...x...x...x...,L95A---------------|A2=0:|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    rhythm_parse("x...x...x...x...", want);
    CHECK(seq_state_load(&t, st) && r[0].from == -1 && r[0].ed && !memcmp(r[0].c, want, 16) && r[1].lock && r[1].c[0] == RC_ACC,
          "an unknown JAM index with saved codes loads the codes");
}

/* 0.22 Task 3: the lane JAM state, applying a block, TIME / VARIATION at render, saving */

/* the bar's codes as they render back from its steps (rhythm_to_steps then rhythm_from_steps: what seq_bar_plays reads) */
static void plays_as(const unsigned char c[16], unsigned char out[16]) {
    seq_step_t st[16];
    rhythm_to_steps(c, st);
    rhythm_from_steps(st, out);
}

/* 1 when drawn lane `lane`'s bar plays codes c (at COMPLEXITY 50, every RHYTHM knob 0: as drawn) */
static int bar_plays_codes(const seq_t *s, int lane, int bar, const unsigned char c[16]) {
    unsigned char got[16], want[16];
    seq_bar_plays(s, lane, bar, got);
    plays_as(c, want);
    return !memcmp(got, want, 16);
}

static bass_set_t jam_bass_set_of(int c, int l) {
    const jam_bass_t *j = jam_bass(c, l);
    return (bass_set_t){-1, j->pedal, j->octave, {j->mov, j->octs, j->app, j->reg}};
}

/* the part's Chords voicing is block (c, l)'s, reading Custom */
static int chords_voicing_is(const seq_t *s, int c, int l) {
    const jam_chords_t *j = jam_chords(c, l);
    const seq_part_t *p = &s->part[s->cur_part];
    return seq_voicing_style(s) == -1 && p->vmode == j->mode && seq_vknob(s, VK_COL) == j->col && seq_vknob(s, VK_INV) == j->inv &&
           seq_vknob(s, VK_SPR) == j->spr && seq_vknob(s, VK_STR) == j->str && p->lane_shape[LANE_CHORDS][SHAPE_OCT] == j->oct;
}

static void test_22_apply(void) {
    static seq_t s;
    const seq_part_t *p = &s.part[0];
    unsigned char k[16];
    int ok = 1, c = -9, l = -9;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    CHECK(!seq_jam_playing(&s, LANE_CHORDS, NULL, NULL) && !seq_jam_playing(&s, LANE_BASS, NULL, NULL) && !seq_jam_edited(&s, LANE_CHORDS) &&
          p->djam[DL_CHORDS].c == -1 && p->djam[DL_BASS].sw == 1 && !seq_can_undo(&s), "a new instance: no block on either lane, SWITCH AT BAR");
    seq_jam_apply_now(&s, LANE_CHORDS, 1, 1);   /* TECHNO DUB STAB */
    rhythm_parse(jam_block_codes(JL_CHORDS, 1, 1), k);
    for (int b = 0; b < 8; b++)
        ok &= p->rb[DL_CHORDS][b].from == RJAM_BASE + 6 && !p->rb[DL_CHORDS][b].ed && !memcmp(p->rb[DL_CHORDS][b].c, k, 16) && bar_plays_codes(&s, LANE_CHORDS, b, k);
    CHECK(ok && chords_voicing_is(&s, 1, 1) && seq_jam_playing(&s, LANE_CHORDS, &c, &l) && c == 1 && l == 1 && !seq_jam_edited(&s, LANE_CHORDS) &&
          seq_can_undo(&s), "CHORDS DUB STAB: every bar its codes as index 70 and plays them; the voicing is the block's, Custom; one undo step");
    seq_jam_apply_now(&s, LANE_BASS, 4, 4);   /* ROCK GALLOP */
    rhythm_parse(jam_block_codes(JL_BASS, 4, 4), k);
    bass_set_t want = jam_bass_set_of(4, 4);
    ok = 1;
    for (int b = 0; b < 8; b++) ok &= p->rb[DL_BASS][b].from == RJAM_BASE + 24 && !memcmp(p->rb[DL_BASS][b].c, k, 16) && bar_plays_codes(&s, LANE_BASS, b, k);
    CHECK(ok && !memcmp(&p->bass.set, &want, sizeof want) && seq_bass_style(&s) == -1 && seq_jam_playing(&s, LANE_BASS, &c, &l) && c == 4 && l == 4 &&
          !p->bass.nlock, "BASS GALLOP: every bar index 88 and its codes; the note settings the block's, Custom; nothing note-locked");
    int pushes = s.undo_pushes;
    seq_jam_apply_now(&s, LANE_BASS, 4, 4);
    seq_jam_apply_now(&s, LANE_BASS, 6, 0), seq_jam_apply_now(&s, LANE_BASS, 0, 5), seq_jam_apply_now(&s, LANE_MEL_UP, 0, 0);
    CHECK(s.undo_pushes == pushes && p->djam[DL_BASS].c == 4, "the block as it plays again, a bad block or another lane: nothing (no undo step)");
}

/* Review Focus: a tap over hand-edited and locked bars (a BASS rhythm-only lock among them) keeps the locked bars' rhythm and notes;
 * UNDO puts every unlocked bar back, one step per tap */
static void test_22_locks_and_undo(void) {
    static seq_t s;
    static seq_part_t was;
    seq_part_t *p = &s.part[0];
    unsigned char e[16], kc[16], kb[16];
    int ok = 1;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 4);
    rhythm_parse("A.x.gx..x-..x.g.", e);
    memcpy(p->rb[DL_CHORDS][1].c, e, 16), p->rb[DL_CHORDS][1].ed = 1;   /* hand-edited, unlocked */
    memcpy(p->rb[DL_CHORDS][2].c, e, 16), p->rb[DL_CHORDS][2].ed = 1, p->rb[DL_CHORDS][2].lock = 1;   /* hand-edited, locked */
    memcpy(p->rb[DL_BASS][0].c, e, 16), p->rb[DL_BASS][0].ed = 1, p->rb[DL_BASS][0].lock = 1;   /* rhythm-only lock */
    memcpy(p->rb[DL_BASS][3].c, e, 16), p->rb[DL_BASS][3].ed = 1;   /* note lock only */
    p->bass.frozen[3] = (bass_set_t){-1, 2, -1, {3, 2, 1, -1}}, p->bass.nlock = 1 << 3;
    p->bass.fix[2 * 16 + 4] = 45;
    seq_set_vknob(&s, VK_INV, 1);
    render_now(&s);
    was = *p;
    seq_jam_apply_now(&s, LANE_CHORDS, 2, 3);   /* TRANCE GATE */
    seq_jam_apply_now(&s, LANE_BASS, 5, 2);     /* DISCO */
    rhythm_parse(jam_block_codes(JL_CHORDS, 2, 3), kc);
    rhythm_parse(jam_block_codes(JL_BASS, 5, 2), kb);
    for (int b = 0; b < 4; b++) ok &= b == 2 ? !memcmp(&p->rb[DL_CHORDS][b], &was.rb[DL_CHORDS][b], sizeof p->rb[0][0]) : !memcmp(p->rb[DL_CHORDS][b].c, kc, 16);
    CHECK(ok && !memcmp(&p->lane[LANE_CHORDS].steps[32], &was.lane[LANE_CHORDS].steps[32], 16 * sizeof(seq_step_t)),
          "CHORDS: the locked bar 3 keeps its codes, lock and steps; bars 1, 2, 4 (the edited one too) take the block's");
    ok = 1;
    for (int b = 0; b < 4; b++)
        ok &= b == 0 || b == 3 ? !memcmp(&p->rb[DL_BASS][b], &was.rb[DL_BASS][b], sizeof p->rb[0][0]) &&
                                     !memcmp(&p->lane[LANE_BASS].steps[b * 16], &was.lane[LANE_BASS].steps[b * 16], 16 * sizeof(seq_step_t))
                               : !memcmp(p->rb[DL_BASS][b].c, kb, 16);
    CHECK(ok && p->bass.nlock == (1 << 0 | 1 << 3) && !memcmp(&p->bass.frozen[0], &was.bass.set, sizeof was.bass.set) &&
          !memcmp(&p->bass.frozen[3], &was.bass.frozen[3], sizeof was.bass.frozen[3]) && p->bass.fix[2 * 16 + 4] == 45,
          "BASS: the rhythm-locked bar 1 and note-locked bar 4 keep codes and notes (bar 1 note-locked at the old settings); the fix stays");
    seq_undo(&s);
    CHECK(!memcmp(p->rb[DL_BASS], was.rb[DL_BASS], sizeof was.rb[DL_BASS]) && !memcmp(&p->bass, &was.bass, sizeof was.bass) &&
          !memcmp(p->lane[LANE_BASS].steps, was.lane[LANE_BASS].steps, 64 * sizeof(seq_step_t)) && p->djam[DL_BASS].c == -1 &&
          p->djam[DL_CHORDS].c == 2, "UNDO: the Bass tap goes whole (bars, settings, the auto note-lock, steps, no block); the Chords one stays");
    seq_undo(&s);
    CHECK(!memcmp(p->rb[DL_CHORDS], was.rb[DL_CHORDS], sizeof was.rb[DL_CHORDS]) && p->vstyle == was.vstyle && p->vmode == was.vmode &&
          p->vcol == was.vcol && p->vinv == was.vinv && p->vspr == was.vspr && p->vstrdir == was.vstrdir &&
          !memcmp(p->lane_shape[LANE_CHORDS], was.lane_shape[LANE_CHORDS], sizeof was.lane_shape[0]) &&
          !memcmp(p->voice, was.voice, sizeof was.voice) && !memcmp(p->lane[LANE_CHORDS].steps, was.lane[LANE_CHORDS].steps, 64 * sizeof(seq_step_t)) &&
          p->djam[DL_CHORDS].c == -1 && !seq_can_undo(&s), "UNDO again: the Chords tap goes in one step (bars, voicing, voices, steps, no block); nothing left");
}

static void test_22_bass_rhythm_lock_autolocks(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    bass_set_t old, both = {-1, 1, 0, {1, 1, 1, 1}};
    seq_init(&s, emit, NULL);
    p->rb[DL_BASS][1].lock = 1;   /* rhythm-only */
    p->rb[DL_BASS][2].lock = 1, p->bass.frozen[2] = both, p->bass.nlock = 1 << 2;   /* rhythm and notes */
    render_now(&s);
    old = p->bass.set;
    seq_step_t st1[16];
    memcpy(st1, &p->lane[LANE_BASS].steps[16], sizeof st1);
    seq_jam_apply_now(&s, LANE_BASS, 1, 4);   /* TECHNO ACID */
    CHECK(p->bass.nlock == (1 << 1 | 1 << 2) && !memcmp(&p->bass.frozen[1], &old, sizeof old) && !memcmp(&p->bass.frozen[2], &both, sizeof both) &&
          !memcmp(st1, &p->lane[LANE_BASS].steps[16], sizeof st1) && seq_bass_style(&s) == -1,
          "the rhythm-locked bar 2 is note-locked at the settings it played (its notes kept); bar 3, locked both ways, keeps its own frozen ones");
}

/* Claude's call (owner to check): a rhythm-locked CHORDS bar keeps its rhythm but takes the block's voicing (voicing is per part) */
static void test_22_chords_locked_takes_voicing(void) {
    static seq_t s, r;
    seq_part_t *p = &s.part[0];
    unsigned char was[16];
    int before;
    seq_init(&s, emit, NULL);
    seq_init(&r, emit, NULL);
    p->rb[DL_CHORDS][1].lock = 1;
    render_now(&s);
    memcpy(was, p->rb[DL_CHORDS][1].c, 16);
    before = seq_part_step_note(&s, 0, LANE_CHORDS, 1, 0);
    seq_jam_apply_now(&s, LANE_CHORDS, 0, 4);   /* HOUSE JACKIN: INVERSION 1, OCTAVE +1 (the lowest note moves) */
    seq_jam_apply_now(&r, LANE_CHORDS, 0, 4);
    int ok = 1;
    for (int st = 0; st < 16; st++) ok &= seq_part_step_note(&s, 0, LANE_CHORDS, 1, st) == seq_part_step_note(&r, 0, LANE_CHORDS, 1, st);
    CHECK(!memcmp(was, p->rb[DL_CHORDS][1].c, 16) && p->rb[DL_CHORDS][1].lock && ok && before != seq_part_step_note(&s, 0, LANE_CHORDS, 1, 0) &&
          chords_voicing_is(&s, 0, 4), "the locked bar keeps its rhythm and plays the new voicing, as an unlocked part does");
}

static void test_22_edited(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    seq_init(&s, emit, NULL);
    p->rb[DL_CHORDS][3].lock = 1;
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 2);
    seq_jam_apply_now(&s, LANE_BASS, 3, 2);
    CHECK(!seq_jam_edited(&s, LANE_CHORDS) && !seq_jam_edited(&s, LANE_BASS), "right after a tap: not edited");
    p->rb[DL_CHORDS][3].c[5] = RC_HIT;
    CHECK(!seq_jam_edited(&s, LANE_CHORDS), "a locked bar's codes changed: not edited");
    seq_set_voicing_style(&s, 3);
    int c, l;
    CHECK(seq_jam_edited(&s, LANE_CHORDS) && seq_jam_playing(&s, LANE_CHORDS, &c, &l) && c == 3 && l == 2, "a VOICING pick after the tap: edited, the block still named");
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 2);
    CHECK(!seq_jam_edited(&s, LANE_CHORDS) && chords_voicing_is(&s, 3, 2), "the block tapped again re-applies over the edit");
    seq_set_vknob(&s, VK_SPR, 3);
    CHECK(seq_jam_edited(&s, LANE_CHORDS), "a SPREAD turn: edited");
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 2);
    p->rb[DL_CHORDS][0].c[3] = p->rb[DL_CHORDS][0].c[3] == RC_REST ? RC_HIT : RC_REST;
    CHECK(seq_jam_edited(&s, LANE_CHORDS), "an unlocked bar's codes changed: edited");
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 2);
    seq_rhythm_pick(&s, LANE_CHORDS, 3);
    CHECK(seq_jam_edited(&s, LANE_CHORDS), "a library rhythm picked into the bars: edited");
    seq_set_bass_style(&s, 5);
    CHECK(seq_jam_edited(&s, LANE_BASS), "a BASS STYLE pick: edited");
    seq_jam_apply_now(&s, LANE_BASS, 3, 2);
    seq_set_bknob(&s, BK_MOV, (seq_bknob(&s, BK_MOV) + 1) % 5);
    CHECK(seq_jam_edited(&s, LANE_BASS), "a MOVEMENT turn: edited");
}

/* TIME and VARIATION render every bar not locked for JAM: TIME then VARIATION's rhythm (phrase positions as MELODY: BAR 4 {3}, BARS 2+4
 * {1, 3}, ALL {1, 2, 3}, position = bar mod 4); a varied Chords bar plays INVERSION +1 / +2 / -1; a varied Bass bar its NEW BAR variation
 * + the position; turning back restores the bars */
static void test_22_time_var_render(void) {
    static seq_t s, r;
    static const char *const VN[4] = {"OFF", "BAR 4", "BARS 2+4", "ALL"};
    static const int VARIED[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 0, 1}, {0, 1, 1, 1}};
    seq_part_t *p = &s.part[0];
    unsigned char k[16], t[16], v[16], kb[16];
    seq_step_t drawn[8 * 16];
    int ok = 1;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 2);   /* POP PIANO 4S */
    seq_jam_apply_now(&s, LANE_BASS, 3, 3);     /* POP ROOT-5 */
    p->rb[DL_CHORDS][6].lock = 1;   /* position 2 */
    p->rb[DL_BASS][5].lock = 1;     /* position 1, rhythm lock */
    p->bass.nlock = 1 << 7, p->bass.frozen[7] = p->bass.set;   /* position 3, note lock */
    render_now(&s);
    memcpy(drawn, p->lane[LANE_CHORDS].steps, sizeof drawn);
    rhythm_parse(jam_block_codes(JL_CHORDS, 3, 2), k);
    rhythm_parse(jam_block_codes(JL_BASS, 3, 3), kb);
    for (int tm = -1; tm <= 1; tm += 2) {
        seq_djam_set(&s, LANE_CHORDS, DJ_TIME, tm);
        rhythm_time(k, tm, t);
        for (int b = 0; b < 8; b++) ok &= bar_plays_codes(&s, LANE_CHORDS, b, b == 6 ? k : t);
    }
    CHECK(ok && seq_djam_get(&s, LANE_CHORDS, DJ_TIME) == 1, "TIME HALF and DOUBLE: every unlocked Chords bar plays rhythm_time's codes, the locked one as drawn");
    seq_djam_set(&s, LANE_CHORDS, DJ_TIME, 0);
    CHECK(!memcmp(drawn, p->lane[LANE_CHORDS].steps, sizeof drawn), "TIME back to NORMAL: the bars as before");
    int inv0[8];
    for (int b = 0; b < 8; b++) inv0[b] = seq_part_step_note(&s, 0, LANE_CHORDS, b, 0);
    seq_init(&r, emit, NULL);
    seq_set_bars(&r, 8);
    seq_jam_apply_now(&r, LANE_CHORDS, 3, 2);
    for (int var = 1; var <= 3; var++) {
        int rok = 1, nok = 1;
        seq_djam_set(&s, LANE_CHORDS, DJ_VAR, var);
        for (int b = 0; b < 8; b++) {
            int pos = b % 4, on = VARIED[var][pos] && b != 6;
            rhythm_vary(k, on ? pos : 0, 3, v);
            rok &= bar_plays_codes(&s, LANE_CHORDS, b, v);
            seq_set_vknob(&r, VK_INV, jam_chords(3, 2)->inv + (on ? (pos == 1 ? 1 : pos == 2 ? 2 : -1) : 0));
            nok &= seq_part_step_note(&s, 0, LANE_CHORDS, b, 0) == seq_part_step_note(&r, 0, LANE_CHORDS, b, 0);
            if (on) nok &= seq_part_step_note(&s, 0, LANE_CHORDS, b, 0) != inv0[b];
        }
        CHECK(rok && nok, "CHORDS VARIATION %s: the varied bars (bar mod 4 in its set, not the locked bar 7) play rhythm_vary's codes and "
              "INVERSION +1 / +2 / -1 at positions 1 / 2 / 3; the rest as drawn", VN[var]);
    }
    seq_djam_set(&s, LANE_CHORDS, DJ_VAR, 0);
    ok = !memcmp(drawn, p->lane[LANE_CHORDS].steps, sizeof drawn);
    for (int b = 0; b < 8; b++) ok &= seq_part_step_note(&s, 0, LANE_CHORDS, b, 0) == inv0[b];
    CHECK(ok, "VARIATION back to OFF: the same steps and notes as before");
    /* BASS: a varied bar's notes are those of its varied rhythm at NEW BAR variation + position */
    seq_step_t bdrawn[8 * 16];
    memcpy(bdrawn, p->lane[LANE_BASS].steps, sizeof bdrawn);
    seq_djam_set(&s, LANE_BASS, DJ_VAR, 3);
    seq_init(&r, emit, NULL);
    seq_set_bars(&r, 8);
    seq_jam_apply_now(&r, LANE_BASS, 3, 3);
    seq_part_t *q = &r.part[0];
    for (int b = 0; b < 8; b++) {
        int pos = b % 4, on = pos && b != 5 && b != 7;
        rhythm_vary(kb, on ? pos : 0, 3, q->rb[DL_BASS][b].c);
        q->lane[LANE_BASS].bar_var[b] = (unsigned char)(on ? pos : 0);
    }
    render_now(&r);
    ok = 1;
    for (int b = 0; b < 8; b++) {
        int pos = b % 4, on = pos && b != 5 && b != 7;
        ok &= on ? !memcmp(&p->lane[LANE_BASS].steps[b * 16], &q->lane[LANE_BASS].steps[b * 16], 16 * sizeof(seq_step_t))
                 : !memcmp(&p->lane[LANE_BASS].steps[b * 16], &bdrawn[b * 16], 16 * sizeof(seq_step_t));
    }
    CHECK(ok, "BASS VARIATION ALL: varied bars play their varied rhythm at variation + position; the rhythm-locked bar 6 and note-locked bar 8 as before");
    seq_djam_set(&s, LANE_BASS, DJ_TIME, -1);
    rhythm_time(kb, -1, t);
    rhythm_vary(t, 1, 3, v);
    CHECK(bar_plays_codes(&s, LANE_BASS, 1, v) && !memcmp(&p->lane[LANE_BASS].steps[5 * 16], &bdrawn[5 * 16], 16 * sizeof(seq_step_t)) &&
          !memcmp(&p->lane[LANE_BASS].steps[7 * 16], &bdrawn[7 * 16], 16 * sizeof(seq_step_t)),
          "BASS TIME HALF with VARIATION: TIME first, then VARIATION's rhythm; the locked bars untouched");
    seq_djam_set(&s, LANE_BASS, DJ_TIME, 0), seq_djam_set(&s, LANE_BASS, DJ_VAR, 0);
    CHECK(!memcmp(bdrawn, p->lane[LANE_BASS].steps, sizeof bdrawn), "both back: the Bass bars as before");
    /* a 2-bar loop varies only the positions it has */
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 2);
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 2);
    seq_djam_set(&s, LANE_CHORDS, DJ_VAR, 1);
    CHECK(bar_plays_codes(&s, LANE_CHORDS, 0, k) && bar_plays_codes(&s, LANE_CHORDS, 1, k), "a 2-bar loop at BAR 4: nothing varies");
    seq_djam_set(&s, LANE_CHORDS, DJ_VAR, 3);
    rhythm_vary(k, 1, 3, v);
    CHECK(bar_plays_codes(&s, LANE_CHORDS, 0, k) && bar_plays_codes(&s, LANE_CHORDS, 1, v), "at ALL: bar 2 (position 1) varies");
    seq_djam_set(&s, LANE_CHORDS, DJ_VAR, 9), seq_djam_set(&s, LANE_CHORDS, DJ_SWING, -3), seq_djam_set(&s, LANE_CHORDS, DJ_SW, 7);
    seq_djam_set(&s, LANE_MEL_UP, DJ_VAR, 1), seq_djam_set(&s, LANE_CHORDS, DJ_COUNT, 1);
    CHECK(seq_djam_get(&s, LANE_CHORDS, DJ_VAR) == 3 && seq_djam_get(&s, LANE_CHORDS, DJ_SWING) == 0 && seq_djam_get(&s, LANE_CHORDS, DJ_SW) == 3 &&
          !seq_djam_get(&s, LANE_MEL_UP, DJ_VAR) && !seq_djam_get(&s, LANE_CHORDS, DJ_COUNT),
          "settings clamp (VARIATION 0..3, SWING 0..5, SWITCH AT 0..3); a bad lane or field is ignored and reads 0");
}

static int djam_same(const djam_t *a, const djam_t *b) {
    return a->c == b->c && a->l == b->l && a->sw == b->sw && a->var == b->var && a->swing == b->swing && a->tm == b->tm;
}

static void test_22_save_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX], st[1024];
    const seq_part_t *p = &s.part[0], *q = &t.part[0];
    int n;
    seq_init(&s, emit, NULL);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, ".dj="), "a new instance writes no .dj");
    s.part[0].rb[DL_CHORDS][2].lock = 1;
    seq_jam_apply_now(&s, LANE_CHORDS, 1, 1);
    seq_jam_apply_now(&s, LANE_BASS, 3, 4);
    seq_djam_set(&s, LANE_CHORDS, DJ_SW, 3), seq_djam_set(&s, LANE_CHORDS, DJ_VAR, 2), seq_djam_set(&s, LANE_CHORDS, DJ_SWING, 4);
    seq_djam_set(&s, LANE_CHORDS, DJ_TIME, -1);
    seq_djam_set(&s, LANE_BASS, DJ_SW, 0), seq_djam_set(&s, LANE_BASS, DJ_VAR, 1), seq_djam_set(&s, LANE_BASS, DJ_TIME, 1);
    n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "|A1.dj=1,1,3,2,4,-1") && strstr(buf, "|A2.dj=3,4,0,1,0,1") && strstr(buf, "|A.rb=70,70,L0,70") &&
          strstr(buf, "|A2.rb=83,83,83,83"), "both lanes jammed: 1.dj and 2.dj (c,l,sw,var,swing,tm), the bars as JAM indexes");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && djam_same(&p->djam[DL_CHORDS], &q->djam[DL_CHORDS]) && djam_same(&p->djam[DL_BASS], &q->djam[DL_BASS]) &&
          !memcmp(p->rb[DL_CHORDS], q->rb[DL_CHORDS], 4 * sizeof p->rb[0][0]) && !memcmp(p->lane[LANE_CHORDS].steps, q->lane[LANE_CHORDS].steps, 4 * 16 * sizeof(seq_step_t)) &&
          bass_same(p, q) && !seq_jam_edited(&t, LANE_CHORDS) && !seq_jam_edited(&t, LANE_BASS) && chords_voicing_is(&t, 1, 1),
          "it loads: the JAM state, bars, steps, voicing and Bass notes come back, neither lane edited");
    CHECK(seq_state_save(&t, again, sizeof again) == n && !strcmp(buf, again), "it saves back byte for byte");
    seq_init(&s, emit, NULL);
    seq_djam_set(&s, LANE_BASS, DJ_SW, 2);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A2.dj=-1,0,2,0,0,0") && !strstr(buf, "A1.dj="), "a setting alone (no block): 2.dj only");
    static const char HEAD[] = "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0352|A.voi=1|ch=2,3,4|A1=0:|A1.g=17,1,50|A2=0:|A2.g=18,1,50";
    static const djam_t NEW = {-1, 0, 1, 0, 0, 0, 0, 0};
    snprintf(st, sizeof st, "%s|A1.dj=1,1,3|A2.dj=1,1,1,1,1,1,1|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && djam_same(&q->djam[DL_CHORDS], &NEW) && djam_same(&q->djam[DL_BASS], &NEW), "3 or 7 values: ignored whole");
    snprintf(st, sizeof st, "%s|A1.dj=9,1,9,9,9,9|A2.dj=2,7,-5,-5,-5,-5|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && q->djam[DL_CHORDS].c == -1 && q->djam[DL_CHORDS].sw == 3 && q->djam[DL_CHORDS].var == 3 &&
          q->djam[DL_CHORDS].swing == 5 && q->djam[DL_CHORDS].tm == 1 && q->djam[DL_BASS].c == -1 && q->djam[DL_BASS].sw == 0 &&
          q->djam[DL_BASS].tm == -1, "a block outside the table reads none; the settings clamp");
    snprintf(st, sizeof st, "%s|A1.dj=1,x,1,0,0,0|A2.dj=1,1,1,0,0,0,|A3=0:|A4=0:", HEAD);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, st) && djam_same(&q->djam[DL_CHORDS], &NEW) && djam_same(&q->djam[DL_BASS], &NEW), "a bad character or a trailing comma: ignored whole");
}

/* Review Focus: a 0.21 project (no dj, no JAM index; pinned as the 0.21 build wrote it, with its notes' hash from that build) loads and plays
 * exactly as before: no block, every JAM knob neutral, the same steps, notes and fills, and it saves back unchanged */
#define CHUNK_021 "csq2|gv=1|k=4|s=1|A.len=8|A.cl=16|A.h=30,1|A.prog=0526|A.voi=1|A.vx=44,1,2,1,0,0|ch=1,2,3|A1=0:|A1.g=17,1,70|A.rb=0,0A.x.gx..x-..x.g.," \
                  "L0A.x.gx..x-..x.g.,0,0,L0,0,0|A.rz=1,0,0,2|A2=0:|A2.g=18,1,35|A2.bv=00000000000000020000000000000000|A2.ab=342d|A2.bx=-1,0,1,2,1,2,1" \
                  "|A2.bl=6:-1.2.-1.3.2.1.-1|A2.rb=0,0,0,0A--.x.g.A.x-x..g,L0,0,0,0|A2.rz=0,2,0,0|A3=0:|A3.g=19,1,10|A4=0:"
static unsigned fnv_bytes(unsigned h, const void *v, size_t n) {
    const unsigned char *b = v;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}
static void lane_notes(const seq_t *s, int part, int lane, int notes[SEQ_LANE_STEPS]) {   /* every step's (-1 past the bars) */
    for (int k = 0; k < SEQ_LANE_STEPS; k++) notes[k] = seq_part_step_note(s, part, lane, k / SEQ_STEPS, k % SEQ_STEPS);
}
static unsigned part0_hash(const seq_t *s) {   /* the 0.21 build's: lanes 1-3's steps and notes, both drawn lanes' fills */
    const seq_part_t *p = &s->part[0];
    unsigned h = 2166136261u;
    int notes[SEQ_LANE_STEPS];
    for (int l = 0; l < 3; l++) {
        h = fnv_bytes(h, p->lane[l].steps, (size_t)p->bars * SEQ_STEPS * sizeof(seq_step_t));
        lane_notes(s, 0, l, notes);
        h = fnv_bytes(h, notes, (size_t)p->bars * SEQ_STEPS * sizeof(int));
    }
    for (int d = 0; d < DL_COUNT; d++) h = fnv_bytes(h, p->rfill[d], (size_t)p->bars * 16), h = fnv_bytes(h, p->rfill_abs[d], (size_t)p->bars * 16);
    return h;
}

static void test_22_load_021_chunk(void) {
    static seq_t t;
    static char buf[SEQ_STATE_MAX];
    static const djam_t NEW = {-1, 0, 1, 0, 0, 0, 0, 0};
    const seq_part_t *q = &t.part[0];
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, CHUNK_021) && djam_same(&q->djam[DL_CHORDS], &NEW) && djam_same(&q->djam[DL_BASS], &NEW) &&
          !seq_jam_playing(&t, LANE_CHORDS, NULL, NULL) && !seq_jam_playing(&t, LANE_BASS, NULL, NULL) && !seq_jam_edited(&t, LANE_CHORDS),
          "a 0.21 chunk loads with no block and every JAM knob neutral (SWITCH AT BAR)");
    CHECK(part0_hash(&t) == 0xd1c217c0u, "its steps, notes and fills hash as the 0.21 build's (0x%08x)", part0_hash(&t));
    CHECK(seq_state_save(&t, buf, sizeof buf) > 0 && !strcmp(buf, CHUNK_021), "it saves back unchanged (no .dj)");
}

/* test_20_save_fits' heavy parts with both drawn lanes jammed (bars as JAM indexes, 8 of them edited / locked as before, every dj field
 * non-default) in all 6 parts: every part still saves */
static void test_22_save_fits(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    int ok = 1;
    seq_init(&s, emit, NULL);
    for (int part = 0; part < SEQ_NPARTS; part++) {
        heavy_part(&s, part, 0, 4, 16);
        seq_jam_apply_now(&s, LANE_CHORDS, 5, 4);
        seq_jam_apply_now(&s, LANE_BASS, 5, 4);
        heavy_part(&s, part, 8, 4, 16);   /* 8 bars edited (Bass ones locked) over the jammed ones */
        for (int d = 0; d < DL_COUNT; d++) {
            seq_djam_set(&s, d, DJ_SW, 3), seq_djam_set(&s, d, DJ_VAR, 3), seq_djam_set(&s, d, DJ_SWING, 5), seq_djam_set(&s, d, DJ_TIME, -1);
        }
    }
    seq_select_part(&s, 0);
    int n = seq_state_save(&s, buf, sizeof buf);
    CHECK(n > 0 && !seq_unsaved_parts(&s) && strstr(buf, "|F1.dj=5,4,3,3,5,-1") && strstr(buf, "|F2.dj=5,4,3,3,5,-1"),
          "6 parts, both lanes jammed, every dj field set, test_20's heavy edits on top: every part saved in %d bytes of %d (headroom %d)",
          n, SEQ_STATE_MAX, SEQ_STATE_MAX - n);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf), "and it loads");
    for (int part = 0; part < SEQ_NPARTS; part++)
        ok &= !memcmp(t.part[part].rb, s.part[part].rb, sizeof s.part[part].rb) && bass_same(&s.part[part], &t.part[part]) &&
              djam_same(&s.part[part].djam[DL_CHORDS], &t.part[part].djam[DL_CHORDS]) && djam_same(&s.part[part].djam[DL_BASS], &t.part[part].djam[DL_BASS]);
    CHECK(ok, "every part's bars, Bass and JAM state come back");
}

/* 0.22 Task 4: CHORDS and BASS JAM blocks queue and switch in time */

/* a lane's note-ons logged since nev = 0, by the step that fired them: h[k] the sum of note x 131 + velocity, n[k] how many; once: no
 * step played a note twice; bal: no note hung (none_hung) */
typedef struct { long h[MJ_KMAX]; int n[MJ_KMAX]; int once, bal; } lane_run_t;

/* every note-on logged since nev = 0 has its note-off after it (a log that starts mid-play holds offs of earlier notes) */
static int none_hung(void) {
    int open[16][128];
    memset(open, 0, sizeof open);
    for (int i = 0; i < nev; i++) {
        int ch = evs[i].status & 15, n = evs[i].note;
        if (is_on(&evs[i])) open[ch][n]++;
        else if (is_off(&evs[i]) && open[ch][n] > 0) open[ch][n]--;
    }
    for (int c = 0; c < 16; c++)
        for (int n = 0; n < 128; n++)
            if (open[c][n]) return 0;
    return 1;
}

static void lane_ons(const seq_t *s, int lane, lane_run_t *r) {
    static unsigned char seen[MJ_KMAX][128];
    int ch = seq_channel_of(s, lane);
    memset(r->h, 0, sizeof r->h), memset(r->n, 0, sizeof r->n), memset(seen, 0, sizeof seen);
    r->once = 1, r->bal = none_hung();
    for (int i = 0; i < nev; i++) {
        const ev_t *e = &evs[i];
        if (!is_on(e) || (e->status & 15) != ch - 1 || e->k < 0 || e->k >= MJ_KMAX) continue;
        r->h[e->k] += e->note * 131 + e->vel, r->n[e->k]++;
        r->once &= ++seen[e->k][e->note] == 1;
    }
}

/* plays x from ppq until step k - 1 has fired, then stops (a fresh log); r: lane's note-ons. Returns the position reached */
static double run_lane(seq_t *x, double ppq, long k, int lane, lane_run_t *r) {
    nev = 0;
    ppq = play_to_k(x, ppq, k);
    seq_block(x, ppq, bpm, 0, 128);
    lane_ons(x, lane, r);
    return ppq;
}

/* 1 when run x played run a's steps in [from, at) and run b's in [at, to), step for step */
static int ran_as(const lane_run_t *x, const lane_run_t *a, const lane_run_t *b, long from, long to, long at) {
    int ok = 1;
    for (long k = from; k < to; k++) {
        const lane_run_t *w = k < at ? a : b;
        ok &= x->h[k] == w->h[k] && x->n[k] == w->n[k];
    }
    return ok;
}

/* 1 when runs a and b differ on a step in [from, to) */
static int runs_differ(const lane_run_t *a, const lane_run_t *b, long from, long to) {
    for (long k = from; k < to; k++)
        if (a->h[k] != b->h[k] || a->n[k] != b->n[k]) return 1;
    return 0;
}

/* 1 when drawn lane `lane` of a's and b's playing parts is the same: its steps, bars, fills, block; the Bass's roles and note settings,
 * the Chords' voicing, voices and labels */
static int lane_same(const seq_t *a, const seq_t *b, int lane) {
    const seq_part_t *p = &a->part[a->cur_part], *q = &b->part[b->cur_part];
    int d = lane == LANE_CHORDS ? DL_CHORDS : DL_BASS, n = p->filled * SEQ_STEPS;
    int ok = p->filled == q->filled && !memcmp(p->lane[lane].steps, q->lane[lane].steps, sizeof(seq_step_t) * (size_t)n) &&
             !memcmp(p->rb[d], q->rb[d], sizeof p->rb[d]) && !memcmp(p->rfill[d], q->rfill[d], sizeof p->rfill[d]) &&
             !memcmp(p->rfill_abs[d], q->rfill_abs[d], sizeof p->rfill_abs[d]) && p->djam[d].c == q->djam[d].c && p->djam[d].l == q->djam[d].l;
    if (lane == LANE_BASS)
        return ok && !memcmp(p->brole, q->brole, (size_t)n) && !memcmp(&p->bass.set, &q->bass.set, sizeof p->bass.set) &&
               !memcmp(p->bass.frozen, q->bass.frozen, sizeof p->bass.frozen) && p->bass.nlock == q->bass.nlock;
    return ok && p->vstyle == q->vstyle && p->vmode == q->vmode && p->vcol == q->vcol && p->vinv == q->vinv && p->vspr == q->vspr &&
           p->vstrdir == q->vstrdir && p->voicing == q->voicing && !memcmp(p->lane_shape[LANE_CHORDS], q->lane_shape[LANE_CHORDS], sizeof p->lane_shape[0]) &&
           !memcmp(p->voice, q->voice, sizeof p->voice) && !memcmp(p->voice_n, q->voice_n, sizeof p->voice_n) &&
           !memcmp(p->slot_text, q->slot_text, sizeof p->slot_text) && !memcmp(p->slot_num, q->slot_num, sizeof p->slot_num);
}

/* 1 when the block queued on `lane`, landing now (a stop installs it), leaves the lane as the same tap applied at once does */
static int jq_fresh(const seq_t *s, int lane) {
    static seq_t r, f;
    int c, l;
    if (!s->playing || !seq_jam_queued(s, lane, &c, &l)) return 0;
    r = *s, r.emit = quiet;
    f = *s, f.emit = quiet;
    seq_block(&r, 0, bpm, 0, 128);
    seq_jam_apply_now(&f, lane, c, l);   /* the queue it replaces is dropped */
    return !seq_jam_queued(&r, lane, NULL, NULL) && lane_same(&r, &f, lane);
}

/* 1 when the playing lane is what its setters render (a key change and back renders every lane afresh) */
static int jlane_is_fresh(const seq_t *s, int lane) {
    static seq_t f;
    f = *s, f.emit = quiet;
    render_now(&f);
    return lane_same(s, &f, lane);
}

/* A 16-bar loop, a tap at bar 2 step 6 on each lane and each SWITCH AT: the queued block's first step plays exactly at its boundary from
 * the song position (bar 2 beat 3, bar 3, bar 3, bar 5), every step before it the old block's, every one from it a tap's applied at once;
 * PUSH never plays the old block's chord early into the boundary */
static void test_22_queue_switch_at(void) {
    static seq_t s, old, nw;
    static lane_run_t rs, ro, rn;
    static const long AT[4] = {24, 32, 32, 64};
    static const char *const NAME[4] = {"BEAT", "BAR", "2 BARS", "PHRASE"};
    static const int LANE[2] = {LANE_CHORDS, LANE_BASS}, BC[2] = {0, 1}, BL[2] = {4, 4};   /* HOUSE JACKIN, TECHNO ACID: hits on 1 and 9 */
    for (int i = 0; i < 2; i++)
        for (int sw = 0; sw < 4; sw++) {
            int c = -1, l = -1, queued, d = i;
            double ppq;
            seq_init(&s, emit, NULL);
            seq_set_bars(&s, 16);
            seq_djam_set(&s, LANE[i], DJ_SW, sw);
            nev = 0;
            ppq = play_to_k(&s, 0, 21);
            old = s, nw = s;
            seq_jam_apply_now(&nw, LANE[i], BC[i], BL[i]);
            queued = seq_jam_queue(&s, LANE[i], BC[i], BL[i]);
            CHECK(queued && seq_jam_queued(&s, LANE[i], &c, &l) && c == BC[i] && l == BL[i] && s.part[0].djam[d].c == -1 &&
                      !seq_jam_playing(&s, LANE[i], NULL, NULL) && jq_fresh(&s, LANE[i]),
                  "%s SWITCH AT %s: a tap at bar 2 step 6 queues the block (none plays yet; the queue a fresh render)", i ? "BASS" : "CHORDS", NAME[sw]);
            run_lane(&s, ppq, AT[sw] + 32, LANE[i], &rs);
            run_lane(&old, ppq, AT[sw] + 32, LANE[i], &ro);
            run_lane(&nw, ppq, AT[sw] + 32, LANE[i], &rn);
            CHECK(rn.n[AT[sw]] && runs_differ(&ro, &rn, AT[sw], AT[sw] + 16) && ran_as(&rs, &ro, &rn, 21, AT[sw] + 32, AT[sw]) && rs.once && rs.bal,
                  "%s SWITCH AT %s: the new block's first step plays at step %ld of bar %ld, every step before it the old block's, none twice",
                  i ? "BASS" : "CHORDS", NAME[sw], AT[sw] % 16 + 1, AT[sw] / 16 + 1);
            int polled = seq_jam_poll(&s), again = seq_jam_poll(&s);
            CHECK(!seq_jam_queued(&s, LANE[i], NULL, NULL) && seq_jam_playing(&s, LANE[i], &c, &l) && c == BC[i] && l == BL[i] &&
                      lane_same(&s, &nw, LANE[i]) && jlane_is_fresh(&s, LANE[i]) && polled == 1 << d && !again,
                  "%s SWITCH AT %s: then nothing queued, the block plays, its lane as a tap at once leaves it; one poll sees the switch",
                  i ? "BASS" : "CHORDS", NAME[sw]);
        }
    /* PUSH 100 on both lanes: at a bar-line boundary the old block's chord / note is not pushed early, the new one plays on the beat */
    for (int i = 0; i < 2; i++) {
        int ch;
        double ppq, sum_s = 0, sum_n = 0;
        seq_init(&s, emit, NULL);
        seq_set_lane_shape(&s, LANE[i], i ? BSHAPE_PUSH : SHAPE_PUSH, 100);
        ch = seq_channel_of(&s, LANE[i]);
        nev = 0;
        ppq = play_to_k(&s, 0, 21);
        nw = s;
        seq_jam_apply_now(&nw, LANE[i], BC[i], BL[i]);
        seq_jam_queue(&s, LANE[i], BC[i], BL[i]);
        nev = 0;
        play_to_k(&s, ppq, 50);
        int early = ons_between(0, ch, 7.8, 7.99), on = ons_near(0, ch, 8.0), later = ons_near(0, ch, 11.875);
        for (int e = 0; e < nev; e++)
            if (is_on(&evs[e]) && (evs[e].status & 15) == ch - 1 && evs[e].ppq > 8.0 - 1e-9 && evs[e].ppq <= 8.0 + blk() + 1e-9) sum_s += evs[e].note;
        nev = 0;
        play_to_k(&nw, ppq, 40);
        for (int e = 0; e < nev; e++)
            if (is_on(&evs[e]) && (evs[e].status & 15) == ch - 1 && evs[e].ppq > 7.875 - 1e-9 && evs[e].ppq <= 7.875 + blk() + 1e-9) sum_n += evs[e].note;
        CHECK(!early && on && sum_s == sum_n && sum_s > 0 && later,
              "%s PUSH 100, SWITCH AT BAR: nothing pushed into the boundary, the block's first %s on the beat (its next bar line pushed)",
              i ? "BASS" : "CHORDS", i ? "note" : "chord");
        seq_block(&s, 10.0, bpm, 0, 128);
    }
}

/* Review Focus: an 8-bar loop, blocks queued at bar 7 step 6 on MELODY (SWITCH AT BEAT), CHORDS (BAR) and BASS (2 BARS): each takes over at
 * its own boundary (step 104, 112, and 128: the loop's wrap; each boundary step already sounds the new block, not the old), none twice. Then in bar 4 of the next pass a BASS block (BEAT) and a CHORDS
 * block (BAR) queued, and a part switch for the same bar line: the BASS one lands on its beat, the part switch drops the CHORDS one */
static void test_22_three_queues(void) {
    static seq_t s, old, nw;
    static lane_run_t rs[3], ro[3], rn[3];
    static const int LANE[3] = {LANE_MEL_UP, LANE_CHORDS, LANE_BASS};
    static const long AT[3] = {104, 112, 128};
    static const char *const NAME[3] = {"MELODY", "CHORDS", "BASS"};
    double ppq;
    int ok = 1, c, l;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    seq_set_mel_switch(&s, 0);
    seq_djam_set(&s, LANE_CHORDS, DJ_SW, 1);
    seq_djam_set(&s, LANE_BASS, DJ_SW, 2);
    nev = 0;
    ppq = play_to_k(&s, 0, 101);
    old = s, nw = s;
    nw.part[0].mjam.set.c = MJ_HOOK, nw.part[0].mjam.set.l = 1;
    mel_rerender(&nw);
    seq_jam_apply_now(&nw, LANE_CHORDS, 0, 4);
    seq_jam_apply_now(&nw, LANE_BASS, 1, 4);
    seq_mel_queue(&s, MJ_HOOK, 1);
    seq_jam_queue(&s, LANE_CHORDS, 0, 4);
    seq_jam_queue(&s, LANE_BASS, 1, 4);
    CHECK(seq_mel_queued(&s, NULL, NULL, NULL) && seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && seq_jam_queued(&s, LANE_BASS, NULL, NULL),
          "three blocks queued at bar 7 step 6: MELODY HOOK at BEAT, CHORDS JACKIN at BAR, BASS ACID at 2 BARS");
    for (int i = 0; i < 3; i++) {
        static seq_t x;
        x = s;
        run_lane(&x, ppq, 176, LANE[i], &rs[i]);
        x = old;
        run_lane(&x, ppq, 176, LANE[i], &ro[i]);
        x = nw;
        run_lane(&x, ppq, 176, LANE[i], &rn[i]);
        CHECK(runs_differ(&ro[i], &rn[i], AT[i], AT[i] + 1) && ran_as(&rs[i], &ro[i], &rn[i], 101, 176, AT[i]) && rs[i].once &&
                  rs[i].bal, "%s takes over at step %ld (bar %ld%s), every step before it the old block's, none twice", NAME[i], AT[i],
              AT[i] % 128 / 16 + 1, AT[i] == 128 ? ", the wrap" : "");
    }
    ppq = play_to_k(&s, ppq, 181);   /* bar 4 step 6 of the second pass: every block took over */
    ok = !seq_mel_queued(&s, NULL, NULL, NULL) && !seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && !seq_jam_queued(&s, LANE_BASS, NULL, NULL);
    ok &= seq_jam_playing(&s, LANE_CHORDS, &c, &l) && c == 0 && l == 4 && seq_jam_playing(&s, LANE_BASS, &c, &l) && c == 1 && l == 4;
    CHECK(ok && s.part[0].mjam.set.c == MJ_HOOK && jlane_is_fresh(&s, LANE_CHORDS) && jlane_is_fresh(&s, LANE_BASS),
          "then nothing queued; the three blocks play, each lane as its setters render it");
    seq_djam_set(&s, LANE_BASS, DJ_SW, 0);
    old = s;
    seq_select_part(&old, 1);   /* the Intro's first use copies the Verse as it plays now */
    nw = old;
    seq_jam_apply_now(&nw, LANE_BASS, 3, 1);   /* on the Verse, which plays until the bar line */
    ok = seq_jam_queue(&s, LANE_BASS, 3, 1) && seq_jam_queue(&s, LANE_CHORDS, 2, 3);
    CHECK(ok, "bar 4 step 6: BASS POP ROOT 4S at BEAT and CHORDS TRANCE GATE at BAR queued");
    seq_select_part(&s, 1);
    CHECK(seq_queued_part(&s) == 1 && seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && seq_jam_queued(&s, LANE_BASS, NULL, NULL),
          "then the Intro queued for the same bar line (bar 5): both blocks still wait");
    for (int i = 0; i < 3; i++) {
        static seq_t x;
        x = s;
        run_lane(&x, ppq, 224, LANE[i], &rs[i]);
        x = old;
        run_lane(&x, ppq, 224, LANE[i], &ro[i]);
        x = nw;
        run_lane(&x, ppq, 224, LANE[i], &rn[i]);
    }
    ok = ran_as(&rs[2], &ro[2], &rn[2], 181, 192, 184) && ran_as(&rs[2], &ro[2], &ro[2], 192, 224, 224) && runs_differ(&ro[2], &rn[2], 184, 192);
    CHECK(ok && rs[2].once && rs[2].bal, "the BASS block takes over on its beat (step 184); the Intro plays its own from the bar line, none twice");
    CHECK(ran_as(&rs[0], &ro[0], &ro[0], 181, 224, 224) && ran_as(&rs[1], &ro[1], &ro[1], 181, 224, 224) && rs[1].once && rs[1].bal,
          "MELODY and CHORDS play on as they were (the part switch drops TRANCE GATE at the bar line), then the Intro's");
    ppq = play_to_k(&s, ppq, 200);
    CHECK(seq_part(&s) == 1 && !seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && !seq_jam_queued(&s, LANE_BASS, NULL, NULL) &&
              s.part[0].djam[DL_BASS].c == 3 && s.part[0].djam[DL_BASS].l == 1 && s.part[0].djam[DL_CHORDS].c == 0 &&
              s.part[0].djam[DL_CHORDS].l == 4 && s.part[1].djam[DL_CHORDS].c == 0 && s.part[1].djam[DL_BASS].c == 1,
          "after the bar line: the Intro plays; the Verse keeps ROOT 4S on BASS and JACKIN on CHORDS (TRANCE GATE dropped)");
    seq_block(&s, ppq, bpm, 0, 128);
}

/* 1 when s holds exactly the n undo entries of `ring` below top (as they were) */
static int ring_is(const seq_t *s, const seq_snap_t *ring, int n, int top) {
    int ok = s->undo_n == n && s->undo_top == top;
    for (int i = 1; ok && i <= n; i++) ok &= !memcmp(&s->undo[(top + SEQ_UNDO - i) % SEQ_UNDO], &ring[(top + SEQ_UNDO - i) % SEQ_UNDO], sizeof *ring);
    return ok;
}

/* 1 when an undo entry of s carries queue tag `tag` */
static int ring_has_tag(const seq_t *s, int tag) {
    for (int i = 1; i <= s->undo_n; i++)
        if (s->undo[(s->undo_top + SEQ_UNDO - i) % SEQ_UNDO].jq == tag) return 1;
    return 0;
}

static long floor_mod_l(long a, long m) { return ((a % m) + m) % m; }

/* A queued tap's undo step (CHORDS: its two joined entries, BASS: one) leaves with the queue: the playing block tapped cancels it, a
 * second tap replaces it (still one step), UNDO while it waits cancels it and does nothing else (the latest queue first); what other
 * edits pushed meanwhile stays. Stopped, a tap applies at once; a block that took over is undone in one step */
static void test_22_queue_cancel_undo(void) {
    static seq_t s;
    static seq_snap_t ring[SEQ_UNDO];
    static seq_part_t was;
    int n0, top0, c = -1, l = -1, ok, tag, k0;
    double ppq;
    seq_init(&s, emit, NULL);
    ok = !seq_jam_queue(&s, LANE_CHORDS, 3, 0) && !seq_jam_queue(&s, LANE_BASS, 3, 0);
    CHECK(ok && s.undo_n == 3 && seq_jam_playing(&s, LANE_CHORDS, &c, &l) && c == 3 && l == 0 && seq_jam_playing(&s, LANE_BASS, &c, &l) && c == 3 &&
              !seq_jam_queued(&s, LANE_CHORDS, NULL, NULL), "stopped: a tap on each lane applies at once (CHORDS two undo entries, BASS one)");
    nev = 0;
    ppq = play_to_k(&s, 0, 21);
    n0 = s.undo_n, top0 = s.undo_top, was = s.part[0];
    memcpy(ring, s.undo, sizeof ring);
    seq_jam_queue(&s, LANE_CHORDS, 2, 3);
    CHECK(s.undo_n == n0 + 2 && s.undo[(s.undo_top + SEQ_UNDO - 1) % SEQ_UNDO].join && !memcmp(&s.part[0], &was, sizeof was) &&
              seq_can_undo(&s), "playing: a CHORDS tap queues TRANCE GATE, the part unchanged; its undo step (two joined entries) pushed");
    seq_jam_queue(&s, LANE_CHORDS, 3, 0);
    CHECK(!seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && ring_is(&s, ring, n0, top0) && !memcmp(&s.part[0], &was, sizeof was),
          "the playing block tapped: the queue cancelled, both its undo entries gone, the ring as before");
    seq_jam_queue(&s, LANE_CHORDS, 2, 3);
    seq_jam_queue(&s, LANE_CHORDS, 4, 1);
    CHECK(seq_jam_queued(&s, LANE_CHORDS, &c, &l) && c == 4 && l == 1 && s.undo_n == n0 + 2 && jq_fresh(&s, LANE_CHORDS),
          "a second tap replaces it: ROCK BACKBEAT queued, still one undo step");
    seq_undo(&s);
    CHECK(!seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && ring_is(&s, ring, n0, top0) && !memcmp(&s.part[0], &was, sizeof was),
          "UNDO while it waits: the queue cancelled with its step, nothing else");
    seq_jam_queue(&s, LANE_BASS, 5, 2);
    CHECK(s.undo_n == n0 + 1, "a BASS tap queued: one undo entry");
    seq_set_step(&s, LANE_CHORDS, 1, 3, !s.part[0].lane[LANE_CHORDS].steps[16 + 3].on);   /* a drawn Chords step: its own undo step */
    seq_jam_queue(&s, LANE_BASS, 3, 0);
    CHECK(!seq_jam_queued(&s, LANE_BASS, NULL, NULL) && s.undo_n == n0 + 1 && s.undo[(s.undo_top + SEQ_UNDO - 1) % SEQ_UNDO].lane == LANE_CHORDS &&
              s.part[0].rb[DL_CHORDS][1].ed, "a CHORDS step drawn meanwhile, then the BASS tap cancelled: only the tap's entry left the ring");
    seq_undo(&s);
    n0 = s.undo_n, top0 = s.undo_top, was = s.part[0];
    memcpy(ring, s.undo, sizeof ring);
    seq_jam_queue(&s, LANE_BASS, 5, 2);
    seq_jam_queue(&s, LANE_CHORDS, 2, 3);
    seq_undo(&s);
    CHECK(!seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && seq_jam_queued(&s, LANE_BASS, &c, &l) && c == 5 && l == 2 && s.undo_n == n0 + 1 &&
              !memcmp(&s.part[0], &was, sizeof was), "both lanes queued: UNDO cancels the latest (CHORDS), the BASS one waits");
    seq_undo(&s);
    CHECK(!seq_jam_queued(&s, LANE_BASS, NULL, NULL) && ring_is(&s, ring, n0, top0) && !memcmp(&s.part[0], &was, sizeof was),
          "UNDO again cancels the BASS one");
    seq_jam_queue(&s, LANE_CHORDS, 2, 3);
    ppq = play_to_k(&s, ppq, 40);   /* bar 3: it took over */
    CHECK(seq_jam_playing(&s, LANE_CHORDS, &c, &l) && c == 2 && l == 3 && s.undo_n == n0 + 2, "TRANCE GATE queued and took over at bar 3");
    seq_undo(&s);
    CHECK(seq_jam_playing(&s, LANE_CHORDS, &c, &l) && c == 3 && l == 0 && ring_is(&s, ring, n0, top0) &&
              !memcmp(s.part[0].rb[DL_CHORDS], was.rb[DL_CHORDS], sizeof was.rb[DL_CHORDS]) && s.part[0].vmode == was.vmode &&
              !memcmp(s.part[0].voice, was.voice, sizeof was.voice) && jlane_is_fresh(&s, LANE_CHORDS),
          "one UNDO takes the block that took over back whole (bars, voicing, voices): POP WHOLE plays again");
    /* fix round 1: an open transaction's CANCEL puts its ring copy back (txn_ring_back); a cancelled tap's entries never return with it */
    n0 = s.undo_n, top0 = s.undo_top;
    memcpy(ring, s.undo, sizeof ring);
    seq_jam_queue(&s, LANE_BASS, 5, 2);
    tag = s.djq[DL_BASS].tag;
    seq_rbar_open(&s, LANE_CHORDS, 0);   /* a bar open on the CHORDS tab */
    seq_jam_queue(&s, LANE_BASS, 3, 0);   /* the playing BASS block: a cancel */
    k0 = seq_bknob(&s, 0);
    seq_set_bknob(&s, 0, k0 == 4 ? 0 : 4);
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(!seq_jam_queued(&s, LANE_BASS, NULL, NULL) && !ring_has_tag(&s, tag) && ring_is(&s, ring, n0, top0) && seq_bknob(&s, 0) == (k0 == 4 ? 0 : 4),
          "a BASS queue cancelled with a CHORDS bar open, a knob turned, then the bar's CANCEL: the tap's entry stays gone, the knob kept");
    seq_set_bknob(&s, 0, k0);   /* the lane as its block again (not edited): a tap on it below is an echo */
    seq_jam_queue(&s, LANE_BASS, 5, 2);   /* BAR: the bar line a part switch takes too */
    tag = s.djq[DL_BASS].tag;
    seq_select_part(&s, 1);
    ppq = play_to_k(&s, ppq, s.next_k + floor_mod_l(s.part_origin_k - s.next_k, SEQ_STEPS) + 4);
    ok = seq_part(&s) == 1 && !seq_jam_queued(&s, LANE_BASS, NULL, NULL) && seq_jam_playing(&s, LANE_BASS, &c, &l);
    seq_rbar_open(&s, LANE_CHORDS, 0);   /* on the Intro */
    seq_jam_queue(&s, LANE_BASS, c, l);   /* the playing block, nothing queued: a host echo */
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(ok && ring_has_tag(&s, tag) && s.djq_drop[DL_BASS] == tag, "a part switch dropped the queue; a tap and a CANCEL with a bar open: its entry still waits for the sweep");
    n0 = s.undo_n;
    seq_undo(&s);
    CHECK(!ring_has_tag(&s, tag) && s.undo_n <= n0 - 2, "the next UNDO sweeps it and undoes a real step");
    seq_block(&s, ppq, bpm, 0, 128);
}

/* A VARIATION or TIME turn, COMPLEXITY, a RHYTHM bar lock, a BASS knob, a chord and the key, while blocks wait on both lanes: each renders
 * the queued block again (one scratch render for a JAM knob), so the block that takes over has them all */
static void test_22_queue_follows_knobs(void) {
    static seq_t s;
    static djq_t was[DL_COUNT];
    int ok = 1, moved = 1, one;
    unsigned r0;
    double ppq;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    nev = 0;
    ppq = play_to_k(&s, 0, 21);
    seq_jam_queue(&s, LANE_CHORDS, 2, 3);
    seq_jam_queue(&s, LANE_BASS, 5, 2);
    ok &= jq_fresh(&s, LANE_CHORDS) && jq_fresh(&s, LANE_BASS);
    memcpy(was, s.djq, sizeof was);
    r0 = s.djq_renders;
    seq_djam_set(&s, LANE_CHORDS, DJ_VAR, 3);
    one = s.djq_renders == r0 + 1;
    ok &= jq_fresh(&s, LANE_CHORDS) && jq_fresh(&s, LANE_BASS);
    moved &= memcmp(was[DL_CHORDS].steps, s.djq[DL_CHORDS].steps, sizeof was[0].steps) != 0 && !memcmp(&was[DL_BASS], &s.djq[DL_BASS], sizeof was[0]);
    r0 = s.djq_renders;
    seq_djam_set(&s, LANE_BASS, DJ_TIME, 1);
    one &= s.djq_renders == r0 + 1;
    ok &= jq_fresh(&s, LANE_BASS);
    moved &= memcmp(was[DL_BASS].steps, s.djq[DL_BASS].steps, sizeof was[0].steps) != 0;
    CHECK(ok && moved && one, "CHORDS VARIATION ALL and BASS TIME DOUBLE while queued: each renders its lane's queued block again, once");
    memcpy(was, s.djq, sizeof was);
    seq_set_complexity(&s, LANE_CHORDS, 90);
    ok &= jq_fresh(&s, LANE_CHORDS);
    moved &= memcmp(was[DL_CHORDS].steps, s.djq[DL_CHORDS].steps, sizeof was[0].steps) != 0;
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_lock(&s, LANE_BASS);
    seq_rbar_done(&s, LANE_BASS);
    ok &= jq_fresh(&s, LANE_BASS) && s.djq[DL_BASS].nlock == 1 << 2 && s.djq[DL_BASS].rb[2].from != RJAM_BASE + 5 * JAM_LEVELS + 2;
    seq_set_bknob(&s, BK_MOV, (seq_bknob(&s, BK_MOV) + 2) % 5);   /* 0.24 LOCK note-locked bar 3 itself: it keeps the settings it had then */
    ok &= jq_fresh(&s, LANE_BASS) && !memcmp(&s.djq[DL_BASS].frozen[2], &s.part[0].bass.frozen[2], sizeof s.part[0].bass.set) &&
          memcmp(&s.part[0].bass.frozen[2], &s.part[0].bass.set, sizeof s.part[0].bass.set);
    memcpy(was, s.djq, sizeof was);
    seq_set_slot(&s, 3, 5, 0, COL_TRIAD);   /* bar 4's chord */
    ok &= jq_fresh(&s, LANE_CHORDS) && jq_fresh(&s, LANE_BASS);
    moved &= memcmp(was[DL_CHORDS].voice, s.djq[DL_CHORDS].voice, sizeof was[0].voice) != 0;
    seq_set_key(&s, 7);
    ok &= jq_fresh(&s, LANE_CHORDS) && jq_fresh(&s, LANE_BASS);
    CHECK(ok && moved, "COMPLEXITY, a BASS bar locked on RHYTHM (auto note-locked in the queued block), MOVEMENT, a chord and the key: each too");
    ppq = play_to_k(&s, ppq, 40);
    CHECK(seq_jam_playing(&s, LANE_CHORDS, NULL, NULL) && seq_jam_playing(&s, LANE_BASS, NULL, NULL) && seq_djam_get(&s, LANE_CHORDS, DJ_VAR) == 3 &&
              seq_djam_get(&s, LANE_BASS, DJ_TIME) == 1 && s.part[0].bass.nlock == 1 << 2 && seq_key(&s) == 7 && jlane_is_fresh(&s, LANE_CHORDS) &&
              jlane_is_fresh(&s, LANE_BASS), "after the switch both lanes are what their setters render, with all of them");
    seq_block(&s, ppq, bpm, 0, 128);
}

/* A part switch, a state load and INIT drop a queue (UNDO then skips a dropped tap's step); a block whose boundary is at or after a queued
 * part switch's bar line is refused; a stop lands it at once; a queue is never saved */
static void test_22_queue_dropped(void) {
    static seq_t s;
    static seq_part_t was;
    static char a[SEQ_STATE_MAX], b[SEQ_STATE_MAX];
    double ppq;
    int c = -1, l = -1;
    seq_init(&s, emit, NULL);   /* 4 bars, SWITCH AT BAR */
    was = s.part[0];
    nev = 0;
    ppq = play_to_k(&s, 0, 21);
    seq_set_step(&s, LANE_BASS, 3, 5, !s.part[0].lane[LANE_BASS].steps[3 * SEQ_STEPS + 5].on);   /* a Bass step drawn: one undo step */
    seq_jam_queue(&s, LANE_CHORDS, 1, 1);
    seq_select_part(&s, 1);   /* the bar line both wait for */
    ppq = play_to_k(&s, ppq, 40);
    int polled = seq_jam_poll(&s), queued;
    CHECK(seq_part(&s) == 1 && !seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && !polled && s.part[0].djam[DL_CHORDS].c == -1 &&
              !memcmp(s.part[0].rb[DL_CHORDS], was.rb[DL_CHORDS], sizeof was.rb[DL_CHORDS]) && !memcmp(s.part[0].voice, was.voice, sizeof was.voice),
          "a part switch at the block's boundary: the switch wins, the queue dropped, the Verse's Chords as they were");
    seq_undo(&s);
    CHECK(!seq_can_undo(&s) && !memcmp(s.part[0].rb, was.rb, sizeof was.rb), "UNDO then skips the dropped tap's step: the Bass step drawn before it is undone");
    seq_select_part(&s, 0);
    seq_jam_queue(&s, LANE_BASS, 2, 2);
    CHECK(seq_queued_part(&s) == 0 && !seq_jam_queued(&s, LANE_BASS, NULL, NULL),
          "the Verse queued for the Intro's next bar line: a BASS block at BAR (that bar line) is refused");
    seq_djam_set(&s, LANE_BASS, DJ_SW, 0);
    queued = seq_jam_queue(&s, LANE_BASS, 2, 2);
    CHECK(queued && seq_jam_queued(&s, LANE_BASS, &c, &l) && c == 2 && l == 2,
          "at BEAT (before the bar line) it queues");
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(seq_part(&s) == 0 && !seq_jam_queued(&s, LANE_BASS, NULL, NULL) && s.part[1].djam[DL_BASS].c == -1,
          "a stop with a part queued: the part switches, the block on the Intro dropped (as MELODY)");
    seq_init(&s, emit, NULL);
    ppq = play_to_k(&s, 0, 21);
    seq_state_save(&s, a, sizeof a);
    seq_jam_queue(&s, LANE_BASS, 2, 2);
    seq_jam_queue(&s, LANE_CHORDS, 2, 2);
    seq_state_save(&s, b, sizeof b);
    CHECK(seq_jam_queued(&s, LANE_BASS, NULL, NULL) && !strcmp(a, b), "a save while blocks wait writes what plays (a queue is never saved)");
    seq_state_load(&s, a);
    CHECK(!seq_jam_queued(&s, LANE_BASS, NULL, NULL) && !seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && !seq_can_undo(&s), "a load drops both queues");
    ppq = play_to_k(&s, ppq, 40);
    polled = seq_jam_poll(&s);
    CHECK(s.part[0].djam[DL_BASS].c == -1 && s.part[0].djam[DL_CHORDS].c == -1 && !polled, "and nothing switches at the bar line");
    seq_jam_queue(&s, LANE_BASS, 2, 2);
    seq_jam_queue(&s, LANE_CHORDS, 2, 2);
    seq_init_preset(&s);
    CHECK(!seq_jam_queued(&s, LANE_BASS, NULL, NULL) && !seq_jam_queued(&s, LANE_CHORDS, NULL, NULL), "INIT drops both queues");
    ppq = play_to_k(&s, ppq, 72);
    polled = seq_jam_poll(&s);
    CHECK(s.part[0].djam[DL_BASS].c == -1 && s.part[0].djam[DL_CHORDS].c == -1 && !polled, "and nothing switches");
    seq_jam_queue(&s, LANE_BASS, 2, 2);
    seq_block(&s, ppq, bpm, 0, 128);
    polled = seq_jam_poll(&s);
    CHECK(!seq_jam_queued(&s, LANE_BASS, NULL, NULL) && seq_jam_playing(&s, LANE_BASS, &c, &l) && c == 2 && l == 2 && polled == 1 << DL_BASS &&
              jlane_is_fresh(&s, LANE_BASS), "a stop before the boundary: the block takes over at once (as MELODY)");
}

/* 0.22 Task 5: SWING on the Chords and Bass JAM lanes, and the weak flags */

/* a lane's note-ons logged since nev = 0 by the step that fired them, in the order they went out: n notes, each note, velocity and the
 * block position it went out in */
#define DJ_NMAX 8
typedef struct { int n, note[DJ_NMAX], vel[DJ_NMAX]; double on[DJ_NMAX]; } step_ons_t;

/* 1 when no step played a note twice */
static int lane_step_ons(const seq_t *s, int lane, step_ons_t *e) {
    int ch = seq_channel_of(s, lane), ok = 1;
    memset(e, 0, sizeof(step_ons_t) * MJ_KMAX);
    for (int i = 0; i < nev; i++) {
        step_ons_t *x;
        if (!is_on(&evs[i]) || (evs[i].status & 15) != ch - 1 || evs[i].k < 0 || evs[i].k >= MJ_KMAX) continue;
        x = &e[evs[i].k];
        for (int j = 0; j < x->n; j++) ok &= x->note[j] != evs[i].note;
        if (x->n < DJ_NMAX) x->note[x->n] = evs[i].note, x->vel[x->n] = evs[i].vel, x->on[x->n++] = evs[i].ppq;
    }
    return ok;
}

/* 1 when step x is step r with each note-on moved by `by` beats (within a block each way): the same notes, velocities, order and spacing */
static int ons_moved(const step_ons_t *x, const step_ons_t *r, double by) {
    int ok = x->n == r->n;
    for (int j = 0; ok && j < r->n; j++) ok &= x->note[j] == r->note[j] && x->vel[j] == r->vel[j] && fabs(x->on[j] - r->on[j] - by) <= blk() + 1e-9;
    return ok;
}

/* one pass of the loop (and a beat after) on drawn lane `lane` at its JAM SWING sw against SWING 0: 1 when the lane's grid is g16, and
 * each step on the grid (g16: an odd step, else p mod 4 = 2) goes out d beats after its step (from step_time's offset g on odd steps: PLAY's
 * SWING, which the larger of the two replaces), every other step where it was, the same notes, velocities and STRUM spacing, none twice,
 * none hanging; *moved, *kept: how many steps of each */
static int dj_swing_pass(seq_t *s, int lane, int sw, int g16, double d, double g, int *moved, int *kept) {
    static step_ons_t ref[MJ_KMAX], e[MJ_KMAX];
    int n = seq_bars(s) * SEQ_STEPS, ok;
    *moved = *kept = 0;
    seq_djam_set(s, lane, DJ_SWING, 0);
    nev = 0;
    play_stop(s, n / 4.0 + 1.0);
    ok = lane_step_ons(s, lane, ref) && balanced();
    seq_djam_set(s, lane, DJ_SWING, sw);
    nev = 0;
    play_stop(s, n / 4.0 + 1.0);
    ok &= lane_step_ons(s, lane, e) && balanced() && seq_djam_grid16(s, lane) == g16 && seq_djam_get(s, lane, DJ_SWING) == sw;
    for (int k = 0; k < n; k++) {
        int late = g16 ? k & 1 : k % 4 == 2;
        ok &= ons_moved(&e[k], &ref[k], late ? d - (k & 1 ? g : 0) : 0);
        if (ref[k].n) *moved += late, *kept += !late;
    }
    return ok;
}

/* the 16th grid (an unlocked bar's codes on an odd step) and the 8th grid (none) on both lanes, at 70% and 58%; a locked bar plays
 * straight (and never sets the grid); the 8th grid's cap (a swung hit 1/8 beat before the bar's next hit: FILL's 16ths on the loop's
 * bar 4, which never set the grid); PLAY's SWING against the lane's (the larger, never the sum); PUSH's chord starts never swung (on
 * beats), the swung step before them still first */
static void test_22_swing_grid(void) {
    static seq_t s;
    static step_ons_t e[MJ_KMAX];
    static const int LANE[2] = {LANE_CHORDS, LANE_BASS}, C16[2] = {0, 0}, L16[2] = {3, 4}, C8[2] = {3, 3}, L8[2] = {4, 2},   /* ORGAN / GALLOP; DRIVE 8S / ROOT 8S */
                     CAP_L[2] = {1, 0};   /* HOUSE OFFBEAT */
    seq_part_t *p = &s.part[0];
    int moved, kept, ok, ch;
    for (int i = 0; i < 2; i++) {
        const char *ln = i ? "BASS" : "CHORDS";
        seq_init(&s, emit, NULL);
        seq_jam_apply_now(&s, LANE[i], C16[i], L16[i]);
        CHECK(seq_djam_grid16(&s, LANE[i]) == 1, "%s %s: odd hits, the 16th grid", ln, jam_block_name(i, C16[i], L16[i]));
        ok = dj_swing_pass(&s, LANE[i], 5, 1, 0.1, 0, &moved, &kept);
        CHECK(ok && moved >= 16, "%s %s, SWING 70%%: %d odd steps 0.1 beat late, %d even ones on time, the same notes", ln,
              jam_block_name(i, C16[i], L16[i]), moved, kept);
        ok = dj_swing_pass(&s, LANE[i], 2, 1, 0.04, 0, &moved, &kept);
        CHECK(ok && moved >= 16, "%s SWING 58%%: 0.04 beat late", ln);
        seq_set_swing(&s, 100);
        ok = dj_swing_pass(&s, LANE[i], 1, 1, 1.0 / 12, 1.0 / 12, &moved, &kept);
        ok &= dj_swing_pass(&s, LANE[i], 5, 1, 0.1, 1.0 / 12, &moved, &kept);
        CHECK(ok && moved >= 16, "%s with PLAY SWING 100 (1/12 beat): lane 54%% plays +1/12, lane 70%% +0.1 (the larger, not the sum)", ln);
        seq_set_swing(&s, 0);
        seq_jam_apply_now(&s, LANE[i], C8[i], L8[i]);
        CHECK(seq_djam_grid16(&s, LANE[i]) == 0, "%s %s: no odd hit, the 8th grid", ln, jam_block_name(i, C8[i], L8[i]));
        ok = dj_swing_pass(&s, LANE[i], 5, 0, 0.2, 0, &moved, &kept);
        CHECK(ok && moved >= 16 && kept >= 16, "%s %s, SWING 70%%: %d off-beat 8ths 0.2 beat late, %d on time", ln, jam_block_name(i, C8[i], L8[i]), moved, kept);
        seq_set_swing(&s, 100);
        ok = dj_swing_pass(&s, LANE[i], 5, 0, 0.2, 1.0 / 12, &moved, &kept);
        CHECK(ok, "%s 8th grid with PLAY SWING 100: off-beat 8ths 0.2 late, the rest as PLAY's SWING plays them", ln);
        seq_set_swing(&s, 0);
        /* a locked bar plays straight: bar 1 locked with a hit on step 4 too (a locked bar never sets the grid) */
        p->rb[i][0].c[3] = RC_HIT, p->rb[i][0].lock = 1;
        render_now(&s);
        seq_djam_set(&s, LANE[i], DJ_SWING, 5);
        nev = 0;
        play_stop(&s, 5.0);
        lane_step_ons(&s, LANE[i], e);
        CHECK(seq_djam_grid16(&s, LANE[i]) == 0 && e[2].n && e[3].n && at_t(e[2].on[0], 0.5) && at_t(e[3].on[0], 0.75) && at_t(e[6].on[0], 1.5) &&
                  at_t(e[18].on[0], 4.7) && balanced(),
              "%s: locked bar 1 plays straight (steps 3, 4, 7 at 0.5, 0.75, 1.5), bar 2's off-beat 8ths 0.2 late", ln);
        /* the cap on an unlocked bar: OFFBEAT at FILL 1, whose fill bar (bar 4) adds step 16 after the swung step 15 */
        seq_init(&s, emit, NULL);
        seq_jam_apply_now(&s, LANE[i], 0, CAP_L[i]);
        seq_djam_set(&s, LANE[i], DJ_SWING, 5);
        seq_set_rknob(&s, LANE[i], RK_FILL, 1);
        nev = 0;
        play_stop(&s, 17.0);
        lane_step_ons(&s, LANE[i], e);
        CHECK(!seq_djam_grid16(&s, LANE[i]) && !p->rb[i][3].lock && e[62].n && e[63].n && at_t(e[62].on[0], 15.625) && at_t(e[63].on[0], 15.75) &&
                  at_t(e[14].on[0], 3.7) && at_t(e[46].on[0], 11.7) && balanced(),
              "%s OFFBEAT, FILL 1 (8th grid): bar 4's step 15 plays 1/8 beat before its fill's step 16 (15.625, not 15.7); bars 1-3's 0.2 late", ln);
    }
    /* PUSH: HOUSE JACKIN (16th grid) at SWING 70% and PUSH 100: bar 2's chord start pushed to 3.875, never swung; step 16 at 3.85 first */
    seq_init(&s, emit, NULL);
    seq_jam_apply_now(&s, LANE_CHORDS, 0, 4);
    seq_djam_set(&s, LANE_CHORDS, DJ_SWING, 5);
    seq_set_lane_shape(&s, LANE_CHORDS, SHAPE_PUSH, 100);
    nev = 0;
    play_stop(&s, 4.5);
    lane_step_ons(&s, LANE_CHORDS, e);   /* the pushed chord goes out while step 16 is the last fired: by time below */
    ok = balanced();
    ch = seq_channel_of(&s, LANE_CHORDS);
    CHECK(ok && seq_djam_grid16(&s, LANE_CHORDS) && e[15].n && at_t(e[15].on[0], 3.85) && ons_near(0, ch, 3.875) && !ons_between(0, ch, 3.85 + blk() + 1e-9, 3.875) &&
              !ons_between(0, ch, 3.875 + blk() + 1e-9, 4.1),
          "PUSH 100: bar 2's chord start pushed to 3.875 (never swung), bar 1's step 16 swung to 3.85 before it");
}

/* the first note-on on channel ch logged since nev = 0 for event time t (its block); -1 none */
static int on_at(int ch, double t) {
    for (int i = 0; i < nev; i++)
        if (is_on(&evs[i]) && (evs[i].status & 15) == ch - 1 && at_t(evs[i].ppq, t)) return evs[i].note;
    return -1;
}

/* Review Focus: SWING with HUMANIZE and STRUM. CHORDS STRUM 16 (STRUM 4, odd hits) and BASS GALLOP at HUMANIZE 4: against HUMANIZE alone,
 * SWING 70% moves every odd step's notes 0.1 beat later on top of HUMANIZE's delay (no delay lost), the same notes, velocities and STRUM
 * spacing, none twice, none hanging. And a swung chord drops only the strum notes due once it sounds: the chord before still plays its
 * last note at 0.75, before the swung chord at 0.85 */
static void test_22_swing_hum_strum(void) {
    static seq_t s;
    static step_ons_t ref[MJ_KMAX], e[MJ_KMAX];
    static const int LANE[2] = {LANE_CHORDS, LANE_BASS}, BC[2] = {4, 0}, BL[2] = {3, 4};
    int n5 = 0, ch, ref_on, ref_top, top[THEORY_CHORD_MAX] = {0};
    for (int i = 0; i < 2; i++) {
        int n, ok, hum = 0, moved = 0;
        seq_init(&s, emit, NULL);
        seq_jam_apply_now(&s, LANE[i], BC[i], BL[i]);
        seq_set_rknob(&s, LANE[i], RK_HUM, 4);
        n = seq_bars(&s) * SEQ_STEPS;
        nev = 0;
        play_stop(&s, n / 4.0 + 1.0);
        ok = lane_step_ons(&s, LANE[i], ref) && balanced();
        seq_djam_set(&s, LANE[i], DJ_SWING, 5);
        nev = 0;
        play_stop(&s, n / 4.0 + 1.0);
        ok &= lane_step_ons(&s, LANE[i], e) && balanced() && seq_djam_grid16(&s, LANE[i]);
        for (int k = 0; k < n; k++) {
            ok &= ons_moved(&e[k], &ref[k], k & 1 ? 0.1 : 0);
            hum += ref[k].n && ref[k].on[0] > k / 4.0 + blk() + 1e-9;
            moved += (k & 1) && ref[k].n;
        }
        CHECK(ok && hum >= 8 && moved >= 16 && (i || ref[3].n >= 3),
              "%s %s, HUMANIZE 4 (%d hits delayed) + SWING 70%%: %d odd steps 0.1 later than HUMANIZE alone, notes, velocities and strum "
              "spacing kept, none twice, none hanging", i ? "BASS" : "CHORDS", jam_block_name(i, BC[i], BL[i]), hum, moved);
    }
    /* a new swung chord never drops the chord before it while that one's strum notes are still due before it sounds: STRUM 16 (16ths) at
     * STRUM 8 with a 5-note chord: step 3's notes at 0.5 + i/16, the 5th at 0.75, where step 4's chord sounds unswung */
    seq_init(&s, emit, NULL);
    seq_jam_apply_now(&s, LANE_CHORDS, 4, 3);
    seq_set_complexity(&s, LANE_CHORDS, CHORDS_CX_NEUTRAL);
    for (int c = -1; c <= 4 && n5 != 5; c++) {
        seq_set_vknob(&s, VK_COL, c);
        n5 = seq_part_step_notes(&s, 0, LANE_CHORDS, 0, 2, top);
    }
    seq_set_vknob(&s, VK_STR, 8);
    ch = seq_channel_of(&s, LANE_CHORDS);
    nev = 0;
    play_stop(&s, 1.5);
    ref_on = ons_between(0, ch, 0.5 - 1e-9, 0.75), ref_top = on_at(ch, 0.75);   /* SWING 50%: step 4's chord at 0.75 drops the 5th */
    seq_djam_set(&s, LANE_CHORDS, DJ_SWING, 5);
    nev = 0;
    play_stop(&s, 1.5);
    CHECK(n5 == 5 && ref_on == 4 && ref_top != top[4] && ons_between(0, ch, 0.5 - 1e-9, 0.75) == 4 && ons_near(0, ch, 0.75) == 1 && on_at(ch, 0.75) == top[4] &&
              ons_between(0, ch, 0.75 + blk() + 1e-9, 0.85) == 0 && ons_near(0, ch, 0.85) == 1 && balanced(),
          "STRUM 8, 5 notes: step 3's 5th note (due 0.75) is dropped by step 4's chord at 0.75, but kept before it swung to 0.85");
}

/* a block queued with another swing grid installs its own at the switch, with nothing rendered between, and swings on it at once; its weak
 * flags read 0 until the next setter */
static void test_22_swing_queue_grid(void) {
    static seq_t s;
    static step_ons_t e[MJ_KMAX];
    static const int LANE[2] = {LANE_CHORDS, LANE_BASS}, FC[2] = {3, 0}, FL[2] = {4, 4}, TC[2] = {0, 3}, TL[2] = {3, 2};   /* 8ths -> ORGAN; GALLOP -> ROOT 8S */
    for (int i = 0; i < 2; i++) {
        const char *ln = i ? "BASS" : "CHORDS";
        int g0, g1;
        double ppq;
        seq_init(&s, emit, NULL);
        seq_jam_apply_now(&s, LANE[i], TC[i], TL[i]);
        g1 = seq_djam_grid16(&s, LANE[i]);
        seq_jam_apply_now(&s, LANE[i], FC[i], FL[i]);
        g0 = seq_djam_grid16(&s, LANE[i]);
        seq_djam_set(&s, LANE[i], DJ_SWING, 5);
        nev = 0;
        ppq = play_to_k(&s, 0, 21);
        seq_jam_queue(&s, LANE[i], TC[i], TL[i]);
        CHECK(g0 != g1 && seq_djam_grid16(&s, LANE[i]) == g0, "%s: %s on %s, %s queued (on %s): still the playing block's grid", ln,
              jam_block_name(i, FC[i], FL[i]), g0 ? "16ths" : "8ths", jam_block_name(i, TC[i], TL[i]), g1 ? "16ths" : "8ths");
        ppq = play_to_k(&s, ppq, 48);
        CHECK(!seq_jam_queued(&s, LANE[i], NULL, NULL) && seq_djam_grid16(&s, LANE[i]) == g1 && seq_djam_weak(&s, LANE[i]) == 0,
              "%s: after the switch at bar 3, nothing rendered: the new block's grid, weak flags 0", ln);
        lane_step_ons(&s, LANE[i], e);
        int ok = 1, sw = 0;
        for (int k = 32; k < 47; k++) {
            int late = g1 ? k & 1 : k % 4 == 2;
            if (!e[k].n) continue;
            ok &= at_t(e[k].on[0], k / 4.0 + (late ? (g1 ? 0.1 : 0.2) : 0));
            sw += late;
        }
        seq_block(&s, ppq, bpm, 0, 128);
        CHECK(ok && sw >= 4 && balanced(), "%s: bar 3 swings on the new grid at once (%d swung steps)", ln, sw);
    }
}

/* the changed and total counts of a drawn lane's knob step, measured as the weak rule reads it: the loop's steps whose sounding notes
 * (a held step its hit's, within its bar; none silence) or note start differ from the same lane with the knob at neutral, of bars x 16;
 * SWING: the hits of the bars not locked for JAM on the swung positions (grid16: odd steps, else step mod 4 = 2), of those hits */
typedef struct { int on[SEQ_LANE_STEPS], n[SEQ_LANE_STEPS], note[SEQ_LANE_STEPS][THEORY_CHORD_MAX]; } lane_snd_t;
static void lane_sound(const seq_t *s, int lane, lane_snd_t *o) {
    const seq_part_t *p = &s->part[s->cur_part];
    for (int b = 0; b < p->bars; b++) {
        int cur = -1, end = 0;
        for (int i = b * SEQ_STEPS; i < (b + 1) * SEQ_STEPS; i++) {
            const seq_step_t *x = &p->lane[lane].steps[i];
            if (x->on) cur = i, end = i + x->len;
            o->on[i] = x->on;
            o->n[i] = cur >= 0 && i < end ? seq_part_step_notes(s, s->cur_part, lane, b, cur % SEQ_STEPS, o->note[i]) : 0;
        }
    }
}
static int dj_measure(seq_t *s, int lane, int field, int v, int *total) {
    static lane_snd_t a, b;
    int n = seq_bars(s) * SEQ_STEPS, c = 0;
    seq_djam_set(s, lane, field, 0);
    lane_sound(s, lane, &a);
    seq_djam_set(s, lane, field, v);
    lane_sound(s, lane, &b);
    if (field == DJ_SWING) {
        const seq_part_t *p = &s->part[s->cur_part];
        int g16 = seq_djam_grid16(s, lane), d = lane == LANE_CHORDS ? DL_CHORDS : DL_BASS;
        *total = 0;
        for (int i = 0; i < n; i++)
            if (b.on[i] && !p->rb[d][i / SEQ_STEPS].lock && !(d == DL_BASS && p->bass.nlock >> (i / SEQ_STEPS) & 1))
                ++*total, c += g16 ? i & 1 : i % 4 == 2;
        return c;
    }
    *total = n;
    for (int i = 0; i < n; i++)
        c += a.on[i] != b.on[i] || a.n[i] != b.n[i] || memcmp(a.note[i], b.note[i], sizeof(int) * (size_t)a.n[i]);
    return c;
}

/* each weak flag set exactly by the rule (fewer than 1 in 8 changed), none at neutral; known cases both ways; a queued block's flags none,
 * the playing lane's kept while it waits; 0 after it takes over until the next setter */
static void test_22_weak(void) {
    static seq_t s;
    static const int F[3] = {DJ_VAR, DJ_SWING, DJ_TIME}, LO[3] = {0, 0, -1}, HI[3] = {3, 5, 1};
    static const unsigned BIT[3] = {DW_VAR, DW_SWING, DW_TIME};
    static const int LANE[2] = {LANE_CHORDS, LANE_BASS};
    int ok = 1, cases = 0, weak = 0, total;
    double ppq;
    CHECK(DW_VAR == 1 && DW_SWING == 2 && DW_TIME == 4, "DW_VAR 1, DW_SWING 2, DW_TIME 4");
    for (int i = 0; i < 2; i++)
        for (int c = 0; c < JAM_COLS; c++)
            for (int l = 0; l < JAM_LEVELS; l += 2) {
                seq_init(&s, emit, NULL);
                seq_jam_apply_now(&s, LANE[i], c, l);
                ok &= seq_djam_weak(&s, LANE[i]) == 0;
                for (int f = 0; f < 3; f++) {
                    for (int v = LO[f]; v <= HI[f]; v++) {
                        int ch, w;
                        if (!v) continue;
                        ch = dj_measure(&s, LANE[i], F[f], v, &total);
                        w = ch * 8 < total;
                        ok &= seq_djam_weak(&s, LANE[i]) == (w ? BIT[f] : 0u);
                        cases++, weak += w;
                    }
                    seq_djam_set(&s, LANE[i], F[f], 0);
                    ok &= seq_djam_weak(&s, LANE[i]) == 0;
                }
            }
    CHECK(ok && weak > 0 && weak < cases, "%d cases (both lanes, 3 levels of each genre): the flag set exactly when fewer than 1 in 8 changed "
          "(%d weak), none at neutral", cases, weak);
    seq_init(&s, emit, NULL);
    seq_jam_apply_now(&s, LANE_CHORDS, 1, 0);   /* DRONE 5TH: one held chord a bar */
    seq_djam_set(&s, LANE_CHORDS, DJ_TIME, -1);
    CHECK(seq_djam_weak(&s, LANE_CHORDS) == DW_TIME, "DRONE 5TH at TIME HALF: the same held chord, weak");
    seq_djam_set(&s, LANE_CHORDS, DJ_TIME, 0);
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 2);   /* PIANO 4S: beats only */
    seq_djam_set(&s, LANE_CHORDS, DJ_SWING, 3);
    CHECK(seq_djam_weak(&s, LANE_CHORDS) == DW_SWING && !seq_djam_grid16(&s, LANE_CHORDS), "PIANO 4S at SWING 62%%: nothing on the off-beat 8ths, weak");
    s.part[0].rb[DL_CHORDS][0].lock = 1;
    rhythm_parse(jam_block_codes(JL_CHORDS, 3, 4), s.part[0].rb[DL_CHORDS][0].c);   /* bar 1 locked at DRIVE 8S: plays straight */
    render_now(&s);
    ok = dj_measure(&s, LANE_CHORDS, DJ_SWING, 3, &total) * 8 < total;
    CHECK(ok && total == 12 && seq_djam_weak(&s, LANE_CHORDS) == DW_SWING, "and with bar 1 locked at DRIVE 8S: its off-beat 8ths play straight and "
          "never count (0 of the other bars' %d hits), still weak", total);
    s.part[0].rb[DL_CHORDS][0].lock = 0;
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 4);   /* DRIVE 8S: half its hits on them */
    CHECK(seq_djam_weak(&s, LANE_CHORDS) == 0, "DRIVE 8S at SWING 62%%: not weak");
    seq_jam_apply_now(&s, LANE_CHORDS, 4, 0);   /* RING OUT: ALL BARS varies 3 bars of 4: the inversion moves every sounding step */
    seq_djam_set(&s, LANE_CHORDS, DJ_SWING, 0);
    seq_djam_set(&s, LANE_CHORDS, DJ_VAR, 3);
    CHECK(seq_djam_weak(&s, LANE_CHORDS) == 0, "RING OUT at VARIATION ALL: not weak");
    seq_djam_set(&s, LANE_CHORDS, DJ_VAR, 0);
    /* queued: none; after it takes over 0 until a setter renders the lane */
    seq_jam_apply_now(&s, LANE_CHORDS, 3, 4);   /* DRIVE 8S */
    seq_djam_set(&s, LANE_CHORDS, DJ_SWING, 3);
    nev = 0;
    ppq = play_to_k(&s, 0, 21);
    seq_jam_queue(&s, LANE_CHORDS, 3, 2);   /* PIANO 4S: weak at SWING 62% */
    CHECK(seq_djam_weak(&s, LANE_CHORDS) == 0, "PIANO 4S queued: the playing DRIVE 8S's flags (none)");
    ppq = play_to_k(&s, ppq, 40);
    CHECK(seq_jam_playing(&s, LANE_CHORDS, NULL, NULL) && !seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && seq_djam_weak(&s, LANE_CHORDS) == 0,
          "after the switch: 0 (no render at the boundary)");
    seq_djam_set(&s, LANE_CHORDS, DJ_SWING, 4);
    CHECK(seq_djam_weak(&s, LANE_CHORDS) == DW_SWING, "the next setter (SWING 66%%): PIANO 4S's SWING flag");
    ok = dj_measure(&s, LANE_CHORDS, DJ_VAR, 1, &total) * 8 < total;   /* ends at VARIATION BAR 4, a render */
    CHECK(seq_djam_weak(&s, LANE_CHORDS) == (DW_SWING | (ok ? DW_VAR : 0u)),
          "and VARIATION BAR 4 renders the lane: its flags by the rule");
    seq_block(&s, ppq, bpm, 0, 128);
    /* VARIATION and TIME both off neutral: each flag against its own neutral with the other knob kept */
    {
        int both = 0, wv = 0, wt = 0, n = 0;
        ok = 1;
        for (int i = 0; i < 2; i++)
            for (int c = 0; c < JAM_COLS; c++)
                for (int l = 0; l < JAM_LEVELS; l += 2)
                    for (int v = 1; v <= 3; v++)
                        for (int t = -1; t <= 1; t += 2) {
                            unsigned want;
                            int tv, tt, cv, ct;
                            seq_init(&s, emit, NULL);
                            seq_jam_apply_now(&s, LANE[i], c, l);
                            seq_djam_set(&s, LANE[i], DJ_TIME, t);
                            cv = dj_measure(&s, LANE[i], DJ_VAR, v, &tv);   /* VARIATION OFF -> v at TIME t */
                            ct = dj_measure(&s, LANE[i], DJ_TIME, t, &tt);  /* TIME NORMAL -> t at VARIATION v */
                            want = (cv * 8 < tv ? DW_VAR : 0u) | (ct * 8 < tt ? DW_TIME : 0u);
                            ok &= seq_djam_weak(&s, LANE[i]) == want;
                            n++, wv += !!(want & DW_VAR), wt += !!(want & DW_TIME), both += want == (DW_VAR | DW_TIME);
                        }
        CHECK(ok && wv > 0 && wt > 0, "%d cases with VARIATION and TIME both off neutral: each flag by the rule against its own neutral, the other "
              "kept (VARIATION weak %d, TIME weak %d, both %d)", n, wv, wt, both);
    }
}

/* 0.23 `gn`: the genre preset last loaded (src/presets.c), 1..6; 0 none, never written then */
static void test_23_preset_field(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(seq_preset_loaded(&s) == 0 && !strstr(buf, "|gn="), "a new instance has no preset and writes no gn");
    seq_set_preset_loaded(&s, 3);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(seq_preset_loaded(&s) == 3 && strstr(buf, "|gn=3"), "preset 3 saves as gn=3");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_preset_loaded(&t) == 3, "it loads back");
    seq_set_preset_loaded(&t, 99);
    seq_set_preset_loaded(&t, -1);
    CHECK(seq_preset_loaded(&t) == 3, "a bad value is ignored");
    seq_init_preset(&t);
    CHECK(seq_preset_loaded(&t) == 0, "INIT clears it");
    seq_init(&s, emit, NULL);
    seq_state_save(&s, buf, sizeof buf);
    seq_set_preset_loaded(&t, 2);
    CHECK(seq_state_load(&t, buf) && seq_preset_loaded(&t) == 0, "a state without gn loads as none");
    strcat(buf, "|gn=x");
    seq_set_preset_loaded(&t, 2);
    CHECK(seq_state_load(&t, buf) && seq_preset_loaded(&t) == 0, "a bad gn loads as none, never refused");
    seq_init(&s, emit, NULL);
    seq_set_key(&s, 4);
    for (int l = 0; l < 3; l++) seq_set_channel(&s, l, l + 1);
    seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    for (int l = 0; l < 3; l++) seq_set_channel(&t, l, l + 5);
    seq_set_follow(&t, 0);
    CHECK(seq_song_load(&t, buf) && seq_key(&t) == 4 && seq_channel_of(&t, 0) == 5 && seq_channel_of(&t, 2) == 7 && !seq_follow(&t),
          "seq_song_load loads the song (E) and keeps the channels (5 6 7) and FOLLOW (off)");
    CHECK(!seq_song_load(&t, "csq1:x") && !seq_song_load(&t, NULL) && seq_key(&t) == 4, "it takes csq2 only");
}

/* 0.24 change points (virtual slots). A new instance in E minor, Em C G D at 1-bar chords grown to 8 bars: bar 6 (index 5) is C */
static void cp_setup(seq_t *s) {
    nev = 0;
    seq_init(s, emit, NULL);
    seq_set_bars(s, 8);
}

/* 1 when every pitch class of notes[0..n) is in the set of m pcs, and n > 0 */
static int pcs_in(const int *notes, int n, const int *pcs, int m) {
    for (int i = 0; i < n; i++) {
        int in = 0;
        for (int j = 0; j < m; j++) in |= ((notes[i] % 12) + 12) % 12 == pcs[j];
        if (!in) return 0;
    }
    return n > 0;
}

/* a hit on the step of the bar on a drawn lane (seq_rbar_paint toggles: a step already a hit stays one) */
static void cp_hit(seq_t *s, int lane, int bar, int step) {
    unsigned char c[16];
    seq_bar_codes(s, lane, bar, c);
    if (c[step] == RC_HIT) return;
    seq_rbar_open(s, lane, bar);
    seq_rbar_paint(s, lane, step, RC_HIT);
    seq_rbar_done(s, lane);
}

static void test_24_cp_slot_at(void) {
    static seq_t s;
    char buf[32];
    seq_cp_t c;
    int v = SEQ_MAX_PROG + 5 * SEQ_CP_MAX, ok = 1;
    cp_setup(&s);
    CHECK(seq_slot_at(&s, 5, 0) == 5 && s.part[0].prog[5] == 5 && seq_cp_count(&s, 5) == 0, "bar 6 is slot 5, C (VI), no point");
    CHECK(seq_cp_set(&s, 5, 8, 2, 0, 0) && seq_cp_count(&s, 5) == 1, "a point at bar 6 step 9: G (III), Knobs");
    for (int i = 0; i < SEQ_STEPS; i++) ok &= seq_slot_at(&s, 5, i) == (i < 8 ? 5 : v);
    CHECK(ok && s.part[0].prog[v] == 2 && s.part[0].src[v] == 0 && s.part[0].prog_len == 8,
          "steps 1-8 the real slot, 9-16 the virtual slot (prog 2); prog_len stays 8");
    CHECK(seq_cp_get(&s, 5, 0, &c) && c.step == 8 && c.deg == 2 && c.src == 0 && c.type == 0 && !seq_cp_get(&s, 5, 1, &c),
          "seq_cp_get reads it back");
    CHECK(seq_chord_change_at(&s, 5, 8) && !seq_chord_change_at(&s, 5, 7) && !seq_chord_change_at(&s, 5, 9) &&
              seq_chord_change_at(&s, 5, 0) && !seq_chord_change_at(&s, 6, 0) && seq_chord_change_at(&s, 7, 0) && !seq_chord_change_at(&s, 4, 8),
          "a chord starts at the point and at a slot boundary whose chord differs (bar 7's G follows the point's G: none)");
    seq_set_names(&s, 1);
    seq_chord_label(&s, 5, buf, sizeof buf);
    CHECK(!strcmp(buf, "C\xC2\xB7G@9"), "NAMES: bar 6 reads C\xC2\xB7G@9 (%s)", buf);
    seq_chord_label(&s, 4, buf, sizeof buf);
    CHECK(!strcmp(buf, "Em"), "a bar without a point: its chord (%s)", buf);
    seq_set_names(&s, 0);
    seq_chord_label(&s, 5, buf, sizeof buf);
    CHECK(!strcmp(buf, "VI\xC2\xB7III@9"), "degrees: VI\xC2\xB7III@9 (%s)", buf);
    seq_slot_name(&s, seq_slot_at(&s, 5, 12), buf, sizeof buf);
    CHECK(!strcmp(buf, "G") && seq_slot_locked(&s, v) && seq_slot_deg(&s, v) == 2, "the virtual slot reads G, locked (%s)", buf);
    CHECK(!seq_cp_set(&s, 5, 0, 2, 0, 0) && !seq_cp_set(&s, 8, 4, 2, 0, 0) && !seq_cp_set(&s, 5, 16, 2, 0, 0) &&
              !seq_cp_set(&s, 5, 4, 7, 0, 0) && !seq_cp_set(&s, 5, 4, 2, 0, 5) && seq_cp_count(&s, 5) == 1,
          "refused: step 0 of a bar that starts a slot, a bar past the loop, bad step, degree or type");
    CHECK(seq_cp_set(&s, 5, 12, 6, 0, 1) && seq_cp_set(&s, 5, 4, 0, 0, 0) && seq_cp_set(&s, 5, 14, 3, 0, 0) &&
              !seq_cp_set(&s, 5, 2, 3, 0, 0) && seq_cp_count(&s, 5) == 4,
          "four points a bar; a fifth is refused (full)");
    CHECK(seq_cp_get(&s, 5, 0, &c) && c.step == 4 && seq_cp_get(&s, 5, 1, &c) && c.step == 8 && seq_cp_get(&s, 5, 2, &c) && c.step == 12 &&
              c.type == 1 && seq_cp_get(&s, 5, 3, &c) && c.step == 14, "kept sorted by step");
    CHECK(seq_cp_set(&s, 5, 8, 4, 0, 0) && seq_cp_get(&s, 5, 1, &c) && c.deg == 4 && seq_cp_count(&s, 5) == 4, "a step that has one: replaced");
    seq_cp_clear(&s, 5, 4);
    seq_cp_clear(&s, 5, 12);
    seq_cp_clear(&s, 5, 14);
    seq_cp_clear(&s, 5, 3);   /* none there: nothing */
    CHECK(seq_cp_count(&s, 5) == 1 && seq_cp_get(&s, 5, 0, &c) && c.step == 8 && c.deg == 4 && seq_slot_at(&s, 5, 9) == v &&
              s.part[0].prog[v + 1] == 0 && s.part[0].voice_n[v + 1] == 0,
          "cleared: the rest move down, the empty entries mirrored empty");
    seq_cp_clear(&s, 5, 8);
    CHECK(seq_cp_count(&s, 5) == 0 && seq_slot_at(&s, 5, 12) == 5 && !seq_chord_change_at(&s, 5, 8), "all cleared: the slot again");
    seq_cp_set(&s, 5, 4, 2, 0, 0);
    seq_cp_set(&s, 5, 10, 3, 0, 0);
    seq_cp_set(&s, 6, 2, 3, 0, 0);
    seq_cp_clear_bar(&s, 5);
    CHECK(seq_cp_count(&s, 5) == 0 && seq_cp_count(&s, 6) == 1 && seq_slot_at(&s, 5, 12) == 5, "seq_cp_clear_bar clears its bar only");
}

/* every lane follows a point: the Bass's notes, the JAM Melody's chord-tone slots, the Chords' voice and KEYS */
static void test_24_cp_lanes_follow(void) {
    static seq_t s;
    static const int C[3] = {0, 4, 7}, G[3] = {7, 11, 2};
    int n[THEORY_CHORD_MAX], c, bass = 0, mel = 0, mel_c = 0, ok = 1;
    mjam_note_t line[SEQ_STEPS];
    seq_step_t was[SEQ_STEPS];
    cp_setup(&s);
    cp_hit(&s, LANE_BASS, 5, 8);
    cp_hit(&s, LANE_BASS, 5, 12);
    cp_hit(&s, LANE_CHORDS, 5, 8);
    memcpy(was, &s.part[0].lane[LANE_BASS].steps[5 * SEQ_STEPS], sizeof was);
    CHECK(seq_bass_style(&s) == 40 && was[8].on && was[8].abs % 12 == 0 && was[12].on && was[12].abs % 12 == 0,
          "before: Pop Root plays C on steps 9 and 13 of bar 6");
    seq_mel_line(&s, 5, line);
    for (int i = 8; i < SEQ_STEPS; i++)
        if (line[i].note) mel_c |= !pcs_in((const int[]){line[i].note}, 1, G, 3);
    CHECK(mel_c, "before: the JAM Melody has a note off G's tones on steps 9-16 (C's)");
    seq_cp_set(&s, 5, 8, 2, 0, 0);
    for (int i = 0; i < SEQ_STEPS; i++) {
        const seq_step_t *x = &s.part[0].lane[LANE_BASS].steps[5 * SEQ_STEPS + i];
        if (!x->on) continue;
        ok &= x->abs % 12 == (i < 8 ? 0 : 7);
        bass += i >= 8;
    }
    CHECK(ok && bass == 2, "the Bass: C before step 9, G (pc 7) on its hits from step 9");
    seq_mel_line(&s, 5, line);
    ok = 1;
    for (int i = 8; i < SEQ_STEPS; i++)
        if (line[i].note) ok &= pcs_in((const int[]){line[i].note}, 1, G, 3), mel++;
    CHECK(ok && mel > 0, "the JAM Melody's chord-tone slots on steps 9-16 are G's tones (%d notes)", mel);
    ok = 1;
    for (int i = 0; i < SEQ_STEPS; i++) {
        c = seq_part_step_notes(&s, 0, LANE_CHORDS, 5, i, n);
        ok &= pcs_in(n, c, i < 8 ? C : G, 3);
    }
    CHECK(ok, "the Chords lane: C's voice on steps 1-8, G's from step 9");
    nev = 0;
    play(&s, 0.0, 5 * 4.0 + 2.1, 0);   /* from bar 1 (a start is the part's bar 1) until bar 6's step 9 fired (beat 22) */
    c = seq_part_step_notes(&s, 0, LANE_CHORDS, 5, 8, n);
    ok = c > 0;
    for (int i = 0; i < c; i++) ok &= n[i] < SEQ_KEY_LO || n[i] > SEQ_KEY_HI || seq_key_lit(&s, n[i]) == 1;
    for (int k = SEQ_KEY_LO; k <= SEQ_KEY_HI; k++)
        if (seq_key_lit(&s, k)) ok &= pcs_in((const int[]){k}, 1, G, 3);
    CHECK(ok && nev > 0, "KEYS latch G's notes after step 9 fires");
}

/* Review focus: 4 points in every bar at 1/4-bar chords; every slot-taking getter and setter on every step (no access out of
 * range under ASan/UBSan); a virtual slot reads as locked and never takes a COLOUR rank */
static void test_24_cp_every_bar_quarter(void) {
    static seq_t s;
    static const signed char STEP[SEQ_CP_MAX] = {1, 6, 10, 15};
    static seq_cp_t was[SEQ_MAX_BARS][SEQ_CP_MAX];
    char buf[64];
    int n[THEORY_CHORD_MAX], notes[SEQ_LANE_STEPS], scores[SEQ_LANE_STEPS], ok = 1, k0 = -1, cx = -1, okc = 1, set = 1;
    char roles[2][16];
    mjam_note_t line[SEQ_STEPS];
    uint64_t lock;
    nev = 0;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_set_chord_len(&s, 4);
    CHECK(seq_slots(&s) == 64, "16 bars of 1/4-bar chords: 64 slots");
    for (int b = 0; b < SEQ_MAX_BARS; b++)
        for (int k = 0; k < SEQ_CP_MAX; k++) {
            int src = (b + k) % 4, deg = (b * 3 + k) % 7, type = (b + 2 * k) % 5;
            set &= seq_cp_set(&s, b, STEP[k], deg, src, type);
        }
    CHECK(set, "64 points set (borrows and every type among them)");
    for (int x = 0; x <= 100 && cx < 0; x++)
        if (!rhythm_cx_colour_flat(x)) cx = x;
    seq_set_complexity(&s, LANE_CHORDS, cx);
    lock = s.part[0].slot_lock;
    memcpy(was, s.part[0].cp, sizeof was);
    for (int b = 0; b < SEQ_MAX_BARS; b++) {
        seq_set_view(&s, b);
        for (int i = 0; i < SEQ_STEPS; i++) {
            int v = seq_slot_at(&s, b, i);
            ok &= v >= 0 && v < SEQ_SLOTS_ALL && (i % 4 < STEP[i / 4] % 4 ? v < SEQ_MAX_PROG : v >= SEQ_MAX_PROG);   /* ruling 31: a point
                                                                                     * lasts to its 1/4-bar slot's end, then the real slot */
            seq_slot_name(&s, v, buf, sizeof buf);
            seq_slot_label(&s, v, buf, sizeof buf);
            seq_slot_numeral(&s, v, buf, sizeof buf);
            ok &= buf[0] != ' ';
            ok &= v < SEQ_MAX_PROG || seq_slot_locked(&s, v);
            seq_slot_deg(&s, v), seq_slot_src(&s, v), seq_slot_colour(&s, v), seq_part_eff_colour(&s, 0, v);
            s.naud = 0;
            seq_audition_chord(&s, v);
            seq_cell_label(&s, LANE_CHORDS, i, buf, sizeof buf);
            seq_cell_label(&s, LANE_BASS, i, buf, sizeof buf);
            for (int l = 0; l < SEQ_ACTIVE_LANES; l++) seq_part_step_notes(&s, 0, l, b, i, n), seq_part_step_note(&s, 0, l, b, i);
            seq_chord_change_at(&s, b, i);
            if (v >= SEQ_MAX_PROG) {   /* the setters ignore a virtual slot */
                seq_set_slot(&s, v, 3, 0, 0);
                seq_toggle_slot_lock(&s, v);
            }
            if (v >= SEQ_MAX_PROG && !s.part[0].cp[b][(v - SEQ_MAX_PROG) % SEQ_CP_MAX].type) {   /* a Knobs point: never a rank */
                int col = seq_part_eff_colour(&s, 0, v), deg = s.part[0].prog[v], src = s.part[0].src[v];
                for (int w = SEQ_MAX_PROG; w < SEQ_SLOTS_ALL; w++)
                    if (w != v && !s.part[0].cp[(w - SEQ_MAX_PROG) / SEQ_CP_MAX][(w - SEQ_MAX_PROG) % SEQ_CP_MAX].type &&
                        s.part[0].prog[w] == deg && s.part[0].src[w] == src)
                        okc &= seq_part_eff_colour(&s, 0, w) == col;
                if (k0 < 0) k0 = v;
            }
        }
        seq_chord_label(&s, b, buf, sizeof buf);
        ok &= strlen(buf) > 0;
        seq_bass_bar_score(&s, b);
        seq_bass_style_roles(&s, 40, 2, b, roles);
        seq_mel_line(&s, b, line);
    }
    CHECK(ok && s.part[0].slot_lock == lock && !memcmp(was, s.part[0].cp, sizeof was),
          "every getter on every step (ruling 31: each point to its slot's end); the slot setters and the picker leave the points and the locks");
    CHECK(cx >= 0 && k0 >= 0 && okc, "COMPLEXITY %d (colour layers not flat): Knobs points of one chord all play one colour (no rank)", cx);
    for (int l = 0; l < SEQ_ACTIVE_LANES; l++) lane_notes(&s, 0, l, notes);
    for (int m = 0; m < VMODE_COUNT; m++) seq_set_vmode(&s, m);
    for (int c = -1; c <= 4; c++) seq_set_vknob(&s, VK_COL, c);
    seq_set_vknob(&s, VK_INV, 2);
    seq_set_vknob(&s, VK_SPR, -1);
    seq_set_scale(&s, SCALE_MAJOR);
    seq_set_key(&s, 2);
    seq_generate_progression(&s);
    seq_jam_apply_now(&s, LANE_CHORDS, 1, 1);
    seq_jam_apply_now(&s, LANE_BASS, 4, 4);
    for (int i = 0; i < SEQ_LANE_STEPS; i++) scores[i] = 0;
    lane_notes(&s, 0, LANE_CHORDS, scores);
    nev = 0;
    play(&s, 0.0, 16 * 4.0, 0);
    ok = 1;
    for (int b = 0; b < SEQ_MAX_BARS; b++) ok &= seq_cp_count(&s, b) == SEQ_CP_MAX;
    CHECK(ok && nev > 0 && balanced(), "voicing, COLOUR, key, scale, HARMONY GEN, JAM and a 16-bar play: the points stay, notes balanced");
    for (int b = 0; b < SEQ_MAX_BARS; b++) ok &= s.part[0].prog[SEQ_MAX_PROG + b * SEQ_CP_MAX + 1] == s.part[0].cp[b][1].deg;
    CHECK(ok && s.part[0].src[SEQ_MAX_PROG + 1] == 1 && s.part[0].prog[SEQ_MAX_PROG + 1] == theory_borrow_degree(SCALE_MAJOR, 1),
          "after the scale change a borrowed point plays its borrow's degree in the new scale");
}

/* Review focus: chord length 1 bar -> 1/4 bar -> 2 bars -> 1 bar, and a 16-bar loop shrunk to 4 then grown back */
static void test_24_cp_chord_len_and_loop(void) {
    static seq_t s;
    static seq_cp_t was[SEQ_MAX_BARS][SEQ_CP_MAX];
    seq_cp_t c;
    nev = 0;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 16);
    seq_cp_set(&s, 1, 8, 2, 0, 0);
    seq_cp_set(&s, 3, 6, 4, 2, 3);
    seq_cp_set(&s, 12, 4, 1, 0, 2);
    memcpy(was, s.part[0].cp, sizeof was);
    seq_set_chord_len(&s, 4);
    CHECK(!memcmp(was, s.part[0].cp, sizeof was) && seq_slot_at(&s, 1, 8) == SEQ_MAX_PROG + SEQ_CP_MAX && seq_slot_at(&s, 1, 7) == 5,
          "1 bar -> 1/4 bar: every point kept at its bar and step");
    seq_set_chord_len(&s, 32);
    CHECK(!memcmp(was, s.part[0].cp, sizeof was) && seq_slot_at(&s, 1, 0) == 0, "-> 2 bars: kept; bar 2 is inside slot 1's chord");
    CHECK(seq_cp_set(&s, 1, 0, 6, 0, 0) && !seq_cp_set(&s, 2, 0, 6, 0, 0) && seq_slot_at(&s, 1, 0) == SEQ_MAX_PROG + SEQ_CP_MAX,
          "2-bar chords: step 0 of bar 2 holds a point, of bar 3 (a slot's start) none");
    seq_set_chord_len(&s, 16);
    CHECK(seq_cp_count(&s, 1) == 1 && seq_cp_get(&s, 1, 0, &c) && c.step == 8 && !memcmp(was, s.part[0].cp, sizeof was),
          "-> 1 bar: the step-0 point is dropped (a slot change now), the others kept");
    seq_undo(&s);
    CHECK(seq_chord_len(&s) == 32 && seq_cp_count(&s, 1) == 2 && seq_cp_get(&s, 1, 0, &c) && c.step == 0 && c.deg == 6 &&
              seq_slot_at(&s, 1, 0) == SEQ_MAX_PROG + SEQ_CP_MAX,
          "UNDO: 2-bar chords and the step-0 point again (the progression snapshot carries the points)");
    seq_set_chord_len(&s, 16);
    seq_set_bars(&s, 4);
    CHECK(seq_cp_count(&s, 12) == 1 && seq_cp_count(&s, 3) == 1 && seq_slot_at(&s, 3, 6) == SEQ_MAX_PROG + 3 * SEQ_CP_MAX &&
              !seq_chord_change_at(&s, 12, 4),
          "16 -> 4 bars: the hidden bar 13 keeps its point; bar 4's plays");
    seq_set_bars(&s, 16);
    CHECK(!memcmp(was, s.part[0].cp, sizeof was) && seq_slot_at(&s, 12, 4) == SEQ_MAX_PROG + 12 * SEQ_CP_MAX,
          "-> 16 bars: bar 13's point plays again");
    seq_init(&s, emit, NULL);
    seq_cp_set(&s, 1, 8, 2, 0, 0);
    seq_set_bars(&s, 8);
    CHECK(seq_cp_count(&s, 5) == 1 && seq_cp_get(&s, 5, 0, &c) && c.step == 8 && c.deg == 2 && seq_slot_at(&s, 5, 8) == SEQ_MAX_PROG + 5 * SEQ_CP_MAX,
          "4 -> 8 bars: never-filled bars repeat the loop's points (as its steps and slots)");
}

/* PROGRESSION picks and tries, a JAM tap and HARMONY GEN leave the points */
static void test_24_cp_kept(void) {
    static seq_t s;
    static seq_cp_t was[SEQ_MAX_BARS][SEQ_CP_MAX];
    cp_setup(&s);
    seq_cp_set(&s, 5, 8, 2, 0, 0);
    seq_cp_set(&s, 2, 4, 1, 1, 3);
    memcpy(was, s.part[0].cp, sizeof was);
    seq_browse_open(&s, CVIEW_PROG);
    seq_prog_try(&s, 3);
    seq_prog_try(&s, 5);
    seq_browse_done(&s);
    CHECK(!memcmp(was, s.part[0].cp, sizeof was) && s.part[0].prog[SEQ_MAX_PROG + 5 * SEQ_CP_MAX] == 2, "a PROGRESSION pick keeps them");
    seq_prog_step(&s, 1);
    seq_jam_apply_now(&s, LANE_CHORDS, 1, 1);
    CHECK(!memcmp(was, s.part[0].cp, sizeof was), "PROGRESSION's arrows and a JAM tap keep them");
    seq_generate_progression(&s);
    CHECK(!memcmp(was, s.part[0].cp, sizeof was) && seq_slot_at(&s, 5, 8) == SEQ_MAX_PROG + 5 * SEQ_CP_MAX, "HARMONY GEN keeps them");
}

/* `.cp=`: 2 points in each of 3 bars of 2 parts survive a save and load; a state without `.cp=` loads none; a bad entry is skipped */
static void test_24_cp_save_round_trip(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], cut[SEQ_STATE_MAX];
    char *a, *e;
    int ok = 1;
    cp_setup(&s);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, ".cp="), "no point: no `.cp=` (a 0.23 state byte for byte)");
    for (int part = 0; part < 2; part++) {
        seq_select_part(&s, part ? 3 : 0);
        for (int b = 0; b < 3; b++) {
            ok &= seq_cp_set(&s, b * 2 + part, 4 + b, (b + part) % 7, part ? b % 4 : 0, b % 5);
            ok &= seq_cp_set(&s, b * 2 + part, 12, (b + 3) % 7, 0, 4 - b);
        }
    }
    seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    CHECK(ok && strstr(buf, "|A.cp=") && strstr(buf, "|D.cp=") && seq_state_load(&t, buf), "saved as A.cp and D.cp; it loads");
    ok = !memcmp(s.part[0].cp, t.part[0].cp, sizeof s.part[0].cp) && !memcmp(s.part[3].cp, t.part[3].cp, sizeof s.part[3].cp);
    ok &= !memcmp(s.part[3].prog, t.part[3].prog, sizeof s.part[3].prog) && !memcmp(s.part[3].voice, t.part[3].voice, sizeof s.part[3].voice);
    CHECK(ok && seq_cp_count(&t, 4) == 2, "the same points, virtual slots and voices after the load");
    strcpy(cut, buf);
    while ((a = strstr(cut, ".cp=")) != NULL) {
        a -= 2;   /* the '|' and the part letter */
        e = strchr(a + 1, '|');
        memmove(a, e ? e : a + strlen(a), strlen(e ? e : a + strlen(a)) + 1);
    }
    CHECK(seq_state_load(&t, cut), "the state without `.cp=` loads");
    ok = 1;
    for (int part = 0; part < SEQ_NPARTS; part++)
        for (int b = 0; b < SEQ_MAX_BARS; b++)
            for (int k = 0; k < SEQ_CP_MAX; k++) ok &= t.part[part].cp[b][k].step == -1;
    CHECK(ok && seq_slot_at(&t, 0, 4) == 0, "no point in any part");
    seq_init(&s, emit, NULL);
    seq_state_save(&s, buf, sizeof buf);
    strcat(buf, "|A.cp=18200,2x200,29900,3a2,3c613,1c2");
    CHECK(seq_state_load(&t, buf) && seq_cp_count(&t, 1) == 1 && seq_cp_count(&t, 2) == 0 && seq_cp_count(&t, 3) == 1 &&
              t.part[0].cp[3][0].step == 12 && t.part[0].cp[3][0].src == 1 && t.part[0].cp[3][0].type == 3,
          "bad entries are skipped, the good ones load");
}

/* 0.24 Task 2: PHRASE's bar editor. Bar b of drawn lane `lane` drawn as the string (A x g - .), edited, no undo step (a test's setup) */
static void ph_draw(seq_t *s, int lane, int b, const char *codes) {
    rbar_t *r = &s->part[s->cur_part].rb[lane == LANE_BASS ? DL_BASS : DL_CHORDS][b];
    rhythm_parse(codes, r->c);
    r->ed = 1;
    render_now(s);
}

static int ph_reads(const seq_t *s, int lane, int bar, const char *want) {
    unsigned char c[16];
    char str[17];
    seq_bar_codes(s, lane, bar, c);
    rhythm_format(c, str);
    return !strcmp(str, want);
}

/* the chord (degree) sounding at a step of a bar */
static int ph_deg(const seq_t *s, int bar, int step) { return seq_slot_deg(s, seq_slot_at(s, bar, step)); }

/* everything an open PHRASE bar's CANCEL puts back */
typedef struct {
    int prog[SEQ_MAX_PROG];
    unsigned char src[SEQ_MAX_PROG], colour[SEQ_MAX_PROG];
    uint64_t lock;
    seq_cp_t cp[SEQ_MAX_BARS][SEQ_CP_MAX];
    signed char hoct[SEQ_MAX_BARS][16];
    rbar_t rb[DL_COUNT][SEQ_MAX_BARS];
    unsigned char fix[SEQ_LANE_STEPS], bv[2][SEQ_MAX_BARS];
    unsigned short nlock;
    bass_set_t frozen[SEQ_MAX_BARS];
} ph_state_t;

static void ph_get(const seq_t *s, ph_state_t *x) {
    const seq_part_t *p = &s->part[s->cur_part];
    memset(x, 0, sizeof *x);
    memcpy(x->prog, p->prog, sizeof x->prog);
    memcpy(x->src, p->src, sizeof x->src);
    memcpy(x->colour, p->colour, sizeof x->colour);
    x->lock = p->slot_lock;
    memcpy(x->cp, p->cp, sizeof x->cp);
    memcpy(x->hoct, p->hoct, sizeof x->hoct);
    memcpy(x->rb, p->rb, sizeof x->rb);
    memcpy(x->fix, p->bass.fix, sizeof x->fix);
    memcpy(x->bv[0], p->lane[LANE_CHORDS].bar_var, SEQ_MAX_BARS);
    memcpy(x->bv[1], p->lane[LANE_BASS].bar_var, SEQ_MAX_BARS);
    x->nlock = p->bass.nlock;
    for (int b = 0; b < SEQ_MAX_BARS; b++)
        if (p->bass.nlock >> b & 1) x->frozen[b] = p->bass.frozen[b];
}

static int ph_same(const seq_t *s, const ph_state_t *want) {
    ph_state_t now;
    ph_get(s, &now);
    return !memcmp(&now, want, sizeof now);
}

static void test_24_phrase_select_paint(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    int n, r;
    cp_setup(&s);   /* Sustain: x--------------- */
    CHECK(seq_rbar_sel(&s, LANE_CHORDS) == -1, "closed: no step selected");
    ph_draw(&s, LANE_CHORDS, 5, "....x-----------");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    CHECK(seq_rbar_sel(&s, LANE_CHORDS) == 4, "the editor opens on the bar's first hit (step 5)");
    seq_rbar_cancel(&s, LANE_CHORDS);
    ph_draw(&s, LANE_CHORDS, 5, "x---------------");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 9);
    CHECK(seq_rbar_sel(&s, LANE_CHORDS) == 9, "any step selects (a hold)");
    seq_rbar_select(&s, LANE_CHORDS, 16);
    seq_rbar_select(&s, LANE_BASS, 3);
    CHECK(seq_rbar_sel(&s, LANE_CHORDS) == 9 && seq_rbar_sel(&s, LANE_BASS) == -1, "a bad step or another lane: nothing");
    n = s.undo_n;
    r = seq_rbar_paint(&s, LANE_CHORDS, 0, RC_HIT);
    CHECK(!r && seq_rbar_sel(&s, LANE_CHORDS) == 0 && s.undo_n == n && ph_reads(&s, LANE_CHORDS, 5, "x---------------"),
          "the paint a step already has only selects it: no change, no undo step");
    r = seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    CHECK(r == 1 && seq_rbar_sel(&s, LANE_CHORDS) == 8 && s.undo_n == n + 1 && ph_reads(&s, LANE_CHORDS, 5, "x-------x-------"),
          "HIT on a hold: painted and selected, one undo step");
    r = seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    CHECK(!r && s.undo_n == n + 1 && ph_reads(&s, LANE_CHORDS, 5, "x-------x-------"), "a second tap keeps the hit (0.18 cleared it)");
    r = seq_rbar_paint(&s, LANE_CHORDS, 3, RC_REST);
    CHECK(r == 1 && seq_rbar_sel(&s, LANE_CHORDS) == 3 && ph_reads(&s, LANE_CHORDS, 5, "x--.....x-------"), "REST clears (a hold: the holds after it too)");
    r = seq_rbar_paint(&s, LANE_CHORDS, 3, RC_REST);
    CHECK(!r && s.undo_n == n + 2, "REST on a rest: only selected");
    r = seq_rbar_paint(&s, LANE_CHORDS, 5, RC_HOLD);
    CHECK(!r && seq_rbar_sel(&s, LANE_CHORDS) == 3 && ph_reads(&s, LANE_CHORDS, 5, "x--.....x-------") && s.undo_n == n + 2,
          "HOLD with a rest before it does nothing, not even select");
    r = seq_rbar_paint(&s, LANE_CHORDS, 3, RC_HOLD);
    CHECK(r == 1 && ph_reads(&s, LANE_CHORDS, 5, "x---....x-------"), "HOLD after a hold: painted");
    seq_rbar_select(&s, LANE_CHORDS, 8);
    seq_rbar_chord(&s, 2, 0);   /* G from step 9 */
    seq_rbar_hit_oct(&s, 1);
    CHECK(seq_cp_count(&s, 5) == 1 && p->hoct[5][8] == 1, "a change point and a hit octave on step 9");
    r = seq_rbar_paint(&s, LANE_CHORDS, 8, RC_GHOST);
    CHECK(r == 1 && p->hoct[5][8] == 1 && ph_reads(&s, LANE_CHORDS, 5, "x---....g-------"), "another hit on the hit: its octave and holds stay");
    r = seq_rbar_paint(&s, LANE_CHORDS, 8, RC_REST);
    CHECK(r == 1 && p->hoct[5][8] == 0 && seq_cp_count(&s, 5) == 1 && ph_deg(&s, 5, 8) == 2 && ph_reads(&s, LANE_CHORDS, 5, "x---............"),
          "REST on the hit: its holds and octave clear, its change point stays (the step's)");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths: A.x.A.x.A.x.A.x. */
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_select(&s, LANE_BASS, 4);
    seq_rbar_note(&s, 4);
    r = seq_rbar_paint(&s, LANE_BASS, 4, RC_REST);
    CHECK(r == 1 && p->bass.fix[2 * 16 + 4] != 0 && seq_bass_fixed_count(&s, 2) == 1, "the Bass: clearing a fixed hit keeps its fix (silent)");
    r = seq_rbar_paint(&s, LANE_BASS, 4, RC_HIT);
    CHECK(r == 1 && s.part[0].lane[LANE_BASS].steps[2 * 16 + 4].abs == p->bass.fix[2 * 16 + 4], "a hit there again plays it");
    seq_rbar_done(&s, LANE_BASS);
}

static void test_24_phrase_chord_rules(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    int n;
    cp_setup(&s);   /* bar 6 (index 5): C (VI, degree 5) */
    ph_draw(&s, LANE_CHORDS, 5, "....x---x-------");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    n = s.undo_n;
    s.naud = 0;
    seq_rbar_select(&s, LANE_CHORDS, 2);
    seq_rbar_chord(&s, 2, 0);   /* G */
    CHECK(p->prog[5] == 2 && (p->slot_lock >> 5 & 1) && seq_cp_count(&s, 5) == 0 && s.undo_n == n + 1 && s.naud > 0,
          "a rest before the slot's first hit: the slot's chord (G), picked, one undo step, heard");
    seq_rbar_select(&s, LANE_CHORDS, 4);
    seq_rbar_chord(&s, 3, 0);   /* Am */
    CHECK(p->prog[5] == 3 && seq_cp_count(&s, 5) == 0 && s.undo_n == n + 2, "the slot's first hit: the slot again (Am)");
    seq_rbar_chord(&s, 3, 0);
    CHECK(s.undo_n == n + 2, "the same pick again: no undo step");
    seq_rbar_select(&s, LANE_CHORDS, 8);
    s.naud = 0;
    seq_rbar_chord(&s, 6, 0);   /* D from step 9 */
    CHECK(p->prog[5] == 3 && seq_cp_count(&s, 5) == 1 && p->cp[5][0].step == 8 && p->cp[5][0].deg == 6 && ph_deg(&s, 5, 8) == 6 &&
              ph_deg(&s, 5, 7) == 3 && s.undo_n == n + 3 && s.naud > 0,
          "a later hit: a change point (D from step 9), the slot kept, one undo step, heard");
    seq_rbar_chord(&s, 6, 0);
    CHECK(seq_cp_count(&s, 5) == 1 && s.undo_n == n + 3, "the point's own chord again: nothing");
    seq_rbar_chord(&s, 0, 0);
    CHECK(seq_cp_count(&s, 5) == 1 && p->cp[5][0].deg == 0 && s.undo_n == n + 4, "another chord: the point's chord replaced (Em)");
    seq_rbar_chord(&s, 3, 0);
    CHECK(seq_cp_count(&s, 5) == 0 && ph_deg(&s, 5, 8) == 3 && s.undo_n == n + 5, "picking the chord that sounds there (Am) removes the point");
    seq_rbar_chord(&s, 3, 0);
    CHECK(seq_cp_count(&s, 5) == 0 && s.undo_n == n + 5, "the sounding chord on a step with no point: nothing");
    seq_rbar_select(&s, LANE_CHORDS, 13);
    seq_rbar_paint(&s, LANE_CHORDS, 12, RC_REST);
    seq_rbar_select(&s, LANE_CHORDS, 13);
    seq_rbar_chord(&s, 1, 1);   /* a borrow on a rest */
    CHECK(seq_cp_count(&s, 5) == 1 && p->cp[5][0].step == 13 && p->cp[5][0].src == 1 && ph_reads(&s, LANE_CHORDS, 5, "....x---x---...."),
          "a pick on a rest works: a point there (a borrowed chord)");
    seq_rbar_select(&s, LANE_CHORDS, 8);
    seq_rbar_type(&s, 2);   /* sus4 on Am, no point there */
    CHECK(seq_cp_count(&s, 5) == 2 && p->cp[5][0].step == 8 && p->cp[5][0].deg == 3 && p->cp[5][0].type == 2 && p->colour[5] != COL_SUS4,
          "TYPE on a later hit with no change of its own makes one (Am sus4); the slot keeps its type");
    seq_rbar_type(&s, 3);
    CHECK(seq_cp_count(&s, 5) == 2 && p->cp[5][0].type == 3, "TYPE on its own point changes the point's type");
    seq_rbar_select(&s, LANE_CHORDS, 4);
    seq_rbar_type(&s, 1);
    CHECK(p->colour[5] == COL_SUS2 && (p->slot_lock >> 5 & 1) && seq_cp_count(&s, 5) == 2, "TYPE on the slot's first hit types the slot (sus2)");
    seq_rbar_select(&s, LANE_CHORDS, 2);
    seq_rbar_chord(&s, 7, 0);
    seq_rbar_chord(&s, 2, 4);
    seq_rbar_type(&s, 5);
    CHECK(p->prog[5] == 3 && p->colour[5] == COL_SUS2, "bad chords and types do nothing");
    seq_rbar_select(&s, LANE_CHORDS, 6);
    seq_rbar_chord(&s, 4, 0);
    seq_rbar_select(&s, LANE_CHORDS, 10);
    seq_rbar_chord(&s, 5, 0);
    n = s.undo_n;
    seq_rbar_select(&s, LANE_CHORDS, 11);
    seq_rbar_chord(&s, 6, 0);
    CHECK(seq_cp_count(&s, 5) == 4 && s.undo_n == n, "a full bar (4 points) takes no fifth");
    seq_rbar_done(&s, LANE_CHORDS);
    cp_setup(&s);
    seq_set_chord_len(&s, 32);   /* 2-bar chords: bar 6 is slot 2's second bar */
    seq_rbar_open(&s, LANE_CHORDS, 5);
    n = p->prog[2];
    seq_rbar_select(&s, LANE_CHORDS, 0);
    seq_rbar_chord(&s, (n + 1) % 7, 0);
    CHECK(p->prog[2] == n && seq_cp_count(&s, 5) == 1 && p->cp[5][0].step == 0 && ph_deg(&s, 5, 0) == (n + 1) % 7 && ph_deg(&s, 4, 0) == n,
          "a 2-bar slot in its second bar: a change point, even on step 1");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rbar_open(&s, LANE_CHORDS, 4);
    seq_rbar_select(&s, LANE_CHORDS, 0);
    seq_rbar_chord(&s, (n + 2) % 7, 0);
    CHECK(p->prog[2] == (n + 2) % 7 && (p->slot_lock >> 2 & 1) && seq_cp_count(&s, 4) == 0, "in its first bar: the slot");
    seq_rbar_done(&s, LANE_CHORDS);
    cp_setup(&s);   /* ruling 12 */
    ph_draw(&s, LANE_CHORDS, 5, "x-------x-------");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_type(&s, 2);   /* the slot: C sus4 */
    seq_rbar_select(&s, LANE_CHORDS, 8);
    seq_rbar_chord(&s, 6, 0);
    CHECK(p->colour[5] == COL_SUS4 && seq_cp_count(&s, 5) == 1 && p->cp[5][0].type == 0,
          "ruling 12: a new point is type 0 (as knobs), not the sus4 of the chord it replaces");
    seq_rbar_type(&s, 3);
    seq_rbar_chord(&s, 4, 0);
    CHECK(p->cp[5][0].deg == 4 && p->cp[5][0].type == 3, "a point given another chord keeps the type picked for it (add9)");
    seq_rbar_done(&s, LANE_CHORDS);
}

static void test_24_phrase_hit_oct(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    seq_part_t *p = &s.part[0];
    int a[THEORY_CHORD_MAX], b[THEORY_CHORD_MAX], a0[THEORY_CHORD_MAX], b0[THEORY_CHORD_MAX], na, nb, n, ok = 1;
    cp_setup(&s);
    ph_draw(&s, LANE_CHORDS, 5, "x-x-x-x-x-x-x-x-");
    na = seq_part_step_notes(&s, 0, LANE_CHORDS, 5, 2, a0);
    nb = seq_part_step_notes(&s, 0, LANE_CHORDS, 5, 4, b0);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 2);
    n = s.undo_n;
    seq_rbar_hit_oct(&s, 1);
    CHECK(p->hoct[5][2] == 1 && seq_hit_oct(&s, 5, 2) == 1 && s.undo_n == n + 1, "+1 on step 3: stored, one undo step");
    CHECK(seq_part_step_notes(&s, 0, LANE_CHORDS, 5, 2, a) == na && seq_part_step_notes(&s, 0, LANE_CHORDS, 5, 4, b) == nb, "the same chords");
    for (int i = 0; i < na; i++) ok &= a[i] == a0[i] + 12;
    for (int i = 0; i < nb; i++) ok &= b[i] == b0[i];
    CHECK(ok, "only step 3's hit plays 12 up");
    seq_rbar_hit_oct(&s, 1);
    seq_rbar_hit_oct(&s, 3);
    seq_rbar_hit_oct(&s, -3);
    seq_rbar_select(&s, LANE_CHORDS, 3);
    seq_rbar_hit_oct(&s, 2);
    CHECK(s.undo_n == n + 1 && p->hoct[5][3] == 0 && p->hoct[5][2] == 1, "the same octave, a bad one or a hold: nothing");
    seq_rbar_select(&s, LANE_CHORDS, 12);
    seq_rbar_hit_oct(&s, -2);
    seq_rbar_done(&s, LANE_CHORDS);
    seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    CHECK(strstr(buf, "|A.ho=") && seq_state_load(&t, buf) && !memcmp(t.part[0].hoct, p->hoct, sizeof p->hoct) &&
              seq_part_step_notes(&t, 0, LANE_CHORDS, 5, 2, a) == na && a[0] == a0[0] + 12,
          "saved as `.ho=`; it loads and plays the same");
    cp_setup(&t);
    seq_state_save(&t, buf, sizeof buf);
    CHECK(!strstr(buf, ".ho="), "no hit octave: no `.ho=`");
    strcat(buf, "|A.ho=5x,x0,3,6?A,4A2,9g");   /* 0.24 ruling 13 sparse: bar 6 step 2 +2; bad bar, no octave, a bad character, a bar past the loop */
    CHECK(seq_state_load(&t, buf) && t.part[0].hoct[5][2] == 0 && t.part[0].hoct[4][0] == -2,
          "bad entries are skipped, the good ones load");
    CHECK(t.part[0].hoct[5][1] == 0 && t.part[0].hoct[4][6] == 0,
          "final review (ruling 32): a loaded octave on a step that is no drawn hit (Sustain's holds: bar 6 step 2, bar 5 step 7) is dropped");
}

static void test_24_this_hit_only(void) {
    static seq_t s;
    int n, r;
    cp_setup(&s);   /* bar 5 Em, bar 6 C */
    ph_draw(&s, LANE_CHORDS, 5, "x---x---x---x-..");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 4);
    r = seq_rbar_this_hit_only(&s);
    CHECK(r == RHO_NOT_A_CHANGE && seq_cp_count(&s, 5) == 0, "a hit that starts no change: refused");
    seq_rbar_select(&s, LANE_CHORDS, 14);
    r = seq_rbar_this_hit_only(&s);
    CHECK(r == RHO_NOT_A_CHANGE, "a rest: refused");
    seq_rbar_select(&s, LANE_CHORDS, 4);
    seq_rbar_chord(&s, 2, 0);   /* G from step 5 */
    n = s.undo_n;
    r = seq_rbar_this_hit_only(&s);
    CHECK(r == 1 && seq_cp_count(&s, 5) == 2 && ph_deg(&s, 5, 4) == 2 && ph_deg(&s, 5, 8) == 5 && ph_deg(&s, 5, 12) == 5 &&
              s.undo_n == n + 1, "on G's hit: C again from the next hit (step 9), one undo step");
    r = seq_rbar_this_hit_only(&s);
    CHECK(r == RHO_LATER_HAS_OWN && s.undo_n == n + 1, "again: the next hit has a change of its own");
    seq_rbar_select(&s, LANE_CHORDS, 12);
    seq_rbar_chord(&s, 6, 0);   /* D from step 13 */
    r = seq_rbar_this_hit_only(&s);
    CHECK(r == RHO_NO_LATER_HIT && seq_cp_count(&s, 5) == 3, "the bar's last hit: no later hit");
    seq_rbar_select(&s, LANE_CHORDS, 0);
    r = seq_rbar_this_hit_only(&s);
    CHECK(r == RHO_LATER_HAS_OWN, "the slot's start (C after Em) is a change; its next hit has its own");
    seq_rbar_done(&s, LANE_CHORDS);
    r = seq_rbar_this_hit_only(&s);
    CHECK(r == RHO_NOT_A_CHANGE, "closed: refused");
}

/* 0.24 final review (ruling 31): a chord degree other than the ones given, -1 none */
static int deg_not(int a, int b, int c) {
    for (int d = 0; d < 7; d++)
        if (d != a && d != b && d != c) return d;
    return -1;
}

/* the chord words (NAMES) of slots v and w and the label they make with the bar's first slot u: u.v@a.w@b (as seq_chord_label) */
static void cp_want(const seq_t *s, int u, int v, int a, int w, int b, char *buf, int len) {
    char x[3][16];
    seq_slot_label(s, u, x[0], sizeof x[0]), seq_slot_label(s, v, x[1], sizeof x[1]), seq_slot_label(s, w, x[2], sizeof x[2]);
    snprintf(buf, (size_t)len, "%s\xC2\xB7%s@%d\xC2\xB7%s@%d", x[0], x[1], a, x[2], b);
}

/* 0.24 final review (ruling 31): at 1/2- and 1/4-bar chords a change point lasts to the next real slot's start; a pick on that slot's
 * first step changes the slot and is heard there; the label shows both changes */
static void test_24_cp_slot_boundary(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    char buf[64], want[64];
    int m[THEORY_CHORD_MAX], c, x, y, r, ok = 1, oka = 1, n;
    cp_setup(&s);
    seq_set_names(&s, 1);
    seq_set_chord_len(&s, 8);   /* 1/2-bar chords: bar 6 is slots 10 (steps 1-8) and 11 (steps 9-16) */
    ph_draw(&s, LANE_CHORDS, 5, "x---x---x---x---");
    x = deg_not(p->prog[10], p->prog[11], -1);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 4);
    r = seq_rbar_chord(&s, x, 0);
    for (int i = 0; i < SEQ_STEPS; i++) ok &= ph_deg(&s, 5, i) == (i < 4 ? p->prog[10] : i < 8 ? x : p->prog[11]);
    CHECK(r == RBC_POINT && ok && seq_slot_at(&s, 5, 8) == 11 && seq_slot_at(&s, 5, 15) == 11,
          "1/2-bar chords: a point on step 5 lasts to slot 11's start (step 9); steps 9-16 play slot 11");
    r = seq_rbar_this_hit_only(&s);
    CHECK(r == RHO_NO_LATER_HIT && seq_cp_count(&s, 5) == 1, "THIS HIT ONLY there: the next hit (step 9) is the next slot's: no later hit");
    y = deg_not(x, p->prog[10], p->prog[11]);
    s.naud = 0;
    seq_rbar_select(&s, LANE_CHORDS, 8);
    n = s.undo_n;
    r = seq_rbar_chord(&s, y, 0);
    ok = 1;
    for (int i = 0; i < SEQ_STEPS; i++) ok &= ph_deg(&s, 5, i) == (i < 4 ? p->prog[10] : i < 8 ? x : y);
    c = seq_slot_notes(&s, 11, -1, m);
    oka = s.naud == c && c > 0;
    for (int i = 0; oka && i < c; i++) oka &= s.aud[i].note == m[i];
    CHECK(r == RBC_SLOT && p->prog[11] == y && (p->slot_lock >> 11 & 1) && ok && seq_cp_count(&s, 5) == 1 && s.undo_n == n + 1 &&
              seq_chord_change_at(&s, 5, 8),
          "a pick on step 9: slot 11's chord, picked, one undo step; steps 9-16 play it, the point still steps 5-8");
    CHECK(oka, "the pick is heard as slot 11's new chord (%d notes)", c);
    cp_want(&s, 10, SEQ_MAX_PROG + 5 * SEQ_CP_MAX, 5, 11, 9, want, sizeof want);
    seq_chord_label(&s, 5, buf, sizeof buf);
    CHECK(!strcmp(buf, want), "the label shows both changes: %s (%s)", buf, want);
    seq_rbar_done(&s, LANE_CHORDS);
    cp_setup(&s);
    seq_set_names(&s, 1);
    seq_set_chord_len(&s, 4);   /* 1/4-bar chords: bar 6 is slots 20-23 */
    ph_draw(&s, LANE_CHORDS, 5, "x-x-x-x-x-x-x-x-");
    x = deg_not(p->prog[20], p->prog[21], -1);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 2);
    r = seq_rbar_chord(&s, x, 0);
    ok = 1;
    for (int i = 0; i < 8; i++) ok &= ph_deg(&s, 5, i) == (i < 2 ? p->prog[20] : i < 4 ? x : p->prog[21]);
    CHECK(r == RBC_POINT && ok && seq_slot_at(&s, 5, 4) == 21, "1/4-bar chords: a point on step 3 lasts to slot 21's start (step 5)");
    y = deg_not(x, p->prog[20], p->prog[21]);
    seq_rbar_select(&s, LANE_CHORDS, 4);
    s.naud = 0;
    r = seq_rbar_chord(&s, y, 0);
    c = seq_slot_notes(&s, 21, -1, m);
    oka = s.naud == c && c > 0;
    for (int i = 0; oka && i < c; i++) oka &= s.aud[i].note == m[i];
    CHECK(r == RBC_SLOT && p->prog[21] == y && ph_deg(&s, 5, 4) == y && ph_deg(&s, 5, 7) == y && ph_deg(&s, 5, 3) == x && oka,
          "a pick on step 5: slot 21's chord, heard, steps 5-8 play it");
    cp_want(&s, 20, SEQ_MAX_PROG + 5 * SEQ_CP_MAX, 3, 21, 5, want, sizeof want);
    seq_chord_label(&s, 5, buf, sizeof buf);
    CHECK(!strncmp(buf, want, strlen(want)), "the label starts with both changes: %s (%s)", buf, want);
    seq_rbar_done(&s, LANE_CHORDS);
}

/* 0.24 final review: a point that plays what its step would play without it goes, in the undo step that made it so: a slot pick under it
 * (seq_set_slot) or a change to the point before it (the PHRASE picker) */
static void test_24_cp_prune(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    int x, y, n, r;
    cp_setup(&s);
    ph_draw(&s, LANE_CHORDS, 5, "x---x---x---x---");
    x = deg_not(p->prog[5], -1, -1);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 8);
    seq_rbar_chord(&s, x, 0);   /* x from step 9 */
    seq_rbar_select(&s, LANE_CHORDS, 0);
    n = s.undo_n;
    r = seq_rbar_chord(&s, x, 0);   /* the slot becomes x: the point plays the chord before it */
    CHECK(r == RBC_SLOT && p->prog[5] == x && seq_cp_count(&s, 5) == 0 && s.undo_n == n + 1 && !seq_chord_change_at(&s, 5, 8),
          "a slot pick that makes a later point its own chord removes it, in one undo step (no X\xC2\xB7X@9)");
    seq_undo(&s);
    CHECK(p->prog[5] != x && seq_cp_count(&s, 5) == 1 && p->cp[5][0].step == 8 && p->cp[5][0].deg == x, "UNDO: the slot and the point back");
    y = deg_not(p->prog[5], x, -1);
    seq_rbar_select(&s, LANE_CHORDS, 4);
    seq_rbar_chord(&s, y, 0);   /* y from step 5, x from step 9 */
    n = s.undo_n;
    seq_rbar_chord(&s, x, 0);   /* step 5's point becomes x: step 9's plays the chord before it */
    CHECK(seq_cp_count(&s, 5) == 1 && p->cp[5][0].step == 4 && p->cp[5][0].deg == x && s.undo_n == n + 1 && ph_deg(&s, 5, 12) == x,
          "a point changed to the chord of the point after it: that one goes, in the same undo step");
    seq_rbar_done(&s, LANE_CHORDS);
}

/* a library rhythm ri and GHOSTS g under which bar 6 of 8ths loses the hit of an even step (*step) that GHOSTS then fills; 1 found */
static int hoct_case(int *ri, int *g, int *step) {
    static seq_t t;
    for (int r = 0; r < RHYTHM_COUNT; r++)
        for (int k = 1; k <= 4; k++) {
            unsigned char c[16];
            cp_setup(&t);
            ph_draw(&t, LANE_CHORDS, 5, "x-x-x-x-x-x-x-x-");
            seq_set_rknob(&t, LANE_CHORDS, RK_GHOSTS, k);
            seq_rbar_open(&t, LANE_CHORDS, 5);
            seq_rhythm_pick(&t, LANE_CHORDS, r);
            seq_bar_codes(&t, LANE_CHORDS, 5, c);
            for (int i = 2; i < SEQ_STEPS; i += 2)
                if (!rhythm_is_hit(c[i]) && t.part[0].lane[LANE_CHORDS].steps[5 * SEQ_STEPS + i].on) {
                    *ri = r, *g = k, *step = i;
                    return 1;
                }
        }
    return 0;
}

/* 0.24 final review (ruling 32): a hit octave belongs to the drawn hit: a hit the knobs add plays at the part's octave, and an octave goes
 * when its step's drawn code stops being a hit (a RHYTHM pick or try, a JAM block), so it never comes back; UNDO and CANCEL bring it back */
static void test_24_hoct_drawn_only(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    const seq_step_t *x;
    int ri = -1, g = 0, st = 0, n, jc = -1, jl = 0;
    double ppq;
    CHECK(hoct_case(&ri, &g, &st), "a rhythm (%d) takes bar 6's hit on step %d away and GHOSTS %d adds one there", ri, st + 1, g);
    if (ri < 0) return;
    cp_setup(&s);
    ph_draw(&s, LANE_CHORDS, 5, "x-x-x-x-x-x-x-x-");
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, g);
    x = &p->lane[LANE_CHORDS].steps[5 * SEQ_STEPS + st];
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, st);
    seq_rbar_hit_oct(&s, 1);
    CHECK(p->hoct[5][st] == 1 && x->on && x->oct == 1, "the hit on step %d plays an octave up", st + 1);
    n = s.undo_n;
    seq_rhythm_pick(&s, LANE_CHORDS, ri);
    CHECK(s.undo_n == n + 1 && p->hoct[5][st] == 0 && x->on && x->oct == 0,
          "a RHYTHM pick takes the hit away and its octave: the ghost GHOSTS adds there plays at the part's octave");
    seq_rbar_paint(&s, LANE_CHORDS, st, RC_HIT);
    CHECK(p->hoct[5][st] == 0 && x->on && x->oct == 0, "a later paint brings the hit back without the octave");
    seq_undo(&s);
    seq_undo(&s);
    CHECK(s.txn.kind == CVIEW_BAR && p->hoct[5][st] == 1 && x->on && x->oct == 1 && ph_reads(&s, LANE_CHORDS, 5, "x-x-x-x-x-x-x-x-"),
          "UNDO of the paint, then of the pick: the hit and its octave back");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rbrowse_open(&s, LANE_CHORDS);
    seq_rbrowse_try(&s, LANE_CHORDS, ri);
    CHECK(p->hoct[5][st] == 0 && x->oct == 0, "a RHYTHMS try that takes the hit away takes its octave");
    seq_browse_cancel(&s);
    CHECK(p->hoct[5][st] == 1 && x->on && x->oct == 1, "its CANCEL puts the octave back");
    for (int c = 0; c < JAM_COLS && jc < 0; c++)   /* a JAM block that rests on the step */
        for (int l = 0; l < JAM_LEVELS && jc < 0; l++) {
            seq_jam_apply_now(&s, LANE_CHORDS, c, l);
            if (!rhythm_is_hit(p->rb[DL_CHORDS][5].c[st])) jc = c, jl = l;
            if (jc >= 0) CHECK(p->hoct[5][st] == 0 && (!x->on || x->oct == 0), "a JAM block (%d, %d) with no hit on the step takes its octave", c, l);
            seq_undo(&s);
            if (jc >= 0) CHECK(p->hoct[5][st] == 1 && x->on && x->oct == 1, "UNDO of the block: the hit and its octave back");
        }
    if (jc < 0) return;
    ppq = play_to_k(&s, 0.0, 4);
    seq_jam_queue(&s, LANE_CHORDS, jc, jl);
    CHECK(p->hoct[5][st] == 1, "queued while playing: the octave stays until the block takes over");
    ppq = play_to_k(&s, ppq, 2 * SEQ_STEPS + 1);
    CHECK(!seq_jam_queued(&s, LANE_CHORDS, NULL, NULL) && p->hoct[5][st] == 0 && (!x->on || x->oct == 0),
          "the queued block took over: its octaves with it (djq_t.hoct), the step's gone");
    seq_undo(&s);
    CHECK(p->hoct[5][st] == 1 && x->on && x->oct == 1, "UNDO of the queued block: the octave back");
    (void)ppq;
}

static void test_24_phrase_bass_note(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    unsigned char line[16], was[16];
    int n;
    seq_init(&s, emit, NULL);   /* E minor: Em C G D */
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths: A.x.A.x.A.x.A.x. */
    seq_rbar_open(&s, LANE_BASS, 2);
    CHECK(seq_rbar_sel(&s, LANE_BASS) == 0 && seq_rbar_oct(&s) == 1, "bar 3 opens on its first hit (G1: octave 1)");
    n = s.undo_n;
    s.naud = 0;
    seq_rbar_note(&s, 0);
    CHECK(p->bass.fix[32] == 24 && s.undo_n == n + 1 && s.naud == 1 && s.aud[0].note == 24 && s.aud[0].lane == LANE_BASS,
          "a key: the hit fixed at C1, one undo step, heard on the Bass");
    seq_rbar_note_oct(&s, 3);
    CHECK(p->bass.fix[32] == 48 && seq_rbar_oct(&s) == 3 && s.undo_n == n + 2, "OCTAVE 3: C3");
    seq_rbar_note_oct(&s, 5);
    seq_rbar_note_oct(&s, 0);
    seq_rbar_note(&s, 12);
    seq_rbar_note(&s, 0);
    CHECK(p->bass.fix[32] == 48 && s.undo_n == n + 2, "a bad octave or key, the same note: nothing");
    seq_rbar_note_oct(&s, 1);
    seq_rbar_note_oct(&s, 4);
    CHECK(p->bass.fix[32] == 60 && s.undo_n == n + 4, "OCTAVE 1 then 4: C1, C4");
    seq_rbar_follow_chord(&s);
    bline(&s, 2, line);
    CHECK(!p->bass.fix[32] && line[0] % 12 == 7 && s.undo_n == n + 5, "FOLLOW CHORD unfixes it: G again");
    seq_rbar_follow_chord(&s);
    CHECK(s.undo_n == n + 5, "FOLLOW CHORD on an unfixed hit: nothing");
    seq_rbar_select(&s, LANE_BASS, 1);
    seq_rbar_note(&s, 4);
    CHECK(seq_rbar_sel(&s, LANE_BASS) == 1 && !p->bass.fix[33] && s.undo_n == n + 5, "a rest selects, but takes no note");
    seq_rbar_select(&s, LANE_BASS, 4);
    seq_rbar_note(&s, 4);   /* E1 on step 5 */
    seq_rbar_done(&s, LANE_BASS);
    bline(&s, 2, was);
    seq_rbar_open(&s, LANE_CHORDS, 2);
    seq_rbar_chord(&s, 3, 0);   /* bar 3's slot: Am */
    seq_rbar_done(&s, LANE_CHORDS);
    bline(&s, 2, line);
    CHECK(line[4] == was[4] && line[4] == 28 && line[0] != was[0] && p->bass.fix[36] == 28, "the fixed note (E1) stays when the chord changes under it");
}

static void test_24_newbar_chords(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    int n, r;
    cp_setup(&s);
    ph_draw(&s, LANE_CHORDS, 5, "x...x...x...x...");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    n = s.undo_n;
    r = seq_rbar_new_bar(&s, LANE_CHORDS);
    CHECK(r == 1 && ph_reads(&s, LANE_CHORDS, 5, "x.x...x...x.....") && p->lane[LANE_CHORDS].bar_var[5] == 1 && p->rb[DL_CHORDS][5].ed &&
              s.undo_n == n + 1 && bar_plays_drawn(&s, 5), "NEW BAR v1: beats 2-4 an 8th early, painted in, one undo step");
    r = seq_rbar_new_bar(&s, LANE_CHORDS);
    CHECK(r == 1 && ph_reads(&s, LANE_CHORDS, 5, "x.x...x...x...x.") && p->lane[LANE_CHORDS].bar_var[5] == 2, "v2: the empty 8ths filled");
    r = seq_rbar_new_bar(&s, LANE_CHORDS);
    CHECK(!r && ph_reads(&s, LANE_CHORDS, 5, "x.x...x...x...x.") && p->lane[LANE_CHORDS].bar_var[5] == 2 && s.undo_n == n + 2,
          "nothing left to change: 0, as it was, no undo step");
    seq_undo(&s);
    CHECK(ph_reads(&s, LANE_CHORDS, 5, "x.x...x...x.....") && p->lane[LANE_CHORDS].bar_var[5] == 1 && seq_rbar_bar(&s, LANE_CHORDS) == 5,
          "UNDO steps back one NEW BAR");
    seq_rbar_cancel(&s, LANE_CHORDS);
    ph_draw(&s, LANE_CHORDS, 5, "x..x..x...x.x...");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    r = seq_rbar_new_bar(&s, LANE_CHORDS);
    CHECK(r == 1 && ph_reads(&s, LANE_CHORDS, 5, "x.xx..x...x.x.x.") && p->lane[LANE_CHORDS].bar_var[5] == 2,
          "v1 changes nothing, so NEW BAR goes on to v2");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    seq_rbar_open(&s, LANE_BASS, 1);
    r = seq_rbar_new_bar(&s, LANE_BASS);
    CHECK(r == 1 && p->lane[LANE_BASS].bar_var[1] >= 1, "the Bass: the next variation whose notes differ (0.19)");
    seq_rbar_done(&s, LANE_BASS);
}

static void test_24_reset_bar(void) {
    static seq_t s;
    static ph_state_t at_open;
    seq_part_t *p = &s.part[0];
    unsigned char sustain[16];
    int n;
    static const signed char zero[16];
    cp_setup(&s);
    rhythm_codes(0, sustain);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    n = s.undo_n;
    seq_rbar_reset_bar(&s, LANE_CHORDS);
    CHECK(s.undo_n == n, "nothing to reset: no undo step");
    seq_rbar_done(&s, LANE_CHORDS);
    ph_draw(&s, LANE_CHORDS, 5, "x...............");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    n = seq_rbar_new_bar(&s, LANE_CHORDS);
    CHECK(n && p->lane[LANE_CHORDS].bar_var[5] == 2, "NEW BAR (v2)");
    seq_rbar_chord(&s, 2, 0);   /* the slot: G, picked */
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    seq_rbar_chord(&s, 6, 0);   /* D from step 9 */
    seq_rbar_hit_oct(&s, -1);
    seq_rbar_lock(&s, LANE_CHORDS);
    ph_get(&s, &at_open);
    n = s.undo_n;
    seq_rbar_reset_bar(&s, LANE_CHORDS);
    CHECK(p->prog[5] == 2 && !(p->slot_lock >> 5 & 1) && !seq_bar_picked(&s, LANE_CHORDS, 5), "RESET BAR: the slot keeps its chord (G) and loses its picked mark");
    CHECK(seq_cp_count(&s, 5) == 0 && !memcmp(p->hoct[5], zero, 16) && !memcmp(p->rb[DL_CHORDS][5].c, sustain, 16) && !p->rb[DL_CHORDS][5].ed &&
              !p->rb[DL_CHORDS][5].lock && !p->lane[LANE_CHORDS].bar_var[5] && s.undo_n == n + 1,
          "its change points, hit octaves, paint, v and lock cleared: one undo step");
    seq_undo(&s);
    CHECK(ph_same(&s, &at_open) && seq_rbar_bar(&s, LANE_CHORDS) == 5, "UNDO puts all of it back");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    seq_rbar_open(&s, LANE_BASS, 1);
    seq_rbar_note(&s, 2);
    seq_rbar_paint(&s, LANE_BASS, 1, RC_HIT);
    seq_rbar_new_bar(&s, LANE_BASS);
    seq_rbar_lock(&s, LANE_BASS);
    n = s.undo_pushes;   /* the ring is full here */
    seq_rbar_reset_bar(&s, LANE_BASS);
    CHECK(!seq_bass_fixed_count(&s, 1) && !(p->bass.nlock >> 1 & 1) && !p->rb[DL_BASS][1].lock && !p->rb[DL_BASS][1].ed &&
              !p->lane[LANE_BASS].bar_var[1] && ph_reads(&s, LANE_BASS, 1, "A.x.A.x.A.x.A.x.") && (p->slot_lock >> 1 & 1) && s.undo_pushes == n + 1,
          "the Bass: its fixed notes, paint, variation and locks cleared (the slot LOCK picked stays)");
    seq_rbar_done(&s, LANE_BASS);
}

static void test_24_lock_combined(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    unsigned char line[16], was[16];
    int n;
    cp_setup(&s);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    n = s.undo_n;
    seq_rbar_lock(&s, LANE_CHORDS);
    CHECK(p->rb[DL_CHORDS][5].lock && (p->slot_lock >> 5 & 1) && !seq_bar_picked(&s, LANE_CHORDS, 5) && s.undo_n == n + 1,
          "LOCK: the rhythm locked, the slot that starts in it picked (one undo step); the tag reads LOCKED, not picked");
    seq_rbar_lock(&s, LANE_CHORDS);
    CHECK(!p->rb[DL_CHORDS][5].lock && (p->slot_lock >> 5 & 1) && seq_bar_picked(&s, LANE_CHORDS, 5) && s.undo_n == n + 2,
          "UNLOCK: the rhythm lock only; the slot stays picked");
    seq_undo(&s);
    seq_undo(&s);
    CHECK(!p->rb[DL_CHORDS][5].lock && !(p->slot_lock >> 5 & 1) && s.undo_n == n, "two UNDOs: as before (each one step)");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_lock(&s, LANE_BASS);
    bline(&s, 2, was);
    CHECK(p->rb[DL_BASS][2].lock && (p->bass.nlock >> 2 & 1) && !memcmp(&p->bass.frozen[2], &p->bass.set, sizeof p->bass.set) &&
              (p->slot_lock >> 2 & 1), "the Bass: LOCK locks the rhythm and the notes (frozen settings) and picks the slot");
    seq_rbar_done(&s, LANE_BASS);
    seq_set_bass_style(&s, 5);
    bline(&s, 2, line);
    CHECK(!memcmp(line, was, 16), "a STYLE pick leaves the locked bar's notes");
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_lock(&s, LANE_BASS);
    CHECK(!p->rb[DL_BASS][2].lock && !(p->bass.nlock >> 2 & 1) && (p->slot_lock >> 2 & 1), "UNLOCK: both locks cleared, the pick stays");
    seq_rbar_done(&s, LANE_BASS);
    cp_setup(&s);
    seq_set_chord_len(&s, 32);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_lock(&s, LANE_CHORDS);
    CHECK(p->rb[DL_CHORDS][5].lock && !p->slot_lock, "a 2-bar slot's second bar: LOCK picks no slot (none starts in it)");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rbar_open(&s, LANE_CHORDS, 4);
    seq_rbar_lock(&s, LANE_CHORDS);
    seq_rbar_lock(&s, LANE_CHORDS);
    CHECK(p->slot_lock == 1u << 2 && seq_bar_picked(&s, LANE_CHORDS, 4) && !seq_bar_picked(&s, LANE_CHORDS, 5), "its first bar picks slot 3; the tag is the first bar's");
    seq_rbar_done(&s, LANE_CHORDS);
}

static void test_24_copy_modes(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    int k13, rep0 = -1, rep1 = -1;
    cp_setup(&s);
    seq_set_bars(&s, 16);
    bar_lock(&s, 9);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    CHECK(seq_rcopy_same_place(&s, LANE_CHORDS) == ((1u << 1) | (1u << 13)), "SAME IN EVERY PHRASE for bar 6: bars 2 and 14 (10 locked)");
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    seq_rbar_chord(&s, 6, 0);   /* D from step 9 */
    seq_rbar_hit_oct(&s, 1);
    CHECK(seq_rcopy_what(&s, LANE_CHORDS) == 0, "COPY reads RHYTHM ONLY at open");
    seq_rbar_copy(&s, LANE_CHORDS, 1u << 1);
    CHECK(ph_reads(&s, LANE_CHORDS, 1, "x-------x-------") && seq_cp_count(&s, 1) == 0 && !p->hoct[1][8], "RHYTHM ONLY: the codes alone");
    k13 = p->prog[13];
    seq_rbar_copy_counts(&s, LANE_CHORDS, 1u << 1, &rep0, NULL);
    seq_rcopy_set_what(&s, LANE_CHORDS, 1);
    seq_rcopy_set_what(&s, LANE_CHORDS, 2);
    seq_rbar_copy_counts(&s, LANE_CHORDS, 1u << 1, &rep1, NULL);
    CHECK(seq_rcopy_what(&s, LANE_CHORDS) == 1 && rep0 == 0 && rep1 == 1, "+ CHORDS (a bad value ignored); release fix: bar 2 (the same rhythm, "
          "no change point) counts as differing with it (%d), not without (%d)", rep1, rep0);
    seq_rcopy_open(&s, LANE_CHORDS);
    seq_rcopy_toggle(&s, LANE_CHORDS, 13);
    seq_rcopy_go(&s, LANE_CHORDS);
    CHECK(ph_reads(&s, LANE_CHORDS, 13, "x-------x-------") && seq_cp_count(&s, 13) == 1 && p->cp[13][0].step == 8 && p->cp[13][0].deg == 6 &&
              p->hoct[13][8] == 1 && p->prog[13] == k13 && ph_deg(&s, 13, 8) == 6, "+ CHORDS: its change points and octaves too, the slot's own chord kept");
    seq_rbar_copy(&s, LANE_CHORDS, 1u << 9);
    CHECK(seq_cp_count(&s, 9) == 0 && !p->rb[DL_CHORDS][9].ed, "a locked bar is skipped");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    CHECK(seq_rcopy_what(&s, LANE_CHORDS) == 0, "the next open: RHYTHM ONLY again");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_note(&s, 4);
    seq_rbar_copy(&s, LANE_BASS, 1u << 3);
    CHECK(!seq_bass_fixed_count(&s, 3), "the Bass, RHYTHM ONLY: no fixed note copied");
    seq_rbar_copy_counts(&s, LANE_BASS, 1u << 3, &rep0, NULL);
    seq_rcopy_set_what(&s, LANE_BASS, 1);
    seq_rbar_copy_counts(&s, LANE_BASS, 1u << 3, &rep1, NULL);
    CHECK(rep0 == 0 && rep1 == 1, "release fix: + NOTES counts bar 4 (the same rhythm, no fixed note) as differing (%d, without %d)", rep1, rep0);
    seq_rbar_copy(&s, LANE_BASS, 1u << 3);
    CHECK(seq_bass_fixed_count(&s, 3) == 1 && p->bass.fix[48] == p->bass.fix[32], "+ NOTES: the fixed notes too");
    seq_rbar_done(&s, LANE_BASS);
}

/* Review Focus: every UNDO steps back one edit; CANCEL puts all of it back */
/* Review Focus: every UNDO steps back one edit (SEQ_UNDO 8 holds 7 after the open's step: two runs); CANCEL puts all of it back */
static void test_24_phrase_undo_cancel_mix(void) {
    static seq_t s;
    static ph_state_t st[8], open;
    int k = 0, ok = 1, n, r;
    cp_setup(&s);
    seq_rhythm_pick(&s, LANE_BASS, 3);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    ph_get(&s, &open);
    n = s.undo_n;
    ph_get(&s, &st[k++]);
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    ph_get(&s, &st[k++]);
    seq_rbar_select(&s, LANE_CHORDS, 0);
    seq_rbar_chord(&s, 2, 0);   /* the slot */
    ph_get(&s, &st[k++]);
    seq_rbar_select(&s, LANE_CHORDS, 8);
    seq_rbar_chord(&s, 6, 0);   /* a point */
    ph_get(&s, &st[k++]);
    seq_rbar_type(&s, 3);
    ph_get(&s, &st[k++]);
    seq_rbar_hit_oct(&s, 1);
    ph_get(&s, &st[k++]);
    seq_rbar_lock(&s, LANE_CHORDS);
    CHECK(s.undo_n == n + 6 && n + 6 <= SEQ_UNDO, "paint, slot pick, point, TYPE, octave, LOCK: 6 steps");
    for (int i = k - 1; i >= 0; i--) {
        seq_undo(&s);
        ok &= ph_same(&s, &st[i]) && seq_rbar_bar(&s, LANE_CHORDS) == 5;
    }
    CHECK(ok && s.undo_n == n, "each UNDO steps back exactly one, the bar staying open");
    k = 0;
    ph_get(&s, &st[k++]);
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    ph_get(&s, &st[k++]);
    seq_rbar_chord(&s, 2, 0);   /* G from step 9 */
    ph_get(&s, &st[k++]);
    seq_rbar_paint(&s, LANE_CHORDS, 12, RC_HIT);
    ph_get(&s, &st[k++]);
    seq_rbar_select(&s, LANE_CHORDS, 8);
    ok &= seq_rbar_this_hit_only(&s) == 1;
    ph_get(&s, &st[k++]);
    seq_rcopy_set_what(&s, LANE_CHORDS, 1);
    seq_rbar_copy(&s, LANE_CHORDS, (1u << 1) | (1u << 2));
    ph_get(&s, &st[k++]);
    seq_rbar_lock(&s, LANE_CHORDS);
    CHECK(ok && s.undo_n == n + 6 && seq_cp_count(&s, 1) == 2 && seq_cp_count(&s, 2) == 2, "paint, point, paint, THIS HIT ONLY, copy + CHORDS, LOCK");
    for (int i = k - 1; i >= 0; i--) {
        seq_undo(&s);
        ok &= ph_same(&s, &st[i]) && seq_rbar_bar(&s, LANE_CHORDS) == 5;
    }
    CHECK(ok && s.undo_n == n, "each UNDO steps back exactly one again");
    seq_rbar_paint(&s, LANE_CHORDS, 8, RC_HIT);
    seq_rbar_chord(&s, 6, 0);
    seq_rbar_hit_oct(&s, -2);
    seq_rcopy_set_what(&s, LANE_CHORDS, 1);
    seq_rbar_copy(&s, LANE_CHORDS, 1u << 6);
    seq_rbar_select(&s, LANE_CHORDS, 0);
    seq_rbar_chord(&s, 1, 2);
    seq_rbar_lock(&s, LANE_CHORDS);
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(ph_same(&s, &open) && s.undo_n == n && seq_rbar_bar(&s, LANE_CHORDS) == -1, "CANCEL: slots, picks, points, octaves, bars, locks as at open");
    seq_rbar_open(&s, LANE_BASS, 2);
    ph_get(&s, &open);
    n = s.undo_n;
    seq_rbar_note(&s, 5);
    seq_rbar_select(&s, LANE_BASS, 4);
    seq_rbar_note_oct(&s, 3);
    seq_rcopy_set_what(&s, LANE_BASS, 1);
    seq_rbar_copy(&s, LANE_BASS, (1u << 3) | (1u << 7));
    r = seq_rbar_new_bar(&s, LANE_BASS);
    seq_rbar_lock(&s, LANE_BASS);
    CHECK(s.undo_n == n + 4 + r && seq_bass_fixed_count(&s, 7) == 2 && (s.part[0].bass.nlock >> 2 & 1), "the Bass: fix, octave, copy + NOTES, NEW BAR, LOCK");
    seq_undo(&s);
    CHECK(!(s.part[0].bass.nlock >> 2 & 1) && !s.part[0].rb[DL_BASS][2].lock && s.undo_n == n + 3 + r, "UNDO: the LOCK goes (both locks, the pick)");
    seq_rbar_cancel(&s, LANE_BASS);
    CHECK(ph_same(&s, &open) && s.undo_n == n, "CANCEL: fixes, variations, note locks and frozen settings as at open");
}

/* moved here from Task 1 (controller ruling 1) */
static void test_24_cp_undo(void) {
    static seq_t s;
    int n;
    cp_setup(&s);
    ph_draw(&s, LANE_CHORDS, 5, "x-------x-------");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    n = s.undo_n;
    seq_rbar_select(&s, LANE_CHORDS, 8);
    seq_rbar_chord(&s, 2, 0);
    CHECK(seq_cp_count(&s, 5) == 1 && s.undo_n == n + 1, "a point set through PHRASE's pick: one undo step");
    seq_undo(&s);
    CHECK(seq_cp_count(&s, 5) == 0 && ph_deg(&s, 5, 8) == 5 && seq_rbar_bar(&s, LANE_CHORDS) == 5 && s.undo_n == n, "one UNDO takes it back");
    seq_rbar_chord(&s, 2, 0);
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(seq_cp_count(&s, 5) == 0 && ph_deg(&s, 5, 8) == 5 && s.undo_n == n, "CANCEL of the bar removes it");
}

/* review fix 1: a Bass paint never touches the Chords' hit octaves (paint, UNDO, CANCEL) */
static void test_24_phrase_cross_lane_hoct(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    seq_step_t ch[SEQ_STEPS];
    int ok;
    cp_setup(&s);
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths: A.x.A.x.A.x.A.x. */
    seq_rbar_open(&s, LANE_CHORDS, 2);   /* Sustain: the hit on step 1 */
    seq_rbar_hit_oct(&s, 1);
    seq_rbar_done(&s, LANE_CHORDS);
    memcpy(ch, &p->lane[LANE_CHORDS].steps[2 * SEQ_STEPS], sizeof ch);
    seq_rbar_open(&s, LANE_BASS, 2);
    seq_rbar_paint(&s, LANE_BASS, 0, RC_REST);
    ok = p->hoct[2][0] == 1 && ph_reads(&s, LANE_BASS, 2, "..x.A.x.A.x.A.x.");
    seq_undo(&s);
    ok &= p->hoct[2][0] == 1 && ph_reads(&s, LANE_BASS, 2, "A.x.A.x.A.x.A.x.") && seq_rbar_bar(&s, LANE_BASS) == 2;
    seq_rbar_paint(&s, LANE_BASS, 0, RC_REST);
    seq_rbar_cancel(&s, LANE_BASS);
    ok &= p->hoct[2][0] == 1 && ph_reads(&s, LANE_BASS, 2, "A.x.A.x.A.x.A.x.");
    CHECK(ok && !memcmp(ch, &p->lane[LANE_CHORDS].steps[2 * SEQ_STEPS], sizeof ch) && ch[0].oct == 1,
          "REST on Bass bar 3 step 1: the Chords hit octave there stays +1 through the paint, UNDO and CANCEL, its steps unchanged");
}

/* review fix 2: a Bass bar with only a note lock is locked for copies (as for JAM): never a target, counted as skipped */
static void test_24_copy_note_lock(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    unsigned char c3[16], f3[16];
    int rep = -1, skip = -1, n;
    cp_setup(&s);   /* 8 bars */
    seq_rhythm_pick(&s, LANE_BASS, 3);
    p->bass.frozen[3] = p->bass.set;   /* bar 4 note-locked alone (a 0.19 project's, or JAM's) */
    p->bass.nlock = 1u << 3;
    memcpy(c3, p->rb[DL_BASS][3].c, 16);
    memcpy(f3, &p->bass.fix[3 * SEQ_STEPS], 16);
    seq_rbar_open(&s, LANE_BASS, 7);
    seq_rbar_paint(&s, LANE_BASS, 1, RC_HIT);
    seq_rbar_select(&s, LANE_BASS, 0);
    seq_rbar_note(&s, 2);
    seq_rcopy_set_what(&s, LANE_BASS, 1);
    CHECK(seq_rcopy_same_place(&s, LANE_BASS) == 0, "SAME IN EVERY PHRASE for bar 8 leaves out bar 4 (note-locked)");
    n = seq_rbar_copy_counts(&s, LANE_BASS, 0xFFFF, &rep, &skip);
    CHECK(n == 6 && skip == 1, "COPY TO ALL's counts: 6 bars replaced, the note-locked one skipped (%d, %d)", n, skip);
    seq_rbar_copy(&s, LANE_BASS, 0xFFFF);
    CHECK(!memcmp(c3, p->rb[DL_BASS][3].c, 16) && !memcmp(f3, &p->bass.fix[3 * SEQ_STEPS], 16) && ph_reads(&s, LANE_BASS, 0, "Axx.A.x.A.x.A.x.") &&
              p->bass.fix[0] == p->bass.fix[7 * SEQ_STEPS], "COPY TO ALL with + NOTES: bar 4 keeps its rhythm and notes, the others take both");
    seq_rcopy_open(&s, LANE_BASS);
    seq_rcopy_toggle(&s, LANE_BASS, 3);
    CHECK(seq_rcopy_mask(&s, LANE_BASS) == 0, "COPY TO...: bar 4 can't be picked");
    seq_rcopy_set_all(&s, LANE_BASS, 1);
    CHECK(seq_rcopy_mask(&s, LANE_BASS) == 0x77, "COPY TO...'s ALL leaves it out (0x%x)", seq_rcopy_mask(&s, LANE_BASS));
    seq_rcopy_go(&s, LANE_BASS);
    CHECK(!memcmp(c3, p->rb[DL_BASS][3].c, 16) && !memcmp(f3, &p->bass.fix[3 * SEQ_STEPS], 16), "and its copy leaves bar 4 as it was");
    seq_rbar_done(&s, LANE_BASS);
}

/* 0.24 Task 3: LOCK WITH and ANTICIPATE. A new instance: E minor, Em C G D at 1-bar chords, 4 bars; Chords on channel 1, Bass on 2 */

/* every bar of the loop of a drawn lane painted with codes s16 (ed), rendered */
static void lane_codes(seq_t *s, int lane, const char *s16) {
    seq_part_t *p = &s->part[s->cur_part];
    for (int b = 0; b < p->bars; b++) {
        rhythm_parse(s16, p->rb[lane == LANE_BASS ? DL_BASS : DL_CHORDS][b].c);
        p->rb[lane == LANE_BASS ? DL_BASS : DL_CHORDS][b].ed = 1;
    }
    render_now(s);
}

/* the bar as it plays (seq_bar_plays) as text, and its drawn codes (seq_bar_codes) */
static const char *plays_str(const seq_t *s, int lane, int bar, char out[17]) {
    unsigned char c[16];
    seq_bar_plays(s, lane, bar, c);
    rhythm_format(c, out);
    return out;
}
static const char *codes_str(const seq_t *s, int lane, int bar, char out[17]) {
    unsigned char c[16];
    seq_bar_codes(s, lane, bar, c);
    rhythm_format(c, out);
    return out;
}

static void test_24_lockwith(void) {
    static seq_t s;
    char t[17], g[17], b0[17];
    unsigned char c[16], gaps[16];
    seq_part_t *p;
    nev = 0;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    for (int b = 0; b < 4; b++) rhythm_parse("x..x..x...x.x...", p->rb[DL_BASS][b].c), p->rb[DL_BASS][b].ed = 1;
    rhythm_parse("...x..x.x.......", p->rb[DL_BASS][0].c);
    render_now(&s);
    codes_str(&s, LANE_CHORDS, 3, b0);   /* the Chords' own (a library rhythm) */
    seq_rbar_open(&s, LANE_CHORDS, 1);
    seq_rbar_paint(&s, LANE_CHORDS, 2, RC_ACC);   /* bar 2 painted: its own */
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rbar_open(&s, LANE_CHORDS, 2);
    seq_rbar_lock(&s, LANE_CHORDS);   /* bar 3 locked as it is */
    seq_rbar_done(&s, LANE_CHORDS);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_OWN && seq_lockwith(&s, LANE_BASS) == LW_OWN && seq_anticipate(&s, LANE_CHORDS) == AN_OFF &&
          !seq_bar_follows(&s, LANE_CHORDS, 0), "a new instance: both lanes OWN, ANTICIPATE OFF");
    int undo0 = s.undo_n;
    seq_set_lockwith(&s, LANE_CHORDS, LW_WITH);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_WITH && s.undo_n == undo0 + 1 && !seq_lockwith_bumped(&s), "WITH: one undo step");
    CHECK(!strcmp(codes_str(&s, LANE_CHORDS, 0, t), "...x..x.x.......") && !strcmp(codes_str(&s, LANE_CHORDS, 3, t), "x..x..x...x.x...") &&
          !strcmp(plays_str(&s, LANE_CHORDS, 3, g), "x..x..x...x.x...") && seq_bar_follows(&s, LANE_CHORDS, 0) == LW_WITH &&
          seq_bar_follows(&s, LANE_CHORDS, 3) == LW_WITH, "the unpainted, unlocked bars take the Bass's drawn hits and play them (%s)", g);
    seq_bar_codes(&s, LANE_CHORDS, 1, c);
    CHECK(c[2] == RC_ACC && !seq_bar_follows(&s, LANE_CHORDS, 1), "the painted bar keeps its own (%s)", codes_str(&s, LANE_CHORDS, 1, t));
    CHECK(!seq_bar_follows(&s, LANE_CHORDS, 1) && !seq_bar_follows(&s, LANE_CHORDS, 2) && !strcmp(codes_str(&s, LANE_CHORDS, 2, t), b0),
          "and so does the locked one (%s)", t);
    seq_rbar_open(&s, LANE_BASS, 3);
    seq_rbar_paint(&s, LANE_BASS, 1, RC_HIT);
    seq_rbar_done(&s, LANE_BASS);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 3, t), "xx.x..x...x.x...") && !strcmp(codes_str(&s, LANE_BASS, 3, g), "xx.x..x...x.x..."),
          "a Bass paint: the Chords bar that follows it plays it at once (%s)", t);
    seq_set_lockwith(&s, LANE_CHORDS, LW_GAPS);
    rhythm_parse("...x..x.x.......", c);
    rhythm_gaps(c, gaps);
    seq_bar_codes(&s, LANE_CHORDS, 0, c);
    CHECK(!memcmp(c, gaps, 16) && seq_bar_follows(&s, LANE_CHORDS, 0) == LW_GAPS, "IN THE GAPS: rhythm_gaps of the Bass's bar (%s)",
          codes_str(&s, LANE_CHORDS, 0, t));
    seq_undo(&s);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_WITH && !strcmp(codes_str(&s, LANE_CHORDS, 0, t), "...x..x.x......."), "UNDO: WITH again");
    /* ruling 2: the editor reads what plays */
    seq_rbar_open(&s, LANE_CHORDS, 0);
    CHECK(seq_rbar_sel(&s, LANE_CHORDS) == 3, "a WITH bar opened in the editor selects its effective first hit (step %d)", seq_rbar_sel(&s, LANE_CHORDS) + 1);
    seq_rbar_hit_oct(&s, 1);
    CHECK(p->hoct[0][3] == 1, "the hit octave takes the effective hit");
    seq_rbar_paint(&s, LANE_CHORDS, 3, RC_HIT);   /* its code already: selects only */
    CHECK(seq_bar_follows(&s, LANE_CHORDS, 0) == LW_WITH, "the paint it has only selects: still following");
    seq_rbar_paint(&s, LANE_CHORDS, 12, RC_HIT);
    CHECK(!seq_bar_follows(&s, LANE_CHORDS, 0) && p->rb[DL_CHORDS][0].ed && !strcmp(codes_str(&s, LANE_CHORDS, 0, t), "...x..x.x...x..."),
          "a paint starts from the effective codes and makes the bar its own (%s)", t);
    seq_rbar_cancel(&s, LANE_CHORDS);
    CHECK(seq_bar_follows(&s, LANE_CHORDS, 0) == LW_WITH, "CANCEL: following again");
    seq_rbar_open(&s, LANE_CHORDS, 0);
    seq_rbar_lock(&s, LANE_CHORDS);
    seq_rbar_done(&s, LANE_CHORDS);
    seq_rbar_open(&s, LANE_BASS, 0);
    seq_rbar_paint(&s, LANE_BASS, 0, RC_HIT);
    seq_rbar_done(&s, LANE_BASS);
    CHECK(!seq_bar_follows(&s, LANE_CHORDS, 0) && p->rb[DL_CHORDS][0].ed && !strcmp(codes_str(&s, LANE_CHORDS, 0, t), "...x..x.x.......") &&
          !strcmp(plays_str(&s, LANE_CHORDS, 0, g), "...x..x.x......."), "LOCK freezes the codes it followed as its own: a Bass paint leaves it (%s)", g);
    /* a RHYTHM pick and a JAM tap turn the lane OWN */
    undo0 = s.undo_n;
    seq_rhythm_pick(&s, LANE_CHORDS, 0);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_OWN && s.undo_n == undo0 + 1, "a RHYTHM pick turns it OWN (one undo step)");
    seq_undo(&s);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_WITH, "its UNDO: WITH again");
    seq_rbrowse_open(&s, LANE_CHORDS);
    seq_rbrowse_try(&s, LANE_CHORDS, 5);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_OWN, "a RHYTHMS browser try too");
    seq_browse_cancel(&s);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_WITH && seq_bar_follows(&s, LANE_CHORDS, 3) == LW_WITH, "its CANCEL: WITH again");
    seq_jam_apply_now(&s, LANE_CHORDS, 0, 0);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_OWN, "a JAM tap turns it OWN");
    seq_undo(&s);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_WITH, "its UNDO: WITH again");
    seq_rbar_open(&s, LANE_CHORDS, 3);
    undo0 = s.undo_n;
    seq_rhythm_pick(&s, LANE_CHORDS, 3);   /* ruling 16: a pick in the open bar (Pulse) */
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_WITH && !seq_bar_follows(&s, LANE_CHORDS, 3) && p->rb[DL_CHORDS][3].ed &&
              !strcmp(codes_str(&s, LANE_CHORDS, 3, t), "x---x---x---x---") && seq_bar_follows(&s, LANE_CHORDS, 2) == 0 && s.undo_n == undo0 + 1,
          "a RHYTHM pick in the open bar makes only that bar its own (%s); the lane stays WITH", t);
    seq_undo(&s);
    CHECK(seq_bar_follows(&s, LANE_CHORDS, 3) == LW_WITH && seq_rbar_bar(&s, LANE_CHORDS) == 3, "its UNDO: the bar follows again, still open");
    seq_rbrowse_open(&s, LANE_CHORDS);   /* the RHYTHMS browser in the open bar */
    seq_rbrowse_try(&s, LANE_CHORDS, 5);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_WITH && !seq_bar_follows(&s, LANE_CHORDS, 3), "a try in the open bar's RHYTHMS: that bar its own, the lane WITH");
    seq_browse_cancel(&s);
    CHECK(seq_bar_follows(&s, LANE_CHORDS, 3) == LW_WITH && seq_rbar_bar(&s, LANE_CHORDS) == 3, "the browser's CANCEL: the bar follows again, still open");
    seq_rbar_copy(&s, LANE_CHORDS, ~0u);   /* COPY TO ALL from a bar that follows */
    CHECK(!strcmp(codes_str(&s, LANE_CHORDS, 1, t), "xx.x..x...x.x...") && !seq_bar_follows(&s, LANE_CHORDS, 1) && p->rb[DL_CHORDS][1].ed,
          "a copy from a WITH bar writes what it plays, as the target's own (%s)", t);
    seq_rbar_cancel(&s, LANE_CHORDS);
    seq_set_lockwith(&s, LANE_CHORDS, LW_OWN);
    CHECK(!strcmp(codes_str(&s, LANE_CHORDS, 3, t), b0), "OWN: the bars read their own codes again (%s)", t);
}

/* Review Focus: both lanes set to follow each other: the one set last wins, the other turns OWN with a message */
static void test_24_lockwith_conflict(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    char a[17], b[17];
    unsigned char c[16], gaps[16];
    seq_part_t *p;
    nev = 0;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    seq_rhythm_pick(&s, LANE_CHORDS, 4);   /* Eighths, A.x.x.x.A.x.x.x. (neither lane painted: both may follow) */
    seq_rhythm_pick(&s, LANE_BASS, 2);     /* Quarters, A...x...x...x... */
    seq_set_lockwith(&s, LANE_CHORDS, LW_WITH);
    seq_set_lockwith(&s, LANE_BASS, LW_GAPS);
    CHECK(seq_lockwith(&s, LANE_BASS) == LW_GAPS && seq_lockwith(&s, LANE_CHORDS) == LW_OWN && seq_lockwith_bumped(&s),
          "Bass IN THE GAPS while the Chords follow it: the Chords turn OWN, the message flag is up");
    seq_bar_codes(&s, LANE_CHORDS, 0, c);
    rhythm_gaps(c, gaps);
    seq_bar_codes(&s, LANE_BASS, 0, c);
    CHECK(!memcmp(c, gaps, 16) && !seq_bar_follows(&s, LANE_CHORDS, 0) && !strcmp(plays_str(&s, LANE_BASS, 0, a), ".x...x...x...x.."),
          "the Bass plays the gaps of the Chords' own bar (%s, %s)", a, codes_str(&s, LANE_CHORDS, 0, b));
    seq_set_anticipate(&s, LANE_BASS, AN_8);
    CHECK(!seq_lockwith_bumped(&s), "the next setter clears the flag");
    seq_undo(&s);
    seq_undo(&s);
    CHECK(seq_lockwith(&s, LANE_CHORDS) == LW_WITH && seq_lockwith(&s, LANE_BASS) == LW_OWN && !strcmp(codes_str(&s, LANE_CHORDS, 0, a), "A...x...x...x..."),
          "UNDO (twice): the Chords follow the Bass again, the Bass OWN");
    seq_set_lockwith(&s, LANE_BASS, LW_OWN);
    CHECK(!seq_lockwith_bumped(&s) && seq_lockwith(&s, LANE_CHORDS) == LW_WITH, "the Bass set OWN: nothing to turn");
    seq_set_anticipate(&s, LANE_CHORDS, AN_BEAT);
    CHECK(seq_state_save(&s, buf, sizeof buf) > 0 && strstr(buf, "|A1.la=1,3") && !strstr(buf, "|A2.la="), "saved: |A1.la=1,3, the Bass's default not");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && seq_lockwith(&t, LANE_CHORDS) == LW_WITH && seq_anticipate(&t, LANE_CHORDS) == AN_BEAT &&
          !memcmp(t.part[0].lane[LANE_CHORDS].steps, p->lane[LANE_CHORDS].steps, sizeof p->lane[0].steps) &&
          !memcmp(t.part[0].sflag, p->sflag, sizeof p->sflag), "it loads back: the same steps and flags");
    strcat(buf, "|A2.la=2,1");
    CHECK(seq_state_load(&t, buf) && seq_lockwith(&t, LANE_CHORDS) == LW_WITH && seq_lockwith(&t, LANE_BASS) == LW_OWN &&
          seq_anticipate(&t, LANE_BASS) == AN_16, "a chunk where both follow each other loads with the Bass OWN");
    strcat(buf, "|A1.la=7,x");
    CHECK(seq_state_load(&t, buf) && seq_lockwith(&t, LANE_CHORDS) == LW_OWN && seq_anticipate(&t, LANE_CHORDS) == AN_OFF, "a bad .la loads the defaults");
    /* a JAM block queued on the Bass while the Chords follow it: the Chords take the block's rhythm at its boundary, with it */
    seq_init(&s, emit, NULL);
    seq_set_lockwith(&s, LANE_CHORDS, LW_WITH);
    play(&s, 0.0, 1.0, 0);
    CHECK(seq_jam_queue(&s, LANE_BASS, 2, 3) == 1, "a Bass block queued while playing");
    play(&s, 1.0, 3.5, 0);
    CHECK(s.part[0].djam[DL_BASS].c == 2, "it took over at the bar line");
    seq_bar_codes(&s, LANE_BASS, 1, c);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 1, a), codes_str(&s, LANE_BASS, 1, b)) && seq_bar_follows(&s, LANE_CHORDS, 1) == LW_WITH,
          "and the Chords play its rhythm from then (%s, %s)", a, b);
}

/* ANTICIPATE on Em C G D: each next downbeat an 8th early into the bar before, playing the next chord */
static void test_24_anticipate(void) {
    static seq_t s;
    static const int PC[4][3] = {{4, 7, 11}, {0, 4, 7}, {7, 11, 2}, {2, 6, 9}}, ROOT[4] = {4, 0, 7, 2};
    char t[17];
    int n[THEORY_CHORD_MAX], ok = 1, undo0;
    seq_part_t *p;
    nev = 0;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    lane_codes(&s, LANE_CHORDS, "x..x..x...x.x...");
    lane_codes(&s, LANE_BASS, "x..x..x...x.x...");
    undo0 = s.undo_n;
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    CHECK(seq_anticipate(&s, LANE_CHORDS) == AN_8 && s.undo_n == undo0 + 1, "ANTICIPATE 8TH: one undo step");
    for (int b = 0; b < 4; b++) {
        int c = seq_part_step_notes(&s, 0, LANE_CHORDS, b, 14, n);
        ok &= !strcmp(plays_str(&s, LANE_CHORDS, b, t), "-..x..x...x.x.x-") && pcs_in(n, c, PC[(b + 1) % 4], 3) &&
              seq_step_flags(&s, LANE_CHORDS, b, 14) == (SF_EARLY | SF_ADDED) && seq_step_flags(&s, LANE_CHORDS, b, 0) == SF_TIED &&
              !seq_step_flags(&s, LANE_CHORDS, b, 3) && p->lane[LANE_CHORDS].steps[b * 16 + 14].len == 3;
    }
    CHECK(ok, "Push: every next downbeat on step 15 of the bar before, playing the next chord (bar 4's: Em, over the wrap), step 16 and the "
          "downbeat held (%s)", plays_str(&s, LANE_CHORDS, 0, t));
    CHECK(!strcmp(plays_str(&s, LANE_BASS, 0, t), "x..x..x...x.x..."), "the Bass, still OFF, as it was");
    seq_set_anticipate(&s, LANE_BASS, AN_8);
    ok = 1;
    for (int b = 0; b < 4; b++) {
        const seq_step_t *x = &p->lane[LANE_BASS].steps[b * 16 + 14];
        ok &= x->on && x->abs % 12 == ROOT[(b + 1) % 4] && p->brole[b * 16 + 14] == '1' && !strcmp(plays_str(&s, LANE_BASS, b, t), "-..x..x...x.x.x-");
    }
    CHECK(ok, "the Bass: the next chord's root on step 15");
    seq_undo(&s);
    CHECK(seq_anticipate(&s, LANE_BASS) == AN_OFF && !strcmp(plays_str(&s, LANE_BASS, 1, t), "x..x..x...x.x...") && !seq_step_flags(&s, LANE_BASS, 1, 0),
          "its UNDO: OFF, the Bass as it was");
    lane_codes(&s, LANE_CHORDS, "A.x.A.x.A.x.A.x.");
    seq_set_anticipate(&s, LANE_CHORDS, AN_16);
    ok = 1;
    for (int b = 0; b < 4; b++) {
        int c = seq_part_step_notes(&s, 0, LANE_CHORDS, b, 15, n);
        ok &= !strcmp(plays_str(&s, LANE_CHORDS, b, t), "-.x.A.x.A.x.A.xA") && pcs_in(n, c, PC[(b + 1) % 4], 3);
    }
    CHECK(ok, "16TH on Eighths: step 16 takes the downbeat's accent and the next chord (%s)", plays_str(&s, LANE_CHORDS, 2, t));
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 2, t), "A.x.A.x.A.x.A.x.") && !seq_step_flags(&s, LANE_CHORDS, 2, 0), "8TH there: step 15 sounds, nothing moves (%s)", t);
    seq_set_anticipate(&s, LANE_CHORDS, AN_OFF);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 2, t), "A.x.A.x.A.x.A.x."), "OFF");
}

/* the chords lane's note-ons (channel 1) since `from` fired at song step k (mod m): count */
static int ch1_ons_at(int from, long k, long m) {
    int c = 0;
    for (int i = from; i < nev; i++) c += is_on(&evs[i]) && (evs[i].status & 15) == 0 && evs[i].k >= 0 && evs[i].k % m == k;
    return c;
}

/* Review Focus: ANTICIPATE across the loop wrap, into and out of a locked bar, at 2-bar chords, into a change point, a busy bar, a FILL
 * bar, a LOCK WITH bar */
static void test_24_anticipate_edges(void) {
    static seq_t s;
    char t[17], u[17];
    int n[THEORY_CHORD_MAX], c;
    nev = 0;
    seq_init(&s, emit, NULL);
    lane_codes(&s, LANE_CHORDS, "x..x..x...x.x...");
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    c = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 14, n);
    CHECK(seq_step_flags(&s, LANE_CHORDS, 0, 0) == SF_TIED && pcs_in(n, c, (const int[]){4, 7, 11}, 3), "the wrap: bar 1's Em on bar 4's step 15");
    seq_rbar_open(&s, LANE_CHORDS, 2);
    seq_rbar_lock(&s, LANE_CHORDS);
    seq_rbar_done(&s, LANE_CHORDS);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 2, t), "x..x..x...x.x...") && !strcmp(plays_str(&s, LANE_CHORDS, 1, u), "-..x..x...x.x..."),
          "bar 3 locked: its downbeat stays, and bar 4's never moves into it (%s, bar 2 %s)", t, u);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 3, t), "x..x..x...x.x.x-") && !strcmp(plays_str(&s, LANE_CHORDS, 0, u), "-..x..x...x.x.x-"),
          "the others as before (%s, %s)", t, u);
    seq_rbar_open(&s, LANE_CHORDS, 2);
    seq_rbar_lock(&s, LANE_CHORDS);   /* UNLOCK */
    seq_rbar_done(&s, LANE_CHORDS);
    seq_set_chord_len(&s, 32);   /* 2-bar chords: Em Em C C */
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 1, t), "x..x..x...x.x.x-") && !strcmp(plays_str(&s, LANE_CHORDS, 0, u), "-..x..x...x.x..."),
          "2-bar chords: no change at the inner bar line, its downbeat stays (%s; bar 1 %s)", t, u);
    seq_undo(&s);   /* 1-bar chords again: Em C G D */
    seq_cp_set(&s, 1, 10, 3, 0, 0);   /* a point: Am from bar 2's step 11 */
    c = seq_part_step_notes(&s, 0, LANE_CHORDS, 1, 8, n);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 1, t), "-..x..x.x--.x.x-") && pcs_in(n, c, (const int[]){9, 0, 4}, 3) &&
          seq_step_flags(&s, LANE_CHORDS, 1, 10) == SF_TIED, "into a change point: its hit an 8th early, playing the point's Am (%s)", t);
    seq_set_anticipate(&s, LANE_CHORDS, AN_BEAT);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 2, t), "x..x..x...x.x...") && !seq_step_flags(&s, LANE_CHORDS, 1, 10),
          "BEAT: a busy bar keeps its downbeat (step 13 sounds), and the point's hit (step 7 sounds) (%s)", t);
    seq_cp_clear(&s, 1, 10);
    lane_codes(&s, LANE_CHORDS, "x...x...x.......");
    c = seq_part_step_notes(&s, 0, LANE_CHORDS, 1, 12, n);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 1, t), "-...x...x...x---") && pcs_in(n, c, (const int[]){7, 11, 2}, 3) &&
          s.part[0].lane[LANE_CHORDS].steps[16 + 12].len == 5, "BEAT: 4 rests: the downbeat on step 13 of the bar before, held 5 (%s)", t);
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 1);   /* PICKUP: bar 4's fill variant hits steps 15 and 16 */
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 0, t), "x...x...x.....x-") && !seq_step_flags(&s, LANE_CHORDS, 0, 0) &&
          !strcmp(plays_str(&s, LANE_CHORDS, 1, u), "-...x...x.....x-"), "FILL on: bar 1's downbeat stays (bar 4's fill hits its last 8th); the rest move (%s, %s)", t, u);
    /* review fix: a FILL variant without the hit at the change (BREAK's silent half) is left as it is */
    lane_codes(&s, LANE_CHORDS, "x.......x.......");
    seq_cp_set(&s, 3, 8, 2, 0, 0);   /* G from bar 4's step 9 */
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 4);   /* BREAK: bar 4's fill variant x............... */
    rhythm_format(s.part[0].rfill[DL_CHORDS][3], u);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 3, t), "-.....x--.....x-") && !strcmp(u, "-.............x-"),
          "BREAK: bar 4's step 9 moves in the bar (%s), not into its fill variant, which keeps its silence (%s)", t, u);
    nev = 0;
    play(&s, 0.0, 16.0, 0);   /* bar 4 plays its fill (a loop of 4 bars) */
    CHECK(ch1_ons_at(0, 3 * 16 + 6, 64) == 0 && ch1_ons_at(0, 3 * 16 + 14, 64) == 3, "on the fill pass step 7 stays silent; step 15 plays bar 1's chord");
    seq_block(&s, 16.0, bpm, 0, 128);
    seq_cp_clear(&s, 3, 8);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 0);
    /* a LOCK WITH bar anticipates as it plays */
    lane_codes(&s, LANE_BASS, "x..x..x...x.x...");
    for (int b = 0; b < 4; b++) s.part[0].rb[DL_CHORDS][b].ed = 0;
    seq_set_lockwith(&s, LANE_CHORDS, LW_WITH);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 2, t), "-..x..x...x.x.x-") && !strcmp(codes_str(&s, LANE_CHORDS, 2, u), "x..x..x...x.x..."),
          "WITH the Bass: the Bass's codes, anticipated (%s, drawn %s)", t, u);
}

/* ANTICIPATE's early hit plays from the song position n steps before the change, once, also over a host loop's wrap */
static void test_24_anticipate_timing(void) {
    static seq_t s;
    int ok = 1, at, kept = 0, cut = 0;
    double ppq;
    nev = 0;
    seq_init(&s, emit, NULL);
    lane_codes(&s, LANE_CHORDS, "x..x..x...x.x...");
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    nev = 0;
    ppq = play(&s, 0.0, 32.0, 0);   /* two passes of the part, the plugin's own loop */
    seq_block(&s, ppq, bpm, 0, 128);
    for (int b = 0; b < 8; b++) ok &= ch1_ons_at(0, b * 16 + 14, 128) == 3 && (b == 0 || ch1_ons_at(0, b * 16, 128) == 0);
    CHECK(ok && ch1_ons_at(0, 0, 128) == 3 && balanced(), "each next downbeat sounds on the step 2 before it (k = 16b + 14), never on it; "
          "the start's tied downbeat plays its chord (Em) once");
    /* a 4-bar host loop: the early Em before the wrap keeps sounding into bar 1; bar 1's tied downbeat never strikes again */
    nev = 0;
    ppq = play(&s, 0.0, 48.0, 16.0);
    at = nev;
    ppq = play(&s, ppq, 1.0, 16.0);
    seq_block(&s, ppq, bpm, 0, 128);
    CHECK(ch1_ons_at(0, 62, 64) == 3 * 3 && ch1_ons_at(0, 0, 64) == 3, "3 passes: bar 4's step 15 each pass, bar 1's downbeat only at the start");
    for (int i = 0; i < at; i++)   /* the early Em before the 3rd wrap (its block ended at ppq 15.5..16) */
        if (is_off(&evs[i]) && (evs[i].status & 15) == 0 && evs[i].ppq < 1.0 && i > 0 && evs[i].k >= 0) {
            kept += evs[i].ppq > 0.1;
            cut += evs[i].ppq <= 0.05;
        }
    CHECK(kept >= 3 && !cut && balanced(), "over each wrap its notes end at their own length (after ppq 0.1), not at the jump; no hung note");
}

/* ruling 8: PUSH pushes a change point's hit as a slot boundary's; a real boundary a point overrides with the same chord is no change */
static void test_24_push_follows_points(void) {
    static seq_t s;
    cp_setup(&s);
    seq_set_chord_shape(&s, SHAPE_PUSH, 100);
    cp_hit(&s, LANE_CHORDS, 5, 8);
    nev = 0;
    play(&s, 0.0, 23.0, 0);
    CHECK(ons_near(0, 1, 21.875) == 0 && ons_near(0, 1, 22.0) == 3, "no point: bar 6's step 9 plays on time");
    seq_cp_set(&s, 5, 8, 2, 0, 0);
    nev = 0;
    play(&s, 0.0, 23.0, 0);
    CHECK(ons_near(0, 1, 21.875) == 3 && ons_near(0, 1, 22.0) == 0, "a point there: pushed 1/8 beat early, as a slot boundary");
    seq_cp_clear(&s, 5, 8);
    seq_set_chord_len(&s, 8);   /* 1/2-bar chords: bar 6 is slots 10 and 11, a boundary at step 9 */
    seq_cp_set(&s, 5, 4, 2, 0, 0);   /* a point from step 5: it overrides the boundary (G on through step 16) */
    cp_hit(&s, LANE_CHORDS, 5, 4);
    nev = 0;
    play(&s, 0.0, 23.0, 0);
    CHECK(ons_near(0, 1, 20.875) == 3 && ons_near(0, 1, 21.875) == 3 && ons_near(0, 1, 22.0) == 0,
          "the point's hit is pushed; final review (ruling 31): the point ends at slot 11's start (step 9), a real boundary, pushed too");
}

/* the save headroom with 0.24's fields: test_22_save_fits' case (both lanes jammed, every dj field, test_20's heavy edits) plus per part, in
 * each of `bars` bars, `pts` change points and `ho` hit octaves (what & 1), LOCK WITH and ANTICIPATE set (what & 2) */
static void heavy_24(seq_t *s, int bars, int pts, int ho, int what) {
    seq_init(s, emit, NULL);
    for (int part = 0; part < SEQ_NPARTS; part++) {
        heavy_part(s, part, 0, 4, 16);
        seq_jam_apply_now(s, LANE_CHORDS, 5, 4);
        seq_jam_apply_now(s, LANE_BASS, 5, 4);
        heavy_part(s, part, 8, 4, 16);
        for (int d = 0; d < DL_COUNT; d++)
            seq_djam_set(s, d, DJ_SW, 3), seq_djam_set(s, d, DJ_VAR, 3), seq_djam_set(s, d, DJ_SWING, 5), seq_djam_set(s, d, DJ_TIME, -1);
        for (int bar = 0; (what & 1) && bar < bars; bar++) {
            for (int k = 0; k < pts; k++) seq_cp_set(s, bar, 3 + 4 * k, (bar + k) % 7, k == 1 && pts == 4, k % 5);
            for (int i = 0; i < ho; i++) s->part[part].hoct[bar][i * SEQ_STEPS / ho] = (signed char)(i % 2 ? -2 : 2);
        }
        if (what & 2) {
            seq_set_lockwith(s, LANE_CHORDS, LW_GAPS);
            seq_set_anticipate(s, LANE_CHORDS, AN_BEAT), seq_set_anticipate(s, LANE_BASS, AN_16);
            seq_set_rknob(s, LANE_CHORDS, RK_CAD, 3), seq_set_rknob(s, LANE_CHORDS, RK_BREATH, 2);   /* 0.24 Task 4 (ruling 2): CADENCE, BREATH */
            seq_set_rknob(s, LANE_BASS, RK_CAD, 1), seq_set_rknob(s, LANE_BASS, RK_BREATH, 3);
        }
    }
    seq_select_part(s, 0);
}

static void test_24_save_fits(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX], big[4 * SEQ_STATE_MAX];
    static const char *const WHAT[4] = {"0.22's case alone", "+ 2 points and 4 hit octaves in 8 bars", "+ LOCK WITH, ANTICIPATE, CADENCE and BREATH", "+ both"};
    int ok = 1, n, all;
    for (int w = 0; w < 4; w++) {
        heavy_24(&s, 8, 2, 4, w);
        all = seq_state_save(&s, big, sizeof big);
        n = seq_state_save(&s, buf, sizeof buf);
        printf("     (no check) 6 parts, %s: %d bytes in all; in %d bytes: %d, unsaved parts 0x%x\n", WHAT[w], all, SEQ_STATE_MAX, n, seq_unsaved_parts(&s));
    }
    CHECK(n > 0 && n == all && !seq_unsaved_parts(&s) && strstr(buf, "|F1.la=2,3") && strstr(buf, "|F2.la=0,1") && strstr(buf, "|F.cp=") &&
              strstr(buf, "|F.ho=") && strstr(buf, "|F.rz=-2,4,4,4,3,2|") && strstr(buf, "|F2.rz=-2,4,4,4,1,3|"),
          "ruling 13 (0.24 Task 4: with CADENCE and BREATH): the realistic 0.24 case saves whole, no part left out: %d bytes of %d (headroom %d)", n,
          SEQ_STATE_MAX, SEQ_STATE_MAX - n);
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf), "and it loads");
    for (int part = 0; part < SEQ_NPARTS; part++) {
        ok &= !memcmp(t.part[part].cp, s.part[part].cp, sizeof s.part[part].cp) && !memcmp(t.part[part].hoct, s.part[part].hoct, sizeof s.part[part].hoct) &&
              !memcmp(t.part[part].lw, s.part[part].lw, sizeof s.part[part].lw) && !memcmp(t.part[part].an, s.part[part].an, sizeof s.part[part].an) &&
              !memcmp(t.part[part].sflag, s.part[part].sflag, sizeof s.part[part].sflag) &&
              !memcmp(t.part[part].rk, s.part[part].rk, sizeof s.part[part].rk);
    }
    CHECK(ok, "every part comes back: its points, hit octaves, LOCK WITH, ANTICIPATE, step flags and RHYTHM knobs (CADENCE, BREATH)");
    heavy_24(&s, 16, 4, 16, 3);
    all = seq_state_save(&s, big, sizeof big);
    n = seq_state_save(&s, buf, sizeof buf);
    printf("     (no check) the worst case: 4 points and 16 hit octaves in all 16 bars of all 6 parts on 0.22's: %d bytes in all; in %d bytes: %d, "
           "unsaved parts 0x%x\n", all, SEQ_STATE_MAX, n, seq_unsaved_parts(&s));
}

/* 0.24 Task 4: BREATH, CADENCE and BASS FILL on the Chords and Bass lanes, and the order in a bar. A new instance: E minor, Em C G D at
 * 1-bar chords, 4 bars; Pop Root on the Bass (D2 = 38 under D, E1 = 28 under Em); Chords on channel 1, Bass on 2 */

/* the top note's pitch class of the Chords' step as it plays (seq_part_step_notes), -1 none */
static int top_pc(const seq_t *s, int bar, int step) {
    int v[THEORY_CHORD_MAX], c = seq_part_step_notes(s, s->cur_part, LANE_CHORDS, bar, step, v);
    return c > 0 ? v[c - 1] % 12 : -1;
}

static void test_24_breath(void) {
    static seq_t s;
    static const char *const WANT[4] = {"x.x.x.x.x.x.x.x.", "x.x.x.x.x.x.x...", "x.x.x.x.x.x.....", "x.x.x.x........."};
    char t[17];
    int ok = 1, n0;
    seq_part_t *p;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    lane_codes(&s, LANE_CHORDS, "x.x.x.x.x.x.x.x.");
    lane_codes(&s, LANE_BASS, "x-------x-------");
    n0 = s.undo_n;
    seq_set_rknob(&s, LANE_CHORDS, RK_BREATH, 9);
    CHECK(seq_rknob(&s, LANE_CHORDS, RK_BREATH) == 3 && s.undo_n == n0 && !seq_rknob(&s, LANE_BASS, RK_BREATH),
          "BREATH clamps to 0..3 (Half), a knob turn: no undo step; the Bass's its own");
    seq_set_rknob(&s, LANE_CHORDS, RK_BREATH, -1);
    CHECK(!seq_rknob(&s, LANE_CHORDS, RK_BREATH), "  and to 0 (None)");
    for (int v = 0; v < 4; v++) {
        seq_set_rknob(&s, LANE_CHORDS, RK_BREATH, v);
        ok &= !strcmp(plays_str(&s, LANE_CHORDS, 3, t), WANT[v]);
        for (int b = 0; b < 3; b++) ok &= !strcmp(plays_str(&s, LANE_CHORDS, b, t), WANT[0]);
    }
    CHECK(ok, "None, 1/8, Beat, Half: bar 4's last 0, 2, 4, 8 steps rest (the phrase's last bar); bars 1-3 as drawn");
    seq_set_rknob(&s, LANE_BASS, RK_BREATH, 2);
    CHECK(!strcmp(plays_str(&s, LANE_BASS, 3, t), "x-------x---....") && p->lane[LANE_BASS].steps[3 * 16 + 8].len == 4 &&
              p->lane[LANE_BASS].steps[3 * 16 + 8].abs == 38 && !strcmp(plays_str(&s, LANE_BASS, 2, t), "x-------x-------"),
          "the Bass, Beat: bar 4's held note cut at the gap, its note kept (D2); bar 3 as drawn (%s)", plays_str(&s, LANE_BASS, 3, t));
    p->rb[DL_CHORDS][3].lock = 1;
    render_now(&s);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 3, t), WANT[0]), "a locked bar 4 plays as drawn: no breath");
    p->rb[DL_CHORDS][3].lock = 0;
    seq_set_bars(&s, 8);
    lane_codes(&s, LANE_CHORDS, "x.x.x.x.x.x.x.x.");
    ok = 1;
    for (int b = 0; b < 8; b++) ok &= !strcmp(plays_str(&s, LANE_CHORDS, b, t), WANT[b % 4 == 3 ? 3 : 0]);
    CHECK(ok, "8 bars: bars 4 and 8 breathe, each phrase's last");
    seq_set_bars(&s, 2);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 1, t), WANT[3]) && !strcmp(plays_str(&s, LANE_CHORDS, 0, t), WANT[0]), "2 bars: the loop's last bar, bar 2");
    seq_set_bars(&s, 1);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 0, t), WANT[3]), "1 bar: that bar");
}

/* the saved `.rz=`: CADENCE and BREATH after the 0.18 four, only when one is set (a 0.18-0.23 lane saves its 4 values as before); 4 values
 * load with them 0, 6 load all, anything else the defaults */
static void test_24_rz_save(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    seq_init(&s, emit, NULL);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 2);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A.rz=0,0,0,2|") && !strstr(buf, "A2.rz"), "FILL alone: 4 values, as 0.23 saved it");
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 2), seq_set_rknob(&s, LANE_BASS, RK_BREATH, 3);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(strstr(buf, "|A.rz=0,0,0,2,2,0|") && strstr(buf, "|A2.rz=0,0,0,0,0,3|"), "CADENCE or BREATH set: 6 values");
    seq_init(&t, emit, NULL);
    CHECK(seq_state_load(&t, buf) && t.part[0].rk[DL_CHORDS][RK_FILL] == 2 && t.part[0].rk[DL_CHORDS][RK_CAD] == 2 &&
              t.part[0].rk[DL_BASS][RK_BREATH] == 3 && !t.part[0].rk[DL_BASS][RK_CAD], "and loads back");
    CHECK(seq_state_load(&t, "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0526|A.voi=1|A1=0:|A1.g=17,1,50|A.rb=0,0,0,0|A.rz=1,2,3,4|A2=0:|A2.g=18,1,50|"
                             "A2.rb=0,0,0,0|A2.rz=0,0,0,1,9,-4|A3=0:|A4=0:") &&
              t.part[0].rk[DL_CHORDS][RK_SYNC] == 1 && t.part[0].rk[DL_CHORDS][RK_FILL] == 4 && !t.part[0].rk[DL_CHORDS][RK_CAD] &&
              !t.part[0].rk[DL_CHORDS][RK_BREATH] && t.part[0].rk[DL_BASS][RK_FILL] == 1 && t.part[0].rk[DL_BASS][RK_CAD] == 3 &&
              !t.part[0].rk[DL_BASS][RK_BREATH],
          "a 0.23 string (4 values) loads with CADENCE and BREATH 0; 6 values each clamped (CADENCE 3, BREATH 0)");
    CHECK(seq_state_load(&t, "csq2|gv=1|k=4|s=1|A.len=4|A.cl=16|A.h=30,1|A.prog=0526|A.voi=1|A1=0:|A1.g=17,1,50|A.rb=0,0,0,0|A.rz=1,2,3,4,1|A2=0:|"
                             "A2.g=18,1,50|A2.rb=0,0,0,0|A3=0:|A4=0:") && !t.part[0].rk[DL_CHORDS][RK_SYNC] && !t.part[0].rk[DL_CHORDS][RK_FILL],
          "5 values: ignored, the knobs 0 (never refused)");
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 0), seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 0), seq_set_rknob(&s, LANE_BASS, RK_BREATH, 0);
    seq_state_save(&s, buf, sizeof buf);
    CHECK(!strstr(buf, ".rz="), "all 0 again: no .rz");
    seq_set_rknob(&s, LANE_BASS, RK_CAD, 1), seq_set_rknob(&s, LANE_BASS, RK_SYNC, -2);
    seq_reset_rhythm_knobs(&s, LANE_BASS);
    CHECK(!seq_rknob(&s, LANE_BASS, RK_CAD) && !seq_rknob(&s, LANE_BASS, RK_SYNC), "RESET KNOBS sets CADENCE and BREATH to 0 too");
    seq_undo(&s);
    CHECK(seq_rknob(&s, LANE_BASS, RK_CAD) == 1 && seq_rknob(&s, LANE_BASS, RK_SYNC) == -2, "  and its UNDO puts them back");
    seq_set_rknob(&s, LANE_BASS, RK_SYNC, 0);
    seq_reset_rhythm_knobs(&s, LANE_BASS);
    CHECK(!seq_rknob(&s, LANE_BASS, RK_CAD), "  CADENCE alone is something to reset");
}

/* CADENCE on the Chords: the chord at the phrase's last bar's last hit (bar 4's D) voiced with its 5th / 3rd / root on top, the smallest
 * octave move of that tone; a play-time offset (as INVERSION's) */
static void test_24_cadence_chords_top(void) {
    static seq_t s;
    static const int D[3] = {2, 6, 9}, TOP[4] = {-1, 9, 6, 2};   /* OPEN A, SOFT F#, HOME D */
    int off[THEORY_CHORD_MAX], n[THEORY_CHORD_MAX], c0, c, ok = 1, x[8], k;
    seq_part_t *p;
    nev = 0;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    lane_codes(&s, LANE_CHORDS, "x...x...x...x...");
    c0 = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 12, off);
    for (int v = 1; v < 4; v++) {
        int moved;
        seq_set_rknob(&s, LANE_CHORDS, RK_CAD, v);
        c = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 12, n);
        moved = off[c0 - 1] % 12 != TOP[v];
        ok &= c == c0 && pcs_in(n, c, D, 3) && n[c - 1] % 12 == TOP[v] &&
              (moved ? n[c - 1] > off[c0 - 1] && n[c - 1] - off[c0 - 1] < 12 : !memcmp(n, off, sizeof(int) * (size_t)c));
        printf("     CADENCE %d: bar 4's last hit (D) top %d (was %d)\n", v, n[c - 1], off[c0 - 1]);
    }
    CHECK(ok, "bar 4's D at OPEN, SOFT, HOME: A, F#, D on top, the same chord tones, one tone moved the least octaves above the old top "
          "(none when it is on top already)");
    ok = 1;
    for (int v = 0; v < 4; v++) {
        int a[THEORY_CHORD_MAX], b[THEORY_CHORD_MAX], na, nb;
        seq_set_rknob(&s, LANE_CHORDS, RK_CAD, v);
        for (int bar = 0; bar < 4; bar++)
            for (int st = 0; st < 16; st += 4) {
                if (bar == 3 && st == 12) continue;
                na = seq_part_step_notes(&s, 0, LANE_CHORDS, bar, st, a);
                seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 0);
                nb = seq_part_step_notes(&s, 0, LANE_CHORDS, bar, st, b);
                seq_set_rknob(&s, LANE_CHORDS, RK_CAD, v);
                ok &= na == nb && !memcmp(a, b, sizeof(int) * (size_t)na);
            }
    }
    CHECK(ok, "every other hit as at OFF (bars 1-3, bar 4's first three)");
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 2);
    nev = 0;
    play(&s, 0.0, 16.0, 0);
    k = notes_at(1, 60, x);
    CHECK(k == c0 && x[k - 1] % 12 == 6, "played: bar 4's step 13 sounds F# on top (%d notes)", k);
    p->rb[DL_CHORDS][3].lock = 1;
    c = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 12, n);
    CHECK(c == c0 && !memcmp(n, off, sizeof(int) * (size_t)c), "a locked bar 4 keeps its voicing");
    p->rb[DL_CHORDS][3].lock = 0;
    seq_set_rknob(&s, LANE_CHORDS, RK_BREATH, 2);
    c = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 8, n);
    CHECK(c == c0 && n[c - 1] % 12 == 6, "BREATH Beat: the last hit is step 9's, voiced with F# on top");
}

/* CADENCE on the Bass: bar 4's last note (not a fill or fixed note) lands on the chord's 5th / 3rd / root, in the octave nearest it */
static void test_24_cadence_bass(void) {
    static seq_t s;
    static const int NOTE[4] = {38, 33, 42, 38};   /* D2; OPEN A1, SOFT F#2, HOME D2 */
    static const char ROLE[4] = {'1', '5', '3', '1'};
    unsigned char note[16];
    char role[16];
    int ok = 1;
    seq_part_t *p;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    lane_codes(&s, LANE_BASS, "x...x...x...x...");
    for (int v = 0; v < 4; v++) {
        seq_set_rknob(&s, LANE_BASS, RK_CAD, v);
        seq_bass_line(&s, 3, note, role);
        ok &= note[0] == 38 && note[4] == 38 && note[8] == 38 && note[12] == NOTE[v] && role[12] == ROLE[v];
        for (int b = 0; b < 3; b++) {
            seq_bass_line(&s, b, note, role);
            ok &= note[12] == note[0];
        }
    }
    CHECK(ok, "bar 4 (D) at OFF, OPEN, SOFT, HOME: its last note D2, A1, F#2, D2 (the nearest), roles 5 3 1; bars 1-3 as they were");
    p->bass.fix[3 * 16 + 12] = 45;
    render_now(&s);
    seq_set_rknob(&s, LANE_BASS, RK_CAD, 2);
    seq_bass_line(&s, 3, note, role);
    CHECK(note[12] == 45 && role[12] == 'f' && note[8] == 42 && role[8] == '3', "a fixed last note stays; the note before it lands (F#2)");
    p->bass.fix[3 * 16 + 12] = 0;
    p->bass.nlock = 1 << 3, p->bass.frozen[3] = p->bass.set;
    render_now(&s);
    seq_bass_line(&s, 3, note, role);
    CHECK(note[12] == 38, "a note-locked bar keeps its note");
    p->bass.nlock = 0;
    p->rb[DL_BASS][3].lock = 1;
    render_now(&s);
    seq_bass_line(&s, 3, note, role);
    CHECK(note[12] == 38, "a rhythm-locked bar too");
    p->rb[DL_BASS][3].lock = 0;
    seq_set_rknob(&s, LANE_BASS, RK_FILL, 3);
    seq_bass_line(&s, 3, note, role);
    CHECK(note[12] == 42 && p->rfill_abs[DL_BASS][3][12] == 21 && p->rfill_abs[DL_BASS][3][8] == 38,
          "FILL RUN: the cadence on the bar's own last note (F#2); the fill variant plays the run there (A0), not the cadence");
}

/* BASS FILL on a Root 8ths bar (bar 4, D) into Em (E1 28): PICKUP D#1 on step 16; STEP-IN C#1 D1 D#1 on 14-16; RUN A0 B0 C#1 D#1 on 13-16;
 * BREAK the second half silent. Played on bar 4 (FILL's bar) */
static void test_24_bass_fill(void) {
    static seq_t s;
    static const char *const CODES[5] = {"A.x.A.x.A.x.A.x.", "A.x.A.x.A.x.A.xx", "A.x.A.x.A.x.Axxx", "A.x.A.x.A.x.xxxx", "A.x.A.x........."};
    static const unsigned char TAIL[5][4] = {{38, 0, 38, 0}, {38, 0, 38, 27}, {38, 25, 26, 27}, {21, 23, 25, 27}, {0, 0, 0, 0}};
    char t[17];
    int ok = 1, x[8];
    seq_part_t *p;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    seq_rhythm_pick(&s, LANE_BASS, 3);   /* Eighths */
    for (int f = 0; f < 5; f++) {
        const unsigned char *fa = p->rfill_abs[DL_BASS][3];
        seq_set_rknob(&s, LANE_BASS, RK_FILL, f);
        rhythm_format(p->rfill[DL_BASS][3], t);
        ok &= !strcmp(t, CODES[f]) && !strcmp(plays_str(&s, LANE_BASS, 3, t), CODES[0]);
        for (int i = 0; i < 4; i++) ok &= !TAIL[f][i] || fa[12 + i] == TAIL[f][i];
        for (int i = 0; i < 12; i++) ok &= !rhythm_is_hit(p->rfill[DL_BASS][3][i]) || fa[i] == 38;
        if (!ok) printf("     FILL %d: %s, tail %d %d %d %d\n", f, t, fa[12], fa[13], fa[14], fa[15]);
    }
    CHECK(ok, "NONE, PICKUP, STEP-IN, RUN, BREAK: bar 4's fill variant and its notes; the bar's own hits D2; as it plays without FILL, unchanged");
    seq_set_rknob(&s, LANE_BASS, RK_FILL, 1);
    CHECK(p->rfill_abs[DL_BASS][0][15] == 35, "PICKUP into the next bar's first root: bar 1's (into C2) is B1");
    seq_set_rknob(&s, LANE_BASS, RK_FILL, 3);
    nev = 0;
    play(&s, 0.0, 16.0, 0);
    ok = notes_at(2, 56, x) == 1 && x[0] == 38;
    for (int i = 0; i < 4; i++) ok &= notes_at(2, 60 + i, x) == 1 && x[0] == TAIL[3][i];
    CHECK(ok && notes_at(2, 44, x) == 1 && x[0] == 31 && !notes_at(2, 45, x), "played: bar 4 runs A0 B0 C#1 D#1 into Em; bar 3 as drawn (G1)");
    CHECK(balanced(), "balanced");
}

/* The order in a bar: drawn -> JAM (TIME) -> COMPLEXITY -> SYNC -> GHOSTS -> BREATH -> CADENCE -> FILL -> ANTICIPATE -> notes -> output */
static void test_24_bar_order(void) {
    static seq_t s;
    char t[17], u[17];
    unsigned char note[16];
    char role[16];
    int a[THEORY_CHORD_MAX], b[THEORY_CHORD_MAX], na, nb, ok;
    seq_part_t *p;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    lane_codes(&s, LANE_CHORDS, "x.......x.......");
    seq_set_rknob(&s, LANE_CHORDS, RK_BREATH, 3);
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 4);
    plays_str(&s, LANE_CHORDS, 0, t);
    plays_str(&s, LANE_CHORDS, 3, u);
    CHECK(strchr(t + 9, 'g') && !strcmp(u + 8, "........"), "GHOSTS before BREATH: ghosts in bar 1's second half (%s), none in bar 4's gap (%s)", t, u);
    seq_set_rknob(&s, LANE_CHORDS, RK_GHOSTS, 0);
    seq_set_complexity(&s, LANE_CHORDS, 100);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 3, t), "xxxxxxxx........") && !strcmp(plays_str(&s, LANE_CHORDS, 0, t), "xxxxxxxxxxxxxxxx"),
          "COMPLEXITY before BREATH: every rest a hit but the gap (%s)", plays_str(&s, LANE_CHORDS, 3, t));
    seq_set_complexity(&s, LANE_CHORDS, 50);
    seq_djam_set(&s, LANE_CHORDS, DJ_TIME, 1);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 3, t), "x...x...........") && !strcmp(plays_str(&s, LANE_CHORDS, 0, t), "x...x...x...x..."),
          "JAM's TIME (DOUBLE) before BREATH (%s)", plays_str(&s, LANE_CHORDS, 3, t));
    seq_djam_set(&s, LANE_CHORDS, DJ_TIME, 0);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 1);
    rhythm_format(p->rfill[DL_CHORDS][3], t);
    CHECK(!strcmp(t, "x.............xg") && !strcmp(plays_str(&s, LANE_CHORDS, 3, u), "x..............."),
          "BREATH before FILL: PICKUP plays in the gap (%s; without FILL %s)", t, u);
    seq_set_rknob(&s, LANE_CHORDS, RK_FILL, 0), seq_set_rknob(&s, LANE_CHORDS, RK_BREATH, 0);
    /* the Bass: BREATH, then CADENCE on the last note before the gap, then FILL's run in it */
    lane_codes(&s, LANE_BASS, "x...x...x...x...");
    seq_set_rknob(&s, LANE_BASS, RK_BREATH, 3), seq_set_rknob(&s, LANE_BASS, RK_CAD, 2), seq_set_rknob(&s, LANE_BASS, RK_FILL, 3);
    seq_bass_line(&s, 3, note, role);
    rhythm_format(p->rfill[DL_BASS][3], t);
    CHECK(note[0] == 38 && note[4] == 42 && role[4] == '3' && !note[8] && !strcmp(t, "x...x.......xxxx") && p->rfill_abs[DL_BASS][3][4] == 42 &&
              p->rfill_abs[DL_BASS][3][12] == 21 && p->rfill_abs[DL_BASS][3][15] == 27,
          "the Bass: BREATH Half, CADENCE SOFT on step 5 (F#2), FILL RUN in the gap (%s)", t);
    seq_set_rknob(&s, LANE_BASS, RK_BREATH, 0), seq_set_rknob(&s, LANE_BASS, RK_CAD, 0), seq_set_rknob(&s, LANE_BASS, RK_FILL, 0);
    /* ANTICIPATE after BREATH and CADENCE: bar 1's downbeat moves into bar 4's gap playing Em, the cadence stays on bar 4's own last hit */
    lane_codes(&s, LANE_CHORDS, "x-------x-------");
    seq_set_rknob(&s, LANE_CHORDS, RK_BREATH, 2);
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    nb = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 14, b);
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 3);
    na = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 14, a);
    CHECK(!strcmp(plays_str(&s, LANE_CHORDS, 3, t), "x-------x---..x-") && seq_step_flags(&s, LANE_CHORDS, 3, 14) == (SF_EARLY | SF_ADDED) &&
              !strcmp(plays_str(&s, LANE_CHORDS, 1, t), "x-------x-------") && pcs_in(a, na, (const int[]){4, 7, 11}, 3) && na == nb &&
              !memcmp(a, b, sizeof(int) * (size_t)na) && top_pc(&s, 3, 8) == 2,
          "BREATH then ANTICIPATE: bar 1's downbeat on bar 4's step 15 (Em, no cadence); CADENCE HOME on step 9's D (%s)", plays_str(&s, LANE_CHORDS, 3, t));
    /* the notes after CADENCE: a hit octave moves the whole chord, its top still D */
    na = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 8, a);
    p->hoct[3][8] = 1;
    render_now(&s);
    nb = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 8, b);
    CHECK(na == nb && b[nb - 1] == a[na - 1] + 12 && top_pc(&s, 3, 8) == 2, "a hit octave +1 on the cadence hit: D on top, an octave up");
    p->hoct[3][8] = 0;
    /* a locked bar skips them all (it plays as drawn, its voicing its own) */
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 0);
    na = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 8, a);
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 2);
    p->rb[DL_CHORDS][3].lock = 1;
    render_now(&s);
    nb = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 8, b);
    ok = !strcmp(plays_str(&s, LANE_CHORDS, 3, t), "x-------x-------") && na == nb && !memcmp(a, b, sizeof(int) * (size_t)na);
    CHECK(ok, "a locked bar 4: no BREATH, no CADENCE, nothing anticipated into it (%s)", plays_str(&s, LANE_CHORDS, 3, t));
}

/* Fix round 1 (ruling 17): ANTICIPATE moving the cadence hit keeps the cadence. One hit a bar on Em C G D: bar 4's D moves to bar 3's step 15
 * (8TH) and plays the D there with F# on top (SOFT); on the Bass the early note is the cadence note (F#1 nearest bar 3's G1), a fixed note
 * still wins; the other early hits play their plain chord / root */
static void test_24_cadence_anticipated(void) {
    static seq_t s;
    static const int D[3] = {2, 6, 9};
    int n[THEORY_CHORD_MAX], a[THEORY_CHORD_MAX], c, na, x[8], k;
    unsigned char note[16];
    char role[16];
    seq_part_t *p;
    seq_init(&s, emit, NULL);
    p = &s.part[0];
    lane_codes(&s, LANE_CHORDS, "x...............");
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 2);
    CHECK(top_pc(&s, 3, 0) == 6, "SOFT, no ANTICIPATE: bar 4's D has F# on top");
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    c = seq_part_step_notes(&s, 0, LANE_CHORDS, 2, 14, n);
    CHECK(seq_step_flags(&s, LANE_CHORDS, 2, 14) == (SF_EARLY | SF_ADDED) && seq_step_flags(&s, LANE_CHORDS, 3, 0) == SF_TIED && pcs_in(n, c, D, 3) &&
              n[c - 1] % 12 == 6, "8TH: bar 4's D on bar 3's step 15, F# still on top (top %d)", n[c - 1]);
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 0);
    na = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 14, a);
    seq_set_rknob(&s, LANE_CHORDS, RK_CAD, 2);
    c = seq_part_step_notes(&s, 0, LANE_CHORDS, 3, 14, n);
    CHECK(c == na && !memcmp(n, a, sizeof(int) * (size_t)c), "bar 4's own early hit (bar 1's Em) plays as at OFF");
    nev = 0;
    play(&s, 0.0, 16.0, 0);
    k = notes_at(1, 46, x);
    CHECK(k == c && x[k - 1] % 12 == 6, "played: song step 47 sounds the D with F# on top");
    /* the Bass: G1 on bar 3, so the early hit's note is the one nearest it */
    lane_codes(&s, LANE_BASS, "x...............");
    seq_set_rknob(&s, LANE_BASS, RK_CAD, 2);
    seq_set_anticipate(&s, LANE_BASS, AN_8);
    seq_bass_line(&s, 2, note, role);
    CHECK(note[14] == 30 && role[14] == '3', "the Bass: the early hit lands the cadence, F#1 (role 3; %d %c)", note[14], role[14]);
    seq_bass_line(&s, 3, note, role);
    CHECK(note[14] == 40 && role[14] == '1', "bar 1's moved downbeat on bar 4's step 15: Em's root nearest D2, E2 (no cadence: %d)", note[14]);
    seq_set_rknob(&s, LANE_BASS, RK_CAD, 3);
    seq_bass_line(&s, 2, note, role);
    CHECK(note[14] == 26 && role[14] == '1', "HOME: D1, the root nearest");
    p->bass.fix[3 * 16] = 45;
    render_now(&s);
    seq_bass_line(&s, 2, note, role);
    CHECK(note[14] == 45 && role[14] == 'f', "a fixed note on the moved hit wins (A2)");
}

/* Review Focus: old projects load and play as the 0.23 build did. OLD22 is a 0.22-format chunk (two parts, both drawn lanes jammed, every
 * RHYTHM knob, painted, locked and fixed bars, a NEW BAR), as the 0.23 build (base 3ffd891) saved it after a load; PIN the 0.23 build's
 * numbers for it and each genre preset (presets_data.h): the FNV-1a of the chunk it saved after the load and its length, of the rendered
 * steps (every part, all lanes, 8 bars: the steps, their notes, the fill variants) and of 32 beats of MIDI from bar 1 (seq_block). Made by
 * a harness compiled against 3ffd891's sources in gcc:12, the same sums as below (Task 4 report) */
static const char OLD22[] =
    "csq2|gv=1|k=4|s=1|A.len=8|A.cl=16|A.h=30,1|A.prog=0526|A.voi=1|A.vx=-1,2,0,0,0,0|A.cs=0,50,0,50,0,50,1|ch=1,2,3|A1=0:|"
    "A1.g=17,1,70|A.rb=75,75,A.g-x..-Axg.x-..,75,75,L75,75,75|A.rz=2,1,3,3|A1.dj=2,1,1,2,3,0|A2=0:|A2.g=18,1,35|A2.bv=00000"
    "002000000000000000000000000|A2.ab=1428|A2.bx=36,0,0,3,1,0,0|A2.rb=81,x.x.x-..A...x.x.,81,81,81,81,81,81|A2.rz=1,2,0,4|"
    "A2.dj=3,2,1,0,0,1|A3=0:|A3.g=19,1,10|A4=0:|B.len=4|B.cl=16|B.h=30,1|B.prog=0526|B.voi=0|B.vx=-1,0,0,0,1,0|B.cs=0,50,0,"
    "50,50,50,0|B1=0:|B1.g=17,1,70|B.rb=84,84,84,84|B.rz=2,1,3,1|B1.dj=4,0,1,2,3,0|B2=0:|B2.g=18,1,35|B2.bv=000000020000000"
    "00000000000000000|B2.ab=1428|B2.bx=-1,0,0,2,0,0,-1|B2.rb=72,72,72,72|B2.rz=1,2,2,4|B2.dj=1,3,1,0,0,1|B3=0:|B3.g=19,1,1"
    "0|B4=0:";
static const struct { unsigned save, steps, play; int len; } PIN24[1 + PRESETS_COUNT] = {
    {0x8bccbbe8u, 0x5cdf696du, 0xa37acb3bu, 715},  {0x40abda46u, 0xa87273f9u, 0x05d824d1u, 1569},
    {0xe170d488u, 0x2dd21925u, 0x4cf0444au, 1543}, {0x4fdeb40cu, 0x5b894725u, 0x4eab678du, 1577},
    {0x6a86ab03u, 0xc47ebc54u, 0x530f4d93u, 1499}, {0xfe90eeefu, 0x898383c5u, 0xad5757aeu, 1624},
    {0x42715ccfu, 0x39fb54cdu, 0x0b8e6fccu, 1563}};

static uint32_t pin_h;
static int pin_rec, pin_blk;
static void pin_mix(int v) {
    for (int i = 0; i < 4; i++) pin_h = (pin_h ^ (uint32_t)((v >> (8 * i)) & 0xff)) * 16777619u;
}
static void pin_emit(void *ctx, int status, int note, int vel, int frame) {
    (void)ctx; (void)frame;
    if (pin_rec) pin_mix(status), pin_mix(note), pin_mix(vel), pin_mix(pin_blk);
}
static uint32_t pin_text(const char *c) {
    uint32_t x = 2166136261u;
    for (; *c; c++) x = (x ^ (uint32_t)(unsigned char)*c) * 16777619u;
    return x;
}
static uint32_t pin_steps(const seq_t *s) {
    pin_h = 2166136261u;
    for (int p = 0; p < SEQ_NPARTS; p++) {
        const seq_part_t *q = &s->part[p];
        int bars = q->bars < 8 ? q->bars : 8;
        for (int l = 0; l < SEQ_LANES; l++)
            for (int k = 0; k < bars * SEQ_STEPS; k++) {
                const seq_step_t *x = &q->lane[l].steps[k];
                int out[THEORY_CHORD_MAX], n;
                pin_mix(x->on), pin_mix(x->len), pin_mix(x->vel), pin_mix(x->tone), pin_mix(x->oct), pin_mix(x->abs);
                if (l < SEQ_ACTIVE_LANES) {
                    n = seq_part_step_notes(s, p, l, k / SEQ_STEPS, k % SEQ_STEPS, out);
                    pin_mix(n);
                    for (int i = 0; i < n; i++) pin_mix(out[i]);
                }
            }
        for (int d = 0; d < 2; d++)
            for (int b = 0; b < bars; b++)
                for (int i = 0; i < 16; i++) pin_mix(q->rfill[d][b][i]), pin_mix(q->rfill_abs[d][b][i]);
    }
    return pin_h;
}
static uint32_t pin_play(seq_t *s) {
    double ppq = 0, step = 128 * 120.0 / (60.0 * 44100.0);
    pin_h = 2166136261u;
    pin_rec = 1;
    for (pin_blk = 0; ppq < 32.0; pin_blk++, ppq += step) seq_block(s, ppq, 120.0, 1, 128);
    seq_block(s, ppq, 120.0, 0, 128);
    pin_rec = 0;
    return pin_h;
}

/* 0.24 Task 7: what seq_rbar_chord and seq_rbar_type did (RBC_*), for PHRASE's message line: the slot's pick, a point, the sounding chord again
 * (heard), the point gone, a full bar (4 points), and nothing with no Chords bar open */
static void test_24_rbar_returns(void) {
    static seq_t s;
    int r[8];
    cp_setup(&s);   /* bar 6 (index 5): C */
    ph_draw(&s, LANE_CHORDS, 5, "x---x---x---x---");
    r[0] = seq_rbar_chord(&s, 2, 0);   /* no bar open */
    seq_rbar_open(&s, LANE_CHORDS, 5);
    r[1] = seq_rbar_chord(&s, 2, 0);   /* step 1, the slot's first hit: the slot */
    seq_rbar_select(&s, LANE_CHORDS, 4);
    r[2] = seq_rbar_chord(&s, 2, 0);   /* G sounds there already: heard */
    r[3] = seq_rbar_chord(&s, 3, 0);   /* Am from step 5: a point */
    r[4] = seq_rbar_chord(&s, 2, 0);   /* G again, the slot's: the point goes */
    r[5] = seq_rbar_type(&s, 2);       /* sus4 on G at step 5: a point */
    r[6] = seq_rbar_type(&s, 2);       /* again: heard */
    seq_rbar_select(&s, LANE_CHORDS, 8), seq_rbar_chord(&s, 4, 0);
    seq_rbar_select(&s, LANE_CHORDS, 12), seq_rbar_chord(&s, 5, 0);
    seq_rbar_select(&s, LANE_CHORDS, 14), seq_rbar_chord(&s, 1, 0);
    seq_rbar_select(&s, LANE_CHORDS, 15);
    r[7] = seq_rbar_chord(&s, 0, 0);   /* a fifth point */
    CHECK(r[0] == RBC_NONE && r[1] == RBC_SLOT && r[2] == RBC_HEARD && r[3] == RBC_POINT && r[4] == RBC_GONE && r[5] == RBC_POINT &&
          r[6] == RBC_HEARD && r[7] == RBC_FULL && seq_cp_count(&s, 5) == 4 && seq_rbar_type(&s, 3) == RBC_FULL,
          "seq_rbar_chord / seq_rbar_type return none %d, slot %d, heard %d, point %d, gone %d, type point %d, heard %d, full %d (4 points)",
          r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    seq_rbar_done(&s, LANE_CHORDS);
}

/* 0.24 Task 7 (Task 2's carried item): seq_bar_picked takes a lane: a bar locked on that lane reads LOCKED, not picked (on the Bass its note lock
 * alone too); another lane's lock doesn't hide it */
static void test_24_bar_picked_lane(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    cp_setup(&s);
    seq_rbar_open(&s, LANE_BASS, 5);
    seq_rbar_lock(&s, LANE_BASS);   /* the Bass bar locked (rhythm and notes); the slot that starts in it picked */
    seq_rbar_done(&s, LANE_BASS);
    CHECK((p->slot_lock >> 5 & 1) && !seq_bar_picked(&s, LANE_BASS, 5) && seq_bar_picked(&s, LANE_CHORDS, 5) && !seq_bar_picked(&s, LANE_MEL_UP, 5),
          "a Bass-locked bar: picked on the CHORDS strip, LOCKED (not picked) on the BASS strip; another lane reads 0");
    p->rb[DL_BASS][5].lock = 0;   /* the note lock alone (a 0.19 project's) */
    CHECK((p->bass.nlock >> 5 & 1) && !seq_bar_picked(&s, LANE_BASS, 5), "a note lock alone hides it on the BASS strip too");
}

/* 0.24 Task 7 (ruling 18): the CHORDS tab's centre is a bar too (seq_centre_bar): at 2-bar chords a status-row tap on a chord's second bar centres
 * that bar (the slot stays the chord's); FOLLOW off a bar given by hand; a strip turn's pause starts on the bar shown; an open bar is the
 * centre; while FOLLOW runs, the playing bar */
static void test_24_centre_bar(void) {
    static seq_t s;
    double ppq = 0;
    cp_setup(&s);
    seq_set_chord_len(&s, 32);   /* 2-bar chords: bars 5-6 are slot 2 */
    seq_set_cview(&s, CVIEW_RHYTHM);   /* PHRASE: the strip pages by bars, 2 pages */
    seq_strip_show_bar(&s, TAB_CHORDS, 5);
    CHECK(seq_centre_bar(&s, TAB_CHORDS) == 5 && seq_centre_slot(&s, TAB_CHORDS) == 2 && seq_follow_paused(&s, TAB_CHORDS) == 2,
          "show bar 6 (FOLLOW on): the centre bar is 6 (%d), the centre slot the chord's (%d), paused by paging", seq_centre_bar(&s, TAB_CHORDS),
          seq_centre_slot(&s, TAB_CHORDS));
    seq_back_to_playing(&s, TAB_CHORDS);
    while (seq_cur_bar(&s) < 3 || seq_cur_step(&s) < 0) seq_block(&s, ppq, 120.0, 1, 128), ppq += 128 * 2.0 / 44100;
    CHECK(seq_centre_bar(&s, TAB_CHORDS) == 3, "running: the playing bar (4)");
    seq_strip_turn(&s, TAB_CHORDS, 1);
    seq_block(&s, ppq + 16.0, 120.0, 1, 128);   /* 4 bars on: the pause holds the bar it started on */
    CHECK(seq_follow_paused(&s, TAB_CHORDS) == 2 && seq_centre_bar(&s, TAB_CHORDS) == 3, "a turn pauses on the bar shown (4): %d",
          seq_centre_bar(&s, TAB_CHORDS));
    seq_block(&s, ppq + 16.0, 120.0, 0, 128);
    seq_rbar_open(&s, LANE_CHORDS, 5);
    CHECK(seq_centre_bar(&s, TAB_CHORDS) == 5, "an open bar is the centre (6)");
    seq_rbar_done(&s, LANE_CHORDS);
    seq_set_follow(&s, 0);
    seq_set_centre_bar(&s, TAB_CHORDS, 7);
    CHECK(seq_centre_bar(&s, TAB_CHORDS) == 7 && seq_centre_slot(&s, TAB_CHORDS) == 3, "FOLLOW off: bar 8 by hand, its chord's slot 4");
    seq_set_centre(&s, TAB_CHORDS, 1);
    CHECK(seq_centre_bar(&s, TAB_CHORDS) == 2, "a slot by hand: its first bar (3)");
    seq_set_centre_bar(&s, TAB_CHORDS, 8);
    CHECK(seq_centre_bar(&s, TAB_CHORDS) == 2, "a bar past the loop is ignored");
    seq_set_bars(&s, 4);
    CHECK(seq_centre_bar(&s, TAB_CHORDS) <= 3, "a shorter loop clamps it (%d)", seq_centre_bar(&s, TAB_CHORDS));
    seq_set_centre_bar(&s, TAB_BASS, 2);
    CHECK(seq_centre_bar(&s, TAB_BASS) == 2 && seq_centre_slot(&s, TAB_BASS) == 2, "BASS: the bar centre is its centre");
}

static void test_24_load_old_unchanged(void) {
    static seq_t t;
    static char out[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    for (int i = -1; i < PRESETS_COUNT; i++) {
        const char *x = i < 0 ? OLD22 : PRESET_CHUNK[i];
        uint32_t sv, st, pl;
        int ok, n;
        char what[48];
        seq_init(&t, pin_emit, NULL);
        ok = seq_state_load(&t, x);
        n = seq_state_save(&t, out, sizeof out);
        sv = pin_text(out);
        st = pin_steps(&t);
        pl = pin_play(&t);
        seq_init(&t, pin_emit, NULL);
        ok &= seq_state_load(&t, out) && seq_state_save(&t, again, sizeof again) == n && !strcmp(again, out);
        snprintf(what, sizeof what, i < 0 ? "the 0.22 chunk (its round trip byte for byte)" : "genre preset %d", i + 1);
        CHECK(ok && sv == PIN24[i + 1].save && n == PIN24[i + 1].len && st == PIN24[i + 1].steps && pl == PIN24[i + 1].play && (i >= 0 || !strcmp(out, x)),
              "%s: loads, saves as 0.23 did (0x%08x, %d bytes) and again byte for byte; its steps (0x%08x) and 8 bars of MIDI (0x%08x) as "
              "0.23's", what, sv, n, st, pl);
    }
}

/* Moved from tests/test_ideas.c when IDEAS left the code: a part switch and a state load change display_rev */
static void test_display_rev_part_switch(void) {
    static seq_t s;
    seq_init(&s, emit, NULL);
    seq_block(&s, 0, 120, 1, 128);
    seq_select_part(&s, 3);
    unsigned rev = s.display_rev;
    double q = 0;
    int held = 1;
    while (s.cur_part != 3 && q < 20) {
        held &= s.display_rev == rev;   /* every block before the switch */
        seq_block(&s, q += 0.05, 120, 1, 128);
    }
    CHECK(held && s.cur_part == 3 && s.display_rev != rev, "a bar-line part switch changes display_rev, nothing before it (%u -> %u)",
          rev, s.display_rev);
    rev = s.display_rev;
    seq_block(&s, q += 0.05, 120, 1, 128);
    CHECK(s.display_rev == rev, "and only the switch does");
    seq_state_load(&s, "csq2|k=4|s=1|A.len=4|A.cl=16|A.prog=0526|A1=0:|A2=0:|A3=0:|A4=0:");
    CHECK(s.display_rev != rev, "a state load changes display_rev");
    rev = s.display_rev;
    seq_state_load(&s, "csq2|bad");
    CHECK(s.display_rev == rev, "a refused load does not");
}

/* The Bass lane plays the generator's steps at every unlocked step: regenerated here on a copy */
static int bass_follows_gen(const seq_t *s) {
    static seq_t x;
    memcpy(&x, s, sizeof x);
    seq_part_t *p = &x.part[x.cur_part];
    seq_lane_t *ln = &p->lane[LANE_BASS];
    if (ln->gen.style == GEN_MANUAL) return 1;
    int c = ln->gen.complexity;
    ln->gen.complexity = c == 100 ? 99 : c + 1;
    seq_set_complexity(&x, LANE_BASS, c);   /* regen: the generator at unlocked steps, locked ones kept */
    return !memcmp(ln->steps, s->part[s->cur_part].lane[LANE_BASS].steps, sizeof(seq_step_t) * (size_t)(p->bars * SEQ_STEPS));
}

static unsigned rw_next(unsigned *r) { *r = *r * 1103515245u + 12345u; return *r >> 8; }

static void random_op(seq_t *s, unsigned *r, char *sv) {
    int op = (int)(rw_next(r) % 16), v = (int)(rw_next(r) % 101);
    static const int STY[4] = {GEN_BASS_ROOT, GEN_BASS_PUMP, GEN_BASS_WALK, GEN_MANUAL};
    switch (op) {
    case 0: seq_set_key(s, v % 12); break;
    case 1: seq_set_scale(s, v % SCALE_COUNT); break;
    case 2: seq_generate_progression(s); break;
    case 3: seq_set_slot(s, v % seq_prog_len(s), v % 7, v % 4 == 0 ? 1 + v % 3 : 0, v % COL_COUNT); break;
    case 4: seq_set_vknob(s, VK_COL, v % 6 - 1); break;
    case 5: seq_set_complexity(s, LANE_BASS, v); break;
    case 6: seq_set_lane_shape(s, LANE_BASS, (int)(rw_next(r) % BSHAPE_COUNT), v % 2 ? v : v % 5 * 25); break;
    case 7: seq_set_style(s, LANE_BASS, STY[v % 7 < 6 ? v % 3 : 3]); break;
    case 8: seq_generate(s, LANE_BASS); break;
    case 9: seq_generate_all(s); break;
    case 10: seq_undo(s); break;
    case 11: seq_state_save(s, sv, SEQ_STATE_MAX); seq_state_load(s, sv); break;
    case 12: seq_set_step_note(s, LANE_BASS, 0, v % 16, v % 3 ? 30 + v % 30 : 0); break;
    case 13: seq_toggle_step_lock(s, LANE_BASS, 0, v % 16); break;
    case 14: seq_select_part(s, v % 2 ? 3 : 0); break;
    default: if (v % 2) seq_set_chord_len(s, 4 << (v % 4)); else seq_set_bars(s, 1 << (v % 3)); break;
    }
}

/* Fix round 2 (moved from tests/test_ideas.c): a shaped Walk (Chromatic approach, MOVEMENT) follows the COLOUR knob: its 7ths are
 * chord tones */
static void test_bass_follows_colour(void) {
    static seq_t s;
    int ok = 1, n = 0;
    for (int g = 0; g < 40; g++) {
        seq_init(&s, emit, NULL);
        seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
        seq_set_complexity(&s, LANE_BASS, 90);
        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_APPROACH, 1);
        seq_set_lane_shape(&s, LANE_BASS, BSHAPE_MOVEMENT, g % 2 ? 100 : 0);
        for (int k = 0; k < g; k++) seq_generate(&s, LANE_BASS);
        for (int c = -1; c <= 4; c++, n++) {
            seq_set_vknob(&s, VK_COL, c);
            ok &= bass_follows_gen(&s);
        }
    }
    CHECK(ok, "a shaped Walk regenerates with the COLOUR knob (%d turns)", n);
}

/* Fix round 2 (moved from tests/test_ideas.c): over random edits the Bass plays the generator's steps */
static void test_bass_random_walk(void) {
    static seq_t s;
    static char sv[SEQ_STATE_MAX];
    int steps_ok = 0, ops = 0;
    for (unsigned run = 0; run < 200; run++) {
        unsigned r = run * 2654435761u + 1;
        seq_init(&s, emit, NULL);
        seq_set_style(&s, LANE_BASS, GEN_BASS_WALK);
        for (int k = 0; k < 40; k++, ops++) {
            random_op(&s, &r, sv);
            steps_ok += bass_follows_gen(&s);
        }
    }
    CHECK(steps_ok == ops, "random edits: the Bass plays the generator's steps at every unlocked step (%d of %d)", steps_ok, ops);
}

/* 0.24.1 both drawn lanes of part 0 render as a state load renders them (drawn_now on each: render_rhythm, the follower twice): their steps,
 * fill variants, flags and roles, the voices and labels, the change points (rb's unsaved bytes aside) */
static int pair_as_loaded(seq_t *s) {
    static seq_t f;
    static char sv[SEQ_STATE_MAX];
    const seq_part_t *p = &s->part[0], *q = &f.part[0];
    int ok, n = p->filled * SEQ_STEPS;
    seq_init(&f, quiet, NULL);
    if (seq_state_save(s, sv, sizeof sv) <= 0 || !seq_state_load(&f, sv)) return 0;
    ok = q->filled == p->filled && !memcmp(p->sflag, q->sflag, sizeof p->sflag) && !memcmp(p->rfill, q->rfill, sizeof p->rfill) &&
         !memcmp(p->rfill_abs, q->rfill_abs, sizeof p->rfill_abs) && !memcmp(p->brole, q->brole, (size_t)n) && !memcmp(p->hoct, q->hoct, sizeof p->hoct) &&
         !memcmp(p->cp, q->cp, sizeof p->cp) && !memcmp(p->prog, q->prog, sizeof p->prog) && !memcmp(p->voice, q->voice, sizeof p->voice) &&
         !memcmp(p->voice_n, q->voice_n, sizeof p->voice_n) && !memcmp(p->slot_text, q->slot_text, sizeof p->slot_text) &&
         !memcmp(p->slot_num, q->slot_num, sizeof p->slot_num);
    for (int lane = LANE_CHORDS; lane <= LANE_BASS; lane++)
        ok &= !memcmp(p->lane[lane].steps, q->lane[lane].steps, sizeof(seq_step_t) * (size_t)n);
    return ok;
}

/* 0.24.1 the blocks queued on both drawn lanes, landing now, leave both lanes as the same taps applied at once do (jq_fresh for both, and
 * each lane following the other) */
static int jq_fresh_both(const seq_t *s) {
    static seq_t r, f;
    int c, l;
    if (!s->playing) return 0;
    r = *s, r.emit = quiet;
    f = *s, f.emit = quiet;
    seq_block(&r, 0, bpm, 0, 128);
    for (int lane = LANE_CHORDS; lane <= LANE_BASS; lane++)
        if (!seq_jam_queued(s, lane, &c, &l)) return 0;
        else seq_jam_apply_now(&f, lane, c, l);
    return lane_same(&r, &f, LANE_CHORDS) && lane_same(&r, &f, LANE_BASS) && !memcmp(r.part[0].sflag, f.part[0].sflag, sizeof r.part[0].sflag);
}

/* 0.24.1 (A) LOCK WITH with ANTICIPATE and a JAM block queued on each lane: the part-wide renders (a key: rekey; a scale, chord pick or change
 * point: regen_harmony; a chord length: regen_part) and a LOCK WITH bump render each drawn lane once and each queued block once, and leave
 * both lanes as a state load renders them and the queued blocks as the taps applied at once; the bump's undo (restore_lane) as a load too */
static void test_241_pair_render(void) {
    static seq_t s;
    static const char *const LEAD[2] = {"the Bass follows the Chords", "the Chords follow the Bass"};
    for (int lead = 0; lead < 2; lead++) {
        int lane = lead ? LANE_BASS : LANE_CHORDS, other = lead ? LANE_CHORDS : LANE_BASS, back;
        unsigned r0, miss = 0;
        for (int queued = 1; queued >= 0; queued--) {
            seq_init(&s, quiet, NULL);
            seq_set_bars(&s, 8);
            lane_codes(&s, LANE_BASS, "x..x..x...x.x...");
            lane_codes(&s, LANE_CHORDS, "x.....x...x.....");
            seq_set_anticipate(&s, LANE_CHORDS, AN_8), seq_set_anticipate(&s, LANE_BASS, AN_16);
            seq_set_lockwith(&s, other, LW_WITH);
            if (queued) {
                play_to_k(&s, 0, 21);
                seq_jam_queue(&s, LANE_CHORDS, 2, 3), seq_jam_queue(&s, LANE_BASS, 4, 2);
            }
            for (int e = queued ? 0 : 5; e < 6; e++) {
                r0 = s.djq_renders;
                if (e == 0) seq_set_key(&s, (seq_key(&s) + 5) % 12);
                else if (e == 1) seq_set_scale(&s, (seq_scale(&s) + 1) % 3);
                else if (e == 2) seq_cp_set(&s, 2, 8, 4, 0, 1);
                else if (e == 3) seq_set_slot(&s, 1, 5, 0, COL_TRIAD);
                else if (e == 4) seq_set_chord_len(&s, 8);
                else seq_set_lockwith(&s, lane, LW_GAPS);   /* the bump: the other lane OWN, this one follows it */
                miss |= (unsigned)!pair_as_loaded(&s) << (3 * e + 1);
                if (queued) miss |= (unsigned)(s.djq_renders != r0 + 2) << (3 * e) | (unsigned)!jq_fresh_both(&s) << (3 * e + 2);
            }
        }
        seq_undo(&s);   /* stopped, nothing queued: the bump's undo, restore_lane */
        back = seq_lockwith(&s, other) == LW_WITH && seq_lockwith(&s, lane) == LW_OWN && pair_as_loaded(&s);
        CHECK(!miss && back, "%s, ANTICIPATE, a block queued on each: key, scale, change point, chord pick, chord length and a LOCK WITH bump "
              "leave both lanes as a load renders them, each queued block rendered once (as the taps at once; misses 0x%x: 3 bits an edit); "
              "the bump's undo too (%d)", LEAD[lead], miss, back);
    }
}

/* 0.24.1 (C, D) an undo that carries the voicing (restore_prog: the revoice and the colour readers once), and a chord pick that leaves change
 * points playing their slot's chord (cp_prune: voiced once at the end) leave the part as a state load makes it */
static void test_241_prog_once(void) {
    static seq_t s;
    int ok;
    seq_init(&s, quiet, NULL);
    seq_set_bars(&s, 4);
    seq_set_lane_shape(&s, LANE_BASS, BSHAPE_MOVEMENT, 100);
    seq_set_vknob(&s, VK_COL, 2);
    seq_set_voicing_style(&s, 3);
    seq_set_vknob(&s, VK_INV, 2);
    seq_undo(&s);
    ok = pair_as_loaded(&s);
    seq_undo(&s);
    ok &= pair_as_loaded(&s);
    CHECK(ok, "an undo of a VOICING style (its snapshot carries the voicing) leaves the part as a load makes it");
    seq_cp_set(&s, 0, 4, 3, 0, 0), seq_cp_set(&s, 0, 8, 3, 0, 0), seq_cp_set(&s, 0, 12, 4, 0, 0);
    ok = seq_cp_count(&s, 0) == 3;
    seq_set_slot(&s, 0, 3, 0, COL_TRIAD);   /* slot 1 plays IV: bar 1's IV at step 5 goes, then the one at 9 (now after the slot); its V stays */
    CHECK(ok && seq_cp_count(&s, 0) == 1 && pair_as_loaded(&s), "a chord pick drops the two points that then play its chord, the V stays (%d); "
          "the voices as a load makes them", seq_cp_count(&s, 0));
}

/* 0.24.2 fix 1: NEW BAR on an anticipated Bass bar compares what the bar plays with what each variation would play, both after ANTICIPATE.
 * One hit a bar under a pedal (no variation moves it), ANTICIPATE 8TH: each downbeat plays on step 15 of the bar before */
static void test_0242_bass_new_bar_anticipated(void) {
    static seq_t s;
    unsigned char a[16], b[16];
    int n, r;
    nev = 0;
    seq_init(&s, emit, NULL);
    lane_codes(&s, LANE_BASS, "x...............");
    seq_set_bass_pedal(&s, 1);
    seq_rbar_open(&s, LANE_BASS, 1);
    n = s.undo_n;
    r = seq_rbar_new_bar(&s, LANE_BASS);
    CHECK(!r && s.undo_n == n, "ANTICIPATE OFF: one hit under a pedal, NEW BAR finds no other line");
    seq_rbar_done(&s, LANE_BASS);
    seq_set_anticipate(&s, LANE_BASS, AN_8);
    seq_rbar_open(&s, LANE_BASS, 1);
    bline(&s, 1, a);
    n = s.undo_n;
    r = seq_rbar_new_bar(&s, LANE_BASS);
    bline(&s, 1, b);
    CHECK(a[14] && !a[0] && !r && s.undo_n == n && !memcmp(a, b, 16) && !s.part[0].lane[LANE_BASS].bar_var[1],
          "ANTICIPATE 8TH (step 15 plays, step 1 held): still no other line, no undo step (r %d, undo +%d, v %d)", r, s.undo_n - n,
          s.part[0].lane[LANE_BASS].bar_var[1]);
    seq_rbar_done(&s, LANE_BASS);
}

/* 0.24.2 fix 2: at 2-bar chords a change point in a slot's first bar lasts to that bar's end, so step 1 of the slot's second bar changes
 * back to the slot's chord: a chord change for the PHRASE tick (seq_chord_change_at), ANTICIPATE and PUSH */
static void test_0242_change_back_2bar(void) {
    static seq_t s;
    seq_part_t *p = &s.part[0];
    char t[17];
    int x;
    cp_setup(&s);
    seq_set_chord_len(&s, 32);   /* 2-bar chords: bars 5 and 6 (index 4, 5) are slot 2 */
    lane_codes(&s, LANE_CHORDS, "x.......x.......");
    x = (p->prog[2] + 2) % 7;
    seq_cp_set(&s, 4, 8, x, 0, 0);   /* another chord from bar 5's step 9 */
    CHECK(ph_deg(&s, 4, 15) == x && ph_deg(&s, 5, 0) == p->prog[2] && seq_chord_change_at(&s, 5, 0) && !seq_chord_change_at(&s, 5, 8),
          "the point lasts to bar 5's end; bar 6's step 1 plays the slot's chord again: a change (%d)", seq_chord_change_at(&s, 5, 0));
    seq_set_chord_shape(&s, SHAPE_PUSH, 100);
    nev = 0;
    play(&s, 0.0, 21.0, 0);
    CHECK(ons_near(0, 1, 19.875) == 3 && ons_near(0, 1, 20.0) == 0, "PUSH pushes that change back 1/8 beat early (%d early, %d on time)",
          ons_near(0, 1, 19.875), ons_near(0, 1, 20.0));
    seq_block(&s, 21.0, bpm, 0, 128);
    seq_set_chord_shape(&s, SHAPE_PUSH, 0);
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    CHECK(seq_step_flags(&s, LANE_CHORDS, 5, 0) == SF_TIED && seq_step_flags(&s, LANE_CHORDS, 4, 14) == (SF_EARLY | SF_ADDED),
          "ANTICIPATE 8TH moves bar 6's downbeat to bar 5's step 15 (%s)", plays_str(&s, LANE_CHORDS, 4, t));
}

/* the pitch classes of the Chords' notes at a step, as a 12-bit mask */
static int pc_mask(const seq_t *s, int bar, int step) {
    int n[THEORY_CHORD_MAX], c = seq_part_step_notes(s, 0, LANE_CHORDS, bar, step, n), m = 0;
    for (int i = 0; i < c; i++) m |= 1 << ((n[i] % 12 + 12) % 12);
    return m;
}

/* 0.24.2 fix 3: THIS HIT ONLY's "back to the chord before" point carries that chord's colour, also one no TYPE picks (a 7th) */
static void test_0242_this_hit_only_colour(void) {
    static seq_t s, t;
    static char buf[SEQ_STATE_MAX];
    seq_part_t *p = &s.part[0];
    int r, v, ok = 1;
    cp_setup(&s);
    seq_set_slot(&s, 5, p->prog[5], 0, COL_7TH);   /* bar 6's slot picked as a 7th */
    ph_draw(&s, LANE_CHORDS, 5, "x---x---x---x-..");
    seq_cp_set(&s, 5, 4, 2, 0, 0);   /* G from step 5 */
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 4);
    r = seq_rbar_this_hit_only(&s);
    v = seq_slot_at(&s, 5, 8);
    CHECK(r == 1 && v >= SEQ_MAX_PROG && seq_slot_colour(&s, v) == COL_7TH && pc_mask(&s, 5, 8) == pc_mask(&s, 5, 0),
          "THIS HIT ONLY on G: step 9 plays the slot's 7th again (colour %d, notes %03x vs %03x)", seq_slot_colour(&s, v), pc_mask(&s, 5, 8),
          pc_mask(&s, 5, 0));
    seq_rbar_done(&s, LANE_CHORDS);
    seq_state_save(&s, buf, sizeof buf);
    seq_init(&t, emit, NULL);
    ok = seq_state_load(&t, buf) && !memcmp(t.part[0].cp, p->cp, sizeof p->cp);
    for (int i = 0; i < SEQ_STEPS; i++) ok &= pc_mask(&t, 5, i) == pc_mask(&s, 5, i);
    CHECK(ok, "saved and loaded: the same points, bar 6 plays the same");
    cp_setup(&s);
    seq_set_slot(&s, 5, p->prog[5], 0, COL_7TH);
    ph_draw(&s, LANE_CHORDS, 5, "x---x---x---x-..");
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 8);
    r = seq_rbar_type(&s, 0);
    v = seq_slot_at(&s, 5, 8);
    CHECK(r == RBC_POINT && v >= SEQ_MAX_PROG && seq_slot_colour(&s, v) == COL_TRIAD,
          "TYPE as knobs on a later hit of a 7th slot: a Knobs point there, not 'heard' (r %d)", r);
    seq_rbar_done(&s, LANE_CHORDS);
}

/* 0.24.2 fix 4: TYPE on a full bar's step that plays that type already is heard (as seq_rbar_chord), not refused as a full bar */
static void test_0242_rbar_type_full_heard(void) {
    static seq_t s;
    int r, n;
    cp_setup(&s);
    ph_draw(&s, LANE_CHORDS, 5, "x-x-x-x-x-x-x-x-");
    for (int k = 0; k < 4; k++) seq_cp_set(&s, 5, 2 + 2 * k, k % 2 ? 2 : 6, 0, 1);   /* 4 sus2 points: a full bar */
    seq_rbar_open(&s, LANE_CHORDS, 5);
    seq_rbar_select(&s, LANE_CHORDS, 10);   /* under the point on step 9 */
    n = s.undo_n;
    s.naud = 0;
    r = seq_rbar_type(&s, 1);
    CHECK(seq_cp_count(&s, 5) == 4 && r == RBC_HEARD && s.naud > 0 && s.undo_n == n, "a full bar, TYPE sus2 where sus2 plays: heard (r %d)", r);
    r = seq_rbar_type(&s, 2);
    CHECK(r == RBC_FULL && seq_cp_count(&s, 5) == 4 && s.undo_n == n, "another type there: a full bar (r %d)", r);
    seq_rbar_done(&s, LANE_CHORDS);
}

/* 0.24.2 fix 5: the fields a drawn lane's render writes (both drawn lanes' steps, flags, fill variants and notes, Bass roles, hit octaves) */
static int an_same(const seq_part_t *a, const seq_part_t *b, char *why) {
    if (memcmp(a->lane[LANE_CHORDS].steps, b->lane[LANE_CHORDS].steps, sizeof a->lane[0].steps)) return snprintf(why, RT_WHY, "Chords steps"), 0;
    if (memcmp(a->lane[LANE_BASS].steps, b->lane[LANE_BASS].steps, sizeof a->lane[0].steps)) return snprintf(why, RT_WHY, "Bass steps"), 0;
    if (memcmp(a->sflag, b->sflag, sizeof a->sflag)) return snprintf(why, RT_WHY, "flags"), 0;
    if (memcmp(a->rfill, b->rfill, sizeof a->rfill)) return snprintf(why, RT_WHY, "fill variants"), 0;
    if (memcmp(a->rfill_abs, b->rfill_abs, sizeof a->rfill_abs)) return snprintf(why, RT_WHY, "fill notes"), 0;
    if (memcmp(a->brole, b->brole, sizeof a->brole)) return snprintf(why, RT_WHY, "Bass roles"), 0;
    if (memcmp(a->hoct, b->hoct, sizeof a->hoct)) return snprintf(why, RT_WHY, "hit octaves"), 0;
    return 1;
}

/* a random edit for test_0242_anticipate_narrow_render: the ones that render a few bars (a bar's paint, fix, NEW BAR, lock, RESET BAR,
 * copy; COMPLEXITY, the Bass knobs) among ones that render the loop or change the harmony (COLOUR, a pick, a point) */
static void an_op(seq_t *s) {
    int bars = seq_bars(s), lane = rt_rnd(2) ? LANE_BASS : LANE_CHORDS;
    switch (rt_rnd(22)) {
    case 0: seq_rbar_open(s, lane, rt_rnd(bars)); seq_rbar_paint(s, lane, rt_rnd(16), rt_rnd(5)); break;
    case 1: seq_rbar_open(s, LANE_BASS, rt_rnd(bars)); seq_rbar_select(s, LANE_BASS, rt_rnd(16)); seq_rbar_note(s, rt_rnd(12)); break;
    case 2: seq_rbar_open(s, lane, rt_rnd(bars)); seq_rbar_new_bar(s, lane); break;
    case 3: seq_rbar_open(s, lane, rt_rnd(bars)); seq_rbar_lock(s, lane); break;
    case 4: seq_rbar_open(s, lane, rt_rnd(bars)); seq_rbar_reset_bar(s, lane); break;
    case 5: seq_rbar_open(s, lane, rt_rnd(bars)); seq_rbar_copy(s, lane, (unsigned)rt_rnd(65536) & ((1u << bars) - 1)); break;
    case 6: seq_set_complexity(s, lane, seq_complexity(s, lane) + rt_rnd(21) - 10); break;   /* small turns: often no bar's hits change */
    case 7: seq_set_complexity(s, lane, rt_rnd(101)); break;
    case 8: seq_set_bknob(s, rt_rnd(4), rt_rnd(7) - 2); break;
    case 9: seq_set_bass_octave(s, rt_rnd(3) - 1); break;
    case 10: seq_set_rknob(s, lane, rt_rnd(4), rt_rnd(7) - 2); break;
    case 11: seq_set_anticipate(s, lane, 1 + rt_rnd(3)); break;
    case 12: seq_cp_set(s, rt_rnd(bars), rt_rnd(16), rt_rnd(7), 0, rt_rnd(5)); break;
    case 13: seq_rbar_open(s, LANE_CHORDS, rt_rnd(bars)); seq_rbar_select(s, LANE_CHORDS, rt_rnd(16)); seq_rbar_hit_oct(s, rt_rnd(5) - 2); break;
    case 14: seq_set_rknob(s, lane, RK_CAD, rt_rnd(4)); break;
    case 15: seq_undo(s); break;
    case 16: seq_set_vknob(s, VK_COL, rt_rnd(6) - 1); break;
    case 17: seq_set_slot(s, rt_rnd(seq_slots(s)), rt_rnd(7), 0, rt_rnd(COL_COUNT)); break;
    case 18: seq_set_rknob(s, lane, RK_FILL, rt_rnd(5)); break;
    case 19: seq_set_rknob(s, lane, RK_BREATH, rt_rnd(4)); break;
    case 20: seq_set_bars(s, (int[]){1, 2, 4, 8, 16}[rt_rnd(5)]); break;
    case 21: seq_set_lockwith(s, lane, rt_rnd(3)); break;
    }
    seq_txn_done(s);
}

/* 0.24.2 fix 5: with ANTICIPATE on, a setter that renders a few bars leaves both drawn lanes as a render of every bar would, after each of
 * many random edits over many random states (bars, chord lengths, LOCK WITH, FILL; COMPLEXITY turns that change no bar's hits) */
static void test_0242_anticipate_narrow_render(void) {
    enum { RUNS = 120, OPS = 40 };
    static seq_t s, t;
    char why[RT_WHY] = "", first[RT_WHY + 48] = "";
    int bad = 0, checks = 0;
    for (int r = 0; r < RUNS; r++) {
        rt_seed = 4242u + (unsigned)r;
        nev = 0;
        seq_init(&s, emit, NULL);
        seq_set_bars(&s, (int[]){2, 4, 8, 16}[rt_rnd(4)]);
        seq_set_chord_len(&s, (int[]){4, 8, 16, 32}[rt_rnd(4)]);
        seq_set_anticipate(&s, LANE_CHORDS, 1 + rt_rnd(3));
        seq_set_anticipate(&s, LANE_BASS, 1 + rt_rnd(3));
        if (rt_rnd(3) == 0) seq_set_lockwith(&s, rt_rnd(2) ? LANE_BASS : LANE_CHORDS, LW_WITH);
        if (rt_rnd(2)) seq_set_rknob(&s, LANE_BASS, RK_FILL, rt_rnd(5));
        for (int i = 0; i < OPS; i++) {
            an_op(&s);
            t = s;
            render_now(&t);
            checks++;
            if (!an_same(&s.part[s.cur_part], &t.part[t.cur_part], why) && !bad++) snprintf(first, sizeof first, "; run %d op %d: %s", r, i, why);
        }
    }
    CHECK(!bad && checks == RUNS * OPS, "ANTICIPATE on: after %d random edits a render of every bar changes nothing (%d differ%s)", checks, bad, first);
}

/* 0.24.2 (fix 5's prerequisite): a COLOUR turn that makes or removes a chord change re-renders a lane that anticipates it. Em C G D with
 * slot 1 picked as C maj7: at COLOUR 7th bar 2's C (Knobs) is the same chord, at Triad a change */
static void test_0242_colour_turn_anticipates(void) {
    static seq_t s, t;
    char why[RT_WHY] = "", a[17];
    seq_part_t *p = &s.part[0];
    nev = 0;
    seq_init(&s, emit, NULL);
    lane_codes(&s, LANE_CHORDS, "x..x..x...x.x...");
    lane_codes(&s, LANE_BASS, "x..x..x...x.x...");
    seq_set_vknob(&s, VK_COL, 1);
    seq_set_slot(&s, 0, p->prog[1], 0, COL_7TH);
    seq_set_anticipate(&s, LANE_CHORDS, AN_8);
    seq_set_anticipate(&s, LANE_BASS, AN_8);
    CHECK(!seq_chord_change_at(&s, 1, 0) && !seq_step_flags(&s, LANE_CHORDS, 0, 14), "COLOUR 7th: no change at bar 2, nothing anticipated");
    seq_set_vknob(&s, VK_COL, 0);
    t = s;
    render_now(&t);
    CHECK(seq_chord_change_at(&s, 1, 0) && seq_step_flags(&s, LANE_CHORDS, 0, 14) == (SF_EARLY | SF_ADDED) && an_same(p, &t.part[0], why),
          "COLOUR Triad: a change at bar 2, both lanes anticipate it at once (bar 1 %s; vs a render of every bar: %s)",
          plays_str(&s, LANE_CHORDS, 0, a), why[0] ? why : "same");
}

/* 0.24.2 fix 5: with ANTICIPATE on, a setter that changes a few bars renders those and their neighbours only (the pass couples a bar with
 * the bars beside it), and none when it changes none; what it leaves is what a render of every bar makes. A step of a bar no render should
 * reach is poisoned (its velocity), checked, then put back */
static void test_0242_anticipate_renders_near(void) {
    static seq_t s, t;
    seq_part_t *p = &s.part[0];
    char why[RT_WHY] = "";
    seq_step_t *x = &p->lane[LANE_BASS].steps[4 * SEQ_STEPS + 3], *y = &p->lane[LANE_BASS].steps[0];
    int v, ok;
    nev = 0;
    seq_init(&s, emit, NULL);
    seq_set_bars(&s, 8);
    lane_codes(&s, LANE_BASS, "x..x..x...x.x...");
    seq_set_anticipate(&s, LANE_BASS, AN_8);
    for (int b = 0; b < 8; b++) p->bass.frozen[b] = p->bass.set;
    p->bass.nlock = 0xfc;   /* bars 3-8 note-locked: a Bass knob renders bars 1 and 2 (and 8, 3 beside them) */
    render_now(&s);
    v = x->vel, x->vel = 1;
    seq_set_bass_octave(&s, 1);
    ok = x->vel == 1;
    x->vel = (unsigned char)v;
    t = s;
    render_now(&t);
    CHECK(ok && an_same(p, &t.part[0], why), "a Bass knob over bars 1-2: bar 5 not rendered (vel %d), the loop as a render of every bar makes it (%s)",
          x->vel, why[0] ? why : "same");
    p->bass.nlock = 0xff;   /* every bar note-locked: the knob changes none */
    render_now(&s);
    v = y->vel, y->vel = 1;
    seq_set_bass_octave(&s, 0);
    ok = y->vel == 1;
    y->vel = (unsigned char)v;
    t = s;
    render_now(&t);
    CHECK(ok && an_same(p, &t.part[0], why), "every bar note-locked (no bar to render): nothing rendered, the loop as a render of every bar makes it (%s)",
          why[0] ? why : "same");
}

int main(void) {
    test_theory();
    test_new_scales();
    test_theory_04();
    test_melody2_retired();
    test_default_pattern();
    test_loops();
    test_start_mid_bar();
    test_tempo_jitter();
    test_mute_and_panic();
    test_labels_and_state();
    test_per_bar_edit();
    test_long_loop();
    test_grow_copies();
    test_shrink_grow_keeps_edits();
    test_chord_len();
    test_shrink_while_playing();
    test_default_smooth();
    test_wrap_voicing();
    test_voicing_golden();
    test_key_scale_prog();
    test_eight_chords_quarter_bar();
    test_unreached_slots_blank();
    test_song_change_while_playing();
    test_loads_keep_close();
    test_setters_rebuild_voicing();
    test_state_v2();
    test_state_voicing();
    test_state_v1();
    test_state_invalid();
    test_note_length();
    test_gen_default();
    test_gen_styles();
    test_gen_develop();
    test_gen_locks();
    test_gen_grow_locks();
    test_manual_tap_one_step();
    test_gen_undo();
    test_new_idea_one_undo();
    test_gen_undo_across_grow();
    test_gen_undo_keeps_complexity();
    test_manual_gen_defaults();
    test_gen_harmony();
    test_prog_preset_undo();
    test_gen_regen_on_change();
    test_gen_selective_regen();
    test_gen_noop_setters();
    test_gen_key_transposes();
    test_gen_regen_time();
    test_state_gen_round_trip();
    test_chord_rhythms();
    test_progression_borrow();
    test_state_gen_size();
    test_state_04();
    test_state_gen_refused();
    test_state_unknown_style();
    test_state_gen_lenient();
    test_state_shrunk_locks();
    test_state_golden();
    test_gen_cache_differential();
    test_state_mel_round_trip();
    test_state_mel_size();
    test_mel_setter_time();
    test_channels();
    test_channel_state();
    test_channel_change_releases();
    test_neutral_output_is_06();
    test_swing_delays_odd_steps_once();
    test_swing_lowered_mid_step();
    test_gate_and_dynamics();
    test_follow();
    test_state_output_stage();
    test_part_switch_at_bar_line();
    test_switch_then_loop_wrap();
    test_part_cancel_and_stopped_select();
    test_part_first_use_copies_verse();
    test_part_used_mask();
    test_mutes_per_part();
    test_lazy_regen_on_key_change();
    test_undo_across_parts();
    test_state_parts_round_trip();
    test_state_07_is_verse();
    test_state_bad_part_dropped();
    test_state_overflow();
    test_save_while_chorus_plays();
    test_switch_view_and_grid();
    test_09_neutral_is_08();
    test_09_triads_voice_as_08();
    test_09_borrowed_in_every_lane();
    test_09_colours();
    test_09_shape_chords_lane();
    test_09_push();
    test_09_strum();
    test_09_state();
    test_09_slots_locks_undo();
    test_09_literal_08_state();
    test_10_lane_shape_neutral();
    test_10_bass_register();
    test_10_bass_gate_accent_octave();
    test_10_push_both_lanes();
    test_10_push_fuzz();
    test_10_bs_state();
    test_10_transforms_neutral();
    test_10_bass_movement();
    test_10_bass_sync();
    test_10_bass_octaves();
    test_10_bass_transform_order();
    test_10_walk_chromatic();
    test_10_abs_playback();
    test_10_approach_register();
    test_10_locks_survive();
    test_10_fix_setters();
    test_10_abs_state();
    test_11_literal_010_state();
    test_11_types();
    test_11_setters_undo();
    test_11_borrowed();
    test_11_locks();
    test_11_state();
    test_11_gate();
    test_11_harmony_follows();
    test_11_classic_knob();
    test_12_follow();
    test_12_fix_follows();
    test_12_fix_bar();
    test_12_audition();
    test_12_move_text();
    test_12_new_rhythm_same();
    test_13_new_rhythm_changes();
    test_13_new_rhythm_manual();
    test_13_new_rhythm_walk();
    test_13_new_rhythm_locked();
    test_13_variant_012_load();
    test_14_slot_count();
    test_14_old_states();
    test_14_locked_try();
    test_14_per_bar_edit();
    test_14_remap();
    test_14_grow_shrink();
    test_14_locks64();
    test_14_gen64();
    test_14_set_progression();
    test_14_colour_rank();
    test_13_variant_save();
    test_13_new_rhythm_variant();
    test_13_new_rhythm_undo();
    test_15_init_preset();
    test_vst12_phrasing_state();
    test_16_start_bar1();
    test_16_notes_per_bar();
    test_17_defaults_sound_as_016();
    test_17_spread();
    test_17_inversion();
    test_17_strum_down();
    test_17_six_notes_strum();
    test_17_colour_levels();
    test_17_knob_api();
    test_17_vmodes();
    test_17_melody_reads_colours();
    test_17_vx_roundtrip();
    test_17_vx_bad();
    test_17_old_state_loads();
    test_17_styles_table();
    test_17_default_style();
    test_17_style_apply_edited();
    test_17_style_strum_octave();
    test_171_reset_knobs();
    test_17_style_undo();
    test_17_style_saves();
    test_17_knobs_lock();
    test_17_cancel_on_full_ring();
    test_17_prog_try_keeps_locks();
    test_17_voice_try_cancel();
    test_17_browser_closed_by_switch();
    test_17_transactions_exclusive();
    test_17_stale_cancel();
    test_17_save_hides_tries();
    test_17_init_style();
    test_17_resend_and_bad_load();
    test_17_strip_follows();
    test_17_turn_pauses();
    test_17_txn_pause_keeps_paging();
    test_17_page_after_shrink();
    test_17_follow_off_pages_by_hand();
    test_17_keys_latch();
    test_17_keys_fold();
    test_17_keys_mute_clears();
    test_17_pause_ends();
    test_17_edits_close_txn();
    test_17_cancel_keeps_other_steps();
    test_17_lock_at_triad();
    test_17_prog_step_keeps_locks();
    test_18_init_sustain();
    test_18_render_follows_knobs();
    test_18_new_idea_keeps_bars();
    test_18_play_tap();
    test_18_loop_grow_repeats();
    test_18_reset_knobs();
    test_18_cx_harmony();
    test_18_low_interval();
    test_18_drift_rises();
    test_18_cx0_quarter_chords();
    test_18_cx_cpu_rule();
    test_18_old_lane_unmoved();
    test_18_rhythm_reset_renders();
    test_18_rknob_set();
    test_18_reset_undo_keeps_rk();
    test_18_fill_bars();
    test_18_humanize();
    test_18_save_round_trip();
    test_18_full_save_fits();
    test_18_old_styles_convert();
    test_18_bad_rb();
    test_18_v1_chords_drawn();
    test_18_paint_rules();
    test_18_copy();
    test_18_cancel_restores_copies();
    test_18_rknob_no_undo();
    test_18_locked_bar_no_fill();
    test_18_pick_targets();
    test_18_browser();
    test_18_rbar_closed_by_switch();
    test_18_rbar_kept_by_view();
    test_18_rbar_not_saved();
    test_18_rbar_shrink();
    test_18_follow_pause();
    test_18_core_closes_bar();
    test_18_strip_by_bars();
    test_19_bass_rb_independent();
    test_19_txn_cross_tab();
    test_19_init_bass();
    test_19_render_style();
    test_19_span_split();
    test_19_bass_centre_bars();
    test_19_play_abs();
    test_19_bass_tap();
    test_19_fill_notes();
    test_19_fix_survives_cut();
    test_19_gen_leaves_bass();
    test_19_harmony_rerender_basic();
    test_19_bknobs();
    test_19_style_apply();
    test_19_style_browser();
    test_19_bedit();
    test_19_bar_on_phrase();
    test_19_bass_txn_closed_by_switch();
    test_19_harmony_rerender();
    test_19_reset_bass_knobs();
    test_19_bass_rhythm_rules();
    test_19_knob_turns_kept();
    test_19_save_round_trip();
    test_19_random_round_trip();
    test_19_full_save_fits();
    test_19_old_bass_converts();
    test_19_bad_fields();
    test_20_save_round_trip();
    test_20_save_fits();
    test_20_old_project_loads();
    test_20_bad_fields();
    test_21_settings();
    test_21_undo_keeps_swing_acc();
    test_21_save_round_trip();
    test_21_time_load_3char();
    test_21_lock_keeps_new_fields();
    test_21_load_020_chunk();
    test_21_bad_fields();
    test_21_queued_gets_knobs();
    test_21_queued_grid16();
    test_21_time_weak();
    test_21_queued_weak();
    test_21_accent();
    test_21_swing_16();
    test_21_swing_8();
    test_21_swing_8_cap();
    test_21_swing_global();
    test_21_swing_edges();
    test_20_tab_mel_strip();
    test_20_txn_mel_safe();
    test_20_init_mel();
    test_20_render_matches_model();
    test_20_short_loops();
    test_20_harmony_rerender();
    test_20_gen_leaves_mel();
    test_20_push_dyn();
    test_20_tap_fix();
    test_20_switch_points();
    test_20_switch_push();
    test_20_switch_cancel_replace_stopped();
    test_20_final_review();
    test_20_switch_at_wrap_and_part();
    test_20_queue_follows_edits();
    test_20_switch_short_loop();
    test_20_switch_audio_safe();
    test_20_settings();
    test_20_swap_picker();
    test_20_medit();
    test_20_fix_edge_cases();
    test_20_mel_txn_closed_by_switch();
    test_20_cx3_bands();
    test_20_jam_guards();
    test_22_jam_index();
    test_22_apply();
    test_22_locks_and_undo();
    test_22_bass_rhythm_lock_autolocks();
    test_22_chords_locked_takes_voicing();
    test_22_edited();
    test_22_time_var_render();
    test_22_save_round_trip();
    test_22_load_021_chunk();
    test_22_save_fits();
    test_22_queue_switch_at();
    test_22_three_queues();
    test_22_queue_cancel_undo();
    test_22_queue_follows_knobs();
    test_22_queue_dropped();
    test_22_swing_grid();
    test_22_swing_hum_strum();
    test_22_swing_queue_grid();
    test_22_weak();
    test_191_dlane_load_same();
    test_191_snap_centre_bar();
    test_191_style_undo_as_cancel();
    test_191_editor_closes();
    test_191_hostile_strings();
    test_23_preset_field();
    test_24_cp_slot_at();
    test_24_cp_lanes_follow();
    test_24_cp_every_bar_quarter();
    test_24_cp_chord_len_and_loop();
    test_24_cp_kept();
    test_24_cp_save_round_trip();
    test_24_phrase_select_paint();
    test_24_phrase_chord_rules();
    test_24_phrase_hit_oct();
    test_24_this_hit_only();
    test_24_cp_slot_boundary();
    test_24_cp_prune();
    test_24_hoct_drawn_only();
    test_24_phrase_bass_note();
    test_24_newbar_chords();
    test_24_reset_bar();
    test_24_lock_combined();
    test_24_copy_modes();
    test_24_phrase_undo_cancel_mix();
    test_24_cp_undo();
    test_24_phrase_cross_lane_hoct();
    test_24_copy_note_lock();
    test_24_lockwith();
    test_24_lockwith_conflict();
    test_24_anticipate();
    test_24_anticipate_edges();
    test_24_anticipate_timing();
    test_24_push_follows_points();
    test_24_save_fits();
    test_24_breath();
    test_24_rz_save();
    test_24_cadence_chords_top();
    test_24_cadence_bass();
    test_24_bass_fill();
    test_24_bar_order();
    test_24_cadence_anticipated();
    test_24_load_old_unchanged();
    test_24_rbar_returns();
    test_24_bar_picked_lane();
    test_24_centre_bar();
    test_display_rev_part_switch();
    test_bass_follows_colour();
    test_bass_random_walk();
    test_241_pair_render();
    test_241_prog_once();
    test_0242_bass_new_bar_anticipated();
    test_0242_change_back_2bar();
    test_0242_this_hit_only_colour();
    test_0242_rbar_type_full_heard();
    test_0242_colour_turn_anticipates();
    test_0242_anticipate_narrow_render();
    test_0242_anticipate_renders_near();
    printf(fails ? "\nFAILED: %d\n" : "\nPASSED\n", fails);
    return fails != 0;
}
