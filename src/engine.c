/* Cadenza plugin glue for mpc-vst-plugins' generic wrapper (wrapper/engine.h): skin parameters in, MIDI out. The
 * sequencer itself is src/seq_core.c (no host or ALSA code); this file turns the skin's parameter sets into seq_* calls,
 * makes every cell's text, light and dots, and sends the notes.
 *
 * MPC ignores a plugin's VST MIDI out, so notes go out through the plugin's own ALSA sequencer port, opened with
 * dlopen so the build needs no ALSA headers (the same approach as the toolchain's Maschine Group port). Each target
 * track picks this port as MIDI input with its lane's channel. MIDI coming back into our own track (MPC echoes the
 * port into it) is ignored: v1 takes no MIDI input.
 *
 * The pages (vst/make_skin.py builds the skin; params.json is append-only, so retired keys keep their slots and do nothing):
 *   PLAY     the position and chord readouts, the chord strip and playhead, the lanes' step cells and mutes, the part cells
 *            and the COMPLEXITY / SWING / GATE / DYNAMICS knobs
 *   CHORDS   JAM (the genre blocks); CHORDS, the sound page (its ring of knobs, the PROGRESSIONS and VOICING browsers);
 *            PHRASE (the strip of bars, the bar editor, COPY TO... and the RHYTHMS browser). Views on cview, the status row
 *            and strip on cmode (option names Ideas and Shape kept from 0.9: Ideas the sound page, Shape PHRASE)
 *   BASS     JAM; BASS, the sound page (its STYLE browser); PHRASE, as CHORDS's. Views on bview, the status row and strip on yview
 *   MELODY   JAM; PHRASE (the strip, the ANSWER BLOCK picker, the bar editor). Groups on mgroup, views on mview
 *   GLOBAL   key, scale, loop and chord length, MIDI channels, what CHORDS SHOW (degrees or names), tempo
 * Each key's history, version by version (what it was added for, what it does, what retired it): docs/ENGINE_HISTORY.md */
#include "engine.h"
#include "gen.h"
#include "jamlib.h"   /* 0.22 the CHORDS and BASS JAM blocks */
#include "melody.h"
#include "presets.h"   /* 0.23 the genre presets */
#include "rhythm.h"
#include "seq_core.h"
#include "theory.h"
#include "util.h"
#include "voicing.h"
#include "keyflags.h"   /* 0.19.1 vst/build: made by vst/make_skin.py */
#include <dlfcn.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TAP_LOG
#include <pthread.h>
#include <time.h>
#endif

/* 0.18 image buttons (TAPB entries): how many, the most cells one has, and how long a press's latch holds at most (1.5 s of
 * the wrapper's 44.1 kHz frames; see the TAPS comment) */
#define TAP_NBTN 17   /* 0.20: wsw_ (row 11) and wnk_ (row 12); 0.24 cqf_, uqf_ (13, 14), uqk_ (15) and cqx_, uqx_ (16, 17) */
#define TAP_BTN_CELLS 12
#define TAP_LATCH_FRAMES (3 * 44100 / 2)
/* 0.19.1 a message line goes blank 4 s after it was set (the wrapper's 44.1 kHz frames, counted in song_pos while stopped too). The
 * lines: each PHRASE page's rmsg (rpage_t.dl; 0.19 the RHYTHM pages'), then the BASS page's umsg, 0.20 the MELODY tab's jmsg */
#define MSG_FADE_FRAMES (4 * 44100)
enum { MSG_BASS = DL_COUNT, MSG_MEL, MSG_JAM, MSG_COUNT = MSG_JAM + DL_COUNT };   /* 0.20 the MELODY tab's jmsg; 0.22 MSG_JAM + DL_*:
                                                                                  * cjmsg, ujmsg */
#define JM_N (JAM_COLS * JAM_LEVELS + 4)   /* 0.22 a JAM page's meters: the 30 blocks', then the playing phrase's 4 bars' */

/* The 28-byte event ALSA's snd_seq_event_t uses, sent direct to whoever subscribed (on MPC: the track input it
 * creates for this port). */
typedef struct { unsigned char client, port; } seq_addr;
typedef struct {
    unsigned char type, flags, tag, queue;
    unsigned int time_tick, time_pad;
    seq_addr source, dest;
    unsigned char data[12];
} seq_ev;
#if defined(__arm__)
_Static_assert(sizeof(seq_ev) == 28, "ALSA snd_seq_event_t is 28 bytes");
#endif

enum {
    SEQ_OPEN_OUTPUT = 1,
    SEQ_CAP_READ = 1 << 0,
    SEQ_CAP_SUBS_READ = 1 << 5,
    SEQ_TYPE_MIDI = 1 << 1,
    SEQ_TYPE_APP = 1 << 20,
    SEQ_ADDR_UNKNOWN = 253,
    SEQ_ADDR_SUBS = 254,
    SEQ_QUEUE_DIRECT = 253,
    SEQ_NOTEON = 6,
    SEQ_NOTEOFF = 7,
    SEQ_CONTROLLER = 10
};

/* 0.13 the stepped knobs (KSTEPS, the k_* option params; COLOUR's k_col is apart) and every knob text the engine makes (the
 * dynamic_display params' <key>_display): the smooth knobs, then one per stepped knob */
enum { KS_HARM, KS_SPR, KS_BMOV, KS_BSYN, KS_BOCT, KS_BREG, KS_MRNG, KS_MLEA, KS_MSYN, KS_MTEN, KS_MANS, KS_MREG, KS_COUNT };
enum { KT_CX1, KT_CX2, KT_CX3, KT_SWING, KT_GATE, KT_DYN, KT_CGATE, KT_CDYN, KT_CSTRUM, KT_CPUSH, KT_BPUSH, KT_BGATE, KT_BACC,
       KT_MGAT, KT_JPUSH, KT_JDYN, KT_KS, KT_COUNT = KT_KS + KS_COUNT };   /* 0.20 KT_JPUSH, KT_JDYN */
/* What one part's Bass knob counts (bcount) were made from: everything seq_part_step_note reads for a Bass step (0.24.1: once the
 * FIX NOTE labels' too). Zero-filled, so equal inputs compare equal byte for byte; the same again, the counts are not made again. */
typedef struct {
    int key, scale, bars, chord_len, prog_len, hseed, vcol, style, seed, complexity;   /* 0.13: seed, complexity (bcount); 0.17 vcol */
    int prog[SEQ_MAX_PROG];
    unsigned char src[SEQ_MAX_PROG], colour[SEQ_MAX_PROG];
    int shape[LSHAPE_COUNT];
    seq_step_t steps[SEQ_LANE_STEPS];
    unsigned char lock[SEQ_LANE_STEPS / 8];
} fix_src_t;

/* 0.12: what one part's FIX slot labels were made from. Zero-filled, compared byte for byte. 0.17: vcol (the COLOUR knob's
 * level, which every Knobs slot plays) for 0.9's knob */
typedef struct {
    uint64_t lock;   /* 0.14: 64 slots; first, so the struct has no padding for the byte compare */
    int names, key, scale, prog_len, chord_len, vcol, hseed, bars, phrase, pad;   /* 0.14 bars, phrase; pad: explicit, zeroed */
    int prog[SEQ_SLOTS_ALL];   /* 0.24 the change points' virtual slots too (seq_slot_at reaches them: tri_text) */
    unsigned char src[SEQ_SLOTS_ALL], colour[SEQ_SLOTS_ALL];
    unsigned char ecol[SEQ_MAX_PROG];   /* 0.18: each slot's colour as it plays (COMPLEXITY's layers on a drawn lane) */
    seq_cp_t cp[SEQ_MAX_BARS][SEQ_CP_MAX];   /* 0.24 final review: the change points' steps (rbar_text reads the bar's changes) */
} slot_src_t;
/* 0.19 what one part's BASS page tension scores (bass_labels) were made from: the chords and the Bass lane's notes as they play.
 * Zero-filled, compared byte for byte: everything before note (the chords) for every bar, note a bar at a time */
typedef struct {
    int key, scale, bars, chord_len, prog_len, drawn;
    int prog[SEQ_MAX_PROG];
    unsigned char src[SEQ_MAX_PROG], note[SEQ_LANE_STEPS];
} bass_src_t;
/* 0.17 what one part's CHORDS page labels (page_labels) were made from: the slots, and the knobs a chord's drift and notes read */
typedef struct {
    slot_src_t slots;
    int vinv, vspr, vmode, rank;   /* rank: rank_len, the phrase a "not the phrase's chord" mark compares with; 0.18 vspr: SPREAD as
                                    * it plays (seq_part_vknob_eff) */
} page_src_t;

/* 0.24 one sound page's status row and centre texts (sound_cells_now): NOW PLAYING, the 4 bars' chord lines, the centre's name (cct_1 /
 * uct_1), where, notes and word; the CHORDS centre's drift level (cdrift). At most the wrapper's 64 bytes each. Task 8 fix: the BASS centre
 * bar's 16 dots (usz_1..16, bass_frame's frames, as PHRASE's) */
typedef struct {
    char now[DL_COUNT][48], bar[DL_COUNT][4][32], name[DL_COUNT][48], wh[DL_COUNT][64], nt[DL_COUNT][64], wd[DL_COUNT][64];
    int drift;
    unsigned char dots[SEQ_STEPS];
} sound_t;

/* 0.24 (Task 7) one PHRASE page's cells (ph_cells_now; the getters copy), per drawn lane: key what they were made at (left out of the
 * compare); the strip's tag lines and chord lines, its page readouts; the left panel's RHYTHM field and the three lines; the centre's where,
 * chords and end; the editor's title, its steps (what plays, the third line, the marks), whether the selected step is a hit, the BASS
 * piano's labels; COPY TO...'s bar cells and inner lines; the dots (64 strip, 16 centre: the editor shares them); the CHORDS KEYS on PHRASE */
typedef struct {
    int key;
    char strip[4][48], chords[4][40], page[16], pgn[24], field[48], fline[64], wline[64], aline[64], cw[64], cc[64], ce[64], title[64];
    char step[SEQ_STEPS][16], line[SEQ_STEPS][16], piano[12][12], copy[SEQ_MAX_BARS][24], inner[SEQ_MAX_BARS][48];
    unsigned char mark[SEQ_STEPS], dots[80], sel_hit, kedge;
    uint64_t keys;
} phcells_t;

typedef struct {
    seq_t seq;   /* first: a getter's seq_t pointer is its instance's (inst_of) */
    void *alsa_lib, *alsa;
    int alsa_port;
    int (*alsa_out)(void *, void *);
    int fix_key, fix_scale, fix_names;   /* the key, scale and NAMES fix_text and bnk_text were made for */
    char fix_text[7 + THEORY_NBORROW][16];   /* fc_ then fb_ labels ("iv" or "Am"), made in set_param (fix_labels), copied by the getters */
    char bnk_text[12][8];                    /* 0.10 bnk_ labels, C..B: "E 1" in the scale, "F" out of it (fix_labels) */
    unsigned char pcdeg[12];                 /* 0.20 each pitch class's scale degree 1..7, 0 out of the scale (fix_labels) */
    /* 0.11 MELODY: mbars per part (made in set_param, part_labels), Pentatonic's Home note names (fix_labels), and per type mkst and
     * mrk_ (made once, in create) */
    fix_src_t fix_src[SEQ_NPARTS];   /* what bcount was made from, per part (calloc: never a real state) */
    char mbars_text[SEQ_NPARTS][64];
    char pent_text[2][16];
    char mkst_text[MEL_NTYPES][64], mrk_text[MEL_NTYPES][32];
    /* 0.12 per part, made in set_param (slot_labels) for the playing and the queued part: the FIX slot labels, [0] plain and [1]
     * with the playing marker (the getter picks by seq_cur_slot); what they were made from */
    char fs_text[SEQ_NPARTS][2][8][40];
    char tri_text[SEQ_NPARTS][SEQ_SLOTS_ALL][8];   /* each slot's triad name ("F#dim"): PLAY's bar_ and l1_ cells (0.24 and each
                                                    * change point's, read through seq_slot_at) */
    slot_src_t slot_src[SEQ_NPARTS];   /* calloc: prog_len 0, never a real state */
    /* 0.17 the CHORDS page per part, made in set_param (page_labels) for the playing and the queued part: each slot's drift
     * score (theory_drift), its strip text (cst_) and its numeral and notes (ccd); what they were made from */
    unsigned char drift[SEQ_NPARTS][SEQ_MAX_PROG];
    char cst_text[SEQ_NPARTS][SEQ_MAX_PROG][24];
    char ccd_text[SEQ_NPARTS][SEQ_MAX_PROG][40];
    page_src_t page_src[SEQ_NPARTS];
    int shown[SEQ_NPARTS];   /* seq_part_prog_preset_shown: the PROGRESSION field's preset, -1 Custom (part_labels) */
    char cvo_text[SEQ_NPARTS][32];   /* the VOICING field's text, made from cvo_src (part_labels) */
    int cvo_src[SEQ_NPARTS][8];      /* style, mode, COLOUR, INVERSION, SPREAD, STRUM, its direction, OCTAVE; [0] -2: not made */
    char bpr_text[16][32];   /* each preset's tile, "Epic \xC2\xB7 Em C G D" (fix_labels: key and scale), BPR_CHARS */
    char cpr_text[16][32];   /* the same as the PROGRESSION field's text, PROG_CHARS */
    int screen_sig;          /* cview, cdrift and the four tints as last reported (screen_sig): a change bumps display_rev; 0.19 the BASS
                              * tab's view and mode too */
    /* 0.13 per part, made in set_param (part_labels) for the playing and the queued part: the Bass transforms' note counts (with
     * bst), and every knob's text with the value it was made from (knob_labels) */
    int bcount[SEQ_NPARTS][3];
    char ktext[SEQ_NPARTS][KT_COUNT][32];
    int kmade[SEQ_NPARTS][KT_COUNT];   /* -1: not made */
    /* 0.13 the load guard: bit i (KOPTIONS order) armed by a state load, so a host restoring k_* params.json defaults after
     * it (a 0.12 project has no values for them) leaves the loaded values alone; blocks since the load (disarmed at 64) */
    uint64_t kguard;   /* 0.21: 64 bits (KOPTIONS has 32; 0.22 38; 0.24 40) */
    int kguard_blocks;
    int program;   /* the PRESET menu's program, from a pick to the next set of any other key, then 0: 0.15 1 INIT (and a new
                    * instance), 0.23 2.. a genre preset (presets.c: preset program - 2) */
    /* 0.18 the RHYTHM page: each bar's chords per part ("Em C", the triads as the CHORDS strip names them; made with the slot
     * labels) and how many; the selected paint (rhythm.h RC_*) and the message line (set by the page's actions, on the UI thread) */
    char rbar_text[SEQ_NPARTS][SEQ_MAX_BARS][40];
    unsigned char rbar_n[SEQ_NPARTS][SEQ_MAX_BARS];
    int rpaint[DL_COUNT];
    char rmsg[DL_COUNT][64];   /* at most what the wrapper reads (vst/vst.json PARAM_TEXT_MAX); 0.19 per RHYTHM page, PHRASE's now (rpage_t.dl) */
    /* 0.19 the BASS page: each bar's tension score per part (seq_part_bass_bar_score, made in set_param for the playing and the
     * queued part: bass_labels) and what they were made from; PEDAL's cell texts ("TONIC I \xC2\xB7 C": fix_labels, key and scale);
     * the message line (set by the page's actions, on the UI thread) */
    unsigned char bscore[SEQ_NPARTS][SEQ_MAX_BARS];
    bass_src_t bscore_src[SEQ_NPARTS];
    char ucn_text[SEQ_NPARTS][SEQ_MAX_BARS][64];   /* ucn per bar ("G1 D2 G2 \xC2\xB7 5 notes"), made with bscore; the wrapper's 64 */
    char upd_text[3][24];
    char umsg[64];
    /* 0.19 the BASS tab's KEYS per part and bar (bass_labels): the keys each bar's notes light, MIDI SEQ_KEY_LO.. (a note outside folded
     * in by octaves) and its edges (bit 0 below, bit 1 above); ukbar the bar they show (the BASS tab's shown bar, made in set_param and
     * song_pos, so the polled getters only bit-test); the STYLE browser's tile pictures and summaries (utp_, uts_, made in set_param
     * while it is open) */
    uint64_t bkeys[SEQ_NPARTS][SEQ_MAX_BARS];
    unsigned char bkedge[SEQ_NPARTS][SEQ_MAX_BARS];
    int ukbar;
    int ufirst, ucentre;   /* 0.19.1 the BASS page's strip page first bar and centre bar as last shown there (bass_shown_now) */
    unsigned char uplaying, utap;   /* 0.19.1 and uwh's "PLAYING" and "tap to change" as last shown there */
    char utp_text[4][SEQ_STEPS + 1];
    char uts_text[4][48];
    /* 0.18 frames rendered (song_pos), and each image button cell's press latch: frames + 1 when it acted, 0 clear (tap_latch) */
    unsigned long long frames, latch_at[TAP_NBTN][TAP_BTN_CELLS];
    unsigned long long msg_at[MSG_COUNT];   /* 0.19.1 frames when each message line was set (MSG_FADE_FRAMES) */
    /* 0.20 the MELODY tab: its message line (jmsg); its KEYS per part and bar (mel_key_labels: as bkeys, from the JAM lane's line, which
     * they were made from) and the bar they show (jkbar, mel_shown_now); the step dots (zdot: zs_1..64 then zc_1..16) and the strip page's
     * first bar, the centre bar and the part they show */
    char mmsg[64];
    uint64_t mkeys[SEQ_NPARTS][SEQ_MAX_BARS];
    unsigned char mkedge[SEQ_NPARTS][SEQ_MAX_BARS];
    mjam_note_t mkeys_src[SEQ_NPARTS][SEQ_LANE_STEPS];
    int jkbar;
    unsigned char zdot[80];
    int zfirst, zcentre, zpart;
    int cx3_sent, cx3_dir;          /* the cx_3 value MPC sent last on a JAM lane and the way it turned (0 a jump), at frame cx3_at */
    unsigned long long cx3_at;
    /* 0.22 the CHORDS and BASS JAM pages, per drawn lane (DL_*): their message lines (cjmsg, ujmsg); knob 1's turn on JAM (k_s1, k_b1: as
     * cx3_sent, cx3_dir, cx3_at); the meters' frames (cjm_ / ujm_ then cjq_ / ujq_) and what NOW PLAYING and NEXT read (jsig), made by
     * jam_cells_now after a set, a block switch or a new bar (jkey: the part, bar and transport they were made at) */
    char djmsg[DL_COUNT][64];
    int k1_sent[DL_COUNT], k1_dir[DL_COUNT];
    unsigned long long k1_at[DL_COUNT];
    unsigned char jmeter[DL_COUNT][JM_N];
    int jsig[DL_COUNT][7], jkey;
    /* 0.24 the CHORDS and BASS sound pages' status row and centre, per drawn lane (DL_*), made by sound_cells_now after a set, a block
     * switch or a new bar (skey: the part, bar and transport they were made at); the getters copy */
    sound_t snd;
    int skey;
    phcells_t ph[DL_COUNT];   /* 0.24 (Task 7) the PHRASE pages' cells (ph_cells_now) */
    int snd_made;   /* 0.24 final review: sound_cells_now built both lanes once (its first call, create's, builds every lane) */
} inst_t;

static const inst_t *inst_of(const seq_t *q) { return (const inst_t *)q; }
static inst_t *inst_rw(seq_t *q) { return (inst_t *)q; }   /* a tap's instance (the seq_t is its first member) */
/* 0.19.1 message line m's text ("" none), and whether it shows at frame `at` (set less than MSG_FADE_FRAMES before) */
static const char *msg_of(const inst_t *s, int m) {
    return m == MSG_BASS ? s->umsg : m == MSG_MEL ? s->mmsg : m >= 0 && m < DL_COUNT ? s->rmsg[m] : m >= MSG_JAM && m < MSG_COUNT ? s->djmsg[m - MSG_JAM] : "";
}
static int msg_shows(const inst_t *s, int m, unsigned long long at) { return msg_of(s, m)[0] && at - s->msg_at[m] < MSG_FADE_FRAMES; }

static void alsa_close(inst_t *s) {
    if (s->alsa && s->alsa_lib) {
        int (*close_fn)(void *) = (int (*)(void *))dlsym(s->alsa_lib, "snd_seq_close");
        if (close_fn) close_fn(s->alsa);
    }
    if (s->alsa_lib) dlclose(s->alsa_lib);
    s->alsa_lib = s->alsa = NULL;
    s->alsa_out = NULL;
    s->alsa_port = -1;
}

/* No ALSA (the offline test container) just means no port: the sequencer still runs. */
static void alsa_open(inst_t *s) {
    s->alsa_port = -1;
    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    int (*open_fn)(void **, const char *, int, int) = (int (*)(void **, const char *, int, int))dlsym(lib, "snd_seq_open");
    int (*name_fn)(void *, const char *) = (int (*)(void *, const char *))dlsym(lib, "snd_seq_set_client_name");
    int (*port_fn)(void *, const char *, unsigned, unsigned) = (int (*)(void *, const char *, unsigned, unsigned))dlsym(lib, "snd_seq_create_simple_port");
    int (*out_fn)(void *, void *) = (int (*)(void *, void *))dlsym(lib, "snd_seq_event_output_direct");
    void *seq = NULL;
    if (!open_fn || !name_fn || !port_fn || !out_fn || open_fn(&seq, "default", SEQ_OPEN_OUTPUT, 0) < 0 || !seq) {
        dlclose(lib);
        return;
    }
    s->alsa_lib = lib;
    s->alsa = seq;
    name_fn(seq, "Cadenza");   /* the port MPC lists: "Cadenza MIDI Out" */
    s->alsa_port = port_fn(seq, "MIDI Out", SEQ_CAP_READ | SEQ_CAP_SUBS_READ, SEQ_TYPE_MIDI | SEQ_TYPE_APP);
    if (s->alsa_port < 0) { alsa_close(s); return; }
    s->alsa_out = out_fn;
}

static void alsa_send(void *ctx, int status, int a, int b) {
    inst_t *s = ctx;
    if (!s->alsa_out || a < 0 || a > 127) return;
    seq_ev ev;
    memset(&ev, 0, sizeof ev);
    ev.queue = SEQ_QUEUE_DIRECT;
    ev.dest.client = SEQ_ADDR_SUBS;
    ev.dest.port = SEQ_ADDR_UNKNOWN;
    ev.source.port = (unsigned char)s->alsa_port;
    ev.data[0] = (unsigned char)(status & 0x0f);
    if ((status & 0xf0) == 0xB0) {   /* snd_seq_ev_ctrl_t: channel, 3 unused bytes, unsigned param, signed value */
        unsigned int param = (unsigned int)a;
        int value = b;
        ev.type = SEQ_CONTROLLER;
        memcpy(&ev.data[4], &param, 4);
        memcpy(&ev.data[8], &value, 4);
    } else {
        int on = (status & 0xf0) == 0x90 && b > 0;
        ev.type = (unsigned char)(on ? SEQ_NOTEON : SEQ_NOTEOFF);
        ev.data[1] = (unsigned char)a;
        ev.data[2] = (unsigned char)(on ? b : 0);
    }
    s->alsa_out(s->alsa, &ev);
}

static void fix_labels(inst_t *s);
static int log2i(int v);
static void part_labels(inst_t *s);
static void part_labels_of(inst_t *s, int part);
static void mel_type_labels(inst_t *s);
static int screen_sig(const seq_t *q);
static void tap_index_init(void);
static void bass_shown_now(inst_t *s);
static void bknob_msg(seq_t *q, int k);
static void mel_key_labels(inst_t *s, int part);
static void mel_shown_now(inst_t *s, int force);
static void jam_cells_now(inst_t *s, int force);
static void sound_cells_now(inst_t *s, int force);
static void ph_cells_now(inst_t *s, int force);
static void ph_cells_init(inst_t *s);

static void *create(const char *data_dir) {
    (void)data_dir;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    seq_init(&s->seq, alsa_send, s);
    seq_init_preset(&s->seq);   /* 0.15: a new instance starts at INIT; a project's own state replaces it on load */
    s->program = 1;
    seq_set_cview(&s->seq, CVIEW_JAM);   /* 0.22: the CHORDS and BASS tabs open on JAM (a project load too: views are never saved) */
    seq_set_bview(&s->seq, BVIEW_JAM);
    s->rpaint[DL_CHORDS] = s->rpaint[DL_BASS] = RC_HIT;
    s->fix_key = -1;
    memset(s->kmade, 0xff, sizeof s->kmade);
    for (int p = 0; p < SEQ_NPARTS; p++) s->cvo_src[p][0] = -2;
    tap_index_init();
    fix_labels(s);
    part_labels(s);
    mel_type_labels(s);
    s->screen_sig = screen_sig(&s->seq);
    mel_shown_now(s, 1);
    s->jkey = -1;
    jam_cells_now(s, 1);
    s->skey = -1;
    sound_cells_now(s, 1);
    ph_cells_init(s);
    alsa_open(s);
    return s;
}

static void destroy(void *p) {
    inst_t *s = p;
    seq_all_notes_off(&s->seq);
    alsa_close(s);
    free(s);
}

static void midi(void *p, const uint8_t *msg, int len) { (void)p; (void)msg; (void)len; }

static void render(void *p, int16_t *out_lr, int frames) {
    (void)p;
    memset(out_lr, 0, (size_t)frames * 2 * sizeof *out_lr);
}

#define KGUARD_BLOCKS 64   /* the load guard's window: about 0.19 s of 128-frame blocks, past the host's restore */
static void song_pos(void *p, double ppq, double bpm, int playing, int frames) {
    inst_t *s = p;
    unsigned long long was = s->frames;
    s->frames += frames > 0 ? (unsigned)frames : 0;   /* 0.18: the image buttons' latch clock (tap_latch); 0.19.1 the messages' */
    for (int m = 0; m < MSG_COUNT; m++)
        if (msg_shows(s, m, was) && !msg_shows(s, m, s->frames)) s->seq.display_rev++;   /* MPC re-reads the faded (unpolled) line */
    if (s->kguard && ++s->kguard_blocks >= KGUARD_BLOCKS) s->kguard = 0;   /* a user's first turn onto a default step applies */
    seq_block(&s->seq, ppq, bpm, playing, frames);
    int switched = seq_mel_poll(&s->seq);
    if (switched) {   /* 0.20: a JAM block switched in time: MPC re-reads cx_3; the KEYS and dots follow the new line */
        s->seq.display_rev++;
        mel_key_labels(s, seq_part(&s->seq));
    }
    bass_shown_now(s);   /* 0.19: the BASS tab's KEYS follow the bar on show (it moves at a bar line) */
    mel_shown_now(s, switched);   /* 0.20: the MELODY tab's KEYS and dots too */
    int jam_switched = seq_jam_poll(&s->seq) != 0;
    /* 0.22: a timed block can change COLOUR and the BASS notes: the chord names, scores and knob texts. Some parts always
     * rebuild; all are bounded, at most once per bar line */
    if (jam_switched) part_labels_of(s, seq_part(&s->seq));
    jam_cells_now(s, jam_switched);   /* 0.22: a CHORDS or BASS block switched in time, a new bar: the JAM meters */
    sound_cells_now(s, jam_switched);   /* 0.24: the sound pages' status row and centre (at most once a bar) */
    ph_cells_now(s, jam_switched);      /* 0.24 Task 7: the PHRASE pages' cells and dots (at most once a bar) */
    int sig = screen_sig(&s->seq);   /* 0.17: FOLLOW turned the strip's page, the centre moved, a stale picker closed */
    if (sig != s->screen_sig) {
        s->screen_sig = sig;
        s->seq.display_rev++;   /* MPC re-reads tint_, cdrift and cview: only on a change, never every block */
    }
}

/* The getters run on the audio thread (the wrapper's poll): chord and idea text comes from caches built by the setters and is
 * copied or appended here; only short numbers (options, BPM, display_rev) still go through snprintf */
static int put(char *buf, int len, const char *text) {   /* returns strlen(text), as snprintf "%s" would */
    int n = (int)strlen(text), k = n < len - 1 ? n : len - 1;
    if (len <= 0) return n;
    memcpy(buf, text, (size_t)k);
    buf[k] = 0;
    return n;
}

static int put_flag(char *buf, int len, int on) { return put(buf, len, on ? "1" : "0"); }
static int put_upper(char *buf, int len, const char *text) {
    int n = 0;
    for (; text[n] && n < len - 1; n++) buf[n] = (char)(text[n] >= 'a' && text[n] <= 'z' ? text[n] - 'a' + 'A' : text[n]);
    if (len > 0) buf[n] = 0;
    return n;
}

/* Text built piece by piece into buf, cut to len (NUL-terminated after every piece); n counts what did not fit too */
typedef struct { char *b; int len, n; } txt_t;
static void add(txt_t *t, const char *s) {
    for (; *s; s++, t->n++)
        if (t->n < t->len - 1) t->b[t->n] = *s;
    if (t->len > 0) t->b[t->n < t->len - 1 ? t->n : t->len - 1] = 0;
}
static void add_int(txt_t *t, int v) {   /* 0..9999 */
    char d[6], *p = d + sizeof d - 1;
    *p = 0;
    do *--p = (char)('0' + v % 10); while ((v /= 10) > 0 && p > d);
    add(t, p);
}

/* The style options of each lane, as gen.h's ids (0 is always GEN_MANUAL; -1 pads a lane with fewer options). The getters
 * answer the engine's current value: undo can change a style or complexity behind MPC's back. 0.12: style_1 gained the six
 * new chord rhythms at its end (an option's position is its saved value). */
#define STYLE_NOPTS 10
static const int STYLE_IDS[SEQ_LANES][STYLE_NOPTS] = {
    {GEN_MANUAL, GEN_CH_SUSTAIN, GEN_CH_PULSE, GEN_CH_STABS, GEN_CH_OFFBEAT, GEN_CH_CHARLESTON, GEN_CH_BALLAD, GEN_CH_FUNK,
     GEN_CH_HALFTIME, GEN_CH_PUSH},
    {GEN_MANUAL, GEN_BASS_ROOT, GEN_BASS_PUMP, GEN_BASS_WALK, -1, -1, -1, -1, -1, -1},
    {GEN_MANUAL, GEN_MEL_HOOK, GEN_MEL_ARP, -1, -1, -1, -1, -1, -1, -1},
    {GEN_MANUAL, GEN_MEL_ANSWER, GEN_MEL_ARP, -1, -1, -1, -1, -1, -1, -1}};

/* Tap cells: "display":"string" list cells (a list cell is the only control MPC lets us size to a 64 px touch target) that
 * the engine treats as buttons. A tap sends the cell's new state: 1 on an unlit cell, 0 on the lit one (the wrapper drops a
 * set equal to the last "_on" it polled, its echo guard). A selector ignores the 0, so re-tapping the selection keeps it; a
 * toggle takes the value; an action acts on any value and never lights. One hybrid: bar_ is a selector that takes a "0" only
 * while FOLLOW is on (its lit cell is then the playing bar, and tapping it picks that bar), so its entry has select 0 and
 * bar_tap drops the other "0"s itself. on and label get i, the 0-based cell.
 * 0.18: an image button (layout.conf "button": cpr_, cvo_, rfl_, rac_3..6, rcx_4, the cst_ hit plates, kyb_) sends two sets per
 * touch: its component's Toggle Switch flips the value and so does its Button, which no label covers (a list row's label takes
 * the touch), the second set undoing the first (1 then 0 on an unlit cell). So an entry with a button is an action that never
 * lights and ignores the 0 (select 1: its list cells, never lit, send 1), or its second act must be a no-op (cst_, a list cell
 * too). LOCK (rac_3) says UNLOCK instead of lighting, as the picker's plk_ does.
 * 0.18 (device tap log, with that rule in): one 350 ms touch on LOCK sent 1 on the press and 1 again on the release, 400 ms
 * later: the 10 ms poll had pushed the never-lit 0 back, so the release's toggle flipped MPC's 0 to 1. So a button cell of a TAPB
 * entry (rac_3..6, rfl_1..3, rcx_4, cpr_1..3, cvo_1..3; its mask) latches: the 1 that acts lights it (its "_on" reads 1, so the
 * poll leaves MPC's value at 1 and the release's toggle sends 0), until a 0 comes or TAP_LATCH_FRAMES pass (a release that never
 * comes: the view changed under the finger); a 1 while latched does nothing (tap_latch). Lit, it shows its "on" image (the amber
 * plate or arrow) while held: press feedback. Its list cells (rac_1, rac_2, rcx_1..3: a list row sends one set) don't latch.
 * 0.19 the BASS tab's image buttons: usy_1..3, yac_3..6, yfl_1..3, ycx_4 (TAPB, as cvo_, rac_, rfl_, rcx_) and unk_1..12, the bar
 * editor's piano keys (TAPBL: a latching button that lights, lit = the selected step's note; select 1, so a touch fixes and
 * auditions once. A touch on the lit key, the step's own note: its press's 0 is ignored, but on the device the release toggles
 * MPC's value back to 1 about 400 ms later, which fixes the step at the note it already has and auditions it then); PEDAL's
 * upd_1..3 and the 0.22 JAM mode tabs cvm_4 and ubm_3, image hit plates like it (select 1, lit once it acts, so the release's
 * 0 is ignored); the ust_ and ysc_ hit plates (select 0, as cst_: the
 * second act, on a bar already open, is a no-op) and the uti_ ones (select 0: the second act tries the style already tried, which
 * changes nothing). 0.24.1 rac_, rfl_, rcx_ and their BASS twins are retired (TAPR: no row; rows 1-3 and 7-9 stay unused): PHRASE's
 * cqf_ / uqf_ and cqx_ / uqx_ latch as they did */
typedef struct {
    const char *prefix; int plen, n, select;         /* plen: strlen(prefix); select: a "0" is ignored */
    int (*on)(const seq_t *, int i);                 /* the cell's lit state */
    void (*tap)(seq_t *, int i, int val);            /* val: the 0/1 MPC sent */
    int (*label)(const seq_t *, int i, char *, int);
    int btn; unsigned mask;                          /* 0.18 TAPB: its row in inst_t.latch_at + 1 (0 none), its button cells */
} tap_t;

static const char *const LANE_NAMES[SEQ_ACTIVE_LANES] = {"CHORDS", "BASS", "MELODY"};
static const int WHITE_KEYS[7] = {0, 2, 4, 5, 7, 9, 11}, BLACK_KEYS[5] = {1, 3, 6, 8, 10};
static const char *const LENGTH_NAMES[5] = {"1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars"};
static const char *const CHORD_LEN_NAMES[4] = {"2 Bars", "1 Bar", "1/2 Bar", "1/4 Bar"};

static int never_on(const seq_t *q, int i) { (void)q; (void)i; return 0; }
static int mt_on(const seq_t *q, int i) { return q->mute[i]; }
static void mt_tap(seq_t *q, int i, int v) { seq_set_mute(q, i, v); }
static int mt_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, LANE_NAMES[i]); }
/* The strip's 8 cells are bars 1..8, lit on seq_strip_bar (the playing bar while following; the grid keeps its bar); past the
 * loop a cell is blank and inert. A tap picks that bar (FOLLOW, a GLOBAL setting, stays). A selector but for FOLLOW: the lit
 * cell is then the playing bar, and its tap (a "0") still picks it. */
static int bar_on(const seq_t *q, int i) { return i < seq_bars(q) && seq_strip_bar(q) == i; }
static void bar_tap(seq_t *q, int i, int v) {
    if (i >= seq_bars(q) || (!v && !seq_follow(q))) return;
    seq_set_view(q, i);
}
/* "1 F#dim": the bar and the triad it starts on (0.12: no colour, one space, for the larger text; vchord has the colour) */
static int bar_label(const seq_t *q, int i, char *buf, int len) {
    txt_t t = {buf, len, 0};
    if (i >= seq_bars(q)) return put(buf, len, " ");
    add_int(&t, i + 1);
    add(&t, " ");
    add(&t, inst_of(q)->tri_text[seq_part(q)][seq_slot_at(q, i, 0)]);
    return t.n;
}
static int fol_on(const seq_t *q, int i) { (void)i; return seq_follow(q); }
static void fol_tap(seq_t *q, int i, int v) { (void)i; seq_set_follow(q, v); }
static int fol_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "FOLLOW"); }
static void act_tap(seq_t *q, int i, int v) { (void)v; if (i == 0) seq_undo(q); else seq_all_notes_off(q); }
static int act_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, i == 0 ? "UNDO" : "ALL NOTES OFF"); }
static int kw_on(const seq_t *q, int i) { return seq_key(q) == WHITE_KEYS[i]; }
static void kw_tap(seq_t *q, int i, int v) { (void)v; seq_set_key(q, WHITE_KEYS[i]); }
static int kw_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, theory_key_name(WHITE_KEYS[i])); }
static int kb_on(const seq_t *q, int i) { return seq_key(q) == BLACK_KEYS[i]; }
static void kb_tap(seq_t *q, int i, int v) { (void)v; seq_set_key(q, BLACK_KEYS[i]); }
static int kb_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, theory_key_name(BLACK_KEYS[i])); }
/* kb2_1..3: F#, G#, A# again as their own list, so the skin can set them over F-G, G-A, A-B (kb_3..5 stay, unused by the skin) */
static int kb2_on(const seq_t *q, int i) { return kb_on(q, i + 2); }
static void kb2_tap(seq_t *q, int i, int v) { kb_tap(q, i + 2, v); }
static int kb2_label(const seq_t *q, int i, char *buf, int len) { return kb_label(q, i + 2, buf, len); }
static int sc_on(const seq_t *q, int i) { return seq_scale(q) == i; }
static void sc_tap(seq_t *q, int i, int v) { (void)v; seq_set_scale(q, i); }   /* the 8th tile is past SCALE_COUNT: ignored */
static int sc_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, i < SCALE_COUNT ? theory_scale_name(i) : " "); }
static int ln_on(const seq_t *q, int i) { return seq_bars(q) == 1 << i; }
static void ln_tap(seq_t *q, int i, int v) { (void)v; seq_set_bars(q, 1 << i); }
static int ln_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, LENGTH_NAMES[i]); }
static int cl_on(const seq_t *q, int i) { return seq_chord_len(q) == 32 >> i; }
static void cl_tap(seq_t *q, int i, int v) { (void)v; seq_set_chord_len(q, 32 >> i); }
static int cl_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, CHORD_LEN_NAMES[i]); }
static void new_tap(seq_t *q, int i, int v) { (void)q; (void)i; (void)v; }   /* 0.18: NEW IDEA is dropped (spec); it still answers */
static int new_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "NEW IDEA"); }
static void gl_tap(seq_t *q, int i, int v) { (void)v; if (i < SEQ_ACTIVE_LANES) seq_generate(q, i); else seq_generate_progression(q); }
static int gl_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, i < SEQ_ACTIVE_LANES ? "GEN" : "NEW PROGRESSION"); }

static const char *const MODE_NAMES[3] = {"IDEAS", "SHAPE", "FIX"};   /* 0.12: FIX, for the larger text */
/* 0.17: the CHORDS page's views by mode cell (cvm_, and 0.9's cmode and md_: Ideas is Chords, Shape is Rhythm); the browsers
 * are Chords's. 0.18: an open bar, its COPY TO... and the RHYTHMS browser are Rhythm's. 0.24.1 Fix (2) is never a view */
static const int VIEW_OF_MODE[4] = {CVIEW_CHORDS, CVIEW_RHYTHM, CVIEW_CHORDS, CVIEW_JAM};   /* 0.22 JAM 3 (cvm_4); 2 unused */
static int get_mode(const seq_t *q) {
    int v = seq_cview(q);
    if (v == CVIEW_JAM) return 3;
    return v == CVIEW_RHYTHM || v == CVIEW_BAR || v == CVIEW_COPY || v == CVIEW_RBROWSE ? 1 : 0;
}
static void set_mode(seq_t *q, int i) {   /* a mode change cancels a picker or browser; an open bar (its COPY TO... or RHYTHMS too)
                                            * is kept as DONE (ruling R32). 0.24 FIX (2) is retired: nothing (cvm_3, md_3, cmode Fix) */
    i = i < 0 ? 0 : i > 3 ? 3 : i;
    if (i == get_mode(q) || i == 2) return;
    seq_set_cview(q, VIEW_OF_MODE[i]);
    q->display_rev++;
}
/* 0.22 cmode (0.9's) gains Jam (3): on JAM it reads Jam, so none of the CHORDS page's cmode:Ideas / cmode:Shape layers shows there; a
 * set of what it reads is MPC's echo; md_ none lit on JAM */
static int get_cmode(const seq_t *q) { return get_mode(q); }
static void set_cmode(seq_t *q, int i) { if (i != get_cmode(q)) set_mode(q, i); }
static int md_on(const seq_t *q, int i) { return get_mode(q) == i; }
static int md_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, MODE_NAMES[i]); }

/* 0.9 IDEAS, retired (KF_RETIRED_24: a set does nothing; IDEAS left the code after 0.24): ds_ the distance, fd_ the field (5 =
 * YOURS), ia_ the actions, pn_ the pins, mv_ the moves; each still answers as it read with nothing tried: Near and YOURS lit,
 * the field blank, REROLL / UNDO / PIN, empty pins, the moves' names. The BASS and MELODY tabs' (bds_ .. bmv_, mds_ .. mmv_)
 * read the same */
static const char *const DIST_NAMES[3] = {"Near", "Far", "Wild"};
static int ds_on(const seq_t *q, int i) { (void)q; return i == 0; }
static int ds_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, DIST_NAMES[i]); }
static int fd_on(const seq_t *q, int i) { (void)q; return i == 4; }
static int fd_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, " "); }
static int ia_label(const seq_t *q, int i, char *buf, int len) {
    static const char *const IDLE[3] = {"REROLL", "UNDO", "PIN"};
    (void)q;
    return put(buf, len, IDLE[i]);
}
static int pn_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "empty"); }
static int mv_label(const seq_t *q, int i, char *buf, int len) {
    static const char *const MOVES[6] = {"SUBSTITUTE", "BORROW", "ADD COLOUR", "RESOLVE", "SIMPLIFY", "SURPRISE"};
    (void)q;
    return put(buf, len, MOVES[i]);
}

/* 0.9 FIX, retired (KF_RETIRED_24; 0.24.1 its selections and chord_mode left the code): fs_ and fsl_ the slots (lit = the slot the
 * bar a pick edited starts on: slot 1 of seq_fix_bar, as selected; texts from the caches, slot_labels), fbs_ the bar on view's slots
 * (blank, lit = the first, as selected), fbar_ the bar strip (never lit; 0.24.1 blank, its texts gone) */
static int fix_span(const seq_t *q) { return seq_chord_len(q) > SEQ_STEPS && seq_bars(q) > 1 ? 2 : 1; }      /* bars a chord */
static int fs_on(const seq_t *q, int i) { return i == seq_fix_bar(q) * SEQ_STEPS / seq_chord_len(q) % seq_prog_len(q); }
static int fbs_on(const seq_t *q, int i) { (void)q; return i == 0; }
static int fbs_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, " "); }
/* Where slot i of chord length cl starts, as bar.beat: "1.1", "1.3"; "1-2" for 2-bar chords (0.12, for the larger text) */
static void slot_where(int cl, int i, txt_t *t) {
    int start = i * cl, bar = start / SEQ_STEPS + 1;
    add_int(t, bar);
    add(t, cl >= 2 * SEQ_STEPS ? "-" : ".");
    add_int(t, cl >= 2 * SEQ_STEPS ? bar + 1 : start % SEQ_STEPS / 4 + 1);
}
/* "1.1  iv7  *" (names: "1.1  Am7  *"; * locked), "> " before it while it sounds: copied from the caches (slot_labels) */
static int fs_label(const seq_t *q, int i, char *buf, int len) {
    return put(buf, len, inst_of(q)->fs_text[seq_part(q)][seq_cur_slot(q) == i][i]);
}
/* 0.17 a progression preset's field and tile text: its short name (the name without its "1-6-3-7 " degrees) and its first 4
 * chords in the key, "Epic \xC2\xB7 Em C G D"; past chars the last chords go until it fits: PROG_CHARS the left panel's field
 * (22 px), BPR_CHARS the PROGRESSIONS tiles (24 px, 215 px: vst/make_skin.py chords_progressions) */
#define PROG_CHARS 22
#define BPR_CHARS 19
/* An edited name's mark after it: " (edited)", or " (ed.)" where name and " (edited)" pass PROG_CHARS (the left panel's fields) */
static void add_edited(txt_t *t, const char *name) { add(t, theory_text_width(name) + 9 <= PROG_CHARS ? " (edited)" : " (ed.)"); }
static const char *preset_name(int i) {
    const char *n = THEORY_PRESETS[i].name, *sp = strchr(n, ' ');
    return n[0] >= '0' && n[0] <= '9' && sp ? sp + 1 : n;
}
static void prog_text(int key, int scale, int i, int chars, char *buf, int len) {
    const theory_preset_t *r = &THEORY_PRESETS[i];
    for (int n = r->len < 4 ? r->len : 4; n >= 0; n--) {
        txt_t t = {buf, len, 0};
        add(&t, preset_name(i));
        if (n) add(&t, " \xC2\xB7");
        for (int k = 0; k < n; k++) {
            char c[16];
            theory_chord_name(key, scale, r->deg[k], c, sizeof c);
            add(&t, " ");
            add(&t, c);
        }
        if (theory_text_width(buf) <= chars) return;
    }
}

/* 0.19 the pitch class PEDAL p (1 tonic, 2 dominant) holds: the key's degree 1 or 5 */
static int pedal_pc(int key, int scale, int p) { return theory_degree_pc(key, scale, p == 2 ? 4 : 0); }

/* 0.19 upd_ PEDAL's cells: "OFF", "TONIC i \xC2\xB7 E" (I on a major-third scale), "DOMINANT V \xC2\xB7 B": the key's degree 1 and 5 */
static void pedal_labels(inst_t *s) {
    static const char *const NAME[3] = {"OFF", "TONIC ", "DOMINANT "};
    int key = seq_key(&s->seq), sc = seq_scale(&s->seq), tonic = theory_degree_pc(key, sc, 0);
    int minor = pmod(theory_degree_pc(key, sc, 2) - tonic, 12) == 3;
    for (int i = 0; i < 3; i++) {
        txt_t t = {s->upd_text[i], sizeof s->upd_text[i], 0};
        add(&t, NAME[i]);
        if (i) add(&t, i == 2 ? "V" : minor ? "i" : "I"), add(&t, " \xC2\xB7 "), add(&t, theory_key_name(pedal_pc(key, sc, i)));
    }
}

/* fc_, fb_, bnk_ (mnk_) and Pentatonic's Home note read only the key, scale and NAMES: made again on the UI thread (set_param)
 * when one changed. fc_ and fb_ are one token, the triad's degree ("iio", "bVI") or, with NAMES, its name ("F#dim"). 0.17: the
 * progression presets' texts too */
static void fix_labels(inst_t *s) {
    const seq_t *q = &s->seq;
    int sc = seq_scale(q), names = seq_names(q);
    if (seq_key(q) == s->fix_key && sc == s->fix_scale && names == s->fix_names) return;
    for (int i = 0; i < 7 + THEORY_NBORROW; i++) {
        int ss = i < 7 ? sc : theory_borrow_scale(sc, i - 6), deg = i < 7 ? i : theory_borrow_degree(sc, i - 6);
        if (names) theory_chord_name(seq_key(q), ss, deg, s->fix_text[i], sizeof s->fix_text[i]);
        else theory_numeral(seq_key(q), sc, ss, deg, s->fix_text[i], sizeof s->fix_text[i]);
    }
    for (int pc = 0; pc < 12; pc++) {   /* the note name, then its scale degree 1..7 when in the scale */
        int deg = 0;
        for (int d = 0; d < 7 && !deg; d++)
            if (theory_degree_pc(seq_key(q), sc, d) == pc) deg = d + 1;
        s->pcdeg[pc] = (unsigned char)deg;
        if (deg) snprintf(s->bnk_text[pc], sizeof s->bnk_text[pc], "%s %d", theory_key_name(pc), deg);
        else put(s->bnk_text[pc], (int)sizeof s->bnk_text[pc], theory_key_name(pc));
    }
    /* Pentatonic's Home note: "E (minor)", "G (major)", the anchors melody.c plays (a major-third scale's minor one is degree 6) */
    int major = pmod(theory_degree_pc(seq_key(q), sc, 2) - seq_key(q), 12) == 4;
    snprintf(s->pent_text[0], sizeof s->pent_text[0], "%s (minor)", theory_key_name(theory_degree_pc(seq_key(q), sc, major ? 5 : 0)));
    snprintf(s->pent_text[1], sizeof s->pent_text[1], "%s (major)", theory_key_name(theory_degree_pc(seq_key(q), sc, major ? 0 : 2)));
    for (int i = 0; i < THEORY_NPRESETS && i < 16; i++) {
        prog_text(seq_key(q), sc, i, BPR_CHARS, s->bpr_text[i], sizeof s->bpr_text[i]);
        prog_text(seq_key(q), sc, i, PROG_CHARS, s->cpr_text[i], sizeof s->cpr_text[i]);
    }
    pedal_labels(s);
    s->fix_key = seq_key(q);
    s->fix_scale = sc;
    s->fix_names = names;
}

/* 0.18 the RHYTHM page's text of one bar's chords: its chords' triad names joined by a space ("Em C"; at 2-bar chords both bars
 * read the chord), as the CHORDS strip names them. 0.24 final review: the chords as the bar plays them, one per chord change
 * (seq_part_bar_changes, PHRASE's centre's too: a change point's chord, no repeat of a slot boundary that plays the same chord), a
 * triad name equal to the one before (a type changed) once; only whole names that fit (rbar_n counts every one, for "+N"). Made with
 * the slot labels (slot_labels: once on a set that changes them), the getters only copy */
#define RBAR_CHARS 23   /* 0.24 final review: 0.23's longest (4 triads of 5 at 1/4-bar chords, "A#dim A#dim A#dim A#dim"), which every
                        * reader of rbar_text was fitted to (tools/textfit.py); a bar of more changes ends "+N" within it */
static void rbar_chords(inst_t *s, int part, int bar) {
    txt_t t = {s->rbar_text[part][bar], sizeof s->rbar_text[part][bar], 0};
    int slots[SEQ_STEPS], n = seq_part_bar_changes(&s->seq, part, bar, slots, NULL), m = 0, shown = 0;
    const char *w[SEQ_STEPS];
    t.b[0] = 0;
    for (int j = 0; j < n; j++)
        if (!m || strcmp(s->tri_text[part][slots[j]], w[m - 1])) w[m++] = s->tri_text[part][slots[j]];
    for (int j = 0, len = 0; j < m; j++) {   /* whole names, leaving room for "+N" when one is left out */
        int need = len + (j ? 1 : 0) + (int)strlen(w[j]) + (j + 1 < m ? 2 : 0);
        if (need > RBAR_CHARS) break;
        len += (j ? 1 : 0) + (int)strlen(w[j]), shown++;
    }
    for (int j = 0; j < shown; j++) {
        if (j) add(&t, " ");
        add(&t, w[j]);
    }
    if (shown < m) add(&t, "+"), add_int(&t, m - shown);
    s->rbar_n[part][bar] = (unsigned char)m;
}

/* 0.18 one part's bar chords (the RHYTHM page's, PHRASE's now), made with its slot labels (0.24.1 FIX by bar's texts are gone) */
static void fix_bar_labels(inst_t *s, int part) {
    for (int b = 0; b < s->seq.part[part].bars; b++) rbar_chords(s, part, b);
}

/* 0.17 the CHORDS page's texts: where a slot sounds, "6" (1-bar chords), "6.3" (shorter: bar.beat), "7-8" (2-bar chords on a
 * loop of 2 bars or more) */
static void slot_bar(int cl, int bars, int i, txt_t *t) {
    int start = i * cl, bar = start / SEQ_STEPS + 1;
    add_int(t, bar);
    if (cl > SEQ_STEPS && bars > 1) add(t, "-"), add_int(t, bar + 1);
    else if (cl < SEQ_STEPS) add(t, "."), add_int(t, start % SEQ_STEPS / 4 + 1);
}

/* A strip cell's text at most this many characters (vst/make_skin.py's strip cells, 150 px of 24 px text) */
#define CST_CHARS 13
/* 0.24 (ruling 23) the one drift-word scale, PHRASE's 5 colours and the sound pages' words: theory_drift_level's close (< 16) split at 8
 * (triads; 7ths, sus, add9, 6ths) into close and a little colour */
static const char *const DRIFT_WORD[5] = {"close", "a little colour", "colourful", "tense", "dissonant"};
static int drift5(int score) { return score < 8 ? 0 : score < 16 ? 1 : score < 40 ? 2 : score < 65 ? 3 : 4; }
static const int DRIFT_TINT[4] = {5, 0, 2, 1};   /* the toolchain's tint=1 gels: green, yellow, orange, red */

/* One strip cell: "\xC2\xB7 6* Gmaj7 III" (\xC2\xB7 locked, * not the phrase's chord), then without the numeral, then the
 * chord alone, until it fits CST_CHARS */
static void cst_text(char *buf, int len, const char *where, int own, int locked, const char *name, const char *num) {
    for (int form = 0; form < 3; form++) {
        txt_t t = {buf, len, 0};
        if (locked) add(&t, "\xC2\xB7 ");
        if (form < 2) {
            add(&t, where);
            if (own) add(&t, "*");
            add(&t, " ");
        }
        add(&t, name);
        if (!form) add(&t, " "), add(&t, num);
        if (theory_text_width(buf) <= CST_CHARS) return;
    }
}

/* 0.17 one part's CHORDS page texts (strip cells, the centre's numeral and notes) and each slot's drift, made again only when
 * what they read changed. A slot is not the phrase's chord (*) when it differs from the slot rank_len before it. 0.18: from the
 * colours and SPREAD as they play (COMPLEXITY's layers), so a COMPLEXITY turn that moves them recolours the strip */
static void page_labels(inst_t *s, int part, const slot_src_t *slots) {
    const seq_t *q = &s->seq;
    const seq_part_t *p = &q->part[part];
    page_src_t in;
    memset(&in, 0, sizeof in);
    in.slots = *slots, in.vinv = p->vinv, in.vspr = seq_part_vknob_eff(q, part, VK_SPR), in.vmode = p->vmode, in.rank = p->rank_len;
    if (!memcmp(&in, &s->page_src[part], sizeof in)) return;
    s->page_src[part] = in;
    for (int i = 0; i < p->prog_len; i++) {
        int ss = theory_slot_scale(q->scale, p->src[i]), c = slots->ecol[i], r = p->rank_len > 0 ? i % p->rank_len : i;
        int notes[THEORY_CHORD_MAX], n = theory_chord_notes(q->key, ss, p->prog[i], c, notes);
        int own = r != i && (p->prog[i] != p->prog[r] || p->src[i] != p->src[r] || p->colour[i] != p->colour[r]);
        char name[16], num[12], where[8];
        txt_t t = {where, sizeof where, 0};
        s->drift[part][i] = (unsigned char)theory_drift(q->key, q->scale, ss, p->prog[i], c, in.vspr < 0, p->vinv);   /* as it plays */
        theory_chord_name_c(q->key, ss, p->prog[i], c, name, sizeof name);
        theory_numeral(q->key, q->scale, ss, p->prog[i], num, sizeof num);
        slot_bar(p->chord_len, p->bars, i, &t);
        cst_text(s->cst_text[part][i], sizeof s->cst_text[part][i], where, own, (int)(p->slot_lock >> i & 1), name, num);
        t = (txt_t){s->ccd_text[part][i], sizeof s->ccd_text[part][i], 0};
        add(&t, num);
        add(&t, " \xC2\xB7");
        for (int k = 0; k < n; k++) {   /* the chord's notes from its root; Cluster's 2nd after the root */
            add(&t, " ");
            add(&t, theory_key_name(notes[k] % 12));
            if (!k && in.vspr < 0) {
                int pc = theory_degree_pc(q->key, ss, p->prog[i] + 1), dup = 0;
                for (int j = 0; j < n; j++) dup |= notes[j] % 12 == pc;
                if (!dup) add(&t, " "), add(&t, theory_key_name(pc));   /* a sus2's 2nd is there already */
            }
        }
    }
}

/* 0.12 one part's fs_ labels, both variants, and its slots' triad names, made again only when what they read changed; 0.17 then
 * its CHORDS page texts */
static void slot_labels(inst_t *s, int part) {
    const seq_t *q = &s->seq;
    const seq_part_t *p = &q->part[part];
    slot_src_t in;
    memset(&in, 0, sizeof in);
    in.names = seq_names(q), in.key = q->key, in.scale = q->scale, in.prog_len = p->prog_len, in.chord_len = p->chord_len;
    in.lock = p->slot_lock, in.vcol = p->vcol, in.hseed = p->harmony_seed;
    in.bars = p->bars, in.phrase = p->phrase;
    memcpy(in.prog, p->prog, sizeof in.prog);
    memcpy(in.src, p->src, sizeof in.src);
    memcpy(in.colour, p->colour, sizeof in.colour);
    memcpy(in.cp, p->cp, sizeof in.cp);
    for (int i = 0; i < p->prog_len; i++) in.ecol[i] = (unsigned char)seq_part_eff_colour(q, part, i);
    page_labels(s, part, &in);
    if (!memcmp(&in, &s->slot_src[part], sizeof in)) return;
    s->slot_src[part] = in;
    for (int i = 0; i < SEQ_SLOTS_ALL; i++)
        theory_chord_name(q->key, theory_slot_scale(q->scale, p->src[i]), p->prog[i], s->tri_text[part][i], sizeof s->tri_text[part][i]);
    for (int i = 0; i < 8; i++) {
        char *plain = s->fs_text[part][0][i], chord[16];
        int ss = theory_slot_scale(q->scale, p->src[i]), c = in.ecol[i];   /* 0 (Triad) past prog_len, unused */
        txt_t t = {plain, sizeof s->fs_text[0][0][0], 0};
        if (i >= p->prog_len) {
            put(plain, t.len, " ");
            put(s->fs_text[part][1][i], t.len, " ");
            continue;
        }
        if (in.names) theory_chord_name_c(q->key, ss, p->prog[i], c, chord, sizeof chord);
        else theory_degree_c(q->key, q->scale, ss, p->prog[i], c, chord, sizeof chord);
        slot_where(p->chord_len, i, &t);
        add(&t, "  ");
        add(&t, chord);
        if (p->slot_lock >> i & 1) add(&t, "  *");
        t = (txt_t){s->fs_text[part][1][i], t.len, 0};
        add(&t, "> ");
        add(&t, plain);
    }
    fix_bar_labels(s, part);
}
/* fc_, fb_ and fo_ (retired: never lit) the 7 scale chords, the 3 borrows (fix_text, as cqk_ and cqr_) and the colours */
static int fc_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->fix_text[i]); }
static int fb_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->fix_text[7 + i]); }
static int fo_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, THEORY_COLOUR_NAME[i]); }
static int dn_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "DONE"); }

/* 0.9 SHAPE: rh_ the Chords rhythm (style_1 Sustain, Pulse, Stabs; none lit on Manual), vo_ the voicing, oc_ the octave.
 * 0.12: crh_ the nine rhythms (style_1's options 1..9, short names for the larger text) on the same functions */
static const char *const RHYTHM_NAMES[STYLE_NOPTS - 1] = {"Sustain", "Pulse", "Stabs", "Offbeat", "Charlstn", "Ballad", "Funk", "Half", "Push"};
static const char *const VOICING_NAMES[2] = {"Close", "Smooth"};
static const char *const OCTAVE_NAMES[3] = {"-1", "0", "+1"};
static int rh_on(const seq_t *q, int i) { return seq_style(q, LANE_CHORDS) == STYLE_IDS[LANE_CHORDS][i + 1]; }
static int rh_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, RHYTHM_NAMES[i]); }
static int vo_on(const seq_t *q, int i) { return seq_voicing(q) == i; }
static int vo_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, VOICING_NAMES[i]); }
static int oc_on(const seq_t *q, int i) { return seq_chord_shape(q, SHAPE_OCT) == i - 1; }
static int oc_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, OCTAVE_NAMES[i]); }
/* 0.24 cpo_1..3 the CHORDS page's OCTAVE -1, 0, +1 (lit = the part's Chords lane octave): a tap sets it, one undo step (seq_set_chord_octave;
 * off the style's the voicing reads (edited), RESET KNOBS puts it back) */
static int cpo_on(const seq_t *q, int i) { return seq_chord_shape(q, SHAPE_OCT) == i - 1; }
static void cpo_tap(seq_t *q, int i, int v) { (void)v; seq_set_chord_octave(q, i - 1); }
static int cpo_label(const seq_t *q, int i, char *buf, int len) { return oc_label(q, i, buf, len); }
/* 0.12 CHORDS: cnm_ NAMES (a toggle), cnp_ NEW PROGRESSION and cnr_ NEW RHYTHM (actions, one undo step each) */
static int cnm_on(const seq_t *q, int i) { (void)i; return seq_names(q); }
static void cnm_tap(seq_t *q, int i, int v) { (void)i; seq_set_names(q, v); }
static int cnm_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "NAMES"); }
/* 0.24 gnm_1..2 GLOBAL's CHORDS SHOW: DEGREES, NAMES (the setting behind cnm_1, which still answers, unpolled: lit = it; not saved) */
static const char *const GNM_NAMES[2] = {"DEGREES", "NAMES"};
static int gnm_on(const seq_t *q, int i) { return seq_names(q) == i; }
static void gnm_tap(seq_t *q, int i, int v) { (void)v; seq_set_names(q, i); }
static int gnm_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, GNM_NAMES[i]); }
static int cnp_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "NEW PROGRESSION"); }
static int cnr_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "NEW RHYTHM"); }

/* 0.17 the CHORDS page. cvm_ the mode cells (CHORDS lit for Pick and the browsers too). 0.24 the three-page model: cvm_4 JAM, cvm_1 CHORDS,
 * cvm_2 PHRASE (the RHYTHM views); cvm_3 FIX retired (unpolled; a tap does nothing, set_mode) */
static const char *const CVM_NAMES[4] = {"CHORDS", "PHRASE", "FIX", "JAM"};   /* 0.22 cvm_4 JAM */
static int cvm_on(const seq_t *q, int i) { return get_mode(q) == i; }
static void cvm_tap(seq_t *q, int i, int v) { (void)v; set_mode(q, i); }
static int cvm_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, CVM_NAMES[i]); }
static int arrow_label(int right, char *buf, int len) { return put(buf, len, right ? ">" : "<"); }
static void no_tap(seq_t *q, int i, int v) { (void)q; (void)i; (void)v; }   /* a display-only cell */
static int blank_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, " "); }

/* An open picker or browser is kept (DONE) before another opens or a field steps: what was picked stays */
static void close_txn(seq_t *q) { seq_txn_done(q); }   /* none open: nothing */

/* 0.18 the RHYTHM page; 0.19 one per drawn lane: the CHORDS tab's (r... cells) and the BASS tab's BASS RHYTHM page (y... cells). Its
 * functions (rp_*) take the page's row of RPAGE: the lane, its tab, rhythm library and views; the cells are thin wrappers (RP_CELL) */
typedef struct {
    int dl, lane, tab, lib, notes;   /* dl: the page's inst_t.rpaint and rmsg; notes: the step row names each hit's note (BASS RHYTHM) */
    int (*view)(const seq_t *);      /* the tab's view (seq_cview, seq_bview) */
    int (*paged)(const seq_t *);     /* 1 on its RHYTHM views (the step row and the strip's pictures on show) */
    int bar, copy, rbrowse;          /* its views: a bar open, its COPY TO..., the RHYTHMS browser */
} rpage_t;
static int chords_paged(const seq_t *q) { return seq_strip_by_bars(q, TAB_CHORDS); }
static int bass_paged(const seq_t *q) { return seq_bmode(q) == 1; }   /* 0.22: not on JAM (seq_bmode 2) */
static const rpage_t RPAGE[DL_COUNT] = {
    {DL_CHORDS, LANE_CHORDS, TAB_CHORDS, RLIB_CHORDS, 0, seq_cview, chords_paged, CVIEW_BAR, CVIEW_COPY, CVIEW_RBROWSE},
    {DL_BASS, LANE_BASS, TAB_BASS, RLIB_BASS, 1, seq_bview, bass_paged, BVIEW_BAR, BVIEW_COPY, BVIEW_RBROWSE}};
#define RP_C (&RPAGE[DL_CHORDS])
#define RP_B (&RPAGE[DL_BASS])

/* The page's bars. The strip page's first bar (on the RHYTHM views a page is 4 bars, else 4 chord slots); the bar on show: the open
 * bar; on the RHYTHM views with FOLLOW running and playing the playing bar (it changes once a bar), else the page's first; off them
 * bar 1 (the step row is not on show: its polled cells stay still while another page plays) */
static int rp_first(const seq_t *q, const rpage_t *P) {
    int page = seq_strip_page(q, P->tab);
    return seq_strip_by_bars(q, P->tab) ? page * 4 : page * 4 * seq_chord_len(q) / SEQ_STEPS;
}
static int rp_live(const seq_t *q, const rpage_t *P) { return seq_follow(q) && !seq_follow_paused(q, P->tab) && seq_cur_step(q) >= 0; }
static int rp_shown(const seq_t *q, const rpage_t *P) {
    int bar = seq_rbar_bar(q, P->lane);
    if (bar >= 0 || !P->paged(q)) return bar >= 0 ? bar : 0;
    return rp_live(q, P) ? seq_cur_bar(q) : clampi(rp_first(q, P), 0, seq_bars(q) - 1);
}
/* A bar FILL plays on and the strip tags (every 4th bar of a loop of 4 bars or more, not locked) */
static int rp_fill_tag(const seq_t *q, const rpage_t *P, int bar) {
    return seq_rknob(q, P->lane, RK_FILL) && seq_bars(q) >= 4 && bar % 4 == 3 && !seq_bar_locked(q, P->lane, bar);
}
static void rp_out(const seq_t *q, const rpage_t *P, int bar, unsigned char out[16]) {   /* the bar as it plays: a fill bar's fill */
    if (rp_fill_tag(q, P, bar)) memcpy(out, q->part[seq_part(q)].rfill[P->dl][bar], 16);
    else seq_bar_plays(q, P->lane, bar, out);
}
static int rp_open(const seq_t *q, const rpage_t *P) {   /* a bar or the browser */
    return seq_rbar_bar(q, P->lane) >= 0 || P->view(q) == P->rbrowse;
}
/* 0.19.1 a strip cell's light, every strip's: the open bar (or the picker's slot), cell `at`, is lit in place of the playing one */
static int strip_lit(const seq_t *q, int open, int playing, int at) { return open >= 0 ? at == open : seq_cur_step(q) >= 0 && at == playing; }
/* The strip's bars on the RHYTHM views (rp_cell; lit while it plays, 0.19.1 the open bar while one is open; a tap opens the bar, inert
 * while a bar or the browser is open); blank, unlit and inert past the loop and off them */
static int rp_strip_on(const seq_t *q, const rpage_t *P, int i) {
    return P->paged(q) && strip_lit(q, seq_rbar_bar(q, P->lane), seq_cur_bar(q), rp_first(q, P) + i);
}
static int rp_strip_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len);

/* cst_ the strip page's 4 chords (blank and inert past the loop; lit while it plays; select 0: the playing one's tap opens the
 * picker too), their tints, the drift of a slot of the playing part (page_labels). 0.18: on the RHYTHM views the page's 4 bars
 * (rp_strip_*; tinted by its first chord) */
static int strip_slot(const seq_t *q, int i) {   /* the cell's chord slot (on the RHYTHM views its bar's first; past the loop none) */
    int bar;
    if (!seq_strip_by_bars(q, TAB_CHORDS)) return seq_strip_page(q, TAB_CHORDS) * 4 + i;
    bar = rp_first(q, RP_C) + i;
    return bar < seq_bars(q) ? bar * SEQ_STEPS / seq_chord_len(q) : SEQ_MAX_PROG;
}
static int slot_level(const seq_t *q, int slot) { return theory_drift_level(inst_of(q)->drift[seq_part(q)][slot]); }
static int tint_of(const seq_t *q, int i) { int slot = strip_slot(q, i); return DRIFT_TINT[slot < seq_slots(q) ? slot_level(q, slot) : 0]; }
static int cst_on(const seq_t *q, int i) {
    if (!seq_strip_by_bars(q, TAB_CHORDS)) return strip_lit(q, -1, seq_cur_slot(q), strip_slot(q, i));
    return rp_strip_on(q, RP_C, i);
}
static int cst_label(const seq_t *q, int i, char *buf, int len) {
    int slot = strip_slot(q, i);
    if (seq_strip_by_bars(q, TAB_CHORDS)) return rp_strip_label(q, RP_C, i, buf, len);
    return put(buf, len, slot < seq_slots(q) ? inst_of(q)->cst_text[seq_part(q)][slot] : " ");
}
/* cpg_, cund_, cbtp_, crk_, cct_: the strip chrome both tabs share (tb_*, after the BASS page) */

/* cpr_ PROGRESSION: <, the field (the preset the unlocked slots show; tapped, the PROGRESSIONS browser), >; the arrows step the 16
 * presets (from Custom: the first or the last), as a try while the browser is open. cvo_ VOICING, the same over the 52 styles */
static int step_of(int cur, int n, int right) { return cur < 0 ? (right ? 0 : n - 1) : (cur + (right ? 1 : n - 1)) % n; }
static void cpr_tap(seq_t *q, int i, int v) {
    int next = step_of(seq_prog_preset_shown(q), THEORY_NPRESETS, i == 2);
    (void)v;
    if (seq_cview(q) == CVIEW_PROG) {
        if (i != 1) seq_prog_try(q, next);
        return;
    }
    close_txn(q);
    if (i == 1) seq_browse_open(q, CVIEW_PROG);
    else seq_prog_step(q, next);   /* ruling 23: the locks stay */
}
static int cpr_label(const seq_t *q, int i, char *buf, int len) {
    int k = inst_of(q)->shown[seq_part(q)];
    if (i != 1) return arrow_label(i == 2, buf, len);
    return put(buf, len, k < 0 ? "Custom" : inst_of(q)->cpr_text[k]);
}
#define STYLES_PER_GENRE 4   /* voicing.h: genre-major, 4 a genre */
static void cvo_tap(seq_t *q, int i, int v) {
    int next = step_of(seq_voicing_style(q), VOICING_NSTYLES, i == 2);
    (void)v;
    if (seq_cview(q) == CVIEW_VOICE) {
        if (i != 1) seq_set_browse_genre(q, next / STYLES_PER_GENRE), seq_voice_try(q, next);
        return;
    }
    close_txn(q);
    if (i == 1) seq_browse_open(q, CVIEW_VOICE);
    else seq_set_voicing_style(q, next);
}
static int cvo_label(const seq_t *q, int i, char *buf, int len) {
    return i != 1 ? arrow_label(i == 2, buf, len) : put(buf, len, inst_of(q)->cvo_text[seq_part(q)]);
}

/* 0.18 the RHYTHM page (header comment, 0.18; 0.19 per page, rpage_t). The message line: what the page's last action did, made on
 * the UI thread */
static txt_t rp_msg_new(seq_t *q, const rpage_t *P) {
    inst_t *s = inst_rw(q);
    s->rmsg[P->dl][0] = 0;
    s->msg_at[P->dl] = s->frames;
    return (txt_t){s->rmsg[P->dl], sizeof s->rmsg[P->dl], 0};
}
static void rp_msg_bar(seq_t *q, const rpage_t *P, const char *a, int bar, const char *b) {   /* a, bar's number, b */
    txt_t m = rp_msg_new(q, P);
    add(&m, a), add_int(&m, bar + 1), add(&m, b);
}

/* Opens a bar (another picker or browser is closed as DONE first), saying when it is heard */
static void rp_open_at(seq_t *q, const rpage_t *P, int bar) {
    seq_rbar_open(q, P->lane, bar);
    if (seq_rbar_bar(q, P->lane) != bar) return;
    if (seq_cur_step(q) < 0) rp_msg_bar(q, P, "Stopped: bar ", bar, " plays when the playhead gets there");
    else if (seq_follow(q)) rp_msg_bar(q, P, "FOLLOW paused: you hear bar ", bar, " when it plays");
    else rp_msg_bar(q, P, "You hear bar ", bar, " when the playhead gets there");
}

static void add_note(txt_t *t, int midi);
/* BASS's (P->notes): the note hit i plays, 0 none; polled, so one read of what the setters rendered (the lane's steps, rfill_abs) */
static int rp_note(const seq_t *q, const rpage_t *P, int bar, int i) {
    const seq_part_t *p = &q->part[seq_part(q)];
    const seq_step_t *x = &p->lane[P->lane].steps[bar * SEQ_STEPS + i];
    return rp_fill_tag(q, P, bar) ? p->rfill_abs[P->dl][bar][i] : seq_abs_note(x);
}
/* "Copied to 14 bars \xC2\xB7 3 changed \xC2\xB7 1 locked skipped": a copy of the open bar to the bars of mask, said before it copies */
static void rp_copy_msg(seq_t *q, const rpage_t *P, unsigned mask) {
    int rep = 0, skip = 0, n = seq_rbar_copy_counts(q, P->lane, mask, &rep, &skip);
    txt_t m = rp_msg_new(q, P);
    if (!n) {
        add(&m, seq_bars(q) == 1 ? "No other bar in a 1-bar loop: nothing copied" : "Every other bar is locked: nothing copied");
        return;
    }
    add(&m, "Copied to "), add_int(&m, n), add(&m, n == 1 ? " bar" : " bars");
    if (rep) add(&m, " \xC2\xB7 "), add_int(&m, rep), add(&m, " changed");
    if (skip) add(&m, " \xC2\xB7 "), add_int(&m, skip), add(&m, " locked skipped");
}
static int rp_unlocked(const seq_t *q, const rpage_t *P) {
    int n = 0;
    for (int b = 0; b < seq_bars(q); b++) n += !seq_bar_locked(q, P->lane, b);
    return n;
}
/* The message after a RHYTHM field pick of ri in the open bar `bar`, else in every unlocked bar: "Funk in bar 6: 16th syncopation,
 * short", "Funk in 15 bars: ..."; one that changed nothing says so ("Bar 6 is locked: nothing changed") */
static void rp_pick_msg(seq_t *q, const rpage_t *P, int bar, int ri, int changed) {
    int n = bar >= 0 ? bar + 1 : rp_unlocked(q, P);
    txt_t m = rp_msg_new(q, P);
    if (changed) {
        add(&m, rhythm_lib_name(P->lib, ri)), add(&m, bar >= 0 ? " in bar " : " in "), add_int(&m, n);
        if (bar < 0) add(&m, n == 1 ? " bar" : " bars");
        add(&m, ": "), add(&m, rhythm_lib_feel(P->lib, ri));
    } else if (bar >= 0 && seq_bar_locked(q, P->lane, bar)) {
        add(&m, "Bar "), add_int(&m, n), add(&m, " is locked: nothing changed");
    } else if (!n) {
        add(&m, "Every bar is locked: nothing changed");
    } else {
        add(&m, "Nothing changed: ");
        if (bar >= 0) add(&m, "bar "), add_int(&m, n), add(&m, " has "), add(&m, rhythm_lib_name(P->lib, ri)), add(&m, " already");
        else add(&m, "every unlocked bar has "), add(&m, rhythm_lib_name(P->lib, ri));
    }
}
/* csr_ the strip page's bars as they play: a picture "A...x...x...x..." (A accent, x hit, g ghost, - hold, . rest); blank past the
 * loop and off the RHYTHM views. Display only */
static void rp_picture(const seq_t *q, const rpage_t *P, int bar, char pic[17]) {   /* the bar as it plays */
    unsigned char o[16];
    rp_out(q, P, bar, o);
    rhythm_format(o, pic);
}
static void rp_no_tap(seq_t *q, const rpage_t *P, int i, int v) { (void)P; no_tap(q, i, v); }
static void first_chord(const char *ch, char *first) {   /* a bar's chords' first, into first (as long as ch); a getter: no snprintf */
    size_t k = strcspn(ch, " ");
    memcpy(first, ch, k);
    first[k] = 0;
}
/* A RHYTHM view's strip cell: "BAR 6 \xC2\xB7 LOCKED \xC2\xB7 Em C" (or " \xC2\xB7 fill"); past CST_CHARS the bar's chords as the first and
 * "+N", then without "BAR ", then without the chords */
static int rp_cell(const seq_t *q, const rpage_t *P, int bar, char *buf, int len) {
    const inst_t *s = inst_of(q);
    const char *flag = seq_bar_locked(q, P->lane, bar) ? "LOCKED" : rp_fill_tag(q, P, bar) ? "fill" : NULL, *ch = s->rbar_text[seq_part(q)][bar];
    int n = s->rbar_n[seq_part(q)][bar];
    char first[sizeof s->rbar_text[0][0]];
    first_chord(ch, first);
    for (int form = 0; form < 6; form++) {   /* BAR + chords, BAR + first+N, chords, first+N, BAR alone, the number alone */
        txt_t t = {buf, len, 0};
        if ((form == 1 || form == 3) && n < 2) continue;
        if (form == 0 || form == 1 || form == 4) add(&t, "BAR ");
        add_int(&t, bar + 1);
        if (flag) add(&t, " \xC2\xB7 "), add(&t, flag);
        if (form < 4) add(&t, " \xC2\xB7 "), add(&t, form == 1 || form == 3 ? first : ch);
        if (form == 1 || form == 3) add(&t, "+"), add_int(&t, n - 1);
        if (theory_text_width(buf) <= CST_CHARS || form == 5) return t.n;
    }
    return 0;
}
static int rp_strip_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    int bar = rp_first(q, P) + i;
    return P->paged(q) && bar < seq_bars(q) ? rp_cell(q, P, bar, buf, len) : put(buf, len, " ");
}

/* rcb_ COPY TO...'s bars of the loop (blank and inert elsewhere): "BAR 6 \xC2\xB7 FROM", " \xC2\xB7 LOCKED" (both inert), " \xC2\xB7 COPY" (lit:
 * picked); a tap toggles */
static int rp_cb_live(const seq_t *q, const rpage_t *P, int i) { return P->view(q) == P->copy && i < seq_bars(q); }
static int rp_cb_on(const seq_t *q, const rpage_t *P, int i) { return rp_cb_live(q, P, i) && (seq_rcopy_mask(q, P->lane) >> i & 1); }
static void rp_cb_tap(seq_t *q, const rpage_t *P, int i, int v) {
    if (rp_cb_live(q, P, i) && (v > 0) != rp_cb_on(q, P, i)) seq_rcopy_toggle(q, P->lane, i);
}
/* rcp_ (ruling R31) each COPY TO... bar's chords and rhythm as it plays, an inner cell of its rcb_ cell: "Em C · x---x---x---x---",
 * "Em+1 x---..." past RCP_CHARS; blank on a locked bar (lit: the skin's dark well) and outside COPY TO...; a tap picks the bar as
 * rcb_ does (copy_targets skips the source and locked bars) */
#define RCP_CHARS 24
static void rp_cp_tap(seq_t *q, const rpage_t *P, int i, int v) { (void)v; if (rp_cb_live(q, P, i)) seq_rcopy_toggle(q, P->lane, i); }
/* rcx_ ALL, NONE, CANCEL, "COPY TO 3 BARS" ("PICK BARS", inert, with none picked); blank and inert outside COPY TO... */
static int rp_picked(const seq_t *q, const rpage_t *P) {
    int n = 0;
    for (unsigned m = seq_rcopy_mask(q, P->lane); m; m &= m - 1) n++;
    return n;
}
static void rp_cx_tap(seq_t *q, const rpage_t *P, int i, int v) {
    unsigned mask = seq_rcopy_mask(q, P->lane);
    (void)v;
    if (P->view(q) != P->copy) return;
    if (i < 2) {
        seq_rcopy_set_all(q, P->lane, i == 0);
    } else if (i == 2) {
        txt_t m = rp_msg_new(q, P);
        seq_rcopy_close(q, P->lane);
        add(&m, "Nothing copied");
    } else if (mask) {
        rp_copy_msg(q, P, mask);
        seq_rcopy_go(q, P->lane);
    }
}
static int rp_cx_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    static const char *const NAME[3] = {"ALL", "NONE", "CANCEL"};
    int n = rp_picked(q, P);
    txt_t t = {buf, len, 0};
    if (P->view(q) != P->copy) return put(buf, len, " ");
    if (i < 3) return put(buf, len, NAME[i]);
    if (!n) return put(buf, len, "PICK BARS");
    add(&t, "COPY TO "), add_int(&t, n), add(&t, n == 1 ? " BAR" : " BARS");
    return t.n;
}

/* The RHYTHMS browser: rbg_ the 6 feel chips (a tap tries the feel's first rhythm), rbt_ the feel's 4 rhythms ("Funk \xC2\xB7 6 hits";
 * lit = the shown bar's, unedited; a tap tries it), rbp_ and rbf_ their pictures and feels (display only) */
static int rp_tile(const seq_t *q, const rpage_t *P, int i) {   /* rhythm.h: 4 a feel, in library order */
    return seq_rbrowse_group(q, P->lane) * 4 + i;
}
static int rp_feel_on(const seq_t *q, const rpage_t *P, int i) { return seq_rbrowse_group(q, P->lane) == i; }
static void rp_feel_tap(seq_t *q, const rpage_t *P, int i, int v) {
    (void)v;
    if (P->view(q) == P->rbrowse) seq_rbrowse_set_group(q, P->lane, i);
}
static int rp_feel_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    (void)q;
    return put(buf, len, rhythm_lib_group_name(P->lib, i));
}
static int rp_tile_on(const seq_t *q, const rpage_t *P, int i) {
    int ed, ri = seq_bar_rhythm(q, P->lane, rp_shown(q, P), &ed);
    return P->view(q) == P->rbrowse && !ed && ri == rp_tile(q, P, i);
}
static void rp_tile_tap(seq_t *q, const rpage_t *P, int i, int v) { (void)v; seq_rbrowse_try(q, P->lane, rp_tile(q, P, i)); }
static int rp_tile_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    unsigned char c[16];
    int ri = rp_tile(q, P, i), n = 0;
    txt_t t = {buf, len, 0};
    rhythm_lib_codes(P->lib, ri, c);
    for (int k = 0; k < 16; k++) n += rhythm_is_hit(c[k]);
    add(&t, rhythm_lib_name(P->lib, ri)), add(&t, " \xC2\xB7 "), add_int(&t, n), add(&t, n == 1 ? " hit" : " hits");
    return t.n;
}
static int rp_tile_pic(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    unsigned char c[16];
    char pic[17];
    rhythm_lib_codes(P->lib, rp_tile(q, P, i), c);
    rhythm_format(c, pic);
    return put(buf, len, pic);
}
static int rp_tile_feel(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    return put(buf, len, rhythm_lib_feel(P->lib, rp_tile(q, P, i)));
}

/* The cells of both pages, r... the CHORDS tab's (0.18) and y... the BASS tab's (0.19), each its page function on its row of RPAGE */
#define RP_ON(r, y, fn) \
    static int r##_on(const seq_t *q, int i) { return fn(q, RP_C, i); } \
    static int y##_on(const seq_t *q, int i) { return fn(q, RP_B, i); }
#define RP_TAP(r, y, fn) \
    static void r##_tap(seq_t *q, int i, int v) { fn(q, RP_C, i, v); } \
    static void y##_tap(seq_t *q, int i, int v) { fn(q, RP_B, i, v); }
#define RP_LABEL(r, y, fn) \
    static int r##_label(const seq_t *q, int i, char *buf, int len) { return fn(q, RP_C, i, buf, len); } \
    static int y##_label(const seq_t *q, int i, char *buf, int len) { return fn(q, RP_B, i, buf, len); }
#define RP_CELL(r, y, on, tap, label) RP_ON(r, y, on) RP_TAP(r, y, tap) RP_LABEL(r, y, label)
RP_LABEL(rcx, ycx, rp_cx_label)   /* 0.24.1 retired (TAPR): its label as PHRASE's ALL, NONE, CANCEL, COPY TO (ph_x) */
RP_CELL(rbg, ybg, rp_feel_on, rp_feel_tap, rp_feel_label)
RP_CELL(rbt, ybt, rp_tile_on, rp_tile_tap, rp_tile_label)
RP_TAP(rbp, ybp, rp_no_tap) RP_LABEL(rbp, ybp, rp_tile_pic)
RP_TAP(rbf, ybf, rp_no_tap) RP_LABEL(rbf, ybf, rp_tile_feel)

/* The chord picker, retired (0.24.1 seq_pick_* left the code; fc_ and fb_ were its CHORD and BORROW rows): pty_ TYPE (never lit),
 * plk_ LOCK, pcn_ CANCEL, pdn_ DONE */
static const char *const PTY_NAMES[5] = {"Knobs", "sus2", "sus4", "add9", "6"};
static int pty_of(int c) { return c == COL_SUS2 ? 1 : c == COL_SUS4 ? 2 : c == COL_ADD9 ? 3 : c == COL_6TH ? 4 : 0; }
static int pty_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, PTY_NAMES[i]); }
static int plk_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "LOCK"); }
static int cancel_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "CANCEL"); }
static int pcn_label(const seq_t *q, int i, char *buf, int len) { return cancel_label(q, i, buf, len); }
static int pdn_label(const seq_t *q, int i, char *buf, int len) { return dn_label(q, i, buf, len); }

/* The browsers: bpr_ the PROGRESSIONS tiles row by row, Pop, Moody, Classic, Loops (theory.c's preset indices, the spec's
 * table; lit = the preset shown), bgn_ the VOICING genres, bvs_ the genre's styles (lit = the part's), bvf_ their feel
 * (display only), bcn_ CANCEL, bdo_ DONE */
static const int BPR_PRESET[16] = {1, 3, 9, 8, 0, 2, 6, 11, 4, 5, 7, 10, 15, 12, 13, 14};
static int bpr_on(const seq_t *q, int i) { return seq_cview(q) == CVIEW_PROG && inst_of(q)->shown[seq_part(q)] == BPR_PRESET[i]; }
static void bpr_tap(seq_t *q, int i, int v) { (void)v; seq_prog_try(q, BPR_PRESET[i]); }
static int bpr_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->bpr_text[BPR_PRESET[i]]); }
/* bgn_14 and bgn_15 (Task 9) fill the skin's 5-a-row genre list: blank and inert */
static int bgn_on(const seq_t *q, int i) { return i < VOICING_NGENRES && seq_browse_genre(q) == i; }
/* 0.17.1 (owner): a genre chip tries its first style, unless the style shown is already one of that genre's */
static void bgn_tap(seq_t *q, int i, int v) {
    int st = seq_voicing_style(q);
    (void)v;
    if (i >= VOICING_NGENRES) return;
    seq_set_browse_genre(q, i);
    if (st < 0 || st / STYLES_PER_GENRE != i) seq_voice_try(q, i * STYLES_PER_GENRE);
}
static int bgn_label(const seq_t *q, int i, char *buf, int len) {
    return i < VOICING_NGENRES ? put(buf, len, VOICING_STYLES[i * STYLES_PER_GENRE].genre) : blank_label(q, i, buf, len);
}
static int bvs_style(const seq_t *q, int i) { return seq_browse_genre(q) * STYLES_PER_GENRE + i; }
static int bvs_on(const seq_t *q, int i) { return seq_cview(q) == CVIEW_VOICE && seq_voicing_style(q) == bvs_style(q, i); }
static void bvs_tap(seq_t *q, int i, int v) { (void)v; seq_voice_try(q, bvs_style(q, i)); }
static int bvs_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, VOICING_STYLES[bvs_style(q, i)].name); }
static void bvf_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int bvf_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, VOICING_STYLES[bvs_style(q, i)].feel); }
/* 0.18 the RHYTHMS browser's CANCEL (keep 0) or DONE, said on its page's message line (0.19: either page's: bcn_ / bdo_ close the
 * CHORDS tab's, ubc_ / ubd_ the BASS tab's) */
static void rp_browse_end(seq_t *q, const rpage_t *P, int keep) {
    int ri;
    txt_t m;
    if (keep) seq_browse_done(q);
    else seq_browse_cancel(q);
    ri = seq_bar_rhythm(q, P->lane, rp_shown(q, P), NULL);
    m = rp_msg_new(q, P);
    if (!keep) add(&m, "Cancelled: back as it was");
    else add(&m, "Kept "), add(&m, ri >= 0 ? rhythm_lib_name(P->lib, ri) : "Custom"), add(&m, ".");
}
static void bcn_tap(seq_t *q, int i, int v) {   /* 0.18: the RHYTHMS browser's says so on the message line */
    (void)i, (void)v;
    if (seq_cview(q) == CVIEW_RBROWSE) rp_browse_end(q, RP_C, 0);
    else seq_browse_cancel(q);
}
static int bcn_label(const seq_t *q, int i, char *buf, int len) { return cancel_label(q, i, buf, len); }
static void bdo_tap(seq_t *q, int i, int v) {
    (void)i, (void)v;
    if (seq_cview(q) == CVIEW_RBROWSE) rp_browse_end(q, RP_C, 1);
    else seq_browse_done(q);
}
static int bdo_label(const seq_t *q, int i, char *buf, int len) { return dn_label(q, i, buf, len); }

/* KEYS, display only: kyw_ the white keys and kyb_ the black ones from MIDI 24 (C1), lit = seq_key_lit; kye_ the edges. Polled
 * every 10 ms on the audio thread, so a bit test: the latch the audio thread keeps (seq_t.keys, what seq_key_lit reads) */
static uint64_t key_bits(const seq_t *q) {   /* 0.24 (Task 7) on PHRASE the chord its centre bar ends on, the selected hit's while editing */
    return RP_C->paged(q) ? inst_of(q)->ph[DL_CHORDS].keys : q->keys.lit;
}
static int key_midi(int white, int i) { return SEQ_KEY_LO + 12 * (i / (white ? 7 : 5)) + (white ? WHITE_KEYS[i % 7] : BLACK_KEYS[i % 5]); }
static int kyw_on(const seq_t *q, int i) { return (int)(key_bits(q) >> (key_midi(1, i) - SEQ_KEY_LO) & 1); }
static void kyw_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int kyw_label(const seq_t *q, int i, char *buf, int len) { return blank_label(q, i, buf, len); }
static int kyb_on(const seq_t *q, int i) { return (int)(key_bits(q) >> (key_midi(0, i) - SEQ_KEY_LO) & 1); }
static void kyb_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int kyb_label(const seq_t *q, int i, char *buf, int len) { return blank_label(q, i, buf, len); }
static int kye_on(const seq_t *q, int i) {
    return RP_C->paged(q) ? inst_of(q)->ph[DL_CHORDS].kedge >> i & 1 : q->keys.edge[i];
}
static void kye_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int kye_label(const seq_t *q, int i, char *buf, int len) { (void)q; return arrow_label(i, buf, len); }

/* 0.10 BASS tab: bmd_ the mode (bmode) */
static const char *const BMODE_NAMES[3] = {"IDEAS", "SHAPE", "FIX A NOTE"};
static void set_bmode(seq_t *q, int i) {
    i = i < 0 ? 0 : i > 2 ? 2 : i;
    if (i == q->bass_mode) return;
    q->bass_mode = i;
    q->display_rev++;
}
static int get_bmode(const seq_t *q) { return q->bass_mode; }
static int bmd_on(const seq_t *q, int i) { return q->bass_mode == i; }
static int bmd_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, BMODE_NAMES[i]); }
/* 0.10 the BASS tab's IDEAS cells (bds_, bfd_, bia_, bpn_) read as the CHORDS tab's (retired with them); bmv_ the moves */
static int bmv_label(const seq_t *q, int i, char *buf, int len) {
    static const char *const MOVES[6] = {"MUTATE", "SIMPLIFY", "BUSIER", "OCTAVE POP", "ANTICIPATE", "ROOTS"};
    (void)q;
    return put(buf, len, MOVES[i]);
}

/* 0.10 BASS SHAPE: bsy_ "Starts from" (style_2 Root, Pump, Walk, Manual), boc_ the lane's octave, bap_ the walk approach */
static const int BSY_STYLES[4] = {GEN_BASS_ROOT, GEN_BASS_PUMP, GEN_BASS_WALK, GEN_MANUAL};
static const char *const BSY_NAMES[4] = {"Root", "Pump", "Walk", "Manual"}, *const APPROACH_NAMES[2] = {"Diatonic", "Chromatic"};
static const char *const BNO_NAMES[4] = {"1", "2", "3", "4"};
static int bsy_on(const seq_t *q, int i) { return seq_style(q, LANE_BASS) == BSY_STYLES[i]; }
static int bsy_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, BSY_NAMES[i]); }
static int boc_on(const seq_t *q, int i) { return seq_lane_shape(q, LANE_BASS, BSHAPE_OCT) == i - 1; }
static int boc_label(const seq_t *q, int i, char *buf, int len) { return oc_label(q, i, buf, len); }
static int bap_on(const seq_t *q, int i) { return seq_lane_shape(q, LANE_BASS, BSHAPE_APPROACH) == i; }
static int bap_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, APPROACH_NAMES[i]); }

/* 0.10 FIX A NOTE on the grid's bar, retired (TAPR; 0.24.1 its selection and octave left the code): bst_ its 16 Bass steps (lit =
 * step 1, as selected; 0.24.1 blank: their cache is gone), bnk_ the note keys C..B (never lit), bno_ the octave (lit = 2), brs_ REST,
 * bdn_ DONE */
static int bst_on(const seq_t *q, int i) { (void)q; return i == 0; }
static int bnk_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->bnk_text[i]); }
static int bno_on(const seq_t *q, int i) { (void)q; return i == 1; }
static int bno_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, BNO_NAMES[i]); }
static int brs_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "REST"); }

/* 0.16 FIX by bar on a note lane (Bass, Melody 1), retired (TAPR): the strip (lit = the bar shown; 0.24.1 blank: its texts are gone),
 * NEW BAR and RESET BAR */
static int nb_on(const seq_t *q, int i) { return i < seq_bars(q) && i == seq_grid_bar(q); }
static int nba_label(int i, char *buf, int len) { return put(buf, len, i ? "RESET BAR" : "NEW BAR"); }
static int bba_label(const seq_t *q, int i, char *buf, int len) { (void)q; return nba_label(i, buf, len); }
static int mba_label(const seq_t *q, int i, char *buf, int len) { (void)q; return nba_label(i, buf, len); }
static int bdn_label(const seq_t *q, int i, char *buf, int len) { return dn_label(q, i, buf, len); }

/* 1 when what part's Bass knob counts read changed since they were made (*made: what they were made from, updated) */
static int bass_src_changed(const seq_t *q, int part, fix_src_t *made) {
    const seq_part_t *p = &q->part[part];
    const seq_lane_t *ln = &p->lane[LANE_BASS];
    fix_src_t in;
    /* 0.18 perf: compared where they are, and copied only when something differs (most sets change nothing here) */
    if (made->key == q->key && made->scale == q->scale && made->bars == p->bars && made->chord_len == p->chord_len &&
        made->prog_len == p->prog_len && made->hseed == p->harmony_seed && made->vcol == p->vcol && made->style == ln->gen.style &&
        made->seed == ln->gen.seed && made->complexity == ln->gen.complexity && !memcmp(made->prog, p->prog, sizeof in.prog) &&
        !memcmp(made->src, p->src, sizeof in.src) && !memcmp(made->colour, p->colour, sizeof in.colour) &&
        !memcmp(made->shape, p->lane_shape[LANE_BASS], sizeof in.shape) && !memcmp(made->steps, ln->steps, sizeof in.steps) &&
        !memcmp(made->lock, ln->lock, sizeof in.lock))
        return 0;
    memset(&in, 0, sizeof in);
    in.key = q->key, in.scale = q->scale, in.bars = p->bars, in.chord_len = p->chord_len, in.prog_len = p->prog_len;
    in.hseed = p->harmony_seed, in.vcol = p->vcol, in.style = ln->gen.style;
    in.seed = ln->gen.seed, in.complexity = ln->gen.complexity;
    memcpy(in.prog, p->prog, sizeof in.prog);
    memcpy(in.src, p->src, sizeof in.src);
    memcpy(in.colour, p->colour, sizeof in.colour);
    memcpy(in.shape, p->lane_shape[LANE_BASS], sizeof in.shape);
    memcpy(in.steps, ln->steps, sizeof in.steps);
    memcpy(in.lock, ln->lock, sizeof in.lock);
    *made = in;
    return 1;
}

/* mbars: the Form's bar roles over bars 1..4 of the part (as many as it has; a Loop has none), each with the bar's first
 * chord (coloured); past 63 bytes the names are the plain triads */
static void mbars_part_label(inst_t *s, int part) {
    static const char *const ROLES[MEL_NFORMS][4] = {{"", "", "", ""}, {"CALL", "ANSWER", "CALL", "ANSWER'"},
                                                     {"CALL", "CALL", "ANSWER", "ANSWER'"}, {"CALL", "CALL", "CALL", "TURN'"}};
    const seq_part_t *p = &s->seq.part[part];
    int ph = p->mel.ph[MP_FORM] < MEL_NFORMS ? p->mel.ph[MP_FORM] : 0, n = p->bars < 4 ? p->bars : 4;
    for (int plain = 0; plain < 2; plain++) {
        txt_t t = {s->mbars_text[part], sizeof s->mbars_text[part], 0};
        for (int b = 0; b < n; b++) {
            int slot = ((b * SEQ_STEPS) / p->chord_len) % p->prog_len;
            char name[16];
            if (plain) theory_chord_name(s->seq.key, theory_slot_scale(s->seq.scale, p->src[slot]), p->prog[slot], name, sizeof name);
            add(&t, b ? " \xC2\xB7 " : "");
            add(&t, ROLES[ph][b]);
            add(&t, *ROLES[ph][b] ? " " : "");
            add(&t, plain ? name : p->slot_text[slot]);
        }
        if (t.n < t.len) return;
    }
}

/* 0.13 the stepped knobs (decision 5): each step's value of the old param it sets, and its name. A knob the core reads by
 * thresholds (SPREAD: above 50 open, from 80 wide; every Melody knob, melody.c) reports the step whose band its value plays
 * in (bands: where steps 1.. start); 0.13 HARMONY too (gen.c's tiers, 34 and 67; Rare + borrow only at 100). The Bass knobs act
 * by degree, so the step nearest the value. A 0.12 value
 * between steps keeps playing as it is until a step is set: the set of the step reported (MPC's echo) is a no-op. */
typedef struct { int n, vals[5]; const char *names[5]; int bands[4]; } kstep_t;
static const kstep_t KSTEPS[KS_COUNT] = {
    {4, {0, 34, 67, 100}, {"Common", "Colourful", "Rare", "Rare + borrow"}, {34, 67, 100}},   /* gen.c's tiers: ii, iii from 34; vii from 67 */
    {3, {50, 51, 80}, {"Close", "Open", "Wide"}, {51, 80}},
    {5, {0, 25, 50, 75, 100}, {"None", "A little", "Some", "Lots", "All"}, {0}},
    {5, {0, 25, 50, 75, 100}, {"None", "A little", "Some", "Lots", "All"}, {0}},
    {5, {0, 25, 50, 75, 100}, {"None", "A little", "Some", "Lots", "All"}, {0}},
    {5, {0, 25, 50, 75, 100}, {"Lowest", "Low", "Natural", "High", "Highest"}, {0}},
    {2, {20, 50}, {"Narrow", "Full"}, {34}},                     /* RANGE: narrow up to 33 */
    {2, {30, 80}, {"Steps", "Leaps"}, {67}},
    {2, {30, 70}, {"On the beat", "Off the beat"}, {60}},
    {2, {25, 80}, {"Inside", "Outside"}, {67}},                  /* chromatic from 67 (Approach) */
    {3, {20, 50, 85}, {"Echo", "Flip", "Shift"}, {34, 67}},
    {3, {15, 50, 85}, {"Low", "Middle", "High"}, {26, 75}}};    /* an octave down up to 25, up from 75 */
static const int KS_BSHAPE[4] = {BSHAPE_MOVEMENT, BSHAPE_SYNC, BSHAPE_OCTAVES, BSHAPE_REGISTER};   /* KS_BMOV .. KS_BREG */
static const int KS_MK[6] = {MK_RANGE, MK_LEAPS, MK_SYNC, MK_TENSION, MK_ANSWER, MK_REGISTER};      /* KS_MRNG .. KS_MREG */

static int ks_value(const seq_t *q, int part, int k) {   /* the old param's value on a part */
    const seq_part_t *p = &q->part[part];
    if (k == KS_HARM) return p->harmony;
    if (k == KS_SPR) return p->lane_shape[LANE_CHORDS][SHAPE_SPREAD];
    if (k <= KS_BREG) return p->lane_shape[LANE_BASS][KS_BSHAPE[k - KS_BMOV]];
    return seq_part_mel_knob(q, part, KS_MK[k - KS_MRNG]);
}
static int ks_step(const seq_t *q, int part, int k) {
    const kstep_t *t = &KSTEPS[k];
    int v = ks_value(q, part, k), best = 0;
    if (t->bands[0]) {
        while (best < t->n - 1 && v >= t->bands[best]) best++;
        return best;
    }
    for (int i = 1; i < t->n; i++)   /* ties: the lower step */
        if (abs(v - t->vals[i]) < abs(v - t->vals[best])) best = i;
    return best;
}

/* What a knob's text is made from, one number (the value, a step and its count, a lane's onsets per bar, rounded) */
static int knob_made(const inst_t *s, int part, int kt) {
    static const signed char SHAPE_OF[KT_MGAT - KT_CGATE][2] = {{LANE_CHORDS, SHAPE_GATE}, {LANE_CHORDS, SHAPE_DYN},
        {LANE_CHORDS, SHAPE_STRUM}, {LANE_CHORDS, SHAPE_PUSH}, {LANE_BASS, BSHAPE_PUSH}, {LANE_BASS, BSHAPE_GATE}, {LANE_BASS, BSHAPE_ACCENT}};
    const seq_t *q = &s->seq;
    const seq_part_t *p = &q->part[part];
    if (kt <= KT_CX3) {
        const seq_step_t *st = p->lane[kt - KT_CX1].steps;
        int on = 0;
        for (int i = 0; i < p->bars * SEQ_STEPS; i++) on += st[i].on != 0;
        return (2 * on + p->bars) / (2 * p->bars);   /* rounded: 0 is under half an onset a bar */
    }
    if (kt == KT_SWING) return q->swing;
    if (kt == KT_GATE) return q->gate;
    if (kt == KT_DYN) return q->dyn;
    if (kt < KT_MGAT) return p->lane_shape[SHAPE_OF[kt - KT_CGATE][0]][SHAPE_OF[kt - KT_CGATE][1]];
    if (kt == KT_MGAT) return seq_part_mel_knob(q, part, MK_GATE);
    if (kt == KT_JPUSH || kt == KT_JDYN) return kt == KT_JPUSH ? p->mjam.push : p->mjam.dyn;   /* 0.20 */
    int k = kt - KT_KS, step = ks_step(q, part, k);
    if (k >= KS_BMOV && k <= KS_BOCT) return step * 1000 + s->bcount[part][k - KS_BMOV];
    if (k == KS_MTEN) return step + 100 * (seq_part_mel_type(q, part) == MEL_APPR);
    return step;
}

/* "1/N", N the power of two nearest 4 / beats in log terms (a note value: 1/4 is a beat), at most 256 */
static void add_note_value(txt_t *t, double beats) {
    long n = lround(log2(4.0 / beats));
    add(t, "1/");
    add_int(t, n >= 8 ? 256 : 1 << (n < 0 ? 0 : n));
}

/* A stepped knob's text from knob_made's number: its step's name, the Bass transforms' notes changed ("Some \xC2\xB7 6"),
 * TENSION off Approach "Approach only". Every knob text is at most 14 characters (the knob value box) */
static void ks_format(int k, int v, txt_t *t) {
    int bass = k >= KS_BMOV && k <= KS_BOCT, step = bass ? v / 1000 : k == KS_MTEN ? v % 100 : v, n = v % 1000;
    add(t, k == KS_MTEN && v < 100 ? "Approach only" : KSTEPS[k].names[step]);
    if (bass && step) add(t, " \xC2\xB7 "), add_int(t, n);
}

/* A knob's text from what it was made from (knob_made), within its knob boxes (PLAY's 130 px, SHAPE's 151 px at 22 px:
 * tools/textfit.py): "12 per bar" ("<1 per bar" under half an onset), "Swing 25%", "Gate 40%",
 * "Dyn x1.0", "Flat", "Even", "Accented +20", "Softer -20", "Strum 1/64", "Ahead 1/32", a step's text (ks_format: "Some \xC2\xB7 6",
 * "Approach only"); COLOUR's is its level (0.17, display_text) */
static void knob_format(int kt, int v, char *buf, int len) {
    txt_t t = {buf, len, 0};
    buf[0] = 0;
    if (kt <= KT_CX3) {
        if (v) add_int(&t, v);
        else add(&t, "<1");
        add(&t, " per bar");
    } else if (kt == KT_SWING) {
        if (v <= 0 || v >= 100) add(&t, v <= 0 ? "Straight" : "Triplet");
        else add(&t, "Swing "), add_int(&t, v), add(&t, "%");
    } else if (kt == KT_GATE || kt == KT_CGATE || kt == KT_BGATE || kt == KT_MGAT) {
        add(&t, "Gate "), add_int(&t, seq_gate_pct(v)), add(&t, "%");
    } else if (kt == KT_DYN) {
        int x = (400 + 12 * v + 50) / 100;   /* the velocity factor (40 + 1.2 v) / 100, in tenths */
        add(&t, "Dyn x"), add_int(&t, x / 10), add(&t, "."), add_int(&t, x % 10);
    } else if (kt == KT_CDYN || kt == KT_BACC || kt == KT_JDYN) {
        if (v <= 5 || v == 50) add(&t, v <= 5 ? "Flat" : "Even");
        else add(&t, v > 50 ? "Accented +" : "Softer -"), add_int(&t, abs(v - 50));
    } else if (kt == KT_CSTRUM || kt == KT_CPUSH || kt == KT_BPUSH || kt == KT_JPUSH) {
        if (v <= 0) add(&t, kt == KT_CSTRUM ? "Together" : "On the beat");
        else add(&t, kt == KT_CSTRUM ? "Strum " : "Ahead "), add_note_value(&t, v / 100.0 * (kt == KT_CSTRUM ? 0.0625 : 0.125));
    } else {
        ks_format(kt - KT_KS, v, &t);
    }
}

/* Every knob text of a part, each made again only when what it reads changed */
static void knob_labels(inst_t *s, int part) {
    for (int kt = 0; kt < KT_COUNT; kt++) {
        int made = knob_made(s, part, kt);
        if (made == s->kmade[part][kt]) continue;
        s->kmade[part][kt] = made;
        knob_format(kt, made, s->ktext[part][kt], sizeof s->ktext[part][kt]);
    }
}

/* 0.17 one part's PROGRESSION and VOICING field texts: the preset its unlocked slots show, and "Lo-fi Keys", "Lo-fi Keys
 * (edited)" ("(ed.)" past PROG_CHARS) or "Custom", made again when the voicing changed */
static void field_labels(inst_t *s, int part) {
    const seq_part_t *p = &s->seq.part[part];
    int in[8] = {p->vstyle, p->vmode, p->vcol, p->vinv, p->vspr, p->lane_shape[LANE_CHORDS][SHAPE_STRUM], p->vstrdir,
                 p->lane_shape[LANE_CHORDS][SHAPE_OCT]};
    txt_t t = {s->cvo_text[part], sizeof s->cvo_text[part], 0};
    s->shown[part] = seq_part_prog_preset_shown(&s->seq, part);
    if (!memcmp(in, s->cvo_src[part], sizeof in)) return;
    memcpy(s->cvo_src[part], in, sizeof in);
    if (p->vstyle < 0 || p->vstyle >= VOICING_NSTYLES) {
        add(&t, "Custom");
        return;
    }
    add(&t, VOICING_STYLES[p->vstyle].name);
    if (seq_part_voicing_edited(&s->seq, part)) add_edited(&t, VOICING_STYLES[p->vstyle].name);
}

/* 0.19 one bar's BASS page caches from its notes as they play (note, 0 silent; all 0 on a lane that isn't DRAWN for ucn, as
 * seq_bass_line reads it): its tension score, its KEYS (every note of the bar, latched a bar, never per step) and ucn's text */
static void bass_bar_labels(inst_t *s, int part, int b, const unsigned char note[SEQ_STEPS], int drawn) {
    unsigned char seen[128] = {0};
    int on[SEQ_STEPS], c = 0, n = 0;
    seq_keys_t k;
    txt_t t = {s->ucn_text[part][b], sizeof s->ucn_text[part][b], 0};
    s->bscore[part][b] = (unsigned char)seq_part_bass_bar_score(&s->seq, part, b);
    for (int i = 0; i < SEQ_STEPS; i++)
        if (note[i]) on[c++] = note[i];
    k = seq_keys_of(on, c);   /* a note outside folded in by octaves */
    s->bkeys[part][b] = k.lit, s->bkedge[part][b] = (unsigned char)(k.edge[0] | k.edge[1] << 1);
    for (int i = 0; i < SEQ_STEPS && drawn; i++) {   /* its notes in order, each once, and how many play */
        if (!note[i]) continue;
        n++;
        if (seen[note[i] & 127]++) continue;
        if (n > 1) add(&t, " ");
        add_note(&t, note[i]);
    }
    if (!n) add(&t, "no notes");
    add(&t, " \xC2\xB7 "), add_int(&t, n), add(&t, n == 1 ? " note" : " notes");
}

/* 0.19 one part's BASS page caches (bass_bar_labels), made again only for the bars whose notes changed, every bar when the chords
 * did */
static void bass_labels(inst_t *s, int part) {
    const seq_part_t *p = &s->seq.part[part];
    bass_src_t in, *was = &s->bscore_src[part];
    int all;
    memset(&in, 0, sizeof in);
    in.key = s->seq.key, in.scale = s->seq.scale, in.bars = p->bars, in.chord_len = p->chord_len, in.prog_len = p->prog_len;
    in.drawn = p->lane[LANE_BASS].gen.style == GEN_BASS_DRAWN;
    memcpy(in.prog, p->prog, sizeof in.prog);
    memcpy(in.src, p->src, sizeof in.src);
    for (int i = 0; i < p->bars * SEQ_STEPS; i++) in.note[i] = (unsigned char)seq_abs_note(&p->lane[LANE_BASS].steps[i]);
    all = memcmp(&in, was, offsetof(bass_src_t, note)) != 0;
    if (!all && !memcmp(in.note, was->note, sizeof in.note)) return;
    for (int b = 0; b < p->bars; b++)
        if (all || memcmp(&in.note[b * SEQ_STEPS], &was->note[b * SEQ_STEPS], SEQ_STEPS))
            bass_bar_labels(s, part, b, &in.note[b * SEQ_STEPS], in.drawn);
    *was = in;
}

/* On the UI thread after every set: the playing and the queued part (the lane, the view or the part may have changed) */
static void part_labels_of(inst_t *s, int part) {
    slot_labels(s, part);
    field_labels(s, part);
    bass_labels(s, part);
    if (bass_src_changed(&s->seq, part, &s->fix_src[part])) seq_part_bass_shape_counts(&s->seq, part, s->bcount[part]);
    mbars_part_label(s, part);
    knob_labels(s, part);
    mel_key_labels(s, part);
}
static void part_labels(inst_t *s) {
    part_labels_of(s, seq_part(&s->seq));
    if (seq_queued_part(&s->seq) >= 0) part_labels_of(s, seq_queued_part(&s->seq));
}

/* 0.11 MELODY tab, on the Melody idea lane (LANE_MEL_UP): mmd_ the mode (mmode; a change goes BACK on every lane) */
static const char *const MMODE_NAMES[4] = {"RECIPE", "IDEAS", "SHAPE", "FIX NOTE"};
static const char *const PHRASE_NAMES[MEL_NPHRASES] = {"1 + 1 bars", "2 + 2 bars"};
static const char *const MNO_NAMES[5] = {"3", "4", "5", "6", "7"};
/* mcap: canvas `hint` per type; a classic lane's and a Manual one's (at most 47 characters) */
static const char *const MEL_HINT[MEL_NTYPES] = {
    "Arp: plays the chord tones in a pattern", "Ostinato: one short riff repeated over every chord",
    "Pentatonic: a riff on the five safest notes of the key", "Hook: a short motif that moves with the chords",
    "Sequence: the same motif, moved a step every bar", "Guide tones: the 3rd and 7th of each chord, moving smoothly",
    "Approach: lands on chord tones from a step away"};
static void set_mmode(seq_t *q, int i) {
    i = clampi(i, 0, 3);
    if (i == q->mel_mode) return;
    q->mel_mode = i;
    q->display_rev++;
}
static int get_mmode(const seq_t *q) { return q->mel_mode; }
static int mmd_on(const seq_t *q, int i) { return q->mel_mode == i; }
static int mmd_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, MMODE_NAMES[i]); }

/* RECIPE: mty_ the types, lit on the type shown (a classic lane's; none on Manual); a tap sets the type's defaults (a classic or
 * Manual lane becomes a recipe; the lit classic type's "0" too). mo<n>_ and mol_ show the type shown (Manual: the Hook's, which a tap converts to); an option
 * lights only on a lane with a type. */
static int mel_label_type(const seq_t *q) { int t = seq_mel_type(q); return t < 0 ? MEL_HOOK : t; }
static int mty_on(const seq_t *q, int i) { return seq_mel_type(q) == i; }
static int mty_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, MEL_TYPE_NAME[i]); }
static int mo_on(const seq_t *q, int w, int i) { return seq_mel_type(q) >= 0 && i < mel_nopts(seq_mel_type(q), w) && seq_mel_opt(q, w) == i; }
static int mo_label(const seq_t *q, int w, int i, char *buf, int len) {
    int t = mel_label_type(q);
    if (i >= mel_nopts(t, w)) return put(buf, len, " ");
    return put(buf, len, t == MEL_PENT && w == 0 ? inst_of(q)->pent_text[i] : mel_opt_name(t, w, i));
}
#define MO_ROW(n) \
    static int mo##n##_on(const seq_t *q, int i) { return mo_on(q, n - 1, i); } \
    static int mo##n##_label(const seq_t *q, int i, char *buf, int len) { return mo_label(q, n - 1, i, buf, len); }
MO_ROW(1) MO_ROW(2) MO_ROW(3)
static int mol_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, mel_row_name(mel_label_type(q), i)); }
static int mph_on(const seq_t *q, int i) { return seq_mel_phrase(q) == i; }
static int mph_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, PHRASE_NAMES[i]); }
static int mcap_text(const seq_t *q, char *buf, int len) {
    int t = seq_mel_type(q);
    if (t < 0) return put(buf, len, "Manual: your own notes; tap a type to start");
    if (seq_mel_is_classic(q))
        return put(buf, len, t == MEL_ARP ? "Arp (classic): a 0.6 arp; tap to rebuild it" : "Hook (classic): a 0.6 hook; tap to rebuild it");
    return put(buf, len, MEL_HINT[t]);
}
static int mkst_text(const seq_t *q, char *buf, int len) { return put(buf, len, inst_of(q)->mkst_text[mel_label_type(q)]); }
static int mbars_text(const seq_t *q, char *buf, int len) { return put(buf, len, inst_of(q)->mbars_text[seq_part(q)]); }

/* IDEAS (retired): as BASS's on the Melody lane; mmv_ the moves, SHIFT with " >" (it stepped the melody one 16th later) */
static int mmv_label(const seq_t *q, int i, char *buf, int len) {
    static const char *const MOVES[4] = {"MUTATE", "REVERSE", "INVERT", "SHIFT >"};
    (void)q;
    return put(buf, len, MOVES[i]);
}

/* SHAPE: mrk_ RESET TO <TYPE> (the knobs and complexity to the type's starts; converts a classic or Manual lane) */
static int mrk_label(const seq_t *q, int i, char *buf, int len) { (void)i; return put(buf, len, inst_of(q)->mrk_text[mel_label_type(q)]); }

/* VST-12 RECIPE phrasing: the steppers' options (mpf, mpb, mpc, mpl), mpr_ RESET PHRASE (0.24.1 retired: TAPR), mphint the line
 * explaining the setting set last (Form's now) */
#define MP_ROW(p, w) \
    static int p##_on(const seq_t *q, int i) { return seq_mel_phrasing(q, w) == i; } \
    static int p##_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, mel_phrase_name(w, i)); }
MP_ROW(mpf, MP_FORM) MP_ROW(mpb, MP_BREATH) MP_ROW(mpc, MP_CADENCE) MP_ROW(mpl, MP_FILL)
static int mpr_label(const seq_t *q, int i, char *buf, int len) { (void)q; (void)i; return put(buf, len, "RESET PHRASE"); }
static const char *const PH_LABEL[MP_COUNT] = {"Form", "Breath", "Cadence", "Fill"};
static const char *const PH_HINT[MP_COUNT][4] = {
    {"no call or answer, every bar alike", "a 1-bar call, then a 1-bar answer", "a 2-bar call, then a 2-bar answer",
     "3 bars of call, bar 4 turns around"},
    {"the line runs straight through", "rests on the call's last 8th", "rests on the call's last beat",
     "rests for the call's second half"},
    {"the last note stays as made", "the loop's last note goes to the key note", "calls end on the 5th, answers come home", ""},
    {"silence in the breath", "a note a step below the answer's first", "", ""}};
static int phrase_hint(const seq_t *q, char *buf, int len) {   /* 0.24.1 the phrasing cells are retired: Form's line, as at first */
    int v = seq_mel_phrasing(q, MP_FORM);
    char t[80];
    snprintf(t, sizeof t, "%s %s: %s", PH_LABEL[MP_FORM], mel_phrase_name(MP_FORM, v), PH_HINT[MP_FORM][v]);
    return put(buf, len, t);
}

/* mkst and mrk_ per type, once: "Knobs start at (Hook): 40 45 30 35 25 50 55 70" (COMPLEXITY, then the MK_* knobs), "RESET TO HOOK" */
static void mel_type_labels(inst_t *s) {
    for (int ty = 0; ty < MEL_NTYPES; ty++) {
        mel_recipe_t r;
        int cx;
        txt_t t = {s->mkst_text[ty], sizeof s->mkst_text[ty], 0};
        memset(&r, 0, sizeof r);
        mel_defaults(ty, &r, &cx);
        add(&t, "Knobs start at (");
        add(&t, MEL_TYPE_NAME[ty]);
        add(&t, "): ");
        add_int(&t, cx);
        for (int k = 0; k < MK_COUNT; k++) {
            add(&t, " ");
            add_int(&t, r.kn[k]);
        }
        int n = snprintf(s->mrk_text[ty], sizeof s->mrk_text[ty], "RESET TO ");
        put_upper(s->mrk_text[ty] + n, (int)sizeof s->mrk_text[ty] - n, MEL_TYPE_NAME[ty]);
    }
}

/* FIX NOTE on the grid's bar, as BASS's on Melody 1, retired (TAPR): mst_ its 16 steps (lit = step 1; blank), mnk_ the note keys C..B
 * (never lit), mno_ octaves 3..7 (lit = 4), mrt_ REST, mdn_ DONE */
static int mst_on(const seq_t *q, int i) { (void)q; return i == 0; }
static int mno_on(const seq_t *q, int i) { (void)q; return i == 1; }
static int mno_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, MNO_NAMES[i]); }
static int mnk_label(const seq_t *q, int i, char *buf, int len) { return bnk_label(q, i, buf, len); }   /* same key and scale */
static int mrt_label(const seq_t *q, int i, char *buf, int len) { return brs_label(q, i, buf, len); }
static int mdn_label(const seq_t *q, int i, char *buf, int len) { return dn_label(q, i, buf, len); }

/* pt_1..6: the parts in screen order. Lit on the playing part (a queued one shows in pos); select 0, so a "0" acts too: the
 * tap on the lit cell or the queued one cancels the queue (seq_select_part decides) */
static int pt_on(const seq_t *q, int i) { return seq_part(q) == SEQ_PART_SCREEN[i]; }
static void pt_tap(seq_t *q, int i, int v) { (void)v; seq_select_part(q, SEQ_PART_SCREEN[i]); }
static int pt_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put_upper(buf, len, SEQ_PART_NAME[SEQ_PART_SCREEN[i]]); }

/* 0.19 the BASS page (header comment, 0.19): the BASS tab's BASS mode on the playing part's DRAWN Bass lane. Its strip pages by bars
 * (seq_strip_by_bars(TAB_BASS) 1), 4 a page; the centre is a bar. umsg: what the page's last action did, made on the UI thread */
static txt_t umsg_new(seq_t *q) {
    inst_t *s = inst_rw(q);
    s->umsg[0] = 0;
    s->msg_at[MSG_BASS] = s->frames;
    return (txt_t){s->umsg, sizeof s->umsg, 0};
}
static void add_note(txt_t *t, int midi) {   /* "G1" (theory_note_name; below C0 as octave 0) */
    char x[12];
    theory_note_name(midi < 12 ? midi + 12 : midi, x, sizeof x);
    add(t, x);
}
static const seq_part_t *ppart(const seq_t *q) { return &q->part[seq_part(q)]; }
static int bass_txn(const seq_t *q) {   /* an open Bass transaction (0.22: JAM is a page) */
    int v = seq_bview(q);
    return v != BVIEW_BASS && v != BVIEW_RHYTHM && v != BVIEW_JAM;
}
static int bass_live(const seq_t *q) { return seq_follow(q) && !seq_follow_paused(q, TAB_BASS) && seq_cur_step(q) >= 0; }
static int bstrip_first(const seq_t *q) { return seq_strip_page(q, TAB_BASS) * 4; }   /* the strip page's first bar */
/* The centre's bar: the editor's; while FOLLOW runs and plays the playing bar; else the BASS tab's centre (seq_centre_slot: a bar on
 * that tab, so a 2-bar chord's second bar can be the centre) */
static int bass_centre(const seq_t *q) {
    int bar = seq_rbar_bar(q, LANE_BASS);
    if (bar >= 0) return bar;
    if (bass_live(q)) return seq_cur_bar(q);
    return seq_centre_slot(q, TAB_BASS);
}
/* 0.19.1 the BASS page's strip page first bar and centre bar: live on its page; while the tab shows RHYTHM as they last were on BASS
 * (ufirst, ucentre), so its polled cells stay still off their page (as ust_on's !seq_bmode) */
static int ushown_first(const seq_t *q) { return seq_bmode(q) ? inst_of(q)->ufirst : bstrip_first(q); }
static int ushown_centre(const seq_t *q) { return seq_bmode(q) ? clampi(inst_of(q)->ucentre, 0, seq_bars(q) - 1) : bass_centre(q); }
static int bar_nlocked(const seq_t *q, int bar) { return ppart(q)->bass.nlock >> bar & 1; }
static int bar_level(const seq_t *q, int bar) { return bass_level(inst_of(q)->bscore[seq_part(q)][bar]); }   /* from bass_labels' cache */

/* ubm_ the mode cells BASS, RHYTHM, 0.22 JAM (lit = seq_bmode; the editor and the STYLE browser are BASS's, an open bar RHYTHM's); 0.24 RHYTHM
 * is PHRASE */
static const char *const UBM_NAMES[3] = {"BASS", "PHRASE", "JAM"};
static int ubm_on(const seq_t *q, int i) { return seq_bmode(q) == i; }
static void ubm_tap(seq_t *q, int i, int v) { static const int V[3] = {BVIEW_BASS, BVIEW_RHYTHM, BVIEW_JAM}; (void)v; seq_set_bview(q, V[i]); }
static int ubm_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, UBM_NAMES[i]); }

/* ust_ the strip page's 4 bars (blank and inert past the loop; lit while it plays; select 0: the playing one's tap opens it too): "BAR 6
 * \xC2\xB7 own \xC2\xB7 2 fixed \xC2\xB7 tense \xC2\xB7 Em C" (own: a NEW BAR line; LOCKED: note-locked; the tension word, bass_level_word,
 * left out when plain: the toolchain's tint=1 can't colour a second strip), shorter until it fits CST_CHARS. A tap opens the bar editor
 * on it (FOLLOW off: it becomes the centre); inert while a Bass transaction is open */
static int ust_on(const seq_t *q, int i) {   /* unlit on RHYTHM, where ysc_ is the strip (a polled cell off its page stays still) */
    return !seq_bmode(q) && strip_lit(q, seq_rbar_bar(q, LANE_BASS), seq_cur_bar(q), bstrip_first(q) + i);   /* 0.19.1 the editor's bar */
}
static int ust_label(const seq_t *q, int i, char *buf, int len) {
    /* the forms, longest first: "BAR " or the number alone; the chords (0 none, 1 all, 2 the first and "+N"); the fixed count; the flag
     * (0: LOCKED alone, never own); the tension word. 0.19.1 the parts go in the order chords, tension, fixed, own: LOCKED stays */
    static const unsigned char FORM[8][5] = {{1, 1, 1, 1, 1}, {1, 2, 1, 1, 1}, {0, 2, 1, 1, 1}, {1, 0, 1, 1, 1}, {0, 0, 1, 1, 1},
                                             {0, 0, 1, 1, 0}, {0, 0, 0, 1, 0}, {0, 0, 0, 0, 0}};
    const inst_t *s = inst_of(q);
    int bar = ushown_first(q) + i, nf, n, level, locked;
    const char *flag, *ch;
    char first[sizeof s->rbar_text[0][0]];
    if (bar >= seq_bars(q)) return put(buf, len, " ");
    locked = bar_nlocked(q, bar);
    flag = locked ? "LOCKED" : ppart(q)->lane[LANE_BASS].bar_var[bar] ? "own" : NULL;
    nf = seq_bass_fixed_count(q, bar), ch = s->rbar_text[seq_part(q)][bar], n = s->rbar_n[seq_part(q)][bar], level = bar_level(q, bar);
    first_chord(ch, first);
    for (int f = 0; f < 8; f++) {
        txt_t t = {buf, len, 0};
        if (FORM[f][0]) add(&t, "BAR ");
        add_int(&t, bar + 1);
        if (flag && (FORM[f][3] || locked)) add(&t, " \xC2\xB7 "), add(&t, flag);
        if (nf && FORM[f][2]) add(&t, " \xC2\xB7 "), add_int(&t, nf), add(&t, " fixed");
        if (level && FORM[f][4]) add(&t, " \xC2\xB7 "), add(&t, bass_level_word(level));
        if (FORM[f][1]) add(&t, " \xC2\xB7 "), add(&t, FORM[f][1] == 1 ? ch : first);
        if (FORM[f][1] == 2 && n > 1) add(&t, "+"), add_int(&t, n - 1);
        if (theory_text_width(buf) <= CST_CHARS || f == 7) return t.n;
    }
    return 0;
}
/* usr_ the strip page's bars as they play: the 16 role characters (bassline.h: 1 o 3 5 p c P f, - held, . rest); blank past the
 * loop. Display only */
static int bar_word(const seq_t *q, int bar, char *buf, int len) {   /* bar's line as it plays, its roles; " " past the loop */
    char role[SEQ_STEPS + 1];
    if (bar >= seq_bars(q)) return put(buf, len, " ");
    memcpy(role, seq_bass_roles(q, bar), SEQ_STEPS);
    role[SEQ_STEPS] = 0;
    return put(buf, len, role);
}
static int usr_label(const seq_t *q, int i, char *buf, int len) { return bar_word(q, ushown_first(q) + i, buf, len); }
/* upg_, und_, ubtp_: the strip chrome both tabs share (tb_*) */

/* usy_ STYLE: <, the field (the style's name, "(edited)" when PEDAL, OCTAVE or knobs 2-5 differ from it; tapped, the STYLE browser),
 * >; the arrows step the 48 styles (from Custom: the first or the last), a try while the browser is open */
static int ustn_text(const seq_t *q, char *buf, int len) {
    int st = seq_bass_style(q);
    txt_t t = {buf, len, 0};
    if (st < 0 || st >= BASS_STYLE_COUNT) return put(buf, len, "Custom");
    add(&t, bass_style_name(st));
    if (seq_bass_edited(q)) add_edited(&t, bass_style_name(st));
    return t.n;
}
static void style_msg(seq_t *q) {   /* "Pop Octaves: an octave now and then" */
    int st = seq_bass_style(q);
    txt_t m = umsg_new(q);
    if (st >= 0 && st < BASS_STYLE_COUNT) add(&m, bass_style_name(st)), add(&m, ": "), add(&m, bass_style_feel(st));
}
static void usy_tap(seq_t *q, int i, int v) {
    int next = step_of(seq_bass_style(q), BASS_STYLE_COUNT, i == 2);
    (void)v;
    if (seq_bview(q) == BVIEW_STYLE) {
        if (i != 1) q->tv[TAB_BASS].browse_genre = bass_style_genre(next), seq_bstyle_try(q, next), style_msg(q);
        return;
    }
    if (i == 1) {
        seq_bstyle_open(q);
        umsg_new(q);
        return;
    }
    seq_set_bass_style(q, next);
    style_msg(q);
}
static int usy_label(const seq_t *q, int i, char *buf, int len) { return i != 1 ? arrow_label(i == 2, buf, len) : ustn_text(q, buf, len); }
/* upd_ PEDAL OFF, TONIC, DOMINANT (lit = the part's; the key's note in each: pedal_labels), uoc_ OCTAVE -1, 0, +1 */
static int upd_on(const seq_t *q, int i) { return ppart(q)->bass.set.pedal == i; }
static void upd_tap(seq_t *q, int i, int v) {
    txt_t m;
    (void)v;
    seq_set_bass_pedal(q, i);
    m = umsg_new(q);
    if (!i) add(&m, "PEDAL off: the bass follows the chords again");
    else add(&m, i == 1 ? "PEDAL tonic: every note is " : "PEDAL dominant: every note is "),
         add(&m, theory_key_name(pedal_pc(seq_key(q), seq_scale(q), i))), add(&m, ", the chords move above");
}
static int upd_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->upd_text[i]); }
static int uoc_on(const seq_t *q, int i) { return ppart(q)->bass.set.octave == i - 1; }
static void uoc_tap(seq_t *q, int i, int v) {
    txt_t m;
    (void)v;
    seq_set_bass_octave(q, i - 1);
    m = umsg_new(q);
    add(&m, "OCTAVE "), add(&m, OCTAVE_NAMES[i]), add(&m, " for the whole line");
}
static int uoc_label(const seq_t *q, int i, char *buf, int len) { return oc_label(q, i, buf, len); }
static int ucw_text(const seq_t *q, char *buf, int len) { return bar_word(q, ushown_centre(q), buf, len); }   /* the centre's line */

/* 0.19.1 the strip chrome both tabs share: one body each, taking the tab (TB_CELL makes the CHORDS tab's cell and the BASS tab's, as
 * RP_CELL the RHYTHM pages'). The page arrows (cpg_, upg_; CHORDS: inert while PHRASE (rpage_t, the RHYTHM page's) holds its bar or browser, an open picker
 * or browser kept first; BASS: inert while a Bass transaction is open, FOLLOW holding its page), UNDO (cund_, und_), BACK TO PLAYING
 * (cbtp_, ubtp_; blank and inert unless paged), RESET KNOBS (crk_, urk_; BASS: seq_reset_bass_knobs: knobs 2-5, PEDAL and OCTAVE to
 * the style's, COMPLEXITY, PUSH, GATE, DYNAMICS fresh, with no Bass transaction open, blank and inert otherwise), the centre (cct_: the
 * picker over the centre chord, ccn's text; uct_: the editor on the centre's bar, inert while a Bass transaction is open, ucw's text);
 * the readouts cfol, ufol and cpage, upage (tb_fol_text, tb_page_text). 0.20 the MELODY tab's PHRASE page too (TB_MEL: wpg_, wund_,
 * wbtp_, wct_, as the BASS tab's, inert while its picker or editor is open, the message on jmsg; wct_'s text blank: wcwh .. wcend hold it) */
static int mel_txn(const seq_t *q);
static int mel_centre(const seq_t *q);
static void medit_at(seq_t *q, int bar);
static txt_t mmsg_new(seq_t *q);
static void tb_pg_tap(seq_t *q, int tab, int i, int v) {
    int page;
    txt_t m;
    (void)v;
    if (tab == TAB_CHORDS) {
        if (rp_open(q, RP_C)) return;   /* 0.18 (FOLLOW paused) */
        close_txn(q);
    } else if (tab == TAB_BASS ? bass_txn(q) : mel_txn(q)) return;
    seq_strip_turn(q, tab, i ? 1 : -1);
    if (tab == TAB_CHORDS || seq_follow_paused(q, tab) != 2) return;
    page = seq_strip_page(q, tab);
    m = tab == TAB_BASS ? umsg_new(q) : mmsg_new(q);
    add(&m, "FOLLOW paused on BARS "), add_int(&m, page * 4 + 1), add(&m, "-"), add_int(&m, page * 4 + 4);
    add(&m, ": BACK TO PLAYING restarts it");
}
static int tb_pg_label(const seq_t *q, int tab, int i, char *buf, int len) { (void)q, (void)tab; return arrow_label(i, buf, len); }
static void tb_und_tap(seq_t *q, int tab, int i, int v) { (void)tab, (void)i, (void)v; seq_undo(q); }
static int tb_und_label(const seq_t *q, int tab, int i, char *buf, int len) { (void)tab, (void)i; return act_label(q, 0, buf, len); }
static void tb_btp_tap(seq_t *q, int tab, int i, int v) { (void)i, (void)v; if (seq_follow_paused(q, tab) == 2) seq_back_to_playing(q, tab); }
static int tb_btp_label(const seq_t *q, int tab, int i, char *buf, int len) {
    (void)i;
    return put(buf, len, seq_follow_paused(q, tab) == 2 ? "BACK TO PLAYING" : " ");
}
static void tb_rk_tap(seq_t *q, int tab, int i, int v) {
    int st;
    txt_t m;
    (void)i, (void)v;
    if (tab == TAB_CHORDS) {
        seq_reset_chord_knobs(q);
        return;
    }
    if (bass_txn(q)) return;
    st = seq_bass_style(q);
    seq_reset_bass_knobs(q);
    m = umsg_new(q);
    add(&m, "RESET KNOBS: back to "), add(&m, st >= 0 && st < BASS_STYLE_COUNT ? bass_style_name(st) : "Custom"), add(&m, "'s own knobs");
}
static int tb_rk_label(const seq_t *q, int tab, int i, char *buf, int len) {
    (void)i;
    return put(buf, len, tab == TAB_BASS && bass_txn(q) ? " " : "RESET KNOBS");
}
/* 0.24 the sound pages' way to PHRASE (EDIT BARS, the centre, a status-row bar): the RHYTHM view (CHORDS: set_mode's, which keeps an open bar
 * as DONE; BASS: seq_set_bview's) */
static void sound_phrase(seq_t *q, int d) {
    if (d == DL_BASS) seq_set_bview(q, BVIEW_RHYTHM);
    else set_mode(q, 1);
}
static void tb_ct_tap(seq_t *q, int tab, int i, int v) {   /* 0.24 cct_1, uct_1: PHRASE (the sound pages' centre) */
    (void)i, (void)v;
    if (tab == TAB_MEL) medit_at(q, mel_centre(q));
    else sound_phrase(q, tab == TAB_BASS ? DL_BASS : DL_CHORDS);
}
static int tb_ct_label(const seq_t *q, int tab, int i, char *buf, int len) {   /* 0.24 the centre's chord name (sound_cells_now) */
    (void)i;
    return put(buf, len, tab == TAB_MEL ? " " : inst_of(q)->snd.name[tab == TAB_BASS ? DL_BASS : DL_CHORDS]);
}
#define TB_TAP(c, u, fn) \
    static void c##_tap(seq_t *q, int i, int v) { fn(q, TAB_CHORDS, i, v); } \
    static void u##_tap(seq_t *q, int i, int v) { fn(q, TAB_BASS, i, v); }
#define TB_LABEL(c, u, fn) \
    static int c##_label(const seq_t *q, int i, char *buf, int len) { return fn(q, TAB_CHORDS, i, buf, len); } \
    static int u##_label(const seq_t *q, int i, char *buf, int len) { return fn(q, TAB_BASS, i, buf, len); }
#define TB_CELL(c, u, tap, label) TB_TAP(c, u, tap) TB_LABEL(c, u, label)
TB_LABEL(cpg, upg, tb_pg_label)   /* 0.24 retired (KF_RETIRED_24): their taps went with the CHORDS and BASS pages' strips */
TB_LABEL(cund, und, tb_und_label)
TB_LABEL(cbtp, ubtp, tb_btp_label)
TB_CELL(crk, urk, tb_rk_tap, tb_rk_label)
TB_CELL(cct, uct, tb_ct_tap, tb_ct_label)

/* The STYLE browser: ugn_ the 12 genre chips (a tap tries the genre's first style, unless the style shown is one of that genre's: the
 * 0.17.1 rule), uti_ the genre's 4 styles ("Pedal Drone \xC2\xB7 PEDAL E \xC2\xB7 one low note under it all", the feel left out past
 * UTI_CHARS; lit = the part's; a tap tries it; blank and inert outside the browser), ubc_ CANCEL, ubd_ DONE (BASS RHYTHM's RHYTHMS
 * browser's too) */
#define UTI_CHARS 32
static int ugn_on(const seq_t *q, int i) { return seq_bstyle_genre(q) == i; }
static void ugn_tap(seq_t *q, int i, int v) {
    int st = seq_bass_style(q);
    (void)v;
    if (st >= 0 && st < BASS_STYLE_COUNT && bass_style_genre(st) == i) q->tv[TAB_BASS].browse_genre = i;
    else seq_bstyle_set_genre(q, i);
    if (seq_bview(q) == BVIEW_STYLE) style_msg(q);
}
static int ugn_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, bass_genre_name(i)); }
static int uti_style(const seq_t *q, int i) { return seq_bstyle_genre(q) * (BASS_STYLE_COUNT / BASS_GENRES) + i; }
static int uti_on(const seq_t *q, int i) { return seq_bview(q) == BVIEW_STYLE && seq_bass_style(q) == uti_style(q, i); }
static void uti_tap(seq_t *q, int i, int v) {
    (void)v;
    if (seq_bview(q) != BVIEW_STYLE) return;
    seq_bstyle_try(q, uti_style(q, i));
    style_msg(q);
}
static int uti_label(const seq_t *q, int i, char *buf, int len) {
    int st = uti_style(q, i), pedal;
    if (seq_bview(q) != BVIEW_STYLE) return put(buf, len, " ");
    pedal = bass_style_set(st).pedal;
    for (int form = 0; form < 2; form++) {
        txt_t t = {buf, len, 0};
        add(&t, bass_style_name(st));
        if (pedal) add(&t, " \xC2\xB7 PEDAL "), add(&t, theory_key_name(pedal_pc(seq_key(q), seq_scale(q), pedal)));
        if (!form) add(&t, " \xC2\xB7 "), add(&t, bass_style_feel(st));
        if (theory_text_width(buf) <= UTI_CHARS || form) return t.n;
    }
    return 0;
}
/* utp_ each tile's picture: the shown bar's 16 roles under its style (seq_bass_style_roles), and uts_ its summary (what the style sets,
 * bset_words: "moves some \xC2\xB7 oct some \xC2\xB7 half below", "pedal tonic \xC2\xB7 low"), made in set_param while the browser is open
 * (utp_labels); display only, blank outside the browser */
static void bset_words(txt_t *t, const bass_set_t *b);
static void utp_labels(inst_t *s) {
    const seq_t *q = &s->seq;
    char role[4][SEQ_STEPS];
    if (seq_bview(q) != BVIEW_STYLE) return;
    seq_bass_style_roles(q, uti_style(q, 0), 4, bass_centre(q), role);   /* the bar's codes and chords once for the 4 */
    for (int i = 0; i < 4; i++) {
        bass_set_t b = bass_style_set(uti_style(q, i));
        txt_t t = {s->uts_text[i], sizeof s->uts_text[i], 0};
        memcpy(s->utp_text[i], role[i], SEQ_STEPS);
        s->utp_text[i][SEQ_STEPS] = 0;
        bset_words(&t, &b);
    }
}
static void utp_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int utp_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, seq_bview(q) == BVIEW_STYLE ? inst_of(q)->utp_text[i] : " "); }
static void uts_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int uts_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, seq_bview(q) == BVIEW_STYLE ? inst_of(q)->uts_text[i] : " "); }
static void ubc_tap(seq_t *q, int i, int v) {
    txt_t m;
    (void)i, (void)v;
    if (seq_bview(q) == BVIEW_RBROWSE) {   /* BASS RHYTHM's RHYTHMS browser, as bcn_ */
        rp_browse_end(q, RP_B, 0);
        return;
    }
    if (seq_bview(q) != BVIEW_STYLE) return;
    seq_bstyle_cancel(q);
    m = umsg_new(q);
    add(&m, "Cancelled: back as it was");
}
static int ubc_label(const seq_t *q, int i, char *buf, int len) { return cancel_label(q, i, buf, len); }
static void ubd_tap(seq_t *q, int i, int v) {
    int st = seq_bass_style(q);
    txt_t m;
    (void)i, (void)v;
    if (seq_bview(q) == BVIEW_RBROWSE) {   /* as bdo_ */
        rp_browse_end(q, RP_B, 1);
        return;
    }
    if (seq_bview(q) != BVIEW_STYLE) return;
    seq_bstyle_done(q);
    m = umsg_new(q);
    add(&m, "Kept "), add(&m, st >= 0 && st < BASS_STYLE_COUNT ? bass_style_name(st) : "Custom"), add(&m, ".");
}
static int ubd_label(const seq_t *q, int i, char *buf, int len) { return dn_label(q, i, buf, len); }

/* The bar editor (PHRASE's Bass bar, seq_rbar_*; every cell blank and inert while it is closed): uea_ NEW BAR, RESET BAR, LOCK / UNLOCK, CANCEL, DONE; ues_
 * the bar's 16 steps ("G1 P": the note and its role, "*" when fixed; "-" held, "\xC2\xB7" a rest; lit = selected; a tap selects a
 * sounding step, else explains); unk_ the note keys in two rows, C# D# F# G# A# then C..B (bnk_'s labels: "E 1", the degree in the
 * scale; lit = the selected step's; a tap fixes it there and plays it); uno_ the editor's octave 1..4; uer_ EDIT THIS BAR'S RHYTHM */
static const char *const UEA_NAME[5] = {"NEW BAR", "RESET BAR", "LOCK", "CANCEL", "DONE"};
static const int UNK_PC[12] = {1, 3, 6, 8, 10, 0, 2, 4, 5, 7, 9, 11};
static int uea_label(const seq_t *q, int i, char *buf, int len) {
    int bar = seq_rbar_bar(q, LANE_BASS);
    if (bar < 0) return put(buf, len, " ");
    return put(buf, len, i == 2 && bar_nlocked(q, bar) ? "UNLOCK" : UEA_NAME[i]);
}
static int step_note(const seq_t *q, int bar, int i) {   /* the note a DRAWN Bass step plays (seq_bass_line's), 0 silent; polled: one read */
    return seq_abs_note(&ppart(q)->lane[LANE_BASS].steps[bar * SEQ_STEPS + i]);
}
static int sel_note(const seq_t *q) {   /* the editor's selected step's note, 0 none */
    int bar = seq_rbar_bar(q, LANE_BASS), sel = seq_rbar_sel(q, LANE_BASS);
    return bar < 0 || sel < 0 ? 0 : step_note(q, bar, sel);
}
static int ues_on(const seq_t *q, int i) { return seq_rbar_sel(q, LANE_BASS) == i; }
static int ues_label(const seq_t *q, int i, char *buf, int len) {
    int bar = seq_rbar_bar(q, LANE_BASS), note;
    char role;
    txt_t t = {buf, len, 0};
    if (bar < 0) return put(buf, len, " ");
    note = step_note(q, bar, i), role = ppart(q)->brole[bar * SEQ_STEPS + i];   /* seq_bass_line's, one step */
    if (note) add_note(&t, note), add(&t, " "), add(&t, (char[2]){role, 0});
    else add(&t, role == '-' ? "-" : "\xC2\xB7");
    if (ppart(q)->bass.fix[bar * SEQ_STEPS + i]) add(&t, "*");
    return t.n;
}
static int unk_on(const seq_t *q, int i) { int n = sel_note(q); return n && n % 12 == UNK_PC[i]; }
static int unk_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->bnk_text[UNK_PC[i]]); }
static int uno_on(const seq_t *q, int i) { return seq_rbar_bar(q, LANE_BASS) >= 0 && seq_rbar_oct(q) == i + 1; }
static int uno_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, BNO_NAMES[i]); }
/* The BASS tab's KEYS (display only), as kyw_, kyb_ and kye_ for the Bass lane: ukw_ the white keys and ukb_ the black ones from MIDI 24,
 * lit = a note of the shown bar (the editor's bar while it is open; latched a bar, never per step), uke_ the low and high edge (a note
 * folded in from there). Polled every 10 ms: a bit test of the caches bass_labels makes, at the bar set_param and song_pos keep */
static uint64_t ukey_bits(const seq_t *q) { return inst_of(q)->bkeys[seq_part(q)][inst_of(q)->ukbar]; }
static int ukw_on(const seq_t *q, int i) { return (int)(ukey_bits(q) >> (key_midi(1, i) - SEQ_KEY_LO) & 1); }
static void ukw_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int ukw_label(const seq_t *q, int i, char *buf, int len) { return blank_label(q, i, buf, len); }
static int ukb_on(const seq_t *q, int i) { return (int)(ukey_bits(q) >> (key_midi(0, i) - SEQ_KEY_LO) & 1); }
static void ukb_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int ukb_label(const seq_t *q, int i, char *buf, int len) { return blank_label(q, i, buf, len); }
static int uke_on(const seq_t *q, int i) { return inst_of(q)->bkedge[seq_part(q)][inst_of(q)->ukbar] >> i & 1; }
static void uke_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int uke_label(const seq_t *q, int i, char *buf, int len) { (void)q; return arrow_label(i, buf, len); }
static int uer_label(const seq_t *q, int i, char *buf, int len) { (void)i; return put(buf, len, seq_rbar_bar(q, LANE_BASS) >= 0 ? "EDIT THIS BAR'S RHYTHM" : " "); }

/* 0.20 the MELODY tab's JAM page (header comment, 0.20) on the playing part's JAM Melody lane. jmsg: what the page's last action did, made
 * on the UI thread */
static int mel_jam_on(const seq_t *q) { return seq_style(q, LANE_MEL_UP) == GEN_MEL_JAM; }
static txt_t mmsg_new(seq_t *q) {
    inst_t *s = inst_rw(q);
    s->mmsg[0] = 0;
    s->msg_at[MSG_MEL] = s->frames;
    return (txt_t){s->mmsg, sizeof s->mmsg, 0};
}
static void add_block(txt_t *t, int c, int l) { add(t, mjam_col_name(c)), add(t, " \xC2\xB7 "), add(t, mjam_block_name(c, l)); }   /* "HOOK · MOTIF" */
/* The tab's centre: the editor's open bar; while FOLLOW runs and plays the playing bar; else the MELODY tab's centre (a bar) */
static int mel_live(const seq_t *q) { return seq_follow(q) && !seq_follow_paused(q, TAB_MEL) && seq_cur_step(q) >= 0; }
static int mel_centre(const seq_t *q) {
    int bar = seq_medit_bar(q);
    if (bar >= 0) return bar;
    return mel_live(q) ? seq_cur_bar(q) : seq_centre_slot(q, TAB_MEL);
}
static int mel_shown_bar(const seq_t *q) { return seq_mmode(q) ? mel_centre(q) : seq_cur_bar(q); }   /* the KEYS': JAM the playing bar */

/* jmd_ the mode cells JAM, PHRASE (lit = seq_mmode: the picker and the editor are PHRASE's) */
static const char *const JMD_NAMES[2] = {"JAM", "PHRASE"};
static int jmd_on(const seq_t *q, int i) { return seq_mmode(q) == i; }
static void jmd_tap(seq_t *q, int i, int v) { (void)v; seq_set_mview(q, i ? MVIEW_PHRASE : MVIEW_JAM); }
static int jmd_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, JMD_NAMES[i]); }

/* jbk_ the 30 blocks, column by column (column c, level l: cell c x 5 + l); see the header comment. A tap says what it did on jmsg */
static const char *const SW_NAME[4] = {"BEAT", "BAR", "2 BARS", "PHRASE"};
static const char *const SW_WAIT[4] = {"the next beat", "the next bar", "the next 2-bar line", "the next phrase"};
static int jbk_on(const seq_t *q, int i) {
    const mjam_set_t *m = &ppart(q)->mjam.set;
    return mel_jam_on(q) && m->c * MJ_LEVELS + m->l == i;
}
static void jbk_tap(seq_t *q, int i, int v) {
    int c = i / MJ_LEVELS, l = i % MJ_LEVELS, had = seq_mel_queued(q, NULL, NULL, NULL), was = jbk_on(q, i), at;
    txt_t m;
    (void)v;
    if (mel_jam_on(q) && !was && seq_mel_blocked(q)) {
        m = mmsg_new(q);
        add(&m, "Part switch queued: tap the block after it");
        return;
    }
    seq_mel_queue(q, c, l);
    if (!mel_jam_on(q)) return;   /* the shown block on a lane that isn't JAM: nothing changed (a host echo) */
    m = mmsg_new(q);
    if (seq_mel_queued(q, NULL, NULL, &at)) add_block(&m, c, l), add(&m, " takes over at bar "), add_int(&m, at + 1);
    else if (!was) add(&m, "Now "), add_block(&m, c, l), add(&m, ": stopped, it switches at once");
    else add(&m, had ? "Cancelled: " : ""), add_block(&m, c, l), add(&m, " plays on");
}
static int jbk_label(const seq_t *q, int i, char *buf, int len) {
    int c = i / MJ_LEVELS, l = i % MJ_LEVELS, qc, ql;
    txt_t t = {buf, len, 0};
    add(&t, mjam_block_name(c, l));
    if (jbk_on(q, i)) add(&t, "  >");
    else if (mel_jam_on(q) && seq_mel_queued(q, &qc, &ql, NULL) && qc == c && ql == l) add(&t, "  NEXT");
    else add(&t, "  "), add_int(&t, l + 1);
    return t.n;
}

/* jph_ the playing phrase's 4 bars: "BAR 1 / CALL" (the role in the bar's FORM, a locked bar's frozen one; "LOOP" on FORM LOOP); lit = the
 * playing bar; blank and inert past the loop; a tap shows PHRASE (select 0: the playing one's too; FOLLOW off, that bar its centre).
 * Task 8: on a lane that isn't JAM (an old project's) they are blank, unlit and inert, and jrole blank, as jnow */
static const mjam_set_t *mel_set_of(const seq_t *q, int bar) {   /* the bar's settings: its frozen ones when locked */
    const mjam_state_t *m = &ppart(q)->mjam;
    return bar >= 0 && bar < SEQ_MAX_BARS && m->nlock >> bar & 1 ? &m->frozen[bar] : &m->set;
}
static int mel_form_of(const seq_t *q, int bar) { return mel_set_of(q, bar)->form; }   /* the bar's FORM: its frozen settings' when locked */
static const char *mel_role_word(const seq_t *q, int bar) {
    int f = mel_form_of(q, bar), r = mjam_role(f, bar % 4, NULL);
    return f == MF_LOOP ? "LOOP" : r == 'A' ? "ANSWER" : r == 'R' ? "AGAIN" : r == 'S' ? "STEP UP" : r == 'F' ? "BREAK UP" : "CALL";
}
static int jph_bar(const seq_t *q, int i) { return seq_cur_bar(q) / 4 * 4 + i; }
static int jph_on(const seq_t *q, int i) { return mel_jam_on(q) && seq_cur_step(q) >= 0 && jph_bar(q, i) == seq_cur_bar(q); }
static void jph_tap(seq_t *q, int i, int v) {
    int bar = jph_bar(q, i);
    (void)v;
    if (bar >= seq_bars(q) || !mel_jam_on(q)) return;
    if (!seq_follow(q)) seq_set_centre(q, TAB_MEL, bar);
    seq_set_mview(q, MVIEW_PHRASE);
}
static int jph_label(const seq_t *q, int i, char *buf, int len) {
    int bar = jph_bar(q, i);
    txt_t t = {buf, len, 0};
    if (bar >= seq_bars(q) || !mel_jam_on(q)) return put(buf, len, " ");
    add(&t, "BAR "), add_int(&t, bar + 1), add(&t, " / "), add(&t, mel_role_word(q, bar));
    return t.n;
}

/* jsw_ SWITCH AT: "SWITCH AT / BAR"; a tap cycles BEAT, BAR, 2 BARS, PHRASE (a queued block waits for the new boundary) */
static void jsw_tap(seq_t *q, int i, int v) {
    int sw = (seq_mel_switch(q) + 1) % 4;
    txt_t m;
    (void)i, (void)v;
    seq_set_mel_switch(q, sw);
    m = mmsg_new(q);
    add(&m, "SWITCH AT "), add(&m, SW_NAME[sw]), add(&m, ": a tapped block waits for "), add(&m, SW_WAIT[sw]);
}
static int jsw_label(const seq_t *q, int i, char *buf, int len) {
    txt_t t = {buf, len, 0};
    (void)i;
    add(&t, "SWITCH AT / "), add(&t, SW_NAME[seq_mel_switch(q) & 3]);
    return t.n;
}

/* The MELODY KEYS (display only), as ukw_, ukb_, uke_ for the JAM lane: lit = a note of the shown bar (jkbar: JAM the playing bar, PHRASE
 * the centre), latched a bar. Polled every 10 ms: a bit test of the caches mel_key_labels makes */
static uint64_t jkey_bits(const seq_t *q) { return inst_of(q)->mkeys[seq_part(q)][inst_of(q)->jkbar]; }
static int jkw_on(const seq_t *q, int i) { return (int)(jkey_bits(q) >> (key_midi(1, i) - SEQ_KEY_LO) & 1); }
static void jkw_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int jkw_label(const seq_t *q, int i, char *buf, int len) { return blank_label(q, i, buf, len); }
static int jkb_on(const seq_t *q, int i) { return (int)(jkey_bits(q) >> (key_midi(0, i) - SEQ_KEY_LO) & 1); }
static void jkb_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int jkb_label(const seq_t *q, int i, char *buf, int len) { return blank_label(q, i, buf, len); }
static int jke_on(const seq_t *q, int i) { return inst_of(q)->mkedge[seq_part(q)][inst_of(q)->jkbar] >> i & 1; }
static void jke_tap(seq_t *q, int i, int v) { no_tap(q, i, v); }
static int jke_label(const seq_t *q, int i, char *buf, int len) { (void)q; return arrow_label(i, buf, len); }

/* One part's MELODY KEYS per bar from the JAM lane's line (none on a lane that isn't JAM), made again for the bars whose line changed: in
 * set_param for the playing and the queued part (part_labels), in song_pos after a block switch for the playing one */
static void mel_key_labels(inst_t *s, int part) {
    const seq_part_t *p = &s->seq.part[part];
    static const mjam_note_t NONE[SEQ_STEPS];
    int jam = p->lane[LANE_MEL_UP].gen.style == GEN_MEL_JAM;
    for (int b = 0; b < p->bars; b++) {
        const mjam_note_t *l = jam ? &p->mline[b * SEQ_STEPS] : NONE;
        mjam_note_t *was = &s->mkeys_src[part][b * SEQ_STEPS];
        int on[SEQ_STEPS], c = 0;
        seq_keys_t k;
        if (!memcmp(l, was, sizeof NONE)) continue;   /* calloc: an unmade bar's is NONE, whose keys are none */
        for (int i = 0; i < SEQ_STEPS; i++)
            if (l[i].note) on[c++] = l[i].note;
        k = seq_keys_of(on, c);
        s->mkeys[part][b] = k.lit, s->mkedge[part][b] = (unsigned char)(k.edge[0] | k.edge[1] << 1);
        memcpy(was, l, sizeof NONE);
    }
}

/* The step dots: zs_ the strip page's 4 bars, zc_ the centre bar (seq_mel_frame), cached in zdot when the bars shown change (song_pos:
 * at most once a bar; force: a set or a block switch, which may change the lines); a change bumps display_rev, the only way MPC re-reads
 * them (unpolled numbers) */
static void mel_dots_now(inst_t *s, int force) {
    const seq_t *q = &s->seq;
    int first = seq_strip_page(q, TAB_MEL) * 4, centre = mel_centre(q), part = seq_part(q);
    unsigned char d[sizeof s->zdot];
    if (!force && first == s->zfirst && centre == s->zcentre && part == s->zpart) return;
    s->zfirst = first, s->zcentre = centre, s->zpart = part;
    for (int i = 0; i < 64; i++) d[i] = (unsigned char)seq_mel_frame(q, first + i / SEQ_STEPS, i % SEQ_STEPS);   /* 0 past the loop */
    for (int i = 0; i < SEQ_STEPS; i++) d[64 + i] = (unsigned char)seq_mel_frame(q, centre, i);
    if (!memcmp(d, s->zdot, sizeof d)) return;
    memcpy(s->zdot, d, sizeof d);
    s->seq.display_rev++;
}
/* jkbar and the dots: made in set_param (force) and song_pos. The dots are PHRASE's (and its picker's and editor's): made only while it
 * shows, so on JAM (or another tab after JAM) they stay still, an edit there bumping no display_rev (as 0.19.1's off-page cells); the tap
 * that shows PHRASE makes them */
static void mel_shown_now(inst_t *s, int force) {
    s->jkbar = mel_shown_bar(&s->seq);
    if (seq_mmode(&s->seq)) mel_dots_now(s, force);
}

/* 0.20 the MELODY tab's PHRASE page (header comment, 0.20; Task 8) on the playing part's JAM Melody lane: its strip pages by bars
 * (seq_strip_by_bars(TAB_MEL) 1), 4 a page; the centre is a bar (mel_centre). While the tab shows JAM, its polled cells read the strip
 * page and centre as PHRASE last showed them (zfirst, zcentre: mel_dots_now), so they stay still off their page (as 0.19.1's BASS page);
 * on a lane that isn't JAM (an old project's) nothing is lit and the readouts that read JAM settings are blank. The messages go on jmsg */
static void mknob_msg(seq_t *q, int f);
static int mel_txn(const seq_t *q) { return seq_mview(q) == MVIEW_PICK || seq_mview(q) == MVIEW_EDIT; }   /* the picker or the editor */
static int mstrip_first(const seq_t *q) { return seq_strip_page(q, TAB_MEL) * 4; }   /* the strip page's first bar */
static int wshown_first(const seq_t *q) { return seq_mmode(q) ? mstrip_first(q) : inst_of(q)->zfirst; }
static int wshown_centre(const seq_t *q) { return seq_mmode(q) ? mel_centre(q) : clampi(inst_of(q)->zcentre, 0, seq_bars(q) - 1); }
static void msg_bar(seq_t *q, const char *a, int bar, const char *b) {   /* jmsg: a, bar's number, b */
    txt_t m = mmsg_new(q);
    add(&m, a), add_int(&m, bar + 1), add(&m, b);
}
/* The editor on bar (FOLLOW off: it becomes the centre); inert while the picker or the editor is open */
static void medit_at(seq_t *q, int bar) {
    int live = mel_live(q);
    if (bar < 0 || bar >= seq_bars(q) || mel_txn(q)) return;
    if (!seq_follow(q)) seq_set_centre(q, TAB_MEL, bar);
    seq_medit_open(q, bar);
    if (seq_medit_bar(q) != bar) return;
    msg_bar(q, live ? "FOLLOW paused while you edit bar " : "Editing bar ", bar, ": tap a step, then a key");
}

/* wst_ the strip page's 4 bars: "2 \xC2\xB7 ANSWER \xC2\xB7 LOCKED \xC2\xB7 own \xC2\xB7 2 fixed \xC2\xB7 G" (the role in the bar's FORM;
 * own: a NEW BAR line), shorter until it fits CST_CHARS (as ust_: the chords shortened, then left out, then the fixed count, then own; past
 * that a locked bar's LOCKED in place of its role); blank and inert past the loop; lit = the centre (the open bar while editing), on
 * PHRASE, on a JAM lane; a tap opens the bar editor on it (FOLLOW off: it becomes the centre) */
static int wst_on(const seq_t *q, int i) {
    int bar = mstrip_first(q) + i;
    return mel_jam_on(q) && seq_mmode(q) && bar < seq_bars(q) && bar == mel_centre(q);
}
static void wst_tap(seq_t *q, int i, int v) { (void)v; medit_at(q, mstrip_first(q) + i); }
static int wst_label(const seq_t *q, int i, char *buf, int len) {
    static const unsigned char FORM[6][4] = {{1, 1, 1, 1}, {1, 1, 1, 2}, {1, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}};   /* role, own,
                                                                                                         * fixed, chords (2: the first) */
    const inst_t *s = inst_of(q);
    int bar = wshown_first(q) + i, jam = mel_jam_on(q), locked, own, nf, n;
    const char *ch;
    char first[sizeof s->rbar_text[0][0]];
    if (bar >= seq_bars(q)) return put(buf, len, " ");
    locked = jam && ppart(q)->mjam.nlock >> bar & 1, own = jam && ppart(q)->lane[LANE_MEL_UP].bar_var[bar];
    nf = seq_mel_fixed_count(q, bar), ch = s->rbar_text[seq_part(q)][bar], n = s->rbar_n[seq_part(q)][bar];
    first_chord(ch, first);
    for (int f = 0; f < 6; f++) {
        txt_t t = {buf, len, 0};
        add_int(&t, bar + 1);
        if (jam && (FORM[f][0] || !locked)) add(&t, " \xC2\xB7 "), add(&t, mel_role_word(q, bar));
        if (locked) add(&t, " \xC2\xB7 LOCKED");
        if (own && FORM[f][1]) add(&t, " \xC2\xB7 own");
        if (nf && FORM[f][2]) add(&t, " \xC2\xB7 "), add_int(&t, nf), add(&t, " fixed");
        if (FORM[f][3]) add(&t, " \xC2\xB7 "), add(&t, FORM[f][3] == 1 ? ch : first);
        if (FORM[f][3] == 2 && n > 1) add(&t, "+"), add_int(&t, n - 1);
        if (theory_text_width(buf) <= CST_CHARS || f == 5) return t.n;
    }
    return 0;
}
/* wpg_, wund_, wbtp_, wct_: the strip chrome and the centre (tb_*) */
#define TB_MEL(w, tap, label) \
    static void w##_tap(seq_t *q, int i, int v) { tap(q, TAB_MEL, i, v); } \
    static int w##_label(const seq_t *q, int i, char *buf, int len) { return label(q, TAB_MEL, i, buf, len); }
TB_MEL(wpg, tb_pg_tap, tb_pg_label)
TB_MEL(wund, tb_und_tap, tb_und_label)
TB_MEL(wbtp, tb_btp_tap, tb_btp_label)
TB_MEL(wct, tb_ct_tap, tb_ct_label)
/* wpage "BARS 1-4" ("BAR 5" for one), wpgn "phrase 1 of 4": the strip page's bars and which page */
static int wpage_text(const seq_t *q, char *buf, int len) {
    int from = wshown_first(q) + 1, to = clampi(from + 3, from, seq_bars(q));
    txt_t t = {buf, len, 0};
    add(&t, from == to ? "BAR " : "BARS "), add_int(&t, from);
    if (from != to) add(&t, "-"), add_int(&t, to);
    return t.n;
}
static int wpgn_text(const seq_t *q, char *buf, int len) {
    txt_t t = {buf, len, 0};
    add(&t, "phrase "), add_int(&t, wshown_first(q) / 4 + 1), add(&t, " of "), add_int(&t, (seq_bars(q) + 3) / 4);
    return t.n;
}

/* The left panel: wfm_ FORM, wan_ ANSWER (lit = the part's, on a JAM lane; a tap sets it, the knob's long text on jmsg; inert while the
 * picker or the editor is open), their lines wfmt and want, ANSWER PLAYS wsw_ (<, the SWAP block, >: with ANSWER SWAP the field opens the
 * picker and the arrows step the SWAP block through the 30, skipping the call's own; else the field reads "pick SWAP first" and a tap says
 * so), wsum the JAM knobs 3-5 (0.21: DENSITY, RANGE, VARIATION, SWING) */
static const char *const FORM_LINE[MF_COUNT] = {"LOOP: every bar plays the block, nothing answers", "1 + 1: a 1-bar call, a 1-bar answer, twice",
                                                "2 + 2: bars 1-2 call, bars 3-4 answer them", "3 + 1: three bars call, bar 4 turns it around",
                                                "AAB: say it, say it again, then answer (blues)", "SENTENCE: idea, idea a step up, break it up, close"};
static const char *const ANS_LINE[MA_COUNT] = {"ECHO: the call again; only its ending changes", "SHIFT: the call's shape, a 3rd higher",
                                               "MIRROR: the call upside down (up becomes down)", "RHYTHM: the call's rhythm, new chord-tone notes",
                                               "TAIL: the call's second half, twice, then home", "SWAP: another JAM block answers (the call keeps its own)"};
static int mjs_on(const seq_t *q, int f, int i) { return mel_jam_on(q) && seq_mjam_get(q, f) == i; }
static void mjs_tap(seq_t *q, int f, int i) {
    if (mel_txn(q)) return;
    seq_mjam_set(q, f, i);
    if (mel_jam_on(q)) mknob_msg(q, f);
}
static int wfm_on(const seq_t *q, int i) { return mjs_on(q, MJS_FORM, i); }
static void wfm_tap(seq_t *q, int i, int v) { (void)v; mjs_tap(q, MJS_FORM, i); }
static int wfm_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, mjam_form_name(i)); }
static int wan_on(const seq_t *q, int i) { return mjs_on(q, MJS_ANS, i); }
static void wan_tap(seq_t *q, int i, int v) { (void)v; mjs_tap(q, MJS_ANS, i); }
static int wan_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, mjam_answer_name(i)); }
static int wfmt_text(const seq_t *q, char *buf, int len) {
    return put(buf, len, mel_jam_on(q) ? FORM_LINE[clampi(seq_mjam_get(q, MJS_FORM), 0, MF_COUNT - 1)] : " ");
}
static int want_text(const seq_t *q, char *buf, int len) {
    if (!mel_jam_on(q)) return put(buf, len, " ");
    if (seq_mjam_get(q, MJS_FORM) == MF_LOOP) return put(buf, len, "LOOP has no answers: pick a FORM to hear one");
    return put(buf, len, ANS_LINE[clampi(seq_mjam_get(q, MJS_ANS), 0, MA_COUNT - 1)]);
}
static int mel_swap_on(const seq_t *q) { return mel_jam_on(q) && seq_mjam_get(q, MJS_ANS) == MA_SWAP; }   /* ANSWER SWAP on a JAM lane */
static void wsw_tap(seq_t *q, int i, int v) {
    const mjam_set_t *m = &ppart(q)->mjam.set;
    int n = MJ_COLS * MJ_LEVELS, k = m->sc * MJ_LEVELS + m->sl, call = m->c * MJ_LEVELS + m->l;
    txt_t t;
    (void)v;
    if (mel_txn(q)) return;
    if (!mel_swap_on(q)) {
        t = mmsg_new(q);
        add(&t, "Pick ANSWER SWAP first.");
        return;
    }
    if (i == 1) {
        seq_mpick_open(q);
        mmsg_new(q);
        return;
    }
    do k = pmod(k + (i ? 1 : -1), n); while (k == call);
    seq_mel_swap(q, k / MJ_LEVELS, k % MJ_LEVELS);
    t = mmsg_new(q);
    add(&t, "The answer bars play "), add_block(&t, k / MJ_LEVELS, k % MJ_LEVELS), add(&t, ".");
}
static int wsw_label(const seq_t *q, int i, char *buf, int len) {
    const mjam_set_t *m = &ppart(q)->mjam.set;
    txt_t t = {buf, len, 0};
    if (i != 1) return arrow_label(i == 2, buf, len);
    if (!mel_swap_on(q)) return put(buf, len, "pick SWAP first");
    add_block(&t, m->sc, m->sl);
    return t.n;
}
/* 0.21 wsum "JAM: DENSITY AS DRAWN \xC2\xB7 RANGE WIDE \xC2\xB7 VAR OFF \xC2\xB7 SWING 58%" (SWING without its grid, as its weak text), without
 * "JAM: " past WSUM_CHARS. The spec's "On JAM: DENSITY .. \xC2\xB7 VARIATION .. \xC2\xB7 SWING 58%." is 70 characters at the defaults, past
 * the 63 bytes of a value text (PARAM_TEXT_MAX) and the 61 characters of its box: VARIATION is VAR, "On" and the period go */
#define WSUM_CHARS 60   /* its box's 61 characters at 22 px, and 63 bytes with its three middle dots (2 bytes each) */
static int wsum_text(const seq_t *q, char *buf, int len) {
    char v[96];
    txt_t t = {v, sizeof v, 0};
    if (!mel_jam_on(q)) return put(buf, len, " ");
    add(&t, "JAM: DENSITY "), add(&t, mjam_dens_name(seq_mjam_get(q, MJS_DENS)));
    add(&t, " \xC2\xB7 RANGE "), add(&t, mjam_rng_name(seq_mjam_get(q, MJS_RNG)));
    add(&t, " \xC2\xB7 VAR "), add(&t, mjam_var_name(seq_mjam_get(q, MJS_VAR)));
    add(&t, " \xC2\xB7 SWING "), add_int(&t, 50 + 4 * seq_mel_swing(q)), add(&t, "%");
    return put(buf, len, theory_text_width(v) <= WSUM_CHARS ? v : v + 5);
}

/* The centre (its bar: wshown_centre): wcwh "PLAYING \xC2\xB7 BAR 2 \xC2\xB7 G \xC2\xB7 tap to change" (PLAYING while FOLLOW runs and
 * plays, on PHRASE; " \xC2\xB7 LOCKED"; tap to change with neither the picker nor the editor open), wcrl its role ("ANSWER \xC2\xB7 ECHO of
 * bar 1", "ANSWER \xC2\xB7 SWAP: RUN \xC2\xB7 RUN-UP", "CALL \xC2\xB7 HOOK \xC2\xB7 MOTIF", "LOOP \xC2\xB7 ARP \xC2\xB7 UP", "STEP UP \xC2\xB7 from
 * bar 1": the bar's settings, frozen when locked; a SWAP block equal to the call's plays as ECHO), wcend where it ends ("ends on E4 = 1
 * (home) \xC2\xB7 colourful": the last sounding note, its scale degree, home on 1, open on 5 or 2, the bar's tension word as BASS's; "no
 * notes") */
static int wcwh_text(const seq_t *q, char *buf, int len) {
    const inst_t *s = inst_of(q);
    int bar = wshown_centre(q);
    txt_t t = {buf, len, 0};
    if (seq_mmode(q) && mel_live(q) && !mel_txn(q)) add(&t, "PLAYING \xC2\xB7 ");
    add(&t, "BAR "), add_int(&t, bar + 1), add(&t, " \xC2\xB7 "), add(&t, s->rbar_text[seq_part(q)][bar]);
    if (mel_jam_on(q) && ppart(q)->mjam.nlock >> bar & 1) add(&t, " \xC2\xB7 LOCKED");
    if (!mel_txn(q)) add(&t, " \xC2\xB7 tap to change");
    return t.n;
}
static int wcrl_text(const seq_t *q, char *buf, int len) {
    int bar = wshown_centre(q), pos = bar % 4, src, r;
    const mjam_set_t *m = mel_set_of(q, bar);
    txt_t t = {buf, len, 0};
    if (!mel_jam_on(q)) return put(buf, len, " ");
    r = mjam_role(m->form, pos, &src);
    src += bar - pos;
    if (r == 'A') {
        add(&t, "ANSWER \xC2\xB7 ");
        if (m->ans == MA_SWAP && (m->sc != m->c || m->sl != m->l)) add(&t, "SWAP: "), add_block(&t, m->sc, m->sl);
        else add(&t, mjam_answer_name(m->ans == MA_SWAP ? MA_ECHO : m->ans)), add(&t, " of bar "), add_int(&t, src + 1);
    } else if (r == 'C') {
        add(&t, m->form == MF_LOOP ? "LOOP \xC2\xB7 " : "CALL \xC2\xB7 "), add_block(&t, m->c, m->l);
    } else {
        add(&t, r == 'R' ? "AGAIN" : r == 'S' ? "STEP UP" : "BREAK UP"), add(&t, " \xC2\xB7 from bar "), add_int(&t, src + 1);
    }
    return t.n;
}
static int wcend_text(const seq_t *q, char *buf, int len) {
    const inst_t *s = inst_of(q);
    int bar = wshown_centre(q), last = -1, deg;
    mjam_note_t l[SEQ_STEPS];
    txt_t t = {buf, len, 0};
    char d[2] = {0, 0};
    if (!mel_jam_on(q)) return put(buf, len, " ");
    seq_mel_line(q, bar, l);
    for (int i = 0; i < SEQ_STEPS; i++)
        if (l[i].note) last = i;
    if (last < 0) return put(buf, len, "no notes");
    deg = s->pcdeg[l[last].note % 12], d[0] = (char)('0' + deg);
    add(&t, "ends on "), add_note(&t, l[last].note);
    if (deg) add(&t, " = "), add(&t, d);   /* a chromatic fix has no scale degree */
    add(&t, deg == 1 ? " (home)" : deg == 5 || deg == 2 ? " (open)" : "");
    add(&t, " \xC2\xB7 "), add(&t, bass_level_word(bass_level(seq_mel_score(q, bar))));
    return t.n;
}

/* The ANSWER BLOCK picker (seq_mpick_*; blank and inert while it is closed): wpk_ the 30 blocks, column by column as jbk_ ("MOTIF  CALL" the
 * call's own, which does nothing; "RUN-UP  ANSWER" the SWAP block, lit; else the level, "TURN  2"; a tap tries it at once), wpc_ CANCEL,
 * wpd_ DONE, wpnt what a tap does */
static int mel_picking(const seq_t *q) { return seq_mview(q) == MVIEW_PICK; }
static int wpk_call(const seq_t *q, int i) { const mjam_set_t *m = &ppart(q)->mjam.set; return m->c * MJ_LEVELS + m->l == i; }
static int wpk_on(const seq_t *q, int i) {
    const mjam_set_t *m = &ppart(q)->mjam.set;
    return mel_picking(q) && m->sc * MJ_LEVELS + m->sl == i && !wpk_call(q, i);
}
static void wpk_tap(seq_t *q, int i, int v) {
    txt_t t;
    (void)v;
    if (!mel_picking(q) || wpk_call(q, i)) return;
    seq_mpick_try(q, i / MJ_LEVELS, i % MJ_LEVELS);
    t = mmsg_new(q);
    add(&t, "The answer bars play "), add_block(&t, i / MJ_LEVELS, i % MJ_LEVELS), add(&t, ".");
}
static int wpk_label(const seq_t *q, int i, char *buf, int len) {
    txt_t t = {buf, len, 0};
    if (!mel_picking(q)) return put(buf, len, " ");
    add(&t, mjam_block_name(i / MJ_LEVELS, i % MJ_LEVELS));
    if (wpk_call(q, i)) add(&t, "  CALL");
    else if (wpk_on(q, i)) add(&t, "  ANSWER");
    else add(&t, "  "), add_int(&t, i % MJ_LEVELS + 1);
    return t.n;
}
static void wpc_tap(seq_t *q, int i, int v) {
    txt_t t;
    (void)i, (void)v;
    if (!mel_picking(q)) return;
    seq_mpick_cancel(q);
    t = mmsg_new(q);
    add(&t, "Cancelled: back as it was.");
}
static int wpc_label(const seq_t *q, int i, char *buf, int len) { (void)i; return put(buf, len, mel_picking(q) ? "CANCEL" : " "); }
static void wpd_tap(seq_t *q, int i, int v) {
    const mjam_set_t *m = &ppart(q)->mjam.set;
    txt_t t;
    (void)i, (void)v;
    if (!mel_picking(q)) return;
    seq_mpick_done(q);
    t = mmsg_new(q);
    add(&t, "Kept "), add_block(&t, m->sc, m->sl), add(&t, " as the answer.");
}
static int wpd_label(const seq_t *q, int i, char *buf, int len) { (void)i; return put(buf, len, mel_picking(q) ? "DONE" : " "); }
static int wpnt_text(const seq_t *q, char *buf, int len) {   /* at most 63 bytes: the longest block name is 18 */
    const mjam_set_t *m = &ppart(q)->mjam.set;
    txt_t t = {buf, len, 0};
    if (!mel_picking(q)) return put(buf, len, " ");
    add(&t, "Tap a block for the answers. The call keeps "), add_block(&t, m->c, m->l), add(&t, ".");
    return t.n;
}

/* The bar editor (seq_medit_*; every cell but the piano and the octaves blank and inert while it is closed): wea_ NEW BAR, RESET BAR,
 * LOCK / UNLOCK, CANCEL, DONE; wes_ the bar's 16 steps ("B4 7": the note and its scale degree; "-" held, "\xC2\xB7" a rest; "*" after the
 * note when fixed; lit = selected; the beat labels are the skin's); wnk_ the note keys in two rows, C# D# F# G# A# then C..B (bnk_'s labels,
 * "E 1"; lit = the selected step's; a tap fixes it there, a rest step too, and plays it); wno_ the editor's octave 3..7; wrh_ REST, HOLD
 * (the selected step fixed as a rest, or as holding the note before it). wett its title, went its note line */
static const char *const WEA_NAME[5] = {"NEW BAR", "RESET BAR", "LOCK", "CANCEL", "DONE"};
static int wbar_locked(const seq_t *q, int bar) { return ppart(q)->mjam.nlock >> bar & 1; }
static int wea_label(const seq_t *q, int i, char *buf, int len) {
    int bar = seq_medit_bar(q);
    if (bar < 0) return put(buf, len, " ");
    return put(buf, len, i == 2 && wbar_locked(q, bar) ? "UNLOCK" : WEA_NAME[i]);
}
static void wea_tap(seq_t *q, int i, int v) {
    int bar = seq_medit_bar(q), live = seq_follow(q) && seq_cur_step(q) >= 0;   /* FOLLOW runs and plays: the editor's own pause ends */
    (void)v;
    if (bar < 0) return;
    if (i == 0) {
        if (seq_medit_new_bar(q)) msg_bar(q, "NEW BAR: bar ", bar, " gets its own line from the same block.");
        else msg_bar(q, "Bar ", bar, " has no other line that differs: nothing changed");
    } else if (i == 1) {
        seq_medit_reset_bar(q);
        msg_bar(q, "RESET BAR: bar ", bar, " follows JAM and PHRASE again.");
    } else if (i == 2) {
        seq_medit_lock(q);
        msg_bar(q, "Bar ", bar, wbar_locked(q, bar) ? " locked: JAM blocks and knobs leave it alone." : " follows JAM again.");
    } else if (i == 3) {
        seq_medit_cancel(q);
        msg_bar(q, "Cancelled: bar ", bar, live ? " is back as it was. FOLLOW restarts." : " is back as it was.");
    } else {
        seq_medit_done(q);
        msg_bar(q, "Kept bar ", bar, live ? ". FOLLOW restarts." : ".");
    }
}
static const mjam_note_t *wes_at(const seq_t *q, int bar, int i) { return &ppart(q)->mline[bar * SEQ_STEPS + i]; }   /* render_mel's */
static int wes_on(const seq_t *q, int i) { return seq_medit_sel(q) == i; }
static void wes_tap(seq_t *q, int i, int v) {
    int bar = seq_medit_bar(q);
    const mjam_note_t *x;
    txt_t m;
    (void)v;
    if (bar < 0 || !seq_medit_select(q, i)) return;
    x = wes_at(q, bar, i);
    m = mmsg_new(q);
    add(&m, "Step "), add_int(&m, i + 1);
    if (x->note) add(&m, ": "), add_note(&m, x->note), add(&m, ". Tap a key to change it, or REST.");
    else add(&m, x->hold ? " holds the note before. Tap a key for a note here." : ": a rest. Tap a key to play a note here.");
}
static int wes_label(const seq_t *q, int i, char *buf, int len) {
    int bar = seq_medit_bar(q), deg;
    const mjam_note_t *x;
    txt_t t = {buf, len, 0};
    if (bar < 0) return put(buf, len, " ");
    x = wes_at(q, bar, i);
    if (x->note) add_note(&t, x->note);
    else add(&t, x->hold ? "-" : "\xC2\xB7");
    if (ppart(q)->mjam.fix[bar * SEQ_STEPS + i]) add(&t, "*");
    if (x->note && (deg = inst_of(q)->pcdeg[x->note % 12]) != 0) add(&t, " "), add_int(&t, deg);
    return t.n;
}
static int wnk_on(const seq_t *q, int i) {
    int bar = seq_medit_bar(q), sel = seq_medit_sel(q), n = bar < 0 || sel < 0 ? 0 : wes_at(q, bar, sel)->note;
    return n && n % 12 == UNK_PC[i];
}
/* an image button (TAPBL, select 1), as unk_: its press's 1, once (tap_latch) */
static void wnk_tap(seq_t *q, int i, int v) {
    int bar = seq_medit_bar(q), sel = seq_medit_sel(q);
    txt_t m;
    (void)v;
    if (bar < 0 || sel < 0) return;
    seq_medit_note(q, UNK_PC[i]);
    m = mmsg_new(q);
    add(&m, "Bar "), add_int(&m, bar + 1), add(&m, ", step "), add_int(&m, sel + 1), add(&m, ": "), add_note(&m, wes_at(q, bar, sel)->note);
    add(&m, ". JAM blocks and knobs leave it alone.");
}
static int wnk_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->bnk_text[UNK_PC[i]]); }
static int wno_on(const seq_t *q, int i) { return seq_medit_bar(q) >= 0 && seq_medit_oct(q) == i + 3; }
static void wno_tap(seq_t *q, int i, int v) { (void)v; seq_medit_set_oct(q, i + 3); }
static int wno_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, MNO_NAMES[i]); }
static void wrh_tap(seq_t *q, int i, int v) {
    int sel = seq_medit_sel(q);
    txt_t m;
    (void)v;
    if (seq_medit_bar(q) < 0 || sel < 0) return;
    if (i) seq_medit_hold(q);
    else seq_medit_rest(q);
    m = mmsg_new(q);
    add(&m, "Step "), add_int(&m, sel + 1), add(&m, i ? " holds the note before it." : " is a rest now.");
}
static int wrh_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, seq_medit_bar(q) < 0 ? " " : i ? "HOLD" : "REST"); }
/* wett "EDITING \xC2\xB7 BAR 6 \xC2\xB7 C \xC2\xB7 ANSWER \xC2\xB7 own line" (own: a NEW BAR line), went its note line (the spec's, cut to
 * the wrapper's 63 bytes); " " closed */
static int wett_text(const seq_t *q, char *buf, int len) {
    int bar = seq_medit_bar(q);
    txt_t t = {buf, len, 0};
    if (bar < 0) return put(buf, len, " ");
    add(&t, "EDITING \xC2\xB7 BAR "), add_int(&t, bar + 1), add(&t, " \xC2\xB7 "), add(&t, inst_of(q)->rbar_text[seq_part(q)][bar]);
    add(&t, " \xC2\xB7 "), add(&t, mel_role_word(q, bar));
    if (ppart(q)->lane[LANE_MEL_UP].bar_var[bar]) add(&t, " \xC2\xB7 own line");
    return t.n;
}
static int went_text(const seq_t *q, char *buf, int len) {
    int bar = seq_medit_bar(q);
    if (bar < 0) return put(buf, len, " ");
    return put(buf, len, wbar_locked(q, bar) ? "LOCKED: JAM blocks and knobs leave this bar alone"
                                             : "Notes from JAM and PHRASE \xC2\xB7 * = fixed \xC2\xB7 a rest takes a note");
}
static int tb_fol_text(const seq_t *q, int tab, char *buf, int len);
static int msg_text(const seq_t *q, int m, char *buf, int len);
/* A readout of the PHRASE page, by its key: 0 for none */
static int w_readout(const seq_t *q, const char *key, char *buf, int len) {
    static const struct { const char *key; int (*text)(const seq_t *, char *, int); } W[] = {
        {"wpage", wpage_text}, {"wpgn", wpgn_text}, {"wfmt", wfmt_text}, {"want", want_text}, {"wsum", wsum_text}, {"wcwh", wcwh_text},
        {"wcrl", wcrl_text}, {"wcend", wcend_text}, {"wpnt", wpnt_text}, {"wett", wett_text}, {"went", went_text}};
    if (!strcmp(key, "wfol")) return tb_fol_text(q, TAB_MEL, buf, len);
    if (!strcmp(key, "wmsg")) return msg_text(q, MSG_MEL, buf, len);
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++)
        if (!strcmp(key, W[i].key)) return W[i].text(q, buf, len);
    return 0;
}

/* 0.22 (VST-29) the CHORDS and BASS JAM pages (header comment, 0.22) on the playing part's drawn Chords and Bass lanes: one set of functions
 * per drawn lane d (DL_*), the cells (cj... and uj...) thin wrappers. The message lines cjmsg, ujmsg: what the page's last action did, made
 * on the UI thread. The cells never light (the skin's meters under them show playing, NEXT and locked: jam_cells_now) */
static int dl_lane(int d) { return d == DL_BASS ? LANE_BASS : LANE_CHORDS; }
static int djl(int d) { return d == DL_BASS ? JL_BASS : JL_CHORDS; }
static txt_t djmsg_new(seq_t *q, int d) {
    inst_t *s = inst_rw(q);
    s->djmsg[d][0] = 0;
    s->msg_at[MSG_JAM + d] = s->frames;
    return (txt_t){s->djmsg[d], sizeof s->djmsg[d], 0};
}
static void add_jblock(txt_t *t, int d, int c, int l) {   /* "TECHNO \xC2\xB7 DUB STAB" */
    add(t, jam_genre_name(c)), add(t, " \xC2\xB7 "), add(t, jam_block_name(djl(d), c, l));
}
static int jam_is(const seq_t *q, int d, int c, int l, int queued) {   /* block (c, l) plays (queued 0) or waits (1) on lane d */
    int x, y;
    return (queued ? seq_jam_queued(q, dl_lane(d), &x, &y) : seq_jam_playing(q, dl_lane(d), &x, &y)) && x == c && y == l;
}
static int jam_shown(const seq_t *q, int lane, int *c, int *l) {   /* the block queued, else playing (1), else none (0; c, l 0) */
    *c = *l = 0;
    return seq_jam_queued(q, lane, c, l) || seq_jam_playing(q, lane, c, l);
}
/* A block tap (or knob 1's level, k1_set): it queues for the lane's SWITCH AT (stopped: at once; the playing block cancels a queue, or
 * over the lane's edits queues it again) and the message line says what happened: "TECHNO \xC2\xB7 DUB STAB takes over at bar 5", "Now ...:
 * stopped, it switches at once", "Cancelled: ... plays on", "Part switch queued: tap the block after it" (refused) */
static void jam_tap(seq_t *q, int d, int c, int l) {
    int lane = dl_lane(d), was = jam_is(q, d, c, l, 0), had = seq_jam_queued(q, lane, NULL, NULL), ed = was && seq_jam_edited(q, lane), at;
    txt_t m;
    if (!seq_lane_drawn(q, lane)) return;
    seq_jam_queue(q, lane, c, l);
    m = djmsg_new(q, d);
    if (jam_is(q, d, c, l, 1) && (at = seq_jam_at_bar(q, lane)) >= 0) add_jblock(&m, d, c, l), add(&m, " takes over at bar "), add_int(&m, at + 1);
    else if (jam_is(q, d, c, l, 0) && (!was || (ed && !seq_jam_edited(q, lane))))
        add(&m, "Now "), add_jblock(&m, d, c, l), add(&m, ": stopped, it switches at once");
    else if (was) add(&m, had ? "Cancelled: " : ""), add_jblock(&m, d, c, l), add(&m, " plays on");
    else add(&m, "Part switch queued: tap the block after it");
}
/* cjb_ / ujb_ the 30 blocks, column by column (column c, level l: cell c x 5 + l): "DUB STAB  >" playing, "OFFBEAT  NEXT" queued, else the
 * level "PAD  1"; a tap queues it (jam_tap). 0.22 skin: a name past 8 characters takes one space before NEXT ("SUB DRONE NEXT": 14
 * characters fill the 152 px block text at 24 px, tools/textfit.py) */
static void jb_tap(seq_t *q, int d, int i) { jam_tap(q, d, i / JAM_LEVELS, i % JAM_LEVELS); }
static int jb_label(const seq_t *q, int d, int i, char *buf, int len) {
    int c = i / JAM_LEVELS, l = i % JAM_LEVELS;
    txt_t t = {buf, len, 0};
    add(&t, jam_block_name(djl(d), c, l));
    if (jam_is(q, d, c, l, 0)) add(&t, "  >");
    else if (jam_is(q, d, c, l, 1)) add(&t, t.n > 8 ? " NEXT" : "  NEXT");
    else add(&t, "  "), add_int(&t, l + 1);
    return t.n;
}
/* cjp_ / ujp_ the playing phrase's 4 bars ("BAR 6 \xC2\xB7 F", the bar's first chord; blank and inert past the loop): a tap opens the bar on
 * the RHYTHM page (as MELODY's jph_ shows PHRASE) */
static int jp_bar(const seq_t *q, int i) { return seq_cur_bar(q) / 4 * 4 + i; }
static void jp_tap(seq_t *q, int d, int i) {
    int bar = jp_bar(q, i);
    if (bar >= seq_bars(q) || !seq_lane_drawn(q, dl_lane(d))) return;
    if (d == DL_BASS) seq_set_bview(q, BVIEW_RHYTHM);
    else set_mode(q, 1);
    rp_open_at(q, &RPAGE[d], bar);
}
static int jp_label(const seq_t *q, int i, char *buf, int len) {
    int bar = jp_bar(q, i);
    char first[sizeof inst_of(q)->rbar_text[0][0]];
    txt_t t = {buf, len, 0};
    if (bar >= seq_bars(q)) return put(buf, len, " ");
    first_chord(inst_of(q)->rbar_text[seq_part(q)][bar], first);
    add(&t, "BAR "), add_int(&t, bar + 1), add(&t, " \xC2\xB7 "), add(&t, first);
    return t.n;
}
/* cjsw_1 / ujsw_1 SWITCH AT ("SWITCH AT / BAR"): a tap cycles BEAT, BAR, 2 BARS, PHRASE (a queued block waits for the new boundary) */
static void jsw_d_tap(seq_t *q, int d) {
    int sw = (seq_djam_get(q, dl_lane(d), DJ_SW) + 1) % 4;
    txt_t m;
    seq_djam_set(q, dl_lane(d), DJ_SW, sw);
    m = djmsg_new(q, d);
    add(&m, "SWITCH AT "), add(&m, SW_NAME[sw]), add(&m, ": a tapped block waits for "), add(&m, SW_WAIT[sw]);
}
static int jsw_d_label(const seq_t *q, int d, char *buf, int len) {
    txt_t t = {buf, len, 0};
    add(&t, "SWITCH AT / "), add(&t, SW_NAME[seq_djam_get(q, dl_lane(d), DJ_SW) & 3]);
    return t.n;
}
#define JAM_CELLS(p, d) \
    static void p##b_tap(seq_t *q, int i, int v) { (void)v; jb_tap(q, d, i); } \
    static int p##b_label(const seq_t *q, int i, char *buf, int len) { return jb_label(q, d, i, buf, len); } \
    static void p##p_tap(seq_t *q, int i, int v) { (void)v; jp_tap(q, d, i); } \
    static int p##p_label(const seq_t *q, int i, char *buf, int len) { return jp_label(q, i, buf, len); } \
    static void p##sw_tap(seq_t *q, int i, int v) { (void)i, (void)v; jsw_d_tap(q, d); } \
    static int p##sw_label(const seq_t *q, int i, char *buf, int len) { (void)i; return jsw_d_label(q, d, buf, len); }
JAM_CELLS(cj, DL_CHORDS)
JAM_CELLS(uj, DL_BASS)
/* The readouts (unpolled: a tap, a knob turn, a block switch or a new bar changes them, jam_cells_now bumping display_rev). NOW PLAYING
 * "TECHNO \xC2\xB7 DUB STAB" with " (edited)" once the lane differs from it, the block's name alone past DJNOW_CHARS; "Tap a block to start"
 * before any. NEXT "NEXT \xC2\xB7 TECHNO \xC2\xB7 OFFBEAT \xC2\xB7 at bar 7" (the loop bar of its boundary), else "Tap a block to switch" */
#define DJNOW_CHARS 20   /* NOW PLAYING's box: 200 px at 22 px (vst/make_skin.py JNOW_W, MELODY's) */
static int jnow_d_text(const seq_t *q, int d, char *buf, int len) {
    int c, l, ed;
    txt_t t = {buf, len, 0};
    if (!seq_jam_playing(q, dl_lane(d), &c, &l)) return put(buf, len, "Tap a block to start");
    ed = seq_jam_edited(q, dl_lane(d));
    add_jblock(&t, d, c, l);
    if (ed) add(&t, " (edited)");
    if (theory_text_width(buf) <= DJNOW_CHARS) return t.n;
    t = (txt_t){buf, len, 0};
    add(&t, jam_block_name(djl(d), c, l));
    if (ed) add(&t, " (edited)");
    return t.n;
}
static int jnext_d_text(const seq_t *q, int d, char *buf, int len) {
    int c, l, at = seq_jam_at_bar(q, dl_lane(d));
    txt_t t = {buf, len, 0};
    if (at < 0 || !seq_jam_queued(q, dl_lane(d), &c, &l)) return put(buf, len, "Tap a block to switch");
    add(&t, "NEXT \xC2\xB7 "), add_jblock(&t, d, c, l), add(&t, " \xC2\xB7 at bar "), add_int(&t, at + 1);
    return t.n;
}
/* The meters (cjm_ / ujm_ the blocks: 0 plain, 1 playing, 2 NEXT; cjq_ / ujq_ the playing phrase's bars: 0 plain, 1 playing, 2 locked for
 * JAM, 3 locked and playing) and what NOW PLAYING and NEXT read, made after a set (force), a block switch (force) or when the part, the bar
 * or the transport changed (song_pos: at most once a bar), so the getters copy; a change bumps display_rev, the only way MPC re-reads them
 * (unpolled whole numbers and readouts) */
static int jam_locked_bar(const seq_t *q, int d, int bar) {   /* locked for JAM: its rhythm (the Bass: or its notes) */
    return seq_bar_locked(q, dl_lane(d), bar) || (d == DL_BASS && ppart(q)->bass.nlock >> bar & 1);
}
static void jam_cells_now(inst_t *s, int force) {
    const seq_t *q = &s->seq;
    int run = seq_cur_step(q) >= 0, key = seq_part(q) | seq_cur_bar(q) << 3 | run << 8, sig[DL_COUNT][7];
    unsigned char m[DL_COUNT][JM_N];
    if (!force && key == s->jkey) return;
    s->jkey = key;
    memset(m, 0, sizeof m);
    for (int d = 0; d < DL_COUNT; d++) {
        int lane = dl_lane(d), c = -1, l = -1, qc = -1, ql = -1, on = seq_jam_playing(q, lane, &c, &l), queued = seq_jam_queued(q, lane, &qc, &ql);
        if (on) m[d][c * JAM_LEVELS + l] = 1;
        if (queued) m[d][qc * JAM_LEVELS + ql] = 2;
        for (int i = 0; i < 4; i++) {
            int bar = jp_bar(q, i);
            if (bar < seq_bars(q)) m[d][JAM_COLS * JAM_LEVELS + i] = (unsigned char)(2 * jam_locked_bar(q, d, bar) + (run && bar == seq_cur_bar(q)));
        }
        int now[7] = {c, l, qc, ql, seq_jam_at_bar(q, lane), on && seq_jam_edited(q, lane), seq_part(q)};
        memcpy(sig[d], now, sizeof now);
    }
    if (!memcmp(m, s->jmeter, sizeof m) && !memcmp(sig, s->jsig, sizeof sig)) return;
    memcpy(s->jmeter, m, sizeof m);
    memcpy(s->jsig, sig, sizeof sig);
    s->seq.display_rev++;
}

/* 0.24 (VST-31) the CHORDS and BASS sound pages' status row and centre (spec "The CHORDS and BASS sound pages"; the boards V24Chords,
 * BassR3Page; header comment, 0.24), made by sound_cells_now after a set, a block or part switch and at a bar line, so the getters copy and
 * they change at most once a bar while playing. The centre's bar: the playing one while FOLLOW runs and plays (the tab not paused), else the
 * tab's centre (BASS bass_centre; CHORDS the bar its centre slot starts in); its chord the one on the bar's first step */
static const char *const VMODE_UPPER[VMODE_COUNT] = {"CLOSE", "SMOOTH", "TOP LINE", "ROOTLESS"};
static int sound_live(const seq_t *q, int d) { return d == DL_BASS ? bass_live(q) : rp_live(q, RP_C); }
static int sound_bar(const seq_t *q, int d) {
    if (d == DL_BASS) return bass_centre(q);
    if (sound_live(q, d)) return seq_cur_bar(q);
    return seq_centre_bar(q, TAB_CHORDS);   /* 0.24 Task 7 (ruling 18): the CHORDS centre as a bar */
}
static void sound_now(const seq_t *q, int d, char *buf, int len) {   /* NOW PLAYING "Lo-fi \xC2\xB7 Lo-fi Keys (edited)", as JAM's */
    int bass = d == DL_BASS, st = bass ? seq_bass_style(q) : seq_voicing_style(q), ed = bass ? seq_bass_edited(q) : seq_voicing_edited(q);
    const char *name;
    txt_t t = {buf, len, 0};
    if (st < 0 || st >= (bass ? BASS_STYLE_COUNT : VOICING_NSTYLES)) {
        put(buf, len, "Custom");
        return;
    }
    name = bass ? bass_style_name(st) : VOICING_STYLES[st].name;
    add(&t, bass ? bass_genre_name(bass_style_genre(st)) : VOICING_STYLES[st].genre), add(&t, " \xC2\xB7 "), add(&t, name);
    if (ed) add(&t, " (edited)");
    if (theory_text_width(buf) <= DJNOW_CHARS) return;
    t = (txt_t){buf, len, 0};
    add(&t, name);
    if (ed) add(&t, " (edited)");
}
static void sound_chord(const seq_t *q, int bar, txt_t *t) {   /* "VI \xC2\xB7 C": the chord on bar's first step */
    char num[16], name[16];
    int slot = seq_slot_at(q, bar, 0);
    seq_slot_numeral(q, slot, num, sizeof num), seq_slot_label(q, slot, name, sizeof name);
    add(t, num), add(t, " \xC2\xB7 "), add(t, name);
}
static void sound_chords_centre(const seq_t *q, int bar, sound_t *n) {   /* cct_1, csnt, cswd, cdrift */
    const seq_part_t *p = ppart(q);
    int slot = seq_slot_at(q, bar, 0), notes[THEORY_CHORD_MAX], k = seq_slot_notes(q, slot, bar, notes), part = seq_part(q), drift;
    char num[16];
    txt_t t = {n->nt[DL_CHORDS], sizeof n->nt[DL_CHORDS], 0};
    seq_slot_label(q, slot, n->name[DL_CHORDS], sizeof n->name[DL_CHORDS]);
    for (int i = 0; i < k; i++) {
        if (i) add(&t, " ");
        add_note(&t, notes[i]);
    }
    drift = theory_drift(q->key, q->scale, theory_slot_scale(q->scale, p->src[slot]), p->prog[slot], seq_part_eff_colour(q, part, slot),
                         seq_part_vknob_eff(q, part, VK_SPR) < 0, p->vinv);
    n->drift = theory_drift_level(drift);
    seq_slot_numeral(q, slot, num, sizeof num);
    t = (txt_t){n->wd[DL_CHORDS], sizeof n->wd[DL_CHORDS], 0};
    add(&t, num), add(&t, " \xC2\xB7 "), add(&t, DRIFT_WORD[drift5(drift)]), add(&t, " \xC2\xB7 "), add(&t, VMODE_UPPER[seq_vmode_eff(q)]);
}
static void sound_bass_centre(const inst_t *s, int bar, sound_t *n) {   /* uct_1, usnt, uswd */
    const seq_t *q = &s->seq;
    const seq_part_t *p = ppart(q);
    int hits = 0, level = bar_level(q, bar);
    txt_t t = {n->name[DL_BASS], sizeof n->name[DL_BASS], 0};
    add(&t, s->rbar_text[seq_part(q)][bar]);
    if (p->bass.set.pedal) add(&t, " over "), add(&t, theory_key_name(pedal_pc(q->key, q->scale, p->bass.set.pedal)));
    t = (txt_t){n->nt[DL_BASS], sizeof n->nt[DL_BASS], 0};
    for (int i = 0; i < SEQ_STEPS; i++) {   /* its notes in order as they play, the first 6 */
        int note = seq_abs_note(&p->lane[LANE_BASS].steps[bar * SEQ_STEPS + i]);
        if (!note) continue;
        if (++hits > 6) continue;
        if (hits > 1) add(&t, " ");
        add_note(&t, note);
    }
    add(&t, !hits ? "no notes" : hits > 6 ? " ..." : "");
    t = (txt_t){n->wd[DL_BASS], sizeof n->wd[DL_BASS], 0};
    add_int(&t, hits), add(&t, hits == 1 ? " note \xC2\xB7 " : " notes \xC2\xB7 "), add(&t, level ? bass_level_word(level) : "chord tones");
}
static void ph_dots(const seq_t *q, const rpage_t *P, int bar, unsigned char out[SEQ_STEPS]);
/* 0.24 final review (lever): the sound page of drawn lane d shows (its tab's view: CHORDS's Chords mode, BASS's sound view), as
 * ph_cells_now's P->paged. Off it, its texts stay as they are: a view switch is a set (force), which builds them on arrival */
static int sound_shown(const seq_t *q, int d) { return d == DL_BASS ? seq_bmode(q) == 0 : get_mode(q) == 0; }
/* lane d's texts of old kept in n (a page that does not show) */
static void sound_keep(sound_t *n, const sound_t *old, int d) {
    memcpy(n->now[d], old->now[d], sizeof n->now[d]), memcpy(n->bar[d], old->bar[d], sizeof n->bar[d]);
    memcpy(n->name[d], old->name[d], sizeof n->name[d]), memcpy(n->wh[d], old->wh[d], sizeof n->wh[d]);
    memcpy(n->nt[d], old->nt[d], sizeof n->nt[d]), memcpy(n->wd[d], old->wd[d], sizeof n->wd[d]);
    if (d == DL_BASS) memcpy(n->dots, old->dots, sizeof n->dots);
    else n->drift = old->drift;
}
static void sound_cells_now(inst_t *s, int force) {
    const seq_t *q = &s->seq;
    int shown = sound_shown(q, DL_CHORDS) | sound_shown(q, DL_BASS) << 1;
    int key = seq_part(q) | seq_cur_bar(q) << 3 | (seq_cur_step(q) >= 0) << 8 | sound_live(q, DL_CHORDS) << 9 | sound_live(q, DL_BASS) << 10 |
              shown << 11;
    sound_t n;
    if (!force && key == s->skey) return;
    s->skey = key;
    if (!shown && s->snd_made) return;   /* 0.24 final review: neither page shows: no work under the engine lock */
    memset(&n, 0, sizeof n);
    for (int d = 0; d < DL_COUNT; d++) {
        int bar = sound_bar(q, d);
        txt_t t = {n.wh[d], sizeof n.wh[d], 0};
        if (!(shown >> d & 1) && s->snd_made) {
            sound_keep(&n, &s->snd, d);
            continue;
        }
        sound_now(q, d, n.now[d], sizeof n.now[d]);
        for (int i = 0; i < 4; i++) {
            txt_t c = {n.bar[d][i], sizeof n.bar[d][i], 0};
            if (jp_bar(q, i) < seq_bars(q)) sound_chord(q, jp_bar(q, i), &c);
            else add(&c, " ");
        }
        add(&t, sound_live(q, d) ? "PLAYING \xC2\xB7 BAR " : "BAR "), add_int(&t, bar + 1), add(&t, " \xC2\xB7 tap to edit on PHRASE");
        if (d == DL_BASS) sound_bass_centre(s, bar, &n), ph_dots(q, RP_B, bar, n.dots);   /* Task 8 fix: the centre's dots (usz_) */
        else sound_chords_centre(q, bar, &n);
    }
    s->snd_made = 1;   /* both lanes made once (create's call): from now on a page that does not show keeps its texts */
    if (!memcmp(&n, &s->snd, sizeof n)) return;
    s->snd = n;
    s->seq.display_rev++;
}
/* The cells: csb_ / usb_ the playing phrase's bars "BAR 6" (lit while it plays; blank and inert past the loop; a tap shows PHRASE with that bar
 * on its strip as the centre, not opened: seq_strip_show_bar, FOLLOW paused as paging does), csc_ / usc_ their chords (display only),
 * cse_1 / use_1 EDIT BARS (PHRASE) */
static int sb_on(const seq_t *q, int i) { return seq_cur_step(q) >= 0 && jp_bar(q, i) == seq_cur_bar(q); }
static int sb_label(const seq_t *q, int i, char *buf, int len) {
    int bar = jp_bar(q, i);
    txt_t t = {buf, len, 0};
    if (bar >= seq_bars(q)) return put(buf, len, " ");
    add(&t, "BAR "), add_int(&t, bar + 1);
    return t.n;
}
static void sb_tap(seq_t *q, int d, int i) {
    int bar = jp_bar(q, i);
    if (bar >= seq_bars(q)) return;
    sound_phrase(q, d);
    seq_strip_show_bar(q, d == DL_BASS ? TAB_BASS : TAB_CHORDS, bar);
}
#define SOUND_CELLS(p, d) \
    static void p##b_tap(seq_t *q, int i, int v) { (void)v; sb_tap(q, d, i); } \
    static int p##b_label(const seq_t *q, int i, char *buf, int len) { return sb_label(q, i, buf, len); } \
    static void p##c_tap(seq_t *q, int i, int v) { no_tap(q, i, v); } \
    static int p##c_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->snd.bar[d][i]); } \
    static void p##e_tap(seq_t *q, int i, int v) { (void)i, (void)v; sound_phrase(q, d); } \
    static int p##e_label(const seq_t *q, int i, char *buf, int len) { (void)q, (void)i; return put(buf, len, "EDIT BARS"); }
SOUND_CELLS(cs, DL_CHORDS)
SOUND_CELLS(us, DL_BASS)

/* 0.24 (VST-31, Task 7) the PHRASE pages of CHORDS (cq... cells) and BASS (uq...): spec "The PHRASE page", "The bar editor", "COPY TO ALL
 * and COPY TO...", "Dots"; the boards ChPhr2Page / Edit / Lock, BassPhr2Page / Edit / Lock, ChPhrCopy, BassPhrCopy. One set of functions
 * (ph_*) on each lane's row of RPAGE (the 0.18 RHYTHM page's: PHRASE is its view), the cells thin wrappers (PH_CELL). The page's bar, the
 * centre: the open bar, else the tab's centre bar (seq_centre_bar, ruling 18: the playing bar while FOLLOW runs, else the bar shown or given
 * by hand). Every text and dot is made by ph_cells_now while the page shows (a set, a block switch, or in song_pos when the bars shown change:
 * at most once a bar), so off the page they stay still (0.19.1) and the getters copy; a change bumps display_rev. The messages go on the
 * page's rmsg (rp_msg_new), fading as today */
#define PH_INNER_CHARS 24   /* COPY TO...'s bar line, rcp_'s RCP_CHARS */
static const char *const PH_DEG[12] = {"1", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7"};   /* a note on its chord */
static const char *const PH_TYPE[5] = {"as knobs", "sus2", "sus4", "add9", "6"};
static const unsigned char PH_PAINT[5] = {RC_ACC, RC_HIT, RC_GHOST, RC_HOLD, RC_REST};   /* the paint row's order (spec) */
static const char *const PH_PAINT_NAME[7] = {"ACCENT", "HIT", "GHOST", "HOLD", "REST", "COPY TO ALL", "COPY TO..."};
static const char *paint_name(int code) {
    for (int i = 0; i < 4; i++)
        if (PH_PAINT[i] == code) return PH_PAINT_NAME[i];
    return "REST";
}
/* The 5 drift colours of a chord (green .. red, the legend's words: DRIFT_WORD, drift5) */
static int ph_drift(const seq_t *q, int slot) {
    const seq_part_t *p = ppart(q);
    int part = seq_part(q);
    return drift5(theory_drift(q->key, q->scale, theory_slot_scale(q->scale, p->src[slot]), p->prog[slot], seq_part_eff_colour(q, part, slot),
                               seq_part_vknob_eff(q, part, VK_SPR) < 0, p->vinv));
}
static int ph_root(const seq_t *q, int slot) {   /* the chord's root pitch class */
    const seq_part_t *p = ppart(q);
    return theory_degree_pc(q->key, theory_slot_scale(q->scale, p->src[slot]), p->prog[slot]);
}
static void ph_chord_text(const seq_t *q, int slot, char *buf, int len) {   /* a step's chord: degrees or names (NAMES) */
    if (seq_names(q)) seq_slot_label(q, slot, buf, len);
    else seq_slot_numeral(q, slot, buf, len);
}
static const char *ph_other(const rpage_t *P) { return P->dl == DL_BASS ? "chords" : "bass"; }
static int ph_centre(const seq_t *q, const rpage_t *P) {
    int bar = seq_rbar_bar(q, P->lane);
    return bar >= 0 ? bar : seq_centre_bar(q, P->tab);
}
static int ph_first(const seq_t *q, const rpage_t *P) { return seq_strip_page(q, P->tab) * 4; }
static int ph_editing(const seq_t *q, const rpage_t *P) { return seq_rbar_bar(q, P->lane) >= 0 && P->view(q) == P->bar; }   /* not COPY TO... */
static const phcells_t *phc(const seq_t *q, const rpage_t *P) { return &inst_of(q)->ph[P->dl]; }
static int ph_put(char *buf, int len, const char *s) { return put(buf, len, *s ? s : " "); }   /* a cache's text; " " before it is made */
static int rp_ctl_text(const seq_t *q, const rpage_t *P, char *buf, int len);
static int rp_cnote_text(const seq_t *q, const rpage_t *P, char *buf, int len);

/* The dots' frames, the one place each mapping lives (Task 8 may switch CHORDS to 0.18's text picture: here). CHORDS (ruling 1's 36): 0 a
 * rest; a hit 1 + height x 5 + colour (GHOST 0, HIT 1, ACCENT 2; its chord's drift colour 0..4), 21 + .. when it starts a chord (the
 * amber tick); a hold 16 + its hit's colour. BASS: MELODY's rule over E1..E4 (a note 1 + height x 5 + its tension colour, bass_level; a
 * hold 61 + its note's height; a rest 0), 73 + .. a ticked note */
static int chords_frame(int code, int colour, int tick) {
    if (code == RC_REST) return 0;
    if (code == RC_HOLD) return 16 + colour;
    return (tick ? 21 : 1) + (code == RC_GHOST ? 0 : code == RC_HIT ? 1 : 2) * 5 + colour;
}
static int bass_height(int m) { return m <= 28 ? 0 : clampi((22 * (m - 28) + 36) / 72, 0, 11); }   /* E1 .. E4, 12 heights (round) */
static int bass_frame(int code, int note, int colour, int tick) {   /* note: a hit's, a hold's hit's (0 none) */
    if (code == RC_HOLD) return note ? 61 + bass_height(note) : 61;
    if (code == RC_REST || !note) return 0;
    return (tick ? 73 : 1) + bass_height(note) * 5 + colour;
}

/* One bar as PHRASE shows it, read once: its codes as they play (a fill bar's fill) and as drawn, each step's flags (seq_step_flags) and, on
 * the Bass, each hit's note */
typedef struct { unsigned char o[SEQ_STEPS], d[SEQ_STEPS], f[SEQ_STEPS]; int note[SEQ_STEPS]; } phbar_t;
static void ph_bar(const seq_t *q, const rpage_t *P, int bar, phbar_t *b) {
    rp_out(q, P, bar, b->o);
    seq_bar_codes(q, P->lane, bar, b->d);
    for (int i = 0; i < SEQ_STEPS; i++) {
        b->f[i] = (unsigned char)seq_step_flags(q, P->lane, bar, i);
        b->note[i] = P->notes && rhythm_is_hit(b->o[i]) ? rp_note(q, P, bar, i) : 0;
    }
}
/* Where the chord step i of bar plays sits: the step itself; ANTICIPATE's early hit the change it ties over, ANTICIPATE's steps on (0.24
 * final review: as playback's step_slot reads it, the next bar's start past the bar line) */
static int ph_slot_of(const seq_t *q, int lane, int bar, int i, int flags, int *b2, int *s2) {
    *b2 = bar, *s2 = i;
    if (flags & SF_EARLY) {
        int j = (bar * SEQ_STEPS + i + seq_anticipate_steps(q, lane)) % (seq_bars(q) * SEQ_STEPS);
        *b2 = j / SEQ_STEPS, *s2 = j % SEQ_STEPS;
    }
    return seq_slot_at(q, *b2, *s2);
}
static int ph_tick(const seq_t *q, const phbar_t *b, int bar, int i) {   /* a hit that starts a chord (the amber mark) */
    return rhythm_is_hit(b->o[i]) && (seq_chord_change_at(q, bar, i) || (b->f[i] & SF_EARLY));
}
static void ph_dots(const seq_t *q, const rpage_t *P, int bar, unsigned char out[SEQ_STEPS]) {
    phbar_t b;
    int colour = 0, held = 0, last = -1;
    memset(out, 0, SEQ_STEPS);
    if (bar >= seq_bars(q)) return;
    ph_bar(q, P, bar, &b);
    for (int i = 0; i < SEQ_STEPS; i++) {
        int o = b.o[i], b2, s2, slot;
        if (rhythm_is_hit(o)) {
            slot = ph_slot_of(q, P->lane, bar, i, b.f[i], &b2, &s2);
            colour = P->dl == DL_BASS ? bass_level(seq_bass_tension_at(q, b2, s2, b.note[i])) : ph_drift(q, slot);
            held = b.note[i], last = i;
        } else if (o == RC_HOLD && last < 0) {   /* held over the bar line: the chord here */
            colour = P->dl == DL_BASS ? 0 : ph_drift(q, seq_slot_at(q, bar, i)), held = 0;
        }
        out[i] = (unsigned char)(P->dl == DL_BASS ? bass_frame(o, rhythm_is_hit(o) ? b.note[i] : held, colour, ph_tick(q, &b, bar, i))
                                                  : chords_frame(o, colour, ph_tick(q, &b, bar, i)));
    }
}

/* A strip cell's tag line: "6 \xC2\xB7 LOCKED \xC2\xB7 picked \xC2\xB7 own \xC2\xB7 1 change \xC2\xB7 1 fixed \xC2\xB7 fill" (the spec's order;
 * own: painted, or on the Bass a NEW BAR line; picked: a picked slot starts in it, not locked on this lane), shorter past CST_CHARS (the
 * strip's cells are MELODY's: the chords have a line of their own, ph_h) by leaving out own, picked, then the changes (the chord line
 * shows them), the fixed count, fill; LOCKED stays */
static int ph_own(const seq_t *q, const rpage_t *P, int bar) {
    int ed = 0;
    seq_bar_rhythm(q, P->lane, bar, &ed);
    return ed || (P->dl == DL_BASS && ppart(q)->lane[LANE_BASS].bar_var[bar]);
}
static void ph_strip_text(const seq_t *q, const rpage_t *P, int bar, char *buf, int len) {
    static const unsigned char DROP[5] = {2, 1, 3, 4, 5};   /* the tags left out first (their index below) */
    const char *tag[6] = {"LOCKED", "picked", "own", NULL, NULL, "fill"};
    char nc[16], nf[16];
    int c = seq_cp_count(q, bar), f = P->dl == DL_BASS ? seq_bass_fixed_count(q, bar) : 0;
    int on[6] = {jam_locked_bar(q, P->dl, bar), seq_bar_picked(q, P->lane, bar), ph_own(q, P, bar), c > 0, f > 0, rp_fill_tag(q, P, bar)};
    txt_t tc = {nc, sizeof nc, 0}, tf = {nf, sizeof nf, 0};
    add_int(&tc, c), add(&tc, c == 1 ? " change" : " changes"), tag[3] = nc;
    add_int(&tf, f), add(&tf, " fixed"), tag[4] = nf;
    for (int k = 0;; k++) {
        txt_t t = {buf, len, 0};
        add_int(&t, bar + 1);
        for (int i = 0; i < 6; i++)
            if (on[i]) add(&t, " \xC2\xB7 "), add(&t, tag[i]);
        if (theory_text_width(buf) <= CST_CHARS || k == 5) return;
        on[DROP[k]] = 0;
    }
}

/* The left panel's RHYTHM field (the centre bar's rhythm, "Push (edited)", or "with the bass" / "in the gaps of the bass" while LOCK WITH
 * drives it) and its line ("PUSH: 16th syncopation, short"), LOCK WITH's and ANTICIPATE's lines (the boards') */
static const char *const LW_LINE[DL_COUNT][3] = {
    {"OWN: the chords keep their own rhythm", "WITH BASS: the chords hit with the bass", "IN THE GAPS: the chords answer the bass"},
    {"OWN: the bass keeps its own rhythm", "WITH CHORDS: the bass hits with the chords", "IN THE GAPS: the bass answers the chords"}};
static const char *const AN_LINE[DL_COUNT][4] = {
    {"OFF: each chord comes in on its beat", "16TH: each new chord comes in a 16th early", "8TH: each new chord comes in an 8th early",
     "BEAT: each new chord comes in a beat early"},
    {"OFF: each root starts on its beat", "16TH: each new root comes in a 16th early", "8TH: each new root comes in an 8th early",
     "BEAT: each new root comes in a beat early"}};   /* Task 8: the board's (the left panel's 44 characters at 22 px) */
static const char *const AN_NAME[4] = {"OFF", "16TH", "8TH", "BEAT"};
static void add_upper(txt_t *t, const char *s) {
    for (; *s; s++) add(t, (char[2]){(char)(*s >= 'a' && *s <= 'z' ? *s - 'a' + 'A' : *s), 0});
}
static void ph_field_texts(const seq_t *q, const rpage_t *P, int bar, phcells_t *c) {
    int lw = seq_bar_follows(q, P->lane, bar), ed = 0, ri = seq_bar_rhythm(q, P->lane, bar, &ed);
    txt_t f = {c->field, sizeof c->field, 0}, l = {c->fline, sizeof c->fline, 0};
    const char *name = ri < 0 ? "Custom" : rhythm_lib_name(P->lib, ri);
    if (lw) {
        add(&f, lw == LW_WITH ? "with the " : "in the gaps of the "), add(&f, ph_other(P));
        add(&l, "Bar "), add_int(&l, bar + 1), add(&l, " follows the "), add(&l, ph_other(P)), add(&l, ": a pick sets OWN");
    } else {
        add(&f, name);
        if (ed) add_edited(&f, name);
        add_upper(&l, name), add(&l, ": "), add(&l, ri < 0 ? "drawn by hand" : rhythm_is_jam(ri) ? "a JAM block's rhythm" : rhythm_lib_feel(P->lib, ri));   /* a JAM one has no feel */
    }
    put(c->wline, sizeof c->wline, LW_LINE[P->dl][clampi(seq_lockwith(q, P->lane), 0, 2)]);
    put(c->aline, sizeof c->aline, AN_LINE[P->dl][clampi(seq_anticipate(q, P->lane), 0, 3)]);
}

/* The centre: where ("PLAYING \xC2\xB7 BAR 6 \xC2\xB7 tap to edit", " \xC2\xB7 LOCKED"), the bar's chords (CHORDS "VI \xC2\xB7 C  ->  III \xC2\xB7 G",
 * names alone past the box; BASS "C -> G \xC2\xB7 10 notes, 1 fixed"), where it ends (CHORDS "ends on G \xC2\xB7 top note D \xC2\xB7 close": the
 * chord at the last hit, the top note it plays (seq_part_step_notes: its octave, CADENCE's voicing), its drift word; BASS "ends on G2 =
 * 1 of G \xC2\xB7 chord tones": the last note, its degree on its chord, the bar's tension word). ASCII "->" (ruling 20: the arrow glyph is unverified on the Force) */
#define PH_CC_CHARS 38   /* Task 8: the centre's chord line, the ring's middle column at 22 px (vst/make_skin.py SR_MW) */
static void ph_centre_chords(const seq_t *q, const rpage_t *P, int bar, int notes, phcells_t *c) {   /* cc */
    int slots[SEQ_STEPS], n = seq_part_bar_changes(q, seq_part(q), bar, slots, NULL);   /* 0.24 final review: rbar_text's helper */
    char num[16], name[16];
    for (int form = P->dl == DL_BASS; form < 2; form++)   /* numerals and names, then (and on the Bass) names alone: the first that fit, */
        for (int keep = n; keep > (form ? 0 : n - 1); keep--) {   /* then " -> ..." */
            txt_t t = {c->cc, sizeof c->cc, 0};
            for (int k = 0; k < keep; k++) {
                seq_slot_numeral(q, slots[k], num, sizeof num), seq_slot_label(q, slots[k], name, sizeof name);
                if (k) add(&t, form ? " -> " : "  ->  ");
                if (!form) add(&t, num), add(&t, " \xC2\xB7 ");
                add(&t, name);
            }
            if (keep < n) add(&t, " -> ...");
            if (P->dl == DL_BASS) {
                add(&t, " \xC2\xB7 "), add_int(&t, notes), add(&t, notes == 1 ? " note" : " notes");
                if (seq_bass_fixed_count(q, bar)) add(&t, ", "), add_int(&t, seq_bass_fixed_count(q, bar)), add(&t, " fixed");
            }
            if (theory_text_width(c->cc) <= PH_CC_CHARS) return;
        }
}
static void ph_centre_end(const seq_t *q, const rpage_t *P, int bar, const phbar_t *b, int last, phcells_t *c) {   /* ce */
    int b2, s2, slot;
    char name[16];
    txt_t t = {c->ce, sizeof c->ce, 0};
    if (last < 0) {
        add(&t, P->dl == DL_BASS ? "no notes in this bar" : "no hits in this bar");
        return;
    }
    slot = ph_slot_of(q, P->lane, bar, last, b->f[last], &b2, &s2);
    add(&t, "ends on ");
    if (P->dl == DL_BASS) {
        int root = ph_root(q, slot), level = bar_level(q, bar);
        add_note(&t, b->note[last]), add(&t, " = "), add(&t, PH_DEG[pmod(b->note[last] - root, 12)]);
        add(&t, " of "), add(&t, theory_key_name(root)), add(&t, " \xC2\xB7 "), add(&t, level ? bass_level_word(level) : "chord tones");
    } else {   /* the notes the hit plays: its octave, an early hit's chord, CADENCE's voicing on a phrase's last bar */
        int v[THEORY_CHORD_MAX], k = seq_part_step_notes(q, seq_part(q), LANE_CHORDS, bar, last, v), top = 0;
        for (int i = 0; i < k; i++) top = v[i] > top ? v[i] : top;
        seq_slot_label(q, slot, name, sizeof name);
        add(&t, name);
        if (k) add(&t, " \xC2\xB7 top note "), add_note(&t, top);
        add(&t, " \xC2\xB7 "), add(&t, DRIFT_WORD[ph_drift(q, slot)]);
    }
}
static void ph_centre_texts(const seq_t *q, const rpage_t *P, int bar, const phbar_t *b, phcells_t *c) {
    int open = rp_open(q, P), last = -1, notes = 0;
    txt_t t = {c->cw, sizeof c->cw, 0};
    if (rp_live(q, P) && !open) add(&t, "PLAYING \xC2\xB7 ");
    add(&t, "BAR "), add_int(&t, bar + 1);
    if (jam_locked_bar(q, P->dl, bar)) add(&t, " \xC2\xB7 LOCKED");
    if (!open) add(&t, " \xC2\xB7 tap to edit");
    for (int i = 0; i < SEQ_STEPS; i++)
        if (rhythm_is_hit(b->o[i]) && (P->dl != DL_BASS || b->note[i])) last = i, notes++;
    ph_centre_chords(q, P, bar, notes, c);
    ph_centre_end(q, P, bar, b, last, c);
}

/* The editor's steps (the open bar): what plays (CHORDS: a hit's chord, a step that starts a chord its chord too; BASS: the note, "*" when
 * fixed; "-" a hold, "\xC2\xB7" a rest), the third line (EARLY, TIED, "cut" a drawn hit the knobs cut; CHORDS the paint, "+" when the knobs
 * add it, its octave "+1"; BASS the note's degree on its chord, " oct" an octave note, "+5" added), the marks (bit 0 a chord starts there:
 * the amber edge; bit 1 a hit the knobs add: dashed); the title "EDITING \xC2\xB7 BAR 6 \xC2\xB7 C\xC2\xB7G@9" (" \xC2\xB7 LOCKED") */
static void ph_step_texts(const seq_t *q, const rpage_t *P, int bar, const phbar_t *b, phcells_t *c) {
    const seq_part_t *p = ppart(q);
    const char *roles = seq_bass_roles(q, bar);
    char ch[40];
    txt_t t = {c->title, sizeof c->title, 0};
    seq_chord_label(q, bar, ch, sizeof ch);
    add(&t, "EDITING \xC2\xB7 BAR "), add_int(&t, bar + 1), add(&t, " \xC2\xB7 "), add(&t, ch);
    if (jam_locked_bar(q, P->dl, bar)) add(&t, " \xC2\xB7 LOCKED");
    for (int i = 0; i < SEQ_STEPS; i++) {
        int o = b->o[i], f = b->f[i], hit = rhythm_is_hit(o), change = seq_chord_change_at(q, bar, i), b2, s2;
        int slot = ph_slot_of(q, P->lane, bar, i, f, &b2, &s2);
        int fixed = P->dl == DL_BASS && p->bass.fix[bar * SEQ_STEPS + i], oct = seq_hit_oct(q, bar, i);
        txt_t w = {c->step[i], sizeof c->step[i], 0}, l = {c->line[i], sizeof c->line[i], 0};
        if (P->dl == DL_BASS && b->note[i]) add_note(&w, b->note[i]);
        else if (P->dl != DL_BASS && (hit || change)) ph_chord_text(q, slot, ch, sizeof ch), add(&w, ch);
        else add(&w, o == RC_HOLD ? "-" : "\xC2\xB7");
        if (fixed) add(&w, "*");
        if (f & SF_EARLY) add(&l, "EARLY");
        else if (f & SF_TIED) add(&l, "TIED");
        else if (rhythm_is_hit(b->d[i]) && !hit) add(&l, "cut");
        else if (P->dl == DL_BASS && b->note[i]) {
            if (f & SF_ADDED) add(&l, "+");
            add(&l, PH_DEG[pmod(b->note[i] - ph_root(q, slot), 12)]);
            if (roles[i] == 'o') add(&l, " oct");
        } else if (P->dl != DL_BASS && (hit || o == RC_HOLD)) {
            if (hit && (f & SF_ADDED)) add(&l, "+");
            add(&l, o == RC_HOLD ? "HOLD" : o == RC_ACC ? "ACC" : o == RC_GHOST ? "GHOST" : "HIT");
            if (hit && oct) add(&l, oct > 0 ? " +" : " -"), add_int(&l, oct < 0 ? -oct : oct);
        } else add(&l, " ");
        c->mark[i] = (unsigned char)(change | (hit && (f & SF_ADDED) ? 2 : 0));
    }
}

/* The BASS piano's 12 keys (UNK_PC's order): "D 5", each key's degree on the chord at the selected step (the centre bar's first step while
 * closed); the CHORDS KEYS on PHRASE: the notes the selected step plays while editing, else the ones the centre bar's last hit plays
 * (seq_part_step_notes: the hit octave, an early hit's chord, CADENCE's voicing), from MIDI 24 (seq_keys_of) */
static void ph_keys_texts(const seq_t *q, const rpage_t *P, int bar, const phbar_t *b, phcells_t *c) {
    int sel = seq_rbar_sel(q, P->lane), at = sel >= 0 ? sel : 0, last = -1, b2, s2, v[THEORY_CHORD_MAX], k;
    seq_keys_t keys;
    c->sel_hit = (unsigned char)(sel >= 0 && rhythm_is_hit(b->d[sel]));
    if (P->dl == DL_BASS) {
        int root = ph_root(q, ph_slot_of(q, P->lane, bar, at, b->f[at], &b2, &s2));
        for (int i = 0; i < 12; i++) {
            txt_t t = {c->piano[i], sizeof c->piano[i], 0};
            add(&t, theory_key_name(UNK_PC[i])), add(&t, " "), add(&t, PH_DEG[pmod(UNK_PC[i] - root, 12)]);
        }
        return;
    }
    for (int i = 0; i < SEQ_STEPS; i++)
        if (rhythm_is_hit(b->o[i])) last = i;
    at = sel >= 0 ? sel : last >= 0 ? last : 0;
    k = seq_part_step_notes(q, seq_part(q), LANE_CHORDS, bar, at, v);   /* as it plays: hit octave, early chord, CADENCE */
    keys = seq_keys_of(v, k);
    c->keys = keys.lit, c->kedge = (unsigned char)(keys.edge[0] | keys.edge[1] << 1);
}

/* COPY TO...'s bars (while it shows): "BAR 6 \xC2\xB7 FROM", " \xC2\xB7 LOCKED", " \xC2\xB7 PICKED" (picked for the copy); their inner lines,
 * the bar's chords and rhythm as it plays ("C\xC2\xB7G@9 \xC2\xB7 x---x---x---x---"), the dot left out past PH_INNER_CHARS, then the chords
 * (the text picture of rcp_: 16 bars of dots would be 256 more params) */
static void ph_copy_texts(const seq_t *q, const rpage_t *P, phcells_t *c) {
    unsigned mask = seq_rcopy_mask(q, P->lane);
    int from = seq_rbar_bar(q, P->lane);
    char ch[40], pic[17];
    for (int i = 0; i < SEQ_MAX_BARS; i++) {
        txt_t t = {c->copy[i], sizeof c->copy[i], 0};
        if (P->view(q) != P->copy || i >= seq_bars(q)) {
            add(&t, " "), put(c->inner[i], sizeof c->inner[i], " ");
            continue;
        }
        add(&t, "BAR "), add_int(&t, i + 1);
        add(&t, i == from ? " \xC2\xB7 FROM" : jam_locked_bar(q, P->dl, i) ? " \xC2\xB7 LOCKED" : mask >> i & 1 ? " \xC2\xB7 PICKED" : "");
        seq_chord_label(q, i, ch, sizeof ch), rp_picture(q, P, i, pic);
        for (int form = 0; form < 3; form++) {
            txt_t u = {c->inner[i], sizeof c->inner[i], 0};
            if (form < 2) add(&u, ch), add(&u, form ? " " : " \xC2\xB7 ");
            add(&u, pic);
            if (theory_text_width(c->inner[i]) <= PH_INNER_CHARS) break;
        }
    }
}

/* Every cell of the page, made again when what they show changed (force: after a set or a block switch): the part, the strip page, the
 * centre, the open bar, the view and, while FOLLOW runs and plays, the playing bar; a change bumps display_rev */
/* Task 8: a strip cell's chord line, cut to fit its box (PH_H_CHARS) at the last chord ("\xC2\xB7") that leaves room for "...", else at a
 * character; ASCII "..." (ruling 20) */
#define PH_H_CHARS 14   /* the strip cell's chord line: 144 px at 22 px (vst/make_skin.py phrase_strip) */
static void fit_chords(char *buf, int chars) {
    int w = 0, cut = -1, hard = 0;
    if (theory_text_width(buf) <= chars) return;
    for (int i = 0; buf[i]; i++) {
        if (((unsigned char)buf[i] & 0xC0) == 0x80) continue;   /* a UTF-8 continuation byte */
        if (w + 3 <= chars) {
            hard = i;
            if ((unsigned char)buf[i] == 0xC2 && (unsigned char)buf[i + 1] == 0xB7) cut = i;
        }
        w++;
    }
    memcpy(buf + (cut > 0 ? cut : hard), "...", 4);
}
static int ph_key(const seq_t *q, const rpage_t *P) {
    int live = rp_live(q, P);
    return seq_part(q) | (live ? seq_cur_bar(q) : 0) << 3 | (seq_cur_step(q) >= 0) << 8 | live << 9 | seq_strip_page(q, P->tab) << 10 |
           ph_centre(q, P) << 14 | (seq_rbar_bar(q, P->lane) + 1) << 19 | P->view(q) << 24;
}
static void ph_cells_of(const seq_t *q, const rpage_t *P, phcells_t *c) {
    int first = ph_first(q, P), bar = ph_centre(q, P), pages = (seq_bars(q) + 3) / 4;
    phbar_t b;
    txt_t t = {c->page, sizeof c->page, 0};
    for (int i = 0; i < 4; i++) {
        if (first + i < seq_bars(q)) {
            ph_strip_text(q, P, first + i, c->strip[i], sizeof c->strip[i]);
            seq_chord_label(q, first + i, c->chords[i], sizeof c->chords[i]);
            fit_chords(c->chords[i], PH_H_CHARS);
        } else {
            put(c->strip[i], sizeof c->strip[i], " "), put(c->chords[i], sizeof c->chords[i], " ");
        }
        ph_dots(q, P, first + i, &c->dots[i * SEQ_STEPS]);
    }
    add(&t, first + 1 >= seq_bars(q) ? "BAR " : "BARS "), add_int(&t, first + 1);
    if (first + 1 < seq_bars(q)) add(&t, "-"), add_int(&t, clampi(first + 4, first + 1, seq_bars(q)));
    t = (txt_t){c->pgn, sizeof c->pgn, 0};
    add(&t, "phrase "), add_int(&t, first / 4 + 1), add(&t, " of "), add_int(&t, pages);
    ph_bar(q, P, bar, &b);
    ph_dots(q, P, bar, &c->dots[64]);
    ph_field_texts(q, P, bar, c);
    ph_centre_texts(q, P, bar, &b, c);
    ph_keys_texts(q, P, bar, &b, c);
    if (seq_rbar_bar(q, P->lane) >= 0) ph_step_texts(q, P, bar, &b, c);
    else {
        put(c->title, sizeof c->title, " ");
        for (int i = 0; i < SEQ_STEPS; i++) put(c->step[i], sizeof c->step[i], " "), put(c->line[i], sizeof c->line[i], " ");
    }
    ph_copy_texts(q, P, c);
}
static void ph_cells_init(inst_t *s) {   /* a new instance's, both pages', though neither shows (they open on JAM) */
    for (int d = 0; d < DL_COUNT; d++) ph_cells_of(&s->seq, &RPAGE[d], &s->ph[d]), s->ph[d].key = -1;
}
static void ph_cells_now(inst_t *s, int force) {
    const seq_t *q = &s->seq;
    for (int d = 0; d < DL_COUNT; d++) {
        const rpage_t *P = &RPAGE[d];
        phcells_t n;
        int key;
        if (!P->paged(q) || !seq_lane_drawn(q, P->lane)) continue;   /* off its page it stays still */
        key = ph_key(q, P);
        if (!force && key == s->ph[d].key) continue;
        memset(&n, 0, sizeof n);
        ph_cells_of(q, P, &n);
        n.key = key;
        if (memcmp((const char *)&n + offsetof(phcells_t, strip), (const char *)&s->ph[d] + offsetof(phcells_t, strip),
                   sizeof n - offsetof(phcells_t, strip)))
            s->seq.display_rev++;
        s->ph[d] = n;
    }
}

/* The strip: ph_s the strip page's 4 bars (the tag line; lit = the centre, the open bar while editing; a tap opens the editor on it, FOLLOW
 * off: it becomes the centre; inert while a bar or the browser is open, blank past the loop), ph_h their chord lines (display only), ph_g
 * the page arrows (FOLLOW on: a paging pause, said), ph_u UNDO, ph_b BACK TO PLAYING (blank and inert unless paused by paging); the
 * readouts page and pgn */
static int ph_s_on(const seq_t *q, const rpage_t *P, int i) {
    int bar = ph_first(q, P) + i;
    return P->paged(q) && bar < seq_bars(q) && bar == ph_centre(q, P);
}
static void ph_open_at(seq_t *q, const rpage_t *P, int bar) {
    if (!P->paged(q) || rp_open(q, P) || bar < 0 || bar >= seq_bars(q)) return;
    if (!seq_follow(q)) seq_set_centre_bar(q, P->tab, bar);
    rp_open_at(q, P, bar);
}
static void ph_s_tap(seq_t *q, const rpage_t *P, int i, int v) { (void)v; ph_open_at(q, P, ph_first(q, P) + i); }
static int ph_s_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { return ph_put(buf, len, phc(q, P)->strip[i]); }
static int ph_h_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { return ph_put(buf, len, phc(q, P)->chords[i]); }
static void ph_g_tap(seq_t *q, const rpage_t *P, int i, int v) {
    int page;
    txt_t m;
    (void)v;
    if (!P->paged(q) || rp_open(q, P)) return;
    seq_strip_turn(q, P->tab, i ? 1 : -1);
    if (seq_follow_paused(q, P->tab) != 2) return;
    page = seq_strip_page(q, P->tab);
    m = rp_msg_new(q, P);
    add(&m, "FOLLOW paused on BARS "), add_int(&m, page * 4 + 1), add(&m, "-"), add_int(&m, page * 4 + 4);
    add(&m, ": BACK TO PLAYING restarts it");
}
static int ph_g_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { (void)q, (void)P; return arrow_label(i, buf, len); }
static void ph_u_tap(seq_t *q, const rpage_t *P, int i, int v) { (void)P, (void)i, (void)v; seq_undo(q); }
static int ph_u_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { (void)P, (void)i; return act_label(q, 0, buf, len); }
static void ph_b_tap(seq_t *q, const rpage_t *P, int i, int v) { tb_btp_tap(q, P->tab, i, v); }
static int ph_b_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { return tb_btp_label(q, P->tab, i, buf, len); }

/* The left panel: ph_f RHYTHM (<, the field: the RHYTHMS browser, >: the next of the 24 for the open bar, else every unlocked bar, which
 * turns LOCK WITH OWN: said); ph_w LOCK WITH (OWN, WITH <the other part>, IN THE GAPS; lit = the lane's; a tap sets it and says what follows,
 * or that the other part turned OWN); ph_a ANTICIPATE (OFF, 16TH, 8TH, BEAT; a tap says how many changes come in early) */
static void ph_f_tap(seq_t *q, const rpage_t *P, int i, int v) {
    int view = P->view(q), bar = seq_rbar_bar(q, P->lane), lw = seq_lockwith(q, P->lane), changed;
    int cur = seq_bar_rhythm(q, P->lane, ph_centre(q, P), NULL), ri = step_of(rhythm_is_jam(cur) ? -1 : cur, RHYTHM_COUNT, i == 2);
    (void)v;
    if (!P->paged(q) || view == P->copy || (i == 1 && view == P->rbrowse)) return;
    if (bar < 0 && view != P->rbrowse) close_txn(q);
    if (i == 1) {
        seq_rbrowse_open(q, P->lane);
        return;
    }
    changed = seq_rhythm_pick(q, P->lane, ri);
    rp_pick_msg(q, P, bar, ri, changed);
    if (lw && !seq_lockwith(q, P->lane)) {   /* the pick turned LOCK WITH OWN */
        txt_t m = rp_msg_new(q, P);
        add(&m, rhythm_lib_name(P->lib, ri)), add(&m, " in every unlocked bar: LOCK WITH is OWN now");
    }
}
static int ph_f_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    return i != 1 ? arrow_label(i == 2, buf, len) : ph_put(buf, len, phc(q, P)->field);
}
static int ph_w_on(const seq_t *q, const rpage_t *P, int i) { return seq_lockwith(q, P->lane) == i; }
static void ph_w_tap(seq_t *q, const rpage_t *P, int i, int v) {
    static const char *const NAME[DL_COUNT][3] = {{"OWN", "WITH BASS", "IN THE GAPS"}, {"OWN", "WITH CHORDS", "IN THE GAPS"}};
    int n = 0;
    txt_t m;
    (void)v;
    seq_set_lockwith(q, P->lane, i);
    m = rp_msg_new(q, P);
    if (i && seq_lockwith_bumped(q)) {
        add(&m, NAME[P->dl][i]), add(&m, P->dl == DL_BASS ? ": the chords followed the bass, so they are OWN now"
                                                          : ": the bass followed the chords, so it is OWN now");
        return;
    }
    if (!i) {
        add(&m, LW_LINE[P->dl][0]);
        return;
    }
    for (int b = 0; b < seq_bars(q); b++) n += seq_bar_follows(q, P->lane, b) != 0;
    add(&m, NAME[P->dl][i]), add(&m, ": "), add_int(&m, n), add(&m, n == 1 ? " bar follows the " : " bars follow the "), add(&m, ph_other(P));
    if (seq_bars(q) > n) add(&m, ", "), add_int(&m, seq_bars(q) - n), add(&m, " keep their own");   /* own or locked */
}
static int ph_w_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    (void)q;
    return put(buf, len, i == 0 ? "OWN" : i == 2 ? "IN THE GAPS" : P->dl == DL_BASS ? "WITH CHORDS" : "WITH BASS");
}
static int ph_a_on(const seq_t *q, const rpage_t *P, int i) { return seq_anticipate(q, P->lane) == i; }
static void ph_a_tap(seq_t *q, const rpage_t *P, int i, int v) {
    int n = 0;
    txt_t m;
    (void)v;
    seq_set_anticipate(q, P->lane, i);
    for (int k = 0; k < seq_bars(q) * SEQ_STEPS; k++) n += (seq_step_flags(q, P->lane, k / SEQ_STEPS, k % SEQ_STEPS) & SF_EARLY) != 0;
    m = rp_msg_new(q, P);
    add(&m, AN_NAME[i]), add(&m, ": ");
    if (!i) add(&m, P->dl == DL_BASS ? "the bass changes chord on the beat" : "every chord comes in on its beat");
    else if (!n) add(&m, "no chord change has rests before it, so nothing moves");
    else if (P->dl == DL_BASS) add(&m, "the bass reaches "), add_int(&m, n), add(&m, n == 1 ? " chord change early" : " chord changes early");
    else add_int(&m, n), add(&m, n == 1 ? " chord change comes in early" : " chord changes come in early");
}
static int ph_a_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { (void)q, (void)P; return put(buf, len, AN_NAME[i]); }

/* The centre: ph_t its tap (blank: the readouts hold its texts) opens the editor on the centre bar */
static void ph_t_tap(seq_t *q, const rpage_t *P, int i, int v) { (void)i, (void)v; ph_open_at(q, P, ph_centre(q, P)); }
static int ph_t_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { (void)P; return blank_label(q, i, buf, len); }

/* The editor (blank and inert while closed or in COPY TO...): ph_e NEW BAR, RESET BAR, LOCK / UNLOCK, CANCEL, DONE; ph_p the paint row
 * ACCENT, HIT, GHOST, HOLD, REST (lit = the selected paint), COPY TO ALL (the rhythm only, to every other unlocked bar), COPY TO...; ph_n the
 * 16 steps (lit = selected; a tap paints the selected paint and selects it: a step that has it already is only selected), ph_l their third
 * lines (display only) */
static void ph_e_tap(seq_t *q, const rpage_t *P, int i, int v) {
    int bar = seq_rbar_bar(q, P->lane), bass = P->dl == DL_BASS;
    (void)v;
    if (!ph_editing(q, P)) return;
    if (i == 0) {
        if (seq_rbar_new_bar(q, P->lane))
            rp_msg_bar(q, P, "NEW BAR: bar ", bar, bass ? " gets another line from the same settings" : " gets another version of its rhythm");
        else
            rp_msg_bar(q, P, "Bar ", bar, bass ? " has no other line that differs: nothing changed" : " has no other version that differs: nothing changed");
    } else if (i == 1) {
        seq_rbar_reset_bar(q, P->lane);
        rp_msg_bar(q, P, "RESET BAR: bar ", bar, bass ? " plays the part again" : " plays the part again; its chords stay, unpicked");
    } else if (i == 2) {
        seq_rbar_lock(q, P->lane);
        rp_msg_bar(q, P, "Bar ", bar, jam_locked_bar(q, P->dl, bar) ? " locked: knobs, picks and copies leave it" : " follows the knobs again");
    } else if (i == 3) {
        seq_rbar_cancel(q, P->lane);
        rp_msg_bar(q, P, "Cancelled: bar ", bar, " is back as it was, copies too");
    } else {
        seq_rbar_done(q, P->lane);
        rp_msg_bar(q, P, "Kept bar ", bar, ".");
    }
}
static int ph_e_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    if (!ph_editing(q, P)) return put(buf, len, " ");
    return put(buf, len, i == 2 && jam_locked_bar(q, P->dl, seq_rbar_bar(q, P->lane)) ? "UNLOCK" : WEA_NAME[i]);
}
static int ph_p_on(const seq_t *q, const rpage_t *P, int i) { return i < 5 && inst_of(q)->rpaint[P->dl] == PH_PAINT[i]; }
static void ph_p_tap(seq_t *q, const rpage_t *P, int i, int v) {
    (void)v;
    if (i < 5) {
        inst_rw(q)->rpaint[P->dl] = PH_PAINT[i];
    } else if (ph_editing(q, P) && i == 5) {   /* COPY TO ALL: the rhythm only (the spec's default), whatever COPY TO... was set to */
        int what = seq_rcopy_what(q, P->lane);
        seq_rcopy_set_what(q, P->lane, 0);
        rp_copy_msg(q, P, ~0u);
        seq_rbar_copy(q, P->lane, ~0u);
        seq_rcopy_set_what(q, P->lane, what);
    } else if (ph_editing(q, P)) {
        rp_msg_new(q, P);
        seq_rcopy_open(q, P->lane);
    }
}
static int ph_p_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { (void)q, (void)P; return put(buf, len, PH_PAINT_NAME[i]); }
static int ph_n_on(const seq_t *q, const rpage_t *P, int i) { return ph_editing(q, P) && seq_rbar_sel(q, P->lane) == i; }
static void ph_n_tap(seq_t *q, const rpage_t *P, int i, int v) {
    int paint = inst_of(q)->rpaint[P->dl], bar = seq_rbar_bar(q, P->lane);
    unsigned char d[16];
    txt_t m;
    (void)v;
    if (!ph_editing(q, P)) return;
    if (seq_rbar_paint(q, P->lane, i, paint)) {
        seq_bar_codes(q, P->lane, bar, d);
        m = rp_msg_new(q, P);
        add(&m, "Bar "), add_int(&m, bar + 1), add(&m, ": step "), add_int(&m, i + 1), add(&m, " is "), add(&m, paint_name(d[i]));
    } else if (seq_rbar_sel(q, P->lane) != i) {   /* HOLD with no hit or hold before it: nothing, not even selected */
        m = rp_msg_new(q, P);
        add(&m, "HOLD rings on the hit before: put a hit before step "), add_int(&m, i + 1);
    }
}
static int ph_n_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { return ph_put(buf, len, phc(q, P)->step[i]); }
static int ph_l_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { return ph_put(buf, len, phc(q, P)->line[i]); }

/* The CHORDS picker on the selected step (inert while closed): cqk_ the 7 scale chords, cqr_ the 3 borrows (fix_text: NAMES applies; lit = the
 * chord at the step), cqy_ TYPE (cycles as knobs, sus2, sus4, add9, 6) and THIS HIT ONLY, cqo_ OCTAVE -2..+2 (the selected hit's, lit; a
 * hit only). Each pick plays once (the core) and says what it did (seq_rbar_chord's RBC_*) */
static int ph_sel_slot(const seq_t *q) {   /* the chord slot at the selected step (an open Chords bar) */
    int sel = seq_rbar_sel(q, LANE_CHORDS);
    return seq_slot_at(q, seq_rbar_bar(q, LANE_CHORDS), sel < 0 ? 0 : sel);
}
static void ph_msg_step(seq_t *q, const char *a, const char *b) {   /* "Step 9", a, the chord at the step, b */
    char name[16];
    txt_t m = rp_msg_new(q, RP_C);
    ph_chord_text(q, ph_sel_slot(q), name, sizeof name);
    add(&m, "Step "), add_int(&m, seq_rbar_sel(q, LANE_CHORDS) + 1), add(&m, a), add(&m, name), add(&m, b);
}
static void ph_chord_msg(seq_t *q, int r) {
    char name[16];
    txt_t m;
    if (r == RBC_FULL) {
        rp_msg_bar(q, RP_C, "Bar ", seq_rbar_bar(q, LANE_CHORDS), " has 4 chord changes: remove one first");
    } else if (r == RBC_SLOT) {
        m = rp_msg_new(q, RP_C);
        ph_chord_text(q, ph_sel_slot(q), name, sizeof name);
        add(&m, "Bar "), add_int(&m, seq_rbar_bar(q, LANE_CHORDS) + 1), add(&m, ": "), add(&m, name), add(&m, " (picked: PROGRESSION picks keep it)");
    } else if (r == RBC_POINT) {
        m = rp_msg_new(q, RP_C);
        ph_chord_text(q, ph_sel_slot(q), name, sizeof name);
        add(&m, name), add(&m, " from step "), add_int(&m, seq_rbar_sel(q, LANE_CHORDS) + 1), add(&m, " to the next change");
    } else if (r == RBC_GONE) {
        ph_msg_step(q, "'s change is gone: ", " plays on");
    } else if (r == RBC_HEARD) {
        ph_msg_step(q, ": ", "");
    }
}
static int cqk_on(const seq_t *q, int i) { return ph_editing(q, RP_C) && !seq_slot_src(q, ph_sel_slot(q)) && seq_slot_deg(q, ph_sel_slot(q)) == i; }
static void cqk_tap(seq_t *q, int i, int v) { (void)v; if (ph_editing(q, RP_C)) ph_chord_msg(q, seq_rbar_chord(q, i, 0)); }
static int cqk_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->fix_text[i]); }
static int cqr_on(const seq_t *q, int i) { return ph_editing(q, RP_C) && seq_slot_src(q, ph_sel_slot(q)) == i + 1; }
static void cqr_tap(seq_t *q, int i, int v) { (void)v; if (ph_editing(q, RP_C)) ph_chord_msg(q, seq_rbar_chord(q, 0, i + 1)); }
static int cqr_label(const seq_t *q, int i, char *buf, int len) { return put(buf, len, inst_of(q)->fix_text[7 + i]); }
static int ph_type(const seq_t *q) { return pty_of(seq_slot_colour(q, ph_sel_slot(q))); }
static void ph_this_hit_only(seq_t *q) {   /* cqy_2 */
    int bar = seq_rbar_bar(q, LANE_CHORDS), sel = seq_rbar_sel(q, LANE_CHORDS), r, j;
    char was[16], now[16];
    unsigned char d[16];
    txt_t m;
    ph_chord_text(q, ph_sel_slot(q), now, sizeof now);
    r = seq_rbar_this_hit_only(q);
    if (r == RHO_FULL) {
        ph_chord_msg(q, RBC_FULL);
        return;
    }
    m = rp_msg_new(q, RP_C);
    if (r == 1) {
        seq_bar_codes(q, LANE_CHORDS, bar, d);
        for (j = sel + 1; j < SEQ_STEPS - 1 && !rhythm_is_hit(d[j]); j++) {}
        ph_chord_text(q, seq_slot_at(q, bar, j), was, sizeof was);
        add(&m, "Step "), add_int(&m, sel + 1), add(&m, "'s "), add(&m, now), add(&m, " is one hit: "), add(&m, was), add(&m, " again from step ");
        add_int(&m, j + 1);
        return;
    }
    add(&m, "THIS HIT ONLY: ");
    if (r == RHO_NO_LATER_HIT)   /* 0.24 final review (ruling 31): at 1/2- and 1/4-bar chords the change ends at the next slot */
        add(&m, "no hit after step "), add_int(&m, sel + 1), add(&m, seq_chord_len(q) < SEQ_STEPS ? " before the next chord" : " in this bar");
    else if (r == RHO_LATER_HAS_OWN) add(&m, "the next hit starts a change of its own");
    else add(&m, "step "), add_int(&m, sel + 1), add(&m, " starts no change: pick a chord first");   /* Task 8: 63 bytes */
}
static void cqy_tap(seq_t *q, int i, int v) {
    int r;
    (void)v;
    if (!ph_editing(q, RP_C)) return;
    if (i) {
        ph_this_hit_only(q);
        return;
    }
    r = seq_rbar_type(q, (ph_type(q) + 1) % 5);
    if (r == RBC_FULL) ph_chord_msg(q, r);
    else if (r != RBC_NONE) ph_msg_step(q, ": ", "");
}
static int cqy_label(const seq_t *q, int i, char *buf, int len) {
    txt_t t = {buf, len, 0};
    if (i) return put(buf, len, "THIS HIT ONLY");
    add(&t, "TYPE \xC2\xB7 "), add(&t, PH_TYPE[ph_editing(q, RP_C) ? ph_type(q) : 0]);
    return t.n;
}
static const char *const PH_OCT[5] = {"-2", "-1", "0", "+1", "+2"};
static int cqo_on(const seq_t *q, int i) {
    int bar = seq_rbar_bar(q, LANE_CHORDS);
    return ph_editing(q, RP_C) && phc(q, RP_C)->sel_hit && seq_hit_oct(q, bar, seq_rbar_sel(q, LANE_CHORDS)) == i - 2;
}
static void cqo_tap(seq_t *q, int i, int v) {
    (void)v;
    if (!ph_editing(q, RP_C)) return;
    if (!phc(q, RP_C)->sel_hit) {
        txt_t m = rp_msg_new(q, RP_C);
        add(&m, "Step "), add_int(&m, seq_rbar_sel(q, LANE_CHORDS) + 1), add(&m, " is not a hit: OCTAVE moves a hit");
        return;
    }
    seq_rbar_hit_oct(q, i - 2);
    ph_msg_step(q, ": ", i == 2 ? " at the part's octave" : i > 2 ? (i == 3 ? " an octave up" : " two octaves up") : i == 1 ? " an octave down"
                                                                                                                             : " two octaves down");
}
static int cqo_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, PH_OCT[i]); }

/* The BASS picker on the selected step (inert while closed): uqk_ the piano C# D# F# G# A#, C..B (each key's degree on the step's chord; lit =
 * the step's note; image buttons, TAPBL row 15, as unk_), uqo_ OCTAVE 1..4 (lit = the editor's), uqy_ FOLLOW CHORD (unfixes the step) */
static int ph_sel_note(const seq_t *q) {
    int bar = seq_rbar_bar(q, LANE_BASS), sel = seq_rbar_sel(q, LANE_BASS);
    return bar < 0 || sel < 0 ? 0 : seq_abs_note(&ppart(q)->lane[LANE_BASS].steps[bar * SEQ_STEPS + sel]);
}
static void ph_note_msg(seq_t *q) {
    int n = ph_sel_note(q), sel = seq_rbar_sel(q, LANE_BASS);
    txt_t m = rp_msg_new(q, RP_B);
    add(&m, "Step "), add_int(&m, sel + 1);
    if (!n) {
        add(&m, " is a rest: paint a hit to give it a note");
        return;
    }
    add(&m, ": "), add_note(&m, n);
    add(&m, ppart(q)->bass.fix[seq_rbar_bar(q, LANE_BASS) * SEQ_STEPS + sel] ? " \xC2\xB7 fixed: knobs and styles leave it" : " follows the chord");
}
static int uqk_on(const seq_t *q, int i) { int n = ph_sel_note(q); return ph_editing(q, RP_B) && n && n % 12 == UNK_PC[i]; }
static void uqk_tap(seq_t *q, int i, int v) {
    (void)v;
    if (!ph_editing(q, RP_B)) return;
    if (ph_sel_note(q)) seq_rbar_note(q, UNK_PC[i]);
    ph_note_msg(q);
}
static int uqk_label(const seq_t *q, int i, char *buf, int len) { return ph_put(buf, len, phc(q, RP_B)->piano[i]); }
static int uqo_on(const seq_t *q, int i) { return ph_editing(q, RP_B) && seq_rbar_oct(q) == i + 1; }
static void uqo_tap(seq_t *q, int i, int v) {
    (void)v;
    if (!ph_editing(q, RP_B)) return;
    seq_rbar_note_oct(q, i + 1);
    ph_note_msg(q);
}
static int uqo_label(const seq_t *q, int i, char *buf, int len) { (void)q; return put(buf, len, BNO_NAMES[i]); }
static void uqy_tap(seq_t *q, int i, int v) {
    (void)i, (void)v;
    if (!ph_editing(q, RP_B)) return;
    seq_rbar_follow_chord(q);
    ph_note_msg(q);
}
static int uqy_label(const seq_t *q, int i, char *buf, int len) { (void)q, (void)i; return put(buf, len, "FOLLOW CHORD"); }

/* COPY TO... (blank and inert outside it): ph_c the loop's bars (the cache's labels; lit = picked; a tap toggles, the source and locked bars
 * inert), ph_i their inner lines (a tap picks as ph_c's), ph_x ALL, NONE, SAME IN EVERY PHRASE (the open bar's place in each phrase,
 * unlocked), COPY: RHYTHM ONLY / RHYTHM + CHORDS (BASS + NOTES) (toggles), CANCEL, "COPY TO 3 BARS" / "PICK BARS" (inert with none); the
 * readouts ctl "COPY BAR 6 TO..." and cn (rcnote's count) */
static int ph_c_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { return ph_put(buf, len, phc(q, P)->copy[i]); }
static int ph_i_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) { return ph_put(buf, len, phc(q, P)->inner[i]); }
static void ph_x_tap(seq_t *q, const rpage_t *P, int i, int v) {
    unsigned mask;
    txt_t m;
    if (P->view(q) != P->copy) return;
    if (i < 2 || i == 4) {
        rp_cx_tap(q, P, i < 2 ? i : 2, v);
    } else if (i == 2) {
        mask = seq_rcopy_same_place(q, P->lane);
        seq_rcopy_set_all(q, P->lane, 0);
        for (int b = 0; b < seq_bars(q); b++)
            if (mask >> b & 1) seq_rcopy_toggle(q, P->lane, b);
        m = rp_msg_new(q, P);
        if (!mask) {
            rp_msg_bar(q, P, "SAME IN EVERY PHRASE: no other unlocked bar at bar ", seq_rbar_bar(q, P->lane), "'s place");
            return;
        }
        add(&m, "SAME IN EVERY PHRASE: bar");
        for (int b = 0, k = 0; b < seq_bars(q); b++)
            if (mask >> b & 1) add(&m, k++ ? ", " : mask & (mask - 1) ? "s " : " "), add_int(&m, b + 1);
    } else if (i == 3) {
        int what = !seq_rcopy_what(q, P->lane);
        seq_rcopy_set_what(q, P->lane, what);
        m = rp_msg_new(q, P);
        add(&m, !what ? "COPY RHYTHM ONLY: the hits alone" : P->dl == DL_BASS ? "COPY RHYTHM + NOTES: its fixed notes too"
                                                                             : "COPY RHYTHM + CHORDS: its chord changes and octaves too");
    } else {
        rp_cx_tap(q, P, 3, v);
    }
}
static int ph_x_label(const seq_t *q, const rpage_t *P, int i, char *buf, int len) {
    if (P->view(q) != P->copy) return put(buf, len, " ");
    if (i == 2) return put(buf, len, "SAME IN EVERY PHRASE");
    if (i == 3)
        return put(buf, len, !seq_rcopy_what(q, P->lane) ? "COPY: RHYTHM ONLY" : P->dl == DL_BASS ? "COPY: RHYTHM + NOTES" : "COPY: RHYTHM + CHORDS");
    return rp_cx_label(q, P, i < 2 ? i : i == 4 ? 2 : 3, buf, len);
}

/* The readouts (after the lane's prefix): 0 for none */
static int ph_readout(const seq_t *q, const rpage_t *P, const char *k, char *buf, int len) {
    static const char *const LEGEND[DL_COUNT] = {"close \xC2\xB7 a little colour \xC2\xB7 colourful \xC2\xB7 tense \xC2\xB7 dissonant",
                                                 "chord tones \xC2\xB7 warm \xC2\xB7 colourful \xC2\xB7 tense \xC2\xB7 clash"};
    static const struct { const char *key; size_t at; } T[] = {
        {"page", offsetof(phcells_t, page)}, {"pgn", offsetof(phcells_t, pgn)}, {"fl", offsetof(phcells_t, fline)},
        {"wl", offsetof(phcells_t, wline)}, {"al", offsetof(phcells_t, aline)}, {"cw", offsetof(phcells_t, cw)}, {"cc", offsetof(phcells_t, cc)},
        {"ce", offsetof(phcells_t, ce)}, {"tt", offsetof(phcells_t, title)}};
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(k, T[i].key)) return ph_put(buf, len, (const char *)phc(q, P) + T[i].at);
    if (!strcmp(k, "lg")) return put(buf, len, LEGEND[P->dl]);
    if (!strcmp(k, "msg")) return msg_text(q, P->dl, buf, len);
    if (!strcmp(k, "ctl")) return rp_ctl_text(q, P, buf, len);
    if (!strcmp(k, "cn")) return rp_cnote_text(q, P, buf, len);
    return 0;
}

/* The cells of both PHRASE pages: cq... the CHORDS tab's, uq... the BASS tab's, each its ph_ function on its row of RPAGE */
#define PH_ON(f, fn) \
    static int cq##f##_on(const seq_t *q, int i) { return fn(q, RP_C, i); } \
    static int uq##f##_on(const seq_t *q, int i) { return fn(q, RP_B, i); }
#define PH_TAP(f, fn) \
    static void cq##f##_tap(seq_t *q, int i, int v) { fn(q, RP_C, i, v); } \
    static void uq##f##_tap(seq_t *q, int i, int v) { fn(q, RP_B, i, v); }
#define PH_LABEL(f, fn) \
    static int cq##f##_label(const seq_t *q, int i, char *buf, int len) { return fn(q, RP_C, i, buf, len); } \
    static int uq##f##_label(const seq_t *q, int i, char *buf, int len) { return fn(q, RP_B, i, buf, len); }
#define PH_CELL(f, on, tap, label) PH_ON(f, on) PH_TAP(f, tap) PH_LABEL(f, label)
PH_CELL(s, ph_s_on, ph_s_tap, ph_s_label)
PH_TAP(h, rp_no_tap) PH_LABEL(h, ph_h_label)
PH_TAP(g, ph_g_tap) PH_LABEL(g, ph_g_label)
PH_TAP(u, ph_u_tap) PH_LABEL(u, ph_u_label)
PH_TAP(b, ph_b_tap) PH_LABEL(b, ph_b_label)
PH_TAP(f, ph_f_tap) PH_LABEL(f, ph_f_label)
PH_CELL(w, ph_w_on, ph_w_tap, ph_w_label)
PH_CELL(a, ph_a_on, ph_a_tap, ph_a_label)
PH_TAP(t, ph_t_tap) PH_LABEL(t, ph_t_label)
PH_TAP(e, ph_e_tap) PH_LABEL(e, ph_e_label)
PH_CELL(p, ph_p_on, ph_p_tap, ph_p_label)
PH_CELL(n, ph_n_on, ph_n_tap, ph_n_label)
PH_TAP(l, rp_no_tap) PH_LABEL(l, ph_l_label)
PH_CELL(c, rp_cb_on, rp_cb_tap, ph_c_label)
PH_TAP(i, rp_cp_tap) PH_LABEL(i, ph_i_label)
PH_TAP(x, ph_x_tap) PH_LABEL(x, ph_x_label)

#define TAP(prefix, n, select, on, name) {prefix, sizeof prefix - 1, n, select, on, name##_tap, name##_label, 0, 0}
#define TAPB(prefix, n, row, mask, name) {prefix, sizeof prefix - 1, n, 1, never_on, name##_tap, name##_label, row, mask}   /* buttons */
#define TAPBL(prefix, n, row, mask, on, name) {prefix, sizeof prefix - 1, n, 1, on, name##_tap, name##_label, row, mask}   /* lit ones */
/* 0.24 (ruling 19) a retired cell (KF_RETIRED_24: set_param returns before any tap): no tap code, its label and light still answer */
#define TAPR(prefix, n, select, on, name) {prefix, sizeof prefix - 1, n, select, on, no_tap, name##_label, 0, 0}
static const tap_t TAPS[] = {
    /* 0.17 KEYS first: 62 cells polled every 10 ms, found after two entries (parse_kind tries the entries in order) */
    TAP("kyw_", 35, 1, kyw_on, kyw), TAP("kyb_", 25, 1, kyb_on, kyb),
    TAP("mt_", SEQ_ACTIVE_LANES, 0, mt_on, mt), TAP("bar_", 8, 0, bar_on, bar), TAP("fol_", 1, 0, fol_on, fol),
    TAP("act_", 2, 0, never_on, act), TAP("kw_", 7, 1, kw_on, kw), TAP("kb_", 5, 1, kb_on, kb), TAP("kb2_", 3, 1, kb2_on, kb2), TAP("sc_", 8, 1, sc_on, sc),
    TAP("ln_", 5, 1, ln_on, ln), TAP("cl_", 4, 1, cl_on, cl), TAP("new_", 1, 0, never_on, new),
    TAP("gl_", SEQ_ACTIVE_LANES + 1, 0, never_on, gl), TAP("pt_", SEQ_NPARTS, 0, pt_on, pt),
    TAPR("md_", 3, 1, md_on, md), TAPR("ds_", 3, 1, ds_on, ds), TAPR("fd_", 9, 0, fd_on, fd),
    TAPR("ia_", 3, 0, never_on, ia), TAPR("pn_", 4, 0, never_on, pn), TAPR("mv_", 6, 0, never_on, mv),
    TAPR("fs_", 8, 0, fs_on, fs), TAPR("fc_", 7, 0, never_on, fc), TAPR("fb_", THEORY_NBORROW, 0, never_on, fb),
    TAPR("fo_", COL_ADD9 + 1, 0, never_on, fo), TAPR("dn_", 1, 0, never_on, dn), TAPR("rh_", 3, 1, rh_on, rh),
    TAPR("vo_", 2, 1, vo_on, vo), TAPR("oc_", 3, 1, oc_on, oc),
    TAP("cnm_", 1, 0, cnm_on, cnm), TAPR("cnp_", 1, 0, never_on, cnp), TAPR("crh_", STYLE_NOPTS - 1, 1, rh_on, rh),
    TAPR("cnr_", 1, 0, never_on, cnr), TAPR("fsl_", 4, 0, fs_on, fs),
    TAPR("bmd_", 3, 1, bmd_on, bmd), TAPR("bds_", 3, 1, ds_on, ds), TAPR("bfd_", 9, 0, fd_on, fd),
    TAPR("bia_", 3, 0, never_on, ia), TAPR("bpn_", 4, 0, never_on, pn), TAPR("bmv_", 6, 0, never_on, bmv),
    TAPR("bsy_", 4, 1, bsy_on, bsy), TAPR("boc_", 3, 1, boc_on, boc), TAPR("bap_", 2, 1, bap_on, bap),
    TAPR("bst_", SEQ_STEPS, 0, bst_on, blank), TAPR("bnk_", 12, 0, never_on, bnk), TAPR("bno_", 4, 0, bno_on, bno),
    TAPR("brs_", 1, 0, never_on, brs), TAPR("bdn_", 1, 0, never_on, bdn),
    TAPR("mmd_", 4, 1, mmd_on, mmd), TAPR("mty_", MEL_NTYPES, 0, mty_on, mty), TAPR("mo1_", 4, 1, mo1_on, mo1),
    TAPR("mo2_", 4, 1, mo2_on, mo2), TAPR("mo3_", 4, 1, mo3_on, mo3), TAPR("mol_", 3, 1, never_on, mol), TAPR("mph_", MEL_NPHRASES, 1, mph_on, mph),
    TAPR("mds_", 3, 1, ds_on, ds), TAPR("mfd_", 9, 0, fd_on, fd), TAPR("mia_", 3, 0, never_on, ia),
    TAPR("mpn_", 4, 0, never_on, pn), TAPR("mmv_", 4, 0, never_on, mmv), TAPR("mrk_", 1, 0, never_on, mrk),
    TAPR("mpr_", 1, 0, never_on, mpr),
    TAPR("bnb_", SEQ_MAX_BARS, 0, nb_on, blank), TAPR("bba_", 2, 0, never_on, bba), TAPR("mnb_", SEQ_MAX_BARS, 0, nb_on, blank),
    TAPR("mba_", 2, 0, never_on, mba), TAPR("mpf_", MEL_NFORMS, 1, mpf_on, mpf), TAPR("mpb_", MEL_NBREATHS, 1, mpb_on, mpb),
    TAPR("mpc_", MEL_NCADS, 1, mpc_on, mpc), TAPR("mpl_", MEL_NFILLS, 1, mpl_on, mpl),
    TAPR("mst_", SEQ_STEPS, 0, mst_on, blank), TAPR("mnk_", 12, 0, never_on, mnk), TAPR("mno_", 5, 0, mno_on, mno),
    TAPR("mrt_", 1, 0, never_on, mrt), TAPR("mdn_", 1, 0, never_on, mdn), TAPR("fbs_", 4, 0, fbs_on, fbs),
    TAPR("fbar_", SEQ_MAX_BARS, 0, never_on, blank),
    TAP("cvm_", 4, 1, cvm_on, cvm), TAPR("cst_", 4, 0, cst_on, cst), TAPR("cpg_", 2, 0, never_on, cpg), TAPR("cund_", 1, 0, never_on, cund),
    TAPR("cbtp_", 1, 0, never_on, cbtp), TAPB("cpr_", 3, 4, 0x7, cpr), TAPB("cvo_", 3, 5, 0x7, cvo),
    TAP("crk_", 1, 0, never_on, crk), TAP("cct_", 1, 0, never_on, cct), TAPR("pty_", 5, 0, never_on, pty),
    TAPR("plk_", 1, 0, never_on, plk), TAPR("pcn_", 1, 0, never_on, pcn), TAPR("pdn_", 1, 0, never_on, pdn),
    TAP("bpr_", 16, 0, bpr_on, bpr), TAP("bgn_", 15, 1, bgn_on, bgn), TAP("bvs_", 4, 0, bvs_on, bvs),
    TAP("bvf_", 4, 1, never_on, bvf), TAP("bcn_", 1, 0, never_on, bcn), TAP("bdo_", 1, 0, never_on, bdo),
    TAP("kye_", 2, 1, kye_on, kye),
    /* 0.18 the RHYTHM page (0.24.1 retired but for its RHYTHMS browser: PHRASE's cells replace them; image button rows 1-3 unused) */
    TAPR("rst_", SEQ_STEPS, 0, never_on, blank), TAPR("rpt_", 5, 1, never_on, blank), TAPR("rac_", 6, 0, never_on, blank),
    TAPR("rfl_", 3, 0, never_on, blank), TAPR("csr_", 4, 1, never_on, blank), TAPR("rcb_", SEQ_MAX_BARS, 0, never_on, blank),
    TAPR("rcx_", 4, 0, never_on, rcx),
    TAP("rbg_", RHYTHM_GROUPS, 1, rbg_on, rbg), TAP("rbt_", 4, 0, rbt_on, rbt), TAP("rbp_", 4, 1, never_on, rbp),
    TAP("rbf_", 4, 1, never_on, rbf), TAPR("rcp_", SEQ_MAX_BARS, 0, never_on, blank),
    /* 0.19 the BASS page */
    TAP("ubm_", 3, 1, ubm_on, ubm), TAPR("ust_", 4, 0, ust_on, ust), TAPR("usr_", 4, 1, never_on, usr), TAPR("upg_", 2, 0, never_on, upg),
    TAPR("und_", 1, 0, never_on, und), TAPR("ubtp_", 1, 0, never_on, ubtp), TAPB("usy_", 3, 6, 0x7, usy), TAP("upd_", 3, 1, upd_on, upd),
    TAP("uoc_", 3, 1, uoc_on, uoc), TAP("urk_", 1, 0, never_on, urk), TAP("uct_", 1, 0, never_on, uct),
    TAP("ugn_", BASS_GENRES, 1, ugn_on, ugn), TAP("uti_", 4, 0, uti_on, uti), TAP("utp_", 4, 1, never_on, utp),
    TAP("uts_", 4, 1, never_on, uts), TAP("ubc_", 1, 0, never_on, ubc), TAP("ubd_", 1, 0, never_on, ubd),
    TAPR("uea_", 5, 0, never_on, uea),
    TAPR("ues_", SEQ_STEPS, 1, ues_on, ues), TAPR("unk_", 12, 1, unk_on, unk), TAPR("uno_", 4, 1, uno_on, uno),
    TAPR("uer_", 1, 0, never_on, uer), TAP("ukw_", 35, 1, ukw_on, ukw), TAP("ukb_", 25, 1, ukb_on, ukb), TAP("uke_", 2, 1, uke_on, uke),
    /* 0.19 the BASS RHYTHM page: the RHYTHM page's cells on the Bass lane (retired as theirs; image button rows 7-9 unused) */
    TAPR("yst_", SEQ_STEPS, 0, never_on, blank), TAPR("ypt_", 5, 1, never_on, blank), TAPR("yac_", 6, 0, never_on, blank),
    TAPR("yfl_", 3, 0, never_on, blank), TAPR("ysr_", 4, 1, never_on, blank), TAPR("ysc_", 4, 0, never_on, blank),
    TAPR("ycb_", SEQ_MAX_BARS, 0, never_on, blank), TAPR("ycp_", SEQ_MAX_BARS, 0, never_on, blank), TAPR("ycx_", 4, 0, never_on, ycx),
    TAP("ybg_", RHYTHM_GROUPS, 1, ybg_on, ybg),
    TAP("ybt_", 4, 0, ybt_on, ybt), TAP("ybp_", 4, 1, never_on, ybp), TAP("ybf_", 4, 1, never_on, ybf),
    /* 0.20 the MELODY tab's JAM page */
    TAP("jmd_", 2, 1, jmd_on, jmd), TAP("jbk_", MJ_COLS * MJ_LEVELS, 0, jbk_on, jbk), TAP("jph_", 4, 0, jph_on, jph),
    TAP("jsw_", 1, 1, never_on, jsw), TAP("jkw_", 35, 1, jkw_on, jkw), TAP("jkb_", 25, 1, jkb_on, jkb), TAP("jke_", 2, 1, jke_on, jke),
    /* 0.20 the PHRASE page, its ANSWER BLOCK picker and bar editor (ANSWER PLAYS latches as usy_, the piano as unk_, in rows of their own) */
    TAP("wst_", 4, 0, wst_on, wst), TAP("wpg_", 2, 0, never_on, wpg), TAP("wund_", 1, 0, never_on, wund), TAP("wbtp_", 1, 0, never_on, wbtp),
    TAP("wfm_", MF_COUNT, 1, wfm_on, wfm), TAP("wan_", MA_COUNT, 1, wan_on, wan), TAPB("wsw_", 3, 11, 0x7, wsw), TAP("wct_", 1, 0, never_on, wct),
    TAP("wpk_", MJ_COLS * MJ_LEVELS, 0, wpk_on, wpk), TAP("wpc_", 1, 0, never_on, wpc), TAP("wpd_", 1, 0, never_on, wpd),
    TAP("wea_", 5, 0, never_on, wea), TAP("wes_", SEQ_STEPS, 1, wes_on, wes), TAPBL("wnk_", 12, 12, 0xFFF, wnk_on, wnk),
    TAP("wno_", 5, 1, wno_on, wno), TAP("wrh_", 2, 0, never_on, wrh),
    /* 0.22 the CHORDS and BASS JAM pages (never lit: their meters show the states) */
    TAP("cjb_", JAM_COLS * JAM_LEVELS, 0, never_on, cjb), TAP("cjp_", 4, 0, never_on, cjp), TAP("cjsw_", 1, 1, never_on, cjsw),
    TAP("ujb_", JAM_COLS * JAM_LEVELS, 0, never_on, ujb), TAP("ujp_", 4, 0, never_on, ujp), TAP("ujsw_", 1, 1, never_on, ujsw),
    /* 0.24 the CHORDS page's OCTAVE, GLOBAL's NAMES, the sound pages' status rows */
    TAP("cpo_", 3, 1, cpo_on, cpo), TAP("gnm_", 2, 1, gnm_on, gnm), TAP("csb_", 4, 0, sb_on, csb), TAP("csc_", 4, 1, never_on, csc),
    TAP("cse_", 1, 0, never_on, cse), TAP("usb_", 4, 0, sb_on, usb), TAP("usc_", 4, 1, never_on, usc), TAP("use_", 1, 0, never_on, use),
    /* 0.24 (Task 7) the PHRASE pages, CHORDS cq... then BASS uq...: the RHYTHM fields latch as rfl_ (rows 13, 14), the BASS piano as unk_ (15),
     * COPY TO...'s SAME IN EVERY PHRASE, COPY, CANCEL and GO as rcx_4 (Task 8: image buttons, rows 16, 17) */
    TAP("cqs_", 4, 0, cqs_on, cqs), TAP("cqh_", 4, 1, never_on, cqh), TAP("cqg_", 2, 0, never_on, cqg), TAP("cqu_", 1, 0, never_on, cqu),
    TAP("cqb_", 1, 0, never_on, cqb), TAPB("cqf_", 3, 13, 0x7, cqf), TAP("cqw_", 3, 1, cqw_on, cqw), TAP("cqa_", 4, 1, cqa_on, cqa),
    TAP("cqt_", 1, 0, never_on, cqt), TAP("cqe_", 5, 0, never_on, cqe), TAP("cqp_", 7, 1, cqp_on, cqp), TAP("cqn_", SEQ_STEPS, 0, cqn_on, cqn),
    TAP("cql_", SEQ_STEPS, 1, never_on, cql), TAP("cqk_", 7, 0, cqk_on, cqk), TAP("cqr_", THEORY_NBORROW, 0, cqr_on, cqr),
    TAP("cqy_", 2, 0, never_on, cqy), TAP("cqo_", 5, 1, cqo_on, cqo), TAP("cqc_", SEQ_MAX_BARS, 0, cqc_on, cqc),
    TAP("cqi_", SEQ_MAX_BARS, 0, never_on, cqi), TAPB("cqx_", 6, 16, 0x3C, cqx),
    TAP("uqs_", 4, 0, uqs_on, uqs), TAP("uqh_", 4, 1, never_on, uqh), TAP("uqg_", 2, 0, never_on, uqg), TAP("uqu_", 1, 0, never_on, uqu),
    TAP("uqb_", 1, 0, never_on, uqb), TAPB("uqf_", 3, 14, 0x7, uqf), TAP("uqw_", 3, 1, uqw_on, uqw), TAP("uqa_", 4, 1, uqa_on, uqa),
    TAP("uqt_", 1, 0, never_on, uqt), TAP("uqe_", 5, 0, never_on, uqe), TAP("uqp_", 7, 1, uqp_on, uqp), TAP("uqn_", SEQ_STEPS, 0, uqn_on, uqn),
    TAP("uql_", SEQ_STEPS, 1, never_on, uql), TAPBL("uqk_", 12, 15, 0xFFF, uqk_on, uqk), TAP("uqo_", 4, 1, uqo_on, uqo),
    TAP("uqy_", 1, 0, never_on, uqy), TAP("uqc_", SEQ_MAX_BARS, 0, uqc_on, uqc), TAP("uqi_", SEQ_MAX_BARS, 0, never_on, uqi),
    TAPB("uqx_", 6, 17, 0x3C, uqx)};

/* A parsed skin key: "l3_12" (cell, lane 2, step 11), "ph_5", "chord_2", "mute_4", a tap cell "kw_3" (TAPS entry, cell 2),
 * each optionally ending in "_on". Hand-rolled rather than sscanf: the wrapper asks for every "<key>_on" every 10 ms on the
 * audio thread. 0.19.1 flags: the key's KF_* (vst/build/keyflags.h) */
typedef enum { K_OTHER, K_CELL, K_PH, K_CHORD, K_MUTE, K_TAP } kind_t;
typedef struct { kind_t kind; int a, b, on, flags; } skey_t;

static int read_num(const char **p, int max) {   /* 1..max -> 0-based index, else -1 */
    int n = 0;
    if (**p < '1' || **p > '9') return -1;
    while (**p >= '0' && **p <= '9') n = n * 10 + (*(*p)++ - '0');
    return n <= max ? n - 1 : -1;
}

/* 0.18 perf: TAPS chained by a hash of their prefix's first two characters (0.24 Task 7: three, every prefix has them, so the 20 cq...
 * and 19 uq... families spread: at most 5 a bucket), so parse_kind (every polled key, on the audio thread) tries a bucket's few entries
 * instead of every entry. Made once, by tap_index_init from create (the UI thread,
 * before the instance exists for anyone else); until then parse_kind tries every entry. Chains keep TAPS order. */
#define NTAPS (sizeof TAPS / sizeof TAPS[0])
_Static_assert(NTAPS < 255, "tap_head and tap_next hold an entry + 1 in a byte");
static unsigned char tap_head[128], tap_next[NTAPS];   /* entry + 1, 0 none */
static unsigned char tap_flags[NTAPS];   /* 0.19.1 each entry's cells' flags (vst/build/keyflags.h), read once */
static int tap_index_state;   /* 0 empty, 1 being made, 2 made */
static unsigned tap_bucket(const char *k) {   /* a key shorter than 3 reads its 0 there: no prefix is */
    return (((unsigned char)k[0] * 31u + (unsigned char)k[1]) * 31u + (unsigned char)(k[1] ? k[2] : 0)) & 127u;
}
static void tap_index_init(void) {
    int empty = 0;
    if (!__atomic_compare_exchange_n(&tap_index_state, &empty, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    for (size_t t = NTAPS; t-- > 0;) {
        unsigned b = tap_bucket(TAPS[t].prefix);
        tap_next[t] = tap_head[b];
        tap_head[b] = (unsigned char)(t + 1);
        tap_flags[t] = (unsigned char)keyflags_find(TAPS[t].prefix, (size_t)TAPS[t].plen);
    }
    __atomic_store_n(&tap_index_state, 2, __ATOMIC_RELEASE);
}

/* 1 if key starts with TAPS[t]'s prefix (k and p then the cell, k->b -1 if its number is no cell's) */
static int tap_match(const char *key, size_t t, skey_t *k, const char **p) {
    if (key[0] != TAPS[t].prefix[0] || key[1] != TAPS[t].prefix[1] || strncmp(key, TAPS[t].prefix, (size_t)TAPS[t].plen)) return 0;
    *p = key + TAPS[t].plen;
    k->b = read_num(p, TAPS[t].n);
    k->kind = K_TAP;
    k->a = (int)t;
    return 1;
}

static skey_t parse_kind(const char *key) {
    static const struct { const char *prefix; kind_t kind; int max; } P[] = {
        {"ph_", K_PH, SEQ_STEPS}, {"chord_", K_CHORD, 8}, {"mute_", K_MUTE, SEQ_LANES}};
    skey_t k = {K_OTHER, -1, -1, 0, 0};
    const char *p = key;
    if (key[0] == 'l' && key[1] >= '1' && key[1] <= '9') {   /* the most numerous keys, and no TAPS prefix: first */
        p++;
        k.a = read_num(&p, SEQ_LANES);
        if (k.a < 0 || *p++ != '_' || (k.b = read_num(&p, SEQ_STEPS)) < 0) return (skey_t){K_OTHER, -1, -1, 0, 0};
        k.kind = K_CELL;
    }
    if (k.kind == K_OTHER && key[0] && __atomic_load_n(&tap_index_state, __ATOMIC_RELAXED) == 2) {
        for (int e = tap_head[tap_bucket(key)]; e && !tap_match(key, (size_t)e - 1, &k, &p); e = tap_next[e - 1]) {}
    } else {
        for (size_t t = 0; t < NTAPS && k.kind == K_OTHER; t++) tap_match(key, t, &k, &p);   /* two characters, then the prefix */
    }
    if (k.kind == K_TAP && k.b < 0) return (skey_t){K_OTHER, -1, -1, 0, 0};
    if (k.kind == K_TAP || k.kind == K_CELL) {
    } else {
        for (size_t i = 0; i < sizeof P / sizeof P[0] && k.kind == K_OTHER; i++) {
            size_t n = strlen(P[i].prefix);
            if (strncmp(key, P[i].prefix, n)) continue;
            p = key + n;
            if ((k.a = read_num(&p, P[i].max)) < 0) return (skey_t){K_OTHER, -1, -1, 0, 0};
            k.kind = P[i].kind;
        }
        if (k.kind == K_OTHER) return k;
    }
    k.on = !strcmp(p, "_on");
    return (*p == 0 || k.on) ? k : (skey_t){K_OTHER, -1, -1, 0, 0};
}
/* 0.19.1 with its flags: a tap cell's its TAPS entry's (tap_flags, or found until the index is made), another key's its own or its cell
 * family's (keyflags_of: a binary search); the grid, playhead, chord and mute keys have none (vst/make_skin.py write_keyflags) */
static skey_t parse_key(const char *key) {   /* set_param's: get_param (polled) needs no flags (parse_kind) */
    skey_t k = parse_kind(key);
    if (k.kind == K_TAP)
        k.flags = __atomic_load_n(&tap_index_state, __ATOMIC_ACQUIRE) == 2 ? tap_flags[k.a]   /* sees the index's RELEASE store */
                                                                           : keyflags_find(TAPS[k.a].prefix, (size_t)TAPS[k.a].plen);
    else if (k.kind == K_OTHER) k.flags = keyflags_of(key);
    return k;
}

static int log2i(int v) { int i = 0; while (v > 1) { v >>= 1; i++; } return i; }

/* Option parameters arrive and leave as their index. Each has a setter (clamping the index) and a getter. */
typedef struct { const char *key; void (*set)(seq_t *, int); int (*get)(const seq_t *); } option_t;

static void set_length(seq_t *q, int i) { seq_set_bars(q, 1 << clampi(i, 0, 4)); }
static int get_length(const seq_t *q) { return log2i(seq_bars(q)); }
static void set_chord_len(seq_t *q, int i) { seq_set_chord_len(q, 32 >> clampi(i, 0, 3)); }
static int get_chord_len(const seq_t *q) { return 5 - log2i(seq_chord_len(q)); }
/* Custom (the last option) is never set, but the stepper arrows wrap through it: asked for it from the last preset, go to the
 * first, and from the first, to the last, so the arrows wrap end to end. Anywhere else it is ignored. */
static void set_prog(seq_t *q, int i) {
    int cur = seq_prog_preset(q);
    if (i == THEORY_NPRESETS && cur == THEORY_NPRESETS - 1) i = 0;
    else if (i == THEORY_NPRESETS && cur == 0) i = THEORY_NPRESETS - 1;
    if (i >= 0 && i < THEORY_NPRESETS) seq_set_prog_preset(q, i);
}
static int get_prog(const seq_t *q) { int i = seq_prog_preset(q); return i < 0 ? THEORY_NPRESETS : i; }
/* View is 16 options whatever the loop length: a request past the loop's last bar is the arrows wrapping, forward to Bar 1
 * (seq_set_view's clamp) or, from Bar 1, back to the loop's last bar. */
static void set_view(seq_t *q, int i) { seq_set_view(q, i >= seq_bars(q) && seq_view_bar(q) == 0 ? seq_bars(q) - 1 : i); }
static void set_voicing(seq_t *q, int i) { seq_set_voicing(q, i > 0 ? VOICING_SMOOTH : VOICING_CLOSE); }
static int get_style(const seq_t *q, int lane);
/* A set equal to the value reported is a no-op: MPC echoing style_3 back must not turn a recipe Melody (reported as Hook)
 * into a classic one.
 * 0.12: style_1 grew from 4 options to 10, so a host that restores a 0.11 project's normalised value (k/3) lands on option 3k
 * of the new list: Sustain (1/3) on Stabs, Pulse (2/3) on Ballad, Stabs (1) on Push. The chunk already restored the real
 * style, so on Chords a set of 3 x the reported value (1..3) is that stale image and is ignored. The cost: from Sustain, Pulse
 * or Stabs, MPC's own style_1 list cannot pick Stabs, Ballad or Push directly (the crh_ cells can). */
static void set_style(seq_t *q, int lane, int i) {
    int cur = get_style(q, lane);
    if (lane == LANE_CHORDS && cur >= 1 && cur <= 3 && i == 3 * cur) return;
    if (i >= 0 && i < STYLE_NOPTS && STYLE_IDS[lane][i] >= 0 && i != cur) seq_set_style(q, lane, STYLE_IDS[lane][i]);
}

/* A recipe Melody (0.11) has no style_3 option of its own: it reads as Hook (1), the classic it plays in 0.10 */
static int get_style(const seq_t *q, int lane) {
    if (lane == LANE_MEL_UP && seq_style(q, lane) == GEN_MEL_RECIPE) return 1;
    for (int i = 0; i < STYLE_NOPTS; i++)
        if (STYLE_IDS[lane][i] == seq_style(q, lane)) return i;
    return 0;
}

#define LANE_OPTIONS(n, lane) \
    static void set_style_##n(seq_t *q, int i) { set_style(q, lane, i); } \
    static int get_style_##n(const seq_t *q) { return get_style(q, lane); } \
    static void set_cx_##n(seq_t *q, int c) { seq_set_complexity(q, lane, clampi(c, 0, 100)); } \
    static int get_cx_##n(const seq_t *q) { return seq_complexity(q, lane); }
LANE_OPTIONS(1, 0) LANE_OPTIONS(2, 1) LANE_OPTIONS(3, 2) LANE_OPTIONS(4, 3)
static void set_harmony(seq_t *q, int c) { seq_set_harmony(q, clampi(c, 0, 100)); }
#define CHANNEL_OPTION(n, lane) \
    static void set_ch_##n(seq_t *q, int i) { seq_set_channel(q, lane, clampi(i, 0, 15) + 1); } \
    static int get_ch_##n(const seq_t *q) { return seq_channel_of(q, lane) - 1; }
CHANNEL_OPTION(1, 0) CHANNEL_OPTION(2, 1) CHANNEL_OPTION(3, 2)
static void set_swing(seq_t *q, int v) { seq_set_swing(q, clampi(v, 0, 100)); }
static void set_gate(seq_t *q, int v) { seq_set_gate(q, clampi(v, 0, 100)); }
static void set_dyn(seq_t *q, int v) { seq_set_dynamics(q, clampi(v, 0, 100)); }
#define SHAPE_OPTION(name, what) \
    static void set_##name(seq_t *q, int v) { seq_set_chord_shape(q, what, v); } \
    static int get_##name(const seq_t *q) { return seq_chord_shape(q, what); }
SHAPE_OPTION(ccol, SHAPE_COLOUR) SHAPE_OPTION(cspr, SHAPE_SPREAD) SHAPE_OPTION(cpush, SHAPE_PUSH)
SHAPE_OPTION(cgate, SHAPE_GATE) SHAPE_OPTION(cstrum, SHAPE_STRUM) SHAPE_OPTION(cdyn, SHAPE_DYN)
#define BSHAPE_OPTION(name, what) \
    static void set_##name(seq_t *q, int v) { seq_set_lane_shape(q, LANE_BASS, what, v); } \
    static int get_##name(const seq_t *q) { return seq_lane_shape(q, LANE_BASS, what); }
BSHAPE_OPTION(bmov, BSHAPE_MOVEMENT) BSHAPE_OPTION(bsync, BSHAPE_SYNC) BSHAPE_OPTION(boct, BSHAPE_OCTAVES)
BSHAPE_OPTION(breg, BSHAPE_REGISTER) BSHAPE_OPTION(bpush, BSHAPE_PUSH) BSHAPE_OPTION(bgate, BSHAPE_GATE) BSHAPE_OPTION(bacc, BSHAPE_ACCENT)
/* 0.22 cx_1, cx_2 (PLAY's knobs, a 0.21 project's Q-Link map on knob 1), cpush, cdyn, bpush and bacc also move a slot's report (k_s1 /
 * k_b1 passing COMPLEXITY through, JAM's DENSITY, PUSH and DYNAMICS on k_s6, k_s8, k_b6, k_b8): a change bumps display_rev, so the wrapper
 * reports the slot's value and a turn of it never jumps (R26, as 0.21's mgat); the slots set them through these too */
#define REP_OPTION(name) \
    static void set_##name##_rep(seq_t *q, int v) { int was = get_##name(q); set_##name(q, v); if (get_##name(q) != was) q->display_rev++; }
REP_OPTION(cx_1) REP_OPTION(cx_2) REP_OPTION(cpush) REP_OPTION(cdyn) REP_OPTION(bpush) REP_OPTION(bacc)
REP_OPTION(cgate) REP_OPTION(bgate)   /* 0.24 knob 7's slots (k_s7, k_b7) show GATE */
static void set_cx_rep(seq_t *q, int lane, int c) { (lane == LANE_BASS ? set_cx_2_rep : set_cx_1_rep)(q, c); }
/* 0.11 the MELODY SHAPE knobs: a classic lane reads its type's starts and a turn converts it (display_rev: style_3 and cx_3 may
 * change with it); a set equal to the value read is a no-op, so MPC echoing a classic lane's knob never converts it */
static void mel_knob_to(seq_t *q, int knob, int v) {   /* 0.13: also the stepped knobs' (no Hook-default guard) */
    int style = seq_style(q, LANE_MEL_UP);
    if (v == seq_mel_knob(q, knob)) return;
    seq_mel_set_knob(q, knob, v);
    if (seq_style(q, LANE_MEL_UP) != style) q->display_rev++;
}
static void set_mel_knob(seq_t *q, int knob, int v) {
    mel_recipe_t hook;
    v = clampi(v, 0, 100);
    if (v == seq_mel_knob(q, knob)) return;
    memset(&hook, 0, sizeof hook);
    mel_defaults(MEL_HOOK, &hook, NULL);   /* params.json's defaults: a host restoring them must not convert a classic Arp */
    if (seq_mel_is_classic(q) && v == hook.kn[knob]) return;
    mel_knob_to(q, knob, v);
}
#define MKNOB_OPTION(name, what) \
    static void set_##name(seq_t *q, int v) { set_mel_knob(q, what, v); } \
    static int get_##name(const seq_t *q) { return seq_mel_knob(q, what); }
MKNOB_OPTION(mrng, MK_RANGE) MKNOB_OPTION(mlea, MK_LEAPS) MKNOB_OPTION(msyn, MK_SYNC) MKNOB_OPTION(mten, MK_TENSION)
MKNOB_OPTION(mans, MK_ANSWER) MKNOB_OPTION(mreg, MK_REGISTER)
/* 0.21 mgat is also JAM's GATE slot (k_j7): a change bumps display_rev, so the wrapper reports k_j7's step to MPC (R26) and a turn of it
 * never jumps after a 0.20 project's Q-Link map or automation moved mgat */
static void set_mgat(seq_t *q, int v) {
    int was = seq_mel_knob(q, MK_GATE);
    set_mel_knob(q, MK_GATE, v);
    if (seq_mel_knob(q, MK_GATE) != was) q->display_rev++;
}
static int get_mgat(const seq_t *q) { return seq_mel_knob(q, MK_GATE); }
/* 0.12 cslots "-", "4", "8": engine-owned, so MPC's set is ignored and the getter answers the value: FIX's slot count, "-" off FIX
 * (0.24.1 always: FIX is gone and no layout reads it) */
static void set_cslots(seq_t *q, int i) { (void)q; (void)i; }
static int get_cslots(const seq_t *q) { (void)q; return 0; }
/* 0.14 cbar "-", "1", "2", "4": the slots a bar FIX showed, engine-owned as cslots; "-" off FIX (0.24.1 always), 0.22 "Jam" (4) on
 * JAM (screen_sig's mode covers its change) */
static void set_cbar(seq_t *q, int i) { (void)q; (void)i; }
static int get_cbar(const seq_t *q) { return get_mode(q) == 3 ? 4 : 0; }
/* 0.14 cfview: FIX's bar stepper over the shared view, a whole chord a step (2 bars at 2-bar chords), "BAR 7" / "BARS 7-8"
 * (cfview_display). The value is the shown bar's step (view_bar). 0.24.1 retired (KF_RETIRED_24): a set does nothing */
static int get_cfview(const seq_t *q) { return seq_view_bar(q) / fix_span(q); }
static int cfview_text(const seq_t *q, char *buf, int len) {
    int span = fix_span(q), bar = seq_grid_bar(q) / span * span + 1;
    txt_t t = {buf, len, 0};
    add(&t, span > 1 ? "BARS " : "BAR ");
    add_int(&t, bar);
    if (span > 1) {
        add(&t, "-");
        add_int(&t, bar + 1);
    }
    return t.n;
}
/* 0.13 the stepped knobs: an option index in, the old param's value for it out (KSTEPS); the getter reports the playing
 * part's step. The set of the step reported is a no-op (MPC's echo, or a host restoring it), so a 0.12 value between
 * steps keeps playing until a different step is set. */
static void ks_set(seq_t *q, int k, int i) {
    int v;
    i = clampi(i, 0, KSTEPS[k].n - 1);
    if (i == ks_step(q, seq_part(q), k)) return;
    v = KSTEPS[k].vals[i];
    if (k == KS_HARM) seq_set_harmony(q, v);
    else if (k == KS_SPR) seq_set_chord_shape(q, SHAPE_SPREAD, v);
    else if (k <= KS_BREG) seq_set_lane_shape(q, LANE_BASS, KS_BSHAPE[k - KS_BMOV], v);
    else mel_knob_to(q, KS_MK[k - KS_MRNG], v);
}
#define KSTEP_OPTION(name, k) \
    static void set_##name(seq_t *q, int i) { ks_set(q, k, i); } \
    static int get_##name(const seq_t *q) { return ks_step(q, seq_part(q), k); }
KSTEP_OPTION(k_harm, KS_HARM) KSTEP_OPTION(k_spr, KS_SPR) KSTEP_OPTION(k_bmov, KS_BMOV) KSTEP_OPTION(k_bsyn, KS_BSYN)
KSTEP_OPTION(k_boct, KS_BOCT) KSTEP_OPTION(k_breg, KS_BREG) KSTEP_OPTION(k_mrng, KS_MRNG) KSTEP_OPTION(k_mlea, KS_MLEA)
KSTEP_OPTION(k_msyn, KS_MSYN) KSTEP_OPTION(k_mten, KS_MTEN) KSTEP_OPTION(k_mans, KS_MANS) KSTEP_OPTION(k_mreg, KS_MREG)
/* COLOUR's 17 steps (0.13, the 0.9 rank rule): retired in 0.17, k_vcol's levels replace them. It answers step 0 and a set does
 * nothing; its text is COLOUR's level, as k_vcol's. k_spr stays as it was: it sets cspr, which no longer sounds (vspr does) */
static int get_k_col(const seq_t *q) { (void)q; return 0; }
static void set_k_col(seq_t *q, int i) { (void)q; (void)i; }
/* 0.17 knobs 2-5 (VK_*): an option index in, the knob's value (index + its lowest) out; a set of the value it has is a no-op */
#define VKNOB_OPTION(name, what, lo) \
    static void set_##name(seq_t *q, int i) { seq_set_vknob(q, what, i + (lo)); } \
    static int get_##name(const seq_t *q) { return seq_vknob(q, what) - (lo); }
VKNOB_OPTION(k_vcol, VK_COL, -1) VKNOB_OPTION(k_vinv, VK_INV, -2) VKNOB_OPTION(k_vspr, VK_SPR, -1) VKNOB_OPTION(k_vstr, VK_STR, -8)
/* 0.18 the slots k_s2..k_s5 (knobs 2-5): CHORDS's VK_* (and FIX's, retired in 0.24), RHYTHM's RK_* (get_mode), as option indices; 0.19 the BASS tab's
 * k_b2..k_b5 the same by seq_bmode: BASS's MOVEMENT, OCTAVES, APPROACH, REGISTER (seq_bknob BK_*), RHYTHM's SYNC, GHOSTS, HUMANIZE, FILL
 * (seq_rknob on the Bass lane). 0.19.1 one table per tab (KSLOTS: its rows KS_CHORDS, KS_BASS) and one slot_get, slot_set, slot_text. A
 * knob with as many steps as its slot maps one to one (ruling R4); one with fewer (GHOSTS, FILL; MOVEMENT's 5 on SYNC's 6) moves a step
 * toward a set, reported at round(v x (slots - 1) / (steps - 1)) so its last step is the slot's last option. A set that changes the value
 * or lands elsewhere than asked (a scaled knob, an end stop) bumps display_rev (ruling R26): the wrapper reports the slot's value back,
 * so what it last reported is always what MPC holds and a mode change re-reports every slot that differs; MPC turns on from it. A BASS
 * knob's turn says what it does (umsg: bknob_msg). 0.22 (VST-29) a third mode, JAM (spec's knob tables), and knobs 6 and 8 (k_s6, k_s8,
 * k_b6, k_b8): a row per Force knob 2-8 (knob 7, the lane's GATE, has none); a JAM knob's turn says what it does on cjmsg / ujmsg.
 * 0.24 (VST-31) PHRASE in RHYTHM's column (the spec's knob table): SYNC, GHOSTS, HUMANIZE, CADENCE (4 steps on 17), BREATH (4 on 6), FILL,
 * PUSH; knob 7 a slot too (k_s7, k_b7: GATE in 11 steps of 10 on JAM and the own page, FILL's 5 on 11 on PHRASE) */
typedef struct { signed char knob[3], lo[3], steps[3]; unsigned char slot_n; } kslot_t;   /* per mode (0 the tab's own page, 1 RHYTHM,
                                                                                          * 0.22 2 JAM; 0.20 MELODY: 0 JAM, 1 PHRASE) */
/* 0.22 the CHORDS and BASS slots' knobs past VK_* and BK_* (xk_get, xk_put): PUSH and DYNAMICS on the own page and RHYTHM (cpush, cdyn /
 * bpush, bacc in 5 steps of 25: a value between steps reads its nearest and a set of the step it reads is MPC's echo, so the first turn
 * moves from there, as 0.21's GATE), JAM's DENSITY (the lane's COMPLEXITY, cx_1 / cx_2, the same 5 steps), VARIATION, SWING, TIME (DJ_*) */
enum { XK_PUSH = 8, XK_DYN, XK_DENS, XK_VAR, XK_SWING, XK_TIME, XK_GATE };   /* 0.24 GATE (cgate / bgate in 11 steps of 10) */
static const kslot_t KS_CHORDS[7] = {
    {{VK_COL, RK_SYNC, XK_DENS}, {-1, -2, 0}, {6, 6, 5}, 6}, {{VK_INV, RK_GHOSTS, VK_COL}, {-2, 0, -1}, {6, 5, 6}, 6},
    {{VK_SPR, RK_HUM, VK_SPR}, {-1, 0, -1}, {5, 5, 5}, 5}, {{VK_STR, RK_CAD, XK_VAR}, {-8, 0, 0}, {17, 4, 4}, 17},
    {{XK_PUSH, RK_BREATH, XK_SWING}, {0, 0, 0}, {5, 4, 6}, 6}, {{XK_GATE, RK_FILL, XK_GATE}, {0, 0, 0}, {11, 5, 11}, 11},
    {{XK_DYN, XK_PUSH, XK_TIME}, {0, 0, -1}, {5, 5, 3}, 5}};
static const kslot_t KS_BASS[7] = {
    {{BK_MOV, RK_SYNC, XK_DENS}, {0, -2, 0}, {5, 6, 5}, 6}, {{BK_OCTS, RK_GHOSTS, BK_MOV}, {0, 0, 0}, {5, 5, 5}, 5},
    {{BK_APP, RK_HUM, BK_OCTS}, {0, 0, 0}, {5, 5, 5}, 5}, {{BK_REG, RK_CAD, XK_VAR}, {-2, 0, 0}, {5, 4, 4}, 5},
    {{XK_PUSH, RK_BREATH, XK_SWING}, {0, 0, 0}, {5, 4, 6}, 6}, {{XK_GATE, RK_FILL, XK_GATE}, {0, 0, 0}, {11, 5, 11}, 11},
    {{XK_DYN, XK_PUSH, XK_TIME}, {0, 0, -1}, {5, 5, 3}, 5}};
/* 0.20 the MELODY tab's knobs 2-5. 0.21 (VST-28) knobs 2-8: JAM DENSITY (-2..2: 5 steps on 6), RANGE (-1..2), REGISTER (3 on 4), VARIATION,
 * SWING, GATE (11 steps), TIME (Amendment 1: -1..1, 3 on 5); PHRASE ANSWER, STRAY, OCTAVE (-1..1, 3 on 4), CADENCE, BREATH (4 on 6), FILL
 * (4 on 11), PUSH (5). SWING, GATE and PUSH are the engine's fields past MJS_COUNT (mj_get, mj_put) */
enum { MJE_SWING = MJS_COUNT, MJE_GATE, MJE_PUSH, MJE_COUNT };
static const kslot_t KS_MEL[7] = {{{MJS_DENS, MJS_ANS}, {-2, 0}, {5, 6}, 6}, {{MJS_RNG, MJS_STRAY}, {-1, 0}, {4, 4}, 4},
                                  {{MJS_REG, MJS_AOCT}, {0, -1}, {3, 3}, 4}, {{MJS_VAR, MJS_CAD}, {0, 0}, {4, 4}, 4},
                                  {{MJE_SWING, MJS_BREATH}, {0, 0}, {6, 4}, 6}, {{MJE_GATE, MJS_FILL}, {0, 0}, {11, 4}, 11},
                                  {{MJS_TIME, MJE_PUSH}, {-1, 0}, {3, 5}, 5}};
/* 0.21 a MELODY slot's field: seq_mjam_get/set's MJS_*, or SWING 0..5, GATE mgat's value in steps of 10 (0..10) and PUSH
 * jpush's in steps of 25 (0..4). GATE and PUSH read the nearest step (a loaded 73 is 7) and a set of the step they read is MPC's echo,
 * which leaves the value, so the first turn moves from there; their sets go through mgat's and jpush's paths */
static int mj_get(const seq_t *q, int f) {
    switch (f) {
    case MJE_SWING: return seq_mel_swing(q);
    case MJE_GATE: return (int)lround(seq_mel_knob(q, MK_GATE) / 10.0);
    case MJE_PUSH: return (int)lround(seq_mel_push(q) / 25.0);
    default: return seq_mjam_get(q, f);
    }
}
static void mj_put(seq_t *q, int f, int v) {
    if (f == MJE_SWING) seq_set_mel_swing(q, v);
    else if (f == MJE_GATE) {
        if (v != mj_get(q, f)) set_mgat(q, clampi(v, 0, 10) * 10);
    } else if (f == MJE_PUSH) {
        if (v != mj_get(q, f)) seq_set_mel_push(q, clampi(v, 0, 4) * 25);
    } else seq_mjam_set(q, f, v);
}
/* 0.22 the CHORDS and BASS tabs' extra knobs (xk_*) and their own-page / JAM texts and turns */
static int xk_get(const seq_t *q, int lane, int k) {
    int bass = lane == LANE_BASS;
    switch (k) {
    case XK_PUSH: return (int)lround((bass ? seq_lane_shape(q, LANE_BASS, BSHAPE_PUSH) : seq_chord_shape(q, SHAPE_PUSH)) / 25.0);
    case XK_DYN: return (int)lround((bass ? seq_lane_shape(q, LANE_BASS, BSHAPE_ACCENT) : seq_chord_shape(q, SHAPE_DYN)) / 25.0);
    case XK_GATE: return (int)lround((bass ? seq_lane_shape(q, LANE_BASS, BSHAPE_GATE) : seq_chord_shape(q, SHAPE_GATE)) / 10.0);
    case XK_DENS: return (int)lround(seq_complexity(q, lane) / 25.0);
    case XK_VAR: return seq_djam_get(q, lane, DJ_VAR);
    case XK_SWING: return seq_djam_get(q, lane, DJ_SWING);
    case XK_TIME: return seq_djam_get(q, lane, DJ_TIME);
    default: return 0;
    }
}
static void set_cx_rep(seq_t *q, int lane, int c);
static void set_cpush_rep(seq_t *q, int v);
static void set_cdyn_rep(seq_t *q, int v);
static void set_bpush_rep(seq_t *q, int v);
static void set_bacc_rep(seq_t *q, int v);
static void set_cgate_rep(seq_t *q, int v);
static void set_bgate_rep(seq_t *q, int v);
static void xk_put(seq_t *q, int lane, int k, int v) {
    if (k == XK_VAR || k == XK_SWING || k == XK_TIME) {
        seq_djam_set(q, lane, k == XK_VAR ? DJ_VAR : k == XK_SWING ? DJ_SWING : DJ_TIME, v);
        return;
    }
    if (v == xk_get(q, lane, k)) return;   /* the step it reads: MPC's echo (a loaded 37 stays 37) */
    if (k == XK_GATE) {   /* 0.24 in steps of 10 (0.21 MELODY's GATE) */
        (lane == LANE_BASS ? set_bgate_rep : set_cgate_rep)(q, clampi(v, 0, 10) * 10);
        return;
    }
    v = clampi(v, 0, 4) * 25;
    if (k == XK_DENS) set_cx_rep(q, lane, v);
    else if (lane == LANE_BASS) (k == XK_PUSH ? set_bpush_rep : set_bacc_rep)(q, v);
    else (k == XK_PUSH ? set_cpush_rep : set_cdyn_rep)(q, v);
}
static int ck_get(const seq_t *q, int k) { return k < VK_COUNT ? seq_vknob(q, k) : xk_get(q, LANE_CHORDS, k); }
static void ck_put(seq_t *q, int k, int v) { if (k < VK_COUNT) seq_set_vknob(q, k, v); else xk_put(q, LANE_CHORDS, k, v); }
static int bk_get(const seq_t *q, int k) { return k < BK_COUNT ? seq_bknob(q, k) : xk_get(q, LANE_BASS, k); }
static void bk_put(seq_t *q, int k, int v) { if (k < BK_COUNT) seq_set_bknob(q, k, v); else xk_put(q, LANE_BASS, k, v); }
static int chords_kmode(const seq_t *q) { int m = get_mode(q); return m == 1 ? 1 : m == 3 ? 2 : 0; }   /* KSLOTS' modes; get_mode's 2 (FIX) never comes */
typedef struct {
    const kslot_t *slot;                                        /* KS_CHORDS, KS_BASS, KS_MEL */
    int lane;                                                   /* RHYTHM's knobs' (seq_rknob); 0.20 -1: both modes' are get/put's */
    int (*mode)(const seq_t *);                                 /* 0 the tab's own page, 1 RHYTHM, 0.22 2 JAM (MELODY: seq_mmode, 0 JAM,
                                                                 * 1 PHRASE) */
    int (*get)(const seq_t *, int knob);                        /* the own page's (0.22 and JAM's) knobs: ck_get, bk_get (mj_get) */
    void (*put)(seq_t *, int knob, int v);
    int (*text)(const seq_t *, int knob, char *buf, int len);   /* ck_text, bkx_text, mj_slot_text */
    void (*turned)(seq_t *, int knob);                          /* a set moved a knob that isn't RHYTHM's: ck_turned, bk_turned,
                                                                 * mknob_turned (either mode's) */
    const char *name[3][7];                                     /* MPC's names by mode (params.json dynamic_name; 0.21 MELODY's 7,
                                                                 * 0.22 knobs 2-8 a tab, knob 7 none) */
} kslots_t;
static int vk_text(const seq_t *q, int vk, char *buf, int len);
static int bk_text(const seq_t *q, int k, char *buf, int len);
static int jk_text(const seq_t *q, int d, int k, char *buf, int len);
static void jknob_msg(seq_t *q, int d, int k);
static int xk_text(const seq_t *q, int lane, int k, char *buf, int len) {   /* 0.22 PUSH and DYNAMICS, 0.24 GATE as cpush's .. bgate's texts */
    int bass = lane == LANE_BASS, kt = k == XK_PUSH ? (bass ? KT_BPUSH : KT_CPUSH) : k == XK_GATE ? (bass ? KT_BGATE : KT_CGATE) : bass ? KT_BACC : KT_CDYN;
    int v = bass ? seq_lane_shape(q, LANE_BASS, k == XK_PUSH ? BSHAPE_PUSH : k == XK_GATE ? BSHAPE_GATE : BSHAPE_ACCENT)
                 : seq_chord_shape(q, k == XK_PUSH ? SHAPE_PUSH : k == XK_GATE ? SHAPE_GATE : SHAPE_DYN);
    knob_format(kt, v, buf, len);
    return (int)strlen(buf);
}
static int ck_text(const seq_t *q, int k, char *buf, int len) {
    if (chords_kmode(q) == 2) return jk_text(q, DL_CHORDS, k, buf, len);
    return k < VK_COUNT ? vk_text(q, k, buf, len) : xk_text(q, LANE_CHORDS, k, buf, len);
}
static int bkx_text(const seq_t *q, int k, char *buf, int len) {   /* bk_text */
    if (seq_bmode(q) == 2) return jk_text(q, DL_BASS, k, buf, len);
    return k < BK_COUNT ? bk_text(q, k, buf, len) : xk_text(q, LANE_BASS, k, buf, len);
}
static void ck_turned(seq_t *q, int k) { if (chords_kmode(q) == 2 && k != XK_GATE) jknob_msg(q, DL_CHORDS, k), q->display_rev++; }   /* cjmsg;
                                                                                                                      * 0.24 GATE none, as 0.22's cgate */
static void bk_turned(seq_t *q, int k) {   /* umsg (BASS's own knobs), 0.22 ujmsg (JAM's): re-read on display_rev */
    if (seq_bmode(q) == 2 && k != XK_GATE) jknob_msg(q, DL_BASS, k);
    else if (k < BK_COUNT) bknob_msg(q, k);
    q->display_rev++;
}
static int mj_slot_text(const seq_t *q, int f, char *buf, int len);
static void mknob_msg(seq_t *q, int f);
static void mknob_turned(seq_t *q, int f) { mknob_msg(q, f), q->display_rev++; }   /* jmsg, as umsg */
/* k_s2..5's names (the wrapper's effGetParamName asks "<key>_name"; MPC reads names again on the UpdateDisplay a mode change's
 * display_rev brings): the mode's knob, as the other knobs' names ("Chords Push"); k_b2..5's the same; 0.22 k_s6, k_s8, k_b6, k_b8 too, 0.24
 * k_s7, k_b7 and PHRASE's ("Chords Cadence", "Bass Breath"); MELODY's knob 4 "Melody Octave" (JAM: REGISTER renamed) and "Melody Answer
 * Octave" (PHRASE) */
static const kslots_t KSLOTS[TAB_COUNT] = {
    {KS_CHORDS, LANE_CHORDS, chords_kmode, ck_get, ck_put, ck_text, ck_turned,
     {{"Chords Colour", "Chords Inversion", "Chords Spread", "Chords Strum", "Chords Push", "Chords Gate", "Chords Dynamics"},
      {"Chords Sync", "Chords Ghosts", "Chords Humanize", "Chords Cadence", "Chords Breath", "Chords Fill", "Chords Push"},
      {"Chords Density", "Chords Colour", "Chords Spread", "Chords Variation", "Chords Swing", "Chords Gate", "Chords Time"}}},
    {KS_BASS, LANE_BASS, seq_bmode, bk_get, bk_put, bkx_text, bk_turned,
     {{"Bass Movement", "Bass Octaves", "Bass Approach", "Bass Register", "Bass Push", "Bass Gate", "Bass Dynamics"},
      {"Bass Sync", "Bass Ghosts", "Bass Humanize", "Bass Cadence", "Bass Breath", "Bass Fill", "Bass Push"},
      {"Bass Density", "Bass Movement", "Bass Octaves", "Bass Variation", "Bass Swing", "Bass Gate", "Bass Time"}}},
    {KS_MEL, -1, seq_mmode, mj_get, mj_put, mj_slot_text, mknob_turned,
     {{"Melody Density", "Melody Range", "Melody Octave", "Melody Variation", "Melody Swing", "Melody Gate", "Melody Time"},
      {"Melody Answer", "Melody Stray", "Melody Answer Octave", "Melody Cadence", "Melody Breath", "Melody Fill", "Melody Push"}}}};
static int slot_scale(int v, int steps, int n) { return steps == n ? v : (int)lround(v * (n - 1) / (double)(steps - 1)); }   /* R4 */
static int slot_rk(const kslots_t *T, int m, int knob) { return m == 1 && T->lane >= 0 && knob < XK_PUSH; }   /* a RHYTHM knob (seq_rknob) */
static int slot_raw(const seq_t *q, const kslots_t *T, int j, int *steps) {   /* the mode's knob as a step 0.., and its step count */
    const kslot_t *k = &T->slot[j];
    int m = T->mode(q);
    *steps = k->steps[m];
    return (slot_rk(T, m, k->knob[m]) ? seq_rknob(q, T->lane, k->knob[m]) : T->get(q, k->knob[m])) - k->lo[m];
}
static int slot_get(const seq_t *q, const kslots_t *T, int j) { int n, v = slot_raw(q, T, j, &n); return slot_scale(v, n, T->slot[j].slot_n); }
static void slot_put(seq_t *q, const kslots_t *T, int j, int v) {   /* step v of the mode's knob */
    const kslot_t *k = &T->slot[j];
    int m = T->mode(q);
    if (slot_rk(T, m, k->knob[m])) seq_set_rknob(q, T->lane, k->knob[m], v + k->lo[m]);
    else T->put(q, k->knob[m], v + k->lo[m]);
}
static void slot_set(seq_t *q, const kslots_t *T, int j, int i) {
    int n, v = slot_raw(q, T, j, &n), cur = slot_scale(v, n, T->slot[j].slot_n), m = T->mode(q), k = T->slot[j].knob[m];
    if (n == T->slot[j].slot_n) slot_put(q, T, j, i);
    else if (i != cur) slot_put(q, T, j, v + (i > cur ? 1 : -1));
    if (T->turned && !slot_rk(T, m, k) && slot_raw(q, T, j, &n) != v) T->turned(q, k);
    if (slot_get(q, T, j) != i || slot_get(q, T, j) != cur) q->display_rev++;
}
#define SLOT_OPTION(t, n, tab) \
    static void set_k_##t##n(seq_t *q, int i) { slot_set(q, &KSLOTS[tab], n - 2, i); } \
    static int get_k_##t##n(const seq_t *q) { return slot_get(q, &KSLOTS[tab], n - 2); }
SLOT_OPTION(s, 2, TAB_CHORDS) SLOT_OPTION(s, 3, TAB_CHORDS) SLOT_OPTION(s, 4, TAB_CHORDS) SLOT_OPTION(s, 5, TAB_CHORDS)
SLOT_OPTION(b, 2, TAB_BASS) SLOT_OPTION(b, 3, TAB_BASS) SLOT_OPTION(b, 4, TAB_BASS) SLOT_OPTION(b, 5, TAB_BASS)
SLOT_OPTION(j, 2, TAB_MEL) SLOT_OPTION(j, 3, TAB_MEL) SLOT_OPTION(j, 4, TAB_MEL) SLOT_OPTION(j, 5, TAB_MEL)
SLOT_OPTION(j, 6, TAB_MEL) SLOT_OPTION(j, 7, TAB_MEL) SLOT_OPTION(j, 8, TAB_MEL)
SLOT_OPTION(s, 6, TAB_CHORDS) SLOT_OPTION(s, 8, TAB_CHORDS) SLOT_OPTION(b, 6, TAB_BASS) SLOT_OPTION(b, 8, TAB_BASS)   /* 0.22 */
SLOT_OPTION(s, 7, TAB_CHORDS) SLOT_OPTION(b, 7, TAB_BASS)   /* 0.24 */
/* 0.17 cview and cdrift: engine-owned, as cslots (a set from MPC is ignored); cdrift is the centre chord's drift level. 0.18 rview
 * "-", "Edit": Edit on RHYTHM's Rhythm and Bar views, so the skin draws the two as one picture (ruling R29); it follows cview, which
 * screen_sig reports */
static int get_cdrift(const seq_t *q) { return inst_of(q)->snd.drift; }   /* 0.24 the sound page's centre chord's (sound_cells_now) */
static int get_rview(const seq_t *q) { return seq_cview(q) == CVIEW_RHYTHM || seq_cview(q) == CVIEW_BAR; }
/* 0.19 yview "Bass", "Edit", "Other": the BASS tab's skin groups, engine-set (as rview): Bass on the BASS page's views (Bass, Edit,
 * Style: its strip), Edit on BASS RHYTHM's Rhythm and Bar (one picture for both), Other on its Copy and Rhythms; follows bview. 0.22 Jam
 * on JAM (no strip) */
static int get_yview(const seq_t *q) {
    int v = seq_bview(q);
    return v == BVIEW_RHYTHM || v == BVIEW_BAR ? 1 : v == BVIEW_COPY || v == BVIEW_RBROWSE ? 2 : v == BVIEW_JAM ? 3 : 0;   /* 0.22 Jam */
}
/* 0.20 mgroup "Jam", "Phrase", "Edit": the MELODY tab's skin groups, engine-set (as yview): Phrase on PHRASE and its picker; follows mview */
static int get_mgroup(const seq_t *q) { int v = seq_mview(q); return v == MVIEW_JAM ? 0 : v == MVIEW_EDIT ? 2 : 1; }
static void set_jpush(seq_t *q, int v) {   /* 0.21 also PHRASE's PUSH slot (k_j8): a change bumps display_rev, as mgat's */
    int was = seq_mel_push(q);
    seq_set_mel_push(q, clampi(v, 0, 100));
    if (seq_mel_push(q) != was) q->display_rev++;
}
static void set_jdyn(seq_t *q, int v) { (void)q, (void)v; }   /* 0.21 a no-op (it still answers: a 0.20 project's Q-Link map turns it):
                                                               * the velocities are ACCENT LIGHT's, fixed */
/* 0.20 cx_3 on a JAM Melody lane (header comment, 0.20): one level a Q-Link step. MPC sends a Q-Link event or a wheel click as the value
 * it holds plus a little: the one the wrapper reported, or in a spin its own running value (the report comes up to 100 ms later, MPC
 * reads back after about a second idle), which the wrapper's settle() rounds toward the reported value, so the same whole number can come
 * twice. A set within MEL_TICK of the value reported, or of the one MPC sent last (within CX3_TURN_FRAMES), moves one level its way; that
 * last value again continues the turn's way; the value reported is an echo; a farther set (a drag, automation) goes to its band. A lane
 * that isn't JAM: cx_3 as before */
#define MEL_TICK 4
#define CX3_TURN_FRAMES (3 * 44100)   /* a turn's last value counts for 3 s: MPC reads back after "about a second" idle (docs/NOTES.md, a
                                       * Key 37), so a tick before its read-back still continues the turn. No later tick collides: a
                                       * read-back one lands within MEL_TICK of the value reported (checked first), and the last value
                                       * sent is 20 x levels away from it once the level moved */
static void set_cx_mel(seq_t *q, int c) {
    inst_t *s = inst_rw(q);
    int cur = seq_complexity(q, LANE_MEL_UP), turning = s->cx3_dir && s->frames - s->cx3_at < CX3_TURN_FRAMES, dir, to;
    if (seq_style(q, LANE_MEL_UP) != GEN_MEL_JAM) {
        set_cx_3(q, c);
        return;
    }
    c = clampi(c, 0, 100);
    if (c == cur) return;   /* an echo */
    if (turning && c == s->cx3_sent) dir = s->cx3_dir;   /* the running value rounded back to the last one: the turn goes on */
    else if (abs(c - cur) <= MEL_TICK) dir = c > cur ? 1 : -1;
    else if (turning && abs(c - s->cx3_sent) <= MEL_TICK) dir = c > s->cx3_sent ? 1 : -1;
    else dir = 0;
    to = dir ? seq_mel_level(q) + dir : c / 20;
    s->cx3_sent = c, s->cx3_dir = dir, s->cx3_at = s->frames;
    seq_set_complexity(q, LANE_MEL_UP, 20 * clampi(to, 0, MJ_LEVELS - 1) + MEL_JAM_CX);
    if (seq_complexity(q, LANE_MEL_UP) != c) q->display_rev++;   /* R26: MPC holds what the wrapper reports */
}
/* 0.22 knob 1 on the CHORDS and BASS tabs (k_s1, k_b1; header comment, 0.22): on the own page and RHYTHM COMPLEXITY passed through (cx_1,
 * cx_2); on JAM the row of the block queued, else playing, as 20 x level + 10 (none yet: level 1, 10), one level a Q-Link step by
 * set_cx_mel's rule, each knob with its own turn memory (k1_sent, k1_dir, k1_at). The level queues in that block's column (stopped: at
 * once; the playing level again cancels a queue) as a block tap does, saying so on the lane's JAM line; at an end stop nothing changes.
 * Before any block it reads level 1 and a turn up starts HOUSE level 2, any other set HOUSE level 1: a start whose report stayed 10 would
 * leave MPC's next tick (the same 10.78 after its read-back) to the wrapper's settle as 10, an echo, so a slow turn up would stop there */
static int k1_mode(const seq_t *q, int d) { return d == DL_BASS ? seq_bmode(q) : chords_kmode(q); }
static int k1_get(const seq_t *q, int d) {
    int c, l;
    if (k1_mode(q, d) != 2) return seq_complexity(q, dl_lane(d));
    jam_shown(q, dl_lane(d), &c, &l);
    return 20 * l + 10;
}
static void k1_set(seq_t *q, int d, int v) {
    inst_t *s = inst_rw(q);
    int cur = k1_get(q, d), turning = s->k1_dir[d] && s->frames - s->k1_at[d] < CX3_TURN_FRAMES, dir, c, l, any, to;
    if (k1_mode(q, d) != 2) {
        set_cx_rep(q, dl_lane(d), v);
        return;
    }
    v = clampi(v, 0, 100);
    if (v == cur) return;   /* an echo */
    if (turning && v == s->k1_sent[d]) dir = s->k1_dir[d];
    else if (abs(v - cur) <= MEL_TICK) dir = v > cur ? 1 : -1;
    else if (turning && abs(v - s->k1_sent[d]) <= MEL_TICK) dir = v > s->k1_sent[d] ? 1 : -1;
    else dir = 0;
    s->k1_sent[d] = v, s->k1_dir[d] = dir, s->k1_at[d] = s->frames;
    any = jam_shown(q, dl_lane(d), &c, &l);
    to = !any ? dir > 0 : clampi(dir ? l + dir : v / 20, 0, JAM_LEVELS - 1);
    if (!any || to != l) jam_tap(q, d, c, to);
    if (k1_get(q, d) != v) q->display_rev++;   /* R26 */
}
static void set_k_s1(seq_t *q, int v) { k1_set(q, DL_CHORDS, v); }
static int get_k_s1(const seq_t *q) { return k1_get(q, DL_CHORDS); }
static void set_k_b1(seq_t *q, int v) { k1_set(q, DL_BASS, v); }
static int get_k_b1(const seq_t *q) { return k1_get(q, DL_BASS); }

/* length 0..4 = 1..16 bars, chord_len 0..3 = 32..4 steps, view 0..15 = Bar 1..16, key 0..11 = C..B, scale 0..6,
 * prog 0..15 = the presets and 16 = Custom, voicing 0..1 = Close, Smooth, style_N 0..3 (0..2 on the Melodies), cx_N and harmony 0..100,
 * ch_N 0..15 = channels 1..16, swing, gate and dyn 0..100, cmode 0..3 (0.22 Jam), the SHAPE knobs ccol..cdyn 0..100
 * (seq_set_chord_shape clamps),
 * bmode 0..2, the BASS SHAPE knobs bmov..bacc 0..100 (seq_set_lane_shape clamps), mmode 0..3, the MELODY knobs mrng..mgat 0..100,
 * 0.12 style_1 0..9 and cslots 0..2 (engine-set), 0.13 the stepped knobs k_harm..k_mreg (KSTEPS; k_col 0..16), 0.14 cbar 0..4 (0.22 Jam)
 * (engine-set) and cfview 0..15, 0.17 cview 0..5 and cdrift 0..3 (engine-set) and k_vcol..k_vstr (VK_*: 0..5, 0..5, 0..4, 0..16), 0.18
 * cview 0..8, k_s2..k_s5 (KS_CHORDS: 0..5, 0..5, 0..4, 0..16) and rview 0..1 (engine-set), 0.19 bview 0..6 and yview 0..2 (engine-set),
 * 0.20 mview 0..3 and mgroup 0..2 (engine-set), jpush and jdyn 0..100, k_j2..k_j5 (KS_MEL: 0..5, 0..3, 0..3, 0..3), 0.21 k_j6..k_j8
 * (0..5, 0..10, 0..4), 0.22 cview 0..9, bview 0..7 and yview 0..3 (engine-set). */
static const option_t OPTIONS[] = {
    {"length", set_length, get_length}, {"chord_len", set_chord_len, get_chord_len},
    {"view", set_view, seq_view_bar}, {"key", seq_set_key, seq_key}, {"scale", seq_set_scale, seq_scale},
    {"prog", set_prog, get_prog}, {"voicing", set_voicing, seq_voicing},
    {"style_1", set_style_1, get_style_1}, {"style_2", set_style_2, get_style_2},
    {"style_3", set_style_3, get_style_3}, {"style_4", set_style_4, get_style_4},
    {"cx_1", set_cx_1_rep, get_cx_1}, {"cx_2", set_cx_2_rep, get_cx_2}, {"cx_3", set_cx_mel, get_cx_3}, {"cx_4", set_cx_4, get_cx_4},
    {"harmony", set_harmony, seq_harmony},
    {"ch_1", set_ch_1, get_ch_1}, {"ch_2", set_ch_2, get_ch_2}, {"ch_3", set_ch_3, get_ch_3},
    {"swing", set_swing, seq_swing}, {"gate", set_gate, seq_gate}, {"dyn", set_dyn, seq_dynamics},
    {"cmode", set_cmode, get_cmode},
    {"ccol", set_ccol, get_ccol}, {"cspr", set_cspr, get_cspr}, {"cpush", set_cpush_rep, get_cpush},
    {"cgate", set_cgate_rep, get_cgate}, {"cstrum", set_cstrum, get_cstrum}, {"cdyn", set_cdyn_rep, get_cdyn},
    {"bmode", set_bmode, get_bmode},
    {"bmov", set_bmov, get_bmov}, {"bsync", set_bsync, get_bsync}, {"boct", set_boct, get_boct}, {"breg", set_breg, get_breg},
    {"bpush", set_bpush_rep, get_bpush}, {"bgate", set_bgate_rep, get_bgate}, {"bacc", set_bacc_rep, get_bacc},
    {"mmode", set_mmode, get_mmode},
    {"mrng", set_mrng, get_mrng}, {"mlea", set_mlea, get_mlea}, {"msyn", set_msyn, get_msyn}, {"mten", set_mten, get_mten},
    {"mans", set_mans, get_mans}, {"mreg", set_mreg, get_mreg}, {"mgat", set_mgat, get_mgat}, {"cslots", set_cslots, get_cslots},
    {"cbar", set_cbar, get_cbar}, {"cfview", set_cslots, get_cfview}, {"cview", set_cslots, seq_cview}, {"cdrift", set_cslots, get_cdrift},
    {"rview", set_cslots, get_rview}, {"bview", set_cslots, seq_bview}, {"yview", set_cslots, get_yview},
    {"mview", set_cslots, seq_mview}, {"mgroup", set_cslots, get_mgroup}, {"jpush", set_jpush, seq_mel_push}, {"jdyn", set_jdyn, seq_mel_dyn}};
/* 0.13 the stepped knobs, a table of their own ("k_" keys): every polled readout misses OPTIONS, and need not miss these too */
static const option_t KOPTIONS[] = {
    {"k_harm", set_k_harm, get_k_harm}, {"k_col", set_k_col, get_k_col}, {"k_spr", set_k_spr, get_k_spr},
    {"k_bmov", set_k_bmov, get_k_bmov}, {"k_bsyn", set_k_bsyn, get_k_bsyn}, {"k_boct", set_k_boct, get_k_boct},
    {"k_breg", set_k_breg, get_k_breg}, {"k_mrng", set_k_mrng, get_k_mrng}, {"k_mlea", set_k_mlea, get_k_mlea},
    {"k_msyn", set_k_msyn, get_k_msyn}, {"k_mten", set_k_mten, get_k_mten}, {"k_mans", set_k_mans, get_k_mans},
    {"k_mreg", set_k_mreg, get_k_mreg}, {"k_vcol", set_k_vcol, get_k_vcol}, {"k_vinv", set_k_vinv, get_k_vinv},
    {"k_vspr", set_k_vspr, get_k_vspr}, {"k_vstr", set_k_vstr, get_k_vstr}, {"k_s2", set_k_s2, get_k_s2}, {"k_s3", set_k_s3, get_k_s3},
    {"k_s4", set_k_s4, get_k_s4}, {"k_s5", set_k_s5, get_k_s5}, {"k_b2", set_k_b2, get_k_b2}, {"k_b3", set_k_b3, get_k_b3},
    {"k_b4", set_k_b4, get_k_b4}, {"k_b5", set_k_b5, get_k_b5}, {"k_j2", set_k_j2, get_k_j2}, {"k_j3", set_k_j3, get_k_j3},
    {"k_j4", set_k_j4, get_k_j4}, {"k_j5", set_k_j5, get_k_j5}, {"k_j6", set_k_j6, get_k_j6}, {"k_j7", set_k_j7, get_k_j7},
    {"k_j8", set_k_j8, get_k_j8},
    {"k_s1", set_k_s1, get_k_s1}, {"k_s6", set_k_s6, get_k_s6}, {"k_s8", set_k_s8, get_k_s8}, {"k_b1", set_k_b1, get_k_b1},
    {"k_b6", set_k_b6, get_k_b6}, {"k_b8", set_k_b8, get_k_b8},   /* 0.22: knob 1 (a number, 0..100) a "k_" key and guarded too */
    {"k_s7", set_k_s7, get_k_s7}, {"k_b7", set_k_b7, get_k_b7}};   /* 0.24 knob 7 */

/* params.json's default step of each KOPTIONS entry (vst/make_skin.py STEPPED and VKNOBS, 0.18 the slots: VKNOBS's again, 0.19 BKNOBS,
 * 0.20 MKNOBS, 0.21 its JAM defaults and MKNOBS_21, Amendment 1 k_j8 TIME NORMAL 2; 0.22 KNOBS_22 and knob 1's value 10; tests/test_plugin.c
 * checks they agree) */
static const int KDEFAULT[] = {0, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0, 1, 1, 1, 2, 1, 8, 3, 1, 1, 0, 3, 0, 0, 0, 3, 1, 2, 0, 0, 5, 2,
                               10, 0, 2, 10, 0, 2, 5, 5};   /* 0.22 k_s2..5, k_b2..5: JAM's options (vst/make_skin.py JAM_DEFAULTS_22);
                                                             * 0.24 k_s7, k_b7 JAM's GATE 50 (KNOBS_24) */
_Static_assert(sizeof KDEFAULT / sizeof KDEFAULT[0] == sizeof KOPTIONS / sizeof KOPTIONS[0], "a default per stepped knob");
_Static_assert(sizeof KOPTIONS / sizeof KOPTIONS[0] < 64, "the load guard holds a bit per stepped knob (inst_t.kguard)");

/* The load guard (inst_t.kguard): 1 if this set of a k_* key is a host restoring its default after a load (ignored). A set of
 * another value disarms the key. */
static int kguard_skips(inst_t *s, const char *key, int i) {
    for (size_t k = 0; s->kguard && k < sizeof KOPTIONS / sizeof KOPTIONS[0]; k++) {
        if (!(s->kguard >> k & 1) || strcmp(key, KOPTIONS[k].key)) continue;
        if (i == KDEFAULT[k]) return 1;
        s->kguard &= ~(1ull << k);
    }
    return 0;
}

/* The option table a key would be in, and its size */
static const option_t *options_of(const char *key, size_t *n) {
    int k = key[0] == 'k' && key[1] == '_';
    *n = k ? sizeof KOPTIONS / sizeof KOPTIONS[0] : sizeof OPTIONS / sizeof OPTIONS[0];
    return k ? KOPTIONS : OPTIONS;
}

/* Returns 1 if `key` was an option. */
static int set_option(seq_t *q, const char *key, int i) {
    size_t n;
    const option_t *t = options_of(key, &n);
    for (size_t k = 0; k < n; k++)
        if (!strcmp(key, t[k].key)) { t[k].set(q, i); return 1; }
    return 0;
}

static int get_option(const seq_t *q, const char *key, char *buf, int len) {
    size_t n;
    const option_t *t = options_of(key, &n);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(key, t[i].key)) return snprintf(buf, len, "%d", t[i].get(q));
    return 0;
}

/* Momentary triggers other than panic: the index of `key` in TRIGGERS, or -1. */
static const char *const TRIGGERS[] = {"gen_1", "gen_2", "gen_3", "gen_4", "gen_prog", "undo"};

static int trigger_index(const char *key) {
    for (int n = 0; n < (int)(sizeof TRIGGERS / sizeof TRIGGERS[0]); n++)
        if (!strcmp(key, TRIGGERS[n])) return n;
    return -1;
}

static void fire_trigger(seq_t *q, int n) {
    if (n < SEQ_LANES) seq_generate(q, n);
    else if (n == SEQ_LANES) seq_generate_progression(q);
    else seq_undo(q);
}

/* 0.17 what MPC re-reads only when display_rev changes, as one number: cview (4 bits, 0.18), the mode (the slots k_s2..5 follow it),
 * cdrift and the four tints (3 bits each); 0.19 bview (3 bits) and the BASS tab's mode (k_b2..5 follow it); 0.20 mview (2 bits, mgroup
 * follows it) and the MELODY tab's mode (k_j2..5); 0.22 the CHORDS mode's 2 bits take JAM (3), the BASS mode moves to bits 27-28 */
static int screen_sig(const seq_t *q) {
    int sig = seq_cview(q) | get_mode(q) << 4 | get_cdrift(q) << 6;
    for (int i = 0; i < 4; i++) sig |= tint_of(q, i) << (8 + 3 * i);
    sig |= seq_bview(q) << 20 | seq_bmode(q) << 27;   /* 0.22 bmode 0..2: 2 bits */
    sig |= seq_mview(q) << 24 | seq_mmode(q) << 26;
    return sig;
}

/* 0.17 (ruling 24) a set of a key that acts keeps an open picker or browser first (DONE), so its CANCEL or UNDO never takes back an
 * edit made elsewhere, unless the key is the open transaction's tab's (the CHORDS tab's when none is open): 0.19.1 its flags
 * (KF_KEEP_CHORDS, KF_KEEP_BASS: vst/make_skin.py KEEP_BOTH .. key_flags, from the pages each key is on). pt_'s switch cancels it
 * (the core), as a load and INIT do, and md_ / cmode (0.9's cvm_) change the view, which cancels; the engine-set options and panic
 * are no edit */
static int txn_key(const seq_t *q, skey_t k) {
    int keep = !q->txn.kind ? KF_KEEP_CHORDS : q->txn.lane == LANE_BASS ? KF_KEEP_BASS : q->txn.lane == LANE_MEL_UP ? KF_KEEP_MEL : KF_KEEP_CHORDS;
    return k.flags & keep;
}

/* An option's value as its getter reports it; 0 if `key` is no option */
static int option_value(const seq_t *q, const char *key, int *v) {
    size_t n;
    const option_t *t = options_of(key, &n);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(key, t[i].key)) { *v = t[i].get(q); return 1; }
    return 0;
}

/* 1 if a set of k_* key `key` to step i is a host restoring its default after a load (kguard_skips, without disarming) */
static int kguard_holds(const inst_t *s, const char *key, int i) {
    for (size_t k = 0; s->kguard && k < sizeof KOPTIONS / sizeof KOPTIONS[0]; k++)
        if (!strcmp(key, KOPTIONS[k].key)) return (s->kguard >> k & 1) && i == KDEFAULT[k];
    return 0;
}

/* 1 if this set acts: a tap (a select cell's release does nothing), a trigger, or a cell, mute or option set to a value other than
 * it reads (a host's echo is no edit) */
static int set_acts(const inst_t *s, const char *key, skey_t k, const char *val) {
    const seq_t *q = &s->seq;
    int v = atoi(val), cur;
    if (k.on) return 0;
    if (k.kind == K_TAP) return v > 0 || !TAPS[k.a].select;
    if (k.kind == K_CELL) return (v > 0) != seq_step_on(q, k.a, k.b);
    if (k.kind == K_MUTE) return (v > 0) != q->mute[k.a];
    if (trigger_index(key) >= 0) return v > 0;
    if (key[0] == 'k' && key[1] == '_' && kguard_holds(s, key, v)) return 0;
    return option_value(q, key, &cur) && v != cur;
}

/* 0.18 an image button cell's press latch (see the TAPS comment), NULL for any other key */
static unsigned long long *latch_of(inst_t *s, skey_t k) {
    const tap_t *t = k.kind == K_TAP ? &TAPS[k.a] : NULL;
    return t && t->btn && k.b >= 0 && k.b < TAP_BTN_CELLS && (t->mask >> k.b & 1) ? &s->latch_at[t->btn - 1][k.b] : NULL;
}
static int latched(inst_t *s, skey_t k) {
    unsigned long long *at = latch_of(s, k);
    return at && *at && s->frames + 1 - *at < TAP_LATCH_FRAMES;
}
/* 1 if this set of an image button cell does nothing because the cell is latched (a 1 while held). A 0 clears the latch (and,
 * select 1, does nothing anyway); a 1 that acts latches it */
static int tap_latch(inst_t *s, skey_t k, int v) {
    unsigned long long *at = k.on ? NULL : latch_of(s, k);
    if (!at) return 0;
    if (!v) *at = 0;
    else if (latched(s, k)) return 1;
    else *at = s->frames + 1;
    return 0;
}

#ifdef TAP_LOG
/* Diagnostic build only (CFLAGS_EXTRA=-DTAP_LOG vst/build.sh): one line per set of an image-button key (and cst_) to
 * /tmp/chordseq-taps.log: the time (CLOCK_MONOTONIC ms), the thread, the key, the value MPC sent, whether it acted (or why
 * not: "zero" a 0 a selector ignores, "latched" a 1 while held), the cell's "_on" before and after and its label after (LOCK or
 * UNLOCK). UI thread (set_param). */
static void tap_log(inst_t *s, const char *key, skey_t k, const char *val, const char *what, int on_was) {
    static const char *const LOGGED[] = {"rac_", "rfl_", "rcx_", "cpr_", "cvo_", "cst_", "usy_", "yac_", "yfl_", "ycx_", "ysc_",
                                         "cqe_", "uqe_", "cqg_", "uqg_", "cqi_", "uqi_", "cqx_", "uqx_"};   /* 0.24 release fix: PHRASE's */
    struct timespec ts;
    FILE *f;
    char lab[64];
    size_t i = 0;
    if (k.kind != K_TAP || k.on || k.b < 0) return;
    while (i < sizeof LOGGED / sizeof LOGGED[0] && strncmp(key, LOGGED[i], 4)) i++;
    if (i == sizeof LOGGED / sizeof LOGGED[0] || !(f = fopen("/tmp/chordseq-taps.log", "a"))) return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    TAPS[k.a].label(&s->seq, k.b, lab, sizeof lab);
    fprintf(f, "%lld.%03ld tid=%lx %s val=%s %s on=%d>%d label=%s\n", (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000,
            ts.tv_nsec / 1000 % 1000, (unsigned long)pthread_self(), key, val, what, on_was, TAPS[k.a].on(&s->seq, k.b) || latched(s, k), lab);
    fclose(f);
}
#endif

static void clear_msgs(inst_t *s) {   /* the pages' message lines, after INIT or a load */
    s->rmsg[DL_CHORDS][0] = s->rmsg[DL_BASS][0] = s->umsg[0] = s->mmsg[0] = 0;
    s->djmsg[DL_CHORDS][0] = s->djmsg[DL_BASS][0] = 0;   /* 0.22 */
}

/* What a project's state load (and 0.23 a preset's) resets beside the state: the message lines, the knob load guard */
static void after_load(inst_t *s) {
    clear_msgs(s);
    s->kguard = (1ull << (sizeof KOPTIONS / sizeof KOPTIONS[0])) - 1;
    s->kguard_blocks = 0;
}

/* 0.23 genre preset i (presets.c, baked: one load, as a project's) as the song; the MIDI channels (the tracks' routing) and FOLLOW
 * (a GLOBAL setting, as INIT keeps it) stay. 1 when it loaded */
static int load_preset(inst_t *s, int i) {
    if (!seq_song_load(&s->seq, preset_chunk(i))) return 0;
    after_load(s);
    return 1;
}

/* A handled tap or trigger bumps display_rev: the wrapper then reports the option values it changed (style_1 from rh_ and
 * crh_, voicing from vo_, prog becoming Custom, cmode); 0.12: so does a change of cslots, after any set; 0.17: and of cview, cdrift
 * or a tint (a knob turn recolours the strip), and a knob 2-5 change while the VOICING browser shows them (btl) */
static void set_param(void *p, const char *key, const char *val) {
    inst_t *s = p;
    skey_t k = parse_key(key);
    int init = !strcmp(key, "init"), was = 0, now, acts = 0;   /* 0.24.1 acts: a setter ran */
    int vknob = seq_cview(&s->seq) == CVIEW_VOICE && (!strncmp(key, "k_v", 3) || !strncmp(key, "k_s", 3));   /* 0.18: the slots too */
#ifdef TAP_LOG
    int on_was = k.kind == K_TAP && !k.on && k.b >= 0 ? TAPS[k.a].on(&s->seq, k.b) || latched(s, k) : 0;
#endif
    int held, mstyle = seq_style(&s->seq, LANE_MEL_UP);
    /* 0.19: what the BASS page retires (KF_RETIRED_BASS: vst/make_skin.py RETIRED_19 ..) does nothing on a DRAWN Bass lane (on a lane a
     * test set up the 0.18 way it still acts); 0.20 what the MELODY pages retire (KF_RETIRED_MEL) on a JAM Melody lane (an old project's
     * lane still takes them, as 0.19 did) */
    if (seq_lane_drawn(&s->seq, LANE_BASS) && k.flags & KF_RETIRED_BASS) return;
    if (mstyle == GEN_MEL_JAM && k.flags & KF_RETIRED_MEL) return;
    if (k.flags & KF_RETIRED_24) return;   /* 0.24 what PHRASE and the sound pages retire (RETIRED_24_TAPS): a set does nothing */
    held = tap_latch(s, k, atoi(val) > 0);   /* 0.18: an image button's 1 while latched: ignored */
    if (!init) s->program = 0;   /* any edit (or a load) leaves INIT or a preset: picking it again loads again */
    if (!txn_key(&s->seq, k) && !held && set_acts(s, key, k, val)) close_txn(&s->seq), acts = 1;
    if (vknob) option_value(&s->seq, key, &was);
    if (k.kind == K_TAP && !k.on) {
        int v = atoi(val) > 0;
        if ((v || !TAPS[k.a].select) && !held) {
            TAPS[k.a].tap(&s->seq, k.b, v);
            s->seq.display_rev++;
            acts = 1;
        }
#ifdef TAP_LOG
        tap_log(s, key, k, val, held ? "latched" : v || !TAPS[k.a].select ? "acted" : "zero", on_was);
#endif
    } else if (k.kind == K_CELL && !k.on) seq_set_step(&s->seq, k.a, seq_fix_bar(&s->seq), k.b, atoi(val) > 0), acts = 1;   /* the bar on screen (redraw lag) */
    else if (k.kind == K_MUTE && !k.on) seq_set_mute(&s->seq, k.a, atoi(val) > 0), acts = 1;
    else if (!strcmp(key, "panic") && atoi(val) > 0) seq_all_notes_off(&s->seq), acts = 1;
    else if (init) {
        int v = atoi(val);
        acts = 1;
        if (v == 1) {   /* MPC's PRESET menu: Init (Current, 0, changes nothing) */
            seq_init_preset(&s->seq);
            s->program = 1;
            clear_msgs(s);
        } else if (v >= 2 && v < 2 + PRESETS_COUNT && load_preset(s, v - 2)) s->program = v;   /* 0.23 a genre preset */
    }
    else if (key[0] == 'k' && key[1] == '_' && kguard_skips(s, key, atoi(val))) {}
    else if (set_option(&s->seq, key, atoi(val))) acts = 1;   /* an echo too: its setter ran */
    else if (trigger_index(key) >= 0) {
        if (atoi(val) > 0) { fire_trigger(&s->seq, trigger_index(key)); s->seq.display_rev++; acts = 1; }
    } else if (!strcmp(key, "state")) {
        seq_state_load(&s->seq, val);   /* a load bumps display_rev itself */
        after_load(s);
        acts = 1;
    }
    fix_labels(s);   /* after any set, on the UI thread: a key, scale or NAMES change makes fc_, fb_ and bnk_ again */
    part_labels(s);
    bass_shown_now(s);
    utp_labels(s);
    /* 0.24.1 the forced ones only when a setter ran or a block switched: else (a latched 1, a selector's 0, a trigger's 0, a load-guard skip,
     * an unknown key) the state they read is as the last set or block left it (this set wrote only program, a latch and the load guard,
     * which none reads), so they are made as a block makes them (force 0) */
    if (seq_mel_poll(&s->seq)) s->seq.display_rev++, acts = 1;   /* 0.20: a JAM block switched since the last block (cx_3) */
    mel_shown_now(s, acts);   /* 0.20: the MELODY KEYS' bar and the dots (a change bumps display_rev) */
    acts |= seq_jam_poll(&s->seq) != 0;   /* 0.22: a CHORDS or BASS block switched since the last block: made again just below */
    jam_cells_now(s, acts);     /* 0.22: the JAM meters, NOW PLAYING and NEXT (a change bumps display_rev) */
    sound_cells_now(s, acts);   /* 0.24: the sound pages' status row and centre (a change bumps display_rev) */
    ph_cells_now(s, acts);      /* 0.24 Task 7: the PHRASE pages' cells and dots (a change bumps display_rev) */
    if (seq_style(&s->seq, LANE_MEL_UP) != mstyle) s->seq.display_rev++;   /* 0.20: the lane turned JAM (or back): cx_3 and the texts change */
    if (screen_sig(&s->seq) != s->screen_sig) {
        s->screen_sig = screen_sig(&s->seq);
        s->seq.display_rev++;
    }
    if (vknob && option_value(&s->seq, key, &now) && now != was) s->seq.display_rev++;   /* btl (unpolled) shows knobs 2-5 */
}

/* "NEXT BAR: CHORUS" while a part is queued; else, if the last save left parts out, "PARTS NOT SAVED: PRE, OUTRO" (bit 0 is a
 * failed save as a whole: not named here); else the bar and the chord on its first step, "BAR 3 \xC2\xB7 Em" (0.22.1: it changes once a
 * bar, not every beat: each change is an UpdateDisplay, which restarts MPC's PRESET drawer) */
static int pos_text(const seq_t *q, char *buf, int len) {
    int queued = seq_queued_part(q), lost = seq_unsaved_parts(q) & ~1, n = 0;
    if (queued >= 0) {
        n = snprintf(buf, len, "NEXT BAR: ");
        return n < len ? n + put_upper(buf + n, len - n, SEQ_PART_NAME[queued]) : len - 1;
    }
    if (lost) {
        n = snprintf(buf, len, "PARTS NOT SAVED: ");
        for (int p = 1; p < SEQ_NPARTS && n < len; p++)
            if (lost & 1 << p) {
                if (lost & ((1 << p) - 1) & ~1) n += snprintf(buf + n, (size_t)(len - n), ", ");
                if (n < len) n += put_upper(buf + n, len - n, SEQ_PART_NAME[p]);
            }
        return n < len ? n : len - 1;
    }
    int step = seq_cur_step(q), bar = seq_cur_bar(q);
    if (step < 0) return put(buf, len, "STOPPED · play a clip on this track");
    char chord[16];
    txt_t t = {buf, len, 0};
    seq_slot_name(q, seq_slot_at(q, bar, 0), chord, sizeof chord);
    add(&t, "BAR ");
    add_int(&t, bar + 1);
    add(&t, " \xC2\xB7 ");
    add(&t, chord);
    return t.n;
}

static int title_text(const seq_t *q, char *buf, int len) {
    int i = seq_prog_preset(q), n = snprintf(buf, len, "%s ", theory_key_name(seq_key(q)));
    if (n >= len) return n;
    for (const char *c = theory_scale_name(seq_scale(q)); *c && n < len - 1; c++) {
        char ch = *c;
        buf[n++] = (char)(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    }
    return n + snprintf(buf + n, (size_t)(len - n), " \xC2\xB7 %s", i < 0 ? "Custom" : THEORY_PRESETS[i].name);
}

/* PLAY's title: the key line, or 0.23 while a genre preset is the program what it was made for, "HOUSE \xC2\xB7 124 BPM \xC2\xB7
 * PIANO, SUB, PLUCK" */
static int play_title(const inst_t *s, char *buf, int len) {
    const preset_info_t *in = preset_info(s->program - 2);
    return in ? snprintf(buf, len, "%s \xC2\xB7 %d BPM \xC2\xB7 %s", jam_genre_name(s->program - 2), in->bpm, in->sounds)
              : title_text(&s->seq, buf, len);
}

static int bpm_text(const seq_t *q, char *buf, int len) {
    return q->playing && q->bpm > 0 ? snprintf(buf, len, "%.1f BPM", q->bpm) : put(buf, len, "\xE2\x80\x94 BPM");
}

/* "Em7  i", "A  IV": the chord (coloured) at the start of the bar strip's lit bar and its numeral (theory_numeral: case, o/+,
 * b/#) */
static int vchord_text(const seq_t *q, char *buf, int len) {
    char chord[16], num[8];
    int slot = seq_slot_at(q, seq_strip_bar(q), 0);
    txt_t t = {buf, len, 0};
    seq_slot_label(q, slot, chord, sizeof chord);
    seq_slot_numeral(q, slot, num, sizeof num);
    add(&t, chord);
    add(&t, "  ");
    add(&t, num);
    return t.n;
}

/* "4 chords \xC2\xB7 1 bar each (chord length is set on GLOBAL)" */
static int lnote_text(const seq_t *q, char *buf, int len) {
    static const char *const EACH[4] = {"2 bars", "1 bar", "1/2 bar", "1/4 bar"};
    int n = seq_prog_len(q);
    txt_t t = {buf, len, 0};
    add_int(&t, n);
    add(&t, n == 1 ? " chord \xC2\xB7 " : " chords \xC2\xB7 ");
    add(&t, EACH[get_chord_len(q)]);
    add(&t, " each (chord length is set on GLOBAL)");
    return t.n;
}

/* "BAR 1 \xC2\xB7 Em": the grid's bar (seq_grid_bar) and the chord it starts on (FIX A NOTE's, retired: RETIRED_19) */
static int bvw_text(const seq_t *q, char *buf, int len) {
    char chord[16];
    int bar = seq_grid_bar(q);
    txt_t t = {buf, len, 0};
    seq_slot_name(q, seq_slot_at(q, bar, 0), chord, sizeof chord);
    add(&t, "BAR ");
    add_int(&t, bar + 1);
    add(&t, " \xC2\xB7 ");
    add(&t, chord);
    return t.n;
}

/* 0.17 knobs 2-5's texts (at most 14 characters): COLOUR's level, INVERSION, SPREAD; STRUM's spread in ms at the host tempo (120
 * BPM stopped): |STR| x 12.5 % of 0.0625 beat */
static const char *const VCOL_NAMES[6] = {"Power", "Triad", "7th", "9th", "11th", "13th"};
static const char *const VINV_NAMES[6] = {"-2", "-1", "Root", "+1", "+2", "+3"};
static const char *const VSPR_NAMES[5] = {"Cluster", "Close", "Open", "Split", "Wide"};
static int strum_ms(const seq_t *q, int v) {   /* 0.18: STRUM step v's spread in ms */
    double bpm = q->playing && q->bpm > 0 ? q->bpm : 120.0;
    return clampi((int)lround(abs(v) * 12.5 / 100 * 0.0625 * 60000 / bpm), 0, 9999);
}
static int strum_text(const seq_t *q, int v, char *buf, int len) {   /* STRUM step v; 0.18: the step is the caller's */
    txt_t t = {buf, len, 0};
    if (!v) return put(buf, len, "Block");
    add(&t, v > 0 ? "Up " : "Down ");
    add_int(&t, strum_ms(q, v));
    add(&t, " ms");
    return t.n;
}

/* 0.17 the CHORDS page's readouts. cpage "BARS 5-8 \xC2\xB7 2/4" ("BAR 5" for one bar), the strip page's bars (0.18: on the RHYTHM views
 * 4 bars a page); 0.19.1 upage the same on the BASS tab (always 4 bars a page), which the strip's page box holds at 22 px */
static int tb_page_text(const seq_t *q, int tab, char *buf, int len) {
    int page = seq_strip_page(q, tab), cl = seq_chord_len(q), first = page * 4, last = first + 3 < seq_slots(q) ? first + 3 : seq_slots(q) - 1;
    int from = first * cl / SEQ_STEPS + 1, to = clampi(((last + 1) * cl - 1) / SEQ_STEPS + 1, from, seq_bars(q));
    if (seq_strip_by_bars(q, tab)) from = first + 1, to = clampi(first + 4, from, seq_bars(q));
    txt_t t = {buf, len, 0};
    add(&t, from == to ? "BAR " : "BARS ");
    add_int(&t, from);
    if (from != to) add(&t, "-"), add_int(&t, to);
    add(&t, " \xC2\xB7 ");
    add_int(&t, page + 1);
    add(&t, "/");
    add_int(&t, seq_strip_pages(q, tab));
    return t.n;
}
/* ccount "> BAR 3 / 16" while playing ("BAR 1 / 16" stopped; ucnt the same); cfol FOLLOW ON, PAUSED (a picker, a browser or the arrows),
 * OFF; ufol the same on the BASS tab (paused by the editor, the STYLE browser or the arrows) */
static int ccount_text(const seq_t *q, char *buf, int len) {
    txt_t t = {buf, len, 0};
    add(&t, seq_cur_step(q) >= 0 ? "> BAR " : "BAR ");
    add_int(&t, seq_cur_bar(q) + 1);
    add(&t, " / ");
    add_int(&t, seq_bars(q));
    return t.n;
}
static int tb_fol_text(const seq_t *q, int tab, char *buf, int len) {
    return put(buf, len, !seq_follow(q) ? "FOLLOW OFF" : seq_follow_paused(q, tab) ? "FOLLOW PAUSED" : "FOLLOW ON");
}
/* ccd "III \xC2\xB7 G B D F#" (page_labels), cdr "tense \xC2\xB7 46": the centre chord's */
static int cdr_text(const seq_t *q, char *buf, int len) {
    int d = inst_of(q)->drift[seq_part(q)][seq_centre_slot(q, TAB_CHORDS)];
    txt_t t = {buf, len, 0};
    add(&t, DRIFT_WORD[drift5(d)]);   /* 0.24 ruling 23: the one scale */
    add(&t, " \xC2\xB7 ");
    add_int(&t, d);
    return t.n;
}
/* ptl, the picked chord's title ("BAR 6 \xC2\xB7 Gmaj7"), retired with the picker (0.24.1): " " */
static int ptl_text(const seq_t *q, char *buf, int len) { (void)q; return put(buf, len, " "); }
/* btl, the open browser's title and what it shows: "PROGRESSIONS \xC2\xB7 Epic \xC2\xB7 i VI III VII" (the numerals), "VOICING \xC2\xB7
 * Lo-fi Keys \xC2\xB7 Smooth, 7th, Open, Up 21 ms"; " " with none. Unpolled: it changes with a tap, or a knob 2-5 turn (set_param bumps
 * display_rev) */
static void btl_prog(const seq_t *q, txt_t *t) {
    int k = seq_prog_preset_shown(q);
    char num[12];
    add(t, "PROGRESSIONS \xC2\xB7 ");
    add(t, k < 0 ? "Custom" : preset_name(k));
    for (int i = 0; k >= 0 && i < THEORY_PRESETS[k].len; i++) {
        theory_numeral(seq_key(q), seq_scale(q), seq_scale(q), THEORY_PRESETS[k].deg[i], num, sizeof num);
        add(t, i ? " " : " \xC2\xB7 ");
        add(t, num);
    }
}
static void btl_voice(const seq_t *q, txt_t *t) {
    static const char *const MODE[VMODE_COUNT] = {"Close", "Smooth", "Top line", "Rootless"};
    int k = seq_voicing_style(q);
    char strum[24];
    add(t, "VOICING \xC2\xB7 ");
    add(t, k < 0 ? "Custom" : VOICING_STYLES[k].name);
    add(t, " \xC2\xB7 ");
    add(t, MODE[q->part[seq_part(q)].vmode]);
    add(t, ", ");
    add(t, VCOL_NAMES[seq_vknob_eff(q, VK_COL) + 1]);   /* 0.18: as they play (COMPLEXITY's layers), as the knob readouts */
    add(t, ", ");
    add(t, VSPR_NAMES[seq_vknob_eff(q, VK_SPR) + 1]);
    add(t, ", ");
    strum_text(q, seq_vknob_eff(q, VK_STR), strum, sizeof strum);
    add(t, strum);
}
/* 0.18 "RHYTHMS \xC2\xB7 Funk \xC2\xB7 bar 6" (" \xC2\xB7 every unlocked bar"); 0.19 either RHYTHM page's (btl, ubtl) */
static void btl_rhythm(const seq_t *q, const rpage_t *P, txt_t *t) {
    int bar = seq_rbar_bar(q, P->lane), ri = seq_bar_rhythm(q, P->lane, rp_shown(q, P), NULL);
    add(t, "RHYTHMS \xC2\xB7 "), add(t, ri >= 0 ? rhythm_lib_name(P->lib, ri) : "Custom");
    if (bar >= 0) add(t, " \xC2\xB7 bar "), add_int(t, bar + 1);
    else add(t, " \xC2\xB7 every unlocked bar");
}
static int btl_text(const seq_t *q, char *buf, int len) {
    txt_t t = {buf, len, 0};
    if (seq_cview(q) == CVIEW_PROG) btl_prog(q, &t);
    else if (seq_cview(q) == CVIEW_VOICE) btl_voice(q, &t);
    else if (seq_cview(q) == CVIEW_RBROWSE) btl_rhythm(q, RP_C, &t);
    else add(&t, " ");
    return t.n;
}

/* 0.18 knobs 2-5's texts: the step's name, or "user > effective" while COMPLEXITY moves the knob (DRAWN lanes), in the first of
 * these forms that fits KNOB_CHARS: long > long, long > short, short > long, short > short, tiny > short. tiny: STRUM's user value
 * without " ms"; the others' is their short name */
#define KNOB_CHARS 14
typedef struct { char l[24], s[24], t[24]; } knob_names_t;
static int arrow_text(const knob_names_t *u, const knob_names_t *e, char *buf, int len) {
    const char *const F[5][2] = {{u->l, e->l}, {u->l, e->s}, {u->s, e->l}, {u->s, e->s}, {u->t, e->s}};
    for (int f = 0; f < 5; f++) {
        txt_t t = {buf, len, 0};
        add(&t, F[f][0]), add(&t, " > "), add(&t, F[f][1]);
        if (theory_text_width(buf) <= KNOB_CHARS || f == 4) return t.n;
    }
    return 0;
}
static void names_of(knob_names_t *k, const char *l, const char *s) {
    put(k->l, sizeof k->l, l);
    put(k->s, sizeof k->s, s);
    put(k->t, sizeof k->t, s);
}
static void strum_names(const seq_t *q, int v, knob_names_t *k) {   /* "Up 21 ms", "+21 ms", "+21"; "Block" */
    int ms = strum_ms(q, v);
    txt_t l = {k->l, sizeof k->l, 0}, s = {k->s, sizeof k->s, 0}, t = {k->t, sizeof k->t, 0};
    if (!v) {
        names_of(k, "Block", "Block");
        return;
    }
    add(&l, v > 0 ? "Up " : "Down "), add_int(&l, ms), add(&l, " ms");
    add(&s, v > 0 ? "+" : "-"), add_int(&s, ms), add(&s, " ms");
    add(&t, v > 0 ? "+" : "-"), add_int(&t, ms);
}
static const char *const VSPR_SHORT[5] = {"Clus", "Close", "Open", "Split", "Wide"};
static void vk_names(const seq_t *q, int vk, int v, knob_names_t *k) {
    if (vk == VK_COL) names_of(k, VCOL_NAMES[v + 1], VCOL_NAMES[v + 1]);
    else if (vk == VK_INV) names_of(k, VINV_NAMES[v + 2], VINV_NAMES[v + 2]);
    else if (vk == VK_SPR) names_of(k, VSPR_NAMES[v + 1], VSPR_SHORT[v + 1]);
    else strum_names(q, v, k);
}
static int vk_text(const seq_t *q, int vk, char *buf, int len) {
    knob_names_t u, e;
    int uv = seq_vknob(q, vk), ev = seq_vknob_eff(q, vk);
    vk_names(q, vk, uv, &u);
    if (ev == uv) return put(buf, len, u.l);
    vk_names(q, vk, ev, &e);
    return arrow_text(&u, &e, buf, len);
}
/* 0.24 PHRASE's CADENCE and BREATH (the MELODY PHRASE knobs' texts, melody2.c's: OFF .. HOME, None .. Half) and the Bass lane's FILL
 * (NONE, PICKUP, STEP-IN, RUN, BREAK: the spec's knob notes, in the lane's lower case; the Chords' FILL is 0.18's) */
static const char *const RK_LONG[RK_COUNT][6] = {{"all on beat", "straighter", "as drawn", "push 1", "push 1+3", "push all"},
                                                 {"none", "few", "some", "many", "every gap"},
                                                 {"tight", "late 4 ms", "late 8 ms", "late 12 ms", "late 16 ms"},
                                                 {"none", "pickup", "busier", "roll", "break"},
                                                 {"OFF", "OPEN", "SOFT", "HOME"}, {"None", "1/8", "Beat", "Half"}};
static const char *const RK_SHORT[RK_COUNT][6] = {{"-2", "-1", "0", "+1", "+2", "+3"}, {"none", "few", "some", "many", "all"},
                                                  {"tight", "4 ms", "8 ms", "12 ms", "16 ms"}, {"none", "pickup", "busier", "roll", "break"},
                                                  {"OFF", "OPEN", "SOFT", "HOME"}, {"None", "1/8", "Beat", "Half"}};
static const char *const RK_BFILL[5] = {"none", "pickup", "step-in", "run", "break"};
static const char *rk_name(int lane, int rk, int v, int brief) {
    return rk == RK_FILL && lane == LANE_BASS ? RK_BFILL[v] : brief ? RK_SHORT[rk][v] : RK_LONG[rk][v];
}
static int rk_text(const seq_t *q, int lane, int rk, char *buf, int len) {   /* 0.19: either drawn lane's */
    knob_names_t u, e;
    int lo = rk == RK_SYNC ? -2 : 0, uv = seq_rknob(q, lane, rk) - lo, ev = seq_rknob_eff(q, lane, rk) - lo;
    if (ev == uv) return put(buf, len, rk_name(lane, rk, uv, 0));
    names_of(&u, rk_name(lane, rk, uv, 0), rk_name(lane, rk, uv, 1));
    names_of(&e, rk_name(lane, rk, ev, 0), rk_name(lane, rk, ev, 1));
    return arrow_text(&u, &e, buf, len);
}
static int cx_hits_text(const seq_t *q, int lane, int bar, int notes, char *buf, int len) {   /* "6 hits a bar", "5 notes": bar's */
    txt_t t = {buf, len, 0};
    int n = seq_bar_hits(q, lane, bar);
    add_int(&t, n);
    if (notes) add(&t, n == 1 ? " note" : " notes");   /* PLAY's knob box holds 13 */
    else add(&t, n == 1 ? " hit a bar" : " hits a bar");
    return t.n;
}
/* cx_1 on a drawn Chords lane: "6 hits a bar", the shown bar's (off the RHYTHM views, following, the playing bar's: the text is
 * read with the screen, never polled; COMPLEXITY's own layers are not shown) */
static int cx1_text(const inst_t *s, char *buf, int len) {
    const seq_t *q = &s->seq;
    if (!seq_lane_drawn(q, LANE_CHORDS)) return put(buf, len, s->ktext[seq_part(q)][KT_CX1]);
    return cx_hits_text(q, LANE_CHORDS, seq_rbar_bar(q, LANE_CHORDS) < 0 && rp_live(q, RP_C) ? seq_cur_bar(q) : rp_shown(q, RP_C), 0, buf, len);
}

/* 0.19 k_b2..5's texts: BASS the step's name (MOVEMENT and APPROACH "held by PEDAL" while a pedal is on; the spec's long texts go to
 * umsg as a knob turns: bknob_msg), RHYTHM the Bass lane's rk_text; their names */
static const char *const BMOV_NAMES[5] = {"None", "A little", "Some", "Lots", "All"};
static const char *const BMOV_TEXT[5] = {"roots only", "+ a 5th", "root and 5th", "+ 3rds", "+ passing notes"};
static const char *const BOCT_TEXT[5] = {"none", "1 in 4 up", "every other up", "3 in 4 up", "all but the 1st"};
static const char *const BAPP_NAMES[5] = {"None", "Scale step", "Half below", "Half above", "Surround"};
static const char *const BREG_NAMES[5] = {"Lowest", "Low", "Natural", "High", "Highest"};
static const char *const BREG_RANGE[5] = {"C1-B1", "D1-C#2", "E1-D#2", "G1-F#2", "A1-G#2"};   /* bassline.c's fold points C D E G A */
static int bk_pedal_held(const seq_t *q, int k) { return ppart(q)->bass.set.pedal && (k == BK_MOV || k == BK_APP); }
static const char *bk_name(const seq_t *q, int k) {   /* the knob's step ("Some", "Half below", "Natural"), at most 14 characters */
    int v = seq_bknob(q, k);
    return k == BK_APP ? BAPP_NAMES[v] : k == BK_REG ? BREG_NAMES[v + 2] : BMOV_NAMES[v];
}
static int bk_text(const seq_t *q, int k, char *buf, int len) { return put(buf, len, bk_pedal_held(q, k) ? "held by PEDAL" : bk_name(q, k)); }
/* The spec's long knob texts on the message line when a BASS knob turns: "MOVEMENT: Some \xC2\xB7 root and 5th", "OCTAVES: Some \xC2\xB7
 * every other up", "APPROACH: Half below to the change", "REGISTER: Natural \xC2\xB7 E1-D#2" (+ " (heard when PEDAL is off)") */
static void bknob_msg(seq_t *q, int k) {
    static const char *const NAME[BK_COUNT] = {"MOVEMENT: ", "OCTAVES: ", "APPROACH: ", "REGISTER: "};
    int v = seq_bknob(q, k);
    txt_t m = umsg_new(q);
    add(&m, NAME[k]), add(&m, bk_name(q, k));
    if (k == BK_MOV || k == BK_OCTS) add(&m, " \xC2\xB7 "), add(&m, k == BK_MOV ? BMOV_TEXT[v] : BOCT_TEXT[v]);
    else if (k == BK_APP) add(&m, v ? " to the change" : "");
    else add(&m, " \xC2\xB7 "), add(&m, BREG_RANGE[v + 2]);
    if (bk_pedal_held(q, k)) add(&m, " (heard when PEDAL is off)");
}
/* 0.20 k_j2..5's texts: the step's name ("1+1", "None", "HOME", "LAST BEAT", "+1", "Middle"); a turn puts the spec's long text on jmsg.
 * 0.24 JAM's REGISTER is OCTAVE: its 3 values read -1, 0, +1 (OCTAVE_NAMES, as PHRASE's answer OCTAVE); the turns say OCTAVE and ANSWER OCT.
 * 0.21 DENSITY, RANGE, VARIATION ("THIN", "WIDE", "BARS 2+4"), SWING with the playing loop's grid ("8TH 58%"), GATE as mgat ("Gate 40%"),
 * TIME ("DOUBLE"), PUSH as jpush ("Ahead 1/64") */
static int mj_text(const seq_t *q, int f, char *buf, int len) {
    int v = mj_get(q, f);
    txt_t t = {buf, len, 0};
    switch (f) {
    case MJS_FORM: return put(buf, len, mjam_form_name(v));
    case MJS_BREATH: return put(buf, len, mjam_breath_name(v));
    case MJS_CAD: return put(buf, len, mjam_cad_name(v));
    case MJS_FILL: return put(buf, len, mjam_fill_name(v));
    case MJS_ANS: return put(buf, len, mjam_answer_name(v));
    case MJS_STRAY: return put(buf, len, mjam_stray_name(v));
    case MJS_AOCT: return put(buf, len, OCTAVE_NAMES[clampi(v + 1, 0, 2)]);
    case MJS_DENS: return put(buf, len, mjam_dens_name(v));
    case MJS_RNG: return put(buf, len, mjam_rng_name(v));
    case MJS_VAR: return put(buf, len, mjam_var_name(v));
    case MJS_TIME: return put(buf, len, mjam_time_name(v));
    case MJE_SWING: add(&t, seq_mel_grid16(q) ? "16TH " : "8TH "), add_int(&t, 50 + 4 * v), add(&t, "%"); return t.n;
    case MJE_GATE: knob_format(KT_MGAT, seq_mel_knob(q, MK_GATE), buf, len); return (int)strlen(buf);
    case MJE_PUSH: knob_format(KT_JPUSH, seq_mel_push(q), buf, len); return (int)strlen(buf);
    default: return put(buf, len, OCTAVE_NAMES[clampi(v, 0, 2)]);   /* 0.24 MJS_REG, renamed OCTAVE */
    }
}
/* 0.21 the MW_* bit of a knob that can read weak (DENSITY, RANGE, VARIATION, SWING, TIME), else 0; mj_weak: set for the playing part
 * on a JAM lane (seq_mel_weak is kept on any lane) and the knob off its neutral step */
static unsigned mj_weak_bit(int f) {
    return f == MJS_DENS ? MW_DENS : f == MJS_RNG ? MW_RNG : f == MJS_VAR ? MW_VAR : f == MJE_SWING ? MW_SWING : f == MJS_TIME ? MW_TIME : 0;
}
static int mj_weak(const seq_t *q, int f) { return mel_jam_on(q) && (seq_mel_weak(q) & mj_weak_bit(f)) && mj_get(q, f); }
/* 0.21 final review: a weak DENSITY, RANGE, VARIATION or (Amendment 1) TIME with every bar of the loop locked (each plays its frozen
 * settings) says so */
static int mj_all_locked(const seq_t *q, int f) {
    unsigned all = (1u << ppart(q)->bars) - 1;
    return (f == MJS_DENS || f == MJS_RNG || f == MJS_VAR || f == MJS_TIME) && (ppart(q)->mjam.nlock & all) == all;
}
/* KSLOTS' MELODY texts: mj_text, but with FORM LOOP ANSWER, STRAY and OCTAVE read "no answer" (they still turn; MPC draws it upper
 * case, "NO ANSWER": the knob's own name says which). 0.21 a weak knob "THIN \xC2\xB7 weak" (SWING without its grid: "58% \xC2\xB7 weak"),
 * "weak here" past the 13 characters of a JAM value box; every bar locked (mj_all_locked) "THIN \xC2\xB7 locked", "all locked" past them */
#define MJ_VALUE_CHARS 13
static int mj_slot_text(const seq_t *q, int f, char *buf, int len) {
    char v[32];
    txt_t t = {buf, len, 0};
    if ((f == MJS_ANS || f == MJS_STRAY || f == MJS_AOCT) && seq_mjam_get(q, MJS_FORM) == MF_LOOP) return put(buf, len, "no answer");
    if (!mj_weak(q, f)) return mj_text(q, f, buf, len);
    mj_text(q, f, v, sizeof v);
    add(&t, f == MJE_SWING ? strchr(v, ' ') + 1 : v), add(&t, mj_all_locked(q, f) ? " \xC2\xB7 locked" : " \xC2\xB7 weak");
    return theory_text_width(buf) <= MJ_VALUE_CHARS ? t.n : put(buf, len, mj_all_locked(q, f) ? "all locked" : "weak here");
}
/* "FORM 1+1 \xC2\xB7 a 1-bar call, a 1-bar answer, twice", "CADENCE HOME \xC2\xB7 call on 5, answer home on 1", "STRAY \xC2\xB7 changes the last
 * beat", "REGISTER \xC2\xB7 Middle \xC2\xB7 the whole line" (the boards' texts; tests/ref/melody_ref.js FORMS, ANS, CAD, FILL). 0.21 "DENSITY
 * THIN \xC2\xB7 drops the weakest off-beat notes in every bar", "SWING 8TH 58% \xC2\xB7 the off-beat notes play late", "GATE 60% \xC2\xB7 notes
 * shorter than drawn", "PUSH Ahead 1/64 \xC2\xB7 the note on each chord change plays early", "TIME HALF \xC2\xB7 each bar's first half at half
 * speed"; a weak knob (mj_weak) says why instead, about the
 * playing melody (a locked bar keeps its own): "DENSITY THIN: the melody has almost nothing to thin", "SWING 58%: ..."; every bar locked
 * (mj_all_locked): "DENSITY: every bar is locked" */
static void mknob_msg(seq_t *q, int f) {
    static const char *const NAME[MJE_COUNT] = {"FORM", "BREATH", "CADENCE", "FILL", "ANSWER", "STRAY", "ANSWER OCT", "OCTAVE", "DENSITY",
                                                "RANGE", "VARIATION", "TIME", "SWING", "GATE", "PUSH"};
    static const char *const TEXT[MJE_COUNT][MF_COUNT] = {
        {"every bar plays the block, nothing answers", "a 1-bar call, a 1-bar answer, twice", "bars 1-2 call, bars 3-4 answer them",
         "three bars call, bar 4 turns it around", "say it, say it again, then answer", "idea, idea a step up, break it up, close"},
        {"the line runs straight through", "rests on the call's last 8th", "rests on the call's last beat", "rests for the call's second half"},
        {"ends where it ends", "call on 2, answer on 5", "call on 5, answer on 3", "call on 5, answer home on 1"},
        {"the gap stays empty", "1 note into the next bar", "3 notes walk in", "a beat of 16ths runs in"},
        {"the call again; only its ending changes", "the call's shape, a 3rd higher", "the call upside down",
         "the call's rhythm, new chord-tone notes", "the call's second half, twice, then home", "another JAM block answers"},
        {"changes the last note", "changes the last beat", "changes the last half", "changes all of it"},
        {"answers an octave down", "answers where the call is", "answers an octave up"},
        {"the whole line", "the whole line", "the whole line"},
        {"only the strongest beats stay", "drops the weakest off-beat notes in every bar", "every note as the block draws it",
         "a passing note in every gap", "passing notes in every gap, twice"},
        {"every leap from the bar's first note halved", "the leaps as the block draws them", "every leap half again as wide",
         "every leap from the bar's first note doubled"},
        {"every bar as the phrase plays it", "bar 4 flips and steps up", "bar 2 steps up, bar 4 flips and steps up",
         "bar 2 steps up, bar 3 flips, bar 4 both"},   /* flips: upside down (mirror) */
        {"each bar's first half at half speed", "every note at the block's own speed", "each bar's pattern twice, at double speed"},
        {"straight", "the off-beat notes play late"}, {"notes shorter than drawn", "notes as long as drawn", "notes held longer than drawn"},
        {"the notes on the beat", "the note on each chord change plays early"}};
    static const char *const WEAK[MJE_COUNT - MJS_DENS] = {   /* DENSITY (its thin side; FILLED and BUSY below), RANGE, VARIATION, TIME,
                                                               * SWING, -, - */
        "the melody has almost nothing to thin", "the melody has almost no leaps to change", "the varied bars barely change this melody",
        "the melody sounds almost the same at this speed", "almost no notes on the off-beats to swing", NULL, NULL};
    char v[32];
    int x = mj_get(q, f), i = clampi(x + (f == MJS_AOCT) + 2 * (f == MJS_DENS) + (f == MJS_RNG) + (f == MJS_TIME), 0, MF_COUNT - 1);
    const char *w = f >= MJS_DENS ? WEAK[f - MJS_DENS] : NULL;
    txt_t m = mmsg_new(q);
    if (f == MJE_SWING || f == MJE_PUSH) i = x > 0;
    if (f == MJE_GATE) i = 1 + (seq_mel_knob(q, MK_GATE) > 50) - (seq_mel_knob(q, MK_GATE) < 50);
    mj_text(q, f, v, sizeof v);
    add(&m, NAME[f]);
    if (mj_weak(q, f) && mj_all_locked(q, f)) { add(&m, ": every bar is locked"); return; }
    if (f == MJE_GATE) {   /* "GATE 60%": the value's own word is GATE's */
        add(&m, " "), add_int(&m, seq_gate_pct(seq_mel_knob(q, MK_GATE))), add(&m, "%");
    } else if (f != MJS_STRAY) {
        add(&m, f == MJS_REG ? " \xC2\xB7 " : " ");
        add(&m, mj_weak(q, f) && f == MJE_SWING ? strchr(v, ' ') + 1 : v);
    }
    if (mj_weak(q, f)) {
        if (f == MJS_DENS && x > 0) w = "the melody has almost no gaps to fill";
        add(&m, ": "), add(&m, w);
        return;
    }
    add(&m, " \xC2\xB7 "), add(&m, TEXT[f][i]);
}
/* 0.22 the CHORDS and BASS JAM knobs' texts (jk_text: at most MJ_VALUE_CHARS, a JAM value box) and their turns' message lines (jknob_msg,
 * on cjmsg / ujmsg). DENSITY SPARSE .. BUSY (COMPLEXITY in 5 steps), COLOUR and SPREAD as the CHORDS page's (vk_text), MOVEMENT and OCTAVES
 * as the BASS page's (bk_text), VARIATION OFF .. ALL BARS, SWING with the lane's grid ("8TH 58%"), TIME HALF, NORMAL, DOUBLE. VARIATION,
 * SWING and TIME off neutral on a lane whose block changes little there (seq_djam_weak, only with a block playing) read "BAR 4 \xC2\xB7 weak"
 * ("58% \xC2\xB7 weak"), "weak here" past the box; with every bar of the loop locked for JAM "BAR 4 \xC2\xB7 locked", "all locked" (0.21's
 * forms) */
static unsigned jk_weak_bit(int k) { return k == XK_VAR ? DW_VAR : k == XK_SWING ? DW_SWING : k == XK_TIME ? DW_TIME : 0u; }
static int jk_weak(const seq_t *q, int d, int k) {
    return seq_jam_playing(q, dl_lane(d), NULL, NULL) && (seq_djam_weak(q, dl_lane(d)) & jk_weak_bit(k)) && xk_get(q, dl_lane(d), k);
}
static int jk_all_locked(const seq_t *q, int d) {
    for (int b = 0; b < seq_bars(q); b++)
        if (!jam_locked_bar(q, d, b)) return 0;
    return 1;
}
static void jk_value(const seq_t *q, int d, int k, char *buf, int len) {   /* the step's name */
    int lane = dl_lane(d), v = k >= XK_PUSH ? xk_get(q, lane, k) : 0;
    txt_t t = {buf, len, 0};
    if (k == XK_DENS) put(buf, len, mjam_dens_name(v - 2));
    else if (k == XK_VAR) put(buf, len, mjam_var_name(v));
    else if (k == XK_TIME) put(buf, len, mjam_time_name(v));
    else if (k == XK_SWING) add(&t, seq_djam_grid16(q, lane) ? "16TH " : "8TH "), add_int(&t, 50 + 4 * v), add(&t, "%");
    else if (k == XK_GATE) xk_text(q, lane, k, buf, len);   /* 0.24 knob 7's slot: cgate's / bgate's text, as 0.22's JAM page showed */
    else if (d == DL_BASS) bk_text(q, k, buf, len);
    else vk_text(q, k, buf, len);
}
static int jk_text(const seq_t *q, int d, int k, char *buf, int len) {
    char v[32];
    txt_t t = {buf, len, 0};
    jk_value(q, d, k, v, sizeof v);
    if (!jk_weak(q, d, k)) return put(buf, len, v);
    add(&t, k == XK_SWING ? strchr(v, ' ') + 1 : v), add(&t, jk_all_locked(q, d) ? " \xC2\xB7 locked" : " \xC2\xB7 weak");
    return theory_text_width(buf) <= MJ_VALUE_CHARS ? t.n : put(buf, len, jk_all_locked(q, d) ? "all locked" : "weak here");
}
/* "DENSITY FILLED \xC2\xB7 6 hits a bar" (the playing bar's), "COLOUR 9th \xC2\xB7 every chord, every bar", "SPREAD Open \xC2\xB7 ...",
 * "MOVEMENT Some \xC2\xB7 root and 5th", "OCTAVES Some \xC2\xB7 every other up" (+ " (heard when PEDAL is off)"), "VARIATION BAR 4 \xC2\xB7 bar 4
 * of each phrase plays a fill", "SWING 8TH 58% \xC2\xB7 the off-beat hits play late", "TIME HALF \xC2\xB7 each bar's first half at half speed";
 * weak: "VARIATION BAR 4: the varied bars barely change this rhythm"; every bar locked: "VARIATION: every bar is locked" */
static void jknob_msg(seq_t *q, int d, int k) {
    static const char *const VAR_LINE[4] = {"every bar as the block plays it", "bar 4 of each phrase plays a fill",
                                            "bar 2 pushed, bar 4 a fill", "bars 2-4: pushed, an 8th late, a fill"};
    static const char *const TIME_LINE[3] = {"each bar's first half at half speed", "every hit at the block's own speed",
                                             "each bar's pattern twice, at double speed"};
    int lane = dl_lane(d), v = k >= XK_PUSH ? xk_get(q, lane, k) : d == DL_BASS ? seq_bknob(q, k) : seq_vknob(q, k), weak = jk_weak(q, d, k);
    const char *name = k == XK_DENS ? "DENSITY" : k == XK_VAR ? "VARIATION" : k == XK_SWING ? "SWING" : k == XK_TIME ? "TIME"
                     : d == DL_BASS ? (k == BK_MOV ? "MOVEMENT" : "OCTAVES") : k == VK_COL ? "COLOUR" : "SPREAD";
    char val[32];
    txt_t m = djmsg_new(q, d);
    jk_value(q, d, k, val, sizeof val);
    add(&m, name);
    if (weak && jk_all_locked(q, d)) {
        add(&m, ": every bar is locked");
        return;
    }
    add(&m, " "), add(&m, weak && k == XK_SWING ? strchr(val, ' ') + 1 : val);
    if (weak) {
        add(&m, k == XK_VAR ? ": the varied bars barely change this rhythm" : k == XK_SWING ? ": almost no off-beat hits to swing"
                                                                             : ": the rhythm sounds almost the same at this speed");
        return;
    }
    add(&m, " \xC2\xB7 ");
    if (k == XK_DENS) {
        char h[24];
        cx_hits_text(q, lane, seq_cur_bar(q), 0, h, sizeof h);
        add(&m, h);
    } else if (k == XK_VAR) add(&m, VAR_LINE[clampi(v, 0, 3)]);
    else if (k == XK_SWING) add(&m, v ? "the off-beat hits play late" : "straight");
    else if (k == XK_TIME) add(&m, TIME_LINE[clampi(v + 1, 0, 2)]);
    else if (d == DL_CHORDS) add(&m, "every chord, every bar");
    else {
        add(&m, k == BK_MOV ? BMOV_TEXT[v] : BOCT_TEXT[v]);
        if (bk_pedal_held(q, k)) add(&m, " (heard when PEDAL is off)");
    }
}
/* 0.22 knob 1's text (k_s1_display, k_b1_display): on the own page and RHYTHM cx_1's / cx_2's; on JAM the block its row reads (the one
 * queued, else playing: "TECHNO \xC2\xB7 DUB", the name alone past PLAY's 13), "Tap a block" before any; its names by mode */
static int cx1_text(const inst_t *s, char *buf, int len);
static int cx2_text(const inst_t *s, char *buf, int len);
static int k1_text(const inst_t *s, int d, char *buf, int len) {
    const seq_t *q = &s->seq;
    int c, l;
    txt_t t = {buf, len, 0};
    if (k1_mode(q, d) != 2) return d == DL_BASS ? cx2_text(s, buf, len) : cx1_text(s, buf, len);
    if (!jam_shown(q, dl_lane(d), &c, &l)) return put(buf, len, "Tap a block");
    add_jblock(&t, d, c, l);
    return theory_text_width(buf) <= 13 ? t.n : put(buf, len, jam_block_name(djl(d), c, l));
}
static const char *k1_name(const seq_t *q, int d) {
    return k1_mode(q, d) == 2 ? (d == DL_BASS ? "Bass Row" : "Chords Row") : d == DL_BASS ? "Bass Complexity" : "Chords Complexity";
}
static int slot_text(const seq_t *q, const kslots_t *T, int j, char *buf, int len) {   /* k_s2..5, k_b2..5, k_j2..8 (0.22 k_s6, k_s8, k_b6,
                                                                                       * k_b8): the mode's knob */
    int m = T->mode(q), k = T->slot[j].knob[m];
    return slot_rk(T, m, k) ? rk_text(q, T->lane, k, buf, len) : T->text(q, k, buf, len);
}
/* The BASS tab's shown bar: on BASS the centre's; on RHYTHM BASS RHYTHM's (the open bar, the playing bar while FOLLOW runs, else the
 * strip page's first); 0.22 on JAM the playing bar */
static int bass_shown_bar(const seq_t *q) {   /* 0.22 on JAM the playing bar (the KEYS', as MELODY JAM's); 0.24 on PHRASE its centre */
    return seq_bmode(q) == 2 ? seq_cur_bar(q) : seq_bmode(q) ? ph_centre(q, RP_B) : bass_centre(q);
}
/* ukbar (the KEYS' bar) and, while the tab shows BASS, ufirst, ucentre (ushown_first, ushown_centre), uplaying and utap (uwh_text): made
 * in set_param and song_pos */
static void bass_shown_now(inst_t *s) {
    const seq_t *q = &s->seq;
    s->ukbar = bass_shown_bar(q);
    if (seq_bmode(q)) return;
    s->ufirst = bstrip_first(q), s->ucentre = bass_centre(q);
    s->uplaying = bass_live(q) && !bass_txn(q), s->utap = !bass_txn(q);
}
/* cx_2 on a drawn Bass lane: "5 notes" (BASS), "5 hits a bar" (RHYTHM), the shown bar's; else 0.13's text */
static int cx2_text(const inst_t *s, char *buf, int len) {
    const seq_t *q = &s->seq;
    if (!seq_lane_drawn(q, LANE_BASS)) return put(buf, len, s->ktext[seq_part(q)][KT_CX2]);
    return cx_hits_text(q, LANE_BASS, bass_shown_bar(q), !seq_bmode(q), buf, len);
}

/* 0.18 the RHYTHM page's readouts (0.19 per page: rtt .. rctl, ytt .. yctl; rp_readout). rtt: "EDITING \xC2\xB7 BAR 6 \xC2\xB7 C" (open),
 * "PLAYING \xC2\xB7 BAR 2 \xC2\xB7 Em" (following), "BAR 5 \xC2\xB7 G" */
static int rp_title(const seq_t *q, const rpage_t *P, char *buf, int len) {
    int bar = rp_shown(q, P);
    txt_t t = {buf, len, 0};
    add(&t, seq_rbar_bar(q, P->lane) >= 0 ? "EDITING \xC2\xB7 " : rp_live(q, P) ? "PLAYING \xC2\xB7 " : "");
    add(&t, "BAR "), add_int(&t, bar + 1), add(&t, " \xC2\xB7 "), add(&t, inst_of(q)->rbar_text[seq_part(q)][bar]);
    return t.n;
}
/* rnote: COPY TO...'s, the browser's, the open bar's paint's explanation, else how to open a bar */
static int rp_note_text(const seq_t *q, const rpage_t *P, char *buf, int len) {
    int view = P->view(q), bar = seq_rbar_bar(q, P->lane);
    txt_t t = {buf, len, 0};
    if (view == P->copy) return put(buf, len, "Tap bars to pick them \xC2\xB7 locked bars are skipped");
    if (view == P->rbrowse && bar < 0) return put(buf, len, "Tap one to hear it now in every unlocked bar");
    if (bar < 0) return put(buf, len, "Tap a bar or a step to edit that bar");
    if (view == P->rbrowse) {
        add(&t, "Tap one to hear it in bar "), add_int(&t, bar + 1);
        return t.n;
    }
    if (seq_bar_locked(q, P->lane, bar)) add(&t, "LOCKED \xC2\xB7 ");
    add(&t, paint_name(inst_of(q)->rpaint[P->dl])), add(&t, ": tap steps to paint, again to clear");
    return t.n;
}
static int rp_fll_text(const seq_t *q, const rpage_t *P, char *buf, int len) {
    int bar = seq_rbar_bar(q, P->lane);
    txt_t t = {buf, len, 0};
    if (bar < 0) return put(buf, len, "RHYTHM \xC2\xB7 ALL BARS");
    add(&t, "RHYTHM \xC2\xB7 BAR "), add_int(&t, bar + 1);
    return t.n;
}
/* rcnote: "Replaces 2 of them that differ \xC2\xB7 one UNDO step"; none picked, how many other bars differ now */
static int rp_cnote_text(const seq_t *q, const rpage_t *P, char *buf, int len) {
    unsigned mask = seq_rcopy_mask(q, P->lane);
    int rep = 0;
    txt_t t = {buf, len, 0};
    if (P->view(q) != P->copy) return put(buf, len, " ");
    seq_rbar_copy_counts(q, P->lane, mask ? mask : ~0u, &rep, NULL);
    if (!mask && !rep) return put(buf, len, seq_rcopy_what(q, P->lane) ? "Every other bar is the same now" : "Every other bar has the same rhythm now");
    if (mask) {
        add(&t, "Replaces "), add_int(&t, rep);
        add(&t, rep == 1 ? " of them that differs \xC2\xB7 one UNDO step" : " of them that differ \xC2\xB7 one UNDO step");
    } else if (seq_rcopy_what(q, P->lane)) {   /* release fix: + CHORDS / + NOTES counts those too */
        add_int(&t, rep), add(&t, rep == 1 ? " other bar differs now" : " other bars differ now");
    } else {
        add_int(&t, rep), add(&t, rep == 1 ? " other bar has a different rhythm now" : " other bars have a different rhythm now");
    }
    return t.n;
}
static int rp_ctl_text(const seq_t *q, const rpage_t *P, char *buf, int len) {
    txt_t t = {buf, len, 0};
    if (P->view(q) != P->copy) return put(buf, len, " ");
    add(&t, "COPY BAR "), add_int(&t, seq_rbar_bar(q, P->lane) + 1), add(&t, " TO...");
    return t.n;
}
static int msg_text(const seq_t *q, int m, char *buf, int len) {   /* 0.19.1 " " once faded: compares, changes nothing */
    const inst_t *s = inst_of(q);
    return put(buf, len, msg_shows(s, m, s->frames) ? msg_of(s, m) : " ");
}
static int rp_msg_text(const seq_t *q, const rpage_t *P, char *buf, int len) { return msg_text(q, P->dl, buf, len); }
/* A readout of the page, by its key after the r or y: 0 for no readout of the page */
static int rp_readout(const seq_t *q, const rpage_t *P, const char *k, char *buf, int len) {
    if (!strcmp(k, "tt")) return rp_title(q, P, buf, len);
    if (!strcmp(k, "note")) return rp_note_text(q, P, buf, len);
    if (!strcmp(k, "msg")) return rp_msg_text(q, P, buf, len);
    if (!strcmp(k, "fll")) return rp_fll_text(q, P, buf, len);
    if (!strcmp(k, "cnote")) return rp_cnote_text(q, P, buf, len);
    if (!strcmp(k, "ctl")) return rp_ctl_text(q, P, buf, len);
    return 0;
}

/* 0.19 the BASS page's readouts (ufol, upage: tb_fol_text, tb_page_text) */
/* The centre's bar: uwh "PLAYING \xC2\xB7 BAR 3 \xC2\xB7 G \xC2\xB7 tap to change" (PLAYING while FOLLOW runs and plays; " \xC2\xB7 LOCKED" on a
 * note-locked bar; "tap to change" with no Bass transaction open), ucn its notes in order, each once, and how many play ("G1 D2 G2
 * \xC2\xB7 5 notes"), utn its tension ("colourful \xC2\xB7 34") */
static int uwh_text(const seq_t *q, char *buf, int len) {
    const inst_t *s = inst_of(q);
    int bar = ushown_centre(q), off = seq_bmode(q);   /* off its page: PLAYING and tap to change as last shown (bass_shown_now) */
    txt_t t = {buf, len, 0};
    if (off ? s->uplaying : bass_live(q) && !bass_txn(q)) add(&t, "PLAYING \xC2\xB7 ");
    add(&t, "BAR "), add_int(&t, bar + 1), add(&t, " \xC2\xB7 "), add(&t, s->rbar_text[seq_part(q)][bar]);
    if (bar_nlocked(q, bar)) add(&t, " \xC2\xB7 LOCKED");
    if (off ? s->utap : !bass_txn(q)) add(&t, " \xC2\xB7 tap to change");
    return t.n;
}
static int ucn_text(const seq_t *q, char *buf, int len) { return put(buf, len, inst_of(q)->ucn_text[seq_part(q)][ushown_centre(q)]); }
static int utn_text(const seq_t *q, char *buf, int len) {
    int bar = ushown_centre(q), score = inst_of(q)->bscore[seq_part(q)][bar];
    txt_t t = {buf, len, 0};
    add(&t, bass_level_word(bass_level(score))), add(&t, " \xC2\xB7 "), add_int(&t, score);
    return t.n;
}
static int ustg_text(const seq_t *q, char *buf, int len) {
    int st = seq_bass_style(q);
    return put(buf, len, st >= 0 && st < BASS_STYLE_COUNT ? bass_genre_name(bass_style_genre(st)) : " ");
}
/* The bar editor's title "EDITING \xC2\xB7 BAR 6 \xC2\xB7 C \xC2\xB7 own line" (own: a NEW BAR line) and its note line; " " closed */
static int uett_text(const seq_t *q, char *buf, int len) {
    int bar = seq_rbar_bar(q, LANE_BASS);
    txt_t t = {buf, len, 0};
    if (bar < 0) return put(buf, len, " ");
    add(&t, "EDITING \xC2\xB7 BAR "), add_int(&t, bar + 1), add(&t, " \xC2\xB7 "), add(&t, inst_of(q)->rbar_text[seq_part(q)][bar]);
    if (ppart(q)->lane[LANE_BASS].bar_var[bar]) add(&t, " \xC2\xB7 own line");
    return t.n;
}
static int uent_text(const seq_t *q, char *buf, int len) {
    int bar = seq_rbar_bar(q, LANE_BASS);
    if (bar < 0) return put(buf, len, " ");
    return put(buf, len, bar_nlocked(q, bar) ? "LOCKED: knobs and styles leave this bar alone" : "Notes from BASS, timing from RHYTHM \xC2\xB7 * = fixed");
}
static int umsg_text(const seq_t *q, char *buf, int len) { return msg_text(q, MSG_BASS, buf, len); }
/* 0.20 the JAM page's readouts: jnow the playing block ("HOOK \xC2\xB7 MOTIF"; " " on a lane that isn't JAM), jnext the queued one and the bar
 * it takes over at ("NEXT \xC2\xB7 ARP \xC2\xB7 PINKY \xC2\xB7 at bar 5", else "Tap a block to switch"), jcnt the bar counter with the playing
 * bar's first chord ("> BAR 2 / 16 \xC2\xB7 C"), jrole its FORM and role ("1+1 \xC2\xB7 ANSWER"; "LOOP"), jmsg the message line */
static int jnow_text(const seq_t *q, char *buf, int len) {
    const mjam_set_t *m = &ppart(q)->mjam.set;
    txt_t t = {buf, len, 0};
    if (!mel_jam_on(q)) return put(buf, len, " ");
    add_block(&t, m->c, m->l);
    return t.n;
}
static int jnext_text(const seq_t *q, char *buf, int len) {
    int c, l, at;
    txt_t t = {buf, len, 0};
    if (!seq_mel_queued(q, &c, &l, &at)) return put(buf, len, "Tap a block to switch");
    add(&t, "NEXT \xC2\xB7 "), add_block(&t, c, l), add(&t, " \xC2\xB7 at bar "), add_int(&t, at + 1);
    return t.n;
}
static int jcnt_text(const seq_t *q, char *buf, int len) {
    const inst_t *s = inst_of(q);
    char first[sizeof s->rbar_text[0][0]];
    int n = ccount_text(q, buf, len);
    txt_t t = {buf, len, n};
    first_chord(s->rbar_text[seq_part(q)][seq_cur_bar(q)], first);
    add(&t, " \xC2\xB7 "), add(&t, first);
    return t.n;
}
static int jrole_text(const seq_t *q, char *buf, int len) {
    int bar = seq_cur_bar(q), f = mel_form_of(q, bar);
    txt_t t = {buf, len, 0};
    if (!mel_jam_on(q)) return put(buf, len, " ");
    add(&t, mjam_form_name(f));
    if (f != MF_LOOP) add(&t, " \xC2\xB7 "), add(&t, mel_role_word(q, bar));
    return t.n;
}
static void add_lower(txt_t *t, const char *s) {   /* s in lower case: the knob step names (BMOV_NAMES ...) in running text */
    for (; *s; s++) add(t, (char[2]){(char)(*s >= 'A' && *s <= 'Z' ? *s - 'A' + 'a' : *s), 0});
}
/* What Bass settings b set, in words: "moves some \xC2\xB7 oct some \xC2\xB7 half below" (APPROACH None left out), "pedal tonic \xC2\xB7
 * low" with a pedal on: ubtl's (the part's) and each STYLE tile's (uts_, its style's) */
static void bset_words(txt_t *t, const bass_set_t *b) {
    static const char *const PEDAL_WORD[3] = {"", "pedal tonic", "pedal dominant"};
    if (b->pedal) {
        add(t, PEDAL_WORD[b->pedal]), add(t, " \xC2\xB7 "), add_lower(t, BREG_NAMES[b->k[BK_REG] + 2]);
        return;
    }
    add(t, "moves "), add_lower(t, BMOV_NAMES[b->k[BK_MOV]]), add(t, " \xC2\xB7 oct "), add_lower(t, BMOV_NAMES[b->k[BK_OCTS]]);
    if (b->k[BK_APP]) add(t, " \xC2\xB7 "), add_lower(t, BAPP_NAMES[b->k[BK_APP]]);
}
/* ubtl, the STYLE browser's title and what the style shown sets: "STYLES \xC2\xB7 moves some \xC2\xB7 oct some \xC2\xB7 half below" ("STYLES
 * \xC2\xB7 pedal tonic \xC2\xB7 low" on a pedal style); BASS RHYTHM's RHYTHMS browser's (btl_rhythm); " " closed */
static int ubtl_text(const seq_t *q, char *buf, int len) {
    txt_t t = {buf, len, 0};
    if (seq_bview(q) == BVIEW_RBROWSE) {   /* BASS RHYTHM's RHYTHMS browser, as btl */
        btl_rhythm(q, RP_B, &t);
        return t.n;
    }
    if (seq_bview(q) != BVIEW_STYLE) return put(buf, len, " ");
    add(&t, "STYLES \xC2\xB7 ");
    bset_words(&t, &ppart(q)->bass.set);
    return t.n;
}

/* 0.20 cx_3 on a JAM Melody lane: the block its level reads (the playing column at seq_mel_level: after a turn, the block queued),
 * "HOOK \xC2\xB7 MOTIF", its name alone past PLAY's knob box (13 characters); else 0.13's text */
static int cx3_text(const inst_t *s, char *buf, int len) {
    const seq_t *q = &s->seq;
    int c = ppart(q)->mjam.set.c, l = seq_mel_level(q);
    txt_t t = {buf, len, 0};
    if (!mel_jam_on(q)) return put(buf, len, s->ktext[seq_part(q)][KT_CX3]);
    add_block(&t, c, l);
    return theory_text_width(buf) <= 13 ? t.n : put(buf, len, mjam_block_name(c, l));
}

/* 0.13 the dynamic_display params' texts (<key>_display): copied from the playing part's caches (knob_labels). 0.17: -1 COLOUR's
 * level (ccol and k_col, retired, read it too), -2 INVERSION, -3 SPREAD, -4 STRUM; 0.18 (vk_text: "user > effective" while
 * COMPLEXITY moves them) the slots k_s2..5 (slot_text), cx_1 on a drawn lane its hits (cx1_text); 0.19 k_b2..5, 0.20 k_j2..5, cx_3 on a
 * JAM lane its block (cx3_text); 0.21 k_j6..8; 0.22 k_s6, k_s8, k_b6, k_b8 and knob 1's k_s1, k_b1 (k1_text). The slots by their key
 * (slot_of_key) */
static const struct { const char *key; int kt; } DISPLAYS[] = {
    {"ccol", -1}, {"k_col", -1}, {"k_vcol", -1}, {"k_vinv", -2}, {"k_vspr", -3}, {"k_vstr", -4}, {"jpush", KT_JPUSH},
    {"jdyn", KT_JDYN},
    {"cx_1", KT_CX1}, {"cx_2", KT_CX2}, {"cx_3", KT_CX3}, {"swing", KT_SWING}, {"gate", KT_GATE},
    {"dyn", KT_DYN}, {"cgate", KT_CGATE}, {"cdyn", KT_CDYN}, {"cstrum", KT_CSTRUM}, {"cpush", KT_CPUSH}, {"bpush", KT_BPUSH},
    {"bgate", KT_BGATE}, {"bacc", KT_BACC}, {"mgat", KT_MGAT}, {"k_harm", KT_KS + KS_HARM}, {"k_spr", KT_KS + KS_SPR},
    {"k_bmov", KT_KS + KS_BMOV}, {"k_bsyn", KT_KS + KS_BSYN}, {"k_boct", KT_KS + KS_BOCT}, {"k_breg", KT_KS + KS_BREG},
    {"k_mrng", KT_KS + KS_MRNG}, {"k_mlea", KT_KS + KS_MLEA}, {"k_msyn", KT_KS + KS_MSYN}, {"k_mten", KT_KS + KS_MTEN},
    {"k_mans", KT_KS + KS_MANS}, {"k_mreg", KT_KS + KS_MREG}};
/* A slot's key ("k_s2" .. "k_s8"; "k_b2" .. "k_b8" (0.24 k_s7, k_b7 too); "k_j2" .. "k_j8"; its first n characters): its row of T's table,
 * else -1; knob 1's (k_s1, k_b1) *k1 its lane (DL_*), else -1 */
static int slot_of_key(const char *key, int n, const kslots_t **T, int *k1) {
    *k1 = -1;
    if (n != 4 || key[0] != 'k' || key[1] != '_' || (key[2] != 's' && key[2] != 'b' && key[2] != 'j')) return -1;
    if (key[3] == '1' && key[2] != 'j') *k1 = key[2] == 'b' ? DL_BASS : DL_CHORDS;
    if (key[3] < '2' || key[3] > '8') return -1;
    *T = &KSLOTS[key[2] == 'b' ? TAB_BASS : key[2] == 'j' ? TAB_MEL : TAB_CHORDS];
    return key[3] - '2';
}
static int display_text(const inst_t *s, const char *key, int n, char *buf, int len) {   /* n: the key's length before "_display" */
    const seq_t *q = &s->seq;
    const kslots_t *T;
    int k1, j = slot_of_key(key, n, &T, &k1);
    if (j >= 0) return slot_text(q, T, j, buf, len);
    if (k1 >= 0) return k1_text(s, k1, buf, len);
    for (size_t i = 0; i < sizeof DISPLAYS / sizeof DISPLAYS[0]; i++) {
        if (strncmp(key, DISPLAYS[i].key, (size_t)n) || DISPLAYS[i].key[n]) continue;
        int kt = DISPLAYS[i].kt;
        if (kt < 0) return vk_text(q, KS_CHORDS[-1 - kt].knob[0], buf, len);
        if (kt == KT_CX1) return cx1_text(s, buf, len);
        if (kt == KT_CX2) return cx2_text(s, buf, len);
        if (kt == KT_CX3) return cx3_text(s, buf, len);
        return put(buf, len, s->ktext[seq_part(q)][kt]);
    }
    return 0;
}

/* 0.12 itag, bitag, mitag: the last move's text on its own lane's tag while there is one, else the idle tag of IDEAS's Near (retired) */
static int tag_text(const seq_t *q, int lane, char *buf, int len) {
    if (seq_last_move_lane(q) == lane) return seq_last_move_text(q, buf, len);
    return put(buf, len, "Near: small changes");
}

static int get_param(void *p, const char *key, char *buf, int len) {
    seq_t *q = &((inst_t *)p)->seq;
    skey_t k = parse_kind(key);
    switch (k.kind) {
    case K_CELL:
        if (k.on) return put_flag(buf, len, seq_step_on(q, k.a, k.b));
        if (k.a != LANE_CHORDS) return seq_cell_label(q, k.a, k.b, buf, len);
        /* 0.12 the Chords lane's cells: the triad name alone ("F#dim"), for the larger text */
        return put(buf, len, seq_step_on(q, k.a, k.b) ? ((inst_t *)p)->tri_text[seq_part(q)][seq_slot_at(q, seq_grid_bar(q), k.b)] : " ");
    case K_PH:   /* 0.20 VST-17: the playing beat's 4 cells, so MPC redraws once a beat, not every step */
        return k.on ? put_flag(buf, len, seq_cur_step(q) >= 0 && seq_cur_step(q) / 4 == k.a / 4 && seq_cur_bar(q) == seq_grid_bar(q))
                    : put(buf, len, " ");
    case K_CHORD: return k.on ? put_flag(buf, len, seq_cur_slot(q) == k.a) : seq_slot_name(q, k.a, buf, len);
    case K_MUTE: return k.on ? 0 : put_flag(buf, len, q->mute[k.a]);
    case K_TAP: return k.on ? put_flag(buf, len, TAPS[k.a].on(q, k.b) || latched(p, k)) : TAPS[k.a].label(q, k.b, buf, len);
    default: break;
    }
    if (!strcmp(key, "part")) return put_upper(buf, len, SEQ_PART_NAME[seq_part(q)]);   /* polled at 10 Hz: before the option lookup */
    if (!strcmp(key, "init")) return snprintf(buf, len, "%d", ((inst_t *)p)->program);
    if (key[0] == 'z' && (key[1] == 's' || key[1] == 'c') && key[2] == '_') {   /* 0.20 the dots: zdot (mel_dots_now), read on display_rev */
        const char *d = key + 3;
        int i = read_num(&d, key[1] == 's' ? 64 : SEQ_STEPS);
        return i >= 0 && !*d ? snprintf(buf, len, "%d", ((inst_t *)p)->zdot[(key[1] == 's' ? 0 : 64) + i]) : 0;
    }
    if ((key[0] == 'c' || key[0] == 'u') && key[1] == 'q') {   /* 0.24 Task 7 the PHRASE pages: their dots, step marks and readouts */
        const phcells_t *c = &((inst_t *)p)->ph[key[0] == 'u'];
        const char *d = key + 4;
        int i, n;
        if (key[2] == 'z' && (key[3] == 's' || key[3] == 'c') && *d++ == '_' && (i = read_num(&d, key[3] == 's' ? 64 : SEQ_STEPS)) >= 0 && !*d)
            return snprintf(buf, len, "%d", c->dots[(key[3] == 'c') * 64 + i]);
        d = key + 4;
        if (key[2] == 'm' && key[3] == '_' && (i = read_num(&d, SEQ_STEPS)) >= 0 && !*d) return snprintf(buf, len, "%d", c->mark[i]);
        if ((n = ph_readout(q, key[0] == 'u' ? RP_B : RP_C, key + 2, buf, len)) > 0) return n;
    }
    if ((key[0] == 'c' || key[0] == 'u') && key[1] == 'j' && (key[2] == 'm' || key[2] == 'q') && key[3] == '_') {   /* 0.22 the JAM meters */
        const char *d = key + 4;
        int i = read_num(&d, key[2] == 'm' ? JAM_COLS * JAM_LEVELS : 4);
        return i >= 0 && !*d ? snprintf(buf, len, "%d", ((inst_t *)p)->jmeter[key[0] == 'u'][(key[2] == 'q') * JAM_COLS * JAM_LEVELS + i]) : 0;
    }
    int n = get_option(q, key, buf, len);
    if (n > 0) return n;
    if (!strcmp(key, "display_rev")) return snprintf(buf, len, "%u", q->display_rev);
    if (!strcmp(key, "pos")) return pos_text(q, buf, len);
    if (!strcmp(key, "title")) return play_title(p, buf, len);
    if (!strcmp(key, "bpm")) return bpm_text(q, buf, len);
    if (!strcmp(key, "vchord")) return vchord_text(q, buf, len);
    if (!strcmp(key, "itag")) return tag_text(q, LANE_CHORDS, buf, len);
    if (!strcmp(key, "bitag")) return tag_text(q, LANE_BASS, buf, len);
    if (!strcmp(key, "bvw") || !strcmp(key, "mvw")) return bvw_text(q, buf, len);
    if (!strcmp(key, "mcap")) return mcap_text(q, buf, len);
    if (!strcmp(key, "mbars")) return mbars_text(q, buf, len);
    if (!strcmp(key, "mphint")) return phrase_hint(q, buf, len);
    if (!strcmp(key, "bfn") || !strcmp(key, "mfn")) return put(buf, len, " ");   /* 0.24.1 FIX by bar's summaries: their texts are gone */
    if (!strcmp(key, "mkst")) return mkst_text(q, buf, len);
    if (!strcmp(key, "mitag")) return tag_text(q, LANE_MEL_UP, buf, len);
    if (!strcmp(key, "lnote")) return lnote_text(q, buf, len);
    if (!strcmp(key, "cfixnote")) return put(buf, len, " ");   /* 0.24.1 FIX's summary: its text is gone */
    if (!strcmp(key, "cfview_display")) return cfview_text(q, buf, len);
    if (!strcmp(key, "cshtag")) return seq_last_move_lane(q) == LANE_CHORDS ? seq_last_move_text(q, buf, len) : put(buf, len, " ");
    if (!strcmp(key, "cpage")) return tb_page_text(q, TAB_CHORDS, buf, len);
    if (!strcmp(key, "ccount")) return ccount_text(q, buf, len);
    if (!strcmp(key, "cfol")) return tb_fol_text(q, TAB_CHORDS, buf, len);
    if ((key[0] == 'c' || key[0] == 'u') && key[1] == 's') {   /* 0.24 the sound pages' NOW PLAYING and centre (sound_cells_now) */
        const sound_t *n = &((inst_t *)p)->snd;
        int d = key[0] == 'u' ? DL_BASS : DL_CHORDS;
        if (!strcmp(key + 2, "now")) return put(buf, len, n->now[d]);
        if (!strcmp(key + 2, "wh")) return put(buf, len, n->wh[d]);
        if (!strcmp(key + 2, "nt")) return put(buf, len, n->nt[d]);
        if (!strcmp(key + 2, "wd")) return put(buf, len, n->wd[d]);
        if (d == DL_BASS && key[2] == 'z' && key[3] == '_') {   /* Task 8 fix: usz_1..16 the BASS centre's dots, read on display_rev */
            const char *z = key + 4;
            int i = read_num(&z, SEQ_STEPS);
            if (i >= 0 && !*z) return snprintf(buf, len, "%d", n->dots[i]);
        }
    }
    if (!strcmp(key, "ccn")) return seq_slot_label(q, seq_centre_slot(q, TAB_CHORDS), buf, len);
    if (!strcmp(key, "ccd")) return put(buf, len, inst_of(q)->ccd_text[seq_part(q)][seq_centre_slot(q, TAB_CHORDS)]);
    if (!strcmp(key, "cdr")) return cdr_text(q, buf, len);
    if (!strcmp(key, "ptl")) return ptl_text(q, buf, len);
    if (!strcmp(key, "btl")) return btl_text(q, buf, len);
    if (key[0] == 'r' || key[0] == 'y') {   /* 0.18 the RHYTHM page's readouts; 0.19 BASS RHYTHM's */
        int n = rp_readout(q, key[0] == 'r' ? RP_C : RP_B, key + 1, buf, len);
        if (n > 0) return n;
    }
    if (key[0] == 'u') {   /* 0.19 the BASS page's readouts */
        if (!strcmp(key, "ucnt")) return ccount_text(q, buf, len);
        if (!strcmp(key, "ufol")) return tb_fol_text(q, TAB_BASS, buf, len);
        if (!strcmp(key, "upage")) return tb_page_text(q, TAB_BASS, buf, len);
        if (!strcmp(key, "ucw")) return ucw_text(q, buf, len);
        if (!strcmp(key, "ucn")) return ucn_text(q, buf, len);
        if (!strcmp(key, "uwh")) return uwh_text(q, buf, len);
        if (!strcmp(key, "utn")) return utn_text(q, buf, len);
        if (!strcmp(key, "ustn")) return ustn_text(q, buf, len);
        if (!strcmp(key, "ustg")) return ustg_text(q, buf, len);
        if (!strcmp(key, "uett")) return uett_text(q, buf, len);
        if (!strcmp(key, "uent")) return uent_text(q, buf, len);
        if (!strcmp(key, "umsg")) return umsg_text(q, buf, len);
        if (!strcmp(key, "ubtl")) return ubtl_text(q, buf, len);
    }
    if ((key[0] == 'c' || key[0] == 'u') && key[1] == 'j') {   /* 0.22 the CHORDS and BASS JAM pages' readouts */
        int d = key[0] == 'u' ? DL_BASS : DL_CHORDS;
        if (!strcmp(key + 2, "now")) return jnow_d_text(q, d, buf, len);
        if (!strcmp(key + 2, "next")) return jnext_d_text(q, d, buf, len);
        if (!strcmp(key + 2, "msg")) return msg_text(q, MSG_JAM + d, buf, len);
    }
    if (key[0] == 'j') {   /* 0.20 the JAM page's readouts */
        if (!strcmp(key, "jnow")) return jnow_text(q, buf, len);
        if (!strcmp(key, "jnext")) return jnext_text(q, buf, len);
        if (!strcmp(key, "jcnt")) return jcnt_text(q, buf, len);
        if (!strcmp(key, "jrole")) return jrole_text(q, buf, len);
        if (!strcmp(key, "jmsg")) return msg_text(q, MSG_MEL, buf, len);
    }
    if (key[0] == 'w') {   /* 0.20 the PHRASE page's readouts */
        int n = w_readout(q, key, buf, len);
        if (n > 0) return n;
    }
    if (!strncmp(key, "tint_", 5) && key[5] >= '1' && key[5] <= '4' && !key[6]) return snprintf(buf, len, "%d", tint_of(q, key[5] - '1'));
    if (!strncmp(key, "k_", 2) && key[2] && key[3] && !strcmp(key + 4, "_name")) {   /* 0.21 k_j6..8 too; 0.22 k_s6, k_s8, k_b6, k_b8 and knob 1 */
        const kslots_t *T;
        int k1, j = slot_of_key(key, 4, &T, &k1);
        if (j >= 0) return put(buf, len, T->name[T->mode(q)][j]);
        if (k1 >= 0) return put(buf, len, k1_name(q, k1));
    }
    size_t kn = strlen(key);
    if (kn > 8 && !strcmp(key + kn - 8, "_display")) return display_text(p, key, (int)kn - 8, buf, len);
    if (!strcmp(key, "panic") || trigger_index(key) >= 0) return put(buf, len, "0");
    if (!strcmp(key, "state")) return seq_state_save(q, buf, len);
    return 0;
}

static const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, NULL, song_pos};
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
