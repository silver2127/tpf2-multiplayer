"""Exercise the shipping policy without importing lobby networking dependencies."""
import ast
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import os
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'netpunch/lobby.py'
node = next(n for n in ast.parse(SOURCE.read_text()).body
            if isinstance(n, ast.FunctionDef) and n.name == '_live_join_on')


class PolicyTest(unittest.TestCase):
    def test_platform_defaults_and_runtime_switch(self):
        for platform in ('win32', 'linux', 'linux2'):
            namespace = {'os': os, 'sys': SimpleNamespace(platform=platform)}
            exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), namespace)
            policy = namespace['_live_join_on']
            with TemporaryDirectory() as directory:
                switch = Path(directory) / 'tpf2mp_live_join.txt'
                self.assertEqual(policy(directory), platform == 'win32')
                for value in ('0', 'off', 'NO', ' Off\n'):
                    switch.write_text(value)
                    self.assertFalse(policy(directory))
                for value in ('1', 'on', 'YES', ' On\n'):
                    switch.write_text(value)
                    self.assertTrue(policy(directory))
                for value in ('', 'invalid'):
                    switch.write_text(value)
                    self.assertEqual(policy(directory), platform == 'win32')
                switch.unlink()
                self.assertEqual(policy(directory), platform == 'win32')


if __name__ == '__main__':
    unittest.main()
