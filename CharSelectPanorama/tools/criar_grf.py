"""Empacota a pasta charselect\\ numa GRF (0x200, zlib), com os arquivos em data\\charselect\\.

Uso: python3 criar_grf.py <pasta charselect> <saída .grf>
Arquivos do jogador (favoritos, preferências, log, capturas) não entram: ficam em savedata\\charselect\\.
"""
import os, struct, sys, zlib

IGNORAR_PASTAS = {'testes'}
IGNORAR_ARQ = {'favoritos.txt', 'preferencias.ini'}
IGNORAR_EXT = ('.log', '.bak')

def main():
    src, dst = sys.argv[1], sys.argv[2]
    files = []
    for root, dirs, names in os.walk(src):
        dirs[:] = [d for d in dirs if d.lower() not in IGNORAR_PASTAS]
        for n in sorted(names):
            if n.lower() in IGNORAR_ARQ or n.lower().endswith(IGNORAR_EXT): continue
            full = os.path.join(root, n)
            rel = os.path.relpath(full, src).replace('/', '\\')
            files.append((rel, full))
    body = bytearray(); table = bytearray()
    for rel, full in files:
        raw = open(full, 'rb').read()
        comp = zlib.compress(raw, 9)
        aligned = len(comp)
        name = ('data\\charselect\\' + rel).encode('cp949')
        table += name + b'\0' + struct.pack('<IIIBI', len(comp), aligned, len(raw), 1, len(body))
        body += comp
    tcomp = zlib.compress(bytes(table), 9)
    header = b'Master of Magic\0' + bytes(range(1, 15)) + struct.pack('<IIII', len(body), 0, len(files) + 7, 0x200)
    with open(dst, 'wb') as f:
        f.write(header); f.write(body)
        f.write(struct.pack('<II', len(tcomp), len(table))); f.write(tcomp)
    print('%s: %d arquivos, %.1f MB' % (dst, len(files), os.path.getsize(dst) / 1048576))

if __name__ == '__main__':
    main()
