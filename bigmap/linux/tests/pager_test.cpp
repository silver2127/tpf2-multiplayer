#include "../terrain_pager.h"
#include <cassert>
#include <cstdio>
int main(){
    linux_pager::TerrainPager pager;
    if(!pager.Start(64,0)){puts("SKIP: userfaultfd unavailable");return 77;}
    std::vector<uint16_t*> tiles;
    for(int n=0;n<32;++n){auto* p=pager.Allocate();assert(p);for(size_t i=0;i<TerrainCodec::Samples;++i)p[i]=uint16_t(i/257+i%257+n);tiles.push_back(p);}
    for(int tries=0;tries<100 && pager.Get().evictions<32;++tries)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(pager.Get().evictions>=32 && pager.Get().resident==0);
    unsigned char residency[linux_pager::TerrainPager::Stride/4096];
    assert(mincore(tiles[0],linux_pager::TerrainPager::Stride,residency)==0);
    for(auto page:residency)assert(!(page&1));
    std::vector<std::thread> readers;
    for(int n=0;n<8;++n)readers.emplace_back([&,n]{for(int k=n;k<32;k+=8)for(size_t i=0;i<TerrainCodec::Samples;++i)assert(tiles[k][i]==uint16_t(i/257+i%257+k));});
    for(auto& t:readers)t.join();
    std::atomic<bool> done{false};
    std::thread writer([&]{uint16_t value=0;while(!done){for(int n=0;n<32;++n)tiles[n][0]=value;++value;}for(int n=0;n<32;++n)tiles[n][0]=1234;});
    std::this_thread::sleep_for(std::chrono::seconds(5));done=true;writer.join();
    for(int n=0;n<32;++n){auto* p=tiles[n];assert(p[0]==1234);for(size_t i=1;i<TerrainCodec::Samples;++i)assert(p[i]==uint16_t(i/257+i%257+n));assert(pager.Release(p));}
    for(int n=0;n<1000;++n){auto p=pager.Allocate();assert(p && p[0]==0 && p[TerrainCodec::Samples-1]==0);p[0]=42;assert(pager.Release(p));}
    auto stats=pager.Get();assert(stats.live==0);printf("PASS: lossless paging, parallel restores, write/evict race, reuse; %llu evictions, %llu faults\n",(unsigned long long)stats.evictions,(unsigned long long)stats.faults);
    return 0;
}
