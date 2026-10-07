#include "input.hpp"

#include <string>

#include "config.hpp"
#include "log.hpp"

namespace {
	// IAT do Codex.exe: user32!GetPhysicalCursorPos
	constexpr uintptr_t IAT_GETPHYSICALCURSORPOS = 0xFA1644;

	using GetPhysicalCursorPosFn = BOOL( WINAPI* )( LPPOINT );
	GetPhysicalCursorPosFn o_GetPhysicalCursorPos = nullptr;

	HWND g_hwnd = nullptr;
	WNDPROC g_origWndProc = nullptr;
	input::RedirectFn g_redirect = nullptr;
	float g_vpWidth = 0.0f, g_vpHeight = 0.0f;
	input::Point g_real, g_reported;
	bool g_leftDown = false;
	bool g_photoMode = false;
	int g_wheel = 0;
	volatile bool g_textCapture = false;
	std::wstring g_typed;          // texto digitado enquanto um campo nosso tem o foco

	// cliente (pixels da janela) <-> viewport (backbuffer)
	void Scale( float& sx, float& sy ){
		RECT rc;
		GetClientRect( g_hwnd, &rc );
		sx = ( rc.right > 0 && g_vpWidth > 0 ) ? g_vpWidth / rc.right : 1.0f;
		sy = ( rc.bottom > 0 && g_vpHeight > 0 ) ? g_vpHeight / rc.bottom : 1.0f;
	}

	input::Point ClientToViewport( POINT p ){
		float sx, sy;
		Scale( sx, sy );
		return { p.x * sx, p.y * sy };
	}

	POINT ViewportToClient( const input::Point& p ){
		float sx, sy;
		Scale( sx, sy );
		return { (LONG)( p.x / sx + 0.5f ), (LONG)( p.y / sy + 0.5f ) };
	}

	// Aplica o redirecionamento a um ponto em coordenadas do cliente.
	POINT Redirect( POINT client ){
		g_real = ClientToViewport( client );
		input::Point target;
		if( g_redirect != nullptr && g_redirect( g_real, target ) ){
			g_reported = target;
			return ViewportToClient( target );
		}
		g_reported = g_real;
		return client;
	}

	BOOL WINAPI Hook_GetPhysicalCursorPos( LPPOINT pt ){
		BOOL ok = o_GetPhysicalCursorPos( pt );
		if( ok && pt != nullptr && g_hwnd != nullptr ){
			POINT client = *pt;
			ScreenToClient( g_hwnd, &client );
			POINT mapped = Redirect( client );
			if( mapped.x != client.x || mapped.y != client.y ){
				ClientToScreen( g_hwnd, &mapped );
				*pt = mapped;
			}
		}
		return ok;
	}

	volatile bool g_diagRequest = false;
	volatile bool g_escCapture = false;
	float g_tgX = 0, g_tgY = 0, g_tgW = 0, g_tgH = 0;
	bool g_tgPressed = false;
	volatile bool g_tgClicked = false;

	bool InToggle( LPARAM lp ){
		if( g_tgW <= 0 ) return false;
		POINT c = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
		input::Point p = ClientToViewport( c );
		return p.x >= g_tgX && p.x < g_tgX + g_tgW && p.y >= g_tgY && p.y < g_tgY + g_tgH;
	}
	volatile bool g_escPressed = false;

	LRESULT CALLBACK WndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ){
		if( g_textCapture ){
			if( msg == WM_CHAR ){
				wchar_t wc = 0;
				char c = (char)wp;
				MultiByteToWideChar( 1252, 0, &c, 1, &wc, 1 );
				if( wp == 8 ) g_typed += L'\b';
				else if( wp == 13 || wp == 27 ) g_typed += (wchar_t)wp;
				else if( wp >= 32 ) g_typed += wc;
				return 0;
			}
			if( msg == WM_KEYDOWN || msg == WM_KEYUP ){
				return 0; // o jogo não recebe as teclas enquanto o campo está ativo
			}
		}
		switch( msg ){
			case WM_LBUTTONDOWN:
			case WM_LBUTTONDBLCLK:
				g_leftDown = true;
				break;
			case WM_LBUTTONUP:
				g_leftDown = false;
				break;
			case WM_KEYUP:
				if( wp == VK_ESCAPE && g_escCapture ) return 0;
				break;
			case WM_CHAR:
				if( wp == 27 && g_escCapture ) return 0;
				break;
			case WM_MOUSEWHEEL:
				g_wheel += GET_WHEEL_DELTA_WPARAM( wp );
				break;
			case WM_KEYDOWN:
				if( wp == VK_ESCAPE && g_escCapture ){
					if( !( lp & ( 1 << 30 ) ) ) g_escPressed = true;
					return 0;
				}
				if( wp == VK_F11 && config::Get().debug ){
					g_diagRequest = true; // registra um quadro no log
					return 0;
				}
				if( wp == VK_PAUSE && !( lp & ( 1 << 30 ) ) ){
					g_photoMode = !g_photoMode;
					logger::Write( "Modo foto %s", g_photoMode ? "ligado" : "desligado" );
					return 0;
				}
				break;
		}

		// chave de modo: o clique é nosso
		if( msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ){
			if( InToggle( lp ) ){
				g_tgPressed = true;
				g_real = ClientToViewport( { (short)LOWORD( lp ), (short)HIWORD( lp ) } );
				return 0;
			}
		}else if( msg == WM_LBUTTONUP && g_tgPressed ){
			g_tgPressed = false;
			if( InToggle( lp ) ) g_tgClicked = true;
			return 0;
		}

		switch( msg ){
			case WM_MOUSEMOVE:
			case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: {
				POINT client = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
				POINT mapped = Redirect( client );
				lp = MAKELPARAM( mapped.x, mapped.y );
				if( msg != WM_MOUSEMOVE && config::Get().debug ){
					logger::Write( "mouse msg=0x%X real=(%d,%d) cliente ve=(%d,%d)", msg, client.x, client.y, mapped.x, mapped.y );
				}
				break;
			}
		}
		return CallWindowProcA( g_origWndProc, hwnd, msg, wp, lp );
	}
}

void input::Install( HWND hwnd ){
	if( g_hwnd != nullptr || hwnd == nullptr ){
		return;
	}
	g_hwnd = hwnd;
	g_origWndProc = reinterpret_cast<WNDPROC>( SetWindowLongA( hwnd, GWL_WNDPROC, reinterpret_cast<LONG>( &WndProc ) ) );

	HMODULE user32 = GetModuleHandleW( L"user32.dll" );
	FARPROC real = user32 ? GetProcAddress( user32, "GetPhysicalCursorPos" ) : nullptr;
	uintptr_t* slot = reinterpret_cast<uintptr_t*>( IAT_GETPHYSICALCURSORPOS );
	if( real != nullptr && *slot == reinterpret_cast<uintptr_t>( real ) ){
		o_GetPhysicalCursorPos = reinterpret_cast<GetPhysicalCursorPosFn>( real );
		DWORD old;
		VirtualProtect( slot, sizeof( *slot ), PAGE_READWRITE, &old );
		*slot = reinterpret_cast<uintptr_t>( &Hook_GetPhysicalCursorPos );
		VirtualProtect( slot, sizeof( *slot ), old, &old );
		logger::Write( "Entrada interceptada (janela %p)", hwnd );
	}else{
		logger::Write( "AVISO: IAT de GetPhysicalCursorPos nao confere; so as mensagens da janela serao redirecionadas" );
	}
}

void input::SetRedirect( RedirectFn fn ){
	g_redirect = fn;
}

void input::SetViewport( float width, float height ){
	g_vpWidth = width;
	g_vpHeight = height;
}

input::Point input::RealCursor(){
	return g_real;
}

input::Point input::CursorOffset(){
	return { g_real.x - g_reported.x, g_real.y - g_reported.y };
}

int input::TakeWheel(){
	int w = g_wheel;
	g_wheel = 0;
	return w;
}

bool input::PhotoMode(){
	return g_photoMode;
}

bool input::IsLeftDown(){
	return g_leftDown;
}

bool input::TakeDiagRequest(){
	bool r = g_diagRequest;
	g_diagRequest = false;
	return r;
}

void input::SetEscapeCapture( bool on ){
	g_escCapture = on;
	if( !on ) g_escPressed = false;
}

bool input::TakeEscape(){
	bool r = g_escPressed;
	g_escPressed = false;
	return r;
}

void input::QuitGame(){
	// o cliente fecha pela mensagem padrão da janela (sem a caixa de confirmação dele)
	if( g_hwnd ) PostMessageA( g_hwnd, WM_CLOSE, 0, 0 );
}

void input::SetToggleRect( float x, float y, float w, float h ){
	g_tgX = x; g_tgY = y; g_tgW = w; g_tgH = h;
	if( w <= 0 ) g_tgPressed = false;
}

bool input::TakeToggleClick(){
	bool r = g_tgClicked;
	g_tgClicked = false;
	return r;
}

void input::PressKey( unsigned vk ){
	if( g_hwnd == nullptr ) return;
	UINT scan = MapVirtualKeyW( vk, MAPVK_VK_TO_VSC );
	PostMessageW( g_hwnd, WM_KEYDOWN, vk, 1 | ( scan << 16 ) );
	PostMessageW( g_hwnd, WM_KEYUP, vk, 1 | ( scan << 16 ) | ( 3u << 30 ) );
}

void input::SetTextCapture( bool on ){
	g_textCapture = on;
	if( !on ) g_typed.clear();
}

std::wstring input::TakeTyped(){
	std::wstring t;
	t.swap( g_typed );
	return t;
}
