// Execute the real TownSystem seed patch in a private image, with every GP/XMM
// register live. The original game files and running processes are untouched.
#include "../src/town_seed_linux.cpp"
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
    out.JumpTo(base + kTownSeedStoreRva);

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
    Write(base + kTownSeedResumeRva, jump.data(), jump.size());
    return reinterpret_cast<Fixture>(page);
}


static uint32_t NativeSeed(uint32_t time)
{
    return (time + UINT32_C(0x53a3cbac)) ^ UINT32_C(0x9e3779ce);
}

static void CheckRegisters(Fixture run, unsigned alignment, uint32_t time, unsigned variant)
{
    State input{}, output{};
    std::array<uint64_t, 15> words{};
    for (unsigned i = 0; i < words.size(); ++i)
        words[i] = UINT64_C(0x8123456700000000) + UINT64_C(0x010101011234567) * (i + variant);
    std::memcpy(&input, words.data(), sizeof(words));
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 16; ++j) input.xmm[i][j] = i * 17 + j + variant;
    input.flags = 0x202 | (variant & 1 ? 0x8d5 : 0);
    uint32_t store[3] = {0xabcdefff, 0xcccccccc, 0xfeedabba};
    input.rbp = reinterpret_cast<uintptr_t>(&store[1]) + 0xa00;
    input.rdx = UINT64_C(0xa1b2c3d400000000) | NativeSeed(time);
    State expected = input;
    expected.rdx = (input.rdx & UINT64_C(0xffffffff00000000)) | Tpf2mpWindowsTimeSeed(21, time);
    const unsigned originalMxcsr = _mm_getcsr();
    const unsigned mxcsr = originalMxcsr | 1u;
    _mm_setcsr(mxcsr);
    run(&input, &output);
    assert(_mm_getcsr() == mxcsr);
    _mm_setcsr(originalMxcsr);
    assert(std::memcmp(&expected, &output, 15 * sizeof(uint64_t)) == 0);
    assert((output.flags & 0x8d5) == (input.flags & 0x8d5));
    assert(std::memcmp(expected.xmm, output.xmm, sizeof(expected.xmm)) == 0);
    assert(output.rspBefore == output.rspAfter && output.rspBefore % 16 == alignment);
    assert(store[1] == uint32_t(expected.rdx));
    assert(store[0] == 0xabcdefff && store[2] == 0xfeedabba);
}

struct MT {
    uint32_t words[624]{};
    uint64_t index = 0;
};
static_assert(sizeof(MT) == 2504);
static_assert(offsetof(MT, index) == 0x9c0);
using Initialize = void (*)(MT*, uint32_t);

// Execute the actual native instructions from the return of GetTime through
// the entire inline initializer. Only the entry and final copy-out are fixture
// code. This tests sign-extension, inverse seed mapping, and the six-byte
// trampoline together, rather than reconstructing the patched instruction.
static Initialize MakeInitializer(uintptr_t base, unsigned char* page)
{
    Emitter out;
    out.Bytes({0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec}); out.U32(0xc60);
    out.Bytes({0x48, 0x89, 0xbd}); out.U32(uint32_t(-0xc60)); // output pointer
    out.Bytes({0x89, 0xf0}); // EAX = GetTime's supplied int32 bit pattern
    out.JumpTo(base + 0x1746822);
    const uintptr_t done = reinterpret_cast<uintptr_t>(page) + out.code.size();
    out.Bytes({0x48, 0x8b, 0xbd}); out.U32(uint32_t(-0xc60));
    out.Bytes({0x48, 0x8d, 0xb5}); out.U32(uint32_t(-0xa00));
    out.Bytes({0xb9}); out.U32(sizeof(MT) / 8);
    out.Bytes({0xfc, 0xf3, 0x48, 0xa5, 0xc9, 0xc3});
    Write(reinterpret_cast<uintptr_t>(page), out.code.data(), out.code.size());
    const auto jump = Jump(done);
    Write(base + kTownInitDoneRva, jump.data(), jump.size());
    // Undo only the test capture jump. The installed seed store stays patched.
    Write(base + kTownSeedResumeRva,
          kTownContextBytes + kTownSeedResumeRva - kTownContextRva, 14);
    return reinterpret_cast<Initialize>(page);
}

static MT ReferenceState(uint32_t seed)
{
    MT mt;
    mt.words[0] = seed;
    for (uint32_t i = 1; i < 624; ++i)
        mt.words[i] = UINT32_C(1812433253) * (mt.words[i - 1] ^ (mt.words[i - 1] >> 30)) + i;
    mt.index = 624;
    // Boost normalizes the unused low 31 bits of word zero. Its constructor
    // oracle already matches Windows; retain this check for the inline copy.
    const uint32_t p = mt.words[396] ^ mt.words[623];
    const uint32_t normalized = (p << 1) ^ (p & 0x80000000u ? 0x321161bfu : 0u);
    mt.words[0] = (seed & 0x80000000u) | (normalized & 0x7fffffffu);
    return mt;
}

static uint32_t Draw(MT& mt)
{
    if (mt.index == 624) {
        for (unsigned i = 0; i < 624; ++i) {
            const uint32_t mixed = (mt.words[i] & 0x80000000u) | (mt.words[(i + 1) % 624] & 0x7fffffffu);
            mt.words[i] = mt.words[(i + 397) % 624] ^ (mixed >> 1) ^ (mixed & 1 ? 0x9908b0dfu : 0u);
        }
        mt.index = 0;
    }
    uint32_t y = mt.words[mt.index++];
    y ^= y >> 11; y ^= (y << 7) & 0x9d2c5680u; y ^= (y << 15) & 0xefc60000u; y ^= y >> 18;
    return y;
}

int main()
{
    assert(!Tpf2mpInstallTownSeed(1, "unknown"));
    assert(!Tpf2mpInstallTownSeed(1, nullptr));
    assert(!Tpf2mpInstallTownSeed(0, kTownBuildId));
    constexpr size_t length = 0x174a000;
    void* memory = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
    std::memcpy(reinterpret_cast<void*>(base + kTownContextRva), kTownContextBytes, sizeof(kTownContextBytes));
    assert(mprotect(memory, length, PROT_READ | PROT_EXEC) == 0);
    for (uintptr_t rva : {kTownContextRva, uintptr_t(0x174681d), uintptr_t(0x1746831),
                         kTownSeedStoreRva, uintptr_t(0x174686b), uintptr_t(0x17468c5)}) {
        const unsigned char old = *reinterpret_cast<unsigned char*>(base + rva), changed = old ^ 1;
        Write(base + rva, &changed, 1);
        assert(!Tpf2mpInstallTownSeed(base, kTownBuildId));
        assert(!g_townInstalled && !Tpf2mpOriginalTownSeed);
        Write(base + rva, &old, 1);
        assert(!std::memcmp(reinterpret_cast<void*>(base + kTownContextRva), kTownContextBytes, sizeof(kTownContextBytes)));
    }
    assert(Tpf2mpInstallTownSeed(base, kTownBuildId));
    assert(g_townInstalled && Tpf2mpOriginalTownSeed);
    assert(!std::memcmp(Tpf2mpOriginalTownSeed,
                       kTownContextBytes + kTownSeedStoreRva - kTownContextRva, 6));

    auto* page = static_cast<unsigned char*>(mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(page != MAP_FAILED);
    const uint32_t boundaries[] = {0, 1, 2, 744000, 750000, 750200, 756000, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff};
    unsigned registerCases = 0;
    for (unsigned alignment : {0u, 8u}) {
        Fixture run = MakeFixture(base, alignment, page);
        for (uint32_t time : boundaries) for (unsigned variant : {0u, 1u}) {
            CheckRegisters(run, alignment, time, variant); ++registerCases;
        }
    }

    // Fixed original-Windows machine-code oracle values (tag21/time).
    const std::array<std::array<uint32_t, 2>, 5> oracle{{
        {{0,0xbd101601}}, {{744000,0x6557472e}}, {{750000,0x1b1051a5}},
        {{750200,0xb4c13bbe}}, {{756000,0x3e2c108d}}
    }};
    for (const auto& pair : oracle) assert(WindowsTownSeed(NativeSeed(pair[0])) == pair[1]);
    Initialize initialize = MakeInitializer(base, page);
    std::vector<uint32_t> times(std::begin(boundaries), std::end(boundaries));
    std::mt19937 fixtureRng(218891);
    for (unsigned i = 0; i < 1024; ++i) times.push_back(fixtureRng());
    unsigned draws = 0;
    for (uint32_t time : times) {
        const uint32_t seed = Tpf2mpWindowsTimeSeed(21, time);
        MT actual;
        initialize(&actual, time);
        const MT expected = ReferenceState(seed);
        assert(std::memcmp(&actual, &expected, sizeof(MT)) == 0);
        std::mt19937 standard(seed);
        for (unsigned i = 0; i < 625; ++i) { assert(Draw(actual) == standard()); ++draws; }
    }
    assert(!Tpf2mpInstallTownSeed(base, kTownBuildId));
    assert(std::strstr(Tpf2mpTownSeedStatus(), "already installed") != nullptr);
    assert(munmap(page, 4096) == 0 && munmap(memory, length) == 0);
    std::printf("Town seed: %u full-register cases; %zu original inline MT initializations and %u standard MT outputs passed\n",
                registerCases, times.size(), draws);
}
