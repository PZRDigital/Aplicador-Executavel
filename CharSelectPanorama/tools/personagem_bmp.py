"""Imagens da janela Personagem desenhada pelo próprio cliente (src/charwnd.cpp): fundos, caixas e botões em
.bmp com o magenta como transparente (o cliente não tem transparência parcial; as bordas arredondadas são
suavizadas contra a cor do fundo da janela). Medidas = as de src/ui.cpp (CharGeo, referência x 0,86).

imagens() -> { caminho na GRF (cp949, minúsculo): PIL.Image }
"""
import os
from PIL import Image, ImageDraw, ImageFont

# cores exatas em 15 bits (X1R5G5B5: a superfície das janelas do cliente; nível v -> v<<3 | v>>2), senão elas
# puxam para o rosa
BG = ( 239, 239, 231 )
EDGE = ( 206, 206, 198 )
BOX = ( 231, 231, 231 )
BOX_EDGE = ( 214, 214, 206 )
WHITE = ( 255, 255, 255 )
BASE = ( 115, 148, 206 )
INK = ( 41, 41, 41 )
MAGENTA = ( 255, 0, 255 )
SS = 4  # supersampling das bordas

PASTA = 'data\\texture\\유저인터페이스\\'
FONTE = os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'fonts', 'Inter.ttf' )


def caixa( w, h, r, fill, edge, fundo, edge_w=1 ):
    """retângulo arredondado sobre a cor de fundo (bordas suavizadas contra ela)"""
    big = Image.new( 'RGB', ( w * SS, h * SS ), fundo )
    d = ImageDraw.Draw( big )
    d.rounded_rectangle( ( 0, 0, w * SS - 1, h * SS - 1 ), r * SS, fill=edge )
    e = edge_w * SS
    d.rounded_rectangle( ( e, e, w * SS - 1 - e, h * SS - 1 - e ), max( 0, ( r - edge_w ) * SS ), fill=fill )
    return big.resize( ( w, h ), Image.LANCZOS )


def janela( w, h, edge=EDGE ):
    """fundo da janela: cantos de fora em magenta (sem suavizar: o que está atrás varia)"""
    im = caixa( w, h, 8, BG, edge, BG )
    mask = Image.new( 'L', ( w, h ), 0 )
    ImageDraw.Draw( mask ).rounded_rectangle( ( 0, 0, w - 1, h - 1 ), 8, fill=255 )
    out = Image.new( 'RGB', ( w, h ), MAGENTA )
    out.paste( im, ( 0, 0 ), mask )
    return out


def botao( w, h, texto, fill, edge ):
    im = caixa( w, h, 6, fill, edge, BG )
    if texto:
        big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
        d = ImageDraw.Draw( big )
        f = ImageFont.truetype( FONTE, 12 * SS )
        bb = d.textbbox( ( 0, 0 ), texto, font=f )
        d.text( ( ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], ( h * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1] ), texto, fill=INK, font=f )
        im = big.resize( ( w, h ), Image.LANCZOS )
    return im


def fechar( t, fill ):
    im = caixa( t, t, t // 2, fill, BOX_EDGE, BG )
    big = im.resize( ( t * SS, t * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    c, a = t * SS / 2, 3.5 * SS
    for p in ( ( c - a, c - a, c + a, c + a ), ( c - a, c + a, c + a, c - a ) ):
        d.line( p, fill=( 74, 74, 74 ), width=int( 1.3 * SS ) )
    return big.resize( ( t, t ), Image.LANCZOS )


def marcar( t, on ):
    im = caixa( t, t, 3, BASE if on else WHITE, BASE if on else BOX_EDGE, BG )
    if on:
        big = im.resize( ( t * SS, t * SS ), Image.NEAREST )
        d = ImageDraw.Draw( big )
        d.line( ( 3 * SS, 7 * SS, 6 * SS, 10 * SS, 11 * SS, 4 * SS ), fill=WHITE, width=int( 1.6 * SS ), joint='curve' )
        im = big.resize( ( t, t ), Image.LANCZOS )
    return im


# inventário: as mesmas medidas de src/ui.hpp (INV_*)
INV_W, INV_H, INV_CELL, INV_TAB_W, INV_TAB_H = 326, 352, 34, 69, 24
# armazém: src/ui.hpp (ST_*)
ST_W, ST_H, ST_ROW_W, ST_ROW_H, ST_TAB_W, ST_TAB_H = 340, 456, 300, 32, 72, 22
CART_W, CART_H = 326, 46 + 7 * 36 + 30   # ui::CART_*
SR_W, SR_H = 332, 46 + 6 * 36 + 10       # ui::SR_*

# janela de habilidades: as mesmas medidas de src/skillwnd.cpp
SK_W, SK_H = 420, 452
SK_ROW_W, SK_ROW_H = 372, 32
SK_TAB_WS, SK_TAB_H = ( 112, 86, 70 ), 26


def mais( t, fill, edge ):
    im = caixa( t, t, 4, fill, edge, BG )
    big = im.resize( ( t * SS, t * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    c, a, w = t * SS / 2, 4 * SS, int( 1.6 * SS )
    d.line( ( c - a, c, c + a, c ), fill=INK, width=w )
    d.line( ( c, c - a, c, c + a ), fill=INK, width=w )
    return big.resize( ( t, t ), Image.LANCZOS )


def seta( t, direita ):
    big = Image.new( 'RGB', ( t * SS, t * SS ), BG )
    d = ImageDraw.Draw( big )
    a, b, c = 3.5 * SS, t * SS / 2, 2 * SS
    if direita:
        pts = ( ( b - c, b - a ), ( b + c, b ), ( b - c, b + a ) )
    else:
        pts = ( ( b + c, b - a ), ( b - c, b ), ( b + c, b + a ) )
    d.line( pts, fill=( 74, 74, 74 ), width=int( 1.6 * SS ), joint='curve' )
    return big.resize( ( t, t ), Image.LANCZOS )


def q15( c ):
    """cor exata em 15 bits (ver BG)"""
    return tuple( ( v >> 3 ) << 3 | ( v >> 5 ) for v in c )


# descrição do item (UIItemInfoWnd, OnDraw 0x8B0910): o cliente monta a moldura com 9 pedaços de 14 px
# (sysbox_lu/mu/ru/lm/bg/rm/ld/md/rd) e põe por cima o fundo do topo (collection_bg, 280x120: imagem do item
# em 10,11 75x100; nome em 90,10; descrição em 91,31). Bordas de grau (g1..g7) e da comparação mudam só a cor.
GRAU = { 1: ( 210, 160, 155 ), 2: ( 220, 190, 170 ), 3: ( 210, 215, 170 ), 4: ( 175, 175, 220 ),
         5: ( 210, 160, 155 ), 6: ( 95, 95, 95 ), 7: ( 220, 190, 111 ) }


def moldura( edge ):
    im = janela( 42, 42, edge )
    pecas = {}
    for j, lin in enumerate( ( 'u', 'm', 'd' ) ):
        for i, col in enumerate( ( 'l', 'm', 'r' ) ):
            pecas[col + lin] = im.crop( ( i * 14, j * 14, i * 14 + 14, j * 14 + 14 ) )
    pecas['bg'] = pecas.pop( 'mm' )
    return pecas


def topo_item( edge ):
    """fundo + cabeçalho em pílula (6,5 250x24) onde o cliente escreve o nome (x 16, y 10); a moldura da
    ilustração é do ui::PaintItemExtras"""
    im = janela( 280, 240, edge ).crop( ( 0, 0, 280, 120 ) )
    im.paste( caixa( 250, 24, 8, WHITE, edge, BG ), ( 6, 5 ) )
    return im


# runas (opções aleatórias): cor de cada tipo = RuneColor de src/ui.cpp; caixa 274x24 com a etiqueta em 3,3
RUNAS = { 'def': ( 112, 118, 132 ), 'rac': ( 172, 84, 58 ), 'siz': ( 132, 112, 52 ), 'ele': ( 56, 132, 196 ),
          'atk': ( 206, 104, 36 ), 'mat': ( 140, 84, 196 ), 'cri': ( 200, 60, 90 ), 'hp': ( 64, 150, 84 ),
          'cur': ( 64, 150, 84 ), 'sp': ( 72, 106, 204 ), 'cst': ( 72, 106, 204 ), 'atr': ( 186, 140, 36 ),
          'cls': ( 150, 96, 160 ), '': ( 120, 100, 170 ) }


def runa( cor ):
    mix = lambda k: q15( tuple( int( v + ( 255 - v ) * k ) for v in cor ) )
    im = caixa( 274, 24, 5, mix( 0.82 ), mix( 0.45 ), BG )
    im.paste( caixa( 26, 18, 3, q15( cor ), q15( cor ), mix( 0.82 ) ), ( 3, 3 ) )
    return im


def xis( cor ):
    im = Image.new( 'RGB', ( 11, 11 ), MAGENTA )
    d = ImageDraw.Draw( im )
    for a in ( 2, 3 ):  # 2 px de espessura, sem suavizar (o fundo atrás varia)
        d.line( ( a, 2, 8 + a - 3, 8 ), fill=cor )
        d.line( ( a, 8, 8 + a - 3, 2 ), fill=cor )
    return im


def _original( caminho ):
    """imagem original do cliente (data.grf), ou None"""
    import io, grf
    try:
        g = grf.Grf( os.path.join( os.path.dirname( __file__ ), '..', '..', '..', 'Cliente 2025', 'data.grf' ) )
        return Image.open( io.BytesIO( g.read( ( PASTA + caminho ).encode( 'cp949' ).lower() ) ) ).convert( 'RGB' )
    except Exception:
        return None


def painel_dano( fundo, edge, edge_w ):
    """painel da aba Dano (64x92): cartão do tema com o bonequinho da imagem original por cima"""
    im = caixa( 64, 92, 7, fundo, edge, BG, edge_w )
    orig = _original( 'showdamage\\btn_damage.bmp' )
    if orig is None or orig.size != ( 64, 92 ):
        return im
    px, dst = orig.load(), im.load()
    for y in range( 26, 90 ):
        for x in range( 3, 61 ):
            r, g, b = px[x, y]
            claro = min( r, g, b ) > 175 and max( r, g, b ) - min( r, g, b ) < 25  # fundo branco/cinza e sombra
            if not claro:
                dst[x, y] = ( r, g, b )
    return im


LG_W, LG_M = 380, 36   # ui::LG_* (painel de login)


def texto_img( w, h, texto, cor, tam=12, fundo=BG, negrito=False, centro=False ):
    """link: só o texto sobre o fundo do painel"""
    big = Image.new( 'RGB', ( w * SS, h * SS ), fundo )
    d = ImageDraw.Draw( big )
    f = ImageFont.truetype( FONTE, tam * SS )
    bb = d.textbbox( ( 0, 0 ), texto, font=f )
    x = ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0] if centro else -bb[0]
    d.text( ( x, ( h * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1] ), texto, fill=cor, font=f )
    if negrito:
        d.text( ( x + SS * 0.6, ( h * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1] ), texto, fill=cor, font=f )
    return big.resize( ( w, h ), Image.LANCZOS )


def botao_principal( w, h, texto, fill ):
    im = caixa( w, h, 10, fill, ( 82, 114, 176 ), BG )
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    f = ImageFont.truetype( FONTE, 15 * SS )
    bb = d.textbbox( ( 0, 0 ), texto, font=f )
    x, y = ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], ( h * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1]
    for dx in ( 0, SS * 0.7 ):
        d.text( ( x + dx, y ), texto, fill=WHITE, font=f )
    return big.resize( ( w, h ), Image.LANCZOS )


def marca_login():
    """logotipo, "O CODEX" e o lema, centrados na largura do painel"""
    w, h = LG_W - 1, 200
    im = Image.new( 'RGB', ( w, h ), BG )
    logo = os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'img', 'logo.png' )
    try:
        lg = Image.open( logo ).convert( 'RGBA' )
        lh = 104
        lw = int( lg.width * lh / lg.height )
        lg = lg.resize( ( lw, lh ), Image.LANCZOS )
        im.paste( lg, ( ( w - lw ) // 2, 0 ), lg )
    except Exception:
        pass
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    serif = os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'fonts', 'Cinzel.ttf' )
    f1 = ImageFont.truetype( serif if os.path.exists( serif ) else FONTE, 30 * SS )
    f2 = ImageFont.truetype( FONTE, 13 * SS )
    for txt, f, y, cor in ( ( 'O CODEX', f1, 118, INK ), ( 'Além das Páginas de Midgard', f2, 170, ( 110, 110, 104 ) ) ):
        bb = d.textbbox( ( 0, 0 ), txt, font=f )
        d.text( ( ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], y * SS - bb[1] ), txt, fill=cor, font=f )
    d.rectangle( ( ( w / 2 - 30 ) * SS, 160 * SS, ( w / 2 + 30 ) * SS, 162 * SS ), fill=BASE )
    return big.resize( ( w, h ), Image.LANCZOS )



SV_MED, SV_BACK = 180, ( 140, 34 )   # ui::SV_*


def medalhao( t, aceso ):
    """medalhão da escolha do servidor: anéis azuis, fundo creme claro e o logotipo; cantos em magenta"""
    import math
    big = Image.new( 'RGB', ( t * SS, t * SS ), MAGENTA )
    d = ImageDraw.Draw( big )
    c = t * SS / 2
    def anel( r, cor, w=0 ):
        if w: d.ellipse( ( c - r, c - r, c + r, c + r ), outline=cor, width=int( w ) )
        else: d.ellipse( ( c - r, c - r, c + r, c + r ), fill=cor )
    anel( c - 1, ( 214, 222, 238 ) if aceso else ( 226, 228, 232 ) )
    anel( c - 1, BASE, 3 * SS )
    anel( c - 12 * SS, WHITE )
    anel( c - 12 * SS, ( 140, 165, 214 ) if aceso else BOX_EDGE, 2 * SS )
    # estrela de seis pontas (dois triângulos) fininha, como no selo da referência
    r = c - 26 * SS
    for rot in ( -90, 90 ):
        pts = [ ( c + r * math.cos( math.radians( rot + k * 120 ) ), c + r * math.sin( math.radians( rot + k * 120 ) ) ) for k in range( 3 ) ]
        d.line( pts + [ pts[0] ], fill=( 190, 204, 232 ), width=int( 1.5 * SS ) )
    im = big.resize( ( t, t ), Image.LANCZOS )
    # cantos fora do círculo de volta a magenta puro (a suavização misturou)
    m = Image.new( 'L', ( t, t ), 0 ); ImageDraw.Draw( m ).ellipse( ( 0, 0, t - 1, t - 1 ), fill=255 )
    out = Image.new( 'RGB', ( t, t ), MAGENTA ); out.paste( im, ( 0, 0 ), m )
    logo = os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'img', 'logo.png' )
    try:
        lg = Image.open( logo ).convert( 'RGBA' )
        lw = int( t * 0.5 ); lh = int( lg.height * lw / lg.width )
        lg = lg.resize( ( lw, lh ), Image.LANCZOS )
        out.paste( lg, ( ( t - lw ) // 2, ( t - lh ) // 2 ), lg )
    except Exception:
        pass
    return out


SEAL, SEAL_FRAMES = 300, 12   # ui::SEAL*


def _logo_rgba():
    try:
        return Image.open( os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'img', 'logo.png' ) ).convert( 'RGBA' )
    except Exception:
        return None


def selo( t, frame, fundo_transp=True, med=False, aceso=False ):
    """selo/medalhão animado: anel de marcas e pontos girando (sentidos opostos), hexagrama girando devagar,
    brilho que pulsa com o quadro; o logotipo fica parado no centro. Simetria de 60 graus: 12 quadros de 5 graus"""
    import math
    ang = frame * 5.0
    pulse = 0.5 + 0.5 * math.sin( frame / SEAL_FRAMES * 2 * math.pi )
    S2 = 3
    big = Image.new( 'RGB', ( t * S2, t * S2 ), MAGENTA )
    d = ImageDraw.Draw( big )
    c = t * S2 / 2
    R = c - 2
    def circ( r, **kw ): d.ellipse( ( c - r, c - r, c + r, c + r ), **kw )
    def mix( a, b, k ): return tuple( int( a[i] + ( b[i] - a[i] ) * k ) for i in range( 3 ) )
    glow = mix( ( 226, 230, 240 ), ( 196, 212, 240 ), pulse if not aceso else 1.0 )
    circ( R, fill=glow )
    circ( R - 5 * S2, fill=BG if not med else ( 248, 248, 244 ) )
    circ( R - 5 * S2, outline=BASE, width=3 * S2 )
    # marcas (36, período 10 graus)
    for k in range( 36 ):
        a = math.radians( ang + k * 10 )
        r1, r2 = R - 10 * S2, R - ( 18 if k % 3 == 0 else 14 ) * S2
        d.line( ( c + r1 * math.cos( a ), c + r1 * math.sin( a ), c + r2 * math.cos( a ), c + r2 * math.sin( a ) ), fill=( 140, 165, 214 ), width=2 * S2 )
    rr = R - 26 * S2
    circ( rr, outline=( 189, 206, 231 ), width=2 * S2 )
    # 12 pontos girando ao contrário (período 30 graus)
    for k in range( 12 ):
        a = math.radians( -ang + k * 30 )
        x, y = c + rr * math.cos( a ), c + rr * math.sin( a )
        d.ellipse( ( x - 3 * S2, y - 3 * S2, x + 3 * S2, y + 3 * S2 ), fill=BASE )
    # hexagrama
    rh = rr - 10 * S2
    for rot in ( -90, 90 ):
        pts = [ ( c + rh * math.cos( math.radians( rot + ang + k * 120 ) ), c + rh * math.sin( math.radians( rot + ang + k * 120 ) ) ) for k in range( 3 ) ]
        d.line( pts + [ pts[0] ], fill=mix( ( 196, 210, 236 ), ( 140, 165, 214 ), pulse ), width=2 * S2 )
    im = big.resize( ( t, t ), Image.LANCZOS )
    m = Image.new( 'L', ( t, t ), 0 ); ImageDraw.Draw( m ).ellipse( ( 0, 0, t - 1, t - 1 ), fill=255 )
    out = Image.new( 'RGB', ( t, t ), MAGENTA ); out.paste( im, ( 0, 0 ), m )
    lg = _logo_rgba()
    if lg is not None:
        lw = int( t * 0.46 ); lh = int( lg.height * lw / lg.width )
        lg = lg.resize( ( lw, lh ), Image.LANCZOS )
        # o logotipo é misturado sobre o fundo claro do centro (não sobre o magenta)
        base = Image.new( 'RGB', ( lw, lh ), BG if not med else ( 248, 248, 244 ) )
        base.paste( lg, ( 0, 0 ), lg )
        mk = lg.split()[3].point( lambda v: 255 if v > 8 else 0 )
        out.paste( base, ( ( t - lw ) // 2, ( t - lh ) // 2 ), mk )
    return out


def convite( nivel ):
    """pílula "Clique no selo para começar" em 4 intensidades (pulsa)"""
    w, h = 260, 34
    k = nivel / 3.0
    borda = tuple( int( BOX_EDGE[i] + ( BASE[i] - BOX_EDGE[i] ) * k ) for i in range( 3 ) )
    im = caixa( w, h, 17, BG, borda, MAGENTA, 2 )
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    dd = ImageDraw.Draw( big )
    f = ImageFont.truetype( FONTE, 13 * SS )
    txt = 'CLIQUE NO SELO PARA COMEÇAR'
    cor = tuple( int( ( 110, 110, 104 )[i] + ( ( 41, 41, 41 )[i] - ( 110, 110, 104 )[i] ) * k ) for i in range( 3 ) )
    bb = dd.textbbox( ( 0, 0 ), txt, font=f )
    dd.text( ( ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], ( h * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1] ), txt, fill=cor, font=f )
    out = big.resize( ( w, h ), Image.LANCZOS )
    # cantos de fora de volta a magenta
    m = Image.new( 'L', ( w, h ), 0 ); ImageDraw.Draw( m ).rounded_rectangle( ( 0, 0, w - 1, h - 1 ), 17, fill=255 )
    o2 = Image.new( 'RGB', ( w, h ), MAGENTA ); o2.paste( out, ( 0, 0 ), m )
    return o2


def halo_ret( w, h, r, nivel, niveis=6 ):
    """anel de brilho em volta de um botão (sobre o fundo do painel)"""
    k = nivel / ( niveis - 1 )
    cor = tuple( int( BG[i] + ( ( 160, 184, 228 )[i] - BG[i] ) * k ) for i in range( 3 ) )
    im = Image.new( 'RGB', ( w, h ), BG )
    big = im.resize( ( w * SS, h * SS ) )
    dd = ImageDraw.Draw( big )
    dd.rounded_rectangle( ( 0, 0, w * SS - 1, h * SS - 1 ), r * SS, fill=cor )
    dd.rounded_rectangle( ( 3 * SS, 3 * SS, w * SS - 1 - 3 * SS, h * SS - 1 - 3 * SS ), ( r - 3 ) * SS, fill=BG )
    return big.resize( ( w, h ), Image.LANCZOS )


def halo_circ( t, nivel, niveis=6 ):
    k = nivel / ( niveis - 1 )
    im = Image.new( 'RGB', ( t * SS, t * SS ), BG )
    dd = ImageDraw.Draw( im )
    for i in range( 14, 0, -1 ):
        kk = k * ( 1 - i / 14.0 ) ** 1.5
        cor = tuple( int( BG[j] + ( ( 160, 184, 228 )[j] - BG[j] ) * kk ) for j in range( 3 ) )
        r = t * SS / 2 - ( 14 - i ) * SS
        dd.ellipse( ( t * SS / 2 - r, t * SS / 2 - r, t * SS / 2 + r, t * SS / 2 + r ), fill=cor )
    return im.resize( ( t, t ), Image.LANCZOS )



# ---------------------------------------------------------------- seleção de personagem (ui::PaintSelectWindow)
SEL_LIST_W, SEL_ROW_W, SEL_ROW_H, SEL_THUMB, SEL_PLAY_H = 360, 336, 64, 52, 62
SEL_ROW_BG = { 0: BOX, 1: ( 247, 247, 239 ), 2: WHITE }  # normal, mouse em cima, escolhida


def sobre_cena( im, r ):
    """cantos de fora em magenta (o que está atrás é o cenário 3D)"""
    w, h = im.size
    mask = Image.new( 'L', ( w * SS, h * SS ), 0 )
    ImageDraw.Draw( mask ).rounded_rectangle( ( 0, 0, w * SS - 1, h * SS - 1 ), r * SS, fill=255 )
    mask = mask.resize( ( w, h ), Image.LANCZOS ).point( lambda v: 255 if v >= 128 else 0 )
    out = Image.new( 'RGB', ( w, h ), MAGENTA )
    out.paste( im, ( 0, 0 ), mask )
    return out


def botao_txt( w, h, texto, fill, edge, cor, tam=12, negrito=True, r=8, fundo=BG, edge_w=1 ):
    im = caixa( w, h, r, fill, edge, fundo, edge_w )
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    f = ImageFont.truetype( FONTE, tam * SS )
    bb = d.textbbox( ( 0, 0 ), texto, font=f )
    x, y = ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], ( h * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1]
    for dx in ( ( 0, SS * 0.6 ) if negrito else ( 0, ) ):
        d.text( ( x + dx, y ), texto, fill=cor, font=f )
    return big.resize( ( w, h ), Image.LANCZOS )


def estrela( t, cor, fundo ):
    import math
    big = Image.new( 'RGB', ( t * SS, t * SS ), fundo )
    c, R = t * SS / 2, t * SS / 2 - SS
    pts = []
    for i in range( 10 ):
        a = -math.pi / 2 + i * math.pi / 5
        rr = R if i % 2 == 0 else R * 0.45
        pts.append( ( c + math.cos( a ) * rr, c + math.sin( a ) * rr ) )
    ImageDraw.Draw( big ).polygon( pts, fill=cor )
    return big.resize( ( t, t ), Image.LANCZOS )


def logo_barra( h, fundo ):
    lg = _logo_rgba()
    if lg is None:
        return Image.new( 'RGB', ( h, h ), fundo )
    w = max( 1, round( lg.width * h / lg.height ) )
    if w > 40:
        w, h2 = 40, max( 1, round( lg.height * 40 / lg.width ) )
    else:
        h2 = h
    lg = lg.resize( ( w, h2 ), Image.LANCZOS )
    out = Image.new( 'RGB', ( 40, h ), fundo )  # largura fixa (ui::PaintSelectWindow)
    out.paste( lg, ( ( 40 - w ) // 2, ( h - h2 ) // 2 ), lg )
    return out


def sel_imagens( put ):
    barra = q15( ( 246, 246, 241 ) )
    put( 'codex\\sel_logo.bmp', logo_barra( 26, barra ) )
    edges = { 0: BOX_EDGE, 1: BASE, 2: BASE }
    for k, bg in SEL_ROW_BG.items():
        put( 'codex\\sel_linha_%d.bmp' % k, caixa( SEL_ROW_W, SEL_ROW_H, 8, q15( bg ), edges[k], BG, 2 if k == 2 else 1 ) )
        put( 'codex\\sel_retrato_%d.bmp' % k, caixa( SEL_THUMB, SEL_THUMB, 6, q15( ( 240, 244, 251 ) ) if k == 2 else WHITE, BOX_EDGE, q15( bg ) ) )
        put( 'codex\\sel_num_%d.bmp' % k, caixa( 24, 24, 6, BASE if k == 2 else WHITE, ( 82, 114, 176 ) if k == 2 else BOX_EDGE, q15( bg ) ) )
        for est, cor in ( ( 'on', ( 222, 170, 40 ) ), ( 'off', ( 190, 190, 182 ) ), ( 'over', ( 230, 200, 130 ) ) ):
            put( 'codex\\sel_estrela_%s_%d.bmp' % ( est, k ), estrela( 18, q15( cor ), q15( bg ) ) )
    azul, azul_on = BASE, ( 132, 162, 216 )
    for est, cor in ( ( '', azul ), ( '_on', azul_on ) ):
        put( 'codex\\sel_jogar%s.bmp' % est, sobre_cena( botao_principal_g( SEL_LIST_W, SEL_PLAY_H, 'JOGAR', cor, 22 ), 10 ) )
        put( 'codex\\sel_criar%s.bmp' % est, sobre_cena( botao_principal_g( SEL_LIST_W, SEL_PLAY_H, 'CRIAR', cor, 22 ), 10 ) )
    perigo = ( 196, 70, 70 )
    for est, fill, edge in ( ( '', BOX, BOX_EDGE ), ( '_on', WHITE, perigo ) ):
        put( 'codex\\sel_apagar%s.bmp' % est, sobre_cena( botao_txt( 120, 38, 'APAGAR', fill, edge, perigo ), 8 ) )
        put( 'codex\\sel_apagar_def%s.bmp' % est, sobre_cena( botao_txt( 180, 38, 'APAGAR DEFINITIVO', fill, edge, perigo ), 8 ) )
    for est, fill, edge in ( ( '', BOX, BOX_EDGE ), ( '_on', WHITE, BASE ) ):
        put( 'codex\\sel_cancelar_exc%s.bmp' % est, sobre_cena( botao_txt( 180, 38, 'CANCELAR EXCLUSÃO', fill, edge, INK ), 8 ) )
        put( 'codex\\sel_esq%s.bmp' % est, botao_txt( 20, 18, '‹', fill, edge, ( 82, 114, 176 ), 13, True, 5 ) )
        put( 'codex\\sel_dir%s.bmp' % est, botao_txt( 20, 18, '›', fill, edge, ( 82, 114, 176 ), 13, True, 5 ) )
    put( 'codex\\sel_sair.bmp', botao_txt( 64, 22, 'SAIR', BOX, BOX_EDGE, ( 110, 110, 104 ), 11, True, 6, barra ) )
    put( 'codex\\sel_sair_on.bmp', botao_txt( 64, 22, 'SAIR', WHITE, perigo, perigo, 11, True, 6, barra ) )


def botao_principal_g( w, h, texto, fill, tam ):
    im = caixa( w, h, 10, fill, ( 82, 114, 176 ), BG )
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    f = ImageFont.truetype( FONTE, tam * SS )
    bb = d.textbbox( ( 0, 0 ), texto, font=f )
    x, y = ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], ( h * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1]
    for dx in ( 0, SS * 0.8 ):
        d.text( ( x + dx, y ), texto, fill=WHITE, font=f )
    return big.resize( ( w, h ), Image.LANCZOS )


# ---------------------------------------------------------------- criação de personagem (ui::PaintMakeWindow)
MK_JOBS = ( 0, 1, 2, 3, 4, 5, 6, 24, 25, 4046 )


def icone_classe( job, t, fundo ):
    caminho = os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'icones', '%d.png' % job )
    out = Image.new( 'RGB', ( t, t ), fundo )
    try:
        ic = Image.open( caminho ).convert( 'RGBA' ).resize( ( t, t ), Image.LANCZOS )
        out.paste( ic, ( 0, 0 ), ic )
    except Exception:
        pass
    return out


def girar( t, direita, on ):
    big = Image.new( 'RGB', ( t * SS, t * SS ), BG )  # (fundo do painel: a borda suavizada não puxa para o rosa)
    d = ImageDraw.Draw( big )
    d.ellipse( ( 0, 0, t * SS - 1, t * SS - 1 ), fill=BASE if on else BOX_EDGE )
    d.ellipse( ( 2 * SS, 2 * SS, t * SS - 1 - 2 * SS, t * SS - 1 - 2 * SS ), fill=q15( ( 246, 246, 241 ) ) )
    c = t * SS / 2
    k = t * SS * 0.16
    pts = [ ( c - k * 0.6, c - k ), ( c + k * 0.8, c ), ( c - k * 0.6, c + k ) ] if direita else [ ( c + k * 0.6, c - k ), ( c - k * 0.8, c ), ( c + k * 0.6, c + k ) ]
    d.polygon( pts, fill=( 82, 114, 176 ) if on else INK )
    im = big.resize( ( t, t ), Image.LANCZOS )
    mask = Image.new( 'L', ( t * SS, t * SS ), 0 )
    ImageDraw.Draw( mask ).ellipse( ( 0, 0, t * SS - 1, t * SS - 1 ), fill=255 )
    mask = mask.resize( ( t, t ), Image.LANCZOS ).point( lambda v: 255 if v >= 128 else 0 )
    out = Image.new( 'RGB', ( t, t ), MAGENTA )
    out.paste( im, ( 0, 0 ), mask )
    return out


def mk_imagens( put ):
    for nome, texto in ( ( 'masc', 'Masculino' ), ( 'fem', 'Feminino' ) ):
        put( 'codex\\mk_%s.bmp' % nome, botao_txt( 162, 32, texto, BOX, BOX_EDGE, INK, 12, True ) )
        put( 'codex\\mk_%s_on.bmp' % nome, botao_txt( 162, 32, texto, WHITE, BASE, INK, 12, True ) )
        put( 'codex\\mk_%s_sel.bmp' % nome, botao_txt( 162, 32, texto, WHITE, BASE, ( 82, 114, 176 ), 12, True, 8, BG, 2 ) )
    put( 'codex\\mk_cabelo.bmp', caixa( 48, 48, 6, BOX, BOX_EDGE, BG ) )
    put( 'codex\\mk_cabelo_on.bmp', caixa( 48, 48, 6, WHITE, BASE, BG ) )
    put( 'codex\\mk_cabelo_sel.bmp', caixa( 48, 48, 6, q15( ( 255, 247, 214 ) ), BASE, BG, 2 ) )
    for lado, dr in ( ( 'esq', False ), ( 'dir', True ) ):
        put( 'codex\\mk_girar_%s.bmp' % lado, girar( 34, dr, False ) )
        put( 'codex\\mk_girar_%s_on.bmp' % lado, girar( 34, dr, True ) )
    for est, cor in ( ( '', BASE ), ( '_on', ( 132, 162, 216 ) ) ):
        put( 'codex\\mk_criar%s.bmp' % est, sobre_cena( botao_principal_g( SEL_LIST_W, 60, 'CRIAR', cor, 22 ), 10 ) )
    put( 'codex\\mk_voltar.bmp', sobre_cena( botao_txt( 180, 36, 'VOLTAR', BOX, BOX_EDGE, INK ), 8 ) )
    put( 'codex\\mk_voltar_on.bmp', sobre_cena( botao_txt( 180, 36, 'VOLTAR', WHITE, BASE, INK ), 8 ) )
    put( 'codex\\mk_raca.bmp', icone_classe( 0, 46, WHITE ) )
    for j in MK_JOBS:
        put( 'codex\\mk_classe_%d.bmp' % j, icone_classe( j, 30, BOX ) )
        put( 'codex\\mk_classe_%d_on.bmp' % j, icone_classe( j, 30, WHITE ) )


# ---------------------------------------------------------------- telas de entrada no estilo antigo (ui::LG2_*)
def marca_cartao():
    """cartão da marca (à esquerda do painel de login): logotipo, "O CODEX", traço e o lema"""
    w, h = 252, 190
    im = Image.new( 'RGB', ( w, h ), BG )
    lg = _logo_rgba()
    if lg is not None:
        lh = 104
        lw = int( lg.width * lh / lg.height )
        lg = lg.resize( ( lw, lh ), Image.LANCZOS )
        im.paste( lg, ( ( w - lw ) // 2, 0 ), lg )
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    serif = os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'fonts', 'Cinzel.ttf' )
    f1 = ImageFont.truetype( serif if os.path.exists( serif ) else FONTE, 24 * SS )
    f2 = ImageFont.truetype( FONTE, 12 * SS )
    for txt, f, y, cor in ( ( 'O CODEX', f1, 116, INK ), ( 'Além das Páginas de Midgard', f2, 166, ( 110, 110, 104 ) ) ):
        bb = d.textbbox( ( 0, 0 ), txt, font=f )
        d.text( ( ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], y * SS - bb[1] ), txt, fill=cor, font=f )
        if txt == 'O CODEX':
            d.text( ( ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0] + SS * 0.7, y * SS - bb[1] ), txt, fill=cor, font=f )
    d.rectangle( ( ( w / 2 - 40 ) * SS, 154 * SS, ( w / 2 + 40 ) * SS, 156 * SS ), fill=BASE )
    return big.resize( ( w, h ), Image.LANCZOS )


def botao_serif( w, h, texto, fill, tam=20 ):
    """botão principal com o texto na fonte de título (Cinzel), como o ENTRAR antigo"""
    im = caixa( w, h, 10, fill, ( 82, 114, 176 ), BG )
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    serif = os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'fonts', 'Cinzel.ttf' )
    f = ImageFont.truetype( serif if os.path.exists( serif ) else FONTE, tam * SS )
    bb = d.textbbox( ( 0, 0 ), texto, font=f )
    x, y = ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], ( h * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1]
    for dx in ( 0, SS * 0.8 ):
        d.text( ( x + dx, y ), texto, fill=WHITE, font=f )
    return big.resize( ( w, h ), Image.LANCZOS )


def entrada_imagens( put ):
    put( 'codex\\lg_marca_c.bmp', marca_cartao() )
    for est, cor in ( ( 'normal', BASE ), ( 'over', ( 132, 162, 216 ) ), ( 'press', ( 98, 130, 190 ) ) ):
        put( 'login_interface\\bt_start_%s.bmp' % est, botao_serif( 324, 54, 'ENTRAR', cor, 20 ) )
        link = ( 82, 114, 176 ) if est == 'normal' else ( 58, 86, 150 )
        put( 'login_interface\\bt_join_%s.bmp' % est, texto_img( 100, 20, 'Criar conta', link, 13, negrito=True ) )
        put( 'login_interface\\bt_close_%s.bmp' % est, texto_img( 100, 20, 'Abertura', link, 13, centro=True, negrito=True ) )
        put( 'login_interface\\bt_close2_%s.bmp' % est, texto_img( 40, 20, 'Sair', link, 13, negrito=True ) )
    put( 'codex\\sv_entrar.bmp', botao_serif( 200, 46, 'ENTRAR', BASE, 20 ) )
    put( 'codex\\sv_entrar_on.bmp', botao_serif( 200, 46, 'ENTRAR', ( 132, 162, 216 ), 20 ) )
    put( 'codex\\sv_voltar_g.bmp', botao_txt( 200, 46, 'VOLTAR', BOX, BOX_EDGE, INK, 11, True ) )
    put( 'codex\\sv_voltar_g_on.bmp', botao_txt( 200, 46, 'VOLTAR', WHITE, BASE, INK, 11, True ) )


# ---------------------------------------------------------------- RODEX (imagens rodexsystem\renewal, mesmas medidas e posições)
def _linha( d, x0, x1, y, cor ):
    d.line( ( x0, y, x1, y ), fill=cor )


def rodex_lista():
    out = Image.new( 'RGB', ( 320, 417 ), MAGENTA )
    out.paste( janela( 309, 416 ), ( 0, 0 ) )   # a janela tem 309x416
    d = ImageDraw.Draw( out )
    _linha( d, 10, 298, 16, EDGE )             # sob a área do título
    _linha( d, 29, 290, 97, EDGE )             # sob as abas
    for y in ( 162, 206, 250, 294, 338 ):     # entre as cartas (cada carta ocupa 44 px a partir de y = 118)
        _linha( d, 29, 290, y, q15( ( 222, 222, 214 ) ) )
    return out


def rodex_leitura():
    out = janela( 300, 400 )
    d = ImageDraw.Draw( out )
    _linha( d, 10, 289, 16, EDGE )
    _linha( d, 10, 289, 66, EDGE )
    out.paste( caixa( 282, 220, 6, WHITE, BOX_EDGE, BG ), ( 9, 71 ) )        # texto da carta
    _linha( d, 10, 289, 295, EDGE )
    for x in ( 27, 61, 95, 129, 163 ):                                         # anexos
        out.paste( caixa( 30, 16, 5, BOX, BOX_EDGE, BG ), ( x, 320 ) )
    out.paste( caixa( 262, 24, 6, WHITE, BOX_EDGE, BG ), ( 19, 343 ) )       # zeny / destinatário
    _linha( d, 10, 289, 372, EDGE )                                            # barra dos botões
    return out


def rodex_aba( w, texto, est ):
    """aba como as do tema: ativa = caixa branca com borda azul de 2 px. As inativas (sem texto: o cliente escreve o
    nome por cima, mais largo que a aba) ficam só com um traço embaixo"""
    if not texto or est != 'press':
        out = Image.new( 'RGB', ( w, 28 ), BG )
        ImageDraw.Draw( out ).line( ( 4, 26, w - 5, 26 ), fill=BASE if est == 'over' else EDGE, width=2 if est == 'over' else 1 )
        if not texto: return out
    fill = BOX if est == 'out' else WHITE
    edge = BOX_EDGE if est == 'out' else BASE
    im = caixa( w - 4, 24, 7, fill, edge, BG, 2 if est == 'press' else 1 )
    out = Image.new( 'RGB', ( w, 28 ), BG )
    out.paste( im, ( 2, 2 ) )
    if texto:
        big = out.resize( ( w * SS, 28 * SS ), Image.NEAREST )
        d = ImageDraw.Draw( big )
        f = ImageFont.truetype( FONTE, 11 * SS )
        bb = d.textbbox( ( 0, 0 ), texto, font=f )
        cor = ( 82, 114, 176 ) if est != 'out' else INK
        x, y = ( w * SS - ( bb[2] - bb[0] ) ) / 2 - bb[0], ( 28 * SS - ( bb[3] - bb[1] ) ) / 2 - bb[1]
        for dx in ( 0, SS * 0.6 ):
            d.text( ( x + dx, y ), texto, fill=cor, font=f )
        out = big.resize( ( w, 28 ), Image.LANCZOS )
    return out


def lupa( w, h, fill, edge ):
    im = caixa( w, h, 5, fill, edge, BG )
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    cx, cy, r = w * SS / 2 - 1.5 * SS, h * SS / 2 - 1.5 * SS, 3.6 * SS
    d.ellipse( ( cx - r, cy - r, cx + r, cy + r ), outline=( 82, 114, 176 ), width=int( 1.6 * SS ) )
    d.line( ( cx + r * 0.7, cy + r * 0.7, cx + r * 1.9, cy + r * 1.9 ), fill=( 82, 114, 176 ), width=int( 2 * SS ) )
    return big.resize( ( w, h ), Image.LANCZOS )


def seta_baixo( w, h, fill, edge, cor ):
    im = caixa( w, h, 5, fill, edge, BG )
    big = im.resize( ( w * SS, h * SS ), Image.NEAREST )
    d = ImageDraw.Draw( big )
    c = w * SS / 2
    d.line( ( c, 4 * SS, c, 13 * SS ), fill=cor, width=int( 2 * SS ) )
    d.polygon( [ ( c - 4 * SS, 10 * SS ), ( c + 4 * SS, 10 * SS ), ( c, 15 * SS ) ], fill=cor )
    d.line( ( c - 6 * SS, 16 * SS, c + 6 * SS, 16 * SS ), fill=cor, width=int( 1.5 * SS ) )
    return big.resize( ( w, h ), Image.LANCZOS )


def balao_id():
    out = Image.new( 'RGB', ( 130, 42 ), MAGENTA )
    big = Image.new( 'RGB', ( 130 * SS, 42 * SS ), MAGENTA )
    d = ImageDraw.Draw( big )
    d.polygon( [ ( 0, 21 * SS ), ( 8 * SS, 15 * SS ), ( 8 * SS, 27 * SS ) ], fill=BOX_EDGE )
    d.rounded_rectangle( ( 6 * SS, 0, 129 * SS, 41 * SS ), 6 * SS, fill=BOX_EDGE )
    d.rounded_rectangle( ( 7 * SS, 1 * SS, 128 * SS, 40 * SS ), 5 * SS, fill=WHITE )
    d.polygon( [ ( 2 * SS, 21 * SS ), ( 9 * SS, 16 * SS ), ( 9 * SS, 26 * SS ) ], fill=WHITE )
    sm = big.resize( ( 130, 42 ), Image.NEAREST )  # sem suavizar contra o magenta
    return sm


def rodex_imagens( put ):
    R = 'basic_interface\\rodexsystem\\renewal\\'
    put( R + 'bg_rodex_list.bmp', rodex_lista() )
    put( R + 'bg_rodex_read.bmp', rodex_leitura() )
    estados = ( ( 'out', 'out' ), ( 'over', 'over' ), ( 'press', 'press' ) )
    for nome, w, texto in ( ( 'search', 68, 'Buscados' ), ( 'default', 68, 'Entrada' ),
                             ( 'admin', 68, 'Notícias' ), ( 'return', 68, 'Vencidos' ) ):
        for e, est in estados:
            put( R + 'btn_listtab_%s_%s.bmp' % ( nome, e ), rodex_aba( w, texto, est ) )
    # as abas que o cliente usa de fato são as "empty" (o nome ele escreve por cima, nas cores de game::OptionButtons):
    # repouso = caixa cinza, mouse em cima = branca com borda azul, escolhida = branca com borda azul de 2 px
    for e, fill, edge, ew in ( ( 'out', BOX, BOX_EDGE, 1 ), ( 'over', WHITE, BASE, 1 ), ( 'press', WHITE, BASE, 2 ) ):
        aba = Image.new( 'RGB', ( 68, 28 ), BG )
        aba.paste( caixa( 64, 24, 6, fill, edge, BG, ew ), ( 2, 2 ) )
        put( R + 'btn_listtab_empty_%s.bmp' % e, aba )
    for e, fill, edge, cor in ( ( 'out', BOX, BOX_EDGE, INK ), ( 'over', WHITE, BASE, ( 82, 114, 176 ) ),
                               ( 'press', WHITE, BASE, ( 82, 114, 176 ) ), ( 'disable', BOX, BOX_EDGE, ( 173, 173, 165 ) ) ):
        for nome, w, h, texto, tam in ( ( 'btn_send', 70, 20, 'Enviar', 11 ), ( 'btn_reply', 70, 20, 'Responder', 11 ),
                                        ( 'btn_delete', 42, 20, 'Apagar', 9 ), ( 'btn_delete%s2', 50, 20, 'Apagar', 10 ),
                                        ( 'btn_return', 42, 20, 'Devolver', 8 ), ( 'btn_confirm_id', 70, 18, 'Procurar', 10 ),
                                        ( 'btn_confirm_id_empty', 70, 18, '', 10 ) ):
            if '%s' in nome:
                arq = ( nome % ( '_' + e ) ).replace( '_' + e + '2', '_' + e + '2' )
                arq = 'btn_delete_%s2' % e
            else:
                arq = '%s_%s' % ( nome, e )
            put( R + arq + '.bmp', botao_txt( w, h, texto, fill, edge, cor, tam, True, 5 ) if texto else caixa( w, h, 5, fill, edge, BG ) )
        put( R + 'btn_search_%s.bmp' % e, lupa( 32, 18, fill, edge ) )
        put( R + 'btn_receive_%s.bmp' % e, seta_baixo( 26, 20, fill, edge, cor ) )
    put( R + 'btn_delete_disable.bmp', botao_txt( 50, 20, 'Apagar', BOX, BOX_EDGE, ( 173, 173, 165 ), 10, True, 5 ) )
    put( R + 'btn_confirm_id_press2.bmp', botao_txt( 70, 18, 'Procurar', WHITE, BASE, ( 82, 114, 176 ), 10, True, 5 ) )
    for e, on in ( ( 'out', False ), ( 'over', True ), ( 'press', True ) ):
        put( R + 'btn_prev_%s.bmp' % e, girar( 24, False, on ) )
        put( R + 'btn_next_%s.bmp' % e, girar( 24, True, on ) )
    put( R + 'checkbox_off.bmp', marcar( 14, False ) )
    put( R + 'checkbox_on.bmp', marcar( 14, True ) )
    put( R + 'checkbox_search_off.bmp', caixa( 10, 10, 2, WHITE, BOX_EDGE, BG ) )
    put( R + 'checkbox_search_on.bmp', caixa( 10, 10, 2, BASE, BASE, BG ) )
    put( R + 'shadow_item_off.bmp', caixa( 32, 18, 5, BOX, BOX_EDGE, BG ) )
    put( R + 'shadow_item_on.bmp', caixa( 32, 18, 5, WHITE, BASE, BG ) )
    put( R + 'bg_tax.bmp', botao_txt( 112, 18, '', WHITE, BOX_EDGE, INK, 9, False, 5 ) )
    peso = caixa( 82, 32, 6, WHITE, BOX_EDGE, BG )
    big = peso.resize( ( 82 * SS, 32 * SS ), Image.NEAREST ); dd = ImageDraw.Draw( big )
    f = ImageFont.truetype( FONTE, 9 * SS )
    dd.text( ( 6 * SS, 3 * SS ), 'Peso', fill=( 110, 110, 104 ), font=f )
    put( R + 'bg_weight.bmp', big.resize( ( 82, 32 ), Image.LANCZOS ) )
    put( R + 'bg_id_balloon.bmp', balao_id() )


# ---------------------------------------------------------------- grupo / amigos: imagens originais com o branco trocado pelo painel
def _do_cliente( caminho ):
    """lê uma imagem das GRFs do cliente (ordem do DATA.INI); None se não houver"""
    import io, grf
    C = os.path.join( os.path.dirname( __file__ ), '..', '..', '..', 'Cliente 2025' ) + os.sep
    chave = ( 'data\\texture\\' + PASTA.split( 'texture\\' )[1] + caminho ).encode( 'cp949' ).lower()
    for n in ( 'estilo.grf', 'codex2.grf', 'codex.grf', 'default.grf', 'data.grf', 'kro.grf' ):
        try:
            g = grf.Grf( C + n )
        except Exception:
            continue
        if chave in g.entries:
            return Image.open( io.BytesIO( g.read( chave ) ) ).convert( 'RGB' )
    return None


def _branco_para_painel( im ):
    px = im.load()
    for y in range( im.height ):
        for x in range( im.width ):
            r, g, b = px[x, y]
            if r >= 246 and g >= 246 and b >= 246:
                px[x, y] = BG
    return im


def grupo_imagens( put ):
    nomes = [ 'renewalparty\\img_friend1.bmp', 'renewalparty\\img_friend2.bmp', 'renewalparty\\bg_partymember.bmp',
              'basic_interface\\mesbtn_left.bmp', 'basic_interface\\mesbtn_mid.bmp', 'basic_interface\\mesbtn_right.bmp' ]
    for i in ( '01', '02', '03', '04', '05', '06', '07', '08', '09', '010', '011', 'partymaster_01' ):
        for suf in ( '', '_a', '_b', '_disable' ):
            nomes.append( 'basic_interface\\mesbtn_%s%s.bmp' % ( i, suf ) )
    for n in nomes:
        if 'mesbtn' in n: continue  # (os botões vêm de mesbtn_tema)
        im = _do_cliente( n )
        if im is not None:
            put( n, _branco_para_painel( im ) )
    mesbtn_tema( put )


def _icone( im, cor=None ):
    """traços do ícone original (escuros) como máscara; cor = None mantém a cor original"""
    rgb = im.convert( 'RGB' )
    a = Image.new( 'L', rgb.size, 0 )
    pr, pa = rgb.load(), a.load()
    for y in range( rgb.height ):
        for x in range( rgb.width ):
            r, g, b = pr[x, y]
            if ( r, g, b ) == ( 255, 0, 255 ): continue
            lum = ( r * 3 + g * 6 + b ) / 10
            pa[x, y] = int( max( 0, min( 1, ( 200 - lum ) / 120 ) ) * 255 )
    tinta = Image.new( 'RGB', rgb.size, cor ) if cor else rgb
    return tinta, a


def mesbtn_tema( put ):
    # barra de baixo da janela de grupo/amigos: só o fundo do painel (a linha ficava solta sob os ícones)
    for n in ( 'left', 'mid', 'right' ):
        put( 'basic_interface\\mesbtn_%s.bmp' % n, Image.new( 'RGB', ( 7, 20 ), BG ) )
    # minimizar das janelas antigas (sys_mini, era a bolinha azul): traço como o X; restaurar (10x10): quadradinho
    for nome, cor in ( ( 'sys_mini_off', ( 90, 90, 90 ) ), ( 'sys_mini_on', BASE ) ):
        im = Image.new( 'RGB', ( 11, 11 ), MAGENTA )
        ImageDraw.Draw( im ).rectangle( ( 2, 6, 8, 7 ), fill=cor )  # 2 px, sem suavizar (o fundo atrás varia)
        put( nome + '.bmp', im )
        put( 'basic_interface\\' + nome + '.bmp', im )
    for est, cor in ( ( 'out', ( 90, 90, 90 ) ), ( 'over', BASE ), ( 'press', BASE ) ):
        im = Image.new( 'RGB', ( 10, 10 ), BG )
        ImageDraw.Draw( im ).rectangle( ( 2, 2, 7, 7 ), outline=cor, width=1 )
        ImageDraw.Draw( im ).line( ( 2, 3, 7, 3 ), fill=cor )
        put( 'renewalparty\\btn_titlebar_maximum_%s.bmp' % est, im )
    # redimensionar (13x13 no canto de baixo à direita): só os traços, o resto magenta para o canto arredondado
    # da janela aparecer (o original era um quadrado listrado que ficava para fora do canto)
    grip = Image.new( 'RGB', ( 13, 13 ), MAGENTA )
    dg = ImageDraw.Draw( grip )
    for a in ( 3, 6 ):
        dg.line( ( a, 10, 10, a ), fill=( 173, 173, 165 ) )
    put( 'btn_resize.bmp', grip )
    # menu do Esc (UIEscOptionWnd): botões 221x20 com o nome escrito (a = normal, b = mouse em cima, c = apertado)
    for nome, texto in ( ( 'esc_01', 'Selecionar Personagem' ), ( 'esc_001', 'Configurações' ),
                         ( 'esc_08', 'Configurações de Atalho' ), ( 'esc_09', 'Sair do Jogo' ),
                         ( 'esc_03', 'Fechar Jogo' ), ( 'esc_04', 'Voltar para o Ponto de Retorno' ),
                         ( 'esc_02', 'Continuar a Jogar' ), ( 'esc_05', 'Ressuscitar' ),
                         ( 'esc_10', 'Voltar para o Ponto de Retorno' ) ):
        perigo = nome in ( 'esc_09', 'esc_03' )
        cor = ( 196, 70, 70 ) if perigo else INK
        borda = ( 196, 70, 70 ) if perigo else BASE
        cor_on = ( 196, 70, 70 ) if perigo else ( 82, 114, 176 )
        put( nome + 'a.bmp', botao_txt( 221, 20, texto, BOX, BOX_EDGE, cor, 11, True, 6 ) )
        put( nome + 'b.bmp', botao_txt( 221, 20, texto, WHITE, borda, cor_on, 11, True, 6 ) )
        put( nome + 'c.bmp', botao_txt( 221, 20, texto, WHITE, borda, cor_on, 11, True, 6, BG, 2 ) )
    # fundo das caixas de mensagem antigas (win_msgbox 280x120: sair do jogo, reiniciar ao morrer...): painel do
    # tema com a linha do título; os cantos de fora em magenta
    caixa_msg = janela( 280, 120 )
    _linha( ImageDraw.Draw( caixa_msg ), 10, 269, 16, EDGE )
    put( 'win_msgbox.bmp', caixa_msg )
    # opções Amigos/Grupo e Assunto/Remetente (UIRadioBtn com as imagens próprias: game::OptionButtons)
    for nome, w, h, texto, tam in ( ( 'op_amigos', 62, 17, 'Amigos', 10 ), ( 'op_grupo', 62, 17, 'Grupo', 10 ),
                                    ( 'op_assunto', 58, 18, 'Assunto', 10 ), ( 'op_remetente', 58, 18, 'Remetente', 9 ) ):
        put( 'codex\\%s.bmp' % nome, botao_txt( w, h, texto, BOX, BOX_EDGE, INK, tam, True, 6 ) )
        put( 'codex\\%s_on.bmp' % nome, botao_txt( w, h, texto, WHITE, BASE, ( 82, 114, 176 ), tam, True, 6, BG, 2 ) )
    for i in ( '01', '02', '03', '04', '05', '06', '07', '08', '09', '010', '011', 'partymaster_01' ):
        orig = _do_cliente( 'basic_interface\\mesbtn_%s.bmp' % i )
        if orig is None: continue
        for suf, fill, edge, cor, ew in ( ( '', BOX, BOX_EDGE, INK, 1 ), ( '_a', WHITE, BASE, ( 82, 114, 176 ), 1 ),
                                         ( '_b', WHITE, BASE, ( 82, 114, 176 ), 2 ), ( '_disable', BOX, BOX_EDGE, ( 173, 173, 165 ), 1 ) ):
            fundo = caixa( 24, 20, 5, fill, edge, BG, ew )
            out = Image.new( 'RGB', ( 26, 20 ), BG )
            out.paste( fundo, ( 1, 0 ) )
            tinta, mask = _icone( orig, cor )
            ImageDraw.Draw( mask ).rectangle( ( 0, 0, 25, 19 ), outline=0, width=2 )  # (sem a moldura do original)
            out.paste( tinta, ( 0, 0 ), mask )
            put( 'basic_interface\\mesbtn_%s%s.bmp' % ( i, suf ), out )
    # botão de opção (Amigo / Grupo)
    for n, on in ( ( 'radiobtn_off', False ), ( 'radiobtn_on', True ), ( 'radiobtn_on1', True ) ):
        orig = _do_cliente( n + '.bmp' )
        w, h = orig.size if orig else ( 12, 12 )
        big = Image.new( 'RGB', ( w * SS, h * SS ), BG )
        d = ImageDraw.Draw( big )
        t = min( w, h ) * SS
        d.ellipse( ( 0, 0, t - 1, t - 1 ), fill=BASE if on else BOX_EDGE )
        d.ellipse( ( SS, SS, t - 1 - SS, t - 1 - SS ), fill=WHITE )
        if on: d.ellipse( ( t * 0.28, t * 0.28, t * 0.72, t * 0.72 ), fill=BASE )
        put( n + '.bmp', big.resize( ( w, h ), Image.LANCZOS ) )


# ---------------------------------------------------------------- conquistas (UIAchievementWnd): mesmas medidas das originais
def _icone_tema( im, fundo ):
    """ícone redondo azul do original (círculo + desenho branco) nos tons do tema: claro -> fundo/branco,
    médio -> destaque, escuro -> destaque escuro"""
    im = im.convert( 'RGB' )
    out = im.copy(); px = out.load(); src = im.load()
    for y in range( im.height ):
        for x in range( im.width ):
            r, g, b = src[x, y]
            lum = ( r * 3 + g * 6 + b ) / 2550
            sat = max( r, g, b ) - min( r, g, b )
            if lum >= 0.97 and sat < 20: px[x, y] = fundo
            elif lum > 0.86: px[x, y] = WHITE
            elif lum > 0.55: px[x, y] = _mistura( BASE, ( 206, 214, 231 ), ( lum - 0.55 ) / 0.31 )
            else: px[x, y] = _mistura( ( 82, 114, 176 ), BASE, max( 0, ( lum - 0.25 ) / 0.30 ) )
    return out


def _mistura( a, b, t ):
    t = max( 0.0, min( 1.0, t ) )
    return tuple( int( a[i] + ( b[i] - a[i] ) * t + 0.5 ) for i in range( 3 ) )


def _trilho( d, x0, y0, x1, y1 ):
    d.rounded_rectangle( ( x0, y0, x1, y1 ), ( y1 - y0 ) // 2, fill=( 222, 222, 214 ) )


def conquistas_imagens( put ):
    A = 'achievement_re\\'
    orig = lambda n: _do_cliente( A + n )
    # barra de cima (750x46): medalhão do nível à esquerda (o cliente escreve o número), divisória, e a caixa da
    # pontuação no meio (o troféu upper_trophy e o número o cliente desenha por cima)
    up = Image.new( 'RGB', ( 750, 46 ), BG )
    d = ImageDraw.Draw( up )
    d.line( ( 0, 45, 749, 45 ), fill=EDGE )
    d.line( ( 139, 4, 139, 41 ), fill=EDGE )
    big = Image.new( 'RGB', ( 40 * SS, 40 * SS ), BG )
    db = ImageDraw.Draw( big )
    db.ellipse( ( 1 * SS, 1 * SS, 38 * SS, 38 * SS ), fill=BASE )
    db.ellipse( ( 3 * SS, 3 * SS, 36 * SS, 36 * SS ), fill=WHITE )
    db.ellipse( ( 6 * SS, 6 * SS, 33 * SS, 33 * SS ), outline=( 206, 214, 231 ), width=SS )
    up.paste( big.resize( ( 40, 40 ), Image.LANCZOS ), ( 4, 3 ) )
    up.paste( caixa( 152, 40, 8, WHITE, BASE, BG ), ( 300, 3 ) )
    put( A + 'bg_upper.bmp', up )
    # coluna das abas (102x405) e as abas (100x40 / sub 100x26; o nome o cliente escreve)
    col = Image.new( 'RGB', ( 102, 405 ), BG )
    ImageDraw.Draw( col ).line( ( 101, 0, 101, 404 ), fill=EDGE )
    put( A + 'bg_tab.bmp', col )
    for est, fill, faixa in ( ( 'out', BG, None ), ( 'over', WHITE, None ), ( 'press', WHITE, BASE ) ):
        t = Image.new( 'RGB', ( 100, 40 ), fill )
        dt = ImageDraw.Draw( t )
        dt.line( ( 6, 39, 93, 39 ), fill=EDGE )
        if faixa: dt.rectangle( ( 0, 0, 2, 39 ), fill=faixa )
        put( A + 'tab_%s.bmp' % est, t )
        st = Image.new( 'RGB', ( 100, 26 ), ( 246, 246, 241 ) if est == 'out' else fill )
        ds = ImageDraw.Draw( st )
        if faixa: ds.rectangle( ( 0, 0, 2, 25 ), fill=faixa )
        put( A + 'tab_sub_%s.bmp' % est, st )
    seta = Image.new( 'RGB', ( 10, 11 ), MAGENTA )
    ImageDraw.Draw( seta ).polygon( [ ( 3, 2 ), ( 7, 5 ), ( 3, 8 ) ], fill=BASE )
    put( A + 'tab_subarrow.bmp', seta )
    # resumo (648x404): cartão do total, cartão das categorias e os dois "recentes"; ícones do original no tema
    sm = Image.new( 'RGB', ( 648, 404 ), BG )
    sm.paste( caixa( 629, 71, 8, WHITE, BOX_EDGE, BG ), ( 8, 6 ) )
    sm.paste( caixa( 629, 182, 8, WHITE, BOX_EDGE, BG ), ( 8, 82 ) )
    o = orig( 'bg_summary.bmp' )
    d = ImageDraw.Draw( sm )
    for ix, iy in ( ( 42, 16 ), ( 42, 90 ), ( 346, 90 ), ( 42, 140 ), ( 346, 140 ), ( 42, 190 ), ( 346, 190 ) ):
        if o: sm.paste( _icone_tema( o.crop( ( ix, iy, ix + 33, iy + 32 ) ), WHITE ), ( ix, iy ) )
    _trilho( d, 44, 50, 600, 57 )
    for by in ( 124, 174, 224 ):
        _trilho( d, 44, by, 296, by + 7 ); _trilho( d, 348, by, 600, by + 7 )
    for rx in ( 8, 327 ):
        card = caixa( 312, 82, 8, WHITE, BOX_EDGE, BG )
        dc = ImageDraw.Draw( card )
        dc.rectangle( ( 1, 1, 310, 38 ), fill=( 246, 246, 241 ) )
        dc.line( ( 1, 39, 310, 39 ), fill=EDGE )
        card.paste( caixa( 40, 23, 6, WHITE, BOX_EDGE, ( 246, 246, 241 ) ), ( 251, 8 ) )
        mask = Image.new( 'L', ( 312, 82 ), 0 )
        ImageDraw.Draw( mask ).rounded_rectangle( ( 0, 0, 311, 81 ), 8, fill=255 )
        base = caixa( 312, 82, 8, WHITE, BOX_EDGE, BG ); base.paste( card, ( 0, 0 ), mask )
        sm.paste( base, ( rx, 310 ) )
    put( A + 'bg_summary.bmp', sm )
    # lista (309x405) e as linhas (309x81): repouso caixa cinza, escolhida branca com borda azul de 2 px e faixa;
    # completas com o topo em azul claro
    put( A + 'bg_list.bmp', Image.new( 'RGB', ( 309, 405 ), BG ) )
    for nome, fill, edge, ew, topo, faixa in (
            ( 'list_out', BOX, BOX_EDGE, 1, None, False ), ( 'list_press', WHITE, BASE, 2, None, True ),
            ( 'list_complete_out', BOX, BOX_EDGE, 1, ( 206, 214, 231 ), False ),
            ( 'list_complete_press', WHITE, BASE, 2, ( 206, 214, 231 ), True ),
            ( 'list_disable', q15( ( 231, 231, 225 ) ), q15( ( 222, 222, 214 ) ), 1, None, False ) ):
        li = Image.new( 'RGB', ( 309, 81 ), BG )
        c = caixa( 307, 79, 6, fill, edge, BG, ew )
        dc = ImageDraw.Draw( c )
        if topo: dc.rectangle( ( ew, ew, 306 - ew, 37 ), fill=topo )
        dc.line( ( 8, 38, 298, 38 ), fill=EDGE )
        if faixa: dc.rectangle( ( ew, 6, ew + 2, 72 ), fill=BASE )
        c.paste( caixa( 40, 23, 6, WHITE, BOX_EDGE, topo or fill ), ( 257, 7 ) )
        li.paste( c, ( 1, 1 ) )
        put( A + nome + '.bmp', li )
    # detalhe (328x405): cartão branco com faixa do título e as divisórias nas mesmas alturas
    dt = Image.new( 'RGB', ( 328, 405 ), BG )
    dt.paste( caixa( 326, 403, 8, WHITE, BOX_EDGE, BG ), ( 1, 1 ) )
    dd = ImageDraw.Draw( dt )
    dd.rounded_rectangle( ( 6, 7, 240, 30 ), 6, fill=( 206, 214, 231 ) )
    dd.rectangle( ( 2, 93, 325, 289 ), fill=( 246, 246, 241 ) )
    dd.line( ( 8, 92, 319, 92 ), fill=EDGE ); dd.line( ( 8, 290, 319, 290 ), fill=EDGE )
    put( A + 'bg_detail.bmp', dt )
    # botões com o nome escrito (Resgatar) e as opções
    for est, fill, edge, cor, ew in ( ( 'out', BOX, BOX_EDGE, INK, 1 ), ( 'over', WHITE, BASE, ( 82, 114, 176 ), 1 ),
                                     ( 'press', WHITE, BASE, ( 82, 114, 176 ), 2 ), ( 'disable', BOX, BOX_EDGE, ( 173, 173, 165 ), 1 ) ):
        put( A + 'btn_receive_%s.bmp' % est, botao_txt( 148, 20, 'Resgatar', fill, edge, cor, 11, True, 6, BG, ew ) )
    for suf, fill, edge, cor, ew in ( ( 'a', BOX, BOX_EDGE, INK, 1 ), ( 'b', WHITE, BASE, ( 82, 114, 176 ), 1 ),
                                     ( 'c', WHITE, BASE, ( 82, 114, 176 ), 2 ), ( 'd', BOX, BOX_EDGE, ( 173, 173, 165 ), 1 ) ):
        put( A + 'reward_%s.bmp' % suf, botao_txt( 221, 20, 'Resgatar', fill, edge, cor, 11, True, 6, BG, ew ) )
    for n, on in ( ( 'btn_radio_off', False ), ( 'btn_radio_on', True ) ):
        big = Image.new( 'RGB', ( 12 * SS, 12 * SS ), BG )
        db = ImageDraw.Draw( big )
        db.ellipse( ( 0, 0, 12 * SS - 1, 12 * SS - 1 ), fill=BASE if on else BOX_EDGE )
        db.ellipse( ( SS, SS, 11 * SS - 1, 11 * SS - 1 ), fill=WHITE )
        if on: db.ellipse( ( 3.5 * SS, 3.5 * SS, 8.5 * SS, 8.5 * SS ), fill=BASE )
        put( A + n + '.bmp', big.resize( ( 12, 12 ), Image.LANCZOS ) )
    # rolagem (11 px): trilho claro, puxador cinza arredondado, setas discretas
    tr = Image.new( 'RGB', ( 11, 11 ), ( 246, 246, 241 ) )
    ImageDraw.Draw( tr ).line( ( 0, 0, 0, 10 ), fill=EDGE )
    put( A + 'scroll_v_bg_mid.bmp', tr ); put( A + 'scroll_v_bg_down.bmp', tr )
    for n, pts in ( ( 'up', [ ( 2, 7 ), ( 8, 7 ), ( 5, 3 ) ] ), ( 'down', [ ( 2, 4 ), ( 8, 4 ), ( 5, 8 ) ] ) ):
        a = Image.new( 'RGB', ( 11, 11 ), ( 246, 246, 241 ) )
        ImageDraw.Draw( a ).polygon( pts, fill=( 110, 110, 104 ) )
        put( A + 'scroll_v_arrow_%s.bmp' % n, a )
    th = Image.new( 'RGB', ( 11, 12 ), ( 246, 246, 241 ) )
    ImageDraw.Draw( th ).rounded_rectangle( ( 2, 0, 8, 11 ), 3, fill=( 173, 173, 165 ) )
    put( A + 'scroll_v_thumb_up.bmp', th.crop( ( 0, 0, 11, 4 ) ) )
    put( A + 'scroll_v_thumb_down.bmp', th.crop( ( 0, 8, 11, 12 ) ) )
    put( A + 'scroll_v_thumb_mid.bmp', th.crop( ( 0, 4, 11, 8 ) ) )


def botao_basico_imagens( put ):
    for est, fill, edge in ( ( 'out', BOX, BOX_EDGE ), ( 'over', WHITE, BASE ), ( 'press', q15( ( 222, 230, 247 ) ), BASE ),
                             ( 'disable', q15( ( 231, 231, 225 ) ), q15( ( 222, 222, 214 ) ) ) ):
        im = caixa( 18, 20, 5, fill, edge, BG )
        for n, x in ( ( 'left', 0 ), ( 'mid', 6 ), ( 'right', 12 ) ):
            put( 'basic_interface\\btn_%s_%s.bmp' % ( est, n ), im.crop( ( x, 0, x + 6, 20 ) ) )


def barra_titulo_imagens( put ):
    """barra de título das janelas antigas do cliente (titlebar_left/mid/right 12x17): faixa do painel com cantos
    arredondados e uma linha fina embaixo, no lugar do azul"""
    w, h = 36, 17
    big = Image.new( 'RGB', ( w * SS, h * SS ), MAGENTA )
    d = ImageDraw.Draw( big )
    d.rounded_rectangle( ( 0, 0, w * SS - 1, ( h + 10 ) * SS ), 8 * SS, fill=EDGE )
    d.rounded_rectangle( ( 1 * SS, 1 * SS, w * SS - 1 - SS, ( h + 10 ) * SS ), 7 * SS, fill=BG )
    d.rectangle( ( 0, ( h - 1 ) * SS, w * SS, h * SS ), fill=EDGE )
    im = big.resize( ( w, h ), Image.NEAREST )
    for n, x in ( ( 'left', 0 ), ( 'mid', 12 ), ( 'right', 24 ) ):
        put( 'basic_interface\\titlebar_%s.bmp' % n, im.crop( ( x, 0, x + 12, h ) ) )
    # botão "minimizar" (bolinha azul à esquerda): ponto discreto do tema
    dot = Image.new( 'RGB', ( 11 * SS, 11 * SS ), BG )
    ImageDraw.Draw( dot ).ellipse( ( 2 * SS, 2 * SS, 9 * SS, 9 * SS ), fill=BOX_EDGE )
    put( 'basic_interface\\sys_base_off.bmp', dot.resize( ( 11, 11 ), Image.LANCZOS ) )
    dot2 = Image.new( 'RGB', ( 11 * SS, 11 * SS ), BG )
    ImageDraw.Draw( dot2 ).ellipse( ( 2 * SS, 2 * SS, 9 * SS, 9 * SS ), fill=BASE )
    put( 'basic_interface\\sys_base_on.bmp', dot2.resize( ( 11, 11 ), Image.LANCZOS ) )

def imagens():
    k = lambda v: int( v * 0.86 + 0.5 )
    out = {}
    def put( nome, im ):
        out[( PASTA + nome ).encode( 'cp949' ).lower()] = im
    put( 'codex\\personagem_bg.bmp', janela( 416, 514 ) )
    put( 'codex\\equipamento_bg.bmp', janela( 416, 284 ) )
    put( 'codex\\personagem_abas_bg.bmp', janela( 280, 196 ) )  # abas Título e Dano (ui::CHAR_TAB_*)
    put( 'codex\\slot.bmp', caixa( k( 38 ), k( 38 ), 6, BOX, BOX_EDGE, BG ) )
    put( 'codex\\visual.bmp', botao( k( 108 ), k( 28 ), 'Mostrar Visual', BOX, BOX_EDGE ) )
    put( 'codex\\titulos.bmp', botao( k( 64 ), k( 24 ), 'Títulos', BOX, BOX_EDGE ) )
    put( 'codex\\titulos_on.bmp', botao( k( 64 ), k( 24 ), 'Títulos', WHITE, BASE ) )
    put( 'codex\\dano.bmp', botao( k( 52 ), k( 24 ), 'Dano', BOX, BOX_EDGE ) )
    put( 'codex\\dano_on.bmp', botao( k( 52 ), k( 24 ), 'Dano', WHITE, BASE ) )
    put( 'codex\\visual_on.bmp', botao( k( 108 ), k( 28 ), 'Mostrar Visual', WHITE, BASE ) )
    put( 'codex\\mais.bmp', caixa( k( 18 ), k( 18 ), 4, BOX, BOX_EDGE, BG ) )
    # fechar (UIButton nativo da janela: normal, em cima, apertado) e a caixa "Exibir Equip." (UICheckBox nativo)
    put( 'codex\\fechar.bmp', fechar( k( 22 ), BOX ) )
    put( 'codex\\fechar_on.bmp', fechar( k( 22 ), WHITE ) )
    put( 'codex\\caixa.bmp', marcar( 14, False ) )
    put( 'codex\\caixa_on.bmp', marcar( 14, True ) )
    # janela de habilidades (src/skillwnd.cpp): fundo, linha da lista (normal e com o mouse), abas, "+",
    # setas de nível e Aplicar/Cancelar
    put( 'codex\\habilidades_bg.bmp', janela( SK_W, SK_H ) )
    put( 'codex\\sk_linha.bmp', caixa( SK_ROW_W, SK_ROW_H, 6, BOX, BOX_EDGE, BG ) )
    put( 'codex\\sk_linha_on.bmp', caixa( SK_ROW_W, SK_ROW_H, 6, WHITE, BOX_EDGE, BG ) )
    for tw in SK_TAB_WS:  # abas em 3 larguras (cabem 3, 4 ou 5 na janela)
        put( 'codex\\sk_aba%d.bmp' % tw, caixa( tw, SK_TAB_H, 7, BOX, BOX_EDGE, BG ) )
        put( 'codex\\sk_aba%d_on.bmp' % tw, caixa( tw, SK_TAB_H, 7, WHITE, BASE, BG, 2 ) )
    put( 'codex\\sk_mais.bmp', mais( 16, WHITE, BOX_EDGE ) )
    put( 'codex\\sk_mais_on.bmp', mais( 16, WHITE, BASE ) )
    put( 'codex\\sk_esq.bmp', seta( 11, False ) )
    put( 'codex\\sk_dir.bmp', seta( 11, True ) )
    put( 'codex\\sk_aplicar.bmp', botao( 76, 24, 'Aplicar', BOX, BOX_EDGE ) )
    put( 'codex\\sk_aplicar_on.bmp', botao( 76, 24, 'Aplicar', WHITE, BASE ) )
    put( 'codex\\sk_cancelar.bmp', botao( 76, 24, 'Cancelar', BOX, BOX_EDGE ) )
    put( 'codex\\sk_cancelar_on.bmp', botao( 76, 24, 'Cancelar', WHITE, BASE ) )
    # inventário e armazém (src/charwnd.cpp: InstallInventory / InstallStorage)
    put( 'codex\\inventario_bg.bmp', janela( INV_W, INV_H ) )
    put( 'codex\\inv_celula.bmp', caixa( INV_CELL, INV_CELL, 5, BOX, BOX_EDGE, BG ) )
    put( 'codex\\inv_aba.bmp', caixa( INV_TAB_W, INV_TAB_H, 7, BOX, BOX_EDGE, BG ) )
    put( 'codex\\inv_aba_on.bmp', caixa( INV_TAB_W, INV_TAB_H, 7, WHITE, BASE, BG, 2 ) )
    put( 'codex\\armazem_bg.bmp', janela( ST_W, ST_H ) )
    put( 'codex\\carrinho_bg.bmp', janela( CART_W, CART_H ) )
    put( 'codex\\pesquisa_bg.bmp', janela( SR_W, SR_H ) )
    put( 'codex\\st_linha.bmp', caixa( ST_ROW_W, ST_ROW_H, 6, BOX, BOX_EDGE, BG ) )
    put( 'codex\\st_aba.bmp', caixa( ST_TAB_W, ST_TAB_H, 7, BOX, BOX_EDGE, BG ) )
    put( 'codex\\st_aba_on.bmp', caixa( ST_TAB_W, ST_TAB_H, 7, WHITE, BASE, BG, 2 ) )
    # descrição do item (moldura, topo, encaixes de carta) e o X das janelas do cliente
    for nome, im in moldura( EDGE ).items():
        put( 'sysbox_%s.bmp' % nome, im )
    for nome, im in moldura( BASE ).items():
        put( 'sysbox_comparison_%s.bmp' % nome, im )
        put( 'basic_interface\\sysbox_comparison_%s.bmp' % nome, im )
    for g in range( 1, 5 ):
        for nome, im in moldura( q15( GRAU[g] ) ).items():
            put( 'sysbox_g%d_%s.bmp' % ( g, nome ), im )
    put( 'basic_interface\\collection_bg.bmp', topo_item( EDGE ) )
    put( 'basic_interface\\collection_comparison_bg.bmp', topo_item( BASE ) )
    for g, cor in GRAU.items():
        put( 'basic_interface\\collection_bg_g%d.bmp' % g, topo_item( q15( cor ) ) )
    put( 'empty_card_slot.bmp', caixa( 24, 24, 5, WHITE, BOX_EDGE, BG ) )
    put( 'disable_card_slot.bmp', caixa( 24, 24, 5, BOX_EDGE, BOX_EDGE, BG ) )
    put( 'basic_interface\\comparison_empty_card_slot.bmp', caixa( 24, 24, 5, WHITE, BOX_EDGE, BG ) )
    put( 'basic_interface\\comparison_disable_card_slot.bmp', caixa( 24, 24, 5, BOX_EDGE, BOX_EDGE, BG ) )
    for tag, cor in RUNAS.items():
        put( 'codex\\runa_%s.bmp' % tag, runa( cor ) )
    put( 'basic_interface\\sys_close_off.bmp', xis( ( 90, 90, 90 ) ) )
    put( 'basic_interface\\sys_close_on.bmp', xis( BASE ) )
    # aba Dano da janela Personagem (conteúdo do cliente: showdamage\\*) e os botões ok/fechar das janelas
    fundo_dano = Image.new( 'RGB', ( 280, 133 ), BG )
    ImageDraw.Draw( fundo_dano ).line( ( 8, 24, 271, 24 ), fill=EDGE )
    put( 'showdamage\\bg_damage.bmp', fundo_dano )
    put( 'showdamage\\btn_damage.bmp', painel_dano( BOX, BOX_EDGE, 1 ) )
    put( 'showdamage\\btn_damage_pick.bmp', painel_dano( WHITE, BASE, 2 ) )
    put( 'showdamage\\btn_damage_press.bmp', painel_dano( WHITE, BASE, 1 ) )
    for nome, texto in ( ( 'btn_apply', 'Ok' ), ( 'btn_close', 'Fechar' ), ( 'btn_ok', 'Ok' ), ( 'btn_cancel', 'Cancelar' ) ):
        put( nome + '.bmp', botao( 42, 20, texto, BOX, BOX_EDGE ) )
        put( nome + '_a.bmp', botao( 42, 20, texto, WHITE, BASE ) )
        put( nome + '_b.bmp', botao( 42, 20, texto, WHITE, BASE ) )
        put( 'basic_interface\\' + nome + '.bmp', botao( 42, 20, texto, BOX, BOX_EDGE ) )
        put( 'basic_interface\\' + nome + '_a.bmp', botao( 42, 20, texto, WHITE, BASE ) )
        put( 'basic_interface\\' + nome + '_b.bmp', botao( 42, 20, texto, WHITE, BASE ) )
    # janela de login (UILoginWnd como painel lateral: ui::PaintLoginWindow / game::SubstituteLoginWindow)
    put( 'codex\\login_marca.bmp', marca_login() )
    fw = LG_W - 2 * LG_M
    for est, cor in ( ( 'normal', BASE ), ( 'over', ( 132, 162, 216 ) ), ( 'press', ( 98, 130, 190 ) ) ):
        put( 'login_interface\\bt_start_%s.bmp' % est, botao_principal( fw, 44, 'ENTRAR', cor ) )
        link = ( 82, 114, 176 ) if est == 'normal' else ( 115, 148, 206 )
        put( 'login_interface\\bt_join_%s.bmp' % est, texto_img( 100, 20, 'Criar conta', link ) )
        put( 'login_interface\\bt_close_%s.bmp' % est, texto_img( 100, 20, 'Abertura', link, centro=True ) )
        sair = ( 160, 70, 70 ) if est == 'normal' else ( 196, 70, 70 )
        img = texto_img( 100, 20, 'Sair do jogo', sair )
        # alinhado à direita
        big = Image.new( 'RGB', ( 100, 20 ), BG ); bb = img.getbbox()
        put( 'login_interface\\bt_close2_%s.bmp' % est, img )
    put( 'login_interface\\checkbox_off.bmp', marcar( 16, False ) )
    put( 'login_interface\\checkbox_on.bmp', marcar( 16, True ) )
    # animações da entrada (ui::LoginLayout): selo, convite, brilho do ENTRAR, medalhão e halo
    for f in range( SEAL_FRAMES ):
        put( 'codex\\lg_selo_%02d.bmp' % f, selo( SEAL, f ) )
        put( 'codex\\sv_med_%02d.bmp' % f, selo( SV_MED, f, med=True ) )
        put( 'codex\\sv_medon_%02d.bmp' % f, selo( SV_MED, f, med=True, aceso=True ) )
    for nv in range( 4 ):
        put( 'codex\\lg_convite_%d.bmp' % nv, convite( nv ) )
    for nv in range( 6 ):
        put( 'codex\\lg_halo_%d.bmp' % nv, halo_ret( LG_W - 2 * LG_M + 10, 44 + 10, 14, nv ) )
        put( 'codex\\sv_halo_%d.bmp' % nv, halo_circ( SV_MED + 28, nv ) )
    # escolha do servidor (UISelectServerWnd: ui::PaintServerWindow / game::SubstituteServerWindow)
    put( 'codex\\sv_medalhao.bmp', medalhao( SV_MED, False ) )
    put( 'codex\\sv_medalhao_on.bmp', medalhao( SV_MED, True ) )
    put( 'codex\\sv_voltar.bmp', botao( SV_BACK[0], SV_BACK[1], 'Voltar', BOX, BOX_EDGE ) )
    put( 'codex\\sv_voltar_on.bmp', botao( SV_BACK[0], SV_BACK[1], 'Voltar', WHITE, BASE ) )
    # botão do carrinho da própria janela de equipamentos (UIButton: normal, em cima, apertado)
    put( 'basic_interface\\btn_items_off.bmp', botao( k( 92 ), k( 28 ), 'Carrinho', BOX, BOX_EDGE ) )
    put( 'basic_interface\\btn_items_on.bmp', botao( k( 92 ), k( 28 ), 'Carrinho', WHITE, BASE ) )
    # conversa com NPC: botões seguir / fechar da faixa (game::SubstituteNpcWindows)
    for nome, texto in ( ( 'npc_seguir', 'Continuar  ›' ), ( 'npc_fechar', 'Fechar' ) ):
        put( 'codex\\%s.bmp' % nome, botao_txt( 120, 30, texto, BOX, BOX_EDGE, INK, 12, True ) )
        put( 'codex\\%s_on.bmp' % nome, botao_txt( 120, 30, texto, WHITE, BASE, ( 82, 114, 176 ), 12, True ) )
    # lojas de NPC (ui::PaintShopWindow / game::ShopLayout)
    for nome, texto in ( ( 'shop_comprar', 'Comprar' ), ( 'shop_vender', 'Vender' ), ( 'shop_cancelar', 'Cancelar' ) ):
        put( 'codex\\%s.bmp' % nome, botao_txt( 76, 24, texto, BOX, BOX_EDGE, INK, 12, True, 6 ) )
        put( 'codex\\%s_on.bmp' % nome, botao_txt( 76, 24, texto, WHITE, BASE, ( 82, 114, 176 ), 12, True, 6 ) )
        put( 'codex\\%s_p.bmp' % nome, botao_txt( 64, 24, texto, BOX, BOX_EDGE, INK, 11, True, 6 ) )
        put( 'codex\\%s_p_on.bmp' % nome, botao_txt( 64, 24, texto, WHITE, BASE, ( 82, 114, 176 ), 11, True, 6 ) )
    # janelas antigas do cliente (mercador): laterais da lista, espaço vazio e botões comprar/vender
    lat = Image.new( 'RGB', ( 20, 8 ), BG )
    ImageDraw.Draw( lat ).line( ( 0, 0, 0, 7 ), fill=EDGE )
    put( 'basic_interface\\itemwin_left.bmp', lat )
    put( 'basic_interface\\itemwin_right.bmp', lat.transpose( Image.FLIP_LEFT_RIGHT ) )
    vazio = caixa( 26, 26, 5, q15( ( 231, 231, 225 ) ), BOX_EDGE, BG )
    slot = Image.new( 'RGB', ( 32, 32 ), BG ); slot.paste( vazio, ( 3, 3 ) )  # sem magenta: na superfície ele vira buraco
    put( 'basic_interface\\itemwin_mid.bmp', slot )
    put( 'codex\\item_casa.bmp', caixa( 30, 30, 6, q15( ( 231, 231, 225 ) ), BOX_EDGE, BG ) )  # casa do item (charwnd: gancho 0x86FFF0)
    for nome, texto in ( ( 'btn_buy', 'Comprar' ), ( 'btn_sell', 'Vender' ), ( 'btn_cancel', 'Cancelar' ) ):
        put( nome + '.bmp', botao_txt( 42, 20, texto, BOX, BOX_EDGE, INK, 9, True, 5 ) )
        put( nome + '_a.bmp', botao_txt( 42, 20, texto, WHITE, BASE, ( 82, 114, 176 ), 9, True, 5 ) )
        put( nome + '_b.bmp', botao_txt( 42, 20, texto, WHITE, BASE, ( 82, 114, 176 ), 9, True, 5 ) )
    sel_imagens( put )
    mk_imagens( put )
    entrada_imagens( put )  # (depois: substitui os botões de login do painel lateral)
    rodex_imagens( put )
    grupo_imagens( put )
    conquistas_imagens( put )
    botao_basico_imagens( put )
    barra_titulo_imagens( put )
    return out


if __name__ == '__main__':
    import sys
    dst = sys.argv[1] if len( sys.argv ) > 1 else '.'
    for k_, im in imagens().items():
        im.save( os.path.join( dst, k_.decode( 'cp949' ).split( '\\' )[-1] ) )
