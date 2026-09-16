"""Run the real native selector and the Lua module searcher against scratch releases."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil
from lupa.lua52 import LuaRuntime

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "netpunch"))
import updater

probe = REPO / "native/out/update_bootstrap_probe.exe"
with tempfile.TemporaryDirectory() as temporary:
    local = Path(temporary)
    test_exe = local / "probe.exe"
    shutil.copyfile(probe, test_exe)
    (local / "tpf2mp_version.txt").write_text("0.4.23")
    root = local / "tpf2mp/updates"
    release = root / "releases/0.4.24"
    release.mkdir(parents=True)
    env = dict(os.environ, LOCALAPPDATA=str(local), TPF2MP_RELEASE_ROOT="stale inherited path")
    def pinned():
        return subprocess.check_output([str(test_exe)], env=env, text=True)
    assert pinned() == ""
    updater.atomic_text(root / "active.txt", "../../escape")
    assert pinned() == ""
    updater.atomic_text(root / "active.txt", "0.4.24")
    assert pinned() == ""
    for name in updater.REQUIRED | {"ready"}:
        target = release / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("test")
    assert Path(pinned()) == release
    (local / "tpf2mp_version.txt").write_text("0.4.25")
    assert pinned() == ""
    (local / "tpf2mp_version.txt").unlink()
    assert pinned() == ""
    (local / "tpf2mp_version.txt").write_text("0.4.23")
    (release / "tpf2_slice.dll").unlink()
    assert pinned() == ""
    print("PASS native selector: missing, malformed, incomplete, complete, inherited environment")

    scripts = release / "mod/res/scripts/mp"
    (scripts / "probe.lua").write_text("return {version='new'}")
    lua = LuaRuntime(unpack_returned_tuples=True)
    lua.globals().release_root = release.as_posix()
    lua.execute('os.getenv = function(k) if k == "TPF2MP_RELEASE_ROOT" then return release_root end end')
    boot = lua.execute((REPO / "mod/mp_lockstep_1/res/scripts/mp/update_bootstrap.lua").read_text())
    boot.setup()
    lua.execute('assert(require("mp/probe").version == "new")')
    lua.execute('assert(require("mp.probe").version == "new")')
    # The path is captured once: changing environment/pointer later cannot mix releases.
    lua.globals().release_root = "missing"
    (scripts / "second.lua").write_text("return 42")
    lua.execute('assert(require("mp/second") == 42)')
    lua.execute('assert(not pcall(require, "mp/missing"))')
    lua.globals().release_root = release.as_posix()
    (scripts / "entry.lua").write_text("function data() return 'entry environment' end")
    lua.globals().wrapper = str(REPO / "mod/mp_lockstep_1/res/config/game_script/lockstep.lua")
    # The game's loader gives each script its own environment. Do not define
    # data() in require's global environment and leave the game script empty.
    lua.globals().package.loaded["mp/update_bootstrap"] = boot
    lua.execute('local env=setmetatable({}, {__index=_G}); assert(loadfile(wrapper,"t",env))(); assert(env.data()=="entry environment"); assert(rawget(_G,"data")==nil)')
    print("PASS Lua selector: slash/dot names, pinned release, missing-module failure")
