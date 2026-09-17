"""Barrier model tests; these do not simulate successful real-engine I/O."""
import itertools
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
from sync_operation import SyncOperation, SyncReplica, snapshot_digest, SILENCE

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

    def progress(self, sender, token):
        data = {k: getattr(self.op, k) for k in ('operation', 'revision', 'epoch', 'phase')}
        data['progress'] = token
        return self.op.progress(sender, data)

    def test_transfer_lasts_as_long_as_bytes_keep_moving(self):
        # A 1 GB save to a slow uplink used to hit the 300 s total limit and
        # leave everyone held (2026-09-16). The phase now ends only after
        # SILENCE seconds without progress, however long the whole takes.
        self.transfer()
        for step in range(20):                          # 20 x 250 s = 83 min of transfer
            self.now += 250
            self.assertTrue(self.progress('client', f'recv={step}'))
            self.op.tick(self.op.members)
            self.assertEqual(self.op.phase, 'transferring', step)
        self.now += SILENCE['transferring'] - 1
        self.op.tick(self.op.members)
        self.assertEqual(self.op.phase, 'transferring')
        self.now += 1
        self.op.tick(self.op.members)
        self.assertEqual(self.op.phase, 'error')
        self.assertEqual(self.op.error['step'], 'transferring')
        self.assertIn('No progress for 300 s', self.op.error['detail'])

    def test_repeated_token_is_not_progress(self):
        self.transfer()
        self.assertTrue(self.progress('client', 'recv=5'))
        self.now += 200
        self.assertFalse(self.progress('client', 'recv=5'))   # same token: nothing advanced
        self.now += 100
        self.op.tick(self.op.members)
        self.assertEqual(self.op.phase, 'error')

    def test_progress_needs_the_current_phase_and_a_member(self):
        self.transfer()
        self.assertFalse(self.progress('stranger', 'x'))
        data = {k: getattr(self.op, k) for k in ('operation', 'revision', 'epoch', 'phase')}
        self.assertFalse(self.op.progress('client', dict(data, phase='loading', progress='x')))
        self.assertFalse(self.op.progress('client', dict(data, revision=data['revision'] - 1, progress='x')))
        self.assertFalse(self.op.progress('client', dict(data, progress='')))
        self.assertFalse(self.op.progress('client', dict(data, progress=7)))
        self.assertFalse(self.op.progress('client', 'not a dict'))
        self.op.phase = 'holding'                      # a fixed-wait phase never extends
        self.assertFalse(self.progress('client', 'x'))

    def test_saving_and_loading_extend_on_engine_work(self):
        self.both()                                     # -> saving
        self.assertEqual(self.op.phase, 'saving')
        for beat in range(10):
            self.now += 100
            self.assertTrue(self.progress('host', f'saving:engine:cpu_command={beat}'))
            self.op.tick(self.op.members)
            self.assertEqual(self.op.phase, 'saving')
        self.ack('host')                                # -> transferring
        self.both()                                     # -> loading
        self.assertEqual(self.op.phase, 'loading')
        for beat in range(10):
            self.now += 250
            self.assertTrue(self.progress('client', f'loading:engine:cpu_ui={beat}'))
            self.op.tick(self.op.members)
            self.assertEqual(self.op.phase, 'loading')
        self.now += 300
        self.op.tick(self.op.members)
        self.assertEqual(self.op.error['step'], 'loading')

    def test_an_acknowledged_member_cannot_stand_for_the_others(self):
        # The host's world is ready and acknowledged; the client's engine is
        # hung mid-load. Whatever the host's engine reports afterwards
        # (rendering, idling) is not the client's progress: the phase fails
        # after SILENCE, it does not stay held for ever.
        self.transfer()
        self.both()                                     # -> loading
        self.assertEqual(self.op.phase, 'loading')
        self.assertTrue(self.progress('client', 'loading:engine:cpu_ui=1'))
        self.assertTrue(self.progress('host', 'loading:engine:cpu_ui=1'))
        self.ack('host')                                # the host's part is done
        self.assertEqual(self.op.phase, 'loading')
        self.now += 100
        self.assertFalse(self.progress('host', 'loading:engine:cpu_ui=2'))
        self.assertFalse(self.progress('host', 'loading:engine:cpu_ui=3'))
        self.now += SILENCE['loading'] - 100
        self.op.tick(self.op.members)
        self.assertEqual(self.op.phase, 'error')
        self.assertIn('No progress for 300 s', self.op.error['detail'])

    def test_progress_tokens_reset_on_every_phase(self):
        self.transfer()
        self.assertTrue(self.progress('client', 'same'))
        self.both()                                     # -> loading
        self.assertTrue(self.progress('client', 'same'))   # a fresh phase: the token counts again

    def test_corrupt_file_sets_rejected(self):
        for files in (FILES[:1], FILES + FILES[:1], [{'suffix': '.sav', 'size': 0, 'sha256': 'z'*64}]):
            with self.assertRaises(ValueError):
                snapshot_digest(files)
        self.transfer()
        self.ack('client', digest='b'*64)
        self.ack('host')
        self.assertEqual(self.op.phase, 'transferring')

    def test_missing_host_fails_in_every_phase(self):
        for phase in ('holding', 'saving', 'transferring', 'loading', 'checking', 'releasing'):
            self.op.phase = phase
            self.op.tick(['client'])
            self.assertEqual(self.op.phase, 'error')
            self.assertEqual(self.op.error['step'], phase)

    def test_departed_client_is_dropped_and_the_rest_carry_on(self):
        # three members, the third leaves while loading: the round is not lost --
        # it continues with the two that remain, and if the leaver was the last
        # one awaited the phase moves on the tick that drops it
        self.op.abort('host', self.op.operation)
        self.op.request('host', ['host', 'client', 'third'], 'resync')
        self.ack('host'); self.ack('client'); self.ack('third')
        self.ack('host')
        self.assertEqual(self.op.phase, 'transferring')
        self.ack('host'); self.ack('client'); self.ack('third')
        self.assertEqual(self.op.phase, 'loading')
        self.ack('host'); self.ack('client')
        self.assertEqual(self.op.phase, 'loading')             # third still loading
        self.op.tick(['host', 'client'])                        # third quit
        self.assertEqual(self.op.phase, 'checking')             # it was the last one awaited
        self.assertEqual(self.op.members, ('client', 'host'))
        self.assertNotIn('third', self.op.acks)
        self.both(); self.both()
        self.assertEqual(self.op.phase, 'complete')

    def test_newcomer_during_holding_joins_the_phase(self):
        # a player arriving while everyone is still pausing is simply one more
        # member to hear from before the save is taken
        self.ack('client')
        self.op.tick(['host', 'client', 'late'])
        self.assertEqual(self.op.members, ('client', 'host', 'late'))
        self.assertEqual(self.op.phase, 'holding')
        self.ack('host')
        self.assertEqual(self.op.phase, 'holding')             # late has not paused yet
        self.ack('late')
        self.assertEqual(self.op.phase, 'saving')

    def test_newcomer_during_a_round_gets_one_more_round_before_anyone_is_released(self):
        # a player arriving once the world is being moved is pending: the round
        # finishes for the members it had, and instead of releasing them the
        # same snapshot goes round again, newcomer included, under a new epoch
        self.transfer()
        self.op.tick(['host', 'client', 'late'])
        self.assertEqual(self.op.members, ('client', 'host'))
        self.assertEqual(self.op.pending, ('late',))
        self.assertEqual(self.op.view()['pending'], ['late'])
        epoch, snapshot = self.op.epoch, self.op.snapshot
        self.both()                                             # transferring -> loading
        self.both()                                             # loading -> checking
        self.both()                                             # checking -> releasing
        self.assertEqual(self.op.phase, 'releasing')
        self.both()
        self.assertEqual(self.op.phase, 'holding')             # not complete: late is in now
        self.assertEqual(self.op.members, ('client', 'host', 'late'))
        self.assertEqual(self.op.pending, ())
        self.assertNotEqual(self.op.epoch, epoch)
        self.assertEqual(self.op.snapshot, snapshot)            # the world everyone holds
        self.ack('host'); self.ack('client'); self.ack('late')
        self.assertEqual(self.op.phase, 'transferring')         # no second save
        for _ in range(4):
            self.ack('host'); self.ack('client'); self.ack('late')
        self.assertEqual(self.op.phase, 'complete')

    def test_join_mode_is_the_hosts_and_a_pending_leaver_is_forgotten(self):
        self.op.abort('host', self.op.operation)
        self.assertFalse(self.op.request('client', ['host', 'client'], 'join'))
        self.assertTrue(self.op.request('host', ['host', 'client'], 'join'))
        self.assertEqual(self.op.mode, 'join')
        self.transfer()
        self.op.tick(['host', 'client', 'late'])
        self.assertEqual(self.op.pending, ('late',))
        self.op.tick(['host', 'client'])                        # late gave up before its round
        self.assertEqual(self.op.pending, ())
        for _ in range(4):
            self.both()
        self.assertEqual(self.op.phase, 'complete')

    def test_retry_takes_the_roster_as_it_is_now(self):
        self.op.abort('host', self.op.operation)
        self.op.request('host', ['host', 'client', 'third'], 'resync')
        self.assertEqual(self.op.members, ('client', 'host', 'third'))
        self.op.fail('save failed')
        self.assertTrue(self.op.retry('host', self.op.operation, ['host', 'client']))
        self.assertEqual(self.op.members, ('client', 'host'))
        self.assertFalse(self.op.retry('host', self.op.operation, ['host']))

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
