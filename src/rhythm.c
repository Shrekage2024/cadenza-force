#include "rhythm.h"
#include "jamlib.h"
#include "theory.h"
#include "util.h"
#include <pthread.h>   /* VST-33: the COMPLEXITY table's one build (pthread_once) */
#include <string.h>

/* The library, in the RHYTHMS browser's order (4 per group). Codes: A accent, x hit, g ghost, - hold, . rest. Of the nine
 * rhythms older than 0.18, Sustain, Pulse and Stabs are what gen_lane_v1 writes at complexity 0 (seed 1, 1 bar); the six
 * 0.12 chord rhythms are their template 0 in full (gen.c CH2, every hit, no ghosts). */
static const struct { const char *name, *feel, *codes; int group, is_new; } LIB[2][RHYTHM_COUNT] = {
  {   /* RLIB_CHORDS */
    {"Sustain", "one chord held for the bar", "x---------------", 0, 0},
    {"Ballad", "1 held, 3 softer", "x-------x-------", 0, 0},
    {"Half-time", "1 and 3, long", "A-------x-------", 0, 0},
    {"Pulse", "quarter notes", "x---x---x---x---", 0, 0},
    {"Eighths", "straight 8ths, driving", "A.x.x.x.A.x.x.x.", 1, 1},
    {"Push", "the & of 4 pushes into the bar", "x-----x-......A-", 1, 0},
    {"Stabs", "short, a little syncopated", "x.......x.......", 1, 0},
    {"Anthem", "3 + 3 + 2, big chorus", "A--x--x-A--x--x-", 1, 1},
    {"Offbeat", "house upbeats", "..x-..x-..x-..x-", 2, 0},
    {"Garage Skip", "2-step skip", "A.....x..x....x.", 2, 1},
    {"Trance Gate", "gated 16ths", "AxgxAxgxAxgxAxgx", 2, 1},
    {"Dub Stab", "echoing upbeat stabs", "..A-......A-....", 2, 1},
    {"Boom Bap", "chopped sample chords", "A-....x..x..x...", 3, 1},
    {"Trap Hold", "long and dark", "A-------..x-----", 3, 1},
    {"Drill", "sliding dotted hits", "A--x--x---x--x--", 3, 1},
    {"Lo-fi Lazy", "behind the beat, soft", "A--.....g.x-....", 3, 1},
    {"Skank", "reggae 2 and 4", "....A.......A...", 4, 1},
    {"Son Clave", "2-3 clave", "..x.x...A..x..x.", 4, 1},
    {"Tresillo", "3 + 3 + 2, Latin pop", "A..x..x.A..x..x.", 4, 1},
    {"Bossa", "bossa nova comping", "A..x..x...x..x..", 4, 1},
    {"Funk", "16th syncopation, short", "A..x..x...x.x.x.", 5, 0},
    {"Charleston", "1 long, the & of 2 short", "A-----x-........", 5, 0},
    {"Neo-soul", "lazy, with ghosts", "A---..g.x--...g.", 5, 1},
    {"Gospel Push", "leans into the next bar", "A-..x-..x-....A-", 5, 1},
  },
  {   /* RLIB_BASS: all new in 0.19 */
    {"Root", "one long note a bar", "A---------------", 0, 1},
    {"Halves", "1 and 3", "A-------x-------", 0, 1},
    {"Quarters", "every beat", "A...x...x...x...", 0, 1},
    {"Eighths", "straight 8ths", "A.x.A.x.A.x.A.x.", 0, 1},
    {"Offbeat", "house bass on the &", "..A...x...A...x.", 1, 1},
    {"Rolling", "techno: 16ths after the kick", ".xxx.xxx.xxx.xxx", 1, 1},
    {"Gallop", "the trance gallop", "A.xxA.xxA.xxA.xx", 1, 1},
    {"Sidechain", "held offbeats that pump", "..A-..x-..A-..x-", 1, 1},
    {"808 Hold", "one long 808", "A-------------..", 2, 1},
    {"808 Bounce", "808 with a double hit", "A-..x-....A-x-..", 2, 1},
    {"Drill", "dotted, sliding hits", "A--x--x---x--x--", 2, 1},
    {"Boom Bap", "laid back, sampled", "A-....x...x-....", 2, 1},
    {"Funk", "16th syncopation", "A..xx.x.A..x.xx.", 3, 1},
    {"Slap", "pops with ghost notes", "A.gxA.gx..x.A.gx", 3, 1},
    {"Motown", "melodic pickups", "A..x..x.A..x.x..", 3, 1},
    {"Disco", "8ths with ghosts between", "A.xgA.xgA.xgA.xg", 3, 1},
    {"One Drop", "reggae: lands on 3", "........A---....", 4, 1},
    {"Steppers", "dub, on every beat", "A...x...A...x...", 4, 1},
    {"Tumbao", "salsa: the & of 2 and 4", "......A-....x---", 4, 1},
    {"Tresillo", "3 + 3 + 2", "A..x..x.A..x..x.", 4, 1},
    {"Walk", "walking quarters", "x...x...x...x...", 5, 1},
    {"Two Feel", "1 and 3 with ghosts", "A-----g-x-----g-", 5, 1},
    {"Neo-soul", "lazy, behind the beat", "A---..g.x--...g.", 5, 1},
    {"Gospel Push", "leans into the next bar", "A-..x-..x-....A-", 5, 1},
  },
};
static const char *GROUPS[2][RHYTHM_GROUPS] = {
    {"Basic", "Pop / Rock", "Dance", "Hip hop", "Latin", "Funk / Soul"},
    {"Basic", "Dance", "Hip hop", "Funk / Disco", "Latin / Reggae", "Jazz / Soul"}};
static const char CODE_CH[5] = {'.', 'x', 'A', 'g', '-'};   /* indexed by RC_* */

_Static_assert((int)JL_CHORDS == (int)RLIB_CHORDS && (int)JL_BASS == (int)RLIB_BASS, "the JAM lookups pass the library as the jamlib lane");
int rhythm_is_jam(int from) { return from >= RJAM_BASE && from < RJAM_BASE + JAM_COLS * JAM_LEVELS; }   /* 0.22 */
const char *rhythm_lib_name(int lib, int i) {
    if (rhythm_is_jam(i)) return jam_block_name(lib, (i - RJAM_BASE) / JAM_LEVELS, (i - RJAM_BASE) % JAM_LEVELS);
    return LIB[lib][i].name;
}
const char *rhythm_lib_feel(int lib, int i) { return rhythm_is_jam(i) ? "" : LIB[lib][i].feel; }
int rhythm_lib_group(int lib, int i) { return rhythm_is_jam(i) ? 0 : LIB[lib][i].group; }
const char *rhythm_lib_group_name(int lib, int g) { return GROUPS[lib][g]; }
int rhythm_lib_is_new(int lib, int i) { return rhythm_is_jam(i) ? 0 : LIB[lib][i].is_new; }
const char *rhythm_name(int i) { return rhythm_lib_name(RLIB_CHORDS, i); }
const char *rhythm_feel(int i) { return rhythm_lib_feel(RLIB_CHORDS, i); }
int rhythm_group(int i) { return rhythm_lib_group(RLIB_CHORDS, i); }
const char *rhythm_group_name(int g) { return rhythm_lib_group_name(RLIB_CHORDS, g); }
int rhythm_is_new(int i) { return rhythm_lib_is_new(RLIB_CHORDS, i); }

int rhythm_parse(const char *s16, unsigned char out[16]) {
    if (!s16 || strlen(s16) != 16) return 0;
    unsigned char c[16];
    for (int k = 0; k < 16; k++) {
        int v = -1;
        for (int j = 0; j < 5; j++) if (s16[k] == CODE_CH[j]) v = j;
        if (v < 0) return 0;
        c[k] = (unsigned char)v;
    }
    memcpy(out, c, 16);
    return 1;
}

void rhythm_format(const unsigned char c[16], char out[17]) {
    for (int k = 0; k < 16; k++) out[k] = CODE_CH[c[k] <= RC_HOLD ? c[k] : RC_REST];
    out[16] = 0;
}

void rhythm_lib_codes(int lib, int i, unsigned char out[16]) {
    rhythm_parse(rhythm_is_jam(i) ? jam_block_codes(lib, (i - RJAM_BASE) / JAM_LEVELS, (i - RJAM_BASE) % JAM_LEVELS) : LIB[lib][i].codes, out);
}
void rhythm_codes(int i, unsigned char out[16]) { rhythm_lib_codes(RLIB_CHORDS, i, out); }

void rhythm_to_steps(const unsigned char c[16], seq_step_t out[16]) {
    memset(out, 0, 16 * sizeof(seq_step_t));
    for (int k = 0; k < 16; k++) {
        if (!rhythm_is_hit(c[k])) continue;
        int len = 1;
        while (k + len < 16 && c[k + len] == RC_HOLD) len++;
        out[k] = (seq_step_t){1, TONE_R, 0, (unsigned char)(c[k] == RC_ACC ? 112 : c[k] == RC_GHOST ? 60 : 100),
                              (unsigned char)len, 0};
    }
}

void rhythm_from_steps(const seq_step_t st[16], unsigned char out[16]) {
    memset(out, RC_REST, 16);
    for (int k = 0; k < 16; k++) {
        if (!st[k].on) continue;
        out[k] = st[k].vel >= 108 ? RC_ACC : st[k].vel <= 70 ? RC_GHOST : RC_HIT;
        for (int j = 1; j < st[k].len && k + j < 16 && !st[k + j].on; j++) out[k + j] = RC_HOLD;
    }
}

int rhythm_step_code(const seq_step_t st[16], int k) {
    int h = k;
    if (st[k].on) return st[k].vel >= 108 ? RC_ACC : st[k].vel <= 70 ? RC_GHOST : RC_HIT;
    while (--h >= 0 && !st[h].on) {}
    return h >= 0 && k - h < st[h].len ? RC_HOLD : RC_REST;   /* held by the hit before it, its len reaching k */
}

int rhythm_lib_find(int lib, const unsigned char c[16]) {
    for (int i = 0; i < RHYTHM_COUNT; i++) {
        unsigned char d[16];
        rhythm_lib_codes(lib, i, d);
        if (memcmp(c, d, 16) == 0) return i;
    }
    return -1;
}
int rhythm_find(const unsigned char c[16]) { return rhythm_lib_find(RLIB_CHORDS, c); }

/* ---- bar transforms (the canvas RhyBars functions) ---- */

static const unsigned char W16[16] = {16, 1, 4, 2, 8, 1, 4, 2, 12, 1, 4, 2, 8, 1, 4, 2};

int rhythm_weight(int step) { return step >= 0 && step < 16 ? W16[step] : 0; }

int rhythm_is_hit(int code) { return code == RC_HIT || code == RC_ACC || code == RC_GHOST; }
static int is_free(int c) { return c == RC_REST || c == RC_HOLD; }   /* where SYNC, GHOSTS and FILL may put a hit */

void rhythm_cut_holds(unsigned char c[16], int i) {
    for (int j = i + 1; j < 16 && c[j] == RC_HOLD; j++) c[j] = RC_REST;
}

static int rank_of(const unsigned char *r, int i) {
    return W16[i] + (r[i] == RC_ACC ? 6 : 0) - (r[i] == RC_GHOST ? 6 : 0);
}

void rhythm_hits(const unsigned char in[16], int cx, unsigned char out[16]) {
    memmove(out, in, 16);
    if (cx < 50) {
        int idx[16], n = 0;
        for (int i = 0; i < 16; i++) if (rhythm_is_hit(in[i])) idx[n++] = i;
        int keep = cx <= 0 ? 1 : (150 + 2 * (n - 1) * cx) / 100;
        if (keep < 1) keep = 1;
        for (int a = 1; a < n; a++) {   /* insertion sort: best first, ties to the lower step */
            int v = idx[a], b = a - 1;
            while (b >= 0 && rank_of(in, idx[b]) < rank_of(in, v)) { idx[b + 1] = idx[b]; b--; }
            idx[b + 1] = v;
        }
        for (int a = keep; a < n; a++) { out[idx[a]] = RC_REST; rhythm_cut_holds(out, idx[a]); }
    } else if (cx > 50) {
        int free_[16], r = 0;
        for (int i = 0; i < 16; i++) if (in[i] == RC_REST) free_[r++] = i;
        int add = (2 * r * (cx > 100 ? 50 : cx - 50) + 50) / 100;
        for (int a = 1; a < r; a++) {   /* highest weight first, ties to the lower step */
            int v = free_[a], b = a - 1;
            while (b >= 0 && W16[free_[b]] < W16[v]) { free_[b + 1] = free_[b]; b--; }
            free_[b + 1] = v;
        }
        for (int a = 0; a < add; a++) out[free_[a]] = RC_HIT;
    }
}

/* Moves the hit at a to b, a rest or a hold (a hold there is cut: the hit before it stops at b). The moved hit keeps its
 * own hold count, clipped at the next hit and the bar end; the steps it left become rests. */
static int move_hit(unsigned char *r, int a, int b) {
    if (!rhythm_is_hit(r[a]) || b < 0 || b >= 16 || !is_free(r[b])) return 0;
    unsigned char c = r[a];
    int n = 0;
    while (a + 1 + n < 16 && r[a + 1 + n] == RC_HOLD) n++;
    r[a] = RC_REST;
    rhythm_cut_holds(r, a);
    r[b] = c;
    int e = b + 1;
    for (int j = 0; j < n && e < 16 && !rhythm_is_hit(r[e]); j++) r[e++] = RC_HOLD;
    while (e < 16 && r[e] == RC_HOLD) r[e++] = RC_REST;   /* the old chain's leftover holds */
    return 1;
}

void rhythm_sync(const unsigned char in[16], int k, unsigned char out[16]) {
    static const unsigned char PUSH[4][2] = {{0, 14}, {8, 6}, {4, 2}, {12, 10}};
    memmove(out, in, 16);
    if (k > 0) {
        int n = k >= 3 ? 4 : k;
        for (int j = 0; j < n; j++) move_hit(out, PUSH[j][0], PUSH[j][1]);
    }
    if (k == -1)
        for (int i = 1; i < 16; i += 2)
            if (rhythm_is_hit(out[i]) && !move_hit(out, i, i - 1)) move_hit(out, i, i + 1);
    if (k <= -2)
        for (int i = 0; i < 16; i++)
            if (i % 4 && rhythm_is_hit(out[i]) && !move_hit(out, i, i - i % 4) &&
                !(i % 4 >= 2 && move_hit(out, i, i - i % 4 + 4))) {
                out[i] = RC_REST;
                rhythm_cut_holds(out, i);
            }
}

/* 0.22 a hold with nothing sounding before it in the bar is a rest */
static void fix_holds(unsigned char c[16]) {
    for (int i = 0; i < 16; i++)
        if (c[i] == RC_HOLD && (i == 0 || c[i - 1] == RC_REST)) c[i] = RC_REST;
}

void rhythm_time(const unsigned char in[16], int tm, unsigned char out[16]) {
    unsigned char t[16];
    memcpy(t, in, 16);
    if (tm < 0) {
        memset(out, RC_REST, 16);
        for (int i = 0; i < 8; i++) {
            out[2 * i] = t[i];
            if (t[i] != RC_REST) out[2 * i + 1] = RC_HOLD;
        }
    } else if (tm > 0) {
        for (int i = 0; i < 8; i++) out[i] = out[i + 8] = t[2 * i];
    } else {
        memcpy(out, t, 16);
        return;
    }
    fix_holds(out);
}

void rhythm_vary(const unsigned char in[16], int pos, int fill_level, unsigned char out[16]) {
    if (pos == 1) rhythm_sync(in, 1, out);
    else if (pos == 2) {   /* rotated 2 steps later; a hold left at the bar start (its hit wrapped round) becomes that hit's code */
        for (int i = 0; i < 16; i++) out[(i + 2) & 15] = in[i];
        for (int k = 0; k < 16; k++) {
            if (out[k] != RC_HOLD || (k && out[k - 1] != RC_REST)) continue;
            int j = (k - 2) & 15, n = 0;
            while (in[j] == RC_HOLD && n++ < 16) j = (j + 15) & 15;
            out[k] = rhythm_is_hit(in[j]) ? in[j] : RC_HIT;
        }
    } else if (pos == 3) rhythm_fill(in, fill_level, out);
    else memmove(out, in, 16);
}

/* 0.24 LOCK WITH's IN THE GAPS (rhythm.h) */
void rhythm_gaps(const unsigned char other[16], unsigned char out[16]) {
    memset(out, RC_REST, 16);
    for (int b = 0; b < 16; b += 4) {
        int any = 0;
        for (int i = b; i < b + 4; i += 2)
            if (other[i] == RC_REST) out[i] = RC_HIT, any = 1;
        for (int i = b; !any && i < b + 4; i++)
            if (other[i] == RC_REST) out[i] = RC_HIT, any = 1;
    }
}

void rhythm_newbar(const unsigned char in[16], int v, unsigned char out[16]) {
    memmove(out, in, 16);
    if (v <= 0) return;
    for (int i = v % 2 ? 4 : 2; i < 16; i += 4) {
        if (!(v % 2)) {
            if (in[i] == RC_REST) out[i] = RC_HIT;
        } else if (rhythm_is_hit(in[i]) && in[i - 1] == RC_REST && in[i - 2] == RC_REST) {
            out[i - 2] = in[i], out[i] = RC_REST;
            rhythm_cut_holds(out, i);
        }
    }
}

void rhythm_ghosts(const unsigned char in[16], int g, unsigned char out[16]) {
    static const unsigned char ORDER[12] = {3, 11, 7, 15, 1, 9, 5, 13, 2, 10, 6, 14};
    static const int COUNT[5] = {0, 1, 2, 4, 99};
    memmove(out, in, 16);
    int left = COUNT[g < 0 ? 0 : g > 4 ? 4 : g];
    for (int j = 0; j < 12 && left > 0; j++)
        if (is_free(out[ORDER[j]])) { out[ORDER[j]] = RC_GHOST; left--; }
}

void rhythm_fill(const unsigned char in[16], int f, unsigned char out[16]) {
    static const unsigned char F2[7] = {10, 12, 14, 9, 11, 13, 15};
    memmove(out, in, 16);
    if (f == 1) {
        if (is_free(out[14])) out[14] = RC_HIT;
        if (is_free(out[15])) out[15] = RC_GHOST;
    }
    if (f == 2) {
        int left = 4;
        for (int j = 0; j < 7 && left > 0; j++)
            if (is_free(out[F2[j]])) { out[F2[j]] = RC_HIT; left--; }
    }
    if (f == 3) { out[12] = RC_HIT; out[13] = RC_HIT; out[14] = RC_HIT; out[15] = RC_ACC; }
    if (f == 4) for (int i = 8; i < 16; i++) out[i] = RC_REST;
}

void rhythm_bass_fill(const unsigned char in[16], int f, unsigned char out[16]) {
    static const signed char FROM[4] = {16, 15, 13, 12};   /* NONE, PICKUP, STEP-IN, RUN: their notes' first step */
    memmove(out, in, 16);
    if (f == 4) for (int i = 8; i < 16; i++) out[i] = RC_REST;
    else if (f > 0 && f < 4) for (int i = FROM[f]; i < 16; i++) out[i] = RC_HIT;
}

void rhythm_breath(const unsigned char in[16], int n, unsigned char out[16]) {
    memmove(out, in, 16);
    for (int i = n < 0 ? 16 : 16 - (n > 16 ? 16 : n); i < 16; i++) out[i] = RC_REST;
}

/* Each hit scores the heaviest step after it (rests and holds, up to the next hit, around the loop) if that step is
 * heavier than the hit: the syncopation is the weight difference. */
int rhythm_lhl(const unsigned char c[16]) {
    int score = 0;
    for (int i = 0; i < 16; i++) {
        if (!rhythm_is_hit(c[i])) continue;
        int top = 0;
        for (int d = 1; d < 16; d++) {
            int j = (i + d) & 15;
            if (rhythm_is_hit(c[j])) break;
            if (W16[j] > top) top = W16[j];
        }
        if (top > W16[i]) score += top - W16[i];
    }
    return score;
}

/* ---- COMPLEXITY layers. One row per layer; tune by editing this table only. A window runs start -> end (end < start
 * for a layer that simplifies below 50); the offset is round(max * smoothstep(t)). ---- */
enum { CX_TENSION = CXT_COUNT };   /* the Tension layer: COLOUR steps summed apart (cx_offsets_t.tension) */
static const struct { int target, start, end, max; } CX_LAYERS[] = {
    /* above 50: add */
    {CXT_GHOSTS, 52, 70, 2},  {CXT_COL, 60, 75, 1},    {CXT_SPR, 62, 77, 1},    {CXT_SYNC, 70, 85, 1},
    {CXT_STRUM, 72, 90, 2},   {CXT_COL, 80, 95, 1},    {CXT_HUM, 80, 100, 1},   {CXT_GHOSTS, 85, 100, 1},
    {CXT_SYNC, 85, 100, 1},   {CX_TENSION, 95, 100, 1},
    /* below 50: simplify */
    {CXT_GHOSTS, 45, 35, -4}, {CXT_HUM, 40, 25, -4},   {CXT_STRUM, 40, 25, -8}, {CXT_SYNC, 40, 15, -5},
    {CXT_SPR, 35, 20, -3},    {CXT_COL, 30, 15, -1},   {CXT_COL, 10, 0, -1},
};
#define CX_VOICE_AT 75   /* the voice-leading layer: CLOSE becomes SMOOTH from here */

static cx_offsets_t cx_offsets_of(int cx) {
    int sum[CXT_COUNT + 1] = {0};
    for (unsigned k = 0; k < sizeof CX_LAYERS / sizeof CX_LAYERS[0]; k++) {
        double t = (double)(cx - CX_LAYERS[k].start) / (CX_LAYERS[k].end - CX_LAYERS[k].start);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        double v = CX_LAYERS[k].max * t * t * (3 - 2 * t);
        sum[CX_LAYERS[k].target] += (int)(v < 0 ? v - 0.5 : v + 0.5);   /* round half away from 0 */
    }
    return (cx_offsets_t){sum[CXT_COL], sum[CXT_SPR], sum[CXT_SYNC], sum[CXT_STRUM], sum[CXT_GHOSTS], sum[CXT_HUM],
                          cx >= CX_VOICE_AT, sum[CX_TENSION]};
}

/* 0.18 perf: the offsets of every COMPLEXITY 0..100, made once (rhythm_init, on the first instance's UI thread) and only read
 * after: each costs 17 double divisions, and the setters' labels and the audio thread ask for it per chord and step. Until
 * the table is made (or for a value outside 0..100) the offsets are worked out as they are asked for. The table is made
 * before the first instance exists, so a reader on another thread got the instance after it (a relaxed read suffices). VST-33:
 * made under pthread_once, so two instances created on two threads at once both wait for the one build. */
static cx_offsets_t cx_table[101];
static unsigned char cx_flat[101];   /* rhythm_cx_colour_flat */
static int cx_table_state;   /* 0 empty, 2 made */
static pthread_once_t cx_once = PTHREAD_ONCE_INIT;

static int colour_flat_of(int cx) {
    cx_offsets_t a = cx_offsets_of(clampi(cx - 2, 0, 100));
    for (int d = -1; d <= 2; d++) {
        cx_offsets_t b = cx_offsets_of(clampi(cx + d, 0, 100));
        if (b.col != a.col || b.tension != a.tension) return 0;
    }
    return 1;
}

static void cx_table_build(void) {
    for (int cx = 0; cx <= 100; cx++) cx_table[cx] = cx_offsets_of(cx), cx_flat[cx] = (unsigned char)colour_flat_of(cx);
    __atomic_store_n(&cx_table_state, 2, __ATOMIC_RELEASE);
}

void rhythm_init(void) { pthread_once(&cx_once, cx_table_build); }

static int cx_ready(int cx) { return cx >= 0 && cx <= 100 && __atomic_load_n(&cx_table_state, __ATOMIC_RELAXED) == 2; }

cx_offsets_t rhythm_cx_offsets(int cx) { return cx_ready(cx) ? cx_table[cx] : cx_offsets_of(cx); }

int rhythm_cx_colour_flat(int cx) { return cx_ready(cx) ? cx_flat[cx] : colour_flat_of(cx); }

int rhythm_cx_apply(int target, int user, int offset) {
    static const struct { int lo, hi, simple; } R[CXT_COUNT] = {
        {-1, 4, -1}, {-1, 3, 0}, {-2, 3, -2}, {-8, 8, 0}, {0, 4, 0}, {0, 4, 0}};
    if (target < 0 || target >= CXT_COUNT) return user;
    int v = user;
    if (offset < 0) {
        if (user > R[target].simple) { v = user + offset; if (v < R[target].simple) v = R[target].simple; }
        else if (user < R[target].simple) { v = user - offset; if (v > R[target].simple) v = R[target].simple; }
    } else {
        v = (target == CXT_STRUM && user < 0) ? user - offset : user + offset;
    }
    return v < R[target].lo ? R[target].lo : v > R[target].hi ? R[target].hi : v;
}
