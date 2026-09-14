"""Production lobby/UDP/save transport with a simulated engine, not a game test."""
import argparse
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
    blocked_phase = None
    blocked_seen = False

    def _tick(self):
        phase = self.state['phase']
        if phase == self.blocked_phase:
            self.blocked_seen = True
            return None
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


parser = argparse.ArgumentParser()
parser.add_argument('--players', type=int, choices=range(2, lobby.CAP + 1), default=2)
players = parser.parse_args().players
names = ('host', 'client') + tuple(f'client{i}' for i in range(3, players + 1))
last = names[-1]
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    ios = {name: lobby.LobbyIO(str(root / name)) for name in names}
    runtimes = {name: EngineStandIn(root / name, root / name, 123, name) for name in ios}
    stop = threading.Event()
    last_stop = threading.Event()
    host = lobby.open_socket(0, socket.AF_INET)
    workers = [threading.Thread(target=lobby.run_host,
        args=(host, 'host', ios['host']), kwargs=dict(stop=stop, log=lambda _: None,
                                                    sync_runtime=runtimes['host']))]
    connections = []
    conn = None
    workers[0].start()
    try:
        conn = lobby._dial_loopback(0, host.getsockname()[1], 5)
        assert conn is not None
        connections.append(conn)
        # Lose initial control messages and acknowledgements. The real transfer
        # and recovery adapter must recover them without repeating engine I/O.
        original_send = conn.send
        lost = {'sync_ack'} | ({'sync_ready'} if players > 2 else set())
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
        for name in names[2:]:
            extra = lobby._dial_loopback(0, host.getsockname()[1], 5)
            assert extra is not None
            connections.append(extra)
            workers.append(threading.Thread(target=lobby.run_client,
                args=(extra, name, ios[name]), kwargs=dict(stop=last_stop if name == last else stop,
                    log=lambda _: None, sync_runtime=runtimes[name])))
            workers[-1].start()
        assert lobby._wait_until(lambda: all(len((lobby._latest_roster(io.out_path) or {}).get('players', [])) == players
                                            for io in ios.values()), timeout=5)
        def command(who, **fields):
            with open(ios[who].in_path, 'a', encoding='utf-8') as stream:
                stream.write(json.dumps(fields) + '\n')
        def wait_for(predicate):
            assert lobby._wait_until(predicate, timeout=15), {name: r.state for name, r in runtimes.items()}
        ready_token = None
        def ready_event(name):
            events = [json.loads(line) for line in Path(ios[name].out_path).read_text(encoding='utf-8').splitlines()]
            return next((e for e in reversed(events) if e.get('type') == 'sync_ready_state'), {})
        def confirm_ready():
            global ready_token
            if players == 2:
                return
            old_token = ready_token
            wait_for(lambda: all(ready_event(n).get('phase') == 'waiting' and
                ready_event(n).get('token') != old_token for n in names))
            ready_token = ready_event('host')['token']
            before = [(r.saves, r.loads, r.state and r.state['revision']) for r in runtimes.values()]
            for name in names[1:-1]:
                command(name, cmd='sync_ready', token=ready_token, id='ready-'+ready_token+name)
            wait_for(lambda: ready_event('host').get('ready_count') == players-1)
            # A stale or invented confirmation must never count for the last player.
            command(last, cmd='sync_ready', token=old_token or 'wrong', id='stale-'+ready_token)
            command(last, cmd='chat', text='ready-check-'+ready_token)
            wait_for(lambda: lobby._has_chat(ios['host'].out_path, 'ready-check-'+ready_token))
            assert ready_event('host')['ready_count'] == players-1
            assert before == [(r.saves, r.loads, r.state and r.state['revision']) for r in runtimes.values()]
            command(last, cmd='sync_ready', token=ready_token, id='ready-'+ready_token+last)
        command('host', cmd='start')
        wait_for(lambda: all(lobby._has_start(io.out_path) for io in ios.values()))
        wait_for(lambda: bool(runtimes['client']._read('tpf2_sync_available.txt')))
        # Both local GUI notices reach their native panels without starting recovery.
        for r in runtimes.values():
            r._write('tpf2_sync_notice.txt', dict(world='before', wall=int(time.time()), desyncs=1, held=0))
        def has_prompt(io):
            return any(json.loads(line).get('type') == 'sync_prompt' and
                       json.loads(line).get('phase') == 'detected'
                       for line in Path(io.out_path).read_text(encoding='utf-8').splitlines())
        wait_for(lambda: all(has_prompt(io) for io in ios.values()))
        assert all(r.state is None and r.saves == 0 and r.loads == 0 for r in runtimes.values())
        if players >= 3:
            runtimes[last].blocked_phase = 'holding'
        # Bypass the client UI/adapter: the authenticated host must reject this too.
        original_send(json.dumps(dict(t='sync_request', id='forged-client-start', sender='host')).encode())
        command('client', cmd='chat', text='client-start-rejected')
        wait_for(lambda: lobby._has_chat(ios['host'].out_path, 'client-start-rejected'))
        assert all(r.state is None for r in runtimes.values())
        command('host', cmd='sync_request', id='recover-once')
        confirm_ready()
        if players >= 3:
            wait_for(lambda: runtimes[last].blocked_seen)
            command(last, cmd='sync_request', id='simultaneous-third')
            command(last, cmd='chat', text='third requested while held')
            wait_for(lambda: lobby._has_chat(ios['host'].out_path, 'third requested while held'))
            assert all(r.state['phase'] == 'holding' and r.saves == 0 and r.loads == 0 for r in runtimes.values())
            runtimes[last].blocked_phase = None
        wait_for(lambda: all(r.finished for r in runtimes.values()))
        first = runtimes['host'].state['operation']
        assert all(r.state['operation'] == first for r in runtimes.values())
        assert runtimes['host'].state['resume_speed'] == 0
        assert runtimes['host'].saves == 1 and all(r.loads == 1 for r in runtimes.values())
        assert not lost
        command('host', cmd='sync_request', id='recover-once')
        command('client', cmd='chat', text='same lobby after resync')
        wait_for(lambda: lobby._has_chat(ios['host'].out_path, 'same lobby after resync'))
        assert runtimes['host'].state['operation'] == first
        # A mismatching fresh fingerprint must hold everyone. Retry reuses the
        # immutable host snapshot under a NEW world epoch and transfers again.
        lagger = runtimes[names[-1]]
        lagger.mismatch = True
        command('host', cmd='sync_request', id='second-operation')
        confirm_ready()
        wait_for(lambda: all(r.state['phase'] == 'error' for r in runtimes.values()))
        state = runtimes['host'].state
        operation, epoch = state['operation'], state['epoch']
        assert state['error']['step'] == 'checking'
        assert runtimes['host'].saves == 2
        lagger.mismatch = False
        # The retained file-command adapter supports diagnostic retry requests too.
        original_send(json.dumps(dict(t='sync_retry', id='client-retry', operation=operation)).encode())
        command('client', cmd='chat', text='client-retry-rejected')
        wait_for(lambda: lobby._has_chat(ios['host'].out_path, 'client-retry-rejected'))
        assert all(r.state['phase'] == 'error' and r.state['epoch'] == epoch for r in runtimes.values())
        runtimes['host']._write('tpf2_sync_request.txt', dict(cmd='sync_retry', id='retry', operation=operation))
        confirm_ready()
        wait_for(lambda: all(r.finished and r.state['epoch'] != epoch for r in runtimes.values()))
        assert runtimes['host'].saves == 2
        assert all(r.state['operation'] == operation and r.loads == 3 for r in runtimes.values())
        assert all(worker.is_alive() for worker in workers)
        if players >= 3:
            lagger.blocked_phase = 'checking'
            lagger.blocked_seen = False
            command('host', cmd='sync_request', id='disconnect-third')
            confirm_ready()
            wait_for(lambda: lagger.blocked_seen)
            assert all(r.state['phase'] == 'checking' and not r.finished for r in runtimes.values())
            command(last, cmd='quit')
            wait_for(lambda: all(runtimes[n].state['phase'] == 'error' for n in names[:-1]))
            assert runtimes['host'].state['error']['step'] == 'checking'
            assert 'disconnected' in runtimes['host'].state['error']['detail']
            assert all(not r.finished for r in runtimes.values())

    finally:
        stop.set()
        last_stop.set()
        for worker in workers:
            worker.join(5)
        for connection in connections:
            connection.close()
        assert all(not worker.is_alive() for worker in workers)
print(f'PASS ({players} players): host-only resync/retry, all-player readiness, stale confirmation rejected, lost control/ACK recovery, snapshot, pause, mismatch/retry and disconnect; engine simulated')
