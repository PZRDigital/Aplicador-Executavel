#include "ui.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <string>
#include <map>
#include <vector>

#include "chardata.hpp"
#include "client.hpp"
#include "d3d_hooks.hpp"
#include "config.hpp"
#include "gfx.hpp"
#include "log.hpp"
#include "vfs.hpp"
#include "scene.hpp"
#include "game.hpp"
#include "itemdb.hpp"

namespace {
	using gfx::Font;
	using gfx::Rgba;

	// Paleta do site (assets/css/style.css)
	const D3DCOLOR PANEL = Rgba( 8, 16, 28, 0.82f );
	const D3DCOLOR PANEL_DEEP = Rgba( 5, 8, 14, 0.92f );
	const D3DCOLOR BORDER = Rgba( 102, 190, 255, 0.22f );
	const D3DCOLOR GOLD = Rgba( 240, 189, 69, 1.0f );
	const D3DCOLOR GOLD_HOVER = Rgba( 255, 225, 140, 1.0f );
	const D3DCOLOR GOLD_DEEP = Rgba( 184, 134, 11, 1.0f );
	const D3DCOLOR BLUE = Rgba( 18, 191, 255, 1.0f );
	const D3DCOLOR BLUE_SOFT = Rgba( 124, 227, 255, 1.0f );
	const D3DCOLOR TEXT = Rgba( 244, 247, 251, 1.0f );
	const D3DCOLOR MUTED = Rgba( 159, 178, 196, 1.0f );
	const D3DCOLOR TRACK = Rgba( 0, 0, 0, 0.55f );
	const D3DCOLOR DARK_TEXT = Rgba( 18, 12, 0, 1.0f );

	constexpr int MAX_CARDS = 64;
	constexpr float SPRITE_HEIGHT = 118.0f; // altura aproximada do sprite no cartão nativo

	enum class Action { None, Card, Native, Local };

	// Ações tratadas pela própria interface (não chegam ao cliente)
	enum LocalAction { OPEN_CONFIRM_RESERVE = 1, OPEN_CONFIRM_FINAL, CLOSE_MODAL, OPEN_EXIT, EXIT_GAME, FAVORITE_BASE = 1000 };
	enum class Modal { None, ConfirmReserve, ConfirmFinal, ConfirmExit };

	struct Hit {
		float x, y, w, h;
		Action action;
		int card;            // Action::Card
		config::Point point; // Action::Native (relativo à janela nativa)
	};

	// Onde cada cartão nativo aparece na interface nova
	struct CardView {
		bool hasChar = false;
		bool inScene = false;
		float feetX = 0, feetY = 0, scale = 1;
		bool hasThumb = false;
		float thumbX = 0, thumbY = 0, thumbSize = 0;
	};

	// Topo dos sprites de cada cartão: medido no frame atual, usado no seguinte
	float g_spriteTop[MAX_CARDS], g_spriteTopPrev[MAX_CARDS];

	ui::NativeFrame g_frame;
	std::vector<Hit> g_hits;
	CardView g_cards[MAX_CARDS];
	bool g_hasLayout = false;
	float g_s = 1.0f; // escala da interface
	RECT g_sceneClip = {};
	RECT g_infoRect = {}; // ficha do personagem (placas embaixo dela não são desenhadas)

	Modal g_modal = Modal::None;

	// Placas de nome: desenhadas depois dos personagens (ficam por cima dos sprites)
	struct Plate {
		float px, py, pw, ph, lw;
		D3DCOLOR bg, frame, lvBg, lvText, nameText;
		std::wstring lv, name;
		bool balloon = false;   // selecionado: balão com selo da classe
		int job = 0;
		std::wstring jobName;
		float tipX = 0;         // ponta do balão (sobre o personagem)
	};
	std::vector<Plate> g_plates;
	bool g_prevDown = false;

	gfx::Image g_logo, g_glow, g_gear, g_disc;
	gfx::Image g_bars[4]; // HP cheia, HP vazia, SP cheia, SP vazia (img\barra_*.png)
	bool g_barsTried = false;

	// Interface do Ragnarok Zero (UIRenewal): imagens em img\zero (tools\zero_ui.py).
	// Posições e regras tiradas do cliente Zero (UIBasicInfoWnd no tema novo, UIHpSpGage e UIExpBar).
	struct ZeroImages {
		gfx::Image gaugeBg, hpNormal, hpDamage, hpDamageDeco, hpDeco, sp;
		gfx::Image expBaseBg, expJobBg, expBase, expJob;
		gfx::Image fieldName;
		bool tried = false;
	} g_zero;
	std::map<int, gfx::Image> g_zeroJobs; // img\zero\job\icon_job_<classe>.png
	std::map<std::wstring, gfx::Image> g_zeroMenu; // img\zero\menu\<nome>.png (carregadas sob demanda)

	// Ícones de classe: os do Ragnarok Zero (img\zero\job) ou, sem eles, charselect\icones\<classe>.png (tools\icones_codex.py)
	std::map<int, gfx::Image> g_icons;
	IDirect3DDevice9* g_iconDev = nullptr;

	const gfx::Image* ClassIcon( int job ){
		auto it = g_icons.find( job );
		if( it != g_icons.end() ){
			return it->second.tex ? &it->second : nullptr;
		}
		gfx::Image& img = g_icons[job];
		// ícone do Ragnarok Zero (img\zero\job); classes que o Zero não tem usam o do Codex
		std::wstring zero = config::BaseDir() + L"img\\zero\\job\\icon_job_" + std::to_wstring( job ) + L".png";
		std::wstring path = vfs::Exists( zero ) ? zero : config::BaseDir() + L"icones\\" + std::to_wstring( job ) + L".png";
		if( g_iconDev && vfs::Exists( path ) ){
			gfx::LoadImageFile( g_iconDev, path, img );
		}
		return img.tex ? &img : nullptr;
	}

	// Ícone da classe (do cliente, recolorido no tema por tools\icones_codex.py), centrado em cx, cy
	// ring dourado = destaque (selecionado/sob o mouse); fill/glyph mantidos por compatibilidade
	void ClassBadge( float cx, float cy, float d, int job, D3DCOLOR ring, D3DCOLOR fill, D3DCOLOR glyph ){
		(void)fill; (void)glyph;
		const gfx::Image* icon = ClassIcon( job );
		if( icon == nullptr ) icon = ClassIcon( 0 );
		bool highlight = ring == GOLD;
		float s = d * 0.9f;
		if( highlight && g_glow.tex ){
			gfx::DrawImage( g_glow, cx - s * 0.9f, cy - s * 0.75f, s * 1.8f, s * 1.5f, Rgba( 240, 189, 69, 0.35f ) );
		}
		if( icon ){
			gfx::DrawImage( *icon, cx - s * 0.5f, cy - s * 0.5f, s, s );
		}
	}
	bool g_assetsTried = false;
	unsigned g_uiSeq = 0; // ordem de desenho das janelas do jogo (maior = por cima)
	struct BigRect { float x, y, w, h; DWORD tick; };
	BigRect g_bigRects[3] = {}; // equipamentos, inventário, habilidades (último quadro)
	void NoteBig( int k, float x, float y, float w, float h ){ g_bigRects[k] = { x, y, w, h, GetTickCount() }; }
	struct { float x, y, w, h; } g_hudRect = {};
	DWORD g_hudTick = 0;

	float S( float v ){
		return v * g_s;
	}

	float Seconds(){
		return GetTickCount() / 1000.0f;
	}

	bool Inside( const input::Point& p, float x, float y, float w, float h ){
		return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
	}

	bool Hovered( float x, float y, float w, float h ){
		return !g_frame.popupActive && Inside( input::RealCursor(), x, y, w, h );
	}

	void AddHit( float x, float y, float w, float h, Action a, int card = -1, config::Point pt = {} ){
		g_hits.push_back( { x, y, w, h, a, card, pt } );
	}

	std::wstring Thousands( long long v ){
		std::wstring digits = std::to_wstring( v < 0 ? -v : v );
		std::wstring out;
		int count = 0;
		for( auto it = digits.rbegin(); it != digits.rend(); ++it ){
			if( count > 0 && count % 3 == 0 ) out.insert( out.begin(), L'.' );
			out.insert( out.begin(), *it );
			count++;
		}
		return v < 0 ? L"-" + out : out;
	}

	void AccentLine( float x, float y, float w ){
		float q = w / 4.0f;
		gfx::RectH( x, y, q, S( 2 ), Rgba( 18, 191, 255, 0.0f ), BLUE );
		gfx::RectH( x + q, y, q, S( 2 ), BLUE, GOLD );
		gfx::RectH( x + 2 * q, y, q, S( 2 ), GOLD, BLUE );
		gfx::RectH( x + 3 * q, y, q, S( 2 ), BLUE, Rgba( 18, 191, 255, 0.0f ) );
	}

	void Panel( float x, float y, float w, float h ){
		gfx::Rect( x + S( 4 ), y + S( 6 ), w, h, Rgba( 0, 0, 0, 0.35f ) );
		gfx::Rect( x, y, w, h, PANEL );
		gfx::Frame( x, y, w, h, BORDER );
		AccentLine( x + w * 0.1f, y, w * 0.8f );
	}

	void SectionTitle( const wchar_t* text, float x, float y, float w ){
		gfx::Text( Font::SmallBold, text, x, y, MUTED );
		float tw = gfx::TextWidth( Font::SmallBold, text );
		float ly = y + gfx::LineHeight( Font::SmallBold ) * 0.5f;
		if( w > tw + S( 8 ) ){
			gfx::RectH( x + tw + S( 8 ), ly, w - tw - S( 8 ), 1.0f, BORDER, Rgba( 102, 190, 255, 0.0f ) );
		}
	}

	void Bar( float x, float y, float w, float h, float ratio, D3DCOLOR a, D3DCOLOR b ){
		ratio = std::min( 1.0f, std::max( 0.0f, ratio ) );
		gfx::Rect( x, y, w, h, TRACK );
		if( ratio > 0.0f ){
			gfx::RectH( x, y, w * ratio, h, a, b );
			gfx::Rect( x, y, w * ratio, 1.0f, Rgba( 255, 255, 255, 0.25f ) );
		}
	}

	void GaugeRow( const wchar_t* label, long long cur, long long max, float x, float y, float w, D3DCOLOR a, D3DCOLOR b ){
		gfx::Text( Font::SmallBold, label, x, y, TEXT );
		gfx::Text( Font::Small, Thousands( cur ) + L" / " + Thousands( max ), x + w, y, MUTED, gfx::Right );
		Bar( x, y + S( 15 ), w, S( 6 ), max > 0 ? (float)cur / (float)max : 0.0f, a, b );
	}

	bool Anchor( int card, float& ax, float& ay ){
		client::WndRect r;
		if( !client::CardRect( card, r ) ){
			return false;
		}
		ax = g_frame.originX + r.x + r.w * 0.5f;
		ay = g_frame.originY + r.y + config::Get().spriteFootY;
		return true;
	}

	// Altura real do personagem no cartão nativo (pés até o topo da cabeça/chapéu)
	float SpriteHeight( int card ){
		float ax, ay;
		if( card >= 0 && card < MAX_CARDS && g_spriteTopPrev[card] < 1e8f && Anchor( card, ax, ay ) ){
			float h = ay - g_spriteTopPrev[card];
			if( h > 20.0f && h < 200.0f ){
				return h;
			}
		}
		return SPRITE_HEIGHT;
	}

	int PageSlot( int card ){
		int perPage = std::max( 1, client::SlotsPerPage() );
		int index = client::IndexInPage();
		int page = index >= 0 ? ( client::SelectedSlot() - index ) / perPage : 0;
		return page * perPage + card;
	}

	// Sem dados do servidor (hook indisponível) mostra todos os cartões do cliente
	bool HasCharacter( int card ){
		return chardata::Count() == 0 || chardata::Get( PageSlot( card ) ) != nullptr;
	}

	void TopBar(){
		float h = S( 34 );
		gfx::Rect( 0, 0, g_frame.vpWidth, h, PANEL_DEEP );
		AccentLine( 0, h - S( 2 ), g_frame.vpWidth );

		float x = S( 16 );
		if( g_logo.tex != nullptr ){
			float lh = S( 26 ), lw = lh * g_logo.width / (float)g_logo.height;
			gfx::DrawImage( g_logo, x, ( h - lh ) * 0.5f, lw, lh );
			x += lw + S( 8 );
		}
		x += gfx::Text( Font::TitleSmall, L"O CODEX", x, ( h - gfx::LineHeight( Font::TitleSmall ) ) * 0.5f, GOLD );
		gfx::Text( Font::Small, L"   ·   ALOJAMENTO", x, ( h - gfx::LineHeight( Font::Small ) ) * 0.5f, MUTED );

		float bw = S( 64 ), bh = S( 22 ), bx = g_frame.vpWidth - S( 16 ) - bw, by = ( h - bh ) * 0.5f;
		bool hover = Hovered( bx, by, bw, bh );
		gfx::Rect( bx, by, bw, bh, hover ? Rgba( 255, 80, 80, 0.25f ) : Rgba( 0, 0, 0, 0.5f ) );
		gfx::Frame( bx, by, bw, bh, hover ? Rgba( 255, 120, 120, 0.8f ) : BORDER );
		gfx::Text( Font::SmallBold, L"SAIR", bx + bw * 0.5f, by + ( bh - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, hover ? TEXT : MUTED, gfx::Center );
		AddHit( bx, by, bw, bh, Action::Local, OPEN_EXIT );

		std::wstring count = std::to_wstring( chardata::Count() ) + L" / " + std::to_wstring( client::SlotsPerPage() ) + L" PERSONAGENS";
		float cw = gfx::TextWidth( Font::SmallBold, count ) + S( 24 ), cx = bx - S( 12 ) - cw;
		gfx::Rect( cx, by, cw, bh, Rgba( 0, 0, 0, 0.6f ) );
		gfx::Frame( cx, by, cw, bh, Rgba( 240, 189, 69, 0.35f ) );
		gfx::Text( Font::SmallBold, count, cx + cw * 0.5f, by + ( bh - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, GOLD, gfx::Center );
	}

	// Ficha compacta do personagem selecionado (canto superior esquerdo)
	// ---------- Favoritos (charselect\favoritos.txt, um GID por linha) ----------
	std::vector<uint32_t> g_favorites;
	bool g_favoritesLoaded = false;

	void LoadFavorites(){
		g_favoritesLoaded = true;
		FILE* f = _wfopen( ( vfs::WritableDir() + L"favoritos.txt" ).c_str(), L"r" );
		if( f == nullptr ) return;
		unsigned gid;
		while( fscanf( f, "%u", &gid ) == 1 ) g_favorites.push_back( gid );
		fclose( f );
	}

	void SaveFavorites(){
		FILE* f = _wfopen( ( vfs::WritableDir() + L"favoritos.txt" ).c_str(), L"w" );
		if( f == nullptr ) return;
		for( uint32_t gid : g_favorites ) fprintf( f, "%u\n", gid );
		fclose( f );
	}

	bool IsFavorite( uint32_t gid ){
		return std::find( g_favorites.begin(), g_favorites.end(), gid ) != g_favorites.end();
	}

	void ToggleFavorite( uint32_t gid ){
		auto it = std::find( g_favorites.begin(), g_favorites.end(), gid );
		if( it != g_favorites.end() ) g_favorites.erase( it );
		else g_favorites.push_back( gid );
		SaveFavorites();
	}

	void Star( float cx, float cy, float r, D3DCOLOR color ){
		float px[10], py[10];
		for( int i = 0; i < 10; i++ ){
			float a = -1.5707963f + i * 3.14159265f / 5.0f;
			float rr = ( i % 2 == 0 ) ? r : r * 0.45f;
			px[i] = cx + cosf( a ) * rr;
			py[i] = cy + sinf( a ) * rr;
		}
		for( int i = 0; i < 10; i++ ){
			gfx::Triangle( cx, cy, px[i], py[i], px[( i + 1 ) % 10], py[( i + 1 ) % 10], color );
		}
	}

	// ---------- Ficha do personagem (atributos em hexágono, no estilo da referência) ----------
	float InfoHeight( const chardata::Character* c ){
		if( c == nullptr ) return S( 110 );
		return S( 492 );
	}

	void InfoCard( const chardata::Character* c, float x, float y, float w ){
		float px = x + S( 18 ), pw = w - S( 36 );
		float h = InfoHeight( c );
		Panel( x, y, w, h );
		float cy = y + S( 16 );

		if( c == nullptr ){
			gfx::Text( Font::Title, L"Slot vazio", px, cy, MUTED );
			gfx::Text( Font::Body, L"Crie um personagem para este slot.", px, cy + S( 40 ), MUTED, gfx::Left, pw );
			return;
		}

		gfx::Text( Font::Title, c->name, px, cy, GOLD, gfx::Left, pw );
		cy += S( 34 );
		std::wstring sub = chardata::JobName( c->job, c->sex );
		if( !c->map.empty() ) sub += L"  ·  " + chardata::MapName( c->map );
		gfx::Text( Font::Body, sub, px, cy, MUTED, gfx::Left, pw );
		cy += S( 26 );

		// Níveis
		float bw = ( pw - S( 10 ) ) * 0.5f, bh = S( 28 );
		const wchar_t* lvLabels[2] = { L"BASE", L"CLASSE" };
		int lvValues[2] = { c->baseLevel, c->jobLevel };
		for( int i = 0; i < 2; i++ ){
			float bx = px + i * ( bw + S( 10 ) );
			gfx::Rect( bx, cy, bw, bh, Rgba( 0, 0, 0, 0.45f ) );
			gfx::Rect( bx, cy, S( 3 ), bh, GOLD );
			gfx::Text( Font::SmallBold, lvLabels[i], bx + S( 12 ), cy + ( bh - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, MUTED );
			gfx::Text( Font::BodyBold, L"Nv " + std::to_wstring( lvValues[i] ), bx + bw - S( 10 ), cy + ( bh - gfx::LineHeight( Font::BodyBold ) ) * 0.5f, GOLD, gfx::Right );
		}
		cy += bh + S( 16 );

		// Cabeçalho ATRIBUTOS · máx.
		int values[6] = { c->str, c->agi, c->vit, c->int_, c->dex, c->luk };
		int top = std::max( 99, *std::max_element( values, values + 6 ) );
		gfx::Rect( x, cy - S( 2 ), S( 3 ), S( 18 ), GOLD );
		gfx::Text( Font::SmallBold, L"ATRIBUTOS", px, cy, GOLD );
		gfx::Text( Font::Small, L"máx. " + std::to_wstring( top ), px + pw, cy, MUTED, gfx::Right );
		cy += S( 22 );

		// Hexágono: FOR no topo e, no sentido horário, AGI, VIT, INT, DES, SOR
		const wchar_t* labels[6] = { L"FOR", L"AGI", L"VIT", L"INT", L"DES", L"SOR" };
		float R = S( 74 );
		float ccx = x + w * 0.5f, ccy = cy + S( 44 ) + R;
		float ax[6], ay[6];
		for( int i = 0; i < 6; i++ ){
			float ang = -1.5707963f + i * 1.0471976f;
			ax[i] = cosf( ang );
			ay[i] = sinf( ang );
		}
		// grade: anéis e raios
		for( int ring = 1; ring <= 4; ring++ ){
			float rr = R * ring / 4.0f;
			for( int i = 0; i < 6; i++ ){
				int j = ( i + 1 ) % 6;
				gfx::Line( ccx + ax[i] * rr, ccy + ay[i] * rr, ccx + ax[j] * rr, ccy + ay[j] * rr, Rgba( 255, 255, 255, ring == 4 ? 0.25f : 0.10f ) );
			}
		}
		for( int i = 0; i < 6; i++ ){
			gfx::Line( ccx, ccy, ccx + ax[i] * R, ccy + ay[i] * R, Rgba( 255, 255, 255, 0.10f ) );
		}
		// polígono dos valores
		float vx[6], vy[6];
		for( int i = 0; i < 6; i++ ){
			float t = std::max( 0.04f, std::min( 1.0f, values[i] / (float)top ) );
			vx[i] = ccx + ax[i] * R * t;
			vy[i] = ccy + ay[i] * R * t;
		}
		for( int i = 0; i < 6; i++ ){
			int j = ( i + 1 ) % 6;
			gfx::Triangle( ccx, ccy, vx[i], vy[i], vx[j], vy[j], Rgba( 240, 189, 69, 0.40f ) );
		}
		for( int i = 0; i < 6; i++ ){
			int j = ( i + 1 ) % 6;
			gfx::Line( vx[i], vy[i], vx[j], vy[j], GOLD, S( 1.6f ) );
		}
		for( int i = 0; i < 6; i++ ){
			gfx::Circle( vx[i], vy[i], S( 3.2f ), GOLD_HOVER );
		}
		// rótulos e valores reais
		for( int i = 0; i < 6; i++ ){
			float lx = ccx + ax[i] * ( R + S( 34 ) ), ly = ccy + ay[i] * ( R + S( 26 ) );
			float lh = gfx::LineHeight( Font::SmallBold );
			gfx::Text( Font::SmallBold, labels[i], lx, ly - lh, MUTED, gfx::Center );
			gfx::Text( Font::BodyBold, Thousands( values[i] ), lx, ly, TEXT, gfx::Center );
		}
		cy = ccy + R + S( 64 );

		// HP / SP
		gfx::Text( Font::SmallBold, L"HP", px, cy, TEXT );
		gfx::Text( Font::Small, Thousands( c->hp ) + L" / " + Thousands( c->maxHp ), px + pw, cy, MUTED, gfx::Right );
		Bar( px, cy + S( 15 ), pw, S( 7 ), c->maxHp > 0 ? (float)c->hp / (float)c->maxHp : 0.0f, Rgba( 40, 160, 80, 1 ), Rgba( 90, 220, 120, 1 ) );
		cy += S( 30 );
		gfx::Text( Font::SmallBold, L"SP", px, cy, TEXT );
		gfx::Text( Font::Small, Thousands( c->sp ) + L" / " + Thousands( c->maxSp ), px + pw, cy, MUTED, gfx::Right );
		Bar( px, cy + S( 15 ), pw, S( 7 ), c->maxSp > 0 ? (float)c->sp / (float)c->maxSp : 0.0f, Rgba( 10, 111, 216, 1 ), BLUE );
		cy += S( 32 );

		gfx::Text( Font::SmallBold, L"ZENY", px, cy, MUTED );
		gfx::Text( Font::SmallBold, Thousands( c->zeny ), px + pw, cy, GOLD, gfx::Right );

		if( c->deleteDate != 0 ){
			time_t when = (time_t)c->deleteDate;
			tm lt = {};
			localtime_s( &lt, &when );
			wchar_t buf[96];
			if( when <= time( nullptr ) ){
				swprintf( buf, 96, L"EXCLUSÃO AGENDADA · já pode ser apagado" );
			}else{
				swprintf( buf, 96, L"EXCLUSÃO AGENDADA · liberada em %02d/%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min );
			}
			gfx::Rect( x, y + h + S( 6 ), w, S( 26 ), Rgba( 120, 20, 30, 0.85f ) );
			gfx::Frame( x, y + h + S( 6 ), w, S( 26 ), Rgba( 255, 110, 110, 0.8f ) );
			gfx::Text( Font::SmallBold, buf, x + w * 0.5f, y + h + S( 6 ) + ( S( 26 ) - gfx::LineHeight( Font::SmallBold ) ) * 0.5f,
				Rgba( 255, 200, 200, 1 ), gfx::Center );
		}
	}

	// ---------- Lista de personagens (favoritos no topo, com rolagem) ----------
	float g_listScroll = 0.0f;

	void SlotList( int page, int perPage, int count, int selected, float x, float y, float w, float h ){
		Panel( x, y, w, h );
		float px = x + S( 12 ), pw = w - S( 24 ), cy = y + S( 16 );
		SectionTitle( L"PERSONAGENS", px, cy, pw - S( 110 ) );

		// Contador e páginas (botões nativos)
		int chars = 0;
		for( int i = 0; i < count; i++ ) if( chardata::Get( page * perPage + i ) ) chars++;
		float aw = S( 20 ), ah = S( 18 ), ay = cy - S( 2 );
		float nx = px + pw - aw, bx = nx - aw - S( 4 );
		for( int dir = 0; dir < 2; dir++ ){
			float axp = dir == 0 ? bx : nx;
			bool hover = Hovered( axp, ay, aw, ah );
			gfx::Rect( axp, ay, aw, ah, hover ? Rgba( 18, 191, 255, 0.25f ) : Rgba( 0, 0, 0, 0.5f ) );
			gfx::Frame( axp, ay, aw, ah, BORDER );
			gfx::Text( Font::SmallBold, dir == 0 ? L"‹" : L"›", axp + aw * 0.5f, ay + S( 1 ), BLUE_SOFT, gfx::Center );
			AddHit( axp, ay, aw, ah, Action::Native, -1, dir == 0 ? config::Get().nativePrevPage : config::Get().nativeNextPage );
		}
		gfx::Text( Font::SmallBold, std::to_wstring( chars ) + L" / " + std::to_wstring( perPage ), bx - S( 8 ), cy, MUTED, gfx::Right );
		cy += S( 26 );

		// Ordem: favoritos, demais personagens e todos os slots vazios (numerados como no servidor)
		std::vector<int> rows;
		for( int pass = 0; pass < 2; pass++ ){
			for( int i = 0; i < count && i < MAX_CARDS; i++ ){
				const chardata::Character* c = chardata::Get( page * perPage + i );
				if( c != nullptr && IsFavorite( c->gid ) == ( pass == 0 ) ) rows.push_back( i );
			}
		}
		for( int i = 0; i < count && i < MAX_CARDS; i++ ){
			if( chardata::Get( page * perPage + i ) == nullptr ) rows.push_back( i );
		}

		float rowH = S( 64 ), gap = S( 4 );
		float areaTop = cy, areaBottom = y + h - S( 10 );
		float contentH = rows.size() * ( rowH + gap );
		float maxScroll = std::max( 0.0f, contentH - ( areaBottom - areaTop ) );
		if( Hovered( x, y, w, h ) ){
			g_listScroll -= input::TakeWheel() / 120.0f * ( rowH + gap );
		}
		g_listScroll = std::min( maxScroll, std::max( 0.0f, g_listScroll ) );
		if( maxScroll > 0.0f ){
			// indicador de rolagem
			float track = areaBottom - areaTop, thumb = track * ( track / contentH );
			float ty = areaTop + ( track - thumb ) * ( g_listScroll / maxScroll );
			gfx::Rect( x + w - S( 5 ), areaTop, S( 2 ), track, Rgba( 255, 255, 255, 0.06f ) );
			gfx::Rect( x + w - S( 5 ), ty, S( 2 ), thumb, Rgba( 240, 189, 69, 0.6f ) );
		}

		for( size_t r = 0; r < rows.size(); r++ ){
			int i = rows[r];
			float ry = areaTop + r * ( rowH + gap ) - g_listScroll;
			if( ry < areaTop - 1.0f || ry + rowH > areaBottom + 1.0f ){
				continue; // fora da área visível
			}
			int slot = page * perPage + i;
			const chardata::Character* c = chardata::Get( slot );
			bool isSelected = i == selected;
			bool hover = Hovered( px, ry, pw, rowH );

			D3DCOLOR bg = isSelected ? Rgba( 240, 189, 69, 0.14f ) : ( hover ? Rgba( 18, 191, 255, 0.08f ) : Rgba( 255, 255, 255, 0.035f ) );
			gfx::Rect( px, ry, pw, rowH, bg );
			gfx::Rect( px, ry + rowH - 1, pw, 1, Rgba( 255, 255, 255, 0.06f ) );
			if( isSelected ){
				gfx::Frame( px, ry, pw, rowH, Rgba( 240, 189, 69, 0.85f ) );
				gfx::Rect( px, ry, S( 3 ), rowH, GOLD );
			}else if( hover ){
				gfx::Frame( px, ry, pw, rowH, Rgba( 18, 191, 255, 0.45f ) );
			}

			// retrato (quadro escuro)
			float ts = rowH - S( 12 ), tx = px + S( 8 ), ty = ry + S( 6 );
			gfx::Rect( tx, ty, ts, ts, Rgba( 3, 6, 12, 0.85f ) );
			gfx::Frame( tx, ty, ts, ts, isSelected ? Rgba( 240, 189, 69, 0.7f ) : Rgba( 255, 255, 255, 0.10f ) );

			// número do slot
			float nb = S( 24 ), nxp = px + pw - nb - S( 10 ), nyp = ry + ( rowH - nb ) * 0.5f;
			gfx::Rect( nxp, nyp, nb, nb, isSelected ? GOLD : Rgba( 0, 0, 0, 0.45f ) );
			gfx::Frame( nxp, nyp, nb, nb, isSelected ? GOLD_HOVER : Rgba( 255, 255, 255, 0.16f ) );
			gfx::Text( Font::SmallBold, std::to_wstring( slot + 1 ), nxp + nb * 0.5f, nyp + ( nb - gfx::LineHeight( Font::SmallBold ) ) * 0.5f,
				isSelected ? DARK_TEXT : MUTED, gfx::Center, 0.0f, false );

			float textX = tx + ts + S( 12 );
			if( c == nullptr ){
				// slot vazio
				gfx::Text( Font::Title, L"+", tx + ts * 0.5f, ty + ( ts - gfx::LineHeight( Font::Title ) ) * 0.5f,
					hover ? BLUE_SOFT : Rgba( 159, 178, 196, 0.45f ), gfx::Center, 0.0f, false );
				gfx::Text( Font::BodyBold, L"Slot vazio", textX, ry + S( 14 ), hover ? BLUE_SOFT : Rgba( 159, 178, 196, 0.75f ) );
				gfx::Text( Font::Small, L"Aguardando criação...", textX, ry + S( 34 ), Rgba( 159, 178, 196, 0.5f ) );
				AddHit( px, ry, pw, rowH, Action::Card, i );
				continue;
			}

			// o sprite do cartão nativo é redesenhado no retrato (ThumbXform)
			CardView& cv = g_cards[i];
			cv.hasThumb = true;
			cv.thumbX = tx;
			cv.thumbY = ty;
			cv.thumbSize = ts;

			float textW = nxp - textX - S( 34 );
			float nameY = ry + S( 6 );
			gfx::Text( Font::BodyBold, c->name, textX, nameY, isSelected ? GOLD_HOVER : TEXT, gfx::Left, textW );

			// selo da classe + nome da classe; embaixo BASE / CLASSE
			float badge = S( 36 );
			float lineY = nameY + gfx::LineHeight( Font::BodyBold ) + S( 1 );
			ClassBadge( textX + badge * 0.5f, lineY + badge * 0.5f + S( 1 ), badge, c->job,
				isSelected ? GOLD : Rgba( 159, 178, 196, 0.8f ), Rgba( 10, 18, 32, 1.0f ), isSelected ? GOLD_HOVER : TEXT );
			float cx2 = textX + badge + S( 8 );
			gfx::Text( Font::Small, chardata::JobName( c->job, c->sex ), cx2, lineY, MUTED, gfx::Left, textW - badge - S( 8 ) );
			float ly = lineY + gfx::LineHeight( Font::Small ) + S( 1 );
			float lx = cx2;
			lx += gfx::Text( Font::Small, L"BASE ", lx, ly, Rgba( 159, 178, 196, 0.6f ), gfx::Left, 0.0f, false );
			lx += gfx::Text( Font::SmallBold, std::to_wstring( c->baseLevel ), lx, ly, isSelected ? GOLD_HOVER : TEXT ) + S( 8 );
			lx += gfx::Text( Font::Small, L"CLASSE ", lx, ly, Rgba( 159, 178, 196, 0.6f ), gfx::Left, 0.0f, false );
			gfx::Text( Font::SmallBold, std::to_wstring( c->jobLevel ), lx, ly, isSelected ? GOLD_HOVER : TEXT );

			// estrela de favorito (ao lado do número)
			float sx = nxp - S( 16 ), sy = ry + rowH * 0.5f, sr = S( 8 );
			bool fav = IsFavorite( c->gid );
			bool starHover = Hovered( sx - S( 13 ), sy - S( 13 ), S( 26 ), S( 26 ) );
			Star( sx, sy, sr, fav ? GOLD : ( starHover ? Rgba( 255, 225, 140, 0.8f ) : Rgba( 159, 178, 196, 0.35f ) ) );

			AddHit( px, ry, pw, rowH, Action::Card, i );
			AddHit( sx - S( 13 ), sy - S( 13 ), S( 26 ), S( 26 ), Action::Local, FAVORITE_BASE + i );
		}
	}

	void Button( const wchar_t* label, float x, float y, float w, float h, int style, const config::Point& target,
		Action action = Action::Native, int local = 0 ){
		bool hover = Hovered( x, y, w, h );
		bool pressed = hover && input::IsLeftDown();
		float lift = hover && !pressed ? S( 2 ) : 0.0f;
		y -= lift;

		if( style == 0 ){ // principal (dourado)
			float pulse = 0.5f + 0.5f * sinf( Seconds() * 2.2f );
			gfx::Rect( x - S( 4 ), y - S( 4 ), w + S( 8 ), h + S( 8 ), Rgba( 240, 189, 69, hover ? 0.28f : 0.08f + 0.10f * pulse ) );
			gfx::Rect( x + S( 2 ), y + S( 5 ), w, h, Rgba( 0, 0, 0, 0.45f ) );
			gfx::RectV( x, y, w, h, GOLD_HOVER, GOLD );
			gfx::Frame( x, y, w, h, Rgba( 255, 240, 200, 0.6f ) );
			gfx::Text( Font::Title, label, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::Title ) ) * 0.5f, DARK_TEXT, gfx::Center, 0.0f, false );
		}else{
			bool danger = style == 2;
			D3DCOLOR base = danger ? Rgba( 255, 90, 90, 0.45f ) : Rgba( 18, 191, 255, 0.45f );
			D3DCOLOR hot = danger ? Rgba( 255, 110, 110, 0.9f ) : Rgba( 124, 227, 255, 0.9f );
			gfx::Rect( x, y, w, h, hover ? ( danger ? Rgba( 255, 60, 60, 0.22f ) : Rgba( 18, 191, 255, 0.22f ) ) : Rgba( 0, 0, 0, 0.6f ) );
			gfx::Frame( x, y, w, h, hover ? hot : base );
			gfx::Text( Font::SmallBold, label, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::SmallBold ) ) * 0.5f,
				danger ? Rgba( 255, 179, 179, 1 ) : BLUE_SOFT, gfx::Center );
		}
		AddHit( x, y, w, h + lift, action, local, target );
	}

	void LocalButton( const wchar_t* label, float x, float y, float w, float h, int style, int local ){
		Button( label, x, y, w, h, style, config::Point{}, Action::Local, local );
	}

	// Confirmação própria antes de acionar o botão nativo de exclusão
	void ConfirmModal( const chardata::Character* c ){
		const config::Settings& cfg = config::Get();
		bool final = g_modal == Modal::ConfirmFinal;
		g_hits.clear(); // só os botões da confirmação respondem

		gfx::Rect( 0, 0, g_frame.vpWidth, g_frame.vpHeight, Rgba( 0, 0, 0, 0.55f ) );
		float w = S( 460 ), h = S( 190 );
		float x = ( g_frame.vpWidth - w ) * 0.5f, y = ( g_frame.vpHeight - h ) * 0.5f;
		Panel( x, y, w, h );
		gfx::Rect( x, y, S( 3 ), h, Rgba( 255, 90, 90, 1 ) );

		std::wstring name = c != nullptr ? c->name : L"";
		gfx::Text( Font::TitleSmall, final ? L"APAGAR DEFINITIVAMENTE?" : L"AGENDAR EXCLUSÃO?", x + w * 0.5f, y + S( 20 ), Rgba( 255, 179, 179, 1 ), gfx::Center );
		gfx::Text( Font::BodyBold, name, x + w * 0.5f, y + S( 50 ), GOLD, gfx::Center, w - S( 40 ) );
		if( final ){
			gfx::Text( Font::Body, L"Esta ação não pode ser desfeita.", x + w * 0.5f, y + S( 80 ), TEXT, gfx::Center );
			gfx::Text( Font::Small, L"O jogo pedirá a confirmação dos seus dados em seguida.", x + w * 0.5f, y + S( 102 ), MUTED, gfx::Center );
		}else{
			gfx::Text( Font::Body, L"O personagem ficará marcado para exclusão.", x + w * 0.5f, y + S( 80 ), TEXT, gfx::Center );
			gfx::Text( Font::Small, L"Você poderá cancelar enquanto a exclusão estiver agendada.", x + w * 0.5f, y + S( 102 ), MUTED, gfx::Center );
		}

		float bw = S( 170 ), bh = S( 40 ), by = y + h - bh - S( 18 );
		Button( final ? L"APAGAR" : L"AGENDAR EXCLUSÃO", x + w * 0.5f - bw - S( 8 ), by, bw, bh, 2,
			final ? cfg.nativeDeleteFinal : cfg.nativeDelete );
		LocalButton( L"CANCELAR", x + w * 0.5f + S( 8 ), by, bw, bh, 1, CLOSE_MODAL );
	}

	// Sair do jogo (ESC ou SAIR): substitui a caixa nativa "Tem certeza que deseja sair?"
	void ExitModal(){
		g_hits.clear();
		gfx::Rect( 0, 0, g_frame.vpWidth, g_frame.vpHeight, Rgba( 0, 0, 0, 0.6f ) );
		float w = S( 420 ), h = S( 200 );
		float x = ( g_frame.vpWidth - w ) * 0.5f, y = ( g_frame.vpHeight - h ) * 0.5f;
		Panel( x, y, w, h );
		gfx::Rect( x, y, S( 3 ), h, GOLD );
		if( g_logo.tex != nullptr ){
			float lh = S( 34 ), lw = lh * g_logo.width / (float)g_logo.height;
			gfx::DrawImage( g_logo, x + ( w - lw ) * 0.5f, y + S( 18 ), lw, lh );
		}
		gfx::Text( Font::TitleSmall, L"SAIR DO JOGO?", x + w * 0.5f, y + S( 60 ), GOLD, gfx::Center );
		gfx::Text( Font::Body, L"Seus personagens ficam salvos no servidor.", x + w * 0.5f, y + S( 90 ), MUTED, gfx::Center );
		float bw = S( 170 ), bh = S( 40 ), by = y + h - bh - S( 20 );
		LocalButton( L"SAIR", x + w * 0.5f - bw - S( 8 ), by, bw, bh, 2, EXIT_GAME );
		LocalButton( L"CONTINUAR", x + w * 0.5f + S( 8 ), by, bw, bh, 1, CLOSE_MODAL );
	}

	// Clique = botão solto sobre um controle (usa os controles do frame anterior)
	void HandleClick(){
		bool down = input::IsLeftDown();
		bool released = g_prevDown && !down;
		g_prevDown = down;
		if( !released || g_frame.popupActive ){
			return;
		}
		input::Point p = input::RealCursor();
		for( auto it = g_hits.rbegin(); it != g_hits.rend(); ++it ){
			if( !Inside( p, it->x, it->y, it->w, it->h ) ){
				continue;
			}
			if( it->action == Action::Local && it->card >= FAVORITE_BASE ){
				const chardata::Character* c = chardata::Get( PageSlot( it->card - FAVORITE_BASE ) );
				if( c != nullptr ) ToggleFavorite( c->gid );
			}else if( it->action == Action::Local ){
				if( it->card == OPEN_CONFIRM_RESERVE ) g_modal = Modal::ConfirmReserve;
				else if( it->card == OPEN_CONFIRM_FINAL ) g_modal = Modal::ConfirmFinal;
				else if( it->card == OPEN_EXIT ) g_modal = Modal::ConfirmExit;
				else if( it->card == EXIT_GAME ){
					logger::Write( "Saida pelo modal (ESC/SAIR)" );
					input::QuitGame();
				}
				else g_modal = Modal::None;
			}else if( g_modal != Modal::None && it->action == Action::Native ){
				g_modal = Modal::None; // confirmado: o clique já foi entregue ao botão nativo
			}
			return;
		}
	}

	config::Point CardPoint( int card ){
		client::WndRect r;
		if( client::CardRect( card, r ) ){
			return { r.x + r.w * 0.5f, r.y + r.h * 0.45f }; // centro do cartão (área clicável)
		}
		return { -10000.0f, -10000.0f };
	}

	// Personagens no mapa: anel no chão (antes do sprite) e placa com nível e nome
	float g_selScale = 0.0f; // escala no lugar do slot selecionado (mesmo vazio)

	// Fração (0..1) do corpo do personagem que fica atrás da ficha
	float UnderInfo( int card ){
		const CardView& cv = g_cards[card];
		float hw = 24.0f * cv.scale, h = SpriteHeight( card ) * cv.scale;
		float x0 = cv.feetX - hw, x1 = cv.feetX + hw, y0 = cv.feetY - h, y1 = cv.feetY;
		float ix = std::max( 0.0f, std::min( x1, (float)g_infoRect.right ) - std::max( x0, (float)g_infoRect.left ) );
		float iy = std::max( 0.0f, std::min( y1, (float)g_infoRect.bottom ) - std::max( y0, (float)g_infoRect.top ) );
		return ( ix * iy ) / std::max( 1.0f, ( x1 - x0 ) * ( y1 - y0 ) );
	}

	// Personagem mais perto da câmera que o selecionado: translúcido (1 = normal)
	float NearFade( int card ){
		int sel = client::IndexInPage();
		if( card == sel || g_selScale <= 0.0f || !g_cards[card].inScene ){
			return 1.0f;
		}
		float ratio = g_cards[card].scale / g_selScale;
		if( ratio <= 1.12f ){
			return 1.0f;
		}
		return 1.0f - 0.65f * std::min( 1.0f, ( ratio - 1.12f ) / 0.5f );
	}

	void SceneCharacters( int page, int perPage, int count, int selected ){
		float t = Seconds();

		// posições primeiro (a transparência depende do selecionado); desenho do mais distante ao mais próximo
		int order[MAX_CARDS];
		int n = 0;
		for( int i = 0; i < count && i < MAX_CARDS; i++ ){
			CardView& cv = g_cards[i];
			int slot = page * perPage + i;
			cv.hasChar = HasCharacter( i );
			cv.inScene = false;
			float fx, fy, scale;
			if( !cv.hasChar || !scene::SlotToScreen( slot, fx, fy, scale ) ){
				continue;
			}
			cv.inScene = true;
			cv.feetX = fx;
			cv.feetY = fy;
			cv.scale = scale;
			order[n++] = i;
		}
		std::sort( order, order + n, []( int a, int b ){ return g_cards[a].scale < g_cards[b].scale; } );

		// lugar do slot selecionado; vazio: anel marcando onde o novo personagem vai ficar
		g_selScale = 0.0f;
		{
			float sx, sy, ss;
			if( selected >= 0 && scene::SlotToScreen( page * perPage + selected, sx, sy, ss ) ){
				g_selScale = ss;
				if( !HasCharacter( selected ) && g_glow.tex != nullptr && g_modal == Modal::None ){
					float pulse = 0.5f + 0.5f * sinf( t * 2.4f );
					float gw = 70.0f * ss, gh = 26.0f * ss;
					gfx::DrawImage( g_glow, sx - gw * 0.5f, sy - gh * 0.55f, gw, gh, Rgba( 18, 191, 255, 0.35f + 0.35f * pulse ) );
					gfx::DrawImage( g_glow, sx - gw * 0.8f, sy - gh * 0.85f, gw * 1.6f, gh * 1.6f, Rgba( 18, 191, 255, 0.15f * pulse ) );
				}
			}
		}

		for( int k = 0; k < n; k++ ){
			int i = order[k];
			CardView& cv = g_cards[i];
			int slot = page * perPage + i;
			float fx = cv.feetX, fy = cv.feetY;
			float fade = NearFade( i );

			float s = cv.scale;
			float bodyW = 44.0f * s, bodyH = SpriteHeight( i ) * s;
			bool isSelected = i == selected;
			bool hover = Hovered( fx - bodyW * 0.5f, fy - bodyH, bodyW, bodyH );

			if( g_glow.tex != nullptr && ( isSelected || hover ) ){
				float gw = 70.0f * s, gh = 26.0f * s;
				float pulse = 0.5f + 0.5f * sinf( t * 3.0f );
				D3DCOLOR c = isSelected ? Rgba( 240, 189, 69, 0.55f + 0.35f * pulse ) : Rgba( 18, 191, 255, 0.45f );
				gfx::DrawImage( g_glow, fx - gw * 0.5f, fy - gh * 0.55f, gw, gh, c );
				if( isSelected ){
					gfx::DrawImage( g_glow, fx - gw * 0.8f, fy - gh * 0.85f, gw * 1.6f, gh * 1.6f, Rgba( 240, 189, 69, 0.18f * pulse ) );
				}
			}

			// Placa: [Nv 300] Nome
			const chardata::Character* c = chardata::Get( slot );
			if( c != nullptr ){
				std::wstring lv = L"Nv " + std::to_wstring( c->baseLevel );
				float lw = gfx::TextWidth( Font::SmallBold, lv ) + S( 12 );
				float nw = gfx::TextWidth( Font::BodyBold, c->name ) + S( 16 );
				float ph = S( 22 ), pw = lw + nw;
				float px = fx - pw * 0.5f, py = fy - bodyH - ph - S( 6 );
				bool underInfo = px < g_infoRect.right && px + pw > g_infoRect.left && py < g_infoRect.bottom && py + ph > g_infoRect.top;
				bool offScene = py < g_sceneClip.top || py + ph > g_sceneClip.bottom;
				if( px < g_sceneClip.left || px + pw > g_sceneClip.right || underInfo || offScene || fade < 0.75f || UnderInfo( i ) > 0.15f ){
					AddHit( fx - bodyW * 0.5f, fy - bodyH, bodyW, bodyH, Action::Card, i );
					continue; // placa sairia da cena (ficaria por baixo da lista)
				}
				D3DCOLOR frameColor = c->deleteDate != 0 ? Rgba( 255, 110, 110, 0.9f )
					: ( isSelected ? Rgba( 240, 189, 69, 0.85f ) : ( hover ? Rgba( 18, 191, 255, 0.6f ) : Rgba( 255, 255, 255, 0.12f ) ) );
				Plate plate{ px, py, pw, ph, lw, isSelected ? Rgba( 8, 16, 28, 0.9f ) : Rgba( 0, 0, 0, 0.55f ), frameColor,
					isSelected ? GOLD : Rgba( 18, 191, 255, 0.35f ), isSelected ? DARK_TEXT : TEXT, isSelected ? GOLD_HOVER : TEXT, lv, c->name };
				if( isSelected ){
					// balão: [selo] Nome / CLASSE, com a ponta apontando para a cabeça
					std::wstring job = chardata::JobName( c->job, c->sex );
					for( wchar_t& ch : job ) ch = towupper( ch );
					float badge = S( 56 );
					float textW = std::max( gfx::TextWidth( Font::Name, c->name ), gfx::TextWidth( Font::Small, job ) );
					float bw = badge * 0.5f + S( 16 ) + textW + S( 18 );
					float bh = S( 42 );
					float bob = S( 2 ) * sinf( t * 2.2f );
					plate.balloon = true;
					plate.job = c->job;
					plate.jobName = job;
					plate.pw = badge * 0.5f + bw;
					plate.ph = badge;
					plate.px = fx - plate.pw * 0.5f;
					plate.py = fy - bodyH - badge - S( 16 ) + bob;
					plate.lw = bh;
					plate.tipX = fx;
					if( plate.px < g_sceneClip.left || plate.px + plate.pw > g_sceneClip.right || plate.py < g_sceneClip.top ){
						plate.balloon = false; // sem espaço: placa simples
						plate.px = px; plate.py = py; plate.pw = pw; plate.ph = ph; plate.lw = lw;
					}else{
						px = plate.px; py = plate.py; pw = plate.pw; ph = plate.ph;
					}
				}
				g_plates.push_back( plate );
				AddHit( px, py, pw, ph, Action::Card, i );
			}
			AddHit( fx - bodyW * 0.5f, fy - bodyH, bodyW, bodyH, Action::Card, i );
		}
	}
}

namespace {
	// ---------------------------------------------------------------- criação de personagem
	ui::MakeFrame g_make;
	std::vector<Hit> g_makeHits;
	bool g_makeLayout = false;
	bool g_nameFocus = false;
	bool g_makePrevDown = false;

	// Controles da janela nativa de criação (relativos ao canto dela)
	const config::Point MK_MALE{ 468, 71 }, MK_FEMALE{ 530, 71 };
	const config::Point MK_ROT_L{ 436, 190 }, MK_ROT_R{ 561, 190 };
	const config::Point MK_NAME{ 499, 275 };
	const config::Point MK_CANCEL{ 101, 392 }, MK_CREATE{ 691, 392 };
	const float MK_HAIR_X[4] = { 624, 662, 700, 738 };
	const float MK_HAIR_Y[6] = { 76, 114, 151, 188, 225, 261 };
	const config::Point MK_COLORS[9] = { { 630, 316 }, { 657, 316 }, { 684, 316 }, { 711, 316 }, { 738, 316 },
		{ 630, 340 }, { 657, 340 }, { 684, 340 }, { 711, 340 } };
	const D3DCOLOR HAIR_SWATCH[9] = { 0, 0xFFE8C547, 0xFF9B6BD6, 0xFFC8935A, 0xFF5FAE5A, 0xFF4F7FD6, 0xFFE8E8E8, 0xFF7A4E2E, 0xFFC83A3A };

	void MakeHit( float x, float y, float w, float h, const config::Point& target ){
		g_makeHits.push_back( { x, y, w, h, Action::Native, -1, target } );
	}

	// Recorta um retângulo da janela nativa (coordenadas relativas a ela) e desenha em dst
	void DrawNativeRegion( float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, D3DCOLOR color = 0xFFFFFFFF ){
		float ax = g_make.originX + sx, ay = g_make.originY + sy;
		for( int i = 0; i < g_make.tileCount; i++ ){
			const ui::MakeTile& t = g_make.tiles[i];
			float x0 = std::max( ax, t.x ), y0 = std::max( ay, t.y );
			float x1 = std::min( ax + sw, t.x + t.w ), y1 = std::min( ay + sh, t.y + t.h );
			if( x1 <= x0 || y1 <= y0 ) continue;
			float u0 = t.u0 + ( x0 - t.x ) / t.w * ( t.u1 - t.u0 ), u1 = t.u0 + ( x1 - t.x ) / t.w * ( t.u1 - t.u0 );
			float v0 = t.v0 + ( y0 - t.y ) / t.h * ( t.v1 - t.v0 ), v1 = t.v0 + ( y1 - t.y ) / t.h * ( t.v1 - t.v0 );
			float kx = dw / sw, ky = dh / sh;
			gfx::Image img;
			img.tex = t.tex;
			img.width = 256;
			img.height = 256;
			gfx::DrawImage( img, dx + ( x0 - ax ) * kx, dy + ( y0 - ay ) * ky, ( x1 - x0 ) * kx, ( y1 - y0 ) * ky, color, u0, v0, u1, v1 );
		}
	}

	// Texto com quebra de linha simples (por palavras)
	float WrapText( Font font, const std::wstring& text, float x, float y, float w, D3DCOLOR color ){
		std::wstring line, word;
		float lh = gfx::LineHeight( font ) + S( 2 );
		auto flush = [&]( bool force ){
			if( !line.empty() || force ){
				gfx::Text( font, line, x, y, color, gfx::Left, 0.0f, false );
				y += lh;
				line.clear();
			}
		};
		for( size_t i = 0; i <= text.size(); i++ ){
			wchar_t ch = i < text.size() ? text[i] : L' ';
			if( ch == L' ' ){
				std::wstring test = line.empty() ? word : line + L" " + word;
				if( !line.empty() && gfx::TextWidth( font, test ) > w ){
					flush( false );
					line = word;
				}else{
					line = test;
				}
				word.clear();
			}else{
				word += ch;
			}
		}
		flush( false );
		return y;
	}

	void MakeTopBar(){
		float h = S( 34 );
		gfx::Rect( 0, 0, g_make.vpWidth, h, PANEL_DEEP );
		AccentLine( 0, h - S( 2 ), g_make.vpWidth );
		float x = S( 16 );
		if( g_logo.tex != nullptr ){
			float lh = S( 26 ), lw = lh * g_logo.width / (float)g_logo.height;
			gfx::DrawImage( g_logo, x, ( h - lh ) * 0.5f, lw, lh );
			x += lw + S( 8 );
		}
		x += gfx::Text( Font::TitleSmall, L"O CODEX", x, ( h - gfx::LineHeight( Font::TitleSmall ) ) * 0.5f, GOLD );
		gfx::Text( Font::Small, L"   ·   CRIAÇÃO DE PERSONAGEM", x, ( h - gfx::LineHeight( Font::Small ) ) * 0.5f, MUTED );
	}

	void MakeButton( const wchar_t* label, float x, float y, float w, float h, bool primary, const config::Point& target ){
		bool hover = Hovered( x, y, w, h );
		if( primary ){
			float pulse = 0.5f + 0.5f * sinf( Seconds() * 2.2f );
			gfx::Rect( x - S( 4 ), y - S( 4 ), w + S( 8 ), h + S( 8 ), Rgba( 240, 189, 69, hover ? 0.28f : 0.08f + 0.10f * pulse ) );
			gfx::RectV( x, y, w, h, GOLD_HOVER, GOLD );
			gfx::Frame( x, y, w, h, Rgba( 255, 240, 200, 0.6f ) );
			gfx::Text( Font::Title, label, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::Title ) ) * 0.5f, DARK_TEXT, gfx::Center, 0.0f, false );
		}else{
			gfx::Rect( x, y, w, h, hover ? Rgba( 18, 191, 255, 0.22f ) : Rgba( 0, 0, 0, 0.6f ) );
			gfx::Frame( x, y, w, h, hover ? Rgba( 124, 227, 255, 0.9f ) : Rgba( 18, 191, 255, 0.45f ) );
			gfx::Text( Font::SmallBold, label, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, BLUE_SOFT, gfx::Center );
		}
		MakeHit( x, y, w, h, target );
	}

	void OriginPanel( const client::MakeState& st, float x, float y, float w, float h ){
		Panel( x, y, w, h );
		float px = x + S( 14 ), pw = w - S( 28 ), cy = y + S( 16 );
		SectionTitle( L"ORIGEM", px, cy, pw );
		cy += S( 26 );

		// raça (a janela nativa só oferece Humano); a altura acompanha o texto
		const wchar_t* desc = L"A raça com maior presença em Rune-Midgard. Talentosos e adaptáveis, podem seguir qualquer caminho.";
		float textTop = cy + S( 12 ) + gfx::LineHeight( Font::Name ) + S( 4 );
		float lines = 0;
		{
			// mede as linhas sem desenhar
			std::wstring line, word, t = desc;
			for( size_t i = 0; i <= t.size(); i++ ){
				wchar_t c2 = i < t.size() ? t[i] : L' ';
				if( c2 == L' ' ){
					std::wstring test = line.empty() ? word : line + L" " + word;
					if( !line.empty() && gfx::TextWidth( Font::Small, test ) > pw - S( 74 ) ){ lines++; line = word; }
					else line = test;
					word.clear();
				}else word += c2;
			}
			if( !line.empty() ) lines++;
		}
		float ch = std::max( S( 80 ), textTop - cy + lines * ( gfx::LineHeight( Font::Small ) + S( 2 ) ) + S( 10 ) );
		gfx::Rect( px, cy, pw, ch, Rgba( 240, 189, 69, 0.12f ) );
		gfx::Frame( px, cy, pw, ch, Rgba( 240, 189, 69, 0.8f ) );
		gfx::Rect( px, cy, S( 3 ), ch, GOLD );
		ClassBadge( px + S( 32 ), cy + S( 34 ), S( 46 ), 0, GOLD, Rgba( 10, 18, 32, 1.0f ), GOLD_HOVER );
		gfx::Text( Font::Name, L"Humano", px + S( 64 ), cy + S( 12 ), GOLD_HOVER );
		WrapText( Font::Small, desc, px + S( 64 ), textTop, pw - S( 74 ), MUTED );
		cy += ch + S( 18 );

		SectionTitle( L"CAMINHOS", px, cy, pw );
		cy += S( 24 );
		const int jobs[9] = { 1, 2, 3, 4, 5, 6, 24, 25, 4046 };
		float cw = pw / 3.0f, rowH = S( 70 );
		for( int i = 0; i < 9; i++ ){
			float cx = px + ( i % 3 ) * cw, ry = cy + ( i / 3 ) * rowH;
			bool hover = Hovered( cx + S( 4 ), ry, cw - S( 8 ), rowH - S( 6 ) );
			gfx::Rect( cx + S( 4 ), ry, cw - S( 8 ), rowH - S( 6 ), hover ? Rgba( 18, 191, 255, 0.10f ) : Rgba( 255, 255, 255, 0.035f ) );
			if( hover ) gfx::Frame( cx + S( 4 ), ry, cw - S( 8 ), rowH - S( 6 ), Rgba( 18, 191, 255, 0.45f ) );
			ClassBadge( cx + cw * 0.5f, ry + S( 22 ), S( 32 ), jobs[i], hover ? GOLD : Rgba( 159, 178, 196, 0.8f ),
				Rgba( 10, 18, 32, 1.0f ), hover ? GOLD_HOVER : TEXT );
			gfx::Text( Font::Small, chardata::JobName( jobs[i], st.sex ), cx + cw * 0.5f, ry + S( 42 ), hover ? TEXT : MUTED, gfx::Center, cw - S( 12 ) );
		}
		cy += 3 * rowH + S( 6 );
		WrapText( Font::Small, L"Todo aventureiro começa como Aprendiz e escolhe o caminho dentro do jogo.", px, cy, pw, Rgba( 159, 178, 196, 0.7f ) );
	}

	void AppearancePanel( const client::MakeState& st, float x, float y, float w, float h ){
		Panel( x, y, w, h );
		float px = x + S( 14 ), pw = w - S( 28 ), cy = y + S( 16 );
		SectionTitle( L"APARÊNCIA", px, cy, pw );
		cy += S( 28 );

		// gênero
		gfx::Text( Font::SmallBold, L"GÊNERO", px, cy, MUTED );
		cy += S( 18 );
		float bw = ( pw - S( 8 ) ) * 0.5f, bh = S( 32 );
		for( int i = 0; i < 2; i++ ){
			bool active = ( i == 0 ) == ( st.sex == 1 );
			float bx = px + i * ( bw + S( 8 ) );
			bool hover = Hovered( bx, cy, bw, bh );
			gfx::Rect( bx, cy, bw, bh, active ? Rgba( 240, 189, 69, 0.18f ) : ( hover ? Rgba( 18, 191, 255, 0.14f ) : Rgba( 0, 0, 0, 0.45f ) ) );
			gfx::Frame( bx, cy, bw, bh, active ? GOLD : ( hover ? Rgba( 124, 227, 255, 0.8f ) : BORDER ) );
			gfx::Text( Font::BodyBold, i == 0 ? L"Masculino" : L"Feminino", bx + bw * 0.5f,
				cy + ( bh - gfx::LineHeight( Font::BodyBold ) ) * 0.5f, active ? GOLD_HOVER : TEXT, gfx::Center );
			MakeHit( bx, cy, bw, bh, i == 0 ? MK_MALE : MK_FEMALE );
		}
		cy += bh + S( 16 );

		// penteados: cabeças recortadas da janela nativa (o destaque do escolhido vem junto)
		gfx::Text( Font::SmallBold, L"ESTILO DE CABELO", px, cy, MUTED );
		gfx::Text( Font::SmallBold, std::to_wstring( st.hair ), px + pw, cy, GOLD, gfx::Right );
		cy += S( 18 );
		int cols = 6;
		float gap = S( 5 ), cell = ( pw - gap * ( cols - 1 ) ) / cols;
		for( int k = 0; k < 24; k++ ){
			int nc = k % 4, nr = k / 4;
			float cx = px + ( k % cols ) * ( cell + gap ), ry = cy + ( k / cols ) * ( cell + gap );
			bool hover = Hovered( cx, ry, cell, cell );
			gfx::Rect( cx, ry, cell, cell, Rgba( 3, 6, 12, 0.85f ) );
			DrawNativeRegion( MK_HAIR_X[nc] - 16, MK_HAIR_Y[nr] - 16, 32, 32, cx + 2, ry + 2, cell - 4, cell - 4 );
			gfx::Frame( cx, ry, cell, cell, hover ? Rgba( 124, 227, 255, 0.9f ) : Rgba( 255, 255, 255, 0.10f ) );
			MakeHit( cx, ry, cell, cell, config::Point{ MK_HAIR_X[nc], MK_HAIR_Y[nr] } );
		}
		cy += 4 * ( cell + gap ) + S( 12 );

		// cores
		gfx::Text( Font::SmallBold, L"COR DO CABELO", px, cy, MUTED );
		gfx::Text( Font::SmallBold, st.hairColor == 0 ? L"PADRÃO" : std::to_wstring( st.hairColor ), px + pw, cy, GOLD, gfx::Right );
		cy += S( 18 );
		float sw = ( pw - S( 6 ) * 8 ) / 9.0f, sh = S( 24 );
		for( int i = 0; i < 9; i++ ){
			float sx = px + i * ( sw + S( 6 ) );
			bool active = st.hairColor == i;
			bool hover = Hovered( sx, cy, sw, sh );
			if( active ) gfx::RoundRect( sx - S( 3 ), cy - S( 3 ), sw + S( 6 ), sh + S( 6 ), S( 10 ), GOLD, g_disc );
			gfx::RoundRect( sx, cy, sw, sh, S( 8 ), i == 0 ? Rgba( 40, 48, 60, 1.0f ) : HAIR_SWATCH[i], g_disc );
			if( i == 0 ) gfx::Line( sx + S( 5 ), cy + sh - S( 5 ), sx + sw - S( 5 ), cy + S( 5 ), MUTED, S( 2 ) );
			if( hover && !active ) gfx::Frame( sx - 1, cy - 1, sw + 2, sh + 2, Rgba( 124, 227, 255, 0.9f ) );
			MakeHit( sx, cy, sw, sh, MK_COLORS[i] );
		}
		cy += sh + S( 18 );

		// nome (o texto vem do campo nativo, que recebe o teclado)
		gfx::Text( Font::SmallBold, L"NOME", px, cy, MUTED );
		cy += S( 18 );
		std::string name;
		client::ReadMakeName( name );
		std::wstring wname( name.begin(), name.end() );
		float nh = S( 34 );
		bool hover = Hovered( px, cy, pw, nh );
		gfx::Rect( px, cy, pw, nh, Rgba( 0, 0, 0, 0.55f ) );
		gfx::Frame( px, cy, pw, nh, g_nameFocus ? GOLD : ( hover ? Rgba( 124, 227, 255, 0.8f ) : BORDER ) );
		float tx = px + S( 10 ), ty = cy + ( nh - gfx::LineHeight( Font::Name ) ) * 0.5f;
		if( wname.empty() && !g_nameFocus ){
			gfx::Text( Font::Body, L"Digite o nome do personagem", tx, cy + ( nh - gfx::LineHeight( Font::Body ) ) * 0.5f, Rgba( 159, 178, 196, 0.55f ) );
		}else{
			float w2 = gfx::Text( Font::Name, wname, tx, ty, TEXT );
			if( g_nameFocus && fmodf( Seconds(), 1.0f ) < 0.55f ){
				gfx::Rect( tx + w2 + S( 2 ), cy + S( 8 ), S( 2 ), nh - S( 16 ), GOLD );
			}
		}
		MakeHit( px, cy, pw, nh, MK_NAME );
	}
}

namespace {
	// ---------------------------------------------------------------- telas de entrada
	ui::LoginFrame g_login;
	int g_serverCountdown = -1; // lista de servidores: ms até entrar sozinho (-1 = sem contagem)
	std::vector<Hit> g_loginHits;
	bool g_loginLayout = false;
	int g_loginFocus = -1;        // 0 = usuário, 1 = senha (campo clicado por último)
	bool g_loginPrevDown = false;
	int g_serverRow = 0;          // linha escolhida na lista de servidores

	// Controles nativos (relativos ao canto da janela)
	const config::Point LG_ID{ 92, 47 }, LG_PW{ 92, 71 }, LG_SAVE{ 62, 91 };
	const config::Point LG_LOGIN{ 234, 74 }, LG_INTRO{ 46, 115 }, LG_REGISTER{ 128, 115 }, LG_CLOSE{ 281, 24 };
	const config::Point SV_OK{ 208, 184 }, SV_CANCEL{ 253, 184 };
	constexpr float SV_ROW0 = 28, SV_ROWH = 18;

	void LoginHit( float x, float y, float w, float h, float ox, float oy, const config::Point& p, int local = -1 ){
		Hit hit{ x, y, w, h, Action::Native, local, config::Point{ ox + p.x, oy + p.y } };
		g_loginHits.push_back( hit );
	}

	void TextBox( const wchar_t* label, const std::wstring& value, bool focused, float x, float y, float w, float h, bool hover ){
		gfx::Text( Font::SmallBold, label, x, y - gfx::LineHeight( Font::SmallBold ) - S( 4 ), MUTED );
		gfx::Rect( x, y, w, h, Rgba( 0, 0, 0, 0.55f ) );
		gfx::Frame( x, y, w, h, focused ? GOLD : ( hover ? Rgba( 124, 227, 255, 0.8f ) : BORDER ) );
		float tx = x + S( 12 ), ty = y + ( h - gfx::LineHeight( Font::Name ) ) * 0.5f;
		float tw = gfx::Text( Font::Name, value, tx, ty, TEXT, gfx::Left, w - S( 24 ) );
		if( focused && fmodf( Seconds(), 1.0f ) < 0.55f ){
			gfx::Rect( tx + tw + S( 2 ), y + S( 9 ), S( 2 ), h - S( 18 ), GOLD );
		}
	}

	void LoginLink( const wchar_t* label, float x, float y, float ox, float oy, const config::Point& p ){
		float w = gfx::TextWidth( Font::BodyBold, label ), h = gfx::LineHeight( Font::BodyBold );
		bool hover = Hovered( x - S( 4 ), y - S( 2 ), w + S( 8 ), h + S( 4 ) );
		gfx::Text( Font::BodyBold, label, x, y, hover ? GOLD_HOVER : BLUE_SOFT );
		if( hover ) gfx::Rect( x, y + h, w, 1, GOLD_HOVER );
		LoginHit( x - S( 4 ), y - S( 2 ), w + S( 8 ), h + S( 4 ), ox, oy, p );
	}

	void LoginGoldButton( const wchar_t* label, float x, float y, float w, float h, float ox, float oy, const config::Point& p ){
		bool hover = Hovered( x, y, w, h );
		float pulse = 0.5f + 0.5f * sinf( Seconds() * 2.2f );
		gfx::Rect( x - S( 4 ), y - S( 4 ), w + S( 8 ), h + S( 8 ), Rgba( 240, 189, 69, hover ? 0.28f : 0.08f + 0.10f * pulse ) );
		gfx::RectV( x, y, w, h, GOLD_HOVER, GOLD );
		gfx::Frame( x, y, w, h, Rgba( 255, 240, 200, 0.6f ) );
		gfx::Text( Font::Title, label, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::Title ) ) * 0.5f, DARK_TEXT, gfx::Center, 0.0f, false );
		LoginHit( x, y, w, h, ox, oy, p );
	}

	void LoginGhostButton( const wchar_t* label, float x, float y, float w, float h, float ox, float oy, const config::Point& p ){
		bool hover = Hovered( x, y, w, h );
		gfx::Rect( x, y, w, h, hover ? Rgba( 18, 191, 255, 0.22f ) : Rgba( 0, 0, 0, 0.6f ) );
		gfx::Frame( x, y, w, h, hover ? Rgba( 124, 227, 255, 0.9f ) : Rgba( 18, 191, 255, 0.45f ) );
		gfx::Text( Font::SmallBold, label, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, BLUE_SOFT, gfx::Center );
		LoginHit( x, y, w, h, ox, oy, p );
	}

	// Marca: logotipo grande, nome e lema (lado esquerdo)
	void LoginBrand( float x, float cy ){
		std::wstring motto = L"Além das Páginas de Midgard";
		const auto& names = chardata::LoginServers();
		if( !names.empty() ){
			size_t t = names[0].find( L'~' );
			if( t != std::wstring::npos ){
				motto = names[0].substr( t + 1 );
				while( !motto.empty() && motto[0] == L' ' ) motto.erase( 0, 1 );
			}
		}
		float lh = S( 150 );
		if( g_logo.tex ){
			float lw = lh * g_logo.width / (float)g_logo.height;
			if( g_glow.tex ) gfx::DrawImage( g_glow, x - lw * 0.3f, cy - lh * 0.9f, lw * 1.6f, lh * 1.1f, Rgba( 240, 189, 69, 0.18f ) );
			gfx::DrawImage( g_logo, x, cy - lh - S( 20 ), lw, lh );
		}
		gfx::Text( Font::Title, L"O CODEX", x, cy, GOLD );
		AccentLine( x, cy + gfx::LineHeight( Font::Title ) + S( 6 ), S( 260 ) );
		gfx::Text( Font::Body, motto, x, cy + gfx::LineHeight( Font::Title ) + S( 16 ), TEXT );
	}

	void LoginPanel( float W, float H ){
		const ui::LoginFrame& f = g_login;
		std::string id;
		int pwLen = 0;
		bool save = false;
		client::ReadLoginFields( id, pwLen, save );
		if( g_loginFocus < 0 ) g_loginFocus = id.empty() ? 0 : 1;

		float pw = S( 380 ), ph = S( 400 );
		float px = W * 0.66f - pw * 0.5f, py = ( H - ph ) * 0.5f;
		Panel( px, py, pw, ph );
		float cx = px + S( 28 ), cw = pw - S( 56 ), cy = py + S( 26 );
		gfx::Text( Font::TitleSmall, L"ENTRAR", cx, cy, GOLD );
		cy += S( 26 );
		const auto& names = chardata::LoginServers();
		std::wstring server = names.empty() ? L"O Codex" : names[0].substr( 0, names[0].find( L" ~" ) );
		gfx::Text( Font::Small, L"Servidor  ·  " + server, cx, cy, MUTED );
		cy += S( 46 );

		float fh = S( 40 );
		bool hoverId = Hovered( cx, cy, cw, fh );
		TextBox( L"USUÁRIO", std::wstring( id.begin(), id.end() ), g_loginFocus == 0, cx, cy, cw, fh, hoverId );
		LoginHit( cx, cy, cw, fh, f.loginX, f.loginY, LG_ID, 0 );
		cy += fh + S( 34 );
		bool hoverPw = Hovered( cx, cy, cw, fh );
		TextBox( L"SENHA", std::wstring( pwLen, L'\x2022' ), g_loginFocus == 1, cx, cy, cw, fh, hoverPw );
		LoginHit( cx, cy, cw, fh, f.loginX, f.loginY, LG_PW, 1 );
		cy += fh + S( 14 );

		// salvar usuário
		float bs = S( 16 );
		bool hoverSave = Hovered( cx, cy, cw * 0.6f, bs );
		gfx::Rect( cx, cy, bs, bs, Rgba( 0, 0, 0, 0.55f ) );
		gfx::Frame( cx, cy, bs, bs, save ? GOLD : ( hoverSave ? Rgba( 124, 227, 255, 0.8f ) : BORDER ) );
		if( save ) gfx::Rect( cx + S( 4 ), cy + S( 4 ), bs - S( 8 ), bs - S( 8 ), GOLD );
		gfx::Text( Font::Body, L"Lembrar usuário", cx + bs + S( 8 ), cy + ( bs - gfx::LineHeight( Font::Body ) ) * 0.5f, hoverSave ? TEXT : MUTED );
		LoginHit( cx, cy, cw * 0.6f, bs, f.loginX, f.loginY, LG_SAVE );
		cy += bs + S( 26 );

		LoginGoldButton( L"ENTRAR", cx, cy, cw, S( 54 ), f.loginX, f.loginY, LG_LOGIN );
		cy += S( 54 ) + S( 24 );

		LoginLink( L"Criar conta", cx, cy, f.loginX, f.loginY, LG_REGISTER );
		float aw = gfx::TextWidth( Font::BodyBold, L"Abertura" );
		LoginLink( L"Abertura", cx + cw * 0.5f - aw * 0.5f, cy, f.loginX, f.loginY, LG_INTRO );
		float sw = gfx::TextWidth( Font::BodyBold, L"Sair" );
		LoginLink( L"Sair", cx + cw - sw, cy, f.loginX, f.loginY, LG_CLOSE );
	}

	void ServerPanel( float W, float H ){
		const ui::LoginFrame& f = g_login;
		struct Row { std::wstring name; int users; };
		std::vector<Row> rows;
		for( const auto& s : chardata::CharServers() ) rows.push_back( { s.name, s.users } );
		if( rows.empty() ){
			for( const auto& n : chardata::LoginServers() ) rows.push_back( { n, -1 } );
		}
		if( rows.empty() ) rows.push_back( { L"O Codex", -1 } );
		g_serverRow = std::min( g_serverRow, (int)rows.size() - 1 );

		float rowH = S( 58 ), pw = S( 460 );
		float ph = S( 96 ) + rows.size() * ( rowH + S( 6 ) ) + S( 76 );
		float px = ( W - pw ) * 0.5f, py = ( H - ph ) * 0.5f;
		Panel( px, py, pw, ph );
		float cx = px + S( 24 ), cw = pw - S( 48 ), cy = py + S( 26 );
		gfx::Text( Font::TitleSmall, L"ESCOLHA O SERVIDOR", cx, cy, GOLD );
		cy += S( 50 );
		for( size_t i = 0; i < rows.size(); i++ ){
			bool sel = (int)i == g_serverRow;
			bool hover = Hovered( cx, cy, cw, rowH );
			gfx::Rect( cx, cy, cw, rowH, sel ? Rgba( 240, 189, 69, 0.14f ) : ( hover ? Rgba( 18, 191, 255, 0.08f ) : Rgba( 255, 255, 255, 0.04f ) ) );
			if( sel ){
				gfx::Frame( cx, cy, cw, rowH, Rgba( 240, 189, 69, 0.85f ) );
				gfx::Rect( cx, cy, S( 3 ), rowH, GOLD );
			}
			// lotação enviada pelo login-server (não é a quantidade de jogadores):
			// 0 tranquilo, 1 normal, 2 cheio, 3 lotado, 4 = desativado (usercount_disable)
			int lot = rows[i].users;
			D3DCOLOR corLot = lot == 1 ? Rgba( 240, 200, 70, 0.95f ) : lot == 2 ? Rgba( 240, 90, 80, 0.95f )
				: lot == 3 ? Rgba( 170, 110, 240, 0.95f ) : Rgba( 90, 220, 120, 0.9f );
			float dot = S( 10 );
			float nameMid = cy + S( 9 ) + gfx::LineHeight( Font::Name ) * 0.5f;
			gfx::DrawImage( g_disc, cx + S( 16 ), nameMid - dot * 0.5f, dot, dot, corLot );
			std::wstring name = rows[i].name;
			size_t t = name.find( L" ~" );
			std::wstring sub = t != std::wstring::npos ? name.substr( t + 2 ) : L"";
			if( t != std::wstring::npos ) name = name.substr( 0, t );
			while( !sub.empty() && sub[0] == L' ' ) sub.erase( 0, 1 );
			float tx = cx + S( 38 );
			gfx::Text( Font::Name, name, tx, cy + S( 9 ), sel ? GOLD_HOVER : TEXT, gfx::Left, cw - S( 160 ) );
			if( !sub.empty() ) gfx::Text( Font::Small, sub, tx, cy + S( 33 ), MUTED, gfx::Left, cw - S( 160 ) );
			if( lot >= 0 && lot <= 3 ){
				const wchar_t* nomes[4] = { L"Tranquilo", L"Normal", L"Cheio", L"Lotado" };
				gfx::Text( Font::SmallBold, nomes[lot], cx + cw - S( 14 ), cy + ( rowH - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, corLot, gfx::Right );
			}
			LoginHit( cx, cy, cw, rowH, f.serversX, f.serversY, config::Point{ 140, SV_ROW0 + SV_ROWH * i }, 100 + (int)i );
			cy += rowH + S( 6 );
		}
		cy += S( 14 );
		if( g_serverCountdown != -1 ){
			int s = ( std::max( 0, g_serverCountdown ) + 999 ) / 1000;
			if( s > 0 ){
				gfx::Text( Font::Small, L"Escolha o servidor em até " + std::to_wstring( s ) + L" s",
					cx, cy - S( 6 ), s <= 10 ? Rgba( 255, 179, 120, 1 ) : MUTED );
			}else{
				gfx::Text( Font::Small, L"Tempo esgotado: volte e entre de novo na conta", cx, cy - S( 6 ), Rgba( 255, 140, 140, 1 ) );
			}
			cy += S( 18 );
		}
		float bw = ( cw - S( 12 ) ) * 0.5f;
		LoginGhostButton( L"VOLTAR", cx, cy, bw, S( 46 ), f.serversX, f.serversY, SV_CANCEL );
		LoginGoldButton( L"ENTRAR", cx + bw + S( 12 ), cy, bw, S( 46 ), f.serversX, f.serversY, SV_OK );
	}

	void WaitPanel( float W, float H ){
		float pw = S( 300 ), ph = S( 110 );
		float px = ( W - pw ) * 0.5f, py = ( H - ph ) * 0.5f;
		Panel( px, py, pw, ph );
		int dots = (int)( Seconds() * 3.0f ) % 4;
		gfx::Text( Font::TitleSmall, std::wstring( L"CONECTANDO" ) + std::wstring( dots, L'.' ), px + S( 28 ), py + S( 30 ), GOLD );
		// barra indeterminada
		float bx = px + S( 28 ), bw = pw - S( 56 ), by = py + S( 70 );
		gfx::Rect( bx, by, bw, S( 4 ), TRACK );
		float t = fmodf( Seconds() * 0.8f, 1.0f );
		float seg = bw * 0.3f, sx = bx + ( bw + seg ) * t - seg;
		float x0 = std::max( bx, sx ), x1 = std::min( bx + bw, sx + seg );
		if( x1 > x0 ) gfx::RectH( x0, by, x1 - x0, S( 4 ), BLUE, GOLD );
	}
}

namespace {
	// ---------------------------------------------------------------- mensagens do cliente
	ui::MessageFrame g_msg;
	bool g_msgActive = false;
	std::vector<Hit> g_msgHits;
	const config::Point MSG_OK1{ 254, 104 }, MSG_OK2{ 209, 105 }, MSG_CANCEL{ 255, 105 };

	bool MsgHover( float x, float y, float w, float h ){
		return Inside( input::RealCursor(), x, y, w, h );
	}

	void MsgButton( const wchar_t* label, float x, float y, float w, float h, bool primary, const config::Point& native ){
		bool hover = MsgHover( x, y, w, h );
		if( primary ){
			gfx::Rect( x - S( 3 ), y - S( 3 ), w + S( 6 ), h + S( 6 ), Rgba( 240, 189, 69, hover ? 0.3f : 0.12f ) );
			gfx::RectV( x, y, w, h, GOLD_HOVER, GOLD );
			gfx::Frame( x, y, w, h, Rgba( 255, 240, 200, 0.6f ) );
			gfx::Text( Font::TitleSmall, label, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::TitleSmall ) ) * 0.5f, DARK_TEXT, gfx::Center, 0.0f, false );
		}else{
			gfx::Rect( x, y, w, h, hover ? Rgba( 18, 191, 255, 0.22f ) : Rgba( 0, 0, 0, 0.6f ) );
			gfx::Frame( x, y, w, h, hover ? Rgba( 124, 227, 255, 0.9f ) : Rgba( 18, 191, 255, 0.45f ) );
			gfx::Text( Font::SmallBold, label, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, BLUE_SOFT, gfx::Center );
		}
		g_msgHits.push_back( { x, y, w, h, Action::Native, -1, config::Point{ g_msg.x + native.x, g_msg.y + native.y } } );
	}

	// altura do texto quebrado em linhas (mesma regra do WrapText)
	float WrapHeight( Font font, const std::wstring& text, float w ){
		std::wstring line, word;
		int lines = 0;
		for( size_t i = 0; i <= text.size(); i++ ){
			wchar_t c = i < text.size() ? text[i] : L' ';
			if( c == L' ' ){
				std::wstring t = line.empty() ? word : line + L" " + word;
				if( !line.empty() && gfx::TextWidth( font, t ) > w ){ lines++; line = word; }
				else line = t;
				word.clear();
			}else word += c;
		}
		if( !line.empty() ) lines++;
		return std::max( 1, lines ) * ( gfx::LineHeight( font ) + S( 2 ) );
	}
}


void ui::SetServerCountdown( int ms ){
	g_serverCountdown = ms;
}

void ui::ClearMessage(){
	g_msgActive = false;
	g_msgHits.clear();
}

void ui::DrawMessage( IDirect3DDevice9* dev, const MessageFrame& frame ){
	g_msg = frame;
	g_s = std::min( 1.5f, std::max( 0.75f, frame.vpHeight / 900.0f ) );
	gfx::SetUiScale( g_s );
	if( !gfx::Begin( dev ) ) return;
	if( !g_assetsTried ){
		g_assetsTried = true;
		gfx::LoadImageFile( dev, config::BaseDir() + L"img\\logo.png", g_logo );
		gfx::CreateGlow( dev, g_glow );
		gfx::CreateGear( dev, g_gear );
		gfx::CreateDisc( dev, g_disc );
	}
	g_msgHits.clear();
	g_msgActive = true;
	float W = frame.vpWidth, H = frame.vpHeight;
	gfx::Rect( 0, 0, W, H, Rgba( 0, 0, 0, 0.55f ) );
	float w = S( 440 ), pad = S( 28 );
	float th = WrapHeight( Font::Body, frame.text, w - pad * 2 );
	float h = S( 64 ) + th + S( 30 ) + S( 44 ) + S( 24 );
	float x = ( W - w ) * 0.5f, y = ( H - h ) * 0.5f;
	Panel( x, y, w, h );
	gfx::Rect( x, y, S( 3 ), h, GOLD );
	gfx::Text( Font::TitleSmall, frame.twoButtons ? L"CONFIRMAR" : L"AVISO", x + pad, y + S( 24 ), GOLD );
	WrapText( Font::Body, frame.text, x + pad, y + S( 60 ), w - pad * 2, TEXT );
	float bh = S( 44 ), by = y + h - bh - S( 22 );
	if( frame.twoButtons ){
		float bw = ( w - pad * 2 - S( 12 ) ) * 0.5f;
		MsgButton( L"CANCELAR", x + pad, by, bw, bh, false, MSG_CANCEL );
		MsgButton( L"CONFIRMAR", x + pad + bw + S( 12 ), by, bw, bh, true, MSG_OK2 );
	}else{
		float bw = S( 160 );
		MsgButton( L"OK", x + w - pad - bw, by, bw, bh, true, MSG_OK1 );
	}
	gfx::End();
}

void ui::DrawLogin( IDirect3DDevice9* dev, const LoginFrame& frame ){
	g_login = frame;
	g_s = std::min( 1.5f, std::max( 0.75f, frame.vpHeight / 900.0f ) );
	gfx::SetUiScale( g_s );
	if( !gfx::Begin( dev ) ){
		return;
	}
	g_iconDev = dev;
	if( !g_assetsTried ){
		g_assetsTried = true;
		gfx::LoadImageFile( dev, config::BaseDir() + L"img\\logo.png", g_logo );
		gfx::CreateGlow( dev, g_glow );
		gfx::CreateGear( dev, g_gear );
		gfx::CreateDisc( dev, g_disc );
	}

	// cliques: foco do campo e linha da lista
	bool down = input::IsLeftDown();
	if( g_loginPrevDown && !down && !frame.popupActive ){
		input::Point p = input::RealCursor();
		for( const Hit& h : g_loginHits ){
			if( !Inside( p, h.x, h.y, h.w, h.h ) ) continue;
			if( h.card == 0 || h.card == 1 ) g_loginFocus = h.card;
			if( h.card >= 100 ) g_serverRow = h.card - 100;
		}
	}
	g_loginPrevDown = down;
	g_loginHits.clear();

	float W = frame.vpWidth, H = frame.vpHeight;
	// leve escurecimento para o texto ler bem sobre o mapa
	gfx::RectH( 0, 0, W * 0.55f, H, Rgba( 4, 8, 16, 0.55f ), Rgba( 4, 8, 16, 0.0f ) );
	LoginBrand( W * 0.09f, H * 0.5f );
	if( frame.hasServers ){
		ServerPanel( W, H );
	}else if( frame.hasLogin ){
		LoginPanel( W, H );
	}
	if( frame.hasWait && !frame.hasServers ){
		gfx::Rect( 0, 0, W, H, Rgba( 0, 0, 0, 0.35f ) );
		WaitPanel( W, H );
		g_loginHits.clear();
	}
	gfx::Text( Font::Small, L"© O Codex", W - S( 16 ), H - S( 24 ), Rgba( 159, 178, 196, 0.5f ), gfx::Right );
	g_loginLayout = true;
	gfx::End();
}

void ui::DrawMake( IDirect3DDevice9* dev, const MakeFrame& frame ){
	g_make = frame;
	g_s = std::min( 1.5f, std::max( 0.75f, frame.vpHeight / 900.0f ) );
	gfx::SetUiScale( g_s );
	if( !gfx::Begin( dev ) ){
		return;
	}
	g_iconDev = dev;
	if( !g_assetsTried ){
		g_assetsTried = true;
		gfx::LoadImageFile( dev, config::BaseDir() + L"img\\logo.png", g_logo );
		gfx::CreateGlow( dev, g_glow );
		gfx::CreateGear( dev, g_gear );
		gfx::CreateDisc( dev, g_disc );
	}

	// clique: foco do campo de nome
	bool down = input::IsLeftDown();
	if( g_makePrevDown && !down && !frame.popupActive ){
		input::Point p = input::RealCursor();
		g_nameFocus = false;
		for( const Hit& h : g_makeHits ){
			if( Inside( p, h.x, h.y, h.w, h.h ) && h.point.x == MK_NAME.x && h.point.y == MK_NAME.y ) g_nameFocus = true;
		}
	}
	g_makePrevDown = down;
	g_makeHits.clear();

	client::MakeState st;
	client::ReadMakeState( st );
	float W = frame.vpWidth, H = frame.vpHeight;

	MakeTopBar();
	float top = S( 48 );
	float leftW = S( 300 ), rightW = S( 360 );
	OriginPanel( st, S( 16 ), top, leftW, S( 560 ) );
	AppearancePanel( st, W - S( 16 ) - rightW, top, rightW, H - top - S( 96 ) );

	// giro da prévia: setas ao lado do personagem
	float fx, fy, scale;
	if( scene::SlotToScreen( client::SelectedSlot(), fx, fy, scale ) ){
		float r = std::max( S( 70 ), 60.0f * scale ), ay = fy - 45.0f * scale;
		for( int i = 0; i < 2; i++ ){
			float cx = fx + ( i == 0 ? -r : r ), d = S( 34 );
			bool hover = Hovered( cx - d * 0.5f, ay - d * 0.5f, d, d );
			if( g_disc.tex ){
				gfx::DrawImage( g_disc, cx - d * 0.5f, ay - d * 0.5f, d, d, hover ? GOLD : Rgba( 240, 189, 69, 0.75f ) );
				gfx::DrawImage( g_disc, cx - d * 0.44f, ay - d * 0.44f, d * 0.88f, d * 0.88f, Rgba( 10, 18, 32, 0.95f ) );
			}
			gfx::Text( Font::Title, i == 0 ? L"‹" : L"›", cx, ay - gfx::LineHeight( Font::Title ) * 0.55f, hover ? GOLD_HOVER : GOLD, gfx::Center, 0.0f, false );
			MakeHit( cx - d * 0.5f, ay - d * 0.5f, d, d, i == 0 ? MK_ROT_L : MK_ROT_R );
		}
	}

	// botões
	float pbw = rightW, pbh = S( 60 );
	MakeButton( L"CRIAR", W - S( 16 ) - pbw, H - S( 16 ) - pbh, pbw, pbh, true, MK_CREATE );
	MakeButton( L"VOLTAR", S( 16 ), H - S( 16 ) - S( 36 ), S( 180 ), S( 36 ), false, MK_CANCEL );

	g_makeLayout = true;
	gfx::End();
}

void ui::DrawModeToggle( IDirect3DDevice9* dev, float vpWidth, float vpHeight ){
	g_s = std::min( 1.5f, std::max( 0.75f, vpHeight / 900.0f ) );
	gfx::SetUiScale( g_s );
	input::SetViewport( vpWidth, vpHeight );
	if( !gfx::Begin( dev ) ){
		return;
	}
	bool custom = config::CustomActive();
	const wchar_t* opts[2] = { L"PERSONALIZADA", L"ORIGINAL" };
	float pad = S( 10 ), h = S( 22 );
	float lw = 0.0f;
	float ow[2] = { gfx::TextWidth( Font::SmallBold, opts[0] ) + pad * 2, gfx::TextWidth( Font::SmallBold, opts[1] ) + pad * 2 };
	float w = lw + ow[0] + ow[1];
	// barra de cima, à esquerda do contador de personagens (mesma geometria da TopBar)
	float barH = S( 34 ), y = ( barH - h ) * 0.5f;
	float sairX = vpWidth - S( 16 ) - S( 64 );
	std::wstring count = std::to_wstring( chardata::Count() ) + L" / " + std::to_wstring( client::SlotsPerPage() ) + L" PERSONAGENS";
	float countX = sairX - S( 12 ) - ( gfx::TextWidth( Font::SmallBold, count ) + S( 24 ) );
	float x = countX - S( 12 ) - w;
	input::Point m = input::RealCursor();
	bool hover = m.x >= x && m.x < x + w && m.y >= y && m.y < y + h;

	gfx::Rect( x + S( 2 ), y + S( 3 ), w, h, Rgba( 0, 0, 0, 0.35f ) );
	gfx::Rect( x, y, w, h, PANEL_DEEP );
	gfx::Frame( x, y, w, h, hover ? Rgba( 240, 189, 69, 0.8f ) : BORDER );
	float ox = x + lw;
	for( int i = 0; i < 2; i++ ){
		bool active = ( i == 0 ) == custom;
		if( active ){
			gfx::Rect( ox + S( 2 ), y + S( 3 ), ow[i] - S( 4 ), h - S( 6 ), GOLD );
		}
		gfx::Text( Font::SmallBold, opts[i], ox + ow[i] * 0.5f, y + ( h - gfx::LineHeight( Font::SmallBold ) ) * 0.5f,
			active ? DARK_TEXT : ( hover ? TEXT : MUTED ), gfx::Center, 0.0f, !active );
		ox += ow[i];
	}
	gfx::End();
	input::SetToggleRect( x, y, w, h );
}

void ui::Warmup( IDirect3DDevice9* dev ){
	static int step = 0;
	if( !config::CustomActive() || step > 1 || !config::Feature( config::FEAT_SELECT ) ) return;
	g_iconDev = dev;
	if( step == 0 ){
		if( !g_assetsTried ){
			g_assetsTried = true;
			gfx::LoadImageFile( dev, config::BaseDir() + L"img\\logo.png", g_logo );
			gfx::CreateGlow( dev, g_glow );
			gfx::CreateGear( dev, g_gear );
			gfx::CreateDisc( dev, g_disc );
		}
		gfx::SharpShader( dev );
		step = 1;
		return;
	}
	// ícones de todas as classes (num quadro da tela de login)
	DWORD t0 = GetTickCount();
	int n = 0;
	for( const std::wstring& name : vfs::List( config::BaseDir() + L"icones\\" ) ){
		if( ClassIcon( _wtoi( name.c_str() ) ) ) n++;
	}
	logger::Write( "Interface pre-carregada: %d icones em %u ms", n, GetTickCount() - t0 );
	step = 2;
}

void ui::Init(){
	gfx::Init();
	input::SetRedirect( &ui::Redirect );
}

void ui::Draw( IDirect3DDevice9* dev, const NativeFrame& frame ){
	g_frame = frame;
	g_s = std::min( 1.5f, std::max( 0.75f, frame.vpHeight / 900.0f ) );
	gfx::SetUiScale( g_s );

	if( !gfx::Begin( dev ) ){
		return;
	}
	g_iconDev = dev;
	if( !g_assetsTried ){
		g_assetsTried = true;
		gfx::LoadImageFile( dev, config::BaseDir() + L"img\\logo.png", g_logo );
		gfx::CreateGlow( dev, g_glow );
		gfx::CreateGear( dev, g_gear );
		gfx::CreateDisc( dev, g_disc );
	}

	if( !g_favoritesLoaded ) LoadFavorites();
	HandleClick();
	g_hits.clear();
	g_plates.clear();
	for( CardView& cv : g_cards ){
		cv = CardView();
	}
	for( int i = 0; i < MAX_CARDS; i++ ){
		g_spriteTopPrev[i] = g_spriteTop[i];
		g_spriteTop[i] = 1e9f;
	}

	int perPage = std::max( 1, client::SlotsPerPage() );
	int count = std::min( client::SlotsOnPage(), MAX_CARDS );
	int index = client::IndexInPage();
	int absolute = client::SelectedSlot();
	int page = index >= 0 ? ( absolute - index ) / perPage : 0;
	const chardata::Character* selected = chardata::Get( absolute );

	float top = S( 34 ) + S( 14 ), margin = S( 16 );
	float listW = S( 360 ), playH = S( 62 );
	float listX = frame.vpWidth - margin - listW;
	float listH = frame.vpHeight - top - margin - playH - S( 14 );

	g_sceneClip = { 0, (LONG)S( 34 ), (LONG)( listX - S( 6 ) ), (LONG)frame.vpHeight };
	float infoH = InfoHeight( selected ) + ( selected != nullptr && selected->deleteDate != 0 ? S( 32 ) : 0.0f );
	g_infoRect = { (LONG)margin, (LONG)top, (LONG)( margin + S( 300 ) + S( 4 ) ), (LONG)( top + infoH + S( 6 ) ) };

	// Primeiro o que fica no mapa (atrás dos painéis)
	SceneCharacters( page, perPage, count, index );

	TopBar();
	InfoCard( selected, margin, top, S( 300 ) );
	SlotList( page, perPage, count, index, listX, top, listW, listH );

	// Botões: JOGAR embaixo da lista; criar/apagar no canto inferior esquerdo
	const config::Settings& cfg = config::Get();
	float playY = frame.vpHeight - margin - playH;
	if( selected != nullptr ){
		Button( L"JOGAR", listX, playY, listW, playH, 0, cfg.nativePlay );
	}else if( index >= 0 ){
		Button( L"CRIAR", listX, playY, listW, playH, 0, CardPoint( index ) );
	}

	int firstEmpty = -1;
	for( int i = 0; i < count; i++ ){
		if( chardata::Get( page * perPage + i ) == nullptr ){
			firstEmpty = i;
			break;
		}
	}
	float bh = S( 38 ), by = frame.vpHeight - margin - bh, bx = margin;
	if( firstEmpty >= 0 ){
		Button( L"CRIAR PERSONAGEM", bx, by, S( 180 ), bh, 1, CardPoint( firstEmpty ) );
		bx += S( 190 );
	}
	if( selected != nullptr && selected->deleteDate != 0 ){
		// Exclusão agendada: os botões nativos viram "Cancelar" e "Deletar"
		Button( L"CANCELAR EXCLUSÃO", bx, by, S( 180 ), bh, 1, cfg.nativeDelete );
		LocalButton( L"APAGAR DEFINITIVO", bx + S( 190 ), by, S( 170 ), bh, 2, OPEN_CONFIRM_FINAL );
	}else if( selected != nullptr ){
		LocalButton( L"APAGAR", bx, by, S( 120 ), bh, 2, OPEN_CONFIRM_RESERVE );
	}

	// ESC: abre (ou fecha) a janela de saída
	if( input::TakeEscape() && !frame.popupActive ){
		g_modal = g_modal == Modal::None ? Modal::ConfirmExit : Modal::None;
	}
	if( ( selected == nullptr && g_modal != Modal::ConfirmExit ) || frame.popupActive ){
		g_modal = Modal::None;
	}
	if( g_modal == Modal::ConfirmExit ){
		ExitModal();
	}else if( g_modal != Modal::None ){
		ConfirmModal( selected );
	}

	gfx::End();
	g_hasLayout = true;
}

namespace { void ReleaseQuestImages(); void ReleasePokedexImages(); }

void ui::OnDeviceLost(){
	gfx::Release( g_logo );
	gfx::Release( g_glow );
	gfx::Release( g_gear );
	gfx::Release( g_disc );
	for( gfx::Image& img : g_bars ) gfx::Release( img );
	g_barsTried = false;
	for( auto& kv : g_icons ) gfx::Release( kv.second );
	g_icons.clear();
	g_assetsTried = false;
	gfx::Image* zero[] = { &g_zero.gaugeBg, &g_zero.hpNormal, &g_zero.hpDamage, &g_zero.hpDamageDeco, &g_zero.hpDeco, &g_zero.sp,
		&g_zero.expBaseBg, &g_zero.expJobBg, &g_zero.expBase, &g_zero.expJob, &g_zero.fieldName };
	for( gfx::Image* img : zero ) gfx::Release( *img );
	g_zero.tried = false;
	for( auto& kv : g_zeroJobs ) gfx::Release( kv.second );
	g_zeroJobs.clear();
	for( auto& kv : g_zeroMenu ) gfx::Release( kv.second );
	g_zeroMenu.clear();
	ReleaseQuestImages();
	ReleasePokedexImages();
	gfx::OnDeviceLost();
}

// ======================================================================== quests (Zero)
namespace {
	// Fundo 592x435 (os 28 px da esquerda são das abas verticais, 28x358 em 1,24); lista 271x36 em 32,75;
	// detalhes em 312,25 (280x373); rodapé 398..435. Posições tiradas do UIRenewQuestWnd do cliente Zero.
	constexpr float QW = 592, QH = 435, Q_LIST_X = 32, Q_LIST_Y = 75, Q_ROW_W = 271, Q_ROW_H = 36, Q_LIST_BOTTOM = 397;
	constexpr float Q_INFO_X = 312, Q_INFO_Y = 25, Q_INFO_W = 272;
	const D3DCOLOR Q_TEXT = Rgba( 34, 34, 34, 1.0f ), Q_MUTED = Rgba( 110, 118, 134, 1.0f ), Q_NAVY = Rgba( 24, 44, 104, 1.0f );

	struct QuestText { std::wstring title, summary, desc; std::string icon; };
	std::map<int, QuestText> g_questText, g_newsText;
	bool g_questTextLoaded = false;
	std::map<std::wstring, gfx::Image> g_questImg; // img\zero\quest\<nome>.png

	enum QLocal { Q_NONE = 0, Q_TAB0, Q_TAB1, Q_TAB2, Q_ROW, Q_FILTER, Q_SEARCH, Q_SEND, Q_CLOSE, Q_TITLE, Q_UP, Q_DOWN };
	struct QHit { float x, y, w, h; int local; int value; };
	struct {
		int tab = 0;                 // 0 em andamento, 1 novidades, 2 em espera
		int selected = 0;            // quest (ou novidade) selecionada
		int filter = 0;              // 0 = todas; senão índice em Q_FILTERS
		float scroll = 0;
		std::wstring search;
		bool searchFocus = false, prevDown = false;
		std::vector<QHit> hits;
		float x = 0, y = 0;
		float nx = 0, ny = 0, nw = 0, nh = 0; // janela nativa (embaixo; parte dela sai da nossa)
		input::Point title, close, neutral; // pontos da janela nativa
		DWORD tick = 0;
	} g_q;
	struct QFilter { const char* icon; const wchar_t* name; };
	const QFilter Q_FILTERS[] = { { "", L"Todas" }, { "ico_ep", L"Episódio" }, { "ico_mq", L"Principal" }, { "ico_jq", L"Classe" },
		{ "ico_dq", L"Diária" }, { "ico_lq", L"Local" }, { "ico_ev", L"Evento" }, { "ico_gq", L"Guia" }, { "ico_nq", L"Outras" } };
	constexpr int Q_FILTER_COUNT = sizeof( Q_FILTERS ) / sizeof( Q_FILTERS[0] );

	std::wstring QuestUtf8( const std::string& s ){
		std::wstring w;
		int n = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0 );
		w.assign( n, L'\0' );
		if( n ) MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n );
		for( size_t i; ( i = w.find( L"\\n" ) ) != std::wstring::npos; ) w.replace( i, 2, L"\n" );
		return w;
	}

	void LoadQuestTable( const wchar_t* file, std::map<int, QuestText>& out ){
		std::vector<unsigned char> data;
		if( !vfs::Read( config::BaseDir() + file, data ) ) return;
		std::string all( data.begin(), data.end() );
		size_t p = 0;
		while( p < all.size() ){
			size_t e = all.find( '\n', p );
			if( e == std::string::npos ) e = all.size();
			std::string line = all.substr( p, e - p );
			p = e + 1;
			std::vector<std::string> f;
			for( size_t a = 0, t; ; a = t + 1 ){
				t = line.find( '\t', a );
				f.push_back( line.substr( a, t == std::string::npos ? std::string::npos : t - a ) );
				if( t == std::string::npos ) break;
			}
			if( f.size() < 5 ) continue;
			QuestText q;
			q.title = QuestUtf8( f[1] );
			q.icon = f[2].substr( 0, f[2].rfind( '.' ) );
			q.summary = QuestUtf8( f[3] );
			q.desc = QuestUtf8( f[4] );
			out[atoi( f[0].c_str() )] = q;
		}
	}

	void ReleaseQuestImages(){
		for( auto& kv : g_questImg ) gfx::Release( kv.second );
		g_questImg.clear();
	}

	const gfx::Image* QuestImage( IDirect3DDevice9* dev, const std::wstring& name ){
		auto it = g_questImg.find( name );
		if( it == g_questImg.end() ){
			gfx::Image& img = g_questImg[name];
			std::wstring path = config::BaseDir() + L"img\\zero\\quest\\" + name + L".png";
			if( vfs::Exists( path ) ) gfx::LoadImageFile( dev, path, img );
			return img.tex ? &img : nullptr;
		}
		return it->second.tex ? &it->second : nullptr;
	}

	void QImg( IDirect3DDevice9* dev, const std::wstring& name, float x, float y ){
		if( const gfx::Image* i = QuestImage( dev, name ) ) gfx::DrawImage( *i, x, y, (float)i->width, (float)i->height );
	}

	std::wstring Lower( std::wstring s ){
		for( wchar_t& c : s ) c = (wchar_t)towlower( c );
		return s;
	}

	std::wstring CodepageText( const std::string& s ){
		std::wstring w;
		int n = MultiByteToWideChar( config::Get().codepage, 0, s.data(), (int)s.size(), nullptr, 0 );
		w.assign( n, L'\0' );
		if( n ) MultiByteToWideChar( config::Get().codepage, 0, s.data(), (int)s.size(), &w[0], n );
		return w;
	}

	// Texto com as cores do cliente (^RRGGBB, ^000000 = cor base), quebrado em palavras dentro de w.
	// Tira as marcas de navegação (<NAVI>[Nome]<INFO>...</INFO></NAVI> vira "Nome"). Retorna a altura usada.
	float QuestWrap( const std::wstring& raw, float x, float y, float w, float maxY, D3DCOLOR base ){
		std::wstring s = raw;
		for( size_t a; ( a = s.find( L"<NAVI>" ) ) != std::wstring::npos; ){
			size_t b = s.find( L"</NAVI>", a );
			if( b == std::wstring::npos ) break;
			std::wstring in = s.substr( a + 6, b - a - 6 );
			size_t info = in.find( L"<INFO>" );
			if( info != std::wstring::npos ) in = in.substr( 0, info );
			if( in.size() >= 2 && in.front() == L'[' && in.back() == L']' ) in = in.substr( 1, in.size() - 2 );
			s.replace( a, b + 7 - a, in );
		}
		float lh = gfx::LineHeight( Font::ZeroSmall ) + 2, cx = x, cy = y;
		D3DCOLOR cur = base;
		std::wstring word;
		auto flush = [&](){
			if( word.empty() ) return;
			float ww = gfx::TextWidth( Font::ZeroSmall, word );
			if( cx > x && cx + ww > x + w ){ cx = x; cy += lh; }
			if( cy + lh <= maxY ) gfx::Text( Font::ZeroSmall, word, cx, cy, cur, gfx::Left, 0, false );
			cx += ww;
			word.clear();
		};
		for( size_t i = 0; i < s.size(); i++ ){
			wchar_t c = s[i];
			if( c == L'^' && i + 6 < s.size() && iswxdigit( s[i + 1] ) ){
				flush();
				unsigned rgb = (unsigned)wcstoul( s.substr( i + 1, 6 ).c_str(), nullptr, 16 );
				cur = rgb == 0 ? base : Rgba( ( rgb >> 16 ) & 255, ( rgb >> 8 ) & 255, rgb & 255, 1.0f );
				i += 6;
			}else if( c == L'\n' ){
				flush(); cx = x; cy += lh;
			}else if( c == L' ' ){
				word += c; flush();
			}else word += c;
		}
		flush();
		return cy + lh - y;
	}
}

void ui::DrawZeroQuestWindow( IDirect3DDevice9* dev, float nx, float ny, float nw, float nh ){
	g_s = 1.0f;
	gfx::SetUiScale( 1.0f );
	if( !gfx::Begin( dev ) ) return;
	if( !g_questTextLoaded ){
		g_questTextLoaded = true;
		LoadQuestTable( L"quests.txt", g_questText );
		LoadQuestTable( L"novidades.txt", g_newsText );
	}
	float x = nx, y = ny;
	if( GetTickCount() - g_q.tick > 1000 ){ g_q.searchFocus = false; input::SetTextCapture( false ); }
	g_q.tick = GetTickCount();
	g_q.x = x; g_q.y = y;
	g_q.nx = nx; g_q.ny = ny; g_q.nw = nw; g_q.nh = nh;
	g_q.title = { nx + 60, ny + 7 };
	g_q.neutral = { nx + 200, ny + 40 }; // topo da lista nativa (longe das bordas: lá o cursor vira o de redimensionar)
	client::WndRect cr;
	g_q.close = game::QuestCloseRect( cr ) ? input::Point{ cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f } : input::Point{ nx + nw - 40, ny + nh - 12 };
	input::Point m = input::RealCursor();

	// lista da aba atual (filtrada e buscada)
	struct Row { int id; std::wstring title, sub; std::string icon; const game::Quest* q; };
	std::vector<game::Quest> quests = game::Quests();
	std::vector<Row> rows;
	std::wstring needle = Lower( g_q.search );
	auto accept = [&]( const std::string& icon, const std::wstring& title ){
		if( g_q.filter > 0 ){
			const char* want = Q_FILTERS[g_q.filter].icon;
			bool known = false;
			for( int k = 1; k < Q_FILTER_COUNT - 1; k++ ) known = known || icon == Q_FILTERS[k].icon;
			if( strcmp( want, "ico_nq" ) == 0 ? known : icon != want ) return false;
		}
		return needle.empty() || Lower( title ).find( needle ) != std::wstring::npos;
	};
	if( g_q.tab == 1 ){
		for( const auto& kv : g_newsText ) if( accept( kv.second.icon, kv.second.title ) ) rows.push_back( { kv.first, kv.second.title, kv.second.summary, kv.second.icon, nullptr } );
	}else{
		for( const game::Quest& q : quests ){
			if( ( g_q.tab == 0 ) != ( q.state == 1 ) ) continue;
			auto t = g_questText.find( q.id );
			std::wstring title = t != g_questText.end() ? t->second.title : L"Missão " + std::to_wstring( q.id );
			std::string icon = t != g_questText.end() ? t->second.icon : "";
			if( !accept( icon, title ) ) continue;
			std::wstring sub = t != g_questText.end() ? t->second.summary : L"";
			if( !q.objectives.empty() ){
				int done = 0;
				for( const game::QuestObjective& o : q.objectives ) if( o.total > 0 && o.count >= o.total ) done++;
				sub = std::to_wstring( done ) + L"/" + std::to_wstring( q.objectives.size() ) + L" objetivos" + ( sub.empty() ? L"" : L" · " + sub );
			}
			rows.push_back( { q.id, title, sub, icon, nullptr } );
		}
	}
	for( Row& r : rows ) for( const game::Quest& q : quests ) if( q.id == r.id && g_q.tab != 1 ) r.q = &q;
	bool hasSel = false;
	for( const Row& r : rows ) hasSel = hasSel || r.id == g_q.selected;
	if( !hasSel ) g_q.selected = rows.empty() ? 0 : rows[0].id;

	// texto da busca
	if( g_q.searchFocus ){
		for( wchar_t c : input::TakeTyped() ){
			if( c == L'\b' ){ if( !g_q.search.empty() ) g_q.search.pop_back(); }
			else if( c == 13 || c == 27 ){ g_q.searchFocus = false; input::SetTextCapture( false ); }
			else if( c >= 32 && g_q.search.size() < 24 ) g_q.search += c;
		}
	}
	// cliques (ao soltar)
	bool down = input::IsLeftDown();
	float listH = Q_LIST_BOTTOM - Q_LIST_Y;
	float maxScroll = std::max( 0.0f, rows.size() * Q_ROW_H - listH );
	if( g_q.prevDown && !down ){
		bool onSearch = false;
		for( const QHit& h : g_q.hits ){
			if( !Inside( m, h.x, h.y, h.w, h.h ) ) continue;
			if( h.local >= Q_TAB0 && h.local <= Q_TAB2 ){ g_q.tab = h.local - Q_TAB0; g_q.scroll = 0; g_q.selected = 0; }
			else if( h.local == Q_ROW ) g_q.selected = h.value;
			else if( h.local == Q_FILTER ){ g_q.filter = ( g_q.filter + 1 ) % Q_FILTER_COUNT; g_q.scroll = 0; }
			else if( h.local == Q_SEARCH ) onSearch = true;
			else if( h.local == Q_SEND && g_q.selected > 0 ) game::SetQuestActive( g_q.selected, g_q.tab == 2 );
			else if( h.local == Q_UP ) g_q.scroll -= Q_ROW_H;
			else if( h.local == Q_DOWN ) g_q.scroll += Q_ROW_H;
		}
		if( onSearch != g_q.searchFocus ){ g_q.searchFocus = onSearch; input::SetTextCapture( onSearch ); }
	}
	g_q.prevDown = down;
	g_q.hits.clear();
	if( Inside( m, x + Q_LIST_X, y + Q_LIST_Y, Q_ROW_W, listH ) ) g_q.scroll -= input::TakeWheel() / 120.0f * Q_ROW_H;
	g_q.scroll = std::min( std::max( g_q.scroll, 0.0f ), maxScroll );

	// fundo, abas e título
	QImg( dev, L"bg", x, y );
	static const wchar_t* TABS[3] = { L"tab_active", L"tab_news", L"tab_inactive" };
	QImg( dev, TABS[g_q.tab], x + 1, y + 24 );
	for( int t = 0; t < 3; t++ ) g_q.hits.push_back( { x + 1, y + 24 + t * 119.0f, 28, 119, Q_TAB0 + t, 0 } );
	gfx::TextOutline( Font::ZeroName, L"Missões", x + 40, y + 5, Rgba( 255, 255, 255, 1.0f ), Rgba( 40, 70, 150, 1.0f ) );
	g_q.hits.push_back( { x + 28, y, QW - 28, 22, Q_TITLE, 0 } );

	// filtro (clique troca a categoria) e busca
	gfx::Text( Font::ZeroSmall, Q_FILTERS[g_q.filter].name, x + 40, y + 31, Q_TEXT, gfx::Left, 70, false );
	g_q.hits.push_back( { x + 35, y + 28, 96, 18, Q_FILTER, 0 } );
	std::wstring shown = g_q.search + ( g_q.searchFocus && ( GetTickCount() / 500 ) % 2 ? L"|" : L"" );
	if( shown.empty() && !g_q.searchFocus ) gfx::Text( Font::ZeroSmall, L"Buscar...", x + 139, y + 31, Q_MUTED, gfx::Left, 110, false );
	else gfx::Text( Font::ZeroSmall, shown, x + 139, y + 31, Q_TEXT, gfx::Left, 112, false );
	g_q.hits.push_back( { x + 134, y + 28, 146, 18, Q_SEARCH, 0 } );
	static const wchar_t* HEADERS[3] = { L"Missões em andamento", L"Novidades", L"Missões em espera" };
	gfx::Text( Font::ZeroSmall, HEADERS[g_q.tab] + std::wstring( L" (" ) + std::to_wstring( rows.size() ) + L")", x + 37, y + 56, Q_NAVY, gfx::Left, 236, false );

	// lista
	float ly = y + Q_LIST_Y - g_q.scroll;
	for( const Row& r : rows ){
		if( ly + Q_ROW_H > y + Q_LIST_Y && ly < y + Q_LIST_BOTTOM ){
			bool sel = r.id == g_q.selected;
			bool vis = ly >= y + Q_LIST_Y && ly + Q_ROW_H <= y + Q_LIST_BOTTOM;
			if( vis ){
				QImg( dev, sel ? L"img_quest_bg_selected" : L"img_quest_bg_unselected", x + Q_LIST_X, ly );
				std::wstring icon( r.icon.begin(), r.icon.end() );
				if( !icon.empty() ) QImg( dev, icon + ( g_q.tab == 2 ? L"_inactive" : L"" ), x + Q_LIST_X + 4, ly + 6 );
				gfx::Text( Font::ZeroSmall, r.title, x + Q_LIST_X + 36, ly + 4, sel ? Q_NAVY : Q_TEXT, gfx::Left, Q_ROW_W - 42, false );
				gfx::Text( Font::ZeroSmall, r.sub, x + Q_LIST_X + 36, ly + 19, Q_MUTED, gfx::Left, Q_ROW_W - 42, false );
				QImg( dev, L"img_quest_line", x + Q_LIST_X, ly + Q_ROW_H - 1 );
				g_q.hits.push_back( { x + Q_LIST_X, ly, Q_ROW_W, Q_ROW_H, Q_ROW, r.id } );
			}
		}
		ly += Q_ROW_H;
	}
	if( rows.empty() ) gfx::Text( Font::ZeroSmall, L"Nenhuma missão.", x + Q_LIST_X + Q_ROW_W * 0.5f, y + Q_LIST_Y + 14, Q_MUTED, gfx::Center, 0, false );
	if( maxScroll > 0 ){
		// barra de rolagem fina entre a lista e os detalhes
		float th = std::max( 20.0f, listH * listH / ( rows.size() * Q_ROW_H ) );
		float ty = y + Q_LIST_Y + ( listH - th ) * ( g_q.scroll / maxScroll );
		gfx::Rect( x + 304, ty, 5, th, Rgba( 150, 170, 210, 0.9f ) );
		g_q.hits.push_back( { x + 303, y + Q_LIST_Y, 8, ty - y - Q_LIST_Y, Q_UP, 0 } );
		g_q.hits.push_back( { x + 303, ty + th, 8, y + Q_LIST_BOTTOM - ty - th, Q_DOWN, 0 } );
	}

	// detalhes
	const Row* sel = nullptr;
	for( const Row& r : rows ) if( r.id == g_q.selected ) sel = &r;
	if( sel ){
		float dx = x + Q_INFO_X + 8, dw = Q_INFO_W - 18, dy = y + Q_INFO_Y + 9;
		gfx::Text( Font::ZeroName, sel->title, dx, dy, Q_NAVY, gfx::Left, dw, false );
		dy += 34;
		const std::map<int, QuestText>& table = g_q.tab == 1 ? g_newsText : g_questText;
		auto t = table.find( sel->id );
		float bottom = y + Q_LIST_BOTTOM - 4;
		if( sel->q && !sel->q->objectives.empty() ){
			gfx::Text( Font::ZeroSmall, L"Objetivos", dx, dy, Q_NAVY, gfx::Left, 0, false );
			dy += 15;
			for( const game::QuestObjective& o : sel->q->objectives ){
				bool ok = o.total > 0 && o.count >= o.total;
				gfx::Text( Font::ZeroSmall, CodepageText( o.name ), dx + 8, dy, ok ? Rgba( 40, 140, 60, 1.0f ) : Q_TEXT, gfx::Left, dw - 70, false );
				gfx::Text( Font::ZeroSmall, std::to_wstring( o.count ) + L" / " + std::to_wstring( o.total ), dx + dw, dy, ok ? Rgba( 40, 140, 60, 1.0f ) : Q_TEXT, gfx::Right, 0, false );
				dy += 15;
			}
			dy += 6;
		}
		if( sel->q && sel->q->end > 0 ){
			time_t end = (time_t)sel->q->end;
			struct tm lt;
			localtime_s( &lt, &end );
			wchar_t buf[64];
			wcsftime( buf, 64, L"Prazo: %d/%m %H:%M", &lt );
			gfx::Text( Font::ZeroSmall, buf, dx, dy, Rgba( 190, 60, 50, 1.0f ), gfx::Left, 0, false );
			dy += 18;
		}
		if( t != table.end() ){
			if( !t->second.summary.empty() && g_q.tab != 1 ){
				dy += QuestWrap( t->second.summary, dx, dy, dw, bottom, Q_NAVY ) + 6;
			}
			QuestWrap( t->second.desc, dx, dy, dw, bottom, Q_TEXT );
		}
	}

	// rodapé: mandar para andamento / espera
	if( g_q.tab != 1 ){
		std::wstring b = g_q.tab == 0 ? L"btn_inactive" : L"btn_active";
		float bx = x + QW - 96 - 12, by = y + QH - 32;
		bool can = sel != nullptr;
		bool hover = can && Hovered( bx, by, 96, 27 );
		QImg( dev, b + ( !can ? L"_disabled" : hover ? ( down ? L"_pressed" : L"_mouseon" ) : L"_normal" ), bx, by );
		if( can ) g_q.hits.push_back( { bx, by, 96, 27, Q_SEND, 0 } );
	}
	// fechar (no canto do título)
	{
		float bx = x + QW - 24, by = y + 4;
		bool hover = Hovered( bx, by, 16, 16 );
		QImg( dev, hover ? ( down ? L"btn_close_pressed" : L"btn_close_mouseon" ) : L"btn_close_normal", bx, by );
		g_q.hits.push_back( { bx, by, 16, 16, Q_CLOSE, 0 } );
	}
	gfx::End();
}

bool ui::ZeroQuestRedirect( const input::Point& real, input::Point& target ){
	if( GetTickCount() - g_q.tick > 250 ) return false;
	if( !Inside( real, g_q.x, g_q.y, QW, QH ) ){
		if( !Inside( real, g_q.nx, g_q.ny, g_q.nw, g_q.nh ) ) return false;
		target = g_q.neutral; // pedaço da nativa fora da nossa: controles invisíveis não recebem o clique
		return true;
	}
	for( auto it = g_q.hits.rbegin(); it != g_q.hits.rend(); ++it ){
		if( !Inside( real, it->x, it->y, it->w, it->h ) ) continue;
		if( it->local == Q_CLOSE ){ target = g_q.close; return true; }
		if( it->local == Q_TITLE ){ target = { g_q.title.x + ( real.x - it->x ) * 0.25f, g_q.title.y }; return true; } // arrastar
		break;
	}
	target = g_q.neutral; // o resto é nosso: o clique não vai para o mapa
	return true;
}

// ======================================================================== Pokédex (janela de pet)
namespace {
	void GameAssets( IDirect3DDevice9* dev ); // logo, brilho, engrenagem e disco (mais abaixo)

	// img\pokedex.png (tools\pokedex.py, 575x490: tela esquerda 287 + dobradiça + tela direita); posições iguais às do script
	constexpr float PDX_W = 575, PDX_H = 490, PDX_LW = 287;
	// tela direita: visor verde (área de texto) e controles
	constexpr float PDX_R = 303, PDX_LCD_X = PDX_R + 19, PDX_LCD_Y = 61, PDX_LCD_W = 234, PDX_LCD_H = 236;
	constexpr float PDX_SCR_X = 46, PDX_SCR_Y = 109, PDX_SCR_W = 208, PDX_SCR_H = 206;
	struct PetAnim { int frames = 0, w = 0, h = 0, ms = 100; gfx::Image strip; bool tried = false; };
	std::map<int, PetAnim> g_petAnims;
	std::map<int, std::string> g_petAnimInfo; // classe -> "quadros,l,a,ms" (pets_anim.txt)
	bool g_petInfoLoaded = false;
	gfx::Image g_pokedex;
	bool g_pokedexTried = false;

	enum PLocal { P_NONE = 0, P_FEED, P_TRICK, P_EGG, P_ACC, P_RENAME, P_CLOSE, P_TITLE, P_PREV, P_NEXT, P_ARENA, P_ELEM };
	struct PHit { float x, y, w, h; int local; };
	struct {
		std::vector<PHit> hits;
		float x = 0, y = 0, nx = 0, ny = 0, nw = 0, nh = 0;
		input::Point title, close, neutral;
		bool prevDown = false, renaming = false;
		std::wstring newName;
		std::wstring notice;
		DWORD noticeTick = 0, tick = 0;
		int page = 0;            // visor da direita: 0 arena, 1 elementos
		DWORD arenaAsked = 0;    // último pedido dos dados da arena
	} g_pdx;

	// elementos.txt (tools\elementos.py): tabela de dano por elemento e o elemento de cada monstro
	constexpr int ELE_N = 10;
	const wchar_t* const ELE_NAME[ELE_N] = { L"Neutro", L"Água", L"Terra", L"Fogo", L"Vento", L"Veneno", L"Sagrado", L"Sombrio", L"Fantasma", L"Maldito" };
	const D3DCOLOR ELE_COLOR[ELE_N] = { Rgba( 190, 190, 190, 1.0f ), Rgba( 50, 120, 230, 1.0f ), Rgba( 150, 95, 45, 1.0f ), Rgba( 230, 60, 35, 1.0f ),
		Rgba( 120, 230, 200, 1.0f ), Rgba( 150, 60, 190, 1.0f ), Rgba( 255, 235, 120, 1.0f ), Rgba( 50, 40, 70, 1.0f ),
		Rgba( 210, 180, 255, 1.0f ), Rgba( 90, 70, 60, 1.0f ) };
	int g_attr[4][ELE_N][ELE_N]; // [nível do alvo - 1][atacante][alvo] = dano %
	std::map<int, std::pair<int, int>> g_mobEle; // classe -> elemento, nível (ausente = Neutro 1)
	bool g_eleLoaded = false;

	void LoadElements(){
		if( g_eleLoaded ) return;
		g_eleLoaded = true;
		for( auto& a : g_attr ) for( auto& b : a ) for( int& v : b ) v = 100;
		std::vector<unsigned char> data;
		if( !vfs::Read( config::BaseDir() + L"elementos.txt", data ) ) return;
		std::string all( data.begin(), data.end() );
		for( size_t p = 0; p < all.size(); ){
			size_t e = all.find( '\n', p );
			if( e == std::string::npos ) e = all.size();
			std::string l = all.substr( p, e - p );
			p = e + 1;
			if( l.empty() || l[0] == ';' ) continue;
			int lv, atk, v[ELE_N];
			if( l.compare( 0, 4, "attr" ) == 0 ){
				if( sscanf_s( l.c_str(), "attr%d=%d:%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &lv, &atk, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9] ) == 12
					&& lv >= 1 && lv <= 4 && atk >= 0 && atk < ELE_N )
					for( int i = 0; i < ELE_N; i++ ) g_attr[lv - 1][atk][i] = v[i];
			}else{
				int id, ele;
				if( sscanf_s( l.c_str(), "%d=%d,%d", &id, &ele, &lv ) == 3 && ele >= 0 && ele < ELE_N && lv >= 1 && lv <= 4 ) g_mobEle[id] = { ele, lv };
			}
		}
	}

	const wchar_t* RankName( int r ){
		switch( r ){ case 1: return L"Bronze"; case 2: return L"Prata"; case 3: return L"Ouro"; case 4: return L"Platina"; default: return r >= 5 ? L"Diamante" : L"-"; }
	}

	void ReleasePokedexImages(){
		gfx::Release( g_pokedex );
		g_pokedexTried = false;
		for( auto& kv : g_petAnims ){ gfx::Release( kv.second.strip ); kv.second.tried = false; }
	}

	PetAnim* LoadPetAnim( IDirect3DDevice9* dev, int job ){
		if( !g_petInfoLoaded ){
			g_petInfoLoaded = true;
			std::vector<unsigned char> data;
			if( vfs::Read( config::BaseDir() + L"pets_anim.txt", data ) ){
				std::string all( data.begin(), data.end() );
				for( size_t p = 0; p < all.size(); ){
					size_t e = all.find( '\n', p );
					if( e == std::string::npos ) e = all.size();
					std::string l = all.substr( p, e - p );
					p = e + 1;
					size_t eq = l.find( '=' );
					if( l.empty() || l[0] == ';' || eq == std::string::npos ) continue;
					g_petAnimInfo[atoi( l.c_str() )] = l.substr( eq + 1 );
				}
			}
		}
		PetAnim& a = g_petAnims[job];
		if( !a.tried ){
			a.tried = true;
			auto it = g_petAnimInfo.find( job );
			if( it != g_petAnimInfo.end() && sscanf_s( it->second.c_str(), "%d,%d,%d,%d", &a.frames, &a.w, &a.h, &a.ms ) == 4 ){
				gfx::LoadImageFile( dev, config::BaseDir() + L"img\\pet\\anim\\" + std::to_wstring( job ) + L".png", a.strip );
			}
		}
		return a.strip.tex && a.frames > 0 ? &a : nullptr;
	}

	const wchar_t* HungerText( int h ){
		return h <= 10 ? L"Muito faminto" : h <= 25 ? L"Faminto" : h <= 75 ? L"Normal" : h <= 90 ? L"Satisfeito" : L"Cheio";
	}
	const wchar_t* LoyaltyText( int i ){
		return i <= 100 ? L"Estranho" : i <= 250 ? L"Tímido" : i <= 750 ? L"Neutro" : i <= 910 ? L"Cordial" : L"Leal";
	}

	// realce do controle sob o mouse (apertado = mais escuro)
	void PdxGlow( float x, float y, float w, float h, float r, bool hover, bool down ){
		if( hover ) gfx::RoundRect( x, y, w, h, r, down ? Rgba( 0, 0, 0, 0.25f ) : Rgba( 255, 255, 255, 0.28f ), g_disc );
	}

	void PdxLabel( const std::wstring& t, float x, float y, int align = gfx::Center ){
		gfx::TextOutline( Font::ZeroSmall, t, x, y, Rgba( 255, 255, 255, 1.0f ), Rgba( 110, 15, 20, 1.0f ), align );
	}
}

namespace {
	// visor verde da direita: atributos da Arena de Pets (página 0) ou fraquezas/forças do elemento (página 1)
	void DrawPokedexRight( float x, float y, const game::Pet& pet, bool down ){
		auto hov = [&]( float hx, float hy, float hw, float hh ){ return Hovered( x + hx, y + hy, hw, hh ); };
		const D3DCOLOR lcd = Rgba( 18, 70, 28, 1.0f ), dim = Rgba( 40, 110, 52, 1.0f ), line = Rgba( 50, 140, 65, 1.0f );
		float sx = x + PDX_LCD_X, sy = y + PDX_LCD_Y;
		DWORD now = GetTickCount();
		game::PetArena ar = game::CurrentPetArena();
		// pede os dados ao abrir, ao trocar de pet e a cada 15 s (vitórias mudam depois das batalhas)
		if( pet.valid && ( g_pdx.arenaAsked == 0 || ( ( ar.job != pet.job || now - ar.tick > 15000 ) && now - g_pdx.arenaAsked > 5000 ) ) ){
			g_pdx.arenaAsked = now;
			game::RequestPetArena();
		}
		auto title = [&]( const std::wstring& t, const std::wstring& right ){
			gfx::Text( Font::ZeroName, t, sx + 8, sy + 6, lcd, gfx::Left, 0, false );
			if( !right.empty() ) gfx::Text( Font::ZeroSmall, right, sx + PDX_LCD_W - 8, sy + 8, lcd, gfx::Right, 0, false );
			gfx::Rect( sx + 6, sy + 24, PDX_LCD_W - 12, 1, line );
		};
		auto center = [&]( const std::wstring& t, float yy ){ gfx::Text( Font::ZeroSmall, t, sx + PDX_LCD_W * 0.5f, yy, lcd, gfx::Center, PDX_LCD_W - 20, false ); };
		// par "rótulo ........ valor" em uma das duas colunas
		auto pair = [&]( int col, float yy, const std::wstring& label, const std::wstring& value ){
			float cx = sx + 8 + col * 116;
			gfx::Text( Font::ZeroSmall, label, cx, yy, dim, gfx::Left, 0, false );
			gfx::Text( Font::ZeroSmall, value, cx + 104, yy, lcd, gfx::Right, 0, false );
		};
		auto num = []( int v ){ return std::to_wstring( v ); };
		auto range = []( int a, int b ){ return std::to_wstring( a ) + L"~" + std::to_wstring( b ); };

		if( !pet.valid ){
			title( g_pdx.page == 0 ? L"ARENA DE PETS" : L"ELEMENTOS", L"" );
			center( L"Nenhum pet por perto.", sy + PDX_LCD_H * 0.5f - 6 );
		}else if( g_pdx.page == 0 ){
			bool ready = ar.valid && ar.job == pet.job;
			title( L"ARENA DE PETS", ready && ar.accepted ? std::wstring( RankName( ar.rank ) ) : L"" );
			if( !ready ) center( L"Carregando...", sy + PDX_LCD_H * 0.5f - 6 );
			else if( !ar.accepted ){
				center( L"Este pet não participa", sy + PDX_LCD_H * 0.5f - 14 );
				center( L"da Arena de Pets.", sy + PDX_LCD_H * 0.5f );
			}else{
				const float L = 15;
				float yy = sy + 31;
				pair( 0, yy, L"Nível", num( ar.level ) );       pair( 1, yy, L"Renasc.", num( ar.rebirths ) ); yy += L;
				pair( 0, yy, L"MMR", num( ar.mmr ) );           pair( 1, yy, L"Vitórias", num( ar.wins ) );    yy += L;
				pair( 1, yy, L"No tier", num( ar.tierWins ) );                                                 yy += L + 3;
				gfx::Rect( sx + 6, yy - 3, PDX_LCD_W - 12, 1, line );
				pair( 0, yy, L"FOR", num( ar.str ) );           pair( 1, yy, L"AGI", num( ar.agi ) );          yy += L;
				pair( 0, yy, L"VIT", num( ar.vit ) );           pair( 1, yy, L"INT", num( ar.int_ ) );         yy += L;
				pair( 0, yy, L"DES", num( ar.dex ) );           pair( 1, yy, L"SOR", num( ar.luk ) );          yy += L + 3;
				gfx::Rect( sx + 6, yy - 3, PDX_LCD_W - 12, 1, line );
				pair( 0, yy, L"HP", num( ar.hp ) );             pair( 1, yy, L"ASPD", num( ar.aspd ) );        yy += L;
				pair( 0, yy, L"ATQ", range( ar.atkMin, ar.atkMax ) ); pair( 1, yy, L"ATQM", range( ar.matkMin, ar.matkMax ) ); yy += L;
				pair( 0, yy, L"PRE", num( ar.hit ) );           pair( 1, yy, L"ESQ", num( ar.flee ) );         yy += L;
				pair( 0, yy, L"DEF", num( ar.def ) );           pair( 1, yy, L"DEFM", num( ar.mdef ) );
			}
		}else{
			LoadElements();
			auto it = g_mobEle.find( pet.job );
			int ele = it != g_mobEle.end() ? it->second.first : 0, lv = it != g_mobEle.end() ? it->second.second : 1;
			title( L"ELEMENTOS", ELE_NAME[ele] + std::wstring( L" " ) + std::to_wstring( lv ) );
			gfx::Rect( sx + PDX_LCD_W - 8 - gfx::TextWidth( Font::ZeroSmall, ELE_NAME[ele] + std::wstring( L" " ) + std::to_wstring( lv ) ) - 12, sy + 10, 8, 8, ELE_COLOR[ele] );
			// lista "elemento N%" em duas colunas
			float yy = sy + 30;
			auto list = [&]( const std::wstring& head, const std::vector<std::pair<int, int>>& items ){
				gfx::Text( Font::ZeroSmall, head, sx + 8, yy, lcd, gfx::Left, 0, false );
				yy += 15;
				if( items.empty() ){ gfx::Text( Font::ZeroSmall, L"Nenhum", sx + 16, yy, dim, gfx::Left, 0, false ); yy += 15; }
				for( size_t i = 0; i < items.size(); i++ ){
					float cx = sx + 16 + ( i % 2 ) * 110;
					gfx::Rect( cx, yy + 3, 8, 8, ELE_COLOR[items[i].first] );
					gfx::Text( Font::ZeroSmall, std::wstring( ELE_NAME[items[i].first] ) + L" " + std::to_wstring( items[i].second ) + L"%", cx + 12, yy, dim, gfx::Left, 0, false );
					if( i % 2 == 1 || i + 1 == items.size() ) yy += 14;
				}
				yy += 5;
			};
			std::vector<std::pair<int, int>> weak, resist, strong;
			for( int a = 0; a < ELE_N; a++ ){
				int v = g_attr[lv - 1][a][ele]; // dano que o pet recebe de um ataque do elemento a
				if( v > 100 ) weak.push_back( { a, v } );
				else if( v < 100 ) resist.push_back( { a, v } );
				int o = g_attr[0][ele][a];      // dano do ataque do pet num alvo nível 1 do elemento a
				if( o > 100 ) strong.push_back( { a, o } );
			}
			std::sort( weak.begin(), weak.end(), []( auto& p, auto& q ){ return p.second > q.second; } );
			std::sort( strong.begin(), strong.end(), []( auto& p, auto& q ){ return p.second > q.second; } );
			std::sort( resist.begin(), resist.end(), []( auto& p, auto& q ){ return p.second < q.second; } );
			list( L"Ponto fraco (recebe mais dano):", weak );
			list( L"Ponto forte (causa mais dano):", strong );
			list( L"Resistente a (recebe menos):", resist );
		}

		// setas e botões das páginas
		struct Btn { float x, y, w, h, r; int local; };
		const Btn btns[] = {
			{ PDX_R + 16, 338, 40, 26, 4, P_PREV }, { PDX_R + 62, 338, 40, 26, 4, P_NEXT },
			{ PDX_R + 16, 384, 116, 32, 5, P_ARENA }, { PDX_R + 140, 384, 116, 32, 5, P_ELEM },
		};
		for( const Btn& b : btns ){
			PdxGlow( x + b.x, y + b.y, b.w, b.h, b.r, hov( b.x, b.y, b.w, b.h ), down );
			g_pdx.hits.push_back( { x + b.x, y + b.y, b.w, b.h, b.local } );
		}
		auto tab = [&]( const wchar_t* t, float bx, bool on ){
			gfx::TextOutline( Font::ZeroSmall, t, x + bx + 58, y + 393, on ? Rgba( 255, 255, 255, 1.0f ) : Rgba( 255, 230, 200, 1.0f ),
				on ? Rgba( 120, 50, 10, 1.0f ) : Rgba( 170, 90, 30, 1.0f ), gfx::Center );
		};
		tab( L"ARENA", PDX_R + 16, g_pdx.page == 0 );
		tab( L"ELEMENTOS", PDX_R + 140, g_pdx.page == 1 );
		// LED da página atual aceso
		gfx::RoundRect( x + PDX_R + ( g_pdx.page == 0 ? 21 : 41 ), y + 313, 10, 10, 5, Rgba( 255, 255, 255, 0.55f ), g_disc );
	}
}

void ui::DrawPokedex( IDirect3DDevice9* dev, float nx, float ny, float nw, float nh ){
	g_s = 1.0f;
	gfx::SetUiScale( 1.0f );
	if( !gfx::Begin( dev ) ) return;
	GameAssets( dev );
	if( !g_pokedexTried ){
		g_pokedexTried = true;
		gfx::LoadImageFile( dev, config::BaseDir() + L"img\\pokedex.png", g_pokedex );
	}
	float x = nx, y = ny;
	if( GetTickCount() - g_pdx.tick > 1000 ){
		if( g_pdx.renaming ){ g_pdx.renaming = false; input::SetTextCapture( false ); }
		g_pdx.arenaAsked = 0; // janela reaberta: pede os dados da arena de novo
	}
	g_pdx.tick = GetTickCount();
	g_pdx.x = x; g_pdx.y = y; g_pdx.nx = nx; g_pdx.ny = ny; g_pdx.nw = nw; g_pdx.nh = nh;
	g_pdx.title = { nx + 60, ny + 6 };
	g_pdx.neutral = { nx + 50, ny + 80 }; // ilustração da nativa (longe das bordas e dos botões)
	client::WndRect cr;
	g_pdx.close = game::PetCloseRect( cr ) ? input::Point{ cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f } : input::Point{ nx + nw - 9, ny + 7 };
	game::Pet pet = game::CurrentPet();
	input::Point m = input::RealCursor();
	bool down = input::IsLeftDown();

	// renomear: texto digitado (Enter envia, Esc cancela)
	if( g_pdx.renaming ){
		for( wchar_t c : input::TakeTyped() ){
			if( c == L'\b' ){ if( !g_pdx.newName.empty() ) g_pdx.newName.pop_back(); }
			else if( c == 27 ){ g_pdx.renaming = false; input::SetTextCapture( false ); }
			else if( c == 13 ){
				g_pdx.renaming = false;
				input::SetTextCapture( false );
				if( !g_pdx.newName.empty() ){
					int n = WideCharToMultiByte( config::Get().codepage, 0, g_pdx.newName.c_str(), (int)g_pdx.newName.size(), nullptr, 0, nullptr, nullptr );
					std::string s( n, '\0' );
					WideCharToMultiByte( config::Get().codepage, 0, g_pdx.newName.c_str(), (int)g_pdx.newName.size(), &s[0], n, nullptr, nullptr );
					game::RenamePet( s );
				}
			}else if( c >= 32 && g_pdx.newName.size() < 23 ) g_pdx.newName += c;
		}
	}
	// cliques (ao soltar)
	if( g_pdx.prevDown && !down ){
		for( const PHit& h : g_pdx.hits ){
			if( !Inside( m, h.x, h.y, h.w, h.h ) ) continue;
			switch( h.local ){
				case P_FEED: game::PetCommand( 1 ); break;
				case P_TRICK: game::PetCommand( 2 ); break;
				case P_EGG: game::PetCommand( 3 ); break;
				case P_ACC:
					if( pet.accessory > 0 ) game::PetCommand( 4 );
					else{ g_pdx.notice = L"Sem acessório"; g_pdx.noticeTick = GetTickCount(); }
					break;
				case P_RENAME:
					if( pet.renamed ){ g_pdx.notice = L"O nome já foi trocado"; g_pdx.noticeTick = GetTickCount(); }
					else{ g_pdx.renaming = true; g_pdx.newName.clear(); input::SetTextCapture( true ); }
					break;
				case P_PREV: case P_NEXT: g_pdx.page ^= 1; break;
				case P_ARENA: g_pdx.page = 0; break;
				case P_ELEM: g_pdx.page = 1; break;
			}
		}
	}
	g_pdx.prevDown = down;
	g_pdx.hits.clear();

	if( g_pokedex.tex ) gfx::DrawImage( g_pokedex, x, y, PDX_W, PDX_H );
	auto hov = [&]( float hx, float hy, float hw, float hh ){ return Hovered( x + hx, y + hy, hw, hh ); };

	// tela: nome e nível em cima, o pet animado no meio
	float sx = x + PDX_SCR_X, sy = y + PDX_SCR_Y;
	if( pet.valid ){
		std::wstring name = CodepageText( pet.name );
		gfx::TextOutline( Font::ZeroName, name, sx + 8, sy + 6, Rgba( 255, 255, 255, 1.0f ), Rgba( 30, 30, 34, 1.0f ) );
		gfx::TextOutline( Font::ZeroSmall, L"Nv. " + std::to_wstring( pet.level ), sx + PDX_SCR_W - 8, sy + 8, Rgba( 255, 230, 120, 1.0f ), Rgba( 30, 30, 34, 1.0f ), gfx::Right );
		if( PetAnim* a = LoadPetAnim( dev, pet.job ) ){
			int f = (int)( GetTickCount() / (DWORD)std::max( 30, a->ms ) % (DWORD)a->frames );
			float areaH = PDX_SCR_H - 34, areaW = PDX_SCR_W - 16;
			float k = std::min( { 2.0f, areaW / a->w, areaH / a->h } ); // até 2x, cabendo na tela (pixel nítido)
			float dw = a->w * k, dh = a->h * k;
			float u0 = f / (float)a->frames, u1 = ( f + 1 ) / (float)a->frames;
			gfx::SetPointFilter( true );
			gfx::DrawImage( a->strip, floorf( sx + ( PDX_SCR_W - dw ) * 0.5f ), floorf( sy + 26 + ( areaH - dh ) * 0.5f ), dw, dh, 0xFFFFFFFF, u0, 0.0f, u1, 1.0f );
			gfx::SetPointFilter( false );
		}
		if( g_pdx.renaming ){
			gfx::Rect( sx + 10, sy + PDX_SCR_H - 46, PDX_SCR_W - 20, 36, Rgba( 20, 20, 24, 0.85f ) );
			gfx::Text( Font::ZeroSmall, L"Novo nome (Enter confirma, Esc cancela):", sx + 16, sy + PDX_SCR_H - 42, Rgba( 200, 200, 200, 1.0f ), gfx::Left, PDX_SCR_W - 30, false );
			gfx::Text( Font::ZeroName, g_pdx.newName + ( ( GetTickCount() / 500 ) % 2 ? L"|" : L"" ), sx + 16, sy + PDX_SCR_H - 27, Rgba( 255, 255, 255, 1.0f ), gfx::Left, PDX_SCR_W - 30, false );
		}else if( !g_pdx.notice.empty() && GetTickCount() - g_pdx.noticeTick < 2500 ){
			gfx::TextOutline( Font::ZeroSmall, g_pdx.notice, sx + PDX_SCR_W * 0.5f, sy + PDX_SCR_H - 20, Rgba( 255, 200, 120, 1.0f ), Rgba( 30, 30, 34, 1.0f ), gfx::Center );
		}
	}else{
		gfx::Text( Font::ZeroSmall, L"Nenhum pet por perto.", sx + PDX_SCR_W * 0.5f, sy + PDX_SCR_H * 0.5f - 6, Rgba( 210, 210, 210, 1.0f ), gfx::Center, 0, false );
	}

	// visor verde: fome e lealdade
	if( pet.valid ){
		const D3DCOLOR lcd = Rgba( 18, 70, 28, 1.0f );
		gfx::Text( Font::ZeroSmall, L"Fome: " + std::wstring( HungerText( pet.hunger ) ), x + 80, y + 413, lcd, gfx::Left, 94, false );
		gfx::Text( Font::ZeroSmall, L"Lealdade: " + std::wstring( LoyaltyText( pet.intimacy ) ), x + 80, y + 428, lcd, gfx::Left, 94, false );
		gfx::Rect( x + 80, y + 446, 92, 6, Rgba( 30, 110, 45, 1.0f ) );
		gfx::Rect( x + 80, y + 446, 92 * std::min( 1.0f, std::max( 0.0f, pet.intimacy / 1000.0f ) ), 6, lcd );
	}

	// controles
	struct Ctl { float x, y, w, h, r; int local; const wchar_t* label; float lx, ly; int align; };
	const Ctl ctls[] = {
		{ 72, 379, 48, 14, 7, P_FEED, L"Alimentar", 96, 363, gfx::Center },
		{ 130, 379, 48, 14, 7, P_TRICK, L"Brincar", 154, 363, gfx::Center },
		{ 26, 369, 38, 38, 19, P_EGG, L"Ovo", 45, 409, gfx::Center },
		{ 199, 381, 68, 68, 8, P_ACC, L"Acessório", 233, 452, gfx::Center },
		{ 61, 324, 26, 26, 13, P_RENAME, L"Renomear", 92, 331, gfx::Left },
	};
	for( const Ctl& c : ctls ){
		bool h = hov( c.x, c.y, c.w, c.h ) && pet.valid;
		PdxGlow( x + c.x, y + c.y, c.w, c.h, c.r, h, down );
		if( pet.valid ){
			if( c.local == P_RENAME ) gfx::TextOutline( Font::ZeroSmall, c.label, x + c.lx, y + c.ly, Rgba( 90, 90, 95, 1.0f ), Rgba( 240, 240, 240, 1.0f ) );
			else PdxLabel( c.label, x + c.lx, y + c.ly, c.align );
			g_pdx.hits.push_back( { x + c.x, y + c.y, c.w, c.h, c.local } );
		}
	}
	// lente azul = fechar; faixa de cima = arrastar
	PdxGlow( x + 23, y + 16, 48, 48, 24, hov( 23, 16, 48, 48 ), down );
	g_pdx.hits.push_back( { x + 23, y + 16, 48, 48, P_CLOSE } );
	g_pdx.hits.push_back( { x, y, PDX_LW, 86, P_TITLE } );
	DrawPokedexRight( x, y, pet, down );
	gfx::End();
}

bool ui::PokedexRedirect( const input::Point& real, input::Point& target ){
	if( GetTickCount() - g_pdx.tick > 250 ) return false;
	if( !Inside( real, g_pdx.x, g_pdx.y, PDX_W, PDX_H ) ){
		if( !Inside( real, g_pdx.nx, g_pdx.ny, g_pdx.nw, g_pdx.nh ) ) return false;
		target = g_pdx.neutral;
		return true;
	}
	for( const PHit& h : g_pdx.hits ){
		if( !Inside( real, h.x, h.y, h.w, h.h ) ) continue;
		if( h.local == P_CLOSE ){ target = g_pdx.close; return true; }
		if( h.local == P_TITLE ){ target = { g_pdx.title.x + ( real.x - h.x ) * 0.25f, g_pdx.title.y }; return true; }
		break;
	}
	target = g_pdx.neutral;
	return true;
}

namespace { bool GameWindowsRedirect( const input::Point& real, input::Point& target ); }

bool ui::Redirect( const input::Point& real, input::Point& target ){
	if( GameWindowsRedirect( real, target ) ) return true;
	{
		// ícones de buff que descemos (abaixo do nome do mapa): o cliente ainda testa o mouse no lugar original
		float dy;
		if( client::InGame() && d3dhooks::BuffIconAt( real.x, real.y, dy ) ){ target = { real.x, real.y - dy }; return true; }
	}
	{
		client::WndRect br;
		if( client::InGame() && game::BasicInfoRect( br ) && StatusHudRedirect( real, target, (float)br.x, (float)br.y ) ) return true;
		if( client::InGame() && ZeroMenuRedirect( real, target ) ) return true;
		if( client::InGame() && ZeroQuestRedirect( real, target ) ) return true;
		if( client::InGame() && PokedexRedirect( real, target ) ) return true;
	}
	if( !config::CustomActive() ){
		return false;
	}
	if( g_msgActive ){
		for( auto it = g_msgHits.rbegin(); it != g_msgHits.rend(); ++it ){
			if( Inside( real, it->x, it->y, it->w, it->h ) ){
				target = { it->point.x, it->point.y };
				return true;
			}
		}
		target = { g_msg.x + 20.0f, g_msg.y + 40.0f }; // dentro da mensagem, longe dos botões
		return true;
	}
	if( !client::InGame() && !client::IsSelectActive() && !client::IsMakeCharActive() && client::AnyLoginWindow() ){
		if( !config::Feature( config::FEAT_LOGIN ) ) return false;
		if( !g_loginLayout || g_login.popupActive || !config::Get().uiEnabled ){
			return false;
		}
		for( auto it = g_loginHits.rbegin(); it != g_loginHits.rend(); ++it ){
			if( Inside( real, it->x, it->y, it->w, it->h ) ){
				target = { it->point.x, it->point.y };
				return true;
			}
		}
		target = { 2.0f, 2.0f }; // fora dos controles: canto vazio
		return true;
	}
	if( client::IsMakeCharActive() && !client::IsSelectActive() ){
		if( !config::Feature( config::FEAT_MAKE ) ) return false;
		if( !g_makeLayout || g_make.popupActive || !config::Get().uiEnabled ){
			return false;
		}
		for( auto it = g_makeHits.rbegin(); it != g_makeHits.rend(); ++it ){
			if( Inside( real, it->x, it->y, it->w, it->h ) ){
				target = { g_make.originX + it->point.x, g_make.originY + it->point.y };
				return true;
			}
		}
		// fora dos controles: um ponto vazio da janela nativa (não fecha nada)
		target = { g_make.originX + 300.0f, g_make.originY + 360.0f };
		return true;
	}
	if( !client::IsSelectActive() || !g_hasLayout || g_frame.popupActive || !config::Get().uiEnabled || !config::Feature( config::FEAT_SELECT ) ){
		return false;
	}

	// Últimos adicionados ficam por cima (painéis e botões antes dos personagens do mapa)
	for( auto it = g_hits.rbegin(); it != g_hits.rend(); ++it ){
		const Hit& h = *it;
		if( !Inside( real, h.x, h.y, h.w, h.h ) ){
			continue;
		}
		if( h.action == Action::Local || h.action == Action::None ){
			break;
		}
		config::Point p = h.action == Action::Card ? CardPoint( h.card ) : h.point;
		if( p.x > -1000.0f ){
			target = { g_frame.originX + p.x, g_frame.originY + p.y };
			return true;
		}
	}

	// Fora dos controles: o cliente enxerga o mouse num canto vazio
	target = { 2.0f, 2.0f };
	return true;
}

int ui::CardAt( float x, float y ){
	int count = std::min( client::SlotsOnPage(), MAX_CARDS );
	for( int i = 0; i < count; i++ ){
		client::WndRect card;
		if( client::CardRect( i, card ) ){
			float x0 = g_frame.originX + card.x, y0 = g_frame.originY + card.y;
			if( x >= x0 && x < x0 + card.w && y >= y0 && y < y0 + card.h ){
				return i;
			}
		}
	}
	return -1;
}

void ui::NoteSprite( int card, float minY ){
	if( card >= 0 && card < MAX_CARDS ){
		g_spriteTop[card] = std::min( g_spriteTop[card], minY );
	}
}

void ui::DrawPlates( IDirect3DDevice9* dev ){
	if( g_plates.empty() || g_modal != Modal::None || !gfx::Begin( dev ) ){
		return;
	}
	for( const Plate& p : g_plates ){
		if( p.balloon ){
			float badge = p.ph, bh = p.lw;
			float cx = p.px + badge * 0.5f, cy = p.py + badge * 0.5f;
			float bx = cx, by = cy - bh * 0.5f, bw = p.pw - badge * 0.5f;
			// ponta
			float tx = std::min( std::max( p.tipX, bx + S( 18 ) ), bx + bw - S( 18 ) );
			gfx::Triangle( tx - S( 8 ), by + bh - 1, tx + S( 8 ), by + bh - 1, tx, by + bh + S( 9 ), Rgba( 240, 189, 69, 0.9f ) );
			gfx::Triangle( tx - S( 6 ), by + bh - 2, tx + S( 6 ), by + bh - 2, tx, by + bh + S( 6 ), Rgba( 10, 18, 32, 0.96f ) );
			// corpo
			gfx::RoundRect( bx - 1, by - 1, bw + 2, bh + 2, S( 8 ), Rgba( 240, 189, 69, 0.9f ), g_disc );
			gfx::RoundRect( bx, by, bw, bh, S( 7 ), Rgba( 10, 18, 32, 0.96f ), g_disc );
			float tx0 = bx + badge * 0.5f + S( 12 );
			gfx::Text( Font::Name, p.name, tx0, by + S( 3 ), GOLD_HOVER );
			gfx::Text( Font::Small, p.jobName, tx0, by + bh - gfx::LineHeight( Font::Small ) - S( 3 ), MUTED, gfx::Left, 0.0f, false );
			ClassBadge( cx, cy, badge, p.job, GOLD, Rgba( 10, 18, 32, 1.0f ), GOLD_HOVER );
			continue;
		}
		gfx::Rect( p.px, p.py, p.pw, p.ph, p.bg );
		gfx::Frame( p.px, p.py, p.pw, p.ph, p.frame );
		gfx::Rect( p.px, p.py, p.lw, p.ph, p.lvBg );
		gfx::Text( Font::SmallBold, p.lv, p.px + p.lw * 0.5f, p.py + ( p.ph - gfx::LineHeight( Font::SmallBold ) ) * 0.5f,
			p.lvText, gfx::Center, 0.0f, false );
		gfx::Text( Font::BodyBold, p.name, p.px + p.lw + S( 8 ), p.py + ( p.ph - gfx::LineHeight( Font::BodyBold ) ) * 0.5f, p.nameText );
	}
	gfx::End();
	g_plates.clear();
}

bool ui::SceneXform( int card, SpriteXform& out ){
	if( g_modal != Modal::None ){
		return false; // a confirmação fica por cima de tudo
	}
	if( !g_hasLayout || card < 0 || card >= MAX_CARDS || !g_cards[card].inScene || !Anchor( card, out.anchorX, out.anchorY ) ){
		return false;
	}
	const CardView& cv = g_cards[card];
	out.targetX = cv.feetX;
	out.targetY = cv.feetY;
	out.scale = cv.scale;
	out.brightness = card == client::IndexInPage() ? 1.0f : config::Get().dimOthers;
	out.order = cv.scale;
	out.alpha = NearFade( card ); // mais perto da câmera que o selecionado: translúcido
	out.clip = g_sceneClip;
	// Personagem passando por baixo da ficha: some suavemente conforme a parte coberta
	out.alpha *= 1.0f - std::min( 1.0f, UnderInfo( card ) * 2.5f );
	return out.alpha > 0.01f;
}

bool ui::ThumbXform( int card, SpriteXform& out ){
	if( g_modal != Modal::None ){
		return false;
	}
	if( !g_hasLayout || card < 0 || card >= MAX_CARDS || !g_cards[card].hasThumb || !Anchor( card, out.anchorX, out.anchorY ) ){
		return false;
	}
	const CardView& cv = g_cards[card];
	// Enquadra cabeça e tronco do mesmo jeito para qualquer tamanho de sprite
	float height = SpriteHeight( card );
	out.scale = cv.thumbSize * 1.6f / height * config::Get().thumbScale;
	out.targetX = cv.thumbX + cv.thumbSize * 0.5f;
	out.targetY = cv.thumbY + S( 3 ) + height * out.scale;
	out.brightness = 1.0f;
	out.clip = { (LONG)cv.thumbX + 1, (LONG)cv.thumbY + 1, (LONG)( cv.thumbX + cv.thumbSize ) - 1, (LONG)( cv.thumbY + cv.thumbSize ) - 1 };
	return true;
}

// ======================================================================== dentro do jogo
namespace {
	const D3DCOLOR HP_A = Rgba( 64, 214, 120, 1.0f ), HP_B = Rgba( 22, 150, 84, 1.0f );
	const D3DCOLOR SP_A = Rgba( 64, 170, 255, 1.0f ), SP_B = Rgba( 28, 96, 210, 1.0f );
	const D3DCOLOR EXP_A = Rgba( 255, 214, 102, 1.0f ), EXP_B = Rgba( 214, 150, 24, 1.0f );
	const D3DCOLOR JOB_A = Rgba( 124, 227, 255, 1.0f ), JOB_B = Rgba( 18, 150, 214, 1.0f );
	const D3DCOLOR UP = Rgba( 86, 222, 120, 1.0f ), DOWN = Rgba( 255, 92, 92, 1.0f );

	void GameAssets( IDirect3DDevice9* dev ){
		g_iconDev = dev;
		if( !g_assetsTried ){
			g_assetsTried = true;
			gfx::LoadImageFile( dev, config::BaseDir() + L"img\\logo.png", g_logo );
			gfx::CreateGlow( dev, g_glow );
			gfx::CreateGear( dev, g_gear );
			gfx::CreateDisc( dev, g_disc );
		}
	}

	std::wstring Percent( double v ){
		wchar_t b[32];
		swprintf( b, 32, v >= 99.95 || v <= 0.0 ? L"%.0f%%" : L"%.1f%%", v );
		std::wstring s = b;
		for( wchar_t& c : s ) if( c == L'.' ) c = L',';
		return s;
	}

}

namespace {
	const D3DCOLOR ZERO_WHITE = Rgba( 255, 255, 255, 1.0f );
	const D3DCOLOR ZERO_NAME_OUTLINE = 0xFF2B2B2B, ZERO_GAUGE_OUTLINE = 0xFF3F4B62, ZERO_EXP_OUTLINE = 0xFF10193C;
	const D3DCOLOR ZERO_JOB_TEXT = Rgba( 0, 240, 255, 1.0f );

	void ZeroAssets( IDirect3DDevice9* dev ){
		if( g_zero.tried ) return;
		g_zero.tried = true;
		struct { gfx::Image* img; const wchar_t* name; } files[] = {
			{ &g_zero.gaugeBg, L"gze_bg" }, { &g_zero.hpNormal, L"gze_hp_normal" }, { &g_zero.hpDamage, L"gze_hp_damage" },
			{ &g_zero.hpDamageDeco, L"gze_hp_damage_deco" }, { &g_zero.hpDeco, L"gze_hp_deco" }, { &g_zero.sp, L"gze_sp_normal" },
			{ &g_zero.expBaseBg, L"bg_exp_base" }, { &g_zero.expJobBg, L"bg_exp_job" }, { &g_zero.expBase, L"gze_exp_base" },
			{ &g_zero.expJob, L"gze_exp_job" }, { &g_zero.fieldName, L"bg_fieldname" },
		};
		for( auto& f : files ) gfx::LoadImageFile( dev, config::BaseDir() + L"img\\zero\\" + f.name + L".png", *f.img );
	}

	const gfx::Image* ZeroJobIcon( IDirect3DDevice9* dev, int job ){
		auto it = g_zeroJobs.find( job );
		if( it == g_zeroJobs.end() ){
			gfx::Image& img = g_zeroJobs[job];
			std::wstring path = config::BaseDir() + L"img\\zero\\job\\icon_job_" + std::to_wstring( job ) + L".png";
			if( vfs::Exists( path ) ) gfx::LoadImageFile( dev, path, img );
			return img.tex ? &img : nullptr;
		}
		return it->second.tex ? &it->second : nullptr;
	}

	// Parte esquerda da imagem, sem esticar (como a barra do cliente: largura proporcional ao valor)
	void ZeroCrop( const gfx::Image& img, float x, float y, int w ){
		if( img.tex == nullptr || w <= 0 ) return;
		w = std::min( w, (int)img.width );
		gfx::DrawImage( img, x, y, (float)w, (float)img.height, 0xFFFFFFFF, 0.0f, 0.0f, w / (float)img.width, 1.0f );
	}

	// Parte da imagem (sx0..sx1 em pixels) esticada em x..x+w
	void ZeroStretch( const gfx::Image& img, float x, float y, float w, float h, float sx0, float sx1, float sh ){
		if( img.tex == nullptr || w <= 0 ) return;
		gfx::DrawImage( img, x, y, w, h, 0xFFFFFFFF, sx0 / img.width, 0.0f, sx1 / img.width, sh / img.height );
	}

	double Ratio( long long cur, long long max ){
		if( max <= 0 ) return 0.0;
		double r = (double)cur / (double)max;
		return r < 0.0 ? 0.0 : ( r > 1.0 ? 1.0 : r );
	}

	// Linha de valores do medidor: atual (alinhado à direita em 134), " / máx" e porcentagem (à direita em 230)
	void ZeroGaugeLine( float x, float y, long long cur, long long max ){
		unsigned pct = (unsigned)( Ratio( cur, max ) * 100.0 );
		gfx::TextOutline( Font::ZeroSmall, std::to_wstring( cur ), x + 134, y, ZERO_WHITE, ZERO_GAUGE_OUTLINE, gfx::Right );
		gfx::TextOutline( Font::ZeroSmall, L" / " + std::to_wstring( max ), x + 135, y, ZERO_WHITE, ZERO_GAUGE_OUTLINE );
		gfx::TextOutline( Font::ZeroSmall, std::to_wstring( pct ) + L"%", x + 230, y, ZERO_WHITE, ZERO_GAUGE_OUTLINE, gfx::Right );
	}
}

void ui::DrawStatusHud( IDirect3DDevice9* dev, float x, float y, const game::Status& st ){
	g_s = 1.0f;
	gfx::SetUiScale( 1.0f );
	if( !gfx::Begin( dev ) ) return;
	GameAssets( dev );
	ZeroAssets( dev );
	g_hudRect = { x, y, 237.0f, 63.0f }; // janela do tema novo do Zero
	g_hudTick = GetTickCount();

	// Medidor (UIHpSpGage) em 43,21: fundo, HP em +3,0 e SP em +3,21
	float gx = x + 43, gy = y + 21;
	if( g_zero.gaugeBg.tex ) gfx::DrawImage( g_zero.gaugeBg, gx, gy, (float)g_zero.gaugeBg.width, (float)g_zero.gaugeBg.height );
	double hp = Ratio( st.hp, st.maxHp );
	if( hp > 0.25 ){
		ZeroCrop( g_zero.hpNormal, gx + 3, gy, (int)( g_zero.hpNormal.width * hp ) );
	}else{
		// HP baixo: barra vermelha; entre 4% e 25% com o brilho na ponta
		int w = (int)( g_zero.hpDamage.width * hp );
		ZeroCrop( g_zero.hpDamage, gx + 3, gy, w );
		if( hp > 0.04 && w > 0 ){
			int dw = (int)( g_zero.hpDamageDeco.width * 0.04 );
			ZeroCrop( g_zero.hpDamageDeco, gx + 3 + w - dw, gy, dw );
		}
	}
	ZeroCrop( g_zero.sp, gx + 3, gy + 21, (int)( g_zero.sp.width * Ratio( st.sp, st.maxSp ) ) );
	if( g_zero.hpDeco.tex ) gfx::DrawImage( g_zero.hpDeco, x + 88, y + 35, (float)g_zero.hpDeco.width, (float)g_zero.hpDeco.height );

	// Ícone grande da classe em 0,11 (por cima do medidor); sem ícone do Zero, o do Codex
	if( const gfx::Image* icon = ZeroJobIcon( dev, st.job ) ){
		gfx::DrawImage( *icon, x, y + 11, (float)icon->width, (float)icon->height );
	}else{
		ClassBadge( x + 26, y + 37, 44, st.job, Rgba( 159, 178, 196, 0.8f ), 0, 0 );
	}

	wchar_t name[96];
	swprintf( name, 96, L"Nv. %d %ls", st.baseLevel, st.name.c_str() );
	gfx::TextOutline( Font::ZeroName, name, x + 47, y + 5, ZERO_WHITE, ZERO_NAME_OUTLINE );
	ZeroGaugeLine( x, y + 25, st.hp, st.maxHp );
	ZeroGaugeLine( x, y + 42, st.sp, st.maxSp );
	gfx::End();
}

void ui::DrawZeroMapName( IDirect3DDevice9* dev, float screenW, const std::wstring& name ){
	if( name.empty() || !gfx::Begin( dev ) ) return;
	ZeroAssets( dev );
	float x = screenW - 145, y = 159, w = 125;
	if( g_zero.fieldName.tex ) gfx::DrawImage( g_zero.fieldName, x, y, (float)g_zero.fieldName.width, (float)g_zero.fieldName.height );
	// centrado; sombra preta 1 px à direita/abaixo e o nome em dourado (cor 0x3AEFFF do cliente = RGB FF EF 3A)
	std::wstring shown = name;
	while( shown.size() > 1 && gfx::TextWidth( Font::ZeroMedium, shown ) > w - 6 ) shown.pop_back();
	float tx = x + floorf( ( w - gfx::TextWidth( Font::ZeroMedium, shown ) ) * 0.5f );
	gfx::Text( Font::ZeroMedium, shown, tx + 1, y + 8, Rgba( 0, 0, 0, 1.0f ), gfx::Left, 0.0f, false );
	gfx::Text( Font::ZeroMedium, shown, tx, y + 7, Rgba( 255, 239, 58, 1.0f ), gfx::Left, 0.0f, false );
	gfx::End();
}

void ui::DrawExpBar( IDirect3DDevice9* dev, float screenW, float screenH, const game::Status& st ){
	if( !gfx::Begin( dev ) ) return;
	ZeroAssets( dev );
	const gfx::Image &bb = g_zero.expBaseBg, &bj = g_zero.expJobBg, &gb = g_zero.expBase, &gj = g_zero.expJob;
	if( bb.tex == nullptr || bj.tex == nullptr || gb.tex == nullptr || gj.tex == nullptr ){
		gfx::End();
		return;
	}
	// UIExpBar: largura min(1004, tela - 20), centrada, colada embaixo; base à esquerda, classe à direita
	float w = std::min( 1004.0f, screenW - 20.0f ), h = (float)bb.height;
	float x = floorf( ( screenW - w ) * 0.5f ), y = screenH - h;
	float half = floorf( w * 0.5f );
	float tab = (float)( bb.width - gb.width ); // aba do nível (ponta direita da base, esquerda da classe)
	float track = half - tab;
	ZeroStretch( bb, x, y, track, h, 0, (float)gb.width, h );
	ZeroStretch( bb, x + track, y, tab, h, (float)gb.width, (float)bb.width, h );
	double rb = Ratio( (long long)st.baseExp, (long long)st.baseNext );
	float fw = floorf( track * (float)rb );
	ZeroStretch( gb, x + track - fw, y + 8, fw, (float)gb.height, gb.width * (float)( 1.0 - rb ), (float)gb.width, (float)gb.height );

	float jtab = (float)( bj.width - gj.width ), jtrack = w - half - jtab;
	ZeroStretch( bj, x + half, y, jtab, h, 0, jtab, h );
	ZeroStretch( bj, x + half + jtab, y, jtrack, h, jtab, (float)bj.width, h );
	double rj = Ratio( (long long)st.jobExp, (long long)st.jobNext );
	ZeroStretch( gj, x + half + jtab, y + 8, floorf( jtrack * (float)rj ), (float)gj.height, 0, gj.width * (float)rj, (float)gj.height );

	gfx::TextOutline( Font::ZeroSmall, L"Nv. base. " + std::to_wstring( st.baseLevel ), x + half - 70, y + 3, ZERO_WHITE, ZERO_EXP_OUTLINE );
	gfx::TextOutline( Font::ZeroSmall, L"Nv. classe. " + std::to_wstring( st.jobLevel ), x + half + 10, y + 3, ZERO_JOB_TEXT, ZERO_EXP_OUTLINE );
	gfx::End();
}

namespace {
	// Ícones do menu do Zero, na ordem dele; rótulos da msgstringtable do cliente.
	// Só entram os que existem no menu nativo (é ele que recebe o clique).
	struct ZeroMenuEntry { const char* name; const wchar_t* label; int legacy = 0; }; // legacy: botão antigo do Alt+V
	const ZeroMenuEntry ZERO_MENU[] = {
		{ "status", L"Atributos" }, { "equip", L"Equipamentos" }, { "item", L"Inventário" }, { "skill", L"Habilidades" },
		{ "party", L"Grupo", game::LEGACY_PARTY }, { "guild", L"Clã", game::LEGACY_GUILD }, { "quest", L"Lista de missões" },
		{ "map", L"Mapa-Múndi", game::LEGACY_MAP },
		{ "navigation", L"Navegação" }, { "option", L"Opção" }, { "bank", L"Banco" }, { "rec", L"Replay" },
		{ "mail", L"RODEX" }, { "achievement", L"Conquistas" }, { "tip", L"Dicas" }, { "shop", L"Loja de dinheiro" },
		{ "keyboard", L"Guia de teclas de atalho" }, { "attendance", L"Logue e Ganhe" },
		{ "adventureragency", L"Adventurer's Brokerage" }, { "repute", L"Reputação" }, { "probability", L"Ver chances" },
		{ "booking", L"Recrutar" },
	};
	// Tema novo do Zero: fechado = só o botão MENU (70x62); aberto = 372x343 com o painel, grade de 5 colunas
	// (células 71x63 a partir de 2,13; imagem 45x41 centrada, rótulo em y+35) e o botão no canto inferior direito.
	constexpr float MENU_BTN_W = 70, MENU_BTN_H = 62, MENU_OPEN_W = 372, MENU_OPEN_H = 343, MENU_MARGIN = 11;
	constexpr float MENU_CELL_W = 71, MENU_CELL_H = 63, MENU_GRID_X = 2, MENU_GRID_Y = 13;
	constexpr int MENU_COLS = 5;

	struct MenuHit { float x, y, w, h, tx, ty; int legacy = 0; };
	struct {
		bool prevDown = false;
		std::vector<MenuHit> hits;   // botão MENU e ícones -> centro do controle nativo
		float x = 0, y = 0, w = 0, h = 0;
		float bgx = 0, bgy = 0;      // fora dos controles: ponto vazio da janela nativa
		DWORD tick = 0;
	} g_menuUi;

	const gfx::Image* ZeroMenuImage( IDirect3DDevice9* dev, const std::wstring& name ){
		auto it = g_zeroMenu.find( name );
		if( it == g_zeroMenu.end() ){
			gfx::Image& img = g_zeroMenu[name];
			std::wstring path = config::BaseDir() + L"img\\zero\\menu\\" + name + L".png";
			if( vfs::Exists( path ) ) gfx::LoadImageFile( dev, path, img );
			return img.tex ? &img : nullptr;
		}
		return it->second.tex ? &it->second : nullptr;
	}

	std::string Lower( std::string s ){
		for( char& c : s ) c = (char)tolower( (unsigned char)c );
		return s;
	}

	// Rótulo centrado embaixo do ícone; o que não cabe na célula vai em duas linhas
	void ZeroMenuLabel( const std::wstring& text, float cx, float y ){
		const D3DCOLOR outline = 0xFF222222;
		if( gfx::TextWidth( Font::ZeroSmall, text ) <= MENU_CELL_W - 1 ){
			gfx::TextOutline( Font::ZeroSmall, text, cx, y, ZERO_WHITE, outline, gfx::Center );
			return;
		}
		size_t best = std::wstring::npos;
		for( size_t i = text.find( L' ' ); i != std::wstring::npos; i = text.find( L' ', i + 1 ) ){
			if( best == std::wstring::npos || abs( (int)i - (int)text.size() / 2 ) < abs( (int)best - (int)text.size() / 2 ) ) best = i;
		}
		if( best == std::wstring::npos ){
			gfx::TextOutline( Font::ZeroSmall, text, cx, y, ZERO_WHITE, outline, gfx::Center );
			return;
		}
		float lh = gfx::LineHeight( Font::ZeroSmall );
		gfx::TextOutline( Font::ZeroSmall, text.substr( 0, best ), cx, y, ZERO_WHITE, outline, gfx::Center );
		gfx::TextOutline( Font::ZeroSmall, text.substr( best + 1 ), cx, y + lh, ZERO_WHITE, outline, gfx::Center );
	}
}

void ui::ZeroMenuOrigin( bool open, float screenW, float screenH, float& x, float& y ){
	x = screenW - ( open ? MENU_OPEN_W : MENU_BTN_W ) - MENU_MARGIN;
	y = screenH - ( open ? MENU_OPEN_H : MENU_BTN_H ) - MENU_MARGIN;
}

void ui::DrawZeroMenu( IDirect3DDevice9* dev, const game::MenuState& m, float screenW, float screenH ){
	g_s = 1.0f;
	gfx::SetUiScale( 1.0f );
	if( !gfx::Begin( dev ) ) return;
	float x, y;
	ZeroMenuOrigin( m.open, screenW, screenH, x, y );
	bool down = input::IsLeftDown();
	// ícones sem botão no menu nativo (Mapa, Clã, Grupo): o clique é nosso (botão antigo do Alt+V)
	if( g_menuUi.prevDown && !down && GetTickCount() - g_menuUi.tick < 250 ){
		input::Point mp = input::RealCursor();
		for( const MenuHit& h : g_menuUi.hits ) if( h.legacy && Inside( mp, h.x, h.y, h.w, h.h ) ) game::LegacyBasicButton( h.legacy );
	}
	g_menuUi.prevDown = down;
	g_menuUi.hits.clear();
	g_menuUi.x = x; g_menuUi.y = y;
	g_menuUi.w = m.open ? MENU_OPEN_W : MENU_BTN_W;
	g_menuUi.h = m.open ? MENU_OPEN_H : MENU_BTN_H;
	// ponto vazio da janela nativa (nem o botão de abrir/fechar nem um ícone)
	g_menuUi.bgx = m.rect.x + 2.0f;
	g_menuUi.bgy = m.rect.y + 2.0f;
	auto inRect = []( float px, float py, const client::WndRect& r ){ return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h; };
	bool found = false;
	for( int py = m.rect.y + 2; py < m.rect.y + m.rect.h - 1 && !found; py += 4 ){
		for( int px = m.rect.x + 2; px < m.rect.x + m.rect.w - 1 && !found; px += 4 ){
			bool busy = inRect( (float)px, (float)py, m.toggle );
			for( const game::MenuButton& b : m.icons ) busy = busy || inRect( (float)px, (float)py, b.rect );
			if( !busy ){ g_menuUi.bgx = (float)px; g_menuUi.bgy = (float)py; found = true; }
		}
	}
	g_menuUi.tick = GetTickCount();

	if( m.open ){
		if( const gfx::Image* bg = ZeroMenuImage( dev, L"bg_menu" ) ) gfx::DrawImage( *bg, x, y, (float)bg->width, (float)bg->height );
		int slot = 0;
		for( const ZeroMenuEntry& e : ZERO_MENU ){
			const game::MenuButton* nb = nullptr;
			for( const game::MenuButton& b : m.icons ){
				std::string n = Lower( b.name );
				if( n == e.name || ( n == "status_doram" && strcmp( e.name, "status" ) == 0 ) ){ nb = &b; break; }
			}
			if( nb == nullptr && !e.legacy ) continue;
			float cx = x + MENU_GRID_X + ( slot % MENU_COLS ) * MENU_CELL_W, cy = y + MENU_GRID_Y + ( slot / MENU_COLS ) * MENU_CELL_H;
			slot++;
			bool hover = Hovered( cx, cy, MENU_CELL_W, MENU_CELL_H );
			std::wstring base = L"bt_" + std::wstring( e.name, e.name + strlen( e.name ) );
			const gfx::Image* img = nullptr;
			if( hover && down ) img = ZeroMenuImage( dev, base + L"_press" );
			if( img == nullptr && hover ) img = ZeroMenuImage( dev, base + L"_mouseon" );
			if( img == nullptr ) img = ZeroMenuImage( dev, base + L"_normal" );
			if( img ) gfx::DrawImage( *img, cx + floorf( ( MENU_CELL_W - img->width ) * 0.5f ), cy, (float)img->width, (float)img->height );
			ZeroMenuLabel( e.label, cx + ( MENU_CELL_W - 1 ) * 0.5f, cy + 35 );
			if( nb ) g_menuUi.hits.push_back( { cx, cy, MENU_CELL_W, MENU_CELL_H, nb->rect.x + nb->rect.w * 0.5f, nb->rect.y + nb->rect.h * 0.5f } );
			else g_menuUi.hits.push_back( { cx, cy, MENU_CELL_W, MENU_CELL_H, g_menuUi.bgx, g_menuUi.bgy, e.legacy } );
		}
	}
	// botão do menu (canto inferior direito da janela): logo do O Codex no lugar do Poring "MENU"
	float bx = x + g_menuUi.w - MENU_BTN_W, by = y + g_menuUi.h - MENU_BTN_H;
	bool hover = Hovered( bx, by, MENU_BTN_W, MENU_BTN_H );
	if( const gfx::Image* logo = ZeroMenuImage( dev, L"logo" ) ){
		float k = std::min( MENU_BTN_W / logo->width, MENU_BTN_H / logo->height ) * ( hover ? ( down ? 0.94f : 1.06f ) : 1.0f );
		float lw = logo->width * k, lh = logo->height * k;
		gfx::DrawImage( *logo, bx + ( MENU_BTN_W - lw ) * 0.5f, by + ( MENU_BTN_H - lh ) * 0.5f, lw, lh );
	}else if( const gfx::Image* btn = ZeroMenuImage( dev, hover ? ( down ? L"bt_menu_press" : L"bt_menu_over" ) : L"bt_menu_normal" ) ){
		gfx::DrawImage( *btn, bx, by, (float)btn->width, (float)btn->height );
	}
	// o botão nativo fica com a parte esquerda embaixo do nosso (o resto pode sair da tela)
	g_menuUi.hits.push_back( { bx, by, MENU_BTN_W, MENU_BTN_H, m.toggle.x + std::min( 6.0f, m.toggle.w * 0.5f ), m.toggle.y + m.toggle.h * 0.5f } );
	gfx::End();
}

bool ui::ZeroMenuRedirect( const input::Point& real, input::Point& target ){
	if( GetTickCount() - g_menuUi.tick > 250 || !Inside( real, g_menuUi.x, g_menuUi.y, g_menuUi.w, g_menuUi.h ) ) return false;
	for( const MenuHit& h : g_menuUi.hits ){
		if( Inside( real, h.x, h.y, h.w, h.h ) ){
			target = { h.tx, h.ty };
			return true;
		}
	}
	// painel fora dos ícones: dentro da janela nativa, longe dos controles (o clique não vai para o mapa)
	target = { g_menuUi.bgx, g_menuUi.bgy };
	return true;
}

void ui::DrawGageBars( IDirect3DDevice9* dev, float x, float y, float w, float h, int hp, int maxHp, int sp, int maxSp, bool withSp, float down ){
	if( !g_barsTried ){
		g_barsTried = true;
		const wchar_t* names[4] = { L"barra_hp_cheia", L"barra_hp_vazia", L"barra_sp_cheia", L"barra_sp_vazia" };
		for( int i = 0; i < 4; i++ ) gfx::LoadImageFile( dev, config::BaseDir() + L"img\\" + names[i] + L".png", g_bars[i] );
	}
	if( g_bars[0].tex == nullptr || g_bars[2].tex == nullptr ) return;
	if( !gfx::Begin( dev ) ) return;
	// Centrada no retângulo da nativa (posição oficial; o cliente muda o tamanho dela com o zoom).
	// Proporção das imagens vezes a espessura; a de SP encaixa embaixo da de HP (altura total = 1,8 barra)
	float scale = config::Get().playerBarScale;
	float bw = floorf( w * scale );
	float bh = bw * g_bars[0].height / (float)g_bars[0].width * config::Get().playerBarThickness;
	float total = withSp ? bh * 1.8f : bh;
	float bx = floorf( x + ( w - bw ) * 0.5f ), by = floorf( y + ( h - total ) * 0.5f + bh * down );
	auto bar = [&]( const gfx::Image& full, const gfx::Image& empty, int cur, int max, float top ){
		float r = max > 0 ? (float)cur / (float)max : 0.0f;
		r = r < 0.0f ? 0.0f : ( r > 1.0f ? 1.0f : r );
		if( empty.tex != nullptr ) gfx::DrawImage( empty, bx, top, bw, bh );
		if( r > 0.0f ) gfx::DrawImage( full, bx, top, bw * r, bh, 0xFFFFFFFF, 0.0f, 0.0f, r, 1.0f );
	};
	bar( g_bars[0], g_bars[1], hp, maxHp, by );
	if( withSp ) bar( g_bars[2], g_bars[3], sp, maxSp, by + bh * 0.8f );
	gfx::End();
}

bool ui::StatusHudRedirect( const input::Point& real, input::Point& target, float nativeX, float nativeY ){
	if( GetTickCount() - g_hudTick > 250 || !Inside( real, g_hudRect.x, g_hudRect.y, g_hudRect.w, g_hudRect.h ) ) return false;
	// arrastar pela barra move a janela nativa (que fica embaixo, escondida)
	target = { nativeX + std::min( std::max( real.x - g_hudRect.x, 4.0f ), 200.0f ), nativeY + std::min( real.y - g_hudRect.y, 12.0f ) };
	return true;
}

namespace {
	// Cores das descrições (^RRGGBB) adaptadas ao fundo escuro
	D3DCOLOR DescColor( unsigned rgb ){
		switch( rgb ){
			case 0x000000: return TEXT;
			case 0x0000FF: case 0x0000CC: case 0x0000BB: case 0x000088: return BLUE_SOFT;
			case 0x777777: case 0x666666: case 0x808080: return MUTED;
			case 0xFF0000: case 0xCC0000: case 0xFF3131: return Rgba( 255, 110, 110, 1.0f );
			case 0x009900: case 0x008000: case 0x00AA00: return UP;
		}
		int r = ( rgb >> 16 ) & 0xFF, g = ( rgb >> 8 ) & 0xFF, b = rgb & 0xFF;
		int lum = ( r * 3 + g * 6 + b ) / 10;
		if( lum < 120 ){ r = r + ( 255 - r ) / 2; g = g + ( 255 - g ) / 2; b = b + ( 255 - b ) / 2; }
		return Rgba( r, g, b, 1.0f );
	}

	struct Run { std::wstring text; D3DCOLOR color; };

	std::vector<Run> ParseRuns( const std::wstring& s, D3DCOLOR base ){
		std::vector<Run> runs;
		D3DCOLOR cur = base;
		std::wstring acc;
		for( size_t i = 0; i < s.size(); i++ ){
			if( s[i] == L'^' && i + 6 < s.size() && iswxdigit( s[i + 1] ) ){
				if( !acc.empty() ){ runs.push_back( { acc, cur } ); acc.clear(); }
				unsigned rgb = (unsigned)wcstoul( s.substr( i + 1, 6 ).c_str(), nullptr, 16 );
				cur = rgb == 0 ? base : DescColor( rgb );
				i += 6;
				continue;
			}
			acc += s[i];
		}
		if( !acc.empty() ) runs.push_back( { acc, cur } );
		return runs;
	}

	// Texto com cores, quebrado em palavras. Retorna a altura usada (sem desenhar se draw = false).
	float RichText( const std::wstring& line, float x, float y, float w, D3DCOLOR base, Font font, bool draw, float indent = 0 ){
		float lh = gfx::LineHeight( font );
		float cx = x, cy = y;
		for( const Run& r : ParseRuns( line, base ) ){
			size_t p = 0;
			while( p < r.text.size() ){
				size_t sp = r.text.find( L' ', p );
				std::wstring word = r.text.substr( p, sp == std::wstring::npos ? std::wstring::npos : sp - p + 1 );
				p = sp == std::wstring::npos ? r.text.size() : sp + 1;
				float ww = gfx::TextWidth( font, word );
				if( cx + ww > x + w && cx > x + indent ){
					cx = x + indent;
					cy += lh;
				}
				if( draw ) gfx::Text( font, word, cx, cy, r.color, gfx::Left, 0, false );
				cx += ww;
			}
		}
		return cy - y + lh;
	}

	bool IsSeparator( const std::wstring& s ){
		std::wstring t;
		for( size_t i = 0; i < s.size(); i++ ){
			if( s[i] == L'^' && i + 6 < s.size() ){ i += 6; continue; }
			if( s[i] != L' ' ) t += s[i];
		}
		return t.size() >= 3 && t.find_first_not_of( L"-_=" ) == std::wstring::npos;
	}

	// Comparação de um número: seta verde (melhor) ou vermelha (pior)
	void Arrow( float x, float y, int diff ){
		if( diff == 0 ) return;
		if( diff > 0 ) gfx::Triangle( x, y + 8, x + 8, y + 8, x + 4, y + 1, UP );
		else gfx::Triangle( x, y + 1, x + 8, y + 1, x + 4, y + 8, DOWN );
	}

	// ---------------------------------------------------------------- opções aleatórias (runas)
	struct OptionText { std::wstring tag, format; };
	std::map<int, OptionText> g_options;
	bool g_optionsLoaded = false;

	const OptionText* Option( int id ){
		if( !g_optionsLoaded ){
			g_optionsLoaded = true;
			std::vector<unsigned char> data;
			if( vfs::Read( config::BaseDir() + L"opcoes.txt", data ) ){
				std::string all( data.begin(), data.end() );
				size_t pos = 0;
				while( pos < all.size() ){
					size_t nl = all.find( '\n', pos );
					std::string line = all.substr( pos, nl == std::string::npos ? std::string::npos : nl - pos );
					pos = nl == std::string::npos ? all.size() : nl + 1;
					size_t eq = line.find( '=' ), bar = line.find( '|' );
					if( eq == std::string::npos || bar == std::string::npos ) continue;
					int n = MultiByteToWideChar( CP_UTF8, 0, line.c_str() + bar + 1, (int)( line.size() - bar - 1 ), nullptr, 0 );
					std::wstring fmt( n, L'\0' );
					MultiByteToWideChar( CP_UTF8, 0, line.c_str() + bar + 1, (int)( line.size() - bar - 1 ), &fmt[0], n );
					while( !fmt.empty() && ( fmt.back() == L'\r' ) ) fmt.pop_back();
					OptionText& o = g_options[atoi( line.c_str() )];
					o.tag = std::wstring( line.begin() + eq + 1, line.begin() + bar );
					o.format = fmt;
				}
			}
		}
		auto it = g_options.find( id );
		return it == g_options.end() ? nullptr : &it->second;
	}

	D3DCOLOR TagColor( const std::wstring& tag ){
		if( tag == L"DEF" ) return Rgba( 120, 140, 170, 1.0f );
		if( tag == L"ATK" || tag == L"MAT" || tag == L"CRI" ) return Rgba( 214, 96, 70, 1.0f );
		if( tag == L"RAC" ) return Rgba( 196, 82, 110, 1.0f );
		if( tag == L"SIZ" || tag == L"CLS" ) return Rgba( 150, 150, 70, 1.0f );
		if( tag == L"ELE" ) return Rgba( 64, 150, 214, 1.0f );
		if( tag == L"HP" || tag == L"CUR" ) return Rgba( 70, 170, 100, 1.0f );
		if( tag == L"SP" || tag == L"CST" ) return Rgba( 80, 120, 220, 1.0f );
		if( tag == L"ATR" ) return Rgba( 200, 150, 50, 1.0f );
		return Rgba( 140, 110, 190, 1.0f );
	}

	std::wstring OptionLine( const game::ItemOption& o, std::wstring& tag ){
		const OptionText* t = Option( o.id );
		std::wstring fmt = t ? t->format : L"Opção " + std::to_wstring( o.id ) + L" %d";
		tag = t ? t->tag : L"ESP";
		size_t p = fmt.find( L"%d" );
		if( p != std::wstring::npos ) fmt.replace( p, 2, std::to_wstring( o.value ) );
		return fmt;
	}

	// ---------------------------------------------------------------- janela do item (tema claro)
	namespace lt {
		const D3DCOLOR BG = Rgba( 246, 247, 252, 0.98f );
		const D3DCOLOR CARD = Rgba( 255, 255, 255, 1.0f );
		const D3DCOLOR LINE = Rgba( 214, 217, 234, 1.0f );
		const D3DCOLOR TITLE = Rgba( 38, 42, 78, 1.0f );
		const D3DCOLOR TEXT = Rgba( 52, 56, 82, 1.0f );
		const D3DCOLOR LABEL = Rgba( 92, 98, 124, 1.0f );
		const D3DCOLOR MUTED = Rgba( 140, 145, 166, 1.0f );
		const D3DCOLOR NAME = Rgba( 132, 88, 196, 1.0f );
		const D3DCOLOR REFINE = Rgba( 226, 132, 36, 1.0f );
		const D3DCOLOR ACCENT = Rgba( 92, 100, 180, 1.0f );
		const D3DCOLOR BLUE = Rgba( 52, 86, 200, 1.0f );
	}

	D3DCOLOR LightDescColor( unsigned rgb ){
		switch( rgb ){
			case 0x000000: return lt::TEXT;
			case 0x0000FF: case 0x0000CC: case 0x0000BB: case 0x000088: return lt::BLUE;
			case 0x777777: case 0x666666: case 0x808080: return lt::MUTED;
			case 0xFF0000: case 0xCC0000: case 0xFF3131: return Rgba( 206, 58, 58, 1.0f );
			case 0x009900: case 0x008000: case 0x00AA00: return Rgba( 36, 150, 80, 1.0f );
		}
		int r = ( rgb >> 16 ) & 0xFF, g = ( rgb >> 8 ) & 0xFF, b = rgb & 0xFF;
		int lum = ( r * 3 + g * 6 + b ) / 10;
		if( lum > 150 ){ r = r * 2 / 3; g = g * 2 / 3; b = b * 2 / 3; } // cores claras escurecem no fundo branco
		return Rgba( r, g, b, 1.0f );
	}

	// Texto com cores (^RRGGBB) para fundo claro, quebrado em palavras; retorna a altura
	float LightText( const std::wstring& line, float x, float y, float w, D3DCOLOR base, Font font, bool draw ){
		float lh = gfx::LineHeight( font );
		float cx = x, cy = y;
		D3DCOLOR cur = base;
		std::wstring word;
		auto flush = [&](){
			if( word.empty() ) return;
			float ww = gfx::TextWidth( font, word );
			if( cx + ww > x + w && cx > x ){ cx = x; cy += lh; }
			if( draw ) gfx::Text( font, word, cx, cy, cur, gfx::Left, 0, false );
			cx += ww;
			word.clear();
		};
		for( size_t i = 0; i < line.size(); i++ ){
			if( line[i] == L'^' && i + 6 < line.size() && iswxdigit( line[i + 1] ) ){
				flush();
				unsigned rgb = (unsigned)wcstoul( line.substr( i + 1, 6 ).c_str(), nullptr, 16 );
				cur = rgb == 0 ? base : LightDescColor( rgb );
				i += 6;
				continue;
			}
			word += line[i];
			if( line[i] == L' ' ) flush();
		}
		flush();
		return cy - y + lh;
	}

	std::wstring PlainText( const std::wstring& s ){
		std::wstring o;
		for( size_t i = 0; i < s.size(); i++ ){
			if( s[i] == L'^' && i + 6 < s.size() && iswxdigit( s[i + 1] ) ){ i += 6; continue; }
			o += s[i];
		}
		return o;
	}

	// itemextra.txt / monstros.txt (tools\itemextra.py)
	struct ItemExtra { long long price = 0; std::wstring flags; std::vector<std::pair<int, int>> drops; };
	std::map<uint32_t, ItemExtra> g_extra;
	std::map<int, std::wstring> g_mobNames;
	bool g_extraLoaded = false;

	void ReadLines( const wchar_t* file, const std::function<void( const std::string& )>& fn ){
		std::vector<unsigned char> data;
		if( !vfs::Read( config::BaseDir() + file, data ) ) return;
		std::string all( data.begin(), data.end() );
		size_t pos = 0;
		while( pos < all.size() ){
			size_t nl = all.find( '\n', pos );
			std::string line = all.substr( pos, nl == std::string::npos ? std::string::npos : nl - pos );
			pos = nl == std::string::npos ? all.size() : nl + 1;
			while( !line.empty() && line.back() == '\r' ) line.pop_back();
			if( !line.empty() ) fn( line );
		}
	}

	std::wstring Utf8( const std::string& s ){
		int n = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0 );
		std::wstring w( n, L'\0' );
		if( n ) MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n );
		return w;
	}

	const ItemExtra* Extra( uint32_t id ){
		if( !g_extraLoaded ){
			g_extraLoaded = true;
			ReadLines( L"itemextra.txt", []( const std::string& l ){
				size_t eq = l.find( '=' ), b1 = l.find( '|' ), b2 = l.find( '|', b1 + 1 );
				if( eq == std::string::npos || b1 == std::string::npos || b2 == std::string::npos ) return;
				ItemExtra& e = g_extra[(uint32_t)strtoul( l.c_str(), nullptr, 10 )];
				e.price = _atoi64( l.c_str() + eq + 1 );
				e.flags = Utf8( l.substr( b1 + 1, b2 - b1 - 1 ) );
				size_t p = b2 + 1;
				while( p < l.size() ){
					size_t c = l.find( ',', p );
					std::string part = l.substr( p, c == std::string::npos ? std::string::npos : c - p );
					size_t colon = part.find( ':' );
					if( colon != std::string::npos ) e.drops.push_back( { atoi( part.c_str() ), atoi( part.c_str() + colon + 1 ) } );
					p = c == std::string::npos ? l.size() : c + 1;
				}
			} );
			ReadLines( L"monstros.txt", []( const std::string& l ){
				size_t eq = l.find( '=' );
				if( eq != std::string::npos ) g_mobNames[atoi( l.c_str() )] = Utf8( l.substr( eq + 1 ) );
			} );
		}
		auto it = g_extra.find( id );
		return it == g_extra.end() ? nullptr : &it->second;
	}

	std::wstring Percent2( int rate ){ // rate do rAthena: 10000 = 100%
		wchar_t b[32];
		swprintf( b, 32, L"%d,%02d%%", rate / 100, rate % 100 );
		return b;
	}

	// Área clicável -> ponto da janela nativa (escondida) que recebe o clique
	struct ItemHit { float x, y, w, h; float nx, ny; };
	struct ItemUi {
		DWORD tick = 0;
		float x = 0, y = 0, w = 0, h = 0;      // janela desenhada
		float nx = 0, ny = 0, nw = 0, nh = 0;  // janela nativa
		std::vector<ItemHit> hits;
		unsigned seq = 0;
	} g_itemUi;
	struct { uint32_t id = 0; float offset = 0; } g_descScroll;

	void Section( const wchar_t* title, float x, float y, float w, float h ){
		gfx::RoundRect( x, y, w, h, 6, lt::CARD, g_disc );
		gfx::Frame( x, y, w, h, lt::LINE );
		if( title ) gfx::Text( Font::BodyBold, title, x + 14, y + 10, lt::TITLE, gfx::Left, 0, false );
	}

	void Medallion( float cx, float cy, float r, D3DCOLOR c, const std::wstring& letter ){
		gfx::Circle( cx, cy, r, c );
		gfx::Circle( cx, cy, r - 2, Rgba( 255, 255, 255, 0.25f ) );
		gfx::Circle( cx, cy, r - 3, c );
		gfx::Text( Font::SmallBold, letter, cx, cy - gfx::LineHeight( Font::SmallBold ) * 0.5f, Rgba( 255, 255, 255, 1.0f ), gfx::Center, 0, false );
	}

	void CardGlyph( float x, float y, float w, float h ){
		gfx::RoundRect( x, y, w, h, 3, Rgba( 70, 96, 170, 1.0f ), g_disc );
		gfx::RoundRect( x + 3, y + 3, w - 6, h - 6, 2, Rgba( 230, 236, 250, 1.0f ), g_disc );
		gfx::Circle( x + w * 0.5f, y + h * 0.45f, w * 0.22f, Rgba( 200, 150, 60, 1.0f ) );
		gfx::Rect( x + 6, y + h - 9, w - 12, 2, Rgba( 70, 96, 170, 0.6f ) );
	}
}

void ui::DrawItemPanels( IDirect3DDevice9* dev, const ItemPanels& p ){
	g_s = 1.0f;
	gfx::SetUiScale( 1.0f );
	if( !gfx::Begin( dev ) ) return;
	GameAssets( dev );
	const game::Item& it = p.item;
	itemdb::Entry e;
	itemdb::Get( it.id, e );
	const ItemExtra* ex = Extra( it.id );

	const float W = 730;
	// altura: o necessário para a descrição inteira (entre 440 e a tela)
	std::vector<game::ItemOption> opts;
	for( const game::ItemOption& o : it.options ) if( o.id != 0 ) opts.push_back( o );
	int filledCards = 0;
	for( int k = 0; k < 4; k++ ) if( it.cards[k] != 0 && it.cards[k] < 0xFF00 ) filledCards = k + 1;
	int cardRows = std::max( e.slots, filledCards );     // sem slot e sem carta: sem a seção
	float cardsH = cardRows > 0 ? 26 + cardRows * 44.0f : 0;
	float optsH = opts.empty() ? 0 : 24 + opts.size() * 22.0f;
	float descW = 388 - 28, descH = 0;
	for( const std::wstring& l : e.lines ) descH += IsSeparator( l ) ? 8 : LightText( l, 0, 0, descW, lt::TEXT, Font::Small, false ) + 2;
	float H = std::min( std::max( 440.0f, 76 + 168 + 12 + descH + 8 + optsH + cardsH + 12 ), p.vpHeight - 8 );
	float x = p.x, y = p.y;
	if( x + W > p.vpWidth - 4 ) x = std::max( 4.0f, p.vpWidth - 4 - W );
	if( y + H > p.vpHeight - 4 ) y = std::max( 4.0f, p.vpHeight - 4 - H );
	// não cobre as janelas grandes abertas: vai para o lado livre
	for( const BigRect& b : g_bigRects ){
		if( GetTickCount() - b.tick > 300 ) continue;
		if( x < b.x + b.w && x + W > b.x && y < b.y + b.h && y + H > b.y ){
			if( b.x - W - 8 >= 4 ) x = b.x - W - 8;
			else if( b.x + b.w + 8 + W <= p.vpWidth - 4 ) x = b.x + b.w + 8;
		}
	}
	g_itemUi.tick = GetTickCount();
	g_itemUi.seq = ++g_uiSeq;
	g_itemUi.x = x; g_itemUi.y = y; g_itemUi.w = W; g_itemUi.h = H;
	g_itemUi.nx = p.x; g_itemUi.ny = p.y; g_itemUi.nw = p.w; g_itemUi.nh = p.h;
	g_itemUi.hits.clear();
	input::Point m = input::RealCursor();

	// janela
	gfx::RoundRect( x + 3, y + 5, W, H, 12, Rgba( 20, 24, 50, 0.22f ), g_disc );
	gfx::RoundRect( x, y, W, H, 12, lt::BG, g_disc );
	// cabeçalho em aba
	gfx::RoundRect( x, y, 330, 46, 12, Rgba( 255, 255, 255, 1.0f ), g_disc );
	gfx::Triangle( x + 330, y, x + 330, y + 46, x + 372, y + 46, Rgba( 255, 255, 255, 1.0f ) );
	gfx::Rect( x + 12, y + 46, W - 24, 1, lt::LINE );
	// ícone do cabeçalho: pergaminho estilizado
	{
		float ix = x + 22, iy = y + 10;
		gfx::RoundRect( ix, iy + 3, 22, 26, 4, Rgba( 64, 72, 150, 1.0f ), g_disc );
		gfx::RoundRect( ix + 4, iy + 7, 14, 18, 2, Rgba( 226, 230, 250, 1.0f ), g_disc );
		gfx::Rect( ix + 7, iy + 12, 8, 2, Rgba( 64, 72, 150, 1.0f ) );
		gfx::Rect( ix + 7, iy + 17, 8, 2, Rgba( 64, 72, 150, 1.0f ) );
		gfx::Line( ix + 20, iy + 2, ix + 28, iy + 22, Rgba( 140, 100, 200, 1.0f ), 2.0f );
	}
	gfx::Text( Font::Name, L"DESCRIÇÃO DO ITEM", x + 58, y + 13, lt::TITLE, gfx::Left, 0, false );
	{ // fechar
		float cx = x + W - 26, cy = y + 23;
		bool hover = Inside( m, cx - 10, cy - 10, 20, 20 );
		D3DCOLOR c = hover ? lt::ACCENT : lt::LABEL;
		gfx::Line( cx - 6, cy - 6, cx + 6, cy + 6, c, 1.6f );
		gfx::Line( cx - 6, cy + 6, cx + 6, cy - 6, c, 1.6f );
		g_itemUi.hits.push_back( { cx - 12, cy - 12, 24, 24, p.x + 270, p.y + 7 } );
	}

	// ------------------------------------------------ cartão principal
	const float cx0 = x + 16, cy0 = y + 60, cw = 388, ch = H - 76;
	Section( nullptr, cx0, cy0, cw, ch );
	// ilustração
	float ix = cx0 + 14, iy = cy0 + 14, iw = 112, ih = 140;
	if( p.image ) p.image( it.id, ix, iy, iw, ih );
	// nome e refino
	std::wstring name = e.name.empty() ? L"Item " + std::to_wstring( it.id ) : e.name;
	float tx = ix + iw + 14, tw = cx0 + cw - 14 - tx;
	float rw = it.refine > 0 ? gfx::Text( Font::Name, L"+" + std::to_wstring( it.refine ), cx0 + cw - 14, cy0 + 16, lt::REFINE, gfx::Right, 0, false ) + 8 : 0;
	gfx::Text( Font::Name, name, tx, cy0 + 16, lt::NAME, gfx::Left, tw - rw, false );
	// ficha
	auto row = [&]( float ry, const wchar_t* label, const std::wstring& value ){
		gfx::Text( Font::Small, label, tx, ry, lt::LABEL, gfx::Left, 0, false );
		gfx::Text( Font::Small, value, tx + 108, ry, lt::TEXT, gfx::Left, tw - 108, false );
	};
	float ry = cy0 + 48, rh = 20;
	if( !e.type.empty() ){ row( ry, L"Tipo", e.type ); ry += rh; }
	if( !e.classes.empty() ){ row( ry, L"Classe", e.classes ); ry += rh; }
	if( e.requiredLevel > 0 ){ row( ry, L"Nv. necessário", std::to_wstring( e.requiredLevel ) ); ry += rh; }
	if( e.weight >= 0 ){ row( ry, L"Peso", std::to_wstring( e.weight ) ); ry += rh; }
	if( e.slots > 0 ){ row( ry, L"Slot", std::to_wstring( e.slots ) ); ry += rh; }

	// cartas e opções embaixo
	float bottom = cy0 + ch - 12;
	float cardsY = bottom - cardsH;
	float optsY = cardsY - optsH;

	// descrição (rolagem)
	float dx = cx0 + 14, dw = cw - 28;
	float dy0 = std::max( iy + ih, ry ) + 12, dy1 = optsY - 8;
	if( g_descScroll.id != it.id ){ g_descScroll.id = it.id; g_descScroll.offset = 0; }
	auto desc = [&]( bool draw, float top ) -> float {
		float cy = top;
		for( const std::wstring& l : e.lines ){
			if( IsSeparator( l ) ){ cy += 8; continue; }
			cy += LightText( l, dx, cy, dw, lt::TEXT, Font::Small, draw ) + 2;
		}
		return cy - top;
	};
	float total = desc( false, dy0 ), view = std::max( 10.0f, dy1 - dy0 );
	float maxOff = std::max( 0.0f, total - view );
	if( Inside( m, cx0, dy0, cw, view ) ) g_descScroll.offset -= input::TakeWheel() / 120.0f * 36.0f;
	g_descScroll.offset = std::min( maxOff, std::max( 0.0f, g_descScroll.offset ) );
	{
		RECT clip = { (LONG)cx0, (LONG)dy0, (LONG)( cx0 + cw ), (LONG)dy1 }, oldRect;
		DWORD oldScissor = FALSE;
		dev->GetRenderState( D3DRS_SCISSORTESTENABLE, &oldScissor );
		dev->GetScissorRect( &oldRect );
		dev->SetScissorRect( &clip );
		dev->SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE );
		desc( true, dy0 - g_descScroll.offset );
		dev->SetRenderState( D3DRS_SCISSORTESTENABLE, oldScissor );
		dev->SetScissorRect( &oldRect );
		if( maxOff > 0 ){
			float bh = std::max( 18.0f, view * view / total );
			float by = dy0 + ( view - bh ) * ( g_descScroll.offset / maxOff );
			gfx::RoundRect( cx0 + cw - 7, by, 4, bh, 2, Rgba( 92, 100, 180, 0.55f ), g_disc );
		}
	}

	// opções aleatórias (runas)
	if( !opts.empty() ){
		gfx::Text( Font::SmallBold, L"Opções", dx, optsY, lt::LABEL, gfx::Left, 0, false );
		float oy = optsY + 20;
		for( const game::ItemOption& o : opts ){
			std::wstring tag;
			std::wstring text = OptionLine( o, tag );
			D3DCOLOR c = TagColor( tag );
			gfx::RoundRect( dx, oy + 1, 34, 17, 8, c, g_disc );
			gfx::Text( Font::SmallBold, tag, dx + 17, oy + 1 + ( 17 - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, Rgba( 255, 255, 255, 1.0f ), gfx::Center, 0, false );
			gfx::Text( Font::Small, text, dx + 42, oy + 1, lt::TEXT, gfx::Left, dw - 42, false );
			oy += 22;
		}
	}

	// cartas (clicáveis: abrem a carta como na janela original)
	if( cardRows > 0 ) gfx::Text( Font::SmallBold, L"Cartas", dx, cardsY, lt::LABEL, gfx::Left, 0, false );
	float by = cardsY + 20;
	for( int k = 0; k < cardRows; k++, by += 44 ){
		uint32_t card = k < 4 ? it.cards[k] : 0;
		bool open = k < e.slots;
		bool filled = card != 0 && card < 0xFF00;
		bool hover = filled && Inside( m, dx, by, dw, 40 );
		gfx::RoundRect( dx, by, dw, 40, 6, hover ? Rgba( 240, 242, 252, 1.0f ) : Rgba( 250, 251, 255, 1.0f ), g_disc );
		gfx::Frame( dx, by, dw, 40, hover ? lt::ACCENT : lt::LINE );
		if( filled ){
			CardGlyph( dx + 10, by + 6, 22, 28 );
			itemdb::Entry ce;
			itemdb::Get( card, ce );
			gfx::Text( Font::SmallBold, ce.name.empty() ? L"Carta " + std::to_wstring( card ) : ce.name, dx + 44, by + 4, lt::TITLE, gfx::Left, dw - 52, false );
			std::wstring effect;
			for( const std::wstring& l : ce.lines ){ effect = PlainText( l ); if( !effect.empty() && !IsSeparator( l ) ) break; }
			gfx::Text( Font::Small, effect, dx + 44, by + 21, lt::TEXT, gfx::Left, dw - 52, false );
			g_itemUi.hits.push_back( { dx, by, dw, 40, p.x + 17 + 24.0f * k, p.y + 451 } );
		}else{
			gfx::RoundRect( dx + 10, by + 6, 22, 28, 3, Rgba( 226, 228, 240, 1.0f ), g_disc );
			gfx::Text( Font::Small, e.slots == 0 ? L"Este item não possui slots" : L"Slot vazio", dx + 44, by + 12, lt::MUTED, gfx::Left, 0, false );
		}
	}

	// ------------------------------------------------ coluna da direita
	const float sx = cx0 + cw + 14, sw = x + W - 16 - sx;
	float sy = cy0;
	// Informações
	{
		std::vector<std::wstring> info;
		std::wstring fl = ex ? ex->flags : L"";
		info.push_back( fl.find( L'R' ) != std::wstring::npos ? L"Pode ser refinado." : L"Não pode ser refinado." );
		if( !opts.empty() ) info.push_back( L"Possui opções encantadas." );
		for( const std::wstring& l : e.lines ){
			if( PlainText( l ).find( L"Indestrutível" ) != std::wstring::npos ){ info.push_back( L"Indestrutível em batalha." ); break; }
		}
		if( fl.find( L'T' ) != std::wstring::npos ) info.push_back( L"Não pode ser negociado." );
		if( fl.find( L'V' ) != std::wstring::npos ) info.push_back( L"Não pode ser vendido." );
		if( fl.find( L'D' ) != std::wstring::npos ) info.push_back( L"Não pode ser derrubado." );
		float h = 40 + info.size() * 20.0f + 6;
		Section( L"Informações", sx, sy, sw, h );
		float ly = sy + 38;
		for( const std::wstring& t : info ){ gfx::Text( Font::Small, t, sx + 14, ly, lt::TEXT, gfx::Left, sw - 28, false ); ly += 20; }
		sy += h + 12;
	}
	// Preço de referência
	{
		float h = cy0 + ch - sy;
		if( h > 60 ){
			Section( L"Preço de Referência", sx, sy, sw, h );
			float ly = sy + 40;
			gfx::Circle( sx + 22, ly + 8, 8, Rgba( 214, 160, 50, 1.0f ) );
			gfx::Circle( sx + 22, ly + 8, 5, Rgba( 240, 196, 90, 1.0f ) );
			long long price = ex ? ex->price : 0;
			gfx::Text( Font::BodyBold, price > 0 ? Thousands( price ) + L" zeny" : L"Sem preço de referência", sx + 38, ly, lt::TEXT, gfx::Left, sw - 50, false );
		}
	}
	gfx::End();
}

bool ui::ItemWindowRedirect( const input::Point& real, input::Point& target ){
	if( GetTickCount() - g_itemUi.tick > 250 ) return false;
	const ItemUi& u = g_itemUi;
	if( !Inside( real, u.x, u.y, u.w, u.h ) ) return false;
	for( const ItemHit& h : u.hits ){
		if( Inside( real, h.x, h.y, h.w, h.h ) ){ target = { h.nx, h.ny }; return true; }
	}
	float dx = real.x - u.x, dy = real.y - u.y;
	if( dy < 46 ){ // cabeçalho: arrasta a janela (barra de título nativa)
		target = { u.nx + std::min( std::max( dx, 20.0f ), u.nw - 40 ), u.ny + 9 };
		return true;
	}
	target = { u.nx + 120, u.ny + 220 }; // corpo da janela nativa: o clique não chega ao mapa
	return true;
}

// ======================================================================== ícones do cliente
namespace {
	const std::string UI_DIR = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA\\"; // 유저인터페이스\ (cp949)
	std::map<std::string, gfx::Image> g_clientImages; // tex == nullptr: não existe

	bool ClientImage( IDirect3DDevice9* dev, const std::string& path, gfx::Image& out ){
		auto it = g_clientImages.find( path );
		if( it == g_clientImages.end() ){
			gfx::Image img;
			std::vector<uint32_t> px;
			int w = 0, h = 0;
			if( game::ClientBitmap( path, px, w, h ) ) gfx::CreateImage( dev, w, h, px.data(), img );
			it = g_clientImages.emplace( path, img ).first;
		}
		out = it->second;
		return out.tex != nullptr;
	}
}

bool ui::ItemIcon( IDirect3DDevice9* dev, uint32_t id, gfx::Image& out ){
	itemdb::Entry e;
	if( !itemdb::Get( id, e ) || e.resource.empty() ) return false;
	return ClientImage( dev, UI_DIR + "item\\" + e.resource + ".bmp", out );
}

bool ui::ItemIllustration( IDirect3DDevice9* dev, uint32_t id, gfx::Image& out ){
	itemdb::Entry e;
	if( !itemdb::Get( id, e ) || e.resource.empty() ) return false;
	return ClientImage( dev, UI_DIR + "collection\\" + e.resource + ".bmp", out );
}

// ======================================================================== janela de equipamentos
namespace {
	struct EquipHit { float x, y, w, h; float nx, ny; int local; }; // local: ação nossa (0 = nenhuma)
	enum EquipLocal { EQ_NONE = 0, EQ_NAV0 = 1, EQ_STAT0 = 10, EQ_DETAILS = 20 };
	struct EquipUi {
		DWORD tick = 0;
		float x = 0, y = 0, w = 0, h = 0;
		float nx = 0, ny = 0, nw = 0, nh = 0;
		std::vector<EquipHit> hits;
		int nav = 0;               // 0 Equipamento, 1 Visual, 2 Título, 3 Carta
		bool prevDown = false;
		float charX = 0, charY = 0, charScale = 1;
		bool charOn = false;
		// área nativa embutida (Visual/Título)
		bool embed = false;
		float ex = 0, ey = 0, escale = 1;
		unsigned seq = 0;
	} g_eq;

	// Slots: posição do ícone na janela nativa (aba Equip)
	struct SlotDef { const wchar_t* name; uint32_t mask; float nx, ny; };
	const SlotDef LEFT_SLOTS[5] = {
		{ L"Topo", 0x100, 13, 45 }, { L"Meio", 0x200, 13, 72 }, { L"Baixo", 0x001, 262, 45 }, { L"Manto", 0x004, 13, 125 }, { L"Acessório", 0x008, 13, 152 } };
	const SlotDef RIGHT_SLOTS[5] = {
		{ L"Armadura", 0x010, 262, 72 }, { L"Arma", 0x002, 13, 99 }, { L"Escudo", 0x020, 262, 99 }, { L"Sapatos", 0x040, 262, 125 }, { L"Acessório", 0x080, 262, 152 } };

	void NavIcon( int k, float cx, float cy, D3DCOLOR c ){
		switch( k ){
			case 0: // espadas cruzadas
				gfx::Line( cx - 11, cy - 11, cx + 11, cy + 11, c, 3 );
				gfx::Line( cx + 11, cy - 11, cx - 11, cy + 11, c, 3 );
				gfx::Line( cx - 13, cy + 6, cx - 6, cy + 13, c, 2.5f );
				gfx::Line( cx + 13, cy + 6, cx + 6, cy + 13, c, 2.5f );
				break;
			case 1: // elmo
				gfx::Circle( cx, cy - 2, 11, c );
				gfx::Rect( cx - 11, cy - 2, 22, 12, c );
				gfx::Rect( cx - 7, cy + 1, 14, 3, Rgba( 255, 255, 255, 0.9f ) );
				break;
			case 2: // medalha
				gfx::Triangle( cx - 8, cy - 14, cx - 1, cy - 14, cx - 4, cy, c );
				gfx::Triangle( cx + 1, cy - 14, cx + 8, cy - 14, cx + 4, cy, c );
				gfx::Circle( cx, cy + 4, 9, c );
				gfx::Circle( cx, cy + 4, 5, Rgba( 255, 255, 255, 0.85f ) );
				break;
			default: // carta
				gfx::RoundRect( cx - 9, cy - 12, 18, 24, 3, c, g_disc );
				gfx::RoundRect( cx - 6, cy - 9, 12, 18, 2, Rgba( 255, 255, 255, 0.9f ), g_disc );
				gfx::Circle( cx, cy, 3.5f, c );
				break;
		}
	}

	void EquipSlot( IDirect3DDevice9* dev, const SlotDef& d, bool iconLeft, float x, float y, float w, float h, input::Point m ){
		game::Item it;
		bool has = game::EquippedAt( d.mask, it );
		bool hover = Inside( m, x, y, w, h );
		gfx::RoundRect( x, y, w, h, 6, hover ? Rgba( 246, 247, 255, 1.0f ) : lt::CARD, g_disc );
		gfx::Frame( x, y, w, h, hover ? lt::ACCENT : lt::LINE );
		float bs = h - 10, bx = iconLeft ? x + 5 : x + w - 5 - bs, by = y + 5;
		gfx::RoundRect( bx, by, bs, bs, 5, Rgba( 244, 245, 251, 1.0f ), g_disc );
		gfx::Frame( bx, by, bs, bs, Rgba( 228, 230, 242, 1.0f ) );
		float tx = iconLeft ? bx + bs + 8 : x + 8, tw = w - bs - 18;
		gfx::Text( Font::Small, d.name, tx, y + 6, lt::MUTED, gfx::Left, tw, false );
		if( has ){
			gfx::Image icon;
			if( ui::ItemIcon( dev, it.id, icon ) ) gfx::DrawImage( icon, bx + ( bs - 32 ) * 0.5f, by + ( bs - 32 ) * 0.5f, 32, 32 );
			if( it.refine > 0 ){
				gfx::Text( Font::Small, L"+" + std::to_wstring( it.refine ), bx + 3, by + 1, lt::REFINE, gfx::Left, 0, false );
				gfx::Text( Font::Small, L"+" + std::to_wstring( it.refine ), bx + bs - 3, by + bs - gfx::LineHeight( Font::Small ), lt::REFINE, gfx::Right, 0, false );
			}
			std::wstring name = itemdb::Name( it.id );
			if( name.empty() ) name = L"Item " + std::to_wstring( it.id );
			LightText( name, tx, y + 22, tw, lt::TEXT, Font::Small, true );
		}else{
			gfx::Text( Font::Small, L"Vazio", tx, y + 22, Rgba( 190, 194, 210, 1.0f ), gfx::Left, 0, false );
		}
		g_eq.hits.push_back( { x, y, w, h, g_eq.nx + d.nx, g_eq.ny + d.ny, EQ_NONE } );
	}

	void SmallButton( const wchar_t* text, float x, float y, float w, float h, input::Point m, bool icon = false ){
		bool hover = Inside( m, x, y, w, h );
		gfx::RoundRect( x, y, w, h, 5, hover ? Rgba( 232, 235, 250, 1.0f ) : Rgba( 244, 245, 252, 1.0f ), g_disc );
		gfx::Frame( x, y, w, h, hover ? lt::ACCENT : Rgba( 190, 196, 228, 1.0f ) );
		float tx = x + w * 0.5f;
		if( icon ){ // lupa
			gfx::Circle( x + 16, y + h * 0.5f - 1, 5, lt::ACCENT );
			gfx::Circle( x + 16, y + h * 0.5f - 1, 3.2f, hover ? Rgba( 232, 235, 250, 1.0f ) : Rgba( 244, 245, 252, 1.0f ) );
			gfx::Line( x + 19, y + h * 0.5f + 2, x + 23, y + h * 0.5f + 6, lt::ACCENT, 2 );
			tx += 8;
		}
		gfx::Text( Font::SmallBold, text, tx, y + ( h - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, lt::ACCENT, gfx::Center, 0, false );
	}
}

void ui::DrawEquipWindow( IDirect3DDevice9* dev, const EquipFrame& f ){
	g_s = 1.0f;
	gfx::SetUiScale( 1.0f );
	if( !gfx::Begin( dev ) ) return;
	GameAssets( dev );
	const float W = 676, H = 556;
	float x = f.nx, y = f.ny;
	if( x + W > f.vpWidth - 4 ) x = std::max( 4.0f, f.vpWidth - 4 - W );
	if( y + H > f.vpHeight - 4 ) y = std::max( 4.0f, f.vpHeight - 4 - H );
	if( GetTickCount() - g_eq.tick > 1000 ) g_eq.nav = 0; // janela reaberta
	g_eq.tick = GetTickCount();
	g_eq.seq = ++g_uiSeq;
	g_eq.x = x; g_eq.y = y; g_eq.w = W; g_eq.h = H;
	NoteBig( 0, x, y, W, H );
	g_eq.nx = f.nx; g_eq.ny = f.ny; g_eq.nw = f.nw; g_eq.nh = f.nh;
	input::Point m = input::RealCursor();

	// cliques nas nossas ações (no soltar do botão)
	bool down = input::IsLeftDown();
	if( g_eq.prevDown && !down ){
		for( const EquipHit& h : g_eq.hits ){
			if( !h.local || !Inside( m, h.x, h.y, h.w, h.h ) ) continue;
			if( h.local >= EQ_NAV0 && h.local < EQ_NAV0 + 4 ) g_eq.nav = h.local - EQ_NAV0;
			else if( h.local >= EQ_STAT0 && h.local < EQ_STAT0 + 6 ) game::StatusUp( h.local - EQ_STAT0 );
			else if( h.local == EQ_DETAILS ){ // Alt+A: janela de atributos do cliente
				INPUT in[4] = {};
				for( INPUT& i : in ) i.type = INPUT_KEYBOARD;
				in[0].ki.wVk = VK_MENU; in[1].ki.wVk = 'A';
				in[2].ki.wVk = 'A'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
				in[3].ki.wVk = VK_MENU; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
				SendInput( 4, in, sizeof( INPUT ) );
			}
		}
	}
	g_eq.prevDown = down;
	g_eq.hits.clear();
	g_eq.charOn = false;
	g_eq.embed = false;

	// janela e cabeçalho
	gfx::RoundRect( x + 3, y + 5, W, H, 12, Rgba( 20, 24, 50, 0.22f ), g_disc );
	gfx::RoundRect( x, y, W, H, 12, lt::BG, g_disc );
	gfx::RoundRect( x, y, 300, 46, 12, Rgba( 255, 255, 255, 1.0f ), g_disc );
	gfx::Triangle( x + 300, y, x + 300, y + 46, x + 342, y + 46, Rgba( 255, 255, 255, 1.0f ) );
	gfx::Rect( x + 120, y + 46, W - 132, 1, lt::LINE );
	{ // brasão
		float cx = x + 34, cy = y + 23;
		gfx::Circle( cx, cy, 15, Rgba( 64, 72, 150, 1.0f ) );
		gfx::Circle( cx, cy, 12, Rgba( 96, 108, 196, 1.0f ) );
		NavIcon( 0, cx, cy, Rgba( 255, 255, 255, 1.0f ) );
	}
	gfx::Text( Font::Name, L"EQUIPAMENTOS", x + 58, y + 13, lt::TITLE, gfx::Left, 0, false );
	{ // fechar
		float cx = x + W - 26, cy = y + 23;
		bool hover = Inside( m, cx - 10, cy - 10, 20, 20 );
		D3DCOLOR c = hover ? lt::ACCENT : lt::LABEL;
		gfx::Line( cx - 6, cy - 6, cx + 6, cy + 6, c, 1.6f );
		gfx::Line( cx - 6, cy + 6, cx + 6, cy - 6, c, 1.6f );
		g_eq.hits.push_back( { cx - 12, cy - 12, 24, 24, f.nx + 269, f.ny + 7, EQ_NONE } );
	}

	// navegação lateral
	const wchar_t* navs[4] = { L"Equipamento", L"Visual", L"Título", L"Carta" };
	const float navTab[4] = { 25, 91, 151, 25 }; // abas nativas (Carta usa a aba Equip)
	for( int k = 0; k < 4; k++ ){
		float ny = y + 56 + k * 74, nh = 70;
		bool sel = g_eq.nav == k, hover = Inside( m, x + 6, ny, 112, nh );
		if( sel ) gfx::RoundRect( x + 6, ny, 112, nh, 8, Rgba( 228, 231, 248, 1.0f ), g_disc );
		else if( hover ) gfx::RoundRect( x + 6, ny, 112, nh, 8, Rgba( 238, 240, 251, 1.0f ), g_disc );
		NavIcon( k, x + 62, ny + 24, sel ? lt::ACCENT : Rgba( 130, 136, 170, 1.0f ) );
		gfx::Text( Font::Small, navs[k], x + 62, ny + 44, sel ? lt::TITLE : lt::LABEL, gfx::Center, 0, false );
		g_eq.hits.push_back( { x + 6, ny, 112, nh, f.nx + navTab[k], f.ny + 27, EQ_NAV0 + k } );
	}
	if( g_logo.tex ) gfx::DrawImage( g_logo, x + 22, y + H - 110, 80, 80 * g_logo.height / (float)std::max( 1u, g_logo.width ), Rgba( 255, 255, 255, 0.35f ) );

	// conteúdo
	const float cx = x + 132, cw = W - 132 - 14, sy0 = y + 58;
	if( g_eq.nav == 0 ){
		const float sw = 168, sh = 52;
		for( int r = 0; r < 5; r++ ){
			EquipSlot( dev, LEFT_SLOTS[r], true, cx, sy0 + r * 58, sw, sh, m );
			EquipSlot( dev, RIGHT_SLOTS[r], false, cx + cw - sw, sy0 + r * 58, sw, sh, m );
		}
		// personagem sobre um círculo mágico
		float mid = cx + cw * 0.5f, cyc = sy0 + 128;
		gfx::Circle( mid, cyc, 92, Rgba( 236, 230, 214, 0.55f ) );
		gfx::Circle( mid, cyc, 88, lt::BG );
		gfx::Circle( mid, cyc, 74, Rgba( 240, 232, 214, 0.45f ) );
		gfx::Circle( mid, cyc, 70, lt::BG );
		for( int k = 0; k < 6; k++ ){
			float a = k * 3.14159f / 3.0f;
			gfx::Line( mid + cosf( a ) * 70, cyc + sinf( a ) * 70, mid + cosf( a + 2.094f ) * 70, cyc + sinf( a + 2.094f ) * 70, Rgba( 226, 214, 186, 0.6f ), 1.2f );
		}
		if( f.character.tex ){ // 50x136 da janela nativa, ampliado 1,5x
			float chw = 50 * 1.5f, chh = 136 * 1.5f;
			gfx::DrawImage( f.character, mid - chw * 0.5f, cyc + 92 - chh, chw, chh );
		}
		// Exibir Equipamento
		float chy = sy0 + 258;
		float chx = mid - 64;
		gfx::RoundRect( chx, chy, 14, 14, 3, f.showEquip ? lt::ACCENT : Rgba( 255, 255, 255, 1.0f ), g_disc );
		gfx::Frame( chx, chy, 14, 14, lt::ACCENT );
		if( f.showEquip ){
			gfx::Line( chx + 3, chy + 7, chx + 6, chy + 10, Rgba( 255, 255, 255, 1.0f ), 2 );
			gfx::Line( chx + 6, chy + 10, chx + 11, chy + 4, Rgba( 255, 255, 255, 1.0f ), 2 );
		}
		gfx::Text( Font::Small, L"Exibir Equipamento", chx + 20, chy - 1, lt::TEXT, gfx::Left, 0, false );
		g_eq.hits.push_back( { chx - 2, chy - 2, 140, 18, f.nx + 13, f.ny + 178, EQ_NONE } );
		// trocar equipamento (botão nativo de troca rápida), no cabeçalho
		float bx = x + W - 140, by = y + 12;
		SmallButton( L"Trocar equip.", bx, by, 92, 22, m );
		g_eq.hits.push_back( { bx, by, 92, 22, f.nx + 250, f.ny + 176, EQ_NONE } );
	}else if( g_eq.nav == 1 || g_eq.nav == 2 ){
		// a aba nativa correspondente, ampliada no espaço dos slots
		float srcY = 38, srcH = 132, scale = std::min( cw / 280.0f, 286.0f / srcH );
		float dw = 280 * scale, dh = srcH * scale, dx = cx + ( cw - dw ) * 0.5f, dy = sy0;
		gfx::RoundRect( dx - 4, dy - 4, dw + 8, dh + 8, 6, lt::CARD, g_disc );
		if( f.blitNative ) f.blitNative( 0, srcY, 280, srcH, dx, dy, dw, dh );
		gfx::Frame( dx - 4, dy - 4, dw + 8, dh + 8, lt::LINE );
		g_eq.embed = true;
		g_eq.ex = dx; g_eq.ey = dy - srcY * scale; g_eq.escale = scale;
	}else{
		// cartas nos equipamentos vestidos
		Section( L"Cartas equipadas", cx, sy0, cw, 286 );
		float ly = sy0 + 38;
		int n = 0;
		const SlotDef* all[10] = { &LEFT_SLOTS[0], &LEFT_SLOTS[1], &LEFT_SLOTS[2], &LEFT_SLOTS[3], &LEFT_SLOTS[4],
			&RIGHT_SLOTS[0], &RIGHT_SLOTS[1], &RIGHT_SLOTS[2], &RIGHT_SLOTS[3], &RIGHT_SLOTS[4] };
		for( const SlotDef* d : all ){
			game::Item it;
			if( !game::EquippedAt( d->mask, it ) ) continue;
			for( int k = 0; k < 4 && ly < sy0 + 270; k++ ){
				uint32_t card = it.cards[k];
				if( card == 0 || card >= 0xFF00 ) continue;
				gfx::Image icon;
				if( ui::ItemIcon( dev, card, icon ) ) gfx::DrawImage( icon, cx + 14, ly, 24, 24 );
				else CardGlyph( cx + 16, ly, 20, 24 );
				std::wstring cn = itemdb::Name( card );
				gfx::Text( Font::SmallBold, cn.empty() ? L"Carta " + std::to_wstring( card ) : cn, cx + 46, ly + 1, lt::TITLE, gfx::Left, 220, false );
				gfx::Text( Font::Small, std::wstring( d->name ) + L" · " + itemdb::Name( it.id ), cx + 280, ly + 2, lt::LABEL, gfx::Left, cw - 290, false );
				ly += 30; n++;
			}
		}
		if( n == 0 ) gfx::Text( Font::Small, L"Nenhuma carta nos equipamentos vestidos.", cx + 14, ly, lt::MUTED, gfx::Left, 0, false );
	}

	// Atributos e Informações
	const float py = sy0 + 300, ph = y + H - 14 - py;
	Section( nullptr, cx, py, cw, ph );
	const game::EquipStats& st = f.stats;
	gfx::Text( Font::BodyBold, L"Atributos", cx + 14, py + 10, lt::TITLE, gfx::Left, 0, false );
	gfx::Text( Font::BodyBold, L"Informações", cx + 232, py + 10, lt::TITLE, gfx::Left, 0, false );
	gfx::Rect( cx + 216, py + 14, 1, ph - 28, lt::LINE );
	const wchar_t* names[6] = { L"For", L"Agi", L"Vit", L"Int", L"Des", L"Sor" };
	for( int k = 0; k < 6; k++ ){
		float ry = py + 38 + k * 22;
		gfx::Text( Font::Small, names[k], cx + 14, ry, lt::TEXT, gfx::Left, 0, false );
		if( !f.hasStats ) continue;
		float bw = gfx::Text( Font::Small, std::to_wstring( st.base[k] ), cx + 56, ry, lt::TEXT, gfx::Left, 0, false );
		if( st.bonus[k] ) gfx::Text( Font::Small, ( st.bonus[k] > 0 ? L"+" : L"" ) + std::to_wstring( st.bonus[k] ), cx + 60 + bw, ry, lt::BLUE, gfx::Left, 0, false );
		if( st.cost[k] > 0 ) gfx::Text( Font::Small, std::to_wstring( st.cost[k] ), cx + 150, ry, lt::LABEL, gfx::Left, 0, false );
		if( st.cost[k] > 0 && st.points >= st.cost[k] ){
			float bx = cx + 180, by = ry + 1;
			bool hv = Inside( m, bx, by, 16, 16 );
			gfx::RoundRect( bx, by, 16, 16, 4, hv ? lt::ACCENT : Rgba( 228, 231, 248, 1.0f ), g_disc );
			gfx::Rect( bx + 4, by + 7, 8, 2, hv ? Rgba( 255, 255, 255, 1.0f ) : lt::ACCENT );
			gfx::Rect( bx + 7, by + 4, 2, 8, hv ? Rgba( 255, 255, 255, 1.0f ) : lt::ACCENT );
			g_eq.hits.push_back( { bx, by, 16, 16, f.nx + 140, f.ny + 100, EQ_STAT0 + k } );
		}
	}
	if( f.hasStats ){
		auto pair = [&]( float ix, float iy, const wchar_t* label, const std::wstring& v ){
			gfx::Text( Font::Small, label, ix, iy, lt::TEXT, gfx::Left, 0, false );
			gfx::Text( Font::Small, v, ix + 70, iy, lt::TEXT, gfx::Left, 0, false );
		};
		auto plus = []( int a, int b ){ return std::to_wstring( a ) + L" + " + std::to_wstring( b ); };
		float ix = cx + 232, ix2 = cx + 232 + 150;
		pair( ix, py + 38, L"ATQ", plus( st.atk, st.atk2 ) );
		pair( ix, py + 60, L"ATQ M", plus( st.matk, st.matk2 ) );
		pair( ix, py + 82, L"DEF", plus( st.def, st.def2 ) );
		pair( ix, py + 104, L"DEF M", plus( st.mdef, st.mdef2 ) );
		pair( ix2, py + 38, L"Precisão", std::to_wstring( st.hit ) );
		pair( ix2, py + 60, L"Esquiva", st.flee2 ? plus( st.flee, st.flee2 ) : std::to_wstring( st.flee ) );
		pair( ix2, py + 82, L"Crítico", std::to_wstring( st.crit ) );
		pair( ix2, py + 104, L"Vel. Atq.", std::to_wstring( st.aspd ) );
		gfx::Text( Font::Small, L"Pontos de Atrib", ix, py + 148, lt::ACCENT, gfx::Left, 0, false );
		gfx::Text( Font::SmallBold, std::to_wstring( st.points ), ix + 110, py + 148, lt::TEXT, gfx::Left, 0, false );
		float bx = cx + cw - 104, by = py + 142;
		SmallButton( L"Detalhes", bx, by, 90, 26, m, true );
		g_eq.hits.push_back( { bx, by, 90, 26, f.nx + 140, f.ny + 100, EQ_DETAILS } );
	}
	gfx::End();
}

bool ui::EquipCharacterTarget( float& x, float& y, float& scale ){
	if( GetTickCount() - g_eq.tick > 250 || !g_eq.charOn ) return false;
	x = g_eq.charX; y = g_eq.charY; scale = g_eq.charScale;
	return true;
}

bool ui::EquipWindowRedirect( const input::Point& real, input::Point& target ){
	if( GetTickCount() - g_eq.tick > 250 ) return false;
	const EquipUi& u = g_eq;
	if( !Inside( real, u.x, u.y, u.w, u.h ) ) return false;
	for( auto it = u.hits.rbegin(); it != u.hits.rend(); ++it ){
		if( Inside( real, it->x, it->y, it->w, it->h ) ){ target = { it->nx, it->ny }; return true; }
	}
	if( u.embed ){ // aba nativa ampliada: o ponto equivalente na janela nativa
		float lx = ( real.x - u.ex ) / u.escale, ly = ( real.y - u.ey ) / u.escale;
		if( lx >= 0 && lx < 280 && ly >= 38 && ly < 170 ){ target = { u.nx + lx, u.ny + ly }; return true; }
	}
	if( real.y - u.y < 46 ){ // cabeçalho: arrasta a janela
		target = { u.nx + std::min( std::max( real.x - u.x, 20.0f ), u.nw - 40 ), u.ny + 9 };
		return true;
	}
	target = { u.nx + 140, u.ny + 100 }; // corpo (soltar item aqui equipa, como na janela original)
	return true;
}

// ======================================================================== inventário
namespace {
	struct InvHit { float x, y, w, h; int item; int local; };  // item = índice do inventário (-1 = nenhum)
	enum InvLocal { INV_NONE = 0, INV_TAB0 = 1, INV_SORT = 20, INV_DROP, INV_SEARCH, INV_LIST, INV_YES, INV_NO };
	struct InvUi {
		DWORD tick = 0;
		float x = 0, y = 0, w = 0, h = 0;
		float nx = 0, ny = 0, nw = 0, nh = 0;
		std::vector<InvHit> hits;
		int tab = 0;
		bool sorted = false, listMode = false, searchFocus = false;
		std::wstring search;
		int selected = -1;           // índice do item selecionado (Descartar)
		int confirmDrop = -1;        // pedindo confirmação
		float scroll = 0;
		bool prevDown = false;
		unsigned seq = 0;
	} g_inv;

	const wchar_t* INV_TABS[7] = { L"Todos", L"Equipamentos", L"Consumo", L"Itens", L"Carta", L"Mascote", L"Outros" };

	int InvCategory( const game::Item& it ){
		switch( it.type ){
			case 0: case 2: case 11: case 18: return 2;   // consumo
			case 4: case 5: case 12: return 1;            // equipamentos (inclui sombra)
			case 3: return 3;                             // itens (etc)
			case 6: return 4;                             // carta
			case 7: case 8: return 5;                     // mascote (ovo e acessório)
		}
		return 6;                                         // outros (munição etc)
	}

	bool InvMatch( const game::Item& it, const std::wstring& name ){
		if( g_inv.tab != 0 && InvCategory( it ) != g_inv.tab ) return false;
		if( g_inv.search.empty() ) return true;
		std::wstring a = name, b = g_inv.search;
		for( wchar_t& c : a ) c = towlower( c );
		for( wchar_t& c : b ) c = towlower( c );
		return a.find( b ) != std::wstring::npos;
	}

	void InvButton( const wchar_t* text, float x, float y, float w, float h, input::Point m, bool danger, int local ){
		bool hover = Inside( m, x, y, w, h );
		D3DCOLOR bg = danger ? ( hover ? Rgba( 252, 228, 228, 1.0f ) : Rgba( 253, 240, 240, 1.0f ) ) : ( hover ? Rgba( 226, 230, 250, 1.0f ) : Rgba( 236, 238, 250, 1.0f ) );
		D3DCOLOR fg = danger ? Rgba( 200, 60, 60, 1.0f ) : lt::ACCENT;
		gfx::RoundRect( x, y, w, h, 6, bg, g_disc );
		gfx::Frame( x, y, w, h, danger ? Rgba( 236, 170, 170, 1.0f ) : Rgba( 196, 202, 232, 1.0f ) );
		gfx::Text( Font::Body, text, x + w * 0.5f, y + ( h - gfx::LineHeight( Font::Body ) ) * 0.5f, fg, gfx::Center, 0, false );
		g_inv.hits.push_back( { x, y, w, h, -1, local } );
	}

	void LockGlyph( float cx, float cy, D3DCOLOR c ){
		gfx::Circle( cx, cy - 4, 6, c );
		gfx::Circle( cx, cy - 4, 4, Rgba( 246, 247, 252, 1.0f ) );
		gfx::RoundRect( cx - 8, cy - 2, 16, 12, 2, c, g_disc );
	}
}

void ui::DrawInventoryWindow( IDirect3DDevice9* dev, const InventoryFrame& f ){
	g_s = 1.0f;
	gfx::SetUiScale( 1.0f );
	if( !gfx::Begin( dev ) ) return;
	GameAssets( dev );
	const float W = 746, H = 528;
	float x = f.nx, y = f.ny;
	if( x + W > f.vpWidth - 4 ) x = std::max( 4.0f, f.vpWidth - 4 - W );
	if( y + H > f.vpHeight - 4 ) y = std::max( 4.0f, f.vpHeight - 4 - H );
	if( GetTickCount() - g_inv.tick > 1000 ){ g_inv.searchFocus = false; g_inv.confirmDrop = -1; input::SetTextCapture( false ); }
	g_inv.tick = GetTickCount();
	g_inv.seq = ++g_uiSeq;
	g_inv.x = x; g_inv.y = y; g_inv.w = W; g_inv.h = H;
	NoteBig( 1, x, y, W, H );
	g_inv.nx = f.nx; g_inv.ny = f.ny; g_inv.nw = f.nw; g_inv.nh = f.nh;
	input::Point m = input::RealCursor();

	// texto do campo de busca
	if( g_inv.searchFocus ){
		for( wchar_t c : input::TakeTyped() ){
			if( c == L'\b' ){ if( !g_inv.search.empty() ) g_inv.search.pop_back(); }
			else if( c == 13 || c == 27 ){ g_inv.searchFocus = false; input::SetTextCapture( false ); }
			else if( g_inv.search.size() < 30 ) g_inv.search += c;
		}
	}
	// cliques nas ações nossas (ao soltar)
	bool down = input::IsLeftDown();
	if( g_inv.prevDown && !down ){
		bool onSearch = false;
		for( const InvHit& h : g_inv.hits ){
			if( !Inside( m, h.x, h.y, h.w, h.h ) ) continue;
			if( h.item >= 0 ) g_inv.selected = h.item;
			if( h.local >= INV_TAB0 && h.local < INV_TAB0 + 7 ){ g_inv.tab = h.local - INV_TAB0; g_inv.scroll = 0; }
			else if( h.local == INV_SORT ) g_inv.sorted = !g_inv.sorted;
			else if( h.local == INV_LIST ) g_inv.listMode = !g_inv.listMode;
			else if( h.local == INV_DROP && g_inv.selected >= 0 ) g_inv.confirmDrop = g_inv.selected;
			else if( h.local == INV_SEARCH ) onSearch = true;
			else if( h.local == INV_YES && g_inv.confirmDrop >= 0 ){
				for( const game::Item& it : game::Inventory() ) if( it.index == g_inv.confirmDrop ) game::DropItem( it.index, it.count );
				g_inv.confirmDrop = -1; g_inv.selected = -1;
			}else if( h.local == INV_NO ) g_inv.confirmDrop = -1;
		}
		if( onSearch != g_inv.searchFocus ){ g_inv.searchFocus = onSearch; input::SetTextCapture( onSearch ); }
	}
	g_inv.prevDown = down;
	g_inv.hits.clear();

	// janela
	gfx::RoundRect( x + 3, y + 5, W, H, 12, Rgba( 20, 24, 50, 0.22f ), g_disc );
	gfx::RoundRect( x, y, W, H, 12, lt::BG, g_disc );
	gfx::RoundRect( x, y, 300, 46, 12, Rgba( 255, 255, 255, 1.0f ), g_disc );
	gfx::Triangle( x + 300, y, x + 300, y + 46, x + 342, y + 46, Rgba( 255, 255, 255, 1.0f ) );
	gfx::Rect( x + 12, y + 46, W - 24, 1, lt::LINE );
	{ // bolsa
		float cx = x + 34, cy = y + 24;
		gfx::RoundRect( cx - 11, cy - 6, 22, 18, 6, Rgba( 64, 72, 150, 1.0f ), g_disc );
		gfx::Circle( cx, cy - 8, 6, Rgba( 64, 72, 150, 1.0f ) );
		gfx::Circle( cx, cy - 8, 3, Rgba( 255, 255, 255, 1.0f ) );
		gfx::Rect( cx - 4, cy + 1, 8, 3, Rgba( 226, 230, 250, 1.0f ) );
	}
	gfx::Text( Font::Name, L"INVENTÁRIO", x + 58, y + 13, lt::TITLE, gfx::Left, 0, false );
	{ // fechar (botão nativo de fechar: 270,7)
		float cx = x + W - 26, cy = y + 23;
		bool hover = Inside( m, cx - 10, cy - 10, 20, 20 );
		D3DCOLOR c = hover ? lt::ACCENT : lt::LABEL;
		gfx::Line( cx - 6, cy - 6, cx + 6, cy + 6, c, 1.6f );
		gfx::Line( cx - 6, cy + 6, cx + 6, cy - 6, c, 1.6f );
		g_inv.hits.push_back( { cx - 12, cy - 12, 24, 24, -2, INV_NONE } );
	}

	// itens filtrados
	std::vector<game::Item> all = game::Inventory();
	std::vector<std::pair<game::Item, std::wstring>> items;
	for( const game::Item& it : all ){
		std::wstring n = itemdb::Name( it.id );
		if( n.empty() ) n = L"Item " + std::to_wstring( it.id );
		if( InvMatch( it, n ) ) items.push_back( { it, n } );
	}
	if( g_inv.sorted ){
		std::stable_sort( items.begin(), items.end(), []( const auto& a, const auto& b ){
			int ca = InvCategory( a.first ), cb = InvCategory( b.first );
			return ca != cb ? ca < cb : a.second < b.second;
		} );
	}
	int capacity = game::InventoryCapacity();

	// abas e contador
	float tx = x + 24, ty = y + 60;
	for( int k = 0; k < 7; k++ ){
		float tw = gfx::TextWidth( Font::Body, INV_TABS[k] ) + 28;
		bool sel = g_inv.tab == k, hover = Inside( m, tx, ty, tw, 30 );
		if( sel ) gfx::RoundRect( tx, ty, tw, 30, 6, Rgba( 92, 100, 180, 1.0f ), g_disc );
		else{
			gfx::RoundRect( tx, ty, tw, 30, 6, hover ? Rgba( 236, 238, 250, 1.0f ) : Rgba( 255, 255, 255, 1.0f ), g_disc );
			gfx::Frame( tx, ty, tw, 30, lt::LINE );
		}
		gfx::Text( Font::Body, INV_TABS[k], tx + tw * 0.5f, ty + ( 30 - gfx::LineHeight( Font::Body ) ) * 0.5f, sel ? Rgba( 255, 255, 255, 1.0f ) : lt::TEXT, gfx::Center, 0, false );
		g_inv.hits.push_back( { tx, ty, tw, 30, -1, INV_TAB0 + k } );
		tx += tw + 6;
	}
	{
		std::wstring cnt = std::to_wstring( all.size() ) + L" / " + std::to_wstring( capacity );
		float cw = gfx::TextWidth( Font::Body, cnt ) + 24;
		float cx = x + W - 24 - cw;
		gfx::RoundRect( cx, ty, cw, 30, 6, Rgba( 255, 255, 255, 1.0f ), g_disc );
		gfx::Frame( cx, ty, cw, 30, lt::LINE );
		gfx::Text( Font::Body, cnt, cx + cw * 0.5f, ty + ( 30 - gfx::LineHeight( Font::Body ) ) * 0.5f, lt::TEXT, gfx::Center, 0, false );
	}

	// grade (8 colunas, rolagem com a roda)
	const float gx = x + 24, gy = y + 104, cell = 78, gap = 9, gh = 4 * ( cell + gap );
	const int cols = 8;
	int slots = g_inv.tab == 0 && g_inv.search.empty() ? std::max( capacity, (int)items.size() ) : (int)items.size();
	int lockedRow = g_inv.tab == 0 && g_inv.search.empty() ? cols : 0; // uma linha de espaços bloqueados (ampliação)
	int total = slots + lockedRow;
	int rows = ( total + cols - 1 ) / cols;
	float maxScroll = std::max( 0.0f, rows * ( cell + gap ) - gh );
	if( Inside( m, gx, gy, cols * ( cell + gap ), gh ) ) g_inv.scroll -= input::TakeWheel() / 120.0f * ( cell + gap );
	g_inv.scroll = std::min( maxScroll, std::max( 0.0f, g_inv.scroll ) );
	RECT clip = { (LONG)gx - 2, (LONG)gy - 2, (LONG)( x + W - 14 ), (LONG)( gy + gh ) }, oldRect;
	DWORD oldScissor = FALSE;
	dev->GetRenderState( D3DRS_SCISSORTESTENABLE, &oldScissor );
	dev->GetScissorRect( &oldRect );
	dev->SetScissorRect( &clip );
	dev->SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE );
	if( !g_inv.listMode ){
		for( int i = 0; i < total; i++ ){
			float cx = gx + ( i % cols ) * ( cell + gap ), cy = gy + ( i / cols ) * ( cell + gap ) - g_inv.scroll;
			if( cy + cell < gy || cy > gy + gh ) continue;
			bool hasItem = i < (int)items.size();
			bool locked = i >= slots;
			bool hover = hasItem && Inside( m, cx, cy, cell, cell ) && m.y >= gy && m.y < gy + gh;
			bool sel = hasItem && items[i].first.index == g_inv.selected;
			gfx::RoundRect( cx, cy, cell, cell, 8, locked ? Rgba( 240, 241, 247, 1.0f ) : Rgba( 255, 255, 255, 1.0f ), g_disc );
			gfx::Frame( cx, cy, cell, cell, sel ? lt::ACCENT : hover ? Rgba( 160, 168, 220, 1.0f ) : lt::LINE );
			if( locked ){ LockGlyph( cx + cell * 0.5f, cy + cell * 0.5f, Rgba( 196, 200, 214, 1.0f ) ); continue; }
			if( !hasItem ) continue;
			const game::Item& it = items[i].first;
			gfx::Image icon;
			if( ui::ItemIcon( dev, it.id, icon ) ) gfx::DrawImage( icon, cx + ( cell - 44 ) * 0.5f, cy + 8, 44, 44 );
			if( it.refine > 0 ) gfx::Text( Font::SmallBold, L"+" + std::to_wstring( it.refine ), cx + 6, cy + 4, lt::REFINE, gfx::Left, 0, false );
			gfx::Text( Font::Body, std::to_wstring( it.count ), cx + cell - 7, cy + cell - 6 - gfx::LineHeight( Font::Body ), lt::TEXT, gfx::Right, 0, false );
			if( cy >= gy - 4 && cy + cell <= gy + gh + 4 ) g_inv.hits.push_back( { cx, cy, cell, cell, it.index, INV_NONE } );
		}
	}else{
		float rh = 34;
		for( size_t i = 0; i < items.size(); i++ ){
			float ry = gy + i * ( rh + 4 ) - g_inv.scroll;
			if( ry + rh < gy || ry > gy + gh ) continue;
			const game::Item& it = items[i].first;
			bool hover = Inside( m, gx, ry, W - 48, rh );
			gfx::RoundRect( gx, ry, W - 48, rh, 6, hover || it.index == g_inv.selected ? Rgba( 240, 242, 252, 1.0f ) : Rgba( 255, 255, 255, 1.0f ), g_disc );
			gfx::Frame( gx, ry, W - 48, rh, it.index == g_inv.selected ? lt::ACCENT : lt::LINE );
			gfx::Image icon;
			if( ui::ItemIcon( dev, it.id, icon ) ) gfx::DrawImage( icon, gx + 8, ry + 5, 24, 24 );
			std::wstring name = ( it.refine > 0 ? L"+" + std::to_wstring( it.refine ) + L" " : L"" ) + items[i].second;
			gfx::Text( Font::Body, name, gx + 42, ry + ( rh - gfx::LineHeight( Font::Body ) ) * 0.5f, lt::TEXT, gfx::Left, W - 260, false );
			gfx::Text( Font::Small, INV_TABS[InvCategory( it )], gx + W - 200, ry + 10, lt::MUTED, gfx::Left, 0, false );
			gfx::Text( Font::Body, L"x" + std::to_wstring( it.count ), gx + W - 60, ry + 8, lt::TEXT, gfx::Right, 0, false );
			if( ry >= gy - 4 && ry + rh <= gy + gh + 4 ) g_inv.hits.push_back( { gx, ry, W - 48, rh, it.index, INV_NONE } );
		}
		maxScroll = std::max( 0.0f, items.size() * ( rh + 4 ) - gh );
		g_inv.scroll = std::min( maxScroll, g_inv.scroll );
	}
	dev->SetRenderState( D3DRS_SCISSORTESTENABLE, oldScissor );
	dev->SetScissorRect( &oldRect );
	if( maxScroll > 0 ){
		float bh = std::max( 30.0f, gh * gh / ( gh + maxScroll ) );
		float by = gy + ( gh - bh ) * ( g_inv.scroll / maxScroll );
		gfx::RoundRect( x + W - 12, by, 4, bh, 2, Rgba( 92, 100, 180, 0.5f ), g_disc );
	}

	// barra de baixo
	float by = y + H - 50;
	gfx::Rect( x + 12, by - 10, W - 24, 1, lt::LINE );
	InvButton( L"Organizar", x + 24, by, 96, 34, m, false, INV_SORT );
	InvButton( L"Descartar", x + 128, by, 104, 34, m, true, INV_DROP );
	{ // busca
		float sx = x + 360, sw = 300;
		gfx::RoundRect( sx, by, sw, 34, 6, Rgba( 255, 255, 255, 1.0f ), g_disc );
		gfx::Frame( sx, by, sw, 34, g_inv.searchFocus ? lt::ACCENT : lt::LINE );
		bool empty = g_inv.search.empty();
		std::wstring shown = empty && !g_inv.searchFocus ? L"Buscar item..." : g_inv.search + ( g_inv.searchFocus && ( GetTickCount() / 500 ) % 2 ? L"|" : L"" );
		gfx::Text( Font::Body, shown, sx + 14, by + ( 34 - gfx::LineHeight( Font::Body ) ) * 0.5f, empty && !g_inv.searchFocus ? lt::MUTED : lt::TEXT, gfx::Left, sw - 50, false );
		gfx::Circle( sx + sw - 22, by + 15, 7, lt::MUTED );
		gfx::Circle( sx + sw - 22, by + 15, 5, Rgba( 255, 255, 255, 1.0f ) );
		gfx::Line( sx + sw - 17, by + 20, sx + sw - 12, by + 25, lt::MUTED, 2 );
		g_inv.hits.push_back( { sx, by, sw, 34, -1, INV_SEARCH } );
	}
	{ // lista / grade
		float lx = x + W - 66, bw = 42;
		bool hover = Inside( m, lx, by, bw, 34 );
		gfx::RoundRect( lx, by, bw, 34, 6, hover || g_inv.listMode ? Rgba( 226, 230, 250, 1.0f ) : Rgba( 255, 255, 255, 1.0f ), g_disc );
		gfx::Frame( lx, by, bw, 34, lt::LINE );
		for( int k = 0; k < 3; k++ ) gfx::Rect( lx + 12, by + 10 + k * 6, 18, 2, lt::ACCENT );
		g_inv.hits.push_back( { lx, by, bw, 34, -1, INV_LIST } );
	}

	// confirmação de descarte
	if( g_inv.confirmDrop >= 0 ){
		std::wstring name;
		int count = 0;
		for( const game::Item& it : all ) if( it.index == g_inv.confirmDrop ){ name = itemdb::Name( it.id ); count = it.count; }
		if( name.empty() && count == 0 ) g_inv.confirmDrop = -1;
		else{
			gfx::RoundRect( x, y, W, H, 12, Rgba( 20, 24, 50, 0.35f ), g_disc );
			float mw = 380, mh = 140, mx = x + ( W - mw ) * 0.5f, my = y + ( H - mh ) * 0.5f;
			gfx::RoundRect( mx, my, mw, mh, 10, Rgba( 255, 255, 255, 1.0f ), g_disc );
			gfx::Frame( mx, my, mw, mh, lt::LINE );
			gfx::Text( Font::BodyBold, L"Descartar item", mx + 20, my + 16, lt::TITLE, gfx::Left, 0, false );
			gfx::Text( Font::Body, std::to_wstring( count ) + L"x " + name + L" será jogado no chão.", mx + 20, my + 46, lt::TEXT, gfx::Left, mw - 40, false );
			InvButton( L"Cancelar", mx + mw - 220, my + mh - 50, 96, 34, m, false, INV_NO );
			InvButton( L"Descartar", mx + mw - 116, my + mh - 50, 96, 34, m, true, INV_YES );
		}
	}
	gfx::End();
}

bool ui::InventoryRedirect( const input::Point& real, input::Point& target ){
	if( GetTickCount() - g_inv.tick > 250 ) return false;
	const InvUi& u = g_inv;
	if( !Inside( real, u.x, u.y, u.w, u.h ) ) return false;
	const input::Point neutral = { u.nx + 200, u.ny + u.nh - 60 }; // célula vazia da janela nativa
	if( u.confirmDrop < 0 ){
		for( auto it = u.hits.rbegin(); it != u.hits.rend(); ++it ){
			if( !Inside( real, it->x, it->y, it->w, it->h ) ) continue;
			if( it->item == -2 ){ target = { u.nx + 270, u.ny + 7 }; return true; } // fechar
			if( it->item >= 0 ){
				// o item precisa estar na aba nativa atual: troca a aba (sem botão apertado) até achar
				int cell = game::NativeInventoryCell( it->item );
				if( cell < 0 && !input::IsLeftDown() ){
					int cur = game::NativeInventoryTab();
					for( int t = 0; t < 4 && cell < 0; t++ ){
						if( t == cur ) continue;
						game::SetNativeInventoryTab( t );
						cell = game::NativeInventoryCell( it->item );
					}
				}
				if( cell >= 0 ){
					target = { u.nx + 57 + ( cell % 7 ) * 32.5f, u.ny + 35 + ( cell / 7 ) * 32.5f };
					return true;
				}
			}
			target = neutral;
			return true;
		}
	}
	if( real.y - u.y < 46 ){
		target = { u.nx + std::min( std::max( real.x - u.x, 20.0f ), u.nw - 40 ), u.ny + 9 };
		return true;
	}
	target = neutral; // soltar aqui um item arrastado (do equipamento, por exemplo) vai para o inventário
	return true;
}


// ======================================================================== habilidades
namespace {
	struct SkillDef { std::string aegis; std::wstring name; int maxLv = 1; std::vector<std::pair<int, int>> need; };
	std::map<int, int> g_jobParent;                       // classe -> anterior
	std::map<int, std::vector<std::pair<int, int>>> g_jobTree; // classe -> (posição, habilidade)
	std::map<int, SkillDef> g_skillDefs;
	bool g_skillDataLoaded = false;

	void LoadSkillData(){
		if( g_skillDataLoaded ) return;
		g_skillDataLoaded = true;
		ReadLines( L"habilidades.txt", []( const std::string& l ){
			if( l.size() < 3 ) return;
			size_t eq = l.find( '=' );
			if( eq == std::string::npos ) return;
			int key = atoi( l.c_str() + 2 );
			std::string v = l.substr( eq + 1 );
			if( l[0] == 'J' ) g_jobParent[key] = atoi( v.c_str() );
			else if( l[0] == 'T' ){
				auto& t = g_jobTree[key];
				size_t p = 0;
				while( p < v.size() ){
					size_t c = v.find( ',', p );
					std::string part = v.substr( p, c == std::string::npos ? std::string::npos : c - p );
					size_t col = part.find( ':' );
					if( col != std::string::npos ) t.push_back( { atoi( part.c_str() ), atoi( part.c_str() + col + 1 ) } );
					p = c == std::string::npos ? v.size() : c + 1;
				}
			}else if( l[0] == 'S' ){
				SkillDef d;
				size_t b1 = v.find( '|' ), b2 = v.find( '|', b1 + 1 ), b3 = v.find( '|', b2 + 1 );
				if( b3 == std::string::npos ) return;
				d.aegis = v.substr( 0, b1 );
				d.name = Utf8( v.substr( b1 + 1, b2 - b1 - 1 ) );
				d.maxLv = atoi( v.c_str() + b2 + 1 );
				std::string need = v.substr( b3 + 1 );
				size_t p = 0;
				while( p < need.size() ){
					size_t c = need.find( ';', p );
					std::string part = need.substr( p, c == std::string::npos ? std::string::npos : c - p );
					size_t col = part.find( ':' );
					if( col != std::string::npos ) d.need.push_back( { atoi( part.c_str() ), atoi( part.c_str() + col + 1 ) } );
					p = c == std::string::npos ? need.size() : c + 1;
				}
				g_skillDefs[key] = d;
			}
		} );
	}

	struct SkHit { float x, y, w, h; int local; int value; };
	enum SkLocal { SK_NONE = 0, SK_JOB, SK_RESET, SK_FILTER, SK_UP, SK_SHORTCUTS };
	struct SkUi {
		DWORD tick = 0;
		float x = 0, y = 0, w = 0, h = 0;
		float nx = 0, ny = 0, nw = 0, nh = 0;
		std::vector<SkHit> hits;
		int job = -1;             // classe mostrada (-1 = a atual)
		int filter = 0;           // 0 ativas, 1 passivas, 2 todas
		bool native = false;      // lista nativa à mostra
		bool dropdown = false;    // lista de classes aberta
		bool prevDown = false;
		unsigned seq = 0;
		float scroll = 0;
	} g_sk;

	bool SkillIcon( IDirect3DDevice9* dev, const std::string& aegis, gfx::Image& out ){
		std::string n = aegis;
		for( char& c : n ) c = (char)tolower( (unsigned char)c );
		return ClientImage( dev, UI_DIR + "item\\" + n + ".bmp", out );
	}
}

bool ui::SkillNativeVisible(){
	return g_sk.native && GetTickCount() - g_sk.tick < 1000;
}

void ui::DrawSkillWindow( IDirect3DDevice9* dev, const SkillFrame& f ){
	g_s = 1.0f;
	gfx::SetUiScale( 1.0f );
	if( !gfx::Begin( dev ) ) return;
	GameAssets( dev );
	LoadSkillData();
	const float W = 716, H = 440;
	float x = f.nx, y = f.ny;
	if( x + W > f.vpWidth - 4 ) x = std::max( 4.0f, f.vpWidth - 4 - W );
	if( y + H > f.vpHeight - 4 ) y = std::max( 4.0f, f.vpHeight - 4 - H );
	if( GetTickCount() - g_sk.tick > 1000 ){ g_sk.job = -1; g_sk.native = false; g_sk.scroll = 0; }
	g_sk.tick = GetTickCount();
	g_sk.seq = ++g_uiSeq;
	g_sk.x = x; g_sk.y = y; g_sk.w = W; g_sk.h = H;
	NoteBig( 2, x, y, W, H );
	g_sk.nx = f.nx; g_sk.ny = f.ny; g_sk.nw = f.nw; g_sk.nh = f.nh;
	input::Point m = input::RealCursor();

	// classes da linhagem (aprendiz -> atual) que têm árvore
	int current = game::CurrentJob();
	std::vector<int> chain;
	for( int j = current, guard = 0; guard < 8; guard++ ){
		if( g_jobTree.count( j ) ) chain.insert( chain.begin(), j );
		auto p = g_jobParent.find( j );
		if( p == g_jobParent.end() || p->second == j ) break;
		j = p->second;
	}
	if( chain.empty() ) chain.push_back( current );
	if( g_sk.job < 0 || std::find( chain.begin(), chain.end(), g_sk.job ) == chain.end() ) g_sk.job = chain.back();

	// cliques nossos (ao soltar)
	std::map<int, game::Skill> have = game::Skills();
	int points = game::SkillPoints();
	bool down = input::IsLeftDown();
	if( g_sk.prevDown && !down ){
		bool clicked = false;
		for( auto hit = g_sk.hits.rbegin(); hit != g_sk.hits.rend(); ++hit ){
			const SkHit& h = *hit;
			if( !Inside( m, h.x, h.y, h.w, h.h ) ) continue;
			if( h.local == SK_JOB ){ // 0 = abre, -1 = fecha, >0 = escolhe a classe (id + 1)
				if( h.value == 0 ) g_sk.dropdown = true;
				else if( h.value < 0 ) g_sk.dropdown = false;
				else{ g_sk.job = h.value - 1; g_sk.scroll = 0; g_sk.dropdown = false; }
				clicked = true;
				break;
			}else if( h.local == SK_RESET ){ g_sk.job = chain.back(); g_sk.filter = 2; g_sk.scroll = 0; }
			else if( h.local == SK_FILTER ) g_sk.filter = h.value;
			else if( h.local == SK_UP ) game::UpgradeSkill( h.value );
			else if( h.local == SK_SHORTCUTS ) g_sk.native = !g_sk.native;
			clicked = true;
			break;
		}
		if( !clicked ) g_sk.dropdown = false;
	}
	g_sk.prevDown = down;
	g_sk.hits.clear();

	// janela
	gfx::RoundRect( x + 3, y + 5, W, H, 12, Rgba( 20, 24, 50, 0.22f ), g_disc );
	gfx::RoundRect( x, y, W, H, 12, lt::BG, g_disc );
	gfx::RoundRect( x, y, 300, 46, 12, Rgba( 255, 255, 255, 1.0f ), g_disc );
	gfx::Triangle( x + 300, y, x + 300, y + 46, x + 342, y + 46, Rgba( 255, 255, 255, 1.0f ) );
	gfx::Rect( x + 12, y + 46, W - 24, 1, lt::LINE );
	{ // estrela
		float cx = x + 34, cy = y + 23;
		gfx::Circle( cx, cy, 15, Rgba( 64, 72, 150, 1.0f ) );
		for( int k = 0; k < 5; k++ ){
			float a = -1.5708f + k * 1.2566f, b = a + 0.6283f;
			gfx::Triangle( cx, cy, cx + cosf( a ) * 10, cy + sinf( a ) * 10, cx + cosf( b ) * 4, cy + sinf( b ) * 4, Rgba( 255, 255, 255, 1.0f ) );
			gfx::Triangle( cx, cy, cx + cosf( a ) * 10, cy + sinf( a ) * 10, cx + cosf( a - 0.6283f ) * 4, cy + sinf( a - 0.6283f ) * 4, Rgba( 255, 255, 255, 1.0f ) );
		}
	}
	gfx::Text( Font::Name, L"HABILIDADES", x + 58, y + 13, lt::TITLE, gfx::Left, 0, false );
	{
		float cx = x + W - 26, cy = y + 23;
		bool hover = Inside( m, cx - 10, cy - 10, 20, 20 );
		D3DCOLOR c = hover ? lt::ACCENT : lt::LABEL;
		gfx::Line( cx - 6, cy - 6, cx + 6, cy + 6, c, 1.6f );
		gfx::Line( cx - 6, cy + 6, cx + 6, cy - 6, c, 1.6f );
		g_sk.hits.push_back( { cx - 12, cy - 12, 24, 24, SK_NONE, -2 } );
	}

	// coluna da esquerda
	float lx = x + 14, lw = 166, ly = y + 60;
	{
		bool hover = Inside( m, lx, ly, lw, 38 );
		gfx::RoundRect( lx, ly, lw, 38, 6, hover ? Rgba( 240, 242, 252, 1.0f ) : Rgba( 255, 255, 255, 1.0f ), g_disc );
		gfx::Frame( lx, ly, lw, 38, lt::LINE );
		ClassBadge( lx + 20, ly + 19, 26, g_sk.job, Rgba( 159, 178, 196, 0.8f ), 0, 0 );
		gfx::Text( Font::BodyBold, chardata::JobName( g_sk.job, 1 ), lx + 38, ly + 10, lt::TITLE, gfx::Left, lw - 60, false );
		gfx::Triangle( lx + lw - 20, ly + 16, lx + lw - 10, ly + 16, lx + lw - 15, ly + 22, lt::LABEL );
		g_sk.hits.push_back( { lx, ly, lw, 38, SK_JOB, g_sk.dropdown ? -1 : 0 } );
	}
	gfx::Text( Font::Small, L"Pontos de Habilidade", lx + lw * 0.5f, ly + 54, lt::LABEL, gfx::Center, 0, false );
	gfx::RoundRect( lx + 30, ly + 74, lw - 60, 28, 6, Rgba( 255, 255, 255, 1.0f ), g_disc );
	gfx::Frame( lx + 30, ly + 74, lw - 60, 28, lt::LINE );
	gfx::Text( Font::BodyBold, std::to_wstring( points ), lx + lw * 0.5f, ly + 79, lt::TEXT, gfx::Center, 0, false );
	SmallButton( L"Restaurar", lx + 20, ly + 112, lw - 40, 30, m );
	g_sk.hits.push_back( { lx + 20, ly + 112, lw - 40, 30, SK_RESET, 0 } );
	const wchar_t* filters[3] = { L"Habilidades Ativas", L"Habilidades Passivas", L"Todas as Habilidades" };
	for( int k = 0; k < 3; k++ ){
		float fy = ly + 168 + k * 42;
		bool sel = g_sk.filter == k, hover = Inside( m, lx, fy, lw, 34 );
		gfx::RoundRect( lx, fy, lw, 34, 6, sel ? Rgba( 92, 100, 180, 1.0f ) : hover ? Rgba( 240, 242, 252, 1.0f ) : Rgba( 255, 255, 255, 1.0f ), g_disc );
		if( !sel ) gfx::Frame( lx, fy, lw, 34, lt::LINE );
		gfx::Text( Font::Small, filters[k], lx + lw * 0.5f, fy + ( 34 - gfx::LineHeight( Font::Small ) ) * 0.5f, sel ? Rgba( 255, 255, 255, 1.0f ) : lt::TEXT, gfx::Center, 0, false );
		g_sk.hits.push_back( { lx, fy, lw, 34, SK_FILTER, k } );
	}

	// árvore
	float tx = x + 196, ty = y + 58, tw = x + W - 14 - tx, th = H - 58 - 52;
	gfx::RoundRect( tx, ty, tw, th, 8, Rgba( 255, 255, 255, 1.0f ), g_disc );
	gfx::Frame( tx, ty, tw, th, lt::LINE );
	const auto& tree = g_jobTree[g_sk.job];
	const float colW = tw / 7.0f, rowH = 78;
	std::map<int, std::pair<float, float>> pos; // habilidade -> centro do ícone
	int maxRow = 0;
	for( const auto& e : tree ) maxRow = std::max( maxRow, e.first / 7 );
	float maxScroll = std::max( 0.0f, ( maxRow + 1 ) * rowH + 16 - th );
	if( Inside( m, tx, ty, tw, th ) ) g_sk.scroll -= input::TakeWheel() / 120.0f * rowH * 0.5f;
	g_sk.scroll = std::min( maxScroll, std::max( 0.0f, g_sk.scroll ) );
	for( const auto& e : tree ){
		float cx = tx + colW * ( e.first % 7 ) + colW * 0.5f, cy = ty + 30 + ( e.first / 7 ) * rowH - g_sk.scroll;
		pos[e.second] = { cx, cy };
	}
	RECT clip = { (LONG)tx, (LONG)ty + 1, (LONG)( tx + tw ), (LONG)( ty + th - 1 ) }, oldRect;
	DWORD oldScissor = FALSE;
	dev->GetRenderState( D3DRS_SCISSORTESTENABLE, &oldScissor );
	dev->GetScissorRect( &oldRect );
	dev->SetScissorRect( &clip );
	dev->SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE );
	// linhas de pré-requisito
	for( const auto& e : tree ){
		auto d = g_skillDefs.find( e.second );
		if( d == g_skillDefs.end() ) continue;
		for( const auto& n : d->second.need ){
			auto a = pos.find( n.first );
			if( a == pos.end() ) continue;
			auto b = pos[e.second];
			float x0 = a->second.first, y0 = a->second.second + 18, x1 = b.first, y1 = b.second - 20;
			D3DCOLOR c = Rgba( 150, 120, 170, 0.75f );
			if( fabsf( x0 - x1 ) < 1 ) gfx::Line( x0, y0 + 22, x1, y1, c, 1.5f );
			else{
				float my = y0 + 26;
				gfx::Line( x0, y0 + 22, x0, my, c, 1.5f );
				gfx::Line( x0, my, x1, my, c, 1.5f );
				gfx::Line( x1, my, x1, y1, c, 1.5f );
			}
			gfx::Triangle( x1 - 4, y1 - 5, x1 + 4, y1 - 5, x1, y1, c );
		}
	}
	// nós
	int hoverSkill = -1;
	for( const auto& e : tree ){
		auto d = g_skillDefs.find( e.second );
		if( d == g_skillDefs.end() ) continue;
		auto hv = have.find( e.second );
		int lv = hv != have.end() ? hv->second.level : 0;
		bool passive = hv != have.end() ? hv->second.inf == 0 : false;
		if( g_sk.filter == 0 && hv != have.end() && passive ) continue;
		if( g_sk.filter == 1 && hv != have.end() && !passive ) continue;
		auto c = pos[e.second];
		float cx = c.first, cy = c.second;
		if( cy < ty - 40 || cy > ty + th + 40 ) continue;
		bool learned = lv > 0;
		bool hover = Inside( m, cx - 24, cy - 22, 48, 60 ) && Inside( m, tx, ty, tw, th );
		if( hover ) hoverSkill = e.second;
		gfx::RoundRect( cx - 19, cy - 19, 38, 38, 6, learned ? Rgba( 240, 242, 252, 1.0f ) : Rgba( 246, 246, 249, 1.0f ), g_disc );
		gfx::Frame( cx - 19, cy - 19, 38, 38, hover ? lt::ACCENT : learned ? Rgba( 196, 202, 232, 1.0f ) : lt::LINE );
		gfx::Image icon;
		if( SkillIcon( dev, d->second.aegis, icon ) ) gfx::DrawImage( icon, cx - 14, cy - 14, 28, 28, learned ? 0xFFFFFFFF : Rgba( 255, 255, 255, 0.45f ) );
		std::wstring lvl = std::to_wstring( lv ) + L"/" + std::to_wstring( d->second.maxLv );
		gfx::RoundRect( cx + 2, cy + 8, gfx::TextWidth( Font::Small, lvl ) + 6, 14, 4, learned ? Rgba( 92, 100, 180, 0.95f ) : Rgba( 160, 164, 180, 0.9f ), g_disc );
		gfx::Text( Font::Small, lvl, cx + 5, cy + 7, Rgba( 255, 255, 255, 1.0f ), gfx::Left, 0, false );
		gfx::Text( Font::Small, d->second.name, cx, cy + 22, learned ? lt::TEXT : lt::MUTED, gfx::Center, colW - 4, false );
		if( hv != have.end() && hv->second.upgradable && points > 0 && lv < d->second.maxLv ){
			float bx = cx + 12, by = cy - 26;
			bool hb = Inside( m, bx, by, 16, 16 );
			gfx::RoundRect( bx, by, 16, 16, 8, hb ? Rgba( 60, 160, 90, 1.0f ) : Rgba( 86, 190, 110, 1.0f ), g_disc );
			gfx::Rect( bx + 4, by + 7, 8, 2, Rgba( 255, 255, 255, 1.0f ) );
			gfx::Rect( bx + 7, by + 4, 2, 8, Rgba( 255, 255, 255, 1.0f ) );
			g_sk.hits.push_back( { bx, by, 16, 16, SK_UP, e.second } );
		}
	}
	dev->SetRenderState( D3DRS_SCISSORTESTENABLE, oldScissor );
	dev->SetScissorRect( &oldRect );
	if( tree.empty() ) gfx::Text( Font::Body, L"Esta classe não possui árvore de habilidades.", tx + tw * 0.5f, ty + th * 0.5f - 8, lt::MUTED, gfx::Center, 0, false );

	// rodapé
	float fy = y + H - 44;
	gfx::Text( Font::Small, L"* Use \"Configurar Atalhos\" para arrastar as habilidades para a barra de atalhos.", tx, fy + 10, lt::MUTED, gfx::Left, tw - 180, false );
	{
		float bx = x + W - 14 - 170, bw = 170;
		bool hover = Inside( m, bx, fy, bw, 32 ) || g_sk.native;
		gfx::RoundRect( bx, fy, bw, 32, 6, hover ? Rgba( 226, 230, 250, 1.0f ) : Rgba( 244, 245, 252, 1.0f ), g_disc );
		gfx::Frame( bx, fy, bw, 32, Rgba( 196, 202, 232, 1.0f ) );
		// engrenagem
		gfx::Circle( bx + 20, fy + 16, 7, lt::ACCENT );
		gfx::Circle( bx + 20, fy + 16, 3, hover ? Rgba( 226, 230, 250, 1.0f ) : Rgba( 244, 245, 252, 1.0f ) );
		gfx::Text( Font::SmallBold, g_sk.native ? L"Fechar Atalhos" : L"Configurar Atalhos", bx + 34, fy + ( 32 - gfx::LineHeight( Font::SmallBold ) ) * 0.5f, lt::ACCENT, gfx::Left, 0, false );
		g_sk.hits.push_back( { bx, fy, bw, 32, SK_SHORTCUTS, 0 } );
	}

	// lista de classes (por cima de tudo)
	if( g_sk.dropdown ){
		float dy = ly + 42, ih = 34;
		gfx::RoundRect( lx + 2, dy + 3, lw, ih * chain.size() + 8, 8, Rgba( 20, 24, 50, 0.18f ), g_disc );
		gfx::RoundRect( lx, dy, lw, ih * chain.size() + 8, 8, Rgba( 255, 255, 255, 1.0f ), g_disc );
		gfx::Frame( lx, dy, lw, ih * chain.size() + 8, lt::LINE );
		for( size_t k = 0; k < chain.size(); k++ ){
			float iy = dy + 4 + k * ih;
			bool sel = chain[k] == g_sk.job, hover = Inside( m, lx, iy, lw, ih );
			if( sel || hover ) gfx::RoundRect( lx + 4, iy, lw - 8, ih, 6, sel ? Rgba( 228, 231, 248, 1.0f ) : Rgba( 240, 242, 252, 1.0f ), g_disc );
			ClassBadge( lx + 22, iy + ih * 0.5f, 22, chain[k], Rgba( 159, 178, 196, 0.8f ), 0, 0 );
			gfx::Text( Font::Body, chardata::JobName( chain[k], 1 ), lx + 40, iy + ( ih - gfx::LineHeight( Font::Body ) ) * 0.5f, lt::TEXT, gfx::Left, lw - 50, false );
			g_sk.hits.push_back( { lx, iy, lw, ih, SK_JOB, chain[k] + 1 } );
		}
		hoverSkill = -1;
	}

	// dica da habilidade sob o mouse
	if( hoverSkill >= 0 ){
		const SkillDef& d = g_skillDefs[hoverSkill];
		auto hv = have.find( hoverSkill );
		std::wstring l2 = L"Nível " + std::to_wstring( hv != have.end() ? hv->second.level : 0 ) + L" de " + std::to_wstring( d.maxLv );
		if( hv != have.end() && hv->second.sp > 0 ) l2 += L"  ·  SP " + std::to_wstring( hv->second.sp );
		std::wstring l3;
		for( const auto& n : d.need ){
			auto nd = g_skillDefs.find( n.first );
			if( nd == g_skillDefs.end() ) continue;
			l3 += ( l3.empty() ? L"Requer: " : L", " ) + nd->second.name + L" " + std::to_wstring( n.second );
		}
		float bw = std::max( gfx::TextWidth( Font::BodyBold, d.name ), std::max( gfx::TextWidth( Font::Small, l2 ), gfx::TextWidth( Font::Small, l3 ) ) ) + 24;
		float bh = l3.empty() ? 50 : 68, bx = std::min( m.x + 18, f.vpWidth - bw - 4 ), by = m.y + 16;
		gfx::RoundRect( bx + 2, by + 3, bw, bh, 8, Rgba( 20, 24, 50, 0.2f ), g_disc );
		gfx::RoundRect( bx, by, bw, bh, 8, Rgba( 255, 255, 255, 1.0f ), g_disc );
		gfx::Frame( bx, by, bw, bh, lt::LINE );
		gfx::Text( Font::BodyBold, d.name, bx + 12, by + 8, lt::TITLE, gfx::Left, 0, false );
		gfx::Text( Font::Small, l2, bx + 12, by + 28, lt::LABEL, gfx::Left, 0, false );
		if( !l3.empty() ) gfx::Text( Font::Small, l3, bx + 12, by + 46, lt::BLUE, gfx::Left, 0, false );
	}
	gfx::End();
}

bool ui::SkillRedirect( const input::Point& real, input::Point& target ){
	if( GetTickCount() - g_sk.tick > 250 ) return false;
	const SkUi& u = g_sk;
	// lista nativa à mostra: dentro dela o cliente recebe o mouse normalmente
	if( u.native && Inside( real, u.nx, u.ny, u.nw, u.nh ) ) return false;
	if( !Inside( real, u.x, u.y, u.w, u.h ) ) return false;
	for( const SkHit& h : u.hits ){
		if( h.value == -2 && Inside( real, h.x, h.y, h.w, h.h ) ){ target = { u.nx + u.nw - 16, u.ny + 7 }; return true; } // fechar
	}
	if( real.y - u.y < 46 ){
		target = { u.nx + std::min( std::max( real.x - u.x, 20.0f ), u.nw - 60 ), u.ny + 9 };
		return true;
	}
	target = { u.nx + 160, u.ny + u.nh - 20 }; // rodapé nativo (texto dos pontos): o clique não faz nada
	return true;
}

namespace {
	bool GameWindowsRedirect( const input::Point& real, input::Point& target ){
		// janelas do jogo: a desenhada por último (por cima) tem prioridade
		struct Cand { unsigned seq; bool ( *fn )( const input::Point&, input::Point& ); } c[4] = {
			{ g_itemUi.seq, &ui::ItemWindowRedirect }, { g_eq.seq, &ui::EquipWindowRedirect }, { g_inv.seq, &ui::InventoryRedirect },
			{ g_sk.seq, &ui::SkillRedirect } };
		std::sort( c, c + 4, []( const Cand& a, const Cand& b ){ return a.seq > b.seq; } );
		for( const Cand& k : c ) if( k.fn( real, target ) ) return true;
		return false;
	}
}
