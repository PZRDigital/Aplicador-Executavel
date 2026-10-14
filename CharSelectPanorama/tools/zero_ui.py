"""Imagens da interface do Ragnarok Zero (UIRenewal) para a barra de status e a barra de EXP.

Uso: python3 zero_ui.py <pasta uirenewal> [saída]
  <pasta uirenewal> = data\\texture\\유저인터페이스\\uirenewal do cliente kRO Zero
  (as entradas vêm dos patches oficiais do kRO Zero; ver tools/zero_patches.py)
  saída = dist/charselect/img/zero (padrão)

BMP: magenta (255,0,255 e vizinhos) vira transparente. TGA: mantém o alfa.
"""
import os, sys, glob
from PIL import Image, ImageDraw, ImageFont

FONTE = '/mnt/c/Windows/Fonts/tahoma.ttf'
FONTE_NEGRITO = '/mnt/c/Windows/Fonts/tahomabd.ttf'

ARQUIVOS = [
    'basicinfo/gze_bg.bmp', 'basicinfo/gze_hp_normal.bmp', 'basicinfo/gze_hp_damage.bmp',
    'basicinfo/gze_hp_damage_deco.bmp', 'basicinfo/gze_hp_deco.bmp', 'basicinfo/gze_sp_normal.bmp',
    'expbar/bg_exp_base.tga', 'expbar/bg_exp_job.tga', 'expbar/gze_exp_base.bmp', 'expbar/gze_exp_job.bmp',
    'mapname/bg_fieldname.bmp',
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


def sem_magenta( im ):
    im = im.convert( 'RGBA' )
    px = im.load()
    for y in range( im.height ):
        for x in range( im.width ):
            r, g, b, a = px[x, y]
            if r >= 0xF8 and g <= 0x08 and b >= 0xF8:
                px[x, y] = ( 0, 0, 0, 0 )
    return im


def copiar_linha( im, origem, linhas, x0, x1 ):
    px = im.load()
    for y in linhas:
        for x in range( x0, x1 ):
            px[x, y] = px[x, origem]


def copiar_coluna( im, origem, colunas, y0, y1 ):
    px = im.load()
    for x in colunas:
        for y in range( y0, y1 ):
            px[x, y] = px[origem, y]


def quests( src, out ):
    """Janela de quests do Zero: tira o coreano gravado (título, abas, botões) e escreve em português."""
    q = os.path.join( src, 'questui' )
    o = os.path.join( out, 'quest' )
    os.makedirs( o, exist_ok=True )
    # fundo: o título "퀘스트" sai (o nome é escrito pela interface)
    bg = sem_magenta( Image.open( os.path.join( q, 'img_quest_bg.bmp' ) ) )
    copiar_coluna( bg, 160, range( 30, 120 ), 2, 22 )
    bg.save( os.path.join( o, 'bg.png' ) )
    # abas verticais (cada imagem = a faixa inteira com uma aba em destaque)
    abas = [ ( 'Em andamento', 0, 119 ), ( 'Novidades', 120, 239 ), ( 'Em espera', 240, 358 ) ]
    for nome in ( 'active', 'news', 'inactive' ):
        im = sem_magenta( Image.open( os.path.join( q, 'tap_quest_%s.bmp' % nome ) ) )
        destaque = { 'active': 0, 'news': 1, 'inactive': 2 }[nome]
        for i, ( texto, y0, y1 ) in enumerate( abas ):
            copiar_linha( im, y0 + 10, range( y0 + 16, y1 - 12 ), 3, 26 )
            f = ImageFont.truetype( FONTE_NEGRITO if i == destaque else FONTE, 11 )
            w = int( f.getlength( texto ) ) + 4
            t = Image.new( 'RGBA', ( w, 16 ), ( 0, 0, 0, 0 ) )
            ImageDraw.Draw( t ).text( ( 2, 1 ), texto, font=f, fill=( 30, 40, 70, 255 ) )
            t = t.rotate( -90, expand=True )
            im.alpha_composite( t, ( 14 - t.width // 2 + 1, ( y0 + y1 ) // 2 - t.height // 2 ) )
        im.save( os.path.join( o, 'tab_%s.png' % nome ) )
    # botões "mandar para andamento / espera"
    for nome, texto in ( ( 'active', 'Para andamento' ), ( 'inactive', 'Para espera' ) ):
        for estado in ( 'normal', 'mouseon', 'pressed', 'disabled' ):
            im = sem_magenta( Image.open( os.path.join( q, 'btn_%s_%s.bmp' % ( nome, estado ) ) ) )
            copiar_coluna( im, 6, range( 7, 90 ), 3, 24 )
            f = ImageFont.truetype( FONTE_NEGRITO, 11 )
            d = ImageDraw.Draw( im )
            x = ( im.width - f.getlength( texto ) ) / 2
            cor = ( 200, 205, 215, 255 ) if estado == 'disabled' else ( 255, 255, 255, 255 )
            for dx, dy in ( ( -1, 0 ), ( 1, 0 ), ( 0, -1 ), ( 0, 1 ) ):
                d.text( ( x + dx, 6 + dy ), texto, font=f, fill=( 40, 70, 140, 255 ) )
            d.text( ( x, 6 ), texto, font=f, fill=cor )
            im.save( os.path.join( o, 'btn_%s_%s.png' % ( nome, estado ) ) )
    # peças sem texto
    for n in ( 'img_quest_bg_selected', 'img_quest_bg_unselected', 'img_quest_line', 'searchbar_bg', 'combobox_btn_bg',
               'combobox_btn_normal', 'combobox_btn_mouseon', 'combobox_btn_pressed', 'btn_searchbar_normal',
               'btn_searchbar__mouseon', 'btn_searchbar_pressed', 'btn_toggle_on', 'btn_toggle_off', 'btn_close_normal',
               'btn_close_mouseon', 'btn_close_pressed', 'btn_scroll_arrow_up', 'btn_scroll_arrow_down',
               'btn_scroll_thumb_up', 'btn_scroll_thumb_mid', 'btn_scroll_thumb_down', 'info_title', 'info_scrooll', 'img_poring' ):
        sem_magenta( Image.open( os.path.join( q, n + '.bmp' ) ) ).save( os.path.join( o, n.replace( '__', '_' ) + '.png' ) )
    for f in glob.glob( os.path.join( q, 'icon_quest', '*.bmp' ) ):
        sem_magenta( Image.open( f ) ).save( os.path.join( o, os.path.splitext( os.path.basename( f ) )[0] + '.png' ) )


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len( sys.argv ) > 2 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'img', 'zero' )
    for rel in ARQUIVOS:
        converter( os.path.join( src, rel ), os.path.join( out, os.path.splitext( os.path.basename( rel ) )[0] + '.png' ) )
    for rel in MENU + [ 'bt_%s_%s.bmp' % ( i, e ) for i in ICONES for e in ESTADOS ]:
        f = os.path.join( src, 'menuicon', rel )
        if os.path.exists( f ):
            converter( f, os.path.join( out, 'menu', os.path.splitext( rel )[0] + '.png' ) )
    quests( src, out )
    # botão do menu: logo do O Codex (sem a margem transparente) no lugar do Poring "MENU"
    logo = Image.open( os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'img', 'logo.png' ) ).convert( 'RGBA' )
    logo.crop( logo.getchannel( 'A' ).point( lambda a: 255 if a > 8 else 0 ).getbbox() ).save( os.path.join( out, 'menu', 'logo.png' ) )
    n = 0
    for f in glob.glob( os.path.join( src, 'jobicon', 'big', 'icon_job_*.bmp' ) ):
        converter( f, os.path.join( out, 'job', os.path.splitext( os.path.basename( f ) )[0] + '.png' ) )
        n += 1
    print( 'OK: %d imagens + %d ícones de classe em %s' % ( len( ARQUIVOS ), n, os.path.normpath( out ) ) )


if __name__ == '__main__':
    main()
