# Security model (2026-09-01)

Threat model: friends sharing a lobby code in Discord. The goal is that an
address alone (or a code seen by someone outside the group) is not enough to
read chat/game traffic or inject lockstep commands.

## What is in place

- **Session key in the code.** The host puts a random 12-byte secret in the
  lobby code (`connect.encode_profile(..., secret=)`, flag 0x80). Everyone who
  has the code derives the same key (`seal.derive_key`). The code is therefore
  the credential: share it privately.
- **Optional password, and it LOCKS the code.** Typed in the menu's PASSWORD
  field (host and every joiner must match) or `--password` on the CLI. It is
  mixed into the session key, and the code itself is encrypted under
  PBKDF2-HMAC-SHA256 (600k iterations, ~0.15 s) + HMAC: `0xFF | ts | ct | tag`.
  A locked code reveals only that it is a code and when it was made; the
  address and the session secret are inside the ciphertext. A joiner with the
  wrong password is told so (the only plaintext a sealed session accepts).
  **Limit, stated plainly:** if the password is posted in the same channel as
  the code, anyone reading the channel can decode it -- the lock is friction
  (pair code with password, run the slow KDF per guess), not a wall. The only
  ways a code carries nothing doxxable are a non-public address (a tailnet
  or VPN) or a rendezvous service that forgets the address after the session;
  both were considered and deliberately not built for now.
- **Every DATA frame sealed** (`seal.py`, stdlib only): NP1 type 'E' =
  nonce(8) + ciphertext + HMAC tag(16); SHA256-CTR keystream, encrypt-then-MAC
  with separate sub-keys, per-sender 64-frame replay window. Applies to the
  host loop, `punch.Connection` (joiner<->host, legacy joiners) and
  `mesh.MeshNode` (joiner<->joiner and relayed envelopes; a relay re-seals).
  ~50 MB/s per core, above any link here. Plain 'D' frames are refused once a
  session is sealed. A code without a secret (old host) falls back to plaintext
  with a loud warning on the joiner.
- **Company id from the lobby, not the sender.** `mp_company_cfg.txt` line 4
  (`a=1,b=2,...`, origin -> company) is authoritative in lockstep.lua: a remote
  command's own `company` stamp is overridden and the mismatch logged. (The
  lobby does not yet WRITE line 4 -- that lands with company assignment in the
  lobby UI; until then behaviour is unchanged.)
- **UPnP mapping removed** when the host's lobby exits (`observe.upnp_unmap`).
- **Argument hygiene:** the pasted code must be pure base32 and the password
  is restricted to `[A-Za-z0-9._-]` before either becomes a netpunch.exe
  argument; the logged command line masks the password.
- **Peer log volume capped** on the host (8 msgs/s, 20 MB/session per peer,
  control characters stripped) so a member cannot fill the disk or forge tags.

## What it does NOT do

- Members are trusted equally: any lobby member can still inject lockstep
  commands for any origin letter. The sealed transport stops outsiders, not
  a hostile friend.
- No forward secrecy, no key rotation; the key lives for the session.
- The save's `.sav.lua` sidecar is executed by the game: joining = trusting the
  host like a mod author. Inherent to the game.
- The merged host log still contains joiners' Windows usernames in paths.
- `seal.py` is a pragmatic stdlib construction, not a reviewed AEAD.

Tests: `python seal.py` (roundtrip/tamper/replay/reorder), `python lobby.py
--selftest-mesh` runs fully sealed; `--selftest`, `--selftest-relay`,
`--selftest-transfer` unchanged.
