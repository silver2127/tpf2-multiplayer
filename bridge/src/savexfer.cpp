#include "savexfer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#pragma comment(lib, "ws2_32.lib")

// wire framing: uint32 fileCount, then per file
//   uint32 nameLen, name bytes, uint64 dataLen, data bytes
static const uint32_t XFER_MAGIC = 0x56415354;   // 'TSAV'
static const size_t   CHUNK = 256 * 1024;

static XferLogFn g_log = nullptr;
static std::string g_dir, g_share;
static uint16_t g_port = 0;

static void LogX(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log("%s", buf);
}

static bool SendAll(SOCKET s, const char* p, size_t n)
{
    while (n) {
        int k = send(s, p, (int)(n > 0x10000 ? 0x10000 : n), 0);
        if (k <= 0) return false;
        p += k; n -= k;
    }
    return true;
}

static bool RecvAll(SOCKET s, char* p, size_t n)
{
    while (n) {
        int k = recv(s, p, (int)(n > 0x10000 ? 0x10000 : n), 0);
        if (k <= 0) return false;
        p += k; n -= k;
    }
    return true;
}

static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                        nullptr, nullptr);
    return s;
}

std::string Save_FindSaveDir()
{
    const wchar_t* roots[] = {
        L"C:\\Program Files (x86)\\Steam\\userdata",
        L"C:\\Program Files\\Steam\\userdata",
    };
    for (const wchar_t* root : roots) {
        std::wstring pat = std::wstring(root) + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;
            std::wstring cand = std::wstring(root) + L"\\" + fd.cFileName +
                                L"\\1066780\\local\\save";
            DWORD a = GetFileAttributesW(cand.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                FindClose(h);
                return Narrow(cand);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return "";
}

// newest *.sav in the directory, base name without extension
static std::string NewestSave(const std::string& dir)
{
    std::wstring pat = Widen(dir) + L"\\*.sav";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::wstring best;
    FILETIME bestT{};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &bestT) > 0) {
            bestT = fd.ftLastWriteTime;
            best = fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best.empty()) return "";
    size_t dot = best.find_last_of(L'.');
    if (dot != std::wstring::npos) best = best.substr(0, dot);
    return Narrow(best);
}

static bool ReadWhole(const std::wstring& path, std::vector<char>& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD want = (DWORD)((out.size() - done > CHUNK) ? CHUNK : out.size() - done);
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) {
            CloseHandle(h);
            return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

static bool WriteWhole(const std::wstring& path, const char* data, size_t n)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < n) {
        DWORD want = (DWORD)((n - done > CHUNK) ? CHUNK : n - done);
        DWORD put = 0;
        if (!WriteFile(h, data + done, want, &put, nullptr) || put == 0) {
            CloseHandle(h);
            return false;
        }
        done += put;
    }
    CloseHandle(h);
    return true;
}

static void ServeOne(SOCKET c)
{
    std::string base = g_share.empty() ? NewestSave(g_dir) : g_share;
    if (base.empty()) {
        LogX("[xfer] no save to share in %s\n", g_dir.c_str());
        uint32_t zero = 0;
        SendAll(c, (const char*)&XFER_MAGIC, 4);
        SendAll(c, (const char*)&zero, 4);
        return;
    }

    // .sav is the world; .sav.lua carries mod script state and must travel
    // with it; .jpg is only the load-menu thumbnail but is cheap
    const char* exts[] = { ".sav", ".sav.lua", ".jpg" };
    std::vector<std::pair<std::string, std::vector<char>>> files;
    for (const char* ext : exts) {
        std::string fn = base + ext;
        std::vector<char> buf;
        if (ReadWhole(Widen(g_dir + "\\" + fn), buf)) {
            LogX("[xfer] serving %s (%llu bytes)\n", fn.c_str(),
                 (unsigned long long)buf.size());
            files.emplace_back(fn, std::move(buf));
        }
    }

    uint32_t count = (uint32_t)files.size();
    if (!SendAll(c, (const char*)&XFER_MAGIC, 4)) return;
    if (!SendAll(c, (const char*)&count, 4)) return;
    for (auto& f : files) {
        uint32_t nl = (uint32_t)f.first.size();
        uint64_t dl = (uint64_t)f.second.size();
        if (!SendAll(c, (const char*)&nl, 4)) return;
        if (!SendAll(c, f.first.data(), nl)) return;
        if (!SendAll(c, (const char*)&dl, 8)) return;
        if (!SendAll(c, f.second.data(), (size_t)dl)) return;
    }
    LogX("[xfer] sent %u file(s) for '%s'\n", count, base.c_str());
}

static DWORD WINAPI ServerThread(LPVOID)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { LogX("[xfer] socket failed\n"); return 0; }
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(g_port);
    if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || listen(srv, 4) != 0) {
        LogX("[xfer] listen on %u FAILED (%d)\n", g_port, WSAGetLastError());
        closesocket(srv);
        return 0;
    }
    LogX("[xfer] serving saves on tcp/%u from %s\n", g_port, g_dir.c_str());
    for (;;) {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        LogX("[xfer] joiner connected\n");
        ServeOne(c);
        shutdown(c, SD_SEND);
        closesocket(c);
    }
    closesocket(srv);
    return 0;
}

void Save_StartServer(uint16_t port, const std::string& saveDir,
                      const std::string& shareName, XferLogFn log)
{
    g_log = log; g_dir = saveDir; g_share = shareName; g_port = port;
    if (g_dir.empty()) { LogX("[xfer] no save dir; server not started\n"); return; }
    CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
}

bool Save_Pull(const char* hostIp, uint16_t port, const std::string& saveDir,
               XferLogFn log)
{
    g_log = log;
    if (saveDir.empty()) { LogX("[xfer] no save dir; cannot pull\n"); return false; }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, hostIp, &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(s);
        return false;                    // host not up yet; caller retries
    }

    uint32_t magic = 0, count = 0;
    if (!RecvAll(s, (char*)&magic, 4) || magic != XFER_MAGIC) {
        LogX("[xfer] bad header from host\n");
        closesocket(s);
        return false;
    }
    if (!RecvAll(s, (char*)&count, 4)) { closesocket(s); return false; }
    if (count == 0) {
        LogX("[xfer] host has no save to share\n");
        closesocket(s);
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t nl = 0; uint64_t dl = 0;
        if (!RecvAll(s, (char*)&nl, 4) || nl == 0 || nl > 512) break;
        std::string name(nl, 0);
        if (!RecvAll(s, &name[0], nl)) break;
        if (!RecvAll(s, (char*)&dl, 8)) break;
        if (dl > (uint64_t)4 * 1024 * 1024 * 1024) break;   // sanity
        std::vector<char> buf((size_t)dl);
        if (dl && !RecvAll(s, buf.data(), (size_t)dl)) break;
        // prefix so a transfer can never clobber the joiner's own saves
        std::string out = "mp_host_" + name;
        if (WriteWhole(Widen(saveDir + "\\" + out), buf.data(), buf.size())) {
            LogX("[xfer] received %s -> %s (%llu bytes)\n", name.c_str(),
                 out.c_str(), (unsigned long long)dl);
        } else {
            LogX("[xfer] FAILED writing %s\n", out.c_str());
        }
    }
    closesocket(s);
    LogX("[xfer] pull complete\n");
    return true;
}
