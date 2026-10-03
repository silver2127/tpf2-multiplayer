#!/usr/bin/env python3
"""
steamkey.py -- hand a joiner the session secret when the join code is only a Steam ID.

The classic join code CARRIES the 12-byte session secret (connect.py): whoever has
the code derives the frame key, nobody else can read or inject a frame. A Steam ID
is public -- it is on the host's profile page -- so a code that is just the Steam ID
carries no secret, and the secret has to reach the joiner over the connection.

It does, by Diffie-Hellman over the Steam tunnel, before any sealed frame:

  joiner -> host   NP1 'X'  "TPK1" || A           A = g^a mod p   (256 bytes)
  host   -> joiner NP1 'X'  "TPK2" || B || box    B = g^b mod p;  box = secret XOR pad || tag

  k    = SHA256("tpf2mp-steamkey-v1" || g^ab mod p || A || B)
  pad  = SHA256(k || "enc")[:12]         tag = HMAC-SHA256(k || "mac", B || secret XOR pad)[:16]

RFC 3526 group 14 (2048-bit MODP), g = 2, 256-bit exponents: stdlib only (pow), so
the frozen netpunch.exe needs no new dependency; one exchange costs a few ms.

What this protects and what it does not. The Steam tunnel only admits traffic from
a Steam user who opened a session with the host, and Steam authenticates that user,
so the exchange is not open to a man in the middle short of Valve itself. It does
NOT make the lobby private: anyone who knows the host's Steam ID can join, exactly
as anyone who saw a classic code could. A lobby password still layers on top: the
frame key is derive_key(secret, password), so a joiner with the secret but the
wrong password is refused at its first sealed frame, as before.

The host answers only from a Steam tunnel endpoint (steamtunnel.is_tunnel_addr), at
most every ANSWER_EVERY seconds per endpoint, and repeats the same answer for the
same offer so a lost reply costs nothing but a resend.
"""
from __future__ import annotations

import hashlib
import hmac
import os

# RFC 3526, 2048-bit MODP group 14
P = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AACAA68FFFFFFFFFFFFFFFF", 16)
G = 2
PUB_LEN = 256
SECRET_LEN = 12
TAG_LEN = 16
OFFER_MAGIC = b"TPK1"
ANSWER_MAGIC = b"TPK2"
ANSWER_EVERY = 0.25          # host: seconds between answers to one endpoint
DOMAIN = b"tpf2mp-steamkey-v1"


def _pub_bytes(x):
    return x.to_bytes(PUB_LEN, "big")


def _valid_pub(v):
    # 1 < v < p-1: rejects the trivial values that would pin the shared secret
    return 1 < v < P - 1


def _derive(shared, a_pub, b_pub):
    return hashlib.sha256(DOMAIN + _pub_bytes(shared) + a_pub + b_pub).digest()


def _pad(k):
    return hashlib.sha256(k + b"enc").digest()[:SECRET_LEN]


def _tag(k, b_pub, box):
    return hmac.new(k + b"mac", b_pub + box, hashlib.sha256).digest()[:TAG_LEN]


class Offer:
    """The joiner's half: make an offer, then read the host's answer for the secret."""

    def __init__(self):
        self._a = int.from_bytes(os.urandom(32), "big") | 1
        self.pub = _pub_bytes(pow(G, self._a, P))

    def payload(self):
        return OFFER_MAGIC + self.pub

    def secret_from(self, answer):
        """The session secret from the host's answer, or None if it is not a valid
        answer to THIS offer (a stray, a replay of another joiner's, or tampered)."""
        if len(answer) != len(ANSWER_MAGIC) + PUB_LEN + SECRET_LEN + TAG_LEN or not answer.startswith(ANSWER_MAGIC):
            return None
        o = len(ANSWER_MAGIC)
        b_pub = answer[o:o + PUB_LEN]; o += PUB_LEN
        box = answer[o:o + SECRET_LEN]; o += SECRET_LEN
        tag = answer[o:o + TAG_LEN]
        b = int.from_bytes(b_pub, "big")
        if not _valid_pub(b):
            return None
        k = _derive(pow(b, self._a, P), self.pub, b_pub)
        if not hmac.compare_digest(tag, _tag(k, b_pub, box)):
            return None
        return bytes(x ^ y for x, y in zip(box, _pad(k)))


def answer(offer_payload, secret):
    """The host's half: the answer to one offer, or None if the offer is malformed."""
    if len(secret) != SECRET_LEN:
        raise ValueError("secret must be 12 bytes")
    if len(offer_payload) != len(OFFER_MAGIC) + PUB_LEN or not offer_payload.startswith(OFFER_MAGIC):
        return None
    a_pub = offer_payload[len(OFFER_MAGIC):]
    a = int.from_bytes(a_pub, "big")
    if not _valid_pub(a):
        return None
    bexp = int.from_bytes(os.urandom(32), "big") | 1
    b_pub = _pub_bytes(pow(G, bexp, P))
    k = _derive(pow(a, bexp, P), a_pub, b_pub)
    box = bytes(x ^ y for x, y in zip(secret, _pad(k)))
    return ANSWER_MAGIC + b_pub + box + _tag(k, b_pub, box)


def selftest():
    import time
    secret = os.urandom(SECRET_LEN)
    t0 = time.perf_counter()
    o = Offer()
    ans = answer(o.payload(), secret)
    got = o.secret_from(ans)
    ms = (time.perf_counter() - t0) * 1000
    assert got == secret, "round trip"
    assert Offer().secret_from(ans) is None, "an answer is only good for its own offer"
    bad = bytearray(ans); bad[-1] ^= 1
    assert o.secret_from(bytes(bad)) is None, "a tampered answer is refused"
    bad = bytearray(ans); bad[len(ANSWER_MAGIC) + PUB_LEN] ^= 1
    assert o.secret_from(bytes(bad)) is None, "a tampered box is refused"
    assert answer(OFFER_MAGIC + _pub_bytes(1), secret) is None, "a trivial public value is refused"
    assert answer(OFFER_MAGIC + _pub_bytes(P - 1), secret) is None, "p-1 is refused"
    assert answer(b"TPK1short", secret) is None, "a short offer is refused"
    assert o.secret_from(b"junk") is None
    print(f"steamkey selftest OK ({ms:.1f} ms for one exchange)")
    return True


if __name__ == "__main__":
    selftest()
