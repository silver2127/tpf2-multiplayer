#include "../src/panel_linux.cpp"
#include <cassert>
namespace lobby {
void Snapshot(View* v) { *v=panel::P().view; }
bool AutoCopyPending(){return false;} bool TakeAutoCopy(std::string*){return false;}
bool CapturesTyping(){return true;}
bool Start(const StartRequest&,std::string*){assert(false);return false;}
void Leave(){assert(false);} std::string SendChat(const std::string&){assert(false);return {};}
std::string StartGame(){assert(false);return {};}
void RefreshSaves(){} std::string SelectSave(const std::string&){return {};}
std::string RecoveryAction(const std::string&){return {};}
std::string SetSeparateCompanies(bool){return {};}
std::string SetCrossplay(bool){return {};}
std::string SetPublic(bool){return {};}
std::string AnswerMods(bool){return {};}
void CycleCompany(int,bool){} bool CopyCode(std::string*){return false;}
bool PublicRow(int,PubRow*){return false;} void PublicRefresh(){}
bool OpenLogs(){return false;}
}
static bool Has(int id) {
    using namespace panel;
    for(int i=0;i<g_hitCount;++i)if(g_hits[i].id==id)return true;
    return false;
}
static void Key(SDL_Keycode k,bool down,int mods=0) {
    using namespace panel;
    SDL_Event e{};e.type=down?SDL_KEYDOWN:SDL_KEYUP;e.key.keysym.sym=k;e.key.keysym.mod=mods;
    Post p;assert(HandleEventLocked(&e,&p));
}
int main(int argc,char** argv) {
    using namespace panel;
    g_initDone=g_fontsOk=true;g_uiState=1;g_lastFrameMs=NowMs();
    P().username="Player";P().gameDir="/nonexistent/";
    if(argc>1)layer::AddFont(argv[1]);
    if(argc>2) {
        P().gameDir=argv[2];g_s=1;g_titleTab=0;RenderLocked(780,540);
        if(argc>3) {
            PrepareTitleBackdrop(1280,720);layer::PlaceOnBackdrop(1280,720,250,90,g_titleBackdrop.data());
            FILE* f=fopen(argv[3],"wb");assert(f);fprintf(f,"P6\n1280 720\n255\n");
            const auto* pixels=layer::Pixels();
            for(int i=0;i<1280*720;++i) { unsigned char rgb[]={pixels[i*4+2],pixels[i*4+1],pixels[i*4]};fwrite(rgb,1,3,f); }
            fclose(f);
        }
    }
    int w,h;g_flagScale=5;LayoutLocked(1280,720,&w,&h);assert(w<=1280 && h<=720);
    g_flagScale=0;LayoutLocked(1920,1080,&w,&h);assert(w==780 && h==540);
    RenderLocked(w,h);assert(Has(110)&&Has(111)&&Has(8)&&!Has(14)&&!Has(3));
    g_titleTab=1;RenderLocked(w,h);assert(Has(14)&&!Has(8)&&Has(2)&&Has(50));
    Key(SDLK_TAB,true);assert(g_focus==3);Key(SDLK_TAB,false);
    Key(SDLK_TAB,true);assert(g_focus==2);Key(SDLK_TAB,false);
    Key(SDLK_TAB,true,KMOD_SHIFT);assert(g_focus==3);Key(SDLK_TAB,false);
    Key(SDLK_ESCAPE,true);assert(g_uiState==0);Key(SDLK_ESCAPE,false);
    g_uiState=1;g_focus=0;Key(SDLK_RETURN,true);Key(SDLK_RETURN,false);
    P().view.modsPrompt="Required workshop content";RenderLocked(w,h);
    assert(Has(16)&&Has(17)&&!Has(2)&&!Has(110));
    P().view.modsPrompt.clear();g_uiState=2;P().view.isHost=true;
    for(int i=0;i<16;++i)P().view.players.push_back({"Player "+std::to_string(i),"",i+1,true,false});
    RenderLocked(w,h);assert(Has(115)&&!Has(114)&&!Has(6));
    P().view.lobbyReady=true;g_playerPage=1;RenderLocked(w,h);assert(Has(114)&&!Has(115)&&Has(6)&&Has(28)&&Has(35));
    P().savePicker=true;P().view.saves.push_back({"/save/test.sav","Test",0});RenderLocked(w,h);
    assert(Has(100)&&Has(91)&&!Has(6));
    PrepareTitleBackdrop(8,8);assert(g_titleBackdrop.size()==256);
    layer::Begin(2,2);layer::Rect(0,0,2,2,layer::rgb(255,0,0),255);
    layer::PlaceOnBackdrop(8,8,3,4,g_titleBackdrop.data());
    const auto* px=layer::Pixels();assert(px[(4*8+3)*4+2]==255 && px[0]==65 && layer::Width()==8);
    puts("title: layout, tab fields, key down/up capture, modal isolation, roster/save paging, readiness and backdrop placement passed");
}
