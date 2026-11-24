#pragma once

// Keeps a joined game showing the host's game (see CoopNet.h for the
// connection). The host's game is the only one that plays:
//
//  * about ten times a second the host sends where every soldier in the
//    sector is, what he is doing and how he is (G_STATE);
//  * when something bigger changes (a fight starts or ends, a turn starts,
//    someone arrives or dies, the sector changes) the host sends the whole
//    game again and the guest reloads it;
//  * the guest does not move anybody itself: its orders for its own mercs
//    (move, attack, stance, turn, end turn) go to the host, which carries
//    them out as if they had been clicked there.

#include "JA2Types.h"
#include "Rotting_Corpses.h"
#include "Types.h"

#include <string_theory/string>

// Once per frame, from CoopNetPoll.
void CoopSyncPoll();
void CoopSyncOnMessage(UINT8 from, UINT8 kind, UINT8 const* data, size_t len);

// Guest: the game is to come from the host, so do not run soldiers, AI or the
// clock here.
bool CoopSyncGuestFrozen();

// Guest: a fresh copy of the host's game arrived under this save name.
void CoopSyncLoadSnapshot(ST::string const& save_name);
// Guest: called by the load screen once that copy is loaded.
void CoopSyncAfterSnapshotLoaded();

// Guest: orders for the guest's mercs. Each returns true when it sent the
// order to the host, in which case the caller must not carry it out itself.
bool CoopSyncGuestMove(SOLDIERTYPE&, GridNo dest, UINT16 movement_mode, bool reverse, GridNo interactive_gridno, UINT8 interactive_direction);
bool CoopSyncGuestAttack(SOLDIERTYPE&, GridNo target, INT8 level, UINT16 item);
bool CoopSyncGuestStance(SOLDIERTYPE&, INT8 stance);
bool CoopSyncGuestTurn(SOLDIERTYPE&, GridNo towards);
bool CoopSyncGuestEndTurn();
// Guest: ask the host for the whole game again (Ctrl+Shift+U).
void CoopSyncRequestResync();

// Hooks in the engine: something changed that the other side must see.
void CoopSyncWorldItemChanged(INT32 index);
void CoopSyncCorpseAdded(ROTTING_CORPSE_DEFINITION const&);
void CoopSyncDoorChanged(GridNo);
void CoopSyncSpeech(UINT8 profile, UINT16 quote, bool alternate_file);
void CoopSyncBattleSound(SOLDIERTYPE const&, UINT8 sound, bool lower_volume);
void CoopSyncExplosion(INT16 x, INT16 y, INT16 z, INT16 grid, UINT16 item, INT8 level);
// Guest: a conversation goes to the host. True when it did (and is not to
// be started here).
bool CoopSyncGuestTalk(SOLDIERTYPE const& npc, SOLDIERTYPE const& merc, UINT8 approach, UINT8 record, bool with_item);
struct MODIFY_MAP;
void CoopSyncMapChange(MODIFY_MAP const&);
void CoopSyncSmokeTile(INT16 grid, INT8 level, UINT8 smoke_type, UINT8 duration, INT8 age, bool add);
void CoopSyncBlood(INT16 grid, INT8 level, UINT8 kind, UINT8 strength, INT8 visible);
void CoopSyncBulletMiss(INT16 grid, INT16 x, INT16 y, INT16 z);
void CoopSyncTalkPanelOpened(SOLDIERTYPE const& npc, SOLDIERTYPE const* merc);
void CoopSyncTalkPanelClosed();
// Guest: a choice in the talking menu goes to the host. True when it did.
bool CoopSyncGuestTalkChoice(UINT8 approach);
// Guest: may it go to this screen? (The laptop is the host's.)
bool CoopSyncGuestMayEnter(UINT32 screen);
void CoopSyncShotFired(SOLDIERTYPE const& firer, FLOAT x, FLOAT y, FLOAT z, UINT16 item, INT16 hit_by, BOOLEAN buckshot);
