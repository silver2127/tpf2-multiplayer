#!/usr/bin/env python3
"""Build TpF2Multiplayer-files.zip: the files the MSI installs, in the game-folder
layout, for the Proton installer (tools/proton/install.py) and manual installs.

    python3 tools/build_files_zip.py                 from the build outputs, like the MSI
    python3 tools/build_files_zip.py --from-msi DIR  from an extracted MSI (msiextract -C DIR ...)

The archive is reproducible: fixed entry times, sorted names. It prints the
SHA-256 for SHA256SUMS.txt.
"""
import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ASSET = "TpF2Multiplayer-files.zip"
# What the MSI installs (installer/Package.wxs + PluginHost.wxs): name in the game folder -> build output.
LAYOUT = {
    "alut.dll": "native/out/alut.dll",
    "tpf2_pluginhost.dll": "native/out/tpf2_pluginhost.dll",
    "tpf2_bridge_mp.dll": "native/out/tpf2_bridge_mp.dll",
    "tpf2_menu.dll": "native/out/tpf2_menu.dll",
    "tpf2_slice.dll": "native/out/tpf2_slice.dll",
    "tpf2_slice.cfg": "installer/cfg/tpf2_slice.cfg",
    "tpf2mp_version.txt": "installer/VERSION",
    "netpunch/netpunch.exe": "netpunch/dist/netpunch.exe",
    "plugins/tpf2_previews.dll": "native/out/tpf2_previews.dll",
    "plugins/tpf2_workshop_register.dll": "native/out/tpf2_workshop_register.dll",
}
MOD = "mod/mp_lockstep_1"


def collect(from_msi):
    files = {}
    if from_msi:
        roots = [p.parent for p in Path(from_msi).rglob("alut.dll")]
        assert len(roots) == 1, f"expected one game folder under {from_msi}, found {len(roots)}"
        for p in sorted(roots[0].rglob("*")):
            if p.is_file():
                files[p.relative_to(roots[0]).as_posix()] = p.read_bytes()
        for name in LAYOUT:
            assert name in files, f"extracted MSI lacks {name}"
        return files
    for name, source in LAYOUT.items():
        path = REPO / source
        assert path.is_file(), f"missing build output: {path} (run native\\build.bat all and freeze netpunch first)"
        files[name] = path.read_bytes()
    for p in sorted((REPO / MOD).rglob("*")):
        if p.is_file():
            files["mods/mp_lockstep_1/" + p.relative_to(REPO / MOD).as_posix()] = p.read_bytes()
    return files


def build(files, out):
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, files[name])
    return hashlib.sha256(out.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from-msi", type=Path, help="directory an MSI was extracted into")
    ap.add_argument("--out", type=Path, default=REPO / "installer/out" / ASSET)
    args = ap.parse_args()
    files = collect(args.from_msi)
    version = files["tpf2mp_version.txt"].decode().strip()
    lua = sum(n.endswith(".lua") for n in files)
    sha = build(files, args.out)
    print(f"Built {args.out}: {len(files)} files ({lua} Lua), version {version}\n{sha}  {ASSET}")


if __name__ == "__main__":
    sys.exit(main())
