#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>

#include "client.hpp"

// Dentro do jogo: dados do personagem (sessão do cliente), equipamentos (pacotes do map-server)
// e as janelas nativas que a interface O Codex substitui.
namespace game {
	// Antes do cliente iniciar: copia a tabela de tamanhos de pacote que ele registra
	// (0xA87960(id, tamanho, mínimo, flag); tamanho -1 = variável).
	void InstallEarly();

	// Fluxo recebido de cada conexão, lido pacote a pacote com a tabela do cliente.
	void OnRecv( uintptr_t socket, const char* data, int len );
	void OnClose( uintptr_t socket );

	// Atributos do personagem (globais da sessão, 0x15D3120...)
	struct Status {
		int hp = 0, maxHp = 0, sp = 0, maxSp = 0;
		int baseLevel = 0, jobLevel = 0, job = 0;
		uint64_t baseExp = 0, baseNext = 0, jobExp = 0, jobNext = 0;
		int weight = 0, maxWeight = 0;
		int zeny = 0;
		std::wstring name;
	};
	bool ReadStatus( Status& out );

	struct ItemOption {
		int16_t id = 0, value = 0;
		uint8_t param = 0;
	};
	// Item de equipamento (lista do inventário ou janela de descrição)
	struct Item {
		int index = -1;
		uint32_t id = 0;
		uint32_t location = 0;   // onde pode ser equipado
		uint32_t wear = 0;       // onde está equipado (0 = no inventário)
		int refine = 0, grade = 0;
		uint32_t cards[4] = {};
		ItemOption options[5];
		bool identified = true;
		bool equip = false;      // equipamento (lista de equipamentos)
		bool favorite = false;
		int type = 0;            // tipo do item (0 cura, 2 usável, 3 etc, 4 arma, 5 armadura, 6 carta, 7 ovo, 8 acessório de mascote, 10 munição, 11/18 usáveis)
		int count = 1;
	};
	// Primeiro equipamento vestido em algum dos locais (máscara EQP_*)
	bool EquippedAt( uint32_t locationMask, Item& out );
	bool InventoryItem( int index, Item& out );
	// Inventário nativo (UIItemWnd) como o cliente o mostra: grade de 32 px a partir de (40,17) da janela,
	// colunas/linhas visíveis, rolagem (linha do topo), aba e o índice do item em cada célula visível
	// (mesma ordem e mesmo filtro do teste de clique 0x934F10)
	struct NativeInventory {
		int cols = 0, rows = 0, scroll = 0, tab = 0, total = 0;
		std::vector<int> cells;
		client::WndRect tabs;   // controle das abas (relativo à janela)
		bool tabsOk = false;
	};
	bool ReadNativeInventory( NativeInventory& out );
	void ResizeNativeInventory( int width, int height ); // pelo próprio cliente (mensagem 0xE, componente 7)
	// Equipamentos (UIEquipWnd) como o cliente os mostra: aba (+0xD0), item de cada encaixe (função do
	// cliente 0x89B140; 0-9 nas colunas, 15 no centro), controles filhos com posição e estado
	struct NativeEquip {
		int tab = 0;               // 0 Equip, 1 Especial, 2 Título, 3 Dano
		bool other = false;        // equipamento de outro jogador
		Item slot[16];
		bool has[16] = {};
		struct Check { client::WndRect r; bool checked = false; std::wstring label; };
		std::vector<Check> checks;          // caixas de marcar visíveis (aba Dano)
		bool showOk = false; client::WndRect show; bool showChecked = false; // "Exibir equipamento/visuais"
		bool applyOn = false, closeOn = false; client::WndRect apply, close;  // ok / fechar
		bool sysCloseOn = false, tabsOn = false; client::WndRect sysClose, tabs; // X da barra de título e as abas
		bool cartOn = false; client::WndRect cart;                              // botão do carrinho
		bool listOn = false; client::WndRect list; int listTop = 0, listSel = -1, listRowH = 18, listCount = 0;
		struct Row { int index = 0, id = 0; bool owned = false; std::wstring text; };
		std::vector<Row> rows;              // linhas visíveis da lista de títulos
		int damage = 0;                     // opção da aba Dano (0-3)
	};
	bool ReadNativeEquip( NativeEquip& out );
	// Habilidades (UINewSkillListWnd) como o cliente as mostra: aba (+0x238), lista da aba (0x9716B0) em ordem,
	// rolagem (+0x23C), modo lista (global) e os controles visíveis (subir, setas de nível, ok/cancelar)
	struct NativeSkills {
		int tab = 0, scroll = 0, tabCount = 3;
		bool listMode = true;
		struct Entry { int id = 0, inf = 0, level = 0, sp = 0, useLevel = 0, maxLevel = 0; };
		std::vector<Entry> entries;
		client::WndRect tabs; bool tabsOk = false;
		std::vector<client::WndRect> ups, arrowsLeft, arrowsRight;
		bool applyOn = false, resetOn = false, descOn = false, descChecked = false;
		client::WndRect apply, reset, desc;
	};
	bool ReadNativeSkills( NativeSkills& out );
	// Janela de habilidades refeita: linhas visíveis da lista (na ordem), como o próprio OnDraw as lê
	struct SkillRow {
		int id = 0, inf = 0, level = 0, maxLevel = 0, useLevel = 0, sp = 0;
		std::string name, icon; // nome (codepage do cliente) e caminho do ícone (유저인터페이스\item\...)
		bool arrows = false, up = false;
	};
	struct SkillView {
		int tab = 0, tabCount = 0, scroll = 0, total = 0, hover = -1, points = 0;
		std::vector<int> tabs;      // abas com habilidades (as vazias não aparecem)
		std::vector<int> tabFirst;  // primeira habilidade de cada uma (para o nome da aba)
		std::vector<SkillRow> rows;
	};
	bool ReadSkillView( uintptr_t window, SkillView& out );
	void SkillSetTab( int tab );              // a mensagem do controle de abas (0x16)
	void SubstituteSkillWindow();             // tamanho fixo, modo lista, controles do tema
	void FixSkillButtons( uintptr_t window ); // no fim do posicionamento do cliente (0x973FF0)
	// Inventário refeito: tamanho e grade (8 x 7) fixos, controles do tema, aba
	void SubstituteInventoryWindow();
	void InventorySetTab( int tab );
	void SubstituteItemWindows(); // descrição do item / comparação: fundo da caixa de texto no tema
	// Armazém (vt 0x101C9C0) refeito: lista de 8 linhas, abas (+0x160), filtros e busca nos lugares do tema
	struct StorageView {
		int tab = 0, scroll = 0, total = 0;
		std::wstring count;               // "usados/capacidade" que o cliente mantém (+0x124)
		std::vector<uintptr_t> items;     // ITEM_INFO (nó + 8) das linhas visíveis
	};
	bool ReadStorageView( uintptr_t w, StorageView& out );
	bool StorageWindowRect( client::WndRect& out );
	void SubstituteStorageWindow();
	void StorageSetTab( int tab );
	// lista de itens de uma janela da família do inventário (+0xC8; rolagem +0xB8 em linhas de +0xC0 colunas):
	// ITEM_INFO (nó + 8) dos visíveis, mesma ordem/filtro dos testes de clique do cliente
	void ReadItemList( uintptr_t w, int maxItems, std::vector<uintptr_t>& out, int& scroll, int& total );
	struct CartInfo { int count = 0, max = 0, weight = 0, maxWeight = 0; };
	CartInfo ReadCartInfo();
	void SubstituteCartWindow();
	void SubstituteSearchWindow();
	bool ListWindowTitle( uintptr_t w, std::string& out ); // texto do título do cliente (ex.: nome do filtro)
	// Atributos (Alt+A, UIStatusWnd 0x1012AC4): do tamanho da nossa; as setas de subir dos 6 atributos (as que o
	// cliente mostra) vão para up[k] (relativo à janela), minimizar e fechar para o canto; o resto sai
	bool StatusWindowRect( client::WndRect& out );
	// esconde a dica (tooltip) do cliente que estiver à mostra: em cima da nossa janela de atributos ela é da
	// geometria antiga da nativa embaixo; a nossa é desenhada no lugar
	void HideTooltip();
	bool TooltipRect( client::WndRect& out ); // dica nativa à mostra
	void SubstituteStatusWindow( int width, int height, const client::WndRect up[6], bool visible[6] );
	// descrição do item nativa: tamanho (as caixas das opções, 23 px a cada 29 de x 3 a 277, ficam acima da
	// faixa de cartas dos últimos 26 px), para as runas serem redesenhadas por cima com etiqueta e cor
	struct ItemNative { int w = 0, h = 0; };
	bool ReadItemNative( bool compare, ItemNative& out );

	// Janelas nativas (UIWindowMgr 0x12F8268): retângulo na tela; false se fechada
	bool BasicInfoRect( client::WndRect& out );
	bool ItemWindowRect( client::WndRect& out );
	bool ReadItemWindow( Item& out );
	// janela de descrição/comparação w (do OnDraw): item, opções e o título do cliente (cp1252)
	bool ReadItemWindowAt( uintptr_t w, Item& out, std::string& title );
	// item mostrado na janela de descrição (ITEM_INFO em +0x94)
	// Comparação nativa ("Equipped", UIItemCollection_ComparisonWnd): o equipado no mesmo lugar
	bool CompareWindowRect( client::WndRect& out );
	bool ReadCompareWindow( Item& out );

	// Atributos e informações (janela de equipamentos)
	struct EquipStats {
		int base[6] = {}, bonus[6] = {}, cost[6] = {};
		int points = 0;
		int atk = 0, atk2 = 0, matk = 0, matk2 = 0, def = 0, def2 = 0, mdef = 0, mdef2 = 0;
		int hit = 0, flee = 0, flee2 = 0, crit = 0, aspd = 0;
	};
	bool ReadEquipStats( EquipStats& out );
	bool ShowEquipFlag();
	bool EquipWindowRect( client::WndRect& out );

	// Menu de ícones (UIMenuIconWnd, janela 0x133): estado aberto, botão de abrir/fechar e os ícones
	// (nome = o da imagem, "equip" de menu_icon\bt_equip.bmp), em coordenadas de tela.
	struct MenuButton {
		std::string name;
		client::WndRect rect;
		uintptr_t ptr = 0;     // o ícone nativo (substituição: vai para baixo do ícone do Zero)
	};
	struct MenuState {
		bool open = false;
		client::WndRect rect, toggle;
		std::vector<MenuButton> icons;
		uintptr_t toggles[3] = {}; // botões nativos de abrir, abrir (novidade) e fechar
	};
	std::string CurrentMap(); // mapa atual ("prontera"); vazio fora do jogo

	// Quests (pacotes 0x09F8 lista, 0x09F9/0x08FE nova, 0x02B4 removida, 0x09FA caça, 0x02B7 estado)
	struct QuestObjective {
		uint32_t hunt = 0;       // quest * 1000 + índice
		int mob = 0;
		std::string name;        // nome do monstro/objetivo (codepage do cliente)
		int count = 0, total = 0;
	};
	struct Quest {
		int id = 0;
		int state = 0;           // 0 em espera, 1 em andamento, 2 concluída
		uint32_t end = 0;        // prazo (unix), 0 = sem prazo
		std::vector<QuestObjective> objectives;
	};
	std::vector<Quest> Quests();
	void SetQuestActive( int id, bool active ); // CZ_ACTIVE_QUEST
	// Pet chocado (ZC_PROPERTY_PET 0x01A2 e ZC_CHANGESTATE_PET 0x01A4)
	struct Pet {
		bool valid = false;
		std::string name;        // codepage do cliente
		bool renamed = false;    // já trocou o nome (não pode de novo)
		int level = 0, hunger = 0, intimacy = 0, accessory = 0, job = 0;
	};
	Pet CurrentPet();
	void PetCommand( int command );            // CZ_COMMAND_PET: 1 alimentar, 2 truque, 3 voltar ao ovo, 4 tirar acessório
	void RenamePet( const std::string& name ); // CZ_RENAME_PET (codepage do cliente)
	// Atributos do pet na Arena de Pets (servidor: F_PetArena_ShowPetStatus, modo Pokédex). A Pokédex pede com
	// RequestPetArena (manda "@pokedex" pelo chat, sem aparecer) e o servidor responde com pacotes de som
	// "#pdx:<chave>=<valores>" com ação "parar" (o cliente não toca nada)
	struct PetArena {
		bool valid = false;      // resposta completa recebida
		bool accepted = false;   // pet aceito na arena (senão só valid + job)
		int job = 0;
		int level = 0, wins = 0, tierWins = 0, rebirths = 0, mmr = 0, rank = 0;
		int str = 0, agi = 0, vit = 0, int_ = 0, dex = 0, luk = 0;
		int hp = 0, aspd = 0, atkMin = 0, atkMax = 0, matkMin = 0, matkMax = 0, hit = 0, flee = 0, def = 0, mdef = 0;
		uint32_t tick = 0;       // GetTickCount da resposta
	};
	PetArena CurrentPetArena();
	void RequestPetArena();
	// Janela de pet nativa (UIPetInfoWnd, Alt+J) e o botão de fechar dela
	bool PetWindowRect( client::WndRect& out );
	// Chat com os botões do Zero (16 px): os ícones do topo sobem de y 4 para 1 (não invadem o histórico)
	void FixChatIcons();
	void HideNativeAmmo();
	void DisableStatusWindow(); // Alt+A e os outros caminhos não abrem mais a janela de atributos // a nativa de equipamentos não desenha a munição no centro (por cima do personagem)
	// Botões antigos do Alt+V (ainda tratados pela UIBasicInfoWnd, mensagem 6): abrem/fecham a janela
	constexpr int LEGACY_MAP = 0xDB, LEGACY_GUILD = 0x175, LEGACY_PARTY = 0xC7;
	void LegacyBasicButton( int id );
	// Botões removidos: "ITEM" da barra de atalhos (Shortcut\btn_open/close_itemslot) e os dois do Alt+Q
	// (btn_e_ability e btn_e_change/off) saem da lista de filhos da janela (o objeto continua vivo:
	// a janela guarda ponteiro para eles)
	void RemoveButtons();
	bool PetCloseRect( client::WndRect& out );
	// Retângulos das janelas visíveis na frente da de pet (o que estiver ali não é da Pokédex)
	void PetCovers( std::vector<client::WndRect>& out );

	// Janela de quests nativa (CUIRenewQuestUI) e o botão de fechar dela
	bool QuestWindowRect( client::WndRect& out );  // aberta (lista de janelas do gerenciador)
	bool QuestCloseRect( client::WndRect& out );
	// Retângulos das janelas visíveis na frente da de quests (o que estiver ali não é da janela do Zero)
	void QuestCovers( std::vector<client::WndRect>& out );
	// O mesmo para cada janela nativa substituída: o que está na frente dela continua sendo desenhado
	// e recebendo os cliques (ex.: clã por cima da barra de status)
	enum class Replaced { Basic, Menu, Quest, Pet, Item, Compare, Equip, Inventory, Skill, Status, Storage, Minimap };
	void CoversOf( Replaced which, std::vector<client::WndRect>& out );
	uintptr_t ShownWindow( uintptr_t vt ); // janela aberta e visível da classe (0 = nenhuma)
	// janela de login do cliente: painel ui::LG_W x altura da tela em 0,0, filhos nas posições ui::LoginGeometry
	void SubstituteLoginWindow( int screenW, int screenH );
	void SubstituteWaitWindow( int screenW, int screenH );
	// Seleção de personagem hardcoded (ui::SEL_*): janela em tela cheia, slots como linhas da lista e botões do tema;
	// restore = devolve tudo ao lugar original (modo "Original" escolhido com a seleção aberta)
	void SubstituteSelectWindow( int screenW, int screenH, bool restore );
	void SubstituteMakeWindow( int screenW, int screenH, bool restore ); // criação hardcoded (ui::MK_*)
	std::wstring NpcName();  // nome do NPC da conversa atual (1º "[...]" da fala)
	void NpcDialogClosed();
	std::string NpcCutin(); // cutin do script na conversa atual ("" = nenhum)
	bool NpcSayOpen();
	void RodexFields(); // campos da carta (Rodex) no tema
	bool ResizeSurface( uintptr_t window, int width, int height ); // superfície nova do tamanho dado (UIWindow)
	void OptionButtons(); // opções Amigos/Grupo e da busca do Rodex como botões do tema
	void MerchantFields( uintptr_t window ); // campos (nome/preço) da loja de mercador nas cores do tema
	void ShopLayout( uintptr_t window, int kind ); // loja: botões do tema e campo (chamado no desenho da janela)
	int NpcButton(); // botão da faixa da fala: 0 nenhum, 1 seguir, 2 fechar
	void SubstituteNpcWindows( int screenW, int screenH ); // fala e menu do NPC no tema (ui::SAY_*, ui::CHOICE_*): cliques
	void NpcCreated( uintptr_t window, bool say );         // OnCreate: já no tamanho e no lugar do tema
	void NpcPlace( uintptr_t window, bool say );           // posição do tema (chamado também no desenho da janela)
	void NpcLayoutSay( uintptr_t say );                    // filhos (texto, botões): chamado no desenho da fala
	void NpcLayoutChoose( uintptr_t choose );              // lista do menu: chamado no desenho do menu
	int NpcChoiceRows( uintptr_t list );
	void DumpWindowTree( uintptr_t w, const char* label ); // TEMP: engenharia reversa
	void SubstituteServerWindow( int screenW, int screenH ); // UISelectServerWnd: cartão central (ui::SV_*) // UIWaitWnd: o mesmo painel, sem os filhos; redesenhada a cada quadro (spinner)
	bool Covered( Replaced which, float x, float y ); // ponto coberto por uma janela na frente
	// Janela de quests nativa com o tamanho da do Zero; o fechar nativo embaixo do X dela e o resto dos
	// controles fora (a janela inteira passa a ser tratada pelo cliente: clique, arraste, ordem, cursor)
	void SubstituteQuestWindow( int width, int height, int closeX, int closeY );
	void SubstitutePetWindow( int width, int height, int closeX, int closeY, int closeW, int closeH ); // o mesmo para a Pokédex
	// Menu nativo do tamanho do do Zero, com cada controle (ícones, abrir/fechar) embaixo do equivalente
	struct Placement { uintptr_t child; client::WndRect rect; }; // rect relativo à janela
	void SubstituteMenu( int width, int height, const std::vector<Placement>& place );
	// Janelas fixas do HUD: barra de status (canto superior esquerdo), chat e barra de atalhos (embaixo, no centro)
	enum class HudWnd { Basic, Chat, Shortcut, Minimap, QuickSlot, Cash, Roulette };
	bool PlayerCell( int& x, int& y ); // posição do personagem no mapa
	void CloseShortcutItemSlot();
	// fecha a fileira "item" da barra de atalhos
	// Mensagens da aba aberta do chat (as últimas 12; total = quantas a aba guarda) e se o campo de digitação está aberto
	struct ChatLine { std::wstring text; uint32_t color = 0xFFFFFFFF; };
	bool ReadChat( std::vector<ChatLine>& out, size_t& total );
	bool ChatTyping();
	bool HudWindowRect( HudWnd which, client::WndRect& out ); // visível
	void PlaceHudWindow( HudWnd which, int x, int y );
	void SubstituteBasicInfo( int width, int height ); // barra de status: do tamanho da do Zero, sem os controles antigos
	bool ReadMenu( MenuState& out );
	bool MenuRect( client::WndRect& out ); // retângulo atual da janela lida por ReadMenu
	void PlaceMenu( int x, int y ); // move a janela nativa (os filhos acompanham)

	// Ações: pacotes pela rotina de envio do próprio cliente (cifra o id como ele faz)
	bool SendPacket( const void* data, int len );
	void StatusUp( int stat );                 // 0 = FOR ... 5 = SOR
	void EquipItem( int index, uint32_t location );
	void UnequipItem( int index );
	// Opções do próprio personagem (ZC_STATE_CHANGE.effectState): falcão 0x10, montaria 0x20, dragão, mado...
	constexpr uint32_t OPT_FALCON = 0x10, OPT_RIDING = 0x20;
	uint32_t PlayerOption();
	void RemoveOption(); // CZ_UNINSTALLATION: tira falcão/montaria (sem eles, o carrinho)
	bool HasCart();      // carrinho equipado
	// Janela "Personagem" (equipamentos + atributos): Alt+A abre a de equipamentos e esconde a de atributos
	void OpenEquipWindow();
	// UIEquipWnd refeita como janela Personagem: o tamanho, os filhos (fechar, caixa exibir, carrinho) e onde
	// fica cada encaixe passam a ser os nossos (o cliente trata clique, arraste, soltar e dica sozinho).
	// slotAt(x, y relativos, wide = ícone + nome, visual, de outro jogador) -> índice do encaixe ou -1
	using EquipSlotAtFn = int( * )( int x, int y, bool wide, bool costume, bool other );
	// tipPos(mouse x, y relativos, x, y): onde a dica do nome do item aparece (relativo à janela)
	using EquipTipFn = void( * )( int mx, int my, int& x, int& y, bool other );
	bool InstallEquipLayout( EquipSlotAtFn slotAt, int characterFeetY, EquipTipFn tipPos ); // characterFeetY: onde o cliente desenha o personagem
	void SubstituteEquipWindow( int width, int height, const client::WndRect& close, const client::WndRect& check, const client::WndRect& cart );
	void EquipSetTab( int tab );
	void SetEquipContentTop( uintptr_t w, int y ); // +0xD4: topo do conteúdo das abas Título/Dano // 0 equipamentos, 1 visual
	void SetEquipFeetY( int y );
	// qual janela de equipamentos as funções acima usam: 0 = a do personagem, 1 = a de outro jogador
	void SelectEquipWnd( int which );
	void SelectEquipWndPtr( uintptr_t w ); // pela própria janela (no OnDraw dela)
	uintptr_t ClientBitmapRes( const std::string& path ); // CBitmapRes do cliente (guardado no quadro; 0 = não existe)
	void ClearBitmapCache(); // fim do quadro
	void CoversOfEquip( std::vector<client::WndRect>& out ); // janelas na frente da escolhida  // onde o cliente desenha o personagem (pés, relativo à janela)
	bool StatusWindowShown();
	void HideStatusWindow();
	void MoveEquipWindow( int x, int y );
	void DropItem( int index, int count );

	// Bitmap do cliente (caminho relativo a data\texture\, cp949), já com transparência
	bool ClientBitmap( const std::string& path, std::vector<uint32_t>& px, int& w, int& h );

	// Inventário (pacotes): itens que não estão vestidos
	std::vector<Item> Inventory();
	int InventoryCapacity();
	// Janela nativa do inventário (UIItemWnd): aba atual (0 consumo, 1 equip, 2 etc, 3 favoritos),
	// troca de aba como o clique faz e posição (célula) de um índice na aba atual (-1 = não está)
	bool InventoryWindowRect( client::WndRect& out );
	int NativeInventoryTab();
	void SetNativeInventoryTab( int tab );
	int NativeInventoryCell( int index );

	// Habilidades (pacotes ZC_SKILLINFO_*) e pontos (ZC_PAR_CHANGE 12)
	struct Skill { int id = 0, inf = 0, level = 0, sp = 0; bool upgradable = false; };
	std::map<int, Skill> Skills();
	int SkillPoints();
	int CurrentJob();
	void UpgradeSkill( int skill );
	bool SkillWindowRect( client::WndRect& out );
}
