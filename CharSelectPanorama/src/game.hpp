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

	// Janelas nativas (UIWindowMgr 0x12F8268): retângulo na tela; false se fechada
	bool BasicInfoRect( client::WndRect& out );
	bool ItemWindowRect( client::WndRect& out );
	bool ReadItemWindow( Item& out );
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
	};
	struct MenuState {
		bool open = false;
		client::WndRect rect, toggle;
		std::vector<MenuButton> icons;
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
	// Janela de pet nativa (UIPetInfoWnd, Alt+J) e o botão de fechar dela
	bool PetWindowRect( client::WndRect& out );
	bool PetCloseRect( client::WndRect& out );

	// Janela de quests nativa (CUIRenewQuestUI) e o botão de fechar dela
	bool QuestWindowRect( client::WndRect& out );  // aberta (lista de janelas do gerenciador)
	bool QuestCloseRect( client::WndRect& out );
	bool ReadMenu( MenuState& out );
	bool MenuRect( client::WndRect& out ); // retângulo atual da janela lida por ReadMenu
	void PlaceMenu( int x, int y ); // move a janela nativa (os filhos acompanham)

	// Ações: pacotes pela rotina de envio do próprio cliente (cifra o id como ele faz)
	bool SendPacket( const void* data, int len );
	void StatusUp( int stat );                 // 0 = FOR ... 5 = SOR
	void EquipItem( int index, uint32_t location );
	void UnequipItem( int index );
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
