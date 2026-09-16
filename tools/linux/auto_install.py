#!/usr/bin/env python3
"""Build on source changes; install the complete Linux package once games close.

No relaunch or termination. --game can be repeated for separate lab copies;
plugins are installed with every copy through the ordinary Linux installer.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def source_digest(root):
    digest = hashlib.sha256()
    paths = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"]).split(b"\0")
    for raw in sorted(set(paths)):
        if not raw:
            continue
        path = root / raw.decode()
        digest.update(raw)
        digest.update(path.read_bytes() if path.is_file() else b"<missing>")
    return digest.hexdigest()


def games_running():
    # Linux comm truncates long names; executable basename is authoritative.
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "exe").readlink().name.removesuffix(" (deleted)") == "TransportFever2":
                return True
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return False


def pass_once(root, games, state, run=subprocess.run, running=games_running):
    current = source_digest(root)
    out = root / "dist/linux-auto"
    version = (root / "installer/VERSION").read_text().strip()
    installer = out / ("tpf2mp-linux-" + version) / "install.sh"
    if state.get("built") != current or not installer.is_file():
        run([str(root / "tools/linux/build_release.sh"), "--out", str(out)], check=True)
        # Never install a package built while its inputs changed.
        if source_digest(root) != current:
            state.pop("built", None)
            return
        state["built"] = current
    if running():
        return
    for game in games or [None]:
        key = str(game) if game else "auto-detected"
        if state.get(key) == current:
            continue
        # Recheck for each install; install.sh checks again and never gets --force.
        if running():
            return
        args = [str(installer)] + (["--game", str(game)] if game else [])
        run(args, check=True)
        state[key] = current


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, action="append", default=[])
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--poll-seconds", type=float, default=5)
    args = parser.parse_args()
    if args.poll_seconds <= 0:
        parser.error("--poll-seconds must be positive")
    state = {}
    while True:
        try:
            pass_once(ROOT, args.game, state)
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"auto-install: {error}", flush=True)
            if args.once:
                return 1
        if args.once:
            return 0
        time.sleep(args.poll_seconds)


if __name__ == "__main__":
    raise SystemExit(main())
