"""Network impairment for one lobby instance (2026-09-17): a fraction of the
outbound UDP datagrams dropped, the rest delayed, so a sandboxed instance on
the rig plays like a player on a bad link.

    <io dir>/tpf2mp_netsim.txt
        loss=0.05        drop this fraction of outbound datagrams
        delay=0.100      seconds every surviving datagram (and every TCP link
                         frame, dual_tcp) waits before it leaves
        jitter=0.020     seconds of extra random delay, uniform

Outbound only (each side impairs what it sends, so a two-sided setup is two
files), sealed and plain frames alike, save chunks included. The TCP backup
link is delayed by the same amount but never dropped here: TCP's own loss is
the OS's, and dropping frames before the socket would just be a queue drop.
Read once at start; absent = no impairment. Lines start with "[netsim]".
"""
import heapq
import os
import random
import threading
import time


def read_config(io_dir):
    """{'loss': .., 'delay': .., 'jitter': ..} or None when the file is absent/empty."""
    try:
        with open(os.path.join(io_dir, "tpf2mp_netsim.txt"), "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    cfg = {"loss": 0.0, "delay": 0.0, "jitter": 0.0}
    for line in text.splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip().lower()
        if k in cfg:
            try:
                cfg[k] = max(0.0, float(v.strip()))
            except ValueError:
                pass
    cfg["loss"] = min(cfg["loss"], 0.99)
    if not any(cfg.values()):
        return None
    return cfg


def describe(cfg):
    return (f"loss {cfg['loss'] * 100:.0f}%, delay {cfg['delay'] * 1000:.0f} ms"
            + (f" +{cfg['jitter'] * 1000:.0f} ms jitter" if cfg["jitter"] else ""))


class ImpairedSocket:
    """Wraps a UDP socket: sendto drops and delays; everything else is the
    wrapped socket's. Delayed datagrams leave from a timer thread in send
    order (a delay never reorders what one side sends)."""

    def __init__(self, udp, cfg, log=lambda _: None, rng=None):
        self.udp = udp
        self.loss, self.delay, self.jitter = cfg["loss"], cfg["delay"], cfg["jitter"]
        self.log = log
        self.rng = rng or random.Random()
        self.dropped = self.delayed = self.sent = 0
        self._heap = []
        self._seq = 0
        self._cv = threading.Condition()
        self._stop = False
        self._pump_started = False
        if self.delay or self.jitter:
            self.start_pump()

    def start_pump(self):
        """The delay thread; started at construction when a delay is set, or
        later by a test that raises the delay on a live socket."""
        if not self._pump_started:
            self._pump_started = True
            threading.Thread(target=self._pump, name="netsim", daemon=True).start()

    def __getattr__(self, name):
        return getattr(self.udp, name)

    def fileno(self):
        return self.udp.fileno()

    def sendto(self, data, addr):
        if self.loss and self.rng.random() < self.loss:
            self.dropped += 1
            return len(data)
        if not (self.delay or self.jitter):
            self.sent += 1
            return self.udp.sendto(data, addr)
        due = time.time() + self.delay + (self.rng.random() * self.jitter if self.jitter else 0.0)
        with self._cv:
            self._seq += 1
            heapq.heappush(self._heap, (due, self._seq, data, addr))
            self.delayed += 1
            self._cv.notify()
        return len(data)

    def _pump(self):
        while not self._stop:
            with self._cv:
                while not self._heap and not self._stop:
                    self._cv.wait()
                if self._stop:
                    return
                due, _, data, addr = self._heap[0]
                wait = due - time.time()
                if wait > 0:
                    self._cv.wait(wait)
                    continue
                heapq.heappop(self._heap)
            try:
                self.udp.sendto(data, addr)
                self.sent += 1
            except OSError:
                pass

    def close(self):
        with self._cv:
            self._stop = True
            self._cv.notify()
        return self.udp.close()

    def summary(self):
        return f"[netsim] sent {self.sent}, delayed {self.delayed}, dropped {self.dropped}"
