#include "CoopNet.h"

#include "ContentManager.h"
#include "Coop.h"
#include "CoopSocket.h"
#include "CoopSync.h"
#include "DirFs.h"
#include "Font_Control.h"
#include "GameInstance.h"
#include "GameSettings.h"
#include "JAScreens.h"
#include "Json.h"
#include "Logger.h"
#include "Overhead.h"
#include "Soldier_Control.h"
#include "Message.h"
#include "SaveLoadGame.h"
#include "ScreenIDs.h"
#include "SGPFile.h"
#include "Options_Screen.h"

#include <string_theory/format>

#include <cstdlib>
#include <deque>
#include <memory>
#include <vector>

extern ScreenID guiCurrentScreen;
extern BOOLEAN gfWorldLoaded;

namespace
{
	// relay frames (tools/coop-relay/relay.py)
	enum : UINT8
	{
		R_HOST = 1, R_CODE = 2, R_JOIN = 3, R_JOINED = 4, R_ERROR = 5,
		R_SEND = 6, R_FROM = 7, R_PEER_JOINED = 8, R_PEER_LEFT = 9,
	};
	constexpr UINT8 TO_EVERYONE = 255;

	// game messages, carried in R_SEND / R_FROM
	enum : UINT8
	{
		G_HELLO = 1,      // u32 protocol, name           joiner -> host
		G_REJECT = 2,     // reason                       host -> joiner
		G_SAVE_BEGIN = 3, // u32 size                     host -> joiner
		G_SAVE_DATA = 4,  // bytes
		G_SAVE_END = 5,
		G_TEXT = 6,       // a line to show          host -> joiner
		G_SYNC_FIRST = 16 // and up: CoopSync's
	};
	constexpr UINT32 PROTOCOL = 1;
	constexpr size_t SAVE_CHUNK = 256 * 1024;
	constexpr UINT32 MAX_FRAME = 64 * 1024 * 1024;

	char const SYNC_SAVE[] = "coop-sync";
	char const JOIN_SAVE[] = "coop-join";

	struct Session
	{
		CoopNetState state = CoopNetState::Off;
		bool host = false;
		CoopSocket::Handle sock = CoopSocket::INVALID;
		std::vector<UINT8> in;
		std::vector<UINT8> out;
		size_t out_pos = 0;
		ST::string code;
		ST::string error;
		UINT8 seat = 0;
		ST::string names[COOP_MAX_PLAYERS];
		std::deque<UINT8> want_save; // host: seats waiting for the game
		bool joined[COOP_MAX_PLAYERS] = {}; // host: seats that said hello
		std::vector<UINT8> save;     // joiner: the game as it arrives
		UINT32 save_size = 0;
	};
	Session g;


	void Msg(ST::string const& text)
	{
		SLOGI("co-op: {}", text);
		ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, text);
	}


	struct Config { ST::string relay; ST::string name; };

	Config ReadConfig()
	{
		Config c{ {}, "Player" };
		DirFs* const home = GCM->userPrivateFiles();
		try
		{
			if (!home->exists("coop.json"))
			{ // leave a template behind so there is something to fill in
				std::unique_ptr<SGPFile> f{ home->openForWriting("coop.json") };
				ST::string const tmpl = "{\n  \"relay\": \"\",\n  \"name\": \"Player\"\n}\n";
				f->write(tmpl.c_str(), tmpl.size());
				return c;
			}
			std::unique_ptr<SGPFile> f{ home->openForReading("coop.json") };
			JsonObject const o = JsonValue::deserialize(f->readStringToEnd()).toObject();
			c.relay = o.getOptionalString("relay").trim();
			c.name  = o.getOptionalString("name", "Player").trim();
			if (c.name.empty()) c.name = "Player";
		}
		catch (std::exception const& e)
		{
			SLOGE("co-op: cannot read coop.json: {}", e.what());
		}
		return c;
	}


	void Fail(ST::string const& why)
	{
		CoopSocket::Close(g.sock);
		g.sock  = CoopSocket::INVALID;
		g.state = CoopNetState::Failed;
		g.error = why;
		g.want_save.clear();
		g.save.clear();
		Msg("Co-op: " + why);
	}


	void PutU32(std::vector<UINT8>& v, UINT32 const x)
	{
		for (int i = 0; i != 4; ++i) v.push_back(static_cast<UINT8>(x >> (8 * i)));
	}

	UINT32 GetU32(UINT8 const* p)
	{
		return p[0] | p[1] << 8 | p[2] << 16 | static_cast<UINT32>(p[3]) << 24;
	}


	void SendFrame(UINT8 const kind, UINT8 const* data, size_t const len)
	{
		PutU32(g.out, static_cast<UINT32>(len + 1));
		g.out.push_back(kind);
		g.out.insert(g.out.end(), data, data + len);
	}


	// a game message to one seat (or TO_EVERYONE)
	void SendGame(UINT8 const to, UINT8 const kind, UINT8 const* data = nullptr, size_t const len = 0)
	{
		PutU32(g.out, static_cast<UINT32>(len + 3));
		g.out.push_back(R_SEND);
		g.out.push_back(to);
		g.out.push_back(kind);
		if (len) g.out.insert(g.out.end(), data, data + len);
	}


	bool Flush()
	{
		while (g.out_pos < g.out.size())
		{
			long const n = CoopSocket::Send(g.sock, g.out.data() + g.out_pos, g.out.size() - g.out_pos);
			if (n < 0) return false;
			if (n == 0) break;
			g.out_pos += static_cast<size_t>(n);
		}
		if (g.out_pos == g.out.size())
		{
			g.out.clear();
			g.out_pos = 0;
		}
		else if (g.out_pos > (4u << 20))
		{ // drop what is sent so the buffer does not grow forever
			g.out.erase(g.out.begin(), g.out.begin() + g.out_pos);
			g.out_pos = 0;
		}
		return true;
	}


	bool Connect()
	{
		Config const c = ReadConfig();
		if (c.relay.empty())
		{
			Fail("no relay set up. Put its address in coop.json in the game folder.");
			return false;
		}
		ST::string host = c.relay;
		UINT16 port = 27960;
		auto const colon = c.relay.find_last(':');
		if (colon != -1)
		{
			host = c.relay.substr(0, colon);
			port = static_cast<UINT16>(c.relay.substr(colon + 1).to_uint(10));
		}

		CoopSocket::Close(g.sock);
		g = Session{};
		g.names[0] = c.name;
		g.state = CoopNetState::Connecting;

		std::string err;
		g.sock = CoopSocket::Connect(host.to_std_string(), port, 5000, err);
		if (g.sock == CoopSocket::INVALID)
		{
			Fail(ST::string("cannot connect to the relay: ") + err);
			return false;
		}
		return true;
	}


	void SendSaveTo(std::vector<UINT8> const& save, UINT8 const seat)
	{
		std::vector<UINT8> size;
		PutU32(size, static_cast<UINT32>(save.size()));
		SendGame(seat, G_SAVE_BEGIN, size.data(), size.size());
		for (size_t at = 0; at < save.size(); at += SAVE_CHUNK)
		{
			size_t const n = std::min(SAVE_CHUNK, save.size() - at);
			SendGame(seat, G_SAVE_DATA, save.data() + at, n);
		}
		SendGame(seat, G_SAVE_END);
	}


	// host: save the game now and send it to everyone who is waiting for it
	void SendSaveToWaitingPlayers()
	{
		if (g.want_save.empty()) return;
		if (guiCurrentScreen != GAME_SCREEN && guiCurrentScreen != MAP_SCREEN) return;
		if (!CanGameBeSaved()) return;

		// A save records the screen to come back to after loading, and takes it
		// from guiPreviousOptionScreen, which still names whatever screen came
		// before (the main menu, say). The joiner belongs where we are now.
		ScreenID const previous = guiPreviousOptionScreen;
		guiPreviousOptionScreen = guiCurrentScreen;
		BOOLEAN const saved = SaveGame(SYNC_SAVE, "Co-op sync");
		guiPreviousOptionScreen = previous;
		if (!saved)
		{
			SLOGW("co-op: could not save for a joining player, will try again");
			return;
		}
		std::vector<UINT8> save;
		try
		{
			ST::string const path = GetSaveGamePath(SYNC_SAVE);
			{
				std::unique_ptr<SGPFile> f{ GCM->saveGameFiles()->openForReading(path) };
				save = f->readToEnd();
			}
			GCM->saveGameFiles()->deleteFile(path);
		}
		catch (std::exception const& e)
		{
			SLOGE("co-op: cannot read the sync save: {}", e.what());
			return;
		}

		while (!g.want_save.empty())
		{
			UINT8 const seat = g.want_save.front();
			g.want_save.pop_front();
			SendSaveTo(save, seat);
			SLOGI("co-op: sent the game to seat {} ({} KB)", seat, save.size() / 1024);
		}
	}


	void OnGameMessage(UINT8 const from, UINT8 const kind, UINT8 const* p, size_t const len)
	{
		if (kind >= G_SYNC_FIRST)
		{
			CoopSyncOnMessage(from, kind, p, len);
			return;
		}
		if (g.host)
		{
			if (kind == G_HELLO && from < COOP_MAX_PLAYERS && len >= 4)
			{
				ST::string const name = ST::string::from_utf8(reinterpret_cast<char const*>(p + 4), len - 4, ST::substitute_invalid);
				if (GetU32(p) != PROTOCOL)
				{
					ST::string const why = "The host runs a different version of the game.";
					SendGame(from, G_REJECT, reinterpret_cast<UINT8 const*>(why.c_str()), why.size());
					return;
				}
				g.names[from] = name.empty() ? CoopPlayerName(from) : name;
				if (gGameOptions.ubCoopPlayers < from + 1) gGameOptions.ubCoopPlayers = from + 1;
				CoopSetPlayerPresent(from, true);
				g.joined[from] = true;

				// a newcomer with nobody to command gets every other merc here
				bool owns_any = false;
				for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
				{
					SOLDIERTYPE const& m = GetMan(i);
					owns_any = owns_any || (m.bActive && m.bTeam == OUR_TEAM && CoopOwner(m) == from);
				}
				if (!owns_any)
				{
					std::vector<ST::string> given;
					int n = 0;
					for (UINT32 i = 0; i != TOTAL_SOLDIERS; ++i)
					{
						SOLDIERTYPE& m = GetMan(i);
						if (!m.bActive || m.bTeam != OUR_TEAM || !m.bInSector || m.uiStatusFlags & SOLDIER_VEHICLE) continue;
						if (CoopOwner(m) != 0) continue;
						if (n++ % 2 == 1) { CoopSetOwner(m, from); given.push_back(m.name); }
					}
					ST::string list;
					for (ST::string const& x : given) list += (list.empty() ? "" : ", ") + x;
					if (!given.empty()) Msg(g.names[from] + " takes " + list + ". Ctrl+Shift+G hands a merc over.");
				}
				g.want_save.push_back(from);
				Msg(ST::format("{} joined as {}. Sending the game as soon as it can be saved...", g.names[from], CoopPlayerName(from)));
			}
			return;
		}

		if (from != 0) return; // joiners only listen to the host
		switch (kind)
		{
			case G_TEXT:
				ScreenMsg(FONT_MCOLOR_LTYELLOW, MSG_INTERFACE, ST::string::from_utf8(reinterpret_cast<char const*>(p), len, ST::substitute_invalid));
				break;

			case G_REJECT:
				Fail(ST::string::from_utf8(reinterpret_cast<char const*>(p), len, ST::substitute_invalid));
				break;

			case G_SAVE_BEGIN:
				if (len < 4 || GetU32(p) > MAX_FRAME) { Fail("the host sent a broken game."); break; }
				g.save_size = GetU32(p);
				g.save.clear();
				g.save.reserve(g.save_size);
				break;

			case G_SAVE_DATA:
				g.save.insert(g.save.end(), p, p + len);
				break;

			case G_SAVE_END:
				if (g.save.size() != g.save_size) { Fail("the host's game arrived incomplete."); break; }
				try
				{
					std::unique_ptr<SGPFile> f{ GCM->saveGameFiles()->openForWriting(GetSaveGamePath(JOIN_SAVE)) };
					f->write(g.save.data(), g.save.size());
				}
				catch (std::exception const& e)
				{
					Fail(ST::format("cannot store the host's game: {}", e.what()));
					break;
				}
				g.save.clear();
				g.save.shrink_to_fit();
				g.save_size = 0;
				if (g.state == CoopNetState::Playing)
				{ // a fresh copy of the host's game while we play
					CoopSyncLoadSnapshot(JOIN_SAVE);
				}
				else
				{
					g.state = CoopNetState::SaveReady;
				}
				break;
		}
	}


	void OnFrame(UINT8 const kind, UINT8 const* p, size_t const len)
	{
		switch (kind)
		{
			case R_CODE:
				g.code  = ST::string::from_utf8(reinterpret_cast<char const*>(p), len, ST::substitute_invalid);
				g.state = CoopNetState::Hosting;
				Msg("Co-op game open. Join code: " + g.code + "   (Ctrl+Shift+J shows it again)");
				break;

			case R_JOINED:
				if (len < 1) break;
				g.seat  = p[0];
				g.state = CoopNetState::WaitingForSave;
				{
					std::vector<UINT8> hello;
					PutU32(hello, PROTOCOL);
					hello.insert(hello.end(), g.names[0].c_str(), g.names[0].c_str() + g.names[0].size());
					g.names[g.seat] = g.names[0];
					g.names[0] = "the host";
					SendGame(0, G_HELLO, hello.data(), hello.size());
				}
				break;

			case R_ERROR:
				Fail(ST::string::from_utf8(reinterpret_cast<char const*>(p), len, ST::substitute_invalid));
				break;

			case R_PEER_JOINED:
				if (len >= 1) SLOGI("co-op: seat {} connected", p[0]);
				break;

			case R_PEER_LEFT:
				if (len >= 1 && p[0] < COOP_MAX_PLAYERS)
				{
					CoopSetPlayerPresent(p[0], false);
					g.joined[p[0]] = false;
					Msg(ST::format("{} left the game.", g.names[p[0]].empty() ? CoopPlayerName(p[0]) : g.names[p[0]]));
				}
				break;

			case R_FROM:
				if (len >= 2) OnGameMessage(p[0], p[1], p + 2, len - 2);
				break;
		}
	}
}


ST::string CoopNetRelayAddress()
{
	return ReadConfig().relay;
}


void CoopNetHost()
{
	if (!Connect()) return;

	// testing aid: go to tactical rather than the map after loading
	if (getenv("JA2_COOP_TEST_TACTICAL") && gfWorldLoaded) guiScreenToGotoAfterLoadingSavedGame = GAME_SCREEN;
	g.host = true;
	g.seat = 0;

	// a hosted game is a co-op game, and this PC is seat 0 for good
	if (gGameOptions.ubCoopPlayers < 2) gGameOptions.ubCoopPlayers = 2;
	CoopFixLocalPlayer(0);
	for (UINT8 p = 1; p != COOP_MAX_PLAYERS; ++p) CoopSetPlayerPresent(p, false);

	SendFrame(R_HOST, nullptr, 0);
}


void CoopNetJoin(ST::string const& code)
{
	if (!Connect()) return;
	g.host = false;
	ST::string const c = code.trim().to_upper();
	SendFrame(R_JOIN, reinterpret_cast<UINT8 const*>(c.c_str()), c.size());
}


void CoopNetDisconnect()
{
	CoopSocket::Close(g.sock);
	g = Session{};
	CoopReleaseLocalPlayer();
}


void CoopNetPoll()
{
	if (g.sock == CoopSocket::INVALID) return;

	UINT8 buf[64 * 1024];
	for (;;)
	{
		long const n = CoopSocket::Recv(g.sock, buf, sizeof(buf));
		if (n == 0) break;
		if (n < 0)
		{
			Fail(g.host ? "lost the connection to the relay; nobody can join now." : "lost the connection to the host.");
			return;
		}
		g.in.insert(g.in.end(), buf, buf + n);
	}

	size_t at = 0;
	while (g.in.size() - at >= 4)
	{
		UINT32 const len = GetU32(g.in.data() + at);
		if (len == 0 || len > MAX_FRAME) { Fail("the relay sent garbage."); return; }
		if (g.in.size() - at - 4 < len) break;
		UINT8 const* const frame = g.in.data() + at + 4;
		at += 4 + len;
		OnFrame(frame[0], frame + 1, len - 1);
		if (g.sock == CoopSocket::INVALID) return; // failed while handling it
	}
	if (at) g.in.erase(g.in.begin(), g.in.begin() + at);

	CoopSyncPoll();
	if (g.sock == CoopSocket::INVALID) return;
	if (g.host) SendSaveToWaitingPlayers();

	if (!Flush()) Fail("lost the connection to the relay.");
}


CoopNetState CoopNetGetState() { return g.state; }
bool         CoopNetIsHost()   { return g.host && g.sock != CoopSocket::INVALID; }
bool         CoopNetIsClient() { return !g.host && g.sock != CoopSocket::INVALID; }
ST::string   CoopNetJoinCode() { return g.code; }
ST::string   CoopNetError()    { return g.error; }


ST::string CoopNetStatusText()
{
	switch (g.state)
	{
		case CoopNetState::Off:            return {};
		case CoopNetState::Connecting:     return "Connecting to the relay...";
		case CoopNetState::Hosting:        return "Hosting. Join code: " + g.code;
		case CoopNetState::WaitingForSave:
			return g.save_size ?
				ST::format("Receiving the game... {}%", g.save.size() * 100 / g.save_size) :
				ST::string("Joined. Waiting for the host to send the game...");
		case CoopNetState::SaveReady:      return "The game has arrived.";
		case CoopNetState::Playing:        return ST::format("Playing as {}.", CoopPlayerName(g.seat));
		case CoopNetState::Failed:         return "Co-op: " + g.error;
	}
	return {};
}


ST::string CoopNetTakeReceivedSave()
{
	if (g.state != CoopNetState::SaveReady) return {};
	g.state = CoopNetState::Playing;
	CoopFixLocalPlayer(g.seat);
	return JOIN_SAVE;
}


ST::string CoopNetReceivedSaveName()
{
	return JOIN_SAVE;
}


UINT8 CoopNetSeat() { return g.seat; }

bool CoopNetGuestPlaying()
{
	return !g.host && g.sock != CoopSocket::INVALID && g.state == CoopNetState::Playing;
}

bool CoopNetHasGuests()
{
	if (!CoopNetIsHost()) return false;
	for (UINT8 p = 1; p != COOP_MAX_PLAYERS; ++p) if (g.joined[p]) return true;
	return false;
}

void CoopNetSendToHost(UINT8 const kind, std::vector<UINT8> const& data)
{
	if (!CoopNetIsClient()) return;
	SendGame(0, kind, data.data(), data.size());
}

void CoopNetSendToGuests(UINT8 const kind, std::vector<UINT8> const& data)
{
	if (!CoopNetHasGuests()) return;
	SendGame(TO_EVERYONE, kind, data.data(), data.size());
}

void CoopNetQueueSnapshot()
{
	if (!CoopNetIsHost()) return;
	for (UINT8 p = 1; p != COOP_MAX_PLAYERS; ++p)
	{
		if (!g.joined[p]) continue;
		bool queued = false;
		for (UINT8 const w : g.want_save) queued = queued || w == p;
		if (!queued) g.want_save.push_back(p);
	}
}

bool CoopNetSnapshotPending()
{
	return !g.want_save.empty();
}

void CoopNetTellGuests(ST::string const& text)
{
	if (!CoopNetHasGuests()) return;
	SendGame(TO_EVERYONE, G_TEXT, reinterpret_cast<UINT8 const*>(text.c_str()), text.size());
}
