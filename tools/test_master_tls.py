"""Network test: the lobby's TLS context verifies the master server, with the
system store and with the embedded ISRG roots ALONE (a Windows that lacks ISRG
Root X2 walked the chain to an expired cross-sign and the host could not
publish or hear knocks, 2026-09-18).

    python tools/test_master_tls.py
"""
import os, ssl, sys, urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "netpunch"))
import roots                                                  # noqa: E402
import lobby                                                  # noqa: E402

URL = lobby.DEFAULT_MASTER.rstrip("/") + "/health"
ok = True
for name, ctx in (("system store + ISRG roots", roots.ssl_context()), ("ISRG roots only", roots.roots_only_context())):
    try:
        with urllib.request.urlopen(URL, timeout=10, context=ctx) as r:
            body = r.read().decode()
        good = body.strip() == "ok"
    except Exception as e:                                    # noqa: BLE001
        good, body = False, repr(e)
    print(("ok   " if good else "FAIL ") + name + ("" if good else "  " + body))
    ok = ok and good
# and the roots-only context still REFUSES a host it should not trust
try:
    with urllib.request.urlopen("https://api.github.com/", timeout=10, context=roots.roots_only_context()) as r:
        r.read()
    refused = False
except ssl.SSLCertVerificationError:
    refused = True
except Exception as e:                                        # noqa: BLE001
    refused = isinstance(e, urllib.error.URLError) and isinstance(e.reason, ssl.SSLCertVerificationError)
print(("ok   " if refused else "FAIL ") + "the roots-only context refuses a chain outside ISRG")
ok = ok and refused
print("ALL OK" if ok else "FAILED")
sys.exit(0 if ok else 1)
