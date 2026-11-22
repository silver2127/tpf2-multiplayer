// CTerrain's 257x257 uint16 height-cache vectors (132,098 bytes each).
// The mechanism lives in pager_impl.inl, shared with material_pager.h.
#pragma once
#include "terrain_codec.h"

// Twice the largest heightmap-legal tile count (724x724 = 524,176 tiles under
// the (64n+1)^2 <= INT_MAX cap), so both CTerrain versions of the biggest map
// stay managed. 131,072 was already reached transiently on a 256x256 map, and
// any excess silently fell back to uncompressed stock-heap vectors. This is
// address space only: 2^20 slots x 132 KiB = 129 GiB of placeholder reservation.
#define PAGER_NS TerrainPager
#define PAGER_ELEMENT uint16_t
#define PAGER_SAMPLES TerrainCodec::Samples
#define PAGER_BYTES TerrainCodec::RawBytes
#define PAGER_MAX_SLOTS (1u << 20)
#define PAGER_CODEC TerrainCodec
#include "pager_impl.inl"
