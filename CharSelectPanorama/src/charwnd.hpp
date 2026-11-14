#pragma once

#include <cstdint>
#include <string>

// Janela Personagem desenhada pelo próprio cliente: o OnDraw da UIEquipWnd (0x8AF8A0) passa a desenhar o layout
// novo (ui::PaintCharacterWindow) na superfície da janela com as rotinas de desenho do cliente (imagem,
// retângulo, texto); depois segue o código original só para o personagem. Os controles nativos que ficam
// (fechar, "Exibir Equip.", carrinho) se desenham sozinhos por cima.
namespace charwnd {
	bool Install();
	bool Active(); // instalada (a DLL não desenha nada por cima dessas janelas)
	// Janela de habilidades (UINewSkillListWnd) refeita do mesmo jeito: OnDraw, teste de clique da lista e posição
	// dos botões trocados (ui::PaintSkillWindow, ui::SK_*)
	bool InstallSkills();
	bool SkillsActive();
	// Inventário (UIItemWnd) do mesmo jeito: OnDraw, testes de clique da grade e posição da dica (ui::INV_*)
	bool InstallInventory();
	bool InventoryActive();
	// Armazém (vt 0x101C9C0) do mesmo jeito (ui::ST_*); depende do InstallInventory (trampolins)
	bool InstallStorage();
	bool StorageActive();
	// Carrinho (vt 0x101D4B8) e resultado da pesquisa do armazém (vt 0x101C8E8), do mesmo jeito (ui::CART_*, ui::SR_*)
	bool InstallCartAndSearch();
	bool CartActive();
	bool SearchActive();
	// Descrição do item / comparação: OnDraw do cliente + cabeçalho e runas (ui::PaintItemExtras)
	bool InstallItemWindow();
	// Janelas de aviso do cliente (UIMessageBox e a que fecha sozinha): moldura e título do tema (ui::PaintMessageBox)
	bool InstallMessageBox();
	bool MessageBoxActive();
	// Janela de login (UILoginWnd): painel lateral do tema (ui::PaintLoginWindow, game::SubstituteLoginWindow)
	bool InstallLogin();
	bool LoginActive();
	bool InstallWait();   // "Conectando" (UIWaitWnd) no painel lateral (ui::PaintWaitWindow)
	bool WaitActive();
	bool InstallServer(); // escolha do servidor: cartão com medalhão (ui::PaintServerWindow)
	bool ServerActive();
	// Seleção de personagem (UINewSelectCharWnd em tela cheia + linhas UISlotForSelectWnd): ui::PaintSelectWindow/Row
	bool InstallSelect();
	bool SelectActive(); // instalada e no modo personalizado (senão o cliente desenha a original)
	bool InstallMake();  // criação de personagem (UINewMakeCharWnd em tela cheia): ui::PaintMakeWindow
	bool MakeActive();
	// Conversa com NPC: fala (UISayDialogWnd) criada já com a largura do texto da faixa e desenhada no tema; menu
	// (UIChoose3Wnd / UIChooseWnd) e a lista dele (vt 0x100C2A8, só quando a mãe é um menu) idem
	bool InstallShop();
	bool InstallMerchant(); // janelas de mercador (Comércio): desenho do cliente com as rotinas de moldura/cor no tema // lojas de NPC (ui::PaintShopWindow) e a escolha comprar/vender
	bool InstallNpc();
	bool NpcActive();
	bool ItemWindowActive();
	// item do cliente (ITEM_INFO): ícone com quantidade (0x870030) e nome (0x894AD0)
	void ItemIcon( uintptr_t w, int x, int y, uintptr_t info );
	void ItemName( uintptr_t w, int x, int y, int width, uintptr_t info, int height );

	// Rotinas de desenho do cliente na superfície da janela w (cores COLORREF, 0x00BBGGRR)
	void Fill( uintptr_t w, int x, int y, int cw, int ch, uint32_t color );
	enum Align { Left = 0, Center = 1, Right = 2 };
	void Text( uintptr_t w, int x, int y, const std::wstring& text, int height, uint32_t color, bool bold = false, int align = Left, int maxWidth = 0 );
	int TextWidth( uintptr_t w, const std::wstring& text, int height, bool bold = false );
	bool Bitmap( uintptr_t w, int x, int y, const std::string& path ); // caminho relativo (cp949: 유저인터페이스\...)
	bool Bitmap( uintptr_t w, int x, int y, uintptr_t res );          // CBitmapRes do cliente
}
