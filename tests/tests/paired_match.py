#!/usr/bin/env python3
"""Reproducible paired-opening UCI matches. No evaluation adjudication.
Use --nodes for screening or --time for an equal wall-time confirmation.
Games alternate colours on the same opening; all results and PGNs are saved.
"""
import argparse, concurrent.futures, json, math, os, random, time
import chess, chess.engine, chess.pgn
BASE = ['e2e4 e7e5 g1f3 b8c6', 'd2d4 d7d5 c2c4 e7e6', 'e2e4 c7c5 g1f3 d7d6', 'd2d4 g8f6 c2c4 g7g6', 'e2e4 e7e6 d2d4 d7d5', 'c2c4 e7e5 b1c3 g8f6', 'g1f3 d7d5 g2g3 g8f6', 'e2e4 c7c6 d2d4 d7d5', 'd2d4 d7d5 g1f3 g8f6 c1f4', 'e2e4 g7g6 d2d4 f8g7', 'e2e4 e7e5 g1f3 g8f6 f3e5 d7d6', 'd2d4 f7f5 g2g3 g8f6']
def openings(path, ref):
    if os.path.exists(path): return json.load(open(path))
    rng=random.Random(20261115)
    out=[]
    with chess.engine.SimpleEngine.popen_uci(ref) as e:
        e.configure({'Hash':16,'Threads':1,'Use NNUE':False})
        for i in range(256):
            b=chess.Board()
            for u in BASE[i%len(BASE)].split(): b.push_uci(u)
            for _ in range(4 + 2*(i//len(BASE)%3)):
                lines=e.analyse(b,chess.engine.Limit(nodes=1500),multipv=3)
                top=lines[0]['score'].pov(b.turn).score(mate_score=10000)
                choices=[x for x in lines if top-x['score'].pov(b.turn).score(mate_score=10000)<65]
                b.push(rng.choice(choices)['pv'][0])
            out.append(' '.join(m.uci() for m in b.move_stack))
    json.dump(out,open(path,'w'),indent=2)
    return out

def play(job):
    idx, opening, a, b, nodes, mt, elo = job
    board=chess.Board()
    for m in opening.split(): board.push_uci(m)
    engines=[]; err=None; aw=idx%2==0
    try:
        for cmd in (a,b):
            e=chess.engine.SimpleEngine.popen_uci(cmd)
            e.configure({'Hash':32,'Threads':1})
            if 'Move Overhead' in e.options: e.configure({'Move Overhead':0})
            engines.append(e)
        if elo: engines[1].configure({'UCI_LimitStrength':True,'UCI_Elo':elo,'Use NNUE':False})
        while not board.is_game_over(claim_draw=True) and board.ply()<500:
            k=0 if board.turn==aw else 1
            try:
                r=engines[k].play(board,chess.engine.Limit(nodes=nodes) if nodes else chess.engine.Limit(time=mt))
                if r.move not in board.legal_moves: raise RuntimeError('illegal move: '+str(r.move))
            except Exception as ex:
                err=f'engine {k}: {ex!r} at {board.fen()}'
                raise
            board.push(r.move)
        outcome=board.outcome(claim_draw=True)
        result=outcome.result() if outcome else '1/2-1/2'
        score=.5 if result=='1/2-1/2' else float((result=='1-0')==aw)
    except Exception as ex:
        err=err or repr(ex); score=None; result='*'
    finally:
        for e in engines:
            try: e.quit()
            except Exception: pass
    pgn=chess.pgn.Game.from_board(board)
    pgn.headers.update({'White':os.path.basename(a if aw else b),'Black':os.path.basename(b if aw else a),'Result':result,'Round':str(idx+1)})
    return {'game':idx,'score':score,'plies':board.ply(),'error':err,'pgn':str(pgn),'termination':str(board.outcome(claim_draw=True))}

def summary(rows):
    scores=[r['score'] for r in rows if r['score'] is not None]
    n=len(scores); w=scores.count(1); d=scores.count(.5); l=scores.count(0)
    s=sum(scores)/n if n else .5
    elo=lambda x:400*math.log10(max(.0001,min(.9999,x))/(1-max(.0001,min(.9999,x))))
    # Descriptive normal interval over opening-pair scores, not independent plies.
    pairs={}
    for r in rows:
        if r['score'] is not None: pairs.setdefault(r['game']//2,[]).append(r['score'])
    ps=[sum(x)/2 for x in pairs.values() if len(x)==2]
    se=(sum((x-sum(ps)/len(ps))**2 for x in ps)/(len(ps)-1)/len(ps))**.5 if len(ps)>1 else .5
    return {'n':n,'wins':w,'draws':d,'losses':l,'score':s,'elo_diff':elo(s),'approx_95_elo':[elo(s-1.96*se),elo(s+1.96*se)],'errors':[r['error'] for r in rows if r['error']]}
if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('a'); p.add_argument('b'); p.add_argument('--games',type=int,default=16); p.add_argument('--start',type=int,default=0); p.add_argument('--nodes',type=int); p.add_argument('--time',type=float,default=.08); p.add_argument('--elo',type=int); p.add_argument('--out',required=True); p.add_argument('--workers',type=int,default=2); p.add_argument('--openings',default='tests/openings.json'); p.add_argument('--sf',default='/tmp/sf/usr/games/stockfish'); args=p.parse_args()
    ops=openings(args.openings,args.sf)
    jobs=[(i,ops[(i//2)%len(ops)],os.path.abspath(args.a),os.path.abspath(args.b),args.nodes,args.time,args.elo) for i in range(args.start,args.start+args.games)]
    rows=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        for r in pool.map(play,jobs):
            rows.append(r)
            with open(args.out+'.jsonl','a') as f: f.write(json.dumps(r)+'\n')
            print(json.dumps({k:r[k] for k in ('game','score','plies','error')}),flush=True)
    allrows=[json.loads(x) for x in open(args.out+'.jsonl')]
    result=summary(allrows); print(json.dumps(result),flush=True)
    json.dump(result,open(args.out+'.summary.json','w'),indent=2)
    with open(args.out+'.pgn','w') as f: f.write('\n\n'.join(r['pgn'] for r in allrows)+'\n')
