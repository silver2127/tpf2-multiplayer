// Execute all eight real patch stubs in a private image, with every GP/XMM
// register live. The original game files and running processes are untouched.
#include "../src/person_cost_linux.cpp"
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
// the interrupted RSP, including the path_walk outgoing-argument alignment.
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

static Fixture MakeFixture(uintptr_t base, const PersonCostSite& site, unsigned alignment,
                           unsigned char* page)
{
    Emitter out;
    // Entry RSP%16=8, eight saved registers preserve that residue.
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
    out.JumpTo(base + site.patchRva);

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
    Write(base + site.resumeRva, jump.data(), jump.size());
    return reinterpret_cast<Fixture>(page);
}

static unsigned writerCalls;
static bool brokenRollback;
static unsigned failAt;
static int FailingWriter(uintptr_t address, const uint8_t* bytes, size_t size, int* error)
{
    ++writerCalls;
    if (writerCalls == failAt) {
        if (brokenRollback) {
            Write(address, bytes, 1); // simulate an unrecoverable partial write
            return TPF2MP_CW_PARTIAL_BROKEN;
        }
        return TPF2MP_CW_UNAVAILABLE;
    }
    if (brokenRollback && writerCalls > failAt) return TPF2MP_CW_UNAVAILABLE;
    return Tpf2mpCodeWriteSelf(address, bytes, size, error);
}

static void CheckRollback(uintptr_t base, bool broken, unsigned failure)
{
    const pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        writerCalls = 0; brokenRollback = broken; failAt = failure;
        assert(!InstallPersonCosts(base, kCostBuildId, FailingWriter));
        assert(writerCalls == 2 * failure); // attempted writes and their restorations
        if (broken) {
            assert(g_costActiveMask == ((1u << failure) - 1));
            assert(std::strstr(Tpf2mpPersonCostStatus(), "ERROR") != nullptr);
        } else {
            assert(g_costActiveMask == 0);
            assert(std::strstr(Tpf2mpPersonCostStatus(), "all attempted windows restored") != nullptr);
            for (const auto& site : kPersonCostSites)
                assert(std::memcmp(reinterpret_cast<void*>(base + site.contextRva), site.context, site.contextSize) == 0);
        }
        _exit(0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void CheckRegisters(Fixture run, const PersonCostSite& site, unsigned alignment, uint32_t person, unsigned variant)
{
    State input{}, output{};
    // Distinct bit patterns expose swapped registers and partial-width writes.
    uint64_t* words = &input.r15;
    for (unsigned i = 0; i < 15; ++i) words[i] = UINT64_C(0x8123456700000000) + UINT64_C(0x010101011234567) * (i + variant);
    for (unsigned i = 0; i < 16; ++i)
        for (unsigned j = 0; j < 16; ++j) input.xmm[i][j] = i * 17 + j + variant;
    input.flags = 0x202 | (variant & 1 ? 0x8d5 : 0);
    const uint64_t signedPerson = person <= INT32_MAX ? uint64_t(person) : uint64_t(person) - (UINT64_C(1) << 32);
    const uint64_t original = signedPerson + UINT32_C(0x9e3779b9);
    if (site.input == CostInput::Rax) input.rax = original;
    else if (site.input == CostInput::R8) input.r8 = original;
    else input.rdx = original;
    const uint32_t line = variant ? 0xffffffffu : 20808u;
    const uint16_t stop = variant ? 0xffffu : 1u;
    unsigned char section[8]{};
    std::memcpy(section, &line, sizeof(line));
    std::memcpy(section + 4, &stop, sizeof(stop));
    if (site.kind == CostKind::Line) input.rsi = reinterpret_cast<uintptr_t>(section);
    alignas(16) unsigned char frame[0xc00]{};
    const uint64_t loadedR9 = UINT64_C(0xb123456789abcdef) + variant;
    if (site.kind == CostKind::PathRevision) {
        input.rbp = reinterpret_cast<uintptr_t>(frame + 0xbf0);
        std::memcpy(frame + 0x10, &loadedR9, sizeof(loadedR9));
        input.r13 = signedPerson;
        input.rax = UINT64_C(0xdeadbeef00000000) | line;
        input.rsi = UINT64_C(0x9e3779b9);
    } else if (site.kind == CostKind::PathBatch) {
        input.rax = person; input.r14 = line;
    }
    State expected = input;
    const uint64_t hash = site.kind == CostKind::Line ? Tpf2mpWindowsLineCostHash(person, line, stop) : site.kind == CostKind::Walk ? Tpf2mpWindowsWalkCostHash(person) : Tpf2mpWindowsDriveCostHash(person);
    if (site.outputs & HashRdi) expected.rdi = hash;
    if (site.outputs & HashRax) expected.rax = hash;
    if (site.outputs & HashRbx) expected.rbx = hash;
    if (site.outputs & HashR12) expected.r12 = hash;
    if (site.outputs & HashR13) expected.r13 = hash;
    if (site.aux == CostAux::MagicRdx) expected.rdx = UINT64_C(0x346dc5d63886594b);
    else if (site.aux == CostAux::Divisor10000) { expected.rdx = 0; expected.rsi = 10000; }
    if (site.kind == CostKind::PathRevision) {
        expected.r13 = Tpf2mpWindowsPathHashInput(person);
        expected.rax = Tpf2mpWindowsPathHashInput(line);
        expected.r9 = loadedR9;
    } else if (site.kind == CostKind::PathBatch) {
        expected.rdx = Tpf2mpWindowsPathHash(person, line);
        expected.rsi = UINT64_C(0x9e3779b9);
    }
    const unsigned oldMxcsr = _mm_getcsr();
    const unsigned mxcsr = oldMxcsr | 1u; // retain a preexisting exception flag
    _mm_setcsr(mxcsr);
    run(&input, &output);
    assert(_mm_getcsr() == mxcsr);
    _mm_setcsr(oldMxcsr);
    assert(std::memcmp(&expected.r15, &output.r15, 15 * sizeof(uint64_t)) == 0);
    assert((output.flags & 0x8d5) == (input.flags & 0x8d5));
    assert(std::memcmp(expected.xmm, output.xmm, sizeof(expected.xmm)) == 0);
    assert(output.rspBefore == output.rspAfter);
    assert(output.rspBefore % 16 == alignment);
}

int main()
{
    assert(!Tpf2mpInstallPersonCosts(1, "unknown")); // refuse before any image read
    assert(!Tpf2mpInstallPersonCosts(0, kCostBuildId));
    assert(!Tpf2mpInstallPersonCosts(1, nullptr));
    const size_t imageSize = 0x155e000;
    void* image = mmap(nullptr, imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    for (const auto& site : kPersonCostSites)
        std::memcpy(reinterpret_cast<void*>(base + site.contextRva), site.context, site.contextSize);
    assert(mprotect(image, imageSize, PROT_READ | PROT_EXEC) == 0);
    for (const auto& site : kPersonCostSites) {
        // A changed argument/hash instruction must disable every site before
        // patching, even when the offending prefix comes after earlier sites.
        const uint8_t changed = OriginalPatch(site)[0] ^ 1;
        Write(base + site.patchRva, &changed, 1);
        assert(!Tpf2mpInstallPersonCosts(base, kCostBuildId));
        assert(g_costActiveMask == 0);
        Write(base + site.patchRva, OriginalPatch(site), 1);
        for (const auto& check : kPersonCostSites)
            assert(std::memcmp(reinterpret_cast<void*>(base + check.contextRva), check.context, check.contextSize) == 0);
    }
    for (unsigned failure = 1; failure <= kCostSiteCount; ++failure) {
        CheckRollback(base, false, failure);
        CheckRollback(base, true, failure);
    }
    assert(Tpf2mpInstallPersonCosts(base, kCostBuildId));
    assert(g_costActiveMask == 255);
    assert(std::strcmp(Tpf2mpPersonCostStatus(), "enabled at 8 verified windows") == 0);
    unsigned char* page = static_cast<unsigned char*>(mmap(nullptr, 4096, PROT_READ | PROT_EXEC,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(page != MAP_FAILED);
    const uint32_t persons[] = {0, 1, 2, 3, 8189, 19951, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff};
    unsigned cases = 0;
    for (const auto& site : kPersonCostSites) for (unsigned alignment : {0u, 8u}) {
        const Fixture run = MakeFixture(base, site, alignment, page);
        for (uint32_t person : persons) for (unsigned variant : {0u, 1u}) {
            CheckRegisters(run, site, alignment, person, variant); ++cases;
        }
    }
    assert(!Tpf2mpInstallPersonCosts(base, kCostBuildId));
    assert(g_costActiveMask == 255);
    assert(std::strstr(Tpf2mpPersonCostStatus(), "already active") != nullptr);
    assert(munmap(page, 4096) == 0);
    assert(munmap(image, imageSize) == 0);
    std::printf("person cost: %u complete GP/XMM/flags/stack cases, eight signatures and rollback paths passed\n", cases);
}
