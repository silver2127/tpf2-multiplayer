"""The action map, generated: every CommandList::Add call site joined with the
make_cmd factory that site also calls, and with the source file it lives in.

Rationale: every player action in this engine ends in the same two lines --
build a Command with a make_cmd::X factory, hand it to CommandList::Add. So the
pair (source file, factory) at a single call site IS the action's identity, and
enumerating the 80-odd Add call sites enumerates the actions. No decompiling,
no runtime probing, no guessing which of 138k functions to look at.

The factory table below is fixed by __FUNCSIG__ strings for the 19 factories
that carry an assert; the rest are pinned by their strictly alphabetical address
order inside make_command.cpp, which the 19 known ones establish and which the
decompiled call sites then confirm (e.g. TownBuilder really does call the slot
predicted to be CreateTowns).

Usage: cmdmap.py
"""
import bisect
import collections
import csv
import os

OUT = r"C:\tools\ghidra_out"
csv.field_size_limit(10 ** 7)

ADD = 0x9D2A00  # CommandList::Add

FACTORY = {
    0x9dc5e0: "Book",
    0x9dc750: "BuildProposal*",
    0x9dca00: "BuyVehicle",
    0x9dcbf0: "ConnectTownsAndIndustries*",
    0x9dcde0: "CreateLine",
    0x9dd0b0: "CreateTowns*",
    0x9dd190: "DeleteLine",
    0x9dd290: "DevelopTown*",
    0x9dd2e0: "InstantlyUpdateTownCargoNeeds*",
    0x9dd820: "RemoveField",
    0x9dd920: "RemoveTown",
    0x9dda20: "ReplaceTerrain*",
    0x9dddb0: "ReplaceVehicle",
    0x9ddfe0: "Reverse",
    0x9de0e0: "SaveGame*",
    0x9de380: "SellVehicle",
    0x9de490: "SendScriptEvent*",
    0x9de6f0: "SendToDepot",
    0x9de7f0: "SetAnimalState*",
    0x9de870: "SetCalendarSpeed*",
    0x9de8a0: "SetColor",
    0x9de9b0: "SetDate*",
    0x9de9e0: "SetGameSpeed*",
    0x9dea10: "SetLine",
    0x9deb70: "SetName",
    0x9ded50: "SetNoCosts*",
    0x9ded80: "SetSimBuildingClosureTimeStamp",
    0x9dee80: "SetSimBuildingManualDevelopment",
    0x9def80: "SetTownInfo*",
    0x9df070: "SetUserStopped",
    0x9df170: "SetVehicleManualDeparture",
    0x9df270: "SetVehicleShouldDepart*",
    0x9df340: "SetVehicleTargetMaintenanceState",
    0x9df480: "SpawnAnimal*",
    0x9df4e0: "UpdateLine",
    0x9df710: "UpdateLogo*",
}
# '*' marks a name pinned by alphabetical position, not by its own __FUNCSIG__.

cpp = {}
for r in csv.DictReader(open(os.path.join(OUT, "func2src.csv"))):
    fs = [f for f in r["source_files"].split(";") if f.endswith(".cpp")]
    if len(fs) == 1:
        cpp[int(r["func_rva"], 16)] = fs[0]
ck = sorted(cpp)


def src(v):
    i = bisect.bisect_right(ck, v) - 1
    if i < 0:
        return "?"
    return ("" if v in cpp else "~") + cpp[ck[i]]


uses = collections.defaultdict(set)
adders = set()
for r in csv.reader(open(os.path.join(OUT, "call_edges.csv"))):
    if r[0] == "caller_rva":
        continue
    ca, ce = int(r[0], 16), int(r[1], 16)
    if ce == ADD:
        adders.add(ca)
    if ce in FACTORY:
        uses[ca].add(FACTORY[ce])

rows = sorted((src(a), a, sorted(uses.get(a, ["<factory not a direct call>"])))
              for a in adders)
cur = None
for s, a, f in rows:
    if s != cur:
        print("\n### %s" % s)
        cur = s
    print("   %-8x  %s" % (a, ", ".join(f)))

orphans = sorted(set(uses) - adders)
print("\n### factory called WITHOUT CommandList::Add at the same site")
for a in orphans:
    print("   %-8x  %-46s %s" % (a, src(a), ", ".join(sorted(uses[a]))))
