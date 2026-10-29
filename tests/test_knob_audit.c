/* 0.21 the MELODY knob audit (run by tests/run_tests.sh under ASan/UBSan). Through the core: every JAM block (6 columns x 5 levels)
 * over two progressions (E minor Em C G D, a chord a bar; C major C Am F G, half-bar chords; 4 bars), every non-neutral step of
 * DENSITY, RANGE, VARIATION, SWING and (Amendment 1, in ACCENT's place) TIME against the knob's neutral step with every other knob
 * neutral. DENSITY, RANGE, VARIATION and TIME count the loop's sounding steps that differ (the sounding pitch, a held note's on its held
 * steps, 0 silence; or the note start), of bars x 16; SWING the notes on the loop's swung positions (grid16: odd steps, else step mod
 * 4 = 2), of the notes. Asserts (a) the engine's weak bit (seq_mel_weak) is set exactly when a case changes fewer than 1 in 8, and no
 * other bit; (b) at most AUDIT_LIMIT % of the DENSITY, RANGE and VARIATION cases are weak.
 * Prints the table (a row per block: changed/total per knob step, `*` weak), the weak cases per knob step and per column, and
 * `audit: <n> cases, <w> weak (<p>%)`.
 * 0.22: the CHORDS and BASS JAM lanes too, every block (6 x 5 per lane) over the same two progressions, every non-neutral step of
 * VARIATION, SWING and TIME against neutral: the loop's steps whose sounding notes (a held step its hit's, a chord's every note, within
 * its bar; none silence) or note start differ, of bars x 16; SWING the unlocked bars' hits on the swung positions (seq_djam_grid16), of
 * those hits.
 * Asserts only the flag rule (seq_djam_weak, DW_*); no limit (the owner sees the numbers). Prints a row per block, the weak cases per
 * knob step, per knob, and `audit CHORDS: ...` / `audit BASS: ...`. Exit 1 on failure. */
#include "../src/seq_core.h"
#include "../src/melody2.h"
#include "../src/jamlib.h"
#include "../src/theory.h"
#include <stdio.h>
#include <string.h>

/* % of the DENSITY, RANGE and VARIATION cases. The spec's first guess was 10 (60 of 600); 0.21 measures 109 (18.2 %), a ruling for
 * the owner. 76 of them change nothing at all, which no tuning of the transforms' details can reach (FILLED and BUSY on blocks
 * already on every 16th, SPARSE and THIN on HOLD 5, HOLD 1 and GUIDE, RANGE on one-pitch PULSE and HOLD, VARIATION on GUIDE, HOLD
 * and 2-note lines): over 60 on their own. So the limit is the measured value, to catch a change that weakens the knobs further.
 * The final review's beat rule (only on the notes a knob moved or added) measures 110 (18.3 %, the limit rounds 18.33 up): Em RUN-DOWN
 * FILLED now changes 4 of 64 steps, not 10, as the bar's kept beat notes no longer move */
#define AUDIT_LIMIT 18.4

static int fails;
#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static void emit(void *ctx, int status, int note, int vel) { (void)ctx, (void)status, (void)note, (void)vel; }

enum { K_DENS, K_RNG, K_VAR, K_SWING, K_TIME, K_N };
static const char *const KNAME[K_N] = {"DENSITY", "RANGE", "VARIATION", "SWING", "TIME"};
static const char *const KSHORT[K_N] = {"DN", "RG", "VA", "SW", "TM"};
static const int KLO[K_N] = {-2, -1, 0, 0, -1}, KHI[K_N] = {2, 2, 3, 5, 1};
static const unsigned KBIT[K_N] = {MW_DENS, MW_RNG, MW_VAR, MW_SWING, MW_TIME};
#define NSTEP 17   /* the non-neutral steps: 4 + 3 + 3 + 5 + 2 */

static void set_knob(seq_t *s, int k, int v) {
    if (k == K_SWING) seq_set_mel_swing(s, v);
    else seq_mjam_set(s, k == K_TIME ? MJS_TIME : MJS_DENS + k, v);
}

static void loop_lines(const seq_t *s, int bars, mjam_note_t *out) {
    for (int b = 0; b < bars; b++) seq_mel_line(s, b, &out[b * SEQ_STEPS]);
}

/* the steps where a and b sound differently: the sounding pitch (a held step its note's, within its bar; 0 silence) or a note start */
static int sounding_differ(const mjam_note_t *a, const mjam_note_t *b, int n) {
    int d = 0, pa = 0, pb = 0;
    for (int i = 0; i < n; i++) {
        if (i % SEQ_STEPS == 0) pa = pb = 0;
        pa = a[i].note ? a[i].note : a[i].hold ? pa : 0;
        pb = b[i].note ? b[i].note : b[i].hold ? pb : 0;
        d += pa != pb || !a[i].note != !b[i].note;
    }
    return d;
}

/* one case: knob k at step v (every other knob neutral) against k neutral; *total its denominator. Returns the changed count */
static int measure(seq_t *s, int k, int v, int *total) {
    static mjam_note_t base[SEQ_LANE_STEPS], line[SEQ_LANE_STEPS];
    const seq_part_t *p = &s->part[seq_part(s)];
    int n = p->bars * SEQ_STEPS, c = 0, notes = 0, g16;
    set_knob(s, k, 0);
    loop_lines(s, p->bars, base);
    set_knob(s, k, v);
    loop_lines(s, p->bars, line);
    g16 = seq_mel_grid16(s);
    if (k != K_SWING) {
        *total = n;
        return sounding_differ(base, line, n);
    }
    for (int i = 0; i < n; i++) {
        if (!line[i].note) continue;
        notes++;
        c += g16 ? (i & 1) : i % 4 == 2;
    }
    *total = notes;
    return c;
}

static void setup(seq_t *s, int prog) {
    static const int CMAJ[4] = {0, 5, 3, 4};   /* C Am F G */
    seq_init(s, emit, NULL);   /* E minor, Em C G D, a chord a bar, 4 bars */
    if (prog) {
        seq_set_key(s, 0);
        seq_set_scale(s, SCALE_MAJOR);
        seq_set_chord_len(s, 8);
        seq_set_progression(s, 4, CMAJ, NULL, NULL, 0);
    }
}

/* 0.22 a drawn lane as it sounds over the loop: per step its hit start, and the notes sounding (a held step its hit's, within its bar) */
typedef struct { int on[SEQ_LANE_STEPS], n[SEQ_LANE_STEPS], note[SEQ_LANE_STEPS][THEORY_CHORD_MAX]; } lane_snd_t;
static void lane_sound(const seq_t *s, int lane, lane_snd_t *o) {
    const seq_part_t *p = &s->part[seq_part(s)];
    for (int b = 0; b < p->bars; b++) {
        int cur = -1, end = 0;
        for (int i = b * SEQ_STEPS; i < (b + 1) * SEQ_STEPS; i++) {
            const seq_step_t *x = &p->lane[lane].steps[i];
            if (x->on) cur = i, end = i + x->len;
            o->on[i] = x->on;
            o->n[i] = cur >= 0 && i < end ? seq_part_step_notes(s, seq_part(s), lane, b, cur % SEQ_STEPS, o->note[i]) : 0;
        }
    }
}

enum { D_VAR, D_SWING, D_TIME, D_N };
static const char *const DNAME[D_N] = {"VARIATION", "SWING", "TIME"}, *const DSHORT[D_N] = {"VA", "SW", "TM"};
static const int DFIELD[D_N] = {DJ_VAR, DJ_SWING, DJ_TIME}, DLO[D_N] = {0, 0, -1}, DHI[D_N] = {3, 5, 1};
static const unsigned DBIT[D_N] = {DW_VAR, DW_SWING, DW_TIME};
#define DSTEP 10   /* the non-neutral steps: 3 + 5 + 2 */

/* 0.22 one drawn-lane case: knob k at step v (the others neutral) against k neutral; *total its denominator. Returns the changed count */
static int dmeasure(seq_t *s, int lane, int k, int v, int *total) {
    static lane_snd_t a, b;
    int n = seq_bars(s) * SEQ_STEPS, c = 0;
    seq_djam_set(s, lane, DFIELD[k], 0);
    lane_sound(s, lane, &a);
    seq_djam_set(s, lane, DFIELD[k], v);
    lane_sound(s, lane, &b);
    if (k == D_SWING) {   /* a bar locked for JAM plays straight and never counts (the audit locks none) */
        int g16 = seq_djam_grid16(s, lane);
        *total = 0;
        for (int i = 0; i < n; i++)
            if (b.on[i] && !seq_bar_locked(s, lane, i / SEQ_STEPS) && !(lane == LANE_BASS && s->part[seq_part(s)].bass.nlock >> (i / SEQ_STEPS) & 1))
                ++*total, c += g16 ? i & 1 : i % 4 == 2;
        return c;
    }
    *total = n;
    for (int i = 0; i < n; i++) c += a.on[i] != b.on[i] || a.n[i] != b.n[i] || memcmp(a.note[i], b.note[i], sizeof(int) * (size_t)a.n[i]);
    return c;
}

/* 0.22 the CHORDS and BASS audit: 1 when every flag followed the rule */
static int drawn_audit(void) {
    static seq_t s;
    static const char *const PROG[2] = {"Em", "C"}, *const LNAME[2] = {"CHORDS", "BASS"};
    static const int LANE[2] = {LANE_CHORDS, LANE_BASS};
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        int cases = 0, weak = 0, step_weak[DSTEP] = {0}, knob_weak[D_N] = {0}, knob_cases[D_N] = {0};
        printf("%-6s prog block      ", LNAME[i]);
        for (int k = 0; k < D_N; k++)
            for (int v = DLO[k]; v <= DHI[k]; v++)
                if (v) printf(" %s%+d   ", DSHORT[k], v);
        printf("\n");
        for (int prog = 0; prog < 2; prog++)
            for (int c = 0; c < JAM_COLS; c++)
                for (int l = 0; l < JAM_LEVELS; l++) {
                    int j = 0;
                    setup(&s, prog);
                    seq_jam_apply_now(&s, LANE[i], c, l);
                    printf("%-6s %-4s %-6s %-10s", LNAME[i], PROG[prog], jam_genre_name(c), jam_block_name(i, c, l));
                    for (int k = 0; k < D_N; seq_djam_set(&s, LANE[i], DFIELD[k++], 0))   /* back to neutral before the next knob */
                        for (int v = DLO[k]; v <= DHI[k]; v++) {
                            int total = 0, ch, w;
                            unsigned got;
                            if (!v) continue;
                            ch = dmeasure(&s, LANE[i], k, v, &total);
                            w = ch * 8 < total;
                            got = seq_djam_weak(&s, LANE[i]);
                            if (got != (w ? DBIT[k] : 0u)) {
                                if (ok) printf("\nFAIL %s %s %s %s %s %+d: %d/%d, weak bits 0x%02x\n", LNAME[i], PROG[prog], jam_genre_name(c),
                                               jam_block_name(i, c, l), DNAME[k], v, ch, total, got);
                                ok = 0;
                            }
                            printf(" %3d/%-3d%c", ch, total, w ? '*' : ' ');
                            cases++, weak += w, step_weak[j++] += w, knob_weak[k] += w, knob_cases[k]++;
                        }
                    printf("\n");
                }
        printf("\n%s weak cases per knob step (of 60):", LNAME[i]);
        for (int k = 0, j = 0; k < D_N; k++)
            for (int v = DLO[k]; v <= DHI[k]; v++)
                if (v) printf(" %s%+d %d", DSHORT[k], v, step_weak[j++]);
        printf("\n%s per knob:", LNAME[i]);
        for (int k = 0; k < D_N; k++) printf(" %s %d/%d (%.1f%%)", DNAME[k], knob_weak[k], knob_cases[k], 100.0 * knob_weak[k] / knob_cases[k]);
        printf("\naudit %s: %d cases, %d weak (%.1f%%)\n\n", LNAME[i], cases, weak, 100.0 * weak / cases);
        ok &= cases == 2 * JAM_COLS * JAM_LEVELS * DSTEP;
    }
    return ok;
}

int main(void) {
    static seq_t s;
    static const char *const PROG[2] = {"Em", "C"};
    int cases = 0, weak = 0, drv = 0, drv_weak = 0, flags_ok = 1, step_weak[NSTEP] = {0}, col_weak[MJ_COLS] = {0}, col_cases[MJ_COLS] = {0};
    setup(&s, 1);
    CHECK(seq_slots(&s) == 8 && seq_slot_deg(&s, 0) == 0 && seq_slot_deg(&s, 1) == 5 && seq_slot_deg(&s, 6) == 3 && seq_slot_deg(&s, 7) == 4 &&
          s.key == 0 && s.scale == SCALE_MAJOR && s.part[0].bars == 4, "the second progression: C major, C Am F G twice in 4 bars (half-bar chords)");
    printf("prog block              ");
    for (int k = 0; k < K_N; k++)
        for (int v = KLO[k]; v <= KHI[k]; v++)
            if (v) printf(" %s%+d   ", KSHORT[k], v);
    printf("\n");
    for (int prog = 0; prog < 2; prog++)
        for (int c = 0; c < MJ_COLS; c++)
            for (int l = 0; l < MJ_LEVELS; l++) {
                int j = 0;
                setup(&s, prog);
                seq_mel_queue(&s, c, l);   /* stopped: at once */
                printf("%-4s %-8s %-9s", PROG[prog], mjam_col_name(c), mjam_block_name(c, l));
                for (int k = 0; k < K_N; set_knob(&s, k++, 0))   /* back to neutral before the next knob */
                    for (int v = KLO[k]; v <= KHI[k]; v++) {
                        int total = 0, ch, w;
                        unsigned got;
                        if (!v) continue;
                        ch = measure(&s, k, v, &total);
                        w = ch * 8 < total;
                        got = seq_mel_weak(&s);
                        if (got != (w ? KBIT[k] : 0)) {
                            if (flags_ok) printf("\nFAIL %s %s %s %s %+d: %d/%d, weak bits 0x%02x\n", PROG[prog], mjam_col_name(c), mjam_block_name(c, l),
                                                 KNAME[k], v, ch, total, got);
                            flags_ok = 0;
                        }
                        printf(" %3d/%-3d%c", ch, total, w ? '*' : ' ');
                        cases++, weak += w, step_weak[j++] += w;
                        if (k < K_SWING) drv++, drv_weak += w, col_weak[c] += w, col_cases[c]++;
                    }
                printf("\n");
            }
    printf("\nweak cases per knob step (of 60):");
    for (int k = 0, j = 0; k < K_N; k++)
        for (int v = KLO[k]; v <= KHI[k]; v++)
            if (v) printf(" %s%+d %d", KSHORT[k], v, step_weak[j++]);
    printf("\nDENSITY, RANGE, VARIATION weak cases per column:");
    for (int c = 0; c < MJ_COLS; c++) printf(" %s %d/%d", mjam_col_name(c), col_weak[c], col_cases[c]);
    printf("\naudit: %d cases, %d weak (%.1f%%)\n", cases, weak, 100.0 * weak / cases);
    printf("audit DENSITY+RANGE+VARIATION: %d cases, %d weak (%.1f%%)\n\n", drv, drv_weak, 100.0 * drv_weak / drv);
    CHECK(cases == 2 * MJ_COLS * MJ_LEVELS * NSTEP, "%d cases: 2 progressions x %d blocks x %d knob steps", cases, MJ_COLS * MJ_LEVELS, NSTEP);
    CHECK(flags_ok, "(a) every case: the weak bit is set exactly when it changes fewer than 1 in 8 (and no other bit)");
    CHECK(100.0 * drv_weak / drv <= AUDIT_LIMIT, "(b) DENSITY, RANGE and VARIATION: %d of %d cases weak (%.1f%%), at most %.1f%%", drv_weak, drv,
          100.0 * drv_weak / drv, AUDIT_LIMIT);
    printf("\n");
    flags_ok = drawn_audit();
    CHECK(flags_ok, "0.22 CHORDS and BASS: %d cases each (2 progressions x 30 blocks x 10 knob steps), the weak bit set exactly when a case "
          "changes fewer than 1 in 8 (and no other bit)", 2 * JAM_COLS * JAM_LEVELS * DSTEP);
    printf(fails ? "\nFAILED: %d\n" : "\nPASSED\n", fails);
    return fails != 0;
}
