#include "buyhook.h"
#include "hook.h"
#include <windows.h>
#include <cstring>
#include <cstdio>

static const uintptr_t BUY_RVA = 0x9dca00;
static const int       STEAL   = 15;

//   48 8B C4              mov  rax, rsp
//   55                    push rbp
//   41 54                 push r12
//   41 57                 push r15
//   48 8D A8 28 F5 FF FF  lea  rbp, [rax-0ad8h]
// 15 is an instruction boundary and none of it is rip-relative, so it
// relocates verbatim into the trampoline.
static const uint8_t EXPECTED[STEAL] = {
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x57,
    0x48, 0x8D, 0xA8, 0x28, 0xF5, 0xFF, 0xFF,
};

static BuyLogFn g_log = nullptr;
static wchar_t  g_outPath[MAX_PATH] = {0};

// Append one purchase record. Opened per write with FILE_APPEND_DATA so the
// Lua side can read it concurrently and so two processes cannot clobber each
// other (the same reason the bridge log uses append mode).
static void EmitBuy(const char* line)
{
    if (!g_outPath[0]) return;
    HANDLE h = CreateFileW(g_outPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line, (DWORD)strlen(line), &written, nullptr);
    CloseHandle(h);
}

extern "C" {
    void* g_buyTramp = nullptr;
    void  BuyRelay();
    volatile uint64_t g_buyHits = 0;
    // rsp as it was at function entry, so the handler can reach stack args
    uint64_t g_buyEntrySp = 0;
}

static bool Readable(const void* p, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD)) return false;
    return (uintptr_t)p + len <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// Dump a window of memory plus any values in it that look like pointers into
// readable memory -- the config is "two vector triplets + a heap pointer" per
// M2, so begin/end/capacity triples should stand out.
static void DumpBlock(const char* label, const void* p, size_t bytes)
{
    if (!g_log) return;
    if (!p || !Readable(p, bytes)) {
        g_log("[buy]   %s = %p (not readable)\n", label, p);
        return;
    }
    const uint64_t* q = (const uint64_t*)p;
    size_t words = bytes / 8;
    for (size_t i = 0; i < words; i += 4) {
        char line[256];
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[buy]   %s+%02zx:", label, i * 8);
        for (size_t j = i; j < i + 4 && j < words; ++j)
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %016llx",
                             (unsigned long long)q[j]);
        g_log("%s\n", line);
    }
}

// A std::vector is {begin, end, capacity}. Report its size and the first slice
// of its contents as 32-bit words -- a modelId is a small int and stands out
// against pointers and floats.
static void DumpVector(const char* label, const void* tripletAt)
{
    if (!g_log || !Readable(tripletAt, 24)) return;
    const uint64_t* t = (const uint64_t*)tripletAt;
    uint64_t b = t[0], e = t[1], c = t[2];
    if (e < b || (e - b) > 0x100000) {
        g_log("[buy]   %s: not a vector (b=%llx e=%llx)\n", label,
              (unsigned long long)b, (unsigned long long)e);
        return;
    }
    size_t size = (size_t)(e - b);
    g_log("[buy]   %s: size=%llu bytes cap=%llu begin=%llx\n", label,
          (unsigned long long)size,
          (unsigned long long)(c >= b ? c - b : 0), (unsigned long long)b);
    if (!b || size == 0) return;
    size_t take = size < 128 ? size : 128;
    if (!Readable((const void*)b, take)) {
        g_log("[buy]     (contents not readable)\n");
        return;
    }
    const uint32_t* w = (const uint32_t*)b;
    size_t words = take / 4;
    for (size_t i = 0; i < words; i += 8) {
        char line[256];
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[buy]     +%03zx:", i * 4);
        for (size_t j = i; j < i + 8 && j < words; ++j)
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %08x", w[j]);
        g_log("%s\n", line);
    }
}

extern "C" void BuyHandler(void* rcx, void* rdx, uint64_t r8, uint64_t r9)
{
    g_buyHits = g_buyHits + 1;
    if (!g_log) return;
    // Purchases are rare (M2: exactly one factory hit per vehicle bought), so
    // logging every hit is fine -- unlike the per-tick sim hook. Cap it anyway
    // in case the factory turns out to fire on UI previews as well.
    if (g_buyHits > 8) return;
    g_log("[buy] hit #%llu  rcx=%p rdx=%p player=%lld depot=%lld\n",
          (unsigned long long)g_buyHits, rcx, rdx, (long long)(int32_t)r8,
          (long long)(int32_t)r9);

    // STACK ARGUMENTS. rcx looks like a hidden struct-return pointer (it dumped
    // as mostly zeros) and rdx like a context object, not a config -- which
    // puts the real arguments here. [entry+00]=return addr, [+08..+20]=shadow
    // for rcx/rdx/r8/r9, [+28] onward = args 5, 6, 7...
    if (g_buyEntrySp && Readable((const void*)g_buyEntrySp, 0x88)) {
        const uint64_t* s = (const uint64_t*)g_buyEntrySp;
        for (int i = 5; i <= 12; ++i) {
            uint64_t v = s[i];       // s[5] == [entry+0x28] == arg5
            g_log("[buy]   arg%-2d [+%02x] = %016llx%s\n", i, i * 8,
                  (unsigned long long)v,
                  (v && Readable((const void*)v, 8)) ? "  (readable ptr)" : "");
        }
        // arg5 IS the vehicle config. Layout established by comparing four cars
        // against one train with five box cars:
        //   +00  std::vector A  -> 128 bytes PER UNIT   (car 128, train 768)
        //   +18  std::vector B  ->   4 bytes PER UNIT   (car   4, train  24)
        // so B is one 32-bit value per unit -- the model ids.
        uint64_t cfg = s[5];
        if (cfg && Readable((const void*)cfg, 0x40)) {
            const uint64_t* q = (const uint64_t*)cfg;
            uint64_t aBeg = q[0], aEnd = q[1], bBeg = q[3], bEnd = q[4];
            uint64_t aSize = (aEnd >= aBeg) ? aEnd - aBeg : 0;
            uint64_t bSize = (bEnd >= bBeg) ? bEnd - bBeg : 0;
            g_log("[buy]   config: unitBytes=%llu idBytes=%llu -> %llu unit(s)\n",
                  (unsigned long long)aSize, (unsigned long long)bSize,
                  (unsigned long long)(bSize / 4));

            // Vector A holds one 128-byte struct per unit, and the model id is
            // its first uint32 -- distinct per vehicle type (3624 loco, 3347 /
            // 3337 / 3357 / 3315 for four different cars). Vector B is all 1s,
            // i.e. a per-unit quantity, not the model.
            const size_t UNIT = 128;
            uint64_t units = aSize / UNIT;
            if (aBeg && units && units <= 64 && Readable((const void*)aBeg, (size_t)aSize)) {
                char ids[512];
                int n = 0;
                ids[0] = 0;
                for (uint64_t k = 0; k < units; ++k) {
                    uint32_t mid = *(const uint32_t*)(aBeg + k * UNIT);
                    n += _snprintf_s(ids + n, sizeof(ids) - n, _TRUNCATE,
                                     k ? ",%u" : "%u", mid);
                }
                g_log("[buy]   -> %llu unit(s) models=%s\n",
                      (unsigned long long)units, ids);
                char rec[640];
                _snprintf_s(rec, sizeof(rec), _TRUNCATE,
                            "BUY depot=%lld mids=%s\n", (long long)(int32_t)r9, ids);
                EmitBuy(rec);

                // FULL DUMP of the per-unit struct.
                //
                // Why this exists: a script-built TransportVehicleConfig wedges
                // the sim, and every field readable from Lua has been matched
                // against a vehicle the game built -- loadConfig, purchaseTime,
                // maintenanceState, reversed/color/logo -- without fixing it.
                // So the difference is in something Lua does not expose.
                //
                // This factory is on the path for BOTH a manual UI purchase and
                // a scripted one, so dumping the raw 128 bytes here gives two
                // records that can be diffed byte for byte. That is the only
                // remaining source of ground truth.
                g_log("[buy]   --- raw unit struct(s), %llu byte(s) each ---\n",
                      (unsigned long long)UNIT);
                uint64_t showUnits = units < 2 ? units : 2;
                for (uint64_t k = 0; k < showUnits; ++k) {
                    const uint32_t* u = (const uint32_t*)(aBeg + k * UNIT);
                    for (size_t i = 0; i < UNIT / 4; i += 8) {
                        char line[256];
                        int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                            "[buy]   unit%llu+%03zx:",
                                            (unsigned long long)k, i * 4);
                        for (size_t j = i; j < i + 8; ++j)
                            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                                             " %08x", u[j]);
                        g_log("%s\n", line);
                    }
                }
                // vector B: one 32-bit value per unit (all 1s in every sample
                // so far -- most likely vehicleGroups)
                if (bBeg && bSize && Readable((const void*)bBeg, (size_t)bSize)) {
                    char line[256];
                    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[buy]   vecB:");
                    const uint32_t* w = (const uint32_t*)bBeg;
                    uint64_t cnt = bSize / 4;
                    for (uint64_t k = 0; k < cnt && k < 16; ++k)
                        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                                         " %u", w[k]);
                    g_log("%s\n", line);
                }
            }
        }
    }
}

uint64_t BuyHook_HitCount() { return g_buyHits; }

bool BuyHook_Install(BuyLogFn log, const wchar_t* outPath)
{
    if (outPath) {
        wcscpy_s(g_outPath, outPath);
        // Truncate per session. The Lua side used to skip to end-of-file on
        // first sight to avoid replaying stale records -- but this file only
        // comes into existence when the FIRST purchase is made, so that skip
        // swallowed exactly the record that created it. Emptying it here means
        // anything present is from this run and Lua can safely start at 0.
        HANDLE h = CreateFileW(outPath, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) return false;
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(base, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    if (_wcsicmp(leaf, L"TransportFever2.exe") != 0) {
        if (log) log("[buy] host is '%S', not the game -- skipping\n", leaf);
        return false;
    }
    return BuyHook_InstallAt((uintptr_t)base + BUY_RVA, log);
}

bool BuyHook_InstallAt(uintptr_t target, BuyLogFn log)
{
    g_log = log;
    if (!Readable((const void*)target, STEAL)) {
        if (log) log("[buy] target %p not readable -- skipping\n", (void*)target);
        return false;
    }
    if (memcmp((const void*)target, EXPECTED, STEAL) != 0) {
        if (log) {
            const uint8_t* got = (const uint8_t*)target;
            char buf[64]; int n = 0;
            for (int i = 0; i < 8; ++i)
                n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "%02X ", got[i]);
            log("[buy] REFUSING to patch: prologue mismatch at %p (found %s...)\n",
                (void*)target, buf);
        }
        return false;
    }
    void* tramp = nullptr;
    if (!InstallHook(target, (void*)&BuyRelay, STEAL, &tramp)) {
        if (log) log("[buy] InstallHook failed\n");
        return false;
    }
    g_buyTramp = tramp;
    if (log) log("[buy] hooked buyVehicle factory at %p (tramp %p)\n", (void*)target, tramp);
    return true;
}
