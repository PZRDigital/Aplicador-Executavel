"""Ícones de classe em medalhão (tema O Codex), a partir dos ícones do cliente
(renewalparty\\icon_jobs_<id>.bmp, os mesmos da bROWiki).

Uso: python3 icones_medalha.py <pasta do cliente> <saída> [tamanho=128]
O símbolo de 25 px é suavizado em alta resolução (bordas limpas, sem serrilhado), pintado em
degradê dourado com sombra e posto num disco azul-escuro com aro dourado.
"""
import io, os, re, sys
import numpy as np
from PIL import Image, ImageFilter
from grf import Vfs

def glyph_layers(img):
    """Máscara do símbolo (0..1) e brilho interno (0..1) no tamanho original."""
    a = np.array(img.convert('RGB')).astype(float)
    h, w, _ = a.shape
    trans = (a[..., 0] > 240) & (a[..., 1] < 20) & (a[..., 2] > 240)
    lum = a @ [0.299, 0.587, 0.114]
    pad = np.pad(trans, 1, constant_values=True)
    near = np.zeros_like(trans)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            near |= pad[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
    frame = near & ~trans
    inner = ~trans & ~frame
    bg = np.zeros((h, 3))
    for y in range(h):
        xs = np.where(inner[y])[0]
        if len(xs) >= 6:
            cols = np.concatenate([xs[1:3], xs[-3:-1]])
            bg[y] = np.median(a[y, cols], axis=0)
        elif len(xs):
            bg[y] = np.median(a[y, xs], axis=0)
    diff = np.abs(a - bg[:, None, :]).max(2)
    glyph = inner & (diff > 34)
    glyph[h - 7:, :] = False          # marcas de evolução
    # tira a linha escura interna da moldura e pixels soltos
    fr = np.pad(frame, 1)
    ring2 = np.zeros_like(frame)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            ring2 |= fr[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
    glyph &= ~( ring2 & ( lum < 140 ) )
    gp = np.pad(glyph, 1).astype(int)
    viz = sum(gp[1 + dy:1 + dy + h, 1 + dx:1 + dx + w] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) - glyph
    glyph &= viz >= 2
    # marcas de evolução: quantas (pontos/losangos) na faixa de baixo
    # marcas de evolução: blocos 3x3 coloridos embaixo (1 a 4)
    sat = a.max(2) - a.min(2)
    colored = (sat > 60) & ~trans
    tier = 0
    for yy in range(h - 4, h - 1):
        row = colored[yy]
        runs, x = 0, 0
        while x < w:
            if row[x]:
                x2 = x
                while x2 < w and row[x2]: x2 += 1
                if x2 - x == 3: runs += 1
                x = x2
            else:
                x += 1
        tier = max(tier, runs)
    tier = max(1, min(4, tier))
    bright = np.clip((lum - 40) / 190, 0, 1)
    return glyph.astype(float), bright, tier

def scale2x(a):
    """Scale2x/EPX: dobra a resolução de pixel art arredondando as diagonais sem borrar."""
    h, w = a.shape
    p = np.pad(a, 1, mode='edge')
    B = p[0:h, 1:w + 1]; D = p[1:h + 1, 0:w]; F = p[1:h + 1, 2:w + 2]; H = p[2:h + 2, 1:w + 1]
    E = a
    out = np.empty((h * 2, w * 2), a.dtype)
    c = (B != H) & (D != F)
    out[0::2, 0::2] = np.where(c & (D == B), D, E)
    out[0::2, 1::2] = np.where(c & (B == F), F, E)
    out[1::2, 0::2] = np.where(c & (D == H), D, E)
    out[1::2, 1::2] = np.where(c & (H == F), F, E)
    return out

def smooth_upscale(m, scale, blur):
    im = Image.fromarray((m * 255).astype(np.uint8)).resize((m.shape[1] * scale, m.shape[0] * scale), Image.BICUBIC)
    return np.array(im.filter(ImageFilter.GaussianBlur(blur))).astype(float) / 255

def disc(n, cx, cy, r, soft=1.5):
    y, x = np.mgrid[0:n, 0:n] + 0.5
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    return np.clip((r - d) / soft + 0.5, 0, 1), d

def medalha(img, size=128):
    ss = 4
    n = size * ss
    mask, bright, tier = glyph_layers(img)
    ys, xs = np.where(mask > 0)
    if len(xs) == 0:
        return None
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    m = mask[y0:y1, x0:x1]; b = bright[y0:y1, x0:x1] * m
    # níveis: 0 fundo, 1 traço escuro, 2 médio, 3 claro (o desenho original, sem misturar cores)
    lv = np.where(m > 0, 1 + np.digitize(b / np.maximum(m, 1e-6), [0.38, 0.72]), 0).astype(np.int8)
    lv = np.pad(lv, 2)
    for _ in range(3):
        lv = scale2x(lv)                                   # 8x
    target = n * 0.66
    k = target / max(lv.shape)
    big = Image.fromarray((lv * 85).astype(np.uint8)).resize((max(1, int(lv.shape[1] * k)), max(1, int(lv.shape[0] * k))), Image.NEAREST)
    lvb = np.array(big).astype(float) / 85
    # antisserrilhado: o supersampling (ss=4) e a redução final cuidam das bordas
    alpha = ( lvb > 0.5 ).astype(float)
    detail = np.clip( ( lvb - 1 ) / 2, 0, 1 )
    gh, gw = alpha.shape
    oy, ox = (n - gh) // 2 + int(n * 0.0), (n - gw) // 2
    def place(arr):
        out = np.zeros((n, n))
        sy0, sx0 = max(0, -oy), max(0, -ox)
        dy0, dx0 = max(0, oy), max(0, ox)
        hh, ww = min(gh - sy0, n - dy0), min(gw - sx0, n - dx0)
        out[dy0:dy0 + hh, dx0:dx0 + ww] = arr[sy0:sy0 + hh, sx0:sx0 + ww]
        return out
    ga = place(alpha); gd = place(detail)
    c = n / 2
    base, d = disc(n, c, c, n * 0.47, ss)
    rim_outer = base
    rim_inner, _ = disc(n, c, c, n * 0.435, ss)
    face, _ = disc(n, c, c, n * 0.42, ss)
    rgb = np.zeros((n, n, 3))
    # aro dourado com degradê vertical
    y = (np.mgrid[0:n, 0:n][0] + 0.5) / n
    gold_hi, gold_lo = np.array([255, 226, 140.]), np.array([176, 120, 22.])
    rim = gold_hi * (1 - y[..., None]) + gold_lo * y[..., None]
    rgb[:] = rim
    # anel escuro fino entre aro e face
    rgb = rgb * (1 - (rim_inner - face)[..., None] * 0.85) + np.array([20, 14, 4.]) * ((rim_inner - face)[..., None] * 0.85)
    # face: radial azul
    rr = np.clip(d / (n * 0.42), 0, 1)[..., None]
    navy_c, navy_e = np.array([36, 64, 104.]), np.array([8, 14, 28.])
    face_rgb = navy_c * (1 - rr) + navy_e * rr
    # brilho suave no alto da face
    hl, _ = disc(n, c, c - n * 0.16, n * 0.30, n * 0.12)
    face_rgb = face_rgb + hl[..., None] * np.array([18, 26, 40.])
    rgb = rgb * (1 - face[..., None]) + face_rgb * face[..., None]
    # sombra do símbolo
    sh = np.roll(np.roll(ga, int(n * 0.018), 0), int(n * 0.012), 1)
    sh = np.array(Image.fromarray((sh * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(n * 0.012))).astype(float) / 255
    rgb = rgb * (1 - sh[..., None] * 0.75 * face[..., None])
    # símbolo: degradê dourado claro, detalhes internos mais escuros
    sym_hi, sym_lo = np.array([255, 244, 205.]), np.array([234, 178, 60.])
    sym = sym_hi * (1 - y[..., None]) + sym_lo * y[..., None]
    sym = sym * (0.30 + 0.70 * gd[..., None])
    rgb = rgb * (1 - ga[..., None]) + sym * ga[..., None]
    # marcas de evolução: pontinhos dourados no aro, embaixo
    alpha_all = rim_outer.copy()
    if tier > 0:
        for k in range(tier):
            px = c + (k - (tier - 1) / 2) * n * 0.085
            dot, _ = disc(n, px, n * 0.925, n * 0.03, ss)
            rgb = rgb * (1 - dot[..., None]) + np.array([255, 246, 210.]) * dot[..., None]
    out = np.dstack([np.clip(rgb, 0, 255), alpha_all * 255]).astype(np.uint8)
    return Image.fromarray(out, 'RGBA').resize((size, size), Image.LANCZOS)

def main():
    client, outdir = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 128
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith('.png'): os.remove(os.path.join(outdir, f))
    v = Vfs(client)
    names = {}
    for g in v.grfs:
        for k in g.entries:
            mm = re.search(rb'renewalparty\\icon_jobs_(\d+)\.bmp$', k.lower())
            if mm and int(mm.group(1)) not in names: names[int(mm.group(1))] = k
    ok = 0
    for job, k in names.items():
        im = medalha(Image.open(io.BytesIO(v.read(k))), size)
        if im is not None:
            im.save(os.path.join(outdir, '%d.png' % job)); ok += 1
    print('%d ícones' % ok)

if __name__ == '__main__':
    main()
