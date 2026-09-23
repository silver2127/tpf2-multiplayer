#pragma once
#include <algorithm>
#include <cstdint>
#include <unistd.h>
namespace linux_memory {
// Windows AutoTerrainBudgets: installed MiB / 30, bounded to 256..4096.
inline int TerrainHotMB(uint64_t physicalBytes,int configured) {
    if(configured>0)return std::clamp(configured,128,16384);
    return int(std::clamp<uint64_t>((physicalBytes>>20)/30,256,4096));
}
inline uint64_t PhysicalBytes() {
    const long pages=sysconf(_SC_PHYS_PAGES),size=sysconf(_SC_PAGESIZE);
    return pages>0 && size>0?uint64_t(pages)*uint64_t(size):0;
}
}
