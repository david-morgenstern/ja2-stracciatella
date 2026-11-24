#pragma once

#include <string_theory/string>

struct IMPPortrait;

/* What an I.M.P. character looks like in the field, picked on the appearance
 * page that follows the portrait page. The colors are palette replacement IDs
 * from ja2pal.dat (hair, skin, vest, pants); the build is only honoured for a
 * man who is strong enough to carry it. */
struct IMPAppearance
{
	ST::string hair;
	ST::string skin;
	ST::string vest;
	ST::string pants;
	bool       bigBuild = false;
};

extern IMPAppearance gIMPAppearance;

// Take the hair, skin and build the portrait suggests; clothes are kept.
void ResetIMPAppearanceForPortrait(IMPPortrait const&);
// Back to the defaults a fresh character starts with.
void ResetIMPAppearance();

void EnterIMPAppearance();
void ExitIMPAppearance();
void HandleIMPAppearance();
void RenderIMPAppearance();
