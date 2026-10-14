"""Baixa as imagens UIRenewal (interface do Ragnarok Zero) dos patches oficiais do kRO Zero.

Uso: python3 zero_patches.py <pasta de saída> [filtro]
  filtro = trecho do caminho (padrão "uirenewal")

Os patches (.gpf, formato GRF 0x102/0x200) ficam no arquivo público usado pelo RO Patcher Lite.
Só o índice de cada patch e as entradas pedidas são baixados (HTTP Range); vale a versão mais recente.
"""
import json, os, re, struct, sys, urllib.request, zlib
from concurrent.futures import ThreadPoolExecutor

sys.path.insert( 0, os.path.dirname( __file__ ) )
import grfdes

LISTA = 'https://nn.ai4rei.net/dev/rsu/patch/mirrorfiles-kro-zero.json'
MIRROR = 'http://ro.archive.aotsw.com/mirror/kro/ZeroPatch/'


def faixa( url, a, b=None ):
    req = urllib.request.Request( url, headers={ 'Range': 'bytes=%d-%s' % ( a, '' if b is None else b ), 'User-Agent': 'Mozilla/5.0' } )
    return urllib.request.urlopen( req, timeout=120 ).read()


def nome_102( raw ):
    out = bytearray()
    for i in range( 0, len( raw ) // 8 * 8, 8 ):
        out += grfdes._decrypt_block( bytes( ( ( c << 4 ) | ( c >> 4 ) ) & 0xFF for c in raw[i:i + 8] ) )
    return bytes( out ).split( b'\0' )[0]


def indice( url ):
    h = faixa( url, 0, 45 )
    off, seed, cnt, ver = struct.unpack_from( '<IIII', h, 30 )
    t = faixa( url, off + 46 )
    ents = {}
    if ver >> 8 == 2:
        c = struct.unpack_from( '<I', t, 0 )[0]
        tb = zlib.decompress( t[8:8 + c] )
        p = 0
        while p < len( tb ):
            e = tb.index( b'\0', p ); nm = tb[p:e]; p = e + 1
            cl, cla, rl, fl, of = struct.unpack_from( '<IIIBI', tb, p ); p += 17
            if fl & 1: ents[nm.lower()] = ( cl, cla, rl, fl, of )
    else:
        p = 0
        for _ in range( cnt - seed - 7 ):
            n = struct.unpack_from( '<I', t, p )[0]; p2 = p + n + 4
            fl = t[p2 + 12]
            if fl:
                nm = nome_102( t[p + 6:p + 4 + n] )
                a, b, rl = struct.unpack_from( '<III', t, p2 ); of = struct.unpack_from( '<I', t, p2 + 13 )[0]
                ext = nm.lower().rsplit( b'.', 1 )[-1]
                fl |= 4 if ext in ( b'gnd', b'gat', b'act', b'str' ) else 2
                ents[nm.lower()] = ( a - rl - 0x02CB, b - 0x92CB, rl, fl, of )
            p = p2 + 17
    return ents


def ler( url, e ):
    cl, cla, rl, fl, of = e
    d = faixa( url, of + 46, of + 46 + cla - 1 )
    if fl & 6: d = grfdes.decode( d, fl, cl )
    return zlib.decompress( d[:cl] )


def main():
    saida = sys.argv[1]
    filtro = ( sys.argv[2] if len( sys.argv ) > 2 else 'uirenewal' ).lower().encode()
    lista = json.loads( urllib.request.urlopen( LISTA, timeout=60 ).read() )
    def data( k ):
        m = re.search( r'\d{4}-\d{2}-\d{2}', k )
        return m.group( 0 ) if m else '0000'
    patches = sorted( ( k for k in lista if k.endswith( '.gpf' ) ), key=lambda k: ( data( k ), k ) )

    def um( f ):
        try: return f, { k: v for k, v in indice( MIRROR + f ).items() if filtro in k }
        except Exception: return f, {}
    recente = {}
    with ThreadPoolExecutor( 6 ) as ex:
        for f, ents in ex.map( um, patches ):
            for k, v in ents.items(): recente[k] = ( f, v )

    def baixar( item ):
        k, ( f, e ) = item
        rel = k.decode( 'cp949' ).split( filtro.decode() + '\\', 1 )[-1].replace( '\\', '/' )
        p = os.path.join( saida, rel )
        os.makedirs( os.path.dirname( p ), exist_ok=True )
        with open( p, 'wb' ) as o: o.write( ler( MIRROR + f, e ) )
    with ThreadPoolExecutor( 8 ) as ex:
        list( ex.map( baixar, recente.items() ) )
    print( 'OK: %d arquivos em %s' % ( len( recente ), saida ) )


if __name__ == '__main__':
    main()
