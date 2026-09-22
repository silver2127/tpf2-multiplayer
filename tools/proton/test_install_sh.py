#!/usr/bin/env python3
"""Offline test of tools/proton/install_proton.sh against a synthetic Steam tree.

The same fixture shape as test_install.py: a fake Steam root whose
libraryfolders.vdf names a second library holding a fake Windows game with
build 35924's PE stamp, a fake stock alut.dll (its hash passed in through
TPF2MP_STOCK_ALUT_SHA256, which exists for this test), a payload zip in the
MSI's layout, and a Proton prefix. Runs the script through bash; on Windows
(Git Bash, no symlinks without privileges) the prefix links are skipped.
  python3 tools/proton/test_install_sh.py
"""
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "install_proton.sh"
BUILD_TIMESTAMP, BUILD_IMAGE_SIZE = 0x675abcc6, 0x046ce000
STOCK_ALUT = b"stock alut for tests\n"
PAYLOAD = {
    "alut.dll": b"proxy v1", "tpf2_pluginhost.dll": b"host v1", "tpf2_bridge_mp.dll": b"bridge v1",
    "tpf2_menu.dll": b"menu v1", "tpf2_slice.dll": b"slice v1", "tpf2_slice.cfg": b"shipped cfg\n",
    "tpf2mp_version.txt": b"0.6\n", "netpunch/netpunch.exe": b"not a bundle",
    "plugins/tpf2_previews.dll": b"previews v1", "plugins/tpf2_workshop_register.dll": b"workshop v1",
    "mods/mp_lockstep_1/mod.lua": b"mod", "mods/mp_lockstep_1/image_00.tga": b"tga",
    "mods/mp_lockstep_1/res/config/game_script/lockstep.lua": b"lockstep",
}
WINDOWS = sys.platform.startswith("win")


def fake_exe(stamp=BUILD_TIMESTAMP, image=BUILD_IMAGE_SIZE):
    data = bytearray(0x200)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    struct.pack_into("<I", data, 0x88, stamp)
    struct.pack_into("<H", data, 0x80 + 20, 240)
    struct.pack_into("<H", data, 0x98, 0x20b)
    struct.pack_into("<I", data, 0x98 + 56, image)
    return bytes(data)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def bash_path():
    if WINDOWS:
        # never the WSL launcher in System32: Git for Windows' bash
        for p in (r"C:\Program Files\Git\bin\bash.exe", r"C:\Program Files\Git\usr\bin\bash.exe",
                  os.path.expandvars(r"%LOCALAPPDATA%\Programs\Git\bin\bash.exe")):
            if os.path.isfile(p):
                return p
        raise SystemExit("Git for Windows' bash.exe was not found; this test needs it on Windows")
    return "bash"


def run(args, cwd, env, expect=0):
    p = subprocess.run([bash_path(), str(SCRIPT)] + args, cwd=cwd, env=env, capture_output=True, text=True)
    if p.returncode != expect:
        print(p.stdout); print(p.stderr, file=sys.stderr)
        raise AssertionError(f"exit {p.returncode}, expected {expect}: {args}")
    return p.stdout + p.stderr


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        steam = root / "steam"; (steam / "steamapps").mkdir(parents=True); (steam / "userdata").mkdir()
        library = root / "library"; (library / "steamapps/workshop").mkdir(parents=True)
        (steam / "steamapps/libraryfolders.vdf").write_text('"libraryfolders"\n{\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' % str(library).replace("\\", "\\\\"))
        (library / "steamapps/appmanifest_1066780.acf").write_text('"AppState" {}')
        game = library / "steamapps/common/Transport Fever 2"; game.mkdir(parents=True)
        (game / "TransportFever2.exe").write_bytes(fake_exe())
        (game / "alut.dll").write_bytes(STOCK_ALUT)
        (game / "tpf2mp.cfg").write_text("mine\n")
        (game / "mods/mp_lockstep_1").mkdir(parents=True)
        (game / "mods/mp_lockstep_1/stale.lua").write_text("old file from 0.5.7")
        prefix = library / "steamapps/compatdata/1066780/pfx"; (prefix / "drive_c/Program Files (x86)/Steam/steamapps").mkdir(parents=True)
        payload = root / "TpF2Multiplayer-files.zip"
        with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED) as z:
            for name, data in PAYLOAD.items():
                z.writestr(name, data)
        env = dict(os.environ, STEAM_ROOT=str(steam), TPF2MP_STOCK_ALUT_SHA256=sha(STOCK_ALUT), HOME=str(root))
        links = ["--no-links"] if WINDOWS else []

        # 1. dry run: the plan names the copies, the stale file, the parked alut; nothing changes
        out = run(["--files-zip", str(payload), "--dry-run"] + links, tmp, env)
        assert "Dry run" in out and "remove mods/mp_lockstep_1/stale.lua" in out and "kept as alut_real.dll" in out, out
        assert "copy   tpf2mp.cfg" not in out, "a live settings file must be kept"
        assert not (game / "alut_real.dll").exists() and (game / "mods/mp_lockstep_1/stale.lua").exists()

        # 2. the install
        out = run(["--files-zip", str(payload)] + links, tmp, env)
        assert "Installed TpF2 Multiplayer 0.6" in out, out
        assert (game / "alut_real.dll").read_bytes() == STOCK_ALUT
        for name, data in PAYLOAD.items():
            if name in ("tpf2mp.cfg",):
                continue
            assert (game / name).read_bytes() == data, name
        assert (game / "tpf2mp.cfg").read_text() == "mine\n"
        assert (game / "tpf2_slice.cfg").read_bytes() == b"shipped cfg\n"       # absent before: shipped
        assert not (game / "mods/mp_lockstep_1/stale.lua").exists()
        backups = list((game / ".tpf2mp-proton-backups").glob("*/mods/mp_lockstep_1/stale.lua"))
        assert len(backups) == 1, "the stale file is kept in the backup"
        manifest = json.loads((game / ".tpf2mp-proton-manifest.json").read_text())
        assert manifest["version"] == "0.6" and manifest["files"]["tpf2_menu.dll"] == sha(b"menu v1")
        assert manifest["alut_real"] == sha(STOCK_ALUT)
        if not WINDOWS:
            winsteam = prefix / "drive_c/Program Files (x86)/Steam"
            assert os.readlink(winsteam / "userdata") == str(steam / "userdata")
            assert os.readlink(winsteam / "steamapps/common") == str(library / "steamapps/common")
            assert os.readlink(winsteam / "steamapps/workshop") == str(library / "steamapps/workshop")

        # 3. a rerun is a no-op
        out = run(["--files-zip", str(payload)] + links, tmp, env)
        assert "Already installed and current" in out, out

        # 4. a replaced alut.dll (another mod) is refused, before anything changes
        (game / "alut_real.dll").write_bytes(b"not stock")
        out = run(["--files-zip", str(payload)] + links, tmp, env, expect=1)
        assert "not the game's own alut.dll" in out, out
        (game / "alut_real.dll").write_bytes(STOCK_ALUT)

        # 5. the wrong build is refused
        (game / "TransportFever2.exe").write_bytes(fake_exe(stamp=0x11111111))
        out = run(["--files-zip", str(payload), "--dry-run"] + links, tmp, env, expect=1)
        assert "not Steam build 35924" in out, out
        (game / "TransportFever2.exe").write_bytes(fake_exe())

        # 6. uninstall: the game's alut.dll is back, the files are gone, the settings stay
        out = run(["--uninstall"] + links, tmp, env)
        assert (game / "alut.dll").read_bytes() == STOCK_ALUT and not (game / "alut_real.dll").exists()
        assert not (game / "tpf2_menu.dll").exists() and not (game / "mods/mp_lockstep_1").exists()
        assert not (game / "netpunch").exists()
        assert (game / "tpf2mp.cfg").read_text() == "mine\n"
        assert not (game / ".tpf2mp-proton-manifest.json").exists()
        print("PASS: install_proton.sh -- dry run, install (park, copy, stale removal, backups, manifest"
              + (", prefix links" if not WINDOWS else "") + "), no-op rerun, refusals, uninstall")


if __name__ == "__main__":
    main()
