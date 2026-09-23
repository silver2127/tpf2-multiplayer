#include "windows_person_cost_linux.h"
#include <cassert>
#include <cstring>
#include <cstdio>

static uint32_t Bits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int main()
{
    // Includes live person IDs and negative int representation boundaries.
    static const struct { uint32_t input, walk, drive; } cases[] = {
        {0x00000000u, 0x3f2460aau, 0x3f572b02u},
        {0x00000001u, 0x3f69ba5eu, 0x3f6ea4a8u},
        {0x00000002u, 0x3f969446u, 0x3f657a78u},
        {0x00000003u, 0x3f958106u, 0x3f9b3d08u},
        {0x00000007u, 0x3f509d49u, 0x3f104ea5u},
        {0x0000002au, 0x3fb6f694u, 0x3fbf5c28u},
        {0x000002ceu, 0x3fb61134u, 0x3f26b50bu},
        {0x00002fc8u, 0x3f30f909u, 0x3f1bac71u},
        {0x0000500eu, 0x3f9afec6u, 0x3f46fd22u},
        {0x0000510bu, 0x3fb872b0u, 0x3fb11340u},
        {0x7fffffffu, 0x3fbd21ffu, 0x3faf0068u},
        {0x80000000u, 0x3f8a8240u, 0x3f2d6388u},
        {0xfffffffeu, 0x3f7c7e28u, 0x3f863bcdu},
        {0xffffffffu, 0x3fa075f7u, 0x3f0ded28u},
    };
    for (const auto& test : cases) {
        assert(Bits(Tpf2mpWindowsWalkCost(test.input)) == test.walk);
        assert(Bits(Tpf2mpWindowsDriveCost(test.input)) == test.drive);
    }
    // Fixtures from original Windows RVA 0x96f68a..0x96f785.
    assert(Tpf2mpWindowsLineCostHash(25872u, 20808u, 0u) == UINT64_C(0x4c5137c78d213ebf));
    assert(Tpf2mpWindowsLineCostHash(25872u, 20808u, 1u) == UINT64_C(0x4c545dc78d241de0));
    assert(Tpf2mpWindowsLineCostHash(0u, 0u, 0u) == UINT64_C(0x9f44763f3b5cfa84));
    assert(Tpf2mpWindowsLineCostHash(4294967295u, 4294967295u, 65535u) == UINT64_C(0x6bfbefa89364b7ab));
    assert(Tpf2mpWindowsLineCostHash(2147483648u, 2147483647u, 32768u) == UINT64_C(0xca47d7757aa52e4f));
    // Both Windows PathFactory inline hash blocks agree on these values.
    assert(Tpf2mpWindowsPathHash(0, 0) == UINT64_C(0xe4e902af706fd137));
    assert(Tpf2mpWindowsPathHash(0, 1) == UINT64_C(0x44f4bea6ca312da6));
    assert(Tpf2mpWindowsPathHash(0, 20808) == UINT64_C(0x724f1d084e1aab68));
    assert(Tpf2mpWindowsPathHash(0, 25872) == UINT64_C(0xf0909b36523821dc));
    assert(Tpf2mpWindowsPathHash(0, 0x7fffffffu) == UINT64_C(0xb8d08294d0c025db));
    std::puts("Windows person cost bit-pattern fixtures passed");
}
