#include "../src/menu_game_linux.cpp"
#include <cassert>
static unsigned calls=0;
static bool allow=true;
static std::string observed;
static uint8_t Original(void*,void*,void*) { ++calls; return allow; }
static void Observer(const char* name) { observed=name; }
int main()
{
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
