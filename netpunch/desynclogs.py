"""desynclogs.py -- send this game's logs to the developers after a desync.

The in-game desync popup (mod: res/scripts/mp/desyncreport.lua) asks the lobby
for it with one command line in lobby_in.jsonl:

    {"cmd":"upload_logs","instance":"b","reason":"DESYNC e vs a","t":16512,"desyncs":1}

The lobby hands it to start(). A background thread waits a little (so the log
lines around the desync are written), gathers the logs a desync report needs,
scrubs them, zips them and POSTs the zip to the master server's /desync
endpoint. The result comes back to the player as a local chat line from
MULTIPLAYER.

What is sent:
  data/   the mod's data folder (%LOCALAPPDATA%\\tpf2mp\\data or TPF2MP_DATADIR):
          *.log, *.txt and *.cfg -- the DLL logs, dash/status/inject files
  game/   stdout.txt (the game's log with the mod's script lines), settings.lua,
          tpf2_menu.log and the game folder's tpf2_slice.cfg / tpf2_menu_flags.txt
  about.json  what was collected, cut or skipped, and the report's metadata

What is not: the lobby's own logs (they hold every player's IP address),
tpf2_names.txt, saves, crash dumps.

Scrubbed from every file: Windows user and computer names (in paths and as
words), the Steam account id in userdata paths, IPv4 and IPv6 addresses
(127.0.0.1 and 0.0.0.0 are kept) and anything shaped like a lobby code.
Player names and chat text can remain.

Standalone, for testing:
    python desynclogs.py --dry-run report.zip
    python desynclogs.py --send http://127.0.0.1:8471/desync
"""
from __future__ import annotations

import glob
import io
import json
import os
import platform
import re
import sys
import threading
import time
import urllib.request
import zipfile

DEFAULT_URL = "https://srv1306562.hstgr.cloud/tpf2mp/desync"
MAX_FILE = 8 << 20          # bytes kept from the end of each file
MAX_TOTAL = 96 << 20        # uncompressed bytes in one report
MAX_ZIP = 15 << 20          # the server refuses more than 16 MB
DELAY_DEFAULT = 20.0        # seconds to wait before gathering
SKIP_DATA = {"tpf2_names.txt"}

_busy = threading.Lock()


# --------------------------------------------------------------------------- #
# where the logs are
# --------------------------------------------------------------------------- #
def data_dir():
    d = os.environ.get("TPF2MP_DATADIR")
    if d:
        return d
    la = os.environ.get("LOCALAPPDATA")
    return os.path.join(la, "tpf2mp", "data") if la else None


def steam_dir():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            p = winreg.QueryValueEx(k, "SteamPath")[0]
            if p and os.path.isdir(p):
                return os.path.normpath(p)
    except (ImportError, OSError):
        pass
    for p in (r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"):
        if os.path.isdir(p):
            return p
    return None


def game_dir():
    here = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
    for cand in (os.path.dirname(here), here):
        if os.path.isfile(os.path.join(cand, "TransportFever2.exe")):
            return cand
    s = steam_dir()
    if s:
        g = os.path.join(s, "steamapps", "common", "Transport Fever 2")
        if os.path.isfile(os.path.join(g, "TransportFever2.exe")):
            return g
    return None


def _newest(paths):
    best, when = None, -1.0
    for p in paths:
        try:
            m = os.path.getmtime(p)
        except OSError:
            continue
        if m > when:
            best, when = p, m
    return best


def gather():
    """[(name in the zip, path on disk)]"""
    out = []
    d = data_dir()
    if d and os.path.isdir(d):
        seen = set()
        for pat in ("*.log", "*.txt", "*.cfg"):
            for p in sorted(glob.glob(os.path.join(d, pat))):
                n = os.path.basename(p)
                if n.lower() in SKIP_DATA or n.lower() in seen:
                    continue
                seen.add(n.lower())
                out.append(("data/" + n, p))
    s = steam_dir()
    if s:
        local = os.path.join(s, "userdata", "*", "1066780", "local")
        so = _newest(glob.glob(os.path.join(local, "crash_dump", "stdout.txt")))
        if so:
            out.append(("game/stdout.txt", so))
        st = _newest(glob.glob(os.path.join(local, "settings.lua")))
        if st:
            out.append(("game/settings.lua", st))
    g = game_dir()
    if g:
        for n in ("tpf2_menu.log", "tpf2_slice.cfg", "tpf2_menu_flags.txt"):
            p = os.path.join(g, n)
            if os.path.isfile(p):
                out.append(("game/" + n, p))
    return out


# --------------------------------------------------------------------------- #
# scrubbing
# --------------------------------------------------------------------------- #
_IPV4 = re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
_HEX = r"[0-9A-Fa-f]{1,4}"
_IPV6 = re.compile(
    r"(?<![\w:])(?:(?:%s:){7}%s|(?:%s(?::%s){0,6})?::(?:%s(?::%s){0,6})?)(?![\w:])"
    % (_HEX, _HEX, _HEX, _HEX, _HEX, _HEX))
_KEEP_IP = {"127.0.0.1", "0.0.0.0"}
_PROFILE = re.compile(r"(?i)([\\/](?:users|sandbox)[\\/])[^\\/\r\n\"'<>|:*?]+")
_STEAMID = re.compile(r"(?i)(userdata[\\/])\d+")
_CODE = re.compile(r"(?<![A-Za-z0-9])[A-Z2-7]{24,}={0,6}(?![A-Za-z0-9=])")


def _private_words():
    words = set()
    for v in (os.environ.get("USERNAME"), os.environ.get("COMPUTERNAME"),
              os.path.basename(os.environ.get("USERPROFILE") or "")):
        if v and len(v) >= 3:
            words.add(v)
    return sorted(words, key=len, reverse=True)


def scrub(data, words=None):
    words = _private_words() if words is None else words
    text = data.decode("utf-8", "surrogateescape")
    text = _PROFILE.sub(r"\1<user>", text)
    text = _STEAMID.sub(r"\1<steamid>", text)
    for w in words:
        text = re.sub(r"(?i)(?<![A-Za-z0-9])%s(?![A-Za-z0-9])" % re.escape(w), "<user>", text)
    text = _IPV4.sub(lambda m: m.group(0) if m.group(0) in _KEEP_IP else "<ip>", text)
    text = _IPV6.sub("<ip>", text)
    text = _CODE.sub("<code>", text)
    return text.encode("utf-8", "surrogateescape")


# --------------------------------------------------------------------------- #
# the report
# --------------------------------------------------------------------------- #
def _read_tail(path, limit):
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        if size <= limit:
            f.seek(0)
            return f.read(), size, False
        f.seek(size - limit)
        data = f.read()
    nl = data.find(b"\n")
    if 0 <= nl < len(data) - 1:
        data = data[nl + 1:]
    return data, size, True


def build(meta, files=None, per_file=MAX_FILE):
    """Zip the gathered files. Returns (zip bytes, about dict)."""
    files = gather() if files is None else files
    words = _private_words()
    about = {"meta": meta, "files": [], "skipped": []}
    buf = io.BytesIO()
    total = 0
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, path in files:
            if total >= MAX_TOTAL:
                about["skipped"].append({"name": name, "why": "report full"})
                continue
            try:
                data, size, cut = _read_tail(path, min(per_file, MAX_TOTAL - total))
            except OSError as e:
                about["skipped"].append({"name": name, "why": type(e).__name__})
                continue
            data = scrub(data, words)
            total += len(data)
            z.writestr(name, data)
            about["files"].append({"name": name, "size": size, "sent": len(data), "cut": cut})
        z.writestr("about.json", json.dumps(about, indent=1))
    return buf.getvalue(), about


def build_capped(meta, files=None):
    per_file = MAX_FILE
    while True:
        blob, about = build(meta, files, per_file)
        if len(blob) <= MAX_ZIP or per_file <= (256 << 10):
            return blob, about
        per_file //= 2


def post(url, blob, meta, version, timeout=60):
    req = urllib.request.Request(url, data=blob, method="POST", headers={
        "Content-Type": "application/zip",
        "X-Tpf2mp-Meta": json.dumps(meta)[:1800],
        "User-Agent": f"tpf2mp-lobby/{version}",
    })
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read(4096) or b"{}")


def _meta(cmd, version):
    def num(v):
        try:
            return int(float(v))
        except (TypeError, ValueError):
            return 0
    return {
        "v": 1,
        "version": str(version)[:20],
        "instance": str(cmd.get("instance") or "")[:4],
        "reason": str(cmd.get("reason") or "")[:160],
        "t": num(cmd.get("t")),
        "desyncs": num(cmd.get("desyncs")),
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "os": platform.platform()[:80],
    }


def start(io_, log, cmd, version, url=None):
    """Gather and send in the background. False when a report is already running."""
    if not _busy.acquire(blocking=False):
        log("[report] desync logs: a report is already being sent -- this request ignored")
        return False
    threading.Thread(target=_run, args=(io_, log, dict(cmd), version, url),
                     name="desync-report", daemon=True).start()
    return True


def _say(io_, text):
    io_.emit({"type": "chat", "from": "MULTIPLAYER", "text": text})


def _run(io_, log, cmd, version, url):
    try:
        try:
            delay = float(cmd.get("delay", DELAY_DEFAULT))
        except (TypeError, ValueError):
            delay = DELAY_DEFAULT
        time.sleep(max(0.0, min(120.0, delay)))
        meta = _meta(cmd, version)
        blob, about = build_capped(meta)
        target = os.environ.get("TPF2MP_REPORT_URL") or url or DEFAULT_URL
        log(f"[report] desync logs: {len(about['files'])} file(s), {len(blob)} B zipped -> {target}")
        res = post(target, blob, meta, version)
        rid = str(res.get("id") or "?")
        log(f"[report] desync logs sent: report {rid}")
        _say(io_, f"Desync logs sent to the developers (report {rid}). Thank you!")
        io_.emit({"type": "report", "ok": True, "id": rid})
    except Exception as e:                      # never take the lobby down with it
        detail = getattr(e, "reason", None) or getattr(e, "code", None) or e
        log(f"[report] desync logs NOT sent: {type(e).__name__}: {detail}")
        _say(io_, f"Could not send the desync logs ({detail}).")
        io_.emit({"type": "report", "ok": False, "detail": str(detail)[:200]})
    finally:
        _busy.release()


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="gather, scrub and send the desync logs")
    ap.add_argument("--dry-run", metavar="ZIP", help="write the report here instead of sending it")
    ap.add_argument("--send", metavar="URL", help="send the report to this /desync URL")
    a = ap.parse_args(argv)
    meta = _meta({"reason": "manual", "instance": "?"}, "manual")
    blob, about = build_capped(meta)
    for f in about["files"]:
        print(f"  {f['name']}: {f['sent']} of {f['size']} B{' (cut)' if f['cut'] else ''}")
    for s in about["skipped"]:
        print(f"  skipped {s['name']}: {s['why']}")
    print(f"{len(blob)} B zipped")
    if a.dry_run:
        with open(a.dry_run, "wb") as f:
            f.write(blob)
    if a.send:
        print(post(a.send, blob, meta, "manual"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
