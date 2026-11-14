/* White-box regression tests for exchange backup and search invariants.
 * Written for Carnivorus, with hand-constructed positions, no external code. */
#define main engine_main
#include "../carnivorus.c"
#undef main
#include <assert.h>

static Pos position(const char *fen) {
    Pos p;
    assert(set_fen(&p, fen));
    return p;
}
static void exchange(const char *fen, const char *uci, int want) {
    Pos p = position(fen);
    Move m = parse_move(&p, uci);
    assert(m);
    int got = see(&p, m);
    if (got != want) fprintf(stderr, "SEE %s: got %d expected %d\n", uci, got, want);
    assert(got == want);
}
int main(void) {
    init_tables(); init_eval(); tt_resize(1);
    exchange("7k/8/8/3p4/8/8/8/K2Q4 w - - 0 1", "d1d5", 100);
    exchange("7k/8/2p5/3p4/8/8/8/K2Q4 w - - 0 1", "d1d5", -800);
    exchange("7k/8/2p5/3p4/8/8/6B1/K2Q4 w - - 0 1", "d1d5", -700);
    exchange("7k/8/8/3p4/4P3/8/8/K7 w - - 0 1", "e4d5", 100);
    exchange("7k/8/8/3pP3/8/8/8/K7 w - d6 0 1", "e5d6", 100);
    exchange("7k/P7/8/8/8/8/8/K7 w - - 0 1", "a7a8q", 800);
    /* A king on e6 cannot recapture on d5, which the bishop on g2 guards. */
    exchange("8/8/4k3/3p4/4P3/8/6B1/K7 w - - 0 1", "e4d5", 100);
    for (int i = 0; i < 1000; i++) {
        Pos p = position(i & 1 ? "7k/8/2p5/3p4/8/8/8/K2Q4 b - - 0 1" :
                                 "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        assert(eval(&p) == eval_uncached(&p));
    }
    Pos p = position("7k/8/8/8/8/8/8/KQ6 w - - 100 1");
    nodes = 0; stopped = 0; g_hlen = 0;
    assert(qsearch(&p, -INF, INF, 0, 0) == 0);
    for (int s = -MATE; s <= MATE; s += 113)
        for (int ply = 0; ply < MAXPLY; ply++)
            assert(score_from_tt(score_to_tt(s, ply), ply) == s);
    free(tt);
    puts("ok exchange evaluation, cache, draw handling, mate-score TT roundtrips");
    return 0;
}
