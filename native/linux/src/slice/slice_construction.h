#pragma once
#include "slice_core.h"

// Read-only entry decoders, also exercised with synthetic libstdc++ objects.
bool SliceConstructionParams(uintptr_t table, SliceRecord* out, size_t* nodes);
bool SliceConstructionRecord(uintptr_t proposal, bool upgrade, SliceRecord* out);

// Existing Windows construction-template weld, adapted to game-owned Linux STL objects.
namespace slice_terrain_assets { struct GameMemory; }
bool SliceMergeTemplateStreet(uintptr_t proposal, const slice_terrain_assets::GameMemory* memory = nullptr);
