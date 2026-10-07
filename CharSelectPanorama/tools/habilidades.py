"""Árvore de habilidades para a janela nova, tirada dos .lub do cliente (GRFs, ordem do DATA.INI).

Saída: dist/charselect/habilidades.txt (UTF-8)
  J <classe>=<classe anterior>            herança (JOB_INHERIT_LIST)
  T <classe>=<pos>:<skill>,...            posições na árvore (SKILL_TREEVIEW_FOR_JOB; pos = linha*7+coluna)
  S <skill>=<AEGIS>|<nome>|<nível máx>|<pré-req skill:nível;...>

Uso: python3 tools/habilidades.py <pasta do cliente>
"""
import os
import re
import sys

sys.path.insert( 0, os.path.dirname( os.path.abspath( __file__ ) ) )
from grf import Vfs  # noqa: E402

BASE = 'data\\luafiles514\\lua files\\skillinfoz\\'


def table( text, name ):
    """Texto entre as chaves de "name = { ... }" (nível superior)."""
    i = text.find( name + ' = {' )
    if i < 0:
        return ''
    i = text.index( '{', i )
    depth = 0
    for j in range( i, len( text ) ):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
    return text[i + 1:]


def entries( body ):
    """[(chave, valor)] dos itens "[chave] = valor" do nível superior de uma tabela."""
    out = []
    i, n, depth = 0, len( body ), 0
    while i < n:
        c = body[i]
        if c == '"':
            i = body.index( '"', i + 1 ) + 1
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        elif c == '[' and depth == 0:
            k = body.index( ']', i )
            key = body[i + 1:k]
            e = body.index( '=', k ) + 1
            # valor até a vírgula do mesmo nível
            d = 0
            j = e
            while j < n:
                ch = body[j]
                if ch == '"':
                    j = body.index( '"', j + 1 )
                elif ch == '{':
                    d += 1
                elif ch == '}':
                    d -= 1
                elif ch == ',' and d == 0:
                    break
                j += 1
            out.append( ( key.strip(), body[e:j].strip() ) )
            i = j
            continue
        i += 1
    return out


def main():
    client = sys.argv[1]
    v = Vfs( client )
    read = lambda f: v.read( BASE + f ).decode( 'cp1252' )
    skid = { k: int( n ) for k, n in re.findall( r'(\w+)\s*=\s*(\d+)', table( read( 'skillid.lub' ), 'SKID' ) ) }
    jobtxt = read( 'jobinheritlist.lub' )
    jobid = { k: int( n ) for k, n in re.findall( r'(\w+)\s*=\s*(\d+)', table( jobtxt, 'JOBID' ) ) }
    jid = lambda s: jobid.get( s.replace( 'JOBID.', '' ).strip() )
    sid = lambda s: skid.get( s.replace( 'SKID.', '' ).strip() )

    lines = []
    for k, val in entries( table( jobtxt, 'JOB_INHERIT_LIST' ) ):
        a, b = jid( k ), jid( val )
        if a is not None and b is not None:
            lines.append( 'J %d=%d' % ( a, b ) )

    tree = table( read( 'skilltreeview.lub' ), 'SKILL_TREEVIEW_FOR_JOB' )
    usados = set()
    for k, val in entries( tree ):
        j = jid( k )
        if j is None:
            continue
        parts = []
        for pos, s in entries( val.strip()[1:-1] ):
            si = sid( s )
            if si is not None and pos.isdigit():
                parts.append( '%s:%d' % ( pos, si ) )
                usados.add( si )
        if parts:
            lines.append( 'T %d=%s' % ( j, ','.join( parts ) ) )

    info = table( read( 'skillinfolist.lub' ), 'SKILL_INFO_LIST' )
    for k, val in entries( info ):
        si = sid( k )
        if si is None:
            continue
        aegis = re.match( r'\{\s*"(\w+)"', val )
        nome = re.search( r'SkillName\s*=\s*"([^"]*)"', val )
        maxlv = re.search( r'MaxLv\s*=\s*(\d+)', val )
        need = []
        m = re.search( r'_NeedSkillList\s*=\s*\{(.*?)\n    \}', val, re.S )
        if m:
            for s, lv in re.findall( r'SKID\.(\w+)\s*,\s*(\d+)', m.group( 1 ) ):
                if s in skid:
                    need.append( '%d:%s' % ( skid[s], lv ) )
        lines.append( 'S %d=%s|%s|%s|%s' % ( si, aegis.group( 1 ) if aegis else k.replace( 'SKID.', '' ),
                                            nome.group( 1 ) if nome else '', maxlv.group( 1 ) if maxlv else '1', ';'.join( need ) ) )

    dest = os.path.join( os.path.dirname( os.path.abspath( __file__ ) ), '..', 'dist', 'charselect', 'habilidades.txt' )
    open( dest, 'w', encoding='utf-8' ).write( '\n'.join( lines ) + '\n' )
    print( 'habilidades: %d linhas (%d classes com árvore)' % ( len( lines ), sum( 1 for l in lines if l.startswith( 'T ' ) ) ) )


if __name__ == '__main__':
    main()
