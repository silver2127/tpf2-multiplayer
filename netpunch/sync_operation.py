"""Host-authoritative start/recovery barrier, independent of transport and engine.

The owner repeats ``view()`` over its authenticated lobby links. Side effects are
performed only on phase transitions; duplicate acknowledgements are harmless.
An acknowledgement always names the operation, revision, phase and world epoch.
"""
import hashlib
import copy
import json
import secrets
import time


PHASES = ('holding', 'waiting', 'saving', 'transferring', 'loading', 'checking',
          'releasing', 'complete', 'error', 'aborted')
ACTIVE = frozenset(PHASES[:-3])
# Phases whose length does not depend on the world: a fixed wait from entry.
TIMEOUT = {'holding': 45, 'checking': 60, 'releasing': 30}
# Phases whose length DOES depend on the world -- the engine writing the save,
# the bytes crossing the wire, every engine loading it -- have no total limit.
# A 1 GB save to a slow uplink is however long it is. What ends them early is
# SILENCE: no member reported any progress for this long (the same numbers
# that were the total limits until 2026-09-16, now measured from the last
# progress report, not from entry). Progress is anything that advances, by a
# member that has not finished its part: bytes acknowledged by a receiver
# (and its verify/write work once it has them all), the engine's own work --
# CPU time of the thread saving or loading, bytes through the disk -- a save
# file growing, a member's control stage changing (see SyncParticipant.progress,
# sync_runtime.engine_work, HostRecovery.tick, ClientRecovery.tick). Never a
# heartbeat that ticks regardless of the engine, and never a member that has
# already acknowledged the phase: what that member's engine does afterwards
# (rendering, idling) says nothing about the members still working.
SILENCE = {'saving': 120, 'transferring': 300, 'loading': 300}


def snapshot_digest(files):
    """Bind all required files, including metadata, to one immutable snapshot."""
    if not isinstance(files, list) or not 2 <= len(files) <= 3:
        raise ValueError('incomplete snapshot')
    suffixes = set()
    canonical = []
    for item in files:
        suffix, size, digest = item.get('suffix'), item.get('size'), item.get('sha256')
        if suffix not in ('.sav', '.sav.lua', '.jpg') or suffix in suffixes:
            raise ValueError('invalid snapshot file set')
        if type(size) is not int or size <= 0 or not isinstance(digest, str) or len(digest) != 64:
            raise ValueError('invalid snapshot file metadata')
        if any(c not in '0123456789abcdef' for c in digest):
            raise ValueError('invalid checksum')
        suffixes.add(suffix)
        canonical.append({'suffix': suffix, 'size': size, 'sha256': digest})
    if not {'.sav', '.sav.lua'} <= suffixes:
        raise ValueError('save or metadata missing')
    encoded = json.dumps(sorted(canonical, key=lambda x: x['suffix']), sort_keys=True,
                         separators=(',', ':')).encode()
    return hashlib.sha256(encoded).hexdigest()


class SyncOperation:
    def __init__(self, host, clock=time.monotonic, token=None):
        self.host = host
        self.clock = clock
        self.token = token or (lambda: secrets.token_hex(16))
        self.operation = None
        self.phase = 'aborted'
        self.revision = 0
        self.epoch = None
        self.members = ()
        self.acks = {}
        self.snapshot = None
        self.mode = None
        self.resume_speed = None
        self.error = None
        self.deadline = None
        self.progress_seen = {}     # member -> its last progress token in this phase
        self.effects = []
        self.confirmed = True
        # THE ROSTER MAY CHANGE UNDER AN OPERATION (2026-09-16). A join is a
        # frozen sync point now: the whole session holds while everyone, the
        # host included, loads one save, so a player who arrives while a round
        # is running must be admitted, never refused -- and NOBODY is released
        # until every member is in. A newcomer during holding/waiting simply
        # joins the phase (it has to acknowledge it like everyone else); one
        # arriving later is `pending`: when the round would complete, the same
        # snapshot goes round again under a fresh epoch with the newcomer as a
        # member. A client that leaves is dropped and the rest carry on; only
        # the host's departure fails the operation.
        self.pending = ()

    def _enter(self, phase):
        self.phase = phase
        self.revision += 1
        self.acks = {}
        self.progress_seen = {}
        if phase in TIMEOUT:
            self.deadline = self.clock() + TIMEOUT[phase]
        elif phase in SILENCE:
            self.deadline = self.clock() + SILENCE[phase]
        else:
            self.deadline = None
        self.effects.append(self.view())

    def progress(self, sender, message):
        """A member reports that its part of the current phase is advancing.

        ``message`` names the operation, revision, epoch and phase like an
        acknowledgement and carries a ``progress`` token; a token that differs
        from the member's previous one moves the silence deadline out again. A
        repeated token is not progress, and neither is anything from a member
        that has already acknowledged this phase: its part is done, so nothing
        it reports can stand for the members still working (a host rendering
        away after its own quick install kept a hung joiner's load 'alive').
        Returns True when the deadline moved."""
        if self.phase not in SILENCE or sender not in self.members or sender in self.acks:
            return False
        if not isinstance(message, dict):
            return False
        if any(message.get(k) != getattr(self, k) for k in ('operation', 'revision', 'epoch', 'phase')):
            return False
        token = message.get('progress')
        if not isinstance(token, str) or not token:
            return False
        if self.progress_seen.get(sender) == token:
            return False
        self.progress_seen[sender] = token
        self.deadline = self.clock() + SILENCE[self.phase]
        return True

    def view(self):
        return {'operation': self.operation, 'revision': self.revision,
                'epoch': self.epoch, 'phase': self.phase, 'mode': self.mode,
                'members': list(self.members), 'host': self.host,
                'pending': list(self.pending),
                'snapshot': copy.deepcopy(self.snapshot), 'resume_speed': self.resume_speed,
                'error': copy.deepcopy(self.error)}

    def take_effects(self):
        result, self.effects = self.effects, []
        return result

    def request(self, sender, members, mode, confirmed=True):
        members = tuple(sorted(set(members)))
        if sender not in members or self.host not in members or len(members) < 2:
            return False
        # 'join': a player arrived in a running session. The same round as a
        # resync (the host saves, everyone loads), started by the host lobby
        # itself, no button and no readiness dance.
        if mode not in ('start', 'resync', 'join') or (mode in ('start', 'join') and sender != self.host):
            return False
        if self.phase in ACTIVE or self.phase == 'error':
            # A second player requesting recovery joins the current operation.
            if confirmed and self.phase != 'error':
                self.confirmed = True
                if self.phase == 'waiting':
                    self._enter('transferring' if self.snapshot else 'saving')
            return self.view()
        self.operation = self.token()
        self.epoch = self.token()
        self.members, self.mode = members, mode
        self.pending = ()
        self.confirmed = confirmed
        self.snapshot = self.resume_speed = self.error = None
        self._enter('holding')
        return self.view()

    def fail(self, detail):
        if self.phase not in ACTIVE:
            return False
        self.error = {'step': self.phase, 'detail': str(detail)[:400]}
        self._enter('error')
        return True

    def _admit(self, members):
        """The lobby roster against the operation's members: a departed client
        is dropped (its part of the phase is no longer awaited), a newcomer is
        admitted -- into the phase itself while nothing world-bound has begun
        (holding/waiting), else as pending for one more round. Returns False
        when the host is gone, which is the one change that ends a round."""
        roster = set(members)
        if self.host not in roster:
            return False
        gone = [m for m in self.members if m not in roster]
        for m in gone:
            self.acks.pop(m, None)
            self.progress_seen.pop(m, None)
        if gone:
            self.members = tuple(m for m in self.members if m in roster)
        self.pending = tuple(m for m in self.pending if m in roster)
        new = sorted(m for m in roster if m not in self.members and m not in self.pending)
        if new:
            if self.phase in ('holding', 'waiting'):
                self.members = tuple(sorted(set(self.members) | set(new)))
                if self.phase == 'holding':
                    self.deadline = self.clock() + TIMEOUT['holding']   # the newcomer's own hold
            else:
                self.pending = tuple(sorted(set(self.pending) | set(new)))
        return True

    def tick(self, members):
        if self.phase not in ACTIVE:
            return
        if not self._admit(members):
            self.fail('Host disconnected')
        elif self._advance():
            return                        # a departed client was the last one awaited
        elif self.deadline is not None and self.clock() >= self.deadline:
            if self.phase in SILENCE:
                self.fail(f'No progress for {SILENCE[self.phase]} s while {self.phase}')
            else:
                self.fail('Timed out waiting for all players')

    def retry(self, sender, operation, members):
        if sender not in self.members or operation != self.operation or self.phase != 'error':
            return False
        members = tuple(sorted(set(members)))
        if self.host not in members or len(members) < 2:
            return False
        # whoever is on the roster now is the round: a member that left is
        # not awaited, one that arrived meanwhile is in
        self.members, self.pending = members, ()
        self.epoch = self.token()
        self.error = None
        self.confirmed = True
        # Always reacquire a world hold. Reuse only a completed immutable snapshot.
        self._enter('holding')
        return True

    def abort(self, sender, operation):
        if sender not in self.members or operation != self.operation or self.phase not in ACTIVE | {'error'}:
            return False
        self._enter('aborted')
        return True

    def acknowledge(self, sender, message):
        if sender not in self.members or self.phase not in ACTIVE:
            return False
        if self.phase == 'waiting':
            return False
        if any(message.get(k) != getattr(self, k) for k in ('operation', 'revision', 'epoch', 'phase')):
            return False
        if sender in self.acks:
            return self.acks[sender] == message
        if message.get('success') is False:
            self.fail(message.get('detail', 'Player reported a failure'))
            return True
        if message.get('success') is not True:
            return False
        if self.phase == 'holding':
            if message.get('paused') is not True or message.get('drained') is not True:
                return False
            speed = message.get('speed')
            # the engine's speed index, 0 (paused) to 4 -- 3 exists: a session ran
            # at it, every holding ack carried speed=3, and the barrier silently
            # rejected them all, so the resync never left 'holding' (2026-09-16)
            if type(speed) not in (int, float) or speed not in (0, 1, 2, 3, 4):
                return False
            if sender == self.host and self.resume_speed is None:
                self.resume_speed = speed
        elif self.phase == 'saving':
            if sender != self.host:
                return False
            try:
                files = message['files']
                digest = snapshot_digest(files)
            except (ValueError, TypeError, KeyError, AttributeError):
                self.fail('Host snapshot is incomplete')
                return True
            self.snapshot = {'files': copy.deepcopy(files), 'digest': digest}
            self._enter('transferring')
            return True
        else:
            if not self.snapshot or message.get('digest') != self.snapshot['digest']:
                return False
            if self.phase in ('loading', 'checking', 'releasing') and message.get('paused') is not True:
                return False
            if self.phase == 'checking':
                fingerprint = message.get('fingerprint')
                if not isinstance(fingerprint, str) or not 1 <= len(fingerprint) <= 8192:
                    return False
        self.acks[sender] = dict(message)
        self._advance()
        return True

    def _advance(self):
        """Every member has acknowledged the phase: the next one. Returns True
        when the phase moved."""
        if self.phase not in ACTIVE or self.phase == 'waiting' or not self.members:
            return False
        if set(self.acks) != set(self.members):
            return False
        if self.phase == 'holding':
            self._enter(('transferring' if self.snapshot else 'saving') if self.confirmed else 'waiting')
        elif self.phase == 'saving':
            return False                  # the host's snapshot ack enters transferring itself
        elif self.phase == 'transferring':
            self._enter('loading')
        elif self.phase == 'loading':
            self._enter('checking')
        elif self.phase == 'checking':
            if len({a['fingerprint'] for a in self.acks.values()}) != 1:
                self.fail('World comparison differs after loading')
            else:
                self._enter('releasing')
        elif self.phase == 'releasing':
            if self.pending:
                # somebody arrived during this round: nobody is released until
                # they are in. The snapshot is the world every member holds, so
                # it goes round once more under a fresh epoch, newcomers included.
                self.members = tuple(sorted(set(self.members) | set(self.pending)))
                self.pending = ()
                self.epoch = self.token()
                self.confirmed = True
                self._enter('holding')
            else:
                self._enter('complete')
        return True


class SyncReplica:
    """Remember the newest host instruction and repeat its local acknowledgement.

    Visibility is deliberately independent: closing a progress window has no
    network/engine side effect. The owner sends abort only for Return to Lobby.
    """
    def __init__(self, player, host):
        self.player, self.host = player, host
        self.current = None
        self.ack = None
        self.visible = False

    def receive(self, sender, state):
        if sender != self.host or not isinstance(state, dict):
            return False
        if state.get('host') != self.host or self.player not in state.get('members', ()):
            return False
        revision = state.get('revision')
        if type(revision) is not int or revision < 1 or state.get('phase') not in PHASES:
            return False
        if not isinstance(state.get('operation'), str) or not isinstance(state.get('epoch'), str):
            return False
        if self.current and revision <= self.current['revision']:
            return False
        new_operation = not self.current or self.current['operation'] != state['operation']
        self.current = copy.deepcopy(state)
        self.ack = None
        if new_operation:
            self.visible = True
        return True

    def acknowledge(self, **fields):
        if not self.current or self.current['phase'] not in ACTIVE:
            return None
        result = dict(fields)
        result.update({key: self.current[key] for key in ('operation', 'revision', 'phase', 'epoch')})
        self.ack = copy.deepcopy(result)
        return result

    def hide(self):
        self.visible = False

    def show(self):
        if self.current:
            self.visible = True
