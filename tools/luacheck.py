#!/usr/bin/env python3
"""Syntax-check the mod's Lua before deploying it.

A syntax error means the game silently skips the whole mod: no [mpb-*] lines,
no replication, and the only clue is an absence. Catching it here costs a
second instead of a five-minute launch cycle.

This is a PARSER, not an interpreter -- it catches syntax errors (unbalanced
end, bad statement), not runtime problems like a nil upvalue.

    python tools/luacheck.py                # check every .lua under mod/
    python tools/luacheck.py path/to/f.lua  # check specific files
"""
import re
import sys
import pathlib

# Lua 5.1 hard limit: a chunk's main function may declare at most 200 locals.
# The parser cannot see this (it is a COMPILER error), and it crashed both game
# instances at script load ("too many local variables") after a 19-local batch.
# Count real top-level local NAMES (multi-name declarations count each name).
LUA51_LOCAL_LIMIT = 200
def _toplevel_locals(tree):
    from luaparser import astnodes as _n
    c = 0
    for node in tree.body.body:
        if isinstance(node, _n.LocalAssign): c += len(node.targets)
        elif isinstance(node, _n.LocalFunction): c += 1
    return c


try:
    from luaparser import ast
except ImportError:
    sys.exit("luaparser not installed:  python -m pip install luaparser")

ROOT = pathlib.Path(__file__).resolve().parent.parent


_DEF = re.compile(r"^\s*local function ([A-Za-z_]\w*)\s*\(", re.M)
_CALL = re.compile(r"(?<![\w.:])([A-Za-z_]\w*)\s*\(")


def _blank_noncode(src: str) -> str:
    """Replace comments and string literals with spaces, keeping line numbers.

    Without this the scan reads prose: a comment saying "report() runs outside a
    pcall" looked like a real call site.
    """
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        # long comment / long string:  --[[ ]]  or  [[ ]]
        if src.startswith("--[[", i) or src.startswith("[[", i):
            j = src.find("]]", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        if src.startswith("--", i):                      # line comment
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if c in "\"'":                                   # quoted string
            q, j = c, i + 1
            while j < n and src[j] != q:
                if src[j] == "\\":
                    j += 1
                j += 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


def check_unterminated_strings(path: pathlib.Path, src: str) -> bool:
    """Flag a quoted string with a raw newline inside it.

    Lua 5.2 rejects this ("unfinished string"), but `luaparser` accepts it, so
    ast.parse() alone gives a false PASS and the break is only discovered when
    the game refuses to load the mod. That happened: a generated line came out
    as `f:write("go` + newline + `")`, luacheck said ok, and the game died with
    `mptest.lua:1362: unfinished string near '"go'`.
    """
    bad = []
    for lineno, line in enumerate(src.split("\n"), start=1):
        # walk the line, skipping comments, tracking quote state
        i, n, q = 0, len(line), None
        while i < n:
            c = line[i]
            if q is None:
                if line.startswith("--", i):
                    break                      # rest of line is a comment
                if c in "\"'":
                    q = c
            else:
                if c == "\\":
                    i += 1                     # skip the escaped character
                elif c == q:
                    q = None
            i += 1
        if q is not None:
            bad.append((lineno, line.strip()[:60]))
    for lineno, text in bad:
        print(f"FAIL {path}:{lineno}: unterminated {'string'} literal -- "
              f"Lua 5.2 rejects a raw newline inside quotes: {text}")
    return not bad


_LOCALVAR = re.compile(r"^\s*local\s+([A-Za-z_]\w*)\s*=", re.M)


def check_localvar_use_before_define(path: pathlib.Path, src: str) -> bool:
    """Flag reading a file-scope `local x = ...` before its declaration line.

    Same failure as calling a not-yet-declared local function, but for
    variables, and it slipped past the function-only check. Real case: a
    `local lastReplayTick` declared below pollEdges resolved to a nil global
    inside it, so `ticks - lastReplayTick` threw -- caught by a pcall, logged,
    and silently aborting every sweep that contained an edge removal. Removals
    stopped replicating and roads doubled up on the peer.

    Only file-scope declarations (no leading indentation) are considered, so
    block-locals and shadowing do not produce noise.
    """
    code = _blank_noncode(src)
    decl = {}
    for m in _LOCALVAR.finditer(code):
        if code[m.start():m.start() + 1] != "l":   # must be column 0
            continue
        decl.setdefault(m.group(1), code.count(chr(10), 0, m.start()) + 1)
    if not decl:
        return True

    bad = []
    for name, dline in decl.items():
        for m in re.finditer(r"(?<![\w.:])" + re.escape(name) + r"(?![\w])", code):
            line = code.count(chr(10), 0, m.start()) + 1
            if line < dline:
                bad.append((line, name, dline))
                break
    for line, name, dline in sorted(bad):
        print(f"FAIL {path}:{line}: reads '{name}' but it is a file-scope local "
              f"declared at line {dline} -- resolves to a nil global at runtime")
    return not bad


def check_use_before_define(path: pathlib.Path, src: str) -> bool:
    """Flag calling a `local function` declared LATER in the file.

    Lua resolves such a call to a *global* of that name, which is nil, so it
    fails only when the line actually runs -- in this project that means a
    ten-minute launch-and-load cycle to discover a typo-grade mistake. It cost
    exactly that once: actCreateLine called stationGroups(), defined 180 lines
    further down, and died with "attempt to call global 'stationGroups'".
    """
    code = _blank_noncode(src)
    defs = {}
    for m in _DEF.finditer(code):
        # first definition wins; a later redefinition does not help an earlier call
        defs.setdefault(m.group(1), src.count("\n", 0, m.start()) + 1)
    if not defs:
        return True

    bad = []
    for m in _CALL.finditer(code):
        name = m.group(1)
        if name not in defs:
            continue
        line = code.count("\n", 0, m.start()) + 1
        if line < defs[name]:
            # the definition line itself matches _CALL; skip it
            bad.append((line, name, defs[name]))
    for line, name, dline in bad:
        print(f"FAIL {path}:{line}: calls '{name}()' but it is a local function "
              f"defined at line {dline} -- resolves to a nil global at runtime")
    return not bad


def check(path: pathlib.Path) -> bool:
    src = path.read_text(encoding="utf-8", errors="replace")
    # A BOM makes the game's Lua parser fail on line 1; it has bitten this
    # project before via Set-Content -Encoding UTF8.
    if src.startswith("﻿"):
        print(f"FAIL {path}: file starts with a UTF-8 BOM -- strip it")
        return False
    try:
        tree = ast.parse(src)
        nloc = _toplevel_locals(tree)
        if nloc > LUA51_LOCAL_LIMIT:
            print(f"FAIL {path}: {nloc} top-level locals exceeds the Lua 5.1 limit of {LUA51_LOCAL_LIMIT} "
                  f"-- the game will crash at script load ('too many local variables'). Fold state into a table.")
            return False
    except Exception as e:  # luaparser raises several distinct types
        print(f"FAIL {path}: {type(e).__name__}: {e}")
        return False
    if not check_unterminated_strings(path, src):
        return False
    if not check_use_before_define(path, src):
        return False
    if not check_localvar_use_before_define(path, src):
        return False
    print(f"ok   {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")
    return True


def main() -> int:
    args = sys.argv[1:]
    if args:
        files = [pathlib.Path(a) for a in args]
    else:
        files = sorted((ROOT / "mod").rglob("*.lua"))
    if not files:
        print("no lua files found")
        return 1
    bad = [f for f in files if not check(f)]
    print(f"\n{len(files) - len(bad)}/{len(files)} parsed")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
