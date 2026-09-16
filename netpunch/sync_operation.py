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
TIMEOUT = {'holding': 45, 'saving': 120, 'transferring': 300,
           'loading': 300, 'checking': 60, 'releasing': 30}


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
        self.effects = []
        self.confirmed = True

    def _enter(self, phase):
        self.phase = phase
        self.revision += 1
        self.acks = {}
        self.deadline = self.clock() + TIMEOUT[phase] if phase in TIMEOUT else None
        self.effects.append(self.view())

    def view(self):
        return {'operation': self.operation, 'revision': self.revision,
                'epoch': self.epoch, 'phase': self.phase, 'mode': self.mode,
                'members': list(self.members), 'host': self.host,
                'snapshot': copy.deepcopy(self.snapshot), 'resume_speed': self.resume_speed,
                'error': copy.deepcopy(self.error)}

    def take_effects(self):
        result, self.effects = self.effects, []
        return result

    def request(self, sender, members, mode, confirmed=True):
        members = tuple(sorted(set(members)))
        if sender not in members or self.host not in members or len(members) < 2:
            return False
        if mode not in ('start', 'resync') or (mode == 'start' and sender != self.host):
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

    def tick(self, members):
        if self.phase not in ACTIVE:
            return
        if set(members) != set(self.members):
            self.fail('Player disconnected or roster changed')
        elif self.deadline is not None and self.clock() >= self.deadline:
            self.fail('Timed out waiting for all players')

    def retry(self, sender, operation, members):
        if sender not in self.members or operation != self.operation or self.phase != 'error':
            return False
        if set(members) != set(self.members):
            return False
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
        if set(self.acks) != set(self.members):
            return True
        if self.phase == 'holding':
            self._enter(('transferring' if self.snapshot else 'saving') if self.confirmed else 'waiting')
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
