// The terrain renderer's material-index cells: one 67,601-byte vector<uint8>
// per tile (260x260 indices + canary) in the DataGrid<uint8> at
// RenderDataManager+0x250, retained for every tile after load (measured
// 4.13 GiB on a 256x256 map). Mechanism: pager_impl.inl.
#pragma once
#include "material_codec.h"

// One cell per tile and no second version: 2^19 slots cover the 524,176-tile
// heightmap cap. Slots are 17 pages (69,632 bytes): 34 GiB of placeholder
// address space, a 16 MiB demand-zero slot table.
#define PAGER_NS MaterialPager
#define PAGER_ELEMENT uint8_t
#define PAGER_SAMPLES MaterialCodec::Bytes
#define PAGER_BYTES MaterialCodec::Bytes
#define PAGER_MAX_SLOTS (1u << 19)
#define PAGER_CODEC MaterialCodec
#include "pager_impl.inl"
