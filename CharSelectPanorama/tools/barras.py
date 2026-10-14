"""Barras de HP/SP do jogador (Estilo 1 do Custom_HP_MOB-MVP) em PNG com transparência.

Uso: python3 barras.py <pasta texture\\effect\\custom> <saída img\\>
Gera barra_hp_cheia/vazia.png e barra_sp_cheia/vazia.png (130x17). O fundo preto dos .bmp vira alfa.
"""
import os, sys
from PIL import Image, ImageOps

def alfa(im):
    im = im.convert('RGB')
    out = Image.new('RGBA', im.size)
    px = [( r, g, b, min( 255, max( r, g, b ) * 16 ) ) for r, g, b in im.getdata()]
    out.putdata( px )
    return out

def vazia(cheia_parcial):
    # metade direita (vazia) espelhada para a esquerda: a barra é simétrica
    w, h = cheia_parcial.size
    dir_ = cheia_parcial.crop( ( w - w // 2, 0, w, h ) )
    out = cheia_parcial.copy()
    out.paste( ImageOps.mirror( dir_ ), ( 0, 0 ) )
    return out

def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs( dst, exist_ok=True )
    abrir = lambda p: Image.open( os.path.join( src, p ) )
    alfa( abrir( 'HP/20/cristal.bmp' ) ).save( os.path.join( dst, 'barra_hp_cheia.png' ) )
    alfa( vazia( abrir( 'HP/1/cristal.bmp' ) ) ).save( os.path.join( dst, 'barra_hp_vazia.png' ) )
    alfa( abrir( 'SP/20/cristal.bmp' ) ).save( os.path.join( dst, 'barra_sp_cheia.png' ) )
    alfa( abrir( 'SP/0/cristal.bmp' ) ).save( os.path.join( dst, 'barra_sp_vazia.png' ) )
    print( 'barras: 4 imagens em', dst )

main()
