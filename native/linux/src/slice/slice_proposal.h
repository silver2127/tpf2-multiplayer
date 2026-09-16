// Build 35924 proposal decoders. Offsets and wire contracts are documented in
// docs/re/linux/SLICE_PROPOSAL.md. These functions only read game memory and
// append to caller-owned records; they never patch, cancel, or call game code.
#pragma once
#include "slice_core.h"

enum class SliceProposalKind {
    None, Road, ConstructionRoad, Stop, StopDelete, EdgeDemolish, ConstructionDemolish
};

// On failure the output must be discarded, including any already appended text.
// construction selects ROADC; otherwise upgrade selects ROADE with removals.
bool SliceProposalBuildRoadRecord(uintptr_t proposal, bool construction, bool upgrade,
                                  SliceRecord* out);
bool SliceProposalBuildStopRecord(uintptr_t proposal, SliceRecord* out);
SliceProposalKind SliceProposalBuildBulldozeRecord(uintptr_t proposal, SliceRecord* out);
