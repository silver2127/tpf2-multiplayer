// Execute the real BuildingTypeRep registration patch in a private image, with every GP/XMM
// register live. The original game files and running processes are untouched.
#include "../src/building_order_linux.cpp"
#include "../src/codewrite_linux.h"
#include <random>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cassert>
#include <vector>
#include <immintrin.h>

struct State {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rdi, rsi, rdx, rcx, rax, flags;
    unsigned char xmm[16][16];
    uint64_t rspBefore, rspAfter;
};
static_assert(offsetof(State, flags) == 120);
static_assert(offsetof(State, xmm) == 128);
static_assert(offsetof(State, rspBefore) == 384);
static_assert(offsetof(State, rspAfter) == 392);

static void Write(uintptr_t address, const void* bytes, size_t size)
{
    int error = 0;
    assert(Tpf2mpCodeWriteSelf(address, static_cast<const uint8_t*>(bytes), size, &error) == TPF2MP_CW_OK);
}

static std::array<unsigned char, 14> Jump(uintptr_t target)
{
    std::array<unsigned char, 14> bytes{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(bytes.data() + 6, &target, sizeof(target));
    return bytes;
}

// Minimal machine-code fixture: load all supplied registers, jump through the
// installed patch, then capture registers before the fixture itself uses them.
// The fixture keeps its own output pointer and saved caller registers above
// the interrupted RSP, including both possible interrupted stack alignments.
struct Emitter {
    std::vector<unsigned char> code;
    void Bytes(std::initializer_list<unsigned char> bytes) { code.insert(code.end(), bytes); }
    void U32(uint32_t x) { for (unsigned i = 0; i < 4; ++i) code.push_back(x >> (i * 8)); }
    void JumpTo(uintptr_t target) { const auto bytes = Jump(target); code.insert(code.end(), bytes.begin(), bytes.end()); }
    void LoadGP(unsigned reg, uint32_t offset) {
        Bytes({static_cast<unsigned char>(0x49 | (reg >= 8 ? 4 : 0)), 0x8b,
               static_cast<unsigned char>(0x87 | ((reg & 7) << 3))});
        U32(offset); // mov reg,[r15+disp32]
    }
    void LoadXMM(unsigned reg, uint32_t offset) {
        Bytes({0xf3, static_cast<unsigned char>(reg >= 8 ? 0x45 : 0x41), 0x0f, 0x6f,
               static_cast<unsigned char>(0x87 | ((reg & 7) << 3))});
        U32(offset); // movdqu xmm,[r15+disp32]
    }
    void StoreXMM(unsigned reg, uint32_t offset) {
        Bytes({0xf3}); if (reg >= 8) Bytes({0x44});
        Bytes({0x0f, 0x7f, static_cast<unsigned char>(0x87 | ((reg & 7) << 3))});
        U32(offset); // movdqu [rdi+disp32],xmm
    }
};

using Fixture = void (*)(const State*, State*);

static Fixture MakeFixture(uintptr_t base, unsigned alignment,
                           unsigned char* page)
{
    Emitter out;
    // Entry RSP%16=8, six saved registers preserve that residue.
    const unsigned char frame = alignment == 0 ? 24 : 16;
    out.Bytes({0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
    out.Bytes({0x48, 0x83, 0xec, frame}); // sub rsp,frame
    out.Bytes({0x48, 0x89, 0x34, 0x24}); // mov [rsp],rsi (output)
    out.Bytes({0x48, 0x89, 0xa6}); out.U32(384); // mov [rsi+384],rsp
    out.Bytes({0x49, 0x89, 0xff}); // mov r15,rdi (input)
    for (unsigned i = 0; i < 16; ++i) out.LoadXMM(i, 128 + i * 16);
    // Hardware register numbers in the snapshot's memory order.
    const unsigned regs[] = {15, 14, 13, 12, 5, 3, 11, 10, 9, 8, 7, 6, 2, 1, 0};
    for (unsigned i = 1; i < 15; ++i) out.LoadGP(regs[i], i * 8);
    out.Bytes({0x41, 0xff, 0x77, 0x78, 0x9d}); // push [r15+120]; popfq
    out.LoadGP(15, 0); // release the final fixture pointer
    out.JumpTo(base + kBuildingPatchRva);

    const uintptr_t continuation = reinterpret_cast<uintptr_t>(page) + out.code.size();
    out.Bytes({0x9c, 0x50, 0x51, 0x52, 0x56, 0x57,
               0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x53, 0x55,
               0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
    out.Bytes({0x48, 0x8b, 0xbc, 0x24}); out.U32(128); // output at original RSP
    for (unsigned i = 0; i < 16; ++i) out.StoreXMM(i, 128 + i * 16);
    out.Bytes({0x48, 0x8d, 0x84, 0x24}); out.U32(128); // interrupted RSP
    out.Bytes({0x48, 0x89, 0x87}); out.U32(392); // output.rspAfter
    out.Bytes({0x48, 0x89, 0xe6, 0xb9, 0x10, 0, 0, 0, 0xfc, 0xf3, 0x48, 0xa5}); // copy GP/flags
    out.Bytes({0x48, 0x8d, 0xa4, 0x24}); out.U32(128 + frame);
    out.Bytes({0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d, 0x5b, 0xc3});
    assert(out.code.size() < 4096);
    Write(reinterpret_cast<uintptr_t>(page), out.code.data(), out.code.size());
    const auto jump = Jump(continuation);
    Write(base + kBuildingResumeRva, jump.data(), jump.size());
    return reinterpret_cast<Fixture>(page);
}

#include "data/building_order_fixture.h"

struct FakeResources {
    std::array<uintptr_t, 7> rep{};
    std::vector<std::array<unsigned char, 48>> records;
    std::vector<std::array<unsigned char, 0x200>> payloads;
    std::vector<Tpf2mpBuildingOrderKey> keys;
    explicit FakeResources(size_t count) : records(count), payloads(count), keys(count) {
        rep[5] = reinterpret_cast<uintptr_t>(records.data());
        rep[6] = rep[5] + records.size() * 48;
        for (size_t i = 0; i < count; ++i) {
            const auto pointer = reinterpret_cast<uintptr_t>(payloads[i].data());
            std::memcpy(records[i].data() + 0x20, &pointer, 8);
        }
    }
    void Key(size_t i, int32_t priority, int32_t year) {
        keys[i] = {priority, year};
        std::memcpy(payloads[i].data() + 0x1f8, &priority, 4);
        std::memcpy(payloads[i].data() + 0x28, &year, 4);
    }
};
static Tpf2mpBuildingOrderKey FixtureKey(void* context, int32_t id)
{ return static_cast<Tpf2mpBuildingOrderKey*>(context)[id]; }

static void CheckRegisters(Fixture run, unsigned alignment, unsigned count, unsigned variant)
{
    std::mt19937 rng(0x52340000 + count + variant);
    FakeResources resources(1200);
    std::vector<int32_t> ids(count);
    for (unsigned i = 0; i < count; ++i) {
        ids[i] = i;
        resources.Key(i, int32_t(rng() % 7) - 3, int32_t(rng() % 9) - 4);
    }
    std::shuffle(ids.begin(), ids.end(), rng);
    auto expectedIds = ids;
    if (count == 486) {
        ids.clear(); expectedIds.clear();
        for (const auto& row : kBuildingNativeCatalogue) {
            resources.Key(row.id, row.priority, row.year); ids.push_back(row.id);
        }
        expectedIds.assign(std::begin(kBuildingWindowsCatalogue), std::end(kBuildingWindowsCatalogue));
    } else Tpf2mpWindowsBuildingOrder(expectedIds.data(), count, resources.keys.data(), FixtureKey);
    const auto originalRecords = resources.records;
    const auto originalPayloads = resources.payloads;
    const auto originalRep = resources.rep;
    State input{}, output{};
    std::array<uint64_t, 15> words{};
    for (unsigned i = 0; i < words.size(); ++i)
        words[i] = UINT64_C(0x8123456700000000) + UINT64_C(0x010101011234567) * (i + variant);
    std::memcpy(&input, words.data(), sizeof(words));
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 16; ++j) input.xmm[i][j] = i * 17 + j + variant;
    input.flags = 0x202 | (variant & 1 ? 0x8d5 : 0);
    std::array<uint64_t, 64> frame;
    frame.fill(UINT64_C(0xacbdedfe01234567));
    input.rbp = reinterpret_cast<uintptr_t>(frame.data()) + 0x180;
    input.r12 = reinterpret_cast<uintptr_t>(resources.rep.data());
    const uintptr_t list[] = {reinterpret_cast<uintptr_t>(ids.data()),
        reinterpret_cast<uintptr_t>(ids.data()) + count * 4,
        reinterpret_cast<uintptr_t>(ids.data()) + ids.capacity() * 4};
    std::memcpy(reinterpret_cast<void*>(input.rbp - 0x130), list, sizeof(list));
    const auto originalFrame = frame;
    State expected = input;
    // The original seven-byte instruction loads RDI after our hook returns.
    std::memcpy(&expected.rdi, reinterpret_cast<void*>(input.rbp - 0x110), 8);
    const unsigned originalMxcsr = _mm_getcsr(), mxcsr = originalMxcsr | 1u;
    _mm_setcsr(mxcsr);
    run(&input, &output);
    assert(_mm_getcsr() == mxcsr); _mm_setcsr(originalMxcsr);
    assert(std::memcmp(&expected, &output, 15 * sizeof(uint64_t)) == 0);
    assert((output.flags & 0x8d5) == (input.flags & 0x8d5));
    assert(std::memcmp(expected.xmm, output.xmm, sizeof(expected.xmm)) == 0);
    assert(output.rspBefore == output.rspAfter && output.rspBefore % 16 == alignment);
    assert(ids == expectedIds);
    assert(resources.records == originalRecords && resources.payloads == originalPayloads);
    assert(resources.rep == originalRep && frame == originalFrame);
}

int main()
{
    assert(!Tpf2mpInstallBuildingOrder(1, "unknown"));
    assert(!Tpf2mpInstallBuildingOrder(1, nullptr));
    assert(!Tpf2mpInstallBuildingOrder(0, kBuildingBuildId));
    constexpr size_t length = 0x3312000;
    void* memory = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
    std::memcpy(reinterpret_cast<void*>(base + kBuildingConstructorRva), kBuildingConstructorBytes, sizeof(kBuildingConstructorBytes));
    std::memcpy(reinterpret_cast<void*>(base + kBuildingGetTypesRva), kBuildingGetTypesBytes, sizeof(kBuildingGetTypesBytes));
    std::memcpy(reinterpret_cast<void*>(base + kBuildingComparatorRva), kBuildingComparatorBytes, sizeof(kBuildingComparatorBytes));
    assert(mprotect(memory, length, PROT_READ | PROT_EXEC) == 0);
    for (uintptr_t rva : {kBuildingConstructorRva, uintptr_t(0x186821e), kBuildingPatchRva,
                         kBuildingGetTypesRva, uintptr_t(0x331081f), kBuildingComparatorRva, uintptr_t(0x330eef2)}) {
        const unsigned char old = *reinterpret_cast<unsigned char*>(base + rva), changed = old ^ 1;
        Write(base + rva, &changed, 1);
        assert(!Tpf2mpInstallBuildingOrder(base, kBuildingBuildId));
        assert(!g_buildingInstalled && !Tpf2mpOriginalBuildingOrder);
        Write(base + rva, &old, 1);
    }
    assert(Tpf2mpInstallBuildingOrder(base, kBuildingBuildId));
    assert(g_buildingInstalled && Tpf2mpOriginalBuildingOrder);
    assert(!std::memcmp(Tpf2mpOriginalBuildingOrder,
                       kBuildingConstructorBytes + kBuildingPatchRva - kBuildingConstructorRva, 7));
    auto* page = static_cast<unsigned char*>(mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(page != MAP_FAILED);
    unsigned registerCases = 0;
    for (unsigned alignment : {0u, 8u}) {
        Fixture run = MakeFixture(base, alignment, page);
        for (unsigned count : {0u, 1u, 2u, 31u, 32u, 33u, 40u, 41u, 42u, 128u, 486u, 1024u})
            for (unsigned variant : {0u, 1u}) { CheckRegisters(run, alignment, count, variant); ++registerCases; }
    }
    // Input-domain coverage for sparse mod IDs, signed/extreme priority/year
    // keys, duplicate keys and both introsort/heap paths. The separately saved
    // Windows machine-code oracle checks exact equal-key permutations too.
    std::mt19937 rng(0x38a5817b);
    unsigned cases = 0;
    for (unsigned n = 0; n <= 1024; ++n) {
        std::vector<int32_t> ids(n);
        std::vector<Tpf2mpBuildingOrderKey> keys(n * 3 + 1);
        for (unsigned i = 0; i < n; ++i) {
            ids[i] = i * 3;
            keys[ids[i]] = {int32_t(rng() % 11) - 5, int32_t(rng() % 17) - 8};
            if (i % 13 == 0) keys[ids[i]] = {INT32_MIN, INT32_MAX};
        }
        auto ascendingIds = ids;
        std::shuffle(ids.begin(), ids.end(), rng);
        Tpf2mpWindowsBuildingOrder(ids.data(), n, keys.data(), FixtureKey);
        for (unsigned i = 1; i < n; ++i) assert((!BuildingLess{keys.data(), FixtureKey}(ids[i], ids[i - 1])));
        auto permutation = ids; std::sort(permutation.begin(), permutation.end());
        assert(permutation == ascendingIds);
        if (n > 32) {
            std::shuffle(ids.begin(), ids.end(), rng);
            WindowsBuildingSort(ids.data(), ids.data() + n, 0, {keys.data(), FixtureKey});
            for (unsigned i = 1; i < n; ++i) assert((!BuildingLess{keys.data(), FixtureKey}(ids[i], ids[i - 1])));
            std::sort(ids.begin(), ids.end()); assert(ids == ascendingIds);
        }
        ++cases;
    }
    assert(!Tpf2mpInstallBuildingOrder(base, kBuildingBuildId));
    std::printf("building order: %u complete-register/metadata cases, %u sparse-key sort/heap cases, all 486 original Windows catalogue IDs match\n", registerCases, cases);
}
