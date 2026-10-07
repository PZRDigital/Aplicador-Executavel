#include "itemdb.hpp"

#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "config.hpp"
#include "log.hpp"

namespace {
	struct Source {
		std::string text;
	};
	std::vector<Source> g_files;
	std::unordered_map<uint32_t, std::pair<int, size_t>> g_index; // id -> (arquivo, início da entrada)
	std::unordered_map<uint32_t, itemdb::Entry> g_cache;
	std::mutex g_lock;
	std::atomic<bool> g_ready{ false };
	std::atomic<bool> g_started{ false };

	std::wstring ExeDir(){
		wchar_t path[MAX_PATH];
		GetModuleFileNameW( nullptr, path, MAX_PATH );
		std::wstring s = path;
		return s.substr( 0, s.find_last_of( L"\\/" ) + 1 );
	}

	bool ReadFile( const std::wstring& path, std::string& out ){
		HANDLE h = CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr );
		if( h == INVALID_HANDLE_VALUE ) return false;
		LARGE_INTEGER size;
		GetFileSizeEx( h, &size );
		out.resize( (size_t)size.QuadPart );
		DWORD got = 0;
		BOOL ok = ::ReadFile( h, &out[0], (DWORD)out.size(), &got, nullptr );
		CloseHandle( h );
		return ok && got == out.size();
	}

	std::wstring Widen( const std::string& s ){
		int n = MultiByteToWideChar( config::Get().codepage, 0, s.data(), (int)s.size(), nullptr, 0 );
		std::wstring w( n, L'\0' );
		if( n > 0 ) MultiByteToWideChar( config::Get().codepage, 0, s.data(), (int)s.size(), &w[0], n );
		return w;
	}

	// Índice das entradas "[id] = {" no nível de tbl, fora de comentários e textos
	void IndexFile( int file ){
		const std::string& t = g_files[file].text;
		size_t n = t.size(), i = 0;
		int depth = 0;
		while( i < n ){
			char c = t[i];
			if( c == '-' && i + 1 < n && t[i + 1] == '-' ){
				if( i + 3 < n && t[i + 2] == '[' && t[i + 3] == '[' ){
					size_t e = t.find( "]]", i + 4 );
					i = e == std::string::npos ? n : e + 2;
				}else{
					size_t e = t.find( '\n', i );
					i = e == std::string::npos ? n : e + 1;
				}
				continue;
			}
			if( c == '"' ){
				for( i++; i < n && t[i] != '"'; i++ ) if( t[i] == '\\' ) i++;
				i++;
				continue;
			}
			if( c == '{' ){ depth++; i++; continue; }
			if( c == '}' ){ depth--; i++; continue; }
			if( c == '[' && depth == 1 ){
				char* end = nullptr;
				unsigned long id = strtoul( t.c_str() + i + 1, &end, 10 );
				if( end && *end == ']' && end > t.c_str() + i + 1 ){
					g_index.emplace( (uint32_t)id, std::make_pair( file, i ) ); // o primeiro arquivo vale
				}
			}
			i++;
		}
	}

	// Texto entre aspas a partir de p (p no '"'); avança p
	std::string QuotedAt( const std::string& t, size_t& p ){
		std::string out;
		for( p++; p < t.size() && t[p] != '"'; p++ ){
			if( t[p] == '\\' && p + 1 < t.size() ){
				p++;
				out += t[p] == 'n' ? '\n' : t[p];
				continue;
			}
			out += t[p];
		}
		p++;
		return out;
	}

	size_t EntryEnd( const std::string& t, size_t start ){
		int depth = 0;
		for( size_t i = t.find( '{', start ); i < t.size(); i++ ){
			if( t[i] == '"' ){ for( i++; i < t.size() && t[i] != '"'; i++ ) if( t[i] == '\\' ) i++; continue; }
			if( t[i] == '{' ) depth++;
			if( t[i] == '}' && --depth == 0 ) return i;
		}
		return t.size();
	}

	size_t FindKey( const std::string& t, const char* key, size_t from, size_t to ){
		size_t len = strlen( key );
		for( size_t p = t.find( key, from ); p != std::string::npos && p < to; p = t.find( key, p + 1 ) ){
			char before = p > 0 ? t[p - 1] : ' ';
			if( isalnum( (unsigned char)before ) ) continue;
			size_t q = p + len;
			while( q < to && ( t[q] == ' ' || t[q] == '\t' ) ) q++;
			if( q < to && t[q] == '=' ) return q + 1;
		}
		return std::string::npos;
	}

	std::wstring StripColors( const std::wstring& s ){
		std::wstring o;
		for( size_t i = 0; i < s.size(); i++ ){
			if( s[i] == L'^' && i + 6 < s.size() && iswxdigit( s[i + 1] ) ){ i += 6; continue; }
			o += s[i];
		}
		return o;
	}

	int NumberAfter( const std::wstring& line, const wchar_t* label ){
		size_t p = line.find( label );
		if( p == std::wstring::npos ) return -1;
		p += wcslen( label );
		while( p < line.size() && !iswdigit( line[p] ) && line[p] != L'-' ) p++;
		if( p >= line.size() ) return -1;
		return _wtoi( line.c_str() + p );
	}

	bool StartsWith( const std::wstring& s, const wchar_t* prefix ){
		return _wcsnicmp( s.c_str(), prefix, wcslen( prefix ) ) == 0;
	}

	itemdb::Entry Parse( int file, size_t start ){
		const std::string& t = g_files[file].text;
		size_t end = EntryEnd( t, start );
		itemdb::Entry e;
		size_t p = FindKey( t, "identifiedDisplayName", start, end );
		if( p != std::string::npos ){ p = t.find( '"', p ); if( p < end ) e.name = Widen( QuotedAt( t, p ) ); }
		p = FindKey( t, "identifiedResourceName", start, end );
		if( p != std::string::npos ){ p = t.find( '"', p ); if( p < end ) e.resource = QuotedAt( t, p ); }
		p = FindKey( t, "slotCount", start, end );
		if( p != std::string::npos ) e.slots = atoi( t.c_str() + p );
		p = FindKey( t, "identifiedDescriptionName", start, end );
		if( p != std::string::npos ){
			size_t open = t.find( '{', p ), close = t.find( '}', open );
			for( size_t q = t.find( '"', open ); q < close; q = t.find( '"', q ) ){
				e.lines.push_back( Widen( QuotedAt( t, q ) ) );
				close = t.find( '}', q );
			}
		}
		// rodapé: "Tipo:", "ATQ:", "Peso:", "Nível da arma:", "Nível necessário:", "Classes:"
		std::vector<std::wstring> body;
		for( const std::wstring& raw : e.lines ){
			std::wstring l = StripColors( raw );
			bool footer = true;
			if( StartsWith( l, L"Tipo:" ) ) e.type = l.substr( 5 );
			else if( StartsWith( l, L"Classes:" ) ) e.classes = l.substr( 8 );
			else if( StartsWith( l, L"ATQ:" ) || StartsWith( l, L"ATQM:" ) ){
				e.atk = NumberAfter( l, L"ATQ:" );
				e.matk = NumberAfter( l, L"ATQM:" );
			}else if( StartsWith( l, L"DEF:" ) || StartsWith( l, L"Defesa:" ) ){
				e.def = NumberAfter( l, L":" );
			}else if( StartsWith( l, L"Peso:" ) ) e.weight = NumberAfter( l, L"Peso:" );
			else if( StartsWith( l, L"Nível da arma:" ) ) e.weaponLevel = NumberAfter( l, L":" );
			else if( StartsWith( l, L"Nível do equipamento:" ) || StartsWith( l, L"Nível da armadura:" ) ) e.armorLevel = NumberAfter( l, L":" );
			else if( StartsWith( l, L"Nível necessário:" ) ) e.requiredLevel = NumberAfter( l, L":" );
			else if( StartsWith( l, L"Equipa em:" ) || StartsWith( l, L"Posição:" ) ) {}
			else footer = false;
			if( !footer ) body.push_back( raw );
		}
		while( !body.empty() && StripColors( body.back() ).find_first_not_of( L"-_ " ) == std::wstring::npos ) body.pop_back();
		while( !e.type.empty() && e.type.front() == L' ' ) e.type.erase( 0, 1 );
		while( !e.classes.empty() && e.classes.front() == L' ' ) e.classes.erase( 0, 1 );
		e.lines = std::move( body );
		return e;
	}

	void Load(){
		DWORD t0 = GetTickCount();
		std::wstring dir = ExeDir() + L"System\\";
		const wchar_t* names[] = { L"Codex.lub", L"bRO.lub", L"itemInfo_EN.lub" };
		std::lock_guard<std::mutex> g( g_lock );
		for( const wchar_t* n : names ){
			Source s;
			if( !ReadFile( dir + n, s.text ) ) continue;
			if( s.text.size() > 4 && s.text[0] == 0x1B ){ // lub compilado: não dá para ler
				logger::Write( "AVISO: %ls compilado; descricoes desse arquivo indisponiveis", n );
				continue;
			}
			g_files.push_back( std::move( s ) );
			IndexFile( (int)g_files.size() - 1 );
		}
		g_ready = true;
		logger::Write( "Itens: %u descricoes indexadas em %lu ms", (unsigned)g_index.size(), GetTickCount() - t0 );
	}
}

void itemdb::StartLoading(){
	if( g_started.exchange( true ) ) return;
	std::thread( Load ).detach();
}

bool itemdb::Ready(){
	return g_ready;
}

bool itemdb::Get( uint32_t id, Entry& out ){
	if( !g_ready ) return false;
	std::lock_guard<std::mutex> g( g_lock );
	auto c = g_cache.find( id );
	if( c != g_cache.end() ){ out = c->second; return true; }
	auto i = g_index.find( id );
	if( i == g_index.end() ) return false;
	out = g_cache[id] = Parse( i->second.first, i->second.second );
	return true;
}

std::wstring itemdb::Name( uint32_t id ){
	Entry e;
	return Get( id, e ) ? e.name : std::wstring();
}
