// Windows/MSVC contract test. No game process or hooks required.
// cl /nologo /std:c++17 /EHsc /MT tools\preview_native_test.cpp /Fe:preview_native_test.exe
#include "../native/src/preview_plugin.cpp"
#include <cassert>
#include <fstream>
#include <iterator>

namespace {
std::string testDir;
const char* testDataDir() { return testDir.c_str(); }
void testLog(const char*, ...) {}
void* fakeConvert(void* result,void*,void*) { return result; }
std::vector<int> heightCalls;
void fakeResetHeight(void*,bool) { heightCalls.push_back(-1); }
void fakeUploadHeight(void*,void* grids,bool) { heightCalls.push_back(**reinterpret_cast<int**>(grids)); }
void fakeClear(void* r,bool,bool) {
    auto state=field<unsigned char*>(r,0x1b8);
    field<void*>(state,0x16b8)=field<void*>(state,0x16b0);
}
bool endHeightError;
void fakeEndHeight(void* r) { endHeightError=field<bool>(field<void*>(r,0x1b8),0x1504); }
void* colorTarget;
bool colorError;
unsigned colorCalls;
void fakeErrorColor(void* r,bool error) {
    colorTarget=r; colorError=error; ++colorCalls;
    field<bool>(field<void*>(r,0x1b8),0x1504)=error;
}
struct TestRenderer {
    alignas(16) unsigned char renderer[0x1d0]{}, state[0x1750]{};
    int grid[6]{};
    TestRenderer(int id,void* terrain) {
        grid[0]=id;
        field<void*>(renderer,0x50)=terrain;
        field<bool>(renderer,0xf0)=true;
        field<bool>(renderer,0xf4)=true;
        field<void*>(renderer,0x1b8)=state;
        field<void*>(state,0x16b0)=grid;
        field<void*>(state,0x16b8)=grid+6;
    }
};
void request(const std::string& body) {
    std::ofstream f(path("request"),std::ios::binary);
    f << body;
}
std::string ack() {
    std::ifstream f(path("ack"),std::ios::binary);
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
}
int main() {
    // Construction-only previews must pass without street pieces; malformed
    // transforms, paths and any removal must fail before native evaluation.
    alignas(16) unsigned char cp[0x2f8]{}, ce[0x8e0]{};
    new(ce) std::string("station/rail/modular_station/modular_station.con");
    field<void*>(cp,0x1f8)=ce; field<void*>(cp,0x200)=ce+sizeof(ce);
    for(size_t i : {size_t(0),size_t(5),size_t(10),size_t(15)}) field<float>(ce,0x728+i*4)=1;
    assert(safeProposal(cp));
    field<void*>(cp,0x1e8)=ce;
    assert(!safeProposal(cp)); field<void*>(cp,0x1e8)=nullptr;
    field<float>(ce,0x728)=0;
    assert(!safeProposal(cp)); field<float>(ce,0x728)=1;
    field<float>(ce,0x728+12*4)=INFINITY;
    assert(!safeProposal(cp)); field<float>(ce,0x728+12*4)=0;
    field<std::string>(ce,0)="../bad.con";
    assert(!safeProposal(cp));
    field<std::string>(ce,0).~basic_string();
    char temp[MAX_PATH]{};
    assert(GetTempPathA(MAX_PATH,temp));
    testDir=std::string(temp)+"tpf2-preview-test-"+std::to_string(GetCurrentProcessId())+"\\";
    assert(CreateDirectoryA(testDir.c_str(),nullptr));
    Tpf2mpHost testHost{};
    testHost.dataDir=testDataDir; testHost.log=testLog;
    host=&testHost;
    setErrorColor=fakeErrorColor;
    TestRenderer colorPeer(1,nullptr), otherPeer(2,nullptr);
    Peer palettePeer{}; palettePeer.renderer=colorPeer.renderer;
    for(unsigned i=0;i<sizeof(palettePeer.originalPalette);++i) palettePeer.originalPalette[i]=static_cast<unsigned char>(i);
    for(int status : {0,1,0,-1}) {
        applySenderPalette(palettePeer,status);
        for(unsigned i=0;i<0x80;++i) {
            const unsigned expected=status==0 ? i%0x40 : (status==1 ? 0x40+i%0x40 : i);
            assert(colorPeer.renderer[0x118+i]==expected);
        }
        assert(otherPeer.renderer[0x118]==0);
        assert(colorPeer.renderer[0x117]==0 && colorPeer.renderer[0x198]==0);
    }
    applySenderColor(colorPeer.renderer,"drawbad");
    errorColor(colorPeer.renderer,false);
    assert(colorCalls==1 && colorTarget==colorPeer.renderer && colorError);
    applySenderColor(otherPeer.renderer,"drawok");
    errorColor(otherPeer.renderer,true);
    assert(colorCalls==2 && colorTarget==otherPeer.renderer && !colorError);
    applySenderColor(colorPeer.renderer,"draw");
    applySenderColor(colorPeer.renderer,"keep");
    assert(colorCalls==2); // Unknown status/keepalive must not overwrite colour.
    errorColor(colorPeer.renderer,true);
    assert(colorCalls==3 && colorError); // Unknown status preserves evaluation.
    applySenderColor(colorPeer.renderer,"drawbad");
    errorColor(otherPeer.renderer,false);
    assert(colorCalls==4 && !colorError && colorTarget==otherPeer.renderer);
    senderColorRenderer=nullptr; senderErrorColor=-1;
    errorColor(colorPeer.renderer,false);
    assert(colorCalls==5 && !colorError); // Override ends with upload scope.
    originalEndHeight=fakeEndHeight;
    peers[0].renderer=colorPeer.renderer;
    for(bool invalid : {false,true}) {
        applySenderColor(colorPeer.renderer,invalid ? "drawbad" : "drawok");
        // Reproduce AddHeightMod's direct write after initial colour selection.
        field<bool>(colorPeer.state,0x1504)=!invalid;
        endHeight(colorPeer.renderer);
        assert(endHeightError==invalid);
        assert(field<bool>(colorPeer.state,0x1504)==invalid);
    }
    peers[0].renderer=nullptr;
    senderColorRenderer=nullptr; senderErrorColor=-1;
    request("42 1 a keep\r\nend\r\n");
    assert(readRequest()=="42 1 a keep");
    request("42 1 a keep\nend\n");
    assert(readRequest()=="42 1 a keep");
    request("42 1 a keep\r\nend");
    assert(readRequest().empty());
    request(std::string(192,'x')+"\nend\n");
    assert(readRequest().empty());

    // Exercise the real conversion hook's protocol; no renderer/native RVAs used.
    originalConvert=fakeConvert; scene=&testHost;
    guiThread=GetCurrentThreadId(); session=42;
    strcpy_s(peers[0].origin,"a");
    alignas(16) unsigned char proposal[0x2f8]{};
    request("42 2 a keep\r\nend\r\n");
    assert(convert(proposal,nullptr,nullptr)==proposal);
    assert(ack()=="42 2 error\nend\n"); // Missing buffers must trigger redraw.
    peers[0].seen=GetTickCount64();
    request("42 3 a keep\r\nend\r\n");
    convert(proposal,nullptr,nullptr);
    assert(ack()=="42 3 ok\nend\n");
    peers[0].seen=GetTickCount64()-5000;
    request("42 4 a keep\r\nend\r\n");
    convert(proposal,nullptr,nullptr);
    assert(ack()=="42 4 error\nend\n"); // Expired previews cannot ACK empty buffers.
    request("41 5 a clear\r\nend\r\n");
    convert(proposal,nullptr,nullptr);
    assert(ack()=="42 4 error\nend\n"); // Previous scene cannot claim current ACK.
    request("42 6 a clear\r\nend\r\n");
    convert(proposal,nullptr,nullptr);
    assert(ack()=="42 6 ok\nend\n" && readRequest().empty());

    // The terrain buffer is shared. A local tool reset must retain every remote
    // preview; remote cancel/expiry must retain the local tool, in that order.
    originalClear=fakeClear; originalEndHeight=fakeEndHeight;
    resetHeight=fakeResetHeight; uploadHeight=fakeUploadHeight;
    terrainTarget=&testHost;
    TestRenderer remote1(1,terrainTarget),remote2(2,terrainTarget),local(3,terrainTarget),otherWorld(4,&temp);
    peers[0].renderer=remote1.renderer; peers[0].seen=GetTickCount64();
    peers[1].renderer=remote2.renderer; peers[1].seen=GetTickCount64();
    endHeight(local.renderer); endHeight(local.renderer); // register only once
    endHeight(otherWorld.renderer);
    composeTerrain();
    assert((heightCalls==std::vector<int>{-1,1,2,3}));
    heightCalls.clear();
    clearRenderer(local.renderer,true,true);
    assert((heightCalls==std::vector<int>{-1,1,2}));
    TestRenderer localAgain(5,terrainTarget);
    endHeight(localAgain.renderer);
    heightCalls.clear();
    clear(peers[0]); composeTerrain();
    assert((heightCalls==std::vector<int>{-1,2,5}));
    heightCalls.clear();
    peers[1].seen=GetTickCount64()-5000;
    assert(expirePeers()); composeTerrain();
    assert((heightCalls==std::vector<int>{-1,5}));
    assert(!expirePeers());
    endHeight(remote1.renderer);
    assert(field<bool>(remote1.renderer,0xf0)); // remote upload suppression is temporary
    localHeightRenderers.clear(); terrainTarget=nullptr;
    peers[0]=Peer{}; peers[1]=Peer{};
    DeleteFileA(path("request").c_str()); DeleteFileA(path("ack").c_str());
    RemoveDirectoryA(testDir.c_str());
    puts("Native preview file/ACK/expiry and shared terrain lifecycle contracts passed");
}
