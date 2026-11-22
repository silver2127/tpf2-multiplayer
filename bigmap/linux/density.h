#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace density {
constexpr double scales[]={.50,.30,.18,.10,.046,.022};
constexpr const char* labels[]={"Reduced (x0.50)","Sparse (x0.30)","Megalomaniac count at 56 km (x0.18)","Minimal (x0.10)","Megalomaniac count at 112 km (x0.046)","Megalomaniac count at 160 km (x0.022)"};
constexpr const char* marker="-- tpf2_bigmap Linux density v1\n";
inline bool Read(const std::string& path,std::string& out) {
    std::ifstream f(path,std::ios::binary);if(!f)return false;
    out.assign(std::istreambuf_iterator<char>(f),{});return !f.bad() && out.size()<16*1024*1024;
}
inline bool Write(const std::string& path,const std::string& text) {
    std::string tmp=path+".XXXXXX";std::vector<char> name(tmp.begin(),tmp.end());name.push_back(0);
    int fd=mkstemp(name.data());if(fd<0)return false;
    struct stat st{}; mode_t mode=stat(path.c_str(),&st)==0 ? st.st_mode&0777 : 0644;
    bool ok=fchmod(fd,mode)==0;size_t done=0;
    while(ok && done<text.size()){ssize_t n=write(fd,text.data()+done,text.size()-done);if(n<=0)ok=false;else done+=size_t(n);}
    if(ok)ok=fsync(fd)==0;
    if(close(fd))ok=false;
    if(ok)ok=rename(name.data(),path.c_str())==0;
    if(!ok)unlink(name.data());
    return ok;
}
inline bool Patch(const std::string& stock,std::string& out) {
    if(stock.find("tpf2_bigmap")!=std::string::npos)return false;
    std::string names,freq;
    for(int i=0;i<6;++i){names+="_(\""+std::string(labels[i])+"\"), ";char n[32];snprintf(n,sizeof(n),", %.6g",.6*scales[i]);freq+=n;}
    const std::string town="pGetText(\"map-town-density\", \"High\"), pGetText(\"map-town-density\", \"Very high\"), ";
    const std::string industry="{ pGetText(\"map-industry-density\", \"Low\"), pGetText(\"map-industry-density\", \"Medium\"), pGetText(\"map-industry-density\", \"High\"), pGetText(\"map-industry-density\", \"Very high\"), ";
    const std::string target="{ pGetText(\"map-industry-density\", \"Disabled\"), "+industry.substr(2);
    const std::pair<std::string,std::string> steps[]={
      {"local osutil = require \"osutil\"\n",std::string("\n")+marker},
      {town,names},{industry,names},{target,names},
      {"local industryFreq = { .4, .6, .8, 1.0",freq},
      {"local targetNumberPerAreaIdx = modParams[\"locations.industry.targetMaxNumberPerArea\"]\n",
       "\t\tprint(string.format(\"[tpf2_bigmap] industry multipliers: start=%g target=%g\", industryFreq[startIndustriesIdx + 1] or -1, industryFreq[targetNumberPerAreaIdx] or 0))\n"}};
    size_t previous=0;out.clear();
    for(auto& step:steps){auto at=stock.find(step.first);if(at==std::string::npos || stock.find(step.first,at+1)!=std::string::npos || at<previous)return false;
      at+=step.first.size();out+=stock.substr(previous,at-previous);out+=step.second;previous=at;}
    out+=stock.substr(previous);return true;
}
// Restore only an exact patch of the saved backup: never discard later edits.
inline bool Sync(const std::string& path,bool want,std::string& why) {
    std::string cur,stock,patched;if(!Read(path,cur)){why="cannot read base_mod.lua";return false;}
    bool ours=cur.find(marker)!=std::string::npos;
    if(ours){if(!Read(path+".bigmap-linux.bak",stock) || !Patch(stock,patched) || cur!=patched){why="modified patch or missing backup; file left untouched";return false;}}
    else stock=cur;
    if(!want){if(!ours)return true;return Write(path,stock);}
    if(!Patch(stock,patched)){why="base_mod.lua anchors changed or a foreign patch is present";return false;}
    if(cur==patched)return true;
    if(!Write(path+".bigmap-linux.bak",stock) || !Write(path,patched)){why="cannot write density patch/backup";return false;}
    return true;
}
inline std::string GamePath() {
    char path[4096];ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);if(n<=0)return {};
    path[n]=0;std::string s(path);return s.substr(0,s.rfind('/'))+"/res/config/base_mod.lua";
}
}
