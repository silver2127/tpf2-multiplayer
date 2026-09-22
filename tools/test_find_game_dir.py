#!/usr/bin/env python3
"""Offline test of the installer's Steam-library scan (installer/ca/tpf2ca.cpp, FindGameDir).

Builds tpf2ca.cpp as a console harness (-DTPF2CA_HARNESS) with the toolchain from
tools/msvc_env.bat, then hands it fake Steam folders under %TEMP%:

  1. the game in a SECOND library on another path, named in config\\libraryfolders.vdf
     with Valve's doubled backslashes: found there, not in the Steam folder
  2. the old steamapps\\libraryfolders.vdf layout ("1" "D:\\\\Games"): found too
  3. the game in the Steam folder itself: found there first
  4. libraries that do not exist on disk, entries that are not paths (app sizes,
     labels), and a library without the game: skipped, and "(none)" when nobody has it
  5. a forward-slash path (a hand-edited vdf): accepted

Nothing outside %TEMP%\\tpf2mp_findgame_test is touched.
"""
import os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = Path(tempfile.gettempdir()) / "tpf2mp_findgame_test"
EXE = WORK / "findgame_harness.exe"
fails = 0


def check(name, ok, extra=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + (("  -- " + extra) if extra else ""))
    if not ok:
        fails += 1


def build():
    WORK.mkdir(parents=True, exist_ok=True)
    bat = WORK / "build.bat"
    bat.write_text(
        "@echo off\r\n"
        f'call "{ROOT / "tools" / "msvc_env.bat"}" >nul 2>nul || exit /b 1\r\n'
        f'cl /nologo /O2 /MT /W3 /EHsc /DTPF2CA_HARNESS "{ROOT / "installer" / "ca" / "tpf2ca.cpp"}" '
        f'/Fe:"{EXE}" /Fo:"{WORK}\\\\" msi.lib > "{WORK / "build.log"}" 2>&1\r\n', encoding="ascii")
    if EXE.exists():
        EXE.unlink()
    subprocess.run(["cmd.exe", "/c", str(bat)], check=False)
    if not EXE.exists():
        print((WORK / "build.log").read_text(errors="replace"))
        sys.exit("harness build failed")


def vdf_path(p):
    return str(p).replace("\\", "\\\\")


def steam_root(name, config_entries=None, legacy_entries=None, game_here=False):
    root = WORK / name
    if root.exists():
        shutil.rmtree(root)
    (root / "steamapps").mkdir(parents=True)
    (root / "config").mkdir()
    if config_entries is not None:
        body = '"libraryfolders"\n{\n'
        for i, e in enumerate(config_entries):
            body += f'\t"{i}"\n\t{{\n\t\t"path"\t\t"{e}"\n\t\t"label"\t\t""\n\t\t"contentid"\t\t"1728880720835917396"\n'
            body += '\t\t"apps"\n\t\t{\n\t\t\t"1066780"\t\t"3600000000"\n\t\t\t"7600"\t\t"1617213128"\n\t\t}\n\t}\n'
        body += "}\n"
        (root / "config" / "libraryfolders.vdf").write_text(body, encoding="utf-8")
    if legacy_entries is not None:
        body = '"LibraryFolders"\n{\n\t"TimeNextStatsReport"\t\t"1620000000"\n\t"ContentStatsID"\t\t"-4800000000000000000"\n'
        for i, e in enumerate(legacy_entries, 1):
            body += f'\t"{i}"\t\t"{e}"\n'
        body += "}\n"
        (root / "steamapps" / "libraryfolders.vdf").write_text(body, encoding="utf-8")
    if game_here:
        put_game(root)
    return root


def library(name, with_game=True):
    lib = WORK / name
    if lib.exists():
        shutil.rmtree(lib)
    (lib / "steamapps" / "common").mkdir(parents=True)
    if with_game:
        put_game(lib)
    return lib


def put_game(lib):
    d = lib / "steamapps" / "common" / "Transport Fever 2"
    d.mkdir(parents=True, exist_ok=True)
    (d / "TransportFever2.exe").write_bytes(b"MZ")


def run(root):
    out = subprocess.run([str(EXE), str(root)], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    libs = [l[len("library "):].strip() for l in out.splitlines() if l.startswith("library ")]
    game = next((l[len("game "):].strip() for l in out.splitlines() if l.startswith("game ")), None)
    return libs, game, out


def same(a, b):
    return os.path.normcase(str(a).rstrip("\\")) == os.path.normcase(str(b).rstrip("\\"))


def main():
    build()
    # 1. second library, new layout, doubled backslashes
    lib2 = library("lib2")
    root = steam_root("steam1", config_entries=[vdf_path(WORK / "steam1"), vdf_path(lib2)])
    libs, game, out = run(root)
    check("1: both libraries listed", len(libs) == 2 and same(libs[0], root) and same(libs[1], lib2), out)
    check("1: the game found in the second library", game and same(game, lib2 / "steamapps" / "common" / "Transport Fever 2"), out)

    # 2. legacy layout only
    lib3 = library("lib3")
    root = steam_root("steam2", legacy_entries=[vdf_path(lib3)])
    libs, game, out = run(root)
    check("2: legacy steamapps\\libraryfolders.vdf entry listed", any(same(l, lib3) for l in libs), out)
    check("2: the game found through it", game and same(game, lib3 / "steamapps" / "common" / "Transport Fever 2"), out)

    # 3. the game in the Steam folder itself wins (it is listed first)
    root = steam_root("steam3", config_entries=[vdf_path(WORK / "steam3"), vdf_path(lib2)], game_here=True)
    libs, game, out = run(root)
    check("3: the Steam folder's own game is taken first", game and same(game, root / "steamapps" / "common" / "Transport Fever 2"), out)

    # 4. missing folders, non-path strings and a library without the game
    lib4 = library("lib4", with_game=False)
    ghost = WORK / "nowhere" / "SteamLibrary"
    root = steam_root("steam4", config_entries=[vdf_path(WORK / "steam4"), vdf_path(ghost), vdf_path(lib4)])
    libs, game, out = run(root)
    check("4: a library missing on disk is skipped", not any(same(l, ghost) for l in libs), out)
    check("4: app sizes and labels are not libraries", all(":" in l for l in libs) and len(libs) == 2, out)
    check("4: no game anywhere -> (none)", game == "(none)", out)

    # 5. forward slashes in the vdf
    root = steam_root("steam5", config_entries=[str(lib2).replace("\\", "/")])
    libs, game, out = run(root)
    check("5: a forward-slash path is accepted", game and same(game, lib2 / "steamapps" / "common" / "Transport Fever 2"), out)

    # 6. no Steam at all
    libs, game, out = run(WORK / "does-not-exist")
    check("6: an absent Steam folder lists nothing and finds nothing", libs == [] and game == "(none)", out)

    print("\n" + ("FAILED" if fails else "all passed"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
