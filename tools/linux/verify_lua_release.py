#!/usr/bin/env python3
"""Verify the cumulative Linux Lua integration: Windows release 0.7.0.3 (dev 122a0ce9)."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
REFERENCE = "122a0ce966ae9ce9f737cbda112e8fcb68f9f472"
INCOMING = REFERENCE
INCOMING_FILES = set()
PREFIX = "mod/mp_lockstep_1/"
# Native cancelled rename/color records explicitly request origin replay.
MERGED_SHA256 = {"res/scripts/mp/inject.lua": "df0cf0bb41d30666a7e1c93083cedeea916eb4e0fe0c3983fd317ad9b8112bd2"}


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
        for p in INCOMING_FILES:
            expected[p] = git("show", INCOMING + ":" + PREFIX + p)
        actual = {p.relative_to(args.mod_dir).as_posix(): p.read_bytes()
                  for p in args.mod_dir.rglob("*.lua")}
        missing = sorted(expected.keys() - actual.keys())
        extra = sorted(actual.keys() - expected.keys())
        different = sorted(p for p in expected.keys() & actual.keys() if (hashlib.sha256(actual[p]).hexdigest() != MERGED_SHA256[p]
                            if p in MERGED_SHA256 else expected[p] != actual[p]))
        for title, names in (("missing", missing), ("extra", extra), ("changed", different)):
            for name in names:
                print(f"FAIL: {title} Lua file: {name}", file=sys.stderr)
        if missing or extra or different:
            return 1
        manifest = "".join(f"{hashlib.sha256(actual[p]).hexdigest()}  {p}\n" for p in sorted(expected))
        print(f"PASS: {len(expected)} Lua files: exact Windows 0.7.0.3 except {len(MERGED_SHA256)} pinned cumulative merges")
        print("Lua manifest sha256: " + hashlib.sha256(manifest.encode()).hexdigest())
        # The glyph overlays are runtime dependencies of the shared stylesheet.
        # Check packaged copies too: Lua equality alone cannot catch omitted assets.
        glyph_prefix = "res/textures/ui/hud/mp_glyph_"
        expected_glyphs = {p[len(PREFIX):]: git("show", commit + ":" + p)
                           for p in paths if p.startswith(PREFIX + glyph_prefix) and p.endswith(".tga")}
        actual_glyphs = {p.relative_to(args.mod_dir).as_posix(): p.read_bytes()
                         for p in (args.mod_dir / "res/textures/ui/hud").glob("mp_glyph_*.tga")}
        bad_glyphs = sorted(p for p in expected_glyphs.keys() | actual_glyphs.keys()
                            if expected_glyphs.get(p) != actual_glyphs.get(p))
        for p in bad_glyphs:
            print(f"FAIL: missing, extra or changed HUD glyph: {p}", file=sys.stderr)
        if bad_glyphs:
            return 1
        print(f"PASS: {len(expected_glyphs)} HUD glyph textures exact Windows 0.7.0.3")
        return 0
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Cannot verify Windows Lua baseline: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
