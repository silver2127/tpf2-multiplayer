// steam_poll_linux.cpp -- the game's Steam poll thread no longer spins (build 35924).
//
// The game runs one thread (loop 0x36b1960) that polls Steam: 0x36be010 takes a
// Steam interface and makes four IPC calls on it, then SteamAPI_RunCallbacks. When
// the flag at +0x130 of its object is clear the loop sleeps 200 ms between polls;
// while it is set -- through a world load -- the loop has no sleep at all, and each
// pass is synchronous IPC to the Steam client over loopback TCP. On the dedicated
// server that thread held ~60% of a core for the whole load (2026-09-27: 14,000
// TCP sends in 60 s, DWARF stacks through steamclient.so to 0x36b1990).
//
// The loop's call to 0x36be010 now comes here and runs at most once per
// steam_poll_ms (default 2): every poll and every RunCallbacks still happens, in
// the same order, on the same thread; a pass that follows the previous one too
// closely waits out the rest of the interval first. The 200 ms idle path is never
// slowed. steam_poll_ms=0 in tpf2_menu_flags.txt turns it off.
#include "steam_poll_linux.h"
#include "slice_core.h"
#include "near_alloc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace {
constexpr uintptr_t kLoop = 0x36b1960, kLoopSize = 0xbd, kSite = 0x36b1990, kPoll = 0x36be010;
const char kLoopBytes[] =
    "\xf3\x0f\x1e\xfa\x55\x48\x89\xe5\x41\x54\x53\x4c\x8d\x65\xd0\x48\x89\xfb\x48\x83\xec\x20\x64\x48"
    "\x8b\x04\x25\x28\x00\x00\x00\x48\x89\x45\xe8\x31\xc0\xeb\x13\x66\x0f\x1f\x84\x00\x00\x00\x00\x00"
    "\xe8\x7b\xc6\x00\x00\xe8\xd6\xc3\x02\xfd\x48\x8b\x43\x08\x48\x8b\x40\x08\x0f\xb6\x80\x00\x02\x00"
    "\x00\x84\xc0\x75\x53\x48\x8b\x43\x08\x48\x8b\x78\x08\x80\xbf\x30\x01\x00\x00\x00\x75\xd2\xe8\x7d"
    "\xae\x02\xfd\x48\xc7\x45\xd0\x00\x00\x00\x00\x48\xc7\x45\xd8\x00\xc2\xeb\x0b\xeb\x0d\x0f\x1f\x00"
    "\xe8\xc3\x9e\x02\xfd\x83\x38\x04\x75\x10\x4c\x89\xe6\x4c\x89\xe7\xe8\x93\xaf\x02\xfd\x83\xf8\xff"
    "\x74\xe6\x48\x8b\x43\x08\x48\x8b\x78\x08\xeb\x94\x0f\x1f\x40\x00\x48\x8b\x45\xe8\x64\x48\x33\x04"
    "\x25\x28\x00\x00\x00\x75\x09\x48\x83\xc4\x20\x5b\x41\x5c\x5d\xc3\xe8\x43\x9e\x02\xfd";
static_assert(sizeof(kLoopBytes) - 1 == kLoopSize, "the whole loop function");

using PollFn = void (*)(void*);
PollFn g_poll = nullptr;
uint64_t g_intervalNs = 0;

uint64_t NowNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void ThrottledPoll(void* self)
{
    static thread_local uint64_t last = 0;
    const uint64_t now = NowNs();
    if (last && now - last < g_intervalNs) {
        const uint64_t wait = g_intervalNs - (now - last);
        timespec ts{time_t(wait / 1000000000ull), long(wait % 1000000000ull)};
        while (nanosleep(&ts, &ts) != 0) {}
    }
    last = NowNs();
    g_poll(self);
}

int FlagMs(const char* root, const char* data)
{
    for (const char* dir : {root, data}) {
        if (!dir || !*dir) continue;
        FILE* f = fopen((std::string(dir) + "/tpf2_menu_flags.txt").c_str(), "r");
        if (!f) continue;
        char line[256]; int n = -1;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "steam_poll_ms=", 14)) n = atoi(line + 14);
        fclose(f);
        return n;
    }
    return -1;
}
} // namespace

bool SliceInstallSteamPoll(uintptr_t base, const char* root, const char* data)
{
    int ms = FlagMs(root, data);
    if (ms == 0) {
        SliceLog("[steampoll] OFF (steam_poll_ms=0) -- the game's Steam poll spins while it is busy\n");
        return false;
    }
    if (ms < 0) ms = 2;
    if (ms > 100) ms = 100;
    char bytes[kLoopSize];
    if (!SliceRead(base + kLoop, bytes, kLoopSize) || memcmp(bytes, kLoopBytes, kLoopSize)) {
        SliceLog("[steampoll] byte guard failed at %lx -- the game's Steam poll is left as it is\n", (unsigned long)kLoop);
        return false;
    }
    g_intervalNs = uint64_t(ms) * 1000000ull;
    g_poll = reinterpret_cast<PollFn>(base + kPoll);
    if (!Tpf2mpRedirectCall(base + kSite, base + kPoll, reinterpret_cast<void*>(&ThrottledPoll))) {
        SliceLog("[steampoll] the poll call at %lx is not the expected one -- left as it is\n", (unsigned long)kSite);
        return false;
    }
    SliceLog("[steampoll] installed: the game's Steam poll runs at most every %d ms while busy (was a spin; idle stays 200 ms)\n", ms);
    return true;
}
