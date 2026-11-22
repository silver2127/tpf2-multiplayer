## EXPERIMENTAL - 0.6.1.27

**Steam Messages send-rate correction**

> **Test build. Everyone needs 0.6.1.27. Messages remains the default.**

### What changed

- Both Steam send-rate settings now use **16 MiB/s**, replacing the previous minimum of 1 MiB/s and maximum of 16 MiB/s.
- The 0.6.1.26 live session stayed at the 1 MiB/s minimum with nearly 4 MB queued. Valve documents equal settings for a manually configured rate.
- Logs now include local and remote connection quality alongside rate, ping and pending bytes.

### How to test

1. Close the game normally on both computers.
2. In the launcher select **Official + Experimental**, then **Update and play**.
3. Transfer the same save. Messages is the default; no extra file is needed.

### Known limitations

**16 MiB/s is a configured rate, not a measured transfer speed.** The real speed
still depends on the connection, relay route and packet loss. This fixed setting
does not automatically adapt to slower connections. Direct TCP remains preferred.
An existing Legacy switch file still selects Legacy and must match on both peers.

### Validation

Built and checked by the official GitHub workflow. No additional local gameplay
or transfer test was run for this release, as requested. Live speed improvement
with the corrected rate is still unconfirmed.

### Downloads

- [Windows MSI](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.27/TpF2Multiplayer.msi)
- [Proton installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.27/install_proton.sh) / [Python installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.27/install_proton.py)
- [Files ZIP](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.27/TpF2Multiplayer-files.zip) / [SHA-256 checksums](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.27/SHA256SUMS.txt)
- [Valve networking headers license](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.27/SteamNetworking-LICENSE.txt)
