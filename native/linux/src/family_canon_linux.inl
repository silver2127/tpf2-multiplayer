// Linux build 35924 family walk. Included by order_canon_linux.cpp.
// Contract and live-validation limits: docs/re/linux/DEV_0115785C.md.
// Snapshot mappings each iteration; no cached engine/family pointers survive it.
struct FamilyMapping { uintptr_t begin, end; bool writable; };
static thread_local std::vector<FamilyMapping> g_familyMappings;
static bool FamilyRange(uintptr_t at, size_t size, bool write = false)
{
    if (!size) return true;
    if (!at || size > UINTPTR_MAX - at) return false;
    const uintptr_t end = at + size;
    for (const auto& m : g_familyMappings) {
        if (at < m.begin) return false;
        if (at >= m.end) continue;
        if (write && !m.writable) return false;
        if (end <= m.end) return true;
        at = m.end;
    }
    return false;
}
static bool Readable(const void* p, size_t n) { return FamilyRange(reinterpret_cast<uintptr_t>(p), n); }
#include "../../src/family_canon.h"
static uintptr_t g_familyBase = 0;
static uint64_t g_familyUs = 0, g_familyMaxUs = 0;

static bool FamilyMappings()
{
    g_familyMappings.clear();
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return false;
    std::unique_ptr<FILE, int(*)(FILE*)> closeFile(f, std::fclose);
    char line[1024], perm[5]; unsigned long begin, end;
    while (std::fgets(line, sizeof(line), f))
        if (std::sscanf(line, "%lx-%lx %4s", &begin, &end, perm) == 3 && perm[0] == 'r')
            g_familyMappings.push_back({begin, end, perm[1] == 'w'});
    const bool ok = !std::ferror(f);
    return ok;
}
static uintptr_t FamilyWord(uintptr_t at) { uintptr_t v; std::memcpy(&v, (void*)at, 8); return v; }
static int CanonFamilyList(uintptr_t nl, size_t stride, FamilyCanonScratch& scratch, size_t* moved)
{
    if (!FamilyRange(nl, 0x40)) return FC_REFUSED;
    const uintptr_t b = FamilyWord(nl+8), e = FamilyWord(nl+16), cap = FamilyWord(nl+24);
    if (e < b || cap < e || (e-b)%stride || (b&3) || (e-b)/stride > FC_MAX_NODES ||
        !FamilyRange(b,e-b,true)) return FC_REFUSED;
    const size_t slots = FamilyWord(nl+0x38);
    if (slots > (FC_MAX_NODES<<2) ||
        !FamilyRange(FamilyWord(nl+0x28),slots*8,true)) return FC_REFUSED;
    const char* why = nullptr;
    return FamilyCanonList(reinterpret_cast<uint8_t*>(nl),stride,scratch,moved,&why);
}
static void CanonFamilies(uintptr_t engine) noexcept
{
    auto& c = g_canonCounters[6];
    const auto call = ++c.calls;
    const auto start = std::chrono::steady_clock::now();
    size_t walked=0, lists=0, changed=0, moved=0, bad=0, count=0;
    try {
        static thread_local FamilyCanonScratch scratch;
        if (!FamilyMappings() || !FamilyRange(engine,0x180)) { ++c.refused; return; }
        count = FamilyWord(engine+0x178);
        uintptr_t node = FamilyWord(engine+0x170);
        if (count > 4096) { ++c.refused; return; }
        // Validate the whole chain before mutating any lists.
        std::vector<uintptr_t> families;
        while (node && walked < count) {
            if (!FamilyRange(node,24)) break;
            families.push_back(FamilyWord(node+16));
            node=FamilyWord(node); ++walked;
        }
        if (node || walked != count) { ++c.refused; return; }
        for (auto fam : families) {
            if (!FamilyRange(fam,8)) { ++bad; continue; }
            auto vft=FamilyWord(fam);
            if (!FamilyRange(vft,24)) { ++bad; continue; }
            auto get=FamilyWord(vft+16);
            if (get==g_familyBase+0xa914e0) continue;
            if (get!=g_familyBase+0xa914c0) { ++bad; continue; }
            auto nl=fam+8;
            if (!FamilyRange(nl,0x40)) { ++bad; continue; }
            const auto v=FamilyWord(nl), first=g_familyBase+0x59ac260;
            if (v<first || v>first+0x80 || (v-first)%0x20) { ++bad; continue; }
            ++lists;
            size_t m=0;
            auto result=CanonFamilyList(nl,8+4*((v-first)/0x20),scratch,&m);
            if (result==FC_REFUSED) ++bad;
            else if (result==FC_REORDERED) { ++changed; moved+=m; }
        }
    } catch (...) { ++bad; } // Allocation failure cannot unwind through the assembly relay.
    if (changed) ++c.reordered;
    if (bad) ++c.refused;
    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    g_familyUs+=us; g_familyMaxUs=std::max(g_familyMaxUs,uint64_t(us));
    if (call<=2 || call%3000==0 || (bad && c.refused==1))
        if (auto log=g_canonLog.load())
            log("[order-canon] step engine=%p calls=%llu families=%zu/%zu lists=%zu reordered=%zu moved=%zu unknown/refused=%zu avg=%llu us max=%llu us\n",
                (void*)engine,(unsigned long long)call,walked,count,lists,changed,moved,bad,
                (unsigned long long)(g_familyUs/call),(unsigned long long)g_familyMaxUs);
}
