#include "../bigmap_linux.cpp"
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <cstdarg>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
static std::map<std::string,int> config;
static std::map<uintptr_t,std::vector<uint8_t>> memory;
static int writes, failWrite, mismatch;
static std::vector<uintptr_t> order;
static uintptr_t mismatchSite;
static bool build=true;
static int Int(const char*,const char* key,int fallback){auto it=config.find(key);return it==config.end()?fallback:it->second;}
// Keep fixture initialization independent of host userfaultfd permissions.
static int compressionRequested=-1;
static int Bool(const char* section,const char* key,int fallback){
    const int value=Int(section,key,fallback);
    if(std::string(key)=="terrain_cache_compress") {compressionRequested=value;return 0;}
    return value;
}
static const char* Str(const char*,const char*,const char* fallback){return fallback;}
static uintptr_t Base(){return 0x40000000;}
static int Build(){return build;}
static void Log(const char*,...){}
static int Verify(uintptr_t rva,const uint8_t* bytes,uint32_t n){
    if(mismatch || rva==mismatchSite)return 0;
    memory[rva]=std::vector<uint8_t>(bytes,bytes+n);return 1;
}
static int PatchBytes(uintptr_t rva,const uint8_t* bytes,uint32_t n){
    if(++writes==failWrite)return 0;
    order.push_back(rva);memory[rva]=std::vector<uint8_t>(bytes,bytes+n);return 1;
}
static uint64_t Stock(int size,int format,void*){assert(size<7 && format<5);return Pack(96,96);}
static int Hook(uintptr_t,void*,int n,void** out){assert(n==18);++writes;*out=reinterpret_cast<void*>(Stock);return 1;}
static const char* Data(){return "/tmp/";}
static Tpf2mpHost host={sizeof(host),1,Log,Int,Bool,Str,Base,Build,Verify,Hook,PatchBytes,Data};
static void Reset(){config.clear();config["newgame_density"]=0;config["save_fast"]=0;config["terrain_minmax_fast"]=0;memory.clear();order.clear();writes=failWrite=mismatch=0;rows=claimCount=patchCount=0;stockRows=7;build=true;mismatchSite=0;}
extern "C" float TestTown(void*,int);
extern "C" uint32_t TestMinMaxBridge(const uint16_t*,const uint16_t*);
extern "C" uint32_t BigmapMinMax(const uint16_t*,const uint16_t*);
extern "C" int TestOctreeLevel(const void* entry,int id);
extern "C" int TestOctreeChild(const void* entry,void* parent,void** children,int octant,int running,int remaining,void*** slot);
extern "C" char TestOctreeLevelReturn[],TestOctreeChildReturn[];

// Windows Depth12Id (bigmap/src/octree_depth12.h), the behaviour to reproduce.
// -1 stands for __fastfail: the Linux stub must stop with SIGILL instead.
static int64_t WindowsId(int incoming,int parentId,int remaining,int existingId,bool exists,uint32_t* next){
    if(parentId<0x09249249)return incoming;
    const int bank=parentId<=0x49249248?0:(parentId>=0x50000000 && parentId<=0x5fffffff?1:-1);
    if(bank<0 || remaining<1)return -1;
    if(exists)return existingId;
    const uint32_t id=++next[bank];
    if(id>=(bank==0?0x60000000u:0x70000000u))return -1;
    return int32_t(id);
}
struct Node { void* parent; int32_t id; uint8_t rest[0xa8-12]; };

static void OctreeStubs(){
    using namespace linux_octree;
    auto* page=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(page!=MAP_FAILED);
    static uint32_t counters[2];
    assert(BuildLevelStub(page,uintptr_t(TestOctreeLevelReturn))==81);
    assert(BuildChildStub(page+256,uintptr_t(TestOctreeChildReturn),counters)==145);
    assert(!std::memcmp(LevelStub+0x1e,LevelBytes,sizeof(LevelBytes)));
    assert(!std::memcmp(ChildStub+0x7c,ChildBytes,5) && !std::memcmp(ChildStub,ChildBytes+5,10));
    assert(mprotect(page,4096,PROT_READ|PROT_EXEC)==0);
    // Decoder: stock levels 0..10 at every boundary, then both compact banks.
    for(int level=0;level<11;++level) {
        int64_t first=0,width=1;for(int i=0;i<level;++i){first+=width;width*=8;}
        for(int64_t id:{first,first+width-1,first+width/2})assert(TestOctreeLevel(page,int(id))==level);
    }
    for(int id:{0x50000000,0x50000001,0x5fffffff})assert(TestOctreeLevel(page,id)==11);
    for(int id:{0x60000000,0x60000001,0x6fffffff})assert(TestOctreeLevel(page,id)==12);
    // Child step: same choices as the Windows allocator, same counter states.
    uint32_t model[2];
    auto run=[&](int running,int parentId,int remaining,int existing,bool exists,int octant) {
        Node parent{},child{};parent.id=parentId;child.id=existing;
        void* children[8]={};if(exists)children[octant]=&child;
        void** slot=nullptr;
        const int incoming=int(uint32_t(running)*8u+1u+uint32_t(octant));
        const uint32_t before[2]={model[0],model[1]};
        const int64_t want=WindowsId(incoming,parentId,remaining,existing,exists,model);
        if(want<0) {
            model[0]=before[0];model[1]=before[1]; // the process died; nothing was published
            const pid_t pid=fork();assert(pid>=0);
            if(!pid){TestOctreeChild(page+256,&parent,children,octant,running,remaining,&slot);_exit(0);}
            int status=0;assert(waitpid(pid,&status,0)==pid);
            assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGILL);
            return;
        }
        const int got=TestOctreeChild(page+256,&parent,children,octant,running,remaining,&slot);
        assert(got==want && slot==&children[octant]);
        assert(counters[0]==model[0] && counters[1]==model[1]);
    };
    counters[0]=model[0]=CounterStart[0];counters[1]=model[1]=CounterStart[1];
    for(int octant=0;octant<8;++octant)for(int p:{0,1,8,0x01249249,0x09249248})run(p,p,3,0,false,octant); // stock
    run(0x09249249,0x09249249,2,0,false,0);            // first level-11 ID
    assert(counters[0]==0x50000000);
    run(0x09249249,0x09249249,2,0x50000000,true,0);    // revisit keeps it
    run(-5,0x49249248,1,0,false,7);                    // overflowed running index ignored
    run(-5,0x50000000,1,0,false,3);                    // level-12 bank
    run(-5,0x5fffffff,1,0x60000000,true,3);
    assert(counters[0]==0x50000001 && counters[1]==0x60000000);
    run(-5,0x60000000,1,0,false,0);                    // below level 12: refused
    run(-5,0x4fffffff,1,0,false,0);                    // outside both banks
    run(-5,0x09249249,0,0,false,0);                    // no remaining depth
    counters[0]=model[0]=0x5fffffff;run(-5,0x09249249,1,0,false,0); // bank 0 exhausted
    counters[1]=model[1]=0x6fffffff;run(-5,0x50000000,1,0,false,0); // bank 1 exhausted
    counters[0]=model[0]=0x5ffffffe;run(-5,0x09249249,1,0,false,1); // last level-11 ID
    assert(counters[0]==0x5fffffff);
    munmap(page,4096);
}
static uintptr_t JumpTarget(uintptr_t rva){
    auto& b=memory[rva];assert(b.size()>=14 && b[0]==0xff && b[1]==0x25);
    uintptr_t at;std::memcpy(&at,b.data()+6,8);return at;
}
int main(){
    std::vector<uint16_t> heights(66049);
    uint32_t rng=1;for(auto& h:heights){rng=rng*1664525+1013904223;h=uint16_t(rng>>16);}
    for(size_t n:{1u,7u,8u,9u,257u,66049u}) {auto lo=*std::min_element(heights.begin(),heights.begin()+n),hi=*std::max_element(heights.begin(),heights.begin()+n);assert(BigmapMinMax(heights.data(),heights.data()+n)==(uint32_t(lo)|(uint32_t(hi)<<16)));assert(TestMinMaxBridge(heights.data(),heights.data()+n)==(uint32_t(lo)|(uint32_t(hi)<<16)));}

    auto* townPage=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(townPage!=MAP_FAILED);townPage[2048]=0xc3;
    auto* townEntry=TownStub(townPage,uintptr_t(townPage+2048));
    assert(mprotect(townPage,4096,PROT_READ|PROT_EXEC)==0);
    for(int i=3;i<10;++i)assert(std::fabs(TestTown(townEntry,i)-(i==3?.5f:float(.3*density::scales[i-4])))<1e-7);
    assert(TestTown(townEntry,10)==1.f && TestTown(townEntry,-1)==1.f);
    munmap(townPage,4096);
    Tpf2mpPluginInfo info{};
    Reset();assert(Tpf2mpPluginInit(&host,&info)==0 && compressionRequested==1);
    Reset();config["terrain_cache_compress"]=0;
    assert(Tpf2mpPluginInit(&host,&info)==0 && compressionRequested==0);
    Reset();config["terrain_cache_compress"]=1;
    assert(Tpf2mpPluginInit(&host,&info)==0 && compressionRequested==1);
    Reset();config["alignment_batch_tiles"]=512;
    assert(Tpf2mpPluginInit(&host,&info)==0 && alignmentBatch==512);
    assert(memory[0x173e443][0]==0xe8);
    for(uintptr_t site:{0x173dae0,0x173e015,0x173df1f,0x173db87,0x173e3ea,0x173e168,0x173e443}) {
        Reset();config["alignment_batch_tiles"]=512;mismatchSite=site;
        assert(Tpf2mpPluginInit(&host,&info)==0 && alignmentBatch==0);
        assert(memory.count(0x173e443)==0);
    }
    Reset();assert(Tpf2mpPluginInit(&host,&info)==0 && alignmentBatch==0);
    assert(memory.count(0x173e443)==0);
    assert(Tpf2mpPluginInit(nullptr,&info)==TPF2MP_ERR_ABI);
    Reset();build=false;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_BUILD && writes==0);
    Reset();config["enabled"]=0;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_DISABLED && writes==0);
    OctreeStubs();
    // df1436c: the menu ceiling must agree with the root actually installed.
    // Native Steam supports all three depths; Windows GOG's downgrade does not
    // apply to this ELF. Exercise explicit lower caps and disabled widening too.
    for(int depth:{11,12,13})for(int enabled:{0,1})for(int requested:{128,256,512,1024,2048}) {
        Reset();config["octree_depth"]=depth;config["octree"]=enabled;config["max_tiles"]=requested;
        assert(Tpf2mpPluginInit(&host,&info)==0);
        assert(cap==std::min(requested,enabled?linux_octree::EdgeTiles(depth):256));
        if(enabled)assert(memory[OctreeSite][9]==depth);
        else assert(!memory.count(OctreeSite) && !memory.count(linux_octree::ChildSite) && !memory.count(linux_octree::LevelSite));
    }
    for(int bad:{0,10,14,99}){Reset();config["octree_depth"]=bad;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_FAILED && writes==0);}
    {
        using namespace linux_octree;
        // Depth 11 publishes exactly the pre-depth-12 bytes; the ID sites are not even read.
        Reset();mismatchSite=ChildSite;assert(Tpf2mpPluginInit(&host,&info)==0 && cap==512);
        assert(!memory.count(ChildSite) && !memory.count(LevelSite));
        assert(memory[OctreeSite][9]==11 && order.front()==OctreeSite);
        for(int depth:{12,13}) {
            Reset();config["octree_depth"]=depth;assert(Tpf2mpPluginInit(&host,&info)==0 && cap==512);
            assert(order[0]==ChildSite && order[1]==LevelSite && order[2]==OctreeSite); // root last
            auto& root=memory[OctreeSite];assert(root.size()==13 && root[9]==depth);
            int32_t rel;std::memcpy(&rel,root.data()+4,4);float half;
            std::memcpy(&half,reinterpret_cast<void*>(Base()+OctreeSite+8+intptr_t(rel)),4);
            assert(half==float(1<<(depth+5)));
            const uintptr_t level=JumpTarget(LevelSite),child=JumpTarget(ChildSite);
            uintptr_t back;std::memcpy(&back,reinterpret_cast<void*>(level+LevelStubBack),8);assert(back==Base()+LevelBack);
            std::memcpy(&back,reinterpret_cast<void*>(child+ChildStubBack),8);assert(back==Base()+ChildBack);
            uintptr_t next;std::memcpy(&next,reinterpret_cast<void*>(child+ChildStubCounters),8);assert(next==uintptr_t(octreeNext));
            assert(!std::memcmp(reinterpret_cast<void*>(level),LevelStub,LevelStubBack));
            assert(!std::memcmp(reinterpret_cast<void*>(child),ChildStub,ChildStubCounters));
            for(size_t i=14;i<memory[ChildSite].size();++i)assert(memory[ChildSite][i]==0x90);
            Reset();config["octree_depth"]=depth;config["max_tiles"]=4096;
            assert(Tpf2mpPluginInit(&host,&info)==0 && cap==EdgeTiles(depth));
            // Any ID-site mismatch refuses the depth with nothing written.
            for(uintptr_t site:{ChildSite,LevelSite,OctreeSite}) {
                Reset();config["octree_depth"]=depth;mismatchSite=site;
                assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_BUILD && writes==0);
            }
            Reset();config["octree_depth"]=depth;build=false;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_BUILD && writes==0);
            // A failed root write rolls the ID sites back too.
            Reset();config["octree_depth"]=depth;failWrite=4;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_FAILED);
            assert(memory[ChildSite]==std::vector<uint8_t>(ChildBytes,ChildBytes+sizeof(ChildBytes)));
            assert(memory[LevelSite]==std::vector<uint8_t>(LevelBytes,LevelBytes+sizeof(LevelBytes)));
            assert(memory[OctreeSite]==std::vector<uint8_t>(OctreeBytes,OctreeBytes+sizeof(OctreeBytes)));
            Reset();config["octree_depth"]=depth;config["octree"]=0;
            assert(Tpf2mpPluginInit(&host,&info)==0 && cap==256 && !memory.count(ChildSite) && !memory.count(OctreeSite));
        }
        Reset();config["max_tiles"]=4096;assert(Tpf2mpPluginInit(&host,&info)==0 && cap==512);
        // Heightmap and placement-distance bounds still bind past 512 tiles.
        Reset();config["octree_depth"]=13;config["max_tiles"]=2048;assert(Tpf2mpPluginInit(&host,&info)==0);
        int bx=2048,by=64;Bound(bx,by);assert(bx==720 && by==64 && Fits(bx,by));
        bx=2048;by=2048;Bound(bx,by);assert(bx==510 && by==512);
        for(int f=0;f<20;++f){uint64_t v=Shape(2048,f);int sx=uint32_t(v),sy=uint32_t(v>>32);assert(sy==sx*(f+1) && Fits(sx,sy) && sy<=2048);}
    }
    Reset();mismatch=1;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_BUILD && writes==0);
    Reset();assert(Tpf2mpPluginInit(&host,&info)==0 && rows==9 && cap==512);
    assert(Size(6,0,nullptr)==Pack(96,96));
    assert(Size(7,0,nullptr)==Pack(128,128));
    assert(Size(15,0,nullptr)==Pack(510,510));
    stockRows=4;assert(Size(4,0,nullptr)==Pack(128,128));
    for(int side:{96,128,224,512})for(int f=0;f<20;++f){
        uint64_t v=Shape(side,f);int x=uint32_t(v),y=uint32_t(v>>32);
        assert(x>=2 && y<=512 && x%2==0 && y%2==0 && y==x*(f+1) && Fits(x,y));
    }
    int x,y;assert(Parse(" 224 x 320 ",x,y) && x==224 && y==320);
    for(const char* bad:{"", "2x", "3x4junk", "-4x6", "999999999999999999999x6"})assert(!Parse(bad,x,y));
    x=INT_MAX;y=INT_MAX;Bound(x,y);assert(x==510 && y==512);
    assert(RasterCell(24576,24576,1)==1);
    assert(RasterCell(57344,57344,1)==2);
    float c=RasterCell(131072,131072,1);assert(c==4 && std::pow(std::floor(131072/c)+1,2)<=cellBudget);
    String text;RatioText(&text,19);assert(text.data==text.local && text.size==4 && !strcmp(text.data,"1:20"));
    claims[0]={7,1,128,256};claimCount=1;assert(Size(4,1,nullptr)==Pack(128,256));
    Reset();config["street_raster"]=0;assert(Tpf2mpPluginInit(&host,&info)==0 && cap==180 && rows==2);
    Reset();config["octree"]=0;assert(Tpf2mpPluginInit(&host,&info)==0 && cap==256 && rows==5);
    Reset();failWrite=4;assert(Tpf2mpPluginInit(&host,&info)==TPF2MP_ERR_FAILED);
    assert(memory[SizeRva]==std::vector<uint8_t>(SizeBytes,SizeBytes+sizeof(SizeBytes)));
    assert(memory[OctreeSite]==std::vector<uint8_t>(OctreeBytes,OctreeBytes+sizeof(OctreeBytes)));
    Reset();config["save_fast"]=1;assert(Tpf2mpPluginInit(&host,&info)==0);
    assert(memory[0xc7b524]==std::vector<uint8_t>({0xb8,1,0,0,0,0x90}));
    uint32_t buffer;std::memcpy(&buffer,memory[0xc7c3a0].data()+1,4);assert(buffer==65536);
    std::memcpy(&buffer,memory[0xc7c3aa].data()+5,4);assert(buffer==65536);
    std::memcpy(&buffer,memory[0xc7b6b1].data()+5,4);assert(buffer==65536);
    puts("PASS: map sizing, ratios, raster overflow, guards, safe caps, rollback and octree depth 12/13 stubs");
}
