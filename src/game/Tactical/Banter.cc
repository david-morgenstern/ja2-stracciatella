#include "Banter.h"

#include "Assignments.h"
#include "ContentManager.h"
#include "Dialogue_Control.h"
#include "FileMan.h"
#include "Font_Control.h"
#include "GameInstance.h"
#include "Json.h"
#include "Logger.h"
#include "Message.h"
#include "MercProfileInfo.h"
#include "Morale.h"
#include "Overhead.h"
#include "Random.h"
#include "Soldier_Control.h"
#include "Soldier_Macros.h"
#include "Soldier_Profile.h"
#include "Sound_Control.h"
#include "SoundMan.h"
#include "StrategicMap.h"
#include "Timer_Control.h"

#include <algorithm>
#include <functional>
#include <set>
#include <vector>


namespace
{

struct BanterLine
{
	ST::string id;
	ST::string trigger;
	UINT8      speaker;
	UINT8      subject;   // or NO_PROFILE
	UINT8      with;      // or NO_PROFILE
	UINT16     quote;     // for alt: the quote it is another take on
	ST::string text;
	ST::string sound;
};

using Present = std::function<bool(UINT8)>;

std::vector<BanterLine> g_lines;
std::set<size_t>        g_said;           // lines said this session
std::set<UINT8>         g_grieved;        // dead subjects already grieved for
bool                    g_loaded = false;
UINT32                  g_last_kill_banter = 0;


UINT8 ProfileByName(ST::string const& name)
{
	MercProfileInfo const* const info = GCM->getMercProfileInfoByName(name);
	return info ? info->profileID : NO_PROFILE;
}


void Load()
{
	if (g_loaded) return;
	g_loaded = true;
	if (!GCM->doesGameResExists("banter.json")) return;
	try
	{
		AutoSGPFile f(GCM->openGameResForReading("banter.json"));
		JsonValue const json = JsonValue::deserialize(f->readStringToEnd());
		for (auto const& element : json.toVec())
		{
			JsonObject const o = element.toObject();
			BanterLine l;
			l.id      = o.getOptionalString("id");
			l.trigger = o.GetString("trigger");
			l.speaker = ProfileByName(o.GetString("speaker"));
			l.subject = o.has("subject") ? ProfileByName(o.GetString("subject")) : NO_PROFILE;
			l.with    = o.has("with")    ? ProfileByName(o.GetString("with"))    : NO_PROFILE;
			l.quote   = static_cast<UINT16>(o.getOptionalUInt("quote", 0xFFFF));
			l.text    = o.GetString("text");
			l.sound   = o.getOptionalString("sound");
			if (l.speaker == NO_PROFILE)
			{
				SLOGW("banter.json: {} has an unknown speaker", l.id);
				continue;
			}
			g_lines.push_back(std::move(l));
		}
		SLOGI("Loaded {} banter lines", g_lines.size());
	}
	catch (std::exception const& e)
	{
		SLOGE("banter.json could not be read: {}", e.what());
	}
}


bool IsRepeatable(ST::string const& trigger)
{
	return trigger == "kill" || trigger == "battle_won";
}


/* The lines for a trigger that match, the ones for company that is present
 * first, and among those the ones not said yet. */
std::vector<size_t> Find(char const* const trigger, UINT8 const speaker, UINT8 const subject, Present const& present)
{
	Load();
	std::vector<size_t> tiers[4]; // with+fresh, with+said, alone+fresh, alone+said
	for (size_t i = 0; i != g_lines.size(); ++i)
	{
		BanterLine const& l = g_lines[i];
		if (l.trigger != trigger) continue;
		if (speaker != NO_PROFILE && l.speaker != speaker) continue;
		if (subject != NO_PROFILE && l.subject != subject) continue;
		if (l.with != NO_PROFILE && !present(l.with)) continue;
		bool const said = g_said.count(i) != 0;
		if (said && !IsRepeatable(l.trigger)) continue;
		tiers[(l.with != NO_PROFILE ? 0 : 2) + (said ? 1 : 0)].push_back(i);
	}
	for (auto const& t : tiers) if (!t.empty()) return t;
	return {};
}


size_t PickOne(std::vector<size_t> const& from)
{
	size_t const i = from[Random(static_cast<UINT32>(from.size()))];
	g_said.insert(i);
	return i;
}


UINT16 QuoteOf(size_t const i)
{
	return static_cast<UINT16>(BANTER_QUOTE_BASE + i);
}


// alive, awake and able to talk
bool CanTalk(SOLDIERTYPE const& s)
{
	return s.bActive && s.ubProfile != NO_PROFILE && s.bLife >= OKLIFE && !s.fMercAsleep &&
		s.bAssignment != ASSIGNMENT_POW && !AM_A_ROBOT(&s);
}


std::vector<SOLDIERTYPE*> TeamIn(SGPSector const& sector, SOLDIERTYPE const* const except)
{
	std::vector<SOLDIERTYPE*> out;
	FOR_EACH_IN_TEAM(s, OUR_TEAM)
	{
		if (s != except && CanTalk(*s) && s->sSector == sector) out.push_back(s);
	}
	return out;
}


Present InSector(SGPSector const& sector)
{
	return [sector](UINT8 const profile)
	{
		SOLDIERTYPE const* const s = FindSoldierByProfileIDOnPlayerTeam(profile);
		return s && CanTalk(*s) && s->sSector == sector;
	};
}


bool OnTeam(UINT8 const profile)
{
	SOLDIERTYPE const* const s = FindSoldierByProfileIDOnPlayerTeam(profile);
	return s && s->bLife > 0;
}


bool IsDead(UINT8 const profile)
{
	return GetProfile(profile).bMercStatus == MERC_IS_DEAD;
}

} // namespace


void LoadBanter()
{
	Load();
}


bool GetBanterLine(UINT16 const quote, ST::string& text, ST::string& sound)
{
	if (quote < BANTER_QUOTE_BASE) return false;
	Load();
	size_t const i = quote - BANTER_QUOTE_BASE;
	if (i >= g_lines.size()) return false;
	text  = g_lines[i].text;
	sound = g_lines[i].sound;
	return true;
}


bool GetBanterAlternate(UINT8 const profile, UINT16 const quote, ST::string& text, ST::string& sound)
{
	Load();
	std::vector<size_t> takes;
	for (size_t i = 0; i != g_lines.size(); ++i)
	{
		BanterLine const& l = g_lines[i];
		if (l.trigger == "alt" && l.speaker == profile && l.quote == quote) takes.push_back(i);
	}
	// the original counts as one of the takes
	UINT32 const pick = Random(static_cast<UINT32>(takes.size() + 1));
	if (pick == takes.size()) return false;
	text  = g_lines[takes[pick]].text;
	sound = g_lines[takes[pick]].sound;
	return true;
}


void BanterOnArrival(SOLDIERTYPE& arrived)
{
	if (arrived.ubProfile == NO_PROFILE) return;
	std::vector<SOLDIERTYPE*> const team = TeamIn(arrived.sSector, &arrived);
	Present const here = InSector(arrived.sSector);

	// someone already here greets the new arrival...
	SOLDIERTYPE* greeter = nullptr;
	for (SOLDIERTYPE* const s : team)
	{
		if (Find("arrival", s->ubProfile, arrived.ubProfile, here).empty()) continue;
		if (greeter && Random(2)) continue;
		greeter = s;
	}
	if (greeter)
	{
		TacticalCharacterDialogue(greeter, QuoteOf(PickOne(Find("arrival", greeter->ubProfile, arrived.ubProfile, here))));
	}

	// ...and the new arrival has a word for him, or for someone else here
	std::vector<size_t> reply;
	if (greeter) reply = Find("meets", arrived.ubProfile, greeter->ubProfile, here);
	if (reply.empty())
	{
		for (SOLDIERTYPE* const s : team)
		{
			std::vector<size_t> const lines = Find("meets", arrived.ubProfile, s->ubProfile, here);
			reply.insert(reply.end(), lines.begin(), lines.end());
		}
	}
	if (!reply.empty()) TacticalCharacterDialogue(&arrived, QuoteOf(PickOne(reply)));
}


void BanterOnKill(SOLDIERTYPE& killer)
{
	if (killer.ubProfile == NO_PROFILE) return;
	UINT32 const now = GetJA2Clock();
	if (now - g_last_kill_banter < 60000) return;
	if (Random(100) >= 35) return;

	Present const here = InSector(killer.sSector);
	std::vector<SOLDIERTYPE*> speakers;
	for (SOLDIERTYPE* const s : TeamIn(killer.sSector, &killer))
	{
		if (!Find("kill", s->ubProfile, killer.ubProfile, here).empty()) speakers.push_back(s);
	}
	if (speakers.empty()) return;
	SOLDIERTYPE* const s = speakers[Random(static_cast<UINT32>(speakers.size()))];
	g_last_kill_banter = now;
	TacticalCharacterDialogue(s, QuoteOf(PickOne(Find("kill", s->ubProfile, killer.ubProfile, here))));
}


void BanterOnBattleWon()
{
	Present const here = InSector(gWorldSector);
	std::vector<SOLDIERTYPE*> const team = TeamIn(gWorldSector, nullptr);

	// someone who lost his brother since the last fight
	for (SOLDIERTYPE* const s : team)
	{
		if (!s->bInSector) continue;
		for (size_t i = 0; i != g_lines.size(); ++i)
		{
			BanterLine const& l = g_lines[i];
			if (l.trigger != "grief" || l.speaker != s->ubProfile) continue;
			if (!IsDead(l.subject) || g_grieved.count(l.subject)) continue;
			g_grieved.insert(l.subject);
			g_said.insert(i);
			TacticalCharacterDialogue(s, QuoteOf(i));
			return; // nobody jokes after that
		}
	}

	if (Random(100) >= 60) return;
	for (SOLDIERTYPE* const subject : team)
	{
		if (!subject->bInSector) continue;
		std::vector<SOLDIERTYPE*> speakers;
		for (SOLDIERTYPE* const s : team)
		{
			if (s != subject && s->bInSector && !Find("battle_won", s->ubProfile, subject->ubProfile, here).empty()) speakers.push_back(s);
		}
		if (speakers.empty()) continue;
		SOLDIERTYPE* const s = speakers[Random(static_cast<UINT32>(speakers.size()))];
		TacticalCharacterDialogue(s, QuoteOf(PickOne(Find("battle_won", s->ubProfile, subject->ubProfile, here))));
		return; // one is plenty
	}
}


void BanterOnDeath(SOLDIERTYPE const& dead)
{
	if (dead.ubProfile == NO_PROFILE) return;
	FOR_EACH_IN_TEAM(s, OUR_TEAM)
	{
		if (s == &dead || !CanTalk(*s)) continue;
		std::vector<size_t> const lines = Find("dies", s->ubProfile, dead.ubProfile, OnTeam);
		if (lines.empty()) continue;
		TacticalCharacterDialogue(s, QuoteOf(PickOne(lines)));
		HandleBuddyDiedMorale(*s);
	}
}


void BanterOnConversation(UINT8 const npc_profile, FACETYPE* const face)
{
	Present const here = InSector(gWorldSector);
	FOR_EACH_IN_TEAM(subject, OUR_TEAM)
	{
		if (!CanTalk(*subject) || !subject->bInSector || subject->sSector != gWorldSector) continue;
		std::vector<size_t> const lines = Find("npc_talk", npc_profile, subject->ubProfile, here);
		if (lines.empty()) continue;
		size_t const said = PickOne(lines);
		CharacterDialogue(npc_profile, QuoteOf(said), face, DIALOGUE_NPC_UI, FALSE);

		// the answer comes from whoever the line was aimed at; a line for two of
		// them gets an answer written for two, from either one
		UINT8 const company = g_lines[said].with;
		std::vector<SOLDIERTYPE*> answerers{ subject };
		if (company != NO_PROFILE) answerers.push_back(FindSoldierByProfileIDOnPlayerTeam(company));
		for (bool const for_two : { company != NO_PROFILE, false })
		{
			for (SOLDIERTYPE* const a : answerers)
			{
				if (!a) continue;
				std::vector<size_t> reply = Find("npc_reply", a->ubProfile, npc_profile, here);
				if (for_two) reply.erase(std::remove_if(reply.begin(), reply.end(), [](size_t const i) { return g_lines[i].with == NO_PROFILE; }), reply.end());
				if (reply.empty()) continue;
				TacticalCharacterDialogue(a, QuoteOf(PickOne(reply)));
				return;
			}
		}
		return;
	}
}


void BanterOnEnemySighting(SOLDIERTYPE& enemy, SOLDIERTYPE& merc)
{
	if (enemy.ubProfile == NO_PROFILE || merc.ubProfile == NO_PROFILE) return;
	std::vector<size_t> const lines = Find("spots", enemy.ubProfile, merc.ubProfile, InSector(merc.sSector));
	if (lines.empty()) return;

	BanterLine const& l = g_lines[PickOne(lines)];
	ScreenMsg(FONT_MCOLOR_LTRED, MSG_INTERFACE, ST::format("{}: \"{}\"", GetProfile(enemy.ubProfile).zNickname, l.text));
	if (!l.sound.empty() && GCM->doesGameResExists(l.sound))
	{
		SoundPlay(l.sound.c_str(), CalculateSpeechVolume(HIGHVOLUME), SoundDir(enemy.sGridNo), 1, NULL, NULL);
	}
}


bool BanterOnAimCall(UINT8 const merc_profile, FACETYPE* const face)
{
	FOR_EACH_IN_TEAM(subject, OUR_TEAM)
	{
		if (subject->ubProfile == NO_PROFILE || subject->bLife <= 0) continue;
		std::vector<size_t> const lines = Find("aim_call", merc_profile, subject->ubProfile, OnTeam);
		if (lines.empty()) continue;
		CharacterDialogue(merc_profile, QuoteOf(PickOne(lines)), face, DIALOGUE_CONTACTPAGE_UI, FALSE);
		return true;
	}
	return false;
}
