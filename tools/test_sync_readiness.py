"""Authority and readiness checks before any engine-side recovery begins."""
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
from sync_lobby import HostRecovery, ClientRecovery, ui_state


class ReadinessTests(unittest.TestCase):
    def make_host(self, count=3):
        self.members = ['host'] + [f'p{i}' for i in range(1, count)]
        self.io = Mock()
        self.runtime = Mock(state=None)
        self.runtime._read.return_value = {}
        self.host = HostRecovery(self.runtime, 'host', self.io, Mock(),
            lambda: self.members, lambda: [], Mock())
        return self.host

    def test_unavailable_request_explains_why_without_starting(self):
        for count in (2, 3):
            h = self.make_host(count)
            h.is_available = lambda: False
            h.unavailable_reason = lambda: 'A player is receiving the save.'
            h.command('host', dict(cmd='sync_request', id='blocked'))
            self.assertIsNone(h.barrier.operation)
            self.io.emit.assert_called_with(dict(type='sync_feedback',
                detail='A player is receiving the save.'))

    def test_repeated_request_shows_current_readiness(self):
        h = self.make_host(3)
        h.command('host', dict(cmd='sync_request', id='first'))
        token = h.readiness['token']
        self.io.reset_mock()
        h.command('host', dict(cmd='sync_request', id='second'))
        self.assertEqual(h.readiness['token'], token)
        self.assertEqual(self.io.emit.call_args.args[0]['ready_count'], 1)
        self.assertIsNone(h.barrier.operation)

    def test_ui_preserves_failure_step_and_reason(self):
        state = dict(phase='error', error=dict(step='loading', detail='Player p1 timed out'))
        message = ui_state(state)
        self.assertEqual(message['step'], 'loading')
        self.assertEqual(message['detail'], 'Player p1 timed out')
        self.assertNotIn('detail', state)
        self.assertEqual(ui_state(dict(phase='holding', error=None))['detail'], '')

    def test_host_only_two_player_start(self):
        h = self.make_host(2)
        for kind in ('sync_request', 'sync_retry', 'sync_abort'):
            h.command('p1', dict(cmd=kind, id=kind, sender='host'))
        self.assertIsNone(h.barrier.operation)
        h.command('host', dict(cmd='sync_request', id='host-start'))
        self.assertEqual(h.barrier.phase, 'holding')
        self.assertIsNone(h.readiness)

    def test_all_members_and_current_token_required(self):
        for count in (3, 5, 8):
            h = self.make_host(count)
            h.command('host', dict(cmd='sync_request', id='start'))
            token = h.readiness['token']
            self.assertEqual(h.readiness['ready'], ['host'])
            for p in self.members[1:]:
                h.command(p, dict(cmd='sync_ready', id='old'+p, token='old'))
            self.assertIsNone(h.barrier.operation)
            for p in self.members[1:-1]:
                msg = dict(cmd='sync_ready', id=p, token=token)
                h.command(p, msg)
                h.command(p, msg)
            self.assertIsNone(h.barrier.operation)
            h.command(self.members[-1], dict(cmd='sync_ready', id='last', token=token))
            self.assertEqual(h.barrier.phase, 'holding')
            self.assertEqual(h.readiness['phase'], 'started')

    def test_roster_change_invalidates_all_readiness(self):
        h = self.make_host()
        h.command('host', dict(cmd='sync_request', id='start'))
        old_token = h.readiness['token']
        self.members[-1] = 'replacement'
        h.tick(10)
        self.assertEqual(h.readiness['phase'], 'cancelled')
        self.assertIsNone(h.barrier.operation)
        self.runtime.accept.assert_not_called()
        h.command('host', dict(cmd='sync_request', id='new-start'))
        for p in self.members[1:]:
            h.command(p, dict(cmd='sync_ready', id=p, token=old_token))
        self.assertEqual(h.readiness['ready'], ['host'])
        self.assertIsNone(h.barrier.operation)

    def test_host_retry_requires_fresh_readiness(self):
        h = self.make_host()
        h.barrier.request('host', self.members, 'resync')
        h.barrier.fail('test failure')
        operation, epoch = h.barrier.operation, h.barrier.epoch
        h.command('p1', dict(cmd='sync_retry', id='client', operation=operation))
        self.assertIsNone(h.readiness)
        h.command('host', dict(cmd='sync_retry', id='host', operation=operation))
        self.assertEqual(h.barrier.epoch, epoch)
        for p in self.members[1:]:
            h.command(p, dict(cmd='sync_ready', id=p, token=h.readiness['token']))
        self.assertEqual(h.barrier.operation, operation)
        self.assertNotEqual(h.barrier.epoch, epoch)
        self.assertEqual(h.barrier.phase, 'holding')

    def test_delayed_ready_state_cannot_reopen_completed_operation(self):
        runtime = Mock(state=None)
        runtime._read.return_value = {}
        io = Mock()
        client = ClientRecovery(runtime, io, Mock(), Mock())
        client.identify('p1', 'host', True)
        client.replica.current = dict(revision=12)
        client.message(dict(t='sync_ready_state', host='host', members=['host', 'p1', 'p2'],
            ready=['host'], token='old', phase='waiting', revision=1, barrier_revision=1))
        io.emit.assert_not_called()
        client.command(dict(cmd='sync_request', id='blocked'))
        client.command(dict(cmd='sync_retry', id='blocked2'))
        self.assertFalse(client.pending)


if __name__ == '__main__':
    unittest.main()
