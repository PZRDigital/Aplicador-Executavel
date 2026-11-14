#include <set>
#include "game.hpp"

#include <winsock2.h>

#include <windows.h>
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

#include "chardata.hpp"
#include "config.hpp"
#include "vfs.hpp"
#include "log.hpp"
#include "ui.hpp"
#include "input.hpp"
#include "scene.hpp"

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

	template <typename T> bool Poke( uintptr_t addr, T value ){
		__try{
			*reinterpret_cast<T*>( addr ) = value;
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
	uint32_t g_aid = 0, g_option = 0; // conta do personagem e opções dele (ZC_STATE_CHANGE)

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

	std::map<int, game::Quest> g_quests;
	game::Pet g_pet;
	game::PetArena g_arena, g_arenaNext;

	// "#pdx:<chave>=<v1>,<v2>,..." (F_PetArena_ShowPetStatus no modo Pokédex)
	void HandleArenaSound( const char* name ){
		char k = name[5];
		int v[4] = {};
		sscanf_s( name + 7, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3] );
		game::PetArena& n = g_arenaNext;
		switch( k ){
			case 'x': g_arena = game::PetArena(); g_arena.valid = true; g_arena.job = v[0]; g_arena.tick = GetTickCount(); break;
			case 'c': n = game::PetArena(); n.job = v[0]; break;
			case 'a': n.level = v[0]; n.wins = v[1]; n.tierWins = v[2]; n.rebirths = v[3]; break;
			case 'm': n.mmr = v[0]; n.rank = v[1]; break;
			case 's': n.str = v[0]; n.agi = v[1]; n.vit = v[2]; break;
			case 'i': n.int_ = v[0]; n.dex = v[1]; n.luk = v[2]; break;
			case 'h': n.hp = v[0]; n.aspd = v[1]; break;
			case 'k': n.atkMin = v[0]; n.atkMax = v[1]; break;
			case 'g': n.matkMin = v[0]; n.matkMax = v[1]; break;
			case 't': n.hit = v[0]; n.flee = v[1]; break;
			case 'd': n.def = v[0]; n.mdef = v[1]; break;
			case 'e':
				if( n.job == v[0] ){ n.valid = n.accepted = true; n.tick = GetTickCount(); g_arena = n; }
				break;
		}
	}

	game::QuestObjective* FindHunt( uint32_t hunt ){
		auto q = g_quests.find( (int)( hunt / 1000 ) );
		if( q == g_quests.end() ) return nullptr;
		for( game::QuestObjective& o : q->second.objectives ) if( o.hunt == hunt ) return &o;
		return nullptr;
	}

	// fala do NPC: nome (o primeiro "[...]" da conversa) para a plaqueta e o retrato (ui::PaintSayWindow)
	std::mutex g_npcLock;
	std::wstring g_npcName;
	uint32_t g_npcId = 0;
	bool g_npcFresh = true; // próxima fala começa conversa nova (depois do fechar)
	std::string g_npcCutin; // cutin mandado pelo script (ZC_SHOW_IMAGE2): usado como retrato
	volatile int g_npcWait = 0; // botão pedido pelo servidor: 0 nenhum, 1 seguir (0x00B5), 2 fechar (0x00B6)

	void NpcSay( uint32_t npc, const char* text, size_t n ){
		std::string t( text, strnlen( text, n ) );
		std::lock_guard<std::mutex> g( g_npcLock );
		if( npc != g_npcId ){ g_npcId = npc; g_npcFresh = true; }
		size_t a = t.find( '[' ), e = a == std::string::npos ? a : t.find( ']', a );
		if( !g_npcFresh || a == std::string::npos || e == std::string::npos || a > 8 ) return; // só a 1ª fala da conversa
		std::string name = t.substr( a + 1, e - a - 1 );
		std::string clean; // tira os códigos de cor ^RRGGBB
		for( size_t i = 0; i < name.size(); i++ ){
			if( name[i] == '^' && i + 6 < name.size() + 0 && isxdigit( (unsigned char)name[i + 1] ) ){ i += 6; continue; }
			clean += name[i];
		}
		while( !clean.empty() && clean.front() == ' ' ) clean.erase( 0, 1 );
		while( !clean.empty() && clean.back() == ' ' ) clean.pop_back();
		int wn = MultiByteToWideChar( 1252, 0, clean.data(), (int)clean.size(), nullptr, 0 );
		std::wstring w( wn, L'\0' );
		if( wn ) MultiByteToWideChar( 1252, 0, clean.data(), (int)clean.size(), &w[0], wn );
		g_npcName = w;
		g_npcFresh = false;
	}

	void Handle( uint16_t id, const uint8_t* b, size_t len ){
		if( id == 0x00B4 && len > 8 ) NpcSay( Rd<uint32_t>( b, 4 ), reinterpret_cast<const char*>( b + 8 ), len - 8 ); // ZC_SAY_DIALOG
		if( id == 0x00B6 && len >= 6 ){ std::lock_guard<std::mutex> g( g_npcLock ); g_npcFresh = true; g_npcWait = 2; } // ZC_CLOSE_DIALOG
		if( id == 0x00B5 ) g_npcWait = 1; // ZC_WAIT_DIALOG: botão seguir
		if( id == 0x00B7 || id == 0x0142 || id == 0x01D4 ) g_npcWait = 0; // menu / número / texto: sem botão na faixa
		if( id == 0x01B3 && len >= 67 ){ // ZC_SHOW_IMAGE2: imagem[64], posição (255 = tirar): vira o retrato da conversa
			std::lock_guard<std::mutex> g( g_npcLock );
			if( b[66] == 255 ) g_npcCutin.clear();
			else g_npcCutin.assign( reinterpret_cast<const char*>( b + 2 ), strnlen( reinterpret_cast<const char*>( b + 2 ), 64 ) );
		}
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
						it.wear = Rd<uint32_t>( b, p + 9 ); // munição equipada (0x8000)
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
			case 0x013C: // ZC_EQUIP_ARROW: índice da munição equipada (0 = nenhuma)
				if( len >= 4 ){
					int idx = Rd<uint16_t>( b, 2 );
					for( auto& kv : g_items ) if( !kv.second.equip ) kv.second.wear = kv.first == idx ? 0x8000 : ( kv.second.wear & ~0x8000u );
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
			case 0x09F8: // ZC_ALL_QUEST_LIST3: qtd; { id, estado, tempo, prazo, nobj; { caça, raça, mob, nvmin, nvmax, atual, total, nome[24] } }
				if( len >= 8 ){
					g_quests.clear();
					uint32_t n = Rd<uint32_t>( b, 4 );
					size_t p = 8;
					for( uint32_t i = 0; i < n && p + 15 <= len; i++ ){
						game::Quest q;
						q.id = (int)Rd<uint32_t>( b, p );
						q.state = b[p + 4];
						q.end = Rd<uint32_t>( b, p + 9 );
						int objs = Rd<uint16_t>( b, p + 13 );
						p += 15;
						for( int k = 0; k < objs && p + 44 <= len; k++, p += 44 ){
							game::QuestObjective o;
							o.hunt = Rd<uint32_t>( b, p );
							o.mob = (int)Rd<uint32_t>( b, p + 8 );
							o.count = Rd<uint16_t>( b, p + 16 );
							o.total = Rd<uint16_t>( b, p + 18 );
							o.name.assign( reinterpret_cast<const char*>( b + p + 20 ), strnlen( reinterpret_cast<const char*>( b + p + 20 ), 24 ) );
							q.objectives.push_back( o );
						}
						g_quests[q.id] = q;
					}
				}
				break;
			case 0x09F9: // ZC_ADD_QUEST_EX: id, estado, tempo, prazo, nobj; { caça, raça, mob, nvmin, nvmax, atual, nome[24] }
				if( len >= 17 ){
					game::Quest q;
					q.id = (int)Rd<uint32_t>( b, 2 );
					q.state = b[6];
					q.end = Rd<uint32_t>( b, 11 );
					int objs = Rd<uint16_t>( b, 15 );
					for( size_t p = 17, k = 0; (int)k < objs && p + 42 <= len; k++, p += 42 ){
						game::QuestObjective o;
						o.hunt = Rd<uint32_t>( b, p );
						o.mob = (int)Rd<uint32_t>( b, p + 8 );
						o.count = Rd<uint16_t>( b, p + 16 );
						o.name.assign( reinterpret_cast<const char*>( b + p + 18 ), strnlen( reinterpret_cast<const char*>( b + p + 18 ), 24 ) );
						q.objectives.push_back( o );
					}
					g_quests[q.id] = q;
				}
				break;
			case 0x08FE: // ZC_HUNTING_QUEST_INFO: { caça, mob, total, atual }
				for( size_t p = 4; p + 12 <= len; p += 12 ){
					if( game::QuestObjective* o = FindHunt( Rd<uint32_t>( b, p ) ) ){
						o->total = Rd<uint16_t>( b, p + 8 );
						o->count = Rd<uint16_t>( b, p + 10 );
					}
				}
				break;
			case 0x09FA: // ZC_UPDATE_MISSION_HUNT_EX: n; { quest, caça, total, atual }
				for( size_t p = 6; p + 12 <= len; p += 12 ){
					if( game::QuestObjective* o = FindHunt( Rd<uint32_t>( b, p + 4 ) ) ){
						o->total = Rd<uint16_t>( b, p + 8 );
						o->count = Rd<uint16_t>( b, p + 10 );
					}
				}
				break;
			case 0x02B4: // ZC_DEL_QUEST
				if( len >= 6 ) g_quests.erase( (int)Rd<uint32_t>( b, 2 ) );
				break;
			case 0x02B7: // ZC_ACTIVE_QUEST: id, ativa
				if( len >= 7 ){
					auto q = g_quests.find( (int)Rd<uint32_t>( b, 2 ) );
					if( q != g_quests.end() ) q->second.state = b[6] ? 1 : 0;
				}
				break;
			case 0x01A2: // ZC_PROPERTY_PET: nome[24], renomeado, nível, fome, intimidade, acessório, classe
				if( len >= 35 ){
					g_pet.valid = true;
					g_pet.name.assign( reinterpret_cast<const char*>( b + 2 ), strnlen( reinterpret_cast<const char*>( b + 2 ), 24 ) );
					g_pet.renamed = b[26] != 0;
					g_pet.level = Rd<int16_t>( b, 27 );
					g_pet.hunger = Rd<int16_t>( b, 29 );
					g_pet.intimacy = Rd<int16_t>( b, 31 );
					g_pet.accessory = Rd<uint16_t>( b, 33 );
					g_pet.job = len >= 37 ? Rd<uint16_t>( b, 35 ) : 0;
				}
				break;
			case 0x01D3: // ZC_SOUND: nome[24], ação, ...; "#pdx:" = dados da Arena de Pets
				if( len >= 26 && memcmp( b + 2, "#pdx:", 5 ) == 0 && b[8] == '=' ){
					char name[25] = {};
					memcpy( name, b + 2, 24 );
					HandleArenaSound( name );
				}
				break;
			case 0x01A4: // ZC_CHANGESTATE_PET: tipo, GID, valor (1 intimidade, 2 fome, 3 acessório)
				if( len >= 11 ){
					int v = Rd<int32_t>( b, 7 );
					if( b[2] == 1 ) g_pet.intimacy = v;
					else if( b[2] == 2 ) g_pet.hunger = v;
					else if( b[2] == 3 ) g_pet.accessory = v;
				}
				break;
			case 0x0283: // ZC_AID: conta do próprio personagem (ao entrar no mapa)
				if( len >= 6 ){ g_aid = Rd<uint32_t>( b, 2 ); g_option = 0; }
				break;
			case 0x0196: case 0x043F: case 0x0983: // ZC_MSG_STATE_CHANGE(2/3): ícone, AID, liga; no login a montaria
				// e o falcão do próprio personagem só vêm por aqui (EFST_RIDING 27, EFST_FALCON 28)
				if( len >= 9 && g_aid != 0 && Rd<uint32_t>( b, 4 ) == g_aid ){
					uint16_t icon = Rd<uint16_t>( b, 2 );
					uint32_t bit = icon == 27 ? game::OPT_RIDING : icon == 28 ? game::OPT_FALCON : 0;
					if( bit ) g_option = b[8] ? ( g_option | bit ) : ( g_option & ~bit );
				}
				break;
			case 0x0229: // ZC_STATE_CHANGE: AID, bodyState, healthState, effectState (falcão 0x10, montaria 0x20...)
				if( len >= 15 && g_aid != 0 && Rd<uint32_t>( b, 2 ) == g_aid ) g_option = Rd<uint32_t>( b, 10 );
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

	// janela de equipamentos (UIEquipWnd) escolhida por game::SelectEquipWnd: 0 = a do personagem (mgr+0x1E0),
	// 1 = a de outro jogador (mgr+0x3FC, +0x94 = 1). As duas podem estar abertas ao mesmo tempo
	int g_equipSel = 0;
	uintptr_t EquipWnd(){
		return Window( g_equipSel ? 0x3FC : 0x1E0, 0x101232C );
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
		if( ( kv.second.equip || ( kv.second.wear & 0x8000 ) ) && ( kv.second.wear & mask ) ){ // munição vem da lista dos empilháveis
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

uint32_t game::PlayerOption(){
	std::lock_guard<std::mutex> g( g_lock );
	return g_option;
}

bool game::HasCart(){
	int cart = 0; // o mesmo teste do botão de carrinho da janela de equipamentos
	return g_sessionOk && Peek( 0x15D8D1C, cart ) && cart > 0;
}

void game::RemoveOption(){
	uint8_t p[3] = { 0x35, 0x0B, 0 }; // CZ_UNINSTALLATION: tira falcão, montaria, dragão, mado...
	SendPacket( p, sizeof( p ) );
}

void game::UnequipItem( int index ){
	uint8_t p[4] = { 0xAB, 0x00 };
	memcpy( p + 2, &index, 2 );
	SendPacket( p, sizeof( p ) );
}

// Aviso "Resource File Loading fail" do cliente (0xA753D0, __thiscall com duas std::string por valor, ret 0x30):
// silenciado enquanto a interface procura um arquivo que pode não existir (ícone ou ilustração de item custom)
namespace {
	constexpr uintptr_t FN_RES_FAIL = 0xA753D0;
	struct QuietResFail {
		uint8_t saved[3] = {};
		bool on = false;
		QuietResFail(){
			DWORD old;
			uint8_t* p = reinterpret_cast<uint8_t*>( FN_RES_FAIL );
			if( p[0] != 0x55 || !VirtualProtect( p, 3, PAGE_EXECUTE_READWRITE, &old ) ) return;
			memcpy( saved, p, 3 );
			p[0] = 0xC2; p[1] = 0x30; p[2] = 0x00; // ret 0x30
			VirtualProtect( p, 3, old, &old );
			on = true;
		}
		~QuietResFail(){
			if( !on ) return;
			DWORD old;
			uint8_t* p = reinterpret_cast<uint8_t*>( FN_RES_FAIL );
			VirtualProtect( p, 3, PAGE_EXECUTE_READWRITE, &old );
			memcpy( p, saved, 3 );
			VirtualProtect( p, 3, old, &old );
		}
	};
}

static bool ReadBitmapResRaw( const char* path, const uint32_t*& src, int& w, int& h );
static bool ReadBitmapRes( const char* path, const uint32_t*& src, int& w, int& h ){
	QuietResFail quiet;
	return ReadBitmapResRaw( path, src, w, h );
}

static bool ReadBitmapResRaw( const char* path, const uint32_t*& src, int& w, int& h ){
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

static uintptr_t BitmapResRaw( const char* path ){
	__try{
		void* mgr = reinterpret_cast<void*( __cdecl* )()>( FN_RESMGR )();
		return mgr ? reinterpret_cast<uintptr_t( __thiscall* )( void*, const char* )>( FN_RESMGR_GET )( mgr, path ) : 0;
	}__except( EXCEPTION_EXECUTE_HANDLER ){
		return 0;
	}
}

static uintptr_t BitmapResQuiet( const char* path ){
	QuietResFail quiet;
	return BitmapResRaw( path );
}

namespace {
	// o gerenciador de recursos pode descartar um bitmap (troca de mapa, limpeza de cache) e reaproveitar a memória
	// para outro: o ponteiro guardado só vale dentro do quadro
	std::map<std::string, uintptr_t> g_bmpCache;
}

void game::ClearBitmapCache(){
	g_bmpCache.clear();
}

uintptr_t game::ClientBitmapRes( const std::string& path ){
	std::map<std::string, uintptr_t>& s_cache = g_bmpCache;
	auto it = s_cache.find( path );
	if( it != s_cache.end() ) return it->second;
	uintptr_t res = BitmapResQuiet( path.c_str() );
	s_cache[path] = res;
	return res;
}

void game::SelectEquipWndPtr( uintptr_t w ){
	g_equipSel = w != 0 && w == Window( 0x3FC, 0x101232C ) ? 1 : 0;
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
	return g_sessionOk && WindowRect( EquipWnd(), r );
}

// ---------------------------------------------------------------- menu de ícones
namespace {
	constexpr uintptr_t FN_GET_WINDOW = 0xA30000; // UIWindowMgr::GetWindow(id)
	constexpr int WND_MENUICON = 0x133;
	constexpr uintptr_t VT_MENUICON = 0x10082A0;
	constexpr uintptr_t G_MENU_OPEN = 0x15DAFDC;   // painel aberto (lido pelo OnDraw do menu)
	constexpr uintptr_t OFS_MENU_NEW = 0xA8, OFS_MENU_BTN_OPEN = 0xAC, OFS_MENU_BTN_NEW = 0xB0, OFS_MENU_BTN_CLOSE = 0xB4;
	constexpr uintptr_t OFS_MENU_ICONS = 0x94;     // std::map<int, UIMenuIcon*>
	constexpr uintptr_t OFS_BUTTON_IMAGE = 0xBC;   // std::string da imagem normal do botão
	uintptr_t g_menuWnd = 0;                       // última janela lida (para o retângulo no meio do quadro)

	// UIWindowMgr::GetWindow(id), conferindo a classe pela vtable
	uintptr_t WindowById( int id, uintptr_t expectVt ){
		static int sigOk = -1;
		if( sigOk < 0 ){
			const uint8_t sig[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
			sigOk = memcmp( reinterpret_cast<void*>( FN_GET_WINDOW ), sig, sizeof( sig ) ) == 0;
		}
		if( !sigOk ) return 0;
		uintptr_t w = 0, vt = 0;
		__try{
			w = reinterpret_cast<uintptr_t( __thiscall* )( void*, int )>( FN_GET_WINDOW )( reinterpret_cast<void*>( G_WINDOW_MGR ), id );
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return 0;
		}
		return w != 0 && Peek( w, vt ) && vt == expectVt ? w : 0;
	}

	uintptr_t MenuWindow(){
		return WindowById( WND_MENUICON, VT_MENUICON );
	}

	bool ReadStdString( uintptr_t a, std::string& out ){
		uint32_t size = 0, cap = 0;
		if( !Peek( a + 0x10, size ) || !Peek( a + 0x14, cap ) || size > 260 ) return false;
		uintptr_t data = a;
		if( cap >= 16 && !Peek( a, data ) ) return false;
		out.assign( size, '\0' );
		for( uint32_t i = 0; i < size; i++ ) if( !Peek( data + i, out[i] ) ) return false;
		return true;
	}

	bool ChildRect( uintptr_t child, const client::WndRect& parent, client::WndRect& r ){
		if( child == 0 || !WindowRect( child, r ) ) return false;
		r.x += parent.x;
		r.y += parent.y;
		return true;
	}

	// Árvore do std::map (MSVC): nó = esquerda, pai, direita, cor, é-nulo(+0xD), chave(+0x10), valor(+0x14)
	void MenuIcons( uintptr_t node, const client::WndRect& parent, std::vector<game::MenuButton>& out, int depth ){
		uint8_t nil = 1;
		if( node == 0 || depth > 32 || out.size() > 64 || !Peek( node + 0xD, nil ) || nil ) return;
		uintptr_t left = 0, right = 0, button = 0;
		Peek( node, left );
		Peek( node + 8, right );
		Peek( node + 0x14, button );
		MenuIcons( left, parent, out, depth + 1 );
		game::MenuButton b;
		b.ptr = button;
		std::string image;
		if( ChildRect( button, parent, b.rect ) && ReadStdString( button + OFS_BUTTON_IMAGE, image ) ){
			size_t s = image.rfind( "bt_" ), e = image.rfind( ".bmp" );
			if( s != std::string::npos && e != std::string::npos && e > s + 3 ){
				b.name = image.substr( s + 3, e - s - 3 );
				out.push_back( b );
			}
		}
		MenuIcons( right, parent, out, depth + 1 );
	}
}

std::string game::CurrentMap(){
	constexpr uintptr_t S_MAP = 0x15D470C; // char[16] da sessão (0x44 antes do nível de base, como no cliente Zero)
	char name[17] = {};
	if( !g_sessionOk ) return {};
	for( int i = 0; i < 16; i++ ) if( !Peek( S_MAP + i, name[i] ) ) return {};
	std::string s( name, strnlen( name, 16 ) );
	size_t dot = s.find( '.' );
	return dot == std::string::npos ? s : s.substr( 0, dot );
}

bool game::ReadMenu( MenuState& m ){
	uintptr_t w = g_sessionOk ? MenuWindow() : 0;
	g_menuWnd = w;
	if( w == 0 || !WindowRect( w, m.rect ) ) return false;
	uint8_t open = 0;
	uint32_t isNew = 0;
	Peek( G_MENU_OPEN, open );
	Peek( w + OFS_MENU_NEW, isNew );
	m.open = open != 0;
	Peek( w + OFS_MENU_BTN_OPEN, m.toggles[0] );
	Peek( w + OFS_MENU_BTN_NEW, m.toggles[1] );
	Peek( w + OFS_MENU_BTN_CLOSE, m.toggles[2] );
	uintptr_t btn = 0;
	Peek( w + ( m.open ? OFS_MENU_BTN_CLOSE : isNew ? OFS_MENU_BTN_NEW : OFS_MENU_BTN_OPEN ), btn );
	if( !ChildRect( btn, m.rect, m.toggle ) ) return false;
	m.icons.clear();
	uintptr_t head = 0, root = 0;
	if( m.open && Peek( w + OFS_MENU_ICONS, head ) && head != 0 && Peek( head + 4, root ) ) MenuIcons( root, m.rect, m.icons, 0 );
	return true;
}

std::vector<game::Quest> game::Quests(){
	std::lock_guard<std::mutex> g( g_lock );
	std::vector<Quest> out;
	for( const auto& kv : g_quests ) out.push_back( kv.second );
	return out;
}

void game::SetQuestActive( int id, bool active ){
	uint8_t p[7] = { 0xB6, 0x02 };
	memcpy( p + 2, &id, 4 );
	p[6] = active ? 1 : 0;
	SendPacket( p, sizeof( p ) );
}

namespace {
	constexpr uintptr_t VT_QUEST = 0x1029D30;     // CUIRenewQuestUI (Alt+U; não fica no mapa por id do GetWindow)
	constexpr uintptr_t OFS_CHILDREN = 0x50;      // std::list<UIWindow*> dos filhos
	constexpr uintptr_t OFS_MGR_WINDOWS = 0x17C;  // std::list<UIWindow*> das janelas abertas (UIWindowMgr::AddWindow 0xA15660)

	// Janela aberta da classe (vtable) na lista do gerenciador
	uintptr_t OpenWindow( uintptr_t vt ){
		uintptr_t head = 0, node = 0;
		if( !Peek( G_WINDOW_MGR + OFS_MGR_WINDOWS, head ) || head == 0 || !Peek( head, node ) ) return 0;
		for( int i = 0; i < 512 && node != 0 && node != head; i++ ){
			uintptr_t w = 0, v = 0;
			if( Peek( node + 8, w ) && w != 0 && Peek( w, v ) && v == vt ) return w;
			if( !Peek( node, node ) ) break;
		}
		return 0;
	}

	constexpr uintptr_t OFS_WND_SHOW = 0x28;      // UIWindow::SetShow (vtable idx 14) grava aqui
	constexpr uintptr_t VT_TOOLTIP = 0x10094CC;   // janela de dica (tooltip) que acompanha o mouse
	constexpr uintptr_t VT_STATUSWND = 0x1012AC4; // UIStatusWnd (Alt+A)

	// Janelas visíveis na frente da janela da classe: o gerenciador desenha a lista do começo para o
	// fim, põe no fim a que recebe o foco (0xA15408) e testa o clique do fim para o começo (0xA1BAF0)
	void WindowsAbove( uintptr_t vt, std::vector<client::WndRect>& out, uintptr_t ptr = 0 ){
		out.clear();
		uintptr_t head = 0, node = 0;
		if( !Peek( G_WINDOW_MGR + OFS_MGR_WINDOWS, head ) || head == 0 || !Peek( head, node ) ) return;
		bool found = false;
		std::string sig;
		for( int i = 0; i < 512 && node != 0 && node != head; i++ ){
			uintptr_t w = 0, v = 0;
			if( Peek( node + 8, w ) && w != 0 && Peek( w, v ) ){
				int show = 0;
				client::WndRect r;
				if( !found ) found = ptr ? w == ptr : v == vt;
				// na janela de atributos a dica nativa some (HideTooltip) e a nossa fica no lugar: não cobre
				else if( vt == VT_STATUSWND && v == VT_TOOLTIP ){}
				// tooltip (segue o mouse) não conta: em cima de uma janela nossa ele é da nativa escondida embaixo
				// (ex.: "Weight 99%" da barra de status antiga) e some com ela
				// com a substituição a dica é da própria janela embaixo do mouse (item, ícone, botão): conta como da frente
				else if( Peek( w + OFS_WND_SHOW, show ) && show != 0 && WindowRect( w, r ) ){
					out.push_back( r );
					char b[64];
					sprintf( b, " %08X(%d,%d %dx%d)", (unsigned)v, r.x, r.y, r.w, r.h );
					sig += b;
				}
			}
			if( !Peek( node, node ) ) break;
		}
		static std::map<uintptr_t, std::string> s_last;
		if( config::Get().debug && s_last[vt] != sig ){
			s_last[vt] = sig;
			logger::Write( "janelas na frente de %08X:%s", (unsigned)vt, sig.empty() ? " nenhuma" : sig.c_str() );
		}
	}
}

void game::QuestCovers( std::vector<client::WndRect>& out ){
	if( g_sessionOk ) WindowsAbove( VT_QUEST, out );
	else out.clear();
}

bool game::QuestWindowRect( client::WndRect& r ){
	if( !g_sessionOk ) return false;
	uintptr_t w = OpenWindow( VT_QUEST );
	if( w == 0 || !WindowRect( w, r ) ) return false;
	return true;
}

bool game::QuestCloseRect( client::WndRect& r ){
	uintptr_t w = g_sessionOk ? OpenWindow( VT_QUEST ) : 0;
	client::WndRect wr;
	if( w == 0 || !WindowRect( w, wr ) ) return false;
	uintptr_t head = 0, node = 0;
	if( !Peek( w + OFS_CHILDREN, head ) || head == 0 || !Peek( head, node ) ) return false;
	for( int i = 0; i < 64 && node != 0 && node != head; i++ ){
		uintptr_t child = 0;
		std::string image;
		if( Peek( node + 8, child ) && ReadStdString( child + OFS_BUTTON_IMAGE, image ) && image.find( "btn_close" ) != std::string::npos
			&& ChildRect( child, wr, r ) ) return true;
		if( !Peek( node, node ) ) break;
	}
	return false;
}

game::Pet game::CurrentPet(){
	std::lock_guard<std::mutex> g( g_lock );
	return g_pet;
}

void game::PetCommand( int command ){
	uint8_t p[3] = { 0xA1, 0x01, (uint8_t)command };
	SendPacket( p, sizeof( p ) );
}

game::PetArena game::CurrentPetArena(){
	std::lock_guard<std::mutex> g( g_lock );
	return g_arena;
}

void game::RequestPetArena(){
	// CZ_REQUEST_CHAT: "<nome> : @pokedex" (comando do servidor; não aparece no chat)
	char name[25] = {};
	for( int i = 0; i < 24; i++ ) if( !Peek( S_NAME + i, name[i] ) ) break;
	if( !name[0] ) return;
	std::string msg = std::string( name ) + " : @pokedex";
	std::vector<uint8_t> p( 4 + msg.size() + 1, 0 );
	p[0] = 0xF3; p[1] = 0x00;
	p[2] = (uint8_t)( p.size() & 0xFF ); p[3] = (uint8_t)( p.size() >> 8 );
	memcpy( p.data() + 4, msg.data(), msg.size() );
	SendPacket( p.data(), (int)p.size() );
}

void game::RenamePet( const std::string& name ){
	uint8_t p[26] = { 0xA5, 0x01 };
	memcpy( p + 2, name.data(), std::min<size_t>( name.size(), 23 ) );
	SendPacket( p, sizeof( p ) );
}

namespace {
	constexpr uintptr_t VT_PETINFO = 0x10109F0; // UIPetInfoWnd

	bool CloseButtonOf( uintptr_t w, client::WndRect& r ){
		client::WndRect wr;
		if( w == 0 || !WindowRect( w, wr ) ) return false;
		uintptr_t head = 0, node = 0;
		if( !Peek( w + OFS_CHILDREN, head ) || head == 0 || !Peek( head, node ) ) return false;
		for( int i = 0; i < 64 && node != 0 && node != head; i++ ){
			uintptr_t child = 0;
			std::string image;
			if( Peek( node + 8, child ) && ReadStdString( child + OFS_BUTTON_IMAGE, image ) && image.find( "close" ) != std::string::npos
				&& ChildRect( child, wr, r ) ) return true;
			if( !Peek( node, node ) ) break;
		}
		return false;
	}
}

// Janela de equipamentos nativa: o ícone e o nome da munição são desenhados no centro, por cima do personagem
// (que a janela Personagem copia). O desenho é pulado: "jbe" -> "jmp" depois do teste do item (0x8AFCBF).
void game::HideNativeAmmo(){
	static bool done = false;
	if( done ) return;
	done = true;
	uint8_t* p = reinterpret_cast<uint8_t*>( 0x8AFCB8 );
	const uint8_t sig[] = { 0x83, 0xBD, 0xE4, 0xFE, 0xFF, 0xFF, 0x00, 0x76, 0x52 };
	if( memcmp( p, sig, sizeof( sig ) ) != 0 ){
		logger::Write( "AVISO: desenho da munição na janela de equipamentos diferente do esperado; mantido" );
		return;
	}
	DWORD old;
	VirtualProtect( p + 7, 1, PAGE_EXECUTE_READWRITE, &old );
	p[7] = 0xEB;
	VirtualProtect( p + 7, 1, old, &old );
	FlushInstructionCache( GetCurrentProcess(), p + 7, 1 );
}

// Janela de atributos (UIStatusWnd, MakeWindow 0x0B) desativada no próprio cliente: equipamentos e atributos
// ficam juntos na janela Personagem (Alt+Q). Três lugares abrem a de atributos:
//  - 0x95A654: atalho Alt+A (UIBasicInfoWnd): "push 0Bh; mov ecx, mgr; call MakeWindow" -> nops;
//  - 0x8C21FB: botão (mensagem 6, 0C0h): "jne" -> "jmp" (pula a abertura);
//  - 0xA21A86: reabrir no login a que ficou aberta: "je" -> "jmp".
void game::DisableStatusWindow(){
	static bool done = false;
	if( done ) return;
	done = true;
	struct Patch { uintptr_t at; uint8_t sig[12]; int sigLen; uint8_t rep[12]; int repLen; };
	const Patch patches[] = {
		{ 0x95A654, { 0x6A, 0x0B, 0xB9, 0x68, 0x82, 0x2F, 0x01, 0xE8, 0x00, 0x71, 0x0C, 0x00 }, 12,
			{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, 12 },
		{ 0x8C21FB, { 0x75, 0x3A, 0x56, 0x6A, 0x0B }, 5, { 0xEB }, 1 },
		{ 0xA21A86, { 0x74, 0x09, 0x6A, 0x0B, 0x8B, 0xCF }, 6, { 0xEB }, 1 },
	};
	for( const Patch& p : patches ){
		uint8_t* a = reinterpret_cast<uint8_t*>( p.at );
		if( memcmp( a, p.sig, p.sigLen ) != 0 ){
			logger::Write( "AVISO: abertura da janela de atributos diferente do esperado em %08X; mantida", (unsigned)p.at );
			continue;
		}
		DWORD old;
		VirtualProtect( a, p.repLen, PAGE_EXECUTE_READWRITE, &old );
		memcpy( a, p.rep, p.repLen );
		VirtualProtect( a, p.repLen, old, &old );
	}
	FlushInstructionCache( GetCurrentProcess(), nullptr, 0 );
}

void game::FixChatIcons(){
	constexpr uintptr_t VT_CHAT = 0x1017FD0;            // UINewChatWnd
	constexpr uintptr_t PUSH_Y[] = { 0x8EE874, 0x8EE8C3 }; // OnDraw: ✕ (modo do chat) e cadeado em "push 4"
	constexpr int TOP = 1;
	static bool patched = false;
	if( !patched ){
		patched = true;
		for( uintptr_t a : PUSH_Y ){
			uint8_t* p = reinterpret_cast<uint8_t*>( a );
			if( p[0] != 0x6A || p[1] != 4 ) continue;
			DWORD old;
			VirtualProtect( p + 1, 1, PAGE_EXECUTE_READWRITE, &old );
			p[1] = (uint8_t)TOP;
			VirtualProtect( p + 1, 1, old, &old );
		}
	}
	uintptr_t w = g_sessionOk ? OpenWindow( VT_CHAT ) : 0, head = 0, node = 0;
	if( w == 0 || !Peek( w + OFS_CHILDREN, head ) || head == 0 || !Peek( head, node ) ) return;
	for( int i = 0; i < 64 && node != 0 && node != head; i++ ){
		uintptr_t child = 0;
		std::string image;
		int y = 0;
		if( Peek( node + 8, child ) && ReadStdString( child + OFS_BUTTON_IMAGE, image ) && Peek( child + client::OFS_WND_Y, y ) && y == 4
			&& ( image.find( "stick" ) != std::string::npos || image.find( "battle_option" ) != std::string::npos || image.find( "wnd_mini" ) != std::string::npos ) ){
			*reinterpret_cast<int*>( child + client::OFS_WND_Y ) = TOP;
		}
		if( !Peek( node, node ) ) break;
	}
}

void game::RemoveButtons(){
	struct Rule { uintptr_t vt; const char* names[3]; };
	static const Rule rules[] = {
		{ 0x10174D4, { "itemslot", nullptr, nullptr } },                          // UIShortCutWnd
		{ 0x101232C, { "btn_e_ability", "btn_e_change", "\\btn_off.bmp" } },     // UIEquipWnd (btn_off = tirar montaria/falcão: encaixe nosso)
	};
	if( !g_sessionOk ) return;
	for( const Rule& r : rules ){
		uintptr_t w = OpenWindow( r.vt ), head = 0, node = 0;
		if( w == 0 || !Peek( w + OFS_CHILDREN, head ) || head == 0 || !Peek( head, node ) ) continue;
		for( int i = 0; i < 64 && node != 0 && node != head; i++ ){
			uintptr_t next = 0, prev = 0, child = 0;
			if( !Peek( node, next ) || !Peek( node + 4, prev ) ) break;
			std::string image;
			bool match = false;
			if( Peek( node + 8, child ) && ReadStdString( child + OFS_BUTTON_IMAGE, image ) ){
				for( const char* n : r.names ) match = match || ( n && image.find( n ) != std::string::npos );
			}
			if( match ){
				// std::list (MSVC): nó = próximo, anterior, valor; tamanho logo depois da cabeça
				*reinterpret_cast<uintptr_t*>( prev ) = next;
				*reinterpret_cast<uintptr_t*>( next + 4 ) = prev;
				--*reinterpret_cast<uint32_t*>( w + OFS_CHILDREN + 4 );
				*reinterpret_cast<int*>( child + client::OFS_WND_X ) = -10000; // se algo ainda testar o retângulo
			}
			node = next;
		}
	}
}

namespace { static void CallSendMsg( uintptr_t w, int msg, int v1 ); }

void game::LegacyBasicButton( int id ){
	uintptr_t w = g_sessionOk ? Window( OFS_MGR_BASICINFO, VT_BASICINFO ) : 0;
	if( w ) CallSendMsg( w, 6, id ); // UIBasicInfoWnd: mensagem 6 = clique no botão (abre/fecha a janela dele)
}

bool game::PetWindowRect( client::WndRect& r ){
	uintptr_t w = g_sessionOk ? OpenWindow( VT_PETINFO ) : 0;
	return w != 0 && WindowRect( w, r );
}

bool game::PetCloseRect( client::WndRect& r ){
	return g_sessionOk && CloseButtonOf( OpenWindow( VT_PETINFO ), r );
}

void game::PetCovers( std::vector<client::WndRect>& out ){
	if( g_sessionOk ) WindowsAbove( VT_PETINFO, out );
	else out.clear();
}


bool game::MenuRect( client::WndRect& r ){
	return g_menuWnd != 0 && WindowRect( g_menuWnd, r );
}

namespace {
	uintptr_t HudWindow( game::HudWnd which ){
		if( !g_sessionOk ) return 0;
		switch( which ){
			case game::HudWnd::Basic: return Window( OFS_MGR_BASICINFO, VT_BASICINFO );
			case game::HudWnd::Chat: return OpenWindow( 0x1017FD0 );     // UINewChatWnd
			case game::HudWnd::Shortcut: return OpenWindow( 0x10174D4 ); // UIShortCutWnd
			case game::HudWnd::Minimap: return OpenWindow( 0x101484C );  // UIMinimapZoomWnd
			case game::HudWnd::QuickSlot: return OpenWindow( 0x10185C8 ); // UIQuickSlotWnd (2 slots no topo)
			case game::HudWnd::Cash: return OpenWindow( 0x1014AD4 );      // UInCash_CallWnd ("Loja")
			case game::HudWnd::Roulette: return OpenWindow( 0x101A088 );  // UIOpenRoulletteWnd
		}
		return 0;
	}
}

namespace {
	constexpr uintptr_t VT_CHAT_HISBOX = 0x100D370, VT_CHAT_EDIT = 0x1008E9C; // UIChatHisBox, UIChatEditCtrl
	constexpr uintptr_t OFS_HIS_TEXTS = 0x100, OFS_HIS_COLORS = 0x10C;        // std::vector<std::string> / <COLORREF>

	// filho visível (x >= 0) da janela do chat com essa vtable (o das outras abas fica em -700)
	uintptr_t ChatChild( uintptr_t vt ){
		uintptr_t w = HudWindow( game::HudWnd::Chat ), head = 0, node = 0;
		if( !w || !Peek( w + OFS_CHILDREN, head ) || !head || !Peek( head, node ) ) return 0;
		for( int i = 0; i < 64 && node && node != head; i++ ){
			uintptr_t c = 0, v = 0;
			int x = -1;
			if( Peek( node + 8, c ) && c && Peek( c, v ) && v == vt && Peek( c + client::OFS_WND_X, x ) && x >= 0 ) return c;
			if( !Peek( node, node ) ) break;
		}
		return 0;
	}
}

bool game::ReadChat( std::vector<ChatLine>& out, size_t& total ){
	out.clear();
	total = 0;
	uintptr_t h = ChatChild( VT_CHAT_HISBOX );
	uintptr_t tb = 0, te = 0, cb = 0, ce = 0;
	if( !h || !Peek( h + OFS_HIS_TEXTS, tb ) || !Peek( h + OFS_HIS_TEXTS + 4, te ) || !Peek( h + OFS_HIS_COLORS, cb )
		|| !Peek( h + OFS_HIS_COLORS + 4, ce ) || te < tb || ( te - tb ) % 24 != 0 || ( te - tb ) / 24 > 5000 ) return false;
	total = ( te - tb ) / 24;
	size_t colors = ce >= cb ? ( ce - cb ) / 4 : 0;
	size_t first = total > 12 ? total - 12 : 0; // só as últimas
	for( size_t k = first; k < total; k++ ){
		std::string text;
		if( !ReadStdString( tb + k * 24, text ) ) continue;
		uint32_t c = 0xFFFFFF;
		if( k < colors ) Peek( cb + k * 4, c );
		ChatLine l;
		int n = MultiByteToWideChar( 1252, 0, text.data(), (int)text.size(), nullptr, 0 );
		l.text.resize( n );
		if( n > 0 ) MultiByteToWideChar( 1252, 0, text.data(), (int)text.size(), &l.text[0], n );
		l.color = 0xFF000000 | ( ( c & 0xFF ) << 16 ) | ( c & 0xFF00 ) | ( ( c >> 16 ) & 0xFF ); // COLORREF (BGR) -> ARGB
		out.push_back( l );
	}
	return true;
}

bool game::ChatTyping(){
	uintptr_t e = ChatChild( VT_CHAT_EDIT );
	int show = 0;
	return e != 0 && Peek( e + OFS_WND_SHOW, show ) && show != 0;
}

void game::CloseShortcutItemSlot(){
	// fileira "item" da barra de atalhos (o botão de abrir/fechar saiu: RemoveButtons). Aberta numa sessão
	// anterior, o cliente a mantém: fecha como o botão faria (SendMsg(0, 6, 0x24D) da UIShortCutWnd)
	constexpr uintptr_t G_ITEMSLOT_OPEN = 0x15DB225;
	constexpr int VT_IDX_SENDMSG = 37, ID_CLOSE_ITEMSLOT = 0x24D;
	uint8_t open = 0;
	uintptr_t w = HudWindow( HudWnd::Shortcut ), vt = 0;
	if( !w || !Peek( G_ITEMSLOT_OPEN, open ) || !open || !Peek( w, vt ) ) return;
	__try{
		reinterpret_cast<void( __thiscall* )( void*, void*, int, int, int, int, int )>( *reinterpret_cast<uintptr_t*>( vt + VT_IDX_SENDMSG * 4 ) )(
			reinterpret_cast<void*>( w ), nullptr, 6, ID_CLOSE_ITEMSLOT, 0, 0, 0 );
	}__except( EXCEPTION_EXECUTE_HANDLER ){}
}

uintptr_t game::ShownWindow( uintptr_t vt ){
	uintptr_t w = OpenWindow( vt );
	int show = 0;
	return w && Peek( w + OFS_WND_SHOW, show ) && show ? w : 0;
}

bool game::HudWindowRect( HudWnd which, client::WndRect& r ){
	uintptr_t w = HudWindow( which );
	int show = 0;
	return w != 0 && Peek( w + OFS_WND_SHOW, show ) && show != 0 && WindowRect( w, r );
}

void game::PlaceHudWindow( HudWnd which, int x, int y ){
	uintptr_t w = HudWindow( which );
	if( w == 0 ) return;
	if( which == HudWnd::Chat ){
		// o fundo do chat é desenhado num retângulo absoluto guardado na janela (+0xA4 x, +0xA8 y, +0xAC/+0xB0
		// tamanho), atualizado só quando o cliente a move: anda junto
		int ox = 0, oy = 0;
		if( Peek( w + client::OFS_WND_X, ox ) && Peek( w + client::OFS_WND_Y, oy ) ){
			*reinterpret_cast<int*>( w + 0xA4 ) += x - ox;
			*reinterpret_cast<int*>( w + 0xA8 ) += y - oy;
		}
	}
	*reinterpret_cast<int*>( w + client::OFS_WND_X ) = x;
	*reinterpret_cast<int*>( w + client::OFS_WND_Y ) = y;
}

bool game::PlayerCell( int& x, int& y ){
	constexpr uintptr_t S_PLAYER_CELL = 0x15C0EE4; // célula do personagem (a mesma do minimapa)
	return g_sessionOk && Peek( S_PLAYER_CELL, x ) && Peek( S_PLAYER_CELL + 4, y ) && x >= 0 && y >= 0 && x < 1000 && y < 1000;
}

void game::PlaceMenu( int x, int y ){
	uintptr_t w = g_sessionOk ? MenuWindow() : 0;
	if( w == 0 ) return;
	*reinterpret_cast<int*>( w + client::OFS_WND_X ) = x;
	*reinterpret_cast<int*>( w + client::OFS_WND_Y ) = y;
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

void game::CoversOf( Replaced which, std::vector<client::WndRect>& out ){
	static const uintptr_t vts[] = { VT_BASICINFO, VT_MENUICON, VT_QUEST, VT_PETINFO, VT_ITEMCOLLECTION, VT_ITEMCOMPARE,
		0x101232C /* equipamentos */, VT_ITEMWND, 0x101F5E0 /* habilidades */, 0x1012AC4 /* atributos */, 0x101C9C0 /* armazém */, 0x101484C /* minimapa */ };
	if( g_sessionOk ) WindowsAbove( vts[(int)which], out );
	else out.clear();
}

bool game::Covered( Replaced which, float x, float y ){
	std::vector<client::WndRect> covers;
	CoversOf( which, covers );
	for( const client::WndRect& r : covers ) if( x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h ) return true;
	return false;
}


namespace {
	constexpr uintptr_t FN_CREATE_SURFACE = 0x553450; // cdecl ( largura, altura, 1 ) -> superfície
	constexpr int VT_IDX_INVALIDATE = 38;            // UIWindow::Invalidate (+0x98)

	bool ResizeWindowSurface( uintptr_t w, int width, int height ){
		__try{
			uintptr_t old = *reinterpret_cast<uintptr_t*>( w + 0x24 );
			uintptr_t surf = reinterpret_cast<uintptr_t( __cdecl* )( int, int, int )>( FN_CREATE_SURFACE )( width, height, 1 );
			if( surf == 0 ) return false;
			*reinterpret_cast<uintptr_t*>( w + 0x24 ) = surf;
			*reinterpret_cast<int*>( w + client::OFS_WND_WIDTH ) = width;
			*reinterpret_cast<int*>( w + client::OFS_WND_HEIGHT ) = height;
			if( old ){
				uintptr_t vt = *reinterpret_cast<uintptr_t*>( old );
				reinterpret_cast<void( __thiscall* )( void*, int )>( *reinterpret_cast<uintptr_t*>( vt ) )( reinterpret_cast<void*>( old ), 1 );
			}
			uintptr_t wvt = *reinterpret_cast<uintptr_t*>( w );
			reinterpret_cast<void( __thiscall* )( void* )>( *reinterpret_cast<uintptr_t*>( wvt + VT_IDX_INVALIDATE * 4 ) )( reinterpret_cast<void*>( w ) );
			return true;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return false;
		}
	}

	// Substituição: a janela nativa fica do tamanho da nossa; os filhos que a nossa também tem (ex.: barra de
	// título, que arrasta, e o fechar dentro dela) vão para baixo do equivalente e o resto sai da lista (não
	// desenha, não recebe o mouse, sem tooltip). O cliente então trata clique, arraste, ordem e cursor da área
	// inteira como sendo da janela.
	constexpr int KEEP_NATIVE = -0x7FFF; // KeepChild::x: não mexe na posição nem nos filhos
	struct KeepChild {
		uintptr_t ptr;         // o próprio filho (0 = pela classe/imagem)
		uintptr_t vt;          // classe do filho (0 = qualquer)
		const char* image;     // ou imagem do botão (nullptr = qualquer)
		int x, y, w, h;        // posição relativa à mãe; w/h = 0 mantém o tamanho
		const KeepChild* inner = nullptr; int innerCount = 0; // filhos dele que ficam
	};

	void DetachNode( uintptr_t parent, uintptr_t node, uintptr_t child ){
		uintptr_t next = 0, prev = 0;
		if( !Peek( node, next ) || !Peek( node + 4, prev ) ) return;
		// std::list (MSVC): nó = próximo, anterior, valor; tamanho logo depois da cabeça
		Poke( prev, next );
		Poke( next + 4, prev );
		uint32_t n = 0;
		if( Peek( parent + OFS_CHILDREN + 4, n ) && n > 0 ) Poke( parent + OFS_CHILDREN + 4, n - 1 );
		Poke( child + client::OFS_WND_X, -10000 ); // se algo ainda testar o retângulo
	}

	void LayoutChildren( uintptr_t w, const KeepChild* keep, int keepCount ){
		uintptr_t head = 0, node = 0;
		if( !Peek( w + OFS_CHILDREN, head ) || head == 0 || !Peek( head, node ) ) return;
		for( int i = 0; i < 128 && node != 0 && node != head; i++ ){
			uintptr_t next = 0, child = 0, cvt = 0;
			if( !Peek( node, next ) || !Peek( node + 8, child ) || !Peek( child, cvt ) ) break;
			std::string image;
			bool hasImage = ReadStdString( child + OFS_BUTTON_IMAGE, image );
			const KeepChild* k = nullptr;
			for( int j = 0; j < keepCount && !k; j++ ){
				const KeepChild& c = keep[j];
				if( c.ptr != 0 ? c.ptr == child
					: ( c.vt == 0 || c.vt == cvt ) && ( c.image == nullptr || ( hasImage && image.find( c.image ) != std::string::npos ) ) ) k = &c;
			}
			if( config::Get().debug ){
				static std::map<uintptr_t, bool> s_seen;
				if( !s_seen[child] ){
					s_seen[child] = true;
					int cx = 0, cy = 0, cw2 = 0, ch2 = 0;
					Peek( child + client::OFS_WND_X, cx ); Peek( child + client::OFS_WND_Y, cy );
					Peek( child + client::OFS_WND_WIDTH, cw2 ); Peek( child + client::OFS_WND_HEIGHT, ch2 );
					uintptr_t sub = 0, subHead = 0; uint32_t subN = 0;
					Peek( child + OFS_CHILDREN, subHead ); Peek( child + OFS_CHILDREN + 4, subN );
					logger::Write( "substituicao mae %08X: filho %08X vt=%08X (%d,%d %dx%d) img='%s' filhos=%u -> %s", (unsigned)w, (unsigned)child,
						(unsigned)cvt, cx, cy, cw2, ch2, hasImage ? image.c_str() : "", subN, k ? "fica" : "sai" );
				}
			}
			if( k && k->x == KEEP_NATIVE ){
				// fica onde o cliente o pôs (com os filhos dele)
			}else if( k ){
				int cw = 0, ch = 0;
				Peek( child + client::OFS_WND_WIDTH, cw ); Peek( child + client::OFS_WND_HEIGHT, ch );
				if( k->w > 0 && k->h > 0 && ( cw != k->w || ch != k->h ) ) ResizeWindowSurface( child, k->w, k->h );
				Poke( child + client::OFS_WND_X, k->x );
				Poke( child + client::OFS_WND_Y, k->y );
				LayoutChildren( child, k->inner, k->innerCount ); // filhos dele que não estão na lista também saem
			}else{
				DetachNode( w, node, child );
			}
			node = next;
		}
	}

	void Substitute( uintptr_t w, int width, int height, const KeepChild* keep, int keepCount ){
		if( w == 0 ) return;
		int cw = 0, ch = 0;
		if( !Peek( w + client::OFS_WND_WIDTH, cw ) || !Peek( w + client::OFS_WND_HEIGHT, ch ) ) return;
		if( cw != width || ch != height ){
			// o gerenciador desenha a superfície da janela (+0x24) com o tamanho da janela: superfície nova do
			// tamanho novo, como em UIWindow::Create (0xA05B00), sem chamar o OnCreate (recriaria os controles)
			if( !ResizeWindowSurface( w, width, height ) ) return;
		}
		LayoutChildren( w, keep, keepCount );
	}
}

void game::SubstituteQuestWindow( int width, int height, int closeX, int closeY ){
	if( !g_sessionOk ) return;
	uintptr_t w = OpenWindow( VT_QUEST );
	if( w == 0 ) return;
	// "Fechar" nativo: botão (0x101FE54) no rodapé, em 327,442 (antes de mexermos); guardado por janela
	static uintptr_t s_wnd = 0, s_close = 0;
	if( s_wnd != w ){
		s_wnd = w;
		s_close = 0;
		uintptr_t head = 0, node = 0;
		if( Peek( w + OFS_CHILDREN, head ) && head != 0 && Peek( head, node ) ){
			for( int i = 0; i < 128 && node != 0 && node != head && s_close == 0; i++ ){
				uintptr_t child = 0, cvt = 0;
				int cx = 0, cy = 0;
				if( Peek( node + 8, child ) && Peek( child, cvt ) && cvt == 0x101FE54 && Peek( child + client::OFS_WND_X, cx )
					&& Peek( child + client::OFS_WND_Y, cy ) && cx == 327 && cy == 442 ) s_close = child;
				if( !Peek( node, node ) ) break;
			}
		}
	}
	// barra de título nativa (arrasta a janela) embaixo da do Zero, sem os filhos dela; o fechar embaixo do X
	constexpr int TITLE_X = 28, TITLE_H = 22;
	const KeepChild keep[] = {
		{ 0, 0x101FF30, nullptr, TITLE_X, 0, width - TITLE_X, TITLE_H, nullptr, 0 },
		{ s_close != 0 ? s_close : 1, 0, nullptr, closeX, closeY, 16, 16, nullptr, 0 },
	};
	Substitute( w, width, height, keep, 2 );
}

void game::SubstitutePetWindow( int width, int height, int closeX, int closeY, int closeW, int closeH ){
	if( !g_sessionOk ) return;
	// janela de pet clássica: o fechar (imagem "close") embaixo do da Pokédex; o resto sai
	const KeepChild keep[] = { { 0, 0, "close", closeX, closeY, closeW, closeH, nullptr, 0 } };
	Substitute( OpenWindow( VT_PETINFO ), width, height, keep, 1 );
}

// ---------------------------------------------------------------- janela Personagem (UIEquipWnd refeita)
namespace {
	constexpr uintptr_t VT_EQUIPWND = 0x101232C;
	// "Qual encaixe está em (x, y)" da UIEquipWnd (thiscall: buf, x, y; preenche o item com 0x89B140 e
	// devolve buf): 0x89B220 só o ícone (clique, arrastar, soltar), 0x89BA30 ícone + nome (dica). As duas
	// usam a geometria antiga fixa no código; trocadas pela da janela Personagem.
	constexpr uintptr_t FN_SLOT_AT_ICON = 0x89B220, FN_SLOT_AT_WIDE = 0x89BA30;
	game::EquipSlotAtFn g_slotAt = nullptr;

	uintptr_t SlotAt( uintptr_t self, uintptr_t buf, int x, int y, bool wide ){
		int tab = 0, other = 0;
		Peek( self + 0xD0, tab );
		Peek( self + 0x94, other );
		int slot = g_slotAt ? g_slotAt( x, y, wide, tab == 1, other != 0 ) : -1;
		__try{
			reinterpret_cast<void*( __thiscall* )( void*, void*, int )>( 0x89B140 )( reinterpret_cast<void*>( self ), reinterpret_cast<void*>( buf ), slot ); // item do encaixe
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
		return buf;
	}
	uintptr_t __fastcall SlotAtIcon( uintptr_t self, void*, uintptr_t buf, int x, int y ){ return SlotAt( self, buf, x, y, false ); }
	uintptr_t __fastcall SlotAtWide( uintptr_t self, void*, uintptr_t buf, int x, int y ){ return SlotAt( self, buf, x, y, true ); }

	bool JumpTo( uintptr_t at, void* fn ){
		const uint8_t sig[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 }; // push ebp; mov ebp, esp; push -1; push ...
		uint8_t* p = reinterpret_cast<uint8_t*>( at );
		if( memcmp( p, sig, sizeof( sig ) ) != 0 ) return false;
		DWORD old;
		VirtualProtect( p, 5, PAGE_EXECUTE_READWRITE, &old );
		p[0] = 0xE9;
		*reinterpret_cast<int32_t*>( p + 1 ) = (int32_t)( reinterpret_cast<uintptr_t>( fn ) - ( at + 5 ) );
		VirtualProtect( p, 5, old, &old );
		FlushInstructionCache( GetCurrentProcess(), p, 5 );
		return true;
	}
}

namespace {
	volatile int32_t g_feetY = 0x95; // pés do personagem na janela de equipamentos (lido pelo OnDraw do cliente)
	game::EquipTipFn g_tipPos = nullptr;
	void __cdecl EquipTipPos( int mx, int my, int* x, int* y, uintptr_t self ){
		int other = 0;
		Peek( self + 0x94, other );
		if( g_tipPos ) g_tipPos( mx, my, *x, *y, other != 0 );
	}
	// dica do nome do item (0x8B63D0): x/y em [ebp-240h]/[ebp-244h], mouse em ebx / [ebp-248h]; continua em 0x8B6499
	__declspec( naked ) void EquipTipStub(){ // janela em edi
		__asm{
			push edi
			lea eax, [ebp - 0x244]
			push eax
			lea eax, [ebp - 0x240]
			push eax
			push dword ptr [ebp - 0x248]
			push ebx
			call EquipTipPos
			add esp, 20
			mov eax, 0x8B6499
			jmp eax
		}
	}
}

bool game::InstallEquipLayout( EquipSlotAtFn slotAt, int characterFeetY, EquipTipFn tipPos ){
	g_slotAt = slotAt;
	g_tipPos = tipPos;
	static int done = -1;
	if( done < 0 ){
		bool a = JumpTo( FN_SLOT_AT_ICON, reinterpret_cast<void*>( &SlotAtIcon ) );
		bool b = a && JumpTo( FN_SLOT_AT_WIDE, reinterpret_cast<void*>( &SlotAtWide ) );
		done = a && b ? 1 : 0;
		// posição da dica do nome: "cmp ebx, 0AAh" (início do cálculo antigo) -> jmp EquipTipStub
		uint8_t* t = reinterpret_cast<uint8_t*>( 0x8B646F );
		const uint8_t tsig[] = { 0x81, 0xFB, 0xAA, 0x00, 0x00, 0x00 };
		if( done && memcmp( t, tsig, sizeof( tsig ) ) == 0 ){
			DWORD old;
			VirtualProtect( t, 6, PAGE_EXECUTE_READWRITE, &old );
			t[0] = 0xE9;
			*reinterpret_cast<int32_t*>( t + 1 ) = (int32_t)( reinterpret_cast<uintptr_t>( &EquipTipStub ) - ( 0x8B646F + 5 ) );
			t[5] = 0x90;
			VirtualProtect( t, 6, old, &old );
			FlushInstructionCache( GetCurrentProcess(), t, 6 );
		}
		// personagem: o OnDraw o desenha em (largura / 2 + 1, 0x95); "push 95h; inc eax" vira "push [g_feetY]"
		// (x = largura / 2): uma área livre da janela, que muda com a janela (a própria ou a de outro jogador).
		// Os nomes dos itens continuam sendo escritos nas posições antigas, por cima do lugar original dele
		uint8_t* p = reinterpret_cast<uint8_t*>( 0x8AFC51 );
		const uint8_t sig[] = { 0x68, 0x95, 0x00, 0x00, 0x00, 0x40, 0x50, 0x56 };
		g_feetY = characterFeetY;
		if( done && memcmp( p, sig, sizeof( sig ) ) == 0 ){
			DWORD old;
			VirtualProtect( p, 6, PAGE_EXECUTE_READWRITE, &old );
			p[0] = 0xFF; p[1] = 0x35;
			*reinterpret_cast<uint32_t*>( p + 2 ) = reinterpret_cast<uint32_t>( &g_feetY );
			VirtualProtect( p, 6, old, &old );
			FlushInstructionCache( GetCurrentProcess(), p, 6 );
		}
		if( !done ) logger::Write( "AVISO: encaixes da janela de equipamentos diferentes do esperado; janela Personagem desativada" );
	}
	return done == 1;
}

namespace {
	constexpr uintptr_t FN_BUTTON_IMAGE = 0x82CF60; // UIButton::SetImage( caminho, estado 0/1/2, 0 )
	constexpr uintptr_t FN_STRING_ASSIGN = 0x4E3F40; // std::string::assign( const char*, size )
	const char* const CLOSE_IMG[3] = { "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA\\codex\\fechar.bmp",
		"\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA\\codex\\fechar_on.bmp",
		"\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA\\codex\\fechar_on.bmp" };
	const char* const CHECK_OFF = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA\\codex\\caixa.bmp";
	const char* const CHECK_ON = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA\\codex\\caixa_on.bmp";

	void AssignRaw( uintptr_t str, const char* s ){
		__try{
			reinterpret_cast<void( __thiscall* )( uintptr_t, const char*, size_t )>( FN_STRING_ASSIGN )( str, s, strlen( s ) );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
	void AssignString( uintptr_t str, const char* s ){
		std::string cur;
		if( ReadStdString( str, cur ) && cur == s ) return;
		AssignRaw( str, s );
	}
	void SetButtonImages( uintptr_t c ){
		__try{
			for( int st = 0; st < 3; st++ )
				reinterpret_cast<bool( __thiscall* )( uintptr_t, const char*, int, int )>( FN_BUTTON_IMAGE )( c, CLOSE_IMG[st], st, 0 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	// fechar e "Exibir Equip." da janela Personagem com as imagens do tema (codex\*.bmp na char.grf), na própria
	// janela: o botão recebe as imagens novas (SetImage), a caixa os caminhos dela (+0x98 desmarcada, +0x80
	// marcada) e a cor/altura do texto (+0xE4/+0xE8)
	void StyleEquipChildren( uintptr_t w ){
		uintptr_t head = 0, node = 0;
		if( !w || !Peek( w + OFS_CHILDREN, head ) || !head || !Peek( head, node ) ) return;
		for( int i = 0; i < 64 && node && node != head; i++ ){
			uintptr_t c = 0, vt = 0;
			if( Peek( node + 8, c ) && c && Peek( c, vt ) ){
				std::string img;
				ReadStdString( c + OFS_BUTTON_IMAGE, img );
				if( img.find( "sys_close" ) != std::string::npos || img.find( "codex\\fechar" ) != std::string::npos ){
					if( img.find( "fechar" ) == std::string::npos ) SetButtonImages( c );
				}else if( img.find( "btn_items" ) != std::string::npos && !game::HasCart() ){
					Poke( c + client::OFS_WND_X, -10000 ); // botão Carrinho: só com carrinho
				}else if( vt == 0x1009FF0 ){
					AssignString( c + 0x98, CHECK_OFF );
					AssignString( c + 0x80, CHECK_ON );
					Poke( c + 0xE4, (uint32_t)RGB( 41, 41, 41 ) );
					Poke( c + 0xE8, 12 );
				}
			}
			if( !Peek( node, node ) ) break;
		}
	}
}

void game::SubstituteEquipWindow( int width, int height, const client::WndRect& close, const client::WndRect& check, const client::WndRect& cart ){
	if( !g_sessionOk ) return;
	// do tamanho da janela Personagem; ficam o fechar, as caixas "exibir" (uma por aba) e o carrinho, cada um
	// embaixo do equivalente nosso; as abas, minimizar, título, lista de títulos e o resto saem
	// as abas Título e Dano usam a geometria original (280 de largura): lista de títulos (0x100C818), ok /
	// fechar, rolagem dos painéis de dano (0x100AFE4) e as opções (0x100A278) ficam onde o cliente os põe
	const KeepChild keep[] = {
		{ 0, 0, "sys_close", close.x, close.y, close.w, close.h, nullptr, 0 },
		{ 0, 0, "codex\\fechar", close.x, close.y, close.w, close.h, nullptr, 0 }, // depois do StyleEquipChildren
		{ 0, 0x1009FF0, nullptr, check.x, check.y, check.w, check.h, nullptr, 0 },
		{ 0, 0, "btn_items", cart.x, cart.y, cart.w, cart.h, nullptr, 0 },
		{ 0, 0x100C818, nullptr, KEEP_NATIVE, 0, 0, 0, nullptr, 0 },
		{ 0, 0x100AFE4, nullptr, KEEP_NATIVE, 0, 0, 0, nullptr, 0 },
		{ 0, 0x100A278, nullptr, KEEP_NATIVE, 0, 0, 0, nullptr, 0 },
		{ 0, 0, "btn_apply", KEEP_NATIVE, 0, 0, 0, nullptr, 0 },
		{ 0, 0, "\\btn_close", KEEP_NATIVE, 0, 0, 0, nullptr, 0 },
	};
	uintptr_t w = EquipWnd();
	Substitute( w, width, height, keep, (int)( sizeof( keep ) / sizeof( keep[0] ) ) );
	StyleEquipChildren( w );
}

void game::SelectEquipWnd( int which ){
	g_equipSel = which;
}

void game::CoversOfEquip( std::vector<client::WndRect>& out ){
	uintptr_t w = g_sessionOk ? EquipWnd() : 0;
	if( w ) WindowsAbove( 0x101232C, out, w );
	else out.clear();
}

void game::SetEquipFeetY( int y ){
	g_feetY = y;
}

void game::SetEquipContentTop( uintptr_t w, int y ){
	int cur = 0;
	if( w && Peek( w + 0xD4, cur ) && cur != y ) Poke( w + 0xD4, y );
}

void game::EquipSetTab( int tab ){
	uintptr_t w = g_sessionOk ? EquipWnd() : 0, vt = 0;
	if( !w || !Peek( w, vt ) ) return;
	__try{ // a mensagem do controle de abas (0x16, aba), que saiu da janela
		reinterpret_cast<void( __thiscall* )( void*, void*, int, int, int, int, int )>( *reinterpret_cast<uintptr_t*>( vt + 37 * 4 ) )(
			reinterpret_cast<void*>( w ), nullptr, 0x16, tab, 0, 0, 0 );
	}__except( EXCEPTION_EXECUTE_HANDLER ){}
}

void game::SubstituteMenu( int width, int height, const std::vector<Placement>& place ){
	uintptr_t w = g_sessionOk ? MenuWindow() : 0;
	if( w == 0 ) return;
	int cw = 0, ch = 0;
	if( !Peek( w + client::OFS_WND_WIDTH, cw ) || !Peek( w + client::OFS_WND_HEIGHT, ch ) ) return;
	if( ( cw != width || ch != height ) && !ResizeWindowSurface( w, width, height ) ) return;
	for( const Placement& p : place ){
		if( p.child == 0 ) continue;
		int x = 0, y = 0, pw = 0, ph = 0;
		if( !Peek( p.child + client::OFS_WND_WIDTH, pw ) || !Peek( p.child + client::OFS_WND_HEIGHT, ph ) ) continue;
		if( ( pw != p.rect.w || ph != p.rect.h ) && !ResizeWindowSurface( p.child, p.rect.w, p.rect.h ) ) continue;
		if( Peek( p.child + client::OFS_WND_X, x ) && x != p.rect.x ) Poke( p.child + client::OFS_WND_X, p.rect.x );
		if( Peek( p.child + client::OFS_WND_Y, y ) && y != p.rect.y ) Poke( p.child + client::OFS_WND_Y, p.rect.y );
	}
}

void game::SubstituteBasicInfo( int width, int height ){
	if( !g_sessionOk ) return;
	Substitute( Window( OFS_MGR_BASICINFO, VT_BASICINFO ), width, height, nullptr, 0 );
}


namespace {
	constexpr uintptr_t FN_ITEM_HIDDEN = 0xD75520, G_ITEM_HIDDEN = 0x15D3120; // usados pelo teste de clique 0x934F10

	bool ItemHidden( int id ){
		__try{
			return reinterpret_cast<int( __thiscall* )( void*, int )>( FN_ITEM_HIDDEN )( reinterpret_cast<void*>( G_ITEM_HIDDEN ), id ) != 0;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return false;
		}
	}

	// item que a grade nativa pula (mesma condição de 0x934F10)
	bool NativeInvSkipped( uintptr_t node ){
		uint8_t flag = 0;
		Peek( node + 0x65, flag );
		if( !flag ) return false;
		std::string name;
		if( !ReadStdString( node + 0x34, name ) ) return false;
		return ItemHidden( atoi( name.c_str() ) );
	}

	void CallSendMsg3( uintptr_t w, int msg, int v1, int v2, int v3 ){
		__try{
			uintptr_t vt = *reinterpret_cast<uintptr_t*>( w );
			auto fn = reinterpret_cast<int( __thiscall* )( void*, void*, int, int, int, int, int )>( *reinterpret_cast<uintptr_t*>( vt + VT_IDX_SENDMSG * 4 ) );
			fn( reinterpret_cast<void*>( w ), nullptr, msg, v1, v2, v3, 0 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){
		}
	}
}

bool game::ReadNativeInventory( NativeInventory& o ){
	uintptr_t w = g_sessionOk ? Window( OFS_MGR_ITEMWND, VT_ITEMWND ) : 0;
	if( !w ) return false;
	o = NativeInventory();
	Peek( w + 0xC0, o.cols ); Peek( w + 0xBC, o.rows ); Peek( w + 0xB8, o.scroll ); Peek( w + OFS_INV_TAB, o.tab );
	if( o.cols <= 0 || o.rows <= 0 || o.cols > 16 || o.rows > 32 ) return false;
	if( o.scroll < 0 ) o.scroll = 0;
	uintptr_t head = 0, node = 0;
	if( Peek( w + OFS_INV_LIST, head ) && head && Peek( head, node ) ){
		int start = o.cols * o.scroll, want = o.cols * o.rows;
		for( int k = 0; node && node != head && k < 1000; k++ ){
			bool skip = NativeInvSkipped( node );
			if( !skip ) o.total++;
			if( k >= start && !skip && (int)o.cells.size() < want ){
				int idx = -1;
				Peek( node + 0xC, idx );
				o.cells.push_back( idx );
			}
			if( !Peek( node, node ) ) break;
		}
	}
	uintptr_t chead = 0, cnode = 0;
	if( Peek( w + OFS_CHILDREN, chead ) && chead && Peek( chead, cnode ) ){
		for( int i = 0; i < 64 && cnode && cnode != chead; i++ ){
			uintptr_t child = 0, cvt = 0;
			if( Peek( cnode + 8, child ) && Peek( child, cvt ) && cvt == 0x100DE34 && WindowRect( child, o.tabs ) ){ o.tabsOk = true; break; }
			if( !Peek( cnode, cnode ) ) break;
		}
	}
	return true;
}

void game::ResizeNativeInventory( int width, int height ){
	uintptr_t w = g_sessionOk ? Window( OFS_MGR_ITEMWND, VT_ITEMWND ) : 0;
	if( !w ) return;
	int cw = 0, ch = 0;
	Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch );
	if( cw != width || ch != height ) CallSendMsg3( w, 0xE, 7, width, height );
}

bool game::ReadItemWindowAt( uintptr_t w, Item& o, std::string& title ){
	if( !g_sessionOk || !ReadItemInfo( w, o ) ) return false;
	ReadStdString( w + 0x98, title ); // nome como o cliente mostra (+refino, [encaixes])
	uintptr_t vt = 0;
	if( Peek( w, vt ) && vt == VT_ITEMCOMPARE ){ // opções do equipado (pacotes do inventário)
		game::Item inv;
		if( game::EquippedAt( o.wear ? o.wear : o.location, inv ) && inv.id == o.id ) for( int k = 0; k < 5; k++ ) o.options[k] = inv.options[k];
	}
	return true;
}

bool game::ReadItemNative( bool compare, ItemNative& out ){
	if( !g_sessionOk ) return false;
	uintptr_t w = compare ? Window( OFS_MGR_ITEMCOMPARE, VT_ITEMCOMPARE ) : Window( OFS_MGR_ITEMCOLLECTION, VT_ITEMCOLLECTION );
	if( !w ) return false;
	if( !Peek( w + client::OFS_WND_WIDTH, out.w ) || !Peek( w + client::OFS_WND_HEIGHT, out.h ) ) return false;
	return true;
}

namespace {
	constexpr uintptr_t FN_EQUIP_SLOT = 0x89B140, FN_STRING_DTOR = 0x4E2EF0;
	constexpr uintptr_t G_TITLES_BEGIN = 0x15D926C, G_TITLES_END = 0x15D9270; // títulos que o personagem tem

	bool CallEquipSlot( uintptr_t w, int slot, uint8_t* buf ){
		__try{
			reinterpret_cast<void*( __thiscall* )( void*, void*, int )>( FN_EQUIP_SLOT )( reinterpret_cast<void*>( w ), buf, slot );
			return true;
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return false;
		}
	}

	void FreeItemInfo( uint8_t* buf ){
		__try{
			reinterpret_cast<void( __thiscall* )( void* )>( FN_STRING_DTOR )( buf + 0x2C );
			reinterpret_cast<void( __thiscall* )( void* )>( FN_STRING_DTOR )( buf + 0x44 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){
		}
	}

	std::wstring Widen( const std::string& s ){
		if( s.empty() ) return L"";
		std::wstring w( s.size(), L'\0' );
		int n = MultiByteToWideChar( 1252, 0, s.data(), (int)s.size(), &w[0], (int)w.size() );
		w.resize( n > 0 ? n : 0 );
		return w;
	}

	// std::map<int, {int id; std::string texto}> da lista (MSVC): em ordem, só as chaves [lo, hi)
	void TitleRows( uintptr_t node, int lo, int hi, std::vector<game::NativeEquip::Row>& out, int depth ){
		uint8_t nil = 1;
		if( !node || depth > 40 || out.size() > 64 || !Peek( node + 0xD, nil ) || nil ) return;
		uintptr_t left = 0, right = 0;
		int key = 0;
		Peek( node, left ); Peek( node + 8, right ); Peek( node + 0x10, key );
		if( key > lo ) TitleRows( left, lo, hi, out, depth + 1 );
		if( key >= lo && key < hi ){
			game::NativeEquip::Row r;
			r.index = key;
			Peek( node + 0x14, r.id );
			std::string t;
			if( ReadStdString( node + 0x18, t ) ) r.text = Widen( t );
			uintptr_t b = 0, e = 0;
			if( Peek( G_TITLES_BEGIN, b ) && Peek( G_TITLES_END, e ) && b && e > b && e - b < 4096 ){
				for( uintptr_t p = b; p < e && !r.owned; p += 4 ){ int id = 0; Peek( p, id ); r.owned = id == r.id; }
			}
			out.push_back( r );
		}
		if( key < hi - 1 ) TitleRows( right, lo, hi, out, depth + 1 );
	}
}

bool game::ReadNativeEquip( NativeEquip& o ){
	uintptr_t w = g_sessionOk ? EquipWnd() : 0;
	if( !w ) return false;
	o = NativeEquip();
	int other = 0;
	Peek( w + 0xD0, o.tab ); Peek( w + 0x94, other ); Peek( w + 0x138, o.damage );
	o.other = other != 0;
	if( o.tab == 0 || o.tab == 1 ){
		static const int SLOTS[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15 };
		for( int s : SLOTS ){
			alignas( 8 ) uint8_t buf[0x200] = {};
			if( !CallEquipSlot( w, s, buf ) ) continue;
			Item it;
			if( ReadItemInfo( reinterpret_cast<uintptr_t>( buf ) - 4 - OFS_ITEMWND_INFO, it ) ){ o.slot[s] = it; o.has[s] = true; }
			FreeItemInfo( buf );
		}
	}
	uintptr_t head = 0, node = 0;
	if( Peek( w + OFS_CHILDREN, head ) && head && Peek( head, node ) ){
		for( int i = 0; i < 64 && node && node != head; i++ ){
			uintptr_t c = 0, vt = 0;
			int show = 0;
			client::WndRect r;
			if( Peek( node + 8, c ) && Peek( c, vt ) && Peek( c + 0x28, show ) && show && WindowRect( c, r ) && r.x > -1000 ){
				std::string img;
				ReadStdString( c + OFS_BUTTON_IMAGE, img );
				int st = 0;
				if( vt == 0x100A278 ){
					NativeEquip::Check k; k.r = r; Peek( c + 0x30, st ); k.checked = st != 0;
					std::string t; if( ReadStdString( c + 0x7C, t ) ) k.label = Widen( t );
					o.checks.push_back( k );
				}else if( vt == 0x1009FF0 ){ // caixa "exibir equipamento/visual" (a da aba aberta)
					o.showOk = true; o.show = r; Peek( c + 0x30, st ); o.showChecked = st != 0;
				}else if( img.find( "btn_apply" ) != std::string::npos ){ o.applyOn = true; o.apply = r; }
				else if( img.find( "btn_close.bmp" ) != std::string::npos && img.find( "sys_" ) == std::string::npos ){ o.closeOn = true; o.close = r; }
				else if( img.find( "sys_close" ) != std::string::npos ){ o.sysCloseOn = true; o.sysClose = r; }
				else if( img.find( "btn_items" ) != std::string::npos ){ // carrinho: só com carrinho (senão o cliente o põe fora da janela)
					int cart = 0;
					Peek( 0x15D8D1C, cart );
					o.cartOn = cart > 0; o.cart = r;
				}
				else if( vt == 0x100DE34 ){ o.tabsOn = true; o.tabs = r; } // UITabControl (Equip, Visual, Título, Dano)
				else if( vt == 0x100C818 ){
					o.listOn = true; o.list = r;
					Peek( c + 0x94, o.listTop ); Peek( c + 0x90, o.listSel ); Peek( c + 0xB4, o.listRowH ); Peek( c + 0x8C, o.listCount );
					if( o.listRowH <= 0 || o.listRowH > 64 ) o.listRowH = 18;
					int visible = r.h / o.listRowH + 1;
					uintptr_t mhead = 0, root = 0;
					if( Peek( c + 0x88, mhead ) && mhead && Peek( mhead + 4, root ) ) TitleRows( root, o.listTop, o.listTop + visible, o.rows, 0 );
				}
			}
			if( !Peek( node, node ) ) break;
		}
	}
	return true;
}

namespace {
	constexpr uintptr_t FN_SKILL_LIST = 0x9716B0, G_SKILL_LISTMODE = 0x15D31B4;

	uintptr_t SkillListOf( uintptr_t w, int tab ){
		__try{
			return reinterpret_cast<uintptr_t>( reinterpret_cast<void*( __thiscall* )( void*, int, int )>( FN_SKILL_LIST )( reinterpret_cast<void*>( w ), tab, 0 ) );
		}__except( EXCEPTION_EXECUTE_HANDLER ){
			return 0;
		}
	}
}

// ---------------------------------------------------------------- janela de habilidades refeita
namespace {
	constexpr uintptr_t VT_SKILLWND = 0x101F5E0, G_SKILL_LIST_SPECIAL = 0x15D3140;
	constexpr uintptr_t FN_SKILL_NAME = 0x7384E0;   // thiscall( nó + 8 ) -> nome
	constexpr uintptr_t FN_SKILL_SP = 0x738410;     // thiscall( nó + 8, nível ) -> SP
	constexpr uintptr_t FN_SKILL_ICON = 0xD44CF0;   // thiscall( 0x15D3120, recurso, buf ) -> caminho do ícone
	constexpr uintptr_t FN_SKILL_USELV = 0xD44D10;  // thiscall( 0x15D3120, id ) -> nível escolhido para usar
	constexpr uintptr_t FN_SKILL_LAYOUT = 0x973E70, FN_SKILL_BUTTONS = 0x973FF0;
	const char* const UI = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA\\";

	uintptr_t SkillListPtr( uintptr_t w, int tab, int tabCount ){
		return tab == tabCount - 1 ? G_SKILL_LIST_SPECIAL : w + 0x25C + tab * 8;
	}

	int TabCount( uintptr_t w ){
		uintptr_t tc = 0, b = 0, e = 0;
		if( !Peek( w + 0x94, tc ) || !tc || !Peek( tc + 0xB4, b ) || !Peek( tc + 0xB8, e ) || e < b ) return 0;
		return (int)( ( e - b ) / 24 ); // itens de 24 bytes (o cliente divide por 24: imul 0x2AAAAAAB; sar 2)
	}

	const char* SkillNameRaw( uintptr_t node ){
		__try{ return reinterpret_cast<const char*( __thiscall* )( uintptr_t )>( FN_SKILL_NAME )( node + 8 ); }
		__except( EXCEPTION_EXECUTE_HANDLER ){ return nullptr; }
	}
	int SkillSp( uintptr_t node, int level ){
		__try{ return reinterpret_cast<int( __thiscall* )( uintptr_t, int )>( FN_SKILL_SP )( node + 8, level ); }
		__except( EXCEPTION_EXECUTE_HANDLER ){ return 0; }
	}
	bool SkillIconPath( int res, char* buf ){
		__try{ reinterpret_cast<void( __thiscall* )( uintptr_t, int, char* )>( FN_SKILL_ICON )( 0x15D3120, res, buf ); return true; }
		__except( EXCEPTION_EXECUTE_HANDLER ){ return false; }
	}
	int SkillUseLevel( int id ){
		__try{ return reinterpret_cast<int( __thiscall* )( uintptr_t, int )>( FN_SKILL_USELV )( 0x15D3120, id ); }
		__except( EXCEPTION_EXECUTE_HANDLER ){ return 0; }
	}
	void CallVoid( uintptr_t fn, uintptr_t self ){
		__try{ reinterpret_cast<void( __thiscall* )( uintptr_t )>( fn )( self ); }
		__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	bool Visible( uintptr_t c ){
		int x = -1000;
		return c && Peek( c + client::OFS_WND_X, x ) && x > -150;
	}

	void Place( uintptr_t c, int x, int y ){
		if( !c ) return;
		Poke( c + client::OFS_WND_X, x );
		Poke( c + client::OFS_WND_Y, y );
	}

	void SetImages( uintptr_t c, const char* normal, const char* over ){
		__try{
			reinterpret_cast<bool( __thiscall* )( uintptr_t, const char*, int, int )>( 0x82CF60 )( c, normal, 0, 0 );
			reinterpret_cast<bool( __thiscall* )( uintptr_t, const char*, int, int )>( 0x82CF60 )( c, over, 1, 0 );
			reinterpret_cast<bool( __thiscall* )( uintptr_t, const char*, int, int )>( 0x82CF60 )( c, over, 2, 0 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	// imagens do tema num UIButton (SetImage: normal, em cima, apertado) e o tamanho dele
	void Restyle( uintptr_t c, const char* normal, const char* over, int w, int h ){
		std::string cur;
		if( !c ) return;
		std::string want = std::string( UI ) + "codex\\" + normal;
		if( !ReadStdString( c + OFS_BUTTON_IMAGE, cur ) || cur != want ){
			std::string o = std::string( UI ) + "codex\\" + over;
			SetImages( c, want.c_str(), o.c_str() );
		}
		int cw = 0, ch = 0;
		Peek( c + client::OFS_WND_WIDTH, cw ); Peek( c + client::OFS_WND_HEIGHT, ch );
		if( cw != w || ch != h ) ResizeWindowSurface( c, w, h );
	}
}

bool game::ReadSkillView( uintptr_t w, SkillView& o ){
	o = SkillView();
	if( !w ) return false;
	Peek( w + 0x238, o.tab ); Peek( w + 0x23C, o.scroll ); Peek( w + 0x234, o.hover ); Peek( w + 0x250, o.points );
	o.tabCount = TabCount( w );
	for( int t = 0; t < o.tabCount; t++ ){
		uint32_t n = 0;
		uintptr_t l = SkillListPtr( w, t, o.tabCount ), h = 0, first = 0;
		if( ( Peek( l + 4, n ) && n > 0 ) || t == o.tab ){
			int id = 0;
			if( n > 0 && Peek( l, h ) && h && Peek( h, first ) && first != h ) Peek( first + 0x10, id );
			o.tabs.push_back( t );
			o.tabFirst.push_back( id );
		}
	}
	uintptr_t lst = SkillListPtr( w, o.tab, o.tabCount ), head = 0, node = 0;
	uint32_t size = 0;
	if( !Peek( lst, head ) || !head || !Peek( head, node ) ) return true;
	Peek( lst + 4, size );
	o.total = (int)size;
	for( int i = 0; i < 400 && node && node != head; i++ ){
		if( i >= o.scroll && i < o.scroll + ui::SK_ROWS ){
			SkillRow r;
			int res = 0;
			Peek( node + 0x10, r.id ); Peek( node + 0x14, r.inf ); Peek( node + 0x18, r.level ); Peek( node + 0x30, r.maxLevel );
			Peek( node + 0x28, res );
			if( const char* n = SkillNameRaw( node ) ) r.name = n;
			char buf[260] = {};
			if( SkillIconPath( res, buf ) && buf[0] ) r.icon = buf;
			r.useLevel = SkillUseLevel( r.id );
			if( r.useLevel <= 0 || r.useLevel > r.level ) r.useLevel = r.level;
			r.sp = r.level > 0 ? SkillSp( node, r.useLevel ) : 0;
			// setas de nível e "+" da linha: os botões que o cliente deixou à mostra (FixSkillButtons os pôs na linha)
			int row = i - o.scroll;
			uintptr_t up = 0, left = 0;
			if( row < 10 ) Peek( w + 0xA8 + row * 4, up );
			Peek( w + 0xD0 + row * 4, left );
			r.up = Visible( up );
			r.arrows = Visible( left );
			o.rows.push_back( r );
		}
		if( !Peek( node, node ) ) break;
	}
	return true;
}

void game::SkillSetTab( int tab ){
	uintptr_t w = g_sessionOk ? Window( 0x2C4, VT_SKILLWND ) : 0, vt = 0;
	if( !w || !Peek( w, vt ) ) return;
	__try{
		reinterpret_cast<void( __thiscall* )( void*, void*, int, int, int, int, int )>( *reinterpret_cast<uintptr_t*>( vt + 37 * 4 ) )(
			reinterpret_cast<void*>( w ), nullptr, 0x16, tab, 0, 0, 0 );
	}__except( EXCEPTION_EXECUTE_HANDLER ){}
}

void game::FixSkillButtons( uintptr_t w ){
	using namespace ui;
	const int x0 = SK_M;
	auto rowTop = [&]( int row ){ return SK_TOP + row * SK_RH; };
	for( int i = 0; i < 10; i++ ){ // "+" (cliente: 0x4E, 34 + 32 * linha)
		uintptr_t c = 0; int x = 0, y = 0;
		if( !Peek( w + 0xA8 + i * 4, c ) || !c ) continue;
		Restyle( c, "sk_mais.bmp", "sk_mais_on.bmp", 16, 16 );
		Peek( c + client::OFS_WND_X, x ); Peek( c + client::OFS_WND_Y, y );
		if( x == 0x4E ) Place( c, x0 + SK_ROW_W - 24, rowTop( ( y - 34 ) / 32 ) + 8 );
	}
	for( int i = 0; i < 42; i++ ){ // setas de nível (cliente: 0x70 / 0xC0, 48 + 32 * linha)
		uintptr_t l = 0, r = 0; int x = 0, y = 0;
		if( Peek( w + 0xD0 + i * 4, l ) && l ){
			Restyle( l, "sk_esq.bmp", "sk_esq.bmp", 11, 11 );
			Peek( l + client::OFS_WND_X, x ); Peek( l + client::OFS_WND_Y, y );
			if( x == 0x70 ) Place( l, x0 + 190, rowTop( ( y - 48 ) / 32 ) + 11 );
		}
		if( Peek( w + 0x178 + i * 4, r ) && r ){
			Restyle( r, "sk_dir.bmp", "sk_dir.bmp", 11, 11 );
			Peek( r + client::OFS_WND_X, x ); Peek( r + client::OFS_WND_Y, y );
			if( x == 0xC0 ) Place( r, x0 + 262, rowTop( ( y - 48 ) / 32 ) + 11 );
		}
	}
	uintptr_t c = 0;
	if( Peek( w + 0xA4, c ) && c ){ Restyle( c, "fechar.bmp", "fechar_on.bmp", 19, 19 ); Place( c, SK_W - 37, 10 ); }   // fechar
	if( Peek( w + 0xA0, c ) ) Place( c, -10000, -10000 );                                                             // minimizar
	if( Peek( w + 0x220, c ) && c ){ Restyle( c, "sk_aplicar.bmp", "sk_aplicar_on.bmp", 76, 24 ); Place( c, x0, SK_H - 36 ); }
	if( Peek( w + 0x224, c ) && c ){ Restyle( c, "sk_cancelar.bmp", "sk_cancelar_on.bmp", 76, 24 ); Place( c, x0 + 82, SK_H - 36 ); }
	if( Peek( w + 0x228, c ) ) Place( c, -10000, -10000 ); // "Descrições"
	if( Peek( w + 0x22C, c ) ) Place( c, -10000, -10000 ); // redimensionar
	if( Peek( w + 0x98, c ) ) Place( c, -10000, -10000 );  // barra de rolagem (a roda continua mandando para ela)
	if( Peek( w + 0x94, c ) ) Place( c, -10000, -10000 );  // abas verticais (as nossas ficam no topo)
	// "base" da barra de título
	uintptr_t head = 0, node = 0;
	if( Peek( w + OFS_CHILDREN, head ) && head && Peek( head, node ) ){
		for( int i = 0; i < 200 && node && node != head; i++ ){
			uintptr_t ch = 0; std::string img;
			if( Peek( node + 8, ch ) && ch && ReadStdString( ch + OFS_BUTTON_IMAGE, img ) && img.find( "sys_base" ) != std::string::npos ) Place( ch, -10000, -10000 );
			if( !Peek( node, node ) ) break;
		}
	}
}

void game::SubstituteSkillWindow(){
	uintptr_t w = g_sessionOk ? Window( 0x2C4, VT_SKILLWND ) : 0;
	if( !w ) return;
	int mode = 0, cw = 0, ch = 0, bottom = 0, top = 0;
	Peek( G_SKILL_LISTMODE, mode );
	Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch );
	Peek( w + 0x24C, bottom ); Peek( w + 0x248, top );
	bool changed = false;
	if( mode == 0 ){ Poke( G_SKILL_LISTMODE, 1 ); changed = true; } // só o modo lista
	// área da lista para o cálculo do cliente: (altura - 0x24C - 30) / 32 = SK_ROWS linhas
	const int wantBottom = ui::SK_H - 30 - ui::SK_ROWS * 32 - 8;
	if( bottom != wantBottom || top != ui::SK_TOP ){ Poke( w + 0x24C, wantBottom ); Poke( w + 0x248, ui::SK_TOP ); changed = true; }
	if( cw != ui::SK_W || ch != ui::SK_H ){
		if( !ResizeWindowSurface( w, ui::SK_W, ui::SK_H ) ) return;
		changed = true;
	}
	if( changed ){
		CallVoid( FN_SKILL_LAYOUT, w );
		CallVoid( FN_SKILL_BUTTONS, w );
	}
}

// ---------------------------------------------------------------- inventário refeito
void game::InventorySetTab( int tab ){
	uintptr_t w = g_sessionOk ? Window( OFS_MGR_ITEMWND, VT_ITEMWND ) : 0;
	if( w ) CallSendMsg( w, 0x16, tab );
}

void game::SubstituteInventoryWindow(){
	uintptr_t w = g_sessionOk ? Window( OFS_MGR_ITEMWND, VT_ITEMWND ) : 0;
	if( !w ) return;
	int cw = 0, ch = 0, cols = 0, rows = 0;
	Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch );
	Peek( w + 0xC0, cols ); Peek( w + 0xBC, rows );
	bool changed = false;
	if( cw != ui::INV_W || ch != ui::INV_H ){
		if( !ResizeWindowSurface( w, ui::INV_W, ui::INV_H ) ) return;
		changed = true;
	}
	if( cols != ui::INV_COLS || rows != ui::INV_ROWS ){
		Poke( w + 0xC0, ui::INV_COLS ); Poke( w + 0xBC, ui::INV_ROWS );
		changed = true;
	}
	if( changed ) CallVoid( 0x94B4E0, w ); // rolagem (linhas visíveis x total)
	uintptr_t c = 0;
	if( Peek( w + 0xDC, c ) && c ){ Restyle( c, "fechar.bmp", "fechar_on.bmp", 19, 19 ); Place( c, ui::INV_W - 35, 10 ); }
	static const int OFF[] = { 0x9C, 0xA0, 0xD4, 0xD8, 0xF8, 0xF4, 0xFC, 0x100, 0x104, 0x10C };
	for( int o : OFF ) if( Peek( w + o, c ) && c ){
		int x = 0;
		Peek( c + client::OFS_WND_X, x );
		if( x != -10000 ) Place( c, -10000, -10000 );
	}
}


namespace {
	void CallInvalidate( uintptr_t w ){
		__try{
			uintptr_t vt = *reinterpret_cast<uintptr_t*>( w );
			reinterpret_cast<void( __thiscall* )( void* )>( *reinterpret_cast<uintptr_t*>( vt + VT_IDX_INVALIDATE * 4 ) )( reinterpret_cast<void*>( w ) );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

// ---------------------------------------------------------------- descrição do item no tema
// A janela é a do cliente com as imagens do tema (moldura sysbox_*, topo collection_bg: tools/personagem_bmp.py);
// a caixa do texto (+0x190) limpa o fundo com a cor de +0xE0 (0x8572A0): vira a do tema
void game::SubstituteItemWindows(){
	constexpr uint32_t THEME_BG = 0xFFE7EFEF; // ( 239, 239, 231 ) em 0xAABBGGRR
	const uintptr_t wins[2] = { Window( OFS_MGR_ITEMCOLLECTION, VT_ITEMCOLLECTION ), Window( OFS_MGR_ITEMCOMPARE, VT_ITEMCOMPARE ) };
	for( uintptr_t w : wins ){
		uintptr_t text = 0;
		uint32_t bg = 0;
		if( !w || !Peek( w + 0x190, text ) || !text || !Peek( text + 0xE0, bg ) || bg == THEME_BG ) continue;
		Poke( text + 0xE0, THEME_BG );
		CallInvalidate( text );
		CallInvalidate( w );
	}
}

// ---------------------------------------------------------------- armazém refeito
namespace {
	constexpr uintptr_t VT_STORAGE = 0x101C9C0;
	uintptr_t StorageWnd(){
		uintptr_t w = g_sessionOk ? OpenWindow( VT_STORAGE ) : 0;
		int show = 0;
		return w && Peek( w + OFS_WND_SHOW, show ) && show ? w : 0;
	}
}

bool game::StorageWindowRect( client::WndRect& r ){
	return WindowRect( StorageWnd(), r );
}

void game::StorageSetTab( int tab ){
	if( uintptr_t w = StorageWnd() ) CallSendMsg( w, 0x16, tab );
}

void game::ReadItemList( uintptr_t w, int maxItems, std::vector<uintptr_t>& out, int& scroll, int& total ){
	out.clear(); scroll = 0; total = 0;
	int cols = 1;
	if( !w ) return;
	Peek( w + 0xB8, scroll ); Peek( w + 0xC0, cols );
	if( scroll < 0 ) scroll = 0;
	if( cols < 1 ) cols = 1;
	uintptr_t head = 0, node = 0;
	if( !Peek( w + OFS_INV_LIST, head ) || !head || !Peek( head, node ) ) return;
	for( int k = 0; node && node != head && k < 2000; k++ ){
		if( !NativeInvSkipped( node ) ){
			if( total >= scroll * cols && (int)out.size() < maxItems ) out.push_back( node + 8 );
			total++;
		}
		if( !Peek( node, node ) ) break;
	}
}

game::CartInfo game::ReadCartInfo(){
	CartInfo c;
	Peek( 0x15D4034, c.count ); Peek( 0x15D4038, c.max ); Peek( 0x15D403C, c.weight ); Peek( 0x15D4040, c.maxWeight );
	return c;
}

namespace {
	// janela da família do inventário com tamanho, linhas/colunas fixas; filhos: fora, ou o X do tema
	void FixListWindow( uintptr_t w, int width, int height, int rows, int cols, std::initializer_list<int> off, int closeOfs ){
		int cw = 0, ch = 0, r = 0, c = 0;
		Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch ); Peek( w + 0xBC, r ); Peek( w + 0xC0, c );
		bool changed = false;
		if( cw != width || ch != height ){
			if( !ResizeWindowSurface( w, width, height ) ) return;
			changed = true;
		}
		if( r != rows || c != cols ){ Poke( w + 0xBC, rows ); Poke( w + 0xC0, cols ); changed = true; }
		if( changed ) CallVoid( 0x94B4E0, w );
		uintptr_t ch2 = 0;
		for( int o : off ) if( Peek( w + o, ch2 ) && ch2 ){
			int x = 0;
			Peek( ch2 + client::OFS_WND_X, x );
			if( x != -10000 ) Place( ch2, -10000, -10000 );
		}
		// título (texto estático do cliente, sem campo na janela): fora
		uintptr_t head = 0, node = 0;
		if( Peek( w + OFS_CHILDREN, head ) && head && Peek( head, node ) ){
			for( int i = 0; i < 64 && node && node != head; i++ ){
				uintptr_t child = 0, cvt = 0;
				int y = 0;
				if( Peek( node + 8, child ) && child && Peek( child, cvt ) && cvt == 0x1009160 && Peek( child + client::OFS_WND_Y, y ) && y == 2 ) Place( child, -10000, -10000 );
				if( !Peek( node, node ) ) break;
			}
		}
		if( closeOfs && Peek( w + closeOfs, ch2 ) && ch2 ){
			Restyle( ch2, "fechar.bmp", "fechar_on.bmp", 19, 19 );
			int x = 0;
			Peek( ch2 + client::OFS_WND_X, x );
			if( x != width - 35 ) Place( ch2, width - 35, 10 );
		}
	}
}

bool game::ListWindowTitle( uintptr_t w, std::string& out ){
	// o título do cliente é um texto estático (0x1009160) filho da janela; o texto é a primeira std::string
	// curta e legível dentro dele
	uintptr_t head = 0, node = 0;
	if( !w || !Peek( w + OFS_CHILDREN, head ) || !head || !Peek( head, node ) ) return false;
	for( int i = 0; i < 64 && node && node != head; i++ ){
		uintptr_t c = 0, cvt = 0;
		if( Peek( node + 8, c ) && c && Peek( c, cvt ) && cvt == 0x1009160 ){
			for( int off = 0x60; off < 0x180; off += 4 ){
				uint32_t size = 0, cap = 0;
				if( !Peek( c + off + 0x10, size ) || !Peek( c + off + 0x14, cap ) || size == 0 || size > 40 || cap < size || cap > 256 ) continue;
				std::string t;
				if( !ReadStdString( c + off, t ) ) continue;
				bool ok = true;
				for( char ch : t ) ok = ok && (unsigned char)ch >= 0x20;
				if( ok ){ out = t; return true; }
			}
		}
		if( !Peek( node, node ) ) break;
	}
	return false;
}

void game::SubstituteCartWindow(){
	uintptr_t w = g_sessionOk ? Window( 0x2F4, 0x101D4B8 ) : 0;
	if( w ) FixListWindow( w, ui::CART_W, ui::CART_H, 7, 8, { 0x9C, 0xA0, 0xD8 }, 0xD4 );
}

void game::SubstituteSearchWindow(){
	uintptr_t w = g_sessionOk ? Window( 0x2AC, 0x101C8E8 ) : 0;
	if( w ) FixListWindow( w, ui::SR_W, ui::SR_H, ui::SR_ROWS, 1, { 0xA0, 0xF8, 0xF4 }, 0xDC );
	// as dos ícones de filtro (uma por ícone, todas da mesma classe): sem fechar (somem ao desmarcar o filtro)
	if( !g_sessionOk ) return;
	uintptr_t head = 0, node = 0;
	if( !Peek( G_WINDOW_MGR + OFS_MGR_WINDOWS, head ) || !head || !Peek( head, node ) ) return;
	for( int i = 0; i < 512 && node && node != head; i++ ){
		uintptr_t fw = 0, vt = 0;
		int show = 0;
		if( Peek( node + 8, fw ) && fw && Peek( fw, vt ) && vt == 0x101C810 && Peek( fw + OFS_WND_SHOW, show ) && show )
			FixListWindow( fw, ui::SR_W, ui::SR_H, ui::SR_ROWS, 1, { 0xA0, 0xF4, 0xF0 }, 0 );
		if( !Peek( node, node ) ) break;
	}
}

bool game::ReadStorageView( uintptr_t w, StorageView& o ){
	o = StorageView();
	if( !w ) return false;
	Peek( w + 0x160, o.tab ); Peek( w + 0xB8, o.scroll ); Peek( w + 0xCC, o.total );
	std::string c;
	if( ReadStdString( w + 0x124, c ) ) o.count.assign( c.begin(), c.end() );
	uintptr_t head = 0, node = 0;
	if( !Peek( w + OFS_INV_LIST, head ) || !head || !Peek( head, node ) ) return true;
	for( int k = 0, shown = 0; node && node != head && k < 2000; k++ ){
		if( !NativeInvSkipped( node ) ){ // mesma ordem e mesmo filtro do teste de clique (0x934DD0)
			if( shown >= o.scroll && (int)o.items.size() < ui::ST_ROWS ) o.items.push_back( node + 8 );
			shown++;
		}
		if( !Peek( node, node ) ) break;
	}
	return true;
}

void game::SubstituteStorageWindow(){
	uintptr_t w = StorageWnd();
	if( !w ) return;
	int cw = 0, ch = 0, rows = 0;
	Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch ); Peek( w + 0xBC, rows );
	bool changed = false;
	if( cw != ui::ST_W || ch != ui::ST_H ){
		if( !ResizeWindowSurface( w, ui::ST_W, ui::ST_H ) ) return;
		changed = true;
	}
	if( rows != ui::ST_ROWS ){ Poke( w + 0xBC, ui::ST_ROWS ); Poke( w + 0xC0, 1 ); changed = true; }
	if( changed ) CallVoid( 0x94B4E0, w );
	bool moved = false;
	auto place = [&]( uintptr_t c, int x, int y ){
		int cx = 0, cy = 0;
		if( c && Peek( c + client::OFS_WND_X, cx ) && Peek( c + client::OFS_WND_Y, cy ) && ( cx != x || cy != y ) ){
			Place( c, x, y );
			if( x > -1000 ){ CallInvalidate( c ); moved = true; } // o filho é composto na superfície da janela
		}
	};
	uintptr_t c = 0;
	static const int OFF[] = { 0x13C, 0x148, 0xA0, 0x9C, 0xA4 }; // título, abas, rolagem, redimensionar, "fechar"
	for( int o : OFF ) if( Peek( w + o, c ) ) place( c, -10000, -10000 );
	for( int k = 0; k < 8; k++ ) if( Peek( w + 0xE0 + k * 4, c ) ) place( c, ui::ST_M + k * 26, ui::ST_FILTER_Y );
	if( Peek( w + 0xD4, c ) ){
		// ordem (combobox): aberta, cresce para cima com a lista (y - altura extra) e o cliente volta o x ao
		// nativo; ao fechar ele desce o mesmo tanto. O canto de baixo fica sempre no lugar do tema
		int chh = 18;
		Peek( c + client::OFS_WND_HEIGHT, chh );
		place( c, ui::ST_W - ui::ST_M - 62, ui::ST_FILTER_Y + 3 - std::max( 0, chh - 18 ) );
	}
	if( Peek( w + 0xA8, c ) ) place( c, ui::ST_W - ui::ST_M - 23, ui::ST_H - 32 );
	if( Peek( w + 0xD0, c ) ) place( c, ui::ST_W - ui::ST_M - 23 - 4 - 121, ui::ST_H - 32 );
	// o X do sistema não tem campo na janela: é o botão filho que sobra
	uintptr_t head = 0, node = 0, a4 = 0, a8 = 0;
	Peek( w + 0xA4, a4 ); Peek( w + 0xA8, a8 );
	if( Peek( w + OFS_CHILDREN, head ) && head && Peek( head, node ) ){
		for( int i = 0; i < 64 && node && node != head; i++ ){
			uintptr_t child = 0, cvt = 0;
			if( Peek( node + 8, child ) && child && child != a4 && child != a8 && Peek( child, cvt ) && cvt == 0x1009910 ){
				Restyle( child, "fechar.bmp", "fechar_on.bmp", 19, 19 );
				place( child, ui::ST_W - 35, 10 );
			}
			if( !Peek( node, node ) ) break;
		}
	}
	if( moved ) CallInvalidate( w ); // sem isso fica a imagem velha do filho no lugar antigo
}

bool game::ReadNativeSkills( NativeSkills& o ){
	uintptr_t w = g_sessionOk ? Window( 0x2C4, 0x101F5E0 ) : 0;
	if( !w ) return false;
	o = NativeSkills();
	int mode = 1;
	Peek( w + 0x238, o.tab ); Peek( w + 0x23C, o.scroll ); Peek( G_SKILL_LISTMODE, mode );
	o.listMode = mode != 0;
	uintptr_t lst = SkillListOf( w, o.tab ), head = 0, node = 0;
	if( lst && Peek( lst, head ) && head && Peek( head, node ) ){
		for( int i = 0; i < 300 && node && node != head; i++ ){
			NativeSkills::Entry e;
			uint32_t use = 0;
			Peek( node + 0x10, e.id ); Peek( node + 0x14, e.inf ); Peek( node + 0x18, e.level ); Peek( node + 0x1C, e.sp );
			Peek( node + 0x30, e.maxLevel ); Peek( node + 0x38, use );
			e.useLevel = (int)( use & 0xFFFF );
			o.entries.push_back( e );
			if( !Peek( node, node ) ) break;
		}
	}
	uintptr_t chead = 0, cnode = 0;
	if( Peek( w + OFS_CHILDREN, chead ) && chead && Peek( chead, cnode ) ){
		for( int i = 0; i < 200 && cnode && cnode != chead; i++ ){
			uintptr_t c = 0, vt = 0;
			int show = 0;
			client::WndRect r;
			if( Peek( cnode + 8, c ) && Peek( c, vt ) && Peek( c + 0x28, show ) && show && WindowRect( c, r ) && r.x > -100 ){
				std::string img;
				ReadStdString( c + OFS_BUTTON_IMAGE, img );
				if( vt == 0x100DE34 ){ o.tabs = r; o.tabsOk = true; o.tabCount = std::max( 1, ( r.h + 22 ) / 45 ); }
				else if( img.find( "skill_up" ) != std::string::npos ) o.ups.push_back( r );
				else if( img.find( "arw_left" ) != std::string::npos ) o.arrowsLeft.push_back( r );
				else if( img.find( "arw_right" ) != std::string::npos ) o.arrowsRight.push_back( r );
				else if( img.find( "btn_apply" ) != std::string::npos ){ o.applyOn = true; o.apply = r; }
				else if( img.find( "btn_reset" ) != std::string::npos ){ o.resetOn = true; o.reset = r; }
				else if( vt == 0x1009FF0 && r.y < 16 ){ int st = 0; Peek( c + 0x30, st ); o.descOn = true; o.desc = r; o.descChecked = st != 0; }
			}
			if( !Peek( cnode, cnode ) ) break;
		}
	}
	return true;
}

void game::OpenEquipWindow(){
	constexpr uintptr_t FN_MAKE_WINDOW = 0xA21760; // UIWindowMgr::MakeWindow(id): cria ou mostra
	constexpr int WND_EQUIP = 0x0A;
	const uint8_t sig[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68 };
	if( !g_sessionOk || memcmp( reinterpret_cast<void*>( FN_MAKE_WINDOW ), sig, sizeof( sig ) ) != 0 ) return;
	__try{
		reinterpret_cast<uintptr_t( __thiscall* )( void*, int )>( FN_MAKE_WINDOW )( reinterpret_cast<void*>( G_WINDOW_MGR ), WND_EQUIP );
	}__except( EXCEPTION_EXECUTE_HANDLER ){}
}

bool game::StatusWindowShown(){
	uintptr_t w = g_sessionOk ? OpenWindow( 0x1012AC4 ) : 0;
	int show = 0;
	return w != 0 && Peek( w + OFS_WND_SHOW, show ) && show != 0;
}

void game::HideStatusWindow(){
	uintptr_t w = g_sessionOk ? OpenWindow( 0x1012AC4 ) : 0, vt = 0;
	if( !w || !Peek( w, vt ) ) return;
	__try{
		reinterpret_cast<void( __thiscall* )( void*, int )>( *reinterpret_cast<uintptr_t*>( vt + 14 * 4 ) )( reinterpret_cast<void*>( w ), 0 ); // SetShow(0)
	}__except( EXCEPTION_EXECUTE_HANDLER ){}
}

void game::MoveEquipWindow( int x, int y ){
	uintptr_t w = g_sessionOk ? EquipWnd() : 0;
	if( !w ) return;
	Poke( w + client::OFS_WND_X, x );
	Poke( w + client::OFS_WND_Y, y );
}

bool game::StatusWindowRect( client::WndRect& r ){
	return g_sessionOk && WindowRect( OpenWindow( 0x1012AC4 ), r );
}

bool game::TooltipRect( client::WndRect& r ){
	if( !g_sessionOk ) return false;
	uintptr_t head = 0, node = 0;
	if( !Peek( G_WINDOW_MGR + OFS_MGR_WINDOWS, head ) || head == 0 || !Peek( head, node ) ) return false;
	for( int i = 0; i < 512 && node != 0 && node != head; i++ ){
		uintptr_t w = 0, v = 0;
		int show = 0;
		if( Peek( node + 8, w ) && w != 0 && Peek( w, v ) && v == VT_TOOLTIP && Peek( w + OFS_WND_SHOW, show ) && show != 0 && WindowRect( w, r ) ) return true;
		if( !Peek( node, node ) ) break;
	}
	return false;
}

void game::HideTooltip(){
	if( !g_sessionOk ) return;
	uintptr_t head = 0, node = 0;
	if( !Peek( G_WINDOW_MGR + OFS_MGR_WINDOWS, head ) || head == 0 || !Peek( head, node ) ) return;
	for( int i = 0; i < 512 && node != 0 && node != head; i++ ){
		uintptr_t w = 0, v = 0;
		int show = 0;
		if( Peek( node + 8, w ) && w != 0 && Peek( w, v ) && v == VT_TOOLTIP && Peek( w + OFS_WND_SHOW, show ) && show != 0 ){
			Poke( w + OFS_WND_SHOW, 0 );
			if( config::Get().debug ) logger::Write( "dica nativa escondida (atributos)" );
		}
		if( !Peek( node, node ) ) break;
	}
}

void game::SubstituteStatusWindow( int width, int height, const client::WndRect up[6], bool visible[6] ){
	for( int k = 0; k < 6; k++ ) visible[k] = false;
	uintptr_t w = g_sessionOk ? OpenWindow( 0x1012AC4 ) : 0;
	if( !w ) return;
	int cw = 0, ch = 0;
	if( !Peek( w + client::OFS_WND_WIDTH, cw ) || !Peek( w + client::OFS_WND_HEIGHT, ch ) ) return;
	if( ( cw != width || ch != height ) && !ResizeWindowSurface( w, width, height ) ) return;
	uintptr_t head = 0, node = 0;
	if( !Peek( w + OFS_CHILDREN, head ) || !head || !Peek( head, node ) ) return;
	int arrow = 0;
	for( int i = 0; i < 64 && node && node != head; i++ ){
		uintptr_t c = 0, next = 0;
		if( !Peek( node, next ) || !Peek( node + 8, c ) ) break;
		std::string img;
		ReadStdString( c + OFS_BUTTON_IMAGE, img );
		int x = 0, y = 0;
		Peek( c + client::OFS_WND_X, x ); Peek( c + client::OFS_WND_Y, y );
		auto place = [&]( int nx, int ny ){ if( x != nx ) Poke( c + client::OFS_WND_X, nx ); if( y != ny ) Poke( c + client::OFS_WND_Y, ny ); };
		if( img.find( "sys_close" ) != std::string::npos ) place( width - 14, 3 );
		else if( img.find( "sys_mini" ) != std::string::npos ) place( width - 28, 3 );
		else if( img.find( "sys_base" ) != std::string::npos ) place( -10000, -10000 );
		else if( img.find( "arw_right" ) != std::string::npos ){
			// o cliente esconde a seta (x = -100) quando o atributo não pode subir
			if( arrow < 6 && x > -50 ){ visible[arrow] = true; place( up[arrow].x, up[arrow].y ); }
			arrow++;
		}
		node = next;
	}
}

namespace {
	// filhos de uma janela (std::list em +0x50)
	template <typename Fn> void ForChildren( uintptr_t w, Fn fn ){
		uintptr_t head = 0, node = 0;
		if( !Peek( w + OFS_CHILDREN, head ) || !head || !Peek( head, node ) ) return;
		for( int i = 0; i < 128 && node && node != head; i++ ){
			uintptr_t c = 0;
			if( Peek( node + 8, c ) && c ) fn( c );
			if( !Peek( node, node ) ) break;
		}
	}
}

void game::SubstituteLoginWindow( int screenW, int screenH ){
	uintptr_t w = reinterpret_cast<uintptr_t>( client::LoginWindow( client::LoginWnd::Login ) );
	if( !w || screenH < 300 ) return;
	const ui::LoginGeo g = ui::LoginGeometry( screenW, screenH );
	int cw = 0, ch = 0, x = 0, y = 0;
	Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch );
	if( ( cw != g.w || ch != g.h ) && !ResizeWindowSurface( w, g.w, g.h ) ) return;
	Peek( w + client::OFS_WND_X, x ); Peek( w + client::OFS_WND_Y, y );
	if( x != g.x || y != g.y ) Place( w, g.x, g.y );
	const int px = g.panelX + ui::LG_PAD, pw = ui::LGP_W - 2 * ui::LG_PAD;
	auto put = [&]( uintptr_t c, int qx, int qy, int qw, int qh ){
		if( !c ) return;
		int cx = 0, cy = 0, cww = 0, chh = 0;
		Peek( c + client::OFS_WND_X, cx ); Peek( c + client::OFS_WND_Y, cy );
		Peek( c + client::OFS_WND_WIDTH, cww ); Peek( c + client::OFS_WND_HEIGHT, chh );
		if( qw > 0 && ( cww != qw || chh != qh ) ) ResizeWindowSurface( c, qw, qh );
		if( cx != qx || cy != qy ){ Place( c, qx, qy ); CallInvalidate( c ); }
	};
	uintptr_t c = 0;
	// campos: dentro das caixas brancas (+0x90/+0x94/+0x98 = fundo R, G, B; +0x128 alinhamento: 0 esquerda)
	auto field = [&]( uintptr_t e, int boxY ){
		if( !e ) return;
		put( e, px + 12, boxY + ( ui::LG_BOX_H - ui::LG_FIELD_H ) / 2, pw - 24, ui::LG_FIELD_H );
		int r = 0, al = 0;
		Peek( e + 0x90, r ); Peek( e + 0x128, al );
		if( r != 255 || al != 0 ){
			Poke( e + 0x90, 255 ); Poke( e + 0x94, 255 ); Poke( e + 0x98, 255 ); Poke( e + 0x128, 0 );
			CallInvalidate( e );
		}
	};
	if( Peek( w + 0x94, c ) ) field( c, g.idY );
	if( Peek( w + 0x98, c ) ) field( c, g.pwY );
	if( Peek( w + 0xC8, c ) ) put( c, -10000, -10000, 0, 0 );
	ForChildren( w, [&]( uintptr_t ch2 ){
		uintptr_t cvt = 0;
		Peek( ch2, cvt );
		std::string img;
		ReadStdString( ch2 + OFS_BUTTON_IMAGE, img );
		// botões com texto próprio (vt 0x1009AC8, std::string em +0x120): o texto vem na imagem do tema
		if( cvt == 0x1009AC8 ){
			std::string t;
			if( ReadStdString( ch2 + 0x120, t ) && !t.empty() ) AssignString( ch2 + 0x120, "" );
		}
		if( img.find( "bt_start" ) != std::string::npos ) put( ch2, px, g.enterY, pw, ui::LG_ENTER_H );
		else if( img.find( "bt_join" ) != std::string::npos ) put( ch2, px, g.linksY, 100, 20 );
		else if( img.find( "bt_close2" ) != std::string::npos ) put( ch2, px + pw - 40, g.linksY, 40, 20 );
		else if( img.find( "bt_close" ) != std::string::npos ) put( ch2, px + pw / 2 - 50, g.linksY, 100, 20 );
		else if( cvt == 0x1009FF0 ) put( ch2, px, g.saveY, 16, 16 );   // lembrar usuário
		else if( cvt == 0x1009160 ) put( ch2, -10000, -10000, 0, 0 ); // rótulo antigo ("Salvar")
	} );
	// a janela que acompanha a de login (vt 0x101A59C, mesmo retângulo): fora
	if( uintptr_t comp = OpenWindow( 0x101A59C ) ) put( comp, -10000, -10000, 0, 0 );
	CallInvalidate( w ); // campo ativo e realce com o mouse
}

void game::SubstituteWaitWindow( int screenW, int screenH ){
	uintptr_t w = reinterpret_cast<uintptr_t>( client::LoginWindow( client::LoginWnd::Wait ) );
	if( !w || screenH < 300 ) return;
	int cw = 0, ch = 0, x = 0, y = 0;
	Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch );
	const ui::EntryGeo g = ui::WaitGeometry( screenW, screenH );
	if( ( cw != g.w || ch != g.h ) && !ResizeWindowSurface( w, g.w, g.h ) ) return;
	Peek( w + client::OFS_WND_X, x ); Peek( w + client::OFS_WND_Y, y );
	if( x != g.x || y != g.y ) Place( w, g.x, g.y );
	ForChildren( w, [&]( uintptr_t c ){
		int cx = 0;
		if( Peek( c + client::OFS_WND_X, cx ) && cx != -10000 ) Place( c, -10000, -10000 );
	} );
	CallInvalidate( w ); // barra animada
}

void game::SubstituteServerWindow( int screenW, int screenH ){
	uintptr_t w = reinterpret_cast<uintptr_t>( client::LoginWindow( client::LoginWnd::SelectServer ) );
	if( !w || screenH < 300 ) return;
	const ui::EntryGeo g = ui::ServerGeometry( screenW, screenH );
	int cw = 0, ch = 0, x = 0, y = 0;
	Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch );
	if( ( cw != g.w || ch != g.h ) && !ResizeWindowSurface( w, g.w, g.h ) ) return;
	Peek( w + client::OFS_WND_X, x ); Peek( w + client::OFS_WND_Y, y );
	if( x != g.x || y != g.y ) Place( w, g.x, g.y );
	auto put = [&]( uintptr_t c, int qx, int qy, int qw, int qh ){
		int cx = 0, cy = 0, cww = 0, chh = 0;
		Peek( c + client::OFS_WND_X, cx ); Peek( c + client::OFS_WND_Y, cy );
		Peek( c + client::OFS_WND_WIDTH, cww ); Peek( c + client::OFS_WND_HEIGHT, chh );
		if( qw > 0 && ( cww != qw || chh != qh ) ) ResizeWindowSurface( c, qw, qh );
		if( cx != qx || cy != qy ) Place( c, qx, qy );
	};
	const int by = g.cardY + ui::ServerButtonsY(), half = ( ui::SV_W - 48 - 12 ) / 2;
	const int okX = g.cardX + 24 + half + 12, backX = g.cardX + 24;
	ForChildren( w, [&]( uintptr_t c ){
		uintptr_t cvt = 0;
		Peek( c, cvt );
		std::string img;
		ReadStdString( c + OFS_BUTTON_IMAGE, img );
		if( img.find( "btn_ok" ) != std::string::npos || img.find( "codex\\sv_entrar" ) != std::string::npos ){
			Restyle( c, "sv_entrar.bmp", "sv_entrar_on.bmp", half, ui::SV_BTN_H );
			put( c, okX, by, half, ui::SV_BTN_H );
		}else if( img.find( "btn_cancel" ) != std::string::npos || img.find( "codex\\sv_voltar_g" ) != std::string::npos ){
			Restyle( c, "sv_voltar_g.bmp", "sv_voltar_g_on.bmp", half, ui::SV_BTN_H );
			put( c, backX, by, half, ui::SV_BTN_H );
		}else if( cvt == 0x100D9E0 ){
			// a lista (o servidor escolhido) precisa ficar dentro da janela (fora dela o cliente não conecta):
			// atrás do ENTRAR, que é desenhado depois e recebe o clique
			put( c, okX, by, half, ui::SV_BTN_H );
		}else{
			put( c, -10000, -10000, 0, 0 );
		}
	} );
	CallInvalidate( w ); // contagem e realce com o mouse
}

namespace {
	void DumpNode( uintptr_t w, int depth ){
		int x = 0, y = 0, cw = 0, ch = 0, show = 0; uintptr_t vt = 0;
		Peek( w, vt ); Peek( w + client::OFS_WND_X, x ); Peek( w + client::OFS_WND_Y, y );
		Peek( w + client::OFS_WND_WIDTH, cw ); Peek( w + client::OFS_WND_HEIGHT, ch ); Peek( w + 0x28, show );
		std::string img; ReadStdString( w + OFS_BUTTON_IMAGE, img );
		uintptr_t ondraw = 0; Peek( vt + 20 * 4, ondraw );
		logger::Write( "%*s%p vt=%06X draw=%06X (%d,%d) %dx%d show=%d img=%s", depth * 2, "", (void*)w, (unsigned)vt, (unsigned)ondraw, x, y, cw, ch, show & 0xFF, img.size() < 80 ? img.c_str() : "?" );
		if( depth > 3 ) return;
		uintptr_t head = 0, node = 0;
		if( Peek( w + OFS_CHILDREN, head ) && head && Peek( head, node ) ){
			for( int i = 0; i < 64 && node && node != head; i++ ){
				uintptr_t c = 0;
				if( Peek( node + 8, c ) && c ) DumpNode( c, depth + 1 );
				if( !Peek( node, node ) ) break;
			}
		}
	}
}

void game::DumpWindowTree( uintptr_t w, const char* label ){
	logger::Write( "=== arvore %s ===", label );
	if( w ) DumpNode( w, 0 );
}

// ---------------------------------------------------------------- seleção de personagem hardcoded
namespace {
	struct SelOrig { uintptr_t c; int x, y, w, h; std::string img[4], text; bool button; };
	uintptr_t g_selWnd = 0;
	std::vector<SelOrig> g_selOrig;  // geometria original (janela + filhos) para voltar ao modo Original
	std::map<uintptr_t, int> g_selRole; // bt_info: 1 = apagar/cancelar exclusão (y 322), 2 = apagar definitivo (y 351)

	void SelSetImage( uintptr_t c, const char* path, int state ){
		__try{ reinterpret_cast<bool( __thiscall* )( uintptr_t, const char*, int, int )>( 0x82CF60 )( c, path, state, 0 ); }
		__except( EXCEPTION_EXECUTE_HANDLER ){}
	}

	void SelRemember( uintptr_t c ){
		for( const SelOrig& o : g_selOrig ) if( o.c == c ) return;
		SelOrig o{ c, 0, 0, 0, 0 };
		Peek( c + client::OFS_WND_X, o.x ); Peek( c + client::OFS_WND_Y, o.y );
		Peek( c + client::OFS_WND_WIDTH, o.w ); Peek( c + client::OFS_WND_HEIGHT, o.h );
		uintptr_t vt = 0;
		Peek( c, vt );
		o.button = vt == 0x1009AC8 || vt == 0x1009910; // botões: imagens normal/em cima/apertado/desativado
		if( o.button ){
			for( int k = 0; k < 4; k++ ) ReadStdString( c + OFS_BUTTON_IMAGE + 0x18 * k, o.img[k] );
			if( vt == 0x1009AC8 ) ReadStdString( c + 0x120, o.text );
		}
		g_selOrig.push_back( o );
	}

	void SelPut( uintptr_t c, int x, int y, int w, int h ){
		SelRemember( c );
		int cx = 0, cy = 0, cw = 0, ch = 0;
		Peek( c + client::OFS_WND_X, cx ); Peek( c + client::OFS_WND_Y, cy );
		Peek( c + client::OFS_WND_WIDTH, cw ); Peek( c + client::OFS_WND_HEIGHT, ch );
		if( w > 0 && ( cw != w || ch != h ) ) ResizeWindowSurface( c, w, h );
		if( cx != x || cy != y ) Place( c, x, y );
	}

	// janelas vivas: a própria e os filhos/netos agora (o cliente recria alguns controles; ponteiro guardado de um
	// controle destruído derruba o jogo, porque o tratador de erros do cliente pega antes do __try)
	void LiveTree( uintptr_t w, std::set<uintptr_t>& out, int depth = 0 ){
		out.insert( w );
		if( depth > 2 ) return;
		ForChildren( w, [&]( uintptr_t c ){ LiveTree( c, out, depth + 1 ); } );
	}

	void RestoreOrig( uintptr_t w, std::vector<SelOrig>& list ){
		std::set<uintptr_t> live;
		LiveTree( w, live );
		for( const SelOrig& o : list ){
			if( !live.count( o.c ) ) continue;
			int cw = 0, ch = 0;
			Peek( o.c + client::OFS_WND_WIDTH, cw ); Peek( o.c + client::OFS_WND_HEIGHT, ch );
			if( o.button ){
				for( int k = 0; k < 4; k++ ) if( !o.img[k].empty() ) SelSetImage( o.c, o.img[k].c_str(), k );
				if( !o.text.empty() ) AssignString( o.c + 0x120, o.text.c_str() );
			}
			if( o.w > 0 && o.h > 0 && ( cw != o.w || ch != o.h ) ) ResizeWindowSurface( o.c, o.w, o.h );
			Place( o.c, o.x, o.y );
		}
		list.clear();
		CallInvalidate( w );
	}

	void SelRestyle( uintptr_t c, const char* normal, const char* over, int x, int y, int w, int h ){
		SelRemember( c );
		Restyle( c, normal, over, w, h );
		std::string dis; // desativado (+0x104): a mesma imagem (o cliente desativa o gênero escolhido, por exemplo)
		std::string want = std::string( UI ) + "codex\\" + normal;
		if( ReadStdString( c + OFS_BUTTON_IMAGE + 0x48, dis ) && dis != want ) SelSetImage( c, want.c_str(), 3 );
		CallInvalidate( c ); // o cliente pode ter redesenhado com a imagem dele antes da troca
		SelPut( c, x, y, w, h );
	}

}

void game::SubstituteSelectWindow( int W, int H, bool restore ){
	uintptr_t w = reinterpret_cast<uintptr_t>( client::SelectWindow() );
	if( w != g_selWnd ){ g_selWnd = w; g_selOrig.clear(); g_selRole.clear(); if( w ) ui::SelectEntered(); }
	if( !w ) return;
	if( restore ){
		if( !g_selOrig.empty() ) RestoreOrig( w, g_selOrig ); // posição, tamanho, imagens e texto originais
		return;
	}
	if( W < 640 || H < 480 ) return;
	SelPut( w, 0, 0, W, H );
	const ui::SelectGeo g = ui::SelectLayout( W, H );
	const chardata::Character* sel = chardata::Get( client::SelectedSlot() );
	ForChildren( w, [&]( uintptr_t c ){
		uintptr_t cvt = 0;
		Peek( c, cvt );
		std::string img;
		ReadStdString( c + OFS_BUTTON_IMAGE, img );
		auto has = [&]( const char* t ){ return img.find( t ) != std::string::npos; };
		if( cvt == 0x100EE28 ){ // linha da lista
			int card = client::SlotIndexOf( reinterpret_cast<void*>( c ) ), rx = 0, ry = 0;
			if( card >= 0 && ui::SelectRowPos( card, W, H, rx, ry ) ) SelPut( c, rx, ry, ui::SEL_ROW_W, ui::SEL_ROW_H );
			else SelPut( c, -10000, -10000, 0, 0 );
			ForChildren( c, [&]( uintptr_t cc ){ SelPut( cc, -10000, -10000, 0, 0 ); } );
			CallInvalidate( c );
		}else if( has( "bt_gameStart" ) || has( "codex\\sel_jogar" ) || has( "codex\\sel_criar" ) ){
			// slot vazio: o cliente desativa o botão (imagem de desativado); a própria linha abre a criação
			if( sel ) SelRestyle( c, "sel_jogar.bmp", "sel_jogar_on.bmp", g.listX, g.playY, ui::SEL_LIST_W, ui::SEL_PLAY_H );
			else SelPut( c, -10000, -10000, 0, 0 );
		}else if( has( "bt_info" ) || has( "codex\\sel_apagar" ) || has( "codex\\sel_cancelar_exc" ) ){
			if( !g_selRole.count( c ) ){
				int y = 0;
				Peek( c + client::OFS_WND_Y, y );
				g_selRole[c] = y >= 340 ? 2 : 1;
			}
			std::string t; // botão com texto próprio (vt 0x1009AC8, +0x120): o texto vem na imagem
			if( ReadStdString( c + 0x120, t ) && !t.empty() ){ SelRemember( c ); AssignString( c + 0x120, "" ); }
			const int by = H - ui::SEL_M - 38;
			if( !sel ) SelPut( c, -10000, -10000, 0, 0 );
			else if( g_selRole[c] == 2 ) SelRestyle( c, "sel_apagar_def.bmp", "sel_apagar_def_on.bmp", ui::SEL_M + 190 + g.offL, by, 180, 38 );
			else if( sel && sel->deleteDate != 0 ) SelRestyle( c, "sel_cancelar_exc.bmp", "sel_cancelar_exc_on.bmp", ui::SEL_M + g.offL, by, 180, 38 );
			else SelRestyle( c, "sel_apagar.bmp", "sel_apagar_on.bmp", ui::SEL_M + g.offL, by, 120, 38 );
		}else if( has( "bt_left" ) || has( "codex\\sel_esq" ) ){
			SelRestyle( c, "sel_esq.bmp", "sel_esq_on.bmp", g.rowX + ui::SEL_ROW_W - 44, g.top + 14, 20, 18 );
		}else if( has( "bt_right" ) || has( "codex\\sel_dir" ) ){
			SelRestyle( c, "sel_dir.bmp", "sel_dir_on.bmp", g.rowX + ui::SEL_ROW_W - 20, g.top + 14, 20, 18 );
		}else if( has( "bt_close2" ) || has( "codex\\sel_sair" ) ){
			SelRestyle( c, "sel_sair.bmp", "sel_sair_on.bmp", W - ui::SEL_M - 64, 6, 64, 22 );
		}else{
			SelPut( c, -10000, -10000, 0, 0 ); // nomes dos cartões, painel de informações, criar/apagar antigos
		}
	} );
	ui::SelectInput( W, H );
	CallInvalidate( w );
}

// ---------------------------------------------------------------- criação de personagem hardcoded
namespace {
	uintptr_t g_mkWnd = 0;
	std::vector<SelOrig> g_mkOrig;
	std::map<uintptr_t, int> g_mkRole; // bt_make: 1 = criar (x >= 300), 2 = cancelar
	int g_mkHairSel = 1; // penteado escolhido (o cliente marca o botão com bt_hairStyle_select)

	void MkRemember( uintptr_t c ){
		std::swap( g_selOrig, g_mkOrig ); // reaproveita SelRemember/SelPut com a lista da criação
		SelRemember( c );
		std::swap( g_selOrig, g_mkOrig );
	}
	void MkPut( uintptr_t c, int x, int y, int w, int h ){ std::swap( g_selOrig, g_mkOrig ); SelPut( c, x, y, w, h ); std::swap( g_selOrig, g_mkOrig ); }
	void MkRestyle( uintptr_t c, const char* n, const char* o, int x, int y, int w, int h ){
		std::swap( g_selOrig, g_mkOrig ); SelRestyle( c, n, o, x, y, w, h ); std::swap( g_selOrig, g_mkOrig );
	}
}

void game::SubstituteMakeWindow( int W, int H, bool restore ){
	uintptr_t w = reinterpret_cast<uintptr_t>( client::MakeWindow() );
	if( w != g_mkWnd ){ g_mkWnd = w; g_mkOrig.clear(); g_mkRole.clear(); g_mkHairSel = 1; }
	if( !w ) return;
	if( restore ){
		if( !g_mkOrig.empty() ) RestoreOrig( w, g_mkOrig );
		return;
	}
	if( W < 640 || H < 480 ) return;
	MkPut( w, 0, 0, W, H );
	const ui::MakeGeo g = ui::MakeLayout( W, H );
	client::MakeState st;
	client::ReadMakeState( st );
	float fx = W * 0.45f, fy = H * 0.6f, sc = 1.0f;
	scene::SlotToScreen( client::SelectedSlot(), fx, fy, sc );
	int color = 0;
	ForChildren( w, [&]( uintptr_t c ){
		uintptr_t cvt = 0;
		Peek( c, cvt );
		std::string img;
		ReadStdString( c + OFS_BUTTON_IMAGE, img );
		auto has = [&]( const char* t ){ return img.find( t ) != std::string::npos; };
		if( has( "bt_male" ) || has( "codex\\mk_masc" ) ){
			bool on = st.sex == 1;
			MkRestyle( c, on ? "mk_masc_sel.bmp" : "mk_masc.bmp", on ? "mk_masc_sel.bmp" : "mk_masc_on.bmp", g.px, g.genderY, 162, 32 );
		}else if( has( "bt_female" ) || has( "codex\\mk_fem" ) ){
			bool on = st.sex == 0;
			MkRestyle( c, on ? "mk_fem_sel.bmp" : "mk_fem.bmp", on ? "mk_fem_sel.bmp" : "mk_fem_on.bmp", g.px + 170, g.genderY, 162, 32 );
		}else if( cvt == 0xFFD958 ){ // penteado: o botão vira a célula e a cabeça (filho) fica no meio
			int style = 0;
			ForChildren( c, [&]( uintptr_t cc ){
				std::string ci;
				ReadStdString( cc + OFS_BUTTON_IMAGE, ci );
				size_t p = ci.find( "img_hairStyle" ); // img_hairStyleNN (masculino) ou img_hairStyle_girlNN
				if( p != std::string::npos ){
					p += 13;
					while( p < ci.size() && !isdigit( (unsigned char)ci[p] ) ) p++;
					style = atoi( ci.c_str() + p );
				}
				MkPut( cc, 6, 5, 0, 0 );
			} );
			if( style <= 0 ){ MkPut( c, -10000, -10000, 0, 0 ); return; }
			int k = style - 1, cx = g.px + ( k % 6 ) * ( ui::MK_CELL + ui::MK_CELL_GAP ), cy = g.hairY + ( k / 6 ) * ( ui::MK_CELL + ui::MK_CELL_GAP );
			if( has( "bt_hairStyle_select" ) ) g_mkHairSel = style;
			bool on = g_mkHairSel == style;
			MkRestyle( c, on ? "mk_cabelo_sel.bmp" : "mk_cabelo.bmp", on ? "mk_cabelo_sel.bmp" : "mk_cabelo_on.bmp", cx, cy, ui::MK_CELL, ui::MK_CELL );
		}else if( cvt == 0x100E814 ){ // cores (o cliente desenha o quadradinho)
			MkPut( c, g.px + color * ( ui::MK_SWATCH + 11 ), g.colorY, 0, 0 );
			color++;
		}else if( cvt == 0x1008AE4 ){ // nome
			MkPut( c, g.px + 10, g.nameY + 7, g.pw - 20, 20 );
		}else if( has( "bt_leftTurn" ) || has( "codex\\mk_girar_esq" ) ){
			MkRestyle( c, "mk_girar_esq.bmp", "mk_girar_esq_on.bmp", (int)( fx - std::max( 70.0f, 60.0f * sc ) ) - 17, (int)( fy - 45.0f * sc ) - 17, 34, 34 );
		}else if( has( "bt_rightTurn" ) || has( "codex\\mk_girar_dir" ) ){
			MkRestyle( c, "mk_girar_dir.bmp", "mk_girar_dir_on.bmp", (int)( fx + std::max( 70.0f, 60.0f * sc ) ) - 17, (int)( fy - 45.0f * sc ) - 17, 34, 34 );
		}else if( has( "bt_make" ) || has( "codex\\mk_criar" ) || has( "codex\\mk_voltar" ) ){
			if( !g_mkRole.count( c ) ){
				int x = 0;
				Peek( c + client::OFS_WND_X, x );
				g_mkRole[c] = x >= 300 ? 1 : 2;
			}
			std::string t;
			if( ReadStdString( c + 0x120, t ) && !t.empty() ){ MkRemember( c ); AssignString( c + 0x120, "" ); }
			if( g_mkRole[c] == 1 ) MkRestyle( c, "mk_criar.bmp", "mk_criar_on.bmp", g.rx, H - ui::SEL_M - 60, ui::MK_RIGHT_W, 60 );
			else MkRestyle( c, "mk_voltar.bmp", "mk_voltar_on.bmp", ui::SEL_M, H - ui::SEL_M - 36, 180, 36 );
		}else{
			MkPut( c, -10000, -10000, 0, 0 ); // fechar, raças, rótulos
		}
	} );
	CallInvalidate( w );
}

std::wstring game::NpcName(){
	std::lock_guard<std::mutex> g( g_npcLock );
	return g_npcName;
}

void game::NpcDialogClosed(){
	std::lock_guard<std::mutex> g( g_npcLock );
	g_npcFresh = true;
	g_npcName.clear();
	g_npcCutin.clear();
	g_npcWait = 0;
}

std::string game::NpcCutin(){
	std::lock_guard<std::mutex> g( g_npcLock );
	return g_npcCutin;
}

bool game::NpcSayOpen(){
	return ShownWindow( 0x1013184 ) != 0;
}

// ---------------------------------------------------------------- conversa com NPC
namespace {
	bool g_npcClose = false, g_npcHasButton = false; // botão da faixa: fechar (senão seguir)
	uintptr_t g_npcPress = 0;                          // esse botão
	bool g_game_npcSayOpen = false;                    // fala aberta (o menu fica acima dela)

	void PressButton( uintptr_t w, uintptr_t btn ){
		__try{
			uintptr_t vt = *reinterpret_cast<uintptr_t*>( w );
			int id = *reinterpret_cast<int*>( btn + 0x2C ), state = *reinterpret_cast<int*>( btn + 0x30 );
			reinterpret_cast<int( __thiscall* )( uintptr_t, uintptr_t, int, int, int, int, int )>( *reinterpret_cast<uintptr_t*>( vt + 37 * 4 ) )( w, btn, 6, id, state, 0, 0 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
	}
}

namespace {
	int g_npcScrW = 1600, g_npcScrH = 900, g_npcBottom = 892; // tela do último quadro (as janelas nascem entre quadros)

	void NpcSayPos( int& x, int& y ){ x = ( g_npcScrW - ui::SAY_W ) / 2; y = g_npcBottom - ui::SAY_H; }

	void NpcChoosePos( int& x, int& y ){
		// menu com altura fixa (cabe ui::CHOICE_ROWS opções); o painel é desenhado de baixo para cima
		const int mh = ui::CHOICE_ROWS * ui::CHOICE_ROW_H + 24;
		int sx, sy;
		NpcSayPos( sx, sy );
		if( g_game_npcSayOpen ){ x = sx + ui::SAY_W - ui::CHOICE_W - 18; y = sy + ui::SAY_BAR_Y - mh - 26; }
		else{ x = ( g_npcScrW - ui::CHOICE_W ) / 2; y = ( g_npcScrH - mh ) / 2; }
	}

	uintptr_t ChoiceList( uintptr_t ch, uintptr_t* ok = nullptr ){
		uintptr_t list = 0;
		ForChildren( ch, [&]( uintptr_t c ){
			uintptr_t cvt = 0;
			Peek( c, cvt );
			std::string img;
			ReadStdString( c + OFS_BUTTON_IMAGE, img );
			if( cvt == 0x100C2A8 ) list = c;
			else if( ok && img.find( "btn_ok" ) != std::string::npos ) *ok = c;
		} );
		return list;
	}
}

int game::NpcChoiceRows( uintptr_t list ){
	uintptr_t b = 0, e = 0;
	Peek( list + 0x88, b ); Peek( list + 0x8C, e );
	const int count = e > b ? (int)( ( e - b ) / 0x18 ) : 0;
	return std::max( 1, std::min( ui::CHOICE_ROWS, count ) );
}

void game::NpcCreated( uintptr_t w, bool say ){
	// recém-criada pelo cliente: já no tamanho e no lugar do tema (o primeiro desenho dela já sai certo)
	int x, y;
	if( say ){
		g_game_npcSayOpen = true;
		ResizeWindowSurface( w, ui::SAY_W, ui::SAY_H );
		NpcSayPos( x, y );
	}else{
		ResizeWindowSurface( w, ui::CHOICE_W, ui::CHOICE_ROWS * ui::CHOICE_ROW_H + 24 );
		NpcChoosePos( x, y );
	}
	Place( w, x, y );
}

void game::NpcLayoutSay( uintptr_t say ){
	int btn = 0;
	uintptr_t press = 0;
	ForChildren( say, [&]( uintptr_t c ){
		uintptr_t cvt = 0;
		Peek( c, cvt );
		if( cvt == 0x1007AB0 ){ // texto: à direita do retrato (sem retrato: à esquerda), fundo do tema
			int cx = 0, cy = 0;
			const int tx = ui::NpcPortrait( NpcName() ).empty() ? 32 : ui::SAY_TEXT_X;
			Peek( c + client::OFS_WND_X, cx ); Peek( c + client::OFS_WND_Y, cy );
			if( cx != tx || cy != ui::SAY_BAR_Y + 26 ){ Place( c, tx, ui::SAY_BAR_Y + 26 ); CallInvalidate( c ); }
			uint32_t bg = 0;
			if( Peek( c + 0x8C, bg ) && bg != 0xFFE7EFEF ){ Poke( c + 0x8C, 0xFFE7EFEFu ); CallInvalidate( c ); } // (231, 239, 239) em 0xAABBGGRR
		}else{ // botões (seguir / fechar): no tema, canto de baixo à direita; só o que o servidor pediu agora
			static std::map<uintptr_t, bool> s_close; // papel de cada botão (o texto do cliente é apagado na 1ª vez)
			std::string t, img;
			ReadStdString( c + 0x120, t ); ReadStdString( c + OFS_BUTTON_IMAGE, img );
			if( cvt == 0x1009AC8 && !t.empty() ){ s_close[c] = t.find( "Fech" ) != std::string::npos || t.find( "Clos" ) != std::string::npos; AssignString( c + 0x120, "" ); }
			else if( !s_close.count( c ) ) s_close[c] = img.find( "close" ) != std::string::npos || img.find( "npc_fechar" ) != std::string::npos;
			const bool close = s_close[c];
			const int wait = g_npcWait;
			if( wait == 0 || ( wait == 2 ) != close ){
				int cx = 0;
				if( Peek( c + client::OFS_WND_X, cx ) && cx != -10000 ) Place( c, -10000, -10000 );
				return;
			}
			if( close ) Restyle( c, "npc_fechar.bmp", "npc_fechar_on.bmp", 120, 30 );
			else Restyle( c, "npc_seguir.bmp", "npc_seguir_on.bmp", 120, 30 );
			g_npcClose = close;
			int bx = ui::SAY_W - 24 - 120, by = ui::SAY_BAR_Y + ui::SAY_BAR_H - 44, cx = 0, cy = 0;
			Peek( c + client::OFS_WND_X, cx ); Peek( c + client::OFS_WND_Y, cy );
			if( cx != bx || cy != by ){ Place( c, bx, by ); CallInvalidate( c ); }
			if( !press ) press = c;
			btn++;
		}
	} );
	g_npcPress = press;
	g_npcHasButton = press != 0;
}

void game::NpcLayoutChoose( uintptr_t ch ){
	uintptr_t list = ChoiceList( ch );
	if( !list ) return;
	uintptr_t b = 0, e = 0;
	Peek( list + 0x88, b ); Peek( list + 0x8C, e );
	const int count = e > b ? (int)( ( e - b ) / 0x18 ) : 0, rows = NpcChoiceRows( list );
	const int listH = rows * ui::CHOICE_ROW_H, H = ui::CHOICE_ROWS * ui::CHOICE_ROW_H + 24;
	ForChildren( ch, [&]( uintptr_t c ){
		if( c == list ){
			int lw = 0, lh = 0, rh = 0, lx = 0, ly = 0;
			Peek( c + client::OFS_WND_WIDTH, lw ); Peek( c + client::OFS_WND_HEIGHT, lh ); Peek( c + 0xB8, rh );
			if( rh != ui::CHOICE_ROW_H ) Poke( c + 0xB8, ui::CHOICE_ROW_H );
			if( lw != ui::CHOICE_W - 24 || lh != listH ) ResizeWindowSurface( c, ui::CHOICE_W - 24, listH );
			Peek( c + client::OFS_WND_X, lx ); Peek( c + client::OFS_WND_Y, ly );
			if( lx != 12 || ly != H - 12 - listH ) Place( c, 12, H - 12 - listH ); // encostada embaixo (o painel cresce para cima)
			ForChildren( c, [&]( uintptr_t sb ){ if( count <= rows ) Place( sb, -10000, -10000 ); } ); // rolagem só se não cabe
		}else{
			Place( c, -10000, -10000 ); // OK/Cancelar: a escolha é o clique na opção
		}
	} );
}

void game::SubstituteNpcWindows( int W, int H ){
	if( W < 640 || H < 480 ) return;
	// acima da barra de atalhos (centro, embaixo)
	int bottom = H - 8;
	{
		uintptr_t sc = OpenWindow( 0x10174D4 );
		int sy = 0, show = 0;
		if( sc && Peek( sc + OFS_WND_SHOW, show ) && show && Peek( sc + client::OFS_WND_Y, sy ) && sy > H / 2 ) bottom = sy - 8;
	}
	g_npcScrW = W; g_npcScrH = H; g_npcBottom = bottom;
	static uintptr_t s_say = 0;
	uintptr_t say = ShownWindow( 0x1013184 );
	if( say != s_say ){ if( !say ) NpcDialogClosed(); s_say = say; }
	g_game_npcSayOpen = say != 0;
	int sayX, sayY;
	NpcSayPos( sayX, sayY );
	if( say ){
		int cw = 0, ch = 0, x = 0, y = 0;
		Peek( say + client::OFS_WND_WIDTH, cw ); Peek( say + client::OFS_WND_HEIGHT, ch );
		if( cw != ui::SAY_W || ch != ui::SAY_H ) ResizeWindowSurface( say, ui::SAY_W, ui::SAY_H );
		Peek( say + client::OFS_WND_X, x ); Peek( say + client::OFS_WND_Y, y );
		if( x != sayX || y != sayY ) Place( say, sayX, sayY );
		NpcLayoutSay( say );
		CallInvalidate( say );
		// clique em qualquer ponto da faixa = o botão (como "Clique para continuar")
		static bool s_down = false, s_in = false;
		bool down = input::IsLeftDown();
		input::Point m = input::RealCursor();
		bool in = m.x >= sayX && m.x < sayX + ui::SAY_W && m.y >= sayY + ui::SAY_BAR_Y && m.y < sayY + ui::SAY_BAR_Y + ui::SAY_BAR_H;
		if( down && !s_down ) s_in = in;
		uintptr_t press = g_npcPress;
		if( !down && s_down && s_in && in && press ){
			int bx = 0, by = 0, bw = 0, bh = 0;
			Peek( press + client::OFS_WND_X, bx ); Peek( press + client::OFS_WND_Y, by ); Peek( press + client::OFS_WND_WIDTH, bw ); Peek( press + client::OFS_WND_HEIGHT, bh );
			bool onBtn = m.x >= sayX + bx && m.x < sayX + bx + bw && m.y >= sayY + by && m.y < sayY + by + bh; // o próprio botão já trata
			if( !onBtn ) PressButton( say, press );
		}
		s_down = down;
	}
	// menu
	uintptr_t ch = ShownWindow( 0x102A074 );
	if( !ch ) ch = ShownWindow( 0x101325C );
	if( !ch ) return;
	uintptr_t ok = 0, list = ChoiceList( ch, &ok );
	if( !list ) return;
	const int mh = ui::CHOICE_ROWS * ui::CHOICE_ROW_H + 24;
	int mx, my, cw = 0, chh = 0, x = 0, y = 0;
	NpcChoosePos( mx, my );
	Peek( ch + client::OFS_WND_WIDTH, cw ); Peek( ch + client::OFS_WND_HEIGHT, chh );
	if( cw != ui::CHOICE_W || chh != mh ) ResizeWindowSurface( ch, ui::CHOICE_W, mh );
	Peek( ch + client::OFS_WND_X, x ); Peek( ch + client::OFS_WND_Y, y );
	if( x != mx || y != my ) Place( ch, mx, my );
	NpcLayoutChoose( ch );
	CallInvalidate( ch );
	CallInvalidate( list );
	// clique numa opção: escolhe (o cliente já marcou a linha; o OK confirma)
	static bool s_down = false;
	static int s_press = -1;
	bool down = input::IsLeftDown();
	int hover = ui::ChoiceHover();
	if( down && !s_down ) s_press = hover;
	if( !down && s_down && s_press >= 0 && s_press == hover && ok ){
		Poke( list + 0x94, hover );
		PressButton( ch, ok );
	}
	if( !down ) s_press = -1;
	s_down = down;
}

int game::NpcButton(){
	return !g_npcHasButton ? 0 : g_npcClose ? 2 : 1;
}

void game::NpcPlace( uintptr_t w, bool say ){
	// posição do tema (o cliente pode mover a janela para a última posição salva depois do OnCreate)
	int x, y, cx = 0, cy = 0;
	if( say ){ g_game_npcSayOpen = true; NpcSayPos( x, y ); }
	else NpcChoosePos( x, y );
	Peek( w + client::OFS_WND_X, cx ); Peek( w + client::OFS_WND_Y, cy );
	if( cx != x || cy != y ) Place( w, x, y );
}

// ---------------------------------------------------------------- lojas de NPC
void game::ShopLayout( uintptr_t w, int kind ){
	int W = 0, H = 0;
	Peek( w + client::OFS_WND_WIDTH, W ); Peek( w + client::OFS_WND_HEIGHT, H );
	const bool compact = kind == ui::SHOP_CART; // carrinho: botões menores (o total fica à esquerda)
	const int BW = compact ? 64 : 76, BH = 24, by = kind < 0 ? H - 34 : H - BH - 6;
	bool hasSell = false;
	ForChildren( w, [&]( uintptr_t c ){
		std::string img;
		ReadStdString( c + OFS_BUTTON_IMAGE, img );
		if( img.find( "btn_sell" ) != std::string::npos || img.find( "shop_vender" ) != std::string::npos ) hasSell = true;
	} );
	ForChildren( w, [&]( uintptr_t c ){
		uintptr_t cvt = 0;
		Peek( c, cvt );
		std::string img;
		ReadStdString( c + OFS_BUTTON_IMAGE, img );
		auto put = [&]( const char* n, const char* o, int x ){
			std::string nn = n, oo = o;
			if( compact ){ nn.insert( nn.size() - 4, "_p" ); oo.insert( oo.size() - 7, "_p" ); }
			Restyle( c, nn.c_str(), oo.c_str(), BW, BH );
			int cx = 0, cy = 0;
			Peek( c + client::OFS_WND_X, cx ); Peek( c + client::OFS_WND_Y, cy );
			if( cx != x || cy != by ){ Place( c, x, by ); CallInvalidate( c ); }
		};
		const int r1 = W - 12 - BW, r2 = r1 - BW - 6, r3 = r2 - BW - 6;
		if( img.find( "btn_cancel" ) != std::string::npos || img.find( "shop_cancelar" ) != std::string::npos ) put( "shop_cancelar.bmp", "shop_cancelar_on.bmp", r1 );
		else if( img.find( "btn_sell" ) != std::string::npos || img.find( "shop_vender" ) != std::string::npos ) put( "shop_vender.bmp", "shop_vender_on.bmp", r2 );
		else if( img.find( "btn_buy" ) != std::string::npos || img.find( "shop_comprar" ) != std::string::npos ) put( "shop_comprar.bmp", "shop_comprar_on.bmp", hasSell ? r3 : r2 );
		else if( cvt == 0x1009160 && kind == ui::SHOP_CART ){ // "Total : ..." embaixo: largura até os botões
			int cy = 0, cww = 0, chh = 0;
			Peek( c + client::OFS_WND_Y, cy ); Peek( c + client::OFS_WND_WIDTH, cww ); Peek( c + client::OFS_WND_HEIGHT, chh );
			const int tw = ( W - 12 - BW - 6 - BW ) - 12 - 6;
			if( cy > H - 40 && cww != tw ) ResizeWindowSurface( c, tw, chh );
		}
		else if( cvt == 0x100D734 && kind < 0 ){ // texto da escolha comprar/vender: sob o título, fundo do tema
			int cx = 0, cy = 0, cww = 0, chh = 0;
			uint32_t bg = 0;
			Peek( c + client::OFS_WND_X, cx ); Peek( c + client::OFS_WND_Y, cy ); Peek( c + client::OFS_WND_WIDTH, cww ); Peek( c + client::OFS_WND_HEIGHT, chh );
			if( cww != W - 24 || chh != by - 34 ) ResizeWindowSurface( c, W - 24, by - 34 );
			if( cx != 12 || cy != 30 ) Place( c, 12, 30 );
			if( Peek( c + 0xE0, bg ) && bg != 0xFFE7EFEF ){ Poke( c + 0xE0, 0xFFE7EFEFu ); CallInvalidate( c ); }
			ForChildren( c, [&]( uintptr_t sb ){ Place( sb, -10000, -10000 ); } ); // rolagem: texto curto
		}
		else if( cvt == 0x10089F8 ){ // campo (pontos promocionais): fundo branco
			int r = 0;
			Peek( c + 0x90, r );
			if( r != 255 ){ Poke( c + 0x90, 255 ); Poke( c + 0x94, 255 ); Poke( c + 0x98, 255 ); CallInvalidate( c ); }
		}
	} );
}

void game::MerchantFields( uintptr_t w ){
	ForChildren( w, [&]( uintptr_t c ){
		uintptr_t cvt = 0;
		Peek( c, cvt );
		if( cvt != 0x10089F8 && cvt != 0x101C64C ) return; // UIEditCtrl / UIEditCtrlWheel
		int r = 0;
		Peek( c + 0x90, r );
		if( r != 255 ){ Poke( c + 0x90, 255 ); Poke( c + 0x94, 255 ); Poke( c + 0x98, 255 ); CallInvalidate( c ); }
	} );
}

namespace {
	// opções (UIRadioBtn, vt 0x100A278) como botões do tema na própria janela: o controle aceita imagens próprias
	// (+0xB8 marcada, +0xD0 desmarcada; desenhadas em (0, centro) e o texto +0x7C ao lado). Recebe as do botão com
	// o nome escrito, fica sem o texto e do tamanho do botão (a área de clique passa a ser o botão inteiro)
	constexpr uintptr_t VT_RADIO = 0x100A278;
	struct OptionButton { const char* text; const char* img; int x, y, w, h; }; // y < 0: a partir de baixo
	const char* const UI_DIR = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA\\codex\\";

	int ClientTextWidth( uintptr_t wnd, const char* s, int len, int height, int bold ){
		__try{
			return reinterpret_cast<int( __thiscall* )( uintptr_t, const char*, int, int, int, int, int )>( 0xA0AAD0 )(
				wnd, s, len, 0, height, bold, 0 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){ return 0; }
	}

	void StyleOptions( uintptr_t w, const OptionButton* list, int n ){
		int wh = 0;
		Peek( w + client::OFS_WND_HEIGHT, wh );
		bool moved = false;
		ForChildren( w, [&]( uintptr_t c ){
			uintptr_t cvt = 0;
			if( !Peek( c, cvt ) || cvt != VT_RADIO ) return;
			std::string text, on;
			ReadStdString( c + 0x7C, text );
			ReadStdString( c + 0xB8, on );
			const OptionButton* o = nullptr;
			for( int i = 0; i < n && !o; i++ ){
				if( on.find( std::string( "codex\\" ) + list[i].img + "_on.bmp" ) != std::string::npos ) o = &list[i];
				else if( !text.empty() && _strnicmp( text.c_str(), list[i].text, strlen( list[i].text ) ) == 0 ) o = &list[i];
			}
			if( !o ) return;
			const std::string base = std::string( UI_DIR ) + o->img;
			AssignString( c + 0xB8, ( base + "_on.bmp" ).c_str() );
			AssignString( c + 0xD0, ( base + ".bmp" ).c_str() );
			if( !text.empty() ) AssignString( c + 0x7C, "" );
			const int y = o->y < 0 ? wh + o->y : o->y;
			int cx = 0, cy = 0, cw = 0, ch = 0;
			Peek( c + client::OFS_WND_X, cx ); Peek( c + client::OFS_WND_Y, cy );
			Peek( c + client::OFS_WND_WIDTH, cw ); Peek( c + client::OFS_WND_HEIGHT, ch );
			if( cw != o->w || ch != o->h ) ResizeWindowSurface( c, o->w, o->h );
			if( cx != o->x || cy != y ){ Place( c, o->x, y ); moved = true; }
			if( on.find( "codex\\" ) == std::string::npos || !text.empty() ) CallInvalidate( c );
		} );
		if( moved ) CallInvalidate( w ); // sem isso fica a imagem velha do controle no lugar antigo
	}
}

void game::OptionButtons(){
	// Amigos / Grupo (Alt+H, Alt+Z): faixa de 16 px embaixo da barra de ícones (o cliente os põe em 4 e 64, h-16)
	static const OptionButton messenger[] = {
		{ "Amig", "op_amigos", 6, -19, 62, 17 }, { "Frien", "op_amigos", 6, -19, 62, 17 },
		{ "Grup", "op_grupo", 71, -19, 62, 17 }, { "Party", "op_grupo", 71, -19, 62, 17 } };
	// Rodex, busca por assunto / remetente: à esquerda do campo (133, 65, 132x18)
	static const OptionButton rodex[] = {
		{ "Assu", "op_assunto", 10, 65, 58, 18 }, { "Titl", "op_assunto", 10, 65, 58, 18 },
		{ "Reme", "op_remetente", 71, 65, 58, 18 }, { "Send", "op_remetente", 71, 65, 58, 18 } };
	if( uintptr_t w = ShownWindow( 0xFF0B9C ) ) StyleOptions( w, messenger, 4 );
	if( uintptr_t w = ShownWindow( 0x1002260 ) ){
		StyleOptions( w, rodex, 4 );
		// abas (vt 0x100E814): o texto vem de uma lista de estilos por estado (+0x100..+0x104, 0x1C cada: +0 estado
		// 3 escolhida / 2 mouse em cima, +0x10 negrito, +0x14 cor, +0x18 sombra); cores do tema e sem sombra
		// a posição (+4 x) também vem do estilo, fixa: x centralizado com a medida do próprio cliente (0xA0AAD0:
		// texto, tamanho, fonte, altura, negrito, 0)
		ForChildren( w, [&]( uintptr_t c ){
			uintptr_t cvt = 0, b = 0, e = 0;
			if( !Peek( c, cvt ) || cvt != 0x100E814 || !Peek( c + 0x100, b ) || !Peek( c + 0x104, e ) ) return;
			std::string nome;
			int tw = 0;
			ReadStdString( c + 0x10C, nome );
			Peek( c + client::OFS_WND_WIDTH, tw );
			bool changed = false;
			for( int i = 0; b && b < e && i < 8; b += 0x1C, i++ ){
				int st = 0, x = 0, alt = 12;
				uint32_t cor = 0;
				uint8_t sombra = 0, negrito = 0;
				Peek( b, st ); Peek( b + 4, x ); Peek( b + 0xC, alt ); Peek( b + 0x10, negrito ); Peek( b + 0x14, cor ); Peek( b + 0x18, sombra );
				const uint32_t quer = st >= 2 ? RGB( 82, 114, 176 ) : RGB( 110, 110, 104 );
				if( cor != quer || sombra ){ Poke( b + 0x14, quer ); Poke( b + 0x18, (uint8_t)0 ); changed = true; }
				if( !nome.empty() && tw > 0 ){
					const int larg = ClientTextWidth( c, nome.c_str(), (int)nome.size(), alt, negrito );
					const int cx = ( tw - larg - ( negrito ? 1 : 0 ) ) / 2;
					if( larg > 0 && larg < tw + 40 && x != cx ){ Poke( b + 4, cx ); changed = true; }
				}
			}
			if( changed ) CallInvalidate( c );
		} );
	}
}

void game::RodexFields(){
	// campos de texto do Rodex (escrever carta): fundo branco em vez do azul (+0x90/+0x94/+0x98 = R, G, B)
	for( uintptr_t vt : { (uintptr_t)0x1001C20, (uintptr_t)0x10020AC } ){ // escrever e ler
	uintptr_t w = ShownWindow( vt );
	if( !w ) continue;
	ForChildren( w, [&]( uintptr_t c ){
		uintptr_t cvt = 0;
		Peek( c, cvt );
		if( cvt != 0x1001B2C && cvt != 0x100E8EC && cvt != 0x100E9D8 && cvt != 0x10089F8 ) return;
		int r = 0, g = 0;
		Peek( c + 0x90, r ); Peek( c + 0x94, g );
		if( r != 255 || g != 255 ){ Poke( c + 0x90, 255 ); Poke( c + 0x94, 255 ); Poke( c + 0x98, 255 ); CallInvalidate( c ); }
	} );
	}
}

bool game::ResizeSurface( uintptr_t w, int width, int height ){
	return ResizeWindowSurface( w, width, height );
}
