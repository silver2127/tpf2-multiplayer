#!/usr/bin/env python3
"""
seal.py -- encrypt + authenticate every NP1 DATA frame (stdlib only).

The lobby CODE now carries a random 12-byte session secret (connect.py). Every
member derives the same key from it, plus an OPTIONAL password typed in the
menu; frames are then sealed as NP1 type 'E' instead of plain 'D':

    E-frame payload = nonce(8) || ciphertext || tag(16)
    nonce           = sender salt(4, random per process) || counter(4)
    keystream       = SHA256(enc_key || nonce || block#)  (32 B per block)
    tag             = HMAC-SHA256(mac_key, nonce || ciphertext)[:16]

Encrypt-then-MAC with separate sub-keys; per-sender replay window (64 frames,
bitmap) so a captured frame cannot be replayed. Anyone who did not see the
code (or, with a password, did not know it) can neither read nor inject
frames -- an address alone is no longer enough. The frozen netpunch.exe needs
no new dependency: hashlib + hmac only. Throughput is ~25 MB/s per core, far
above the transport, and game frames are tiny.

This is a pragmatic construction, not a reviewed AEAD; it is the fix for
"plaintext, unauthenticated UDP" in a friends-and-Discord threat model.
"""

from __future__ import annotations

import hashlib
import hmac
import os
import struct
import threading

SECRET_LEN = 12
NONCE_LEN = 8
TAG_LEN = 16
WINDOW = 64

# Domain separator for sign()/unsign(). Without it a signed (plaintext) frame
# and a sealed (encrypted) frame share one MAC construction, so either could
# be presented to the other's verifier. The transport already tells them apart
# by frame type, but the crypto should not depend on that.
SIGN_DOMAIN = b"sign"


def derive_key(secret: bytes, password: str = "") -> bytes:
    return hashlib.sha256(b"tpf2mp-seal-v1|" + bytes(secret) + b"|"
                          + (password or "").encode("utf-8")).digest()


def _keystream(enc_key: bytes, nonce: bytes, n: int) -> bytes:
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(enc_key + nonce + struct.pack("!I", i)).digest()
        i += 1
    return bytes(out[:n])


def _xor(a: bytes, b: bytes) -> bytes:
    return (int.from_bytes(a, "big") ^ int.from_bytes(b, "big")).to_bytes(len(a), "big") if a else b""


class Sealer:
    """One per process; shared by every socket owner (thread-safe counter)."""

    def __init__(self, key: bytes):
        self.enc_key = hashlib.sha256(key + b"|enc").digest()
        self.mac_key = hashlib.sha256(key + b"|mac").digest()
        self.salt = os.urandom(4)
        self.ctr = 0
        self._lock = threading.Lock()
        self._windows = {}          # sender salt -> [highest, bitmap]
        self.rejected = 0

    def seal(self, plain: bytes) -> bytes:
        with self._lock:
            self.ctr += 1
            if self.ctr > 0xFFFFFFFF:
                # This used to be `(self.ctr + 1) & 0xFFFFFFFF`, which wraps the
                # counter back to 0 under an UNCHANGED salt. The nonce is
                # salt(4)||counter(4) and the keystream is a pure function of
                # it, so that repeats a keystream -- and two ciphertexts under
                # one keystream XOR to give away both plaintexts. Roll a fresh
                # salt instead: the nonce is 64 bits and this counter is the
                # only place it can ever collide.
                # Cheap on the receiving side too: it keys its replay window on
                # the salt, so a new salt just opens a new window -- one extra
                # dict entry per 2^32 frames, which at game-frame rates is
                # never.
                self.salt = os.urandom(4)
                self.ctr = 1
            nonce = self.salt + struct.pack("!I", self.ctr)
        ct = _xor(plain, _keystream(self.enc_key, nonce, len(plain)))
        tag = hmac.new(self.mac_key, nonce + ct, hashlib.sha256).digest()[:TAG_LEN]
        return nonce + ct + tag

    # -- authenticate-only (no encryption) ---------------------------------- #
    # For BULK SAVE CHUNKS only. Same nonce, same replay window, same tag --
    # the only thing dropped is the keystream XOR.
    #
    # WHY: the keystream is one SHA-256 per 32 bytes of payload, which measures
    # 60 MB/s, against 614 MB/s for the HMAC that protects it. Sealing a 642 MB
    # save for two peers is ~25 s of pure CPU; signing it is ~2 s. On a save
    # transfer that is the difference between minutes and seconds.
    #
    # WHAT THIS COSTS: confidentiality of the save file, and nothing else.
    # Authenticity and integrity are unchanged (same HMAC over nonce||payload),
    # replay is still rejected by the same window, and every file additionally
    # carries a SHA-256 that the receiver verifies before writing. An observer
    # on the wire could read the map you are playing. Control messages (join,
    # chat, roster, start) keep full encryption.
    #
    # Enabled per-frame by the caller, never globally: see TYPE_ADATA in
    # punch.py and _pack_data(bulk=True) in lobby.py.
    def sign(self, plain: bytes) -> bytes:
        with self._lock:
            self.ctr += 1
            if self.ctr > 0xFFFFFFFF:
                self.salt = os.urandom(4)
                self.ctr = 1
            nonce = self.salt + struct.pack("!I", self.ctr)
        tag = hmac.new(self.mac_key, SIGN_DOMAIN + nonce + plain,
                       hashlib.sha256).digest()[:TAG_LEN]
        return nonce + plain + tag

    def unsign(self, data: bytes):
        """-> plaintext, or None (bad tag / replay / malformed)."""
        if len(data) < NONCE_LEN + TAG_LEN:
            self.rejected += 1
            return None
        nonce, body, tag = data[:NONCE_LEN], data[NONCE_LEN:-TAG_LEN], data[-TAG_LEN:]
        want = hmac.new(self.mac_key, SIGN_DOMAIN + nonce + body,
                        hashlib.sha256).digest()[:TAG_LEN]
        if not hmac.compare_digest(want, tag):
            self.rejected += 1
            return None
        if not self._accept_nonce(nonce):
            return None
        return body

    # Replay window, shared by open() and unsign() so a frame of either kind
    # can only be accepted once per (sender salt, counter).
    def _accept_nonce(self, nonce: bytes) -> bool:
        salt, ctr = nonce[:4], struct.unpack("!I", nonce[4:])[0]
        with self._lock:
            w = self._windows.get(salt)
            if w is None:
                self._windows[salt] = [ctr, 1]
                return True
            high, bits = w
            if ctr > high:
                shift = ctr - high
                w[0], w[1] = ctr, ((bits << shift) | 1) & ((1 << WINDOW) - 1)
                return True
            back = high - ctr
            if back >= WINDOW or (bits >> back) & 1:
                self.rejected += 1
                return False                     # replayed or too old
            w[1] = bits | (1 << back)
            return True

    def open(self, data: bytes):
        """-> plaintext, or None (bad tag / replay / malformed)."""
        if len(data) < NONCE_LEN + TAG_LEN:
            self.rejected += 1
            return None
        nonce, ct, tag = data[:NONCE_LEN], data[NONCE_LEN:-TAG_LEN], data[-TAG_LEN:]
        want = hmac.new(self.mac_key, nonce + ct, hashlib.sha256).digest()[:TAG_LEN]
        if not hmac.compare_digest(want, tag):
            self.rejected += 1
            return None
        if not self._accept_nonce(nonce):
            return None
        return _xor(ct, _keystream(self.enc_key, nonce, len(ct)))


def selftest():
    k = derive_key(os.urandom(SECRET_LEN), "pw")
    a, b = Sealer(k), Sealer(k)
    ok = True
    for size in (0, 1, 31, 32, 33, 1200, 4000):
        m = os.urandom(size)
        s = a.seal(m)
        if b.open(s) != m:
            ok = False; print(f"[seal] FAIL roundtrip size={size}")
    # tamper, replay, wrong key
    s = a.seal(b"hello")
    if b.open(s) != b"hello": ok = False
    if b.open(s) is not None: ok = False; print("[seal] FAIL replay accepted")
    t = bytearray(s); t[NONCE_LEN] ^= 1
    if b.open(bytes(t)) is not None: ok = False; print("[seal] FAIL tamper accepted")
    c = Sealer(derive_key(os.urandom(SECRET_LEN), "pw"))
    if c.open(a.seal(b"x")) is not None: ok = False; print("[seal] FAIL wrong key accepted")
    # reorder within the window is fine
    frames = [a.seal(bytes([i])) for i in range(10)]
    for f in reversed(frames):
        if b.open(f) is None: ok = False; print("[seal] FAIL reorder rejected")
    # sign/unsign: same guarantees minus confidentiality
    s2 = Sealer(k); v2 = Sealer(k)
    m = os.urandom(1200)
    sg = s2.sign(m)
    if m not in sg: ok = False; print("[seal] FAIL sign is supposed to be plaintext")
    if v2.unsign(sg) != m: ok = False; print("[seal] FAIL sign roundtrip")
    if v2.unsign(sg) is not None: ok = False; print("[seal] FAIL sign replay accepted")
    t2 = bytearray(s2.sign(m)); t2[NONCE_LEN] ^= 1
    if v2.unsign(bytes(t2)) is not None: ok = False; print("[seal] FAIL sign tamper accepted")
    if Sealer(derive_key(os.urandom(SECRET_LEN), "")).unsign(s2.sign(m)) is not None:
        ok = False; print("[seal] FAIL sign wrong key accepted")
    # a signed frame must NOT open as sealed, and vice versa
    if v2.open(s2.sign(m)) is not None: ok = False; print("[seal] FAIL signed frame verified as sealed")
    if Sealer(k).unsign(Sealer(k).seal(m)) is not None:
        ok = False; print("[seal] FAIL sealed frame verified as signed")
    # the 2^32 counter wrap must roll the salt: same salt + repeated counter is
    # a repeated keystream, which is the one way this construction breaks
    w = Sealer(k)
    w.ctr = 0xFFFFFFFF
    salt0 = w.salt
    wrapped = w.seal(b"wrap")
    if w.salt == salt0 or w.ctr != 1:
        ok = False; print("[seal] FAIL counter wrap did not re-salt")
    if Sealer(k).open(wrapped) != b"wrap":
        ok = False; print("[seal] FAIL frame after wrap does not open")
    import time
    t0 = time.time(); n = 0
    while time.time() - t0 < 0.5:
        b.open(a.seal(b"x" * 1200)); n += 1
    print(f"[seal] {'PASS' if ok else 'FAIL'}: roundtrip/tamper/replay/reorder; "
          f"~{n * 1200 * 2 / 0.5 / 1e6:.0f} MB/s seal+open")
    return ok


if __name__ == "__main__":
    import sys
    sys.exit(0 if selftest() else 1)
