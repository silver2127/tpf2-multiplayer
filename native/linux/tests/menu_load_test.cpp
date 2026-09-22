#include "../src/menu_game_linux.cpp"
#include <cassert>
static unsigned calls=0;
static bool allow=true;
static std::string observed;
static uint8_t Original(void*,void*,void*) { ++calls; return allow; }
static void Observer(const char* name) { observed=name; }
int main()
{
    // Synthetic objects only: never execute the ELF. Reject absent pointers,
    // wrong vtables, unreadable memory and non-finite/out-of-range floats.
    alignas(8) unsigned char menu[0x4a0]{}, bar[0x448]{}, monitor[16]{};
    uintptr_t pointer=uintptr_t(bar); memcpy(menu+0x498,&pointer,8);
    pointer=uintptr_t(monitor); memcpy(bar+0x440,&pointer,8);
    pointer=0x59d8c60; memcpy(monitor,&pointer,8);
    MenuGame_ObserveMenu(menu);
    assert(MenuGame_LoadPercent()==-1);
    g_progressReady=true;
    for (float value : {0.0f,0.125f,0.999f,1.0f}) {
        memcpy(monitor+8,&value,4); assert(MenuGame_LoadPercent()==int(value*100));
    }
    for (float value : {-1.0f,1.1f,__builtin_nanf("")}) {
        memcpy(monitor+8,&value,4); assert(MenuGame_LoadPercent()==-1);
    }
    float value=0.5; memcpy(monitor+8,&value,4); monitor[0]^=1;
    assert(MenuGame_LoadPercent()==-1);
    MenuGame_ObserveMenu(reinterpret_cast<void*>(1)); assert(MenuGame_LoadPercent()==-1);
    MenuGame_ObserveMenu(nullptr); assert(MenuGame_LoadPercent()==-1);
    g_originalStartSavegame=reinterpret_cast<void*>(&Original);
    MenuGame_ObserveLoads(&Observer);
    std::string name="test world";
    assert(StartSavegameDetour(nullptr,&name,nullptr)); assert(observed==name && calls==1);
    observed.clear(); allow=false;
    assert(!StartSavegameDetour(nullptr,&name,nullptr)); assert(observed.empty());
    allow=true; t_alInFlight=1;
    assert(StartSavegameDetour(nullptr,&name,nullptr)); assert(observed.empty());
    t_alInFlight=0; name="../not-a-save";
    assert(StartSavegameDetour(nullptr,&name,nullptr)); assert(observed.empty());
    name=std::string("with\0nul",8);
    assert(StartSavegameDetour(nullptr,&name,nullptr)); assert(observed.empty());
    puts("vanilla load observer: accepted, refused, own autoload, invalid names PASS");
}
