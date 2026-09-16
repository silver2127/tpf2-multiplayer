// Included by slice_hook.cpp after Readable/IsHeapPtr/Log. Build 35924.
// A modular station has many frozen nodes/segments, unlike the old depot weld.
// Adopt ONLY the snapped boundary segment, and remap indices after compaction.
// No count cap (2026-09-16): a station of any size welds. The old 64-node /
// 64-segment cap answered "not ours" to a bigger one on every instance, and
// the raw apron was built beside ours. STATION_WELD_SANITY_BYTES is the
// misread-pointer guard on a vector span, not a limit on a station.
#include <vector>
static const uint64_t STATION_WELD_SANITY_BYTES = 1ull << 30;
static bool MergeStationEndpoint(uint64_t p)
{
    if (!Readable((void*)p, 0x210)) return false;
    auto q = [](uint64_t a) { uint64_t v; memcpy(&v,(void*)a,8); return v; };
    uint64_t nb=q(p), ne=q(p+8), sb=q(p+0x18), se=q(p+0x20);
    uint64_t cb=q(p+0x1f8), ce=q(p+0x200);
    if (ce-cb!=0x8e0 || !Readable((void*)cb,0x8e0) || ne<nb || se<sb ||
        (ne-nb)%24 || (se-sb)%120) return false;
    if (ne-nb>STATION_WELD_SANITY_BYTES || se-sb>STATION_WELD_SANITY_BYTES) {
        Log("[station-weld] node/segment vectors span %llu/%llu B -- past the misread-pointer bound, not ours\n",
            (unsigned long long)(ne-nb), (unsigned long long)(se-sb));
        return false;
    }
    int n=(int)((ne-nb)/24), m=(int)((se-sb)/120);
    if(n<4 || m<3 || !Readable((void*)nb,(size_t)(ne-nb)) || !Readable((void*)sb,(size_t)(se-sb))) return false;
    auto ni = [&](int i) { int v; memcpy(&v,(void*)(nb+i*24+20),4); return v; };
    auto si = [&](int i,int off) { int v; memcpy(&v,(void*)(sb+i*120+off),4); return v; };
    auto node = [&](int id) { for(int i=0;i<n;i++) if(ni(i)==id) return i; return -1; };
    auto xyz = [&](int i,float* v) { memcpy(v,(void*)(nb+i*24),12); };
    std::vector<int> degree(n,0); std::vector<uint8_t> owned(n,0);
    for(int s=0;s<m;s++) if(si(s,0x74)==1) {
        for(int off=8;off<=12;off+=4) { int i=node(si(s,off)); if(i>=0) {degree[i]++;owned[i]=true;} }
    }
    int o=-1,a=-1,x=-1,t=-1,u=-1;
    // Require a unique exact inner-node match and a dangling template endpoint
    // pointing the same way as the captured connector. Never pick an interior edge.
    for(int s=0;s<m;s++) if(si(s,0x74)==0) {
        int v0=si(s,8),v1=si(s,12);
        if((v0<0)==(v1<0)) continue;
        int xi=node(v0<0?v0:v1); if(xi<0 || owned[xi]) continue;
        float xp[3],dir[3]; xyz(xi,xp);
        memcpy(dir,(void*)(sb+s*120+(v0<0?0x10:0x1c)),12);
        if(v1<0) for(int k=0;k<3;k++) dir[k]=-dir[k];
        for(int ti=0;ti<n;ti++) if(owned[ti]) {
            float tp[3]; xyz(ti,tp); float d=0; for(int k=0;k<3;k++) d+=(xp[k]-tp[k])*(xp[k]-tp[k]);
            if(d>0.0625f) continue;
            for(int j=0;j<m;j++) if(si(j,0x74)==1 && si(j,4)==si(s,4)) {
                int j0=si(j,8),j1=si(j,12),ui=-1;
                if(j0==ni(ti)) ui=node(j1); else if(j1==ni(ti)) ui=node(j0);
                if(ui<0 || degree[ui]!=1) continue;
                float up[3]; xyz(ui,up); float dot=0,dd=0,ll=0;
                for(int k=0;k<3;k++) {float v=up[k]-tp[k];dot+=v*dir[k];dd+=dir[k]*dir[k];ll+=v*v;}
                if(dot<=0 || dot*dot<0.99f*dd*ll || dd<1e-6f || ll<1e-6f) continue;
                if(o>=0) return false;
                o=s;a=j;x=xi;t=ti;u=ui;
            }
        }
    }
    if(o<0) return false;
    int before; memcpy(&before,(void*)(cb+0x780),4);
    if(before<0 || before>m || o>=before || a<before) return false;
    // The non-empty construction-edge set has a different container layout.
    // Refuse that shape until its index update is implemented.
    if(q(p+0x198)!=0) return false;
    uint64_t fb[2]={q(p+0x170),q(cb+0x768)}, fe[2]={q(p+0x178),q(cb+0x770)};
    for(int k=0;k<2;k++) {
        // any number of frozen indices (the old 256-byte cap allowed 64); the
        // span bound is the misread guard, every index is checked against n below
        if(fe[k]<fb[k] || (fe[k]-fb[k])%4 || fe[k]-fb[k]>STATION_WELD_SANITY_BYTES ||
            (fe[k]>fb[k] && !Readable((void*)fb[k],(size_t)(fe[k]-fb[k])))) return false;
        for(uint64_t v=fb[k];v<fe[k];v+=4) {int i;memcpy(&i,(void*)v,4);if(i<0||i>=n||i==u)return false;}
    }
    if(fb[0]<fe[1] && fb[1]<fe[0] && (fb[0]!=fb[1] || fe[0]!=fe[1]))return false;
    uint64_t tb=q(p+0x1c8),te=q(p+0x1d0);
    if(te<tb || (te!=tb && (te-tb!=(uint64_t)m*32 || !Readable((void*)tb,m*32))))return false;
    // A removed script connector must own no heap-backed object vector.
    if(q(sb+o*120+0x30)!=q(sb+o*120+0x38))return false;
    std::vector<int> remap(n); int count=0;
    for(int i=0;i<n;i++) remap[i]=(i==x||i==u)?-1:count++;
    remap[x]=remap[t];
    // All validation is above this point: rejected proposals are untouched.
    memcpy((void*)(sb+a*120+8),(void*)(sb+o*120+8),32);
    int tid=ni(t),xid=ni(x);
    for(int j=0;j<m;j++) for(int off=8;off<=12;off+=4)
        if(si(j,off)==xid)memcpy((void*)(sb+j*120+off),&tid,4);
    for(int k=0;k<2;k++) if(k==0 || fb[1]!=fb[0]) for(uint64_t v=fb[k];v<fe[k];v+=4) {
        int i;memcpy(&i,(void*)v,4);i=remap[i];memcpy((void*)v,&i,4);
    }
    for(int i=0,w=0;i<n;i++) if(i!=x&&i!=u) {memmove((void*)(nb+w*24),(void*)(nb+i*24),24);w++;}
    memmove((void*)(sb+o*120),(void*)(sb+(o+1)*120),(m-o-1)*120);
    if(te!=tb) {memmove((void*)(tb+o*32),(void*)(tb+(o+1)*32),(m-o-1)*32);te-=32;memcpy((void*)(p+0x1d0),&te,8);}
    ne-=48;se-=120;before--;
    memcpy((void*)(p+8),&ne,8);memcpy((void*)(p+0x20),&se,8);memcpy((void*)(cb+0x780),&before,4);
    Log("[station-weld] adopted connector %d into station segment %d; nodes %d->%d, segments %d->%d; frozen indices remapped\n",o,a,n,n-2,m,m-1);
    return true;
}
