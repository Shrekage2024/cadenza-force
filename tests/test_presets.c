/* Offline test of src/presets.c, the 0.23 genre presets (run by tests/run_tests.sh under ASan/UBSan). Exit 1 on failure. */
#include "../src/presets.h"
#include "../src/seq_core.h"
#include "../src/jamlib.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int fails;

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static void no_midi(void *ctx, int status, int note, int vel) { (void)ctx, (void)status, (void)note, (void)vel; }

static int held[16][128], ons;   /* note-ons minus note-offs per channel and note; note-ons logged */
static void count_midi(void *ctx, int status, int note, int vel) {
    (void)ctx;
    if ((status & 0xf0) == 0x90 && vel > 0) held[status & 15][note]++, ons++;
    else if ((status & 0xf0) == 0x80 || (status & 0xf0) == 0x90) held[status & 15][note]--;
}

static double play(seq_t *s, double ppq, double beats) {   /* 128-frame blocks at 120 BPM */
    for (double end = ppq + beats; ppq < end; ppq += 128 * 2.0 / 44100.0) seq_block(s, ppq, 120.0, 1, 128);
    return ppq;
}

static int ascii(const char *s) {
    for (; *s; s++)
        if (*s < 32 || *s > 126) return 0;
    return 1;
}

/* by seq part index (Verse, Intro, Pre, Chorus, Bridge, Outro): bars and the CHORDS / BASS / MELODY JAM levels (0-based) */
static const int BARS[SEQ_NPARTS] = {8, 8, 4, 8, 8, 4};
static const int LC[SEQ_NPARTS] = {1, 0, 2, 3, 1, 0}, LB[SEQ_NPARTS] = {1, 0, 2, 3, 0, 0}, LM[SEQ_NPARTS] = {1, 0, 2, 3, 1, 0};

static void test_23_presets(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX], again[SEQ_STATE_MAX];
    for (int i = 0; i < PRESETS_COUNT; i++) {
        const preset_info_t *in = preset_info(i);
        int n = preset_build(i, buf, sizeof buf), parts = 1, levels = 1;
        CHECK(in && strlen(in->name) <= 23 && ascii(in->name) && ascii(in->sounds) && !strncasecmp(in->name, jam_genre_name(i),
              strlen(jam_genre_name(i))) && in->bpm >= 60 && in->bpm <= 200, "%s: an ASCII name of at most 23 characters, after its JAM genre %s",
              in ? in->name : "?", jam_genre_name(i));
        CHECK(n > 0 && n < SEQ_STATE_MAX, "%s builds: %d bytes", in->name, n);
        CHECK(preset_build(i, again, sizeof again) == n && !strcmp(buf, again), "%s builds the same bytes again", in->name);
        CHECK(preset_chunk(i) && !strcmp(preset_chunk(i), buf), "%s: src/presets_data.h is current (else run tools/make_presets.sh)", in->name);
        seq_init(&s, no_midi, NULL);
        CHECK(seq_state_load(&s, buf) && seq_part(&s) == 0 && seq_preset_loaded(&s) == i + 1, "%s loads, on the Verse, gn %d", in->name,
              i + 1);
        for (int p = 0; p < SEQ_NPARTS; p++) {
            int c, l;
            parts &= seq_part_used(&s, p) && s.part[p].bars == BARS[p];
            seq_select_part(&s, p);
            levels &= seq_jam_playing(&s, LANE_CHORDS, &c, &l) && c == i && l == LC[p];
            levels &= seq_jam_playing(&s, LANE_BASS, &c, &l) && c == i && l == LB[p];
            levels &= s.part[p].mjam.set.l == LM[p];
        }
        CHECK(parts, "%s: all 6 parts in use, 8 8 4 8 8 4 bars (Verse Intro Pre Chorus Bridge Outro)", in->name);
        CHECK(levels, "%s: every part's CHORDS and BASS blocks are its genre at the part's level, MELODY at its level", in->name);
    }
    CHECK(!preset_info(-1) && !preset_info(PRESETS_COUNT) && !preset_chunk(-1) && !preset_chunk(PRESETS_COUNT), "no preset out of range");
    CHECK(!preset_build(-1, buf, sizeof buf) && !preset_build(PRESETS_COUNT, buf, sizeof buf), "out of range builds nothing");
    CHECK(!preset_build(0, buf, 100), "a buffer too small builds nothing");
}

/* A pick while playing: the old notes end, the preset plays on from the song position, nothing hangs after a stop */
static void test_23_pick_while_playing(void) {
    static seq_t s;
    static char buf[SEQ_STATE_MAX];
    int bad = 0, after;
    double ppq;
    memset(held, 0, sizeof held);
    ons = 0;
    seq_init(&s, count_midi, NULL);
    ppq = play(&s, 0.0, 6.3);
    CHECK(preset_build(0, buf, sizeof buf) && seq_state_load(&s, buf), "House loads mid-bar while playing");
    after = ons;
    ppq = play(&s, ppq, 8.0);
    seq_block(&s, ppq, 120.0, 0, 128);
    for (int c = 0; c < 16; c++)
        for (int n = 0; n < 128; n++) bad += held[c][n] != 0;
    CHECK(ons > after && !bad, "it plays on (%d note-ons after the load) and every note-on has its note-off (%d hanging)", ons - after, bad);
}

int main(void) {
    test_23_presets();
    test_23_pick_while_playing();
    printf(fails ? "\nFAILED: %d\n" : "\nPASSED\n", fails);
    return fails != 0;
}
