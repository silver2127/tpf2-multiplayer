// Shared kill-switch reader: root flags file takes precedence over data.
#pragma once
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

inline bool SliceFlagOff(const char* root, const char* data, const char* key)
{
    for (const char* dir : {root, data}) {
        if (!dir || !*dir) continue;
        std::string path = std::string(dir) + "/tpf2_menu_flags.txt";
        FILE* f = fopen(path.c_str(), "r");
        if (!f) continue;
        char line[256]; bool off = false;
        const std::string setting = std::string(key) + "=0";
        while (fgets(line, sizeof(line), f)) if (!strncmp(line, setting.c_str(), setting.size())) off = true;
        fclose(f); return off;
    }
    return false;
}
