// Diagnóstico (debug=1): registra os arquivos que o cliente tenta abrir e, quando ele
// mostra uma MessageBox, grava a mensagem junto com os últimos arquivos pedidos.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "log.hpp"

namespace {
	constexpr uintptr_t IAT_CREATEFILEA = 0xFA1224;
	constexpr uintptr_t IAT_FOPEN = 0xFA1A44;
	constexpr uintptr_t IAT_FOPEN_S = 0xFA1A14;
	constexpr uintptr_t IAT_MESSAGEBOXA = 0xFA1628;
	constexpr int RECENT = 40;

	using CreateFileAFn = HANDLE( WINAPI* )( LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE );
	using FopenFn = FILE*( __cdecl* )( const char*, const char* );
	using FopenSFn = errno_t( __cdecl* )( FILE**, const char*, const char* );
	using MessageBoxAFn = int( WINAPI* )( HWND, LPCSTR, LPCSTR, UINT );

	CreateFileAFn o_CreateFileA = nullptr;
	FopenFn o_fopen = nullptr;
	FopenSFn o_fopen_s = nullptr;
	MessageBoxAFn o_MessageBoxA = nullptr;

	std::string g_recent[RECENT];
	int g_next = 0;

	void Note( const char* api, const char* name, bool ok ){
		if( name == nullptr ){
			return;
		}
		char buf[400];
		snprintf( buf, sizeof( buf ), "%s %s %s", api, ok ? "ok   " : "FALHA", name );
		g_recent[g_next] = buf;
		g_next = ( g_next + 1 ) % RECENT;
	}

	HANDLE WINAPI Hook_CreateFileA( LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl ){
		HANDLE h = o_CreateFileA( name, access, share, sa, disp, flags, tmpl );
		Note( "CreateFileA", name, h != INVALID_HANDLE_VALUE );
		return h;
	}

	FILE* __cdecl Hook_fopen( const char* name, const char* mode ){
		FILE* f = o_fopen( name, mode );
		Note( "fopen", name, f != nullptr );
		return f;
	}

	errno_t __cdecl Hook_fopen_s( FILE** out, const char* name, const char* mode ){
		errno_t e = o_fopen_s( out, name, mode );
		Note( "fopen_s", name, e == 0 );
		return e;
	}

	int WINAPI Hook_MessageBoxA( HWND wnd, LPCSTR text, LPCSTR caption, UINT type ){
		logger::Write( "MessageBoxA \"%s\": %s", caption ? caption : "", text ? text : "" );
		logger::Write( "Ultimos arquivos pedidos pelo cliente:" );
		for( int i = 0; i < RECENT; i++ ){
			const std::string& s = g_recent[( g_next + i ) % RECENT];
			if( !s.empty() ){
				logger::Write( "  %s", s.c_str() );
			}
		}
		return o_MessageBoxA( wnd, text, caption, type );
	}

	template <typename T> void PatchIat( uintptr_t slot, T hook, T& original ){
		uintptr_t* p = reinterpret_cast<uintptr_t*>( slot );
		original = reinterpret_cast<T>( *p );
		DWORD old;
		VirtualProtect( p, sizeof( *p ), PAGE_READWRITE, &old );
		*p = reinterpret_cast<uintptr_t>( hook );
		VirtualProtect( p, sizeof( *p ), old, &old );
	}
}

namespace filelog {
	void Install(){
		PatchIat( IAT_CREATEFILEA, &Hook_CreateFileA, o_CreateFileA );
		PatchIat( IAT_FOPEN, &Hook_fopen, o_fopen );
		PatchIat( IAT_FOPEN_S, &Hook_fopen_s, o_fopen_s );
		PatchIat( IAT_MESSAGEBOXA, &Hook_MessageBoxA, o_MessageBoxA );
		logger::Write( "Registro de arquivos (debug) instalado" );
	}
}
