// Compile the installer here to exercise its private signature constants and
// original machine-code paths in an isolated fake image. No game files or
// running game processes are needed or touched by this test.
#include "../src/destination_rng_linux.cpp"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <array>

struct FixtureMT {
    uint32_t state[624]{};
    uint64_t index = 0;
};
static_assert(offsetof(FixtureMT, index) == 0x9c0);

static uint32_t Untemper(uint32_t y)
{
    uint32_t x = y;
    for (unsigned i = 0; i < 32; ++i) x = y ^ (x >> 18);
    y = x;
    for (unsigned i = 0; i < 32; ++i) x = y ^ ((x << 15) & 0xefc60000u);
    y = x;
    for (unsigned i = 0; i < 32; ++i) x = y ^ ((x << 7) & 0x9d2c5680u);
    y = x;
    for (unsigned i = 0; i < 32; ++i) x = y ^ (x >> 11);
    return x;
}

static FixtureMT Words(std::initializer_list<uint32_t> raw)
{
    FixtureMT mt;
    size_t i = 0;
    for (uint32_t word : raw) mt.state[i++] = Untemper(word);
    return mt;
}

// Stand in for the native assertion target without dereferencing its static
// diagnostic strings. A child process proves invalid ranges reach this target.
[[noreturn]] static void FixtureAssertion() { _exit(73); }

static void CheckInvalidRange(ExclusiveRandom call, int minimum, int maximum)
{
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        FixtureMT mt;
        call(&mt, minimum, maximum);
        _exit(74);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 73);
}

static unsigned twistCalls = 0;
static void FixtureTwist(void* context)
{
    // Verify the adapter delegates exhaustion to the original engine path.
    // Real MT twist parity is checked by the independent original-binary
    // oracle; this small fixture supplies a known next word at that boundary.
    auto& mt = *static_cast<FixtureMT*>(context);
    assert(mt.index == 624);
    mt.index = 0;
    mt.state[0] = Untemper(42);
    ++twistCalls;
}

static void AbsoluteJump(unsigned char* p, uintptr_t target)
{
    const unsigned char jump[] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(p, jump, sizeof(jump));
    std::memcpy(p + 6, &target, sizeof(target));
}

template<typename Function = ExclusiveRandom>
static Function MakeCaller(unsigned char* page, uintptr_t target, uintptr_t returnPc)
{
    // Enter the real patched function with a chosen caller return address,
    // maintaining the SysV stack alignment. The verified call windows remain
    // byte-for-byte original; their argument setup need not be simulated.
    const unsigned char stub[] = {
        0x48,0x83,0xec,0x08,       // sub rsp,8
        0x48,0xb8,0,0,0,0,0,0,0,0,// mov rax,returnPc
        0x50,                    // push rax
        0x48,0xb8,0,0,0,0,0,0,0,0,// mov rax,target
        0xff,0xe0                // jmp rax
    };
    std::memcpy(page, stub, sizeof(stub));
    std::memcpy(page + 6, &returnPc, sizeof(returnPc));
    std::memcpy(page + 17, &target, sizeof(target));
    return reinterpret_cast<Function>(page);
}

static PersonMt MakeRegisterCaller(unsigned char* page, uintptr_t target,
                                    uintptr_t returnPc, const PersonSeedRegisters& registers)
{
    // Preserve our caller's nonvolatile registers, then provide each original
    // game register independently. The target jump does not clobber RAX/RDX.
    unsigned char* p = page;
    auto emit = [&](std::initializer_list<unsigned char> bytes) { for (auto b : bytes) *p++ = b; };
    auto imm = [&](uint64_t value) { std::memcpy(p, &value, 8); p += 8; };
    emit({0x53,0x41,0x54,0x41,0x57}); // push rbx; push r12; push r15
    emit({0x48,0xbb}); imm(registers.rbx);
    emit({0x49,0xbc}); imm(registers.r12);
    emit({0x49,0xbf}); imm(registers.r15);
    emit({0x48,0xb8}); imm(returnPc); emit({0x50});
    emit({0x48,0xba}); imm(registers.rdx);
    emit({0x48,0xb8}); imm(registers.rax);
    AbsoluteJump(p, target); p += 14;
    assert(p - page <= 128);
    return reinterpret_cast<PersonMt>(page);
}

static int64_t SignedBits(uint32_t bits)
{
    return bits <= UINT32_C(0x7fffffff) ? int64_t(bits) : int64_t(bits) - (INT64_C(1) << 32);
}

int main()
{
    // Unknown IDs must be rejected before reading even an unmapped base.
    assert(!Tpf2mpInstallDestinationRng(1, "unknown"));
    assert(!Tpf2mpInstallDestinationRng(1, nullptr));
    assert(!Tpf2mpInstallDestinationRng(0, kRngBuildId));
    assert(!Tpf2mpInstallPersonSeeds(1, "unknown"));
    assert(!Tpf2mpInstallPersonSeeds(1, nullptr));
    assert(!Tpf2mpInstallPersonSeeds(0, kRngBuildId));

    constexpr size_t imageSize = (kRandomRva + 8191) & ~size_t(4095);
    auto* image = static_cast<unsigned char*>(mmap(nullptr, imageSize,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    std::memcpy(image + kRandomRva, kRandomBytes, sizeof(kRandomBytes));
    std::memcpy(image + kInclusiveRva, kInclusiveBytes, sizeof(kInclusiveBytes));
    std::memcpy(image + kPersonMtRva, kPersonMtBytes, sizeof(kPersonMtBytes));
    std::memcpy(image + kPersonSeedWindowRva, kPersonSeedCallBytes, sizeof(kPersonSeedCallBytes));
    std::memcpy(image + kArrivalSeedWindowRva, kArrivalSeedCallBytes, sizeof(kArrivalSeedCallBytes));
    std::memcpy(image + kIdleSeedWindowRva, kIdleSeedCallBytes, sizeof(kIdleSeedCallBytes));
    for (const auto& site : kExtraPersonSeedSites) std::memcpy(image + site.windowRva, site.bytes, site.size);
    for (uintptr_t rva : {kRandomRva, kInclusiveRva}) {
        image[rva] ^= 1;
        assert(!Tpf2mpInstallDestinationRng(base, kRngBuildId));
        image[rva] ^= 1;
        assert(g_originalRandom == nullptr);
        assert(std::memcmp(image + kRandomRva, kRandomBytes, sizeof(kRandomBytes)) == 0);
    }
    for (uintptr_t rva : {kPersonMtRva, kPersonSeedWindowRva, kArrivalSeedWindowRva, kIdleSeedWindowRva}) {
        image[rva] ^= 1;
        assert(!Tpf2mpInstallPersonSeeds(base, kRngBuildId));
        image[rva] ^= 1;
        assert(g_originalPersonMt == nullptr);
        assert(std::memcmp(image + kPersonMtRva, kPersonMtBytes, sizeof(kPersonMtBytes)) == 0);
    }
    for (const auto& site : kExtraPersonSeedSites) {
        image[site.windowRva] ^= 1;
        assert(!Tpf2mpInstallPersonSeeds(base, kRngBuildId));
        image[site.windowRva] ^= 1;
        assert(g_originalPersonMt == nullptr);
        assert(std::memcmp(image + kPersonMtRva, kPersonMtBytes, sizeof(kPersonMtBytes)) == 0);
    }

    AbsoluteJump(image + 0x14ed8d0, reinterpret_cast<uintptr_t>(FixtureTwist));
    AbsoluteJump(image + 0x2fcb860, reinterpret_cast<uintptr_t>(FixtureAssertion));
    const unsigned char finish[] = {0x48,0x83,0xc4,0x08,0xc3}; // add rsp,8; ret
    std::memcpy(image + kPersonSeedReturnRva, finish, sizeof(finish));
    std::memcpy(image + kArrivalSeedReturnRva, finish, sizeof(finish));
    // Two ordinary caller contexts: neither uses a destination/car return PC.
    std::memcpy(image + 8192, finish, sizeof(finish));
    std::memcpy(image + 8320, finish, sizeof(finish));
    auto integerA = MakeCaller(image + 4096, base + kRandomRva, base + 8192);
    auto integerB = MakeCaller(image + 4160, base + kRandomRva, base + 8320);
    auto departure = MakeCaller<PersonMt>(image + 4288, base + kPersonMtRva, base + kPersonSeedReturnRva);
    auto otherCtor = MakeCaller<PersonMt>(image + 4352, base + kPersonMtRva, base + 8192);
    auto arrival = MakeCaller<PersonMt>(image + 4416, base + kPersonMtRva, base + kArrivalSeedReturnRva);
    const unsigned char finishCaptured[] = {0x41,0x5f,0x41,0x5c,0x5b,0xc3}; // pop r15/r12/rbx; ret
    std::memcpy(image + kIdleSeedReturnRva, finishCaptured, sizeof(finishCaptured));
    std::memcpy(image + 8256, finishCaptured, sizeof(finishCaptured));
    PersonSeedRegisters poison{0xaaaaaaaaaaaaaaaau, 0xbbbbbbbbbbbbbbbbu, 1, 1, 1, 0};
    auto poisonedOther = MakeRegisterCaller(image + 4672, base + kPersonMtRva, base + 8256, poison);
    PersonSeedSite entitySites[8] = {kIdlePersonSeedSite};
    for (unsigned i = 0; i < 7; ++i) entitySites[i + 1] = kExtraPersonSeedSites[i];
    struct RegisterCase { uint32_t tag, time, entity; PersonMt call; FixtureMT expected; };
    std::array<RegisterCase, 32> registerCases{};
    const uint32_t times[] = {2800, 0x7fffffff, 0x80000000, 0xffffffff};
    const uint32_t entities[] = {8189, 0x7fffffff, 0x80000000, 0xffffffff};
    unsigned n = 0;
    for (const auto& site : entitySites) {
        std::memcpy(image + site.returnRva, finishCaptured, sizeof(finishCaptured));
        for (unsigned i = 0; i < 4; ++i) {
            auto& c = registerCases[n]; c.tag = site.tag; c.time = times[i]; c.entity = entities[i];
            PersonSeedRegisters registers = poison;
            const uintptr_t entityAddress = reinterpret_cast<uintptr_t>(&c.entity);
            if (site.entity == EntityRegister::Rbx) registers.rbx = entityAddress;
            else if (site.entity == EntityRegister::R12) registers.r12 = entityAddress;
            else registers.r15 = entityAddress;
            constexpr uint64_t k = UINT64_C(0x9e3779b9);
            const uint64_t first = k + site.tag;
            const uint64_t s = (uint64_t(SignedBits(c.time)) + k + (first << 6) + (first >> 2)) ^ first;
            assert(s < (UINT64_C(1) << 38));
            if (site.intermediate == SeedIntermediate::Rax) registers.rax = s;
            else if (site.intermediate == SeedIntermediate::Rdx) registers.rdx = s;
            else registers.rdx = uint64_t(SignedBits(c.entity)) + k + (s << 6);
            c.call = MakeRegisterCaller(image + 16384 + n * 128, base + kPersonMtRva,
                                        base + site.returnRva, registers);
            ++n;
        }
    }
    assert(n == registerCases.size());
    assert(mprotect(image, imageSize, PROT_READ | PROT_EXEC) == 0);

    // Capture full original engine states before either hook is installed.
    struct SeedCase { uint32_t nativeSeed, windowsSeed; };
    const SeedCase seeds[] = {
        {0xcd94abe5, 0x37775b60}, // time 2800: first new destination batch
        {0x4d94beda, 0x83fdc18e}, // INT_MAX time
        {0x4d94bed5, 0x2359aee2}, // INT_MIN time bits
        {0xcd94beda, 0x83faaf0e}, // -1 time bits
    };
    FixtureMT nativeStates[4], windowsStates[4];
    for (unsigned i = 0; i < 4; ++i) {
        otherCtor(&nativeStates[i], seeds[i].nativeSeed);
        otherCtor(&windowsStates[i], seeds[i].windowsSeed);
        assert(nativeStates[i].index == 624 && windowsStates[i].index == 624);
        assert(std::memcmp(&nativeStates[i], &windowsStates[i], sizeof(FixtureMT)) != 0);
    }
    FixtureMT arrivalExpected;
    otherCtor(&arrivalExpected, 0x9173eac0);
    for (auto& c : registerCases) otherCtor(&c.expected, Tpf2mpWindowsEntitySeed(c.tag, c.time, c.entity));

    FixtureMT mt = Words({46662977});
    assert(integerB(&mt, 0, 10) == 0 && mt.index == 1);
    assert(Tpf2mpInstallDestinationRng(base, kRngBuildId));
    assert(Tpf2mpInstallPersonSeeds(base, kRngBuildId));
    for (unsigned i = 0; i < 4; ++i) {
        departure(&mt, seeds[i].nativeSeed);
        assert(std::memcmp(&mt, &windowsStates[i], sizeof(mt)) == 0);
        otherCtor(&mt, seeds[i].nativeSeed);
        assert(std::memcmp(&mt, &nativeStates[i], sizeof(mt)) == 0);
    }
    arrival(&mt, 0xcd94aba4);
    assert(std::memcmp(&mt, &arrivalExpected, sizeof(mt)) == 0);
    poisonedOther(&mt, seeds[0].nativeSeed);
    assert(std::memcmp(&mt, &nativeStates[0], sizeof(mt)) == 0);
    for (const auto& c : registerCases) {
        c.call(&mt, 0x12345678); // the lossy final seed cannot replace the register inputs
        assert(std::memcmp(&mt, &c.expected, sizeof(mt)) == 0);
    }
    mt = Words({46662977});
    assert(integerB(&mt, 0, 9999999) == 6662981 && mt.index == 1);
    mt = Words({46662977});
    assert(integerA(&mt, 0, 10) == 7 && mt.index == 1);
    mt = Words({46662977});
    assert(integerB(&mt, 0, 10) == 7 && mt.index == 1);
    mt = Words({46662977});
    auto direct = reinterpret_cast<ExclusiveRandom>(base + kRandomRva);
    assert(direct(&mt, 0, 10) == 7 && mt.index == 1);

    mt = Words({46662977});
    assert(integerA(&mt, 0, 1) == 0 && mt.index == 0);
    assert(integerB(&mt, 0, 1) == 0 && mt.index == 0);
    mt = Words({UINT32_MAX});
    assert(integerA(&mt, 0, 2) == 1 && mt.index == 1);
    mt = Words({UINT32_MAX, 4294967290u, 5});
    assert(integerA(&mt, 0, 10) == 5 && mt.index == 3);
    mt = Words({46662977});
    assert(integerA(&mt, -10, 0) == -3 && mt.index == 1);

    mt = Words({UINT32_MAX, 0});
    assert(integerB(&mt, INT_MIN, INT_MAX) == INT_MIN && mt.index == 2);
    CheckInvalidRange(integerA, 4, 4);
    CheckInvalidRange(integerB, 4, -4);
    CheckInvalidRange(direct, INT_MIN, INT_MIN);

    mt = Words({0});
    mt.index = 624;
    assert(integerA(&mt, 0, 1) == 0 && mt.index == 624 && twistCalls == 0);
    assert(integerA(&mt, 0, 10) == 2 && mt.index == 1 && twistCalls == 1);
    assert(!Tpf2mpInstallDestinationRng(base, kRngBuildId)); // refuses an already-patched entry
    assert(!Tpf2mpInstallPersonSeeds(base, kRngBuildId));
    puts("PASS: Windows MT integer semantics for all callers, native invalid-range assertions, all ten seed gates, signature refusal and full MT state/advancement");
    munmap(image, imageSize);
}
