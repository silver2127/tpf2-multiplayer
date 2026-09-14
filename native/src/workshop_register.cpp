#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include "plugin/tpf2mp_plugin.h"
#include <fstream>
#include <sstream>
#include "../../tools/workshop-registration/catalogue.h"

static const Tpf2mpHost* host;
static uintptr_t base;
static std::filesystem::path registry, receipt;
using Refresh = bool (*)(void*, const Result*);
static Refresh original;
struct ModKey { Text name; int32_t version, padding; };
static_assert(sizeof(ModKey)==40);

static bool Hook(void* rep, const Result* source) {
    int steam = -1;
    auto prefixes = (const Text*)(base+0x4147330);
    for (int i=0;i<2;++i) if (prefixes[i].equals("*")) steam=i;
    std::string token = "startup";
    std::vector<std::unique_ptr<Shadow>> shadows;
    const Result* input = source;
    try {
        std::ifstream in(registry);
        std::string line;
        if (std::getline(in, line) && line.size()==32 && line.find_first_not_of("0123456789abcdef")==std::string::npos) {
            token=line;
            unsigned count=0;
            while (std::getline(in,line)) {
                if (++count>128 || line.size()>4096) throw std::runtime_error("registry limit");
                auto tab=line.find('\t');
                if (tab==std::string::npos) throw std::runtime_error("registry row");
                auto id=line.substr(0,tab);
                if (id.empty() || id.size()>20 || id.find_first_not_of("0123456789")!=std::string::npos)
                    throw std::runtime_error("registry id");
                auto folder=std::filesystem::u8path(line.substr(tab+1));
                std::error_code ec;
                if (!folder.is_absolute() || !std::filesystem::is_regular_file(folder/L"mod.lua",ec)) continue;
                shadows.push_back(std::make_unique<Shadow>(*input,steam,id,folder.wstring()));
                input=&shadows.back()->result;
            }
        }
    } catch (const std::exception& e) {
        host->log("Workshop registry rejected: %s",e.what());
        input=source; token="invalid";
    }
    bool changed = original(rep,input);
    auto first = *(const ModKey**)((char*)rep+0xe0);
    auto last = *(const ModKey**)((char*)rep+0xe8);
    try {
        auto temp=receipt; temp+=L".tmp";
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);
        out<<token<<"\n";
        for (auto p=first;p!=last;++p) {
            out.write(p->name.chars(),p->name.size);
            out<<"\t"<<p->version<<"\n";
        }
        out.close();
        if (out) MoveFileExW(temp.c_str(),receipt.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    } catch (const std::exception& e) { host->log("Workshop receipt failed: %s",e.what()); }
    return changed;
}

extern "C" __declspec(dllexport)
int Tpf2mpPluginInit(const Tpf2mpHost* h,Tpf2mpPluginInfo* out) {
    out->name="workshop_register"; out->version="1";
    out->summary="Register consented multiplayer Workshop downloads";
    if (!h || h->abiMajor!=TPF2MP_ABI_MAJOR || h->size<sizeof(Tpf2mpHost)) return TPF2MP_ERR_ABI;
    host=h;
    registry=std::filesystem::u8path(h->dataDir())/L"mods_registry.txt";
    receipt=std::filesystem::u8path(h->dataDir())/L"mods_catalogue.txt";
    base=h->moduleBase();
    const uint8_t expected[]={0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,
                             0x41,0x57,0x48,0x8d,0xac,0x24,0xd0,0xfe,0xff,0xff};
    if (!base || !h->buildOk() || !h->verifyBytes(0x2373210,expected,sizeof(expected)))
        return TPF2MP_ERR_BUILD;
    return h->installHook(base+0x2373210,(void*)Hook,sizeof(expected),(void**)&original)
        ? TPF2MP_OK : TPF2MP_ERR_FAILED;
}
