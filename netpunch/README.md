# netpunch

The lobby: NAT traversal, roster and chat, the sealed transport, the game-frame relay and
the save transfer. It is frozen into `netpunch.exe` and started by the in-game Multiplayer
panel; the same program runs the dedicated relay on a server. The protocol, the IPC with the
game and the relay are described in [docs/NETWORKING.md](../docs/NETWORKING.md); the threat
model in [docs/SECURITY.md](../docs/SECURITY.md).

| file | role |
|---|---|
| `lobby.py` | the program: `host`, `join`, `host --relay-only`, the file IPC with the game, save transfer, self-tests |
| `punch.py` | `NP1:` framing and the hole-punching `Connection` |
| `connect.py` | the join-code format (including password-locked codes) and the connect race |
| `observe.py` | the connectivity profile: STUN, UPnP, IPv6 |
| `mesh.py` | joiner-to-joiner links on one socket, and relay envelopes |
| `seal.py` | frame sealing: encrypt-then-MAC with a replay window |
| `masterserver.py` | the public game list service (server side only, not in the exe) |
| `swarm.py` | an unused experiment: peer-to-peer piece distribution of the save |
| `netpunch.spec` | PyInstaller spec (the build command regenerates it) |

## Running from source

Python 3.12 and `pip install -r requirements.txt` (`pystun3` for STUN, `miniupnpc` for UPnP;
without `miniupnpc` UPnP falls back to an `upnpc` executable on PATH, or is skipped).

```
python lobby.py host --name alice [--password pw] [--lobby-name "Alice's game"]
python lobby.py join <CODE> --name bob [--password pw]
python lobby.py host --relay-only --name relay --lobby-name "My relay" --io-dir <dir>
```

Useful options: `--io-dir` (where the IPC files live; default the working directory),
`--local-port` (default 29471), `--timeout` (join, default 40 s), `--game-relay-port` /
`--game-local-port` (the loopback ports to the game's bridge DLL), `--publish <url>` and
`--public` (announce to a master server), `--forward-log <file>` (repeatable; ship a log
file's new lines to the host's merged log), `--no-mesh` (joiner talks only to the host).
Run two lobbies on one machine from different `--io-dir`s. Self-tests are listed in
[docs/NETWORKING.md](../docs/NETWORKING.md#self-tests).

## Freezing

```
python -m pip install pyinstaller -r requirements.txt
python -m PyInstaller --noconfirm --onefile --name netpunch lobby.py
```

The result is `dist\netpunch.exe` (git-ignored). `installer\build_msi.ps1` runs the same
command. The panel prefers `netpunch.exe` and falls back to `python lobby.py`, so a stale
exe next to the game silently wins over edited sources: re-freeze after changing anything
here.
