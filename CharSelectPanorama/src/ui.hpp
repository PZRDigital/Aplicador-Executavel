#pragma once

#include <functional>

#include "game.hpp"
#include "gfx.hpp"

#include <windows.h>
#include <d3d9.h>
#include <string>

#include "input.hpp"

// Interface nova da seleção no estilo "alojamento" (Tree of Savior) com o tema O Codex:
// todos os personagens ficam no mapa com placas de nome, a câmera segue o selecionado,
// lista com miniaturas à direita e botões que acionam os controles nativos via input::.
namespace ui {
	struct NativeFrame {
		float vpWidth = 0, vpHeight = 0;
		float originX = 0, originY = 0; // canto da janela nativa na tela
		bool popupActive = false;       // outra janela do cliente aberta por cima
	};

	// Tela de criação: textura da janela nativa (blocos) para recortar os penteados
	struct MakeTile {
		IDirect3DTexture9* tex;
		float x, y, w, h;       // na tela
		float u0, v0, u1, v1;
	};
	struct MakeFrame {
		float vpWidth = 0, vpHeight = 0;
		float originX = 0, originY = 0; // canto da janela de criação
		bool popupActive = false;
		const MakeTile* tiles = nullptr;
		int tileCount = 0;
	};
	void DrawMake( IDirect3DDevice9* dev, const MakeFrame& frame );

	// Telas de entrada (login, escolha de servidor, aguarde)
	struct LoginFrame {
		float vpWidth = 0, vpHeight = 0;
		bool popupActive = false;      // mensagem nativa por cima: só desenha, não recebe cliques
		bool hasLogin = false, hasServers = false, hasWait = false;
		float loginX = 0, loginY = 0;  // canto das janelas nativas
		float serversX = 0, serversY = 0;
	};
	void DrawLogin( IDirect3DDevice9* dev, const LoginFrame& frame );

	// Mensagem do cliente (UIMessageBox) redesenhada no tema; cliques vão aos botões nativos
	struct MessageFrame {
		float vpWidth = 0, vpHeight = 0;
		float x = 0, y = 0, w = 0, h = 0; // janela nativa (escondida)
		std::wstring text;
		bool twoButtons = false;           // OK + cancelar
		int buttons = 0;                   // botões achados na janela nativa (centro de cada um, relativo a ela)
		float bx[3] = {}, by[3] = {};      // em ordem de x: OK primeiro, cancelar depois
		uintptr_t btn[3] = {};             // os próprios botões (UIButton)
		bool prompt = false;               // pedido de e-mail da exclusão (campo de texto do cliente)
		std::wstring input;                // o que foi digitado no campo
	};
	// botão da mensagem escolhido na nossa (acionado no quadro: d3d_hooks), 0 = nenhum
	uintptr_t TakeMessageCommand();
	void DrawMessage( IDirect3DDevice9* dev, const MessageFrame& frame );
	void ClearMessage();                   // sem mensagem neste quadro
	void SetServerCountdown( int ms );     // lista de servidores: tempo até entrar (-1 = sem contagem)

	// Chave "seleção personalizada / original" (barra de cima, ao lado do contador), nos dois modos
	void DrawModeToggle( IDirect3DDevice9* dev, float vpWidth, float vpHeight );

	// Fora da seleção: carrega logotipo, texturas e ícones de classe (a seleção abre sem travar)
	void Warmup( IDirect3DDevice9* dev );

	void Init();
	void Draw( IDirect3DDevice9* dev, const NativeFrame& frame );
	void OnDeviceLost();

	// Redirecionamento do mouse para os controles nativos (registrado em input::)
	bool Redirect( const input::Point& real, input::Point& target );
	bool RedirectInner( const input::Point& real, input::Point& target ); // os desvios em si (Redirect decide se valem)
	bool CloseClickActive(); // clique no X de uma janela nossa levado ao X nativo (cursor/tooltip de lá não aparecem)

	// Como redesenhar os sprites de um cartão nativo: x' = target + (x - anchor) * scale
	struct SpriteXform {
		float anchorX, anchorY;
		float targetX, targetY;
		float scale;
		float brightness; // multiplica a cor dos vértices (1 = normal)
		float alpha = 1;  // multiplica o alfa dos vértices
		float order = 0;  // profundidade (maior = mais perto; desenhado por último)
		RECT clip;        // recorte (scissor)
	};
	int CardAt( float x, float y );                  // cartão nativo nesse ponto (-1 = nenhum)
	void DrawPlates( IDirect3DDevice9* dev );        // placas de nome (depois dos personagens)
	void NoteSprite( int card, float minY );         // topo real do sprite (mede a altura de cada personagem)
	bool SceneXform( int card, SpriteXform& out );   // personagem no mapa
	bool ThumbXform( int card, SpriteXform& out );   // miniatura na lista

	// ---------------------------------------------------------------- dentro do jogo
	// Barra de status (no lugar da UIBasicInfoWnd; x, y = posição da janela nativa)
	// Cartão claro de HP/SP no canto superior esquerdo (HUD_W x HUD_H)
	constexpr float HUD_W = 272, HUD_H = 60, HUD_X = 12, HUD_Y = 10;
	void DrawStatusHud( IDirect3DDevice9* dev, float x, float y, const game::Status& status );
	float ExpBarHeight(); // altura da barra de EXP do Zero (0 antes de carregar)
	// Minimapa: moldura escura em volta do mapa (size x size) e nome + coordenadas embaixo (cx < 0 = sem coordenadas)
	void DrawMinimapFrame( IDirect3DDevice9* dev, float x, float y, float size, const std::wstring& map, int cx, int cy );
	// Barra de cast e caixa do nome da habilidade no estilo da referência (no lugar das nativas)
	void DrawCastBar( IDirect3DDevice9* dev, float centerX, float top, float nativeW, float progress );
	void DrawSkillNameBox( IDirect3DDevice9* dev, float x, float y, float w, float h );
	void MinimapIconButton( float x, float y, float size, float& bx, float& by, float& bw, float& bh ); // botão "NPC"
	// Fundo dos atalhos: quadrados cinza translúcidos arredondados
	void DrawShortcutSlots( IDirect3DDevice9* dev, float x, float y, float slot, float pitch, int cols, int rows, float rowPitch );
	// Mensagens novas do chat, centradas e acima de bottom (alpha de cada uma = fade)
	struct ChatFeedLine { std::wstring text; uint32_t color = 0xFFFFFFFF; float alpha = 1; };
	void DrawChatFeed( IDirect3DDevice9* dev, float centerX, float bottom, float maxWidth, const std::vector<ChatFeedLine>& lines );
	bool StatusHudRedirect( const input::Point& real, input::Point& target, float nativeX, float nativeY );
	// Barra de EXP do Ragnarok Zero, colada embaixo da tela
	void DrawExpBar( IDirect3DDevice9* dev, float screenW, float screenH, const game::Status& status );
	// Menu de ícones do Ragnarok Zero (no lugar da UIMenuIconWnd), no canto inferior direito.
	// ZeroMenuOrigin = canto superior esquerdo (a janela nativa vai para lá, escondida embaixo)
	void ZeroMenuOrigin( bool open, float screenW, float screenH, float& x, float& y );
	void DrawZeroMenu( IDirect3DDevice9* dev, const game::MenuState& menu, float screenW, float screenH );
	bool ZeroMenuRedirect( const input::Point& real, input::Point& target );
	// Tamanho do menu do Zero e onde fica cada controle nativo (relativo à janela) para a substituição
	void ZeroMenuLayout( const game::MenuState& menu, int& width, int& height, std::vector<game::Placement>& place );
	// Janela de quests do Zero (no lugar da CUIRenewQuestUI; x, y = janela nativa)
	void DrawZeroQuestWindow( IDirect3DDevice9* dev, float nativeX, float nativeY, float nativeW, float nativeH );
	bool ZeroQuestRedirect( const input::Point& real, input::Point& target );
	// Pokédex no lugar da janela de pet (UIPetInfoWnd; x, y = janela nativa)
	void DrawPokedex( IDirect3DDevice9* dev, float nativeX, float nativeY, float nativeW, float nativeH );
	bool PokedexRedirect( const input::Point& real, input::Point& target );
	// Barra de HP (e SP) com as imagens no lugar de uma barra nativa (x, y, w, h = janela dela).
	// down = quanto desce, em alturas de barra
	void DrawGageBars( IDirect3DDevice9* dev, float x, float y, float w, float h, int hp, int maxHp, int sp, int maxSp, bool withSp, float down );

	// Descrição do item (no lugar da UIItemCollectionWnd) e, à esquerda, o equipado no mesmo lugar.
	// image(itemId, x, y, w, h): desenha a ilustração do item (false = sem imagem guardada)
	struct ItemPanels {
		float x = 0, y = 0, w = 0, h = 0;   // janela nativa
		float vpWidth = 0, vpHeight = 0;
		game::Item item;
		bool hasEquipped = false;
		game::Item equipped;
		bool hasEquippedPos = false;        // posição da comparação nativa (senão: à esquerda)
		float ex = 0, ey = 0;
		std::function<bool( uint32_t, float, float, float, float )> image;
	};
	void DrawItemPanels( IDirect3DDevice9* dev, const ItemPanels& panels );
	bool ItemWindowRedirect( const input::Point& real, input::Point& target ); // cliques na janela do item
	// Descrição do item substituída: tamanho e posições (relativas à janela) que a janela do cliente recebe
	// descrição do item nativa: runas (opções aleatórias) redesenhadas nas caixas do cliente, com etiqueta e cor
	void DrawItemRunes( IDirect3DDevice9* dev, float x, float y, const game::Item& item, const game::ItemNative& native );

	// Janela de equipamentos (no lugar da UIEquipWnd)
	struct EquipFrame {
		float nx = 0, ny = 0, nw = 0, nh = 0;   // janela nativa
		float vpWidth = 0, vpHeight = 0;
		bool hasStats = false;
		game::EquipStats stats;
		bool showEquip = false;
		gfx::Image character;                  // personagem da janela nativa (sem fundo)
		// copia um pedaço da janela nativa (aba Visual/Título): origem relativa à nativa -> destino na tela
		std::function<void( float, float, float, float, float, float, float, float )> blitNative;
	};
	void DrawEquipWindow( IDirect3DDevice9* dev, const EquipFrame& frame );
	bool EquipWindowRedirect( const input::Point& real, input::Point& target );
	// Equipamentos substituídos (geometria do cliente); character = personagem copiado da janela do cliente,
	// blit = copia um pedaço da janela do cliente (prévias da aba Dano)
	void DrawNativeEquip( IDirect3DDevice9* dev, float x, float y, float w, float h, const gfx::Image& character,
		const std::function<void( float, float, float, float, float, float, float, float )>& blit );
	// Janela Personagem (equipamentos + atributos) no lugar da de equipamentos; a nativa fica no canto superior
	// esquerdo dela. character = personagem copiado da nativa (sem fundo)
	constexpr float CHAR_K = 0.86f;                                   // escala da referência (484x597)
	constexpr float CHAR_W = 416, CHAR_H = 514;
	constexpr float CHAR_OTHER_H = 284; // equipamento de outro jogador: só os encaixes
	void DrawCharacterWindow( IDirect3DDevice9* dev, float nx, float ny, float nw, float nh, const gfx::Image& character, int which ); // which: 0 = a própria, 1 = de outro jogador
	bool CharacterRedirect( const input::Point& real, input::Point& target );
	// Janela Personagem desenhada pelo cliente: o layout (chamado no OnDraw da UIEquipWnd) e os cliques nas ações
	// nossas (uma vez por quadro)
	void PaintCharacterWindow( uintptr_t window );
	void CharacterInput();
	// geometria usada pelo próprio cliente (game::InstallEquipLayout / SubstituteEquipWindow)
	int CharacterSlotAt( int x, int y, bool wide, bool costume, bool other );
	void CharacterTipPos( int mx, int my, int& x, int& y, bool other );
	void CharacterChildRects( client::WndRect& close, client::WndRect& check, client::WndRect& cart, int tab = 0 );
	// abas Título (2) e Dano (3): o conteúdo é do cliente, na geometria original dele
	constexpr int CHAR_TAB_W = 280, CHAR_TAB_H = 196;
	void CharacterWindowClosed( int which );
	bool CharacterRect( float& x, float& y, float& w, float& h ); // aberta (desenhada há pouco)
	// Onde o personagem da janela nativa deve aparecer (âncora = pés): false = esconder
	bool EquipCharacterTarget( float& x, float& y, float& scale );
	// Ícone (24x24) e ilustração do item, carregados do próprio cliente
	bool ItemIcon( IDirect3DDevice9* dev, uint32_t itemId, gfx::Image& out );
	bool ItemIllustration( IDirect3DDevice9* dev, uint32_t itemId, gfx::Image& out );

	// Inventário (no lugar da UIItemWnd). Cliques nos itens vão para a célula nativa do mesmo item.
	struct InventoryFrame {
		float nx = 0, ny = 0, nw = 0, nh = 0;
		float vpWidth = 0, vpHeight = 0;
	};
	void DrawInventoryWindow( IDirect3DDevice9* dev, const InventoryFrame& frame );
	void DrawNativeInventory( IDirect3DDevice9* dev, float x, float y, float w, float h ); // inventário substituído (geometria do cliente)
	bool InventoryRedirect( const input::Point& real, input::Point& target );

	// Habilidades (no lugar da UINewSkillListWnd). "Configurar Atalhos" mostra a lista nativa por cima.
	struct SkillFrame {
		float nx = 0, ny = 0, nw = 0, nh = 0;
		float vpWidth = 0, vpHeight = 0;
	};
	void DrawSkillWindow( IDirect3DDevice9* dev, const SkillFrame& frame );
	// habilidades substituídas (geometria do cliente); blit copia um pedaço da janela do cliente (nível escolhido)
	// Inventário (Alt+E) refeito no próprio cliente (charwnd::InstallInventory): medidas relativas à janela
	constexpr int INV_W = 326, INV_H = 352, INV_M = 16, INV_COLS = 8, INV_ROWS = 7, INV_CELL = 34, INV_PITCH = 36;
	constexpr int INV_GRID_Y = 74, INV_TAB_Y = 40, INV_TAB_W = 69, INV_TAB_H = 24, INV_TAB_GAP = 6;
	void PaintInventoryWindow( uintptr_t window );
	void InventoryInput();
	// Armazém (@storage) do mesmo jeito: lista de 8 linhas, 8 abas em duas fileiras, filtros e busca embaixo
	constexpr int ST_W = 340, ST_H = 456, ST_M = 16, ST_ROWS = 8, ST_TOP = 98, ST_RH = 36, ST_ROW_W = 300, ST_ROW_H = 32;
	constexpr int ST_TAB_Y = 40, ST_TAB_W = 72, ST_TAB_H = 22, ST_TAB_GAP = 6, ST_FILTER_Y = ST_TOP + ST_ROWS * ST_RH + 4;
	void PaintStorageWindow( uintptr_t window );
	// Carrinho (Alt+W) como o inventário (grade 8 x 7, sem abas) e o resultado da pesquisa do armazém como a lista
	constexpr int CART_W = 326, CART_GRID_Y = 46, CART_H = CART_GRID_Y + 7 * 36 + 30;
	constexpr int SR_W = 332, SR_M = 16, SR_ROWS = 6, SR_TOP = 46, SR_RH = 36, SR_ROW_W = 300, SR_ROW_H = 32, SR_H = SR_TOP + SR_ROWS * SR_RH + 10;
	void PaintCartWindow( uintptr_t window );
	void PaintSearchWindow( uintptr_t window );
	// Descrição do item / comparação: depois do desenho do cliente (charwnd::InstallItemWindow), moldura da imagem
	// e as runas (opções aleatórias) coloridas, na própria janela
	void PaintItemExtras( uintptr_t window );
	void PaintMessageBox( uintptr_t window ); // janela de aviso do cliente (charwnd::InstallMessageBox)
	// Telas de entrada no estilo anterior ao login.mp4, desenhadas nas próprias janelas do cliente (charwnd::InstallLogin,
	// InstallWait, InstallServer): a UILoginWnd vira o cartão da marca (esquerda) + o painel ENTRAR, com fundo
	// transparente entre os dois; os campos e botões são os dela (game::SubstituteLoginWindow)
	constexpr int LGB_W = 280, LGB_H = 218, LGP_W = 380, LGP_H = 400;
	constexpr int LG_PAD = 28, LG_BOX_H = 40, LG_FIELD_H = 16, LG_ENTER_H = 54;
	struct LoginGeo { int x, y, w, h, brandX, brandY, panelX, panelY, idY, pwY, saveY, enterY, linksY; }; // x/y na tela, o resto na janela
	LoginGeo LoginGeometry( int screenW, int screenH );
	void PaintLoginWindow( uintptr_t window );
	constexpr int WT_W = 300, WT_H = 110; // "Conectando" (UIWaitWnd) centrado
	void PaintWaitWindow( uintptr_t window );
	// Escolha do servidor (UISelectServerWnd): cartão com as linhas dos servidores; o OK do cliente vira ENTRAR e o
	// cancelar VOLTAR
	constexpr int SV_W = 460, SV_ROW_H = 58, SV_ROW_Y = 76, SV_BTN_W = 200, SV_BTN_H = 46;
	int ServerRows();
	int ServerButtonsY();
	int ServerHeight();
	// servidor e "Conectando" também levam o cartão da marca à esquerda (janela do cartão até o painel, fundo
	// transparente entre eles); cardX/cardY = painel dentro da janela
	struct EntryGeo { int x, y, w, h, brandX, brandY, cardX, cardY; };
	EntryGeo ServerGeometry( int screenW, int screenH );
	EntryGeo WaitGeometry( int screenW, int screenH );
	void PaintServerWindow( uintptr_t window );
	// Seleção de personagem hardcoded: a UINewSelectCharWnd fica em tela cheia e o OnDraw dela desenha a barra, a
	// ficha e a moldura da lista; cada UISlotForSelectWnd vira uma linha da lista (PaintSelectRow); os botões nativos
	// ganham as imagens do tema (game::SubstituteSelectWindow)
	constexpr int SEL_BAR_H = 34, SEL_M = 16, SEL_LIST_W = 360, SEL_PLAY_H = 62, SEL_ROW_W = 336, SEL_ROW_H = 64, SEL_ROW_GAP = 4;
	constexpr int SEL_INFO_W = 300, SEL_THUMB = 52;
	struct SelectGeo { int W, H, top, listX, listH, rowX, areaTop, areaBottom, playY, offL, offR; }; // off: entrada pelas bordas
	void SelectEntered(); // janela de seleção nova: os painéis entram
	SelectGeo SelectLayout( int W, int H );
	bool SelectRowPos( int card, int W, int H, int& x, int& y ); // false = fora da área visível da lista
	void SelectInput( int W, int H );                            // rolagem da lista e estrela de favorito
	void PaintSelectWindow( uintptr_t window );
	void PaintSelectRow( uintptr_t row, int card );
	// Criação de personagem hardcoded: UINewMakeCharWnd em tela cheia; painéis no OnDraw e controles nativos no lugar
	constexpr int MK_LEFT_W = 300, MK_LEFT_H = 560, MK_RIGHT_W = 360, MK_CELL = 48, MK_CELL_GAP = 8, MK_SWATCH = 22;
	struct MakeGeo { int W, H, rx, ry, rh, px, pw, genderY, hairLabelY, hairY, colorLabelY, colorY, nameLabelY, nameY; };
	MakeGeo MakeLayout( int W, int H );
	void PaintMakeWindow( uintptr_t window );
	// Conversa com NPC (hardcoded): a fala (UISayDialogWnd) vira uma faixa larga embaixo com plaqueta de nome e
	// retrato (ilustração da tabela charselect\retratos.txt, sem depender de cutin do script); o menu
	// (UIChoose3Wnd/UIChooseWnd) vira uma lista do tema à direita, acima da faixa (game::SubstituteNpcWindows)
	constexpr int SAY_W = 840, SAY_H = 600, SAY_BAR_Y = 430, SAY_BAR_H = 170, SAY_TEXT_X = 272, SAY_TEXT_W = 540, SAY_TEXT_H = 96;
	constexpr int CHOICE_W = 330, CHOICE_ROW_H = 32, CHOICE_ROWS = 9; // menu de altura fixa: 9 opções (mais = rolagem)
	std::string NpcPortrait( const std::wstring& name ); // caminho da ilustração ("" = sem retrato)
	void PaintSayWindow( uintptr_t window );
	void PaintChooseWindow( uintptr_t window );
	void PaintChoiceList( uintptr_t list );
	int ChoiceHover(); // linha da lista sob o mouse (-1 = nenhuma)
	// Lojas de NPC (UIItemShopWnd/2, UIItemPurchaseWnd, UIItemSellWnd/2): desenho do tema na própria janela, com a
	// mesma geometria de linhas do cliente (32 px a partir de y = 17), então clique, arraste e dica seguem nativos
	enum ShopKind { SHOP_LIST = 0, SHOP_LIST2, SHOP_CART, SHOP_SELL, SHOP_SELL2 };
	void PaintShopWindow( uintptr_t window, int kind );
	void PaintChooseSellBuy( uintptr_t window );
	void PaintLegacyFrame( uintptr_t window );
	void PaintPlainFrame( uintptr_t window );
	void PaintTitleStrip( uintptr_t window, const char* title ); // só a faixa do título (cp1252) no tema
	std::string ItemSlotImage(); // casa do item do tema (30x30)  // só o painel (janelas sem barra de título) // janelas antigas do cliente (mercador): fundo do tema no lugar da barra de título
	bool ItemRunesNative(); // runas desenhadas pela janela (o desenho por cima fica desligado)
	void StorageInput();
	// Janela de habilidades (Alt+S) refeita no próprio cliente (charwnd::InstallSkills): medidas relativas à janela
	constexpr int SK_W = 420, SK_H = 452, SK_M = 20, SK_TOP = 82, SK_RH = 36, SK_ROW_W = 372, SK_ROW_H = 32, SK_ROWS = 9;
	constexpr int SK_TAB_Y = 44, SK_TAB_H = 26, SK_TAB_GAP = 6;
	int SkillTabWidth( int tabCount ); // 112, 86 ou 70 (as imagens de tools/personagem_bmp.py)
	void PaintSkillWindow( uintptr_t window ); // no OnDraw da UINewSkillListWnd
	void SkillInput();                         // cliques nas abas (uma vez por quadro)
	// Atributos (Alt+A) substituídos: tamanho, onde ficam os "+" (relativo à janela) e quais o cliente mostra
	constexpr float STATUS_W = 320, STATUS_H = 214;
	void StatusUpButtons( client::WndRect up[6] );
	void SetStatusUpVisible( const bool visible[6] );
	void DrawNativeStatus( IDirect3DDevice9* dev, float x, float y, float w, float h );
	void DrawNativeSkills( IDirect3DDevice9* dev, float x, float y, float w, float h,
		const std::function<void( float, float, float, float, float, float, float, float )>& blit );
	bool SkillRedirect( const input::Point& real, input::Point& target );
	bool SkillNativeVisible(); // lista nativa à mostra (arrastar para os atalhos)
}
