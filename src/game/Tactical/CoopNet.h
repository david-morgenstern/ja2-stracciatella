#pragma once

// Co-op over the internet. Both games connect out to a relay (see
// tools/coop-relay), so nobody needs to open a port. The host gets a join code
// from the relay; a joining game gives that code and gets a seat.
//
// When someone joins, the host waits for a moment it could save the game,
// saves it and sends the file; the joining game loads it. That works from any
// save and at any time.
//
// The relay address and the player's name live in coop.json in the game's
// home folder (next to ja2.json):
//   { "relay": "example.org:27960", "name": "Dave" }
//
// Everything is polled from the game loop; nothing here blocks for long.

#include "Types.h"
#include <string_theory/string>

#include <vector>

enum class CoopNetState
{
	Off,
	Connecting,     // talking to the relay
	Hosting,        // we have a join code; players may join
	WaitingForSave, // joined; the host is sending the game
	SaveReady,      // the host's game arrived and can be loaded
	Playing,        // joined and loaded
	Failed,         // see CoopNetError()
};

// The relay from coop.json, empty when none is set up.
ST::string CoopNetRelayAddress();

// Start hosting the game that is loaded now.
void CoopNetHost();
// Join a hosted game by its code.
void CoopNetJoin(ST::string const& code);
void CoopNetDisconnect();

// Once per frame.
void CoopNetPoll();

CoopNetState CoopNetGetState();
bool         CoopNetIsHost();
bool         CoopNetIsClient();
ST::string   CoopNetJoinCode();
ST::string   CoopNetError();
// One line on the state of the session, for messages and the main menu.
ST::string   CoopNetStatusText();

// Joining side: the save the host sent, ready for LoadSavedGame(). Calling
// this moves the state on to Playing.
ST::string   CoopNetTakeReceivedSave();
// The name a received game is stored under.
ST::string   CoopNetReceivedSaveName();

// --- used by CoopSync ---
// This PC's seat (0 is the host).
UINT8        CoopNetSeat();
// Joined and playing: this game shows the host's game and does not run its own.
bool         CoopNetGuestPlaying();
// Host: anyone has joined and got the game.
bool         CoopNetHasGuests();
// Messages for CoopSync use kinds from 16 up.
void         CoopNetSendToHost(UINT8 kind, std::vector<UINT8> const& data);
void         CoopNetSendToGuests(UINT8 kind, std::vector<UINT8> const& data);
// Host: send everyone the whole game again, as soon as it can be saved.
void         CoopNetQueueSnapshot();
bool         CoopNetSnapshotPending();
// Host: show a line on every guest's screen.
void         CoopNetTellGuests(ST::string const& text);
