#!/usr/bin/env python3
"""UCI protocol, legal PV/bestmove, stop, mate and clock-budget smoke tests.
Requires python-chess only. Run from repository root.
"""
import sys, time
import chess, chess.engine
cmd = sys.argv[1] if len(sys.argv)>1 else './carnivorus'
with chess.engine.SimpleEngine.popen_uci(cmd) as e:
    e.configure({'Hash': 16, 'Move Overhead': 0})
    cases = [chess.Board(), chess.Board('7k/5K2/6Q1/8/8/8/8/8 w - - 0 1'),
             chess.Board('8/8/8/8/1k6/8/1p6/1K6 w - - 0 1')]
    for b in cases:
        info=e.analyse(b,chess.engine.Limit(nodes=15000))
        c=b.copy()
        for m in info.get('pv',[]):
            assert m in c.legal_moves, (c.fen(), m)
            c.push(m)
        r=e.play(b,chess.engine.Limit(time=.05))
        assert r.move in b.legal_moves
    b=chess.Board()
    t=time.monotonic()
    with e.analysis(b) as analysis:
        time.sleep(.05)
        analysis.stop()
        result=analysis.wait()
        assert result.move in b.legal_moves
    assert time.monotonic()-t < 2, 'stop took too long'
    for limit in [chess.engine.Limit(time=.1), chess.engine.Limit(white_clock=.3,black_clock=.3,white_inc=.01,black_inc=.01)]:
        t=time.monotonic(); r=e.play(b,limit)
        assert r.move in b.legal_moves
        assert time.monotonic()-t < .6, 'large time overshoot'
print('ok UCI legal PV/bestmove, stop, fixed movetime, short clock')
