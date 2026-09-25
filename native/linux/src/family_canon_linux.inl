// Linux build 35924 family walk. Included by order_canon_linux.cpp.
// Contract and live-validation limits: docs/re/linux/DEV_0115785C.md.
// Query current permissions without enumerating mappings on Linux >= 6.11.
// Older kernels retain a per-iteration snapshot; never cache a positive range
// across iterations (munmap/mprotect would make that unsafe).
// Stable Linux UAPI from linux/fs.h, spelled locally for the soldier SDK.
struct FamilyMapQuery {
    uint64_t size, query_flags, query_addr, vma_start, vma_end, vma_flags;
    uint64_t vma_page_size, vma_offset, inode;
    uint32_t dev_major, dev_minor, vma_name_size, build_id_size;
    uint64_t vma_name_addr, build_id_addr;
};
static_assert(sizeof(FamilyMapQuery) == 104);
constexpr auto kFamilyMapQuery = _IOWR('f', 17, FamilyMapQuery);
struct FamilyMapFd {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    bool query = false;
    FamilyMapFd() {
        FamilyMapQuery q{}; q.size = sizeof(q);
        // No VMA is normally at zero; ENOENT also proves ioctl support.
        query = fd >= 0 && (ioctl(fd, kFamilyMapQuery, &q) == 0 || errno == ENOENT);
    }
    ~FamilyMapFd() { if (fd >= 0) close(fd); }
};
static thread_local FamilyMapFd g_familyMapFd;
struct FamilyMapping { uintptr_t begin, end; bool writable; };
static thread_local std::vector<FamilyMapping> g_familyMappings;
static bool FamilyRange(uintptr_t at, size_t size, bool write = false)
{
    if (!size) return true;
    if (!at || size > UINTPTR_MAX - at) return false;
    const uintptr_t end = at + size;
    if (g_familyMapFd.query) {
        while (at < end) {
            FamilyMapQuery q{};
            q.size = sizeof(q); q.query_addr = at;
            q.query_flags = write ? 3 : 1; // readable, optionally writable
            int result;
            do { result = ioctl(g_familyMapFd.fd, kFamilyMapQuery, &q); }
            while (result < 0 && errno == EINTR);
            if (result < 0 || q.vma_start > at || q.vma_end <= at ||
                (q.vma_flags & q.query_flags) != q.query_flags) return false;
            at = q.vma_end;
        }
        return true;
    }
    auto first = std::lower_bound(g_familyMappings.begin(), g_familyMappings.end(), at,
        [](const FamilyMapping& m, uintptr_t address) { return m.end <= address; });
    for (auto it = first; it != g_familyMappings.end(); ++it) {
        const auto& m = *it;
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
#include "family_getters_linux.h"
#include "town_trace_linux.h"
#include "../../src/town_trace.h"
static uintptr_t g_familyBase = 0;

// GetNodeList (vtable slot 2) of a family, classified by the generated inventory.
// MSVC folds every family's getter into one function; GCC keeps one copy per
// family template (28 node-list copies, 35 no-list copies in build 35924), so a
// single recognised address left 27 of the 28 node lists unsorted on the native
// build while every Windows peer sorted all 28 (the server logged `lists=1 ...
// unknown/refused=61`). Each address is verified byte for byte at install; none
// is ever called.
enum FamilyGetterKind { FG_UNKNOWN, FG_LIST, FG_NONE };
static FamilyGetterKind FamilyGetter(uintptr_t get)
{
    if (!g_familyBase || get < g_familyBase) return FG_UNKNOWN;
    const uintptr_t rva = get - g_familyBase;
    for (uintptr_t g : kFamilyListGetters) if (g == rva) return FG_LIST;
    for (uintptr_t g : kFamilyNoListGetters) if (g == rva) return FG_NONE;
    return FG_UNKNOWN;
}
static uint64_t g_familyUs = 0, g_familyMaxUs = 0;

static bool FamilyMappings()
{
    if (g_familyMapFd.query) return true;
    g_familyMappings.clear();
    const int fd = g_familyMapFd.fd;
    if (fd < 0 || lseek(fd, 0, SEEK_SET) < 0) return false;
    // Compatibility path: buffered read, parse only the address/permission
    // prefix, discard arbitrarily long pathnames. No scanf per mapping.
    char buffer[16384], prefix[80];
    size_t used = 0;
    for (;;) {
        ssize_t got;
        do { got = read(fd, buffer, sizeof(buffer)); } while (got < 0 && errno == EINTR);
        if (got < 0) { g_familyMappings.clear(); return false; }
        if (!got) return used == 0;
        for (ssize_t i = 0; i < got; ++i) {
            if (buffer[i] != '\n') {
                if (used < sizeof(prefix)) prefix[used++] = buffer[i];
                continue;
            }
            size_t cursor = 0;
            auto hex = [&](uintptr_t& out) {
                out = 0; const auto start = cursor;
                while (cursor < used) {
                    const char c = prefix[cursor];
                    unsigned digit = c >= '0' && c <= '9' ? c-'0' :
                                     c >= 'a' && c <= 'f' ? c-'a'+10 : 16;
                    if (digit == 16) break;
                    if (out > (UINTPTR_MAX-digit)/16) return false;
                    out = out*16+digit; ++cursor;
                }
                return cursor != start;
            };
            uintptr_t begin, end;
            if (!hex(begin) || cursor >= used || prefix[cursor++] != '-' ||
                !hex(end) || cursor+3 >= used || prefix[cursor++] != ' ' || end <= begin) {
                g_familyMappings.clear(); return false;
            }
            if (prefix[cursor] == 'r')
                g_familyMappings.push_back({begin, end, prefix[cursor+1] == 'w'});
            used = 0;
        }
    }
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
        // Town trace (diagnostic, off by default): the lists as the systems will
        // see them this iteration, one TF line per 600 TownSystem iterations.
        int64_t traceTime = -1; int traceEngine = -1;
        const bool trace = Tpf2mpTownTraceFamiliesDue(engine, &traceTime, &traceEngine);
        static thread_local std::vector<uint64_t> traceTokens;
        traceTokens.clear();
        for (auto fam : families) {
            if (!FamilyRange(fam,8)) { ++bad; continue; }
            auto vft=FamilyWord(fam);
            if (!FamilyRange(vft,24)) { ++bad; continue; }
            const auto kind=FamilyGetter(FamilyWord(vft+16));
            if (kind==FG_NONE) continue;
            if (kind!=FG_LIST) { ++bad; continue; }
            auto nl=fam+8;
            if (!FamilyRange(nl,0x40)) { ++bad; continue; }
            const auto v=FamilyWord(nl), first=g_familyBase+0x59ac260;
            if (v<first || v>first+0x80 || (v-first)%0x20) { ++bad; continue; }
            ++lists;
            size_t m=0;
            const size_t stride=8+4*((v-first)/0x20);
            auto result=CanonFamilyList(nl,stride,scratch,&m);
            if (result==FC_REFUSED) ++bad;
            else if (result==FC_REORDERED) { ++changed; moved+=m; }
            if (trace && result!=FC_REFUSED) {
                const uintptr_t b=FamilyWord(nl+8), e=FamilyWord(nl+16);
                const size_t n=b&&e>b?(e-b)/stride:0;
                traceTokens.push_back((uint64_t(n)<<32)|TownTraceListDigest(reinterpret_cast<const uint8_t*>(b),n,stride));
            }
        }
        if (trace) Tpf2mpTownTraceFamilies(traceTime,traceEngine,traceTokens.data(),traceTokens.size());
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
