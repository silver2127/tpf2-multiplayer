// panel_linux.cpp -- the Multiplayer panel's state, pages and input on Linux
// (menu_hook.cpp: "multiplayer panel state", "the Multiplayer window",
// RenderPanelLayer, OnHit, LlKeyboard, "the public game list").
//
// Two pages, as on Windows: HOST / JOIN, with the public game list under the
// fields, and the LOBBY: players with their company chips, the chat, LEAVE,
// START GAME and the mods question. The lobby itself -- its process, events and
// files -- is lobby_linux.cpp; this file draws its View and turns clicks and
// typing into its calls.
//
// INPUT comes from the game's own SDL2, not from Win32 polling and a low-level
// keyboard hook: an SDL event filter sees every event before the game does and
// can consume it. So a click on the panel no longer also clicks the menu under
// it, and typing arrives as SDL_TEXTINPUT, already composed for the player's
// keyboard layout. While the lobby page is up the chat takes every key, as the
// Windows keyboard hook did, until the shared save is placed.
//
// LOCKS. The filter runs on the UI thread, the overlay on the render thread and
// the lobby on its own threads, so the panel's state is behind one mutex, taken
// before the lobby's (lobby_linux.h). The clipboard is used with it released:
// the game ships libSDL2 2.25.0, whose X11 GetClipboardText waits for the
// clipboard's owner by pumping events (release-2.24.0 and release-2.26.0
// src/video/x11/SDL_x11clipboard.c: `while (videodata->selection_waiting) {
// SDL_PumpEvents(); ...`), and SDL_PushEvent calls this filter from inside that
// pump (SDL_events.c: SDL_EventOK.callback under the recursive
// SDL_event_watchers_lock). With the panel's mutex held that re-entry would
// deadlock the UI thread. Events that arrive during a clipboard call pass
// straight to the game. Clipboard calls are made only from the filter, for
// window and input events: the thread that pumps the window's events. For the
// same lock order, SDL_SetEventFilter (InstallInput) runs unlocked, and so does
// everything panel::Init hands to other code (lobby::Init, which asks the
// menu-game area for its save folder).
//
// Strings and the View live in a leaked struct (P()): the render thread may
// still draw while exit() runs static destructors.
#include "panel.h"
#include "panel_layer.h"
#include "lobby_linux.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <dlfcn.h>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace panel {

using layer::Rgb;
using layer::rgb;

// ---- state ------------------------------------------------------------------------
struct PanelState {
    std::string dataDir;
    std::string joinCode, passCode, username, lobbyName, chatInput;
    std::string status;
    bool userAuto = true;
    std::string steamName;
    uint64_t steamNext = 0;
    std::string flagMaster = "https://srv1306562.hstgr.cloud/tpf2mp";   // menu_hook.cpp g_flagMaster
    lobby::View view;
    std::vector<lobby::PubRow> pubRows;
    std::string pubNote;
};
static PanelState& P()
{
    static PanelState* p = new PanelState;
    return *p;
}

static std::mutex g_mtx;
static Tpf2mpLogFn g_log = nullptr;

static int  g_uiState = 0;          // 0 closed, 1 host / join, 2 lobby
static bool g_pageHidden = false;   // a full-screen menu page covers the title menu
static bool g_dirty = true;
static std::atomic<bool> g_asyncDirty{false};   // the lobby's threads changed something drawn
static uint64_t g_lastRenderMs = 0;
static uint64_t g_lastFrameMs = 0;   // the overlay last drew us; input only counts while it does

static int g_px = 0, g_py = 0, g_pw = 0, g_ph = 0;   // panel rect on the screen
static float g_s = 1.f;                              // UI scale

// id: 2 HOST, 3 JOIN, 4 close, 5 LEAVE (and the lobby's close), 6 START GAME,
// 7 copy code, 8 code field, 9 chat field, 10 password, 11 PUBLIC, 12 REFRESH,
// 13 player name, 14 lobby name, 15 OPEN LOGS, 16 YES, 17 NO, 20..35 company
// chips, 40..47 public games (menu_hook.cpp struct Hit).
struct Hit { int x, y, w, h, id; bool btn; };
static Hit g_hits[64];
static int g_hitCount = 0;
static int g_hover = 0, g_pressed = 0;

static int  g_focus = 0;            // 1 code, 2 password, 3 player name, 4 lobby name
static bool g_public = false;
static bool g_separateCompanies = false;
static bool g_capturedRight = false;
static bool g_dashShown = true;

static float g_flagScale = 0.f;
static int   g_flagRelayAutosaveMin = 2;
static int   g_flagShareMods = 0;   // share_mods: 0 ask, 1 always, 2 never
static bool  g_flagAutoLoad = true;
static bool  g_fontsOk = false;     // set once Init is done: the panel shows, and takes clicks, from then on
static bool  g_initDone = false;

static const int ROSTER_ROWS = 16;  // rows the lobby page shows; the rest is "+N more"

static const Rgb MW_BG   = rgb(5, 25, 40);   // MenuWindow backgroundColor
static const int MW_BG_A = 190;
static const Rgb MW_TEXT = rgb(255, 255, 255);
static const Rgb MW_DIM  = rgb(190, 205, 218);
static const Rgb MW_YOU  = rgb(150, 210, 170);

static uint64_t NowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int S(float v) { return (int)(v * g_s + 0.5f); }
static float UiScale(int screenH) { return g_flagScale > 0.f ? g_flagScale : (screenH > 0 ? screenH / 1080.f : 1.f); }

// ---- names (menu_hook.cpp: LoadNames, SaveNames, ensureUsername) --------------------
static const char* const NAME_ADJ[] = {
    "Brave","Calm","Clever","Crisp","Daring","Eager","Fancy","Fuzzy","Gentle","Giant",
    "Golden","Happy","Hasty","Icy","Jolly","Keen","Lucky","Merry","Mighty","Nimble",
    "Noble","Odd","Plucky","Proud","Quick","Quiet","Rapid","Rusty","Shiny","Silent",
    "Sleepy","Sly","Snowy","Solar","Spicy","Steady","Stormy","Swift","Tidy","Witty",
    "Zesty","Amber","Copper","Dusty","Frosty","Misty","Rosy","Sunny","Velvet","Wild" };
static const char* const NAME_NOUN[] = {
    "Otter","Badger","Falcon","Heron","Lynx","Moose","Panda","Raven","Tiger","Walrus",
    "Beaver","Bison","Camel","Dingo","Ferret","Gecko","Ibis","Jaguar","Koala","Lemur",
    "Marmot","Newt","Ocelot","Puffin","Quail","Rabbit","Salmon","Toucan","Urchin","Viper",
    "Wombat","Yak","Zebra","Engine","Signal","Depot","Tender","Boxcar","Caboose","Tram",
    "Ferry","Barge","Trolley","Wagon","Piston","Rail","Switch","Girder","Trestle","Viaduct" };

static void EnsureUsernameLocked()
{
    if (!P().username.empty()) return;
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned s = (unsigned)ts.tv_nsec ^ ((unsigned)getpid() * 2654435761u) ^ (unsigned)ts.tv_sec;
    s = s * 1103515245u + 12345u; const unsigned a = (s >> 8) % (sizeof(NAME_ADJ) / sizeof(NAME_ADJ[0]));
    s = s * 1103515245u + 12345u; const unsigned n = (s >> 8) % (sizeof(NAME_NOUN) / sizeof(NAME_NOUN[0]));
    P().username = std::string(NAME_ADJ[a]) + NAME_NOUN[n];
    if (g_log) g_log("[panel] username: %s\n", P().username.c_str());
}

static void SaveNamesLocked()
{
    FILE* f = fopen((P().dataDir + "tpf2_names.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "player=%s\nlobby=%s\nauto=%d\n", P().username.c_str(), P().lobbyName.c_str(), P().userAuto ? 1 : 0);
    fclose(f);
}

static void LoadNamesLocked()
{
    FILE* f = fopen((P().dataDir + "tpf2_names.txt").c_str(), "r");
    bool sawAuto = false, sawPlayer = false;
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char* e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char* v = eq + 1;
            if (!strcmp(line, "player") && v[0]) { P().username.assign(v, strnlen(v, 127)); sawPlayer = true; }
            else if (!strcmp(line, "lobby")) P().lobbyName.assign(v, strnlen(v, 127));
            else if (!strcmp(line, "auto")) { sawAuto = true; P().userAuto = v[0] == '1'; }
        }
        fclose(f);
    }
    if (!sawAuto) P().userAuto = !sawPlayer;
    EnsureUsernameLocked();
    if (!f) SaveNamesLocked();   // first run: keep the random name from now on
}

// Use only the game's already loaded Steam API. SysV C exports verified in
// the shipped libsteam_api.so; no Steam initialization or interface vtable guesses.
static void SteamNameTickLocked()
{
    const uint64_t now = NowMs();
    if (now < P().steamNext) return;
    P().steamNext = now + (P().steamName.empty() ? 2000 : 30000);
    void* h = dlopen("libsteam_api.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return;
    auto user = reinterpret_cast<int (*)()>(dlsym(h, "SteamAPI_GetHSteamUser"));
    auto friends = reinterpret_cast<void* (*)()>(dlsym(h, "SteamAPI_SteamFriends_v017"));
    auto persona = reinterpret_cast<const char* (*)(void*)>(dlsym(h, "SteamAPI_ISteamFriends_GetPersonaName"));
    std::string name;
    if (user && friends && persona && user()) {
        void* f = friends();
        const char* n = f ? persona(f) : nullptr;
        bool space = false;
        for (size_t i = 0; n && n[i] && name.size() < 127; ++i) {
            unsigned char c = n[i];
            if (c < 32 || c == '"' || c == '\\' || c == '#') continue;
            if (c == ' ') { space = !name.empty(); continue; }
            if (space) { name += ' '; space = false; }
            if (name.size() < 127) name += char(c);
        }
        // Do not retain a partial UTF-8 codepoint at the byte limit.
        if (n && n[0] && name.size() == 127) {
            size_t start = name.size() - 1;
            while (start && (static_cast<unsigned char>(name[start]) & 0xc0) == 0x80) --start;
            unsigned char c = name[start];
            size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (name.size() - start < width) name.resize(start);
        }
    }
    dlclose(h);
    if (name.empty()) return;
    P().steamName = name;
    if (!P().userAuto || g_focus == 3 || g_uiState >= 2 || P().username == name) return;
    P().username = name;
    SaveNamesLocked();
    g_dirty = true;
}

static void WriteDashFlagLocked()
{
    FILE* f = fopen((P().dataDir + "tpf2mp_dash.txt").c_str(), "w");
    if (!f) return;
    fputs(g_dashShown ? "1" : "0", f);
    fclose(f);
}

// ---- flags (menu_hook.cpp: ReadFlags) -------------------------------------------------
// Every value is checked: a garbled line leaves that setting at its default.
// slot= is menu_linux.cpp's; automod= is read where the mod list is edited.
static void ReadFlags(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { if (g_log) g_log("[panel] flags: no %s (defaults)\n", path.c_str()); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* v = eq + 1;
        char* e = v + strlen(v);
        while (e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        const bool digit = v[0] >= '0' && v[0] <= '9';
        if (!strcmp(line, "scale")) {
            // "1.25" by hand: the game sets the locale, and atof would follow it
            float s = 0, frac = 0.1f;
            bool point = false, bad = !digit;
            for (const char* q = v; *q && !bad; q++) {
                if (*q == '.' && !point) point = true;
                else if (*q >= '0' && *q <= '9') { if (point) { s += (*q - '0') * frac; frac *= 0.1f; } else s = s * 10 + (*q - '0'); }
                else bad = true;
            }
            g_flagScale = (!bad && s >= 0.5f && s <= 3.0f) ? s : 0.f;
        } else if (!strcmp(line, "relay_autosave_min")) {
            const int m = atoi(v);
            if (digit && m >= 0 && m <= 60) g_flagRelayAutosaveMin = m;
        } else if (!strcmp(line, "share_mods")) {
            g_flagShareMods = !strcmp(v, "always") ? 1 : !strcmp(v, "never") ? 2 : 0;
        } else if (!strcmp(line, "autoload")) {
            if (!strcmp(v, "0")) g_flagAutoLoad = false; else if (!strcmp(v, "1")) g_flagAutoLoad = true;
        } else if (!strcmp(line, "master_url")) {
            while (e > v && e[-1] == '/') *--e = 0;
            // an argument of the lobby: a plain http(s) URL, no blanks or quotes
            const bool ok = !v[0] || ((!strncmp(v, "https://", 8) || !strncmp(v, "http://", 7)) && !strpbrk(v, " \t\"'"));
            if (ok) P().flagMaster = v;
            else if (g_log) g_log("[panel] flags: master_url ignored (not a plain http(s) URL)\n");
        }
    }
    fclose(f);
    if (g_log) g_log("[panel] flags: scale=%d%% autoload=%d relay_autosave_min=%d share_mods=%d master_url=%s\n",
                     (int)(g_flagScale * 100 + 0.5f), g_flagAutoLoad ? 1 : 0, g_flagRelayAutosaveMin, g_flagShareMods,
                     P().flagMaster.empty() ? "(none)" : P().flagMaster.c_str());
}

// ---- widgets (menu_hook.cpp: mw*) --------------------------------------------------------
static void AddHit(int x, int y, int w, int h, int id, bool btn = false)
{
    if (g_hitCount < 64) g_hits[g_hitCount++] = Hit{ x, y, w, h, id, btn };
}

static void MwButton(int x, int y, int w, int h, const char* label, int id)
{
    layer::Text(x, y, w, h, label, S(13), MW_TEXT, layer::kCenter | layer::kVCenter);
    AddHit(x, y, w, h, id, true);
}
static int MwButtonW(const char* label) { return layer::TextWidth(label, S(13)) + S(2 * 10); }
static void MwHeader(int x, int y, int w, const char* text)
{
    layer::Text(x, y, w, S(22), text, S(13), MW_DIM, layer::kLeft | layer::kVCenter | layer::kEndEllipsis);
}
static void MwBody(int x, int y, int w, int h, const char* text, Rgb c = MW_TEXT)
{
    layer::Text(x, y, w, h, text, S(13), c, layer::kLeft | layer::kWordBreak);
}
static void MwCheck(int x, int y, const char* label, bool on, int id)
{
    const int sz = S(16);
    layer::Rect(x, y + S(7), sz, sz, rgb(0, 0, 0), 60);
    layer::Rect(x, y + S(7), sz, 1, MW_TEXT, 90);
    layer::Rect(x, y + S(7) + sz - 1, sz, 1, MW_TEXT, 90);
    layer::Rect(x, y + S(7), 1, sz, MW_TEXT, 90);
    layer::Rect(x + sz - 1, y + S(7), 1, sz, MW_TEXT, 90);
    if (on) layer::Text(x, y + S(5), sz, sz + S(4), "\xE2\x9C\x93", S(13), MW_TEXT, layer::kCenter | layer::kVCenter);
    layer::Text(x + sz + S(8), y, S(360), S(30), label, S(13), MW_TEXT, layer::kLeft | layer::kVCenter);
    AddHit(x, y, sz + S(8) + layer::TextWidth(label, S(13)), S(30), id, true);
}
// TextInputField: black@50, padding {5,10}; caret while focused. A focused field
// whose text is wider than the box shows its end, where the typing happens.
static void MwField(int x, int y, int w, int h, const std::string& text, bool focused, const char* placeholder, int id)
{
    layer::Rect(x, y, w, h, rgb(0, 0, 0), focused ? 90 : 50);
    std::string shown;
    if (!text.empty() || focused) {
        shown = text;
        const int room = w - S(20) - layer::TextWidth("|", S(13));
        size_t cut = 0;
        while (focused && cut < shown.size() && layer::TextWidth(shown.c_str() + cut, S(13)) > room) {
            cut++;
            while (cut < shown.size() && ((unsigned char)shown[cut] & 0xC0) == 0x80) cut++;
        }
        shown = shown.substr(cut) + ((focused && (NowMs() / 500) % 2 == 0) ? "|" : "");
    } else {
        shown = placeholder;
    }
    layer::Text(x + S(10), y, w - S(20), h, shown.c_str(), S(13), text.empty() ? MW_DIM : MW_TEXT,
                layer::kLeft | layer::kVCenter, text.empty() ? 160 : 255);
    AddHit(x, y, w, h, id);
}
static void MwClose(int w, int id)
{
    const int sz = S(32), x = w - S(25) - sz + S(8), y = S(8);
    layer::Text(x, y, sz, sz, "\xC3\x97", S(20), MW_TEXT, layer::kCenter | layer::kVCenter);
    AddHit(x, y, sz, sz, id, true);
}
static void MwTitle(const char* t, int w = 400)
{
    layer::Text(S(25), S(8), S((float)w), S(32), t, S(18), MW_TEXT, layer::kLeft | layer::kVCenter | layer::kEndEllipsis);
}
static void MwStatus(int w, int h)
{
    layer::Text(S(25), h - S(34), w - S(50), S(24), P().status.c_str(), S(12), MW_DIM, layer::kLeft | layer::kVCenter | layer::kEndEllipsis);
}

// Company chips: 20 Trubetskoy colours, then the shared golden-angle hue walk.
static Rgb CoColor(int cid)
{
    static const Rgb first[20] = { rgb(230,25,75), rgb(0,130,200), rgb(60,180,75), rgb(245,130,48), rgb(145,30,180), rgb(70,240,240), rgb(240,50,230), rgb(255,225,25), rgb(0,128,128), rgb(170,110,40), rgb(210,245,60), rgb(128,0,0), rgb(0,0,128), rgb(128,128,0), rgb(250,190,212), rgb(220,190,255), rgb(170,255,195), rgb(255,215,180), rgb(128,128,128), rgb(255,250,200) };
    if (cid >= 1 && cid <= 20) return first[cid - 1];
    const float h = (float)(((cid - 21) * 137.508) - (int)(((cid - 21) * 137.508) / 360.0) * 360.0);
    const float sat = 0.62f, val = 0.85f, c = val * sat, x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f)), m = val - c;
    float r, g, b;
    if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; } else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; } else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
    return rgb((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}

// ---- LOBBY (menu_hook.cpp RenderPanelLayer, g_uiState == 2) ----------------------------------
static void RenderLobbyLocked(int w, int h)
{
    const lobby::View& v = P().view;
    const int pad = S(25), cy = S(56);
    const std::string title = v.title.empty() ? std::string("LOBBY") : "LOBBY  --  " + v.title;
    MwTitle(title.c_str(), 300);
    int titleW = layer::TextWidth(title.c_str(), S(18)) + S(16);
    if (titleW > S(316)) titleW = S(316);
    MwClose(w, 5);
    if (v.haveCode) {
        // ROOM CODE, DELIBERATELY NOT RENDERED (menu_hook.cpp): the code is the
        // credential, and the clipboard is the only way it leaves. A fixed
        // placeholder, so not even its length shows.
        static const char kPlaceholder[] = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2  ROOM CODE  \xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
        const int cw = layer::TextWidth(kPlaceholder, S(14)) + S(20), cx = S(25) + titleW;
        layer::Rect(cx, S(11), cw, S(26), rgb(0, 0, 0), 50);
        layer::Text(cx + S(10), S(11), cw, S(26), kPlaceholder, S(14), MW_TEXT, layer::kLeft | layer::kVCenter);
        layer::Text(cx + cw + S(10), S(11), S(160), S(26), "click to copy (never shown)", S(11), MW_DIM, layer::kLeft | layer::kVCenter, 180);
        AddHit(cx, S(11), cw, S(26), 7, true);
    }
    const int bottom = h - S(44);
    const int listW = S(220), chatX = pad + listW + S(20), chatW = w - chatX - pad;
    const int contentH = bottom - cy - S(12);

    // players, each with a company chip: click your own (the host: anyone's) to cycle it
    const int n = (int)v.players.size();
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "PLAYERS (%d)", n);
    MwHeader(pad, cy, listW, hdr);
    int rowsMax = (bottom - S(6) - (cy + S(30))) / S(26);
    if (rowsMax > ROSTER_ROWS) rowsMax = ROSTER_ROWS;
    if (rowsMax < 1) rowsMax = 1;
    const int rows = n < rowsMax ? n : rowsMax;
    for (int i = 0; i < rows; i++) {
        const lobby::Player& p = v.players[(size_t)i];
        const int ry = cy + S(30) + i * S(26);
        layer::Rect(pad, ry + S(4), S(22), S(16), CoColor(p.company), 220);
        char num[8];
        snprintf(num, sizeof(num), "%d", p.company);
        layer::Text(pad, ry + S(4), S(22), S(16), num, S(11), rgb(0, 0, 0), layer::kCenter | layer::kVCenter);
        if (p.you || v.youAreHost) AddHit(pad, ry + S(2), S(24), S(20), 20 + i, true);
        layer::Text(pad + S(30), ry, listW - S(80), S(24), (p.stage.empty() ? p.name : p.name + "  (" + p.stage + ")").c_str(), S(14), p.you ? MW_YOU : MW_TEXT,
                    layer::kLeft | layer::kVCenter | layer::kEndEllipsis);
        if (p.host) layer::Text(pad + listW - S(50), ry, S(50), S(24), "HOST", S(11), MW_DIM, layer::kRight | layer::kVCenter, 180);
    }
    if (n > rowsMax) {
        char more[48];
        snprintf(more, sizeof(more), "+ %d more", n - rowsMax);
        layer::Text(pad + S(30), cy + S(30) + rowsMax * S(26), listW, S(20), more, S(11), MW_DIM, layer::kLeft | layer::kVCenter, 180);
    }
    int legendY = cy + S(30) + (rows < 8 ? 8 : rows) * S(26) + S(6) + (n > rowsMax ? S(22) : 0);
    if (legendY > bottom - S(44)) legendY = bottom - S(44);
    layer::Text(pad, legendY, listW, S(40),
                P().view.separateCompanies
                    ? "Separate companies: each player runs their own. Left-click a chip for the next company, right-click for the previous."
                    : "Co-op: everyone runs company 1 together. Left-click a chip for the next company, right-click for the previous.",
                S(11), MW_DIM, layer::kLeft | layer::kWordBreak, 170);

    // chat
    const int inH = S(30), logH = contentH - inH - S(8);
    layer::Rect(chatX, cy, chatW, logH, rgb(0, 0, 0), 50);
    const int lh = S(22), maxLines = (logH - S(16)) / lh, cnt = (int)v.chat.size();
    int ly = cy + S(8);
    for (int i = cnt > maxLines ? cnt - maxLines : 0; i < cnt; i++) {
        layer::Text(chatX + S(10), ly, chatW - S(20), lh, v.chat[(size_t)i].c_str(), S(13), MW_TEXT,
                    layer::kLeft | layer::kVCenter | layer::kEndEllipsis);
        ly += lh;
    }
    MwField(chatX, cy + logH + S(8), chatW, inH, P().chatInput, !v.lobbyDone, "Type a message and press Enter", 9);

    // LEAVE left, START GAME right (the host), the status or the mods question between
    const int bw1 = MwButtonW("LEAVE");
    MwButton(pad, bottom, bw1, S(30), "LEAVE", 5);
    int rightCut = S(160);
    if (v.isHost) {
        const int bw2 = MwButtonW("START GAME");
        MwButton(w - pad - bw2, bottom, bw2, S(30), "START GAME", 6);
        if (!P().flagMaster.empty()) MwCheck(w - pad - bw2 - S(110), bottom, "PUBLIC", g_public, 11);
        MwCheck(w - pad - bw2 - S(340), bottom, "SEPARATE COMPANIES", P().view.separateCompanies, 50);
        const int need = bw2 + S(360);
        if (need > rightCut) rightCut = need;
    }
    const char* st = P().status.c_str();
    Rgb stColor = MW_DIM;
    if (!v.modsPrompt.empty()) {
        st = v.modsPrompt.c_str();
        stColor = MW_TEXT;
        const int bwN = MwButtonW("NO"), bwY = MwButtonW("YES");
        MwButton(w - pad - bwN, bottom, bwN, S(30), "NO", 17);
        MwButton(w - pad - bwN - S(8) - bwY, bottom, bwY, S(30), "YES", 16);
        rightCut = bwN + bwY + S(30);
    }
    layer::Text(pad + bw1 + S(20), bottom, w - 2 * pad - bw1 - rightCut, S(30), st, S(12), stColor,
                layer::kLeft | layer::kVCenter | layer::kEndEllipsis);
}

// ---- HOST / JOIN (menu_hook.cpp RenderPanelLayer, the else branch) -----------------------------------
static void RenderHostJoinLocked(int w, int h)
{
    const int pad = S(25), cy = S(56);
    const bool master = !P().flagMaster.empty();
    MwTitle("MULTIPLAYER");
    MwClose(w, 4);
    { const int lb = MwButtonW("OPEN LOGS"); MwButton(w - S(65) - lb, S(10), lb, S(28), "OPEN LOGS", 15); }
    const int colW = (w - 2 * pad - S(40)) / 2, lx = pad, rx = pad + colW + S(40);
    layer::Rect(pad + colW + S(20), cy, 1, S(130), MW_TEXT, 40);
    MwHeader(lx, cy, colW, "HOST A GAME");
    MwBody(lx, cy + S(24), colW, S(36), "Opens a lobby and shares your newest save with everyone who joins.");
    {
        // not EnsureUsername here: an emptied name field would roll a new name
        // on the next frame, before anything could be typed
        const std::string def = P().username.empty() ? std::string("Your game  (click to name the lobby)")
                                                     : P().username + "'s game  (click to name the lobby)";
        MwField(lx, cy + S(60), colW, S(30), P().lobbyName, g_focus == 4, def.c_str(), 14);
    }
    {
        const int hb = MwButtonW("HOST GAME");
        MwButton(lx, cy + S(96), hb, S(30), "HOST GAME", 2);
        if (master) MwCheck(lx + hb + S(16), cy + S(96), "PUBLIC (listed in the browser)", g_public, 11);
        MwCheck(lx, cy + S(128), "SEPARATE COMPANIES (off = co-op)", g_separateCompanies, 50);
    }
    MwHeader(rx, cy, colW, "JOIN A GAME");
    MwBody(rx, cy + S(28), colW, S(24), "Paste or type the code from your host.");
    MwField(rx, cy + S(58), colW, S(30), P().joinCode, g_focus == 1, "Click to paste the code", 8);
    MwButton(rx, cy + S(96), MwButtonW("JOIN GAME"), S(30), "JOIN GAME", 3);
    MwBody(rx, cy + S(134), colW, S(20),
           "The shared save must have the Multiplayer mod enabled.");
    MwHeader(pad, cy + S(192), S(260), "YOUR NAME");
    MwField(pad, cy + S(216), S(260), S(30), P().username, g_focus == 3, "Steam name (click to type your own)", 13);
    MwHeader(pad + S(290), cy + S(192), w - 2 * pad - S(290),
             "PASSWORD  --  optional; anyone who has the code can read your IP address");
    MwField(pad + S(290), cy + S(216), S(260), S(30), std::string(P().passCode.size(), '*'), g_focus == 2,
            "Click to type a password", 10);

    if (master) {
        // PUBLIC GAMES: the server browser. A click drops the row's code into the join field.
        int ly = cy + S(260);
        const int lw = w - 2 * pad;
        MwHeader(pad, ly, lw - S(120), "PUBLIC GAMES  --  click a row, then JOIN GAME");
        { const int rb = MwButtonW("REFRESH"); MwButton(w - pad - rb, ly - S(4), rb, S(30), "REFRESH", 12); }
        ly += S(26);
        const int cName = pad + S(10), cType = pad + S(395), cPl = pad + S(520), cVer = pad + S(600), cAge = pad + S(670);
        const unsigned one = layer::kLeft | layer::kVCenter;
        layer::Text(cName, ly, S(375), S(20), "HOST", S(13), MW_DIM, one);
        layer::Text(cType, ly, S(120), S(20), "TYPE", S(13), MW_DIM, one);
        layer::Text(cPl, ly, S(70), S(20), "PLAYERS", S(13), MW_DIM, one);
        layer::Text(cVer, ly, S(60), S(20), "VERSION", S(13), MW_DIM, one);
        layer::Text(cAge, ly, S(80), S(20), "SEEN", S(13), MW_DIM, one);
        ly += S(22);
        const int rh = S(24);
        int maxRows = (h - S(40) - ly) / (rh + S(2));
        if (maxRows > 8) maxRows = 8;
        const std::vector<lobby::PubRow>& rowsV = P().pubRows;
        for (int i = 0; i < (int)rowsV.size() && i < maxRows; i++) {
            const lobby::PubRow& r = rowsV[(size_t)i];
            layer::Rect(pad, ly, lw, rh, rgb(0, 0, 0), (i & 1) ? 35 : 55);
            // a master from before the type field: the relay is known by its game string
            const char* type = r.type == "relay" ? "dedicated server" : r.type == "host" ? "player hosted"
                             : r.game == "dedicated relay" ? "dedicated server" : "player hosted";
            const std::string name = r.locked ? r.name + "  [locked]" : r.name;
            char pl[32], age[32];
            snprintf(pl, sizeof(pl), "%d / %d", r.players, r.max);
            if (r.age < 60) snprintf(age, sizeof(age), "just now");
            else snprintf(age, sizeof(age), "%d min ago", r.age / 60);
            layer::Text(cName, ly, S(375), rh, name.c_str(), S(13), MW_TEXT, one | layer::kEndEllipsis);
            layer::Text(cType, ly, S(120), rh, type, S(13), MW_DIM, one | layer::kEndEllipsis);
            layer::Text(cPl, ly, S(70), rh, pl, S(13), MW_TEXT, one);
            layer::Text(cVer, ly, S(60), rh, r.version.c_str(), S(13), MW_DIM, one | layer::kEndEllipsis);
            layer::Text(cAge, ly, S(80), rh, age, S(13), MW_DIM, one);
            AddHit(pad, ly, lw, rh, 40 + i, true);
            ly += rh + S(2);
        }
        if (rowsV.empty())
            layer::Text(cName, ly, lw - S(20), S(24), P().pubNote.empty() ? "Looking for public games\xE2\x80\xA6" : P().pubNote.c_str(),
                        S(13), MW_DIM, one | layer::kEndEllipsis);
    }
    MwStatus(w, h);
}

static void RenderLocked(int w, int h)
{
    layer::Begin(w, h);
    g_hitCount = 0;
    layer::Rect(0, 0, w, h, MW_BG, MW_BG_A);
    if (g_uiState == 2) RenderLobbyLocked(w, h);
    else RenderHostJoinLocked(w, h);
}

static void LayoutLocked(int screenW, int screenH, int* w, int* h)
{
    g_s = UiScale(screenH);
    *w = S(780);
    *h = (g_uiState == 2 || !P().flagMaster.empty()) ? S(540) : S(330);
    if (*w > screenW) *w = screenW;
    if (*h > screenH) *h = screenH;
}

// ---- actions (menu_hook.cpp: OnHit, StartLobby) --------------------------------------------------
static void SetStatusLocked(const std::string& s)
{
    if (s == P().status) return;
    // Progress lines ("Receiving save... 42%") change often: logged where they
    // start only. Told by shape -- both end in digits and '%' and agree before
    // them -- so lines that merely begin alike ("Save ready -- loading it...",
    // then the autoload's "Save ready -- open LOAD GAME ...") are all logged.
    auto stem = [](const std::string& x) {
        if (x.empty() || x.back() != '%') return std::string();
        size_t e = x.size() - 1;
        while (e > 0 && x[e - 1] >= '0' && x[e - 1] <= '9') e--;
        return e + 1 < x.size() ? x.substr(0, e) : std::string();   // at least one digit
    };
    const std::string was = stem(P().status), now = stem(s);
    const bool progress = !was.empty() && was == now;
    if (g_log && !progress) g_log("[panel] status: %s\n", s.c_str());
    P().status = s;
    g_dirty = true;
}

// What a click or key asks for once the panel's lock is released (see LOCKS).
struct Post {
    bool pasteCode = false;           // the clipboard into the code field
    bool joinFromClipboard = false;   // JOIN with the clipboard's code
    std::string copy;                 // onto the clipboard
    std::string copied;               // the status once it is there
};

static void StartLobbyLocked(bool join, const std::string& clip)
{
    lobby::StartRequest r;
    r.join = join;
    r.name = P().username;
    r.password = P().passCode;
    r.lobbyName = P().lobbyName.empty() ? P().username + "'s game" : P().lobbyName;
    r.pub = g_public;
    r.separateCompanies = g_separateCompanies;
    if (join) r.code = P().joinCode.size() >= 8 ? P().joinCode : clip;
    g_focus = 0;
    std::string why;
    if (!lobby::Start(r, &why)) { SetStatusLocked(why); return; }
    g_uiState = 2;
    P().chatInput.clear();
    P().view = lobby::View();
    SetStatusLocked(join ? "Joining lobby\xE2\x80\xA6" : "Starting lobby\xE2\x80\xA6");
}

static void AppendCodeLocked(const std::string& text)
{
    for (unsigned char c : text)
        if (c > 32 && P().joinCode.size() < 200) P().joinCode.push_back((char)c);
}

static void OnHitLocked(int id, Post* post, bool previous = false)
{
    if (previous && !(id >= 20 && id < 36)) return;
    if (g_log) g_log("[panel] hit id=%d\n", id);
    switch (id) {
        case 16: case 17: SetStatusLocked(lobby::AnswerMods(id == 16)); break;
        case 15:
            // the gathering's own verdict comes after this line: its status waits for this lock
            SetStatusLocked(lobby::OpenLogs() ? "Gathering logs..." : "Couldn't start gathering the logs -- see tpf2_menu.log");
            break;
        case 4:  g_uiState = 0; g_focus = 0; break;
        case 2:  EnsureUsernameLocked(); SaveNamesLocked(); StartLobbyLocked(false, std::string()); break;
        case 3:
            EnsureUsernameLocked(); SaveNamesLocked();
            if (P().joinCode.size() >= 8) StartLobbyLocked(true, std::string());
            else post->joinFromClipboard = true;   // menu_hook.cpp: a short field takes the clipboard's code
            break;
        case 5:  lobby::Leave(); g_uiState = 1; break;
        case 6: { const std::string s = lobby::StartGame(); if (!s.empty()) SetStatusLocked(s); break; }
        case 7: {
            std::string code;
            if (lobby::CopyCode(&code)) { post->copy = code; post->copied = "Code copied to clipboard \xE2\x80\x94 share it in Discord."; }
            break;
        }
        case 8:  g_focus = 1; if (P().joinCode.empty()) post->pasteCode = true; else P().joinCode.clear(); break;
        case 9:  break;   // the chat field is always focused in the lobby
        case 10: g_focus = 2; break;
        case 13: g_focus = 3; break;
        case 14: g_focus = 4; break;
        case 11:
            g_public = !g_public;
            if (g_uiState == 2) SetStatusLocked(lobby::SetPublic(g_public));
            else SetStatusLocked(g_public ? "Your game will be listed publicly when you host." : "Your game will not be listed.");
            break;
        case 50:
            if (g_uiState == 2) SetStatusLocked(lobby::SetSeparateCompanies(!P().view.separateCompanies));
            else {
                g_separateCompanies = !g_separateCompanies;
                SetStatusLocked(g_separateCompanies ? "Players will each get their own company." : "Players will share one company.");
            }
            break;
        case 12: lobby::PublicRefresh(); SetStatusLocked("Refreshing the public game list\xE2\x80\xA6"); break;
        default:
            if (id >= 40 && id < 48) {
                lobby::PubRow r;
                if (lobby::PublicRow(id - 40, &r)) {
                    P().joinCode = r.code;
                    g_focus = 1;
                    SetStatusLocked(r.locked ? r.name + "'s game needs its password: type it below, then JOIN GAME."
                                             : r.name + "'s code is filled in -- press JOIN GAME.");
                }
            } else if (id >= 20 && id < 36) {
                lobby::CycleCompany(id - 20, previous);
            }
            break;
    }
    g_dirty = true;
}

// ---- SDL -----------------------------------------------------------------------------------
static decltype(&SDL_SetEventFilter)         s_SetEventFilter = nullptr;
static decltype(&SDL_GetEventFilter)         s_GetEventFilter = nullptr;
static decltype(&SDL_GetWindowFromID)        s_GetWindowFromID = nullptr;
static decltype(&SDL_GetWindowSize)          s_GetWindowSize = nullptr;
static decltype(&SDL_Vulkan_GetDrawableSize) s_GetDrawableSize = nullptr;
static decltype(&SDL_GetClipboardText)       s_GetClipboardText = nullptr;
static decltype(&SDL_SetClipboardText)       s_SetClipboardText = nullptr;
static decltype(&SDL_free)                   s_free = nullptr;
static SDL_EventFilter g_prevFilter = nullptr;
static void* g_prevUserdata = nullptr;
static bool g_inputInstalled = false;
static thread_local bool t_inClipboard = false;

// Window coordinates -> swapchain pixels (the same thing unless the desktop scales the window).
static void ToPixels(uint32_t windowID, int x, int y, int* px, int* py)
{
    *px = x; *py = y;
    SDL_Window* win = s_GetWindowFromID ? s_GetWindowFromID(windowID) : nullptr;
    if (!win || !s_GetWindowSize || !s_GetDrawableSize) return;
    int ww = 0, wh = 0, dw = 0, dh = 0;
    s_GetWindowSize(win, &ww, &wh);
    s_GetDrawableSize(win, &dw, &dh);
    if (ww > 0 && wh > 0 && dw > 0 && dh > 0) { *px = x * dw / ww; *py = y * dh / wh; }
}

static int HitAtLocked(int lx, int ly)
{
    for (int i = 0; i < g_hitCount; i++) {
        const Hit& h = g_hits[i];
        if (lx >= h.x && lx < h.x + h.w && ly >= h.y && ly < h.y + h.h) return h.id;
    }
    return 0;
}

static bool VisibleLocked() { return g_uiState != 0 && !g_pageHidden && g_fontsOk; }

// Characters a field accepts (menu_hook.cpp: LlKeyboard). The name and password
// become arguments of the lobby, so they stay plain ASCII.
static bool WordChar(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}

static void TypeLocked(const char* text)
{
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        const unsigned char c = *p;
        switch (g_focus) {
            case 1: if (c > 32 && P().joinCode.size() < 200) P().joinCode.push_back((char)c); break;
            case 2: if (WordChar(c) && P().passCode.size() < 32) P().passCode.push_back((char)c); break;
            case 3: if ((WordChar(c) || (c == ' ' && !P().username.empty())) && P().username.size() < 64) P().username.push_back((char)c); break;
            case 4: if ((WordChar(c) || ((c == ' ' || c == '\'') && !P().lobbyName.empty())) && P().lobbyName.size() < 64)
                        P().lobbyName.push_back((char)c);
                    break;
            default: break;
        }
    }
    g_dirty = true;
}

// The chat takes any text, whole characters up to 190 bytes (menu_hook.cpp g_chatInput).
static void TypeChatLocked(const char* text)
{
    std::string& in = P().chatInput;
    for (const unsigned char* p = (const unsigned char*)text; *p;) {
        size_t len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 1;
        for (size_t k = 1; k < len; k++) if (!p[k]) { len = k; break; }
        if (in.size() + len > 190) break;
        if (*p >= 0x20 && *p != 0x7f) in.append((const char*)p, len);
        p += len;
    }
    g_dirty = true;
}

static void PopCharLocked(std::string* s)
{
    if (s->empty()) return;
    size_t start = s->size() - 1;
    while (start && ((unsigned char)(*s)[start] & 0xC0) == 0x80) --start;
    s->resize(start);   // a whole UTF-8 character
    g_dirty = true;
}

static void BackspaceLocked()
{
    std::string* s = g_focus == 1 ? &P().joinCode : g_focus == 2 ? &P().passCode : g_focus == 3 ? &P().username
                   : g_focus == 4 ? &P().lobbyName : nullptr;
    if (s) PopCharLocked(s);
}

static bool ChatFocusLocked() { return g_uiState == 2 && lobby::CapturesTyping(); }

static bool PassKey(SDL_Keycode k)
{
    // modifiers, Esc and Tab pass, so nobody is ever trapped in a field
    return k == SDLK_ESCAPE || k == SDLK_TAB || k == SDLK_LSHIFT || k == SDLK_RSHIFT || k == SDLK_LCTRL ||
           k == SDLK_RCTRL || k == SDLK_LALT || k == SDLK_RALT || k == SDLK_LGUI || k == SDLK_RGUI;
}

// True when the event is ours and the game must not see it.
static bool HandleEventLocked(SDL_Event* e, Post* post)
{
    const Uint32 t = e->type;
    const bool windowThread = t == SDL_MOUSEMOTION || t == SDL_MOUSEBUTTONDOWN || t == SDL_MOUSEBUTTONUP || t == SDL_KEYDOWN ||
                              t == SDL_KEYUP || t == SDL_TEXTINPUT || t == SDL_WINDOWEVENT;
    // a new room code goes onto the clipboard, as menu_hook.cpp did on its 'code' event
    if (windowThread && lobby::AutoCopyPending()) {
        std::string code;
        if (lobby::TakeAutoCopy(&code)) { post->copy = code; post->copied = "Your code is copied \xE2\x80\x94 share it in Discord."; }
    }
    if (t == SDL_KEYDOWN && e->key.keysym.sym == SDLK_d &&
        (e->key.keysym.mod & KMOD_CTRL) && (e->key.keysym.mod & KMOD_SHIFT)) {
        if (!e->key.repeat) {
            g_dashShown = !g_dashShown;
            WriteDashFlagLocked();
            if (g_log) g_log("[panel] dashboard %s (Ctrl+Shift+D)\n", g_dashShown ? "shown" : "hidden");
        }
        return true;
    }
    // Swallow the matching release even if the pointer left or the panel closed.
    if (t == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_RIGHT && g_capturedRight) {
        g_capturedRight = false;
        return true;
    }
    // Only while the overlay is really drawing the panel: if it stopped (a
    // Vulkan failure, a swapchain it cannot copy from), an invisible panel must
    // not keep eating the menu's clicks.
    if (!VisibleLocked() || NowMs() - g_lastFrameMs > 1000) { g_hover = 0; g_pressed = 0; return false; }

    auto inside = [](int lx, int ly) { return lx >= 0 && ly >= 0 && lx < g_pw && ly < g_ph; };
    switch (t) {
        case SDL_MOUSEMOTION: {
            int x, y;
            ToPixels(e->motion.windowID, e->motion.x, e->motion.y, &x, &y);
            const int lx = x - g_px, ly = y - g_py;
            g_hover = inside(lx, ly) ? HitAtLocked(lx, ly) : 0;
            return inside(lx, ly);
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int x, y;
            ToPixels(e->button.windowID, e->button.x, e->button.y, &x, &y);
            const int lx = x - g_px, ly = y - g_py;
            const bool in = inside(lx, ly);
            if (t == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_RIGHT && in) {
                g_capturedRight = true;
                const int id = HitAtLocked(lx, ly);
                if (id) OnHitLocked(id, post, true);
            } else if (t == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
                const int prevFocus = g_focus;
                g_focus = 0;
                if (in) {
                    const int id = HitAtLocked(lx, ly);
                    g_pressed = id;
                    if (id) OnHitLocked(id, post);
                }
                if (g_focus != prevFocus) g_dirty = true;
            } else if (t == SDL_MOUSEBUTTONUP) {
                g_pressed = 0;
            }
            return in;
        }
        case SDL_MOUSEWHEEL:
            // SDL2 has no wheel position: consume it when the cursor is over the panel
            return g_hover != 0;
        case SDL_TEXTINPUT:
            if (ChatFocusLocked()) { TypeChatLocked(e->text.text); return true; }
            if (!g_focus) return false;
            TypeLocked(e->text.text);
            return true;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const SDL_Keycode k = e->key.keysym.sym;
            if (ChatFocusLocked()) {
                if (PassKey(k)) return false;
                if (t == SDL_KEYDOWN) {
                    if (k == SDLK_BACKSPACE) PopCharLocked(&P().chatInput);
                    else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && !P().chatInput.empty()) {
                        const std::string why = lobby::SendChat(P().chatInput);
                        if (why.empty()) P().chatInput.clear();   // kept when not sent: nothing typed is lost
                        else SetStatusLocked(why);
                        g_dirty = true;
                    }
                }
                return true;   // down and up: no game binding fires while chatting
            }
            if (!g_focus) {
                if (t == SDL_KEYDOWN && k == SDLK_ESCAPE && g_uiState == 1) { g_uiState = 0; g_dirty = true; return true; }
                return false;
            }
            if (PassKey(k)) {
                if (t == SDL_KEYDOWN && k == SDLK_ESCAPE) { g_focus = 0; g_dirty = true; return true; }
                return false;
            }
            if (t == SDL_KEYDOWN) {
                if (k == SDLK_BACKSPACE) BackspaceLocked();
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    const int f = g_focus;
                    g_focus = 0;
                    if (f == 3) {
                        P().userAuto = P().username.empty() || P().username == P().steamName;
                        if (P().username.empty()) { P().username = P().steamName; EnsureUsernameLocked(); }
                    }
                    if (f == 3 || f == 4) SaveNamesLocked();
                    if (f == 1) OnHitLocked(3, post);
                    g_dirty = true;
                } else if (k == SDLK_v && (e->key.keysym.mod & KMOD_CTRL) && g_focus == 1) {
                    post->pasteCode = true;
                }
            }
            return true;   // down and up: no game binding fires while typing
        }
        default:
            return false;
    }
}

// The clipboard, with the panel's lock released (see LOCKS).
static void RunPost(const Post& post)
{
    if (!post.copy.empty()) {
        int r = -1;
        if (s_SetClipboardText) {
            t_inClipboard = true;
            r = s_SetClipboardText(post.copy.c_str());
            t_inClipboard = false;
        }
        SetStatus(r == 0 ? post.copied.c_str() : "Couldn't put the code on the clipboard.");
        if (r != 0 && g_log) g_log("[panel] SDL_SetClipboardText failed\n");
    }
    if (post.pasteCode || post.joinFromClipboard) {
        std::string clip;
        if (s_GetClipboardText && s_free) {
            t_inClipboard = true;
            char* c = s_GetClipboardText();
            t_inClipboard = false;
            if (c) { clip = c; s_free(c); }
        }
        std::lock_guard<std::mutex> lk(g_mtx);
        if (post.pasteCode) AppendCodeLocked(clip);
        if (post.joinFromClipboard) StartLobbyLocked(true, clip);
        g_dirty = true;
    }
}

static int SDLCALL EventFilter(void*, SDL_Event* e)
{
    // re-entered from inside a clipboard call on this thread: straight through
    if (t_inClipboard) return g_prevFilter ? g_prevFilter(g_prevUserdata, e) : 1;
    Post post;
    bool consumed;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        consumed = HandleEventLocked(e, &post);
    }
    if (!post.copy.empty() || post.pasteCode || post.joinFromClipboard) RunPost(post);
    if (consumed) return 0;
    return g_prevFilter ? g_prevFilter(g_prevUserdata, e) : 1;
}

void InstallInput()
{
    Tpf2mpLogFn log;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_inputInstalled) return;
        g_inputInstalled = true;
        log = g_log;
        s_SetEventFilter   = (decltype(s_SetEventFilter))dlsym(RTLD_DEFAULT, "SDL_SetEventFilter");
        s_GetEventFilter   = (decltype(s_GetEventFilter))dlsym(RTLD_DEFAULT, "SDL_GetEventFilter");
        s_GetWindowFromID  = (decltype(s_GetWindowFromID))dlsym(RTLD_DEFAULT, "SDL_GetWindowFromID");
        s_GetWindowSize    = (decltype(s_GetWindowSize))dlsym(RTLD_DEFAULT, "SDL_GetWindowSize");
        s_GetDrawableSize  = (decltype(s_GetDrawableSize))dlsym(RTLD_DEFAULT, "SDL_Vulkan_GetDrawableSize");
        s_GetClipboardText = (decltype(s_GetClipboardText))dlsym(RTLD_DEFAULT, "SDL_GetClipboardText");
        s_SetClipboardText = (decltype(s_SetClipboardText))dlsym(RTLD_DEFAULT, "SDL_SetClipboardText");
        s_free             = (decltype(s_free))dlsym(RTLD_DEFAULT, "SDL_free");
    }
    if (!s_SetEventFilter) {
        if (log) log("[panel] SDL_SetEventFilter not found -- the panel gets no input\n");
        return;
    }
    if (!s_SetClipboardText || !s_GetClipboardText || !s_free)
        if (log) log("[panel] SDL clipboard functions not found -- codes are neither copied nor pasted\n");
    // Without the panel's lock (see LOCKS): SDL_GetEventFilter and
    // SDL_SetEventFilter take SDL_event_watchers_lock, and SDL_PushEvent calls
    // EventFilter under that lock (SDL release-2.24.0 SDL_events.c:1121-1123),
    // which then takes the panel's. Nothing but this thread has seen the
    // pointers above or g_prevFilter until SDL_SetEventFilter publishes
    // EventFilter under SDL's lock.
    // SDL_SetEventFilter also discards every queued event
    // (SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT), SDL_events.c:1173-1177):
    // what the game had not polled yet at its first present is lost, once.
    // A filter the game set is kept and called for everything we pass on.
    if (s_GetEventFilter && s_GetEventFilter(&g_prevFilter, &g_prevUserdata) != SDL_TRUE) g_prevFilter = nullptr;
    s_SetEventFilter(&EventFilter, nullptr);
    if (log) log("[panel] SDL event filter installed%s\n", g_prevFilter ? " (chained to the game's own)" : "");
}

// ---- the lobby's callbacks (lobby_linux.h: never with a lobby lock held) -------------------------
static void StatusFromLobby(const char* utf8) { SetStatus(utf8); }
static void DirtyFromLobby() { g_asyncDirty = true; }

// ---- the interface ------------------------------------------------------------------------
void Init(const char* dataDir, const char* gameDir, const char* libDir, Tpf2mpLogFn log)
{
    // The fonts before the lock: the Noto fallback is a 16 MB read, and the
    // render thread takes the lock every frame (Visible). Only Init touches
    // layer::'s faces until g_fontsOk is set below, under the lock.
    const std::string game = gameDir;
    const bool lato = layer::AddFont((game + "res/fonts/Lato2OFL/Lato-Regular.ttf").c_str());
    const bool noto = layer::AddFont((game + "res/fonts/Noto/NotoSansCJKsc-Regular.otf").c_str());
    const bool fontsOk = layer::HaveFont();
    if (log) log("[panel] fonts: Lato %s, Noto CJK fallback %s\n", lato ? "loaded" : "MISSING", noto ? "loaded" : "missing");

    lobby::Config cfg;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_log = log;
        P().dataDir = dataDir;
        ReadFlags(std::string(libDir) + "tpf2_menu_flags.txt");
        LoadNamesLocked();
        WriteDashFlagLocked();
        cfg.dataDir = P().dataDir;
        cfg.gameDir = game;
        cfg.masterUrl = P().flagMaster;
        cfg.relayAutosaveMin = g_flagRelayAutosaveMin;
        cfg.shareMods = g_flagShareMods;
        cfg.autoload = g_flagAutoLoad;
    }
    // Unlocked: lobby::Init calls into the menu-game area (lobby_linux.h LOCKS).
    lobby::Init(cfg, log, &StatusFromLobby, &DirtyFromLobby);

    // Visible, and so clickable, only from here: every click reaches an inited lobby.
    std::lock_guard<std::mutex> lk(g_mtx);
    g_fontsOk = fontsOk;
    g_initDone = true;
    g_dirty = true;
}

void Open()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    // the lobby page comes back while a lobby runs (its close button is LEAVE)
    if (g_uiState == 0) g_uiState = 1;
    g_pageHidden = false;
    g_dirty = true;
    if (!g_initDone) { if (g_log) g_log("[panel] opened while the panel is still loading -- it shows when it is done\n"); }
    else if (!g_fontsOk && g_log) g_log("[panel] no font loaded -- the panel cannot be drawn\n");
}

void OnMenuPage(int page)
{
    lobby::OnMenuPage(page);
    std::lock_guard<std::mutex> lk(g_mtx);
    // The title menu builds pages 0 -> 2 -> 1; full-screen pages (settings,
    // campaign, load...) are 3 and up (menu_hook.cpp: MyCreatePage).
    static int seen = 0;
    if (seen < 30 && g_log) { seen++; g_log("[panel] CreatePage page=%d\n", page); }
    if (page == 2) g_pageHidden = false;
    else if (page >= 3) { g_pageHidden = true; g_focus = 0; }
}

void OnGameUiFrame() { lobby::OnGameUiFrame(); }

void SetStatus(const char* utf8)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    SetStatusLocked(utf8 ? utf8 : "");
}

bool Visible()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return VisibleLocked();
}

void MaxSize(int screenW, int screenH, int* w, int* h)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_s = UiScale(screenH);
    *w = S(800) < screenW ? S(800) : screenW;
    *h = S(560) < screenH ? S(560) : screenH;
}

bool Frame(int screenW, int screenH, int* x, int* y, int* w, int* h, bool* changed)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    SteamNameTickLocked();
    if (!VisibleLocked()) return false;
    int pw, ph;
    LayoutLocked(screenW, screenH, &pw, &ph);
    g_pw = pw; g_ph = ph;
    g_px = (screenW - pw) / 2;
    g_py = (screenH - ph) / 2;
    if (g_uiState == 1) lobby::PublicPoll();
    const uint64_t now = NowMs();
    const bool async = g_asyncDirty.exchange(false);
    *changed = g_dirty || async || layer::Width() != pw || layer::Height() != ph || now - g_lastRenderMs > 500;
    if (*changed) {
        if (g_uiState == 2) lobby::Snapshot(&P().view);
        else lobby::PublicSnapshot(&P().pubRows, &P().pubNote);
        RenderLocked(pw, ph);
        g_dirty = false;
        g_lastRenderMs = now;
    }
    g_lastFrameMs = now;
    *x = g_px; *y = g_py; *w = pw; *h = ph;
    return true;
}

bool Hover(int* x, int* y, int* w, int* h, bool* pressed)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_hover) return false;
    for (int i = 0; i < g_hitCount; i++) {
        const Hit& hh = g_hits[i];
        if (hh.btn && hh.id == g_hover) {
            *x = hh.x; *y = hh.y; *w = hh.w; *h = hh.h;
            *pressed = g_pressed == g_hover;
            return true;
        }
    }
    return false;
}

}  // namespace panel
