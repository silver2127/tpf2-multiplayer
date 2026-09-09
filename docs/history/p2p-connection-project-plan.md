# Serverless P2P Game Connection — Project Plan

Goal: connect two friends' machines directly over the internet with no servers of your own — using candidate gathering, a human signaling channel (Discord codes), and a parallel connection race. Built in Python first for understanding, with an optional migration to aiortc at the end.

The author's environment (baked into the plan): a residential fibre connection with a real public IPv4, no CGNAT, no native IPv6 (6to4 works via a public relay), a consumer router with UPnP available, Windows.

---

## Phase 0 — Environment prep (1 evening)

- [ ] Python 3.11+ installed; create a project venv
- [ ] `pip install pystun3 miniupnpc` (miniupnpc may need the wheel; fallback is calling `upnpc.exe` CLI)
- [ ] Enable UPnP on the router if not already on
- [ ] Windows Defender: plan to allow Python/your script through the firewall on first run (it will prompt)
- [ ] Confirm baseline facts once: `ipconfig` shows your 2002: address; router status page shows public WAN IPv4; note both
- [ ] Recruit the friend. Have them check: router WAN IP vs whatsmyip result (mismatch = CGNAT, changes the plan for Phase 3 role-flip)

Deliverable: a repo with a venv and a NOTES.md recording both machines' connectivity profiles.

## Phase 1 — The punch engine (the core, ~1 weekend)

The 60-line heart: two UDP sockets, simultaneous-send retry loop, handshake.

- [ ] Single socket bound to a fixed port (e.g. 29471)
- [ ] `punch(target_ip, target_port)`: send a hello packet every 100 ms for 30 s; on receiving any packet, record its source address as "the peer" and switch to replying
- [ ] Tiny handshake protocol: HELLO → ACK → CONNECTED (both sides must see an ACK to declare success)
- [ ] Keepalive every 15 s after connection
- [ ] Peer address is always "most recent source address heard" (survives carrier NAT re-mapping)
- [ ] Manual test mode: run with IP:port passed on the command line

Test milestones:
1. Loopback test (two instances, 127.0.0.1, different ports) — proves the state machine
2. LAN test (two devices on your wifi) — proves real sockets
3. Internet test with the friend using manually exchanged router-WAN IPs + the fixed port (the port-preservation bet, or UPnP-mapped) — the first real connection. Do NOT test against your own public IP from inside your LAN (hairpinning false-failures).

Deliverable: `punch.py` that connects two machines across the internet with hand-typed addresses.

## Phase 2 — Observation: STUN + UPnP (~1 weekend)

Replace guesses with facts.

- [ ] STUN query from the game socket (same socket — the mapping is per-socket) against two servers from a hardcoded list: stun.l.google.com:19302, stun.cloudflare.com:3478, one spare
- [ ] Compare the two answers: same port → normal NAT; different ports → symmetric flag
- [ ] UPnP: query router WAN IP; request a mapping for the game port; record success/failure ("open" flag)
- [ ] CGNAT detection: UPnP WAN IP ≠ STUN answer → CGNAT flag
- [ ] IPv6 detection: enumerate global v6 addresses (native or 2002:) → v6 flag
- [ ] Re-run STUN at connect time, not just app start (mappings expire); keepalive to the STUN server keeps the socket warm while waiting for the friend's code
- [ ] Output: a "connectivity profile" struct — candidates (LAN v4, public v4:port, v6) + flags (open / symmetric / cgnat / v6)

Test milestones: your profile should read public-v4 correct, not symmetric, not CGNAT, v6 present (2002:), open=true if UPnP worked. Friend's profile tells you which Phase 3 branch their connection exercises.

Deliverable: `observe.py` printing the profile as JSON.

## Phase 3 — Exchange + race (~1 weekend)

The full ladder.

- [ ] Encode profile → shareable code: pack candidates + flags into bytes, base32 or wordlist-encode (~45 chars or ~10 words); include a timestamp for staleness warnings
- [ ] Decode incoming codes; warn if older than ~10 minutes
- [ ] The race: fire the Phase 1 punch loop at all candidate pairs in parallel — v6↔v6, LAN↔LAN, public v4↔public v4 — first pair to finish the handshake wins, others stop
- [ ] Role-flip rule: if one profile is CGNAT/symmetric and the other is open, skip punching — open side listens, constrained side dials (plain client-server)
- [ ] Wire into a two-command CLI: `host`/`join` or just `connect` (symmetric — both paste each other's codes)

Test milestones:
1. LAN candidates win instantly when both machines are on your wifi
2. Internet test with the friend: watch which pair wins (expect public v4 punch, or v6 if they enable 6to4)
3. Deliberately break the winner (disable UPnP, block a port) and confirm the race falls through to another rung

Deliverable: `connect.py` — paste codes, get a connected socket.

## Phase 4 — Diagnosis + polish (~a few evenings)

- [ ] Failure messages by name, from the flags: "no UDP out (network blocks it)", "both symmetric NATs", "both behind CGNAT — use Tailscale tonight", "code stale — re-exchange"
- [ ] STUN total-timeout detected in Phase 2 → report before generating a code at all
- [ ] Log every candidate pair's outcome (worked / timed out / refused) for post-mortems
- [ ] Optional: QR-code display of the connection code for phone-relay between machines

Deliverable: failures that teach instead of hang.

## Phase 5 — Make it a game transport (optional, ongoing)

- [ ] Sequence numbers + ack for the messages that need reliability; raw UDP for position updates
- [ ] A trivial demo app over the connection: chat, then pong
- [ ] Measure with your friend: iperf3-style jitter/loss over the established path

## Phase 6 — The aiortc off-ramp (optional, ~1 weekend)

When you want encryption and a decade of edge cases handled:

- [ ] Rebuild Phase 3 on aiortc data channels; signaling = your existing code-exchange (strip/compress the SDP to candidate essentials)
- [ ] Keep your Phase 2 UPnP step (aiortc won't do it) and your Phase 4 diagnosis (read ICE candidate-pair states)
- [ ] Compare: which candidate pair does ICE pick vs what your race picked?

You'll be configuring the ladder you already built — nothing from Phases 1–4 is wasted.

---

## Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Friend is on CGNAT | Medium | Phase 3 role-flip: you host (your side is open) |
| Friend on CGNAT AND symmetric | Low | Named failure + Tailscale for the night (Phase 4) |
| Port 29471 taken on a NAT | Low | STUN reports the real port anyway (Phase 2 makes this a non-issue) |
| UPnP off/broken on a router | Medium | Punch path doesn't need it; it's a bonus rung |
| 6to4 relay flakiness | Medium | Only affects v6↔internet; friend-to-friend 2002: traffic is relay-free; v4 rungs always present |
| Hamachi/Tailscale adapters hijacking routes | Medium (on your box) | Disable their adapters during testing; check first traceroute hop |
| The ISP lights up native v6 | Someday | Pluggable candidates: new address type drops into Phase 2, nothing else changes |

## Definition of done

Two machines on different residential networks, connected by pasting two codes over Discord, exchanging game packets directly — with zero infrastructure owned by you, and every failure mode that remains explainable by name.
