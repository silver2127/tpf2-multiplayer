#include "windows_person_seed_linux.h"
#include <cassert>
#include <cstdio>

int main()
{
    // Golden outputs from original Windows 35924 instructions at
    // 0xa9368f..0xa936ef, executed independently with the supplied time bits.
    // Include wraparound and both signed boundaries of the game's int time.
    struct Case { uint32_t time, nativeSeed, windowsSeed; };
    const Case cases[] = {
        {0,          0xcd94bed5, 0x23569462},
        {1,          0xcd94bed4, 0x896cc91d},
        {14,         0xcd94becb, 0x2694e104},
        {1600,       0xcd94b415, 0xd359a02c},
        {2600,       0xcd94a82d, 0x2bf0c218},
        {2800,       0xcd94abe5, 0x37775b60},
        {3000,       0xcd94aa9d, 0xa11ef73d},
        {72000,      0xcd939915, 0x306cacd4},
        {84000,      0xcd927635, 0x7b5362cd},
        {144000,     0xcd928055, 0xc32ee5fa},
        {600000,     0xcd9b9695, 0x0077bf36},
        {0x7fffffff, 0x4d94beda, 0x83fdc18e},
        {0x80000000, 0x4d94bed5, 0x2359aee2},
        {0xffffffff, 0xcd94beda, 0x83faaf0e},
    };
    for (const auto& c : cases) {
        assert(((c.time + UINT32_C(0x53a3c768)) ^ UINT32_C(0x9e3779bd)) == c.nativeSeed);
        assert(Tpf2mpWindowsDepartureSeed(c.nativeSeed) == c.windowsSeed);
    }
    const uint32_t arrival[][2] = {
        {0xcd94be94,0x7d5da5c2}, {0xcd94be95,0x0b0a7953},
        {0xcd94aba4,0x9173eac0}, {0xcd9399d4,0x900a528c},
        {0x4d94be9b,0x1df971e6}, {0x4d94be94,0x7d5d5c42},
        {0xcd94be9b,0x1df85f66},
    };
    for (const auto& c : arrival) assert(Tpf2mpWindowsArrivalSeed(c[0]) == c[1]);
    const uint32_t idle[][3] = {
        {0x00000000,0x00000000,0x0baa2f58},
        {0x00000af0,0x00001ffd,0xf2635aa9},
        {0x00000af0,0x00001ffc,0x09b94978},
        {0x00000af1,0x00001ffd,0x8240e7db},
        {0x00011940,0x00004def,0x3ab7701c},
        {0x7fffffff,0x7fffffff,0x38c645f9},
        {0x80000000,0x80000000,0x0ab39ff8},
        {0xffffffff,0xffffffff,0x3836bb99},
        {0x00000000,0xffffffff,0xabcdf5f4},
        {0xffffffff,0x00000000,0xd85a5e25},
    };
    for (const auto& c : idle) assert(Tpf2mpWindowsIdleSeed(c[0], c[1]) == c[2]);
    puts("PASS: Windows departure/arrival/idle seed witnesses, independent inputs and signed boundaries");
}
