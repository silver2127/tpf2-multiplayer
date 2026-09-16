"""Run the production PeersLog without starting a lobby or opening sockets."""
import ast
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'netpunch'))
source = ast.parse((ROOT / 'netpunch/lobby.py').read_text())
cls = next(n for n in source.body if isinstance(n, ast.ClassDef) and n.name == 'PeersLog')
namespace = dict(os=os, sys=sys, threading=threading, time=time, PEERS_LOG_NAME='peers.log')
exec(compile(ast.Module(body=[cls], type_ignores=[]), 'lobby.py', 'exec'), namespace)


class KeepLogs(unittest.TestCase):
    def test_data_flag_and_io_flag(self):
        with tempfile.TemporaryDirectory() as tmp:
            data, io = Path(tmp) / 'data', Path(tmp) / 'io'
            data.mkdir(); io.mkdir()
            path = io / 'peers.log'
            with patch.dict(os.environ, TPF2MP_DATADIR=str(data)):
                for flag in (data / 'tpf2mp_keep_logs.txt', io / 'tpf2mp_keep_logs.txt'):
                    path.write_text('old session\n'); flag.touch()
                    namespace['PeersLog'](str(io))
                    self.assertTrue(path.read_text().startswith('old session\n'))
                    flag.unlink()
                    namespace['PeersLog'](str(io))
                    self.assertNotIn('old session', path.read_text())


if __name__ == '__main__':
    unittest.main()
