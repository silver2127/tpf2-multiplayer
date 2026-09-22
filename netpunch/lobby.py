#!/usr/bin/env python3
"""
lobby.py -- N-player LOBBY (usernames + roster + chat) over the netpunch engine.

This sits on top of the Phase 1-4 transport (``punch.py`` / ``observe.py`` /
``connect.py``) and turns a pile of hole-punched UDP links into a small,
host-authoritative lobby that the in-game Transport Fever 2 menu drives through
flat files.

TOPOLOGY -- host-as-relay star
------------------------------
The HOST is assumed reachable (open NAT). Every JOINER dials the host with the
existing connect race, so each joiner ends up with one ``punch.Connection`` to
the host. The host owns a SINGLE UDP socket and receives from ALL joiners on it;
it demultiplexes by source address (each distinct source == one peer). The host
is the authority for the roster and relays chat to everyone. No mesh, so it
scales to a handful of players without N^2 links.

Two layers ride the same ``NP1:`` wire:
  * the transport handshake (HELLO/ACK/CONNECTED/KEEPALIVE) -- reused verbatim
    from punch.py; the host replies to HELLO with ACK so joiners connect, and
    the joiner side is a stock ``Connection``.
  * the lobby protocol -- carried INSIDE ``TYPE_DATA`` payloads as one UTF-8
    JSON object with a ``"t"`` (type) field. This is the "extend NP1 with new
    types" the spec asks for: every lobby message is a real ``NP1:`` DATA frame,
    so ``Connection.send``/``recv`` on the joiner and ``_unpack`` on the host
    both handle it unchanged.

Lobby message types (the ``"t"`` field):
    join    {t:join, name, version}              joiner -> host (exact release match)
    welcome {t:welcome, you, host, version}      host -> one joiner (your final,
                                                  de-duplicated, username)
    roster  {t:roster, players[sorted], host,
             started, start_save}                 host -> all (also the ~2 s heal).
                                                  ``started`` is PER PEER: true only
                                                  if THAT joiner was included in a
                                                  start, so a joiner that missed the
                                                  start burst catches up from the
                                                  heal; ``start_save`` mirrors the
                                                  start's save flag.
    chat    {t:chat, from, text, ts, cid}        host -> all (relayed + stamped)
    chat    {t:chat, text}                        joiner -> host (host stamps it)
    ping    {t:ping}                              joiner -> host (fast keepalive)
    start   {t:start, save}                       host -> all (save=true when a save
                                                  transfer completed for that peer
                                                  this session; false = legacy
                                                  no-save start)
    status  {t:status, state, detail}             host -> one joiner (advisory; e.g.
                                                  a late joiner learns the game has
                                                  already started)
    leave   {t:leave}                             joiner -> host
    reject  {t:reject, reason}                    host -> one joiner (lobby full)
    bye     {t:bye}                               host -> all (lobby closing)

FILE-IPC CONTRACT (how the menu drives it -- kept EXACT)
--------------------------------------------------------
All three files live in the process CWD (override with --io-dir).

  lobby_out.jsonl   the lobby APPENDS newline-delimited JSON events:
      {"type":"code","code":"<base32>"}                 (host, once)
      {"type":"status","state":"waiting|connected|failed","detail":"..."}
      {"type":"roster","players":["alice","bob"],"you":"alice","host":"alice"}
      {"type":"chat","from":"bob","text":"hi","ts":<unix>}
      {"type":"start","save":true|false}
          save=true  -> load incoming_save.* (a save transfer completed);
          save=false -> legacy no-save start.
          A JOINER emits this ONLY when save==false, or save==true AND its
          receiver completed this session (it emitted save_ready). Any other
          start is logged and ignored WITHOUT latching 'started', so a later
          retried START GAME still works.
      {"type":"transfer","role":"send|recv",...}         (progress; per-peer on
          the host: "peer","pct" or "state":"done|failed|dropped")
      {"type":"save_ready","name":"incoming_save","dir":...,"files":[...]}
      {"type":"status","state":"failed",
       "detail":"save transfer failed for <names> -- press START GAME to retry"}
          (host, when ANY peer's transfer failed: the host does NOT broadcast
          start and clears the transfer so START GAME can be pressed again)
      {"type":"status","state":"connected",
       "detail":"game already started -- ask the host to press START GAME again"}
          (a LATE joiner -- one that joins after a start -- gets this instead
          of a start)
  lobby_state.json  a SINGLE JSON object, overwritten, mirroring the latest
      roster/state for easy polling:
      {"state","code","players","you","host","started"}
  lobby_in.jsonl    the menu APPENDS command lines; the lobby TAILS it (tracks a
      byte offset, processes only new whole lines) and acts on:
      {"cmd":"chat","text":"..."}     -> send CHAT
      {"cmd":"name","name":"..."}     -> change own username, re-JOIN/broadcast
      {"cmd":"start"}                 -> host broadcasts START save=false
                                         (no-op on a client)
      {"cmd":"start","save":<path>}   -> host pushes the save to every joiner,
                                         then broadcasts START save=true; with
                                         zero joiners it emits a status ('no
                                         players to share with') and does NOT
                                         start
      {"cmd":"quit"}                  -> leave cleanly

On startup both jsonl files are TRUNCATED so stale lines aren't reprocessed. A
JOINER also DELETES any stale incoming_save.sav / .sav.lua / .jpg in its io dir
before joining, so a previous session's save can never be mistaken for this one.

GAME RELAY (lockstep frames over the punched socket)
----------------------------------------------------
The lockstep bridge (tpf2_bridge_mp.dll) speaks plain point-to-point UDP to ONE
fixed peer address. To run it between two machines behind NAT with no new NAT
code, the menu points the bridge at 127.0.0.1:<game relay port> and the lobby
carries its frames over the socket it has ALREADY punched:

  bridge --UDP--> 127.0.0.1:P (this lobby) --NP1 DATA 'g'+frame--> peer lobby
         --UDP--> 127.0.0.1:L (the peer's bridge)

Enabled with ``--game-relay-port P --game-local-port L``:
  * a UDP socket is bound to 127.0.0.1:P; every datagram arriving there (from
    the local bridge) is forwarded over the lobby transport as a BINARY DATA
    payload whose first byte is ``GAME_RELAY_MAGIC`` (b'g' -- distinct from the
    '{' of JSON messages and the 'N' of CHUNK_MAGIC save chunks). The host
    forwards to every joiner; a joiner forwards to the host only.
  * any 'g' payload arriving from the transport is unwrapped and sent to
    127.0.0.1:L from that same socket.
  * frames over ``GAME_RELAY_MAX`` bytes are dropped with a stderr warning; a
    stats line (forwarded / delivered counts) goes to stderr every 10 s.
No ARQ here -- the bridge has its own. Without the flags nothing changes.
The menu passes P=7773 for HOST / 7774 for JOIN (distinct so two instances on
one machine can both run) and L = the port the bridge reported it bound.

RELAY-ONLY HOST (a dedicated server without a game)
----------------------------------------------------
``host --relay-only`` runs this same host loop on a machine with no game: it
is the star's centre (frames, chat, roster, save fan-out) and the master-server
announcer, but NOT a player. The oldest connected joiner is the LEADER: the
roster names it as ``host`` and carries ``relay: true`` plus a sticky
``letters`` map (name -> origin letter, a for the first joiner ever, never
reused while the relay lives), so the menu DLL gives the leader the host role
(START GAME, the hot-join sync save) and every bridge keeps its letter across
leader changes. The leader UPLOADS its save to the relay (the client-side
``start`` command drives a _HostSaveTransfer at the relay); the relay stores it
as its own incoming_save.* and pushes it to every peer that has not started,
then broadcasts ``start``. The leader accepts that start because it uploaded.

CLI
---
    python lobby.py host --name <username>          # observe, print CODE=, serve
    python lobby.py host --relay-only --name <lobby> --publish <url> --public
    python lobby.py join <CODE> --name <username>   # dial host, participate
    python lobby.py --selftest                      # 1 host + 2 joiners, loopback
    python lobby.py --selftest-transfer             # reliable save transfer
    python lobby.py --selftest-relay                # game relay both ways
    python lobby.py --print-public-list <url>       # GET <url>/list, print the body
Optional: --local-port 29471, --timeout 40, --io-dir <dir>,
          --game-relay-port <P> --game-local-port <L> (see GAME RELAY),
          --parent-pid <pid> (not on Windows: leave once that process exits).

On Linux the game starts this program itself (docs/linux/NETPUNCH.md): paths
come from linuxpaths.py, SIGTERM leaves like a quit, and --parent-pid stands in
for the Windows DLL's kill-on-close Job object.
"""

from __future__ import annotations

import argparse
from sync_lobby import HostRecovery, ClientRecovery, make_runtime
import bulk_tcp                            # the TCP side channel the save/mod transfers stream over (2026-09-17)
import dual_tcp                            # every sealed frame a second time over a TCP link, first copy wins (2026-09-17)
import netsim                              # tpf2mp_netsim.txt: loss and delay for one instance, for the rig (2026-09-17)
import collections
import hashlib
import secrets
import itertools
import json
import os
import re
import queue
import random
import select
import shutil
import signal
import socket
import struct
import sys
import tempfile
import threading
import time

# Reuse the transport verbatim -- do NOT reinvent the framing/handshake.
from punch import (
    DEFAULT_PORT, TYPE_HELLO, TYPE_ACK, TYPE_CONNECTED, TYPE_KEEPALIVE,
    TYPE_DATA, TYPE_EDATA, TYPE_ADATA, TYPE_KEYX, TOKEN_LEN, _pack, _unpack, open_socket,
)
from seal import Sealer, derive_key, SECRET_LEN
import modshare                     # share the mods a save needs (mod zips ride the save transfer)
import steamtunnel                  # Steam's own networking as a transport (native/src/steam_tunnel.cpp), 2026-09-21
import steamkey                     # the session secret over Steam when the join code is only a Steam ID, 2026-09-22
# Reuse the code exchange + the connect race + observe/announce.
from connect import decode_code, race, _observe_and_announce, encode_profile, _targets_v4, parse_hostport
from mesh import MeshNode

# --------------------------------------------------------------------------- #
# Tunables
# --------------------------------------------------------------------------- #
CAP = 200               # max players in a lobby, INCLUDING the host (origins a..z, aa..)
MAX_COMPANIES = 200     # company ids 1..200 (one addPlayer() entity each on every peer; measured 264 fine)
PING_INTERVAL = 3.0     # joiner -> host lobby keepalive cadence
DROP_AFTER = 10.0       # host drops a peer unheard-from for this long
# A mods round goes out in batches of about this much ON DISK (run_host
# start_pack_job). 192 MB until 2026-09-19: a save with 556 Workshop mods went
# out as 359 batches, most of them one mod, each a full round trip (zip, send,
# unzip, done) at about 15 s -- an hour and a half on loopback. Workshop
# vehicle packs zip 5:1, so this is ~150-200 MB on the wire per batch. The
# mods are packed smallest first, so the small ones fill a batch together
# and the big ones go alone.
MODS_BATCH_BYTES = 768 * 1024 * 1024
MODS_PACK_THREADS = 3                      # batches zipped at once; zlib releases the GIL (3 threads: 21.7 s -> 11.3 s for three 1 GB mods)
MODS_ZIP_LEVEL = 3                         # deflate level for a mods round (modshare.zip_mod)
# A joiner unpacks a batch's zips on this many threads. Measured 2026-09-20 on the
# two-instance rig, 555 mods / 93 GB in 140 batches: each batch crossed loopback
# in 0.7-1.0 s and then took 7-9 s (23 s for the 65 small mods of batch 1) to
# unpack on one thread -- ~110 MB/s -- while the host's packers ran 300 mods
# ahead. The round was paced by the joiner's unzip alone. zlib and file writes
# release the GIL, and every mod is its own zip, so they unpack side by side.
MODS_UNPACK_THREADS = 8                    # file-by-file across every zip of the batch (modshare.install_mod_zips)
TCP_CONNECT_TRIES = 3                      # a joiner's TCP connect for a transfer, before UDP takes over
TCP_CONNECT_RETRY = 0.5                    # seconds between those attempts
MODS_RATE_WINDOW = 5                       # the status line's MB/s is over this many landed batches
ROSTER_HEAL = 2.0       # host re-sends the roster this often (UDP self-heal +
                        # doubles as a host -> joiner keepalive)
HOST_GONE_AFTER = 12.0  # joiner declares the host dead after this much silence
CHAT_BURST = 3          # copies of a chat/start packet (best-effort redundancy;
                        # clients de-dupe by cid, so extras are harmless)

# --------------------------------------------------------------------------- #
# Save-transfer tunables (reliable host -> all-joiners file push)
# --------------------------------------------------------------------------- #
# The transport is best-effort UDP (loss/dup/reorder), so a reliability layer is
# built on top: chunk the file(s), sequence them, and let each RECEIVER drive
# recovery (cumulative base + selective NACKs) while the host streams within a
# flow-control window and retransmits on request. Correct under any loss pattern
# and pipelined (never stop-and-wait), so a ~200 MB save moves at link speed.
CHUNK_LOCAL = 8192          # bytes of file data per chunk when every target peer is
                            # on 127.0.0.1. 1200 exists to fit an internet MTU; on
                            # loopback it just multiplies the per-datagram cost.
                            # NOT used on a LAN: 8 KB fragments at 1500 MTU and one
                            # lost fragment loses the whole chunk.
CHUNK_STEAM = 32000         # bytes of file data per chunk when every non-loopback peer is
                            # reached through the Steam tunnel (steamtunnel.is_tunnel_addr).
                            # BIG CHUNKS OVER STEAM (2026-09-22). The tunnel sends anything over
                            # 1,200 B with Steam's RELIABLE P2P send, so Steam segments, paces and
                            # retransmits it in C++. 1,100 B unreliable chunks (0.6.1.16-0.6.1.20)
                            # moved a 134 MB save at ~1.7 MB/s on a direct P2P link with Steam's
                            # queue empty: 122k chunks, each sealed in Python, and a 2,048-chunk
                            # window (2.25 MB) that stalled for a round trip at every lost packet.
                            # The window below is bounded in BYTES (SEND_WINDOW_STEAM): the one
                            # earlier try at bigger Steam chunks stalled because it had the
                            # loopback window, 134 MB fired into Steam at once (2026-09-21).
# 32 KB chunks over Steam's reliable send stalled live three times (0.6.1.15 at 15/12781,
# 0.6.1.21 at 132/4199, 0.6.1.22 at 0/4199). The cause: bulk chunks shared the replay
# window with control frames, and a chunk delayed in Steam's reliable queue arrived after
# 64 later pings and rosters and was refused as too old (fixed in seal.py Sealer.sign,
# reproduced by tools/test_steam_save_transfer.py --steam-queue). False = 1,100 B chunks.
# 0.6.1.23 (replay windows split, send-rate ids right) STILL stalled live: base 256/4199,
# exactly 8 MB -- the Steam send/receive buffer size the tunnel sets -- then nothing, and
# no chunk refused as too old. Off by default again; a file tpf2mp_steam_big_chunks.txt
# in the data dir (or TPF2MP_STEAM_BIG_CHUNKS=1), on the HOST, turns it on to test.
def _steam_big_chunks():
    if os.environ.get("TPF2MP_STEAM_BIG_CHUNKS") == "1":
        return True
    return os.path.exists(os.path.join(modshare.data_dir(), "tpf2mp_steam_big_chunks.txt"))


STEAM_BIG_CHUNKS = True    # 0.6.1.25 experimental: exercise bounded reliable transfers without a local flag
CHUNK_STEAM_MIXED = 1100    # a transfer with Steam peers AND internet UDP peers (CROSS-PLAY):
                            # one chunk size serves everyone, and 32 KB datagrams on the open
                            # internet fragment; 1100+17+28 = 1145 B fits Steam's 1,200 B
                            # unreliable limit and every internet MTU
CHUNK_DATA = 1350           # bytes of file data per chunk (1200 until 2026-09-10: +12% per
                            # datagram; 1350+17+28 = 1395 B stays under a 1492 PPPoE MTU and
                            # a 1400 B VPN MTU; every path measured so far is v4). Wire =
                            # NP1 frame(5) + chunk header(12) + 1200 = 1217 bytes,
                            # under the 1280 IPv6 min-MTU and 1500 v4 MTU (even
                            # through PPPoE/VPN overhead) -- no fragmentation.
CHUNK_MAGIC = b"NPF1"       # 4-byte tag: a DATA payload starting with this is a
                            # binary chunk, not a JSON lobby message ('{' != 'N').
# Chunks a peer may have in flight. LOCALITY-DEPENDENT, for the same reason the
# chunk size is, and getting this wrong is worse than getting the chunk size
# wrong: the window is how much UNACKNOWLEDGED data we are willing to blast
# before hearing anything back.
#
# 16384 chunks x 1200 B is ~19.7 MB in flight against a 4 MB socket buffer
# (XFER_BUF_BYTES). On loopback that is harmless -- there is no loss and the
# receiver drains faster than we can send. Across the internet it overruns the
# receiver's buffer immediately, almost everything after the first few MB is
# dropped, the cumulative base never advances, and the transfer sits at 0% until
# it times out. Measured live 2026-09-07: a 114 MB save to a remote joiner made
# no progress at all, while the loopback selftest passed at 11 MB/s -- which is
# exactly why a loopback-only test could not catch it.
#
# The remote value is bounded by the receive buffer: 2048 x 1200 B = 2.4 MB sits
# inside the 4 MB buffer with room for reordering, and is the value every
# internet transfer before this ran on.
# THE TCP BULK CHANNEL (2026-09-17, bulk_tcp.py). BULK[0] is this process's
# listener (the host's / relay's, on the lobby port; None on a joiner and when
# the port cannot be bound); BULK_TCP[0] False keeps every transfer on UDP
# (the self-test's lossy rounds, a diagnosis). A host transfer advertises the
# listener in its fbegin, a receiver connects and the file streams; the
# feedback, verify and start messages are the same as over UDP.
BULK = [None]
BULK_TCP = [True]
# TCP FOR STEAM PEERS (2026-09-22). A peer reached through the Steam tunnel is a
# loopback endpoint here, so neither end knows the other's address to open the bulk
# TCP channel, and a save crawled through Steam instead. Over the sealed link each
# end now names its own addresses: the host in fbegin (tcp.addrs, its listener's
# port), the joiner in fbegin_ack (tcp_addrs + tcp_port of a listener it opens for
# the transfer). The joiner dials the host's, the host dials the joiner's, and the
# first stream that connects carries the file; Steam carries it if neither does.
# Only a peer already admitted to the sealed session ever sees these addresses.
MY_TCP_ADDRS = [[]]      # this machine's addresses, from its NAT observation (public, LAN, v6)
JOINER_BULK = [None]     # the joiner's own listener for a host-dialled stream (opened on first need)
_JOINER_BULK_LOCK = threading.Lock()   # one listener: Windows SO_REUSEADDR lets a second bind share the port
# A STEAM JOINER'S LISTENER WAS UNREACHABLE (2026-09-22). Joining a friend through
# Steam, the joiner offered only its LAN, VPN and 6to4 addresses (the STUN answer
# was missing from its profile) and its router had no mapping for the listener's
# port, so the host's dial could never land; with the host's own port closed to
# TCP as well, every save crawled through Steam at 0.5-1 MB/s. The joiner now maps
# its listener's TCP port by UPnP as soon as it is in through Steam and offers the
# router's WAN IP first; the mapping goes when the lobby exits.
JOINER_UPNP = {"port": None, "done": threading.Event()}


def _joiner_bulk_listener(port, log):
    """The joiner's bulk listener on ``port`` (opened once), or None."""
    with _JOINER_BULK_LOCK:
        if JOINER_BULK[0] is None:
            try:
                JOINER_BULK[0] = bulk_tcp.BulkListener.open(port, log)
            except (OSError, AttributeError):
                JOINER_BULK[0] = None
        return JOINER_BULK[0]


def _open_steam_joiner_tcp(port, log, mapper=None):
    """A joiner in through Steam: open the bulk listener now and map its TCP port
    on the router (UPnP), putting the WAN IP first in MY_TCP_ADDRS. Runs on a
    thread; JOINER_UPNP['done'] is set when it has finished either way."""
    done = JOINER_UPNP["done"]
    try:
        lst = _joiner_bulk_listener(port, log)
        if lst is None:
            return
        if mapper is None:
            from observe import upnp_map_tcp as mapper
        ok, wan, detail = mapper(lst.port)
        if ok:
            JOINER_UPNP["port"] = lst.port
        if ok and wan:
            MY_TCP_ADDRS[0] = [wan] + [a for a in MY_TCP_ADDRS[0] if a != wan][:5]
            log(f"[bulk] the router maps tcp/{lst.port} (UPnP): a host reached through Steam can dial {redact(wan)}")
        elif ok:
            log(f"[bulk] the router maps tcp/{lst.port} (UPnP) but reports no public address -- LAN/VPN offers only")
        else:
            log(f"[bulk] no UPnP mapping for tcp/{lst.port} ({detail}): the host can dial us only on the LAN/VPN "
                "or through a forwarded port; Steam carries what TCP cannot")
    except Exception as e:                           # noqa: BLE001 -- best effort, Steam still carries the save
        log(f"[bulk] TCP listener setup through Steam failed: {e!r}")
    finally:
        done.set()


def _close_steam_joiner_tcp(log):
    port = JOINER_UPNP["port"]
    if port:
        JOINER_UPNP["port"] = None
        try:
            from observe import upnp_unmap_tcp
            if upnp_unmap_tcp(port):
                log(f"[bulk] UPnP mapping for tcp/{port} removed")
        except Exception:                            # noqa: BLE001
            pass


def _profile_ips(profile):
    """The IPs an observed profile names (its candidates' 'ip:port' strings), public first."""
    out = []
    cands = (profile or {}).get("candidates") or {}
    for k in ("public_v4", "lan_v4", "vpn_v4", "vpn2_v4", "v6"):
        v = cands.get(k)
        if not isinstance(v, str) or not v:
            continue
        ip = v.rsplit(":", 1)[0].strip("[]") if ":" in v else v
        if ip and ip not in out and not ip.startswith("127."):
            out.append(ip)
    return out[:6]


def _valid_tcp_addrs(v):
    """A peer's address list as offered, filtered to plain IP strings (at most 6)."""
    import ipaddress
    out = []
    for a in (v if isinstance(v, list) else [])[:6]:
        try:
            ip = ipaddress.ip_address(str(a))
        except ValueError:
            continue
        if not ip.is_loopback and not ip.is_multicast and not ip.is_unspecified:
            out.append(str(ip))
    return out
# THE TCP BACKUP LINK (dual_tcp.py): DUAL[0] is the host's DualSocket (the joiner's
# is conn.sock); off with tpf2mp_tcp_backup.txt = 0 in the io dir or in an
# unsealed session.
DUAL = [None]


def _tcp_backup_on(io_dir):
    try:
        with open(os.path.join(io_dir, "tpf2mp_tcp_backup.txt"), "r", encoding="utf-8") as f:
            return f.read().strip() not in ("0", "off", "no")
    except OSError:
        return True


def _live_join_on(io_dir):
    """LIVE JOIN (2026-09-22): the players already in a session keep their
    worlds at a hot join; only the newcomer loads (sync_operation `retain`). On
    by default since 0.7; tpf2mp_live_join.txt = 0 in the host's io dir turns it
    off (the frozen join: everyone holds, saves, loads). Native Linux remains
    opt-in pending live validation of canonical ordering. Read at each join."""
    try:
        with open(os.path.join(io_dir, "tpf2mp_live_join.txt"), "r", encoding="utf-8") as f:
            value = f.read().strip().lower()
            if sys.platform.startswith("linux"):
                return value in ("1", "on", "yes")
            return value not in ("0", "off", "no")
    except OSError:
        return not sys.platform.startswith("linux")


def _dual_hello(name):
    """The hello a peer proves itself with: its name, sealed with the session key."""
    return dual_tcp.hello_bytes(name, SEAL[0].seal(name.encode("utf-8", "replace")))


def _dual_hello_ok(line, cipher):
    """(name) from a peer's hello when the sealed name opens and matches, else None."""
    name, sealed = dual_tcp.parse_hello(line)
    if name is None or cipher is None:
        return None
    plain = cipher.open(sealed)
    return name if plain is not None and plain.decode("utf-8", "replace") == name else None


def _match_link_hello(peers, name, addr, has_link):
    """HOST: which joiner a TCP link hello belongs to -> (peer addr, None) or (None, why).

    The joiner dials the moment its UDP punch lands, before the host has named
    it, so its hello carries the name it ASKED for. When that name was taken
    the host renamed it ('ComradeSilver' -> 'ComradeSilver#2': two instances on
    one Steam account, 2026-09-22), and matching on the assigned name alone
    closed every link: `tcp_first=0 tcp_only=0` for the whole session. So a
    hello matches the assigned name first, then the asked one, among joiners
    that have no link yet; several such joiners are told apart by the exact
    address the connection came from (a dial is bound to the lobby port), then
    by its IP. Still ambiguous -> refused: a link attached to the wrong joiner
    would hand its frames to that joiner's seal window."""
    try:
        items = list(peers.items())
    except RuntimeError:                       # the host loop changed the roster meanwhile
        return None, "the roster changed -- retrying"
    for key in ("name", "asked"):
        cands = [a for a, p in items if isinstance(p, dict) and p.get(key) == name and not has_link(a)]
        if len(cands) > 1:
            same = [a for a in cands if isinstance(a, tuple) and tuple(a[:2]) == tuple(addr[:2])] \
                or [a for a in cands if isinstance(a, tuple) and a[0] == addr[0]]
            cands = same
        if len(cands) == 1:
            return cands[0], None
        if cands:
            return None, f"{len(cands)} joiners asked for that name from {addr[0]}"
    return None, "no joiner by that name without a link"


def _impair_client(conn, io_dir, log):
    """tpf2mp_netsim.txt on a joiner: wrap its punched socket before anything
    else does (the dual socket's TCP copies are delayed the same, never dropped)."""
    sim = netsim.read_config(io_dir)
    if not sim:
        return None
    conn.sock = netsim.ImpairedSocket(conn.sock, sim, log)
    log(f"[netsim] impairing what this instance sends: {netsim.describe(sim)}")
    return sim


def _start_dual_client(conn, name, log, sim=None):
    """A joiner's TCP backup link to the host: wrap the punched socket and run
    the simultaneous open on a thread (the host is reachable through the relay,
    a UPnP TCP mapping or a port forward; a NAT that preserves ports lets the
    host's own attempt land on our listener)."""
    dsock = dual_tcp.DualSocket(conn.sock, log)
    if sim:
        dsock.link_delay = sim["delay"]
    conn.sock = dsock                    # the reader thread and the mesh pick it up on their next turn
    host_addr = conn.peer
    if steamtunnel.is_tunnel_addr(host_addr):
        log("[dual] the host is reached through Steam's networking -- no TCP link (Steam carries the frames)")
        return
    local_port = dsock.getsockname()[1]
    hello = _dual_hello(name)

    def work():
        c, how = dual_tcp.dial(host_addr, local_port, hello, log, listen_too=True)
        if c is None:
            log(f"[dual] no TCP link to the host on tcp/{host_addr[1]} within {dual_tcp.DIAL_FOR:.0f} s -- UDP only")
            return
        if how == "accepted":
            if _dual_hello_ok(dual_tcp.read_hello(c) or b"", conn.cipher) is None:
                log("[dual] the host's TCP attempt did not prove itself -- closed")
                c.close()
                return
        dsock.attach(c, host_addr, "joiner connected" if how == "connected" else "host connected")
    threading.Thread(target=work, name="dual-dial", daemon=True).start()
    return dsock
SEND_WINDOW_LOCAL  = 16384  # ~19.7 MB in flight: loopback only, no loss to lose
SEND_WINDOW_REMOTE = 2048   # ~2.4 MB, inside XFER_BUF_BYTES
# Reliable Steam data needs stalled-stream probes, not UDP window retransmits.
RESEND_AFTER_STEAM = 3.0
# TCP FIRST, STEAM AS THE FALLBACK (2026-09-22). For a peer reached through Steam the
# chunk pump holds while a TCP stream can still come (the joiner dialling the host's
# addresses, the host dialling the joiner's listener): Steam starts only when neither
# connects within TCP_FIRST_WAIT, or sooner when both ends have given up. A stream
# that breaks hands the rest to Steam as before.
TCP_FIRST_WAIT = 15.0
STEAM_WINDOW_START = 16    # grow from 512 KB using delivery acknowledgements
STEAM_RETRY_MAX = 8.0      # probe a stalled stream, never requeue its whole window
SEND_WINDOW_STEAM  = 128    # x CHUNK_STEAM = ~4 MB in flight: half the 8 MB send buffer the
                            # tunnel gives Steam (steam_tunnel.cpp SendBufferSize), and ~40 MB/s
                            # at a 100 ms round trip, over the 16 MB/s rate it allows


def _window_for(chunk):
    """The flow-control window for a chunk size: both ends derive it from the
    chunk in fbegin, so they agree how far ahead the NACK scan looks."""
    return {CHUNK_LOCAL: SEND_WINDOW_LOCAL, CHUNK_STEAM: SEND_WINDOW_STEAM}.get(chunk, SEND_WINDOW_REMOTE)
SEND_BUDGET = 256           # max datagrams sent per peer per pump() -- bounds the
                            # time one host loop iteration spends, so pings/roster
                            # for OTHER peers keep being serviced during a send.
FEEDBACK_INTERVAL = 0.05    # receiver 'fack' (base + NACKs) cadence.
BEGIN_INTERVAL = 0.2        # host re-sends 'fbegin' this often until a peer is ready.
DONE_NUDGE_INTERVAL = 1.0   # a peer that holds every chunk but has not said done gets
                            # the last chunk again this often (it answers with done)
RESEND_AFTER = 0.5          # if a peer's facks go silent this long, rewind its send
                            # cursor and re-stream the window (recovers lost facks).
PEER_XFER_TIMEOUT = 30.0    # no forward progress for this long -> skip that peer.
# A peer that HAS every byte and is verifying, writing or unpacking it gets this
# long between two facks whose progress count moved (2026-09-19: a joiner
# unpacking a 665 MB mod batch was timed out at 30 s and the whole round with
# it; a slow disk, a virus scanner or a Sandboxie overlay can hold a count
# still for longer than that). Its facks are logged every 10 s so a silent
# receiver is visible as such, not as "timed out".
PEER_XFER_VERIFY_TIMEOUT = 300.0
FINALIZE_SLICE = 8 << 20    # a receiver hashes and writes its completed save in these steps,
                            # reporting each in its facks; a slow disk moves one in well under a second
_UNSET = object()           # "no verify progress reported yet" (a reported None must differ from it)
MAX_NACK = 128              # holes a receiver reports per fack (rest next round).
DRAIN_CAP = 2048            # inbound datagrams a joiner drains per loop iteration.
HOST_DRAIN = 128            # inbound datagrams the host drains per ready cycle.
XFER_SELECT_TIMEOUT = 0.002 # host select() timeout while a transfer is active.
MAX_FILE_RETRIES = 3        # whole-file re-request attempts on a hash mismatch.
INCOMING_BASENAME = "incoming_save"   # joiner writes incoming_save.sav[.lua/.jpg]
# The ONLY names a joiner will ever write. The sender proposes names in its
# `fbegin`; anything not on this list is refused outright rather than sanitised,
# because there is no legitimate reason for a different name to arrive.
ALLOWED_INCOMING = frozenset(INCOMING_BASENAME + sfx for sfx in (".sav", ".sav.lua", ".jpg"))


def _safe_incoming_name(name):
    """True only for one of the three exact basenames we ever write.

    Rejects absolute paths, directory components, traversal, and anything else:
    the check is a whitelist of the full name, not a filter applied to it.
    """
    if not isinstance(name, str):
        return False
    if name not in ALLOWED_INCOMING and modshare.parse_mod_zip_name(name) is None:
        return False
    # belt and braces: a whitelisted constant can never contain these, so this
    # only ever fires if ALLOWED_INCOMING itself is edited carelessly later.
    return (os.path.basename(name) == name
            and not os.path.isabs(name)
            and ".." not in name.split("/") and ".." not in name.split("\\"))
XFER_BUF_BYTES = 16 * 1024 * 1024     # best-effort SO_RCVBUF/SO_SNDBUF for bursts (4 MB until 2026-09-22:
                                      # a Steam window of 4 MB arrives from the tunnel as one burst)


_buffer_cap_logged = [False]


def _boost_socket_buffers(sock):
    """Best-effort: enlarge the socket's send/recv buffers so bursty chunk
    traffic during a big save transfer isn't dropped in the kernel. Silently
    ignored where the OS clamps or rejects it -- the ARQ layer copes with loss.

    Linux clamps SO_RCVBUF to net.core.rmem_max without an error (212992 B on
    a stock kernel, under a tenth of what SEND_WINDOW_REMOTE puts in flight),
    so the clamp is logged once per process: it is the first thing to check
    when a Linux player's save transfer crawls."""
    for opt in (socket.SO_RCVBUF, socket.SO_SNDBUF):
        try:
            sock.setsockopt(socket.SOL_SOCKET, opt, XFER_BUF_BYTES)
        except (OSError, AttributeError):
            pass
    if sys.platform.startswith("linux") and not _buffer_cap_logged[0]:
        try:
            granted = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF) // 2   # Linux reports double
        except (OSError, AttributeError):
            return
        _buffer_cap_logged[0] = True
        if granted < XFER_BUF_BYTES:
            _log(f"[net] the kernel caps UDP receive buffers at {granted} B "
                 f"(net.core.rmem_max), not {XFER_BUF_BYTES}: a big save transfer may "
                 f"lose and resend more (sysctl -w net.core.rmem_max={XFER_BUF_BYTES} lifts it)")


# Everything below /24 is masked out of the logs. These files get pasted into
# bug reports and screenshots, and a player's home IP has no business travelling
# with them: "[host] JOIN ('203.0.113.47', 51299)" identifies a person's house,
# while "203.0.113.x" still tells you which peer a line is about and whether two
# lines are the same peer. Set TPF2MP_LOG_IPS=1 when you genuinely need the full
# address to debug a NAT problem.
_IPV4_RE = re.compile(r"\b(\d{1,3}\.\d{1,3}\.\d{1,3})\.(\d{1,3})\b")
_SHOW_IPS = os.environ.get("TPF2MP_LOG_IPS", "") == "1"


def redact(text):
    """Mask the host part of every IPv4 address in ``text``.

    Loopback and the RFC1918 ranges are left alone -- 127.0.0.1 and 192.168.x.y
    identify nobody, and masking them makes local debugging harder for no gain.
    """
    if _SHOW_IPS:
        return text

    def mask(m):
        head, tail = m.group(1), m.group(2)
        if head.startswith(("127.", "10.", "192.168.")) or head.startswith("169.254."):
            return m.group(0)
        if head.startswith("172."):
            second = int(head.split(".")[1] or 0)
            if 16 <= second <= 31:
                return m.group(0)
        return head + ".x"

    return _IPV4_RE.sub(mask, str(text))


_log_sinks = []       # callables(line): the log forwarder taps every line


def _log(msg):
    """Diagnostics go to stderr; stdout is reserved for the single CODE= line.
    Every line is also offered to the registered sinks (see LogForwarder), so a
    joiner's lobby/mesh/relay diagnostics reach the host's merged log."""
    line = redact(msg)
    print(line, file=sys.stderr, flush=True)
    for sink in list(_log_sinks):
        try:
            sink(line)
        except Exception:          # noqa: BLE001 -- logging must never raise
            pass


# --------------------------------------------------------------------------- #
# Log forwarding: every joiner ships its log lines to the host, which merges
# them (tagged per peer) into <io-dir>/lobby_peers.log. Extra files can be
# tailed with --forward-log PATH (e.g. the bridge log) and ride the same way.
# --------------------------------------------------------------------------- #
PEERS_LOG_NAME = "lobby_peers.log"
LOG_BATCH_BYTES = 1000     # keep a 'log' message under one datagram
LOG_LINE_MAX = 240
LOG_FLUSH_INTERVAL = 0.5
LOG_QUEUE_MAX = 2000       # oldest lines are dropped beyond this (never blocks)
LOG_MSGS_PER_SEC = 8       # host: inbound 'log' messages accepted per peer per second
LOG_BYTES_PER_PEER = 20 * 1024 * 1024   # host: merged-log bytes accepted per peer per session


class LogForwarder:
    """Collects log lines (own + tailed files) for shipping in small batches."""

    def __init__(self, tail_paths=()):
        self.q = collections.deque(maxlen=LOG_QUEUE_MAX)
        self.tails = []                          # [path, tag, offset]
        for path in tail_paths:
            # a file that already exists is tailed from its END (this session's
            # lines only); one that appears later is read from its start
            try:
                start = os.path.getsize(path)
            except OSError:
                start = 0
            self.tails.append([path, os.path.basename(path), start])
        self.last_flush = 0.0
        self.dropped = 0
        self._lock = threading.Lock()

    def add(self, line, tag=None):
        if tag:
            line = f"[{tag}] {line}"
        if len(line) > LOG_LINE_MAX:
            line = line[:LOG_LINE_MAX - 3] + "..."
        with self._lock:
            if len(self.q) == self.q.maxlen:
                self.dropped += 1
            self.q.append(line)

    def poll_tails(self):
        """Read whatever appended to each tailed file since last time."""
        for t in self.tails:
            path, tag, off = t
            try:
                size = os.path.getsize(path)
            except OSError:
                continue
            if size < off:
                t[2] = 0                          # truncated/rotated: restart
                off = 0
            if size == off:
                continue
            try:
                with open(path, "rb") as f:
                    f.seek(off)
                    data = f.read(min(size - off, 64 * 1024))
            except OSError:
                continue
            t[2] = off + len(data)
            for raw in data.splitlines():
                line = raw.decode("utf-8", "replace").rstrip()
                if line:
                    self.add(line, tag)

    def drain(self, now):
        """A batch of lines to send now, or [] (rate-limited, size-capped)."""
        if now - self.last_flush < LOG_FLUSH_INTERVAL:
            return []
        self.last_flush = now
        out, size = [], 0
        with self._lock:
            if self.dropped:
                out.append(f"[log] {self.dropped} lines dropped (queue full)")
                self.dropped = 0
            while self.q and size + len(self.q[0]) + 4 <= LOG_BATCH_BYTES:
                line = self.q.popleft()
                out.append(line)
                size += len(line) + 4
        return out


class PeersLog:
    """The host's merged log: one file, every line tagged with who logged it."""

    def __init__(self, directory):
        self.path = os.path.join(directory, PEERS_LOG_NAME)
        self._lock = threading.Lock()
        # KEEP LOGS: with <io dir>/tpf2mp_keep_logs.txt present the previous run's
        # merged log is kept and this run appends after its banner.
        keep = os.path.isfile(os.path.join(directory, "tpf2mp_keep_logs.txt"))
        if sys.platform != "win32":
            import linuxpaths
            data = linuxpaths.data_dir()
            keep = keep or bool(data and os.path.isfile(os.path.join(data, "tpf2mp_keep_logs.txt")))
        with open(self.path, "a" if keep else "w", encoding="utf-8") as f:
            f.write(("\n" if keep else "") + "# merged lobby log, started " + time.strftime("%Y-%m-%d %H:%M:%S")
                    + (" (tpf2mp_keep_logs.txt present: appending)" if keep else "") + "\n")

    def write(self, who, lines):
        stamp = time.strftime("%H:%M:%S")
        with self._lock:
            with open(self.path, "a", encoding="utf-8") as f:
                for line in lines:
                    f.write(stamp + " [" + who + "] " + line + "\n")


def _dedupe(name, taken):
    """Return ``name`` unless it collides, then ``name#2``, ``name#3``, ..."""
    name = (name or "").strip() or "player"
    if name not in taken:
        return name
    i = 2
    while f"{name}#{i}" in taken:
        i += 1
    return f"{name}#{i}"


# --------------------------------------------------------------------------- #
# File IPC: the flat-file surface the in-game menu reads/writes
# --------------------------------------------------------------------------- #
def _report_command(cmd, io, log, url_base=None):
    """A game script's request to the lobby: {"cmd":"note","text":...} shows a chat
    line from MULTIPLAYER to this player only. True when cmd was one. (The desync
    log upload that also came this way was removed on 2026-09-20: nothing leaves
    the player's machine but what they host or join with.)
    """
    c = cmd.get("cmd")
    if c == "note":
        io.emit({"type": "chat", "from": "MULTIPLAYER", "text": str(cmd.get("text", ""))[:400]})
        return True
    return False


class LobbyIO:
    """Owns lobby_out.jsonl / lobby_in.jsonl / lobby_state.json in one dir.

    * ``emit`` appends an event to lobby_out.jsonl.
    * ``write_state`` overwrites lobby_state.json atomically (merged fields).
    * ``poll_commands`` tails lobby_in.jsonl, returning only new whole lines.
    Both jsonl files are truncated on construction so stale lines don't replay.
    """

    def __init__(self, directory):
        self.dir = directory
        os.makedirs(directory, exist_ok=True)
        self.out_path = os.path.join(directory, "lobby_out.jsonl")
        self.in_path = os.path.join(directory, "lobby_in.jsonl")
        self.state_path = os.path.join(directory, "lobby_state.json")
        # Truncate the event log and the command inbox on startup.
        open(self.out_path, "w", encoding="utf-8").close()
        open(self.in_path, "w", encoding="utf-8").close()
        self._in_offset = 0
        # A fresh id per lobby run, for anything that wants to act once per session.
        self._state = {"session": os.urandom(6).hex()}
        self._lock = threading.Lock()
        self.write_state()

    def emit(self, event):
        with self._lock:
            with open(self.out_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(event) + "\n")
                f.flush()

    def _write_lobby_panel(self):
        """Small display-only snapshot for the in-game Lua UI (no join secrets)."""
        s = self._state
        def clean(value):
            return " ".join(str(value or "").split())[:160]
        state = s.get("state", "connecting")
        connected = state == "connected"
        players = s.get("players", []) if connected else []
        companies = s.get("companies", {}) or {}
        lines = ["Connected" if connected else clean(state).capitalize(),
                 clean(s.get("lobby")) or "Multiplayer lobby",
                 "Host: " + (clean(s.get("host")) or "waiting")]
        for name in players[:200]:
            tags = []
            if name == s.get("host"): tags.append("host")
            if name == s.get("you"): tags.append("you")
            suffix = " (" + ", ".join(tags) + ")" if tags else ""
            lines.append(clean(name) + suffix + " - company " + clean(companies.get(name, "-")))
        path = os.path.join(self.dir, "lobby_panel.txt")
        try:
            with open(path + ".tmp", "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(lines) + "\n")
            os.replace(path + ".tmp", path)
        except OSError:
            pass  # Advisory UI: never stop networking because it cannot be drawn.

    def write_state(self, **fields):
        with self._lock:
            self._state.update(fields)
            data = json.dumps(self._state)
            # The state mirror is advisory -- the menu reads lobby_out.jsonl, not
            # this file -- so a failed write must NEVER crash the client. Sandboxie
            # (and some overlay filesystems) reject the temp-file + os.replace dance
            # with FileNotFoundError; fall back to a direct write, and swallow even
            # that if the sandbox blocks it. Without this, a sandboxed joiner died
            # on the first 'welcome' message and got dropped by the host.
            try:
                tmp = self.state_path + ".tmp"
                with open(tmp, "w", encoding="utf-8") as f:
                    f.write(data)
                    f.flush()
                os.replace(tmp, self.state_path)
            except OSError:
                try:
                    with open(self.state_path, "w", encoding="utf-8") as f:
                        f.write(data)
                        f.flush()
                except OSError:
                    pass
            self._write_lobby_panel()

    def poll_commands(self):
        """Return a list of newly-appended command dicts (whole lines only)."""
        cmds = []
        try:
            with open(self.in_path, "rb") as f:
                f.seek(self._in_offset)
                chunk = f.read()
        except FileNotFoundError:
            return cmds
        if not chunk:
            return cmds
        nl = chunk.rfind(b"\n")
        if nl == -1:
            return cmds                       # no complete line yet; wait
        complete = chunk[:nl + 1]
        self._in_offset += len(complete)      # byte-accurate advance
        for line in complete.split(b"\n"):
            line = line.strip()
            if not line:
                continue
            try:
                cmds.append(json.loads(line.decode("utf-8")))
            except (ValueError, UnicodeDecodeError):
                pass                          # ignore malformed command lines
        return cmds


# --------------------------------------------------------------------------- #
# Sealing: when the lobby code carried a session secret, every DATA frame this
# process sends is encrypted + authenticated (seal.py) and goes out as 'E'.
# Plain 'D' frames are then refused, except the host's plain "wrong password"
# reject so a joiner learns why it is being ignored.
# --------------------------------------------------------------------------- #
SEAL = [None]               # the process-wide seal.Sealer, or None (plaintext)
# Send bulk save chunks AUTHENTICATED-ONLY (type A) instead of sealed (type E).
#
# OFF, because it is the one behavioural difference between the build that
# demonstrably transferred a save between these machines and the build that sits
# at 0% forever, and a transfer that works beats one that is 10x faster in
# theory. It is not proven guilty: the crypto round-trips correctly in isolation
# and the send/receive plumbing is identical to the sealed path. What is proven
# is that NOTHING tested it -- _run_transfer_once never sets SEAL[0], so every
# self-test ran the plaintext branch and the signed branch has never once been
# exercised end to end.
#
# Turning this back on needs a test that seals with a SEPARATE Sealer per peer
# (one process-wide Sealer makes every in-process member share a salt and a
# replay window, which breaks the join before a chunk is ever sent -- measured).
BULK_SIGN = False
# Send save chunks with NO crypto at all -- not sealed, not signed.
#
# The map file is not a secret and does not need confidentiality. Integrity is
# already guaranteed by something stronger than the frame MAC: every file in the
# transfer carries a SHA-256 that the receiver verifies before it writes
# anything, plus an overall hash, and a transfer that fails either is rejected
# and retried rather than loaded. A corrupted or injected chunk therefore costs
# a retry, not a bad save.
#
# It also takes the whole per-chunk crypto cost off the transfer: the keystream
# alone measured 60 MB/s, which on a 642 MB save is tens of seconds of CPU
# before a byte moves.
#
# Control traffic is untouched. Every join, roster, chat and start message stays
# sealed, and the receiver's plaintext carve-out is keyed on the CHUNK_MAGIC
# prefix, so nothing that is not a save chunk can arrive unauthenticated.
BULK_PLAIN = True
REJECT_PLAIN_EVERY = 2.0    # host: rate limit for the plain reject per address


def _pack_data(payload, bulk=False):
    """Frame an application payload.

    bulk=True is for SAVE CHUNKS ONLY: authenticated but not encrypted (type A).
    The keystream costs one SHA-256 per 32 bytes (60 MB/s measured) against
    614 MB/s for the HMAC protecting it, so encrypting a 642 MB save for two
    peers was ~25 s of pure CPU. Authenticity, integrity and replay protection
    are unchanged; the save's own per-file SHA-256 is still verified before
    anything is written. Only confidentiality of the map file is given up.
    Every control message (join/chat/roster/start) stays fully sealed.
    """
    if bulk and BULK_PLAIN:
        # Save chunks go out in the clear even in a sealed session. The receiver
        # only accepts plaintext whose first four bytes are CHUNK_MAGIC, so this
        # cannot be used to inject a control message.
        return _pack(TYPE_DATA, payload)
    if SEAL[0] is not None:
        if bulk and BULK_SIGN:
            return _pack(TYPE_ADATA, SEAL[0].sign(payload))
        return _pack(TYPE_EDATA, SEAL[0].seal(payload))
    return _pack(TYPE_DATA, payload)


# --------------------------------------------------------------------------- #
# Control-message fragmentation: a lobby message is as big as the lobby
# --------------------------------------------------------------------------- #
# A control message (roster, welcome, fbegin, sync_state, chat, log) used to be
# ONE datagram. The roster carries every player, every profile and every
# peer's link list -- N^2 -- and crossed 64 KB at 60-70 players, far below
# CAP: sendto raised WSAEMSGSIZE, _send_data swallowed it, and nobody got a
# roster again (2026-09-16). Anything over FRAG_DATA now goes out as
# MTU-sized fragments and is put back together on receipt. Each fragment is a
# complete NP1 frame on its own (sealed on its own in a sealed session), so
# nothing below this layer changes: the relay and the mesh carry fragments
# exactly as they carry any other DATA frame. A message that fits in one
# fragment is sent exactly as before: plain JSON.
#
#   fragment = FRAG_MAGIC(1) 'F' | id u32 | index u32 | count u32 | bytes
#
# Loss handling is the sender's, as before: the roster is re-sent every
# ROSTER_HEAL, chat and start go out CHAT_BURST times, fbegin repeats until
# acked, sync_state every 0.25 s. Nothing here retransmits.
FRAG_MAGIC = b"F"           # '{' JSON, 'N' chunk, 'g' game, 'r' relay envelope, 'F' fragment
FRAG_HEADER = struct.Struct("!III")
FRAG_DATA = 1300            # payload bytes per fragment: 1300 + 13 + seal 24 + NP1 5
                            # = 1342, under the 1400 B VPN MTU (see CHUNK_DATA)
FRAG_TTL = 15.0             # a message none of whose fragments arrived for this long is abandoned
FRAG_PENDING_PER_ADDR = 64  # partial messages kept per sender: a garbage guard, not a
                            # message limit (a sender's fragments go out back to back,
                            # so a real peer never has more than a handful open)
_frag_ids = itertools.count(int.from_bytes(os.urandom(4), "big"))   # next() is atomic: any thread may send


def _fragments(payload, limit=FRAG_DATA):
    """The frames to send for one control payload: [payload] when it fits,
    else its fragments in order."""
    if len(payload) <= limit:
        return [payload]
    fid = next(_frag_ids) & 0xFFFFFFFF
    count = (len(payload) + limit - 1) // limit
    return [FRAG_MAGIC + FRAG_HEADER.pack(fid, i, count) + payload[i * limit:(i + 1) * limit]
            for i in range(count)]


class _Reassembler:
    """Puts fragments back together per sender address. ``feed`` returns the
    whole payload once its last piece arrives, else None."""

    def __init__(self, log=None):
        self.pending = {}           # (addr, id) -> [count, {index: bytes}, last-seen]
        self.log = log or (lambda s: None)
        self.warned = 0.0

    def feed(self, addr, frame, now=None):
        now = time.time() if now is None else now
        if len(frame) < 1 + FRAG_HEADER.size:
            return None
        fid, index, count = FRAG_HEADER.unpack_from(frame, 1)
        if count == 0 or index >= count:
            return None
        key = (addr, fid)
        entry = self.pending.get(key)
        if entry is None:
            mine = [k for k in self.pending if k[0] == addr]
            if len(mine) >= FRAG_PENDING_PER_ADDR:
                oldest = min(mine, key=lambda k: self.pending[k][2])
                del self.pending[oldest]
                if now - self.warned >= 5.0:
                    self.warned = now
                    self.log(f"[frag] {addr} has {FRAG_PENDING_PER_ADDR} unfinished messages -- "
                             f"dropped the oldest (id {oldest[1]})")
            entry = self.pending[key] = [count, {}, now]
        elif entry[0] != count:
            return None                 # a different message reusing the id: ignore the stray
        entry[1][index] = frame[1 + FRAG_HEADER.size:]
        entry[2] = now
        if len(entry[1]) < count:
            return None
        del self.pending[key]
        return b"".join(entry[1][i] for i in range(count))

    def expire(self, now=None):
        now = time.time() if now is None else now
        for key in [k for k, e in self.pending.items() if now - e[2] > FRAG_TTL]:
            del self.pending[key]

    def forget(self, addr):
        for key in [k for k in self.pending if k[0] == addr]:
            del self.pending[key]


_send_failures = {}         # addr -> when a send failure was last logged for it


def _log_send_failure(addr, size, err, log=_log):
    """Every failed send is worth a line -- one swallowed WSAEMSGSIZE hid the
    roster cap for months -- but a peer that has gone away raises on every
    datagram (Windows turns ICMP port-unreachable into an exception), so the
    line is rate-limited per address."""
    now = time.time()
    if now - _send_failures.get(addr, 0.0) < 5.0:
        return
    _send_failures[addr] = now
    log(f"[net] send of {size} B to {addr} failed: {err!r}")


def _send_data(sock, addr, msg, log=_log):
    """Wrap a lobby message dict in ``NP1:`` DATA frame(s) and fire it at addr."""
    payload = json.dumps(msg).encode("utf-8")
    for piece in _fragments(payload):
        try:
            sock.sendto(_pack_data(piece), addr)
        except OSError as e:
            # A peer that has gone away raises here; the drop-timer evicts it.
            # Logged (rate-limited), never silent: a message size the socket
            # refuses must show up, not vanish.
            _log_send_failure(addr, len(payload), e, log)
            return


# --------------------------------------------------------------------------- #
# Game relay: carry the lockstep bridge's UDP frames over the punched socket
# --------------------------------------------------------------------------- #
GAME_RELAY_MAGIC = b"g"     # first byte of a relayed-frame DATA payload. JSON
                            # lobby messages start with '{' and save chunks with
                            # CHUNK_MAGIC ('N'), so one byte tells them apart.
GAME_RELAY_MAX = 1400       # largest bridge frame we relay (bytes). The bridge's
                            # Packet is ~1.05 KB; NP1(5)+'g'(1)+1400 stays under
                            # the 1500 MTU. Bigger frames are dropped + warned.
GAME_RELAY_DRAIN = 256      # loopback datagrams drained per ready cycle.
GAME_RELAY_STATS = 10.0     # seconds between stderr stats lines.
GAME_RELAY_WARN_EVERY = 5.0 # rate limit for the oversize warning.
GAME_LOCAL_PORT_DEFAULT = 7771   # the bridge's port if the menu passes none.

# --------------------------------------------------------------------------- #
# Mesh: joiners punch each other on their ONE observed socket (mesh.py) and
# fan their bridge frames out DIRECTLY; a pair with no direct path sends an
# envelope through a relay -- the host, or any peer that has a direct link to
# the destination. Exactly one copy reaches every participant either way.
# --------------------------------------------------------------------------- #
MESH_RELAY_MAGIC = b"r"     # 'r' + len(to) + to + len(frm) + frm + payload
MESH_LINKS_INTERVAL = 2.0   # joiner -> host: report my direct links this often
MESH_HI_INTERVAL = 0.5      # re-send mesh_hi on an unnamed connected link


def _relay_wrap(to, frm, payload):
    tb, fb = to.encode("utf-8"), frm.encode("utf-8")
    return (MESH_RELAY_MAGIC + bytes([len(tb)]) + tb + bytes([len(fb)]) + fb
            + payload)


def _relay_unwrap(data):
    """-> (to, frm, payload) or None."""
    try:
        i = 1
        lt = data[i]; i += 1
        to = data[i:i + lt].decode("utf-8"); i += lt
        lf = data[i]; i += 1
        frm = data[i:i + lf].decode("utf-8"); i += lf
        return to, frm, data[i:]
    except (IndexError, UnicodeDecodeError):
        return None


class _MeshHostLink:
    """A punch.Connection-shaped view of the HOST link inside a MeshNode, so
    run_client's host handling is unchanged. Traffic from any OTHER address is
    handed to ``on_peer(addr, payload)`` (set by run_client)."""

    def __init__(self, mesh, host_addr):
        self.mesh = mesh
        self.peer = host_addr
        self.sock = mesh.sock
        self.on_peer = lambda _a, _p: None

    @property
    def peer_str(self):
        return f"{self.peer[0]}:{self.peer[1]}"

    def send(self, data):
        if not self.mesh.send(self.peer, data):
            raise RuntimeError("host link send failed")

    def recv(self, timeout=None):
        end = None if timeout is None else time.time() + timeout
        while True:
            rem = None if end is None else max(0.0, end - time.time())
            r = self.mesh.recv(rem)
            if r is None:
                return None
            addr, payload = r
            if addr == self.peer:
                return payload
            try:
                self.on_peer(addr, payload)
            except Exception as e:                       # never die on one msg
                _log(f"[mesh] peer message error: {e!r}")
            if end is not None and time.time() >= end:
                return None

    def last_seen_age(self):
        return self.mesh.last_seen_age(self.peer)

    def close(self):
        self.mesh.close()


def _mesh_from_conn(conn, log=_log):
    """Take the connect race's socket away from its single-peer Connection
    and put a MeshNode on it, with the host link adopted as connected."""
    conn._stop.set()                       # stop the reader without closing
    if conn._thread.is_alive():
        conn._thread.join(timeout=1.0)
    mesh = MeshNode(conn.sock, log=log, name="joiner", cipher=getattr(conn, "cipher", None))
    mesh.adopt(conn.peer, "host", connected=True)
    return mesh, _MeshHostLink(mesh, conn.peer)


def _open_loopback_udp(port):
    """A non-blocking UDP socket bound to 127.0.0.1:``port`` -- loopback ONLY,
    the relay must never be reachable from the network. Windows' UDP
    'connection reset' on ICMP-unreachable is switched off where available so
    a bridge that isn't listening yet can't poison later recvs."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if hasattr(socket, "SIO_UDP_CONNRESET"):  # Windows only
        try:
            s.ioctl(socket.SIO_UDP_CONNRESET, struct.pack("I", 0))
        except OSError:
            pass
    s.bind(("127.0.0.1", port))
    s.setblocking(False)
    _boost_socket_buffers(s)
    return s


class GameRelay:
    """Loopback <-> lobby-transport relay for the bridge's lockstep frames.

    Owns ONE UDP socket bound to 127.0.0.1:``relay_port``. Whoever runs the
    lobby loop (host or client) does two things with it:

      * ``pump_outbound(forward)`` -- drain frames the LOCAL bridge sent to the
        relay port and hand each, prefixed with GAME_RELAY_MAGIC, to
        ``forward(payload)`` which puts it on the transport (host: to every
        joiner; joiner: to the host). ``forward`` returns how many peers it
        reached.
      * ``deliver(payload)`` -- a 'g' payload came in from the transport: strip
        the tag and send the frame to the local bridge at 127.0.0.1:``local_port``
        (from this same socket, so the bridge sees one stable source).

    ``tick(now)`` prints a stats line every GAME_RELAY_STATS seconds. The host
    folds ``sock`` into its select(); the client (whose transport is a
    queue-fed ``Connection``, not a select loop) runs ``pump_loop`` on a
    thread. No ARQ: the bridge has its own.
    """

    def __init__(self, relay_port, local_port, log=_log):
        self.relay_port = int(relay_port)
        self.local_addr = ("127.0.0.1", int(local_port))
        self.log = log
        self.sock = _open_loopback_udp(self.relay_port)
        self.forwarded = 0          # loopback -> transport (frames)
        self.delivered = 0          # transport -> loopback (frames)
        self.no_peer = 0            # local frames with nobody to send to
        self.oversize = 0           # local frames dropped for size
        self._last_warn = 0.0
        self._last_stats = time.time()
        self._last_counts = (0, 0)
        self.log(f"[relay] game relay up: bridge -> 127.0.0.1:{self.relay_port} "
                 f"-> transport -> peer; peer -> 127.0.0.1:{self.local_addr[1]}")

    @staticmethod
    def is_game(payload):
        return payload[:1] == GAME_RELAY_MAGIC

    def pump_outbound(self, forward):
        """Drain up to GAME_RELAY_DRAIN local frames; returns how many were read."""
        n = 0
        while n < GAME_RELAY_DRAIN:
            try:
                data, _src = self.sock.recvfrom(65535)
            except BlockingIOError:
                break
            except ConnectionResetError:
                continue                     # Windows ICMP echo of our own send
            except OSError:
                break
            n += 1
            if len(data) > GAME_RELAY_MAX:
                self.oversize += 1
                now = time.time()
                if now - self._last_warn >= GAME_RELAY_WARN_EVERY:
                    self._last_warn = now
                    self.log(f"[relay] WARNING: dropped {len(data)}-byte frame "
                             f"from the bridge (cap {GAME_RELAY_MAX} bytes; "
                             f"{self.oversize} dropped so far)")
                continue
            try:
                reached = forward(GAME_RELAY_MAGIC + data)
            except (RuntimeError, OSError):
                reached = 0
            if reached:
                self.forwarded += 1
            else:
                self.no_peer += 1
        return n

    def deliver(self, payload):
        """A 'g' payload from the transport -> the local bridge."""
        try:
            self.sock.sendto(payload[1:], self.local_addr)
        except OSError:
            return
        self.delivered += 1

    def tick(self, now):
        if now - self._last_stats < GAME_RELAY_STATS:
            return
        dt = now - self._last_stats
        self._last_stats = now
        f0, d0 = self._last_counts
        self._last_counts = (self.forwarded, self.delivered)
        self.log(f"[relay] stats: forwarded={self.forwarded} "
                 f"(+{self.forwarded - f0}) delivered={self.delivered} "
                 f"(+{self.delivered - d0}) in {dt:.0f}s; "
                 f"no_peer={self.no_peer} oversize={self.oversize}")

    def pump_loop(self, forward, stop):
        """Client-side pump: select() on the loopback socket until ``stop``."""
        while not stop.is_set():
            try:
                ready, _, _ = select.select([self.sock], [], [], 0.2)
            except (OSError, ValueError):
                break
            if ready:
                self.pump_outbound(forward)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- #
# Reliable save transfer (host -> every joiner), layered on the DATA channel
# --------------------------------------------------------------------------- #
# Wire additions (all inside NP1 DATA frames):
#   JSON control messages (keep the "t" convention):
#     fbegin      host  -> joiner : {t, sid, total_bytes, chunk, total_chunks,
#                                    files:[{name,size,sha256}], sha256}
#     fbegin_ack  joiner-> host   : {t, sid}                (receiver allocated)
#     fack        joiner-> host   : {t, sid, base, nack:[seq...]}  (feedback)
#     fdone       joiner-> host   : {t, sid, ok[, final]}   (verified / gave up)
#   BINARY chunk (host -> joiner), NOT JSON, to avoid base64 bloat:
#     CHUNK_MAGIC(4) + sid(uint32 BE) + seq(uint32 BE) + up-to-CHUNK_DATA bytes
#
# All files (.sav + optional .sav.lua + .jpg) are concatenated into ONE byte
# stream with a single sequence space; the receiver splits them back out using
# the per-file sizes in `fbegin`. Integrity is SHA-256 per file AND overall.
# Required mods are offered to joiners; approval and engine registration gate loading.
SHARE_MODS = [True]
MODS_ANSWER_WAIT = 90.0    # s the host waits for a joiner to answer the download prompt
# THE WORKSHOP FIRST (2026-09-22). A joiner that lacks some of the host's Workshop mods
# subscribes to them through Steam (the bridge's tunnel, steam_tunnel.cpp UgcCommand)
# and registers what Steam installs, exactly as a mod already on disk is registered;
# the host sends only local mods and whatever Steam could not deliver. The player's
# YES to the mods prompt covers both (subscribing is still installing code).
UGC_SUBSCRIBE_WAIT = 20.0  # s for Steam to confirm a subscription before the host's copy is asked for
UGC_STALL = 120.0          # s without a byte of progress before the rest falls back to the host
UGC_POLL_EVERY = 1.0       # s between state polls
_UGC = [None]              # the tunnel client the Workshop requests go through (cached once up)


def _ugc_tunnel():
    """The Steam tunnel for Workshop requests, or None (no tunnel: the host sends everything)."""
    t = _UGC[0]
    if t is None or not t.available:
        t = steamtunnel.SteamTunnel(modshare.data_dir(), _log)
        if not t.available:
            return None
        _UGC[0] = t
    return t


def _workshop_item(name):
    """(mod id, version, Workshop id) for a folder name '*<id>_<ver>', else None."""
    m, sep, v = str(name).rpartition("_")
    if not sep or not m.startswith("*") or not m[1:].isdigit() or not v.isdigit():
        return None
    return m, int(v), m[1:]
MOD_DISPLAY_NAME = "Transport Fever 2 Multiplayer"   # the mod's name in the game's mod list
_mod_refusal_notes = {}                               # save path -> when the chat was last told


def _save_has_mp_mod(save_path):
    """Was this save made with our mod enabled? True, False, or None when
    there is no way to tell.

    The .sav itself is Zstandard-compressed, but the game writes a plain
    <name>.sav.lua beside it holding one entry per game script that ran,
    keyed by the script's file name. Ours is lockstep.lua, so the entry is
    there exactly when the mod was on: on the saves checked (2026-09-10) it
    matched the mod list inside the compressed .sav every time."""
    try:
        with open(os.path.abspath(save_path) + ".lua", "rb") as f:
            return b'["lockstep.lua"]' in f.read()
    except OSError:
        return None


def _mod_check(save_path, io, log):
    """True when ``save_path`` may be shared. A save made without the mod is
    refused: nothing in it would replicate, and every player would load a
    world that silently never syncs. The panel's status line is one short
    row, so the full explanation goes to the chat log too (at most once a
    minute per save: the relay leader's periodic upload retries every 2 min)."""
    has = _save_has_mp_mod(save_path)
    if has is None:
        log(f"[host] {save_path}: no .sav.lua beside it -- cannot tell whether the mod is on; sharing anyway")
        return True
    if has:
        return True
    label = os.path.basename(save_path)
    if label.lower().endswith(".sav"):
        label = label[:-4]
    log(f"[host] NOT sharing {save_path}: made without the {MOD_DISPLAY_NAME} mod (no lockstep.lua entry in its .sav.lua)")
    io.emit({"type": "status", "state": "connected",
             "detail": f"Not shared: '{label}' does not have the {MOD_DISPLAY_NAME} mod enabled (see chat)"})
    now = time.time()
    if now - _mod_refusal_notes.get(save_path, 0.0) > 60.0:
        _mod_refusal_notes[save_path] = now
        io.emit({"type": "chat", "from": "MULTIPLAYER",
                 "text": f"'{label}' was saved without the {MOD_DISPLAY_NAME} mod, so nothing would sync. "
                         "Load it, enable the mod in its Mods panel on the load screen, save, and press START GAME again."})
    return False


def _host_missing_mods(mods, lookup=None):
    """The (id, version) pairs of ``mods`` that are nowhere on THIS PC: not in a
    Steam Workshop folder, not a managed download, not in the game's mods or
    dlcs folders (the host's own lookup, modshare.find_mod)."""
    lookup = lookup or modshare.find_mod
    return [(m, v) for m, v in mods if modshare.valid_mod(m, v) and lookup(m, v) is None]


def _host_mods_check(save_path, mods, io, log, lookup=None):
    """True when the host's own game can load ``save_path``: every mod the save
    needs is on this PC. Otherwise the start is refused and the missing mods are
    NAMED. Until 2026-09-20 the host pushed the save, every joiner loaded it, and
    the host's own game refused it with nothing but "Couldn't start the shared
    save by itself -- open LOAD GAME" (the host had unsubscribed from 557 Workshop
    items between two sessions; the game found 29 mods where the save wanted
    563). ``mods`` is the list save_mod_list read (None = unknown: nothing to
    check, the transfer goes ahead as before)."""
    if not mods:
        return True
    missing = _host_missing_mods(mods, lookup)
    if not missing:
        return True
    label = os.path.basename(str(save_path))
    if label.lower().endswith(".sav"):
        label = label[:-4]
    names = [modshare.mod_folder_name(m, v) for m, v in missing]
    shown = ", ".join(names[:8]) + (f", ... ({len(names) - 8} more)" if len(names) > 8 else "")
    log(f"[host] NOT sharing {save_path}: it needs {len(names)} mod(s) this PC does not have, "
        f"so the host's own game could not load it: {', '.join(names)}")
    io.emit({"type": "status", "state": "connected",
             "detail": f"Not started: '{label}' needs {len(names)} mod(s) not installed on this PC (see chat)"})
    now = time.time()
    key = ("host-mods", save_path)
    if now - _mod_refusal_notes.get(key, 0.0) > 60.0:
        _mod_refusal_notes[key] = now
        io.emit({"type": "chat", "from": "MULTIPLAYER",
                 "text": f"'{label}' needs {len(names)} mod(s) that are not installed on this PC, so your own game "
                         f"cannot load it: {shown}. Subscribe to them on the Steam Workshop (or put them in the "
                         "game's mods folder), let Steam finish downloading, then press START GAME again."})
    return False


def _read_save_files(save_path):
    """Read the .sav and any sidecars; return (blob, files_meta).

    ``blob`` is the concatenation (order: .sav, .sav.lua, .jpg). ``files_meta``
    is the list the host advertises in ``fbegin`` -- each entry names the file
    with the joiner's target basename (incoming_save.*) plus its size + sha256.
    Raises on a missing/unreadable .sav.
    """
    save_path = os.path.abspath(save_path)
    if not os.path.isfile(save_path):
        raise FileNotFoundError(f"save not found: {save_path}")
    if save_path.lower().endswith(".sav"):
        stem = save_path[:-4]
    else:
        stem = os.path.splitext(save_path)[0]
    sources = [(save_path, INCOMING_BASENAME + ".sav")]
    lua = save_path + ".lua"                      # <name>.sav.lua
    jpg = stem + ".jpg"                           # <name>.jpg
    if os.path.isfile(lua):
        sources.append((lua, INCOMING_BASENAME + ".sav.lua"))
    if os.path.isfile(jpg):
        sources.append((jpg, INCOMING_BASENAME + ".jpg"))
    blob = bytearray()
    files_meta = []
    for src, logical in sources:
        with open(src, "rb") as f:
            data = f.read()
        files_meta.append({"name": logical, "size": len(data),
                           "sha256": hashlib.sha256(data).hexdigest()})
        blob += data
    # Return the bytearray as-is (no bytes() copy) -- for a ~200 MB save that
    # avoids a transient second 200 MB allocation; slicing it per chunk works.
    return blob, files_meta


def _keepalive_sweep(peers, now, drop_after, transfers, log, exempt=()):
    """The host's keepalive eviction: the addresses of the peers to drop --
    silent for longer than ``drop_after`` and NOT mid-transfer. A peer that
    is receiving a save is judged by the transfer's own PEER_XFER_TIMEOUT,
    never by the lobby keepalive: a joiner verifying and writing a 1 GB save
    on an HDD (plus a mod unzip) went quiet for longer than DROP_AFTER and
    was evicted mid-transfer, which failed the transfer for everyone
    (2026-09-16). Such a peer is logged once (drop_deferred) until it is
    heard from again."""
    dead = []
    for a, p in peers.items():
        if now - p["last"] <= drop_after:
            p.pop("drop_deferred", None)
            continue
        if _mid_transfer(a, *transfers) or a in exempt:
            if not p.get("drop_deferred"):
                p["drop_deferred"] = True
                log(f"[host] {p['name']} silent for {now - p['last']:.0f} s but mid-transfer "
                    "-- not dropped (the transfer's own timeout decides)")
            continue
        dead.append(a)
    return dead


def _mid_transfer(addr, *transfers):
    """True while ``addr`` is an ACTIVE target of any of the given
    _HostSaveTransfer objects (None entries are skipped). The host's keepalive
    eviction defers to this: such a peer is judged by PEER_XFER_TIMEOUT."""
    for t in transfers:
        if t is None:
            continue
        p = getattr(t, "peers", {}).get(addr)
        if p is not None and p.get("state") == "active":
            return True
    return False


def _merge_sender_stage(current, text, pct):
    """What the roster should say for a peer when the host's own SAVE SENDER
    reports ``text`` (pct = the send progress, None = the transfer is done),
    given the peer's current stage: the new text, or None to leave it.

    The joiner's own "receiving save M%" wins while it is at least as far
    along (its report is the newer one); anything else the peer said is older
    than a transfer that is running now, except a load already under way
    ("loading world ..."), which only the peer's own DLL can see (2026-09-16).
    """
    current = current or ""
    if current.startswith("loading world"):
        return None
    if pct is None:
        return text
    m = re.match(r"receiving save (\d+)%$", current)
    if m and int(m.group(1)) >= pct:
        return None
    return text


class _HostSaveTransfer:
    """Reliable host -> all-joiners file push, PUMPED from the host's main loop.

    No hard size cap (real TF2 saves are ~200 MB); the whole concatenation is
    held in memory once and fanned out to every peer. Reliability is
    receiver-driven selective repeat:

      * the host streams chunks within a flow-control window (self.window);
      * each receiver periodically reports its cumulative ``base`` (every chunk
        below base is in hand) plus an explicit ``nack`` list of holes;
      * the host drops everything below base, retransmits NACKed chunks first,
        then sends new in-order chunks up to base+window;
      * if a receiver goes silent (its facks were lost) the host rewinds and
        re-streams the window, so it self-heals under ANY loss pattern.

    ``pump()`` sends at most SEND_BUDGET datagrams PER PEER, so it never hogs the
    single demux loop -- keepalives/roster for other peers keep flowing during a
    transfer. All ACK-side messages arrive through the host's normal recv path
    and are routed in via on_begin_ack / on_fack / on_fdone.
    """

    @staticmethod
    def _pick_chunk(targets):
        """Big chunks when nobody is across the internet.

        The chunking is decided ONCE for the whole transfer (one blob, one
        sequence space shared by every peer), so this is all-or-nothing: a
        single non-loopback peer puts everyone back on the MTU-safe size.

        LOOPBACK ONLY, deliberately. An 8 KB datagram on a 1500-MTU LAN is IP
        fragmented into ~6 pieces and losing any one loses the whole chunk, so
        a bigger chunk would make a lossy wifi link WORSE. On 127.0.0.1 there
        is no MTU to speak of and no loss, and the win is real: a 642 MB save
        is 535k chunks at 1200 B against 78k at 8192 B, each costing a sign, a
        pack and a sendto in single-threaded Python.
        """
        tunnel = internet = False
        for addr, _name in targets:
            if steamtunnel.is_tunnel_addr(addr):
                tunnel = True
                continue
            host = addr[0] if isinstance(addr, tuple) else str(addr)
            if not host.startswith("127."):
                internet = True
        if tunnel:
            big = _steam_big_chunks() if STEAM_BIG_CHUNKS is None else STEAM_BIG_CHUNKS
            return CHUNK_STEAM if big and not internet else CHUNK_STEAM_MIXED
        return CHUNK_DATA if internet else CHUNK_LOCAL

    @staticmethod
    def _pick_window(chunk):
        """The window follows the same all-or-nothing locality call as the chunk.

        Tied to the chunk size rather than re-deriving locality so the two can
        never disagree: a big window with MTU-safe chunks is precisely the
        combination that stalled a real transfer.
        """
        return _window_for(chunk)

    def __init__(self, sock, sid, blob, files_meta, targets, io, log, mods=None, kind="save", stage_cb=None, extra=None):
        self.sock = sock
        self.kind = kind                      # "save" or "mods" (the round after it)
        # The host loop's view of how far each peer's SAVE is, as this sender
        # sees it, so the roster shows "receiving save N%" before the joiner
        # itself reports it (2026-09-16): stage_cb(name, text, pct), pct None
        # once the peer verified the file. Only the save round: a mods round
        # is the player's own download, reported by the receiver.
        self.stage_cb = stage_cb if kind == "save" else None
        # [(id, ver)] the save needs, told in fbegin. None means the host could
        # not READ the list (modshare.save_mod_list failed): the save still
        # goes out, but every receiver is told the list is unknown rather than
        # empty -- "or []" here used to advertise such a save as needing
        # nothing (2026-09-16).
        self.mods_unknown = mods is None
        self.mods = list(mods or [])
        self.progress_at = time.time()        # last sign of a receiver working (base advance, verify heartbeat, done)
        self.sid = sid
        self.blob = blob
        self.total_bytes = len(blob)
        self.chunk = self._pick_chunk(targets)
        self.window = self._pick_window(self.chunk)
        self.total_chunks = (self.total_bytes + self.chunk - 1) // self.chunk
        self.files_meta = files_meta
        self.overall_sha = hashlib.sha256(blob).hexdigest()
        self.io = io
        self.log = log
        self.begin_msg = {"t": "fbegin", "sid": sid,
                          "total_bytes": self.total_bytes, "chunk": self.chunk,
                          "total_chunks": self.total_chunks,
                          "files": files_meta, "sha256": self.overall_sha,
                          "kind": kind, "mods": [[m, v] for m, v in self.mods],
                          "mods_unknown": self.mods_unknown}
        if extra:
            self.begin_msg.update(extra)          # "batch": [k, n] for a mods round sent in batches
        # THE TCP CHANNEL. A per-transfer token rides in the (sealed) fbegin; a
        # receiver that can reach our listener connects with it and the file
        # streams (_tcp_serve). With no listener here (a joiner uploading to the
        # relay) the RELAY's fbegin_ack names its listener and we connect to it
        # instead (_tcp_push, on_begin_ack). A peer on the stream is skipped by
        # the UDP pump; its feedback still drives base, stage and timeouts.
        self.tcp_token = os.urandom(16).hex() if BULK_TCP[0] else None
        self.tcp_bytes = 0
        self._tcp_lock = threading.Lock()
        if self.tcp_token and BULK[0] is not None:
            self.begin_msg["tcp"] = {"port": BULK[0].port, "token": self.tcp_token}
            if MY_TCP_ADDRS[0] and any(steamtunnel.is_tunnel_addr(a) for a, _ in targets):
                self.begin_msg["tcp"]["addrs"] = list(MY_TCP_ADDRS[0])   # a Steam peer cannot see where we are
            BULK[0].expect(sid, "recv", self.tcp_token, self._tcp_serve)
        elif self.tcp_token:
            self.begin_msg["tcp"] = {"token": self.tcp_token}     # no listener: the other side may offer one
        now = time.time()
        self.peers = {}          # addr -> per-peer send state
        for addr, name in targets:
            self.peers[addr] = {
                "name": name, "ready": False, "base": 0, "next": 0,
                "nack": [], "last_fack": now, "last_begin": 0.0,
                "last_resend": 0.0, "last_advance": now,
                "state": "active", "last_pct": -1, "need": [], "ask": False, "ask_since": 0.0, "ask_logged": False,
                "tcp": False, "tcp_tried": False,
            }
        self.log(f"[host] save transfer sid={sid} {self.total_bytes}B in "
                 f"{self.total_chunks} chunks of {self.chunk}B "
                 f"({'local' if self.chunk == CHUNK_LOCAL else 'steam' if self.chunk == CHUNK_STEAM else 'steam+internet' if self.chunk == CHUNK_STEAM_MIXED else 'internet-safe'}) "
                 f"-> {len(self.peers)} peer(s)")

    # -- the TCP channel --------------------------------------------------- #
    def _peer_for_stream(self, addr, name):
        """The peer record a connection is for: by the name it said, else the
        one peer at that address (a NAT shows the UDP and TCP sides alike)."""
        for a, p in self.peers.items():
            if p["state"] == "active" and not p["tcp"] and name and p["name"] == name:
                return a, p
        same = [(a, p) for a, p in self.peers.items()
                if p["state"] == "active" and not p["tcp"] and isinstance(a, tuple) and a[0] == addr[0]]
        return same[0] if len(same) == 1 else (None, None)

    def _tcp_serve(self, sock, addr, name):
        """ACCEPT THREAD HELPER: a receiver connected to our listener."""
        _, p = self._peer_for_stream(addr, name)
        if p is None:
            self.log(f"[host] a TCP stream from {addr[0]} ({name!r}) matches no waiting receiver -- closed")
            sock.close()
            return
        self._tcp_stream(sock, p)

    def _tcp_push(self, ip, port, p):
        """A thread: WE connect (a joiner's upload to the relay's listener; a Steam
        joiner's own listener, ip then a list of the addresses it named)."""
        ips = ip if isinstance(ip, list) else [ip]
        errs = []
        for one in ips:
            if p["tcp"] or p["state"] != "active":
                return                          # its stream to us won the race
            sock = bulk_tcp.bulk_connect(one, port, "send", self.sid, self.tcp_token, p["name"], errors=errs)
            if sock is not None:
                self.log(f"[host] {p['name']}: connected to its TCP listener at {redact(one)}:{port}")
                self._tcp_stream(sock, p)
                return
        p["push_pending"] = False
        self.log(f"[host] {p['name']}: no TCP stream to {', '.join(redact(i) for i in ips)} port {port} -- "
                 + ("Steam carries the rest" if len(ips) > 1 or steamtunnel.is_tunnel_addr(p.get('addr')) else "the upload runs over UDP")
                 + (f" [{redact('; '.join(errs))}]" if errs else ""))

    def _tcp_stream(self, sock, p):
        with self._tcp_lock:                    # both ends may dial: the first stream wins
            if p["tcp"]:
                try:
                    sock.close()
                except OSError:
                    pass
                return
            p["tcp"] = True
        p["ready"] = True
        p["tcp_done"] = 0.0
        self.log(f"[host] {p['name']} takes the {self.kind} over TCP")
        self.io.emit({"type": "transfer", "role": "send", "peer": p["name"], "state": "tcp"})
        t0 = time.time()
        ok = bulk_tcp.stream_send(sock, self.blob, lambda n: p.__setitem__("tcp_sent", n))
        self.tcp_bytes += p.get("tcp_sent", 0)
        if ok:
            # the receiver may still be feeding the last blocks to its buffer:
            # stay off the UDP pump until its feedback says so (on_fack clears
            # the flag if the base stops short of the end for a while)
            p["tcp_done"] = time.time()
            self.log(f"[host] {p['name']}: sent over TCP, {bulk_tcp.rate_text(self.total_bytes, time.time() - t0)}")
        else:
            # the receiver's feedback names what is missing; the UDP pump resumes from its base
            p["tcp"] = False
            self.log(f"[host] {p['name']}: the TCP stream broke after {p.get('tcp_sent', 0)} B -- UDP takes over")

    def _stage_word(self):
        """'mods k/n' or 'mods' -- the roster's 120 px stage column."""
        b = (self.begin_msg or {}).get("batch")
        if isinstance(b, list) and len(b) == 2:
            return f"mods {b[0]}/{b[1]}"
        return "mods"

    def _what(self):
        """'save', 'mods', or 'mod batch k/n' -- for the log."""
        b = (self.begin_msg or {}).get("batch")
        if self.kind == "mods" and isinstance(b, list) and len(b) == 2:
            return f"mod batch {b[0]}/{b[1]}"
        return self.kind

    # -- progress ---------------------------------------------------------- #
    def _emit_pct(self, p):
        if self.total_bytes == 0:
            pct = 100
        else:
            done = min(p["base"] * self.chunk, self.total_bytes)
            pct = int(done * 100 // self.total_bytes)
        if pct // 10 > p["last_pct"] // 10:
            p["last_pct"] = pct
            if self.kind != "mods":
                # The menu prints every transfer event over its status line
                # ("Sending save... N%", then "Save transfer complete."). A mods
                # batch crosses in under a second and the round's own status
                # line (landed GB, MB/s, time left) is the one to keep in view.
                self.io.emit({"type": "transfer", "role": "send",
                              "peer": p["name"], "pct": pct})
            if self.stage_cb:
                self.stage_cb(p["name"], (f"{self._stage_word()} {pct}%" if self.kind == "mods"
                                          else f"receiving save {pct}%"), pct)

    # -- outbound chunk ---------------------------------------------------- #
    def _send_chunk(self, addr, seq):
        off = seq * self.chunk
        data = self.blob[off:off + self.chunk]
        frame = CHUNK_MAGIC + struct.pack("!II", self.sid, seq) + data
        try:
            self.sock.sendto(_pack_data(frame, bulk=True), addr)
        except OSError:
            pass                      # kernel buffer full etc.; ARQ will re-send

    # -- inbound ACK routing (called from the host recv loop) -------------- #
    def on_begin_ack(self, addr, msg):
        p = self.peers.get(addr)
        if not p or msg.get("sid") != self.sid:
            return
        if not p["ready"]:
            if self.chunk == CHUNK_STEAM:
                p["last_advance"] = time.time()
            p["ready"] = True
            self.log(f"[host] {p['name']} ready for {self.kind}")
        # the receiver has a listener (the relay, for our upload): connect and stream
        port = msg.get("tcp_port")
        if self.tcp_token and isinstance(port, int) and 0 < port < 65536 and not p["tcp"] and not p["tcp_tried"] \
                and isinstance(addr, tuple) and not steamtunnel.is_tunnel_addr(addr):
            p["tcp_tried"] = True
            threading.Thread(target=self._tcp_push, args=(addr[0], port, p), name="bulk-push", daemon=True).start()
        elif self.tcp_token and isinstance(port, int) and 0 < port < 65536 and not p["tcp"] and not p["tcp_tried"] \
                and steamtunnel.is_tunnel_addr(addr):
            ips = _valid_tcp_addrs(msg.get("tcp_addrs"))
            if ips:
                p["tcp_tried"] = True
                p["addr"] = addr
                p["push_pending"] = True
                self.log(f"[host] {p['name']} (through Steam) offers a TCP listener on {len(ips)} address(es) -- dialling")
                threading.Thread(target=self._tcp_push, args=(ips, port, p), name="bulk-push", daemon=True).start()
        if steamtunnel.is_tunnel_addr(addr) and not p["tcp"] and "tcp_first_until" not in p:
            pull = bool(msg.get("tcp_pull"))           # the joiner is dialling our addresses
            if pull or p.get("push_pending"):
                p["tcp_first_until"] = time.time() + TCP_FIRST_WAIT
                p["pull_pending"] = pull
                self.log(f"[host] {p['name']}: TCP first ("
                         + " and ".join(w for w, on in (("it dials us", pull), ("we dial it", p.get("push_pending"))) if on)
                         + f"), Steam after {TCP_FIRST_WAIT:.0f} s if no stream connects")
        need = msg.get("need")
        if isinstance(need, list):
            # the ids this joiner does not have installed, out of self.mods
            want = {modshare.mod_folder_name(m, v): (m, v) for m, v in self.mods}
            p["need"] = [want[n] for n in need if isinstance(n, str) and n in want]
            p["ask"] = bool(msg.get("ask")) and bool(p["need"])
            p["ask_since"] = time.time()
            if p["need"]:
                self.log(f"[host] {p['name']} lacks {len(p['need'])} mod(s): "
                         + ", ".join(modshare.mod_folder_name(m, v) for m, v in p['need'])
                         + (" -- waiting for their yes/no" if p["ask"] else ""))

    def on_tcp_gave_up(self, addr, msg):
        """The joiner could not reach any address we named: its dialling is over."""
        p = self.peers.get(addr)
        if p and msg.get("sid") == self.sid and p.get("pull_pending"):
            p["pull_pending"] = False
            self.log(f"[host] {p['name']} could not reach us over TCP")

    def on_fack(self, addr, msg):
        p = self.peers.get(addr)
        if not p or msg.get("sid") != self.sid or p["state"] != "active":
            return
        now = time.time()
        if self.chunk == CHUNK_STEAM and not p["ready"]:
            p["last_advance"] = now
        p["ready"] = True
        p["last_fack"] = now
        base = int(msg.get("base", 0))
        if p["tcp"] and p.get("tcp_done") and base < self.total_chunks and now - p["tcp_done"] > 5.0 \
                and now - p["last_advance"] > 2.0:
            # the stream ended but the receiver's base stopped short: whatever
            # is missing comes over UDP from here
            p["tcp"] = False
            self.log(f"[host] {p['name']}: base {base}/{self.total_chunks} after the TCP stream -- UDP fills the rest")
        if base > p["base"]:
            if self.chunk == CHUNK_STEAM:
                p["steam_window"] = min(self.window, p.get("steam_window", STEAM_WINDOW_START) + base - p["base"])
                p["steam_retry_delay"] = RESEND_AFTER_STEAM
            p["base"] = base
            p["last_advance"] = now
            self.progress_at = now
            if p["next"] < base:
                p["next"] = base
        elif base >= self.total_chunks:
            # Everything is delivered and the receiver is verifying and
            # writing it (a 1 GB save on an HDD, then a mod unzip). Its facks
            # keep coming while it works (base = total, "verifying": true,
            # "progress": bytes hashed/written/unpacked so far), and a MOVED
            # progress count is progress: the 30 s no-advance timeout used to
            # fail exactly the peers that had the whole file and were busiest
            # with it (2026-09-16). A fack whose count has not moved is not:
            # a worker stuck in a disk hang or a wedged unzip would otherwise
            # hold transfer[0] (and every resync) open for ever.
            mark = msg.get("progress")
            p["verifying"] = bool(msg.get("verifying"))
            if mark != p.get("verify_progress", _UNSET):
                p["verify_progress"] = mark
                p["last_advance"] = now
                self.progress_at = now
            if now - p.get("verify_said", 0.0) >= 10.0:
                p["verify_said"] = now
                self.log(f"[host] {p['name']} has the whole {self.kind}; verifying/unpacking, {mark if mark is not None else '?'} B so far")
        elif base < p["base"]:
            # REWIND: the receiver restarted from scratch (hash mismatch ->
            # whole-file re-request). Without this the host would filter every
            # re-reported hole as "below base" and never resend -- the peer
            # would just sit there until PEER_XFER_TIMEOUT. Honour the new base,
            # re-stream from it, and drop the now-meaningless pending holes.
            self.log(f"[host] {p['name']} rewound save cursor "
                     f"{p['base']} -> {base} (receiver re-requested the file)")
            p["base"] = base
            p["next"] = base
            p["nack"] = []
            p.pop("steam_retry_at", None)
            p.pop("steam_started_at", None)
            p["steam_window"] = STEAM_WINDOW_START
            p["steam_retry_delay"] = RESEND_AFTER_STEAM
            p["last_advance"] = now
            p["last_pct"] = -1
        # Merge the reported holes with any still-pending ones (all >= base).
        holes = {s for s in msg.get("nack", [])
                 if isinstance(s, int) and p["base"] <= s < self.total_chunks}
        holes |= {s for s in p["nack"] if s >= p["base"]}
        p["nack"] = sorted(holes)
        if self.chunk == CHUNK_STEAM:
            # With <=128 outstanding chunks MAX_NACK covers the entire window.
            # A received chunk beyond a hole distinguishes local loss from a
            # reliable Steam queue which simply has not delivered anything yet.
            p["steam_missing"] = set(p["nack"])
        self._emit_pct(p)

    def progress_tokens(self):
        """(member name, token) per receiver, the token changing whenever that
        receiver's part of the transfer moved: its chunk cursor, its
        verify/write count, its final state. The host feeds these to the
        resync barrier per member (sync_lobby.HostRecovery.tick)."""
        return [(p["name"], "transfer:%s/%s/%s" % (p["base"], p.get("verify_progress", ""), p["state"]))
                for p in self.peers.values()]

    def on_fdone(self, addr, msg):
        p = self.peers.get(addr)
        if not p or msg.get("sid") != self.sid:
            return
        if msg.get("ok"):
            if p["state"] == "active":
                p["state"] = "done"
                self.progress_at = time.time()
                if self.kind != "mods":
                    self.io.emit({"type": "transfer", "role": "send",
                                  "peer": p["name"], "state": "done"})
                self.log(f"[host] {p['name']} verified {self._what()} transfer")
                if self.stage_cb:
                    self.stage_cb(p["name"], "save received, loading" if self.kind != "mods"
                                  else f"{self._stage_word()} done", None)
        elif msg.get("final"):
            if p["state"] == "active":
                p["state"] = "failed"
                # Per-peer outcome is a 'transfer' event (like "done"); the
                # single failed STATUS comes from the host loop once every
                # peer has resolved, naming all failures.
                self.io.emit({"type": "transfer", "role": "send",
                              "peer": p["name"], "state": "failed",
                              "detail": "could not verify the file"})
                self.log(f"[host] {p['name']} FAILED save (receiver gave up)")
        # A non-final ok:false means the receiver is retrying -> keep serving.

    def on_peer_dropped(self, addr):
        p = self.peers.get(addr)
        if p and p["state"] == "active":
            p["state"] = "dropped"
            self.io.emit({"type": "transfer", "role": "send",
                          "peer": p["name"], "state": "dropped",
                          "detail": "dropped mid-transfer"})
            self.log(f"[host] {p['name']} dropped mid-transfer -- skipping")

    # -- the pump (one slice of work per peer) ----------------------------- #
    def pump(self, now):
        for addr, p in self.peers.items():
            if p["state"] != "active":
                continue
            verifying = p.get("verifying") and p["base"] >= self.total_chunks
            if now - p["last_advance"] > (PEER_XFER_VERIFY_TIMEOUT if verifying else PEER_XFER_TIMEOUT):
                p["state"] = "failed"
                self.io.emit({"type": "transfer", "role": "send",
                              "peer": p["name"], "state": "failed",
                              "detail": "timed out"})
                self.log(f"[host] {p['name']} save transfer TIMED OUT ({'its verify/unpack count stopped moving' if verifying else 'no forward progress'} "
                         f"for {now - p['last_advance']:.0f} s; last fack {now - p['last_fack']:.0f} s ago, base {p['base']}/{self.total_chunks})")
                continue
            if not p["ready"] or self.total_chunks == 0:
                if now - p["last_begin"] >= BEGIN_INTERVAL:
                    _send_data(self.sock, addr, self.begin_msg)
                    p["last_begin"] = now
                continue
            if p["base"] >= self.total_chunks and not p.get("verifying"):
                # Every chunk is delivered and the peer is past its verify, yet no
                # done has arrived: its announcement was lost. Re-send the last
                # chunk now and then (a chunk to a finished receiver is answered
                # with done) so recovery does not rest on the peer's timer alone.
                # This is the ONLY path for a TCP peer, which no chunk otherwise reaches.
                if now - p.get("last_nudge", 0.0) >= DONE_NUDGE_INTERVAL:
                    p["last_nudge"] = now
                    self._send_chunk(addr, self.total_chunks - 1)
                continue
            if p["tcp"]:
                continue                        # streaming over TCP: nothing to send here
            until = p.get("tcp_first_until")
            if until:
                if now < until and (p.get("push_pending") or p.get("pull_pending")):
                    p["last_advance"] = now     # waiting for TCP is not a stall
                    continue
                p.pop("tcp_first_until", None)
                self.log(f"[host] {p['name']}: no TCP stream -- Steam carries the {self.kind}")
            if self.chunk == CHUNK_STEAM:
                self._pump_steam(addr, p, now)
                continue
            # Receiver silent for too long? Its facks were lost -- rewind and
            # re-stream the window so it (and its facks) can catch up.
            resend_after = RESEND_AFTER
            if (now - p["last_fack"] > resend_after
                    and now - p["last_resend"] > resend_after):
                p["next"] = p["base"]
                p["last_resend"] = now
            budget = SEND_BUDGET
            # 1) selective retransmits (explicit holes) first
            while budget > 0 and p["nack"]:
                seq = p["nack"].pop(0)
                if seq < p["base"] or seq >= self.total_chunks:
                    continue
                self._send_chunk(addr, seq)
                budget -= 1
            # 2) new in-order chunks, capped by the flow-control window
            limit = min(self.total_chunks, p["base"] + self.window)
            while budget > 0 and p["next"] < limit:
                self._send_chunk(addr, p["next"])
                p["next"] += 1
                budget -= 1

    def _pump_steam(self, addr, p, now):
        """Steam already retransmits accepted reliable messages. ACKs bound new
        bytes in flight; a stalled contiguous cursor gets one recovery probe,
        or a bounded batch of holes before already received reliable data.
        This also recovers loss on either local UDP leg or a refused Steam send.
        NACKs include not-yet-delivered/not-yet-sent chunks, not proven loss.
        """
        p["nack"] = []
        started = p.setdefault("steam_started_at", now)
        window = p.setdefault("steam_window", STEAM_WINDOW_START)
        delay = p.setdefault("steam_retry_delay", RESEND_AFTER_STEAM)
        if (p["base"] < p["next"] and now - max(started, p["last_advance"]) >= delay
                and now - p.get("steam_retry_at", started) >= delay):
            missing = p.get("steam_missing", {p["base"]})
            end = min(p["next"], p["base"] + self.window, self.total_chunks)
            received = [seq for seq in range(p["base"], end) if seq not in missing]
            holes = sorted(seq for seq in missing if p["base"] <= seq < max(received, default=p["base"]))
            probes = holes[:8] if holes else [p["base"]]
            for seq in probes:
                self._send_chunk(addr, seq)
            p["steam_retry_at"] = now
            p["steam_retry_delay"] = min(STEAM_RETRY_MAX, delay * 2)
            if not holes:
                window = p["steam_window"] = max(4, window // 2)
            p["steam_retries"] = p.get("steam_retries", 0) + len(probes)
        limit = min(self.total_chunks, p["base"] + window)
        for _ in range(min(SEND_BUDGET, max(0, limit - p["next"]))):
            self._send_chunk(addr, p["next"])
            p["next"] += 1
        if now - p.get("steam_stats_at", 0.0) >= 5.0:
            previous = p.get("steam_stats_bytes", 0)
            done = min(p["base"] * self.chunk, self.total_bytes)
            elapsed = now - p.get("steam_stats_at", now)
            rate = max(0, done - previous) / elapsed if elapsed > 0 else 0
            self.log(f"[xfer] Steam send sid={self.sid} peer={p['name']}: acked={done}/{self.total_bytes}B "
                     f"rate={rate / 1e6:.3f}MB/s in_flight={max(0, p['next'] - p['base']) * self.chunk}B "
                     f"window={window} retries={p.get('steam_retries', 0)}")
            p["steam_stats_at"], p["steam_stats_bytes"] = now, done

    def all_resolved(self):
        return all(p["state"] in ("done", "failed", "dropped")
                   for p in self.peers.values())

    def failed_names(self):
        """Names of peers whose transfer FAILED (verify/timeout). 'dropped'
        peers have already left the lobby and never block a start."""
        return [p["name"] for p in self.peers.values()
                if p["state"] == "failed"]

    def on_mods_answer(self, addr, msg):
        """The joiner's player answered the download prompt (or its flags did)."""
        p = self.peers.get(addr)
        if not p or msg.get("sid") != self.sid:
            return
        p["ask"] = False
        if not msg.get("accept"):
            if p["need"]:
                self.log(f"[host] {p['name']} declined the mod download")
            p["need"] = []
            p["state"] = "failed"
        elif p["need"]:
            self.log(f"[host] {p['name']} accepted the mod download")

    def awaiting_answers(self, now):
        """True while a verified peer still has to say yes or no to the mods it
        lacks. No answer within MODS_ANSWER_WAIT counts as no: the start must
        not hang on a player who walked away from the prompt."""
        waiting = False
        for p in self.peers.values():
            if p["state"] != "done" or not p["need"] or not p["ask"]:
                continue
            if now - p["ask_since"] > MODS_ANSWER_WAIT:
                self.log(f"[host] {p['name']} did not answer the mod download prompt in {MODS_ANSWER_WAIT:.0f} s -- treating it as no")
                p["ask"] = False
                p["need"] = []
                p["state"] = "failed"
                continue
            if not p["ask_logged"]:
                p["ask_logged"] = True
                self.log(f"[host] waiting for {p['name']} to answer the mod download prompt (up to {MODS_ANSWER_WAIT:.0f} s)")
                self.io.emit({"type": "status", "state": "connected",
                              "detail": f"waiting for {p['name']} to accept the mods this save needs"})
            waiting = True
        return waiting

    def mod_needs(self):
        """{addr: [(id, ver)]} for the peers that verified and still lack mods."""
        return {a: p["need"] for a, p in self.peers.items() if p["state"] == "done" and p["need"]}

    def done_addrs(self):
        return [a for a, p in self.peers.items() if p["state"] == "done"]

    def done_count(self):
        return sum(1 for p in self.peers.values() if p["state"] == "done")


class _ClientSaveReceiver:
    """Receive + verify a save on a joiner; drives recovery via facks/NACKs.

    Holds the whole file set in memory (a bytearray sized from ``fbegin``),
    writes each received chunk at its exact offset (so reorder/dupes are free),
    tracks the contiguous ``base``, and periodically reports base + holes. On
    completion it verifies SHA-256 per file and overall, writes the files into
    the lobby IO directory as incoming_save.*, and emits ``save_ready``.
    """

    def __init__(self, conn, io, log, server_cache=None):
        self.conn = conn
        self.server_cache=server_cache
        self.io = io
        self.log = log
        self.sid = None
        self.buf = None
        self.have = None
        self.total_bytes = 0
        self.chunk = CHUNK_DATA
        self.window = SEND_WINDOW_REMOTE
        self.total_chunks = 0
        self.files = []
        self.overall_sha = None
        self.base = 0
        self.recv_count = 0
        self.recv_bytes = self.duplicate_chunks = 0
        self.recv_stats_at = time.time()
        self.recv_stats_bytes = 0
        self.complete = False
        self.failed = False
        self.retries = 0
        self.last_fack = 0.0
        self.last_pct = -1
        self.done_sends = 0
        self.last_done = 0.0
        self.kind = "save"
        self.need = []
        self.ask = False
        # THE JOINER DECIDES WHAT IT INSTALLS (2026-09-11). A mod is Lua the game
        # runs. The host used to be the only side that honoured a NO: a joiner
        # installed any kind=="mods" transfer it was sent, prompt or not, so a
        # modified host could skip the question and push code onto every joiner.
        # Now a mods round is taken only right after a verified save round, only
        # after THIS player said yes, and only the mods they were asked about
        # are installed; the yes covers one round.
        self.offered = []          # folder names the player was asked about (the last save round)
        self.approved = set()      # of those, what the player said yes to
        self.mods_satisfied = True
        self.last_mod_request = 0
        self.consent_id=0
        self.cancel_reason = "Required mods were cancelled or could not be installed."
        self.cancelled = False
        self.catalogue_token = None
        self.catalogue_since = 0
        self.required = []
        self.manifest = []
        self.manifest_key = None
        self.preflight = False
        self.save_done = False     # the save round before a mods round verified here
        self.finalizing = False    # a worker thread is verifying/writing the completed transfer
        self.finalize_progress = 0 # bytes that worker has hashed, written or unpacked so far (read by the loop)
        self._worker = None
        self._progress_lock = threading.Lock()   # _advance is called from the unpack threads too
        self._results = queue.Queue()
        self.deferred_begin = None # sid of an fbegin held back while finalizing (logged once)
        self.manifest_unknown = False
        self.batch = None            # [k, n] of the mods batch being received (a host from 0.6.1.7 on)
        self.batch_open = False      # a round is under way: more batches follow, do not ask again
        self.batch_done = False      # this batch is installed and its done is being announced (cleared by the next fbegin)
        self.batch_got = set()       # mods installed or present across the round's batches
        self.registering = []        # [(id, ver, folder)] on disk here, being registered with the game (catalogue_token pending)
        self.round_receipt = False   # the pending catalogue_token closes a download round (else it is a registration)
        self.steam_items = {}        # Workshop id -> (mod id, ver, folder name): subscribed through Steam, not installed yet
        self.steam_done = []         # [(id, ver, folder)] Steam installed; registered when the Steam phase ends
        self.steam_failed = []       # folder names Steam did not deliver: the host sends those
        self.steam_since = 0.0
        self.steam_poll_at = 0.0
        self.steam_progress = None
        self.steam_progress_at = 0.0
        # THE TCP CHANNEL (bulk_tcp.py). A joiner connects to the sender's listener
        # (the host's or relay's, named in fbegin) and a reader thread queues the
        # stream; the relay, which listens itself, accepts the leader's upload the
        # same way and tells the leader its port in fbegin_ack. tick() feeds the
        # queued bytes to on_chunk in chunk-sized pieces, so every receive-side
        # rule (holes, hashes, finalize, feedback) is the one the UDP path uses.
        self.my_name = ""          # said in the hello so the host matches the stream to us
        self.tcp_active = False
        self.tcp_bytes = 0
        self._tcp_q = queue.Queue()
        self._tcp_buf = bytearray()
        self._tcp_off = 0
        self._tcp_started = 0.0

    def _manifest_unknown(self):
        """The host could not READ its save's mod list. Not "no mods": the
        player is told, nothing is offered, and the save is still taken --
        the game itself decides whether it can load it here."""
        if self.manifest_unknown:
            return
        self.manifest_unknown = True
        self.log("[client] the host cannot read which mods its save needs -- no download will be offered")
        self.io.emit({"type": "chat", "from": "MULTIPLAYER",
                      "text": "The host could not read which mods its save needs, so none can be offered to you. "
                              "If your game refuses the save, install the host's mods by hand."})

    def on_manifest(self, entries, unknown=False):
        if entries is None or unknown:
            self._manifest_unknown()
            return
        if not isinstance(entries,list):
            self.log(f"[client] the host's mod list is not a list ({type(entries).__name__}) -- leaving the lobby")
            self.cancelled=True
            return
        mods=[]
        for entry in entries:              # as many entries as the save has (a 128 cap until 2026-09-16)
            if not isinstance(entry,(list,tuple)) or len(entry)!=2 or not modshare.valid_mod(entry[0],entry[1]):
                self.log(f"[client] mod list entry {entry!r} is not a (folder name, version) pair -- leaving the lobby")
                self.cancelled=True
                return
            mods.append(tuple(entry))
        self.manifest_unknown = False
        key=tuple(mods)
        if self.manifest_key==key:
            return
        self.manifest_key=key
        self.manifest=mods
        self.io.emit({"type":"mods_manifest", "mods":mods})
        # A fresh save supersedes earlier advertisements in on_begin.
        if self.active() or self.catalogue_token:
            return
        self.required=mods
        missing_dlc=[m for m,v in mods if modshare.is_dlc(m) and modshare.installed_mod(m,v) is None]
        if missing_dlc:
            self.cancelled=True
            self.cancel_reason="Required DLC is not installed: " + ", ".join(missing_dlc) + ". DLC is never transferred."
            self.io.emit({"type":"mods_cancelled","text":"Required DLC is not installed: " + ", ".join(missing_dlc) + ". DLC is never transferred."})
            return
        self.need, on_disk = self._split_missing(mods)
        self.offered=list(self.need)
        self.approved=set()
        self.preflight=True
        self.ask=bool(self.need)
        self.mods_satisfied=not self.need and not on_disk
        if on_disk and not self._register(on_disk):
            return
        if self.ask:
            self.consent_id+=1
            self.io.emit({"type":"mods_prompt", "offer":self.consent_id, "count":len(self.need), "text":", ".join(self.need)})
        else:
            self.io.emit({"type":"mods_clear"})

    def _split_missing(self, mods):
        """Of the mods a save needs, the folder names to DOWNLOAD (nowhere on
        this machine) and the [(id, ver, folder)] to REGISTER: on disk here, but
        not in the game's catalogue. Until 2026-09-20 both were "need": a joiner
        whose game had not catalogued a folder -- every mod of a round that never
        finished, since the registry was published only at a round's last batch
        -- had it zipped, sent and found "present" on arrival (21 folders, 2 GB,
        on 2026-09-20). The game loads what its catalogue lists, so a folder that
        is here is registered, never fetched."""
        need, on_disk = [], []
        for m, v in mods:
            if modshare.installed_mod(m, v) is not None:
                continue
            folder = modshare.on_disk_mod(m, v)
            if folder:
                on_disk.append((m, v, folder))
            else:
                need.append(modshare.mod_folder_name(m, v))
        self._publish_rows(mods)
        self._say_unreadable(mods)
        return need, on_disk

    def _say_unreadable(self, mods):
        """Which required mods this game could not read (its stdout.txt said 'Mod
        will be skipped' at startup): find_mod passed those folders over, so they
        are fetched from the host and registered in their place -- say so."""
        bad = [(modshare.mod_folder_name(m, v), modshare.skipped_copies.get(m)) for m, v in mods if m in modshare.skipped_copies]
        if not bad:
            return
        shown = ", ".join(f"{n} ({p})" for n, p in bad[:4]) + (f", +{len(bad) - 4} more" if len(bad) > 4 else "")
        self.log(f"[client] {len(bad)} required mod(s) have a copy here the game could not read (stdout.txt: 'Mod will be skipped') -- "
                 f"the host's copy is fetched and registered in its place: {shown}")
        self.io.emit({"type": "chat", "from": "MULTIPLAYER",
                      "text": f"Your game could not read {len(bad)} mod(s) this save needs (" + ", ".join(n for n, _ in bad[:4])
                              + (", ..." if len(bad) > 4 else "") + "); the host's copy will be fetched. "
                                "A Workshop item that stays broken here: unsubscribe, delete its folder and subscribe again."})

    def _publish_rows(self, mods):
        """A registry row for EVERY Workshop mod of the save whose folder is on
        this machine, catalogued or not (the token is kept; no refresh is asked
        for -- the world load's own refresh reads the registry, and the plugin
        registers each row in place of the game's own entry for that id).

        Why also the catalogued ones: the game's own entry for a Workshop id can
        lack a folder (an item Steam lists but has not installed), and the
        loader runs a listed mod's mod.lua at world load from whatever entry it
        holds. A row names the folder this lobby verified on disk, and the
        plugin registers it in place of the game's entry. Background: a joiner
        whose world load died running the Boeing 777 Pack's mod file, with Car
        Parks on the same stack, while the host loaded the same save
        (2026-09-20); the error text of that run is still to be seen."""
        # only this save's Workshop mods are registered from here on (modshare.set_registry_scope)
        modshare.set_registry_scope(m for m, v in mods if isinstance(m, str) and m.startswith("*"))
        rows = _workshop_rows(mods, modshare.on_disk_mod)
        if not rows:
            return
        try:
            modshare.write_registry(None, rows)
            self.log(f"[client] registry: {len(rows)} Workshop folder(s) of this save published for the game's next refresh")
        except (OSError, ValueError) as e:
            self.log(f"[client] could not publish the Workshop rows: {e}")

    def _register(self, on_disk):
        """Publish the registry naming these folders and ask the game to refresh
        its catalogue; the receipt (tick) must list every one. A registration
        already pending is folded in. False if the registry could not be
        written (the receiver has failed)."""
        rows = {(m, v): folder for m, v, folder in self.registering}
        rows.update({(m, v): folder for m, v, folder in on_disk})
        pending = [(m, v, folder) for (m, v), folder in rows.items()]
        extra = [(m[1:], folder) for m, v, folder in pending if m.startswith("*")]
        try:
            self.catalogue_token = modshare.request_catalogue(extra)
        except (OSError, ValueError) as e:
            self._fail("cannot publish mod registry: " + str(e))
            return False
        self.catalogue_since = time.time()
        self.registering = pending
        self.round_receipt = False
        names = [modshare.mod_folder_name(m, v) for m, v, _ in pending]
        shown = ", ".join(names[:6]) + (f", +{len(names) - 6} more" if len(names) > 6 else "")
        self.log(f"[client] {len(names)} mod(s) the save needs are on this PC but not in the game's catalogue -- "
                 f"registering them, not downloading: {shown}")
        self.io.emit({"type": "status", "state": "connected",
                      "detail": f"registering {len(names)} mod(s) already on this PC\u2026"})
        self.io.emit({"type": "mods_refresh"})
        return True

    def answer_mods(self, accept, offer=None):
        if offer is not None and offer != self.consent_id: return
        if not self.ask or self.cancelled:
            return
        self.ask=False
        self.approved=set(self.offered) if accept else set()
        if not accept:
            self.cancelled=True
            self._send({"t":"leave"})
            self.io.emit({"type":"mods_cancelled", "text":"Mod download cancelled; left the lobby."})
            return
        if self.preflight and self._steam_begin():
            return                                # tick asks the host for the rest once Steam is done
        if self.preflight:
            self.last_mod_request=time.time()
            self.first_mod_request=self.last_mod_request
            self._send({"t":"mods_request", "need":list(self.offered), "batches": 1})
        else:
            self._send({"t":"mods_answer", "sid":self.sid, "accept":True})
        shown = ", ".join(self.offered[:8]) + (f", +{len(self.offered)-8} more" if len(self.offered) > 8 else "")
        self.log(f"[client] mod download accepted -- asking the host for {len(self.offered)} mod(s): {shown}")

    def _steam_begin(self):
        """Subscribe through Steam to the Workshop mods the player just agreed to.
        True when Steam took the request (tick polls it); False to have the host
        send everything as before (no tunnel, an old bridge, no Workshop mods)."""
        if self.server_cache or "ignore_steam_workshop" in modshare.test_flags():
            return False                      # the relay; or the rig forcing the host's copies (test_flags)
        items = {}
        for name in self.offered:
            w = _workshop_item(name)
            if w:
                items[w[2]] = (w[0], w[1], name)
        if not items:
            return False
        t = _ugc_tunnel()
        if t is None or not t.ugc_subscribe(sorted(items)):
            self.log("[client] Steam's Workshop is not reachable from this lobby -- the host sends every mod")
            return False
        now = time.time()
        self.steam_items, self.steam_done, self.steam_failed = items, [], []
        self.steam_since = self.steam_progress_at = now
        self.steam_poll_at = 0.0
        self.steam_progress = None
        rest = len(self.offered) - len(items)
        self.log(f"[client] subscribing to {len(items)} Workshop mod(s) through Steam"
                 + (f"; the host sends the other {rest}" if rest else "") + ": " + ", ".join(sorted(items)[:8])
                 + (", ..." if len(items) > 8 else ""))
        self.io.emit({"type": "chat", "from": "MULTIPLAYER",
                      "text": f"Subscribing you to {len(items)} Workshop mod(s) this save needs; Steam downloads them."
                              + (f" The host sends the other {rest}." if rest else "")})
        self.io.emit({"type": "status", "state": "connected",
                      "detail": f"subscribing to {len(items)} Workshop mod(s) through Steam\u2026"})
        return True

    def _steam_poll(self, now):
        """One look at Steam's state for the items still coming. Installed ones
        are kept for registration; one Steam never subscribed, or a download
        that stopped moving, is left to the host."""
        t = _ugc_tunnel()
        states = t.ugc_state(sorted(self.steam_items)) if t is not None else None
        if states is None:
            self.log("[client] Steam's Workshop stopped answering -- the host sends the rest")
            self.steam_failed += [n for _, _, n in self.steam_items.values()]
            self.steam_items = {}
            self._steam_finish()
            return
        got = total = 0
        for wid, (m, v, name) in list(self.steam_items.items()):
            flags, done, size, folder = states.get(wid, (0, 0, 0, ""))
            busy = flags & (steamtunnel.UGC_DOWNLOADING | steamtunnel.UGC_DOWNLOAD_PENDING | steamtunnel.UGC_NEEDS_UPDATE)
            if flags & steamtunnel.UGC_INSTALLED and not busy and folder and os.path.isfile(os.path.join(folder, "mod.lua")):
                self.steam_done.append((m, v, modshare.on_disk_mod(m, v) or folder))
                del self.steam_items[wid]
            elif not flags & steamtunnel.UGC_SUBSCRIBED and now - self.steam_since > UGC_SUBSCRIBE_WAIT:
                self.log(f"[client] Steam did not subscribe to {name} in {UGC_SUBSCRIBE_WAIT:.0f} s "
                         "(hidden, removed, or Steam offline) -- the host sends its copy")
                self.steam_failed.append(name)
                del self.steam_items[wid]
            else:
                got += done
                total += max(size, done)
        mark = (len(self.steam_done), got)
        if mark != self.steam_progress:
            self.steam_progress, self.steam_progress_at = mark, now
        elif self.steam_items and now - self.steam_progress_at > UGC_STALL:
            names = [n for _, _, n in self.steam_items.values()]
            self.log(f"[client] Steam's download made no progress in {UGC_STALL:.0f} s -- the host sends the "
                     f"{len(names)} left: " + ", ".join(names[:8]))
            self.steam_failed += names
            self.steam_items = {}
        if not self.steam_items:
            self._steam_finish()
            return
        n = len(self.steam_done) + len(self.steam_items)
        size = f", {got / 1e6:.0f} of {total / 1e6:.0f} MB" if total else ""
        self.io.emit({"type": "status", "state": "connected",
                      "detail": f"Steam Workshop: {len(self.steam_done)} of {n} mod(s) installed{size}\u2026"})

    def _steam_finish(self):
        """The Steam phase is over: register what Steam installed and leave the
        rest (local mods, what Steam did not deliver) to the host's round."""
        done, failed = self.steam_done, self.steam_failed
        self.steam_done, self.steam_failed = [], []
        got = {modshare.mod_folder_name(m, v) for m, v, _ in done}
        self.offered = [n for n in self.offered if n not in got]
        self.need = [n for n in self.need if n not in got]
        if not self.offered:
            self.approved = set()               # nothing is left for the host to send
        self.log(f"[client] Steam installed {len(done)} Workshop mod(s)" + (f"; {len(failed)} fall back to the host" if failed else "")
                 + (f"; asking the host for {len(self.offered)}" if self.offered else ""))
        if done:
            self.io.emit({"type": "chat", "from": "MULTIPLAYER",
                          "text": f"Steam installed {len(done)} Workshop mod(s); you stay subscribed to them."
                                  + (f" {len(failed)} could not come from the Workshop, so the host sends its copy." if failed else "")})
            self._register(done)
        elif not self.offered:
            self.mods_satisfied = True
            self.io.emit({"type": "mods_ready", "failed": []})
        self.last_mod_request = 0
        self.first_mod_request = time.time()

    def _refusal(self, kind, files):
        """Why this proposed transfer must not be taken, or None."""
        names = [m.get("name") if isinstance(m, dict) else None for m in files]
        if kind == "save":
            bad = [n for n in names if n not in ALLOWED_INCOMING or not _safe_incoming_name(n)]
            return f"refused: sender proposed unexpected filename(s) {bad}" if bad else None
        if not self.save_done and not self.preflight:
            return "refused a mods transfer that did not follow a verified save transfer"
        if not self.approved:
            return "refused a mods transfer this player did not agree to"
        bad = [n for n in names if not _safe_incoming_name(n) or modshare.parse_mod_zip_name(n) is None]
        if bad:
            return f"refused a mods transfer carrying files that are not mod zips: {bad}"
        if len(set(names)) != len(names):
            return "refused a mods transfer that names the same file twice"
        return None

    def _refuse_mods(self, detail):
        """A mods round the player did not agree to: nothing is received or
        installed, the host hears a final failure (it starts us anyway), and the
        player is told. Not a status line: the save itself arrived fine."""
        self.failed = True
        self.cancelled = True
        self.approved = set()
        self.log(f"[client] {detail}")
        self.io.emit({"type": "chat", "from": "MULTIPLAYER",
                      "text": "Blocked mod files from the host that you did not agree to download; nothing was installed."})
        self._send({"t": "fdone", "sid": self.sid, "ok": False, "final": True})

    def active(self):
        """True while a transfer is in progress (steer the loop to poll fast)."""
        return self.sid is not None and not self.complete and not self.failed

    def _send(self, msg):
        payload = json.dumps(msg).encode("utf-8")
        try:
            for piece in _fragments(payload):
                self.conn.send(piece)
        except (RuntimeError, OSError) as e:
            _log_send_failure(getattr(self.conn, "peer", None), len(payload), e, self.log)

    def _fail(self, detail):
        """Give up on this session: tell the menu AND the host (fdone ok:false
        final) so the host resolves us as failed NOW rather than after
        PEER_XFER_TIMEOUT."""
        self.failed = True
        if self.kind=="mods":
            self.cancelled=True
            self.cancel_reason=detail
        self.log(f"[client] giving up this transfer: {detail}")
        self.io.emit({"type": "status", "state": "failed",
                      "detail": f"save transfer failed: {detail}"})
        self._send({"t": "fdone", "sid": self.sid, "ok": False, "final": True})

    # -- inbound ----------------------------------------------------------- #
    def on_begin(self, msg):
        if self.cancelled:
            return
        sid = msg.get("sid")
        if sid == self.sid:
            if self.failed:
                # The host missed our final fdone and is still re-sending
                # fbegin: repeat the verdict (never re-ack, or it would start
                # streaming at a receiver that can't take the file).
                self._send({"t": "fdone", "sid": sid, "ok": False,
                            "final": True})
            else:
                self._send({"t": "fbegin_ack", "sid": sid, "need": self.need, "ask": self.ask})  # duplicate -> re-ack
            return
        if self.finalizing:
            # The previous round is still being verified/written by the worker.
            # Not acked: the sender repeats fbegin every BEGIN_INTERVAL until it
            # is, and this one is taken as soon as the worker is done.
            if self.deferred_begin != sid:
                self.deferred_begin = sid
                self.log(f"[client] fbegin sid={sid} while still writing sid={self.sid} -- answered once that is done")
            return
        # A brand-new session (first ever, or a later transfer): (re)allocate.
        kind = "mods" if msg.get("kind") == "mods" else "save"
        files = msg.get("files", [])
        if not isinstance(files, list):
            files = []
        # Validate BEFORE allocating, acking or asking the player anything: a
        # rejected transfer must cost the joiner nothing.
        refusal = self._refusal(kind, files)
        if refusal:
            self.sid, self.kind, self.files = sid, kind, []
            self.buf = self.have = None
            if kind == "mods":
                self._refuse_mods(refusal)
            else:
                self._fail(refusal)
            return
        self.sid = sid
        self.total_bytes = int(msg.get("total_bytes", 0))
        self.chunk = int(msg.get("chunk", CHUNK_DATA)) or CHUNK_DATA
        # Same rule as the host, derived from the chunk it actually chose,
        # so the two ends agree how far ahead the NACK scan should look.
        self.window = _window_for(self.chunk)
        self.total_chunks = int(msg.get("total_chunks", 0))
        self.files = files
        self.kind = kind
        b = msg.get("batch")
        self.batch = [int(b[0]), int(b[1])] if kind == "mods" and isinstance(b, list) and len(b) == 2 else None
        if kind == "mods" and (self.batch is None or self.batch[0] == 1):
            self.batch_got = set()
        # the mods this save needs that are not installed here (told back in the ack)
        self.need = []
        previous_approval=set(self.approved) if self.preflight else set()
        if kind == "save" and self.steam_items:
            # the host shared its save before Steam finished: what Steam has installed
            # is on disk and registers below; the rest the host's round sends
            self.log(f"[client] the save arrived while Steam still had {len(self.steam_items)} Workshop mod(s) to go -- "
                     "the host sends those")
            self.steam_items, self.steam_done, self.steam_failed = {}, [], []
        if kind == "save":
            self.preflight=False
            self.required=[]
            self.save_done = False
            on_disk = []
            for ent in (msg.get("mods") or []):
                try:
                    m, v = str(ent[0]), int(ent[1])
                except (TypeError, ValueError, IndexError):
                    continue
                if not modshare.valid_mod(m,v):
                    self.cancelled=True
                    return
                self.required.append((m,v))
                if self.server_cache:
                    present=modshare.is_dlc(m) or os.path.isfile(os.path.join(self.server_cache,modshare.cache_name(m,v)))
                else:
                    present=modshare.installed_mod(m,v) is not None
                    if not present and modshare.is_dlc(m):
                        self.cancelled=True
                        self.cancel_reason="Required DLC is missing. Deluxe and Early Supporter content cannot be downloaded from the host."
                        self.io.emit({"type":"mods_cancelled","text":"Required DLC is missing. Deluxe and Early Supporter content cannot be downloaded from the host."})
                        return
                    if not present:
                        folder = modshare.on_disk_mod(m, v)
                        if folder:                       # here, not catalogued: register (see _split_missing)
                            on_disk.append((m, v, folder))
                            present = True
                if not present:
                    self.need.append(modshare.mod_folder_name(m, v))
            self._publish_rows(self.required)
            if on_disk and not (self.catalogue_token and self.round_receipt) and not self._register(on_disk):
                return
            if msg.get("mods_unknown"):
                self._manifest_unknown()             # the list is unknown, not empty: say so, take the save
            # a new save round asks afresh: an earlier yes does not carry over
            self.offered = list(self.need)
            self.approved = previous_approval.intersection(self.offered)
        self.ask = bool(self.need) and kind == "save" and not set(self.need).issubset(self.approved)
        if kind=="save": self.mods_satisfied=not self.need
        if self.server_cache and kind=="save":
            self.approved=set(self.offered)
            self.ask=False
        if self.ask:
            self.log(f"[client] the save needs mods we lack: {', '.join(self.need)} -- asking the player")
            # the panel shows YES / NO (or answers from its share_mods flag)
            self.consent_id+=1
            self.io.emit({"type": "mods_prompt", "offer":self.consent_id, "count": len(self.need), "mods": list(self.need),
                          "text": ", ".join(self.need)})
            self.io.emit({"type": "chat", "from": "MULTIPLAYER",
                          "text": "Mods are code that runs in your game, and these come from the host's "
                                  "computer: only download them from a host you trust."})
        self.overall_sha = msg.get("sha256")
        self.buf = None
        self.have = None
        self.complete = False
        try:
            self.buf = bytearray(self.total_bytes)
            # inside the SAME guard: a host-declared huge total_chunks used to
            # crash the joiner here instead of failing cleanly through _fail
            self.have = bytearray(self.total_chunks)
        except (MemoryError, OverflowError):
            self._fail(f"cannot allocate {self.total_bytes} bytes "
                       f"/ {self.total_chunks} chunks")
            return
        self.base = 0
        self.recv_count = 0
        self.recv_bytes = self.duplicate_chunks = 0
        self.recv_stats_at = time.time()
        self.recv_stats_bytes = 0
        self.complete = False
        self.failed = False
        self.retries = 0
        self.last_pct = -1
        self.done_sends = 0
        self.batch_done = False
        self.log(f"[client] save incoming sid={sid} {self.total_bytes}B "
                 f"{self.total_chunks} chunks")
        ack = {"t": "fbegin_ack", "sid": sid, "need": self.need, "ask": self.ask}
        self._tcp_buf, self._tcp_off, self.tcp_bytes = bytearray(), 0, 0
        while not self._tcp_q.empty():
            self._tcp_q.get_nowait()
        tcp = msg.get("tcp") if BULK_TCP[0] and self.total_bytes > 0 else None
        if isinstance(tcp, dict) and isinstance(tcp.get("token"), str) and tcp["token"]:
            if isinstance(self.conn, _PeerConn):
                # we are the relay taking the leader's upload: WE listen, it connects
                if BULK[0] is not None:
                    BULK[0].expect(sid, "send", tcp["token"], self._tcp_accepted)
                    ack["tcp_port"] = BULK[0].port
            elif isinstance(tcp.get("port"), int) and getattr(self.conn, "peer", None) \
                    and not steamtunnel.is_tunnel_addr(self.conn.peer):
                threading.Thread(target=self._tcp_pull, args=(self.conn.peer[0], tcp["port"], tcp["token"], sid),
                                 name="bulk-pull", daemon=True).start()
            elif getattr(self.conn, "peer", None) and steamtunnel.is_tunnel_addr(self.conn.peer):
                # THROUGH STEAM: dial the addresses the host named, and name ours so the
                # host can dial us (see MY_TCP_ADDRS); Steam carries whatever neither reaches
                host_ips = _valid_tcp_addrs(tcp.get("addrs"))
                if host_ips and isinstance(tcp.get("port"), int):
                    ack["tcp_pull"] = True             # the host holds Steam while we dial
                    threading.Thread(target=self._tcp_pull, args=(host_ips, tcp["port"], tcp["token"], sid, True),
                                     name="bulk-pull", daemon=True).start()
                if JOINER_UPNP.get("started"):
                    JOINER_UPNP["done"].wait(3.0)     # the router mapping (and our WAN IP) is on its way
                if MY_TCP_ADDRS[0]:
                    try:
                        _joiner_bulk_listener(self.conn.sock.getsockname()[1], self.log)
                    except AttributeError:
                        pass
                    if JOINER_BULK[0] is not None:
                        JOINER_BULK[0].expect(sid, "send", tcp["token"], self._tcp_accepted)
                        ack["tcp_port"] = JOINER_BULK[0].port
                        ack["tcp_addrs"] = list(MY_TCP_ADDRS[0])
                self.log(f"[client] the host is reached through Steam: TCP to {len(host_ips)} host address(es)"
                         + (f", and our listener on tcp/{ack['tcp_port']} offered" if "tcp_port" in ack else ", none offered here"))
        self._send(ack)
        if kind != "mods":
            self.io.emit({"type": "transfer", "role": "recv", "pct": 0})   # a mods batch: the round's status line stays
        if self.total_chunks == 0:
            self._finalize()

    # -- the TCP channel --------------------------------------------------- #
    def _tcp_pull(self, ip, port, token, sid, tell_host=False):
        """A thread: connect to the sender's listener and read the file. A
        connect that fails is tried again (TCP_CONNECT_TRIES): the UDP fallback
        is 20x slower than the stream, and nothing has been read yet, so a fresh
        stream from the start is consistent with what UDP delivers meanwhile
        (chunks already in hand are dropped as duplicates)."""
        ips = ip if isinstance(ip, list) else [ip]
        for attempt in range(1, TCP_CONNECT_TRIES + 1):
            errs = []
            for one in ips:
                if sid != self.sid or getattr(self, "_tcp_claim", None) == sid:
                    return                   # a later transfer replaced this one, or the host's dial won
                sock = bulk_tcp.bulk_connect(one, port, "recv", sid, token, self.my_name, errors=errs)
                if sock is not None:
                    self._tcp_read(sock, sid)
                    return
            if attempt < TCP_CONNECT_TRIES:
                self.log(f"[client] no TCP stream from {', '.join(redact(i) for i in ips)} port {port} (attempt {attempt}) -- trying again"
                         + (f" [{redact('; '.join(errs))}]" if errs else ""))
                time.sleep(TCP_CONNECT_RETRY)
        self.log(f"[client] no TCP stream from {', '.join(redact(i) for i in ips)} port {port} after {TCP_CONNECT_TRIES} attempts -- "
                 + ("Steam carries it" if len(ips) > 1 or steamtunnel.is_tunnel_addr(getattr(self.conn, 'peer', None)) else "receiving over UDP")
                 + (f" [{redact('; '.join(errs))}]" if errs else ""))
        if tell_host and sid == self.sid:
            self._send({"t": "tcp_gave_up", "sid": sid})   # the host stops waiting for our dial

    def _tcp_accepted(self, sock, addr, name):
        """ACCEPT THREAD HELPER (the relay): the leader connected to push its upload."""
        self._tcp_read(sock, self.sid)

    def _tcp_read(self, sock, sid):
        with self._progress_lock:              # both ends may dial: the first stream wins
            if getattr(self, "_tcp_claim", None) == sid:
                try:
                    sock.close()
                except OSError:
                    pass
                return
            self._tcp_claim = sid
        self.tcp_active, self._tcp_started = True, time.time()
        self.log(f"[client] taking the {self.kind} over TCP")
        self.io.emit({"type": "transfer", "role": "recv", "state": "tcp"})
        q = self._tcp_q

        def sink(data):
            while q.qsize() > 16:            # ~64 MB queued: let the loop thread catch up
                time.sleep(0.005)
            q.put((sid, bytes(data)))
        ok = bulk_tcp.stream_recv(sock, self.total_bytes, sink)
        q.put((sid, None if ok else b""))
        if not ok:
            self.log("[client] the TCP stream broke -- the rest comes over UDP")

    def _drain_tcp(self):
        """LOOP THREAD: feed queued stream bytes to on_chunk, chunk by chunk.
        Bounded per call so the feedback timer keeps running during a fast stream."""
        fed = 0
        while fed < 8 * bulk_tcp.RECV_BLOCK:
            try:
                sid, data = self._tcp_q.get_nowait()
            except queue.Empty:
                break
            if sid != self.sid:
                continue
            if data is None or data == b"":
                self.tcp_active = False
                if data is None:
                    self.tcp_bytes = self._tcp_off
                    self.log(f"[client] received over TCP, {bulk_tcp.rate_text(self._tcp_off, time.time() - self._tcp_started)}")
                break
            fed += len(data)
            self._tcp_buf += data
            while self.sid == sid and not self.failed and not self.complete:
                remaining = self.total_bytes - self._tcp_off
                take = min(self.chunk, remaining)
                if take <= 0 or len(self._tcp_buf) < take:
                    break
                piece = bytes(self._tcp_buf[:take])
                del self._tcp_buf[:take]
                self.on_chunk(sid, self._tcp_off // self.chunk, piece)
                self._tcp_off += take

    def on_chunk(self, sid, seq, data):
        if self.sid is None or sid != self.sid:
            return
        if self.complete or self.batch_done:
            self._maybe_send_done(force=True)   # nudge host to stop resending
            return
        if self.finalizing:
            return                               # all in hand; the worker is on it
        if self.failed or seq < 0 or seq >= self.total_chunks:
            return
        if self.have[seq]:
            self.duplicate_chunks += 1
            return                               # dup / reorder -- already have it
        off = seq * self.chunk
        # bytearray slice-assignment GROWS the buffer when the slice runs past
        # the end, so an oversized host-controlled chunk silently changed
        # total_bytes out from under the hash check. Refuse instead.
        if off < 0 or off + len(data) > self.total_bytes:
            self._fail(f"refused: chunk {seq} would write "
                       f"{off}..{off + len(data)} past {self.total_bytes}")
            return
        self.buf[off:off + len(data)] = data
        self.have[seq] = 1
        self.recv_count += 1
        self.recv_bytes += len(data)
        while self.base < self.total_chunks and self.have[self.base]:
            self.base += 1
        self._emit_pct()
        if self.base >= self.total_chunks:
            self._finalize()

    def _emit_pct(self):
        if self.total_chunks == 0:
            pct = 100
        else:
            pct = int(self.recv_count * 100 // self.total_chunks)
        if pct // 10 > self.last_pct // 10:
            self.last_pct = pct
            if self.kind != "mods":
                self.io.emit({"type": "transfer", "role": "recv", "pct": pct})
            # the roster's stage column is 120 px wide: "mods 32/140 90%", not a sentence
            if self.kind == "mods":
                what = f"mods {self.batch[0]}/{self.batch[1]}" if self.batch else "mods"
                self._send({"t": "stage", "text": f"{what} {pct}%"})
            else:
                self._send({"t": "stage", "text": f"receiving save {pct}%"})

    # -- periodic (called from the client loop) ---------------------------- #
    def tick(self, now):
        self._poll_worker()                      # a finished verify/write lands here, on the loop thread
        if not self._tcp_q.empty():
            self._drain_tcp()
        if self.cancelled:
            return
        if self.active() and self.chunk == CHUNK_STEAM and now - self.recv_stats_at >= 5.0:
            rate = (self.recv_bytes - self.recv_stats_bytes) / (now - self.recv_stats_at)
            self.log(f"[xfer] Steam receive sid={self.sid}: unique={self.recv_bytes}/{self.total_bytes}B "
                     f"rate={rate / 1e6:.3f}MB/s base={self.base}/{self.total_chunks} "
                     f"duplicates={self.duplicate_chunks}")
            self.recv_stats_at, self.recv_stats_bytes = now, self.recv_bytes
        # frames the replay window refused as too old: silent until 2026-09-22, when
        # they were every Steam-delayed save chunk (seal.py Sealer.sign)
        sealer = SEAL[0]
        if sealer is not None and self.active() and now - getattr(self, "_old_at", 0.0) >= 5:
            self._old_at = now
            n, seen = getattr(sealer, "too_old", 0), getattr(self, "_old_seen", 0)
            if n != seen:
                self._old_seen = n
                self.log(f"[client] {n - seen} frame(s) refused as too old by the replay window during the transfer (total {n})")
        if self.steam_items and now - self.steam_poll_at >= UGC_POLL_EVERY:
            self.steam_poll_at = now
            self._steam_poll(now)
        if self.preflight and self.approved and not self.steam_items and not self.active() and not self.catalogue_token and not self.batch_open and now-self.last_mod_request>1:
            self.last_mod_request=now
            self._send({"t":"mods_request","need":list(self.offered), "batches": 1})
            first = getattr(self, "first_mod_request", 0) or now
            if now - first > 20 and not getattr(self, "silence_logged", False):
                self.silence_logged = True
                self.log(f"[client] the host has not answered the mod request in {now - first:.0f} s -- still asking every second "
                         f"(its log says whether it received it and what it is doing)")
                self.io.emit({"type": "status", "state": "connected",
                              "detail": "waiting for the host to send the mods (no answer yet)\u2026"})
        if self.catalogue_token:
            token, entries = modshare.catalogue()
            if token == self.catalogue_token:
                check = self.required if self.round_receipt else [(m, v) for m, v, _ in self.registering]
                missing=[modshare.mod_folder_name(m,v) for m,v in check if (m,str(v)) not in entries]
                self.catalogue_token=None
                self.registering=[]
                if missing:
                    self._fail(("mods not recognised by game: " if self.round_receipt else
                                "mods on this PC the game does not recognise (delete the folder to download the host's copy): ")
                               + ", ".join(missing))
                    self.cancelled=True
                    return
                if self.round_receipt:
                    self.complete=True
                    self.mods_satisfied=True
                    self.io.emit(dict({"type":"mods_ready", "failed":[]}, **getattr(self,"install_result",{})))
                    self._maybe_send_done(force=True)
                else:
                    # a registration: the on-disk folders are catalogued; what is
                    # not here at all is still to be downloaded (need, prompt, round)
                    self.mods_satisfied = not self.need
                    self.log(f"[client] the game registered the mods already on this PC"
                             + ("" if self.mods_satisfied else f"; {len(self.need)} still to download"))
                    if self.mods_satisfied:
                        self.io.emit({"type": "mods_ready", "failed": [], "registered": len(check)})
            elif now-self.catalogue_since>45:
                self._fail("game did not refresh its mod catalogue")
                self.cancelled=True
                self.catalogue_token=None
            if self.round_receipt or self.cancelled or self.sid is None:
                return                    # a registration beside a live transfer keeps feeding it
        if self.sid is None or self.failed:
            return
        if self.complete or self.batch_done:
            # A batch that is not the last of its round leaves `complete` false
            # (the round is still open), and until 2026-09-20 that meant its done
            # went out exactly ONCE: one lost datagram and the host, with every
            # chunk already delivered and nothing to retransmit, timed the peer
            # out 30 s later and failed the round (batch 18/359, 2026-09-20 01:14).
            self._maybe_send_done(now=now)
            return
        if now - self.last_fack >= FEEDBACK_INTERVAL:
            self.last_fack = now
            self._send_fack()

    def _send_fack(self):
        nack = []
        limit = min(self.total_chunks, self.base + self.window)
        s = self.base
        while s < limit and len(nack) < MAX_NACK:
            if not self.have[s]:
                nack.append(s)
            s += 1
        msg = {"t": "fack", "sid": self.sid, "base": self.base, "nack": nack, "verifying": self.finalizing}
        if self.finalizing:
            msg["progress"] = self.finalize_progress    # the host times out a count that stops moving
        self._send(msg)

    def _maybe_send_done(self, now=None, force=False):
        # After completion the host may not have heard our fdone (it can be
        # lost), so we keep re-announcing it on a timer AND force a reply to any
        # chunk the host retransmits -- either way the host learns we're done.
        now = now or time.time()
        # every 0.2 s for the first 8 s, then every second for as long as the host
        # keeps this transfer open (it stops asking once it moves on)
        if force or now - self.last_done >= (0.2 if self.done_sends < 40 else 1.0):
            self.last_done = now
            self.done_sends += 1
            self._send({"t": "fdone", "sid": self.sid, "ok": True})

    # -- assemble + verify + write ----------------------------------------- #
    def _finalize(self):
        """Every chunk is in hand: verify and write it OFF the lobby loop.

        The hashing and the writes used to run inline here, and for that long
        the joiner sent nothing -- no ping to the host, no fack -- so a 1 GB
        save on an HDD (two SHA-256 passes, the write, then a mod unzip) went
        past the host's DROP_AFTER and the joiner was evicted mid-transfer
        (2026-09-16). Now a worker thread does the work while the loop keeps
        pinging and keeps sending facks (base = total, "verifying": true),
        which the host counts as progress. The outcome comes back through a
        queue and is applied by _poll_worker on the loop thread, so every
        state change, emit and reply still happens where it always did.

        ZERO-COPY, as before: bytearray and memoryview both support the
        buffer protocol, so hashlib and file.write take them directly.
        """
        if self.finalizing:
            return
        self.finalizing = True
        self.finalize_progress = 0
        approved = None
        if self.kind == "mods":
            approved = set(self.approved)
            if not (self.batch and self.batch[0] < self.batch[1]):
                self.approved = set()               # the yes is used up by this round (its LAST batch)
        job = {"sid": self.sid, "buf": self.buf, "files": list(self.files),
               "overall_sha": self.overall_sha, "kind": self.kind,
               "approved": approved, "total_bytes": self.total_bytes}
        self._worker = threading.Thread(target=self._finalize_work, args=(job,),
                                        name=f"{threading.current_thread().name}/save-finalize", daemon=True)
        self._worker.start()

    def _advance(self, count):
        """WORKER THREAD: another ``count`` bytes hashed, written or unpacked.
        The loop reads the total into every fack while verifying; the unpack
        threads of a batch all add to it, hence the lock."""
        with self._progress_lock:
            self.finalize_progress += count

    def _sha256(self, data):
        """A SHA-256 hex digest computed in FINALIZE_SLICE windows so the
        count moves while a 1 GB buffer is hashed."""
        h = hashlib.sha256()
        for i in range(0, len(data), FINALIZE_SLICE):
            piece = data[i:i + FINALIZE_SLICE]
            h.update(piece)
            self._advance(len(piece))
        return h.hexdigest()

    def _finalize_work(self, job):
        """WORKER THREAD: hash, then write the save or unpack the mods. Touches
        no receiver state but finalize_progress; everything it learns goes
        into the result."""
        res = {"sid": job["sid"], "ok": True, "seconds": 0.0}
        t0 = time.time()
        try:
            view = memoryview(job["buf"])
            off = 0
            parts = {}
            for meta in job["files"]:
                size = int(meta.get("size", 0))
                part = view[off:off + size]                  # a window, not a copy
                off += size
                if self._sha256(part) != meta.get("sha256"):
                    res["ok"] = False
                    break
                parts[meta.get("name")] = part
            if res["ok"] and job["overall_sha"]:
                if self._sha256(view) != job["overall_sha"]:
                    res["ok"] = False
            res["seconds"] = time.time() - t0
            if res["ok"]:
                if job["kind"] == "mods":
                    res["install"] = self._install_files(parts, job["approved"])
                else:
                    written = []
                    try:
                        for meta in job["files"]:
                            name = meta.get("name")
                            if not _safe_incoming_name(name):
                                res["error"] = f"refused: unexpected filename {name!r}"
                                break
                            part = parts[name]
                            with open(os.path.join(self.io.dir, name), "wb") as f:
                                for i in range(0, len(part), FINALIZE_SLICE):
                                    f.write(part[i:i + FINALIZE_SLICE])   # memoryview: no copy
                                    self._advance(min(FINALIZE_SLICE, len(part) - i))
                            written.append(name)
                    except OSError as e:
                        res["error"] = f"write error: {e}"
                    res["written"] = written
            # Release the memoryviews before the bytearray they borrow from can
            # be dropped; a lingering export would keep the whole save alive.
            for v in parts.values():
                v.release()
            view.release()
        except Exception as e:                               # noqa: BLE001 -- the loop must hear it
            res["error"] = f"verify/write crashed: {e!r}"
        self._results.put(res)

    def _poll_worker(self):
        """LOOP THREAD: apply a finished verify/write, if there is one."""
        try:
            res = self._results.get_nowait()
        except queue.Empty:
            return
        self._worker = None
        self.finalizing = False
        if self.cancelled or res.get("sid") != self.sid:
            return                                           # a round abandoned meanwhile
        self._finish(res)

    def settle(self, timeout=120.0):
        """Block until a running verify/write has finished and its outcome is
        applied. For tests and shutdown; the lobby loop never waits."""
        w = self._worker
        if w is not None:
            w.join(timeout)
        self._poll_worker()

    def _finish(self, res):
        self.log(f"[client] save verify: {self.total_bytes}B in "
                 f"{res['seconds']:.1f}s ({'ok' if res['ok'] else 'MISMATCH'})")
        if res.get("error"):
            self._fail(res["error"])
            return
        if not res["ok"]:
            self.retries += 1
            if self.retries <= MAX_FILE_RETRIES:
                self.log(f"[client] save hash mismatch -- re-request "
                         f"(attempt {self.retries})")
                self.have = bytearray(self.total_chunks)   # request everything
                self.base = 0
                self.recv_count = 0
                self.recv_bytes = self.duplicate_chunks = 0
                self.recv_stats_at = time.time()
                self.recv_stats_bytes = 0
                self.last_pct = -1
                self._send_fack()
                return
            self._fail("hash mismatch")
            return
        if self.kind == "mods":
            self._mods_installed(res["install"])
            if self.failed:
                self.cancelled=True
                return
            if self.server_cache:
                self.complete=True
                self.mods_satisfied=True
                self._maybe_send_done(force=True)
                return
            if self.batch and self.batch[0] < self.batch[1]:
                # one batch of a round: hand it back and wait for the next
                self.batch_open = True
                self.complete = False
                self.batch_done = True
                try:
                    # publish what this batch installed NOW (keeping any pending token):
                    # a round that stops here still registers its mods at the next game start
                    modshare.write_registry()
                except (OSError, ValueError) as e:
                    self.log(f"[client] could not publish the mod registry after batch {self.batch[0]}: {e}")
                self.log(f"[client] mod batch {self.batch[0]}/{self.batch[1]} installed -- waiting for the next")
                self._maybe_send_done(force=True)
                return
            self.batch_open = False
            self.complete = False
            try:
                self.catalogue_token=modshare.request_catalogue()
            except (OSError,ValueError) as e:
                self._fail("cannot publish mod registry: " + str(e))
                return
            self.round_receipt = True
            self.registering = []
            self.catalogue_since=time.time()
            self.io.emit({"type":"mods_refresh"})
            return
        written = res.get("written", [])
        self.complete = True
        self.complete_at = time.time()
        self.save_done = True                    # a mods round may follow (still needs the player's yes)
        self.io.emit({"type": "transfer", "role": "recv", "pct": 100})
        self.io.emit({"type": "save_ready", "name": INCOMING_BASENAME,
                      "dir": os.path.abspath(self.io.dir), "files": written})
        self.log(f"[client] save ready: {written} in {self.io.dir}")
        if _save_has_mp_mod(os.path.join(self.io.dir, INCOMING_BASENAME + ".sav")) is False:
            self.log(f"[client] the received save was made without the {MOD_DISPLAY_NAME} mod")
            self.io.emit({"type": "chat", "from": "MULTIPLAYER",
                          "text": f"The host's save does not have the {MOD_DISPLAY_NAME} mod enabled, so nothing will sync. "
                                  "Ask the host to enable it in that save's Mods panel and share it again."})
        self._maybe_send_done(force=True)


    def _install_files(self, parts, approved):
        """WORKER THREAD half of a mods round: unpack each
        incoming_mod_<id>_<ver>.zip into the game's mods folder (never over an
        existing one). Only the mods this player said yes to are installed; the
        host sends one zip set to everyone who lacked something, so the rest
        are left alone. Returns what happened; _mods_installed tells the player."""
        done, kept, bad, skipped = [], [], [], []
        jobs = []
        for name, part in parts.items():
            idv = modshare.parse_mod_zip_name(name)
            if not idv:
                bad.append(str(name)); continue
            label = modshare.mod_folder_name(*idv)
            if label not in approved:
                if modshare.installed_mod(*idv) is None:
                    skipped.append(label)
                self.log(f"[client] mod {label}: not agreed to here -- not installed")
                continue
            if self.server_cache and modshare.is_dlc(idv[0]):
                bad.append(label); continue
            jobs.append((idv, label, part))
        results = []
        if self.server_cache:
            for idv, label, part in jobs:
                try:
                    os.makedirs(self.server_cache,exist_ok=True)
                    path=os.path.join(self.server_cache,modshare.cache_name(*idv))
                    with open(path+".tmp","wb") as f: f.write(part)
                    os.replace(path+".tmp",path)
                    results.append((label, "installed", path))
                except OSError as e:
                    self.log(f"[relay] cannot cache {label}: {e}")
                    results.append((label, "failed", None))
        else:
            # every file of every zip in the batch over one pool: a batch of many
            # small mods and one mod of 1,300 files both keep every thread busy
            # (one thread per zip left a 1,330-file mod alone for 8 s, 2026-09-20)
            outcome = modshare.install_mod_zips([(bytes(part), idv[0], idv[1]) for idv, _, part in jobs],
                                                self.log, progress=self._advance, threads=MODS_UNPACK_THREADS)
            results = [(label,) + outcome.get(label, ("failed", None)) for _, label, _ in jobs]
        for label, st, path in results:
            self.log(f"[client] mod {label}: {st}" + (f" -> {path}" if path else ""))
            (done if st == "installed" else kept if st == "present" else bad).append(label)
        return {"done": done, "kept": kept, "bad": bad, "skipped": skipped, "approved": sorted(approved)}

    def _mods_installed(self, r):
        """LOOP THREAD half of a mods round: tell the player, and fail the
        round if anything agreed to did not land. The yes was used up when the
        round began."""
        done, kept, bad, skipped = r["done"], r["kept"], r["bad"], r["skipped"]
        self.batch_got |= set(done) | set(kept)
        self.log(f"[client] mods installed: batch={self.batch} done={done} kept={kept} bad={bad} approved={sorted(r['approved'])} got={sorted(self.batch_got)}")
        text = []
        if self.batch:
            text.append(f"mod batch {self.batch[0]}/{self.batch[1]}")
        if done: text.append("Installed from the host: " + ", ".join(done) + " (they show in the load screen's Mods panel)")
        if kept: text.append("already installed: " + ", ".join(kept))
        if bad: text.append("FAILED to install: " + ", ".join(bad) + " -- install it by hand")
        if skipped: text.append("not installed (you did not agree to them): " + ", ".join(skipped))
        if text:
            self.io.emit({"type": "chat", "from": "MULTIPLAYER", "text": "; ".join(text)})
        self.install_result={"installed":done,"present":kept,"failed":bad}
        absent=set(r["approved"])-self.batch_got
        if self.batch and self.batch[0] < self.batch[1]:
            absent=set()                          # the rest of the round is still to come
        if bad or absent:
            self._fail("required mod installation failed: " + ", ".join(sorted(set(bad)|absent)))


def _clear_stale_incoming(directory, log=_log):
    """Delete a previous session's incoming_save.* from ``directory``.

    A joiner runs this BEFORE joining: a stale file from an earlier lobby must
    never be picked up as this session's save (the DLL loads incoming_save.*
    on a start with save=true, and we only emit that after save_ready).
    """
    for suffix in (".sav", ".sav.lua", ".jpg"):
        path = os.path.join(directory, INCOMING_BASENAME + suffix)
        try:
            os.remove(path)
            log(f"[client] removed stale {path}")
        except FileNotFoundError:
            pass
        except OSError as e:
            log(f"[client] WARNING: could not remove stale {path}: {e}")


# --------------------------------------------------------------------------- #
# PUBLISH: the OpenTTD-style public list (netpunch/masterserver.py)
# --------------------------------------------------------------------------- #
LOBBY_VERSION = "0.7"


def version_rejection(remote):
    """Exact release match; legacy peers without a version fail closed."""
    if isinstance(remote, str) and remote == LOBBY_VERSION:
        return None
    label = remote[:64] if isinstance(remote, str) and remote else "unknown (older build)"
    return (f"Multiplayer version mismatch: you have {LOBBY_VERSION}; "
            f"the other side has {label}. Install the same multiplayer version on both sides.")


PUBLISH_EVERY = 10.0        # the master drops a row 30 s after its last announce
class _Publisher:
    """Announces this lobby to the master server every PUBLISH_EVERY seconds
    while ``on``; a ``leave`` goes out when it is switched off or the host
    exits. Runs on its own thread: an HTTP round trip must never stall the
    relay loop. What is published is exactly what a Discord post would be --
    the code (host address + session secret) and a name -- so it is opt-in,
    and a password-locked code shows as locked (useless without the password)."""

    def __init__(self, url, code, kind, locked, log, stable_key=None):
        self.url = url.rstrip("/")
        self.code = code
        self.kind = kind if kind in ("relay", "host", "dedicated") else "host"   # listed as its type, never a save name
        self.locked = bool(locked)
        self.log = log
        # A STABLE id: derived from the lobby name + port + machine, so a
        # restarted relay REPLACES its old row instead of sitting next to a
        # stale copy of itself until that expired (2026-09-10). A plain host
        # gets a fresh id per run (its code changes anyway).
        import hashlib, socket as _sk
        seed = stable_key or ""
        self.id = hashlib.sha256((seed + "|" + _sk.gethostname()).encode("utf-8")).hexdigest()[:16] if seed else os.urandom(8).hex()
        self.name = "host"
        self.players = 1
        self.on = False
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True, name="publish")
        self._t.start()

    def set(self, on):
        self.on = bool(on)
        self._wake.set()

    def update(self, name, players):
        self.name, self.players = name, players

    def close(self):
        self._stop.set(); self._wake.set()
        self._t.join(timeout=6)

    def _post(self, path, body):
        import urllib.request, urllib.error   # noqa: F401 -- HTTPError is caught by the caller
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(self.url + path, data=data,
                                     headers={"Content-Type": "application/json",
                                              "User-Agent": "tpf2mp-lobby/" + LOBBY_VERSION})
        with urllib.request.urlopen(req, timeout=6, context=_master_ssl_context()) as r:
            return r.status

    def _run(self):
        import urllib.error
        announced = False
        while not self._stop.is_set():
            try:
                if self.on:
                    self._post("/announce", {"id": self.id, "name": self.name, "code": self.code,
                                             "players": self.players, "max": CAP, "type": self.kind,
                                             "version": LOBBY_VERSION, "locked": self.locked})
                    if not announced:
                        self.log(f"[publish] listed publicly at {self.url} as {self.name!r}")
                    announced = True
                elif announced:
                    self._post("/leave", {"id": self.id})
                    self.log("[publish] removed from the public list")
                    announced = False
            except Exception as e:                            # noqa: BLE001
                self.log(f"[publish] {self.url}: {e}")
            self._wake.wait(PUBLISH_EVERY)
            self._wake.clear()
        if announced:
            try:
                self._post("/leave", {"id": self.id})
            except Exception:                                 # noqa: BLE001
                pass


# --------------------------------------------------------------------------- #
# RENDEZVOUS: hole punching to the host through the master server
# --------------------------------------------------------------------------- #
# The host used to be reachable only when its port was OPEN: a joiner dials the
# code's addresses and nothing ever came back the other way, so a host whose
# UPnP mapping "succeeded" but did not take (a second router, a firewall) could
# not be joined at all (2026-09-11: open=true in the code, no handshake).
#
# Now every joiner posts its own STUN-observed address to the master server while
# it dials, sealed with a key derived from the code's secret, under a tag also
# derived from it. The host polls its tag, opens the note and fires HELLOs at
# that address for RV_PUNCH_FOR seconds. Its outbound packets open its own NAT
# for the joiner's HELLOs, which then arrive and are ACKed as usual: the classic
# simultaneous punch, over the handshake that already exists. The master can
# neither read a note nor link a tag to a lobby. No master, no punch: joining an
# open host still works exactly as before.
DEFAULT_MASTER = "https://srv1306562.hstgr.cloud/tpf2mp"   # the menu's master_url default
RV_POLL_EVERY = 1.0       # host: seconds between polls for knocks
RV_KNOCK_EVERY = 2.0      # joiner: seconds between knocks while it dials
# Punching is the FALLBACK: a host that UPnP (or a forwarded port) really opened
# answers the plain dial within a second, and then the master never hears of the
# join. Only a dial still unanswered after RV_KNOCK_AFTER seconds knocks.
RV_KNOCK_AFTER = 4.0
RV_PUNCH_FOR = 15.0       # host: seconds to keep punching toward one knocked address
RV_PUNCH_EVERY = 0.2      # host: seconds between punch bursts
# THE RELAY FALLBACK. A host behind CGNAT cannot be punched, and a joiner behind
# a symmetric NAT cannot be punched toward; the pair had no path at all
# (2026-09-18). From its second knock on, a joiner still unanswered asks the
# master for a relay port; the master binds one UDP port for that joiner and
# tells both ends (the joiner in the reply, the host with the note). Each end
# sends a bind packet to it, then the port swaps their datagrams verbatim: the
# frames stay sealed, the master forwards what it cannot read. The joiner adds
# the port to its dial as one more candidate, so a direct path that answers
# first still wins, and the host binds every RV_PUNCH_EVERY until that peer is in.
RV_RELAY_FROM_KNOCK = 2   # the joiner asks for the relay on this knock and after
RELAY_MAGIC = b"TRLB"
RELAY_ACK = b"TRLA"


def _rv_tag(secret):
    return hashlib.sha256(b"tpf2mp-rendezvous-v1|" + bytes(secret)).hexdigest()[:24]


def _rv_sealer(secret, password):
    # its own Sealer (own replay window) under a key separate from the session's
    return Sealer(hashlib.sha256(derive_key(secret, password or "") + b"|rendezvous").digest())


def _steam_tunnel(args):
    """The Steam tunnel client for this lobby (steamtunnel.py): a no-op object
    without a data dir, the kill switch, or a bridge whose Steam is not up."""
    d = getattr(args, "sync_runtime_dir", None) or os.environ.get("TPF2MP_DATADIR") \
        or os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "tpf2mp", "data")
    t = steamtunnel.SteamTunnel(d, _log, wait=1.0)   # the bridge wrote the identity long before HOST/JOIN; 1 s covers a game still starting
    if not t.available:
        _log("[steam] no Steam transport (no tunnel identity in the data folder)")
    return t


def _rv_url(args):
    """The master used for knocks: --rendezvous, else --publish, else the default;
    '--rendezvous off' disables them."""
    v = (getattr(args, "rendezvous", "") or "").strip()
    if v.lower() == "off":
        return ""
    return (v or getattr(args, "publish", "") or DEFAULT_MASTER).rstrip("/")


def _http_json(url, body=None, timeout=6):
    import urllib.request
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data,
                                 headers={"Content-Type": "application/json",
                                          "User-Agent": "tpf2mp-lobby/" + LOBBY_VERSION})
    # roots.ssl_context: the system store plus the ISRG roots, so a Windows
    # without ISRG Root X2 does not fail the master's chain as "expired"
    with urllib.request.urlopen(req, timeout=timeout, context=_master_ssl_context()) as r:
        return json.loads(r.read().decode("utf-8") or "{}")


_SSL_CTX = [None]


def _master_ssl_context():
    if _SSL_CTX[0] is None:
        try:
            from roots import ssl_context
            _SSL_CTX[0] = ssl_context()
        except ImportError:                    # a deploy that shipped lobby.py without roots.py
            import ssl
            _SSL_CTX[0] = ssl.create_default_context()
    return _SSL_CTX[0]


class _RendezvousHost:
    """Polls the master for joiners' sealed address notes; each valid one is put
    on ``queue`` as a list of (ip, port) punch targets for run_host."""

    def __init__(self, url, secret, password, log, poll_every=RV_POLL_EVERY, tunnel=None):
        self.url, self.tag, self.log = url.rstrip("/"), _rv_tag(secret), log
        self.sealer = _rv_sealer(secret, password)
        self.poll_every = poll_every
        self.tunnel = tunnel                  # the Steam tunnel: a knock that names a SteamID is dialled through it
        self.queue = queue.Queue()
        self._since = 0.0
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True, name="rendezvous")
        self._t.start()

    def close(self):
        self._stop.set()
        # Linux's launcher gives quit 1.5 s, then SIGTERM 2 s. A daemon HTTP
        # poll owns no cleanup resources: waiting for it here can exhaust that
        # budget before the publisher sends /leave and UPnP unmaps the port.
        # Setting the event prevents another poll; process exit ends this one.
        self._t.join(timeout=3 if sys.platform == "win32" else 0)

    @staticmethod
    def relay_from(k):
        """A knock's relay allocation -> (ip, port, id bytes), or None."""
        r = k.get("relay") if isinstance(k, dict) else None
        if not isinstance(r, dict):
            return None
        try:
            ip, port, aid = str(r.get("ip") or ""), int(r.get("port") or 0), bytes.fromhex(str(r.get("id") or ""))
        except (TypeError, ValueError):
            return None
        if not ip or not (0 < port < 65536) or len(aid) != 16:
            return None
        return (ip, port, aid)

    def targets_from(self, blob_b64):
        """A knock's blob -> [(ip, port), ...], or None if it is not ours."""
        import base64
        try:
            raw = base64.b64decode(blob_b64, validate=True)
        except (ValueError, TypeError):
            return None
        plain = self.sealer.open(raw)
        if plain is None:
            return None
        try:
            prof = decode_code(plain.decode("ascii"))
        except (ValueError, UnicodeDecodeError, KeyError, IndexError):
            return None
        out = []
        for key in ("public_v4", "lan_v4"):
            hp = parse_hostport(prof.get("candidates", {}).get(key))
            if hp and hp[1] and hp not in out:
                out.append(hp)
        # the joiner's Steam identity: open its session from our side (the OPEN
        # implicitly accepts) and punch at the endpoint like any other address
        sid = prof.get("candidates", {}).get("steam")
        if sid and self.tunnel is not None and self.tunnel.available:
            ep = self.tunnel.dial(sid)
            if ep and ep not in out:
                self.log(f"[steam] the joiner is {sid} on Steam -- session opened, punching at {ep[0]}:{ep[1]}")
                out.append(ep)
        return out or None

    def _run(self):
        warned = False
        while not self._stop.is_set():
            try:
                r = _http_json(f"{self.url}/knock?s={self.tag}&since={self._since:.3f}")
                for k in r.get("knocks") or []:
                    try:
                        self._since = max(self._since, float(k.get("t") or 0))
                    except (TypeError, ValueError):
                        continue
                    t = self.targets_from(str(k.get("blob") or ""))
                    if t:
                        self.log(f"[rendezvous] a joiner knocked: punching toward {t}")
                        r = self.relay_from(k)
                        if r:
                            # a 3-tuple rides in the same list: run_host binds it
                            self.log(f"[rendezvous] the joiner asked for the master's relay at {r[0]}:{r[1]} -- binding to it")
                            t = list(t) + [r]
                        self.queue.put(t)
                if warned:
                    self.log("[rendezvous] master reachable again")
                warned = False
            except Exception as e:                            # noqa: BLE001
                if not warned:
                    self.log(f"[rendezvous] cannot poll {self.url} ({e}) -- joiners need our port open")
                    warned = True
            self._stop.wait(self.poll_every)


class _RendezvousKnock:
    """A joiner: posts its sealed profile code to the master every RV_KNOCK_EVERY
    seconds until closed (a fresh seal each time, so the host's replay window
    accepts every one)."""

    def __init__(self, url, secret, password, profile_code, log, every=RV_KNOCK_EVERY,
                 delay=RV_KNOCK_AFTER, sock=None, late=None, relay_from=RV_RELAY_FROM_KNOCK):
        self.url, self.tag, self.log, self.every = url.rstrip("/"), _rv_tag(secret), log, every
        self.delay = delay
        self._sealer = _rv_sealer(secret, password)
        self._plain = profile_code.encode("ascii")
        self.sent = 0
        # the relay fallback: ``sock`` is the game socket (the bind must come
        # from the address the HELLOs come from), ``late`` the race's extra
        # target list; None disables the request
        self.sock, self.late, self.relay_from = sock, late, relay_from
        self.nonce = secrets.token_hex(6)
        self.relay = None                     # (ip, port, id) once the master allocated one
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True, name="knock")
        self._t.start()

    def close(self):
        self._stop.set()
        # As with the host poll, an in-flight daemon request must not hold up
        # Linux process teardown. It has no save/socket/mapping cleanup duty.
        self._t.join(timeout=3 if sys.platform == "win32" else 0)

    def _run(self):
        import base64
        warned = False
        if self._stop.wait(self.delay):
            return                                # the direct dial connected first: no knock
        if self.delay > 0:
            self.log(f"[rendezvous] no answer to the direct dial after {self.delay:.0f} s -- asking the host to punch")
        while not self._stop.is_set():
            try:
                blob = base64.b64encode(self._sealer.seal(self._plain)).decode("ascii")
                body = {"s": self.tag, "blob": blob}
                want_relay = self.sock is not None and self.late is not None and self.sent + 1 >= self.relay_from
                if want_relay:
                    body["relay"] = 1
                    body["j"] = self.nonce
                r = _http_json(self.url + "/knock", body)
                self.sent += 1
                if self.sent == 1:
                    self.log("[rendezvous] knocked at the master: the host punches toward us")
                if want_relay:
                    self._relay_bind(_RendezvousHost.relay_from(r))
            except Exception as e:                            # noqa: BLE001
                if not warned:
                    self.log(f"[rendezvous] cannot knock at {self.url} ({e}) -- dialing the host directly only")
                    warned = True
            self._stop.wait(self.every)

    def _relay_bind(self, r):
        """Bind to the master's relay port and add it to the dial (once)."""
        if r is None:
            if self.relay is None and self.sent == self.relay_from:
                self.log("[rendezvous] the master offers no relay -- the punch is the only fallback")
            return
        ip, port, aid = r
        try:
            self.sock.sendto(RELAY_MAGIC + aid + b"J", (ip, port))
        except OSError:
            return
        if self.relay is None:
            self.relay = r
            if (ip, port) not in self.late:
                self.late.append((ip, port))
            self.log(f"[rendezvous] no direct path yet -- the master relays for us at {ip}:{port}, dialling it too")


# --------------------------------------------------------------------------- #
# HOST: single socket, N peers, authority for roster + chat relay
# --------------------------------------------------------------------------- #
def _origin_letter(idx):
    """a..z, then aa, ab, ... -- the same sequence the menu DLL's originName() uses."""
    if idx < 26:
        return chr(97 + idx)
    idx -= 26
    return chr(97 + (idx // 26) % 26) + chr(97 + idx % 26)


class _PeerConn:
    """The receive side (_ClientSaveReceiver) talks to 'the host' through a
    .send(bytes); on the relay the sender is one joiner, so wrap (sock, addr)."""
    def __init__(self, sock, addr):
        self.sock, self.addr = sock, addr
    def send(self, payload):
        for piece in _fragments(payload):
            self.sock.sendto(_pack_data(piece), self.addr)


def run_host(sock, my_name, io, code=None, stop=None, drop_after=DROP_AFTER,
             log=_log, relay=None, forward_logs=(), publisher=None, lobby_name="",
             relay_only=False, punch_q=None, sync_runtime=None, companies_mode=False,
             cross_code=None, steam_code=None, steam_secret=None, crossplay=True):
    """Run the lobby server forever on ``sock`` (blocks until ``stop`` is set).

    ``punch_q`` (a queue of [(ip, port), ...] from :class:`_RendezvousHost`):
    each entry is punched toward with HELLOs for RV_PUNCH_FOR seconds.

    ``sock`` is a bound UDP socket (the observe/game socket for the real CLI, a
    plain loopback socket for the self-test). ``io`` is a :class:`LobbyIO`.
    ``relay`` is an optional :class:`GameRelay`: local bridge frames fan out
    to every joiner, joiners' 'g' frames go to the local bridge.

    STEAM BY DEFAULT (2026-09-22). ``steam_code`` is this host's SteamID64 when its game
    has Steam's networking; ``cross_code`` the classic code. With ``crossplay`` off the
    code shown and listed is the Steam ID, a joiner gets the session secret over the
    tunnel (TYPE_KEYX, steamkey.py, answered with ``steam_secret``) and a HELLO from
    anywhere but a Steam tunnel endpoint goes unanswered: only players on Steam get in.
    The host's 'crossplay' command switches it live -- on, the classic code is shown and
    listed and anyone with it can join, exactly as before this change.
    """
    stop = stop or threading.Event()
    sock.setblocking(False)
    _boost_socket_buffers(sock)             # help bursty save-transfer traffic
    # NETWORK IMPAIRMENT for this instance (netsim.py, tpf2mp_netsim.txt in the io dir)
    sim = netsim.read_config(io.dir)
    if sim:
        sock = netsim.ImpairedSocket(sock, sim, log)
        log(f"[netsim] impairing what this instance sends: {netsim.describe(sim)}")
    # THE TCP BACKUP LINK: every sealed frame to a joiner goes out on its TCP link
    # too, and its TCP copies come in through this same socket (dual_tcp.py)
    dual = None
    if _tcp_backup_on(io.dir) and SEAL[0] is not None:
        sock = dual_tcp.DualSocket(sock, log)
        dual = sock
        if sim:
            dual.link_delay = sim["delay"]
    DUAL[0] = dual
    last_dual_tick = [0.0]

    transport_lobby = os.urandom(16).hex()
    io.emit(dict(type='transport_lobby', epoch=transport_lobby))
    resync_world = [None]                   # the completed resync's epoch the nonce last followed (a world switch mints its own)
    host_name = _dedupe(my_name, set())     # reassigned by the 'name' command
    peers = collections.OrderedDict()       # addr -> {"name":str, "last":float,
                                            #          "started":bool,
                                            #          "profile":code|None,
                                            #          "links":[names],
                                            #          "company":1..200}
    host_company = [1]                      # the host's own company id
    host_stage = [""]                       # what the host itself is doing (its own load / a world switch), "" = nothing (2026-09-16)
    # The lobby's mode (2026-09-16): "coop" puts everyone in company 1; "companies"
    # gives every player their own. It sets the chips automatically, on a change
    # and for each joiner; a chip click still overrides one player.
    mode = ["companies" if companies_mode else "coop"]
    cid_counter = [0]                       # host-authoritative chat id
    started = [False]
    start_save = [False]                    # save flag of the last broadcast start
    last_emitted_roster = [None]
    transfer = [None]                       # the active _HostSaveTransfer, or None
    unplaced_feedback = set()               # (addr, sid) of facks/fdones the transfer could not place, logged once each
    upload = [None]                         # relay-only: the leader's save coming in
    pending_resume = [None]                 # relay-only: (leader addr, when) -- the stored world goes out then
    # A relay that holds a world loads it as soon as a leader arrives (2026-09-12: the
    # 10 s grace and /resume are gone -- simpler; /new stays, until the world is sent).
    # The few seconds let players who arrive together share one transfer instead of two.
    RESUME_GRACE = 3.0
    letters = {}                            # relay-only: name -> origin letter (sticky)
    letters_path = os.path.join(io.dir, "relay_letters.json")
    chips = {}                              # relay-only: name -> company chip (sticky, like letters)
    chips_path = os.path.join(io.dir, "relay_companies.json")
    if relay_only:
        try:
            with open(letters_path, "r", encoding="utf-8") as f:
                letters.update({str(k): str(v) for k, v in json.load(f).items()})
            log(f"[relay] {len(letters)} letter(s) remembered from the last run")
        except (OSError, ValueError):
            pass
        try:
            with open(chips_path, "r", encoding="utf-8") as f:
                chips.update({str(k): int(v) for k, v in json.load(f).items()})
        except (OSError, ValueError):
            pass

    def remember_chip(name, cid):
        if not relay_only:
            return
        chips[name] = int(cid)
        try:
            with open(chips_path, "w", encoding="utf-8") as f:
                json.dump(chips, f)
        except OSError:
            pass

    def leader_addr():
        """relay-only: the oldest connected joiner (peers is insertion-ordered)."""
        for a in peers:
            return a
        return None

    def leader_name():
        """The roster's ``host``: this lobby's host, unless we are a relay --
        then the oldest joiner. (0.4.5-0.4.9 returned the oldest joiner for
        EVERY lobby, so a plain HOST GAME told the joiner it was the host and
        both sides derived the wrong letters; nobody could connect.)"""
        if not relay_only:
            return host_name
        a = leader_addr()
        return peers[a]["name"] if a is not None else ""

    def letter_for(name):
        if name not in letters:
            used = set(letters.values())
            i = 0
            while _origin_letter(i) in used:
                i += 1
            letters[name] = _origin_letter(i)
            try:
                with open(letters_path, "w", encoding="utf-8") as f:
                    json.dump(letters, f)
            except OSError:
                pass
        return letters[name]

    HOTJOIN_STORED_MAX = 180.0                # a late joiner is served from the stored world when it is this fresh
    RELAY_MODS_GRACE = 8.0                    # seconds a completed upload waits for the leader's mods round before it goes out

    session_epoch = [0.0]                     # when the current session's world left the relay (resume push)

    def stored_age():
        """Age of the stored world for the freshness rule. A copy the running
        session was RESUMED from is the session's own starting state: it is
        current until the leader uploads a newer one, however old its mtime
        (an hour-old copy was refused to three joiners while the leader had
        not even loaded yet, 2026-09-10)."""
        p_, age = stored_save()
        if age is None:
            return -1
        try:
            if os.path.getmtime(p_) <= session_epoch[0] and session_epoch[0] > 0:
                return 0
        except OSError:
            pass
        return int(age)

    def stored_save():
        """relay-only: the last save uploaded here, if any (path, age seconds)."""
        path = os.path.join(io.dir, INCOMING_BASENAME + ".sav")
        try:
            return path, time.time() - os.path.getmtime(path)
        except OSError:
            return None, None

    xplay = [bool(crossplay) or not steam_code]      # no Steam here: the classic code is the only one
    keyx_last = {}                                   # tunnel endpoint -> (offer, answer, when)
    shown_code = [code]

    def emit_code():
        io.emit({"type": "code", "code": shown_code[0], "steam": steam_code or "",
                 "crossplay": xplay[0], "cross_code": cross_code or ""})

    def set_crossplay(on):
        on = bool(on) or not steam_code
        xplay[0] = on
        shown_code[0] = (cross_code or code) if on else steam_code
        if publisher is not None and shown_code[0]:
            publisher.code = shown_code[0]
            publisher._wake.set()
        emit_code()

    if steam_code:
        set_crossplay(xplay[0])
        log("[host] CROSS-PLAY " + ("ON: the classic code works for players without Steam too" if xplay[0] else
            "OFF: the code is this host's Steam ID; only players on Steam can join"))
    elif code:
        emit_code()

    # merged log: our own lines + every joiner's, tagged; extra files tailed
    peers_log = PeersLog(io.dir)
    own_fwd = LogForwarder(forward_logs)
    _log_sinks.append(own_fwd.add)
    log(f"[host] merged log -> {peers_log.path}")

    # ---- roster helpers ---------------------------------------------------- #
    def all_names(exclude_addr=None):
        names = {host_name}
        for a, p in peers.items():
            if a != exclude_addr:
                names.add(p["name"])
        return names

    def roster_players():
        if relay_only:
            return sorted(p["name"] for p in peers.values())
        return sorted([host_name] + [p["name"] for p in peers.values()])

    def host_has_world():
        """The host's game is in a world: the native adapter's status file
        (written by the game itself, PID-scoped) says has_world=1. `started`
        only latches after a START GAME or a save share, so a host that loaded
        its world on its own and then got a joiner read as "not started" -- the
        joiner was not late, no frozen join ran, and the menu's autosave path
        served it while the world kept ticking (2026-09-17 00:05)."""
        return sync_runtime is not None and sync_runtime._read('tpf2_native_status.txt').get('has_world') == '1'

    def recovery_supported():
        return (sync_runtime is not None and not relay_only and len(peers) >= 1
                and all(p.get("recovery") == 4 for p in peers.values())
                and (started[0] or host_has_world()) and transfer[0] is None)

    def recovery_unavailable_reason():
        if not peers:
            return "Resync needs at least one connected player."
        if not all(p.get("recovery") == 4 for p in peers.values()):
            return "Every player needs a version that supports resync."
        if not started[0]:
            return "Start the multiplayer game before requesting resync."
        if transfer[0] is not None:
            return "A player is receiving the save. Wait for that transfer to finish, then try again."
        return "Resync is not ready yet."

    frags = _Reassembler(log)                 # big control messages from joiners, per address

    def recovery_send(name, message):
        for address, peer in peers.items():
            if peer["name"] == name:
                _send_data(sock, address, message)

    def recovery_transfer(sid, blob, files, targets):
        # The frozen join's and the resync's save carries its mod list like START
        # GAME's does. It never did: every joiner of a dedicated server heard "the
        # host cannot read which mods its save needs" (2026-09-18) -- the list was
        # never asked for, not unreadable. The .sav is the part of the blob its
        # entry names; None stays None (unknown, said so).
        mods, off = None, 0
        for f in files or []:
            size = int(f.get("size") or 0)
            if str(f.get("name", "")).endswith(".sav"):
                mods = modshare.save_mod_list_bytes(bytes(blob[off:off + size]), log)
                break
            off += size
        if mods is None:
            log("[host] the sync save's mod list could not be read -- the joiner is told, nothing is offered")
        elif mods:
            log(f"[host] the sync save needs {len(mods)} mod(s) besides ours: " + ", ".join(modshare.mod_folder_name(m, v) for m, v in mods))
        return _HostSaveTransfer(sock, sid, blob, files, targets, io, log, mods=mods)

    recovery = HostRecovery(sync_runtime, host_name, io, recovery_send,
        roster_players, lambda: [(a, p["name"]) for a, p in peers.items()],
        recovery_transfer,
        available=recovery_supported, unavailable_reason=recovery_unavailable_reason,
        live_join=lambda: _live_join_on(io.dir)) if sync_runtime is not None and not relay_only else None

    def roster_companies():
        """name -> company id. Same id = same company (co-op); different ids =
        separate companies. Everyone starts on 1, so nothing changes until
        someone clicks a chip."""
        m = {} if relay_only else {host_name: host_company[0]}
        for p in peers.values():
            m[p["name"]] = int(p.get("company", 1))
        return m

    def free_company(taken):
        """the lowest company id nobody in `taken` uses"""
        for cid in range(1, MAX_COMPANIES + 1):
            if cid not in taken:
                return cid
        return MAX_COMPANIES

    def assign_by_mode():
        """coop: everyone on 1. companies: the host (relay: the leader) keeps 1,
        the others get the lowest free id in join order."""
        changed = False
        if mode[0] == "coop":
            if not relay_only and host_company[0] != 1:
                host_company[0] = 1
                changed = True
            for p in peers.values():
                if int(p.get("company", 1)) != 1:
                    p["company"] = 1
                    remember_chip(p["name"], 1)
                    changed = True
            return changed
        taken = set() if relay_only else {1}
        if not relay_only and host_company[0] != 1:
            host_company[0] = 1
            changed = True
        seen = set()
        for p in peers.values():                     # join order: the leader first on a relay
            cid = int(p.get("company", 1))
            if cid in taken or cid in seen:
                cid = free_company(taken | seen)
            if int(p.get("company", 1)) != cid:
                p["company"] = cid
                remember_chip(p["name"], cid)
                changed = True
            seen.add(cid)
        return changed

    def set_mode(value):
        value = "companies" if value == "companies" else "coop"
        if value == mode[0]:
            return False
        mode[0] = value
        assign_by_mode()
        return True

    def set_company(name, cid):
        try:
            cid = int(cid)
        except (TypeError, ValueError):
            return False
        if not 1 <= cid <= MAX_COMPANIES:
            return False
        if name == host_name:
            host_company[0] = cid
            return True
        for p in peers.values():
            if p["name"] == name:
                p["company"] = cid
                remember_chip(name, cid)
                return True
        return False

    def roster_stages():
        # what each joiner is doing right now ("receiving save 40%", "loading
        # world", "catching up (12 s behind)"): shown beside the name in every
        # panel while a hot join runs; empty once in sync (2026-09-16)
        stages = {p["name"]: p["stage"] for p in peers.values() if p.get("stage")}
        if host_stage[0] and not relay_only:
            stages[host_name] = host_stage[0]
        return stages

    def sender_stage(name, text, pct=None):
        # The host's own save sender knows how far a joiner's save is before the
        # joiner says so: put it in the roster right away (2026-09-16). The
        # joiner's own report still overrides -- see _merge_sender_stage.
        for p in peers.values():
            if p["name"] != name:
                continue
            new = _merge_sender_stage(p.get("stage", ""), text, pct)
            if new is not None and new != p.get("stage", ""):
                p["stage"] = new
                log(f"[host] {name} stage <- sender: {new}")
                send_roster_packets()
                emit_roster()
            return

    def send_roster_packets():
        # Doubles as the start self-heal: a joiner that lost the whole start
        # burst sees started:true here (~2 s later) and starts. The flag is
        # PER PEER so a late joiner (not included in the start) never starts
        # off a heal -- it needs the host to press START GAME again.
        players = roster_players()
        profiles = {p["name"]: p["profile"] for p in peers.values()
                    if p.get("profile")}
        links = {p["name"]: p.get("links", []) for p in peers.values()}
        companies = roster_companies()
        for a, p in list(peers.items()):
            _send_data(sock, a, {"t": "roster", "version": LOBBY_VERSION, "transport_lobby": transport_lobby, "players": players, "recovery": 4 if recovery_supported() else 0,
                                 "host": leader_name(), "lobby": lobby_name,
                                 "relay": relay_only, "mods": advertised[1],
                                 "mods_unknown": advertised[1] is None,
                                 "stored_age": stored_age() if relay_only else -1,
                                 "stored_max": int(HOTJOIN_STORED_MAX) if relay_only else -1,
                                 "letters": {p2["name"]: letter_for(p2["name"]) for p2 in peers.values()} if relay_only else None,
                                 "started": bool(p.get("started")),
                                 "start_save": start_save[0],
                                 "profiles": profiles, "links": links,
                                 "companies": companies, "stages": roster_stages(),
                                 "mode": mode[0]})

    def emit_roster():
        if publisher is not None:
            publisher.update(lobby_name or host_name, len(peers) + (0 if relay_only else 1))
        players = roster_players()
        # join_freeze: the menu must NOT take its own hot-join save when the
        # roster grows -- the lobby brings the newcomer in through a recovery
        # round (do_join). Off when recovery cannot run (an old client, a
        # transfer in flight, a relay), so the menu's save still serves then.
        freeze = bool(recovery is not None and not relay_only and (recovery.held or recovery_supported())
                      and not (_live_join_on(io.dir) and not recovery.held))   # live join: the menu's save serves
        io.emit({"type": "roster", "players": players,
                 "you": host_name, "host": leader_name(), "lobby": lobby_name,
                 "relay": relay_only, "companies": roster_companies(), "stages": roster_stages(),
                 "mode": mode[0], "join_freeze": freeze})
        io.write_state(state="connected", code=code, players=players,
                       you=host_name, host=leader_name(), started=started[0],
                       lobby=lobby_name, companies=roster_companies(), mode=mode[0])

    def roster_changed(broadcast=True):
        """Push the roster to peers, and emit an event only if it changed."""
        if relay_only and not peers and (started[0] or transfer[0] is not None or upload[0] is not None):
            # the last player left: the session is over. The next joiner is a
            # fresh leader (not a "late" one), the stored save is offered again,
            # and no transfer is left dangling.
            started[0] = False
            start_save[0] = False
            transfer[0] = None
            upload[0] = None
            session_epoch[0] = 0.0
            spath, age = stored_save()
            log("[relay] everyone left -- session closed" + (f"; holding a save from {int(age)} s ago -- the next player continues it" if spath else ""))
        if broadcast:
            send_roster_packets()
        key = (tuple(roster_players()), tuple(sorted(roster_companies().items())), mode[0])
        if last_emitted_roster[0] != key:
            last_emitted_roster[0] = key
            emit_roster()

    def mod_package(m,v):
        if modshare.is_dlc(m) or not SHARE_MODS[0]: return None
        if relay_only:
            path=os.path.join(io.dir,"mod_cache",modshare.cache_name(m,v))
            try:
                with open(path,"rb") as f: return f.read()     # whole: a cached mod is as big as it is
            except OSError: return None
        return modshare.package_mod(m,v, level=MODS_ZIP_LEVEL, log=log)

    def broadcast_chat(frm, text):
        cid_counter[0] += 1
        ts = int(time.time())
        msg = {"t": "chat", "from": frm, "text": text, "ts": ts,
               "cid": cid_counter[0]}
        for _ in range(CHAT_BURST):
            for a in list(peers):
                _send_data(sock, a, msg)
        io.emit({"type": "chat", "from": frm, "text": text, "ts": ts})

    def mod_list_note(path, where):
        """The save's mod list could not be READ. Say so everywhere it matters
        -- the log, the host's panel, the lobby chat -- and advertise the list
        as UNKNOWN (None). It must never become "no mods": that told every
        joiner the save needed nothing, and the game then refused to load it
        on any machine that lacked a mod (2026-09-16)."""
        log(f"[host] the mod list of {path} is UNKNOWN ({where}); players are told so, and "
            "nobody is offered the mods it needs -- if the game refuses to load it, install the host's mods by hand")
        io.emit({"type": "chat", "from": "MULTIPLAYER",
                 "text": f"Could not read which mods {os.path.basename(str(path))} needs; players who lack them "
                         "will not be offered a download. If their game refuses the save, they must install your mods by hand."})

    cached_world = stored_save()[0] if relay_only else None
    advertised = [cached_world, modshare.save_mod_list(cached_world, log) if cached_world else []]
    if cached_world and advertised[1] is None:
        mod_list_note(cached_world, "stored world")
    preflight_requests = {}
    mod_preflight = [False]
    pending_start = [None]
    serve_hold = [0.0]        # until when the serve-again waits for the host's hot-join save
    mod_round = [None]        # the addrs to start once a mods round resolves
    pack_job = [None]         # the worker packaging one joiner's mods (see the preflight block)
    pack_queue = []           # jobs waiting for the worker (a round asked for while a preflight runs)

    def broadcast_start(save, only=None):
        """Start everyone currently in the lobby -- or, with ``only`` (a set of
        addrs), just those: the peers a save transfer actually reached. A
        peer that joined DURING the transfer has no save; starting it anyway
        marked it started, its start(save=true) was refused for want of a
        save, and nothing ever served it again (relay, 2026-09-10). Those stay
        unstarted and get the next push (serve loop below)."""
        started[0] = True
        start_save[0] = bool(save)
        targets = [a for a in list(peers) if only is None or a in only]
        # A WORLD SWITCH is flagged PER PEER. The peers that were playing have
        # to be told they are leaving that world (their menu loads the new save
        # in place, without going back to the title screen); a peer that joined
        # while the switch was being pushed is an ordinary newcomer and gets an
        # ordinary start. A peer marked for the switch that this round did not
        # reach keeps its flag for the round that finally serves it.
        switching = {a for a in targets if peers[a].pop("switch", False)}
        for a in targets:
            peers[a]["started"] = True      # heal roster carries started:true
        if relay_only and upload[0] is not None and getattr(upload[0], "complete", False):
            upload[0] = None                # this upload has been distributed
        for _ in range(CHAT_BURST):
            for a in targets:
                msg = {"t": "start", "save": start_save[0]}
                if a in switching:
                    msg["switch"] = True
                _send_data(sock, a, msg)
        event = {"type": "start", "save": start_save[0]}
        if switching:
            event["switch"] = True
        io.emit(event)
        io.write_state(started=True)
        log(f"[host] START broadcast (save={start_save[0]}"
            f"{', world switch' if switching else ''}) to "
            f"{len(targets)} of {len(peers)} peer(s)")

    # ---- inbound lobby messages -------------------------------------------- #
    def do_join(addr, name, profile=None, is_mesh=False, version=None, recovery_protocol=0):
        reason = version_rejection(version)
        if reason:
            # Phrase the error from the joining player's perspective.
            remote = version[:64] if isinstance(version, str) and version else "unknown (older build)"
            reason = (f"Multiplayer version mismatch: you have {remote}; the host has "
                      f"{LOBBY_VERSION}. Install the same multiplayer version on both sides.")
            _send_data(sock, addr, {"t": "reject", "reason": reason})
            if addr in peers:
                del peers[addr]
                if transfer[0] is not None:
                    transfer[0].on_peer_dropped(addr)
                roster_changed()
            log(f"[host] rejected incompatible multiplayer version: {remote!r}")
            return
        # Only while a recovery is actually in flight (HostRecovery.roster_locked).
        # Testing the bare operation token here kept rejecting every new joiner
        # after the first resync, for as long as this lobby process lived --
        # including in the next NEW game, since the lobby outlives the world.
        # A recovery in flight no longer fixes the player list (2026-09-16): a
        # join IS a recovery round now, and a player arriving during one is
        # admitted by the barrier (SyncOperation._admit) -- unless its version
        # cannot take part, which would strand the round.
        if recovery and recovery.roster_locked and addr not in peers and recovery_protocol != 4:
            _send_data(sock, addr, {"t": "reject", "reason": "A world sync is in progress and your version cannot take part in it. Update the mod, or try again in a moment."})
            return
        late = False
        if addr in peers:                                   # rename in place
            peers[addr]["name"] = _dedupe(name, all_names(exclude_addr=addr))
            peers[addr]["asked"] = name
            if profile:
                peers[addr]["profile"] = profile
            peers[addr]["mesh"] = bool(is_mesh)
        else:                                               # brand-new joiner
            if len(peers) + 1 >= CAP:
                _send_data(sock, addr, {"t": "reject", "reason": "lobby full"})
                log(f"[host] rejected {addr} (lobby full)")
                return
            assigned = _dedupe(name, all_names())
            # coop: company 1. companies: a returning name gets its chip back when
            # nobody took it, anyone else the lowest free id.
            if mode[0] == "companies":
                taken = set(roster_companies().values())
                company = chips.get(assigned) if chips.get(assigned) not in taken else None
                company = company or free_company(taken)
            else:
                company = 1
            peers[addr] = {"name": assigned, "asked": name, "last": time.time(),
                           "started": False, "profile": profile,
                           "links": [], "mesh": bool(is_mesh),
                           "company": company}
            remember_chip(assigned, company)
            if dual is not None and isinstance(addr, tuple) and not dual.has_link(addr):
                # our half of the TCP simultaneous open toward the joiner (a NAT that
                # preserves ports lets it land on the joiner's listener); the joiner's
                # own connect to our listener is the usual way in
                def host_dial(a=addr, n=assigned):
                    c, how = dual_tcp.dial(a, sock.getsockname()[1], _dual_hello(host_name), log, listen_too=False)
                    if c is not None:
                        if dual.has_link(a):
                            c.close()
                        else:
                            dual.attach(c, a, "host connected")
                threading.Thread(target=host_dial, name="dual-host-dial", daemon=True).start()
            late = started[0] or host_has_world()
            log(f"[host] JOIN {addr} as {assigned!r}"
                + (" (late -- game already started)" if started[0] else " (late -- the host is in a world)" if late else ""))
        peers[addr]["last"] = time.time()
        peers[addr]["recovery"] = recovery_protocol
        # FROZEN JOIN (2026-09-16): a late joiner in a player-hosted session is
        # brought in through a recovery round -- the session holds, the host
        # saves, everyone (host included) loads that save -- instead of the
        # running host sharing an autosave for the newcomer to catch up on.
        # The menu's own hot-join save stands down when the roster says
        # join_freeze (emit_roster). Falls back to that path when recovery is
        # not available (an old client, a transfer in flight): logged.
        # LIVE JOIN (2026-09-22, user: "get rid of the forced pausing and the
        # screen that pops up"): with tpf2mp_live_join.txt = 1 there is no round
        # at all -- nobody holds, nobody sees the recovery window. The host's
        # menu takes its hot-join save while the session runs (join_freeze is
        # false, emit_roster), the newcomer loads it and catches up on the
        # command history. That path diverged the person sim until the engine
        # read its batches in entity-id order (docs/re/HOTJOIN_ORDER.md).
        if late and recovery and not relay_only and _live_join_on(io.dir):
            # marked, so the waiting-member sweep below never starts a round for it
            # either (it did, at once: a host that loaded its world alone never
            # latched `started`, and the sweep held everyone, 2026-09-22)
            peers[addr]["live_join"] = True
            log(f"[host] live join for {peers[addr]['name']!r}: the session keeps running; the menu's hot-join save "
                "serves it and it catches up on the command history")
        elif late and recovery and not relay_only:
            if recovery_protocol == 4 and recovery.join(peers[addr]["name"]):
                log(f"[host] frozen join for {peers[addr]['name']!r}: holding the session, "
                    + ("the players in keep their worlds, the newcomer loads (live join)" if recovery.barrier.retain
                       else "everyone loads the shared world"))
            else:
                log(f"[host] frozen join NOT possible for {peers[addr]['name']!r} "
                    f"({'its version has no resync' if recovery_protocol != 4 else recovery_unavailable_reason()}) -- the menu's hot-join save serves it")
        if relay_only:
            letter_for(peers[addr]["name"])
        _send_data(sock, addr, {"t": "welcome", "version": LOBBY_VERSION, "transport_lobby": transport_lobby,
                                "you": peers[addr]["name"], "host": leader_name(),
                                "recovery": 4 if recovery_supported() else 0,
                                "lobby": lobby_name, "relay": relay_only, "mods": advertised[1],
                                "mods_unknown": advertised[1] is None})
        if relay_only and started[0] and addr != leader_addr() and not peers[addr].get("started"):
            age = stored_age()
            if 0 <= age <= HOTJOIN_STORED_MAX:
                _send_data(sock, addr, {"t": "status", "state": "connected",
                                        "detail": f"joining the running game: the relay is sending you its world ({age} s old)\u2026"})
                log(f"[relay] late joiner {peers[addr]['name']!r}: serving the stored world ({age} s old), no sync from the leader")
            else:
                _send_data(sock, addr, {"t": "status", "state": "connected",
                                        "detail": "joining the running game: waiting for the leader's save\u2026"})
        if relay_only and not started[0] and transfer[0] is None and upload[0] is None:
            # A session that has not started. With a stored world the relay loads it
            # for everyone (the leader receives the save like any joiner and starts);
            # without one the leader sends theirs with START GAME.
            spath, age = stored_save()
            if addr == leader_addr():
                if spath:
                    _send_data(sock, addr, {"t": "status", "state": "connected",
                                            "detail": f"loading the relay's world (saved {int(age // 60)} min ago)…"})
                    log(f"[relay] fresh session with a stored save ({int(age)} s old): loading it for {peers[addr]['name']!r}")
                    pending_resume[0] = (addr, time.time() + RESUME_GRACE)
                else:
                    _send_data(sock, addr, {"t": "status", "state": "connected",
                                            "detail": "this relay has no saved world yet -- press START GAME to send your most recent save"})
                    log(f"[relay] fresh session, no stored save: waiting for {peers[addr]['name']!r} to press START GAME")
            else:
                _send_data(sock, addr, {"t": "status", "state": "connected",
                                        "detail": "loading the relay's world…" if spath
                                                  else f"waiting for the leader ({leader_name()!r}) to press START GAME"})
        roster_changed()
        if late and not relay_only:   # a relay tells late joiners what it is doing itself (above)
            # A late joiner is NOT started: it has no save (a save start) and
            # nobody is in the lobby to sync with. Tell it why; the host has to
            # press START GAME again to bring it in.
            _send_data(sock, addr, {"t": "status", "state": "connected",
                                    "detail": "game already started -- ask the "
                                              "host to press START GAME again"})

    def relay_forward(payload):
        """Host side of the game relay: a local bridge frame -> every joiner.
        Returns how many joiners it was sent to."""
        frame = _pack_data(payload)
        n = 0
        for a in list(peers):
            try:
                sock.sendto(frame, a)
                n += 1
            except OSError:
                pass
        return n

    def handle_data(addr, payload):
        if payload[:1] == MESH_RELAY_MAGIC:
            # A relay envelope: for us -> deliver the inner frame; for another
            # joiner -> forward verbatim (the host is the default relay).
            if addr not in peers:
                return
            peers[addr]["last"] = time.time()
            env = _relay_unwrap(payload)
            if env is None:
                return
            to, _frm, inner = env
            if to == host_name:
                if GameRelay.is_game(inner) and relay is not None:
                    relay.deliver(inner)
                return
            for a, p in peers.items():
                if p["name"] == to:
                    # a mesh joiner unwraps envelopes itself; a legacy (star)
                    # joiner only understands plain frames
                    out = payload if p.get("mesh") else inner
                    try:
                        sock.sendto(_pack_data(out), a)
                    except OSError:
                        pass
                    return
            return
        if GameRelay.is_game(payload):
            # A joiner's bridge frame (binary, never JSON): straight to our
            # bridge. Only from a peer that has joined -- strays are dropped.
            if addr in peers:
                peers[addr]["last"] = time.time()
                if relay is not None:
                    relay.deliver(payload)
                if relay_only and peers[addr].get("mesh"):
                    # a mesh joiner sends a PLAIN frame only to "the host": in a
                    # relay lobby that means the leader (0.4.6 clients did this)
                    la = leader_addr()
                    if la is not None and la != addr:
                        try:
                            sock.sendto(_pack_data(payload), la)
                        except OSError:
                            pass
                    return
                if not peers[addr].get("mesh"):
                    # legacy star joiner: it cannot reach the others itself
                    frame = _pack_data(payload)
                    for a in list(peers):
                        if a != addr:
                            try:
                                sock.sendto(frame, a)
                            except OSError:
                                pass
            return
        if payload[:4] == CHUNK_MAGIC:
            # relay-only: a save chunk from the leader's upload
            if relay_only and upload[0] is not None and addr == leader_addr() and len(payload) >= 12:
                peers[addr]["last"] = time.time()
                sid, seq = struct.unpack("!II", payload[4:12])
                upload[0].on_chunk(sid, seq, payload[12:])
            return
        if payload[:1] == FRAG_MAGIC:
            # a piece of a big control message (a joiner's log batch, a long
            # mods_request, a resync ack): whole once the last piece lands
            if addr in peers:
                peers[addr]["last"] = time.time()
            payload = frags.feed(addr, payload)
            if payload is None:
                return
        try:
            msg = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        if not isinstance(msg, dict):
            return
        t = msg.get("t")
        if t != "join" and addr not in peers:
            return
        if addr in peers:
            peers[addr]["last"] = time.time()
        if recovery and addr in peers:
            if recovery.command(peers[addr]["name"], msg) or recovery.feedback(addr, msg):
                return
        if relay_only and t in ("fbegin", "start") and addr in peers:
            if addr != leader_addr():
                log(f"[relay] {t} from {peers[addr]['name']!r} ignored -- only the leader "
                    f"({leader_name()!r}) starts")
                return
            if t == "start":
                broadcast_start(save=False)
                return
            if transfer[0] is not None:
                log("[relay] upload refused -- still pushing the previous save")
                return
            u = upload[0]
            if u is not None and u.complete and u.sid == msg.get("sid"):
                return                                      # a late duplicate fbegin of a finished upload
            if u is None or (msg.get("kind")!="mods" and (u.complete or u.failed or u.sid != msg.get("sid"))):
                # the stored save is NOT cleared first: the receiver holds the new
                # one in memory and overwrites the files only once it has verified,
                # so a failed upload leaves the previous world intact for the next session
                upload[0] = _ClientSaveReceiver(_PeerConn(sock, addr), io, log, server_cache=os.path.join(io.dir,"mod_cache"))
                log(f"[relay] save upload from the leader {peers[addr]['name']!r} begins")
            upload[0].on_begin(msg)
            return
        if t == "join":
            do_join(addr, msg.get("name", "player"), msg.get("profile"),
                    msg.get("mesh", False), msg.get("version"), msg.get("recovery", 0))
            if addr in peers:
                peers[addr]["batches"] = bool(msg.get("batches"))   # a client from 0.6.1.7 on takes a mods round in batches
        elif t == "links":
            if addr in peers:
                new = [str(x) for x in msg.get("direct", [])][:CAP]
                if new != peers[addr].get("links"):
                    peers[addr]["links"] = new
                    send_roster_packets()          # let everyone re-plan relays
        elif t == "stage":
            # a joiner says what it is doing (hot-join progress); "" clears it
            if addr in peers:
                text = str(msg.get("text", ""))[:80]
                if peers[addr].get("stage", "") != text:
                    peers[addr]["stage"] = text
                    send_roster_packets()
                    emit_roster()
        elif t == "company":
            # a joiner may set ITS OWN company; the host sets anyone's -- and on
            # a relay the leader stands in for the host
            if addr in peers:
                target = str(msg.get("player") or peers[addr]["name"])
                allowed = target == peers[addr]["name"] or (relay_only and addr == leader_addr())
                if allowed and set_company(target, msg.get("id")):
                    log(f"[host] {target} -> company {msg.get('id')}" + ("" if target == peers[addr]["name"] else f" (set by the leader {peers[addr]['name']})"))
                    roster_changed()
                elif not allowed:
                    log(f"[host] {peers[addr]['name']} tried to set {target}'s company -- only the leader may")
        elif t == "mode":
            # the lobby's mode: on a relay the leader sets it; a host sets it locally (below)
            if addr in peers and relay_only and addr == leader_addr():
                if set_mode(str(msg.get("mode", ""))):
                    log(f"[host] mode -> {mode[0]} (set by the leader {peers[addr]['name']}); companies {roster_companies()}")
                    roster_changed()
        elif t == "mesh_hi":
            pass                                    # names are host-assigned
        elif t == "log":
            # Untrusted input from a peer, written to our disk: cap the volume
            # per peer (bytes per session, messages per second) and strip
            # control characters so a line cannot forge another peer's tag or
            # fill the drive.
            if addr in peers:
                p = peers[addr]
                nowt = time.time()
                if nowt - p.get("log_win", 0.0) >= 1.0:
                    p["log_win"], p["log_n"] = nowt, 0
                p["log_n"] = p.get("log_n", 0) + 1
                if p["log_n"] > LOG_MSGS_PER_SEC or p.get("log_bytes", 0) > LOG_BYTES_PER_PEER:
                    return
                lines = []
                for x in msg.get("lines", [])[:64]:
                    x = "".join(ch if ch >= " " else " " for ch in str(x))[:LOG_LINE_MAX]
                    lines.append(x)
                    p["log_bytes"] = p.get("log_bytes", 0) + len(x) + 1
                if lines:
                    peers_log.write(p["name"], lines)
        elif t == "chat":
            if addr in peers:
                text = str(msg.get("text", ""))
                if relay_only and text.strip().lower() == "/new":
                    # the leader discards the stored world before the session starts,
                    # and shares their own save with START GAME instead
                    if addr != leader_addr():
                        _send_data(sock, addr, {"t": "status", "state": "connected",
                                                "detail": f"only the leader ({leader_name()!r}) can start a new world"})
                    elif started[0]:
                        _send_data(sock, addr, {"t": "status", "state": "connected",
                                                "detail": "the session is running -- /new only works before it starts"})
                    elif transfer[0] is not None:
                        _send_data(sock, addr, {"t": "status", "state": "connected",
                                                "detail": "the relay's world is already on its way -- /new only works before it is sent"})
                    else:
                        pending_resume[0] = None
                        removed = 0
                        for sfx in (".sav", ".sav.lua", ".jpg"):
                            try:
                                os.remove(os.path.join(io.dir, INCOMING_BASENAME + sfx)); removed += 1
                            except OSError:
                                pass
                        log(f"[relay] /new by the leader: stored world forgotten ({removed} file(s)); waiting for START GAME")
                        for a in list(peers):
                            _send_data(sock, a, {"t": "status", "state": "connected",
                                                 "detail": "the relay's old world was discarded -- the leader's START GAME sends a fresh one"
                                                           if a == addr else
                                                           f"the relay's old world was discarded -- waiting for the leader ({leader_name()!r}) to press START GAME"})
                    return
                broadcast_chat(peers[addr]["name"], text)
        elif t == "ping":
            pass                                            # last-seen refreshed
        elif t == "leave":
            if addr in peers:
                log(f"[host] LEAVE {addr} ({peers[addr]['name']})")
                del peers[addr]
                if transfer[0] is not None:
                    transfer[0].on_peer_dropped(addr)
                roster_changed()
        # ---- reliable save-transfer feedback (receiver -> host) ---------- #
        elif t == "fbegin_ack":
            if transfer[0] is not None:
                transfer[0].on_begin_ack(addr, msg)
        elif t == "tcp_gave_up":
            if transfer[0] is not None:
                transfer[0].on_tcp_gave_up(addr, msg)
        elif t in ("fack", "fdone"):
            xf = transfer[0]
            if xf is not None:
                if addr not in xf.peers or msg.get("sid") != xf.sid:
                    key = (addr, msg.get("sid"))
                    if key not in unplaced_feedback:
                        unplaced_feedback.add(key)
                        log(f"[host] {t} from {peers.get(addr, {}).get('name', addr)} for sid {msg.get('sid')} cannot be placed: "
                            f"the transfer is sid {xf.sid} to {', '.join(q['name'] for q in xf.peers.values())}")
                elif t == "fack":
                    xf.on_fack(addr, msg)
                else:
                    xf.on_fdone(addr, msg)
        elif t == "mods_request":
            if transfer[0] is not None and addr in transfer[0].peers:
                # A joiner prompted BEFORE the save is in "preflight" mode, and
                # answers yes with mods_request -- never the mods_answer this
                # host is waiting for (see _ClientMods.answer_mods). Dropping it
                # outright meant the player pressed yes, the host waited the full
                # MODS_ANSWER_WAIT for a message that mode never sends, called it
                # a no and failed the save transfer (seen 2026-09-15). Count it as
                # the consent it is; the queued preflight request is still skipped
                # while a transfer to this peer is open.
                pr = transfer[0].peers.get(addr)
                if pr and pr.get("ask"):
                    transfer[0].on_mods_answer(addr, {"sid": transfer[0].sid, "accept": True})
                return
            if addr in peers:
                peers[addr]["batches"] = bool(msg.get("batches"))   # a client from 0.6.1.7 on takes a round in batches
            if pack_job[0] is not None and addr in pack_job[0]["addrs"]:
                return                                  # already packaging for this joiner: it repeats every second
            if any(addr in j["addrs"] for j in pack_queue):
                return
            allowed={modshare.mod_folder_name(m,v):(m,v) for m,v in (advertised[1] or [])}
            requested=msg.get("need",[])
            if isinstance(requested,list):          # as many as the save needs (a 128 cap until 2026-09-16)
                wanted=[allowed[n] for n in requested if isinstance(n,str) and n in allowed]
                unknown=[n for n in requested if not (isinstance(n,str) and n in allowed)]
                if addr not in preflight_requests:
                    # said once per request round (the joiner repeats it every second until answered)
                    shown=", ".join(str(n) for n in requested[:6]) + (f", +{len(requested)-6} more" if len(requested) > 6 else "")
                    log(f"[host] {peers[addr]['name']!r} asks for {len(requested)} mod(s), {len(wanted)} of them in this save's list of {len(allowed)}: {shown}")
                    if unknown:
                        log(f"[host] ... {len(unknown)} of those are not in the list this save advertised (a different save since the join?): "
                            + ", ".join(str(n) for n in unknown[:6]))
                if wanted:
                    preflight_requests[addr]=wanted
                elif requested:
                    _send_data(sock, addr, {"t": "status", "state": "connected",
                                            "detail": "the host's save no longer lists the mods you asked for -- rejoin to get the current list"})
            else:
                log(f"[host] mods_request from {peers[addr]['name']!r} carries no list -- ignored")
        elif t == "mods_answer":
            if transfer[0] is not None:
                transfer[0].on_mods_answer(addr, msg)

    # ---- save transfer: read the file(s), fan out reliably, THEN start ----- #
    last_shared = [None]                    # the save path last pushed (host: START GAME; relay: stored)

    def begin_save_transfer(save_path, include_leader=False):
        last_shared[0] = save_path
        """Kick off a reliable push of ``save_path`` (+ sidecars) to all peers.

        Runs entirely off the main loop: this only builds the transfer object;
        pump()/ACK-routing happen in the serve loop so pings/roster keep flowing.
        Once every peer has verified (or dropped), the loop broadcasts
        'start' with save=true. If any peer FAILED, or on a read error, we emit
        a failed status and do NOT start (START GAME can be pressed again).
        """
        if transfer[0] is not None:
            log("[host] start(save) ignored -- a transfer is already running")
            return
        if not peers:
            # Nobody to share the save with: do NOT latch 'started' (that would
            # turn every subsequent joiner into a 'late' one). Just say so.
            log("[host] start(save): no joiners connected -- not starting")
            io.emit({"type": "status", "state": "connected",
                     "detail": "no players to share with -- wait for a player "
                               "to join, then press START GAME"})
            return
        try:
            blob, files_meta = _read_save_files(save_path)
        except (OSError, ValueError) as e:
            io.emit({"type": "status", "state": "failed",
                     "detail": f"save transfer failed: {e}"})
            log(f"[host] save read failed: {e}")
            return
        sid = int(time.time() * 1000) & 0xFFFFFFFF
        if relay_only:
            la = leader_addr()
            targets = [(a, peers[a]["name"]) for a in peers
                       if (include_leader or a != la) and not peers[a].get("started")]
            if not targets:
                # nobody is waiting: the upload was a hot-join sync for peers that
                # have since left, or a plain re-start -- just start the leader
                if started[0]:
                    # the leader's periodic upload: everyone is already playing.
                    # Re-broadcasting START here made every game's panel flash a
                    # join/transfer every 2 min (2026-09-10). Just keep the copy.
                    log("[relay] save stored; nobody is waiting for it")
                    if upload[0] is not None and getattr(upload[0], "complete", False):
                        upload[0] = None
                    return
                log("[relay] save arrived but no peer is waiting for it -- starting")
                broadcast_start(save=True)
                return
        else:
            targets = [(a, peers[a]["name"]) for a in peers if not (started[0] and peers[a].get("started"))]
            if not targets:
                log("[host] start(save): everyone already has this save -- nothing to push")
                return
        mods = modshare.save_mod_list(save_path, log)        # None = could not read it (NOT "none")
        if not relay_only and not _host_mods_check(save_path, mods, io, log):
            return                                           # refused: the host's own game could not load it (status + chat name the mods)
        advertised[:]=[save_path,mods]
        if mods and not relay_only:
            # the host loads this save too: its own removed-item subscriptions need the rows as much
            modshare.set_registry_scope(m for m, v in mods if isinstance(m, str) and m.startswith("*"))
            rows = _workshop_rows(mods, modshare.find_mod)
            if rows:
                try:
                    modshare.write_registry(None, rows)
                    log(f"[host] registry: {len(rows)} Workshop folder(s) of this save published for the game's next refresh")
                except (OSError, ValueError) as e:
                    log(f"[host] could not publish the Workshop rows: {e}")
        for a,_ in targets: preflight_requests.pop(a,None)
        if mods is None:
            mod_list_note(save_path, "START GAME")
            broadcast_chat("MULTIPLAYER", f"The host could not read which mods {os.path.basename(save_path)} needs: "
                                          "you will not be offered a download. If your game refuses the save, install the host's mods by hand.")
        elif mods:
            log(f"[host] the save needs {len(mods)} mod(s) besides ours: "
                + ", ".join(modshare.mod_folder_name(m, v) for m, v in mods))
        transfer[0] = _HostSaveTransfer(sock, sid, blob, files_meta, targets,
                                        io, log, mods=mods, stage_cb=sender_stage)

    def begin_world_switch(save_path):
        """A WORLD SWITCH: the host loaded a different world while the session
        was running, so everybody has to leave the world they are in and load
        this one.

        Every peer goes back to 'unstarted' -- that is what makes
        begin_save_transfer push to ALL of them rather than only to the ones
        still waiting for a first save -- and is marked so broadcast_start
        tells it this is a switch. ``last_shared`` is pointed at the new file
        BEFORE any peer is unstarted: the serve-again loop fires on 'a peer is
        unstarted', so if it runs between here and the transfer it can only
        ever push THIS world, never the one everyone is leaving.

        The switch is a NEW WORLD for the bridges too (2026-09-19). Only a
        resync used to mint a world epoch; a switch kept the lobby's nonce, so
        the players still in the old world went on feeding their heartbeats
        into the host's new one: the host loaded an earlier save and read
        itself as 5,000 game units behind (actions off, every build dropped),
        and its load gate took the old-world heartbeats as "everyone is in".
        A fresh nonce goes to our own menu now and rides the next roster to
        every player: each bridge starts a new cohort and drops the old
        world's datagrams until their sender has moved too."""
        nonlocal transport_lobby
        transport_lobby = os.urandom(16).hex()
        io.emit(dict(type='transport_lobby', epoch=transport_lobby))
        last_shared[0] = save_path
        playing = sum(1 for p in peers.values() if p.get("started"))
        for p in peers.values():
            p["started"] = False
            p["switch"] = True
        name = os.path.basename(save_path)
        log(f"[host] world switch: pushing {name} to {len(peers)} player(s)"
            f" ({playing} of them already playing)")
        io.emit({"type": "status", "state": "connected",
                 "detail": f"Switching everyone to {name}\u2026"})
        send_roster_packets()                 # carries the new nonce: the players' bridges leave the old world now
        begin_save_transfer(save_path)

    # ---- local (host's own menu) commands ---------------------------------- #
    def handle_command(cmd):
        nonlocal host_name
        if recovery and recovery.command(host_name, cmd):
            return
        if recovery and recovery.held and cmd.get("cmd") in ("start", "name", "company"):
            return
        c = cmd.get("cmd")
        if c == "advertise_mods":
            path=str(cmd.get("save", ""))
            mods=modshare.save_mod_list(path, log)            # None = could not read it (NOT "none")
            if mods is None:
                mod_list_note(path, "advertise_mods")
            advertised[:]=[path,mods]
            for a in list(peers): _send_data(sock,a,{"t":"mods_manifest","mods":mods,"mods_unknown":mods is None})
            io.emit({"type":"mods_manifest","mods":mods,"mods_unknown":mods is None})
            return

        if c == "chat":
            broadcast_chat(host_name, str(cmd.get("text", "")))
        elif c == "name":
            host_name = _dedupe(str(cmd.get("name", "player")),
                                {p["name"] for p in peers.values()})
            if recovery:
                recovery.barrier.host = host_name
                recovery.runtime.player = host_name
            roster_changed()
        elif c == "company":
            target = str(cmd.get("player") or host_name)
            if set_company(target, cmd.get("id")):
                log(f"[host] {target} -> company {cmd.get('id')} (set by host)")
                roster_changed()
        elif c == "stage":
            # the host's own menu says what it is doing (loading the world it
            # picked, a world switch); "" clears it (2026-09-16)
            text = str(cmd.get("text", ""))[:80]
            if text != host_stage[0]:
                host_stage[0] = text
                send_roster_packets()
                emit_roster()
        elif c == "mode":
            if set_mode(str(cmd.get("mode", ""))):
                log(f"[host] mode -> {mode[0]} (set by host); companies {roster_companies()}")
                roster_changed()
        elif c == "crossplay":
            if not steam_code:
                log("[host] cross-play asked for, but this game has no Steam networking: the classic code is the only one")
                emit_code()
            else:
                set_crossplay(bool(cmd.get("on", True)))
                log("[host] CROSS-PLAY " + ("ON: the classic code is shown and listed" if xplay[0] else "OFF: the Steam ID is the code"))
        elif c == "publish":
            if publisher is not None:
                publisher.set(bool(cmd.get("on", True)))
                log(f"[host] public listing {'ON' if publisher.on else 'OFF'}")
            else:
                log("[host] publish requested but no --publish URL was given")
        elif c == "sync_taking":
            # The host's menu is taking a hot-join save for the newcomer(s). The
            # serve-again below must not push the save START GAME shared meanwhile
            # -- it did, within a second of the join, and the fresh autosave then
            # arrived to "a save transfer is in progress" (2026-09-16). Its start
            # lifts the hold; if the save never appears the hold expires.
            serve_hold[0] = time.time() + 120
            log("[host] the host is saving for a hot join -- holding the serve-again")
        elif c == "start":
            save = cmd.get("save")
            # "switch":true -- the host's menu saw it load ANOTHER world while
            # this session was running (see begin_world_switch). The flag rides
            # in the queued command, so a switch that has to wait for a running
            # transfer is still a switch when it runs.
            switch = bool(cmd.get("switch"))
            serve_hold[0] = 0.0
            if relay_only:
                log("[relay] 'start' from the local panel ignored -- the leader starts")
            elif transfer[0] is not None:
                if mod_preflight[0]:
                    pending_start[0] = dict(cmd)
                    io.emit({"type": "status", "state": "connected",
                             "detail": "Waiting for mod downloads before starting the game."})
                elif save:
                    # never drop the host's save: it is the world the game is in NOW
                    pending_start[0] = dict(cmd)
                    log("[host] start queued until the running save transfer ends"
                        + (" (world switch)" if switch else ""))
                else:
                    log("[host] start ignored -- a save transfer is in progress")
            elif save and not _mod_check(save, io, log):
                pass                              # refused: made without the mod (status + chat say so)
            elif save and switch and started[0] and peers:
                begin_world_switch(save)          # everyone leaves the world they are in
            elif save:
                begin_save_transfer(save)         # start(save=True) when done
            else:
                broadcast_start(save=False)       # legacy start, no transfer
        elif c == "quit":
            _STOPPING[0] = True               # a SIGTERM from here on must not cut the cleanup short
            stop.set()
        elif not relay_only:
            _report_command(cmd, io, log, publisher.url if publisher is not None else None)

    # ---- serve ------------------------------------------------------------- #
    io.emit({"type": "status", "state": "connected",
             "detail": f"lobby ready on {sock.getsockname()[1]}"})
    emit_roster()                                           # initial: just host
    log(f"[host] serving as {host_name!r} on udp/{sock.getsockname()[1]}")
    # the TCP listener the save/mod transfers stream over (bulk_tcp.py); one per process
    BULK[0] = bulk_tcp.BulkListener.open(sock.getsockname()[1], log) if BULK_TCP[0] else None
    if BULK[0] is not None and dual is not None:
        # a joiner's TCP backup link arrives on the same listener ("TPF2LINK1 <name> <sealed>")
        def link_hello(c, addr, line):
            name = _dual_hello_ok(line, SEAL[0])
            # the joiner dials the moment its UDP punch lands, often before its
            # `join` has been processed here: give the roster a few seconds
            found, why = None, "no joiner by that name"
            deadline = time.time() + 10.0
            while name and found is None and time.time() < deadline:
                found, why = _match_link_hello(peers, name, addr, dual.has_link)
                if found is None:
                    time.sleep(0.1)
            if found is None:
                log(f"[dual] TCP link hello from {addr[0]} ({name!r}) matches no joiner ({why}) -- closed")
                c.close()
                return
            dual.attach(c, found, "joiner connected")
        BULK[0].link_handler = link_hello

    last_heal = last_drop = 0.0
    last_serve_check = [0.0]
    punching = {}                           # (ip, port) a joiner knocked from -> punch until
    relay_binds = {}                        # (ip, port) of a master relay allocation -> (id, bind until)
    last_punch = [0.0]
    punch_token = os.urandom(TOKEN_LEN)     # nobody echoes it back to us; any token opens the NAT
    reject_sent = {}                        # addr -> when we last sent a plain reject
    # The game relay's loopback socket joins the select set so a bridge frame
    # wakes the loop immediately (lockstep latency) instead of on the next tick.
    rlist = [sock] if relay is None else [sock, relay.sock]
    try:
        while not stop.is_set():
            # While a transfer runs, poll fast so we pump chunks + absorb ACKs
            # promptly; otherwise idle at 0.2 s to keep the loop cheap.
            timeout = (XFER_SELECT_TIMEOUT if transfer[0] is not None or (recovery and recovery.transfer is not None)
                       else 0.05 if recovery and recovery.held else 0.2)
            try:
                ready, _, _ = select.select(rlist, [], [], timeout)
            except (OSError, ValueError):
                break
            now = time.time()

            # Drain up to HOST_DRAIN datagrams this cycle -- a busy transfer can
            # deliver a burst of facks/pings, and one-per-iteration would let the
            # kernel recv buffer overflow (self-inflicted loss).
            if sock in ready:
                for _ in range(HOST_DRAIN):
                    try:
                        data, addr = sock.recvfrom(65535)
                    except BlockingIOError:
                        break                               # nothing left to read
                    except (ConnectionResetError, OSError):
                        break
                    if not data:
                        break
                    ptype, payload = _unpack(data)
                    if ptype == TYPE_HELLO and not xplay[0] and not steamtunnel.is_tunnel_addr(addr) \
                            and addr not in peers:
                        # CROSS-PLAY OFF: only a Steam tunnel endpoint gets a handshake
                        if now - reject_sent.get(addr, 0.0) >= REJECT_PLAIN_EVERY:
                            reject_sent[addr] = now
                            log(f"[host] {addr[0]}:{addr[1]} knocked outside Steam while cross-play is off -- not answered")
                    elif ptype == TYPE_HELLO:
                        # Complete the joiner's handshake: echo THEIR token.
                        try:
                            sock.sendto(_pack(TYPE_ACK, payload), addr)
                        except OSError:
                            pass
                    elif ptype in (TYPE_ACK, TYPE_CONNECTED):
                        pass                                # informational
                    elif ptype == TYPE_KEYX:
                        # A Steam-code joiner asks for the session secret (steamkey.py). Only
                        # over the tunnel, at most every ANSWER_EVERY s per endpoint; the same
                        # offer gets the same answer, so a lost reply costs only a resend.
                        if steam_secret is None or not steamtunnel.is_tunnel_addr(addr):
                            continue
                        last = keyx_last.get(addr)
                        if last and last[0] == payload:
                            ans = last[1]
                        elif last and now - last[2] < steamkey.ANSWER_EVERY:
                            continue
                        else:
                            ans = steamkey.answer(payload, steam_secret)
                            if ans is None:
                                continue
                            keyx_last[addr] = (payload, ans, now)
                            log(f"[steam] {addr[0]}:{addr[1]} asked for the session key -- answered over Steam")
                        try:
                            sock.sendto(_pack(TYPE_KEYX, ans), addr)
                        except OSError:
                            pass
                    elif ptype == TYPE_KEEPALIVE:
                        if addr in peers:
                            peers[addr]["last"] = now
                    elif ptype == TYPE_ADATA:
                        # Authenticated, not encrypted (bulk save chunks).
                        # Only trusted in a sealed session: a plaintext session
                        # has no key to verify with, so an unauthenticated bulk
                        # frame is never accepted.
                        #
                        # The dispatch to handle_data is NOT optional. This
                        # branch originally verified the payload and then fell
                        # out of the if/elif chain, so a valid signed frame was
                        # silently dropped. Harmless while only the host sends
                        # bulk, but it would quietly swallow joiner-side bulk
                        # traffic (e.g. peer-to-peer save fanout over the mesh).
                        plain = SEAL[0].unsign(payload) if SEAL[0] is not None else None
                        if plain is not None:
                            handle_data(addr, plain)
                    elif ptype == TYPE_EDATA:
                        if SEAL[0] is None:
                            continue                        # we run plaintext
                        plain = SEAL[0].open(payload)
                        if plain is not None:
                            handle_data(addr, plain)
                        elif addr not in peers and now - reject_sent.get(addr, 0.0) >= REJECT_PLAIN_EVERY:
                            # wrong password (or wrong code): say so in the clear
                            reject_sent[addr] = now
                            try:
                                sock.sendto(_pack(TYPE_DATA, json.dumps(
                                    {"t": "reject", "reason": "wrong password"}).encode("utf-8")), addr)
                            except OSError:
                                pass
                    elif ptype == TYPE_DATA:
                        if SEAL[0] is None:
                            handle_data(addr, payload)      # plaintext session
                        elif relay_only and addr in peers and payload[:4] == CHUNK_MAGIC:
                            # the leader's upload: bulk chunks ride in the clear (BULK_PLAIN),
                            # exactly the rule a joiner applies to the host's chunks
                            handle_data(addr, payload)

            # Hole punching: a joiner knocked through the master, so fire HELLOs
            # at its address; they open our NAT for the joiner's own HELLOs, which
            # the branch above ACKs. A joiner that has connected needs no more.
            if punch_q is not None:
                try:
                    while True:
                        for t in punch_q.get_nowait():
                            if len(t) >= 3:
                                # the master's relay port for a joiner: bind to it
                                # (TRLB|id|H) until that joiner's frames come through it
                                relay_binds[(str(t[0]), int(t[1]))] = (bytes(t[2]), now + RV_PUNCH_FOR)
                            else:
                                punching[(str(t[0]), int(t[1]))] = now + RV_PUNCH_FOR
                except queue.Empty:
                    pass
                if (punching or relay_binds) and now - last_punch[0] >= RV_PUNCH_EVERY:
                    last_punch[0] = now
                    for a in list(punching):
                        if punching[a] < now or a in peers:
                            del punching[a]
                            continue
                        try:
                            sock.sendto(_pack(TYPE_HELLO, punch_token), a)
                        except OSError:
                            pass
                    for a in list(relay_binds):
                        aid, until = relay_binds[a]
                        if until < now or a in peers:
                            del relay_binds[a]
                            continue
                        try:
                            sock.sendto(RELAY_MAGIC + aid + b"H", a)
                        except OSError:
                            pass

            # Game relay: local bridge frames -> every joiner.
            if relay is not None and relay.sock in ready:
                relay.pump_outbound(relay_forward)

            if now - last_drop >= 1.0:
                last_drop = now
                frags.expire(now)
                dead = _keepalive_sweep(peers, now, drop_after,
                                        (transfer[0], recovery.transfer if recovery else None), log,
                                        exempt=set(pack_job[0]["addrs"]) if pack_job[0] else ())
                for a in dead:
                    log(f"[host] DROP {a} ({peers[a]['name']}) -- silent")
                    del peers[a]
                    frags.forget(a)
                    if transfer[0] is not None:
                        transfer[0].on_peer_dropped(a)     # skip it, keep going
                if dead:
                    roster_changed()

            # After a resync the members run in the operation's epoch, not in
            # this lobby's nonce. Advertise THAT from now on (welcome, roster):
            # a player who joins later must start in the world the others are
            # in. Their bridges take a nonce that names their current world as
            # a rename, not a reset (net.cpp, Net_BeginLobby). Checked BEFORE
            # the heal below so the old and the new nonce never go out back to
            # back (reordered, the old one would reset a member's bridge).
            # The late joiner is also served the resync snapshot from now on,
            # not the save START GAME shared: that world was left behind.
            if recovery:
                world = recovery.world_epoch()
                if world and world != resync_world[0]:   # not transport_lobby: a world switch after the resync minted its own nonce
                    resync_world[0] = world
                    transport_lobby = world
                    last_heal = now
                    snapshot = recovery.runtime.save_directory / ('mp_' + world[:12] + '.sav')
                    if snapshot.is_file():
                        last_shared[0] = str(snapshot)
                    log(f"[host] transport lobby follows the completed resync ({world[:8]}..); late joiners get {os.path.basename(last_shared[0] or '')}")
                    # every member loaded that world: a newcomer brought in by a
                    # frozen join is started now (no START GAME push for it)
                    for p in peers.values():
                        p["started"] = True
                        p["stage"] = ""
                    # The barrier has verified every loaded world. Legacy menu
                    # stage watchers are bypassed by frozen joins, so Windows
                    # 0.6.1.18 can otherwise leave "receiving save 100%" forever
                    # and block company changes on every participant.
                    host_stage[0] = ""
                    started[0] = True
                    io.emit(dict(type='transport_lobby', epoch=transport_lobby))
                    send_roster_packets()
                    emit_roster()

            if now - last_heal >= ROSTER_HEAL:
                last_heal = now
                send_roster_packets()

            for cmd in io.poll_commands():
                handle_command(cmd)
            if recovery:
                recovery.tick(now)
                # DEDICATED HOST (2026-09-18): a member who joined BEFORE the host's world
                # was up sat in the lobby for ever -- nobody presses START GAME on a
                # dedicated server. Once the world is up, each such member is brought in
                # through the frozen-join round exactly as a late joiner would be.
                # LIVE JOIN (2026-09-22, user: "dedicated servers shouldn't do it either"):
                # no round -- the host's menu is asked for its hot-join save (the file
                # /sync writes, menu SyncPoll), shared to every unstarted member like
                # START GAME; they load it and catch up while the session runs on.
                if (not relay_only and not started[0] and transfer[0] is None and host_has_world()
                        and _live_join_on(io.dir)):
                    waiting = [p for p in peers.values()
                               if not p.get("started") and not p.get("live_join") and p.get("recovery") == 4]
                    if waiting:
                        for p in waiting:
                            p["live_join"] = True
                        try:
                            with open(os.path.join(str(recovery.runtime.directory), "tpf2_sync_save.txt"), "w") as f:
                                f.write("live join\n")
                            log(f"[host] live join for {', '.join(repr(p['name']) for p in waiting)} (waiting for the host's "
                                "world): the host's menu takes its hot-join save; nobody holds")
                        except OSError as e:
                            log(f"[host] live join: could not ask the menu for a save: {e}")
                elif not relay_only and not started[0] and transfer[0] is None and host_has_world():
                    for a, p in list(peers.items()):
                        if not p.get("started") and not p.get("frozen_join") and not p.get("live_join") and p.get("recovery") == 4:
                            if recovery.join(p["name"]):
                                p["frozen_join"] = True
                                log(f"[host] frozen join for {p['name']!r} (it was waiting for the host's world): holding the session, "
                                    + ("the players in keep their worlds, the newcomer loads (live join)" if recovery.barrier.retain
                                       else "everyone loads the shared world"))

            if relay is not None:
                relay.tick(now)                             # 10 s stats line
            if dual is not None and now - last_dual_tick[0] >= 0.5:
                last_dual_tick[0] = now
                dual.tick({a: p["name"] for a, p in peers.items()})

            own_fwd.poll_tails()
            own_lines = own_fwd.drain(now)
            if own_lines:
                peers_log.write(host_name, own_lines)

            # relay-only: the stored world goes out (after RESUME_GRACE, so players arriving together share one transfer)
            if relay_only and pending_resume[0] is not None and now >= pending_resume[0][1]:
                la, _ = pending_resume[0]
                pending_resume[0] = None
                spath, age = stored_save()
                if la in peers and la == leader_addr() and spath and not started[0] and transfer[0] is None and upload[0] is None:
                    log(f"[relay] resuming the stored world ({int(age)} s old) for {peers[la]['name']!r}")
                    session_epoch[0] = time.time()          # this copy IS the session from here on
                    begin_save_transfer(spath, include_leader=True)
            # Anyone who joined during a transfer is still unstarted: serve them
            # from the same save now that the pipe is free (relay: its stored
            # world; host: the file START GAME shared). One push per batch.
            if not (recovery and recovery.held) and started[0] and transfer[0] is None and upload[0] is None and last_shared[0] and now - last_serve_check[0] >= 1.0 and now >= serve_hold[0]:
                last_serve_check[0] = now
                waiting = [a for a in peers if not peers[a].get("started")]
                fresh = (not relay_only) or (0 <= stored_age() <= HOTJOIN_STORED_MAX)
                if waiting and fresh and os.path.isfile(last_shared[0]) and (not relay_only or leader_addr() not in waiting):
                    log(f"[host] {len(waiting)} peer(s) waiting for the save -- pushing it again")
                    begin_save_transfer(last_shared[0])
            # relay-only: the leader's upload
            if relay_only and upload[0] is not None:
                u = upload[0]
                try:
                    u.tick(now)
                except Exception as e:                     # noqa: BLE001
                    log(f"[relay] upload error: {e!r}")
                    upload[0] = None
                    u = None
                if u is not None and not u.failed and not u.complete and u.last_pct >= 0:
                    # tell everyone who is waiting how the leader's upload is going
                    if u.last_pct // 10 != getattr(u, "told_pct", -1) // 10:
                        u.told_pct = u.last_pct
                        for a, p2 in list(peers.items()):
                            if not p2.get("started") and a != leader_addr():
                                _send_data(sock, a, {"t": "status", "state": "connected",
                                                     "detail": f"the leader is uploading the world to the relay\u2026 {u.last_pct}%"})
                if u is not None and u.failed:
                    log("[relay] the leader's upload failed -- waiting for a new START")
                    upload[0] = None
                elif u is not None and u.complete and transfer[0] is None and not getattr(u, "handed", False) \
                        and (u.mods_satisfied or now - getattr(u, "complete_at", now) >= RELAY_MODS_GRACE):
                    # The relay runs no game, so it "lacks" every mod a save
                    # names; a leader that shares mods follows the save with a
                    # mods round into the relay's cache (mods_satisfied), a leader
                    # that cannot (share_mods=never, a mod it does not have) never
                    # does. Waiting for the round left every such upload unhanded,
                    # and every joiner arriving after the leader's first periodic
                    # upload waited for a save that never came (2026-09-18, three
                    # players in a row). Handing off at once instead raced a
                    # leader that DOES send mods: its round hit "still pushing the
                    # previous save" (relay_mod_download_test). So: the round gets
                    # RELAY_MODS_GRACE seconds to begin, then the world goes out
                    # and the joiners settle their mods with the leader.
                    u.handed = True
                    path = os.path.join(io.dir, INCOMING_BASENAME + ".sav")
                    log(f"[relay] upload complete -> pushing {path} to the waiting peers"
                        + ("" if u.mods_satisfied else f" (the save names {len(u.need)} mod(s) the relay does not hold; the joiners sort those out)"))
                    begin_save_transfer(path)
            if transfer[0] is None and pending_start[0] is not None:
                queued, pending_start[0] = pending_start[0], None
                handle_command(queued)
            # MOD ROUNDS run on a worker thread, in BATCHES. Zipping ran in this
            # loop until 2026-09-18, as ONE blob: a joiner asking for 314 of a
            # save's 486 Workshop mods (~15 GB) left the host deaf for as long
            # as the zips took, would have held every byte in RAM, and the host
            # dropped it as silent meanwhile. A job now plans batches of about
            # MODS_BATCH_BYTES on disk, zips them MODS_PACK_THREADS at a time (at
            # most two finished ones waiting for the sender) and each goes out as
            # its own mods transfer with "batch": [k, n] in its fbegin; the joiner
            # keeps the round open until the last one lands. A joiner from before
            # batches (no "batches" in its mods_request) gets the whole set as one
            # blob. The batches go out in plan order whatever thread finishes first.
            def start_pack_job(kind, addrs, wanted, got=None):
                addrs = [x for x in addrs if x in peers]
                if not addrs or not wanted:
                    return
                batched = all(peers[x].get("batches") for x in addrs)
                names = ", ".join(peers[x]["name"] for x in addrs)
                job = {"kind": kind, "addrs": addrs, "names": names, "wanted": list(wanted), "got": got,
                       "batched": batched, "plan": None, "ready": [], "taken": 0, "done": 0,
                       "started": now, "told": 0.0, "finished": False, "abort": False, "bytes": 0}
                if pack_job[0] is not None:
                    pack_queue.append(job)
                    log(f"[host] mod round for {names} queued behind the one for {pack_job[0]['names']}")
                    return
                shown = ", ".join(modshare.mod_folder_name(m, v) for m, v in wanted[:8]) + (f", +{len(wanted)-8} more" if len(wanted) > 8 else "")
                log(f"[host] packaging {len(wanted)} mod(s) for {names}{'' if batched else ' as one blob (a client from before batches)'}: {shown}")

                def _pack_mods(job=job):
                    sizes = []
                    for m, v in job["wanted"]:
                        f = modshare.find_mod(m, v)
                        sizes.append(modshare.folder_bytes(f) if f else 0)
                    job["bytes"] = sum(sizes)
                    plan, cur, cur_bytes = [], [], 0
                    if job["batched"]:
                        # smallest first: the small mods fill a batch together, the big ones go alone
                        for (m, v), sz in sorted(zip(job["wanted"], sizes), key=lambda t: t[1]):
                            if cur and cur_bytes + sz > MODS_BATCH_BYTES:
                                plan.append(cur)
                                cur, cur_bytes = [], 0
                            cur.append((m, v))
                            cur_bytes += sz
                        if cur:
                            plan.append(cur)
                    else:
                        plan = [list(job["wanted"])]
                    job["plan"] = plan
                    size_of = dict(zip(job["wanted"], sizes))
                    job["sizes"] = [size_of.get(mv, 0) for group in plan for mv in group]   # plan order, for the status
                    job["batch_bytes"] = [sum(size_of.get(mv, 0) for mv in group) for group in plan]
                    log(f"[host] mod round for {job['names']}: {len(job['wanted'])} mod(s), {job['bytes'] / (1024.0 ** 3):.2f} GB on disk, "
                        f"{len(plan)} batch(es), {MODS_PACK_THREADS} packer(s) at deflate level {MODS_ZIP_LEVEL}")

                    def pack_group(group):
                        blob, meta, missing = bytearray(), [], []
                        for m, v in group:
                            if job["abort"]:
                                break
                            try:
                                data = mod_package(m, v)
                            except Exception as e:                          # noqa: BLE001
                                log(f"[host] packaging {modshare.mod_folder_name(m, v)} failed: {e!r}")
                                data = None
                            if data is None:
                                # say WHICH and WHY: this rejection logged nothing and named
                                # nothing, and a host with its Workshop mods in another
                                # Steam library looked exactly like one that refused (2026-09-18)
                                why = ("DLC, never transferred" if modshare.is_dlc(m)
                                       else "mod sharing is off here (share_mods=never)" if not SHARE_MODS[0]
                                       else "not installed here; looked in " + ", ".join(
                                           [d for d in modshare.workshop_dirs()] + [modshare.managed_workshop()]) if m.startswith("*")
                                       else "not installed here (game mods folder, userdata mods)")
                                missing.append((modshare.mod_folder_name(m, v), why))
                            else:
                                meta.append({"name": modshare.mod_zip_name(m, v), "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
                                blob += data
                            job["done"] += 1
                        return blob, meta, missing

                    # MODS_PACK_THREADS packers take the plan's groups in order; a finished
                    # group waits in `slots` until every earlier one is done, then moves to
                    # `ready` -- the sender only ever sees the plan order. A packer starts a
                    # new group only while fewer than two finished batches wait for the sender.
                    lock = threading.Lock()
                    slots, next_group, moved = {}, [0], [0]

                    def packer():
                        while not job["abort"]:
                            with lock:
                                gi = next_group[0]
                                if gi >= len(plan):
                                    return
                                waiting = len(job["ready"]) - job["taken"] + len(slots)
                                if waiting >= 2:
                                    gi = None
                                else:
                                    next_group[0] = gi + 1
                            if gi is None:
                                time.sleep(0.2)
                                continue
                            result = pack_group(plan[gi])
                            with lock:
                                slots[gi] = result
                                while moved[0] in slots:
                                    job["ready"].append(slots.pop(moved[0]))
                                    moved[0] += 1

                    threads = [threading.Thread(target=packer, name=f"mod-pack-{i}", daemon=True) for i in range(max(1, MODS_PACK_THREADS))]
                    for t in threads:
                        t.start()
                    for t in threads:
                        t.join()
                    job["finished"] = True

                pack_job[0] = job
                threading.Thread(target=_pack_mods, name="mod-pack", daemon=True).start()

            if pack_job[0] is None and pack_queue:
                nxt = pack_queue.pop(0)
                start_pack_job(nxt["kind"], nxt["addrs"], nxt["wanted"], got=nxt["got"])
            if transfer[0] is None and pack_job[0] is None and preflight_requests:
                a, wanted = preflight_requests.popitem()
                if a in peers and wanted:
                    start_pack_job("preflight", [a], wanted)
            if pack_job[0] is not None:
                job = pack_job[0]
                live = [x for x in job["addrs"] if x in peers]
                if not live:
                    job["abort"] = True
                    pack_job[0] = None
                    log(f"[host] mod round for {job['names']} abandoned -- they left")
                    if job["kind"] == "round" and job["got"] is not None:
                        rest = set(job["got"]) - set(job["addrs"])
                        if rest:
                            broadcast_start(save=True, only=rest)
                elif transfer[0] is None and job["taken"] < len(job["ready"]):
                    blob, meta, missing = job["ready"][job["taken"]]
                    # the transfer owns the blob now: drop the job's reference, or the round's
                    # every batch stays in RAM until the lobby exits (27 GB after a 140-batch
                    # round on 2026-09-20 -- the commit charge it took pushed Big Maps'
                    # terrain pager into a compress/expand flip-flop, and the game stuttered)
                    job["ready"][job["taken"]] = None
                    job["taken"] += 1
                    n, k = len(job["plan"]), job["taken"]
                    if missing:
                        names = ", ".join(nm for nm, _ in missing)
                        for nm, why in missing:
                            log(f"[host] cannot supply {nm} to {job['names']}: {why}")
                        for x in live:
                            _send_data(sock, x, {"t": "reject", "reason": f"The host cannot supply {names}: {missing[0][1]}."})
                            del peers[x]
                        roster_changed()
                        job["abort"] = True
                        pack_job[0] = None
                        if job["kind"] == "round" and job["got"] is not None:
                            rest = set(job["got"]) - set(job["addrs"])
                            log(f"[host] mods round failed for {job['names']} -- starting the other {len(rest)} peer(s)")
                            if rest:
                                broadcast_start(save=True, only=rest)
                    else:
                        mb = len(blob) / (1024.0 * 1024.0)
                        final = k == n
                        log(f"[host] sending mod batch {k}/{n} ({len(meta)} mod(s), {mb:.1f} MB) to {job['names']}")
                        mod_preflight[0] = job["kind"] == "preflight"
                        mod_round[0] = set(job["got"]) if (job["kind"] == "round" and final and job["got"] is not None) else None
                        targets = [(x, peers[x]["name"]) for x in live]
                        transfer[0] = _HostSaveTransfer(sock, int.from_bytes(os.urandom(4), "big"), blob, meta, targets, io, log, kind="mods",
                                                        extra={"batch": [k, n]} if job["batched"] else None)
                elif job["finished"] and job["taken"] >= len(job["ready"]):
                    took = now - job["started"]
                    gb = job["bytes"] / (1024.0 ** 3)
                    log(f"[host] mod round for {job['names']}: {job['done']} mod(s), {gb:.2f} GB on disk, {len(job['plan'] or [])} batch(es), packaged in {took:.0f} s")
                    pack_job[0] = None
                elif now - job["told"] >= 2.0:
                    job["told"] = now
                    n = len(job["plan"]) if job["plan"] else 0
                    in_flight = transfer[0] is not None and transfer[0].kind == "mods"
                    detail = _mods_progress_text(job, in_flight, now)
                    for x in live:
                        _send_data(sock, x, {"t": "status", "state": "connected", "detail": detail})
                    # the host waits too: its panel shows the same pace and time left
                    io.emit({"type": "status", "state": "connected", "detail": f"{job['names']}: {detail}"})
            # Pump the save transfer (if any). Once every peer has resolved:
            #   all done (dropped peers don't block) -> start with save=true;
            #   any FAILED -> failed status naming them, NO start, and the
            #   transfer is cleared so START GAME can simply be pressed again.
            if transfer[0] is not None:
                try:
                    transfer[0].pump(now)
                    if transfer[0].all_resolved() and transfer[0].kind == "mods":
                        # Only recipients with a successful catalogue receipt may start.
                        xfer, transfer[0] = transfer[0], None
                        if pack_job[0] is not None and pack_job[0].get("batch_bytes") and xfer.done_count():
                            j = pack_job[0]
                            j.setdefault("landed", []).append((now, j["batch_bytes"][min(j["taken"], len(j["batch_bytes"])) - 1]))
                        got = set(mod_round[0] or set())
                        was_preflight,mod_preflight[0]=mod_preflight[0],False
                        mod_round[0] = None
                        bad = xfer.failed_names()
                        if bad:
                            for a,p in list(xfer.peers.items()):
                                if p["state"] != "done" and a in peers:
                                    _send_data(sock,a,{"t":"reject","reason":"Required mod download failed."})
                                    del peers[a]
                            roster_changed()
                            log(f"[host] mod transfer failed for {', '.join(bad)} -- they will not be started")
                            broadcast_chat("MULTIPLAYER", "Mod transfer failed for " + ", ".join(bad)
                                           + " -- they may be missing mods this save needs.")
                        log(f"[host] mods shared -- starting {len(got)} peer(s)")
                        got -= {a for a,p in xfer.peers.items() if p["state"] != "done"}
                        if got and not was_preflight: broadcast_start(save=True, only=got)
                    elif transfer[0].all_resolved() and not transfer[0].awaiting_answers(now):
                        xfer, transfer[0] = transfer[0], None
                        failed = xfer.failed_names()
                        if failed:
                            detail = (f"save transfer failed for "
                                      f"{', '.join(failed)} -- press START "
                                      f"GAME to retry")
                            log(f"[host] {detail}")
                            io.emit({"type": "status", "state": "failed",
                                     "detail": detail})
                        elif xfer.done_count() == 0:
                            # Every target dropped mid-transfer: nobody holds
                            # the save, so (as with zero joiners) don't start.
                            log("[host] every peer dropped mid-transfer -- "
                                "not starting")
                            io.emit({"type": "status", "state": "connected",
                                     "detail": "no players to share with -- "
                                               "wait for a player to join, "
                                               "then press START GAME"})
                        else:
                            got = set(xfer.done_addrs())
                            la = leader_addr() if relay_only else None
                            if la is not None and upload[0] is not None and getattr(upload[0], "complete", False):
                                got.add(la)                   # the uploader has the save it sent
                            waiting = [a for a in peers if a not in got and not peers[a].get("started")]
                            needs = xfer.mod_needs()
                            if needs:
                                # MODS ROUND: every mod somebody lacks goes to those peers, in
                                # batches on the worker (start_pack_job above); everyone
                                # starts once the last batch resolves (mod_round).
                                wanted = {}
                                for lst in needs.values():
                                    for m, v in lst:
                                        wanted[(m, v)] = True
                                start_pack_job("round", list(needs.keys()), list(wanted), got=got)
                            if transfer[0] is None and pack_job[0] is None and not pack_queue:   # no mods round started: start now
                                log(f"[host] all save transfers resolved -- starting {len(got)} peer(s)"
                                    + (f"; {len(waiting)} joined during the transfer and will be served next" if waiting else ""))
                                broadcast_start(save=True, only=got)
                except Exception as e:                     # never crash the lobby
                    log(f"[host] save transfer error: {e!r}")
                    io.emit({"type": "status", "state": "failed",
                             "detail": f"save transfer failed: {e}"})
                    transfer[0] = None
    except KeyboardInterrupt:
        pass
    finally:
        _STOPPING[0] = True
        try:
            _log_sinks.remove(own_fwd.add)
        except ValueError:
            pass
        if BULK[0] is not None:
            BULK[0].close()
            BULK[0] = None
        if dual is not None:
            dual.close_links()
            DUAL[0] = None
        for a in list(peers):
            _send_data(sock, a, {"t": "bye"})
        io.emit({"type": "status", "state": "failed",
                 "detail": "lobby closed"})
        io.write_state(state="failed")
        if relay is not None:
            relay.close()
        try:
            sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- #
# CLIENT: one Connection to the host; participate
# --------------------------------------------------------------------------- #
def run_client(conn, my_name, io, stop=None, host_gone_after=HOST_GONE_AFTER,
               log=_log, receiver_cls=_ClientSaveReceiver, relay=None,
               mesh=None, profile_code=None, forward_logs=(), sync_runtime=None):
    """Participate in the lobby over a connected ``punch.Connection`` (blocks).

    ``receiver_cls`` is the save-receive implementation (the self-test swaps in
    a deliberately corrupting one to prove the failure path). ``relay`` is an
    optional :class:`GameRelay`: local bridge frames go to the host only, the
    host's 'g' frames go to the local bridge.
    """
    stop = stop or threading.Event()

    desired = [my_name]     # what we asked to be called
    assigned = [my_name]    # what the host actually named us (from 'welcome')
    seen_nonces = set()     # transport lobby nonces already handed to the menu
    started = [False]
    last_roster = [None]
    host_name = [None]
    version_checked = [False]
    participants = [[]]     # every player name, from the roster
    roster_links = [{}]     # name -> [names it has direct links to]
    roster_profiles = [{}]  # name -> profile code (how to punch it)
    last_hi = {}            # addr -> when we last sent mesh_hi on it
    last_links_report = [0.0]
    reported_links = [None]
    last_ignored_start = [None]     # (save, sid) of the last start we refused
    last_switch = [None]            # ("applied"/"ignored", receiver sid) of the last world switch
    is_relay = [False]              # the host is a relay-only server (roster/welcome say so)
    uploader = [None]               # our save going UP to the relay (we are the leader)
    uploaded = [False]              # an upload completed this session -> a start(save) is ours
    seen_cids = collections.deque(maxlen=512)
    seen_set = set()

    try:
        _boost_socket_buffers(conn.sock)    # help bursty save-transfer traffic
    except AttributeError:
        pass
    _clear_stale_incoming(io.dir, log)      # never trust a previous session's save
    receiver = receiver_cls(conn, io, log)  # save-transfer receive side
    receiver.my_name = my_name              # said in the TCP hello so the host matches the stream
    fwd = LogForwarder(forward_logs)        # our log lines -> the host's merged log
    _log_sinks.append(fwd.add)

    io.emit({"type": "status", "state": "connecting",
             # no address in the panel: a joiner's screen (or a stream of it)
             # must not show the host's IP. The redacted log still has it.
             "detail": "checking multiplayer version..."})
    io.write_state(state="connecting", you=desired[0], started=False)

    frags = _Reassembler(log)               # big control messages from the host (roster, fbegin, sync_state)

    def send(msg):
        payload = json.dumps(msg).encode("utf-8")
        try:
            for piece in _fragments(payload):
                conn.send(piece)
        except (RuntimeError, OSError) as e:
            _log_send_failure(getattr(conn, "peer", None), len(payload), e, log)

    recovery = ClientRecovery(sync_runtime, io, send, receiver) if sync_runtime is not None else None

    def apply_start(save, via, switch=False):
        """The save-flag rule: emit start only if save==false, or save==true
        AND our receiver completed this session (it emitted save_ready). An
        unsatisfiable start is ignored WITHOUT latching started, so a retried
        START GAME (after the host re-sends the save) still works.

        ``switch`` marks a WORLD SWITCH -- apply_switch_start has already
        cleared ``started`` for it, and the menu needs to know this start
        replaces a world it is playing rather than starting a first one."""
        if started[0] or (recovery and recovery.held) or receiver.cancelled or receiver.failed or receiver.ask or receiver.catalogue_token or not receiver.mods_satisfied:
            return
        if recovery and recovery.completed:
            # a frozen join: this game loaded the shared world through the
            # recovery round and is playing it -- the roster's started:true is
            # already true of us, not a save to load
            started[0] = True
            io.write_state(started=True)
            log(f"[client] START via {via} taken as already started -- this game joined through a world sync")
            return
        if receiver.need and not receiver.complete:
            return
        save = bool(save)
        if save and not receiver.complete and not uploaded[0]:
            key = (save, receiver.sid)
            if last_ignored_start[0] != key:      # log once per situation
                last_ignored_start[0] = key
                log(f"[client] start(save=True) via {via} ignored -- no "
                    f"verified save this session (receiver "
                    f"{'failed' if receiver.failed else 'incomplete'})")
            return
        started[0] = True
        event = {"type": "start", "save": save}
        if switch:
            event["switch"] = True
        io.emit(event)
        io.write_state(started=True)
        log(f"[client] START (save={save}{', world switch' if switch else ''}) via {via}")

    def apply_switch_start(save):
        """A WORLD SWITCH start: the host loaded another world and pushed it
        here. This is the one start that is taken although we already started --
        we are leaving the world we are playing for the save that just arrived.

        Guarded by the receiver's session id, because the host sends the start
        CHAT_BURST times: without it every copy would emit another load. A
        switch for which nothing was received is ignored, logged once."""
        sid = receiver.sid
        if (recovery and recovery.held) or receiver.cancelled or receiver.failed or receiver.ask \
                or receiver.catalogue_token or not receiver.mods_satisfied:
            return
        if not bool(save) or not receiver.complete or sid is None:
            if last_switch[0] != ("ignored", sid):
                last_switch[0] = ("ignored", sid)
                log("[client] start(switch) ignored -- no verified save arrived this session")
            return
        if last_switch[0] == ("applied", sid):
            return                                  # another copy of the start burst
        last_switch[0] = ("applied", sid)
        started[0] = False                          # this start replaces the world we are in
        send({"t": "stage", "text": "loading the host's new world"})
        apply_start(True, via="start (world switch)", switch=True)

    # ---- mesh: direct links to the other joiners, relay for the rest -------- #
    def mesh_plan_dials():
        """Dial every other joiner we have a profile for and no link to yet."""
        if mesh is None:
            return
        me = assigned[0]
        for nm, code in roster_profiles[0].items():
            if nm == me or nm == host_name[0] or not code:
                continue
            if mesh.by_name(nm) or mesh.dial_pending(nm) or mesh.dial_failed(nm):
                continue
            try:
                prof = decode_code(code)
            except ValueError:
                continue
            mesh.dial(nm, _targets_v4(prof))

    def mesh_send_hi(addr):
        try:
            mesh.send(addr, json.dumps({"t": "mesh_hi", "name": assigned[0]}).encode("utf-8"))
        except Exception:
            pass

    def mesh_housekeeping(now):
        if mesh is None:
            return
        # name freshly connected links (both sides say hi; duplicates harmless)
        for ln in list(mesh.links.values()):
            if ln.connected and ln.name is None and now - last_hi.get(ln.addr, 0.0) >= MESH_HI_INTERVAL:
                last_hi[ln.addr] = now
                mesh_send_hi(ln.addr)
        # a failed dial may succeed later (NAT state changes): retry per roster
        # heal by forgetting failures every 30 s
        for nm in list(mesh.dials):
            d = mesh.dials[nm]
            if d.failed and now - d.started > 30.0:
                mesh.forget_dial(nm)
        mesh_plan_dials()
        # tell the host who we reach directly (it goes into everyone's roster)
        if now - last_links_report[0] >= MESH_LINKS_INTERVAL:
            last_links_report[0] = now
            direct = [n for n in mesh.direct_names() if n != "host"]
            if direct != reported_links[0]:
                reported_links[0] = direct
                send({"t": "links", "direct": direct})

    def mesh_relay_via(dest):
        """Who forwards our envelope to ``dest``: the host if it is alive, else
        any direct peer that reports a direct link to ``dest``."""
        if conn.last_seen_age() < host_gone_after:
            return conn.peer
        for nm in mesh.direct_names():
            if dest in roster_links[0].get(nm, []):
                ln = mesh.by_name(nm)
                if ln:
                    return ln.addr
        return None

    def mesh_forward(payload):
        """A local bridge frame -> every OTHER participant exactly once:
        direct where we have a link, else one envelope via a relay."""
        me = assigned[0]
        n = 0
        for nm in participants[0]:
            if nm == me:
                continue
            if nm == host_name[0] and not is_relay[0]:
                # the game host: its lobby is our transport peer
                if mesh.send(conn.peer, payload):
                    n += 1
                continue
            # (in a relay lobby the roster's host is the LEADER -- a joiner like
            # us: direct link or an envelope via the relay, never a plain frame
            # to the relay's address, which has no game to deliver it to)
            if mesh.by_name(nm):
                if mesh.send(nm, payload):
                    n += 1
                continue
            via = mesh_relay_via(nm)
            if via is not None and mesh.send(via, _relay_wrap(nm, me, payload)):
                n += 1
        return n

    def handle_peer(addr, payload):
        """Traffic from a NON-host address: a direct peer's bridge frame, a
        relay envelope (for us, or to forward), or a mesh_hi."""
        if not version_checked[0] or stop.is_set():
            return
        if payload[:1] == MESH_RELAY_MAGIC:
            env = _relay_unwrap(payload)
            if env is None:
                return
            to, frm, inner = env
            if to == assigned[0]:
                if GameRelay.is_game(inner) and relay is not None:
                    relay.deliver(inner)
            elif mesh.by_name(to):
                mesh.send(to, payload)          # we are the relay for this pair
            return
        if GameRelay.is_game(payload):
            ln = mesh.link(addr)
            if ln is not None and ln.connected and relay is not None:
                relay.deliver(payload)
            return
        if payload[:1] == FRAG_MAGIC:
            payload = frags.feed(addr, payload)
            if payload is None:
                return
        try:
            m = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        if m.get("t") == "mesh_hi":
            nm = str(m.get("name", ""))
            if nm and nm != assigned[0]:
                ln = mesh.link(addr)
                fresh = ln is None or ln.name != nm
                mesh.name_link(addr, nm)
                mesh.forget_dial(nm)
                if fresh:
                    mesh_send_hi(addr)          # make sure they can name us too
                    last_links_report[0] = 0.0  # report the new link promptly

    if mesh is not None and hasattr(conn, "on_peer"):
        conn.on_peer = handle_peer

    def handle_msg(raw):
        if stop.is_set():
            return
        if raw[:1] == FRAG_MAGIC:
            # a piece of a big control message (the roster of a big lobby is
            # N^2 and passes 64 KB at 60-70 players): whole once the last lands
            raw = frags.feed(conn.peer, raw)
            if raw is None:
                return
        if not version_checked[0]:
            try:
                greeting = json.loads(raw.decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                return
            if not isinstance(greeting, dict):
                return
            if greeting.get("t") in ("welcome", "roster"):
                reason = version_rejection(greeting.get("version"))
                if reason:
                    send({"t": "leave"})
                    io.emit({"type": "status", "state": "failed", "detail": reason})
                    io.write_state(state="failed", detail=reason, started=False)
                    log(f"[client] {reason}")
                    stop.set()
                    return
                version_checked[0] = True
                io.emit({"type": "status", "state": "connected", "detail": "joined the host"})
                io.write_state(state="connected")
            elif greeting.get("t") not in ("reject", "bye"):
                return
        # Two binary payload kinds are NOT JSON: a relayed bridge frame (first
        # byte GAME_RELAY_MAGIC) and a save chunk (CHUNK_MAGIC); everything
        # else is a JSON lobby message.
        if GameRelay.is_game(raw):
            if relay is not None:
                relay.deliver(raw)
            return
        if raw[:1] == MESH_RELAY_MAGIC:
            if mesh is not None:
                handle_peer(conn.peer, raw)
            else:
                env = _relay_unwrap(raw)
                if env and env[0] == assigned[0] and GameRelay.is_game(env[2]) and relay is not None:
                    relay.deliver(env[2])
            return
        if raw[:4] == CHUNK_MAGIC:
            if len(raw) >= 12:
                sid, seq = struct.unpack("!II", raw[4:12])
                receiver.on_chunk(sid, seq, raw[12:])
            return
        try:
            m = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        t = m.get("t")
        if t == "mods_manifest":
            receiver.on_manifest(m.get("mods"), m.get("mods_unknown"))
            return
        if recovery and recovery.message(m):
            return
        if t == "fbegin":
            if recovery and recovery.begin(m):
                return
            if recovery and recovery.held:
                return
            receiver.on_begin(m)
            return
        if t in ("fbegin_ack", "fack", "fdone"):
            if uploader[0] is not None:
                getattr(uploader[0], "on_" + t.replace("fbegin_ack", "begin_ack"))(conn.peer, m)
            return
        if t in ("welcome", "roster"):
            lobby_epoch = m.get("transport_lobby", "")
            # A nonce only ever moves forward (the lobby's own, then each completed
            # resync's epoch); one seen before is a reordered old roster, and
            # handing it to the menu would reset the bridge mid-game.
            if isinstance(lobby_epoch, str) and re.fullmatch(r"[0-9a-f]{32}", lobby_epoch) and lobby_epoch not in seen_nonces:
                seen_nonces.add(lobby_epoch)
                io.emit(dict(type='transport_lobby', epoch=lobby_epoch))
        if t == "welcome":
            receiver.on_manifest(m.get("mods", []), m.get("mods_unknown"))
            assigned[0] = m.get("you", desired[0])
            host_name[0] = m.get("host")
            is_relay[0] = bool(m.get("relay"))
            if recovery:
                recovery.identify(assigned[0], host_name[0], m.get("recovery") == 4 and not is_relay[0])
            io.write_state(you=assigned[0], host=m.get("host"))
            log(f"[client] host named us {assigned[0]!r}")
        elif t == "roster":
            receiver.on_manifest(m.get("mods", []), m.get("mods_unknown"))
            players = m.get("players", [])
            host_name[0] = m.get("host", host_name[0])
            is_relay[0] = bool(m.get("relay", is_relay[0]))
            if recovery:
                recovery.identify(assigned[0], host_name[0], m.get("recovery") == 4 and not is_relay[0])
            participants[0] = list(players)
            roster_links[0] = m.get("links", {}) or {}
            roster_profiles[0] = m.get("profiles", {}) or {}
            mesh_plan_dials()
            companies = m.get("companies", {}) or {}
            stages = m.get("stages") or {}
            lobby_mode = m.get("mode") or "coop"
            key = (tuple(players), tuple(sorted(companies.items())), tuple(sorted(stages.items())), lobby_mode)
            if key != last_roster[0]:
                last_roster[0] = key
                io.emit({"type": "roster", "players": players,
                         "you": assigned[0], "host": m.get("host"),
                         "lobby": m.get("lobby", ""), "companies": companies, "stages": stages,
                         "relay": is_relay[0], "letters": m.get("letters") or {},
                         "stored_age": m.get("stored_age", -1), "stored_max": m.get("stored_max", -1),
                         "mode": lobby_mode})
                io.write_state(state="connected", players=players,
                               you=assigned[0], host=m.get("host"),
                               started=started[0], lobby=m.get("lobby", ""), companies=companies, mode=lobby_mode)
            # Start self-heal: the host's roster carries started:true for us
            # once we were included in a start -- catches a lost start burst.
            if m.get("started") is True:
                apply_start(m.get("start_save", False), via="roster")
        elif t == "chat":
            cid = m.get("cid")
            if cid in seen_set:
                return                                      # de-dupe the burst
            if len(seen_cids) == seen_cids.maxlen:
                seen_set.discard(seen_cids.popleft())
            seen_cids.append(cid)
            seen_set.add(cid)
            io.emit({"type": "chat", "from": m.get("from"),
                     "text": m.get("text"), "ts": m.get("ts")})
        elif t == "start":
            if m.get("switch"):
                apply_switch_start(m.get("save", False))
            else:
                apply_start(m.get("save", False), via="start")
        elif t == "status":
            # Advisory from the host (e.g. late joiner: game already started).
            io.emit({"type": "status",
                     "state": str(m.get("state", "connected")),
                     "detail": str(m.get("detail", ""))})
            log(f"[client] host says: {m.get('detail', '')}")
        elif t == "reject":
            io.emit({"type": "status", "state": "failed",
                     "detail": m.get("reason", "rejected")})
            io.write_state(state="failed", detail=m.get("reason", "rejected"), started=False)
            stop.set()
        elif t == "bye":
            io.emit({"type": "status", "state": "failed",
                     "detail": "host closed the lobby"})
            stop.set()

    def handle_command(cmd):
        if recovery and recovery.command(cmd):
            return
        if recovery and recovery.held and cmd.get("cmd") in ("start", "name", "company"):
            return
        c = cmd.get("cmd")
        if not version_checked[0] and c not in ("quit", "name"):
            return
        if c == "chat":
            send({"t": "chat", "text": str(cmd.get("text", ""))})
        elif c == "mods":
            receiver.answer_mods(bool(cmd.get("accept")),cmd.get("offer"))
        elif c == "stage":
            send({"t": "stage", "text": str(cmd.get("text", ""))[:80]})
        elif c == "company":
            # the panel names a player when the leader of a relay lobby clicks
            # someone else's chip; this used to be overwritten with our own
            # name, so the leader could only ever change its own (2026-09-10)
            send({"t": "company", "player": str(cmd.get("player") or assigned[0]), "id": cmd.get("id")})
        elif c == "mode":
            # the relay leader's SEPARATE COMPANIES checkbox; the relay accepts it from the leader only
            send({"t": "mode", "mode": str(cmd.get("mode", ""))})
        elif c == "name":
            desired[0] = str(cmd.get("name", "player"))
            m2 = {"t": "join", "version": LOBBY_VERSION, "name": desired[0], "mesh": mesh is not None, "recovery": 4 if recovery else 0}
            if profile_code:
                m2["profile"] = profile_code
            send(m2)
        elif c == "start":
            if not is_relay[0]:
                log("[client] 'start' ignored -- only the host can start")
            elif host_name[0] != assigned[0]:
                log(f"[client] 'start' ignored -- the leader is {host_name[0]!r}")
            elif not cmd.get("save"):
                send({"t": "start"})                      # no-save start via the relay
            elif uploader[0] is not None:
                log("[client] start ignored -- an upload is in progress")
            elif not _mod_check(str(cmd.get("save")), io, log):
                pass                                      # refused: made without the mod (status + chat say so)
            else:
                try:
                    blob, files_meta = _read_save_files(str(cmd.get("save")))
                except (OSError, ValueError) as e:
                    io.emit({"type": "status", "state": "failed", "detail": f"save read failed: {e}"})
                    log(f"[client] save read failed: {e}")
                    return
                sid = int(time.time() * 1000) & 0xFFFFFFFF
                mods = modshare.save_mod_list(str(cmd.get("save")), log)   # None = could not read it (NOT "none")
                if mods is None:
                    log(f"[client] the mod list of {cmd.get('save')} is UNKNOWN -- the relay is told so; nobody is offered its mods")
                    io.emit({"type": "chat", "from": "MULTIPLAYER",
                             "text": f"Could not read which mods {os.path.basename(str(cmd.get('save')))} needs; players who lack them "
                                     "will not be offered a download. If their game refuses the save, they must install your mods by hand."})
                uploader[0] = _HostSaveTransfer(conn.sock, sid, blob, files_meta,
                                                [(conn.peer, "relay")], io, log, mods=mods)
                io.emit({"type": "status", "state": "connected",
                         "detail": "uploading the save to the relay..."})
                log(f"[client] uploading {cmd.get('save')} ({len(blob)} B) to the relay")
        elif c == "quit":
            _STOPPING[0] = True
            send({"t": "leave"})
            io.emit({"type": "status", "state": "failed", "detail": "left lobby"})
            stop.set()
        else:
            _report_command(cmd, io, log)

    # Game relay, outbound half: the client's transport is a queue-fed
    # Connection (no select loop to join), so the loopback socket gets its own
    # select() thread that wraps each local bridge frame and sends it to the
    # host. The inbound half rides the normal inbox path (handle_msg above).
    relay_stop = threading.Event()
    relay_thread = None
    if relay is not None:
        def relay_forward(payload):
            if not version_checked[0] or stop.is_set():
                return 0
            if mesh is not None:
                return mesh_forward(payload)
            conn.send(payload)          # RuntimeError (no peer yet) -> pump
            return 1

        relay_thread = threading.Thread(target=relay.pump_loop,
                                        name="game-relay",
                                        args=(relay_forward, relay_stop),
                                        daemon=True)
        relay_thread.start()

    join_msg = {"t": "join", "version": LOBBY_VERSION, "name": desired[0], "mesh": mesh is not None, "recovery": 4 if recovery else 0,
                "batches": 1}          # this client takes a mods round in batches (0.6.1.7)
    if profile_code:
        join_msg["profile"] = profile_code
    send(join_msg)                                          # announce ourselves
    last_ping = 0.0
    last_dual_tick = [0.0]
    join_sent_at = time.time()
    welcomed = [False]
    try:
        while not stop.is_set():
            if not version_checked[0] and time.time() - join_sent_at > 15.0:
                reason = "Host did not confirm a compatible multiplayer version. Install the same version on both sides."
                send({"t": "leave"})
                io.emit({"type": "status", "state": "failed", "detail": reason})
                io.write_state(state="failed", detail=reason, started=False)
                break
            if conn.last_seen_age() > host_gone_after:
                io.emit({"type": "status", "state": "failed",
                         "detail": "host unreachable"})
                io.write_state(state="failed")
                break
            # Drain a burst of inbound datagrams (chunks arrive fast during a
            # transfer). Block briefly on the first recv so we don't spin when
            # idle; then pull whatever else is already queued, up to DRAIN_CAP.
            # An UPLOAD in progress is as busy as a download: the sender's pump
            # runs once per loop iteration, so the 0.2 s idle timeout capped a
            # leader's upload at ~5 pumps/s x SEND_BUDGET x 1200 B = 1.5 MB/s
            # (139 MB took minutes on a link that could do several MB/s).
            busy = receiver.active() or uploader[0] is not None
            raw = conn.recv(timeout=0.002 if uploader[0] is not None else (0.02 if busy else 0.2))
            drained = 0
            while raw is not None:
                try:
                    handle_msg(raw)
                except Exception as e:                      # never die on one msg
                    log(f"[client] message error: {e!r}")
                drained += 1
                if drained >= DRAIN_CAP:
                    break
                raw = conn.recv(timeout=0.0)
            if stop.is_set():
                break
            now = time.time()
            mesh_housekeeping(now)
            ds = getattr(conn, "sock", None)
            if isinstance(ds, dual_tcp.DualSocket) and now - last_dual_tick[0] >= 0.5:
                last_dual_tick[0] = now
                ds.tick({conn.peer: "host"})              # the TCP backup link's counters, every 10 s
            receiver.tick(now)                              # facks / fdone cadence
            if receiver.cancelled:
                send({"t":"leave"})
                io.emit({"type":"mods_cancelled", "text":receiver.cancel_reason})
                io.write_state(state="disconnected",started=False)
                stop.set()
                continue
            if recovery:
                recovery.tick(now)
            if uploader[0] is not None:
                try:
                    uploader[0].pump(now)
                    if uploader[0].all_resolved():
                        up, uploader[0] = uploader[0], None
                        if up.failed_names() or up.done_count() == 0:
                            io.emit({"type": "status", "state": "failed",
                                     "detail": "upload to the relay failed -- press START GAME to retry"})
                            log("[client] upload to the relay FAILED")
                        elif up.kind=="save" and up.mod_needs():
                            needed=up.mod_needs().get(conn.peer,[])
                            blob2,meta2=bytearray(),[]
                            for m,v in needed:
                                data=modshare.package_mod(m,v)
                                if data is None: raise ValueError("cannot upload required mod " + m)
                                meta2.append({"name":modshare.mod_zip_name(m,v),"size":len(data),"sha256":hashlib.sha256(data).hexdigest()})
                                blob2+=data
                            uploader[0]=_HostSaveTransfer(conn.sock,(up.sid+1)&0xffffffff,blob2,meta2,[(conn.peer,"relay")],io,log,kind="mods")
                        else:
                            uploaded[0] = True
                            io.emit({"type": "status", "state": "connected",
                                     "detail": "save uploaded -- the relay is sharing it..."})
                            log("[client] upload to the relay complete")
                except Exception as e:                      # noqa: BLE001
                    log(f"[client] upload error: {e!r}")
                    uploader[0] = None
            if now - last_ping >= PING_INTERVAL:
                last_ping = now
                send({"t": "ping"})
                # No welcome yet: the join can be lost (UDP), or the host's port
                # was briefly shared with another process. Say it again; the
                # host treats a repeat as a rename in place, so it is harmless.
                if host_name[0] is None and now - join_sent_at >= PING_INTERVAL:
                    send(join_msg)
                    log("[client] no welcome yet -- re-sending join")
            for cmd in io.poll_commands():
                handle_command(cmd)
            if relay is not None:
                relay.tick(now)                             # 10 s stats line
            fwd.poll_tails()
            lines = fwd.drain(now)
            if lines:
                send({"t": "log", "lines": lines})
    except KeyboardInterrupt:
        send({"t": "leave"})
    finally:
        _STOPPING[0] = True
        io.write_state(state="failed" if io._state.get("state") == "failed" else "disconnected", started=False)
        lines = fwd.drain(time.time() + LOG_FLUSH_INTERVAL)   # last words
        if lines:
            send({"t": "log", "lines": lines})
        try:
            _log_sinks.remove(fwd.add)
        except ValueError:
            pass
        relay_stop.set()
        if relay_thread is not None:
            relay_thread.join(timeout=1.0)  # its select() wakes within 0.2 s
        if relay is not None:
            relay.close()
        conn.close()


# --------------------------------------------------------------------------- #
# CLI commands
# --------------------------------------------------------------------------- #
def _make_relay(args):
    """Build the :class:`GameRelay` from --game-relay-port / --game-local-port,
    or None when the menu didn't ask for one (legacy behaviour, unchanged).

    A bind failure does NOT kill the lobby -- roster/chat/save still work --
    but it is shouted on stderr because lockstep will not connect.
    """
    port = getattr(args, "game_relay_port", None)
    if not port:
        return None
    local = getattr(args, "game_local_port", None) or GAME_LOCAL_PORT_DEFAULT
    try:
        return GameRelay(port, local)
    except OSError as e:
        _log(f"[relay] WARNING: cannot bind game relay 127.0.0.1:{port}: {e} "
             f"-- bridge frames will NOT be relayed this session")
        return None


def _host_upnp_unmap(port):
    """Remove the UPnP mapping the host's observe step keeps for the session
    (observe(..., keep_upnp=True)). Best-effort."""
    try:
        from observe import upnp_unmap
        if upnp_unmap(port):
            _log(f"[host] UPnP mapping for udp/{port} removed")
    except Exception as e:                            # noqa: BLE001
        _log(f"[host] UPnP unmap skipped: {e}")


def cmd_host(args):
    io = LobbyIO(args.io_dir or os.getcwd())
    io.emit({"type": "status", "state": "waiting", "detail": "observing NAT"})
    io.write_state(state="waiting")
    relay = _make_relay(args)               # bind early so a clash shows up now
    # observe + print the single CODE= line (reused from connect.py). The code
    # carries a fresh session secret: whoever has the code can talk to us,
    # nobody else can read or inject; --password layers on top of it.
    secret = None
    if getattr(args, "relay_only", False) or getattr(args, "dedicated", False):
        # a dedicated relay -- or a dedicated game server (--dedicated) -- keeps
        # its secret: the same code stays valid across restarts (the address and
        # port are fixed too), so nobody re-pastes
        spath = os.path.join(io.dir, "relay_secret.bin")
        try:
            with open(spath, "rb") as f:
                secret = f.read()
            if len(secret) != SECRET_LEN:
                secret = None
        except OSError:
            secret = None
        if secret is None:
            secret = os.urandom(SECRET_LEN)
            try:
                with open(spath, "wb") as f:
                    f.write(secret)
                os.chmod(spath, 0o600)
            except OSError as e:
                _log(f"[host] could not keep the relay secret: {e}")
    if secret is None:
        secret = os.urandom(SECRET_LEN)
    SEAL[0] = Sealer(derive_key(secret, args.password or ""))
    tunnel = _steam_tunnel(args)
    try:
        sock, _profile, code = _observe_and_announce(args.local_port, secret=secret,
                                                     password=args.password or None,
                                                     extra_candidates={"steam": tunnel.id})
        MY_TCP_ADDRS[0] = _profile_ips(_profile)
        if tunnel.available:
            tunnel.hello(sock.getsockname()[1])
    except BaseException:
        # Stopped while observing (outside Windows: SIGTERM or --parent-pid).
        # observe() may already have added the mapping it keeps for the session,
        # and the try/finally below that removes it does not exist yet.
        _host_upnp_unmap(args.local_port)
        tunnel.close()
        raise
    steam_code = tunnel.id if tunnel.available and not args.relay_only and not getattr(args, "dedicated", False) else None
    crossplay = bool(getattr(args, "crossplay", False)) or not steam_code
    publisher = None
    rendezvous = None
    # From here on the mapping exists: every way out -- run_host returning, or a
    # stop before the lobby loop is up -- goes through the finally.
    try:
        if args.password:
            _log("[host] the code is LOCKED: without the password it reveals nothing")
        else:
            _log("[host] the code is plain: anyone who sees it can read your address "
                 "-- set a password to lock it")
        _log("[host] frames are sealed (session key from the code"
             + (" + password)" if args.password else ")"))
        if args.publish:
            publisher = _Publisher(args.publish, code if crossplay else steam_code, "relay" if args.relay_only else ("dedicated" if getattr(args, "dedicated", False) else "host"), bool(args.password), _log,
                                   stable_key=f"relay|{args.lobby_name}|{args.local_port}" if args.relay_only else None)
            # systemd stops the relay with SIGTERM; without a handler Python just
            # dies and the finally: below (publisher.close -> /leave) never runs,
            # so the public list kept the dead row for a full TTL
            import signal as _sig
            def _term(_signo, _frame):
                raise KeyboardInterrupt
            try:
                # outside Windows main() already installed the once-only _on_sigterm
                _sig.signal(_sig.SIGTERM, _term if sys.platform == "win32" else _on_sigterm)
            except (ValueError, OSError):
                pass
            publisher.update(args.lobby_name or args.name, 0 if args.relay_only else 1)
            if args.public:
                publisher.set(True)
        # A dedicated relay's port is open by construction; only player hosts punch.
        rv_url = "" if args.relay_only else _rv_url(args)
        if rv_url:
            rendezvous = _RendezvousHost(rv_url, secret, args.password or "", _log, tunnel=tunnel)
            _log(f"[rendezvous] polling {rv_url} for joiners to punch toward")
        if args.relay_only:
            _log("[host] RELAY-ONLY: no game here; the oldest joiner is the leader")
        else:
            _publish_registry_at_start(_log)
        run_host(sock, args.name, io, code=code, relay=None if args.relay_only else relay,
                 forward_logs=args.forward_log or (), publisher=publisher,
                 lobby_name=args.lobby_name, relay_only=bool(args.relay_only),
                 punch_q=rendezvous.queue if rendezvous is not None else None,
                 sync_runtime=make_runtime(args) if not args.relay_only else None,
                 companies_mode=bool(args.companies),
                 cross_code=code, steam_code=steam_code,
                 steam_secret=secret if steam_code else None, crossplay=crossplay)
    finally:
        _STOPPING[0] = True
        if rendezvous is not None:
            rendezvous.close()
        if publisher is not None:
            publisher.close()
        tunnel.close()
        _host_upnp_unmap(args.local_port)
    return 0


STEAM_KEYX_TIMEOUT = 30.0   # joiner: seconds to wait for the host's key over Steam
STEAM_KEYX_EVERY = 0.5      # joiner: seconds between offers


def _steam_key_exchange(sock, ep, timeout):
    """Ask the host at Steam tunnel endpoint ``ep`` for the session secret (steamkey.py).
    Resends the offer until an answer to it arrives; anything else that arrives is
    dropped (nothing else can arrive before the handshake). The secret, or None."""
    offer = steamkey.Offer()
    frame = _pack(TYPE_KEYX, offer.payload())
    old = sock.gettimeout()
    deadline = time.time() + timeout
    next_send = 0.0
    try:
        while time.time() < deadline:
            now = time.time()
            if now >= next_send:
                try:
                    sock.sendto(frame, ep)
                except OSError:
                    pass
                next_send = now + STEAM_KEYX_EVERY
            sock.settimeout(max(0.05, min(next_send, deadline) - time.time()))
            try:
                data, addr = sock.recvfrom(65535)
            except (socket.timeout, BlockingIOError):
                continue
            except OSError:
                continue
            if tuple(addr[:2]) != tuple(ep):
                continue
            ptype, payload = _unpack(data)
            if ptype == TYPE_KEYX:
                secret = offer.secret_from(payload)
                if secret is not None:
                    _log(f"[steam] the host sent the session key over Steam ({(timeout - (deadline - time.time())):.1f} s)")
                    return secret
        _log(f"[steam] no key from the host over Steam within {timeout:.0f} s")
        return None
    finally:
        sock.settimeout(old)


def cmd_join(args):
    io = LobbyIO(args.io_dir or os.getcwd())
    io.emit({"type": "status", "state": "waiting", "detail": "dialing host"})
    io.write_state(state="waiting")
    try:
        peer = decode_code(args.code, password=args.password or None)
    except ValueError as e:
        io.emit({"type": "status", "state": "failed", "detail": f"bad code: {e}"})
        io.write_state(state="failed")
        _log(f"[join] bad code: {e}")
        return 2
    if peer.get("stale"):
        _log(f"[join] WARNING: code is {peer['age']}s old -- may be stale")
    steam_only = bool(peer.get("steam_only"))
    if steam_only:
        _log(f"[join] the code is a Steam ID ({peer['candidates']['steam']}): joining through Steam; "
             "the session key comes from the host over Steam")
    elif peer.get("secret"):
        SEAL[0] = Sealer(derive_key(peer["secret"], args.password or ""))
        _log("[join] frames are sealed (session key from the code"
             + (" + password)" if args.password else ")"))
    else:
        _log("[join] WARNING: this code carries no session secret -- the lobby "
             "is PLAINTEXT and unauthenticated (old host?)")
    relay = _make_relay(args)               # bind early so a clash shows up now
    # Star model: the host is open, so we simply DIAL its v4 candidates. This
    # reuses connect.py's race (role='dial', v4 only -> single socket).
    sock = open_socket(args.local_port, socket.AF_INET)
    # Mesh: learn our OWN public mapping on this same socket first (STUN, no
    # UPnP), so the host can hand the other joiners a code that punches us.
    profile_code = None
    prof = None
    tunnel = _steam_tunnel(args)
    if tunnel.available:
        tunnel.hello(sock.getsockname()[1])
    steam_ep = None
    if steam_only:
        fail = None
        if not tunnel.available:
            fail = ("this host's code is a Steam ID, and this game has no Steam networking: "
                    "start the game through Steam, or ask the host to tick CROSS-PLAY and send the long code")
        elif peer["candidates"]["steam"] == tunnel.id:
            fail = "that Steam ID is your own"
        else:
            steam_ep = tunnel.dial(peer["candidates"]["steam"])
            if not steam_ep:
                fail = "Steam could not open a connection to that player"
            else:
                secret = _steam_key_exchange(sock, steam_ep, min(args.timeout, STEAM_KEYX_TIMEOUT))
                if secret is None:
                    fail = ("no lobby answered through Steam: the host has not opened one, "
                            "or its game does not have this version")
                else:
                    SEAL[0] = Sealer(derive_key(secret, args.password or ""))
                    _log("[join] frames are sealed (session key from the host over Steam"
                         + (" + password)" if args.password else ")"))
        if fail:
            io.emit({"type": "status", "state": "failed", "detail": fail})
            io.write_state(state="failed")
            _log(f"[join] FAILED: {fail}")
            tunnel.close()
            if relay is not None:
                relay.close()
            return 1
    if not getattr(args, "no_mesh", False):
        try:
            from observe import observe
            prof = observe(args.local_port, sock=sock, do_upnp=False)
            MY_TCP_ADDRS[0] = _profile_ips(prof)       # offered to a host reached through Steam
            if tunnel.available:
                prof["candidates"]["steam"] = tunnel.id   # the knock tells the host to open our Steam session
            profile_code = encode_profile(prof)
            _log(f"[join] self-observed candidates={prof['candidates']} "
                 f"flags={prof['flags']}")
        except Exception as e:                            # noqa: BLE001
            _log(f"[join] self-observe failed: {e} -- peers will reach us via relay")
    # Knock at the master while we dial: the host punches toward our address, so
    # a host whose port is not really open still gets through (see RENDEZVOUS).
    knock = None
    rv_url = _rv_url(args)
    late_targets = [steam_ep] if steam_ep else []   # the master's relay port, once asked for (see RV_RELAY_FROM_KNOCK)
    # The host's Steam identity from the code: dial it through the tunnel as one
    # more candidate. On a thread, so the direct dial starts at once; the race
    # picks the endpoint up from late_targets. Steam punches or relays on its own,
    # so this is the path that works where nothing else does (CGNAT, both closed).
    host_steam = peer.get("candidates", {}).get("steam")
    if host_steam and tunnel.available and host_steam != tunnel.id and not steam_only:
        def _dial_steam():
            ep = tunnel.dial(host_steam)
            if ep:
                _log(f"[steam] the host is {host_steam} on Steam -- dialling it through Steam at {ep[0]}:{ep[1]} as well")
                late_targets.append(ep)
        threading.Thread(target=_dial_steam, name="steam-dial", daemon=True).start()
    elif host_steam and not tunnel.available:
        _log("[steam] the host is on Steam but this game has no Steam transport -- direct dial and the master's punch only")
    if rv_url and profile_code and peer.get("secret"):
        knock = _RendezvousKnock(rv_url, peer["secret"], args.password or "", profile_code, _log,
                                 sock=sock, late=late_targets)
    try:
        conn = race(sock, peer, "dial", args.local_port, args.timeout,
                    my_has_v6=False, mine=prof, late_targets=late_targets)
    finally:
        if knock is not None:
            knock.close()
    if not conn:
        io.emit({"type": "status", "state": "failed",
                 "detail": "could not reach host"})
        io.write_state(state="failed")
        _log("[join] FAILED to reach host")
        if relay is not None:
            relay.close()
        return 1
    _log(f"[join] connected to host {conn.peer_str}"
         + (" -- through Steam's networking" if steamtunnel.is_tunnel_addr(conn.peer) else ""))
    conn.cipher = SEAL[0]
    sim = _impair_client(conn, io.dir, _log)
    if _tcp_backup_on(io.dir) and SEAL[0] is not None:
        _start_dual_client(conn, args.name, _log, sim)
    mesh = None
    if not getattr(args, "no_mesh", False):
        mesh, conn = _mesh_from_conn(conn)
        _log("[join] mesh: direct links to other joiners enabled")
    _publish_registry_at_start(_log)
    if steamtunnel.is_tunnel_addr(conn.peer) and BULK_TCP[0]:
        # the host cannot see where we are: open our TCP listener and map it now,
        # so a save's fbegin_ack can offer an address the host is able to dial
        JOINER_UPNP["started"] = True
        threading.Thread(target=_open_steam_joiner_tcp, args=(sock.getsockname()[1], _log),
                         name="steam-joiner-tcp", daemon=True).start()
    try:
        run_client(conn, args.name, io, relay=relay, mesh=mesh,
                   profile_code=profile_code, forward_logs=args.forward_log or (), sync_runtime=make_runtime(args))
    finally:
        _close_steam_joiner_tcp(_log)
    return 0


# --------------------------------------------------------------------------- #
# Self-test: 1 host + 2 joiners on loopback; roster + chat must reach all three
# --------------------------------------------------------------------------- #
def _read_events(path):
    events = []
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        events.append(json.loads(line))
                    except ValueError:
                        pass
    except FileNotFoundError:
        pass
    return events


def _latest_roster(path):
    rosters = [e for e in _read_events(path) if e.get("type") == "roster"]
    return rosters[-1] if rosters else None


def _has_chat(path, text):
    return any(e.get("type") == "chat" and e.get("text") == text
               for e in _read_events(path))


def _wait_until(predicate, timeout=12.0, interval=0.15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


def _has_start(path, save=None):
    """True if lobby_out.jsonl at ``path`` has a start event (optionally with
    the given ``save`` flag)."""
    return any(e.get("type") == "start"
               and (save is None or e.get("save") is save)
               for e in _read_events(path))


def _dial_loopback(port, host_port, timeout):
    """Dial a loopback host with the connect race; returns a Connection/None."""
    peer = {"candidates": {"public_v4": f"127.0.0.1:{host_port}",
                           "lan_v4": None, "v6": None},
            "flags": {"open": True, "v6": False}}
    s = open_socket(port, socket.AF_INET)
    return race(s, peer, "dial", port, timeout, my_has_v6=False)


def selftest():
    HP, P1, P2, P3 = 29520, 29521, 29522, 29523
    names = {"host": "alice", "j1": "bob", "j2": "carol"}
    late_name = "dave"                        # joins AFTER the start (case d)
    expected = sorted(names.values())
    base = tempfile.mkdtemp(prefix="lobby_selftest_")
    dirs = {k: os.path.join(base, k) for k in list(names) + ["j3"]}
    ios = {k: LobbyIO(dirs[k]) for k in names}
    stop = threading.Event()
    conns = []
    print(f"[selftest] scratch dir: {base}")
    print(f"[selftest] host udp/{HP}  joiners udp/{P1},{P2}  late udp/{P3}")

    # A stale save from "a previous session" in j1's io dir: the joiner must
    # delete it before joining (case e).
    stale = os.path.join(dirs["j1"], INCOMING_BASENAME + ".sav")
    with open(stale, "wb") as f:
        f.write(b"stale save from an earlier lobby")

    ok = True
    try:
        # --- host ---
        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "SELFTESTCODE", "stop": stop},
                         daemon=True).start()
        time.sleep(0.4)                       # let the server bind + listen

        # --- two joiners dial the host over loopback (reuse the connect race) --
        for port, key in ((P1, "j1"), (P2, "j2")):
            peer = {"candidates": {"public_v4": f"127.0.0.1:{HP}",
                                   "lan_v4": None, "v6": None},
                    "flags": {"open": True, "v6": False}}
            s = open_socket(port, socket.AF_INET)
            conn = race(s, peer, "dial", port, 10, my_has_v6=False)
            if not conn:
                print(f"[selftest] FAIL: joiner {names[key]} could not connect")
                return False
            conns.append(conn)
            threading.Thread(target=run_client, name=key,
                             args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop}, daemon=True).start()

        # (a) every roster must list all three usernames
        def rosters_full():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or sorted(r.get("players", [])) != expected:
                    return False
            return True

        if not _wait_until(rosters_full, timeout=12):
            ok = False
            for k in names:
                print(f"[selftest]   {k} roster = {_latest_roster(ios[k].out_path)}")
            print("[selftest] FAIL (a): all three not in every roster")
        else:
            print(f"[selftest] OK (a): every roster = {expected}")

        # (a1) bob picks company 2: every roster carries companies {bob: 2}
        with open(ios["j1"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "company", "id": 2}) + "\n")
        def companies_ok():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or (r.get("companies") or {}).get("bob") != 2:
                    return False
            return True
        if not _wait_until(companies_ok, timeout=8):
            ok = False
            print("[selftest] FAIL (a1): company choice did not reach every roster")
        else:
            print("[selftest] OK (a1): bob -> company 2 visible in every roster")

        # (a2) every joiner's log lines reach the host's merged log, tagged
        merged = os.path.join(dirs["host"], PEERS_LOG_NAME)
        def merged_has_all():
            try:
                txt = open(merged, encoding="utf-8").read()
            except OSError:
                return False
            return all(("[" + names[k] + "] ") in txt for k in ("j1", "j2", "host"))
        if not _wait_until(merged_has_all, timeout=8):
            ok = False
            print(f"[selftest] FAIL (a2): merged log lacks a peer's lines: {merged}")
        else:
            print("[selftest] OK (a2): host merged log has lines from bob, carol and itself")

        # (b) a chat from j1 must reach all three via their lobby_out.jsonl
        chat_text = "hello lobby from bob"
        with open(ios["j1"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "chat", "text": chat_text}) + "\n")

        def chat_everywhere():
            return all(_has_chat(ios[k].out_path, chat_text) for k in names)

        if not _wait_until(chat_everywhere, timeout=12):
            ok = False
            for k in names:
                got = _has_chat(ios[k].out_path, chat_text)
                print(f"[selftest]   {k} has chat: {got}")
            print("[selftest] FAIL (b): chat did not reach all three")
        else:
            print(f"[selftest] OK (b): chat reached host + both joiners")

        # (c) a legacy (no-save) START from the host -> everyone emits
        #     {"type":"start","save":false}
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start"}) + "\n")

        def started_everywhere():
            return all(_has_start(ios[k].out_path, save=False) for k in names)

        if not _wait_until(started_everywhere, timeout=12):
            ok = False
            for k in names:
                print(f"[selftest]   {k} start(save=false): "
                      f"{_has_start(ios[k].out_path, save=False)}")
            print("[selftest] FAIL (c): legacy start did not reach all three")
        else:
            print("[selftest] OK (c): legacy start save=false reached all three")

        # (d) a LATE joiner (after the start) must NOT be started -- not by a
        #     start message, and not by the periodic roster heal either -- and
        #     must be told why.
        ios["j3"] = LobbyIO(dirs["j3"])
        conn = _dial_loopback(P3, HP, 10)
        if not conn:
            print(f"[selftest] FAIL (d): late joiner {late_name} could not connect")
            ok = False
        else:
            conns.append(conn)
            threading.Thread(target=run_client, name="j3",
                             args=(conn, late_name, ios["j3"]),
                             kwargs={"stop": stop}, daemon=True).start()

            def late_notified():
                return any(e.get("type") == "status"
                           and "game already started" in e.get("detail", "")
                           for e in _read_events(ios["j3"].out_path))

            if not _wait_until(late_notified, timeout=12):
                ok = False
                print("[selftest] FAIL (d): late joiner never got the "
                      "'game already started' status")
            else:
                # Outlast a couple of roster heals to prove they don't start it.
                time.sleep(ROSTER_HEAL * 1.5)
                if _has_start(ios["j3"].out_path):
                    ok = False
                    print("[selftest] FAIL (d): late joiner was started")
                elif not (_latest_roster(ios["host"].out_path) and late_name in
                          _latest_roster(ios["host"].out_path)["players"]):
                    ok = False
                    print("[selftest] FAIL (d): late joiner not in host roster")
                else:
                    print("[selftest] OK (d): late joiner told 'game already "
                          "started', not started, in roster")

        # (e) the stale incoming_save.sav in j1's io dir was deleted on startup
        if os.path.exists(stale):
            ok = False
            print("[selftest] FAIL (e): stale incoming_save.sav survived join")
        else:
            print("[selftest] OK (e): stale incoming_save.sav removed before join")
    finally:
        stop.set()
        time.sleep(0.3)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        try:
            shutil.rmtree(base, ignore_errors=True)
        except Exception:
            pass

    print(f"[selftest] {'PASS' if ok else 'FAIL'}")
    return ok


# --------------------------------------------------------------------------- #
# Self-test: reliable SAVE TRANSFER host -> 2 joiners, with packet-loss injection
# --------------------------------------------------------------------------- #
class _LossySocket:
    """Wrap a UDP socket and randomly DROP a fraction of outbound datagrams.

    Used only by the transfer self-test to prove the ARQ retransmit path on
    loopback (loopback itself never loses). It forwards ``fileno`` so select()
    still works, exposes ``family``, and delegates everything else to the real
    socket; only ``sendto`` is intercepted."""

    def __init__(self, sock, loss=0.0):
        self._sock = sock
        self.loss = loss

    def sendto(self, data, addr):
        if self.loss and random.random() < self.loss:
            return len(data)                      # pretend sent; silently dropped
        return self._sock.sendto(data, addr)

    def fileno(self):
        return self._sock.fileno()

    @property
    def family(self):
        return self._sock.family

    def __getattr__(self, name):                  # recvfrom/setblocking/close/...
        return getattr(self._sock, name)


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _run_transfer_once(loss, size_bytes, tag, tcp=False, tcp_unreachable=False):
    """One full host + 2-joiner save transfer over loopback. Returns True/False.

    Asserts: each joiner's incoming_save.* is byte-identical (SHA-256) to the
    source, each joiner emitted save_ready + start, the host emitted a per-peer
    state:"done", and the host's {"type":"start"} came AFTER both done events.
    ``tcp`` runs the round with the TCP bulk channel (bulk_tcp.py) and asserts
    both joiners took it; ``tcp_unreachable`` makes every connect fail, so the
    round must complete over UDP with nobody on TCP.
    """
    HP, P1, P2 = 29520, 29521, 29522
    BULK_TCP[0] = bool(tcp)
    real_connect = bulk_tcp.bulk_connect
    if tcp_unreachable:
        bulk_tcp.bulk_connect = lambda *a, **k: None
    names = {"host": "alice", "j1": "bob", "j2": "carol"}
    base = tempfile.mkdtemp(prefix="lobby_xfer_")
    dirs = {k: os.path.join(base, k) for k in names}
    ios = {k: LobbyIO(dirs[k]) for k in names}
    stop = threading.Event()
    conns = []

    # Build the "save": a realistic .sav plus the two small sidecars.
    save_path = os.path.join(base, "world.sav")
    with open(save_path, "wb") as f:
        f.write(os.urandom(size_bytes))
    with open(save_path + ".lua", "wb") as f:            # world.sav.lua sidecar
        f.write(b'["lockstep.lua"] = { }\n-- meta\n' + os.urandom(2048))   # made with the mod
    with open(os.path.join(base, "world.jpg"), "wb") as f:  # world.jpg sidecar
        f.write(os.urandom(4096))
    src_sha = {
        "incoming_save.sav": _sha256_file(save_path),
        "incoming_save.sav.lua": _sha256_file(save_path + ".lua"),
        "incoming_save.jpg": _sha256_file(os.path.join(base, "world.jpg")),
    }

    ok = True
    t0 = time.time()
    lossy_socks = []
    try:
        # Establish the lobby with NO loss first: the base lobby sends 'join'
        # only once (roster/chat self-heal, but join does not retransmit), so
        # dropping the handshake/join is a separate pre-existing concern. We
        # switch loss ON only for the transfer, which is what we're validating.
        hsock = _LossySocket(open_socket(HP, socket.AF_INET), 0.0)
        lossy_socks.append(hsock)
        threading.Thread(target=run_host, name="xfer-host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "XFERCODE", "stop": stop},
                         daemon=True).start()
        time.sleep(0.4)

        for port, key in ((P1, "j1"), (P2, "j2")):
            peer = {"candidates": {"public_v4": f"127.0.0.1:{HP}",
                                   "lan_v4": None, "v6": None},
                    "flags": {"open": True, "v6": False}}
            s = _LossySocket(open_socket(port, socket.AF_INET), 0.0)
            lossy_socks.append(s)
            conn = race(s, peer, "dial", port, 12, my_has_v6=False)
            if not conn:
                print(f"[xfer:{tag}] FAIL: joiner {names[key]} could not connect")
                return False
            conns.append(conn)
            threading.Thread(target=run_client, name=key,
                             args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop}, daemon=True).start()

        # Wait until both joiners are in the roster (fully joined) before start.
        def all_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or len(r.get("players", [])) < 3:
                    return False
            return True

        if not _wait_until(all_joined, timeout=15):
            print(f"[xfer:{tag}] FAIL: not all three joined")
            return False

        # NOW turn on packet loss to exercise the transfer's retransmit path.
        for ls in lossy_socks:
            ls.loss = loss

        # Host issues the save-transfer start (the new command).
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start", "save": save_path}) + "\n")

        # Wait for the WHOLE handshake to settle: both joiners save_ready AND the
        # host recorded a per-peer done for both AND broadcast start. (save_ready
        # fires the instant a joiner assembles the file, which is BEFORE its
        # fdone reaches the host -- so we must not check host state on save_ready
        # alone, or we'd race the in-flight fdone.)
        def transfer_settled():
            for k in ("j1", "j2"):
                if not any(e.get("type") == "save_ready"
                           for e in _read_events(ios[k].out_path)):
                    return False
            hev = _read_events(ios["host"].out_path)
            done = sum(1 for e in hev if e.get("type") == "transfer"
                       and e.get("role") == "send" and e.get("state") == "done")
            started_ev = any(e.get("type") == "start" for e in hev)
            return done >= 2 and started_ev

        if not _wait_until(transfer_settled, timeout=120):
            print(f"[xfer:{tag}] FAIL: transfer did not settle in time")
            ok = False

        # (1) byte-identical files on each joiner
        for k in ("j1", "j2"):
            for fname, sha in src_sha.items():
                p = os.path.join(ios[k].dir, fname)
                if not os.path.isfile(p):
                    print(f"[xfer:{tag}] FAIL: {k} missing {fname}")
                    ok = False
                    continue
                if _sha256_file(p) != sha:
                    print(f"[xfer:{tag}] FAIL: {k} {fname} SHA mismatch")
                    ok = False

        # (2) save_ready shape check (dir + files list)
        for k in ("j1", "j2"):
            sr = [e for e in _read_events(ios[k].out_path)
                  if e.get("type") == "save_ready"]
            if sr and (sr[-1].get("name") != INCOMING_BASENAME
                       or "incoming_save.sav" not in sr[-1].get("files", [])):
                print(f"[xfer:{tag}] FAIL: {k} save_ready shape = {sr[-1]}")
                ok = False

        # (3) host per-peer done x2, and start(save=true) AFTER both done events
        hev = _read_events(ios["host"].out_path)
        done_idx = [i for i, e in enumerate(hev)
                    if e.get("type") == "transfer" and e.get("role") == "send"
                    and e.get("state") == "done"]
        start_idx = [i for i, e in enumerate(hev) if e.get("type") == "start"]
        if len(done_idx) < 2:
            print(f"[xfer:{tag}] FAIL: host emitted {len(done_idx)} done (want 2)")
            ok = False
        if not start_idx:
            print(f"[xfer:{tag}] FAIL: host never broadcast start")
            ok = False
        elif hev[start_idx[-1]].get("save") is not True:
            print(f"[xfer:{tag}] FAIL: host start lacks save:true: "
                  f"{hev[start_idx[-1]]}")
            ok = False
        if done_idx and start_idx and start_idx[-1] < max(done_idx):
            print(f"[xfer:{tag}] FAIL: start was broadcast BEFORE transfers done")
            ok = False

        # (4) both joiners actually started, with save:true
        for k in ("j1", "j2"):
            if not _has_start(ios[k].out_path, save=True):
                print(f"[xfer:{tag}] FAIL: {k} never received start(save=true)")
                ok = False

        # (5) the path: with TCP on and reachable both joiners streamed (and the
        # host served two streams); otherwise nobody touched TCP
        via_tcp = [k for k in ("j1", "j2") if any(e.get("type") == "transfer" and e.get("state") == "tcp"
                                                   for e in _read_events(ios[k].out_path))]
        served = sum(1 for e in hev if e.get("type") == "transfer" and e.get("role") == "send" and e.get("state") == "tcp")
        if tcp and not tcp_unreachable and (len(via_tcp) != 2 or served != 2):
            print(f"[xfer:{tag}] FAIL: expected both joiners on TCP, got {via_tcp} (host served {served})")
            ok = False
        if (not tcp or tcp_unreachable) and (via_tcp or served):
            print(f"[xfer:{tag}] FAIL: TCP was used ({via_tcp}, host served {served}) although it was off/unreachable")
            ok = False
    finally:
        stop.set()
        time.sleep(0.5)                           # let the host release its port
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        shutil.rmtree(base, ignore_errors=True)
        bulk_tcp.bulk_connect = real_connect
        BULK_TCP[0] = True

    dt = time.time() - t0
    mb = size_bytes / (1024 * 1024)
    rate = mb / dt if dt else 0
    print(f"[xfer:{tag}] {'OK' if ok else 'FAIL'}  "
          f"({mb:.0f} MB, loss={loss:.0%}, {dt:.1f}s, ~{rate:.1f} MB/s to 2 peers)")
    return ok


def _run_transfer_failure(tag, size_bytes=4 * 1024 * 1024):
    # NB: size must exceed SEND_WINDOW chunks (~2.4 MB) so the host has SEEN
    # an advanced base before the receiver re-requests from 0 -- that is the
    # only way the rewind path is actually exercised.
    """One joiner's receiver FAILS (its copy of chunk 0 is corrupted, so every
    hash check fails until it gives up with fdone ok:false final); then the
    host is asked to START again with the corruption gone.

    Asserts, round 1: the failing joiner emits status failed and NO start; the
    healthy joiner (verified its copy) gets NO start either; the host emits
    the 'save transfer failed for <name> -- press START GAME to retry' status
    and NO start -- all well inside PEER_XFER_TIMEOUT, which proves the host
    honoured the receiver's base REWIND on each re-request.
    Round 2 (retry): both joiners save_ready + start(save=true), host start.
    """
    HP, P1, P2 = 29520, 29521, 29522
    names = {"host": "alice", "j1": "bob", "j2": "carol"}
    base = tempfile.mkdtemp(prefix="lobby_xferfail_")
    dirs = {k: os.path.join(base, k) for k in names}
    ios = {k: LobbyIO(dirs[k]) for k in names}
    stop = threading.Event()
    conns = []
    corrupt = [True]                          # flipped off before the retry

    class _CorruptingReceiver(_ClientSaveReceiver):
        """Zeroes chunk 0 while ``corrupt`` is set -> per-file SHA mismatch."""
        def on_chunk(self, sid, seq, data):
            if seq == 0 and corrupt[0]:
                data = bytes(len(data))
            super().on_chunk(sid, seq, data)

    save_path = os.path.join(base, "world.sav")
    with open(save_path, "wb") as f:
        f.write(os.urandom(size_bytes))
    src_sha = _sha256_file(save_path)

    ok = True
    t0 = time.time()
    try:
        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="xferfail-host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "XFERFAIL", "stop": stop},
                         daemon=True).start()
        time.sleep(0.4)

        for port, key, cls in ((P1, "j1", _ClientSaveReceiver),
                               (P2, "j2", _CorruptingReceiver)):
            conn = _dial_loopback(port, HP, 12)
            if not conn:
                print(f"[xfer:{tag}] FAIL: joiner {names[key]} could not connect")
                return False
            conns.append(conn)
            threading.Thread(target=run_client, name=key,
                             args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop, "receiver_cls": cls},
                             daemon=True).start()

        def all_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or len(r.get("players", [])) < 3:
                    return False
            return True

        if not _wait_until(all_joined, timeout=15):
            print(f"[xfer:{tag}] FAIL: not all three joined")
            return False

        def host_failed_status():
            return [e for e in _read_events(ios["host"].out_path)
                    if e.get("type") == "status" and e.get("state") == "failed"
                    and "press START GAME to retry" in e.get("detail", "")]

        # ---- round 1: carol's receiver cannot verify -> nobody starts ------ #
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start", "save": save_path}) + "\n")

        # Must resolve WELL before PEER_XFER_TIMEOUT (30 s): each re-request
        # only completes because the host rewinds its cursor to the new base.
        if not _wait_until(host_failed_status, timeout=PEER_XFER_TIMEOUT - 10):
            print(f"[xfer:{tag}] FAIL: host never emitted the retry status "
                  f"(receiver rewind not honoured?)")
            ok = False
        else:
            detail = host_failed_status()[-1]["detail"]
            if detail != ("save transfer failed for carol -- press START GAME "
                          "to retry"):
                print(f"[xfer:{tag}] FAIL: unexpected failed detail: {detail!r}")
                ok = False
        time.sleep(0.5)                       # let any stray start land (none may)
        if _has_start(ios["host"].out_path):
            print(f"[xfer:{tag}] FAIL: host broadcast start despite a failure")
            ok = False
        for k in ("j1", "j2"):
            if _has_start(ios[k].out_path):
                print(f"[xfer:{tag}] FAIL: {k} started despite a failed transfer")
                ok = False
        if not any(e.get("type") == "status" and e.get("state") == "failed"
                   and "hash mismatch" in e.get("detail", "")
                   for e in _read_events(ios["j2"].out_path)):
            print(f"[xfer:{tag}] FAIL: j2 never reported the hash mismatch")
            ok = False
        if any(e.get("type") == "save_ready"
               for e in _read_events(ios["j2"].out_path)):
            print(f"[xfer:{tag}] FAIL: j2 emitted save_ready for a bad file")
            ok = False
        if not any(e.get("type") == "save_ready"
                   for e in _read_events(ios["j1"].out_path)):
            print(f"[xfer:{tag}] FAIL: j1 (healthy) never emitted save_ready")
            ok = False
        if ok:
            print(f"[xfer:{tag}] round 1 OK: failure surfaced, nobody started "
                  f"({time.time() - t0:.1f}s)")

        # ---- round 2: fix carol, press START GAME again -> all start ------- #
        corrupt[0] = False
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start", "save": save_path}) + "\n")

        def retry_settled():
            return (_has_start(ios["host"].out_path, save=True)
                    and all(_has_start(ios[k].out_path, save=True)
                            for k in ("j1", "j2")))

        if not _wait_until(retry_settled, timeout=30):
            print(f"[xfer:{tag}] FAIL: retry did not start everyone")
            for k in names:
                print(f"[xfer:{tag}]   {k} start(save=true): "
                      f"{_has_start(ios[k].out_path, save=True)}")
            ok = False
        for k in ("j1", "j2"):
            p = os.path.join(ios[k].dir, INCOMING_BASENAME + ".sav")
            if not os.path.isfile(p) or _sha256_file(p) != src_sha:
                print(f"[xfer:{tag}] FAIL: {k} incoming_save.sav wrong after retry")
                ok = False
    finally:
        stop.set()
        time.sleep(0.5)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        shutil.rmtree(base, ignore_errors=True)

    print(f"[xfer:{tag}] {'OK' if ok else 'FAIL'}  "
          f"(failed peer -> retry status, then successful retry, "
          f"{time.time() - t0:.1f}s)")
    return ok


def _run_transfer_mods(tag):
    """Host + 2 joiners over loopback: the save 'needs' two mods, each joiner
    has one of them, the host has both. Asserts that each joiner asked for the
    one it lacked, received it after the save, unpacked it under its own mods
    folder, told the panel (mods_ready), and was started only after that."""
    HP, P1, P2 = 29530, 29531, 29532
    names = {"host": "alice", "j1": "bob", "j2": "carol"}
    base = tempfile.mkdtemp(prefix="lobby_mods_")
    dirs = {k: os.path.join(base, k) for k in names}
    ios = {k: LobbyIO(dirs[k]) for k in names}
    stop = threading.Event()
    conns = []
    save_path = os.path.join(base, "world.sav")
    with open(save_path, "wb") as f:
        f.write(os.urandom(256 * 1024))
    with open(save_path + ".lua", "wb") as f:
        f.write(b'["lockstep.lua"] = { }\n')
    # the host's two mod folders
    src = {}
    for mid in ("mod_zz", "mod_have"):
        d = os.path.join(base, "hostmods", f"{mid}_1")
        os.makedirs(os.path.join(d, "res", "scripts"))
        with open(os.path.join(d, "mod.lua"), "w") as f:
            f.write(f"-- {mid}\nfunction data() return {{}} end\n")
        with open(os.path.join(d, "res", "scripts", "thing.lua"), "w") as f:
            f.write("return " + repr(mid) + "\n")
        src[mid] = d
    dest = os.path.join(base, "joinermods")
    registry_real=(modshare.request_catalogue,modshare.catalogue,modshare.write_registry)
    modshare.request_catalogue=lambda extra=None: "test-catalogue"
    modshare.write_registry=lambda token=None, extra=None: "test-catalogue"
    cache_real, modshare.mod_zip_cache_dir = modshare.mod_zip_cache_dir, (lambda: os.path.join(base, "zipcache"))
    modshare.catalogue=lambda: ("test-catalogue",{("mod_zz","1"),("mod_have","1")})
    real = (modshare.save_mod_list, modshare.find_mod, modshare.installed_mod, modshare.install_target)
    on_disk_real, modshare.on_disk_mod = modshare.on_disk_mod, lambda m, v: src.get(m) if m == "mod_have" else None
    share_was, SHARE_MODS[0] = SHARE_MODS[0], True          # off by default; this test is the round itself
    modshare.save_mod_list = lambda p, log=None: [("mod_zz", 1), ("mod_have", 1)]
    modshare.find_mod = lambda m, v: src.get(m)                       # the host has both
    modshare.installed_mod = lambda m, v: src.get(m) if m == "mod_have" else None   # joiners lack mod_zz
    # a joiner is its loop thread; its verify/write worker is "<joiner>/save-finalize"
    modshare.install_target = lambda m, v: os.path.join(dest, threading.current_thread().name.split("/")[0], f"{m}_{v}")
    ok = True
    t0 = time.time()
    try:
        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="mods-host", args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "MODSCODE", "stop": stop}, daemon=True).start()
        time.sleep(0.4)
        for port, key in ((P1, "j1"), (P2, "j2")):
            peer = {"candidates": {"public_v4": f"127.0.0.1:{HP}", "lan_v4": None, "v6": None},
                    "flags": {"open": True, "v6": False}}
            s = open_socket(port, socket.AF_INET)
            conn = race(s, peer, "dial", port, 12, my_has_v6=False)
            if not conn:
                print(f"[mods:{tag}] FAIL: joiner {names[key]} could not connect")
                return False
            conns.append(conn)
            threading.Thread(target=run_client, name=key, args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop}, daemon=True).start()

        def all_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or len(r.get("players", [])) < 3:
                    return False
            return True
        if not _wait_until(all_joined, timeout=15):
            print(f"[mods:{tag}] FAIL: not all three joined")
            return False
        with open(ios["host"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "start", "save": save_path}) + "\n")

        answered = set()

        def settled():
            # the panel would show YES / NO on each joiner; press YES on every one
            # that has asked BEFORE checking anyone's progress (the host waits for
            # all answers, so answering one at a time would stall it)
            for k in ("j1", "j2"):
                if k not in answered and any(e.get("type") == "mods_prompt" for e in _read_events(ios[k].out_path)):
                    answered.add(k)
                    with open(ios[k].in_path, "a", encoding="utf-8") as f:
                        f.write(json.dumps({"cmd": "mods", "accept": True}) + "\n")
            for k in ("j1", "j2"):
                ev = _read_events(ios[k].out_path)
                if not any(e.get("type") == "mods_ready" for e in ev):
                    return False
                if not _has_start(ios[k].out_path, save=True):
                    return False
            return any(e.get("type") == "start" for e in _read_events(ios["host"].out_path))
        if not _wait_until(settled, timeout=60):
            print(f"[mods:{tag}] FAIL: mods round did not settle in time")
            ok = False
        for k in ("j1", "j2"):
            ev = _read_events(ios[k].out_path)
            pr = [e for e in ev if e.get("type") == "mods_prompt"]
            if not pr or pr[-1].get("mods") != ["mod_zz_1"]:
                print(f"[mods:{tag}] FAIL: {k} prompt = {pr[-1] if pr else None}")
                ok = False
            mr = [e for e in ev if e.get("type") == "mods_ready"]
            if not mr or mr[-1].get("installed") != ["mod_zz_1"] or mr[-1].get("failed"):
                print(f"[mods:{tag}] FAIL: {k} mods_ready = {mr[-1] if mr else None}")
                ok = False
            got = os.path.join(dest, k, "mod_zz_1")
            if not os.path.isfile(os.path.join(got, "mod.lua")) or not os.path.isfile(os.path.join(got, "res", "scripts", "thing.lua")):
                print(f"[mods:{tag}] FAIL: {k} did not unpack mod_zz_1 into {got}")
                ok = False
            if os.path.isdir(os.path.join(dest, k, "mod_have_1")):
                print(f"[mods:{tag}] FAIL: {k} received mod_have although it had it")
                ok = False
            # started only after the mods landed
            idx_ready = max(i for i, e in enumerate(ev) if e.get("type") == "mods_ready") if mr else -1
            idx_start = [i for i, e in enumerate(ev) if e.get("type") == "start"]
            if idx_start and idx_ready >= 0 and idx_start[-1] < idx_ready:
                print(f"[mods:{tag}] FAIL: {k} was started before its mods arrived")
                ok = False
        lateio=LobbyIO(os.path.join(base,"late"))
        lateSock=open_socket(29533,socket.AF_INET)
        late=race(lateSock,peer,"dial",29533,12,my_has_v6=False)
        if not late:
            ok=False
        else:
            conns.append(late)
            threading.Thread(target=run_client,name="late",args=(late,"dave",lateio),kwargs={"stop":stop},daemon=True).start()
            answered_late=[0]
            def late_settled():
                events=_read_events(lateio.out_path)
                prompts=sum(e.get("type")=="mods_prompt" for e in events)
                if prompts>answered_late[0]:
                    answered_late[0]=prompts
                    with open(lateio.in_path,"a",encoding="utf-8") as f: f.write(json.dumps({"cmd":"mods","accept":True})+"\n")
                return _has_start(lateio.out_path,save=True) and any(e.get("type")=="mods_ready" for e in events)
            if not _wait_until(late_settled,timeout=30):
                print("[mods] FAIL: hotjoin mods did not complete before start")
                ok=False
            else:
                print("[mods] OK: hotjoin downloaded and registered required mods before start")
        # the round is announced per batch in the joiners' chat ("mod batch 1/1; Installed
        # from the host: ..."); the host's "Sharing N mod(s)" chat went with batching
        jchat = [e.get("text", "") for k in ("j1", "j2") for e in _read_events(ios[k].out_path) if e.get("type") == "chat"]
        if not any("mod batch 1/1" in t and "Installed from the host" in t for t in jchat):
            print(f"[mods:{tag}] FAIL: no joiner reported an installed mod batch: {jchat}")
            ok = False
    finally:
        stop.set()
        time.sleep(0.5)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        (modshare.save_mod_list, modshare.find_mod, modshare.installed_mod, modshare.install_target) = real
        modshare.on_disk_mod = on_disk_real
        modshare.request_catalogue,modshare.catalogue,modshare.write_registry=registry_real
        modshare.mod_zip_cache_dir = cache_real
        SHARE_MODS[0] = share_was
        shutil.rmtree(base, ignore_errors=True)
    print(f"[mods:{tag}] {'OK' if ok else 'FAIL'}  ({time.time() - t0:.1f}s)")
    return ok


def selftest_mods():
    """The mods round: a joiner lacking a mod the save needs gets it from the
    host after the save, unpacked, before it is started."""
    print("[selftest-mods] host -> 2-joiner mod share")
    ok = modshare_selftest_ok() and _run_mods_gate("gate") and _run_transfer_mods("share-one")
    # and the real thing: the newest save on this machine must parse (proves the
    # zstandard decoder is bundled in a frozen build)
    ud = modshare.userdata_mods_dir()
    sd = ud and os.path.join(os.path.dirname(ud), "save")
    saves = sorted((os.path.join(sd, f) for f in os.listdir(sd) if f.lower().endswith(".sav")),
                   key=os.path.getmtime) if sd and os.path.isdir(sd) else []
    if saves:
        mods = modshare.save_mod_list(saves[-1])
        print(f"[selftest-mods] {os.path.basename(saves[-1])}: mods = {mods}")
        if mods is None:
            print("[selftest-mods] FAIL: could not read the mod list off a real save")
            ok = False
    else:
        print("[selftest-mods] (no local save to parse)")
    print(f"[selftest-mods] {'PASS' if ok else 'FAIL'}")
    return ok


def _run_mods_gate(tag):
    """The joiner-side gate: a mods round is installed only right after a
    verified save round, only after this player said yes, and only the mods
    they were asked about. Drives _ClientSaveReceiver directly, the way a host
    that skips the prompt would."""
    base = tempfile.mkdtemp(prefix="lobby_modgate_")
    dest = os.path.join(base, "mods")
    registry_request_real=(modshare.request_catalogue, modshare.write_registry)
    modshare.request_catalogue=lambda extra=None: "consent-test"
    modshare.write_registry=lambda token=None, extra=None: "consent-test"
    cache_real, modshare.mod_zip_cache_dir = modshare.mod_zip_cache_dir, (lambda: os.path.join(base, "zipcache"))
    real = (modshare.installed_mod, modshare.install_target)
    on_disk_real, modshare.on_disk_mod = modshare.on_disk_mod, lambda m, v: None
    modshare.installed_mod = lambda m, v: None
    modshare.install_target = lambda m, v: os.path.join(dest, f"{m}_{v}")
    zips = {}
    for mid in ("mod_zz", "evil"):
        d = os.path.join(base, "src", f"{mid}_1")
        os.makedirs(d)
        with open(os.path.join(d, "mod.lua"), "w") as f:
            f.write("function data() return {} end\n")
        zips[mid] = modshare.zip_mod(d)

    class Conn:
        def __init__(self):
            self.sent = []

        def send(self, raw):
            self.sent.append(json.loads(raw.decode("utf-8")))

    def push(r, sid, kind, files, mods=None):
        blob = b"".join(data for _, data in files)
        meta = [{"name": n, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()} for n, data in files]
        total = (len(blob) + CHUNK_LOCAL - 1) // CHUNK_LOCAL
        msg = {"t": "fbegin", "sid": sid, "kind": kind, "files": meta, "total_bytes": len(blob),
               "total_chunks": total, "chunk": CHUNK_LOCAL, "sha256": hashlib.sha256(blob).hexdigest()}
        if mods is not None:
            msg["mods"] = mods
        r.on_begin(msg)
        for seq in range(total):
            r.on_chunk(sid, seq, blob[seq * CHUNK_LOCAL:(seq + 1) * CHUNK_LOCAL])
        r.settle()                      # the verify/write runs on a worker thread; apply its outcome

    def installed(mid):
        return os.path.isfile(os.path.join(dest, f"{mid}_1", "mod.lua"))

    def receiver(name):
        c = Conn()
        return c, _ClientSaveReceiver(c, LobbyIO(os.path.join(base, name)), lambda s: None)

    save = [(INCOMING_BASENAME + ".sav", os.urandom(20000))]
    mzz = [(modshare.mod_zip_name("mod_zz", 1), zips["mod_zz"])]
    evil = [(modshare.mod_zip_name("evil", 1), zips["evil"])]
    results = []

    def check(name, cond):
        print(f"[mods:{tag}] {'ok  ' if cond else 'FAIL'} {name}")
        results.append(bool(cond))

    try:
        c, r = receiver("io1")
        push(r, 11, "mods", mzz)
        check("a mods round with no save round and no yes is refused and installs nothing",
              not installed("mod_zz") and not any(m.get("t") == "fbegin_ack" for m in c.sent)
              and any(m.get("t") == "fdone" and m.get("ok") is False for m in c.sent))

        c, r = receiver("io2")
        push(r, 21, "save", save, mods=[["mod_zz", 1]])
        check("the save round asks about the missing mod", r.save_done and r.offered == ["mod_zz_1"])
        r.answer_mods(False)
        push(r, 22, "mods", mzz)
        check("after NO the mods round is refused and installs nothing", not installed("mod_zz"))

        c, r = receiver("io3")
        push(r, 31, "save", save, mods=[["mod_zz", 1]])
        r.answer_mods(True)
        push(r, 32, "mods", mzz + evil)
        check("after YES the mod asked about is installed", installed("mod_zz"))
        check("a mod the player was not asked about is not installed", not installed("evil"))
        shutil.rmtree(os.path.join(dest, "mod_zz_1"), ignore_errors=True)
        push(r, 33, "mods", mzz)
        check("a second mods round on the same YES is refused", not installed("mod_zz"))

        c, r = receiver("io4")
        push(r, 41, "save", save + mzz, mods=[["mod_zz", 1]])
        check("a save round carrying a mod zip is refused", r.failed and not r.save_done and not installed("mod_zz"))

        c, r = receiver("io5")
        push(r, 51, "save", save, mods=[["mod_zz", 1]])
        r.answer_mods(True)
        push(r, 52, "mods", mzz + [(INCOMING_BASENAME + ".sav", b"x" * 10)])
        check("a mods round carrying a non-mod file is refused", not installed("mod_zz"))
    finally:
        modshare.request_catalogue, modshare.write_registry=registry_request_real
        modshare.mod_zip_cache_dir = cache_real
        modshare.installed_mod, modshare.install_target = real
        modshare.on_disk_mod = on_disk_real
        shutil.rmtree(base, ignore_errors=True)
    return all(results)


def modshare_selftest_ok():
    try:
        modshare.selftest()
        return True
    except AssertionError as e:
        print(f"[selftest-mods] modshare selftest FAILED: {e}")
        return False


def selftest_transfer():
    """Run the save-transfer self-test several times (clean + lossy) so it
    proves both correctness and non-flakiness, and exercises retransmit;
    then the failure + retry case."""
    print("[selftest-transfer] reliable host -> 2-joiner save transfer")
    runs = [
        ("clean-1", 0.00, 20 * 1024 * 1024, False, False),
        ("clean-2", 0.00, 16 * 1024 * 1024, False, False),
        ("lossy-8pct", 0.08, 12 * 1024 * 1024, False, False),
        ("lossy-15pct", 0.15, 10 * 1024 * 1024, False, False),
        # the TCP bulk channel: a bigger file so the rate means something, then the
        # same with every connect refused, which must fall back to UDP unnoticed
        ("tcp-64MB", 0.00, 64 * 1024 * 1024, True, False),
        ("tcp-unreachable-udp-fallback", 0.00, 8 * 1024 * 1024, True, True),
    ]
    allok = True
    for tag, loss, size, tcp, unreachable in runs:
        allok = _run_transfer_once(loss, size, tag, tcp=tcp, tcp_unreachable=unreachable) and allok
    allok = _run_transfer_failure("fail-retry") and allok
    print(f"[selftest-transfer] {'PASS' if allok else 'FAIL'}")
    return allok


# --------------------------------------------------------------------------- #
# Self-test: GAME RELAY -- two stand-in bridges exchange frames via host+joiner
# --------------------------------------------------------------------------- #
def _run_dual_round(tag, loss, delay, dual_on, n_frames=300, joiner_name="bob"):
    """Host + joiner on loopback with game relays, SEALED; the joiner's UDP
    sends impaired (netsim) once the lobby has formed; ``n_frames`` frames each
    way. With the TCP backup link every frame the joiner sends must reach the
    host (UDP loses ~loss of them, the TCP copies cover); with it off the loss
    shows. Prints the dual counters. Returns (ok, delivered fraction joiner->host)."""
    HP, P1 = 29540, 29541
    RELAY_HOST, RELAY_JOIN = 7793, 7794
    LOCAL_HOST, LOCAL_JOIN = 7791, 7792
    names = {"host": "alice", "j1": joiner_name}
    # the roster shows the name the host gave (a joiner asking for a taken name is renamed)
    expect_roster = sorted([names["host"], _dedupe(joiner_name, {names["host"]})])
    base = tempfile.mkdtemp(prefix="lobby_dual_")
    ios = {k: LobbyIO(os.path.join(base, k)) for k in names}
    if not dual_on:
        for k in names:
            with open(os.path.join(ios[k].dir, "tpf2mp_tcp_backup.txt"), "w") as f:
                f.write("0\n")
    stop = threading.Event()
    conns, relays, standins = [], [], []
    ok = True
    KEY = derive_key(b"selftest-secret!"[:SECRET_LEN], "pw")
    SEAL[0] = Sealer(KEY)

    def frame(t, i):
        head = t + struct.pack("!I", i)
        return head + bytes((i * 7 + j) & 0xFF for j in range(900 - len(head)))

    delivered = 0.0
    try:
        bridge_a = _open_loopback_udp(LOCAL_HOST)
        bridge_b = _open_loopback_udp(LOCAL_JOIN)
        standins += [bridge_a, bridge_b]
        relay_h = GameRelay(RELAY_HOST, LOCAL_HOST, log=lambda _: None)
        relay_j = GameRelay(RELAY_JOIN, LOCAL_JOIN, log=lambda _: None)
        relays += [relay_h, relay_j]
        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="dual-host", args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "DUALCODE", "stop": stop, "relay": relay_h}, daemon=True).start()
        time.sleep(0.4)
        conn = _dial_loopback(P1, HP, 10)
        if not conn:
            print(f"[dual:{tag}] FAIL: joiner could not connect")
            return False, 0.0
        conns.append(conn)
        conn.cipher = Sealer(KEY)
        sim = {"loss": 0.0, "delay": 0.0, "jitter": 0.0}      # impaired only once the lobby has formed
        conn.sock = netsim.ImpairedSocket(conn.sock, sim)
        impaired = conn.sock
        dsock = _start_dual_client(conn, names["j1"], lambda _: None, sim) if dual_on else None
        threading.Thread(target=run_client, name="dual-j1", args=(conn, names["j1"], ios["j1"]),
                         kwargs={"stop": stop, "relay": relay_j}, daemon=True).start()

        def both_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or sorted(r.get("players", [])) != expect_roster:
                    return False
            return True
        if not _wait_until(both_joined, timeout=12):
            print(f"[dual:{tag}] FAIL: lobby did not form")
            return False, 0.0
        if dual_on:
            if not _wait_until(lambda: dsock.has_link(conn.peer) and DUAL[0] is not None and len(DUAL[0].links) > 0, timeout=15):
                print(f"[dual:{tag}] FAIL: the TCP link did not come up (joiner {dsock.has_link(conn.peer)}, host {DUAL[0] and list(DUAL[0].links)})")
                return False, 0.0
        impaired.loss, impaired.delay = loss, delay
        if delay and not impaired._pump_started:
            impaired.start_pump()
        if dsock is not None:
            dsock.set_link_delay(delay)          # the TCP copies wait the same as the UDP ones
        for i in range(n_frames):
            bridge_a.sendto(frame(b"A", i), ("127.0.0.1", RELAY_HOST))
            bridge_b.sendto(frame(b"B", i), ("127.0.0.1", RELAY_JOIN))
            if i % 10 == 9:
                time.sleep(0.005)
        got = {b"A": set(), b"B": set()}
        deadline = time.time() + 8.0
        while time.time() < deadline and (len(got[b"A"]) < n_frames or len(got[b"B"]) < n_frames):
            try:
                ready, _, _ = select.select([bridge_a, bridge_b], [], [], 0.2)
            except (OSError, ValueError):
                break
            for s in ready:
                for _ in range(GAME_RELAY_DRAIN):
                    try:
                        data, _src = s.recvfrom(65535)
                    except (BlockingIOError, ConnectionResetError, OSError):
                        break
                    t = data[:1]
                    if t in got and len(data) >= 5 and data == frame(t, struct.unpack("!I", data[1:5])[0]):
                        got[t].add(struct.unpack("!I", data[1:5])[0])
        delivered = len(got[b"B"]) / n_frames
        print(f"[dual:{tag}] host->joiner {len(got[b'A'])}/{n_frames}, joiner->host {len(got[b'B'])}/{n_frames} "
              f"(joiner sends: {impaired.dropped} dropped, {impaired.delayed} delayed)")
        if len(got[b"A"]) != n_frames:
            ok = False
            print(f"[dual:{tag}] FAIL: host->joiner frames missing")
        if dual_on:
            host_dual = DUAL[0]
            time.sleep(dual_tcp.AGE_OUT + 0.5)   # a copy that never came is counted after the age-out
            host_dual.stats._last_log = 0.0
            host_dual.tick({a: "bob" for a in host_dual.links})
            for _a, p in host_dual.stats.peers.items():
                print(f"[dual:{tag}] host counters: udp_first={p['udp_first']} tcp_first={p['tcp_first']} "
                      f"tcp_only={p['tcp_only']} udp_only={p['udp_only']}; tcp later by {host_dual.stats._q(p['tcp_late'])}, "
                      f"udp later by {host_dual.stats._q(p['udp_late'])}")
            covered = sum(p["tcp_only"] for p in host_dual.stats.peers.values())
            if len(got[b"B"]) != n_frames:
                ok = False
                print(f"[dual:{tag}] FAIL: with the TCP link every joiner frame must arrive")
            if loss and covered < n_frames * loss * 0.5:
                ok = False
                print(f"[dual:{tag}] FAIL: expected the TCP copies to cover ~{loss:.0%} of {n_frames} frames, tcp_only={covered}")
        elif loss and delivered > 1.0 - loss * 0.5:
            ok = False
            print(f"[dual:{tag}] FAIL: with the link off, {loss:.0%} loss should show; delivered {delivered:.0%}")
    finally:
        stop.set()
        time.sleep(0.3)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        for r in relays:
            try:
                r.close()
            except Exception:
                pass
        for s in standins:
            try:
                s.close()
            except Exception:
                pass
        SEAL[0] = None
        shutil.rmtree(base, ignore_errors=True)
    return ok, delivered


def selftest_dual():
    """The TCP backup link: a joiner losing 30% of its UDP sends (50 ms delay)
    still gets every lockstep frame to the host; with the link off the loss shows."""
    print("[selftest-dual] every sealed frame on UDP and a TCP link; first copy wins")
    ok1, d1 = _run_dual_round("link-on-30pct-loss", 0.30, 0.05, True)
    time.sleep(0.5)
    ok2, d2 = _run_dual_round("link-off-30pct-loss", 0.30, 0.05, False)
    print(f"[selftest-dual] joiner->host delivered: link on {d1:.0%}, link off {d2:.0%}")
    allok = ok1 and ok2
    print(f"[selftest-dual] {'PASS' if allok else 'FAIL'}")
    return allok


def selftest_relay():
    """1 host + 1 joiner on loopback, each with a game relay; two plain UDP
    sockets stand in for the two bridges (bound to the --game-local-ports).

    (a) 200 numbered 900-byte frames go EACH way in through the relay ports and
        must all arrive, byte-exact, at the OTHER stand-in (reordering allowed;
        loopback loses nothing).
    (b) an oversize frame is dropped (counted + warned), never delivered.
    (c) lobby chat still flows after the relay traffic.
    (d) the relay counters agree with (a).
    """
    HP, P1 = 29530, 29531                       # lobby transport ports
    RELAY_HOST, RELAY_JOIN = 7783, 7784         # --game-relay-port (per role)
    LOCAL_HOST, LOCAL_JOIN = 7781, 7782         # --game-local-port (the bridges)
    N, SIZE = 200, 900
    names = {"host": "alice", "j1": "bob"}
    base = tempfile.mkdtemp(prefix="lobby_relay_")
    ios = {k: LobbyIO(os.path.join(base, k)) for k in names}
    stop = threading.Event()
    conns, relays, standins = [], [], []
    ok = True
    print(f"[selftest-relay] host udp/{HP}: relay {RELAY_HOST} -> bridge "
          f"{LOCAL_HOST};  joiner udp/{P1}: relay {RELAY_JOIN} -> bridge "
          f"{LOCAL_JOIN}")

    def frame(tag, i):
        head = tag + struct.pack("!I", i)
        return head + bytes((i * 7 + j) & 0xFF for j in range(SIZE - len(head)))

    try:
        # The two "bridges": plain UDP sockets on the local ports.
        bridge_a = _open_loopback_udp(LOCAL_HOST)     # host machine's bridge
        bridge_b = _open_loopback_udp(LOCAL_JOIN)     # joiner machine's bridge
        standins += [bridge_a, bridge_b]
        relay_h = GameRelay(RELAY_HOST, LOCAL_HOST)
        relay_j = GameRelay(RELAY_JOIN, LOCAL_JOIN)
        relays += [relay_h, relay_j]

        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="relay-host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "RELAYCODE", "stop": stop,
                                 "relay": relay_h}, daemon=True).start()
        time.sleep(0.4)
        conn = _dial_loopback(P1, HP, 10)
        if not conn:
            print("[selftest-relay] FAIL: joiner could not connect")
            return False
        conns.append(conn)
        threading.Thread(target=run_client, name="relay-j1",
                         args=(conn, names["j1"], ios["j1"]),
                         kwargs={"stop": stop, "relay": relay_j},
                         daemon=True).start()

        def both_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or sorted(r.get("players", [])) != sorted(names.values()):
                    return False
            return True

        if not _wait_until(both_joined, timeout=12):
            print("[selftest-relay] FAIL: lobby did not form")
            return False
        print("[selftest-relay] lobby formed; sending frames")

        # (a) Drain while transmitting, like the real bridge. Waiting until
        # all 200 packets were sent overflowed Linux's default receive queue
        # at 185 packets even though both relay delivery counters read 200.
        def send_frames():
            for i in range(N):
                bridge_a.sendto(frame(b"A", i), ("127.0.0.1", RELAY_HOST))
                bridge_b.sendto(frame(b"B", i), ("127.0.0.1", RELAY_JOIN))
                if i % 20 == 19:
                    time.sleep(0.002)
        sender = threading.Thread(target=send_frames, name="relay-test-sender", daemon=True)
        sender.start()

        got = {b"A": set(), b"B": set()}          # tag -> indices received
        bad = 0
        deadline = time.time() + 15.0
        while (time.time() < deadline
               and (len(got[b"A"]) < N or len(got[b"B"]) < N)):
            try:
                ready, _, _ = select.select([bridge_a, bridge_b], [], [], 0.2)
            except (OSError, ValueError):
                break
            for s in ready:
                for _ in range(GAME_RELAY_DRAIN):
                    try:
                        data, _src = s.recvfrom(65535)
                    except (BlockingIOError, ConnectionResetError, OSError):
                        break
                    tag = data[:1]
                    if tag not in got or len(data) < 5:
                        bad += 1
                        continue
                    idx = struct.unpack("!I", data[1:5])[0]
                    # A-frames left bridge_a and must land on bridge_b; B the
                    # other way. Anything else is misrouted or corrupt.
                    expect = bridge_b if tag == b"A" else bridge_a
                    if s is not expect or data != frame(tag, idx):
                        bad += 1
                        continue
                    got[tag].add(idx)

        sender.join(timeout=1)
        for tag, dst in ((b"A", "joiner"), (b"B", "host")):
            missing = sorted(set(range(N)) - got[tag])
            if missing:
                ok = False
                print(f"[selftest-relay] FAIL (a): {len(missing)}/{N} "
                      f"{tag.decode()} frames never reached the {dst}'s bridge "
                      f"(first missing {missing[:5]})")
            else:
                print(f"[selftest-relay] OK (a): all {N} {tag.decode()} frames "
                      f"reached the {dst}'s bridge")
        if bad:
            ok = False
            print(f"[selftest-relay] FAIL (a): {bad} corrupt/misrouted frames")

        # (b) an oversize frame is dropped, never delivered.
        bridge_a.sendto(b"X" * (GAME_RELAY_MAX + 100), ("127.0.0.1", RELAY_HOST))
        time.sleep(0.5)
        leaked = False
        while True:
            try:
                data, _src = bridge_b.recvfrom(65535)
            except (BlockingIOError, ConnectionResetError, OSError):
                break
            if data[:1] == b"X":
                leaked = True
        if leaked or relay_h.oversize != 1:
            ok = False
            print(f"[selftest-relay] FAIL (b): oversize frame leaked={leaked} "
                  f"oversize_count={relay_h.oversize}")
        else:
            print("[selftest-relay] OK (b): oversize frame dropped (warned)")

        # (c) the lobby still works around the relay traffic: chat from joiner.
        chat_text = "still chatting after relay"
        with open(ios["j1"].in_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"cmd": "chat", "text": chat_text}) + "\n")
        if not _wait_until(lambda: all(_has_chat(ios[k].out_path, chat_text)
                                       for k in names), timeout=10):
            ok = False
            print("[selftest-relay] FAIL (c): chat broken after relay traffic")
        else:
            print("[selftest-relay] OK (c): chat still flows")

        # (d) counters: each relay forwarded N and delivered N.
        counts = (relay_h.forwarded, relay_h.delivered,
                  relay_j.forwarded, relay_j.delivered)
        if counts != (N, N, N, N):
            ok = False
            print(f"[selftest-relay] FAIL (d): counters host(fwd,del)="
                  f"{counts[:2]} joiner(fwd,del)={counts[2:]} (want {N} each)")
        else:
            print(f"[selftest-relay] OK (d): counters host/joiner fwd={N} "
                  f"delivered={N}")
    finally:
        stop.set()
        time.sleep(0.5)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        for r in relays:
            r.close()                     # idempotent; the loops close too
        for s in standins:
            try:
                s.close()
            except OSError:
                pass
        shutil.rmtree(base, ignore_errors=True)

    print(f"[selftest-relay] {'PASS' if ok else 'FAIL'}")
    return ok


# --------------------------------------------------------------------------- #
# Self-test: host + 3 joiners; j1<->j2 punch directly, j3 is relay-only
# --------------------------------------------------------------------------- #
def selftest_mesh():
    """Every participant's bridge frames must reach every OTHER participant's
    bridge EXACTLY once. j1 and j2 carry loopback profiles, so they must end up
    with a direct link; j3 joins with no mesh (the old star), so everything to
    and from it is relayed through the host. Also proves the host still gets
    one copy (not a duplicate) of each joiner frame."""
    HP = 29570
    JP = {"j1": 29571, "j2": 29572, "j3": 29573}
    RELAY = {"host": 7793, "j1": 7794, "j2": 7795, "j3": 7796}   # bridge -> lobby
    LOCAL = {"host": 7783, "j1": 7784, "j2": 7785, "j3": 7786}   # lobby -> bridge
    names = {"host": "alice", "j1": "bob", "j2": "carol", "j3": "dave"}
    N, SIZE = 60, 600
    base = tempfile.mkdtemp(prefix="lobby_mesh_")
    ios = {k: LobbyIO(os.path.join(base, k)) for k in names}
    stop = threading.Event()
    conns, relays, standins, meshes = [], [], [], {}
    ok = True
    # the whole test runs SEALED: one process-wide sealer stands in for the key
    # every member would derive from the code
    KEY = derive_key(b"selftest-secret!"[:SECRET_LEN], "pw")
    SEAL[0] = Sealer(KEY)                # the host's instance (own replay window)

    def frame(tag, i):
        head = tag + struct.pack("!I", i)
        return head + bytes((i * 3 + j) & 0xFF for j in range(SIZE - len(head)))

    try:
        bridges = {k: _open_loopback_udp(LOCAL[k]) for k in names}
        standins += list(bridges.values())
        rel = {k: GameRelay(RELAY[k], LOCAL[k]) for k in names}
        relays += list(rel.values())

        hsock = open_socket(HP, socket.AF_INET)
        threading.Thread(target=run_host, name="mesh-host",
                         args=(hsock, names["host"], ios["host"]),
                         kwargs={"code": "MESHCODE", "stop": stop,
                                 "relay": rel["host"]}, daemon=True).start()
        time.sleep(0.4)

        for key in ("j1", "j2", "j3"):
            port = JP[key]
            peer = {"candidates": {"public_v4": f"127.0.0.1:{HP}",
                                   "lan_v4": None, "v6": None},
                    "flags": {"open": True, "v6": False}}
            s = open_socket(port, socket.AF_INET)
            conn = race(s, peer, "dial", port, 10, my_has_v6=False)
            if not conn:
                print(f"[selftest-mesh] FAIL: {names[key]} could not connect")
                return False
            conns.append(conn)
            conn.cipher = Sealer(KEY)    # one instance per member, like real processes
            mesh = None
            code = None
            if key != "j3":
                code = encode_profile({"candidates": {"lan_v4": f"127.0.0.1:{port}",
                                                      "public_v4": None, "v6": None},
                                       "flags": {"open": True}})
                mesh, conn = _mesh_from_conn(conn, log=lambda m, k=key: print(f"  [{k}] {m}"))
                meshes[key] = mesh
            threading.Thread(target=run_client, name=f"mesh-{key}",
                             args=(conn, names[key], ios[key]),
                             kwargs={"stop": stop, "relay": rel[key],
                                     "mesh": mesh, "profile_code": code},
                             daemon=True).start()

        def all_joined():
            for k in names:
                r = _latest_roster(ios[k].out_path)
                if not r or sorted(r.get("players", [])) != sorted(names.values()):
                    return False
            return True

        if not _wait_until(all_joined, timeout=15):
            print("[selftest-mesh] FAIL: lobby did not form")
            return False

        # (a) j1 <-> j2 must become a direct link; j3 has none
        def linked():
            return (meshes["j1"].by_name(names["j2"]) is not None
                    and meshes["j2"].by_name(names["j1"]) is not None)
        if not _wait_until(linked, timeout=12):
            ok = False
            print("[selftest-mesh] FAIL (a): j1<->j2 did not link directly")
        else:
            print("[selftest-mesh] OK (a): j1<->j2 direct link on their own sockets")
        time.sleep(1.0)                      # let 'links' reports reach the roster

        # (b) frames from every bridge reach every other bridge exactly once
        tags = {"host": b"H", "j1": b"1", "j2": b"2", "j3": b"3"}
        for i in range(N):
            for k in names:
                bridges[k].sendto(frame(tags[k], i), ("127.0.0.1", RELAY[k]))
            if i % 10 == 9:
                time.sleep(0.003)
        got = {k: {t: collections.Counter() for t in tags.values()} for k in names}
        deadline = time.time() + 15.0
        def complete():
            for k in names:
                for src, t in tags.items():
                    if src != k and len(got[k][t]) < N:
                        return False
            return True
        while time.time() < deadline and not complete():
            try:
                ready, _, _ = select.select(list(bridges.values()), [], [], 0.2)
            except (OSError, ValueError):
                break
            for s in ready:
                k = next(kk for kk, ss in bridges.items() if ss is s)
                for _ in range(GAME_RELAY_DRAIN):
                    try:
                        data, _src = s.recvfrom(65535)
                    except (BlockingIOError, ConnectionResetError, OSError):
                        break
                    t = data[:1]
                    if t in got[k] and len(data) >= 5:
                        got[k][t][struct.unpack("!I", data[1:5])[0]] += 1
        time.sleep(0.5)                      # catch late duplicates, if any
        for s in bridges.values():
            while True:
                try:
                    data, _src = s.recvfrom(65535)
                except (BlockingIOError, ConnectionResetError, OSError):
                    break
                k = next(kk for kk, ss in bridges.items() if ss is s)
                t = data[:1]
                if t in got[k] and len(data) >= 5:
                    got[k][t][struct.unpack("!I", data[1:5])[0]] += 1
        for k in names:
            for src, t in tags.items():
                if src == k:
                    if got[k][t]:
                        ok = False
                        print(f"[selftest-mesh] FAIL (b): {k} received its own frames")
                    continue
                c = got[k][t]
                missing = N - len(c)
                dups = sum(v - 1 for v in c.values() if v > 1)
                if missing or dups:
                    ok = False
                    print(f"[selftest-mesh] FAIL (b): {src}->{k}: missing={missing} duplicates={dups}")
        if ok:
            print(f"[selftest-mesh] OK (b): {N} frames from each of 4 bridges reached "
                  f"the other 3 exactly once (direct j1<->j2, relayed j3)")
    finally:
        stop.set()
        time.sleep(0.5)
        for c in conns:
            try:
                c.close()
            except Exception:
                pass
        for r in relays:
            r.close()
        for s in standins:
            try:
                s.close()
            except OSError:
                pass
        shutil.rmtree(base, ignore_errors=True)
        SEAL[0] = None
    print(f"[selftest-mesh] {'PASS' if ok else 'FAIL'}"
          + (" (all frames sealed)" if ok else ""))
    return ok


# --------------------------------------------------------------------------- #
# Linux: the process around the lobby
# --------------------------------------------------------------------------- #
# On Windows the menu DLL starts netpunch.exe inside a kill-on-close Job object
# and reads the public list with WinHTTP. On Linux the game's menu library starts
# this program itself (docs/linux/NETPUNCH.md), so three things live here:
#   * --print-public-list URL: the server browser's GET <master>/list;
#   * SIGTERM leaves like a quit (leave / bye), so the launcher can stop us;
#   * --parent-pid PID: leave once the game has exited, crashed or not. A dead
#     game sends no quit, and PR_SET_PDEATHSIG fires when the THREAD that
#     started us ends, not the game.
PUBLIC_LIST_TIMEOUT = 5.0       # s, the Windows panel's WinHTTP timeouts
PARENT_POLL = 1.0               # s between looks at --parent-pid

# True once the lobby is leaving on its own: a quit command, or the finally: of
# run_host / run_client / cmd_host. The launcher follows its quit with SIGTERM
# after a fixed wait (lobby_linux.cpp: 1.5 s), and the host's cleanup after
# run_host -- publisher.close() waiting for /leave (up to 6 s), the UPnP unmap
# (a blocking discover) -- can take longer; a KeyboardInterrupt there would skip
# /leave and the unmap. Only _on_sigterm reads it.
_STOPPING = [False]


def print_public_list(master_url, timeout=PUBLIC_LIST_TIMEOUT):
    """GET <master_url>/list and print the body; 0 on HTTP 200. Anything else
    prints one line on stdout -- ``HTTP <code>`` or the error -- and returns 1:
    the two outcomes httpGet() in menu_hook.cpp tells apart. The answer goes to
    stdout only. A reader that stops reading early gets exit status 1 and no
    traceback on stderr."""
    import http.client
    import urllib.error
    import urllib.request
    url = str(master_url).rstrip("/") + "/list"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "tpf2mp-lobby/" + LOBBY_VERSION})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            status, body = r.status, r.read()
        if status == 200:
            rc, out = 0, body if body.endswith(b"\n") else body + b"\n"
        else:
            rc, out = 1, f"HTTP {status}\n".encode("ascii")
    except urllib.error.HTTPError as e:
        rc, out = 1, f"HTTP {e.code}\n".encode("ascii")
    except (urllib.error.URLError, http.client.HTTPException, OSError, ValueError) as e:
        detail = getattr(e, "reason", None) or e
        line = " ".join(str(detail).split()) or type(e).__name__
        rc, out = 1, (line + "\n").encode("utf-8", "replace")
    if sys.stdout is None:
        return rc
    try:
        sys.stdout.flush()
        sys.stdout.buffer.write(out)
        sys.stdout.flush()
    except BrokenPipeError:
        # The reader has gone (a 500-row list is about 170 KB). Point stdout at
        # /dev/null, or the interpreter's own flush at exit reports the same
        # broken pipe on stderr.
        try:
            os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        except (OSError, ValueError):
            pass
        return 1
    return rc


def _on_sigterm(_signo, _frame):
    # Once: the launcher's group SIGTERM, PR_SET_PDEATHSIG and the --parent-pid
    # watch can all arrive, and a second KeyboardInterrupt landing in the first
    # one's cleanup would cut the leave / bye short. Not at all while the lobby
    # is already leaving (_STOPPING): that cleanup keeps running and gets the
    # launcher's whole SIGTERM wait before its SIGKILL.
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    if _STOPPING[0]:
        return
    raise KeyboardInterrupt


def _proc_identity(pid):
    """(start time, alive) of ``pid`` from /proc/<pid>/stat. The start time
    (field 22) and the pid together name one process, so a reused pid is not
    taken for the game; a zombie counts as gone."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as f:
            stat = f.read().decode("ascii", "replace")
    except OSError:
        return None, False
    fields = stat[stat.rfind(")") + 2:].split()      # the name may hold spaces
    if len(fields) < 20:
        return None, True
    return fields[19], fields[0] != "Z"


def _parent_alive(pid, start):
    if os.path.isdir("/proc/self"):
        now_start, alive = _proc_identity(pid)
        return alive and (start is None or now_start == start)
    try:                                              # no /proc: existence only
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except OSError:
        return True


def _watch_parent(pid, log=_log):
    """Leave once process ``pid`` has exited: SIGTERM to ourselves, so the
    KeyboardInterrupt paths that send leave / bye run. False when that process
    is not running to begin with. Not on Windows (os.kill ends a process there)."""
    start = _proc_identity(pid)[0] if os.path.isdir("/proc/self") else None
    if not _parent_alive(pid, start):
        return False

    def run():
        while _parent_alive(pid, start):
            time.sleep(PARENT_POLL)
        log(f"[lobby] the process that started us (pid {pid}) has exited -- leaving")
        try:
            os.kill(os.getpid(), signal.SIGTERM)
        except OSError:
            os._exit(1)

    threading.Thread(target=run, name="parent-watch", daemon=True).start()
    return True


def _run_mode(fn, args):
    """cmd_host / cmd_join. Outside Windows a SIGTERM (or --parent-pid) that
    lands before the lobby loop is up -- while observing or dialling -- ends
    the process with a log line and exit status 130 instead of a traceback.
    cmd_host has removed its UPnP mapping by the time this sees it."""
    if sys.platform == "win32":
        return fn(args)
    try:
        return fn(args)
    except KeyboardInterrupt:
        _log("[lobby] stopped before the lobby was up")
        return 130


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _time_left_text(seconds):
    """'<1 min', '7 min', '1 h 12 min' -- for a status line."""
    if seconds is None:
        return "?"
    m = int(seconds // 60 + (1 if seconds % 60 >= 30 else 0))
    if m < 1:
        return "<1 min"
    if m < 60:
        return f"{m} min"
    return f"{m // 60} h {m % 60} min"


def _mods_progress_text(job, in_flight, now):
    """The status line both panels show during a mods round: what the joiner
    has (bytes of files landed), the pace since the round began, and what is
    left at that pace. Before the first batch lands it is the packaging line
    (bytes, not a mod count: batches go smallest first, so the count runs far
    ahead of the batches and read as a stall). ``in_flight`` is True while a
    batch is still crossing, so it is not counted as delivered."""
    gb = job["bytes"] / (1024.0 ** 3)
    n = len(job["plan"]) if job.get("plan") else 0
    batch_bytes = job.get("batch_bytes") or []
    landed = max(0, job["taken"] - (1 if in_flight else 0))
    delivered = sum(batch_bytes[:landed])
    if n and delivered:
        # the pace over the last few landed batches (their bytes over the time
        # since the landing before them), not since the round began: the average
        # carried the packaging lead-in and one slow batch for minutes ("13 MB/s"
        # while batches were landing at 50+, 2026-09-20 02:16)
        log_ = job.get("landed") or []
        recent = log_[-MODS_RATE_WINDOW:]
        if recent:
            t0 = log_[-MODS_RATE_WINDOW - 1][0] if len(log_) > MODS_RATE_WINDOW else job["started"]
            rate = sum(b for _, b in recent) / max(1e-3, recent[-1][0] - t0)
        else:
            rate = delivered / max(1e-3, now - job["started"])
        left = (job["bytes"] - delivered) / rate if rate > 0 else None
        # short: the panel draws this beside the buttons at the bottom, ~50 characters
        # wide on the joiner and less on the host, where the joiner's name leads
        return (f"Mods {delivered / (1024.0 ** 3):.1f}/{gb:.1f} GB, "
                f"{rate / (1024.0 ** 2):.0f} MB/s, {_time_left_text(left)} left, batch {landed}/{n}")
    sizes = job.get("sizes") or []
    packed_gb = sum(sizes[:min(job["done"], len(sizes))]) / (1024.0 ** 3)
    if gb:
        return (f"Packaging mods {packed_gb:.1f}/{gb:.1f} GB"
                + (f", batch {job['taken']}/{n} sent" if n else ""))
    return (f"Packaging mods {job['done']}/{len(job['wanted'])}"
            + (f", batch {job['taken']}/{n} sent" if n else ""))


def _workshop_rows(mods, lookup):
    """[(workshop id, folder)] for the '*' mods of ``mods`` that ``lookup`` finds on disk."""
    rows = []
    for m, v in mods:
        if isinstance(m, str) and m.startswith("*") and modshare.valid_mod(m, v):
            folder = lookup(m, v)
            if folder:
                rows.append((m[1:], folder))
    return rows


def _publish_registry_at_start(log):
    """Rewrite the Workshop registry from what is on disk now, keeping its
    token, so the NEXT game start registers every consented download even if
    the round that fetched it never reached its last batch."""
    try:
        # no save yet: register nothing beyond it; the save's own mods are
        # scoped in when its mod list arrives (_publish_rows / start(save))
        modshare.set_registry_scope(())
        modshare.write_registry()
        rows = modshare.read_registry()[1]
        if rows:
            log(f"[mods] registry published: {len(rows)} Workshop folder(s) for the game's next catalogue refresh")
    except (OSError, ValueError) as e:
        log(f"[mods] could not publish the mod registry: {e}")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    ap = argparse.ArgumentParser(description="netpunch N-player lobby")
    ap.add_argument("mode", nargs="?", choices=["host", "join"],
                    help="host a lobby or join one with a CODE")
    ap.add_argument("code", nargs="?", help="peer CODE (join mode)")
    ap.add_argument("--sync-runtime-dir")
    ap.add_argument("--save-dir")
    ap.add_argument("--game-pid", type=int)
    ap.add_argument("--name", default="player", help="your username")
    ap.add_argument("--local-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=int, default=40,
                    help="seconds to keep dialing the host (join)")
    ap.add_argument("--io-dir", default=None,
                    help="directory for the lobby_*.json[l] files (default: cwd)")
    ap.add_argument("--selftest", action="store_true",
                    help="run the loopback 1-host + 2-joiner self-test and exit")
    ap.add_argument("--selftest-mods", action="store_true",
                    help="self-test: the mods round of the save transfer over loopback")
    ap.add_argument("--selftest-transfer", action="store_true",
                    help="run the reliable save-transfer self-test (clean + "
                         "lossy) and exit")
    ap.add_argument("--relay-only", action="store_true",
                    help="host without a game (a dedicated relay): the oldest joiner leads")
    ap.add_argument("--publish", default="",
                    help="master server base URL; the lobby is listed there while "
                         "public (see --public and the 'publish' command)")
    ap.add_argument("--crossplay", action="store_true",
                    help="host: show and list the classic code, so players without Steam can join too "
                         "(default: a game with Steam shows its Steam ID and only Steam players get in)")
    ap.add_argument("--companies", action="store_true",
                    help="start in separate-companies mode: every player gets their own company (default: co-op, one company)")
    ap.add_argument("--dedicated", action="store_true",
                    help="a game that hosts by itself (tpf2_menu_flags.txt dedicated=1): keep the session secret "
                         "across restarts so the code stays valid, and list as a dedicated server")
    ap.add_argument("--public", action="store_true",
                    help="start listed publicly (host only)")
    ap.add_argument("--rendezvous", default="",
                    help="master server used for hole punching to the host (default: "
                         "--publish, else " + DEFAULT_MASTER + "; 'off' disables)")
    ap.add_argument("--lobby-name", default="",
                    help="what the lobby is called (shown to joiners and in the public list)")
    ap.add_argument("--password", default="",
                    help="optional lobby password: mixed into the session key, "
                         "so everyone must enter the same one (host + joiners)")
    ap.add_argument("--forward-log", action="append", default=None,
                    metavar="PATH",
                    help="also tail this file and ship its new lines to the "
                         "host's merged lobby_peers.log (repeatable; e.g. the "
                         "bridge log)")
    ap.add_argument("--share-mods", action="store_true",
                    help="host: send joiners the mods the shared save needs (off by default)")
    ap.add_argument("--no-share-mods", action="store_true",
                    help="host: do not send joiners the mods the shared save needs (the default)")
    ap.add_argument("--no-mesh", action="store_true",
                    help="joiner: do not punch other joiners directly; keep "
                         "every frame on the host relay (the pre-mesh star)")
    ap.add_argument("--selftest-mesh", action="store_true",
                    help="run the mesh self-test (host + 3 joiners, two of "
                         "them directly linked, one relay-only) and exit")
    ap.add_argument("--selftest-dual", action="store_true",
                    help="run the TCP backup-link self-test (a joiner losing 30%% of its UDP "
                         "sends still delivers every frame; the link off shows the loss)")
    ap.add_argument("--selftest-relay", action="store_true",
                    help="run the game-relay self-test (host + joiner, two "
                         "stand-in bridges, frames both ways) and exit")
    ap.add_argument("--game-relay-port", type=int, default=None,
                    help="bind 127.0.0.1:<port> and relay the lockstep "
                         "bridge's UDP frames over the lobby transport "
                         "(the menu passes 7773 for host, 7774 for join)")
    ap.add_argument("--game-local-port", type=int,
                    default=GAME_LOCAL_PORT_DEFAULT,
                    help="the local bridge's UDP port that relayed frames are "
                         "delivered to (the menu reads it from "
                         "tpf2_instance.txt; default %(default)s)")
    ap.add_argument("--print-public-list", metavar="MASTER_URL", default=None,
                    help="GET MASTER_URL/list, print the body and exit 0 on HTTP "
                         "200; otherwise print 'HTTP <code>' or the error and exit 1")
    ap.add_argument("--parent-pid", type=int, default=0, metavar="PID",
                    help="not on Windows: leave the lobby once process PID (the "
                         "game that started it) has exited")
    args = ap.parse_args(argv)
    SHARE_MODS[0] = not getattr(args, "no_share_mods", False)

    ca_bundle = None
    if sys.platform != "win32":
        import linuxpaths
        ca_bundle = linuxpaths.ssl_cert_fallback()     # before the first HTTPS request
    if args.print_public_list:
        return print_public_list(args.print_public_list)
    if args.mode in ("host", "join") and sys.platform != "win32":
        if ca_bundle:
            _log(f"[lobby] no CA certificates where this build's OpenSSL looks; using {ca_bundle}")
        signal.signal(signal.SIGTERM, _on_sigterm)
        if args.parent_pid > 0 and not _watch_parent(args.parent_pid):
            _log(f"[lobby] --parent-pid {args.parent_pid} is not running -- not starting")
            return 1

    if args.selftest:
        return 0 if selftest() else 1
    if args.selftest_transfer:
        return 0 if selftest_transfer() else 1
    if args.selftest_mods:
        return 0 if selftest_mods() else 1
    if args.selftest_relay:
        return 0 if selftest_relay() else 1
    if args.selftest_dual:
        return 0 if selftest_dual() else 1
    if args.selftest_mesh:
        return 0 if selftest_mesh() else 1
    if args.mode == "host":
        return _run_mode(cmd_host, args)
    if args.mode == "join":
        if not args.code:
            ap.error("join requires a CODE argument")
        return _run_mode(cmd_join, args)
    ap.error("give a mode: host / join / --selftest / --selftest-transfer / "
             "--selftest-relay")
    return 2


if __name__ == "__main__":
    sys.exit(main())
