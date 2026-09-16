import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('auto_install', Path(__file__).with_name('auto_install.py'))
auto = importlib.util.module_from_spec(spec)
spec.loader.exec_module(auto)


class AutoInstall(unittest.TestCase):
    def test_build_while_running_install_all_copies_after_exit(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'installer').mkdir()
            (root / 'installer/VERSION').write_text('0.5.6')
            installer = root / 'dist/linux-auto/tpf2mp-linux-0.5.6/install.sh'
            calls = []
            def run(args, check):
                calls.append(args)
                if args[0].endswith('build_release.sh'):
                    installer.parent.mkdir(parents=True, exist_ok=True)
                    installer.touch()
            state = {}
            with patch.object(auto, 'source_digest', return_value='source'):
                auto.pass_once(root, ['A', 'B'], state, run, lambda: True)
                self.assertEqual(len(calls), 1)
                auto.pass_once(root, ['A', 'B'], state, run, lambda: False)
                self.assertEqual([c[-1] for c in calls[1:]], ['A', 'B'])
                auto.pass_once(root, ['A', 'B'], state, run, lambda: False)
                self.assertEqual(len(calls), 3)
                self.assertTrue(all('--force' not in c for c in calls))


if __name__ == '__main__':
    unittest.main()
