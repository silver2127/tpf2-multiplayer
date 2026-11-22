// Real reserved memory, real access violations, real decommits: the small
// pager and its variable-length codec, isolated from the game.
#include "../src/small_pager.h"
#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <cassert>

static std::vector<uint16_t> Pattern(size_t n,uint32_t seed) {
    std::vector<uint16_t> v(n);
    std::mt19937 rng(seed);
    uint16_t h=uint16_t(20000+rng()%1000);
    for(size_t i=0;i<n;++i){int d=int(rng()%7)-3;h=uint16_t(h+d);v[i]=h;}   // terrain-like: small steps
    return v;
}

int main() {
    using namespace SmallPager;
    // ---- codec: sizes from 1 to a full tile, terrain-like and random ----
    {
        auto enc=new BlockCodec::EncodeScratch;auto dec=new BlockCodec::DecodeScratch;
        std::vector<uint8_t> blob(BlockCodec::MaxSamples*2+256);
        size_t ratioNum=0,ratioDen=0;
        for(size_t n:{1u,2u,3u,7u,255u,256u,257u,1000u,4096u,8000u,16384u,66049u,71824u}) {
            auto v=Pattern(n,unsigned(n));
            size_t c=BlockCodec::Encode(v.data(),n,blob.data(),blob.size(),*enc);
            assert(c && c<n*2+64);
            std::vector<uint16_t> out(n,0x5a5a);
            assert(BlockCodec::Decode(blob.data(),c,out.data(),n,*dec) && out==v);
            assert(!BlockCodec::Decode(blob.data(),c-1,out.data(),n,*dec));        // truncated
            assert(!BlockCodec::Decode(blob.data(),c,out.data(),n+1,*dec));        // wrong count
            blob[c/2]^=0x40;assert(!BlockCodec::Decode(blob.data(),c,out.data(),n,*dec));blob[c/2]^=0x40;   // corrupted
            if(n>=1000){ratioNum+=c;ratioDen+=n*2;}
        }
        assert(BlockCodec::Encode(nullptr,0,blob.data(),blob.size(),*enc)==0);
        assert(BlockCodec::Encode(blob.data()?reinterpret_cast<uint16_t*>(blob.data()):nullptr,BlockCodec::MaxSamples+1,blob.data(),blob.size(),*enc)==0);
        // Escapes: big jumps every few samples.
        std::vector<uint16_t> jumpy(5000);for(size_t i=0;i<jumpy.size();++i)jumpy[i]=uint16_t((i%9)?i*3:i*3000);
        size_t c=BlockCodec::Encode(jumpy.data(),jumpy.size(),blob.data(),blob.size(),*enc);assert(c);
        std::vector<uint16_t> out(jumpy.size());assert(BlockCodec::Decode(blob.data(),c,out.data(),jumpy.size(),*dec)&&out==jumpy);
        // Incompressible: random does not fit under 95%.
        std::vector<uint16_t> rnd(8000);std::mt19937 rng(7);for(auto& x:rnd)x=uint16_t(rng());
        assert(BlockCodec::Encode(rnd.data(),rnd.size(),blob.data(),rnd.size()*2*95/100,*enc)==0);
        printf("codec: terrain-like ratio %.1f%%\n",100.0*ratioNum/ratioDen);
        delete enc;delete dec;
    }
    assert(Init(1ull<<20));
    // ---- lazy allocation, first touch, release, span recycling ----
    {
        auto b=Snapshot();
        auto p=Allocate(8000);assert(p && uintptr_t(p)%32==0);
        auto s1=Snapshot();assert(s1.live==b.live+1 && s1.lazy==b.lazy+1 && s1.resident==b.resident && s1.residentBytes==b.residentBytes);
        assert(p[0]==0 && p[7999]==0);                     // first touch commits zero pages
        auto s2=Snapshot();assert(s2.resident==b.resident+1 && s2.lazy==b.lazy && s2.residentBytes==b.residentBytes+4*PageSize && s2.commits==b.commits+1);
        assert(*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(p)-8)==reinterpret_cast<uint8_t*>(p)-Header);   // aligned-alloc header
        assert(Capacity(p)==(4*PageSize-Header)/2);
        p[3]=77;assert(p[3]==77);
        assert(Release(reinterpret_cast<uint8_t*>(p)-Header));   // by raw base, as the aligned delete passes
        auto s3=Snapshot();assert(s3.live==b.live && s3.resident==b.resident && s3.residentBytes==b.residentBytes);
        auto q=Allocate(8000);assert(q==p);                // same page count: the span is recycled
        assert(q[3]==0);                                   // and comes back zero
        assert(Release(q));
        auto u=Allocate(100);assert(u);assert(Release(u)); // never touched: no commit, no failure
        assert(!Release(reinterpret_cast<uint8_t*>(p)+1));  // released: not live
        assert(Snapshot().failures==b.failures);
    }
    // ---- evict, restore, write during evict, release while cold ----
    {
        auto b=Snapshot();
        auto v=Pattern(20000,3);
        auto p=Allocate(v.size());assert(p);memcpy(p,v.data(),v.size()*2);
        uint32_t idx=0;{Guard g;idx=uint32_t(Find(p)-records);}
        assert(Evict(idx,true));
        auto s1=Snapshot();assert(s1.cold==b.cold+1 && s1.resident==b.resident && s1.evictions==b.evictions+1 && s1.compressedBytes>0);
        assert(memcmp(p,v.data(),v.size()*2)==0);          // fault: commit + decode
        auto s2=Snapshot();assert(s2.restores==b.restores+1 && s2.cold==b.cold && s2.resident==b.resident+1 && s2.compressedBytes==0);
        p[10]=1;v[10]=1;
        assert(Evict(idx,true));p[11]=2;v[11]=2;           // write fault on a cold span
        assert(memcmp(p,v.data(),v.size()*2)==0);
        assert(Evict(idx,true));assert(Release(p));       // release while cold frees the blob
        auto s3=Snapshot();assert(s3.live==b.live && s3.cold==b.cold && s3.compressedBytes==0 && s3.failures==b.failures);
        // Incompressible stays resident and sticky.
        std::vector<uint16_t> rnd(8000);std::mt19937 rng(9);for(auto& x:rnd)x=uint16_t(rng());
        auto r=Allocate(rnd.size());memcpy(r,rnd.data(),rnd.size()*2);
        {Guard g;idx=uint32_t(Find(r)-records);}
        assert(!Evict(idx,true) && Snapshot().incompressible==b.incompressible+1);
        assert(memcmp(r,rnd.data(),rnd.size()*2)==0);assert(Release(r));
    }
    // ---- Tick policy: ring order, budget, age ----
    {
        auto b=Snapshot();
        std::vector<uint16_t*> t(16);
        for(size_t k=0;k<t.size();++k){t[k]=Allocate(4000);assert(t[k]);auto v=Pattern(4000,unsigned(k));memcpy(t[k],v.data(),8000);}
        SetBudget(2*2*PageSize);                             // 16 x 2 pages resident, 2 allowed
        {Guard g;stats.lastBulkAllocation=0;}
        Tick();assert(Snapshot().resident==b.resident+16);    // all younger than MinAgeMs: nothing
        {Guard g;for(auto x:t)Find(x)->touched=GetTickCount64()-MinAgeMs-1;}
        Tick();auto s=Snapshot();assert(s.resident==b.resident+2 && s.cold==b.cold+14);
        for(size_t k=0;k<t.size();++k){auto v=Pattern(4000,unsigned(k));assert(memcmp(t[k],v.data(),8000)==0);}   // restores
        for(auto x:t)assert(Release(x));
        SetBudget(1ull<<20);
    }
    // ---- throttle: a commit waits (bounded) while over budget and tight ----
    {
        auto b=Snapshot();
        auto keep=Allocate(4000);assert(keep);keep[0]=1;   // one resident span
        SetBudget(0);SetThrottle(true);
        auto p=Allocate(4000);assert(p);
        auto t0=GetTickCount64();p[0]=5;auto dt=GetTickCount64()-t0;
        assert(p[0]==5 && dt>=ThrottleMaxMs-100 && Snapshot().throttleWaits==b.throttleWaits+1);
        SetThrottle(false);SetBudget(1ull<<20);
        assert(Release(p));assert(Release(keep));
    }
    // ---- concurrency: writers own spans, an evictor forces evictions ----
    {
        auto b=Snapshot();
        std::atomic<bool> go{false},stop{false};std::atomic<unsigned> bad{0};
        std::thread ev([&]{while(!go)SwitchToThread();while(!stop){uint32_t n;{Guard g;n=used;}for(uint32_t i=0;i<n;++i)Evict(i,true);}});
        std::vector<std::thread> ws;
        for(unsigned w=0;w<8;++w)ws.emplace_back([&,w]{
            while(!go)SwitchToThread();
            std::mt19937 rng(w);
            for(unsigned k=0;k<400;++k) {
                size_t n=200+rng()%30000;
                auto v=Pattern(n,k*8+w);
                auto p=Allocate(n);if(!p){++bad;continue;}
                memcpy(p,v.data(),n*2);
                for(int r=0;r<3;++r){if(memcmp(p,v.data(),n*2)!=0)++bad;p[r]=uint16_t(k);v[r]=uint16_t(k);}
                if(memcmp(p,v.data(),n*2)!=0)++bad;
                if(!Release(p))++bad;
            }
        });
        go=true;for(auto& t:ws)t.join();stop=true;ev.join();
        assert(bad==0);
        auto s=Snapshot();assert(s.live==b.live && s.resident==b.resident && s.cold==b.cold && s.compressedBytes==0 && s.failures==b.failures);
        printf("concurrency: evictions=%llu restores=%llu cancelled=%llu\n",s.evictions-b.evictions,s.restores-b.restores,s.cancelled-b.cancelled);
    }
    auto s=Snapshot();
    printf("PASS: codec, lazy spans, recycling, evict/restore/decommit, sticky, ring policy, throttle, 8 writers vs evictor; faults=%llu failures=%llu\n",s.faults,s.failures);
    return 0;
}
