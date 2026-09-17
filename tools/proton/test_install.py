#!/usr/bin/env python3
"""Offline test of tools/proton/install.py against a synthetic Steam tree.

A fake Steam root with a second library in libraryfolders.vdf, a fake Windows
game whose TransportFever2.exe carries build 35924's PE stamp, a fake stock
alut.dll, a payload zip in the MSI's layout, and a Proton prefix. The lobby
repair runs against the real netpunch.exe when one is at hand
(netpunch/dist/netpunch.exe on the build machine, or $TPF2MP_TEST_LOBBY).

  python3 tools/proton/test_install.py
"""
import contextlib
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import install  # noqa: E402

STOCK_ALUT = b"stock alut for tests\n"
PAYLOAD = {
    "alut.dll": b"proxy v1", "tpf2_pluginhost.dll": b"host v1", "tpf2_bridge_mp.dll": b"bridge v1",
    "tpf2_menu.dll": b"menu v1", "tpf2_slice.dll": b"slice v1", "tpf2_slice.cfg": b"shipped cfg\n",
    "tpf2mp_version.txt": b"0.5.7\n", "netpunch/netpunch.exe": b"not a bundle",
    "plugins/tpf2_previews.dll": b"previews v1", "plugins/tpf2_workshop_register.dll": b"workshop v1",
    "mods/mp_lockstep_1/mod.lua": b"mod", "mods/mp_lockstep_1/image_00.tga": b"tga",
    "mods/mp_lockstep_1/res/config/game_script/lockstep.lua": b"lockstep",
    "mods/mp_lockstep_1/res/scripts/mp/net.lua": b"net",
}


def fake_exe(stamp=install.BUILD_TIMESTAMP, image=install.BUILD_IMAGE_SIZE):
    data = bytearray(0x80 + 24 + 240)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    struct.pack_into("<I", data, 0x88, stamp)
    struct.pack_into("<H", data, 0x80 + 20, 240)
    struct.pack_into("<H", data, 0x98, 0x20b)
    struct.pack_into("<I", data, 0x98 + 56, image)
    return bytes(data)


def make_zip(path, files):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)


def run(*args, expect=0):
    """install.main in-process (the stock alut hash is patched for the fake game)."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        try:
            code = install.main([str(a) for a in args]) or 0
        except install.Fail as error:
            print(f"ERROR: {error}")
            code = 1
    assert code == expect, (args, code, out.getvalue())
    return out.getvalue()


def main():
    assert install.STOCK_ALUT_SHA256 == "3df103ae3d94a6b90c4d2a6d75dcb388cd835f5e3af9962b22c20d4473cfc035"
    install.STOCK_ALUT_SHA256 = hashlib.sha256(STOCK_ALUT).hexdigest()
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        steam = td / "Steam"
        library = td / "Library2"
        (steam / "steamapps").mkdir(parents=True)
        (steam / "userdata/123").mkdir(parents=True)
        (steam / "steamapps/libraryfolders.vdf").write_text(
            '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' % (steam, library))
        game = library / "steamapps/common" / install.GAME_FOLDER
        (game / "mods/urbangames_sandbox_1").mkdir(parents=True)
        (library / "steamapps/workshop").mkdir(parents=True)
        (library / "steamapps" / f"appmanifest_{install.APP_ID}.acf").write_text('"AppState"\n{\n\t"appid"\t\t"1066780"\n}\n')
        (game / "TransportFever2.exe").write_bytes(fake_exe())
        (game / "alut.dll").write_bytes(STOCK_ALUT)
        (game / "mods/urbangames_sandbox_1/mod.lua").write_bytes(b"stock mod")
        prefix = library / "steamapps/compatdata" / install.APP_ID / "pfx"
        (prefix / "drive_c/Program Files (x86)/Steam/steamapps").mkdir(parents=True)
        (prefix / "drive_c/Program Files (x86)/Steam/steamapps/libraryfolders.vdf").write_text("proton's own\n")
        (prefix / "drive_c/users/steamuser/AppData/Local/tpf2mp").mkdir(parents=True)
        zip1 = td / "files-1.zip"
        make_zip(zip1, PAYLOAD)
        env_root = ["--steam-root", steam]

        # 1. dry run changes nothing
        out = run(*env_root, "--files-zip", zip1, "--dry-run")
        assert "14 files to install" in out and "3 prefix links to create" in out and "alut.dll is kept as alut_real.dll" in out, out
        assert not (game / "alut_real.dll").exists() and not (game / "tpf2_menu.dll").exists()

        # 2. install: files, stock alut parked, links, manifest, stock mods untouched
        out = run(*env_root, "--files-zip", zip1)
        assert "PASS: TpF2 Multiplayer 0.5.7 (14 files)" in out, out
        assert (game / "alut_real.dll").read_bytes() == STOCK_ALUT and (game / "alut.dll").read_bytes() == b"proxy v1"
        assert (game / "mods/mp_lockstep_1/res/scripts/mp/net.lua").read_bytes() == b"net"
        assert (game / "mods/urbangames_sandbox_1/mod.lua").read_bytes() == b"stock mod"
        winsteam = prefix / "drive_c/Program Files (x86)/Steam"
        assert os.readlink(winsteam / "userdata") == str(steam / "userdata")
        assert os.readlink(winsteam / "steamapps/common") == str(game.parent)
        assert os.readlink(winsteam / "steamapps/workshop") == str(library / "steamapps/workshop")
        assert (winsteam / "steamapps/libraryfolders.vdf").read_text() == "proton's own\n"      # Proton's file kept
        manifest = json.loads((game / install.MANIFEST_NAME).read_text())
        assert manifest["version"] == "0.5.7" and len(manifest["files"]) == 14 and not (game / install.BACKUPS).exists()

        # 3. repeat: nothing to do; verify passes
        assert "nothing to do" in run(*env_root, "--files-zip", zip1)
        assert "PASS" in run(*env_root, "--verify")

        # 4. upgrade: a changed cfg is kept, a stale mod file is removed, replaced files are backed up
        (game / "tpf2_slice.cfg").write_bytes(b"my settings\n")
        (game / "mods/mp_lockstep_1/res/scripts/mp/old.lua").write_bytes(b"stale")
        upgraded = dict(PAYLOAD, **{"tpf2_menu.dll": b"menu v2", "tpf2mp_version.txt": b"0.5.8\n", "tpf2_slice.cfg": b"new cfg\n"})
        del upgraded["mods/mp_lockstep_1/res/scripts/mp/net.lua"]
        upgraded["mods/mp_lockstep_1/res/scripts/mp/new.lua"] = b"new"
        zip2 = td / "files-2.zip"
        make_zip(zip2, upgraded)
        out = run(*env_root, "--files-zip", zip2)
        assert "PASS: TpF2 Multiplayer 0.5.8" in out, out
        assert (game / "tpf2_slice.cfg").read_bytes() == b"my settings\n"
        assert not (game / "mods/mp_lockstep_1/res/scripts/mp/old.lua").exists()
        assert not (game / "mods/mp_lockstep_1/res/scripts/mp/net.lua").exists()
        assert (game / "mods/mp_lockstep_1/res/scripts/mp/new.lua").read_bytes() == b"new"
        backups = list((game / install.BACKUPS).iterdir())
        assert len(backups) == 1 and (backups[0] / "tpf2_menu.dll").read_bytes() == b"menu v1"
        assert (backups[0] / "mods/mp_lockstep_1/res/scripts/mp/old.lua").read_bytes() == b"stale"

        # 5. verify catches a changed managed file
        (game / "tpf2_slice.dll").write_bytes(b"tampered")
        assert "changed or missing tpf2_slice.dll" in run(*env_root, "--verify", expect=1)
        run(*env_root, "--files-zip", zip2)

        # 6. refusals: foreign alut, wrong build, native Linux game, payload inside the game folder
        foreign = td / "foreign"
        foreign.mkdir()
        (foreign / "alut.dll").write_bytes(b"some other mod")
        (foreign / "TransportFever2.exe").write_bytes(fake_exe())
        assert "another mod or tool has replaced it" in run(*env_root, "--game-dir", foreign, "--files-zip", zip2, expect=1)
        (foreign / "TransportFever2.exe").write_bytes(fake_exe(stamp=0x12345678))
        assert "not Steam build 35924" in run(*env_root, "--game-dir", foreign, "--files-zip", zip2, expect=1)
        native = td / "native"
        native.mkdir()
        (native / "TransportFever2").write_bytes(b"\x7fELF")
        assert "native Linux game" in run(*env_root, "--game-dir", native, "--files-zip", zip2, expect=1)
        inside = game / "payload"
        inside.mkdir()
        for name, data in upgraded.items():
            (inside / name).parent.mkdir(parents=True, exist_ok=True)
            (inside / name).write_bytes(data)
        assert "must not be inside the game folder" in run(*env_root, "--payload-dir", inside, expect=1)
        import shutil
        shutil.rmtree(inside)

        # 7. an extracted MSI's nesting is found; a prefix path holding data stops the install
        nested = td / "extracted/Program Files/Steam/steamapps/common/Transport Fever 2"
        for name, data in upgraded.items():
            (nested / name).parent.mkdir(parents=True, exist_ok=True)
            (nested / name).write_bytes(data)
        assert "nothing to do" in run(*env_root, "--payload-dir", nested)
        (winsteam / "userdata").unlink()
        (winsteam / "userdata/config").mkdir(parents=True)
        (winsteam / "userdata/config/x").write_bytes(b"data")
        assert "exists and is not empty" in run(*env_root, "--payload-dir", nested, expect=1)
        shutil.rmtree(winsteam / "userdata")
        run(*env_root, "--payload-dir", nested)

        # 8. uninstall restores the game's own alut.dll and removes the mod, links and manifest
        out = run(*env_root, "--uninstall")
        assert "restoring the game's own alut.dll" in out and "uninstalled" in out, out
        assert (game / "alut.dll").read_bytes() == STOCK_ALUT and not (game / "alut_real.dll").exists()
        assert not (game / "mods/mp_lockstep_1").exists() and (game / "mods/urbangames_sandbox_1/mod.lua").exists()
        assert not (game / "tpf2_menu.dll").exists() and not (winsteam / "userdata").exists()
        assert (game / "tpf2_slice.cfg").exists()       # settings survive
        assert not (game / install.MANIFEST_NAME).exists()

        # 9. uninstall with another product's plugin present keeps the shared proxy and host
        run(*env_root, "--files-zip", zip2)
        (game / "plugins/tpf2_bigmap.dll").write_bytes(b"bigmap")
        out = run(*env_root, "--uninstall")
        assert "stay for tpf2_bigmap.dll" in out, out
        assert (game / "alut.dll").read_bytes() == b"proxy v1" and (game / "alut_real.dll").read_bytes() == STOCK_ALUT
        assert (game / "tpf2_pluginhost.dll").exists() and not (game / "tpf2_menu.dll").exists()

        # 10. the lobby repair on a real Windows netpunch.exe, when one is at hand
        candidates = [os.environ.get("TPF2MP_TEST_LOBBY"), HERE.parents[1] / "netpunch/dist/netpunch.exe"]
        lobby = next((Path(c) for c in candidates if c and Path(c).is_file() and Path(c).read_bytes()[:2] == b"MZ"), None)
        if lobby is None:
            print("note: no Windows netpunch.exe at hand; the lobby repair ran only on the pinned-hash paths")
        else:
            data = lobby.read_bytes()
            state = install.lobby_state(data)[0]
            assert state in ("broken", "repaired"), state
            repaired = install.repair_lobby(data)
            assert install.lobby_state(repaired)[0] == "repaired"
            assert install.repair_lobby(repaired) == repaired                       # idempotent
            a, b = install.Archive(data), install.Archive(repaired)
            assert [e.meta() for e in a.entries] == [e.meta() for e in b.entries]
            changed = [e.name for e, f in zip(a.entries, b.entries) if a.unpack(e) != b.unpack(f)]
            assert changed == ([] if state == "repaired" else [e.name for e in a.entries if e.name.startswith("miniupnpc-")]), changed
            # the installer repairs the installed copy and a cached updater copy in the prefix
            real_zip = td / "files-real.zip"
            make_zip(real_zip, dict(upgraded, **{"netpunch/netpunch.exe": data}))
            cached = prefix / "drive_c/users/steamuser/AppData/Local/tpf2mp/updates/releases/0.5.9/netpunch/netpunch.exe"
            cached.parent.mkdir(parents=True)
            cached.write_bytes(data)
            out = run(*env_root, "--files-zip", real_zip)
            assert "PASS" in out, out
            assert install.lobby_state((game / "netpunch/netpunch.exe").read_bytes())[0] == "repaired"
            assert install.lobby_state(cached.read_bytes())[0] == "repaired"
            assert "PASS" in run(*env_root, "--verify")
            assert "nothing to do" in run(*env_root, "--files-zip", real_zip)   # the repaired lobby counts as current
            # --repair-lobby on a file
            tool_copy = td / "netpunch.exe"
            tool_copy.write_bytes(data)
            out = run("--repair-lobby", tool_copy)
            assert ("repaired (64" in out) == (state == "broken"), out
            assert tool_copy.read_bytes() == repaired
            assert "already repaired" in run("--repair-lobby", tool_copy)
            # --no-lobby-repair leaves the shipped lobby alone
            run(*env_root, "--uninstall")
            out = run(*env_root, "--files-zip", real_zip, "--no-lobby-repair", expect=(1 if state == "broken" else 0))
            if state == "broken":
                assert "not repaired for Wine" in out, out
        print("PASS: proton installer: discovery through libraryfolders.vdf, dry run, install (stock alut parked, links, "
              "manifest), idempotent rerun, upgrade (settings kept, stale mod files removed, backups), verify, refusals "
              "(foreign alut, wrong build, native game, payload inside game, non-empty prefix path), extracted-MSI nesting, "
              "uninstall (restore alut; keep the shared proxy for another product)"
              + ("" if lobby is None else "; lobby repair on the real netpunch.exe (installed and cached copies, tool mode, idempotent)"))


if __name__ == "__main__":
    main()
