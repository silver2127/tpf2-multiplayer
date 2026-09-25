### With TpF2 Multiplayer

TpF2 Multiplayer 0.7 and later ship Big Maps themselves and remove this package
when they install (its UpgradeCode is listed in their MSI), so the plugin has one
owner. Before 0.7 the two packages coexisted: they shared the proxy and the
plugin host under the **same component GUIDs** (`installer/PluginHost.wxs`), so
Windows Installer reference-counted them and only the last one out put the game's
own `alut.dll` back. `installer\test_coexist.ps1` proves that against a
throwaway folder with the real `msiexec` transactions (it needs an elevated
PowerShell, because the packages are per-machine).

