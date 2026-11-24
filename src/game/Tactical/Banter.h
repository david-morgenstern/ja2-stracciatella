#pragma once

/* Banter: extra lines characters say about each other, beyond the fixed set
 * of quotes every character has. They come from banter.json, which a mod can
 * provide, as a list of
 *
 *   { "id": "ivan_arrival_chris", "speaker": "IVAN", "subject": "CHRIS_MORNINGSTAR",
 *     "with": "DAVE_MORNINGSTAR", "trigger": "arrival",
 *     "text": "...", "sound": "speech/banter/ivan_arrival_chris.wav" }
 *
 * speaker, subject and with are profile internal names. "with" is optional: a
 * line with it is only said when that character is around too (in the sector,
 * or on the team for aim_call), and such lines are picked first.
 * The triggers:
 *   arrival      the speaker, already in the sector, as the subject arrives there
 *   meets        the speaker, just arrived, about the subject he finds there
 *   kill         the speaker, in the sector, after the subject kills someone
 *   battle_won   the speaker, after a won fight the subject was in
 *   dies         the speaker, anywhere on the team, when the subject dies; he
 *                also takes the morale blow of losing his closest friend
 *   grief        the speaker, after the first won fight since the subject died
 *   npc_talk     the speaker (an NPC), when the player starts talking to him
 *                with the subject in the sector
 *   npc_reply    the speaker, answering an npc_talk line of the subject's NPC
 *   aim_call     the speaker (an A.I.M. merc), after his greeting on a video call
 *                while the subject is on the player's team
 *   spots        the speaker, a named character fighting the player, when he
 *                first sees the subject (shown in the message log)
 *   alt          another take on one of the speaker's own quotes, given as
 *                "quote": <number>; when that quote comes up, the original and
 *                its takes are picked from at random
 *
 * Each line is said through the normal dialogue system under a quote number
 * from BANTER_QUOTE_BASE up, so it gets the speaker's face, subtitles and the
 * queue like any other line. Kill and battle_won lines can come again; the
 * others are said once per session. */

#include "Types.h"

#include <string_theory/string>

struct FACETYPE;
struct SOLDIERTYPE;

constexpr UINT16 BANTER_QUOTE_BASE = 1000;

// Reads banter.json now, so a broken file shows in the log at start-up.
void LoadBanter();

// The text and sound of a banter line by its quote number; false if it is none.
bool GetBanterLine(UINT16 quote, ST::string& text, ST::string& sound);

// Swaps in another take of the profile's own quote now and then; false keeps the original.
bool GetBanterAlternate(UINT8 profile, UINT16 quote, ST::string& text, ST::string& sound);

void BanterOnArrival(SOLDIERTYPE& arrived);
void BanterOnKill(SOLDIERTYPE& killer);
void BanterOnBattleWon();
void BanterOnDeath(SOLDIERTYPE const& dead);
void BanterOnConversation(UINT8 npc_profile, FACETYPE* face);
void BanterOnEnemySighting(SOLDIERTYPE& enemy, SOLDIERTYPE& merc);
// Queues the merc's line after his greeting; false if he has none to say.
bool BanterOnAimCall(UINT8 merc_profile, FACETYPE* face);
