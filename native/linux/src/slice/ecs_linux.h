// The Linux engine's component walk, without the asserting accessor.
//
// ecs::Engine::GetComponentDataIndex 0x9e5590 aborts when the entity lacks the
// component (Engine.h:291 `it != components.end()`), and every user here --
// HUD icons, a rename -- sees entities that legitimately have neither
// PlayerOwned nor StationGroup. So the entity's own component record is read
// directly, the way train_order_linux.cpp already reads a Name.
//
// Layout, watched under gdb in the running game and fixed statically by the
// engine's own PlayerOwned read at 0x138babb/0x138ba66:
//   engine+0x80  the component pool table, indexed by type
//   engine+0x98  the per-entity records, 24 bytes each: a std::vector of
//                8-byte (type, slot) pairs
//   pool+0xb8/+0xc0  the component vector's begin/end  (slot < 0x40000000)
//   pool+0xd0        the page table, 16 bytes per entry, 32 components a page
// Evidence: docs/re/linux/DEV_D6DB920F.md.
#pragma once
#include <cstddef>
#include <cstdint>

// typeinfo objects, from their RTTI name strings in the shipped ELF.
constexpr uintptr_t SLICE_RVA_TYPE_FIND   = 0x9e3d50;    // node (engine+0x48, const type_info**)
constexpr uintptr_t SLICE_TI_PLAYER       = 0x5a025f0;   // ecs::component::Player
constexpr uintptr_t SLICE_TI_PLAYEROWNED  = 0x5a01c18;   // ecs::component::PlayerOwned
constexpr uintptr_t SLICE_TI_STATIONGROUP = 0x5a01b98;   // ecs::component::StationGroup

constexpr size_t SLICE_STRIDE_PLAYEROWNED  = 4;    // one ecs::Entity
constexpr size_t SLICE_STRIDE_STATIONGROUP = 24;   // one std::vector<ecs::Entity>

// Must be called once with the verified image base before anything else here.
void SliceEcsSetBase(uintptr_t base);
// Every byte anchor this walk depends on still matches build 35924.
bool SliceEcsAnchored(uintptr_t base);

// -1 when the engine does not know the type.
int SliceEcsTypeIndex(uintptr_t engine, uintptr_t typeinfoRva);
// -1 when the entity does not have the component. Never asserts.
int32_t SliceEcsComponentSlot(uintptr_t engine, int32_t entity, int type);
// 0 unless the whole component lies inside the pool's own vector/page.
uintptr_t SliceEcsComponentAt(uintptr_t engine, int type, int32_t slot, size_t stride);

// The company that owns `entity`: its PlayerOwned, or -- when it has none --
// the PlayerOwned of its station group's first station. -1 for a town, an
// industry, a plain building, or a company entity itself.
int SliceEcsOwner(uintptr_t engine, int32_t entity);
// True when `entity` is a company: it carries a Player component. Proven live
// on the lab save (entity 19427 "ComradeSilver Transport": Player yes,
// PlayerOwned no).
bool SliceEcsIsCompany(uintptr_t engine, int32_t entity);
