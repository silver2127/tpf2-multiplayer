"""A peer's broken mod zip fails that one install cleanly.

An entry flagged encrypted (RuntimeError) or packed with a compression method
Python lacks (NotImplementedError) escaped install_mod_zip: the lobby's broad
handler turned it into "verify/write crashed", the whole batch failed (valid
mods too), and a *.mp_incoming folder was left behind.

    python tools/mod_zip_errors_test.py
"""
import io
import os
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "netpunch"))

import modshare   # noqa: E402

fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def mod_zip():
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr("mod.lua", "function data() return {} end")
    return bytearray(b.getvalue())


def encrypted():
    data = mod_zip()
    data[6] |= 1                                   # local header: general purpose flag bit 0
    data[data.find(b"PK\x01\x02") + 8] |= 1        # central directory: the same bit
    return bytes(data)


def unknown_method():
    data = mod_zip()
    data[8:10] = (99).to_bytes(2, "little")        # local header: compression method
    i = data.find(b"PK\x01\x02")
    data[i + 10:i + 12] = (99).to_bytes(2, "little")
    return bytes(data)


base = tempfile.mkdtemp()
modshare.install_target = lambda m, v: os.path.join(base, f"{m}_{v}")
for label, data in (("an encrypted entry", encrypted()), ("an unknown compression method", unknown_method())):
    try:
        res = modshare.install_mod_zip(data, "brokenmod", 1)
    except Exception as e:
        res = ("raised", repr(e))
    check(f"{label}: the install fails instead of raising", res == ("failed", None), str(res))
    check(f"{label}: nothing is left in the mods folder", os.listdir(base) == [], str(os.listdir(base)))
res = modshare.install_mod_zip(bytes(mod_zip()), "goodmod", 1)
check("a good zip still installs", res[0] == "installed", str(res))

print()
if fails:
    print(f"{len(fails)} FAILED")
    sys.exit(1)
print("PASS: broken mod zips fail cleanly")
