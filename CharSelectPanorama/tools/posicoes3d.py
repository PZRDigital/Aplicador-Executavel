"""Escolhe posições dos personagens e a direção de câmera de cada um com linha de visão livre.

Uso: python3 posicoes3d.py <pasta do mapa> [qtd=15] [inclinacao=22] [distancia=120] [altura_alvo=14]
Imprime as linhas da seção [posicoes3d] (slot=x,y,direcao).
"""
import math, os, struct, sys
import numpy as np
import preview3d as pv

CELL = 2.0  # resolução do mapa de ocupação (unidades do mundo)

def occupancy(d):
    m = pv.load(d)
    V = m['V']['p'].astype(np.float64)
    W, H = m['W'], m['H']
    nx, nz = int(10 * W / CELL) + 8, int((10 * H + 20) / CELL) + 8
    lo = np.full((nx, nz), np.inf); hi = np.full((nx, nz), -np.inf)
    tris = V.reshape(-1, 3, 3)
    # só o que é opaco (lotes 0 e 1) conta como obstáculo
    keep = np.zeros(len(tris), bool)
    for tex, kind, cull, start, count in m['batches']:
        if kind != 2: keep[start // 3:(start + count) // 3] = True
    tris = tris[keep]
    x0 = np.floor(tris[:, :, 0].min(1) / CELL).astype(int); x1 = np.floor(tris[:, :, 0].max(1) / CELL).astype(int)
    z0 = np.floor(tris[:, :, 2].min(1) / CELL).astype(int); z1 = np.floor(tris[:, :, 2].max(1) / CELL).astype(int)
    y0 = tris[:, :, 1].min(1); y1 = tris[:, :, 1].max(1)
    for i in range(len(tris)):
        a, b, c, e = max(x0[i], 0), min(x1[i], nx - 1), max(z0[i], 0), min(z1[i], nz - 1)
        if a > b or c > e: continue
        # triângulos grandes (paredes/copas): intervalo real por célula seria melhor; bbox basta
        np.minimum(lo[a:b + 1, c:e + 1], y0[i], out=lo[a:b + 1, c:e + 1])
        np.maximum(hi[a:b + 1, c:e + 1], y1[i], out=hi[a:b + 1, c:e + 1])
    return lo, hi

def gat(d):
    b = open(os.path.join(d, 'gat.bin'), 'rb').read(); w, h = struct.unpack_from('<ii', b)
    return np.frombuffer(b, np.dtype([('h', '<f4'), ('t', 'u1')]), w * h, 8).reshape(h, w)

def main():
    d = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    pitch = float(sys.argv[3]) if len(sys.argv) > 3 else 22
    dist = float(sys.argv[4]) if len(sys.argv) > 4 else 120
    th = float(sys.argv[5]) if len(sys.argv) > 5 else 14
    H = W = 100
    lo, hi = occupancy(d)
    g = gat(d)
    walk = (g['t'] == 0) | (g['t'] == 3)
    # distância até a borda andável
    dist_b = np.zeros(walk.shape, int); cur = walk.copy(); k = 0
    while cur.any():
        k += 1; dist_b[cur] = k; nn = cur.copy()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)): nn &= np.roll(np.roll(cur, dy, 0), dx, 1)
        cur = nn
    def world(cx, cy): return 5 * cx + 2.5, float(g['h'][cy, cx]), 10 * H - 5 * cy + 7.5
    def blocked(x, y, z, margin=1.5):
        i, j = int(x / CELL), int(z / CELL)
        if i < 0 or j < 0 or i >= lo.shape[0] or j >= lo.shape[1]: return False
        return lo[i, j] - margin <= y <= hi[i, j] + margin and hi[i, j] > -1e9
    def body_clear(cx, cy):
        x, y, z = world(cx, cy)
        for dx in (-3, 0, 3):
            for dz in (-3, 0, 3):
                i, j = int((x + dx) / CELL), int((z + dz) / CELL)
                # algo entre 4 e 26 unidades acima do chão = árvore/pedra no corpo
                if hi[i, j] > y + 4 and lo[i, j] < y + 26: return False
        return True
    def view_score(cx, cy, yaw):
        x, y, z = world(cx, cy)
        ty = y + th
        cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
        ex = x + dist * math.sin(math.radians(yaw)) * cp; ey = ty + dist * sp; ez = z + dist * math.cos(math.radians(yaw)) * cp
        bad = 0
        # do olho até perto do personagem (corpo inteiro: pés, meio e cabeça)
        for h_ in (y + 3, y + 10, y + 18):
            for t in np.linspace(0.12, 0.93, 28):
                px, py, pz = ex + (x - ex) * t, ey + (h_ - ey) * t, ez + (z - ez) * t
                if blocked(px, py, pz): bad += 1
        return bad
    cand = [(cx, cy) for cy, cx in zip(*np.where(dist_b >= 3)) if body_clear(cx, cy)]
    print('candidatas livres:', len(cand), file=sys.stderr)
    cx0, cz0 = 5 * W + 10, 5 * H + 10
    best = {}
    for (cx, cy) in cand:
        x, y, z = world(cx, cy)
        to_center = math.degrees(math.atan2(cx0 - x, cz0 - z))
        opts = []
        for dyaw in range(-60, 61, 10):
            yaw = to_center + dyaw
            s = view_score(cx, cy, yaw)
            opts.append((s, abs(dyaw), yaw))
        opts.sort()
        best[(cx, cy)] = opts[0]
    good = [p for p in cand if best[p][0] == 0]
    print('com visão livre:', len(good), file=sys.stderr)
    pool = good if len(good) >= n else sorted(cand, key=lambda p: best[p][0])[:max(n * 3, len(good))]
    xs = np.array([p[0] for p in pool]); ys = np.array([p[1] for p in pool])
    mx, my = xs.mean(), ys.mean()
    pts = [min(pool, key=lambda p: (p[0] - mx) ** 2 + (p[1] - my) ** 2)]
    while len(pts) < n:
        pts.append(max(pool, key=lambda p: min((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 for q in pts)))
    sp = min(math.hypot(a[0] - b[0], a[1] - b[1]) for i, a in enumerate(pts) for b in pts[i + 1:])
    print('espaçamento mínimo %.1f células' % sp, file=sys.stderr)
    for i, p in enumerate(pts):
        s, _, yaw = best[p]
        yaw = (yaw + 180) % 360 - 180
        print('%d=%d,%d,%.0f' % (i, p[0], p[1], yaw) + ('' if s == 0 else '   ; obstruida %d' % s))

if __name__ == '__main__':
    main()
