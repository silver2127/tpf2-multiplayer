"""Real control files, simulated native/Lua completions; no engine claims."""
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
from sync_runtime import SyncParticipant, read_fields, write_fields
from sync_snapshot import PreparedSnapshot


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = SyncParticipant(self.root, self.root, 123, 'host')
        self.snapshot = PreparedSnapshot((('.sav', b'world'), ('.sav.lua', b'metadata')))
        self.state = dict(operation='a'*32, epoch='b'*32, revision=1, phase='holding',
                          members=['host', 'client'], host='host', mode='resync', resume_speed=0,
                          snapshot=dict(files=self.snapshot.files, digest=self.snapshot.digest))
        self.write('native_status', supported=1, has_world=1, busy=0)

    def write(self, kind, **fields):
        write_fields(self.root / ('tpf2_' + kind + '.txt'), dict(pid=123, **fields))

    def phase(self, phase):
        self.state.update(phase=phase, revision=self.state['revision']+1)
        self.assertTrue(self.runtime.accept(self.state))

    def lua(self, world='old', **fields):
        data = {k: self.state[k] for k in ('operation', 'epoch', 'revision', 'phase')}
        data.update(world=world, held=1, paused=1, speed=0)
        data.update(fields)
        self.write('sync_lua_ack', **data)

    def native_done(self, cmd, step, success=1):
        request = read_fields(self.root / 'tpf2_native_request.txt')
        self.assertEqual(request['cmd'], cmd)
        self.write('native_event', id=request['id'], step=step, success=success)

    def test_hold_waits_for_lua_before_drain_and_repeats_no_commands(self):
        self.phase('holding')
        self.assertIsNone(self.runtime.tick())
        self.native_done('hold', 'held')
        self.assertIsNone(self.runtime.tick())
        self.assertEqual(read_fields(self.root/'tpf2_native_request.txt')['cmd'], 'hold')
        self.lua()
        self.assertIsNone(self.runtime.tick())
        self.native_done('pause', 'paused')
        ack = self.runtime.tick()
        self.assertTrue(ack['drained'])
        self.assertEqual(ack['speed'], 0)
        self.assertEqual(self.runtime.tick(), ack)

    def test_load_requires_bridge_epoch_native_ready_and_new_lua_world(self):
        self.phase('transferring')
        self.runtime.snapshot = self.snapshot
        self.lua()
        self.runtime.tick()
        self.phase('loading')
        self.write('bridge_ctl', instance='a', peer='127.0.0.1:7773')
        self.assertIsNone(self.runtime.tick())
        self.assertEqual(self.runtime._read('tpf2_epoch_request.txt')['epoch'], self.state['epoch'])
        self.assertNotIn('epoch', self.runtime._read('tpf2_bridge_ctl.txt'))
        self.assertFalse((self.root/'tpf2_native_request.txt').exists())
        self.write('epoch_ready', epoch='c'*32, ok=1)
        self.assertIsNone(self.runtime.tick())
        self.write('epoch_ready', epoch=self.state['epoch'], ok=1)
        self.runtime.tick()
        self.native_done('load', 'load_accepted')
        self.assertIsNone(self.runtime.tick())
        self.native_done('load', 'world_ready')
        self.lua(world='old')
        self.assertIsNone(self.runtime.tick())
        self.lua(world='new')
        self.runtime.tick()
        self.native_done('pause', 'paused')
        self.assertTrue(self.runtime.tick()['paused'])

    def test_native_failure_does_not_continue(self):
        self.phase('holding')
        self.runtime.tick()
        self.native_done('hold', 'request', success=0)
        self.assertFalse(self.runtime.tick()['success'])
        self.assertEqual(read_fields(self.root/'tpf2_native_request.txt')['cmd'], 'hold')

    def test_error_and_abort_do_not_release(self):
        for phase in ('waiting', 'error', 'aborted'):
            self.phase(phase)
            self.assertIsNone(self.runtime.tick())
            self.assertFalse((self.root/'tpf2_native_request.txt').exists())

    def test_corrupt_transfer_never_installed(self):
        self.phase('transferring')
        with self.assertRaises(ValueError):
            self.runtime.receive_snapshot(b'wrong')
        self.assertIsNone(self.runtime.tick())
        self.assertEqual(list(self.root.glob('*.sav')), [])

    def test_partial_and_previous_process_files_ignored(self):
        self.phase('holding')
        self.write('native_status', supported=1, has_world=1)
        (self.root/'tpf2_native_status.txt').write_text('pid=123\nsupported=1')
        self.assertIsNone(self.runtime.tick())
        write_fields(self.root/'tpf2_native_status.txt', dict(pid=999, supported=1, has_world=1))
        self.assertIsNone(self.runtime.tick())
        self.assertFalse((self.root/'tpf2_native_request.txt').exists())

    def test_error_step_and_detail_reach_the_lua_control_file(self):
        self.phase('holding')
        control = read_fields(self.root/'tpf2_sync_lua.txt')
        self.assertEqual((control['step'], control['detail']), ('', ''))
        self.state['error'] = {'step': 'checking', 'detail': 'Fresh worlds\r\ndiffer: ' + 'x' * 500}
        self.phase('error')
        control = read_fields(self.root/'tpf2_sync_lua.txt')
        self.assertEqual(control['step'], 'checking')
        self.assertTrue(control['detail'].startswith('Fresh worlds differ: xxx'))
        self.assertEqual(len(control['detail']), 400)
        self.state['error'] = None

    def test_temporary_names_are_unique_and_removed(self):
        stale = self.root/'tpf2_sync_lua.txt.sync.tmp'
        stale.write_text('pid=999\n')
        self.phase('holding')
        self.phase('saving')
        self.assertEqual(read_fields(self.root/'tpf2_sync_lua.txt')['phase'], 'saving')
        self.assertEqual([p.name for p in self.root.glob('*.tmp')], [stale.name])

    def test_control_wire_format_is_lf_on_windows(self):
        self.phase('holding')
        raw = (self.root/'tpf2_sync_lua.txt').read_bytes()
        self.assertNotIn(b'\r', raw)
        self.assertTrue(raw.endswith(b'pid=123\n'))

    def test_transient_windows_replace_failure_retries_publication(self):
        import os
        replace = os.replace
        conflict = PermissionError('reader temporarily denies replacement')
        conflict.winerror = 32
        calls = []
        def busy_then_ready(source, target):
            calls.append(target)
            if len(calls) < 3:
                raise conflict
            return replace(source, target)
        with patch('sync_runtime.os.replace', side_effect=busy_then_ready):
            self.phase('holding')
        self.assertEqual(len(calls), 3)
        self.assertIsNone(self.runtime.ack)
        self.assertEqual(read_fields(self.root/'tpf2_sync_lua.txt')['phase'], 'holding')
        self.assertFalse((self.root/'tpf2_native_request.txt').exists())

    def test_persistent_control_failure_reports_error_without_lobby_crash(self):
        with patch('sync_runtime.os.replace', side_effect=OSError('disk unavailable')):
            self.phase('loading')
        self.assertFalse(self.runtime.tick()['success'])
        self.assertIn('disk unavailable', self.runtime.ack['detail'])
        self.assertFalse((self.root/'tpf2_native_request.txt').exists())


if __name__ == '__main__':
    unittest.main()
