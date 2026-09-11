"""Offline test of the hole-punch rendezvous (lobby.py RENDEZVOUS + masterserver.py /knock).

Runs the real master server on a loopback port, then:
  1. a joiner's knock reaches the host poller, which decodes the joiner's addresses
  2. a note sealed with another code's key, or garbage, is ignored
  3. the master refuses bad tags, oversized or malformed blobs, and floods (429)
  4. run_host fires HELLOs at a knocked address and stops once it expires

Real NAT traversal cannot be simulated here; this proves every piece the punch
depends on.

    python tools/rendezvous_test.py
"""
import json, os, socket, sys, tempfile, threading, time, urllib.error, urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "netpunch"))
import masterserver                                          # noqa: E402
import lobby                                                 # noqa: E402
from connect import encode_profile                           # noqa: E402
from punch import _unpack, TYPE_HELLO                        # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


logs = []
log = logs.append

masterserver.H.log_message = lambda *a, **k: None          # keep the request log out of the output
srv = masterserver.ThreadingHTTPServer(("127.0.0.1", 0), masterserver.H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
URL = f"http://127.0.0.1:{srv.server_address[1]}"

secret = os.urandom(12)
joiner_profile = {"candidates": {"lan_v4": "192.168.5.5:40000", "public_v4": "203.0.113.7:51000", "v6": None},
                  "flags": {"open": False, "symmetric": False, "cgnat": False, "v6": False}}
jcode = encode_profile(joiner_profile)

# ---- 1. knock -> host poller -> targets ----
host = lobby._RendezvousHost(URL, secret, "", log, poll_every=0.1)
knock = lobby._RendezvousKnock(URL, secret, "", jcode, log, every=0.2)
got = None
deadline = time.time() + 5
while time.time() < deadline and got is None:
    try:
        got = host.queue.get(timeout=0.2)
    except Exception:
        pass
knock.close()
check("the host receives the joiner's addresses", got is not None and ("203.0.113.7", 51000) in got
      and ("192.168.5.5", 40000) in got, str(got))
check("public address first", bool(got) and got[0] == ("203.0.113.7", 51000), str(got))
check("the joiner logged its first knock", any("knocked at the master" in l for l in logs))

# with a password the key changes: a passwordless host cannot open it
pw_host = lobby._RendezvousHost(URL, secret, "", log, poll_every=10)
pw_knock_sealer = lobby._rv_sealer(secret, "hunter2")
import base64                                                # noqa: E402
blob = base64.b64encode(pw_knock_sealer.seal(jcode.encode("ascii"))).decode("ascii")
check("a note sealed with a different password is ignored", pw_host.targets_from(blob) is None)
other = lobby._rv_sealer(os.urandom(12), "")
blob2 = base64.b64encode(other.seal(jcode.encode("ascii"))).decode("ascii")
check("a note sealed for another lobby is ignored", pw_host.targets_from(blob2) is None)
check("garbage is ignored", pw_host.targets_from("!!notbase64!!") is None and pw_host.targets_from("QUJD") is None)
pw_host.close()
check("different secrets give different tags", lobby._rv_tag(secret) != lobby._rv_tag(os.urandom(12))
      and len(lobby._rv_tag(secret)) == 24)


# ---- 3. master validation ----
def post(body):
    req = urllib.request.Request(URL + "/knock", data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def get(q):
    try:
        with urllib.request.urlopen(URL + "/knock" + q, timeout=3) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, None


check("bad tag refused", post({"s": "nothex", "blob": "QUJD"}) == 400)
check("oversized blob refused", post({"s": "a" * 24, "blob": "A" * 2000}) == 400)
check("non-base64 blob refused", post({"s": "a" * 24, "blob": "<script>"}) == 400)
check("GET with a bad tag refused", get("?s=zz")[0] == 400)
st, body = get("?s=" + "b" * 24)
check("GET an empty tag -> no knocks", st == 200 and body["knocks"] == [], str(body))
tag = "c" * 24
codes = [post({"s": tag, "blob": "QUJD"}) for _ in range(45)]
check("a flood from one address is cut off (429)", codes.count(200) <= masterserver.KNOCK_PER_IP_MIN and 429 in codes,
      f"{codes.count(200)} accepted")
st, body = get(f"?s={tag}&since=0")
check("at most KNOCK_PER_TAG notes kept per tag", st == 200 and len(body["knocks"]) <= masterserver.KNOCK_PER_TAG,
      str(len(body["knocks"]) if body else None))
last = max(k["t"] for k in body["knocks"])
st, body2 = get(f"?s={tag}&since={last}")
check("since= returns only newer notes", st == 200 and body2["knocks"] == [])
host.close()

# ---- 4. run_host punches a knocked address ----
lobby.RV_PUNCH_FOR = 1.0
lobby.RV_PUNCH_EVERY = 0.1
hsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
hsock.bind(("127.0.0.1", 0))
target = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
target.bind(("127.0.0.1", 0))
target.settimeout(0.3)
import queue as _q                                           # noqa: E402
pq = _q.Queue()
stop = threading.Event()
io = lobby.LobbyIO(tempfile.mkdtemp(prefix="rvtest_"))
th = threading.Thread(target=lobby.run_host, args=(hsock, "host", io),
                      kwargs={"stop": stop, "log": log, "punch_q": pq}, daemon=True)
th.start()
time.sleep(0.3)
pq.put([("127.0.0.1", target.getsockname()[1])])
hellos, t_first, t_last = 0, None, None
t0 = time.time()
while time.time() - t0 < 2.5:
    try:
        data, addr = target.recvfrom(2048)
    except socket.timeout:
        continue
    ptype, _ = _unpack(data)
    if ptype == TYPE_HELLO and addr[1] == hsock.getsockname()[1]:
        hellos += 1
        t_first = t_first or time.time() - t0
        t_last = time.time() - t0
stop.set()
th.join(timeout=3)
# the host loop wakes at least every 0.2 s, so a 1 s punch window yields ~5
check("the host fires HELLOs at the knocked address", hellos >= 3, f"{hellos} HELLO(s)")
check("the punch stops when it expires", t_last is not None and t_last < 1.8, f"last at {t_last}")

srv.shutdown()
print("FAILED: " + ", ".join(fails) if fails else "ALL OK")
sys.exit(1 if fails else 0)
