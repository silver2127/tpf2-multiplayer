// Steam 35924: the urbansim travel-time limits, two float cells in .rdata.
//
//   0x3094978  1200.0f  (20 min at 1x)  read by
//     PathFactory::Compute (0x90e880) for entities WITHOUT SimCargo (people),
//     destination_util scoring (0x928370, people's destination choice), and
//     the destination_util task (0x928ed0) that calls
//     simulation_util::path_finder::GetReachableStationsBFS(node, maxTime, ...,
//     CargoTypeId, ...) for every cargo type, passengers included: the stations
//     a cargo type can reach from a station within the limit.
//   0x309497c  6000.0f  (100 min)  read by
//     PathFactory::Compute for entities WITH SimCargo, and
//     stocklistsystem GetRandomTarget (0xaa2df0).
//
// Both are plain data, each read by exactly the functions above (RIP-relative
// scan of .text, 2026-09-17), so a new value is a 4-byte rewrite of the cell
// after verifying the stock bytes. The values are game seconds (730 s = one
// game year at 1x). 0 keeps stock; anything else is clamped to 60..86400.
#pragma once
static int g_travelTimeLimit = 0, g_cargoPathTime = 0;
struct TravelCell { uintptr_t rva; float stock; int* value; const char* what; };
static bool InstallTravelTime() {
    if (g_gog) return false;
    const TravelCell cells[] = {
        {0x3094978, 1200.0f, &g_travelTimeLimit, "travel time limit (reachable stations, people's paths and destinations)"},
        {0x309497c, 6000.0f, &g_cargoPathTime,   "cargo path time (cargo paths and stock-list targets)"},
    };
    int done = 0;
    for (const auto& c : cells) {
        if (*c.value <= 0) continue;
        int v = *c.value < 60 ? 60 : *c.value > 86400 ? 86400 : *c.value;
        uint8_t before[4], after[4];
        float fs = c.stock, fv = float(v);
        memcpy(before, &fs, 4); memcpy(after, &fv, 4);
        if (!H->verifyBytes(c.rva, before, 4)) { H->log("travel time: byte mismatch at %llx; %s left stock", (unsigned long long)c.rva, c.what); continue; }
        if (!H->patchBytes(c.rva, after, 4)) { H->log("travel time: patch failed at %llx; %s left stock", (unsigned long long)c.rva, c.what); continue; }
        H->log("travel time: %s %.0f -> %d s", c.what, double(c.stock), v);
        ++done;
    }
    return done > 0;
}
extern "C" __declspec(dllexport)
int BigmapTestInstallTravelTime(const Tpf2mpHost* host, int gog, int limit, int cargoPath) {
    const auto oldHost = H; const bool oldGog = g_gog; const int oldLimit = g_travelTimeLimit, oldCargo = g_cargoPathTime;
    H = host; g_gog = gog != 0; g_travelTimeLimit = limit; g_cargoPathTime = cargoPath;
    bool ok = InstallTravelTime();
    H = oldHost; g_gog = oldGog; g_travelTimeLimit = oldLimit; g_cargoPathTime = oldCargo;
    return ok;
}
