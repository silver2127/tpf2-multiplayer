# Security model

The threat model is a group of friends sharing a lobby code (in Discord, say), or a public
listing. The goal: an address, or a code seen by someone outside the group, is not enough
to read the group's traffic or inject game commands. Everyone inside the lobby is trusted.
The code is `netpunch/seal.py`, `netpunch/connect.py` and the checks in `netpunch/lobby.py`
and `native/src/menu_hook.cpp`.

## What is protected

- **The code is the credential.** A host's code carries a random 12-byte session secret.
  Every lobby message and game frame is sealed with a key derived from it and the lobby
  password, if one is set: `SHA-256("tpf2mp-seal-v1|" || secret || "|" || password)`.
- **Sealed frames** (`E`): an 8-byte nonce (a random per-process salt plus a counter), the
  ciphertext (a SHA-256 counter-mode keystream) and a 16-byte HMAC-SHA256 tag, encrypt-then-MAC
  with separate sub-keys. Each receiver keeps a 64-frame replay window per sender. In a sealed
  session plaintext is refused, except save chunks (below) and the host's "wrong password"
  reply.
- **A password locks the code.** It is typed in the panel's PASSWORD field by the host and
  every joiner. Besides entering the session key, it encrypts the code itself
  (PBKDF2-HMAC-SHA256 with 600,000 iterations, plus an HMAC tag), so a locked code reveals
  only when it was made; the host's address and the secret are inside. A locked public
  listing shows as `[locked]`.
- **Save transfers are verified.** Chunks travel unencrypted (a map is not a secret), but the
  SHA-256 of every file is in the sealed `fbegin` message. A receiver accepts only the three
  `incoming_save.*` names, refuses writes past the announced size, and writes nothing whose
  hashes do not match.
- **Inputs are sanitised before they become process arguments.** The pasted code must be
  base32; player names and passwords are limited to letters, digits and `-_.`; the logged
  command line masks the password.
- **The game's sockets answer only this PC.** The bridge DLL's UDP socket is bound to 127.0.0.1
  whenever its peer is on this PC, which every lobby session arranges, and it drops any datagram
  that does not come from its peer's address. The pre-lobby TCP save server is off unless
  `save_server=1`, and then answers only the peer address.
- **Construction settings from other players stay data.** The mod reads them with Lua's `load` in
  an empty environment, so a crafted string cannot reach `io` or `os`.
- **Log volume is capped.** A host accepts at most 8 forwarded-log messages per second, 64
  lines per message and 20 MiB per player into its merged log, with control characters
  stripped.
- **The UPnP port mapping is removed** when the host's lobby exits (best effort; see below).
- **A relay's secret** (`relay_secret.bin`) is written with mode 0600.

## What it does not do

- **The public game list is not vetted.** Anyone can announce a lobby under any name, including one
  that claims to be a dedicated server. Join only lobbies you trust: the host sends the save, and a
  save is code (below).
- **Lobby members are trusted equally.** Anyone in the lobby holds the session key and can
  send commands under any player's instance letter. Sealing stops outsiders, not a hostile
  player.
- **A public lobby without a password is open to anyone.** The master server lists the code,
  and an unlocked code contains the host's public IP address. Set a password to keep both to
  the people you give it to.
- **A password posted next to the code is no lock.** The key derivation makes guessing slow;
  it does not help if the password is in the same channel.
- **The relay operator can read everything in a relay lobby.** The relay created the secret,
  opens and re-seals every frame, and stores the uploaded world on disk.
- **A save is code.** The game executes a save's `.sav.lua` when it loads, so joining a game
  means trusting the host the way you would trust a mod author. This is inherent to the game.
- **No forward secrecy and no key rotation**; the key lives for the session. `seal.py` is a
  pragmatic construction from the Python standard library, not a reviewed AEAD.
- **Logs contain addresses and game traffic.** The lobby's and the bridge's own log lines mask IP
  addresses (set `TPF2MP_LOG_IPS=1` to unmask them), but connection diagnostics in `lobby_proc.log`
  print them in full. `tpf2_bridge.log` records the start of every line sent and received, and the
  host's `lobby_peers.log` merges every player's forwarded logs. Read logs before posting them
  publicly.
- **Clean-up can be cut short.** The panel asks the lobby to quit and kills it 1.5 s later;
  removing the UPnP mapping and de-listing from the master server can take longer, in which
  case the listing ages out after 30 s.
