// Aplicador Codex: embute a interface O Codex (login, seleção e criação) no executável hexed (gerado pelo WARP).
// O resultado vai para "Aplicados\<data>\" ao lado do aplicador: Codex.exe + char.grf, prontos para copiar no cliente.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "embutir.hpp"
#include "resource.h"

#pragma comment( lib, "comctl32.lib" )
#pragma comment( lib, "comdlg32.lib" )
#pragma comment( lib, "shell32.lib" )
#pragma comment( lib, "shlwapi.lib" )
#pragma comment( lib, "gdiplus.lib" )
#pragma comment( linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"" )

namespace {
	// ---------------------------------------------------------------- tema O Codex
	const COLORREF FUNDO = RGB( 7, 12, 22 ), FUNDO_TOPO = RGB( 14, 26, 46 ), PAINEL = RGB( 11, 20, 36 );
	const COLORREF BORDA = RGB( 32, 62, 96 ), OURO = RGB( 240, 189, 69 ), OURO_CLARO = RGB( 255, 225, 140 );
	const COLORREF AZUL = RGB( 18, 191, 255 ), AZUL_CLARO = RGB( 124, 227, 255 ), TEXTO = RGB( 244, 247, 251 );
	const COLORREF APAGADO = RGB( 159, 178, 196 ), ESCURO = RGB( 18, 12, 0 ), ERRO = RGB( 255, 120, 120 ), OK = RGB( 110, 225, 140 );

	enum { ID_ENTRADA = 1001, ID_PROCURAR, ID_APLICAR, ID_ABRIR, ID_LOG, ID_OPCAO = 1100 };
	const int W = 720, H = 1024;

	// ---------------------------------------------------------------- alterações (caixas de seleção)
	struct Opcao {
		unsigned bit;
		const wchar_t* nome;
		const wchar_t* desc;
		bool marcada;
		HWND h;
	};
	Opcao g_opcoes[] = {
		{ embutir::LOGIN, L"Tela de login", L"Servidores, login e \"conectando\" no tema", true, nullptr },
		{ embutir::SELECAO, L"Seleção de personagem", L"Mapa 3D panorâmico, lista, ficha e balão", true, nullptr },
		{ embutir::CRIACAO, L"Criação de personagem", L"Painéis novos e prévia no mapa", true, nullptr },
		{ embutir::CHAVE, L"Chave Personalizada/Original", L"O jogador pode voltar à seleção original", true, nullptr },
		{ embutir::COMBATE, L"Postura de combate", L"Selecionado em guarda e com golpe", true, nullptr },
		{ embutir::FADE, L"Transição rápida", L"Troca de mapa e @refresh sem demora", true, nullptr },
		{ embutir::STATUS, L"Barra de status", L"HP, SP e EXP no estilo Ragnarok Zero", true, nullptr },
		{ embutir::ITEM, L"Janela de item", L"Descrição do item no tema", true, nullptr },
		{ embutir::EQUIP, L"Equipamentos", L"Alt+Q com atributos e informações", true, nullptr },
		{ embutir::INVENTARIO, L"Inventário", L"Alt+E com abas, busca e grade", true, nullptr },
		{ embutir::HABILIDADES, L"Habilidades", L"Alt+S em árvore por classe", true, nullptr },
	};
	const int N_OPCOES = sizeof( g_opcoes ) / sizeof( g_opcoes[0] );

	bool OpcaoAtiva( int i ){
		// criação, chave e combate dependem da seleção
		if( g_opcoes[i].bit & ( embutir::CRIACAO | embutir::CHAVE | embutir::COMBATE ) ){
			return g_opcoes[1].marcada;
		}
		return true;
	}

	unsigned Alteracoes(){
		unsigned f = 0;
		for( int i = 0; i < N_OPCOES; i++ ) if( g_opcoes[i].marcada && OpcaoAtiva( i ) ) f |= g_opcoes[i].bit;
		return f;
	}

	std::wstring NomesAlteracoes( unsigned f ){
		std::wstring s;
		for( const Opcao& o : g_opcoes ) if( f & o.bit ) s += ( s.empty() ? L"" : L", " ) + std::wstring( o.nome );
		return s.empty() ? L"(nenhuma)" : s;
	}

	HWND g_wnd, g_entrada, g_procurar, g_aplicar, g_abrir, g_log;
	HFONT g_titulo, g_sub, g_rotulo, g_texto, g_botao, g_botaoGrande, g_mono;
	HBRUSH g_brPainel, g_brFundo;
	Gdiplus::Image* g_logo = nullptr;
	int g_hover = 0;
	std::wstring g_ultimaPasta;

	std::wstring PastaDoApp(){
		wchar_t p[MAX_PATH] = {};
		GetModuleFileNameW( nullptr, p, MAX_PATH );
		wchar_t* b = wcsrchr( p, L'\\' );
		if( b ) b[1] = 0;
		return p;
	}
	std::wstring PastaAplicados(){ return PastaDoApp() + L"Aplicados\\"; }
	bool Existe( const std::wstring& p ){ return GetFileAttributesW( p.c_str() ) != INVALID_FILE_ATTRIBUTES; }

	bool Ler( const std::wstring& p, std::vector<unsigned char>& out ){
		HANDLE h = CreateFileW( p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
		if( h == INVALID_HANDLE_VALUE ) return false;
		LARGE_INTEGER t;
		GetFileSizeEx( h, &t );
		out.resize( (size_t)t.QuadPart );
		DWORD lido = 0;
		bool ok = ReadFile( h, out.data(), (DWORD)out.size(), &lido, nullptr ) && lido == out.size();
		CloseHandle( h );
		return ok;
	}

	bool Gravar( const std::wstring& p, const void* d, size_t n ){
		HANDLE h = CreateFileW( p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr );
		if( h == INVALID_HANDLE_VALUE ) return false;
		DWORD esc = 0;
		bool ok = WriteFile( h, d, (DWORD)n, &esc, nullptr ) && esc == n;
		CloseHandle( h );
		return ok;
	}

	bool Recurso( int id, const unsigned char*& p, DWORD& n ){
		HRSRC r = FindResourceW( nullptr, MAKEINTRESOURCEW( id ), RT_RCDATA );
		if( !r ) return false;
		p = static_cast<const unsigned char*>( LockResource( LoadResource( nullptr, r ) ) );
		n = SizeofResource( nullptr, r );
		return p && n;
	}

	std::wstring DataInterface(){
		const unsigned char* p;
		DWORD n;
		if( !Recurso( IDR_INTERFACE, p, n ) ) return L"?";
		auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>( p + reinterpret_cast<const IMAGE_DOS_HEADER*>( p )->e_lfanew );
		time_t t = nt->FileHeader.TimeDateStamp;
		tm lt;
		localtime_s( &lt, &t );
		wchar_t b[64];
		wcsftime( b, 64, L"%d/%m/%Y %H:%M", &lt );
		return b;
	}

	// ---------------------------------------------------------------- trabalho
	// Gera Aplicados\<data>\Codex.exe (+ char.grf + instruções). Devolve a pasta em "pasta".
	bool Executar( const std::wstring& entrada, bool copiarGrf, unsigned alteracoes, std::wstring& pasta, void ( *log )( const std::wstring&, int ) ){
		log( L"Executável hexed: " + entrada, 0 );
		log( L"Alterações: " + NomesAlteracoes( alteracoes ), 0 );
		if( !( alteracoes & ( embutir::LOGIN | embutir::SELECAO ) ) ){
			log( L"Marque pelo menos a tela de login ou a seleção de personagem.", 2 );
			return false;
		}
		std::vector<unsigned char> exe, out;
		const unsigned char* pd;
		DWORD nd;
		if( !Ler( entrada, exe ) ){ log( L"Não foi possível ler o executável escolhido.", 2 ); return false; }
		if( !Recurso( IDR_INTERFACE, pd, nd ) ){ log( L"Interface não encontrada dentro do aplicador.", 2 ); return false; }
		std::vector<unsigned char> dll( pd, pd + nd );
		embutir::Resultado r = embutir::Aplicar( exe, dll, alteracoes, out );
		for( const auto& l : r.log ) log( l, 0 );
		if( !r.ok ){ log( r.mensagem, 2 ); return false; }

		wchar_t data[64];
		time_t t = time( nullptr );
		tm lt;
		localtime_s( &lt, &t );
		wcsftime( data, 64, L"%Y-%m-%d %Hh%Mm%Ss", &lt );
		CreateDirectoryW( PastaAplicados().c_str(), nullptr );
		pasta = PastaAplicados() + data + L"\\";
		if( !CreateDirectoryW( pasta.c_str(), nullptr ) ){ log( L"Não foi possível criar a pasta " + pasta, 2 ); return false; }
		if( !Gravar( pasta + L"Codex.exe", out.data(), out.size() ) ){ log( L"Não foi possível gravar o Codex.exe.", 2 ); return false; }
		log( L"Codex.exe gerado.", 0 );
		if( copiarGrf ){
			std::wstring grf = PastaDoApp() + L"char.grf";
			if( !Existe( grf ) ) log( L"char.grf não está ao lado do aplicador; não foi incluída.", 1 );
			else if( CopyFileW( grf.c_str(), ( pasta + L"char.grf" ).c_str(), FALSE ) ) log( L"char.grf incluída.", 0 );
			else log( L"Não foi possível copiar a char.grf.", 1 );
		}
		std::wstring leia =
			L"COMO INSTALAR\r\n"
			L"=============\r\n\r\n"
			L"Copie os arquivos desta pasta para a pasta do cliente, substituindo os existentes:\r\n"
			L"  - Codex.exe  (executável com a interface O Codex embutida)\r\n"
			L"  - char.grf   (mapa 3D, ícones, fontes e configurações da interface)\r\n\r\n"
			L"Alterações aplicadas neste executável:\r\n";
		for( const Opcao& o : g_opcoes ){
			leia += std::wstring( ( alteracoes & o.bit ) ? L"  [x] " : L"  [ ] " ) + o.nome + L"\r\n";
		}
		leia += L"\r\nNão é preciso nenhuma .dll. Se houver uma d3d9.dll ou codex_ui.dll antiga da interface\r\n"
			L"na pasta do cliente, apague-as.\r\n";
		{
			int n = WideCharToMultiByte( CP_UTF8, 0, leia.c_str(), (int)leia.size(), nullptr, 0, nullptr, nullptr );
			std::string u( n, '\0' );
			WideCharToMultiByte( CP_UTF8, 0, leia.c_str(), (int)leia.size(), &u[0], n, nullptr, nullptr );
			u.insert( 0, "\xEF\xBB\xBF" );
			Gravar( pasta + L"COMO INSTALAR.txt", u.data(), u.size() );
		}
		log( L"Pronto! Copie o conteúdo da pasta para o cliente e substitua os arquivos.", 3 );
		log( pasta, 3 );
		return true;
	}

	// ---------------------------------------------------------------- janela
	struct Linha { std::wstring texto; int tipo; };
	std::vector<Linha> g_linhas;

	void LogJanela( const std::wstring& s, int tipo ){
		const wchar_t* pre[] = { L"•  ", L"!  ", L"✕  ", L"✓  " };
		int n = GetWindowTextLengthW( g_log );
		SendMessageW( g_log, EM_SETSEL, n, n );
		SendMessageW( g_log, EM_REPLACESEL, FALSE, (LPARAM)( pre[tipo] + s + L"\r\n" ).c_str() );
		g_linhas.push_back( { s, tipo } );
	}

	std::wstring Texto( HWND h ){
		int n = GetWindowTextLengthW( h );
		std::wstring s( n, L'\0' );
		GetWindowTextW( h, &s[0], n + 1 );
		return s;
	}

	HFONT Fonte( const wchar_t* face, int px, int peso ){
		return CreateFontW( -px, 0, 0, 0, peso, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH, face );
	}

	void Retangulo( HDC dc, RECT r, COLORREF cor ){
		HBRUSH b = CreateSolidBrush( cor );
		FillRect( dc, &r, b );
		DeleteObject( b );
	}

	void Moldura( HDC dc, RECT r, COLORREF cor, int esp = 1 ){
		for( int i = 0; i < esp; i++ ){
			RECT a = { r.left + i, r.top + i, r.right - i, r.bottom - i };
			HBRUSH b = CreateSolidBrush( cor );
			FrameRect( dc, &a, b );
			DeleteObject( b );
		}
	}

	void Degrade( HDC dc, RECT r, COLORREF a, COLORREF b, bool horizontal ){
		TRIVERTEX v[2] = {
			{ r.left, r.top, (COLOR16)( GetRValue( a ) << 8 ), (COLOR16)( GetGValue( a ) << 8 ), (COLOR16)( GetBValue( a ) << 8 ), 0xFF00 },
			{ r.right, r.bottom, (COLOR16)( GetRValue( b ) << 8 ), (COLOR16)( GetGValue( b ) << 8 ), (COLOR16)( GetBValue( b ) << 8 ), 0xFF00 } };
		GRADIENT_RECT g = { 0, 1 };
		GradientFill( dc, v, 2, &g, 1, horizontal ? GRADIENT_FILL_RECT_H : GRADIENT_FILL_RECT_V );
	}

	void Escrever( HDC dc, HFONT f, COLORREF cor, const std::wstring& s, RECT r, UINT fmt = DT_LEFT | DT_TOP | DT_SINGLELINE ){
		HGDIOBJ old = SelectObject( dc, f );
		SetTextColor( dc, cor );
		SetBkMode( dc, TRANSPARENT );
		DrawTextW( dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX );
		SelectObject( dc, old );
	}

	RECT Ret( int x, int y, int w, int h ){ return RECT{ x, y, x + w, y + h }; }

	void Pintar( HDC dc ){
		RECT tudo = { 0, 0, W, H };
		Retangulo( dc, tudo, FUNDO );
		// cabeçalho
		Degrade( dc, Ret( 0, 0, W, 112 ), FUNDO_TOPO, FUNDO, false );
		if( g_logo ){
			Gdiplus::Graphics g( dc );
			g.SetInterpolationMode( Gdiplus::InterpolationModeHighQualityBicubic );
			g.DrawImage( g_logo, 24, 18, 76, 76 );
		}
		Escrever( dc, g_titulo, OURO, L"O CODEX", Ret( 116, 22, 400, 44 ) );
		Escrever( dc, g_sub, APAGADO, L"Aplicador da interface", Ret( 118, 66, 400, 22 ) );
		Escrever( dc, g_rotulo, APAGADO, L"INTERFACE", Ret( W - 224, 34, 200, 16 ), DT_RIGHT | DT_SINGLELINE );
		Escrever( dc, g_texto, OURO_CLARO, DataInterface(), Ret( W - 224, 52, 200, 20 ), DT_RIGHT | DT_SINGLELINE );
		// linha de destaque azul-dourado
		Degrade( dc, Ret( 0, 112, W / 4, 2 ), FUNDO, AZUL, true );
		Degrade( dc, Ret( W / 4, 112, W / 4, 2 ), AZUL, OURO, true );
		Degrade( dc, Ret( W / 2, 112, W / 4, 2 ), OURO, AZUL, true );
		Degrade( dc, Ret( 3 * W / 4, 112, W / 4, 2 ), AZUL, FUNDO, true );

		// painel principal
		RECT p = Ret( 20, 134, W - 40, 262 );
		Retangulo( dc, p, PAINEL );
		Moldura( dc, p, BORDA );
		Escrever( dc, g_rotulo, APAGADO, L"1   EXECUTÁVEL HEXED  ·  GERADO PELO WARP", Ret( 40, 152, 600, 18 ) );
		RECT campo = Ret( 40, 176, 506, 36 );
		Retangulo( dc, campo, RGB( 3, 6, 12 ) );
		Moldura( dc, campo, GetFocus() == g_entrada ? OURO : BORDA );
		Escrever( dc, g_texto, APAGADO,
			L"É o executável que o WARP gera depois de aplicar os patches (o \"diffado\"). Também aceita um Codex.exe que "
			L"já recebeu a interface: ele é refeito com as alterações marcadas. Dá para arrastar o arquivo para cá.",
			Ret( 40, 222, W - 80, 60 ), DT_LEFT | DT_TOP | DT_WORDBREAK );
		Escrever( dc, g_rotulo, APAGADO, L"2   DESTINO", Ret( 40, 294, 600, 18 ) );
		Escrever( dc, g_texto, TEXTO, L"Aplicados\\<data e hora>\\   →   Codex.exe  +  char.grf", Ret( 40, 316, W - 80, 20 ) );
		Escrever( dc, g_texto, APAGADO, L"Depois, copie o conteúdo da pasta gerada para a pasta do cliente, substituindo os arquivos.",
			Ret( 40, 342, W - 80, 20 ) );

		// alterações
		RECT pa = Ret( 20, 410, W - 40, 358 );
		Retangulo( dc, pa, PAINEL );
		Moldura( dc, pa, BORDA );
		Escrever( dc, g_rotulo, APAGADO, L"3   ALTERAÇÕES QUE SERÃO APLICADAS", Ret( 40, 428, 600, 18 ) );
		Escrever( dc, g_texto, RGB( 110, 128, 148 ), L"Criação, chave e postura de combate dependem da seleção de personagem.",
			Ret( 40, 740, W - 80, 20 ) );

		// registro
		Escrever( dc, g_rotulo, APAGADO, L"REGISTRO", Ret( 24, 848, 200, 18 ) );
		RECT lg = Ret( 20, 868, W - 40, 100 );
		Retangulo( dc, lg, RGB( 3, 6, 12 ) );
		Moldura( dc, lg, BORDA );
		Escrever( dc, g_texto, RGB( 90, 106, 124 ), L"© O Codex", Ret( W - 220, H - 44, 200, 20 ), DT_RIGHT | DT_SINGLELINE );
	}

	void DesenharBotao( const DRAWITEMSTRUCT* d ){
		HDC dc = d->hDC;
		RECT r = d->rcItem;
		int id = (int)d->CtlID;
		bool hover = g_hover == id, apertado = ( d->itemState & ODS_SELECTED ) != 0, ativo = !( d->itemState & ODS_DISABLED );
		if( id >= ID_OPCAO && id < ID_OPCAO + N_OPCOES ){
			int i = id - ID_OPCAO;
			bool on = OpcaoAtiva( i ), marcada = g_opcoes[i].marcada && on;
			Retangulo( dc, r, PAINEL );
			if( hover && on ) Retangulo( dc, r, RGB( 14, 30, 50 ) );
			RECT cx = Ret( r.left + 8, r.top + 12, 20, 20 );
			Retangulo( dc, cx, marcada ? OURO : RGB( 3, 6, 12 ) );
			Moldura( dc, cx, !on ? RGB( 40, 52, 66 ) : ( marcada ? OURO_CLARO : ( hover ? AZUL_CLARO : BORDA ) ) );
			if( marcada ) Escrever( dc, g_botao, ESCURO, L"✓", cx, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
			Escrever( dc, g_botao, on ? ( marcada ? TEXTO : APAGADO ) : RGB( 70, 84, 100 ), g_opcoes[i].nome,
				Ret( r.left + 38, r.top + 6, r.right - r.left - 40, 20 ) );
			Escrever( dc, g_texto, on ? RGB( 120, 138, 158 ) : RGB( 60, 72, 86 ), g_opcoes[i].desc,
				Ret( r.left + 38, r.top + 26, r.right - r.left - 40, 18 ) );
			return;
		}
		wchar_t txt[128];
		GetWindowTextW( d->hwndItem, txt, 128 );
		if( id == ID_APLICAR ){
			Retangulo( dc, r, FUNDO );
			RECT b = r;
			if( apertado ) OffsetRect( &b, 0, 1 );
			Degrade( dc, b, ativo ? ( hover ? RGB( 255, 236, 170 ) : OURO_CLARO ) : RGB( 120, 110, 80 ), ativo ? OURO : RGB( 90, 80, 50 ), false );
			Moldura( dc, b, RGB( 255, 240, 200 ) );
			Escrever( dc, g_botaoGrande, ESCURO, txt, b, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
		}else{
			Retangulo( dc, r, hover ? RGB( 10, 40, 60 ) : RGB( 3, 6, 12 ) );
			Moldura( dc, r, hover ? AZUL_CLARO : RGB( 22, 110, 150 ) );
			Escrever( dc, g_botao, AZUL_CLARO, txt, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
		}
	}

	LRESULT CALLBACK BotaoProc( HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ){
		if( m == WM_MOUSEMOVE ){
			int id = GetDlgCtrlID( h );
			if( g_hover != id ){
				g_hover = id;
				InvalidateRect( h, nullptr, FALSE );
				TRACKMOUSEEVENT t = { sizeof( t ), TME_LEAVE, h, 0 };
				TrackMouseEvent( &t );
			}
		}else if( m == WM_MOUSELEAVE ){
			g_hover = 0;
			InvalidateRect( h, nullptr, FALSE );
		}else if( m == WM_SETCURSOR ){
			SetCursor( LoadCursor( nullptr, IDC_HAND ) );
			return TRUE;
		}
		return DefSubclassProc( h, m, wp, lp );
	}

	std::wstring EscolherArquivo( HWND dono, const std::wstring& inicial ){
		wchar_t arq[MAX_PATH] = {};
		wcsncpy_s( arq, inicial.c_str(), _TRUNCATE );
		OPENFILENAMEW of = { sizeof( of ) };
		of.hwndOwner = dono;
		of.lpstrFilter = L"Executável (*.exe)\0*.exe\0Todos os arquivos\0*.*\0";
		of.lpstrFile = arq;
		of.nMaxFile = MAX_PATH;
		of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
		of.lpstrTitle = L"Executável hexed (gerado pelo WARP)";
		return GetOpenFileNameW( &of ) ? arq : L"";
	}

	HWND Controle( const wchar_t* cls, const wchar_t* txt, DWORD estilo, RECT r, int id, HFONT f ){
		HWND c = CreateWindowExW( 0, cls, txt, WS_CHILD | WS_VISIBLE | estilo, r.left, r.top, r.right - r.left, r.bottom - r.top,
			g_wnd, (HMENU)(INT_PTR)id, nullptr, nullptr );
		SendMessageW( c, WM_SETFONT, (WPARAM)f, TRUE );
		if( lstrcmpW( cls, L"BUTTON" ) == 0 ) SetWindowSubclass( c, BotaoProc, 1, 0 );
		return c;
	}

	// marcações lembradas entre usos (aplicador.ini ao lado do programa)
	void SalvarOpcoes(){
		std::wstring ini = PastaDoApp() + L"aplicador.ini";
		for( const Opcao& o : g_opcoes ){
			WritePrivateProfileStringW( L"alteracoes", std::to_wstring( o.bit ).c_str(), o.marcada ? L"1" : L"0", ini.c_str() );
		}
	}

	void CarregarOpcoes(){
		std::wstring ini = PastaDoApp() + L"aplicador.ini";
		for( Opcao& o : g_opcoes ){
			o.marcada = GetPrivateProfileIntW( L"alteracoes", std::to_wstring( o.bit ).c_str(), 1, ini.c_str() ) != 0;
		}
	}

	void Aplicar(){
		std::wstring e = Texto( g_entrada );
		if( e.empty() ){
			MessageBoxW( g_wnd, L"Escolha o executável hexed (gerado pelo WARP).", L"O Codex", MB_ICONWARNING );
			return;
		}
		SetWindowTextW( g_log, L"" );
		EnableWindow( g_aplicar, FALSE );
		HCURSOR old = SetCursor( LoadCursor( nullptr, IDC_WAIT ) );
		std::wstring pasta;
		bool ok = Executar( e, true, Alteracoes(), pasta, &LogJanela );
		SetCursor( old );
		EnableWindow( g_aplicar, TRUE );
		if( ok ){
			g_ultimaPasta = pasta;
			if( MessageBoxW( g_wnd, L"Interface aplicada!\n\nOs arquivos estão na pasta Aplicados. Abrir a pasta agora?",
				L"O Codex", MB_ICONINFORMATION | MB_YESNO ) == IDYES ){
				ShellExecuteW( g_wnd, L"open", pasta.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
			}
		}else{
			MessageBoxW( g_wnd, L"Não foi possível aplicar. Veja o registro na janela.", L"O Codex", MB_ICONERROR );
		}
	}

	LRESULT CALLBACK Proc( HWND h, UINT m, WPARAM wp, LPARAM lp ){
		switch( m ){
			case WM_CREATE: {
				g_wnd = h;
				g_titulo = Fonte( L"Cinzel", 34, FW_BOLD );
				g_sub = Fonte( L"Inter", 16, FW_NORMAL );
				g_rotulo = Fonte( L"Inter", 12, FW_BOLD );
				g_texto = Fonte( L"Inter", 14, FW_NORMAL );
				g_botao = Fonte( L"Inter", 13, FW_BOLD );
				g_botaoGrande = Fonte( L"Cinzel", 22, FW_BOLD );
				g_mono = Fonte( L"Inter", 13, FW_NORMAL );
				g_brPainel = CreateSolidBrush( RGB( 3, 6, 12 ) );
				g_brFundo = CreateSolidBrush( FUNDO );
				g_entrada = Controle( L"EDIT", L"", ES_AUTOHSCROLL, Ret( 50, 185, 486, 20 ), ID_ENTRADA, g_texto );
				g_procurar = Controle( L"BUTTON", L"PROCURAR", BS_OWNERDRAW, Ret( 558, 176, 122, 36 ), ID_PROCURAR, g_botao );
				for( int i = 0; i < N_OPCOES; i++ ){
					int col = i < 6 ? 0 : 1, lin = i % 6;
					g_opcoes[i].h = Controle( L"BUTTON", g_opcoes[i].nome, BS_OWNERDRAW,
						Ret( 32 + col * 330, 452 + lin * 48, 318, 46 ), ID_OPCAO + i, g_botao );
				}
				g_aplicar = Controle( L"BUTTON", L"APLICAR INTERFACE", BS_OWNERDRAW, Ret( 20, 782, W - 40, 54 ), ID_APLICAR, g_botaoGrande );
				g_log = Controle( L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, Ret( 30, 876, W - 52, 86 ), ID_LOG, g_mono );
				g_abrir = Controle( L"BUTTON", L"ABRIR PASTA APLICADOS", BS_OWNERDRAW, Ret( 20, H - 50, 220, 34 ), ID_ABRIR, g_botao );
				SendMessageW( g_entrada, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM( 2, 2 ) );
				LogJanela( L"Escolha o executável hexed e clique em Aplicar interface.", 0 );
				DragAcceptFiles( h, TRUE );
				return 0;
			}
			case WM_PAINT: {
				PAINTSTRUCT ps;
				HDC dc = BeginPaint( h, &ps );
				// sem tremer: pinta numa imagem e copia
				HDC mem = CreateCompatibleDC( dc );
				HBITMAP bmp = CreateCompatibleBitmap( dc, W, H );
				HGDIOBJ old = SelectObject( mem, bmp );
				Pintar( mem );
				BitBlt( dc, 0, 0, W, H, mem, 0, 0, SRCCOPY );
				SelectObject( mem, old );
				DeleteObject( bmp );
				DeleteDC( mem );
				EndPaint( h, &ps );
				return 0;
			}
			case WM_ERASEBKGND:
				return 1;
			case WM_CTLCOLOREDIT:
			case WM_CTLCOLORSTATIC:
				SetTextColor( (HDC)wp, (HWND)lp == g_log ? APAGADO : TEXTO );
				SetBkColor( (HDC)wp, RGB( 3, 6, 12 ) );
				return (LRESULT)g_brPainel;
			case WM_DRAWITEM:
				DesenharBotao( reinterpret_cast<const DRAWITEMSTRUCT*>( lp ) );
				return TRUE;
			case WM_DROPFILES: {
				wchar_t arq[MAX_PATH];
				DragQueryFileW( (HDROP)wp, 0, arq, MAX_PATH );
				DragFinish( (HDROP)wp );
				SetWindowTextW( g_entrada, arq );
				return 0;
			}
			case WM_COMMAND:
				if( HIWORD( wp ) == EN_SETFOCUS || HIWORD( wp ) == EN_KILLFOCUS ){
					RECT c = Ret( 40, 176, 506, 36 );
					InvalidateRect( h, &c, FALSE );
				}
				switch( LOWORD( wp ) ){
					case ID_PROCURAR: {
						std::wstring a = EscolherArquivo( h, Texto( g_entrada ) );
						if( !a.empty() ) SetWindowTextW( g_entrada, a.c_str() );
						return 0;
					}
					case ID_APLICAR:
						Aplicar();
						return 0;
					default:
						if( LOWORD( wp ) >= ID_OPCAO && LOWORD( wp ) < ID_OPCAO + N_OPCOES ){
							int i = LOWORD( wp ) - ID_OPCAO;
							if( OpcaoAtiva( i ) ){
								g_opcoes[i].marcada = !g_opcoes[i].marcada;
								SalvarOpcoes();
								for( const Opcao& o : g_opcoes ) InvalidateRect( o.h, nullptr, FALSE );
							}
							return 0;
						}
						break;
					case ID_ABRIR:
						CreateDirectoryW( PastaAplicados().c_str(), nullptr );
						ShellExecuteW( h, L"open", ( g_ultimaPasta.empty() ? PastaAplicados() : g_ultimaPasta ).c_str(), nullptr, nullptr, SW_SHOWNORMAL );
						return 0;
				}
				break;
			case WM_DESTROY:
				PostQuitMessage( 0 );
				return 0;
		}
		return DefWindowProcW( h, m, wp, lp );
	}

	// Linha de comando: "Aplicador Codex.exe" <hexed.exe> [/semgrf]  ->  Aplicados\<data>\ (registro em aplicador.log)
	FILE* g_arqLog = nullptr;
	void LogArquivo( const std::wstring& s, int tipo ){
		const wchar_t* pre[] = { L"", L"AVISO: ", L"ERRO: ", L"" };
		if( g_arqLog ){ fwprintf( g_arqLog, L"%s%s\n", pre[tipo], s.c_str() ); fflush( g_arqLog ); }
	}

	void CarregarFontes(){
		const int ids[] = { IDR_FONTE_TITULO, IDR_FONTE_TEXTO };
		for( int id : ids ){
			const unsigned char* p;
			DWORD n, qtd = 0;
			if( Recurso( id, p, n ) ) AddFontMemResourceEx( (void*)p, n, nullptr, &qtd );
		}
	}

	Gdiplus::Image* CarregarLogo(){
		const unsigned char* p;
		DWORD n;
		if( !Recurso( IDR_LOGO, p, n ) ) return nullptr;
		IStream* s = SHCreateMemStream( p, n );
		if( !s ) return nullptr;
		Gdiplus::Image* img = Gdiplus::Image::FromStream( s );
		s->Release();
		return img;
	}
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE, LPWSTR, int show ){
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW( GetCommandLineW(), &argc );
	if( argc >= 2 ){
		bool grf = true;
		unsigned alt = embutir::TODAS;
		for( int i = 2; i < argc; i++ ){
			if( _wcsicmp( argv[i], L"/semgrf" ) == 0 ) grf = false;
			if( _wcsnicmp( argv[i], L"/alteracoes=", 12 ) == 0 ){
				// ex.: /alteracoes=login,selecao,criacao,chave,combate,fade,status,item,equip,inventario,habilidades
				std::wstring v = argv[i] + 12;
				alt = 0;
				const wchar_t* nomes[] = { L"login", L"selecao", L"criacao", L"chave", L"combate", L"fade", L"status", L"item", L"", L"equip", L"inventario", L"habilidades" };
				for( int k = 0; k < 12; k++ ) if( nomes[k][0] ) if( v.find( nomes[k] ) != std::wstring::npos ) alt |= 1u << k;
			}
		}
		_wfopen_s( &g_arqLog, ( PastaDoApp() + L"aplicador.log" ).c_str(), L"w, ccs=UTF-8" );
		std::wstring pasta;
		if( !( alt & embutir::SELECAO ) ) alt &= ~( embutir::CRIACAO | embutir::CHAVE | embutir::COMBATE );
		bool ok = Executar( argv[1], grf, alt, pasta, &LogArquivo );
		if( g_arqLog ) fclose( g_arqLog );
		return ok ? 0 : 1;
	}

	Gdiplus::GdiplusStartupInput gsi;
	ULONG_PTR token;
	Gdiplus::GdiplusStartup( &token, &gsi, nullptr );
	CarregarFontes();
	CarregarOpcoes();
	g_logo = CarregarLogo();
	INITCOMMONCONTROLSEX icc = { sizeof( icc ), ICC_STANDARD_CLASSES };
	InitCommonControlsEx( &icc );

	WNDCLASSEXW wc = { sizeof( wc ) };
	wc.lpfnWndProc = Proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor( nullptr, IDC_ARROW );
	wc.lpszClassName = L"AplicadorCodex";
	wc.hIcon = LoadIconW( inst, MAKEINTRESOURCEW( IDI_APP ) );
	wc.hIconSm = wc.hIcon;
	RegisterClassExW( &wc );
	DWORD estilo = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	RECT r = { 0, 0, W, H };
	AdjustWindowRect( &r, estilo, FALSE );
	HWND w = CreateWindowExW( 0, wc.lpszClassName, L"O Codex · Aplicador da interface", estilo,
		CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr );
	// barra de título escura (Windows 10/11)
	BOOL escuro = TRUE;
	HMODULE dwm = LoadLibraryW( L"dwmapi.dll" );
	if( dwm ){
		auto f = reinterpret_cast<HRESULT( WINAPI* )( HWND, DWORD, LPCVOID, DWORD )>( GetProcAddress( dwm, "DwmSetWindowAttribute" ) );
		if( f ){
			f( w, 20, &escuro, sizeof( escuro ) );
			COLORREF cap = FUNDO_TOPO;
			f( w, 35, &cap, sizeof( cap ) );
		}
	}
	ShowWindow( w, show );
	MSG m;
	while( GetMessageW( &m, nullptr, 0, 0 ) ){
		if( !IsDialogMessageW( w, &m ) ){
			TranslateMessage( &m );
			DispatchMessageW( &m );
		}
	}
	delete g_logo;
	Gdiplus::GdiplusShutdown( token );
	return 0;
}
