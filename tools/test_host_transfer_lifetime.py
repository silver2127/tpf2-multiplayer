"""Exercise host-loop ownership across passes, without a game or large payloads.

Inject synthetic senders at the real loop boundary using tracing, since run_host
owns its transfer collections in a closure. Only weak references escape; the
production loop must release completed senders while it is still running.
"""
import inspect
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
import weakref

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import lobby


class Sender:
    def __init__(self, kind, outcome):
        self.kind = kind
        self.outcome = outcome
        self.peers = {}
        self.sid = 'fixture'
        self.pumps = 0
        self.payload = bytearray(1024)

    def pump(self, now):
        self.pumps += 1
        if self.pumps == 2 and self.outcome == 'error':
            raise RuntimeError('synthetic pump failure')

    def all_resolved(self): return self.pumps >= 2
    def awaiting_answers(self, now): return False
    def failed_names(self): return ['dropped'] if self.outcome == 'failed' else []
    def done_count(self): return 0


class HostTransferLifetimeTest(unittest.TestCase):
    def test_ownership_between_passes(self):
        lines, start = inspect.getsourcelines(lobby.run_host)
        boundary = start + next(i for i, line in enumerate(lines)
                                if line.strip() == 'while not stop.is_set():')
        for kind, outcome in [('save', 'done'), ('mods', 'done'),
                              ('save', 'failed'), ('save', 'error')]:
            with self.subTest(kind=kind, outcome=outcome), tempfile.TemporaryDirectory() as root:
                stop = threading.Event()
                refs = []
                observations = []
                passes = 0
                deadline = time.monotonic() + 5

                def trace(frame, event, arg):
                    nonlocal passes
                    if frame.f_code is not lobby.run_host.__code__:
                        return None
                    if time.monotonic() > deadline:
                        stop.set()
                    if event == 'line' and frame.f_lineno == boundary:
                        local = frame.f_locals
                        passes += 1
                        if passes == 1:
                            save = Sender(kind, outcome)
                            terrain = Sender('terr', outcome)
                            refs.extend((weakref.ref(save), weakref.ref(terrain)))
                            local['transfer'][0] = save
                            local['terr_streams'].append(terrain)
                        else:
                            observations.append(tuple(ref() is not None for ref in refs))
                            if passes == 3:
                                stop.set()
                    return trace

                sock = lobby.open_socket(0, lobby.socket.AF_INET)
                previous = sys.gettrace()
                try:
                    sys.settrace(trace)
                    lobby.run_host(sock, 'host', lobby.LobbyIO(root), stop=stop,
                                   log=lambda _: None)
                finally:
                    sys.settrace(previous)
                    sock.close()
                self.assertEqual(passes, 3, 'host did not reach the loop boundary')
                self.assertEqual(observations, [(True, True), (False, False)],
                                 'active senders must survive; resolved senders must die before the next transfer')


if __name__ == '__main__':
    unittest.main()
