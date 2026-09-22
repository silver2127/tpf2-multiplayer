#include "dedicated_linux.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

namespace dedicated {
namespace {
Settings& Current() { static auto* s = new Settings; return *s; }
std::string Trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? "" : s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}
bool Number(const std::string& s, int lo, int hi, int* out) {
    if (s.empty()) return false;
    char* end = nullptr; errno = 0;
    const long n = strtol(s.c_str(), &end, 10);
    if (errno || !end || *end || n < lo || n > hi) return false;
    *out = int(n); return true;
}
}
Settings Read(const std::string& path) {
    Settings s;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos || line.empty() || line[0] == '#') continue;
        const auto key = Trim(line.substr(0, equal));
        const auto value = Trim(line.substr(equal + 1));
        int n;
        if (key == "dedicated" && Number(value, 0, 1, &n)) s.enabled = n;
        else if (key == "dedicated_public" && Number(value, 0, 1, &n)) s.publicGame = n;
        else if (key == "dedicated_companies" && Number(value, 0, 1, &n)) s.companies = n;
        else if (key == "dedicated_render" && Number(value, 0, 1, &n)) s.render = n;
        else if (key == "dedicated_pin_batch" && Number(value, 0, 1, &n)) s.pinBatch = n;
        else if (key == "dedicated_autosave_min" && Number(value, 0, 600, &n)) s.autosaveMinutes = n;
        else if (key == "dedicated_empty_speed" && Number(value, 0, 4, &n)) s.emptySpeed = n;
        else if (key == "dedicated_pause_empty" && Number(value, 0, 1, &n) && n) s.emptySpeed = 0;
        else if (key == "dedicated_port" && Number(value, 0, 65535, &n)) s.port = n;
        else if (key == "dedicated_fps" && Number(value, 1, 240, &n)) s.fps = n;
        else if (key == "dedicated_save" && value.size() < 64 &&
                 value.find_first_of("/\\\r\n") == std::string::npos &&
                 (value.empty() || value[0] != '.')) s.save = value;
        else if (key == "dedicated_lobby" && !value.empty()) s.lobby = value.substr(0, 63);
        else if (key == "dedicated_name" && !value.empty()) s.name = value.substr(0, 31);
        else if (key == "dedicated_password") s.password = value.substr(0, 39);
    }
    return s;
}
void Configure(const std::string& flagsPath, const std::string& dataDir) {
    Current() = Read(flagsPath);
    const auto& s = Current();
    const std::string path = dataDir + "mp_dedicated.txt";
    if (!s.enabled) { unlink(path.c_str()); return; }
    const std::string temporary = path + ".tmp";
    if (FILE* f = fopen(temporary.c_str(), "w")) {
        const bool written = fprintf(f, "dedicated=1\nempty_speed=%d\npause_empty=%d\npin_batch=%d\n",
                                     s.emptySpeed, int(s.emptySpeed == 0), int(s.pinBatch)) > 0;
        const bool closed = fclose(f) == 0;
        if (written && closed) rename(temporary.c_str(), path.c_str());
    }
}
const Settings& Get() { return Current(); }
}
