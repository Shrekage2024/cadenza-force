/* Offline test of src/melody2.c (run by tests/run_tests.sh under ASan/UBSan). Exit 1 on failure.
 * The sweeps and the spot lines are the canvas model in tests/ref/melody_ref.js (run it to see the same numbers).
 * `test_melody2 --pictures` prints vst/jam_pictures.txt (the JAM blocks' dot frames on E minor bar 0). */
#include "../src/melody2.h"
#include "../src/theory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

#define MEL_SWEEP_A_FNV 0xe47edec5u
#define MEL_SWEEP_B_FNV 0x3ebc2f40u
#define MEL_SWEEP_V_FNV 0xcace6f93u

/* E minor, the loop Em C G D at 1-bar chords (the reference's CH) */
static const mjam_chord_t EM_CH[4] = {{4, 7, 11}, {0, 4, 7}, {7, 11, 2}, {2, 6, 9}};
static mjam_bar_in_t EM_BAR[4];

static void em_init(void) {
    for (int b = 0; b < 4; b++) {
        EM_BAR[b].key = 4;
        EM_BAR[b].scale = SCALE_MINOR;
        for (int i = 0; i < SEQ_STEPS; i++) EM_BAR[b].chord[i] = &EM_CH[b];
    }
}

static mjam_set_t mk(int c, int l, int form, int breath, int cad, int fill, int ans, int stray, int aoct, int reg) {
    mjam_set_t s = {(signed char)c, (signed char)l, (signed char)form, (signed char)breath, (signed char)cad, (signed char)fill,
                    (signed char)ans, (signed char)stray, (signed char)aoct, (signed char)reg, 4, 2, 0, 0, 0, 0};
    return s;
}

static unsigned fnv(unsigned h, int byte) { h ^= (unsigned)byte & 255u; return h * 16777619u; }

/* mjam_bar is mjam_bar_ref with every note folded by octaves into 36..100: the same holds, tensions, frames and score */
static int fold_ok(const mjam_note_t *ref, const mjam_note_t *out) {
    for (int i = 0; i < SEQ_STEPS; i++) {
        int r = ref[i].note, m = out[i].note;
        if (ref[i].hold != out[i].hold || ref[i].tension != out[i].tension || (r == 0) != (m == 0)) return 0;
        if (r && (m < 36 || m > 100 || (m - r) % 12 != 0 || (r >= 36 && r <= 100 && m != r))) return 0;
        if (mjam_frame(ref, i) != mjam_frame(out, i)) return 0;
    }
    return mjam_score(ref) == mjam_score(out);
}

/* One reference bar into the hash (the JS hashBar); counts bars where the fold breaks fold_ok */
static unsigned hash_bar(unsigned h, mjam_set_t set, int b, int v, int *fold_bad) {
    unsigned char vv[4] = {(unsigned char)v, (unsigned char)v, (unsigned char)v, (unsigned char)v};
    mjam_loop_in_t in = {4, SCALE_MINOR, 4, set, EM_BAR, vv};
    mjam_note_t ref[SEQ_STEPS], out[SEQ_STEPS];
    mjam_bar_ref(&in, b, &set, ref);
    mjam_bar(&in, b, &set, out);
    if (!fold_ok(ref, out)) (*fold_bad)++;
    for (int i = 0; i < SEQ_STEPS; i++) h = fnv(h, ref[i].note);
    for (int i = 0; i < SEQ_STEPS; i++) h = fnv(h, ref[i].hold);
    for (int i = 0; i < SEQ_STEPS; i++) h = fnv(h, mjam_frame(ref, i));
    return fnv(h, mjam_score(ref));
}

static void test_20_sweep_a(void) {
    unsigned h = 2166136261u;
    int n = 0, bad = 0;
    for (int c = 0; c < 6; c++) for (int l = 0; l < 5; l++) for (int b = 0; b < 4; b++) for (int form = 0; form < 6; form++)
    for (int ans = 0; ans < 6; ans++) for (int stray = 0; stray < 4; stray++) for (int cad = 0; cad < 4; cad++) {
        h = hash_bar(h, mk(c, l, form, 2, cad, 1, ans, stray, 0, 1), b, 0, &bad);
        n++;
    }
    CHECK(n == 69120, "20: sweep A cases %d", n);
    CHECK(h == MEL_SWEEP_A_FNV, "20: sweep A checksum 0x%08x matches the canvas 0x%08x", h, MEL_SWEEP_A_FNV);
    CHECK(bad == 0, "20: sweep A: the fold into 36..100 changes only out-of-range notes (%d bars differ)", bad);
}

static void test_20_sweep_b(void) {
    unsigned h = 2166136261u;
    int n = 0, bad = 0;
    for (int c = 0; c < 6; c++) for (int l = 0; l < 5; l++) for (int b = 0; b < 4; b++) for (int form = 0; form < 6; form++)
    for (int breath = 0; breath < 4; breath++) for (int fill = 0; fill < 4; fill++) for (int aoct = -1; aoct <= 1; aoct++)
    for (int reg = 0; reg < 3; reg++) {
        h = hash_bar(h, mk(c, l, form, breath, 3, fill, 0, 1, aoct, reg), b, 0, &bad);
        n++;
    }
    CHECK(n == 103680, "20: sweep B cases %d", n);
    CHECK(h == MEL_SWEEP_B_FNV, "20: sweep B checksum 0x%08x matches the canvas 0x%08x", h, MEL_SWEEP_B_FNV);
    CHECK(bad == 0, "20: sweep B: the fold into 36..100 changes only out-of-range notes (%d bars differ)", bad);
}

static void test_20_sweep_v(void) {
    unsigned h = 2166136261u;
    int n = 0, bad = 0;
    for (int c = 0; c < 6; c++) for (int l = 0; l < 5; l++) for (int b = 0; b < 4; b++) for (int v = 1; v <= 3; v++) {
        h = hash_bar(h, mk(c, l, 1, 2, 3, 1, 0, 1, 0, 1), b, v, &bad);
        n++;
    }
    CHECK(n == 360, "20: sweep V cases %d", n);
    CHECK(h == MEL_SWEEP_V_FNV, "20: sweep V checksum 0x%08x matches the canvas 0x%08x", h, MEL_SWEEP_V_FNV);
    CHECK(bad == 0, "20: sweep V: the fold into 36..100 changes only out-of-range notes (%d bars differ)", bad);
}

/* The reference's show(): note names, - held, . rest, then the dots and the score */
static void show(const mjam_note_t *line, char *buf, int len) {
    int n = 0;
    for (int i = 0; i < SEQ_STEPS; i++) {
        char nm[8];
        if (line[i].note) theory_note_name(line[i].note, nm, sizeof nm);
        else strcpy(nm, line[i].hold ? "-" : ".");
        n += snprintf(buf + n, (size_t)(len - n), "%s%s", i ? " " : "", nm);
    }
    n += snprintf(buf + n, (size_t)(len - n), " | dots ");
    for (int i = 0; i < SEQ_STEPS; i++) n += snprintf(buf + n, (size_t)(len - n), "%s%d", i ? "," : "", mjam_frame(line, i));
    snprintf(buf + n, (size_t)(len - n), " | score %d", mjam_score(line));
}

static void test_20_spots(void) {
    static const struct { const char *name; int c, l, form, breath, cad, fill, ans, stray, b; const char *want; } SPOT[7] = {
        {"HOOK MOTIF 1+1 ECHO HOME bar 1", 2, 1, 1, 2, 3, 1, 0, 1, 0,
         "B4 - A4 - B4 - - - . . . . . . . F#4 | dots 26,66,27,66,26,66,66,66,0,0,0,0,0,0,0,22 | score 21"},
        {"HOOK MOTIF 1+1 ECHO HOME bar 2", 2, 1, 1, 2, 3, 1, 0, 1, 1,
         "G4 - F#4 - E4 - - - . . . . G4 - E4 - | dots 21,65,23,65,16,64,64,64,0,0,0,0,21,65,16,64 | score 28"},
        {"HOOK MOTIF 1+1 ECHO HOME bar 3", 2, 1, 1, 2, 3, 1, 0, 1, 2,
         "D5 - C5 - B4 - - - . . . . . . . G4 | dots 31,67,33,67,26,66,66,66,0,0,0,0,0,0,0,21 | score 28"},
        {"HOOK MOTIF 1+1 ECHO HOME bar 4", 2, 1, 1, 2, 3, 1, 0, 1, 3,
         "A4 - G4 - F#4 - - - . . . . A4 - E4 - | dots 26,66,23,65,21,65,65,65,0,0,0,0,26,66,17,64 | score 30"},
        {"PEDAL ALT LOOP bar 2", 5, 2, 0, 0, 0, 0, 0, 1, 1,
         "B4 - C4 - B4 - E4 - B4 - G4 - B4 - E4 - | dots 29,66,11,63,29,66,16,64,29,66,21,65,29,66,16,64 | score 45"},
        {"SWAP RUN-UP last half bar 2", 2, 1, 1, 2, 3, 1, 5, 2, 1,
         "G4 - F#4 - E4 - - - G4 - - - C4 D4 F#4 E4 | dots 21,65,23,65,16,64,64,64,21,65,65,65,11,12,23,16 | score 30"},
        {"ARP PINKY 2+2 FILL RUN bar 2", 0, 2, 2, 1, 3, 3, 0, 1, 1,
         "C4 - C5 - E4 - C5 - G4 - B4 - C4 D4 E4 F#4 | dots 11,63,31,67,16,64,31,67,21,65,28,66,11,12,16,23 | score 31"}};
    for (int k = 0; k < 7; k++) {
        mjam_set_t s = mk(SPOT[k].c, SPOT[k].l, SPOT[k].form, SPOT[k].breath, SPOT[k].cad, SPOT[k].fill, SPOT[k].ans, SPOT[k].stray, 0, 1);
        mjam_loop_in_t in = {4, SCALE_MINOR, 4, s, EM_BAR, NULL};
        mjam_note_t line[SEQ_STEPS];
        char got[256];
        mjam_bar(&in, SPOT[k].b, &s, line);
        show(line, got, sizeof got);
        CHECK(strcmp(got, SPOT[k].want) == 0, "20: %s: %s", SPOT[k].name, got);
    }
}

static int in_key(int key, int scale, int pc) {
    for (int d = 0; d < 7; d++) if (theory_degree_pc(key, scale, d) == pc) return 1;
    return 0;
}

static int is_tone(const mjam_chord_t *c, int pc) { return pc == c->root || pc == c->third || pc == c->fifth; }

/* the "other keys" results, all 1 while they hold */
typedef struct { int range, pc, slot, pedal, chord, colour, bars; } ok_t;

/* the phrase under 6 settings for block c, l: every note in range, a scale note or a tone of the chord under it */
static void other_key_bars(const mjam_bar_in_t *bar, int c, int l, ok_t *ok) {
    static const unsigned char V[4] = {0, 1, 2, 3};
    mjam_set_t sets[6] = {mjam_default(), mk(c, l, MF_SENT, 3, 3, 3, MA_SHIFT, 3, 1, 2), mk(c, l, MF_1_1, 1, 1, 2, MA_MIRROR, 0, -1, 0),
                          mk(c, l, MF_AAB, 2, 2, 1, MA_RHYTHM, 2, 1, 2), mk(c, l, MF_3_1, 2, 3, 1, MA_TAIL, 1, 0, 1),
                          mk(c, l, MF_2_2, 2, 3, 1, MA_SWAP, 1, 0, 1)};
    sets[0].c = (signed char)c;
    sets[0].l = (signed char)l;
    for (int k = 0; k < 6; k++) {
        mjam_loop_in_t in = {bar[0].key, bar[0].scale, 4, sets[k], bar, V};
        for (int b = 0; b < 4; b++) {
            mjam_note_t line[SEQ_STEPS];
            mjam_bar(&in, b, &sets[k], line);
            for (int i = 0; i < SEQ_STEPS; i++) {
                int m = line[i].note;
                if (!m) continue;
                ok->range &= m >= 36 && m <= 100;
                ok->pc &= in_key(bar[0].key, bar[0].scale, m % 12) || is_tone(bar[b].chord[i], m % 12);
            }
            ok->bars++;
        }
    }
}

/* the raw block c, l: chord-tone-slot-only blocks play only tones of the chord under each note; the pedal stays */
static void other_key_blocks(const mjam_bar_in_t *bar, int c, int l, ok_t *ok) {
    int key = bar[0].key, slots_only = c == MJ_ARP || (c == MJ_OST && l <= 2) || (c == MJ_LEAD && l <= 2);
    mjam_loop_in_t in = {key, bar[0].scale, 4, mjam_default(), bar, NULL};
    for (int b = 0; b < 4; b++) {
        mjam_note_t line[SEQ_STEPS];
        mjam_block_bar(&in, c, l, b, line);
        for (int i = 0; i < SEQ_STEPS; i++) {
            int m = line[i].note;
            if (!m) continue;
            ok->range &= m >= 36 && m <= 100;
            if (slots_only) ok->slot &= is_tone(bar[b].chord[i], m % 12);
            if (c == MJ_PEDAL && i % (l == 2 ? 4 : 2) == 0) ok->pedal &= m % 12 == (key + (l == 1 ? 0 : 7)) % 12;
        }
        if (c == MJ_PEDAL && l == 0) {   /* HOLD 5: one note held to step 14 */
            ok->pedal &= line[0].note && line[0].note % 12 == (key + 7) % 12 && !line[15].note && !line[15].hold;
            for (int i = 1; i < 15; i++) ok->pedal &= !line[i].note && line[i].hold;
        }
    }
}

/* I-vi-IV-V with IV borrowed (theory_slot_scale(scale, 1)) */
static void other_key_chords(int key, int scale, mjam_chord_t ch[4], ok_t *ok) {
    static const int PROG[4] = {0, 5, 3, 4};
    for (int i = 0; i < 4; i++) {
        int ss = i == 2 ? theory_slot_scale(scale, 1) : scale, deg = i == 2 ? theory_borrow_degree(scale, 1) : PROG[i];
        int tri[THEORY_CHORD_MAX], c13[THEORY_CHORD_MAX];
        mjam_chord_of(key, scale, ss, deg, &ch[i]);
        theory_chord_notes(key, ss, deg, COL_TRIAD, tri);
        theory_chord_notes(key, ss, deg, COL_13TH, c13);
        ok->chord &= ch[i].root == tri[0] % 12 && ch[i].third == tri[1] % 12 && ch[i].fifth == tri[2] % 12;
        /* COLOUR 13th has the same root and 3rd: the chord the melody sees never changes with COLOUR */
        ok->colour &= ch[i].root == c13[0] % 12 && ch[i].third == c13[1] % 12;
    }
}

/* Review Focus 5: every key and scale, 1-bar and 1/2-bar chords */
static void test_20_other_keys(void) {
    ok_t ok = {1, 1, 1, 1, 1, 1, 0};
    for (int key = 0; key < 12; key++) for (int scale = 0; scale < SCALE_COUNT; scale++) {
        mjam_chord_t ch[4];
        other_key_chords(key, scale, ch, &ok);
        for (int half = 0; half < 2; half++) {
            mjam_bar_in_t bar[4];
            for (int b = 0; b < 4; b++) {
                bar[b].key = key;
                bar[b].scale = scale;
                for (int i = 0; i < SEQ_STEPS; i++) bar[b].chord[i] = &ch[half ? (2 * b + i / 8) % 4 : b];
            }
            for (int c = 0; c < MJ_COLS; c++) for (int l = 0; l < MJ_LEVELS; l++) {
                other_key_bars(bar, c, l, &ok);
                other_key_blocks(bar, c, l, &ok);
            }
        }
    }
    CHECK(ok.bars == 12 * SCALE_COUNT * 2 * 30 * 6 * 4, "20: other keys: %d bars", ok.bars);
    CHECK(ok.chord, "20: other keys: mjam_chord_of is the chord's triad");
    CHECK(ok.colour, "20: other keys: COLOUR 13th keeps the root and 3rd the melody plays (COLOUR never moves a note)");
    CHECK(ok.range, "20: other keys: every note in 36..100");
    CHECK(ok.pc, "20: other keys: every note is a scale note or a tone of the chord under it");
    CHECK(ok.slot, "20: other keys: chord-tone slots play tones of the chord under them (1-bar and 1/2-bar chords)");
    CHECK(ok.pedal, "20: other keys: PEDAL's own pattern stays on the key's 5th (HOLD 1: the tonic)");
}

/* the last note start of a line, -1 if none */
static int last_start(const mjam_note_t *line) {
    int L = -1;
    for (int i = 0; i < SEQ_STEPS; i++) if (line[i].note) L = i;
    return L;
}

/* Loops not a multiple of 4 bars: FORM LOOP lands home and takes BREATH and FILL on the loop's last bar
 * as well as each phrase's bar 4; every form renders in range. E minor, bar b on EM_CH[(b + off) % 4] */
static void test_20_short_loops_model(void) {
    static const int BARS[3] = {1, 3, 6};
    int range = 1, home = 1, breath = 1, fill = 1, n = 0;
    for (int k = 0; k < 3; k++) for (int off = 0; off < 4; off++) {
        int bars = BARS[k];
        mjam_bar_in_t bar[6];
        for (int b = 0; b < bars; b++) {
            bar[b].key = 4, bar[b].scale = SCALE_MINOR;
            for (int i = 0; i < SEQ_STEPS; i++) bar[b].chord[i] = &EM_CH[(b + off) % 4];
        }
        for (int c = 0; c < MJ_COLS; c++) for (int l = 0; l < MJ_LEVELS; l++) {
            for (int form = 0; form < MF_COUNT; form++) {
                mjam_set_t s = mk(c, l, form, 2, 3, 1, MA_SWAP, 2, 1, 2);
                mjam_loop_in_t in = {4, SCALE_MINOR, bars, s, bar, NULL};
                for (int b = 0; b < bars; b++, n++) {
                    mjam_note_t line[SEQ_STEPS];
                    mjam_bar(&in, b, &s, line);
                    for (int i = 0; i < SEQ_STEPS; i++) range &= !line[i].note || (line[i].note >= 36 && line[i].note <= 100);
                }
            }
            mjam_set_t loop = mk(c, l, MF_LOOP, 0, 3, 0, MA_ECHO, 1, 0, 1), half = mk(c, l, MF_LOOP, 3, 0, 0, MA_ECHO, 1, 0, 1),
                       pick = mk(c, l, MF_LOOP, 0, 0, 1, MA_ECHO, 1, 0, 1);
            mjam_loop_in_t in = {4, SCALE_MINOR, bars, loop, bar, NULL};
            mjam_note_t line[SEQ_STEPS];
            for (int b = 0; b < bars; b++) {
                if (b != bars - 1 && b % 4 != 3) continue;
                mjam_bar(&in, b, &loop, line);
                home &= last_start(line) >= 0 && line[last_start(line)].note % 12 == 4;   /* E: the tonic, CADENCE HOME's target */
                in.set = half;
                mjam_bar(&in, b, &half, line);
                for (int i = 8; i < SEQ_STEPS; i++) breath &= !line[i].note && !line[i].hold;
                in.set = pick;
                mjam_bar(&in, b, &pick, line);
                fill &= line[15].note != 0;   /* PICKUP: a note on step 16, leading into the loop's next bar */
                in.set = loop;
            }
        }
    }
    CHECK(n == 4 * 30 * MF_COUNT * (1 + 3 + 6), "20: short loops: %d bars of 1-, 3- and 6-bar loops render", n);
    CHECK(range, "20: short loops: every note in 36..100");
    CHECK(home, "20: short loops: LOOP with CADENCE HOME ends the loop's last bar (and each phrase's bar 4) on the tonic");
    CHECK(breath, "20: short loops: LOOP's BREATH Half rests the second half of the loop's last bar");
    CHECK(fill, "20: short loops: LOOP's FILL PICKUP plays step 16 of the loop's last bar");
}

static void test_20_names(void) {
    static const char *const BLOCKS[30] = {
        "UP", "UP-DOWN", "PINKY", "ALBERTI", "2-OCTAVE", "PULSE", "OCTAVE", "ROOT-5", "CELL", "3+3+2",
        "2-NOTE", "MOTIF", "PENTA", "SEQUENCE", "PICKUP", "GUIDE", "HALVES", "PHRASE", "PASSING", "SOAR",
        "STEP-IN", "TURN", "RUN-UP", "RUN-DOWN", "CASCADE", "HOLD 5", "HOLD 1", "ALT", "ALT 16", "CLIMB"};
    static const char *const COLS[6] = {"ARP", "OSTINATO", "HOOK", "LEAD", "RUN", "PEDAL"};
    int ok = 1, uniq = 1;
    for (int c = 0; c < MJ_COLS; c++) {
        ok &= strcmp(mjam_col_name(c), COLS[c]) == 0;
        for (int l = 0; l < MJ_LEVELS; l++) {
            ok &= strcmp(mjam_block_name(c, l), BLOCKS[c * MJ_LEVELS + l]) == 0;
            for (int j = 0; j < c * MJ_LEVELS + l; j++) uniq &= strcmp(mjam_block_name(c, l), BLOCKS[j]) != 0;
        }
    }
    CHECK(ok, "20: 6 column and 30 block names in the spec order");
    CHECK(uniq, "20: block names are unique");
    static const char *const F[6] = {"LOOP", "1+1", "2+2", "3+1", "AAB", "SENT"};
    static const char *const A[6] = {"ECHO", "SHIFT", "MIRROR", "RHYTHM", "TAIL", "SWAP"};
    static const char *const B[4] = {"None", "1/8", "Beat", "Half"};
    static const char *const C[4] = {"OFF", "OPEN", "SOFT", "HOME"};
    static const char *const FL[4] = {"REST", "PICKUP", "STEP-IN", "RUN"};
    static const char *const S[4] = {"LAST NOTE", "LAST BEAT", "LAST HALF", "ALL"};
    ok = 1;
    for (int i = 0; i < 6; i++) ok &= strcmp(mjam_form_name(i), F[i]) == 0 && strcmp(mjam_answer_name(i), A[i]) == 0;
    for (int i = 0; i < 4; i++)
        ok &= strcmp(mjam_breath_name(i), B[i]) == 0 && strcmp(mjam_cad_name(i), C[i]) == 0 && strcmp(mjam_fill_name(i), FL[i]) == 0 &&
              strcmp(mjam_stray_name(i), S[i]) == 0;
    CHECK(ok, "20: form, answer, breath, cadence, fill and stray names");
    CHECK(!strcmp(mjam_time_name(-1), "HALF") && !strcmp(mjam_time_name(0), "NORMAL") && !strcmp(mjam_time_name(1), "DOUBLE") &&
          !strcmp(mjam_time_name(9), "DOUBLE") && !strcmp(mjam_time_name(-9), "HALF"), "21: TIME names HALF, NORMAL, DOUBLE (clamped)");
    CHECK(strcmp(mjam_block_name(9, 9), mjam_block_name(MJ_PEDAL, 4)) == 0 && strcmp(mjam_form_name(-1), "LOOP") == 0,
          "20: names clamp out-of-range indexes");
}

static void test_20_default_and_roles(void) {
    mjam_set_t d = mjam_default();
    CHECK(d.c == MJ_ARP && d.l == 0 && d.form == MF_LOOP && d.breath == 0 && d.cad == 3 && d.fill == 0 && d.ans == MA_ECHO &&
          d.stray == 1 && d.aoct == 0 && d.reg == 1 && d.sc == MJ_RUN && d.sl == 2, "20: mjam_default is ARP UP, LOOP, HOME, SWAP RUN-UP");
    static const char *const ROLES[MF_COUNT] = {"CCCC", "CACA", "CCAA", "CCCA", "CRAA", "CSFA"};
    static const int SRC[MF_COUNT][4] = {{0, 1, 2, 3}, {0, 0, 2, 2}, {0, 1, 0, 1}, {0, 1, 2, 0}, {0, 0, 0, 1}, {0, 0, 0, 1}};
    int ok = 1;
    for (int f = 0; f < MF_COUNT; f++)
        for (int p = 0; p < 4; p++) {
            int src = -1;
            ok &= mjam_role(f, p, &src) == ROLES[f][p] && src == SRC[f][p];
        }
    CHECK(ok, "20: mjam_role is the reference's FORMS");
}

static void pictures(int frames[MJ_COLS * MJ_LEVELS][SEQ_STEPS]) {
    mjam_loop_in_t in = {4, SCALE_MINOR, 4, mjam_default(), EM_BAR, NULL};
    for (int c = 0; c < MJ_COLS; c++)
        for (int l = 0; l < MJ_LEVELS; l++) {
            mjam_note_t line[SEQ_STEPS];
            mjam_block_bar(&in, c, l, 0, line);
            for (int i = 0; i < SEQ_STEPS; i++) frames[c * MJ_LEVELS + l][i] = mjam_frame(line, i);
        }
}

static void test_20_pictures_pinned(void) {
    int got[MJ_COLS * MJ_LEVELS][SEQ_STEPS], want[MJ_COLS * MJ_LEVELS][SEQ_STEPS], n = 0;
    FILE *f = fopen("vst/jam_pictures.txt", "r");
    pictures(got);
    if (f) {
        while (n < MJ_COLS * MJ_LEVELS * SEQ_STEPS && fscanf(f, "%d", &want[n / SEQ_STEPS][n % SEQ_STEPS]) == 1) n++;
        fclose(f);
    }
    CHECK(n == MJ_COLS * MJ_LEVELS * SEQ_STEPS, "20: vst/jam_pictures.txt has 30 lines of 16 frames (%d read)", n);
    CHECK(n == MJ_COLS * MJ_LEVELS * SEQ_STEPS && memcmp(got, want, sizeof got) == 0, "20: the JAM pictures equal vst/jam_pictures.txt");
}


/* ---- 0.21: DENSITY, RANGE, VARIATION ---- */

static mjam_set_t k21(int c, int l, int dens, int rng, int var) {
    mjam_set_t s = mjam_default();
    s.c = (signed char)c;
    s.l = (signed char)l;
    s.dens = (signed char)dens;
    s.rng = (signed char)rng;
    s.var = (signed char)var;
    return s;
}

static void bar21(mjam_set_t s, int b, mjam_note_t out[SEQ_STEPS]) {
    mjam_loop_in_t in = {4, SCALE_MINOR, 4, s, EM_BAR, NULL};
    mjam_bar(&in, b, &s, out);
}

static int starts(const mjam_note_t *l) {
    int n = 0;
    for (int i = 0; i < SEQ_STEPS; i++) n += l[i].note != 0;
    return n;
}

static int same_line(const mjam_note_t *a, const mjam_note_t *b) {
    for (int i = 0; i < SEQ_STEPS; i++) if (a[i].note != b[i].note || a[i].hold != b[i].hold) return 0;
    return 1;
}

static void test_21_neutral_is_020(void) {
    mjam_set_t z = mk(2, 3, 1, 2, 3, 1, 0, 1, 0, 1), d = z;
    mjam_note_t a[SEQ_STEPS], b[SEQ_STEPS];
    d.dens = 0; d.rng = 0; d.var = 0; d.tm = 0;
    CHECK(z.dens == 0 && z.rng == 0 && z.var == 0 && z.tm == 0 && mjam_default().dens == 0 && mjam_default().rng == 0 && mjam_default().var == 0 &&
          mjam_default().tm == 0, "21: positional sets and mjam_default leave dens, rng, var, tm at 0");
    bar21(z, 1, a);
    bar21(d, 1, b);
    CHECK(same_line(a, b), "21: a zeroed set sounds as 0.20 (the three sweeps above hold the checksums)");
}

static void test_21_thin(void) {
    mjam_note_t l[SEQ_STEPS];
    int ok1 = 1, ok2 = 1;
    bar21(k21(MJ_ARP, 0, -1, 0, 0), 0, l);
    for (int i = 0; i < SEQ_STEPS; i++) ok1 &= (l[i].note != 0) == (i % 4 == 0);
    bar21(k21(MJ_ARP, 0, -2, 0, 0), 0, l);
    for (int i = 0; i < SEQ_STEPS; i++) ok2 &= (l[i].note != 0) == (i % 8 == 0);
    CHECK(ok1, "21: THIN on ARP UP keeps steps 0, 4, 8, 12");
    CHECK(ok2, "21: SPARSE on ARP UP keeps steps 0 and 8");
}

static void test_21_filled(void) {
    mjam_note_t l[SEQ_STEPS];
    bar21(k21(MJ_ARP, 0, 1, 0, 0), 0, l);
    CHECK(starts(l) == 16, "21: FILLED on ARP UP: 16 note starts (%d)", starts(l));
    bar21(k21(MJ_ARP, 0, 2, 0, 0), 0, l);
    CHECK(starts(l) == 16, "21: BUSY on ARP UP: still 16 (%d)", starts(l));
}

static void test_21_dens_monotonic(void) {
    int bad = 0;
    for (int c = 0; c < MJ_COLS; c++) for (int l = 0; l < MJ_LEVELS; l++) for (int b = 0; b < 4; b++) {
        int prev = -1;
        for (int d = -2; d <= 2; d++) {
            mjam_note_t line[SEQ_STEPS];
            int n;
            bar21(k21(c, l, d, 0, 0), b, line);
            n = starts(line);
            if (n < prev) bad++;
            prev = n;
        }
    }
    CHECK(bad == 0, "21: note starts never fall as DENSITY rises, 30 blocks x 4 bars (%d breaks)", bad);
}

static int is_chord_tone_pc(int pc, const mjam_chord_t *ch) { return pc == ch->root || pc == ch->third || pc == ch->fifth; }

static void test_21_range(void) {
    mjam_note_t n0[SEQ_STEPS], hu[SEQ_STEPS], na[SEQ_STEPS];
    int wider = 1, beats = 1, lo, hi, nlo, nhi;
    bar21(k21(MJ_ARP, 0, 0, 0, 0), 0, n0);
    bar21(k21(MJ_ARP, 0, 0, 2, 0), 0, hu);
    for (int i = 0; i < SEQ_STEPS; i++) {
        if (hu[i].note && n0[i].note) wider &= abs((int)hu[i].note - n0[0].note) >= abs((int)n0[i].note - n0[0].note);
        if (hu[i].note && i % 4 == 0) beats &= is_chord_tone_pc(hu[i].note % 12, &EM_CH[0]);
    }
    CHECK(wider, "21: HUGE: every note is at least as far from the bar's first note as AS DRAWN");
    CHECK(hu[4].note == 79 && hu[12].note == 79, "21: HUGE: steps 4 and 12 play the 3rd an octave up (G5 79; got %d, %d)", hu[4].note, hu[12].note);
    CHECK(beats, "21: HUGE: every beat note is a chord tone");
    bar21(k21(MJ_ARP, 0, 0, -1, 0), 0, na);
    lo = hi = n0[0].note; nlo = nhi = na[0].note;
    for (int i = 0; i < SEQ_STEPS; i++) {
        if (n0[i].note && n0[i].note < lo) lo = n0[i].note;
        if (n0[i].note > hi) hi = n0[i].note;
        if (na[i].note && na[i].note < nlo) nlo = na[i].note;
        if (na[i].note > nhi) nhi = na[i].note;
    }
    CHECK(nhi - nlo < hi - lo, "21: NARROW: the bar's span %d is smaller than AS DRAWN's %d", nhi - nlo, hi - lo);
}

static void test_21_variation(void) {
    static const int WANT[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 0, 1}, {0, 1, 1, 1}};
    for (int v = 1; v <= 3; v++) {
        int ok = 1;
        for (int b = 0; b < 4; b++) {
            mjam_note_t off[SEQ_STEPS], on[SEQ_STEPS];
            bar21(k21(MJ_ARP, 0, 0, 0, 0), b, off);
            bar21(k21(MJ_ARP, 0, 0, 0, v), b, on);
            ok &= same_line(off, on) == !WANT[v][b];
        }
        CHECK(ok, "21: VARIATION %d (%s) changes exactly the bars %d%d%d%d", v, mjam_var_name(v), WANT[v][0], WANT[v][1], WANT[v][2], WANT[v][3]);
    }
}

/* the bar's beat notes are all chord tones (0.20's own blocks RUN, HOOK, LEAD ... leave some off the chord on a beat) */
static int beats_on_chord(const mjam_note_t *line, const mjam_chord_t *ch) {
    for (int i = 0; i < SEQ_STEPS; i += 4) if (line[i].note && !is_chord_tone_pc(line[i].note % 12, ch)) return 0;
    return 1;
}

/* The transforms by hand (E minor, ARP UP, non-beat steps 2 and 6, which the beat rule leaves alone; bar b's chord root degree r):
 * bar 1 (C, r -2): AS DRAWN d 0, 5 = E4 64, C5 72; a step up d 1, 6 = F#4 66, D5 74.
 * bar 2 (G, r 2): AS DRAWN d 4, 9; mirrored about the first note (d 2) d 0, -5 = E4 64, G3 55.
 * bar 3 (D, r -1): AS DRAWN d 1, 6; mirrored about the first note (d -1) d -3, -8, then a step up d -2, -7 = C4 60, E3 52. */
static void test_21_variation_transforms(void) {
    static const int WANT[3][2] = {{66, 74}, {64, 55}, {60, 52}};
    int ok = 1;
    for (int b = 1; b <= 3; b++) {
        mjam_note_t l[SEQ_STEPS];
        bar21(k21(MJ_ARP, 0, 0, 0, 3), b, l);
        ok &= l[2].note == WANT[b - 1][0] && l[6].note == WANT[b - 1][1] && l[10].note == WANT[b - 1][0] && l[14].note == WANT[b - 1][1];
    }
        CHECK(ok, "21: VARIATION ALL on ARP UP: bar 1 a step up, bar 2 mirrored, bar 3 mirrored then a step up (hand values)");
}

static void test_21_in_scale(void) {
    static const int MINOR_PC[7] = {4, 6, 7, 9, 11, 0, 2};
    int bad_scale = 0, bad_range = 0, bad_beat = 0, n = 0, checked = 0;
    for (int c = 0; c < MJ_COLS; c++) for (int l = 0; l < MJ_LEVELS; l++) for (int b = 0; b < 4; b++) {
        mjam_note_t base[SEQ_STEPS];
        mjam_set_t s0 = k21(c, l, 0, 0, 0);
        int base_ok;
        s0.cad = 0;   /* CADENCE moves a last note to a tonic degree, off the chord (as 0.20) */
        bar21(s0, b, base);
        base_ok = c != MJ_PEDAL && beats_on_chord(base, &EM_CH[b]);   /* a PEDAL note holds its scale note, as 0.20 */
        for (int d = -2; d <= 2; d++) for (int r = -1; r <= 2; r++) for (int v = 0; v <= 3; v++) for (int t = -1; t <= 1; t++) {
            mjam_set_t s = k21(c, l, d, r, v);
            mjam_note_t line[SEQ_STEPS];
            s.cad = 0;
            s.tm = (signed char)t;   /* 0.21 Amendment 1: TIME too */
            bar21(s, b, line);
            n++;
            checked += base_ok;
            for (int i = 0; i < SEQ_STEPS; i++) {
                int in_s = 0;
                if (!line[i].note) continue;
                for (int k = 0; k < 7; k++) in_s |= MINOR_PC[k] == line[i].note % 12;
                bad_scale += !in_s;
                bad_range += line[i].note < 36 || line[i].note > 100;
            }
            bad_beat += base_ok && !beats_on_chord(line, &EM_CH[b]);
        }
    }
    CHECK(n == 30 * 4 * 5 * 4 * 4 * 3, "21: in-scale sweep cases %d", n);
    CHECK(bad_scale == 0, "21: every note is in E natural minor (%d off)", bad_scale);
    CHECK(bad_range == 0, "21: every note is in 36..100 (%d off)", bad_range);
    CHECK(bad_beat == 0 && checked > 1000, "21: where 0.20's beats are chord tones, every knob keeps them so (%d off in %d cases)", bad_beat, checked);
}

static void test_21_fill_targets_played_note(void) {
    static const int BLOCKS[1][2] = {{MJ_RUN, 4}};   /* where VARIATION really moves bar 1's first note (ARP UP snaps it back) */
  for (int k = 0; k < 1; k++) {
    mjam_set_t s = k21(BLOCKS[k][0], BLOCKS[k][1], 0, 0, 3);
    mjam_note_t b0[SEQ_STEPS], b1[SEQ_STEPS];
    static const int MINOR_PC[7] = {4, 6, 7, 9, 11, 0, 2};
    int first = 0, between = 0;
    s.form = MF_1_1;   /* CACA: bar 0 is a call before an answer, a gap bar */
    s.fill = 1;        /* PICKUP: a note on step 15 */
    bar21(s, 0, b0);
    bar21(s, 1, b1);
    while (!b1[first].note) first++;
    for (int m = b0[15].note + 1; m < b1[first].note; m++) for (int k = 0; k < 7; k++) between += MINOR_PC[k] == m % 12;
    CHECK(b0[15].note && b0[15].note < b1[first].note && between == 0,
          "21: FILL PICKUP on bar 0 is the scale step below bar 1's first played note (%s: %d under %d)",
          mjam_block_name(BLOCKS[k][0], BLOCKS[k][1]), b0[15].note, b1[first].note);
  }
}

static void test_21_swing16(void) {
    static const mjam_set_t EMPTY;
    mjam_set_t up = k21(MJ_ARP, 0, 0, 0, 0), alb = k21(MJ_ARP, 3, 0, 0, 0), run = up, dens = k21(MJ_ARP, 0, 1, 0, 0);
    mjam_loop_in_t in = {4, SCALE_MINOR, 4, EMPTY, EM_BAR, NULL};
    int got[4];
    run.fill = 3;
    run.form = MF_1_1;
    mjam_set_t sets[4] = {up, alb, run, dens};
    for (int i = 0; i < 4; i++) { in.set = sets[i]; got[i] = mjam_swing16(&in); }
    CHECK(got[0] == 0 && got[1] == 1 && got[2] == 0 && got[3] == 1, "21: swing16: ARP UP 0, ALBERTI 1, UP with FILL RUN 0, UP with dens 1 is 1 (%d %d %d %d)",
          got[0], got[1], got[2], got[3]);
}

/* 0.21 the beat rule moves only the notes a knob moved or added, never a PEDAL note: on every PEDAL block under every DENSITY and
 * RANGE step, a pedal step that still sounds plays the key's 5th B4 71 (HOLD 1: the tonic E5 76); CADENCE OFF (it moves the last note) */
static void test_21_pedal_kept(void) {
    int bad = 0, seen = 0;
    for (int l = 0; l < MJ_LEVELS; l++) for (int b = 0; b < 4; b++) for (int d = -2; d <= 2; d++) for (int r = -1; r <= 2; r++) {
        mjam_set_t s = k21(MJ_PEDAL, l, d, r, 0);
        mjam_note_t line[SEQ_STEPS];
        int want = l == 1 ? 76 : 71, every = l < 2 ? 16 : l == 2 ? 4 : 2;   /* HOLD: step 0; ALT: the beats; ALT 16, CLIMB: even steps */
        s.cad = 0;
        bar21(s, b, line);
        for (int i = 0; i < SEQ_STEPS; i += every) {
            if (!line[i].note) continue;
            seen++;
            if (line[i].note != want) {
                if (bad < 6) printf("     PEDAL %s bar %d DENSITY %s RANGE %s: step %d plays %d, not %d\n", mjam_block_name(MJ_PEDAL, l), b + 1,
                                    mjam_dens_name(d), mjam_rng_name(r), i, line[i].note, want);
                bad++;
            }
        }
    }
    CHECK(bad == 0 && seen > 500, "21: PEDAL blocks keep every sounding pedal note on the 5th (HOLD 1 the tonic) under DENSITY and RANGE (%d moved of %d)",
          bad, seen);
}

/* 0.21 THIN and SPARSE only remove notes: every kept note plays the pitch it has at AS DRAWN, on all 30 blocks, 4 bars, every RANGE and
 * VARIATION step (CADENCE OFF: it moves whichever note is last) */
static void test_21_thin_keeps_pitches(void) {
    int bad = 0, n = 0;
    for (int c = 0; c < MJ_COLS; c++) for (int l = 0; l < MJ_LEVELS; l++) for (int b = 0; b < 4; b++)
        for (int r = -1; r <= 2; r++) for (int v = 0; v <= 3; v++) {
            mjam_set_t s0 = k21(c, l, 0, r, v);
            mjam_note_t base[SEQ_STEPS];
            s0.cad = 0;
            bar21(s0, b, base);
            for (int d = -2; d <= -1; d++) {
                mjam_set_t s = s0;
                mjam_note_t line[SEQ_STEPS];
                s.dens = (signed char)d;
                bar21(s, b, line);
                n++;
                for (int i = 0; i < SEQ_STEPS; i++)
                    if (line[i].note && line[i].note != base[i].note) {
                        if (bad < 6) printf("     %s bar %d RANGE %s VARIATION %s DENSITY %s: step %d plays %d, AS DRAWN %d\n",
                                            mjam_block_name(c, l), b + 1, mjam_rng_name(r), mjam_var_name(v), mjam_dens_name(d), i, line[i].note, base[i].note);
                        bad++;
                    }
            }
        }
    CHECK(bad == 0 && n == 30 * 4 * 4 * 4 * 2, "21: THIN and SPARSE never change a kept note's pitch (%d notes moved in %d cases)", bad, n);
}

/* 0.21 final review (setter cost): mjam_bar_odd's flag is mjam_swing16_bar's test from the bar's own render, and the same line as
 * mjam_bar, for every block, DENSITY step, FILL and bar; mjam_swing16 is any bar's */
static void test_21_bar_odd(void) {
    int bad = 0, n = 0;
    for (int c = 0; c < MJ_COLS; c++) for (int l = 0; l < MJ_LEVELS; l++) for (int d = -2; d <= 2; d++) for (int f = 0; f <= 3; f += 3) {
        mjam_set_t s = k21(c, l, d, 0, 0);
        mjam_loop_in_t in = {4, SCALE_MINOR, 4, s, EM_BAR, NULL};
        int any = 0;
        s.fill = (signed char)f;
        in.set = s;
        for (int b = 0; b < 4; b++) {
            mjam_note_t a[SEQ_STEPS], o[SEQ_STEPS];
            int odd = -1;
            mjam_bar(&in, b, &s, a);
            mjam_bar_odd(&in, b, &s, o, &odd);
            bad += odd != mjam_swing16_bar(&in, b) || memcmp(a, o, sizeof a) != 0;
            any |= odd;
            n++;
        }
        bad += any != mjam_swing16(&in);
    }
    CHECK(bad == 0, "21: mjam_bar_odd: the line as mjam_bar, the flag as mjam_swing16_bar (%d bad of %d bars)", bad, n);
}

/* 0.21 final review: one metric strength (DENSITY's THIN and ACCENT's velocities read it) */
static void test_21_strength(void) {
    static const int WANT[SEQ_STEPS] = {4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0};
    int ok = 1;
    for (int i = 0; i < SEQ_STEPS; i++) ok &= mjam_strength(i) == WANT[i];
    CHECK(ok, "21: mjam_strength: 4 the downbeat, 3 beat 3, 2 beats 2 and 4, 1 the other 8ths, 0 the 16ths");
}

/* ---- 0.21 Amendment 1: TIME ---- */

static mjam_set_t t21(int c, int l, int tm) {
    mjam_set_t s = k21(c, l, 0, 0, 0);
    s.tm = (signed char)tm;
    return s;
}

/* HALF: the bar's notes before step 8 at 2p, twice as long; the rest dropped. ARP UP bar 0 (Em): notes on 0, 4, 8, 12, each 4 steps,
 * playing NORMAL's steps 0, 2, 4, 6 (E4 G4 B4 E5), beat notes chord tones. A chord-tone slot resolves against the chord under its new
 * step: on a bar of Em then C from step 8, NORMAL's step 4 (B4, the 5th) moves to step 8 and plays C's 5th G4 67. Bar 4 (CADENCE HOME
 * after TIME): its last note, on step 12, lands on the tonic E */
static void test_21_time_half(void) {
    mjam_note_t n[SEQ_STEPS], h[SEQ_STEPS];
    mjam_bar_in_t split = EM_BAR[0];
    mjam_set_t s = t21(MJ_ARP, 0, -1);
    mjam_loop_in_t in = {4, SCALE_MINOR, 1, s, &split, NULL};
    int ok = 1;
    bar21(t21(MJ_ARP, 0, 0), 0, n);
    bar21(s, 0, h);
    for (int i = 0; i < SEQ_STEPS; i++) ok &= i % 4 ? !h[i].note && h[i].hold : h[i].note == n[i / 2].note;
    CHECK(ok && h[0].note == 64 && h[12].note == 76 && beats_on_chord(h, &EM_CH[0]),
          "21: TIME HALF on ARP UP: notes on 0, 4, 8, 12, 4 steps each, NORMAL's steps 0, 2, 4, 6 (%d %d %d %d), beat notes chord tones",
          h[0].note, h[4].note, h[8].note, h[12].note);
    for (int i = 8; i < SEQ_STEPS; i++) split.chord[i] = &EM_CH[1];
    s.cad = 0;
    in.set = s;
    mjam_bar(&in, 0, &s, h);
    CHECK(h[8].note == 67 && h[4].note == 67, "21: TIME HALF: the chord-tone slot moved to step 8 plays the chord there (C's 5th G4 67: %d)", h[8].note);
    bar21(t21(MJ_ARP, 0, -1), 3, h);
    CHECK(h[12].note && h[12].note % 12 == 4 && !h[13].note && !h[14].note && !h[15].note,
          "21: TIME HALF then CADENCE HOME: bar 4's last note (step 13) is the tonic E (%d)", h[12].note);
}

/* DOUBLE: the notes on even steps at p/2, max(1, len/2) long, odd-step notes dropped, the result again 8 steps later. ARP UP bar 0: a
 * note on every step, 1 step each, steps 8-15 repeating 0-7, step k NORMAL's step 2k; on the 16th swing grid (HALF and NORMAL are not).
 * Bar 4 (CADENCE HOME after TIME): the last note, step 16, lands on the tonic E */
static void test_21_time_double(void) {
    mjam_note_t n[SEQ_STEPS], d[SEQ_STEPS];
    mjam_loop_in_t in = {4, SCALE_MINOR, 4, t21(MJ_ARP, 0, 1), EM_BAR, NULL};
    int ok = 1, g[3];
    bar21(t21(MJ_ARP, 0, 0), 0, n);
    bar21(t21(MJ_ARP, 0, 1), 0, d);
    for (int i = 0; i < SEQ_STEPS; i++) ok &= d[i].note && d[i].note == n[2 * (i % 8)].note && !d[i].hold;
    CHECK(ok && beats_on_chord(d, &EM_CH[0]), "21: TIME DOUBLE on ARP UP: a 1-step note on every step, steps 8-15 repeat 0-7, step k NORMAL's 2k");
    for (int t = -1; t <= 1; t++) {
        in.set = t21(MJ_ARP, 0, t);
        g[t + 1] = mjam_swing16(&in);
    }
    CHECK(g[0] == 0 && g[1] == 0 && g[2] == 1, "21: TIME and the swing grid: ARP UP HALF %d, NORMAL %d, DOUBLE %d (16ths)", g[0], g[1], g[2]);
    bar21(t21(MJ_ARP, 0, 1), 3, d);
    CHECK(d[15].note && d[15].note % 12 == 4, "21: TIME DOUBLE then CADENCE HOME: bar 4's step 16 is the tonic E (%d)", d[15].note);
}

/* PEDAL blocks keep their pedal under HALF and DOUBLE: every pedal note TIME moves still plays the key's 5th B4 71 (HOLD 1: the tonic
 * E5 76), on all 5 PEDAL blocks and 4 bars (CADENCE OFF: it moves the last note) */
static void test_21_time_pedal(void) {
    int bad = 0, seen = 0;
    for (int l = 0; l < MJ_LEVELS; l++) for (int b = 0; b < 4; b++) for (int t = -1; t <= 1; t += 2) {
        mjam_set_t s = t21(MJ_PEDAL, l, t);
        mjam_note_t line[SEQ_STEPS];
        int want = l == 1 ? 76 : 71, every = l < 2 ? 16 : l == 2 ? 4 : 2;   /* NORMAL's pedal steps: HOLD step 0, ALT the beats, else even */
        s.cad = 0;
        bar21(s, b, line);
        for (int p = 0; p < SEQ_STEPS; p += every) {
            int at[2] = {t < 0 ? 2 * p : p / 2, t < 0 ? -1 : p / 2 + 8};
            if (t < 0 && p >= 8) continue;
            for (int k = 0; k < 2; k++) {
                if (at[k] < 0) continue;
                seen++;
                if (line[at[k]].note != want) {
                    if (bad < 6) printf("     PEDAL %s bar %d TIME %s: step %d plays %d, not %d\n", mjam_block_name(MJ_PEDAL, l), b + 1,
                                        mjam_time_name(t), at[k], line[at[k]].note, want);
                    bad++;
                }
            }
        }
    }
    CHECK(bad == 0 && seen > 200, "21: PEDAL blocks keep their pedal under TIME HALF and DOUBLE (%d moved of %d)", bad, seen);
}

int main(int argc, char **argv) {
    em_init();
    if (argc > 1 && strcmp(argv[1], "--pictures") == 0) {
        int fr[MJ_COLS * MJ_LEVELS][SEQ_STEPS];
        pictures(fr);
        for (int k = 0; k < MJ_COLS * MJ_LEVELS; k++)
            for (int i = 0; i < SEQ_STEPS; i++) printf("%d%s", fr[k][i], i < SEQ_STEPS - 1 ? " " : "\n");
        return 0;
    }
    test_20_sweep_a();
    test_20_sweep_b();
    test_20_sweep_v();
    test_20_spots();
    test_20_other_keys();
    test_20_short_loops_model();
    test_20_names();
    test_20_default_and_roles();
    test_20_pictures_pinned();
    test_21_neutral_is_020();
    test_21_thin();
    test_21_filled();
    test_21_dens_monotonic();
    test_21_range();
    test_21_variation();
    test_21_variation_transforms();
    test_21_in_scale();
    test_21_fill_targets_played_note();
    test_21_swing16();
    test_21_pedal_kept();
    test_21_thin_keeps_pitches();
    test_21_bar_odd();
    test_21_strength();
    test_21_time_half();
    test_21_time_double();
    test_21_time_pedal();
    if (fails) { printf("FAILED: %d\n", fails); return 1; }
    printf("PASSED\n");
    return 0;
}
