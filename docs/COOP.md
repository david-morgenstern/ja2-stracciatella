# Co-op

Two (engine-wise up to four) players share one Arulco campaign. Each merc on
the player team belongs to one player, and only that player can select and
command them.

## Phase 1: co-op rules in the engine (done)

* **Switching it on**: New Game → House Rules → "Co-op for two players". It is
  stored in a spare byte of the saved game options, so old saves load as
  normal games.
* **Ownership**: `SOLDIERTYPE::ubCoopOwner`, saved in two spare bytes of the
  soldier record (a `0xC0` marker followed by the player). Unmarked records,
  which covers every old save, mean player 1. Hired mercs go to the player at
  the keyboard; so do recruited NPCs.
* **Control**: selection, squad cycling, select-all, drag-select, move-squad,
  stealth toggling and the team panel's next-merc skip only work on the local
  player's mercs. Clicking someone else's merc shows "X takes orders from
  Player N". Other players' mercs have blue names in the team panel.
* **Turns**: in combat every player moves during the player turn. End Turn
  marks the local player as done, and the turn only ends once every player
  who still has a merc who can act is done. Interrupts reset that, and the
  owner of the first interrupter gets the controls.
* **Hot seat** (until networking exists): after End Turn the keyboard passes
  to the next player who isn't done. `Ctrl+Shift+P` passes it by hand,
  `Ctrl+Shift+G` gives the selected merc to the next player.

Code: `src/game/Tactical/Coop.{h,cc}`, tests in `Coop_unittest.cc`.

## Phase 2: network play over a relay (next)

Topology: both games connect out over TCP to a small relay on the FORPSI
server, so neither player has to forward ports. The relay knows nothing about
JA2; it pairs the players of a session by a join code and forwards framed
messages between them.

Sync model, host-authoritative:

1. **Join**: the host saves the game and sends the save file through the
   relay. The client loads it. This also serves as the resync for any desync
   that turns up later, because the save already holds the whole game state.
2. **Commands**: the client never changes the game itself. Its UI actions
   (move, shoot, stance, item use, door, end turn...) become small command
   messages for the host, which checks that the merc belongs to that player
   and runs the command as if it had been clicked locally.
3. **Results**: the host streams what happened (soldier position, direction,
   stance, life, AP, items, plus animation starts) so the client can show it.
   A per-turn checksum of the important state catches drift, and a drift
   gets resolved with a save transfer.
4. **Strategic layer**: the host drives the map screen, laptop and time
   compression; the client watches (read-only map screen) and gets a fresh
   save after each strategic step.

The phase 1 checks (`CoopIsLocalMerc`, `CoopLocalPlayerEndsTurn`) are where
the network layer plugs in: on a client the local player is fixed to its seat
rather than cycled, and turn readiness goes to the host.

Phase 2 work list:

* `Coop/Net`: framed TCP client (SDL_net or plain sockets), message codec.
* Relay server: one small standalone program (sessions, join codes,
  forwarding, keepalives) to deploy on FORPSI.
* Command layer: wrap the UI handlers in `Handle_UI.cc` and
  `Turn_Based_Input.cc` so they emit commands instead of acting directly
  on a client.
* State replication and checksums; save transfer for join and resync.
* Lobby UI: host or join, join code, player list.

## Phase 2 status (2026-11-24): host/join over the relay

David's goal: a **Host Game** button on the main menu (pick any save, play as normal,
get a join code) and **Join Game** (enter the code, join at any time).

Done:
* `tools/coop-relay/relay.py`: relay (asyncio, rooms by 6-letter code, up to 4 seats,
  framed forwarding; protocol in its docstring). `smoke_test.py` checks it (no args:
  local relay; `host port`: a deployed one).
* Deployed on David's server: `/opt/ja2-relay/relay.py`, systemd unit `ja2-relay`
  (from `ja2-relay.service`, DynamicUser), port 27960/tcp opened in ufw. Smoke test passes
  over the internet. To update it, copy relay.py over and run `systemctl restart ja2-relay`.

* Game side (`src/game/Tactical/CoopNet.{h,cc}`, `CoopSocket.{h,cc}`): relay client polled
  from `GameLoop`; main menu **Host Game** (pick a save; it opens once loaded, the join code
  shows in the message log, Ctrl+Shift+J repeats it) and **Join Game** (code box). On join the
  host saves `coop-sync` at the next point `CanGameBeSaved()` allows, sends it and deletes it;
  the joiner stores it as `coop-join.sav` and loads it through the load screen.
  Config: `coop.json` in the home folder (`relay`, `name`); a template is written if missing.
  Testing aid: env `JA2_COOP_AUTOHOST=<save>` / `JA2_COOP_AUTOJOIN=<code>`.
  Verified on 2026-11-24 with two instances on one PC over the live relay: host opened a code,
  joiner joined, 492 KB save arrived and was written. The joiner's load into the game
  was not yet confirmed on screen.

Old plan (steps 1-3 done):
1. Game net client `src/game/Coop/Net.{h,cc}`: non-blocking TCP (winsock/BSD),
   the relay framing, polled once per frame from the game loop. Relay address
   and player name from `%APPDATA%\JA2\coop.json`.
2. Main menu (`MainMenuScreen.cc`): the image buttons fill y=277..425, so add Host/Join
   as `CreateTextButton`s beside the column. Host → `SAVE_LOAD_SCREEN` with a "host after
   load" flag. Join → code entry, then a waiting box.
3. Join snapshot: on PEER_JOINED the host waits for `CanGameBeSaved()`, then calls
   `SaveGame("coop-sync", ...)`, reads `GetSaveGamePath("coop-sync")` and sends it in chunks. The client
   writes it to a save path and loads it the way `DoneFadeOutForSaveLoadScreen` in
   `SaveLoadScreen.cc` does (`LoadSavedGame`, then `guiScreenToGotoAfterLoadingSavedGame`).
4. Then: client commands run by the host, and live state replication (the design is above).


## Live sync (2026-11-24)

`src/game/Tactical/CoopSync.{h,cc}`: the host's game is the only one that plays.
* Host -> guests ~10/s (`S_STATE`): sector, whose turn, fight on/off, clock, and per soldier in
  the sector: position, animation and frame, facing, life, bleeding, breath, AP, visibility.
* Host -> guests, whole game again (save transfer) when the sector's "shape" changes: sector,
  fight, current team, world item count, soldiers arriving/dying/changing level or owner. Sent
  once `CanGameBeSaved()` and no attack is busy, at most every 1.5 s. The guest reloads in place
  (`LoadSaveInPlace`) and keeps its screen and selected merc.
* Guest -> host orders for its own mercs: move (with door/switch use), attack (HandleItem with
  aim/burst/weapon mode), stance, turn, end turn; resync request. The host checks ownership.
* Guest freezes its own soldier animation, AI, bombs and clock (`CoopSyncGuestFrozen`).
* Joining player who owns nobody gets every other merc in the sector.
Verified 2026-11-24 over the live relay: guest order -> host walks Ghost 13507->13511 ->
guest sees him at 13511. Not yet verified: combat turns, attacks, doors, sector changes.
Known gaps: guest inventory/item handling, laptop/map actions, talking to NPCs are host-only;
the guest's own view of bullets and explosions is only the result.
Test aids: `JA2_COOP_TEST_TACTICAL=1` (host loads into tactical), `JA2_COOP_TEST_GUEST_MOVE=1`.

## No more reloads during play (2026-11-24)

The guest now reloads only when joining, when the sector changes, when a player's own
merc arrives/leaves/dies, or when a soldier climbs a roof. Everything else is live:
* fights starting/ending and whose turn it is (turn bar, UI lock, first own merc selected);
* soldiers arriving (`S_SOLDIER`: a whole save-format record, created with
  `TacticalCreateSoldierFromExisting`) and leaving (missing from the stream -> removed);
* corpses (`S_CORPSE`, the 160-byte save record; hook in `AddRottingCorpse`);
* items on the ground (`S_ITEM` per world item slot; hooks in `AddItemToWorld` /
  `RemoveItemFromWorld`; the guest puts items in the host's slot via `SetNextWorldItemIndex`);
  the guest's own drops and pickups become `S_ITEM_DROP` / `S_ITEM_TAKE` requests;
* doors (`S_DOOR`; hook in `HandleDoorChangeFromGridNo`);
* walking to pick something up (the move carries the pending action; a choose-items
  pickup becomes "take all" so no menu opens on the host).
Soldier updates go out 20 times a second.
Verified 2026-11-24 (two copies, live relay): one reload in the whole session; a host item
drop appeared on the guest; enemy and player turns switched live; both players' End Turn
needed. Not yet verified: corpses, arrivals, doors, guest drops.
Test aid: `JA2_COOP_TEST_LIVE=1` on the host (drops an item, starts a fight, ends the turn).
* Sight (`S_SIGHT`, 5/s): our team's public opplist, last known positions, bOppCnt and every
  own merc's bOppList for the soldiers in the sector, so the guest's targeting and enemy
  markers match the host's.
* Shots (`S_SHOT`, hook in `FireBulletGivenTarget`): the guest plays the shot or burst sound
  and flies a bullet only for show (it ends at the first structure; hits come from the host).
Verified 2026-11-24: host shot an enemy; guest heard/saw it, the enemy was removed and his
gear appeared live; the guest's sight count followed the host's; still one reload per session.
* Speech (`S_SPEECH`, hook in `ExecuteCharacterDialogue`): every quote the host plays (mercs,
  NPCs in conversations, banter) is queued on the guests with that character's face; the
  guest needs the same mods for modded voices/banter.
* Battle sounds (`S_BATTLE_SOUND`, hook in `InternalDoMercBattleSound`), except the host's own
  "yes sir" acknowledgements for its own mercs.
* Explosions (`S_EXPLOSION`, hook in `InternalIgniteExplosion`): the guest plays the blast
  animation and sound via `ShowExplosionOnly` (no damage keyframe, no explosion data).
* A guest starting a conversation (`InitiateConversationFull`) sends `S_TALK`; the host's game
  runs it (talk panel on the host's screen) and everybody hears it. Handing items to people
  from the guest is refused for now.
Test runs skip autosaving (`JA2_COOP_TEST_LIVE`): earlier test runs on 2026-11-24 overwrote
David's Auto02 (14:02) and maybe Auto01 (13:55).

## Effects, conversations, strategic (2026-11-24)
* Map damage (`S_MAP_CHANGE`): every MODIFY_MAP record the host writes for the loaded sector
  (windows, walls, removed/added structures, damaged structs) is applied on the guest with
  `ApplyMapChangeLive` (the apply switch of `LoadAllMapChangesFromMapTempFileAndApplyThem`
  moved into `ApplyMapChange`).
* Smoke/gas (`S_SMOKE`): hooks in `AddSmokeEffectToTile` / `RemoveSmokeEffectFromTile`.
* Blood (`S_BLOOD`): hook in `InternalDropBlood`. Bullet misses (`S_MISS`): hook in
  `StructureHit`, guest shows `ShowBulletMissOnly`. Muzzle flash: guest lights it with each
  `S_SHOT` for 150 ms. Damage numbers ride along in `S_STATE`.
* Conversations: host's `InternalInitiateConversation` -> `S_TALK_OPEN`, guest opens the same
  panel (`CoopShowTalkPanel`); NPC lines go into that panel; a choice clicked on the guest is
  `S_TALK_CHOICE` -> host `ChooseTalkApproach`; closing on either side closes both
  (`S_TALK_CLOSE`). The guest skips the story check that closing a panel runs.
* Strategic: the guest cannot open the laptop; entering or leaving the map screen asks the host
  for a fresh copy (the map screen is a screen change anyway).
Verified 2026-11-24 (two copies, live relay): explosions, smoke tiles, blood, window shatter
(2 records = 2 swaps, as on the host), conversation with Waldo opening/greeting/closing, shots;
one reload per session; saves untouched.
