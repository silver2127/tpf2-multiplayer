#!/usr/bin/env python3
"""Desktop entry point: keep one lab instance per actor and retain launch output."""
import argparse
import datetime
import fcntl
from pathlib import Path
import shutil
import subprocess


def notify(message):
    print(message, flush=True)
    if shutil.which("notify-send"):
        subprocess.run(["notify-send", "Transport Fever 2 test lab", message], check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("actor", choices=("native", "proton"))
    parser.add_argument("--root", type=Path, default=Path.home() / ".local/share/tpf2mp-lab")
    args = parser.parse_args()
    root = args.root.expanduser().resolve()
    actor = root / args.actor
    if not (root / "lab.json").is_file():
        notify(f"The test lab is not configured at {root}")
        return 1
    with (actor / "launch.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            notify(f"The {args.actor} test instance is already running.")
            return 0
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        log_path = actor / "logs" / f"launch-{stamp}.log"
        with log_path.open("x") as log:
            latest = actor / "logs/latest-launch.log"
            latest.unlink(missing_ok=True)
            latest.symlink_to(log_path.name)
            command = [str(Path(__file__).resolve().with_name("tpf2mp-lab")),
                       "run", args.actor, "--root", str(root)]
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
        if result.returncode:
            notify(f"The {args.actor} test instance exited with code {result.returncode}. Log: {log_path}")
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
