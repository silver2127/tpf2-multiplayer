#!/usr/bin/env python3
"""Supervise a native game child; never search for or kill unrelated games."""
import os
from pathlib import Path
import signal
import subprocess
import time


def command(env):
    game = Path(env["GAME_DIR"]).resolve()
    data = Path(env.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
    wrapper = data / "tpf2mp/tpf2mp-launch"
    for path in (game / "TransportFever2", game / "run.sh", wrapper):
        if not path.is_file():
            raise RuntimeError(f"Missing native installation file: {path}")
    return [str(wrapper), str(game / "run.sh")], game, data / "tpf2mp/data/tpf2_menu.log"


def stop(child):
    # run.sh can exit before the game or lobby. Reap the entire session even
    # when its leader has already exited; never discover targets by name.
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        child.wait()
        return
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        child.poll()
        try:
            os.killpg(child.pid, 0)
        except ProcessLookupError:
            break
        time.sleep(.1)
    else:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    child.wait()


def main():
    args, game, log = command(os.environ)
    stall = max(30, int(os.environ.get("STALL_SECONDS", "180")))
    stopping = False

    def shutdown(_signum, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    env = dict(os.environ, SteamAppId="1066780", SteamGameId="1066780")
    while not stopping:
        started = time.time()
        print(f"Starting native Transport Fever 2 in {game}", flush=True)
        child = subprocess.Popen(args, cwd=game, env=env, start_new_session=True)
        try:
            while not stopping and child.poll() is None:
                try:
                    modified = log.stat().st_mtime
                except FileNotFoundError:
                    modified = 0
                now = time.time()
                # Allow a full startup window even when an old log exists.
                if now - max(started, modified) >= stall:
                    print(f"Native game heartbeat silent for {stall}s; restarting", flush=True)
                    break
                time.sleep(1)
        finally:
            stop(child)
        for _ in range(15):
            if stopping:
                break
            time.sleep(1)


if __name__ == "__main__":
    main()
