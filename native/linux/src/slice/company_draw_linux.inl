// Native ItemCreator draw paths, Steam build 35924. Unlike Windows, ordinary
// vehicle vertices have no RGBA field. Move their six copied vertices to the
// game's HudColorVertexBuffer, retaining positions, UVs, layer and texture.
#include <cmath>

namespace {
constexpr uintptr_t kVehicleDraw = 0x1385ec0, kPlainBuffer = 0x137cce0;
constexpr uintptr_t kColorBuffer = 0x137d010, kColorGrow = 0x1385be0;
constexpr uintptr_t kLabelDraw = 0x1383570;
constexpr uintptr_t kVehicleDrawCalls[] = {0x138bcd8,0x138c08d,0x138c359,0x138c4dd};
struct DrawRect { uint64_t position, size; };
struct DrawVertex { float xy[2], uv[2], rgba[4]; };
static_assert(sizeof(DrawVertex)==32, "HudColorVertexBuffer stride");
using VehicleDrawFn = DrawRect(*)(void*,void*,int,float,float,float,float);
using BufferFn = uintptr_t(*)(void*,int,void*);
using GrowFn = void(*)(uintptr_t,uintptr_t,const DrawVertex*);
std::atomic<uint64_t> vehicleDraws{0}, labelDraws{0};

void CompanyRgb(int cid,float* out)
{
    static const int colors[20][3]={{230,25,75},{0,130,200},{60,180,75},{245,130,48},
        {145,30,180},{70,240,240},{240,50,230},{255,225,25},{0,128,128},{170,110,40},
        {210,245,60},{128,0,0},{0,0,128},{128,128,0},{250,190,212},{220,190,255},
        {170,255,195},{255,215,180},{128,128,128},{255,250,200}};
    if(cid>=1 && cid<=20){for(int i=0;i<3;++i)out[i]=colors[cid-1][i]/255.f;return;}
    double hd=(cid-21)*137.508;
    float h=float(hd-int(hd/360)*360);if(h<0)h+=360;
    const float c=.85f*.62f,x=c*(1-std::fabs(std::fmod(h/60,2)-1)),m=.85f-c;
    if(h<60){out[0]=c;out[1]=x;out[2]=0;}
    else if(h<120){out[0]=x;out[1]=c;out[2]=0;}
    else if(h<180){out[0]=0;out[1]=c;out[2]=x;}
    else if(h<240){out[0]=0;out[1]=x;out[2]=c;}
    else if(h<300){out[0]=x;out[1]=0;out[2]=c;}
    else{out[0]=c;out[1]=0;out[2]=x;}
    for(int i=0;i<3;++i)out[i]+=m;
}

bool EntityColor(uintptr_t engine,const int* entity,float* rgba)
{
    try {
        int id=-1;
        if(!engine || !entity || !SliceReadT(reinterpret_cast<uintptr_t>(entity),&id) || id<0)return false;
        const int owner=SliceEcsOwner(engine,id);
        const int company=owner>=0?CompanyOfPid(owner):0;
        if(company<=0)return false;
        CompanyRgb(company,rgba);rgba[3]=1;return true;
    } catch(...) { ++refused;return false; }
}

bool VertexRange(uintptr_t buffer,size_t stride,uintptr_t (&range)[3])
{
    return buffer && SliceRead(buffer+8,range,sizeof(range)) &&
        range[1]>=range[0] && range[2]>=range[1] &&
        (range[1]-range[0])%stride==0 && (range[2]-range[0])%stride==0 &&
        range[2]-range[0]<=128*1024*1024 && (!range[2] || range[0]);
}
}

extern "C" __attribute__((visibility("hidden")))
DrawRect SliceTintVehicleDraw(void* buffer,void* texture,int layer,const int* entity,
                              uintptr_t engine,float x,float y,float width,float height)
{
    float rgba[4];
    const bool tint=EntityColor(engine,entity,rgba);
    uintptr_t plain=0,before[3]{};
    if(tint){
        plain=reinterpret_cast<BufferFn>(tintBase+kPlainBuffer)(buffer,layer,texture);
        if(!VertexRange(plain,16,before))plain=0;
    }
    const auto result=reinterpret_cast<VehicleDrawFn>(tintBase+kVehicleDraw)(buffer,texture,layer,x,y,width,height);
    uintptr_t after[3]{};
    if(!plain || !VertexRange(plain,16,after) ||
       after[1]-after[0] != before[1]-before[0]+6*16)return result;
    DrawVertex vertices[6]{};
    for(int i=0;i<6;++i){
        if(!SliceRead(after[1]-6*16+i*16,&vertices[i],16))return result;
        memcpy(vertices[i].rgba,rgba,16);
    }
    const auto colored=reinterpret_cast<BufferFn>(tintBase+kColorBuffer)(buffer,layer,texture);
    uintptr_t target[3]{};
    if(!VertexRange(colored,32,target))return result;
    for(const auto& v:vertices){
        // The game's grow helper copies the complete 32-byte vertex before
        // returning; its own allocator retains ownership of both vectors.
        const auto end=*reinterpret_cast<uintptr_t*>(colored+16);
        const auto cap=*reinterpret_cast<uintptr_t*>(colored+24);
        if(end==cap)reinterpret_cast<GrowFn>(tintBase+kColorGrow)(colored+8,end,&v);
        else {memcpy(reinterpret_cast<void*>(end),&v,32);*reinterpret_cast<uintptr_t*>(colored+16)=end+32;}
    }
    *reinterpret_cast<uintptr_t*>(plain+16)=after[1]-6*16;
    ++vehicleDraws;
    return result;
}

extern "C" __attribute__((visibility("hidden"),naked)) void SliceTintVehicleEntry()
{
    // All four call sites retain ItemCreatorImpl in rbx and entity* in r12.
    // Integer and SSE argument banks are independent under SysV.
    asm volatile("mov %r12,%rcx\nmov 0x28(%rbx),%r8\njmp SliceTintVehicleDraw");
}

extern "C" __attribute__((visibility("hidden")))
void SliceTintLabelDraw(void* buffer,void* texture,const float* original,uint64_t position,
                        uint64_t size,const int* entity)
{
    alignas(16) static thread_local float color[4];
    uintptr_t ui=0,game=0,state=0,engine=0;
    if(SliceReadT(tintBase+0x5a4fb38,&ui) && ui && SliceReadT(ui+0x448,&game) && game &&
       SliceReadT(game+0x150,&state) && state && SliceReadT(state+0x28,&engine) &&
       EntityColor(engine,entity,color)) {original=color;++labelDraws;}
    reinterpret_cast<void(*)(void*,void*,const float*,uint64_t,uint64_t)>(tintBase+kLabelDraw)
        (buffer,texture,original,position,size);
}
extern "C" __attribute__((visibility("hidden"),naked)) void SliceTintLabelEntry()
{
    asm volatile("mov -0xd0(%rbp),%r9\njmp SliceTintLabelDraw");
}

namespace {
void InstallDrawTint()
{
    if(FlagSays("iconcolor","0") || !SliceEcsAnchored(tintBase))return;
    if(!Anchored(tintBase,kTintDrawChecks))return;
    size_t count=0;
    for(const auto site:kVehicleDrawCalls)
        if(Tpf2mpRedirectCall(tintBase+site,tintBase+kVehicleDraw,reinterpret_cast<void*>(&SliceTintVehicleEntry)))++count;
    const bool label=Tpf2mpRedirectCall(tintBase+0x1389559,tintBase+kLabelDraw,reinterpret_cast<void*>(&SliceTintLabelEntry));
    SliceLog("[iconcolor] %zu/4 vehicle draw calls; station label=%d\n",count,int(label));
}
}
