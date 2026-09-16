"""Barrier model tests; these do not simulate successful real-engine I/O."""
import itertools
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
from sync_operation import SyncOperation, SyncReplica, snapshot_digest

FILES = [{'suffix': s, 'size': 100, 'sha256': 'a' * 64} for s in ('.sav', '.sav.lua')]


class OperationTests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        ids = iter(str(i) for i in itertools.count())
        self.op = SyncOperation('host', clock=lambda: self.now, token=lambda: next(ids))
        self.op.request('client', ['host', 'client'], 'resync')

    def ack(self, sender, **fields):
        data = {k: getattr(self.op, k) for k in ('operation', 'revision', 'epoch', 'phase')}
        data.update(success=True, paused=True, drained=True, speed=0,
                    digest=self.op.snapshot['digest'] if self.op.snapshot else None,
                    files=FILES, fingerprint='fresh-world')
        data.update(fields)
        self.op.acknowledge(sender, data)
        return data

    def both(self):
        self.ack('client')
        self.ack('host')

    def transfer(self):
        self.both()
        self.ack('host')
        self.assertEqual(self.op.phase, 'transferring')

    def test_simultaneous_requests_share_operation(self):
        initial = self.op.view()
        self.assertEqual(self.op.request('host', ['host', 'client'], 'resync'), initial)
        self.assertEqual(len(self.op.take_effects()), 1)

    def test_detected_desync_holds_until_one_player_requests_recovery(self):
        self.op.abort('host', self.op.operation)
        self.op.request('client', ['host', 'client'], 'resync', confirmed=False)
        self.both()
        self.assertEqual(self.op.phase, 'waiting')
        self.now = 10000
        self.op.tick(['host', 'client'])
        self.assertEqual(self.op.phase, 'waiting')
        self.op.request('host', ['host', 'client'], 'resync')
        self.assertEqual(self.op.phase, 'saving')

    def test_release_needs_every_barrier_and_keeps_user_pause(self):
        self.transfer()
        self.both()
        self.both()
        self.both()
        self.assertEqual(self.op.phase, 'releasing')
        self.ack('host')
        self.assertEqual(self.op.phase, 'releasing')
        self.ack('client')
        self.assertEqual(self.op.phase, 'complete')
        self.assertEqual(self.op.resume_speed, 0)

    def test_every_engine_speed_is_a_valid_resume_speed(self):
        # The engine's speed index runs 0..4 and a session really does sit at 3
        # (a speed vote lands there). Every holding ack carried speed=3 and the
        # barrier rejected them all, so the resync never left 'holding'
        # (2026-09-16). A junk speed is still refused.
        for speed in (0, 1, 2, 3, 4):
            self.setUp()
            self.ack('client', speed=speed)
            self.ack('host', speed=speed)
            self.assertEqual(self.op.phase, 'saving', speed)
            self.assertEqual(self.op.resume_speed, speed)
        self.setUp()
        self.ack('client', speed=7)
        self.ack('host', speed='fast')
        self.assertEqual(self.op.phase, 'holding')

    def test_duplicate_and_stale_messages_cannot_advance(self):
        first = self.ack('host')
        for _ in range(10):
            self.op.acknowledge('host', first)
        self.assertEqual(self.op.phase, 'holding')
        self.ack('client')
        self.assertFalse(self.op.acknowledge('client', first))
        self.assertEqual(self.op.phase, 'saving')

    def test_lost_ack_times_out_without_release(self):
        self.ack('host')
        self.now = 46
        self.op.tick(['host', 'client'])
        self.assertEqual(self.op.phase, 'error')
        self.assertEqual(self.op.error['step'], 'holding')

    def test_corrupt_file_sets_rejected(self):
        for files in (FILES[:1], FILES + FILES[:1], [{'suffix': '.sav', 'size': 0, 'sha256': 'z'*64}]):
            with self.assertRaises(ValueError):
                snapshot_digest(files)
        self.transfer()
        self.ack('client', digest='b'*64)
        self.ack('host')
        self.assertEqual(self.op.phase, 'transferring')

    def test_missing_player_fails_in_every_phase(self):
        for phase in ('holding', 'saving', 'transferring', 'loading', 'checking', 'releasing'):
            self.op.phase = phase
            self.op.tick(['host'])
            self.assertEqual(self.op.phase, 'error')
            self.assertEqual(self.op.error['step'], phase)

    def test_retry_reuses_only_complete_snapshot_and_changes_epoch(self):
        self.both()
        old = self.op.epoch
        self.op.fail('save failed')
        self.op.retry('client', self.op.operation, self.op.members)
        self.assertNotEqual(old, self.op.epoch)
        self.both()
        self.assertEqual(self.op.phase, 'saving')
        self.ack('host')
        snapshot = self.op.snapshot
        self.op.fail('transfer failed')
        self.op.retry('host', self.op.operation, self.op.members)
        self.both()
        self.assertEqual(self.op.phase, 'transferring')
        self.assertEqual(self.op.snapshot, snapshot)

    def test_world_difference_never_auto_retries(self):
        self.transfer()
        self.both()
        self.both()
        self.ack('host')
        self.ack('client', fingerprint='other-world')
        self.assertEqual(self.op.phase, 'error')
        self.now = 10000
        self.op.tick(self.op.members)
        self.assertEqual(self.op.phase, 'error')

    def test_failures_and_abort_in_every_phase(self):
        for phase in ('holding', 'saving', 'transferring', 'loading', 'checking', 'releasing'):
            self.op.phase = phase
            self.ack('client', success=False, detail='engine failure')
            self.assertEqual(self.op.phase, 'error')
            self.assertEqual(self.op.error['step'], phase)
            self.assertTrue(self.op.abort('client', self.op.operation))
            self.assertEqual(self.op.phase, 'aborted')

    def test_unpaused_and_previous_epoch_cannot_pass_check(self):
        self.transfer()
        self.both()
        self.both()
        self.ack('host')
        self.ack('client', paused=False)
        self.ack('client', epoch='old-world')
        self.assertEqual(self.op.phase, 'checking')

    def test_snapshot_views_cannot_mutate_prepared_snapshot(self):
        self.transfer()
        view = self.op.view()
        view['snapshot']['files'][0]['size'] = 1
        self.assertEqual(self.op.snapshot['files'][0]['size'], 100)

    def test_hidden_progress_continues_without_reopening(self):
        replica = SyncReplica('client', 'host')
        first = self.op.view()
        self.assertTrue(replica.receive('host', first))
        replica.hide()
        self.both()
        self.assertTrue(replica.receive('host', self.op.view()))
        self.assertFalse(replica.visible)
        self.assertFalse(replica.receive('host', first))
        self.assertFalse(replica.receive('stranger', self.op.view()))
        replica.show()
        self.assertTrue(replica.visible)

    def test_replica_duplicate_does_not_erase_acknowledgement(self):
        replica = SyncReplica('client', 'host')
        view = self.op.view()
        replica.receive('host', view)
        ack = replica.acknowledge(success=True, paused=True, drained=True, speed=0)
        self.assertFalse(replica.receive('host', view))
        self.assertEqual(replica.ack, ack)


if __name__ == '__main__':
    unittest.main()
