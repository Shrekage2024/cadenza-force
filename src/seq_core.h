/* The sequencer itself, with no host or ALSA code, so it can be tested offline.
 *
 * A part is a loop of 1-16 bars of 16th-note steps over a chord progression in the project's key. Steps are
 * chord-relative (a tone above the root of the chord under them), so the same step follows any chord. 0.8 has six parts
 * (song sections, switched live at a bar line); the Verse, part 0, is v1's song by default: E minor, i-VI-III-VII, one
 * chord per bar, 4 bars.
 * Steps are placed from the host's song position (docs/MIDI_TIMING.md in mpc-vst-plugins), never from a clock we
 * count ourselves. */
#pragma once
#include <stdint.h>
#include "bassline.h"     /* bass_set_t, one per part */
#include "mel_recipe.h"   /* mel_recipe_t, one per part */
#include "melody2.h"      /* 0.20 mjam_set_t, mjam_note_t: the JAM Melody lane */
#include "theory.h"       /* THEORY_CHORD_MAX */

#define SEQ_LANES 4
#define SEQ_ACTIVE_LANES 3   /* 0.7: Melody 2 (lane 3) stays in the state but never plays, generates or takes taps */
#define SEQ_PARTS 8
#define SEQ_NPARTS 6   /* parts in use this release: 0 Verse, 1 Intro, 2 Pre, 3 Chorus, 4 Bridge, 5 Outro */
#define SEQ_STEPS 16                               /* steps per bar */
#define SEQ_MAX_BARS 16
#define SEQ_LANE_STEPS (SEQ_STEPS * SEQ_MAX_BARS)
#define SEQ_MAX_PROG 64                            /* 0.14: chord slots in a loop (16 bars of 1/4-bar chords) */
/* 0.24 change points: up to SEQ_CP_MAX a bar, each a virtual slot SEQ_MAX_PROG + bar x SEQ_CP_MAX + k after the real ones, so
 * every per-slot array the chord under a step indexes holds SEQ_SLOTS_ALL entries (seq_part_t.cp) */
#define SEQ_CP_MAX 4
#define SEQ_SLOTS_ALL (SEQ_MAX_PROG + SEQ_MAX_BARS * SEQ_CP_MAX)
#define SEQ_LEGACY_PROG 16                         /* the most a 0.13 build loads in `.prog` (it refuses more) */
#define SEQ_MAX_PENDING 128
#define SEQ_MAX_STRUM 32                           /* STRUM's queued note-ons (5 per chord at most: a 6-note chord plays its first at once;
                                                    * 0.22 6 for a swung one, beside the chord before's 5) */
#define SEQ_MAX_AUDITION 8                         /* 0.12: audition notes queued for the next seq_block */
#define SEQ_MOVE_TEXT 48                           /* 0.12: the last move's text, at most 47 characters */
#define SEQ_STATE_MAX 8192                         /* the toolchain wrapper's chunk buffer (wrapper/vst2_wrap.c) */
#define SEQ_UNDO 8                                 /* undo snapshots kept */

enum { LANE_CHORDS, LANE_BASS, LANE_MEL_UP, LANE_MEL_DOWN };
/* 0.19 the lanes with drawn bars and RHYTHM knobs (seq_part_t.rb, rk, rfill; seq_t.hum): Chords and Bass. Melody has none. */
enum { DL_CHORDS, DL_BASS, DL_COUNT };
/* 0.19 the tabs with their own view, strip and FOLLOW state (seq_t.tv): CHORDS and BASS */
enum { TAB_CHORDS, TAB_BASS, TAB_MEL, TAB_COUNT };
enum { VOICING_CLOSE, VOICING_SMOOTH };
/* 0.17 CHORDS page knobs, per part (seq_set_vknob): COLOUR -1..4 (Power, Triad, 7th, 9th, 11th, 13th), INVERSION -2..3, SPREAD
 * -1..3 (Cluster, Close, Open, Split, Wide), STRUM -8..8 (+ low to high, - high to low) */
enum { VK_COL, VK_INV, VK_SPR, VK_STR, VK_COUNT };
/* 0.9 chord-lane shaping, per part: values 0..100 (SHAPE_OCT -1..1); SHAPE_NEUTRAL plays exactly as 0.8 */
enum { SHAPE_COLOUR, SHAPE_SPREAD, SHAPE_PUSH, SHAPE_GATE, SHAPE_STRUM, SHAPE_DYN, SHAPE_OCT, SHAPE_COUNT };
/* 0.10 Bass-lane shaping, per part: values 0..100 (BSHAPE_OCT -1..1, BSHAPE_APPROACH 0..1); neutral 0,0,0,50,0,50,50,0,0
 * plays exactly as 0.9. MOVEMENT, SYNC, OCTAVES and APPROACH transform the generated steps (seq_set_lane_shape). */
enum { BSHAPE_MOVEMENT, BSHAPE_SYNC, BSHAPE_OCTAVES, BSHAPE_REGISTER, BSHAPE_PUSH, BSHAPE_GATE, BSHAPE_ACCENT, BSHAPE_OCT,
       BSHAPE_APPROACH, BSHAPE_COUNT };
#define LSHAPE_COUNT 9   /* a lane's SHAPE row: the larger of SHAPE_COUNT and BSHAPE_COUNT */
_Static_assert(LSHAPE_COUNT >= SHAPE_COUNT && LSHAPE_COUNT >= BSHAPE_COUNT, "LSHAPE_COUNT holds every lane's SHAPE row");

/* status is a full MIDI status byte (0x90 | channel, 0x80 | channel); vel 0 for note-offs */
typedef void (*seq_emit_fn)(void *ctx, int status, int note, int vel);

typedef struct {
    int status_off;   /* 0x80 | channel */
    int note;
    double off_ppq;
} seq_pending_t;

typedef struct {   /* a STRUM note-on not yet due: note_on runs at on_ppq (its note-off is queued then) */
    int lane, note, vel, len;
    double on_ppq, gf;   /* gf: the Chords lane's own GATE factor */
} seq_strum_t;

typedef struct {   /* 0.18 HUMANIZE: a drawn lane's delayed hit (0.19: one per lane, seq_t.hum), not yet due: it plays at on_ppq
                    * (one at a time: the next hit plays a waiting one first, so no hit ever sounds before the hit before it) */
    int on, bar, step, len, vel;
    int note;      /* 0.19: a drawn Bass hit's note (its fill variant's on a fill bar), else 0 */
    double on_ppq;
} seq_hum_t;

typedef struct { unsigned char lane, note; } seq_audition_t;   /* 0.12: a queued audition note (velocity 100, 1 beat) */

typedef struct {
    unsigned char on;
    unsigned char tone;   /* TONE_R..TONE_7 (theory.h), above the root of the chord under the step */
    signed char oct;      /* octaves above the chord root's octave 3, -4..4 */
    unsigned char vel;    /* 1..127 */
    unsigned char len;    /* in steps, 1..16 */
    unsigned char abs;    /* 0.10: 0 chord-relative, else the MIDI note 1..127 it plays. An absolute step's tone and oct are
                           * a fixed placeholder (TONE_R, 0), never a pitch: read abs. Its 0.9 fallback record (the root at
                           * the octave nearest the note) depends on the progression, so it is computed only when saved
                           * (seq_state_save), never stored, and can never be read stale. */
} seq_step_t;

/* A lane's generator settings (gen.h): style GEN_* (GEN_MANUAL = 0: the steps are the user's), seed 0..65535, complexity
 * 0..100. A generated lane's steps are regenerated by every setter that changes an input, except at locked steps. */
typedef struct { int style, seed, complexity, variant; } seq_gen_t;   /* variant: 0.13, a 0.12 chord rhythm's template (0..2) */

/* gen.h's rhythm cache (gen_cache_t), declared here so seq_t can hold one per melody lane: a melody's rhythm and the
 * inputs it was made from, so a key, scale or progression change only redoes the pitches. It checks every input it was
 * made from on each use, so it is never stale; all zero is empty. */
typedef struct {
    int valid, style, seed, complexity, bars, chord_len, lane, lead;   /* lead: there was one */
    unsigned char bar_var[SEQ_MAX_BARS];                               /* 0.16: the bars' variations (gen_params_t) */
    unsigned char lead_on[SEQ_LANE_STEPS], lead_len[SEQ_LANE_STEPS];   /* the lead's rhythm (an Answer reads only that) */
    unsigned w[SEQ_LANE_STEPS];
    short mi[SEQ_LANE_STEPS];
    seq_step_t st[SEQ_LANE_STEPS];
} seq_gen_cache_t;

typedef struct {
    seq_step_t steps[SEQ_LANE_STEPS];
    unsigned char mute;   /* a loaded state's A<n> mute flag, until seq_state_load moves it to seq_t.mute (mutes are global) */
    seq_gen_t gen;
    unsigned char lock[SEQ_LANE_STEPS / 8];   /* lock bit = hand-edited step: regeneration leaves it as it is */
    unsigned char bar_var[SEQ_MAX_BARS];      /* 0.16 (Bass, Melody 1): per bar, 0 the phrase's bar, else its own variation
                                               * (NEW BAR); saved as <P><n>.bv only when one is set */
} seq_lane_t;

/* 0.18 RHYTHM page: a bar of a drawn lane (0.19: Chords or Bass) as drawn. c: 16 step codes (rhythm.h RC_*); from: the library
 * rhythm it was picked from (rhythm.h, 0..RHYTHM_COUNT-1), -1 none; ed: painted since; lock: it plays as drawn, the knobs leave it */
typedef struct { unsigned char c[16]; signed char from; unsigned char ed, lock; } rbar_t;
/* 0.18 the RHYTHM knobs 2-5, per part and drawn lane (seq_part_t.rk): SYNC -2..3, GHOSTS, HUMANIZE, FILL 0..4; neutral 0. 0.24 PHRASE's
 * CADENCE (OFF, OPEN, SOFT, HOME) and BREATH (0, 2, 4, 8 steps: None, 1/8, Beat, Half) 0..3, appended; RK_COUNT the knobs */
enum { RK_SYNC, RK_GHOSTS, RK_HUM, RK_FILL, RK_CAD, RK_BREATH, RK_COUNT };
/* 0.24 LOCK WITH, per part and drawn lane (seq_part_t.lw): OWN its own bars, WITH the other lane's drawn hits, GAPS a hit in each of the
 * other lane's silent 8ths (rhythm_gaps). ANTICIPATE (seq_part_t.an): OFF, a 16th, an 8th, a beat (0, 1, 2, 4 steps) early */
enum { LW_OWN, LW_WITH, LW_GAPS };
enum { AN_OFF, AN_16, AN_8, AN_BEAT };
/* 0.24 a drawn lane's rendered step's flags (seq_part_t.sflag, seq_step_flags): SF_ADDED a knob added the hit (not in the drawn codes:
 * COMPLEXITY, SYNC, GHOSTS, JAM's TIME and VARIATION, ANTICIPATE), SF_EARLY ANTICIPATE's early hit (SF_ADDED too), SF_TIED the change
 * step it ties over (a hold) */
enum { SF_ADDED = 1, SF_EARLY = 2, SF_TIED = 4 };
#define CHORDS_CX_NEUTRAL 50   /* 0.18: the Chords lane's COMPLEXITY that plays the bars as drawn (new instances, INIT, RESET) */
#define BASS_CX_NEUTRAL 50     /* 0.19: the Bass lane's, the same */
/* 0.19 a GEN_BASS_DRAWN Bass lane's notes (bassline.h; seq_part_t.bass). set: the part's note settings; a note-locked bar (nlock
 * bit b) plays frozen[b] instead; fix: the fixed notes per step (0 none, else MIDI), kept (silent) on a step that no longer sounds.
 * 0.19.1: one struct (it was seq_part_t.bset, bfrozen, bnlock, bfix) */
typedef struct {
    bass_set_t set;
    bass_set_t frozen[SEQ_MAX_BARS];
    unsigned short nlock;
    unsigned char fix[SEQ_LANE_STEPS];
} bass_state_t;

/* 0.20 a GEN_MEL_JAM Melody lane's notes (melody2.h; seq_part_t.mjam). set: the part's melody settings; a locked bar (nlock bit b)
 * plays frozen[b] instead; fix: the fixed steps (0 none, 1..127 a MIDI note, MJ_FIX_REST a rest, MJ_FIX_HOLD holding the note
 * before it); sw: SWITCH AT 0..3 (BEAT, BAR, 2 BARS, PHRASE); push, dyn: its PUSH and DYNAMICS 0..100 (0, 50), as the Bass's */
#define MJ_FIX_REST 128
#define MJ_FIX_HOLD 129
typedef struct {
    mjam_set_t set;
    mjam_set_t frozen[SEQ_MAX_BARS];
    unsigned short nlock;
    unsigned char fix[SEQ_LANE_STEPS];
    unsigned char sw;
    unsigned char push, dyn;
    unsigned char swing, acc;   /* 0.21 SWING 0..5 (50..70 %, default 0) and ACCENT 0..3 (EVEN..HARD), per part, as push and dyn; Amendment 1:
                                 * acc stays LIGHT (1), its velocities fixed (no knob sets it; a load keeps 1) */
    unsigned char weak;         /* 0.21 MW_* bits: the knobs whose current step changes little here (render_mel; the knob text says "weak") */
    unsigned char grid16;       /* 0.21 1 when the loop swings on 16ths, 0 on 8ths (mjam_swing16; render_mel) */
} mjam_state_t;
enum { MW_DENS = 1, MW_RNG = 2, MW_VAR = 4, MW_SWING = 8, MW_TIME = 16 };   /* 0.21 Amendment 1: TIME takes ACCENT's bit */
#define MEL_JAM_CX 10   /* 0.20: a JAM Melody lane's COMPLEXITY (cx_3) at level 1: 20 x level + 10 (new instances, INIT) */

/* 0.22 a drawn lane's JAM state (seq_part_t.djam, per drawn lane DL_*): c, l the playing block (jamlib.h; c -1 none yet: a new instance,
 * INIT, a 0.21 project); sw SWITCH AT 0..3 (BEAT, BAR, 2 BARS, PHRASE; default BAR); var VARIATION 0..3 (OFF, BAR 4, BARS 2+4, ALL);
 * swing SWING 0..5 (50..70 %); tm TIME -1..1 (HALF, NORMAL, DOUBLE); grid16, weak derived (the swing grid, the knobs' weak bits).
 * Saved as `<P>1.dj=` / `<P>2.dj=` only once used */
typedef struct { signed char c, l, sw, var, swing, tm; unsigned char grid16, weak; } djam_t;

/* 0.24 a change point: from its step (0..15; -1 an empty entry) to the bar's next point or end, the chord deg / src (as a slot's:
 * src 0 the scale, 1..3 a borrow, whose degree deg is) of type `type` (seq_rbar_type's 0 Knobs .. 4 6th; 0.24.2 5..9 7th, power, 9th,
 * 11th, 13th: a point THIS HIT ONLY makes of a slot of that colour) plays instead of the slot */
typedef struct { signed char step, deg, src, type; } seq_cp_t;

typedef struct {
    int bars;                  /* 1, 2, 4, 8 or 16 */
    int filled;                /* bars holding real content: a shrink hides bars, a grow back shows them again */
    int chord_len;             /* steps per chord: 32, 16, 8 or 4 */
    /* 0.14: the loop's slot list, a chord per slot. Slot i sounds from step i x chord_len. Hidden slots (past prog_len,
     * below filled_slots) keep their chords, as hidden bars keep their steps. 0.24: entries SEQ_MAX_PROG.. are the change
     * points' virtual slots (cp), mirrored from cp by every change (cp_sync); prog_len stays the real slot count */
    int prog[SEQ_SLOTS_ALL];   /* scale degrees, 0-based; a borrowed slot's is its borrow's degree (older builds play it diatonic) */
    int prog_len;              /* 0.14: always seq_slots, max(1, bars x 16 / chord_len), after any setter or load */
    unsigned char src[SEQ_SLOTS_ALL];      /* 0.9: 0 the project scale, 1..3 the scale's borrowed chord (theory_slot_scale) */
    unsigned char colour[SEQ_SLOTS_ALL];   /* 0.9: COL_TRIAD..COL_ADD9; Triad takes the COLOUR knob's */
    uint64_t slot_lock;        /* bit i: slot i locked (a pick); HARMONY GEN, ideas and moves keep it (0.14: 64 slots) */
    int filled_slots;          /* 0.14: slots holding real chords (>= prog_len): a grow fills only the slots past it */
    /* 0.14: the period the slots repeat with, 1..16, as saved in the legacy `.prog` (a phrase a preset, NEW PROGRESSION or
     * an idea filled; a 0.13 project's progression). When the slots stop repeating with it, the smallest period that
     * divides the slot count, else 16 (the slots then save with `.ch` too). Above prog_len: the loop is the start of a
     * longer phrase (a 0.13 progression longer than its loop, a shrunk loop), whose hidden slots save in `.prog`. The
     * smooth voicing's chain follows it, so a 0.13 project plays as 0.13 played it. */
    int phrase;
    /* 0.14: the period the COLOUR knob ranks slots by (slot i at i % rank_len), 1..16: the phrase a preset, NEW PROGRESSION,
     * an idea of a new length or a load dealt (a 0.13 project's progression), kept through FIX edits, locks and moves, so
     * they never recolour another slot. Saved as `.rk` when it is not the phrase. */
    int rank_len;
    int lane_shape[SEQ_ACTIVE_LANES][LSHAPE_COUNT];   /* each lane's SHAPE knobs: Chords SHAPE_*, Bass BSHAPE_*, Melody
                                                      * none yet (a neutral row) */
    int voicing;               /* VOICING_CLOSE (v1, and every project saved before 0.4) or VOICING_SMOOTH; vmode CLOSE is
                                * VOICING_CLOSE, every other vmode VOICING_SMOOTH (what `.voi` saves) */
    /* 0.17 the Chords lane's voicing (seq_set_vknob, seq_set_vmode); saved as `.vx`. vstyle: -1 Custom, else a voicing style
     * (the 0.17 style table); vmode: VMODE_* (how a chord is led from the one before); vcol: the colour of every slot whose own
     * is Triad, a level -1..4; vinv: INVERSION steps -2..3; vspr: SPREAD -1..3; vstrdir: STRUM 0 low to high, 1 high to low */
    int vstyle, vmode, vcol, vinv, vspr, vstrdir;
    int voice[SEQ_SLOTS_ALL][THEORY_CHORD_MAX];   /* Chords-lane notes per progression slot (the vmode's voicing, before INVERSION,
                                                   * SPREAD and OCTAVE); recomputed by every setter that changes them (0.24 and
                                                   * per change point, after the real slots) */
    int voice_n[SEQ_SLOTS_ALL];   /* notes in voice[i]: 2..5 */
    seq_lane_t lane[SEQ_LANES];
    int harmony;               /* 0..100, the HARMONY knob */
    int harmony_seed;          /* the seed of the last generated progression */
    /* seq_slot_label's and seq_slot_numeral's text per slot (" " past prog_len), made with the voices: the getters, polled
     * on the audio thread, only copy it */
    char slot_text[SEQ_SLOTS_ALL][16], slot_num[SEQ_SLOTS_ALL][8];
    mel_recipe_t mel;          /* 0.11: Melody 1's recipe, played while its style is GEN_MEL_RECIPE (kept, unused, on any other
                                * style); new and loaded parts start at the Hook's defaults */
    /* 0.18 a GEN_CH_DRAWN Chords lane: its bars as drawn and the RHYTHM knobs. The setters render them into the lane's
     * steps (bars below filled) and into rfill, each bar's codes with FILL on top (a locked bar's as drawn), which seq_block
     * plays on a fill bar. Saved as `<P>.rb=` (bars up to filled) and `<P>.rz=` (the knobs, when not all 0); a loaded 0.12-0.17
     * Chords lane is converted to drawn bars (settle_loaded). 0.19: per drawn lane (DL_CHORDS, DL_BASS); both render and save
     * (a DRAWN Bass lane's as `<P>2.rb=` and `<P>2.rz=`; seq_core.c DLANE). */
    rbar_t rb[DL_COUNT][SEQ_MAX_BARS];
    signed char rk[DL_COUNT][RK_COUNT];   /* 0.24 CADENCE and BREATH appended */
    unsigned char rfill[DL_COUNT][SEQ_MAX_BARS][16];
    unsigned char rfill_abs[DL_COUNT][SEQ_MAX_BARS][16];   /* 0.19.1: the note each rfill hit plays, 0 the step's own (the Chords') */
    /* 0.19 a GEN_BASS_DRAWN Bass lane's notes (bass_state_t). The setters render each bar's notes into the lane's steps (abs),
     * its roles into brole (bass_span's) and the notes of its FILL variant (rfill[DL_BASS]) into rfill_abs[DL_BASS]. New
     * instances, INIT and loads: Pop Root. */
    bass_state_t bass;
    char brole[SEQ_LANE_STEPS];
    /* 0.20 a GEN_MEL_JAM Melody lane's notes (mjam_state_t). The setters render each bar below filled from the melody model
     * (mjam_bar) and the fixes into the lane's steps (abs) and into mline, the model's line per step (note, hold, tension) for
     * the labels and dots. New instances and INIT: ARP · UP, LOOP, CADENCE HOME. */
    mjam_state_t mjam;
    mjam_note_t mline[SEQ_LANE_STEPS];
    djam_t djam[DL_COUNT];   /* 0.22 the Chords and Bass lanes' JAM state (djam_t) */
    /* 0.24 each bar's change points (seq_cp_t), sorted by step, the empty entries (step -1) last. Step 0 holds one only in a bar
     * that starts inside a slot (the second bar of a 2-bar chord). PROGRESSION picks and tries, HARMONY GEN, ideas, moves and JAM
     * taps never touch them; a chord-length or loop change keeps them (dropping a step-0 one in a bar that now starts a slot).
     * Saved as `<P>.cp=` only when one exists */
    seq_cp_t cp[SEQ_MAX_BARS][SEQ_CP_MAX];
    /* 0.24 PHRASE's hit octaves: per bar and step, -2..2 octaves the Chords lane's hit there plays from the part's octave (render_one_k
     * adds it to the step's oct). It belongs to the step's hit: painting the hit away clears it; RESET BAR clears the bar's. Saved as
     * `<P>.ho=` (bars up to filled with any) */
    signed char hoct[SEQ_MAX_BARS][16];
    /* 0.24 LOCK WITH (LW_*) and ANTICIPATE (AN_*) per drawn lane, default OWN and OFF (new instances, INIT, loads without them): one
     * lane follows the other at most (setting one follows-the-other turns the other OWN). Saved as `<P>1.la=` / `<P>2.la=` when not
     * default. sflag: each rendered step's SF_* (the setters render it with the steps; djq_commit installs a queued block's) */
    signed char lw[DL_COUNT], an[DL_COUNT];
    unsigned char sflag[DL_COUNT][SEQ_LANE_STEPS];
} seq_part_t;

/* 0.22 a JAM block queued on a drawn lane of the playing part (seq_t.djq[DL_*], seq_jam_queue): c, l the block (c < 0 nothing queued),
 * part the part it is for, tag the seq_snap_t.jq its tap's undo entries carry (cancelling the queue removes them). The setters apply
 * it to a copy of the part (seq_t.jam_tmp), render it there and keep here everything the lane plays from (as MELODY's mj_next):
 * steps (bars below filled), rb, rfill, rfill_abs, grid16; the Bass's brole and note settings with the auto note-locks (bset, frozen,
 * nlock); the Chords' voicing (vstyle .. vstrdir, STRUM, OCTAVE, voicing), voices and slot labels. seq_block copies them into the
 * lane at its SWITCH AT boundary (no render, no allocation, no wait). Not saved. */
typedef struct {
    signed char c, l;
    int part, tag;
    seq_step_t steps[SEQ_LANE_STEPS];
    rbar_t rb[SEQ_MAX_BARS];
    unsigned char rfill[SEQ_MAX_BARS][16], rfill_abs[SEQ_MAX_BARS][16];
    unsigned char grid16;
    char brole[SEQ_LANE_STEPS];
    bass_set_t bset, frozen[SEQ_MAX_BARS];
    unsigned short nlock;
    int vstyle, vmode, vcol, vinv, vspr, vstrdir, strum, oct, voicing;
    int voice[SEQ_SLOTS_ALL][THEORY_CHORD_MAX], voice_n[SEQ_SLOTS_ALL];   /* 0.24 the change points' too */
    char slot_text[SEQ_SLOTS_ALL][16], slot_num[SEQ_SLOTS_ALL][8];
    unsigned char sflag[SEQ_LANE_STEPS];   /* 0.24 its steps' flags (ANTICIPATE's) */
    signed char hoct[SEQ_MAX_BARS][16];    /* 0.24 final review (ruling 32): the Chords' hit octaves as the block's codes leave them */
    /* 0.24 the other drawn lane when it follows this one (LOCK WITH, fon 1): its steps as the block's codes make them, installed with it */
    struct {
        unsigned char fon;
        seq_step_t steps[SEQ_LANE_STEPS];
        unsigned char rfill[SEQ_MAX_BARS][16], rfill_abs[SEQ_MAX_BARS][16], sflag[SEQ_LANE_STEPS];
        char brole[SEQ_LANE_STEPS];
    } f;
} djq_t;

/* 0.19.1 seq_snap_t.bkeep, what a Bass snapshot's undo puts back of the note settings: BKEEP_PO PEDAL and OCTAVE (alone: a PEDAL
 * or OCTAVE step); BKEEP_KNOBS the style and knobs k[] too (a STYLE or RESET KNOBS step, or the STYLE browser's open); but not
 * knob k (BK_*) with bit BKEEP_TURNED << k, turned in the STYLE browser (stamped on its step at DONE, never undone) */
enum { BKEEP_PO = 1, BKEEP_KNOBS = 2, BKEEP_TURNED = 4 };

/* An undo snapshot: a lane as it was (lane >= 0), or the part's progression (lane -1), and the part's length then.
 * join: undone together with the snapshot before it (NEW IDEA's lanes are one undo step). */
typedef struct {
    int part, lane, bars, filled, join;
    int recipe;                  /* 0.10: a recipe lane's KEEP or move: undo restores its complexity and MOVEMENT, SYNC,
                                  * OCTAVES, REGISTER, PUSH too */
    int shape[LSHAPE_COUNT];     /* the lane's SHAPE row then */
    seq_lane_t ln;
    mel_recipe_t mel;            /* 0.11: the part's melody recipe then; a Melody 1 snapshot's undo restores it (its knobs
                                  * only with `recipe`, as the complexity) */
    int prog[SEQ_MAX_PROG], prog_len, harmony_seed;
    unsigned char src[SEQ_MAX_PROG], colour[SEQ_MAX_PROG];
    uint64_t slot_lock;
    int chord_len, filled_slots, phrase, rank_len;   /* 0.14: a progression snapshot restores these too (a chord-length
                                                      * change's undo) */
    int vox;                     /* 0.17: a voicing style's application: undo restores vstyle, vmode, vcol, vinv, vspr, vstrdir,
                                  * STRUM and OCTAVE (vx) too; any other progression snapshot leaves the voicing as it is */
    int vx[8];
    int txn;                     /* 0.17: pushed by the open picker or browser itself (a pick, a browser's open): its CANCEL drops it */
    int knobs;                   /* 0.17.1: RESET KNOBS's lane snapshot: undo restores the lane's complexity and whole SHAPE row;
                                  * 0.18: 2 for RHYTHM's RESET KNOBS, whose undo restores rk too (1: CHORDS', it leaves rk) */
    rbar_t rb[SEQ_MAX_BARS];     /* 0.18: the part's drawn bars; a Chords snapshot's undo restores them (and rk with knobs). 0.19: the
                                  * snapshot lane's (a Chords or Bass snapshot; any other leaves them unset) */
    signed char rk[RK_COUNT];
    /* 0.19 a Bass snapshot's notes: its undo restores the note locks, frozen settings and fixed notes, and of the note settings
     * (bass.set) what bkeep says (BKEEP_*; 0: none of them); knob turns are not undo steps */
    unsigned char bkeep;
    bass_state_t bass;
    mjam_state_t mjam;           /* 0.20 a Melody snapshot's JAM notes: its undo restores the locks, frozen settings and fixes (not
                                  * the settings or SWITCH AT: knob turns are not undo steps) */
    signed char djc, djl;        /* 0.22 a Chords or Bass snapshot's playing JAM block (djam c, l): its undo puts it back (a JAM tap's
                                  * undo then reads the block before it, not "(edited)"); the JAM knobs stay */
    int jq;                      /* 0.22 pushed by a queued JAM tap: its queue's tag (djq_t.tag), which a cancel removes; else 0 */
    seq_cp_t cp[SEQ_MAX_BARS][SEQ_CP_MAX];   /* 0.24 the change points: a progression snapshot's undo restores them */
    signed char hoct[SEQ_MAX_BARS][16];      /* 0.24 the hit octaves: a drawn lane's snapshot's undo restores them (final review: the
                                              * Bass's too, the Chords following it lose those of the hits it takes away) */
    unsigned char prog_too;                  /* 0.24 a PHRASE edit's lane snapshot whose undo restores the progression too (one step) */
    signed char lw[DL_COUNT], an;            /* 0.24 a Chords or Bass snapshot's: both lanes' LOCK WITH (a set may turn the other OWN), its ANTICIPATE */
} seq_snap_t;

/* 0.17 the CHORDS page's views. CVIEW_PROG / VOICE are open transactions (seq_cview); the user picks the others.
 * 0.18: CVIEW_BAR (a bar open on RHYTHM), CVIEW_COPY (its COPY TO... picker) and CVIEW_RBROWSE (the RHYTHMS browser, in a bar or
 * not) are open transactions too. 0.24.1 CVIEW_PICK (the FIX chord picker) and CVIEW_FIX are retired: never a view; they keep the
 * numbering (cview's options) */
enum { CVIEW_CHORDS, CVIEW_PICK, CVIEW_PROG, CVIEW_VOICE, CVIEW_FIX, CVIEW_RHYTHM, CVIEW_BAR, CVIEW_COPY, CVIEW_RBROWSE, CVIEW_JAM };   /* 0.22 JAM */
/* 0.19 the BASS tab's own transaction kinds (seq_txn_t.kind), numbered after every CVIEW_* value: the bar editor and the STYLE
 * browser. Its RHYTHM transactions keep CVIEW_BAR / COPY / RBROWSE, told apart by seq_txn_t.lane. Never a view (seq_bview's BVIEW_*).
 * 0.22 renumbered after CVIEW_JAM (never saved) */
enum { TXN_BEDIT = CVIEW_JAM + 1, TXN_BSTYLE, TXN_MPICK, TXN_MEDIT };   /* 0.20: the MELODY tab's ANSWER BLOCK picker and bar editor.
                                                                     * 0.24 TXN_BEDIT retired (the bar editor is PHRASE's CVIEW_BAR) */

/* 0.17 an open chord picker or PROGRESSIONS / VOICING browser (not saved): what to put back on CANCEL. kind CVIEW_CHORDS: none. */
typedef struct {
    int kind, part, slot;
    int lane;                    /* 0.19: the lane it edits (LANE_CHORDS: the CHORDS tab's; LANE_BASS: the BASS tab's) */
    int stale;                   /* seq_block switched the part under it (the audio thread only sets this); the next setter cancels */
    int pushes0;                 /* seq_t.undo_pushes at open */
    int own;                     /* the pushes since that are its own (seq_snap_t.txn); any other: the ring is not put back whole */
    int undo_n, undo_top;        /* the undo ring as it was at open, and a copy of it (a push may drop the oldest snapshot) */
    seq_snap_t ring[SEQ_UNDO];
    int prog[SEQ_MAX_PROG], prog_len, filled_slots, phrase, rank_len;   /* the progression (a picker: its slot, and the phrase) */
    unsigned char src[SEQ_MAX_PROG], colour[SEQ_MAX_PROG];
    uint64_t lock;
    int vx[8];                   /* the voicing, as seq_snap_t.vx */
    /* 0.18 an open bar (kind CVIEW_BAR, slot the bar) or RHYTHMS browser (kind CVIEW_RBROWSE, slot -1): the drawn bars at open.
     * sub: the open bar's COPY TO... picker (CVIEW_COPY, its picked bars in copy_mask) or RHYTHMS browser (CVIEW_RBROWSE, the
     * bar as it opened in sub_rb), else 0. 0.19: the transaction lane's bars */
    rbar_t rb[SEQ_MAX_BARS];
    int sub;
    unsigned copy_mask;
    rbar_t sub_rb;
    signed char sub_hoct[16];    /* 0.24 final review: that bar's hit octaves at the RHYTHMS browser's open (its CANCEL puts them back) */
    /* 0.19 the BASS tab's: the part's note settings at open (the STYLE browser's CANCEL puts them back). 0.24: bsel the open PHRASE
     * bar's (CVIEW_BAR) selected step, boct the Bass's note octave (1..4); bvar, bnlock, bfix the Melody bar editor's (below) */
    bass_set_t bset;
    unsigned char bvar, bnlock, bfix[SEQ_STEPS];
    int bsel, boct;
    /* 0.24 an open PHRASE bar's (CVIEW_BAR) CANCEL also puts back, as at open: the progression (prog .. lock above), the change points,
     * hit octaves, the lane's NEW BAR versions (bvars), and the Bass's fixed notes, note locks and frozen settings (bass, not its
     * settings). copy_what: its COPY's 0 RHYTHM ONLY, 1 + CHORDS / + NOTES (seq_rcopy_set_what). Final review (ruling 32): hoct is
     * saved for the RHYTHMS browser too (either drawn lane: a try clears the octaves of the hits it takes away) */
    seq_cp_t cp[SEQ_MAX_BARS][SEQ_CP_MAX];
    signed char hoct[SEQ_MAX_BARS][16];
    unsigned char bvars[SEQ_MAX_BARS];
    bass_state_t bass;
    int copy_what;
    signed char lw;              /* 0.24 a RHYTHM transaction's lane's LOCK WITH at open (a lane-wide pick or try turns it OWN; CANCEL puts it back) */
    unsigned char bturned;       /* the STYLE browser's: bit BK_*, a knob turned since its last try (CANCEL and UNDO keep its value) */
    /* 0.20 the MELODY tab's: the part's JAM settings at open (the ANSWER BLOCK picker's CANCEL puts back their SWAP block, sc and sl);
     * the bar editor's bar's frozen settings at open (with bvar, bnlock, bfix, bsel and boct as the Bass bar editor's: boct 3..7) */
    mjam_set_t mset, mfrozen;
    int bstep;                   /* 0.19.1 the STYLE browser's: its open's undo step (seq_t.undo index); DONE stamps bturned on it */
} seq_txn_t;

/* 0.19 a tab's view state (seq_t.tv, none of it saved). view: the user's view (the CHORDS tab: CVIEW_CHORDS, CVIEW_JAM or
 * CVIEW_RHYTHM); rgroup: the RHYTHMS browser's feel chip (rhythm.h group); browse_genre: the browser's genre chip (CHORDS:
 * VOICING's); the strip: paging 1 while the page arrows paused FOLLOW (hold_page), page_hand the page FOLLOW off shows, csel the
 * centre slot when paused or FOLLOW off (the last slot tapped; a pause starts on the one shown), snap_page and snap_centre what
 * the strip showed when the tab's transaction opened. 0.24 (ruling 18) cbar, snap_cbar: the CHORDS tab's centre as a bar beside its slot
 * (csel, snap_centre), kept in the same places, so PHRASE's centre can be a 2-bar chord's second bar (seq_centre_bar) */
typedef struct { int view, rgroup, browse_genre, paging, hold_page, page_hand, csel, snap_page, snap_centre, cbar, snap_cbar; } seq_tabview_t;

/* 0.17 KEYS: a set of keys in the window MIDI SEQ_KEY_LO..SEQ_KEY_HI (bit i: key SEQ_KEY_LO + i); fold: the lit keys a note
 * from outside the window landed on (a key a note sits on itself is not folded); edge[0] / [1]: a note came from below / above */
#define SEQ_KEY_LO 24
#define SEQ_KEY_HI 83
typedef struct { uint64_t lit, fold; unsigned char edge[2]; } seq_keys_t;

typedef struct {
    int key, scale;            /* the whole project's: 4 = E, SCALE_MINOR */
    int chan[SEQ_LANES];       /* each lane's MIDI channel, 1-16: 1, 2, 3 new; 2, 3, 4 for a state saved before 0.7 */
    int swing, gate, dyn;      /* the output stage, 0..100 each; neutral (0, 50, 50) plays exactly as 0.6 */
    seq_part_t part[SEQ_PARTS];
    int cur_part;              /* the playing part, which the setters edit */
    int queued;                /* the part seq_block switches to at the next bar line, -1 none */
    int used_mask, stale_mask; /* bit p: part p is in use; part p missed a key or scale change (catches up when selected) */
    long part_origin_k;        /* the 16th the playing part's bar 1 began on (0 after a stop); its position counts from it */
    unsigned char mute[SEQ_LANES];   /* lane mutes, for every part (a performance setting) */
    int unsaved_mask;          /* bit p: the last seq_state_save left part p out (seq_unsaved_parts); not saved */
    int view_bar;              /* the bar picked (0-based): the grid shows and edits it unless following (seq_grid_bar) */
    int follow;                /* 1 (the default): while playing the grid, the FIX pages and the bar strip show the playing
                                * bar (seq_grid_bar). A GLOBAL setting: bar picks leave it; saved as `fo=0` only when off */
    int preset;                /* 0.23 the genre preset last loaded (src/presets.c), 1..SEQ_PRESETS_MAX, 0 none: saved as `gn=`
                                * only when set; edits keep it, INIT clears it */
    int bass_mode;             /* 0.10: the BASS tab's mode, 0 IDEAS, 1 SHAPE, 2 FIX (engine `bmode`); not saved */
    int mel_mode;              /* 0.11: the MELODY tab's mode, 0 RECIPE, 1 IDEAS, 2 SHAPE, 3 FIX (engine `mmode`); not saved */
    unsigned display_rev;      /* bumped when a value MPC must re-read changes (the wrapper's HAS_DISPLAY_REV): taps, a part switch, a load */
    int playing;
    double bpm;        /* the host tempo of the last block, for the engine's readout; not saved */
    double last_ppq;   /* high-water mark: the end of the interval already played */
    long next_k;       /* next 16th index (song position * 4) allowed to fire */
    long cur_k;        /* the 16th last fired, -1 when stopped */
    /* 0.20 a JAM block switch queued on the playing part (seq_mel_queue): mjq_c, mjq_l the block, mjq_c < 0 nothing queued; mjq_part
     * the part it is for. The setters render the whole loop with it (every bar below filled) into mj_next and mj_next_line, again with
     * every render of that part; seq_block copies them into the lane at the SWITCH AT boundary and sets mj_switched, which
     * seq_mel_poll reads and clears on the screen side. Not saved. */
    seq_step_t mj_next[SEQ_LANE_STEPS];
    mjam_note_t mj_next_line[SEQ_LANE_STEPS];
    unsigned char mj_next_grid16;   /* 0.21 the queued block's swing grid and weak flags (MW_*), which mel_commit copies in */
    unsigned char mj_next_weak;
    signed char mjq_c, mjq_l;
    int mjq_part;
    volatile int mj_switched;
    /* 0.22 a JAM block queued on the playing part's Chords and Bass lanes (djq_t), each with its own SWITCH AT (djam_t.sw); jam_tmp the
     * scratch part the setters render a queued block on (never allocated); dj_switched bit d: drawn lane d's block took over in time
     * (seq_jam_poll reads and clears it); djq_drop[d] the tag of a queue a part switch dropped, whose undo entries the next UNDO or tap
     * removes (0 none); djq_renders the scratch renders made (tests, the cost report). Not saved. */
    djq_t djq[DL_COUNT];
    int djq_drop[DL_COUNT];
    volatile int dj_switched;
    unsigned djq_renders;
    seq_pending_t pending[SEQ_MAX_PENDING];
    int npending;
    seq_strum_t strum[SEQ_MAX_STRUM];   /* STRUM's queued note-ons, in time order per chord; 0.21 and a swung JAM Melody note (0.22 and a
                                         * swung JAM Chords or Bass hit's) */
    int nstrum;
    long pushed_k[SEQ_ACTIVE_LANES];   /* per lane: the step whose PUSH already played, -1 none */
    long early_k[SEQ_ACTIVE_LANES];    /* 0.24 per lane: the change step ANTICIPATE's last early hit ties over (it sounds into it), -1 none */
    unsigned chase;                    /* 0.24 bit lane: after a start or a resync that ended its notes, a tied step (SF_TIED) its first
                                        * step lands on plays the early hit's chord / note there (it never sounded) */
    int lw_bumped;                     /* 0.24 1 after a LOCK WITH set turned the other lane OWN, until the next setter (seq_lockwith_bumped) */
    seq_hum_t hum[DL_COUNT];           /* 0.18: HUMANIZE's waiting hit (0.19: per drawn lane) */
    seq_audition_t aud[SEQ_MAX_AUDITION];   /* 0.12: auditions the setters queued, sent at the next seq_block's start */
    int naud;
    int fix_prev;      /* 0.12: the bar playing before the latest bar change of this part's playback, -1 none (a start, a
                        * part switch); fix_ppq: the song position of that change (seq_fix_bar) */
    double fix_ppq;
    double idle_ppq;   /* 0.12: while stopped, the pending queue's clock (beats of stopped blocks), so audition offs fire */
    int names;         /* 0.12: chord text as names (1) or degrees (0, the default); the engine's NAMES toggle; not saved */
    char move_text[SEQ_MOVE_TEXT];   /* 0.12: the last move's description, "" none; not saved */
    int move_lane;                   /* the lane whose move set it (LANE_CHORDS, LANE_BASS, LANE_MEL_UP) */
    double move_secs;                /* seconds of playback left before move_text clears (long enough to read) */
    seq_emit_fn emit;
    void *ctx;
    seq_part_t load_tmp;   /* seq_state_load parses here first, so a bad state changes nothing; per instance, since
                            * every plugin instance loads its own state and they must not share scratch */
    seq_part_t jam_tmp;    /* 0.22 a queued JAM block's scratch part (djq_render), per instance as load_tmp */
    unsigned gen_counter;  /* mixed into every new seed; not saved (the seeds are) */
    seq_snap_t undo[SEQ_UNDO];   /* a ring: undo_top is the next slot written, undo_n the snapshots held */
    int undo_top, undo_n;
    int undo_pushes;           /* snapshots ever pushed (a transaction's stale cancel counts its own); not saved */
    seq_txn_t txn;             /* 0.17: the open picker or browser, none of it saved (0.19: one across both tabs; txn.lane's) */
    seq_keys_t keys;           /* 0.17 KEYS: the keys the last Chords-lane chord lights (audio thread writes, UI reads; not saved) */
    seq_tabview_t tv[TAB_COUNT];   /* 0.19: each tab's view, browser chips, strip and FOLLOW pause (0.17/0.18: the CHORDS tab's) */
    seq_gen_cache_t gen_cache[2];   /* Melody 1's and Melody 2's rhythm caches; not saved */
} seq_t;

extern const char *const SEQ_PART_NAME[SEQ_NPARTS];   /* "Verse", "Intro", "Pre", "Chorus", "Bridge", "Outro" */
extern const int SEQ_PART_SCREEN[SEQ_NPARTS];         /* screen order -> part index: Intro, Verse, Pre, Chorus, Bridge, Outro */

/* The playing part generated at c 30, seed 1: Chords Pulse, Bass Root, Melody 1 Hook, Melody 2 Answer (0.15: the engine then
 * puts a new instance at INIT, seq_init_preset). 0.18: Chords DRAWN Sustain bars at c 50; 0.19: Bass DRAWN Root bars at c 50, Pop
 * Root; 0.20: Melody JAM, ARP · UP, LOOP, CADENCE HOME at c 10 (level 1), GATE 50, PUSH 0, DYNAMICS 50 */
void seq_init(seq_t *s, seq_emit_fn emit, void *ctx);
/* 0.15 INIT (MPC's PRESET menu): the whole instance as a safe start. C major, 1-5-6-4 Pop, 4 bars of 1-bar chords, close
 * voicing; Chords Sustain and Bass Root at COMPLEXITY 0 (one chord, one root an octave below it, per bar; 0.19: the Bass DRAWN
 * Root bars at COMPLEXITY 50, Pop Root: the same onsets and lengths, its notes in E1-D#2); Melody the Arp
 * recipe Up-down (0.20: JAM, ARP · UP, LOOP, CADENCE HOME at COMPLEXITY 10, GATE 50, PUSH 0, DYNAMICS 50); every knob neutral, no mutes; the Verse alone (the other parts copy it on first use). The MIDI
 * channels stay. Held notes end and the song position is kept, as on a state load (FOLLOW kept, the FIX picks back to
 * the first); clears the undo history. */
void seq_init_preset(seq_t *s);
void seq_reset_v1(seq_t *s);   /* the playing part back to v1's song, every lane MANUAL; clears the undo history */

/* Once per host block: ppq = song position at the block start (-1 when unknown), bpm, transport playing, frame count. */
void seq_block(seq_t *s, double ppq, double bpm, int playing, int frames);

/* MANUAL lane: a melody re-applies its arp recipe to that bar; Chords and Bass change only the step (on: the root at
 * the lane's octave, vel 100, length 1). Generated lane: locks the step (pushing an undo snapshot) and
 * turns it on as the generator has it, or as the root at the lane's octave if the generator has it off. 0.18, a drawn
 * Chords lane (the step as it plays): on draws a HIT, off clears the drawn hit and its holds (none there: nothing); one
 * undo step, and the bar reads edited; an open bar or RHYTHMS browser is closed as DONE first. 0.19: a DRAWN Bass lane the same. */
void seq_set_step(seq_t *s, int lane, int bar, int step, int on);
void seq_set_mute(seq_t *s, int lane, int muted);   /* global: the lane stays muted in every part */
void seq_set_channel(seq_t *s, int lane, int ch);   /* 1-16, else ignored; the lane's held notes end on the old channel first */
int seq_channel_of(const seq_t *s, int lane);
/* The output stage, 0..100 each, else ignored. SWING delays odd 16ths by up to a triplet (1/12 beat at 100); GATE scales
 * note lengths (x0.2 at 0, x1 at 50, x2 at 100); DYNAMICS scales velocities (x0.4 at 0, x1 at 50, x1.6 at 100). */
void seq_set_swing(seq_t *s, int v);
void seq_set_gate(seq_t *s, int v);
void seq_set_dynamics(seq_t *s, int v);
int seq_swing(const seq_t *s);
int seq_gate(const seq_t *s);
int seq_dynamics(const seq_t *s);
/* 1, 2, 4, 8, 16; growing repeats the bars into never-filled ones. 0.14: the slots too (the slots past filled_slots
 * repeat the loop: its phrase when the loop repeats one, so a 0.13 progression goes on as 0.13 played it, else slots
 * 0..old-1), chords and locks; shrinking hides slots; a grow back shows them as they were. No undo (as 0.13); an undo of
 * an older snapshot grows the restored slots the same way. */
void seq_set_bars(seq_t *s, int bars);
/* 32, 16, 8, 4. 0.14: remaps the slots by time, one undo step: new slot i takes the old slot sounding at step
 * i x new length, (i x new / old) % old slots, its src, colour and lock with it (shorter repeats each chord, longer keeps
 * the first of each new span); the undo restores the slots and the length exactly. */
void seq_set_chord_len(seq_t *s, int steps);
void seq_set_follow(seq_t *s, int on);         /* off while playing holds the playing bar. On in a new instance and after a
                                                * load of a state without `fo=0`; INIT keeps it. */
int seq_follow(const seq_t *s);
/* 0.23 the genre preset last loaded (src/presets.c), saved with the project: 0 none, else 1..SEQ_PRESETS_MAX (others ignored).
 * For the genre's NEW TAKE button (planned: spec 2026-10-23-genre-presets-design.md), which needs the genre after a reload */
#define SEQ_PRESETS_MAX 16
int seq_preset_loaded(const seq_t *s);
void seq_set_preset_loaded(seq_t *s, int n);
void seq_set_view(seq_t *s, int bar);          /* 0-based; out of range shows bar 1. Keeps FOLLOW (while it plays, the grid
                                                * shows the playing bar; the pick shows once stopped or FOLLOW is off) */
void seq_set_key(seq_t *s, int key);            /* 0..11; else ignored */
void seq_set_scale(seq_t *s, int scale);        /* 0..SCALE_COUNT-1; else ignored */
void seq_set_prog_preset(seq_t *s, int i);      /* 0..THEORY_NPRESETS-1; fills the loop with its phrase, every lock cleared */
void seq_prog_step(seq_t *s, int i);            /* 0.17 PROGRESSION's arrows: the preset's phrase in the unlocked slots (as
                                                * seq_prog_try, locks stay), one undo step; all locked: nothing */
void seq_set_voicing(seq_t *s, int v);          /* VOICING_CLOSE / VOICING_SMOOTH: sets vmode VMODE_CLOSE / VMODE_SMOOTH */
void seq_set_vmode(seq_t *s, int mode);         /* 0.17: VMODE_*; else ignored. Keeps `voicing` in step */
void seq_all_notes_off(seq_t *s);

/* Parts. A part is in use once selected or queued; its first use makes it a copy of the Verse. Stopped, a selection
 * switches at once; playing, it queues the part, which starts from its own bar 1 at the playing part's next bar line
 * (held notes end first). Selecting the queued part, or the playing one, cancels the queue; a stop starts a queued part
 * at once. Key and scale changes regenerate the playing and the queued part; the others catch up in this call. */
int  seq_part(const seq_t *s);                 /* the playing (and edited) part */
int  seq_queued_part(const seq_t *s);          /* -1 when nothing is queued */
void seq_select_part(seq_t *s, int part);      /* 0..SEQ_NPARTS-1, else ignored */
int  seq_part_used(const seq_t *s, int part);

/* The generator (gen.h) on the playing part. Each regenerates what it changes; none runs on the audio thread.
 * An Answer on Melody 2 always takes Melody 1's steps as its lead (whatever Melody 1's style, Manual included), so
 * every change to Melody 1's steps regenerates it too. */
void seq_set_style(seq_t *s, int lane, int style);   /* Chords: MANUAL or CH_*; Bass: MANUAL or BASS_*; Melody 1: MANUAL,
                                                      * MEL_HOOK, MEL_ARP or MEL_RECIPE (the part's recipe, as it is);
                                                      * Melody 2: MANUAL, MEL_ANSWER or MEL_ARP; else
                                                      * ignored. MANUAL keeps the steps and clears the locks. 0.18, a
                                                      * GEN_CH_DRAWN Chords lane: an old CH_* style's library rhythm in
                                                      * every unlocked bar (one undo step; an open bar or browser closed
                                                      * as DONE first); MANUAL: ignored. seq_style
                                                      * then reads the old style of bar 1's rhythm, else Pulse */
void seq_set_complexity(seq_t *s, int lane, int c);  /* 0..100; pushes no undo (turning it back restores the lane). 0.18,
                                                      * a DRAWN Chords lane: revoices only when the harmony layers move and
                                                      * renders only the bars whose hits change (all when SYNC or GHOSTS
                                                      * move); see seq_vknob_eff. 0.20, the JAM Melody lane: queues the
                                                      * playing column's level min(4, c / 20) as seq_mel_queue */
void seq_generate(seq_t *s, int lane);               /* GEN: a new seed for a generated lane. Chords: NEW RHYTHM,
                                                      * which always changes the rhythm (a new variation, or the next
                                                      * rhythm: seq_next_rhythm), one undo step; Manual: nothing,
                                                      * "NEW RHYTHM: pick a rhythm first"; 0.18 drawn bars: nothing
                                                      * (0.19: a DRAWN Bass too, no undo) */
int seq_next_rhythm(int style);                      /* NEW RHYTHM's family order; other styles: themselves */
const char *seq_rhythm_name(int style);              /* a chord rhythm's name ("Half-time"); other styles: "" */
void seq_generate_all(seq_t *s);                     /* NEW IDEA: GEN on every generated lane, one undo step (0.18:
                                                      * a drawn Chords lane is left; 0.19: a drawn Bass lane too) */
void seq_undo(seq_t *s);                             /* restores the last snapshot; nothing to undo: no-op */
void seq_set_harmony(seq_t *s, int c);               /* 0..100; stores only */
void seq_generate_progression(seq_t *s);             /* a new phrase of the phrase's length (2..8) at HARMONY, filled
                                                      * over the loop; locked slots keep theirs */
int seq_style(const seq_t *s, int lane);
int seq_complexity(const seq_t *s, int lane);
int seq_harmony(const seq_t *s);
int seq_can_undo(const seq_t *s);

int seq_bars(const seq_t *s);
int seq_chord_len(const seq_t *s);
int seq_view_bar(const seq_t *s);      /* the bar picked (the engine's `view` option) */
/* 0.12: the bar the grid and the FIX pages show and edit: the playing bar (seq_cur_bar) while FOLLOW is on and playing,
 * else view_bar. Always below the playing part's length. seq_step_on, seq_cell_label and the engine's FIX labels read it;
 * the engine passes it as the bar to seq_set_step, seq_set_step_note and seq_toggle_step_lock. */
int seq_grid_bar(const seq_t *s);
int seq_strip_bar(const seq_t *s);     /* the bar strip's lit bar: seq_grid_bar */
/* 0.12: the bar a FIX NOTE pick or lock edits: the bar on screen, seq_grid_bar, except that while following and playing
 * a pick within 0.5 beat after a bar change edits the bar before it (what the screen still showed: MPC redraws late).
 * Never a bar past the playing part's length. The engine passes it to seq_set_step_note and seq_toggle_step_lock. */
int seq_fix_bar(const seq_t *s);
int seq_key(const seq_t *s);
int seq_scale(const seq_t *s);
int seq_prog_preset(const seq_t *s);   /* index of the preset whose phrase fills the loop (its length first), or -1 */
int seq_voicing(const seq_t *s);

/* Each lane plays on its own MIDI channel (seq_set_channel): new instances 1, 2, 3; states saved before 0.7 load as 2, 3, 4. */
int seq_cur_step(const seq_t *s);   /* 0..15 within the bar, or -1 when stopped */
int seq_cur_bar(const seq_t *s);    /* 0..bars-1; 0 when stopped */
int seq_cur_slot(const seq_t *s);   /* progression slot playing, or -1 when stopped */
int seq_slot_at(const seq_t *s, int bar, int step);               /* the progression slot under a step (0.24: a change point's
                                                                   * virtual slot, >= SEQ_MAX_PROG, from its step on) */
int seq_slot_name(const seq_t *s, int slot, char *buf, int len);  /* "Em7" (seq_slot_label); " " for a slot the loop never reaches */
/* 0.24 change points (seq_part_t.cp) of the playing part's bar (0..SEQ_MAX_BARS-1, hidden bars included; else 0): their count, and
 * point k (0 when there is none). seq_chord_change_at: 1 when a chord starts at the step of a bar of the loop: a point, or a slot
 * boundary whose chord (degree, borrow, colour as it plays) differs from the step before's (the loop wraps). seq_chord_label: the
 * bar's chords in time order, "C\xC2\xB7G@9" (the first plain, each later one at its step + 1; degrees or names as seq_names), cut
 * to len; returns its length as copy_text. seq_cp_set / seq_cp_clear / seq_cp_clear_bar: a point set (a step that has one: its chord
 * replaced), cleared, or every point of a bar cleared, on the playing part, revoiced and its harmony lanes rendered; no undo step
 * (the PHRASE editor's picks add one). Set
 * returns 0 for a full bar, a bad value, a bar past the loop or step 0 of a bar that starts a slot (a slot change there).
 * The slot getters (seq_slot_name, _label, _numeral, _deg, _src, _colour, _locked: always 1; seq_part_eff_colour,
 * seq_audition_chord) take a point's virtual slot of a bar of the loop too; the slot setters and the picker ignore it */
int  seq_cp_count(const seq_t *s, int bar);
int  seq_cp_get(const seq_t *s, int bar, int k, seq_cp_t *out);
int  seq_chord_change_at(const seq_t *s, int bar, int step);
int  seq_chord_label(const seq_t *s, int bar, char *buf, int len);
int  seq_cp_set(seq_t *s, int bar, int step, int deg, int src, int type);
void seq_cp_clear(seq_t *s, int bar, int step);
void seq_cp_clear_bar(seq_t *s, int bar);

/* Cell state and text for the grid's bar (seq_grid_bar): the chord name on the Chords lane (coloured, as seq_slot_name), else the note name ("E4"); " " when the
 * step is off. */
int seq_step_on(const seq_t *s, int lane, int step);
int seq_cell_label(const seq_t *s, int lane, int step, char *buf, int len);
/* 0.10: the note a lane's step plays (on or off) in any part (0..SEQ_NPARTS-1), as seq_cell_label names it (the Chords lane:
 * its lowest); -1 for a bad argument. For labels made in the setters for a part not yet playing (a queued one). */
int seq_part_step_note(const seq_t *s, int part, int lane, int bar, int step);
/* 0.22 every note it plays (a chord's, ascending; out holds THEORY_CHORD_MAX), as the step sounds; the count, 0 for a bad argument */
int seq_part_step_notes(const seq_t *s, int part, int lane, int bar, int step, int out[THEORY_CHORD_MAX]);

/* Every part in use, its steps and the key as text, for the plugin state saved with the MPC project (< SEQ_STATE_MAX):
 * the Verse as part A (all that 0.3-0.7 read), the others as B-F, each whole or left out when it does not fit.
 * A load plays the Verse from the song position, the saved parts in use and ready, the rest unused, the mutes from A's
 * flags; a part B-F with a missing or bad field loads as unused. 0.9's per-part `.pq`, `.sl` and `.cs` and 0.10's `2.bs`
 * and `<n>.ab` are written only when not neutral and never refuse a load (a bad one loads neutral).
 * 0.14: a part's `.prog`, `.pq` and `.sl` hold the loop's phrase (seq_part_t.phrase slots, as 0.13 wrote its progression;
 * a 0.13 project saves back byte for byte); a loop with no period of 16 or less writes its first 16 slots there (0.13
 * loads them and plays them repeating) and every slot in `<P>.ch=` (4 characters a slot: degree, src, colour, lock). A
 * load takes a valid `.ch` of the loop's length, else unrolls the phrase over the loop (never refused). `<P>.rk=` the
 * COLOUR ranks' period (seq_part_t.rank_len), only when it is not the phrase; a bad one is ignored (the phrase's). */
int seq_state_save(seq_t *s, char *buf, int len);
int seq_state_load(seq_t *s, const char *str);   /* 1 if it was valid and applied; csq1 (v1) and csq2 */
int seq_song_load(seq_t *s, const char *str);    /* 0.23 a csq2 state as seq_state_load, the MIDI channels and FOLLOW kept (a preset) */
int seq_unsaved_parts(const seq_t *s);           /* bit p: the last save left part p out (it did not fit) */

/* 0.9: per-slot chord quality, slot locks and the Chords lane's SHAPE, on the playing part. A slot plays the chord on its
 * degree from its scale (src) in its colour; every lane resolves through it at playback (gen.c is unchanged). Each setter
 * revoices and regenerates the lanes that read harmony; none runs on the audio thread. */
void seq_set_slot(seq_t *s, int slot, int deg, int src, int colour);   /* a pick (PHRASE's phrase_slot): sets and locks the slot; one undo step. src > 0
                                                                        * ignores deg (the borrow's degree). Bad values: ignored */
void seq_toggle_slot_lock(seq_t *s, int slot);                         /* no undo; locking a Knobs chord writes the COLOUR knob's level */
int  seq_slot_locked(const seq_t *s, int slot);
int  seq_slot_deg(const seq_t *s, int slot);
int  seq_slot_src(const seq_t *s, int slot);
int  seq_slot_colour(const seq_t *s, int slot);   /* the slot's own colour (the COLOUR knob's is not included) */
int  seq_prog_len(const seq_t *s);   /* 0.14: the slot count, seq_slots */
int  seq_slots(const seq_t *s);      /* 0.14: the playing part's slots, max(1, bars x 16 / chord_len), 1..64 */
/* The progression (src, colour NULL: 0); undo 1 pushes one snapshot first. Locks are kept. 0.14:
 *   n = seq_slots: the arrays are the whole loop, every slot written (the caller merges locked slots first).
 *   any other n (clamped 1..8): a phrase written into every slot by repetition, locked slots unchanged. */
void seq_set_progression(seq_t *s, int n, const int *deg, const unsigned char *src, const unsigned char *colour, int undo);
void seq_set_chord_shape(seq_t *s, int what, int v);   /* SHAPE_*: clamped to 0..100 (SHAPE_OCT -1..1); no undo, like the knobs.
                                                        * 0.17: SHAPE_COLOUR and SHAPE_SPREAD no longer act (vcol and vspr do) */
/* 0.17 the CHORDS page's COLOUR, INVERSION, SPREAD and STRUM on the playing part (VK_*), clamped to their ranges; no undo, like
 * the knobs. COLOUR colours every slot whose own colour is Triad (theory_colour_of_level) and revoices the part's lanes that
 * read colours. STRUM -8..8 sets SHAPE_STRUM to |v| x 12.5 (rounded) and the direction (- high to low); seq_vknob reads STRUM as
 * the step nearest SHAPE_STRUM, signed by the direction. A bad `what`: ignored, reads 0. seq_reset_chord_knobs (0.17.1, owner): RESET
 * KNOBS, all 8 Chords knobs to a fresh start: the VOICING style's own COLOUR, INVERSION, SPREAD, STRUM, mode and octave (Custom:
 * the four to 0), a new instance's COMPLEXITY, PUSH, GATE and DYNAMICS; one undo step, none when nothing changes. */
void seq_set_vknob(seq_t *s, int what, int v);
int  seq_vknob(const seq_t *s, int what);
/* 0.18 COMPLEXITY's layers on a DRAWN Chords lane (rhythm_cx_offsets; any other Chords lane plays as at 50). seq_vknob keeps the
 * user's value; seq_vknob_eff is what plays: COLOUR, SPREAD and STRUM plus their layers, toward the simple end below 50 (SPREAD to
 * Close, never Cluster); INVERSION as set. Per chord, COLOUR's layers act on unlocked Knobs chords, each a turn or two apart by
 * its COLOUR rank, and the Tension step (95..100) only where it makes no avoid note (theory_avoid); a 9th, 11th or 13th with a
 * 2nd whose lower note is below E3 plays one SPREAD step wider. seq_rknob_eff: RK_SYNC (COMPLEXITY's part at most +2),
 * RK_GHOSTS, RK_HUM under their layers, RK_FILL as set; a bad `what` reads 0. seq_vmode_eff: VMODE_SMOOTH where the user's
 * VMODE_CLOSE meets the voice-leading layer (75 and up), else the user's mode. */
int  seq_vknob_eff(const seq_t *s, int what);
int  seq_part_vknob_eff(const seq_t *s, int part, int what);   /* the same for any part (a bad part reads the playing one) */
int  seq_rknob_eff(const seq_t *s, int lane, int what);
int  seq_vmode_eff(const seq_t *s);
void seq_reset_chord_knobs(seq_t *s);
/* 0.18 RHYTHM page, the playing part's Chords lane (GEN_CH_DRAWN): bar 0..15. A bad bar reads all rests, -1, 0.
 * 0.19: every RHYTHM call (the bar getters, the RHYTHM knobs, RESET KNOBS, the bar editing, COPY TO..., picks and the RHYTHMS
 * browser) takes the drawn lane, LANE_CHORDS or LANE_BASS: its own bars, knobs and rhythm library (RLIB_CHORDS, RLIB_BASS). Any
 * other lane does nothing and reads as a bad bar does (0 for a knob). One transaction is open at a time across both lanes; a
 * call for the lane that has none open does nothing. */
void seq_bar_codes(const seq_t *s, int lane, int bar, unsigned char out[16]);   /* as drawn (rhythm.h RC_*); 0.24 LOCK WITH's when it follows */
void seq_bar_plays(const seq_t *s, int lane, int bar, unsigned char out[16]);   /* as it plays, after the knobs, without FILL */
int  seq_bar_plays_at(const seq_t *s, int lane, int bar, int step);              /* its step `step` alone (a polled cell's) */
int seq_bar_rhythm(const seq_t *s, int lane, int bar, int *edited);            /* the library rhythm it came from, -1 none; edited may be NULL */
int seq_bar_locked(const seq_t *s, int lane, int bar);
int seq_bar_hits(const seq_t *s, int lane, int bar);                            /* the hits it plays, as seq_bar_plays */
/* 0.24 LOCK WITH and ANTICIPATE on the playing part's drawn lane (LANE_CHORDS, LANE_BASS; any other: nothing, reads 0).
 * seq_set_lockwith(LW_*): a bar that is not painted (ed), not locked (for JAM: its rhythm lock, or on the Bass its note lock) reads the
 * other lane's drawn codes (before their knobs): WITH as they are, GAPS through rhythm_gaps. seq_bar_codes returns these effective
 * drawn codes, render plays them, and painting such a bar starts from them (and makes it ed); LOCK freezes them into the bar. Setting
 * WITH or GAPS while the other lane follows this one turns the other OWN: seq_lockwith_bumped reads 1 until the next setter. A RHYTHM
 * pick (or try) and a JAM tap turn their lane OWN. seq_bar_follows: the bar's LW_WITH / LW_GAPS when it reads the other lane's, else 0.
 * seq_set_anticipate(AN_*): at each step where a chord starts (seq_chord_change_at) and the lane hits, with the n steps before it rests
 * (wrapping into the bar before; a fill bar's FILL variant too) and neither bar locked, the hit moves n steps earlier and holds over
 * the change, playing the change's chord (the Chords: its voicing; the Bass: its root nearest the note, or the change step's fixed
 * note); the change step becomes a hold. seq_bar_plays shows it (a hold carried over a bar line too). seq_step_flags: the rendered
 * step's SF_* bits. Each set is one undo step (none when it is so already); an open transaction is kept (DONE) first. */
void seq_set_lockwith(seq_t *s, int lane, int v);
int  seq_lockwith(const seq_t *s, int lane);
int  seq_lockwith_bumped(const seq_t *s);
int  seq_bar_follows(const seq_t *s, int lane, int bar);
void seq_set_anticipate(seq_t *s, int lane, int v);
int  seq_anticipate(const seq_t *s, int lane);
int  seq_anticipate_steps(const seq_t *s, int lane);   /* 0.24 final review: the steps an early hit moves (0, 1, 2, 4), as playback reads them */
int  seq_step_flags(const seq_t *s, int lane, int bar, int step);
/* RHYTHM's RESET KNOBS (ruling R3): SYNC, GHOSTS, HUMANIZE and FILL (0.24 CADENCE and BREATH) to 0, COMPLEXITY to 50, PUSH 0, GATE 50,
 * DYNAMICS 50; one undo step, none when nothing changes. seq_reset_chord_knobs leaves the RHYTHM knobs. */
void seq_reset_rhythm_knobs(seq_t *s, int lane);
/* 0.24 CADENCE (RK_CAD: OFF, OPEN, SOFT, HOME) and BREATH (RK_BREATH: None, 1/8, Beat, Half), 0..3, on a DRAWN lane's phrase's last bar
 * (bar 4, 8, 12, 16; a loop under 4 bars: its last), never a locked one: BREATH rests its last 0 / 2 / 4 / 8 steps (after GHOSTS, before
 * FILL, whose notes play in the gap); CADENCE takes the bar's last hit then (seq_core.c cad_step; on the Bass not a fixed note): the Bass
 * plays the chord's 5th / 3rd / root there in the octave nearest its note, the Chords voice that chord with that tone on top (the smallest
 * octave move; at play time, as INVERSION). 0.24 BASS FILL 1 PICKUP, 2 STEP-IN, 3 RUN walk into the next bar's first root (the half step
 * below on step 16; 3 chromatic notes on 14-16; -7 -5 -3 -1 semitones on 13-16), 4 BREAK rests steps 9-16; the Chords' FILL is 0.18's.
 * The order of every stage in a bar: seq_core.c THE ORDER IN A BAR.
 * 0.18 the RHYTHM knobs on the playing part (RK_*): SYNC clamped to -2..3, GHOSTS, HUMANIZE and FILL to 0..4. A turn renders the
 * Chords lane (every bar), adds no undo step (knobs are not undo steps) and leaves an open transaction open. A bad `what`: ignored,
 * reads 0. At play time on a DRAWN Chords lane: FILL plays each bar's fill variant on every 4th bar (bars 4, 8, 12, 16 of a loop
 * of 4 bars or more; every 4th song bar in 1- and 2-bar loops), never on a locked bar; HUMANIZE h (as seq_rknob_eff reads it)
 * delays each hit 0..h x 4 ms and moves its velocity +-h x 3, fixed per bar and step, never before the hit before it. */
void seq_set_rknob(seq_t *s, int lane, int what, int v);
int  seq_rknob(const seq_t *s, int lane, int what);
/* 0.19 the BASS page, the playing part's DRAWN Bass lane (GEN_BASS_DRAWN), bar 0 .. bars-1; anything else reads silent and 0.
 * seq_bass_line: the bar's notes as it plays, never its fill variant (0 silent), and roles (bassline.h: 1 o 3 5 p c P f, - held,
 * . rest); seq_bass_bar_score: its tension (bass_score, each note under its own chord); seq_bass_fixed_count: its fixed notes,
 * sounding or not. Getters: they read what the setters rendered. */
void seq_bass_line(const seq_t *s, int bar, unsigned char note[16], char role[16]);
int  seq_abs_note(const seq_step_t *x);                /* an absolute step's note as it sounds: abs when on, else 0 (silent) */
int  seq_lane_drawn(const seq_t *s, int lane);         /* 1 for the playing part's DRAWN Chords or Bass lane (GEN_CH_DRAWN, GEN_BASS_DRAWN) */
const char *seq_bass_roles(const seq_t *s, int bar);   /* seq_bass_line's 16 roles alone, in place (not 0-ended): a polled read */
int  seq_bass_bar_score(const seq_t *s, int bar);
int  seq_part_bass_bar_score(const seq_t *s, int part, int bar);   /* the same for any part (a bad part reads the playing one): the
                                                                    * engine's caches for the queued part */
int  seq_bass_fixed_count(const seq_t *s, int bar);
/* 0.24 (Task 7) the tension (bass_tension, 0..85) of MIDI note midi at step `step` of bar under the chord there (the change points'
 * too), for PHRASE's Bass dots; 0 for a bad bar, step or note */
int  seq_bass_tension_at(const seq_t *s, int bar, int step, int midi);
/* 0.20 the JAM Melody lane (GEN_MEL_JAM) of the playing part, bar 0 .. bars-1; anything else reads silent and 0. seq_mel_line: the
 * bar's line as it plays (melody model and fixes: note 0 silent, hold, tension); seq_mel_frame: a step's dot frame 0..72
 * (mjam_frame); seq_mel_score: the bar's tension (mjam_score); seq_mel_fixed_count: its fixed steps, sounding or not;
 * seq_mel_level: the playing block's COMPLEXITY band, min(4, cx_3 / 20). Getters: they read what the setters rendered.
 * PLAY's tap on the lane (seq_set_step): a silent step takes a note fix, the chord root nearest the bar's notes; a sounding one a
 * rest fix; one undo step. GEN, NEW IDEA and seq_generate leave it, with no undo step. It plays its notes as rendered (no
 * REGISTER, no OCTAVE), GATE the recipe's MK_GATE (mgat), PUSH the part's
 * mjam.push (as the Bass's); 0.21 its velocities ACCENT LIGHT's (mjam.acc, fixed), SWING the part's mjam.swing; mjam.dyn (jdyn) is kept but no
 * longer plays. */
void seq_mel_line(const seq_t *s, int bar, mjam_note_t out[16]);
int  seq_mel_frame(const seq_t *s, int bar, int step);
int  seq_mel_score(const seq_t *s, int bar);
int  seq_mel_fixed_count(const seq_t *s, int bar);
int  seq_mel_level(const seq_t *s);
void seq_set_mel_push(seq_t *s, int v);   /* the playing part's; 0..100, else ignored; no undo step */
void seq_set_mel_dyn(seq_t *s, int v);
int  seq_mel_push(const seq_t *s);
int  seq_mel_dyn(const seq_t *s);
/* 0.21 the playing part's MELODY SWING 0..5 and ACCENT 0..3 (else clamped), as PUSH: no undo step, kept by UNDO. ACCENT renders (the
 * velocities are baked in the steps); Amendment 1: no knob sets ACCENT (the engine never calls seq_set_mel_acc; tests only).
 * seq_mel_grid16: the loop's swing grid (1 16ths, 0 8ths); seq_mel_weak: the MW_* bits */
void seq_set_mel_swing(seq_t *s, int v);
int  seq_mel_swing(const seq_t *s);
void seq_set_mel_acc(seq_t *s, int v);
int  seq_mel_acc(const seq_t *s);
int  seq_mel_grid16(const seq_t *s);
unsigned seq_mel_weak(const seq_t *s);
/* 0.20 Task 6 (owner: old projects load as they are): an old project's Melody 1 (a recipe, a 0.10 style, MANUAL notes) loads and
 * plays as 0.19 played it. The first JAM action on it (seq_mel_queue, seq_mjam_set, seq_mel_swap, seq_set_mel_switch, seq_mpick_open,
 * seq_medit_open, each with good arguments; a value equal to the shown one, as a host echo, leaves it) first makes it a new instance's JAM lane (JAM level 1, mjam_default, GATE 50; its steps,
 * locks and NEW BAR variations dropped): one undo step, which puts the old lane back; an open transaction closes as DONE first.
 * 0.20 switching JAM blocks in time (the playing part's JAM lane). seq_mel_queue queues block c 0..5,
 * l 0..4: it takes over at the next SWITCH AT boundary of the loop (BEAT, BAR, 2 BARS, PHRASE: 4, 16, 32, 64 steps from the part's
 * bar 1, at most the loop's length), its notes from that point of the bar and phrase. The playing block cancels a queued one,
 * another replaces it; stopped, it switches at once (and COMPLEXITY, cx_3, follows: 20 x l + 10). A part switch, a stop with a part
 * queued, a state load and INIT drop the queue; a stop otherwise switches at once. No undo step.
 * seq_mel_queued: 1 when a block is queued on a JAM lane (c, l and at_bar, the 0-based loop bar of its boundary from the play position; NULL
 * pointers skipped). seq_set_mel_switch: the part's SWITCH AT 0..3, else ignored; no undo step; a queued block keeps waiting for
 * the new boundary. seq_mel_poll, on the screen side (the engine's set_param and song_pos): after seq_block switched a block, sets
 * cx_3 to it and returns 1 (labels to refresh), else 0. */
void seq_mel_queue(seq_t *s, int c, int l);
int  seq_mel_blocked(const seq_t *s);   /* 1 when playing with a part switch queued whose bar line is at or before a new block's boundary: seq_mel_queue refuses it */
int  seq_mel_queued(const seq_t *s, int *c, int *l, int *at_bar);
void seq_set_mel_switch(seq_t *s, int sw);
int  seq_mel_switch(const seq_t *s);
int  seq_mel_poll(seq_t *s);
/* 0.20 the MELODY tab's views (not saved). MVIEW_JAM and MVIEW_PHRASE are the user's (its two pages: seq_set_mview takes only these,
 * and closes an open Melody transaction as DONE first); MVIEW_PICK (the ANSWER BLOCK picker) and MVIEW_EDIT (the bar editor) are its
 * open transactions. seq_mmode: 0 JAM, 1 the rest (PHRASE, PICK, EDIT). The MELODY tab's strip pages by bars; FOLLOW follows the
 * BASS rules on it (seq_follow_paused(TAB_MEL): an open picker or editor pauses it). Every Melody transaction closes as the BASS
 * tab's do: a part switch, a state load, INIT and UNDO (with none of its own steps left) cancel it; a loop or chord-length change,
 * another transaction's opening, seq_set_mview and a PLAY tap on the JAM lane close it as DONE, but a loop shrink to or below the
 * editor's bar cancels it. */
enum { MVIEW_JAM, MVIEW_PHRASE, MVIEW_PICK, MVIEW_EDIT };
int  seq_mview(const seq_t *s);
void seq_set_mview(seq_t *s, int v);
int  seq_mmode(const seq_t *s);
/* 0.20 the playing part's JAM melody settings (melody2.h mjam_set_t), by field MJS_*: FORM 0..5 (MF_*), BREATH, CADENCE and FILL
 * 0..3, ANSWER 0..5 (MA_*), STRAY 0..3, OCTAVE (the answers') -1..1, REGISTER 0..2. A value outside its range is clamped; a bad field
 * is ignored and reads 0. A change renders every bar at once (a locked bar keeps its frozen settings) and a queued block; no undo step
 * (knob turns); an open transaction stays open. seq_mel_swap sets the SWAP block (c 0..5, l 0..4) the ANSWER SWAP plays, as a
 * setting; the call's own block (the playing one) is refused (if a jam makes them the same, the answers play as ECHO). A lane that
 * isn't JAM is made JAM first (above). */
enum { MJS_FORM, MJS_BREATH, MJS_CAD, MJS_FILL, MJS_ANS, MJS_STRAY, MJS_AOCT, MJS_REG, MJS_DENS, MJS_RNG, MJS_VAR, MJS_TIME, MJS_COUNT };   /* 0.21: DENSITY -2..2, RANGE -1..2, VARIATION 0..3, TIME -1..1 (0 neutral) */
int  seq_mjam_get(const seq_t *s, int field);
void seq_mjam_set(seq_t *s, int field, int v);
void seq_mel_swap(seq_t *s, int c, int l);
/* 0.20 the ANSWER BLOCK picker (MVIEW_PICK), a transaction on the JAM lane, as the STYLE browser but with no undo step (the SWAP
 * block is a setting): seq_mpick_try(c, l) sets the SWAP block at once (the call's own block does nothing); CANCEL (or UNDO) puts
 * the SWAP block back as at open; DONE keeps it. */
void seq_mpick_open(seq_t *s);
void seq_mpick_try(seq_t *s, int c, int l);
void seq_mpick_done(seq_t *s);
void seq_mpick_cancel(seq_t *s);
/* 0.20 the bar editor (MVIEW_EDIT), a transaction on bar 0..bars-1 of the JAM lane, as the Bass bar editor (seq_medit_bar: -1 none
 * open). It opens with the bar's first sounding step selected (else step 1) and the editor's octave (3..7) that note's (else 4).
 * seq_medit_select picks any step (a sounding one sets the octave to its note's) and returns 1 (0 for a bad step). seq_medit_note(pc
 * 0..11) fixes the selected step at MIDI (octave + 1) x 12 + pc (a rest step takes a note too) and plays it once (the audition queue,
 * Melody channel); seq_medit_set_oct sets the octave and, with a sounding step selected, re-fixes it there; seq_medit_rest and
 * seq_medit_hold fix it as a rest, or as holding the note before it (silent with none). NEW BAR gives the bar the next variation (up
 * to 8 tries) whose notes differ: 1, or 0 with nothing changed when none does. RESET BAR: variation 0, no fix, unlocked (nothing
 * when already). LOCK / UNLOCK toggles the bar's lock: a locked bar keeps the JAM settings it had (frozen: block, phrase settings,
 * register) and follows its chords. A fix, NEW BAR, RESET BAR and LOCK are one undo step each (none when nothing changes), its
 * own: UNDO steps them back one at a time (the editor stays open), CANCEL puts the bar's variation, fixes, lock and frozen settings
 * back as at open and drops them; DONE keeps. Knob turns meanwhile stay. */
void seq_medit_open(seq_t *s, int bar);
int  seq_medit_bar(const seq_t *s);
int  seq_medit_sel(const seq_t *s);
int  seq_medit_select(seq_t *s, int step);
int  seq_medit_oct(const seq_t *s);
void seq_medit_set_oct(seq_t *s, int o);
void seq_medit_note(seq_t *s, int pc);
void seq_medit_rest(seq_t *s);
void seq_medit_hold(seq_t *s);
int  seq_medit_new_bar(seq_t *s);
void seq_medit_reset_bar(seq_t *s);
void seq_medit_lock(seq_t *s);
void seq_medit_cancel(seq_t *s);
void seq_medit_done(seq_t *s);
/* 0.22 JAM on the playing part's Chords and Bass lanes (lane LANE_CHORDS or LANE_BASS, DRAWN; any other lane or a bad block is ignored and
 * reads 0). seq_jam_apply_now puts block c 0..JAM_COLS-1, l 0..JAM_LEVELS-1 in at once (the stopped-transport path), one undo step on the
 * lane (its bars, voicing / note settings and any auto note-locks; UNDO also puts back the block that played before): every bar of the
 * loop that isn't locked for JAM (its rhythm locked; on the Bass, or its notes) takes the block's rhythm as its JAM index (RJAM_BASE +
 * c x JAM_LEVELS + l, editable on RHYTHM); a Bass bar with only its rhythm locked is note-locked first at the settings it plays, so it
 * keeps everything; the lane's voicing (CHORDS: per part, so a rhythm-locked bar takes it too) or note settings become the block's,
 * reading Custom. The block as it plays (not edited) again does nothing (a host echo). seq_jam_playing: 1 when a block plays (c, l
 * may be NULL); seq_jam_edited: 1 when the lane differs from it (its voicing / note settings, or an unlocked bar's codes or index).
 * seq_djam_set: the lane's JAM knobs by DJ_*: VARIATION 0..3, SWING 0..5, TIME -1..1, SWITCH AT 0..3, clamped, no undo step; VARIATION
 * and TIME render the lane (TIME then VARIATION's rhythm on every bar not locked for JAM, before COMPLEXITY's hits; a varied Chords bar
 * plays INVERSION +1 / +2 / -1 at phrase position 1 / 2 / 3, a varied Bass bar its NEW BAR variation + the position). */
enum { DJ_VAR, DJ_SWING, DJ_TIME, DJ_SW, DJ_COUNT };
int  seq_djam_get(const seq_t *s, int lane, int field);
void seq_djam_set(seq_t *s, int lane, int field, int v);
/* 0.22 the lane's derived JAM state (any other lane reads 0). seq_djam_grid16: SWING's grid, 1 16ths when a bar not locked for JAM hits
 * an odd step in its codes after TIME and VARIATION (before COMPLEXITY's layers and FILL), else 0 8ths; a hit on it (an odd step, else
 * step mod 4 = 2) sounds (S - 0.5) x 2 x grid late (S = 50..70 %), the larger of it and PLAY's SWING, at most 1/8 beat before the bar's
 * next step sounding (8ths), never on a pushed step nor a bar locked for JAM (it plays straight); HUMANIZE's delay on top. seq_djam_weak: the DW_* bits of the knobs whose step
 * changes fewer than 1 in 8 of the loop's steps (VARIATION, TIME: the sounding notes or a note start against the knob at neutral) or
 * of its unlocked bars' hits (SWING: those on the swung positions), worked out in the setters that render the lane (never on the audio thread); a block
 * that takes over in time reads 0 until the next setter renders the lane */
enum { DW_VAR = 1, DW_SWING = 2, DW_TIME = 4 };
int  seq_djam_grid16(const seq_t *s, int lane);
unsigned seq_djam_weak(const seq_t *s, int lane);
void seq_jam_apply_now(seq_t *s, int lane, int c, int l);
int  seq_jam_playing(const seq_t *s, int lane, int *c, int *l);
int  seq_jam_edited(const seq_t *s, int lane);
/* 0.22 switching JAM blocks in time on the Chords and Bass lanes, as MELODY's (seq_mel_queue): seq_jam_queue queues block c, l on the lane
 * (1 when it is queued, else 0): it takes over at the lane's next SWITCH AT boundary from the part's bar 1 (BEAT, BAR, 2 BARS, PHRASE: 4, 16,
 * 32, 64 steps, at most the loop), as seq_jam_apply_now would apply it then (the lane's state then, and every knob turned meanwhile). Its
 * undo step is pushed at the tap; a tap that changes anything (a cancel too) closes an open transaction as DONE first. The playing block
 * (not edited, or with a block queued) cancels a queue and removes its undo step;
 * another block replaces it (one step); the block queued again does nothing (a host echo); stopped, seq_jam_apply_now at once; refused
 * (dropping a queued one, as MELODY) when its boundary is at or after a queued part switch's bar line. A part switch, a state load and
 * INIT drop the queues; a stop installs them at once; UNDO while one waits cancels the latest and does nothing else. Each lane queues on
 * its own (and MELODY's apart). seq_jam_queued: 1 when a block waits on the lane (c, l may be NULL). seq_jam_poll, on the screen side:
 * the drawn lanes (bit DL_*) whose block took over since the last poll, then clears them. */
int  seq_jam_queue(seq_t *s, int lane, int c, int l);
int  seq_jam_queued(const seq_t *s, int lane, int *c, int *l);
int  seq_jam_poll(seq_t *s);
int  seq_jam_at_bar(const seq_t *s, int lane);   /* 0.22 the 0-based loop bar a queued block takes over at (seq_mel_queued's at_bar), -1 none */
/* 0.19 the STYLE browser's tile pictures: bar's roles as it would play under each of the n styles from `style` (role[j]: style + j,
 * 0..BASS_STYLE_COUNT-1) instead of the part's settings (a note-locked bar keeps its own): its rhythm as it plays, its NEW BAR
 * variation and fixed notes. Pure, no state change; a bad bar or style, or a lane that isn't DRAWN: all '.' */
void seq_bass_style_roles(const seq_t *s, int style, int n, int bar, char role[][16]);
/* 0.19 the BASS tab's views (not saved). BVIEW_BASS and BVIEW_RHYTHM are the user's (its two pages: seq_set_bview takes only these,
 * and closes an open Bass transaction as DONE first); the others are its open transactions: the bar editor (EDIT; 0.24 retired, never
 * read: the bar editor is PHRASE's open bar, BAR), the STYLE browser
 * (STYLE), and on RHYTHM an open bar (BAR), its COPY TO... (COPY) and the RHYTHMS browser (RBROWSE, in a bar or not). seq_bmode: 0
 * BASS for BASS, EDIT and STYLE, 1 RHYTHM for the rest; 0.22 BVIEW_JAM (the user's, a page of its own) 2. The BASS tab's strip pages by bars on both pages (seq_strip_by_bars 1);
 * FOLLOW follows the CHORDS rules on it (seq_follow_paused(TAB_BASS): an open Bass transaction pauses it, paging until BACK TO
 * PLAYING). Every Bass transaction closes as the CHORDS ones do: a part switch, a state load, INIT and UNDO (with none of its own
 * steps left) cancel it; a loop or chord-length change, another transaction's opening and seq_set_bview close it as DONE, but a
 * loop shrink to or below the editor's bar cancels it. */
enum { BVIEW_BASS, BVIEW_EDIT, BVIEW_STYLE, BVIEW_RHYTHM, BVIEW_BAR, BVIEW_COPY, BVIEW_RBROWSE, BVIEW_JAM };   /* 0.22 JAM, the user's too */
int  seq_bview(const seq_t *s);
void seq_set_bview(seq_t *s, int v);
int  seq_bmode(const seq_t *s);
/* 0.19 the BASS page's note settings on the playing part (bassline.h bass_set_t), rendered into the Bass lane's notes; a note-locked
 * bar keeps its own (seq_rbar_lock). seq_set_bknob: knobs 2-5 (BK_MOV, BK_OCTS, BK_APP 0..4, BK_REG -2..2, clamped; a bad knob is
 * ignored and reads 0): no undo step, an open transaction stays open. PEDAL (0 off, 1 TONIC, 2 DOMINANT), OCTAVE (-1..1) and a STYLE
 * (0..BASS_STYLE_COUNT-1: its PEDAL, OCTAVE and knobs 2-5; with the STYLE browser open a try) are one undo step each (PEDAL's and
 * OCTAVE's restores only itself, a STYLE's the settings), none when nothing changes; bad values are ignored; an open transaction closes as DONE first. seq_bass_edited: 1 when
 * PEDAL, OCTAVE or a knob differs from the style's (0 for Custom, -1). RESET KNOBS (BASS page): knobs 2-5, PEDAL and OCTAVE to the
 * style's (Custom: 0), COMPLEXITY 50, PUSH 0, GATE 50, DYNAMICS (ACCENT) 50, as one undo step, none when nothing changes (the RHYTHM
 * page's is seq_reset_rhythm_knobs(LANE_BASS)). */
int  seq_bknob(const seq_t *s, int k);
void seq_set_bknob(seq_t *s, int k, int v);
void seq_set_bass_pedal(seq_t *s, int p);
void seq_set_bass_octave(seq_t *s, int o);
void seq_set_bass_style(seq_t *s, int i);
int  seq_bass_style(const seq_t *s);
int  seq_bass_edited(const seq_t *s);
void seq_reset_bass_knobs(seq_t *s);
/* 0.19 the STYLE browser (BVIEW_STYLE), a transaction on a DRAWN Bass lane, as the VOICING browser: its open pushes the one undo step
 * DONE keeps (DONE with nothing changed and CANCEL remove it); a try applies style i (0..BASS_STYLE_COUNT-1) with no undo step; CANCEL
 * (or UNDO) puts the note settings back as at open, but for a knob turned since the last try, which keeps its value (and the UNDO of
 * the step DONE kept keeps it too). The genre chip (0..BASS_GENRES-1) is the style's genre when it opens; a chip
 * sets it and, open, tries the genre's first style. */
void seq_bstyle_open(seq_t *s);
void seq_bstyle_try(seq_t *s, int i);
int  seq_bstyle_genre(const seq_t *s);
void seq_bstyle_set_genre(seq_t *s, int g);
void seq_bstyle_done(seq_t *s);
void seq_bstyle_cancel(seq_t *s);
/* 0.18 RHYTHM's bar editing, on the playing part's DRAWN Chords lane, as one transaction (seq_txn_t, kind CVIEW_BAR). It follows the
 * picker's rules: a part switch, a state load, INIT and opening a picker or browser cancel it; a view change (seq_set_cview, ruling
 * R32) and a loop or chord-length change close it as DONE first (its COPY TO... or RHYTHMS browser with it), except that a loop
 * shrink to or below the open bar cancels it. FOLLOW pauses while it is open (1),
 * the strip holding the open bar's page (the page of the bar's first slot); a knob turn never closes it, pushes no undo step and is
 * kept by CANCEL.
 * seq_rbar_open(bar) (0 .. bars-1, else nothing) closes any open transaction as DONE first; seq_rbar_bar: -1 none open. A paint
 * (code RC_*: the paints HIT, ACCENT, GHOST, HOLD, REST; 0.24: PHRASE's rule below, the code a step has no longer clears it): HOLD
 * needs a hit or a hold on the step before, else nothing; a step that becomes a rest clears the holds after it. A change is one undo
 * step and marks the bar edited (ed); 1 when it changed something. seq_rbar_lock toggles the open bar's lock (one undo step). A copy gives every bar of the
 * loop in mask (bit b: bar b) that is not the source and not locked the source's codes, `from` and `ed` (not its lock): one undo
 * step, none when nothing changes. seq_rbar_copy_counts returns how many of mask's bars a copy replaces (the unlocked ones) and
 * gives the ones of them whose codes differ (with seq_rcopy_what 1 also those whose change points or hit octaves, or fixed notes, differ)
 * and the locked ones skipped (0 with no bar open).
 * UNDO with the bar open steps back its own paints, copies, locks and picks one at a time (it stays open); with none left it is the
 * bar's CANCEL. CANCEL puts every drawn bar and lock back as at open and drops its own undo steps; DONE keeps them.
 * COPY TO... (seq_rcopy_*, seq_cview CVIEW_COPY) is a picker inside the open bar: toggles take bars of the loop other than the source
 * and the locked ones; seq_rcopy_go copies to the picked bars and closes it (an empty pick: nothing); close (or UNDO) closes it.
 * seq_rhythm_pick(ri) (0..RHYTHM_COUNT-1): library rhythm ri (ed 0, from ri) in the open bar, else in every unlocked bar of the
 * loop (any other transaction closed as DONE first); a locked bar keeps its own; one undo step, none when nothing changes; with
 * the RHYTHMS browser open it is a try. 1 when it changed a bar. The RHYTHMS browser (seq_rbrowse_open; seq_cview CVIEW_RBROWSE) fills the same bars: in
 * the open bar, or as its own transaction when none is open (a picker or browser open is cancelled). Its open pushes one undo step
 * (DONE keeps it, CANCEL and a DONE that changed nothing remove it); seq_rbrowse_try(ri) fills without one. seq_browse_done and
 * seq_browse_cancel close it (CANCEL: the bars as when it opened; UNDO is its CANCEL). The feel chip (seq_rbrowse_group, 0..
 * RHYTHM_GROUPS-1) is the shown bar's (the open bar, else bar 1) rhythm's when it opens; seq_rbrowse_set_group sets it and, open,
 * tries the group's first rhythm. */
void seq_rbar_open(seq_t *s, int lane, int bar);
int  seq_rbar_bar(const seq_t *s, int lane);
int  seq_rbar_paint(seq_t *s, int lane, int step, int code);
/* 0.24 PHRASE's bar editor is the open bar (CVIEW_BAR) of lane LANE_CHORDS or LANE_BASS. It opens with the bar's first hit (drawn)
 * selected, else step 0. seq_rbar_sel: the selected step (-1 none open); seq_rbar_select selects any step (on the Bass, a sounding
 * step's note sets the octave seq_rbar_oct reads, 1..4).
 * Paint (changed from 0.18): a paint selects the step; the code a step already has only selects it (no change, no undo step); REST
 * clears; HOLD needs a hit or hold before it, else nothing (not selected). A step that stops being a hit loses its hit octave; its
 * change point and its fixed Bass note stay. 1 when it changed something.
 * CHORDS picks on the selected step (any step: a hit, a hold or a rest). From the start of a slot that starts in the bar up to and
 * including its first hit (no change point between): the pick is the slot's (it sets the slot,
 * marks it picked: slot_lock). A later step (a 2-bar slot's second bar: any step) has a change point: the chord picked starts there,
 * a new point of type 0 Knobs (ruling 12; a point given another chord keeps its type); picking the chord that sounds there without
 * the step's own point removes the point; the point's own chord again, or the sounding chord on a step with none, changes nothing. seq_rbar_type (0 Knobs .. 4 6th) sets the
 * step's own point's type, or the slot's, else makes a point of the sounding chord at that type. A full bar (4 points) takes no new
 * point. Each change is one undo step and plays the chord at the step once.
 * seq_rbar_hit_oct(-2..2): the selected hit's octave (a hit only; one undo step, none when it is already). seq_hit_oct reads one.
 * seq_rbar_this_hit_only: on a hit that starts a chord change, a point at the bar's next hit back to the chord before it (one undo
 * step): 1, else the reason (RHO_*, all <= 0) and nothing: not on a hit that starts a change, no later hit in the bar before the
 * next slot's start (0.24 final review: the change ends there), the next hit (or a step before it) starts a change of its own, or
 * the bar already has 4 points.
 * BASS picks on the selected step when it sounds (the 0.19 editor's rules): seq_rbar_note(pc 0..11) fixes it at MIDI (oct + 1) x 12
 * + pc; seq_rbar_note_oct(1..4) sets the octave and re-fixes a sounding step there; seq_rbar_follow_chord unfixes it. Each plays the
 * note once (Bass channel); a change is one undo step.
 * seq_rbar_new_bar: BASS the next NEW BAR variation whose notes differ (0.19); CHORDS the bar's codes through rhythm_newbar at the
 * next version v (Chords lane bar_var) whose codes differ, painted in (ed); up to 8 tries: 1, or 0 with nothing changed.
 * seq_rbar_reset_bar: the bar's library rhythm again (from: ed 0; a bar drawn from nothing keeps its codes), v 0, unlocked; CHORDS
 * its change points and hit octaves cleared and the real slots that start in it unpicked (they keep their chords); BASS its fixed
 * notes and note lock cleared. One undo step, none when there is nothing to reset.
 * seq_rbar_lock: LOCK marks the real slots that start in the bar picked (a Knobs one keeps its COLOUR level, as seq_toggle_slot_lock)
 * and locks the rhythm; on the Bass also the notes (the settings it plays now frozen). UNLOCK clears the rhythm and note locks only.
 * seq_bar_picked: 1 when a real slot starting in bar is picked and the bar is not locked on `lane` (0.24 Task 7: locked as for JAM, its
 * rhythm lock or on the Bass its note lock: a locked bar reads LOCKED, not picked; any other lane reads 0).
 * COPY: seq_rcopy_set_what (0 RHYTHM ONLY, the default at every open; 1 RHYTHM + CHORDS on the Chords, RHYTHM + NOTES on the Bass) is
 * read by seq_rbar_copy and seq_rcopy_go: with 1 a copy also writes the source bar's change points and hit octaves (Chords) or fixed
 * notes (Bass); a target's own slots never change. A bar locked for copies is one locked for JAM: its rhythm lock, or on the Bass its
 * note lock alone (never replaced, counted as skipped). seq_rcopy_same_place: the bars a copy may replace (unlocked, not the open bar)
 * at the open bar's place in each 4-bar phrase.
 * CANCEL puts all of it back as at open: the drawn bars, the progression and slot picks, change points, hit octaves, NEW BAR versions,
 * fixed notes, note locks and frozen settings. UNDO steps back one edit at a time. */
enum { RHO_NOT_A_CHANGE = 0, RHO_NO_LATER_HIT = -1, RHO_LATER_HAS_OWN = -2, RHO_FULL = -3 };
/* 0.24 (Task 7) what seq_rbar_chord and seq_rbar_type did, for PHRASE's message line: RBC_NONE nothing (no Chords bar open with a step, a bad
 * chord or type), RBC_HEARD only heard (the chord or type it plays already), RBC_SLOT the real slot's pick, RBC_POINT a change point set,
 * RBC_GONE the step's point removed (its chord sounds there anyway), RBC_FULL nothing: the bar has 4 points (RHO_FULL's value) */
enum { RBC_FULL = RHO_FULL, RBC_NONE = 0, RBC_HEARD, RBC_SLOT, RBC_POINT, RBC_GONE };
int  seq_rbar_sel(const seq_t *s, int lane);
void seq_rbar_select(seq_t *s, int lane, int step);
int  seq_rbar_chord(seq_t *s, int deg, int src);
int  seq_rbar_type(seq_t *s, int type);
void seq_rbar_hit_oct(seq_t *s, int o);
int  seq_hit_oct(const seq_t *s, int bar, int step);
int  seq_rbar_this_hit_only(seq_t *s);
void seq_rbar_note(seq_t *s, int pc);
void seq_rbar_note_oct(seq_t *s, int oct);
int  seq_rbar_oct(const seq_t *s);
void seq_rbar_follow_chord(seq_t *s);
int  seq_rbar_new_bar(seq_t *s, int lane);
void seq_rbar_reset_bar(seq_t *s, int lane);
int  seq_bar_picked(const seq_t *s, int lane, int bar);
void seq_rcopy_set_what(seq_t *s, int lane, int what);
int  seq_rcopy_what(const seq_t *s, int lane);
unsigned seq_rcopy_same_place(const seq_t *s, int lane);
void seq_rbar_lock(seq_t *s, int lane);
void seq_rbar_copy(seq_t *s, int lane, unsigned mask);
int  seq_rbar_copy_counts(const seq_t *s, int lane, unsigned mask, int *replaced_differing, int *skipped_locked);
void seq_rbar_cancel(seq_t *s, int lane);
void seq_rbar_done(seq_t *s, int lane);
void seq_rcopy_open(seq_t *s, int lane);
void seq_rcopy_toggle(seq_t *s, int lane, int bar);
void seq_rcopy_set_all(seq_t *s, int lane, int on);
unsigned seq_rcopy_mask(const seq_t *s, int lane);
void seq_rcopy_go(seq_t *s, int lane);
void seq_rcopy_close(seq_t *s, int lane);
int  seq_rhythm_pick(seq_t *s, int lane, int ri);
void seq_rbrowse_open(seq_t *s, int lane);
void seq_rbrowse_try(seq_t *s, int lane, int ri);
int  seq_rbrowse_group(const seq_t *s, int lane);
void seq_rbrowse_set_group(seq_t *s, int lane, int g);
/* 0.17 voicing styles (voicing.h): applying style i (0..VOICING_NSTYLES - 1; else ignored) sets the playing part's connection mode,
 * COLOUR, INVERSION, SPREAD, STRUM (and its direction) and the Chords lane OCTAVE, and vstyle; one undo step, none when it changes
 * nothing. seq_voicing_style: the part's style (-1 Custom). seq_voicing_edited: 1 when any of those eight differ from the style's
 * (turning a knob never changes the style), 0 for Custom. New instances start at VOICING_DEFAULT; a state without `.vx` loads Custom. */
void seq_set_voicing_style(seq_t *s, int i);
int  seq_voicing_style(const seq_t *s);
int  seq_voicing_edited(const seq_t *s);
int  seq_part_voicing_edited(const seq_t *s, int part);      /* the same for any part (a bad part reads the playing one) */
/* 0.24 the CHORDS page's OCTAVE: the Chords lane's octave (SHAPE_OCT, -1..1; else ignored) on the playing part, one undo step (the voicing's
 * snapshot, none when it is the octave already; an open transaction is kept first, as DONE). Off the style's it reads edited
 * (seq_voicing_edited); RESET KNOBS puts the style's back */
void seq_set_chord_octave(seq_t *s, int o);
/* 0.24 the notes slot's chord plays in bar `bar` (a change point's virtual slot too), MIDI low to high, at the Chords lane's step octave and
 * the part's OCTAVE (before SWING, STRUM or CADENCE): the CHORDS sound page's centre. 0 for a slot the loop never reaches. Allocation-free */
int  seq_slot_notes(const seq_t *s, int slot, int bar, int out[THEORY_CHORD_MAX]);
/* 0.17 the PROGRESSIONS / VOICING browsers, on the playing part. One is open at a time (opening one cancels
 * any open); a part switch, a state load, INIT and UNDO cancel it (seq_block only flags a switch; the next setter cancels).
 * A length, chord-length, GEN, NEW PROGRESSION or preset change closes it first as DONE (seq_txn_done: DONE for whichever is open, a
 * stale one cancelled). CANCEL puts back what was there at open and drops its own undo steps (the ring as it was, when nothing else
 * pushed since; another edit's steps stay). seq_cview: CVIEW_PROG / VOICE while open, else the user's
 * view (seq_set_cview takes CHORDS, RHYTHM or 0.22 JAM only, and cancels any open one but an open bar, which it closes as DONE;
 * not saved). 0.24 FIX is retired (the three-page model: JAM, CHORDS, PHRASE, which is the RHYTHM view); 0.24.1 its chord picker
 * left the code. A Knobs chord that gets locked (seq_set_slot, seq_toggle_slot_lock) keeps the COLOUR knob's level as its own
 * colour (theory_colour_of_level); unlocking leaves it.
 * Browsers: open pushes ONE undo snapshot (DONE keeps it, so one UNDO goes back to before; CANCEL removes it; DONE with nothing
 * changed removes it too). seq_prog_try(preset) fills the unlocked slots from the preset's phrase (as seq_set_prog_preset, locks stay),
 * seq_voice_try(style) applies the style; neither pushes an undo step, both need their browser open. seq_prog_preset_shown: the preset
 * whose phrase the unlocked slots match, else -1. seq_browse_genre: the VOICING browser's chip, the style's genre when it opens. */
int  seq_cview(const seq_t *s);
void seq_set_cview(seq_t *s, int v);
/* 0.17 KEYS (Chords lane only): the keys the CHORDS page's keyboard lights, MIDI 24..83 (else 0). 0 off, 1 lit, 2 lit folded (a
 * note outside the window shown at its nearest octave inside; a note that sits on its own key there wins: 1). seq_key_edge: 1
 * when a folded note came from that side (0 low, 1 high). Latched: every note of the last Chords-lane chord fired, strummed ones
 * too (all at the step), not cleared by note-offs; cleared by a stop, muting the Chords lane, a load and INIT. Allocation-free. */
int  seq_key_lit(const seq_t *s, int midi);
int  seq_key_edge(const seq_t *s, int side);
seq_keys_t seq_keys_of(const int *notes, int n);      /* the keys n MIDI notes light, folded as above (the BASS tab's KEYS too) */
void seq_txn_done(seq_t *s);
void seq_browse_open(seq_t *s, int kind);   /* CVIEW_PROG or CVIEW_VOICE */
void seq_browse_done(seq_t *s);
void seq_browse_cancel(seq_t *s);
void seq_prog_try(seq_t *s, int preset);
int  seq_prog_preset_shown(const seq_t *s);
int  seq_part_prog_preset_shown(const seq_t *s, int part);   /* the same for any part (a bad part reads the playing one): the
                                                             * engine's caches for the queued part */
void seq_voice_try(seq_t *s, int style);
int  seq_browse_genre(const seq_t *s);
/* 0.17 the CHORDS strip: 4 chords a page (seq_strip_pages = ceil(slots / 4), 1 at least; seq_strip_page 0-based). While FOLLOW runs the
 * page and the centre chord (seq_centre_slot) are the playing slot's (slot 0 when stopped: seq_cur_bar's stopped value is bar 0).
 * seq_follow_paused (0 with FOLLOW off): 0 running, 1 a picker or browser is open (the page and centre it opened on hold; the picker's
 * own slot is the centre), 2 paged by the arrows (seq_strip_turn, which wraps; ignored with one page, or while a picker or browser is open
 * with FOLLOW on). A picker or browser closing (done, cancel or automatic) goes back to running unless it was 2: the same page then.
 * seq_back_to_playing ends 2. FOLLOW off: the arrows (seq_strip_turn) page by hand, no pause, the centre is the slot last given to seq_set_centre
 * (a strip tap; out of range ignored; with FOLLOW on and running the UI opens the picker instead); turning FOLLOW off keeps the
 * page and centre on show. The page and centre clamp to the last page and slot after any length or chord-length change, a load and INIT.
 * 0.18 (ruling R22): on the RHYTHM views (seq_strip_by_bars 1: the user's RHYTHM view, an open bar, its COPY TO... and the RHYTHMS
 * browser) a page is 4 bars (seq_strip_pages = ceil(bars / 4); running, the playing bar's), else 4 chord slots; a view change
 * keeps the page's first bar.
 * 0.19: each tab (TAB_CHORDS, TAB_BASS) has its own strip (seq_t.tv); a transaction pauses and holds only its own lane's tab. The
 * BASS tab's strip pages by bars on both its pages (seq_strip_by_bars 1), and its centre (seq_centre_slot, seq_set_centre) is a bar,
 * 0..bars-1 (the playing bar while FOLLOW runs), not a chord slot. */
int  seq_strip_page(const seq_t *s, int tab);
int  seq_strip_pages(const seq_t *s, int tab);
int  seq_strip_by_bars(const seq_t *s, int tab);
void seq_strip_turn(seq_t *s, int tab, int delta);
void seq_back_to_playing(seq_t *s, int tab);
int  seq_follow_paused(const seq_t *s, int tab);
int  seq_centre_slot(const seq_t *s, int tab);
void seq_set_centre(seq_t *s, int tab, int slot);
/* 0.24 shows bar (0 .. bars-1, else nothing) on tab's strip as its centre, without opening it: the page holding it (in the tab's units as it
 * shows now) and the centre (CHORDS: the slot playing at the bar's start; BASS, MELODY: the bar). FOLLOW on: a paging pause (seq_follow_paused
 * 2, BACK TO PLAYING ends it), as seq_strip_turn; FOLLOW off: by hand. Nothing while a transaction is shown on the tab with FOLLOW on */
void seq_strip_show_bar(seq_t *s, int tab, int bar);
/* 0.24 (ruling 18) the tab's centre as a bar: BASS and MELODY seq_centre_slot (a bar there); CHORDS the bar the centre slot names, kept as a
 * bar (the open picker's slot's first bar; while FOLLOW runs the playing bar; paused or FOLLOW off the bar last given: seq_strip_show_bar,
 * seq_set_centre_bar, or seq_set_centre's slot's first bar), so a 2-bar chord's second bar can be PHRASE's centre. seq_set_centre_bar(bar)
 * (0 .. bars-1, else nothing) sets it, and on CHORDS the centre slot to the slot playing at the bar's start */
int  seq_centre_bar(const seq_t *s, int tab);
void seq_set_centre_bar(seq_t *s, int tab, int bar);
void seq_set_browse_genre(seq_t *s, int g);   /* 0..12; else ignored */
int  seq_chord_shape(const seq_t *s, int what);       /* 0 for a bad `what` */
/* 0.10: a lane's SHAPE on the playing part (seq_set_chord_shape is lane 0's): Chords SHAPE_*, Bass BSHAPE_* (clamped to
 * 0..100, BSHAPE_OCT -1..1, BSHAPE_APPROACH 0..1); Melody has none yet. No undo, like the knobs. A bad lane or `what`:
 * ignored, reads 0. The Bass's PUSH, GATE, ACCENT and OCTAVE act at playback as the Chords lane's PUSH, GATE, DYNAMICS
 * and OCTAVE do. REGISTER (50 neutral) folds each Bass note, at its step's own octave, by octaves: below 50 down to at
 * most a ceiling 60 - (50 - v) x 24 / 50 (36 at 0), above 50 up to at least a floor 28 + (v - 50) x 24 / 50 (52 at 100);
 * the OCTAVE knob's 12 x oct is added after, so it always moves the note an octave. */
void seq_set_lane_shape(seq_t *s, int lane, int what, int v);
int  seq_lane_shape(const seq_t *s, int lane, int what);
/* 0.10 Bass transforms, run on the generated steps of a generated Bass lane (never on locked steps), each a fixed rank per
 * step (a hash of the lane's seed and the step) under its value, so a step changed at v is changed the same at every
 * higher v; all 0: the generator's steps. In this order:
 *   MOVEMENT: an onset off a chord change (a slot's first step) becomes tone 2, 3, 4 or 5 of its chord, the nearest to
 *             the next onset's pitch (wrapping to the loop's first), at its own octave.
 *   SYNC:     an onset on an off-beat 8th (ranks 0..49), then on beat 2 or 4 (50..99), off a chord change, moves one 16th
 *             later (a step shorter) when that step is off and in the same slot.
 *   OCTAVES:  an onset off a chord change goes up an octave, or down one when above the lane's octave (-1).
 *   APPROACH 1 (Chromatic), Walk only, last: a slot's last onset that is not a chord tone (R, 3, 5; 7 on a 7th chord)
 *             becomes an absolute step, the semitone below or above the next slot's root, whichever is nearer the onset
 *             before it (ties: below), at the octave nearest that onset. Being generated, REGISTER folds it as it
 *             folds the onsets around it.
 * Their setters regenerate the lane, no undo. */

/* 0.10 FIX A NOTE, on the playing part, lanes Bass and Melody 1 (else ignored). note 1..127: the step on with that
 * absolute pitch, the generator's velocity and length there (or 100 and 1); note 0: a rest (the step off). A generated
 * lane locks the step; a MANUAL one is set directly. One undo step; the same state again pushes nothing. An absolute step
 * plays its note (a pick is the user's: REGISTER does not fold it; OCTAVE moves it), keeps it through key and scale changes, and is saved as
 * `<P><n>.ab=` <step hex2><note hex2> pairs, its step record the root at the nearest octave (what 0.9 plays). 0.19: a DRAWN
 * Bass lane is ignored (its fixed notes are seq_part_t.bass.fix). */
void seq_set_step_note(seq_t *s, int lane, int bar, int step, int note);
/* 0.16 notes for every bar, on a generated Bass or Melody 1 lane of the playing part (else ignored):
 *   new_bar: bar 0..bars-1 gets its own variation (another each tap), so only that bar regenerates in the lane's style;
 *            its fixed (locked) steps stay. One undo step.
 *   reset_bar: the bar back to the phrase's (variation 0) with its fixed steps unlocked; one undo step, none when it is.
 * A try on the lane goes BACK first (the edit is on YOURS). Saved as `<P><n>.bv=` (out_bar_var) only when a bar has one. */
void seq_lane_new_bar(seq_t *s, int lane, int bar);
void seq_lane_reset_bar(seq_t *s, int lane, int bar);
int seq_lane_bar_var(const seq_t *s, int lane, int bar);     /* 0: the phrase's bar */
int seq_lane_bar_locks(const seq_t *s, int lane, int bar);   /* the bar's fixed (locked) steps */
/* A generated lane's step (any active lane): locked, it unlocks and regenerates (an absolute pitch goes); unlocked, it
 * locks as it is. One undo step. A MANUAL lane: nothing to lock, ignored. */
void seq_toggle_step_lock(seq_t *s, int lane, int bar, int step);
int  seq_slot_label(const seq_t *s, int slot, char *buf, int len);     /* the chord played, coloured: "Em7"; " " past prog_len */
int  seq_slot_numeral(const seq_t *s, int slot, char *buf, int len);   /* "bII"; " " past prog_len */
/* 0.12, for the engine's FIX caches (made in set_param, for the playing and the queued part): the colour a part's slot plays
 * (its own, or on a Triad slot the COLOUR knob's: 0.17 vcol; COL_TRIAD for a bad part or slot), and how many of the
 * part's prog_len slots the COLOUR knob colours (own-coloured slots not counted), *kind the knob's colour then (COL_TRIAD at
 * 0, COL_7TH below 67, COL_ADD9 from 67). A bad part reads the playing one. */
int  seq_part_eff_colour(const seq_t *s, int part, int slot);
/* 0.24 final review: the slots part `part`'s bar plays in time order, one per chord change (its step 0, then each step seq_chord_change_at
 * names: a slot boundary whose chord differs, a change point), into slots[] (and their steps into at[] when not NULL); returns the count,
 * 0 for a bad part or a bar past the loop. Setter / label side (scans 16 steps) */
int  seq_part_bar_changes(const seq_t *s, int part, int bar, int slots[SEQ_STEPS], int at[SEQ_STEPS]);
/* 0.13 knob texts: GATE's factor x 100 (20 at 0, 100 at 50, 200 at 100); how many unlocked steps of a part's generated Bass
 * MOVEMENT, SYNC and OCTAVES change (out[0..2]; none on a MANUAL Bass; setters only, it generates); a part's Melody knob
 * (seq_mel_knob's) and its shown type (seq_mel_type's). A bad part reads the playing one. */
int  seq_gate_pct(int g);
void seq_part_bass_shape_counts(const seq_t *s, int part, int out[3]);
int  seq_part_mel_knob(const seq_t *s, int part, int knob);
int  seq_part_mel_type(const seq_t *s, int part);

/* 0.12 move feedback: NEW RHYTHM (seq_generate on the Chords lane) describes what it did, at most 47 characters
 * ("NEW RHYTHM: Ballad"; 0.12's chord, Bass and Melody moves left with IDEAS). seq_last_move_text copies it ("" when
 * none). It clears on any other setter that changes something (a re-send that changes nothing keeps it) and after 4
 * beats of playback (counted in seq_block). Not saved. */
int  seq_last_move_text(const seq_t *s, char *buf, int len);
int  seq_last_move_lane(const seq_t *s);   /* the lane whose move set the text, -1 when there is none */
void seq_set_names(seq_t *s, int on);   /* chord text in degrees (0, the default) or names (1); not saved */
int  seq_names(const seq_t *s);

/* 0.12 audition: notes queued here go out at the start of the next seq_block, playing or stopped, mutes ignored, on the
 * lane's channel at that block, velocity 100 (no DYNAMICS), 1 beat long (4 steps less the step tail; no GATE). Their
 * note-offs go through the pending queue, timed by the song position while playing and by the stopped blocks' own clock
 * while stopped; a same-note retrigger ends the held note first. The queue holds SEQ_MAX_AUDITION notes: a note that does
 * not fit is dropped (a chord all or nothing). ALL NOTES OFF, a stop and a load empty the queue and end the held notes; a loop
 * wrap or a part switch ends a held audition early, as every held note; a channel change ends it on the old channel. */
void seq_audition_chord(seq_t *s, int slot);            /* the playing part's slot 0..prog_len-1 as the Chords lane plays it
                                                         * (voicing, SPREAD, OCTAVE) */
void seq_audition_note(seq_t *s, int lane, int note);   /* an active lane, note 1..127 as given (0, a REST: nothing) */

/* 0.11 the melody RECIPE style (GEN_MEL_RECIPE, melody.h) on Melody 1 of the playing part. The lane generates with
 * mel_generate_v1 from the part's recipe, its seed and complexity (knob 1, cx_3), over each span's real chord (borrow
 * and effective colour); its steps are absolute, locked steps are kept as on every lane, and key, scale, progression and
 * COLOUR changes regenerate it. At playback its note length is len x GATE's curve (MK_GATE, as the output stage's GATE:
 * x0.2 at 0, x1 at 50, x2 at 100). A classic lane (Hook or Arp, 0.6-0.10) plays as before.
 * Saved as `<P>3.g=<GEN_MEL_HOOK>,<seed>,<complexity>` (what 0.10 then plays) and
 * `<P>3.mr=type,o1,o2,o3,phrase,range,leaps,sync,tension,answer,register,gate,rev,inv,shift` (phrase: the old one nearest
 * the phrasing, mel_legacy_phrase) and, VST-12, `<P>3.mp=form,breath,cadence,fill`; a valid `.mr` on a Hook
 * `.g` loads as the recipe, a bad one is ignored (the Hook stays). A classic lane saves as 0.10 did.
 *   set_type: the type's defaults (options, phrasing, knobs, complexity), the lane made GEN_MEL_RECIPE (a
 *             classic or Manual lane converted); one undo step, none when nothing changes. 0..MEL_NTYPES-1, else ignored.
 *   set_opt, set_phrase, set_flags, set_knob: on a classic lane (and, but for set_knob, a Manual one) first converted
 *             as set_type with the type shown (the classic style's: Hook or Arp; Manual: Hook), then applied. A
 *             conversion is always one undo step (a recipe snapshot: its undo is the classic or Manual lane exactly),
 *             set_flags with undo 0 and a knob turn included. On a recipe lane set_opt, set_phrase and set_flags (when
 *             `undo`) are one undo step, a knob turn none. Out of range, a re-send, or a knob on a Manual lane: ignored.
 *             rev, inv 0/1, shift 0..15; knobs MK_*, 0..100 (a GATE turn alone does not regenerate: it acts at playback).
 *   reset_knobs: the knobs and complexity to the type's defaults; one undo step, none when already there. On a classic
 *             or Manual lane it converts (plan decision 7), as a type tap with the type shown.
 * An ignored or re-sent tap changes nothing, a try included: the checks run before a BACK and again after it.
 * An undo restores the recipe; its knobs and complexity only for set_type, reset_knobs or a conversion (knob turns are
 * not undoable, as seq_set_complexity's).
 * The getters show the lane as the RECIPE tab does: type -1 for Manual, MEL_HOOK / MEL_ARP for a classic lane (its
 * options and knobs that type's defaults), else the recipe's. 0.20: on a JAM lane (GEN_MEL_JAM) every setter here is a no-op but
 * seq_mel_set_knob(MK_GATE): the JAM lane's GATE (mgat), as its PUSH and DYNAMICS (no undo step), which seq_mel_knob reads. */
void seq_mel_set_type(seq_t *s, int type);
void seq_mel_set_opt(seq_t *s, int which, int i);   /* which 0..2, i 0..mel_nopts(type, which) - 1 */
void seq_mel_set_phrase(seq_t *s, int phrase);      /* MEL_PHRASE_*: the old phrase's phrasing (mel_phrase_from_legacy) */
void seq_mel_set_phrasing(seq_t *s, int which, int v);   /* VST-12: MP_*, 0..mel_phrase_n(which) - 1; as set_opt */
void seq_mel_reset_phrasing(seq_t *s);                   /* VST-12: the type's phrasing; one undo step, none when there */
void seq_mel_set_knob(seq_t *s, int knob, int v);
void seq_mel_reset_knobs(seq_t *s);
void seq_mel_set_flags(seq_t *s, int rev, int inv, int shift, int undo);   /* the moves' flags (REVERSE, INVERT, SHIFT) */
int  seq_mel_type(const seq_t *s);                  /* -1 Manual, else MEL_* */
int  seq_mel_is_classic(const seq_t *s);            /* 1 on a gen_v1 Hook or Arp lane */
int  seq_mel_opt(const seq_t *s, int which);        /* 0 for a bad `which` */
int  seq_mel_phrase(const seq_t *s);                /* the old phrase the phrasing is exactly, else -1 */
int  seq_mel_phrasing(const seq_t *s, int which);   /* VST-12: MP_*, as the RECIPE tab shows it */
int  seq_part_mel_phrasing(const seq_t *s, int part, int which);   /* the same for any part (0 for a bad part) */
int  seq_mel_knob(const seq_t *s, int knob);        /* 0 for a bad knob */
void seq_mel_flags(const seq_t *s, int *rev, int *inv, int *shift);   /* NULL pointers are skipped */
