"""Real loopback lobby; native world status is simulated, no game is launched."""
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import lobby
from sync_runtime import SyncParticipant


class WaitingEngine(SyncParticipant):
    def _tick(self):
        # Deliberately leave any recovery at holding: this fixture has no game
        # process and must not inspect a real PID or acknowledge engine work.
        return None


class LiveJoinTest(unittest.TestCase):
    def scenario(self, world_before_join, enabled):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ios = {n: lobby.LobbyIO(str(root / n)) for n in ('host', 'client')}
            runtimes = {n: WaitingEngine(root / n, root / n, 123, n) for n in ios}
            if enabled:
                (root / 'host' / 'tpf2mp_live_join.txt').write_text('1\n')
            def world():
                runtimes['host']._write('tpf2_native_status.txt', {'has_world': 1})
            if world_before_join:
                world()
            stop = threading.Event()
            sock = lobby.open_socket(0, lobby.socket.AF_INET)
            logs = []
            workers = [threading.Thread(target=lobby.run_host,
                args=(sock, 'host', ios['host']), kwargs=dict(stop=stop,
                log=logs.append, sync_runtime=runtimes['host']))]
            connection = None
            workers[0].start()
            def wait(predicate):
                self.assertTrue(lobby._wait_until(predicate, timeout=8), logs)
            try:
                connection = lobby._dial_loopback(0, sock.getsockname()[1], 5)
                self.assertIsNotNone(connection)
                workers.append(threading.Thread(target=lobby.run_client,
                    args=(connection, 'client', ios['client']), kwargs=dict(stop=stop,
                    log=logs.append, sync_runtime=runtimes['client'])))
                workers[-1].start()
                wait(lambda: len((lobby._latest_roster(ios['host'].out_path) or {}).get('players', [])) == 2)
                if not world_before_join:
                    self.assertIsNone(runtimes['host'].state)
                    world()
                request = root / 'host' / 'tpf2_sync_save.txt'
                if enabled:
                    wait(lambda: any('live join for' in line for line in logs))
                    if not world_before_join:
                        wait(request.exists)
                        self.assertEqual(request.read_text(), 'live join\n')
                        request.unlink()  # Simulate menu consumption, but no engine save.
                    # Multiple real host sweeps must neither re-request nor start a round.
                    deadline = time.monotonic() + 1.5
                    while time.monotonic() < deadline:
                        self.assertTrue(all(w.is_alive() for w in workers), logs)
                        self.assertTrue(all(r.state is None for r in runtimes.values()), logs)
                        self.assertFalse(request.exists(), logs)
                        time.sleep(.02)
                    self.assertFalse(lobby._latest_roster(ios['host'].out_path)['join_freeze'])
                    # Flag changes must not reclassify this already admitted live join.
                    (root / 'host' / 'tpf2mp_live_join.txt').write_text('0\n')
                    time.sleep(.3)
                    self.assertIsNone(runtimes['host'].state)
                else:
                    wait(lambda: runtimes['host'].state is not None)
                    self.assertEqual(runtimes['host'].state['mode'], 'join')
                    self.assertEqual(runtimes['host'].state['phase'], 'holding')
                    self.assertFalse(request.exists())
            finally:
                stop.set()
                for worker in workers:
                    worker.join(5)
                if connection:
                    connection.close()
                sock.close()
                self.assertTrue(all(not w.is_alive() for w in workers))

    def test_host_loaded_alone_live(self):
        self.scenario(True, True)

    def test_waiting_member_live(self):
        self.scenario(False, True)

    def test_host_loaded_alone_default_frozen(self):
        self.scenario(True, False)

    def test_waiting_member_default_frozen(self):
        self.scenario(False, False)


if __name__ == '__main__':
    unittest.main()
