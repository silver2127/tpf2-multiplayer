"""Offline, lossless per-tile benchmark; does not modify the game or saves.

Input: concatenated 257 x 257 little-endian uint16 cache samples.
Each tile is compressed independently, as required for random tile access.
"""
import argparse
import hashlib
import json
import time
from pathlib import Path

import numpy as np
import zstandard as zstd


def transform(raw, mode):
    a = np.frombuffer(raw, dtype='<u2').reshape(257, 257)
    if mode.startswith('delta'):
        b = a.copy()
        b[:, 1:] = a[:, 1:] - a[:, :-1]
        a = b
    if mode.endswith('shuffle'):
        return a.view(np.uint8).reshape(-1, 2).T.copy().tobytes()
    return a.tobytes()


def restore(raw, mode):
    if mode.endswith('shuffle'):
        raw = np.frombuffer(raw, dtype=np.uint8).reshape(2, -1).T.copy().tobytes()
    a = np.frombuffer(raw, dtype='<u2').reshape(257, 257)
    if mode.startswith('delta'):
        a = np.cumsum(a, axis=1, dtype=np.uint16)
    return a.tobytes()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('samples', type=Path)
    p.add_argument('--total-tiles', type=int, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    data = args.samples.read_bytes()
    size = 257 * 257 * 2
    assert data and len(data) % size == 0
    tiles = [data[i:i+size] for i in range(0, len(data), size)]
    report = dict(sample_count=len(tiles), sample_sha256=hashlib.sha256(data).hexdigest(),
                  total_tiles=args.total_tiles, raw_cache_gib=args.total_tiles*size/2**30,
                  constant_samples=sum(len(set(np.frombuffer(t, dtype='<u2'))) == 1 for t in tiles),
                  unique_samples=len(set(tiles)), results=[])
    for mode in ('raw', 'shuffle', 'delta', 'delta-shuffle'):
        c = zstd.ZstdCompressor(level=1)
        d = zstd.ZstdDecompressor()
        start = time.perf_counter()
        packed = [c.compress(transform(t, mode)) for t in tiles]
        encode = time.perf_counter() - start
        start = time.perf_counter()
        restored = [restore(d.decompress(t), mode) for t in packed]
        decode = time.perf_counter() - start
        assert restored == tiles, mode
        ratio = sum(map(len, packed)) / len(data)
        report['results'].append(dict(mode=mode, ratio=ratio,
            projected_payload_gib=report['raw_cache_gib']*ratio,
            encode_us_per_tile=encode/len(tiles)*1e6,
            decode_us_per_tile=decode/len(tiles)*1e6, exact_roundtrip=True))
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
