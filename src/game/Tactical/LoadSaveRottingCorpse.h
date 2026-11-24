#ifndef LOADSAVEROTTINGCORPSE_H
#define LOADSAVEROTTINGCORPSE_H

#include "Rotting_Corpses.h"


void ExtractRottingCorpseFromFile(HWFILE, ROTTING_CORPSE_DEFINITION*);
void InjectRottingCorpseIntoFile(HWFILE, ROTTING_CORPSE_DEFINITION const*);

// The same record in memory (co-op sends it over the network).
constexpr size_t ROTTING_CORPSE_RECORD_SIZE = 160;
void ExtractRottingCorpse(BYTE const* data, ROTTING_CORPSE_DEFINITION*);
void InjectRottingCorpse(BYTE* data, ROTTING_CORPSE_DEFINITION const*);

#endif
