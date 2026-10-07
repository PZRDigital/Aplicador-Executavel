#include "log.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

#include "config.hpp"
#include "vfs.hpp"

namespace {
	FILE* g_file = nullptr;
	CRITICAL_SECTION g_lock;
}

void logger::Init(){
	InitializeCriticalSection( &g_lock );

	// Garante a pasta mesmo que o usuário só tenha copiado o d3d9.dll
	std::wstring path = vfs::WritableDir() + L"charselect.log";
	g_file = _wfopen( path.c_str(), L"w" );
}

void logger::Write( const char* fmt, ... ){
	if( g_file == nullptr ){
		return;
	}

	EnterCriticalSection( &g_lock );

	SYSTEMTIME st;
	GetLocalTime( &st );
	fprintf( g_file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds );

	va_list ap;
	va_start( ap, fmt );
	vfprintf( g_file, fmt, ap );
	va_end( ap );

	fputc( '\n', g_file );
	fflush( g_file );

	LeaveCriticalSection( &g_lock );
}

void logger::HexDump( const char* label, const void* data, size_t len ){
	const unsigned char* p = static_cast<const unsigned char*>( data );

	logger::Write( "%s (%u bytes @ %p)", label, (unsigned)len, data );

	char line[128];
	for( size_t i = 0; i < len; i += 16 ){
		int n = sprintf( line, "  +%03X:", (unsigned)i );
		for( size_t j = i; j < i + 16 && j < len; j++ ){
			n += sprintf( line + n, " %02X", p[j] );
		}
		logger::Write( "%s", line );
	}
}
