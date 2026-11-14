#!/usr/bin/env python3
"""Play games between Carnivorus and a reference UCI engine using python-chess.
usage: match.py REF_CMD GAMES MOVETIME_S [UCI_Elo]   (REF can be carnivorus itself for self-play)
Reports W/D/L, crashes and illegal moves, plus the Elo difference implied by the score."""
import sys, math, chess, chess.engine, chess.pgn, random, concurrent.futures as cf

OPENINGS = ["e2e4 e7e5 g1f3 b8c6", "d2d4 d7d5 c2c4 e7e6", "e2e4 c7c5 g1f3 d7d6", "d2d4 g8f6 c2c4 g7g6",
            "e2e4 e7e6 d2d4 d7d5", "c2c4 e7e5 b1c3 g8f6", "g1f3 d7d5 g2g3 g8f6", "e2e4 c7c6 d2d4 d7d5",
            "d2d4 d7d5 g1f3 g8f6 c1f4", "e2e4 g7g6 d2d4 f8g7", "e2e4 e7e5 g1f3 g8f6 f3e5 d7d6", "d2d4 f7f5 g2g3 g8f6"]

def play(args):
    ref_cmd, ref_elo, mt, opening, car_white, ours_cmd = args
    board = chess.Board()
    for mv in opening.split(): board.push_uci(mv)
    ours = chess.engine.SimpleEngine.popen_uci(ours_cmd)
    ref = chess.engine.SimpleEngine.popen_uci(ref_cmd)
    if ref_elo:
        ref.configure({"UCI_LimitStrength": True, "UCI_Elo": ref_elo})
        try: ref.configure({"Use NNUE": False})
        except Exception: pass
    issue = None
    try:
        while not board.is_game_over(claim_draw=True) and board.ply() < 400:
            eng = ours if (board.turn == chess.WHITE) == car_white else ref
            try:
                r = eng.play(board, chess.engine.Limit(time=mt))
            except Exception as e:
                issue = f"{'carnivorus' if eng is ours else 'ref'} error: {e!r} at {board.fen()}"
                break
            board.push(r.move)
    finally:
        for e in (ours, ref):
            try: e.quit()
            except Exception: pass
    if issue and "carnivorus" in issue: res = "crash"
    elif issue: res = "refcrash"
    else:
        o = board.outcome(claim_draw=True)
        res = o.result() if o else "1/2-1/2"
    if res in ("crash", "refcrash"): score = None
    elif res == "1/2-1/2": score = 0.5
    else: score = 1.0 if (res == "1-0") == car_white else 0.0
    return score, issue, board.ply()

if __name__ == "__main__":
    ref = sys.argv[1]; games = int(sys.argv[2]); mt = float(sys.argv[3])
    elo = int(sys.argv[4]) if len(sys.argv) > 4 else None
    ours = "./carnivorus"
    jobs = []
    for g in range(games):
        jobs.append((ref, elo, mt, OPENINGS[(g // 2) % len(OPENINGS)], g % 2 == 0, ours))
    w = d = l = 0; issues = []
    with cf.ThreadPoolExecutor(max_workers=2) as ex:
        for score, issue, ply in ex.map(play, jobs):
            if issue: issues.append(issue)
            if score == 1: w += 1
            elif score == 0.5: d += 1
            elif score == 0: l += 1
            print(f"game done: score={score} plies={ply}", flush=True)
    n = w + d + l
    s = (w + d / 2) / n if n else 0
    sc = min(max(s, 0.01), 0.99)
    diff = -400 * math.log10(1 / sc - 1)
    print(f"RESULT vs {ref} elo={elo} movetime={mt}: +{w} ={d} -{l}  score={s:.3f}  elo_diff={diff:+.0f}  issues={len(issues)}")
    for i in issues: print("ISSUE", i)
