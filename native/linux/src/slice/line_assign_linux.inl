// Included in slice_lines. Build 35924 UI::UpdateLineAssignment 10ba900 has
// six SysV register arguments. 10c2434..10c251e builds the same 14-pointer
// MovePathUtilContext as Windows; only the enclosing CGame offsets differ.
namespace assignment {
constexpr uintptr_t rva=0x10ba900;
struct Context {uintptr_t f[14];};
using Assign=void(*)(void*,void*,const int32_t*,void*,bool*,uint8_t);
static void* original=nullptr;
struct Capture {Context context;int32_t entity;uint64_t at;uint8_t flag;bool valid;};
static thread_local Capture capture{};
static long seen=0;
static void Hook(void* engine,void* context,const int32_t* entity,void* line,bool* ok,uint8_t flag) {
    capture.valid=SliceReadT(uintptr_t(entity),&capture.entity)&&SliceReadT(uintptr_t(context),&capture.context);
    capture.at=SliceNowMs();capture.flag=flag;
    reinterpret_cast<Assign>(original)(engine,context,entity,line,ok,flag);
}
static uintptr_t World(uintptr_t engine) {
    uintptr_t ui=0,game=0,pair=0,candidates[3]{};
    if(!SliceReadT(SliceAddr(0x5a4fb38),&ui)||!ui||!SliceReadT(ui+0x448,&game)||!game)return 0;
    // CGame::Sync a30e87..a30ecb: GameSim at +160 owns two states; +150
    // is the UI's current state. Match the factory's engine, never a guess.
    SliceReadT(game+0x150,&candidates[0]);
    if(SliceReadT(game+0x160,&pair)&&pair)SliceRead(pair,candidates+1,16);
    for(auto gs:candidates){uintptr_t e=0;if(gs&&SliceReadT(gs+0x28,&e)&&e==engine)return gs;}
    return 0;
}
static bool Build(uintptr_t gs,Context* out) {
    uintptr_t h8=0;if(!gs||!SliceReadT(gs+8,&h8)||!h8)return false;
    const size_t offsets[]={0x38,0,0,0x20,0x28,0x138,0x150,0x1a0,0x158,0x160,0xb0,0x170,0x180,0x200};
    for(unsigned i=0;i<14;++i) {
        const uintptr_t address=i==1?h8+0x18:i==2?h8+0x90:gs+offsets[i];
        if(!SliceReadT(address,&out->f[i]))return false;
    }
    return true;
}
static int Tag(uintptr_t engine,int32_t entity) {
    if(!SliceHookInstalled(rva)||!capture.valid||capture.entity!=entity||SliceNowMs()-capture.at>2000)return -1;
    Context mine{};
    if(!Build(World(engine),&mine)||memcmp(&mine,&capture.context,sizeof(mine))) {
        SliceLog("[lineassign] context mismatch for line %d; capture assignment tag refused\n",entity);return -1;
    }
    return capture.flag?1:0;
}
static void Replay(uintptr_t engine,int32_t entity,uintptr_t line) {
    if(!g_spareGuard||!SliceHookInstalled(rva))return;
    char instance[8];if(!SliceInstance(instance,sizeof(instance)))return;
    const auto path=std::string(SliceDataDir())+"lockstep_lassign_"+instance+".txt";
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return;
    struct stat st{};char data[128]{};const auto now=time(nullptr);
    const bool fresh=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<127&&
        now>=st.st_mtim.tv_sec&&now-st.st_mtim.tv_sec<=5;
    const auto n=fresh?read(fd,data,sizeof(data)-1):-1;close(fd);if(n<=0)return;
    long lid=0,flag=0,seq=0;char extra;
    if(sscanf(data,"%ld %ld %ld %c",&lid,&flag,&seq,&extra)!=3||lid!=entity||seq==seen||seq<=0||(flag!=0&&flag!=1))return;
    seen=seq;Context context{};
    if(!Build(World(engine),&context)) {SliceLog("[lineassign] replay line %d has no matching world\n",entity);return;}
    const bool ok=callGame(reinterpret_cast<Assign>(original),reinterpret_cast<void*>(engine),&context,&entity,
                           reinterpret_cast<void*>(line),static_cast<bool*>(nullptr),uint8_t(flag));
    SliceLog("[lineassign] replay line=%d flag=%ld seq=%ld: %s\n",entity,flag,seq,ok?"assigned at stamp":"engine threw");
}
static void Install() {
    static const uint8_t bytes[]={0x55,0x48,0x89,0xf8,0x45,0x0f,0xb6,0xc9,0x48,0x89,0xe5,0x53,0x48,0x89,0xcb};
    SliceRegisterHook({"slice-lines","platform assignment",-1,rva,bytes,sizeof(bytes),15,
                      reinterpret_cast<void*>(Hook),&original,true});
}
}
