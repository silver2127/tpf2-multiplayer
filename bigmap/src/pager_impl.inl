// Shared body of the fixed-address, lossless, fault-restored pagers.
// Included (never directly) by terrain_pager.h and material_pager.h, each of
// which defines, before including:
//   PAGER_NS         namespace name
//   PAGER_ELEMENT    element type of the managed vector (uint16_t, uint8_t)
//   PAGER_SAMPLES    element count of one managed allocation
//   PAGER_BYTES      byte size of one managed allocation
//   PAGER_MAX_SLOTS  slot capacity (address space only)
//   PAGER_CODEC      codec namespace with EncodeScratch, DecodeScratch, Encode, Decode
//
// Windows 10 1803+ placeholders. Public mappings are made inaccessible BEFORE
// snapshotting through a private alias of the same section. A fault restores
// the exact bytes at the original address. No engine reader can see a moving
// buffer.
//
// Eviction (September 15 revision, both measured problems in a live 256x256 load):
// - Encoding runs OUTSIDE the pool lock, so several worker threads can evict
//   in parallel (one thread could not keep up while two terrain versions were
//   created during a save load: 14 GiB resident, 38 GB peak working set).
//   While a slot is being encoded its public view is inaccessible; a fault on it
//   cancels the eviction (the encoded snapshot is discarded) and restores access.
//   A per-slot generation number rejects results for a slot that was released
//   and reused meanwhile.
// - Second chance: without memory pressure a candidate is first only made
//   inaccessible ("soft-blocked", still resident). An access within SoftDelayMs
//   restores it with one protection change instead of a decode; only slots left
//   untouched are encoded. After load the engine re-read ~2,250 evicted terrain
//   tiles per second, each paying a full decode.
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace PAGER_NS {
constexpr size_t Samples=PAGER_SAMPLES, Bytes=PAGER_BYTES, Offset=32;
constexpr size_t SlotBytes=(Bytes+Offset+4095)&~size_t(4095);
constexpr unsigned MaxSlots=PAGER_MAX_SLOTS;
// An allocation that does not encode below 95% of its raw size stays resident.
constexpr size_t PackLimit=Bytes*95/100;
// MinAgeMs: a slot restored or allocated more recently is not evicted.
// LoadingMinAgeMs applies within 15 s of a burst of allocations: a save load
// created all 131,072 terrain versions within a few seconds and none could be
// evicted for 5 s (31.5 GB peak working set).
constexpr uint64_t MinAgeMs=5000, LoadingMinAgeMs=1000, SoftDelayMs=3000;
using Element=PAGER_ELEMENT;
// h1/h2: two independent 64-bit hashes of the raw allocation, the key of the
// content-dedup index (see Evict). Set once at creation, immutable afterwards.
struct Packed {unsigned refs,bytes;size_t commit;uint64_t h1,h2;uint8_t data[1];};
struct Slot {
    HANDLE section;
    Packed* packed;
    uint64_t touched;
    uint64_t blockedAt;
    unsigned next;
    unsigned generation;   // bumped on allocation and release; survives both
    bool active;
    bool blocked;          // public view inaccessible
    bool readOnly;
    bool viewMissing;
    bool soft;             // blocked by the second-chance stage, not yet encoding
    bool evicting;         // an unlocked encode of this slot is in progress
    bool cancel;           // accessed during that encode: discard the result
    bool restoring;        // an unlocked decode into this slot is in progress
    // Copy-on-write sharing (terrain only, cfg-gated). Several slots may map ONE
    // section, each at its own fixed address through its own duplicated handle,
    // all PAGE_READONLY. Sharers form a circular list through shareNext. A write
    // to any of them privatizes that one slot first, so the versions never alias.
    bool shared;
    unsigned shareNext;
    // The tile's content came from the terrain sidecar (terrain_serve.h): the
    // load's refine/alignment publication copies into it are skipped. Cleared
    // with the rest of the slot on allocation.
    bool served;
};
struct Stats {
    uint64_t live, resident, compressedBytes, compressedCommit, faults, evictions, failures;
    uint64_t encodes, reusedEvictions, writeFaults, sharedClones;
    uint64_t lastBulkAllocation;
    // Allocations refused because every slot was in use. Each one silently
    // became an uncompressed stock vector; the 131,072-slot cap hid exactly
    // this on 256x256+ maps, so it is counted and logged.
    uint64_t overflows;
    uint64_t rateLimited;       // Tick passes cut short by the eviction rate limit
    uint64_t evictMicros, evictOps;   // wall time of every eviction and soft block, for the adaptive rate
    uint64_t lazyAllocations;         // allocations handed out as cold slots on the zero blob (no section yet)
    uint64_t softBlocked;       // current soft-blocked resident slots
    uint64_t softRescues;       // faults satisfied by a protection change alone
    uint64_t cancelledEvictions;
    // COW sharing. privatizations/sharedViews is the ratio that decides whether
    // sharing is worth keeping: 0 means the second version is never written.
    uint64_t sharedViews;       // COW copies satisfied by mapping the same section
    uint64_t sharedSlots;       // slots currently in a sharer ring
    uint64_t privatizations;    // sharers written to, and so given a private copy
    uint64_t privatizeBytes;
    // Why a share was refused. A first 256x256 load measured cow_shared=0, so
    // these separate "the hook is never reached" from "the source was busy".
    uint64_t shareRefusedNotSlot, shareRefusedCold, shareRefusedPacked, shareRefusedBusy;
    // Content dedup: evictions that found an identical blob already stored and
    // shared it instead of encoding (hits), and index rebuilds (tombstone sweeps).
    uint64_t dedupHits, dedupRebuilds;
    // Restores that had to wait for a section (the commit charge at its limit).
    uint64_t restoreRetries, restoreGiveUps;
    // Backpressure: cold restores that waited for eviction while the commit
    // charge was tight, and the milliseconds they waited in total.
    uint64_t throttleWaits, throttleMillis;
};
// Set by the worker while the commit charge is tight. MEASURED 2026-09-17:
// the load fills tiles (and the alignment pass its blocks) at up to ~30,000
// sections per second, eviction sheds a few thousand, and the commit charge
// runs out between two worker ticks whatever the target says. With the
// throttle on, a fault that would create a NEW section first waits, up to
// ThrottleMaxMs, while the pool is over budget, so the burst becomes a stream
// the evictors can absorb. Slots that already have a section (blocked, soft
// blocked, read-only) never wait: those are protection changes, not commit.
static volatile LONG throttle=0;
constexpr unsigned ThrottleMaxMs=2000;
static void SetThrottle(bool on){InterlockedExchange(&throttle,on?1:0);}
// Test hook: the next N section creations fail, as they do at the commit limit.
static volatile LONG injectSectionFailures=0;
static HANDLE NewSection() {
    if(injectSectionFailures>0 && InterlockedDecrement(&injectSectionFailures)>=0)return nullptr;
    return CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(SlotBytes),nullptr);
}
using Alloc2=void*(WINAPI*)(HANDLE,void*,SIZE_T,ULONG,ULONG,void*,ULONG);
using Map3=void*(WINAPI*)(HANDLE,HANDLE,void*,ULONG64,SIZE_T,ULONG,ULONG,void*,ULONG);
using Unmap2=BOOL(WINAPI*)(HANDLE,void*,ULONG);
static Alloc2 alloc2;
static Map3 map3;
static Unmap2 unmap2;
static SRWLOCK lock=SRWLOCK_INIT;
static uint8_t* arena;
static Slot* slots;
static void* veh;
static unsigned allocated, freeHead=MaxSlots, cursor;
static Stats stats{};
static size_t budget=1024ull*1024*1024;
static uint64_t allocationWindow;
static unsigned windowAllocations;
static PAGER_CODEC::DecodeScratch* decodeScratch;   // restores run under the lock
static HANDLE packHeap;
static bool ready=false;
// Lazy zero allocations: the canonical all-zero blob (one reference held by
// the pager for its lifetime) and the switch; see Allocate.
static Packed* zeroBlob=nullptr;
static bool lazyZero=false;
static thread_local bool inFault=false;
// Each evicting thread encodes with its own scratch, allocated on first use.
static thread_local PAGER_CODEC::EncodeScratch* threadEncodeScratch=nullptr;
static thread_local uint8_t* threadPackedScratch=nullptr;
// Each faulting thread decodes with its own scratch (allocated on its first
// cold restore), so restores of different slots run in parallel.
static thread_local PAGER_CODEC::DecodeScratch* threadDecodeScratch=nullptr;
// Fault code never calls the game, its allocator, logging, or STL containers.
// Compressed blobs live in a private Win32 heap that only this pager touches;
// frees on the fault path run under `lock`, which the faulting thread cannot
// already hold. Immutable compressed copies survive read-only restoration and
// are freed after their last owner writes or is destroyed.
struct Guard { Guard(){AcquireSRWLockExclusive(&lock);} ~Guard(){ReleaseSRWLockExclusive(&lock);} };
static uint8_t* Base(unsigned i){return arena+size_t(i)*SlotBytes;}
static bool Contains(const void* p) {
    return arena && uintptr_t(p)>=uintptr_t(arena) &&
        uintptr_t(p)-uintptr_t(arena)<size_t(MaxSlots)*SlotBytes;
}
static unsigned Index(const void* p){return unsigned((uintptr_t(p)-uintptr_t(arena))/SlotBytes);}
static size_t Committed(size_t n){return (n+4095)&~size_t(4095);}
// Mark the live slot holding `p` as served by the sidecar (under the lock).
static bool SetServed(const void* p) {
    if(!Contains(p))return false;
    Guard g;unsigned i=Index(p);
    if(i>=allocated||!slots[i].active)return false;
    slots[i].served=true;return true;
}
// Lock-free read for the block-copy hot path: `served` is set only on a live
// slot and cleared under the lock before the slot is handed out again, so a
// caller holding a pointer into a tile never sees another tile's flag.
static bool IsServed(const void* p) {
    if(!Contains(p))return false;
    unsigned i=Index(p);
    return i<allocated && slots[i].active && slots[i].served;
}
static size_t PackedSize(unsigned bytes){return offsetof(Packed,data)+bytes;}
// Caller holds lock.
static void ClearSoft(Slot& s){if(s.soft){s.soft=false;--stats.softBlocked;}}
// Content dedup (docs/terrain-cow-sharing.md, "where the 8.25 GiB actually
// is"; MEASURED 2026-09-17 with the probe below: during a 256x256 save load
// every one of the 131,072 live tiles has exactly one byte-identical twin in
// the other CTerrain version). An eviction hashes the bytes before encoding and
// looks the pair of hashes up in this index of stored blobs; a hit shares the
// existing blob (the Clone mechanism: one immutable blob, refs counted, dropped
// on the first write) and skips the encode. Two independent 64-bit hashes are
// required to match, so a false share needs a 128-bit collision.
//
// The index is open addressing over Packed pointers, sized for MaxSlots blobs at
// <= 50% load, with tombstones on removal and a rebuild from the slot table
// once tombstones and entries together reach that load. Every function here
// runs under `lock`; the table is allocated only by EnableDedup.
static Packed** dedupTable=nullptr;
static size_t dedupUsed=0,dedupTombstones=0;
constexpr size_t DedupCap=size_t(MaxSlots)*2;
static Packed* const DedupTombstone=reinterpret_cast<Packed*>(uintptr_t(1));
static size_t DedupHome(uint64_t h1){return size_t(h1^(h1>>31))&(DedupCap-1);}
static Packed* DedupFind(uint64_t h1,uint64_t h2) {
    if(!dedupTable)return nullptr;
    for(size_t k=DedupHome(h1);;k=(k+1)&(DedupCap-1)) {
        Packed* p=dedupTable[k];
        if(!p)return nullptr;
        if(p!=DedupTombstone && p->h1==h1 && p->h2==h2)return p;
    }
}
static void DedupInsertRaw(Packed* p) {
    for(size_t k=DedupHome(p->h1);;k=(k+1)&(DedupCap-1)) {
        if(dedupTable[k]==DedupTombstone){dedupTable[k]=p;--dedupTombstones;++dedupUsed;return;}
        if(!dedupTable[k]){dedupTable[k]=p;++dedupUsed;return;}
    }
}
static void DedupRebuild() {
    memset(dedupTable,0,DedupCap*sizeof(Packed*));
    dedupUsed=dedupTombstones=0;++stats.dedupRebuilds;
    for(unsigned i=0;i<allocated;++i) {
        Packed* p=slots[i].packed;
        if(p && (p->h1|p->h2) && !DedupFind(p->h1,p->h2))DedupInsertRaw(p);
    }
}
static void DedupInsert(Packed* p) {
    if(!dedupTable)return;
    if(dedupUsed+dedupTombstones>=DedupCap/2)DedupRebuild();
    if(dedupUsed+dedupTombstones>=DedupCap/2)return;   // full of live blobs: index nothing more
    DedupInsertRaw(p);
}
static void DedupRemove(Packed* p) {
    if(!dedupTable)return;
    for(size_t k=DedupHome(p->h1);;k=(k+1)&(DedupCap-1)) {
        Packed* q=dedupTable[k];
        if(!q)return;
        if(q==p){dedupTable[k]=DedupTombstone;--dedupUsed;++dedupTombstones;return;}
    }
}
// Second, independent hash over the raw bytes (different constants and mixing
// from the codec's own; the codec's hash is h1).
static uint64_t Hash2(const void* data) {
    constexpr uint64_t K3=0xD6E8FEB86659FD93ull,K4=0xA0761D6478BD642Full;
    const uint8_t* b=static_cast<const uint8_t*>(data);
    uint64_t h=0x8A5CD789635D2DFFull;size_t i=0;
    for(;i+8<=Bytes;i+=8){uint64_t w;memcpy(&w,b+i,8);h=(h^(w*K3));h=(h<<23|h>>41)*K4;}
    for(;i<Bytes;++i){h^=b[i];h=(h<<23|h>>41)*K4;}
    h^=h>>31;h*=K3;h^=h>>29;
    return h;
}
// The last reference to a blob frees it and drops it from the dedup index.
static void ReleasePacked(Packed* p) {
    if(--p->refs)return;
    DedupRemove(p);
    stats.compressedBytes-=p->bytes;stats.compressedCommit-=p->commit;
    HeapFree(packHeap,0,p);
}
static void DropPacked(Slot& s) {
    if(!s.packed)return;
    auto p=s.packed;s.packed=nullptr;
    ReleasePacked(p);
}
// Caller holds lock. Unlink slot i from its sharer ring; a ring left with one
// member is no longer shared (and becomes an eviction candidate again).
static void Unshare(unsigned i) {
    auto& s=slots[i];
    if(!s.shared)return;
    unsigned prev=i;
    while(slots[prev].shareNext!=i)prev=slots[prev].shareNext;
    slots[prev].shareNext=s.shareNext;
    s.shared=false;s.shareNext=i;--stats.sharedSlots;
    if(slots[prev].shareNext==prev){slots[prev].shared=false;--stats.sharedSlots;}
}
// Caller holds lock. Give slot i a private writable section holding the same
// bytes, leaving every other sharer on the original one. Runs entirely under
// the lock: the copy is a warm 132 KiB memcpy, cheaper than the decode Restore
// already performs here, and holding the lock throughout means Release needs no
// new spin state and no handler-visible intermediate exists.
static bool Privatize(unsigned i) {
    auto& s=slots[i];
    HANDLE section=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(SlotBytes),nullptr);
    if(!section){++stats.failures;return false;}
    auto dst=static_cast<uint8_t*>(MapViewOfFile(section,FILE_MAP_ALL_ACCESS,0,0,SlotBytes));
    // Copy through private aliases, never through Base(i): an evictor may have
    // made the public view PAGE_NOACCESS, and a re-entrant fault inside the
    // handler is rejected by inFault and would become an unhandled violation.
    auto src=dst?static_cast<uint8_t*>(MapViewOfFile(s.section,FILE_MAP_READ,0,0,SlotBytes)):nullptr;
    if(src)memcpy(dst,src,SlotBytes);
    if(dst)UnmapViewOfFile(dst);
    if(src)UnmapViewOfFile(src);
    if(!src){CloseHandle(section);++stats.failures;return false;}
    if(!s.viewMissing && !unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER)) {
        CloseHandle(section);++stats.failures;return false;
    }
    // No view is mapped from here until map3 succeeds; on failure the slot keeps
    // the shared section and viewMissing, so the next fault retries.
    s.viewMissing=true;
    if(map3(section,GetCurrentProcess(),Base(i),0,SlotBytes,MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE,nullptr,0)!=Base(i)){CloseHandle(section);++stats.failures;return false;}
    CloseHandle(s.section);
    Unshare(i);
    s.section=section;s.viewMissing=false;s.blocked=false;ClearSoft(s);s.readOnly=false;
    // One more unique section now exists, so the budget must count it.
    ++stats.resident;++stats.privatizations;stats.privatizeBytes+=SlotBytes;
    DropPacked(s);
    return true;
}
static bool Restore(unsigned i,bool writing=false) {
    auto& s=slots[i];
    if(s.section) {
        if(s.blocked) {
            if(s.evicting){s.cancel=true;++stats.cancelledEvictions;}
            else if(s.soft)++stats.softRescues;
        }
        // A shared view must never be made writable in place: every other
        // sharer maps the same section and would observe the write.
        if(writing && s.shared)return Privatize(i);
        if(s.viewMissing || (writing&&s.readOnly)) {
            // A read-only section view cannot be upgraded to writable with
            // VirtualProtect. Remap the SAME backing section with write access;
            // its contents survive, and competing faults remain behind lock.
            if(!s.viewMissing && !unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER)) {
                ++stats.failures;return false;
            }
            s.viewMissing=true;
            DWORD protection=writing||!s.packed?PAGE_READWRITE:PAGE_READONLY;
            if(map3(s.section,GetCurrentProcess(),Base(i),0,SlotBytes,MEM_REPLACE_PLACEHOLDER,
                    protection,nullptr,0)!=Base(i)){++stats.failures;return false;}
            s.viewMissing=false;s.blocked=false;ClearSoft(s);s.readOnly=protection==PAGE_READONLY;
            if(writing)DropPacked(s);return true;
        }
        if(!s.blocked && !(writing&&s.readOnly))return true;
        DWORD old;
        DWORD protection=writing||!s.packed?PAGE_READWRITE:PAGE_READONLY;
        if(!VirtualProtect(Base(i),SlotBytes,protection,&old)){++stats.failures;return false;}
        s.blocked=false;ClearSoft(s);s.readOnly=protection==PAGE_READONLY;
        if(writing)DropPacked(s);return true;
    }
    HANDLE section=NewSection();
    if(!section){++stats.failures;return false;}
    auto alias=static_cast<uint8_t*>(MapViewOfFile(section,FILE_MAP_ALL_ACCESS,0,0,SlotBytes));
    if(!alias){CloseHandle(section);++stats.failures;return false;}
    bool ok=true;
    if(s.packed && s.packed!=zeroBlob)   // a fresh section is already all zero
        ok=PAGER_CODEC::Decode(s.packed->data,s.packed->bytes,reinterpret_cast<Element*>(alias+Offset),*decodeScratch);
    // Match the stock MSVC large-allocation header. Normally only our destroy
    // and resize hooks consume this allocation; keeping the header aids audit.
    *reinterpret_cast<void**>(alias+Offset-8)=Base(i);
    DWORD protection=writing||!s.packed?PAGE_READWRITE:PAGE_READONLY;
    if(ok)ok=map3(section,GetCurrentProcess(),Base(i),0,SlotBytes,
                 MEM_REPLACE_PLACEHOLDER,protection,nullptr,0)==Base(i);
    UnmapViewOfFile(alias);
    if(!ok){CloseHandle(section);++stats.failures;return false;}
    s.section=section;s.blocked=false;s.readOnly=protection==PAGE_READONLY;
    s.touched=GetTickCount64();++stats.resident;
    if(writing)DropPacked(s);
    return true;
}
// Cold restore of slot i outside the lock (caller set s.restoring and holds a
// reference on `source`). Returns the new section mapped at Base(i), or fails.
static bool RestoreUnlocked(unsigned i,Packed* source,bool writing) {
    if(!threadDecodeScratch)
        threadDecodeScratch=static_cast<PAGER_CODEC::DecodeScratch*>(VirtualAlloc(nullptr,sizeof(PAGER_CODEC::DecodeScratch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    HANDLE section=threadDecodeScratch?NewSection():nullptr;
    auto alias=section?static_cast<uint8_t*>(MapViewOfFile(section,FILE_MAP_ALL_ACCESS,0,0,SlotBytes)):nullptr;
    bool decoded=alias && (source==zeroBlob || PAGER_CODEC::Decode(source->data,source->bytes,reinterpret_cast<Element*>(alias+Offset),*threadDecodeScratch));
    if(alias)*reinterpret_cast<void**>(alias+Offset-8)=Base(i);
    Guard g;
    auto& s=slots[i];
    s.restoring=false;
    bool ok=decoded;
    if(ok) {
        DWORD protection=writing||!s.packed?PAGE_READWRITE:PAGE_READONLY;
        ok=map3(section,GetCurrentProcess(),Base(i),0,SlotBytes,MEM_REPLACE_PLACEHOLDER,protection,nullptr,0)==Base(i);
        if(ok) {
            s.section=section;s.blocked=false;s.readOnly=protection==PAGE_READONLY;
            s.touched=GetTickCount64();++stats.resident;
            if(writing)DropPacked(s);
            ++stats.faults;if(writing)++stats.writeFaults;
        }
    }
    // Drop the reference taken for the unlocked decode.
    ReleasePacked(source);
    if(alias)UnmapViewOfFile(alias);
    if(!ok){if(section)CloseHandle(section);++stats.failures;}
    return ok;
}
// One restore attempt for a faulting slot. *live is cleared when the address
// is not a live allocation of ours (nothing to restore, no retry).
static bool FaultRestore(unsigned i,bool writing,bool* live) {
    for(;;) {
        Packed* source=nullptr;
        {
            Guard g;
            if(i>=allocated || !slots[i].active){*live=false;return false;}
            auto& s=slots[i];
            if(!s.restoring) {
                if(s.section || !s.packed) {
                    // Protection change, remap or a fresh zero section: cheap, locked.
                    bool ok=Restore(i,writing);
                    if(ok){++stats.faults;if(writing)++stats.writeFaults;s.touched=GetTickCount64();}
                    return ok;
                }
                // Cold: decode without holding the pool lock. Other threads that
                // fault on this slot wait for it; other slots proceed in parallel.
                s.restoring=true;source=s.packed;++source->refs;
            }
        }
        if(!source){SwitchToThread();continue;}
        return RestoreUnlocked(i,source,writing);
    }
}
static LONG CALLBACK Fault(EXCEPTION_POINTERS* e) {
    auto r=e->ExceptionRecord;
    if(inFault || r->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || r->NumberParameters<2 ||
       r->ExceptionInformation[0]>1 || !Contains(reinterpret_cast<void*>(r->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_SEARCH;
    // The original last-error value belongs to the interrupted engine code.
    DWORD last=GetLastError();inFault=true;
    unsigned i=Index(reinterpret_cast<void*>(r->ExceptionInformation[1]));
    bool writing=r->ExceptionInformation[0]==1;
    bool ok=false,live=true;
    if(InterlockedCompareExchange(&throttle,0,0)) {
        unsigned waited=0;bool counted=false;
        while(waited<ThrottleMaxMs && InterlockedCompareExchange(&throttle,0,0)) {
            bool wait;
            {Guard g;wait=i<allocated && slots[i].active && !slots[i].section && !slots[i].restoring && stats.resident*SlotBytes>budget;}
            if(!wait)break;
            if(!counted){Guard g;++stats.throttleWaits;counted=true;}
            Sleep(4);waited+=4;
        }
        if(waited){Guard g;stats.throttleMillis+=waited;}
    }
    // A restore needs a section, and the OS refuses one when the commit charge
    // is at its limit. MEASURED 2026-09-17: during a commit-tight load a write
    // into an evicted tile found no section (failures=1) and the access
    // violation went back to the engine as a crash. The eviction threads are
    // freeing sections at exactly that moment, so wait for them: retry for up
    // to ~2 s before giving the fault back.
    for(unsigned attempt=0;live;++attempt) {
        ok=FaultRestore(i,writing,&live);
        if(ok||!live||attempt>=500)break;
        {Guard g;++stats.restoreRetries;}
        Sleep(4);
    }
    if(!ok && live){Guard g;++stats.restoreGiveUps;}
    inFault=false;SetLastError(last);
    return ok?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_CONTINUE_SEARCH;
}
static bool Init(size_t budgetBytes) {
    if(ready)return true;
    auto kernel=GetModuleHandleW(L"kernelbase.dll");
    alloc2=reinterpret_cast<Alloc2>(GetProcAddress(kernel,"VirtualAlloc2"));
    map3=reinterpret_cast<Map3>(GetProcAddress(kernel,"MapViewOfFile3"));
    unmap2=reinterpret_cast<Unmap2>(GetProcAddress(kernel,"UnmapViewOfFile2"));
    if(!alloc2||!map3||!unmap2)return false;
    arena=static_cast<uint8_t*>(alloc2(GetCurrentProcess(),nullptr,size_t(MaxSlots)*SlotBytes,
        MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,nullptr,0));
    // Demand-zero: only slot records that are actually used become resident.
    slots=static_cast<Slot*>(VirtualAlloc(nullptr,sizeof(Slot)*MaxSlots,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    decodeScratch=static_cast<PAGER_CODEC::DecodeScratch*>(VirtualAlloc(nullptr,sizeof(PAGER_CODEC::DecodeScratch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    packHeap=HeapCreate(0,0,0);
    if(arena&&slots&&decodeScratch&&packHeap)veh=AddVectoredExceptionHandler(1,Fault);
    ready=veh!=nullptr;budget=budgetBytes;
    // Failed initialization has no live game allocations. Release everything.
    if(!ready) {
        if(arena)VirtualFree(arena,0,MEM_RELEASE);
        if(slots)VirtualFree(slots,0,MEM_RELEASE);
        if(decodeScratch)VirtualFree(decodeScratch,0,MEM_RELEASE);
        if(packHeap)HeapDestroy(packHeap);
        arena=nullptr;slots=nullptr;decodeScratch=nullptr;packHeap=nullptr;
    }
    return ready;
}
// Caller holds lock. Slot addresses are recycled only after the engine has
// destroyed the owning vector; compressed blobs have separate refs.
static unsigned NewSlot() {
    unsigned i;
    if(freeHead!=MaxSlots){i=freeHead;freeHead=slots[i].next;}
    else {
        if(allocated==MaxSlots){++stats.overflows;return MaxSlots;}
        i=allocated;
        // Split the front of the remaining placeholder. The final slot is
        // already an exact-size placeholder and requires no split.
        if(i+1<MaxSlots && !VirtualFree(Base(i),SlotBytes,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) {
            ++stats.failures;return MaxSlots;
        }
        ++allocated;
    }
    auto now=GetTickCount64();
    if(now-allocationWindow>=1000){allocationWindow=now;windowAllocations=0;}
    if(++windowAllocations>=1024)stats.lastBulkAllocation=now;
    unsigned generation=slots[i].generation+1;
    slots[i]={};slots[i].generation=generation;slots[i].active=true;slots[i].touched=now;slots[i].shareNext=i;++stats.live;
    return i;
}
static void FreeSlot(unsigned i) {
    unsigned generation=slots[i].generation+1;
    slots[i]={};slots[i].generation=generation;slots[i].shareNext=i;slots[i].next=freeHead;freeHead=i;
}
// Lazy zero allocations (MEASURED 2026-09-17, 103,680-tile save load): the
// engine allocates every tile of both versions first, all zero, and fills them
// over the next 20-30 s; at one probe 112,678 of 131,072 live tiles were still
// zero. A section per allocation committed up to 27 GiB before any terrain
// existed, faster than eviction could shed it, and the commit charge hit the
// limit. With lazyZero a fresh allocation is a cold slot on the shared zero
// blob: no section, no commit, until the first touch creates one (a write
// gets a private zero section, a read a read-only one on the blob). Nothing
// else changes: the address is fixed, the restore path is the ordinary cold
// restore, and the blob is never freed (the pager holds one reference).
static Element* Allocate() {
    if(!ready)return nullptr;
    Guard g;unsigned i=NewSlot();if(i==MaxSlots)return nullptr;
    if(lazyZero && zeroBlob) {
        slots[i].packed=zeroBlob;++zeroBlob->refs;++stats.lazyAllocations;
        return reinterpret_cast<Element*>(Base(i)+Offset);
    }
    if(!Restore(i,true)){FreeSlot(i);--stats.live;return nullptr;}
    return reinterpret_cast<Element*>(Base(i)+Offset);
}
static Element* Clone(const void* src) {
    if(!Contains(src))return nullptr;
    Guard g;unsigned source=Index(src);
    if(source>=allocated || src!=Base(source)+Offset || !slots[source].active || !slots[source].packed)return nullptr;
    unsigned i=NewSlot();if(i==MaxSlots)return nullptr;
    slots[i].packed=slots[source].packed;++slots[i].packed->refs;++stats.sharedClones;
    return reinterpret_cast<Element*>(Base(i)+Offset);
}
// Copy-on-write share: map the source's section a second time at a new slot's
// address instead of copying 132 KiB. Both views become read-only, so the first
// write to either one faults into Privatize. Returns nullptr whenever the source
// is not a settled resident slot, and the caller falls back to the eager copy.
static Element* Share(const void* src) {
    if(!Contains(src))return nullptr;          // unmanaged: counted by the caller
    Guard g;unsigned source=Index(src);
    if(source>=allocated || src!=Base(source)+Offset || !slots[source].active) {
        ++stats.shareRefusedNotSlot;return nullptr;
    }
    auto& s=slots[source];
    // A compressed source is the existing Clone path; an in-flight encode or
    // decode owns the view, and a blocked view would hand the new slot a
    // protection state it does not own.
    if(!s.section){++stats.shareRefusedCold;return nullptr;}
    if(s.packed){++stats.shareRefusedPacked;return nullptr;}
    if(s.blocked||s.evicting||s.restoring||s.viewMissing){++stats.shareRefusedBusy;return nullptr;}
    if(!s.readOnly) {
        DWORD old;
        if(!VirtualProtect(Base(source),SlotBytes,PAGE_READONLY,&old)){++stats.failures;return nullptr;}
        s.readOnly=true;
    }
    // Each sharer owns its own handle, so every existing CloseHandle stays correct.
    HANDLE dup=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),s.section,GetCurrentProcess(),&dup,0,FALSE,DUPLICATE_SAME_ACCESS)) {
        ++stats.failures;return nullptr;
    }
    unsigned i=NewSlot();
    if(i==MaxSlots){CloseHandle(dup);return nullptr;}
    if(map3(dup,GetCurrentProcess(),Base(i),0,SlotBytes,MEM_REPLACE_PLACEHOLDER,PAGE_READONLY,nullptr,0)!=Base(i)) {
        CloseHandle(dup);FreeSlot(i);--stats.live;++stats.failures;return nullptr;
    }
    auto& d=slots[i];
    d.section=dup;d.readOnly=true;d.touched=GetTickCount64();
    // Deliberately no ++stats.resident: the section's bytes are already counted
    // once. The budget must measure real backing, or a shared tile is counted
    // twice and the pager evicts far too eagerly.
    if(!s.shared){s.shared=true;s.shareNext=source;++stats.sharedSlots;}
    d.shared=true;d.shareNext=s.shareNext;s.shareNext=i;++stats.sharedSlots;
    ++stats.sharedViews;
    return reinterpret_cast<Element*>(Base(i)+Offset);
}
static bool Release(void* p) {
    if(!Contains(p))return false;
    unsigned i=Index(p);
    // The engine does not destroy a vector while reading it, but a restore
    // started by another thread's fault must finish before the slot goes.
    for(;;) {
        {Guard g;if(i>=allocated || !slots[i].restoring)break;}
        SwitchToThread();
    }
    Guard g;auto& s=slots[i];
    if(i>=allocated || p!=Base(i)+Offset || !s.active || s.restoring)return false;
    if(s.section) {
        if(!s.viewMissing && !unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER)) {
            // An owned pointer MUST NOT reach the game's heap free. Retain it
            // on OS failure; safe leak with diagnostics instead of wrong free.
            ++stats.failures;return true;
        }
        // An in-progress encode keeps its own alias view of the section; the
        // generation bump below makes it discard its result.
        bool shared=s.shared;
        if(shared)Unshare(i);
        // Closing this sharer's duplicated handle does not destroy a section
        // another sharer still maps, so the resident count (unique sections)
        // only drops when the last sharer goes.
        CloseHandle(s.section);
        if(!shared)--stats.resident;
    }
    ClearSoft(s);
    DropPacked(s);
    FreeSlot(i);--stats.live;
    return true;
}
// Release by any address inside a slot: the raw base (what an aligned delete
// reads at [-8] and hands to free) or the data pointer. False when it is not
// a live allocation, so the caller never passes an arena address to the CRT.
static bool ReleaseAny(void* p) {
    if(!Contains(p))return false;
    unsigned i=Index(p);
    return Release(Base(i)+Offset);
}
// Second-chance stage: make an aged resident slot inaccessible without
// encoding it. Returns true if it was blocked.
static bool SoftBlock(unsigned i) {
    Guard g;
    if(i>=allocated)return false;
    auto& s=slots[i];
    auto now=GetTickCount64();
    if(!s.active||!s.section||s.blocked||s.evicting||s.shared||now-s.touched<MinAgeMs)return false;
    DWORD old;
    if(!VirtualProtect(Base(i),SlotBytes,PAGE_NOACCESS,&old)){++stats.failures;return false;}
    s.blocked=true;s.soft=true;s.blockedAt=now;++stats.softBlocked;
    return true;
}
static bool EnsureThreadScratch() {
    if(!threadEncodeScratch)
        threadEncodeScratch=static_cast<PAGER_CODEC::EncodeScratch*>(VirtualAlloc(nullptr,sizeof(PAGER_CODEC::EncodeScratch),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!threadPackedScratch)
        threadPackedScratch=static_cast<uint8_t*>(VirtualAlloc(nullptr,PackLimit,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    return threadEncodeScratch&&threadPackedScratch;
}
static bool Evict(unsigned i,bool force=false,uint64_t minAge=MinAgeMs) {
    uint8_t* alias=nullptr;unsigned generation=0;
    {
        Guard g;
        if(i>=allocated)return false;
        auto& s=slots[i];
        // A shared backing is never evicted at this stage: one encode for N
        // sharers needs the backing-refcount design in docs/terrain-cow-sharing.md.
        // Sharers become evictable again as soon as they privatize or unshare.
        if(!s.active||!s.section||s.evicting||s.shared || (!force &&
           (stats.resident*SlotBytes<=budget || GetTickCount64()-s.touched<minAge)))return false;
        if(s.packed) {
            // No writes since restoration: the read-only mapping guarantees this
            // immutable blob is still exact. Re-eviction needs no encoding or copy.
            DWORD old;
            if(!s.blocked && !VirtualProtect(Base(i),SlotBytes,PAGE_NOACCESS,&old)){++stats.failures;return false;}
            s.blocked=true;
            if(unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER)) {
                CloseHandle(s.section);s.section=nullptr;ClearSoft(s);--stats.resident;
                ++stats.evictions;++stats.reusedEvictions;return true;
            }
            DWORD ignored;if(VirtualProtect(Base(i),SlotBytes,PAGE_READONLY,&ignored)){s.blocked=false;ClearSoft(s);}
            ++stats.failures;s.touched=GetTickCount64();return false;
        }
        if(!EnsureThreadScratch()){++stats.failures;return false;}
        // Alias does not fault and stays accessible after revoking the game view.
        alias=static_cast<uint8_t*>(MapViewOfFile(s.section,FILE_MAP_READ,0,0,SlotBytes));
        if(!alias){++stats.failures;s.touched=GetTickCount64();return false;}
        if(!s.blocked) {
            DWORD old=0;
            if(!VirtualProtect(Base(i),SlotBytes,PAGE_NOACCESS,&old)) {
                UnmapViewOfFile(alias);++stats.failures;s.touched=GetTickCount64();return false;
            }
            s.blocked=true;
        }
        ClearSoft(s);
        s.evicting=true;s.cancel=false;generation=s.generation;
    }
    // Unlocked: no engine thread can read or write this slot now (every access
    // faults and cancels), and a concurrent Release only bumps the generation.
    // Dedup first: an identical blob already stored is shared instead of
    // encoded. The reference taken here keeps it alive across the unlocked
    // window; the commit below either hands it to the slot or gives it back.
    uint64_t h1=0,h2=0;Packed* twin=nullptr;
    if(dedupTable) {
        h1=PAGER_CODEC::Hash(reinterpret_cast<const Element*>(alias+Offset));
        h2=Hash2(alias+Offset);
        Guard g;twin=DedupFind(h1,h2);if(twin)++twin->refs;
    }
    size_t count=0;Packed* compressed=twin;
    if(!twin) {
        count=PAGER_CODEC::Encode(reinterpret_cast<const Element*>(alias+Offset),threadPackedScratch,PackLimit,*threadEncodeScratch);
        if(count) {
            compressed=static_cast<Packed*>(HeapAlloc(packHeap,0,PackedSize(unsigned(count))));
            if(compressed) {
                compressed->refs=1;compressed->bytes=unsigned(count);compressed->h1=h1;compressed->h2=h2;
                // Usable block size plus the heap's per-block header, so the logged
                // commit reflects allocator granularity rather than payload alone.
                SIZE_T usable=HeapSize(packHeap,0,compressed);
                compressed->commit=(usable==SIZE_T(-1)?PackedSize(unsigned(count)):usable)+16;
                memcpy(compressed->data,threadPackedScratch,count);
            }
        }
    }
    Guard g;
    if(twin)++stats.dedupHits;else ++stats.encodes;
    if(count&&!compressed)++stats.failures;
    auto& s=slots[i];
    bool current=s.generation==generation && s.active;
    if(current)s.evicting=false;
    if(!current || s.cancel) {
        // Released and possibly reused, or accessed while encoding: the
        // snapshot may be stale. Nothing of this slot's state is touched unless
        // it is still the same allocation (Restore already made it accessible).
        if(twin)ReleasePacked(twin);else if(compressed)HeapFree(packHeap,0,compressed);
        UnmapViewOfFile(alias);
        if(current)s.cancel=false;
        return false;
    }
    if(!twin && compressed && dedupTable) {
        // Another thread stored the same bytes while this encode ran: share its
        // blob rather than keep two.
        if(Packed* t=DedupFind(h1,h2)){HeapFree(packHeap,0,compressed);compressed=twin=t;++t->refs;++stats.dedupHits;}
    }
    bool ok=compressed && unmap2(GetCurrentProcess(),Base(i),MEM_PRESERVE_PLACEHOLDER);
    if(ok) {
        UnmapViewOfFile(alias);CloseHandle(s.section);s.section=nullptr;
        s.packed=compressed;
        if(!twin){stats.compressedBytes+=count;stats.compressedCommit+=compressed->commit;DedupInsert(compressed);}
        --stats.resident;++stats.evictions;
        return true;
    }
    if(compressed){if(twin)ReleasePacked(twin);else HeapFree(packHeap,0,compressed);++stats.failures;}
    DWORD ignored;
    if(!VirtualProtect(Base(i),SlotBytes,PAGE_READWRITE,&ignored)) {
        // Inaccessible but intact resident backing: Fault must retry the
        // protection change rather than swallowing an unresolvable fault.
        ++stats.failures;
    } else s.blocked=false;
    UnmapViewOfFile(alias);s.touched=GetTickCount64();
    return false;
}
static Stats Snapshot(){Guard g;return stats;}
static void SetBudget(size_t bytes){Guard g;budget=bytes;}
// Eviction rate limit (per second, 0 = none) for the quiet case: no loading
// burst, no memory pressure. MEASURED 2026-09-17: draining a loaded world's
// allowance evicted ~3,100 material cells and up to ~6,700 terrain tiles per
// second; every eviction is a VirtualProtect, an encode and an unmap, and at
// that rate the game stutters even though the work is on background threads
// (the 2026-09-15 note measured the same at ~3,000 cycles/s). Pressure (over
// three times the budget, which a commit-tight back-off produces) and loading
// keep the old unlimited behaviour. Soft blocks count too: they are a
// protection change each and become evictions a few seconds later.
static unsigned evictPerSecond=0;
static uint64_t rateWindow=0;static unsigned rateCount=0;
// Set by the worker while the commit charge is tight: the only case that lifts
// the rate limit. Being over three times the budget is not enough on its own:
// MEASURED 2026-09-17, the material pager's allowance is small next to what a
// load allocates, so that test was true from the first second after the load
// and the cap never applied (57,288 cells evicted in 30 s).
static bool urgent=false;
static void SetEvictRate(unsigned perSecond){Guard g;evictPerSecond=perSecond;}
static void SetUrgent(bool on){Guard g;urgent=on;}
// Turn content dedup on (before or after allocations; blobs stored earlier are
// indexed by the next rebuild). 32 MiB of demand-zero address space.
// Encode the canonical zero allocation once and hand it to Allocate. Needs
// this thread's encode scratch (allocated on first use, as for evictors).
static bool EnableLazyZero() {
    Guard g;
    if(zeroBlob){lazyZero=true;return true;}
    if(!EnsureThreadScratch())return false;
    auto zero=static_cast<Element*>(VirtualAlloc(nullptr,Bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!zero)return false;
    size_t count=PAGER_CODEC::Encode(zero,threadPackedScratch,PackLimit,*threadEncodeScratch);
    Packed* p=count?static_cast<Packed*>(HeapAlloc(packHeap,0,PackedSize(unsigned(count)))):nullptr;
    if(p) {
        p->refs=1;p->bytes=unsigned(count);p->h1=PAGER_CODEC::Hash(zero);p->h2=Hash2(zero);
        SIZE_T usable=HeapSize(packHeap,0,p);p->commit=(usable==SIZE_T(-1)?PackedSize(unsigned(count)):usable)+16;
        memcpy(p->data,threadPackedScratch,count);
        stats.compressedBytes+=count;stats.compressedCommit+=p->commit;
        DedupInsert(p);   // evictions of untouched zero tiles share it too
    }
    VirtualFree(zero,0,MEM_RELEASE);
    zeroBlob=p;lazyZero=p!=nullptr;
    return lazyZero;
}
static void SetLazyZero(bool on){Guard g;lazyZero=on&&zeroBlob;}
static bool EnableDedup() {
    Guard g;
    if(dedupTable)return true;
    dedupTable=static_cast<Packed**>(VirtualAlloc(nullptr,DedupCap*sizeof(Packed*),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!dedupTable)return false;
    dedupUsed=dedupTombstones=0;
    // Blobs stored while dedup was off carry no hashes (0,0) and stay unindexed.
    for(unsigned i=0;i<allocated;++i){Packed* p=slots[i].packed;if(p && (p->h1|p->h2) && !DedupFind(p->h1,p->h2))DedupInsertRaw(p);}
    return true;
}
// Content probe (docs/terrain-cow-sharing.md, "where the 8.25 GiB actually is"):
// hash every live allocation and count how many carry the same bytes as another.
// A measurement, not a mechanism: it decides whether content dedup between the
// two CTerrain versions is worth building. A slot with a packed blob (cold, or
// resident read-only and unwritten since its restore) contributes the hash the
// blob already carries; a writable resident slot is hashed through a private
// alias, so a blocked public view is never touched and nothing faults. Slots in
// the middle of an encode or decode are skipped. Untouched allocations are all
// zero, so the size of that group is reported on its own: during a load it is
// tiles not yet filled, not evidence of duplication.
struct ProbeResult {
    uint64_t live, hashedResident, hashedPacked, skipped;
    uint64_t distinct, duplicated;   // duplicated = hashed - distinct: freeable by dedup
    uint64_t zero;                   // members of the all-zero group
    uint64_t pairs;                  // hash groups of exactly two members
    uint64_t largestGroup;
    uint64_t lowHalf;                // live slots in the lower half of the allocated range
    uint64_t ms;
};
// Both codecs write the version byte and then the raw hash, little-endian.
static uint64_t StoredHash(const Packed* p) {
    if(p->bytes<9)return 0;
    uint64_t h=0;for(int i=0;i<8;++i)h|=uint64_t(p->data[1+i])<<(8*i);
    return h;
}
// 0: not a live allocation, 1: resident bytes hashed, 2: stored hash of the
// packed blob, 3: live but busy (encode or decode in flight), skipped.
static int ProbeSlot(unsigned i,uint64_t* out) {
    uint8_t* alias=nullptr;
    {
        Guard g;
        if(i>=allocated||!slots[i].active)return 0;
        auto& s=slots[i];
        if(s.packed){*out=StoredHash(s.packed);return 2;}
        if(!s.section||s.evicting||s.restoring||s.viewMissing)return 3;
        alias=static_cast<uint8_t*>(MapViewOfFile(s.section,FILE_MAP_READ,0,0,SlotBytes));
        if(!alias)return 3;
    }
    *out=PAGER_CODEC::Hash(reinterpret_cast<const Element*>(alias+Offset));
    UnmapViewOfFile(alias);
    return 1;
}
static bool Probe(ProbeResult* r,void(*yield)()=nullptr) {
    *r={};
    LARGE_INTEGER f{},t0{},t1{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t0);
    unsigned n;{Guard g;n=allocated;r->live=stats.live;}
    if(!n)return true;
    struct Entry{uint64_t hash;uint32_t count;};
    size_t cap=1;while(cap<size_t(n)*2)cap<<=1;
    auto table=static_cast<Entry*>(VirtualAlloc(nullptr,cap*sizeof(Entry),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto zeroTile=static_cast<Element*>(VirtualAlloc(nullptr,Bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!table||!zeroTile){if(table)VirtualFree(table,0,MEM_RELEASE);if(zeroTile)VirtualFree(zeroTile,0,MEM_RELEASE);return false;}
    uint64_t zeroHash=PAGER_CODEC::Hash(zeroTile);
    VirtualFree(zeroTile,0,MEM_RELEASE);
    for(unsigned i=0;i<n;++i) {
        uint64_t h=0;int kind=ProbeSlot(i,&h);
        if(!kind)continue;
        if(i<n/2)++r->lowHalf;
        if(kind==3){++r->skipped;continue;}
        if(kind==1)++r->hashedResident;else ++r->hashedPacked;
        if(h==zeroHash)++r->zero;
        if(!h)h=1;   // 0 marks an empty entry
        for(size_t k=size_t(h^(h>>29))&(cap-1);;k=(k+1)&(cap-1)) {
            if(!table[k].hash){table[k].hash=h;table[k].count=1;++r->distinct;break;}
            if(table[k].hash==h){++table[k].count;break;}
        }
        if(yield&&(i&63)==63)yield();
    }
    for(size_t k=0;k<cap;++k) {
        if(!table[k].hash)continue;
        if(table[k].count==2)++r->pairs;
        if(table[k].count>r->largestGroup)r->largestGroup=table[k].count;
    }
    r->duplicated=r->hashedResident+r->hashedPacked-r->distinct;
    VirtualFree(table,0,MEM_RELEASE);
    QueryPerformanceCounter(&t1);
    r->ms=f.QuadPart?uint64_t((t1.QuadPart-t0.QuadPart)*1000/f.QuadPart):0;
    return true;
}
// attempts=0: the worker's policy, safe to run from several threads at once.
// Visit at least 256 slots, or 1/128 of the allocated range on huge maps, but
// stop after ~40 ms of work. While allocations are bursting (loading), encode
// directly with LoadingMinAgeMs. Otherwise, over three times the budget, encode
// directly; below that, soft-block the excess and encode only slots still
// untouched SoftDelayMs later. (At 2x, a zoomed-out camera on a 256x256 map
// kept 24-34k terrain tiles in use against a 7.9k-tile budget and bypassed the
// second chance entirely: ~3,000 decodes and unmaps per second.)
static void Tick(unsigned attempts=0) {
    unsigned limit=attempts;
    if(!limit){Guard g;limit=allocated/128>256?allocated/128:256;}
    LARGE_INTEGER frequency{},start{},now{};
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    for(unsigned n=0;n<limit;++n) {
        unsigned i;bool loading,pressure,ripe=false,excess=false,limited;
        {
            Guard g;
            size_t budgetSlots=budget/SlotBytes;
            if(!allocated||stats.resident<=budgetSlots)return;
            // Loading: within 15 s of a burst of allocations the second-chance
            // delay (and the full minimum age) would only raise the peak.
            uint64_t tick=GetTickCount64();
            loading=stats.lastBulkAllocation && tick-stats.lastBulkAllocation<15000;
            pressure=stats.resident>3*budgetSlots;
            if(tick-rateWindow>=1000){rateWindow=tick;rateCount=0;}
            limited=evictPerSecond && !loading && !urgent;
            if(limited && rateCount>=evictPerSecond){++stats.rateLimited;return;}
            if(cursor>=allocated)cursor=0;
            i=cursor++;
            auto& s=slots[i];
            ripe=s.soft && !s.evicting && tick-s.blockedAt>=SoftDelayMs;
            excess=stats.resident-stats.softBlocked>budgetSlots;
        }
        bool did=false;LARGE_INTEGER t0{},t1{};QueryPerformanceCounter(&t0);
        if(loading)did=Evict(i,false,LoadingMinAgeMs);
        else if(pressure||ripe)did=Evict(i);
        else if(excess)did=SoftBlock(i);
        if(did) {
            QueryPerformanceCounter(&t1);
            Guard g;if(limited)++rateCount;
            ++stats.evictOps;if(frequency.QuadPart)stats.evictMicros+=uint64_t((t1.QuadPart-t0.QuadPart)*1000000/frequency.QuadPart);
        }
        if(!attempts && (n&15)==15) {
            QueryPerformanceCounter(&now);
            if((now.QuadPart-start.QuadPart)*25>frequency.QuadPart)return;
        }
    }
}
}

#undef PAGER_NS
#undef PAGER_ELEMENT
#undef PAGER_SAMPLES
#undef PAGER_BYTES
#undef PAGER_MAX_SLOTS
#undef PAGER_CODEC
