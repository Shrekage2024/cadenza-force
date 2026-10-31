/* The core-API parity trace (VST-33): the path iOS's audio thread and portrait UI take, seq_* calls with no wrapper or
 * cells, pinned by tests/core_parity.sha256 as tests/web_parity.sha256 pins the Force-look path. A fixed script of
 * setters, loads, presets, jumps and tempo changes is played twice, at the Force's 44100 Hz in 128-frame blocks and at
 * 48000 Hz in 256-frame blocks, and every event is printed with the frame of its block (seq_emit_fn's frame). Built
 * natively under ASan/UBSan (tests/run_tests.sh) and with clang on a Mac (tests/ios_tests.sh); both must print the same
 * bytes. A refactor must leave the hash unchanged; an intended change re-pins it and names the changed lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "seq_core.h"

static long blocks;
static void print(void *ctx, int status, int note, int vel, int frame) { (void)ctx; printf("%ld %02x %d %d %d\n", blocks, status, note, vel, frame); }

static double play(seq_t *s, double ppq, double bpm, double beats, int frames) {
    for (double end = ppq + beats; ppq < end - 1e-9; blocks++) {
        seq_block(s, ppq, bpm, 1, frames);
        ppq += frames * bpm / (60.0 * s->rate);
    }
    return ppq;
}

static void idle(seq_t *s, int n, int frames) { for (; n > 0; n--, blocks++) seq_block(s, -1, 120, 0, frames); }

static void pass(double rate, int frames) {
    static seq_t s;
    static char kept[8192];
    const int deg[4] = {0, 5, 3, 4};
    const unsigned char src[4] = {0, 0, 0, 0}, col[4] = {0, 0, 0, 0};
    double ppq = 0;
    blocks = 0;
    printf("> pass %g Hz, %d frames\n", rate, frames);
    seq_init(&s, print, NULL);
    seq_init_preset(&s);
    seq_set_rate(&s, rate);
    ppq = play(&s, ppq, 120, 8, frames);                       /* the INIT song */
    printf("> steps and mutes\n");
    seq_set_step(&s, LANE_CHORDS, 0, 6, 1);
    seq_set_step(&s, LANE_BASS, 1, 2, 1);
    seq_set_mute(&s, LANE_MEL_UP, 1);
    ppq = play(&s, ppq, 120, 4, frames);
    seq_set_mute(&s, LANE_MEL_UP, 0);
    printf("> key, scale, length\n");
    seq_set_key(&s, 7);
    seq_set_scale(&s, SCALE_MINOR);
    seq_set_bars(&s, 2);
    seq_set_chord_len(&s, 8);
    ppq = play(&s, ppq, 120, 8, frames);
    printf("> progression, audition, undo\n");
    seq_set_progression(&s, 4, deg, src, col, 1);
    seq_audition_note(&s, LANE_BASS, 43);
    seq_audition_chord(&s, 1);
    ppq = play(&s, ppq, 120, 4, frames);
    seq_undo(&s);
    ppq = play(&s, ppq, 120, 4, frames);
    printf("> tempo change (a small step back), a loop wrap, a locate forward\n");
    ppq = play(&s, ppq, 140, 2, frames);
    ppq = play(&s, ppq - 0.3, 140, 2, frames);
    ppq = play(&s, 0, 140, 4, frames);
    ppq = play(&s, ppq + 7, 140, 2, frames);
    printf("> stop, idle, save, load, start\n");
    seq_block(&s, ppq, 140, 0, frames); blocks++;
    idle(&s, 30, frames);
    seq_state_save(&s, kept, sizeof kept);
    seq_set_key(&s, 2);
    seq_state_load(&s, kept);
    printf("%s\n", kept);
    ppq = play(&s, 0, 120, 8, frames);
    printf("> channels, all notes off\n");
    seq_set_channel(&s, LANE_CHORDS, 10);
    ppq = play(&s, ppq, 120, 2, frames);
    seq_all_notes_off(&s);
    printf("> end %ld blocks\n", blocks);
}

int main(void) {
    pass(44100, 128);
    pass(48000, 256);
    return 0;
}
