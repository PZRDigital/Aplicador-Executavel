// Skin escura das janelas do jogo (estilo Ragnarok Zero, cores O Codex).
//  - Imagens: o cliente procura cada arquivo primeiro no disco (CreateFileA) e só depois nas GRFs.
//    Se a char.grf tiver a versão recolorida (skin\<fnv>.<ext>, gerada por tools\skin.py), ela é
//    copiada para savedata\charselect\skin\ e aberta no lugar da original.
//  - Textos: o cliente escreve com GDI (SetTextColor); cores escuras viram claras para ler no fundo
//    escuro (branco e cores claras ficam como estão).
#include "skin.hpp"

#include <windows.h>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.hpp"
#include "log.hpp"
#include "vfs.hpp"

namespace {
	constexpr uintptr_t IAT_CREATEFILEA = 0xFA1224;
	constexpr uintptr_t IAT_SETTEXTCOLOR = 0xFA10E0;
	constexpr uintptr_t IAT_CREATESOLIDBRUSH = 0xFA1104;
	constexpr uintptr_t IAT_SETBKCOLOR = 0xFA1108;
	// CDCBitmap (imagem em memória onde cada janela é montada): método 11 = ClearSurface(RECT*, cor)
	constexpr uintptr_t VT_CDCBITMAP = 0xFB5B34;
	constexpr uintptr_t FN_CDCBITMAP_CLEAR = 0x530970;

	using CreateFileAFn = HANDLE( WINAPI* )( LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE );
	using SetTextColorFn = COLORREF( WINAPI* )( HDC, COLORREF );
	CreateFileAFn o_CreateFileA = nullptr;
	SetTextColorFn o_SetTextColor = nullptr;
	using CreateSolidBrushFn = HBRUSH( WINAPI* )( COLORREF );
	CreateSolidBrushFn o_CreateSolidBrush = nullptr;
	SetTextColorFn o_SetBkColor = nullptr;

	std::mutex g_lock;
	std::unordered_map<uint32_t, std::wstring> g_files; // fnv -> arquivo extraído ("" = sem versão da skin)
	bool g_ready = false;
	volatile bool g_textOn = false;

	uint32_t Fnv( const char* s ){
		uint32_t h = 0x811C9DC5u;
		for( ; *s; s++ ){
			unsigned char c = (unsigned char)*s;
			if( c >= 'A' && c <= 'Z' ) c = c - 'A' + 'a';
			if( c == '/' ) c = '\\';
			h ^= c;
			h *= 0x01000193u;
		}
		return h;
	}

	bool IsData( const char* s ){
		if( s[0] == '.' && ( s[1] == '\\' || s[1] == '/' ) ) s += 2;
		return _strnicmp( s, "data\\", 5 ) == 0 || _strnicmp( s, "data/", 5 ) == 0;
	}

	// Caminho da versão da skin já extraída (vazio = não há)
	std::wstring SkinFile( const char* name ){
		if( name[0] == '.' && ( name[1] == '\\' || name[1] == '/' ) ) name += 2;
		const char* dot = strrchr( name, '.' );
		if( dot == nullptr || ( _stricmp( dot, ".bmp" ) != 0 && _stricmp( dot, ".tga" ) != 0 ) ) return std::wstring();
		uint32_t h = Fnv( name );
		std::lock_guard<std::mutex> g( g_lock );
		auto it = g_files.find( h );
		if( it != g_files.end() ) return it->second;
		wchar_t file[32];
		swprintf( file, 32, L"%08x%hs", h, dot );
		for( wchar_t* p = file; *p; p++ ) *p = towlower( *p );
		std::wstring out;
		std::vector<unsigned char> data;
		if( vfs::Read( config::BaseDir() + L"skin\\" + file, data ) ){
			std::wstring dir = vfs::WritableDir() + L"skin\\";
			CreateDirectoryW( dir.c_str(), nullptr );
			std::wstring path = dir + file;
			HANDLE f = CreateFileW( path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
			if( f != INVALID_HANDLE_VALUE ){
				DWORD wrote = 0;
				WriteFile( f, data.data(), (DWORD)data.size(), &wrote, nullptr );
				CloseHandle( f );
				if( wrote == data.size() ) out = path;
			}
		}
		g_files[h] = out;
		if( out.empty() && config::Get().debug ) logger::Write( "Skin: sem versao para %s", name );
		return out;
	}

	HANDLE WINAPI Hook_CreateFileA( LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl ){
		if( g_ready && name != nullptr && disp == OPEN_EXISTING && !( access & GENERIC_WRITE ) && IsData( name ) ){
			std::wstring skin = SkinFile( name );
			if( !skin.empty() ){
				HANDLE h = CreateFileW( skin.c_str(), access, share, sa, disp, flags, tmpl );
				if( h != INVALID_HANDLE_VALUE ) return h;
			}
		}
		return o_CreateFileA( name, access, share, sa, disp, flags, tmpl );
	}

	// Cor escura -> mesma cor, clara (luminosidade mínima); preto vira branco azulado
	COLORREF Lighten( COLORREF c ){
		float r = GetRValue( c ) / 255.0f, g = GetGValue( c ) / 255.0f, b = GetBValue( c ) / 255.0f;
		float mx = std::max( r, std::max( g, b ) ), mn = std::min( r, std::min( g, b ) );
		float l = ( mx + mn ) * 0.5f;
		if( l >= 0.6f ) return c;
		float d = mx - mn;
		if( d < 0.08f ){ // neutro
			return RGB( 230, 236, 244 );
		}
		float s = d / ( 1.0f - fabsf( 2.0f * l - 1.0f ) );
		float h;
		if( mx == r ) h = fmodf( ( g - b ) / d, 6.0f );
		else if( mx == g ) h = ( b - r ) / d + 2.0f;
		else h = ( r - g ) / d + 4.0f;
		h *= 60.0f;
		if( h < 0 ) h += 360.0f;
		float nl = 0.74f, ns = std::min( 1.0f, s );
		float cc = ( 1.0f - fabsf( 2.0f * nl - 1.0f ) ) * ns;
		float hp = h / 60.0f;
		float x = cc * ( 1.0f - fabsf( fmodf( hp, 2.0f ) - 1.0f ) );
		float rr = 0, gg = 0, bb = 0;
		if( hp < 1 ){ rr = cc; gg = x; }
		else if( hp < 2 ){ rr = x; gg = cc; }
		else if( hp < 3 ){ gg = cc; bb = x; }
		else if( hp < 4 ){ gg = x; bb = cc; }
		else if( hp < 5 ){ rr = x; bb = cc; }
		else { rr = cc; bb = x; }
		float m = nl - cc * 0.5f;
		return RGB( (int)( ( rr + m ) * 255 + 0.5f ), (int)( ( gg + m ) * 255 + 0.5f ), (int)( ( bb + m ) * 255 + 0.5f ) );
	}

	// Fundo claro e neutro (ou azul-claro do kRO) -> azul-marinho da skin
	COLORREF Darken( COLORREF c ){
		float r = GetRValue( c ) / 255.0f, g = GetGValue( c ) / 255.0f, b = GetBValue( c ) / 255.0f;
		float mx = std::max( r, std::max( g, b ) ), mn = std::min( r, std::min( g, b ) );
		float l = ( mx + mn ) * 0.5f, d = mx - mn;
		float s = d < 1e-4f ? 0 : d / ( 1 - fabsf( 2 * l - 1 ) );
		if( l < 0.62f || s > 0.55f ) return c;
		float nl = 0.055f + ( 1 - l ) * 0.34f;
		float cc = ( 1 - fabsf( 2 * nl - 1 ) ) * 0.42f, m = nl - cc * 0.5f;
		float x = cc * ( 1 - fabsf( fmodf( 216.0f / 60.0f, 2.0f ) - 1 ) );
		return RGB( (int)( m * 255 + 0.5f ), (int)( ( x + m ) * 255 + 0.5f ), (int)( ( cc + m ) * 255 + 0.5f ) );
	}

	// Fundos lisos das janelas (ex.: lista de habilidades, abas): cor 0xAABBGGRR
	using ClearSurfaceFn = void( __thiscall* )( void*, const RECT*, DWORD );
	ClearSurfaceFn o_ClearSurface = nullptr;
	void __fastcall Hook_ClearSurface( void* self, void*, const RECT* rect, DWORD color ){
		if( g_textOn ){
			COLORREF c = Darken( color & 0xFFFFFF );
			color = ( color & 0xFF000000 ) | c;
		}
		o_ClearSurface( self, rect, color );
	}

	HBRUSH WINAPI Hook_CreateSolidBrush( COLORREF color ){
		return o_CreateSolidBrush( g_textOn ? Darken( color ) : color );
	}

	COLORREF WINAPI Hook_SetBkColor( HDC dc, COLORREF color ){
		return o_SetBkColor( dc, g_textOn ? Darken( color ) : color );
	}

	COLORREF WINAPI Hook_SetTextColor( HDC dc, COLORREF color ){
		return o_SetTextColor( dc, g_textOn ? Lighten( color ) : color );
	}

	template <typename T> bool PatchIat( uintptr_t slot, T hook, T& original ){
		uintptr_t* p = reinterpret_cast<uintptr_t*>( slot );
		if( *p == 0 ) return false;
		original = reinterpret_cast<T>( *p );
		DWORD old;
		if( !VirtualProtect( p, sizeof( *p ), PAGE_READWRITE, &old ) ) return false;
		*p = reinterpret_cast<uintptr_t>( hook );
		VirtualProtect( p, sizeof( *p ), old, &old );
		return true;
	}
}

void skin::Install(){
	if( !vfs::Exists( config::BaseDir() + L"skin\\lista.txt" ) ){
		logger::Write( "Skin: charselect\\skin ausente; janelas do jogo com a aparência original" );
		return;
	}
	bool a = PatchIat( IAT_CREATEFILEA, &Hook_CreateFileA, o_CreateFileA );
	bool b = PatchIat( IAT_SETTEXTCOLOR, &Hook_SetTextColor, o_SetTextColor );
	PatchIat( IAT_CREATESOLIDBRUSH, &Hook_CreateSolidBrush, o_CreateSolidBrush );
	PatchIat( IAT_SETBKCOLOR, &Hook_SetBkColor, o_SetBkColor );
	uintptr_t* slot = reinterpret_cast<uintptr_t*>( VT_CDCBITMAP + 11 * 4 );
	if( *slot == FN_CDCBITMAP_CLEAR ){
		o_ClearSurface = reinterpret_cast<ClearSurfaceFn>( *slot );
		DWORD old;
		VirtualProtect( slot, sizeof( *slot ), PAGE_READWRITE, &old );
		*slot = reinterpret_cast<uintptr_t>( &Hook_ClearSurface );
		VirtualProtect( slot, sizeof( *slot ), old, &old );
	}
	g_ready = a;
	logger::Write( "Skin: imagens %s, textos %s", a ? "ok" : "FALHOU", b ? "ok" : "FALHOU" );
}

void skin::SetTextLightening( bool on ){
	g_textOn = on;
}
