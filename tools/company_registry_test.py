"""Offline check (Lua 5.2): the companies registry (mp/companies.lua, rewrite
2026-09-27) gives every machine the same answer.

Each "machine" is its own Lua state running the real companies.lua and
shared_infra.lua against a small fake engine: player entities with a balance and
a loan, entities with an owner, addPlayer, setPlayer (the 0x60000000-tagged
entity-only form too), bookJournalEntry, getEntities. The machines load the same
world and apply the same company commands in the same order, the way lockstep
does -- every command crosses the wire in net.lua's text form first -- and the
test compares what each machine ends up with: which company each player plays,
and what each COMPANY owns and has in its wallet.

Covered: a fresh two-company game whatever order the joins land in; the
2026-09-27 report (an older save loaded with the other player hosting: the host
got the empty company, the joiner the starting money and every vehicle); two
companies created at the same stamp; passwords (v1 and v2); delete with a chosen
company taking over everything; attribution at the stamp; the v2 save round trip
and a hot join; a player known by name getting a Steam id; colours and vehicle
paint; the perms file for the slice; the dash lines the menu reads; co-op going
separate; the action hold before a game has joined.

    python tools/company_registry_test.py
"""
import os

from company_harness import (check, fails, Machine, Session, encode, to_lua, from_lua, lupa,
                             TOWN, same_company_state)


# ===========================================================================
# 1. a fresh two-company game: the friend's join lands first
# ===========================================================================
ROSTER = {"a": "Kaguya", "b": "Friend"}
for order in (["b", "a"], ["a", "b"]):
    A = Machine("a", "7656100000000001", "Kaguya", ROSTER, chip=1)
    B = Machine("b", "7656100000000002", "Friend", ROSTER, chip=2)
    for mach in (A, B):
        mach.world({100: {"balance": 5000000}}, TOWN, 100)
        mach.boot()
        mach.join()
    s = Session([A, B])
    s.pump(order)
    tag = "fresh game, joins " + "/".join(order)
    check(f"{tag}: the host plays company 1", A.cm().cmMyCompany == 1 and A.T.companyOf("a") == 1, str(A.cm().cmMyCompany))
    check(f"{tag}: the friend plays company 2", B.cm().cmMyCompany == 2 and B.T.companyOf("b") == 2, str(B.cm().cmMyCompany))
    check(f"{tag}: the starting money is company 1's", A.holdings(1)[1] == 5000000 and B.holdings(1)[1] == 5000000,
          f"{A.holdings(1)} {B.holdings(1)}")
    check(f"{tag}: the host's own player holds it", A.T.W.players[100].balance == 5000000)
    check(f"{tag}: the friend's own player is company 2 (empty)", B.T.W.players[100].balance == 0)
    same_company_state(s, tag, [1, 2])

# ===========================================================================
# 2. the 2026-09-27 report: an older save, the other player hosting now
# ===========================================================================
# Last session Friend hosted (a) and founded company 1; Kaguya joined (b) with
# company 2. This is KAGUYA's save: her machine's human entity 100 held company 2
# (after her switch), company 1 was the addPlayer entity 901.
V1 = {"v": 1, "mode": "companies", "mine": 2, "roster": [1, 2], "origin": {"a": 1, "b": 2},
      "pid": {"1": 901, "2": 100}, "pw": {}, "names": {}, "open": {}, "founded": {}, "foundedCount": {}}
WORLD_PLAYERS = {100: {"balance": 0, "loan": 0}, 901: {"balance": 5000000, "loan": 0}}
WORLD_ENTS = {1: {"kind": "CONSTRUCTION"}, 5001: {"kind": "VEHICLE", "owner": 100}, 5002: {"kind": "VEHICLE", "owner": 100},   # Kaguya's buses
              6001: {"kind": "VEHICLE", "owner": 901}, 6002: {"kind": "CONSTRUCTION", "owner": 901}}  # Friend's
for order in (["b", "a"], ["a", "b"]):
    A = Machine("a", "7656100000000001", "Kaguya", ROSTER, chip=1)   # Kaguya hosts now: letter a
    B = Machine("b", "7656100000000002", "Friend", ROSTER, chip=2)
    for mach in (A, B):
        mach.world(WORLD_PLAYERS, WORLD_ENTS, 100)
        mach.load(V1)
        mach.boot()
        mach.join()
    s = Session([A, B])
    tag = "older save, other host, joins " + "/".join(order)
    check(f"{tag}: the menu says the save was imported", "older version" in A.T.parseDash().migrated)
    s.pump(order)
    check(f"{tag}: Kaguya (host) plays her company 2", A.T.companyOf("a") == 2 and A.cm().cmMyCompany == 2, f"co{A.T.companyOf('a')}")
    check(f"{tag}: Friend plays his company 1", B.T.companyOf("b") == 1 and B.cm().cmMyCompany == 1, f"co{B.T.companyOf('b')}")
    check(f"{tag}: Kaguya's machine: her buses are hers", A.holdings(2)[0] == "5001,5002", A.holdings(2)[0])
    check(f"{tag}: Friend's machine: his money and his vehicle are his", B.holdings(1)[0] == "6001,6002" and B.holdings(1)[1] == 5000000,
          str(B.holdings(1)))
    check(f"{tag}: Friend's human entity now holds his company", B.T.W.players[100].balance == 5000000)
    same_company_state(s, tag, [1, 2])
    check(f"{tag}: once every company has its player the import is done and the note goes",
          A.T.parseDash().migrated == "" and B.T.parseDash().migrated == "" and A.cm().co.legacy is None and B.cm().co.legacy is None,
          repr(A.T.parseDash().migrated))
    check(f"{tag}: ... and later saves no longer carry it", "legacy" not in str(dict(A.cm().cmSaveState() or {})))
    check(f"{tag}: the claimed companies are named after their players",
          A.cm().cmNameOf(2) == "Kaguya's company" and B.cm().cmNameOf(1) == "Friend's company", f"{A.cm().cmNameOf(2)} / {B.cm().cmNameOf(1)}")

# the old rule, for contrast: a v1 save WITHOUT the saver's hint would have swapped
# (not reproducible any more -- the host now claims `mine`); the same host as the
# save keeps everything as it was:
A = Machine("a", "7656100000000002", "Friend", {"a": "Friend", "b": "Kaguya"}, chip=1)
B = Machine("b", "7656100000000001", "Kaguya", {"a": "Friend", "b": "Kaguya"}, chip=2)
V1f = dict(V1, mine=1, pid={"1": 100, "2": 901})     # Friend's own save: his human held company 1
WP = {100: {"balance": 5000000}, 901: {"balance": 0}}
WE = {1: {"kind": "CONSTRUCTION"}, 5001: {"kind": "VEHICLE", "owner": 901}, 6001: {"kind": "VEHICLE", "owner": 100}}
for mach in (A, B):
    mach.world(WP, WE, 100)
    mach.load(V1f)
    mach.boot()
    mach.join()
s = Session([A, B])
s.pump(["b", "a"])
check("older save, same host: Friend keeps company 1", A.T.companyOf("a") == 1 and A.cm().cmMyCompany == 1)
check("older save, same host: Kaguya keeps company 2", B.T.companyOf("b") == 2 and B.cm().cmMyCompany == 2)
same_company_state(s, "older save, same host", [1, 2])

# ===========================================================================
# 3. two companies created at the same stamp
# ===========================================================================
A = Machine("a", "7656100000000001", "Kaguya", ROSTER, chip=1)
B = Machine("b", "7656100000000002", "Friend", ROSTER, chip=2)
for mach in (A, B):
    mach.world({100: {"balance": 5000000}}, TOWN, 100)
    mach.boot()
    mach.join()
s = Session([A, B])
s.pump(["a", "b"])
s.request(A, "CMNEW 5 1 Alpha")
s.request(B, "CMNEW 6 0 Beta")
s.pump(["a", "b"])
ca, cb = A.T.companyOf("a"), A.T.companyOf("b")
check("simultaneous new companies get different ids", ca != cb and ca not in (1, 2) and cb not in (1, 2), f"{ca} {cb}")
check("... each creator plays their own", A.cm().cmMyCompany == ca and B.cm().cmMyCompany == cb, f"{A.cm().cmMyCompany} {B.cm().cmMyCompany}")
check("... with the colour and paint asked for", A.cm().co.list[ca].color == 5 and A.cm().co.list[cb].paint is False)
check("... named as asked", A.cm().cmNameOf(ca) == "Alpha" and B.cm().cmNameOf(cb) == "Beta")
same_company_state(s, "after two new companies", [1, 2, ca, cb])

# ===========================================================================
# 4. passwords, sharing, attribution at the stamp
# ===========================================================================
s.request(B, f"CMPW {cb} sesame")
s.pump()
s.request(A, f"CMSWITCH {cb}")
s.pump()
check("a locked company refuses a switch without its password", A.T.companyOf("a") == ca)
s.request(A, f"CMSWITCH {cb} wrong")
s.pump()
check("... and with the wrong one", A.T.companyOf("a") == ca)
s.request(A, f"CMSWITCH {cb} sesame")
s.pump()
check("... and opens with the right one: both play it", A.T.companyOf("a") == cb and A.cm().cmMyCompany == cb and B.T.companyOf("b") == cb)
c = to_lua(A.L, encode({"op": "VBUY", "origin": "a", "company": ca}))
A.T.apply(c)
check("a command stamped with its old company is attributed at the stamp", c.company == cb, str(c.company))
# a v1 password (cid-salted) still opens
A.cm().co.list[ca].pw = A.cm().cmHashPwV1(ca, "old")
B.cm().co.list[ca].pw = B.cm().cmHashPwV1(ca, "old")
A.cm().co.list[ca].pwv = 1
B.cm().co.list[ca].pwv = 1
s.request(A, f"CMSWITCH {ca} old")
s.pump()
check("a password from an older save still opens its company", A.T.companyOf("a") == ca, str(A.T.companyOf("a")))

# ===========================================================================
# 5. delete, with the company that takes over chosen
# ===========================================================================
# give company 2 something on both machines: a vehicle and money
for mach in (A, B):
    pid2 = mach.cm().cmCompanyPid[2]
    mach.T.W.ents[7001] = mach.L.table_from({"kind": "VEHICLE", "owner": pid2})
    mach.T.W.players[pid2].balance = 1234
s.request(A, f"CMDEL 2 {ca}")
s.pump()
check("delete moves the company's assets to the chosen company", "7001" in A.holdings(ca)[0].split(",") and "7001" in B.holdings(ca)[0].split(","),
      f"{A.holdings(ca)} {B.holdings(ca)}")
check("... and its money", A.holdings(ca)[1] >= 1234)
check("... and the company is gone everywhere", A.cm().cmCo(2) is None and B.cm().cmCo(2) is None)
same_company_state(s, "after the delete", [1, ca, cb])
s.request(A, f"CMDEL {cb} {ca} sesame")
s.pump()
check("a company somebody plays cannot be deleted", A.cm().cmCo(cb) is not None and "playing it" in str(A.cm().cmLastNote), str(A.cm().cmLastNote))
s.request(B, f"CMSWITCH 1")
s.pump()
s.request(A, f"CMDEL {cb} 1")
s.pump()
check("a locked company is not deleted without its password", A.cm().cmCo(cb) is not None, str(A.cm().cmLastNote))
s.request(A, f"CMDEL {cb} 1 sesame")
s.pump()
check("... with it, it is, into company 1", A.cm().cmCo(cb) is None and B.cm().cmCo(cb) is None, str(A.cm().cmLastNote))

# ===========================================================================
# 6. colours, vehicle paint, the slice's file, the menu's view
# ===========================================================================
s.request(A, f"CMCOLOR {ca} 9 1")
s.pump()
check("a colour change with paint on repaints the company's vehicles", any(x.op == "color" for x in A.T.W.cmds.values()))
check("... and the registry has the colour", A.cm().cmColorOf(ca) == 9 and B.cm().cmColorOf(ca) == 9)
A.cm().cmWritePerms()
perms = open(os.path.join(A.dir, "mp_company_perms.txt")).read()
check("the perms file carries each company's palette index", f"pid {A.cm().cmCompanyPid[ca]} {ca} 9" in perms, perms)
check("... and this game's own player entity", "me 100" in perms, perms)
st = A.T.parseDash()
check("the menu reads every company from the dash", len(list(st.list.values())) == len(A.cm().cmIds()), str(len(list(st.list.values()))))
check("... its own first", st.list[1].cid == st.mine == ca, f"{st.list[1].cid} {st.mine}")
check("... with colour and name", st.byId[ca].color == 9 and st.byId[ca].name == "Alpha")

# ===========================================================================
# 7. the save: round trip, and a hot join into the session that saved
# ===========================================================================
rec = from_lua(A.cm().cmSaveState())
C = Machine("c", "7656100000000003", "Cid", {"a": "Kaguya", "b": "Friend", "c": "Cid"}, chip=3)
C.world({}, TOWN, 100)
C.T.W.human = A.cm().cmCompanyPid[A.cm().cmMyCompany]    # the save's human entity
for pid, p in A.T.W.players.items():
    C.T.W.players[pid] = C.L.table_from({"balance": p.balance, "loan": p.loan})
C.T.CM.cmLoadState(to_lua(C.L, rec))
C.boot()
check("a hot joiner loads the registry", sorted(C.cm().cmIds().values()) == sorted(A.cm().cmIds().values()))
check("... and keeps who plays what (same players on the same letters)", C.T.companyOf("a") == A.T.companyOf("a") and C.T.companyOf("b") == A.T.companyOf("b"))
D = Machine("a", "7656100000000009", "Zed", {"a": "Zed", "b": "Friend"}, chip=1)
D.world({}, TOWN, C.T.W.human)
D.T.CM.cmLoadState(to_lua(D.L, rec))
D.boot()
check("a new session (another player on letter a) forgets the old letter", D.T.companyOf("a") is None and D.T.companyOf("b") == A.T.companyOf("b"))

# ===========================================================================
# 8. a player known by name gets a Steam id; co-op going separate; the hold
# ===========================================================================
E = Machine("a", None, "Bob", {"a": "Bob", "b": "Ann"}, lobby_mode="coop", chip=1)
F = Machine("b", None, "Ann", {"a": "Bob", "b": "Ann"}, lobby_mode="coop", chip=1)
for mach in (E, F):
    mach.world({100: {"balance": 5000000}}, TOWN, 100)
    mach.boot()
check("before its join a live game holds its actions", E.cm().cmHoldActions() is True)
for mach in (E, F):
    mach.join()
s = Session([E, F])
s.pump()
check("after it, they go out", E.cm().cmHoldActions() is False)
check("co-op: everyone shares company 1", E.T.companyOf("a") == 1 and E.T.companyOf("b") == 1 and E.cm().cmMode == "coop")
check("co-op: nobody's player entity moved", E.T.W.players[100].balance == 5000000 and F.T.W.players[100].balance == 5000000)
s.request(F, "CMNEW 4 1 Annco")
s.pump()
check("a new company in co-op makes the session separate", E.cm().cmMode == "companies" and F.cm().cmMode == "companies")
check("... Ann plays her new company, Bob still company 1", F.T.companyOf("b") == F.cm().cmMyCompany != 1 and E.T.companyOf("a") == 1)
same_company_state(s, "co-op gone separate", [1, F.cm().cmMyCompany])
check("co-op players are keyed by name without Steam", E.cm().co.members["n:Bob"] == 1)
# Bob comes back with Steam
G = Machine("a", "7656100000000077", "Bob", {"a": "Bob", "b": "Ann"}, chip=1)
G.world({100: {"balance": 0}}, TOWN, 100)
G.T.CM.cmLoadState(to_lua(G.L, from_lua(E.cm().cmSaveState())))
G.boot()
G.join()
Session([G]).pump()
check("the same name with a Steam id is the same player", G.cm().co.members["s:7656100000000077"] == 1 and G.cm().co.members["n:Bob"] is None)
# ... and back: Bob's Steam id unreadable next time (an empty tpf2_steam.txt)
H = Machine("a", None, "Bob", {"a": "Bob", "b": "Ann"}, chip=1)
H.world({100: {"balance": 0}}, TOWN, 100)
H.T.CM.cmLoadState(to_lua(H.L, from_lua(G.cm().cmSaveState())))
H.boot()
H.join()
Session([H]).pump()
check("a known Steam player without their id this time is still that player", H.T.companyOf("a") == 1
      and H.cm().co.origin["a"] == "s:7656100000000077" and H.cm().co.members["n:Bob"] is None, str(H.cm().co.origin["a"]))

# ===========================================================================
# 8b. starting capital: every new company starts like company 1 did
# ===========================================================================
for order in (["b", "a"], ["a", "b"]):
    A = Machine("a", "7656100000000001", "Kaguya", ROSTER, chip=1)
    B = Machine("b", "7656100000000002", "Friend", ROSTER, chip=2)
    for m in (A, B):
        m.world({100: {"balance": 10000000, "loan": 10000000}}, TOWN, 100)   # a new game: the money is borrowed
        m.boot()
        m.join()
    s = Session([A, B])
    s.pump(order)
    tag = "starting capital, joins " + "/".join(order)
    check(f"{tag}: company 1 keeps the world's money", A.holdings(1)[1:3] == (10000000, 10000000), str(A.holdings(1)))
    check(f"{tag}: company 2 starts with the same loan and cash", A.holdings(2)[1:3] == (10000000, 10000000)
          and B.holdings(2)[1:3] == (10000000, 10000000), f"{A.holdings(2)} {B.holdings(2)}")
    same_company_state(s, tag, [1, 2])
    s.request(A, "CMNEW 0 1 Third")
    s.pump()
    third = A.cm().cmMyCompany
    check(f"{tag}: a company founded later starts the same way", A.holdings(third)[1:3] == (10000000, 10000000)
          and B.holdings(third)[1:3] == (10000000, 10000000), f"{A.holdings(third)} {B.holdings(third)}")
    rec = from_lua(A.cm().cmSaveState())
    check(f"{tag}: the starting loan rides in the save", rec.get("start", {}).get("l") == 10000000, str(rec.get("start")))

# ===========================================================================
# 9. one Steam account in two games (a local test with a sandbox)
# ===========================================================================
SAME = "7656100000000042"
R2 = {"a": "Host", "b": "Box"}
for order in (["a", "b"], ["b", "a"]):
    P = Machine("a", SAME, "Host", R2, chip=1)
    Q = Machine("b", SAME, "Box", R2, chip=2)
    for m in (P, Q):
        m.world({100: {"balance": 5000000}}, TOWN, 100)
        m.boot()
        m.join()
    s = Session([P, Q])
    s.pump(order)
    tag = "one Steam account, joins " + "/".join(order)
    check(f"{tag}: two players, two companies", P.T.companyOf("a") == 1 and P.T.companyOf("b") == 2 and Q.T.companyOf("b") == 2,
          f"{P.T.companyOf('a')} {P.T.companyOf('b')}")
    check(f"{tag}: both stay in the session", P.cm().co.origin["a"] is not None and P.cm().co.origin["b"] is not None)
    for m in (P, Q):
        m.cm().ticks = 100000          # long after the join: no second CMJOIN goes out
        m.join()
    check(f"{tag}: nobody sends their join again", len(list(P.T.sent.values())) == 0 and len(list(Q.T.sent.values())) == 0,
          f"{len(list(P.T.sent.values()))} {len(list(Q.T.sent.values()))}")
    # reload in the other order: everyone keeps their company
    rec = from_lua(P.cm().cmSaveState())
    P2 = Machine("a", SAME, "Host", R2, chip=1)
    Q2 = Machine("b", SAME, "Box", R2, chip=2)
    for m in (P2, Q2):
        m.world({100: {"balance": 0}}, TOWN, 100)
        m.T.CM.cmLoadState(to_lua(m.L, rec))
        m.boot()
        m.join()
    Session([P2, Q2]).pump(list(reversed(order)))
    check(f"{tag}: after a reload joined the other way round, still the same companies",
          P2.T.companyOf("a") == 1 and P2.T.companyOf("b") == 2, f"{P2.T.companyOf('a')} {P2.T.companyOf('b')}")
# a lone player who changed their lobby name is still themselves
S1 = Machine("a", SAME, "Old", {"a": "Old"}, chip=1)
S1.world({100: {"balance": 0}}, TOWN, 100)
S1.boot(); S1.join(); Session([S1]).pump()
rec = from_lua(S1.cm().cmSaveState())
S2 = Machine("a", SAME, "New", {"a": "New"}, chip=1)
S2.world({100: {"balance": 0}}, TOWN, 100)
S2.T.CM.cmLoadState(to_lua(S2.L, rec)); S2.boot(); S2.join(); Session([S2]).pump()
check("a renamed player keeps their Steam entry", S2.cm().co.origin["a"] == "s:" + SAME and S2.T.companyOf("a") == 1, str(S2.cm().co.origin["a"]))

print("FAILED: " + ", ".join(fails) if fails else "ALL PASS: every machine agrees on the companies")
raise SystemExit(1 if fails else 0)
