# netpunch — serverless P2P UDP connection engine

Internet-transport layer for the Transport Fever 2 co-op mod. Two friends
connect **directly, no servers**, by pasting a short code to each other over
Discord. Pure UDP hole-punching with STUN + UPnP assistance.

Files:

| File         | Phase | Role |
|--------------|-------|------|
| `punch.py`   | 1     | Core UDP hole-punch engine + `Connection` object |
| `observe.py` | 2     | Builds a "connectivity profile" (STUN / UPnP / v6) |
| `connect.py` | 3 + 4 | Code exchange, the connect race, the CLI the C++ menu drives, failure diagnosis |

Every file is both **runnable** (`python <file> ...`) and **importable**
(`import punch` / `observe` / `connect`). Dependencies: stdlib + `pystun3` +
`miniupnpc` only.

## Install

```
pip install pystun3 miniupnpc      # or: pip install -r requirements.txt
```

On this Windows/Python 3.12 box the `miniupnpc` wheel installed cleanly. If it
ever fails on another machine, the UPnP path **degrades gracefully**: it tries
the `upnpc.exe` CLI if present, and otherwise just reports `open=false` instead
of crashing.

---

## How to run each phase

### Phase 1 — punch.py (the transport)
```
python punch.py --selftest                       # loopback: two instances must both CONNECT
python punch.py --local-port 29471 --peer IP:PORT [--name A]   # real punch + tiny chat
```
`--selftest` spawns two `Connection`s on 127.0.0.1 (ports 29471/29472), asserts
both reach CONNECTED, and exchanges a datagram each way. **Verified PASS.**

Programmatic use:
```python
from punch import punch
conn = punch("203.0.113.7", 29471, timeout=30)   # -> Connection or None
conn.send(b"hello");  data = conn.recv(timeout=5);  conn.close()
conn.peer_str        # "203.0.113.7:29471"
```

### Phase 2 — observe.py (connectivity profile)
```
python observe.py [--local-port 29471] [--no-upnp]     # prints the profile as JSON
```
```python
from observe import observe
profile = observe(local_port=29471)   # dict; re-run at connect time, mappings expire
```

### Phase 3/4 — connect.py (the interface the menu calls)
```
python connect.py host                 # prints CODE=..., then waits for the joiner
python connect.py join <CODE>          # prints its own CODE=..., dials the host
python connect.py selftest             # codec round-trip + localhost dial/listen race
```
Optional: `--local-port 29471`, `--timeout 40`.

---

## Code format

A profile is packed into a compact binary blob, then **base32**-encoded (padding
stripped). Real codes are **~18 chars** (v4-only) to **~47 chars** (open NAT +
IPv6 6to4) — short enough to paste in Discord.

```
byte 0   bit flags:
         0x01 open          0x08 has lan_v4
         0x02 symmetric     0x10 has public_v4
         0x04 cgnat         0x20 has v6
                            0x40 v6 is 6to4, derived from public_v4
byte 1-4 uint32 big-endian unix timestamp  (drives the >10-min stale warning)
[lan_v4]     4-byte IPv4 + 2-byte port      (present if bit 0x08)
[public_v4]  4-byte IPv4 + 2-byte port      (present if bit 0x10)
[v6]         2-byte port + v6 address       (present if bit 0x20)
             v6 address is 10 bytes if 6to4-derived (bit 0x40), else 16 bytes
```

**6to4 compression:** a `2002:VVVV:VVVV:…` address embeds the public IPv4 in its
top 48 bits. When `public_v4` is also in the code we ship only the low 80 bits
and rebuild the top from `public_v4` on decode — saves 6 bytes.

Encode/decode live in `connect.py`:
```python
from connect import encode_profile, decode_code
code = encode_profile(profile)
peer = decode_code(code)   # {"candidates", "flags", "ts", "age", "stale"}
```
`decode_code` sets `stale=True` when the code is older than 10 minutes
(`CODE_TTL`), and rejects malformed codes with `ValueError`.

---

## connect.py CLI contract (keep EXACT — a C++ caller drives this)

**stdout:** exactly one line, `CODE=<base32>`, and nothing else. All diagnostics
go to **stderr**.

**Status file:** `netpunch_status.txt` in the current working directory, written
as JSON lines. First line is written at startup, terminal line at the end:
```
{"state":"waiting"}
{"state":"connected","peer":"1.2.3.4:29471"}      // success
{"state":"failed","reason":"..."}                 // failure
```
Read the **last** line for the outcome. (Run host and join in **different working
directories** if on the same box, or they share this file.)

**Exit code:** `0` on connect, non-zero on failure (`1` connect/timeout failure,
`2` bad arguments / bad code).

**host** — observes, prints `CODE=`, writes `waiting`, then **listens** on the
game socket (it shared first and is usually the open end). If the joiner's reply
code is piped to host's **stdin**, host decodes it and may flip to dial/punch for
the awkward "host is the constrained one" case. Blocks until connected/failed.

**join `<CODE>`** — decodes the host code, observes itself, prints its **own**
`CODE=` (for the reply), writes `waiting`, picks a role from both ends' flags,
and dials the host's candidates. Blocks until connected/failed.

### The race
One `Connection` per address family reuses **one socket** (the STUN/game socket,
so the NAT mapping lines up):
- **v4** socket sends HELLOs at both `public_v4` and `lan_v4` of the peer;
- **v6** socket sends at the peer's 6to4/global `v6` (only if both ends have v6).

First family to complete the handshake wins; the loser is closed. Per-family
outcomes are logged to stderr.

### Role-flip (symmetric / CGNAT)
`choose_role(mine, peer)` returns complementary roles:
- exactly one end constrained (symmetric/cgnat) and the other `open` →
  **open listens, constrained dials** (don't punch — a symmetric NAT makes a new
  mapping per destination, so only the constrained→open direction is reliable);
- otherwise **both punch** simultaneously (never deadlocks).

### Named failures (Phase 4 diagnosis)
`diagnose(...)` produces the reason written to the status file:
- `no UDP out -- STUN got no response; a firewall is likely blocking UDP`
- `both symmetric NATs -- direct punch won't work; use Tailscale`
- `both behind CGNAT -- use Tailscale tonight`
- `code stale -- re-exchange a fresh code`
- `handshake timed out on all candidate pairs`

STUN total-timeout is measured in `observe` (`profile["stun"]["elapsed_ms"]` and
`["timed_out"]`) and reported on stderr before the code is generated.

---

## Handshake (the state machine)

Framed packets: `MAGIC(b"NP1:") + TYPE + PAYLOAD`, so stray UDP is dropped.
```
HELLO(token=ours)  --->   send every 100 ms until connected
                   <---   ACK(echoes the sender's token)
CONNECTED          --->   sent once our own token comes back
KEEPALIVE          --->   every 15 s after connect; peer last-seen tracked
DATA               <-->   application datagrams
```
A side declares success when it receives an **ACK echoing its own token** (proof
the peer heard us). Both ends reach that independently. The peer address is
always the **most-recent** source, so a mid-handshake NAT re-map is survived.

One reader thread owns the socket for its whole life (handshake → steady state),
so there is never a second reader stealing packets. Windows quirks handled:
`SIO_UDP_CONNRESET` and swallowing the ICMP-port-unreachable `ConnectionResetError`
on send/recv; `IPV6_V6ONLY=1` so v4 and v6 sockets can share the game port.

---

## This machine's observed profile (reference)

Residential fibre (public IPv4, no CGNAT), consumer router with UPnP:
- `open=true` (UPnP works), `symmetric=false`, `cgnat=false`, `v6=true`
- public_v4 `198.51.100.77` (port-preserving NAT: external port == game port)
- v6 candidate is the **6to4** `2002:c633:644d:1::1000` (embeds the public v4;
  routable via HE relays even with no native v6). Note: Python's `ipaddress`
  marks 6to4 as *private*, so `observe.enumerate_v6` special-cases `2002::/16`
  and also consults the connect-trick (the address the OS actually routes out),
  and filters Tailscale/Hamachi/ULA/link-local overlays.

---

## Test status

**Automated (done, on this one machine):**
- `python punch.py --selftest` — two loopback instances both CONNECT + exchange
  data. PASS (ran repeatedly, non-flaky).
- `python connect.py selftest` — codec round-trips (incl. 6to4 compression),
  stale detection, localhost listen/dial race. PASS.
- Full two-process `host` + `join` over loopback (separate cwds, crafted peer
  code) — both reach `{"state":"connected"}`, exit paths correct. PASS.
- Failure paths — bad code → exit 2; unreachable target → timeout diagnosis,
  exit 1. PASS.
- Live STUN (Google + Nextcloud agreed on the mapped port) and live UPnP
  (add/get/delete mapping) against the real router. PASS.

**MANUAL, PENDING A SECOND MACHINE (a real friend):**
- Real internet hole-punch between two different NATs (public_v4 ↔ public_v4).
- 6to4 v6 ↔ v6 path across two networks.
- Role-flip against a genuinely symmetric or CGNAT friend.
- ISP hairpin behavior (only matters for same-network testing).
- End-to-end code exchange latency vs the 10-minute staleness window.

## Integration note (open item)
`connect.py` currently **establishes and holds** the punched path (idles keeping
the socket/mapping alive). Wiring the live game data through it still needs a
decision: either the mod embeds `punch.Connection` (`send`/`recv`) directly, or
`connect.py` grows a local UDP bridge (loopback port ⇄ peer) the mod talks to.
Not required by the Phase 1-4 contract, but it's the next piece for the mod.

---

## Lobby (`lobby.py`) — N-player lobby: usernames, roster, chat

Layer on top of Phases 1–4: turns hole-punched UDP links into a small,
**host-authoritative** lobby that the in-game menu drives through flat files.
Stdlib + the existing modules only (imports `punch`, `connect`, `observe`).

### Topology — host-as-relay star
The **host is assumed reachable** (open NAT). Every **joiner dials the host**
using the existing `connect.race` (role `dial`, v4 only → one socket). The host
owns a **single UDP socket** and receives from ALL joiners on it, demultiplexing
by **source address** (each distinct source = one peer). The host is the
authority for the roster and **relays chat to everyone**. No mesh → scales to a
handful of players. (A constrained/non-open host is out of scope for the star;
that would need the host to punch back per joiner.)

Two layers share the same `NP1:` wire:
- **transport handshake** (`HELLO/ACK/CONNECTED/KEEPALIVE`) — reused verbatim
  from `punch.py`. The host replies to `HELLO` with `ACK(token)` so joiners
  connect; the joiner side is a stock `punch.Connection`.
- **lobby protocol** — carried INSIDE `TYPE_DATA` payloads as one UTF-8 **JSON**
  object with a `"t"` (type) field. Every lobby message is a real `NP1:` DATA
  frame, so `Connection.send`/`recv` (joiner) and `_unpack` (host) handle it
  unchanged. This is the "extend `NP1:` with new types".

### Wire message types (the `"t"` field, inside a DATA frame)
| `t`       | direction        | payload |
|-----------|------------------|---------|
| `join`    | joiner → host    | `{t:"join", name}` |
| `welcome` | host → one joiner| `{t:"welcome", you, host}` — your final (de-duplicated) name |
| `roster`  | host → all       | `{t:"roster", players:[sorted names], host}` |
| `chat`    | host → all       | `{t:"chat", from, text, ts, cid}` (relayed + stamped) |
| `chat`    | joiner → host    | `{t:"chat", text}` (host stamps `from`/`ts`/`cid`) |
| `ping`    | joiner → host    | `{t:"ping"}` — fast (3 s) lobby keepalive |
| `start`   | host → all       | `{t:"start"}` |
| `leave`   | joiner → host    | `{t:"leave"}` |
| `reject`  | host → one joiner| `{t:"reject", reason}` — e.g. lobby full |
| `bye`     | host → all       | `{t:"bye"}` — lobby closing |

Host behavior: on `join` add `{source_addr → username}`, broadcast `roster`; on
`chat` stamp with the sender's username + a monotonic `cid` and broadcast to all
(**including an echo** back to the sender); on a peer missing keepalive **~10 s**
drop it and re-broadcast `roster`; on `start` (host only) broadcast `start`.
Duplicate usernames get `#2`, `#3`, … . Lobby is **capped at 8 incl. the host**;
the 9th joiner gets `reject{reason:"lobby full"}`.

Reliability: `roster` is idempotent and **re-broadcast every 2 s** (UDP
self-heal + doubles as a host→joiner keepalive). `chat`/`start` are sent in a
small **burst (×3)**; clients **de-dupe `chat` by `cid`** and apply `start`
once. Best-effort — fine on loopback; real-net loss is softened but not a full
reliable channel (see pending).

### File-IPC contract (how the menu drives it — EXACT)
All three files live in the process **CWD** (override with `--io-dir`). Both
jsonl files are **TRUNCATED on startup** so stale lines don't replay.

**`lobby_out.jsonl`** — the lobby **APPENDS** newline-delimited JSON events:
```json
{"type":"code","code":"<base32>"}                              // host, once
{"type":"status","state":"waiting|connected|failed","detail":"..."}
{"type":"roster","players":["alice","bob"],"you":"alice","host":"alice"}
{"type":"chat","from":"bob","text":"hi","ts":1700000000}
{"type":"start"}
```
`players` is **sorted**; `you` is THIS process's own (post-dedupe) username;
`host` is the host's username. A `roster` event is emitted only when the roster
**content changes** (the 2 s heal packets do not spam the log). `chat` events are
emitted once per unique message (host echoes its own chats too).

**`lobby_state.json`** — a **single JSON object, overwritten** (atomic
`os.replace`), mirroring the latest state for easy polling:
```json
{"state":"waiting|connected|failed","code":"<base32>|null",
 "players":["alice","bob"],"you":"alice","host":"alice","started":false}
```
(`code` is host-only; `started` flips true after `start`.)

**`lobby_in.jsonl`** — the menu **APPENDS** command lines; the lobby **TAILS** it
(tracks a **byte offset**, processes only new **whole** lines):
```json
{"cmd":"chat","text":"..."}     // send CHAT
{"cmd":"name","name":"..."}     // change own username, re-JOIN / re-broadcast
{"cmd":"start"}                 // host broadcasts START (no-op on a client)
{"cmd":"quit"}                  // leave cleanly
```

Status lifecycle: host → `waiting` (observing) → `connected` (lobby ready) →
`failed` (`lobby closed`) on quit. Joiner → `waiting` (dialing) → `connected`
(handshake) → `failed` (`host unreachable` / `left lobby` / `lobby full` /
`host closed the lobby`). `failed` is the only terminal state the spec allows, so
clean exits use it with a descriptive `detail`.

### How to run
```
python lobby.py host --name alice            # observe, print ONE  CODE=<base32>, serve forever
python lobby.py join <CODE> --name bob       # dial the host, participate
python lobby.py --selftest                   # 1 host + 2 joiners on loopback
```
Optional: `--local-port 29471`, `--timeout 40` (join dial budget),
`--io-dir <dir>` (relocate the `lobby_*.json[l]` files off CWD).

`host` prints **exactly one** `CODE=<base32>` line to **stdout** (reused from
`connect._observe_and_announce`); everything else goes to stderr. Run host and
each joiner in **different working dirs** (or use `--io-dir`) so they don't share
the flat files.

### Test status
**Automated (loopback, this one machine) — PASS, non-flaky (ran repeatedly):**
- `python lobby.py --selftest` — 1 host (`alice`) + 2 joiners (`bob`,`carol`)
  over loopback (ports 29520/21/22, temp dirs). Asserts (a) all three usernames
  appear in **every** roster, and (b) a chat from one joiner reaches **all three**
  via their `lobby_out.jsonl`. Proves multi-peer relay on one host socket.
- Ad-hoc probes (also PASS): duplicate usernames → `dave`/`dave#2`/`dave#3`;
  `name` command renames + updates `you`; `start` reaches all (`started:true`);
  `quit` drops the peer and re-broadcasts the roster; keepalive-timeout drop;
  lobby-full `reject`.

**PENDING A SECOND MACHINE (cannot test here — no second host):**
- Live cross-NAT star: real joiners dialing a real open host over the internet
  (the whole point; only ever loopback-tested here).
- Behavior when the host is **behind a constrained NAT** (star assumes open host;
  no per-joiner punch-back / role-flip on the host side yet).
- **Packet-loss reliability** on a real link — chat/start are best-effort
  burst-×3 + roster self-heal, not a guaranteed channel; a lossy path could drop
  a chat. Roster converges (re-broadcast); chat/start do not retransmit on NACK.
- Many-player (near the 8 cap) load / churn over a real network.
- v6-only joiners: the host lobby listens on its **v4** socket only (single-socket
  demux); joiners dial v4 candidates. v6 star path is not wired.

---

## Save transfer (`lobby.py`) — reliable host → all-joiners `.sav` push

Layered on the **existing** lobby DATA channel (no new sockets, no new
connections): when the host starts the game it first pushes the `.sav` (and any
sidecars) to **every** connected joiner, waits until each has received +
verified it, and only **then** broadcasts the normal `start` so everyone begins
from the same world. Designed for **~200 MB** real saves — pipelined (never
stop-and-wait), **no hard size cap**, correct under UDP loss / dup / reorder.

### Trigger command (menu → `lobby_in.jsonl`)
```json
{"cmd":"start","save":"<absolute path to the host's .sav>"}
```
- The host reads `<name>.sav` plus, if present, `<name>.sav.lua` and
  `<name>.jpg`, transfers them to all joiners, waits for every joiner to verify
  (or drop / time out — then it proceeds without that one), then broadcasts the
  normal `start`.
- The **legacy** `{"cmd":"start"}` (no `save`) is unchanged — broadcast start,
  no transfer. A `start` arriving while a transfer is already running is ignored.
- Joiners are **receive-only**; a joiner never sends a save.

### File-IPC events (lobby → `lobby_out.jsonl`)
Host, per joiner (progress throttled to ~every 10 %):
```json
{"type":"transfer","role":"send","peer":"<name>","pct":<0-100>}
{"type":"transfer","role":"send","peer":"<name>","state":"done"}
```
Joiner:
```json
{"type":"transfer","role":"recv","pct":<0-100>}
{"type":"save_ready","name":"incoming_save","dir":"<abs io dir>","files":["incoming_save.sav","incoming_save.sav.lua","incoming_save.jpg"]}
```
On any failure (either side):
```json
{"type":"status","state":"failed","detail":"save transfer failed: <reason>"}
```
The `start` event (`{"type":"start"}`) is emitted **after** all per-peer `done`
events. `files` in `save_ready` lists only the files that were actually present
(the `.sav` is always there; sidecars appear only if the host had them).

### Where the joiner writes files
Into the lobby **IO directory** (`IO.directory` — CWD, or `--io-dir`), named
`incoming_save.sav` (+ `incoming_save.sav.lua`, `incoming_save.jpg` if sent).
Basename is `incoming_save`. The game menu moves them into the save folder and
loads them; the lobby's only job is to land byte-identical files and emit
`save_ready`.

### Wire protocol (all inside `NP1:` DATA frames)
JSON control messages keep the `"t"` convention:

| `t`          | direction        | payload |
|--------------|------------------|---------|
| `fbegin`     | host → joiner    | `{t,sid,total_bytes,chunk,total_chunks,files:[{name,size,sha256}],sha256}` |
| `fbegin_ack` | joiner → host    | `{t,sid}` — receiver allocated its buffer |
| `fack`       | joiner → host    | `{t,sid,base,nack:[seq,…]}` — cumulative base + selective holes |
| `fdone`      | joiner → host    | `{t,sid,ok[,final]}` — verified (`ok:true`) or gave up (`ok:false,final:true`) |

**Chunk frames** (host → joiner) are **binary, not JSON**, to avoid base64
bloat — a DATA payload beginning with `NPF1` is a chunk (`'{'` ≠ `'N'` so it can
never collide with a JSON message):
```
CHUNK_MAGIC(b"NPF1") + sid(uint32 BE) + seq(uint32 BE) + up-to-1200 data bytes
```
All files are concatenated into **one byte stream with a single sequence space**
(order: `.sav`, `.sav.lua`, `.jpg`); the receiver splits them back out using the
per-file `size`s from `fbegin`. Integrity is **SHA-256 per file and overall**.

### Chunk size / window / timeouts (and why)
| Constant | Value | Rationale |
|----------|-------|-----------|
| `CHUNK_DATA` | **1200 B** | datagram = NP1(5) + header(12) + 1200 = **1217 B**, under the 1280 IPv6 min-MTU and 1500 v4 MTU even with PPPoE/VPN overhead → **no fragmentation** |
| `SEND_WINDOW` | **2048 chunks** (~2.4 MB) | pipelines the link so RTT doesn't throttle a big save |
| `SEND_BUDGET` | **256 dgram/peer/pump** | bounds one host-loop iteration so other peers' pings/roster keep flowing during a send |
| `FEEDBACK_INTERVAL` | **50 ms** | receiver `fack` cadence |
| `RESEND_AFTER` | **0.5 s** | fack silence → host rewinds + re-streams the window (recovers lost facks) |
| `PEER_XFER_TIMEOUT` | **30 s** | no forward progress → skip that peer, keep serving the rest |
| `MAX_NACK` | **128** | holes reported per fack (rest next round) |

### Reliability scheme — receiver-driven selective repeat (windowed ARQ)
1. Host sends `fbegin` (retried every 200 ms until the peer acks or a fack
   arrives), then **streams chunks within a flow-control window** `[base,
   base+SEND_WINDOW)`.
2. Each receiver writes every chunk at its exact byte offset (so **reorder /
   dups are free**), tracks the contiguous high-water `base`, and every 50 ms
   reports `{base, nack:[holes…]}`.
3. Host drops everything `< base`, **retransmits NACKed chunks first**, then
   sends new in-order chunks up to the window edge.
4. If a receiver goes silent (its facks were lost), the host **rewinds and
   re-streams the window** — self-heals under any loss pattern.
5. On `base == total`, the receiver verifies SHA-256, writes the files, emits
   `save_ready`, and sends `fdone`. A hash mismatch triggers a bounded whole-file
   re-request (`MAX_FILE_RETRIES=3`), then a `final` failure.

**Interleaving:** the transfer is **pumped from the host's single demux loop** —
`pump()` sends at most `SEND_BUDGET` datagrams *per peer* and never blocks, the
loop drains up to `HOST_DRAIN` inbound datagrams per cycle and polls at 2 ms
while a transfer is active, and `fack`/`ping` traffic refreshes each peer's
last-seen — so keepalive, roster heal, chat and drop-detection **keep working
for all peers during a transfer**. A peer dropped by the normal keepalive logic
is skipped (`on_peer_dropped`) and the transfer proceeds for the others. Any
transfer exception is caught and emitted as `failed` — it never crashes the
lobby thread. Socket send/recv buffers are bumped to 4 MB (best-effort) to
absorb bursts.

### How to run / test
```
python lobby.py --selftest             # original roster+chat test (still PASS)
python lobby.py --selftest-transfer    # save transfer: clean + lossy, x4, PASS
```
`--selftest-transfer` brings up 1 host + 2 joiners on loopback (ports
29520/21/22, separate temp `--io-dir`s) and runs **four** iterations — two clean
(20 MB, 16 MB) and two with **injected packet loss** (8 %, 15 %) via a
`_LossySocket` that randomly drops outbound datagrams — proving retransmit and
non-flakiness. Each iteration asserts: every joiner's `incoming_save.*` is
**byte-identical** (SHA-256) to the source, each joiner emitted `save_ready` +
`start`, the host emitted a per-peer `state:"done"` for both, and the host's
`start` came **after** both `done` events. Loss is enabled **only after** the
lobby is established (the base lobby sends `join` once and does not retransmit
it — a pre-existing gap, separate from the transfer).

**Measured (loopback, this machine):** 100 MB → 2 joiners in ~8 s (~12 MB/s per
peer); 10–20 MB clean in ~2.5–2.8 s; the same under 8–15 % loss in ~3 s. All
runs verified byte-identical.

### Limitations / caveats
- **Memory:** sender holds the whole concatenation once (~200 MB); each receiver
  holds its own `bytearray(total_bytes)`. Fine on a normal PC; there is no
  streaming-to-disk path (kept simple + correct). No hard byte cap — allocation
  failure is caught and reported as `failed`.
- **Throughput** is bounded by `SEND_WINDOW x CHUNK_DATA / RTT` on a real link
  (~2.4 MB per RTT ⇒ tens of MB/s at typical WAN RTTs) and by `SEND_BUDGET` on
  loopback. Raise `SEND_WINDOW` for very-high-BDP paths.
- **Cross-NAT:** rides the existing host-as-relay star, so it inherits its
  caveats — the host must be the reachable/open end; joiners behind their own
  NATs are fine (they dial the host), but a constrained host is out of scope.
- New joiners **during** a transfer are not enrolled into the in-flight
  transfer; they get the world on the next start.
- `fbegin`/roster/chat share the one DATA channel; a giant transfer is paced by
  `SEND_BUDGET` so it doesn't starve them, but chat latency rises slightly
  mid-transfer.
