"""Gera os ícones de classe (charselect\\icones\\<id>.png): um símbolo por família, branco com alfa.

Uso: python3 icones.py <src/common/mmo.hpp do emulador> <saída> [fonte .ttf]
A fonte padrão é a Segoe UI Symbol do Windows (C:\\Windows\\Fonts\\seguisym.ttf).
"""
import os, re, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

# (palavras no nome da classe, símbolo, engrossar) — a primeira que casar vale
FAMILIAS = [
    (('SUPER_NOVICE', 'SUPER_BABY', 'HYPER_NOVICE'), '★', 0),
    (('CRUSADER', 'PALADIN', 'ROYAL_GUARD', 'IMPERIAL_GUARD'), '\U0001F6E1', 4),
    (('DEATH_KNIGHT', 'DARK_COLLECTOR', 'GANGSI'), '☠', 0),
    (('SWORDMAN', 'KNIGHT'), '⚔', 6),
    (('SAGE', 'PROFESSOR', 'SORCERER', 'ELEMENTAL_MASTER'), '\U0001F4D6', 0),
    (('WIZARD', 'WARLOCK', 'ARCH_MAGE'), '\U0001F525', 0),
    (('MAGE',), '\U0001F52E', 0),
    (('BARD', 'CLOWN', 'MINSTREL', 'TROUBADOUR', 'DANCER', 'GYPSY', 'WANDERER', 'TROUVERE'), '♫', 3),
    (('ARCHER', 'HUNTER', 'SNIPER', 'RANGER', 'WINDHAWK'), '\U0001F3F9', 2),
    (('MONK', 'CHAMPION', 'SURA', 'INQUISITOR'), '✊', 0),
    (('ACOLYTE', 'PRIEST', 'BISHOP', 'CARDINAL'), '✝', 7),
    (('BLACKSMITH', 'WHITESMITH', 'MECHANIC', 'MEISTER'), '\U0001F528', 0),
    (('ALCHEMIST', 'CREATOR', 'GENETIC', 'BIOLO'), '⚗', 2),
    (('MERCHANT',), '\U0001F4B0', 0),
    (('ROGUE', 'STALKER', 'SHADOW_CHASER', 'ABYSS_CHASER'), '\U0001F3AD', 0),
    (('THIEF', 'ASSASSIN', 'GUILLOTINE', 'SHADOW_CROSS'), '\U0001F5E1', 6),
    (('STAR_GLADIATOR', 'STAR_EMPEROR', 'SKY_EMPEROR'), '☀', 0),
    (('SOUL_LINKER', 'SOUL_REAPER', 'SOUL_ASCETIC'), '\U0001F47B', 0),
    (('TAEKWON',), '☯', 0),
    (('GUNSLINGER', 'REBELLION', 'NIGHT_WATCH'), '\U0001F52B', 0),
    (('NINJA', 'KAGEROU', 'OBORO', 'SHINKIRO', 'SHIRANUI'), '✴', 2),
    (('SUMMONER', 'SPIRIT_HANDLER'), '\U0001F43E', 0),
]
PADRAO = ('\U0001F331', 0)  # aprendiz e roupas especiais

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

def simbolo(nome):
    for chaves, ch, grosso in FAMILIAS:
        if any(k in nome for k in chaves):
            return ch, grosso
    return PADRAO

def desenhar(ch, grosso, fonte, n=128):
    big = n * 4
    img = Image.new('L', (big, big), 0)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(fonte, int(big * 0.62))
    sw = grosso * 4 // 3
    bb = d.textbbox((0, 0), ch, font=f, stroke_width=sw)
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    d.text(((big - w) / 2 - bb[0], (big - h) / 2 - bb[1]), ch, font=f, fill=255, stroke_width=sw, stroke_fill=255)
    a = np.array(img.resize((n, n), Image.LANCZOS))
    rgba = np.zeros((n, n, 4), np.uint8); rgba[..., :3] = 255; rgba[..., 3] = a
    return Image.fromarray(rgba, 'RGBA')

def main():
    mmo, out = sys.argv[1], sys.argv[2]
    fonte = sys.argv[3] if len(sys.argv) > 3 else '/mnt/c/Windows/Fonts/seguisym.ttf'
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith('.png'): os.remove(os.path.join(out, f))
    cache = {}
    for nome, jid in jobs(mmo).items():
        if nome in ('JOB_MAX', 'JOB_MAX_BASIC') or 'START' in nome or 'END' in nome: continue
        key = simbolo(nome)
        if key not in cache: cache[key] = desenhar(key[0], key[1], fonte)
        cache[key].save(os.path.join(out, '%d.png' % jid))
    print('%d ícones, %d símbolos' % (len(os.listdir(out)), len(cache)))

if __name__ == '__main__':
    main()
