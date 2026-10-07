#include "embutir.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {
	DWORD Alinhar( DWORD v, DWORD a ){ return ( v + a - 1 ) / a * a; }

	std::wstring Fmt( const wchar_t* f, ... ){
		wchar_t buf[512];
		va_list ap;
		va_start( ap, f );
		_vsnwprintf_s( buf, _countof( buf ), _TRUNCATE, f, ap );
		va_end( ap );
		return buf;
	}

	struct Pe {
		const unsigned char* d = nullptr;
		size_t n = 0;
		const IMAGE_NT_HEADERS32* nt = nullptr;
		const IMAGE_SECTION_HEADER* sec = nullptr;
		int nsec = 0;

		bool Abrir( const std::vector<unsigned char>& v ){
			d = v.data(); n = v.size();
			if( n < sizeof( IMAGE_DOS_HEADER ) ) return false;
			auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( d );
			if( dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof( IMAGE_NT_HEADERS32 ) > n ) return false;
			nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>( d + dos->e_lfanew );
			if( nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ) return false;
			sec = IMAGE_FIRST_SECTION( nt );
			nsec = nt->FileHeader.NumberOfSections;
			return reinterpret_cast<const unsigned char*>( sec + nsec ) <= d + n;
		}

		// RVA -> deslocamento no arquivo (0 = fora das seções)
		size_t Ofs( DWORD rva ) const {
			for( int i = 0; i < nsec; i++ ){
				DWORD tam = max( sec[i].Misc.VirtualSize, sec[i].SizeOfRawData );
				if( rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + tam ){
					DWORD o = rva - sec[i].VirtualAddress;
					return o < sec[i].SizeOfRawData ? sec[i].PointerToRawData + o : 0;
				}
			}
			return rva < nt->OptionalHeader.SizeOfHeaders ? rva : 0;
		}
	};

	DWORD Checksum( const std::vector<unsigned char>& d, size_t ofsChecksum ){
		unsigned long long soma = 0;
		size_t n = d.size();
		for( size_t i = 0; i + 1 < n + 1; i += 2 ){
			if( i == ofsChecksum || i == ofsChecksum + 2 ) continue;
			unsigned w = d[i] | ( i + 1 < n ? d[i + 1] << 8 : 0 );
			soma += w;
			soma = ( soma & 0xFFFF ) + ( soma >> 16 );
		}
		soma = ( soma & 0xFFFF ) + ( soma >> 16 );
		return (DWORD)( soma + n );
	}
}

namespace {
	// Executável que já recebeu a interface: devolve o executável como era antes (sem a seção .codex)
	// e a lista de importações originais dele. false = não tinha a interface.
	bool Desembutir( const std::vector<unsigned char>& in, std::vector<unsigned char>& base, std::vector<IMAGE_IMPORT_DESCRIPTOR>& descs,
		std::wstring& erro ){
		Pe pe;
		if( !pe.Abrir( in ) ) return false;
		const IMAGE_SECTION_HEADER& cx = pe.sec[pe.nsec - 1];
		if( strncmp( reinterpret_cast<const char*>( cx.Name ), ".codex", 8 ) != 0 ){
			for( int i = 0; i < pe.nsec; i++ ){
				if( strncmp( reinterpret_cast<const char*>( pe.sec[i].Name ), ".codex", 8 ) == 0 ){
					erro = L"A seção da interface não é a última do executável; não dá para reaplicar.";
					return true;
				}
			}
			return false;
		}
		const IMAGE_OPTIONAL_HEADER32& o = pe.nt->OptionalHeader;
		// trecho de entrada: 6A 00 6A 01 68 <base> E8 <rel> E9 <rel para o início original>
		size_t ofs = pe.Ofs( o.AddressOfEntryPoint );
		const unsigned char esperado[] = { 0x6A, 0x00, 0x6A, 0x01, 0x68 };
		if( ofs == 0 || ofs + 20 > pe.n || memcmp( pe.d + ofs, esperado, 5 ) != 0 || pe.d[ofs + 9] != 0xE8 || pe.d[ofs + 14] != 0xE9 ){
			erro = L"Não reconheci o início da interface já aplicada; use o executável hexed.";
			return true;
		}
		int rel;
		memcpy( &rel, pe.d + ofs + 15, 4 );
		DWORD oep = o.ImageBase + o.AddressOfEntryPoint + 19 + rel; // destino do jmp (E9 em +14, 5 bytes)
		DWORD oepRva = oep - o.ImageBase;
		// importações originais: as que não estão dentro da seção da interface
		size_t io = pe.Ofs( o.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress );
		for( ; io && io + sizeof( IMAGE_IMPORT_DESCRIPTOR ) <= pe.n; io += sizeof( IMAGE_IMPORT_DESCRIPTOR ) ){
			IMAGE_IMPORT_DESCRIPTOR d = *reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>( pe.d + io );
			if( d.Name == 0 ) break;
			if( d.Name < cx.VirtualAddress ) descs.push_back( d );
		}
		if( descs.empty() ){
			erro = L"Tabela de importação do executável não encontrada.";
			return true;
		}
		// arquivo sem a seção, com o cabeçalho como antes
		base.assign( in.begin(), in.begin() + cx.PointerToRawData );
		size_t ntOfs = reinterpret_cast<const unsigned char*>( pe.nt ) - pe.d;
		auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>( base.data() + ntOfs );
		auto secs = IMAGE_FIRST_SECTION( nt );
		int n = --nt->FileHeader.NumberOfSections;
		memset( &secs[n], 0, sizeof( IMAGE_SECTION_HEADER ) );
		const IMAGE_SECTION_HEADER& ult = secs[n - 1];
		nt->OptionalHeader.SizeOfImage = Alinhar( ult.VirtualAddress + max( ult.Misc.VirtualSize, ult.SizeOfRawData ), o.SectionAlignment );
		nt->OptionalHeader.AddressOfEntryPoint = oepRva;
		return true;
	}

	embutir::Resultado AplicarBase( const std::vector<unsigned char>& exeBytes, const std::vector<unsigned char>& dllBytes, unsigned alteracoes,
		std::vector<unsigned char>& saida, const std::vector<IMAGE_IMPORT_DESCRIPTOR>* importsOriginais );
}

embutir::Resultado embutir::Aplicar( const std::vector<unsigned char>& exeBytes, const std::vector<unsigned char>& dllBytes, unsigned alteracoes,
	std::vector<unsigned char>& saida ){
	std::vector<unsigned char> base;
	std::vector<IMAGE_IMPORT_DESCRIPTOR> descs;
	std::wstring erro;
	if( Desembutir( exeBytes, base, descs, erro ) ){
		Resultado r;
		if( !erro.empty() ){ r.mensagem = erro; return r; }
		r = AplicarBase( base, dllBytes, alteracoes, saida, &descs );
		r.log.insert( r.log.begin(), L"Este executável já tinha a interface: a anterior foi retirada e a atual aplicada com as alterações escolhidas." );
		return r;
	}
	return AplicarBase( exeBytes, dllBytes, alteracoes, saida, nullptr );
}

namespace {
embutir::Resultado AplicarBase( const std::vector<unsigned char>& exeBytes, const std::vector<unsigned char>& dllBytes, unsigned alteracoes,
	std::vector<unsigned char>& saida, const std::vector<IMAGE_IMPORT_DESCRIPTOR>* importsOriginais ){
	using embutir::Resultado;
	Resultado r;
	Pe exe, dll;
	if( !exe.Abrir( exeBytes ) ){ r.mensagem = L"O arquivo escolhido não é um executável de 32 bits válido."; return r; }
	if( !dll.Abrir( dllBytes ) ){ r.mensagem = L"A interface embutida no aplicador está corrompida."; return r; }
	const IMAGE_OPTIONAL_HEADER32& eo = exe.nt->OptionalHeader;
	const IMAGE_OPTIONAL_HEADER32& dop = dll.nt->OptionalHeader;

	if( eo.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE ){
		r.mensagem = L"O executável usa ASLR (DYNAMIC_BASE). Desative essa opção no WARP e gere de novo.";
		return r;
	}
	if( eo.ImageBase != 0x400000 ){
		r.mensagem = L"Base do executável diferente de 0x400000: não parece ser o Codex.exe.";
		return r;
	}
	// mesmo cliente para o qual a interface foi feita (Codex.exe 2025-06-04)
	{
		struct Assin { DWORD va; unsigned char b[5]; int n; const wchar_t* oque; };
		const Assin as[] = {
			{ 0xD0C2F3, { 0xA0, 0xC2, 0x0F, 0x5D, 0x01 }, 5, L"seleção de personagem" },
			{ 0xFFD194, { 0xE0, 0x8E, 0x79, 0x00 }, 4, L"janela de seleção" },
		};
		for( const Assin& a : as ){
			size_t o = exe.Ofs( a.va - eo.ImageBase );
			if( o == 0 || o + a.n > exe.n || memcmp( exe.d + o, a.b, a.n ) != 0 ){
				r.mensagem = Fmt( L"Este executável não é da versão do cliente suportada (Codex.exe 2025-06-04): %s diferente.\n"
					L"A interface só funciona nessa versão.", a.oque );
				return r;
			}
		}
		r.log.push_back( L"Executável reconhecido: Codex.exe 2025-06-04." );
	}
	if( dop.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size != 0 ){
		r.mensagem = L"A interface usa TLS e não pode ser embutida.";
		return r;
	}

	const IMAGE_SECTION_HEADER& ult = exe.sec[exe.nsec - 1];
	DWORD secRva = Alinhar( ult.VirtualAddress + max( ult.Misc.VirtualSize, ult.SizeOfRawData ), eo.SectionAlignment );
	DWORD novaBase = eo.ImageBase + secRva;

	// 1) imagem da DLL montada e realocada
	std::vector<unsigned char> img( dop.SizeOfImage, 0 );
	memcpy( img.data(), dll.d, min( (size_t)dop.SizeOfHeaders, dll.n ) );
	for( int i = 0; i < dll.nsec; i++ ){
		const IMAGE_SECTION_HEADER& s = dll.sec[i];
		size_t tam = min( (size_t)s.SizeOfRawData, (size_t)max( s.Misc.VirtualSize, s.SizeOfRawData ) );
		tam = min( tam, (size_t)dop.SizeOfImage - s.VirtualAddress );
		if( s.PointerToRawData + tam > dll.n ) tam = dll.n - s.PointerToRawData;
		memcpy( img.data() + s.VirtualAddress, dll.d + s.PointerToRawData, tam );
	}
	// alterações escolhidas: marcador "CODEX_FEAT01" + flags na imagem da interface
	{
		const char magic[] = "CODEX_FEAT01";
		bool achou = false;
		for( size_t i = 0; i + 16 <= img.size(); i++ ){
			if( memcmp( img.data() + i, magic, 12 ) == 0 ){
				memcpy( img.data() + i + 12, &alteracoes, 4 );
				achou = true;
				break;
			}
		}
		if( !achou ){
			r.mensagem = L"A interface embutida no aplicador não aceita escolha de alterações (versão antiga).";
			return r;
		}
	}

	DWORD delta = novaBase - dop.ImageBase;
	int nReloc = 0;
	{
		const IMAGE_DATA_DIRECTORY& rd = dop.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
		DWORD p = rd.VirtualAddress, fim = rd.VirtualAddress + rd.Size;
		while( rd.Size && p + 8 <= fim ){
			auto blk = reinterpret_cast<IMAGE_BASE_RELOCATION*>( img.data() + p );
			if( blk->SizeOfBlock < 8 ) break;
			auto ent = reinterpret_cast<WORD*>( blk + 1 );
			int qtd = ( blk->SizeOfBlock - 8 ) / 2;
			for( int k = 0; k < qtd; k++ ){
				int tipo = ent[k] >> 12;
				DWORD rva = blk->VirtualAddress + ( ent[k] & 0xFFF );
				if( tipo == IMAGE_REL_BASED_HIGHLOW ){
					*reinterpret_cast<DWORD*>( img.data() + rva ) += delta;
					nReloc++;
				}else if( tipo != IMAGE_REL_BASED_ABSOLUTE ){
					r.mensagem = Fmt( L"Realocação de tipo %d não suportada.", tipo );
					return r;
				}
			}
			p += blk->SizeOfBlock;
		}
	}

	// 2) importações da DLL passam a apontar para a nova seção
	std::vector<IMAGE_IMPORT_DESCRIPTOR> descs;
	{
		DWORD o = dop.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		for( ;; o += sizeof( IMAGE_IMPORT_DESCRIPTOR ) ){
			IMAGE_IMPORT_DESCRIPTOR d = *reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>( img.data() + o );
			if( d.Name == 0 ) break;
			DWORD thunks[2] = { d.OriginalFirstThunk, d.FirstThunk };
			for( int t = 0; t < 2; t++ ){
				if( thunks[t] == 0 || ( t == 1 && thunks[1] == thunks[0] ) ) continue;
				for( DWORD p = thunks[t];; p += 4 ){
					DWORD& v = *reinterpret_cast<DWORD*>( img.data() + p );
					if( v == 0 ) break;
					if( !( v & IMAGE_ORDINAL_FLAG32 ) ) v += secRva;
				}
			}
			IMAGE_IMPORT_DESCRIPTOR n = {};
			n.OriginalFirstThunk = d.OriginalFirstThunk ? d.OriginalFirstThunk + secRva : 0;
			n.Name = d.Name + secRva;
			n.FirstThunk = d.FirstThunk + secRva;
			descs.push_back( n );
		}
	}

	// 3) tabela nova: a do executável (sem codex_ui.dll) + a da interface
	std::vector<IMAGE_IMPORT_DESCRIPTOR> tabela;
	{
		std::vector<IMAGE_IMPORT_DESCRIPTOR> orig;
		if( importsOriginais ){
			orig = *importsOriginais;
		}else{
			size_t o = exe.Ofs( eo.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress );
			if( o == 0 ){ r.mensagem = L"Tabela de importação do executável não encontrada."; return r; }
			for( ;; o += sizeof( IMAGE_IMPORT_DESCRIPTOR ) ){
				IMAGE_IMPORT_DESCRIPTOR d = *reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>( exe.d + o );
				if( d.Name == 0 ) break;
				orig.push_back( d );
			}
		}
		for( IMAGE_IMPORT_DESCRIPTOR d : orig ){
			size_t on = exe.Ofs( d.Name );
			const char* nome = on ? reinterpret_cast<const char*>( exe.d + on ) : "";
			if( _stricmp( nome, "codex_ui.dll" ) == 0 ){
				r.log.push_back( L"Importação codex_ui.dll (patch de DLL do WARP) retirada: não é mais necessária." );
				continue;
			}
			d.TimeDateStamp = 0;
			d.ForwarderChain = 0;
			tabela.push_back( d );
		}
		tabela.insert( tabela.end(), descs.begin(), descs.end() );
		tabela.push_back( IMAGE_IMPORT_DESCRIPTOR{} );
	}
	DWORD tamTabela = (DWORD)( tabela.size() * sizeof( IMAGE_IMPORT_DESCRIPTOR ) );
	DWORD impRva = secRva + Alinhar( (DWORD)img.size(), 16 );

	// 4) entrada: DllMain(base, DLL_PROCESS_ATTACH, 0) e segue para o início original
	DWORD stubRva = Alinhar( impRva + tamTabela, 16 );
	DWORD entradaVa = novaBase + dop.AddressOfEntryPoint;
	DWORD oepVa = eo.ImageBase + eo.AddressOfEntryPoint;
	std::vector<unsigned char> stub = { 0x6A, 0x00, 0x6A, 0x01, 0x68 };
	auto put32 = [&stub]( DWORD v ){ for( int i = 0; i < 4; i++ ) stub.push_back( (unsigned char)( v >> ( 8 * i ) ) ); };
	put32( novaBase );
	DWORD callEm = eo.ImageBase + stubRva + (DWORD)stub.size();
	stub.push_back( 0xE8 ); put32( entradaVa - ( callEm + 5 ) );
	DWORD jmpEm = eo.ImageBase + stubRva + (DWORD)stub.size();
	stub.push_back( 0xE9 ); put32( oepVa - ( jmpEm + 5 ) );

	std::vector<unsigned char> corpo( stubRva - secRva + stub.size(), 0 );
	memcpy( corpo.data(), img.data(), img.size() );
	memcpy( corpo.data() + ( impRva - secRva ), tabela.data(), tamTabela );
	memcpy( corpo.data() + ( stubRva - secRva ), stub.data(), stub.size() );

	// 5) seção nova no arquivo
	saida = exeBytes;
	DWORD rawPtr = Alinhar( (DWORD)saida.size(), eo.FileAlignment );
	DWORD rawTam = Alinhar( (DWORD)corpo.size(), eo.FileAlignment );
	saida.resize( rawPtr, 0 );
	saida.insert( saida.end(), corpo.begin(), corpo.end() );
	saida.resize( rawPtr + rawTam, 0 );

	size_t ntOfs = reinterpret_cast<const unsigned char*>( exe.nt ) - exe.d;
	auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>( saida.data() + ntOfs );
	auto secs = IMAGE_FIRST_SECTION( nt );
	size_t shOfs = reinterpret_cast<unsigned char*>( secs + nt->FileHeader.NumberOfSections ) - saida.data();
	DWORD primeiroRaw = 0xFFFFFFFF;
	for( int i = 0; i < nt->FileHeader.NumberOfSections; i++ ) if( secs[i].PointerToRawData ) primeiroRaw = min( primeiroRaw, secs[i].PointerToRawData );
	if( shOfs + sizeof( IMAGE_SECTION_HEADER ) > primeiroRaw || shOfs + sizeof( IMAGE_SECTION_HEADER ) > nt->OptionalHeader.SizeOfHeaders ){
		r.mensagem = L"Não há espaço no cabeçalho do executável para mais uma seção.";
		return r;
	}
	IMAGE_SECTION_HEADER nova = {};
	memcpy( nova.Name, ".codex", 6 );
	nova.Misc.VirtualSize = (DWORD)corpo.size();
	nova.VirtualAddress = secRva;
	nova.SizeOfRawData = rawTam;
	nova.PointerToRawData = rawPtr;
	nova.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
	memcpy( saida.data() + shOfs, &nova, sizeof( nova ) );

	nt->FileHeader.NumberOfSections++;
	IMAGE_OPTIONAL_HEADER32& oh = nt->OptionalHeader;
	oh.SizeOfImage = Alinhar( secRva + (DWORD)corpo.size(), oh.SectionAlignment );
	oh.AddressOfEntryPoint = stubRva;
	oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = impRva;
	oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = tamTabela;
	oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = {};
	oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] = {}; // assinatura deixa de valer
	oh.CheckSum = 0;
	size_t ofsCk = reinterpret_cast<unsigned char*>( &oh.CheckSum ) - saida.data();
	DWORD ck = Checksum( saida, ofsCk );
	reinterpret_cast<IMAGE_NT_HEADERS32*>( saida.data() + ntOfs )->OptionalHeader.CheckSum = ck;

	r.log.push_back( Fmt( L"Interface embutida na seção .codex em 0x%X (%u KB, %d realocações).", novaBase, (unsigned)( corpo.size() / 1024 ), nReloc ) );
	r.log.push_back( Fmt( L"Tabela de importação: %u bibliotecas.", (unsigned)( tabela.size() - 1 ) ) );
	r.ok = true;
	r.mensagem = L"Interface aplicada com sucesso.";
	return r;
}
}
