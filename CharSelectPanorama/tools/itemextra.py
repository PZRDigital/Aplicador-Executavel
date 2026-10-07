"""Dados extras da janela de item, tirados da db do rAthena (re + import):
  - quem dropa o item e a chance (mob_db Drops/MvpDrops);
  - preço de referência (item_db Buy; sem Buy = Sell * 2);
  - informações: refinável, indestrutível (pela descrição não dá), restrições de troca.

Saída: dist/charselect/itemextra.txt, uma linha por item:
  id=preco|flags|mob:chance,mob:chance,...
  flags: R = refinável, T = sem troca, V = sem venda, D = sem drop, A = sem armazém, G = sem armazém da guilda
  chance em centésimos de % (rate do rAthena: 10000 = 100%)
E dist/charselect/monstros.txt: id=nome (para os nomes dos que dropam).

Uso: python3 tools/itemextra.py <pasta do emulador>
"""
import os
import sys

import yaml

LOADER = getattr( yaml, 'CSafeLoader', yaml.SafeLoader )


def body( path ):
    if not os.path.isfile( path ):
        return []
    with open( path, encoding='utf-8' ) as f:
        doc = yaml.load( f, Loader=LOADER ) or {}
    return doc.get( 'Body' ) or []


def main():
    emu = sys.argv[1]
    db = os.path.join( emu, 'db' )
    items = {}   # id -> dict
    aegis = {}   # AegisName (minúsculo) -> id
    for name in ( 'item_db_equip.yml', 'item_db_usable.yml', 'item_db_etc.yml' ):
        for it in body( os.path.join( db, 're', name ) ):
            items[it['Id']] = it
    for it in body( os.path.join( db, 'import', 'item_db.yml' ) ):
        base = items.get( it['Id'], {} )
        base.update( it )
        items[it['Id']] = base
    for i, it in items.items():
        if it.get( 'AegisName' ):
            aegis[str( it['AegisName'] ).lower()] = i

    mobs = {}
    for src in ( os.path.join( db, 're', 'mob_db.yml' ), os.path.join( db, 'import', 'mob_db.yml' ) ):
        for m in body( src ):
            base = mobs.get( m['Id'], {} )
            base.update( m )
            mobs[m['Id']] = base

    drops = {}  # item id -> [(rate, mob id)]
    for mid, m in mobs.items():
        for key in ( 'Drops', 'MvpDrops' ):
            for d in m.get( key ) or []:
                iid = aegis.get( str( d.get( 'Item', '' ) ).lower() )
                if iid is None:
                    continue
                drops.setdefault( iid, [] ).append( ( int( d.get( 'Rate', 0 ) ), mid ) )

    out = []
    usados = set()
    for iid in sorted( items ):
        it = items[iid]
        price = int( it.get( 'Buy', 0 ) or 0 ) or int( it.get( 'Sell', 0 ) or 0 ) * 2
        flags = ''
        if it.get( 'Refineable' ):
            flags += 'R'
        tr = it.get( 'Trade' ) or {}
        if tr.get( 'NoTrade' ):
            flags += 'T'
        if tr.get( 'NoSell' ):
            flags += 'V'
        if tr.get( 'NoDrop' ):
            flags += 'D'
        if tr.get( 'NoStorage' ):
            flags += 'A'
        if tr.get( 'NoGuildStorage' ):
            flags += 'G'
        lst = sorted( drops.get( iid, [] ), reverse=True )
        seen = set()
        parts = []
        for rate, mid in lst:
            if mid in seen:
                continue
            seen.add( mid )
            parts.append( '%d:%d' % ( mid, rate ) )
            usados.add( mid )
            if len( parts ) == 6:
                break
        if price or flags or parts:
            out.append( '%d=%d|%s|%s' % ( iid, price, flags, ','.join( parts ) ) )
    base = os.path.join( os.path.dirname( os.path.abspath( __file__ ) ), '..', 'dist', 'charselect' )
    open( os.path.join( base, 'itemextra.txt' ), 'w', encoding='utf-8' ).write( '\n'.join( out ) + '\n' )
    nomes = [ '%d=%s' % ( mid, mobs[mid].get( 'Name', mobs[mid].get( 'AegisName', '' ) ) ) for mid in sorted( usados ) ]
    open( os.path.join( base, 'monstros.txt' ), 'w', encoding='utf-8' ).write( '\n'.join( nomes ) + '\n' )
    print( 'itemextra: %d itens, %d monstros' % ( len( out ), len( nomes ) ) )


if __name__ == '__main__':
    main()
