#include "../src/plugin/preview_plugin_linux.cpp"
#include <sys/mman.h>
#include <cstdlib>
#define CHECK(test) do { if(!(test)) { fprintf(stderr,"CHECK failed at %s:%d: %s\n",__FILE__,__LINE__,#test); abort(); } } while(0)
#include <fstream>
#include <iterator>

// Every time query deliberately clobbers XMM0, reproducing an allowed libc
// register clobber between Update's entry and forwarding its float argument.
extern "C" __attribute__((noinline)) int clock_gettime(clockid_t clock,timespec* out) noexcept {
    const long result=syscall(SYS_clock_gettime,clock,out);
    __asm__ volatile("pxor %%xmm0, %%xmm0" ::: "xmm0");
    return static_cast<int>(result);
}

namespace {
std::string testDir;
const char* testDataDir() { return testDir.c_str(); }
void testLog(const char*, ...) {}
void* fakeConvert(void* result,void*,void*) { return result; }
std::vector<int> heightCalls;
void fakeResetHeight(void*,bool) { heightCalls.push_back(-1); }
void fakeUploadHeight(void*,void* grids,bool) { heightCalls.push_back(**reinterpret_cast<int**>(grids)); }
void fakeClear(void* r,bool,bool) {
    auto state=field<unsigned char*>(r,0x198);
    field<void*>(state,0x1838)=field<void*>(state,0x1830);
}
bool endHeightError;
void fakeEndHeight(void* r) { endHeightError=field<bool>(field<void*>(r,0x198),0x1684); }
void* colorTarget;
bool colorError;
unsigned colorCalls;
void fakeErrorColor(void* r,bool error) {
    colorTarget=r; colorError=error; ++colorCalls;
    field<bool>(field<void*>(r,0x198),0x1684)=error;
}
struct TestRenderer {
    alignas(16) unsigned char renderer[0x1b0]{}, state[0x1918]{};
    int grid[6]{};
    TestRenderer(int id,void* terrain) {
        grid[0]=id;
        field<void*>(renderer,0x50)=terrain;
        field<bool>(renderer,0xd0)=true;
        field<bool>(renderer,0xd4)=true;
        field<void*>(renderer,0x198)=state;
        field<void*>(state,0x1830)=grid;
        field<void*>(state,0x1838)=grid+6;
        field<void*>(state,0x1840)=grid+6;
    }
};
void (*foreignThrow)();
void* fakeFactoryRenderer;
unsigned createdData,deletedData,deletedContexts,uploadedProposals,addedRenderers,deletedRenderers;
void* fakeFactory(void*) { return fakeFactoryRenderer; }
void fakeAdd(void*,void*) { ++addedRenderers; }
void fakeDelete(void* r) { beforeDestroyRenderer(r); ++deletedRenderers; }
void fakeDtor(void*) {}
float observedDt;
void* observedUpdateSelf;
void* observedUpdateScene;
void fakeUpdate(void* renderer,void* target,float dt) {
    observedDt=dt; observedUpdateSelf=renderer; observedUpdateScene=target;
}
void fakeDataDtor(void*) { ++deletedData; }
void fakeContextDtor(void*) { ++deletedContexts; }
void checkContext(void* context) {
    CHECK(field<unsigned char>(context,1)==1 && field<unsigned char>(context,9)==1);
    CHECK(field<float>(context,4)==1 && field<float>(context,0x10)==0.75f);
    CHECK(field<int>(context,0x14)==-1);
    CHECK(field<void*>(context,0x18)==static_cast<char*>(context)+0x48);
    CHECK(field<uint64_t>(context,0x20)==1 && field<float>(context,0x38)==1);
    for(size_t off:{size_t(0x28),size_t(0x30),size_t(0x40),size_t(0x48),size_t(0x50),size_t(0x58),size_t(0x60)})
        CHECK(field<uint64_t>(context,off)==0);
}
void* fakeCreateData(void* data,void*,void* cost,void*,void* shapes,void* context) {
    CHECK(!cost && !shapes); checkContext(context); ++createdData; return data;
}
void fakeUploadProposal(void*,void* renderer,void*,const float* offset,const void* map,bool a,bool b,bool c) {
    CHECK(!a && !b && c && offset[0]==0 && offset[1]==0 && offset[2]==0);
    CHECK(static_cast<const EmptyEntityMap*>(map)->empty());
    const auto* words=static_cast<const uintptr_t*>(map);
    CHECK(words[0]==reinterpret_cast<uintptr_t>(map)+0x30 && words[1]==1 && words[2]==0 && words[3]==0);
    ++uploadedProposals;
    auto state=field<unsigned char*>(renderer,0x198);
    field<void*>(state,0x1838)=static_cast<char*>(field<void*>(state,0x1830))+24;
    // Actual AddToRenderer reaches the hooked EndHeightMod; simulate that nested
    // foreign call, including its error propagation to the final ACK.
    endHeight(renderer);
}
void throwFromEndHeight(void*) { foreignThrow(); }
void throwFromClear(void*,bool,bool) { foreignThrow(); }
void* throwFromConvert(void*,void*,void*) { foreignThrow(); return nullptr; }
void* throwFromCreate(void*,void*,void*,void*,void*,void*) { foreignThrow(); return nullptr; }
void callClearForForeignCatch() { clearRenderer(nullptr,false,false); }
void callConvertForForeignCatch() { convert(nullptr,nullptr,nullptr); }
void vectors(void* p,size_t off,void* begin,size_t bytes) {
    field<void*>(p,off)=begin;
    field<void*>(p,off+8)=static_cast<char*>(begin)+bytes;
    field<void*>(p,off+16)=static_cast<char*>(begin)+bytes;
}
struct RoadProposal {
    alignas(16) unsigned char proposal[0x3c0]{},nodes[48]{},edge[120]{};
    RoadProposal() {
        vectors(proposal,0,nodes,sizeof(nodes)); vectors(proposal,0x18,edge,sizeof(edge));
        field<int>(nodes,0x14)=-1; field<int>(nodes+24,0x14)=-2;
        field<float>(nodes+24,0)=20;
        field<int>(edge,0)=-1; field<int>(edge,8)=-1; field<int>(edge,12)=-2;
        field<float>(edge,0x10)=20; field<float>(edge,0x1c)=20;
    }
};
unsigned verifyCalls,installCalls,failVerify,failInstall;
int testBuildOk() { return 1; }
int testEnabled(const char*,const char*,int) { return 1; }
int testDisabled(const char*,const char*,int) { return 0; }
uintptr_t testBase() { return 0x100000000ULL; }
int fakeVerify(uintptr_t,const uint8_t*,uint32_t) { return ++verifyCalls!=failVerify; }
int fakeInstall(uintptr_t,void*,int,void** original) {
    *original=reinterpret_cast<void*>(0x12340000); // Must never imply success.
    return ++installCalls!=failInstall;
}
void testInitialization(Tpf2mpHost& h) {
    Tpf2mpPluginInfo info{};
    h.size=sizeof(h); h.abiMajor=TPF2MP_ABI_MAJOR; h.buildOk=testBuildOk;
    h.moduleBase=testBase; h.cfgBool=testDisabled; h.verifyBytes=fakeVerify; h.installHook=fakeInstall;
    CHECK(Tpf2mpPluginInit(nullptr,&info)==TPF2MP_ERR_ABI);
    CHECK(write("ready","999") && write("request","999 1 a draw") && write("ack","999 1 ok"));
    CHECK(Tpf2mpPluginInit(&h,&info)==TPF2MP_ERR_DISABLED);
    CHECK(readRequest().empty());
    for(const char* name:{"ready","ack"}) {
        std::ifstream file(path(name),std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        CHECK(content=="\nend\n");
    }
    h.cfgBool=testEnabled;
    for(failVerify=1;failVerify<=19;++failVerify) {
        verifyCalls=installCalls=0;
        CHECK(Tpf2mpPluginInit(&h,&info)==TPF2MP_ERR_BUILD && !hooksReady && !installCalls);
    }
    failVerify=0;
    for(failInstall=1;failInstall<=8;++failInstall) {
        verifyCalls=installCalls=0;
        CHECK(Tpf2mpPluginInit(&h,&info)==TPF2MP_ERR_FAILED && !hooksReady);
        CHECK(installCalls==failInstall);
        observeRenderThread(scene); CHECK(!renderThreadVerified);
    }
    failInstall=0; verifyCalls=installCalls=0;
    CHECK(Tpf2mpPluginInit(&h,&info)==TPF2MP_OK && hooksReady && installCalls==8);
    CHECK(verifyCalls==19 && !strcmp(info.name,"previews"));
    CHECK(previewVtable[-2]==nullptr && reinterpret_cast<uintptr_t>(previewVtable[-1])==base+0x5a20748);
    CHECK(reinterpret_cast<uintptr_t>(previewVtable[0])==base+0x13948d0);
    CHECK(reinterpret_cast<uintptr_t>(previewVtable[1])==base+0x1394d00);
    base=0;
}

void request(const std::string& body) {
    std::ofstream f(path("request"),std::ios::binary);
    f << body;
}
std::string ack() {
    std::ifstream f(path("ack"),std::ios::binary);
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
}
int main(int argc,char** argv) {
    // Load a separate dynamic C++ runtime, as in the game. This test itself
    // should use the plugin's hidden static runtime.
    void* foreignLibrary=dlopen(argc>1 ? argv[1] : "libstdc++.so.6",RTLD_NOW|RTLD_GLOBAL);
    CHECK(foreignLibrary && resolveGameRuntime());
    foreignThrow=reinterpret_cast<void(*)()>(dlsym(foreignLibrary,"SliceTestForeignThrow"));
    // Construction-only previews must pass without street pieces; malformed
    // transforms, paths and any removal must fail before native evaluation.
    alignas(16) unsigned char cp[0x3c0]{}, ce[0x8f0]{};
    new(ce) std::string("station/rail/modular_station/modular_station.con");
    field<void*>(cp,0x2a0)=ce; field<void*>(cp,0x2a8)=ce+sizeof(ce); field<void*>(cp,0x2b0)=ce+sizeof(ce);
    for(size_t i : {size_t(0),size_t(5),size_t(10),size_t(15)}) field<float>(ce,0x738+i*4)=1;
    CHECK(safeProposal(cp));
    field<void*>(cp,0x290)=ce;
    CHECK(!safeProposal(cp)); field<void*>(cp,0x290)=nullptr;
    field<float>(ce,0x738)=0;
    CHECK(!safeProposal(cp)); field<float>(ce,0x738)=1;
    field<float>(ce,0x738+12*4)=INFINITY;
    CHECK(!safeProposal(cp)); field<float>(ce,0x738+12*4)=0;
    field<std::string>(ce,0)="../bad.con";
    CHECK(!safeProposal(cp));
    field<std::string>(ce,0).~basic_string();
    char temp[]="/tmp/tpf2-preview-test-XXXXXX";
    CHECK(mkdtemp(temp));
    testDir=std::string(temp)+"/";
    Tpf2mpHost testHost{};
    testHost.dataDir=testDataDir; testHost.log=testLog;
    host=&testHost;
    testInitialization(testHost);
    hooksReady=true; renderThreadVerified=true;
    setErrorColor=fakeErrorColor;
    TestRenderer colorPeer(1,nullptr), otherPeer(2,nullptr);
    Peer palettePeer{}; palettePeer.renderer=colorPeer.renderer;
    for(unsigned i=0;i<sizeof(palettePeer.originalPalette);++i) palettePeer.originalPalette[i]=static_cast<unsigned char>(i);
    for(int status : {0,1,0,-1}) {
        applySenderPalette(palettePeer,status);
        for(unsigned i=0;i<0x80;++i) {
            const unsigned expected=status==0 ? i%0x40 : (status==1 ? 0x40+i%0x40 : i);
            CHECK(colorPeer.renderer[0xf8+i]==expected);
        }
        CHECK(otherPeer.renderer[0xf8]==0);
        CHECK(colorPeer.renderer[0xf7]==0 && colorPeer.renderer[0x178]==0);
    }
    applySenderColor(colorPeer.renderer,"drawbad");
    errorColor(colorPeer.renderer,false);
    CHECK(colorCalls==1 && colorTarget==colorPeer.renderer && colorError);
    applySenderColor(otherPeer.renderer,"drawok");
    errorColor(otherPeer.renderer,true);
    CHECK(colorCalls==2 && colorTarget==otherPeer.renderer && !colorError);
    applySenderColor(colorPeer.renderer,"draw");
    applySenderColor(colorPeer.renderer,"keep");
    CHECK(colorCalls==2); // Unknown status/keepalive must not overwrite colour.
    errorColor(colorPeer.renderer,true);
    CHECK(colorCalls==3 && colorError); // Unknown status preserves evaluation.
    applySenderColor(colorPeer.renderer,"drawbad");
    errorColor(otherPeer.renderer,false);
    CHECK(colorCalls==4 && !colorError && colorTarget==otherPeer.renderer);
    senderColorRenderer=nullptr; senderErrorColor=-1;
    errorColor(colorPeer.renderer,false);
    CHECK(colorCalls==5 && !colorError); // Override ends with upload scope.
    originalEndHeight=fakeEndHeight;
    peers[0].renderer=colorPeer.renderer;
    for(bool invalid : {false,true}) {
        applySenderColor(colorPeer.renderer,invalid ? "drawbad" : "drawok");
        // Reproduce AddHeightMod's direct write after initial colour selection.
        field<bool>(colorPeer.state,0x1684)=!invalid;
        endHeight(colorPeer.renderer);
        CHECK(endHeightError==invalid);
        CHECK(field<bool>(colorPeer.state,0x1684)==invalid);
    }
    peers[0].renderer=nullptr;
    senderColorRenderer=nullptr; senderErrorColor=-1;
    request("42 1 a keep\r\nend\r\n");
    CHECK(readRequest()=="42 1 a keep");
    request("42 1 a keep\nend\n");
    CHECK(readRequest()=="42 1 a keep");
    request("42 1 a keep\r\nend");
    CHECK(readRequest().empty());
    request(std::string(192,'x')+"\nend\n");
    CHECK(readRequest().empty());

    request(std::string("42 1 a keep\0ignored\nend\n",25));
    CHECK(readRequest().empty());
    RoadProposal road;
    CHECK(safeProposal(road.proposal));
    field<int>(road.edge,0x48)=1; CHECK(safeProposal(road.proposal));
    field<int>(road.edge,8)=700; CHECK(!safeProposal(road.proposal)); field<int>(road.edge,8)=-1;
    field<float>(road.edge,0x10)=0; CHECK(!safeProposal(road.proposal)); field<float>(road.edge,0x10)=20;
    field<int>(road.nodes+24,0x14)=-1; CHECK(!safeProposal(road.proposal)); field<int>(road.nodes+24,0x14)=-2;
    field<void*>(road.proposal,0x28)=road.edge+1; CHECK(!safeProposal(road.proposal)); field<void*>(road.proposal,0x28)=road.edge+120;
    void* inaccessible=mmap(nullptr,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(inaccessible!=MAP_FAILED && !safeProposal(inaccessible));
    vectors(road.proposal,0,inaccessible,48); CHECK(!safeProposal(road.proposal));
    vectors(road.proposal,0,road.nodes,sizeof(road.nodes)); CHECK(munmap(inaccessible,4096)==0);

    // Exercise the real conversion hook's protocol; no renderer/native RVAs used.
    originalConvert=fakeConvert; scene=&testHost;
    guiThread=threadId(); session=42;
    snprintf(peers[0].origin,sizeof(peers[0].origin),"a");
    alignas(16) unsigned char proposal[0x3c0]{};
    request("42 2 a keep\r\nend\r\n");
    CHECK(convert(proposal,nullptr,nullptr)==proposal);
    CHECK(ack()=="42 2 error\nend\n"); // Missing buffers must trigger redraw.
    peers[0].seen=nowMs();
    request("42 3 a keep\r\nend\r\n");
    convert(proposal,nullptr,nullptr);
    CHECK(ack()=="42 3 ok\nend\n");
    peers[0].seen=nowMs()-5000;
    request("42 4 a keep\r\nend\r\n");
    convert(proposal,nullptr,nullptr);
    CHECK(ack()=="42 4 error\nend\n"); // Expired previews cannot ACK empty buffers.
    request("41 5 a clear\r\nend\r\n");
    convert(proposal,nullptr,nullptr);
    CHECK(ack()=="42 4 error\nend\n"); // Previous scene cannot claim current ACK.
    request("42 6 a clear\r\nend\r\n");
    convert(proposal,nullptr,nullptr);
    CHECK(ack()=="42 6 ok\nend\n" && readRequest().empty());

    request("42 6 a clear trailing\nend\n"); convert(proposal,nullptr,nullptr);
    CHECK(ack()=="42 6 ok\nend\n");
    request("42 7 a unknown\nend\n"); convert(proposal,nullptr,nullptr);
    CHECK(ack()=="42 7 error\nend\n");
    renderThreadVerified=false;
    request("42 8 a clear\nend\n"); convert(proposal,nullptr,nullptr);
    CHECK(ack()=="42 7 error\nend\n");
    observeRenderThread(scene); CHECK(renderThreadVerified);
    pthread_t wrongThread;
    CHECK(pthread_create(&wrongThread,nullptr,[](void* target)->void* { observeRenderThread(target); return nullptr; },scene)==0);
    CHECK(pthread_join(wrongThread,nullptr)==0 && threadMismatch);
    convert(proposal,nullptr,nullptr); CHECK(ack()=="42 7 error\nend\n");
    threadMismatch=false;

    // The terrain buffer is shared. A local tool reset must retain every remote
    // preview; remote cancel/expiry must retain the local tool, in that order.
    originalClear=fakeClear; originalEndHeight=fakeEndHeight;
    resetHeight=fakeResetHeight; uploadHeight=fakeUploadHeight;
    terrainTarget=&testHost;
    TestRenderer remote1(1,terrainTarget),remote2(2,terrainTarget),local(3,terrainTarget),otherWorld(4,&temp);
    peers[0].renderer=remote1.renderer; peers[0].seen=nowMs();
    peers[1].renderer=remote2.renderer; peers[1].seen=nowMs();
    endHeight(local.renderer); endHeight(local.renderer); // register only once
    endHeight(otherWorld.renderer);
    composeTerrain();
    CHECK((heightCalls==std::vector<int>{-1,1,2,3}));
    heightCalls.clear();
    clearRenderer(local.renderer,true,true);
    CHECK((heightCalls==std::vector<int>{-1,1,2}));
    TestRenderer localAgain(5,terrainTarget);
    endHeight(localAgain.renderer);
    heightCalls.clear();
    clear(peers[0]); composeTerrain();
    CHECK((heightCalls==std::vector<int>{-1,2,5}));
    heightCalls.clear();
    peers[1].seen=nowMs()-5000;
    CHECK(expirePeers()); composeTerrain();
    CHECK((heightCalls==std::vector<int>{-1,5}));
    CHECK(!expirePeers());
    endHeight(remote1.renderer);
    CHECK(field<bool>(remote1.renderer,0xd0)); // remote upload suppression is temporary
    localHeightRenderers.clear(); terrainTarget=nullptr;
    peers[0]=Peer{}; peers[1]=Peer{};
    // Full road/rail draw, sender palette, nested terrain completion and
    // destruction are exercised through the actual request/convert path.
    TestRenderer drawPeer(8,&testHost);
    fakeFactoryRenderer=drawPeer.renderer;
    originalFactory=fakeFactory; originalAdd=fakeAdd; deleteRenderer=fakeDelete;
    originalRendererDtor=fakeDtor; removeRenderable=fakeAdd;
    createData=fakeCreateData; uploadProposal=fakeUploadProposal;
    destroyData=fakeDataDtor; destroyContext=fakeContextDtor;
    for(unsigned i=0;i<0x80;++i) drawPeer.renderer[0xf8+i]=static_cast<unsigned char>(i);
    request("42 10 a drawbad\nend\n"); convert(road.proposal,nullptr,nullptr);
    CHECK(ack()=="42 10 ok\nend\n" && createdData==1 && uploadedProposals==1);
    CHECK(deletedData==1 && deletedContexts==1 && peers[0].seen);
    CHECK(field<void**>(drawPeer.renderer,0)==previewVtable && addedRenderers==1);
    CHECK(field<bool>(drawPeer.state,0x1684));
    rendererUpdate=fakeUpdate;
    const float frameDt=0.3125f;
    updateRenderer(drawPeer.renderer,scene,frameDt);
    CHECK(observedDt==frameDt && observedUpdateSelf==drawPeer.renderer && observedUpdateScene==scene);
    CHECK(previewVtable[2]==reinterpret_cast<void*>(updateRenderer));
    for(unsigned i=0;i<0x80;++i) CHECK(drawPeer.renderer[0xf8+i]==0x40+i%0x40);
    request("42 11 a drawok\nend\n"); convert(road.proposal,nullptr,nullptr);
    CHECK(ack()=="42 11 ok\nend\n" && addedRenderers==1 && !field<bool>(drawPeer.state,0x1684));
    for(unsigned i=0;i<0x80;++i) CHECK(drawPeer.renderer[0xf8+i]==i%0x40);

    if(foreignThrow) {
        auto foreignCatch=reinterpret_cast<bool(*)(void(*)())>(dlsym(foreignLibrary,"SliceTestForeignCatch"));
        CHECK(foreignCatch);
        // Foreign exceptions thrown by original gameplay functions must retain
        // their game caller's catch path; detours have no static-runtime cleanup.
        originalClear=throwFromClear; CHECK(foreignCatch(callClearForForeignCatch));
        originalConvert=throwFromConvert; CHECK(foreignCatch(callConvertForForeignCatch));
        originalClear=fakeClear; originalConvert=fakeConvert;
        // A nested mesh failure must not escape through the plugin mutex/string
        // cleanups, nor be silently converted to a successful draw ACK.
        originalEndHeight=throwFromEndHeight;
        request("42 12 a drawok\nend\n"); convert(road.proposal,nullptr,nullptr);
        CHECK(ack()=="42 12 error\nend\n" && serviceFailed && !peers[0].seen);
        CHECK(deletedData==3 && deletedContexts==3 && !editingRemote);
        CHECK(pthread_mutex_trylock(&previewMutex)==0); pthread_mutex_unlock(&previewMutex);
        serviceFailed=false; originalEndHeight=fakeEndHeight;
        createData=throwFromCreate;
        request("42 13 a drawok\nend\n"); convert(road.proposal,nullptr,nullptr);
        CHECK(ack()=="42 13 error\nend\n" && serviceFailed && !peers[0].seen);
        CHECK(deletedData==3 && deletedContexts==4);
        createData=fakeCreateData; serviceFailed=false;
    }
    // Unexpected native destruction invalidates the borrowed peer pointer. The
    // same origin can subsequently allocate a fresh cosmetic renderer safely.
    destroyRenderer(drawPeer.renderer); CHECK(!peers[0].renderer && !peers[0].seen);
    request("42 14 a draw\nend\n"); convert(road.proposal,nullptr,nullptr);
    CHECK(ack()=="42 14 ok\nend\n" && addedRenderers==2);
    dispose(); CHECK(!scene && !terrainTarget && !renderThreadVerified && deletedRenderers==1);
    unlink(path("request").c_str()); unlink(path("ack").c_str()); unlink(path("ready").c_str());
    CHECK(rmdir(temp)==0);
    puts("Linux native preview proposal/file/ACK/expiry and shared terrain lifecycle contracts passed");
}
