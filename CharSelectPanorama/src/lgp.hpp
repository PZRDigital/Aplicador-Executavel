#pragma once

#include <cstddef>
#include <cstdint>

// LGP: marcações nas células do chão (áreas de skills, quadrado e anel em volta do personagem).
// Implementação própria para o Codex.exe 2025-06-04 (endereços em Projeto LGP\MAPA_CLIENTE.md).
//
// Comandos do chat (não vão para o servidor):
//   @lgp [on|off]        liga/desliga o módulo (sem argumento alterna)
//   @aoes [on|off]       área inteira de Storm Gust / Lord of Vermilion / Meteor Storm (desligado = só o centro)
//   @circle [on|off]     anel em volta do personagem
//   @square on|off|1-18  contorno de quadrado a N células do personagem
// Configuração do servidor: lgp.ini (pasta charselect\ ou char.grf). Escolhas do jogador:
// savedata\charselect\preferencias.ini, seção [lgp].
struct IDirect3DDevice9;

namespace lgp {
	// Confere as assinaturas do executável e instala os desvios. false = cliente diferente (nada é alterado).
	bool Install();
	// Cada pacote recebido do servidor (chamado por game::Handle).
	void OnPacket( uint16_t id, const uint8_t* data, size_t len );
	// Device criado (depois dos desvios do framework): BeginScene/EndScene/SetRenderState para o diagnóstico.
	void OnDevice( IDirect3DDevice9* dev );
	// Cada desenho do cliente (BeforeDraw do framework), para a linha do tempo do diagnóstico.
	void CountDraw();
	// Texturas em D3DPOOL_DEFAULT: liberar antes do Reset do device.
	void OnDeviceLost();
}
