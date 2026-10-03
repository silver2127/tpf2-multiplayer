#pragma once
#include <cstdint>
// The game's Steam poll thread, throttled while it is busy (steam_poll_linux.cpp).
// False, changing nothing, when steam_poll_ms=0 or a guard fails.
bool SliceInstallSteamPoll(uintptr_t base, const char* root, const char* data);
