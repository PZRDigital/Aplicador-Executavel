#pragma once

#include <d3d9.h>

// Fundo panorâmico: uma câmera 2D que se move sobre imagens do mapa
// e troca de enquadramento conforme o slot selecionado.
namespace scene {
	void SetSlot( int slot );   // chamado a cada frame com o slot selecionado
	void Reset();               // saiu da seleção: próxima entrada começa do zero
	bool Draw( IDirect3DDevice9* dev ); // false se nada foi desenhado (ex.: imagem ausente)
	void OnDeviceLost();        // antes de Reset do device
	void Preload();             // começa a carregar o mapa 3D em segundo plano (início do jogo)
	void Warmup( IDirect3DDevice9* dev ); // fora da seleção: deixa o mapa pronto na placa

	// Converte um ponto da imagem (0..1) para a tela, pela câmera do último frame.
	bool WorldToScreen( float u, float v, float& x, float& y );
	float ZoomRatio();          // zoom atual / zoom padrão (escala dos personagens)
	// Pés do personagem do slot na tela e escala do sprite (mapa 3D ou imagem)
	bool SlotToScreen( int slot, float& x, float& y, float& scale );
}
