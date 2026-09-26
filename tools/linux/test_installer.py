"""Black-box install/upgrade/uninstall tests in a temporary fake Steam library.

Usage: python3 tools/linux/test_installer.py /path/to/tpf2mp-linux-VERSION.run
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(args, env):
    result = subprocess.run([str(a) for a in args], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    if result.returncode:
        raise AssertionError(result.stdout)
    return result.stdout


with tempfile.TemporaryDirectory(prefix="tpf2mp-installer-test-") as temp:
    root = Path(temp)
    env = dict(os.environ, SNAP_REAL_HOME=str(root / "profile"))
    release = root / "release"
    run(["bash", Path(sys.argv[1]).resolve(), "--extract", release], env)
    if (release / "lib/tpf2_pluginhost.so").exists():
        assert (release / "lib/plugins/tpf2_previews.so").is_file(), "native preview plugin missing from release"
        assert (release / "lib/plugins/tpf2_bigmap.so").is_file(), "Big Maps missing from unified release"
        assert (release / "lib/plugins/tpf2_bigmap.cfg").is_file()
        assert (release / "lib/bigmap-density-restore").is_file()

    def checksum_release():
        rows = []
        for path in sorted(release.rglob("*")):
            if path.is_file() and path.name != "SHA256SUMS":
                rows.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(release)}\n")
        (release / "SHA256SUMS").write_text("".join(rows))

    # PyInstaller onedir bundles may use both library and directory aliases.
    internal = release / "netpunch/_internal"
    internal.mkdir(parents=True, exist_ok=True)
    (internal / "tpf2mp-test-target.so").write_bytes(b"alias target")
    (internal / "tpf2mp-test-alias.so").symlink_to("tpf2mp-test-target.so")
    (internal / "tpf2mp-test-directory").mkdir()
    (internal / "tpf2mp-test-directory/payload").write_bytes(b"directory target")
    (internal / "tpf2mp-test-directory-alias").symlink_to("tpf2mp-test-directory", target_is_directory=True)
    checksum_release()
    game = root / "Steam/steamapps/common/Transport Fever 2"
    game.mkdir(parents=True)
    shutil.copy2("/bin/true", game / "TransportFever2")
    stock = b'#!/bin/sh\nexec ./TransportFever2 "$@"\n'
    (game / "run.sh").write_bytes(stock)
    (game / "run.sh").chmod(0o755)
    data_home = root / "data-home"
    # A valid path containing '..' must have canonical manifest entries so
    # upgrade/uninstall's traversal guard does not ignore its own files.
    requested_data_home = root / "unused/../data-home"
    installed = data_home / "tpf2mp"
    install = ["bash", release / "install.sh", "--game", game,
               "--data-home", requested_data_home, "--force"]
    shipped = {}
    shipped_links = {}
    for folder, target in [(release / "lib", installed),
                           (release / "netpunch", installed / "netpunch"),
                           (release / "mod/mp_lockstep_1", game / "mods/mp_lockstep_1")]:
        for source in folder.rglob("*"):
            if source.is_symlink():
                shipped_links[target / source.relative_to(folder)] = os.readlink(source)
            if source.is_file():
                shipped[target / source.relative_to(folder)] = hashlib.sha256(source.read_bytes()).digest()

    def verify_installed():
        for path, digest in shipped.items():
            assert path.is_file(), f"missing installed file: {path}"
            assert hashlib.sha256(path.read_bytes()).digest() == digest, f"changed installed file: {path}"
        for path, link in shipped_links.items():
            assert path.is_symlink() and os.readlink(path) == link, f"missing or changed installed alias: {path}"

    output = run(install + ["--dry-run"], env)
    assert not installed.exists() and not (game / "mods").exists()
    legacy = game / "mods/m3_determinism_1"
    legacy.mkdir(parents=True)
    (legacy / "mod.lua").write_text("obsolete probe")
    output = run(install + ["--dry-run"], env)
    assert legacy.is_dir() and "would remove" in output
    output = run(install, env)
    assert not legacy.exists()
    assert "tpf2mp-launch %command%" in output
    assert (game / "run.sh").read_bytes() == stock
    assert (installed / "netpunch/netpunch").is_file()
    assert (installed / "netpunch/_internal").is_dir()
    assert (installed / "tpf2mp-launch").stat().st_mode & 0o111
    verify_installed()
    rows = (installed / "tpf2mp_install.txt").read_text().splitlines()
    for path in shipped.keys() | shipped_links.keys():
        if path.is_relative_to(installed):
            assert f"file\t{path}" in rows, f"file missing from manifest: {path}"
    # Execute the wrapper with a harmless program and inspect exactly its child env.
    output = run([installed / "tpf2mp-launch", "/usr/bin/env"], env)
    assert f"XDG_DATA_HOME={data_home}\n" in output
    # boot/ alone: the container mounts the preloaded library's folder read-only.
    assert f"LD_PRELOAD={installed}/boot/libtpf2mp_boot.so" in output
    # Upgrade must delete formerly shipped files while preserving runtime/user data.
    (installed / "old-library.so").write_text("old")
    (installed / "old-alias.so").symlink_to("old-library.so")
    with (installed / "tpf2mp_install.txt").open("a") as f:
        f.write(f"file\t{installed}/old-library.so\n")
        f.write(f"file\t{installed}/old-alias.so\n")
    (installed / "data").mkdir(exist_ok=True)
    (installed / "data/user-save.txt").write_text("keep")
    (installed / "user.cfg").write_text("keep")
    # A density record must be handled before an upgrade removes its helper.
    base_mod = game / 'base_mod.lua'
    base_mod.write_text('-- stock fixture\n')
    density_record = installed / 'data/bigmap-base-mod.path'
    density_record.write_text(str(base_mod) + '\n')
    run(install + ['--dry-run'], env)
    assert density_record.exists()
    run(install, env)
    assert not density_record.exists()
    assert base_mod.read_text() == '-- stock fixture\n'
    verify_installed()
    assert not (installed / "old-library.so").exists()
    assert not (installed / "old-alias.so").is_symlink()
    assert (installed / "user.cfg").read_text() == "keep"
    assert (installed / "data/user-save.txt").read_text() == "keep"

    # Invalid loading changes must fail before updating libraries, mod or manifest.
    manifest_before = (installed / "tpf2mp_install.txt").read_bytes()
    next_boot = release / "lib/libtpf2mp_boot.so"
    next_mod = release / "mod/mp_lockstep_1/mod.lua"
    original_boot, original_mod = next_boot.read_bytes(), next_mod.read_bytes()
    next_boot.write_bytes(original_boot + b"next version")
    next_mod.write_bytes(original_mod + b"\n-- next version\n")
    checksum_release()
    for contents, loading in [(None, "--patch-runsh"),
                              (b"no shebang\n", "--patch-runsh"),
                              (stock + b"# tpf2mp (Linux port): preload the multiplayer loader when it is installed.\n# edited\n", "--patch-runsh"),
                              (stock + b"# tpf2mp (Linux port): preload the multiplayer loader when it is installed.\n# edited\n", "--launch-options")]:
        if contents is None:
            (game / "run.sh").unlink()
        else:
            (game / "run.sh").write_bytes(contents)
        rejected = subprocess.run([str(a) for a in install + [loading]], env=env, capture_output=True)
        assert rejected.returncode != 0, "invalid run.sh accepted"
        verify_installed()
        assert (installed / "tpf2mp_install.txt").read_bytes() == manifest_before
        assert not (game / "run.sh.tpf2mp-orig").exists()
        (game / "run.sh").write_bytes(stock)
    next_boot.write_bytes(original_boot)
    next_mod.write_bytes(original_mod)
    checksum_release()

    run(install + ["--patch-runsh"], env)
    assert (game / "run.sh").read_bytes() != stock
    run(install + ["--launch-options"], env)
    verify_installed()
    assert (game / "run.sh").read_bytes() == stock
    # Corrupt releases must fail before changing an existing install.
    before = hashlib.sha256((installed / "libtpf2mp_boot.so").read_bytes()).digest()
    with (release / "lib/libtpf2mp_boot.so").open("ab") as f:
        f.write(b"corrupt")
    bad = subprocess.run([str(a) for a in install], env=env, capture_output=True)
    assert bad.returncode != 0
    assert hashlib.sha256((installed / "libtpf2mp_boot.so").read_bytes()).digest() == before
    density_record.write_text(str(base_mod) + '\n')
    run(["bash", release / "uninstall.sh", "--game", game, "--data-home", requested_data_home, "--force"], env)
    assert not density_record.exists()
    assert not (installed / "libtpf2mp_boot.so").exists()
    assert not (installed / "netpunch/netpunch").exists()
    assert not (installed / "tpf2mp-launch").exists()
    assert not (game / "mods/mp_lockstep_1").exists()
    for path in shipped.keys() | shipped_links.keys():
        assert not path.exists() and not path.is_symlink(), f"packaged file remains after uninstall: {path}"
    assert (game / "run.sh").read_bytes() == stock
    assert (installed / "data/user-save.txt").read_text() == "keep"
    assert (installed / "user.cfg").read_text() == "keep"
print("PASS: .run extraction/checksums, dry-run, wrapper, install, aliases, upgrade, loading preflight, legacy migration, uninstall/data preservation")
