#pragma once

#include <cstddef>

namespace logger {
	void Init();
	void Write( const char* fmt, ... );
	void HexDump( const char* label, const void* data, size_t len );
}
