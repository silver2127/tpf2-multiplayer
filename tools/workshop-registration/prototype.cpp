#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include "tpf2mp_plugin.h"
#include "catalogue.h"

static const Tpf2mpHost* host;
static uintptr_t base;
static std::string itemId;
static std::wstring folder;
static bool replaceTestItem;
using Refresh = bool (*)(void*, const Result*);
static Refresh original;
struct ModKey { Text name; int32_t version, padding; };
static_assert(sizeof(ModKey)==40);

static bool Hook(void* rep, const Result* source) {
    // Config is opt-in. Recheck the top-level mod file at each catalogue refresh.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(folder)/L"mod.lua", ec))
        return original(rep, source);
    int steam = -1;
    auto prefixes = (const Text*)(base+0x4147330);
    for (int i=0;i<2;++i) if (prefixes[i].equals("*")) steam=i;
    // Only preparation is caught: never retry an engine call that threw midway.
    std::unique_ptr<Shadow> shadow;
    try { shadow = std::make_unique<Shadow>(*source,steam,itemId,folder,replaceTestItem); }
    catch (const std::exception& e) {
        host->log("workshop_register: unchanged: %s",e.what());
        return original(rep,source);
    }
    bool changed = original(rep,&shadow->result);
    auto first = *(const ModKey**)((char*)rep+0xe0);
    auto last = *(const ModKey**)((char*)rep+0xe8);
    bool found = false;
    for (auto p=first; p!=last; ++p)
        if (p->name.equals("*"+itemId) && p->version==1) found=true;
    host->log("workshop_register: id=%s offered=%d catalogue=%s",itemId.c_str(),
              shadow->added,found ? "PRESENT" : "MISSING");
    return changed;
}

extern "C" __declspec(dllexport)
int Tpf2mpPluginInit(const Tpf2mpHost* h,Tpf2mpPluginInfo* out) {
    out->name="workshop_register"; out->version="prototype-2";
    out->summary="Offline Workshop catalogue registration experiment";
    if (!h || h->abiMajor!=TPF2MP_ABI_MAJOR || h->size<sizeof(Tpf2mpHost)) return TPF2MP_ERR_ABI;
    host=h;
    if (!h->cfgBool("workshop_register","enabled",0)) return TPF2MP_ERR_DISABLED;
    itemId=h->cfgStr("workshop_register","id","");
    replaceTestItem=h->cfgBool("workshop_register","replace_test_item",0)!=0;
    host->log("workshop_register: replace_test_item=%d",replaceTestItem);
    if (itemId.empty() || itemId.size()>20 || itemId.find_first_not_of("0123456789")!=std::string::npos)
        return TPF2MP_ERR_FAILED;
    const char* path=h->cfgStr("workshop_register","path","");
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,nullptr,0);
    if (n<2) return TPF2MP_ERR_FAILED;
    std::vector<wchar_t> wide(n);
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide.data(),n);
    folder=wide.data();
    if (!std::filesystem::path(folder).is_absolute()) return TPF2MP_ERR_FAILED;
    base=h->moduleBase();
    const uint8_t expected[]={0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,
                             0x41,0x57,0x48,0x8d,0xac,0x24,0xd0,0xfe,0xff,0xff};
    if (!base || !h->buildOk() || !h->verifyBytes(0x2373210,expected,sizeof(expected)))
        return TPF2MP_ERR_BUILD;
    return h->installHook(base+0x2373210,(void*)Hook,sizeof(expected),(void**)&original)
        ? TPF2MP_OK : TPF2MP_ERR_FAILED;
}
