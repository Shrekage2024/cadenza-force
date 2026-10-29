#include "bassline.h"
#include "rhythm.h"
#include "util.h"

/* The 48 styles, 4 per genre, in the BASS page's order. Settings: pedal, MOVEMENT, OCTAVES, APPROACH, REGISTER, octave. */
static const struct { const char *name, *feel; signed char pedal, mov, octs, app, reg, octave; } STYLE[BASS_STYLE_COUNT] = {
    {"House Root", "roots, nothing else", 0, 0, 0, 0, 0, 0},
    {"Octave Bounce", "root, then an octave up", 0, 0, 2, 0, 0, 0},
    {"Disco Walk", "walks into each change", 0, 3, 1, 1, 0, 0},
    {"Organ Fifths", "root and 5th", 0, 2, 0, 0, 0, 0},
    {"Deep Sub", "low roots", 0, 0, 0, 0, -1, 0},
    {"Dub Fifths", "low root and 5th", 0, 2, 0, 0, -1, 0},
    {"Jazzy Walk", "passing notes, half steps in", 0, 4, 0, 2, 0, 0},
    {"Warm Thirds", "root, 3rd and 5th", 0, 3, 0, 0, 0, 0},
    {"Root Drone", "low roots only", 0, 0, 0, 0, -1, 0},
    {"Pedal Drone", "one low note under it all", 1, 0, 0, 0, -1, 0},
    {"Acid Jumps", "octaves and half steps", 0, 4, 3, 3, 0, 0},
    {"Hypnotic Fifths", "root and 5th, low", 0, 2, 0, 0, -1, 0},
    {"Driving Root", "roots, natural register", 0, 0, 0, 0, 0, 0},
    {"Pedal Pump", "the key note, chords move above", 1, 0, 0, 0, 0, 0},
    {"Uplifting Octaves", "octaves on most notes", 0, 0, 3, 0, 0, 0},
    {"Dominant Rise", "the 5th held: it builds", 2, 0, 0, 0, 0, 0},
    {"Reese Sub", "low, dark roots", 0, 0, 0, 0, -2, 0},
    {"Liquid Walk", "smooth steps to the change", 0, 3, 0, 1, 0, 0},
    {"Jump Up", "octave jumps", 0, 1, 2, 0, 0, 0},
    {"Roller Fifths", "root and 5th, a few octaves", 0, 2, 1, 0, 0, 0},
    {"808 Root", "low roots for the 808", 0, 0, 0, 0, -1, 0},
    {"808 Octaves", "808 with octave jumps", 0, 0, 1, 0, -1, 0},
    {"Dark Pedal", "the 808 stays on the key", 1, 0, 0, 0, -1, 0},
    {"Melodic 808", "808 that follows the chords", 0, 2, 1, 1, -1, 0},
    {"Drill 808", "low, a little movement", 0, 1, 1, 0, -1, 0},
    {"UK Drill", "half steps into the change", 0, 2, 0, 2, -1, 0},
    {"Bounce", "octave jumps, low", 0, 0, 2, 0, -1, 0},
    {"Dark Slide", "creeps up a half step", 0, 1, 0, 3, -1, 0},
    {"Dusty Root", "roots, a 5th now and then", 0, 1, 0, 0, 0, 0},
    {"Upright Walk", "jazz walking line", 0, 4, 0, 1, 0, 0},
    {"Sample Fifths", "root, 5th and octaves", 0, 2, 1, 0, 0, 0},
    {"Two Feel", "root and 5th", 0, 2, 0, 0, 0, 0},
    {"Neo-soul Walk", "surrounds each change", 0, 3, 0, 4, 0, 0},
    {"Gospel Pedal", "the 5th under moving chords", 2, 0, 0, 0, 0, 0},
    {"90s R&B", "root and 5th, steps in", 0, 2, 0, 1, 0, 0},
    {"Slow Jam", "roots with octave pickups", 0, 1, 2, 0, 0, 0},
    {"Funk Thirds", "busy: 3rds and 5ths", 0, 3, 1, 0, 0, 0},
    {"Slap Octaves", "root and octave pops", 0, 0, 4, 0, 0, 0},
    {"Disco Octaves", "every other note up", 0, 0, 2, 0, 0, 0},
    {"Motown Walk", "melodic, walks between", 0, 3, 1, 1, 0, 0},
    {"Pop Root", "roots, nothing else", 0, 0, 0, 0, 0, 0},
    {"Pop Octaves", "an octave now and then", 0, 1, 1, 0, 0, 0},
    {"Rock Fifths", "root and 5th", 0, 2, 0, 0, 0, 0},
    {"Ballad Walk", "steps between chords", 0, 2, 0, 1, 0, 0},
    {"Tonic Pedal", "the key note held: settled", 1, 0, 0, 0, -1, 0},
    {"Dominant Pedal", "the 5th held: tension", 2, 0, 0, 0, -1, 0},
    {"Ostinato", "a repeating figure", 0, 2, 1, 0, 0, 0},
    {"Long Roots", "low roots", 0, 0, 0, 0, -1, 0},
};

static const char *const GENRE[BASS_GENRES] = {"House", "Deep House", "Techno", "Trance", "Drum & Bass", "Trap", "Drill",
                                               "Boom Bap", "R&B / Soul", "Funk / Disco", "Pop / Rock", "Film / Ambient"};

const char *bass_style_name(int i) { return STYLE[i].name; }
const char *bass_style_feel(int i) { return STYLE[i].feel; }
int bass_style_genre(int i) { return i / 4; }
const char *bass_genre_name(int g) { return GENRE[g]; }

bass_set_t bass_style_set(int i) {
    bass_set_t s;
    s.style = (signed char)i;
    s.pedal = STYLE[i].pedal;
    s.octave = STYLE[i].octave;
    s.k[BK_MOV] = STYLE[i].mov;
    s.k[BK_OCTS] = STYLE[i].octs;
    s.k[BK_APP] = STYLE[i].app;
    s.k[BK_REG] = STYLE[i].reg;
    return s;
}

int bass_set_edited(const bass_set_t *s) {
    if (s->style < 0) return 0;
    bass_set_t o = bass_style_set(s->style);
    if (s->pedal != o.pedal || s->octave != o.octave) return 1;
    for (int j = 0; j < BK_COUNT; j++) if (s->k[j] != o.k[j]) return 1;
    return 0;
}


static int in_scale(const bass_key_t *k, int pc) {
    for (int i = 0; i < 7; i++) if (k->scale[i] == pc) return 1;
    return 0;
}

int bass_scale_below(const bass_key_t *k, int pc) {
    for (int d = 1; d < 12; d++) if (in_scale(k, pmod(pc - d, 12))) return pmod(pc - d, 12);
    return pmod(pc - 1, 12);
}

int bass_scale_above(const bass_key_t *k, int pc) {
    for (int d = 1; d < 12; d++) if (in_scale(k, (pc + d) % 12)) return (pc + d) % 12;
    return (pc + 1) % 12;
}

/* MOVEMENT's role cycles: R root, 3, 5, p (the scale note below the next chord's root) */
static const struct { int len; char r[4]; } ROLES[5] = {
    {1, {'R'}}, {4, {'R', 'R', 'R', '5'}}, {2, {'R', '5'}}, {4, {'R', '3', '5', '3'}}, {4, {'R', '3', '5', 'p'}}};
static const int REG_FOLD[5] = {0, 2, 4, 7, 9};   /* the fold point F for REGISTER -2..2: C D E G A */

static int oct_up(int octs, int h) {
    switch (octs) {
    case 1: return h % 4 == 3;
    case 2: return h % 2 == 1;
    case 3: return h % 4 != 0;
    case 4: return h > 0;
    default: return 0;
    }
}

static unsigned char midi_of(const bass_set_t *s, int pc, int up) {
    int m = 24 + pc + (pc < REG_FOLD[s->k[BK_REG] + 2] ? 12 : 0) + 12 * s->octave + (up ? 12 : 0);
    return (unsigned char)(m < 1 ? 1 : m > 127 ? 127 : m);
}

/* APPROACH on the span's last hit L and the one before it L2 (positions in steps; the limits are span * 10 / 16 and
 * span * 8 / 16, compared in integers) */
static void approach(const bass_span_in_t *in, const int *hits, int nh, unsigned char note[], char role[]) {
    const bass_chord_t *ch = in->chord, *nx = in->next;
    int app = in->set.k[BK_APP], L = hits[nh - 1], L2 = hits[nh - 2], t = nx->root;
    if (!app || in->set.pedal || nh < 2 || nx->root == ch->root || 16 * L < 10 * in->span) return;
    if (app == 1) { note[L] = midi_of(&in->set, t > ch->root ? bass_scale_below(in->key, t) : bass_scale_above(in->key, t), 0); role[L] = 'p'; }
    if (app == 2 || app == 4) { note[L] = midi_of(&in->set, (t + 11) % 12, 0); role[L] = 'c'; }
    if (app == 3) { note[L] = midi_of(&in->set, (t + 1) % 12, 0); role[L] = 'c'; }
    if (app == 4 && 16 * L2 >= 8 * in->span) { note[L2] = midi_of(&in->set, (t + 1) % 12, 0); role[L2] = 'c'; }
}

void bass_span(const bass_span_in_t *in, unsigned char note[], char role[]) {
    const bass_set_t *s = &in->set;
    const bass_chord_t *ch = in->chord;
    int hits[16], nh = 0;
    int ped = s->pedal == 1 ? in->key->tonic : s->pedal == 2 ? in->key->scale[4] : -1;
    for (int i = 0; i < in->span; i++) {
        note[i] = 0;
        role[i] = in->codes[i] == RC_HOLD ? '-' : '.';
        if (rhythm_is_hit(in->codes[i])) hits[nh++] = i;
    }
    for (int h = 0; h < nh; h++) {
        char r = h == 0 ? 'R' : ROLES[s->k[BK_MOV]].r[(h + in->v) % ROLES[s->k[BK_MOV]].len];
        int up = oct_up(s->k[BK_OCTS], h) != (in->v % 2 == 1 && h == 1), pc;
        if (ped >= 0) { r = 'P'; pc = ped; }
        else if (r == '5') pc = (ch->root + 7) % 12;
        else if (r == '3') pc = ch->third;
        else if (r == 'p') pc = bass_scale_below(in->key, in->next->root);
        else pc = ch->root;
        note[hits[h]] = midi_of(s, pc, up && r != 'p');
        role[hits[h]] = r == 'R' ? (up ? 'o' : '1') : r == 'P' ? (up ? 'o' : 'P') : r;
    }
    if (nh > 1) approach(in, hits, nh, note, role);
    if (in->fix)
        for (int i = 0; i < in->span; i++)
            if (note[i] && in->fix[i]) { note[i] = in->fix[i] > 127 ? 127 : in->fix[i]; role[i] = 'f'; }
}

int bass_root_note(const bass_set_t *s, int pc) { return midi_of(s, pmod(pc, 12), 0); }

void bass_fill_notes(int f, int to, unsigned char note[16]) {
    static const signed char OFF[4][4] = {{0}, {-1}, {-3, -2, -1}, {-7, -5, -3, -1}}, N[4] = {0, 1, 3, 4};
    if (f < 1 || f > 3) return;
    for (int i = 0; i < N[f]; i++) {
        int m = to + OFF[f][i];
        note[16 - N[f] + i] = (unsigned char)(m < 1 ? 1 : m > 127 ? 127 : m);
    }
}

int bass_tension(int pc, const bass_chord_t *c, const bass_key_t *k) {
    int iv = pmod(pc - c->root, 12), third = c->minor ? 3 : 4;
    if (iv == 0 || iv == 7) return 0;
    if (iv == third) return 8;
    if (!in_scale(k, pc)) return iv == 1 || iv == 6 ? 85 : 60;
    switch (iv) {
    case 2: return 30;
    case 5: return c->minor ? 28 : 42;
    case 9: return 18;
    case 10: return c->minor ? 24 : 36;
    case 11: return 48;
    case 8: return 55;
    case 3: case 4: return 70;
    case 1: return 85;
    case 6: return 80;
    default: return 30;
    }
}

int bass_score(const unsigned char note[], int n, const bass_chord_t *const chord_of_step[], const bass_key_t *k) {
    int max = 0, sum = 0, cnt = 0;
    for (int i = 0; i < n; i++) {
        if (!note[i]) continue;
        int t = bass_tension(note[i] % 12, chord_of_step[i], k);
        if (t > max) max = t;
        sum += t;
        cnt++;
    }
    if (!cnt) return 0;
    return (6 * max * cnt + 4 * sum + 5 * cnt) / (10 * cnt);   /* round half up of 0.6 max + 0.4 mean */
}

int bass_level(int score) { return score >= 72 ? 4 : score >= 50 ? 3 : score >= 28 ? 2 : score >= 12 ? 1 : 0; }

const char *bass_level_word(int level) {
    static const char *const W[5] = {"plain", "warm", "colourful", "tense", "clash"};
    return W[level < 0 ? 0 : level > 4 ? 4 : level];
}
