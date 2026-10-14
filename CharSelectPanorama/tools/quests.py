"""Textos das quests para a janela de quests do Zero.

Uso: python3 quests.py <pasta do cliente> [saída]
  Lê System\\OngoingQuests.lub (QuestInfoList) e System\\RecommendedQuests.lub (RecommendedQuestInfoList),
  ambos em texto (lua), e grava em <saída> (padrão dist/charselect):
    quests.txt     id<TAB>título<TAB>ícone<TAB>resumo<TAB>descrição
    novidades.txt  id<TAB>título<TAB>ícone<TAB>resumo<TAB>texto
  Quebras de linha viram \\n; textos em UTF-8.
"""
import os, re, sys


def tokens( s ):
    i, n = 0, len( s )
    while i < n:
        c = s[i]
        if c in ' \t\r\n,;':
            i += 1
        elif s.startswith( '--', i ):
            if s.startswith( '--[[', i ):
                e = s.find( ']]', i )
                i = n if e < 0 else e + 2
            else:
                e = s.find( '\n', i )
                i = n if e < 0 else e + 1
        elif c in '{}=[]':
            yield c; i += 1
        elif c in '"\'':
            j, out = i + 1, []
            while j < n and s[j] != c:
                if s[j] == '\\' and j + 1 < n:
                    nx = s[j + 1]
                    out.append( { 'n': '\n', 't': '\t', '\\': '\\', '"': '"', "'": "'" }.get( nx, nx ) )
                    j += 2
                else:
                    out.append( s[j] ); j += 1
            yield ( 'str', ''.join( out ) ); i = j + 1
        else:
            m = re.match( r'[A-Za-z_][A-Za-z0-9_\.]*|-?\d+(\.\d+)?', s[i:] )
            if not m:
                i += 1; continue
            t = m.group( 0 )
            yield ( 'num', float( t ) if '.' in t else int( t ) ) if re.match( r'-?\d', t ) else ( 'id', t )
            i += len( t )


def valor( it, tok ):
    if tok == '{':
        tab, idx = {}, 1
        while True:
            t = next( it )
            if t == '}':
                return tab
            if t == '[':
                k = next( it )[1]; next( it ); next( it )  # ] =
                tab[k] = valor( it, next( it ) )
            elif isinstance( t, tuple ) and t[0] == 'id':
                t2 = next( it )
                if t2 == '=':
                    tab[t[1]] = valor( it, next( it ) )
                else:
                    tab[idx] = t[1]; idx += 1
                    if t2 == '}': return tab
                    # o token lido já é o próximo elemento
                    tab[idx] = valor( it, t2 ); idx += 1
            else:
                tab[idx] = valor( it, t ); idx += 1
    if isinstance( tok, tuple ):
        return tok[1]
    return None


def tabela( texto, nome ):
    it = tokens( texto )
    achou = None
    for t in it:
        if t == ( 'id', nome ) and next( it ) == '=':
            v = valor( it, next( it ) )
            if v: achou = v  # a segunda atribuição ("= {}" e depois "= { ... }") vale
    return achou or {}


def linhas( v ):
    if isinstance( v, dict ):
        return [ str( v[k] ) for k in sorted( v ) if isinstance( v[k], str ) ]
    return [ v ] if isinstance( v, str ) else []


def limpa( s ):
    return s.replace( '\t', ' ' ).replace( '\r', '' ).replace( '\n', '\\n' )


def ler( caminho ):
    data = open( caminho, 'rb' ).read()
    try:
        return data.decode( 'utf-8' )
    except UnicodeDecodeError:
        return data.decode( 'cp1252' )


def main():
    cliente = sys.argv[1]
    saida = sys.argv[2] if len( sys.argv ) > 2 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect' )
    q = tabela( ler( os.path.join( cliente, 'System', 'OngoingQuests.lub' ) ), 'QuestInfoList' )
    with open( os.path.join( saida, 'quests.txt' ), 'w', encoding='utf-8', newline='\n' ) as f:
        for k in sorted( x for x in q if isinstance( x, int ) ):
            e = q[k]
            if not isinstance( e, dict ): continue
            f.write( '%d\t%s\t%s\t%s\t%s\n' % ( k, limpa( e.get( 'Title', '' ) ), e.get( 'IconName', '' ), limpa( e.get( 'Summary', '' ) ),
                limpa( '\n'.join( linhas( e.get( 'Description', '' ) ) ) ) ) )
    r = {}
    rec = os.path.join( cliente, 'System', 'RecommendedQuests.lub' )
    if os.path.exists( rec ):
        r = tabela( ler( rec ), 'RecommendedQuestInfoList' )
    with open( os.path.join( saida, 'novidades.txt' ), 'w', encoding='utf-8', newline='\n' ) as f:
        for k in sorted( x for x in r if isinstance( x, int ) ):
            e = r[k]
            if not isinstance( e, dict ): continue
            partes = [ '\n'.join( linhas( e.get( 'QuestInfo%d' % i ) ) ) for i in ( 1, 2, 3 ) ]
            f.write( '%d\t%s\t%s\t%s\t%s\n' % ( k, limpa( e.get( 'Title', '' ) ), e.get( 'IconName', '' ), limpa( e.get( 'Summary', '' ) ),
                limpa( '\n\n'.join( p for p in partes if p ) ) ) )
    print( 'quests: %d, novidades: %d' % ( len( q ), len( r ) ) )


if __name__ == '__main__':
    main()
