#include "lgp.hpp"

#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.hpp"
#include "d3d_hooks.hpp"
#include "gfx.hpp"
#include "log.hpp"
#include "vfs.hpp"

// Endereços do Codex.exe 2025-06-04 (base fixa 0x400000). Evidência de cada um em Projeto LGP\MAPA_CLIENTE.md.
namespace {
	constexpr uintptr_t G_MODE_MGR = 0x11EDEB8;      // CModeMgr: +0x58 tipo do modo (1 = jogo), +4 modo atual
	constexpr uintptr_t VT_GAME_MODE = 0x106F658;    // vtable de CGameMode
	constexpr uintptr_t OFS_GM_WORLD = 0xCC;         // CGameMode -> CWorld*
	constexpr uintptr_t OFS_GM_VIEW = 0xD0;          // CGameMode -> CView*
	constexpr uintptr_t OFS_WORLD_PLAYER = 0x2C;     // CWorld -> CPlayer* (o próprio personagem)
	constexpr uintptr_t OFS_WORLD_ATTR = 0x30;       // CWorld -> C3dAttr*
	constexpr uintptr_t OFS_VIEW_MATRIX = 0x98;      // CView: matriz da câmera usada pelo ProjectVertex
	constexpr uintptr_t OFS_PLAYER_POS = 0x10;       // CPlayer: x (+0x10), altura (+0x14), z (+0x18)
	constexpr uintptr_t OFS_ATTR_WIDTH = 0x110, OFS_ATTR_HEIGHT = 0x114, OFS_ATTR_ZOOM = 0x118, OFS_ATTR_CELLS = 0x11C;
	constexpr int CELL_TYPE_BLOCKED = 1;             // célula não andável (C3dAttr: 4 alturas + tipo)

	constexpr uintptr_t G_RENDERER = 0x122A390;      // CRenderer* global
	constexpr uintptr_t FN_PROJECT = 0x546980;       // CRenderer::ProjectVertex(const vector3d*, const matrix*, tlvertex*)
	constexpr uintptr_t G_WINDOW_MGR = 0x12F8268;    // UIWindowMgr global
	constexpr uintptr_t FN_WND_SENDMSG = 0xA33190;   // UIWindowMgr::SendMsg(1, texto, COLORREF, 0, 0) = linha no chat
	constexpr uintptr_t FN_SEND_PACKET = 0xBFC400;   // CRagConnection::SendPacket(len, buf): pacote em claro na entrada

	constexpr uint16_t CZ_REQUEST_CHAT = 0x00F3;
	constexpr uint16_t ZC_NOTIFY_GROUNDSKILL = 0x0117;
	constexpr uint16_t ZC_SKILL_DISAPPEAR = 0x0120;
	constexpr uint16_t ZC_SKILL_ENTRY = 0x011F, ZC_SKILL_ENTRY2 = 0x01C9, ZC_SKILL_ENTRY3 = 0x08C7, ZC_SKILL_ENTRY4 = 0x099F, ZC_SKILL_ENTRY5 = 0x09CA;
	constexpr uint32_t UNT_DUMMYSKILL = 0x86;        // unit invisível (Storm Gust, LoV, Meteor no rAthena)

	// Tipos próprios para as áreas marcadas pelo lançamento (ZC_NOTIFY_GROUNDSKILL); mesmas chaves do lgp.ini.
	// Ficam abaixo de 0x7E (primeiro UNT_* do rAthena). Mapeamento skill -> área em [LGP::LANCAMENTO].
	constexpr uint32_t AREA_STORMGUST = 0x10, AREA_METEOR = 0x11, AREA_VERMILION = 0x12, AREA_HEAVENDRIVE = 0x13;
	constexpr uint32_t SLOT_SQUARE = 0x100000, SLOT_CIRCLE = 0x100001; // fora da faixa de units

	constexpr COLORREF MSG_COLOR = RGB( 0xFF, 0xFF, 0x00 );

	// ---------------------------------------------------------------- memória do cliente
	template <typename T> bool Rd( uintptr_t a, T& out ){
		__try{ out = *reinterpret_cast<const T*>( a ); return true; }
		__except( EXCEPTION_EXECUTE_HANDLER ){ return false; }
	}

	struct Tl { float x, y, z, oow; }; // tlvertex3d de saída do ProjectVertex

	bool Project( uintptr_t renderer, const float* matrix, const float pos[3], Tl& out ){
		__try{
			reinterpret_cast<void( __thiscall* )( uintptr_t, const float*, const float*, Tl* )>( FN_PROJECT )( renderer, pos, matrix, &out );
			return out.oow > 0.0f;
		}__except( EXCEPTION_EXECUTE_HANDLER ){ return false; }
	}

	// Estado do mundo lido a cada quadro (nada é guardado entre quadros além do C3dAttr para detectar troca de mapa)
	struct World {
		uintptr_t attr = 0, renderer = 0;
		const float* matrix = nullptr;
		int width = 0, height = 0, zoom = 5;
		uintptr_t cells = 0;
		int px = 0, py = 0; // célula do personagem
		float wx = 0, wz = 0; // posição do personagem no mundo
	};
	const char* g_why = "";  // motivo da última falha de leitura do mundo (diagnóstico)

	bool ReadWorld( World& w ){
		int type = 0;
		uintptr_t mode = 0, vt = 0, world = 0, view = 0, player = 0;
		g_why = "modo";
		if( !Rd( G_MODE_MGR + 0x58, type ) || type != 1 || !Rd( G_MODE_MGR + 4, mode ) || !mode ) return false;
		g_why = "vtable do CGameMode";
		if( !Rd( mode, vt ) || vt != VT_GAME_MODE ) return false;
		g_why = "CWorld/CView";
		if( !Rd( mode + OFS_GM_WORLD, world ) || !world || !Rd( mode + OFS_GM_VIEW, view ) || !view ) return false;
		g_why = "C3dAttr/CPlayer";
		if( !Rd( world + OFS_WORLD_ATTR, w.attr ) || !w.attr || !Rd( world + OFS_WORLD_PLAYER, player ) || !player ) return false;
		g_why = "CRenderer";
		if( !Rd( G_RENDERER, w.renderer ) || !w.renderer ) return false;
		g_why = "campos do C3dAttr";
		if( !Rd( w.attr + OFS_ATTR_WIDTH, w.width ) || !Rd( w.attr + OFS_ATTR_HEIGHT, w.height ) || !Rd( w.attr + OFS_ATTR_ZOOM, w.zoom )
			|| !Rd( w.attr + OFS_ATTR_CELLS, w.cells ) ) return false;
		if( w.width <= 0 || w.height <= 0 || w.width > 2048 || w.height > 2048 || w.zoom <= 0 || !w.cells ) return false;
		float x = 0, z = 0;
		g_why = "posicao do CPlayer";
		if( !Rd( player + OFS_PLAYER_POS, x ) || !Rd( player + OFS_PLAYER_POS + 8, z ) ) return false;
		g_why = "";
		w.wx = x;
		w.wz = z;
		w.matrix = reinterpret_cast<const float*>( view + OFS_VIEW_MATRIX );
		w.px = (int)std::floor( x / w.zoom ) + w.width / 2;
		w.py = (int)std::floor( z / w.zoom ) + w.height / 2;
		return true;
	}

	struct Cell { float h[4]; int type; };

	bool ReadCell( const World& w, int x, int y, Cell& c ){
		if( x < 0 || y < 0 || x >= w.width || y >= w.height ) return false;
		return Rd( w.cells + ( (size_t)y * w.width + x ) * sizeof( Cell ), c );
	}

	// ---------------------------------------------------------------- desvios
	void* Detour( uintptr_t at, const uint8_t* expect, size_t n, void* hook, const char* name ){
		if( n < 5 || memcmp( reinterpret_cast<void*>( at ), expect, n ) != 0 ){
			logger::Write( "LGP: assinatura de %s diferente em %08X; recurso desativado", name, (unsigned)at );
			return nullptr;
		}
		auto* t = static_cast<uint8_t*>( VirtualAlloc( nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) );
		if( t == nullptr ) return nullptr;
		memcpy( t, reinterpret_cast<void*>( at ), n );
		t[n] = 0xE9;
		*reinterpret_cast<int32_t*>( t + n + 1 ) = (int32_t)( ( at + n ) - ( reinterpret_cast<uintptr_t>( t ) + n + 5 ) );
		auto* p = reinterpret_cast<uint8_t*>( at );
		DWORD old;
		VirtualProtect( p, n, PAGE_EXECUTE_READWRITE, &old );
		p[0] = 0xE9;
		*reinterpret_cast<int32_t*>( p + 1 ) = (int32_t)( reinterpret_cast<uintptr_t>( hook ) - ( at + 5 ) );
		for( size_t i = 5; i < n; i++ ) p[i] = 0x90;
		VirtualProtect( p, n, old, &old );
		FlushInstructionCache( GetCurrentProcess(), p, n );
		return t;
	}

	// ---------------------------------------------------------------- configuração (lgp.ini)
	struct Style {
		D3DCOLOR color = 0;      // 0 = não desenha
		std::wstring image;      // arquivo em lgp\ (vazio = textura padrão)
		DWORD fadeMs = 0;        // 0 = sem esmaecer
		int radius = 0;          // raio em células (áreas e units de célula única que cobrem 3x3)
		DWORD durationMs = 0;    // áreas: tempo na tela
	};

	struct Settings {
		std::unordered_map<uint32_t, Style> styles;
		std::unordered_map<uint16_t, uint32_t> casts; // id da skill -> área marcada no lançamento
		int circleRadius = 9;
	};
	Settings g_cfg;

	// Escolhas do jogador
	struct Prefs {
		bool on = true, aoes = true, circle = false, square = false;
		int squareSize = 5;
	};
	Prefs g_prefs;

	using Ini = std::map<std::wstring, std::map<std::wstring, std::wstring>>;

	std::wstring Lower( std::wstring s ){
		for( auto& ch : s ) ch = (wchar_t)towlower( ch );
		return s;
	}

	std::wstring Trim( const std::wstring& s ){
		size_t a = s.find_first_not_of( L" \t\r\n" );
		if( a == std::wstring::npos ) return L"";
		size_t b = s.find_last_not_of( L" \t\r\n" );
		return s.substr( a, b - a + 1 );
	}

	bool ParseIni( const std::wstring& path, Ini& out ){
		std::vector<unsigned char> data;
		if( !vfs::Read( path, data ) ) return false;
		size_t skip = data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ? 3 : 0;
		int n = MultiByteToWideChar( CP_UTF8, 0, reinterpret_cast<const char*>( data.data() + skip ), (int)( data.size() - skip ), nullptr, 0 );
		std::wstring text( n, L'\0' );
		if( n > 0 ) MultiByteToWideChar( CP_UTF8, 0, reinterpret_cast<const char*>( data.data() + skip ), (int)( data.size() - skip ), &text[0], n );
		std::wstring section;
		size_t pos = 0;
		while( pos < text.size() ){
			size_t nl = text.find( L'\n', pos );
			std::wstring line = Trim( text.substr( pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos ) );
			pos = nl == std::wstring::npos ? text.size() : nl + 1;
			if( line.empty() || line[0] == L';' || line[0] == L'#' ) continue;
			if( line[0] == L'[' ){
				size_t e = line.find( L']' );
				section = Lower( Trim( line.substr( 1, e == std::wstring::npos ? std::wstring::npos : e - 1 ) ) );
				continue;
			}
			size_t eq = line.find( L'=' );
			if( eq == std::wstring::npos ) continue;
			out[section][Lower( Trim( line.substr( 0, eq ) ) )] = Trim( line.substr( eq + 1 ) );
		}
		return true;
	}

	// "SkillXXXX" (hexadecimal, como no plugin.ini) -> tipo da unit
	bool SkillKey( const std::wstring& key, uint32_t& id ){
		if( key.size() < 6 || key.compare( 0, 5, L"skill" ) != 0 ) return false;
		wchar_t* end = nullptr;
		unsigned long v = wcstoul( key.c_str() + 5, &end, 16 );
		if( end == key.c_str() + 5 || *end != 0 ) return false;
		id = (uint32_t)v;
		return true;
	}

	void LoadSettings(){
		Settings s;
		// padrões do servidor (pré-renovação, db/pre-re/skill_db.yml): Storm Gust 9x9 por 4,6 s, LoV 11x11 por 4 s, Meteor 7x7
		s.styles[AREA_STORMGUST] = { D3DCOLOR_ARGB( 0x60, 0x40, 0xA0, 0xFF ), L"", 0, 4, 4600 };
		s.styles[AREA_VERMILION] = { D3DCOLOR_ARGB( 0x60, 0xFF, 0xE0, 0x40 ), L"", 0, 5, 4000 };
		s.styles[AREA_METEOR] = { D3DCOLOR_ARGB( 0x60, 0xFF, 0x50, 0x20 ), L"", 0, 3, 1000 };
		s.styles[AREA_HEAVENDRIVE] = { D3DCOLOR_ARGB( 0x60, 0xA0, 0x52, 0x2D ), L"", 1000, 2, 1000 }; // Fúria da Terra 5x5
		s.styles[0x85] = { D3DCOLOR_ARGB( 0x60, 0x40, 0xFF, 0x60 ), L"", 0, 1, 0 }; // Pneuma: só o centro vem como 0x85
		// WZ_STORMGUST, WZ_METEOR, WZ_VERMILION, WZ_HEAVENDRIVE
		s.casts = { { 89, AREA_STORMGUST }, { 83, AREA_METEOR }, { 85, AREA_VERMILION }, { 91, AREA_HEAVENDRIVE } };
		s.styles[SLOT_SQUARE] = { D3DCOLOR_ARGB( 0x77, 0xDF, 0x01, 0x01 ) };
		s.styles[SLOT_CIRCLE] = { D3DCOLOR_ARGB( 0x77, 0xFF, 0xFF, 0xFF ) };

		Ini ini;
		std::wstring path = config::BaseDir() + L"lgp.ini";
		if( !ParseIni( path, ini ) ){
			logger::Write( "LGP: %ls nao encontrado; usando padroes", path.c_str() );
		}
		auto hex = []( const std::wstring& v ){ return (D3DCOLOR)wcstoul( v.c_str(), nullptr, 16 ); };
		for( auto& kv : ini[L"lgp::cell_color"] ){
			uint32_t id;
			if( kv.first == L"square_color" ) s.styles[SLOT_SQUARE].color = hex( kv.second );
			else if( kv.first == L"circle_color" ) s.styles[SLOT_CIRCLE].color = hex( kv.second );
			else if( SkillKey( kv.first, id ) ) s.styles[id].color = hex( kv.second );
		}
		for( auto& kv : ini[L"lgp::cell_image"] ){
			uint32_t id;
			if( kv.first == L"square_image" ) s.styles[SLOT_SQUARE].image = kv.second;
			else if( kv.first == L"circle_image" ) s.styles[SLOT_CIRCLE].image = kv.second;
			else if( SkillKey( kv.first, id ) ) s.styles[id].image = kv.second;
		}
		for( auto& kv : ini[L"lgp::cell_fade_out_time"] ){
			uint32_t id;
			if( SkillKey( kv.first, id ) ) s.styles[id].fadeMs = wcstoul( kv.second.c_str(), nullptr, 10 );
		}
		for( auto& kv : ini[L"lgp::area"] ){ // SkillXXXX = raio[,duração em ms]
			uint32_t id;
			if( !SkillKey( kv.first, id ) ) continue;
			s.styles[id].radius = std::max( 0, std::min( 20, _wtoi( kv.second.c_str() ) ) );
			size_t comma = kv.second.find( L',' );
			if( comma != std::wstring::npos ) s.styles[id].durationMs = wcstoul( kv.second.c_str() + comma + 1, nullptr, 10 );
		}
		for( auto& kv : s.styles ){ // imagem sem cor: cor padrão (mesma regra do plugin.ini)
			if( !kv.second.image.empty() && kv.second.color == 0 ) kv.second.color = D3DCOLOR_ARGB( 0x77, 0x00, 0xFF, 0x00 );
		}
		for( auto& kv : ini[L"lgp::lancamento"] ){ // id da skill (decimal) = chave da área (hexadecimal, como em SkillXXXX)
			int skill = _wtoi( kv.first.c_str() );
			uint32_t area = (uint32_t)wcstoul( kv.second.c_str(), nullptr, 16 );
			if( skill <= 0 || skill > 0xFFFF ) continue;
			if( area == 0 ) s.casts.erase( (uint16_t)skill );
			else s.casts[(uint16_t)skill] = area;
		}
		// skill com cor e sem imagem própria: imagem_padrao de [LGP] (vazia = quadrado gerado pelo LGP)
		std::wstring defaultImage = ini[L"lgp"][L"imagem_padrao"];
		if( !defaultImage.empty() ){
			for( auto& kv : s.styles ){
				if( kv.second.image.empty() && ( kv.second.color >> 24 ) != 0 ) kv.second.image = defaultImage;
			}
		}
		auto& geral = ini[L"lgp"];
		if( geral.count( L"raio_circulo" ) ) s.circleRadius = std::max( 2, std::min( 20, _wtoi( geral[L"raio_circulo"].c_str() ) ) );
		g_cfg = std::move( s );
		logger::Write( "LGP: %u estilos de celula carregados", (unsigned)g_cfg.styles.size() );
	}

	std::wstring PrefsPath(){ return vfs::WritableDir() + L"preferencias.ini"; }

	void LoadPrefs(){
		std::wstring p = PrefsPath();
		g_prefs.on = GetPrivateProfileIntW( L"lgp", L"ligado", 1, p.c_str() ) != 0;
		g_prefs.aoes = GetPrivateProfileIntW( L"lgp", L"areas", 1, p.c_str() ) != 0;
		g_prefs.circle = GetPrivateProfileIntW( L"lgp", L"circulo", 0, p.c_str() ) != 0;
		g_prefs.square = GetPrivateProfileIntW( L"lgp", L"quadrado", 0, p.c_str() ) != 0;
		g_prefs.squareSize = std::max( 1, std::min( 18, (int)GetPrivateProfileIntW( L"lgp", L"quadrado_tamanho", 5, p.c_str() ) ) );
	}

	void SavePrefs(){
		std::wstring p = PrefsPath();
		WritePrivateProfileStringW( L"lgp", L"ligado", g_prefs.on ? L"1" : L"0", p.c_str() );
		WritePrivateProfileStringW( L"lgp", L"areas", g_prefs.aoes ? L"1" : L"0", p.c_str() );
		WritePrivateProfileStringW( L"lgp", L"circulo", g_prefs.circle ? L"1" : L"0", p.c_str() );
		WritePrivateProfileStringW( L"lgp", L"quadrado", g_prefs.square ? L"1" : L"0", p.c_str() );
		WritePrivateProfileStringW( L"lgp", L"quadrado_tamanho", std::to_wstring( g_prefs.squareSize ).c_str(), p.c_str() );
	}

	// ---------------------------------------------------------------- skills no chão
	struct Unit { int x = 0, y = 0; uint32_t type = 0; DWORD tick = 0; };
	struct Area { int x = 0, y = 0; uint32_t type = 0; DWORD tick = 0; };

	std::mutex g_lock;
	std::unordered_map<uint32_t, Unit> g_units; // id da unit -> unit (ZC_SKILL_ENTRY / ZC_SKILL_DISAPPEAR)
	std::vector<Area> g_areas;                  // lançamentos de Storm Gust / LoV / Meteor
	uintptr_t g_lastAttr = 0;

	template <typename T> T Get( const uint8_t* p, size_t ofs ){
		T v;
		memcpy( &v, p + ofs, sizeof( T ) );
		return v;
	}

	// ---------------------------------------------------------------- chat
	void ArmTrace(); // diagnóstico (definido junto do desenho)

	void PushChat( const char* text ){
		__try{
			reinterpret_cast<int( __thiscall* )( uintptr_t, int, int, int, int, int )>( FN_WND_SENDMSG )(
				G_WINDOW_MGR, 1, reinterpret_cast<int>( text ), (int)MSG_COLOR, 0, 0 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void ChatLine( const wchar_t* text ){
		UINT cp = config::Get().codepage;
		int n = WideCharToMultiByte( cp, 0, text, -1, nullptr, 0, nullptr, nullptr );
		if( n <= 0 ) return;
		std::string s( n, '\0' );
		WideCharToMultiByte( cp, 0, text, -1, &s[0], n, nullptr, nullptr );
		PushChat( s.c_str() );
	}

	// sem argumento alterna; "on"/"off" ligam/desligam
	void Toggle( bool& flag, const std::string& arg, const wchar_t* on, const wchar_t* off ){
		flag = arg == "on" ? true : arg == "off" ? false : !flag;
		SavePrefs();
		ChatLine( flag ? on : off );
	}

	// texto depois de "nome : " -> true se for um comando do LGP (não vai para o servidor)
	bool Command( std::string msg ){
		while( !msg.empty() && ( msg.back() == ' ' || msg.back() == '\0' ) ) msg.pop_back();
		std::string cmd = msg.substr( 0, msg.find( ' ' ) );
		std::string arg = cmd.size() < msg.size() ? msg.substr( cmd.size() + 1 ) : "";
		for( auto& ch : cmd ) ch = (char)tolower( (unsigned char)ch );
		for( auto& ch : arg ) ch = (char)tolower( (unsigned char)ch );
		if( cmd == "@lgp" && arg == "diag" ){ ArmTrace(); ChatLine( L"LGP: diagnóstico gravado no charselect.log." ); return true; }
		if( cmd == "@lgp" ){ Toggle( g_prefs.on, arg, L"LGP ligado.", L"LGP desligado." ); return true; }
		if( cmd == "@aoes" ){ Toggle( g_prefs.aoes, arg, L"LGP: áreas das skills ligadas.", L"LGP: áreas das skills desligadas (só o centro)." ); return true; }
		if( cmd == "@circle" ){ Toggle( g_prefs.circle, arg, L"LGP: anel ligado.", L"LGP: anel desligado." ); return true; }
		if( cmd == "@square" ){
			if( arg == "on" ){ g_prefs.square = true; SavePrefs(); ChatLine( L"LGP: quadrado ligado." ); return true; }
			if( arg == "off" ){ g_prefs.square = false; SavePrefs(); ChatLine( L"LGP: quadrado desligado." ); return true; }
			char* end = nullptr;
			long n = arg.empty() ? -1 : strtol( arg.c_str(), &end, 10 );
			if( arg.empty() || end == arg.c_str() || *end != 0 ){
				ChatLine( L"LGP: use @square on, @square off ou @square 1-18." );
				return true;
			}
			if( n < 1 || n > 18 ){ ChatLine( L"LGP: o tamanho do quadrado vai de 1 a 18." ); return true; }
			g_prefs.squareSize = (int)n;
			g_prefs.square = true;
			SavePrefs();
			ChatLine( ( L"LGP: quadrado a " + std::to_wstring( n ) + L" células." ).c_str() );
			return true;
		}
		return false;
	}

	using SendPacketFn = bool( __thiscall* )( void*, int, const void* );
	SendPacketFn o_sendPacket = nullptr;

	bool __fastcall Hook_SendPacket( void* conn, void*, int len, const void* buf ){
		if( buf != nullptr && len > 4 && Get<uint16_t>( static_cast<const uint8_t*>( buf ), 0 ) == CZ_REQUEST_CHAT ){
			std::string text( static_cast<const char*>( buf ) + 4, strnlen( static_cast<const char*>( buf ) + 4, (size_t)len - 4 ) );
			size_t sep = text.find( " : " );
			if( sep != std::string::npos && Command( text.substr( sep + 3 ) ) ) return true;
		}
		return o_sendPacket( conn, len, buf );
	}

	// ---------------------------------------------------------------- desenho
	struct Vertex { float x, y, z, rhw; D3DCOLOR color; float u, v; };
	constexpr DWORD VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

	gfx::Image g_tile;                                     // textura padrão: borda forte e miolo suave
	std::unordered_map<std::wstring, gfx::Image> g_images; // imagens do lgp.ini (lgp\arquivo)
	std::unordered_map<IDirect3DTexture9*, std::vector<Vertex>> g_batch;

	IDirect3DTexture9* TileTexture( IDirect3DDevice9* dev ){
		if( g_tile.tex == nullptr ){
			constexpr unsigned N = 32;
			std::vector<uint32_t> px( N * N );
			for( unsigned y = 0; y < N; y++ ){
				for( unsigned x = 0; x < N; x++ ){
					unsigned edge = std::min( std::min( x, y ), std::min( N - 1 - x, N - 1 - y ) );
					uint32_t a = edge < 2 ? 0xFF : ( edge < 3 ? 0xB0 : 0x70 );
					px[y * N + x] = ( a << 24 ) | 0xFFFFFF;
				}
			}
			gfx::CreateImage( dev, N, N, px.data(), g_tile );
		}
		return g_tile.tex;
	}

	IDirect3DTexture9* TextureFor( IDirect3DDevice9* dev, const Style& st ){
		if( st.image.empty() ) return TileTexture( dev );
		auto it = g_images.find( st.image );
		if( it == g_images.end() ){
			gfx::Image img;
			std::vector<unsigned char> px;
			unsigned w = 0, h = 0;
			if( gfx::DecodeImageFile( config::BaseDir() + L"lgp\\" + st.image, px, w, h, 256 ) ){
				// transparência do Ragnarok: magenta (FF00FF) vira alfa 0
				for( size_t k = 0; k + 3 < px.size(); k += 4 ){
					if( px[k] >= 0xF8 && px[k + 1] <= 0x08 && px[k + 2] >= 0xF8 ) px[k] = px[k + 1] = px[k + 2] = px[k + 3] = 0;
				}
				gfx::CreateImage( dev, w, h, px.data(), img );
			}else{
				logger::Write( "LGP: imagem lgp\\%ls nao encontrada; usando a padrao", st.image.c_str() );
			}
			it = g_images.emplace( st.image, img ).first;
		}
		return it->second.tex != nullptr ? it->second.tex : TileTexture( dev );
	}

	D3DCOLOR Faded( D3DCOLOR c, DWORD elapsed, DWORD fadeMs ){
		if( fadeMs == 0 ) return c;
		if( elapsed >= fadeMs ) return 0;
		DWORD a = ( c >> 24 ) * ( fadeMs - elapsed ) / fadeMs;
		return ( a << 24 ) | ( c & 0xFFFFFF );
	}

	// Uma célula do chão: os 4 cantos com as alturas do C3dAttr, levemente acima do chão
	void PushCell( IDirect3DDevice9* dev, const World& w, int cx, int cy, D3DCOLOR color, const Style& st ){
		if( ( color >> 24 ) == 0 ) return;
		Cell c;
		if( !ReadCell( w, cx, cy, c ) || c.type == CELL_TYPE_BLOCKED ) return;
		float x0 = (float)( ( cx - w.width / 2 ) * w.zoom ), z0 = (float)( ( cy - w.height / 2 ) * w.zoom );
		float x1 = x0 + w.zoom, z1 = z0 + w.zoom;
		const float corners[4][3] = { { x0, c.h[0] - 1.0f, z0 }, { x1, c.h[1] - 1.0f, z0 }, { x0, c.h[2] - 1.0f, z1 }, { x1, c.h[3] - 1.0f, z1 } };
		// v = 0 no lado de z menor: imagens de células do Ragnarok (ex.: textos "PNEUMA") ficam legíveis
		const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
		Vertex v[4];
		for( int k = 0; k < 4; k++ ){
			Tl t;
			if( !Project( w.renderer, w.matrix, corners[k], t ) ) return;
			v[k] = { t.x, t.y, 0.0f, t.oow, color, uv[k][0], uv[k][1] };
		}
		auto& out = g_batch[TextureFor( dev, st )];
		const int order[6] = { 0, 1, 2, 2, 1, 3 };
		for( int k : order ) out.push_back( v[k] );
	}

	void PushArea( IDirect3DDevice9* dev, const World& w, int cx, int cy, int radius, D3DCOLOR color, const Style& st ){
		for( int dy = -radius; dy <= radius; dy++ )
			for( int dx = -radius; dx <= radius; dx++ ) PushCell( dev, w, cx + dx, cy + dy, color, st );
	}

	// ---------------------------------------------------------------- diagnóstico: linha do tempo de alguns quadros
	// BS/ES = BeginScene/EndScene do cliente, Z0/Z1 = SetRenderState(ZENABLE), dN = N desenhos do cliente,
	// LGP = onde as marcações foram desenhadas.
	// Quadro do Codex.exe observado em jogo: BS Z1 d660 Z0 Z1 d36 Z0 d27 ES (mundo, personagens, interface).
	// O LGP desenha no primeiro ZENABLE=FALSE da cena: depois do chão, antes dos personagens e das janelas.
	int g_sceneDepth = 0;
	bool g_drawnThisScene = false;
	int g_traceUi = 0;          // quadros (EndScene) ainda a registrar
	int g_diagDraws = 0;        // próximos desenhos do LGP a detalhar
	std::string g_trace;
	unsigned g_traceDraws = 0;

	void TraceFlushDraws(){
		if( g_traceDraws ){ g_trace += " d" + std::to_string( g_traceDraws ); g_traceDraws = 0; }
	}
	void TraceEvent( const char* e ){
		if( g_traceUi <= 0 ) return;
		TraceFlushDraws();
		g_trace += ' ';
		g_trace += e;
		if( g_trace.size() > 1500 ){ logger::Write( "LGP trace:%s", g_trace.c_str() ); g_trace.clear(); }
	}
	void ArmTrace(){
		g_traceUi = 3;
		g_diagDraws = 3;
		g_trace.clear();
		g_traceDraws = 0;
		logger::Write( "LGP: registrando 3 quadros (diagnostico)" );
	}

	using SceneFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9* );
	using SetRSFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD );
	SceneFn o_beginScene = nullptr, o_endScene = nullptr;
	SetRSFn o_setRS = nullptr;

	void SafeDraw();

	HRESULT STDMETHODCALLTYPE Hook_BeginScene( IDirect3DDevice9* d ){
		HRESULT hr = o_beginScene( d );
		if( SUCCEEDED( hr ) ){ g_sceneDepth++; g_drawnThisScene = false; }
		TraceEvent( SUCCEEDED( hr ) ? "BS" : "BS(falhou)" );
		return hr;
	}
	HRESULT STDMETHODCALLTYPE Hook_EndScene( IDirect3DDevice9* d ){
		TraceEvent( "ES" );
		if( g_traceUi > 0 && --g_traceUi == 0 ){
			TraceFlushDraws();
			logger::Write( "LGP trace:%s", g_trace.c_str() );
			g_trace.clear();
		}
		if( g_sceneDepth > 0 ) g_sceneDepth--;
		return o_endScene( d );
	}
	HRESULT STDMETHODCALLTYPE Hook_SetRS( IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v ){
		if( st != D3DRS_ZENABLE || gfx::IsDrawing() ) return o_setRS( d, st, v );
		TraceEvent( v ? "Z1" : "Z0" );
		HRESULT hr = o_setRS( d, st, v );
		if( v == FALSE && !g_drawnThisScene && g_sceneDepth > 0 ){
			g_drawnThisScene = true;
			TraceEvent( "LGP" );
			SafeDraw();
		}
		return hr;
	}

	template <typename T> void PatchVtable( void* obj, int idx, T hook, T& orig ){
		void** vt = *reinterpret_cast<void***>( obj );
		if( vt[idx] == reinterpret_cast<void*>( hook ) ) return; // vtable compartilhada já alterada
		DWORD old;
		VirtualProtect( &vt[idx], sizeof( void* ), PAGE_EXECUTE_READWRITE, &old );
		orig = reinterpret_cast<T>( vt[idx] );
		vt[idx] = reinterpret_cast<void*>( hook );
		VirtualProtect( &vt[idx], sizeof( void* ), old, &old );
	}

	const Style* StyleOf( uint32_t id ){
		auto it = g_cfg.styles.find( id );
		return it == g_cfg.styles.end() || ( it->second.color >> 24 ) == 0 ? nullptr : &it->second;
	}

	void Draw(){
		if( !g_prefs.on ) return;
		IDirect3DDevice9* dev = d3dhooks::Device();
		World w;
		if( dev == nullptr || !ReadWorld( w ) ){
			if( g_diagDraws > 0 ){ g_diagDraws--; logger::Write( "LGP diag: mundo nao lido (%s), device=%p", dev ? g_why : "sem device", dev ); }
			return;
		}
		DWORD now = GetTickCount();
		g_batch.clear();
		{
			std::lock_guard<std::mutex> g( g_lock );
			if( w.attr != g_lastAttr ){ // mapa novo: o que havia no anterior não vale mais
				g_lastAttr = w.attr;
				g_units.clear();
				g_areas.clear();
				ArmTrace();
			}
			for( size_t i = 0; i < g_areas.size(); ){
				const Area& a = g_areas[i];
				const Style* st = StyleOf( a.type );
				DWORD elapsed = now - a.tick;
				DWORD life = st ? std::max( st->durationMs, st->fadeMs ) : 0;
				if( st == nullptr || elapsed > ( life ? life : 1000 ) ){ g_areas.erase( g_areas.begin() + i ); continue; }
				PushArea( dev, w, a.x, a.y, g_prefs.aoes ? st->radius : 0, Faded( st->color, elapsed, st->fadeMs ), *st );
				i++;
			}
			for( auto& kv : g_units ){
				const Unit& u = kv.second;
				const Style* st = StyleOf( u.type );
				if( st == nullptr ) continue;
				PushArea( dev, w, u.x, u.y, st->radius, Faded( st->color, now - u.tick, st->fadeMs ), *st );
			}
		}
		if( g_prefs.square ){
			if( const Style* st = StyleOf( SLOT_SQUARE ) ){
				int n = g_prefs.squareSize;
				for( int d = -n; d <= n; d++ ){
					PushCell( dev, w, w.px + d, w.py - n, st->color, *st );
					PushCell( dev, w, w.px + d, w.py + n, st->color, *st );
					if( d != -n && d != n ){
						PushCell( dev, w, w.px - n, w.py + d, st->color, *st );
						PushCell( dev, w, w.px + n, w.py + d, st->color, *st );
					}
				}
			}
		}
		if( g_prefs.circle ){
			if( const Style* st = StyleOf( SLOT_CIRCLE ) ){
				int r = g_cfg.circleRadius;
				for( int dy = -r - 1; dy <= r + 1; dy++ )
					for( int dx = -r - 1; dx <= r + 1; dx++ )
						if( (int)std::lround( std::sqrt( (double)( dx * dx + dy * dy ) ) ) == r ) PushCell( dev, w, w.px + dx, w.py + dy, st->color, *st );
			}
		}
		size_t verts = 0;
		for( auto& kv : g_batch ) verts += kv.second.size();
		bool diag = g_diagDraws > 0;
		if( diag ){
			g_diagDraws--;
			Cell c{};
			bool cellOk = ReadCell( w, w.px, w.py, c );
			logger::Write( "LGP diag: mapa %dx%d zoom %d, personagem mundo (%.1f, %.1f) celula (%d, %d) altura %.1f tipo %d (%d), quadrados %u, cena %d",
				w.width, w.height, w.zoom, w.wx, w.wz, w.px, w.py, c.h[0], c.type, cellOk, (unsigned)( verts / 6 ), g_sceneDepth );
			const float pos[3] = { w.wx, cellOk ? c.h[0] : 0.0f, w.wz };
			Tl t{};
			bool pok = Project( w.renderer, w.matrix, pos, t );
			logger::Write( "LGP diag: personagem projetado ok=%d tela (%.1f, %.1f) z %.4f oow %.5f", pok, t.x, t.y, t.z, t.oow );
			if( verts ){
				const Vertex& v = g_batch.begin()->second[0];
				logger::Write( "LGP diag: 1o vertice (%.1f, %.1f) rhw %.5f cor %08X", v.x, v.y, v.rhw, (unsigned)v.color );
			}
		}
		if( verts == 0 ) return;
		HRESULT hr = E_FAIL;
		if( gfx::Begin( dev ) ){
			dev->SetFVF( VERTEX_FVF );
			dev->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
			dev->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE );
			for( auto& kv : g_batch ){
				dev->SetTexture( 0, kv.first );
				hr = dev->DrawPrimitiveUP( D3DPT_TRIANGLELIST, (UINT)( kv.second.size() / 3 ), kv.second.data(), sizeof( Vertex ) );
			}
			dev->SetTexture( 0, nullptr );
			gfx::End();
		}
		if( diag ) logger::Write( "LGP diag: desenho hr=%08X", (unsigned)hr );
	}

	void SafeDraw(){
		__try{ Draw(); }__except( EXCEPTION_EXECUTE_HANDLER ){ logger::Write( "LGP: excecao no desenho" ); }
	}
}

bool lgp::Install(){
	LoadSettings();
	LoadPrefs();
	const uint8_t sigProject[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x0C };
	const uint8_t sigSendMsg[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
	if( memcmp( reinterpret_cast<void*>( FN_PROJECT ), sigProject, sizeof( sigProject ) ) != 0
		|| memcmp( reinterpret_cast<void*>( FN_WND_SENDMSG ), sigSendMsg, sizeof( sigSendMsg ) ) != 0 ){
		logger::Write( "LGP: executavel diferente do esperado (Codex.exe 2025-06-04); LGP desativado" );
		return false;
	}
	const uint8_t sigSend[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1 };                      // push ebp; mov ebp, esp; push esi; mov esi, ecx
	o_sendPacket = reinterpret_cast<SendPacketFn>( Detour( FN_SEND_PACKET, sigSend, sizeof( sigSend ), reinterpret_cast<void*>( &Hook_SendPacket ), "SendPacket" ) );
	logger::Write( "LGP: comandos=%d (ligado=%d areas=%d anel=%d quadrado=%d/%d); desenho no device D3D9",
		o_sendPacket != nullptr,
		g_prefs.on, g_prefs.aoes, g_prefs.circle, g_prefs.square, g_prefs.squareSize );
	return true;
}

void lgp::OnPacket( uint16_t id, const uint8_t* b, size_t len ){
	DWORD now = GetTickCount();
	switch( id ){
	case ZC_NOTIFY_GROUNDSKILL: { // SKID +2, AID +4, nível +8, x +0xA, y +0xC
		if( len < 14 ) return;
		std::lock_guard<std::mutex> g( g_lock );
		auto it = g_cfg.casts.find( Get<uint16_t>( b, 2 ) );
		if( it == g_cfg.casts.end() ) return;
		uint32_t type = it->second;
		g_areas.push_back( { Get<int16_t>( b, 10 ), Get<int16_t>( b, 12 ), type, now } );
		return;
	}
	case ZC_SKILL_DISAPPEAR: {
		if( len < 6 ) return;
		std::lock_guard<std::mutex> g( g_lock );
		g_units.erase( Get<uint32_t>( b, 2 ) );
		return;
	}
	case ZC_SKILL_ENTRY: case ZC_SKILL_ENTRY2: case ZC_SKILL_ENTRY3: case ZC_SKILL_ENTRY4: case ZC_SKILL_ENTRY5: {
		// 0x011F sem campo de tamanho; os demais têm o tamanho em +2 e o resto deslocado 2 bytes
		size_t o = id == ZC_SKILL_ENTRY ? 0 : 2;
		bool wide = id == ZC_SKILL_ENTRY5 || id == ZC_SKILL_ENTRY4; // tipo da unit em int32
		if( len < o + 15 + ( wide ? 3 : 0 ) ) return;
		Unit u;
		uint32_t gid = Get<uint32_t>( b, o + 2 );
		u.x = Get<int16_t>( b, o + 10 );
		u.y = Get<int16_t>( b, o + 12 );
		u.type = wide ? Get<uint32_t>( b, o + 14 ) : b[o + 14];
		u.tick = now;
		if( u.type == UNT_DUMMYSKILL ) return;
		std::lock_guard<std::mutex> g( g_lock );
		g_units[gid] = u;
		return;
	}
	default:
		return;
	}
}

void lgp::OnDevice( IDirect3DDevice9* dev ){
	PatchVtable( dev, 41, &Hook_BeginScene, o_beginScene );
	PatchVtable( dev, 42, &Hook_EndScene, o_endScene );
	PatchVtable( dev, 57, &Hook_SetRS, o_setRS );
}

void lgp::CountDraw(){
	if( g_traceUi > 0 ) g_traceDraws++;
}

void lgp::OnDeviceLost(){
	gfx::Release( g_tile );
	for( auto& kv : g_images ) gfx::Release( kv.second );
	g_images.clear();
}
