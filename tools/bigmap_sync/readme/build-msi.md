### Building the MSI

```
tools\vendor_host.ps1 -FromMsi TpF2Multiplayer.msi -Release v0.4.18
                                      # alut.dll, tpf2_pluginhost.dll, tpf2ca.dll out of the
                                      # latest TpF2 Multiplayer release MSI; the source goes
                                      # into installer\vendor\VENDORED.md
installer\build_msi.ps1 -Validate -AcceptWixEula
```

The three shared binaries are built in the multiplayer repository and vendored
here unchanged: both packages must ship the same bytes under the same GUIDs.
Vendor them from the **latest multiplayer release MSI** before each release. A
rebuild of the same commit gives different bytes, so an install of one product
could replace the other's copy. `tools\vendor_host.ps1 -Build` vendors from a
checkout's build outputs instead (dev only). `build_msi.ps1` refuses to build if
`PluginHost.wxs` has drifted from the multiplayer copy (line endings aside). WiX v7 asks you to accept its
[OSMF EULA](https://wixtoolset.org/osmf/); `-AcceptWixEula` passes it
per-invocation and nothing accepts it for you.

GitHub Actions runs the same script (`.github/workflows/build-msi.yml`): every push to `dev` or `main` and
every pull request builds `TpF2BigMaps-<version>.msi` and `SHA256SUMS.txt` on a `windows-2022` runner from the
vendored shared binaries and keeps them as the run's artifact; a `v*` tag (which must equal `installer\VERSION`)
also creates a draft GitHub release with them attached, ready to be edited and published. The runner has no
multiplayer checkout beside this one, so the workflow compares `PluginHost.wxs` with the copy at the release
named in `installer\vendor\VENDORED.md` instead. The workflow passes `-AcceptWixEula`, which is the
repository owner accepting the WiX terms for those builds.

