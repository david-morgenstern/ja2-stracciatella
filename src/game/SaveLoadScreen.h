#ifndef _SAVE_LOAD_SCREEN__H_
#define _SAVE_LOAD_SCREEN__H_

#include "JA2Types.h"
#include "MessageBoxScreen.h"
#include "ScreenIDs.h"

#include <string_theory/string>


//This flag is used to diferentiate between loading a game and saveing a game.
// gfSaveGame=TRUE	For saving a game
// gfSaveGame=FALSE	For loading a game
extern BOOLEAN gfSaveGame;

extern	BOOLEAN gfCameDirectlyFromGame;

ScreenID SaveLoadScreenHandle(void);

void DoSaveLoadMessageBox(const ST::string& str, ScreenID uiExitScreen, MessageBoxFlags, MSGBOX_CALLBACK ReturnCallback);

void DoQuickSave(void);
void DoAutoSave(void);
void DoDeadIsDeadSave(void);
void DoQuickLoad(void);
// Load the named save from wherever the player is, with a quick fade.
void LoadSaveInPlace(ST::string const& name);

bool AreThereAnySavedGameFiles();

void DeleteSaveGameNumber(UINT8 save_slot_id);

#endif
