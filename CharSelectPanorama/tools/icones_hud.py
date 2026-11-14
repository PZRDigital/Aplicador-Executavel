"""Ícones de classe do cartão de HP e os de falcão/montaria da janela Personagem.

Classes: os nativos do cliente (renewalparty\\icon_jobs_<classe>.bmp, 25x25, os da
janela de grupo), com o magenta transparente e ampliados 4x sem suavizar (a DLL reduz para o tamanho da caixa).

Uso: python3 icones_hud.py <pasta do cliente> [pasta charselect]   (grava <charselect>/img/hud_job/<classe>.png)
"""
import io, os, re, sys
from PIL import Image
from grf import Vfs

PASTA = b'data\\texture\\\xc0\xaf\xc0\xfa\xc0\xce\xc5\xcd\xc6\xe4\xc0\xcc\xbd\xba\\renewalparty\\'  # 유저인터페이스 (cp949)


def main():
    cliente = sys.argv[1]
    base = sys.argv[2] if len( sys.argv ) > 2 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect' )
    dst = os.path.join( base, 'img', 'hud_job' )
    os.makedirs( dst, exist_ok=True )
    for f in os.listdir( dst ):
        os.remove( os.path.join( dst, f ) )
    v = Vfs( cliente )
    nomes = set()
    for g in v.grfs:
        for k in g.entries:
            m = re.match( re.escape( PASTA ) + rb'icon_jobs_(\d+)\.bmp$', k )
            if m:
                nomes.add( int( m.group( 1 ) ) )
    n = 0
    for job in sorted( nomes ):
        im = Image.open( io.BytesIO( v.read( PASTA + b'icon_jobs_%d.bmp' % job ) ) ).convert( 'RGBA' )
        px = im.load()
        for y in range( im.height ):
            for x in range( im.width ):
                r, g, b, a = px[x, y]
                if r > 230 and g < 30 and b > 230:
                    px[x, y] = ( 0, 0, 0, 0 )
        im.resize( ( im.width * 4, im.height * 4 ), Image.NEAREST ).save( os.path.join( dst, '%d.png' % job ) )
        n += 1
    print( 'ícones do cartão de HP: %d (nativos) em %s' % ( n, os.path.normpath( dst ) ) )
    # ícones do encaixe de falcão/montaria da janela Personagem (os de status do próprio cliente)
    for nome, arq in ( ( 'montaria_riding', '라이딩.tga' ), ( 'montaria_falcon', '팔콘.tga' ) ):
        dados = v.read( b'data\\texture\\effect\\' + arq.encode( 'cp949' ) )
        Image.open( io.BytesIO( dados ) ).convert( 'RGBA' ).save( os.path.join( base, 'img', nome + '.png' ) )


if __name__ == '__main__':
    main()
