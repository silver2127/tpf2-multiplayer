// Steam 35924 RandomLocationFactory spacing cost, RVA 0x910ce0.
// Coordinates here are heightmap pixels (4 metres), not octree coordinates.
// Stock computes dx*dx+dy*dy in signed int32 before taking the minimum.
// Preserve its INT_MAX initial minimum, but saturate instead of wrapping.
#pragma once
#include <vector>

static int g_placementAttempts = 200;

static bool InstallFastPlacement()
{
    if (g_placementAttempts == 200) return false;
    if (g_gog || g_placementAttempts < 1 || g_placementAttempts > 200) {
        H->log("fast placement: unsupported build or attempts outside 1..200; OFF");
        return false;
    }
    // RandomLocationFactory worker passes attempts in R9D to the optimizer.
    // Keep the four workers, outer passes and all feasibility tests intact.
    const uint8_t expected[] = {0x41,0xb9,0xc8,0x00,0x00,0x00};
    uint8_t patch[] = {0x41,0xb9,0x00,0x00,0x00,0x00};
    memcpy(patch + 2, &g_placementAttempts, 4);
    if (!H->verifyBytes(0x912f59, expected, sizeof expected) ||
        !H->patchBytes(0x912f59, patch, sizeof patch)) {
        H->log("fast placement: byte check or patch failed; OFF");
        return false;
    }
    H->log("fast placement: %d/200 attempts per worker batch; "
           "requested counts and feasibility checks unchanged", g_placementAttempts);
    return true;
}

struct PlacementPoint { int32_t x, y; float angle; };
struct PlacementExclusion { int32_t x, y; };
template<class T> struct PlacementVector { T* begin; T* end; T* capacity; };
static_assert(sizeof(PlacementPoint) == 12, "game point ABI");
static_assert(sizeof(PlacementVector<float>) == 24, "game vector ABI");

static int32_t PlacementDistanceSquared(int32_t ax, int32_t ay,
                                       int32_t bx, int32_t by)
{
    const int64_t dx = int64_t(ax) - bx, dy = int64_t(ay) - by;
    // This early saturation also handles the full int32 coordinate domain
    // without allowing the sum of two int64 squares to overflow.
    if (dx < -46340 || dx > 46340 || dy < -46340 || dy > 46340)
        return INT32_MAX;
    const int64_t squared = dx * dx + dy * dy;
    return squared > INT32_MAX ? INT32_MAX : int32_t(squared);
}

static void __fastcall PlacementSpacingDetour(PlacementVector<float>* output,
    const PlacementVector<PlacementPoint>* points, float minimumDistance,
    const PlacementVector<PlacementExclusion>* exclusions, float resolution)
{
    if (points->begin == points->end) return;
    const size_t count = points->end - points->begin;
    // Only our scratch allocation uses the plugin CRT. Game vectors remain
    // borrowed storage; their allocation, size and ownership never change.
    std::vector<int32_t> nearest(count, INT32_MAX);
    const float resolutionSquared = resolution * resolution;
    for (size_t i = 0; i < count; ++i) {
        const auto& p = points->begin[i];
        int32_t best = nearest[i];
        for (size_t j = i + 1; j < count; ++j) {
            const auto& q = points->begin[j];
            const int32_t d = PlacementDistanceSquared(p.x, p.y, q.x, q.y);
            if (d < best) best = d;
            if (d < nearest[j]) nearest[j] = d;
        }
        for (auto q = exclusions->begin; q != exclusions->end; ++q) {
            const int32_t d = PlacementDistanceSquared(p.x, p.y, q->x, q->y);
            if (d < best) best = d;
        }
        const float distance = sqrtf(float(best) * resolutionSquared);
        output->begin[i] = minimumDistance <= distance
            ? minimumDistance / distance : 99999.0f;
    }
}

static bool InstallPlacementSpacing()
{
    if (g_gog) {
        H->log("placement spacing: GOG site not measured; fix unavailable");
        return false;
    }
    constexpr uintptr_t rva = 0x910ce0;
    const uint8_t expected[] = {
        0x4c,0x8b,0xdc,0x49,0x89,0x4b,0x08,0x53,
        0x48,0x81,0xec,0xa0,0x00,0x00,0x00
    };
    if (!H->verifyBytes(rva, expected, sizeof expected)) {
        H->log("placement spacing: prologue mismatch; NOT hooked");
        return false;
    }
    void* trampoline = nullptr;
    if (!H->installHook(H->moduleBase() + rva,
            (void*)&PlacementSpacingDetour, sizeof expected, &trampoline)) {
        H->log("placement spacing: installHook failed; NOT hooked");
        return false;
    }
    H->log("placement spacing: int32 squared-distance overflow fixed "
           "(64-bit arithmetic, stock INT_MAX saturation)");
    return true;
}

extern "C" __declspec(dllexport)
void BigmapTestPlacementSpacing(PlacementVector<float>* output,
    const PlacementVector<PlacementPoint>* points, float minimumDistance,
    const PlacementVector<PlacementExclusion>* exclusions, float resolution)
{
    PlacementSpacingDetour(output, points, minimumDistance, exclusions, resolution);
}

extern "C" __declspec(dllexport)
int BigmapTestInstallPlacement(const Tpf2mpHost* host, int gog)
{
    const auto previousHost = H;
    const bool previousGog = g_gog;
    H = host; g_gog = gog != 0;
    const bool result = InstallPlacementSpacing();
    H = previousHost; g_gog = previousGog;
    return result;
}

extern "C" __declspec(dllexport)
int BigmapTestInstallFastPlacement(const Tpf2mpHost* host, int gog, int attempts)
{
    const auto previousHost = H;
    const bool previousGog = g_gog;
    const int previousAttempts = g_placementAttempts;
    H = host; g_gog = gog != 0; g_placementAttempts = attempts;
    const bool result = InstallFastPlacement();
    H = previousHost; g_gog = previousGog; g_placementAttempts = previousAttempts;
    return result;
}
