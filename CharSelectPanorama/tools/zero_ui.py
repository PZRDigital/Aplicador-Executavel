"""Imagens da interface do Ragnarok Zero (UIRenewal) para a barra de status e a barra de EXP.

Uso: python3 zero_ui.py <pasta uirenewal> [saída]
  <pasta uirenewal> = data\\texture\\유저인터페이스\\uirenewal do cliente kRO Zero
  (as entradas vêm dos patches oficiais do kRO Zero; ver tools/zero_patches.py)
  saída = dist/charselect/img/zero (padrão)

BMP: magenta (255,0,255 e vizinhos) vira transparente. TGA: mantém o alfa.
"""
import os, sys, glob
from PIL import Image

ARQUIVOS = [
    'basicinfo/gze_bg.bmp', 'basicinfo/gze_hp_normal.bmp', 'basicinfo/gze_hp_damage.bmp',
    'basicinfo/gze_hp_damage_deco.bmp', 'basicinfo/gze_hp_deco.bmp', 'basicinfo/gze_sp_normal.bmp',
    'expbar/bg_exp_base.tga', 'expbar/bg_exp_job.tga', 'expbar/gze_exp_base.bmp', 'expbar/gze_exp_job.bmp',
]

# Menu de ícones (UIMenuIconWnd do tema novo): fundo, botão MENU e cada ícone em 3 estados
MENU = [ 'bg_menu.tga', 'bt_menu_normal.bmp', 'bt_menu_over.bmp', 'bt_menu_press.bmp' ]
ICONES = [ 'status', 'equip', 'item', 'skill', 'party', 'guild', 'quest', 'map', 'navigation', 'option', 'bank', 'rec',
    'mail', 'achievement', 'tip', 'shop', 'keyboard', 'attendance', 'adventureragency', 'repute', 'probability', 'booking' ]
ESTADOS = [ 'normal', 'mouseon', 'press' ]


def converter( origem, destino ):
    im = Image.open( origem )
    if origem.lower().endswith( '.bmp' ):
        im = im.convert( 'RGBA' )
        px = im.load()
        for y in range( im.height ):
            for x in range( im.width ):
                r, g, b, a = px[x, y]
                if r >= 0xF8 and g <= 0x08 and b >= 0xF8:
                    px[x, y] = ( 0, 0, 0, 0 )
    else:
        im = im.convert( 'RGBA' )
    os.makedirs( os.path.dirname( destino ), exist_ok=True )
    im.save( destino, optimize=True )


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len( sys.argv ) > 2 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'img', 'zero' )
    for rel in ARQUIVOS:
        converter( os.path.join( src, rel ), os.path.join( out, os.path.splitext( os.path.basename( rel ) )[0] + '.png' ) )
    for rel in MENU + [ 'bt_%s_%s.bmp' % ( i, e ) for i in ICONES for e in ESTADOS ]:
        f = os.path.join( src, 'menuicon', rel )
        if os.path.exists( f ):
            converter( f, os.path.join( out, 'menu', os.path.splitext( rel )[0] + '.png' ) )
    n = 0
    for f in glob.glob( os.path.join( src, 'jobicon', 'big', 'icon_job_*.bmp' ) ):
        converter( f, os.path.join( out, 'job', os.path.splitext( os.path.basename( f ) )[0] + '.png' ) )
        n += 1
    print( 'OK: %d imagens + %d ícones de classe em %s' % ( len( ARQUIVOS ), n, os.path.normpath( out ) ) )


if __name__ == '__main__':
    main()
