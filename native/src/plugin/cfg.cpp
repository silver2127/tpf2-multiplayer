#include "cfg.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace tpf2mp {
namespace {

// section -> key -> value, both keys lowercased so lookups are case-insensitive
// without allocating on every read.
typedef std::map<std::string, std::map<std::string, std::string> > Table;

Table       g_table;
std::string g_path;
std::mutex  g_mtx;

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

// Read a whole file. Lines are split here rather than with fgets so a line of
// any length stays one line -- the 128-byte truncation bug is structurally
// impossible now.
bool ReadWhole(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        out.append(buf, got);
    CloseHandle(h);
    return true;
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
        // A comment is decided by the FIRST non-blank character only. An inline
        // '#' is part of the value: paths and format strings contain them, and
        // a trailing-comment rule truncates those silently.
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
        table[section][key] = val;
    }
}

// Returns nullptr when absent, so callers can tell "not set" from "set to the
// same thing as the default".
const std::string* Find(const char* section, const char* key)
{
    std::string s = Lower(section ? section : "");
    std::string k = Lower(key ? key : "");
    Table::const_iterator si = g_table.find(s);
    if (si == g_table.end()) return nullptr;
    std::map<std::string, std::string>::const_iterator ki = si->second.find(k);
    if (ki == si->second.end()) return nullptr;
    return &ki->second;
}

}  // namespace

const std::string& CfgLoad(const wchar_t* cfgDir, const wchar_t* dataDir)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_table.clear();
    g_path.clear();

    const wchar_t* dirs[2] = { cfgDir, dataDir };
    for (int i = 0; i < 2; ++i) {
        if (!dirs[i] || !dirs[i][0]) continue;
        std::wstring p(dirs[i]);
        if (p[p.size() - 1] != L'\\' && p[p.size() - 1] != L'/') p += L'\\';
        p += L"tpf2mp.cfg";
        std::string text;
        if (!ReadWhole(p, text)) continue;
        Parse(text, g_table);
        char narrow[MAX_PATH * 2] = "";
        WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
        g_path = narrow;
        break;                                  // first file found wins
    }
    return g_path;
}

const std::string& CfgPath() { return g_path; }

bool CfgMergeFile(const wchar_t* path)
{
    if (!path || !path[0]) return false;
    std::string text;
    if (!ReadWhole(path, text)) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    Parse(text, g_table);                       // Parse assigns, so later wins
    return true;
}

const char* CfgStr(const char* section, const char* key, const char* def)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    const std::string* v = Find(section, key);
    return v ? v->c_str() : def;               // c_str() stays valid: the map owns it
}

int CfgInt(const char* section, const char* key, int def)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    const std::string* v = Find(section, key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    long n = strtol(v->c_str(), &end, 0);       // 0 => 0x.. hex also accepted
    if (end == v->c_str()) return def;          // nothing parsed
    while (*end == ' ' || *end == '\t') ++end;
    if (*end) return def;                       // trailing junk: not a number
    return (int)n;
}

bool CfgBool(const char* section, const char* key, bool def)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    const std::string* v = Find(section, key);
    if (!v) return def;
    std::string s = Lower(*v);
    if (s == "1" || s == "true"  || s == "yes" || s == "on")  return true;
    if (s == "0" || s == "false" || s == "no"  || s == "off") return false;
    return def;
}

bool CfgHasKey(const char* section, const char* key)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return Find(section, key) != nullptr;
}

}  // namespace tpf2mp
