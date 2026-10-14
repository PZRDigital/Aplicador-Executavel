"""Ilustrações dos pets para a Pokédex (janela de pet, Alt+J).

Uso: python3 pets.py <pasta do cliente> [saída]
  datainfo\\petinfo.lub (PetIllustNameTable / _Eng, PetNameTable) + npcidentity.lub (jobtbl) do cliente;
  a ilustração (유저인터페이스\\illust\\<nome>.bmp) vira <saída>\\img\\pet\\<classe>.png (magenta = transparente)
  e <saída>\\pets.txt guarda classe=espécie.
  Animação: sprite do pet (sprite\\몬스터\\<jobname>.spr/.act), ação 0 (parado, de frente), vira a tira
  <saída>\\img\\pet\\anim\\<classe>.png (quadros lado a lado) e pets_anim.txt: classe=quadros,largura,altura,ms
"""
import io, math, os, re, struct, sys
from PIL import Image
from grf import Vfs


def ler_spr( dados ):
    """Quadros do SPR como imagens RGBA (paleta + quadros RGBA)."""
    ver = struct.unpack_from( '<H', dados, 2 )[0]
    npal = struct.unpack_from( '<H', dados, 4 )[0]
    nrgba = struct.unpack_from( '<H', dados, 6 )[0] if ver >= 0x200 else 0
    p = 8 if ver >= 0x200 else 6
    pal = dados[-1024:]
    cores = [ ( pal[i * 4], pal[i * 4 + 1], pal[i * 4 + 2], 0 if i == 0 else 255 ) for i in range( 256 ) ]
    quadros = []
    for _ in range( npal ):
        w, h = struct.unpack_from( '<HH', dados, p ); p += 4
        if ver >= 0x201:
            tam = struct.unpack_from( '<H', dados, p )[0]; p += 2
            bruto, i, fim = bytearray(), p, p + tam
            while i < fim:
                c = dados[i]; i += 1
                if c == 0:
                    n = dados[i]; i += 1
                    bruto += b'\0' * ( n if n else 1 )
                else:
                    bruto.append( c )
            p = fim
        else:
            bruto = dados[p:p + w * h]; p += w * h
        im = Image.new( 'RGBA', ( w, h ) )
        im.putdata( [ cores[c] for c in bytes( bruto[:w * h] ).ljust( w * h, b'\0' ) ] )
        quadros.append( im )
    for _ in range( nrgba ):
        w, h = struct.unpack_from( '<HH', dados, p ); p += 4
        px = dados[p:p + w * h * 4]; p += w * h * 4
        im = Image.frombytes( 'RGBA', ( w, h ), bytes( px[i + 3 - k] for i in range( 0, len( px ), 4 ) for k in range( 4 ) ) )
        quadros.append( im.transpose( Image.FLIP_TOP_BOTTOM ) )
    return quadros, npal


def ler_act( dados ):
    """Ações do ACT: [ [quadro = [camada(x, y, spr, espelho, cor, ex, ey, ângulo, tipo)]] ], atrasos (ms)."""
    ver = struct.unpack_from( '<H', dados, 2 )[0]
    nacoes = struct.unpack_from( '<H', dados, 4 )[0]
    p = 16
    acoes = []
    for _ in range( nacoes ):
        nq = struct.unpack_from( '<I', dados, p )[0]; p += 4
        quadros = []
        for _ in range( nq ):
            p += 32
            nc = struct.unpack_from( '<I', dados, p )[0]; p += 4
            camadas = []
            for _ in range( nc ):
                x, y, spr, esp = struct.unpack_from( '<iiiI', dados, p ); p += 16
                cor, ex, ey, ang, tipo = ( 255, 255, 255, 255 ), 1.0, 1.0, 0, 0
                if ver >= 0x200:
                    cor = tuple( dados[p:p + 4] ); p += 4
                    if ver >= 0x204:
                        ex, ey = struct.unpack_from( '<ff', dados, p ); p += 8
                    else:
                        ex = ey = struct.unpack_from( '<f', dados, p )[0]; p += 4
                    ang, tipo = struct.unpack_from( '<ii', dados, p ); p += 8
                    if ver >= 0x205: p += 8
                camadas.append( ( x, y, spr, esp, cor, ex, ey, ang, tipo ) )
            if ver >= 0x200: p += 4
            if ver >= 0x203:
                na = struct.unpack_from( '<I', dados, p )[0]; p += 4 + na * 16
            quadros.append( camadas )
        acoes.append( quadros )
    atrasos = [ 4.0 ] * nacoes
    if ver >= 0x201:
        ne = struct.unpack_from( '<I', dados, p )[0]; p += 4 + ne * 40
    if ver >= 0x202:
        atrasos = list( struct.unpack_from( '<%df' % nacoes, dados, p ) )
    return acoes, [ int( a * 25 ) for a in atrasos ]


def animacao( spr, act ):
    """Ação 0 (parado, olhando para o sul) composta quadro a quadro, todos com o mesmo tamanho."""
    imgs, npal = ler_spr( spr )
    acoes, atrasos = ler_act( act )
    if not acoes or not acoes[0]: return None
    pecas = []
    for camadas in acoes[0]:
        q = []
        for x, y, n, esp, cor, ex, ey, ang, tipo in camadas:
            idx = n + ( npal if tipo == 1 else 0 )
            if n < 0 or idx >= len( imgs ): continue
            im = imgs[idx]
            if esp: im = im.transpose( Image.FLIP_LEFT_RIGHT )
            if ex != 1.0 or ey != 1.0:
                im = im.resize( ( max( 1, int( im.width * abs( ex ) ) ), max( 1, int( im.height * abs( ey ) ) ) ), Image.NEAREST )
            if ang: im = im.rotate( -ang, expand=True )
            if cor != ( 255, 255, 255, 255 ):
                r, g, b, a = im.split()
                im = Image.merge( 'RGBA', ( r.point( lambda v: v * cor[0] // 255 ), g.point( lambda v: v * cor[1] // 255 ),
                    b.point( lambda v: v * cor[2] // 255 ), a.point( lambda v: v * cor[3] // 255 ) ) )
            q.append( ( x - im.width // 2, y - im.height // 2, im ) )
        pecas.append( q )
    xs = [ x for q in pecas for x, y, im in q ] + [ x + im.width for q in pecas for x, y, im in q ]
    ys = [ y for q in pecas for x, y, im in q ] + [ y + im.height for q in pecas for x, y, im in q ]
    if not xs: return None
    x0, x1, y0, y1 = min( xs ), max( xs ), min( ys ), max( ys )
    w, h = x1 - x0, y1 - y0
    tira = Image.new( 'RGBA', ( w * len( pecas ), h ), ( 0, 0, 0, 0 ) )
    for i, q in enumerate( pecas ):
        for x, y, im in q:
            tira.alpha_composite( im, ( i * w + x - x0, y - y0 ) )
    return tira, len( pecas ), w, h, max( 50, atrasos[0] if atrasos else 100 )

DATAINFO = 'data\\luafiles514\\lua files\\datainfo\\'


def tabelas( texto, nome ):
    """Todas as atribuições `nome = { [jobtbl.JT_X] = "valor", ... }` (a última vale por chave)."""
    out = {}
    for m in re.finditer( r'^%s\s*=\s*\{(.*?)^\}' % re.escape( nome ), texto, re.S | re.M ):
        for k, v in re.findall( r'\[jobtbl\.(\w+)\]\s*=\s*"([^"]*)"', m.group( 1 ) ):
            out[k] = v
    return out


def main():
    cliente = sys.argv[1]
    saida = sys.argv[2] if len( sys.argv ) > 2 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect' )
    vfs = Vfs( cliente )
    ler = lambda n: vfs.read( DATAINFO + n ).decode( 'latin-1' )
    ids = { k: int( v ) for k, v in re.findall( r'(JT_\w+)\s*=\s*(\d+)', ler( 'npcidentity.lub' ) ) }
    info = ler( 'petinfo.lub' )
    ilus = tabelas( info, 'PetIllustNameTable' )
    ilus_eng = tabelas( info, 'PetIllustNameTable_Eng' )
    nomes = tabelas( info, 'PetNameTable' )
    sprites = { k: v for k, v in re.findall( r'\[jobtbl\.(\w+)\]\s*=\s*"([^"]*)"', ler( 'jobname.lub' ) ) }
    anim = os.path.join( saida, 'img', 'pet', 'anim' )
    os.makedirs( anim, exist_ok=True )
    animados = 0
    linhas_anim = [ '; classe=quadros,largura,altura,ms por quadro (ação parado, de frente)' ]
    pasta = os.path.join( saida, 'img', 'pet' )
    os.makedirs( pasta, exist_ok=True )
    feitos = 0
    with open( os.path.join( saida, 'pets.txt' ), 'w', encoding='utf-8', newline='\n' ) as f:
        f.write( '; classe=espécie (PetNameTable do cliente)\n' )
        for jt in sorted( set( ilus ) | set( ilus_eng ) | set( nomes ) ):
            if jt not in ids: continue
            classe = ids[jt]
            if jt in nomes:
                f.write( '%d=%s\n' % ( classe, nomes[jt].replace( '_', ' ' ).title() ) )
            if jt in sprites:
                base = 'data\\sprite\\' + '몬스터'.encode( 'cp949' ).decode( 'latin-1' ) + '\\' + sprites[jt]
                try:
                    r = animacao( vfs.read( ( base + '.spr' ).encode( 'latin-1' ) ), vfs.read( ( base + '.act' ).encode( 'latin-1' ) ) )
                except Exception as ex:
                    r = None
                    print( 'aviso: animação de %s: %s' % ( sprites[jt], ex ) )
                if r:
                    tira, n, w, h, ms = r
                    tira.save( os.path.join( anim, '%d.png' % classe ) )
                    linhas_anim.append( '%d=%d,%d,%d,%d' % ( classe, n, w, h, ms ) )
                    animados += 1
            for arq in ( ilus.get( jt ), ilus_eng.get( jt ) ):
                if not arq: continue
                chave = 'data\\texture\\' + '유저인터페이스'.encode( 'cp949' ).decode( 'latin-1' ) + '\\illust\\' + arq
                try:
                    dados = vfs.read( chave.encode( 'latin-1' ) )
                except KeyError:
                    continue
                im = Image.open( io.BytesIO( dados ) ).convert( 'RGBA' )
                px = im.load()
                for y in range( im.height ):
                    for x in range( im.width ):
                        r, g, b, a = px[x, y]
                        if r >= 0xF8 and g <= 0x08 and b >= 0xF8: px[x, y] = ( 0, 0, 0, 0 )
                im.save( os.path.join( pasta, '%d.png' % classe ) )
                feitos += 1
                break
    with open( os.path.join( saida, 'pets_anim.txt' ), 'w', encoding='utf-8', newline='\n' ) as f:
        f.write( '\n'.join( linhas_anim ) + '\n' )
    print( 'pets: %d ilustrações, %d animações em %s' % ( feitos, animados, os.path.normpath( pasta ) ) )


if __name__ == '__main__':
    main()
