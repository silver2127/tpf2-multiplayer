#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
// Town development trace (native build 35924). A diagnostic, OFF by default:
// TPF2MP_TOWN_TRACE=1 installs it, anything else leaves the game untouched
// (no hook, no file). Line format and meaning: native/src/town_trace.h; the
// Windows slice writes the same lines (towntrace=1 in tpf2_slice.cfg), and
// tools/town_trace_diff.py names the first line two peers disagree on.
//
// Sites: the TownSystem update's `call TownDeveloper::Develop` (0x1747917) is
// pointed at a logging wrapper; the tick's clock and the update's context come
// from the town seed hook (town_seed_linux.cpp), and the node-list digests
// from the per-iteration family sort (family_canon_linux.inl).
namespace tpf2mp_town_trace { inline std::atomic<bool> on{false}; }
bool Tpf2mpInstallTownTrace(uintptr_t imageBase, const char* buildId, const char* path);
const char* Tpf2mpTownTraceStatus();
// TownSystem update, before its generator is seeded: the tick's GameTime+0x34
// and the update's context (context+8 -> the Town node vector).
void Tpf2mpTownTraceTick(uint32_t time, uintptr_t context);
// Per-iteration family sort: whether this engine owes a TF line now, and the line.
bool Tpf2mpTownTraceFamiliesDue(uintptr_t engine, int64_t* time, int* engineIndex);
void Tpf2mpTownTraceFamilies(int64_t time, int engineIndex, uint64_t* tokens, size_t count);
// Test seam: the Develop wrapper with an explicit original.
using Tpf2mpTownDevelopFn = void (*)(void*, void*, uint32_t, bool, void*, uint64_t, void*);
void Tpf2mpTownTraceSetOriginal(Tpf2mpTownDevelopFn original);
void Tpf2mpTownTraceDevelop(void* self, void* engine, uint32_t town, bool flag, void* mt, uint64_t opt, void* progress);
void Tpf2mpTownTraceOpenForTest(const char* path);
void Tpf2mpTownTraceFlush();
