#pragma once
#include <cstdint>

// Translate only the native seed from SimPersonSystem::NoteAtBuildingPersonsLeave.
// Its verified formula bijectively encodes the 32-bit game-time value. Recover
// those bits and apply the Windows build's FNV/hash_combine formula for tag 4.
// The clock, engine state, MT constructor and other callers are untouched.
uint32_t Tpf2mpWindowsDepartureSeed(uint32_t nativeSeed);

// NoteWalkPersonsArrived has the analogous tag-3/time seed formula.
uint32_t Tpf2mpWindowsArrivalSeed(uint32_t nativeSeed);

// Shared Windows hash_combine(tag, time) for verified time-only engine seeds.
// Inputs are the original 32-bit representation, including negative times.
uint32_t Tpf2mpWindowsTimeSeed(uint32_t tag, uint32_t timeBits);

// NoteSimEntityIdleChanged uses tag 9, time and entity ID. Both input bit
// patterns must come from its verified call context; its truncated native
// seed alone cannot recover these two inputs.
uint32_t Tpf2mpWindowsIdleSeed(uint32_t timeBits, uint32_t entityBits);

uint32_t Tpf2mpWindowsEntitySeed(uint32_t tag, uint32_t timeBits, uint32_t entityBits);

// Recover time from the original pre-constructor register value. With mixed
// true, intermediate is signext(entity)+K+(S<<6); otherwise it is S, the native
// tag/time hash. Only the verified call-site mappings supply these values.
uint32_t Tpf2mpWindowsEntitySeedFromNative(uint32_t tag, uint64_t intermediate,
                                        uint32_t entityBits, bool mixed);
