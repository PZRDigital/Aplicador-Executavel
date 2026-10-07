"""Skin escura (estilo Ragnarok Zero, cores O Codex) para as janelas do jogo.

Lê as imagens originais da interface nas GRFs do cliente (ordem do DATA.INI), recolore e grava em
dist/charselect/skin/<fnv>.<ext>, onde <fnv> = FNV-1a 32 bits do caminho pedido pelo cliente em
minúsculas (ex.: data\\texture\\유저인터페이스\\basic_interface\\itemwin_mid.bmp). A interface
embutida entrega esses arquivos quando o cliente abre o original (CreateFileA).

Recoloração:
  - fundos claros/azulados (o "cromado" padrão do kRO) -> azul-marinho escuro;
  - linhas e textos escuros neutros -> dourado claro;
  - pixels saturados (ícones, detalhes coloridos) -> mantidos;
  - magenta (255,0,255) = transparente no RO -> mantido.

Uso: python3 tools/skin.py <pasta do cliente> [saída=dist/charselect/skin]
"""
import io
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert( 0, os.path.dirname( os.path.abspath( __file__ ) ) )
from grf import Vfs  # noqa: E402

UI = '유저인터페이스'.encode( 'cp949' )
PREFIX = b'data\\texture\\' + UI + b'\\'

# pastas inteiras da skin
FOLDERS = [b'basic_interface\\', b'inventory\\', b'equipmentpropertieswnd\\', b'swap_equipment\\', b'equip_preview\\',
           b'statuswnd\\', b'shortcut\\', b'menu_icon\\', b'minimap\\', b'cashemotion\\', b'renewalparty\\', b'tipbox\\',
           b'information\\', b'refining_renewal\\', b'enchantui\\', b'grade_enchant\\', b'renew_questui\\']
# arquivos soltos na raiz da interface que não são cromado de janela
ROOT_SKIP = ( b'loading', b'loaging', b'login', b'rag_title', b'ad_title', b'crack_', b'midgard', b'pasta', b'prontera',
              b'nakhyang', b'pay_arche', b'anibackg', b'gravityplanet', b'client_select', b'montype', b'emblem_frame' )
EXTS = ( b'.bmp', b'.tga' )


def fnv1a( data ):
    h = 0x811C9DC5
    for c in data:
        h ^= c
        h = ( h * 0x01000193 ) & 0xFFFFFFFF
    return h


def wanted( key ):
    if not key.startswith( PREFIX ) or not key.endswith( EXTS ):
        return False
    rest = key[len( PREFIX ):]
    if b'\\' not in rest:
        return not rest.startswith( ROOT_SKIP )
    return any( rest.startswith( f ) for f in FOLDERS )


def smoothstep( a, b, x ):
    t = np.clip( ( x - a ) / ( b - a ), 0.0, 1.0 )
    return t * t * ( 3 - 2 * t )


def hsl_to_rgb( h, s, l ):
    c = ( 1 - np.abs( 2 * l - 1 ) ) * s
    hp = ( h / 60.0 ) % 6
    x = c * ( 1 - np.abs( hp % 2 - 1 ) )
    z = np.zeros_like( h )
    r = np.select( [hp < 1, hp < 2, hp < 3, hp < 4, hp < 5], [c, x, z, z, x], c )
    g = np.select( [hp < 1, hp < 2, hp < 3, hp < 4, hp < 5], [x, c, c, x, z], z )
    b = np.select( [hp < 1, hp < 2, hp < 3, hp < 4, hp < 5], [z, z, x, c, c], x )
    m = l - c / 2
    return np.stack( [r + m, g + m, b + m], axis=-1 )


def recolor( rgb ):
    """rgb float 0..1 (H, W, 3) -> recolorido."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx = rgb.max( axis=-1 )
    mn = rgb.min( axis=-1 )
    l = ( mx + mn ) / 2
    d = mx - mn
    s = np.where( d < 1e-6, 0, d / np.maximum( 1e-6, 1 - np.abs( 2 * l - 1 ) ) )
    h = np.where( d < 1e-6, 0,
        np.where( mx == r, ( ( g - b ) / np.maximum( d, 1e-6 ) ) % 6,
        np.where( mx == g, ( b - r ) / np.maximum( d, 1e-6 ) + 2, ( r - g ) / np.maximum( d, 1e-6 ) + 4 ) ) ) * 60
    bluish = ( h >= 170 ) & ( h <= 265 )
    # quanto o pixel é "cromado" de janela (neutro, ou azul claro do kRO)
    chrome = np.clip( 1 - ( s - 0.22 ) / 0.25, 0, 1 )
    chrome = np.maximum( chrome, np.where( bluish & ( l > 0.45 ), np.clip( 1 - ( s - 0.6 ) / 0.3, 0, 1 ), 0 ) )

    t = smoothstep( 0.30, 0.62, l )                       # 0 = escuro (linha/texto), 1 = claro (fundo)
    navy_l = 0.055 + ( 1 - l ) * 0.34                      # branco -> quase preto azulado
    navy = hsl_to_rgb( np.full_like( l, 216.0 ), np.full_like( l, 0.42 ), navy_l )
    gold_l = 0.58 + ( 0.45 - np.minimum( l, 0.45 ) ) * 0.42  # preto -> dourado claro
    gold = hsl_to_rgb( np.full_like( l, 42.0 ), np.full_like( l, 0.72 ), gold_l )
    themed = gold * ( 1 - t[..., None] ) + navy * t[..., None]
    return rgb * ( 1 - chrome[..., None] ) + themed * chrome[..., None]


def convert( data, ext ):
    im = Image.open( io.BytesIO( data ) )
    if ext == b'.tga':
        im = im.convert( 'RGBA' )
        a = np.asarray( im ).astype( np.float32 ) / 255.0
        out = a.copy()
        out[..., :3] = recolor( a[..., :3] )
        img = Image.fromarray( ( np.clip( out, 0, 1 ) * 255 + 0.5 ).astype( np.uint8 ), 'RGBA' )
        buf = io.BytesIO()
        img.save( buf, 'TGA' )
        return buf.getvalue()
    im = im.convert( 'RGB' )
    a = np.asarray( im )
    magenta = ( a[..., 0] >= 254 ) & ( a[..., 1] <= 3 ) & ( a[..., 2] >= 254 )
    out = recolor( a.astype( np.float32 ) / 255.0 )
    out = ( np.clip( out, 0, 1 ) * 255 + 0.5 ).astype( np.uint8 )
    out[magenta] = ( 255, 0, 255 )
    buf = io.BytesIO()
    Image.fromarray( out, 'RGB' ).save( buf, 'BMP' )
    return buf.getvalue()


def main():
    client = sys.argv[1]
    dest = sys.argv[2] if len( sys.argv ) > 2 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect', 'skin' )
    os.makedirs( dest, exist_ok=True )
    for f in os.listdir( dest ):
        os.remove( os.path.join( dest, f ) )
    vfs = Vfs( client )
    keys = set()
    for g in vfs.grfs:
        keys |= { k for k in g.entries if wanted( k ) }
    ok = fail = 0
    lista = []
    for k in sorted( keys ):
        try:
            data = vfs.read( k )
            ext = k[k.rfind( b'.' ):]
            out = convert( data, ext )
        except Exception as ex:  # imagem estranha: o cliente usa a original
            fail += 1
            print( 'pulado:', k.decode( 'cp949', 'replace' ), ex )
            continue
        name = '%08x%s' % ( fnv1a( k ), ext.decode() )
        open( os.path.join( dest, name ), 'wb' ).write( out )
        lista.append( '%s=%s' % ( name, k.decode( 'cp949', 'replace' ) ) )
        ok += 1
    open( os.path.join( dest, 'lista.txt' ), 'w', encoding='utf-8' ).write( '\n'.join( lista ) + '\n' )
    print( 'skin: %d imagens, %d puladas -> %s' % ( ok, fail, dest ) )


if __name__ == '__main__':
    main()
