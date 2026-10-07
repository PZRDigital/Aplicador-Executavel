#include "game.hpp"

#include <winsock2.h>

#include <windows.h>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#include "chardata.hpp"
#include "config.hpp"
#include "log.hpp"

namespace {
	template <typename T> T Rd( const uint8_t* b, size_t o ){
		T v;
		memcpy( &v, b + o, sizeof( T ) );
		return v;
	}

	template <typename T> bool Peek( uintptr_t addr, T& out ){
		__try{
			out = *reinterpret_cast<const T*>( addr );
			return true;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return false;
		}
	}

	// ---------------------------------------------------------------- tabela de pacotes do cliente
	constexpr uintptr_t FN_ADD_PACKET = 0xA87960;
	int16_t g_len[0x10000] = {}; // 0 = desconhecido, -1 = variável
	volatile bool g_tableReady = false;

	using AddPacketFn = void( __thiscall* )( void* self, int id, int len, int minLen, int flag );
	AddPacketFn g_addPacketTramp = nullptr;

	void __fastcall HookAddPacket( void* self, void*, int id, int len, int minLen, int flag ){
		if( id > 0 && id < 0x10000 ){
			g_len[id] = (int16_t)( len < 0 ? -1 : len );
			g_tableReady = true;
		}
		g_addPacketTramp( self, id, len, minLen, flag );
	}

	// ---------------------------------------------------------------- equipamentos (pacotes do map-server)
	constexpr uint16_t ZC_INVENTORY_START = 0x0B08;
	constexpr uint16_t ZC_EQUIPMENT_ITEMLIST3 = 0x0B39;
	constexpr uint16_t ZC_ITEM_PICKUP_ACK = 0x0B41;
	constexpr uint16_t ZC_REQ_WEAR_EQUIP_ACK = 0x0999;
	constexpr uint16_t ZC_REQ_TAKEOFF_EQUIP_ACK = 0x099A;
	constexpr uint16_t ZC_DELETE_ITEM_FROM_BODY = 0x07FA;
	constexpr uint16_t ZC_ITEM_THROW_ACK = 0x00AF;
	constexpr uint16_t ZC_ACK_ITEMREFINING = 0x0188;
	constexpr size_t EQUIPITEM_INFO_SIZE = 68;

	std::mutex g_lock;
	std::map<int, game::Item> g_items; // índice -> equipamento do inventário
	volatile bool g_showEquip = false;  // ZC_CONFIG tipo 0 ("Exibir Equip")

	void ReadOptions( const uint8_t* b, game::Item& it ){
		for( int k = 0; k < 5; k++ ){
			it.options[k].id = Rd<int16_t>( b, k * 5 );
			it.options[k].value = Rd<int16_t>( b, k * 5 + 2 );
			it.options[k].param = b[k * 5 + 4];
		}
	}

	// EQUIPITEM_INFO (PACKETVER 20250604): index, ITID, type, location, WearState, slot[4],
	// HireExpireDate, bindOnEquipType, wItemSpriteNumber, option_count, option_data[5], refine, grade, Flag
	game::Item ParseEquipInfo( const uint8_t* b ){
		game::Item it;
		it.index = Rd<uint16_t>( b, 0 );
		it.id = Rd<uint32_t>( b, 2 );
		it.location = Rd<uint32_t>( b, 7 );
		it.wear = Rd<uint32_t>( b, 11 );
		for( int k = 0; k < 4; k++ ) it.cards[k] = Rd<uint32_t>( b, 15 + k * 4 );
		ReadOptions( b + 40, it );
		it.refine = b[65];
		it.grade = b[66];
		it.identified = ( b[67] & 1 ) != 0;
		it.favorite = ( b[67] & 4 ) != 0;
		it.type = b[6];
		it.count = 1;
		return it;
	}

	int g_capacity = 100;

	void TakeAway( int index, int count ){
		auto i = g_items.find( index );
		if( i == g_items.end() ) return;
		if( i->second.equip || count >= i->second.count ) g_items.erase( i );
		else i->second.count -= count;
	}

	std::map<int, game::Skill> g_skills;
	int g_skillPoints = 0;

	void StoreSkill( const uint8_t* b, bool withName ){
		game::Skill sk;
		sk.id = Rd<uint16_t>( b, 0 );
		sk.inf = Rd<int32_t>( b, 2 );
		sk.level = Rd<uint16_t>( b, 6 );
		sk.sp = Rd<uint16_t>( b, 8 );
		sk.upgradable = b[withName ? 36 : 12] != 0;
		g_skills[sk.id] = sk;
	}

	void Handle( uint16_t id, const uint8_t* b, size_t len ){
		std::lock_guard<std::mutex> g( g_lock );
		switch( id ){
			case ZC_INVENTORY_START:
				if( len >= 5 && b[4] == 0 ) g_items.clear();
				break;
			case 0x0B09: // ZC_INVENTORY_ITEMLIST_NORMAL: index, ITID, type, count, WearState, slot[4], HireExpireDate, Flag
				if( len >= 5 && b[4] == 0 ){
					for( size_t p = 5; p + 34 <= len; p += 34 ){
						game::Item it;
						it.index = Rd<uint16_t>( b, p );
						it.id = Rd<uint32_t>( b, p + 2 );
						it.type = b[p + 6];
						it.count = Rd<int16_t>( b, p + 7 );
						for( int k = 0; k < 4; k++ ) it.cards[k] = Rd<uint32_t>( b, p + 13 + k * 4 );
						it.identified = ( b[p + 33] & 1 ) != 0;
						it.favorite = ( b[p + 33] & 2 ) != 0;
						g_items[it.index] = it;
					}
				}
				break;
			case 0x01C8: // ZC_USE_ITEM_ACK: index, id, AID, amount restante, result
				if( len >= 15 && b[14] ){
					auto i = g_items.find( Rd<uint16_t>( b, 2 ) );
					if( i != g_items.end() ){
						int left = Rd<int16_t>( b, 12 );
						if( left <= 0 ) g_items.erase( i ); else i->second.count = left;
					}
				}
				break;
			case 0x0B32: // ZC_SKILLINFO_LIST: id, inf, level, sp, range2, upFlag, level2 (15 bytes)
				g_skills.clear();
				for( size_t p = 4; p + 15 <= len; p += 15 ) StoreSkill( b + p, false );
				break;
			case 0x010F: // versão antiga (com nome, 37 bytes)
				g_skills.clear();
				for( size_t p = 4; p + 37 <= len; p += 37 ) StoreSkill( b + p, true );
				break;
			case 0x0B31: if( len >= 17 ) StoreSkill( b + 2, false ); break; // ZC_ADD_SKILL
			case 0x0111: if( len >= 39 ) StoreSkill( b + 2, true ); break;
			case 0x010E: // ZC_SKILLINFO_UPDATE: id, level, sp, range2, upFlag
				if( len >= 11 ){
					game::Skill& sk = g_skills[Rd<uint16_t>( b, 2 )];
					sk.id = Rd<uint16_t>( b, 2 );
					sk.level = Rd<uint16_t>( b, 4 );
					sk.sp = Rd<uint16_t>( b, 6 );
					sk.upgradable = b[10] != 0;
				}
				break;
			case 0x0441: if( len >= 4 ) g_skills.erase( Rd<uint16_t>( b, 2 ) ); break; // ZC_SKILLINFO_DELETE
			case 0x00B0: // ZC_PAR_CHANGE: SP_SKILLPOINT = 12
				if( len >= 8 && Rd<uint16_t>( b, 2 ) == 12 ) g_skillPoints = Rd<int32_t>( b, 4 );
				break;
			case 0x0B18: // ZC_EXTEND_BODYITEM_SIZE
				if( len >= 4 ) g_capacity = 100 + Rd<uint16_t>( b, 2 );
				break;
			case ZC_EQUIPMENT_ITEMLIST3:
				if( len >= 5 && b[4] == 0 ){ // só o inventário (1 = carrinho, 2 = armazém)
					for( size_t p = 5; p + EQUIPITEM_INFO_SIZE <= len; p += EQUIPITEM_INFO_SIZE ){
						game::Item it = ParseEquipInfo( b + p );
						it.equip = true;
						g_items[it.index] = it;
					}
				}
				break;
			case ZC_ITEM_PICKUP_ACK:
				// Index, count, nameid, IsIdentified, IsDamaged, slot[4], location, type, result,
				// HireExpireDate, bindOnEquipType, option_data[5], favorite, look, refine, grade
				if( len >= 70 && b[33] == 0 ){
					int index = Rd<uint16_t>( b, 2 ), count = Rd<uint16_t>( b, 4 );
					auto old = g_items.find( index );
					if( old != g_items.end() && old->second.id == Rd<uint32_t>( b, 6 ) && !old->second.equip ){
						old->second.count += count;
						break;
					}
					game::Item it;
					it.index = index;
					it.id = Rd<uint32_t>( b, 6 );
					it.identified = b[10] != 0;
					for( int k = 0; k < 4; k++ ) it.cards[k] = Rd<uint32_t>( b, 12 + k * 4 );
					it.location = Rd<uint32_t>( b, 28 );
					it.type = b[32];
					it.equip = it.location != 0;
					it.count = it.equip ? 1 : count;
					ReadOptions( b + 40, it );
					it.favorite = b[65] != 0;
					it.refine = b[68];
					it.grade = b[69];
					g_items[it.index] = it;
				}
				break;
			case ZC_REQ_WEAR_EQUIP_ACK:
				if( len >= 11 && b[10] == 0 ){
					auto i = g_items.find( Rd<uint16_t>( b, 2 ) );
					if( i != g_items.end() ) i->second.wear = Rd<uint32_t>( b, 4 );
				}
				break;
			case ZC_REQ_TAKEOFF_EQUIP_ACK:
				if( len >= 9 && b[8] == 0 ){
					auto i = g_items.find( Rd<uint16_t>( b, 2 ) );
					if( i != g_items.end() ) i->second.wear = 0;
				}
				break;
			case ZC_DELETE_ITEM_FROM_BODY:
				if( len >= 8 ) TakeAway( Rd<uint16_t>( b, 4 ), Rd<int16_t>( b, 6 ) );
				break;
			case ZC_ITEM_THROW_ACK:
				if( len >= 6 ) TakeAway( Rd<uint16_t>( b, 2 ), Rd<uint16_t>( b, 4 ) );
				break;
			case 0x02D9: // ZC_CONFIG: tipo, valor
				if( len >= 10 && Rd<uint32_t>( b, 2 ) == 0 ) g_showEquip = Rd<uint32_t>( b, 6 ) != 0;
				break;
			case ZC_ACK_ITEMREFINING:
				if( len >= 8 && Rd<uint16_t>( b, 2 ) == 0 ){
					auto i = g_items.find( Rd<uint16_t>( b, 4 ) );
					if( i != g_items.end() ) i->second.refine = Rd<uint16_t>( b, 6 );
				}
				break;
		}
	}

	struct Stream {
		uint16_t lastId[8] = {}; int lastLen[8] = {}; int lastN = 0;
		std::string buf;
		bool first = true;
		bool dead = false;
	};
	std::unordered_map<uintptr_t, Stream> g_streams;
	std::mutex g_streamLock;

	// ---------------------------------------------------------------- janelas nativas
	constexpr uintptr_t G_WINDOW_MGR = 0x12F8268;
	constexpr uintptr_t OFS_MGR_BASICINFO = 0x1DC;
	constexpr uintptr_t OFS_MGR_ITEMCOLLECTION = 0x218;
	constexpr uintptr_t OFS_MGR_ITEMCOMPARE = 0x220;
	constexpr uintptr_t VT_ITEMCOMPARE = 0x1012D4C;
	constexpr uintptr_t VT_BASICINFO = 0x101E2DC;
	constexpr uintptr_t VT_ITEMCOLLECTION = 0x1012B9C;
	constexpr uintptr_t OFS_ITEMWND_INFO = 0x94; // ITEM_INFO dentro de UIItemCollectionWnd

	uintptr_t Window( uintptr_t ofs, uintptr_t vt ){
		uintptr_t w = 0, v = 0;
		if( !Peek( G_WINDOW_MGR + ofs, w ) || w == 0 || !Peek( w, v ) || v != vt ) return 0;
		return w;
	}

	bool WindowRect( uintptr_t w, client::WndRect& r ){
		return w != 0 && Peek( w + client::OFS_WND_X, r.x ) && Peek( w + client::OFS_WND_Y, r.y )
			&& Peek( w + client::OFS_WND_WIDTH, r.w ) && Peek( w + client::OFS_WND_HEIGHT, r.h ) && r.w > 0 && r.h > 0;
	}

	// Globais da sessão (Codex.exe 2025-06-04); conferidas em InstallEarly
	constexpr uintptr_t S_JOB = 0x15D4728;
	constexpr uintptr_t S_BASE_EXP = 0x15D4730, S_BASE_NEXT = 0x15D4738, S_JOB_EXP = 0x15D4740, S_JOB_NEXT = 0x15D4748;
	constexpr uintptr_t S_BASE_LEVEL = 0x15D4750, S_JOB_LEVEL = 0x15D4758;
	constexpr uintptr_t S_ZENY = 0x15D47F0, S_MAX_WEIGHT = 0x15D47FC, S_WEIGHT = 0x15D4800;
	constexpr uintptr_t S_HP = 0x15D8668, S_MAX_HP = 0x15D866C, S_SP = 0x15D8670, S_MAX_SP = 0x15D8674;
	constexpr uintptr_t S_NAME = 0x15DB2B8;
	bool g_sessionOk = false;
}

namespace {
	// connect: conexão nova no mesmo número de socket começa a leitura do zero
	constexpr uintptr_t IAT_CONNECT = 0xFA17F0;
	using ConnectFn = int( WSAAPI* )( uintptr_t, const void*, int );
	ConnectFn o_connect = nullptr;
	int WSAAPI Hook_connect( uintptr_t s, const void* addr, int len ){
		game::OnClose( s );
		if( config::Get().debug ) logger::Write( "connect: socket %u (leitura de pacotes reiniciada)", (unsigned)s );
		return o_connect( s, addr, len );
	}
}

void game::InstallEarly(){
	{
		uintptr_t* slot = reinterpret_cast<uintptr_t*>( IAT_CONNECT );
		HMODULE ws = GetModuleHandleW( L"ws2_32.dll" );
		FARPROC real = ws ? GetProcAddress( ws, "connect" ) : nullptr;
		if( real && *slot == reinterpret_cast<uintptr_t>( real ) ){
			o_connect = reinterpret_cast<ConnectFn>( real );
			DWORD old;
			VirtualProtect( slot, 4, PAGE_READWRITE, &old );
			*slot = reinterpret_cast<uintptr_t>( &Hook_connect );
			VirtualProtect( slot, 4, old, &old );
		}
	}
	// UIBasicInfoWnd::OnDraw lê a EXP da sessão: mov esi, [0x15D4738]
	const uint8_t sigSession[] = { 0x8B, 0x35, 0x38, 0x47, 0x5D, 0x01 };
	g_sessionOk = memcmp( reinterpret_cast<void*>( 0x959966 ), sigSession, sizeof( sigSession ) ) == 0;

	const uint8_t expect[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
	uint8_t* fn = reinterpret_cast<uint8_t*>( FN_ADD_PACKET );
	if( !g_sessionOk || memcmp( fn, expect, sizeof( expect ) ) != 0 ){
		logger::Write( "AVISO: cliente diferente do esperado; barra de status e janela de item desativadas" );
		g_sessionOk = false;
		return;
	}
	uint8_t* tramp = static_cast<uint8_t*>( VirtualAlloc( nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) );
	if( tramp == nullptr ) return;
	memcpy( tramp, fn, 10 );
	tramp[10] = 0xE9;
	*reinterpret_cast<int32_t*>( tramp + 11 ) = (int32_t)( ( fn + 10 ) - ( tramp + 15 ) );
	g_addPacketTramp = reinterpret_cast<AddPacketFn>( tramp );
	DWORD old;
	VirtualProtect( fn, 5, PAGE_EXECUTE_READWRITE, &old );
	fn[0] = 0xE9;
	*reinterpret_cast<int32_t*>( fn + 1 ) = (int32_t)( reinterpret_cast<uint8_t*>( &HookAddPacket ) - ( fn + 5 ) );
	VirtualProtect( fn, 5, old, &old );
	FlushInstructionCache( GetCurrentProcess(), fn, 5 );
}

void game::OnRecv( uintptr_t s, const char* data, int len ){
	if( !g_tableReady || len <= 0 ) return;
	std::lock_guard<std::mutex> g( g_streamLock );
	Stream& st = g_streams[s];
	if( st.dead ) return;
	st.buf.append( data, len );
	const uint8_t* b = reinterpret_cast<const uint8_t*>( st.buf.data() );
	size_t n = st.buf.size(), p = 0;
	while( n - p >= 2 ){
		uint16_t id = Rd<uint16_t>( b, p );
		int size = g_len[id];
		if( st.first ){
			st.first = false;
			if( size == 0 && n - p >= 4 ){ p += 4; continue; } // char-server: 4 bytes do AID antes do 1º pacote
		}
		if( size == 0 ){
			st.dead = true;
			logger::Write( "Pacotes: id %04X desconhecido; leitura desta conexao parada", id );
			for( int k = 0; k < 8; k++ ){
				int q = ( st.lastN + k ) % 8;
				if( st.lastLen[q] ) logger::Write( "  antes: %04X tam %d (tabela %d)", st.lastId[q], st.lastLen[q], g_len[st.lastId[q]] );
			}
			break;
		}
		if( size < 0 ){
			if( n - p < 4 ) break;
			size = Rd<uint16_t>( b, p + 2 );
			if( size < 4 ){
				st.dead = true;
				break;
			}
		}
		if( n - p < (size_t)size ) break;
		st.lastId[st.lastN % 8] = id; st.lastLen[st.lastN % 8] = size; st.lastN++;
		Handle( id, b + p, size );
		p += size;
	}
	st.buf.erase( 0, p );
}

void game::OnClose( uintptr_t s ){
	std::lock_guard<std::mutex> g( g_streamLock );
	g_streams.erase( s );
}

bool game::ReadStatus( Status& o ){
	if( !g_sessionOk ) return false;
	bool ok = Peek( S_HP, o.hp ) && Peek( S_MAX_HP, o.maxHp ) && Peek( S_SP, o.sp ) && Peek( S_MAX_SP, o.maxSp )
		&& Peek( S_BASE_LEVEL, o.baseLevel ) && Peek( S_JOB_LEVEL, o.jobLevel ) && Peek( S_JOB, o.job )
		&& Peek( S_BASE_EXP, o.baseExp ) && Peek( S_BASE_NEXT, o.baseNext ) && Peek( S_JOB_EXP, o.jobExp ) && Peek( S_JOB_NEXT, o.jobNext )
		&& Peek( S_WEIGHT, o.weight ) && Peek( S_MAX_WEIGHT, o.maxWeight ) && Peek( S_ZENY, o.zeny );
	if( !ok ) return false;
	char name[25] = {};
	for( int i = 0; i < 24; i++ ) if( !Peek( S_NAME + i, name[i] ) ) break;
	int n = MultiByteToWideChar( config::Get().codepage, 0, name, (int)strnlen( name, 24 ), nullptr, 0 );
	o.name.assign( n, L'\0' );
	if( n > 0 ) MultiByteToWideChar( config::Get().codepage, 0, name, (int)strnlen( name, 24 ), &o.name[0], n );
	return o.maxHp > 0;
}

bool game::EquippedAt( uint32_t mask, Item& out ){
	std::lock_guard<std::mutex> g( g_lock );
	for( const auto& kv : g_items ){
		if( kv.second.equip && ( kv.second.wear & mask ) ){
			out = kv.second;
			return true;
		}
	}
	return false;
}

bool game::InventoryItem( int index, Item& out ){
	std::lock_guard<std::mutex> g( g_lock );
	auto i = g_items.find( index );
	if( i == g_items.end() ) return false;
	out = i->second;
	return true;
}

bool game::BasicInfoRect( client::WndRect& r ){
	return g_sessionOk && WindowRect( Window( OFS_MGR_BASICINFO, VT_BASICINFO ), r );
}

bool game::ItemWindowRect( client::WndRect& r ){
	return g_sessionOk && WindowRect( Window( OFS_MGR_ITEMCOLLECTION, VT_ITEMCOLLECTION ), r );
}

namespace {
	// ITEM_INFO: +4 tipo, +8 índice, +0C local, +10 vestido em, +20 cartas[4], +30 id (std::string), +60 identificado, +64 refino
	bool ReadItemInfo( uintptr_t w, game::Item& o ){
		if( w == 0 ) return false;
		uintptr_t it = w + OFS_ITEMWND_INFO;
		int index = 0;
		uint32_t location = 0, wear = 0;
		if( !Peek( it + 0x08, index ) || !Peek( it + 0x0C, location ) || !Peek( it + 0x10, wear ) ) return false;
		for( int k = 0; k < 4; k++ ) if( !Peek( it + 0x20 + k * 4, o.cards[k] ) ) return false;
		char text[16] = {};
		uint32_t size = 0, cap = 0;
		if( !Peek( it + 0x40, size ) || !Peek( it + 0x44, cap ) || size == 0 || size > 12 ) return false;
		uintptr_t src = it + 0x30;
		if( cap >= 16 && !Peek( it + 0x30, src ) ) return false;
		for( uint32_t i = 0; i < size; i++ ){
			if( !Peek( src + i, text[i] ) || text[i] < '0' || text[i] > '9' ) return false;
		}
		uint8_t identified = 1, refine = 0;
		Peek( it + 0x60, identified );
		Peek( it + 0x64, refine );
		o.index = index;
		o.id = (uint32_t)strtoul( text, nullptr, 10 );
		o.location = location;
		o.wear = wear;
		o.refine = refine;
		o.identified = identified != 0;
		// opções aleatórias e grau: só na lista do inventário (pacotes)
		game::Item inv;
		if( index > 0 && game::InventoryItem( index, inv ) && inv.id == o.id ){
			for( int k = 0; k < 5; k++ ) o.options[k] = inv.options[k];
			o.grade = inv.grade;
		}
		return o.id != 0;
	}
}

bool game::ReadItemWindow( Item& o ){
	return g_sessionOk && ReadItemInfo( Window( OFS_MGR_ITEMCOLLECTION, VT_ITEMCOLLECTION ), o );
}

bool game::CompareWindowRect( client::WndRect& r ){
	return g_sessionOk && WindowRect( Window( OFS_MGR_ITEMCOMPARE, VT_ITEMCOMPARE ), r );
}

bool game::ReadCompareWindow( Item& o ){
	return g_sessionOk && ReadItemInfo( Window( OFS_MGR_ITEMCOMPARE, VT_ITEMCOMPARE ), o );
}


// ---------------------------------------------------------------- ações e dados extras
namespace {
	// Atributos (globais da sessão, mesma área de S_BASE_LEVEL)
	constexpr uintptr_t S_STATUS_POINT = 0x15D4754;
	constexpr uintptr_t S_BONUS = 0x15D476C;   // int[6] FOR..SOR (bônus)
	constexpr uintptr_t S_BASE = 0x15D4784;    // int[6]
	constexpr uintptr_t S_COST = 0x15D479C;    // int[6] pontos para subir
	constexpr uintptr_t S_AMOTION = 0x15D47B4; // VelAtq = 200 - amotion / 10
	constexpr uintptr_t S_ATK = 0x15D47B8, S_MDEF = 0x15D47BC, S_ATK2 = 0x15D47C0, S_DEF = 0x15D47C4, S_DEF2 = 0x15D47C8;
	constexpr uintptr_t S_MATK2 = 0x15D47D0, S_MATK = 0x15D47D4, S_MDEF2 = 0x15D47D8, S_HIT = 0x15D47DC, S_FLEE = 0x15D47E0;
	constexpr uintptr_t S_CRIT = 0x15D47E4, S_FLEE2 = 0x15D47E8;

	// CRagConnection::GetInstance() e SendPacket(tamanho, dados) do cliente (cifram o id)
	constexpr uintptr_t FN_CONN_INSTANCE = 0xBFC810;
	constexpr uintptr_t FN_SEND_PACKET = 0xBFC400;

	// Gerenciador de recursos: CBitmapRes já decodificado (largura +0x114, altura +0x118, pixels +0x11C)
	constexpr uintptr_t FN_RESMGR = 0xA786D0;
	constexpr uintptr_t FN_RESMGR_GET = 0xA75820;
}

bool game::ReadEquipStats( EquipStats& o ){
	if( !g_sessionOk ) return false;
	for( int k = 0; k < 6; k++ ){
		if( !Peek( S_BASE + k * 4, o.base[k] ) || !Peek( S_BONUS + k * 4, o.bonus[k] ) || !Peek( S_COST + k * 4, o.cost[k] ) ) return false;
	}
	int amotion = 0;
	Peek( S_STATUS_POINT, o.points );
	Peek( S_AMOTION, amotion );
	o.aspd = 200 - amotion / 10;
	Peek( S_ATK, o.atk ); Peek( S_ATK2, o.atk2 ); Peek( S_MATK, o.matk ); Peek( S_MATK2, o.matk2 );
	Peek( S_DEF, o.def ); Peek( S_DEF2, o.def2 ); Peek( S_MDEF, o.mdef ); Peek( S_MDEF2, o.mdef2 );
	Peek( S_HIT, o.hit ); Peek( S_FLEE, o.flee ); Peek( S_FLEE2, o.flee2 ); Peek( S_CRIT, o.crit );
	return true;
}

static bool CallSend( uint8_t* buf, int len ){
	__try{
		void* conn = reinterpret_cast<void*( __cdecl* )()>( FN_CONN_INSTANCE )();
		if( conn == nullptr ) return false;
		reinterpret_cast<bool( __thiscall* )( void*, int, void* )>( FN_SEND_PACKET )( conn, len, buf );
		return true;
	}__except( EXCEPTION_EXECUTE_HANDLER ){
		return false;
	}
}

bool game::SendPacket( const void* data, int len ){
	static int ok = -1;
	if( ok < 0 ){
		const uint8_t sig[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x6C, 0x00 }; // push ebp ... cmp byte [esi+6Ch], 0
		ok = g_sessionOk && memcmp( reinterpret_cast<void*>( FN_SEND_PACKET ), sig, sizeof( sig ) ) == 0 ? 1 : 0;
		if( !ok ) logger::Write( "AVISO: envio de pacotes do cliente diferente do esperado; ações desativadas" );
	}
	if( !ok ) return false;
	uint8_t buf[64]; // o cliente cifra o id no próprio buffer
	if( len > (int)sizeof( buf ) ) return false;
	memcpy( buf, data, len );
	return CallSend( buf, len );
}

void game::StatusUp( int stat ){
	uint8_t p[5] = { 0xBB, 0x00, (uint8_t)( 13 + stat ), 0x00, 1 }; // CZ_STATUS_CHANGE: SP_STR = 13
	SendPacket( p, sizeof( p ) );
}

void game::EquipItem( int index, uint32_t location ){
	uint8_t p[8] = { 0x98, 0x09 };
	memcpy( p + 2, &index, 2 );
	memcpy( p + 4, &location, 4 );
	SendPacket( p, sizeof( p ) );
}

void game::DropItem( int index, int count ){
	uint8_t p[6] = { 0xA2, 0x00 }; // CZ_ITEM_THROW: index, count
	memcpy( p + 2, &index, 2 );
	memcpy( p + 4, &count, 2 );
	SendPacket( p, sizeof( p ) );
}

void game::UnequipItem( int index ){
	uint8_t p[4] = { 0xAB, 0x00 };
	memcpy( p + 2, &index, 2 );
	SendPacket( p, sizeof( p ) );
}

static bool ReadBitmapRes( const char* path, const uint32_t*& src, int& w, int& h ){
	__try{
		void* mgr = reinterpret_cast<void*( __cdecl* )()>( FN_RESMGR )();
		if( mgr == nullptr ) return false;
		uintptr_t res = reinterpret_cast<uintptr_t( __thiscall* )( void*, const char* )>( FN_RESMGR_GET )( mgr, path );
		if( res == 0 ) return false;
		w = *reinterpret_cast<int*>( res + 0x114 );
		h = *reinterpret_cast<int*>( res + 0x118 );
		src = *reinterpret_cast<const uint32_t**>( res + 0x11C );
		return w > 0 && h > 0 && w <= 2048 && h <= 2048 && src != nullptr;
	}__except( EXCEPTION_EXECUTE_HANDLER ){
		return false;
	}
}

bool game::ClientBitmap( const std::string& path, std::vector<uint32_t>& px, int& w, int& h ){
	const uint32_t* src = nullptr;
	if( !ReadBitmapRes( path.c_str(), src, w, h ) ) return false;
	px.assign( src, src + w * h );
	for( uint32_t& c : px ){ // o cliente usa cor-chave: preto puro e magenta = transparente
		uint32_t rgb = c & 0xFFFFFF;
		c = ( rgb == 0 || rgb == 0xFF00FF ) ? 0 : ( rgb | 0xFF000000 );
	}
	return true;
}

bool game::ShowEquipFlag(){
	return g_showEquip;
}

bool game::EquipWindowRect( client::WndRect& r ){
	return g_sessionOk && WindowRect( Window( 0x1E0, 0x101232C ), r );
}

// ---------------------------------------------------------------- inventário
std::vector<game::Item> game::Inventory(){
	std::lock_guard<std::mutex> g( g_lock );
	std::vector<Item> out;
	for( const auto& kv : g_items ) if( kv.second.wear == 0 ) out.push_back( kv.second );
	return out;
}

int game::InventoryCapacity(){
	return g_capacity;
}

namespace {
	constexpr uintptr_t OFS_MGR_ITEMWND = 0x1D4;
	constexpr uintptr_t VT_ITEMWND = 0x101D3E0;
	constexpr uintptr_t OFS_INV_LIST = 0xC8, OFS_INV_COUNT = 0xCC, OFS_INV_TAB = 0xEC;
	constexpr int VT_IDX_SENDMSG = 37;

	static void CallSendMsg( uintptr_t w, int msg, int v1 ){
		__try{
			uintptr_t vt = *reinterpret_cast<uintptr_t*>( w );
			auto fn = reinterpret_cast<int( __thiscall* )( void*, void*, int, int, int, int, int )>( *reinterpret_cast<uintptr_t*>( vt + VT_IDX_SENDMSG * 4 ) );
			fn( reinterpret_cast<void*>( w ), nullptr, msg, v1, 0, 0, 0 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){
		}
	}
}

bool game::InventoryWindowRect( client::WndRect& r ){
	return g_sessionOk && WindowRect( Window( OFS_MGR_ITEMWND, VT_ITEMWND ), r );
}

int game::NativeInventoryTab(){
	uintptr_t w = Window( OFS_MGR_ITEMWND, VT_ITEMWND );
	int tab = -1;
	if( w ) Peek( w + OFS_INV_TAB, tab );
	return tab;
}

void game::SetNativeInventoryTab( int tab ){
	uintptr_t w = Window( OFS_MGR_ITEMWND, VT_ITEMWND );
	if( w ) CallSendMsg( w, 0x16, tab ); // o mesmo que o clique na aba: guarda e atualiza a lista
}

int game::NativeInventoryCell( int index ){
	// std::list<ITEM_INFO> da aba atual: +0xC8 = nó sentinela (próximo, anterior, item); índice no nó +0xC
	uintptr_t w = Window( OFS_MGR_ITEMWND, VT_ITEMWND );
	if( !w ) return -1;
	uint32_t head = 0, node = 0;
	if( !Peek( w + OFS_INV_LIST, head ) || !head || !Peek( head, node ) ) return -1;
	for( int i = 0; node && node != head && i < 400; i++ ){
		int idx = 0;
		if( Peek( node + 0xC, idx ) && idx == index ) return i;
		if( !Peek( node, node ) ) break;
	}
	return -1;
}


// ---------------------------------------------------------------- habilidades
std::map<int, game::Skill> game::Skills(){
	std::lock_guard<std::mutex> g( g_lock );
	return g_skills;
}

int game::SkillPoints(){
	return g_skillPoints;
}

int game::CurrentJob(){
	int job = 0;
	Peek( S_JOB, job );
	return job;
}

void game::UpgradeSkill( int skill ){
	uint8_t p[4] = { 0x12, 0x01 };
	memcpy( p + 2, &skill, 2 );
	SendPacket( p, sizeof( p ) );
}

bool game::SkillWindowRect( client::WndRect& r ){
	return g_sessionOk && WindowRect( Window( 0x2C4, 0x101F5E0 ), r );
}
