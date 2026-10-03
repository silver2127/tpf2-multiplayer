#include <cassert>
#include <unistd.h>
#include <array>
#include <algorithm>
#include "slice/slice_core_internal.h"
#include "../src/slice/train_order_linux.cpp"

template<class T> void Put(std::vector<uint8_t>& mem, size_t offset, T value)
{ memcpy(mem.data() + offset, &value, sizeof(value)); }


static uint32_t fakeSeed = 0xffffffffu;
static int fallbackCalls = 0;
static uintptr_t fakeWorld;
static uint8_t typeNode[24];
static uint32_t GetSeed(void*) { return fakeSeed; }
static uintptr_t FindType(void* registry, const uintptr_t*)
{ assert(uintptr_t(registry) == fakeWorld + 0x48); return uintptr_t(typeNode); }
static void Fallback(int32_t* b, int32_t* e, uint64_t* rng)
{ assert(e - b == 5 && *rng == 123); ++fallbackCalls; }
static void Jump(uintptr_t at, uintptr_t to)
{
    uint8_t code[]{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    memcpy(code + 2, &to, 8); memcpy(reinterpret_cast<void*>(at), code, sizeof(code));
}
// The synthetic CALL/RET site adds one extra return address; bias its
// entry stack by eight so the patched CALL sees the real game alignment.
extern "C" void Fixture(int32_t*, int32_t*, uint64_t*, uintptr_t, uintptr_t, uintptr_t);
asm(".text\n.type Fixture,@function\nFixture:\n.cfi_startproc\n"
    "push %rbp\n.cfi_def_cfa_offset 16\n.cfi_offset %rbp,-16\n"
    "mov %rsp,%rbp\n.cfi_def_cfa_register %rbp\n"
    "push %r14\n.cfi_offset %r14,-24\nsub $0x190,%rsp\n"
    "mov %rcx,%r14\nmov %r8,-0x198(%rbp)\ncall *%r9\n"
    "add $0x190,%rsp\npop %r14\npop %rbp\n.cfi_def_cfa %rsp,8\nret\n"
    ".cfi_endproc\n.size Fixture,.-Fixture\n");

int main()
{
    char flagRoot[] = "/tmp/tpf2mp-order-root.XXXXXX";
    char flagData[] = "/tmp/tpf2mp-order-data.XXXXXX";
    assert(mkdtemp(flagRoot) && mkdtemp(flagData));
    const auto rootFlags = std::string(flagRoot) + "/tpf2_menu_flags.txt";
    const auto dataFlags = std::string(flagData) + "/tpf2_menu_flags.txt";
    auto writeFlags = [](const std::string& path, const char* text) {
        FILE* f = fopen(path.c_str(), "w"); assert(f);
        assert(fputs(text, f) >= 0); assert(fclose(f) == 0);
    };
    assert(!SliceFlagOff(flagRoot, flagData, "trainorder"));
    writeFlags(dataFlags, "trainorder=0\nroadspace=0\n");
    assert(SliceFlagOff(flagRoot, flagData, "trainorder"));
    assert(SliceFlagOff(flagRoot, flagData, "roadspace"));
    writeFlags(rootFlags, "trainorder=1\n");
    assert(!SliceFlagOff(flagRoot, flagData, "trainorder"));
    assert(!SliceFlagOff(flagRoot, flagData, "roadspace"));
    writeFlags(rootFlags, "trainorder=0 # prefix semantics retained\n");
    assert(SliceFlagOff(flagRoot, flagData, "trainorder"));
    assert(!SliceFlagOff(flagRoot, flagData, "roadspace"));
    assert(unlink(rootFlags.c_str()) == 0 && unlink(dataFlags.c_str()) == 0);
    assert(rmdir(flagRoot) == 0 && rmdir(flagData) == 0);
    assert(SliceReadInit());
    constexpr int type = 2;
    std::vector<uint8_t> world(0xb0), pool(0xf0), self(0x50);
    std::array<uintptr_t, 3> pools{0, 0, uintptr_t(pool.data())};
    using Pair = std::array<int32_t, 2>;
    std::array<std::vector<Pair>, 5> entities;
    entities[0] = {{type, 0}};
    entities[1] = {{type, 1}};
    entities[2] = {{type, 0x40000001}};
    entities[3] = {{7, 0}}; // unnamed, no asserting engine lookup
    entities[4] = {{type, 2}};
    std::array<std::string, 3> flat{"Zulu", "alpha", std::string(100, 'b')};
    std::array<std::string, 32> page;
    page[1] = "ALPHA";
    std::array<uintptr_t, 2> pages{uintptr_t(page.data()), 0};
    Put(world, 0x80, uintptr_t(pools.data())); Put(world, 0x98, uintptr_t(entities.data()));
    Put(pool, 0xb8, uintptr_t(flat.data())); Put(pool, 0xd0, uintptr_t(pages.data()));
    const uintptr_t w = uintptr_t(world.data());
    assert(NameComponent(w, 0, type) == uintptr_t(&flat[0]));
    assert(NameComponent(w, 2, type) == uintptr_t(&page[1]));
    assert(NameComponent(w, 3, type) == 0);
    assert(NameComponent(w, -1, type) == 0);
    assert(NameComponent(w, 0, -1) == 0);
    // The batched copy must find a late pair and honor its fixed scratch cap.
    entities[0].assign(4096, Pair{7, 0});
    entities[0].back() = {type, 0};
    assert(NameComponent(w, 0, type) == uintptr_t(&flat[0]));
    entities[0].push_back({type, 0});
    assert(NameComponent(w, 0, type) == 0);
    entities[0].clear();
    assert(NameComponent(w, 0, type) == 0); // no stale thread-local pairs
    entities[0] = {{type, 0}};
    // A matching first pair cannot hide an unreadable tail in the bulk read.
    const size_t pg = size_t(sysconf(_SC_PAGESIZE));
    auto* m = static_cast<uint8_t*>(mmap(nullptr, pg*2, PROT_READ|PROT_WRITE,
                                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
    assert(m != MAP_FAILED);
    Pair match{type, 0}; memcpy(m+pg-8, &match, 8);
    uintptr_t header[]{uintptr_t(m+pg-8), uintptr_t(m+pg+8), uintptr_t(m+pg+8)};
    Put(world, 0x98, uintptr_t(header));
    assert(mprotect(m+pg, pg, PROT_NONE) == 0);
    assert(NameComponent(w, 0, type) == 0);
    assert(munmap(m, pg*2) == 0);
    Put(world, 0x98, uintptr_t(entities.data()));
    // SliceEntityNames gives each id what NameComponent + SliceReadStdString give it alone
    auto oneByOne = [&](int t, const std::vector<int32_t>& ids) {
        std::vector<std::string> out;
        for (int32_t id : ids) {
            char text[TRAINORDER_NAME_MAX + 1]; size_t len = 0;
            const uintptr_t c = NameComponent(w, id, t);
            out.push_back(c && SliceReadStdString(c, text, sizeof(text), &len, TRAINORDER_NAME_MAX) ? std::string(text, len) : "");
        }
        return out;
    };
    auto sameNames = [&](int t, const std::vector<int32_t>& ids) {
        std::vector<std::string> batch{"stale", "stale"};
        SliceEntityNames(w, t, ids.data(), ids.size(), &batch);
        assert(batch == oneByOne(t, ids));
        return batch;
    };
    {
        const std::vector<int32_t> ids{4, 0, -1, 2, 3, 1, 0, 2};
        const auto names = sameNames(type, ids);
        assert(names[0] == std::string(100, 'b') && names[1] == "Zulu" && names[2].empty() &&
               names[3] == "ALPHA" && names[4].empty() && names[5] == "alpha");
        sameNames(-1, ids);
        std::vector<std::string> none{"x"};
        SliceEntityNames(0, type, ids.data(), ids.size(), &none);
        assert(none.size() == ids.size() && std::all_of(none.begin(), none.end(), [](const std::string& s) { return s.empty(); }));
        pages[0] = 0;                                   // the page is gone: the paged name only
        assert(sameNames(type, ids)[3].empty());
        pages[0] = uintptr_t(page.data());
        const std::string saved = flat[1];
        flat[1].assign(TRAINORDER_NAME_MAX + 1, 'x');   // over the length cap
        assert(sameNames(type, ids)[5].empty());
        flat[1] = saved;
        std::vector<int32_t> many;                      // past one SliceReadMany batch
        for (int i = 0; i < 700; ++i) many.push_back(i % 6 - 1);
        sameNames(type, many);
    }
    std::vector<std::array<int32_t, 3>> records{{0,0,0}, {1,0,0}, {2,0,0}, {3,0,0}, {4,0,0}};
    Put(self, 8, uintptr_t(&records));
    const uintptr_t s = uintptr_t(self.data());
    for (uint32_t seed : {0u, 1u, 42u, 0x80000000u, 0xffffffffu}) {
        int32_t order[]{0,1,2,3,4};
        assert(Arrange(order, order + 5, s, w, type, seed));
        std::vector<int> ids;
        for (int i : order) ids.push_back(records[i][0]);
        std::reverse(records.begin(), records.end());
        int32_t other[]{0,1,2,3,4};
        assert(Arrange(other, other + 5, s, w, type, seed));
        for (int i = 0; i < 5; ++i) assert(ids[i] == records[other[i]][0]);
    }
    int32_t bad[]{1,0,2,3,4};
    assert(!Arrange(bad, bad + 5, s, w, type, 1));
    assert(bad[0] == 1 && bad[1] == 0);
    int32_t untouched[]{0,1,2,3,4};
    assert(!Arrange(untouched, untouched + 4, s, w, type, 1));
    assert(!Arrange(untouched, untouched + 5, 0, w, type, 1));
    assert(Arrange(untouched, untouched, 0, w, type, 1)); // empty is valid
    // Read failure means an empty name, as on Windows; no crash on inaccessible text.
    Put(pool, 0xb8, uintptr_t(1));
    assert(Arrange(untouched, untouched + 5, s, w, type, 1));
    {   // unreadable flat data: those names empty, the paged one still read
        const std::vector<int32_t> ids{0, 2, 1};
        const auto names = sameNames(type, ids);
        assert(names[0].empty() && names[1] == "ALPHA" && names[2].empty());
    }
    entities[0] = {{type, -1}};
    assert(NameComponent(w, 0, type) == 0);
    assert(!SliceInstallTrainOrder(0, "", "")); // no readable verified site => no patch

    // Synthetic code image exercises actual guarded installation + near relay.
    // It never maps or executes game code: only our stub functions and CALL/RET.
    const size_t imageSize = 0x5a04000;
    void* image = mmap(nullptr, imageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    const uintptr_t base = uintptr_t(image);
    for (const auto& c : kTrainOrderChecks) memcpy(reinterpret_cast<void*>(base + c.rva), c.bytes, c.size);
    uintptr_t typeName = uintptr_t("N3ecs9component4NameE");
    memcpy(reinterpret_cast<void*>(base + 0x5a02608), &typeName, 8);
    auto* call = reinterpret_cast<uint8_t*>(base + 0x175849d);
    call[1] ^= 1;
    assert(!SliceInstallTrainOrder(base, "", ""));
    assert(call[1] == (0x6e ^ 1)); // failed check wrote nothing
    call[1] ^= 1;
    assert(SliceInstallTrainOrder(base, "", ""));
    call[5] = 0xc3; // fixture resumes with RET, not any engine instructions
    Jump(base + 0xc0cf10, uintptr_t(&GetSeed));
    Jump(base + 0x9e3d50, uintptr_t(&FindType));
    Jump(base + 0x175a510, uintptr_t(&Fallback));
    int storedType = type + 1; memcpy(typeNode + 0x10, &storedType, 4);
    std::array<uintptr_t, 4> timeSystem{0,0,0,1};
    Put(self, 0x48, uintptr_t(timeSystem.data()));
    Put(pool, 0xb8, uintptr_t(flat.data())); entities[0] = {{type, 0}};
    fakeWorld = w;
    uint64_t rng = 123;
    int32_t viaRelay[]{0,1,2,3,4}, expected[]{0,1,2,3,4};
    assert(Arrange(expected, expected + 5, s, w, type, fakeSeed));
    Fixture(viaRelay, viaRelay + 5, &rng, s, w, uintptr_t(call));
    assert(std::equal(viaRelay, viaRelay + 5, expected) && !fallbackCalls);
    int32_t notIota[]{1,0,2,3,4};
    Fixture(notIota, notIota + 5, &rng, s, w, uintptr_t(call));
    assert(fallbackCalls == 1 && notIota[0] == 1 && notIota[1] == 0);
    munmap(image, imageSize);
    puts("PASS Linux train-order: flat/paged names, SSO/heap, absent/unreadable names, registration independence, high seeds, refusals, byte guard");
}
