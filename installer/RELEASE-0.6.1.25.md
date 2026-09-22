## ⚠ EXPERIMENTAL — 0.6.1.25

**Steam save-transfer test build**

> **For testing — not a stable release.**
> Everyone in your session needs **0.6.1.25**. Stable **0.6.1.19** is unchanged.

### What changed

- **Fewer duplicate save blocks.** The sender waits for delivery acknowledgements instead of repeatedly filling Steam's queue.
- **Controlled recovery.** Missing blocks are retried in small groups, with longer waits when a transfer stalls.
- **Steam test enabled.** The corrected 32 KB Steam path is active without an extra test file. Direct TCP remains preferred when reachable; mixed CROSS-PLAY sessions keep small blocks.
- **Better diagnostics.** Logs show actual byte progress, transfer rate, duplicates, Steam queue size and send failures.

### How to test

1. Close the game normally on **both computers** and install **0.6.1.25**.
2. Transfer the **same save** and record its size and elapsed time.
3. Use **OPEN LOGS** on both computers. Note whether the transfer used **TCP or Steam**.

### Known limitations

- **A real Steam fix is not yet confirmed.** Earlier large-block builds stalled; this candidate still needs your two-computer test.
- A successful **TCP** transfer does not test the Steam fallback.
- The progress display updates in **10% steps**. A displayed 0% can mean data is arriving below that threshold.
- Optional **live join remains off by default**. Existing unrelated gameplay issues are outside this fix.

### Validation

Automated queue, bandwidth, missing-packet, retry and exact-hash tests passed, including two receivers with up to 15% injected loss. TCP/fallback tests and the native build passed.

In a **simulated 1 MiB/s connection**, a 12 MiB save took about **12 seconds instead of 71**. This is a test result, not a promise about live Steam speed.

---

### Downloads

- **Windows:** [Download the MSI](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.25/TpF2Multiplayer.msi)
- **Linux / Steam Deck:** [Proton installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.25/install_proton.sh) · [Python installer](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.25/install_proton.py)
- **Manual installation:** [Files ZIP](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.25/TpF2Multiplayer-files.zip) · [SHA-256 checksums](https://github.com/silver2127/tpf2-multiplayer/releases/download/v0.6.1.25/SHA256SUMS.txt)

The GitHub **Source code** archives are not the playable mod.
