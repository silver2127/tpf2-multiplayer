// The HUD company tint's owner walk, against a synthetic engine laid out the
// way gdb found the real one (docs/re/linux/DEV_D6DB920F.md): engine+0x80 =
// the component pools, engine+0x98 = the per-entity component records, and in
// a pool +0xb0 count, +0xb8/+0xc0 the flat vector, +0xd0 the page table.
#include <cassert>
#include <cstring>
#include <sys/mman.h>
#include <vector>
#include "slice/slice_core_internal.h"
#include "../src/slice/ecs_linux.h"
#include "../src/slice/ecs_checks_linux.h"
#include "../src/slice/company_tint_linux.cpp"

namespace {

constexpr int TI_PLAYEROWNED = 52, TI_STATIONGROUP = 55, TI_PLAYER = 18;

// A fake image: one reservation big enough for every RVA the tint uses, with
// only the pages it touches made readable/executable.
uint8_t* image;
void* Window(uintptr_t rva, size_t len, int prot)
{
    uint8_t* at = image + (rva & ~uintptr_t(0xfff));
    assert(mprotect(at, (len + (rva & 0xfff) + 0xfff) & ~size_t(0xfff), prot) == 0);
    return image + rva;
}

std::vector<std::pair<void*, std::string>> applied;
void FakeAddStyleClass(void* widget, GStr* cls)
{
    // Exactly what the engine's addStyleClass promises: an empty string is
    // dropped, anything else is taken and the source is cleared.
    assert(cls->p == cls->buf || cls->len >= 16);
    if (!cls->len) return;
    applied.emplace_back(widget, std::string(cls->p, cls->len));
    cls->p = cls->buf;
    cls->len = 0;
    cls->buf[0] = 0;
}

uint64_t nodePlayerOwned[4], nodeStationGroup[4], nodePlayer[4];
uintptr_t FakeTypeFind(void* mgr, const uintptr_t* ti)
{
    assert(mgr != nullptr);
    if (*ti == reinterpret_cast<uintptr_t>(image) + SLICE_TI_PLAYEROWNED)
        return reinterpret_cast<uintptr_t>(nodePlayerOwned);
    if (*ti == reinterpret_cast<uintptr_t>(image) + SLICE_TI_STATIONGROUP)
        return reinterpret_cast<uintptr_t>(nodeStationGroup);
    if (*ti == reinterpret_cast<uintptr_t>(image) + SLICE_TI_PLAYER)
        return reinterpret_cast<uintptr_t>(nodePlayer);
    return 0;
}

// ---- the synthetic world ---------------------------------------------------
struct Pool {
    uint8_t bytes[0x100];
    std::vector<uint8_t> data;
    std::vector<uintptr_t> pageTable;
    std::vector<uint8_t> page;
    void Flat(const void* src, size_t n, size_t count)
    {
        data.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + n);
        memset(bytes, 0, sizeof(bytes));
        uintptr_t begin = reinterpret_cast<uintptr_t>(data.data()), end = begin + n;
        memcpy(bytes + 0xb0, &count, 8);
        memcpy(bytes + 0xb8, &begin, 8);
        memcpy(bytes + 0xc0, &end, 8);
    }
    void Paged(const void* src, size_t n)
    {
        page.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + n);
        pageTable.assign(4, 0);
        pageTable[0] = reinterpret_cast<uintptr_t>(page.data());
        uintptr_t table = reinterpret_cast<uintptr_t>(pageTable.data());
        memcpy(bytes + 0xd0, &table, 8);
    }
};

struct Record { uintptr_t begin, end, cap; };   // std::vector<pair<int,int>>

struct World {
    uint8_t engine[0x200]{};
    std::vector<uintptr_t> pools;
    std::vector<Record> records;
    std::vector<std::vector<int32_t>> pairs;
    void Build(size_t entityCount, size_t poolCount)
    {
        pools.assign(poolCount, 0);
        records.assign(entityCount, Record{0, 0, 0});
        pairs.assign(entityCount, {});
        uintptr_t p = reinterpret_cast<uintptr_t>(pools.data());
        uintptr_t r = reinterpret_cast<uintptr_t>(records.data());
        memcpy(engine + 0x80, &p, 8);
        memcpy(engine + 0x98, &r, 8);
    }
    void Give(int32_t entity, int type, int32_t slot)
    {
        auto& v = pairs[entity];
        v.push_back(type);
        v.push_back(slot);
        records[entity].begin = reinterpret_cast<uintptr_t>(v.data());
        records[entity].end = records[entity].begin + v.size() * 4;
        records[entity].cap = records[entity].end;
    }
    void Install(int type, Pool& pool) { pools[type] = reinterpret_cast<uintptr_t>(pool.bytes); }
    uintptr_t Engine() { return reinterpret_cast<uintptr_t>(engine); }
};


// Put the engine's own component-read bytes into a synthetic image, so the
// anchor gate that guards the walk passes on it exactly as on the real one.
static void PlantEcsAnchors(uint8_t* image)
{
    for (const auto& c : kEcsChecks) {
        uint8_t* page = image + (c.rva & ~uintptr_t(0xfff));
        assert(mprotect(page, ((c.rva & 0xfff) + c.size + 0xfff) & ~size_t(0xfff),
                        PROT_READ | PROT_WRITE) == 0);
        std::memcpy(image + c.rva, c.bytes, c.size);
    }
}

} // namespace

int main()
{
    image = static_cast<uint8_t*>(mmap(nullptr, 0x6000000, PROT_NONE,
                                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    assert(image != MAP_FAILED);
    assert(SliceReadInit());
    tintBase = reinterpret_cast<uintptr_t>(image);
    SliceEcsSetBase(tintBase);
    PlantEcsAnchors(image);
    assert(SliceEcsAnchored(tintBase));
    tintRoot.clear();
    tintData.clear();

    // Point the two RVAs the walk calls at our own functions through a stub.
    auto stub = [](uintptr_t rva, void* fn) {
        uint8_t* at = static_cast<uint8_t*>(Window(rva, 12, PROT_READ | PROT_WRITE));
        uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};   // mov rax,imm64; jmp rax
        uintptr_t target = reinterpret_cast<uintptr_t>(fn);
        memcpy(code + 2, &target, 8);
        memcpy(at, code, sizeof(code));
        Window(rva, 12, PROT_READ | PROT_EXEC);
    };
    stub(RVA_ADD_STYLE_CLASS, reinterpret_cast<void*>(&FakeAddStyleClass));
    stub(SLICE_RVA_TYPE_FIND, reinterpret_cast<void*>(&FakeTypeFind));
    Window(SLICE_TI_PLAYEROWNED, 8, PROT_READ | PROT_WRITE);
    Window(SLICE_TI_STATIONGROUP, 8, PROT_READ | PROT_WRITE);
    Window(SLICE_TI_PLAYER, 8, PROT_READ | PROT_WRITE);
    nodePlayerOwned[2] = TI_PLAYEROWNED + 1;      // node+0x10 holds index+1
    nodeStationGroup[2] = TI_STATIONGROUP + 1;
    nodePlayer[2] = TI_PLAYER + 1;

    // ---- a world shaped like the lab's save --------------------------------
    // 14 PlayerOwned entries, all the company entity 19427; two station groups.
    World w;
    w.Build(40000, 64);
    int32_t owners[14];
    for (int32_t& o : owners) o = 19427;
    Pool po;
    po.Flat(owners, sizeof(owners), 14);
    w.Install(TI_PLAYEROWNED, po);

    int32_t stations[1] = {28300};
    struct GroupVec { uintptr_t begin, end, cap; } groups[2];
    groups[0] = {0, 0, 0};
    groups[1] = {reinterpret_cast<uintptr_t>(stations),
                 reinterpret_cast<uintptr_t>(stations) + sizeof(stations),
                 reinterpret_cast<uintptr_t>(stations) + sizeof(stations)};
    Pool sg;
    sg.Flat(groups, sizeof(groups), 2);
    w.Install(TI_STATIONGROUP, sg);

    w.Give(28301, TI_STATIONGROUP, 1);
    w.Give(28301, TI_PLAYEROWNED, 7);
    w.Give(28300, TI_PLAYEROWNED, 6);
    w.Give(19427, TI_PLAYER, 0);   // the company itself: a Player, no PlayerOwned
    w.Give(500, 22, 0);            // a town: no owner at all

    // the slot scan finds and misses without ever calling the engine
    assert(SliceEcsComponentSlot(w.Engine(), 28301, TI_PLAYEROWNED) == 7);
    assert(SliceEcsComponentSlot(w.Engine(), 28301, TI_STATIONGROUP) == 1);
    assert(SliceEcsComponentSlot(w.Engine(), 500, TI_PLAYEROWNED) == -1);
    assert(SliceEcsComponentSlot(w.Engine(), -1, TI_PLAYEROWNED) == -1);

    // stride 4 addressing, and the pool's own vector bounds the read
    assert(SliceEcsComponentAt(w.Engine(), TI_PLAYEROWNED, 7, SLICE_STRIDE_PLAYEROWNED) ==
           reinterpret_cast<uintptr_t>(po.data.data()) + 7 * 4);
    assert(SliceEcsComponentAt(w.Engine(), TI_PLAYEROWNED, 14, SLICE_STRIDE_PLAYEROWNED) == 0);
    assert(SliceEcsComponentAt(w.Engine(), TI_PLAYEROWNED, 13, 8) == 0);   // a wrong stride runs off the end
    assert(SliceEcsComponentAt(w.Engine(), TI_STATIONGROUP, 1, SLICE_STRIDE_STATIONGROUP) ==
           reinterpret_cast<uintptr_t>(sg.data.data()) + 24);

    // the owner: directly from PlayerOwned, as the Linux build actually stores it
    assert(SliceEcsOwner(w.Engine(), 28301) == 19427);
    assert(SliceEcsOwner(w.Engine(), 28300) == 19427);
    // a town and the company entity have no owner; neither may assert
    assert(SliceEcsOwner(w.Engine(), 500) == -1);
    assert(SliceEcsOwner(w.Engine(), 19427) == -1);
    assert(SliceEcsOwner(w.Engine(), 39999) == -1);

    // the Windows shape: a group that is not owned itself falls back to its
    // first station. A second engine pointer, so the type cache is rebuilt.
    World w2;
    w2.Build(40000, 64);
    Pool po2;
    po2.Flat(owners, sizeof(owners), 14);
    w2.Install(TI_PLAYEROWNED, po2);
    Pool sg2;
    sg2.Flat(groups, sizeof(groups), 2);
    w2.Install(TI_STATIONGROUP, sg2);
    w2.Give(28301, TI_STATIONGROUP, 1);        // no PlayerOwned on the group
    w2.Give(28300, TI_PLAYEROWNED, 6);
    assert(SliceEcsOwner(w2.Engine(), 28301) == 19427);
    // an empty group is not an owner
    World w3;
    w3.Build(40000, 64);
    Pool po3;
    po3.Flat(owners, sizeof(owners), 14);
    w3.Install(TI_PLAYEROWNED, po3);
    Pool sg3;
    sg3.Flat(groups, sizeof(groups), 2);
    w3.Install(TI_STATIONGROUP, sg3);
    w3.Give(28301, TI_STATIONGROUP, 0);        // groups[0] is the empty vector
    assert(SliceEcsOwner(w3.Engine(), 28301) == -1);

    // the paged half of the pool layout (slot >= 0x40000000)
    World w4;
    w4.Build(40000, 64);
    int32_t paged[32];
    for (int32_t& o : paged) o = 777;
    Pool po4;
    po4.Flat(owners, sizeof(owners), 14);
    po4.Paged(paged, sizeof(paged));
    w4.Install(TI_PLAYEROWNED, po4);
    Pool sg4;
    sg4.Flat(groups, sizeof(groups), 2);
    w4.Install(TI_STATIONGROUP, sg4);
    w4.Give(4242, TI_PLAYEROWNED, 0x40000000 + 5);
    assert(SliceEcsComponentAt(w4.Engine(), TI_PLAYEROWNED, 0x40000000 + 5, SLICE_STRIDE_PLAYEROWNED) ==
           reinterpret_cast<uintptr_t>(po4.page.data()) + 5 * 4);
    assert(SliceEcsOwner(w4.Engine(), 4242) == 777);

    // ---- the class the icon gets ------------------------------------------
    applied.clear();
    int widget = 0;
    AppendClass(&widget, 2);
    AppendClass(&widget, 200);
    assert(applied.size() == 2);
    assert(applied[0].first == &widget && applied[0].second == "mpWinCo2");
    assert(applied[1].second == "mpWinCo200");
    // every company id the stylesheet defines stays inside the local buffer,
    // so the string never owns heap the game would have to free
    for (int cid = 1; cid <= 200; ++cid) {
        char text[32];
        snprintf(text, sizeof(text), "%s%d", ClassPrefix(), cid);
        assert(strlen(text) < sizeof(GStr::buf));
    }
    // an unknown pid is not a company, so nothing is appended
    assert(CompanyOfPid(19427) == 0);

    // ---- who slice-lines may rename --------------------------------------
    // Only an entity with a Player component is the game's company window.
    assert(SliceEcsIsCompany(w.Engine(), 19427));
    assert(!SliceEcsIsCompany(w.Engine(), 28301));
    assert(!SliceEcsIsCompany(w.Engine(), 500));

    printf("company_tint: ok -- slot scan, stride 4 / 24 with pool bounds, flat and paged, "
           "direct and station-group owners, towns refused, %zu classes appended\n", applied.size());
    return 0;
}
