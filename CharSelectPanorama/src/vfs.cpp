#include "vfs.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>

#include "config.hpp"
#include "miniz.h"

namespace {
	struct Entry {
		unsigned compSize, compAligned, realSize, offset;
		unsigned char flags;
	};

	bool g_tried = false;
	bool g_ok = false;
	HANDLE g_file = INVALID_HANDLE_VALUE;
	std::map<std::string, Entry> g_entries; // chave: nome em minúsculas, sem "data\charselect\"
	CRITICAL_SECTION g_lock;

	std::wstring ExeDir(){
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW( nullptr, exe, MAX_PATH );
		wchar_t* slash = wcsrchr( exe, L'\\' );
		if( slash ) slash[1] = 0;
		return exe;
	}

	bool ReadAt( unsigned long long pos, void* buf, unsigned n ){
		LARGE_INTEGER li;
		li.QuadPart = (LONGLONG)pos;
		DWORD got = 0;
		return SetFilePointerEx( g_file, li, nullptr, FILE_BEGIN ) && ReadFile( g_file, buf, n, &got, nullptr ) && got == n;
	}

	std::string Key( std::string s ){
		for( char& c : s ){
			if( c == '/' ) c = '\\';
			if( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
		}
		return s;
	}

	// GRF 0x200 (zlib), formato padrão do Ragnarok
	void OpenGrf(){
		g_tried = true;
		InitializeCriticalSection( &g_lock );
		std::wstring path = ExeDir() + L"char.grf";
		g_file = CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr );
		if( g_file == INVALID_HANDLE_VALUE ) return;
		unsigned char h[46];
		if( !ReadAt( 0, h, 46 ) ) return;
		unsigned tableOfs, seed, count, version;
		memcpy( &tableOfs, h + 30, 4 );
		memcpy( &seed, h + 34, 4 );
		memcpy( &count, h + 38, 4 );
		memcpy( &version, h + 42, 4 );
		if( version != 0x200 ) return;
		unsigned sizes[2];
		if( !ReadAt( 46ull + tableOfs, sizes, 8 ) ) return;
		std::vector<unsigned char> comp( sizes[0] ), table( sizes[1] );
		if( !ReadAt( 46ull + tableOfs + 8, comp.data(), sizes[0] ) ) return;
		mz_ulong len = sizes[1];
		if( uncompress( table.data(), &len, comp.data(), sizes[0] ) != MZ_OK ) return;
		const char prefix[] = "data\\charselect\\";
		size_t p = 0;
		while( p < len ){
			const char* name = reinterpret_cast<const char*>( &table[p] );
			size_t nl = strnlen( name, len - p );
			p += nl + 1;
			if( p + 17 > len ) break;
			Entry e;
			memcpy( &e.compSize, &table[p], 4 );
			memcpy( &e.compAligned, &table[p + 4], 4 );
			memcpy( &e.realSize, &table[p + 8], 4 );
			e.flags = table[p + 12];
			memcpy( &e.offset, &table[p + 13], 4 );
			p += 17;
			std::string k = Key( std::string( name, nl ) );
			if( ( e.flags & 1 ) && k.compare( 0, sizeof( prefix ) - 1, prefix ) == 0 ){
				g_entries[k.substr( sizeof( prefix ) - 1 )] = e;
			}
		}
		g_ok = !g_entries.empty();
	}

	// Caminho (absoluto em BaseDir ou relativo) -> relativo, com "\"
	std::wstring Relative( const std::wstring& path ){
		const std::wstring& base = config::BaseDir();
		if( path.size() >= base.size() && _wcsnicmp( path.c_str(), base.c_str(), base.size() ) == 0 ){
			return path.substr( base.size() );
		}
		return path;
	}

	std::string Narrow( const std::wstring& w ){
		std::string s;
		for( wchar_t c : w ) s += c < 128 ? (char)c : '?';
		return Key( s );
	}

	bool DiskRead( const std::wstring& path, std::vector<unsigned char>& out ){
		FILE* f = _wfopen( path.c_str(), L"rb" );
		if( f == nullptr ) return false;
		fseek( f, 0, SEEK_END );
		long n = ftell( f );
		fseek( f, 0, SEEK_SET );
		out.resize( n > 0 ? (size_t)n : 0 );
		bool ok = n >= 0 && fread( out.data(), 1, out.size(), f ) == out.size();
		fclose( f );
		return ok;
	}
}

bool vfs::Read( const std::wstring& path, std::vector<unsigned char>& out ){
	std::wstring rel = Relative( path );
	if( DiskRead( config::BaseDir() + rel, out ) ) return true;
	if( !g_tried ) OpenGrf();
	if( !g_ok ) return false;
	auto it = g_entries.find( Narrow( rel ) );
	if( it == g_entries.end() ) return false;
	const Entry& e = it->second;
	std::vector<unsigned char> comp( e.compAligned );
	EnterCriticalSection( &g_lock );
	bool ok = ReadAt( 46ull + e.offset, comp.data(), e.compAligned );
	LeaveCriticalSection( &g_lock );
	if( !ok ) return false;
	out.resize( e.realSize );
	mz_ulong len = e.realSize;
	return uncompress( out.data(), &len, comp.data(), e.compSize ) == MZ_OK && len == e.realSize;
}

bool vfs::Exists( const std::wstring& path ){
	std::wstring rel = Relative( path );
	if( GetFileAttributesW( ( config::BaseDir() + rel ).c_str() ) != INVALID_FILE_ATTRIBUTES ) return true;
	if( !g_tried ) OpenGrf();
	return g_ok && g_entries.count( Narrow( rel ) ) > 0;
}

std::vector<std::wstring> vfs::List( const std::wstring& folder ){
	std::wstring rel = Relative( folder );
	if( !rel.empty() && rel.back() != L'\\' ) rel += L'\\';
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW( ( config::BaseDir() + rel + L"*" ).c_str(), &fd );
	if( h != INVALID_HANDLE_VALUE ){
		do{
			if( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) out.push_back( fd.cFileName );
		}while( FindNextFileW( h, &fd ) );
		FindClose( h );
	}
	if( !out.empty() ) return out;
	if( !g_tried ) OpenGrf();
	std::string prefix = Narrow( rel );
	for( const auto& kv : g_entries ){
		if( kv.first.compare( 0, prefix.size(), prefix ) == 0 && kv.first.find( '\\', prefix.size() ) == std::string::npos ){
			std::string n = kv.first.substr( prefix.size() );
			out.push_back( std::wstring( n.begin(), n.end() ) );
		}
	}
	return out;
}

const std::wstring& vfs::WritableDir(){
	static std::wstring dir;
	if( dir.empty() ){
		std::wstring save = ExeDir() + L"savedata\\";
		CreateDirectoryW( save.c_str(), nullptr );
		dir = save + L"charselect\\";
		CreateDirectoryW( dir.c_str(), nullptr );
	}
	return dir;
}

bool vfs::GrfLoaded(){
	if( !g_tried ) OpenGrf();
	return g_ok;
}
