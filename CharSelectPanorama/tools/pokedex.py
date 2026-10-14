"""Corpo da Pokédex (janela de pet, Alt+J): img/pokedex.png, 575x490, com bordas suavizadas.

Tela esquerda (0..287): pet animado e controles. Dobradiça (287..303). Tela direita (303..575, a partir de y 36):
visor verde com os atributos da Arena de Pets e os elementos, setas, LEDs e os botões ARENA / ELEMENTOS.

Uso: python3 pokedex.py [saída]   (padrão dist/charselect/img/pokedex.png)
As posições são as da referência do usuário; a DLL (ui.cpp, POKEDEX_*) usa as mesmas.
"""
import os, sys
from PIL import Image, ImageDraw

W, H, K = 575, 490, 4  # desenha em 4x e reduz (antialias)
LW = 287  # largura da tela esquerda
RX = 303  # início da tela direita
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


def direita( d ):
    """Dobradiça e tela direita (posições usadas em ui.cpp, PDX_R_*)."""
    # dobradiça: cilindro em três gomos
    for y0, y1 in ( ( 50, 170 ), ( 180, 340 ), ( 350, 476 ) ):
        d.rounded_rectangle( s( LW - 6, y0 - 2, RX + 6, y1 + 2 ), radius=4 * K, fill=CONTORNO )
        d.rounded_rectangle( s( LW - 4, y0, RX + 4, y1 ), radius=3 * K, fill=VERMELHO_ESCURO )
        d.line( s( LW + 2, y0 + 3, LW + 2, y1 - 3 ), fill=( 235, 90, 90, 255 ), width=2 * K )
    # corpo
    d.rounded_rectangle( s( RX, 36, W, H ), radius=16 * K, fill=CONTORNO )
    d.rounded_rectangle( s( RX + 4, 40, W - 4, H - 4 ), radius=13 * K, fill=VERMELHO_ESCURO )
    d.rounded_rectangle( s( RX + 4, 40, W - 10, H - 10 ), radius=12 * K, fill=VERMELHO )
    # visor verde (moldura escura); área de texto 234x236 em (RX + 19, 61)
    d.rounded_rectangle( s( RX + 13, 55, RX + 259, 303 ), radius=6 * K, fill=CONTORNO )
    d.rounded_rectangle( s( RX + 16, 58, RX + 256, 300 ), radius=4 * K, fill=( 70, 190, 90, 255 ) )
    d.rounded_rectangle( s( RX + 19, 61, RX + 253, 297 ), radius=3 * K, fill=( 96, 206, 110, 255 ) )
    # LEDs laranja e verde, pílulas verde e laranja
    circulo( d, RX + 26, 318, 5, ( 245, 150, 40, 255 ), CONTORNO, 2 )
    circulo( d, RX + 46, 318, 5, ( 60, 200, 90, 255 ), CONTORNO, 2 )
    pilula( d, RX + 150, 314, RX + 196, 322, ( 60, 200, 90, 255 ) )
    pilula( d, RX + 208, 314, RX + 254, 322, ( 245, 150, 40, 255 ) )
    # setas (◄ ►)
    for x0 in ( RX + 16, RX + 62 ):
        d.rounded_rectangle( s( x0 - 2, 336, x0 + 42, 366 ), radius=5 * K, fill=CONTORNO )
        d.rounded_rectangle( s( x0, 338, x0 + 40, 364 ), radius=4 * K, fill=( 70, 70, 75, 255 ) )
    d.polygon( [ ( x * K, y * K ) for x, y in ( ( RX + 28, 351 ), ( RX + 44, 343 ), ( RX + 44, 359 ) ) ], fill=( 200, 200, 205, 255 ) )
    d.polygon( [ ( x * K, y * K ) for x, y in ( ( RX + 90, 351 ), ( RX + 74, 343 ), ( RX + 74, 359 ) ) ], fill=( 200, 200, 205, 255 ) )
    # alto-falante amarelo
    circulo( d, RX + 234, 351, 15, ( 245, 205, 40, 255 ), CONTORNO, 2.5 )
    for i in range( -2, 3 ):
        d.line( s( RX + 226, 351 + i * 4, RX + 242, 351 + i * 4 ), fill=( 150, 115, 20, 255 ), width=int( 1.5 * K ) )
    # botões laranja (ARENA / ELEMENTOS; o texto é da DLL)
    for x0 in ( RX + 16, RX + 140 ):
        d.rounded_rectangle( s( x0 - 2, 382, x0 + 118, 418 ), radius=6 * K, fill=CONTORNO )
        d.rounded_rectangle( s( x0, 384, x0 + 116, 416 ), radius=5 * K, fill=( 245, 150, 40, 255 ) )
        d.rounded_rectangle( s( x0 + 3, 387, x0 + 113, 398 ), radius=4 * K, fill=( 252, 185, 95, 255 ) )
    # frisos de ventilação
    for i in range( 3 ):
        y = 440 + i * 9
        d.line( s( RX + 20, y, RX + 252, y ), fill=VERMELHO_ESCURO, width=3 * K )


def main():
    saida = sys.argv[1] if len( sys.argv ) > 1 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'img', 'pokedex.png' )
    im = Image.new( 'RGBA', ( W * K, H * K ), ( 0, 0, 0, 0 ) )
    d = ImageDraw.Draw( im )
    # corpo: contorno, lateral/base mais escura (volume) e a frente vermelha
    direita( d )
    d.rounded_rectangle( s( 0, 0, LW, H ), radius=18 * K, fill=CONTORNO )
    d.rounded_rectangle( s( 4, 4, LW - 4, H - 4 ), radius=15 * K, fill=VERMELHO_ESCURO )
    d.rounded_rectangle( s( 12, 4, LW - 4, H - 10 ), radius=14 * K, fill=VERMELHO )
    # cabeçalho: degrau diagonal em duas linhas
    for off in ( 0, 7 ):
        d.line( s( 12, 64 + off, 160, 64 + off ), fill=VERMELHO_ESCURO, width=3 * K )
        d.line( s( 160, 64 + off, 186, 36 + off ), fill=VERMELHO_ESCURO, width=3 * K )
        d.line( s( 186, 36 + off, LW - 6, 36 + off ), fill=VERMELHO_ESCURO, width=3 * K )
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
