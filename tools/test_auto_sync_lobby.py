"""Production lobby/UDP/save transport with a simulated engine, not a game test."""
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import lobby
from sync_runtime import SyncParticipant
from sync_snapshot import PreparedSnapshot


class EngineStandIn(SyncParticipant):
    saves = 0
    loads = 0
    mismatch = False

    def _tick(self):
        phase = self.state['phase']
        if phase == 'holding':
            return self._ack(paused=True, drained=True, speed=0)
        if phase == 'saving' and self.player == self.state['host']:
            self.saves += 1
            self.snapshot = PreparedSnapshot((('.sav', b'world' * 10000), ('.sav.lua', b'meta')))
            return self._ack(files=self.snapshot.files)
        if phase == 'transferring' and self.snapshot:
            self.snapshot.install(self.save_directory, 'mp_' + self.state['epoch'][:12])
            return self._ack(digest=self.snapshot.digest)
        if phase in ('loading', 'checking', 'releasing'):
            assert self.snapshot and self.snapshot.digest == self.state['snapshot']['digest']
            if phase == 'loading':
                self.loads += 1
            return self._ack(paused=True, digest=self.snapshot.digest,
                             fingerprint='different' if self.mismatch else 'fresh same world')
        if phase == 'complete':
            self.finished = True


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    ios = {name: lobby.LobbyIO(str(root / name)) for name in ('host', 'client')}
    runtimes = {name: EngineStandIn(root / name, root / name, 123, name) for name in ios}
    stop = threading.Event()
    host = lobby.open_socket(0, socket.AF_INET)
    workers = [threading.Thread(target=lobby.run_host,
        args=(host, 'host', ios['host']), kwargs=dict(stop=stop, log=lambda _: None,
                                                    sync_runtime=runtimes['host']))]
    conn = None
    workers[0].start()
    try:
        conn = lobby._dial_loopback(0, host.getsockname()[1], 5)
        assert conn is not None
        # Lose initial control messages and acknowledgements. The real transfer
        # and recovery adapter must recover them without repeating engine I/O.
        original_send = conn.send
        lost = {'sync_request', 'sync_ack'}
        def lossy_send(raw):
            message = json.loads(raw)
            if message.get('t') in lost:
                lost.remove(message['t'])
                return
            original_send(raw)
        conn.send = lossy_send
        workers.append(threading.Thread(target=lobby.run_client,
            args=(conn, 'client', ios['client']), kwargs=dict(stop=stop, log=lambda _: None,
                                                           sync_runtime=runtimes['client'])))
        workers[-1].start()
        assert lobby._wait_until(lambda: all(len((lobby._latest_roster(io.out_path) or {}).get('players', [])) == 2
                                            for io in ios.values()), timeout=5)
        def command(who, **fields):
            with open(ios[who].in_path, 'a', encoding='utf-8') as stream:
                stream.write(json.dumps(fields) + '\n')
        def wait_for(predicate):
            assert lobby._wait_until(predicate, timeout=15), {name: r.state for name, r in runtimes.items()}
        command('host', cmd='start')
        wait_for(lambda: all(lobby._has_start(io.out_path) for io in ios.values()))
        wait_for(lambda: bool(runtimes['client']._read('tpf2_sync_available.txt')))
        command('client', cmd='sync_request', id='recover-once')
        wait_for(lambda: all(r.finished for r in runtimes.values()))
        first = runtimes['host'].state['operation']
        assert runtimes['client'].state['operation'] == first
        assert runtimes['host'].state['resume_speed'] == 0
        assert runtimes['host'].saves == 1 and all(r.loads == 1 for r in runtimes.values())
        assert not lost
        command('client', cmd='sync_request', id='recover-once')
        command('client', cmd='chat', text='same lobby after resync')
        wait_for(lambda: lobby._has_chat(ios['host'].out_path, 'same lobby after resync'))
        assert runtimes['host'].state['operation'] == first
        # A mismatching fresh fingerprint must hold everyone. Retry reuses the
        # immutable host snapshot under a NEW world epoch and transfers again.
        runtimes['client'].mismatch = True
        command('host', cmd='sync_request', id='second-operation')
        wait_for(lambda: all(r.state['phase'] == 'error' for r in runtimes.values()))
        state = runtimes['host'].state
        operation, epoch = state['operation'], state['epoch']
        assert state['error']['step'] == 'checking'
        assert runtimes['host'].saves == 2
        runtimes['client'].mismatch = False
        # through tpf2_sync_request.txt, as the Multiplayer window's Retry button writes it
        runtimes['client']._write('tpf2_sync_request.txt', dict(cmd='sync_retry', id='retry', operation=operation))
        wait_for(lambda: all(r.finished and r.state['epoch'] != epoch for r in runtimes.values()))
        assert runtimes['host'].saves == 2
        assert all(r.state['operation'] == operation and r.loads == 3 for r in runtimes.values())
        assert all(worker.is_alive() for worker in workers)
    finally:
        stop.set()
        for worker in workers:
            worker.join(5)
        if conn:
            conn.close()
        assert all(not worker.is_alive() for worker in workers)
print('PASS: real UDP resync, lost control/ACK recovery, exact snapshot transfer, pause preserved, duplicate request, mismatch and retry; engine simulated')
