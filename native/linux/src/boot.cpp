// libtpf2mp_boot.so -- the Linux counterpart of proxy_alut.cpp: the earliest
// foothold in the game process.
//
// Why LD_PRELOAD and not a proxy: on Windows the exe statically imports
// alut.dll, so a DLL in its place runs before the entry point. The Linux
// build's loader offers the same timing to anything in LD_PRELOAD, set in the
// game's Steam launch options:
//   LD_PRELOAD="/path/to/libtpf2mp_boot.so:$LD_PRELOAD" %command%
// Our constructor runs before main(), long before the title menu is built, and
// no game file is replaced (a Steam "verify files" leaves it working).
//
// LD_PRELOAD is inherited. Steam starts the game through run.sh, so /bin/sh,
// ldconfig and grep map this library too: everything below does nothing unless
// the process image is TransportFever2. In the game we then drop ourselves from
// LD_PRELOAD, so the lobby and anything else it starts stay clean.
//
// Like the proxy, this is a thin loader: it only pulls in the libraries that
// do the work (bridge, menu, slice, plugin host), each from $XDG_DATA_HOME/tpf2mp/
// if present, else from its own folder.
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <unistd.h>
#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include "datadir_linux.h"
#include "lua_fixed_format_linux.h"
#include "destination_rng_linux.h"
#include "person_cost_linux.h"
#include "float_rng_linux.h"
#include "town_seed_linux.h"
#include "tree_rng_linux.h"
#include "animal_rng_linux.h"
#include "building_order_linux.h"
#include "resident_hash_linux.h"
#include "person_map_order_linux.h"
#include "target_order_linux.h"
#include "network_person_order_linux.h"
#include "network_index_order_linux.h"

// GNU build-id of the Steam Linux build 35924 (depot build 16719842). Logged
// against the running image; the libraries that patch code check it themselves.
static const char kBuildId35924[] = "3a0e156390b0e6f1e372051c24802c8493ae454a";

// Only the proven Lua string.format floating conversion may use the legacy
// Windows decimal tie rule. All native engine and other-library printf calls
// retain glibc semantics, including callers from an unknown game build.
static std::atomic<uintptr_t> g_luaFloatFormatReturn{0};

extern "C" __attribute__((visibility("default"), noinline))
int __sprintf_chk(char* buffer, int flag, size_t size, const char* format, ...) noexcept
{
    using VFormat = int (*)(char*, int, size_t, const char*, va_list);
    static const auto realV = reinterpret_cast<VFormat>(dlsym(RTLD_NEXT, "__vsprintf_chk"));
    // The Steam runtime is glibc and imports __sprintf_chk itself. A missing
    // implementation must not turn a fortified call into an unchecked write.
    if (!realV) std::abort();
    va_list args;
    va_start(args, format);
    int precision;
    bool compatible = false;
    double value = 0;
    if (reinterpret_cast<uintptr_t>(__builtin_return_address(0)) ==
            g_luaFloatFormatReturn.load(std::memory_order_relaxed) &&
        Tpf2mpLuaFixedPrecision(format, &precision)) {
        va_list copy;
        va_copy(copy, args);
        value = va_arg(copy, double);
        va_end(copy);
        compatible = true;
    }
    const int result = realV(buffer, flag, size, format, args);
    va_end(args);
    if (compatible) Tpf2mpWindowsFixedTieCorrection(buffer, result, value, precision);
    return result;
}

static void Log(const char* fmt, ...)
{
    char path[4096];
    if (!Tpf2mpDataDirA(path, sizeof(path))) return;
    strncat(path, "tpf2_proxy.log", sizeof(path) - strlen(path) - 1);
    FILE* f = fopen(path, "ab");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static bool IsGameProcess()
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    const char* base = strrchr(exe, '/');
    return base && strcmp(base + 1, "TransportFever2") == 0;
}

// Keep the release's Lua byte-for-byte unchanged. Its Windows environment
// names are process-local aliases for the same directories our Linux libraries
// already use. Publish before dlopen: their worker threads and the lobby inherit
// a complete environment, while HOME/XDG_DATA_HOME retain the user's values.
static bool PublishLuaEnvironment()
{
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    char localAppData[4096], dataDir[4096];
    int n;
    if (xdg && xdg[0] == '/') n = snprintf(localAppData, sizeof(localAppData), "%s", xdg);
    else if (home && home[0] == '/') n = snprintf(localAppData, sizeof(localAppData), "%s/.local/share", home);
    else return false;
    if (n <= 0 || size_t(n) >= sizeof(localAppData) || !Tpf2mpDataDirA(dataDir, sizeof(dataDir))) return false;
    return setenv("LOCALAPPDATA", localAppData, 1) == 0 && setenv("TPF2MP_DATADIR", dataDir, 1) == 0;
}

// ---- clock(): wall time, as on Windows --------------------------------------
// The mod's Lua was written against the MSVC CRT, where clock() -- and so
// os.clock -- is wall time since the process started. net.lua uses it for
// heartbeat round trips and to project a peer's game clock forward when it
// stamps a command. glibc's clock() is CPU time summed over every thread, which
// in a busy multi-threaded game runs several times faster than the wall. The
// game imports clock() from libc, and build 35924 has two callers: Lua's
// os.clock (0x994e20) and a half-second blink test (0x318fd30), both of which
// want wall time. A preloaded clock() wins the symbol lookup, so both get the
// Windows meaning. Outside the game (run.sh's sh, grep, ldconfig) it forwards
// to libc unchanged.
static const timespec g_clockOrigin = [] {
    timespec t = {};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}();

__attribute__((visibility("default")))
clock_t clock(void)
{
    static const bool inGame = IsGameProcess();
    if (!inGame) {
        static const auto real = (clock_t (*)(void))dlsym(RTLD_NEXT, "clock");
        return real ? real() : (clock_t)-1;
    }
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (clock_t)((now.tv_sec - g_clockOrigin.tv_sec) * CLOCKS_PER_SEC
                     + (now.tv_nsec - g_clockOrigin.tv_nsec) / (1000000000L / CLOCKS_PER_SEC));
}

// The main executable is the first object dl_iterate_phdr reports (empty name).
struct MainImage { uintptr_t base = 0; std::string buildId; };

static int FindMain(dl_phdr_info* info, size_t, void* data)
{
    auto* img = static_cast<MainImage*>(data);
    img->base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const char* p = (const char*)(info->dlpi_addr + ph.p_vaddr);
        const char* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto* nh = (const ElfW(Nhdr)*)p;
            const char* name = p + sizeof(*nh);
            const unsigned char* desc = (const unsigned char*)name + ((nh->n_namesz + 3) & ~3u);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                char hex[3];
                for (unsigned k = 0; k < nh->n_descsz; k++) {
                    snprintf(hex, sizeof(hex), "%02x", desc[k]);
                    img->buildId += hex;
                }
            }
            p = (const char*)desc + ((nh->n_descsz + 3) & ~3u);
        }
    }
    return 1;   // stop after the first object
}

// Remove every LD_PRELOAD entry that names this library, keeping the rest
// (Steam's overlay renderer rides in the same variable).
static void DropSelfFromPreload(const char* selfPath)
{
    const char* cur = getenv("LD_PRELOAD");
    if (!cur || !selfPath) return;
    const char* selfBase = strrchr(selfPath, '/');
    selfBase = selfBase ? selfBase + 1 : selfPath;
    std::string kept, entry;
    std::string all(cur);
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t sep = all.find_first_of(": ", pos);
        entry = all.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        pos = sep == std::string::npos ? all.size() + 1 : sep + 1;
        if (entry.empty()) continue;
        const char* eb = strrchr(entry.c_str(), '/');
        if (strcmp(eb ? eb + 1 : entry.c_str(), selfBase) == 0) continue;
        if (!kept.empty()) kept += ':';
        kept += entry;
    }
    if (kept.empty()) unsetenv("LD_PRELOAD");
    else setenv("LD_PRELOAD", kept.c_str(), 1);
}

// $XDG_DATA_HOME/tpf2mp/<name> when that file exists, otherwise next to THIS
// library. There is no third place: the log names the path that was tried.
static std::string ResolveShipped(const char* name, const std::string& selfDir)
{
    char root[4096];
    if (Tpf2mpRootDir(root, sizeof(root))) {
        std::string p = std::string(root) + "/" + name;
        if (access(p.c_str(), R_OK) == 0) return p;
    }
    return selfDir + name;
}

__attribute__((constructor))
static void BootInit()
{
    if (!IsGameProcess()) return;
    if (!PublishLuaEnvironment()) {
        Log("[boot] cannot publish the Lua data-directory environment; libraries not loaded\n");
        return;
    }

    Dl_info self = {};
    dladdr((void*)&BootInit, &self);
    std::string selfPath = self.dli_fname ? self.dli_fname : "";
    std::string selfDir = selfPath.substr(0, selfPath.rfind('/') + 1);
    DropSelfFromPreload(selfPath.c_str());

    MainImage img;
    dl_iterate_phdr(FindMain, &img);
    const bool buildOk = img.buildId == kBuildId35924;
    Log("[boot] attached to pid %d from %s\n", (int)getpid(), selfPath.c_str());
    Log("[boot] image base 0x%lx build-id %s (%s)\n", (unsigned long)img.base,
        img.buildId.empty() ? "?" : img.buildId.c_str(),
        buildOk ? "build 35924" : "UNKNOWN BUILD: code patches will stay off");

    // str_format's floating branch calls luaL_checknumber, places that single
    // double in xmm0 and calls __sprintf_chk (return RVA 0x99a940). Verify the
    // whole argument/call window as well as the ELF build-id before enabling.
    static const unsigned char floatCall[] = {
        0xe8,0xa0,0xa6,0xfe,0xff, 0x48,0x8b,0xbd,0x48,0xdf,0xff,0xff,
        0x48,0x89,0xd9, 0x48,0xc7,0xc2,0xff,0xff,0xff,0xff,
        0xbe,0x01,0x00,0x00,0x00, 0xb8,0x01,0x00,0x00,0x00,
        0xe8,0xe0,0x13,0xd4,0xff
    };
    const bool formatOk = buildOk &&
        std::memcmp(reinterpret_cast<const void*>(img.base + 0x99a91b),
                    floatCall, sizeof(floatCall)) == 0;
    if (formatOk) g_luaFloatFormatReturn.store(img.base + 0x99a940, std::memory_order_relaxed);
    Log("[boot] Lua fixed-decimal Windows tie formatting: %s\n",
        formatOk ? "enabled at verified string.format call" : "off (unverified image/call)");

    // Windows maps MT draws to integer ranges by modulo/rejection; native uses
    // bucket division. Match this shared MT overload before game main starts;
    // other RNG engine overloads retain their original implementation.
    const bool destinationRngOk = Tpf2mpInstallDestinationRng(img.base, img.buildId.c_str());
    Log("[boot] Windows MT integer sampling: %s\n",
        destinationRngOk ? "enabled for all verified MT-overload callers" : "off (unverified image/function or hook failure)");

    const bool floatRngOk = Tpf2mpInstallFloatRng(img.base, img.buildId.c_str());
    Log("[boot] Windows MT unit-float endpoint: %s\n",
        floatRngOk ? "enabled at verified clamp branch" : "off (unverified image/function or patch failure)");

    // Person callbacks also hash their input seeds differently on Windows.
    // Translate only verified callers, retaining the game's MT implementation.
    const bool personSeedsOk = Tpf2mpInstallPersonSeeds(img.base, img.buildId.c_str());
    Log("[boot] Windows person seed hashing: %s\n",
        personSeedsOk ? "enabled at 10 verified callers" : "off (unverified image/call or hook failure)");

    Tpf2mpInstallPersonCosts(img.base, img.buildId.c_str());
    Log("[boot] Windows person travel cost hashing: %s\n", Tpf2mpPersonCostStatus());

    // Town development initializes its MT inline instead of calling the person
    // constructor. Correct that verified tag/time seed before the original init.
    Tpf2mpInstallTownSeed(img.base, img.buildId.c_str());
    Log("[boot] Windows town development seed hashing: %s\n", Tpf2mpTownSeedStatus());

    Tpf2mpInstallTreeRng(img.base, img.buildId.c_str());
    Log("[boot] Windows town tree selection: %s\n", Tpf2mpTreeRngStatus());

    Tpf2mpInstallAnimalRng(img.base, img.buildId.c_str());
    Log("[boot] Windows animal random engine: %s\n", Tpf2mpAnimalRngStatus());

    Tpf2mpInstallBuildingOrder(img.base, img.buildId.c_str());
    Log("[boot] Windows town building registration order: %s\n", Tpf2mpBuildingOrderStatus());

    Tpf2mpInstallResidentHash(img.base, img.buildId.c_str());
    Log("[boot] Windows resident set hashing: %s\n", Tpf2mpResidentHashStatus());

    Tpf2mpPersonMapOrderSetLog(Log);
    Tpf2mpInstallPersonMapOrder(img.base, img.buildId.c_str());
    Log("[boot] Windows temporary person map order: %s\n", Tpf2mpPersonMapOrderStatus());

    Tpf2mpTargetOrderSetLog(Log);
    Tpf2mpInstallTargetOrder(img.base, img.buildId.c_str());
    Log("[boot] Windows person target set order: %s\n", Tpf2mpTargetOrderStatus());

    // PersonMapOrder verifies the complete unmodified helper constructor.
    // Install these narrower sites inside that constructor after its check.
    Tpf2mpNetworkPersonOrderSetLog(Log);
    Tpf2mpInstallNetworkPersonOrder(img.base, img.buildId.c_str());
    Log("[boot] Windows affected-network person order: %s\n", Tpf2mpNetworkPersonOrderStatus());

    // Target-order validation includes the original route-index initialization.
    // Install the persistent index hooks only after that validation has run.
    Tpf2mpNetworkIndexOrderSetLog(Log);
    Tpf2mpInstallNetworkIndexOrder(img.base, img.buildId.c_str());
    Log("[boot] Windows persistent person-route index order: %s\n", Tpf2mpNetworkIndexOrderStatus());

    // dlopen from a constructor is safe with glibc (the loader lock is
    // recursive), so unlike the Windows proxy there is no loader thread: the
    // libraries' own constructors start their threads.
    for (const char* name : { "tpf2_bridge_mp.so", "tpf2_menu.so", "tpf2_slice.so", "tpf2_pluginhost.so" }) {
        std::string path = ResolveShipped(name, selfDir);
        if (access(path.c_str(), R_OK) != 0) {
            Log("[boot] pid=%d %s not present (%s)\n", (int)getpid(), name, path.c_str());
            continue;
        }
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        Log("[boot] pid=%d %s load %s from %s%s%s\n", (int)getpid(), name, h ? "OK" : "FAILED",
            path.c_str(), h ? "" : ": ", h ? "" : dlerror());
    }
}
