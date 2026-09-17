"""Real control files, simulated native/Lua completions; no engine claims."""
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
from sync_runtime import SyncParticipant, read_fields, write_fields
from sync_snapshot import PreparedSnapshot
from sync_lobby import publish_prompt


class RuntimeTests(unittest.TestCase):
    def test_native_prompt_is_local_fresh_and_does_not_reopen_after_recovery(self):
        from unittest.mock import Mock
        io = Mock()
        def notice(world='old', count=1, held=0, wall=100, pid=123):
            write_fields(self.root/'tpf2_sync_notice.txt',
                         dict(pid=pid, world=world, desyncs=count, held=held, wall=wall))
            publish_prompt(self.runtime, io, True, 100)
        notice(pid=999)
        notice(wall=94)
        notice(world='invalid token')
        io.emit.assert_not_called()
        notice()
        io.emit.assert_called_once_with(dict(type='sync_prompt', phase='detected'))
        self.assertIsNone(self.runtime.state)  # a notice does not pause or save
        notice()
        self.assertEqual(io.emit.call_count, 1)
        self.phase('holding')
        notice(count=2)
        self.phase('complete')
        notice(count=2)
        self.assertEqual(io.emit.call_count, 1)  # old world's desync stays consumed
        notice(world='new', count=0)
        io.emit.assert_called_with(dict(type='sync_prompt', phase='clear'))
        notice(world='new', count=1)
        io.emit.assert_called_with(dict(type='sync_prompt', phase='detected'))
        publish_prompt(self.runtime, io, False, 100)
        io.emit.assert_called_with(dict(type='sync_prompt', phase='unavailable'))

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

    def test_engine_progress_is_its_work_not_the_mailbox_heartbeat(self):
        import time
        from sync_runtime import engine_work, ENGINE_CPU_STEP_MS, ENGINE_IO_STEP
        self.phase('loading')
        fields = dict(supported=1, has_world=0, busy=1, cpu_ui=5000, cpu_command=0,
                      io_read=10 * ENGINE_IO_STEP, io_write=0)
        self.write('native_status', **fields)
        token = self.runtime.progress()
        self.assertIn('engine:cpu_ui=5,io_read=10', token)
        # The mailbox rewriting the same numbers every 100 ms is not progress:
        # that heartbeat ticked while a hung game thread sat in a save, and
        # the barrier's silence timeout never fired (2026-09-16).
        for _ in range(3):
            time.sleep(0.01)
            self.write('native_status', **fields)
            self.assertEqual(self.runtime.progress(), token)
        # Sub-step noise is not progress either: a dialog-blocked thread
        # burning milliseconds, a log line written.
        self.write('native_status', **dict(fields, cpu_ui=5000 + ENGINE_CPU_STEP_MS - 1,
                                           io_read=fields['io_read'] + ENGINE_IO_STEP - 1))
        self.assertEqual(self.runtime.progress(), token)
        # A CPU second burnt by the loading thread, or 16 MB read, is.
        self.write('native_status', **dict(fields, cpu_ui=5000 + ENGINE_CPU_STEP_MS))
        self.assertNotEqual(self.runtime.progress(), token)
        self.write('native_status', **dict(fields, io_read=fields['io_read'] + ENGINE_IO_STEP))
        self.assertNotEqual(self.runtime.progress(), token)
        # Loading is the UI thread reading; the command thread and the writes
        # are the engine SAVING, and mean nothing here.
        self.write('native_status', **dict(fields, cpu_command=99000, io_write=99 * ENGINE_IO_STEP))
        self.assertEqual(self.runtime.progress(), token)
        self.assertEqual(engine_work('saving', {'cpu_command': '2500', 'io_write': str(3 * ENGINE_IO_STEP + 5)}),
                         'cpu_command=2,io_write=3')
        self.assertEqual(engine_work('loading', {'cpu_ui': 'garbage', 'io_read': '1'}), 'io_read=0')
        # An idle engine reports no engine part at all.
        self.write('native_status', **dict(fields, busy=0))
        self.assertNotIn('engine:', self.runtime.progress())
        # A status without the counters (an older DLL) is static while busy:
        # silence, never a heartbeat.
        self.write('native_status', supported=1, has_world=0, busy=1)
        older = self.runtime.progress()
        time.sleep(0.01)
        self.write('native_status', supported=1, has_world=0, busy=1)
        self.assertEqual(self.runtime.progress(), older)

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

    def test_host_loads_the_snapshot_in_every_mode(self):
        # resync and join alike: the host took the snapshot from the world it is
        # holding, and still LOADS it -- a world kept running from memory holds
        # its entities in creation order, a loaded one in save order, and the
        # person sim consumes that order (measured 2026-09-16: a catch-up joiner
        # split within ~35 game units). Both peers loading the file agree.
        for mode in ('resync', 'join', 'start'):
            self.runtime = SyncParticipant(self.root, self.root, 123, 'host')
            self.state.update(mode=mode, revision=self.state['revision'] + 1, phase='transferring')
            self.assertTrue(self.runtime.accept(self.state))
            self.runtime.snapshot = self.snapshot
            self.lua()
            self.runtime.tick()
            self.phase('loading')
            self.write('bridge_ctl', instance='a', peer='127.0.0.1:7773')
            self.assertIsNone(self.runtime.tick())
            self.write('epoch_ready', epoch=self.state['epoch'], ok=1)
            self.runtime.tick()
            request = read_fields(self.root / 'tpf2_native_request.txt')
            self.assertEqual(request['cmd'], 'load', mode)
            (self.root / 'tpf2_native_request.txt').unlink()

    def test_load_requires_bridge_epoch_native_ready_and_new_lua_world(self):
        self.runtime = SyncParticipant(self.root, self.root, 123, 'client')
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
