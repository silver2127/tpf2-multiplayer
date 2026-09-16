#pragma once
#include "slice_core.h"
#include <array>
#include <string>
#include <vector>

namespace slice_terrain_assets {
constexpr size_t MaxBytes = 64u << 20;
constexpr size_t MaxAssetGroups = 4096, MaxAssetModels = 20000, MaxAssetString = 511;
constexpr size_t MaxTotalModels = MaxBytes / 69; // smallest TPAS model record
struct Grid {
    std::array<int32_t, 4> rect{}; // x0,y0,width,height
    std::vector<uint8_t> data;
};
struct Terrain {
    Grid height, material, mask; // mask is canonical little-endian u32 words
    uint64_t bits = 0;
};
struct Model { std::string model, extra; std::array<float, 16> matrix{}; };
struct Assets {
    std::vector<std::vector<Model>> groups;
    uint32_t originalRemovals = 0;
    std::vector<int32_t> removals;
};
std::string Base64(const std::vector<uint8_t>& data);
bool Unbase64(const std::string& text, std::vector<uint8_t>* out);
bool EncodeTerrain(const Terrain& terrain, std::vector<uint8_t>* out);
bool DecodeTerrain(const std::vector<uint8_t>& bytes, Terrain* out);
bool EncodeAssets(const Assets& assets, std::vector<uint8_t>* out);
bool DecodeAssets(const std::vector<uint8_t>& bytes, Assets* out);
bool ParseAssetsFile(const std::string& text, Assets* out);
bool ReadTerrain(uintptr_t proposal, Terrain* out);
bool ReadAssets(uintptr_t proposal, Assets* out);
bool CarrierEmpty(uintptr_t proposal);
// An injectable allocator permits off-game lifetime and allocation-failure tests.
// Every allocation transferred to the game must be freed by this exact runtime.
struct GameMemory {
    void* (*allocate)(size_t);
    void (*release)(void*);
    void (*constructEntity)(void*);
};
// Empty until the area verifies the game allocator and constructor at startup.
GameMemory RuntimeMemory();
bool InstallTerrain(uintptr_t proposal, const Terrain& data, const GameMemory& memory);
bool InstallAssets(uintptr_t proposal, const Assets& data, const GameMemory& memory);
}
// Called by recurring UI hooks; only releases a flag on its original UI thread.
void SliceTerrainPollHeldTools();
