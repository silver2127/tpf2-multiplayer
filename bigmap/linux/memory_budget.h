#pragma once
#include <algorithm>
#include <cstdint>
#include <unistd.h>
#include <cstdio>
#include <cstring>
namespace linux_memory {
// Startup fallback (dynamic automatic target below supersedes this): installed MiB / 30, bounded to 256..4096.
inline int TerrainHotMB(uint64_t physicalBytes,int configured) {
    if(configured>0)return std::clamp(configured,128,16384);
    return int(std::clamp<uint64_t>((physicalBytes>>20)/30,256,4096));
}
// Windows PagerHeadroom / PagerCapMB, with Linux MemAvailable (no swap).
inline uint64_t TerrainTarget(uint64_t physical,uint64_t available,
                              uint64_t resident,uint64_t live,uint64_t fallback) {
    if(!physical || available==UINT64_MAX)return std::min(live,fallback);
    constexpr uint64_t GiB=1ull<<30;
    const auto reserve=std::clamp<uint64_t>(physical/7,2*GiB,12*GiB);
    const auto cap=std::clamp<uint64_t>(physical/4,4*GiB,8*GiB);
    const auto room=available>=reserve?resident+(available-reserve):
        (resident>reserve-available?resident-(reserve-available):0);
    return std::min({live,cap,room});
}
inline uint64_t AvailableBytes() {
    FILE* f=fopen("/proc/meminfo","r");if(!f)return UINT64_MAX;
    char line[256];unsigned long long kb=0;bool found=false;
    while(fgets(line,sizeof(line),f))if(sscanf(line,"MemAvailable: %llu kB",&kb)==1){found=true;break;}
    fclose(f);return found?uint64_t(kb)*1024:UINT64_MAX;
}
inline uint64_t PhysicalBytes() {
    const long pages=sysconf(_SC_PHYS_PAGES),size=sysconf(_SC_PAGESIZE);
    return pages>0 && size>0?uint64_t(pages)*uint64_t(size):0;
}
}
