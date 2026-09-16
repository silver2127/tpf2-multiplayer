#!/usr/bin/env python3
"""Verify that Linux ships the exact Lua sources from Windows release 0.5.5."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
REFERENCE = "ecbaf1d8752f7636d88ebc13d0d3084dd3977996"
PREFIX = "mod/mp_lockstep_1/"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mod-dir", type=Path, default=REPO / PREFIX)
    args = parser.parse_args()
    try:
        def git(*arguments):
            return subprocess.check_output(["git", "-C", str(REPO), *arguments])
        commit = git("rev-parse", REFERENCE + "^{commit}").decode().strip()
        paths = git("ls-tree", "-r", "--name-only", commit, "--", PREFIX).decode().splitlines()
        expected = {p[len(PREFIX):]: git("show", commit + ":" + p)
                    for p in paths if p.endswith(".lua")}
        actual = {p.relative_to(args.mod_dir).as_posix(): p.read_bytes()
                  for p in args.mod_dir.rglob("*.lua")}
        missing = sorted(expected.keys() - actual.keys())
        extra = sorted(actual.keys() - expected.keys())
        different = sorted(p for p in expected.keys() & actual.keys() if expected[p] != actual[p])
        for title, names in (("missing", missing), ("extra", extra), ("changed", different)):
            for name in names:
                print(f"FAIL: {title} Lua file: {name}", file=sys.stderr)
        if missing or extra or different:
            return 1
        manifest = "".join(f"{hashlib.sha256(expected[p]).hexdigest()}  {p}\n" for p in sorted(expected))
        print(f"PASS: {len(expected)} Lua files match Windows 0.5.5 ({commit}) byte for byte")
        print("Lua manifest sha256: " + hashlib.sha256(manifest.encode()).hexdigest())
        return 0
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Cannot verify Windows Lua baseline: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
