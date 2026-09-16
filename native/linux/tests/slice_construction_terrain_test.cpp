// Actual guarded reads, Windows-wire goldens, adversarial payloads and ownership
// transfer/failure tests. Optional argv[1] supplies the verified game ELF ctor.
#include "slice/slice_construction.h"
#include "slice/slice_terrain_assets.h"
#include "slice/slice_core_internal.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

using namespace slice_terrain_assets;
using namespace std::string_literals;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
static std::map<std::string, SliceFactoryHandler> handlers;
static SliceArm arm{};
static bool armed = false, canArm = false;
static SliceHookSpec dtorHook{};
void FakeDestroy(void* tool) { static_cast<uint8_t*>(tool)[0xd0] = 0x7f; }
bool SliceRegisterHook(const SliceHookSpec& h) {
    CHECK(h.rva == 0xe58980 && h.steal == 5);
    dtorHook = h; *h.trampoline = reinterpret_cast<void*>(FakeDestroy); return true;
}
bool SliceHookInstalled(uintptr_t rva) { return rva == 0xe58980 && dtorHook.detour; }
bool SliceOnFactory(const SliceFactoryHandler& h) { handlers[h.area] = h; return true; }
bool SliceOnAdd(const char*, SliceAddObserverFn, void*) { return true; }
bool SliceCancelAvailable() { return canArm; }
bool SliceArmCancel(const SliceFactoryCall&, const SliceArm& a) { if (!canArm) return false; arm = a; armed = true; return true; }
const char* SliceOutcomeName(SliceOutcome) { return "test"; }

template<size_t N> using Bytes = std::array<uint8_t, N>;
template<class T> void Put(void* p, size_t offset, const T& v) { std::memcpy(static_cast<uint8_t*>(p) + offset, &v, sizeof(v)); }
template<class T> T Get(const void* p, size_t offset) { T v; std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(v)); return v; }
template<class T> void VectorAt(void* p, size_t offset, std::vector<T>& vector) { std::memcpy(static_cast<uint8_t*>(p) + offset, &vector, 24); }

// lua::Value's exact layout without recursively depending on a C++ variant ABI.
struct Value { Bytes<0x30> payload{}; uint8_t tag = 0; uint8_t pad[7]{}; };
static_assert(sizeof(Value) == 0x38);
struct Less { bool operator()(const Value& a, const Value& b) const {
    if (a.tag != b.tag) return a.tag < b.tag;
    if (a.tag == 2) return Get<double>(a.payload.data(), 0) < Get<double>(b.payload.data(), 0);
    return std::strcmp(Get<const char*>(a.payload.data(), 0), Get<const char*>(b.payload.data(), 0)) < 0;
} };
using Table = std::map<Value, Value, Less>;
Value Number(double v) { Value a; a.tag = 2; Put(a.payload.data(), 0, v); return a; }
Value String(const std::string& v) { Value a; a.tag = 3; std::memcpy(a.payload.data(), &v, sizeof(v)); return a; }
Value Boolean(bool v) { Value a; a.tag = 1; a.payload[0] = v; return a; }
Value Nested(const Table& v) { Value a; a.tag = 4; std::memcpy(a.payload.data(), &v, sizeof(v)); return a; }
void Construction()
{
    std::string seed = "seed", enabled = "enabled", modules = "modules", escaped = "a\n\"\\\0z"s;
    Table nested; nested.emplace(Number(1), Boolean(true)); nested.emplace(Number(2), String(escaped));
    Table table; table.emplace(String(seed), Number(123)); table.emplace(String(enabled), Boolean(false));
    auto mod = table.emplace(String(modules), Value{}).first;
    new (mod->second.payload.data()) Table(nested); mod->second.tag = 4;
    SliceRecord out{}; size_t nodes = 0;
    CHECK(SliceConstructionParams(uintptr_t(&table), &out, &nodes));
    CHECK(nodes == 5);
    CHECK(std::string(out.data) == "{[\"enabled\"]=false,[\"modules\"]={[1]=true,[2]=\"a\\010\\\"\\\\\\000z\"},[\"seed\"]=123}");
    SliceRecordFree(&out);
    // 0.5.6 regression: modular station params must not truncate at 8 KB.
    // Exceed both the old node and text limits.
    Table large;
    const std::string payload(1024, 'x');
    std::string expected = "{";
    for (int i = 1; i <= 2050; ++i) {
        large.emplace(Number(i), String(payload));
        if (i > 1) expected += ",";
        expected += "[" + std::to_string(i) + "]=\"" + payload + "\"";
    }
    expected += "}";
    CHECK(expected.size() > 65536);
    CHECK(SliceConstructionParams(uintptr_t(&large), &out, &nodes));
    CHECK(nodes == 2050 && std::string(out.data, out.len) == expected);
    SliceRecordFree(&out);
    // 100 nested maps use a heap traversal stack, with valid parent headers.
    Table deep;
    std::vector<Table*> nestedTables;
    Table* at = &deep;
    for (int i=0; i<100; ++i) {
        auto entry = at->emplace(Number(i), Value{}).first;
        entry->second.tag = 4;
        at = new (entry->second.payload.data()) Table;
        nestedTables.push_back(at);
    }
    at->emplace(Number(100), Boolean(true));
    CHECK(SliceConstructionParams(uintptr_t(&deep), &out, &nodes) && nodes == 101);
    SliceRecordFree(&out);
    for (auto i=nestedTables.rbegin(); i!=nestedTables.rend(); ++i) (*i)->~Table();
    Table bad; bad.emplace(Number(1), Number(INFINITY));
    CHECK(!SliceConstructionParams(uintptr_t(&bad), &out, nullptr)); SliceRecordFree(&out);
    CHECK(!SliceConstructionParams(1, &out, nullptr)); SliceRecordFree(&out);
    Table empty;
    CHECK(SliceConstructionParams(uintptr_t(&empty), &out, nullptr)); SliceRecordFree(&out);
    // Cycle nested payload back into itself: must reject the ancestor alias, never hang.
    Table cycle; cycle.emplace(Number(1), Value{});
    cycle.begin()->second = Nested(cycle);
    CHECK(!SliceConstructionParams(uintptr_t(&cycle), &out, nullptr)); SliceRecordFree(&out);

    Bytes<0x3c0> p{}; std::vector<Bytes<0x8f0>> ces(2); ces.resize(1); std::vector<int32_t> rm{42};
    std::string file = "station/rail/modular.con";
    std::memcpy(ces[0].data(), &file, sizeof(file));
    auto* params = new (ces[0].data() + 0x448) Table;
    params->emplace(String(seed), Number(123));
    float t[16]{}; for (int i = 0; i < 16; i += 5) t[i] = 1; t[12] = 2.5f;
    std::memcpy(ces[0].data() + 0x738, t, sizeof(t));
    VectorAt(p.data(), 0x288, rm); VectorAt(p.data(), 0x2a0, ces);
    CHECK(SliceConstructionRecord(uintptr_t(p.data()), true, &out));
    CHECK(std::string(out.data).find("CONUP 42 station/rail/modular.con t=1.0000,") == 0); SliceRecordFree(&out);
    CHECK(SliceConstructionRecord(uintptr_t(p.data()), false, &out)); SliceRecordFree(&out);
    rm.push_back(43); VectorAt(p.data(), 0x288, rm);
    CHECK(SliceConstructionRecord(uintptr_t(p.data()), true, &out)); SliceRecordFree(&out);
    rm.clear(); VectorAt(p.data(), 0x288, rm);
    CHECK(SliceConstructionRecord(uintptr_t(p.data()), false, &out)); SliceRecordFree(&out);
    file = "invalid path.con"; std::memcpy(ces[0].data(), &file, sizeof(file));
    CHECK(!SliceConstructionRecord(uintptr_t(p.data()), false, &out)); SliceRecordFree(&out);
    file = "station/rail/modular.con"; std::memcpy(ces[0].data(), &file, sizeof(file));
    // Existing Windows capture takes the first CE, even when the proposal
    // contains companion constructions. Reserving above preserves table address.
    ces.emplace_back(); VectorAt(p.data(), 0x2a0, ces);
    CHECK(SliceConstructionRecord(uintptr_t(p.data()), false, &out)); SliceRecordFree(&out);
    params->~Table();
    reinterpret_cast<Table*>(mod->second.payload.data())->~Table();
}

Terrain SampleTerrain(uint64_t bits = 65)
{
    Terrain t;
    t.height.rect = {-2, 3, 1, 1}; t.height.data.resize(8);
    Put(t.height.data.data(), 0, 123.25f); Put(t.height.data.data(), 4, 120.0f);
    t.material.rect = {4, 5, int32_t(bits), 1}; t.material.data.assign(bits, 0xff); t.material.data[0] = 3;
    t.mask.rect = t.material.rect; t.bits = bits; t.mask.data.assign(((bits + 31) / 32) * 4, 0xff);
    return t;
}
void Codecs()
{
    std::vector<uint8_t> bytes, raw;
    CHECK(Base64({}) == ""); CHECK(Base64({'M'}) == "TQ==");
    CHECK(Base64({'M','a'}) == "TWE="); CHECK(Base64({'M','a','n'}) == "TWFu");
    CHECK(Unbase64(" T W F u \n", &raw) && raw == std::vector<uint8_t>({'M','a','n'}));
    for (const char* invalid : {"A", "AAA", "====", "A===", "AA=A", "AB==", "AAB=", "TQ==A", "TQ==#", "A?=="})
        CHECK(!Unbase64(invalid, &raw));
    for (uint64_t bits : {1,31,32,33,63,64,65,95,96,97}) {
        Terrain t = SampleTerrain(bits), parsed;
        CHECK(EncodeTerrain(t, &bytes));
        CHECK(bytes.size() == 8 + 0x80 + 0x70 + 24 + 8 + bits + ((bits + 31) / 32) * 4);
        CHECK(std::memcmp(bytes.data(), "TPTG\1\0\0\0", 8) == 0);
        CHECK(Get<uint64_t>(bytes.data(), 8 + 0x78) == bits);
        for (size_t i = 8 + 0x80; i < 8 + 0x80 + 0x70; ++i) CHECK(bytes[i] == 0);
        CHECK(DecodeTerrain(bytes, &parsed));
        CHECK(parsed.bits == bits && parsed.height.data == t.height.data && parsed.material.data == t.material.data);
        std::vector<uint8_t> again; CHECK(EncodeTerrain(parsed, &again) && again == bytes);
        CHECK(Unbase64(Base64(bytes), &raw) && raw == bytes);
        for (size_t cut : {size_t(0), size_t(7), bytes.size() - 1}) {
            std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + cut);
            CHECK(!DecodeTerrain(truncated, &parsed));
        }
        bytes.push_back(0); CHECK(!DecodeTerrain(bytes, &parsed));
    }
    Terrain bad = SampleTerrain(); bad.height.rect[2] = -1; CHECK(!EncodeTerrain(bad, &bytes));
    bad = SampleTerrain(); bad.bits++; CHECK(!EncodeTerrain(bad, &bytes));
    Assets a; a.groups.resize(1); a.originalRemovals = 2; a.removals = {12};
    Model model; model.model = "tree.mdl"; model.extra = "tag"; for (int i = 0; i < 16; i += 5) model.matrix[i] = 1;
    a.groups[0].push_back(model);
    CHECK(EncodeAssets(a, &bytes)); CHECK(std::memcmp(bytes.data(), "TPAS\2\0\0\0\1\0\0\0\2\0\0\0", 16) == 0);
    Assets decoded; CHECK(DecodeAssets(bytes, &decoded));
    CHECK(decoded.groups[0][0].model == "tree.mdl" && decoded.originalRemovals == 2);
    CHECK(ParseAssetsFile("rm 12\n" + Base64(bytes), &decoded) && decoded.removals == std::vector<int32_t>{12});
    CHECK(ParseAssetsFile("rm -\n" + Base64(bytes), &decoded) && decoded.removals.empty());
    for (const char* prefix : {"rm \n", "rm 1,\n", "rm 0\n", "rm -1\n", "rm 2147483648\n", "rm 1,2,3\n"})
        CHECK(!ParseAssetsFile(std::string(prefix) + Base64(bytes), &decoded));
    bytes.push_back(0); CHECK(!DecodeAssets(bytes, &decoded));
    a.groups[0][0].model.assign(70000, 'm');
    a.originalRemovals = 5000;
    CHECK(EncodeAssets(a, &bytes) && DecodeAssets(bytes, &decoded));
    CHECK(decoded.groups[0][0].model.size() == 70000 && decoded.originalRemovals == 5000);
    auto old = bytes; old[4] = 1; CHECK(!DecodeAssets(old, &decoded));
    for (size_t n : {size_t(16), size_t(20), size_t(24), bytes.size()-1}) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin()+n);
        CHECK(!DecodeAssets(truncated, &decoded));
    }
    a.groups[0][0].model = "a\0b"s; CHECK(!EncodeAssets(a, &bytes));
}

static std::set<void*> allocations;
static int failAfter = -1;
void* Alloc(size_t n)
{
    if (failAfter == 0) return nullptr;
    if (failAfter > 0) --failAfter;
    void* p = ::operator new(n, std::nothrow);
    if (p) allocations.insert(p);
    return p;
}
void Free(void* p) { CHECK(allocations.erase(p) == 1); ::operator delete(p); }
void FakeCtor(void* p)
{
    std::memset(p, 0, 0x8f0);
    // Only test-relevant defaults; optional game ELF exercises the complete ctor.
    for (size_t off : {size_t(0), size_t(0x8c0)}) {
        char* text = static_cast<char*>(p) + off + 16; Put(p, off, text);
    }
    for (int i = 0; i < 16; i += 5) Put(p, 0x738 + i * 4, 1.0f);
}

struct MergeFixture {
    Bytes<0x3c0> proposal{};
    std::vector<Bytes<24>> nodes{3};
    std::vector<Bytes<120>> edges, removed;
    std::vector<Bytes<0x8f0>> construction{1};
    std::vector<int32_t> frozen{1}, ceFrozen{1};
    std::vector<Bytes<32>> tags;
    explicit MergeFixture(bool endpoint) : edges(endpoint ? 2 : 3), removed(endpoint ? 0 : 1) {
        for (int i = 0; i < 3; ++i) {
            Put(nodes[i].data(), 0, i == 2 || (i == 0 && !endpoint) ? 10.0f : 0.0f);
            Put(nodes[i].data(), 0x14, -1 - i);
            nodes[i][0xd] = 0x7f; // std padding is never interpreted as flags
        }
        for (size_t i = 0; i < edges.size(); ++i) {
            Put(edges[i].data(), 0, -10 - int32_t(i));
            Put(edges[i].data(), 0x10, 10.0f); Put(edges[i].data(), 0x1c, 10.0f);
            Put(edges[i].data(), 0x48, int32_t(1)); Put(edges[i].data(), 0x60, int32_t(29));
        }
        if (endpoint) { Put(edges[0].data(), 8, int32_t(-1)); Put(edges[0].data(), 12, int32_t(100)); }
        else {
            Put(edges[0].data(), 8, int32_t(100)); Put(edges[0].data(), 12, int32_t(-1));
            Put(edges[1].data(), 8, int32_t(-1)); Put(edges[1].data(), 12, int32_t(101));
            Put(removed[0].data(), 0x48, int32_t(1)); Put(removed[0].data(), 0x60, int32_t(16));
            Put(removed[0].data(), 0x6c, uint32_t(1234));
        }
        auto& apron = edges.back(); Put(apron.data(), 8, int32_t(-2)); Put(apron.data(), 12, int32_t(-3));
        Put(apron.data(), 0x68, uint64_t(0x123456789abcdef0)); Put(apron.data(), 0x70, int32_t(3));
        apron[0x74] = 1; apron[0x75] = 0x7f;
        Put(construction[0].data(), 0x790, int32_t(edges.size() - 1));
        VectorAt(construction[0].data(), 0x778, ceFrozen);
        VectorAt(proposal.data(), 0, nodes); VectorAt(proposal.data(), 0x18, edges);
        VectorAt(proposal.data(), 0x48, removed); VectorAt(proposal.data(), 0x220, frozen);
        VectorAt(proposal.data(), 0x2a0, construction);
        Put(proposal.data(), 0x260, size_t(13)); // hash resize policy, not element count
    }
    uintptr_t Address() { return uintptr_t(proposal.data()); }
    void Objects(Bytes<120>& edge, std::initializer_list<uint64_t> values) {
        auto* data = static_cast<uint64_t*>(Alloc(values.size() * 8)); CHECK(data);
        std::copy(values.begin(), values.end(), data);
        Put(edge.data(), 0x30, data); Put(edge.data(), 0x38, data + values.size()); Put(edge.data(), 0x40, data + values.size());
    }
    void Tags(const std::string& old, const std::string& replacement) {
        tags.resize(2);
        for (size_t i = 0; i < 2; ++i) {
            const std::string& text = i ? replacement : old;
            char* data = text.size() <= 15 ? reinterpret_cast<char*>(tags[i].data() + 16) : static_cast<char*>(Alloc(text.size() + 1)); CHECK(data);
            std::memcpy(data, text.c_str(), text.size() + 1);
            Put(tags[i].data(), 0, data); Put(tags[i].data(), 8, text.size());
            if (text.size() > 15) Put(tags[i].data(), 16, text.size());
        }
        VectorAt(proposal.data(), 0x270, tags);
    }
    void DestroyTransferred() {
        SliceVec span{}; CHECK(SliceReadStdVector(Address() + 0x18, 120, 64, &span));
        for (size_t i = 0; i < span.count; ++i) { void* object = Get<void*>(edges[i].data(), 0x30); if (object) Free(object); }
        for (auto& edge : removed) { void* object = Get<void*>(edge.data(), 0x30); if (object) Free(object); }
        if (!tags.empty()) {
            CHECK(SliceReadStdVector(Address() + 0x270, 32, 64, &span));
            for (size_t i = 0; i < span.count; ++i) {
                void* text = Get<void*>(tags[i].data(), 0); if (text != tags[i].data() + 16) Free(text);
            }
        }
    }
};
void MergeTemplates()
{
    GameMemory memory{Alloc, Free, FakeCtor};
    for (const std::string& replacement : {std::string("apron"), std::string("a deliberately long construction apron tag")}) {
        MergeFixture f(true);
        f.Objects(f.edges[0], {11}); f.Objects(f.edges[1], {22,33});
        const auto* moved = Get<void*>(f.edges[1].data(), 0x30);
        f.Tags("a deliberately long old tag", replacement);
        CHECK(SliceMergeTemplateStreet(f.Address(), &memory));
        SliceVec span; CHECK(SliceReadStdVector(f.Address(), 24, 64, &span) && span.count == 1);
        CHECK(SliceReadStdVector(f.Address() + 0x18, 120, 64, &span) && span.count == 1);
        CHECK(Get<void*>(f.edges[0].data(), 0x30) == moved && Get<void*>(f.edges[1].data(), 0x30) == nullptr);
        CHECK(f.frozen[0] == 0 && f.ceFrozen[0] == 0 && Get<int32_t>(f.construction[0].data(), 0x790) == 0);
        CHECK(f.edges[0][0x74] == 1 && Get<uint64_t>(f.edges[0].data(), 0x68) == 0x123456789abcdef0);
        CHECK(f.nodes[0][0xc] == 0 && f.nodes[0][0xd] == 0x7f);
        char tag[128]; size_t len; CHECK(SliceReadStdString(uintptr_t(f.tags[0].data()), tag, sizeof(tag), &len));
        CHECK(std::string(tag, len) == replacement);
        CHECK(Get<void*>(f.tags[1].data(), 0) == f.tags[1].data() + 16 && Get<size_t>(f.tags[1].data(), 8) == 0);
        f.DestroyTransferred(); CHECK(allocations.empty());
    }
    // Every clone allocation failure leaves all original objects and links intact.
    for (int fail = 0; fail < 2; ++fail) {
        MergeFixture f(false); f.Objects(f.removed[0], {22,33});
        auto original = f.proposal; auto oldEdges = f.edges; auto oldNodes = f.nodes;
        failAfter = fail;
        CHECK(!SliceMergeTemplateStreet(f.Address(), &memory));
        CHECK(f.proposal == original && f.edges == oldEdges && f.nodes == oldNodes && allocations.size() == 1);
        failAfter = -1; f.DestroyTransferred(); CHECK(allocations.empty());
    }
    {
        MergeFixture f(false); f.Objects(f.removed[0], {22,33});
        f.Objects(f.edges[0], {44}); f.Objects(f.edges[1], {55});
        CHECK(SliceMergeTemplateStreet(f.Address(), &memory));
        SliceVec span; CHECK(SliceReadStdVector(f.Address(), 24, 64, &span) && span.count == 2);
        CHECK(Get<int32_t>(f.edges[2].data(), 12) == -1 && Get<float>(f.edges[2].data(), 0x10) == 10);
        CHECK(Get<int32_t>(f.edges[0].data(), 0x60) == 16 && Get<uint32_t>(f.edges[1].data(), 0x6c) == 1234);
        const auto* a = Get<uint64_t*>(f.edges[0].data(), 0x30); const auto* b = Get<uint64_t*>(f.edges[1].data(), 0x30);
        CHECK(a != b && a[0] == 22 && b[1] == 33 && a != Get<uint64_t*>(f.removed[0].data(), 0x30));
        CHECK(f.ceFrozen[0] == 1 && Get<int32_t>(f.construction[0].data(), 0x790) == 2);
        f.DestroyTransferred(); CHECK(allocations.empty());
    }
    {
        MergeFixture f(false); f.Objects(f.removed[0], {22});
        auto original = f.proposal; auto oldEdges = f.edges;
        GameMemory absent{};
        CHECK(!SliceMergeTemplateStreet(f.Address(), &absent));
        CHECK(f.proposal == original && f.edges == oldEdges && allocations.size() == 1);
        f.DestroyTransferred(); CHECK(allocations.empty());
    }
    {
        MergeFixture f(true); auto oldNodes = f.nodes; auto oldEdges = f.edges;
        void* readonly = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(readonly != MAP_FAILED);
        std::memcpy(readonly, f.proposal.data(), f.proposal.size()); CHECK(mprotect(readonly, 4096, PROT_READ) == 0);
        CHECK(!SliceMergeTemplateStreet(uintptr_t(readonly), &memory));
        CHECK(f.nodes == oldNodes && f.edges == oldEdges && allocations.empty());
        CHECK(munmap(readonly, 4096) == 0);
    }
    for (int bad = 0; bad < 5; ++bad) {
        MergeFixture f(true);
        if (bad == 0) f.ceFrozen[0] = 2;
        if (bad == 1) Put(f.edges[0].data(), 12, int32_t(-3));
        if (bad == 2) Put(f.proposal.data(), 0x250, size_t(1));
        if (bad == 3) Put(f.proposal.data(), 0x248, uintptr_t(1));
        if (bad == 4) { Put(f.edges[0].data(), 0x30, f.edges.data()); Put(f.edges[0].data(), 0x38, f.edges.data()); Put(f.edges[0].data(), 0x40, f.edges.data()); }
        auto original = f.proposal; auto oldEdges = f.edges; auto oldNodes = f.nodes;
        CHECK(!SliceMergeTemplateStreet(f.Address(), &memory));
        CHECK(f.proposal == original && f.edges == oldEdges && f.nodes == oldNodes && allocations.empty());
    }
}

void* elfImage = nullptr; size_t elfSize = 0;
void* runtimeHandle = nullptr;
void (*runtimeDelete)(void*) = nullptr;
void (*CtorFromElf(const char* path))(void*)
{
    std::ifstream file(path, std::ios::binary); CHECK(file.good());
    Elf64_Ehdr eh{}; file.read(reinterpret_cast<char*>(&eh), sizeof(eh));
    CHECK(!std::memcmp(eh.e_ident, ELFMAG, SELFMAG) && eh.e_phentsize == sizeof(Elf64_Phdr));
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    file.seekg(eh.e_phoff); file.read(reinterpret_cast<char*>(ph.data()), ph.size() * sizeof(Elf64_Phdr));
    for (auto& p : ph) if (p.p_type == PT_LOAD) elfSize = std::max(elfSize, size_t(p.p_vaddr + p.p_memsz));
    elfSize = (elfSize + 4095) & ~size_t(4095);
    elfImage = mmap(nullptr, elfSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(elfImage != MAP_FAILED);
    for (auto& p : ph) if (p.p_type == PT_LOAD) {
        file.seekg(p.p_offset); file.read(static_cast<char*>(elfImage) + p.p_vaddr, p.p_filesz); CHECK(file.good());
    }
    auto* ctor = static_cast<uint8_t*>(elfImage) + 0xdbec10;
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < 2233; ++i) { hash ^= ctor[i]; hash *= 1099511628211ull; }
    CHECK(hash == 0xf1af220929f4f9d0ull);
    // Resolve the same allocator pair the game uses; enable the real init gate.
    runtimeHandle = dlopen("libstdc++.so.6", RTLD_NOW | RTLD_LOCAL); CHECK(runtimeHandle);
    // Prefer the process bindings (ASan deliberately interposes both). Static
    // CMake builds have neither binding until the explicit runtime is loaded.
    auto* gameNew = dlsym(RTLD_DEFAULT, "_Znwm");
    auto* gameDelete = dlsym(RTLD_DEFAULT, "_ZdlPv");
    if (!gameNew || !gameDelete) {
        gameNew = dlsym(runtimeHandle, "_Znwm"); gameDelete = dlsym(runtimeHandle, "_ZdlPv");
    }
    CHECK(gameNew && gameDelete);
    runtimeDelete = reinterpret_cast<void (*)(void*)>(gameDelete);
    Put(elfImage, 0x5a46cf0, gameNew);
    Put(elfImage, 0x5a46ce8, gameDelete);
    CHECK(mprotect(elfImage, elfSize, PROT_READ | PROT_EXEC) == 0);
    return reinterpret_cast<void (*)(void*)>(ctor);
}
void Installation(void (*ctor)(void*))
{
    GameMemory mem{Alloc, Free, ctor};
    Terrain t = SampleTerrain();
    for (int fail = 0; fail < 3; ++fail) {
        Bytes<0x3c0> p{}, original = p; failAfter = fail;
        CHECK(!InstallTerrain(uintptr_t(p.data()), t, mem)); CHECK(p == original && allocations.empty());
    }
    failAfter = -1;
    Bytes<0x3c0> p{};
    CHECK(InstallTerrain(uintptr_t(p.data()), t, mem)); CHECK(allocations.size() == 3);
    CHECK(Get<uint32_t>(p.data(), 0x3b0) == 1);
    auto* mask = Get<uint64_t*>(p.data(), 0x398); CHECK(mask[0] == UINT64_MAX && mask[1] == 1);
    Terrain reread; CHECK(ReadTerrain(uintptr_t(p.data()), &reread)); CHECK(reread.bits == 65);
    CHECK(!InstallTerrain(uintptr_t(p.data()), t, mem));
    for (size_t offset : {size_t(0x348), size_t(0x370), size_t(0x398)}) Free(Get<void*>(p.data(), offset));
    p = {};
    Assets a; a.originalRemovals = 1; a.removals = {99};
    Model m; m.model = "vegetation/trees/long_filename.mdl"; m.extra = "name"; m.matrix[15] = 1;
    a.groups = {{m, m}, {m}};
    // Fail EVERY allocation position: no carrier mutation, no leaked game data.
    int successAt = 0;
    for (int fail = 0; fail < 30; ++fail) {
        p = {}; auto before = p; failAfter = fail;
        if (InstallAssets(uintptr_t(p.data()), a, mem)) { successAt = fail; break; }
        CHECK(p == before && allocations.empty());
    }
    CHECK(successAt > 5); failAfter = -1;
    Assets rereadAssets; CHECK(ReadAssets(uintptr_t(p.data()), &rereadAssets));
    CHECK(rereadAssets.groups.size() == 2 && rereadAssets.groups[0].size() == 2 && rereadAssets.removals[0] == 99);
    CHECK(rereadAssets.groups[1][0].model == m.model);
    auto* ces = Get<uint8_t*>(p.data(), 0x2a0);
    struct TM { std::string first, second; float mat[16]; }; static_assert(sizeof(TM) == 0x80);
    for (size_t i = 0; i < a.groups.size(); ++i) {
        auto* ce = ces + i * 0x8f0;
        CHECK(Get<int32_t>(ce, 0x20) == 11 && ce[0x1fd] == 1);
        CHECK(Get<float>(ce, 0x738) == 1 && Get<float>(ce, 0x774) == 1);
        void* record = Get<void*>(ce, 0x578);
        CHECK(Get<float>(record, 0x38) == 0.75f && Get<float>(record, 0x3c) == 2.5f); Free(record);
        auto* models = Get<uint8_t*>(ce, 0x478);
        // Hand the transferred strings to REAL libstdc++ vector/string
        // destructors, proving the manually constructed ABI frees correctly.
        for (size_t j = 0; j < a.groups[i].size(); ++j)
            CHECK(allocations.erase(Get<void*>(models + j * 0x80, 0)) == 1);
        CHECK(allocations.erase(models) == 1);
        std::vector<TM> owned; std::memcpy(&owned, ce + 0x478, sizeof(owned));
        CHECK(owned[0].first == m.model && owned[0].second == "name");
    }
    Free(ces); Free(Get<void*>(p.data(), 0x288)); CHECK(allocations.empty());
    void* readonly = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(readonly != MAP_FAILED);
    CHECK(!InstallTerrain(uintptr_t(readonly), t, mem)); CHECK(!InstallAssets(uintptr_t(readonly), a, mem));
    CHECK(allocations.empty()); munmap(readonly, 4096);
    CHECK(!InstallAssets(1, a, mem));
}

extern const SliceAreaEntry __start_tpf2mp_slice_areas[];
extern const SliceAreaEntry __stop_tpf2mp_slice_areas[];
std::string ReadFile(const std::string& path) { std::ifstream f(path); return {std::istreambuf_iterator<char>(f), {}}; }
void PutFile(const std::string& path, const std::string& text) { std::ofstream f(path); f << text; CHECK(f.good()); }
template<class Handler> void CarrierConsumption(Handler handler, SliceFactoryCall call, const std::string& dir)
{
    call.retRva = 0x1971333; call.script = true;
    for (const char* filename : {"terrain_inject_A.bin", "asset_inject_A.txt"}) {
        const std::string path = dir + filename;
        const bool terrain = filename[0] == 't';
        const std::string malformed = terrain ? "not-base64" : "rm -\nAAAA";
        Bytes<0x3c0> proposal{};
        std::vector<Bytes<24>> nodes(1);
        VectorAt(proposal.data(), 0, nodes);
        call.rdx = uintptr_t(proposal.data());
        PutFile(path, malformed);
        const auto nonempty = proposal;
        handler(call, nullptr);
        CHECK(proposal == nonempty && ReadFile(path) == malformed);
        proposal = {};
        handler(call, nullptr);
        CHECK(access(path.c_str(), F_OK) != 0 && CarrierEmpty(call.rdx));
        // Unchanged .22 Lua has no retry protocol: a read but rejected payload
        // must never fill a later unrelated empty command.
        handler(call, nullptr);
        CHECK(CarrierEmpty(call.rdx));

        std::vector<uint8_t> raw;
        if (terrain) CHECK(EncodeTerrain(SampleTerrain(), &raw));
        else {
            Assets assets; assets.originalRemovals = 1;
            CHECK(EncodeAssets(assets, &raw));
        }
        PutFile(path, (terrain ? "" : "rm 77\n") + Base64(raw));
        void* readonly = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(readonly != MAP_FAILED);
        call.rdx = uintptr_t(readonly);
        handler(call, nullptr); // Valid wire data, rejected installation.
        CHECK(access(path.c_str(), F_OK) != 0 && CarrierEmpty(call.rdx));
        CHECK(munmap(readonly, 4096) == 0);
        call.rdx = uintptr_t(proposal.data());
        handler(call, nullptr);
        CHECK(CarrierEmpty(call.rdx));
    }
}
void Integration()
{
    char temp[] = "/tmp/tpf2mp-terrain-test-XXXXXX"; CHECK(mkdtemp(temp));
    std::string dir = std::string(temp) + "/";
    SliceCoreEnv env{}; env.base = uintptr_t(elfImage); env.dataDir = dir.c_str(); env.rootDir = "";
    CHECK(SliceCoreOpen(env) == SliceOpenResult::Ok);
    PutFile(dir + "tpf2_instance.txt", "A\npid=" + std::to_string(getpid()) + "\n");
    PutFile(dir + "lockstep_status_A.txt", "t=10  peer=1\n");
    for (auto* a = __start_tpf2mp_slice_areas; a != __stop_tpf2mp_slice_areas; ++a) a->registerFn();
    CHECK(handlers.count("slice-construction") == 1 && handlers.count("slice-terrain-assets") == 1);
    auto ch = handlers["slice-construction"].onEntry;
    auto th = handlers["slice-terrain-assets"].onEntry;
    SliceFactoryCall c{}; c.factory = SliceFactoryByTag(15); c.tid = SliceTid(); c.rdi = 1;
    c.retRva = 0xe34860; ch(c, nullptr);
    CHECK(ReadFile(dir + "lockstep_inject_A.txt").empty());

    std::string seed = "seed", filename = "station.con"; Table table; table.emplace(String(seed), Number(42));
    Bytes<0x3c0> p{}; std::vector<Bytes<0x8f0>> ces(1); std::vector<int32_t> rm{55};
    std::memcpy(ces[0].data(), &filename, sizeof(filename));
    auto* params = new (ces[0].data() + 0x448) Table(table);
    VectorAt(p.data(), 0x288, rm); VectorAt(p.data(), 0x2a0, ces);
    canArm = true; c.retRva = 0xf229a5; c.rdx = uintptr_t(p.data());
    PutFile(dir + "lockstep_inject_A.txt", ""); ch(c, nullptr);
    CHECK(armed && arm.done == SliceDone::IfPresent && !arm.honourArmed && arm.prepareCancel);
    CHECK(ReadFile(dir + "lockstep_inject_A.txt").empty());
    CHECK(arm.prepareCancel({}, nullptr));
    std::string shipped = ReadFile(dir + "lockstep_inject_A.txt"); CHECK(shipped.find("ARMED 1\nCONUP 55 station.con ") == 0);
    arm.landed(nullptr, SliceOutcome::CancelledNotFired, nullptr); armed = false;
    CHECK(ReadFile(dir + "lockstep_inject_A.txt") == shipped);
    ch(c, nullptr); CHECK(armed); arm.landed(nullptr, SliceOutcome::RanNatively, nullptr); armed = false;
    CHECK(ReadFile(dir + "lockstep_inject_A.txt") == shipped);
    // A module edit can regenerate internal tracks; .22 CONUP still carries
    // the full new params and each UI callback remains required.
    std::vector<Bytes<24>> internalNodes(3); VectorAt(p.data(), 0, internalNodes);
    for (uintptr_t caller : {uintptr_t(0xe4f6bd), uintptr_t(0xdd4e99)}) {
        c.retRva = caller; ch(c, nullptr);
        CHECK(armed && arm.done == SliceDone::Required && arm.prepareCancel({}, nullptr));
        arm.landed(nullptr, SliceOutcome::CancelledFired, nullptr); armed = false;
    }
    internalNodes.clear(); VectorAt(p.data(), 0, internalNodes);
    rm.clear(); VectorAt(p.data(), 0x288, rm);
    std::string emptyName; std::memcpy(ces[0].data() + 0x8c0, &emptyName, sizeof(emptyName));
    c.retRva = 0xe34860; ch(c, nullptr);
    CHECK(armed && arm.done == SliceDone::Required && arm.prepareCancel({}, nullptr));
    arm.landed(nullptr, SliceOutcome::CancelledFired, nullptr); armed = false;
    CHECK(ReadFile(dir + "lockstep_inject_A.txt").find("ARMED 1\nCONXP station.con ") != std::string::npos);
    // Connected placement writes its street and params together, behind one
    // ARMED line. The proposal area never writes a separate early companion.
    MergeFixture connected(false);
    VectorAt(p.data(), 0, connected.nodes); VectorAt(p.data(), 0x18, connected.edges);
    PutFile(dir + "lockstep_inject_A.txt", ""); ch(c, nullptr);
    CHECK(armed && arm.prepareCancel({}, nullptr));
    const std::string paired = ReadFile(dir + "lockstep_inject_A.txt");
    CHECK(paired.find("ARMED 1\nROADC ") == 0 && paired.find("\nCONXP station.con ") != std::string::npos);
    CHECK(paired.find("ARMED ", 1) == std::string::npos);
    arm.landed(nullptr, SliceOutcome::CancelledFired, nullptr); armed = false;
    // An invalid street header must not be misclassified as free-standing.
    Put(p.data(), 0x18, uintptr_t(1)); c.rdx = uintptr_t(p.data()); ch(c, nullptr); CHECK(!armed);
    params->~Table();

    // Exercise the real initialized dynamic nothrow allocator and actual game
    // ctor through the Lua file-carrier route, with no injected memory callbacks.
    p = {}; Terrain terrain = SampleTerrain(); std::vector<uint8_t> raw; CHECK(EncodeTerrain(terrain, &raw));
    PutFile(dir + "terrain_inject_A.bin", Base64(raw));
    c.retRva = 0x1971333; c.rdx = uintptr_t(p.data()); c.script = true;
    th(c, nullptr); CHECK(access((dir + "terrain_inject_A.bin").c_str(), F_OK) != 0);
    Terrain result; CHECK(ReadTerrain(uintptr_t(p.data()), &result) && result.bits == terrain.bits);
    // Capture commits before cancellation; Hold only retains the tool while its
    // destructor observer is installed. Marker release is confined to the UI tid.
    c.retRva = 0xe59622; c.script = false; th(c, nullptr);
    CHECK(armed && arm.done == SliceDone::Required && arm.prepareCancel && !arm.honourArmed);
    Bytes<0x140> tool{}; Put(tool.data(), 0, uintptr_t(elfImage) + 0x5a0bcc0);
    Bytes<32> callback{}; Put(callback.data(), 0, uintptr_t(tool.data()));
    Put(callback.data(), 0x10, uintptr_t(elfImage) + 0xe57670);
    Put(callback.data(), 0x18, uintptr_t(elfImage) + 0xe593a0);
    SliceAddCall add{}; add.done = callback.data(); add.tid = SliceTid();
    CHECK(arm.prepareCancel(add, nullptr));
    arm.landed(&add, SliceOutcome::CancelledFired, nullptr); armed = false;
    CHECK(tool[0xd0] == 1);
    Bytes<0x3c0> empty{}; SliceFactoryCall marker = c; marker.retRva = 0x1971333; marker.rdx = uintptr_t(empty.data()); marker.script = true;
    std::thread other([&] { th(marker, nullptr); SliceTerrainPollHeldTools(); }); other.join();
    CHECK(tool[0xd0] == 1); SliceTerrainPollHeldTools(); CHECK(tool[0xd0] == 0);
    th(c, nullptr); CHECK(armed && arm.prepareCancel(add, nullptr));
    arm.landed(&add, SliceOutcome::CancelledFired, nullptr); armed = false;
    CHECK(tool[0xd0] == 1 && dtorHook.detour);
    reinterpret_cast<void (*)(void*)>(dtorHook.detour)(tool.data());
    CHECK(tool[0xd0] == 0x7f);
    // Reuse exactly the old address AND vtable. The stale hold must be gone.
    th(marker, nullptr); SliceTerrainPollHeldTools(); CHECK(tool[0xd0] == 0x7f);
    // Destruction INSIDE the completion callback happens between prepare and
    // landed. An identical replacement object must not acquire the old hold.
    th(c, nullptr); CHECK(armed && arm.prepareCancel(add, nullptr));
    reinterpret_cast<void (*)(void*)>(dtorHook.detour)(tool.data());
    arm.landed(&add, SliceOutcome::CancelledFired, nullptr); armed = false;
    CHECK(tool[0xd0] == 0x7f);
    for (size_t offset : {size_t(0x348), size_t(0x370), size_t(0x398)}) runtimeDelete(Get<void*>(p.data(), offset));
    p = {}; Assets assets; assets.originalRemovals = 1; assets.removals = {77};
    Model model; model.model = "tree.mdl"; model.matrix[15] = 1; assets.groups = {{model}};
    CHECK(EncodeAssets(assets, &raw)); PutFile(dir + "asset_inject_A.txt", "rm 77\n" + Base64(raw));
    c.retRva = 0x1971333; c.script = true;
    th(c, nullptr); CHECK(access((dir + "asset_inject_A.txt").c_str(), F_OK) != 0);
    Assets resultAssets; CHECK(ReadAssets(uintptr_t(p.data()), &resultAssets) && resultAssets.removals[0] == 77);
    uint8_t* ce = Get<uint8_t*>(p.data(), 0x2a0);
    // Both fixture strings fit SSO; their only owners are these four blocks.
    runtimeDelete(Get<void*>(ce, 0x478)); runtimeDelete(Get<void*>(ce, 0x578));
    runtimeDelete(ce); runtimeDelete(Get<void*>(p.data(), 0x288));
    CarrierConsumption(th, c, dir);
    for (const char* name : {"tpf2_slice.log", "tpf2_slice.lock", "tpf2_instance.txt", "lockstep_status_A.txt", "lockstep_inject_A.txt"})
        CHECK(unlink((dir + name).c_str()) == 0);
    CHECK(rmdir(temp) == 0);
}

int main(int argc, char** argv)
{
    CHECK(SliceReadInit());
    Construction(); Codecs(); Installation(FakeCtor); MergeTemplates();
    if (argc > 1) { Installation(CtorFromElf(argv[1])); Integration(); munmap(elfImage, elfSize); dlclose(runtimeHandle); }
    puts("construction/terrain/assets: codecs, malformed inputs, guarded reads, ABI ownership and all allocation failures passed");
}
