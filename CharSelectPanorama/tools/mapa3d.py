"""Converte um mapa do Ragnarok (RSW/GND/GAT/RSM) numa malha estática para a DLL.

Uso: python3 mapa3d.py <pasta do cliente> <mapa> <pasta de saída>

Saída:
  mapa.bin      malha (ver formato em Escrever())
  tNNN.png      texturas (magenta vira transparente)
  luz.png       atlas dos lightmaps do chão (rgb = cor, a = sombra)
  agua/NN.png   quadros da água (se houver)
  gat.bin       células andáveis: largura, altura, e por célula (altura média float, tipo u8)
"""
import io, math, os, struct, sys
import numpy as np
from PIL import Image
from grf import Vfs
from romap import Gnd, Rsw, Rsm, T, S, Rot

def rotY(a): return Rot(math.radians(a), [0, 1, 0])
def rotX(a): return Rot(math.radians(a), [1, 0, 0])
def rotZ(a): return Rot(math.radians(a), [0, 0, 1])

def bleed(rgba):
    """Espalha a cor dos pixels opacos para os transparentes (mipmaps sem franja escura)."""
    a = rgba[:, :, 3] > 0
    if a.all() or not a.any(): return rgba
    col = rgba[:, :, :3].astype(np.float32) * a[:, :, None]
    w = a.astype(np.float32)
    out = rgba.copy()
    filled = a.copy()
    for _ in range(16):
        if filled.all(): break
        cs = np.zeros_like(col); ws = np.zeros_like(w)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                cs += np.roll(np.roll(col, dy, 0), dx, 1); ws += np.roll(np.roll(w, dy, 0), dx, 1)
        new = (~filled) & (ws > 0)
        col[new] = cs[new] / ws[new][:, None]; w[new] = 1; filled |= new
    out[~a, :3] = np.clip(col[~a], 0, 255).astype(np.uint8)
    return out

def load_texture(vfs, name):
    data = vfs.read(b'data\\texture\\' + name)
    img = Image.open(io.BytesIO(data))
    if img.mode == 'P' or img.mode == 'RGB' or name.lower().endswith(b'.bmp'):
        a = np.array(img.convert('RGB'))
        key = (a[:, :, 0] >= 254) & (a[:, :, 1] <= 3) & (a[:, :, 2] >= 254)
        rgba = np.dstack([a, np.where(key, 0, 255).astype(np.uint8)])
        rgba = bleed(rgba)
        return Image.fromarray(rgba, 'RGBA'), bool(key.any()), False
    img = img.convert('RGBA')
    al = np.array(img)[:, :, 3]
    img = Image.fromarray(bleed(np.array(img)), 'RGBA')
    semi = bool(((al > 8) & (al < 247)).mean() > 0.002)
    return img, bool((al < 128).any()), semi

def light_dir(light):
    lon, lat = math.radians(light['lon']), math.radians(light['lat'])
    return np.array([math.cos(lon) * math.sin(lat), math.cos(lat), math.sin(lon) * math.sin(lat)])

def light_mult(normals, light, mode=None):
    diff = np.array(light['diffuse']); amb = np.array(light['ambient'])
    if normals is None:
        return np.minimum(amb + diff * 0.5, 1)[None, :]
    nl = np.clip(normals @ light_dir(light), 0, 1)
    return np.clip(amb + nl[:, None] * diff, 0, 1)

POINT_LIGHTS = []   # (pos mundo, cor, alcance), preenchido em main()
EMITTERS = []       # (pos mundo, id do efeito)
GLOWS = []          # (pos mundo, cor, tamanho)

def point_light(pos, normals):
    """Luzes pontuais do RSW somadas nos vértices (o brilho quente do sol, o frio da lua)."""
    add = np.zeros((len(pos), 3))
    for lp, col, rng in POINT_LIGHTS:
        d = pos - lp; dist = np.linalg.norm(d, axis=1)
        k = np.clip(1 - dist / rng, 0, 1) ** 2
        if normals is not None:
            ln = -d / np.maximum(dist[:, None], 1e-6)
            k = k * (0.35 + 0.65 * np.clip((normals * ln).sum(1), 0, 1))
        add += k[:, None] * np.array(col) * 1.6
    return add

EMISSIVE = (b'suntree', b'moontree')

class Builder:
    def __init__(self):
        self.batches = {}   # (tex, kind, cull) -> list of arrays
        self.textures = []  # nomes
        self.texinfo = []

    def tex(self, vfs, name):
        key = name.lower()
        for i, n in enumerate(self.textures):
            if n == key: return i
        try:
            img, alpha, semi = load_texture(vfs, name)
        except Exception as ex:
            print('  textura ausente', name.decode('cp949', 'replace'), ex)
            img, alpha, semi = Image.new('RGBA', (8, 8), (200, 200, 200, 255)), False, False
        self.textures.append(key); self.texinfo.append((img, alpha, semi))
        return len(self.textures) - 1

    def add(self, tex, kind, cull, pos, color, uv, uv2):
        # pos (n,3) color (n,4 0..1) uv (n,2) uv2 (n,2); n múltiplo de 3
        self.batches.setdefault((tex, kind, cull), []).append(np.hstack([pos, color, uv, uv2]).astype(np.float32))

def build_ground(b, vfs, gnd, rsw):
    W, H = gnd.width, gnd.height
    c = gnd.cubes
    h = c['h'].astype(np.float64)  # (H, W, 4): h1 h2 h3 h4 ; eixo y para cima = -h
    # atlas de lightmaps
    per_row = 2048 // gnd.lmw
    rows = (len(gnd.lightmaps) + per_row - 1) // per_row
    aw = 2048; ah = 1
    while ah < rows * gnd.lmh: ah *= 2
    atlas = np.zeros((ah, aw, 4), np.uint8)
    n = gnd.lmw * gnd.lmh
    for k, lm in enumerate(gnd.lightmaps):
        a = np.frombuffer(lm, np.uint8)
        sh = a[:n].reshape(gnd.lmh, gnd.lmw)
        col = a[n:].reshape(gnd.lmh, gnd.lmw, 3)
        x0 = (k % per_row) * gnd.lmw; y0 = (k // per_row) * gnd.lmh
        atlas[y0:y0 + gnd.lmh, x0:x0 + gnd.lmw, :3] = col
        atlas[y0:y0 + gnd.lmh, x0:x0 + gnd.lmw, 3] = sh
    def lmuv(idx):
        x0 = (idx % per_row) * gnd.lmw; y0 = (idx // per_row) * gnd.lmh
        u1 = (x0 + 1) / aw; v1 = (y0 + 1) / ah
        u2 = (x0 + gnd.lmw - 1) / aw; v2 = (y0 + gnd.lmh - 1) / ah
        return u1, v1, u2, v2
    # normais suaves por canto
    def P(x, y, k):
        hx = [0, 10, 0, 10][k]; hz = [10, 10, 0, 0][k]
        return np.array([10 * x + hx, -h[y, x, k], 10 * H - 10 * y - 10 + hz])
    # face normal por cubo (média dos 2 triângulos)
    fn = np.zeros((H, W, 3))
    for y in range(H):
        for x in range(W):
            p1, p2, p3, p4 = P(x, y, 0), P(x, y, 1), P(x, y, 2), P(x, y, 3)
            n1 = np.cross(p3 - p1, p2 - p1); n2 = np.cross(p3 - p2, p4 - p2)
            nn = n1 + n2; l = np.linalg.norm(nn)
            fn[y, x] = nn / l if l > 0 else [0, 1, 0]
    if fn[..., 1].mean() < 0: fn = -fn
    vn = np.zeros((H + 1, W + 1, 3))
    vn[:-1, :-1] += fn; vn[:-1, 1:] += fn; vn[1:, :-1] += fn; vn[1:, 1:] += fn
    vn /= np.maximum(np.linalg.norm(vn, axis=2, keepdims=True), 1e-9)
    tiles = gnd.tiles
    texidx = [b.tex(vfs, t) for t in gnd.textures]
    def tcol(y, x):
        if 0 <= y < H and 0 <= x < W and c[y, x]['up'] >= 0:
            return tiles[c[y, x]['up']]['color'][[2, 1, 0, 3]] / 255.0
        return np.array([1.0, 1, 1, 1])
    out = {}
    def push(tex, verts):
        out.setdefault(tex, []).extend(verts)
    for y in range(H):
        for x in range(W):
            cu = c[y, x]
            if cu['up'] >= 0:
                t = tiles[cu['up']]
                u1, v1, u2, v2 = lmuv(int(t['lm']))
                # cantos: 1=(x,y+?)... segue GndRenderer (z cresce para y menor)
                zA = 10 * H - 10 * y; zB = zA + 10
                n_ = lambda gx, gy: vn[gy, gx]
                V1 = ((10 * x, -cu['h'][2], zA), (t['u'][2], t['v'][2]), (u1, v2), tcol(y + 1, x), n_(x, y + 1))
                V2 = ((10 * x + 10, -cu['h'][3], zA), (t['u'][3], t['v'][3]), (u2, v2), tcol(y + 1, x + 1), n_(x + 1, y + 1))
                V3 = ((10 * x, -cu['h'][0], zB), (t['u'][0], t['v'][0]), (u1, v1), tcol(y, x), n_(x, y))
                V4 = ((10 * x + 10, -cu['h'][1], zB), (t['u'][1], t['v'][1]), (u2, v1), tcol(y, x + 1), n_(x + 1, y))
                push(t['tex'], [V4, V2, V1, V4, V1, V3])
            if cu['side'] >= 0 and x < W - 1:
                t = tiles[cu['side']]; adj = c[y, x + 1]
                u1, v1, u2, v2 = lmuv(int(t['lm']))
                nrm = np.array([-1.0, 0, 0]) * (-1 if cu['h'][2] < adj['h'][0] else 1)
                zA = 10 * H - 10 * y; zB = zA + 10
                c1 = tcol(y, x + 1); c2 = tcol(y + 1, x + 1)
                A = ((10 * x + 10, -cu['h'][1], zB), (t['u'][1], t['v'][1]), (u2, v1), c1, nrm)
                Bv = ((10 * x + 10, -cu['h'][3], zA), (t['u'][0], t['v'][0]), (u1, v1), c2, nrm)
                C = ((10 * x + 10, -adj['h'][0], zB), (t['u'][3], t['v'][3]), (u2, v2), c1, nrm)
                D = ((10 * x + 10, -adj['h'][2], zA), (t['u'][2], t['v'][2]), (u1, v2), c2, nrm)
                push(t['tex'], [A, C, D, D, Bv, A])
            if cu['front'] >= 0 and y < H - 1:
                t = tiles[cu['front']]; adj = c[y + 1, x]
                u1, v1, u2, v2 = lmuv(int(t['lm']))
                nrm = np.array([0, 0, -1.0]) * (-1 if cu['h'][2] < adj['h'][0] else 1)
                z = 10 * H - 10 * y
                c1 = tcol(y + 1, x); c2 = tcol(y + 1, x + 1)
                A = ((10 * x, -cu['h'][2], z), (t['u'][0], t['v'][0]), (u1, v1), c1, nrm)
                Bv = ((10 * x + 10, -cu['h'][3], z), (t['u'][1], t['v'][1]), (u2, v1), c2, nrm)
                D = ((10 * x + 10, -adj['h'][1], z), (t['u'][3], t['v'][3]), (u2, v2), c2, nrm)
                C = ((10 * x, -adj['h'][0], z), (t['u'][2], t['v'][2]), (u1, v2), c1, nrm)
                push(t['tex'], [A, Bv, C, C, Bv, D])
    for ti, verts in out.items():
        pos = np.array([v[0] for v in verts], np.float64)
        uv = np.array([v[1] for v in verts]); uv2 = np.array([v[2] for v in verts])
        col = np.array([v[3] for v in verts]); nrm = np.array([v[4] for v in verts])
        mult = np.clip(light_mult(nrm, rsw.light) + point_light(pos, nrm), 0, 1.0)
        color = np.hstack([col[:, :3] * mult, np.ones((len(pos), 1))])
        b.add(texidx[ti], 'ground', 1, pos, color, uv, uv2)
    return Image.fromarray(atlas, 'RGBA')

def model_world(gnd, obj, rsm):
    m = S([1, 1, -1])
    m = m @ T([5 * gnd.width + obj['pos'][0], -obj['pos'][1], -10 - 5 * gnd.height + obj['pos'][2]])
    m = m @ rotZ(-obj['rot'][2]) @ rotX(-obj['rot'][0]) @ rotY(obj['rot'][1])
    m = m @ S([obj['scale'][0], -obj['scale'][1], obj['scale'][2]])
    if rsm.version < 0x202:
        m = m @ T([-rsm.realbbrange[0], rsm.realbbmin[1], -rsm.realbbrange[2]])
    else:
        m = m @ S([1, -1, 1])
    return m

def build_models(b, vfs, gnd, rsw):
    cache = {}
    tris = 0
    for obj in rsw.models:
        fn = obj['file']
        if fn not in cache:
            try:
                cache[fn] = Rsm(vfs.read(b'data\\model\\' + fn))
            except Exception as ex:
                print('  modelo com erro', fn.decode('cp949', 'replace'), ex); cache[fn] = None
        rsm = cache[fn]
        if rsm is None: continue
        world = model_world(gnd, obj, rsm)
        texids = [b.tex(vfs, t) for t in rsm.textures]
        for mesh in rsm.walk():
            if not mesh.faces: continue
            M = world @ mesh.final
            det = np.linalg.det(M[:3, :3])
            N3 = M[:3, :3]
            vi = np.array([f[0] for f in mesh.faces]); ti = np.array([f[1] for f in mesh.faces])
            if vi.max() >= len(mesh.verts) or (len(mesh.tcs) and ti.max() >= len(mesh.tcs)):
                print('  índice inválido em', fn); continue
            v = mesh.verts
            p = (M @ np.c_[v, np.ones(len(v))].T).T[:, :3]
            # normais de face no espaço local -> mundo (como o shader do BrowEdit)
            fnorm = np.cross(v[vi[:, 1]] - v[vi[:, 0]], v[vi[:, 2]] - v[vi[:, 0]])
            fnorm /= np.maximum(np.linalg.norm(fnorm, axis=1, keepdims=True), 1e-12)
            if rsm.shade == 2:
                acc = {}
                for k, f in enumerate(mesh.faces):
                    for j in range(3): acc.setdefault((f[4], f[0][j]), np.zeros(3)); acc[(f[4], f[0][j])] += fnorm[k]
                vnorm = np.array([[acc[(f[4], f[0][j])] for j in range(3)] for f in mesh.faces])
            else:
                vnorm = np.repeat(fnorm[:, None, :], 3, axis=1)
            wn = vnorm.reshape(-1, 3) @ N3.T
            wn /= np.maximum(np.linalg.norm(wn, axis=1, keepdims=True), 1e-12)
            # como o BrowEdit (mesh->reverseCullFace): offset com determinante negativo inverte a normal
            rev = np.linalg.det(mesh.offset[:3, :3]) * (np.prod(mesh.scale) if rsm.version < 0x202 else 1) < 0
            if rev: wn = -wn
            emissive = any(e in fn.lower() for e in EMISSIVE)
            if emissive:
                mult = np.ones((len(wn), 3))
            elif rsm.shade == 0:
                mult = np.tile(light_mult(None, rsw.light), (len(wn), 1))
            else:
                mult = light_mult(wn, rsw.light)
            if not emissive:
                ppos = p[vi.reshape(-1)]
                mult = np.clip(mult + point_light(ppos, wn), 0, 1)
            mult = mult.reshape(-1, 3, 3)
            texs = np.array([f[2] for f in mesh.faces]); two = np.array([f[3] for f in mesh.faces])
            alpha = rsm.alpha / 255.0
            for k in range(len(mesh.faces)):
                pass
            order = [0, 2, 1] if det < 0 else [0, 1, 2]
            for tloc in np.unique(texs):
                if tloc < 0 or tloc >= len(mesh.tex): continue
                gi = mesh.tex[tloc]
                if gi >= len(texids): continue
                tid = texids[gi]
                img, has_alpha, semi = b.texinfo[tid]
                for twoside in (0, 1):
                    sel = np.where((texs == tloc) & ((two > 0) == bool(twoside)))[0]
                    if len(sel) == 0: continue
                    idx_v = vi[sel][:, order].reshape(-1)
                    idx_t = ti[sel][:, order].reshape(-1)
                    mm = mult[sel][:, order].reshape(-1, 3)
                    pos = p[idx_v]
                    uv = mesh.tcs[idx_t] if len(mesh.tcs) else np.zeros((len(idx_v), 2))
                    col = np.hstack([mm, np.full((len(pos), 1), alpha)])
                    kind = 'blend' if (semi or alpha < 0.99) else 'model'
                    b.add(tid, kind, 0 if twoside else 1, pos, col, uv, np.zeros_like(uv))
                    tris += len(sel)
    print('  modelos: %d objetos, %d arquivos, %d triângulos' % (len(rsw.models), len(cache), tris))

def build_water(vfs, gnd, rsw, outdir):
    w = gnd.water or rsw.water
    if not w: return None
    typ = w['type']
    os.makedirs(os.path.join(outdir, 'agua'), exist_ok=True)
    n = 0
    for k in range(32):
        name = ('data\\texture\\\xbf\xf6\xc5\xcd\\water%d%02d.jpg' % (typ, k)).encode('latin-1')
        try:
            img = Image.open(io.BytesIO(vfs.read(name))).convert('RGB')
        except KeyError:
            break
        img.save(os.path.join(outdir, 'agua', '%02d.png' % k)); n += 1
    print('  água tipo %d nível %.1f, %d quadros' % (typ, w['level'], n))
    return w, n

def build_clouds(vfs, outdir):
    os.makedirs(os.path.join(outdir, 'nuvens'), exist_ok=True)
    n = 0
    for k in range(1, 8):
        try:
            img = Image.open(io.BytesIO(vfs.read('data\\texture\\effect\\cloud%d.tga' % k))).convert('RGBA')
        except KeyError:
            continue
        a = np.array(img)
        if a[:, :, 3].max() == 0 or a[:, :, 3].min() == 255:
            # sem alfa útil: usa o brilho
            lum = a[:, :, :3].max(2)
            a[:, :, 3] = lum
            a[:, :, :3] = 255
        Image.fromarray(bleed(a), 'RGBA').save(os.path.join(outdir, 'nuvens', '%02d.png' % n)); n += 1
    print('  %d nuvens' % n)

def write(outdir, b, gnd, rsw, water):
    kinds = {'ground': 0, 'model': 1, 'blend': 2}
    for i, (img, a, semi) in enumerate(b.texinfo):
        img.save(os.path.join(outdir, 't%03d.png' % i))
    verts = []; batches = []; start = 0
    for (tex, kind, cull), arrs in sorted(b.batches.items(), key=lambda kv: (kinds[kv[0][1]], kv[0][0])):
        a = np.vstack(arrs); verts.append(a)
        batches.append((tex, kinds[kind], cull, start, len(a))); start += len(a)
    V = np.vstack(verts)
    pos = V[:, :3]
    col = np.clip(V[:, 3:7] * 255 + 0.5, 0, 255).astype(np.uint32)
    argb = (col[:, 3] << 24) | (col[:, 0] << 16) | (col[:, 1] << 8) | col[:, 2]
    rec = np.zeros(len(V), np.dtype([('p', '<f4', 3), ('c', '<u4'), ('t', '<f4', 2), ('t2', '<f4', 2)]))
    rec['p'] = pos; rec['c'] = argb; rec['t'] = V[:, 7:9]; rec['t2'] = V[:, 9:11]
    with open(os.path.join(outdir, 'mapa.bin'), 'wb') as f:
        f.write(b'CSM3'); f.write(struct.pack('<I', 2))
        f.write(struct.pack('<iiff', gnd.width, gnd.height, 0, 0))
        wl = water[0] if water else None
        f.write(struct.pack('<ifffffi', 1 if wl else 0, -(wl['level'] if wl else 0), wl['amp'] if wl else 0, wl['speed'] if wl else 0,
                            wl['pitch'] if wl else 0, float(wl['anim']) if wl else 0, water[1] if water else 0))
        f.write(struct.pack('<fff', *rsw.light['diffuse'])); f.write(struct.pack('<fff', *rsw.light['ambient']))
        f.write(struct.pack('<ii', len(b.texinfo), len(batches)))
        for img, a, semi in b.texinfo:
            f.write(struct.pack('<ii', 1 if a else 0, 1 if semi else 0))
        for bt in batches: f.write(struct.pack('<iiiii', *bt))
        f.write(struct.pack('<i', len(rec))); f.write(rec.tobytes())
        # v2: luzes pontuais e emissores de efeito (partículas)
        f.write(struct.pack('<i', len(POINT_LIGHTS)))
        for lp, col, rng in POINT_LIGHTS: f.write(struct.pack('<7f', *lp, *col, rng))
        f.write(struct.pack('<i', len(EMITTERS)))
        for ep, eid in EMITTERS: f.write(struct.pack('<3fi', *ep, eid))
        f.write(struct.pack('<i', len(GLOWS)))
        for gp, col, size in GLOWS: f.write(struct.pack('<7f', *gp, *col, size))
    print('  %d vértices, %d lotes, %d texturas' % (len(rec), len(batches), len(b.texinfo)))
    return V, batches

def write_gat(vfs, name, outdir):
    d = vfs.read('data\\%s.gat' % name)
    w, h = struct.unpack_from('<ii', d, 6)
    cells = np.frombuffer(d, np.dtype([('h', '<f4', 4), ('t', '<u4')]), w * h, 14)
    with open(os.path.join(outdir, 'gat.bin'), 'wb') as f:
        f.write(struct.pack('<ii', w, h))
        rec = np.zeros(w * h, np.dtype([('h', '<f4'), ('t', 'u1')]))
        rec['h'] = -cells['h'].mean(1); rec['t'] = cells['t']
        f.write(rec.tobytes())
    return w, h, cells

def main():
    client, name, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    vfs = Vfs(client)
    rsw = Rsw(vfs.read('data\\%s.rsw' % name))
    gnd = Gnd(vfs.read(b'data\\' + rsw.gnd))
    print('%s: gnd %dx%d, %d modelos, luz %s' % (name, gnd.width, gnd.height, len(rsw.models), rsw.light))
    def rsw_world(p):
        return np.array([5 * gnd.width + p[0], -p[1], 5 * gnd.height + 10 - p[2]])
    POINT_LIGHTS[:] = [(rsw_world(l['pos']), l['color'], l['range']) for l in rsw.lights]
    EMITTERS[:] = [(rsw_world(e['pos']), e['id']) for e in rsw.effects]
    # halos nos modelos que brilham (árvore-sol, árvore-lua): centro da caixa do modelo
    GLOWS[:] = []
    for obj in rsw.models:
        fnl = obj['file'].lower()
        if not any(e in fnl for e in EMISSIVE): continue
        rsm = Rsm(vfs.read(b'data\\model\\' + obj['file']))
        world = model_world(gnd, obj, rsm)
        pts = []
        for mesh in rsm.walk():
            if len(mesh.verts): pts.append((world @ mesh.final @ np.c_[mesh.verts, np.ones(len(mesh.verts))].T).T[:, :3])
        pts = np.vstack(pts); lo, hi = pts.min(0), pts.max(0)
        col = (1.0, 0.62, 0.22) if b'sun' in fnl else (0.55, 0.7, 1.0)
        GLOWS.append(((lo + hi) / 2, col, float(np.max(hi - lo))))
    print('  %d luzes, %d emissores, %d halos' % (len(POINT_LIGHTS), len(EMITTERS), len(GLOWS)))
    b = Builder()
    atlas = build_ground(b, vfs, gnd, rsw)
    atlas.save(os.path.join(outdir, 'luz.png'))
    build_models(b, vfs, gnd, rsw)
    water = build_water(vfs, gnd, rsw, outdir)
    build_clouds(vfs, outdir)
    write(outdir, b, gnd, rsw, water)
    write_gat(vfs, name, outdir)

if __name__ == '__main__':
    main()
