#include "chardata.hpp"

#include <winsock2.h>
#include <windows.h>
#include <array>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <map>
#include <unordered_map>

#include "config.hpp"
#include "log.hpp"
#include "client.hpp"
#include "game.hpp"
#include "d3d_hooks.hpp"
#include "vfs.hpp"

namespace {
	// O Codex.exe resolve recv com GetProcAddress na inicialização da rede e guarda o
	// ponteiro nesta global (proteção contra hook de IAT); o laço de leitura chama
	// "mov eax, [0x159E778]; call eax" em 0xBFC086.
	constexpr uintptr_t G_RECV_PTR = 0x159E778;
	constexpr uintptr_t SIG_RECV_CALL = 0xBFC086;

	// CHARACTER_INFO para PACKETVER 20250604 (src/common/packets.hpp)
	constexpr size_t CHAR_INFO_SIZE = 175;
	constexpr uint16_t HC_ACK_CHARINFO_PER_PAGE = 0x0B72;
	constexpr uint16_t HC_ACCEPT_MAKECHAR = 0x0B6F;
	constexpr uint16_t HC_ACCEPT_ENTER2 = 0x082D;
	constexpr uint16_t HC_DELETE_CHAR3 = 0x082A;
	constexpr uint16_t HC_DELETE_CHAR3_RESERVED = 0x0828;
	constexpr uint16_t HC_DELETE_CHAR3_CANCEL = 0x082C;
	constexpr size_t STREAM_KEEP = 4096;

	using RecvFn = int( WSAAPI* )( SOCKET, char*, int, int );
	RecvFn o_recv = nullptr;
	using SendFn = int( WSAAPI* )( SOCKET, const char*, int, int );
	SendFn o_send = nullptr;
	constexpr uintptr_t G_SEND_PTR = 0x159E774;   // mesma rotina que guarda o recv (0xBFC646)

	// Login digitado: o cliente manda a senha só como hash no CT_AUTH (0x0ACF), que o
	// rAthena não confere, e depois o CA_SSO_LOGIN_REQ (0x0825) com o token "token" que o
	// servidor devolveu no lugar da senha. Com "-t:senha" o token já é a senha; aqui o
	// token é trocado pela senha da janela de login, igual ao que o -t faz.
	constexpr uint16_t CA_SSO_LOGIN_REQ = 0x0825;
	constexpr size_t SSO_FIXED = 92;               // cabeçalho do 0x0825 antes do token
	char g_password[32] = {};
	bool g_sendOk = false;

	void CachePassword(){
		char buf[sizeof( g_password )] = {};
		if( client::ReadLoginPassword( buf, sizeof( buf ) ) && buf[0] ){
			memcpy( g_password, buf, sizeof( g_password ) );
		}
		SecureZeroMemory( buf, sizeof( buf ) );
	}

	int WSAAPI Hook_send( SOCKET s, const char* buf, int len, int flags ){
		if( len > (int)SSO_FIXED && *reinterpret_cast<const uint16_t*>( buf ) == CA_SSO_LOGIN_REQ
			&& *reinterpret_cast<const uint16_t*>( buf + 2 ) == len
			&& len - SSO_FIXED == 5 && memcmp( buf + SSO_FIXED, "token", 5 ) == 0 ){
			CachePassword();
			size_t pw = strnlen( g_password, sizeof( g_password ) );
			if( pw > 0 ){
				char out[SSO_FIXED + sizeof( g_password )];
				memcpy( out, buf, SSO_FIXED );
				memcpy( out + SSO_FIXED, g_password, pw );
				*reinterpret_cast<uint16_t*>( out + 2 ) = (uint16_t)( SSO_FIXED + pw );
				SecureZeroMemory( g_password, sizeof( g_password ) );
				int r = o_send( s, out, (int)( SSO_FIXED + pw ), flags );
				SecureZeroMemory( out, sizeof( out ) );
				logger::Write( "Login digitado: token do 0x0825 trocado pela senha" );
				return r == (int)( SSO_FIXED + pw ) ? len : r;
			}
			logger::Write( "AVISO: 0x0825 com token sem senha guardada" );
		}
		return o_send( s, buf, len, flags );
	}
	bool g_enabled = false;

	std::map<int, chardata::Character> g_chars; // slot -> personagem
	std::unordered_map<SOCKET, std::string> g_streams;
	std::unordered_map<int, std::array<std::wstring, 2>> g_jobNames;
	std::unordered_map<std::string, std::wstring> g_mapNames;

	// O servidor envia os segundos restantes até a exclusão ser liberada (PACKETVER_CHAR_DELETEDATE)
	uint32_t DeleteDateFromRemaining( int32_t remaining ){
		return remaining == 0 ? 0 : (uint32_t)( time( nullptr ) + remaining );
	}

	template <typename T> T Rd( const uint8_t* p, size_t ofs ){
		T v;
		memcpy( &v, p + ofs, sizeof( T ) );
		return v;
	}

	std::wstring Widen( const char* s, size_t max, UINT codepage ){
		size_t len = strnlen( s, max );
		int n = MultiByteToWideChar( codepage, 0, s, (int)len, nullptr, 0 );
		std::wstring out( n, L'\0' );
		MultiByteToWideChar( codepage, 0, s, (int)len, &out[0], n );
		return out;
	}

	bool ParseCharacter( const uint8_t* p, chardata::Character& c, int& slot ){
		const char* name = reinterpret_cast<const char*>( p + 108 );
		size_t nameLen = strnlen( name, 24 );
		if( nameLen == 0 || nameLen >= 24 ){
			return false;
		}
		for( size_t i = 0; i < nameLen; i++ ){
			if( (uint8_t)name[i] < 0x20 ){
				return false;
			}
		}

		slot = p[138];
		int level = Rd<int16_t>( p, 92 );
		if( slot >= 64 || level <= 0 || level > 9999 ){
			return false;
		}

		c.valid = true;
		c.gid = Rd<uint32_t>( p, 0 );
		c.baseExp = Rd<int64_t>( p, 4 );
		c.zeny = Rd<int32_t>( p, 12 );
		c.jobExp = Rd<int64_t>( p, 16 );
		c.jobLevel = Rd<int32_t>( p, 24 );
		c.hp = Rd<int64_t>( p, 50 );
		c.maxHp = Rd<int64_t>( p, 58 );
		c.sp = Rd<int64_t>( p, 66 );
		c.maxSp = Rd<int64_t>( p, 74 );
		c.job = Rd<int16_t>( p, 84 );
		c.baseLevel = level;
		c.headBottom = Rd<int16_t>( p, 96 );
		c.headTop = Rd<int16_t>( p, 100 );
		c.headMid = Rd<int16_t>( p, 102 );
		c.name = Widen( name, 24, config::Get().codepage );
		c.str = p[132];
		c.agi = p[133];
		c.vit = p[134];
		c.int_ = p[135];
		c.dex = p[136];
		c.luk = p[137];

		// Atributos completos (servidor O Codex): 16 bits em virtue/honor/jobpoint/sppoint.
		// Só usa se baterem com os valores de 1 byte (servidor sem a mudança não é afetado).
		uint32_t virtue = Rd<uint32_t>( p, 40 ), honor = Rd<uint32_t>( p, 44 );
		int full[6] = { (int)( virtue & 0xFFFF ), (int)( virtue >> 16 ), (int)( honor & 0xFFFF ), (int)( honor >> 16 ),
			(int)Rd<uint16_t>( p, 48 ), (int)Rd<uint16_t>( p, 94 ) };
		int* stats[6] = { &c.str, &c.agi, &c.vit, &c.int_, &c.dex, &c.luk };
		bool consistent = true;
		for( int k = 0; k < 6; k++ ){
			int byteVal = *stats[k];
			if( byteVal < 255 ? full[k] != byteVal : full[k] < 255 ){
				consistent = false;
			}
		}
		if( consistent ){
			for( int k = 0; k < 6; k++ ) *stats[k] = full[k];
		}
		c.map.assign( reinterpret_cast<const char*>( p + 142 ), strnlen( reinterpret_cast<const char*>( p + 142 ), 16 ) );
		size_t dot = c.map.find( '.' );
		if( dot != std::string::npos ){
			c.map.resize( dot );
		}
		c.deleteDate = DeleteDateFromRemaining( Rd<int32_t>( p, 158 ) );
		c.robe = Rd<int32_t>( p, 162 );
		c.sex = p[174];
		return true;
	}

	void StoreCharacter( const uint8_t* p ){
		chardata::Character c;
		int slot;
		if( ParseCharacter( p, c, slot ) ){
			g_chars[slot] = c;
		}
	}

	// Procura pacotes conhecidos no fluxo. A lista de personagens é validada pelo
	// tamanho; os pacotes curtos só são aceitos no início do bloco recebido.
	std::vector<chardata::Server> g_servers;

	void Scan( std::string& stream, size_t chunkStart ){
		const uint8_t* b = reinterpret_cast<const uint8_t*>( stream.data() );
		size_t n = stream.size();

		for( size_t i = 0; i + 4 <= n; i++ ){
			uint16_t id = Rd<uint16_t>( b, i );
			if( id == HC_ACK_CHARINFO_PER_PAGE ){
				uint16_t len = Rd<uint16_t>( b, i + 2 );
				if( len >= 4 && ( len - 4 ) % CHAR_INFO_SIZE == 0 && ( len - 4 ) / CHAR_INFO_SIZE <= 64 && i + len <= n ){
					for( size_t k = i + 4; k < i + len; k += CHAR_INFO_SIZE ){
						StoreCharacter( b + k );
					}
					i += len - 1;
				}
			}else if( id == HC_ACCEPT_ENTER2 && ( i == chunkStart || i == chunkStart + 4 ) && Rd<uint16_t>( b, i + 2 ) == 29 ){
				// Nova entrada no char-server: descarta a conta anterior
				g_chars.clear();
			}else if( ( id == 0x0AC4 || id == 0x0069 ) && i == chunkStart && i + 4 <= n ){
				// login aceito: lista de servidores de personagem
				size_t head = id == 0x0AC4 ? 64 : 47, entry = id == 0x0AC4 ? 160 : 32;
				uint16_t len = Rd<uint16_t>( b, i + 2 );
				if( len >= head && ( len - head ) % entry == 0 && i + len <= n ){
					g_servers.clear();
					for( size_t k = i + head; k + entry <= i + len; k += entry ){
						chardata::Server sv;
						sv.name = Widen( reinterpret_cast<const char*>( b + k + 6 ), 20, config::Get().codepage );
						sv.users = Rd<uint16_t>( b, k + 26 );
						g_servers.push_back( sv );
					}
					logger::Write( "Login aceito: %u servidores de personagem", (unsigned)g_servers.size() );
				}
			}else if( id == HC_ACCEPT_MAKECHAR && i == chunkStart && i + 2 + CHAR_INFO_SIZE <= n ){
				StoreCharacter( b + i + 2 );
			}else if( ( id == HC_DELETE_CHAR3_RESERVED || id == HC_DELETE_CHAR3_CANCEL ) && i == chunkStart
				&& i + ( id == HC_DELETE_CHAR3_RESERVED ? 14u : 10u ) <= n && Rd<int32_t>( b, i + 6 ) == 1 ){
				// Exclusão agendada (com a data) ou cancelada
				uint32_t gid = Rd<uint32_t>( b, i + 2 );
				for( auto& kv : g_chars ){
					if( kv.second.gid == gid ){
						kv.second.deleteDate = id == HC_DELETE_CHAR3_RESERVED ? DeleteDateFromRemaining( Rd<int32_t>( b, i + 10 ) ) : 0;
					}
				}
			}else if( id == HC_DELETE_CHAR3 && i == chunkStart && i + 10 <= n && Rd<int32_t>( b, i + 6 ) == 1 ){
				uint32_t gid = Rd<uint32_t>( b, i + 2 );
				for( auto it = g_chars.begin(); it != g_chars.end(); ++it ){
					if( it->second.gid == gid ){
						g_chars.erase( it );
						break;
					}
				}
			}
		}
	}

	int WSAAPI Hook_recv( SOCKET s, char* buf, int len, int flags ){
		int r = o_recv( s, buf, len, flags );
		static int logged = 0;
		if( r > 0 && config::Get().debug && logged < 40 ){
			logged++;
			logger::Write( "recv %d bytes, inicio %02X %02X %02X %02X (personagens conhecidos: %d)",
				r, (uint8_t)buf[0], r > 1 ? (uint8_t)buf[1] : 0, r > 2 ? (uint8_t)buf[2] : 0, r > 3 ? (uint8_t)buf[3] : 0, (int)g_chars.size() );
		}
		if( r > 0 && !( flags & MSG_PEEK ) ){
			game::OnRecv( (uintptr_t)s, buf, r );
			std::string& stream = g_streams[s];
			size_t chunkStart = stream.size();
			stream.append( buf, r );
			Scan( stream, chunkStart );
			if( stream.size() > STREAM_KEEP ){
				stream.erase( 0, stream.size() - STREAM_KEEP );
			}
		}else if( r == 0 ){
			g_streams.erase( s );
			game::OnClose( (uintptr_t)s );
		}
		return r;
	}

	std::wstring FromUtf8( const std::string& s ){
		int n = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0 );
		std::wstring out( n, L'\0' );
		MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n );
		return out;
	}

	template <typename F> void ReadTable( const std::wstring& path, F onEntry ){
		std::vector<unsigned char> data;
		if( !vfs::Read( path, data ) ){
			logger::Write( "AVISO: tabela ausente: %ls", path.c_str() );
			return;
		}
		std::string all( data.begin(), data.end() );
		size_t pos = 0;
		while( pos < all.size() ){
			size_t nl = all.find( '\n', pos );
			std::string line = all.substr( pos, nl == std::string::npos ? std::string::npos : nl - pos );
			pos = nl == std::string::npos ? all.size() : nl + 1;
			while( !line.empty() && ( line.back() == '\r' || line.back() == '\n' ) ) line.pop_back();
			if( line.size() >= 3 && (uint8_t)line[0] == 0xEF ) line.erase( 0, 3 ); // BOM
			if( line.empty() || line[0] == ';' ) continue;
			size_t eq = line.find( '=' );
			if( eq != std::string::npos ){
				onEntry( line.substr( 0, eq ), line.substr( eq + 1 ) );
			}
		}
	}
}

bool chardata::Install(){
	const uint8_t sig[] = { 0xA1, 0x78, 0xE7, 0x59, 0x01 }; // mov eax, [0x159E778]
	g_enabled = memcmp( reinterpret_cast<void*>( SIG_RECV_CALL ), sig, sizeof( sig ) ) == 0;
	const uint8_t sigSend[] = { 0xA3, 0x74, 0xE7, 0x59, 0x01 }; // mov [0x159E774], eax
	g_sendOk = memcmp( reinterpret_cast<void*>( 0xBFC646 ), sigSend, sizeof( sigSend ) ) == 0;
	if( !g_enabled ){
		logger::Write( "ERRO: leitura de rede do cliente diferente do esperado; dados dos personagens indisponiveis" );
	}
	return g_enabled;
}

void chardata::Poll(){
	if( !g_enabled ){
		return;
	}
	// A global só é preenchida quando o cliente inicia a rede; troca assim que aparecer
	volatile uintptr_t* slot = reinterpret_cast<volatile uintptr_t*>( G_RECV_PTR );
	uintptr_t current = *slot;
	if( current != 0 && current != reinterpret_cast<uintptr_t>( &Hook_recv ) ){
		o_recv = reinterpret_cast<RecvFn>( current );
		*slot = reinterpret_cast<uintptr_t>( &Hook_recv );
		logger::Write( "Hook de recv instalado (ponteiro do cliente)" );
	}
	volatile uintptr_t* sendSlot = reinterpret_cast<volatile uintptr_t*>( G_SEND_PTR );
	uintptr_t currentSend = *sendSlot;
	if( g_sendOk && currentSend != 0 && currentSend != reinterpret_cast<uintptr_t>( &Hook_send ) ){
		o_send = reinterpret_cast<SendFn>( currentSend );
		*sendSlot = reinterpret_cast<uintptr_t>( &Hook_send );
		logger::Write( "Hook de send instalado (ponteiro do cliente)" );
	}
	if( client::LoginWindow( client::LoginWnd::Login ) != nullptr ){
		CachePassword(); // a janela fecha antes do 0x0825 sair
	}
}

void chardata::LoadTables(){
	ReadTable( config::BaseDir() + L"classes.txt", []( const std::string& k, const std::string& v ){
		size_t bar = v.find( '|' );
		auto& names = g_jobNames[atoi( k.c_str() )];
		names[1] = FromUtf8( v.substr( 0, bar ) );
		names[0] = bar == std::string::npos ? names[1] : FromUtf8( v.substr( bar + 1 ) );
	} );
	ReadTable( config::BaseDir() + L"mapas.txt", []( const std::string& k, const std::string& v ){
		std::string name = v.substr( 0, v.find( ',' ) ); // "Kunlun, a Ilha..." -> "Kunlun"
		g_mapNames[k] = FromUtf8( name );
	} );
	logger::Write( "Tabelas: %u classes, %u mapas", (unsigned)g_jobNames.size(), (unsigned)g_mapNames.size() );
}

const chardata::Character* chardata::Get( int slot ){
	auto it = g_chars.find( slot );
	return it != g_chars.end() ? &it->second : nullptr;
}

int chardata::Count(){
	return (int)g_chars.size();
}

std::wstring chardata::JobName( int job, int sex ){
	auto it = g_jobNames.find( job );
	if( it == g_jobNames.end() ){
		return L"Classe " + std::to_wstring( job );
	}
	return it->second[sex ? 1 : 0];
}

std::wstring chardata::MapName( const std::string& map ){
	auto it = g_mapNames.find( map );
	return it != g_mapNames.end() ? it->second : FromUtf8( map );
}

const std::vector<chardata::Server>& chardata::CharServers(){
	return g_servers;
}

void chardata::ClearCharServers(){
	g_servers.clear();
}

const std::vector<std::wstring>& chardata::LoginServers(){
	static std::vector<std::wstring> list;
	static bool loaded = false;
	if( !loaded ){
		loaded = true;
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW( nullptr, exe, MAX_PATH );
		wchar_t* slash = wcsrchr( exe, L'\\' );
		if( slash ) slash[1] = 0;
		FILE* f = _wfopen( ( std::wstring( exe ) + L"data\\clientinfo.xml" ).c_str(), L"rb" );
		if( f ){
			std::string xml;
			char buf[4096];
			size_t got;
			while( ( got = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) xml.append( buf, got );
			fclose( f );
			size_t p = 0;
			while( ( p = xml.find( "<display>", p ) ) != std::string::npos ){
				size_t e = xml.find( "</display>", p );
				if( e == std::string::npos ) break;
				std::string name = xml.substr( p + 9, e - p - 9 );
				list.push_back( Widen( name.c_str(), name.size(), 1252 ) );
				p = e;
			}
		}
	}
	return list;
}
