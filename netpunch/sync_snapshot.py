"""Immutable save sets for an explicitly selected save or completed native save.

The caller must wait for the engine's save-completion callback before read().
No save discovery, timestamp selection or replacement of user saves occurs here.
"""
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re

from sync_operation import snapshot_digest


SUFFIXES = ('.sav', '.sav.lua', '.jpg')


@dataclass(frozen=True)
class PreparedSnapshot:
    parts: tuple

    def __post_init__(self):
        if not isinstance(self.parts, tuple) or any(
                not isinstance(p, tuple) or len(p) != 2 or not isinstance(p[1], bytes)
                for p in self.parts):
            raise ValueError('snapshot must own immutable bytes')
        snapshot_digest(self.files)

    @property
    def files(self):
        return [{'suffix': suffix, 'size': len(data),
                 'sha256': hashlib.sha256(data).hexdigest()}
                for suffix, data in self.parts]

    @property
    def digest(self):
        return snapshot_digest(self.files)

    @classmethod
    def read(cls, save):
        save = Path(save)
        if save.suffix.lower() != '.sav':
            raise ValueError('an exact .sav path is required')
        stem = save.with_suffix('')
        paths = [(suffix, Path(str(stem) + suffix)) for suffix in SUFFIXES]
        paths = [(s, p) for s, p in paths if s != '.jpg' or p.exists()]
        before = [p.stat() for _, p in paths]
        result = cls(tuple((s, p.read_bytes()) for s, p in paths))
        after = [p.stat() for _, p in paths]
        for a, b in zip(before, after):
            if (a.st_size, a.st_mtime_ns, a.st_ino) != (b.st_size, b.st_mtime_ns, b.st_ino):
                raise ValueError('save changed while preparing snapshot')
        return result

    def transfer(self):
        """Existing reliable transfer's blob/metadata format, with exact hashes."""
        return (b''.join(data for _, data in self.parts),
                [{'name': 'incoming_save' + f['suffix'], 'size': f['size'],
                  'sha256': f['sha256']} for f in self.files])

    @classmethod
    def receive(cls, blob, files, expected_digest):
        snapshot_digest(files)
        parts, offset = [], 0
        for item in files:
            end = offset + item['size']
            data = bytes(blob[offset:end])
            if len(data) != item['size'] or hashlib.sha256(data).hexdigest() != item['sha256']:
                raise ValueError('snapshot checksum mismatch')
            parts.append((item['suffix'], data))
            offset = end
        if offset != len(blob):
            raise ValueError('unexpected snapshot bytes')
        result = cls(tuple(parts))
        if result.digest != expected_digest:
            raise ValueError('snapshot belongs to another operation')
        return result

    def install(self, directory, basename):
        """Publish exactly this set under a reserved, operation-specific name.

        A retry may reuse an identical existing set. Any conflicting file fails
        before writing. The .sav is published last, after metadata and preview.
        """
        if not re.fullmatch(r'mp_[A-Za-z0-9_-]{1,12}', basename):
            raise ValueError('invalid native snapshot name')
        directory = Path(directory)
        if not directory.is_dir():
            raise ValueError('save directory does not exist')
        ordered = sorted(self.parts, key=lambda p: p[0] == '.sav')
        targets = [(directory / (basename + s), data) for s, data in ordered]
        for target, data in targets:
            if target.exists() and target.read_bytes() != data:
                raise FileExistsError('snapshot name already contains a different save')
        if '.jpg' not in dict(self.parts) and (directory / (basename + '.jpg')).exists():
            raise FileExistsError('snapshot name has an unrelated preview')
        created = []
        try:
            for target, data in targets:
                try:
                    stream = target.open('xb')
                except FileExistsError:
                    if target.read_bytes() != data:
                        raise
                    continue
                created.append(target)
                with stream:
                    stream.write(data)
                    stream.flush()
                    os.fsync(stream.fileno())
            installed = self.read(directory / (basename + '.sav'))
            if installed.digest != self.digest:
                raise ValueError('installed snapshot verification failed')
        except Exception:
            for target in reversed(created):
                target.unlink(missing_ok=True)
            raise
        return directory / (basename + '.sav')
