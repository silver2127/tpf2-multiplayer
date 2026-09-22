#!/usr/bin/env python3
"""Install PyInstaller with its bootloader compiled here, not the stock one.

    python tools/pyinstaller_from_source.py [--python <exe>] [--force]

A one-file PyInstaller exe is the stock bootloader stub with an archive
appended, and that stub is the same bytes in every PyInstaller download in the
world -- which is why antivirus engines keep signatures for it and why
netpunch.exe came back from VirusTotal as "Trojan.Win64.Krypt" from two of
them (2026-09-20). A stub compiled on this machine is a different binary: the
signature matches go away (the behaviour heuristics do not; those want code
signing).

PyInstaller's source package carries PREBUILT bootloaders, so plain
`pip install --no-binary pyinstaller pyinstaller` keeps the stock stub. This
downloads the pinned version's source, compiles the bootloaders with the
Visual Studio toolchain (bootloader/waf, found through tools\\msvc_env.bat),
checks the result differs from the stock stub, and installs that tree into
the given Python. It writes BUILT_FROM_SOURCE.txt beside the bootloaders and
does nothing when that marker is already there for the pinned version.

installer/build_msi.ps1 runs it before the freeze; the GitHub Actions workflow
runs it in place of `pip install pyinstaller`.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

VERSION = "6.22.2"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def bootloader_dir(py):
    out = subprocess.run([py, "-c", "import PyInstaller, os; print(os.path.dirname(PyInstaller.__file__))"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return None
    return os.path.join(out.stdout.strip(), "bootloader", "Windows-64bit-intel")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--python", default=sys.executable, help="the interpreter to install into (default: this one)")
    ap.add_argument("--force", action="store_true", help="rebuild even when the marker says it is done")
    a = ap.parse_args()
    py = a.python
    bl = bootloader_dir(py)
    marker = bl and os.path.join(bl, "BUILT_FROM_SOURCE.txt")
    if marker and os.path.isfile(marker) and not a.force:
        text = open(marker, encoding="utf-8").read()
        if text.startswith(VERSION + " "):
            print(f"pyinstaller {VERSION} with a bootloader compiled from source is already installed ({text.strip()})")
            return 0
    if sys.platform != "win32":
        print("this script builds the Windows bootloader; on other platforms install PyInstaller as usual", file=sys.stderr)
        return 2
    work = tempfile.mkdtemp(prefix="pyinstaller-src-")
    print(f"[bootloader] downloading pyinstaller {VERSION} source into {work}")
    subprocess.check_call([py, "-m", "pip", "download", "--quiet", "--no-binary", "pyinstaller", "--no-deps",
                           f"pyinstaller=={VERSION}", "-d", work])
    tgz = next(f for f in os.listdir(work) if f.endswith(".tar.gz"))
    with tarfile.open(os.path.join(work, tgz)) as t:
        # the top-level tests/ and news/ trees are not needed and hold names Windows
        # dislikes; bootloader/tests stays -- the bootloader's wscript recurses into it
        skip = (f"pyinstaller-{VERSION}/tests/", f"pyinstaller-{VERSION}/news/")
        members = [m for m in t.getmembers() if not m.name.startswith(skip)]
        t.extractall(work, members=members, filter="data")
    src = os.path.join(work, f"pyinstaller-{VERSION}")
    stock = os.path.join(src, "PyInstaller", "bootloader", "Windows-64bit-intel", "run.exe")
    stock_hash = sha(stock)
    env_bat = os.path.join(ROOT, "tools", "msvc_env.bat")
    print("[bootloader] compiling with the Visual Studio toolchain (bootloader/waf distclean all)")
    cmd = f'call "{env_bat}" >nul 2>nul && cd /d "{os.path.join(src, "bootloader")}" && "{py}" ./waf distclean all'
    # one string, not an argument list: as an argument Python escapes the inner quotes
    # with backslashes, which cmd.exe reads as part of the path
    r = subprocess.run(f'cmd /c "{cmd}"', capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        print(r.stdout[-3000:], r.stderr[-2000:], file=sys.stderr)
        print("[bootloader] waf failed", file=sys.stderr)
        return 1
    built_hash = sha(stock)
    if built_hash == stock_hash:
        print("[bootloader] the compiled stub is byte-identical to the stock one -- nothing was rebuilt", file=sys.stderr)
        return 1
    print(f"[bootloader] compiled run.exe sha256 {built_hash[:16]} (stock {stock_hash[:16]})")
    # a local tree is always rebuilt and reinstalled by pip, and its dependencies come along
    subprocess.check_call([py, "-m", "pip", "install", "--quiet", src])
    bl = bootloader_dir(py)
    installed = sha(os.path.join(bl, "run.exe"))
    if installed != built_hash:
        print("[bootloader] the installed stub is not the compiled one", file=sys.stderr)
        return 1
    with open(os.path.join(bl, "BUILT_FROM_SOURCE.txt"), "w", encoding="utf-8") as f:
        f.write(f"{VERSION} run.exe sha256 {built_hash}\n")
    print(f"[bootloader] installed pyinstaller {VERSION} with the compiled bootloader into {py}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
