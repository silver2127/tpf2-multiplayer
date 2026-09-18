#!/usr/bin/env python3
"""Set Proton for the game in the Steam client's config.vdf (the client must be stopped).

    python3 steam_compat.py [--tool proton_9] [--appid 1066780] [config.vdf]

Steam Play is off for "other titles" on a fresh Linux client, so a Windows-only game
cannot even be installed; the client stores that choice as CompatToolMapping under
InstallConfigStore/Software/Valve/Steam. "0" is the default for every title, the app
id the game's own. Idempotent; keeps everything else byte for byte.
"""
import os
import re
import sys

tool, appid = "proton_9", "1066780"
args = sys.argv[1:]
while args and args[0].startswith("--"):
    k = args.pop(0)
    if k == "--tool":
        tool = args.pop(0)
    elif k == "--appid":
        appid = args.pop(0)
path = args[0] if args else os.path.expanduser("~/.steam/steam/config/config.vdf")
t = open(path, encoding="utf-8").read()

def entry(key):
    return '\t\t\t\t\t"%s"\n\t\t\t\t\t{\n\t\t\t\t\t\t"name"\t\t"%s"\n\t\t\t\t\t\t"config"\t\t""\n\t\t\t\t\t\t"priority"\t\t"%s"\n\t\t\t\t\t}\n' % (key, tool, "75" if key == "0" else "250")

m = re.search(r'\n(\t*)"CompatToolMapping"\n\t*\{\n', t)
if m:
    start = m.start(0) + 1
    depth, i = 0, t.index("{", m.end(0) - 2)
    j = i
    while True:                       # the matching brace of the mapping block
        c = t[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    block = t[start:j + 1]
    for key in ("0", appid):
        if re.search(r'"%s"\s*\{' % key, block):
            block = re.sub(r'("%s"\s*\{[^}]*"name"\s*")[^"]*(")' % key, lambda mm: mm.group(1) + tool + mm.group(2), block)
        else:
            block = block[:block.rstrip().rfind("}")] + entry(key) + block[block.rstrip().rfind("}"):]
    t = t[:start] + block + t[j + 1:]
else:
    anchor = re.search(r'("Steam"\n\t{3}\{\n)', t)
    if not anchor:
        sys.exit("InstallConfigStore/Software/Valve/Steam not found in " + path)
    block = '\t\t\t\t"CompatToolMapping"\n\t\t\t\t{\n' + entry("0") + entry(appid) + '\t\t\t\t}\n'
    t = t[:anchor.end()] + block + t[anchor.end():]
open(path, "w", encoding="utf-8").write(t)
print("CompatToolMapping: default and", appid, "->", tool, "in", path)
