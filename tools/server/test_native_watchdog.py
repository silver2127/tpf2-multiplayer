#!/usr/bin/env python3
"""Exercise native supervision with real child processes, without a game."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

import native_watchdog as watchdog


@unittest.skipUnless(sys.platform == "linux", "Linux process groups")
class WatchdogTest(unittest.TestCase):
    def test_installation_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            env = {"GAME_DIR": str(root / "game with spaces"), "XDG_DATA_HOME": str(root / "data")}
            with self.assertRaises(RuntimeError):
                watchdog.command(env)
            for path in (root / "game with spaces/TransportFever2", root / "game with spaces/run.sh",
                         root / "data/tpf2mp/tpf2mp-launch"):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            args, cwd, log = watchdog.command(env)
            self.assertEqual(args, [str(root / "data/tpf2mp/tpf2mp-launch"), str(cwd / "run.sh")])
            self.assertEqual(log, root / "data/tpf2mp/data/tpf2_menu.log")

    def test_stop_only_own_session(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], start_new_session=True)
        owned = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], start_new_session=True)
        try:
            watchdog.stop(owned)
            self.assertEqual(owned.returncode, -signal.SIGTERM)
            self.assertIsNone(unrelated.poll())
        finally:
            watchdog.stop(unrelated)

    def test_exited_wrapper_does_not_orphan_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / "child"
            # The wrapper exits after its child has installed its shutdown handler.
            code = ("import os,signal,time\n"
                    "signal.signal(signal.SIGTERM, lambda *_: (open('stopped','w').close(), exit(0)))\n"
                    "open('child','w').write(str(os.getpid()))\n"
                    "while True: time.sleep(.1)\n")
            wrapper = subprocess.Popen([sys.executable, "-c",
                "import subprocess,sys,time,pathlib; subprocess.Popen([sys.executable,'-c',sys.argv[1]]); "
                "\nwhile not pathlib.Path('child').exists(): time.sleep(.01)", code],
                cwd=tmp, start_new_session=True)
            wrapper.wait(timeout=10)
            self.assertTrue(marker.exists())
            watchdog.stop(wrapper)
            self.assertTrue((Path(tmp) / "stopped").exists())


if __name__ == "__main__":
    unittest.main()
