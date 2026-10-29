/* Offline test of src/rhythm.c (run by tests/run_tests.sh under ASan/UBSan). Exit 1 on failure. */
#include "../src/rhythm.h"
#include "../src/gen.h"
#include "../src/theory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static void test_18_library(void) {
    static const char *names[RHYTHM_COUNT] = {
        "Sustain", "Ballad", "Half-time", "Pulse", "Eighths", "Push", "Stabs", "Anthem", "Offbeat", "Garage Skip",
        "Trance Gate", "Dub Stab", "Boom Bap", "Trap Hold", "Drill", "Lo-fi Lazy", "Skank", "Son Clave", "Tresillo",
        "Bossa", "Funk", "Charleston", "Neo-soul", "Gospel Push"};
    static const char *groups[RHYTHM_GROUPS] = {"Basic", "Pop / Rock", "Dance", "Hip hop", "Latin", "Funk / Soul"};
    static const int is_new[RHYTHM_COUNT] = {0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1};
    int names_ok = 1, uniq = 1, groups_ok = 1, new_ok = 1, new_n = 0, feel_ok = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        names_ok &= strcmp(rhythm_name(i), names[i]) == 0;
        feel_ok &= rhythm_feel(i) != NULL && rhythm_feel(i)[0] != 0;
        for (int j = 0; j < i; j++) uniq &= strcmp(rhythm_name(i), rhythm_name(j)) != 0;
        groups_ok &= rhythm_group(i) == i / 4;
        new_ok &= rhythm_is_new(i) == is_new[i];
        new_n += rhythm_is_new(i);
    }
    CHECK(names_ok, "18: the 24 names, in the spec's order");
    CHECK(uniq, "18: names are unique");
    CHECK(feel_ok, "18: every rhythm has a feel line");
    CHECK(groups_ok, "18: groups 0..5, four each, in table order");
    CHECK(new_ok && new_n == 15, "18: exactly the 15 starred rhythms are new (%d)", new_n);
    int gn_ok = 1;
    for (int g = 0; g < RHYTHM_GROUPS; g++) gn_ok &= strcmp(rhythm_group_name(g), groups[g]) == 0;
    CHECK(gn_ok, "18: group names");
}

static void test_18_parse_format(void) {
    int ok = 1, found = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        unsigned char c[16], d[16];
        char s[17];
        rhythm_codes(i, c);
        rhythm_format(c, s);
        ok &= strlen(s) == 16 && rhythm_parse(s, d) == 1 && memcmp(c, d, 16) == 0;
        found &= rhythm_find(c) == i;
    }
    CHECK(ok, "18: format then parse round-trips all 24");
    CHECK(found, "18: rhythm_find returns each library index");
    unsigned char d[16];
    CHECK(rhythm_parse("A..x", d) == 0, "18: a short string is refused");
    CHECK(rhythm_parse("A...............z", d) == 0 && rhythm_parse("A..............z", d) == 0,
          "18: a bad character or length is refused");
    memset(d, RC_HIT, 16);
    CHECK(rhythm_find(d) == -1, "18: an unknown pattern finds -1");
}

static void test_18_steps_round_trip(void) {
    int ok = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        unsigned char c[16], d[16];
        seq_step_t st[16];
        rhythm_codes(i, c);
        rhythm_to_steps(c, st);
        rhythm_from_steps(st, d);
        ok &= memcmp(c, d, 16) == 0;
    }
    CHECK(ok, "18: from_steps(to_steps(c)) == c for all 24");
    unsigned char c[16];
    seq_step_t st[16];
    CHECK(rhythm_parse("A---x...", c) == 0, "18: 8 characters is not a bar");
    rhythm_parse("A---x...........", c);
    rhythm_to_steps(c, st);
    CHECK(st[0].on && st[0].len == 4 && st[0].vel == 112 && st[0].tone == TONE_R && st[0].oct == 0,
          "18: a hit with 3 holds has len 4, accent velocity 112 (len %d vel %d)", st[0].len, st[0].vel);
    CHECK(st[4].on && st[4].len == 1 && st[4].vel == 100 && !st[1].on, "18: plain hit 100, hold steps are off");
    rhythm_parse("g...............", c);
    rhythm_to_steps(c, st);
    CHECK(st[0].vel == 60, "18: ghost velocity 60");
    /* a long note is cut at the next hit and the bar end */
    memset(st, 0, sizeof st);
    st[0] = (seq_step_t){1, TONE_R, 0, 100, 16, 0};
    st[2] = (seq_step_t){1, TONE_R, 0, 100, 1, 0};
    rhythm_from_steps(st, c);
    char s[17];
    rhythm_format(c, s);
    CHECK(strcmp(s, "x-x.............") == 0, "18: holds are cut at the next hit (%s)", s);
}

/* The six 0.12 chord rhythms: gen.c's CH2 template 0 in full (every hit whatever its complexity gate, no ghosts), as
 * {pos, vel, len}; a length is cut at the next hit and the bar end. CH2 is static in gen.c, so the values are copied. */
typedef struct { int pos, vel, len; } hit_t;
static const struct { int lib, n; hit_t h[6]; } V2[6] = {
    {8, 4, {{2, 104, 2}, {6, 100, 2}, {10, 104, 2}, {14, 100, 2}}},                                   /* Offbeat */
    {21, 2, {{0, 112, 6}, {6, 100, 2}}},                                                              /* Charleston */
    {1, 2, {{0, 100, 16}, {8, 88, 8}}},                                                               /* Ballad */
    {20, 6, {{0, 112, 1}, {3, 92, 1}, {6, 104, 1}, {10, 100, 1}, {12, 92, 1}, {14, 100, 1}}},         /* Funk */
    {2, 2, {{0, 108, 8}, {8, 100, 8}}},                                                               /* Half-time */
    {5, 3, {{0, 104, 6}, {6, 96, 2}, {14, 112, 2}}},                                                  /* Push */
};

static void test_18_old_nine_match_gen(void) {
    static const int styles[3] = {GEN_CH_SUSTAIN, GEN_CH_PULSE, GEN_CH_STABS};
    static const int lib[3] = {0, 3, 6};
    static const int prog[1] = {0};
    /* the gen_v1 styles: gen_lane_v1 at complexity 0 (seed 1, variant 0, 1 bar, chord_len 16) */
    for (int k = 0; k < 3; k++) {
        gen_params_t p;
        memset(&p, 0, sizeof p);
        p.style = styles[k]; p.seed = 1; p.complexity = 0; p.variant = 0;
        p.bars = 1; p.chord_len = 16; p.prog = prog; p.prog_len = 1; p.key = 0; p.scale = 0;
        seq_step_t st[16];
        memset(st, 0, sizeof st);
        gen_lane_v1(&p, st);
        unsigned char got[16], want[16];
        rhythm_from_steps(st, got);
        rhythm_codes(lib[k], want);
        char a[17], b[17];
        rhythm_format(got, a);
        rhythm_format(want, b);
        CHECK(memcmp(got, want, 16) == 0, "18: %s: generator %s, library %s", rhythm_name(lib[k]), a, b);
    }
    for (int k = 0; k < 6; k++) {
        seq_step_t st[16];
        memset(st, 0, sizeof st);
        for (int j = 0; j < V2[k].n; j++) {
            int len = V2[k].h[j].len;
            int room = (j + 1 < V2[k].n ? V2[k].h[j + 1].pos : 16) - V2[k].h[j].pos;
            if (len > room) len = room;
            st[V2[k].h[j].pos] = (seq_step_t){1, TONE_R, 0, (unsigned char)V2[k].h[j].vel, (unsigned char)len, 0};
        }
        unsigned char got[16], want[16];
        rhythm_from_steps(st, got);
        rhythm_codes(V2[k].lib, want);
        char a[17], b[17];
        rhythm_format(got, a);
        rhythm_format(want, b);
        CHECK(memcmp(got, want, 16) == 0, "18: %s: template 0 %s, library %s", rhythm_name(V2[k].lib), a, b);
    }
}

static void test_18_library_distinct(void) {
    int distinct = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++)
        for (int j = 0; j < i; j++) {
            unsigned char a[16], b[16];
            rhythm_codes(i, a);
            rhythm_codes(j, b);
            distinct &= memcmp(a, b, 16) != 0;
        }
    CHECK(distinct, "18: all 24 library code arrays are pairwise different");
}


static void fmt(const unsigned char *c, char out[17]) { rhythm_format(c, out); }

static int n_on(const unsigned char *c) {
    int n = 0;
    for (int i = 0; i < 16; i++) n += c[i] == RC_HIT || c[i] == RC_ACC || c[i] == RC_GHOST;
    return n;
}

static void test_18_hits_floor(void) {
    int one = 1, beat1 = 1, ident = 1, full = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        unsigned char c[16], o[16];
        rhythm_codes(i, c);
        rhythm_hits(c, 0, o);
        one &= n_on(o) == 1;
        int best = -1, bs = -99;
        for (int k = 0; k < 16; k++) {
            if (c[k] != RC_HIT && c[k] != RC_ACC && c[k] != RC_GHOST) continue;
            int s = rhythm_weight(k) + (c[k] == RC_ACC ? 6 : 0) - (c[k] == RC_GHOST ? 6 : 0);
            if (s > bs) { bs = s; best = k; }
        }
        beat1 &= o[best] != RC_REST && o[best] != RC_HOLD;
        if (c[0] == RC_HIT || c[0] == RC_ACC) beat1 &= o[0] == c[0];
        rhythm_hits(c, 50, o);
        ident &= memcmp(o, c, 16) == 0;
        rhythm_hits(c, 100, o);
        for (int k = 0; k < 16; k++) full &= o[k] != RC_REST;
    }
    CHECK(one, "18: hits at 0 leaves exactly one hit for all 24");
    CHECK(beat1, "18: hits at 0 keeps beat 1 (or the highest-ranked hit)");
    CHECK(ident, "18: hits at 50 is the identity");
    CHECK(full, "18: hits at 100 leaves no rest");
    unsigned char c[16], o[16];
    rhythm_parse("x-x-x-x-x-x-x-x-", c);
    rhythm_hits(c, 0, o);
    char s[17];
    fmt(o, s);
    CHECK(strcmp(s, "x-..............") == 0, "18: a removed hit clears its holds (%s)", s);
}

static void test_18_hits_monotone(void) {
    int ok = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        unsigned char c[16], o[16];
        rhythm_codes(i, c);
        int prev = 0;
        for (int cx = 0; cx <= 100; cx++) {
            rhythm_hits(c, cx, o);
            int n = n_on(o);
            if (n < prev) ok = 0;
            prev = n;
        }
    }
    CHECK(ok, "18: hit count never falls as cx rises 0..100, all 24");
}

static void test_18_sync_ghosts_fill(void) {
    unsigned char p[16], o[16], f[16];
    char s[17];
    rhythm_parse("x...x...x...x...", p);   /* Pulse drawn on rests: the library's Pulse holds fill 14 and 3 */
    rhythm_sync(p, 1, o);
    CHECK(o[0] == RC_REST && o[14] == RC_HIT, "18: Pulse SYNC +1 moves step 0 to 14");
    rhythm_codes(20, f);
    rhythm_sync(f, -2, o);
    int beats = 1;
    for (int k = 0; k < 16; k++) if (k % 4 && o[k] != RC_REST && o[k] != RC_HOLD) beats = 0;
    CHECK(beats && n_on(o) > 0, "18: Funk SYNC -2 leaves hits on beats only");
    rhythm_ghosts(p, 2, o);
    CHECK(o[3] == RC_GHOST && o[11] == RC_GHOST && n_on(o) == 6, "18: Pulse GHOSTS 2 adds ghosts at 3 and 11");
    unsigned char q[16];
    rhythm_parse("x---x---x---x---", q);   /* the library Pulse: holds fill the gaps */
    rhythm_sync(q, 1, o);
    fmt(o, s);
    CHECK(strcmp(s, "....x---x---x-x-") == 0, "18: Pulse (held) SYNC +1 moves 0 to 14, cuts 12's hold (%s)", s);
    rhythm_parse("x---------------", q);   /* Sustain */
    rhythm_sync(q, 1, o);
    fmt(o, s);
    CHECK(strcmp(s, "..............x-") == 0, "18: Sustain SYNC +1 moves its hit to 14 holding 15 (%s)", s);
    rhythm_ghosts(q, 2, o);
    fmt(o, s);
    CHECK(strcmp(s, "x--g-------g----") == 0, "18: Sustain GHOSTS 2 adds ghosts at 3 and 11, holding on (%s)", s);
    rhythm_parse("x---x---x---x---", q);
    rhythm_ghosts(q, 2, o);
    CHECK(o[3] == RC_GHOST && o[11] == RC_GHOST, "18: Pulse (held) GHOSTS 2 puts ghosts at 3 and 11");
    rhythm_parse("x---------------", q);
    rhythm_fill(q, 1, o);
    fmt(o, s);
    CHECK(strcmp(s, "x-------------xg") == 0, "18: Sustain FILL 1 gives a hit at 14 and a ghost at 15 (%s)", s);
    rhythm_hits(q, 100, o);
    CHECK(memcmp(o, q, 16) == 0, "18: hits at 100 leaves Sustain's holds as holds");
    rhythm_parse("x---x---x---x---", q);
    rhythm_fill(q, 2, o);
    fmt(o, s);
    CHECK(strcmp(s, "x---x---xxxxx-x-") == 0, "18: held Pulse FILL 2 places 4 hits onto holds (%s)", s);
    rhythm_ghosts(p, 4, o);
    int none = 1;
    for (int k = 0; k < 16; k++) none &= o[k] != RC_REST;
    CHECK(none, "18: GHOSTS 4 fills every rest");
    rhythm_fill(p, 3, o);
    fmt(o, s);
    CHECK(strcmp(s + 12, "xxxA") == 0, "18: FILL 3 sets 12..15 to x x x A (%s)", s);
    rhythm_fill(p, 4, o);
    fmt(o, s);
    CHECK(strcmp(s + 8, "........") == 0, "18: FILL 4 clears 8..15 (%s)", s);
}

static void test_18_cx_layers(void) {
    cx_offsets_t a = rhythm_cx_offsets(50), b = rhythm_cx_offsets(75), c = rhythm_cx_offsets(100),
                 z = rhythm_cx_offsets(0);
    CHECK(!a.col && !a.spr && !a.sync && !a.strum && !a.ghosts && !a.hum && !a.smooth && !a.tension, "18: cx 50 offsets all 0");
    CHECK(b.col == 1 && b.spr == 1 && b.sync >= 0 && b.smooth == 1, "18: cx 75 col +1 spr +1 smooth");
    CHECK(c.col == 2 && c.tension == 1 && c.sync == 2 && c.ghosts == 3, "18: cx 100 col +2 tension +1 sync +2 ghosts +3");
    CHECK(!rhythm_cx_offsets(95).tension && rhythm_cx_offsets(95).col == 2 && !z.tension && !b.tension,
          "18: Tension only above 95, apart from col (Colour 1 and 2)");
    CHECK(z.col == -2 && z.spr == -3 && z.sync == -5 && z.strum == -8 && z.ghosts == -4 && z.hum == -4 && !z.smooth,
          "18: cx 0 offsets col -2 spr -3 sync -5 strum -8 ghosts -4 hum -4");
    CHECK(rhythm_cx_apply(CXT_COL, 0, z.col) == -1 && rhythm_cx_apply(CXT_SPR, 0, z.spr) == 0 &&
          rhythm_cx_apply(CXT_SYNC, 0, z.sync) == -2 && rhythm_cx_apply(CXT_STRUM, 0, z.strum) == 0 &&
          rhythm_cx_apply(CXT_GHOSTS, 0, z.ghosts) == 0 && rhythm_cx_apply(CXT_HUM, 0, z.hum) == 0,
          "18: neutral knobs at cx 0 land on Power, Close, -2, 0, 0, 0");
    CHECK(rhythm_cx_apply(CXT_SPR, -1, z.spr) == 0, "18: Cluster simplifies to Close, not past it");
    CHECK(rhythm_cx_apply(CXT_STRUM, -5, -8) == 0 && rhythm_cx_apply(CXT_STRUM, 5, -8) == 0 &&
          rhythm_cx_apply(CXT_STRUM, 3, 2) == 5 && rhythm_cx_apply(CXT_STRUM, -3, 2) == -5 &&
          rhythm_cx_apply(CXT_STRUM, 7, 2) == 8, "18: STRUM simplifies to 0 from either side, adds away from 0");
    CHECK(rhythm_cx_apply(CXT_COL, 4, 3) == 4 && rhythm_cx_apply(CXT_SYNC, 3, 2) == 3, "18: clamped to range");
    int step_ok = 1;
    for (int cx = 0; cx < 100; cx++) {
        cx_offsets_t p = rhythm_cx_offsets(cx), q = rhythm_cx_offsets(cx + 1);
        step_ok &= abs(p.col + p.tension - q.col - q.tension) <= 1 && abs(p.spr - q.spr) <= 1 && abs(p.sync - q.sync) <= 1 &&
                   abs(p.strum - q.strum) <= 1 && abs(p.ghosts - q.ghosts) <= 1 && abs(p.hum - q.hum) <= 1;
    }
    CHECK(step_ok, "18: offsets never jump by more than 1 between cx and cx+1");
}

static void test_18_lhl_rises(void) {
    /* More hits fill the gaps and so lower syncopation, so the rise is measured on the SYNC layer: the rhythms as
     * drawn (cx 50 hits) with the cx offset's SYNC applied. */
    double prev = -1;
    int rises = 1, capped = 1;
    for (int cx = 50; cx <= 100; cx += 5) {
        cx_offsets_t o = rhythm_cx_offsets(cx);
        long sum = 0;
        for (int i = 0; i < RHYTHM_COUNT; i++) {
            unsigned char c[16], y[16], cap[16];
            rhythm_codes(i, c);
            rhythm_sync(c, o.sync, y);
            rhythm_sync(c, 3, cap);
            sum += rhythm_lhl(y);
            capped &= rhythm_lhl(y) <= rhythm_lhl(cap);
        }
        double mean = (double)sum / RHYTHM_COUNT;
        if (mean < prev) rises = 0;
        prev = mean;
    }
    CHECK(rises, "18: mean LHL with the cx SYNC offset never falls from cx 50 to 100");
    CHECK(capped, "18: LHL at the cx SYNC offset stays within the value at SYNC +3");
    unsigned char p[16], o[16];
    rhythm_parse("x...x...x...x...", p);
    rhythm_sync(p, 1, o);
    CHECK(rhythm_lhl(o) > rhythm_lhl(p), "18: LHL scores a pushed Pulse above a plain one");
}

static void test_19_bass_library(void) {
    static const char *names[RHYTHM_COUNT] = {
        "Root", "Halves", "Quarters", "Eighths", "Offbeat", "Rolling", "Gallop", "Sidechain", "808 Hold", "808 Bounce",
        "Drill", "Boom Bap", "Funk", "Slap", "Motown", "Disco", "One Drop", "Steppers", "Tumbao", "Tresillo", "Walk",
        "Two Feel", "Neo-soul", "Gospel Push"};
    static const char *groups[RHYTHM_GROUPS] = {"Basic", "Dance", "Hip hop", "Funk / Disco", "Latin / Reggae",
                                                "Jazz / Soul"};
    int names_ok = 1, uniq = 1, groups_ok = 1, find_ok = 1, new_ok = 1, gn_ok = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        unsigned char c[16];
        names_ok &= strcmp(rhythm_lib_name(RLIB_BASS, i), names[i]) == 0;
        for (int j = 0; j < i; j++) uniq &= strcmp(rhythm_lib_name(RLIB_BASS, i), rhythm_lib_name(RLIB_BASS, j)) != 0;
        groups_ok &= rhythm_lib_group(RLIB_BASS, i) == i / 4;
        new_ok &= rhythm_lib_is_new(RLIB_BASS, i) == 1 && rhythm_lib_feel(RLIB_BASS, i)[0] != 0;
        rhythm_lib_codes(RLIB_BASS, i, c);
        find_ok &= rhythm_lib_find(RLIB_BASS, c) == i;
    }
    for (int g = 0; g < RHYTHM_GROUPS; g++) gn_ok &= strcmp(rhythm_lib_group_name(RLIB_BASS, g), groups[g]) == 0;
    CHECK(names_ok, "19: the 24 bass names, in the spec's order");
    CHECK(uniq, "19: bass names are unique");
    CHECK(groups_ok && gn_ok, "19: bass groups 0..5, 4 each, with the 6 names");
    CHECK(new_ok, "19: every bass rhythm is new and has a feel");
    CHECK(find_ok, "19: rhythm_lib_find(RLIB_BASS) returns each rhythm's own index");
}

static void test_19_chords_library_unchanged(void) {
    int same = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        unsigned char a[16], b[16];
        rhythm_codes(i, a);
        rhythm_lib_codes(RLIB_CHORDS, i, b);
        same &= strcmp(rhythm_lib_name(RLIB_CHORDS, i), rhythm_name(i)) == 0 && memcmp(a, b, 16) == 0 &&
                rhythm_lib_find(RLIB_CHORDS, b) == rhythm_find(a);
    }
    CHECK(same, "19: the Chords library through rhythm_lib_* equals the 0.18 calls");
}

static void test_19_bass_hits_floor(void) {
    int ok = 1;
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        unsigned char c[16], o[16];
        int n = 0;
        rhythm_lib_codes(RLIB_BASS, i, c);
        rhythm_hits(c, 0, o);
        for (int k = 0; k < 16; k++) n += rhythm_is_hit(o[k]);
        ok &= n == 1;
    }
    CHECK(ok, "19: rhythm_hits at COMPLEXITY 0 leaves exactly 1 hit of every bass rhythm");
}

/* rhythm_step_code reads one step as rhythm_from_steps reads the bar: random bars of hits of any velocity and length */
static void test_19_step_code(void) {
    unsigned x = 12345u;
    int ok = 1;
    for (int n = 0; n < 2000; n++) {
        seq_step_t st[16];
        unsigned char c[16];
        memset(st, 0, sizeof st);
        for (int k = 0; k < 16; k++) {
            x = x * 1103515245u + 12345u;
            if ((x >> 16) % 3) continue;
            st[k] = (seq_step_t){1, TONE_R, 0, (unsigned char)(1 + (x >> 8) % 127), (unsigned char)(1 + (x >> 20) % 16), 0};
        }
        rhythm_from_steps(st, c);
        for (int k = 0; k < 16; k++) ok &= rhythm_step_code(st, k) == c[k];
    }
    CHECK(ok, "19: rhythm_step_code(st, k) == rhythm_from_steps' out[k] over 2000 random bars");
}

static void fmt16(const unsigned char c[16], char o[17]) { rhythm_format(c, o); }

/* 0.22 TIME and VARIATION */
static void test_22_time_vary(void) {
    unsigned char in[16], out[16];
    char s[17];
    rhythm_parse("x.x.x.x.x.x.x.x.", in);
    rhythm_time(in, -1, out);
    fmt16(out, s);
    CHECK(strcmp(s, "x-..x-..x-..x-..") == 0, "22: HALF stretches the first 8 codes");
    rhythm_parse("x...x...x...x...", in);
    rhythm_time(in, 1, out);
    fmt16(out, s);
    CHECK(strcmp(s, "x.x.x.x.x.x.x.x.") == 0, "22: DOUBLE copies the even codes into both halves");
    rhythm_time(in, 0, out);
    CHECK(!memcmp(in, out, 16), "22: TIME 0 is identity");
    rhythm_parse("-A-g----x-x-----", in);
    int bad = 0;
    for (int tm = -1; tm <= 1; tm += 2) {
        rhythm_time(in, tm, out);
        for (int i = 0; i < 16; i++) bad |= out[i] == RC_HOLD && (i == 0 || out[i - 1] == RC_REST);
    }
    CHECK(!bad, "22: a hold never follows a rest after TIME");
    rhythm_parse("x...x...x...x...", in);
    rhythm_vary(in, 0, 3, out);
    CHECK(!memcmp(in, out, 16), "22: VARIATION 0 is identity");
    for (int pos = 1; pos <= 3; pos++) {
        rhythm_vary(in, pos, 3, out);
        CHECK(memcmp(in, out, 16) != 0, "22: VARIATION 1..3 change a four-on-the-floor bar");
    }
    rhythm_vary(in, 2, 3, out);
    fmt16(out, s);
    CHECK(strcmp(s, "..x...x...x...x.") == 0, "22: VARIATION 2 rotates the bar 2 steps later");
    rhythm_parse(".............A-.", in);
    rhythm_vary(in, 2, 3, out);
    fmt16(out, s);
    CHECK(strcmp(s, "A..............A") == 0, "22: a wrapped hold at the bar start takes its hit's code");
}

/* 0.22 every library and JAM bar through TIME then VARIATION: codes stay valid, no hold after a rest */
static void test_22_transform_sweep(void) {
    int bad = 0, n = 0;
    for (int lib = 0; lib < 2; lib++)
        for (int k = 0; k < RHYTHM_COUNT + JAM_COLS * JAM_LEVELS; k++) {
            unsigned char c[16], t[16], v[16];
            rhythm_lib_codes(lib, k < RHYTHM_COUNT ? k : RJAM_BASE + k - RHYTHM_COUNT, c);
            for (int tm = -1; tm <= 1; tm++)
                for (int pos = 0; pos <= 3; pos++) {
                    rhythm_time(c, tm, t);
                    rhythm_vary(t, pos, 3, v);
                    n++;
                    for (int i = 0; i < 16; i++) bad |= v[i] > RC_HOLD || (v[i] == RC_HOLD && (i == 0 || v[i - 1] == RC_REST));
                }
        }
    CHECK(!bad && n == 2 * 54 * 12, "22: TIME then VARIATION over every library and JAM bar: valid codes, no hold after a rest");
}

/* 0.24 CHORDS NEW BAR's versions (rhythm_newbar) */
static void test_24_newbar(void) {
    unsigned char in[16], out[16];
    char t[17];
    rhythm_parse("x...x...x...x...", in);
    rhythm_newbar(in, 1, out);
    fmt16(out, t);
    CHECK(strcmp(t, "x.x...x...x.....") == 0, "24: v1 moves the hits on beats 2-4 an 8th early (%s)", t);
    rhythm_newbar(in, 3, out);
    fmt16(out, t);
    CHECK(strcmp(t, "x.x...x...x.....") == 0, "24: any odd v is the same version");
    rhythm_parse("x..x..x...x.x...", in);   /* Push: no beat hit has two rests before it */
    rhythm_newbar(in, 1, out);
    CHECK(!memcmp(in, out, 16), "24: Push at v1 changes nothing");
    rhythm_newbar(in, 2, out);
    fmt16(out, t);
    CHECK(strcmp(t, "x.xx..x...x.x.x.") == 0, "24: Push at v2 fills the empty 8ths (%s)", t);
    rhythm_parse("x...x---x...A-..", in);
    rhythm_newbar(in, 1, out);
    fmt16(out, t);
    CHECK(strcmp(t, "x.x.....x.A.....") == 0, "24: a moved hit's holds clear; a hit with a hold before it stays (%s)", t);
    rhythm_parse("x-------x-------", in);
    rhythm_newbar(in, 2, out);
    fmt16(out, t);
    CHECK(strcmp(t, "x-------x-------") == 0, "24: a held 8th is not empty (%s)", t);
    rhythm_newbar(in, 0, out);
    CHECK(!memcmp(in, out, 16), "24: v0 is the bar");
}

/* 0.24 LOCK WITH's IN THE GAPS: a hit on each of the other lane's silent 8ths, else one on a beat's first silent 16th */
static void test_24_rhythm_gaps(void) {
    unsigned char in[16], out[16];
    char t[17];
    rhythm_parse("A.x.A.x.A.x.A.x.", in);
    rhythm_gaps(in, out);
    fmt16(out, t);
    CHECK(strcmp(t, ".x...x...x...x..") == 0, "24: every 8th sounds: each beat's first silent 16th (%s)", t);
    rhythm_parse("x..x..x...x.x...", in);
    rhythm_gaps(in, out);
    fmt16(out, t);
    CHECK(strcmp(t, "..x.x...x.....x.") == 0, "24: Push's gaps: its silent 8ths (%s)", t);
    rhythm_parse("x-------x-------", in);
    rhythm_gaps(in, out);
    fmt16(out, t);
    CHECK(strcmp(t, "................") == 0, "24: a hold is not silent: no gap (%s)", t);
    rhythm_parse("................", in);
    rhythm_gaps(in, out);
    fmt16(out, t);
    CHECK(strcmp(t, "x.x.x.x.x.x.x.x.") == 0, "24: an empty bar: every 8th (%s)", t);
}

int main(void) {
    test_18_library();
    test_18_parse_format();
    test_18_steps_round_trip();
    test_18_old_nine_match_gen();
    test_18_library_distinct();
    test_18_hits_floor();
    test_18_hits_monotone();
    test_18_sync_ghosts_fill();
    test_18_cx_layers();
    test_18_lhl_rises();
    test_19_bass_library();
    test_19_chords_library_unchanged();
    test_19_bass_hits_floor();
    test_19_step_code();
    test_22_time_vary();
    test_22_transform_sweep();
    test_24_newbar();
    test_24_rhythm_gaps();
    if (fails) { printf("FAILED: %d\n", fails); return 1; }
    printf("PASSED\n");
    return 0;
}
