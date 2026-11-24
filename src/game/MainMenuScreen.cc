#include "Button_System.h"
#include "Cursor_Control.h"
#include "Cursors.h"
#include "Directories.h"
#include "English.h"
#include "Font.h"
#include "Font_Control.h"
#include "GameRes.h"
#include "GameSettings.h"
#include "GameLoop.h"
#include "GameVersion.h"
#include "Input.h"
#include "JA2_Splash.h"
#include "JAScreens.h"
#include "MainMenuScreen.h"
#include "MessageBoxScreen.h"
#include "Music_Control.h"
#include "ContentMusic.h"
#include "Object_Cache.h"
#include "Options_Screen.h"
#include "Render_Dirty.h"
#include "SGP.h"
#include "SaveLoadScreen.h"
#include "SysUtil.h"
#include "Text.h"
#include "Timer_Control.h"
#include "VObject.h"
#include "VSurface.h"
#include "Video.h"
#include "WordWrap.h"
#include "UILayout.h"
#include "CoopNet.h"
#include "HImage.h"
#include "Text_Input.h"

#include <string_theory/format>

#include <cstdlib>



// MENU ITEMS
enum
{
	NEW_GAME,
	LOAD_GAME,
	PREFERENCES,
	CREDITS,
	QUIT,
	NUM_MENU_ITEMS
};

#define MAINMENU_Y       277
#define MAINMENU_Y_SPACE  37


static BUTTON_PICS* iMenuImages[NUM_MENU_ITEMS];
static GUIButtonRef iMenuButtons[NUM_MENU_ITEMS];

namespace {
cache_key_t const guiMainMenuBackGroundImage{ LOADSCREENSDIR "/mainmenubackground.sti" };
cache_key_t const guiJa2LogoImage{ LOADSCREENSDIR "/ja2logo.sti" };
}

static INT8    gbHandledMainMenu = 0;
static BOOLEAN fInitialRender    = FALSE;

static BOOLEAN gfMainMenuScreenEntry = FALSE;
static BOOLEAN gfMainMenuScreenExit = FALSE;

static ScreenID guiMainMenuExitScreen = MAINMENU_SCREEN;


extern BOOLEAN gfLoadGameUponEntry;
extern ST::string gzLoadGameUponEntryName;
extern bool gfCoopHostAfterLoad;


// Co-op: Host Game and Join Game, beside the regular menu
#define COOP_BTN_X  (STD_SCREEN_X + 25) // ends left of "Continue Saved Game"
#define COOP_BTN_Y  (STD_SCREEN_Y + MAINMENU_Y)
#define COOP_BTN_W  130
#define COOP_BTN_H  26
#define JOIN_BOX_X  (STD_SCREEN_X + 170)
#define JOIN_BOX_Y  (STD_SCREEN_Y + 285)
#define JOIN_BOX_W  300
#define JOIN_BOX_H  120

static GUIButtonRef gCoopHostButton;
static GUIButtonRef gCoopJoinButton;
static GUIButtonRef gJoinOkButton;
static GUIButtonRef gJoinCancelButton;
static bool         gfJoinBox = false;
static ST::string   gzJoinBoxStatus;

static void ShowJoinBox(bool show);
static void HandleJoinBox();
static void RenderJoinBox();


static void ExitMainMenu(void);
static void HandleMainMenuInput(void);
static void HandleMainMenuScreen(void);
static void RenderMainMenu(void);
static void RenderGameVersion(void);
static void RenderCopyright(void);


static void CaptureButtonAreaBackground() {
	SGPBox
		buttonArea = {
			std::numeric_limits<UINT16>().max(), std::numeric_limits<UINT16>().max(),
			0, 0
		};

	for( const auto &curButton : iMenuButtons ) {
		buttonArea.x = std::min( buttonArea.x, static_cast<UINT16>( curButton->Area.RegionTopLeftX ));
		buttonArea.y = std::min( buttonArea.y, static_cast<UINT16>( curButton->Area.RegionTopLeftY ));
		buttonArea.w = std::max( buttonArea.w, static_cast<UINT16>( curButton->Area.RegionBottomRightX ));
		buttonArea.h = std::max( buttonArea.h, static_cast<UINT16>( curButton->Area.RegionBottomRightY ));
	}

	buttonArea.w -= buttonArea.x;
	buttonArea.h -= buttonArea.y;

	BltVideoSurface( guiEXTRABUFFER, FRAME_BUFFER, buttonArea.x, buttonArea.y, &buttonArea );
}


static void RedrawActiveButtonBackgrounds() {
	for( const auto &curButton : iMenuButtons ) {
		if(( curButton->uiFlags ^ curButton->uiOldFlags ) & ( BUTTON_CLICKED_ON | BUTTON_ENABLED )) {
			const SGPBox
				buttonRect = {
					static_cast<UINT16>( curButton->X()), static_cast<UINT16>( curButton->Y()),
					static_cast<UINT16>( curButton->W()), static_cast<UINT16>( curButton->H())
				};

			BltVideoSurface( FRAME_BUFFER, guiEXTRABUFFER, buttonRect.x, buttonRect.y, &buttonRect );
		}
	}
}


ScreenID MainMenuScreenHandle(void)
{
	if (guiSplashStartTime + INTRO_SPLASH_DURATION > GetJA2Clock())
	{
		SetCurrentCursorFromDatabase(VIDEO_NO_CURSOR);
		SetMusicMode(MUSIC_NONE);
		return MAINMENU_SCREEN; // The splash screen hasn't been up long enough yet.
	}
	if (guiSplashFrameFade)
	{
		// Fade the splash screen.
		if (guiSplashFrameFade > 2)
		{
			FRAME_BUFFER->ShadowRectUsingLowPercentTable(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
		}
		else if (guiSplashFrameFade > 1)
		{
			FRAME_BUFFER->Fill(0);
		}
		else
		{
			SetMusicMode(MUSIC_MAIN_MENU);
			SetCurrentCursorFromDatabase(CURSOR_NORMAL);
		}

		guiSplashFrameFade--;

		InvalidateScreen();
		return MAINMENU_SCREEN;
	}

	if (gfMainMenuScreenEntry)
	{
		InitMainMenu();
		gfMainMenuScreenEntry = FALSE;
		gfMainMenuScreenExit  = FALSE;
		guiMainMenuExitScreen = MAINMENU_SCREEN;
		SetMusicMode(MUSIC_MAIN_MENU);
	}

	// Testing aid: JA2_COOP_AUTOHOST=<save name> loads that save and hosts it,
	// JA2_COOP_AUTOJOIN=<code> joins, without clicking through the menu.
	static bool coop_auto_done = false;
	if (!coop_auto_done)
	{
		coop_auto_done = true;
		if (char const* const host = getenv("JA2_COOP_AUTOHOST"))
		{
			gzLoadGameUponEntryName = host;
			gfLoadGameUponEntry     = TRUE;
			gfCoopHostAfterLoad     = true;
			gbHandledMainMenu       = LOAD_GAME;
		}
		else if (char const* const code = getenv("JA2_COOP_AUTOJOIN"))
		{
			ShowJoinBox(true);
			CoopNetJoin(code);
		}
	}


	if (fInitialRender)
	{
		ClearMainMenu();
		RenderMainMenu();
		RenderGameVersion();
		RenderCopyright();
		CaptureButtonAreaBackground();

		fInitialRender = FALSE;
	}

	RedrawActiveButtonBackgrounds();

	if (gfJoinBox) HandleJoinBox();

	RenderButtons();
	if (gfJoinBox) RenderAllTextFields();

	HandleMainMenuInput();
	HandleMainMenuScreen();

	if (gfMainMenuScreenExit)
	{
		ExitMainMenu();
		gfMainMenuScreenExit  = FALSE;
		gfMainMenuScreenEntry = TRUE;
	}

	if (guiMainMenuExitScreen != MAINMENU_SCREEN) gfMainMenuScreenEntry = TRUE;

	return guiMainMenuExitScreen;
}


static void SetMainMenuExitScreen(ScreenID uiNewScreen);


static void HandleMainMenuScreen(void)
{
	if (gbHandledMainMenu == 0) return;

	// Exit according to handled value!
	switch (gbHandledMainMenu)
	{
		case QUIT:
			gfMainMenuScreenExit = TRUE;
			requestGameExit();
			break;

		case LOAD_GAME:
			// Select the game which is to be restored
			guiPreviousOptionScreen = guiCurrentScreen;
			gbHandledMainMenu       = 0;
			gfSaveGame              = FALSE;
			SetMainMenuExitScreen(SAVE_LOAD_SCREEN);
			break;

		case PREFERENCES:
			guiPreviousOptionScreen = guiCurrentScreen;
			gbHandledMainMenu       = 0;
			SetMainMenuExitScreen(OPTIONS_SCREEN);
			break;

		case CREDITS:
			gbHandledMainMenu = 0;
			SetMainMenuExitScreen(CREDIT_SCREEN);
			break;
	}
}


static void CreateDestroyMainMenuButtons(BOOLEAN fCreate);


void InitMainMenu(void)
{
	CreateDestroyMainMenuButtons(TRUE);

	// If there are no saved games, disable the button
	if (!AreThereAnySavedGameFiles())
	{
		DisableButton(iMenuButtons[LOAD_GAME]);
		DisableButton(gCoopHostButton);
	}

	gbHandledMainMenu = 0;
	fInitialRender    = TRUE;

	SetPendingNewScreen(MAINMENU_SCREEN);
	guiMainMenuExitScreen = MAINMENU_SCREEN;

	InitGameOptions();

	DequeueAllInputEvents();
}


static void ExitMainMenu(void)
{
	if (gfJoinBox) ShowJoinBox(false);
	CreateDestroyMainMenuButtons(FALSE);
	RemoveVObject(guiMainMenuBackGroundImage);
	RemoveVObject(guiJa2LogoImage);
	gMsgBox.uiExitScreen = MAINMENU_SCREEN;
}


static void MenuButtonCallback(GUI_BUTTON *btn, UINT32 reason)
{
	if (reason & MSYS_CALLBACK_REASON_POINTER_UP)
	{
		INT8 const bID = btn->GetUserData();

		gbHandledMainMenu = bID;
		RenderMainMenu();

		switch (gbHandledMainMenu)
		{
			case NEW_GAME:  SetMainMenuExitScreen(GAME_INIT_OPTIONS_SCREEN); break;
			case LOAD_GAME: if (_KeyDown(ALT)) gfLoadGameUponEntry = TRUE;   break;
		}
	}
}


static void HandleMainMenuInput(void)
{
	InputAtom InputEvent;
	while (DequeueSpecificEvent(&InputEvent, KEYBOARD_EVENTS))
	{
		if (gfJoinBox)
		{
			if (HandleTextInput(&InputEvent)) continue;
			if (InputEvent.usEvent == KEY_UP)
			{
				if (InputEvent.usParam == SDLK_RETURN) CoopNetJoin(GetStringFromField(0));
				if (InputEvent.usParam == SDLK_ESCAPE) ShowJoinBox(false);
			}
			continue;
		}
		if (InputEvent.usEvent == KEY_UP)
		{
			switch (InputEvent.usParam)
			{
				case 'q': if (_KeyDown(CTRL)) gbHandledMainMenu = QUIT; break;					
				case SDLK_ESCAPE: gbHandledMainMenu = QUIT; break;

				case 'c':
					if (_KeyDown(ALT)) gfLoadGameUponEntry = TRUE;
					gbHandledMainMenu = LOAD_GAME;
					break;

				case 'o':
					gbHandledMainMenu = PREFERENCES;
					break;

				case 's':
					gbHandledMainMenu = CREDITS;
					break;
			}
		}
	}
}


void ClearMainMenu(void)
{
	FRAME_BUFFER->Fill(0);
	InvalidateScreen();
}


static void SetMainMenuExitScreen(ScreenID const uiNewScreen)
{
	guiMainMenuExitScreen = uiNewScreen;
	gfMainMenuScreenExit  = TRUE;
}


static void CoopHostCallback(GUI_BUTTON*, UINT32 reason);
static void CoopJoinCallback(GUI_BUTTON*, UINT32 reason);

static void CreateDestroyMainMenuButtons(BOOLEAN fCreate)
{
	static BOOLEAN fButtonsCreated = FALSE;

	if (fCreate)
	{
		if (fButtonsCreated) return;

		// Reset the variable that allows the user to ALT click on the continue save btn to load the save instantly
		gfLoadGameUponEntry = FALSE;

		// Load button images
		const char* const filename = GetMLGFilename(MLG_TITLETEXT);

		INT32 Slot;
		Slot = 0;
		iMenuImages[NEW_GAME]    = LoadButtonImage(filename, Slot, Slot, Slot + 1, Slot + 2, -1);
		iMenuImages[LOAD_GAME]   = UseLoadedButtonImage(iMenuImages[NEW_GAME],  6,  3,  4,  5, -1);
		iMenuImages[PREFERENCES] = UseLoadedButtonImage(iMenuImages[NEW_GAME],  7,  7,  8,  9, -1);
		iMenuImages[CREDITS]     = UseLoadedButtonImage(iMenuImages[NEW_GAME], 13, 10, 11, 12, -1);
		iMenuImages[QUIT]        = UseLoadedButtonImage(iMenuImages[NEW_GAME], 14, 14, 15, 16, -1);

		for (UINT32 cnt = 0; cnt < NUM_MENU_ITEMS; ++cnt)
		{
			BUTTON_PICS* const img = iMenuImages[cnt];
			const UINT16       w   = GetDimensionsOfButtonPic(img)->w;
			const INT16        x   = (SCREEN_WIDTH - w) / 2;
			const INT16        y   = STD_SCREEN_Y + MAINMENU_Y + cnt * MAINMENU_Y_SPACE;
			GUIButtonRef const b = QuickCreateButton(img, x, y, MSYS_PRIORITY_HIGHEST, MenuButtonCallback);
			iMenuButtons[cnt] = b;
			b->SetUserData(cnt);
		}

		gCoopHostButton = CreateTextButton("Host Game", FONT14ARIAL, FONT_MCOLOR_WHITE, FONT_MCOLOR_BLACK, COOP_BTN_X, COOP_BTN_Y, COOP_BTN_W, COOP_BTN_H, MSYS_PRIORITY_HIGHEST, CoopHostCallback);
		gCoopHostButton->SetFastHelpText("Load one of your games and let friends join it over the internet.");
		gCoopJoinButton = CreateTextButton("Join Game", FONT14ARIAL, FONT_MCOLOR_WHITE, FONT_MCOLOR_BLACK, COOP_BTN_X, COOP_BTN_Y + MAINMENU_Y_SPACE, COOP_BTN_W, COOP_BTN_H, MSYS_PRIORITY_HIGHEST, CoopJoinCallback);
		gCoopJoinButton->SetFastHelpText("Join a friend's game with the code their game shows.");

		fButtonsCreated = TRUE;
	}
	else
	{
		if (!fButtonsCreated) return;

		// Delete images/buttons
		for (UINT32 cnt = 0; cnt < NUM_MENU_ITEMS; ++cnt)
		{
			RemoveButton(iMenuButtons[cnt]);
			UnloadButtonImage(iMenuImages[cnt]);
		}
		RemoveButton(gCoopHostButton);
		RemoveButton(gCoopJoinButton);
		fButtonsCreated = FALSE;
	}
}


static void RenderMainMenu(void)
{
	BltVideoObject(FRAME_BUFFER, guiMainMenuBackGroundImage, 0, STD_SCREEN_X,       STD_SCREEN_Y     );
	BltVideoObject(FRAME_BUFFER, guiJa2LogoImage,            0, STD_SCREEN_X + 188, STD_SCREEN_Y + 15);
}

void RenderGameVersion() {
	SetFontAttributes(FONT10ARIAL, FONT_MCOLOR_WHITE);
	MPrint(g_ui.m_versionPosition.iX, g_ui.m_versionPosition.iY, ST::format("{}", g_version_label));
}

void RenderCopyright() {
	DrawTextToScreen(gzCopyrightText, 0, SCREEN_HEIGHT - 15, SCREEN_WIDTH, FONT10ARIAL, FONT_MCOLOR_WHITE, FONT_MCOLOR_BLACK, CENTER_JUSTIFIED);
}


static bool CheckRelaySetUp()
{
	if (!CoopNetRelayAddress().empty()) return true;
	DoMessageBox(MSG_BOX_BASIC_STYLE,
		"Co-op needs a relay. Put its address in coop.json in the game's home folder, next to ja2.json, like this: { \"relay\": \"example.org:27960\", \"name\": \"Your name\" }",
		MAINMENU_SCREEN, MSG_BOX_FLAG_OK, nullptr, nullptr);
	return false;
}


static void CoopHostCallback(GUI_BUTTON*, UINT32 const reason)
{
	if (!(reason & MSYS_CALLBACK_REASON_POINTER_UP)) return;
	if (!CheckRelaySetUp()) return;

	// pick the game on the load screen; it opens for others once loaded
	gfCoopHostAfterLoad = true;
	gbHandledMainMenu   = LOAD_GAME;
}


static void CoopJoinCallback(GUI_BUTTON*, UINT32 const reason)
{
	if (!(reason & MSYS_CALLBACK_REASON_POINTER_UP)) return;
	if (!CheckRelaySetUp()) return;
	ShowJoinBox(true);
}


static void JoinOkCallback(GUI_BUTTON*, UINT32 const reason)
{
	if (reason & MSYS_CALLBACK_REASON_POINTER_UP) CoopNetJoin(GetStringFromField(0));
}


static void JoinCancelCallback(GUI_BUTTON*, UINT32 const reason)
{
	if (reason & MSYS_CALLBACK_REASON_POINTER_UP) ShowJoinBox(false);
}


static void ShowMenuButtons(bool const show)
{
	for (GUIButtonRef const& b : iMenuButtons) { if (show) b->Show(); else b->Hide(); }
	if (show) { gCoopHostButton->Show(); gCoopJoinButton->Show(); }
	else      { gCoopHostButton->Hide(); gCoopJoinButton->Hide(); }
}


static void CloseJoinBoxWidgets()
{
	KillTextInputMode();
	RemoveButton(gJoinOkButton);
	RemoveButton(gJoinCancelButton);
	ShowMenuButtons(true);
	gfJoinBox = false;
	gzJoinBoxStatus.clear();
}


static void ShowJoinBox(bool const show)
{
	if (show == gfJoinBox) return;

	if (show)
	{
		gfJoinBox = true;
		ShowMenuButtons(false);

		InitTextInputMode();
		SetTextInputFont(FONT14ARIAL);
		Set16BPPTextFieldColor(Get16BPPColor(FROMRGB(0, 0, 0)));
		SetTextInputRegularColors(FONT_MCOLOR_WHITE, FONT_MCOLOR_BLACK);
		SetTextInputHilitedColors(FONT_MCOLOR_BLACK, FONT_MCOLOR_WHITE, FONT_MCOLOR_WHITE);
		SetCursorColor(Get16BPPColor(FROMRGB(255, 255, 255)));
		AddTextInputField(JOIN_BOX_X + 100, JOIN_BOX_Y + 40, 100, 20, MSYS_PRIORITY_HIGHEST, {}, 6, INPUTTYPE_FULL_TEXT);

		gJoinOkButton     = CreateTextButton("Join",   FONT14ARIAL, FONT_MCOLOR_WHITE, FONT_MCOLOR_BLACK, JOIN_BOX_X + 40,  JOIN_BOX_Y + 85, 100, 24, MSYS_PRIORITY_HIGHEST, JoinOkCallback);
		gJoinCancelButton = CreateTextButton("Cancel", FONT14ARIAL, FONT_MCOLOR_WHITE, FONT_MCOLOR_BLACK, JOIN_BOX_X + 160, JOIN_BOX_Y + 85, 100, 24, MSYS_PRIORITY_HIGHEST, JoinCancelCallback);
		gzJoinBoxStatus = "Type the code the host's game shows.";
		RenderJoinBox();
	}
	else
	{
		CloseJoinBoxWidgets();
		// a join under way is called off with the box
		if (CoopNetIsClient()) CoopNetDisconnect();
		fInitialRender = TRUE; // redraw the menu without the box
	}
}


static void HandleJoinBox()
{
	// the host's game has arrived: load it through the load screen
	ST::string const save = CoopNetTakeReceivedSave();
	if (!save.empty())
	{
		CloseJoinBoxWidgets(); // the connection stays: we are in the game now
		gzLoadGameUponEntryName = save;
		gfLoadGameUponEntry     = TRUE;
		gbHandledMainMenu       = LOAD_GAME;
		return;
	}

	ST::string status = CoopNetStatusText();
	if (status.empty()) status = "Type the code the host's game shows.";
	if (status != gzJoinBoxStatus)
	{
		gzJoinBoxStatus = status;
		RenderJoinBox();
	}
}


static void RenderJoinBox()
{
	RenderMainMenu();
	RenderGameVersion();
	RenderCopyright();
	FRAME_BUFFER->ShadowRect(JOIN_BOX_X, JOIN_BOX_Y, JOIN_BOX_X + JOIN_BOX_W, JOIN_BOX_Y + JOIN_BOX_H);
	FRAME_BUFFER->ShadowRect(JOIN_BOX_X, JOIN_BOX_Y, JOIN_BOX_X + JOIN_BOX_W, JOIN_BOX_Y + JOIN_BOX_H);
	DrawTextToScreen("Join a co-op game", JOIN_BOX_X, JOIN_BOX_Y + 10, JOIN_BOX_W, FONT14ARIAL, FONT_MCOLOR_WHITE, FONT_MCOLOR_BLACK, CENTER_JUSTIFIED);
	DrawTextToScreen("Code:", JOIN_BOX_X + 40, JOIN_BOX_Y + 43, 55, FONT12ARIAL, FONT_MCOLOR_WHITE, FONT_MCOLOR_BLACK, LEFT_JUSTIFIED);
	DrawTextToScreen(gzJoinBoxStatus, JOIN_BOX_X + 5, JOIN_BOX_Y + 66, JOIN_BOX_W - 10, FONT10ARIAL, FONT_MCOLOR_LTYELLOW, FONT_MCOLOR_BLACK, CENTER_JUSTIFIED);
	MarkButtonsDirty();
	InvalidateScreen();
}
