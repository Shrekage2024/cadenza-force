/* Offline test of src/bassline.c (run by tests/run_tests.sh under ASan/UBSan). Exit 1 on failure.
 * The sweep and the spot lines are the canvas model in tests/ref/bass_ref.js (run it to see the same numbers). */
#include "../src/bassline.h"
#include "../src/rhythm.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

#define BASS_SWEEP_FNV 0xc46384cbu

static const bass_key_t EMIN = {4, {4, 6, 7, 9, 11, 0, 2}};
static const bass_chord_t CH[4] = {{4, 7, 1}, {0, 4, 0}, {7, 11, 0}, {2, 6, 0}};   /* Em C G D */

static unsigned fnv(unsigned h, int byte) { h ^= (unsigned)byte & 255u; return h * 16777619u; }

/* One bar of the loop on a 16-step span; returns the bar score. */
static int bar_line(const char *rs, int b, bass_set_t set, int v, const unsigned char *fix, unsigned char note[16], char role[16]) {
    unsigned char codes[16];
    const bass_chord_t *of[16];
    rhythm_parse(rs, codes);
    bass_span_in_t in = {codes, 16, &CH[b % 4], &CH[(b + 1) % 4], &EMIN, set, v, fix};
    bass_span(&in, note, role);
    for (int i = 0; i < 16; i++) of[i] = &CH[b % 4];
    return bass_score(note, 16, of, &EMIN);
}

static void test_19_styles(void) {
    int uniq = 1, feel_ok = 1, genre_ok = 1, edited_ok = 1;
    for (int i = 0; i < BASS_STYLE_COUNT; i++) {
        uniq &= bass_style_name(i) != NULL && bass_style_name(i)[0] != 0;
        feel_ok &= bass_style_feel(i) != NULL && bass_style_feel(i)[0] != 0;
        for (int j = 0; j < i; j++) uniq &= strcmp(bass_style_name(i), bass_style_name(j)) != 0;
        genre_ok &= bass_style_genre(i) == i / 4;
        bass_set_t s = bass_style_set(i);
        edited_ok &= s.style == i && !bass_set_edited(&s);
    }
    CHECK(uniq, "19: 48 style names, unique");
    CHECK(feel_ok, "19: every style has a feel");
    CHECK(genre_ok, "19: bass_style_genre is i / 4");
    CHECK(edited_ok, "19: a style's own set is not edited");
    CHECK(strcmp(bass_style_name(0), "House Root") == 0 && strcmp(bass_style_name(47), "Long Roots") == 0, "19: first and last names");
    CHECK(strcmp(bass_genre_name(0), "House") == 0 && strcmp(bass_genre_name(11), "Film / Ambient") == 0, "19: genre names");
    bass_set_t s = bass_style_set(1);
    CHECK(s.pedal == 0 && s.octave == 0 && s.k[BK_MOV] == 0 && s.k[BK_OCTS] == 2 && s.k[BK_APP] == 0 && s.k[BK_REG] == 0, "19: Octave Bounce");
    s = bass_style_set(9);
    CHECK(s.pedal == 1 && s.k[BK_REG] == -1, "19: Pedal Drone has pedal 1 and reg -1");
    s = bass_style_set(45);
    CHECK(s.pedal == 2, "19: Dominant Pedal has pedal 2");
    s = bass_style_set(1);
    s.k[BK_OCTS] = 3;
    CHECK(bass_set_edited(&s) == 1, "19: a changed knob is edited");
    s = bass_style_set(1);
    s.pedal = 1;
    CHECK(bass_set_edited(&s) == 1, "19: a changed pedal is edited");
    s = bass_style_set(1);
    s.octave = 1;
    CHECK(bass_set_edited(&s) == 1, "19: a changed octave is edited");
    s.style = -1;
    CHECK(bass_set_edited(&s) == 0, "19: style -1 is never edited");
}

static void test_19_sweep_matches_canvas(void) {
    static const char *RH[4] = {"A.x.A.x.A.x.A.x.", "A---------------", "x...x...x...x...", "A..xx.x.A..x.xx."};
    unsigned h = 2166136261u;
    int n = 0;
    for (int r = 0; r < 4; r++) for (int b = 0; b < 4; b++) for (int pedal = 0; pedal < 3; pedal++)
    for (int mov = 0; mov < 5; mov++) for (int oct = 0; oct < 5; oct++) for (int app = 0; app < 5; app++)
    for (int reg = -2; reg <= 2; reg++) for (int octave = 0; octave <= 1; octave++) for (int v = 0; v < 3; v++) {
        bass_set_t set = {-1, (signed char)pedal, (signed char)octave, {(signed char)mov, (signed char)oct, (signed char)app, (signed char)reg}};
        unsigned char note[16];
        char role[16];
        int score = bar_line(RH[r], b, set, v, NULL, note, role);
        for (int i = 0; i < 16; i++) h = fnv(h, note[i]);
        for (int i = 0; i < 16; i++) h = fnv(h, role[i]);
        h = fnv(h, score);
        n++;
    }
    CHECK(n == 180000, "19: sweep cases %d", n);
    CHECK(h == BASS_SWEEP_FNV, "19: sweep checksum 0x%08x matches the canvas 0x%08x", h, BASS_SWEEP_FNV);
}

static int notes_are(const unsigned char *note, const int *want, int n) {
    for (int i = 0; i < n; i++) if (note[i] != want[i]) return 0;
    return 1;
}

static void test_19_spots(void) {
    unsigned char note[16];
    char role[17];
    bass_set_t s = {-1, 0, 0, {0, 0, 0, 0}};
    int sc;
    s.k[BK_OCTS] = 2;
    sc = bar_line("A.x.A.x.A.x.A.x.", 0, s, 0, NULL, note, role); role[16] = 0;
    CHECK(strcmp(role, "1.o.1.o.1.o.1.o.") == 0 && note[0] == 28 && note[2] == 40 && note[14] == 40 && note[1] == 0 && sc == 0,
          "19: octave bounce, bar 0: %s", role);
    s = (bass_set_t){-1, 0, 0, {4, 0, 4, 0}};
    sc = bar_line("x...x...x...x...", 1, s, 0, NULL, note, role); role[16] = 0;
    CHECK(strcmp(role, "1...3...c...c...") == 0 && note[0] == 36 && note[4] == 28 && note[8] == 32 && note[12] == 30 && sc == 63,
          "19: walk + surround, bar 1: %s score %d", role, sc);
    s = (bass_set_t){-1, 2, 0, {0, 0, 0, 0}};
    sc = bar_line("A---------------", 1, s, 0, NULL, note, role); role[16] = 0;
    CHECK(strcmp(role, "P---------------") == 0 && note[0] == 35 && sc == 48, "19: dominant pedal, bar 1: %s score %d", role, sc);
    s = (bass_set_t){-1, 1, 0, {0, 0, 0, 0}};
    sc = bar_line("A---------------", 3, s, 0, NULL, note, role); role[16] = 0;
    CHECK(strcmp(role, "P---------------") == 0 && note[0] == 28 && sc == 30, "19: tonic pedal, bar 3: %s score %d", role, sc);
    unsigned char fix[16] = {0};
    fix[10] = 31;
    s = (bass_set_t){-1, 0, 0, {0, 2, 0, 0}};
    sc = bar_line("A.x.A.x.A.x.A.x.", 1, s, 1, fix, note, role); role[16] = 0;
    {
        const int want[16] = {36, 0, 36, 0, 36, 0, 48, 0, 36, 0, 31, 0, 36, 0, 48, 0};
        CHECK(strcmp(role, "1.1.1.o.1.f.1.o.") == 0 && notes_are(note, want, 16) && sc == 0, "19: fixed step 10, bar 1: %s", role);
    }
}

static void test_19_spans(void) {
    unsigned char codes[16], note[16];
    char role[16];
    bass_set_t s = {-1, 0, 0, {0, 0, 2, 0}};   /* APPROACH: half below */
    bass_span_in_t in = {codes, 8, &CH[0], &CH[1], &EMIN, s, 0, NULL};
    rhythm_parse("A.x.A.x.........", codes);
    bass_span(&in, note, role);
    CHECK(role[6] == 'c' && note[6] == 35 && role[4] == '1', "19: a half-bar span approaches its last hit (step 6 >= 5)");
    in.span = 4;
    bass_span(&in, note, role);
    CHECK(role[2] == '1' && note[2] == 28, "19: a quarter-bar span's step 2 is too early for an approach");
    in.span = 8;
    in.next = &CH[0];
    bass_span(&in, note, role);
    CHECK(role[6] == '1' && note[6] == 28, "19: no approach when the next chord has the same root");
    s.k[BK_APP] = 4;   /* surround: the hit before the last must be at step >= 4 of 8 */
    in.set = s;
    in.next = &CH[1];
    bass_span(&in, note, role);
    CHECK(role[6] == 'c' && note[6] == 35 && role[4] == 'c' && note[4] == 37,
          "19: surround takes the hit before the last at step >= 4 of 8");
}

static void test_19_scale_steps(void) {
    CHECK(bass_scale_below(&EMIN, 4) == 2 && bass_scale_above(&EMIN, 4) == 6, "19: E minor: below(4)=2, above(4)=6");
    CHECK(bass_scale_below(&EMIN, 0) == 11, "19: below(0)=11");
    CHECK(bass_scale_below(&EMIN, 8) == 7 && bass_scale_above(&EMIN, 8) == 9, "19: borrowed G#: below 7, above 9");
}

static void test_19_extremes(void) {
    static const bass_chord_t F = {5, 9, 0};
    unsigned char codes[16], note[16], fix[16] = {0};
    char role[16];
    int ok = 1, ped_ok = 1;
    rhythm_parse("A.x.A.x.A.x.A.x.", codes);
    fix[4] = 127;
    fix[8] = 200;
    bass_set_t s = {-1, 2, 1, {4, 4, 4, 2}};
    bass_span_in_t in = {codes, 16, &F, &CH[0], &EMIN, s, 2, fix};
    bass_span(&in, note, role);
    for (int i = 0; i < 16; i++) {
        if (codes[i] == RC_REST) { ok &= note[i] == 0; continue; }
        ok &= note[i] >= 1 && note[i] <= 127;
        if (i != 4 && i != 8) ped_ok &= note[i] % 12 == 11;
    }
    CHECK(ok, "19: extremes: every note in 1..127");
    CHECK(ped_ok, "19: extremes: the pedal's pitch class is 11");
    CHECK(note[4] == 127 && role[4] == 'f' && note[8] == 127, "19: extremes: fixed notes clamp to 127");
    CHECK(bass_tension(11, &F, &EMIN) == 80, "19: B over F is 80 (iv 6)");
}

static void test_19_tension_score(void) {
    CHECK(bass_tension(4, &CH[0], &EMIN) == 0 && bass_tension(11, &CH[0], &EMIN) == 0, "19: root and 5th are 0");
    CHECK(bass_tension(7, &CH[0], &EMIN) == 8, "19: the chord's 3rd is 8");
    CHECK(bass_tension(5, &CH[0], &EMIN) == 85 && bass_tension(10, &CH[0], &EMIN) == 85 && bass_tension(3, &CH[0], &EMIN) == 60, "19: out of the scale");
    CHECK(bass_level(0) == 0 && bass_level(11) == 0 && bass_level(12) == 1 && bass_level(28) == 2 && bass_level(50) == 3 &&
          bass_level(72) == 4 && bass_level(100) == 4, "19: levels at 0 / 12 / 28 / 50 / 72");
    CHECK(strcmp(bass_level_word(0), "plain") == 0 && strcmp(bass_level_word(4), "clash") == 0, "19: level words");
    unsigned char none[2] = {0, 0};
    const bass_chord_t *of[2] = {&CH[0], &CH[0]};
    CHECK(bass_score(none, 2, of, &EMIN) == 0, "19: an empty bar scores 0");
}

int main(void) {
    test_19_styles();
    test_19_sweep_matches_canvas();
    test_19_spots();
    test_19_spans();
    test_19_scale_steps();
    test_19_extremes();
    test_19_tension_score();
    if (fails) { printf("FAILED: %d\n", fails); return 1; }
    printf("PASSED\n");
    return 0;
}
