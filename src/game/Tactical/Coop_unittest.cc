// -*-coding: utf-8-unix;-*-

#include "gtest/gtest.h"

#include "Coop.h"
#include "GameSettings.h"
#include "Overhead_Types.h"
#include "Soldier_Control.h"


TEST(CoopTest, OwnerPacking)
{
	for (UINT8 owner = 0; owner != COOP_MAX_PLAYERS; ++owner)
	{
		UINT8 bytes[2];
		CoopPackOwner(owner, bytes);
		EXPECT_EQ(CoopUnpackOwner(bytes), owner);
	}

	// player 0 keeps the zeroes old saves had there
	UINT8 zero[2];
	CoopPackOwner(0, zero);
	EXPECT_EQ(zero[0], 0);
	EXPECT_EQ(zero[1], 0);

	// whatever else old saves hold there means player 0
	UINT8 const junk[][2] = { { 0, 1 }, { 0xFF, 0xFF }, { 0xC0, 9 }, { 1, 0xC0 } };
	for (auto const& j : junk) EXPECT_EQ(CoopUnpackOwner(j), 0);
}


TEST(CoopTest, LocalMercOnlyInCoop)
{
	UINT8 const old_players = gGameOptions.ubCoopPlayers;

	SOLDIERTYPE s{};
	s.bTeam = OUR_TEAM;
	CoopSetOwner(s, 1);

	gGameOptions.ubCoopPlayers = 1;
	EXPECT_FALSE(CoopActive());
	EXPECT_TRUE(CoopIsLocalMerc(s)); // without co-op everyone is yours
	EXPECT_EQ(CoopOwner(s), 0);

	gGameOptions.ubCoopPlayers = 2;
	CoopSetLocalPlayer(0);
	EXPECT_TRUE(CoopActive());
	EXPECT_FALSE(CoopIsLocalMerc(s));
	CoopCycleLocalPlayer();
	EXPECT_EQ(CoopLocalPlayer(), 1);
	EXPECT_TRUE(CoopIsLocalMerc(s));

	CoopCycleOwner(s);
	EXPECT_EQ(CoopOwner(s), 0);

	// enemies and civilians are nobody's
	SOLDIERTYPE enemy{};
	enemy.bTeam = ENEMY_TEAM;
	CoopSetOwner(enemy, 0);
	EXPECT_TRUE(CoopIsLocalMerc(enemy));

	CoopSetLocalPlayer(0);
	gGameOptions.ubCoopPlayers = old_players;
}
