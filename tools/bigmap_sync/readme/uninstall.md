### Uninstall

Add/Remove Programs → **TpF2 Big Maps**. Puts the stock `res\config\base_mod.lua`
back (the plugin's own restore, run through rundll32 before its files go), then
removes the plugin, its config, and — if TpF2 Multiplayer is not installed — the
proxy, the plugin host, the Segment Heap value, and restores the stock `alut.dll`. Steam's
*Verify integrity of game files* also puts the stock `alut.dll` back without
uninstalling anything; **Repair** from Add/Remove Programs reinstalls the proxy.

