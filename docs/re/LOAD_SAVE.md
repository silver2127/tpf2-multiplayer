# Loading a save in-process (toward a portable, deterministic save load)

Goal: replace the fragile per-resolution Continue-click in `menu_hook.cpp`
(`clickContinueLoad`) with a direct in-process call that loads a specific save
file — resolution-independent AND deterministic (loads exactly our `mp_shared`
save, not "whatever's newest"). RVAs are image-base 0x140000000.

## The load chain (decompiled 2026-08-29)

- **`LoadGame` @ `0x2e5ec0`** — the real loader (prints "Loading from file" @
  string 0x2f816b8). Sig: `unique_ptr<CGame> LoadGame(GameUserProfileContext,
  unique_ptr<ModRep const>, const platform::SaveGameId&, const vector<ModId>*,
  const unordered_map<string,lua::Table>*, const vector<pair<string,string>>*,
  bool, const string&, const string&, IProgressMonitor&)`. Too many args to call
  raw. Only caller: `0x67d130` (menuui.cpp), which assembles all of it from a
  pre-built context object.

- **`UI::CMenuUI::StartSavegame(const LoadGameParams&, const SavegameInfo&)` @
  `0x6785c0`** — THE clean entry. `this` = CMenuUI. Every load path funnels here
  (callers: gameui.cpp 57b730, ingamemenuui.cpp 5f1d90, menuui.cpp 6592b0/
  65e780/65f2f0/672b10/67ce30). It internally pulls profile (bb23c0/bb45d0),
  modrep (2371c20), globalsettings (2a49c0), validates via serializer
  (`0x2e5df0`), and enqueues the load. Returns bool. Guard: `this+0x1988` = "load
  already active" flag (logs "Game initialization is already active!" if set).
  Reads: LoadGameParams +0x68 (mods), +0x80 (bool pick info-vs-params mods),
  +0x108 (campaign string), +0x128 (mission string) — asserts
  `campaign.empty()==mission.empty()` (both empty for a plain save). SavegameInfo
  passed to serializer + copied (FUN_1404c21d0).

- **`platform::StandardSaveGameBackend::GetSavegameInfo(const SaveGameId&)` @
  `0x2471830`** — builds a `SavegameInfo` from a `SaveGameId`. Reads the id's
  name string @ id+0x20 (len id+0x30, cap id+0x38) and saveDirectory @ id+0x40
  (asserts "saveDirectory"). So a `SaveGameId` ≈ { …; string name @0x20; string
  dir @0x40; … }.
- **`platform::StandardSaveGameBackend::FindAllSaveGames(const string&)` @
  `0x24700d0`** — lists saves (→ vector<platform::SaveGameInfo>). A way to obtain
  a valid SaveGameId without hand-building one.
- Helpers: `GetSavegameInfo`→SavegameInfo; `UI::SavegameInfoComp::ApplySettings`
  @ `0x6e2b70` (mods/params/luaTables); `UI::CMenuUI::StartSavegame` is 0x6785c0.

## Plan to implement (needs the LIVE game to verify offsets)

1. Capture `CMenuUI this` — DONE: `g_menuThis` set in `MyCreatePage` (it's the
   CreatePage `this`, same class as StartSavegame).
2. Get the `StandardSaveGameBackend` singleton (find its accessor / global).
3. Build or fetch a `SaveGameId` for `mp_shared` (via FindAllSaveGames, or
   construct { name="mp_shared", dir=<save folder> }).
4. `GetSavegameInfo(backend, &info, &saveId)` → a valid `SavegameInfo`.
5. Build a `LoadGameParams` (campaign+mission empty strings, mods from info).
6. `StartSavegame(g_menuThis, &params, &info)`.

## Why it's not done yet

Steps 3–5 require reconstructing three non-trivial C++ structs (`SaveGameId`,
`SavegameInfo`, `LoadGameParams` — each with std::string/std::vector members that
need correct construction AND destruction) by exact byte layout, plus finding the
backend singleton. A wrong offset crashes. The `.fields.txt` from DecompileTargets
(`C:\tools\ghidra_out\decomp\*.fields.txt`) gives the field-access maps to start
from, but offsets should be confirmed against a **live** SavegameInfo/LoadGameParams
(inspect memory during a real Continue) before wiring the call. This is a
multi-step interop task, best done with the game running.

Interim: the coord-click `clickContinueLoad` stays as the working (non-portable)
fallback until this lands.
