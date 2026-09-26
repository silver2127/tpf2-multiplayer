// Real Windows memory mappings and real concurrent access violations, isolated
// from the game. Optional argv[1]: offline 257x257 uint16 terrain sample file.
#include "../src/terrain_pager.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <cassert>
#include <string>

int main(int argc,char** argv) {
    using namespace TerrainPager;
    assert(Init(4*SlotBytes));
    auto p=Allocate();assert(p && uintptr_t(p)%32==0);
    for(size_t i=0;i<Samples;++i)assert(p[i]==0);
    std::vector<uint16_t> expected(Samples);
    for(size_t y=0;y<257;++y)for(size_t x=0;x<257;++x)
        expected[y*257+x]=uint16_t(y*173+x*x+65500);
    memcpy(p,expected.data(),Bytes);
    for(int n=0;n<100;++n) {
        assert(Evict(Index(p),true));
        assert(memcmp(p,expected.data(),Bytes)==0);
        size_t direct=(n*313)%Samples;p[direct]^=0x3197;expected[direct]^=0x3197;
        assert(memcmp(p,expected.data(),Bytes)==0);
        // Write faults, including both ends and all row boundaries.
        assert(Evict(Index(p),true));
        size_t pos=(n*257)%Samples;p[pos]^=0x9753;expected[pos]^=0x9753;
        assert(memcmp(p,expected.data(),Bytes)==0);
    }
    assert(Evict(Index(p),true));assert(Release(p));
    auto reused=Allocate();assert(reused==p);
    for(size_t i=0;i<Samples;++i)assert(reused[i]==0);
    assert(Release(reused));assert(!Release(reused));
    // Incompressible data must remain resident and accessible.
    p=Allocate();std::mt19937 rng(42);
    for(auto& v:expected)v=uint16_t(rng());
    memcpy(p,expected.data(),Bytes);assert(!Evict(Index(p),true));
    assert(memcmp(p,expected.data(),Bytes)==0);assert(Release(p));

    // Normal worker policy (not forced eviction): honor resident budget and
    // the five-second grace period, then restore cold storage on demand.
    std::vector<uint16_t*> policy(16);
    for(auto& t:policy){t=Allocate();assert(t);}
    Tick();assert(Snapshot().resident==16);
    {Guard g;for(auto t:policy)slots[Index(t)].touched=GetTickCount64()-6000;}
    // Over three times the budget, encode directly down to three times the
    // budget; the rest of the excess only becomes inaccessible (second chance)
    // and is encoded once it has stayed untouched for SoftDelayMs.
    Tick();assert(Snapshot().resident==12 && Snapshot().softBlocked==8);
    {Guard g;auto now=GetTickCount64();for(auto t:policy){auto& sl=slots[Index(t)];if(sl.soft)sl.blockedAt=now-SoftDelayMs;}}
    Tick();assert(Snapshot().resident==4 && Snapshot().softBlocked==0);
    // Loading burst: slots only 1.5 s old are evicted directly, down to budget.
    {Guard g;auto now=GetTickCount64();stats.lastBulkAllocation=now;for(auto t:policy)slots[Index(t)].touched=now-1500;}
    for(auto t:policy)assert(t[0]==0);
    {Guard g;auto now=GetTickCount64();stats.lastBulkAllocation=now;for(auto t:policy)slots[Index(t)].touched=now-1500;}
    Tick();assert(Snapshot().resident==4 && Snapshot().softBlocked==0);
    {Guard g;stats.lastBulkAllocation=0;}
    for(auto t:policy)assert(t[Samples-1]==0);
    for(auto t:policy)assert(Release(t));

    // Shared compressed COW snapshots: no decompression or payload duplication
    // to create a version; first write isolates it, even after parent release.
    p=Allocate();for(size_t i=0;i<Samples;++i)expected[i]=uint16_t(i*11);
    memcpy(p,expected.data(),Bytes);assert(Evict(Index(p),true));
    auto packedBefore=Snapshot();std::vector<uint16_t*> clones(32);
    for(auto& t:clones){t=Clone(p);assert(t && t!=p);}
    assert(Snapshot().resident==0);
    assert(Snapshot().compressedBytes==packedBefore.compressedBytes);
    assert(Snapshot().sharedClones==packedBefore.sharedClones+clones.size());
    assert(Release(p));
    for(size_t n=0;n<clones.size();++n) {
        auto t=clones[n];assert(memcmp(t,expected.data(),Bytes)==0);
        auto before=Snapshot();assert(Evict(Index(t),true));
        assert(Snapshot().encodes==before.encodes && Snapshot().reusedEvictions==before.reusedEvictions+1);
        // Read restores RO; direct write remaps the same section RW.
        volatile auto sample=t[n];assert(sample==expected[n]);t[n]=uint16_t(1234+n);
        assert(Evict(Index(t),true));assert(t[n]==uint16_t(1234+n));
        assert(Release(t));
    }
    assert(Snapshot().live==0 && Snapshot().compressedBytes==0);

    // Each writer owns a disjoint word; eviction races all readers/writers.
    // Addresses remain retained across every eviction. Atomic increments make
    // lost/duplicated writes independently observable.
    constexpr unsigned N=16,Threads=8,Iterations=500000;
    std::vector<uint16_t*> tiles(N);
    for(auto& t:tiles){t=Allocate();assert(t);}
    std::atomic<bool> start{false},done{false};
    std::thread evictor([&]{while(!start.load())SwitchToThread();
        while(!done.load())for(auto t:tiles)Evict(Index(t),true);});
    std::vector<std::thread> writers;
    for(unsigned n=0;n<Threads;++n)writers.emplace_back([&,n]{
        while(!start.load())SwitchToThread();
        for(unsigned k=0;k<Iterations;++k) {
            auto t=tiles[k%N];
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(t)+n*1024);
            if(!(k%127))SwitchToThread();
        }
    });
    start=true;for(auto& w:writers)w.join();done=true;evictor.join();
    for(auto t:tiles) {
        for(unsigned n=0;n<Threads;++n)assert(reinterpret_cast<LONG*>(t)[n*1024]==Iterations/N);
        assert(Release(t));
    }
    // Multiple concurrent COW users share one immutable compressed source.
    p=Allocate();memcpy(p,expected.data(),Bytes);assert(Evict(Index(p),true));
    writers.clear();start=false;done=false;
    std::thread cowEvictor([&]{while(!start.load())SwitchToThread();
        while(!done.load())for(unsigned i=0;i<128;++i)Evict(i,true);});
    for(unsigned n=0;n<Threads;++n)writers.emplace_back([&,n]{
        while(!start.load())SwitchToThread();
        for(unsigned k=0;k<200;++k) {
            auto copy=Clone(p);assert(copy);
            assert(memcmp(copy,expected.data(),Bytes)==0);
            copy[n]=uint16_t(k);assert(copy[n]==k);assert(Release(copy));
        }
    });
    start=true;for(auto& w:writers)w.join();done=true;cowEvictor.join();
    assert(memcmp(p,expected.data(),Bytes)==0);assert(Release(p));
    if(argc>1) {
        FILE* f=nullptr;fopen_s(&f,argv[1],"rb");assert(f);
        size_t total=0,compressed=0,committed=0;unsigned count=0;
        ULONGLONG elapsed=GetTickCount64();
        while(fread(expected.data(),Bytes,1,f)==1) {
            p=Allocate();assert(p);memcpy(p,expected.data(),Bytes);
            if(Evict(Index(p),true)){auto s=Snapshot();compressed+=s.compressedBytes;committed+=s.compressedCommit;}
            else {compressed+=Bytes;committed+=SlotBytes;}
            assert(memcmp(p,expected.data(),Bytes)==0);
            assert(Release(p));total+=Bytes;++count;
        }
        assert(feof(f));fclose(f);
        printf("real tiles=%u ratio=%.6f total_roundtrip_ms=%llu projected_64980_gib=%.3f cold_commit_gib=%.3f\n",
            count,double(compressed)/total,GetTickCount64()-elapsed,
            double(compressed)/total*64980*Bytes/(1024.*1024*1024),
            double(committed)/total*64980*Bytes/(1024.*1024*1024));
    }
    // Second chance: without pressure the excess is only made inaccessible; an
    // access restores it by protection alone, and only untouched slots encode.
    {
        std::vector<uint16_t> pat(Samples);
        for(size_t k=0;k<Samples;++k)pat[k]=uint16_t(30000+(k%257)+(k/257));
        std::vector<uint16_t*> soft(16);
        for(auto& t:soft){t=Allocate();assert(t);memcpy(t,pat.data(),Bytes);}
        SetBudget(12*SlotBytes);
        // Earlier sections allocate in bursts; this checks the steady-state policy.
        {Guard g;stats.lastBulkAllocation=0;for(auto t:soft)slots[Index(t)].touched=GetTickCount64()-6000;}
        auto s0=Snapshot();Tick();auto s1=Snapshot();
        assert(s1.resident==s0.resident && s1.softBlocked==4 && s1.encodes==s0.encodes);
        uint16_t* rescued=nullptr;
        {Guard g;for(auto t:soft)if(slots[Index(t)].soft){rescued=t;break;}}
        assert(rescued && rescued[123]==pat[123]);
        auto s2=Snapshot();
        assert(s2.softRescues==s1.softRescues+1 && s2.softBlocked==3 && s2.encodes==s1.encodes && s2.resident==s1.resident);
        Tick();assert(Snapshot().softBlocked==4);
        {Guard g;auto now=GetTickCount64();for(auto t:soft){auto& sl=slots[Index(t)];if(sl.soft){sl.touched=now-6000;sl.blockedAt=now-SoftDelayMs;}}}
        Tick();auto s3=Snapshot();
        assert(s3.resident==s0.resident-4 && s3.softBlocked==0 && s3.encodes==s2.encodes+4);
        for(auto t:soft){assert(memcmp(t,pat.data(),Bytes)==0);assert(Release(t));}
        SetBudget(4*SlotBytes);
    }
    // Many threads restoring cold slots at once (unlocked decodes) while an
    // evictor keeps re-evicting them: every read sees exact data.
    {
        constexpr unsigned Readers=16,Cold=64;
        std::vector<uint16_t> pat(Samples);for(size_t k=0;k<Samples;++k)pat[k]=uint16_t(k*31+7);
        std::vector<uint16_t*> cold(Cold);
        for(auto& t:cold){t=Allocate();assert(t);memcpy(t,pat.data(),Bytes);assert(Evict(Index(t),true));}
        std::atomic<bool> go{false},stop{false};
        std::thread ev([&]{while(!go)SwitchToThread();while(!stop)for(auto t:cold)Evict(Index(t),true);});
        std::vector<std::thread> readers;
        std::atomic<unsigned> bad{0};
        for(unsigned n=0;n<Readers;++n)readers.emplace_back([&,n]{
            while(!go)SwitchToThread();std::mt19937 r(n+1);
            for(unsigned k=0;k<4000;++k){auto t=cold[r()%Cold];size_t at=r()%Samples;if(t[at]!=pat[at])++bad;}
        });
        go=true;for(auto& x:readers)x.join();stop=true;ev.join();
        assert(bad==0);
        for(auto t:cold){assert(memcmp(t,pat.data(),Bytes)==0);assert(Release(t));}
    }
    // Slot churn racing four unlocked encoders: a release or reuse during an
    // encode must discard its result (generation) without failures or leaks.
    {
        std::atomic<bool> stop{false};std::vector<std::thread> evictors;
        for(unsigned n=0;n<4;++n)evictors.emplace_back([&,n]{
            std::mt19937 r(100+n);
            while(!stop){unsigned a;{Guard g;a=allocated;}if(a)Evict(r()%a,true);}
        });
        std::vector<uint16_t> pat(Samples);for(size_t k=0;k<Samples;++k)pat[k]=uint16_t(k^0x5a5a);
        for(unsigned k=0;k<3000;++k) {
            auto t=Allocate();assert(t);memcpy(t,pat.data(),Bytes);
            if(k%2)SwitchToThread();
            assert(memcmp(t,pat.data(),Bytes)==0);assert(Release(t));
        }
        stop=true;for(auto& x:evictors)x.join();
        auto sc=Snapshot();assert(sc.live==0 && sc.failures==0 && sc.softBlocked==0);
        printf("unlocked eviction: cancelled=%llu encodes=%llu\n",sc.cancelledEvictions,sc.encodes);
    }
    // Copy-on-write sharing (docs/terrain-cow-sharing.md, measurement stage): a
    // second version maps the SAME section at its own address instead of copying
    // 132 KiB. Both views are read-only; the first write to either one takes a
    // private copy and must leave every other sharer byte-identical.
    {
        std::vector<uint16_t> pat(Samples);
        for(size_t k=0;k<Samples;++k)pat[k]=uint16_t(k*7+3);
        DWORD h0=0,h1=0;GetProcessHandleCount(GetCurrentProcess(),&h0);
        auto base=Snapshot();
        auto a=Allocate();assert(a);memcpy(a,pat.data(),Bytes);
        auto afterAlloc=Snapshot();
        // Baseline the handle count here, not before Allocate: the slot's own
        // section is one handle, and sharing must add exactly one more.
        DWORD hAlloc=0;GetProcessHandleCount(GetCurrentProcess(),&hAlloc);
        auto b=Share(a);assert(b && b!=a);
        auto sh=Snapshot();
        // Free: no second section, no encode, no extra resident backing. Exactly
        // one new handle (the sharer's duplicate) and one new live slot.
        assert(sh.resident==afterAlloc.resident && sh.encodes==afterAlloc.encodes);
        assert(sh.sharedViews==base.sharedViews+1 && sh.sharedSlots==2);
        assert(sh.live==afterAlloc.live+1);
        GetProcessHandleCount(GetCurrentProcess(),&h1);assert(h1==hAlloc+1);
        assert(memcmp(a,pat.data(),Bytes)==0 && memcmp(b,pat.data(),Bytes)==0);
        // A shared backing is never an eviction candidate, not even forced.
        assert(!Evict(Index(a),true) && !Evict(Index(b),true));
        assert(Snapshot().encodes==afterAlloc.encodes);
        // Writing through b privatizes b alone.
        std::vector<uint16_t> expectB(pat);expectB[5]=0x1234;
        b[5]=0x1234;
        auto pv=Snapshot();
        assert(pv.privatizations==base.privatizations+1);
        assert(pv.resident==afterAlloc.resident+1);   // a second real section now
        assert(pv.sharedSlots==0);                    // the two-member ring collapsed
        assert(memcmp(a,pat.data(),Bytes)==0 && memcmp(b,expectB.data(),Bytes)==0);
        // Unshared again, so both behave like ordinary slots.
        assert(Evict(Index(a),true));assert(memcmp(a,pat.data(),Bytes)==0);
        assert(Evict(Index(b),true));assert(memcmp(b,expectB.data(),Bytes)==0);
        assert(Release(a));assert(Release(b));

        // The other direction: writing the SOURCE must not disturb the sharer.
        auto c=Allocate();assert(c);memcpy(c,pat.data(),Bytes);
        auto d=Share(c);assert(d);
        c[9]=0x4321;
        assert(c[9]==0x4321 && memcmp(d,pat.data(),Bytes)==0);
        assert(Release(c));assert(memcmp(d,pat.data(),Bytes)==0);assert(Release(d));

        // A ring of three: releasing members keeps the survivors sharing, and
        // the last one standing is no longer shared, so it can be evicted again.
        auto r0=Snapshot().resident;
        auto e=Allocate();assert(e);memcpy(e,pat.data(),Bytes);
        auto f=Share(e);assert(f);
        auto k3=Share(e);assert(k3);
        assert(Snapshot().sharedSlots==3 && Snapshot().resident==r0+1);
        assert(Release(f));
        assert(Snapshot().sharedSlots==2 && Snapshot().resident==r0+1);
        assert(memcmp(e,pat.data(),Bytes)==0 && memcmp(k3,pat.data(),Bytes)==0);
        assert(Release(k3));
        assert(Snapshot().sharedSlots==0 && Snapshot().resident==r0+1);
        assert(Evict(Index(e),true));assert(memcmp(e,pat.data(),Bytes)==0);
        assert(Release(e));assert(Snapshot().resident==r0);

        // Sharing, privatizing writes and releases racing a forced evictor.
        auto src=Allocate();assert(src);memcpy(src,pat.data(),Bytes);
        std::atomic<bool> go{false},stop{false};std::atomic<unsigned> bad{0},shares{0};
        // The evictor must leave the shared SOURCE resident, or it is compressed
        // almost immediately, every Share correctly refuses a packed source, and
        // the race silently exercises nothing (measured: shares fell 2399 -> 0).
        unsigned keep=Index(src);
        std::thread ev([&]{while(!go)SwitchToThread();
            while(!stop){unsigned n;{Guard gg;n=allocated;}for(unsigned i=0;i<n;++i)if(i!=keep)Evict(i,true);}});
        std::vector<std::thread> ws;
        for(unsigned n=0;n<8;++n)ws.emplace_back([&,n]{
            while(!go)SwitchToThread();
            for(unsigned k=0;k<300;++k) {
                auto t=Share(src);
                if(t)++shares; else {t=Allocate();if(!t)continue;memcpy(t,pat.data(),Bytes);}
                if(memcmp(t,pat.data(),Bytes)!=0)++bad;
                t[n]=uint16_t(k);                      // privatizes while shared
                if(t[n]!=uint16_t(k))++bad;
                Release(t);
            }
        });
        go=true;for(auto& w:ws)w.join();stop=true;ev.join();
        assert(bad==0);
        assert(shares>0);   // the race must actually share, not just allocate
        assert(memcmp(src,pat.data(),Bytes)==0);assert(Release(src));
        auto sc=Snapshot();
        assert(sc.live==0 && sc.sharedSlots==0 && sc.failures==base.failures);
        GetProcessHandleCount(GetCurrentProcess(),&h1);assert(h1==h0);
        printf("cow sharing: shares=%u privatized=%llu failures=%llu\n",
               shares.load(),sc.privatizations,sc.failures);
    }
    // Content probe: identical tiles are counted whether resident or cold, the
    // all-zero (never written) group is reported on its own, and a slot that is
    // mid-encode is skipped rather than touched.
    {
        auto base=Snapshot();
        std::vector<uint16_t> patA(Samples),patB(Samples);
        for(size_t k=0;k<Samples;++k){patA[k]=uint16_t(k*3+11);patB[k]=uint16_t(k/7+900);}
        auto a1=Allocate(),a2=Allocate(),a3=Allocate(),b1=Allocate(),b2=Allocate(),z1=Allocate(),z2=Allocate(),u=Allocate();
        assert(a1&&a2&&a3&&b1&&b2&&z1&&z2&&u);
        memcpy(a1,patA.data(),Bytes);memcpy(a2,patA.data(),Bytes);memcpy(a3,patA.data(),Bytes);
        memcpy(b1,patB.data(),Bytes);memcpy(b2,patB.data(),Bytes);
        for(size_t k=0;k<Samples;++k)u[k]=uint16_t(rng());
        assert(Evict(Index(a2),true));assert(Evict(Index(b2),true));   // cold: the blob's hash
        assert(b1[0]==patB[0]);                                          // restored read-only: still the blob's hash
        assert(Evict(Index(b1),true));assert(b1[1]==patB[1]);
        ProbeResult r{};assert(Probe(&r));
        assert(r.live==base.live+8 && r.skipped==0);
        assert(r.hashedPacked==2+1 && r.hashedResident==5);
        assert(r.distinct==4 && r.duplicated==4 && r.zero==2 && r.pairs==2 && r.largestGroup==3);
        // A slot whose encode is in flight is skipped, not read.
        {Guard g;slots[Index(u)].evicting=true;}
        assert(Probe(&r) && r.skipped==1 && r.hashedResident==4 && r.distinct==3);
        {Guard g;slots[Index(u)].evicting=false;}
        for(auto t:{a1,a2,a3,b1,b2,z1,z2,u})assert(Release(t));
        printf("content probe: distinct=%llu duplicated=%llu zero=%llu ms=%llu\n",r.distinct,r.duplicated,r.zero,r.ms);
    }
    // Content dedup: the second eviction of identical bytes shares the first
    // blob (no encode, no extra compressed bytes); a write to one twin gives it
    // private bytes and leaves the other on the blob; the blob leaves the index
    // with its last owner, so the same bytes encode afresh afterwards.
    {
        assert(EnableDedup());
        std::vector<uint16_t> pat(Samples);
        for(size_t k=0;k<Samples;++k)pat[k]=uint16_t(k*5+7);
        auto base=Snapshot();
        auto a=Allocate(),b=Allocate();assert(a&&b);
        memcpy(a,pat.data(),Bytes);memcpy(b,pat.data(),Bytes);
        assert(Evict(Index(a),true));
        auto s1=Snapshot();assert(s1.encodes==base.encodes+1 && s1.dedupHits==base.dedupHits);
        assert(Evict(Index(b),true));
        auto s2=Snapshot();
        assert(s2.encodes==s1.encodes && s2.dedupHits==base.dedupHits+1);
        assert(s2.compressedBytes==s1.compressedBytes && s2.resident==s1.resident-1);
        {Guard g;assert(slots[Index(a)].packed==slots[Index(b)].packed && slots[Index(a)].packed->refs==2);}
        assert(memcmp(a,pat.data(),Bytes)==0 && memcmp(b,pat.data(),Bytes)==0);   // reads restore, still shared
        b[3]=0x5555;                                                              // a write privatizes b
        {Guard g;assert(!slots[Index(b)].packed && slots[Index(a)].packed && slots[Index(a)].packed->refs==1);}
        assert(a[3]==pat[3]);
        // a is resident, read-only and packed: its re-eviction reuses the blob.
        assert(Evict(Index(a),true));assert(Snapshot().reusedEvictions==s2.reusedEvictions+1);
        // b's bytes now differ: a fresh encode.
        assert(Evict(Index(b),true));assert(Snapshot().dedupHits==s2.dedupHits && Snapshot().encodes==s2.encodes+1);
        // A third tile with a's bytes hits a's blob.
        auto c=Allocate();assert(c);memcpy(c,pat.data(),Bytes);
        assert(Evict(Index(c),true));assert(Snapshot().dedupHits==s2.dedupHits+1);
        assert(memcmp(c,pat.data(),Bytes)==0);
        assert(Release(a));assert(Release(b));assert(Release(c));
        // The blob went with its last owner: the same bytes encode again.
        auto d=Allocate();assert(d);memcpy(d,pat.data(),Bytes);
        auto s3=Snapshot();assert(Evict(Index(d),true));
        assert(Snapshot().dedupHits==s3.dedupHits && Snapshot().encodes==s3.encodes+1);
        assert(memcmp(d,pat.data(),Bytes)==0);assert(Release(d));
        // A rebuild indexes every stored blob exactly once, shared ones included.
        auto e=Allocate(),f=Allocate();assert(e&&f);memcpy(e,pat.data(),Bytes);memcpy(f,pat.data(),Bytes);
        assert(Evict(Index(e),true) && Evict(Index(f),true));
        {Guard g;size_t used=dedupUsed;assert(used==1);DedupRebuild();assert(dedupUsed==used && dedupTombstones==0);}
        assert(memcmp(e,pat.data(),Bytes)==0 && memcmp(f,pat.data(),Bytes)==0);
        assert(Release(e));assert(Release(f));
        // Eight writers allocating twins, evicting, reading, writing and
        // releasing, against a forced evictor: every read sees its own bytes.
        std::atomic<bool> go{false},stop{false};std::atomic<unsigned> bad{0};
        std::thread ev([&]{while(!go)SwitchToThread();
            while(!stop){unsigned n;{Guard gg;n=allocated;}for(unsigned i=0;i<n;++i)Evict(i,true);}});
        std::vector<std::thread> ws;
        for(unsigned n=0;n<8;++n)ws.emplace_back([&,n]{
            while(!go)SwitchToThread();
            for(unsigned k=0;k<300;++k) {
                auto t=Allocate();if(!t)continue;
                memcpy(t,pat.data(),Bytes);
                Evict(Index(t),true);
                if(memcmp(t,pat.data(),Bytes)!=0)++bad;
                t[n]=uint16_t(k);if(t[n]!=uint16_t(k))++bad;
                if(k&1){Evict(Index(t),true);if(t[n]!=uint16_t(k))++bad;}
                Release(t);
            }
        });
        go=true;for(auto& w:ws)w.join();stop=true;ev.join();
        assert(bad==0);
        auto sd=Snapshot();assert(sd.live==0 && sd.compressedBytes==0 && sd.failures==base.failures);
        printf("content dedup: hits=%llu encodes=%llu rebuilds=%llu\n",sd.dedupHits-base.dedupHits,sd.encodes-base.encodes,sd.dedupRebuilds);
    }
    // No section at the commit limit: a restore waits for one instead of
    // handing the access violation back to the engine.
    {
        auto base=Snapshot();
        std::vector<uint16_t> pat(Samples);
        for(size_t k=0;k<Samples;++k)pat[k]=uint16_t(k%1000+20000);
        auto a=Allocate();assert(a);memcpy(a,pat.data(),Bytes);
        assert(Evict(Index(a),true));
        InterlockedExchange(&injectSectionFailures,3);
        assert(memcmp(a,pat.data(),Bytes)==0);                  // cold read: three refusals, then a section
        auto s1=Snapshot();
        assert(s1.restoreRetries==base.restoreRetries+3 && s1.failures==base.failures+3 && s1.restoreGiveUps==base.restoreGiveUps);
        assert(Evict(Index(a),true));
        InterlockedExchange(&injectSectionFailures,2);
        a[7]=0x4242;                                              // cold write, same wait
        assert(a[7]==0x4242 && a[8]==pat[8]);
        assert(Snapshot().restoreRetries==s1.restoreRetries+2);
        assert(injectSectionFailures==0);
        assert(Release(a));
        printf("restore retry: retries=%llu giveups=%llu\n",Snapshot().restoreRetries-base.restoreRetries,Snapshot().restoreGiveUps);
        {Guard g;stats.failures=base.failures;}   // the injected refusals are not pager failures
    }
    // Eviction rate limit: outside loading a Tick pass stops at the per-second
    // allowance, over three times the budget too; only a tight commit charge
    // (SetUrgent) lifts it.
    {
        std::vector<uint16_t*> t(16);
        for(auto& x:t){x=Allocate();assert(x);}
        SetBudget(8*SlotBytes);                        // 16 resident, 8 allowed: excess 8, no pressure
        {Guard g;auto now=GetTickCount64();stats.lastBulkAllocation=0;for(auto x:t)slots[Index(x)].touched=now-6000;rateWindow=0;}
        SetEvictRate(3);
        auto b=Snapshot();
        Tick();                                        // soft-blocks count against the allowance
        auto s1=Snapshot();assert(s1.softBlocked==3 && s1.rateLimited==b.rateLimited+1);
        Tick();assert(Snapshot().softBlocked==3);      // same second: nothing more
        {Guard g;rateWindow=GetTickCount64()-1001;}    // next second
        Tick();assert(Snapshot().softBlocked==6);
        SetBudget(4*SlotBytes);                        // 16 > 12: over three times the budget
        {Guard g;rateWindow=GetTickCount64()-1001;}
        Tick();assert(Snapshot().resident==13);        // still three per second
        SetUrgent(true);
        {Guard g;rateWindow=GetTickCount64()-1001;}
        // Urgent encodes straight down to three times the budget (12) with no
        // allowance; the rest is only second-chanced.
        Tick();assert(Snapshot().resident==12 && Snapshot().softBlocked==8);
        SetUrgent(false);
        SetEvictRate(0);
        for(auto x:t)assert(Release(x));
        auto se=Snapshot();assert(se.evictOps>b.evictOps && se.evictMicros>b.evictMicros);   // every eviction and soft block is timed
        printf("evict rate: limited passes=%llu, %llu us per eviction\n",se.rateLimited-b.rateLimited,se.evictMicros/se.evictOps);
    }
    // Lazy zero allocations: no section until the first touch; a read gets a
    // read-only zero section on the shared blob, a write a private one; an
    // untouched slot releases without ever having had a section.
    {
        assert(EnableLazyZero());
        auto base=Snapshot();
        auto a=Allocate();assert(a);
        auto s1=Snapshot();assert(s1.resident==base.resident && s1.live==base.live+1 && s1.lazyAllocations==base.lazyAllocations+1);
        {Guard g;assert(slots[Index(a)].packed==zeroBlob && !slots[Index(a)].section);}
        for(size_t k=0;k<Samples;k+=257)assert(a[k]==0);           // read: section appears, still on the blob
        assert(Snapshot().resident==base.resident+1);
        {Guard g;assert(slots[Index(a)].packed==zeroBlob && slots[Index(a)].readOnly);}
        a[5]=1234;assert(a[5]==1234 && a[6]==0);                    // write: private
        {Guard g;assert(!slots[Index(a)].packed);}
        assert(Evict(Index(a),true));assert(a[5]==1234);            // ordinary from here
        assert(Release(a));
        auto b=Allocate();assert(b);assert(Release(b));            // never touched: no section, no failure
        auto c=Allocate();assert(c);c[0]=7;assert(c[0]==7 && c[Samples-1]==0);   // write first
        assert(Snapshot().resident==base.resident+1);assert(Release(c));
        // An untouched tile evicted... cannot be: not resident. A touched-but-
        // still-zero tile evicts as a dedup hit on the blob.
        auto d=Allocate();assert(d);assert(d[0]==0);
        auto s2=Snapshot();assert(Evict(Index(d),true));
        assert(Snapshot().reusedEvictions==s2.reusedEvictions+1);   // read-only on the blob: reused, no encode
        assert(d[1]==0);assert(Release(d));
        SetLazyZero(false);
        auto e=Allocate();assert(e);assert(Snapshot().resident==base.resident+1);assert(Release(e));
        auto sd=Snapshot();assert(sd.live==base.live && sd.resident==base.resident && sd.failures==base.failures);
        printf("lazy zero: %llu lazy allocations\n",sd.lazyAllocations-base.lazyAllocations);
    }
    // Backpressure: with the throttle on and the pool over budget, a fault that
    // needs a new section waits (bounded), then proceeds; slots that already
    // have a section do not wait; the throttle off means no wait at all.
    {
        assert(EnableLazyZero());
        auto base=Snapshot();
        auto a=Allocate();assert(a);
        SetBudget(0);SetThrottle(true);
        {Guard g;assert(stats.resident*SlotBytes>budget || stats.resident==0);}
        // resident may be 0 here (nothing over budget): make one resident slot first
        auto r=Allocate();assert(r);r[0]=1;   // this one may or may not wait; the next must
        auto t0=GetTickCount64();a[0]=5;auto dt=GetTickCount64()-t0;
        assert(a[0]==5 && Snapshot().throttleWaits>=base.throttleWaits+1 && dt>=ThrottleMaxMs-100);
        volatile auto again=a[1];(void)again;   // already has a section: no wait
        auto w=Snapshot().throttleWaits;
        assert(Evict(Index(r),true));            // cold again
        SetThrottle(false);
        t0=GetTickCount64();r[1]=2;assert(r[1]==2 && GetTickCount64()-t0<500);   // throttle off: immediate
        assert(Snapshot().throttleWaits==w);
        SetBudget(4*SlotBytes);SetLazyZero(false);
        assert(Release(a));assert(Release(r));
        printf("throttle: waited %llu ms\n",Snapshot().throttleMillis);
    }
    // Scale up together to exercise placeholder splitting, O(1) lookup and
    // complete release of both mapped and compressed backing at world teardown.
    DWORD beforeHandles=0,afterHandles=0;
    GetProcessHandleCount(GetCurrentProcess(),&beforeHandles);
    std::vector<uint16_t*> many(4096);
    for(auto& t:many){t=Allocate();assert(t);memcpy(t,expected.data(),Bytes);}
    for(auto t:many){Evict(Index(t),true);assert(memcmp(t,expected.data(),Bytes)==0);Evict(Index(t),true);}
    for(auto t:many)assert(Release(t));
    GetProcessHandleCount(GetCurrentProcess(),&afterHandles);assert(beforeHandles==afterHandles);
    // The zero blob stays for the pager's lifetime; everything else is gone.
    auto s=Snapshot();assert(s.live==0&&s.resident==0&&s.compressedBytes==zeroBlob->bytes&&s.compressedCommit==zeroBlob->commit&&s.failures==0);
    printf("PASS: exact roundtrips, write faults, address reuse, incompressible fallback, "
           "%u concurrent writes; faults=%llu evictions=%llu failures=%llu\n",
           Threads*Iterations,s.faults,s.evictions,s.failures);
}
