#!/usr/bin/env python3
"""Exercise the real preload adapter in isolated nongame and fake-game images."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--boot", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tpf2mp-format-probe-") as temporary:
        root = Path(temporary)
        loader = root / "loader/libtpf2mp_boot.so"
        loader.parent.mkdir()
        shutil.copy2(args.boot, loader)
        for name in ("not-the-game", "TransportFever2"):
            probe = root / name
            shutil.copy2(args.probe, probe)
            env = os.environ.copy()
            env.update(LD_PRELOAD=str(loader), XDG_DATA_HOME=str(root / (name + "-data")))
            env.pop("TPF2MP_DATADIR", None)
            subprocess.run([str(probe), str(loader)], cwd=root, env=env, check=True)
        log = root / "TransportFever2-data/tpf2mp/data/tpf2_proxy.log"
        assert "off (unverified image/call)" in log.read_text()


if __name__ == "__main__":
    main()
