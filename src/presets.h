/* 0.23 genre presets (MPC's PRESET menu after Init; spec 2026-10-23-genre-presets-design.md): one per JAM genre column, in
 * jamlib.c's order (HOUSE TECHNO TRANCE POP ROCK DISCO: jam_genre_name(i)), each filling all 6 parts. A preset is a recipe built
 * through the seq API on a fresh scratch instance (preset_build) and baked into src/presets_data.h as its state chunk
 * (tools/make_presets.sh), so a pick is one seq_song_load, no generation while the audio thread waits, and the same every time.
 * No host code. */
#ifndef PRESETS_H
#define PRESETS_H

#include "jamlib.h"

#define PRESETS_COUNT JAM_COLS

typedef struct {
    const char *name;     /* the PRESET menu's: "House 124" (ASCII, at most 23 characters) */
    const char *sounds;   /* the sounds it was made for, PLAY's hint: "PIANO, SUB, PLUCK" */
    int bpm;              /* the tempo it was made for (a plugin can't set MPC's) */
} preset_info_t;

const preset_info_t *preset_info(int i);   /* 0..PRESETS_COUNT-1, else NULL */
const char *preset_chunk(int i);           /* preset i's baked state (gn = i + 1, the Verse playing), else NULL */

/* Builds preset i into buf (what preset_chunk bakes): its length, or 0 for a bad i, a full buf or no memory. Allocates a
 * scratch seq_t (about 290 KB) and generates all 6 parts: tools/make_presets.sh and the tests only, never on a pick */
int preset_build(int i, char *buf, int len);

#endif
