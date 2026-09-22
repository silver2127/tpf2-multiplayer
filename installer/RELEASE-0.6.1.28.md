## EXPERIMENTAL - 0.6.1.28

**Adaptive Steam rate and Cross-play control**

> **Everyone needs 0.6.1.28. Messages remains the default Steam transport.**

### What changed

- **Adaptive sending rate:** starts at 1 MiB/s, halves after poor delivery reports, and grows by 25% only after three good five-second samples with data waiting to send.
- **Remembers congestion:** after a failed rate probe, a lower ceiling prevents repeated overload during that game session. The rate stays between 256 KiB/s and 16 MiB/s.
- **Cross-play is visible in the new menu**, both under Create game and in the host lobby. It uses the existing lobby switch and invitation-code update.
- Logs show each rate adjustment as **[steam-rate]**, alongside actual transfer progress and Steam delivery quality.

### Why

The fixed 16 MiB/s setting in 0.6.1.27 increased network traffic but caused poor
delivery on the tested relay route. Useful save progress fell to about 0.06 MB/s.
This build starts conservatively and reacts to delivery feedback instead of
forcing the high rate.

### How to test

1. Close the game on both computers and update through **Official + Experimental** in the launcher.
2. Transfer the same save with the same host. Leave Cross-play off for this Steam comparison.
3. Check actual save progress and the **[steam-rate]** entries. No extra file is needed for Messages.

### Known limitations

- A live speed improvement is **not yet confirmed**. The route can still limit throughput.
- The process uses one conservative rate budget; the worst active outgoing peer governs it.
- The learned ceiling resets when the game restarts. Unknown feedback does not increase the rate.
- Direct TCP still takes priority. An existing Legacy switch file still selects the legacy transport, which does not use this adaptive controller.

### Validation

The GitHub workflow checks the adapter and rate controller, including backoff,
slow growth, multiple peers, missing feedback and configuration failures.
No additional local game or transfer suite is run for this release.

### Downloads

- [Windows MSI](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.28/TpF2Multiplayer.msi)
- [Proton installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.28/install_proton.sh) / [Python installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.28/install_proton.py)
- [Files ZIP](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.28/TpF2Multiplayer-files.zip) / [SHA-256 checksums](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.28/SHA256SUMS.txt)
- [Valve networking headers license](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.28/SteamNetworking-LICENSE.txt)
