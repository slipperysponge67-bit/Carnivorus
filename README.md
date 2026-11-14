# Carnivorus

A UCI chess engine written from scratch in C. Single-threaded, hand-crafted evaluation, no NNUE, no external code.

## Build and run

    make            # gcc -O2 -Wall -Wextra, builds with no warnings
    ./carnivorus    # speaks UCI on stdin/stdout; use with any UCI GUI or cutechess-cli
    make test       # perft checks against known node counts

Options: `Hash` (MB, default 64), `Move Overhead` (ms, default 30), `Clear Hash`.
Extras: `go perft N` after a `position` command.

## Design

- Board: 64-bit bitboards plus a mailbox, Zobrist hashing, copy-make.
- Attacks: magic bitboards for sliders (magic numbers found at startup), lookup tables for knights, kings, pawns.
- Move generation: pseudo-legal, filtered by a king-safety check in make-move.
- Search: negamax with PVS, iterative deepening, aspiration windows, quiescence search with SEE and delta pruning, check extensions, internal iterative reduction.
- Pruning: transposition table, reverse futility, null move, futility, late move pruning, late move reductions, SEE pruning of bad captures.
- Ordering: TT move, SEE-sorted captures, killers, countermove, history.
- Evaluation (tapered mid/endgame): material, formula-built piece-square tables, safe mobility, bishop pair, pawn structure (isolated, doubled, supported), passed pawns with king proximity, rooks on open files and the 7th, pawn shield and king-zone attackers, drawish-ending scaling.
- Time management: soft and hard limits from clock, increment and moves to go; polls stdin during search so `stop` works.

## Testing

- `tests/perft.sh`: startpos perft 5 (4865609) and 6 (119060324), plus five standard test positions.
- `tests/match.py`: plays games against a reference UCI engine with python-chess.
- `tests/tactics.py`: mate and only-move tactic check against a reference engine.

## Version 1.1

The strength pass fixes exchange evaluation and tactical search, adds an evaluation
cache, and improves time use. In a 128-game independent paired-opening test at
35 ms/move, it scored +91 =25 -12 against 1.0 (about +250 relative Elo).
Fresh Stockfish 14.1 limited-strength checks suggest roughly 2250 on that test
scale, not an official rating. See [STRENGTH.md](STRENGTH.md) for methods,
uncertainty, reproduction commands and the distinction between the relative gain
and the noisy old absolute estimate. Full final game records are in `MATCHES.pgn`, with summaries in `MATCH_RESULTS.json`.

`make test` also runs exchange/search regressions. With `python-chess` installed,
run `python3 tests/uci_smoke.py` for protocol checks and
`python3 tests/paired_match.py --help` for paired-opening matches.
