#include "jamlib.h"
#include "theory.h"
#include "util.h"

/* 0.22 the JAM block tables, from the owner-approved mock (exploration page rev 2.1). Settings are the matching genre's
 * styles in voicing.c / bassline.c (named in each comment); a BASS pattern's digits became hits (A where the genre accents
 * beat 1) and its role digits chose MOVEMENT / OCTAVES. Column order is the JAM grid's. First drafts, to tune by ear. */
#define C(n, k, m, co, i, s, st, o) {n, k, m, co, i, s, st, o}
#define B(n, k, p, m, o, a, r, oc) {n, k, p, m, o, a, r, oc}

static const jam_chords_t CHORDS[JAM_COLS][JAM_LEVELS] = {
    {   /* HOUSE: Disco Strings, Organ Stab, Piano House, Jackin Chords */
        C("PAD", "x-------x-------", VMODE_SMOOTH, 0, 0, 1, 0, 0),
        C("OFFBEAT", "..x...x...x...x.", VMODE_ROOTLESS, 1, 0, 0, 0, 0),
        C("M1 PIANO", "x..x..x...x..x..", VMODE_CLOSE, 1, 0, 0, 0, 1),
        C("ORGAN", ".x.x.x.x.x.x.x.x", VMODE_ROOTLESS, 1, 0, 0, 0, 0),
        C("JACKIN", "x.xx.x.xx.x.xx.x", VMODE_CLOSE, 0, 1, 0, 0, 1),
    }, {   /* TECHNO: Hypnotic, Dub Techno, Dark Stab, Acid Chord */
        C("DRONE 5TH", "x---------------", VMODE_CLOSE, -1, 0, 1, 0, 0),
        C("DUB STAB", "..x.....x.x.....", VMODE_ROOTLESS, 1, 0, 0, 0, 0),
        C("OFFBEAT", "..x...x...x...x.", VMODE_ROOTLESS, 1, 0, 0, 0, 0),
        C("3+3+2", "x..x..x.x..x..x.", VMODE_CLOSE, 0, 0, -1, 0, 0),
        C("ROLLING", ".xxx.xxx.xxx.xxx", VMODE_CLOSE, -1, 0, 0, 0, -1),
    }, {   /* TRANCE: Uplifting, Pluck, Supersaw Stab, Gated Pad (GATE and GATED 16 swapped: GATED 16 is the busiest) */
        C("WIDE PAD", "x-------x-------", VMODE_SMOOTH, 2, 0, 3, 0, 0),
        C("PLUCK 8S", "x.x.x.x.x.x.x.x.", VMODE_TOPLINE, 0, 0, 0, 0, 1),
        C("OFFBEAT", "..x-..x-..x-..x-", VMODE_CLOSE, 0, 1, 0, 0, 1),
        C("GATE", "x.xxx.xx.xx.x.xx", VMODE_SMOOTH, 0, 0, 1, 0, 0),
        C("GATED 16", "xxxxxxxxxxxxxxxx", VMODE_SMOOTH, 0, 0, 1, 0, 0),
    }, {   /* POP: Ballad, Pop Piano, Synth Pop */
        C("WHOLE", "A---------------", VMODE_SMOOTH, 2, 0, 2, 0, 0),
        C("HALVES", "A-------x-------", VMODE_SMOOTH, 0, 0, 0, 0, 0),
        C("PIANO 4S", "A---x---x---x---", VMODE_SMOOTH, 0, 0, 0, 0, 0),
        C("SYNCO", "A..x..x...x.x...", VMODE_CLOSE, 0, 0, 0, 0, 1),
        C("DRIVE 8S", "A.x.x.x.x.x.x.x.", VMODE_CLOSE, 0, 0, 0, 0, 1),
    }, {   /* ROCK: Guitar Strum, close power voicings (GALLOP and STRUM 16 swapped: GALLOP is the busiest) */
        C("RING OUT", "A---------------", VMODE_CLOSE, 0, 0, 1, 4, 0),
        C("BACKBEAT", "....x.......x...", VMODE_CLOSE, -1, 0, 0, 0, 0),
        C("CHUG 8S", "A.x.x.x.x.x.x.x.", VMODE_CLOSE, -1, 0, 0, 0, 0),
        C("STRUM 16", "A.xx.xx.x.xx.xx.", VMODE_CLOSE, 0, 0, 1, 4, 0),
        C("GALLOP", "A.xxx.xxx.xxx.xx", VMODE_CLOSE, -1, 0, 0, 0, 0),
    }, {   /* DISCO: Disco Strings, Organ Stab, Piano House (CHOP and SYNCO swapped: CHOP is the busiest) */
        C("STRINGS", "A-------x-------", VMODE_SMOOTH, 0, 0, 1, 0, 0),
        C("FLOOR", "A...x...x...x...", VMODE_ROOTLESS, 1, 0, 0, 0, 0),
        C("ORGAN", "..x...x...x...x.", VMODE_ROOTLESS, 1, 0, 0, 0, 0),
        C("SYNCO", "A..x..x..x..x.x.", VMODE_CLOSE, 1, 0, 0, 0, 1),
        C("CHOP", ".x.x.x.x.x.x.x.x", VMODE_ROOTLESS, 1, 0, 0, 0, 0),
    },
};

static const jam_bass_t BASS[JAM_COLS][JAM_LEVELS] = {
    {   /* HOUSE: House Root, Octave Bounce, Organ Fifths, Disco Walk */
        B("OFFBEAT", "..x...x...x...x.", 0, 0, 0, 0, 0, 0),
        B("BOUNCE", "..x-..x-..x-..x-", 0, 0, 2, 0, 0, 0),
        B("5THS", "x..x..x.x..x..x.", 0, 2, 0, 0, 0, 0),
        B("WALK", "x.x.x.x.x.x.x.x.", 0, 3, 1, 1, 0, 0),
        B("GALLOP", ".xx..xx..xx..xx.", 0, 0, 0, 0, 0, 0),
    }, {   /* TECHNO: Pedal Drone, Root Drone, Driving Root, Hypnotic Fifths, Acid Jumps (ACID's 15th step 1: it must be the busiest) */
        B("SUB DRONE", "x---------------", 1, 0, 0, 0, -1, 0),
        B("OFFBEAT", "..x...x...x...x.", 0, 0, 0, 0, -1, 0),
        B("ROLLING", ".xxx.xxx.xxx.xxx", 0, 0, 0, 0, 0, 0),
        B("ROLL 5TH", ".xxx.xxx.xxx.xxx", 0, 2, 0, 0, -1, 0),
        B("ACID", "xx.x.xxxx.x.xxxx", 0, 4, 3, 3, 0, 0),
    }, {   /* TRANCE: Pedal Pump, Driving Root, Uplifting Octaves */
        B("PEDAL", "x---x---x---x---", 1, 0, 0, 0, 0, 0),
        B("OFFBEAT", "..x...x...x...x.", 0, 0, 0, 0, 0, 0),
        B("GALLOP", ".xx..xx..xx..xx.", 0, 0, 0, 0, 0, 0),
        B("OCT RUN", ".xx..xx..xx..xx.", 0, 0, 3, 0, 0, 0),
        B("RISE", ".xx.xxx.xxx.xxxx", 0, 2, 3, 0, 0, 0),
    }, {   /* POP: Pop Root, Rock Fifths, Motown Walk */
        B("ROOT", "A---------------", 0, 0, 0, 0, 0, 0),
        B("ROOT 4S", "A---x---x---x---", 0, 0, 0, 0, 0, 0),
        B("ROOT 8S", "A.x.x.x.x.x.x.x.", 0, 0, 0, 0, 0, 0),
        B("ROOT-5", "A---x---x---x-x.", 0, 2, 0, 0, 0, 0),
        B("WALK UP", "A.x.x.x.x.x.x.x.", 0, 3, 0, 1, 0, 0),
    }, {   /* ROCK: Long Roots, Pop Root, Rock Fifths, Ostinato (RIFF and GALLOP swapped: GALLOP is the busiest) */
        B("ROOT HOLD", "A-------x-------", 0, 0, 0, 0, -1, 0),
        B("DRIVE 8S", "A.x.x.x.x.x.x.x.", 0, 0, 0, 0, 0, 0),
        B("R-5-OCT", "A.x.x.x.x.x.x.x.", 0, 2, 2, 0, 0, 0),
        B("RIFF", "A..xx.x..x.xx.xx", 0, 2, 1, 0, 0, 0),
        B("GALLOP", "A.xxx.xxx.xxx.xx", 0, 1, 0, 0, 0, 0),
    }, {   /* DISCO: Pop Root, Disco Octaves, Motown Walk, Funk Thirds, Slap Octaves (MOTOWN gets one extra hit, step 14, so ROOT 8S is not denser) */
        B("ROOT 8S", "A.x.x.x.x.x.x.x.", 0, 0, 0, 0, 0, 0),
        B("OCTAVES", "A.x.x.x.x.x.x.x.", 0, 0, 2, 0, 0, 0),
        B("MOTOWN", "A..x.x..x.x.x.xx", 0, 3, 1, 1, 0, 0),
        B("FUNK 3RDS", "A.xx.x.xx.x.x.xx", 0, 3, 1, 0, 0, 0),
        B("SLAP", "A.xx.x.xx.xx.xx.", 0, 0, 4, 0, 0, 0),
    },
};

static const char *const GENRE[JAM_COLS] = {"HOUSE", "TECHNO", "TRANCE", "POP", "ROCK", "DISCO"};

const char *jam_genre_name(int c) { return GENRE[clampi(c, 0, JAM_COLS - 1)]; }
const jam_chords_t *jam_chords(int c, int l) { return &CHORDS[clampi(c, 0, JAM_COLS - 1)][clampi(l, 0, JAM_LEVELS - 1)]; }
const jam_bass_t *jam_bass(int c, int l) { return &BASS[clampi(c, 0, JAM_COLS - 1)][clampi(l, 0, JAM_LEVELS - 1)]; }
const char *jam_block_name(int lane, int c, int l) { return lane == JL_BASS ? jam_bass(c, l)->name : jam_chords(c, l)->name; }
const char *jam_block_codes(int lane, int c, int l) { return lane == JL_BASS ? jam_bass(c, l)->codes : jam_chords(c, l)->codes; }

/* 0.22 the BASS pictures' height window: MIDI 24 (C1) .. 59 over 12 levels, round half up of (m - 24) 11 / 35, so the bass register and
 * octave jumps stay visible (melody2.c's window starts at 55 and would put every bass note at 0). Only the tests call this; the skin reads
 * vst/jam_pictures_cb.txt, which holds the same window. */
int jam_bass_level(int midi) {
    int v = (22 * (midi - 24) + 35) / 70;
    return midi <= 24 ? 0 : v > 11 ? 11 : v;
}
