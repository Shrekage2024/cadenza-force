#include "presets.h"
#include "seq_core.h"
#include "theory.h"
#ifndef PRESETS_NO_DATA
#include "presets_data.h"   /* PRESET_CHUNK: made by tools/make_presets.sh */
#else
static const char *const PRESET_CHUNK[PRESETS_COUNT];   /* tools/make_presets.c, which makes it: none yet */
#endif
#include <stdlib.h>

/* 0.23 the six genre presets: first drafts from the spec's table, to tune by ear (then run tools/make_presets.sh). A recipe's
 * genre column is its index (the menu's order is jamlib.c's). Progressions are theory.c's preset indices (THEORY_PRESETS, in
 * order); Melody columns melody2.h's MJ_*. */
enum { PR_EPIC, PR_POP, PR_SAD_POP, PR_FIFTIES, PR_CLASSIC, PR_BLUES, PR_ANDALUSIAN, PR_ROCK, PR_ROYAL_ROAD, PR_LIFT, PR_JAZZ, PR_DREAM, PR_VAMP, PR_FLOAT, PR_DRONE, PR_CANON };
enum { P_VERSE, P_CHORUS, P_PRE, P_BRIDGE };   /* a part's progression role */

typedef struct {
    preset_info_t info;
    int key, scale;
    int prog[4];   /* by role: Verse, Chorus, Pre, Bridge */
    int mel[2];    /* Melody column: main, Bridge */
} genre_recipe_t;

static const genre_recipe_t RECIPE[PRESETS_COUNT] = {
    {{"House 124", "PIANO, SUB, PLUCK", 124}, 9, SCALE_MINOR, {PR_EPIC, PR_EPIC, PR_LIFT, PR_VAMP}, {MJ_HOOK, MJ_LEAD}},
    {{"Techno 130", "STAB, ROLLING BASS, ACID", 130}, 9, SCALE_PHRYGIAN, {PR_DRONE, PR_FLOAT, PR_FLOAT, PR_VAMP}, {MJ_OST, MJ_ARP}},
    {{"Trance 138", "SUPERSAW, BASS, PLUCK", 138}, 9, SCALE_MINOR, {PR_EPIC, PR_EPIC, PR_LIFT, PR_DREAM}, {MJ_ARP, MJ_LEAD}},
    {{"Pop 110", "PIANO, BASS, SYNTH LEAD", 110}, 7, SCALE_MAJOR, {PR_SAD_POP, PR_POP, PR_FIFTIES, PR_ROYAL_ROAD}, {MJ_HOOK, MJ_LEAD}},
    {{"Rock 120", "GUITAR, BASS, LEAD GUITAR", 120}, 4, SCALE_MINOR, {PR_ROCK, PR_EPIC, PR_LIFT, PR_ANDALUSIAN}, {MJ_LEAD, MJ_RUN}},
    {{"Disco 118", "STRINGS, SLAP BASS, BRASS", 118}, 2, SCALE_DORIAN, {PR_VAMP, PR_FLOAT, PR_CLASSIC, PR_DREAM}, {MJ_HOOK, MJ_RUN}},
};

/* Every preset's parts, by seq part index (SEQ_PART_NAME: Verse, Intro, Pre, Chorus, Bridge, Outro). Levels 0-based (the JAM
 * grid's 1-5): level 5 is left for the user to push into */
typedef struct { int bars, prog, chords, bass, mel_bridge, mel; } preset_part_t;
static const preset_part_t PART[SEQ_NPARTS] = {
    {8, P_VERSE, 1, 1, 0, 1},    /* Verse: the main groove, where a pick lands */
    {8, P_VERSE, 0, 0, 0, 0},    /* Intro: sparse */
    {4, P_PRE, 2, 2, 0, 2},      /* Pre: builds */
    {8, P_CHORUS, 3, 3, 0, 3},   /* Chorus: the fullest */
    {8, P_BRIDGE, 1, 0, 1, 1},   /* Bridge: contrast */
    {4, P_CHORUS, 0, 0, 0, 0},   /* Outro: winds down */
};

const preset_info_t *preset_info(int i) { return i >= 0 && i < PRESETS_COUNT ? &RECIPE[i].info : NULL; }
const char *preset_chunk(int i) { return i >= 0 && i < PRESETS_COUNT ? PRESET_CHUNK[i] : NULL; }

static void no_midi(void *ctx, int status, int note, int vel) { (void)ctx, (void)status, (void)note, (void)vel; }

int preset_build(int i, char *buf, int len) {
    const genre_recipe_t *r;
    seq_t *sc;
    int n;
    if (i < 0 || i >= PRESETS_COUNT || !buf || len <= 0) return 0;
    r = &RECIPE[i];
    sc = calloc(1, sizeof *sc);
    if (!sc) return 0;
    seq_init(sc, no_midi, NULL);   /* a new instance is at INIT, stopped: every pick below applies at once */
    seq_set_key(sc, r->key);
    seq_set_scale(sc, r->scale);
    for (int p = 0; p < SEQ_NPARTS; p++) {   /* the Verse first: another part's first use copies it */
        const preset_part_t *t = &PART[p];
        seq_select_part(sc, p);
        seq_set_bars(sc, t->bars);
        seq_set_prog_preset(sc, r->prog[t->prog]);
        seq_jam_apply_now(sc, LANE_CHORDS, i, t->chords);
        seq_jam_apply_now(sc, LANE_BASS, i, t->bass);
        seq_mel_queue(sc, r->mel[t->mel_bridge], t->mel);
    }
    seq_select_part(sc, 0);
    seq_set_preset_loaded(sc, i + 1);
    n = seq_state_save(sc, buf, len);
    if (seq_unsaved_parts(sc)) n = 0;   /* a part that didn't fit: never a preset with a part missing */
    free(sc);
    return n > 0 && n < len ? n : 0;
}
