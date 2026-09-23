#!/usr/bin/env python3
"""Install the verified Windows 0.4.22 payload for an existing Steam Proton game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import time

MANIFEST = json.loads(Path(__file__).with_name("windows-0.4.22.json").read_text())
FILES = {row["path"]: row["sha256"] for row in MANIFEST["files"]}
MOD = Path("mods/mp_lockstep_1")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def plain_path(root, path):
    """Never copy through a managed file/directory symlink."""
    for part in (path, *path.parents):
        if part == root:
            return
        require(not part.is_symlink(), f"Managed path is a symlink: {part}")
        if part != path:
            require(not part.exists() or part.is_dir(), f"Not a directory: {part}")
    raise ValueError(f"Path is outside its managed root: {path}")


def validate_payload(payload):
    require(payload.is_dir(), f"Payload directory is missing: {payload}")
    paths = list(payload.rglob("*"))
    require(not any(p.is_symlink() for p in paths), "Payload must contain ordinary files/directories, no symlinks")
    actual = {p.relative_to(payload).as_posix() for p in paths if p.is_file()}
    require(actual == FILES.keys(), f"Payload file set differs from official 0.4.22: missing={sorted(FILES.keys() - actual)}, extra={sorted(actual - FILES.keys())}")
    for relative, expected in FILES.items():
        require(digest(payload / relative) == expected, f"Official payload checksum mismatch: {relative}")
    require(sum(p.endswith(".lua") for p in actual) == 24, "Expected exactly 24 Windows Lua files")


def validate_game(game):
    exe = game / "TransportFever2.exe"
    require(exe.is_file(), f"Windows game executable is missing: {exe}")
    with exe.open("rb") as source:
        header = source.read(64)
        require(header[:2] == b"MZ" and len(header) == 64, "Game is not a PE executable")
        source.seek(struct.unpack_from("<I", header, 0x3c)[0])
        pe = source.read(84)
    require(len(pe) == 84 and pe[:4] == b"PE\0\0" and struct.unpack_from("<H", pe, 4)[0] == 0x8664,
            "Game is not an x64 PE executable")
    require(struct.unpack_from("<I", pe, 8)[0] == 0x675abcc6 and struct.unpack_from("<I", pe, 80)[0] == 0x046ce000,
            "Game does not match Windows build 35924; the released DLL offsets are incompatible")


def game_running(game):
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            if "transportfever" not in (process / "comm").read_text().lower():
                continue
            if str(game / "TransportFever2.exe") in (process / "maps").read_text():
                return process.name
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return None


def prefix_shadows(prefix, files=FILES):
    for profile in (prefix / "drive_c/users").glob("*"):
        root = profile / "AppData/Local/tpf2mp"
        for name in ("tpf2_bridge_mp.dll", "tpf2_menu.dll", "tpf2_slice.dll", "tpf2_pluginhost.dll", "netpunch/netpunch.exe"):
            candidate = root / name
            if candidate.exists() or candidate.is_symlink():
                require(candidate.is_file() and digest(candidate) == files[name],
                        f"Stale prefix payload shadows the game copy: {candidate}; move it aside before setup")
        require(not (root / "netpunch/lobby.py").exists() or (root / "netpunch/netpunch.exe").is_file(),
                f"Prefix Python lobby shadows the released game lobby: {root / 'netpunch'}")


def links_for(game, steam, prefix):
    # Proton owns steamapps/libraryfolders.vdf: link only the required children.
    winsteam = prefix / "drive_c/Program Files (x86)/Steam"
    return {
        winsteam / "userdata": steam / "userdata",
        winsteam / "steamapps/common": game.parent,
        winsteam / "steamapps/workshop": game.parent.parent / "workshop",
    }


def validate_links(prefix, links):
    for link, target in links.items():
        plain_path(prefix, link.parent)
        if link.is_symlink():
            require(link.resolve() == target.resolve(), f"Conflicting prefix link: {link} -> {os.readlink(link)}")
        elif link.exists():
            require(link.is_dir() and not any(link.iterdir()), f"Prefix path contains existing data; move it aside first: {link}")


def atomic_copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".tpf2mp-", dir=target.parent)
    os.close(fd)
    try:
        shutil.copy2(source, temporary)
        os.replace(temporary, target)
    finally:
        Path(temporary).unlink(missing_ok=True)


def verify(game, prefix, links, files=FILES):
    require((game / "alut_real.dll").is_file() and digest(game / "alut_real.dll") == MANIFEST["stock_alut_sha256"], "Preserved stock alut_real.dll is missing or changed")
    for relative, expected in files.items():
        require((game / relative).is_file() and digest(game / relative) == expected, f"Installed payload is missing or changed: {relative}")
    for link, target in links.items():
        require(link.is_symlink() and link.resolve() == target.resolve(), f"Required prefix link is missing or changed: {link}")
    prefix_shadows(prefix, files)
    variant = "official Windows 0.4.22" if files == FILES else "Windows 0.4.22 with the pinned lobby dependency repair"
    print(f"PASS: {len(files)} files verified ({variant}), including 24 unchanged Lua files; stock audio and prefix links verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--steam-root", type=Path, required=True)
    parser.add_argument("--payload-dir", type=Path, required=True, help="extracted MSI's Transport Fever 2 directory")
    parser.add_argument("--repaired-lobby", type=Path, help="explicitly install/verify the pinned miniupnpc relocation repair")
    parser.add_argument("--prefix", type=Path, help="pfx directory; default: game's Steam library/steamapps/compatdata/1066780/pfx")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--dry-run", action="store_true")
    modes.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    game, steam, payload = (p.expanduser().resolve() for p in (args.game_dir, args.steam_root, args.payload_dir))
    prefix = (args.prefix or game.parent.parent / "compatdata/1066780/pfx").expanduser().resolve()
    require(game != payload and not payload.is_relative_to(game), "Payload must be outside the installed game directory")
    require((steam / "userdata").is_dir() and (steam / "steamapps").is_dir(), "Steam root must contain userdata and steamapps")
    validate_payload(payload)
    files = FILES.copy()
    sources = {relative: payload / relative for relative in files}
    if args.repaired_lobby:
        from fix_lobby_relocations import REPAIRED_SHA256
        repaired = args.repaired_lobby.expanduser().resolve()
        require(repaired.is_file() and digest(repaired) == REPAIRED_SHA256, "Repaired lobby does not match the pinned repair; generate it with fix_lobby_relocations.py")
        files["netpunch/netpunch.exe"] = REPAIRED_SHA256
        sources["netpunch/netpunch.exe"] = repaired
    validate_game(game)
    for relative in (*FILES, "alut_real.dll", ".tpf2mp-proton-backups"):
        plain_path(game, game / relative)
        require(not (game / relative).exists() or (game / relative).is_file() or relative == ".tpf2mp-proton-backups",
                f"Expected a file at {game / relative}")
    existing_mod = {p.relative_to(game).as_posix() for p in (game / MOD).rglob("*") if p.is_file() or p.is_symlink()}
    require(existing_mod <= FILES.keys(), f"Unexpected files in installed multiplayer mod: {sorted(existing_mod - FILES.keys())}")
    stock = game / "alut_real.dll" if (game / "alut_real.dll").exists() else game / "alut.dll"
    require(stock.is_file() and digest(stock) == MANIFEST["stock_alut_sha256"], f"Cannot preserve audio: {stock} is not the stock build 35924 alut.dll")
    prefix_shadows(prefix, files)
    links = links_for(game, steam, prefix)
    validate_links(prefix, links)
    if args.verify:
        verify(game, prefix, links, files)
        return
    # Activate the proxy only after all its dependencies have been installed.
    changes = sorted((relative for relative, expected in files.items()
                      if not (game / relative).is_file() or digest(game / relative) != expected),
                     key=lambda relative: (relative == "alut.dll", relative))
    missing_links = [link for link in links if not link.is_symlink()]
    variant = "Windows 0.4.22 + lobby dependency repair" if args.repaired_lobby else "Official 0.4.22"
    print(f"{variant}: {len(changes)} files to install, {len(missing_links)} prefix links to create")
    print("This compatibility package retains Windows 0.4.22 native fallbacks; global strict cancellation and cross-platform determinism remain unvalidated.")
    if args.dry_run:
        return
    running = game_running(game)
    require(not running, f"Close the game before installation (PID {running}); setup has changed nothing")
    if not (game / "alut_real.dll").exists():
        atomic_copy(stock, game / "alut_real.dll")
    backup = None
    for relative in changes:
        target = game / relative
        if target.exists():
            if backup is None:
                backup = game / ".tpf2mp-proton-backups" / f"{time.strftime('%Y%m%d-%H%M%S', time.gmtime())}-{time.time_ns()}"
                backup.mkdir(parents=True)
            atomic_copy(target, backup / relative)
        atomic_copy(sources[relative], target)
    for link in missing_links:
        link.parent.mkdir(parents=True, exist_ok=True)
        if link.exists():
            link.rmdir()  # preflight permits only empty directories here
        link.symlink_to(links[link], target_is_directory=True)
    if backup:
        print(f"Previous managed files preserved: {backup}")
    verify(game, prefix, links, files)
    print("Select Proton in Steam and remove any Linux preload launch wrapper before starting the game.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
