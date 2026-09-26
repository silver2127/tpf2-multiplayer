#include "../density.h"
#include <cassert>
int main(int argc,char** argv){
    std::string stock;
    if(argc>1){assert(density::Read(argv[1],stock));}else{
        stock="local osutil = require \"osutil\"\n";
        stock+="pGetText(\"map-town-density\", \"High\"), pGetText(\"map-town-density\", \"Very high\"), \n";
        const std::string industry="pGetText(\"map-industry-density\", \"Low\"), pGetText(\"map-industry-density\", \"Medium\"), pGetText(\"map-industry-density\", \"High\"), pGetText(\"map-industry-density\", \"Very high\"), ";
        stock+="{ "+industry+"}\n{ pGetText(\"map-industry-density\", \"Disabled\"), "+industry+"}\n";
        stock+="local industryFreq = { .4, .6, .8, 1.0 }\nlocal targetNumberPerAreaIdx = modParams[\"locations.industry.targetMaxNumberPerArea\"]\n";
    }
    std::string patched,why;assert(density::Patch(stock,patched));
    assert(patched.find(", 0.3, 0.18, 0.108, 0.06, 0.0276, 0.0132")!=std::string::npos);
    for(auto label:density::labels){size_t n=0,at=0;while((at=patched.find(label,at))!=std::string::npos){++n;++at;}assert(n==3);}
    std::string ignored;assert(!density::Patch(stock+stock,ignored));assert(!density::Patch(patched,ignored));assert(!density::Patch("changed file",ignored));
    char dir[]="/tmp/bigmap-density-XXXXXX";assert(mkdtemp(dir));std::string path=std::string(dir)+"/base_mod.lua",read;
    assert(density::Write(path,stock));assert(density::Sync(path,true,why));assert(density::Sync(path,true,why));assert(density::Read(path,read)&&read==patched);
    assert(density::Write(path,patched+"-- user edit\n"));assert(!density::Sync(path,false,why));assert(density::Read(path,read)&&read==patched+"-- user edit\n");
    assert(density::Write(path,patched));assert(density::Sync(path,false,why));assert(density::Read(path,read)&&read==stock);
    // Steam replaced the base file: take a fresh backup rather than an old one.
    assert(density::Write(path,stock+"-- game update\n"));assert(density::Sync(path,true,why));assert(density::Sync(path,false,why));assert(density::Read(path,read)&&read==stock+"-- game update\n");
    unlink(path.c_str());unlink((path+".bigmap-linux.bak").c_str());rmdir(dir);
    if(argc>2)assert(density::Write(argv[2],patched));
    puts("PASS: six density levels, unique anchors, idempotence, backup/restore, preserve edits, game updates");
}
