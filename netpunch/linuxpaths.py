"""
linuxpaths.py -- where things are when the lobby runs on Linux.

The Windows code finds Steam in the registry and the mod's data folder under
%LOCALAPPDATA%; neither exists on Linux. modshare.py, desynclogs.py,
observe.py and lobby.py ask here instead, and only when sys.platform is not
"win32", so every Windows path is unchanged. Stdlib only.

Data folder -- the chain of native/linux/src/datadir_linux.h, which the native
libraries and the Lua mod (lockstep.lua K.BASE, CM.netDir) resolve the same
way, so all of them agree on ONE folder:
    TPF2MP_DATADIR (absolute)
    else $XDG_DATA_HOME/tpf2mp/data   (XDG_DATA_HOME absolute)
    else $HOME/.local/share/tpf2mp/data
The lobby's own folder (its IPC files; CM.netDir looks for lobby_out.jsonl
there) is the netpunch/ sibling of data/. Under the Steam snap HOME is
~/snap/steam/common for the game and everything it starts.

Steam roots -- every folder that has a steamapps/ in it, in this order:
TPF2MP_STEAM_ROOT, STEAM_COMPAT_CLIENT_INSTALL_PATH (when Steam set it),
$XDG_DATA_HOME/Steam, then for $HOME, the account's home from the password
database and $SNAP_REAL_HOME: .steam/root, .steam/steam, .local/share/Steam,
snap/steam/common/.local/share/Steam (the Steam snap seen from outside it) and
the Flatpak's .var/app/com.valvesoftware.Steam/{.local/share,data}/Steam.
Symlinks are resolved, duplicates dropped.

The game -- TPF2MP_GAME_DIR (whoever starts the lobby knows which install is
running), then a folder with the TransportFever2 binary beside the lobby, then
every library of every root (steamapps/libraryfolders.vdf and
config/libraryfolders.vdf) at steamapps/common/<installdir from
appmanifest_1066780.acf>.

Measured on the Snap Steam install this was written on (2026-09-12): root
~/snap/steam/common/.local/share/Steam (~/.steam/root inside the snap links to
it), one library listed in both libraryfolders.vdf files, installdir
"Transport Fever 2", saves and the game's log under
userdata/<account>/1066780/local/{save,crash_dump/stdout.txt} exactly as on
Windows, Workshop items under steamapps/workshop/content/1066780 of that root.

    python linuxpaths.py      # print what this machine resolves to
"""
from __future__ import annotations

import os
import re
import socket
import sys

TF2_APPID = "1066780"
GAME_BINARY = "TransportFever2"
DEFAULT_INSTALLDIR = "Transport Fever 2"

# CA bundles, most common first. /run/host/etc is the host's /etc as the Steam
# Runtime container (pressure-vessel) shows it.
CA_BUNDLES = (
    "/etc/ssl/certs/ca-certificates.crt",          # Debian, Ubuntu, Arch, the Steam Runtime
    "/etc/pki/tls/certs/ca-bundle.crt",            # Fedora, RHEL
    "/etc/ssl/ca-bundle.pem",                      # openSUSE
    "/etc/ssl/cert.pem",                           # Alpine, Void
    "/run/host/etc/ssl/certs/ca-certificates.crt",
    "/run/host/etc/pki/tls/certs/ca-bundle.crt",
)

_VDF_PATH = re.compile(r'"path"\s+"((?:[^"\\]|\\.)*)"')
_VDF_OLD = re.compile(r'^\s*"\d+"\s+"(/(?:[^"\\]|\\.)*)"', re.M)   # before 2021: "1" "/path"
_ACF_INSTALLDIR = re.compile(r'"installdir"\s+"((?:[^"\\]|\\.)*)"')
_VDF_ESCAPE = re.compile(r"\\(.)")
_GENERIC_HOSTS = {"localhost", "localhost.localdomain"}


def _unescape(s):
    return _VDF_ESCAPE.sub(r"\1", s)


def _abs_env(name):
    """An environment variable holding an absolute path, else None (the same
    rule as datadir_linux.h: a relative XDG_DATA_HOME or HOME is ignored)."""
    v = os.environ.get(name)
    return v if v and v.startswith("/") else None


def _passwd():
    try:
        import pwd
        return pwd.getpwuid(os.getuid())
    except (ImportError, KeyError, OSError):
        return None


def _homes():
    """$HOME, the account's home and $SNAP_REAL_HOME -- inside the Steam snap
    HOME is ~/snap/steam/common, so the real home is a separate candidate."""
    pw = _passwd()
    out = []
    for h in (_abs_env("HOME"), pw.pw_dir if pw else None, _abs_env("SNAP_REAL_HOME")):
        if h and h.startswith("/") and h not in out:
            out.append(h)
    return out


def _read_text(path, limit=1 << 20):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read(limit)
    except OSError:
        return None


def _existing(paths, marker):
    """The paths that hold ``marker``, resolved, first occurrence only."""
    out = []
    for p in paths:
        if not p:
            continue
        real = os.path.realpath(p)
        if real not in out and os.path.isdir(os.path.join(real, marker)):
            out.append(real)
    return out


# --------------------------------------------------------------------------- #
# the mod's folders
# --------------------------------------------------------------------------- #
def data_root():
    """$XDG_DATA_HOME/tpf2mp, else $HOME/.local/share/tpf2mp (Tpf2mpRootDir)."""
    xdg = _abs_env("XDG_DATA_HOME")
    if xdg:
        return os.path.join(xdg, "tpf2mp")
    home = _abs_env("HOME")
    return os.path.join(home, ".local", "share", "tpf2mp") if home else None


def data_dir():
    """The runtime data folder (Tpf2mpDataDirA), without creating it."""
    env = _abs_env("TPF2MP_DATADIR")
    if env:
        return env
    root = data_root()
    return os.path.join(root, "data") if root else None


def net_dir():
    """The lobby's folder: its IPC files, where the Lua mod looks for them."""
    root = data_root()
    return os.path.join(root, "netpunch") if root else None


# --------------------------------------------------------------------------- #
# Steam and the game
# --------------------------------------------------------------------------- #
def steam_roots():
    """Every Steam root that exists, in the order of the module docstring."""
    cands = [_abs_env("TPF2MP_STEAM_ROOT"), _abs_env("STEAM_COMPAT_CLIENT_INSTALL_PATH")]
    xdg = _abs_env("XDG_DATA_HOME")
    if xdg:
        cands.append(os.path.join(xdg, "Steam"))
    for home in _homes():
        cands += [
            os.path.join(home, ".steam", "root"),
            os.path.join(home, ".steam", "steam"),
            os.path.join(home, ".local", "share", "Steam"),
            os.path.join(home, "snap", "steam", "common", ".local", "share", "Steam"),
            os.path.join(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
            os.path.join(home, ".var", "app", "com.valvesoftware.Steam", "data", "Steam"),
        ]
    return _existing(cands, "steamapps")


def library_dirs(root):
    """The root itself, then every library its libraryfolders.vdf files list."""
    found = [root]
    for vdf in (os.path.join(root, "steamapps", "libraryfolders.vdf"),
                os.path.join(root, "config", "libraryfolders.vdf")):
        text = _read_text(vdf)
        if text is None:
            continue
        found += [_unescape(m.group(1)) for m in _VDF_PATH.finditer(text)]
        found += [_unescape(m.group(1)) for m in _VDF_OLD.finditer(text)]
    return _existing([p for p in found if p.startswith("/")], "steamapps")


def installdir(library):
    """The game's folder name under steamapps/common, from its app manifest."""
    text = _read_text(os.path.join(library, "steamapps", f"appmanifest_{TF2_APPID}.acf"))
    m = _ACF_INSTALLDIR.search(text or "")
    name = _unescape(m.group(1)) if m else ""
    if not name or "/" in name or name in (".", ".."):
        return DEFAULT_INSTALLDIR
    return name


def is_game_dir(path):
    return bool(path) and os.path.isfile(os.path.join(path, GAME_BINARY))


def game_dir(beside=None):
    """The Transport Fever 2 folder, or None. ``beside`` is the lobby's own
    folder: the game counts when the lobby sits in it or one level below."""
    env = _abs_env("TPF2MP_GAME_DIR")
    if is_game_dir(env):
        return os.path.normpath(env)
    if beside:
        for cand in (os.path.dirname(beside), beside):
            if is_game_dir(cand):
                return cand
    for root in steam_roots():
        for lib in library_dirs(root):
            g = os.path.join(lib, "steamapps", "common", installdir(lib))
            if is_game_dir(g):
                return g
    return None


def game_library(game):
    """<library> for <library>/steamapps/common/<installdir>, else None."""
    if not game:
        return None
    common = os.path.dirname(os.path.normpath(game))
    steamapps = os.path.dirname(common)
    if os.path.basename(common) == "common" and os.path.basename(steamapps) == "steamapps":
        return os.path.dirname(steamapps)
    return None


def workshop_dir(game=None):
    """steamapps/workshop/content/1066780 of the library the game is in (Steam
    keeps an app's Workshop items with the app; this machine has one library,
    so only that case was seen), else of the first Steam root."""
    lib = game_library(game)
    if lib is None:
        roots = steam_roots()
        lib = roots[0] if roots else None
    return os.path.join(lib, "steamapps", "workshop", "content", TF2_APPID) if lib else None


def userdata_mods_dir():
    """<root>/userdata/<account>/1066780/local/mods of the account that has a
    save folder, the newest account folder winning across every root -- the
    rule of modshare.userdata_mods_dir on Windows."""
    best, best_t = None, -1.0
    for root in steam_roots():
        ud = os.path.join(root, "userdata")
        try:
            accounts = os.listdir(ud)
        except OSError:
            continue
        for acc in accounts:
            local = os.path.join(ud, acc, TF2_APPID, "local")
            if not os.path.isdir(os.path.join(local, "save")):
                continue
            try:
                t = os.path.getmtime(os.path.join(ud, acc))
            except OSError:
                continue
            if t > best_t:
                best, best_t = os.path.join(local, "mods"), t
    return best


# --------------------------------------------------------------------------- #
# process environment
# --------------------------------------------------------------------------- #
def private_words():
    """Login name, home folder name and host name: what desync reports scrub,
    as USERNAME / COMPUTERNAME / USERPROFILE on Windows. Not the basename of
    $HOME, which is 'common' inside the Steam snap."""
    vals = [os.environ.get("USER"), os.environ.get("LOGNAME")]
    pw = _passwd()
    if pw:
        vals += [pw.pw_name, os.path.basename((pw.pw_dir or "").rstrip("/"))]
    real = _abs_env("SNAP_REAL_HOME")
    if real:
        vals.append(os.path.basename(real.rstrip("/")))
    try:
        host = socket.gethostname()
    except OSError:
        host = ""
    if host and host not in _GENERIC_HOSTS:
        vals += [host, host.split(".")[0]]
    out = []
    for v in vals:
        if v and v not in out:
            out.append(v)
    return out


def ssl_cert_fallback():
    """Point OpenSSL at a CA bundle when its default locations are empty.

    A frozen build carries its own OpenSSL, which looks only where it was
    compiled to look. The python-build-standalone CPython that
    tools/linux/build_netpunch.sh freezes looks in /etc/ssl/cert.pem and
    /etc/ssl/certs (a Debian or Ubuntu Python: /usr/lib/ssl/...). A system
    whose certificates are elsewhere would then fail every HTTPS request --
    the public list, publishing, desync reports. INFERRED from their layouts,
    not run: Fedora and Arch provide /etc/ssl/cert.pem, so this is for rarer
    layouts. Measured 2026-09-12 with that build: on the Ubuntu host and inside
    the Steam Runtime soldier container the game runs in (/etc/ssl/certs held
    136 hashed names, no /etc/ssl/cert.pem) HTTPS worked with nothing changed.
    SSL_CERT_FILE is read whenever a context loads the default paths, so this
    must run before the first request. Returns the bundle it chose, or None
    when nothing had to change."""
    if os.environ.get("SSL_CERT_FILE") or os.environ.get("SSL_CERT_DIR"):
        return None
    try:
        import ssl
    except ImportError:
        return None
    paths = ssl.get_default_verify_paths()
    if paths.cafile:                     # None unless the file exists
        return None
    if paths.capath:
        # OpenSSL searches a capath only by c_rehash names (8 hex digits, dot,
        # number); a folder that holds just a bundle file does not count
        hashed = re.compile(r"^[0-9a-f]{8}\.\d+$")
        try:
            if any(hashed.match(n) for n in os.listdir(paths.capath)):
                return None
        except OSError:
            pass
    for bundle in CA_BUNDLES:
        if os.path.isfile(bundle):
            os.environ["SSL_CERT_FILE"] = bundle
            return bundle
    return None


def child_env():
    """The environment for a program this process starts. A PyInstaller build
    puts its own library folder (_internal/ of the onedir build, a onefile
    build's unpack folder) first in LD_LIBRARY_PATH and keeps the caller's
    value in LD_LIBRARY_PATH_ORIG (unset when there was none; both layouts
    measured with PyInstaller 6.22.3); a system program has to get the original
    back or it may load the bundled libraries."""
    env = dict(os.environ)
    if getattr(sys, "frozen", False):
        orig = env.pop("LD_LIBRARY_PATH_ORIG", None)
        if orig is None:
            env.pop("LD_LIBRARY_PATH", None)
        else:
            env["LD_LIBRARY_PATH"] = orig
    return env


def report():
    roots = steam_roots()
    game = game_dir()
    return {
        "data_dir": data_dir(),
        "net_dir": net_dir(),
        "steam_roots": roots,
        "libraries": {r: library_dirs(r) for r in roots},
        "game_dir": game,
        "workshop_dir": workshop_dir(game),
        "userdata_mods_dir": userdata_mods_dir(),
        "private_words": private_words(),
    }


if __name__ == "__main__":
    import json
    print(json.dumps(report(), indent=1))
