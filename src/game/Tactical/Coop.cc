#include "Coop.h"

#include "Font_Control.h"
#include "GameSettings.h"
#include "Interface.h"
#include "Message.h"
#include "Overhead.h"
#include "PathAI.h"
#include "Soldier_Control.h"
#include "Soldier_Macros.h"

#include <string_theory/format>

namespace
{
	UINT8 g_local_player = 0;
	bool  g_turn_ready[COOP_MAX_PLAYERS] = {};
	bool  g_local_fixed = false;
	bool  g_absent[COOP_MAX_PLAYERS] = {};

	// marks a soldier record that carries an owner; old saves hold other bytes there
	constexpr UINT8 OWNER_MARKER = 0xC0;
}


bool CoopActive()
{
	return gGameOptions.ubCoopPlayers >= 2;
}


UINT8 CoopPlayerCount()
{
	UINT8 const n = gGameOptions.ubCoopPlayers;
	return n < 2 ? 1 : (n > COOP_MAX_PLAYERS ? COOP_MAX_PLAYERS : n);
}


UINT8 CoopLocalPlayer()
{
	return CoopActive() && g_local_player < CoopPlayerCount() ? g_local_player : 0;
}


void CoopSetLocalPlayer(UINT8 const player)
{
	if (g_local_fixed) return;
	g_local_player = player < CoopPlayerCount() ? player : 0;
}


void CoopCycleLocalPlayer()
{
	CoopSetLocalPlayer((CoopLocalPlayer() + 1) % CoopPlayerCount());
}


void CoopFixLocalPlayer(UINT8 const player)
{
	g_local_fixed  = false;
	g_local_player = player < COOP_MAX_PLAYERS ? player : 0;
	g_local_fixed  = true;
}


void CoopReleaseLocalPlayer()
{
	g_local_fixed = false;
	for (bool& a : g_absent) a = false;
}


bool CoopIsLocalPlayerFixed()
{
	return g_local_fixed;
}


void CoopSetPlayerPresent(UINT8 const player, bool const present)
{
	if (player < COOP_MAX_PLAYERS) g_absent[player] = !present;
}


bool CoopIsPlayerPresent(UINT8 const player)
{
	return player < COOP_MAX_PLAYERS && (!g_absent[player] || player == CoopLocalPlayer());
}


ST::string CoopPlayerName(UINT8 const player)
{
	return ST::format("Player {}", player + 1);
}


UINT8 CoopOwner(SOLDIERTYPE const& s)
{
	return s.ubCoopOwner < CoopPlayerCount() ? s.ubCoopOwner : 0;
}


void CoopSetOwner(SOLDIERTYPE& s, UINT8 const player)
{
	s.ubCoopOwner = player < COOP_MAX_PLAYERS ? player : 0;
}


void CoopCycleOwner(SOLDIERTYPE& s)
{
	CoopSetOwner(s, (CoopOwner(s) + 1) % CoopPlayerCount());
}


bool CoopIsLocalMerc(SOLDIERTYPE const& s)
{
	if (!CoopActive()) return true;
	// only the player team is split between players; anyone else is nobody's
	if (s.bTeam != OUR_TEAM) return true;
	return CoopOwner(s) == CoopLocalPlayer();
}


void CoopResetTurnReady()
{
	for (bool& r : g_turn_ready) r = false;
}


bool CoopPlayerCanAct(UINT8 const player)
{
	CFOR_EACH_IN_TEAM(s, OUR_TEAM)
	{
		if (CoopOwner(*s) != player)          continue;
		if (!OkControllableMerc(s))           continue;
		if (!OK_INTERRUPT_MERC(s))            continue;
		if (s->uiStatusFlags & SOLDIER_VEHICLE) continue;
		return true;
	}
	return false;
}


void CoopEveryoneEndsTurn()
{
	for (bool& r : g_turn_ready) r = true;
}


bool CoopIsPlayerReady(UINT8 const player)
{
	return player < COOP_MAX_PLAYERS && (g_turn_ready[player] || !CoopIsPlayerPresent(player) || !CoopPlayerCanAct(player));
}


bool CoopLocalPlayerEndsTurn()
{
	return CoopPlayerEndsTurn(CoopLocalPlayer());
}


bool CoopPlayerEndsTurn(UINT8 const player)
{
	if (!CoopActive()) return true;

	if (player < COOP_MAX_PLAYERS) g_turn_ready[player] = true;
	for (UINT8 p = 0; p != CoopPlayerCount(); ++p)
	{
		if (!CoopIsPlayerReady(p)) return false;
	}
	return true;
}


void CoopPackOwner(UINT8 const owner, UINT8 out[2])
{
	if (owner == 0)
	{ // player 0 is what an unmarked record means, so keep the old zeroes
		out[0] = 0;
		out[1] = 0;
		return;
	}
	out[0] = OWNER_MARKER;
	out[1] = owner;
}


UINT8 CoopUnpackOwner(UINT8 const in[2])
{
	if (in[0] != OWNER_MARKER || in[1] >= COOP_MAX_PLAYERS) return 0;
	return in[1];
}


static SOLDIERTYPE* FirstMercOf(UINT8 const player)
{
	FOR_EACH_IN_TEAM(s, OUR_TEAM)
	{
		if (CoopOwner(*s) != player)            continue;
		if (!OkControllableMerc(s))             continue;
		if (!OK_INTERRUPT_MERC(s))              continue;
		if (s->uiStatusFlags & SOLDIER_VEHICLE) continue;
		if (s->fMercAsleep)                     continue;
		return s;
	}
	return nullptr;
}


void CoopSwitchToPlayer(UINT8 const player)
{
	if (!CoopActive()) return;
	if (g_local_fixed)
	{ // network play: every PC keeps its own player
		if (player != CoopLocalPlayer()) ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, "You play " + CoopPlayerName(CoopLocalPlayer()) + "; the others play on their own PCs.");
		return;
	}

	CoopSetLocalPlayer(player);
	ErasePath();
	fInterfacePanelDirty = DIRTYLEVEL2; // names show whose merc is whose

	// drop multi selections; they may hold the other player's mercs
	FOR_EACH_IN_TEAM(s, OUR_TEAM) s->uiStatusFlags &= ~SOLDIER_MULTI_SELECTED;

	SOLDIERTYPE* const s = FirstMercOf(CoopLocalPlayer());
	if (s)
	{
		SelectSoldier(s, SELSOLDIER_FORCE_RESELECT);
		LocateSoldier(s, TRUE);
	}
	else
	{
		SetSelectedMan(nullptr);
	}

	ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, s ?
		CoopPlayerName(CoopLocalPlayer()) + " has the controls." :
		CoopPlayerName(CoopLocalPlayer()) + " has the controls, but no merc who can act.");
}


void CoopStartPlayerTurn()
{
	CoopResetTurnReady();
	if (!CoopActive()) return;

	for (UINT8 p = 0; p != CoopPlayerCount(); ++p)
	{
		if (!CoopPlayerCanAct(p)) continue;
		if (p != CoopLocalPlayer() && !g_local_fixed) CoopSwitchToPlayer(p);
		return;
	}
}


void CoopHandOverAfterEndTurn()
{
	if (!CoopActive()) return;

	UINT8 const n    = CoopPlayerCount();
	UINT8 const done = CoopLocalPlayer();
	for (UINT8 i = 1; i != n; ++i)
	{
		UINT8 const p = (done + i) % n;
		if (CoopIsPlayerReady(p)) continue;
		ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, CoopPlayerName(done) + " is done. Waiting for " + CoopPlayerName(p) + ".");
		if (!g_local_fixed) CoopSwitchToPlayer(p);
		return;
	}
}
