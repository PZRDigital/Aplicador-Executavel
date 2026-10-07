#pragma once

#include <string>

#include <cstdint>

// Integração com o Codex.exe (cliente 2025-06-04, sem ASLR, base 0x400000).
// Os endereços abaixo valem SOMENTE para este executável; Install() confere
// assinaturas antes de aplicar qualquer hook e desativa tudo se não baterem.
namespace client {
	// vtable de UINewSelectCharWnd (RTTI .?AVUINewSelectCharWnd@@)
	constexpr uintptr_t VT_SELECTCHARWND = 0xFFD194;
	constexpr int VT_IDX_DTOR = 0;      // scalar deleting destructor (ret 4)
	constexpr int VT_IDX_ONCREATE = 15; // OnCreate(int, int)        (ret 8)
	constexpr int VT_IDX_ONDRAW = 20;   // OnDraw()
	constexpr uintptr_t FN_DTOR = 0x798EE0;
	constexpr uintptr_t FN_ONCREATE = 0x799610;
	constexpr uintptr_t FN_ONDRAW = 0x79B690;

	// vtable de UINewMakeCharWnd (tela de criação de personagem)
	constexpr uintptr_t VT_MAKECHARWND = 0xFFDA34;
	constexpr uintptr_t FN_MAKE_DTOR = 0x79E570;
	constexpr uintptr_t FN_MAKE_ONCREATE = 0x79E6A0;

	// Slot absoluto selecionado (página * slots_por_página + índice).
	// É o byte enviado no CH_SELECT_CHAR (0x0066), ver 0xD0C2F3.
	constexpr uintptr_t G_SELECTED_SLOT = 0x15D0FC2;
	constexpr uintptr_t SIG_SELECT_PACKET = 0xD0C2F3; // mov al, byte ptr [0x15D0FC2]

	// Campos de UINewSelectCharWnd
	constexpr uintptr_t OFS_SLOT_WINDOWS = 0xC8;  // UISlotForSelectWnd** (um por slot da página)
	constexpr uintptr_t OFS_SLOTS_PER_PAGE = 0xFC;
	constexpr uintptr_t OFS_INDEX_IN_PAGE = 0x100;
	constexpr uintptr_t OFS_SLOT_COUNT = 0x108;
	// Vetor de estado dos bonecos (um por slot absoluto, 0x15C bytes): posição, ação e classe
	constexpr uintptr_t OFS_SLOT_STATES = 0xE8;
	constexpr size_t SLOT_STATE_SIZE = 0x15C;
	constexpr uintptr_t OFS_STATE_ACTION = 0x14C; // ação do sprite (0 = parado, 16 = sentado; o cliente zera a cada quadro)
	constexpr uintptr_t OFS_STATE_MOTION = 0x150; // quadro da ação
	// CActRes::GetMotion(action, motion) (thiscall): quadro de uma ação; confere o limite
	constexpr uintptr_t FN_GETMOTION = 0x70D9D0;
	constexpr uintptr_t OFS_ACT_ACTIONS = 0x110;  // std::vector<std::vector<CMotion (0x44)>>
	constexpr size_t MOTION_SIZE = 0x44;
	constexpr size_t SLOT_WND_SIZE = 0x13C;

	// Campos de UIWindow
	constexpr uintptr_t OFS_WND_WIDTH = 0x14;
	constexpr uintptr_t OFS_WND_HEIGHT = 0x18;
	constexpr uintptr_t OFS_WND_X = 0x1C;
	constexpr uintptr_t OFS_WND_Y = 0x20;

	struct WndRect {
		int x = 0, y = 0, w = 0, h = 0;
	};

	bool Install();
	bool IsSelectActive();
	bool IsMakeCharActive();  // tela de criação aberta
	bool IsCreatingSelect();  // janela de seleção sendo criada (o cliente já desenha quadros)

	// Janelas das telas de entrada (antes da seleção)
	enum class LoginWnd { Login = 0, SelectServer = 1, Wait = 2, Notice = 3, Confirm = 4, Disconnected = 5, MessageBox = 6, MessageBoxAuto = 7, COUNT };
	void* LoginWindow( LoginWnd which );   // instância aberta (nullptr = fechada)
	bool LoginWindowRect( LoginWnd which, WndRect& out );
	bool MessageText( void* messageBox, std::string& out ); // texto da UIMessageBox (cp1252)
	void* LoginWindowRaw( LoginWnd which );  // ponteiro mesmo se escondida (depuração)
	bool AnyLoginWindow();
	bool InGame();                          // já entrou no mapa (fim das telas de entrada)
	// Campos da janela de login: ID digitado, tamanho da senha e "Salvar ID"
	bool ReadLoginFields( std::string& id, int& passwordLength, bool& saveId );
	bool ReadLoginPassword( char* buf, size_t n ); // senha digitada (quem chama zera o buffer)
	void SetInGame( bool on );
	int SelectedSlot();
	void* SelectWindow();

	int SlotsOnPage();   // cartões na página atual
	int IndexInPage();   // cartão selecionado na página
	int SlotsPerPage();
	bool CardRect( int index, WndRect& out ); // relativo à janela de seleção
	bool MakeWindowRect( WndRect& out );      // janela de criação (tela)
	void* MakeWindow();

	// Escolhas atuais da janela de criação (UINewMakeCharWnd)
	constexpr uintptr_t OFS_MAKE_DIR = 0x1E0;       // direção da prévia (0..7)
	constexpr uintptr_t OFS_MAKE_HAIR = 0x246;      // penteado (word)
	constexpr uintptr_t OFS_MAKE_HAIRCOLOR = 0x258; // cor do cabelo (word)
	constexpr uintptr_t OFS_MAKE_SEX = 0x29E;       // 1 = masculino, 0 = feminino (byte)
	struct MakeState {
		int sex = 1, hair = 1, hairColor = 0, dir = 0;
	};
	bool ReadMakeState( MakeState& out );
	void DebugFindText( const char* text ); // engenharia reversa: onde está o texto do campo de nome
	void DebugFindTextIn( void* wnd, size_t size, const char* text, const char* label );
	// Nome digitado no campo nativo: [[janela+0x2A0]+0xD8] (char*)
	constexpr uintptr_t OFS_MAKE_NAMEEDIT = 0x2A0;
	constexpr uintptr_t OFS_EDIT_TEXT = 0xD8;
	bool ReadMakeName( std::string& out );

	// Desenha um personagem com a rotina da seleção (objeto 0x7AB690 / desenho 0x7ABCA0).
	// x, y = pés relativos à janela dona. Os sprites saem por DrawPrimitiveUP.
	constexpr uintptr_t FN_CHARVIEW_CTOR = 0x7AB690;
	constexpr uintptr_t FN_CHARVIEW_DRAW = 0x7ABCA0;
	constexpr uintptr_t FN_CHARVIEW_DTOR = 0x798BC0;
	bool DrawCharacter( void* owner, int x, int y, int sex, int job, int hair, int hairColor, int action, int motion );
	void LogGeometry();
	// Ação do boneco do slot selecionado (ex.: 32 = postura de combate); -1 = a do cliente
	// e, a cada attackEveryMs, um golpe (attackAction, ex.: 40; -1 = nunca)
	// Fades da troca de mapa (CModeMgr: escurecer 0xA5DBD0, clarear 0xA5D9B0): 255 ms cada, pelo
	// "agora - início" em ms (sub eax, [0x1574D8C]). Os três pontos passam a chamar uma rotina
	// que escala esse tempo para a duração pedida (0 = instantâneo).
	bool InstallFastFade( int durationMs );
	void SetSelectedAction( int action, int frameMs = 110, int attackAction = -1, int attackEveryMs = 6000 );
}
