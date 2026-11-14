"""Reskin nativo do Alt+Q, do Alt+A e do Alt+S no estilo da proposta 2 (artifact "Janelas NexusRO").

As janelas continuam sendo do cliente (ele desenha, posiciona os textos e trata os cliques); só as imagens
próprias delas mudam, no mesmo tamanho e com as caixas nos mesmos lugares das originais:
  - Alt+Q: equipwin_bg / bg2 / bg3 / special / costume / costume2 / cash (encaixes em x 6 e 250, linhas
    de 26 px; o cliente escreve o nome do item a partir de x 35, no meio da linha);
  - Alt+A: statuswnd\\w_statwin_bg e w_ex_statwin_bg (linhas de 16 px a partir de 6; valor em 34..74,
    custo em 74..86, seta em 87; colunas de informação em 102..190 e 190..279) e as barras de expandir;
  - Alt+S: skill_upbar_*, skill_botbar_* e o botão de subir (skill_up_a/b/c).
Usado por substituicoes.py (imagens( vfs ) -> { caminho na GRF: imagem }).
"""
import io
from PIL import Image, ImageDraw, ImageFont

BASE = 'data\\texture\\유저인터페이스\\'
K = 4  # desenho em 4x e redução (bordas suaves)

BG = ( 251, 251, 254 )
CARD = ( 255, 255, 255 )
CARD_LINE = ( 227, 230, 242 )
FOOT = ( 243, 244, 250 )
TRACK = ( 236, 238, 247 )
LABEL = ( 166, 170, 192 )
MUTED = ( 138, 143, 168 )
TITLE = ( 63, 68, 102 )
ACCENT = ( 138, 146, 206 )
ACCENT_SOFT = ( 230, 232, 248 )
EMPTY_TILE = ( 244, 245, 250 )
DASH = ( 216, 219, 234 )
# cores dos atributos (as do modelo editado no artifact)
STAT_COR = [ ( 176, 138, 60 ), ( 138, 146, 206 ), ( 200, 96, 236 ), ( 60, 123, 244 ), ( 168, 100, 191 ), ( 110, 160, 122 ) ]

FONTE_B = '/mnt/c/Windows/Fonts/segoeuib.ttf'
FONTE_SB = '/mnt/c/Windows/Fonts/seguisb.ttf'


class Tela:
    """Imagem desenhada em K vezes o tamanho; coordenadas no tamanho final."""

    def __init__( self, w, h, fundo=BG ):
        self.w, self.h = w, h
        self.im = Image.new( 'RGB', ( w * K, h * K ), fundo )
        self.d = ImageDraw.Draw( self.im )

    def caixa( self, x, y, w, h, r, cor, borda=None ):
        self.d.rounded_rectangle( [ x * K, y * K, ( x + w ) * K - 1, ( y + h ) * K - 1 ], radius=r * K, fill=cor,
            outline=borda, width=K if borda else 0 )

    def rect( self, x, y, w, h, cor ):
        self.d.rectangle( [ x * K, y * K, ( x + w ) * K - 1, ( y + h ) * K - 1 ], fill=cor )

    def tracejado( self, x, y, w, h, r, cor ):
        # contorno arredondado tracejado: segmentos de 2 px com espaço de 2
        X0, Y0, X1, Y1 = x * K, y * K, ( x + w ) * K - K, ( y + h ) * K - K
        for t in range( int( ( r + 1 ) * K ), int( ( w - r - 1 ) * K ), 4 * K ):
            self.d.rectangle( [ X0 + t, Y0, X0 + t + 2 * K - 1, Y0 + K - 1 ], fill=cor )
            self.d.rectangle( [ X0 + t, Y1, X0 + t + 2 * K - 1, Y1 + K - 1 ], fill=cor )
        for t in range( int( ( r + 1 ) * K ), int( ( h - r - 1 ) * K ), 4 * K ):
            self.d.rectangle( [ X0, Y0 + t, X0 + K - 1, Y0 + t + 2 * K - 1 ], fill=cor )
            self.d.rectangle( [ X1, Y0 + t, X1 + K - 1, Y0 + t + 2 * K - 1 ], fill=cor )
        for cx, cy, a in ( ( X0, Y0, 180 ), ( X1 - 2 * r * K + K, Y0, 270 ), ( X0, Y1 - 2 * r * K + K, 90 ), ( X1 - 2 * r * K + K, Y1 - 2 * r * K + K, 0 ) ):
            self.d.arc( [ cx, cy, cx + 2 * r * K, cy + 2 * r * K ], a, a + 90, fill=cor, width=K )

    def texto( self, x, y, s, tam, cor, fonte=FONTE_B, alinha='l', espaco=0.0 ):
        f = ImageFont.truetype( fonte, int( tam * K ) )
        if espaco:
            # letra a letra, com espaçamento (rótulos em maiúsculas)
            larg = sum( f.getlength( c ) for c in s ) + espaco * K * ( len( s ) - 1 )
            px = x * K - ( larg if alinha == 'r' else larg / 2 if alinha == 'c' else 0 )
            for c in s:
                self.d.text( ( px, y * K ), c, font=f, fill=cor )
                px += f.getlength( c ) + espaco * K
            return
        a = { 'l': 'la', 'r': 'ra', 'c': 'ma' }[alinha]
        self.d.text( ( x * K, y * K ), s, font=f, fill=cor, anchor=a )

    def degrade_v( self, x, y, w, h, r, c0, c1 ):
        # caixa arredondada com degradê vertical (máscara + faixas)
        g = Image.new( 'RGB', ( w * K, h * K ) )
        gd = ImageDraw.Draw( g )
        for i in range( h * K ):
            t = i / max( 1, h * K - 1 )
            gd.line( [ ( 0, i ), ( w * K, i ) ], fill=tuple( int( c0[j] + ( c1[j] - c0[j] ) * t ) for j in range( 3 ) ) )
        m = Image.new( 'L', ( w * K, h * K ), 0 )
        ImageDraw.Draw( m ).rounded_rectangle( [ 0, 0, w * K - 1, h * K - 1 ], radius=r * K, fill=255 )
        self.im.paste( g, ( x * K, y * K ), m )

    def elipse( self, x, y, w, h, cor, alfa ):
        cam = Image.new( 'RGBA', self.im.size, ( 0, 0, 0, 0 ) )
        ImageDraw.Draw( cam ).ellipse( [ x * K, y * K, ( x + w ) * K, ( y + h ) * K ], fill=cor + ( int( 255 * alfa ), ) )
        self.im = Image.alpha_composite( self.im.convert( 'RGBA' ), cam ).convert( 'RGB' )
        self.d = ImageDraw.Draw( self.im )

    def final( self ):
        return self.im.resize( ( self.w, self.h ), Image.LANCZOS )


# ---------------------------------------------------------------- Alt+Q
ESQ = [ 'TOPO', 'BAIXO', 'MÃO DIREITA', 'CAPA', 'ACESSÓRIO' ]
DIR = [ 'MEIO', 'ARMADURA', 'MÃO ESQUERDA', 'CALÇADOS', 'ACESSÓRIO' ]


def equip( altura, rodape ):
    """Fundo das abas do Alt+Q (280 x altura); rodape = altura da faixa de baixo (0 = sem)."""
    t = Tela( 280, altura )
    corpo = altura - rodape
    # pedestal do personagem (o cliente desenha o personagem no centro, ~x 113..169)
    t.degrade_v( 96, 2, 88, min( 126, corpo - 4 ), 30, ( 247, 248, 253 ), ( 225, 229, 247 ) )
    t.elipse( 116, min( 126, corpo - 4 ) - 12, 48, 8, ( 110, 118, 180 ), 0.20 )
    for r in range( 5 ):
        y = 1 + 26 * r
        if y + 24 > corpo: break
        for x, rot, al, lx in ( ( 6, ESQ[r], 'l', 34 ), ( 250, DIR[r], 'r', 246 ) ):
            t.caixa( x, y, 24, 24, 7, EMPTY_TILE )
            t.tracejado( x, y, 24, 24, 7, DASH )
            t.texto( lx, y + 0.5, rot, 6.5, LABEL, alinha=al, espaco=0.4 )
    if rodape:
        t.rect( 0, corpo, 280, rodape, FOOT )
        t.rect( 0, corpo, 280, 1, CARD_LINE )
    return t.final()


def equip_rodape():
    t = Tela( 280, 20, FOOT )
    t.rect( 0, 0, 280, 1, CARD_LINE )
    return t.final()


# ---------------------------------------------------------------- Alt+A
NOMES = [ 'FOR', 'AGI', 'VIT', 'INT', 'DES', 'SOR' ]
MEIO = [ 'ATQ', 'ATQM', 'PRECISÃO', 'CRÍTICO', 'PONTOS', 'CLÃ' ]
DIREITA = [ 'DEF', 'DEFM', 'ESQUIVA', 'VEL. ATQ.' ]
TALENTOS = [ 'POW', 'STA', 'WIS', 'SPL', 'CON', 'CRT' ]
TAL_MEIO = [ 'P.ATQ', 'S.ATQM', 'CURA +', 'TX. CRÍT.', 'PONTOS' ]
TAL_DIR = [ 'RES', 'RESM' ]


def bloco_atributos( t, y0, nomes, meio, direita, cores ):
    for k in range( 6 ):
        y = y0 + 4 + 16 * k
        # cartão do atributo: nome colorido, valor (o cliente escreve em 34..74) e custo (74..86) com a seta
        t.caixa( 2, y, 99, 15, 5, CARD, CARD_LINE )
        t.caixa( 33, y + 2, 42, 11, 3, ( 247, 248, 252 ) )
        t.caixa( 76, y + 2, 10, 11, 3, ( 247, 248, 252 ) )
        t.texto( 6, y + 2.2, nomes[k], 8.5, cores[k] if cores else ACCENT )
    for col, lista, x0, x1 in ( ( 1, meio, 103, 189 ), ( 2, direita, 191, 278 ) ):
        for k, rot in enumerate( lista ):
            y = y0 + 2 + 16 * k
            larg = x1 - x0 if not ( col == 1 and rot == 'PONTOS' ) else 278 - x0
            t.caixa( x0, y, larg, 15, 5, FOOT )
            t.texto( x0 + 4, y + 3.6, rot, 6.5, LABEL, espaco=0.3 )


def atributos( ex ):
    t = Tela( 280, 224 if ex else 124 )
    bloco_atributos( t, 0, NOMES, MEIO, DIREITA, STAT_COR )
    t.rect( 0, 101, 280, 23, FOOT )
    t.rect( 0, 101, 280, 1, CARD_LINE )
    if ex:
        bloco_atributos( t, 121, TALENTOS, TAL_MEIO, TAL_DIR, None )
        t.rect( 0, 221, 280, 3, FOOT )
    return t.final()


def expandir( aberto ):
    t = Tela( 276, 18, FOOT )
    # seta (chevron) em lavanda no canto esquerdo
    cx, cy = 9, 9
    d = t.d
    s = 3.2
    if aberto:
        pts = [ ( cx - s, cy + 1.5 ), ( cx, cy - 2 ), ( cx + s, cy + 1.5 ) ]
    else:
        pts = [ ( cx - s, cy - 1.5 ), ( cx, cy + 2 ), ( cx + s, cy - 1.5 ) ]
    d.line( [ ( p[0] * K, p[1] * K ) for p in pts ], fill=ACCENT, width=int( 1.6 * K ), joint='curve' )
    t.texto( 18, 4.2, 'MENOS' if aberto else 'MAIS', 6.5, MUTED, espaco=0.4 )
    return t.final()


# ---------------------------------------------------------------- Alt+S
def barra( w, h, lado, cor_topo=None ):
    t = Tela( w, h, FOOT )
    t.rect( 0, 0 if lado != 'cima' else h - 1, w, 1, CARD_LINE )
    return t.final()


def subir( estado ):
    # botão de subir nível (24x24): círculo lavanda com "+", como no modelo
    t = Tela( 24, 24, ( 255, 0, 255 ) )
    fundo = { 'a': ACCENT_SOFT, 'b': ACCENT, 'c': ( 220, 222, 236 ) }[estado]
    traco = { 'a': ACCENT, 'b': ( 255, 255, 255 ), 'c': ( 180, 184, 204 ) }[estado]
    t.d.ellipse( [ 3 * K, 3 * K, 21 * K, 21 * K ], fill=fundo )
    t.rect( 7.5, 11.2, 9, 1.6, traco )
    t.rect( 11.2, 7.5, 1.6, 9, traco )
    im = t.final()
    # fora do círculo volta ao magenta exato (transparente para o cliente)
    px = im.load()
    for y in range( 24 ):
        for x in range( 24 ):
            if ( x - 11.5 ) ** 2 + ( y - 11.5 ) ** 2 > 9.4 ** 2: px[x, y] = ( 255, 0, 255 )
    return im


def imagens( vfs ):
    bi = BASE + 'basic_interface\\'
    out = {
        bi + 'equipwin_bg.bmp': equip( 130, 0 ),
        bi + 'equipwin_bg2.bmp': equip( 150, 20 ),
        bi + 'equipwin_bg3.bmp': equip_rodape(),
        bi + 'equipwin_special.bmp': equip( 133, 0 ),
        bi + 'equipwin_costume.bmp': equip( 130, 0 ),
        bi + 'equipwin_costume2.bmp': equip( 130, 0 ),
        bi + 'equipwin_cash.bmp': equip( 133, 0 ),
        BASE + 'statuswnd\\w_statwin_bg.bmp': atributos( False ),
        BASE + 'statuswnd\\w_ex_statwin_bg.bmp': atributos( True ),
        BASE + 'statuswnd\\expand_off_normal.bmp': expandir( False ),
        BASE + 'statuswnd\\expand_off_press.bmp': expandir( False ),
        BASE + 'statuswnd\\expand_on_normal.bmp': expandir( True ),
        BASE + 'statuswnd\\expand_on_press.bmp': expandir( True ),
        bi + 'skill_upbar_left.bmp': barra( 12, 20, 'cima' ),
        bi + 'skill_upbar_mid.bmp': barra( 5, 20, 'cima' ),
        bi + 'skill_upbar_right.bmp': barra( 12, 20, 'cima' ),
        bi + 'skill_botbar_left.bmp': barra( 12, 32, 'baixo' ),
        bi + 'skill_botbar_mid.bmp': barra( 5, 32, 'baixo' ),
        bi + 'skill_botbar_right.bmp': barra( 12, 32, 'baixo' ),
        bi + 'skill_up_a.bmp': subir( 'a' ),
        bi + 'skill_up_b.bmp': subir( 'b' ),
        bi + 'skill_up_c.bmp': subir( 'c' ),
    }
    return { k.encode( 'cp949' ): v for k, v in out.items() }


if __name__ == '__main__':
    import os, sys
    destino = sys.argv[1] if len( sys.argv ) > 1 else '.'
    os.makedirs( destino, exist_ok=True )
    for k, im in imagens( None ).items():
        im.save( os.path.join( destino, k.decode( 'cp949' ).split( '\\' )[-1].replace( '.bmp', '.png' ) ) )
