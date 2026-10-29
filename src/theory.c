#include "theory.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

static const char *NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const int SCALE[SCALE_COUNT][7] = {{0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10},
                                         {0, 2, 4, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 6, 7, 9, 11},
                                         {0, 2, 3, 5, 7, 8, 11}};
static const char *SCALE_NAME[SCALE_COUNT] = {"Major", "Minor", "Dorian", "Mixolydian", "Phrygian", "Lydian", "Harm. minor"};

const theory_preset_t THEORY_PRESETS[] = {
    {"1-6-3-7 Epic", 4, {0, 5, 2, 6}},
    {"1-5-6-4 Pop", 4, {0, 4, 5, 3}},
    {"6-4-1-5 Sad Pop", 4, {5, 3, 0, 4}},
    {"1-6-4-5 50s", 4, {0, 5, 3, 4}},
    {"1-4-5-1 Classic", 4, {0, 3, 4, 0}},
    {"1-4-1-5 Blues", 4, {0, 3, 0, 4}},
    {"1-7-6-5 Andalusian", 4, {0, 6, 5, 4}},
    {"1-7-6-7 Rock", 4, {0, 6, 5, 6}},
    {"4-5-3-6 Royal Road", 4, {3, 4, 2, 5}},
    {"1-4-6-5 Lift", 4, {0, 3, 5, 4}},
    {"2-5-1-1 Jazz", 4, {1, 4, 0, 0}},
    {"1-3-4-4 Dream", 4, {0, 2, 3, 3}},
    {"1-4 Vamp", 2, {0, 3}},
    {"1-2 Float", 2, {0, 1}},
    {"1 Drone", 1, {0}},
    {"Canon (8)", 8, {0, 4, 5, 2, 3, 0, 3, 4}},
};
const int THEORY_NPRESETS = (int)(sizeof THEORY_PRESETS / sizeof THEORY_PRESETS[0]);

/* 0.18 perf: snprintf(buf, len, "%s%s%s%s%s", a, b, c, d, e) without the format parser (0.2 us a call on the Force, and the
 * setters' labels name every slot several times): the same text, cut to len - 1 bytes and NUL-ended when len > 0, and the
 * same return, the whole text's length. NULL pieces are left out. */
static int cat5(char *buf, int len, const char *a, const char *b, const char *c, const char *d, const char *e) {
    const char *piece[5] = {a, b, c, d, e};
    int n = 0;
    for (int k = 0; k < 5; k++)
        for (const char *t = piece[k]; t && *t; t++, n++)
            if (n < len - 1) buf[n] = *t;
    if (len > 0) buf[n < len - 1 ? n : len - 1] = 0;
    return n;
}
static int cat4(char *buf, int len, const char *a, const char *b, const char *c, const char *d) { return cat5(buf, len, a, b, c, d, NULL); }
static int cat2(char *buf, int len, const char *a, const char *b) { return cat5(buf, len, a, b, NULL, NULL, NULL); }

int theory_degree_pc(int key, int scale, int deg) { return (key + SCALE[scale][pmod(deg, 7)]) % 12; }

int theory_tone_note(int key, int scale, int deg, int tone, int octave) {
    int k = tone - 1, root = theory_degree_pc(key, scale, deg);
    int above = pmod(theory_degree_pc(key, scale, deg + k) - root, 12) + 12 * (k / 7);
    return 48 + 12 * octave + root + above;
}

/* the triad quality: 0 major, 1 minor, 2 diminished, 3 augmented */
static int triad_quality(int key, int scale, int deg) {
    int root = theory_degree_pc(key, scale, deg);
    int third = pmod(theory_degree_pc(key, scale, deg + 2) - root, 12);
    int fifth = pmod(theory_degree_pc(key, scale, deg + 4) - root, 12);
    return fifth == 6 ? 2 : third == 4 && fifth == 8 ? 3 : third == 3 ? 1 : 0;
}

int theory_chord_name(int key, int scale, int deg, char *buf, int len) {
    static const char *const QUALITY[4] = {"", "m", "dim", "+"};
    return cat2(buf, len, NOTE[theory_degree_pc(key, scale, deg)], QUALITY[triad_quality(key, scale, deg)]);
}

int theory_note_name(int note, char *buf, int len) {
    char oct[12];
    int o = note / 12 - 1, k = (int)sizeof oct - 1;
    unsigned u = o < 0 ? 0u - (unsigned)o : (unsigned)o;
    oct[k] = 0;
    do oct[--k] = (char)('0' + u % 10); while ((u /= 10) > 0);
    if (o < 0) oct[--k] = '-';
    return cat2(buf, len, NOTE[note % 12], oct + k);
}

const char *theory_key_name(int key) { return NOTE[pmod(key, 12)]; }

const char *theory_scale_name(int scale) { return SCALE_NAME[scale]; }

void theory_triad(int key, int scale, int deg, int out[3]) {
    out[0] = theory_tone_note(key, scale, deg, TONE_R, 0);
    out[1] = theory_tone_note(key, scale, deg, TONE_3, 0);
    out[2] = theory_tone_note(key, scale, deg, TONE_5, 0);
}

static int voice_cost(const int v[3], const int prev[3]) {
    int i, sum = 0;
    for (i = 0; i < 3; i++) sum += v[i] > prev[i] ? v[i] - prev[i] : prev[i] - v[i];
    return sum;
}

void theory_voice_next(int key, int scale, int deg, const int prev[3], int out[3]) {
    int root[3], inv, shift, i, best = -1, cost, cand[3];
    theory_triad(key, scale, deg, root);
    for (inv = 0; inv < 3; inv++)
        for (shift = -12; shift <= 12; shift += 12) {
            for (i = 0; i < 3; i++) cand[i] = root[(inv + i) % 3] + (inv + i >= 3 ? 12 : 0) + shift;
            if (cand[0] < 43 || cand[0] > 62) continue;
            cost = voice_cost(cand, prev);
            if (best < 0 || cost < best || (cost == best && cand[0] < out[0])) {
                best = cost;
                memcpy(out, cand, 3 * sizeof(int));
            }
        }
    if (best < 0) memcpy(out, root, 3 * sizeof(int));   /* unreachable: every triad has a voicing in range */
}

const char *const THEORY_COLOUR_NAME[COL_COUNT] = {"Triad", "7th", "sus2", "sus4", "add9", "Power", "9th", "11th", "13th", "6th"};

/* Each project scale's three borrowed chords: {scale, degree}, left to right on the PHRASE chord picker's borrow row (cqr_) */
static const unsigned char BORROW[SCALE_COUNT][THEORY_NBORROW][2] = {
    {{SCALE_MINOR, 3}, {SCALE_MINOR, 5}, {SCALE_MINOR, 6}},       /* Major: iv bVI bVII */
    {{SCALE_MAJOR, 3}, {SCALE_MAJOR, 4}, {SCALE_PHRYGIAN, 1}},    /* Minor: IV V bII */
    {{SCALE_MINOR, 3}, {SCALE_MAJOR, 4}, {SCALE_MINOR, 5}},       /* Dorian: iv V bVI */
    {{SCALE_MINOR, 3}, {SCALE_MINOR, 5}, {SCALE_MAJOR, 4}},       /* Mixolydian: iv bVI V */
    {{SCALE_MAJOR, 3}, {SCALE_MAJOR, 4}, {SCALE_LYDIAN, 1}},      /* Phrygian: IV V #II */
    {{SCALE_MAJOR, 3}, {SCALE_MINOR, 3}, {SCALE_MINOR, 6}},       /* Lydian: bIV biv bVII (a semitone under #4) */
    {{SCALE_MAJOR, 3}, {SCALE_PHRYGIAN, 1}, {SCALE_MINOR, 6}}};   /* Harm. minor: IV bII bVII */

int theory_borrow_scale(int scale, int b) { return BORROW[scale][b - 1][0]; }
int theory_borrow_degree(int scale, int b) { return BORROW[scale][b - 1][1]; }
int theory_slot_scale(int scale, int src) { return src ? theory_borrow_scale(scale, src) : scale; }

int theory_colour_tone(int colour, int tone) {
    if (tone != TONE_3) return tone;
    return colour == COL_SUS2 ? TONE_2 : colour == COL_SUS4 ? TONE_4 : tone;
}

/* Each colour's tones, 0-ended (sus2 / sus4 swap the 3 in theory_colour_tone) */
static const signed char COLOUR_TONES[COL_COUNT][6] = {
    {1, 3, 5}, {1, 3, 5, 7}, {1, 3, 5}, {1, 3, 5}, {1, 3, 5, 9}, {1, 5}, {1, 3, 5, 7, 9}, {1, 3, 7, 9, 11}, {1, 3, 7, 9, 13}, {1, 3, 5, 6}};

int theory_chord_notes(int key, int scale, int deg, int colour, int out[THEORY_CHORD_MAX]) {
    const signed char *t = COLOUR_TONES[colour >= 0 && colour < COL_COUNT ? colour : COL_TRIAD];
    int n = 0;
    for (; t[n]; n++) out[n] = theory_tone_note(key, scale, deg, theory_colour_tone(colour, t[n]), 0);
    return n;
}

int theory_colour_of_level(int level) {
    static const unsigned char COLOUR[6] = {COL_POWER, COL_TRIAD, COL_7TH, COL_9TH, COL_11TH, COL_13TH};
    return level >= -1 && level <= 4 ? COLOUR[level + 1] : COL_TRIAD;
}

static int is_ext(int colour) { return colour >= COL_9TH && colour <= COL_13TH; }
static const char *const EXT_NUM[COL_COUNT] = {[COL_9TH] = "9", [COL_11TH] = "11", [COL_13TH] = "13"};

int theory_chord_name_c(int key, int scale, int deg, int colour, char *buf, int len) {
    static const char *const SEV[4][2] = {{"7", "maj7"}, {"m7", "mMaj7"}, {"dim7", "m7b5"}, {"+7", "+maj7"}};
    static const char *const XPRE[4][2] = {{"", "maj"}, {"m", "mMaj"}, {"dim", "m"}, {"+", "+maj"}};   /* SEV's, before the number */
    int root = theory_degree_pc(key, scale, deg), q, sev, n;
    if (colour == COL_SUS2 || colour == COL_SUS4) return cat2(buf, len, NOTE[root], THEORY_COLOUR_NAME[colour]);
    if (colour == COL_POWER) return cat2(buf, len, NOTE[root], "5");
    if (colour != COL_7TH && !is_ext(colour)) {
        n = theory_chord_name(key, scale, deg, buf, len);
        return (colour == COL_ADD9 || colour == COL_6TH) && n < len ? n + cat2(buf + n, len - n, colour == COL_6TH ? "6" : "add9", NULL) : n;
    }
    q = triad_quality(key, scale, deg);
    sev = pmod(theory_degree_pc(key, scale, deg + 6) - root, 12);   /* 9, 10 or 11 in every scale here */
    if (is_ext(colour))   /* the 7th's name with its 7 replaced: Em9, Cmaj13, D11, F#m9b5 */
        return cat4(buf, len, NOTE[root], XPRE[q][q == 2 ? sev == 10 : sev == 11], EXT_NUM[colour], q == 2 && sev == 10 ? "b5" : "");
    /* index 1: major 7th, except on dim where index 1 is the minor 7th (half-diminished) */
    return cat2(buf, len, NOTE[root], SEV[q][q == 2 ? sev == 10 : sev == 11]);
}

int theory_numeral(int key, int scale, int slot_scale, int deg, char *buf, int len) {
    static const char *const UP[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    static const char *const LO[7] = {"i", "ii", "iii", "iv", "v", "vi", "vii"};
    int d = pmod(deg, 7), q = triad_quality(key, slot_scale, d);
    int diff = pmod(theory_degree_pc(key, slot_scale, d) - theory_degree_pc(key, scale, d), 12);
    return cat4(buf, len, diff == 11 ? "b" : diff == 1 ? "#" : "", q == 0 || q == 3 ? UP[d] : LO[d], q == 2 ? THEORY_DIM : q == 3 ? "+" : "",
                NULL);
}

int theory_text_width(const char *text) {
    int n = 0;
    for (; *text; text++) n += ((unsigned char)*text & 0xC0) != 0x80;   /* UTF-8: continuation bytes are not characters */
    return n;
}

/* The numeral's length without its quality mark: the dim sign (2 bytes) or the + */
static int numeral_stem(int q, int n) { return q == 2 ? n - 2 : q == 3 ? n - 1 : n; }

int theory_degree_c(int key, int scale, int slot_scale, int deg, int colour, char *buf, int len) {
    /* the 7th's suffix by triad quality (major, minor, dim, aug), as theory_chord_name_c's SEV: [1] the major 7th (on dim, the
     * minor 7th); the numeral's case already says minor, and its dim sign or + goes into the suffix */
    static const char *const SEV[4][2] = {{"7", "maj7"}, {"7", "maj7"}, {THEORY_DIM "7", "m7b5"}, {"+7", "+maj7"}};
    char num[12];
    static const char *const XPRE[4][2] = {{"", "maj"}, {"", "maj"}, {THEORY_DIM, "m"}, {"+", "+maj"}};   /* SEV's, before the number */
    int d = pmod(deg, 7), q = triad_quality(key, slot_scale, d), n = theory_numeral(key, scale, slot_scale, d, num, sizeof num);
    if (colour == COL_TRIAD || colour >= COL_COUNT || colour < 0) return cat2(buf, len, num, NULL);
    if (colour == COL_POWER || colour == COL_6TH) return cat2(buf, len, num, colour == COL_POWER ? "5" : "6");   /* "i5", "IV6" */
    if (colour != COL_7TH && !is_ext(colour)) return cat4(buf, len, num, " ", THEORY_COLOUR_NAME[colour], NULL);   /* "vi\xC2\xB0 sus4", "IV add9" */
    num[numeral_stem(q, n)] = 0;
    int sev = pmod(theory_degree_pc(key, slot_scale, d + 6) - theory_degree_pc(key, slot_scale, d), 12);
    if (is_ext(colour))   /* "i9", "Imaj9", "VII11", "iim9b5" */
        return cat4(buf, len, num, XPRE[q][q == 2 ? sev == 10 : sev == 11], EXT_NUM[colour], q == 2 && sev == 10 ? "b5" : "");
    return cat2(buf, len, num, SEV[q][q == 2 ? sev == 10 : sev == 11]);
}

/* NAMES on: the name with a compact suffix, as 0.12 */
static int name_card(int key, int slot_scale, int d, int colour, char *buf, int len) {
    char base[16];
    int q = triad_quality(key, slot_scale, d), root = theory_degree_pc(key, slot_scale, d);
    theory_chord_name(key, slot_scale, d, base, sizeof base);
    if (colour <= COL_TRIAD || colour >= COL_COUNT) return cat2(buf, len, base, NULL);
    if (colour == COL_ADD9) return cat2(buf, len, base, "9");   /* 9: add9 on a card */
    if (colour >= COL_POWER) return theory_chord_name_c(key, slot_scale, d, colour, buf, len);   /* "E5", "Em9", "Cmaj13", "Em6" */
    if (colour == COL_7TH && q != 2)   /* a major 7th reads M7 (CM7), so a card never shows a maj7 as a dominant 7 */
        return cat2(buf, len, base, pmod(theory_degree_pc(key, slot_scale, d + 6) - root, 12) == 11 ? "M7" : "7");
    /* sus, or a 7th on a diminished triad: the root alone */
    if (colour != COL_7TH) return cat2(buf, len, NOTE[root], THEORY_COLOUR_NAME[colour]);
    return cat2(buf, len, NOTE[root], pmod(theory_degree_pc(key, slot_scale, d + 6) - root, 12) == 10 ? "m7b5" : "o7");
}

int theory_chord_card(int key, int scale, int slot_scale, int deg, int colour, int names, char *buf, int len) {
    static const char *const SUF[COL_COUNT] = {"", "7", "s2", "s4", "9", "5", "9", "11", "13", "6"};
    char num[12];
    int d = pmod(deg, 7), root = theory_degree_pc(key, slot_scale, d), sev;
    if (names) return name_card(key, slot_scale, d, colour, buf, len);
    theory_numeral(key, scale, slot_scale, d, num, sizeof num);
    if (colour <= COL_TRIAD || colour >= COL_COUNT) return cat2(buf, len, num, NULL);
    if (colour != COL_7TH && !is_ext(colour)) return cat4(buf, len, num, "(", SUF[colour], ")");
    sev = pmod(theory_degree_pc(key, slot_scale, d + 6) - root, 12);   /* the seventh: 11 major, 10 minor, 9 diminished */
    if (is_ext(colour)) return cat5(buf, len, num, "(", sev == 11 ? "M" : "", SUF[colour], ")");   /* "i(9)", "I(M13)" */
    return cat4(buf, len, num, "(", sev == 11 ? "M7" : sev == 9 ? THEORY_DIM "7" : "7", ")");
}

/* theory_voice_chord's search, the cost also weighting the top note's move (top_weight 0: not at all) */
static int voice_search(const int *notes, int n, const int *prev, int np, int top_weight, int *out) {
    int inv, shift, i, best = -1, bsum = 0, cost, sum, cand[THEORY_CHORD_MAX];
    for (inv = 0; inv < n && np > 0; inv++)
        for (shift = -12; shift <= 12; shift += 12) {
            /* the first inv notes up an octave, then sorted: add9's 9th lies past the octave, so a plain rotation
             * would not be ascending (for triads and 7ths this is the rotation) */
            for (i = 0; i < n; i++) cand[i] = notes[i] + (i < inv ? 12 : 0) + shift;
            sort_ints(cand, n);
            if (cand[0] < 43 || cand[0] > 62) continue;
            for (i = cost = sum = 0; i < n; i++) {
                int p = prev[i < np ? i : np - 1];
                cost += cand[i] > p ? cand[i] - p : p - cand[i];
                sum += cand[i];
            }
            cost += top_weight * (cand[n - 1] > prev[np - 1] ? cand[n - 1] - prev[np - 1] : prev[np - 1] - cand[n - 1]);
            if (best < 0 || cost < best || (cost == best && (cand[0] < out[0] || (cand[0] == out[0] && sum < bsum)))) {
                best = cost;
                bsum = sum;
                memcpy(out, cand, n * sizeof(int));
            }
        }
    if (best < 0) memcpy(out, notes, n * sizeof(int));   /* no prev, or no voicing in range: root position */
    return n;
}

int theory_voice_chord(const int notes[THEORY_CHORD_MAX], int n, const int prev[THEORY_CHORD_MAX], int np, int out[THEORY_CHORD_MAX]) {
    return voice_search(notes, n, prev, np, 0, out);
}

int theory_voice_mode(const int notes[THEORY_CHORD_MAX], int n, const int prev[THEORY_CHORD_MAX], int np, int mode, int out[THEORY_CHORD_MAX]) {
    if (mode == VMODE_CLOSE) {
        memcpy(out, notes, n * sizeof(int));
        return n;
    }
    if (mode == VMODE_ROOTLESS && n >= 4) return voice_search(notes + 1, n - 1, prev, np, 0, out);
    return voice_search(notes, n, prev, np, mode == VMODE_TOPLINE ? 100 : 0, out);
}

int theory_avoid(int key, int scale, int slot_scale, int deg, int colour) {
    static const signed char EXT[COL_COUNT][2] = {[COL_ADD9] = {9}, [COL_9TH] = {9}, [COL_11TH] = {9, 11}, [COL_13TH] = {9, 13}, [COL_6TH] = {6}};
    int c = colour >= 0 && colour < COL_COUNT ? colour : COL_TRIAD, avoid = 0;
    (void)scale;   /* the chord's own scale decides */
    for (int e = 0; e < 2 && EXT[c][e]; e++)   /* an extension a semitone above the root, 3rd or 5th (even if the voicing leaves the 5th out) */
        for (int t = 0; t < 3; t++)
            avoid |= pmod(theory_degree_pc(key, slot_scale, deg + EXT[c][e] - 1) - theory_degree_pc(key, slot_scale, deg + 2 * t), 12) == 1;
    return avoid;
}

int theory_drift(int key, int scale, int slot_scale, int deg, int colour, int cluster, int inversion) {
    static const unsigned char COST[COL_COUNT] = {0, 10, 12, 12, 12, 0, 22, 38, 50, 12};   /* by colour */
    int score = 0, c = colour >= 0 && colour < COL_COUNT ? colour : COL_TRIAD;
    if (slot_scale != scale) score += 30;
    if (triad_quality(key, slot_scale, deg) == 2) score += 15;
    score += COST[c];
    if (theory_avoid(key, scale, slot_scale, deg, c)) score += 20;
    if (cluster) score += 22;
    score += 2 * (inversion < 0 ? -inversion : inversion);
    return score > 100 ? 100 : score;
}

int theory_drift_level(int score) { return score < 16 ? 0 : score < 40 ? 1 : score < 65 ? 2 : 3; }
