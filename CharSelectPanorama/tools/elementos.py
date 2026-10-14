"""Elementos dos pets para a Pokédex (fraquezas e forças).

Uso: python3 elementos.py <pasta do emulador> [saída]
  db/re/mob_db.yml + db/import/mob_db.yml (Element, ElementLevel) e db/re/attr_fix.yml + import (tabela de dano
  por elemento) viram <saída>\\elementos.txt (padrão dist/charselect):
    attr<nível>=<atacante>:<10 valores, um por elemento do alvo>   (ordem: Neutro, Água, Terra, Fogo, Vento,
                                                                    Veneno, Sagrado, Sombrio, Fantasma, Maldito)
    <classe>=<elemento 0-9>,<nível 1-4>                             (só monstros fora de Neutro 1)
"""
import os, sys
import yaml

ELEMENTOS = [ 'Neutral', 'Water', 'Earth', 'Fire', 'Wind', 'Poison', 'Holy', 'Dark', 'Ghost', 'Undead' ]


def corpo( caminho ):
    if not os.path.exists( caminho ): return []
    dados = yaml.safe_load( open( caminho, encoding='utf-8', errors='replace' ) ) or {}
    return dados.get( 'Body' ) or []


def main():
    emu = sys.argv[1]
    saida = sys.argv[2] if len( sys.argv ) > 2 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect' )
    attr = { lv: { a: [ 100 ] * 10 for a in ELEMENTOS } for lv in range( 1, 5 ) }
    for sub in ( 're', 'import' ):
        for e in corpo( os.path.join( emu, 'db', sub, 'attr_fix.yml' ) ):
            lv = e.get( 'Level' )
            if lv not in attr: continue
            for a, alvos in e.items():
                if a not in attr[lv] or not isinstance( alvos, dict ): continue
                for d, v in alvos.items():
                    if d in ELEMENTOS: attr[lv][a][ELEMENTOS.index( d )] = int( v )
    mobs = {}
    for sub in ( 're', 'import' ):
        for m in corpo( os.path.join( emu, 'db', sub, 'mob_db.yml' ) ):
            if 'Id' not in m: continue
            ele = ELEMENTOS.index( m['Element'] ) if m.get( 'Element' ) in ELEMENTOS else 0
            mobs[int( m['Id'] )] = ( ele, int( m.get( 'ElementLevel', 1 ) ) )
    with open( os.path.join( saida, 'elementos.txt' ), 'w', encoding='utf-8', newline='\n' ) as f:
        f.write( '; attr<nível do alvo>=<elemento do atacante>:<dano %% por elemento do alvo>; <classe>=<elemento>,<nível>\n' )
        for lv in range( 1, 5 ):
            for i, a in enumerate( ELEMENTOS ):
                f.write( 'attr%d=%d:%s\n' % ( lv, i, ','.join( str( v ) for v in attr[lv][a] ) ) )
        n = 0
        for k in sorted( mobs ):
            if mobs[k] != ( 0, 1 ):
                f.write( '%d=%d,%d\n' % ( k, mobs[k][0], mobs[k][1] ) ); n += 1
    print( 'elementos: %d monstros com elemento' % n )


if __name__ == '__main__':
    main()
