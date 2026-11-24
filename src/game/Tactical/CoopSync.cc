#include "CoopSync.h"

#include "Animation_Control.h"
#include "Animation_Data.h"
#include "Coop.h"
#include "CoopNet.h"
#include "Font_Control.h"
#include "Game_Clock.h"
#include "GameScreen.h"
#include "Handle_Doors.h"
#include "Handle_Items.h"
#include "Handle_UI.h"
#include "Interactive_Tiles.h"
#include "Interface.h"
#include "Interface_Items.h"
#include "Interface_Control.h"
#include "GameVersion.h"
#include "LoadSaveRottingCorpse.h"
#include "LoadSaveSoldierType.h"
#include "Soldier_Add.h"
#include "Overhead.h"
#include "Soldier_Create.h"
#include "TeamTurns.h"
#include "SaveLoadMap.h"
#include "SmokeEffects.h"
#include "Smell.h"
#include "Lighting.h"
#include "Isometric_Utils.h"
#include "Dialogue_Control.h"
#include "Explosion_Control.h"
#include "Faces.h"
#include "Interface_Dialogue.h"
#include "Soldier_Profile.h"
#include "LOS.h"
#include "OppList.h"
#include "Sound_Control.h"
#include "Weapons.h"
#include "Items.h"
#include "ContentManager.h"
#include "GameInstance.h"
#include "WeaponModels.h"
#include "LoadSaveData.h"
#include "LoadSaveObjectType.h"
#include "Isometric_Utils.h"
#include "Logger.h"
#include "Map_Information.h"
#include "Message.h"
#include "Options_Screen.h"
#include "Overhead.h"
#include "RenderWorld.h"
#include "SaveLoadGame.h"
#include "SaveLoadScreen.h"
#include "ScreenIDs.h"
#include "Soldier.h"
#include "Soldier_Control.h"
#include "Soldier_Macros.h"
#include "StrategicMap.h"
#include "Structure.h"
#include "Timer_Control.h"
#include "World_Items.h"
#include "WorldDef.h"

#include <string_theory/format>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern ScreenID guiCurrentScreen;
extern ScreenID guiScreenToGotoAfterLoadingSavedGame;

namespace
{
	enum : UINT8
	{
		S_STATE = 16,   // host -> guests: soldiers and turn
		S_MOVE,         // guest -> host
		S_ATTACK,
		S_STANCE,
		S_TURN,
		S_END_TURN,
		S_RESYNC,       // guest -> host: please send the whole game again
		S_INVENTORY,    // both ways: a merc's whole inventory after it changed
		S_SOLDIER,      // host -> guests: a whole soldier, new in the sector
		S_WANT_SOLDIER, // guest -> host: send me that soldier
		S_ITEM,         // host -> guests: a world item slot as it is now
		S_ITEM_DROP,    // guest -> host: put this item down here
		S_ITEM_TAKE,    // guest -> host: this world item was picked up
		S_CORPSE,       // host -> guests: a new corpse
		S_DOOR,         // host -> guests: a door is now open or closed
		S_SIGHT,        // host -> guests: who on our side sees whom
		S_SHOT,         // host -> guests: a bullet flies (for show and sound)
		S_SPEECH,       // host -> guests: a character says a quote
		S_BATTLE_SOUND, // host -> guests: a soldier's grunt, scream, ...
		S_EXPLOSION,    // host -> guests: something blows up (for show)
		S_TALK,         // guest -> host: start this conversation
		S_MAP_CHANGE,   // host -> guests: a wall, window, ... changed (MODIFY_MAP)
		S_SMOKE,        // host -> guests: a smoke/gas tile appears or goes
		S_BLOOD,        // host -> guests: blood on the ground
		S_MISS,         // host -> guests: a bullet hits the ground or a wall
		S_TALK_OPEN,    // host -> guests: a conversation panel opens
		S_TALK_CLOSE,   // both ways: the conversation panel closes
		S_TALK_CHOICE,  // guest -> host: a choice in the conversation panel
	};

	// while the guest carries out what the host says, its own hooks keep quiet
	bool g_applying = false;

	constexpr size_t SOLDIER_RECORD_SIZE = 2328; // SOLDIERTYPE in the save format


	constexpr UINT32 STATE_INTERVAL_MS    = 50; // 20 a second: walking stays smooth
	constexpr UINT32 RESYNC_MIN_GAP_MS    = 3000;
	constexpr UINT32 SNAPSHOT_MIN_GAP_MS  = 1500;
	constexpr UINT32 INVENTORY_CHECK_MS   = 250;
	constexpr size_t OBJECT_BYTES         = 36; // one OBJECTTYPE in the save format

	// ---- inventories ----------------------------------------------------------
	// Items are handled on either side (the guest reloads, swaps and attaches
	// on its own screen; the host's shots use up ammo), so a merc's inventory
	// goes over whole whenever it changes where its owner can change it.

	uint64_t g_inv_seen[TOTAL_SOLDIERS] = {};
	UINT32   g_last_inv_check = 0;

	std::vector<UINT8> PackInventory(SOLDIERTYPE const& s)
	{
		std::vector<UINT8> b(NUM_INV_SLOTS * OBJECT_BYTES);
		DataWriter d{b.data()};
		for (OBJECTTYPE const& o : s.inv) InjectObject(d, &o);
		return b;
	}

	uint64_t Hash(std::vector<UINT8> const& b)
	{
		uint64_t h = 1469598103934665603ULL;
		for (UINT8 const c : b) { h ^= c; h *= 1099511628211ULL; }
		return h;
	}

	void SendInventoryIfChanged(SOLDIERTYPE const& s, bool const to_host)
	{
		std::vector<UINT8> const inv = PackInventory(s);
		uint64_t const h = Hash(inv);
		if (h == g_inv_seen[s.ubID]) return;
		g_inv_seen[s.ubID] = h;

		std::vector<UINT8> msg;
		msg.push_back(s.ubID);
		msg.insert(msg.end(), inv.begin(), inv.end());
		if (to_host) CoopNetSendToHost(S_INVENTORY, msg);
		else         CoopNetSendToGuests(S_INVENTORY, msg);
	}

	void ApplyInventory(SOLDIERTYPE& s, UINT8 const* const p, size_t const len)
	{
		if (len != NUM_INV_SLOTS * OBJECT_BYTES) return;
		DataReader d{p};
		for (OBJECTTYPE& o : s.inv) ExtractObject(d, &o);
		g_inv_seen[s.ubID] = Hash(std::vector<UINT8>(p, p + len)); // do not echo it back
		DirtyMercPanelInterface(&s, DIRTYLEVEL2);
		fInterfacePanelDirty = DIRTYLEVEL2;
	}

	// guest: our own mercs, and only while no item hangs on the cursor (that
	// one would look lost until it is put down)
	void CheckInventories(bool const host)
	{
		UINT32 const now = GetJA2Clock();
		if (now - g_last_inv_check < INVENTORY_CHECK_MS) return;
		g_last_inv_check = now;
		if (!host && gpItemPointer) return;

		for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
		{
			SOLDIERTYPE const& s = GetMan(i);
			if (!s.bActive || s.bTeam != OUR_TEAM) continue;
			if (!host && !CoopIsLocalMerc(s)) continue;
			SendInventoryIfChanged(s, !host);
		}
	}

	// ---- byte packing -------------------------------------------------------

	struct Writer
	{
		std::vector<UINT8> b;
		template<typename T> Writer& put(T const v)
		{
			UINT8 raw[sizeof(T)];
			std::memcpy(raw, &v, sizeof(T)); // all players run on little-endian PCs
			b.insert(b.end(), raw, raw + sizeof(T));
			return *this;
		}
	};

	struct Reader
	{
		UINT8 const* p;
		size_t left;
		bool ok = true;
		template<typename T> T get()
		{
			T v{};
			if (left < sizeof(T)) { ok = false; return v; }
			std::memcpy(&v, p, sizeof(T));
			p += sizeof(T);
			left -= sizeof(T);
			return v;
		}
	};

	// ---- host: whole soldiers, items, corpses, doors -------------------------

	bool g_guests_have[TOTAL_SOLDIERS] = {};
	std::vector<INT32> g_items_changed;

	void PutSector(Writer& w)
	{
		w.put<INT16>(gWorldSector.x).put<INT16>(gWorldSector.y).put<INT8>(gWorldSector.z);
	}

	void SendSoldier(SOLDIERTYPE const& s)
	{
		Writer w;
		PutSector(w);
		w.put<UINT8>(s.ubID);
		std::vector<UINT8> rec(SOLDIER_RECORD_SIZE);
		InjectSoldierType(rec.data(), &s);
		w.b.insert(w.b.end(), rec.begin(), rec.end());
		CoopNetSendToGuests(S_SOLDIER, w.b);
		SLOGD("co-op: sent soldier {} ({})", s.ubID, s.name);
	}

	void SendChangedItems()
	{
		if (g_items_changed.empty()) return;
		std::vector<INT32> changed;
		changed.swap(g_items_changed);
		std::sort(changed.begin(), changed.end());
		changed.erase(std::unique(changed.begin(), changed.end()), changed.end());

		for (INT32 const i : changed)
		{
			if (i < 0) continue;
			Writer w;
			PutSector(w);
			w.put<INT32>(i);
			bool const exists = static_cast<size_t>(i) < gWorldItems.size() && gWorldItems[i].fExists;
			w.put<UINT8>(exists ? 1 : 0);
			if (exists)
			{
				WORLDITEM const& wi = gWorldItems[i];
				w.put<INT16>(wi.sGridNo).put<UINT8>(wi.ubLevel).put<UINT16>(wi.usFlags);
				w.put<INT8>(wi.bRenderZHeightAboveLevel).put<INT8>(wi.bVisible);
				UINT8 obj[OBJECT_BYTES];
				DataWriter d{obj};
				InjectObject(d, &wi.o);
				w.b.insert(w.b.end(), obj, obj + OBJECT_BYTES);
			}
			CoopNetSendToGuests(S_ITEM, w.b);
		}
	}

	// ---- host ---------------------------------------------------------------

	UINT32 g_last_state   = 0;
	UINT32 g_last_snap    = 0;
	uint64_t g_last_shape   = 0;

	bool InPlayableScreen()
	{
		return guiCurrentScreen == GAME_SCREEN || guiCurrentScreen == MAP_SCREEN;
	}

	// What the sector looks like in ways a soldier update cannot carry. When it
	// changes, the guests get the whole game again.
	uint64_t SectorShape()
	{
		uint64_t h = 1469598103934665603ULL;
		auto mix = [&h](uint64_t const v) { h ^= v; h *= 1099511628211ULL; };
		mix(static_cast<uint64_t>(gWorldSector.x) | static_cast<uint64_t>(gWorldSector.y) << 8 | static_cast<uint64_t>(gWorldSector.z) << 16);
		mix(gfWorldLoaded ? 1 : 0);
		// the players' own mercs: who is here, alive, whose (everyone else,
		// fights, turns, items, doors and corpses go over as live updates)
		for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
		{
			SOLDIERTYPE const& s = GetMan(i);
			if (!s.bActive || s.bTeam != OUR_TEAM) continue;
			mix(i);
			mix(static_cast<uint64_t>(s.bTeam) << 8 | (s.bInSector ? 1 : 0));
			if (!s.bInSector) continue;
			mix(static_cast<uint64_t>(s.bLevel) << 8 | (s.bLife >= OKLIFE ? 1 : 0) | (s.bLife > 0 ? 2 : 0));
			mix(s.bAssignment);
			mix(CoopOwner(s));
		}
		return h;
	}

	void SendState()
	{
		Writer w;
		w.put<INT16>(gWorldSector.x).put<INT16>(gWorldSector.y).put<INT8>(gWorldSector.z);
		w.put<UINT8>(gTacticalStatus.ubCurrentTeam);
		w.put<UINT8>(gTacticalStatus.uiFlags & INCOMBAT ? 1 : 0);
		w.put<UINT32>(guiGameClock);

		std::vector<SOLDIERTYPE const*> in_sector;
		for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
		{
			SOLDIERTYPE const& s = GetMan(i);
			if (s.bActive && s.bInSector && s.sGridNo != NOWHERE) in_sector.push_back(&s);
		}
		// soldiers the guests have not had yet go over whole first
		for (SOLDIERTYPE const* const s : in_sector)
		{
			if (!g_guests_have[s->ubID]) SendSoldier(*s);
		}
		for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i) g_guests_have[i] = false;
		for (SOLDIERTYPE const* const s : in_sector) g_guests_have[s->ubID] = true;

		w.put<UINT16>(static_cast<UINT16>(in_sector.size()));
		for (SOLDIERTYPE const* const s : in_sector)
		{
			w.put<UINT8>(s->ubID);
			w.put<INT8>(s->bLevel);
			w.put<float>(s->dXPos).put<float>(s->dYPos);
			w.put<UINT16>(s->usAnimState).put<UINT16>(s->usAniFrame);
			w.put<INT8>(s->bDirection).put<UINT8>(s->ubHiResDirection);
			w.put<INT8>(s->bLife).put<INT8>(s->bBleeding).put<INT8>(s->bBreath);
			w.put<INT8>(s->bActionPoints).put<INT8>(s->bVisible).put<INT8>(s->bCollapsed);
			w.put<UINT8>(s->bMoved ? 1 : 0);
			w.put<UINT8>(s->fDisplayDamage ? 1 : 0).put<INT16>(s->sDamage);
		}
		CoopNetSendToGuests(S_STATE, w.b);

		// who sees whom, five times a second: the guest's targeting, enemy
		// markers and so on read these lists
		static UINT32 sight_tick = 0;
		if (sight_tick++ % 4 == 0)
		{
			Writer v;
			PutSector(v);
			v.put<UINT16>(static_cast<UINT16>(in_sector.size()));
			for (SOLDIERTYPE const* const s : in_sector)
			{
				v.put<UINT8>(s->ubID).put<INT8>(gbPublicOpplist[OUR_TEAM][s->ubID]);
				v.put<INT16>(gsPublicLastKnownOppLoc[OUR_TEAM][s->ubID]).put<INT8>(s->bOppCnt);
			}
			UINT16 ours = 0;
			for (SOLDIERTYPE const* const s : in_sector) if (s->bTeam == OUR_TEAM) ++ours;
			v.put<UINT16>(ours);
			for (SOLDIERTYPE const* const s : in_sector)
			{
				if (s->bTeam != OUR_TEAM) continue;
				v.put<UINT8>(s->ubID);
				for (SOLDIERTYPE const* const o : in_sector)
				{
					v.put<INT8>(o->ubID < MAX_NUM_SOLDIERS ? s->bOppList[o->ubID] : 0);
				}
			}
			CoopNetSendToGuests(S_SIGHT, v.b);
		}
		static UINT32 sent = 0;
		if (sent++ % 30 == 0)
		{
			ST::string where;
			for (SOLDIERTYPE const* const x : in_sector) if (x->bTeam == OUR_TEAM) where += ST::format(" {}@{}/a{}", x->name, x->sGridNo, x->usAnimState);
			SLOGD("co-op: host state{}", where);
		}
	}

	SOLDIERTYPE* GuestsMerc(UINT8 const from, Reader& r)
	{
		UINT8 const id = r.get<UINT8>();
		if (!r.ok || id >= TOTAL_SOLDIERS) return nullptr;
		SOLDIERTYPE& s = GetMan(id);
		if (!s.bActive || !s.bInSector || s.bTeam != OUR_TEAM) return nullptr;
		if (CoopOwner(s) != from)
		{
			SLOGW("co-op: player {} sent an order for {}, who is not theirs", from, s.name);
			return nullptr;
		}
		if (!OkControllableMerc(&s)) return nullptr;
		if (!OK_INTERRUPT_MERC(&s))
		{
			CoopNetTellGuests(s.name + " cannot act during this interrupt.");
			return nullptr;
		}
		return &s;
	}

	bool OrdersAllowedNow()
	{
		// in a fight only during the player turn; outside of one always
		if (!(gTacticalStatus.uiFlags & INCOMBAT)) return true;
		return gTacticalStatus.ubCurrentTeam == OUR_TEAM;
	}

	void HostOnOrder(UINT8 const from, UINT8 const kind, Reader r)
	{
		if (guiCurrentScreen != GAME_SCREEN && kind != S_RESYNC && kind != S_INVENTORY && kind != S_TALK_CLOSE && kind != S_TALK_CHOICE)
		{
			CoopNetTellGuests("The host is not in tactical right now; give your order again once they are.");
			return;
		}

		if (kind == S_RESYNC)
		{
			CoopNetQueueSnapshot();
			return;
		}

		if (kind == S_WANT_SOLDIER)
		{
			UINT8 const id = r.get<UINT8>();
			if (r.ok && id < TOTAL_SOLDIERS && GetMan(id).bActive && GetMan(id).bInSector) SendSoldier(GetMan(id));
			return;
		}

		if (kind == S_TALK_CLOSE)
		{
			CoopCloseTalkPanelFromRemote();
			return;
		}

		if (kind == S_TALK_CHOICE)
		{
			UINT8 const approach = r.get<UINT8>();
			if (r.ok && gfInTalkPanel)
			{
				SLOGI("co-op: player {} chooses {} in the conversation", from, approach);
				ChooseTalkApproach(approach);
			}
			return;
		}

		if (kind == S_TALK)
		{
			UINT8 const npc_id   = r.get<UINT8>();
			UINT8 const merc_id  = r.get<UINT8>();
			UINT8 const approach = r.get<UINT8>();
			UINT8 const record   = r.get<UINT8>();
			if (!r.ok || npc_id >= TOTAL_SOLDIERS || merc_id >= TOTAL_SOLDIERS) return;
			SOLDIERTYPE& npc  = GetMan(npc_id);
			SOLDIERTYPE& merc = GetMan(merc_id);
			if (!npc.bActive || !merc.bActive || merc.bTeam != OUR_TEAM || CoopOwner(merc) != from) return;
			ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, merc.name + " (" + CoopPlayerName(from) + ") talks to " + npc.name + ".");
			InitiateConversationFull(&npc, &merc, static_cast<Approach>(approach), record, nullptr);
			return;
		}

		if (kind == S_ITEM_DROP)
		{
			INT16 grid = r.get<INT16>();
			UINT8 const level = r.get<UINT8>();
			if (!r.ok || r.left != OBJECT_BYTES || grid < 0 || grid >= GRIDSIZE) return;
			OBJECTTYPE o;
			DataReader d{r.p};
			ExtractObject(d, &o);
			AddItemToPool(grid, &o, VISIBLE, level, 0, -1); // the hook sends it back out
			return;
		}

		if (kind == S_ITEM_TAKE)
		{
			INT32 const index = r.get<INT32>();
			if (!r.ok || index < 0 || static_cast<size_t>(index) >= gWorldItems.size()) return;
			WORLDITEM& wi = gWorldItems[index];
			if (wi.fExists) RemoveItemFromPool(wi);
			return;
		}

		if (kind == S_INVENTORY)
		{
			UINT8 const id = r.get<UINT8>();
			if (!r.ok || id >= TOTAL_SOLDIERS) return;
			SOLDIERTYPE& m = GetMan(id);
			if (!m.bActive || m.bTeam != OUR_TEAM || CoopOwner(m) != from) return;
			ApplyInventory(m, r.p, r.left);
			SLOGI("co-op: {}'s inventory from player {} (gun in hand: {} rounds)", m.name, from, m.inv[HANDPOS].ubGunShotsLeft);
			return;
		}

		if (kind == S_END_TURN)
		{
			if (!(gTacticalStatus.uiFlags & INCOMBAT) || gTacticalStatus.ubCurrentTeam != OUR_TEAM) return;
			ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, CoopPlayerName(from) + " is done with the turn.");
			if (CoopPlayerEndsTurn(from))
			{
				FinishPlayerTurn();
			}
			else
			{
				CoopNetTellGuests("Waiting for " + CoopPlayerName(CoopLocalPlayer()) + " to end the turn.");
			}
			return;
		}

		if (!OrdersAllowedNow()) return;
		SOLDIERTYPE* const s = GuestsMerc(from, r);
		if (!s) return;
		SLOGI("co-op: order {} from player {} for {}", kind, from, s->name);

		switch (kind)
		{
			case S_MOVE:
			{
				GridNo const dest      = r.get<INT32>();
				UINT16 const mode      = r.get<UINT16>();
				UINT8  const reverse   = r.get<UINT8>();
				GridNo const int_grid  = r.get<INT32>();
				UINT16 const int_id    = r.get<UINT16>();
				UINT8  const int_dir   = r.get<UINT8>();
				UINT8  const pending   = r.get<UINT8>();
				UINT32 const pend1     = r.get<UINT32>();
				INT16  const pend2     = r.get<INT16>();
				INT8   const pend3     = r.get<INT8>();
				UINT32 const pend4     = r.get<UINT32>();
				if (!r.ok) return;

				STRUCTURE* const structure = int_grid != NOWHERE ? FindStructureByID(int_grid, int_id) : nullptr;
				if (structure && s->sGridNo == dest)
				{ // already there: just use it
					StartInteractiveObject(int_grid, *structure, *s, int_dir);
					InteractWithOpenableStruct(*s, *structure, int_dir);
					return;
				}

				s->usUIMovementMode = mode;
				s->bReverse         = reverse != 0;
				Soldier{s}.removePendingAction();
				if (pending != NO_PENDING_ACTION)
				{ // e.g. pick up what lies there once he arrives
					Soldier{s}.setPendingAction(pending);
					s->uiPendingActionData1 = pend1;
					s->sPendingActionData2  = pend2;
					s->bPendingActionData3  = pend3;
					s->uiPendingActionData4 = pend4;
				}
				BOOLEAN const ok = EVENT_InternalGetNewSoldierPath(s, dest, mode, TRUE, s->fNoAPToFinishMove);
				SLOGI("co-op: {} walks from {} to {}: {}", s->name, s->sGridNo, dest, ok ? "path found" : "no path");
				if (structure) StartInteractiveObject(int_grid, *structure, *s, int_dir);
				break;
			}

			case S_ATTACK:
			{
				GridNo const target = r.get<INT32>();
				INT8   const level  = r.get<INT8>();
				UINT16 const item   = r.get<UINT16>();
				INT8   const aim    = r.get<INT8>();
				INT8   const cube   = r.get<INT8>();
				UINT8  const mode   = r.get<UINT8>();
				INT8   const burst  = r.get<INT8>();
				if (!r.ok) return;

				s->fDontChargeTurningAPs = FALSE;
				s->bAimTime              = aim;
				s->bTargetCubeLevel      = cube;
				s->bWeaponMode           = static_cast<WeaponModes>(mode);
				s->bDoBurst              = burst;
				if (gTacticalStatus.uiFlags & INCOMBAT)
				{
					s->sFinalDestination = s->sGridNo;
					s->bGoodContPath     = FALSE;
				}
				HandleItem(s, target, level, item, TRUE);
				break;
			}

			case S_STANCE:
			{
				INT8 const stance = r.get<INT8>();
				if (r.ok) UIHandleSoldierStanceChange(s, stance);
				break;
			}

			case S_TURN:
			{
				GridNo const towards = r.get<INT32>();
				if (r.ok) UIMakeSoldierTurn(s, towards);
				break;
			}
		}
	}

	void HostPoll()
	{
		if (!CoopNetHasGuests()) return;

		// testing aid: JA2_COOP_TEST_LIVE=1 drops an item, starts a fight and ends the turn
		static UINT32 test_start = 0;
		static int    test_step  = 0;
		if (getenv("JA2_COOP_TEST_LIVE") && guiCurrentScreen == GAME_SCREEN && test_step < 3)
		{
			UINT32 const now = GetJA2Clock();
			if (test_start == 0) test_start = now;
			SOLDIERTYPE* host_merc = nullptr;
			for (UINT32 i = 0; i != TOTAL_SOLDIERS && !host_merc; ++i)
			{
				SOLDIERTYPE& m = GetMan(i);
				if (m.bActive && m.bInSector && m.bTeam == OUR_TEAM && CoopOwner(m) == 0) host_merc = &m;
			}
			if (test_step == 0 && now - test_start > 6000 && host_merc)
			{
				OBJECTTYPE o = host_merc->inv[HANDPOS];
				INT32 const idx = AddItemToPool(host_merc->sGridNo, &o, VISIBLE, 0, 0, 0);
				SLOGI("co-op test: host dropped item {} (type {}) at {}", idx, o.usItem, host_merc->sGridNo);
				SLOGI("co-op test: host merc speaks, grunts, and a stun grenade goes off far away");
				TacticalCharacterDialogue(host_merc, QUOTE_SEE_ENEMY);
				DoMercBattleSound(host_merc, BATTLE_SOUND_HIT1);
				IgniteExplosion(nullptr, 0, host_merc->sGridNo + 14 * WORLD_COLS, STUN_GRENADE, 0);
				IgniteExplosion(nullptr, 0, host_merc->sGridNo - 14 * WORLD_COLS, SMOKE_GRENADE, 0);
				IgniteExplosion(nullptr, 0, host_merc->sGridNo + 16, HAND_GRENADE, 0);
				InternalDropBlood(host_merc->sGridNo + 2, 0, HUMAN, 5, VISIBLE);
				// and shatter the nearest window
				GridNo best = NOWHERE;
				for (GridNo g = 0; g != GRIDSIZE; ++g)
				{
					STRUCTURE const* const w = FindStructure(g, STRUCTURE_WALLNWINDOW);
					if (!w || (w->fFlags & STRUCTURE_WALLNWINDOW) == 0) continue;
					if (best == NOWHERE || PythSpacesAway(host_merc->sGridNo, g) < PythSpacesAway(host_merc->sGridNo, best)) best = g;
				}
				if (best != NOWHERE)
				{
					STRUCTURE const* const w = FindStructure(best, STRUCTURE_WALLNWINDOW);
					SLOGI("co-op test: host shatters the window at {}", best);
					WindowHit(best, w->usStructureID, FALSE, FALSE);
				}
				test_step = 1;
			}
			else if (test_step == 1 && now - test_start > 9000)
			{
				EnterCombatMode(OUR_TEAM);
				SLOGI("co-op test: host fight: {}", gTacticalStatus.uiFlags & INCOMBAT ? "started" : "no enemies here");
				test_step = 2;
			}
			else if (test_step == 2 && now - test_start > 12000 && gTacticalStatus.ubCurrentTeam == OUR_TEAM && host_merc && !(gTacticalStatus.uiFlags & INCOMBAT && host_merc->bActionPoints < 8))
			{
				static bool shot = false;
				if (!shot)
				{
					shot = true;
					SOLDIERTYPE const* target = nullptr;
					for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
					{
						SOLDIERTYPE const& m = GetMan(i);
						if (!m.bActive || !m.bInSector || m.bTeam == OUR_TEAM || m.bLife <= 0 || m.sGridNo == NOWHERE) continue;
						if (!target || PythSpacesAway(host_merc->sGridNo, m.sGridNo) < PythSpacesAway(host_merc->sGridNo, target->sGridNo)) target = &m;
					}
					if (target)
					{
						SLOGI("co-op test: host shoots at {} ({})", target->ubID, target->sGridNo);
						host_merc->bAimTime = 0;
						HandleItem(host_merc, target->sGridNo, target->bLevel, host_merc->inv[HANDPOS].usItem, FALSE);
					}
				}
			}
			static int talk_step = 0;
			if (talk_step == 0 && now - test_start > 7000 && host_merc)
			{
				talk_step = 1;
				for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
				{
					SOLDIERTYPE& m = GetMan(i);
					if (!m.bActive || !m.bInSector || m.bTeam == OUR_TEAM || m.ubProfile == NO_PROFILE || m.bLife < OKLIFE) continue;
					SLOGI("co-op test: host talks to {}", m.name);
					InitiateConversation(&m, host_merc, NPC_INITIAL_QUOTE);
					break;
				}
			}
			else if (talk_step == 1 && now - test_start > 11000)
			{
				talk_step = 2;
				if (gfInTalkPanel) { SLOGI("co-op test: host closes the conversation"); DeleteTalkingMenu(); }
			}
			if (test_step == 2 && now - test_start > 18000)
			{
				if (gTacticalStatus.uiFlags & INCOMBAT && gTacticalStatus.ubCurrentTeam == OUR_TEAM)
				{
					SLOGI("co-op test: host ends the turn");
					UIHandleEndTurn(nullptr);
				}
				test_step = 3;
			}
		}
		CheckInventories(true);
		if (!InPlayableScreen()) return;

		UINT32 const now = GetJA2Clock();

		uint64_t const shape = SectorShape();
		if (shape != g_last_shape && now - g_last_snap >= SNAPSHOT_MIN_GAP_MS && gTacticalStatus.ubAttackBusyCount == 0)
		{ // the snapshot goes out once the game can be saved (CoopNet)
			g_last_shape = shape;
			g_last_snap  = now;
			CoopNetQueueSnapshot();
		}

		if (gfWorldLoaded && now - g_last_state >= STATE_INTERVAL_MS)
		{
			g_last_state = now;
			SendState();
		}
		SendChangedItems();
	}


	// ---- guest --------------------------------------------------------------

	ST::string g_pending_snapshot;
	bool       g_loading_snapshot = false;
	ScreenID   g_screen_before_snapshot = GAME_SCREEN;
	SoldierID  g_selected_before_snapshot = NOBODY;
	UINT32     g_last_resync_ask = 0;

	void AskForResync()
	{
		UINT32 const now = GetJA2Clock();
		if (now - g_last_resync_ask < RESYNC_MIN_GAP_MS || g_loading_snapshot) return;
		g_last_resync_ask = now;
		CoopNetSendToHost(S_RESYNC, {});
	}


	bool   g_seen_in_state[TOTAL_SOLDIERS] = {};
	UINT32 g_soldier_asked[TOTAL_SOLDIERS] = {};
	std::vector<INT32> g_local_items; // world item slots the guest changed itself

	void AskForSoldier(UINT8 const id)
	{
		UINT32 const now = GetJA2Clock();
		if (now - g_soldier_asked[id] < 2000) return;
		g_soldier_asked[id] = now;
		CoopNetSendToHost(S_WANT_SOLDIER, { id });
	}

	void SelectFirstOwnMerc()
	{
		SOLDIERTYPE const* const sel = GetSelectedMan();
		if (sel && CoopIsLocalMerc(*sel) && OkControllableMerc(sel)) return;
		for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
		{
			SOLDIERTYPE& m = GetMan(i);
			if (m.bActive && m.bInSector && m.bTeam == OUR_TEAM && CoopIsLocalMerc(m) && OkControllableMerc(&m))
			{
				SelectSoldier(&m, SELSOLDIER_FORCE_RESELECT);
				return;
			}
		}
	}

	// the host's list is complete: whoever we still show but it does not is gone
	void RemoveSoldiersTheHostLacks()
	{
		for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
		{
			SOLDIERTYPE& m = GetMan(i);
			if (!m.bActive || !m.bInSector || m.sGridNo == NOWHERE || g_seen_in_state[i]) continue;
			if (m.bTeam == OUR_TEAM)
			{ // one of ours left or fell: the host sends the whole game for that
				AskForResync();
				continue;
			}
			SLOGD("co-op: soldier {} is gone on the host", i);
			g_applying = true;
			TacticalRemoveSoldier(m);
			g_applying = false;
		}
	}

	void GuestOnSoldier(Reader& r)
	{
		UINT8 const id = r.get<UINT8>();
		if (!r.ok || id >= TOTAL_SOLDIERS || r.left != SOLDIER_RECORD_SIZE) return;

		SOLDIERTYPE& old = GetMan(id);
		if (old.bActive)
		{
			if (old.bTeam == OUR_TEAM) return; // ours never change hands like this
			TacticalRemoveSoldier(old);
		}

		SOLDIERTYPE rec{};
		ExtractSoldierType(r.p, &rec, false, SAVE_GAME_VERSION);
		if (rec.ubID != id) return;
		GridNo const where = rec.sGridNo;
		INT8   const dir   = rec.bDirection;
		SOLDIERTYPE* const s = TacticalCreateSoldierFromExisting(&rec);
		if (!s) return;
		s->sInsertionGridNo     = where;
		s->ubInsertionDirection = dir + 100; // face the way the host says
		AddSoldierToSectorNoCalculateDirection(s);
		SLOGD("co-op: soldier {} ({}) arrived", id, s->name);
	}

	void GuestOnItem(Reader& r)
	{
		INT32 const index  = r.get<INT32>();
		UINT8 const exists = r.get<UINT8>();
		if (!r.ok || index < 0 || index > 0x10000) return;

		if (static_cast<size_t>(index) < gWorldItems.size() && gWorldItems[index].fExists)
		{
			RemoveItemFromPool(gWorldItems[index]);
		}
		if (!exists) return;

		INT16 grid        = r.get<INT16>();
		UINT8 const level = r.get<UINT8>();
		UINT16 const flags = r.get<UINT16>();
		INT8 const height = r.get<INT8>();
		INT8 const vis    = r.get<INT8>();
		if (!r.ok || r.left != OBJECT_BYTES || grid < 0 || grid >= GRIDSIZE) return;
		OBJECTTYPE o;
		DataReader d{r.p};
		ExtractObject(d, &o);
		SetNextWorldItemIndex(index);
		InternalAddItemToPool(&grid, &o, static_cast<Visibility>(vis), level, flags, height);
		SetNextWorldItemIndex(-1);
		SLOGI("co-op: item {} (type {}) now at {}", index, o.usItem, grid);
	}

	void GuestOnCorpse(Reader& r)
	{
		if (r.left != ROTTING_CORPSE_RECORD_SIZE) return;
		ROTTING_CORPSE_DEFINITION def{};
		ExtractRottingCorpse(r.p, &def);
		AddRottingCorpse(&def);
	}

	void GuestOnDoor(Reader& r)
	{
		INT16 const grid = r.get<INT16>();
		UINT8 const open = r.get<UINT8>();
		if (!r.ok || grid < 0 || grid >= GRIDSIZE) return;
		STRUCTURE const* const door = FindStructure(grid, STRUCTURE_ANYDOOR);
		if (!door) return;
		bool const is_open = (door->fFlags & STRUCTURE_OPEN) != 0;
		if (is_open != (open != 0)) HandleDoorChangeFromGridNo(nullptr, grid, TRUE);
	}



	FACETYPE* FaceOf(UINT8 const profile)
	{
		SOLDIERTYPE* const s = FindSoldierByProfileID(profile);
		if (s && s->bActive)
		{
			if (!s->face) InitSoldierFace(*s);
			if (s->face) return s->face;
		}
		return GetExternalNPCFace(profile);
	}

	void GuestOnSpeech(Reader& r)
	{
		UINT8  const profile = r.get<UINT8>();
		UINT16 const quote   = r.get<UINT16>();
		UINT8  const alt     = r.get<UINT8>();
		if (!r.ok) return;
		if (alt)
		{
			SOLDIERTYPE* const s = FindSoldierByProfileID(profile);
			if (s) CharacterDialogueUsingAlternateFile(*s, quote, DIALOGUE_TACTICAL_UI);
			return;
		}
		if (gfInTalkPanel && gTalkPanel.ubCharNum == profile && gTalkPanel.face)
		{ // the line belongs in the conversation panel
			SLOGI("co-op: {} says quote {} in the conversation", profile, quote);
			CharacterDialogue(profile, quote, gTalkPanel.face, DIALOGUE_NPC_UI, FALSE);
			return;
		}
		FACETYPE* const face = FaceOf(profile);
		if (!face) return;
		SLOGI("co-op: {} says quote {}", profile, quote);
		CharacterDialogue(profile, quote, face, DIALOGUE_TACTICAL_UI, FALSE);
	}

	void GuestOnBattleSound(Reader& r)
	{
		UINT8 const id    = r.get<UINT8>();
		UINT8 const snd   = r.get<UINT8>();
		UINT8 const lower = r.get<UINT8>();
		if (!r.ok || id >= TOTAL_SOLDIERS || snd >= NUM_MERC_BATTLE_SOUNDS) return;
		SOLDIERTYPE& s = GetMan(id);
		if (!s.bActive) return;
		SLOGI("co-op: {} battle sound {}", s.name, snd);
		InternalDoMercBattleSound(&s, static_cast<BattleSound>(snd), lower != 0);
	}

	void GuestOnExplosion(Reader& r)
	{
		INT16 const x = r.get<INT16>();
		INT16 const y = r.get<INT16>();
		INT16 const z = r.get<INT16>();
		INT16 const grid = r.get<INT16>();
		UINT16 const item = r.get<UINT16>();
		INT8 const level = r.get<INT8>();
		if (!r.ok || grid < 0 || grid >= GRIDSIZE) return;
		SLOGI("co-op: explosion at {}", grid);
		ShowExplosionOnly(x, y, z, grid, item, level);
	}


	// muzzle flashes the guest lit, and when they go out
	struct Flash { SOLDIERTYPE* s; UINT32 until; };
	std::vector<Flash> g_flashes;

	void ShowMuzzleFlash(SOLDIERTYPE& s)
	{
		// as the firing animation does (Soldier_Ani.cc, code 441)
		if (s.bVisible == -1 || ubAmbientLightLevel < MIN_AMB_LEVEL_FOR_MERC_LIGHTS) return;
		if (s.muzzle_flash) { LightSpriteDestroy(s.muzzle_flash); s.muzzle_flash = nullptr; }
		LIGHT_SPRITE* const l = LightSpriteCreate("L-R03.LHT");
		if (!l) return;
		s.muzzle_flash = l;
		LightSpritePower(l, TRUE);
		GridNo const ahead = NewGridNo(s.sGridNo, DirectionInc(s.bDirection));
		INT16 x, y;
		ConvertGridNoToXY(ahead, &x, &y);
		LightSpritePosition(l, x, y);
		g_flashes.push_back({ &s, GetJA2Clock() + 150 });
	}

	void PutOutMuzzleFlashes()
	{
		UINT32 const now = GetJA2Clock();
		for (auto i = g_flashes.begin(); i != g_flashes.end();)
		{
			if (now < i->until) { ++i; continue; }
			if (i->s->muzzle_flash) { LightSpriteDestroy(i->s->muzzle_flash); i->s->muzzle_flash = nullptr; }
			i = g_flashes.erase(i);
		}
	}

	void GuestOnMapChange(Reader& r)
	{
		MODIFY_MAP m{};
		m.usGridNo        = r.get<UINT16>();
		m.usImageType     = r.get<UINT16>();
		m.usSubImageIndex = r.get<UINT16>();
		m.ubType          = r.get<UINT8>();
		m.ubExtra         = r.get<UINT8>();
		if (!r.ok || m.usGridNo >= GRIDSIZE) return;
		SLOGI("co-op: map change {} at {}", m.ubType, m.usGridNo);
		ApplyMapChangeLive(m);
	}

	void GuestOnSmoke(Reader& r)
	{
		INT16 const grid  = r.get<INT16>();
		INT8  const level = r.get<INT8>();
		UINT8 const type  = r.get<UINT8>();
		UINT8 const dur   = r.get<UINT8>();
		INT8  const age   = r.get<INT8>();
		UINT8 const add   = r.get<UINT8>();
		if (!r.ok || grid < 0 || grid >= GRIDSIZE || level < 0 || level > 1) return;
		static int smokes = 0;
		if (smokes++ < 3) SLOGI("co-op: smoke tile at {} ({})", grid, add ? "on" : "off");
		if (!add)
		{
			RemoveSmokeEffectFromTile(grid, level);
			return;
		}
		SmokeEffectModel const* const model = GCM->getSmokeEffect(static_cast<SmokeEffectID>(type));
		if (!model) return;
		SMOKEEFFECT smoke{};
		smoke.ubDuration = dur;
		smoke.bAge       = age;
		AddSmokeEffectToTile(&smoke, model, grid, level);
	}

	void GuestOnBlood(Reader& r)
	{
		INT16 const grid     = r.get<INT16>();
		INT8  const level    = r.get<INT8>();
		UINT8 const kind     = r.get<UINT8>();
		UINT8 const strength = r.get<UINT8>();
		INT8  const visible  = r.get<INT8>();
		if (!r.ok || grid < 0 || grid >= GRIDSIZE) return;
		SLOGI("co-op: blood at {}", grid);
		InternalDropBlood(grid, level, static_cast<BloodKind>(kind), strength, visible);
	}

	void GuestOnMiss(Reader& r)
	{
		INT16 const grid = r.get<INT16>();
		INT16 const x = r.get<INT16>();
		INT16 const y = r.get<INT16>();
		INT16 const z = r.get<INT16>();
		if (!r.ok || grid < 0 || grid >= GRIDSIZE) return;
		ShowBulletMissOnly(grid, x, y, z);
	}

	void GuestOnTalkOpen(Reader& r)
	{
		UINT8 const npc_id  = r.get<UINT8>();
		UINT8 const merc_id = r.get<UINT8>();
		if (!r.ok || npc_id >= TOTAL_SOLDIERS) return;
		SOLDIERTYPE& npc = GetMan(npc_id);
		if (!npc.bActive) return;
		SOLDIERTYPE* const merc = merc_id < TOTAL_SOLDIERS && GetMan(merc_id).bActive ? &GetMan(merc_id) : nullptr;
		SLOGI("co-op: conversation with {} opens", npc.name);
		CoopShowTalkPanel(&npc, merc);
	}

	void GuestOnSight(Reader& r)
	{
		UINT16 const n = r.get<UINT16>();
		if (!r.ok || n > TOTAL_SOLDIERS) return;
		std::vector<UINT8> ids;
		for (UINT16 i = 0; i != n; ++i)
		{
			UINT8 const id  = r.get<UINT8>();
			INT8  const pub = r.get<INT8>();
			INT16 const loc = r.get<INT16>();
			INT8  const cnt = r.get<INT8>();
			if (!r.ok || id >= TOTAL_SOLDIERS) return;
			ids.push_back(id);
			gbPublicOpplist[OUR_TEAM][id]        = pub;
			gsPublicLastKnownOppLoc[OUR_TEAM][id] = loc;
			GetMan(id).bOppCnt                   = cnt;
		}
		UINT16 const ours = r.get<UINT16>();
		for (UINT16 i = 0; i != ours && r.ok; ++i)
		{
			UINT8 const id = r.get<UINT8>();
			if (!r.ok || id >= TOTAL_SOLDIERS) return;
			SOLDIERTYPE& s = GetMan(id);
			for (UINT8 const o : ids)
			{
				INT8 const seen = r.get<INT8>();
				if (o < MAX_NUM_SOLDIERS) s.bOppList[o] = seen;
			}
		}
		static INT32 last_seen = -1;
		INT32 seen = 0;
		for (UINT8 const o : ids) if (gbPublicOpplist[OUR_TEAM][o] == SEEN_CURRENTLY) ++seen;
		if (seen != last_seen) { SLOGI("co-op: our side sees {} soldiers now", seen); last_seen = seen; }
		fInterfacePanelDirty = DIRTYLEVEL2;
	}

	void GuestOnShot(Reader& r)
	{
		UINT8  const id      = r.get<UINT8>();
		float  const x       = r.get<float>();
		float  const y       = r.get<float>();
		float  const z       = r.get<float>();
		UINT16 const item    = r.get<UINT16>();
		INT16  const hit_by  = r.get<INT16>();
		UINT8  const buck    = r.get<UINT8>();
		INT8   const burst   = r.get<INT8>();
		UINT8  const hand    = r.get<UINT8>();
		if (!r.ok || id >= TOTAL_SOLDIERS) return;
		SOLDIERTYPE& s = GetMan(id);
		if (!s.bActive || !s.bInSector || s.sGridNo == NOWHERE) return;

		// the sound, as the host's game made it
		if (GCM->getItem(item)->isWeapon() && GCM->getItem(item)->getItemClass() != IC_THROWING_KNIFE)
		{
			auto const weapon = GCM->getWeapon(item);
			if (burst == 1)
			{
				s.bDoBurst = 1;
				ST::string const burst_sound = GetBurstSoundName(s);
				if (!burst_sound.empty()) PlayLocationJA2SampleFromFile(s.sGridNo, burst_sound.c_str(), HIGHVOLUME, 1);
			}
			else if (burst == 0)
			{
				bool const silenced = hand < NUM_INV_SLOTS && FindAttachment(&s.inv[hand], SILENCER) != NO_SLOT;
				ST::string const& snd = silenced ? weapon->silencedSound : weapon->sound;
				if (!snd.empty()) PlayLocationJA2Sample(s.sGridNo, snd, HIGHVOLUME, 1);
			}
		}

		SLOGI("co-op: {} fires (item {}, burst {})", s.name, item, burst);
		ShowMuzzleFlash(s);
		// and the bullet, only for show
		FireBulletGivenTarget(&s, x, y, z, item, hit_by, buck != 0, FALSE);
	}

	// the guest dropped or picked up something itself: the host does it, and
	// tells everybody, the guest included
	void SendLocalItemChanges()
	{
		if (g_local_items.empty() || gpItemPointer) return;
		std::vector<INT32> changed;
		changed.swap(g_local_items);
		std::sort(changed.begin(), changed.end());
		changed.erase(std::unique(changed.begin(), changed.end()), changed.end());

		for (INT32 const i : changed)
		{
			if (i < 0) continue;
			if (static_cast<size_t>(i) < gWorldItems.size() && gWorldItems[i].fExists)
			{
				WORLDITEM& wi = gWorldItems[i];
				Writer w;
				w.put<INT16>(wi.sGridNo).put<UINT8>(wi.ubLevel);
				UINT8 obj[OBJECT_BYTES];
				DataWriter d{obj};
				InjectObject(d, &wi.o);
				w.b.insert(w.b.end(), obj, obj + OBJECT_BYTES);
				CoopNetSendToHost(S_ITEM_DROP, w.b);
				g_applying = true;
				RemoveItemFromPool(wi); // it comes back from the host
				g_applying = false;
			}
			else
			{
				Writer w;
				w.put<INT32>(i);
				CoopNetSendToHost(S_ITEM_TAKE, w.b);
			}
		}
	}

	void ApplySoldier(Reader& r)
	{
		UINT8  const id     = r.get<UINT8>();
		INT8   const level  = r.get<INT8>();
		float  const x      = r.get<float>();
		float  const y      = r.get<float>();
		UINT16 const anim   = r.get<UINT16>();
		UINT16 const frame  = r.get<UINT16>();
		INT8   const dir    = r.get<INT8>();
		UINT8  const hires  = r.get<UINT8>();
		INT8   const life   = r.get<INT8>();
		INT8   const bleed  = r.get<INT8>();
		INT8   const breath = r.get<INT8>();
		INT8   const ap     = r.get<INT8>();
		INT8   const vis    = r.get<INT8>();
		INT8   const coll   = r.get<INT8>();
		UINT8  const moved  = r.get<UINT8>();
		UINT8  const show_damage = r.get<UINT8>();
		INT16  const damage = r.get<INT16>();
		if (!r.ok || id >= TOTAL_SOLDIERS) return;

		g_seen_in_state[id] = true;
		SOLDIERTYPE& s = GetMan(id);
		if (!s.bActive || !s.bInSector || s.sGridNo == NOWHERE)
		{ // somebody we do not have yet
			AskForSoldier(id);
			return;
		}
		if (s.bLevel != level)
		{ // climbed onto or off a roof: that takes the whole game
			AskForResync();
			return;
		}

		if (s.dXPos != x || s.dYPos != y)
		{
			// the guest plans no routes: let the destination follow, or a walking
			// soldier is held on the tile he is heading for
			EVENT_SetSoldierPositionXY(&s, x, y, SSP_NONE);
		}
		if (s.usAnimState != anim)
		{
			s.usOldAniState = s.usAnimState;
			s.usAnimState   = anim;
			SetSoldierAnimationSurface(&s, anim);
		}
		s.bDirection       = dir;
		s.ubHiResDirection = hires;
		s.usAniFrame       = frame;
		s.bLife            = life;
		s.bBleeding        = bleed;
		s.bBreath          = breath;
		s.bActionPoints    = ap;
		s.bVisible         = vis;
		s.bCollapsed       = coll;
		s.bMoved           = moved != 0;
		if (show_damage && !s.fDisplayDamage)
		{ // the red number over his head
			s.fDisplayDamage      = TRUE;
			s.bDisplayDamageCount = 0;
			s.sDamage             = damage;
			s.sDamageX            = s.sBoundingBoxOffsetX;
			s.sDamageY            = s.sBoundingBoxOffsetY;
		}
	}

	void GuestOnState(Reader r)
	{
		if (g_loading_snapshot || !gfWorldLoaded) return;

		INT16 const sx = r.get<INT16>();
		INT16 const sy = r.get<INT16>();
		INT8  const sz = r.get<INT8>();
		UINT8 const team = r.get<UINT8>();
		UINT8 const combat = r.get<UINT8>();
		UINT32 const clock = r.get<UINT32>();
		UINT16 const n = r.get<UINT16>();
		if (!r.ok) return;

		if (sx != gWorldSector.x || sy != gWorldSector.y || sz != gWorldSector.z)
		{
			SLOGI("co-op: the host is in another sector, asking for the game again");
			AskForResync();
			return;
		}

		bool const fight = combat != 0;
		bool const fight_changed = fight != ((gTacticalStatus.uiFlags & INCOMBAT) != 0);
		if (fight_changed)
		{
			if (fight)
			{
				gTacticalStatus.uiFlags |= INCOMBAT;
				ResetInterfaceAndUI();
				ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, "Combat!");
				SLOGI("co-op: fight started on the host");
			}
			else
			{
				gTacticalStatus.uiFlags &= ~INCOMBAT;
				EndTopMessage();
				gTacticalStatus.ubCurrentTeam = OUR_TEAM;
				guiPendingOverrideEvent = LU_ENDUILOCK;
				ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, "The fight is over.");
				SLOGI("co-op: fight over on the host");
			}
			fInterfacePanelDirty = DIRTYLEVEL2;
		}

		if (fight && (fight_changed || team != gTacticalStatus.ubCurrentTeam))
		{ // whose turn it is: lock our orders during the others' turns
			gTacticalStatus.ubCurrentTeam = team;
			SLOGI("co-op: now team {}'s turn", team);
			if (team == OUR_TEAM)
			{
				InitPlayerUIBar(FALSE);
				guiPendingOverrideEvent = LU_ENDUILOCK;
				SelectFirstOwnMerc();
			}
			else
			{
				AddTopMessage(COMPUTER_TURN_MESSAGE);
				guiPendingOverrideEvent = LU_BEGINUILOCK;
			}
			fInterfacePanelDirty = DIRTYLEVEL2;
		}
		guiGameClock = clock;

		for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i) g_seen_in_state[i] = false;
		for (UINT16 i = 0; i != n && r.ok; ++i) ApplySoldier(r);
		if (r.ok) RemoveSoldiersTheHostLacks();
		static UINT32 states = 0;
		if (states % 30 == 0)
		{
			ST::string where;
			for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i) { SOLDIERTYPE const& x = GetMan(i); if (x.bActive && x.bInSector && x.bTeam == OUR_TEAM) where += ST::format(" {}@{}/a{}", x.name, x.sGridNo, x.usAnimState); }
			SLOGD("co-op: guest sees{}", where);
		}
		if (states++ % 100 == 0) SLOGD("co-op: soldier update {} ({} soldiers)", states, n);
		SetRenderFlags(RENDER_FLAG_FULL);
	}

	bool IsGuestOrder(SOLDIERTYPE const& s)
	{
		return CoopNetGuestPlaying() && s.bTeam == OUR_TEAM;
	}
}


void CoopSyncPoll()
{
	if (CoopNetIsHost())
	{
		HostPoll();
		return;
	}

	// guest testing aid: JA2_COOP_TEST_GUEST_MOVE=1 walks our first merc a few tiles once
	static UINT32 test_start = 0;
	static int    test_step  = 0;
	static SOLDIERTYPE* test_merc = nullptr;
	static GridNo test_from = NOWHERE;
	if (test_step < 2 && getenv("JA2_COOP_TEST_GUEST_MOVE") && CoopNetGuestPlaying() && guiCurrentScreen == GAME_SCREEN && !g_loading_snapshot)
	{
		UINT32 const now = GetJA2Clock();
		if (test_start == 0) test_start = now;
		if (test_step == 0 && now - test_start > 4000)
		{
			for (UINT32 i = 0; i != TOTAL_SOLDIERS && !test_merc; ++i)
			{
				SOLDIERTYPE& m = GetMan(i);
				if (m.bActive && m.bInSector && m.bTeam == OUR_TEAM && CoopIsLocalMerc(m) && OkControllableMerc(&m)) test_merc = &m;
			}
			if (test_merc)
			{
				test_from = test_merc->sGridNo;
				SLOGI("co-op test: ordering {} from {} to {}", test_merc->name, test_from, test_from + 4);
				CoopSyncGuestMove(*test_merc, test_from + 4, WALKING, false, NOWHERE, 0);
				// and pretend to reload: the host should get the new round count
				test_merc->inv[HANDPOS].ubGunShotsLeft += 1;
				SLOGI("co-op test: {} now has {} rounds here", test_merc->name, test_merc->inv[HANDPOS].ubGunShotsLeft);
			}
			else
			{
				SLOGI("co-op test: no merc of ours here");
			}
			test_step = 1;
		}
		else if (test_step == 1 && now - test_start > 16000)
		{
			if (test_merc) SLOGI("co-op test: {} is now at {} (was {})", test_merc->name, test_merc->sGridNo, test_from);
			test_step = 2;
		}
	}

	static bool test_ended_turn = false;
	if (!test_ended_turn && getenv("JA2_COOP_TEST_GUEST_MOVE") && CoopNetGuestPlaying() && gTacticalStatus.uiFlags & INCOMBAT && gTacticalStatus.ubCurrentTeam == OUR_TEAM)
	{
		static UINT32 fight_seen = 0;
		if (fight_seen == 0) fight_seen = GetJA2Clock();
		if (GetJA2Clock() - fight_seen > 7000)
		{
			SLOGI("co-op test: guest ends the turn");
			CoopSyncGuestEndTurn();
			test_ended_turn = true;
		}
	}

	PutOutMuzzleFlashes();

	// guest on the map screen: the strategic game is the host's, so see its
	// latest state when arriving there and when going back
	static UINT32 last_screen = 0;
	if (CoopNetGuestPlaying() && !g_loading_snapshot && guiCurrentScreen != last_screen)
	{
		bool const map_now = guiCurrentScreen == MAP_SCREEN;
		bool const map_before = last_screen == MAP_SCREEN;
		last_screen = guiCurrentScreen;
		if (map_now != map_before && InPlayableScreen() && last_screen != 0) CoopSyncRequestResync();
	}

	if (CoopNetGuestPlaying() && !g_loading_snapshot)
	{
		CheckInventories(false);
		SendLocalItemChanges();
	}

	// guest: a copy that arrived while we could not load it
	if (!g_pending_snapshot.empty() && !g_loading_snapshot && InPlayableScreen())
	{
		ST::string const name = g_pending_snapshot;
		g_pending_snapshot.clear();
		CoopSyncLoadSnapshot(name);
	}
}


void CoopSyncOnMessage(UINT8 const from, UINT8 const kind, UINT8 const* const data, size_t const len)
{
	Reader const r{ data, len };
	if (CoopNetIsHost())
	{
		HostOnOrder(from, kind, r);
	}
	else if (kind == S_STATE && from == 0 && CoopNetGuestPlaying())
	{
		GuestOnState(r);
	}
	else if (kind == S_INVENTORY && from == 0 && CoopNetGuestPlaying() && !g_loading_snapshot && len >= 1 && data[0] < TOTAL_SOLDIERS)
	{
		SOLDIERTYPE& m = GetMan(data[0]);
		if (m.bActive) ApplyInventory(m, data + 1, len - 1);
	}
	else if (from == 0 && CoopNetGuestPlaying() && !g_loading_snapshot && gfWorldLoaded)
	{
		Reader rr = r;
		INT16 const sx = rr.get<INT16>();
		INT16 const sy = rr.get<INT16>();
		INT8  const sz = rr.get<INT8>();
		if (!rr.ok || sx != gWorldSector.x || sy != gWorldSector.y || sz != gWorldSector.z) return;
		g_applying = true;
		switch (kind)
		{
			case S_SOLDIER: GuestOnSoldier(rr); break;
			case S_ITEM:    GuestOnItem(rr);    break;
			case S_CORPSE:  GuestOnCorpse(rr);  break;
			case S_DOOR:    GuestOnDoor(rr);    break;
			case S_SIGHT:   GuestOnSight(rr);   break;
			case S_SHOT:    GuestOnShot(rr);    break;
			case S_SPEECH:  GuestOnSpeech(rr);  break;
			case S_BATTLE_SOUND: GuestOnBattleSound(rr); break;
			case S_EXPLOSION: GuestOnExplosion(rr); break;
			case S_MAP_CHANGE: GuestOnMapChange(rr); break;
			case S_SMOKE:   GuestOnSmoke(rr);   break;
			case S_BLOOD:   GuestOnBlood(rr);   break;
			case S_MISS:    GuestOnMiss(rr);    break;
			case S_TALK_OPEN: GuestOnTalkOpen(rr); break;
			case S_TALK_CLOSE: if (gfInTalkPanel) { SLOGI("co-op: conversation closes"); DeleteTalkingMenu(); } break;
		}
		g_applying = false;
	}
}


bool CoopSyncGuestFrozen()
{
	return CoopNetGuestPlaying();
}


void CoopSyncLoadSnapshot(ST::string const& save_name)
{
	if (g_loading_snapshot || !InPlayableScreen())
	{ // later, from CoopSyncPoll
		g_pending_snapshot = save_name;
		return;
	}
	g_loading_snapshot         = true;
	g_screen_before_snapshot   = guiCurrentScreen;
	SOLDIERTYPE const* const sel = GetSelectedMan();
	g_selected_before_snapshot = sel ? sel->ubID : NOBODY;
	guiPreviousOptionScreen    = guiCurrentScreen;
	LoadSaveInPlace(save_name);
}


void CoopSyncAfterSnapshotLoaded()
{
	g_loading_snapshot = false;
	g_local_items.clear();
	for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i) g_inv_seen[i] = Hash(PackInventory(GetMan(i)));

	// stay on the screen we were on, if the game allows it
	if (g_screen_before_snapshot == GAME_SCREEN && gfWorldLoaded)
	{
		guiScreenToGotoAfterLoadingSavedGame = GAME_SCREEN;
	}
	else if (g_screen_before_snapshot == MAP_SCREEN)
	{
		guiScreenToGotoAfterLoadingSavedGame = MAP_SCREEN;
	}

	// and with the merc we had selected (the save holds the host's choice)
	if (g_selected_before_snapshot != NOBODY)
	{
		SOLDIERTYPE& s = GetMan(g_selected_before_snapshot);
		if (s.bActive && CoopIsLocalMerc(s)) gPreferredInitialSelectedGuy = &s;
	}
	CoopFixLocalPlayer(CoopNetSeat());
}


bool CoopSyncGuestMove(SOLDIERTYPE& s, GridNo const dest, UINT16 const mode, bool const reverse, GridNo const int_grid, UINT8 const int_dir)
{
	if (!IsGuestOrder(s)) return false;
	UINT16 int_id = 0;
	if (int_grid != NOWHERE)
	{
		STRUCTURE const* const st = FindStructure(int_grid, STRUCTURE_OPENABLE);
		if (st) int_id = st->usStructureID;
	}
	// a pick up that would open the choice of items opens it on the host's
	// screen: take everything instead
	UINT32 pend1 = s.uiPendingActionData1;
	if (s.ubPendingAction == MERC_PICKUPITEM && pend1 == ITEM_PICKUP_SELECTION) pend1 = ITEM_PICKUP_ACTION_ALL;

	Writer w;
	w.put<UINT8>(s.ubID).put<INT32>(dest).put<UINT16>(mode).put<UINT8>(reverse ? 1 : 0);
	w.put<INT32>(int_id ? int_grid : NOWHERE).put<UINT16>(int_id).put<UINT8>(int_dir);
	w.put<UINT8>(s.ubPendingAction).put<UINT32>(pend1).put<INT16>(s.sPendingActionData2);
	w.put<INT8>(s.bPendingActionData3).put<UINT32>(s.uiPendingActionData4);
	CoopNetSendToHost(S_MOVE, w.b);
	Soldier{&s}.removePendingAction(); // the host does it now
	return true;
}


bool CoopSyncGuestAttack(SOLDIERTYPE& s, GridNo const target, INT8 const level, UINT16 const item)
{
	if (!IsGuestOrder(s)) return false;
	Writer w;
	w.put<UINT8>(s.ubID).put<INT32>(target).put<INT8>(level).put<UINT16>(item);
	w.put<INT8>(s.bAimTime).put<INT8>(s.bTargetCubeLevel).put<UINT8>(static_cast<UINT8>(s.bWeaponMode)).put<INT8>(s.bDoBurst);
	CoopNetSendToHost(S_ATTACK, w.b);
	return true;
}


bool CoopSyncGuestStance(SOLDIERTYPE& s, INT8 const stance)
{
	if (!IsGuestOrder(s)) return false;
	Writer w;
	w.put<UINT8>(s.ubID).put<INT8>(stance);
	CoopNetSendToHost(S_STANCE, w.b);
	return true;
}


bool CoopSyncGuestTurn(SOLDIERTYPE& s, GridNo const towards)
{
	if (!IsGuestOrder(s)) return false;
	Writer w;
	w.put<UINT8>(s.ubID).put<INT32>(towards);
	CoopNetSendToHost(S_TURN, w.b);
	return true;
}


bool CoopSyncGuestEndTurn()
{
	if (!CoopNetGuestPlaying()) return false;
	CoopNetSendToHost(S_END_TURN, {});
	ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, "You are done with the turn. Waiting for the others...");
	return true;
}


void CoopSyncRequestResync()
{
	if (!CoopNetGuestPlaying()) return;
	CoopNetSendToHost(S_RESYNC, {});
	ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, "Asking the host for a fresh copy of the game...");
}


void CoopSyncWorldItemChanged(INT32 const index)
{
	if (g_applying) return;
	if (CoopNetIsHost())
	{
		if (CoopNetHasGuests() && gfWorldLoaded) g_items_changed.push_back(index);
	}
	else if (CoopNetGuestPlaying() && !g_loading_snapshot && gfWorldLoaded)
	{
		g_local_items.push_back(index);
	}
}


void CoopSyncCorpseAdded(ROTTING_CORPSE_DEFINITION const& def)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	if (gTacticalStatus.uiFlags & LOADING_SAVED_GAME) return;
	Writer w;
	PutSector(w);
	std::vector<UINT8> rec(ROTTING_CORPSE_RECORD_SIZE);
	InjectRottingCorpse(rec.data(), &def);
	w.b.insert(w.b.end(), rec.begin(), rec.end());
	CoopNetSendToGuests(S_CORPSE, w.b);
}


void CoopSyncDoorChanged(GridNo const grid)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	STRUCTURE const* const door = FindStructure(grid, STRUCTURE_ANYDOOR);
	if (!door) return;
	Writer w;
	PutSector(w);
	w.put<INT16>(static_cast<INT16>(grid)).put<UINT8>(door->fFlags & STRUCTURE_OPEN ? 1 : 0);
	CoopNetSendToGuests(S_DOOR, w.b);
}


void CoopSyncShotFired(SOLDIERTYPE const& firer, FLOAT const x, FLOAT const y, FLOAT const z, UINT16 const item, INT16 const hit_by, BOOLEAN const buckshot)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	Writer w;
	PutSector(w);
	w.put<UINT8>(firer.ubID).put<float>(x).put<float>(y).put<float>(z);
	w.put<UINT16>(item).put<INT16>(hit_by).put<UINT8>(buckshot ? 1 : 0);
	w.put<INT8>(firer.bDoBurst).put<UINT8>(firer.ubAttackingHand);
	CoopNetSendToGuests(S_SHOT, w.b);
}


void CoopSyncSpeech(UINT8 const profile, UINT16 const quote, bool const alternate_file)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests()) return;
	Writer w;
	PutSector(w);
	w.put<UINT8>(profile).put<UINT16>(quote).put<UINT8>(alternate_file ? 1 : 0);
	CoopNetSendToGuests(S_SPEECH, w.b);
	SLOGD("co-op: speech {} quote {}", profile, quote);
}


void CoopSyncBattleSound(SOLDIERTYPE const& s, UINT8 const sound, bool const lower_volume)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	// "yes sir" and the like answer whoever gave the order: not the guests'
	// business when the host clicked its own merc
	if (s.bTeam == OUR_TEAM && CoopOwner(s) == 0)
	{
		switch (sound)
		{
			case BATTLE_SOUND_OK1: case BATTLE_SOUND_OK2: case BATTLE_SOUND_ATTN1:
			case BATTLE_SOUND_LOWMORALE_OK1: case BATTLE_SOUND_LOWMORALE_OK2:
			case BATTLE_SOUND_LOWMORALE_ATTN1: case BATTLE_SOUND_PISSED:
				return;
			default: break;
		}
	}
	Writer w;
	PutSector(w);
	w.put<UINT8>(s.ubID).put<UINT8>(sound).put<UINT8>(lower_volume ? 1 : 0);
	CoopNetSendToGuests(S_BATTLE_SOUND, w.b);
}


void CoopSyncExplosion(INT16 const x, INT16 const y, INT16 const z, INT16 const grid, UINT16 const item, INT8 const level)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	Writer w;
	PutSector(w);
	w.put<INT16>(x).put<INT16>(y).put<INT16>(z).put<INT16>(grid).put<UINT16>(item).put<INT8>(level);
	CoopNetSendToGuests(S_EXPLOSION, w.b);
}


bool CoopSyncGuestTalk(SOLDIERTYPE const& npc, SOLDIERTYPE const& merc, UINT8 const approach, UINT8 const record, bool const with_item)
{
	if (!CoopNetGuestPlaying() || g_applying) return false;
	if (with_item)
	{
		ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, "Handing items to people goes through the host for now.");
		return true;
	}
	Writer w;
	w.put<UINT8>(npc.ubID).put<UINT8>(merc.ubID).put<UINT8>(approach).put<UINT8>(record);
	CoopNetSendToHost(S_TALK, w.b);
	ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, merc.name + " talks to " + npc.name + "; the host's game runs the conversation.");
	return true;
}


void CoopSyncMapChange(MODIFY_MAP const& m)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	Writer w;
	PutSector(w);
	w.put<UINT16>(m.usGridNo).put<UINT16>(m.usImageType).put<UINT16>(m.usSubImageIndex);
	w.put<UINT8>(m.ubType).put<UINT8>(m.ubExtra);
	CoopNetSendToGuests(S_MAP_CHANGE, w.b);
}


void CoopSyncSmokeTile(INT16 const grid, INT8 const level, UINT8 const type, UINT8 const duration, INT8 const age, bool const add)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	if (gTacticalStatus.uiFlags & LOADING_SAVED_GAME) return;
	Writer w;
	PutSector(w);
	w.put<INT16>(grid).put<INT8>(level).put<UINT8>(type).put<UINT8>(duration).put<INT8>(age).put<UINT8>(add ? 1 : 0);
	CoopNetSendToGuests(S_SMOKE, w.b);
}


void CoopSyncBlood(INT16 const grid, INT8 const level, UINT8 const kind, UINT8 const strength, INT8 const visible)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	if (gTacticalStatus.uiFlags & LOADING_SAVED_GAME) return;
	Writer w;
	PutSector(w);
	w.put<INT16>(grid).put<INT8>(level).put<UINT8>(kind).put<UINT8>(strength).put<INT8>(visible);
	CoopNetSendToGuests(S_BLOOD, w.b);
}


void CoopSyncBulletMiss(INT16 const grid, INT16 const x, INT16 const y, INT16 const z)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests() || !gfWorldLoaded) return;
	Writer w;
	PutSector(w);
	w.put<INT16>(grid).put<INT16>(x).put<INT16>(y).put<INT16>(z);
	CoopNetSendToGuests(S_MISS, w.b);
}


void CoopSyncTalkPanelOpened(SOLDIERTYPE const& npc, SOLDIERTYPE const* const merc)
{
	if (g_applying || !CoopNetIsHost() || !CoopNetHasGuests()) return;
	Writer w;
	PutSector(w);
	w.put<UINT8>(npc.ubID).put<UINT8>(merc ? merc->ubID : NOBODY);
	CoopNetSendToGuests(S_TALK_OPEN, w.b);
}


void CoopSyncTalkPanelClosed()
{
	if (g_applying) return;
	if (CoopNetIsHost())
	{
		if (!CoopNetHasGuests()) return;
		Writer w;
		PutSector(w);
		CoopNetSendToGuests(S_TALK_CLOSE, w.b);
	}
	else if (CoopNetGuestPlaying())
	{
		CoopNetSendToHost(S_TALK_CLOSE, {});
	}
}


bool CoopSyncGuestTalkChoice(UINT8 const approach)
{
	if (!CoopNetGuestPlaying()) return false;
	if (gTalkPanel.face && gTalkPanel.face->fTalking) return true; // as the host's menu: not while talking
	Writer w;
	w.put<UINT8>(approach);
	CoopNetSendToHost(S_TALK_CHOICE, w.b);
	return true;
}


bool CoopSyncGuestMayEnter(UINT32 const screen)
{
	if (!CoopNetGuestPlaying()) return true;
	if (screen == LAPTOP_SCREEN)
	{
		ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, "The laptop belongs to the host in co-op.");
		return false;
	}
	return true;
}
