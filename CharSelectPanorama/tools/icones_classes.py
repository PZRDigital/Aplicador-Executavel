"""Ícones de classe no tema O Codex: arte vetorial de RPG (game-icons.net, CC BY 3.0) em medalhão.

Uso: python3 icones_classes.py <src/common/mmo.hpp do emulador> <saída> [tamanho=128]
Um desenho por família de classe (tools/arte_classes/*.svg); pontinhos no aro = nível da classe.
"""
import io, os, re, sys
import numpy as np
import cairosvg
from PIL import Image, ImageFilter

ARTE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'arte_classes')

# (palavras no nome da classe, desenho) — a primeira que casar vale
FAMILIAS = [
    (('SUPER_NOVICE', 'SUPER_BABY', 'HYPER_NOVICE'), 'delapouite_falling-star'),
    (('CRUSADER', 'PALADIN', 'ROYAL_GUARD', 'IMPERIAL_GUARD'), 'delapouite_cross-shield'),
    (('DEATH_KNIGHT', 'DARK_COLLECTOR', 'GANGSI'), 'lorc_broken-skull'),
    (('SWORDMAN', 'KNIGHT'), 'lorc_crossed-swords'),
    (('SAGE', 'PROFESSOR', 'SORCERER', 'ELEMENTAL_MASTER'), 'delapouite_spell-book'),
    (('WIZARD', 'WARLOCK', 'ARCH_MAGE'), 'delapouite_fire-spell-cast'),
    (('MAGE',), 'lorc_wizard-staff'),
    (('BARD', 'CLOWN', 'MINSTREL', 'TROUBADOUR', 'DANCER', 'GYPSY', 'WANDERER', 'TROUVERE'), 'lorc_lyre'),
    (('ARCHER', 'HUNTER', 'SNIPER', 'RANGER', 'WINDHAWK'), 'lorc_pocket-bow'),
    (('MONK', 'CHAMPION', 'SURA', 'INQUISITOR'), 'lorc_fist'),
    (('ACOLYTE', 'PRIEST', 'BISHOP', 'CARDINAL'), 'lorc_holy-symbol'),
    (('BLACKSMITH', 'WHITESMITH', 'MECHANIC', 'MEISTER'), 'lorc_anvil'),
    (('ALCHEMIST', 'CREATOR', 'GENETIC', 'BIOLO'), 'lorc_round-bottom-flask'),
    (('MERCHANT',), 'delapouite_coins'),
    (('ROGUE', 'STALKER', 'SHADOW_CHASER', 'ABYSS_CHASER'), 'delapouite_robber-mask'),
    (('ASSASSIN', 'GUILLOTINE', 'SHADOW_CROSS'), 'lorc_daggers'),
    (('THIEF',), 'lorc_hood'),
    (('STAR_GLADIATOR', 'STAR_EMPEROR', 'SKY_EMPEROR'), 'delapouite_striped-sun'),
    (('SOUL_LINKER', 'SOUL_REAPER', 'SOUL_ASCETIC'), 'lorc_ghost'),
    (('TAEKWON',), 'delapouite_high-kick'),
    (('GUNSLINGER', 'REBELLION', 'NIGHT_WATCH'), 'skoll_revolver'),
    (('NINJA', 'KAGEROU', 'OBORO', 'SHINKIRO', 'SHIRANUI'), 'faithtoken_ninja-star'),
    (('SUMMONER', 'SPIRIT_HANDLER'), 'lorc_paw'),
]
PADRAO = 'lorc_sprout'

NIVEL = [
    (4, ('DRAGON_KNIGHT', 'MEISTER', 'SHADOW_CROSS', 'ARCH_MAGE', 'CARDINAL', 'WINDHAWK', 'IMPERIAL_GUARD', 'BIOLO',
         'ABYSS_CHASER', 'ELEMENTAL_MASTER', 'INQUISITOR', 'TROUBADOUR', 'TROUVERE', 'SKY_EMPEROR', 'SOUL_ASCETIC',
         'SHINKIRO', 'SHIRANUI', 'NIGHT_WATCH', 'HYPER_NOVICE', 'SPIRIT_HANDLER')),
    (3, ('RUNE_KNIGHT', 'WARLOCK', 'RANGER', 'ARCH_BISHOP', 'ARCHBISHOP', 'MECHANIC', 'GUILLOTINE', 'ROYAL_GUARD', 'SORCERER',
         'MINSTREL', 'WANDERER', 'SURA', 'GENETIC', 'SHADOW_CHASER', 'STAR_EMPEROR', 'SOUL_REAPER', 'SUPER_NOVICE_E', 'SUPER_BABY_E')),
    (2, ('KNIGHT', 'PRIEST', 'WIZARD', 'BLACKSMITH', 'WHITESMITH', 'HUNTER', 'SNIPER', 'ASSASSIN', 'CRUSADER', 'PALADIN', 'MONK',
         'CHAMPION', 'SAGE', 'PROFESSOR', 'ROGUE', 'STALKER', 'ALCHEMIST', 'CREATOR', 'BARD', 'CLOWN', 'DANCER', 'GYPSY',
         'STAR_GLADIATOR', 'SOUL_LINKER', 'KAGEROU', 'OBORO', 'REBELLION', 'DEATH_KNIGHT', 'DARK_COLLECTOR')),
    (1, ('SWORDMAN', 'MAGE', 'ARCHER', 'ACOLYTE', 'MERCHANT', 'THIEF', 'TAEKWON', 'GUNSLINGER', 'NINJA', 'SUMMONER')),
]

def jobs(mmo):
    src = open(mmo, encoding='utf-8', errors='replace').read()
    i = src.index('JOB_NOVICE,')
    body = src[src.rfind('{', 0, i):src.index('}', i)]
    out, v = {}, -1
    for line in body.split('\n'):
        m = re.match(r'\s*(JOB_\w+)\s*(?:=\s*(\d+))?\s*,', line)
        if m:
            v = int(m.group(2)) if m.group(2) else v + 1
            out[m.group(1)] = v
    return out

def familia(nome):
    for chaves, arte in FAMILIAS:
        if any(k in nome for k in chaves): return arte
    return PADRAO

def nivel(nome):
    for n, chaves in NIVEL:
        if any(k in nome for k in chaves): return n
    return 0

def mascara(arte, n):
    """Desenho branco do SVG como máscara 0..1 (o fundo preto do game-icons é removido)."""
    svg = open(os.path.join(ARTE, arte + '.svg'), encoding='utf-8').read()
    svg = svg.replace('<path d="M0 0h512v512H0z"/>', '')
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=n, output_height=n)
    im = Image.open(io.BytesIO(png)).convert('RGBA')
    a = np.array(im).astype(float)
    return a[..., 3] / 255 * ( a[..., :3].mean(2) / 255 )

def disc(n, cx, cy, r, soft):
    y, x = np.mgrid[0:n, 0:n] + 0.5
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    return np.clip((r - d) / soft + 0.5, 0, 1), d

def blur(a, r):
    return np.array(Image.fromarray(np.clip(a * 255, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(r))).astype(float) / 255

def medalhao(arte, tier, size=128):
    ss = 4
    n = size * ss
    c = n / 2
    yy = (np.mgrid[0:n, 0:n][0] + 0.5) / n
    base, d = disc(n, c, c, n * 0.47, ss)
    rim_inner, _ = disc(n, c, c, n * 0.435, ss)
    face, _ = disc(n, c, c, n * 0.42, ss)
    # aro dourado
    gold_hi, gold_lo = np.array([255, 226, 140.]), np.array([176, 120, 22.])
    rgb = gold_hi * (1 - yy[..., None]) + gold_lo * yy[..., None]
    ring = (rim_inner - face)[..., None] * 0.85
    rgb = rgb * (1 - ring) + np.array([20, 14, 4.]) * ring
    # face azul radial com brilho no alto
    rr = np.clip(d / (n * 0.42), 0, 1)[..., None]
    face_rgb = np.array([36, 64, 104.]) * (1 - rr) + np.array([8, 14, 28.]) * rr
    hl, _ = disc(n, c, c - n * 0.16, n * 0.30, n * 0.12)
    face_rgb = face_rgb + hl[..., None] * np.array([18, 26, 40.])
    rgb = rgb * (1 - face[..., None]) + face_rgb * face[..., None]
    # desenho: 58% do diâmetro, centrado
    gs = int(n * 0.58)
    m = mascara(arte, gs)
    ga = np.zeros((n, n)); o = (n - gs) // 2
    ga[o:o + gs, o:o + gs] = m
    ga *= face
    # sombra projetada
    sh = blur(np.roll(np.roll(ga, int(n * 0.02), 0), int(n * 0.012), 1), n * 0.014)
    rgb = rgb * (1 - sh[..., None] * 0.8 * face[..., None])
    # relevo: luz de cima/esquerda pela inclinação da máscara suavizada
    bm = blur(ga, n * 0.006)
    gy, gx = np.gradient(bm)
    light = np.clip(0.5 - (gx * -0.6 + gy * -0.8) * n * 0.012, 0, 1)
    sym = np.array([255, 244, 205.]) * (1 - yy[..., None]) + np.array([228, 168, 52.]) * yy[..., None]
    sym = sym * (0.72 + 0.5 * light[..., None])
    rgb = rgb * (1 - ga[..., None]) + np.clip(sym, 0, 255) * ga[..., None]
    # nível: pontinhos no aro, embaixo
    for k in range(tier):
        px = c + (k - (tier - 1) / 2) * n * 0.085
        dot, _ = disc(n, px, n * 0.925, n * 0.03, ss)
        rgb = rgb * (1 - dot[..., None]) + np.array([255, 246, 210.]) * dot[..., None]
    out = np.dstack([np.clip(rgb, 0, 255), base * 255]).astype(np.uint8)
    return Image.fromarray(out, 'RGBA').resize((size, size), Image.LANCZOS)

def main():
    mmo, outdir = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 128
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith('.png'): os.remove(os.path.join(outdir, f))
    cache = {}
    total = 0
    for nome, jid in jobs(mmo).items():
        if nome in ('JOB_MAX', 'JOB_MAX_BASIC') or 'START' in nome or 'END' in nome: continue
        key = (familia(nome), nivel(nome))
        if key not in cache: cache[key] = medalhao(key[0], key[1], size)
        cache[key].save(os.path.join(outdir, '%d.png' % jid)); total += 1
    print('%d ícones, %d desenhos' % (total, len(cache)))

if __name__ == '__main__':
    main()
