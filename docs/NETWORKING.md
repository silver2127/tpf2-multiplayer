# Networking

How players find each other, how game traffic moves between machines, and how the
shared save gets to everyone. The code is `netpunch/` (Python, frozen into
`netpunch.exe`), the lobby panel in `native/src/menu_hook.cpp`, and the bridge DLL's
UDP link in `native/src/net.cpp`. For the threat model see [SECURITY.md](SECURITY.md).

```
 machine A                                             machine B
 TransportFever2.exe                                   TransportFever2.exe
   bridge DLL  --UDP 127.0.0.1-->  netpunch.exe  ==UDP (sealed)==>  netpunch.exe  --UDP 127.0.0.1-->  bridge DLL
   menu DLL    <--jsonl files-->   (lobby)                          (lobby)        <--jsonl files-->  menu DLL
```

The game never opens an internet socket itself. Each instance's bridge talks to its
own lobby process over loopback; the lobbies carry the frames between machines.

## Ports

| what | where | notes |
|---|---|---|
| lobby | UDP 29471 on the host | The host's lobby port. It need not be open: the host punches toward each joiner that knocks through the master server (see [Connecting](#connecting)). Joiners bind an ephemeral port. |
| game relay | UDP `127.0.0.1:7773` (host) / first free of 7774-7805 (joiners) | The bridge sends its frames here; the lobby delivers inbound frames to the bridge's own port. Loopback only. |
| bridge | UDP `127.0.0.1:7771` (or 7772, or a fallback) | The `port=` line of `tpf2_instance.txt` in the data folder. Bound to loopback, and it drops datagrams that are not from its peer ([SECURITY.md](SECURITY.md#what-is-protected)). |
| STUN | outbound UDP 19302 / 3478 | `stun.l.google.com`, `stun.nextcloud.com`, `stun.cloudflare.com`, `stun.services.mozilla.com`. |
| master server | outbound HTTPS | The public game list, and the rendezvous for hole punching (`/knock`); see [Master server](#master-server). |
| dedicated relay | UDP 29471 on the server | Same protocol as a host. |

## Join codes

A code is base32 (RFC 4648, padding stripped) of a small binary profile
(`connect.py`):

| bytes | content |
|---|---|
| 1 | flags: `0x01` open NAT, `0x02` symmetric, `0x04` CGNAT, `0x08` LAN address present, `0x10` public address present, `0x20` IPv6 present, `0x40` IPv6 is 6to4, `0x80` session secret present |
| 4 | Unix timestamp |
| 6 | LAN IPv4 + port (if flagged) |
| 6 | public IPv4 + port (if flagged) |
| 12 or 18 | IPv6 port + address, 10 bytes when 6to4 (if flagged) |
| 12 | session secret (if flagged) |

A typical host code is 47 characters. With a lobby password the code is **locked**:
`0xFF | timestamp | ciphertext | tag`, where the key is PBKDF2-HMAC-SHA256 over the
password (600,000 iterations, salted with the timestamp). A locked code reveals nothing
but its age; a wrong or missing password fails at decode time. A locked host code is
about 74 characters.

- A normal host gets a new secret, so a new code, on every launch.
- A dedicated relay keeps its secret in `relay_secret.bin`, so its code stays valid
  across restarts.
- Codes older than 10 minutes produce a warning, not a failure.
- The IPv6 part is carried but unused: the lobby connects over IPv4 only.

## Connecting

**Host.** `netpunch.exe host` observes its own socket (STUN, plus a UPnP port mapping
it removes on exit), prints `CODE=...`, answers every HELLO, and polls the master server
for knocks every second. For each knock it can open, it sends HELLO to that joiner's
public and LAN addresses every 0.2 s for 15 s. Those packets open the host's own NAT for
the joiner's HELLOs, so the host does not need UPnP or a forwarded port. A host still
has to be reachable directly when the master is unreachable, or when both ends sit
behind symmetric NAT or CGNAT. A dedicated relay does not poll: its port is open.

**Joiner.** `netpunch.exe join <code>` decodes the code, observes its own socket (no
UPnP), and sends HELLO every 100 ms to the host's candidates, LAN first, until an ACK
echoes its token or 40 s pass. Punching is the fallback: a host whose port is open (UPnP,
a forward) answers within a second and nothing else happens. If there is no answer after
4 s, the joiner **knocks**: every 2 s it posts its own
profile code to the master server's `/knock`, sealed with a key derived from the code's
secret (and password), under a tag also derived from the secret
(`SHA-256("tpf2mp-rendezvous-v1|" + secret)`, 24 hex characters). The master can
neither read a knock nor tell which lobby a tag belongs to, and keeps knocks for 60 s.
If the joiner's public address equals the host's, only the LAN candidate is used (no
NAT hairpin). There is no TURN server and no port prediction. `--rendezvous <url>`
picks the master used for knocks (default: `--publish`, else the project's);
`--rendezvous off` disables them.

**Joiner to joiner (mesh).** Every joiner puts its own profile code in its `join`, the
host shares all profiles in the roster, and joiners dial each other on their single
socket (HELLO every 100 ms for up to 8 s, retried every 30 s; a silent link dies after
30 s). A pair that cannot connect directly exchanges frames in an envelope through the
host, or, if the host has been silent for 12 s, through any peer that has a direct link.
`--no-mesh` gives the old star (everything through the host).

## Game frames

- The bridge DLL sends each lockstep frame as one UDP datagram to its lobby's game relay
  port. The lobby wraps it in a `g` payload and sends it on; the receiving lobby delivers
  it to its bridge's port. Frames over 1,400 bytes are dropped.
- **A normal host** sends local frames to every joiner, delivers joiners' frames to its
  bridge, and forwards envelopes addressed to other players.
- **A mesh joiner** sends to the game host directly, to directly linked peers directly,
  and to everyone else in an envelope (`r` + destination + source + frame).
- **A relay** has no bridge: it only forwards envelopes by name (and, for old star
  clients, fans their frames out).
- The lobby layer does not retransmit game frames. Reliability lives above it: the
  bridge's `net.cpp` link and the Lua layer's per-origin sequence numbers with NACK and
  resend (see [ARCHITECTURE.md](ARCHITECTURE.md#reliability)).

The bridge link's packets: a 21-byte header (magic `TPF2`, a per-process session id, sequence
number, acknowledgement and a 32-bit acknowledgement bitmap, type 0 keepalive or 1 event) and, for
events, a 1,029-byte body (chunk index, chunk count, up to 1,024 bytes of text). An event packet is
always 1,050 bytes, well under the lobby's 1,400-byte frame limit. Unacknowledged packets are resent
every 250 ms; with more than 512 pending the backlog is dropped; a peer is considered gone after
10 s of silence. Without a lobby, two games on one machine take 7771 and 7772 and talk to each
other directly.

## Lobby protocol

Every packet is `NP1:` + a one-byte type + payload; anything else is ignored.

| type | meaning |
|---|---|
| `H` / `A` / `C` | HELLO (8-byte token) / ACK (echoes it) / CONNECTED |
| `K` | keepalive, every 15 s on punched links |
| `E` | sealed data (the normal case) |
| `D` | plaintext data: accepted only for save chunks, in an unsealed session, or (by a joiner that is not yet meshed) a "wrong password" reject |
| `S` | signed-only data (defined, not sent) |

A data payload starting with `{` is a JSON lobby message, `NPF1` a save chunk, `g` a
game frame, `r` a relay envelope.

| `t` | direction | fields |
|---|---|---|
| `join` | joiner → host | `name`, `mesh`, `profile`; re-sent every 3 s until the joiner knows the host's name |
| `welcome` | host → joiner | `you`, `host`, `lobby`, `relay` |
| `roster` | host → each joiner, on change and every 2 s | `players`, `host`, `lobby`, `relay`, `letters` (relay only), `started` (for that peer), `start_save`, `stored_age`, `stored_max`, `profiles`, `links`, `companies` |
| `chat` | both | joiner sends `text`; host stamps `from`, `ts`, `cid` and sends three copies |
| `ping` | joiner → host, every 3 s | |
| `company` | joiner → host | `player`, `id` (1-200); a joiner may set only its own, a relay lobby's leader anyone's |
| `links` | joiner → host, when changed | `direct`: names it has a direct link to |
| `mesh_hi` | joiner ↔ joiner | `name` |
| `log` | joiner → host | forwarded log lines for the host's merged `lobby_peers.log` |
| `start` | host → players (three copies); leader → relay | `save` |
| `status` | host → one joiner | `state`, `detail` |
| `leave` / `bye` | joiner → host / host → all | |
| `reject` | host → joiner | `reason`: `lobby full`, or `wrong password` (sent plaintext) |
| `fbegin`, `fbegin_ack`, `fack`, `fdone` | save transfer | see [Save transfer](#save-transfer) |

Timers: a host drops a peer after 10 s of silence; a joiner declares the host gone
after 12 s. Duplicate names become `name#2`, `name#3`. The lobby accepts up to 200
players including the host; the roster (with every profile and mesh link) travels as one
datagram, so a fully meshed lobby runs out of datagram size well before that, somewhere
around 45-60 joiners.

**Letters.** Each player's instance letter (`a`, `b`, ...; after `z`: `aa`, `ab`, ...)
namespaces its commands. In a normal lobby the host is `a` and the others get `b`, `c`,
... in name order, derived by the menu DLL from the roster. A relay assigns letters itself
(lowest unused), saves them in `relay_letters.json` and keeps them per name forever, so a
returning player gets the same letter.

## The lobby and the game

The menu DLL starts the lobby when the player presses HOST GAME or JOIN GAME:

```
host --name <player> --game-relay-port 7773 --game-local-port <bridge port> --forward-log <file> x4
     [--password <pw>] --lobby-name "<name>" [--publish <master url> [--public]]
join <code> --name <player> --local-port 0 --game-relay-port <7774+> --game-local-port <bridge port>
     --forward-log <file> x4 [--password <pw>]
```

It runs the lobby from `%LOCALAPPDATA%\tpf2mp\netpunch\` when that folder holds
`netpunch.exe` or `lobby.py`, otherwise from `netpunch\` next to `tpf2_menu.dll` (the game
folder), preferring `netpunch.exe` to `python lobby.py`. That folder is the lobby's working directory and holds the
IPC files. stdout and stderr go to `lobby_proc.log` there; the process runs in a job
object so closing the game kills it. Inputs are sanitised before they become arguments:
the code must be base32, names are restricted to letters, digits and `-_.` (lobby names
also allow spaces and `'`), and the logged command line masks the password.

| file | written by | content |
|---|---|---|
| `lobby_out.jsonl` | lobby (truncated at start) | events, one JSON object per line |
| `lobby_in.jsonl` | menu DLL and the Lua mod (truncated by the lobby at start) | commands, one JSON object per line |
| `lobby_state.json` | lobby | a merged snapshot (`state`, `players`, `you`, `host`, `started`, `code` on a host) |
| `lobby_peers.log` | host or relay | every player's forwarded log lines, merged |
| `incoming_save.sav` / `.sav.lua` / `.jpg` | receiver | the shared save, written only after its hashes verify |
| `lobby_proc.log` | menu DLL | the lobby process's stdout and stderr |

Events (`lobby_out.jsonl`): `code`; `status` (`state` waiting/connected/failed and a
human-readable `detail`, which the panel shows); `roster` (`players`, `you`, `host`,
`lobby`, `relay`, `companies`, and on joiners `letters`, `stored_age`, `stored_max`);
`chat` (`from`, `text`, `ts`); `start` (`save`); `transfer` (`role` send/recv, `pct`,
`state`, `peer`); `save_ready` (`name`, `dir`, `files`).

Commands (`lobby_in.jsonl`): `chat` (`text`); `company` (`player`, `id`);
`publish` (`on`); `start` (optional `save` = path of the save to share); `quit`; `name`
(accepted, never sent).

Chat lines starting with `/` are conventions of the game side, not the lobby, except
`/new` and `/resume`, which a relay interprets (below). The panel treats a chat line
starting with `!hotjoin ` as a status message.

## Save transfer

1. The host presses START GAME. The menu sends `start` with the newest `*.sav` in the
   save folder (skipping `mp_shared.sav` unless it is the only one); with no save at all it
   sends a plain `start`.
2. The lobby reads the `.sav`, its `.sav.lua` and `.jpg` sidecars, and sends them as one
   stream named `incoming_save.sav`, `incoming_save.sav.lua`, `incoming_save.jpg`, with a
   SHA-256 per file and overall in the (sealed) `fbegin`.
3. Chunks are 1,350 bytes (8,192 when every receiver is on loopback), sent as plaintext
   `NPF1` frames inside a 2,048-chunk window (16,384 on loopback). Receivers write each
   chunk at its offset, report `{base, nack}` every 50 ms, and the sender resends NACKed
   chunks first. It rewinds after 0.5 s without feedback and gives up on a peer after 30 s
   without progress.
4. The receiver checks the proposed filenames against a whitelist before allocating, refuses
   writes past the end, verifies every hash (retrying the whole transfer up to three
   times), writes the files and emits `save_ready`.
5. **Mods the save needs (off by default since 2026-09-11; `lobby.py host --share-mods`
   turns it on).** A joiner whose game could not see the save's Workshop mods was never
   asked (their folders existed) and its load was refused, and copying a Workshop item
   into the workshop folder is unlikely to make the game load it; players install the
   save's mods themselves until that is solved. With the flag, the host reads the save's active mod list off the `.sav`
   (a Zstandard frame; the list sits before the game settings and is parsed from that
   anchor) and lists it in `fbegin` as `mods`. Each receiver answers in `fbegin_ack` with
   `need`, the ids it has no folder for (`<id>_<version>` under the game's `mods`, the
   profile's `local/mods`, or `steamapps/workshop/content/1066780/<id>` for a `*<id>`
   workshop item). When the save has verified everywhere and somebody needs something,
   the host zips each such mod folder and sends the union as a second transfer
   (`kind: "mods"`, files `incoming_mod_<id>_<version>.zip`, same chunking, hashes and
   window); a receiver unpacks each into its game's `mods` folder (never over an existing
   one, paths inside the zip are checked) and emits `mods_ready`. The multiplayer mod
   itself is never sent. A mod the host cannot find, or one over 512 MB, is named in chat
   and skipped. A receiver takes a mods round only right after a verified save round, only
   after its player said YES, and installs only the mods it was asked about. A dedicated
   relay never shares mods. Per-save mod settings need nothing: they are inside the save.
6. When every receiver has verified (and any mods round has resolved), the host sends `start` to them. On each joiner the menu
   DLL copies the files into the game's save folder as `mp_shared.sav` (+ sidecars) and the
   player loads **mp_shared** from LOAD GAME. A failed transfer leaves the host with
   "press START GAME to retry".

A player who joins while a transfer is running is served as soon as it finishes; one who
joins after the start is sent the last shared save.

## Hot join

A player who arrives while a game is running needs the world as it is now. The host's
menu DLL takes care of it: when the roster grows in game (or someone types `/sync`), it
forces the game's own autosave (see [re/GAME_LOOP_AND_UI.md](re/GAME_LOOP_AND_UI.md)),
waits for the file to stop growing, and sends it to the lobby as a normal `start` with a
save. Players already in the game ignore the start; the newcomer loads it. The Lua side
brings the newcomer's clock up to the session with command history and a catch-up speed
(see [ARCHITECTURE.md](ARCHITECTURE.md)).

## Dedicated relay

`lobby.py host --relay-only` runs a lobby with no game, for groups where nobody can
accept inbound connections, and for an always-on public server.

- **Leader.** The oldest connected joiner is the leader: it gets the host role in the panel
  and is the session's clock. When it leaves, the next-oldest takes over.
- **Stored world.** The relay keeps the last save a leader uploaded
  (`incoming_save.*` in its data folder), replaced only when a new upload verifies.
- **Resuming.** When a leader joins a relay that holds a world and no session is running,
  everyone is told the world will continue in 10 s. The leader can say `/new` in chat within
  that time to discard it and share their own save with START GAME instead. After the grace
  the relay pushes the stored world to everyone and starts them. `/resume` (leader) pushes
  it on demand.
- **Uploads.** The leader's START GAME uploads its save to the relay, which then shares it
  with everyone waiting. While a leader plays, its menu DLL re-uploads a fresh autosave every
  2 minutes (`relay_autosave_min` in `tpf2_menu_flags.txt`; 0 turns it off); when nobody is
  waiting for it the relay just keeps the copy.
- **Late joiners** are served the stored world if it is at most 180 s old, or if it is the
  copy the running session resumed from (until a newer upload replaces it); otherwise they
  wait for the leader's next save.
- **Persistence.** Letters (`relay_letters.json`), company chips (`relay_companies.json`) and
  the secret survive restarts. When the last player leaves, the session is closed but the
  world is kept.
- **It does not simulate.** The world only advances while players are connected.

Operating one (all scripts take the SSH target as their first argument):

| script | does |
|---|---|
| `tools/relay_deploy.sh [target] [lobby name] [udp port]` | copies the netpunch sources to `/opt/tpf2mp/netpunch`, writes `/etc/tpf2mp/relay.env` and the `tpf2mp-relay` systemd unit (runs as user `tpf2mp`, data in `/var/lib/tpf2mp/relay`), opens the port in ufw, restarts, and prints the public listing. Refuses to restart while players are connected unless `FORCE=1`. |
| `tools/relay_put_save.sh <save.sav> [target]` | replaces the relay's stored world with a local save (does not restart the service). |
| `python tools/relay_selftest.py` | runs a real relay and players locally: letters, upload, mid-session joins, restart with the same code, frames both ways. Needs internet for STUN. |

## Master server

`netpunch/masterserver.py` is a stdlib HTTP service that only lists games; the code in a
row is the join, nothing is brokered. It runs as the `tpf2mp-master` systemd unit on
`127.0.0.1:8471` behind nginx, which serves it at `https://srv1306562.hstgr.cloud/tpf2mp/` (the
VPS's own hostname, with its own Let's Encrypt certificate); `tools/masterserver_deploy.sh`
installs all of it. A relay deployed by `tools/relay_deploy.sh` announces to `127.0.0.1:8471`
directly.

| endpoint | behaviour |
|---|---|
| `GET /list` (or `/`) | `{"servers": [{id, name, code, players, max, game, type, version, locked, age}], "now", "ttl": 30}`, newest first |
| `POST /announce` | body `{id, name, code, players, max, type, version, locked}` (at most 4 KB; `code` required); at most 500 entries |
| `POST /leave` | body `{id}` |
| `GET /health` | `ok` |

`type` is `relay` or `host`, shown in the panel as "dedicated server" or "player hosted"; the
list never carries a save name. The master also writes that label into `game`, the column that
panels from 0.4.11 and earlier display. An announce without a valid `type` (a lobby from before
the field) counts as `relay` when its `game` is `dedicated relay`, and as `host` otherwise.

Entries expire 30 s after their last announce and live only in memory. A host with PUBLIC
ticked (or a relay started with `--public`) announces every 10 s and sends `/leave` when
unticked or closed (a relay also on SIGTERM, so stopping its service removes the row); a
host killed hard simply ages out. A relay announces under a stable id derived from its lobby
name, port and machine, so a restarted relay replaces its own row. The panel fetches `/list`
every 10 s while the HOST/JOIN page is open. `master_url=` in `tpf2_menu_flags.txt` points the panel
and hosts at another server; an empty value hides the list and disables publishing.

## Self-tests

```
cd netpunch
python lobby.py --selftest            # host + 2 joiners + a late joiner: roster, chat
python lobby.py --selftest-transfer   # save transfer, clean and with 8% / 15% loss, plus a retry
python lobby.py --selftest-relay      # game frames through the relay, both directions
python lobby.py --selftest-mesh       # host + 3 sealed joiners, every frame arrives once
python seal.py                        # sealing: round trip, tamper, replay, reorder
python mesh.py                        # three mesh nodes
python connect.py selftest            # code format and a loopback connect race
python ..\tools\relay_selftest.py     # real processes, dedicated relay (needs internet)
```

`punch.py`, `connect.py` and `observe.py` also run standalone (a two-player punch test, the
original two-player connect CLI, and a printout of this machine's connectivity profile).
