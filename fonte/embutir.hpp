#pragma once

#include <string>
#include <vector>

// Embute a interface (DLL compilada) num Codex.exe gerado pelo WARP.
// Mesma lógica de CharSelectPanorama/tools/embutir.py.
namespace embutir {
	struct Resultado {
		bool ok = false;
		std::wstring mensagem;            // erro ou resumo
		std::vector<std::wstring> log;    // passos
	};

	// Alterações (iguais a config::Feature da interface)
	enum : unsigned { LOGIN = 1, SELECAO = 2, CRIACAO = 4, CHAVE = 8, COMBATE = 16, FADE = 32, STATUS = 64, ITEM = 128, SKIN = 256, EQUIP = 512, INVENTARIO = 1024, HABILIDADES = 2048, TODAS = 0xEFF };

	// exe: bytes do executável (entrada); dll: bytes da interface; saida: executável final
	Resultado Aplicar( const std::vector<unsigned char>& exe, const std::vector<unsigned char>& dll, unsigned alteracoes,
		std::vector<unsigned char>& saida );
}
