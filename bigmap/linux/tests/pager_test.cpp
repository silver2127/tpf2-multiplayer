#include "../terrain_pager.h"
#include <cassert>
#include <cstdio>
int main(){
    // Deterministic policy boundaries independent of kernel UFFD availability.
    linux_pager::Recency age;
    age.Reset(100);assert(!age.Eligible(2099) && age.Eligible(2100));
    age.Fault(2200);assert(!age.Eligible(4199) && age.Eligible(4200));
    age.Fault(4300);assert(!age.Eligible(14299) && age.Eligible(14300));
    age.Fault(15000);assert(age.Eligible(17000));
    age.Reset(18000);assert(age.lastFault==0 && age.Eligible(20000));
    constexpr uint64_t GiB=1ull<<30;
    using linux_memory::TerrainTarget;
    assert(TerrainTarget(32*GiB,16*GiB,0,12*GiB,GiB)==8*GiB);
    assert(TerrainTarget(32*GiB,16*GiB,0,2*GiB,GiB)==2*GiB);
    assert(TerrainTarget(32*GiB,32*GiB/7,6*GiB,12*GiB,GiB)==6*GiB);
    assert(TerrainTarget(32*GiB,0,GiB,12*GiB,GiB)==0);
    assert(TerrainTarget(0,16*GiB,0,12*GiB,GiB)==GiB);
    assert(TerrainTarget(32*GiB,UINT64_MAX,0,12*GiB,GiB)==GiB);
    assert(TerrainTarget(16*GiB,16*GiB,0,12*GiB,GiB)==4*GiB);
    // dev b6d73041: a large machine with ample available RAM keeps its cap.
    // Windows free-commit thresholds do not change Linux physical headroom.
    assert(TerrainTarget(94*GiB,33*GiB,0,12*GiB,GiB)==8*GiB);
    assert(TerrainTarget(94*GiB,12*GiB,8*GiB,12*GiB,GiB)==8*GiB);
    assert(TerrainTarget(94*GiB,10*GiB,8*GiB,12*GiB,GiB)==6*GiB);
    assert(TerrainTarget(94*GiB,4*GiB,8*GiB,12*GiB,GiB)==0);
    // dev 2b8505c: pressure releases only the shortfall, not a flat 256 MiB.
    // Linux uses MemAvailable and its existing RAM reserve, not Windows commit.
    constexpr uint64_t MiB=1ull<<20, reserve=32*GiB/7;
    const uint64_t initial=2724*MiB, deficit=1536*MiB;
    const uint64_t target=TerrainTarget(32*GiB,reserve-deficit,initial,12*GiB,GiB);
    assert(target==1188*MiB);
    // Repeated samples before eviction must not subtract from the old target.
    for(int sample=0;sample<6;++sample)
        assert(TerrainTarget(32*GiB,reserve-deficit,initial,12*GiB,GiB)==target);
    // Partial/full reclaim increases available RAM by the bytes returned.
    for(uint64_t reclaimed:{uint64_t(0),512*MiB,deficit})
        assert(TerrainTarget(32*GiB,reserve-deficit+reclaimed,
                             initial-reclaimed,12*GiB,GiB)==target);
    assert(TerrainTarget(32*GiB,reserve,initial,12*GiB,GiB)==initial);
    assert(TerrainTarget(32*GiB,reserve-1,initial,12*GiB,GiB)==initial-1);
    assert(TerrainTarget(32*GiB,reserve-deficit,1024*MiB,12*GiB,GiB)==0);
    puts("PASS: resident-based pressure shortfall, repeated samples and reclaim accounting");
    linux_pager::TerrainPager pager;
    if(!pager.Start(64,0)){puts("SKIP: userfaultfd unavailable");return 77;}
    {   // a new tile is real, zeroed memory: resident before its first write
        auto* p=pager.Allocate();assert(p);
        unsigned char in[linux_pager::TerrainPager::Stride/4096];
        assert(mincore(p,linux_pager::TerrainPager::Stride,in)==0);
        for(auto page:in)assert(page&1);
        for(size_t i=0;i<TerrainCodec::Samples;++i)assert(p[i]==0);
        assert(pager.Release(p));
    }
    // dev d8c98ccb: allocation must not wait for the zero budget to recover.
    const auto allocationStart=std::chrono::steady_clock::now();
    std::vector<uint16_t*> tiles;
    for(int n=0;n<32;++n){auto* p=pager.Allocate();assert(p);for(size_t i=0;i<TerrainCodec::Samples;++i)p[i]=uint16_t(i/257+i%257+n);tiles.push_back(p);}
    const auto allocationMs=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-allocationStart).count();
    assert(pager.Get().budget==0 && pager.Get().resident>0 && allocationMs<1000);
    for(int tries=0;tries<100 && pager.Get().evictions<32;++tries)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(pager.Get().evictions>=32 && pager.Get().resident==0);
    unsigned char residency[linux_pager::TerrainPager::Stride/4096];
    assert(mincore(tiles[0],linux_pager::TerrainPager::Stride,residency)==0);
    for(auto page:residency)assert(!(page&1));
    // Restore two cold tiles while over a fixed zero budget. There is no
    // allocation-burst flag or fault-side budget wait in the native backend.
    // The second restore must finish without waiting for the first tile's
    // two-second recency protection to expire and permit eviction.
    const auto restoreStart=std::chrono::steady_clock::now();
    for(int n=0;n<2;++n)
        for(size_t i=0;i<TerrainCodec::Samples;++i)
            assert(tiles[n][i]==uint16_t(i/257+i%257+n));
    const auto restoreMs=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-restoreStart).count();
    assert(pager.Get().budget==0 && pager.Get().resident>=2 && restoreMs<1000);
    printf("PASS: zero-budget allocation %lld ms, cold restores %lld ms\n",
           (long long)allocationMs,(long long)restoreMs);
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
