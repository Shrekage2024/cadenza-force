/* Offline test of src/jamlib.c (run by tests/run_tests.sh under ASan/UBSan). Exit 1 on failure.
 * `test_jamlib --pictures` prints vst/jam_pictures_cb.txt: 30 CHORDS lines then 30 BASS lines (column-major, 5 levels each) of 16
 * frames. CHORDS: per step 0 rest, 1 hit, 2 hold. BASS: the block rendered on E minor bar 0 (Em, next C) through bass_span with
 * the block's settings and codes: 0 rest, 1 + pitch level * 5 + tension level for a hit, 61 + pitch level for a hold. */
#include "../src/jamlib.h"
#include "../src/bassline.h"
#include "../src/rhythm.h"
#include "../src/theory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static const bass_key_t EMIN = {4, {4, 6, 7, 9, 11, 0, 2}};
static const bass_chord_t EM = {4, 7, 1}, C_MAJ = {0, 4, 0};

static int hits(const char *s) { int n = 0; for (int i = 0; s[i]; i++) n += s[i] == 'x' || s[i] == 'A' || s[i] == 'g'; return n; }

static int in_range(int v, int lo, int hi) { return v >= lo && v <= hi; }

static void test_22_tables(void) {
    int names = 1, fit = 1, codes = 1, hold = 1, ranges = 1, sparse = 1, busy = 1;
    for (int lane = 0; lane < JL_COUNT; lane++)
        for (int c = 0; c < JAM_COLS; c++) {
            int lo = 99, hi = 0, h[JAM_LEVELS];
            for (int l = 0; l < JAM_LEVELS; l++) {
                const char *n = jam_block_name(lane, c, l), *s = jam_block_codes(lane, c, l);
                unsigned char u[16];
                names &= n && n[0] && strlen(n) <= 10;
                fit &= n && strlen(n) * 0.45 * 18 <= 140;   /* tools/textfit.py's rule: 0.45 x size 18 in the 140 px cell */
                codes &= s && strlen(s) == 16 && rhythm_parse(s, u) == 1 && hits(s) >= 1;
                for (int i = 0, sound = 0; s && s[i]; i++) {
                    if (s[i] == '-') hold &= sound;
                    else sound = s[i] != '.';
                }
                if (lane == JL_CHORDS) {
                    const jam_chords_t *b = jam_chords(c, l);
                    ranges &= in_range(b->mode, 0, VMODE_COUNT - 1) && in_range(b->col, -1, 4) && in_range(b->inv, -2, 3) &&
                              in_range(b->spr, -1, 3) && in_range(b->str, -8, 8) && in_range(b->oct, -1, 1);
                } else {
                    const jam_bass_t *b = jam_bass(c, l);
                    ranges &= in_range(b->pedal, 0, 2) && in_range(b->mov, 0, 4) && in_range(b->octs, 0, 4) && in_range(b->app, 0, 4) &&
                              in_range(b->reg, -2, 2) && in_range(b->octave, -1, 1);
                }
                h[l] = hits(s);
                if (h[l] < lo) lo = h[l];
                if (h[l] > hi) hi = h[l];
            }
            sparse &= h[0] == lo;
            busy &= h[JAM_LEVELS - 1] == hi;
        }
    CHECK(names, "22: all 60 blocks have a name of 1..10 characters");
    CHECK(fit, "22: every name fits the 140 px block cell at size 18");
    CHECK(codes, "22: every codes string is 16 of . x A g - with a hit");
    CHECK(hold, "22: no hold without a sound before it in the bar");
    CHECK(ranges, "22: settings are in their ranges");
    CHECK(sparse, "22: level 1 has the fewest hits in every column");
    CHECK(busy, "22: level 5 has the most hits in every column");
}

static void test_22_accessors(void) {
    CHECK(strcmp(jam_genre_name(0), "HOUSE") == 0 && strcmp(jam_genre_name(5), "DISCO") == 0 && strcmp(jam_genre_name(9), "DISCO") == 0 &&
          strcmp(jam_genre_name(-1), "HOUSE") == 0, "22: genre names in order, clamped");
    CHECK(jam_chords(-1, 9) == jam_chords(0, 4) && jam_bass(9, -1) == jam_bass(5, 0), "22: block lookups clamp");
    CHECK(strcmp(jam_block_name(JL_CHORDS, 0, 0), "PAD") == 0 && strcmp(jam_block_name(JL_BASS, 1, 0), "SUB DRONE") == 0 &&
          strcmp(jam_block_codes(JL_CHORDS, 1, 0), "x---------------") == 0, "22: names and codes by lane");
}

static int level(int m) { return jam_bass_level(m); }

static void chords_frames(int c, int l, int f[16]) {
    const char *s = jam_chords(c, l)->codes;
    for (int i = 0; i < 16; i++) f[i] = s[i] == '-' ? 2 : s[i] == '.' ? 0 : 1;
}

static void bass_frames(int c, int l, int f[16]) {
    const jam_bass_t *b = jam_bass(c, l);
    unsigned char codes[16], note[16];
    char role[16];
    bass_set_t set = {-1, b->pedal, b->octave, {b->mov, b->octs, b->app, b->reg}};
    bass_span_in_t in = {codes, 16, &EM, &C_MAJ, &EMIN, set, 0, NULL};
    rhythm_parse(b->codes, codes);
    bass_span(&in, note, role);
    for (int i = 0; i < 16; i++) {
        f[i] = 0;
        if (note[i]) f[i] = 1 + level(note[i]) * 5 + bass_level(bass_tension(note[i] % 12, &EM, &EMIN));
        else if (role[i] == '-') {
            int j = i - 1;
            while (j > 0 && !note[j]) j--;
            f[i] = 61 + (note[j] ? level(note[j]) : 0);
        }
    }
}

static void pictures(int fr[2 * JAM_COLS * JAM_LEVELS][16]) {
    for (int c = 0; c < JAM_COLS; c++)
        for (int l = 0; l < JAM_LEVELS; l++) {
            chords_frames(c, l, fr[c * JAM_LEVELS + l]);
            bass_frames(c, l, fr[JAM_COLS * JAM_LEVELS + c * JAM_LEVELS + l]);
        }
}

static void test_22_bass_heights(void) {
    static int fr[2 * JAM_COLS * JAM_LEVELS][16];
    int ok = 1, win = jam_bass_level(24) == 0 && jam_bass_level(59) == 11 && jam_bass_level(41) == 5 && jam_bass_level(100) == 11 && jam_bass_level(0) == 0;
    pictures(fr);
    for (int c = 0; c < JAM_COLS; c++)
        for (int l = 0; l < JAM_LEVELS; l++) {
            const jam_bass_t *b = jam_bass(c, l);
            int lo = 99, hi = -1, *f = fr[JAM_COLS * JAM_LEVELS + c * JAM_LEVELS + l];
            for (int i = 0; i < 16; i++) if (f[i] > 0 && f[i] < 61) { int h = (f[i] - 1) / 5; if (h < lo) lo = h; if (h > hi) hi = h; }
            if ((b->octs > 0 || b->mov > 0) && lo == hi) { ok = 0; printf("flat: %s %s\n", jam_genre_name(c), b->name); }
        }
    CHECK(win, "22: jam_bass_level maps MIDI 24..59 to 0..11");
    CHECK(ok, "22: a BASS block with OCTAVES or MOVEMENT shows more than one height");
}

static void test_22_pictures_pinned(void) {
    enum { N = 2 * JAM_COLS * JAM_LEVELS };
    static int got[N][16], want[N][16];
    int n = 0, bass_sound = 1;
    FILE *f = fopen("vst/jam_pictures_cb.txt", "r");
    pictures(got);
    if (f) {
        while (n < N * 16 && fscanf(f, "%d", &want[n / 16][n % 16]) == 1) n++;
        fclose(f);
    }
    for (int k = JAM_COLS * JAM_LEVELS; k < N; k++) {
        int any = 0;
        for (int i = 0; i < 16; i++) any |= got[k][i] > 0 && got[k][i] < 61;
        bass_sound &= any;
    }
    CHECK(bass_sound, "22: every BASS block sounds a note on Em");
    CHECK(n == N * 16, "22: vst/jam_pictures_cb.txt has 60 lines of 16 frames (%d read)", n);
    CHECK(n == N * 16 && memcmp(got, want, sizeof got) == 0, "22: the JAM block pictures equal vst/jam_pictures_cb.txt");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--pictures") == 0) {
        static int fr[2 * JAM_COLS * JAM_LEVELS][16];
        pictures(fr);
        for (int k = 0; k < 2 * JAM_COLS * JAM_LEVELS; k++)
            for (int i = 0; i < 16; i++) printf("%d%s", fr[k][i], i < 15 ? " " : "\n");
        return 0;
    }
    test_22_tables();
    test_22_accessors();
    test_22_bass_heights();
    test_22_pictures_pinned();
    if (fails) { printf("FAILED: %d\n", fails); return 1; }
    printf("PASSED\n");
    return 0;
}
