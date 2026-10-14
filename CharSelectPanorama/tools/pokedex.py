"""Corpo da Pokédex (janela de pet, Alt+J): img/pokedex.png, 287x490, com bordas suavizadas.

Uso: python3 pokedex.py [saída]   (padrão dist/charselect/img/pokedex.png)
As posições são as da referência do usuário; a DLL (ui.cpp, POKEDEX_*) usa as mesmas.
"""
import os, sys
from PIL import Image, ImageDraw

W, H, K = 287, 490, 4  # desenha em 4x e reduz (antialias)
CONTORNO = ( 35, 35, 40, 255 )
VERMELHO = ( 215, 38, 38, 255 )
VERMELHO_ESCURO = ( 168, 28, 32, 255 )


def s( *v ):
    return [ int( round( x * K ) ) for x in v ]


def circulo( d, cx, cy, r, cor, borda=None, larg=2.5 ):
    if borda:
        d.ellipse( s( cx - r - larg, cy - r - larg, cx + r + larg, cy + r + larg ), fill=borda )
    d.ellipse( s( cx - r, cy - r, cx + r, cy + r ), fill=cor )


def pilula( d, x0, y0, x1, y1, cor ):
    d.rounded_rectangle( s( x0 - 2, y0 - 2, x1 + 2, y1 + 2 ), radius=( y1 - y0 + 4 ) * K / 2, fill=CONTORNO )
    d.rounded_rectangle( s( x0, y0, x1, y1 ), radius=( y1 - y0 ) * K / 2, fill=cor )


def main():
    saida = sys.argv[1] if len( sys.argv ) > 1 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'img', 'pokedex.png' )
    im = Image.new( 'RGBA', ( W * K, H * K ), ( 0, 0, 0, 0 ) )
    d = ImageDraw.Draw( im )
    # corpo: contorno, lateral/base mais escura (volume) e a frente vermelha
    d.rounded_rectangle( s( 0, 0, W, H ), radius=18 * K, fill=CONTORNO )
    d.rounded_rectangle( s( 4, 4, W - 4, H - 4 ), radius=15 * K, fill=VERMELHO_ESCURO )
    d.rounded_rectangle( s( 12, 4, W - 4, H - 10 ), radius=14 * K, fill=VERMELHO )
    # cabeçalho: degrau diagonal em duas linhas
    for off in ( 0, 7 ):
        d.line( s( 12, 64 + off, 160, 64 + off ), fill=VERMELHO_ESCURO, width=3 * K )
        d.line( s( 160, 64 + off, 186, 36 + off ), fill=VERMELHO_ESCURO, width=3 * K )
        d.line( s( 186, 36 + off, W - 6, 36 + off ), fill=VERMELHO_ESCURO, width=3 * K )
    # lente azul e as três luzes
    circulo( d, 47, 40, 24, ( 255, 255, 255, 255 ), CONTORNO, 3 )
    circulo( d, 47, 40, 18, ( 30, 155, 230, 255 ), CONTORNO, 2 )
    d.ellipse( s( 37, 30, 47, 38 ), fill=( 140, 210, 250, 255 ) )
    for x, cor in ( ( 91, ( 230, 40, 40, 255 ) ), ( 115, ( 240, 215, 40, 255 ) ), ( 139, ( 40, 170, 70, 255 ) ) ):
        circulo( d, x, 21, 6, cor, CONTORNO, 2 )
    # moldura da tela (canto inferior esquerdo cortado), dois pontos vermelhos e a tela
    moldura = [ ( 37, 93 ), ( 259, 93 ), ( 267, 101 ), ( 267, 345 ), ( 259, 353 ), ( 67, 353 ), ( 29, 315 ), ( 29, 101 ) ]
    d.polygon( [ ( x * K, y * K ) for x, y in [ ( a - 3 if a < 148 else a + 3, b - 3 if b < 223 else b + 3 ) for a, b in moldura ] ], fill=CONTORNO )
    d.polygon( [ ( x * K, y * K ) for x, y in moldura ], fill=( 240, 240, 240, 255 ) )
    for x in ( 139, 157 ):
        circulo( d, x, 101, 3, ( 220, 40, 40, 255 ) )
    d.rounded_rectangle( s( 44, 107, 256, 317 ), radius=3 * K, fill=( 150, 150, 152, 255 ) )
    d.rounded_rectangle( s( 46, 109, 254, 315 ), radius=2 * K, fill=( 88, 89, 91, 255 ) )
    # botão vermelho e alto-falante da moldura
    circulo( d, 74, 337, 11, ( 220, 40, 40, 255 ), CONTORNO, 2.5 )
    for i in range( 4 ):
        y = 326 + i * 7
        d.line( s( 200, y, 252, y ), fill=( 60, 60, 64, 255 ), width=int( 2.5 * K ) )
    # botão redondo escuro, pílulas vermelha e azul, visor verde e direcional
    circulo( d, 45, 388, 17, ( 75, 75, 80, 255 ), CONTORNO, 3 )
    pilula( d, 74, 381, 118, 391, ( 220, 40, 40, 255 ) )
    pilula( d, 132, 381, 176, 391, ( 40, 170, 170, 255 ) )
    d.rounded_rectangle( s( 71, 406, 181, 463 ), radius=5 * K, fill=CONTORNO )
    d.rounded_rectangle( s( 74, 409, 178, 460 ), radius=4 * K, fill=( 60, 180, 80, 255 ) )
    cx, cy, braco, larg = 233, 415, 31, 11
    for x0, y0, x1, y1 in ( ( cx - larg, cy - braco, cx + larg, cy + braco ), ( cx - braco, cy - larg, cx + braco, cy + larg ) ):
        d.rounded_rectangle( s( x0 - 3, y0 - 3, x1 + 3, y1 + 3 ), radius=4 * K, fill=CONTORNO )
    for x0, y0, x1, y1 in ( ( cx - larg, cy - braco, cx + larg, cy + braco ), ( cx - braco, cy - larg, cx + braco, cy + larg ) ):
        d.rounded_rectangle( s( x0, y0, x1, y1 ), radius=3 * K, fill=( 70, 70, 75, 255 ) )
    circulo( d, cx, cy, 5, ( 55, 55, 60, 255 ) )
    os.makedirs( os.path.dirname( saida ), exist_ok=True )
    im.resize( ( W, H ), Image.LANCZOS ).save( saida )
    print( 'OK:', os.path.normpath( saida ) )


if __name__ == '__main__':
    main()
