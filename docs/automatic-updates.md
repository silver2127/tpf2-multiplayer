# User-local multiplayer updates

The 0.4.23 MSI installs bootstrap ABI 1. This initial installation still needs
administrator approval for the shared game-folder loader and system settings.
Ordinary multiplayer updates then run as the player, without elevation.

The menu checks GitHub once when the multiplayer menu DLL starts. CHECK UPDATES
retries a check; DOWNLOAD UPDATE downloads the latest stable release after the
player clicks. Checks/downloads run in a separate hidden process. Network failure
does not prevent hosting or joining the currently installed version.

A GitHub release publishes one asset, `TpF2Multiplayer.msi` (since 2026-09-16).
The updater takes its payload from that MSI: after the download is verified it
runs an administrative install (`msiexec /a <msi> /qn TARGETDIR=<temp>`) into a
folder under the user's temp directory, which lays the package's file tree out
without installing anything and without elevation, finds the game folder in it
by its `tpf2_menu.dll`, maps the DLLs, the plugins, `netpunch\netpunch.exe` and
the mod's `res/scripts/mp/*.lua` into the bundle layout, derives `entry.lua` and
`mod_data.lua` from the mod's `lockstep.lua` and `mod.lua` (`updater.derive_mod_scripts`,
the one function `tools/build_update.py` also uses), checks that the MSI's
`tpf2mp_version.txt` stamp is the release version, hashes every file into a
manifest and installs the result as a bundle. The temp folder is removed
afterwards; a failing `msiexec` is reported with its exit code. Nothing in the
bundle layout or the native bootstrap changed for this.

`installer/build_msi.ps1` still builds `TpF2Multiplayer-update.zip` beside the
MSI, from the same outputs: it is the offline test fixture and a fallback asset.
`tools/updater_test.py` checks that the MSI conversion and the zip agree hash
for hash. Releases before 2026-09-16 published that zip; a release whose only
asset is the zip is still accepted, and the MSI wins when both are attached.
Publish only after the native and frozen Python outputs have been rebuilt for
the release. GitHub's release asset SHA-256 digest is required, for the MSI as
for the zip. A release with neither asset requires installing by hand. Drafts,
prereleases, equal versions, and older versions are not offered.

Downloads are checked against the HTTPS GitHub asset metadata. Bundle paths, total
sizes, the bootstrap ABI, required files, and every manifest hash are validated
before extraction. A process lock serializes activation. Payloads live under
`%LOCALAPPDATA%\tpf2mp\updates\releases\<version>`. `active.txt` is replaced
atomically after staging a complete release; `previous.txt` retains the previous
selection. Existing release files are never overwritten. No running game is
killed or restarted. Old releases are retained so existing processes can keep
using them.

At game startup, the installed proxy validates the selected folder and pins
`TPF2MP_RELEASE_ROOT` for that process. Bridge/menu/slice DLLs, the previews
plugin, the networking executable, and MP Lua modules use that same release.
The Lua entry points preserve the game's script environment. Changing the
pointer while a game is running cannot change that game's selection. Missing
required files cause whole-release fallback to the MSI. Missing Lua modules
inside an otherwise selected release fail instead of mixing installed versions.

The shared plugin host remains installed beside the game and still discovers
Big Maps and other existing plugins. The selected release takes precedence for
the multiplayer previews plugin. A newer MSI overrides an older cached release;
removing the MSI's version marker disables cached multiplayer release selection.
Loader, shared host, system registry, or new game-resource entry-point changes
require an MSI/ABI update. The bundle currently updates MP Lua scripts, not arbitrary
game resources or third-party mods. Dedicated relay servers still use their
normal deployment process.

To revert an update, close the game and replace `active.txt` with the version in
`previous.txt`, or remove `active.txt` to use the installed MSI. Do not delete
release directories while instances may still be using them.

Validation: updater failure/admission tests, native bootstrap probe, Lua 5.2
searcher/pinning checks, bundle hashes and Lua compilation, frozen updater
GitHub check, existing version-gate/NTG/preview regressions, native builds and
MSI administrative extraction.

Live validation on 2026-09-14: installed/repaired the 0.4.23 MSI, hash-checked
its native and Lua payload, activated the built ZIP as an ordinary user, then
launched normal and GameAgent instances. Both reached the title menu with
bridge/menu/slice/previews modules confirmed under the AppData release folder;
Big Maps continued loading from the game folder. The frozen updater's GitHub
check succeeded. This used the local built ZIP: a public GitHub bundle download
and a loaded-world multiplayer session on this release are not yet validated.
The pre-restart host world was autosaved and backed up.

On this development machine, MSI initially preserved locally modified,
unversioned binaries/scripts. A forced MSI repair (`msiexec /fa <MSI>`) was
needed before the installed payload matched. The user-local update path avoids
these MSI replacement rules because each release has its own directory.
