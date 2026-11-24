#include "ContentMercs.h"

#include "ContentManager.h"
#include "Dialogs.h"
#include "MercProfile.h"

bool Content::canMercBeHired(const ContentManager *cm, uint16_t profileNum)
{
	// a mod may give the dialogue as <file>.edt.json instead of the binary file
	ST::string const filename = Content::GetDialogueTextFilename(MercProfile(profileNum), false, false);
	return cm->doesGameResExists(filename) || cm->doesGameResExists(filename + ".json");
}
