"""Tabela das opções aleatórias (runas/encantamentos) para a janela de item.

Ids: db/re/item_randomopt_db.yml do rAthena (mesmos números do enumvar.lub do cliente) e o
db/import, se existir. Textos: addrandomoptionnametable.lub do cliente (GRFs, ordem do DATA.INI).
Saída: dist/charselect/opcoes.txt, linhas "id=ETIQUETA|formato" (formato com %d).

Uso: python3 tools/opcoes.py <pasta do cliente> <pasta do emulador>
"""
import os
import re
import sys

sys.path.insert( 0, os.path.dirname( os.path.abspath( __file__ ) ) )
from grf import Vfs  # noqa: E402


def etiqueta( nome ):
    n = nome.upper()
    if 'SIZE' in n:
        return 'SIZ'
    if n.startswith( 'CLASS_' ):
        return 'CLS'
    if n.startswith( 'RACE_' ) or 'KILLRACE' in n:
        return 'RAC'
    if 'ATTR' in n or 'PROPERTY' in n or n.startswith( 'ADDSKILLMDAMAGE' ):
        return 'ELE'
    if 'DEFPOWER' in n or 'MDEF' in n or n.endswith( 'RESAMOUNT' ):
        return 'DEF'
    if 'MAGICATK' in n or 'ATTMPOWER' in n or 'SMATK' in n:
        return 'MAT'
    if 'ATK' in n or 'ATTPOWER' in n:
        return 'ATK'
    if 'HP' in n or 'SP' in n and 'ASPD' not in n and 'SPELL' not in n and 'SP_CONSUMPTION' not in n:
        return 'HP' if 'HP' in n else 'SP'
    if 'ASPD' in n:
        return 'SPD'
    if 'CRI' in n:
        return 'CRI'
    if 'HIT' in n:
        return 'HIT'
    if 'AVOID' in n:
        return 'FLE'
    if 'SPELL' in n or 'CONSUMPTION' in n:
        return 'CST'
    if 'HEAL' in n:
        return 'CUR'
    if 'EXP' in n:
        return 'EXP'
    if re.match( r'VAR_(STR|AGI|VIT|INT|DEX|LUK|POW|SPL|STA|WIS|CON|CRT)AMOUNT', n ):
        return 'ATR'
    return 'ESP'


def main():
    client, emu = sys.argv[1], sys.argv[2]
    ids = {}
    for rel in ( 'db/re/item_randomopt_db.yml', 'db/import/item_randomopt_db.yml' ):
        p = os.path.join( emu, rel )
        if os.path.isfile( p ):
            for i, n in re.findall( r'-\s*Id:\s*(\d+)\s+Option:\s*(\w+)', open( p, encoding='utf-8' ).read() ):
                ids[int( i )] = n
    vfs = Vfs( client )
    textos = {}
    try:
        lub = vfs.read( 'data\\luafiles514\\lua files\\datainfo\\addrandomoptionnametable.lub' )
        if not lub.startswith( b'\x1bLua' ):
            for nome, fmt in re.findall( rb'\[EnumVAR\.(\w+)\[1\]\]\s*=\s*"((?:[^"\\]|\\.)*)"', lub ):
                textos[nome.decode()] = fmt.decode( 'cp1252' )
    except KeyError:
        pass
    linhas = []
    for i in sorted( ids ):
        n = ids[i]
        fmt = textos.get( n, n.replace( '_', ' ' ).title() + ' %d' )
        linhas.append( '%d=%s|%s' % ( i, etiqueta( n ), fmt.replace( '%%', '%' ) ) )
    dest = os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'opcoes.txt' )
    open( dest, 'w', encoding='utf-8' ).write( '\n'.join( linhas ) + '\n' )
    print( 'opcoes: %d (textos do cliente: %d)' % ( len( linhas ), len( textos ) ) )


if __name__ == '__main__':
    main()
