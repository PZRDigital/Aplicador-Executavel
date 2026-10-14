"""Imagens do cliente trocadas pela interface (entregues pela DLL quando o cliente abre o original).

Uso: python3 substituicoes.py <pasta do cliente> [saída]
  Grava <saída>\\skin\\<fnv>.<ext> (padrão dist/charselect/skin), onde <fnv> = FNV-1a 32 bits do caminho
  na GRF em minúsculas (data\\texture\\유저인터페이스\\...), e skin\\lista.txt. src/skin.cpp (InstallFiles)
  abre esses arquivos no lugar dos originais.
  - chat: botões e campos do chat do Ragnarok Zero (base/zero_chat = UIRenewal\\chat dos patches do kRO Zero)
    com os nomes do basic_interface que o cliente usa;
  - janela de atributos (Alt+A): nomes em português (statwin_ptbr.py).
"""
import io, os, sys
from PIL import Image
from grf import Vfs
import statwin_ptbr

BASE = 'data\\texture\\유저인터페이스\\basic_interface\\'
# nome no cliente -> nome no chat do Zero (mesma função)
CHAT = {
    'battle_option_a': 'add_a', 'battle_option_b': 'add_b',          # "+" (nova aba)
    'battle_option2_a': 'option_a', 'battle_option2_b': 'option_b',  # engrenagem
    'wnd_mini_a': 'del_a', 'wnd_mini_b': 'del_b',                    # "-" (minimizar)
    'stickon_a': 'stickon_a', 'stickon_b': 'stickon_b', 'stickoff_a': 'stickoff_a', 'stickoff_b': 'stickoff_b',
    'chatmode_on': 'chatmode_on', 'chatmode_off': 'chatmode_off',
    'lock_dragwnd': 'lock_dragwnd', 'unlock_dragwnd': 'unlock_dragwnd',
    'dialog_btn0': 'dialog_btn0', 'dialog_btn1': 'dialog_btn1', 'dialog_btn2': 'dialog_btn2',
    'dialog_left': 'dialog_left', 'dialog_right': 'dialog_right', 'dialog_mid': 'resizer',
}


def fnv1a( data ):
    h = 0x811C9DC5
    for c in data:
        h ^= c
        h = ( h * 0x01000193 ) & 0xFFFFFFFF
    return h


def main():
    cliente = sys.argv[1]
    saida = sys.argv[2] if len( sys.argv ) > 2 else os.path.join( os.path.dirname( __file__ ), '..', 'dist', 'charselect' )
    dest = os.path.join( saida, 'skin' )
    os.makedirs( dest, exist_ok=True )
    zero = os.path.join( os.path.dirname( __file__ ), '..', 'base', 'zero_chat' )
    lista = []

    def grava( chave, dados ):
        k = chave.lower().replace( b'/', b'\\' )
        nome = '%08x%s' % ( fnv1a( k ), k[k.rfind( b'.' ):].decode() )
        open( os.path.join( dest, nome ), 'wb' ).write( dados )
        lista.append( '%s=%s' % ( nome, k.decode( 'cp949', 'replace' ) ) )

    for cliente_nome, zero_nome in CHAT.items():
        f = os.path.join( zero, zero_nome + '.bmp' )
        if os.path.exists( f ):
            grava( ( BASE + cliente_nome + '.bmp' ).encode( 'cp949' ), open( f, 'rb' ).read() )
    for k, im in statwin_ptbr.imagens( Vfs( cliente ) ).items():
        b = io.BytesIO()
        im.save( b, format='BMP' )
        grava( k, b.getvalue() )
    open( os.path.join( dest, 'lista.txt' ), 'w', encoding='utf-8' ).write( '\n'.join( lista ) + '\n' )
    print( 'substituições: %d imagens em %s' % ( len( lista ), os.path.normpath( dest ) ) )


if __name__ == '__main__':
    main()
