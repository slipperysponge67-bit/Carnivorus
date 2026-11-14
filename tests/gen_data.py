#!/usr/bin/env python3
"""Self-play data generation for Texel tuning of Carnivorus's own eval.
Positions come from Carnivorus self-play games only (original code, no
external labels); Stockfish is used once, only to diversify openings.
Output: '<result> <fen>' lines, result 1.0/0.5/0.0 from White's view.
Chunked: 'openings' mode builds the opening file in slices; 'play' mode
plays a slice of games and appends positions."""
import argparse, concurrent.futures, json, os, random, sys
import chess, chess.engine

BASE = ['e2e4 e7e5 g1f3 b8c6', 'd2d4 d7d5 c2c4 e7e6', 'e2e4 c7c5 g1f3 d7d6', 'd2d4 g8f6 c2c4 g7g6', 'e2e4 e7e6 d2d4 d7d5', 'c2c4 e7e5 b1c3 g8f6', 'g1f3 d7d5 g2g3 g8f6', 'e2e4 c7c6 d2d4 d7d5', 'd2d4 d7d5 g1f3 g8f6 c1f4', 'e2e4 g7g6 d2d4 f8g7', 'e2e4 e7e5 g1f3 g8f6 f3e5 d7d6', 'd2d4 f7f5 g2g3 g8f6']

def gen_opening(sf, i, seed):
    rng = random.Random(seed * 1000003 + i)
    b = chess.Board()
    for u in BASE[i % len(BASE)].split(): b.push_uci(u)
    with chess.engine.SimpleEngine.popen_uci(sf) as e:
        e.configure({'Hash':16,'Threads':1,'Use NNUE':False})
        for _ in range(4 + 2*(i//len(BASE) % 4)):
            lines = e.analyse(b, chess.engine.Limit(nodes=1200), multipv=3)
            top = lines[0]['score'].pov(b.turn).score(mate_score=10000)
            ch = [x for x in lines if top - x['score'].pov(b.turn).score(mate_score=10000) < 70]
            b.push(rng.choice(ch)['pv'][0])
    return ' '.join(m.uci() for m in b.move_stack)

def play(job):
    idx, opening, eng, nodes = job
    board = chess.Board()
    for m in opening.split(): board.push_uci(m)
    positions = []
    try:
        with chess.engine.SimpleEngine.popen_uci(eng) as e:
            e.configure({'Hash':16,'Threads':1,'Move Overhead':0})
            last_tactical = False
            while not board.is_game_over(claim_draw=True) and board.ply() < 400:
                fen = board.fen()
                quiet = board.ply() >= 16 and not board.is_check() and not last_tactical
                r = e.play(board, chess.engine.Limit(nodes=nodes))
                if r.move not in board.legal_moves: return []
                if quiet: positions.append(fen)
                last_tactical = board.is_capture(r.move) or r.move.promotion is not None
                board.push(r.move)
        oc = board.outcome(claim_draw=True)
        result = oc.result() if oc else '1/2-1/2'
    except Exception:
        return []
    y = {'1-0': 1.0, '0-1': 0.0}.get(result, 0.5)
    return [(f, y) for f in positions]

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=['openings', 'play'])
    p.add_argument('--engine', default='./carnivorus')
    p.add_argument('--sf', default='/tmp/sf/usr/games/stockfish')
    p.add_argument('--start', type=int, default=0)
    p.add_argument('--count', type=int, required=True)
    p.add_argument('--nodes', type=int, default=2000)
    p.add_argument('--seed', type=int, default=20261114)
    p.add_argument('--openings', default='tests/tune_openings.json')
    p.add_argument('--out')
    p.add_argument('--workers', type=int, default=2)
    args = p.parse_args()
    if args.mode == 'openings':
        ops = json.load(open(args.openings)) if os.path.exists(args.openings) else []
        while len(ops) < args.start: ops.append(None)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
            for j, op in zip(range(args.count), pool.map(lambda i: gen_opening(args.sf, i, args.seed),
                                                        range(args.start, args.start + args.count))):
                ops.append(op)
        assert all(ops), 'gap in openings'
        json.dump(ops, open(args.openings, 'w'), indent=1)
        print(f'openings: {len(ops)} total')
    else:
        ops = json.load(open(args.openings))
        jobs = [(i, ops[i], os.path.abspath(args.engine), args.nodes) for i in range(args.start, args.start + args.count)]
        total = 0
        with open(args.out, 'a') as f, concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
            for rows in pool.map(play, jobs):
                for fen, y in rows:
                    f.write(f'{y} {fen}\n'); total += 1
        print(f'play slice {args.start}..{args.start+args.count}: +{total} positions')
