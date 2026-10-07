#pragma once

#include <string>
#include <vector>

// Arquivos da interface: pasta "charselect\" ao lado do executável (se existir, para desenvolvimento)
// ou char.grf (entradas em data\charselect\...). Os caminhos recebidos são absolutos dentro de
// config::BaseDir() ou relativos a ele.
namespace vfs {
	bool Read( const std::wstring& path, std::vector<unsigned char>& out );
	bool Exists( const std::wstring& path );
	// Nomes (relativos à pasta pedida) dos arquivos de uma pasta, ex.: L"fonts\\"
	std::vector<std::wstring> List( const std::wstring& folder );
	// Pasta gravável (preferências, favoritos, log): savedata\charselect\ ao lado do executável
	const std::wstring& WritableDir();
	bool GrfLoaded();
}
