## EXPERIMENTAL - TCP save transfer and progress display

Test build based on 0.7. Everyone in the session must update to 0.7.0.1.

### Changes

- Save transfers can accept direct TCP connections over IPv4 and IPv6. IPv4 remains available when IPv6 is disabled.
- Fixed simultaneous TCP connections choosing opposite streams at the two ends, which could close both connections and force a slow fallback.
- The host attempts its TCP router mapping independently of the UDP mapping and reports failures. A successful UDP mapping is no longer treated as evidence that TCP was mapped.
- Lobby and resync show the transfer route, TCP status, received/total MB and current MB/s. Rates use received or acknowledged bytes, not data waiting in a send queue.
- The resync panel has more room for the transfer details. It uses the same progress data as the lobby and in-game transfer status.

### How to test

1. Close the game on both computers. In the existing launcher select **Official + Experimental**, then **Update and play**.
2. Transfer the same save and check whether the status shows **TCP**. Either computer may accept the direct connection.
3. Run a resync and check that size, progress and speed appear there too. If TCP fails, note the displayed fallback and keep both players' logs.

### Known limitations

- Internet reachability and a live speed improvement are not yet confirmed. IPv4 may need a TCP router mapping; IPv6 still needs inbound firewall permission. UPnP can be refused by a router.
- **TCP failed / unavailable** identifies the fallback, not the exact router or firewall responsible.
- If direct TCP is unavailable, Steam/UDP still carries the save. The existing Steam adaptive-rate issue is unchanged in this build and can still cause slow fallback transfers.
- This release changes transfer connectivity and transfer information. It does not fix a separate world-loading hang or include the other pending resync redesign.

### Validation

Local IPv4/IPv6 transfers in both directions, the simultaneous-connect regression,
Steam-to-TCP and fallback integration, source and frozen-executable transfer tests,
two/three-player simulated resync, and native menu/renderer tests passed.
The UI previews were inspected. These are local tests, not a two-computer Internet game test.

### Downloads

Use the existing launcher for the normal Windows update path.

- [Windows MSI](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.1/TpF2Multiplayer.msi)
- [Proton installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.1/install_proton.sh) / [Python installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.1/install_proton.py)
- [Files ZIP](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.1/TpF2Multiplayer-files.zip) / [SHA-256 checksums](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.1/SHA256SUMS.txt)
