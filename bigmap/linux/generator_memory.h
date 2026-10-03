#pragma once
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <memory>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include "generator_memory_lua.inc"
#include "../src/generator_memory_text.h"

namespace linux_generator {
constexpr uintptr_t FopenPlt = 0x6dd160;
constexpr uint8_t FopenBytes[] = {0xff,0x25,0xca,0xa5,0x36,0x05,0x68,0x94,0x01,0,0,0xe9,0xa0,0xe6,0xff,0xff};
// Linux has no Windows commit guarantee in heuristic/always-overcommit mode.
// Only strict mode (2) treats CommitLimit - Committed_AS as a hard ceiling.
inline int budgetPct=50;
inline uint64_t Budget(uint64_t available,uint64_t limit,uint64_t committed,int mode,int pct) {
    if(pct<=0 || available==UINT64_MAX || mode<0)return 0;
    if(mode==2) {
        if(limit==UINT64_MAX || committed==UINT64_MAX)return 0;
        available=std::min(available,limit>committed?limit-committed:0);
    }
    return available/100*uint64_t(std::min(pct,90));
}
inline uint64_t BudgetBytes(const char* meminfo="/proc/meminfo",
                            const char* overcommit="/proc/sys/vm/overcommit_memory") {
    if(budgetPct<=0)return 0;
    FILE* f=std::fopen(overcommit,"r");if(!f)return 0;
    int mode=-1;
    const bool valid=std::fscanf(f,"%d",&mode)==1 && mode>=0 && mode<=2;
    std::fclose(f);if(!valid)return 0;
    f=std::fopen(meminfo,"r");if(!f)return 0;
    uint64_t available=UINT64_MAX,limit=UINT64_MAX,committed=UINT64_MAX;
    char line[256],key[64];unsigned long long kb;
    while(std::fgets(line,sizeof line,f)) {
        if(std::sscanf(line,"%63s %llu kB",key,&kb)!=2 || kb>UINT64_MAX/1024)continue;
        if(!std::strcmp(key,"MemAvailable:"))available=kb*1024;
        else if(!std::strcmp(key,"CommitLimit:"))limit=kb*1024;
        else if(!std::strcmp(key,"Committed_AS:"))committed=kb*1024;
    }
    const bool ok=!std::ferror(f);std::fclose(f);
    return ok?Budget(available,limit,committed,mode,budgetPct):0;
}
inline int Index(const char* path) {
    if (!path) return -1;
    const char* files[] = {"fantasia_map_generator.gen.lua", "fantasia_map_generator_dry.gen.lua", "fantasia_map_generator_tropical.gen.lua"};
    const std::string p(path);
    for (int i=0;i<3;++i) {
        const std::string suffix=std::string("/res/config/terrain_generators/")+files[i];
        if(p.size()>=suffix.size() && p.compare(p.size()-suffix.size(),suffix.size(),suffix)==0)return i;
    }
    return -1;
}
// A deleter type, not decltype(&std::fclose): newer glibc declares fclose with
// attributes a template argument drops (-Wignored-attributes, an error under -Werror).
struct FileCloser { void operator()(FILE* f) const { std::fclose(f); } };
inline FILE* Copy(const char* path) {
    std::unique_ptr<FILE,FileCloser> source(std::fopen(path,"rb"));
    FILE* src=source.get();
    if(!src)return nullptr;
    struct stat original{};
    if(::fstat(::fileno(src),&original))return nullptr;
    std::string text; char buf[8192]; size_t n;
    while((n=std::fread(buf,1,sizeof buf,src))) {
        // Refuse unexpectedly large/non-generator input, keeping fallback bounded.
        if(text.size()+n>4*1024*1024){return nullptr;}
        text.append(buf,n);
    }
    const bool ok=!std::ferror(src);source.reset();
    if(!ok)return nullptr;
    size_t len=0;char* patched=PatchGeneratorText(text.data(),text.size(),&len,BudgetBytes());
    if(!patched)return nullptr;
    // Anonymous, per-open stream: no shared cache name, symlink or stale-copy race.
    FILE* out=std::tmpfile();
    // Match path and descriptor modification-time queries, including nanoseconds.
    // POSIX has no settable creation time; ctime is not a file write timestamp.
    const timespec times[]={original.st_atim,original.st_mtim};
    if(out && (std::fwrite(patched,1,len,out)!=len || std::fflush(out) ||
               ::futimens(::fileno(out),times) || std::fseek(out,0,SEEK_SET))) {
        std::fclose(out);out=nullptr;
    }
    std::free(patched);return out;
}
inline const Tpf2mpHost* host=nullptr;
inline FILE* Open(const char* path,const char* mode) noexcept {
    const int saved=errno;
    try {
        if(mode && mode[0]=='r' && !std::strpbrk(mode,"wa+") && Index(path)>=0) {
            if(FILE* out=Copy(path)) {
                if(host)host->log("generator memory: %s served with buffer reuse above 32 x 32 km",path);
                errno=saved;return out;
            }
            if(host)host->log("generator memory: %s served unchanged (unreadable, anchor or temporary stream failure)",path);
        }
    } catch(...) {} // Never propagate allocation failures through the engine's C ABI.
    errno=saved;
    return std::fopen(path,mode); // Plugin's own libc import, not the patched game PLT.
}
inline bool Install(const Tpf2mpHost* h) {
    if(!h->cfgBool("tpf2_bigmap","generator_memory",1)) {
        h->log("generator memory: off (generator_memory=0)");return false;
    }
    budgetPct=h->cfgInt("tpf2_bigmap","generator_memory_budget_pct",50);
    if(!h->verifyBytes(FopenPlt,FopenBytes,sizeof FopenBytes)) {
        h->log("generator memory: fopen PLT mismatch; OFF");return false;
    }
    uint8_t bytes[16]={0xff,0x25,0,0,0,0};
    auto fn=&Open;std::memcpy(bytes+6,&fn,sizeof fn);bytes[14]=bytes[15]=0x90;
    host=h;
    if(!h->patchBytes(FopenPlt,bytes,sizeof bytes)) {
        h->log("generator memory: fopen patch failed; OFF");return false;
    }
    h->log("generator memory: native fopen route active; Fantasia files stay unchanged on disk");return true;
}
}
