# Vendored shared binaries

These three files are built in the tpf2-multiplayer repository and copied here
unchanged. Both packages ship them under the SAME component GUIDs (see
PluginHost.wxs), so they must be the same bytes. Regenerate with
`tools\vendor_host.ps1`; never edit or rebuild them here.

source repo:    https://github.com/silver2127/tpf2-multiplayer
source release: v0.6.1.14 (TpF2Multiplayer.msi, sha256 7a907d792e9c7c18401f04e68f10a0652448fcdc1f155c73e9cda840b8edbab3)
source commit:  664110faba12260f732f7cf64921dcd81c9942aa (the tag)
extracted:      alut.dll and tpf2_pluginhost.dll from an administrative image, tpf2ca.dll from the Binary table
vendored on:    2026-09-21 13:36

| file | bytes | sha256 |
| --- | --- | --- |
| alut.dll | 180224 | 7955c0338ed6e3870171f15bf60985594bf25ad7343e9af133c3ba61431ab15c |
| tpf2_pluginhost.dll | 219136 | a5ac23603cc8e47f760c17be37bc514261d4d12d991e557c19249f3760809f02 |
| tpf2ca.dll | 225792 | c042dea695627ca78440c04f0b1442790e122a1f16b025affc1ee40d2dac72f2 |
