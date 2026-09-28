"""A late joiner gets a FRESH save (2026-09-28, user: "make sure saves are new
and fresh"). Real loopback lobby; the host's world and menu are simulated.

A host that shared a save more than HOTJOIN_SAVE_MAX_AGE ago does not push
that file to a newcomer: it asks its menu for a new save (tpf2_sync_save.txt)
and waits for it. A recent save still goes out as before.

    python tools/test_fresh_hotjoin_save.py
"""
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import lobby
from sync_runtime import SyncParticipant


class WaitingEngine(SyncParticipant):
    def _tick(self):
        return None               # no game process: never acknowledge engine work


class FreshSave(unittest.TestCase):
    def scenario(self, age):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ios = {n: lobby.LobbyIO(str(root / n)) for n in ('host', 'first', 'late')}
            runtimes = {n: WaitingEngine(root / n, root / n, 123, n) for n in ios}
            (root / 'host' / 'tpf2mp_live_join.txt').write_text('1\n')
            runtimes['host']._write('tpf2_native_status.txt', {'has_world': 1})
            save = root / 'saves' / 'autosave_mp_shared_1975-06-11.sav'
            save.parent.mkdir()
            save.write_bytes(b'world' * 4096)
            Path(str(save) + '.lua').write_text('["lockstep.lua"] = {}')
            request = root / 'host' / 'tpf2_sync_save.txt'
            stop = threading.Event()
            sock = lobby.open_socket(0, lobby.socket.AF_INET)
            logs = []
            workers = [threading.Thread(target=lobby.run_host, args=(sock, 'host', ios['host']),
                                        kwargs=dict(stop=stop, log=logs.append, sync_runtime=runtimes['host']))]
            workers[0].start()
            conns = []

            def wait(predicate, what, timeout=15):
                self.assertTrue(lobby._wait_until(predicate, timeout=timeout), what + '\n' + '\n'.join(logs[-40:]))

            def join(name):
                c = lobby._dial_loopback(0, sock.getsockname()[1], 5)
                self.assertIsNotNone(c)
                conns.append(c)
                t = threading.Thread(target=lobby.run_client, args=(c, name, ios[name]),
                                     kwargs=dict(stop=stop, log=logs.append, sync_runtime=runtimes[name]))
                t.start()
                workers.append(t)

            try:
                # the first player comes in the ordinary way: the menu sees the
                # newcomer, takes its hot-join save and shares it (a start command)
                join('first')
                wait(lambda: any("live join for 'first'" in x for x in logs), 'the first joiner is a live join')
                with open(ios['host'].in_path, 'a', encoding='utf8') as f:
                    f.write(json.dumps({'cmd': 'start', 'save': str(save)}) + '\n')
                wait(lambda: lobby._has_start(ios['first'].out_path, save=True), 'the first joiner starts')
                # time passes: the save is `age` seconds old when the next player arrives
                old = time.time() - age
                os.utime(save, (old, old))
                join('late')
                wait(lambda: any("live join for 'late'" in x for x in logs), 'the late joiner is a live join')
                if age > 120:                                     # HOTJOIN_SAVE_MAX_AGE
                    wait(request.exists, 'an old save: the host asks its menu for a fresh one')
                    self.assertEqual(request.read_text(), 'fresh save\n')
                    time.sleep(2.5)                                # several serve-again sweeps
                    self.assertFalse(any('pushing it again' in x for x in logs), 'the old save must not go out')
                    self.assertFalse(lobby._has_start(ios['late'].out_path, save=True))
                    # Simulate the menu finishing its new save and lifting the hold.
                    fresh = save.with_name('autosave_mp_shared_fresh.sav')
                    fresh.write_bytes(b'fresh world' * 4096)
                    Path(str(fresh) + '.lua').write_text('["lockstep.lua"] = {}')
                    with open(ios['host'].in_path, 'a', encoding='utf8') as f:
                        f.write(json.dumps({'cmd': 'start', 'save': str(fresh)}) + '\n')
                    wait(lambda: lobby._has_start(ios['late'].out_path, save=True),
                         'the fresh save releases the late joiner')
                else:
                    wait(lambda: any('pushing it again' in x for x in logs), 'a recent save goes out as before')
                    self.assertFalse(any('fresh save' in x for x in logs))
                    wait(lambda: lobby._has_start(ios['late'].out_path, save=True), 'the late joiner starts')
                return logs
            finally:
                stop.set()
                for c in conns:
                    c.close()
                for w in workers:
                    w.join(5)
                sock.close()

    def test_a_slow_save_read_does_not_stall_the_lobby(self):
        """Reading and hashing the save runs on a worker (2026-09-28: the dedicated
        server stood 12.9 s doing it on the loop, and two joiners gave up on it)."""
        real = lobby._read_save_files

        def slow(path):
            time.sleep(3.0)                    # a machine short of memory
            return real(path)
        lobby._read_save_files = slow
        try:
            logs = self.scenario(age=10)
        finally:
            lobby._read_save_files = real
        self.assertFalse([x for x in logs if 'the lobby stood' in x], 'the host loop stood while the save was read')

    def test_a_slow_mod_scan_does_not_stall_the_lobby(self):
        """Save mod discovery must stay on the worker too, including on Linux."""
        real = lobby.modshare.save_mod_list
        worker_calls = []

        def slow(path, log=None):
            if threading.current_thread().name == 'save-read':
                worker_calls.append(path)
            time.sleep(3.0)
            return real(path, log)

        with mock.patch.object(lobby.modshare, 'save_mod_list', side_effect=slow):
            logs = self.scenario(age=10)
        self.assertGreaterEqual(len(worker_calls), 2, 'initial and late-join save preparation')
        self.assertFalse([x for x in logs if 'the lobby stood' in x],
                         'the host loop stood while discovering save mods')

    def test_an_old_save_is_taken_again(self):
        self.scenario(age=600)

    def test_a_recent_save_goes_out(self):
        self.scenario(age=10)


if __name__ == '__main__':
    unittest.main()
