#!/usr/bin/env python3
"""Run the collector's actual masking block against staged JSON/JSONL/text copies."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('collect_logs.sh').read_text()
block = source[source.index("MASK_KEYS="):source.index('# ---- 5. the archive')]
with tempfile.TemporaryDirectory(dir=Path.cwd()) as temp:
    stage = Path(temp)
    record = {key: 'secret"\\value' for key in (
        'code', 'cross_code', 'steam', 'steam_code', 'steam_secret',
        'password', 'pass', 'passcode', 'secret')}
    record.update(type='code', players=['A', 'B'], crossplay=True)
    original = json.dumps(record)
    for suffix in ('json', 'jsonl', 'txt'):
        (stage / ('lobby file.' + suffix)).write_text(original + '\n' + original + '\n')
    env = dict(os.environ, STAGE=str(stage))
    subprocess.run(['bash', '-ec', 'note() { :; };\n' + block], env=env, check=True)
    for file in stage.iterdir():
        for line in file.read_text().splitlines():
            got = json.loads(line)
            for key in record:
                assert got[key] == (record[key] if key in ('type', 'players', 'crossplay') else '<masked>')
print('PASS: collector masks all nine keys, escaped quotes/backslashes, multiple lines, and spaced filenames')
