// datadir_linux.h -- the Linux counterpart of datadir.h: the ONE runtime data
// directory every half of the mod agrees on.
//
// TPF2MP_DATADIR, if set, wins; otherwise $XDG_DATA_HOME/tpf2mp/data, otherwise
// $HOME/.local/share/tpf2mp/data. The preload publishes the resolved path as
// TPF2MP_DATADIR and the data home as LOCALAPPDATA, so unchanged Windows Lua
// selects the same directory once the bridge writes its identity file.
//
// Under the Steam snap, HOME is ~/snap/steam/common for the game and everything
// it starts, so the folder lands in ~/snap/steam/common/.local/share/tpf2mp/data.
// That is expected: the snap cannot see the real ~/.local.
// Header-only so each library's build stays single-file.
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

// mkdir -p for an absolute path. Returns false if the final directory is missing.
static inline bool Tpf2mpMkdirs(const char* path)
{
    char buf[4096];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) return false;
    for (char* p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0755);
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
    struct stat st;
    return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}

// The folder that holds data/ and any locally deployed libraries
// ($XDG_DATA_HOME/tpf2mp), without a trailing slash. Empty when TPF2MP_DATADIR
// is the only source: that override names the data folder alone.
static inline bool Tpf2mpRootDir(char* out, size_t cch)
{
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    int n;
    if (xdg && xdg[0] == '/')      n = snprintf(out, cch, "%s/tpf2mp", xdg);
    else if (home && home[0] == '/') n = snprintf(out, cch, "%s/.local/share/tpf2mp", home);
    else { if (cch) out[0] = 0; return false; }
    return n > 0 && (size_t)n < cch;
}

// Fills `out` with the data dir INCLUDING a trailing slash, creating it if
// needed. Returns false when no candidate is set or it cannot be created.
static inline bool Tpf2mpDataDirA(char* out, size_t cch)
{
    const char* env = getenv("TPF2MP_DATADIR");
    char root[4096];
    int n;
    if (env && env[0] == '/')           n = snprintf(out, cch, "%s", env);
    else if (Tpf2mpRootDir(root, sizeof(root))) n = snprintf(out, cch, "%s/data", root);
    else return false;
    if (n <= 0 || (size_t)n + 1 >= cch) return false;
    if (!Tpf2mpMkdirs(out)) return false;
    if (out[n - 1] != '/') { out[n] = '/'; out[n + 1] = 0; }
    return true;
}
