"""Build the user-local update bundle zip from the same outputs as the MSI.

A GitHub release publishes one asset, the MSI; the in-game updater extracts its
payload from that (updater.msi_payload). This zip is the offline test fixture
and the fallback asset the updater still accepts. Both go through
updater.payload and updater.write_bundle, so the file set, the derived
entry.lua/mod_data.lua and every hash agree byte for byte
(tools/updater_test.py checks that against a built MSI)."""
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "netpunch"))
import lobby
import updater


def files():
    """The bundle's files as tools/build_update.py ships them from the build tree."""
    return updater.payload(REPO / "native/out", REPO / "native/out",
                           REPO / "netpunch/dist/netpunch.exe", REPO / "mod/mp_lockstep_1")


def build():
    version = (REPO / "installer/VERSION").read_text(encoding="utf-8").strip()
    assert version == lobby.LOBBY_VERSION, "Installer/lobby version mismatch"
    out = REPO / "installer/out" / updater.ASSET_ZIP
    out.parent.mkdir(exist_ok=True)
    with open(out, "wb") as stream:
        updater.write_bundle(stream, version, files())
    print(f"Built {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    build()
