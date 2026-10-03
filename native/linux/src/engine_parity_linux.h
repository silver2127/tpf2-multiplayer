#pragma once
#include <cstdint>

// Startup only (before game main): make the verified build-35924 libstdc++
// random distributions and default_random_engine sites behave like the
// Windows (MSVC STL) build. Kill switch: TPF2MP_ENGINE_PARITY=0.
//   - uniform_int_distribution<uint64>(boost mt), <int>/<uint32>/<uint64>(std mt)
//   - std::shuffle<uint32>/<8-byte>(boost mt) (StockListUpdateHelper)
//   - unit-float endpoint of std mt float / boost mt double canonical
//   - TownBuildingTransformator: std::mt19937 sidecar instead of minstd_rand0
//   - NameRep town-name selection: std::mt19937 + MSVC shuffle
//   - AirConnectParts minstd_rand unit float: (float(g) - 1) / 2^31, no clamp
bool Tpf2mpInstallEngineParity(uintptr_t imageBase, const char* buildId);
const char* Tpf2mpEngineParityStatus();
