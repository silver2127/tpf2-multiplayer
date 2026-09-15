"""Build the GitHub user-local update asset from the same outputs as the MSI."""
import hashlib
import json
from pathlib import Path
import sys
import zipfile

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "netpunch"))
import lobby
import updater


def build():
    version = (REPO / "installer/VERSION").read_text(encoding="utf-8").strip()
    assert version == lobby.LOBBY_VERSION, "Installer/lobby version mismatch"
    files = {}
    for name in ("tpf2_bridge_mp.dll", "tpf2_menu.dll", "tpf2_slice.dll"):
        files[name] = (REPO / "native/out" / name).read_bytes()
    files["plugins/tpf2_workshop_register.dll"] = (REPO / "native/out/tpf2_workshop_register.dll").read_bytes()
    files["plugins/tpf2_previews.dll"] = (REPO / "native/out/tpf2_previews.dll").read_bytes()
    files["netpunch/netpunch.exe"] = (REPO / "netpunch/dist/netpunch.exe").read_bytes()
    for path in (REPO / "mod/mp_lockstep_1/res/scripts/mp").glob("*.lua"):
        files["mod/res/scripts/mp/" + path.name] = path.read_bytes()
    # Derive these from the canonical installed sources, so future edits cannot
    # accidentally update the MSI while leaving the automatic payload behind.
    source = (REPO / "mod/mp_lockstep_1/res/config/game_script/lockstep.lua").read_text(encoding="utf-8")
    files["mod/res/scripts/mp/entry.lua"] = ("-- MP Lockstep" + source.split("-- MP Lockstep", 1)[1]).encode()
    source = (REPO / "mod/mp_lockstep_1/mod.lua").read_text(encoding="utf-8")
    start = source.index("\n\treturn {")
    files["mod/res/scripts/mp/mod_data.lua"] = ("return function()" + source[start:]).encode()
    manifest = {"version": version, "bootstrap_abi": updater.BOOTSTRAP_ABI,
                "files": {n: hashlib.sha256(data).hexdigest() for n, data in files.items()}}
    out = REPO / "installer/out" / updater.ASSET
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(files.items()):
            z.writestr(name, data)
        z.writestr("manifest.json", json.dumps(manifest, sort_keys=True))
    print(f"Built {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    build()
