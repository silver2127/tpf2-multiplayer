## EXPERIMENTAL - 0.6.1.26

**Steam transport comparison: Messages vs Legacy**

> **Test build - live speed improvement is not confirmed.**
> Everyone must use **0.6.1.26** and the **same transport mode**.

### What changed

- **Messages is now the default Steam transport**, using SteamNetworkingMessages v002 instead of legacy SendP2PPacket.
- **Legacy remains selectable** for a comparison with the same build, save and computers.
- New diagnostics show Steam's estimated sending capacity, actual wire rate, ping, pending bytes and unacknowledged bytes.
- Save acknowledgements, bounded retries and the preferred direct TCP path remain in place.

### First test: Messages

1. Close the game normally on both computers. In the launcher select **Official + Experimental**, then **Update and play**.
2. Transfer the same save with the same host. Record the elapsed time.
3. Open logs: both computers must report **transport=Messages**. A **TCP** transfer does not measure the Steam path.

### Optional comparison: Legacy

Close the game on both computers. In PowerShell on **each computer**, run:

```powershell
New-Item -ItemType File -Force "$env:LOCALAPPDATA\tpf2mp\data\tpf2mp_steam_legacy.txt"
```

Start normally through the launcher and repeat the same transfer. Logs must report **transport=Legacy**.
To return to Messages, close the game and remove only that switch file:

```powershell
Remove-Item -LiteralPath "$env:LOCALAPPDATA\tpf2mp\data\tpf2mp_steam_legacy.txt"
```

The switch is read at startup and persists across updates. Under Proton, use the corresponding game's Windows profile data directory.

### Known limitations

- **0.6.1.25 remained near 0.54 MB/s in the live test.** This build tests a different Steam API; it does not promise a specific speed.
- Mixed Messages/Legacy sessions cannot communicate. There is no silent transport fallback.
- An unavailable Messages API is logged and disables Steam transport for that session; select Legacy explicitly if needed.
- Progress still updates in 10% steps. Optional live join remains off by default.

### Validation

Native build and adapter tests cover reliable-send flags, API failures, message ownership, oversized messages, callbacks and queue reporting. Existing bounded-transfer and exact-hash tests passed. These checks do not measure an internet Steam transfer.

### Downloads

- [Windows MSI](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.26/TpF2Multiplayer.msi)
- [Proton installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.26/install_proton.sh) / [Python installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.26/install_proton.py)
- [Files ZIP](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.26/TpF2Multiplayer-files.zip) / [SHA-256 checksums](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.26/SHA256SUMS.txt)
