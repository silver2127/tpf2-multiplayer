// Executes the real inline jump and original native MT draw/twist privately.
#include "../src/codewrite_linux.h"
static unsigned treeWriteMode = 0;
static unsigned treeWriteCalls = 0;
static unsigned treeWriteFailAt = 1;
static int TreeTestWrite(uintptr_t address, const uint8_t* bytes, size_t size, int* error)
{
    ++treeWriteCalls;
    if (treeWriteMode == 1 && treeWriteCalls == treeWriteFailAt) { treeWriteMode = 0; return TPF2MP_CW_UNAVAILABLE; }
    if (treeWriteMode == 2 && treeWriteCalls == treeWriteFailAt) {
        treeWriteMode = 0;
        const int result = Tpf2mpCodeWriteSelf(address, bytes, size / 2, error);
        return result == TPF2MP_CW_OK ? TPF2MP_CW_READBACK : result;
    }
    return Tpf2mpCodeWriteSelf(address, bytes, size, error);
}
#define Tpf2mpCodeWriteSelf TreeTestWrite
#include "../src/tree_rng_linux.cpp"
#undef Tpf2mpCodeWriteSelf
#include <random>
#include <cmath>
#include <sys/mman.h>
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
                           unsigned char* page, uintptr_t entry = kTreeEntryRva,
                           uintptr_t resume = kTreeResumeRva)
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
    out.JumpTo(base + entry);

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
    Write(base + resume, jump.data(), jump.size());
    return reinterpret_cast<Fixture>(page);
}

struct MT {
    uint32_t words[624]{};
    uint64_t index = 0;
};
static_assert(sizeof(MT) == 2504);
static_assert(offsetof(MT, index) == 0x9c0);
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

static uint32_t ReferenceIndex(MT& mt, uint32_t count)
{
    if (!count) return Draw(mt);
    if (count == 1) return 0;
    const uint64_t limit = ((uint64_t{1} << 32) / count) * count;
    uint32_t raw;
    do raw = Draw(mt); while (raw >= limit);
    return raw % count;
}

static uint32_t Untemper(uint32_t word)
{
    uint32_t x = word;
    for (unsigned i = 0; i < 6; ++i) x = word ^ (x >> 18);
    word = x;
    for (unsigned i = 0; i < 6; ++i) x = word ^ ((x << 15) & 0xefc60000u);
    word = x;
    for (unsigned i = 0; i < 6; ++i) x = word ^ ((x << 7) & 0x9d2c5680u);
    word = x;
    for (unsigned i = 0; i < 6; ++i) x = word ^ (x >> 11);
    return x;
}

static void CheckRegisters(Fixture run, unsigned alignment, uint32_t count,
                           MT& actual, unsigned variant)
{
    State input{}, output{};
    std::array<uint64_t, 15> words{};
    for (unsigned i = 0; i < words.size(); ++i)
        words[i] = UINT64_C(0x8123456700000000) + UINT64_C(0x010101011234567) * (i + variant);
    std::memcpy(&input, words.data(), sizeof(words));
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 16; ++j) input.xmm[i][j] = i * 17 + j + variant;
    input.flags = 0x202 | (variant & 1 ? 0x8d5 : 0);
    uint64_t vectorSlot[] = {0xabcdefff, 0x012345678901abcd, 0xfeedabba};
    input.rbp = reinterpret_cast<uintptr_t>(&vectorSlot[1]) + 0x1830;
    input.r13 = reinterpret_cast<uintptr_t>(&actual);
    input.r12 = UINT64_C(0xa1b2c3d400000000) | count;
    State expected = input;
    MT expectedMT = actual;
    expected.rax = ReferenceIndex(expectedMT, count);
    expected.rsi = vectorSlot[1];
    const unsigned originalMxcsr = _mm_getcsr();
    const unsigned mxcsr = originalMxcsr | 1u;
    _mm_setcsr(mxcsr);
    run(&input, &output);
    assert(_mm_getcsr() == mxcsr);
    _mm_setcsr(originalMxcsr);
    assert(std::memcmp(&actual, &expectedMT, sizeof(MT)) == 0);
    assert(std::memcmp(&expected, &output, 15 * sizeof(uint64_t)) == 0);
    assert((output.flags & 0x8d5) == (input.flags & 0x8d5));
    assert(std::memcmp(expected.xmm, output.xmm, sizeof(expected.xmm)) == 0);
    assert(output.rspBefore == output.rspAfter && output.rspBefore % 16 == alignment);
    assert(vectorSlot[0] == 0xabcdefff && vectorSlot[1] == 0x012345678901abcd
           && vectorSlot[2] == 0xfeedabba);
}

static void TestSinCos(float angle, float* sine, float* cosine)
{
    *sine = std::sin(angle); *cosine = std::cos(angle);
}

static void CheckDraws(Fixture run, MT& actual)
{
    State input{}, output{};
    alignas(16) unsigned char frame[0x1900];
    std::memset(frame, 0xab, sizeof(frame));
    const uintptr_t rbp = reinterpret_cast<uintptr_t>(frame + 0x1880);
    input.rbp = rbp; input.r13 = reinterpret_cast<uintptr_t>(&actual);
    input.rbx = 0x123456789; input.r12 = 0xabcde; input.r14 = 0xf0123; input.r15 = 0x789ab;
    MT expectedMT = actual;
    const float scaleUnit = float(Draw(expectedMT)) * 0x1p-32f;
    const float angleUnit = float(Draw(expectedMT)) * 0x1p-32f;
    float angleMultiplier; std::memcpy(&angleMultiplier, &kTreeAngle, 4);
    const float angle = angleUnit * angleMultiplier + 0.0f;
    unsigned char expectedFrame[sizeof(frame)]; std::memcpy(expectedFrame, frame, sizeof(frame));
    const float cosine = std::cos(angle), sine = std::sin(angle);
    std::memcpy(expectedFrame + 0x1880 - 0x1848, &scaleUnit, 4);
    std::memcpy(expectedFrame + 0x1880 - 0x1860, &cosine, 4);
    std::memcpy(expectedFrame + 0x1880 - 0x185c, &sine, 4);
    run(&input, &output);
    assert(!std::memcmp(&actual, &expectedMT, sizeof(MT)));
    assert(!std::memcmp(expectedFrame, frame, sizeof(frame)));
    assert(!std::memcmp(output.xmm[0], &scaleUnit, 4));
    assert(output.rbp == input.rbp && output.rbx == input.rbx && output.r12 == input.r12
           && output.r14 == input.r14 && output.r15 == input.r15 && output.r13 == rbp - 0x1780);
    assert(output.rspBefore == output.rspAfter && output.rspBefore % 16 == 0);
}

int main()
{
    assert(!Tpf2mpInstallTreeRng(1, "unknown"));
    assert(!Tpf2mpInstallTreeRng(1, nullptr));
    assert(!Tpf2mpInstallTreeRng(0, kTreeBuildId));
    constexpr size_t length = 0x3e8e000;
    void* memory = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
    std::memcpy(reinterpret_cast<void*>(base + kTreeContextRva), kTreeContextBytes, sizeof(kTreeContextBytes));
    std::memcpy(reinterpret_cast<void*>(base + kTreeInclusiveRva), kTreeInclusiveBytes, sizeof(kTreeInclusiveBytes));
    std::memcpy(reinterpret_cast<void*>(base + kTreeTwistRva), kTreeTwistBytes, sizeof(kTreeTwistBytes));
    std::memcpy(reinterpret_cast<void*>(base + kTreeUnitRva), kTreeUnitBytes, sizeof(kTreeUnitBytes));
    std::memcpy(reinterpret_cast<void*>(base + kTreeAngleRva), &kTreeAngle, 4);
    std::memcpy(reinterpret_cast<void*>(base + kTreeUnitScaleRva), &kTreeUnitScale, 4);
    std::memcpy(reinterpret_cast<void*>(base + kTreeUnitOneRva), &kTreeUnitOne, 4);
    assert(mprotect(memory, length, PROT_READ | PROT_EXEC) == 0);
    for (uintptr_t rva : {kTreeContextRva, uintptr_t(0x15466de), kTreeEntryRva,
                         kTreeResumeRva, kTreeInclusiveRva, kTreeTwistRva, kTreeDrawsRva,
                         kTreeUnitRva, kTreeAngleRva, kTreeUnitScaleRva, kTreeUnitOneRva,
                         uintptr_t(0x31b6032)}) {
        const unsigned char old = *reinterpret_cast<unsigned char*>(base + rva), changed = old ^ 1;
        Write(base + rva, &changed, 1);
        assert(!Tpf2mpInstallTreeRng(base, kTreeBuildId));
        assert(!g_treeInstalled && !g_treeInclusive && !Tpf2mpTreeResume);
        Write(base + rva, &old, 1);
    }
    for (unsigned failAt : {1u, 2u}) for (unsigned mode : {1u, 2u}) {
        treeWriteFailAt = failAt;
        treeWriteMode = mode; treeWriteCalls = 0;
        assert(!Tpf2mpInstallTreeRng(base, kTreeBuildId));
        assert(treeWriteCalls == (failAt == 1 ? mode : mode + 2));
        assert(!g_treeInstalled && !g_treeInclusive && !Tpf2mpTreeResume);
        assert(std::strstr(Tpf2mpTreeRngStatus(), "original tree selection intact"));
        assert(!std::memcmp(reinterpret_cast<void*>(base + kTreeContextRva), kTreeContextBytes, sizeof(kTreeContextBytes)));
    }
    assert(Tpf2mpInstallTreeRng(base, kTreeBuildId));
    assert(g_treeInstalled && g_treeInclusive && Tpf2mpTreeResume);

    auto* page = static_cast<unsigned char*>(mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(page != MAP_FAILED);
    const uint32_t counts[] = {0, 1, 2, 3, 10, 16, 37, 256, 10000, 9999999,
                              0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff};
    unsigned registerCases = 0;
    for (unsigned alignment : {0u, 8u}) {
        Fixture run = MakeFixture(base, alignment, page);
        for (uint32_t count : counts) for (unsigned variant : {0u, 1u}) {
            MT engine = ReferenceState(5489 + variant);
            CheckRegisters(run, alignment, count, engine, variant); ++registerCases;
            // Controlled tail rejection followed by accepted values. The full
            // MT snapshot checks consumption, including singleton no-draw.
            engine.index = 0;
            const uint32_t raws[] = {0xffffffffu, 0xfffffffeu, 0x80000000u, 42, 0};
            for (unsigned i = 0; i < 624; ++i) engine.words[i] = Untemper(raws[i % 5]);
            CheckRegisters(run, alignment, count, engine, variant); ++registerCases;
        }
    }
    // Long sequences cross many actual original twist calls. Compare result,
    // all 624 state words and index against an independent reference each time.
    unsigned sequenceCases = 0;
    for (uint32_t seed : {0u, 1u, 5489u, 0x80000000u, 0xffffffffu}) {
        MT actual = ReferenceState(seed), expected = actual;
        std::mt19937 choices(seed ^ 8189u);
        for (unsigned i = 0; i < 5000; ++i) {
            const uint32_t count = i < std::size(counts) ? counts[i] : choices();
            assert(WindowsTreeIndex(&actual, count) == ReferenceIndex(expected, count));
            assert(!std::memcmp(&actual, &expected, sizeof(MT)));
            ++sequenceCases;
        }
    }
    // Fixed Windows witnesses from the original common distribution oracle.
    MT witness{}; witness.words[0] = Untemper(46662977);
    assert(WindowsTreeIndex(&witness, 10) == 7 && witness.index == 1);
    witness.index = 0;
    assert(WindowsTreeIndex(&witness, 9999999) == 6662981 && witness.index == 1);
    witness.index = 0;
    assert(WindowsTreeIndex(&witness, 1) == 0 && witness.index == 0);
    // Execute the actual installed local draw window, including the original
    // MT float core and twist. Check both outputs, all MT bytes, nonvolatile
    // registers, exact frame writes and stack balance. Forced 1.0 catches the
    // otherwise rare native endpoint clamp as well as swapped draw order.
    auto sincosJump = Jump(reinterpret_cast<uintptr_t>(TestSinCos));
    Write(base + 0x6dc480, sincosJump.data(), sincosJump.size());
    Fixture drawFixture = MakeFixture(base, 0, page, kTreeDrawsRva, kTreeDrawsEndRva);
    unsigned drawCases = 0;
    for (uint32_t seed : {0u, 1u, 5489u, 0xffffffffu}) {
        MT engine = ReferenceState(seed);
        for (unsigned i = 0; i < 1000; ++i) { CheckDraws(drawFixture, engine); ++drawCases; }
    }
    for (uint32_t first : {0u, 1u, 0x80000000u, 0xffffff80u, 0xffffffffu})
        for (uint32_t second : {0u, 1u, 0x80000000u, 0xffffff80u, 0xffffffffu}) {
            MT engine = ReferenceState(5489); engine.index = 0;
            engine.words[0] = Untemper(first); engine.words[1] = Untemper(second);
            CheckDraws(drawFixture, engine); ++drawCases;
        }
    MT crossing = ReferenceState(8189); crossing.index = 623;
    CheckDraws(drawFixture, crossing); ++drawCases;
    std::printf("Tree scale/angle: %u original-code order, endpoint, frame and MT-state cases passed\n", drawCases);
    assert(!Tpf2mpInstallTreeRng(base, kTreeBuildId));
    assert(std::strstr(Tpf2mpTreeRngStatus(), "already installed"));
    assert(munmap(page, 4096) == 0 && munmap(memory, length) == 0);
    std::printf("Tree RNG: %u full-register/forced-rejection cases and %u original-MT result/state sequences passed\n", registerCases, sequenceCases);
}
