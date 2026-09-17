import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "netpunch"))
import updater


def bundle(version="0.4.24", change=None):
    files = {n: b"test payload" for n in updater.REQUIRED}
    if change:
        change(files)
    manifest = {"version": version, "bootstrap_abi": 1,
                "files": {n: hashlib.sha256(v).hexdigest() for n, v in files.items()}}
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        for n, data in files.items(): z.writestr(n, data)
        z.writestr("manifest.json", json.dumps(manifest))
    out.seek(0)
    return out


def asset(name, size=100, digest="a" * 64):
    return {"name": name, "size": size, "digest": "sha256:" + digest,
            "browser_download_url": f"https://github.com/{updater.REPO}/releases/download/v0.4.24/{name}"}


def test_msi():
    """The MSI the conversion test runs against: installer/out/TpF2Multiplayer.msi,
    packaged from the existing build outputs when it is missing and they are all
    there (no native build, no PyInstaller). None -> the case is skipped."""
    msi = REPO / "installer/out/TpF2Multiplayer.msi"
    if msi.exists():
        # a fixture older than what it must match is a stale build, not a defect:
        # the outputs and the mod sources it packages move on between releases
        newest = 0.0
        for d, pat in ((REPO / "native/out", "*.dll"), (REPO / "netpunch/dist", "netpunch.exe"),
                       (REPO / "mod/mp_lockstep_1", "**/*.lua")):
            for f in d.glob(pat):
                newest = max(newest, f.stat().st_mtime)
        if msi.stat().st_mtime < newest:
            return None
        return msi
    outputs = [REPO / "native/out" / n for n in (
        "alut.dll", "tpf2_pluginhost.dll", "tpf2_bridge_mp.dll", "tpf2_menu.dll", "tpf2_slice.dll",
        "tpf2_previews.dll", "tpf2_workshop_register.dll")]
    outputs += [REPO / "netpunch/dist/netpunch.exe", REPO / "installer/out/tpf2ca.dll"]
    missing = [str(p.relative_to(REPO)) for p in outputs if not p.exists()]
    if missing:
        raise unittest.SkipTest("no installer/out/TpF2Multiplayer.msi and not every output to package one "
                                "is built (missing: " + ", ".join(missing) + "); build the MSI first")
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    str(REPO / "installer/build_msi.ps1"), "-SkipBuild", "-SkipFreeze", "-AcceptWixEula"],
                   check=True, cwd=REPO, stdin=subprocess.DEVNULL)
    if not msi.exists():
        raise unittest.SkipTest("installer/build_msi.ps1 -SkipBuild produced no MSI")
    return msi


class Updates(unittest.TestCase):
    def test_atomic_install_retains_old_release(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            updater.install_bundle(bundle("0.4.23"), "0.4.23", root)
            updater.install_bundle(bundle(), "0.4.24", root)
            self.assertEqual((root / "active.txt").read_text(), "0.4.24")
            self.assertEqual((root / "previous.txt").read_text(), "0.4.23")
            self.assertTrue((root / "releases/0.4.23/ready").exists())
            updater.install_bundle(bundle(), "0.4.24", root)
            with self.assertRaises(ValueError):
                updater.install_bundle(bundle(change=lambda f: f.update({"tpf2_menu.dll": b"different"})), "0.4.24", root)

    def test_bad_bundles_never_activate(self):
        for mutate in (
            lambda f: f.pop("tpf2_menu.dll"),
            lambda f: f.update({"../escape.dll": b"bad"}),
            lambda f: f.update({"C:/escape.dll": b"bad"}),
            lambda f: f.update({"tpf2_menu.dll:stream": b"bad"}),
            lambda f: f.update({"TPF2_MENU.DLL": b"bad"}),
            lambda f: f.update({"alut.dll": b"unexpected"}),
        ):
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                with self.assertRaises(ValueError):
                    updater.install_bundle(bundle(change=mutate), "0.4.24", root)
                self.assertFalse((root / "active.txt").exists())
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(ValueError):
                updater.install_bundle(bundle(), "0.4.25", Path(temp))

    def test_download_hash_and_network_failure_leave_active_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            updater.atomic_text(root / "active.txt", "0.4.23")
            for name in (updater.ASSET_ZIP, updater.ASSET_MSI):
                with patch.object(updater, "latest", return_value=("0.4.24", asset(name, 3, "0" * 64))):
                    with patch.object(updater, "fetch", return_value=b"bad"), self.assertRaises(ValueError):
                        updater.run("download", "0.4.23", root)
                    with patch.object(updater, "fetch", side_effect=TimeoutError()), self.assertRaises(TimeoutError):
                        updater.run("download", "0.4.23", root)
            self.assertEqual((root / "active.txt").read_text(), "0.4.23")

    def test_release_selection(self):
        release = {"tag_name": "v0.4.24", "assets": [asset(updater.ASSET_ZIP)]}
        with patch.object(updater, "fetch", return_value=json.dumps(release).encode()):
            self.assertEqual(updater.latest("0.4.23")[0], "0.4.24")
            self.assertIsNone(updater.latest("0.4.24"))
            self.assertIsNone(updater.latest("0.4.25"))
        release["assets"] = []
        with patch.object(updater, "fetch", return_value=json.dumps(release).encode()), self.assertRaises(ValueError):
            updater.latest("0.4.23")

    def test_release_asset_is_the_msi(self):
        # The release publishes one asset, the MSI (2026-09-16). Older releases
        # carried the bundle zip: still accepted; the MSI wins when both exist.
        release = {"tag_name": "v0.4.24", "assets": []}
        def picked():
            with patch.object(updater, "fetch", return_value=json.dumps(release).encode()):
                return updater.latest("0.4.23")
        release["assets"] = [asset(updater.ASSET_MSI, 11 * 1024 * 1024)]
        self.assertEqual(picked(), ("0.4.24", release["assets"][0]))
        release["assets"] = [asset(updater.ASSET_ZIP), asset(updater.ASSET_MSI), asset("TpF2Multiplayer.msi.sha256")]
        self.assertEqual(picked()[1]["name"], updater.ASSET_MSI)
        release["assets"] = [asset(updater.ASSET_ZIP), asset("Source code (zip)")]
        self.assertEqual(picked()[1]["name"], updater.ASSET_ZIP)
        for assets in ([], [asset("TpF2Multiplayer-0.4.24.msi")], [asset("tpf2multiplayer.msi")]):
            release["assets"] = assets
            with self.assertRaises(ValueError, msg=assets):
                picked()
        release["assets"] = [asset(updater.ASSET_MSI, digest="")]
        with self.assertRaises(ValueError):
            picked()
        release["assets"] = [asset(updater.ASSET_MSI, size=updater.MAX_BYTES + 1)]
        with self.assertRaises(ValueError):
            picked()
        release["assets"] = [asset(updater.ASSET_MSI)]
        release["prerelease"] = True
        self.assertIsNone(picked())

    def test_four_part_bugfix_versions_order_after_their_feature(self):
        # 0.x = major feature, 0.x.y = minor feature, 0.x.y.z = bugfix (2026-09-16)
        self.assertLess(updater.version("0.5.7"), updater.version("0.5.7.1"))
        self.assertLess(updater.version("0.5.7.1"), updater.version("0.5.7.2"))
        self.assertLess(updater.version("0.5.7.9"), updater.version("0.5.8"))
        self.assertLess(updater.version("0.5.8"), updater.version("0.6"))
        release = {"tag_name": "v0.5.7.1", "assets": [asset(updater.ASSET_MSI)]}
        with patch.object(updater, "fetch", return_value=json.dumps(release).encode()):
            self.assertEqual(updater.latest("0.5.7")[0], "0.5.7.1")
            self.assertIsNone(updater.latest("0.5.7.1"))
            self.assertIsNone(updater.latest("0.5.8"))
        for bad in ("5", "0.5.7.1.2", "0.5.7.", "v0.5.7.1", "0.5.7a"):
            with self.assertRaises(ValueError, msg=bad):
                updater.version(bad)

    def test_derived_mod_scripts_come_from_the_shared_function(self):
        # build_update.py used to derive these itself; both paths now call updater.
        derived = updater.derive_mod_scripts(REPO / "mod/mp_lockstep_1")
        self.assertEqual(set(derived), {updater.MOD_SCRIPTS + "entry.lua", updater.MOD_SCRIPTS + "mod_data.lua"})
        self.assertTrue(derived[updater.MOD_SCRIPTS + "entry.lua"].startswith(b"-- MP Lockstep"))
        self.assertTrue(derived[updater.MOD_SCRIPTS + "mod_data.lua"].startswith(b"return function()\n\treturn {"))
        for data in derived.values():
            self.assertNotIn(b"\r", data)

    def test_msi_payload_matches_the_local_bundle(self):
        # The release asset is the MSI: the updater must extract exactly what
        # tools/build_update.py packages from the same outputs, hash for hash.
        if os.name != "nt":
            self.skipTest("msiexec: Windows only")
        msi = test_msi()
        if msi is None:
            self.skipTest("installer/out/TpF2Multiplayer.msi is missing or predates the build outputs / mod sources; "
                          "rebuild it (installer\build_msi.ps1) to run the conversion case")
        sys.path.insert(0, str(REPO / "tools"))
        import build_update
        version = (REPO / "installer/VERSION").read_text(encoding="utf-8").strip()
        expected = build_update.files()
        extracts = lambda: set(Path(tempfile.gettempdir()).glob("tpf2mp-*"))
        before = extracts()
        with self.assertRaises(ValueError):
            updater.msi_payload(msi, "0.0.1")   # the MSI's own version stamp must match the release
        converted = updater.msi_payload(msi, version)
        lua = {n for n in expected if n.startswith(updater.MOD_SCRIPTS) and n.endswith(".lua")}
        self.assertEqual(set(expected), updater.REQUIRED | lua)
        self.assertEqual(set(converted), set(expected))
        for name in sorted(expected):
            self.assertEqual(hashlib.sha256(converted[name]).hexdigest(), hashlib.sha256(expected[name]).hexdigest(),
                             f"{name}: installer/out/TpF2Multiplayer.msi and the build outputs disagree; rebuild the MSI")
        self.assertEqual(updater.manifest(version, converted), updater.manifest(version, expected))
        # And the whole download path, against a temp root: never the real updates folder.
        data = msi.read_bytes()
        picked = asset(updater.ASSET_MSI, len(data), hashlib.sha256(data).hexdigest())
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with patch.object(updater, "latest", return_value=(version, picked)), patch.object(updater, "fetch", return_value=data):
                self.assertEqual(updater.run("download", "0.0.1", root)[0], "Ready")
            release = root / "releases" / version
            self.assertEqual((root / "active.txt").read_text(), version)
            self.assertTrue((release / "ready").exists())
            for name, content in expected.items():
                self.assertEqual((release / name).read_bytes(), content, name)
        self.assertEqual(extracts() - before, set(), "extract folder left behind in the temp directory")


if __name__ == "__main__": unittest.main()
