"""The Workshop registry a lobby publishes at start (netpunch/lobby.py
_publish_registry_at_start): a dedicated server keeps every mod in its managed
Workshop folder registered, a player's host starts with none until a save's mod
list arrives. A dedicated server loads its world by itself before any mod list
reaches the lobby, and its Steam client is offline: scoped to nothing, its world
load was refused for a missing mod (2026-09-27).

  python tools/registry_dedicated_test.py
"""
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "netpunch"))


def mod(folder):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "mod.lua").write_text("function data() return {} end\n", encoding="utf-8")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        os.environ["TPF2MP_DATADIR"] = tmp
        os.environ["XDG_DATA_HOME"] = tmp
        import modshare
        import lobby
        data = Path(modshare.data_dir())
        mod(data / "workshop" / "2403182754")          # put there by the operator
        elsewhere = Path(tmp) / "library" / "1954591986"
        mod(elsewhere)                                 # a row an earlier save published
        gone = Path(tmp) / "library" / "123"           # a row whose folder is gone
        (data).mkdir(parents=True, exist_ok=True)
        token = "0123456789abcdef0123456789abcdef"
        Path(modshare.registry_path()).write_text(
            f"{token}\n1954591986\t{elsewhere}\n123\t{gone}\n", encoding="utf-8")
        log = []

        lobby._publish_registry_at_start(log.append, dedicated=True)
        got_token, rows = modshare.read_registry()
        assert got_token == token, "the token a pending receipt waits for is kept"
        assert set(rows) == {"2403182754", "1954591986"}, rows
        assert Path(rows["2403182754"]) == (data / "workshop" / "2403182754").resolve()

        lobby._publish_registry_at_start(log.append)   # a player's host: nothing until a save
        assert modshare.read_registry()[1] == {}, modshare.read_registry()
    print("PASS: a dedicated server's start keeps its Workshop folders registered; a host starts with none")


if __name__ == "__main__":
    main()
