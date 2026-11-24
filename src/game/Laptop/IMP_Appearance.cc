#include "IMP_Appearance.h"

#include "Button_System.h"
#include "CharProfile.h"
#include "Cursors.h"
#include "Directories.h"
#include "Font.h"
#include "Font_Control.h"
#include "HImage.h"
#include "IMP_Compile_Character.h"
#include "IMP_MainPage.h"
#include "IMP_Portraits.h"
#include "IMPPolicy.h"
#include "IMPVideoObjects.h"
#include "Laptop.h"
#include "Soldier_Control.h"
#include "VObject.h"
#include "VObject_Blitters.h"
#include "VSurface.h"
#include "Video.h"

#include <string_theory/format>
#include <string_theory/string>

#include <array>
#include <memory>
#include <vector>


IMPAppearance gIMPAppearance;

namespace
{

// The palette replacement types in ja2pal.dat, found through an ID that is
// known to be of that type so the order of the file does not matter.
enum Row { ROW_VEST, ROW_PANTS, ROW_HAIR, ROW_SKIN, ROW_BUILD, NUM_ROWS };

char const* const ROW_LABEL[NUM_ROWS] = { "Shirt", "Pants", "Hair", "Skin", "Build" };
char const* const ROW_PROBE[NUM_ROWS] = { "WHITEVEST", "BLACKPANTS", "BROWNHEAD", "TANSKIN", nullptr };

// Friendlier names for the IDs the original data uses. Anything not listed is
// shown as its ID.
struct NiceName { char const* id; char const* name; };
NiceName const NICE_NAMES[] =
{
	{ "BLONDHEAD", "Blond" }, { "BROWNHEAD", "Brown" }, { "BLACKHEAD", "Black" },
	{ "WHITEHEAD", "Grey" }, { "REDHEAD", "Red" }, { "redbrown", "Auburn" },
	{ "dirtblnde", "Dirty blond" },
	{ "GREENPANTS", "Green" }, { "JEANPANTS", "Jeans" }, { "TANPANTS", "Tan" },
	{ "BLACKPANTS", "Black" }, { "BLUEPANTS", "Navy" }, { "BEIGEPANTS", "Beige" },
	{ "PINKSKIN", "Fair" }, { "TANSKIN", "Tan" }, { "DARKSKIN", "Dark" },
	{ "BLACKSKIN", "Black" },
	{ "BROWNVEST", "Brown vest" }, { "greyVEST", "Grey vest" },
	{ "GREENVEST", "Green vest" }, { "JEANVEST", "Denim vest" },
	{ "REDVEST", "Red vest" }, { "BLUEVEST", "Blue vest" },
	{ "YELLOWVEST", "Yellow vest" }, { "WHITEVEST", "White vest" },
	{ "BLACKSHIRT", "Black shirt" }, { "GYELLOWSHIRT", "Olive shirt" },
	{ "PURPLESHIRT", "Purple shirt" },
};

INT16 const ROW_Y0     = 62;
INT16 const ROW_DY     = 40;
INT16 const LABEL_X    = 196;
INT16 const LEFT_X     = 262;
INT16 const SWATCH_X   = 290;
INT16 const NAME_X     = 312;
INT16 const RIGHT_X    = 430;
INT16 const PORTRAIT_X = 40;
INT16 const PORTRAIT_Y = 60;
INT16 const PREVIEW_X  = 36;
INT16 const PREVIEW_Y  = 200;
INT16 const PREVIEW_W  = 130;
INT16 const PREVIEW_H  = 110;

GUIButtonRef       gArrowButtons[NUM_ROWS * 2];
BUTTON_PICS*       gArrowImages[2];
GUIButtonRef       gDoneButton;
BUTTON_PICS*       gDoneImage;
bool               gRedraw = false;


ST::string NiceName(ST::string const& id)
{
	for (auto const& n : NICE_NAMES)
	{
		if (id == n.id) return n.name;
	}
	return id;
}


// all replacement IDs of the type the probe ID belongs to, in file order
std::vector<ST::string> OptionsFor(Row const row)
{
	std::vector<ST::string> out;
	if (!ROW_PROBE[row]) return out;
	auto const probe = GetPaletteRepIndexFromID(ROW_PROBE[row]);
	if (!probe) return out;
	UINT8 const type = gpPalRep[*probe].ubType;
	UINT32 start = 0;
	for (UINT8 t = 0; t < type; ++t) start += gubpNumReplacementsPerRange[t];
	for (UINT32 i = 0; i < gubpNumReplacementsPerRange[type]; ++i)
	{
		out.push_back(gpPalRep[start + i].ID);
	}
	return out;
}


ST::string& ValueFor(Row const row)
{
	switch (row)
	{
		case ROW_VEST:  return gIMPAppearance.vest;
		case ROW_PANTS: return gIMPAppearance.pants;
		case ROW_HAIR:  return gIMPAppearance.hair;
		default:        return gIMPAppearance.skin;
	}
}


bool StrongEnoughForBigBuild()
{
	return fCharacterIsMale && iStrength >= IMP_BIG_BODY_MIN_STRENGTH;
}


void Cycle(Row const row, int const delta)
{
	if (row == ROW_BUILD)
	{
		if (StrongEnoughForBigBuild()) gIMPAppearance.bigBuild = !gIMPAppearance.bigBuild;
		return;
	}
	std::vector<ST::string> const options = OptionsFor(row);
	if (options.empty()) return;
	ST::string& value = ValueFor(row);
	int idx = 0;
	for (size_t i = 0; i != options.size(); ++i)
	{
		if (options[i].compare_i(value) == 0) idx = static_cast<int>(i);
	}
	int const n = static_cast<int>(options.size());
	idx = ((idx + delta) % n + n) % n;
	value = options[idx];
}


// a light shade of the replacement, so the swatch reads as the cloth color
UINT16 SwatchColor(ST::string const& id)
{
	auto const idx = GetPaletteRepIndexFromID(id);
	if (!idx) return 0;
	PaletteReplacementType const& rep = gpPalRep[*idx];
	if (rep.ubPaletteSize == 0) return 0;
	SGPPaletteEntry const& c = rep.rgb[rep.ubPaletteSize * 3 / 4];
	return Get16BPPColor(FROMRGB(c.r, c.g, c.b));
}


char const* StandingAnimFile()
{
	if (!fCharacterIsMale) return ANIMSDIR "/f_merc/f_r_std.sti";
	return gIMPAppearance.bigBuild && StrongEnoughForBigBuild() ? ANIMSDIR "/m_merc/m_r_std.sti" : ANIMSDIR "/s_merc/s_r_std.sti";
}


// Draw the merc standing in the field in the picked colors, the same way
// CreateSoldierPalettes() colors a soldier.
void RenderPreview()
{
	INT16 const x = LAPTOP_SCREEN_UL_X + PREVIEW_X;
	INT16 const y = LAPTOP_SCREEN_WEB_UL_Y + PREVIEW_Y;
	ColorFillVideoSurfaceArea(FRAME_BUFFER, x, y, x + PREVIEW_W, y + PREVIEW_H, Get16BPPColor(FROMRGB(54, 58, 44)));
	ColorFillVideoSurfaceArea(FRAME_BUFFER, x + 1, y + 1, x + PREVIEW_W - 1, y + PREVIEW_H - 1, Get16BPPColor(FROMRGB(92, 98, 70)));

	AutoSGPVObject vo(AddVideoObjectFromFile(StandingAnimFile()));
	SGPPaletteEntry pal[256];
	std::copy_n(vo->Palette(), 256, pal);
	SetPaletteReplacement(pal, gIMPAppearance.hair);
	SetPaletteReplacement(pal, gIMPAppearance.vest);
	SetPaletteReplacement(pal, gIMPAppearance.pants);
	SetPaletteReplacement(pal, gIMPAppearance.skin);
	std::unique_ptr<UINT16[]> pal16(Create16BPPPaletteShaded(pal, 255, 255, 255, FALSE));

	UINT16 const frames_per_direction = vo->SubregionCount() / 8;
	// facing the viewer, and from behind over the shoulder
	UINT16 const directions[] = { 3, 0 };
	SGPVSurface::Lock l(FRAME_BUFFER);
	for (size_t i = 0; i != lengthof(directions); ++i)
	{
		UINT16 const region = directions[i] * frames_per_direction;
		INT32 const foot_x = x + 38 + static_cast<INT32>(i) * 56;
		INT32 const foot_y = y + PREVIEW_H - 18;
		Blt8BPPDataTo16BPPBufferTransShadow(l.Buffer<UINT16>(), l.Pitch(), vo.get(), foot_x, foot_y, region, pal16.get());
	}
}


void RenderRows()
{
	for (int r = 0; r != NUM_ROWS; ++r)
	{
		Row const row = static_cast<Row>(r);
		INT16 const y = LAPTOP_SCREEN_WEB_UL_Y + ROW_Y0 + r * ROW_DY;
		SetFontAttributes(FONT12ARIAL, FONT_WHITE);
		MPrint(LAPTOP_SCREEN_UL_X + LABEL_X, y + 3, ROW_LABEL[r]);

		ST::string name;
		if (row == ROW_BUILD)
		{
			if (!fCharacterIsMale)               name = "Regular";
			else if (!StrongEnoughForBigBuild()) name = ST::format("Regular (Big needs {} STR)", IMP_BIG_BODY_MIN_STRENGTH);
			else                                 name = gIMPAppearance.bigBuild ? "Big" : "Regular";
		}
		else
		{
			ST::string const& value = ValueFor(row);
			INT16 const sx = LAPTOP_SCREEN_UL_X + SWATCH_X;
			ColorFillVideoSurfaceArea(FRAME_BUFFER, sx, y, sx + 16, y + 16, Get16BPPColor(FROMRGB(0, 0, 0)));
			ColorFillVideoSurfaceArea(FRAME_BUFFER, sx + 1, y + 1, sx + 15, y + 15, SwatchColor(value));
			name = NiceName(value);
		}
		SetFontAttributes(row == ROW_BUILD && !StrongEnoughForBigBuild() ? FONT10ARIAL : FONT12ARIAL, FONT_YELLOW);
		MPrint(LAPTOP_SCREEN_UL_X + (row == ROW_BUILD ? SWATCH_X : NAME_X), y + 3, name);
	}
}


void BtnArrowCallback(GUI_BUTTON* btn, UINT32 reason)
{
	if (!(reason & MSYS_CALLBACK_REASON_POINTER_UP)) return;
	INT32 const data = btn->GetUserData();
	Cycle(static_cast<Row>(data / 2), data % 2 ? 1 : -1);
	gRedraw = true;
}


void BtnDoneCallback(GUI_BUTTON*, UINT32 reason)
{
	if (!(reason & MSYS_CALLBACK_REASON_POINTER_UP)) return;
	if (!StrongEnoughForBigBuild()) gIMPAppearance.bigBuild = false;

	// the same as leaving the portrait page used to do
	iCurrentImpPage = IMP_MAIN_PAGE;
	if (iCurrentProfileMode < 4) iCurrentProfileMode = 4;
	if (iCurrentProfileMode == 5) iCurrentImpPage = IMP_FINISH;
	fButtonPendingFlag = TRUE;
}

} // namespace


void ResetIMPAppearance()
{
	gIMPAppearance = IMPAppearance{};
	gIMPAppearance.vest  = "WHITEVEST";
	gIMPAppearance.pants = "BLACKPANTS";
	gIMPAppearance.hair  = "BROWNHEAD";
	gIMPAppearance.skin  = "TANSKIN";
}


void ResetIMPAppearanceForPortrait(IMPPortrait const& portrait)
{
	if (gIMPAppearance.vest.empty()) ResetIMPAppearance();
	gIMPAppearance.hair     = portrait.hair;
	gIMPAppearance.skin     = portrait.skin;
	gIMPAppearance.bigBuild = portrait.bigBody;
}


void EnterIMPAppearance()
{
	if (gIMPAppearance.vest.empty()) ResetIMPAppearance();

	gArrowImages[0] = LoadButtonImage(LAPTOPDIR "/attributearrows.sti", 0, 1);
	gArrowImages[1] = LoadButtonImage(LAPTOPDIR "/attributearrows.sti", 3, 4);
	for (int r = 0; r != NUM_ROWS; ++r)
	{
		INT16 const y = LAPTOP_SCREEN_WEB_UL_Y + ROW_Y0 + r * ROW_DY;
		for (int side = 0; side != 2; ++side)
		{
			INT16 const x = LAPTOP_SCREEN_UL_X + (side ? RIGHT_X : LEFT_X);
			GUIButtonRef& b = gArrowButtons[r * 2 + side];
			b = QuickCreateButton(gArrowImages[side], x, y, MSYS_PRIORITY_HIGHEST - 1, BtnArrowCallback);
			b->SetUserData(r * 2 + side);
			b->SetCursor(CURSOR_WWW);
		}
	}

	gDoneImage  = LoadButtonImage(LAPTOPDIR "/button_5.sti", 0, 1);
	gDoneButton = CreateIconAndTextButton(gDoneImage, "Done", FONT12ARIAL,
		FONT_WHITE, DEFAULT_SHADOW, FONT_WHITE, DEFAULT_SHADOW,
		LAPTOP_SCREEN_UL_X + 187 + 140, LAPTOP_SCREEN_WEB_UL_Y + 300, MSYS_PRIORITY_HIGH, BtnDoneCallback);
	gDoneButton->SetCursor(CURSOR_WWW);

	RenderIMPAppearance();
}


void ExitIMPAppearance()
{
	for (auto& b : gArrowButtons) RemoveButton(b);
	UnloadButtonImage(gArrowImages[0]);
	UnloadButtonImage(gArrowImages[1]);
	RemoveButton(gDoneButton);
	UnloadButtonImage(gDoneImage);
}


void HandleIMPAppearance()
{
	if (gRedraw)
	{
		RenderIMPAppearance();
		MarkButtonsDirty();
		gRedraw = false;
	}
}


void RenderIMPAppearance()
{
	RenderProfileBackGround();

	SetFontAttributes(FONT14ARIAL, FONT_WHITE);
	MPrint(LAPTOP_SCREEN_UL_X + LABEL_X, LAPTOP_SCREEN_WEB_UL_Y + 30, "How do you look in the field?");

	// the portrait picked on the previous page
	RenderPortraitFrame(PORTRAIT_X - 9, PORTRAIT_Y - 9);
	if (iPortraitNumber >= 0 && iPortraitNumber < static_cast<INT32>(GetIMPPortraits().size()))
	{
		ST::string const filename = ST::format(FACESDIR "/bigfaces/{}.sti", GetIMPPortraits()[iPortraitNumber].face);
		BltVideoObjectOnce(FRAME_BUFFER, filename.c_str(), 0, LAPTOP_SCREEN_UL_X + PORTRAIT_X, LAPTOP_SCREEN_WEB_UL_Y + PORTRAIT_Y);
	}

	RenderPreview();
	RenderRows();
}
