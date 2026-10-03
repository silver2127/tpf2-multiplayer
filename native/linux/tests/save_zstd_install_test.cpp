// Exercise every partial-install boundary, especially the first init succeeding
// and the second failing. No engine memory or executable patches are used.
#include "near_alloc.h"
#include <cassert>
#include <dlfcn.h>
#include "slice/save_zstd_linux.h"
static unsigned Version() { return 10502; }
static void* Create() { return reinterpret_cast<void*>(1); }
static size_t Free(void*) { return 0; }
static size_t Parameter(void*, int, int) { return 0; }
static size_t Reset(void*, int) { return 0; }
static size_t Compress(void*, savezstd::OutBuf*, savezstd::InBuf*, int) { return 0; }
static unsigned Error(size_t) { return 0; }
static void* Open(const char*, int) { return reinterpret_cast<void*>(1); }
static void* Symbol(void*, const char* name)
{
    if (!strcmp(name, "ZSTD_versionNumber")) return (void*)&Version;
    if (!strcmp(name, "ZSTD_createCCtx")) return (void*)&Create;
    if (!strcmp(name, "ZSTD_freeCCtx")) return (void*)&Free;
    if (!strcmp(name, "ZSTD_CCtx_setParameter")) return (void*)&Parameter;
    if (!strcmp(name, "ZSTD_CCtx_reset")) return (void*)&Reset;
    if (!strcmp(name, "ZSTD_compressStream2")) return (void*)&Compress;
    if (!strcmp(name, "ZSTD_isError")) return (void*)&Error;
    return nullptr;
}
static int redirects, failAt;
static bool badGuard;
static bool Redirect(uintptr_t, uintptr_t, void*) { return redirects++ != failAt; }
#define dlopen Open
#define dlsym Symbol
#define Tpf2mpRedirectCall Redirect
#include "../src/slice/save_zstd_linux.cpp"
#undef Tpf2mpRedirectCall
#undef dlopen
#undef dlsym
void SliceLog(const char*, ...) {}
bool SliceRead(uintptr_t at, void* out, size_t size)
{
    for (const auto& g : kGuards) if (at == g.rva && size == g.size) {
        memcpy(out, g.bytes, size);
        if (badGuard) static_cast<char*>(out)[0] ^= 1;
        return true;
    }
    return false;
}
static int inits;
static size_t OriginalInit(void* cs, int level)
{ assert(cs == reinterpret_cast<void*>(123) && level == 7); ++inits; return 42; }
int main()
{
    redirects = 0; badGuard = true;
    assert(!SliceInstallSaveZstd(0, nullptr, nullptr));
    assert(redirects == 0 && !g_takeover.load());
    badGuard = false;
    for (failAt = 0; failAt < 6; ++failAt) {
        redirects = 0;
        assert(!SliceInstallSaveZstd(0, nullptr, nullptr));
        assert(redirects == failAt + 1 && !g_takeover.load());
        g_gameInit = OriginalInit;
        assert(HookInit(reinterpret_cast<void*>(123), 7) == 42);
        assert(g_streams->Snapshot().streams == 0);
        delete g_streams; g_streams = nullptr;
    }
    assert(inits == 6);
    redirects = 0; failAt = -1;
    assert(SliceInstallSaveZstd(0, nullptr, nullptr));
    assert(redirects == 6 && g_takeover.load());
    delete g_streams; g_streams = nullptr;
}
