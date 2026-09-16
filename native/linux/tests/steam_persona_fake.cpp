#include <cstdlib>
extern "C" int SteamAPI_GetHSteamUser() { return std::getenv("TEST_STEAM_READY") ? 1 : 0; }
extern "C" void* SteamAPI_SteamFriends_v017() { static int friends; return &friends; }
extern "C" const char* SteamAPI_ISteamFriends_GetPersonaName(void*) { return std::getenv("TEST_STEAM_PERSONA"); }
