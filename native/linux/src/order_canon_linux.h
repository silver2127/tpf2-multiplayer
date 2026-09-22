#pragma once
#include <cstdint>
#include <atomic>
namespace tpf2mp_order_detail {
// Published only after every batch/capacity patch is installed.
inline std::atomic<bool> active{false};
}
// Hot-join order canonicalisation (native build 35924).
//
// A world that kept running and the same world loaded from its save hold the
// same entities, but the engine's node lists are in a different order: a load
// registers entities in (reverse topological) id order, a running world in
// add / swap-remove history. Person decisions walk those lists and consume one
// random stream in list order, so a retained host and a freshly loaded joiner
// pick different destinations within a few game units of the resume.
//
// These hooks sort the entity batches each decision consumes, so the decision
// depends on the batch's CONTENT only:
//   candidates  GetTargetsByLandUse's copy of the PersonCapacity node list
//   departures  SimEntityAtBuildingSystem::Update2's leave batch
//   arrivals    PersonMoveSystem::Update2's walk-arrival batch
//   idle        SimEntityIdleSystem's pending list (PathFactory::Compute, per trip)
// Every peer of a session must run them (they change which building a draw
// lands on relative to vanilla). Pending local live validation: TPF2MP_ORDER_CANON=1 opts in; default off.
bool Tpf2mpInstallOrderCanon(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpOrderCanonStatus();
using Tpf2mpOrderCanonLog = void (*)(const char*, ...);
void Tpf2mpOrderCanonSetLog(Tpf2mpOrderCanonLog log);
// calls / batches that were out of order / refused (bad vector), per site
void Tpf2mpOrderCanonCounters(unsigned site, uint64_t* calls, uint64_t* reordered, uint64_t* refused);
