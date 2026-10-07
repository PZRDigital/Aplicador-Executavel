"""Ícones de classe do cliente (renewalparty\\icon_jobs_<id>.bmp, os mesmos da bROWiki) recoloridos no tema O Codex.

Uso: python3 icones_codex.py <pasta do cliente> <saída> [escala=4]
Fundo azul-escuro em degradê, moldura e marcas de evolução douradas, símbolo claro com contorno escuro.
"""
import io, os, re, sys
import numpy as np
from PIL import Image
from grf import Vfs

NAVY_TOP = np.array([0x1A, 0x2E, 0x4C], float)
NAVY_BOT = np.array([0x07, 0x0D, 0x18], float)
GOLD = np.array([0xF0, 0xBD, 0x45], float)
GOLD_DARK = np.array([0x8A, 0x62, 0x10], float)
LIGHT = np.array([0xFF, 0xF4, 0xDA], float)
INK = np.array([0x05, 0x09, 0x12], float)

def recolor(img):
    a = np.array(img.convert('RGB')).astype(float)
    h, w, _ = a.shape
    trans = (a[..., 0] > 240) & (a[..., 1] < 20) & (a[..., 2] > 240)
    lum = a @ [0.299, 0.587, 0.114]
    # moldura: pixels opacos vizinhos de transparente ou da borda da imagem
    pad = np.pad(trans, 1, constant_values=True)
    near = np.zeros_like(trans)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            near |= pad[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
    frame = near & ~trans
    # fundo de cada linha: mediana das colunas logo dentro da moldura
    inner = ~trans & ~frame
    bg = np.zeros((h, 3))
    for y in range(h):
        xs = np.where(inner[y])[0]
        if len(xs) >= 6:
            # pula a linha escura interna da moldura original
            cols = np.concatenate([xs[1:3], xs[-3:-1]])
            bg[y] = np.median(a[y, cols], axis=0)
        elif len(xs):
            bg[y] = np.median(a[y, xs], axis=0)
    diff = np.abs(a - bg[:, None, :]).max(2)
    glyph = inner & (diff > 34)
    # tira pixels soltos (ruído do degradê original): precisa de ao menos 2 vizinhos do símbolo
    gp = np.pad(glyph, 1).astype(int)
    viz = sum(gp[1 + dy:1 + dy + h, 1 + dx:1 + dx + w] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) - glyph
    glyph &= viz >= 2
    # linha escura interna da moldura original: vira parte da moldura (dourado escuro)
    pad2 = np.pad(frame, 1, constant_values=False)
    ring2 = np.zeros_like(frame)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            ring2 |= pad2[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
    ring2 &= inner & (lum < 140)
    # faixa de baixo (marcas de evolução): tudo que não é fundo vira dourado
    marks_row = np.zeros_like(trans); marks_row[h - 7:, :] = True
    out = np.zeros((h, w, 4))
    t = np.linspace(0, 1, h)[:, None, None]
    grad = NAVY_TOP * (1 - t) + NAVY_BOT * t
    out[..., :3] = np.broadcast_to(grad, (h, w, 3))
    out[..., 3] = 255
    k = np.clip((lum - 40) / 190, 0, 1)[..., None]
    sym = INK * (1 - k) + LIGHT * k
    gm = glyph & ~marks_row
    out[gm, :3] = sym[gm]
    mk = glyph & marks_row
    out[mk, :3] = (GOLD_DARK * (1 - k) + GOLD * k)[mk]
    out[ring2, :3] = GOLD_DARK
    out[frame, :3] = GOLD
    out[trans] = 0
    return Image.fromarray(out.astype(np.uint8), 'RGBA')

def main():
    client, outdir = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith('.png'): os.remove(os.path.join(outdir, f))
    v = Vfs(client)
    names = {}
    for g in v.grfs:
        for k in g.entries:
            m = re.search(rb'renewalparty\\icon_jobs_(\d+)\.bmp$', k.lower())
            if m and int(m.group(1)) not in names: names[int(m.group(1))] = k
    for job, k in names.items():
        im = recolor(Image.open(io.BytesIO(v.read(k))))
        # pixel art: amplia sem suavizar (o jogo reduz com filtro linear)
        im.resize((im.width * scale, im.height * scale), Image.NEAREST).save(os.path.join(outdir, '%d.png' % job))
    print('%d ícones' % len(names))

if __name__ == '__main__':
    main()
