# Carnivorus 1.3 strength pass (2026-11-15)

Engine baseline for this pass: `81faa88` (1.2, including singular extensions).
Original 1.1 baseline: `d4c44365fe236e56299a532321214af5525dd650`.
Engine candidate: 1.3 (this branch).
All engine changes are original C. No external engine code or weights were copied.
No spending was added. NNUE, book and tablebase dependencies: none.

## Changes retained

Four search refinements, each original, each screened against the 1.2 build with
224-game paired-opening matches at nodes=8000 per move before integration:

- Continuation history: a (previous piece, target square) -> (piece, target
  square) quiet-move history table, used in move ordering, in the LMR reduction
  and updated with the same gravity formula as main history on quiet fail-highs.
  Screen: +17 Elo [-21, +55]; extended to 448 pooled games: +15 [-11, +42].
- Cutnode-aware LMR: expected cut nodes are tracked through the tree and late
  quiet moves at expected cut nodes are reduced one ply more.
  Screen: +25 Elo [-12, +62].
- SEE pruning for quiet moves: quiet moves losing material by static exchange
  evaluation (see < -25 * depth^2) are pruned at depth <= 8.
  Screen: +22 Elo [-16, +60].
- LMR for late bad captures: non-improving captures ordered below the quiet
  threshold (negative-SEE captures) at depth >= 3 are reduced one ply, with the
  usual re-search on fail high.
  Screen: +23 Elo [-11, +58].

Integrated build versus 1.2: +29 Elo [-4, +62] over 256 games at nodes=8000,
and +33 Elo [-11, +77] over 128 games at 35 ms/move. The direction is
consistent across both conditions.

- Version string is 1.3.

Discarded after measurement this pass: razoring at depth <= 3 (-14 Elo),
a fourth Texel tuning round (594k self-play positions, validation loss
0.090895 -> 0.090699, but -19 Elo over 224 games at nodes=8000),
history-threshold quiet pruning (threshold unreachable in practice; the match
was a perfect mirror, 86/52/86), 4-way transposition-table buckets
(+2 Elo, neutral). The round-4 tuning data and tuned weights are kept in the
repository for the record even though the weights were not adopted.

## Validation

GCC `-O2 -Wall -Wextra -std=gnu11 -pthread`: no warnings.
`make test`: all perft positions pass, plus the exchange/cache/draw/TT
white-box regressions. `python3 tests/uci_smoke.py`: legal PV and bestmove,
stop, movetime and short-clock checks pass.

## 1.3 versus 1.1 matches

Both engines: 32 MiB hash, one thread, Move Overhead 0, same build flags.
Two games concurrently on a two-vCPU host, alternating colours, saved openings
from `tests/validation_openings.json` (seed 20261115). Node-limited games are
deterministic per opening, so the 256-opening set caps node-match size;
time-control matches are not capped this way.

| Match | Games | 1.3 W/D/L | 1.3 score | Implied Elo difference |
| --- | ---: | --- | ---: | ---: |
| nodes=8000 per move | 256 | 113/64/79 | 56.25% | +46 |
| 35 ms/move | 128 | 56/42/30 | 60.16% | +72 |

Approximate 95% intervals over opening-pair scores (descriptive, not SPRT):
+9 to +85 Elo (nodes) and +30 to +115 Elo (time control).

An honest note on additivity: 1.2 measured +79 Elo over 1.1 on this exact
node-match setup, and 1.3 beats 1.2 by about +30 in direct matches, yet the
1.3-versus-1.1 node margin comes out smaller (+46). Self-play margins against
a fixed opponent are not additive; openings interact with playing style, and
paired-opening Elo between two engine pairs does not compose like a rating
list. The direct 1.3-versus-1.2 matches above are the reliable evidence for
this pass; the 1.1 matches anchor the longer-range level.

## Approximate absolute rating

The 1.2 calibration against Stockfish limited-strength settings (about 2300 on
that scale, practical range 2150-2450) carries over: 1.3's measured gain over
1.2 is about +30 Elo, so the same rough range applies, shifted up slightly.
The caveat from 1.2 stands: this is not FIDE, CCRL or CEGT Elo, ultra-fast
games against a weakened Stockfish are not a calibrated scale, and the direct
matches are the meaningful evidence.

Final validation PGNs: `MATCHES-1.3-nodes.pgn`, `MATCHES-1.3-tc.pgn`.
Summaries are in `MATCH_RESULTS.json`; per-game records are under `work/`.

## Post-1.3: follow-up continuation history (1.3.1)

One more measured win landed after the 1.3 delivery: a 2-ply follow-up
continuation history table (same shape and update rule as the 1-ply
continuation history kept in 1.3), used in quiet-move ordering, LMR reduction
and fail-high history updates. All original code.

Screened against the shipped 1.3 build, paired openings, one thread:

| Match | Games | 1.3.1 W/D/L | 1.3.1 score | Implied Elo difference |
| --- | ---: | --- | ---: | ---: |
| nodes=8000 per move | 224 | 91/64/69 | 54.91% | +34 |
| 35 ms/move | 128 | 45/46/37 | 53.13% | +22 |

Approximate 95% intervals: -11 to +81 (nodes) and -17 to +61 (time control).
Both conditions positive and consistent, the same evidence bar the kept 1.3
changes met. Per-game records: `work/screen-ch2ply.jsonl` (deduplicated to
unique openings 0-223 after a killed resume repeated 12 games) and
`work/screen-ch2tc.jsonl`; summaries in `MATCH_RESULTS.json`.
