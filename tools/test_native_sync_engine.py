"""Opt-in ONE-instance engine probe through the production control files.

Starts with an explicitly named save, then saves/reloads a recovery copy in the
same process. Local phases are driven by this test, not by real remote players;
this is not a multiplayer acceptance test. Output belongs under .local-test.
"""
import argparse
import json
from pathlib import Path
import secrets
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
from sync_runtime import SyncParticipant, read_fields, write_fields


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--save-dir', required=True)
    parser.add_argument('--source', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    runtime = SyncParticipant(args.runtime, args.save_dir, args.pid, 'engine-probe')
    runtime.selected_save = args.source
    status = runtime._read('tpf2_native_status.txt')
    if status.get('supported') != '1':
        raise RuntimeError('Current process has no supported production native adapter')
    # Keep the bridge's existing role/peer, but address this running process.
    control = read_fields(Path(args.runtime)/'tpf2_bridge_ctl.txt')
    control.update(pid=args.pid, instance='a', peer='127.0.0.1:7773', players=2)
    write_fields(Path(args.runtime)/'tpf2_bridge_ctl.txt', control)
    revision = int(time.time()*1000)
    with out.open('a', encoding='utf-8') as log:
        for mode in ('start', 'resync'):
            state = dict(operation=secrets.token_hex(16), epoch=secrets.token_hex(16),
                         mode=mode, members=['engine-probe'], host='engine-probe',
                         snapshot=None, resume_speed=0, error=None)
            for phase in ('holding', 'saving', 'transferring', 'loading', 'checking', 'releasing', 'complete'):
                revision += 1
                state.update(phase=phase, revision=revision)
                runtime.accept(state)
                started = time.monotonic()
                while time.monotonic()-started < 180:
                    ack = runtime.tick()
                    if ack or runtime.finished:
                        break
                    time.sleep(0.1)
                else:
                    raise TimeoutError(f'{mode}/{phase}: no engine completion')
                event = dict(mode=mode, phase=phase, elapsed=round(time.monotonic()-started, 2),
                             ack=ack, finished=runtime.finished)
                log.write(json.dumps(event)+'\n'); log.flush()
                print(json.dumps(event), flush=True)
                if ack and not ack['success']:
                    raise RuntimeError(f'{mode}/{phase}: {ack.get("detail")}')
                if phase == 'saving':
                    state['snapshot'] = dict(files=runtime.snapshot.files, digest=runtime.snapshot.digest)
    print('PASS: single real engine exact start/save/reload with pause and new-world checks; no multiplayer claim')


if __name__ == '__main__':
    main()
