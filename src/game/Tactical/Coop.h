#pragma once

// Co-op: several players share one campaign, and each merc belongs to one of
// them. The game is switched on for co-op on the new game screen (House
// Rules), and the setting is kept in the save.
//
// Only the player who owns a merc may select or command it. In combat both
// players move their mercs during the player turn, and the turn ends once
// every player who still has someone to move has pressed End Turn.
//
// This layer knows nothing about networking. Today the players share one PC
// (hot seat: Ctrl+Shift+P hands the keyboard over); a network session will
// later pin the local player to the seat of the person at this PC.

#include "Types.h"
#include <string_theory/string>

struct SOLDIERTYPE;

constexpr UINT8 COOP_MAX_PLAYERS = 4;

// True when the campaign is a co-op one.
bool  CoopActive();
// Number of players, 1 when co-op is off.
UINT8 CoopPlayerCount();

// The player at the keyboard of this PC.
UINT8 CoopLocalPlayer();
void  CoopSetLocalPlayer(UINT8 player);
// Hot seat: hand the keyboard to the next player.
void  CoopCycleLocalPlayer();

// Network play: this PC is that player for good, and the keyboard is never
// handed over. Released when the session ends.
void  CoopFixLocalPlayer(UINT8 player);
void  CoopReleaseLocalPlayer();
bool  CoopIsLocalPlayerFixed();

// Network play: a player who is not connected holds nobody up. Everyone counts
// as present in a hot seat game.
void  CoopSetPlayerPresent(UINT8 player, bool present);
bool  CoopIsPlayerPresent(UINT8 player);

// Display name of a player, e.g. "Player 2".
ST::string CoopPlayerName(UINT8 player);

UINT8 CoopOwner(SOLDIERTYPE const&);
void  CoopSetOwner(SOLDIERTYPE&, UINT8 player);
// Give the merc to the next player.
void  CoopCycleOwner(SOLDIERTYPE&);

// True when the person at this PC may control this merc. Always true when
// co-op is off, so callers need no special case.
bool  CoopIsLocalMerc(SOLDIERTYPE const&);

// Turn-based combat. Forget who is ready; called when a player turn starts.
void  CoopResetTurnReady();
// The local player pressed End Turn. Returns true when the turn may end now,
// that is every player is ready or has nobody left who can act.
bool  CoopLocalPlayerEndsTurn();
// The same for any player, e.g. a guest over the network.
bool  CoopPlayerEndsTurn(UINT8 player);
// The turn ends for everyone, e.g. when a timed turn runs out.
void  CoopEveryoneEndsTurn();
bool  CoopIsPlayerReady(UINT8 player);
// True when the player owns a merc who can still act this turn.
bool  CoopPlayerCanAct(UINT8 player);

// Hot seat: give the keyboard to this player, select one of their mercs and
// tell them it is their go.
void  CoopSwitchToPlayer(UINT8 player);
// A player turn starts: nobody is ready, and the first player who can act
// gets the keyboard.
void  CoopStartPlayerTurn();
// The local player ended the turn but someone else still has to: hand the
// keyboard to the next player who is not done.
void  CoopHandOverAfterEndTurn();

// Save game packing: the owner sits in two spare bytes of the soldier record,
// a marker and the player. Records without the marker belong to player 0.
void  CoopPackOwner(UINT8 const owner, UINT8 out[2]);
UINT8 CoopUnpackOwner(UINT8 const in[2]);
