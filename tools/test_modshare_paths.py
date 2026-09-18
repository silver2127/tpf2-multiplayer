"""Offline test: Workshop mods are looked up in the LIBRARY that holds the game
(D:\\SteamLibrary), not only under Steam's own folder (modshare.workshop_dirs).
A host with the game on another drive found none of its Workshop mods and every
joiner was rejected with "The host cannot supply all required mods" (2026-09-18).

    python tools/test_modshare_paths.py
"""
import os, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "netpunch"))
import modshare                                               # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


tmp = tempfile.mkdtemp(prefix="modshare_paths_")
steam = os.path.join(tmp, "Steam")                            # Steam's own folder, game NOT here
lib = os.path.join(tmp, "SteamLibrary")                        # the library with the game
game = os.path.join(lib, "steamapps", "common", "Transport Fever 2")
os.makedirs(os.path.join(steam, "steamapps"))
os.makedirs(game)
open(os.path.join(game, "TransportFever2.exe"), "wb").close()
ws = os.path.join(lib, "steamapps", "workshop", "content", modshare.TF2_APPID, "2987815473")
os.makedirs(ws)
open(os.path.join(ws, "mod.lua"), "w").write("function data() return {} end")
managed = os.path.join(tmp, "data", "workshop")
os.makedirs(managed)

modshare.steam_root = lambda: steam
modshare.game_dir = lambda: game
modshare.managed_workshop = lambda: managed

check("the library root is the game's library", modshare.library_root() == lib, str(modshare.library_root()))
dirs = modshare.workshop_dirs()
check("the game's library is searched first, Steam's folder second",
      dirs == [os.path.join(lib, "steamapps", "workshop", "content", modshare.TF2_APPID),
               os.path.join(steam, "steamapps", "workshop", "content", modshare.TF2_APPID)], str(dirs))
check("a Workshop mod in the game's library is found", modshare.find_mod("*2987815473", 1) == ws, str(modshare.find_mod("*2987815473", 1)))
check("a Workshop id that is nowhere is None", modshare.find_mod("*1", 1) is None)
# the game in Steam's own folder: one folder, no duplicate
modshare.game_dir = lambda: os.path.join(steam, "steamapps", "common", "Transport Fever 2")
os.makedirs(modshare.game_dir())
check("game in Steam's folder: one Workshop folder", modshare.workshop_dirs() == [os.path.join(steam, "steamapps", "workshop", "content", modshare.TF2_APPID)], str(modshare.workshop_dirs()))
modshare.game_dir = lambda: None
check("no game: Steam's folder alone", modshare.workshop_dirs() == [os.path.join(steam, "steamapps", "workshop", "content", modshare.TF2_APPID)])

open(os.path.join(ws, "big.bin"), "wb").write(b"x" * 5000)
os.makedirs(os.path.join(ws, ".git")); open(os.path.join(ws, ".git", "junk"), "wb").write(b"y" * 9000)
check("folder_bytes counts the mod's files and skips dot folders", modshare.folder_bytes(ws) == 5000 + len("function data() return {} end"), str(modshare.folder_bytes(ws)))
check("folder_bytes of a missing folder is 0", modshare.folder_bytes(os.path.join(tmp, "nope")) == 0)

print("FAILED: " + ", ".join(fails) if fails else "ALL OK")
sys.exit(1 if fails else 0)
