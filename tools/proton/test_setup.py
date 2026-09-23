#!/usr/bin/env python3
"""Exercise Proton installation in temporary directories using verified release files."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

SCRIPT = Path(__file__).with_name("setup.py")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload-dir", type=Path, required=True)
    parser.add_argument("--stock-alut", type=Path, required=True)
    parser.add_argument("--repaired-lobby", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tpf2mp-proton-test-") as temporary:
        root = Path(temporary)
        steam = root / "Steam spaces '$()"
        game = steam / "steamapps/common/Transport Fever 2"
        prefix = steam / "steamapps/compatdata/1066780/pfx"
        game.mkdir(parents=True)
        (steam / "userdata").mkdir()
        pe = bytearray(256)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 0x3c, 128)
        pe[128:132] = b"PE\0\0"
        struct.pack_into("<H", pe, 132, 0x8664)
        struct.pack_into("<I", pe, 136, 0x675abcc6)
        struct.pack_into("<I", pe, 208, 0x046ce000)
        (game / "TransportFever2.exe").write_bytes(pe)
        shutil.copy2(args.stock_alut, game / "alut.dll")
        command = [sys.executable, str(SCRIPT), "--game-dir", str(game), "--steam-root", str(steam), "--payload-dir", str(args.payload_dir)]

        def run(*extra, ok=True):
            result = subprocess.run(command + list(extra), text=True, capture_output=True, timeout=30)
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            return result.stdout + result.stderr

        original = (game / "alut.dll").read_bytes()
        run("--dry-run")
        assert not prefix.exists() and not (game / "alut_real.dll").exists()
        # Bad stock must fail before creating prefix directories or copying DLLs.
        (game / "alut.dll").write_bytes(b"unrecognized proxy")
        run(ok=False)
        assert not prefix.exists() and not (game / "tpf2_menu.dll").exists()
        (game / "alut.dll").write_bytes(original)

        # Fresh prefix setup remains compatible with later Proton initialization.
        run()
        run("--verify")
        winsteam = prefix / "drive_c/Program Files (x86)/Steam"
        (winsteam / "steamapps/libraryfolders.vdf").write_text("Proton-owned registry of libraries")
        (prefix / "user.reg").write_text("Wine registry fixture")
        assert (game / "alut_real.dll").read_bytes() == original
        backups = list((game / ".tpf2mp-proton-backups").iterdir())
        mtimes = {p: p.stat().st_mtime_ns for p in game.rglob("*") if p.is_file()}
        assert "0 files to install, 0 prefix links" in run()
        assert all(p.stat().st_mtime_ns == mtime for p, mtime in mtimes.items())
        assert list((game / ".tpf2mp-proton-backups").iterdir()) == backups
        assert (winsteam / "steamapps/libraryfolders.vdf").read_text() == "Proton-owned registry of libraries"

        # A conflicting prefix directory is rejected before replacing a changed cfg.
        cfg = game / "tpf2_slice.cfg"
        cfg.write_text("user diagnostic settings")
        (winsteam / "userdata").unlink()
        (winsteam / "userdata").mkdir()
        (winsteam / "userdata/existing-save").write_text("keep")
        run(ok=False)
        assert cfg.read_text() == "user diagnostic settings"
        assert (winsteam / "userdata/existing-save").read_text() == "keep"
        (winsteam / "userdata/existing-save").unlink()
        # Empty destination directories may be replaced, with the modified cfg backed up.
        run()
        assert any((p / "tpf2_slice.cfg").is_file() and (p / "tpf2_slice.cfg").read_text() == "user diagnostic settings"
                   for p in (game / ".tpf2mp-proton-backups").iterdir())
        run("--verify")

        shadow = prefix / "drive_c/users/steamuser/AppData/Local/tpf2mp/tpf2_slice.dll"
        shadow.parent.mkdir(parents=True)
        shadow.write_bytes(b"old dll")
        assert "shadows" in run("--verify", ok=False)
        shadow.unlink()
        extra = game / "mods/mp_lockstep_1/res/scripts/unrequested.lua"
        extra.write_text("return {}")
        assert "Unexpected files" in run(ok=False)
        extra.unlink()
        run("--verify")
        if args.repaired_lobby:
            repaired_args = ["--repaired-lobby", str(args.repaired_lobby)]
            run(*repaired_args)
            assert "pinned lobby dependency repair" in run(*repaired_args, "--verify")
            run("--verify", ok=False)
            assert "0 files to install" in run(*repaired_args)
            invalid = root / "invalid repair.exe"
            invalid.write_bytes(b"not the verified repair")
            assert "pinned repair" in run("--repaired-lobby", str(invalid), "--dry-run", ok=False)
            run()  # default installation restores the original and backs up the repair
            run("--verify")
        # An altered official payload is rejected, even if its path looks correct.
        changed = root / "changed payload"
        shutil.copytree(args.payload_dir, changed)
        (changed / "mods/mp_lockstep_1/mod.lua").write_text("return {}")
        command[-1] = str(changed)
        assert "checksum mismatch" in run("--dry-run", ok=False)
    print("PASS: dry run, stock protection, fresh prefix, idempotence, backups, prefix conflicts, stale DLLs, exact Lua and payload checks")


if __name__ == "__main__":
    main()
