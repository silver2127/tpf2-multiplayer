#!/usr/bin/env python3
"""Exercise the real Linux preload constructor with unchanged Windows .22 Lua."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--lua", default=None)
    args = parser.parse_args()
    lua = args.lua or next((shutil.which(n) for n in ("lua", "lua5.4", "lua5.3", "lua5.2") if shutil.which(n)), None)
    if not lua:
        parser.error("a Lua interpreter is required")
    repo = Path(__file__).resolve().parents[2]
    source = repo / "mod/mp_lockstep_1/res/config/game_script/lockstep.lua"
    script = Path(__file__).with_suffix(".lua")
    with tempfile.TemporaryDirectory(prefix="tpf2mp-boot-env-") as temp:
        root = Path(temp)
        # Isolate sibling lookup: the probe must not load the actual bridge,
        # slice, menu or host just because they share the build directory.
        lib = root / "loader/libtpf2mp_boot.so"
        lib.parent.mkdir()
        shutil.copy2(args.boot, lib)
        probe = root / "TransportFever2"
        shutil.copy2(args.probe, probe)
        for case in ("home", "xdg", "override", "snap", "unicode", "relative-xdg"):
            folder = root / case
            folder.mkdir()
            env = os.environ.copy()
            for name in ("HOME", "XDG_DATA_HOME", "TPF2MP_DATADIR", "LOCALAPPDATA", "LD_PRELOAD"):
                env.pop(name, None)
            home = folder / ("Hôme with spaces" if case == "unicode" else "home")
            if case == "snap":
                home = folder / "snap/steam/common"
            home.mkdir(parents=True)
            env["HOME"] = str(home)
            if case in ("xdg", "override"):
                env["XDG_DATA_HOME"] = str(folder / "xdg data")
            elif case == "relative-xdg":
                env["XDG_DATA_HOME"] = "relative-is-not-an-XDG-root"
            xdg = env.get("XDG_DATA_HOME", "")
            local = xdg if xdg.startswith("/") else str(home / ".local/share")
            data = str(Path(local) / "tpf2mp/data") + "/"
            if case == "override":
                data = str(folder / "custom data") + "/"
                env["TPF2MP_DATADIR"] = data.rstrip("/")
            env["LOCALAPPDATA"] = "/incorrect/inherited/windows/path"
            env["LD_PRELOAD"] = str(lib)
            # The real bridge writes identity before a world loads. Include it
            # for overrides/Unicode, matching .22's first-proven-candidate rule.
            if case in ("override", "unicode"):
                Path(data).mkdir(parents=True)
                (Path(data) / "tpf2_instance.txt").write_text("A\npid=1\n")
            net = str(Path(local) / "tpf2mp/netpunch")
            Path(net).mkdir(parents=True)
            (Path(net) / "lobby_out.jsonl").write_text("")
            subprocess.run([str(probe), lua, str(script), str(source), data, local,
                            str(home), env.get("XDG_DATA_HOME", "<unset>"), net],
                           cwd=folder, env=env, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            assert Path(data).is_dir(), "boot did not create the final data directory"
        # LD_PRELOAD may also reach shell utilities before the game. They must
        # retain their own environment and must not create runtime directories.
        other = root / "not-the-game"
        shutil.copy2(args.probe, other)
        env = os.environ.copy()
        env.update(HOME=str(root / "untouched"), LOCALAPPDATA="keep", TPF2MP_DATADIR="keep-too", LD_PRELOAD=str(lib))
        check = 'assert(os.getenv("LOCALAPPDATA")=="keep" and os.getenv("TPF2MP_DATADIR")=="keep-too"); assert(os.getenv("LD_PRELOAD"))'
        subprocess.run([str(other), lua, "-e", check], cwd=root, env=env, check=True)
        assert not (root / "untouched").exists()
    print("boot environment: six Linux path cases and non-game passthrough passed")


if __name__ == "__main__":
    main()
