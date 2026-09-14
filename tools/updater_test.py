import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "netpunch"))
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
            asset = {"browser_download_url": "unused", "size": 3, "digest": "sha256:" + "0" * 64}
            with patch.object(updater, "latest", return_value=("0.4.24", asset)):
                with patch.object(updater, "fetch", return_value=b"bad"), self.assertRaises(ValueError):
                    updater.run("download", "0.4.23", root)
                with patch.object(updater, "fetch", side_effect=TimeoutError()), self.assertRaises(TimeoutError):
                    updater.run("download", "0.4.23", root)
            self.assertEqual((root / "active.txt").read_text(), "0.4.23")

    def test_release_selection(self):
        release = {"tag_name": "v0.4.24", "assets": [{"name": updater.ASSET,
            "size": 100, "digest": "sha256:" + "a" * 64}]}
        with patch.object(updater, "fetch", return_value=json.dumps(release).encode()):
            self.assertEqual(updater.latest("0.4.23")[0], "0.4.24")
            self.assertIsNone(updater.latest("0.4.24"))
            self.assertIsNone(updater.latest("0.4.25"))
        release["assets"] = []
        with patch.object(updater, "fetch", return_value=json.dumps(release).encode()), self.assertRaises(ValueError):
            updater.latest("0.4.23")


if __name__ == "__main__": unittest.main()
