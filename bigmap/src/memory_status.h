// memory_status.h -- the pagers' view of the machine's memory, with a rig-only
// switch to pretend the machine is smaller.
//
// simulate_physical_mb (tpf2_bigmap.cfg, 0 = off): the pager policy sees a
// machine of that size. Free RAM and free commit are scaled to the SAME
// FRACTIONS the real machine shows, so the policy reacts to real changes in
// proportion: "a 32 GiB machine that is as full as this one". Only the policy
// (thresholds, headroom, caps, budgets) is fooled; the engine allocates what it
// allocates. For a faithful test boot with `bcdedit /set truncatememory`.
#pragma once
#include <windows.h>
#include <cstdint>
static int g_simPhysicalMB = 0;
static BOOL PagerMemoryStatus(MEMORYSTATUSEX* m) {
    BOOL ok = GlobalMemoryStatusEx(m);
    if (!ok || g_simPhysicalMB <= 0 || !m->ullTotalPhys) return ok;
    const uint64_t sim = uint64_t(g_simPhysicalMB) << 20;
    const uint64_t pageFile = m->ullTotalPageFile > m->ullTotalPhys ? m->ullTotalPageFile - m->ullTotalPhys : 0;  // the real page file, kept
    const double physFree = double(m->ullAvailPhys) / double(m->ullTotalPhys);
    const double commitFree = m->ullTotalPageFile ? double(m->ullAvailPageFile) / double(m->ullTotalPageFile) : physFree;
    m->ullTotalPhys = sim;
    m->ullAvailPhys = uint64_t(double(sim) * physFree);
    m->ullTotalPageFile = sim + pageFile;
    m->ullAvailPageFile = uint64_t(double(m->ullTotalPageFile) * commitFree);
    m->dwMemoryLoad = DWORD(100.0 * (1.0 - physFree));
    return ok;
}
