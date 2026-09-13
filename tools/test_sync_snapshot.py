"""Filesystem/transfer tests; no engine behavior is simulated or certified."""
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
from sync_snapshot import PreparedSnapshot


class SnapshotTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.snapshot = PreparedSnapshot((('.sav', b'world'), ('.sav.lua', b'metadata'), ('.jpg', b'preview')))

    def test_roundtrip_and_exact_retry(self):
        path = self.snapshot.install(self.directory, 'mp_roundtrip')
        self.assertEqual(PreparedSnapshot.read(path), self.snapshot)
        self.assertEqual(self.snapshot.install(self.directory, 'mp_roundtrip'), path)

    def test_preserves_user_files_and_rejects_collision(self):
        path = self.directory / 'mp_existing.sav'
        path.write_bytes(b'user world')
        with self.assertRaises(FileExistsError):
            self.snapshot.install(self.directory, 'mp_existing')
        self.assertEqual(path.read_bytes(), b'user world')
        self.assertEqual(list(self.directory.iterdir()), [path])

    def test_missing_metadata_and_optional_preview(self):
        with self.assertRaises(ValueError):
            PreparedSnapshot((('.sav', b'world'),))
        short = PreparedSnapshot(self.snapshot.parts[:2])
        path = short.install(self.directory, 'mp_short')
        self.assertEqual(PreparedSnapshot.read(path), short)

    def test_transfer_corruption_and_wrong_operation(self):
        blob, metadata = self.snapshot.transfer()
        self.assertEqual(PreparedSnapshot.receive(blob, self.snapshot.files, self.snapshot.digest), self.snapshot)
        self.assertEqual(metadata[0]['name'], 'incoming_save.sav')
        for broken in (blob[:-1], b'X' + blob[1:], blob + b'X'):
            with self.assertRaises(ValueError):
                PreparedSnapshot.receive(broken, self.snapshot.files, self.snapshot.digest)
        with self.assertRaises(ValueError):
            PreparedSnapshot.receive(blob, self.snapshot.files, '0' * 64)

    def test_rollback_only_removes_new_files(self):
        existing = self.directory / 'mp_failure.sav.lua'
        existing.write_bytes(b'metadata')
        with patch('sync_snapshot.os.fsync', side_effect=OSError('disk failure')):
            with self.assertRaises(OSError):
                self.snapshot.install(self.directory, 'mp_failure')
        self.assertEqual(list(self.directory.iterdir()), [existing])
        self.assertEqual(existing.read_bytes(), b'metadata')

    def test_rejects_path_traversal_and_mutable_payload(self):
        for name in ('../escape', 'user_save', 'mp_' + 'a' * 13):
            with self.assertRaises(ValueError):
                self.snapshot.install(self.directory, name)
        with self.assertRaises(ValueError):
            PreparedSnapshot((('.sav', bytearray(b'world')), ('.sav.lua', b'metadata')))


if __name__ == '__main__':
    unittest.main()
