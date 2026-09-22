# -*- mode: python ; coding: utf-8 -*-
# netpunch-linux.spec -- the Linux lobby as ONE FOLDER, dist-linux/netpunch/:
# the executable netpunch plus its _internal/ folder, deployed together. The
# game's Multiplayer panel starts it (netpunch.spec builds the Windows onefile
# exe). Built and checked by tools/linux/build_netpunch.sh.
#
# Onedir, not onefile. Measured with PyInstaller 6.22.3 on 2026-09-12:
#   * a onefile build unpacks 42 MB into $TMPDIR/_MEI* on every start (the
#     server browser starts the program every 10 s), runs Python as a child of
#     its bootloader, and leaves that folder behind when its process group is
#     SIGKILLed;
#   * the onedir bootloader unpacks nothing and runs Python in its own process:
#     same pid, no child, the caller's process group. A signal reaches the lobby
#     once, and a SIGKILL leaves nothing behind.
# It still sets LD_LIBRARY_PATH to _internal/ (the caller's value kept in
# LD_LIBRARY_PATH_ORIG): linuxpaths.child_env undoes that for programs we start.
import os

HERE = SPECPATH   # netpunch/

a = Analysis(
    [os.path.join(HERE, 'lobby.py')],
    pathex=[HERE],
    binaries=[],
    datas=[],
    # Imported inside functions. The bytecode scan finds these too; listing them
    # keeps a refactor from dropping one silently: Steam transport, the
    # connectivity profile, the Linux paths, UPnP, and zstandard's C backend
    # (it reads a save's mod list).
    hiddenimports=['steamtunnel', 'observe', 'linuxpaths', 'miniupnpc',
                   'zstandard', 'zstandard.backend_c'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['tkinter'],
    noarchive=False,
    optimize=0,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,      # onedir: the libraries go to _internal/ (COLLECT)
    name='netpunch',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,                  # the output must not depend on whether upx is on PATH
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    contents_directory='_internal',   # named here: the build script and NETPUNCH.md rely on it
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='netpunch',
)
