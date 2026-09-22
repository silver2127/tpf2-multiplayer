// Fixed-address, lossless, fault-restored pager for VARIABLE-length uint16
// blocks: the terrain alignment pass's per-region result and work vectors.
//
// MEASURED 2026-09-17 (LONGBOI, 207,360 tiles): a save load resizes 3.3
// million alignment result vectors (one per road or track piece per tile it
// overlaps, ~16 KB on average) and constructs ~10 million work vectors, and
// holds them all until the publication pass has copied each into its tile.
// That is the game's own 34 GiB private peak, and it is written once and read
// once. The fixed-size tile pager cannot take them (a 16 KB block in a 132 KB
// slot would cost eight times the commit), so this one works in 4 KiB pages:
//
// - The arena is one reserved range (no commit). An allocation is a span of
//   whole pages holding a 32-byte header (the raw pointer at [-8], as the
//   MSVC aligned allocator lays it out) and the data at +32; the span is
//   handed out RESERVED ONLY ("lazy"): the first touch commits it, zero.
// - Eviction protects the span read-only, encodes it (small_codec.h) outside
//   the lock, then DECOMMITS the pages: the commit charge goes back to the OS.
//   A write during the encode faults, cancels it, and the span stays.
// - A fault on a decommitted span commits it again and decodes into it. Reads
//   and writes by the engine continue at the same address as always.
// - Release (through the CRT free import) decommits and recycles the span in
//   a free list of its page count.
// - Resident spans queue in commit order in a ring; Tick evicts from the old
//   end while the pool is over budget, so a sweep never walks cold records.
//
// One SRW lock guards the records; encodes and decodes run outside it with
// per-thread scratch. The fault path never calls the game or its allocator.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "small_codec.h"

namespace SmallPager {
constexpr size_t PageSize=4096, Header=32;
constexpr uint64_t ReserveBytes=64ull<<30;             // address space only
constexpr unsigned MaxPages=unsigned(ReserveBytes/PageSize), MaxObjects=8u<<20, MaxSpanPages=64;
constexpr unsigned RingMask=MaxObjects-1;
constexpr size_t MaxSamples=BlockCodec::MaxSamples;
constexpr uint64_t MinAgeMs=2000, LoadingMinAgeMs=500;
constexpr unsigned ThrottleMaxMs=2000, CommitRetries=500;
enum State : uint8_t { Free=0, Lazy, Resident, Cold };
struct Packed { unsigned bytes; size_t commit; uint8_t data[1]; };
struct Record {
    uint32_t page, pages, samples, next;    // next: free list of this page count
    Packed* packed;
    uint64_t touched;
    unsigned generation;
    State state;
    bool evicting, cancel, restoring, sticky;   // sticky: incompressible, do not retry
};
struct Stats {
    uint64_t live, lazy, resident, cold;      // objects by state
    uint64_t residentBytes, compressedBytes;
    uint64_t faults, commits, restores, evictions, releases, failures, incompressible, cancelled, commitRetries;
    uint64_t throttleWaits, throttleMillis, overflows, ringDrops;
    uint64_t lastBulkAllocation;
};
static SRWLOCK lock=SRWLOCK_INIT;
static uint8_t* arena=nullptr;
static Record* records=nullptr;
static uint32_t* owner=nullptr;         // page -> record index + 1
static uint32_t* ring=nullptr;          // resident records in commit order
static uint64_t ringHead=0, ringTail=0;
static uint32_t freeList[MaxSpanPages+1];
static uint32_t used=0, nextPage=0;
static Stats stats{};
static size_t budget=1ull<<30;
static volatile LONG throttle=0;
static HANDLE packHeap=nullptr;
static void* veh=nullptr;
static bool ready=false;
static uint64_t allocationWindow=0; static unsigned windowAllocations=0;
static thread_local bool inFault=false;
static thread_local BlockCodec::EncodeScratch* encScratch=nullptr;
static thread_local BlockCodec::DecodeScratch* decScratch=nullptr;
struct Guard { Guard(){AcquireSRWLockExclusive(&lock);} ~Guard(){ReleaseSRWLockExclusive(&lock);} };

static bool Contains(const void* p){return arena && uintptr_t(p)>=uintptr_t(arena) && uintptr_t(p)-uintptr_t(arena)<ReserveBytes;}
static uint8_t* Span(const Record& r){return arena+size_t(r.page)*PageSize;}
static size_t SpanBytes(const Record& r){return size_t(r.pages)*PageSize;}
// Caller holds lock. The record owning an address, or nullptr.
static Record* Find(const void* p) {
    if(!Contains(p))return nullptr;
    uint32_t page=uint32_t((uintptr_t(p)-uintptr_t(arena))/PageSize);
    uint32_t o=owner[page];
    if(!o)return nullptr;
    Record& r=records[o-1];
    return r.state!=Free?&r:nullptr;
}
// Caller holds lock.
static void RingPush(uint32_t idx){if(ringTail-ringHead>=MaxObjects){++stats.ringDrops;return;}ring[ringTail&RingMask]=idx;++ringTail;}
static void SetThrottle(bool on){InterlockedExchange(&throttle,on?1:0);}
static void SetBudget(size_t bytes){Guard g;budget=bytes;}
static Stats Snapshot(){Guard g;return stats;}

static bool EnsureDecode(){if(!decScratch)decScratch=static_cast<BlockCodec::DecodeScratch*>(VirtualAlloc(nullptr,sizeof(BlockCodec::DecodeScratch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));return decScratch!=nullptr;}
static bool EnsureEncode(){if(!encScratch)encScratch=static_cast<BlockCodec::EncodeScratch*>(VirtualAlloc(nullptr,sizeof(BlockCodec::EncodeScratch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));return encScratch!=nullptr;}

// Wait (bounded) for evictions while the commit charge is tight and the pool
// is over budget, before committing more. Never called under the lock.
static void ThrottleWait() {
    if(!InterlockedCompareExchange(&throttle,0,0))return;
    unsigned waited=0;bool counted=false;
    while(waited<ThrottleMaxMs && InterlockedCompareExchange(&throttle,0,0)) {
        bool over;{Guard g;over=stats.residentBytes>budget;}
        if(!over)break;
        if(!counted){Guard g;++stats.throttleWaits;counted=true;}
        Sleep(4);waited+=4;
    }
    if(waited){Guard g;stats.throttleMillis+=waited;}
}

// Commit a span read-write (zero pages) and write the aligned-allocation
// header. Retries while the OS refuses commit (the charge at its limit) so
// the evictors can return some. Caller holds NO lock.
static bool CommitSpan(Record& r) {
    void* span=Span(r);
    for(unsigned attempt=0;;++attempt) {
        if(VirtualAlloc(span,SpanBytes(r),MEM_COMMIT,PAGE_READWRITE))break;
        if(attempt>=CommitRetries)return false;
        {Guard g;++stats.commitRetries;}
        Sleep(4);
    }
    *reinterpret_cast<void**>(static_cast<uint8_t*>(span)+Header-8)=span;
    return true;
}

static LONG CALLBACK Fault(EXCEPTION_POINTERS* e) {
    auto x=e->ExceptionRecord;
    if(inFault || x->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || x->NumberParameters<2 ||
       x->ExceptionInformation[0]>1 || !Contains(reinterpret_cast<void*>(x->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD last=GetLastError();inFault=true;
    void* addr=reinterpret_cast<void*>(x->ExceptionInformation[1]);
    bool writing=x->ExceptionInformation[0]==1;
    bool ok=false;
    for(;;) {
        Record* r;unsigned gen=0;State st=Free;Packed* source=nullptr;bool mine=false,settled=false;
        {
            Guard g;
            r=Find(addr);
            if(!r)break;
            gen=r->generation;st=r->state;
            if(!r->restoring) {
                if(st==Resident) {
                    if(r->evicting) {
                        if(!writing){ok=true;settled=true;}   // reads do not fault on read-only pages; defensive
                        else r->cancel=true;                  // the encoder restores access and gives up
                    } else {
                        // Read-only left behind by a cancelled encode, or a stray: make it writable.
                        DWORD old;ok=VirtualProtect(Span(*r),SpanBytes(*r),PAGE_READWRITE,&old)!=0;
                        if(!ok)++stats.failures;
                        ++stats.faults;r->touched=GetTickCount64();settled=true;
                    }
                } else if(st==Lazy||st==Cold) {
                    r->restoring=true;source=r->packed;mine=true;
                } else settled=true;   // Free: not ours to fix
            }
        }
        if(settled)break;
        if(mine) {
            ThrottleWait();
            bool done=CommitSpan(*r);
            if(done && st==Cold) {
                done=EnsureDecode() && source && BlockCodec::Decode(source->data,source->bytes,reinterpret_cast<uint16_t*>(Span(*r)+Header),r->samples,*decScratch);
                if(!done)VirtualFree(Span(*r),SpanBytes(*r),MEM_DECOMMIT);
            }
            Guard g;
            r->restoring=false;
            if(done) {
                r->state=Resident;r->touched=GetTickCount64();
                stats.residentBytes+=SpanBytes(*r);++stats.resident;
                if(st==Lazy){--stats.lazy;++stats.commits;}
                else {--stats.cold;++stats.restores;if(source){stats.compressedBytes-=source->bytes;HeapFree(packHeap,0,source);r->packed=nullptr;}}
                RingPush(uint32_t(r-records));
                ++stats.faults;ok=true;
            } else {++stats.failures;ok=false;}
            break;
        }
        // Another thread is restoring this span, or an encode is in flight
        // that this write has just cancelled: let it finish, then re-evaluate.
        SwitchToThread();
    }
    inFault=false;SetLastError(last);
    return ok?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_CONTINUE_SEARCH;
}

static bool Init(size_t budgetBytes) {
    if(ready)return true;
    arena=static_cast<uint8_t*>(VirtualAlloc(nullptr,ReserveBytes,MEM_RESERVE,PAGE_NOACCESS));
    records=static_cast<Record*>(VirtualAlloc(nullptr,sizeof(Record)*size_t(MaxObjects),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    owner=static_cast<uint32_t*>(VirtualAlloc(nullptr,sizeof(uint32_t)*size_t(MaxPages),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    ring=static_cast<uint32_t*>(VirtualAlloc(nullptr,sizeof(uint32_t)*size_t(MaxObjects),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    packHeap=HeapCreate(0,0,0);
    if(arena&&records&&owner&&ring&&packHeap)veh=AddVectoredExceptionHandler(1,Fault);
    ready=veh!=nullptr;budget=budgetBytes;
    if(!ready) {
        if(arena)VirtualFree(arena,0,MEM_RELEASE);
        if(records)VirtualFree(records,0,MEM_RELEASE);
        if(owner)VirtualFree(owner,0,MEM_RELEASE);
        if(ring)VirtualFree(ring,0,MEM_RELEASE);
        if(packHeap)HeapDestroy(packHeap);
        arena=nullptr;records=nullptr;owner=nullptr;ring=nullptr;packHeap=nullptr;
    }
    return ready;
}

// A span for `samples` uint16 values, reserved only; nullptr when the arena
// or the record table is exhausted. Returns the data pointer (32-byte aligned).
static uint16_t* Allocate(size_t samples) {
    if(!ready || !samples || samples>MaxSamples)return nullptr;
    uint32_t pages=uint32_t((samples*2+Header+PageSize-1)/PageSize);
    if(pages>MaxSpanPages)return nullptr;
    Guard g;
    uint32_t idx;
    if(freeList[pages]) {
        idx=freeList[pages]-1;freeList[pages]=records[idx].next;
    } else {
        if(used==MaxObjects || nextPage+pages>MaxPages){++stats.overflows;return nullptr;}
        idx=used++;
        records[idx].page=nextPage;records[idx].pages=pages;nextPage+=pages;
        for(uint32_t p=0;p<pages;++p)owner[records[idx].page+p]=idx+1;
    }
    Record& r=records[idx];
    unsigned gen=r.generation+1;
    uint32_t page=r.page;
    r={};r.page=page;r.pages=pages;r.samples=uint32_t(samples);r.generation=gen;r.state=Lazy;r.touched=GetTickCount64();
    ++stats.live;++stats.lazy;
    auto now=r.touched;
    if(now-allocationWindow>=1000){allocationWindow=now;windowAllocations=0;}
    if(++windowAllocations>=1024)stats.lastBulkAllocation=now;
    return reinterpret_cast<uint16_t*>(Span(r)+Header);
}

// The usable sample capacity of an allocation (for in-place resizes); 0 if
// the address is not a live allocation.
static size_t Capacity(const void* p) {
    Guard g;Record* r=Find(p);
    return r?(SpanBytes(*r)-Header)/2:0;
}

// Release by any address inside the span. Waits for an encode or a decode in
// flight, then decommits and recycles the span.
static bool Release(void* p) {
    if(!Contains(p))return false;
    for(;;) {
        {
            Guard g;
            Record* r=Find(p);
            if(!r)return false;
            if(!r->restoring && !r->evicting) {
                if(r->state==Resident){VirtualFree(Span(*r),SpanBytes(*r),MEM_DECOMMIT);stats.residentBytes-=SpanBytes(*r);--stats.resident;}
                else if(r->state==Cold){--stats.cold;if(r->packed){stats.compressedBytes-=r->packed->bytes;HeapFree(packHeap,0,r->packed);r->packed=nullptr;}}
                else --stats.lazy;
                uint32_t idx=uint32_t(r-records);
                unsigned gen=r->generation+1;uint32_t page=r->page,pages=r->pages;
                *r={};r->page=page;r->pages=pages;r->generation=gen;r->state=Free;
                r->next=freeList[pages];freeList[pages]=idx+1;
                --stats.live;++stats.releases;
                return true;
            }
        }
        SwitchToThread();
    }
}

// Encode and decommit one resident span. force ignores age and budget.
static bool Evict(uint32_t idx,bool force=false,uint64_t minAge=MinAgeMs) {
    unsigned gen;size_t samples;uint8_t* data;
    {
        Guard g;
        if(idx>=used)return false;
        Record& r=records[idx];
        if(r.state!=Resident||r.evicting||r.restoring)return false;
        if(!force && (stats.residentBytes<=budget || GetTickCount64()-r.touched<minAge || r.sticky))return false;
        DWORD old;
        if(!VirtualProtect(Span(r),SpanBytes(r),PAGE_READONLY,&old)){++stats.failures;return false;}
        r.evicting=true;r.cancel=false;gen=r.generation;samples=r.samples;data=Span(r)+Header;
    }
    if(!EnsureEncode()){Guard g;Record& r=records[idx];DWORD old;VirtualProtect(Span(r),SpanBytes(r),PAGE_READWRITE,&old);r.evicting=false;++stats.failures;return false;}
    size_t cap=samples*2*95/100;
    uint8_t* tmp=static_cast<uint8_t*>(HeapAlloc(packHeap,0,cap+64));
    size_t count=tmp?BlockCodec::Encode(reinterpret_cast<const uint16_t*>(data),samples,tmp,cap,*encScratch):0;
    Guard g;
    Record& r=records[idx];
    bool current=r.generation==gen && r.state==Resident;
    if(current)r.evicting=false;
    if(!current||r.cancel||!count) {
        if(tmp)HeapFree(packHeap,0,tmp);
        if(current) {
            DWORD old;VirtualProtect(Span(r),SpanBytes(r),PAGE_READWRITE,&old);
            if(r.cancel){r.cancel=false;++stats.cancelled;}
            else if(!count){r.sticky=true;++stats.incompressible;}
            r.touched=GetTickCount64();
            RingPush(idx);
        }
        return false;
    }
    Packed* packed=static_cast<Packed*>(HeapAlloc(packHeap,0,offsetof(Packed,data)+count));
    if(!packed){HeapFree(packHeap,0,tmp);DWORD old;VirtualProtect(Span(r),SpanBytes(r),PAGE_READWRITE,&old);++stats.failures;RingPush(idx);return false;}
    packed->bytes=unsigned(count);SIZE_T usable=HeapSize(packHeap,0,packed);packed->commit=(usable==SIZE_T(-1)?offsetof(Packed,data)+count:usable)+16;
    memcpy(packed->data,tmp,count);HeapFree(packHeap,0,tmp);
    if(!VirtualFree(Span(r),SpanBytes(r),MEM_DECOMMIT)) {
        HeapFree(packHeap,0,packed);DWORD old;VirtualProtect(Span(r),SpanBytes(r),PAGE_READWRITE,&old);++stats.failures;RingPush(idx);return false;
    }
    r.packed=packed;r.state=Cold;
    stats.residentBytes-=SpanBytes(r);--stats.resident;++stats.cold;++stats.evictions;stats.compressedBytes+=count;
    return true;
}

// Worker policy: evict from the old end of the resident ring while the pool
// is over budget, with a bounded time slice. Safe from several threads.
static void Tick(unsigned attempts=0) {
    unsigned limit=attempts?attempts:4096;
    LARGE_INTEGER frequency{},start{},now{};
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    for(unsigned n=0;n<limit;++n) {
        uint32_t i;uint64_t minAge;
        {
            Guard g;
            if(stats.residentBytes<=budget || ringHead==ringTail)return;
            auto tick=GetTickCount64();
            bool loading=stats.lastBulkAllocation && tick-stats.lastBulkAllocation<15000;
            minAge=loading?LoadingMinAgeMs:MinAgeMs;
            i=ring[ringHead&RingMask];
            Record& r=records[i];
            if(r.state!=Resident||r.evicting||r.restoring||r.sticky){++ringHead;continue;}   // stale entry
            if(tick-r.touched<minAge)return;   // the oldest is still young: everything behind it is younger
            ++ringHead;
        }
        Evict(i,false,minAge);   // on cancel or failure the record re-enters the ring
        if(!attempts && (n&15)==15) {
            QueryPerformanceCounter(&now);
            if((now.QuadPart-start.QuadPart)*25>frequency.QuadPart)return;
        }
    }
}
}  // namespace SmallPager
