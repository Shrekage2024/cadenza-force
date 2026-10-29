/* Offline test of the Bass's chord-tone rule in src/seq_core.c (static: the source is included). Run by tests/run_tests.sh under
 * ASan/UBSan. Exit 1 on failure. */
#include "../src/seq_core.c"

static int fails;

#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static void emit(void *ctx, int status, int note, int vel) { (void)ctx, (void)status, (void)note, (void)vel; }

/* A last onset on the 7th of slot 0 (the Walk's APPROACH keeps a chord tone and moves a passing tone) */
static int approach_moves(seq_t *s, int colour) {
    seq_part_t *p = &s->part[0];
    seq_step_t st[SEQ_LANE_STEPS];
    memset(st, 0, sizeof st);
    st[8].on = 1;
    st[8].tone = TONE_7;
    p->colour[0] = (unsigned char)colour;
    xf_approach(s, p, st, p->bars * SEQ_STEPS);
    return st[8].abs != 0;
}

int main(void) {
    static seq_t s;
    static const struct { int colour; const char *name; int seventh; } T[COL_COUNT] = {
        {COL_TRIAD, "Triad", 0}, {COL_7TH, "7th", 1}, {COL_SUS2, "sus2", 0}, {COL_SUS4, "sus4", 0}, {COL_ADD9, "add9", 0},
        {COL_POWER, "Power", 0}, {COL_9TH, "9th", 1}, {COL_11TH, "11th", 1}, {COL_13TH, "13th", 1}, {COL_6TH, "6th", 1}};
    seq_init(&s, emit, NULL);
    for (int c = 0; c < COL_COUNT; c++) {
        s.part[0].colour[0] = (unsigned char)T[c].colour;
        CHECK(chord_tone(&s, &s.part[0], 0, TONE_7) == T[c].seventh && approach_moves(&s, T[c].colour) == !T[c].seventh,
              "%s chord: the 7th is %s to the Bass (chord_tone, APPROACH)", T[c].name, T[c].seventh ? "a chord tone" : "a passing tone");
    }
    printf(fails ? "\nFAILED: %d\n" : "\nPASSED\n", fails);
    return fails != 0;
}
