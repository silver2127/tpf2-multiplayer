"""Loopback test of the relay-only host (python tools/relay_selftest.py): relay + joiners on 127.0.0.1.
The first joiner (leader) sends start(save=<file>); the relay must receive the
upload, push it to the second joiner, and both must get start save=true."""
import json, os, subprocess, sys, time, tempfile, shutil
NP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "netpunch")
tmp = tempfile.mkdtemp(prefix="relaytest_")
def d(n):
    p = os.path.join(tmp, n); os.makedirs(p, exist_ok=True); return p
save = os.path.join(tmp, "world.sav"); open(save, "wb").write(os.urandom(3 * 1024 * 1024))
open(save + ".lua", "w").write("return {}\n")
procs = []
def run(args, iodir, name):
    lg = open(os.path.join(tmp, name + ".log"), "w")
    p = subprocess.Popen([sys.executable, "lobby.py"] + args + ["--io-dir", iodir], cwd=NP, stdout=lg, stderr=subprocess.STDOUT)
    procs.append(p); return p
def events(iodir):
    out = []
    try:
        for ln in open(os.path.join(iodir, "lobby_out.jsonl"), encoding="utf-8"):
            ln = ln.strip()
            if ln:
                try: out.append(json.loads(ln))
                except ValueError: pass
    except FileNotFoundError: pass
    return out
def wait(pred, t=20):
    end = time.time() + t
    while time.time() < end:
        if pred(): return True
        time.sleep(0.2)
    return pred()
try:
    rd = d("relay"); run(["host", "--relay-only", "--name", "Relay", "--lobby-name", "Relay Test", "--local-port", "29571"], rd, "relay")
    assert wait(lambda: any(e.get("type") == "code" for e in events(rd)), 40), "no code"
    code = [e for e in events(rd) if e.get("type") == "code"][0]["code"]
    print("code", code[:12] + "...")
    ad = d("a"); run(["join", code, "--name", "Alice", "--local-port", "0", "--no-mesh"], ad, "alice")
    assert wait(lambda: any(e.get("type") == "roster" and "Alice" in e.get("players", []) for e in events(ad)), 40), "alice not in roster"
    bd = d("b"); run(["join", code, "--name", "Bob", "--local-port", "0", "--no-mesh"], bd, "bob")
    assert wait(lambda: any(e.get("type") == "roster" and "Bob" in e.get("players", []) for e in events(ad)), 40), "bob not in roster"
    ra = [e for e in events(ad) if e.get("type") == "roster"][-1]
    print("alice roster:", ra)
    assert ra.get("host") == "Alice" and ra.get("relay") is True, "alice should lead"
    assert ra.get("letters", {}).get("Alice") == "a" and ra.get("letters", {}).get("Bob") == "b", "letters"
    # the leader starts with a save
    with open(os.path.join(ad, "lobby_in.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps({"cmd": "start", "save": save}) + "\n")
    assert wait(lambda: any(e.get("type") == "save_ready" for e in events(bd)), 60), "bob never got the save"
    assert wait(lambda: any(e.get("type") == "start" and e.get("save") is True for e in events(bd)), 30), "bob no start"
    assert wait(lambda: any(e.get("type") == "start" and e.get("save") is True for e in events(ad)), 30), "alice no start"
    got = open(os.path.join(bd, "incoming_save.sav"), "rb").read()
    assert got == open(save, "rb").read(), "save bytes differ"
    # hot join: Carol arrives, Alice re-shares; only Carol should receive
    cd = d("c"); run(["join", code, "--name", "Carol", "--local-port", "0", "--no-mesh"], cd, "carol")
    assert wait(lambda: any(e.get("type") == "roster" and "Carol" in e.get("players", []) for e in events(ad)), 40), "carol not in roster"
    nb = len([e for e in events(bd) if e.get("type") == "save_ready"])
    with open(os.path.join(ad, "lobby_in.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps({"cmd": "start", "save": save}) + "\n")
    assert wait(lambda: any(e.get("type") == "save_ready" for e in events(cd)), 60), "carol never got the save"
    assert wait(lambda: any(e.get("type") == "start" for e in events(cd)), 30), "carol no start"
    time.sleep(2)
    assert len([e for e in events(bd) if e.get("type") == "save_ready"]) == nb, "bob re-received the save"
    rc = [e for e in events(cd) if e.get("type") == "roster"][-1]
    assert rc.get("letters", {}).get("Carol") == "c", rc
    # ---- restart the relay: same code (kept secret), letters remembered, /resume serves the stored save
    for pr in procs: pr.kill()
    time.sleep(1.5); procs.clear()
    open(os.path.join(rd, "lobby_out.jsonl"), "w").close()
    run(["host", "--relay-only", "--name", "Relay", "--lobby-name", "Relay Test", "--local-port", "29571"], rd, "relay2")
    assert wait(lambda: any(e.get("type") == "code" for e in events(rd)), 40), "no code after restart"
    code2 = [e for e in events(rd) if e.get("type") == "code"][0]["code"]
    print("old code still used below; new code differs only by its timestamp:", code2 != code)
    dd = d("dave"); run(["join", code, "--name", "Dave", "--local-port", "0", "--no-mesh"], dd, "dave")
    assert wait(lambda: any(e.get("type") == "roster" and "Dave" in e.get("players", []) for e in events(dd)), 40), "dave not in roster"
    rdv = [e for e in events(dd) if e.get("type") == "roster"][-1]
    assert rdv["host"] == "Dave" and rdv["letters"]["Dave"] == "d", rdv     # a,b,c are remembered for Alice/Bob/Carol
    assert wait(lambda: any(e.get("type") == "status" and "holds a save" in e.get("detail", "") for e in events(dd)), 10), "no stored-save hint"
    with open(os.path.join(dd, "lobby_in.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps({"cmd": "chat", "text": "/resume"}) + "\n")
    assert wait(lambda: any(e.get("type") == "save_ready" for e in events(dd)), 60), "dave never got the stored save"
    assert wait(lambda: any(e.get("type") == "start" and e.get("save") is True for e in events(dd)), 30), "dave no start"
    assert open(os.path.join(dd, "incoming_save.sav"), "rb").read() == open(save, "rb").read(), "resumed save differs"
    print("RELAY SELFTEST OK (incl. restart + /resume)")
finally:
    for p in procs:
        try: p.kill()
        except Exception: pass
    time.sleep(1)
    for n in ("relay", "relay2", "alice", "bob", "carol", "dave"):
        try:
            lines = open(os.path.join(tmp, n + ".log"), encoding="utf-8", errors="replace").read().splitlines()
            print("---", n, "(last 6)"); print("\n".join(lines[-6:]))
        except FileNotFoundError: pass
    shutil.rmtree(tmp, ignore_errors=True)
