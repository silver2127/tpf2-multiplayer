#include "../src/panel_linux.cpp"
#include <cassert>

int main(int argc, char** argv)
{
    assert(argc == 3);
    using namespace panel;
    // Fixed/overflow boundaries: company 7 now has a distinct fixed colour;
    // company 21 restarts the golden-angle walk after the 20-colour palette.
    const auto checkColor = [](int cid, int r, int g, int b) {
        const auto c = CoColor(cid);
        assert(c.r == r && c.g == g && c.b == b);
    };
    checkColor(1, 230, 25, 75);
    checkColor(7, 240, 50, 230);
    checkColor(20, 255, 250, 200);
    checkColor(21, 216, 82, 82);
    checkColor(22, 82, 216, 121);
    P().dataDir = argv[2];
    const std::string file = P().dataDir + "tpf2_names.txt";
    unlink(file.c_str());
    LoadNamesLocked();
    assert(P().userAuto && !P().username.empty());
    const std::string fallback = P().username;
    SteamNameTickLocked(); assert(P().username == fallback); // absent SDK: no loading/initializing Steam
    void* fixture = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(fixture);
    setenv("TEST_STEAM_PERSONA", "  A  Steam #name\"\\\n", 1);
    auto tick = [] { P().steamNext = 0; SteamNameTickLocked(); };
    tick(); assert(P().username == fallback); // game has not initialized Steam
    setenv("TEST_STEAM_READY", "1", 1); tick();
    assert(P().username == "A Steam name");
    g_focus = 3;
    setenv("TEST_STEAM_PERSONA", "Renamed", 1); tick(); assert(P().username == "A Steam name");
    g_focus = 0; g_uiState = 2; tick(); assert(P().username == "A Steam name");
    g_uiState = 1; tick(); assert(P().username == "Renamed");
    P().userAuto = false; P().username = "Typed"; SaveNamesLocked();
    P().username.clear(); LoadNamesLocked(); tick(); assert(!P().userAuto && P().username == "Typed");
    FILE* f = fopen(file.c_str(), "w"); assert(f); fputs("player=Legacy\nlobby=Legacy room\n", f); fclose(f);
    LoadNamesLocked(); tick(); assert(!P().userAuto && P().username == "Legacy");
    g_focus = 3; P().username.clear(); TypeLocked(std::string(90, 'a').c_str());
    assert(P().username.size() == 64);
    g_focus = 4; P().lobbyName.clear(); TypeLocked(std::string(90, 'b').c_str());
    assert(P().lobbyName.size() == 64);
    std::string utf8 = "x\xc3\xa9"; PopCharLocked(&utf8); assert(utf8 == "x");
    P().username = std::string(100, 'p'); P().lobbyName = std::string(100, 'l'); SaveNamesLocked();
    P().username.clear(); P().lobbyName.clear(); LoadNamesLocked();
    assert(P().username.size() == 100 && P().lobbyName.size() == 100);
    dlclose(fixture); unlink(file.c_str());
    puts("panel names: Steam availability, sanitizing, refresh, overrides, legacy files and length limits passed");
}
