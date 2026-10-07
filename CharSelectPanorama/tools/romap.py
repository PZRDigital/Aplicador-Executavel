"""Parsers de mapa do Ragnarok: GND, RSW, RSM (1.x e 2.x)."""
import struct, math
import numpy as np

class R:
    def __init__(self, d, p=0): self.d = d; self.p = p
    def u(self, fmt):
        v = struct.unpack_from('<' + fmt, self.d, self.p); self.p += struct.calcsize('<' + fmt); return v
    def i(self): return self.u('i')[0]
    def f(self): return self.u('f')[0]
    def s(self, n):
        b = self.d[self.p:self.p + n]; self.p += n
        return b.split(b'\0')[0]
    def dyn(self):
        n = self.i(); return self.s(n)
    def fs(self, n): return list(self.u('%df' % n))

# ---------------------------------------------------------------- GND
class Gnd:
    def __init__(self, d):
        r = R(d)
        assert d[:4] == b'GRGN'
        self.version = (d[4] << 8) | d[5]; r.p = 6
        self.width, self.height = r.i(), r.i()
        self.zoom = r.f()
        ntex, texlen = r.i(), r.i()
        self.textures = [r.s(texlen) for _ in range(ntex)]
        nlm = r.i(); self.lmw, self.lmh = r.i(), r.i(); r.i()
        sz = self.lmw * self.lmh * 4
        self.lightmaps = [d[r.p + k * sz: r.p + (k + 1) * sz] for k in range(nlm)]
        r.p += nlm * sz
        ntile = r.i()
        dt = np.dtype([('u', '<f4', 4), ('v', '<f4', 4), ('tex', '<i2'), ('lm', '<u2'), ('color', 'u1', 4)])
        self.tiles = np.frombuffer(d, dt, ntile, r.p); r.p += ntile * dt.itemsize
        cdt = np.dtype([('h', '<f4', 4), ('up', '<i4'), ('front', '<i4'), ('side', '<i4')])
        self.cubes = np.frombuffer(d, cdt, self.width * self.height, r.p).reshape(self.height, self.width)
        r.p += self.width * self.height * cdt.itemsize
        self.water = None
        if self.version >= 0x108 and r.p < len(d):
            lvl, typ, amp, spd, pitch, anim = r.u('fifffi')
            sw, sh = r.i(), r.i()
            zones = []
            for _ in range(sw * sh):
                z = [r.f()]
                if self.version >= 0x109: z += list(r.u('ifffi'))
                zones.append(z)
            self.water = dict(level=lvl, type=typ, amp=amp, speed=spd, pitch=pitch, anim=anim, split=(sw, sh), zones=zones)

# ---------------------------------------------------------------- RSW
class Rsw:
    def __init__(self, d):
        r = R(d)
        assert d[:4] == b'GRSW'
        self.version = (d[4] << 8) | d[5]; r.p = 6
        self.build = r.i() if self.version >= 0x205 else 0
        if self.version >= 0x202: r.p += 1
        self.ini, self.gnd, self.gat, self.src = r.s(40), r.s(40), r.s(40), r.s(40)
        self.water = None
        if self.version < 0x206:
            lvl = r.f() if self.version >= 0x103 else 0
            typ, amp, spd, pitch = (r.i(), r.f(), r.f(), r.f()) if self.version >= 0x108 else (0, 1, 2, 50)
            anim = r.i() if self.version >= 0x109 else 3
            self.water = dict(level=lvl, type=typ, amp=amp, speed=spd, pitch=pitch, anim=anim)
        self.light = dict(lon=45, lat=45, diffuse=[1, 1, 1], ambient=[.3, .3, .3], intensity=.5)
        if self.version >= 0x105:
            self.light['lon'], self.light['lat'] = r.i(), r.i()
            self.light['diffuse'] = r.fs(3); self.light['ambient'] = r.fs(3)
        if self.version >= 0x107: self.light['intensity'] = r.f()
        if self.version >= 0x106: r.p += 16
        if self.version >= 0x207:
            n = r.i(); r.p += 4 * n
        n = r.i()
        self.models, self.lights, self.effects = [], [], []
        for _ in range(n):
            t = r.i()
            if t == 1:
                name = r.s(40); anim, spd, block = r.i(), r.f(), r.i()
                if self.version >= 0x206 and self.build >= 186: r.p += 1
                if self.version >= 0x207: r.p += 4
                fn = r.s(80); node = r.s(80)
                pos, rot, scl = r.fs(3), r.fs(3), r.fs(3)
                self.models.append(dict(name=name, file=fn, pos=pos, rot=rot, scale=scl, animSpeed=spd))
            elif t == 2:
                name = r.s(80); pos = r.fs(3); col = r.fs(3); rng = r.f()
                self.lights.append(dict(name=name, pos=pos, color=col, range=rng))
            elif t == 3:
                name = r.s(80); wav = r.s(80); pos = r.fs(3); r.u('fiiiff'); r.fs(3)
                if self.version >= 0x200: r.f()
            elif t == 4:
                name = r.s(80); pos = r.fs(3); eid = r.i(); delay = r.f(); params = r.fs(4)
                self.effects.append(dict(name=name, pos=pos, id=eid))
            else:
                raise ValueError('objeto RSW desconhecido %d em %d' % (t, r.p))

# ---------------------------------------------------------------- RSM
def quat_mat(x, y, z, w):
    n = math.sqrt(x*x + y*y + z*z + w*w) or 1; x, y, z, w = x/n, y/n, z/n, w/n
    return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w), 0],
                     [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w), 0],
                     [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y), 0],
                     [0, 0, 0, 1]], dtype=np.float64)

def T(v):
    m = np.eye(4); m[:3, 3] = v; return m

def S(v):
    return np.diag([v[0], v[1], v[2], 1.0])

def Rot(angle, axis):
    a = np.asarray(axis, dtype=np.float64); n = np.linalg.norm(a)
    if n < 1e-9: return np.eye(4)
    x, y, z = a / n; c, s = math.cos(angle), math.sin(angle); t = 1 - c
    return np.array([[t*x*x+c, t*x*y-s*z, t*x*z+s*y, 0],
                     [t*x*y+s*z, t*y*y+c, t*y*z-s*x, 0],
                     [t*x*z-s*y, t*y*z+s*x, t*z*z+c, 0],
                     [0, 0, 0, 1]])

class Mesh: pass

class Rsm:
    def __init__(self, d):
        r = R(d)
        assert d[:4] == b'GRSM', d[:4]
        self.version = v = (d[4] << 8) | d[5]; r.p = 6
        self.animLen, self.shade = r.i(), r.i()
        self.alpha = d[r.p] if v >= 0x104 else 255
        if v >= 0x104: r.p += 1
        self.textures = []
        if v >= 0x203:
            self.fps = r.f(); nroot = r.i(); root = [r.dyn() for _ in range(nroot)]
        elif v >= 0x202:
            self.fps = r.f(); nt = r.i(); self.textures = [r.dyn() for _ in range(nt)]
            nroot = r.i(); root = [r.dyn() for _ in range(nroot)]
        else:
            r.p += 16; nt = r.i(); self.textures = [r.s(40) for _ in range(nt)]
            root = [r.s(40)]
        nmesh = r.i()
        self.meshes = []
        for _ in range(nmesh):
            m = Mesh()
            if v >= 0x202: m.name, m.parentName = r.dyn(), r.dyn()
            else: m.name, m.parentName = r.s(40), r.s(40)
            nt = r.i()
            if v >= 0x203:
                m.tex = []
                for _ in range(nt):
                    fn = r.dyn()
                    if fn not in self.textures: self.textures.append(fn)
                    m.tex.append(self.textures.index(fn))
            else:
                m.tex = list(r.u('%di' % nt))
            o = r.fs(9)
            m.offset = np.eye(4)
            # glm coluna-major: offset[c][r] lidos em ordem -> coluna c
            for c in range(3):
                for rr in range(3):
                    m.offset[rr, c] = o[c * 3 + rr]
            m.pos_ = np.array(r.fs(3))
            if v >= 0x202:
                m.pos = np.zeros(3); m.rotangle = 0.0; m.rotaxis = np.zeros(3); m.scale = np.ones(3)
            else:
                m.pos = np.array(r.fs(3)); m.rotangle = r.f(); m.rotaxis = np.array(r.fs(3)); m.scale = np.array(r.fs(3))
            nv = r.i(); m.verts = np.frombuffer(d, '<f4', nv * 3, r.p).reshape(nv, 3).astype(np.float64); r.p += nv * 12
            ntc = r.i()
            if v >= 0x102:
                tc = np.frombuffer(d, '<f4', ntc * 3, r.p).reshape(ntc, 3); r.p += ntc * 12; m.tcs = tc[:, 1:3].astype(np.float64)
            else:
                m.tcs = np.frombuffer(d, '<f4', ntc * 2, r.p).reshape(ntc, 2).astype(np.float64); r.p += ntc * 8
            nf = r.i(); m.faces = []
            for _ in range(nf):
                ln = r.i() if v >= 0x202 else -1
                vi = r.u('3h'); ti = r.u('3h'); tex, pad, two = r.u('hhi')
                sg = 0
                if v >= 0x102:
                    sg = r.i()
                    extra = 0
                    if ln > 24: extra += 1
                    if ln > 28: extra += 1
                    extra += max(0, (ln - 32 + 3) // 4) if ln > 32 else 0
                    r.p += 4 * extra
                m.faces.append((vi, ti, tex, two, sg))
            m.scaleFrames = []; m.rotFrames = []; m.posFrames = []
            if v >= 0x106:
                n = r.i()
                for _ in range(n): m.scaleFrames.append(r.u('i3ff'))
            n = r.i()
            for _ in range(n): m.rotFrames.append(r.u('i4f'))
            if v >= 0x202:
                n = r.i()
                for _ in range(n): m.posFrames.append(r.u('i3ff'))
                if v >= 0x203:
                    n = r.i()
                    for _ in range(n):
                        tid = r.i(); na = r.i()
                        for _ in range(na):
                            typ, nfr = r.i(), r.i(); r.p += 8 * nfr
            self.meshes.append(m)
        byname = {}
        for m in self.meshes:
            nm = m.name
            while nm in byname: nm += b'(dup)'
            m.name = nm; byname[nm] = m
        self.root = byname.get(root[0] if root else b'', self.meshes[0])
        for m in self.meshes: m.children = []; m.parent = None
        def fetch(p):
            for m in self.meshes:
                if m.parentName == p.name and m is not p and m.parent is None and m is not self.root:
                    m.parent = p; p.children.append(m)
            for c in p.children: fetch(c)
        fetch(self.root)
        self.calc()

    def walk(self, m=None):
        m = m or self.root
        yield m
        for c in m.children: yield from self.walk(c)

    def calc(self):
        """Replica Rsm::updateMatrices / calcMatrix1(0) / calcMatrix2 do BrowEdit."""
        v = self.version
        # bounding box (versão 1.x)
        if v < 0x202:
            bmin = np.full(3, 1e9); bmax = np.full(3, -1e9)
            def bbox(m):
                nonlocal bmin, bmax
                mat = m.offset.copy()
                if m.parent is not None: mat = T(m.pos_) @ mat  # aproximação do BrowEdit (setBoundingBox)
                for vert in m.verts:
                    p = mat @ np.append(vert, 1)
                    if m.parent is not None or m.children: p[:3] += m.pos + m.pos_
                    bmin = np.minimum(bmin, p[:3]); bmax = np.maximum(bmax, p[:3])
                for c in m.children: bbox(c)
            self._bbox_v1()
        for m in self.walk():
            self.calc1(m)
        if v < 0x202:
            for m in self.walk():
                m.matrix2 = np.eye(4)
                if m.parent is not None or m.children:
                    m.matrix2 = T(m.pos_)
                m.matrix2 = m.matrix2 @ m.offset
        # matriz final de cada malha
        for m in self.walk():
            if v >= 0x202:
                m.final = m.matrix2
            else:
                sub = m.parent.sub if m.parent is not None else np.eye(4)
                m.final = sub @ m.matrix1 @ m.matrix2
                m.sub = sub @ m.matrix1
        # bbox real (y invertido), usado pelo posicionamento no mapa (v1)
        rmin = np.full(3, 1e9); rmax = np.full(3, -1e9)
        flip = S([1, -1, 1])
        for m in self.walk():
            if len(m.verts) == 0: continue
            p = (flip @ m.final @ np.c_[m.verts, np.ones(len(m.verts))].T).T[:, :3]
            rmin = np.minimum(rmin, p.min(0)); rmax = np.maximum(rmax, p.max(0))
        self.realbbmin, self.realbbmax = rmin, rmax
        self.realbbrange = (rmin + rmax) / 2

    def _bbox_v1(self):
        # Rsm::Mesh::setBoundingBox do BrowEdit
        self.bbmin = np.full(3, 1e9); self.bbmax = np.full(3, -1e9)
        def rec(m, parent_off=None):
            myofs = m.offset.copy()
            if m.parent is not None:
                myofs = T(m.pos_) @ myofs  # não usado de fato; manter compatível
            for vert in m.verts:
                p = (m.offset @ np.append(vert, 1))[:3]
                if m.parent is not None or m.children:
                    p = p + m.pos + m.pos_
                self.bbmin = np.minimum(self.bbmin, p); self.bbmax = np.maximum(self.bbmax, p)
            for c in m.children: rec(c)
        rec(self.root)
        self.bbrange = (self.bbmin + self.bbmax) / 2

    def calc1(self, m):
        v = self.version
        if v < 0x202:
            mat = np.eye(4)
            if m.parent is None:
                if m.children:
                    mat = mat @ T([-self.bbrange[0], -self.bbmax[1], -self.bbrange[2]])
                else:
                    mat = mat @ T([0, -self.bbmax[1] + self.bbrange[1], 0])
            else:
                mat = mat @ T(m.pos)
            if not m.rotFrames:
                if abs(m.rotangle) > 0.01:
                    mat = mat @ Rot(m.rotangle, m.rotaxis)
            else:
                t, x, y, z, w = m.rotFrames[0]
                mat = mat @ quat_mat(x, y, z, w)
            mat = mat @ S(m.scale)
            m.matrix1 = mat
        else:
            mat = np.eye(4)
            if m.rotFrames:
                t, x, y, z, w = m.rotFrames[0]
                mat = quat_mat(x, y, z, w) @ mat
            else:
                mat = m.offset @ mat
                if m.parent is not None:
                    mat = np.linalg.inv(m.parent.offset) @ mat
            if m.scaleFrames:
                t, sx, sy, sz, dd = m.scaleFrames[0]
                mat = mat @ S([sx, sy, sz])
            if m.posFrames:
                position = np.array(m.posFrames[0][1:4])
            elif m.parent is not None:
                position = m.pos_ - m.parent.pos_
                position = (np.linalg.inv(m.parent.offset) @ np.append(position, 0))[:3]
            else:
                position = m.pos_
            mat = T(position) @ mat
            m.matrix1 = mat
            m.matrix2 = mat if m.parent is None else m.parent.matrix2 @ mat
