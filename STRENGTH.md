# Carnivorus 1.1 strength pass (2026-11-14)

Engine baseline: `9dbf8496ee5a9dd529c80da29f5c1bebaeecbaed` (1.0).
Engine candidate: `d4c44365fe236e56299a532321214af5525dd650` (1.1).
All engine changes are original C. No external engine code or weights were copied.
Stockfish was used only as a separate test opponent and opening generator.
No spending, NNUE, SMP, book or tablebase dependency was added.

## Changes retained

- Fixed an off-by-one error in static exchange evaluation: the final recapture
  was omitted when minimax values were backed up. This undervalued the danger of
  losing captures. Added a defended-square check for king recaptures.
- Quiescence now detects repetitions, the 50-move limit and insufficient material;
  it no longer evaluates an in-check node simply because six qsearch plies passed.
- Promotions are not discarded by ordinary losing-capture SEE pruning.
- Checking moves survive forward futility, late-move and SEE pruning.
- Quiescence does not overwrite a deeper same-position search entry.
- Added a 1 MiB full-position evaluation cache, separate from search bounds.
- Fixed-time search can use its actual budget rather than stopping at 60% of it.
  Clock-managed search adjusts its soft target to best-move stability and score
  drops, while retaining a hard cap. UCI options are unchanged; version is 1.1.

Untuned knight-outpost/bad-bishop terms and larger history bonuses were tried,
but neither improved its 64-game screening, so neither is included. There is no
claim that every retained change independently adds Elo: matches test the bundle.

## Validation

GCC `-O2 -Wall -Wextra -std=gnu11`: no warnings.
`make test`: all seven existing perft positions pass, including initial depth 6
(119060324), plus new white-box exchange/cache/draw/TT regressions.
`python3 tests/uci_smoke.py`: legal PV and bestmove, stop, movetime and short-clock
checks pass. White-box tests also pass AddressSanitizer and UndefinedBehaviorSanitizer.
The existing random-position tactics generator did not finish an 8-position run
within a bounded two-minute test; no new tactics percentage is claimed.

## Independent old-versus-new matches

Both engines: 32 MiB hash, one thread, Move Overhead 0, same `-O2` build flags.
Two games run concurrently on a two-vCPU host. Each opening is played with both
colours. Games end by python-chess draw-claim rules, checkmate or 500 plies; no
evaluation-based adjudication. Node polling granularity is 2048, so very short
budgets can overshoot slightly. Neither binary changed during final validation.

| Match | Games | New W/D/L | New score | Implied Elo difference |
| --- | ---: | --- | ---: | ---: |
| Independent validation, 35 ms/move | 128 | 91/25/12 | 80.86% | +250 |
| Slower check, 100 ms/move | 16 | 13/3/0 | 90.63% | +394 |

For the 128-game result, an approximate 95% interval calculated over the 64
opening-pair scores is +195 to +320 Elo. This is descriptive, not a formal SPRT
or a rating-list result. The slower sample is too small to use as the headline.
The direction of improvement is clear in these tests; its size can change with
opponents, openings, hardware and time control.

Final openings were generated with seed 20261115 after selecting the candidate,
and are saved in `tests/validation_openings.json`. Screening used seed 20261114.
The earlier 48-game screening (+36 =10 -2) is not included in the 128-game result.
Final validation PGNs are in `MATCHES-1.pgn` through `MATCHES-8.pgn`; final and screening summaries
are in `MATCH_RESULTS.json`.

## Fresh approximate absolute rating

Stockfish 14.1 Ubuntu binary, one thread, 32 MiB hash, UCI_LimitStrength true,
Use NNUE false, 100 ms/move, alternating colours and paired saved openings:

| Stockfish UCI_Elo | Games | Carnivorus W/D/L | Score | Setting plus implied difference |
| --- | ---: | --- | ---: | ---: |
| 2200 | 32 | 13/6/13 | 50.00% | 2200 |
| 2400 | 48 | 10/14/24 | 35.42% | 2296 |

**Approximate rating: around 2250 on this limited-strength test scale**, with a
rough practical uncertainty range of 2100-2450. This is not FIDE, CCRL or CEGT Elo.
These are ultra-fast games, and Stockfish's setting is not an absolute calibrated
rating at this time control. Small samples and Stockfish's stochastic weakening
also matter. The previous 1.0 estimate of 2200 was based on much smaller samples;
it is not precise enough to add the self-play +250 and claim 2450. The fresh
calibration and the direct relative gain answer different questions.

## Reproduce

Requires Python 3 and `pip install python-chess` for match/protocol scripts only;
the engine itself has no Python dependency.

```sh
make test
python3 tests/uci_smoke.py
# Compile baseline without replacing main's working tree:
git show 9dbf849:carnivorus.c > /tmp/carnivorus-v1.c
cc -O2 -Wall -Wextra -std=gnu11 /tmp/carnivorus-v1.c -lm -o /tmp/carnivorus-v1
mkdir -p results
python3 tests/paired_match.py ./carnivorus /tmp/carnivorus-v1 --games 128 --time .035 --openings tests/validation_openings.json --out results/retest
python3 tests/paired_match.py ./carnivorus /path/to/stockfish --games 48 --time .1 --elo 2400 --openings tests/validation_openings.json --out results/sf-retest
```

A saved openings file avoids needing Stockfish to generate one. Elo confidence
intervals in the runner use a normal approximation on paired scores and should
not be overinterpreted for small or nearly perfect samples.

## Remaining headroom

No automated evaluation tuning, singular extensions, multithreading or external
rating-list testing was completed. No finite pass can make an engine maximally
strong. Next work should use longer matches, more opponents and statistically
controlled tests rather than add unmeasured features.
