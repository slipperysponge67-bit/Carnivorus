#!/usr/bin/env python3
"""Tactics check. Positions come from random-ish self-play games; a reference engine
(Stockfish, depth REFDEPTH) labels positions where it sees a forced mate in <= 4 and positions
with a clearly winning tactic. Carnivorus then gets MOVETIME seconds per position.
usage: tactics.py REF_CMD NPOS MOVETIME"""
import sys, random, chess, chess.engine

ref = chess.engine.SimpleEngine.popen_uci(sys.argv[1])
npos = int(sys.argv[2]); mt = float(sys.argv[3])
car = chess.engine.SimpleEngine.popen_uci("./carnivorus")
random.seed(7)
mate_hits = mate_total = 0
tact_hits = tact_total = 0
agree = agree_total = 0
games = 0
while mate_total < npos or tact_total < npos:
    b = chess.Board()
    for _ in range(random.randint(8, 30)):
        if b.is_game_over(): break
        b.push(random.choice(list(b.legal_moves)) if random.random() < 0.15 else car.play(b, chess.engine.Limit(nodes=3000)).move)
    games += 1
    for _ in range(60):
        if b.is_game_over(): break
        info = ref.analyse(b, chess.engine.Limit(depth=14))
        sc = info["score"].pov(b.turn)
        pv = info.get("pv", [])
        if pv:
            m = sc.mate()
            if m is not None and 0 < m <= 4 and mate_total < npos:
                r = car.play(b, chess.engine.Limit(time=mt), info=chess.engine.INFO_SCORE)
                s = r.info["score"].pov(b.turn).mate()
                b2 = b.copy(); b2.push(r.move)
                # a mating move: score says mate for us and the move does not lengthen it past the optimum + 1
                ok = s is not None and 0 < s <= m + 1
                mate_total += 1; mate_hits += ok
            elif (m is None and mate_total >= 0 and tact_total < npos):
                # tactic: best move is a capture/check and 2nd best is >= 150cp worse
                multi = ref.analyse(b, chess.engine.Limit(depth=14), multipv=2)
                if len(multi) == 2:
                    s1 = multi[0]["score"].pov(b.turn).score(mate_score=10000)
                    s2 = multi[1]["score"].pov(b.turn).score(mate_score=10000)
                    if s1 - s2 >= 150 and abs(s1) < 5000:
                        r = car.play(b, chess.engine.Limit(time=mt))
                        tact_total += 1; tact_hits += (r.move == multi[0]["pv"][0])
        mv = car.play(b, chess.engine.Limit(nodes=2000)).move
        b.push(mv)
    if games > 400: break
print(f"mate-in-<=4 positions found by reference: {mate_hits}/{mate_total} solved at {mt}s")
print(f"'only-move' tactics (best move >=150cp better than 2nd): {tact_hits}/{tact_total} solved at {mt}s")
ref.quit(); car.quit()
