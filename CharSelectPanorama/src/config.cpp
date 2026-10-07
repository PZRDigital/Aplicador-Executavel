#include "config.hpp"

#include <windows.h>
#include <cwchar>
#include <cwctype>
#include <cmath>

#include "log.hpp"
#include "vfs.hpp"
#include <map>

namespace {
	constexpr int MAX_SLOTS = 64;

	config::Settings g_settings;
	std::wstring g_baseDir;
	std::wstring g_iniPath;

	// charselect.ini lido da pasta ou de char.grf: [seção] -> chave -> valor (minúsculas)
	std::map<std::wstring, std::map<std::wstring, std::wstring>> g_ini;
	bool g_iniFound = false;

	std::wstring Lower( std::wstring s ){
		for( wchar_t& c : s ) c = (wchar_t)towlower( c );
		return s;
	}

	void ParseIni(){
		std::vector<unsigned char> data;
		g_iniFound = vfs::Read( g_iniPath, data );
		std::string text( data.begin(), data.end() );
		if( text.size() >= 3 && (unsigned char)text[0] == 0xEF ) text.erase( 0, 3 );
		int n = MultiByteToWideChar( 1252, 0, text.data(), (int)text.size(), nullptr, 0 );
		std::wstring w( n, L'\0' );
		MultiByteToWideChar( 1252, 0, text.data(), (int)text.size(), &w[0], n );
		std::wstring section;
		size_t pos = 0;
		while( pos < w.size() ){
			size_t nl = w.find( L'\n', pos );
			std::wstring line = w.substr( pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos );
			pos = nl == std::wstring::npos ? w.size() : nl + 1;
			while( !line.empty() && ( line.back() == L'\r' || iswspace( line.back() ) ) ) line.pop_back();
			size_t a = 0;
			while( a < line.size() && iswspace( line[a] ) ) a++;
			line = line.substr( a );
			if( line.empty() || line[0] == L';' || line[0] == L'#' ) continue;
			if( line[0] == L'[' ){
				size_t e = line.find( L']' );
				section = Lower( line.substr( 1, e == std::wstring::npos ? std::wstring::npos : e - 1 ) );
				continue;
			}
			size_t eq = line.find( L'=' );
			if( eq == std::wstring::npos ) continue;
			std::wstring k = line.substr( 0, eq ), v = line.substr( eq + 1 );
			while( !k.empty() && iswspace( k.back() ) ) k.pop_back();
			size_t vs = 0;
			while( vs < v.size() && iswspace( v[vs] ) ) vs++;
			g_ini[section][Lower( k )] = v.substr( vs );
		}
	}

	std::wstring ReadString( const wchar_t* section, const wchar_t* key, const wchar_t* def = L"" ){
		auto s = g_ini.find( Lower( section ) );
		if( s != g_ini.end() ){
			auto k = s->second.find( Lower( key ) );
			if( k != s->second.end() ) return k->second;
		}
		return def;
	}

	int ReadInt( const wchar_t* section, const wchar_t* key, int def ){
		std::wstring v = ReadString( section, key );
		return v.empty() ? def : _wtoi( v.c_str() );
	}

	float ReadFloat( const wchar_t* section, const wchar_t* key, float def ){
		std::wstring s = ReadString( section, key );
		return s.empty() ? def : (float)_wtof( s.c_str() );
	}

	std::wstring Trim( const std::wstring& s ){
		size_t a = 0, b = s.size();
		while( a < b && iswspace( s[a] ) ) a++;
		while( b > a && iswspace( s[b - 1] ) ) b--;
		return s.substr( a, b - a );
	}

	config::Point ReadPoint( const wchar_t* section, const wchar_t* key, config::Point def ){
		std::wstring v = ReadString( section, key );
		if( !v.empty() ){
			swscanf( v.c_str(), L"%f,%f", &def.x, &def.y );
		}
		return def;
	}

	int ImageIndex( const std::wstring& file ){
		std::wstring path = g_baseDir + L"scenes\\" + file;

		for( size_t i = 0; i < g_settings.images.size(); i++ ){
			if( _wcsicmp( g_settings.images[i].c_str(), path.c_str() ) == 0 ){
				return (int)i;
			}
		}

		if( !vfs::Exists( path ) ){
			logger::Write( "AVISO: imagem de cena nao encontrada: %ls", path.c_str() );
			return -1;
		}

		g_settings.images.push_back( path );
		return (int)g_settings.images.size() - 1;
	}

	// Formato: arquivo,focoX,focoY,zoom
	bool ParseView( const std::wstring& value, config::View& out ){
		if( value.empty() ){
			return false;
		}

		std::wstring parts[4];
		size_t n = 0, start = 0;
		for( size_t i = 0; i <= value.size() && n < 4; i++ ){
			if( i == value.size() || value[i] == L',' ){
				parts[n++] = Trim( value.substr( start, i - start ) );
				start = i + 1;
			}
		}

		out.image = ImageIndex( parts[0] );
		if( out.image < 0 ){
			return false;
		}
		if( n > 1 && !parts[1].empty() ) out.focusX = (float)_wtof( parts[1].c_str() );
		if( n > 2 && !parts[2].empty() ) out.focusY = (float)_wtof( parts[2].c_str() );
		if( n > 3 && !parts[3].empty() ) out.zoom = (float)_wtof( parts[3].c_str() );
		if( out.zoom < 1.0f ) out.zoom = 1.0f;
		return true;
	}
}

const std::wstring& config::BaseDir(){
	if( g_baseDir.empty() ){
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW( nullptr, exe, MAX_PATH );
		wchar_t* slash = wcsrchr( exe, L'\\' );
		if( slash != nullptr ){
			slash[1] = L'\0';
		}
		g_baseDir = std::wstring( exe ) + L"charselect\\";
	}
	return g_baseDir;
}

void config::Load(){
	BaseDir();
	g_iniPath = g_baseDir + L"charselect.ini";
	ParseIni();

	Settings& s = g_settings;
	s.enabled = ReadInt( L"geral", L"ativo", 1 ) != 0;
	s.debug = ReadInt( L"geral", L"debug", 0 ) != 0;

	std::wstring mode = ReadString( L"geral", L"modo", L"replace" );
	if( _wcsicmp( mode.c_str(), L"under" ) == 0 ) s.mode = BackgroundMode::Under;
	else if( _wcsicmp( mode.c_str(), L"over" ) == 0 ) s.mode = BackgroundMode::Over;
	else s.mode = BackgroundMode::Replace;

	s.fullscreenThreshold = ReadFloat( L"geral", L"limite_tela_cheia", 0.95f );
	std::wstring tex = ReadString( L"geral", L"textura_fundo" );
	if( !tex.empty() ){
		swscanf( tex.c_str(), L"%ux%u", &s.bgTextureWidth, &s.bgTextureHeight );
	}

	s.transitionMs = ReadInt( L"camera", L"transicao_ms", 900 );
	s.crossfadeMs = ReadInt( L"camera", L"esmaecer_ms", 500 );
	s.pushIn = ReadFloat( L"camera", L"aproximacao", 0.06f );
	s.sway = ReadFloat( L"camera", L"balanco", 0.004f );
	s.pullBack = ReadFloat( L"camera", L"afastamento", 0.0f );
	s.smoothness = ReadFloat( L"camera", L"suavidade", 0.11f );

	s.uiEnabled = ReadInt( L"interface", L"ativa", 1 ) != 0;
	s.codepage = ReadInt( L"interface", L"codigo_pagina", 1252 );
	s.charScale = ReadFloat( L"interface", L"escala_personagem", 1.8f );
	s.charX = ReadFloat( L"interface", L"personagem_x", 0.5f );
	s.charY = ReadFloat( L"interface", L"personagem_y", 0.72f );
	s.pixelated = ReadInt( L"interface", L"pixelado", 1 ) != 0;
	s.spriteSmooth = ReadFloat( L"interface", L"suavizar_sprites", 0.3f );
	s.spriteFootY = ReadFloat( L"interface", L"pe_no_cartao", 146.0f );
	s.dimOthers = ReadFloat( L"interface", L"brilho_outros", 0.72f );
	s.thumbScale = ReadFloat( L"interface", L"escala_miniatura", 0.9f );
	s.selectedAction = ReadInt( L"interface", L"acao_selecionado", 32 );
	s.actionFrameMs = ReadInt( L"interface", L"quadro_ms", 110 );
	s.fadeMs = ReadInt( L"jogo", L"duracao_fade_ms", 80 );
	if( s.fadeMs < 0 ) s.fadeMs = 0;
	s.serverChoiceMs = ReadInt( L"interface", L"tempo_escolha_servidor_s", 120 ) * 1000;
	s.attackAction = ReadInt( L"interface", L"acao_golpe", 40 );
	s.attackEveryMs = ReadInt( L"interface", L"intervalo_golpe_ms", 0 );
	s.frameX = ReadFloat( L"camera", L"enquadramento_x", -0.12f );
	s.frameY = ReadFloat( L"camera", L"enquadramento_y", 0.16f );

	// Posições padrão: três fileiras de cinco, levemente intercaladas
	s.charPositions.assign( MAX_SLOTS, Point() );
	for( int i = 0; i < MAX_SLOTS; i++ ){
		int col = i % 5, row = ( i / 5 ) % 3;
		s.charPositions[i].x = 0.30f + col * 0.10f + ( row == 1 ? 0.05f : 0.0f );
		s.charPositions[i].y = 0.50f + row * 0.11f;
		wchar_t key[16];
		swprintf( key, _countof( key ), L"%d", i );
		s.charPositions[i] = ReadPoint( L"posicoes", key, s.charPositions[i] );
	}

	std::wstring nsize = ReadString( L"nativo", L"janela" );
	if( !nsize.empty() ){
		swscanf( nsize.c_str(), L"%ux%u", &s.nativeWidth, &s.nativeHeight );
	}
	std::wstring msize = ReadString( L"nativo", L"fundo_criacao" );
	if( !msize.empty() ){
		swscanf( msize.c_str(), L"%ux%u", &s.makeBgWidth, &s.makeBgHeight );
	}
	std::wstring hide = ReadString( L"foto", L"esconder", L"E52A6248,E61AE9F6,E529E8F2,E529F10F,E4FE770D" );
	for( size_t pos = 0; pos < hide.size(); ){
		size_t comma = hide.find( L',', pos );
		std::wstring item = Trim( hide.substr( pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos ) );
		if( !item.empty() ){
			s.photoHide.push_back( (unsigned)wcstoul( item.c_str(), nullptr, 16 ) );
		}
		if( comma == std::wstring::npos ) break;
		pos = comma + 1;
	}
	s.nativePlay = ReadPoint( L"nativo", L"jogar", s.nativePlay );
	s.nativeDelete = ReadPoint( L"nativo", L"apagar", s.nativeDelete );
	s.nativeDeleteFinal = ReadPoint( L"nativo", L"apagar_definitivo", s.nativeDeleteFinal );
	s.nativePrevPage = ReadPoint( L"nativo", L"pagina_anterior", s.nativePrevPage );
	s.nativeNextPage = ReadPoint( L"nativo", L"pagina_proxima", s.nativeNextPage );
	s.nativeClose = ReadPoint( L"nativo", L"fechar", s.nativeClose );

	s.map3d = ReadInt( L"mapa3d", L"ativo", 0 ) != 0;
	std::wstring dir3d = ReadString( L"mapa3d", L"pasta", L"mapa3d\\2@exds" );
	s.map3dDir = ( dir3d.size() > 1 && dir3d[1] == L':' ) ? dir3d : g_baseDir + dir3d;
	s.camYaw = ReadFloat( L"mapa3d", L"direcao", s.camYaw );
	s.camPitch = ReadFloat( L"mapa3d", L"inclinacao", s.camPitch );
	s.camDist = ReadFloat( L"mapa3d", L"distancia", s.camDist );
	s.camFov = ReadFloat( L"mapa3d", L"campo_visao", s.camFov );
	s.camHeight = ReadFloat( L"mapa3d", L"altura_alvo", s.camHeight );
	s.camOrbit = ReadFloat( L"mapa3d", L"orbita", s.camOrbit );
	s.camArc = ReadFloat( L"mapa3d", L"arco", s.camArc );
	s.camSwing = ReadFloat( L"mapa3d", L"giro", s.camSwing );
	s.camIdle = ReadFloat( L"mapa3d", L"movimento_ocioso", s.camIdle );
	s.unitsPerPixel = ReadFloat( L"mapa3d", L"unidades_por_pixel", s.unitsPerPixel );
	s.skyTop = 0xFF000000 | wcstoul( ReadString( L"mapa3d", L"ceu_topo", L"4F7FB8" ).c_str(), nullptr, 16 );
	s.skyHorizon = 0xFF000000 | wcstoul( ReadString( L"mapa3d", L"ceu_horizonte", L"B9D3EA" ).c_str(), nullptr, 16 );
	{
		config::Point fog = ReadPoint( L"mapa3d", L"nevoa", { s.fogStart, s.fogEnd } );
		s.fogStart = fog.x;
		s.fogEnd = fog.y;
	}
	s.water = ReadInt( L"mapa3d", L"agua", 0 ) != 0;
	s.clouds = ReadInt( L"mapa3d", L"nuvens", s.clouds );
	s.cloudLevel = ReadFloat( L"mapa3d", L"altura_nuvens", s.cloudLevel );
	{
		// RRGGBB ou AARRGGBB
		std::wstring c = ReadString( L"mapa3d", L"cor_nuvens", L"FFFFFF" );
		unsigned v = wcstoul( c.c_str(), nullptr, 16 );
		s.cloudColor = c.size() > 6 ? v : ( 0xFF000000 | v );
	}
	s.stars = ReadInt( L"mapa3d", L"estrelas", s.stars );
	s.particles = ReadFloat( L"mapa3d", L"particulas", s.particles );
	s.glow = ReadFloat( L"mapa3d", L"halo", s.glow );
	s.supersample = ReadFloat( L"mapa3d", L"superamostragem", s.supersample );
	s.charCells.assign( MAX_SLOTS, Point{ -1.0f, -1.0f } );
	s.charYaw.assign( MAX_SLOTS, NAN );
	for( int i = 0; i < MAX_SLOTS; i++ ){
		wchar_t key[16];
		swprintf( key, _countof( key ), L"%d", i );
		std::wstring v = ReadString( L"posicoes3d", key );
		float x, y, yaw;
		int n = v.empty() ? 0 : swscanf( v.c_str(), L"%f,%f,%f", &x, &y, &yaw );
		if( n >= 2 ) s.charCells[i] = Point{ x, y };
		if( n >= 3 ) s.charYaw[i] = yaw;
	}

	if( !ParseView( ReadString( L"cenas", L"padrao" ), s.defaultView ) ){
		logger::Write( "AVISO: [cenas] padrao ausente ou invalido" );
	}

	s.slotViews.assign( MAX_SLOTS, View() );
	for( int i = 0; i < MAX_SLOTS; i++ ){
		wchar_t key[16];
		swprintf( key, _countof( key ), L"%d", i );
		ParseView( ReadString( L"cenas", key ), s.slotViews[i] );
	}

	if( !g_iniFound ){
		logger::Write( "AVISO: %ls nao encontrado; usando padroes (sem cenas)", g_iniPath.c_str() );
	}
	if( s.images.empty() ){
		logger::Write( "AVISO: nenhuma imagem de cena valida; o fundo nao sera alterado" );
	}

	logger::Write( "Alteracoes ativas: login=%d selecao=%d criacao=%d chave=%d combate=%d fade=%d status=%d item=%d equip=%d inventario=%d habilidades=%d",
		Feature( FEAT_LOGIN ), Feature( FEAT_SELECT ), Feature( FEAT_MAKE ), Feature( FEAT_TOGGLE ), Feature( FEAT_COMBAT ), Feature( FEAT_FADE ),
		Feature( FEAT_HUD ), Feature( FEAT_ITEMWND ), Feature( FEAT_EQUIPWND ), Feature( FEAT_INVWND ), Feature( FEAT_SKILLWND ) );
	logger::Write( "Config: ativo=%d debug=%d modo=%ls imagens=%u mapa3d=%d (%ls)", s.enabled, s.debug, mode.c_str(),
		(unsigned)s.images.size(), s.map3d, s.map3dDir.c_str() );
}

const config::Settings& config::Get(){
	return g_settings;
}

config::View config::ViewForSlot( int slot ){
	if( slot >= 0 && slot < (int)g_settings.slotViews.size() && g_settings.slotViews[slot].image >= 0 ){
		return g_settings.slotViews[slot];
	}
	// Sem cena própria: a câmera da imagem padrão segue o personagem do slot
	View v = g_settings.defaultView;
	if( slot >= 0 && slot < (int)g_settings.charPositions.size() ){
		v.focusX = g_settings.charPositions[slot].x;
		v.focusY = g_settings.charPositions[slot].y;
		v.follow = true;
	}
	return v;
}

namespace {
	volatile int g_custom = -1; // -1 = ainda não lido

	// Marcador procurado e preenchido pelo Aplicador Codex (não mudar o formato)
#pragma pack( push, 1 )
	struct CodexFeatures {
		char magic[12];
		unsigned flags;
	};
#pragma pack( pop )
	volatile CodexFeatures g_features = { { 'C', 'O', 'D', 'E', 'X', '_', 'F', 'E', 'A', 'T', '0', '1' }, 0xFFFFFFFFu };
}

bool config::Feature( unsigned f ){
	unsigned flags = g_features.flags;
	if( ( f & ( FEAT_MAKE | FEAT_TOGGLE | FEAT_COMBAT ) ) && !( flags & FEAT_SELECT ) ) return false; // dependem da seleção
	return ( flags & f ) == f;
}

bool config::CustomActive(){
	if( !Feature( FEAT_TOGGLE ) ){
		return g_settings.uiEnabled; // sem a chave para o jogador: sempre a personalizada
	}
	if( g_custom < 0 ){
		std::wstring prefs = vfs::WritableDir() + L"preferencias.ini";
		wchar_t buf[32];
		GetPrivateProfileStringW( L"preferencias", L"selecao", L"personalizada", buf, _countof( buf ), prefs.c_str() );
		g_custom = _wcsicmp( buf, L"original" ) == 0 ? 0 : 1;
	}
	return g_custom == 1 && g_settings.uiEnabled;
}

void config::SetCustomActive( bool on ){
	g_custom = on ? 1 : 0;
	std::wstring prefs = vfs::WritableDir() + L"preferencias.ini";
	WritePrivateProfileStringW( L"preferencias", L"selecao", on ? L"personalizada" : L"original", prefs.c_str() );
	logger::Write( "Selecao de personagem: %s", on ? "personalizada" : "original" );
}
