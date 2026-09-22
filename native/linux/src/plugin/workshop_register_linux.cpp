#include "plugin/tpf2mp_plugin.h"
#include "workshop_catalogue_linux.h"
#include "preview_game_guard.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstdio>

using namespace workshop;
namespace {
const Tpf2mpHost* host;
uintptr_t base;
std::string registry,receipt;
using Refresh=bool(*)(void*,const Result*);
Refresh original;

bool Hook(void* rep,const Result* source) {
    std::string token="startup";
    std::unique_ptr<Shadow> shadow;
    try {
        std::ifstream input(registry);std::string line;
        if(std::getline(input,line)) {
            if(line.size()!=32 || line.find_first_not_of("0123456789abcdef")!=std::string::npos)
                throw std::runtime_error("registry token");
            token=line;
            std::vector<std::pair<std::string,std::string>> rows;
            while(std::getline(input,line)) {
                const auto tab=line.find('\t');
                if(tab==std::string::npos)throw std::runtime_error("registry row");
                auto id=line.substr(0,tab),folder=line.substr(tab+1);
                if(id.empty() || id.size()>20 || id.find_first_not_of("0123456789")!=std::string::npos)
                    throw std::runtime_error("registry id");
                std::error_code ec;
                const auto path=std::filesystem::u8path(folder);
                if(path.is_absolute() && std::filesystem::is_regular_file(path/"mod.lua",ec))rows.emplace_back(id,folder);
            }
            // 31ae9b7 selects this table, then key*32 and the first prefix byte.
            const auto* prefixes=reinterpret_cast<const Text*>(base+0x5b36d40);
            int steam=-1;for(int i=0;i<2;++i)if(prefixes[i].equals("*"))steam=i;
            shadow=std::make_unique<Shadow>(*source,steam,rows);
        }
    } catch(const std::exception& e) {
        host->log("Workshop registry rejected: %s",e.what());shadow.reset();token="invalid";
    }
    bool changed=false;
    // The game copies the borrowed strings; only trivial frames run between
    // the guard and its original function, so foreign EH never sees our cleanup.
    if(!callGameResult(&changed,original,rep,shadow?&shadow->result:source)) {
        host->log("Workshop catalogue refresh threw");return false;
    }
    try {
        // 31af818/31af85e publish the vector at rep+d8/e0; 31af1c4 stride=40.
        const auto first=*reinterpret_cast<const ModKey**>(static_cast<char*>(rep)+0xd8);
        const auto last=*reinterpret_cast<const ModKey**>(static_cast<char*>(rep)+0xe0);
        if(uintptr_t(last)<uintptr_t(first) || (uintptr_t(last)-uintptr_t(first))%sizeof(ModKey))
            throw std::runtime_error("catalogue span");
        const auto tmp=receipt+".tmp";std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
        out<<token<<'\n';
        for(auto p=first;p!=last;++p){out.write(p->name.pointer,p->name.size);out<<'\t'<<p->version<<'\n';}
        out.close();if(out)std::rename(tmp.c_str(),receipt.c_str());
    } catch(const std::exception& e){host->log("Workshop receipt failed: %s",e.what());}
    return changed;
}
}
extern "C" __attribute__((visibility("default")))
int Tpf2mpPluginInit(const Tpf2mpHost* h,Tpf2mpPluginInfo* out) {
    out->name="workshop_register";out->version="1";out->summary="Register consented multiplayer Workshop downloads";
    if(!h || h->abiMajor!=TPF2MP_ABI_MAJOR || h->size<sizeof(Tpf2mpHost))return TPF2MP_ERR_ABI;
    host=h;base=h->moduleBase();registry=std::string(h->dataDir())+"mods_registry.txt";receipt=std::string(h->dataDir())+"mods_catalogue.txt";
    const uint8_t expected[]={0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x57,0x49,0x89,0xf7,0x41,0x56};
    if(!base || !h->buildOk() || !h->verifyBytes(0x31ae800,expected,sizeof(expected)) || !resolveGameRuntime())return TPF2MP_ERR_BUILD;
    return h->installHook(base+0x31ae800,reinterpret_cast<void*>(Hook),sizeof(expected),reinterpret_cast<void**>(&original)) ? TPF2MP_OK:TPF2MP_ERR_FAILED;
}
