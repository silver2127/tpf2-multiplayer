// cfg_linux.cpp -- the Linux build of plugin/cfg.cpp. Parse and Find follow
// cfg.cpp rule for rule; files are read with stdio, and a value is a string
// that is never freed, so a pointer CfgStr returned cannot dangle
// (cfg_linux.h).
#include "cfg_linux.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace tpf2mp {
namespace {

// section -> key -> value, with sections and keys lowercased so lookups are
// case-insensitive without allocating on every read. Replacing a key points it
// at a new string and leaves the old one where a plugin may still be reading
// it. The settings files are a few KB, so the leak is bounded by what they hold.
typedef std::map<std::string, std::map<std::string, const std::string*> > Table;

struct State {
    std::mutex  mtx;
    Table       table;
    std::string path;
};

// Leaked on purpose: a plugin's thread may still read settings while exit()
// runs static destructors.
State& S()
{
    static State* s = new State;
    return *s;
}

std::string Lower(const std::string& s)
{
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

// Read a whole file; lines are split in Parse, so a line of any length stays
// one line. `err` is 0 on success, else the errno of the open or the read:
// ENOENT for a missing file; fopen succeeds on a directory on Linux, and the
// read then fails with EISDIR.
bool ReadWhole(const char* path, std::string& out, int& err)
{
    err = 0;
    FILE* f = fopen(path, "rbe");
    if (!f) {
        err = errno ? errno : EIO;
        return false;
    }
    out.clear();
    char buf[8192];
    size_t got;
    errno = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, got);
    const bool ok = !ferror(f);
    if (!ok) err = errno ? errno : EIO;   // saved before fclose can change errno
    fclose(f);
    return ok;
}

void Parse(const std::string& text, Table& table)
{
    std::string section;   // "" until the first [header]
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (nl == std::string::npos) pos = text.size() + 1; else pos = nl + 1;

        std::string line = Trim(raw);
        if (line.empty()) continue;
        // A comment is decided by the FIRST non-blank character only (cfg.h).
        if (line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close != std::string::npos) section = Lower(Trim(line.substr(1, close - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;      // not key=value; ignore
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;
        const std::string*& slot = table[section][key];
        if (!slot || *slot != val) slot = new std::string(val);   // later wins; the old string stays
    }
}

// Returns nullptr when absent, so callers can tell "not set" from "set to the
// same thing as the default". Called with the lock held.
const std::string* Find(const char* section, const char* key)
{
    std::string s = Lower(section ? section : "");
    std::string k = Lower(key ? key : "");
    Table::const_iterator si = S().table.find(s);
    if (si == S().table.end()) return nullptr;
    std::map<std::string, const std::string*>::const_iterator ki = si->second.find(k);
    if (ki == si->second.end()) return nullptr;
    return ki->second;
}

}  // namespace

const std::string& CfgLoad(const std::vector<std::string>& dirs,
                           std::vector<std::pair<std::string, int> >* unreadable)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    S().table.clear();
    S().path.clear();

    std::string text;
    for (size_t i = 0; i < dirs.size(); ++i) {
        if (dirs[i].empty()) continue;
        std::string p(dirs[i]);
        if (p[p.size() - 1] != '/') p += '/';
        p += "tpf2mp.cfg";
        int err = 0;
        if (!ReadWhole(p.c_str(), text, err)) {
            if (err != ENOENT && unreadable) unreadable->push_back(std::make_pair(p, err));
            continue;                           // skipped like an absent file, as on Windows
        }
        Parse(text, S().table);
        S().path = p;
        break;                                  // first file found wins
    }
    return S().path;
}

const std::string& CfgPath() { return S().path; }

bool CfgMergeFile(const char* path, int* err)
{
    int e = EINVAL;
    std::string text;
    const bool read = path && path[0] && ReadWhole(path, text, e);
    if (err) *err = read ? 0 : e;
    if (!read) return false;
    std::lock_guard<std::mutex> lk(S().mtx);
    Parse(text, S().table);                     // Parse assigns, so later wins
    return true;
}

const char* CfgStr(const char* section, const char* key, const char* def)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    const std::string* v = Find(section, key);
    return v ? v->c_str() : def;               // never freed (see Table)
}

int CfgInt(const char* section, const char* key, int def)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    const std::string* v = Find(section, key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    long n = strtol(v->c_str(), &end, 0);       // 0 => 0x.. hex also accepted
    if (end == v->c_str()) return def;          // nothing parsed
    while (*end == ' ' || *end == '\t') ++end;
    if (*end) return def;                       // trailing junk: not a number
    if (n > INT_MAX) return INT_MAX;            // long is 64-bit here, 32-bit on Windows
    if (n < INT_MIN) return INT_MIN;
    return (int)n;
}

bool CfgBool(const char* section, const char* key, bool def)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    const std::string* v = Find(section, key);
    if (!v) return def;
    std::string s = Lower(*v);
    if (s == "1" || s == "true"  || s == "yes" || s == "on")  return true;
    if (s == "0" || s == "false" || s == "no"  || s == "off") return false;
    return def;
}

bool CfgHasKey(const char* section, const char* key)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    return Find(section, key) != nullptr;
}

}  // namespace tpf2mp
