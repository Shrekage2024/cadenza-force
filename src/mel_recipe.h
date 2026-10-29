/* The melody recipe (0.11, style GEN_MEL_RECIPE) as data: the types, the knobs, the phrasing and mel_recipe_t. No
 * dependencies, so both melody.h (the generator) and seq_core.h (one recipe per part) include it.
 * Saved: type and option indices are append only, never renumber. */
#pragma once

enum { MEL_ARP, MEL_OST, MEL_PENT, MEL_HOOK, MEL_SEQ, MEL_GUIDE, MEL_APPR, MEL_NTYPES };
/* The knobs after COMPLEXITY (which is the lane's complexity), each 0..100 */
enum { MK_RANGE, MK_LEAPS, MK_SYNC, MK_TENSION, MK_ANSWER, MK_REGISTER, MK_GATE, MK_COUNT };
/* The phrase saved by 0.11-0.13 (`.mr`), which VST-12's phrasing replaced: mel_phrase_from_legacy maps it on load */
enum { MEL_PHRASE_1_1, MEL_PHRASE_2_2, MEL_NPHRASES };
/* VST-12 phrasing, four settings (ph[MP_*]): Form, where calls end and answers start in each 4 bars; Breath, the rest at
 * a call's end; Cadence, how phrases end; Fill, a rest or a pickup note in the breath */
enum { MP_FORM, MP_BREATH, MP_CADENCE, MP_FILL, MP_COUNT };
enum { MEL_FORM_LOOP, MEL_FORM_1_1, MEL_FORM_2_2, MEL_FORM_3_1, MEL_NFORMS };
enum { MEL_BREATH_NONE, MEL_BREATH_8TH, MEL_BREATH_BEAT, MEL_BREATH_HALF, MEL_NBREATHS };
enum { MEL_CAD_NONE, MEL_CAD_LAND, MEL_CAD_QA, MEL_NCADS };
enum { MEL_FILL_REST, MEL_FILL_PICKUP, MEL_NFILLS };

/* opt[w]: the option picked in row w (0..mel_nopts - 1). ph: the phrasing, MP_* (mel_phrase_n values each). rev, inv:
 * 0/1. shift: 16ths, 0..15. kn[MK_GATE] is applied at playback, not here. */
typedef struct { unsigned char type, opt[3], ph[MP_COUNT], rev, inv, shift; unsigned char kn[MK_COUNT]; } mel_recipe_t;
