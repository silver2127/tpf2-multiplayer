#include "../src/float_rng_linux.cpp"
#include <sys/mman.h>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <initializer_list>

struct TestMT { uint32_t words[624]{}; uint64_t index = 0; };
static_assert(offsetof(TestMT, index) == 0x9c0);

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
static unsigned twistCalls = 0;
static void Twist(TestMT* mt)
{
    assert(mt->index == 624);
    mt->index = 0;
    mt->words[0] = Untemper(42);
    ++twistCalls;
}
static float NextAfter(float a, float b) { return std::nextafter(a, b); }
static void Jump(unsigned char* p, uintptr_t target)
{
    const unsigned char op[] = {0xff,0x25,0,0,0,0};
    std::memcpy(p, op, sizeof(op)); std::memcpy(p + 6, &target, 8);
}
static uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

int main()
{
    assert(!Tpf2mpInstallFloatRng(1, "unknown"));
    assert(!Tpf2mpInstallFloatRng(1, nullptr));
    assert(!Tpf2mpInstallFloatRng(0, kFloatBuildId));
    constexpr size_t length = (kFloatScaleRva + 8191) & ~size_t(4095);
    auto* memory = static_cast<unsigned char*>(mmap(nullptr, length,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
    std::memcpy(memory + kFloatCoreRva, kFloatCoreBytes, sizeof(kFloatCoreBytes));
    std::memcpy(memory + kFloatScaleRva, &kFloatScale, 4);
    std::memcpy(memory + kFloatOneRva, &kFloatOne, 4);
    for (const uintptr_t rva : {kFloatCoreRva, kFloatClampRva, kFloatScaleRva, kFloatOneRva}) {
        memory[rva] ^= 1;
        assert(!Tpf2mpInstallFloatRng(base, kFloatBuildId));
        assert(memory[kFloatClampRva + 1] == 0x1c);
        memory[rva] ^= 1;
    }
    Jump(memory + 0x14ed8d0, reinterpret_cast<uintptr_t>(Twist));
    Jump(memory + 0x6dc590, reinterpret_cast<uintptr_t>(NextAfter));
    assert(mprotect(memory, length, PROT_READ | PROT_EXEC) == 0);
    auto draw = reinterpret_cast<float (*)(TestMT*)>(memory + kFloatCoreRva);
    TestMT before; before.words[0] = Untemper(UINT32_MAX);
    assert(Bits(draw(&before)) == 0x3f7fffffu && before.index == 1);
    assert(Tpf2mpInstallFloatRng(base, kFloatBuildId));
    assert(memory[kFloatClampRva] == 0x90 && memory[kFloatClampRva + 1] == 0x90);
    unsigned changed = 0;
    for (uint64_t value = UINT64_C(0xfffffe00); value <= UINT32_MAX; ++value) {
        TestMT mt; mt.words[0] = Untemper(uint32_t(value));
        const auto original = mt.words[0];
        const float actual = draw(&mt);
        volatile float converted = float(value);
        assert(Bits(actual) == Bits(converted * 0x1p-32f));
        assert(mt.index == 1 && mt.words[0] == original);
        changed += actual == 1.0f;
    }
    assert(changed == 128);
    for (uint32_t value : {0u, 1u, 42u, 0x7fffffffu, 0x80000000u}) {
        TestMT mt; mt.words[0] = Untemper(value);
        volatile float converted = float(value);
        assert(Bits(draw(&mt)) == Bits(converted * 0x1p-32f) && mt.index == 1);
    }
    TestMT exhausted; exhausted.index = 624;
    assert(draw(&exhausted) == 42.0f * 0x1p-32f);
    assert(twistCalls == 1 && exhausted.index == 1);
    assert(munmap(memory, length) == 0);
    std::puts("Original native float core: Windows upper endpoint and MT advancement verified");
}
