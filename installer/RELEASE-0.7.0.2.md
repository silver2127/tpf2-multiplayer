## 0.7.0.2 - TCP transfers and resync lobby

Stable update based on 0.7, including the resync menu changes on main. Everyone in a session must update to 0.7.0.2.

### Changes

- Direct save transfers accept IPv4 and IPv6 TCP connections. IPv4 remains available if IPv6 is disabled.
- Fixed simultaneous TCP connections selecting different streams and closing both connections.
- TCP router mapping is attempted independently of UDP, with failures reported.
- Lobby and resync display transport, TCP status, transferred/total MB and current MB/s, measured from received or acknowledged bytes.
- If TCP fails, a local chat hint identifies the host TCP port. Resync also shows this guidance below the player list and chat. The normal host port is TCP 29471; custom host ports are shown correctly. A UDP forwarding rule alone does not forward TCP.
- Resync now uses the shared lobby layout with player progress and chat.
- Resync restarts the loading-progress reader and replaces completed transfer text with world-loading progress, including after repeated resyncs and late transfer updates.

### Update

Close the game and use **Official / Stable** in the existing launcher, then **Update and play**. All participants need the same version. Pending changes from dev are not included.

### Known limitations

- Routers and firewalls can still block TCP. IPv4 may require forwarding the displayed TCP port to the host PC; IPv6 needs inbound firewall permission. Router acceptance of UPnP does not prove Internet reachability.
- TCP failure does not identify the exact blocking router or firewall. Steam/UDP fallback remains available and the existing Steam adaptive-rate issue can still make it slow.
- Automated local transfer and menu tests passed. A successful two-computer Internet transfer after the latest manual port forwarding has not been confirmed.
- The loading display fix does not claim to fix an independent game-engine loading hang.

### Validation

Local IPv4/IPv6, simultaneous-connect, transfer-meter and fallback tests; source/frozen transfer self-tests; two/three-player simulated resync; native menu rendering across five scales, resync phases, loading progress and save-picker checks. Rendered menu previews inspected. Package/download verification is performed before publication.

### Downloads

Use the existing launcher for normal Windows updates.

- [Windows MSI](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.2/TpF2Multiplayer.msi)
- [Proton installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.2/install_proton.sh) / [Python installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.2/install_proton.py)
- [Files ZIP](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.2/TpF2Multiplayer-files.zip) / [SHA-256 checksums](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.7.0.2/SHA256SUMS.txt)
