#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Dados dos personagens lidos dos pacotes do char-server (recv interceptado).
namespace chardata {
	struct Character {
		bool valid = false;
		uint32_t gid = 0;
		std::wstring name;
		int job = 0;
		int baseLevel = 0;
		int jobLevel = 0;
		int64_t hp = 0, maxHp = 0, sp = 0, maxSp = 0;
		int64_t baseExp = 0, jobExp = 0;
		int zeny = 0;
		int str = 0, agi = 0, vit = 0, int_ = 0, dex = 0, luk = 0;
		std::string map;
		int sex = 0; // 0 = feminino, 1 = masculino
		// Visual
		int headTop = 0, headMid = 0, headBottom = 0, robe = 0;
		uint32_t deleteDate = 0; // exclusão agendada (unix time); 0 = nenhuma
	};

	bool Install();                        // confere o executável
	void Poll();                           // instala o hook de recv quando a rede do cliente iniciar
	void LoadTables();                     // charselect\classes.txt e mapas.txt
	const Character* Get( int slot );      // slot absoluto; nullptr se vazio
	int Count();                           // personagens conhecidos
	std::wstring JobName( int job, int sex );
	std::wstring MapName( const std::string& map );

	// Lista de servidores de personagem (pacote 0x0AC4/0x0069 do login-server)
	struct Server {
		std::wstring name;
		int users = 0;
	};
	const std::vector<Server>& CharServers();
	void ClearCharServers();                  // tela de login aberta de novo
	// Servidores de login (data\clientinfo.xml, <display>)
	const std::vector<std::wstring>& LoginServers();
}
