// Linux userfaultfd backend for the shared Windows lossless terrain codec.
// No signal handlers. Writers are stopped by UFFD write protection while a
// tile is encoded; readers fault back to identical bytes at the same address.
#pragma once
#include "../src/terrain_codec.h"
#include "memory_budget.h"
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <memory>
#include <vector>
#include <cerrno>
#include <cstdlib>
// Stable Linux UAPI definitions absent from the soldier SDK headers.
#ifndef UFFDIO_WRITEPROTECT
struct uffdio_writeprotect { struct uffdio_range range; uint64_t mode; };
#define UFFDIO_WRITEPROTECT _IOWR(UFFDIO, 0x06, struct uffdio_writeprotect)
#define UFFDIO_WRITEPROTECT_MODE_WP (1ULL << 0)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFDIO_COPY_MODE_WP
#define UFFDIO_COPY_MODE_WP (1ULL << 1)
#endif
namespace linux_pager {
// userfaultfd cannot see ordinary resident reads/writes. Recency measures
// serviced faults, not true LRU. Rapid refaults receive a longer grace period.
struct Recency {
    uint64_t touched=0,lastFault=0,protectUntil=0;
    void Reset(uint64_t now){touched=now;lastFault=0;protectUntil=now+2000;}
    void Fault(uint64_t now){
        protectUntil=now+((lastFault && now-lastFault<5000)?10000:2000);
        touched=lastFault=now;
    }
    bool Eligible(uint64_t now)const{return now>=protectUntil;}
};
class TerrainPager {
public:
    static constexpr size_t Bytes=TerrainCodec::RawBytes, Stride=(Bytes+4095)&~size_t(4095);
    struct Stats {uint64_t live,resident,packed,faults,evictions,refusals,budget;};
private:
    struct Slot {std::mutex lock;bool active=false,cold=false;void* packed=nullptr;size_t packedSize=0,packedMap=0;Recency age;};
    int fd=-1;uint8_t* base=nullptr;size_t count=0;unsigned next=0;std::atomic<unsigned> highWater{0};
    std::unique_ptr<Slot[]> slots;std::mutex poolLock;std::vector<unsigned> free;
    std::thread handler,policy;std::atomic<bool> stop{false};
    std::atomic<uint64_t> live{0},resident{0},packedBytes{0},faults{0},evictions{0},refusals{0};
    std::atomic<size_t> budget{0};bool automatic=false;size_t fallbackBudget=0;
    static uint64_t Now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    uint8_t* Address(size_t i){return base+i*Stride;}
    static void Fatal(){ssize_t ignored=write(2,"tpf2_bigmap: terrain pager invariant failed\n",42);(void)ignored;abort();}
    void Check(bool ok){if(!ok)Fatal();}
    void Protect(unsigned i,bool on){uffdio_writeprotect wp{};wp.range={uintptr_t(Address(i)),Stride};wp.mode=on?UFFDIO_WRITEPROTECT_MODE_WP:0;Check(ioctl(fd,UFFDIO_WRITEPROTECT,&wp)==0);}
    void Wake(unsigned i){uffdio_range range{uintptr_t(Address(i)),Stride};Check(ioctl(fd,UFFDIO_WAKE,&range)==0);}
    void DropBlob(Slot& s){if(s.packed){packedBytes-=s.packedMap;munmap(s.packed,s.packedMap);s.packed=nullptr;s.packedSize=s.packedMap=0;}}
    void Serve(){
        auto scratch=std::unique_ptr<TerrainCodec::DecodeScratch>(new TerrainCodec::DecodeScratch);
        void* decoded=mmap(nullptr,Stride,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);Check(decoded!=MAP_FAILED);
        pollfd p{fd,POLLIN,0};
        while(!stop){
            int r=poll(&p,1,100);if(r<0 && errno==EINTR)continue;Check(r>=0 && !(p.revents&(POLLERR|POLLHUP|POLLNVAL)));if(!r)continue;
            uffd_msg msg{};ssize_t n=read(fd,&msg,sizeof(msg));if(n<0 && (errno==EAGAIN || errno==EINTR))continue;
            Check(n==sizeof(msg) && msg.event==UFFD_EVENT_PAGEFAULT);
            unsigned i=unsigned((msg.arg.pagefault.address-uintptr_t(base))/Stride);Check(i<count);
            auto& s=slots[i];std::lock_guard<std::mutex> lock(s.lock);Check(s.active);++faults;
            if(s.cold){
                Check(TerrainCodec::Decode(static_cast<uint8_t*>(s.packed),s.packedSize,static_cast<uint16_t*>(decoded),*scratch));
                uffdio_copy copy{};copy.src=uintptr_t(decoded);copy.dst=uintptr_t(Address(i));copy.len=Stride;copy.mode=UFFDIO_COPY_MODE_DONTWAKE|UFFDIO_COPY_MODE_WP;
                Check(ioctl(fd,UFFDIO_COPY,&copy)==0 && copy.copy==ssize_t(Stride));
                s.cold=false;++resident;Protect(i,false);DropBlob(s);
            }
            s.age.Fault(Now());Wake(i);
        }
        munmap(decoded,Stride);
    }
    void Policy(){
        auto scratch=std::unique_ptr<TerrainCodec::EncodeScratch>(new TerrainCodec::EncodeScratch);
        auto encoded=std::unique_ptr<uint8_t[]>(new uint8_t[Bytes]);
        std::vector<std::pair<uint64_t,unsigned>> candidates;
        const auto physical=linux_memory::PhysicalBytes();
        uint64_t sampled=0;
        while(!stop){
            const auto now=Now();
            if(automatic && now-sampled>=1000){
                budget=linux_memory::TerrainTarget(physical,linux_memory::AvailableBytes(),
                    resident*Stride,live*Stride,fallbackBudget);sampled=now;
            }
            candidates.clear();
            if(resident*Stride>budget){
                for(unsigned i=0,end=highWater.load();i<end;++i){
                    auto& s=slots[i];std::unique_lock<std::mutex> lock(s.lock,std::try_to_lock);
                    if(lock && s.active && !s.cold && s.age.Eligible(now))
                        candidates.emplace_back(s.age.touched,i);
                }
                std::sort(candidates.begin(),candidates.end());
            }
            unsigned work=0;
            for(const auto& candidate:candidates){
                if(stop || resident*Stride<=budget || work>=256)break;
                unsigned i=candidate.second;auto& s=slots[i];
                std::unique_lock<std::mutex> lock(s.lock,std::try_to_lock);
                // Revalidate after sorting: a restore or slot reuse may intervene.
                if(!lock || !s.active || s.cold || !s.age.Eligible(Now()) ||
                   s.age.touched!=candidate.first)continue;
                ++work;
                Protect(i,true);
                size_t n=TerrainCodec::Encode(reinterpret_cast<uint16_t*>(Address(i)),encoded.get(),Bytes*95/100,*scratch);
                if(!n){Protect(i,false);s.age.Reset(Now());++refusals;continue;}
                size_t len=(n+4095)&~size_t(4095);void* blob=mmap(nullptr,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                if(blob==MAP_FAILED){Protect(i,false);s.age.Reset(Now());++refusals;continue;}
                memcpy(blob,encoded.get(),n);s.packed=blob;s.packedSize=n;s.packedMap=len;packedBytes+=len;
                Check(madvise(Address(i),Stride,MADV_DONTNEED)==0);s.cold=true;--resident;++evictions;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
public:
    bool Start(size_t capacity,size_t hotBytes,bool autoBudget=false){
        if(!capacity || capacity>(1u<<20) || fd>=0)return false;
        fd=int(syscall(SYS_userfaultfd,O_CLOEXEC|O_NONBLOCK|UFFD_USER_MODE_ONLY));if(fd<0)return false;
        uffdio_api api{};api.api=UFFD_API;api.features=UFFD_FEATURE_PAGEFAULT_FLAG_WP;
        if(ioctl(fd,UFFDIO_API,&api)){close(fd);fd=-1;return false;}
        base=static_cast<uint8_t*>(mmap(nullptr,capacity*Stride,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0));
        if(base==MAP_FAILED){base=nullptr;close(fd);fd=-1;return false;}
        uffdio_register reg{};reg.range={uintptr_t(base),capacity*Stride};reg.mode=UFFDIO_REGISTER_MODE_MISSING|UFFDIO_REGISTER_MODE_WP;
        if(ioctl(fd,UFFDIO_REGISTER,&reg)){munmap(base,capacity*Stride);base=nullptr;close(fd);fd=-1;return false;}
        count=capacity;budget=hotBytes;fallbackBudget=hotBytes;automatic=autoBudget;slots.reset(new Slot[count]);
        handler=std::thread([this]{Serve();});policy=std::thread([this]{Policy();});return true;
    }
    bool Contains(const void* p)const{return base && uintptr_t(p)>=uintptr_t(base) && uintptr_t(p)<uintptr_t(base)+count*Stride;}
    uint16_t* Allocate(){
        unsigned i;
        {std::lock_guard<std::mutex> lock(poolLock);if(!free.empty()){i=free.back();free.pop_back();}else if(next<count){i=next++;highWater=next;}else{++refusals;return nullptr;}}
        auto& s=slots[i];std::lock_guard<std::mutex> lock(s.lock);Check(!s.active);s.active=true;s.cold=false;s.age.Reset(Now());
        uffdio_zeropage zero{};zero.range={uintptr_t(Address(i)),Stride};Check(ioctl(fd,UFFDIO_ZEROPAGE,&zero)==0 && zero.zeropage==ssize_t(Stride));
        ++live;++resident;return reinterpret_cast<uint16_t*>(Address(i));
    }
    bool Release(void* p){
        if(!Contains(p))return false;
        unsigned i=unsigned((uintptr_t(p)-uintptr_t(base))/Stride);Check(p==Address(i));
        {auto& s=slots[i];std::lock_guard<std::mutex> lock(s.lock);Check(s.active);s.active=false;--live;if(!s.cold)--resident;DropBlob(s);Check(madvise(Address(i),Stride,MADV_DONTNEED)==0);}
        std::lock_guard<std::mutex> lock(poolLock);free.push_back(i);return true;
    }
    Stats Get()const{return {live.load(),resident.load(),packedBytes.load(),faults.load(),evictions.load(),refusals.load(),budget.load()};}
    ~TerrainPager(){stop=true;if(policy.joinable())policy.join();if(handler.joinable())handler.join();if(fd>=0)close(fd);if(base)munmap(base,count*Stride);if(slots)for(size_t i=0;i<count;++i)DropBlob(slots[i]);}
};
}
