## 0.7.0.5 - faster saves for joiners behind closed routers

Stable update based on 0.7.0.4, for Windows, Proton and the native Linux game. Everyone in a session, including the dedicated server, must update to 0.7.0.5.

### Changes

- **A slow save transfer can move to a TCP pipe on the master server.** When host and joiner cannot reach each other directly (no port forwarding, Steam unavailable), the save went through the master's UDP relay at the lobby's own UDP pace: on 2026-09-26 a 200 MB save took 10-15 minutes and the host gave up. Now, if a transfer of 16 MB or more is still not on TCP 15 seconds after it started and has more than a minute to go, host and joiner both connect out to the master server's TCP pipe and the save streams through it. Both sides connect outward, so no router setup is needed. UDP keeps going until the stream starts, and the save is checked by its hash as before. The pipe is used as soon as the master server offers it.
- **A joiner in through the master's relay no longer tries TCP at the relay's address.** It tried the relay's address three times, about 10 seconds lost per join, before taking UDP. It now tries the host's own addresses, as a joiner through Steam already did.

### Update

Windows: close the game and use **Official / Stable** in the existing launcher, then **Update and play**, or run the MSI. Linux / Steam Deck under Proton: run install_proton.sh. Native Linux: close the game and run the `.run` installer. All participants need the same version.

### Known limitations

- The pipe speeds up a transfer only as far as the host's upload and the distance allow: the save still leaves the host's home connection first.
- Save bytes going through the pipe pass the master server unencrypted (the lobby's own messages stay sealed). The joiner checks the save's hash, so a changed file is rejected.
- The native Linux terrain pager can still thrash on a running big map.

### Validation

TCP connectivity tests, including the master's pipe pairing and a save streamed host to joiner through a real local pipe; the lobby self-tests (relay, transfer, dual path, mesh), the rendezvous and master TLS tests, transfer status and Steam Messages tests. The live master relay was measured at 16 MB/s without loss. The Linux libraries are built and tested (CTest) in the pinned Steam Runtime soldier SDK, and the Linux release check confirms that the bundled Lua matches this Windows release exactly.
