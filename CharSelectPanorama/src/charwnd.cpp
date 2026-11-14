#include "charwnd.hpp"

#include <windows.h>
#include <cstring>

#include "client.hpp"
#include "game.hpp"
#include "config.hpp"
#include "log.hpp"
#include "ui.hpp"
#include "game.hpp"

namespace {
	constexpr uintptr_t AT_BG_CALL = 0x8AF8F9;   // call DrawBitmap (fundo) no início do OnDraw
	constexpr uintptr_t AT_AFTER_BG = 0x8AF8FE;  // push 0xFAE7B8 (título) -> jmp para o personagem
	constexpr uintptr_t AT_CHARACTER = 0x8AFBC3; // mov ecx, esi; call 0x8CBD20 (dados do personagem)
	constexpr uintptr_t AT_TABS_2_3 = 0x8AFA26;
	constexpr uintptr_t AT_DAMAGE_DIR_W = 0x8A26B4; // push 0x3C: largura das opções de direção do dano (OnCreate)  // conteúdo das abas Título (2) e Dano (3), feito pelo cliente
	constexpr uintptr_t FN_BITMAP = 0xA061F0;    // UIWindow::DrawBitmap( x, y, CBitmapRes*, cor-chave )
	constexpr uintptr_t FN_FILL = 0xA063F0;      // UIWindow::FillRect( x, y, w, h, cor )
	constexpr uintptr_t FN_TEXT = 0xA0E280;      // UIWindow::TextOut( x, y, texto, len, fonte, altura, cor, negrito, itálico )
	// medida do texto (o mesmo objeto que o TextOut monta na pilha): ctor( superfície ), fonte, GetTextExtent, dtor
	constexpr uintptr_t FN_TXT_CTOR = 0x5369C0, FN_TXT_FONT = 0x539560, FN_TXT_EXTENT = 0x539AA0, FN_TXT_DTOR = 0x536CF0;
	constexpr int CP = 1252; // textos do cliente (pt-BR)

	bool g_active = false;

	std::string Ansi( const std::wstring& w ){
		if( w.empty() ) return std::string();
		int n = WideCharToMultiByte( CP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr );
		std::string s( n, '\0' );
		WideCharToMultiByte( CP, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr );
		return s;
	}

	int Width( uintptr_t w, const std::string& s, int height, bool bold ){
		uintptr_t surf = *reinterpret_cast<uintptr_t*>( w + 0x24 );
		if( !surf || s.empty() ) return 0;
		alignas( 8 ) uint8_t helper[0x40] = {};
		SIZE sz = {};
		__try{
			reinterpret_cast<void*( __thiscall* )( void*, uintptr_t )>( FN_TXT_CTOR )( helper, surf );
			reinterpret_cast<void( __thiscall* )( void*, int, int, int, int )>( FN_TXT_FONT )( helper, 0, height, bold ? 1 : 0, 0 );
			int cp = *reinterpret_cast<int*>( helper + 0x14 );
			reinterpret_cast<void*( __thiscall* )( void*, SIZE*, const char*, int, int )>( FN_TXT_EXTENT )( helper, &sz, s.c_str(), (int)s.size(), cp );
			reinterpret_cast<void( __thiscall* )( void* )>( FN_TXT_DTOR )( helper );
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return 0;
		}
		return sz.cx;
	}

	// OnDraw: o fundo (primeira imagem desenhada) vira o layout inteiro; o resto do desenho antigo é pulado
	void __fastcall DrawLayout( uintptr_t self, void*, int, int, void*, int ){
		__try{
			ui::PaintCharacterWindow( self );
		}__except( EXCEPTION_EXECUTE_HANDLER ){
		}
	}

	// depois do nosso fundo: Equip/Visual -> o personagem; Título/Dano -> o conteúdo dessas abas (esi = janela)
	__declspec( naked ) void AfterLayoutStub(){
		__asm{
			cmp dword ptr [esi + 0xD0], 2
			jl character
			push AT_TABS_2_3
			ret
		character:
			push AT_CHARACTER
			ret
		}
	}

	bool PatchCall( uintptr_t at, const void* fn ){
		uint8_t* p = reinterpret_cast<uint8_t*>( at );
		DWORD old;
		VirtualProtect( p, 5, PAGE_EXECUTE_READWRITE, &old );
		*reinterpret_cast<int32_t*>( p + 1 ) = (int32_t)( reinterpret_cast<uintptr_t>( fn ) - ( at + 5 ) );
		VirtualProtect( p, 5, old, &old );
		return true;
	}

	bool PatchJmp( uintptr_t at, uintptr_t to ){
		uint8_t* p = reinterpret_cast<uint8_t*>( at );
		DWORD old;
		VirtualProtect( p, 5, PAGE_EXECUTE_READWRITE, &old );
		p[0] = 0xE9;
		*reinterpret_cast<int32_t*>( p + 1 ) = (int32_t)( to - ( at + 5 ) );
		VirtualProtect( p, 5, old, &old );
		return true;
	}
}

void charwnd::Fill( uintptr_t w, int x, int y, int cw, int ch, uint32_t color ){
	// recorta na janela (a rotina do cliente não aceita retângulo saindo pela esquerda/topo)
	const int W = *reinterpret_cast<int*>( w + 0x14 ), H = *reinterpret_cast<int*>( w + 0x18 );
	if( x < 0 ){ cw += x; x = 0; }
	if( y < 0 ){ ch += y; y = 0; }
	if( x + cw > W ) cw = W - x;
	if( y + ch > H ) ch = H - y;
	if( cw <= 0 || ch <= 0 ) return;
	reinterpret_cast<void( __thiscall* )( uintptr_t, int, int, int, int, uint32_t )>( FN_FILL )( w, x, y, cw, ch, color );
}

int charwnd::TextWidth( uintptr_t w, const std::wstring& text, int height, bool bold ){
	return Width( w, Ansi( text ), height, bold );
}

void charwnd::Text( uintptr_t w, int x, int y, const std::wstring& text, int height, uint32_t color, bool bold, int align, int maxWidth ){
	std::string s = Ansi( text );
	if( s.empty() ) return;
	int tw = ( align != Left || maxWidth > 0 ) ? Width( w, s, height, bold ) : 0;
	if( maxWidth > 0 && tw > maxWidth ){ // corta com reticências
		while( s.size() > 1 && Width( w, s + "...", height, bold ) > maxWidth ) s.pop_back();
		s += "...";
		tw = Width( w, s, height, bold );
	}
	if( align == Right ) x -= tw;
	else if( align == Center ) x -= tw / 2;
	if( x < 0 || y < 0 || x + tw > *reinterpret_cast<int*>( w + 0x14 ) || y >= *reinterpret_cast<int*>( w + 0x18 ) ) return; // idem
	reinterpret_cast<void( __thiscall* )( uintptr_t, int, int, const char*, int, int, int, uint32_t, int, int )>( FN_TEXT )(
		w, x, y, s.c_str(), (int)s.size(), 0, height, color, bold ? 1 : 0, 0 );
}

bool charwnd::Bitmap( uintptr_t w, int x, int y, uintptr_t res ){
	if( !res ) return false;
	// a rotina do cliente não recorta: fora da janela a imagem "dá a volta" e aparece do outro lado
	const int W = *reinterpret_cast<int*>( w + 0x14 ), H = *reinterpret_cast<int*>( w + 0x18 );
	const int bw = *reinterpret_cast<int*>( res + 0x114 ), bh = *reinterpret_cast<int*>( res + 0x118 );
	if( x < 0 || y < 0 || x + bw > W || y + bh > H ) return false;
	reinterpret_cast<void( __thiscall* )( uintptr_t, int, int, uintptr_t, int )>( FN_BITMAP )( w, x, y, res, 1 ); // 1 = magenta transparente
	return true;
}

bool charwnd::Bitmap( uintptr_t w, int x, int y, const std::string& path ){
	return Bitmap( w, x, y, game::ClientBitmapRes( path ) );
}

bool charwnd::Active(){
	return g_active;
}

bool charwnd::Install(){
	const uint8_t sigBg[] = { 0xE8, 0xF2, 0x68, 0x15, 0x00 };   // call 0xA061F0
	const uint8_t sigAfter[] = { 0x68, 0xB8, 0xE7, 0xFA, 0x00 }; // push 0xFAE7B8
	const uint8_t sigChar[] = { 0x8B, 0xCE, 0xE8, 0x56, 0xC1, 0x01, 0x00 }; // mov ecx, esi; call 0x8CBD20
	if( memcmp( reinterpret_cast<void*>( AT_BG_CALL ), sigBg, sizeof( sigBg ) ) != 0
		|| memcmp( reinterpret_cast<void*>( AT_AFTER_BG ), sigAfter, sizeof( sigAfter ) ) != 0
		|| memcmp( reinterpret_cast<void*>( AT_CHARACTER ), sigChar, sizeof( sigChar ) ) != 0 ){
		logger::Write( "AVISO: OnDraw da janela de equipamentos diferente do esperado; janela Personagem nativa desativada" );
		return false;
	}
	PatchCall( AT_BG_CALL, reinterpret_cast<const void*>( &DrawLayout ) );
	PatchJmp( AT_AFTER_BG, reinterpret_cast<uintptr_t>( &AfterLayoutStub ) );
	{ // opções de direção do dano (aba Dano): caixas de 60 -> 72 px, "Esquerda" cabe (push 0x3C em 0x8A26B4)
		uint8_t* p = reinterpret_cast<uint8_t*>( AT_DAMAGE_DIR_W );
		if( p[0] == 0x6A && p[1] == 0x3C ){
			DWORD o;
			VirtualProtect( p, 2, PAGE_EXECUTE_READWRITE, &o );
			p[1] = 72;
			VirtualProtect( p, 2, o, &o );
		}
	}
	FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
	g_active = true;
	logger::Write( "Janela Personagem: OnDraw nativo refeito" );
	return true;
}

// ======================================================================== habilidades
namespace {
	constexpr uintptr_t SK_ONDRAW = 0x973050, SK_HIT = 0x9711A0, SK_INLIST = 0x971DF0, SK_BUTTONS_END = 0x9742DC, SK_BUTTONS_RET = 0x9742E6;
	bool g_skills = false;

	void __fastcall SkillDraw( uintptr_t self, void* ){
		__try{
			ui::PaintSkillWindow( self );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
		*reinterpret_cast<uint8_t*>( self + 0x255 ) = 0; // como o original (já desenhado)
	}

	int RowAt( int x, int y ){
		using namespace ui;
		if( x < SK_M || x >= SK_M + SK_ROW_W || y < SK_TOP || y >= SK_TOP + SK_ROWS * SK_RH ) return -1;
		int row = ( y - SK_TOP ) / SK_RH;
		if( y - SK_TOP - row * SK_RH >= SK_ROW_H ) return -1; // entre as linhas
		return row;
	}

	// 0x9711A0: índice (na lista da aba) do que está em (x, y), -1 = nada
	int __fastcall SkillHit( uintptr_t self, void*, int x, int y ){
		int row = RowAt( x, y );
		if( row < 0 ) return -1;
		int scroll = *reinterpret_cast<int*>( self + 0x23C ), cols = *reinterpret_cast<int*>( self + 0x240 );
		return ( scroll + row ) * ( cols > 0 ? cols : 1 );
	}

	// 0x971DF0: o ponto está na área da lista
	bool __fastcall SkillInList( uintptr_t self, void*, int x, int y ){
		return RowAt( x, y ) >= 0;
	}

	void __stdcall SkillButtons( uintptr_t self ){
		__try{ game::FixSkillButtons( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	// fim do posicionamento dos botões do cliente (0x973FF0): os nossos lugares, depois o original
	__declspec( naked ) void SkillButtonsStub(){
		__asm{
			pushad
			push edi
			call SkillButtons
			popad
			mov ecx, dword ptr [ebp - 0x0C]
			mov dword ptr fs:[0], ecx
			push SK_BUTTONS_RET
			ret
		}
	}

	bool JumpFn( uintptr_t at, const void* fn, const uint8_t* sig, size_t n ){
		if( memcmp( reinterpret_cast<void*>( at ), sig, n ) != 0 ) return false;
		return PatchJmp( at, reinterpret_cast<uintptr_t>( fn ) );
	}
}

bool charwnd::SkillsActive(){
	return g_skills;
}

bool charwnd::InstallSkills(){
	const uint8_t sigDraw[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
	const uint8_t sigHit[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0x75, 0x0C };
	const uint8_t sigIn[] = { 0x55, 0x8B, 0xEC, 0x83, 0x3D, 0xB4, 0x31, 0x5D, 0x01, 0x00 };
	const uint8_t sigEnd[] = { 0x8B, 0x4D, 0xF4, 0x64, 0x89, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x59, 0x5F };
	if( memcmp( reinterpret_cast<void*>( SK_ONDRAW ), sigDraw, sizeof( sigDraw ) ) != 0
		|| memcmp( reinterpret_cast<void*>( SK_HIT ), sigHit, sizeof( sigHit ) ) != 0
		|| memcmp( reinterpret_cast<void*>( SK_INLIST ), sigIn, sizeof( sigIn ) ) != 0
		|| memcmp( reinterpret_cast<void*>( SK_BUTTONS_END ), sigEnd, sizeof( sigEnd ) ) != 0 ){
		logger::Write( "AVISO: janela de habilidades diferente do esperado; mantida a original" );
		return false;
	}
	JumpFn( SK_ONDRAW, reinterpret_cast<const void*>( &SkillDraw ), sigDraw, sizeof( sigDraw ) );
	JumpFn( SK_HIT, reinterpret_cast<const void*>( &SkillHit ), sigHit, sizeof( sigHit ) );
	JumpFn( SK_INLIST, reinterpret_cast<const void*>( &SkillInList ), sigIn, sizeof( sigIn ) );
	PatchJmp( SK_BUTTONS_END, reinterpret_cast<uintptr_t>( &SkillButtonsStub ) );
	FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
	g_skills = true;
	logger::Write( "Janela de habilidades: desenho e lista nativos refeitos" );
	return true;
}

// ======================================================================== inventário
// A UIItemWnd testa a grade nas medidas dela (32 px a partir de 40,17) em várias funções (0x934F10 e 0x9349E0
// devolvem o item, 0x9347D0 o índice): as três passam por um trampolim que troca o ponto da nossa grade
// (ui::INV_*) pelo centro da célula equivalente na dela. A dica do item (0x948530) é posta na nossa célula.
namespace {
	constexpr uintptr_t INV_ONDRAW = 0x941E80, INV_ITEM_AT = 0x934F10, INV_ITEM_AT2 = 0x9349E0, INV_INDEX_AT = 0x9347D0;
	constexpr uintptr_t INV_TIP_AT = 0x948588, INV_TIP_RET = 0x9485B3, INV_DBL_Y = 0x9450D3, INV_DRAG_Y = 0x9460C6;
	constexpr uintptr_t VT_ITEMWND = 0x101D3E0;
	bool g_inventory = false, g_storage = false;
	uint8_t* g_tramp = nullptr; // [0] item, [16] item 2, [32] índice, [48] item do armazém, [64] da pesquisa, [80] do filtro

	// início copiado + jmp de volta
	void* Trampoline( int slot, uintptr_t fn, int len ){
		uint8_t* t = g_tramp + slot * 16;
		memcpy( t, reinterpret_cast<void*>( fn ), len );
		t[len] = 0xE9;
		*reinterpret_cast<int32_t*>( t + len + 1 ) = (int32_t)( ( fn + len ) - reinterpret_cast<uintptr_t>( t + len + 5 ) );
		return t;
	}

	bool OurCell( int x, int y, int& c, int& r ){
		using namespace ui;
		if( x < INV_M || y < INV_GRID_Y ) return false;
		c = ( x - INV_M ) / INV_PITCH; r = ( y - INV_GRID_Y ) / INV_PITCH;
		return c < INV_COLS && r < INV_ROWS && ( x - INV_M ) % INV_PITCH < INV_CELL && ( y - INV_GRID_Y ) % INV_PITCH < INV_CELL;
	}

	constexpr uintptr_t VT_STORAGE = 0x101C9C0;
	constexpr uintptr_t VT_CART = 0x101D4B8, VT_SEARCH = 0x101C8E8, VT_FILTER = 0x101C810;
	bool g_cart = false, g_search = false;

	// célula do carrinho (a grade do inventário, sem as abas: começa em ui::CART_GRID_Y)
	bool CartCell( int x, int y, int& c, int& r ){
		using namespace ui;
		if( x < INV_M || y < CART_GRID_Y ) return false;
		c = ( x - INV_M ) / INV_PITCH; r = ( y - CART_GRID_Y ) / INV_PITCH;
		return c < INV_COLS && r < INV_ROWS && ( x - INV_M ) % INV_PITCH < INV_CELL && ( y - CART_GRID_Y ) % INV_PITCH < INV_CELL;
	}

	bool SearchRow( int x, int y, int& r ){
		using namespace ui;
		if( x < SR_M || x >= SR_M + SR_ROW_W || y < SR_TOP ) return false;
		r = ( y - SR_TOP ) / SR_RH;
		return r < SR_ROWS && ( y - SR_TOP ) % SR_RH < SR_ROW_H;
	}

	// linha do armazém em (x, y) da janela
	bool OurRow( int x, int y, int& r ){
		using namespace ui;
		if( x < ST_M || x >= ST_M + ST_ROW_W || y < ST_TOP ) return false;
		r = ( y - ST_TOP ) / ST_RH;
		return r < ST_ROWS && ( y - ST_TOP ) % ST_RH < ST_ROW_H;
	}

	void MapPoint( uintptr_t self, int& x, int& y ){
		uintptr_t vt = *reinterpret_cast<uintptr_t*>( self );
		int c, r;
		if( vt == VT_ITEMWND && g_inventory ){
			if( OurCell( x, y, c, r ) ){ x = 0x28 + c * 32 + 16; y = 0x11 + r * 32 + 16; }
			else{ x = -1; y = -1; }
		}else if( vt == VT_CART && g_cart ){
			if( CartCell( x, y, c, r ) ){ x = 0x28 + c * 32 + 16; y = 0x11 + r * 32 + 16; }
			else{ x = -1; y = -1; }
		}else if( ( vt == VT_SEARCH || vt == VT_FILTER ) && g_search ){ // lista do cliente: coluna do ícone em 20..51
			if( SearchRow( x, y, r ) ){ x = 0x14 + 16; y = 0x11 + r * 32 + 16; }
			else{ x = -1; y = -1; }
		}else if( vt == VT_STORAGE && g_storage ){ // a lista do cliente: só a coluna do ícone (40..71), 32 px por linha
			if( OurRow( x, y, r ) ){ x = 0x28 + 16; y = 0x11 + r * 32 + 16; }
			else{ x = -1; y = -1; }
		}
	}

	void* __fastcall InvItemAt( uintptr_t self, void*, void* out, int x, int y ){
		MapPoint( self, x, y );
		return reinterpret_cast<void*( __thiscall* )( uintptr_t, void*, int, int )>( g_tramp )( self, out, x, y );
	}
	void* __fastcall InvItemAt2( uintptr_t self, void*, void* out, int x, int y ){
		MapPoint( self, x, y );
		return reinterpret_cast<void*( __thiscall* )( uintptr_t, void*, int, int )>( g_tramp + 16 )( self, out, x, y );
	}
	int __fastcall InvIndexAt( uintptr_t self, void*, int x, int y ){
		MapPoint( self, x, y );
		return reinterpret_cast<int( __thiscall* )( uintptr_t, int, int )>( g_tramp + 32 )( self, x, y );
	}

	void __fastcall InvDraw( uintptr_t self, void* ){
		__try{
			ui::PaintInventoryWindow( self );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void __stdcall InvTipPos( int* px, int* py, int x, int y ){
		int c = 0, r = 0;
		OurCell( x, y, c, r );
		*px = ui::INV_M + c * ui::INV_PITCH;
		*py = ui::INV_GRID_Y + r * ui::INV_PITCH + 1;
	}

	// 0x948588: esi = x, ebx = y (da janela), edi = janela; a posição vai para [ebp-0x140], [ebp-0x13C]
	__declspec( naked ) void InvTipStub(){
		__asm{
			pushad
			push ebx
			push esi
			lea eax, [ebp - 0x13C]
			push eax
			lea eax, [ebp - 0x140]
			push eax
			call InvTipPos
			popad
			mov ecx, edi
			push INV_TIP_RET
			ret
		}
	}
}

bool charwnd::InventoryActive(){
	return g_inventory;
}

bool charwnd::InstallInventory(){
	const uint8_t sigDraw[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xE5, 0x84, 0xEA, 0x00 };
	const uint8_t sigItem[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x7E, 0x6C, 0xEA, 0x00 };
	const uint8_t sigItem2[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x7E, 0x6B, 0xEA, 0x00 };
	const uint8_t sigIndex[] = { 0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x55, 0x08 };
	const uint8_t sigTip[] = { 0x8D, 0x46, 0xD8, 0x8B, 0xCF };
	const uint8_t sigDbl[] = { 0x83, 0xFF, 0x11 };  // cmp edi, 0x11 (clique duplo no título)
	const uint8_t sigDrag[] = { 0x83, 0xFE, 0x11 }; // cmp esi, 0x11 (apertar no título = arrastar)
	if( memcmp( reinterpret_cast<void*>( INV_ONDRAW ), sigDraw, sizeof( sigDraw ) ) != 0
		|| memcmp( reinterpret_cast<void*>( INV_ITEM_AT ), sigItem, sizeof( sigItem ) ) != 0
		|| memcmp( reinterpret_cast<void*>( INV_ITEM_AT2 ), sigItem2, sizeof( sigItem2 ) ) != 0
		|| memcmp( reinterpret_cast<void*>( INV_INDEX_AT ), sigIndex, sizeof( sigIndex ) ) != 0
		|| memcmp( reinterpret_cast<void*>( INV_TIP_AT ), sigTip, sizeof( sigTip ) ) != 0
		|| memcmp( reinterpret_cast<void*>( INV_DBL_Y ), sigDbl, sizeof( sigDbl ) ) != 0
		|| memcmp( reinterpret_cast<void*>( INV_DRAG_Y ), sigDrag, sizeof( sigDrag ) ) != 0 ){
		logger::Write( "AVISO: janela de inventário diferente do esperado; mantida a original" );
		return false;
	}
	g_tramp = static_cast<uint8_t*>( VirtualAlloc( nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) );
	if( !g_tramp ) return false;
	Trampoline( 0, INV_ITEM_AT, 5 );
	Trampoline( 1, INV_ITEM_AT2, 5 );
	Trampoline( 2, INV_INDEX_AT, 7 );
	PatchJmp( INV_ONDRAW, reinterpret_cast<uintptr_t>( &InvDraw ) );
	PatchJmp( INV_ITEM_AT, reinterpret_cast<uintptr_t>( &InvItemAt ) );
	PatchJmp( INV_ITEM_AT2, reinterpret_cast<uintptr_t>( &InvItemAt2 ) );
	PatchJmp( INV_INDEX_AT, reinterpret_cast<uintptr_t>( &InvIndexAt ) );
	PatchJmp( INV_TIP_AT, reinterpret_cast<uintptr_t>( &InvTipStub ) );
	// apertar no título inteiro (acima das abas) arrasta, não só nos 17 px de cima; o clique duplo nele não
	// minimiza mais (a janela minimizada do cliente não cabe no layout novo)
	const struct { uintptr_t at; uint8_t y; } limits[] = { { INV_DRAG_Y, (uint8_t)( ui::INV_TAB_Y - 4 ) }, { INV_DBL_Y, 0 } };
	for( const auto& l : limits ){
		uint8_t* p = reinterpret_cast<uint8_t*>( l.at + 2 );
		DWORD old;
		VirtualProtect( p, 1, PAGE_EXECUTE_READWRITE, &old );
		*p = l.y;
		VirtualProtect( p, 1, old, &old );
	}
	FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
	g_inventory = true;
	logger::Write( "Inventário: desenho e grade nativos refeitos" );
	return true;
}

// ======================================================================== armazém
namespace {
	constexpr uintptr_t ST_ONDRAW = 0x941810, ST_ITEM_AT = 0x934DD0, ST_TIP_AT = 0x948388, ST_TIP_RET = 0x9483B3, ST_DRAG_Y = 0x945F96;
	constexpr uintptr_t ST_COMBO_OPEN = 0x86012B, ST_COMBO_CLOSE = 0x86019A;
	constexpr uintptr_t FN_ITEM_ICON = 0x870030; // ( x, y, ITEM_INFO*, 0, 0 ): ícone e quantidade
	constexpr uintptr_t FN_ITEM_NAME = 0x894AD0; // ( x, y, largura, ITEM_INFO*, 0, 0, altura ): nome

	void* __fastcall StItemAt( uintptr_t self, void*, void* out, int x, int y ){
		MapPoint( self, x, y );
		return reinterpret_cast<void*( __thiscall* )( uintptr_t, void*, int, int )>( g_tramp + 48 )( self, out, x, y );
	}

	void __fastcall StDraw( uintptr_t self, void* ){
		__try{
			ui::PaintStorageWindow( self );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void __stdcall StTipPos( int* px, int* py, int x, int y ){
		int r = 0;
		OurRow( x, y, r );
		*px = ui::ST_M;
		*py = ui::ST_TOP + r * ui::ST_RH + 1;
	}

	__declspec( naked ) void StTipStub(){
		__asm{
			pushad
			push ebx
			push esi
			lea eax, [ebp - 0x13C]
			push eax
			lea eax, [ebp - 0x140]
			push eax
			call StTipPos
			popad
			mov ecx, edi
			push ST_TIP_RET
			ret
		}
	}
}

void charwnd::ItemIcon( uintptr_t w, int x, int y, uintptr_t info ){
	__try{ reinterpret_cast<void( __thiscall* )( uintptr_t, int, int, uintptr_t, int, int )>( FN_ITEM_ICON )( w, x, y, info, 0, 0 ); }
	__except( EXCEPTION_EXECUTE_HANDLER ){}
}

void charwnd::ItemName( uintptr_t w, int x, int y, int width, uintptr_t info, int height ){
	__try{ reinterpret_cast<void( __thiscall* )( uintptr_t, int, int, int, uintptr_t, int, int, int )>( FN_ITEM_NAME )( w, x, y, width, info, 0, 0, height ); }
	__except( EXCEPTION_EXECUTE_HANDLER ){}
}

bool charwnd::StorageActive(){
	return g_storage;
}

bool charwnd::InstallStorage(){
	const uint8_t sigDraw[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x95, 0x84, 0xEA, 0x00 };
	const uint8_t sigItem[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x3E, 0x6C, 0xEA, 0x00 };
	const uint8_t sigTip[] = { 0x8D, 0x46, 0xD8, 0x8B, 0xCF };
	const uint8_t sigDrag[] = { 0x83, 0xFE, 0x11 };
	if( !g_tramp || memcmp( reinterpret_cast<void*>( ST_ONDRAW ), sigDraw, sizeof( sigDraw ) ) != 0
		|| memcmp( reinterpret_cast<void*>( ST_ITEM_AT ), sigItem, sizeof( sigItem ) ) != 0
		|| memcmp( reinterpret_cast<void*>( ST_TIP_AT ), sigTip, sizeof( sigTip ) ) != 0
		|| memcmp( reinterpret_cast<void*>( ST_DRAG_Y ), sigDrag, sizeof( sigDrag ) ) != 0 ){
		logger::Write( "AVISO: janela do armazém diferente do esperado; mantida a original" );
		return false;
	}
	Trampoline( 3, ST_ITEM_AT, 5 );
	PatchJmp( ST_ONDRAW, reinterpret_cast<uintptr_t>( &StDraw ) );
	PatchJmp( ST_ITEM_AT, reinterpret_cast<uintptr_t>( &StItemAt ) );
	PatchJmp( ST_TIP_AT, reinterpret_cast<uintptr_t>( &StTipStub ) );
	// a ordem (combobox 0x100CF38) abre/fecha com Move( 0xD6, y ): x fixo da janela original -> o x atual dela
	// (push 0xD6 -> push dword ptr [esi+0x1C]; nop; nop)
	for( uintptr_t at : { ST_COMBO_OPEN, ST_COMBO_CLOSE } ){
		const uint8_t want[] = { 0x68, 0xD6, 0x00, 0x00, 0x00 }, put[] = { 0xFF, 0x76, 0x1C, 0x90, 0x90 };
		if( memcmp( reinterpret_cast<void*>( at ), want, 5 ) != 0 ) continue;
		DWORD o;
		VirtualProtect( reinterpret_cast<void*>( at ), 5, PAGE_EXECUTE_READWRITE, &o );
		memcpy( reinterpret_cast<void*>( at ), put, 5 );
		VirtualProtect( reinterpret_cast<void*>( at ), 5, o, &o );
	}
	uint8_t* p = reinterpret_cast<uint8_t*>( ST_DRAG_Y + 2 ); // apertar no título inteiro arrasta
	DWORD old;
	VirtualProtect( p, 1, PAGE_EXECUTE_READWRITE, &old );
	*p = (uint8_t)( ui::ST_TAB_Y - 4 );
	VirtualProtect( p, 1, old, &old );
	FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
	g_storage = true;
	logger::Write( "Armazém: desenho e lista nativos refeitos" );
	return true;
}

// ======================================================================== descrição do item / comparação
// As duas janelas (vt 0x1012B9C e 0x1012D4C) usam o mesmo OnDraw (0x8B0910, índice 20 da vtable): o desenho do
// cliente continua (moldura e topo com as imagens do tema, texto, cartas) e depois entram cabeçalho e runas
// (ui::PaintItemExtras). O cabeçalho (pílula) vem no fundo collection_bg; o nome do cliente vai para dentro dele
// (push 0x5A -> 16 em 0x8B0AFD) e a ilustração desce para y 36 (push 0x0B em 0x8B0ACF).
namespace {
	constexpr uintptr_t IT_ONDRAW = 0x8B0910, IT_ART_Y = 0x8B0ACF, IT_NAME_XY = 0x8B0AFB;
	constexpr uintptr_t IT_VTS[2] = { 0x1012B9C, 0x1012D4C };
	bool g_itemWnd = false;

	void __fastcall ItemDraw( uintptr_t self, void* ){
		reinterpret_cast<void( __thiscall* )( uintptr_t )>( IT_ONDRAW )( self );
		__try{ ui::PaintItemExtras( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

bool charwnd::ItemWindowActive(){
	return g_itemWnd;
}

bool charwnd::InstallItemWindow(){
	const uint8_t sigArt[] = { 0x6A, 0x0B, 0x6A, 0x0A };
	const uint8_t sigName[] = { 0x6A, 0x0A, 0x6A, 0x5A };
	if( memcmp( reinterpret_cast<void*>( IT_NAME_XY ), sigName, sizeof( sigName ) ) != 0 ) return false;
	for( uintptr_t vt : IT_VTS ) if( *reinterpret_cast<uintptr_t*>( vt + 20 * 4 ) != IT_ONDRAW ){
		logger::Write( "AVISO: janela de descrição do item diferente do esperado; mantida a original" );
		return false;
	}
	if( memcmp( reinterpret_cast<void*>( IT_ART_Y ), sigArt, sizeof( sigArt ) ) != 0 ) return false;
	for( uintptr_t vt : IT_VTS ){
		DWORD o;
		uintptr_t* slot = reinterpret_cast<uintptr_t*>( vt + 20 * 4 );
		VirtualProtect( slot, 4, PAGE_READWRITE, &o );
		*slot = reinterpret_cast<uintptr_t>( &ItemDraw );
		VirtualProtect( slot, 4, o, &o );
	}
	DWORD o;
	uint8_t* p = reinterpret_cast<uint8_t*>( IT_ART_Y + 1 );
	VirtualProtect( p, 1, PAGE_EXECUTE_READWRITE, &o );
	*p = 36;
	VirtualProtect( p, 1, o, &o );
	p = reinterpret_cast<uint8_t*>( IT_NAME_XY + 3 );
	VirtualProtect( p, 1, PAGE_EXECUTE_READWRITE, &o );
	*p = 16;
	VirtualProtect( p, 1, o, &o );
	FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
	g_itemWnd = true;
	logger::Write( "Descrição do item: cabeçalho e runas na própria janela" );
	return true;
}

// ======================================================================== carrinho e pesquisa do armazém
namespace {
	constexpr uintptr_t CA_ONDRAW = 0x9436F0, CA_TIP_AT = 0x949078, CA_TIP_RET = 0x9490A3, CA_DRAG_Y = 0x946576;
	constexpr uintptr_t SR_ONDRAW = 0x941270, SR_ITEM_AT = 0x934B50, SR_TIP_AT = 0x947F88, SR_TIP_RET = 0x947FB3, SR_DRAG_Y = 0x945D56;
	// janela que os ícones de filtro do armazém abrem (mesma lista da pesquisa)
	constexpr uintptr_t FI_ONDRAW = 0x941540, FI_ITEM_AT = 0x934C90, FI_TIP_AT = 0x948188, FI_TIP_RET = 0x9481B3, FI_DRAG_Y = 0x945E76;

	void __fastcall CartDraw( uintptr_t self, void* ){
		__try{ ui::PaintCartWindow( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
	void __fastcall SearchDraw( uintptr_t self, void* ){
		__try{ ui::PaintSearchWindow( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
	void* __fastcall SrItemAt( uintptr_t self, void*, void* out, int x, int y ){
		MapPoint( self, x, y );
		return reinterpret_cast<void*( __thiscall* )( uintptr_t, void*, int, int )>( g_tramp + 64 )( self, out, x, y );
	}
	void* __fastcall FiItemAt( uintptr_t self, void*, void* out, int x, int y ){
		MapPoint( self, x, y );
		return reinterpret_cast<void*( __thiscall* )( uintptr_t, void*, int, int )>( g_tramp + 80 )( self, out, x, y );
	}
	void __stdcall CartTipPos( int* px, int* py, int x, int y ){
		int c = 0, r = 0;
		CartCell( x, y, c, r );
		*px = ui::INV_M + c * ui::INV_PITCH;
		*py = ui::CART_GRID_Y + r * ui::INV_PITCH + 1;
	}
	void __stdcall SearchTipPos( int* px, int* py, int x, int y ){
		int r = 0;
		SearchRow( x, y, r );
		*px = ui::SR_M;
		*py = ui::SR_TOP + r * ui::SR_RH + 1;
	}
	__declspec( naked ) void CartTipStub(){
		__asm{
			pushad
			push ebx
			push esi
			lea eax, [ebp - 0x13C]
			push eax
			lea eax, [ebp - 0x140]
			push eax
			call CartTipPos
			popad
			mov ecx, edi
			push CA_TIP_RET
			ret
		}
	}
	__declspec( naked ) void FilterTipStub(){
		__asm{
			pushad
			push ebx
			push esi
			lea eax, [ebp - 0x13C]
			push eax
			lea eax, [ebp - 0x140]
			push eax
			call SearchTipPos
			popad
			mov ecx, edi
			push FI_TIP_RET
			ret
		}
	}
	__declspec( naked ) void SearchTipStub(){
		__asm{
			pushad
			push ebx
			push esi
			lea eax, [ebp - 0x13C]
			push eax
			lea eax, [ebp - 0x140]
			push eax
			call SearchTipPos
			popad
			mov ecx, edi
			push SR_TIP_RET
			ret
		}
	}

	void SetByte( uintptr_t at, uint8_t v ){
		DWORD o;
		VirtualProtect( reinterpret_cast<void*>( at ), 1, PAGE_EXECUTE_READWRITE, &o );
		*reinterpret_cast<uint8_t*>( at ) = v;
		VirtualProtect( reinterpret_cast<void*>( at ), 1, o, &o );
	}
}

bool charwnd::CartActive(){ return g_cart; }
bool charwnd::SearchActive(){ return g_search; }

bool charwnd::InstallCartAndSearch(){
	const uint8_t sigCart[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x45, 0x86, 0xEA, 0x00 };
	const uint8_t sigSearch[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xFD, 0x83, 0xEA, 0x00 };
	const uint8_t sigItem[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xBE, 0x6B, 0xEA, 0x00 };
	const uint8_t sigTip[] = { 0x8D, 0x46, 0xD8, 0x8B, 0xCF };
	const uint8_t sigDrag[] = { 0x83, 0xFE, 0x11 };
	if( !g_tramp ) return false;
	if( memcmp( reinterpret_cast<void*>( CA_ONDRAW ), sigCart, sizeof( sigCart ) ) == 0
		&& memcmp( reinterpret_cast<void*>( CA_TIP_AT ), sigTip, sizeof( sigTip ) ) == 0
		&& memcmp( reinterpret_cast<void*>( CA_DRAG_Y ), sigDrag, sizeof( sigDrag ) ) == 0 ){
		PatchJmp( CA_ONDRAW, reinterpret_cast<uintptr_t>( &CartDraw ) );
		PatchJmp( CA_TIP_AT, reinterpret_cast<uintptr_t>( &CartTipStub ) );
		SetByte( CA_DRAG_Y + 2, (uint8_t)( ui::CART_GRID_Y - 8 ) );
		g_cart = true; // o teste de clique é o 0x9349E0 do inventário (MapPoint pela vtable)
	}else logger::Write( "AVISO: janela do carrinho diferente do esperado; mantida a original" );
	if( memcmp( reinterpret_cast<void*>( SR_ONDRAW ), sigSearch, sizeof( sigSearch ) ) == 0
		&& memcmp( reinterpret_cast<void*>( SR_ITEM_AT ), sigItem, sizeof( sigItem ) ) == 0
		&& memcmp( reinterpret_cast<void*>( SR_TIP_AT ), sigTip, sizeof( sigTip ) ) == 0
		&& memcmp( reinterpret_cast<void*>( SR_DRAG_Y ), sigDrag, sizeof( sigDrag ) ) == 0 ){
		Trampoline( 4, SR_ITEM_AT, 5 );
		PatchJmp( SR_ONDRAW, reinterpret_cast<uintptr_t>( &SearchDraw ) );
		PatchJmp( SR_ITEM_AT, reinterpret_cast<uintptr_t>( &SrItemAt ) );
		PatchJmp( SR_TIP_AT, reinterpret_cast<uintptr_t>( &SearchTipStub ) );
		SetByte( SR_DRAG_Y + 2, (uint8_t)( ui::SR_TOP - 8 ) );
		g_search = true;
	}else logger::Write( "AVISO: janela de pesquisa do armazém diferente do esperado; mantida a original" );
	const uint8_t sigFilter[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x3D, 0x84, 0xEA, 0x00 };
	const uint8_t sigFiItem[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xFE, 0x6B, 0xEA, 0x00 };
	if( g_search && memcmp( reinterpret_cast<void*>( FI_ONDRAW ), sigFilter, sizeof( sigFilter ) ) == 0
		&& memcmp( reinterpret_cast<void*>( FI_ITEM_AT ), sigFiItem, sizeof( sigFiItem ) ) == 0
		&& memcmp( reinterpret_cast<void*>( FI_TIP_AT ), sigTip, sizeof( sigTip ) ) == 0
		&& memcmp( reinterpret_cast<void*>( FI_DRAG_Y ), sigDrag, sizeof( sigDrag ) ) == 0 ){
		Trampoline( 5, FI_ITEM_AT, 5 );
		PatchJmp( FI_ONDRAW, reinterpret_cast<uintptr_t>( &SearchDraw ) );
		PatchJmp( FI_ITEM_AT, reinterpret_cast<uintptr_t>( &FiItemAt ) );
		PatchJmp( FI_TIP_AT, reinterpret_cast<uintptr_t>( &FilterTipStub ) );
		SetByte( FI_DRAG_Y + 2, (uint8_t)( ui::SR_TOP - 8 ) );
	}else logger::Write( "AVISO: janela do filtro do armazém diferente do esperado; mantida a original" );
	FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
	logger::Write( "Carrinho e pesquisa do armazém: desenho e listas nativos refeitos (%d/%d)", g_cart, g_search );
	return g_cart || g_search;
}

// ======================================================================== janelas de aviso (UIMessageBox)
// OnDraw (vtable 20) da mensagem e da que fecha sozinha: moldura e título do tema (ui::PaintMessageBox); o texto
// (caixa 0x100D734) e os botões (imagens btn_ok/btn_cancel do tema) são os do próprio cliente
namespace {
	constexpr uintptr_t MSG_VTS[2] = { 0x100FFD0, 0x10100A8 };
	bool g_msgBox = false;

	void __fastcall MsgDraw( uintptr_t self, void* ){
		__try{ ui::PaintMessageBox( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

bool charwnd::MessageBoxActive(){
	return g_msgBox;
}

bool charwnd::InstallMessageBox(){
	// o OnDraw já pode estar com o acompanhamento das telas de entrada (client::Install): troca-se o original dele
	const client::LoginWnd kinds[2] = { client::LoginWnd::MessageBox, client::LoginWnd::MessageBoxAuto };
	const uintptr_t expect[2] = { 0x87E5E0, 0x87E610 };
	for( int i = 0; i < 2; i++ ){
		if( client::ReplaceLoginOnDraw( kinds[i], reinterpret_cast<void( __thiscall* )( void* )>( &MsgDraw ) ) ) continue;
		uintptr_t* slot = reinterpret_cast<uintptr_t*>( MSG_VTS[i] + 20 * 4 );
		if( *slot != expect[i] ){
			logger::Write( "AVISO: janela de aviso diferente do esperado; mantida a original" );
			return false;
		}
		DWORD o;
		VirtualProtect( slot, 4, PAGE_READWRITE, &o );
		*slot = reinterpret_cast<uintptr_t>( &MsgDraw );
		VirtualProtect( slot, 4, o, &o );
	}
	g_msgBox = true;
	logger::Write( "Janelas de aviso: desenho do tema na propria janela" );
	return true;
}

// ======================================================================== janela de login (UILoginWnd)
namespace {
	constexpr uintptr_t VT_LOGIN = 0x1010258, LOGIN_ONDRAW = 0x87DD90;
	bool g_login = false;

	void __fastcall LoginDraw( uintptr_t self, void* ){
		if( !config::CustomActive() ){ // tela original escolhida pelo jogador: o desenho do cliente
			reinterpret_cast<void( __thiscall* )( uintptr_t )>( LOGIN_ONDRAW )( self );
			return;
		}
		__try{ ui::PaintLoginWindow( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

bool charwnd::LoginActive(){
	return g_login;
}

bool charwnd::InstallLogin(){
	uintptr_t* slot = reinterpret_cast<uintptr_t*>( VT_LOGIN + 20 * 4 );
	if( *slot != LOGIN_ONDRAW ){
		logger::Write( "AVISO: janela de login diferente do esperado; mantida a original" );
		return false;
	}
	DWORD o;
	VirtualProtect( slot, 4, PAGE_READWRITE, &o );
	*slot = reinterpret_cast<uintptr_t>( &LoginDraw );
	VirtualProtect( slot, 4, o, &o );
	g_login = true;
	logger::Write( "Janela de login: painel do tema na propria janela" );
	return true;
}

// ======================================================================== "Conectando" (UIWaitWnd)
namespace {
	constexpr uintptr_t VT_WAIT = 0x10120A4, WAIT_ONDRAW = 0x8B5090;
	bool g_wait = false;

	void __fastcall WaitDraw( uintptr_t self, void* ){
		if( !config::CustomActive() ){ // tela original escolhida pelo jogador: o desenho do cliente
			reinterpret_cast<void( __thiscall* )( uintptr_t )>( WAIT_ONDRAW )( self );
			return;
		}
		__try{ ui::PaintWaitWindow( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

bool charwnd::WaitActive(){
	return g_wait;
}

bool charwnd::InstallWait(){
	uintptr_t* slot = reinterpret_cast<uintptr_t*>( VT_WAIT + 20 * 4 );
	if( *slot != WAIT_ONDRAW ){
		logger::Write( "AVISO: janela de espera diferente do esperado; mantida a original" );
		return false;
	}
	DWORD o;
	VirtualProtect( slot, 4, PAGE_READWRITE, &o );
	*slot = reinterpret_cast<uintptr_t>( &WaitDraw );
	VirtualProtect( slot, 4, o, &o );
	g_wait = true;
	return true;
}

// ======================================================================== escolha do servidor (UISelectServerWnd)
namespace {
	constexpr uintptr_t VT_SERVER = 0x1010180, SERVER_ONDRAW = 0x87F830;
	bool g_server = false;

	void __fastcall ServerDraw( uintptr_t self, void* ){
		if( !config::CustomActive() ){ // tela original escolhida pelo jogador: o desenho do cliente
			reinterpret_cast<void( __thiscall* )( uintptr_t )>( SERVER_ONDRAW )( self );
			return;
		}
		__try{ ui::PaintServerWindow( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

bool charwnd::ServerActive(){
	return g_server;
}

bool charwnd::InstallServer(){
	uintptr_t* slot = reinterpret_cast<uintptr_t*>( VT_SERVER + 20 * 4 );
	if( *slot != SERVER_ONDRAW ){
		logger::Write( "AVISO: janela de servidores diferente do esperado; mantida a original" );
		return false;
	}
	DWORD o;
	VirtualProtect( slot, 4, PAGE_READWRITE, &o );
	*slot = reinterpret_cast<uintptr_t>( &ServerDraw );
	VirtualProtect( slot, 4, o, &o );
	g_server = true;
	return true;
}

// ======================================================================== seleção de personagem
namespace {
	constexpr uintptr_t VT_SELSLOT = 0x100EE28, SELSLOT_ONDRAW = 0x8562B0;
	bool g_select = false;

	void __fastcall SelRowDraw( uintptr_t self, void* ){
		int card = charwnd::SelectActive() ? client::SlotIndexOf( reinterpret_cast<void*>( self ) ) : -1;
		if( card < 0 ){
			reinterpret_cast<void( __thiscall* )( uintptr_t )>( SELSLOT_ONDRAW )( self );
			return;
		}
		__try{ ui::PaintSelectRow( self, card ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void SelPaint( void* self ){
		if( !charwnd::SelectActive() ) return;
		__try{ ui::PaintSelectWindow( reinterpret_cast<uintptr_t>( self ) ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

bool charwnd::SelectActive(){
	return g_select && config::CustomActive() && config::Feature( config::FEAT_SELECT ) && config::Get().uiEnabled;
}

bool charwnd::InstallSelect(){
	uintptr_t* slot = reinterpret_cast<uintptr_t*>( VT_SELSLOT + 20 * 4 );
	if( *slot != SELSLOT_ONDRAW ){
		logger::Write( "AVISO: cartao da selecao diferente do esperado; selecao original mantida" );
		return false;
	}
	DWORD o;
	VirtualProtect( slot, 4, PAGE_READWRITE, &o );
	*slot = reinterpret_cast<uintptr_t>( &SelRowDraw );
	VirtualProtect( slot, 4, o, &o );
	client::SetSelectPaint( &SelPaint );
	g_select = true;
	logger::Write( "Selecao de personagem: desenhada na propria janela" );
	return true;
}

// ======================================================================== criação de personagem
namespace {
	constexpr uintptr_t VT_MAKE = 0xFFDA34, MAKE_ONDRAW = 0x79F390;
	bool g_make = false;

	void __fastcall MakeDraw( uintptr_t self, void* ){
		reinterpret_cast<void( __thiscall* )( uintptr_t )>( MAKE_ONDRAW )( self );
		if( !charwnd::MakeActive() ) return;
		__try{ ui::PaintMakeWindow( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

bool charwnd::MakeActive(){
	return g_make && config::CustomActive() && config::Feature( config::FEAT_MAKE ) && config::Get().uiEnabled;
}

bool charwnd::InstallMake(){
	uintptr_t* slot = reinterpret_cast<uintptr_t*>( VT_MAKE + 20 * 4 );
	if( *slot != MAKE_ONDRAW ){
		logger::Write( "AVISO: janela de criacao diferente do esperado; criacao original mantida" );
		return false;
	}
	DWORD o;
	VirtualProtect( slot, 4, PAGE_READWRITE, &o );
	*slot = reinterpret_cast<uintptr_t>( &MakeDraw );
	VirtualProtect( slot, 4, o, &o );
	g_make = true;
	logger::Write( "Criacao de personagem: desenhada na propria janela" );
	return true;
}

// ======================================================================== conversa com NPC
namespace {
	constexpr uintptr_t VT_SAY = 0x1013184, SAY_ONCREATE = 0x8AAB60, SAY_ONDRAW = 0x8B2640;
	constexpr uintptr_t VT_CHOOSE3 = 0x102A074, VT_CHOOSE = 0x101325C, CHOOSE_ONDRAW = 0x8AF7E0;
	constexpr uintptr_t VT_LIST = 0x100C2A8, LIST_ONDRAW = 0x852E60;
	bool g_npc = false;

	bool IsChoose( uintptr_t w ){
		uintptr_t vt = w ? *reinterpret_cast<uintptr_t*>( w ) : 0;
		return vt == VT_CHOOSE3 || vt == VT_CHOOSE;
	}

	// OnCreate( cx, cy ): o visualizador de texto fica com cx-20 x cy-40 e quebra as linhas em cx-65; com o tamanho
	// do texto da faixa, as falas que chegam depois já quebram na largura certa
	void __fastcall SayCreate( uintptr_t self, void*, int cx, int cy ){
		const bool on = charwnd::NpcActive();
		if( on ){ cx = ui::SAY_TEXT_W + 20; cy = ui::SAY_TEXT_H + 40; }
		reinterpret_cast<void( __thiscall* )( uintptr_t, int, int )>( SAY_ONCREATE )( self, cx, cy );
		if( on ) game::NpcCreated( self, true );
	}

	constexpr uintptr_t CHOOSE_ONCREATE = 0x8A0840;
	void __fastcall ChooseCreate( uintptr_t self, void*, int cx, int cy ){
		reinterpret_cast<void( __thiscall* )( uintptr_t, int, int )>( CHOOSE_ONCREATE )( self, cx, cy );
		if( charwnd::NpcActive() ) game::NpcCreated( self, false );
	}

	void __fastcall SayDraw( uintptr_t self, void* ){
		if( !charwnd::NpcActive() ){ reinterpret_cast<void( __thiscall* )( uintptr_t )>( SAY_ONDRAW )( self ); return; }
		__try{ ui::PaintSayWindow( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void __fastcall ChooseDraw( uintptr_t self, void* ){
		if( !charwnd::NpcActive() ){ reinterpret_cast<void( __thiscall* )( uintptr_t )>( CHOOSE_ONDRAW )( self ); return; }
		__try{ ui::PaintChooseWindow( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void __fastcall ListDraw( uintptr_t self, void* ){
		if( !charwnd::NpcActive() || !IsChoose( *reinterpret_cast<uintptr_t*>( self + 0x10 ) ) ){
			reinterpret_cast<void( __thiscall* )( uintptr_t )>( LIST_ONDRAW )( self );
			return;
		}
		__try{ ui::PaintChoiceList( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	// cutin do script: durante a conversa ele vira o retrato da faixa; a janela dele não desenha nada
	constexpr uintptr_t VT_ILLUST = 0x101C1E4, ILLUST_ONDRAW = 0x92DBF0;
	void __fastcall IllustDraw( uintptr_t self, void* ){
		if( charwnd::NpcActive() && game::NpcSayOpen() ){
			charwnd::Fill( self, 0, 0, *reinterpret_cast<int*>( self + 0x14 ), *reinterpret_cast<int*>( self + 0x18 ), RGB( 255, 0, 255 ) );
			return;
		}
		reinterpret_cast<void( __thiscall* )( uintptr_t )>( ILLUST_ONDRAW )( self );
	}

	bool Swap( uintptr_t vt, int idx, uintptr_t expect, const void* fn ){
		uintptr_t* slot = reinterpret_cast<uintptr_t*>( vt + idx * 4 );
		if( *slot != expect ) return false;
		DWORD o;
		VirtualProtect( slot, 4, PAGE_READWRITE, &o );
		*slot = reinterpret_cast<uintptr_t>( fn );
		VirtualProtect( slot, 4, o, &o );
		return true;
	}
}

bool charwnd::NpcActive(){
	return g_npc && config::Get().uiEnabled;
}

bool charwnd::InstallNpc(){
	if( *reinterpret_cast<uintptr_t*>( VT_SAY + 15 * 4 ) != SAY_ONCREATE || *reinterpret_cast<uintptr_t*>( VT_SAY + 20 * 4 ) != SAY_ONDRAW
		|| *reinterpret_cast<uintptr_t*>( VT_CHOOSE3 + 20 * 4 ) != CHOOSE_ONDRAW || *reinterpret_cast<uintptr_t*>( VT_LIST + 20 * 4 ) != LIST_ONDRAW ){
		logger::Write( "AVISO: janelas de NPC diferentes do esperado; mantidas as originais" );
		return false;
	}
	Swap( VT_SAY, 15, SAY_ONCREATE, &SayCreate );
	Swap( VT_SAY, 20, SAY_ONDRAW, &SayDraw );
	Swap( VT_CHOOSE3, 15, CHOOSE_ONCREATE, &ChooseCreate );
	Swap( VT_CHOOSE, 15, CHOOSE_ONCREATE, &ChooseCreate );
	Swap( VT_CHOOSE3, 20, CHOOSE_ONDRAW, &ChooseDraw );
	Swap( VT_CHOOSE, 20, CHOOSE_ONDRAW, &ChooseDraw );
	Swap( VT_LIST, 20, LIST_ONDRAW, &ListDraw );
	Swap( VT_ILLUST, 20, ILLUST_ONDRAW, &IllustDraw );
	g_npc = true;
	logger::Write( "Conversa com NPC: fala e menu desenhados nas proprias janelas" );
	return true;
}

// ======================================================================== lojas de NPC
namespace {
	struct ShopVt { uintptr_t vt, draw; int kind; };
	const ShopVt SHOP_VTS[] = {
		{ 0x101CB70, 0x940B20, ui::SHOP_LIST }, { 0x101CC48, 0x9406C0, ui::SHOP_LIST2 }, { 0x101CD20, 0x93F6D0, ui::SHOP_CART },
		{ 0x101F16C, 0x93F6D0, ui::SHOP_CART }, { 0x101CDF8, 0x940150, ui::SHOP_SELL }, { 0x101CED0, 0x93FD10, ui::SHOP_SELL2 } };
	constexpr uintptr_t VT_SELLBUY = 0x1013694, SELLBUY_ONDRAW = 0x8AF790;
	bool g_shop = false;

	template <int K> void __fastcall ShopDraw( uintptr_t self, void* ){
		if( !config::Get().uiEnabled ){ reinterpret_cast<void( __thiscall* )( uintptr_t )>( SHOP_VTS[K].draw )( self ); return; }
		__try{ ui::PaintShopWindow( self, SHOP_VTS[K].kind ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void __fastcall SellBuyDraw( uintptr_t self, void* ){
		if( !config::Get().uiEnabled ){ reinterpret_cast<void( __thiscall* )( uintptr_t )>( SELLBUY_ONDRAW )( self ); return; }
		__try{ ui::PaintChooseSellBuy( self ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void* const SHOP_FNS[] = { (void*)&ShopDraw<0>, (void*)&ShopDraw<1>, (void*)&ShopDraw<2>, (void*)&ShopDraw<3>, (void*)&ShopDraw<4>, (void*)&ShopDraw<5> };
}

bool charwnd::InstallShop(){
	int n = 0;
	for( int k = 0; k < 6; k++ ) if( Swap( SHOP_VTS[k].vt, 20, SHOP_VTS[k].draw, SHOP_FNS[k] ) ) n++;
	if( Swap( VT_SELLBUY, 20, SELLBUY_ONDRAW, &SellBuyDraw ) ) n++;
	g_shop = n > 0;
	logger::Write( "Lojas de NPC: %d janelas desenhadas no tema", n );
	return g_shop;
}

// ======================================================================== janelas de mercador (Comércio)
// O desenho é o do cliente; enquanto ele desenha uma dessas janelas, a barra de título vira o painel do tema
// (0x895E30), o rodapé antigo (0x893C30) não é desenhado e as cores claras da lista viram as do tema (0xA063F0)
namespace {
	constexpr uintptr_t FN_TITLEBAR = 0x895E30, FN_BTNBAR = 0x893C30, FN_SLOT = 0x86FFF0, FN_FRAME9 = 0x8701D0;
	struct MerchVt { uintptr_t vt, draw; };
	const MerchVt MERCH_VTS[] = { { 0x101CFA8, 0x942E10 }, { 0x101D080, 0x9426A0 }, { 0x101D230, 0x9428F0 }, { 0x101D740, 0x9439B0 },
		{ 0x101D590, 0x93F320 }, { 0x101D158, 0x9428C0 }, { 0x101D668, 0x93F6C0 }, // ... quantidade
		{ 0x101EAD0, 0x95F740 }, { 0x10124DC, 0x8B1030 }, // histórico de vendas, comparação de equipamento
		{ 0xFF0B9C, 0x702230 }, // grupo / amigos (UIMessengerGroupWnd)
		{ 0x10184F0, 0x8ED090 }, // menu do Esc (UIEscOptionWnd)
		{ 0xFF92F4, 0x77B930 } }; // conquistas (UIAchievementWnd)
	uint8_t* g_mt = nullptr; // trampolins: [0] preenchimento, [16] título, [32] rodapé, [48] casa do item, [64] moldura simples
	volatile int g_legacy = 0;
	uintptr_t g_legacyWnd = 0;

	void* MTramp( int slot, uintptr_t fn, int len ){
		uint8_t* t = g_mt + slot * 16;
		memcpy( t, reinterpret_cast<void*>( fn ), len );
		t[len] = 0xE9;
		*reinterpret_cast<int32_t*>( t + len + 1 ) = (int32_t)( ( fn + len ) - reinterpret_cast<uintptr_t>( t + len + 5 ) );
		return t;
	}

	void __fastcall FillHook( uintptr_t self, void*, int x, int y, int w, int h, uint32_t c ){
		const uintptr_t lvt = g_legacyWnd ? *reinterpret_cast<uintptr_t*>( g_legacyWnd ) : 0;
		if( g_legacy && lvt != 0x10020AC && lvt != 0x1001C20 ){ // (o Rodex usa os brancos de propósito)
			const uint32_t r = c & 0xFF, g = ( c >> 8 ) & 0xFF, b = ( c >> 16 ) & 0xFF;
			if( self == g_legacyWnd ){ // a própria janela (os campos e controles filhos ficam com as cores deles)
				if( r >= 0xE8 && g >= 0xE8 && b >= 0xE8 ) c = RGB( 239, 239, 231 );      // fundo branco da lista -> painel
				else if( b > r + 20 && b > 0x90 ) c = RGB( 206, 214, 231 );              // destaque azulado -> o do tema
			}else if( r == g && g == b && r >= 0x60 && r <= 0xB0 ) c = RGB( 222, 222, 214 ); // campo desativado (cinza escuro)
		}
		reinterpret_cast<void( __thiscall* )( uintptr_t, int, int, int, int, uint32_t )>( g_mt )( self, x, y, w, h, c );
	}

	// devolve a altura do título (o cliente pinta o corpo dali para baixo com 0x8936F0)
	int __fastcall TitleHook( uintptr_t self, void*, int a, const char* s, int c ){
		if( g_legacy ){
			const uintptr_t vt = *reinterpret_cast<uintptr_t*>( self );
			if( vt == 0x10020AC || vt == 0x1001C20 ){ // Rodex (ler/escrever): o fundo já veio; só a faixa do título
				__try{ ui::PaintTitleStrip( self, s ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
				return 17;
			}
			ui::PaintLegacyFrame( self );
			if( vt == 0x10184F0 || vt == 0xFF92F4 ){ // Esc e conquistas: o título vem só por aqui
				__try{ ui::PaintTitleStrip( self, s ); }__except( EXCEPTION_EXECUTE_HANDLER ){}
				if( vt == 0x10184F0 ) return *reinterpret_cast<int*>( self + 0x18 ); // (corpo já pintado: senão cobre as bordas)
			}
			return 17;
		}
		return reinterpret_cast<int( __thiscall* )( uintptr_t, int, const char*, int )>( g_mt + 16 )( self, a, s, c );
	}

	void __fastcall BtnBarHook( uintptr_t self, void*, int a, int b, int c ){
		if( g_legacy ) return;
		reinterpret_cast<void( __thiscall* )( uintptr_t, int, int, int )>( g_mt + 32 )( self, a, b, c );
	}

	// casa do item (itemwin_mid, 32x32): a do tema em volta do ícone. O ícone sai em casa+4 (24x24), menos na "Fazer
	// Comércio", que o põe em (7, 48 + 42n) com a casa em (3, 54 + 42n)
	void __fastcall SlotHook( uintptr_t self, void*, int x, int y ){
		if( g_legacy ){
			uintptr_t vt = *reinterpret_cast<uintptr_t*>( self );
			const int dy = vt == 0x101D740 ? -9 : 0;
			charwnd::Bitmap( self, x + 1, y + 1 + dy, ui::ItemSlotImage() );
			return;
		}
		reinterpret_cast<void( __thiscall* )( uintptr_t, int, int )>( g_mt + 48 )( self, x, y );
	}

	void __fastcall Frame9Hook( uintptr_t self, void*, int a, int b, int c, int d ){
		if( g_legacy && self == g_legacyWnd ){ ui::PaintPlainFrame( self ); return; }
		reinterpret_cast<void( __thiscall* )( uintptr_t, int, int, int, int )>( g_mt + 64 )( self, a, b, c, d );
	}

	template <int K> void __fastcall MerchDraw( uintptr_t self, void* ){
		const bool on = config::Get().uiEnabled;
		if( on ) game::MerchantFields( self );
		uintptr_t prev = g_legacyWnd;
		if( on ){ g_legacy++; g_legacyWnd = self; }
		__try{ reinterpret_cast<void( __thiscall* )( uintptr_t )>( MERCH_VTS[K].draw )( self ); }
		__except( EXCEPTION_EXECUTE_HANDLER ){}
		if( on ){ g_legacy--; g_legacyWnd = prev; }
	}
	void* const MERCH_FNS[] = { (void*)&MerchDraw<0>, (void*)&MerchDraw<1>, (void*)&MerchDraw<2>, (void*)&MerchDraw<3>, (void*)&MerchDraw<4>, (void*)&MerchDraw<5>, (void*)&MerchDraw<6>,
		(void*)&MerchDraw<7>, (void*)&MerchDraw<8>, (void*)&MerchDraw<9>, (void*)&MerchDraw<10>, (void*)&MerchDraw<11> };
}

namespace {
	// barras de progresso das conquistas (UICustomGageBar: +0x7C valor, +0x80 máximo; o original faz moldura,
	// contorno e preenchimento com as cores +0xA4/+0xA8/+0xA0/+0x9C): pílula do tema, trilho cinza e azul
	constexpr uintptr_t VT_ACH_GAGE = 0xFF8DE4, ACH_GAGE_DRAW = 0x77B960;

	void Pill( uintptr_t w, int x, int y, int pw, int ph, uint32_t c ){
		if( pw <= 0 || ph <= 0 ) return;
		if( pw < 3 || ph < 3 ){ charwnd::Fill( w, x, y, pw, ph, c ); return; }
		charwnd::Fill( w, x + 1, y, pw - 2, ph, c );
		charwnd::Fill( w, x, y + 1, pw, ph - 2, c );
	}

	void __fastcall AchGageDraw( uintptr_t self, void* ){
		if( !config::Get().uiEnabled ){ reinterpret_cast<void( __thiscall* )( uintptr_t )>( ACH_GAGE_DRAW )( self ); return; }
		__try{
			const int W = *reinterpret_cast<int*>( self + 0x14 ), H = *reinterpret_cast<int*>( self + 0x18 );
			const int v = *reinterpret_cast<int*>( self + 0x7C ), m = *reinterpret_cast<int*>( self + 0x80 );
			charwnd::Fill( self, 0, 0, W, H, RGB( 255, 0, 255 ) );
			const int h = H > 8 ? 6 : H, y = ( H - h ) / 2;
			Pill( self, 0, y, W, h, RGB( 222, 222, 214 ) );
			if( m > 0 && v > 0 ){
				const int fw = (int)( (long long)W * ( v < m ? v : m ) / m );
				Pill( self, 0, y, fw < 3 ? 3 : fw, h, RGB( 115, 148, 206 ) );
			}
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

bool charwnd::InstallMerchant(){
	const uint8_t sigFill[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 }, sigTitle[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 };
	const uint8_t sigBar[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00 };
	if( memcmp( reinterpret_cast<void*>( FN_FILL ), sigFill, 6 ) || memcmp( reinterpret_cast<void*>( FN_TITLEBAR ), sigTitle, 6 )
		|| memcmp( reinterpret_cast<void*>( FN_BTNBAR ), sigBar, 9 ) ){
		logger::Write( "AVISO: rotinas de moldura diferentes do esperado; janelas de mercador originais" );
		return false;
	}
	const uint8_t sigSlot[] = { 0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0xF9 }, sigFrame[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10 };
	const bool extra = !memcmp( reinterpret_cast<void*>( FN_SLOT ), sigSlot, 7 ) && !memcmp( reinterpret_cast<void*>( FN_FRAME9 ), sigFrame, 6 );
	g_mt = static_cast<uint8_t*>( VirtualAlloc( nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) );
	if( !g_mt ) return false;
	MTramp( 0, FN_FILL, 6 ); MTramp( 1, FN_TITLEBAR, 6 ); MTramp( 2, FN_BTNBAR, 9 );
	if( extra ){
		MTramp( 3, FN_SLOT, 7 ); MTramp( 4, FN_FRAME9, 6 );
		PatchJmp( FN_SLOT, reinterpret_cast<uintptr_t>( &SlotHook ) );
		PatchJmp( FN_FRAME9, reinterpret_cast<uintptr_t>( &Frame9Hook ) );
	}
	PatchJmp( FN_FILL, reinterpret_cast<uintptr_t>( &FillHook ) );
	PatchJmp( FN_TITLEBAR, reinterpret_cast<uintptr_t>( &TitleHook ) );
	PatchJmp( FN_BTNBAR, reinterpret_cast<uintptr_t>( &BtnBarHook ) );
	int n = 0;
	for( int k = 0; k < 12; k++ ) if( Swap( MERCH_VTS[k].vt, 20, MERCH_VTS[k].draw, MERCH_FNS[k] ) ) n++;
	if( Swap( VT_ACH_GAGE, 20, ACH_GAGE_DRAW, &AchGageDraw ) ) n++;
	logger::Write( "Janelas de mercador: %d no tema", n );
	return n > 0;
}
