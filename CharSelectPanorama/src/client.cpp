#include "client.hpp"

#include <windows.h>
#include <cstring>

#include "config.hpp"
#include "log.hpp"
#include "chardata.hpp"
#include "d3d_hooks.hpp"

namespace {
	using DtorFn = void*( __thiscall* )( void* self, unsigned flags );
	using OnCreateFn = void( __thiscall* )( void* self, int cx, int cy );
	using OnDrawFn = void( __thiscall* )( void* self );

	DtorFn g_origDtor = nullptr;
	OnCreateFn g_origOnCreate = nullptr;
	OnDrawFn g_origOnDraw = nullptr;
	volatile int g_selectedAction = -1;
	volatile int g_frameMs = 110;
	volatile bool g_inSelectDraw = false;
	volatile int g_attackAction = -1;     // golpe de vez em quando (-1 = nunca)
	volatile int g_attackEveryMs = 6000;
	volatile int g_attackFrames = 0;      // quadros do golpe (aprendidos no desvio)
	DWORD g_attackStart = 0;              // início do golpe atual (0 = em postura)
	DWORD g_lastAttackEnd = 0;
	DtorFn g_origMakeDtor = nullptr;
	OnCreateFn g_origMakeOnCreate = nullptr;
	void* volatile g_selectWnd = nullptr;
	volatile bool g_creatingSelect = false;
	volatile bool g_inGame = false;   // passou da seleção (mapa)
	void* volatile g_makeWnd = nullptr;
	bool g_installed = false;

	template <typename T> T Read( uintptr_t addr ){
		return *reinterpret_cast<T*>( addr );
	}

	// Desvio de CActRes::GetMotion: durante a seleção, o quadro dá a volta pelo total real da camada
	using GetMotionFn = void*( __thiscall* )( void* self, int action, int motion );
	GetMotionFn g_getMotionTramp = nullptr;

	void* __fastcall HookGetMotion( void* self, void* /*edx*/, int action, int motion ){
		if( g_inSelectDraw && motion > 0 ){
			__try{
				uintptr_t act = reinterpret_cast<uintptr_t>( self );
				uintptr_t begin = Read<uintptr_t>( act + client::OFS_ACT_ACTIONS );
				uintptr_t end = Read<uintptr_t>( act + client::OFS_ACT_ACTIONS + 4 );
				if( begin != 0 && end > begin && action >= 0 && (uintptr_t)action < ( end - begin ) / 12 ){
					uintptr_t f0 = Read<uintptr_t>( begin + action * 12 );
					uintptr_t f1 = Read<uintptr_t>( begin + action * 12 + 4 );
					int frames = f1 > f0 ? (int)( ( f1 - f0 ) / client::MOTION_SIZE ) : 0;
					if( action == g_attackAction && frames > g_attackFrames ) g_attackFrames = frames;
					if( frames > 0 ) motion %= frames;
				}
			}__except( EXCEPTION_EXECUTE_HANDLER ){
			}
		}
		return g_getMotionTramp( self, action, motion );
	}

	// Início original de GetMotion: push ebp / mov ebp,esp / push -1 / push imm32 (10 bytes)
	bool InstallGetMotionHook(){
		const uint8_t expect[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
		uint8_t* fn = reinterpret_cast<uint8_t*>( client::FN_GETMOTION );
		if( memcmp( fn, expect, sizeof( expect ) ) != 0 ){
			logger::Write( "AVISO: GetMotion diferente do esperado; animacao do selecionado desativada" );
			return false;
		}
		uint8_t* tramp = static_cast<uint8_t*>( VirtualAlloc( nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) );
		if( tramp == nullptr ) return false;
		memcpy( tramp, fn, 10 );
		tramp[10] = 0xE9;
		*reinterpret_cast<int32_t*>( tramp + 11 ) = (int32_t)( ( fn + 10 ) - ( tramp + 15 ) );
		g_getMotionTramp = reinterpret_cast<GetMotionFn>( tramp );
		DWORD old;
		VirtualProtect( fn, 5, PAGE_EXECUTE_READWRITE, &old );
		fn[0] = 0xE9;
		*reinterpret_cast<int32_t*>( fn + 1 ) = (int32_t)( reinterpret_cast<uint8_t*>( &HookGetMotion ) - ( fn + 5 ) );
		VirtualProtect( fn, 5, old, &old );
		FlushInstructionCache( GetCurrentProcess(), fn, 5 );
		logger::Write( "Animacao do selecionado: GetMotion desviado" );
		return true;
	}

	bool PatchPointer( uintptr_t addr, uintptr_t value ){
		DWORD old;
		if( !VirtualProtect( reinterpret_cast<void*>( addr ), sizeof( uintptr_t ), PAGE_READWRITE, &old ) ){
			return false;
		}
		*reinterpret_cast<uintptr_t*>( addr ) = value;
		VirtualProtect( reinterpret_cast<void*>( addr ), sizeof( uintptr_t ), old, &old );
		return true;
	}

	// Sem objetos C++ aqui para poder usar __try.
	void DumpWindow( void* self ){
		__try{
			uintptr_t base = reinterpret_cast<uintptr_t>( self );
			logger::Write( "UINewSelectCharWnd=%p slotsPorPagina=%u indice=%u qtd=%u slotAbs=%u",
				self, Read<uint8_t>( base + client::OFS_SLOTS_PER_PAGE ), Read<uint32_t>( base + client::OFS_INDEX_IN_PAGE ),
				Read<uint32_t>( base + client::OFS_SLOT_COUNT ), Read<uint8_t>( client::G_SELECTED_SLOT ) );
			logger::HexDump( "UINewSelectCharWnd", self, 0x140 );

			void** slots = Read<void**>( base + client::OFS_SLOT_WINDOWS );
			if( slots != nullptr && Read<uint32_t>( base + client::OFS_SLOT_COUNT ) > 0 ){
				logger::HexDump( "UISlotForSelectWnd[0]", slots[0], client::SLOT_WND_SIZE );
			}
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			logger::Write( "DumpWindow: excecao ao ler a janela" );
		}
	}

	// Antes de desenhar: ação do boneco selecionado (o cliente volta a zerar depois de desenhar)
	void ApplyAction( void* self ){
		int action = g_selectedAction;
		if( action < 0 ) return;
		__try{
			uintptr_t base = reinterpret_cast<uintptr_t>( self );
			uintptr_t states = Read<uintptr_t>( base + client::OFS_SLOT_STATES );
			if( states == 0 ) return;
			int count = Read<int>( states - 4 ); // new[] guarda a quantidade antes do vetor
			int slot = Read<uint8_t>( client::G_SELECTED_SLOT );
			if( slot < 0 || slot >= count || count > 256 ) return;
			int* act = reinterpret_cast<int*>( states + slot * client::SLOT_STATE_SIZE + client::OFS_STATE_ACTION );
			if( *act == 0 ){ // mantém "sentado" (exclusão agendada)
				*act = action;
				int* motion = reinterpret_cast<int*>( states + slot * client::SLOT_STATE_SIZE + client::OFS_STATE_MOTION );
				int ms = g_frameMs < 16 ? 16 : g_frameMs;
				DWORD now = GetTickCount();
				*motion = g_getMotionTramp ? (int)( ( now / (DWORD)ms ) & 0xFFFF ) : 0;
				// golpe uma vez ao pré-selecionar (e, se configurado, a cada intervalo); depois, postura
				static int lastSlot = -1;
				if( g_getMotionTramp && g_attackAction >= 0 ){
					if( slot != lastSlot ){
						lastSlot = slot;
						g_attackStart = now + 550; // quando a câmera chega no personagem
					}else if( g_attackStart == 0 && g_attackEveryMs > 0 && now - g_lastAttackEnd >= (DWORD)g_attackEveryMs ){
						g_attackStart = now;
					}
					if( g_attackStart != 0 && (int)( now - g_attackStart ) >= 0 ){
						int frame = (int)( ( now - g_attackStart ) / (DWORD)ms );
						int total = g_attackFrames > 0 ? g_attackFrames : 8;
						if( frame >= total ){
							g_attackStart = 0;
							g_lastAttackEnd = now;
						}else{
							*act = g_attackAction + ( action % 8 ); // mesma direção
							*motion = frame;
						}
					}
				}
			}
		}__except( EXCEPTION_EXECUTE_HANDLER ){
		}
	}

	void __fastcall HookOnDraw( void* self, void* /*edx*/ ){
		if( self == g_selectWnd ){
			ApplyAction( self );
		}
		g_inSelectDraw = true;
		g_origOnDraw( self );
		g_inSelectDraw = false;
	}

	void* __fastcall HookDtor( void* self, void* /*edx*/, unsigned flags ){
		if( self == g_selectWnd ){
			g_selectWnd = nullptr;
			logger::Write( "Selecao de personagem fechada" );
			// fechou sem abrir a criação: foi para o jogo (voltar ao login reabre UILoginWnd e desfaz)
			if( g_makeWnd == nullptr ) g_inGame = true;
		}
		return g_origDtor( self, flags );
	}

	void* __fastcall HookMakeDtor( void* self, void* /*edx*/, unsigned flags ){
		if( self == g_makeWnd ){
			g_makeWnd = nullptr;
			logger::Write( "Criacao de personagem fechada" );
		}
		return g_origMakeDtor( self, flags );
	}

	void __fastcall HookMakeOnCreate( void* self, void* /*edx*/, int cx, int cy ){
		g_origMakeOnCreate( self, cx, cy );
		g_makeWnd = self;
		logger::Write( "Criacao de personagem aberta (%d x %d)", cx, cy );
	}

	void __fastcall HookOnCreate( void* self, void* /*edx*/, int cx, int cy ){
		// o cliente desenha quadros durante a criação: já são da seleção (só o cenário)
		g_creatingSelect = true;
		g_inGame = false;
		g_origOnCreate( self, cx, cy );
		g_creatingSelect = false;
		g_selectWnd = self;
		logger::Write( "Selecao de personagem aberta (%d x %d)", cx, cy );

		if( config::Get().debug ){
			DumpWindow( self );
		}
	}
}

namespace {
	// ---------------------------------------------------------------- janelas das telas de entrada
	struct Tracked {
		const char* name;
		uintptr_t vt, dtor, onCreate;
		DtorFn origDtor;
		OnCreateFn origOnCreate;
		void* volatile wnd;
		uintptr_t onDraw = 0;          // janelas sem criação própria: registradas ao desenhar
		void( __thiscall* origOnDraw )( void* ) = nullptr;
		volatile DWORD lastDraw = 0;   // GetTickCount do último desenho (as escondidas param de desenhar)
	};
	Tracked g_login[(int)client::LoginWnd::COUNT] = {
		{ "UILoginWnd", 0x1010258, 0x86D4C0, 0x8756D0, nullptr, nullptr, nullptr },
		{ "UISelectServerWnd", 0x1010180, 0x86DB10, 0x87BDB0, nullptr, nullptr, nullptr },
		{ "UIWaitWnd", 0x10120A4, 0x8914A0, 0x8AD490, nullptr, nullptr, nullptr },
		{ "UINoticeWnd", 0x1010408, 0x86D7B0, 0x876FF0, nullptr, nullptr, nullptr },
		{ "UINoticeConfirmWnd", 0x10135BC, 0x890730, 0x8A6430, nullptr, nullptr, nullptr },
		{ "UIDisconnectedServerMsgWnd", 0x101376C, 0x88FC70, 0x8A0C80, nullptr, nullptr, nullptr },
		{ "UIMessageBox", 0x100FFD0, 0x86D650, 0, nullptr, nullptr, nullptr, 0x87E5E0 },
		{ "UIMessageBoxAutoreturn", 0x10100A8, 0x86D680, 0, nullptr, nullptr, nullptr, 0x87E610 },
	};

	template <int K> void* __fastcall HookLoginDtor( void* self, void* /*edx*/, unsigned flags ){
		if( self == g_login[K].wnd ){
			g_login[K].wnd = nullptr;
			logger::Write( "%s fechada", g_login[K].name );
		}
		return g_login[K].origDtor( self, flags );
	}

	template <int K> void __fastcall HookLoginOnCreate( void* self, void* /*edx*/, int cx, int cy ){
		g_login[K].wnd = self; // antes: o cliente pode desenhar durante a criação
		g_login[K].origOnCreate( self, cx, cy );
		if( K == 0 ){
			g_inGame = false; // voltou ao login
			chardata::ClearCharServers();
		}
		client::WndRect r;
		client::LoginWindowRect( (client::LoginWnd)K, r );
		logger::Write( "%s aberta (%d,%d) %dx%d", g_login[K].name, r.x, r.y, r.w, r.h );
	}

	template <int K> void __fastcall HookLoginOnDraw( void* self, void* /*edx*/ ){
		g_login[K].wnd = self;
		g_login[K].lastDraw = GetTickCount();
		g_login[K].origOnDraw( self );
	}

	template <int K> void InstallLogin(){
		Tracked& t = g_login[K];
		if( t.onDraw ){
			if( Read<uintptr_t>( t.vt + client::VT_IDX_DTOR * 4 ) != t.dtor || Read<uintptr_t>( t.vt + client::VT_IDX_ONDRAW * 4 ) != t.onDraw ){
				logger::Write( "AVISO: %s diferente do esperado", t.name );
				return;
			}
			t.origDtor = reinterpret_cast<DtorFn>( t.dtor );
			t.origOnDraw = reinterpret_cast<void( __thiscall* )( void* )>( t.onDraw );
			PatchPointer( t.vt + client::VT_IDX_DTOR * 4, reinterpret_cast<uintptr_t>( &HookLoginDtor<K> ) );
			PatchPointer( t.vt + client::VT_IDX_ONDRAW * 4, reinterpret_cast<uintptr_t>( &HookLoginOnDraw<K> ) );
			return;
		}
		if( Read<uintptr_t>( t.vt + client::VT_IDX_DTOR * 4 ) != t.dtor || Read<uintptr_t>( t.vt + client::VT_IDX_ONCREATE * 4 ) != t.onCreate ){
			logger::Write( "AVISO: %s diferente do esperado; tela de entrada nativa", t.name );
			return;
		}
		t.origDtor = reinterpret_cast<DtorFn>( t.dtor );
		t.origOnCreate = reinterpret_cast<OnCreateFn>( t.onCreate );
		PatchPointer( t.vt + client::VT_IDX_DTOR * 4, reinterpret_cast<uintptr_t>( &HookLoginDtor<K> ) );
		PatchPointer( t.vt + client::VT_IDX_ONCREATE * 4, reinterpret_cast<uintptr_t>( &HookLoginOnCreate<K> ) );
	}
}

void* client::LoginWindow( LoginWnd which ){
	const Tracked& t = g_login[(int)which];
	if( !g_installed || t.wnd == nullptr ) return nullptr;
	return t.wnd;
}

void* client::LoginWindowRaw( LoginWnd which ){
	return g_login[(int)which].wnd;
}

bool client::MessageText( void* wnd, std::string& out ){
	if( wnd == nullptr ) return false;
	__try{
		uintptr_t s = reinterpret_cast<uintptr_t>( wnd ) + 0xB0;   // std::string do texto
		unsigned len = Read<unsigned>( s + 0x10 ), cap = Read<unsigned>( s + 0x14 );
		if( len > 1024 || cap < len ) return false;
		const char* p = cap >= 16 ? Read<const char*>( s ) : reinterpret_cast<const char*>( s );
		out.assign( p, len );
		return true;
	}__except( EXCEPTION_EXECUTE_HANDLER ){
		return false;
	}
}

bool client::AnyLoginWindow(){
	for( int k = 0; k <= 2; k++ ) if( g_login[k].wnd ) return g_installed; // login, servidores, aguarde
	return false;
}

namespace {
	bool ReadEditText( uintptr_t wnd, uintptr_t field, char* buf, size_t n ){
		__try{
			uintptr_t edit = Read<uintptr_t>( wnd + field );
			if( edit == 0 ) return false;
			const char* text = Read<const char*>( edit + 0xD8 );
			if( text == nullptr ) return false;
			size_t i = 0;
			for( ; i + 1 < n && text[i]; i++ ) buf[i] = text[i];
			buf[i] = 0;
			return true;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return false;
		}
	}
}

bool client::ReadLoginFields( std::string& id, int& passwordLength, bool& saveId ){
	uintptr_t w = reinterpret_cast<uintptr_t>( g_login[0].wnd );
	if( w == 0 ) return false;
	char buf[64];
	id = ReadEditText( w, 0x94, buf, sizeof( buf ) ) ? buf : "";
	passwordLength = ReadEditText( w, 0x98, buf, sizeof( buf ) ) ? (int)strlen( buf ) : 0;
	SecureZeroMemory( buf, sizeof( buf ) );
	__try{
		saveId = Read<uint8_t>( w + 0xA8 ) != 0;
	}__except( EXCEPTION_EXECUTE_HANDLER ){
		saveId = false;
	}
	return true;
}

bool client::ReadLoginPassword( char* buf, size_t n ){
	uintptr_t w = reinterpret_cast<uintptr_t>( g_login[0].wnd );
	return w != 0 && ReadEditText( w, 0x98, buf, n );
}

// Escala do tempo dos fades: decorrido * g_fadeMul / g_fadeDiv (255/255 = original)
static volatile unsigned g_fadeMul = 255, g_fadeDiv = 255;

// Substitui "sub eax, [0x1574D8C]": entra com eax = agora (timeGetTime), sai com o decorrido escalado.
// Preserva tudo menos eax; valores já grandes passam direto (sem estouro no div).
static __declspec( naked ) void FadeElapsed(){
	__asm {
		sub eax, dword ptr ds:[0x1574D8C]
		cmp eax, 0x100000
		jae fim
		push edx
		mul dword ptr [g_fadeMul]
		div dword ptr [g_fadeDiv]
		pop edx
	fim:
		ret
	}
}

bool client::InstallFastFade( int durationMs ){
	const uintptr_t sites[] = { 0xA5D9DC, 0xA5DBF9, 0xA5DC97 };
	const uint8_t sig[] = { 0x2B, 0x05, 0x8C, 0x4D, 0x57, 0x01 }; // sub eax, [0x1574D8C]
	for( uintptr_t a : sites ){
		if( memcmp( reinterpret_cast<void*>( a ), sig, sizeof( sig ) ) != 0 ){
			logger::Write( "ERRO: fade da troca de mapa diferente do esperado em %08X; mantido o original", (unsigned)a );
			return false;
		}
	}
	g_fadeMul = 255;
	g_fadeDiv = durationMs > 0 ? (unsigned)durationMs : 1;
	for( uintptr_t a : sites ){
		uint8_t patch[6] = { 0xE8, 0, 0, 0, 0, 0x90 }; // call FadeElapsed; nop
		int32_t rel = (int32_t)( reinterpret_cast<uintptr_t>( &FadeElapsed ) - ( a + 5 ) );
		memcpy( patch + 1, &rel, 4 );
		DWORD old;
		VirtualProtect( reinterpret_cast<void*>( a ), sizeof( patch ), PAGE_EXECUTE_READWRITE, &old );
		memcpy( reinterpret_cast<void*>( a ), patch, sizeof( patch ) );
		VirtualProtect( reinterpret_cast<void*>( a ), sizeof( patch ), old, &old );
	}
	FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
	logger::Write( "Fade da troca de mapa: %d ms (original 255 ms)", durationMs );
	return true;
}

bool client::InGame(){
	return g_inGame;
}

void client::SetInGame( bool on ){
	if( on != g_inGame ) logger::Write( on ? "Entrou no jogo" : "Telas de entrada" );
	g_inGame = on;
}

bool client::Install(){
	uintptr_t vt = VT_SELECTCHARWND;
	const uint8_t sig[] = { 0xA0, 0xC2, 0x0F, 0x5D, 0x01 };

	bool ok = Read<uintptr_t>( vt + VT_IDX_DTOR * 4 ) == FN_DTOR
		&& Read<uintptr_t>( vt + VT_IDX_ONCREATE * 4 ) == FN_ONCREATE
		&& Read<uintptr_t>( vt + VT_IDX_ONDRAW * 4 ) == FN_ONDRAW
		&& memcmp( reinterpret_cast<void*>( SIG_SELECT_PACKET ), sig, sizeof( sig ) ) == 0;

	if( !ok ){
		logger::Write( "ERRO: executavel diferente do esperado (Codex.exe 2025-06-04). Hooks do cliente desativados." );
		return false;
	}

	g_origDtor = reinterpret_cast<DtorFn>( FN_DTOR );
	g_origOnCreate = reinterpret_cast<OnCreateFn>( FN_ONCREATE );
	g_origOnDraw = reinterpret_cast<OnDrawFn>( FN_ONDRAW );

	ok = PatchPointer( vt + VT_IDX_DTOR * 4, reinterpret_cast<uintptr_t>( &HookDtor ) )
		&& PatchPointer( vt + VT_IDX_ONCREATE * 4, reinterpret_cast<uintptr_t>( &HookOnCreate ) )
		&& PatchPointer( vt + VT_IDX_ONDRAW * 4, reinterpret_cast<uintptr_t>( &HookOnDraw ) );

	// Tela de criação: opcional (só troca o fundo)
	uintptr_t mvt = VT_MAKECHARWND;
	if( Read<uintptr_t>( mvt + VT_IDX_DTOR * 4 ) == FN_MAKE_DTOR && Read<uintptr_t>( mvt + VT_IDX_ONCREATE * 4 ) == FN_MAKE_ONCREATE ){
		g_origMakeDtor = reinterpret_cast<DtorFn>( FN_MAKE_DTOR );
		g_origMakeOnCreate = reinterpret_cast<OnCreateFn>( FN_MAKE_ONCREATE );
		PatchPointer( mvt + VT_IDX_DTOR * 4, reinterpret_cast<uintptr_t>( &HookMakeDtor ) );
		PatchPointer( mvt + VT_IDX_ONCREATE * 4, reinterpret_cast<uintptr_t>( &HookMakeOnCreate ) );
	}else{
		logger::Write( "AVISO: tela de criacao nao reconhecida; o fundo dela nao sera trocado" );
	}

	InstallLogin<0>();
	InstallLogin<1>();
	InstallLogin<2>();
	InstallLogin<3>();
	InstallLogin<4>();
	InstallLogin<5>();
	InstallLogin<6>();
	InstallLogin<7>();

	logger::Write( ok ? "Hooks do cliente instalados" : "ERRO: falha ao aplicar hooks do cliente" );
	g_installed = ok;
	return ok;
}

bool client::IsSelectActive(){
	return g_installed && g_selectWnd != nullptr;
}

bool client::IsCreatingSelect(){
	return g_installed && g_creatingSelect;
}

bool client::IsMakeCharActive(){
	return g_installed && g_makeWnd != nullptr;
}

int client::SelectedSlot(){
	return g_installed ? Read<uint8_t>( G_SELECTED_SLOT ) : -1;
}

void* client::SelectWindow(){
	return g_selectWnd;
}

namespace {
	bool ReadWnd( uintptr_t wnd, client::WndRect& r ){
		__try{
			r.w = Read<int>( wnd + client::OFS_WND_WIDTH );
			r.h = Read<int>( wnd + client::OFS_WND_HEIGHT );
			r.x = Read<int>( wnd + client::OFS_WND_X );
			r.y = Read<int>( wnd + client::OFS_WND_Y );
			return r.w > 0 && r.h > 0 && r.w < 8192 && r.h < 8192;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return false;
		}
	}

	uintptr_t SlotWindow( int index ){
		uintptr_t base = reinterpret_cast<uintptr_t>( g_selectWnd );
		if( base == 0 || index < 0 || index >= client::SlotsOnPage() ){
			return 0;
		}
		__try{
			uintptr_t* slots = Read<uintptr_t*>( base + client::OFS_SLOT_WINDOWS );
			return slots != nullptr ? slots[index] : 0;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return 0;
		}
	}
}

int client::SlotsOnPage(){
	uintptr_t base = reinterpret_cast<uintptr_t>( g_selectWnd );
	if( base == 0 ) return 0;
	int n = Read<int>( base + OFS_SLOT_COUNT );
	return ( n > 0 && n <= 64 ) ? n : 0;
}

int client::IndexInPage(){
	uintptr_t base = reinterpret_cast<uintptr_t>( g_selectWnd );
	if( base == 0 ) return -1;
	int i = Read<int>( base + OFS_INDEX_IN_PAGE );
	return ( i >= 0 && i < SlotsOnPage() ) ? i : -1;
}

int client::SlotsPerPage(){
	uintptr_t base = reinterpret_cast<uintptr_t>( g_selectWnd );
	if( base == 0 ) return 0;
	int n = Read<uint8_t>( base + OFS_SLOTS_PER_PAGE );
	return n > 0 ? n : SlotsOnPage();
}

bool client::CardRect( int index, WndRect& out ){
	// Os cartões guardam a posição relativa à janela de seleção
	uintptr_t slot = SlotWindow( index );
	return slot != 0 && ReadWnd( slot, out );
}

void client::SetSelectedAction( int action, int frameMs, int attackAction, int attackEveryMs ){
	g_selectedAction = action;
	g_frameMs = frameMs;
	g_attackAction = attackAction;
	g_attackEveryMs = attackEveryMs;
	g_lastAttackEnd = GetTickCount();
	static bool hooked = false;
	if( action >= 0 && !hooked && g_installed ){
		hooked = true;
		InstallGetMotionHook();
	}
}

bool client::LoginWindowRect( LoginWnd which, WndRect& out ){
	uintptr_t wnd = reinterpret_cast<uintptr_t>( client::LoginWindow( which ) );
	return wnd != 0 && ReadWnd( wnd, out );
}

bool client::MakeWindowRect( WndRect& out ){
	uintptr_t wnd = reinterpret_cast<uintptr_t>( g_makeWnd );
	return wnd != 0 && ReadWnd( wnd, out );
}

void* client::MakeWindow(){
	return g_makeWnd;
}

bool client::ReadMakeState( MakeState& out ){
	uintptr_t w = reinterpret_cast<uintptr_t>( g_makeWnd );
	if( w == 0 ) return false;
	__try{
		out.dir = Read<int>( w + OFS_MAKE_DIR ) & 7;
		out.hair = Read<uint16_t>( w + OFS_MAKE_HAIR );
		out.hairColor = Read<uint16_t>( w + OFS_MAKE_HAIRCOLOR );
		out.sex = Read<uint8_t>( w + OFS_MAKE_SEX ) ? 1 : 0;
		return true;
	}__except( EXCEPTION_EXECUTE_HANDLER ){
		return false;
	}
}

namespace {
	bool Readable( uintptr_t p, size_t n ){
		MEMORY_BASIC_INFORMATION mbi;
		if( p < 0x10000 || !VirtualQuery( reinterpret_cast<void*>( p ), &mbi, sizeof( mbi ) ) ) return false;
		if( mbi.State != MEM_COMMIT || ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) ) return false;
		return p + n <= reinterpret_cast<uintptr_t>( mbi.BaseAddress ) + mbi.RegionSize;
	}

	void FindIn( uintptr_t base, size_t n, const char* text, const char* path, int depth ){
		size_t tl = strlen( text );
		if( !Readable( base, n ) ) return;
		const char* mem = reinterpret_cast<const char*>( base );
		for( size_t i = 0; i + tl <= n; i++ ){
			if( memcmp( mem + i, text, tl ) == 0 ){
				logger::Write( "Texto '%s' em %s+0x%X (end %08X)", text, path, (unsigned)i, (unsigned)( base + i ) );
			}
		}
		if( depth <= 0 ) return;
		for( size_t i = 0; i + 4 <= n; i += 4 ){
			uintptr_t p = *reinterpret_cast<const uintptr_t*>( mem + i );
			if( p >= 0x01000000 && p < 0x7FFF0000 && Readable( p, 0x200 ) ){
				char sub[128];
				sprintf( sub, "%s+0x%X->", path, (unsigned)i );
				FindIn( p, 0x200, text, sub, depth - 1 );
			}
		}
	}
}

namespace {
	bool ReadNameRaw( uintptr_t w, char* buf, size_t n ){
		__try{
			uintptr_t edit = Read<uintptr_t>( w + client::OFS_MAKE_NAMEEDIT );
			if( edit == 0 ) return false;
			const char* text = Read<const char*>( edit + client::OFS_EDIT_TEXT );
			if( text == nullptr ) return false;
			size_t i = 0;
			for( ; i + 1 < n && text[i] != 0; i++ ) buf[i] = text[i];
			buf[i] = 0;
			return true;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return false;
		}
	}
}

bool client::ReadMakeName( std::string& out ){
	uintptr_t w = reinterpret_cast<uintptr_t>( g_makeWnd );
	char buf[64];
	if( w == 0 || !ReadNameRaw( w, buf, sizeof( buf ) ) ) return false;
	out = buf;
	return true;
}

void client::DebugFindTextIn( void* wnd, size_t size, const char* text, const char* label ){
	if( wnd == nullptr ) return;
	__try{
		FindIn( reinterpret_cast<uintptr_t>( wnd ), size, text, label, 2 );
	}__except( EXCEPTION_EXECUTE_HANDLER ){
	}
}

void client::DebugFindText( const char* text ){
	uintptr_t w = reinterpret_cast<uintptr_t>( g_makeWnd );
	if( w == 0 ) return;
	__try{
		FindIn( w, 0x600, text, "make", 2 );
	}__except( EXCEPTION_EXECUTE_HANDLER ){
	}
}

namespace {
	using CharCtorFn = void*( __thiscall* )( void* self, void* wnd, int x, int y, int sex, int job, int job2, int body, int head,
		int headBottom, int headTop, int robe, int z1, int z2, int headMid, int bodyPalette, int headPalette, int action, int motion, int timer );
	using CharDrawFn = void( __thiscall* )( void* self, int flag );
	using CharDtorFn = void( __thiscall* )( void* self );

	bool DrawCharacterRaw( void* owner, int x, int y, int sex, int job, int hair, int hairColor, int action, int motion ){
		alignas( 16 ) unsigned char obj[0x200] = {};
		__try{
			reinterpret_cast<CharCtorFn>( client::FN_CHARVIEW_CTOR )( obj, owner, x, y, sex, job, job, 0, hair,
				0, 0, 0, 0, 0, 0, 0, hairColor, action, motion, (int)GetTickCount() );
			g_inSelectDraw = true;
			reinterpret_cast<CharDrawFn>( client::FN_CHARVIEW_DRAW )( obj, 1 );
			g_inSelectDraw = false;
			reinterpret_cast<CharDtorFn>( client::FN_CHARVIEW_DTOR )( obj );
			return true;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			g_inSelectDraw = false;
			return false;
		}
	}
}

bool client::DrawCharacter( void* owner, int x, int y, int sex, int job, int hair, int hairColor, int action, int motion ){
	static bool failed = false;
	if( failed || owner == nullptr || !g_installed ) return false;
	if( !DrawCharacterRaw( owner, x, y, sex, job, hair, hairColor, action, motion ) ){
		failed = true;
		logger::Write( "ERRO: excecao ao desenhar a previa do personagem; previa desativada" );
		return false;
	}
	return true;
}

void client::LogGeometry(){
	WndRect wnd;
	if( !g_selectWnd || !ReadWnd( reinterpret_cast<uintptr_t>( g_selectWnd ), wnd ) ){
		logger::Write( "Geometria: janela indisponivel" );
		return;
	}
	logger::Write( "Geometria: janela (%d,%d) %dx%d, %d cartoes, %d por pagina, indice %d",
		wnd.x, wnd.y, wnd.w, wnd.h, SlotsOnPage(), SlotsPerPage(), IndexInPage() );
	for( int i = 0; i < SlotsOnPage(); i++ ){
		WndRect c;
		if( CardRect( i, c ) ){
			logger::Write( "  cartao %d: (%d,%d) %dx%d", i, c.x, c.y, c.w, c.h );
		}
	}
}
