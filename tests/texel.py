#!/usr/bin/env python3
"""Texel tuning for Carnivorus's own handcrafted eval (original code).
Positions+results come from Carnivorus self-play (tests/gen_data.py);
features come from the engine's own feature dump (tests/featdump.c).
Minimizes mean (result - sigmoid(pred/K))^2 with Adam over the linear
weights only; fixed nonlinear terms (king storm, drawish scale, tempo)
are added per position exactly as the engine computes them.
"""
import argparse, json, subprocess, sys
import numpy as np

def engine_weights(cmd):
    p = subprocess.Popen([cmd], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    p.stdin.write('weights\nquit\n'); p.stdin.flush()
    w = {}
    for line in p.stdout:
        parts = line.split()
        if parts and parts[0] in ('w_mg', 'w_eg'):
            w[parts[0]] = np.array([int(x) for x in parts[1:]], dtype=np.float64)
    p.wait()
    assert len(w.get('w_mg', [])) == 512 and len(w.get('w_eg', [])) == 512, 'weights dump failed'
    return w['w_mg'], w['w_eg']

def load_features(path):
    labels, phase, ks, draw, tempo = [], [], [], [], []
    idx_all, val_all, off = [], [], [0]
    for line in open(path):
        parts = line.rstrip('\n').split('\t')
        if len(parts) < 7: continue
        labels.append(float(parts[0]))
        phase.append(int(parts[1])); ks.append(int(parts[2]))
        draw.append(int(parts[3])); tempo.append(int(parts[4]))
        toks = parts[6].split()  # '<nf> i:v i:v ...' (features follow nf by space)
        feats = toks[1:]
        for f in feats:
            i, v = f.split(':')
            idx_all.append(int(i)); val_all.append(int(v))
        off.append(len(idx_all))
    n = len(labels)
    assert off[-1] > n * 10, 'feature parse produced too few entries'
    return (np.array(labels), np.array(phase), np.array(ks), np.array(draw), np.array(tempo),
            np.array(idx_all, dtype=np.int64), np.array(val_all, dtype=np.float64),
            np.array(off, dtype=np.int64), n)

def batch_dots(rows, idx_all, val_all, off, w):
    starts, ends = off[rows], off[rows + 1]
    counts = ends - starts
    flat = np.concatenate([np.arange(s, e) for s, e in zip(starts, ends)])
    rid = np.repeat(np.arange(len(rows)), counts)
    contrib = val_all[flat] * w[idx_all[flat]]
    return np.bincount(rid, weights=contrib, minlength=len(rows))

def predict(mg, eg, ph, ks, draw, tempo):
    return ((mg + ks) * ph + eg * (24 - ph)) / 24.0 / draw + tempo

def loss_of(pred, y, K):
    s = 1.0 / (1.0 + 10.0 ** (-pred * K / 400.0))
    return float(np.mean((y - s) ** 2))

def fit_K(pred, y):
    best, bk = 1e9, 1.0
    for K in np.linspace(0.4, 4.0, 73):
        l = loss_of(pred, y, K)
        if l < best: best, bk = l, K
    lo, hi = bk - 0.1, bk + 0.1
    for _ in range(40):
        m1, m2 = lo + (hi - lo) * 0.382, lo + (hi - lo) * 0.618
        if loss_of(pred, y, m1) < loss_of(pred, y, m2): hi = m2
        else: lo = m1
    return (lo + hi) / 2

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine', default='./carnivorus')
    ap.add_argument('--features', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--epochs', type=int, default=12)
    ap.add_argument('--batch', type=int, default=16384)
    ap.add_argument('--lr', type=float, default=0.75)
    ap.add_argument('--lam', type=float, default=2e-6)
    ap.add_argument('--seed', type=int, default=7)
    args = ap.parse_args()

    w_mg0, w_eg0 = engine_weights(args.engine)
    y, ph, ks, draw, tempo, idx_all, val_all, off, n = load_features(args.features)
    print(f'{n} positions', flush=True)
    phf = ph.astype(np.float64); ksf = ks.astype(np.float64)
    drawf = draw.astype(np.float64); tempof = tempo.astype(np.float64)

    rng = np.random.default_rng(args.seed)
    perm = rng.permutation(n)
    nval = n // 10
    val_rows, train_rows = perm[:nval], perm[nval:]

    def full_pred(w_mg, w_eg, rows):
        outs = []
        for i in range(0, len(rows), 65536):
            r = rows[i:i+65536]
            mg = batch_dots(r, idx_all, val_all, off, w_mg)
            eg = batch_dots(r, idx_all, val_all, off, w_eg)
            outs.append(predict(mg, eg, phf[r], ksf[r], drawf[r], tempof[r]))
        return np.concatenate(outs)

    pred0 = full_pred(w_mg0, w_eg0, val_rows)
    K = fit_K(pred0, y[val_rows])
    print(f'K = {K:.4f}  init val loss = {loss_of(pred0, y[val_rows], K):.6f}', flush=True)

    w_mg, w_eg = w_mg0.copy(), w_eg0.copy()
    m_mg = np.zeros(512); v_mg = np.zeros(512)
    m_eg = np.zeros(512); v_eg = np.zeros(512)
    b1, b2, eps = 0.9, 0.999, 1e-8
    steps_per_epoch = max(1, len(train_rows) // args.batch)
    t = 0
    for ep in range(args.epochs):
        rp = rng.permutation(train_rows)
        lr = args.lr / (1.0 + ep / max(1.0, args.epochs / 3.0))
        for s in range(steps_per_epoch):
            rows = rp[s * args.batch:(s + 1) * args.batch]
            t += 1
            mg = batch_dots(rows, idx_all, val_all, off, w_mg)
            eg = batch_dots(rows, idx_all, val_all, off, w_eg)
            pred = predict(mg, eg, phf[rows], ksf[rows], drawf[rows], tempof[rows])
            sig = 1.0 / (1.0 + 10.0 ** (-pred * K / 400.0))
            dloss = -2 * (y[rows] - sig) * sig * (1 - sig) * K / 400.0 * np.log(10.0)
            dloss /= len(rows)
            coef_mg = dloss * phf[rows] / (24.0 * drawf[rows])
            coef_eg = dloss * (24 - phf[rows]) / (24.0 * drawf[rows])
            starts, ends = off[rows], off[rows + 1]
            counts = ends - starts
            flat = np.concatenate([np.arange(a, b) for a, b in zip(starts, ends)])
            rid = np.repeat(np.arange(len(rows)), counts)
            ii = idx_all[flat]; vv = val_all[flat]
            g_mg = np.zeros(512); g_eg = np.zeros(512)
            np.add.at(g_mg, ii, coef_mg[rid] * vv)
            np.add.at(g_eg, ii, coef_eg[rid] * vv)
            g_mg += args.lam * (w_mg - w_mg0); g_eg += args.lam * (w_eg - w_eg0)
            m_mg = b1 * m_mg + (1 - b1) * g_mg; v_mg = b2 * v_mg + (1 - b2) * g_mg * g_mg
            m_eg = b1 * m_eg + (1 - b1) * g_eg; v_eg = b2 * v_eg + (1 - b2) * g_eg * g_eg
            w_mg -= lr * (m_mg / (1 - b1 ** t)) / (np.sqrt(v_mg / (1 - b2 ** t)) + eps)
            w_eg -= lr * (m_eg / (1 - b1 ** t)) / (np.sqrt(v_eg / (1 - b2 ** t)) + eps)
        pv = full_pred(w_mg, w_eg, val_rows)
        print(f'epoch {ep+1}: val loss {loss_of(pv, y[val_rows], K):.6f}', flush=True)

    final = loss_of(full_pred(w_mg, w_eg, val_rows), y[val_rows], K)
    init = loss_of(pred0, y[val_rows], K)
    print(f'init {init:.6f} -> final {final:.6f} (val, K={K:.4f})', flush=True)
    np.savez(args.out, w_mg=w_mg, w_eg=w_eg, K=K, init_loss=init, final_loss=final,
             w_mg0=w_mg0, w_eg0=w_eg0)
    print('saved', args.out)
