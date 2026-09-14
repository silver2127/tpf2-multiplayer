# User-local multiplayer updates

The 0.4.23 MSI installs bootstrap ABI 1. This initial installation still needs
administrator approval for the shared game-folder loader and system settings.
Ordinary multiplayer updates then run as the player, without elevation.

The menu checks GitHub once when the multiplayer menu DLL starts. CHECK UPDATES
retries a check; DOWNLOAD UPDATE downloads the latest stable release after the
player clicks. Checks/downloads run in a separate hidden process. Network failure
does not prevent hosting or joining the currently installed version.

Publish `TpF2Multiplayer-update.zip` beside `TpF2Multiplayer.msi` in each GitHub
release. `installer/build_msi.ps1` builds both; `tools/build_update.py` can rebuild
the ZIP from the native and frozen Python outputs. Publish only after those
outputs have been rebuilt for the release. GitHub's release asset SHA-256 digest
is required. Old releases without the ZIP require the MSI. Drafts, prereleases,
equal versions, and older versions are not offered.

Downloads are checked against the HTTPS GitHub asset metadata. ZIP paths, total
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
require an MSI/ABI update. The ZIP currently updates MP Lua scripts, not arbitrary
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
