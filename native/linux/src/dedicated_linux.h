#pragma once
#include <string>

namespace dedicated {
struct Settings {
    bool enabled = false, publicGame = true, companies = false;
    bool render = false, pinBatch = true;
    int autosaveMinutes = 10, emptySpeed = 1, port = 0, fps = 30;
    std::string save, lobby = "Dedicated Server", name = "Server", password;
};
Settings Read(const std::string& path);
// Called before the Vulkan dispatcher and lobby threads are installed.
void Configure(const std::string& flagsPath, const std::string& dataDir);
const Settings& Get();
}
