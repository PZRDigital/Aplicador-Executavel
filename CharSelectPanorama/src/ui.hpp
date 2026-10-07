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
	};
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
	void DrawStatusHud( IDirect3DDevice9* dev, float x, float y, const game::Status& status );
	bool StatusHudRedirect( const input::Point& real, input::Point& target, float nativeX, float nativeY );

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
	bool InventoryRedirect( const input::Point& real, input::Point& target );

	// Habilidades (no lugar da UINewSkillListWnd). "Configurar Atalhos" mostra a lista nativa por cima.
	struct SkillFrame {
		float nx = 0, ny = 0, nw = 0, nh = 0;
		float vpWidth = 0, vpHeight = 0;
	};
	void DrawSkillWindow( IDirect3DDevice9* dev, const SkillFrame& frame );
	bool SkillRedirect( const input::Point& real, input::Point& target );
	bool SkillNativeVisible(); // lista nativa à mostra (arrastar para os atalhos)
}
