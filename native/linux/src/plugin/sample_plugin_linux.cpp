// sample_plugin_linux.cpp -> tpf2mp_sample.so: the smallest useful plugin for
// the Linux plugin host, and a template for real ones.
//
// It changes nothing in the game. It makes the checks every plugin holding
// RVAs makes before it patches (ABI and table size, game build, the bytes at the
// site) and stops short of the patch. That makes it safe to drop into
// <data dir>plugins/ as a smoke test of the host: tpf2mp_host.log then shows its
// lines under [tpf2mp_sample].
//
// RVAs here are ELF virtual addresses of the Linux TransportFever2 of build
// 35924 (build-id 3a0e1563...), never the Windows RVAs: re-derive every site.
//
// The site it checks, build 35924 Linux: 0x2fd1420, the translation lookup the
// title menu calls for its labels (menu_linux.cpp, RVA_TR). functions.csv has an
// FDE starting there (size 932), and the file's bytes disassemble as
//   f3 0f 1e fa   endbr64
//   55            push rbp
//   48 89 e5      mov  rbp, rsp
//   41 57         push r15
//   41 56         push r14
//   4c 8d 77 10   lea  r14, [rdi+10h]
// None of the Linux libraries patches it (2026-09-12), so no other library's hook
// can race this check.
//
// Build: no C++ runtime needed, and with hidden visibility the one export is
// Tpf2mpPluginInit. One command:
//   g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wl,--no-undefined
//       -I native/src native/linux/src/plugin/sample_plugin_linux.cpp -o tpf2mp_sample.so
// A plugin that uses the C++ library adds -static-libstdc++ -static-libgcc
// -Wl,--exclude-libs,ALL, as our own libraries do: the game's libstdc++ is the
// Steam runtime's, older than the one we build with.
//
// Settings, in tpf2mp.cfg or in plugins/tpf2mp_sample.cfg beside the .so:
//   [tpf2mp_sample]
//   enabled = 0        read by the host: the .so is then never opened
//   greeting = hello   echoed to the log
#include <cstddef>
#include "plugin/tpf2mp_plugin.h"

static const uintptr_t RVA_TR = 0x2fd1420;
static const uint8_t   TR_EXPECTED[16] = {
    0xF3, 0x0F, 0x1E, 0xFA,        // endbr64
    0x55,                          // push rbp
    0x48, 0x89, 0xE5,              // mov  rbp, rsp
    0x41, 0x57,                    // push r15
    0x41, 0x56,                    // push r14
    0x4C, 0x8D, 0x77, 0x10,        // lea  r14, [rdi+10h]
};

extern "C" __attribute__((visibility("default")))
int Tpf2mpPluginInit(const Tpf2mpHost* host, Tpf2mpPluginInfo* out)
{
    out->name    = "tpf2mp_sample";
    out->version = "1";
    out->summary = "checks the host table and one build-35924 site; patches nothing";

    // Another major is another table layout: touch nothing past the two
    // leading fields. Then `size`: every member used below is in ABI 1's
    // first table, and one added later would need a larger size to exist.
    if (!host || host->abiMajor != TPF2MP_ABI_MAJOR) return TPF2MP_ERR_ABI;
    if (host->size < offsetof(Tpf2mpHost, dataDir) + sizeof(host->dataDir)) return TPF2MP_ERR_ABI;

    host->log("data dir %s, greeting '%s'", host->dataDir(),
              host->cfgStr("tpf2mp_sample", "greeting", "(none)"));

    if (!host->moduleBase()) {
        host->log("not in the game process -- nothing to check");
        return TPF2MP_OK;
    }
    if (!host->buildOk()) {
        host->log("not build 35924 -- a plugin holding RVAs stops here");
        return TPF2MP_ERR_BUILD;
    }
    if (!host->verifyBytes(RVA_TR, TR_EXPECTED, sizeof(TR_EXPECTED))) {
        host->log("bytes at %#lx differ from build 35924 -- a plugin would not patch", (unsigned long)RVA_TR);
        return TPF2MP_ERR_BUILD;
    }
    host->log("bytes at %#lx match build 35924: a real plugin would patch here "
              "(patchBytes, or installHook at moduleBase()+rva)", (unsigned long)RVA_TR);
    return TPF2MP_OK;
}
