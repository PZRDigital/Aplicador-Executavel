"""Janela de atributos (Alt+A) com os nomes em português.

O w_statwin_bg.bmp / w_ex_statwin_bg.bmp do data.grf do cliente (versão LATAM) vem sem os nomes;
na versão kRO eles fazem parte da imagem. Este script escreve os nomes em português nas mesmas
posições e no mesmo estilo da kRO (azul-escuro 40,60,101, negrito) e grava na pasta data do cliente.

Uso: python3 statwin_ptbr.py <pasta do cliente>
"""
import io, os, sys
from PIL import Image, ImageDraw, ImageFont
from grf import Vfs

COR = ( 40, 60, 101 )
FONTE = '/mnt/c/Windows/Fonts/tahomabd.ttf'
COLUNAS = ( 5, 107, 193 )
# nomes curtos: os valores (alinhados à direita) chegam perto dos nomes com níveis altos
# linhas: topo das letras em 7 + 16 * linha; a expandida tem o 2º bloco a partir de 128
BASICOS = [
    ( 0, 0, 'For' ), ( 0, 1, 'Agi' ), ( 0, 2, 'Vit' ), ( 0, 3, 'Int' ), ( 0, 4, 'Des' ), ( 0, 5, 'Sor' ),
    ( 1, 0, 'Atq' ), ( 1, 1, 'AtqM' ), ( 1, 2, 'Prec.' ), ( 1, 3, 'Crít.' ), ( 1, 4, 'Pts de Atributo' ), ( 1, 5, 'Clã' ),
    ( 2, 0, 'Def' ), ( 2, 1, 'DefM' ), ( 2, 2, 'Esq.' ), ( 2, 3, 'Vel.Atq' ),
]
TALENTOS = [
    ( 0, 0, 'Pow' ), ( 0, 1, 'Sta' ), ( 0, 2, 'Wis' ), ( 0, 3, 'Spl' ), ( 0, 4, 'Con' ), ( 0, 5, 'Crt' ),
    ( 1, 0, 'P.Atq' ), ( 1, 1, 'S.AtqM' ), ( 1, 2, 'Cura +' ), ( 1, 3, 'Tx. Crít.' ), ( 1, 4, 'Pts de Talento' ),
    ( 2, 0, 'Res' ), ( 2, 1, 'ResM' ),
]


def escrever( im, nomes, topo ):
    d = ImageDraw.Draw( im )
    f = ImageFont.truetype( FONTE, 11 )
    sobe = f.getbbox( 'S' )[1] # do ponto de desenho até o topo da letra maiúscula
    for col, lin, txt in nomes:
        d.text( ( COLUNAS[col], topo + 16 * lin - sobe ), txt, font=f, fill=COR )


def main():
    cliente = sys.argv[1]
    vfs = Vfs( cliente )
    pasta = os.path.join( cliente, 'data', 'texture', '유저인터페이스'.encode( 'cp949' ).decode( 'latin-1' ), 'statuswnd' )
    os.makedirs( pasta, exist_ok=True )
    for nome, blocos in ( ( 'w_statwin_bg.bmp', [ ( BASICOS, 7 ) ] ), ( 'w_ex_statwin_bg.bmp', [ ( BASICOS, 7 ), ( TALENTOS, 128 ) ] ) ):
        # direto das GRFs (a pasta data já pode ter a versão escrita por este script)
        k = vfs.key( 'data\\texture\\유저인터페이스\\statuswnd\\' + nome )
        g = next( g for g in vfs.grfs if k in g.entries )
        im = Image.open( io.BytesIO( g.read( k ) ) ).convert( 'RGB' )
        for nomes, topo in blocos:
            escrever( im, nomes, topo )
        im.save( os.path.join( pasta, nome ) )
    print( 'OK: nomes em português em', pasta )


if __name__ == '__main__':
    main()
