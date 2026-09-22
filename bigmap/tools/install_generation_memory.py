"""Install/restore conservative buffer reuse in the three stock generators.

Keeps exact backups and refuses changes to a generator modified since install.
Does not restart the game or change native DLL/configuration.
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATORS = ('desert', 'temperate', 'tropical')
ANCHOR = b'\t\treturn result'
REPLACEMENT = b'\t\treturn require("terrain/bigmap_memory").Optimize(result) -- tpf2_bigmap memory'

def patched(original):
    if original.count(ANCHOR) != 1 or b'tpf2_bigmap memory' in original:
        raise ValueError('generator return anchor is missing, duplicated or already modified')
    return original.replace(ANCHOR, REPLACEMENT)

def install(res, restore=False):
    # Preflight every file before the first write.
    changes = []
    for name in GENERATORS:
        path = res / 'config/terrain_generators' / (name + '.gen.lua')
        backup = path.with_name(path.name + '.bigmap-memory.bak')
        current = path.read_bytes()
        original = backup.read_bytes() if backup.exists() else current
        changed = patched(original)
        if current not in (original, changed):
            raise ValueError(f'{path}: changed since backup; refusing overwrite')
        if not restore and not backup.exists(): changes.append((backup, None, original))
        target = original if restore else changed
        if target != current: changes.append((path, current, target))
    if not restore:
        module = res / 'scripts/terrain/bigmap_memory.lua'
        desired = (ROOT / 'mod/generation/bigmap_memory.lua').read_bytes()
        current = module.read_bytes() if module.exists() else None
        if current is not None and current != desired:
            raise ValueError(f'{module}: existing differing module; refusing overwrite')
        if current != desired: changes.insert(0, (module, current, desired))
    applied = []
    try:
        for path, before, after in changes:
            path.parent.mkdir(parents=True, exist_ok=True)
            applied.append((path, before))
            path.write_bytes(after)
    except BaseException:
        for path, before in reversed(applied):
            if before is None: path.unlink(missing_ok=True)
            else: path.write_bytes(before)
        raise
    return len(changes)

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--res', type=Path, default=Path(
        r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\res'))
    ap.add_argument('--restore', action='store_true')
    args = ap.parse_args()
    count = install(args.res, args.restore)
    print(f'{"Restored" if args.restore else "Installed"} terrain memory mode: {count} file changes')
