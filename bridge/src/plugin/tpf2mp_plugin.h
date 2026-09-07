// tpf2mp_plugin.h -- the plugin ABI.
//
// This is the ONLY file a plugin needs from the host. Plugins live in their own
// repositories and VENDOR a copy of this header; there is no build-time coupling
// between a plugin and the host tree, and no submodule.
//
// A plugin is a DLL in <datadir>\plugins\ (or <gamedir>\tpf2mp\plugins\) that
// exports:
//
//     extern "C" __declspec(dllexport)
//     int Tpf2mpPluginInit(const Tpf2mpHost* host, Tpf2mpPluginInfo* out);
//
// It is called ONCE, on the proxy's loader thread, before the exe entry point
// has run -- so a plugin may patch code, but must not assume the game's own
// subsystems exist yet. Return TPF2MP_OK to stay loaded; anything else and the
// host logs the reason and leaves the plugin inert (it is not unloaded, because
// unloading a DLL that has already patched code is worse than leaving it).
//
// WHY AN ABI AND NOT JUST A HEADER SHARED BY BUILD
// The host and a plugin are compiled separately, possibly by different people
// with different compiler versions. Everything crossing this boundary is plain
// C: no std::, no exceptions, no ownership transfer. The host owns every pointer
// it hands out and keeps it alive for the life of the process.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped only when the layout of Tpf2mpHost changes incompatibly. The host
// refuses a plugin built against a different major; additive changes (new
// function pointers appended, size grows) keep the major and are detected by
// `size`.
#define TPF2MP_ABI_MAJOR 1

#define TPF2MP_OK              0
#define TPF2MP_ERR_ABI         1   // built against an incompatible ABI
#define TPF2MP_ERR_BUILD       2   // game build is not the one this plugin knows
#define TPF2MP_ERR_DISABLED    3   // switched off in config (not an error; logged quietly)
#define TPF2MP_ERR_FAILED      4   // anything else; plugin should log the detail itself

// What the plugin tells the host about itself. Strings must have static
// lifetime (a literal is ideal) -- the host does not copy them.
typedef struct Tpf2mpPluginInfo {
    const char* name;       // short id, also the config section name ("bigmap")
    const char* version;    // free text, shown in the log and the overlay
    const char* summary;    // one line, shown in the overlay
} Tpf2mpPluginInfo;

// Services the host provides. `size` is sizeof(Tpf2mpHost) as the HOST built it:
// a plugin compiled against an older (smaller) header must check `size` before
// touching a field added later. Never reorder or remove members.
typedef struct Tpf2mpHost {
    uint32_t size;
    uint32_t abiMajor;

    // ---- diagnostics ----
    // Writes one line to the shared log, prefixed with the plugin name.
    // Newline is added if absent. printf formatting.
    void (*log)(const char* fmt, ...);

    // ---- configuration ----
    // Values come from the ONE config file, section = the plugin's own name.
    // `def` is returned when the key is absent or unparseable.
    int         (*cfgInt)(const char* section, const char* key, int def);
    int         (*cfgBool)(const char* section, const char* key, int def);
    const char* (*cfgStr)(const char* section, const char* key, const char* def);

    // ---- the game image ----
    // Base address of TransportFever2.exe, or 0 if we are not in the game
    // (the DLLs get loaded by test harnesses too -- always check).
    uintptr_t (*moduleBase)(void);

    // True only when the running exe is the build the host knows (PE
    // TimeDateStamp + SizeOfImage). A plugin holding hardcoded RVAs must
    // refuse to patch when this is false.
    int (*buildOk)(void);

    // Compare `len` bytes at moduleBase()+rva against `expected`. Returns 1 on
    // an exact match, 0 otherwise (including unreadable memory). This is the
    // guard to run before every patch: an RVA that has shifted points into the
    // middle of some other instruction, and writing there corrupts something
    // random instead of failing cleanly.
    int (*verifyBytes)(uintptr_t rva, const uint8_t* expected, uint32_t len);

    // ---- patching ----
    // 14-byte absolute-jump detour + trampoline. `stealBytes` must be >= 14 and
    // must land on an instruction boundary; the stolen bytes must not be
    // RIP-relative (they are relocated verbatim). Returns 1 on success and
    // writes the trampoline to *trampolineOut.
    int (*installHook)(uintptr_t target, void* detour, int stealBytes, void** trampolineOut);

    // Overwrite `len` bytes at moduleBase()+rva, handling page protection.
    // Verify first. Returns 1 on success.
    int (*patchBytes)(uintptr_t rva, const uint8_t* bytes, uint32_t len);

    // ---- paths ----
    // Runtime data directory, UTF-8, WITH a trailing backslash. Everything a
    // plugin writes at run time belongs here; the game directory may be
    // read-only (Program Files).
    const char* (*dataDir)(void);
} Tpf2mpHost;

typedef int (*Tpf2mpPluginInitFn)(const Tpf2mpHost* host, Tpf2mpPluginInfo* out);

#ifdef __cplusplus
}   // extern "C"
#endif
