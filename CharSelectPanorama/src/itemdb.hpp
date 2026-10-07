#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Nomes e descrições dos itens: System\Codex.lub, bRO.lub e itemInfo_EN.lub (a mesma ordem
// do System\iteminfo.lub do cliente; o primeiro arquivo que tiver o item vale).
namespace itemdb {
	struct Entry {
		std::wstring name;
		std::string resource;              // identifiedResourceName (cp949): ícone e ilustração
		int slots = 0;
		std::vector<std::wstring> lines;   // efeitos (com códigos de cor ^RRGGBB)
		// rodapé padronizado das descrições do bRO
		std::wstring type, classes;
		int atk = -1, matk = -1, def = -1, weight = -1, weaponLevel = -1, armorLevel = -1, requiredLevel = -1;
	};
	void StartLoading();                        // lê os arquivos em segundo plano
	bool Ready();
	bool Get( uint32_t id, Entry& out );
	std::wstring Name( uint32_t id );           // vazio se não existir
}
