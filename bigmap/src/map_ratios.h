// Steam 35924. Replace the closed five-case text formatter and extend the
// original combo loop. GetNumTilesNew must already be detoured before enabling
// any extra rows; its stock ratio table only accepts indices 0..4.
#pragma once
static MsvcString* __fastcall MapRatioText(MsvcString* out,int format) {
    if(format<0 || format>=MAX_RATIOS)format=0;
    char text[16];_snprintf_s(text,sizeof text,_TRUNCATE,"1:%d",format+1);
    SsoString(out,text);return out;
}
static const uint8_t kRatioTextBytes[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x85,0xd2,0x74,0x68,0x83,0xea,0x01};
static const uint8_t kRatioLoopBytes[]={0x83,0xfb,0x05,0x0f,0x8c,0x53,0xff,0xff,0xff};
static const uint8_t kRatioGateBytes[]={0x83,0xfb,0x02,0x7e,0x13};
static int InstallMapRatios() {
    if(g_gog || !g_orig || g_maxRatio<=5 || g_maxRatio>MAX_RATIOS)return 0;
    int count=g_maxRatio;
    if(count>EffectiveMaxTiles()/2)count=EffectiveMaxTiles()/2;
    if(count<=5)return 0;
    if(!H->verifyBytes(0x880940,kRatioTextBytes,sizeof kRatioTextBytes) ||
       !H->verifyBytes(0x662ac9,kRatioLoopBytes,sizeof kRatioLoopBytes) ||
       !H->verifyBytes(0x662a25,kRatioGateBytes,sizeof kRatioGateBytes)) {
        H->log("map ratios: Steam byte mismatch; no extra rows");return 0;
    }
    // Whole formatter replacement: its short conditional branches cannot be
    // copied into the host's non-relocating trampoline. All strings fit SSO.
    uint8_t jump[sizeof kRatioTextBytes];memset(jump,0x90,sizeof jump);
    jump[0]=0xff;jump[1]=0x25;memset(jump+2,0,4);
    uintptr_t dest=reinterpret_cast<uintptr_t>(MapRatioText);memcpy(jump+6,&dest,8);
    if(!H->patchBytes(0x880940,jump,sizeof jump))return 0;
    uint8_t enabled=uint8_t(count-1),bound=uint8_t(count);
    if(!H->patchBytes(0x662a27,&enabled,1)) {
        H->patchBytes(0x880940,kRatioTextBytes,sizeof kRatioTextBytes);return 0;
    }
    if(!H->patchBytes(0x662acb,&bound,1)) {
        uint8_t stock=2;H->patchBytes(0x662a27,&stock,1);
        H->patchBytes(0x880940,kRatioTextBytes,sizeof kRatioTextBytes);return 0;
    }
    H->log("map ratios: 1:1 through 1:%d enabled; even tiles, octree and heightmap caps retained",count);
    return 1;
}
extern "C" __declspec(dllexport) void* BigmapTestRatioText(MsvcString* out,int format){return MapRatioText(out,format);}
extern "C" __declspec(dllexport) int BigmapTestInstallRatios(const Tpf2mpHost* host,int gog,int maxRatio,int cap,int hasSizeHook) {
    H=host;g_gog=gog!=0;g_maxRatio=maxRatio;g_maxTiles=cap;g_octreeOn=true;g_octreeDepth=13;
    g_orig=hasSizeHook?reinterpret_cast<GetNumTilesFn>(1):nullptr;
    return InstallMapRatios();
}
extern "C" __declspec(dllexport) uint64_t BigmapTestRatioSize(int size,int format,void* cfg,GetNumTilesFn original,int cap) {
    g_orig=original;g_sizeRowsLive=false;g_numClaims=0;g_tilesX=g_tilesY=0;
    g_maxTiles=cap;g_octreeOn=true;g_octreeDepth=13;return Detour(size,format,cfg);
}
