// Dois modos de carga, decididos pelo nome do arquivo:
//  - d3d9.dll (proxy): o Codex.exe carrega este arquivo da pasta do cliente, que repassa
//    tudo ao d3d9.dll do sistema e instala os hooks da seleção;
//  - outro nome (ex.: codex_ui.dll, importado pelo exe via patch de DLL do WARP): desvia a
//    importação d3d9.dll!Direct3DCreate9Ex do executável. Exporta "Init" no ordinal 1.
#include <windows.h>
#include <d3d9.h>

#include "client.hpp"
#include "config.hpp"
#include "d3d_hooks.hpp"
#include "game.hpp"
#include "skin.hpp"
#include "log.hpp"

namespace {
	HMODULE g_real = nullptr;

	using Direct3DCreate9Fn = IDirect3D9*( WINAPI* )( UINT );
	using Direct3DCreate9ExFn = HRESULT( WINAPI* )( UINT, IDirect3D9Ex** );
}

#define PASSTHROUGH_LIST( X ) \
	X( D3DPERF_BeginEvent ) \
	X( D3DPERF_EndEvent ) \
	X( D3DPERF_GetStatus ) \
	X( D3DPERF_QueryRepeatFrame ) \
	X( D3DPERF_SetMarker ) \
	X( D3DPERF_SetOptions ) \
	X( D3DPERF_SetRegion ) \
	X( DebugSetLevel ) \
	X( DebugSetMute ) \
	X( Direct3D9EnableMaximizedWindowedModeShim ) \
	X( Direct3DCreate9On12 ) \
	X( Direct3DCreate9On12Ex ) \
	X( Direct3DShaderValidatorCreate9 ) \
	X( PSGPError ) \
	X( PSGPSampleTexture )

#define DECLARE_POINTER( name ) static FARPROC p_##name = nullptr;
PASSTHROUGH_LIST( DECLARE_POINTER )

static FARPROC p_Direct3DCreate9 = nullptr;
static FARPROC p_Direct3DCreate9Ex = nullptr;

// Nunca é chamado dentro do DllMain (loader lock), só na primeira exportação usada.
extern "C" void ResolveRealD3D9(){
	if( g_real != nullptr ){
		return;
	}

	wchar_t path[MAX_PATH];
	GetSystemDirectoryW( path, MAX_PATH );
	wcscat_s( path, L"\\d3d9.dll" );
	g_real = LoadLibraryW( path );
	if( g_real == nullptr ){
		logger::Write( "ERRO: nao foi possivel carregar %ls", path );
		return;
	}

#define RESOLVE_POINTER( name ) p_##name = GetProcAddress( g_real, #name );
	PASSTHROUGH_LIST( RESOLVE_POINTER )
	p_Direct3DCreate9 = GetProcAddress( g_real, "Direct3DCreate9" );
	p_Direct3DCreate9Ex = GetProcAddress( g_real, "Direct3DCreate9Ex" );
}

// Exportações repassadas sem alteração: resolve e salta direto para o d3d9.dll real.
#define DEFINE_PASSTHROUGH( name ) \
	extern "C" __declspec( naked ) void Proxy_##name(){ \
		__asm { call ResolveRealD3D9 } \
		__asm { jmp dword ptr [p_##name] } \
	} \
	__pragma( comment( linker, "/EXPORT:" #name "=_Proxy_" #name ) )
PASSTHROUGH_LIST( DEFINE_PASSTHROUGH )

#pragma comment( linker, "/EXPORT:Direct3DCreate9=_Proxy_Direct3DCreate9@4" )
extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9( UINT sdk ){
	ResolveRealD3D9();
	if( p_Direct3DCreate9 == nullptr ){
		return nullptr;
	}
	IDirect3D9* d3d = reinterpret_cast<Direct3DCreate9Fn>( p_Direct3DCreate9 )( sdk );
	d3dhooks::OnDirect3DCreated( d3d, false );
	return d3d;
}

#pragma comment( linker, "/EXPORT:Direct3DCreate9Ex=_Proxy_Direct3DCreate9Ex@8" )
extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex( UINT sdk, IDirect3D9Ex** out ){
	ResolveRealD3D9();
	if( p_Direct3DCreate9Ex == nullptr ){
		return D3DERR_NOTAVAILABLE;
	}
	HRESULT hr = reinterpret_cast<Direct3DCreate9ExFn>( p_Direct3DCreate9Ex )( sdk, out );
	if( SUCCEEDED( hr ) && out != nullptr ){
		d3dhooks::OnDirect3DCreated( *out, true );
	}
	return hr;
}

// Ordinal 1: o patch de DLL do WARP importa por ele (a função em si não faz nada)
#pragma comment( linker, "/EXPORT:Init=_CodexInit@0,@1" )
extern "C" int WINAPI CodexInit(){
	return 1;
}

namespace {
	Direct3DCreate9ExFn g_exeCreate9Ex = nullptr;

	HRESULT WINAPI ImportedCreate9Ex( UINT sdk, IDirect3D9Ex** out ){
		HRESULT hr = g_exeCreate9Ex( sdk, out );
		if( SUCCEEDED( hr ) && out != nullptr ){
			d3dhooks::OnDirect3DCreated( *out, true );
		}
		return hr;
	}

	// Troca a entrada d3d9.dll!Direct3DCreate9Ex na tabela de importação (IAT) do executável.
	// Procura pelo endereço já resolvido (não depende da lista de nomes, que o WARP pode omitir).
	bool HookExeImport(){
		HMODULE d3d9 = GetModuleHandleW( L"d3d9.dll" );
		FARPROC target = d3d9 ? GetProcAddress( d3d9, "Direct3DCreate9Ex" ) : nullptr;
		if( target == nullptr ) return false;
		auto base = reinterpret_cast<BYTE*>( GetModuleHandleW( nullptr ) );
		auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>( base + reinterpret_cast<IMAGE_DOS_HEADER*>( base )->e_lfanew );
		const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if( dir.VirtualAddress == 0 ) return false;
		for( auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>( base + dir.VirtualAddress ); imp->Name != 0; imp++ ){
			if( _stricmp( reinterpret_cast<char*>( base + imp->Name ), "d3d9.dll" ) != 0 || imp->FirstThunk == 0 ) continue;
			for( auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>( base + imp->FirstThunk ); iat->u1.Function != 0; iat++ ){
				if( iat->u1.Function != reinterpret_cast<ULONG_PTR>( target ) ) continue;
				DWORD old;
				VirtualProtect( &iat->u1.Function, sizeof( iat->u1.Function ), PAGE_READWRITE, &old );
				g_exeCreate9Ex = reinterpret_cast<Direct3DCreate9ExFn>( target );
				iat->u1.Function = reinterpret_cast<ULONG_PTR>( &ImportedCreate9Ex );
				VirtualProtect( &iat->u1.Function, sizeof( iat->u1.Function ), old, &old );
				return true;
			}
		}
		return false;
	}

	bool LoadedAsD3D9( HINSTANCE instance ){
		wchar_t path[MAX_PATH] = {};
		if( GetModuleFileNameW( instance, path, MAX_PATH ) == 0 ) return false; // embutida no executável
		const wchar_t* name = wcsrchr( path, L'\\' );
		return _wcsicmp( name ? name + 1 : path, L"d3d9.dll" ) == 0;
	}
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, LPVOID ){
	if( reason == DLL_PROCESS_ATTACH ){
		DisableThreadLibraryCalls( instance );
		d3dhooks::SetSelfImage( instance );
		logger::Init();
		config::Load();
		if( config::Get().enabled ){
			client::Install();
			if( config::Feature( config::FEAT_HUD ) || config::Feature( config::FEAT_ITEMWND ) ) game::InstallEarly();
			if( config::Feature( config::FEAT_FADE ) ) client::InstallFastFade( config::Get().fadeMs );
			bool custom = config::CustomActive() && config::Feature( config::FEAT_COMBAT );
			client::SetSelectedAction( custom ? config::Get().selectedAction : -1, config::Get().actionFrameMs,
				custom ? config::Get().attackAction : -1, config::Get().attackEveryMs );
			if( !LoadedAsD3D9( instance ) ){
				bool ok = HookExeImport();
				logger::Write( ok ? "Modo importacao: Direct3DCreate9Ex do executavel desviado"
					: "ERRO: modo importacao sem Direct3DCreate9Ex na tabela do executavel" );
			}
		}
	}
	return TRUE;
}
