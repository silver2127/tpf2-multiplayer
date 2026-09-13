"""Local participant for the host barrier. Native I/O remains in the UI thread.

The lobby owns transport and repeats acknowledgements returned by tick(). This
module never discovers saves or drives menus. Its file interface is also usable
by deterministic tests; those tests do not replace real engine acceptance tests.
"""
from pathlib import Path
import itertools
import os
import re
import time

from sync_snapshot import PreparedSnapshot


def read_fields(path):
    try:
        data = Path(path).read_text(encoding='utf-8')
    except (OSError, UnicodeError):
        return {}
    if len(data) > 16384 or not data.endswith('\n'):
        return {}
    return dict(line.split('=', 1) for line in data.splitlines() if '=' in line)


_temporary_counter = itertools.count()


def write_fields(path, fields):
    path = Path(path)
    data = ''.join(f'{key}={value}\n' for key, value in fields.items())
    if any('\n' in str(v) or '\r' in str(v) for v in fields.values()):
        raise ValueError('invalid control field')
    # One temporary per writer and call: a shared ".sync.tmp" let a second writer
    # (another lobby process on the same data folder) truncate or move this one's
    # file between write and replace, and the replace failed with ENOENT during a
    # real transfer (2026-09-14, "No such file or directory: ...sync.tmp").
    temporary = path.with_name(f'{path.name}.{os.getpid()}.{next(_temporary_counter)}.sync.tmp')
    # Match the native/Lua wire format on Windows as well: no CRLF translation.
    with temporary.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(data)
    # A Lua/engine reader can briefly deny atomic replacement on Windows.
    # Retry publication only; never repeat the native save/load side effect.
    for attempt in range(21):
        try:
            os.replace(temporary, path)
            break
        except OSError as exc:
            if getattr(exc, 'winerror', None) not in (5, 32, 33) or attempt == 20:
                raise
            time.sleep(0.01)


class SyncParticipant:
    def __init__(self, directory, save_directory, pid, player):
        self.directory, self.save_directory = Path(directory), Path(save_directory)
        self.pid, self.player = str(pid), player
        self.state = None
        self.snapshot = None
        self.selected_save = None
        self.old_world = None
        self.ack = None
        self.commands = {}
        self.finished = False

    def _read(self, name):
        data = read_fields(self.directory / name)
        return data if data.get('pid') == self.pid else {}

    def _write(self, name, fields):
        write_fields(self.directory / name, dict(fields, pid=self.pid))

    def accept(self, state):
        if self.player not in state.get('members', ()):
            return False
        if self.state and state['revision'] <= self.state['revision']:
            return False
        if not all(re.fullmatch('[0-9a-f]{32}', state.get(k, '')) for k in ('operation', 'epoch')):
            return False
        if not self.state or state['operation'] != self.state['operation']:
            self.snapshot = None
            self.old_world = None
        self.state = dict(state)
        self.commands, self.ack, self.finished = {}, None, False
        if state['phase'] == 'loading':
            self.old_world = self._read('tpf2_sync_lua_ack.txt').get('world')
        try:
            self._publish_lua()
        except (OSError, ValueError, RuntimeError) as exc:
            self._failure(exc)
        return True

    def _failure(self, exc):
        self.ack = {k: self.state[k] for k in ('operation', 'epoch', 'revision', 'phase')}
        self.ack.update(success=False, detail=str(exc)[:400])
        return self.ack

    def _publish_lua(self):
        fields = {k: self.state[k] for k in ('operation', 'epoch', 'revision', 'phase')}
        fields['digest'] = (self.state.get('snapshot') or {}).get('digest', '')
        fields['resume_speed'] = self.state.get('resume_speed') or 0
        error = self.state.get('error') or {}
        fields['step'] = str(error.get('step') or '')[:24]
        fields['detail'] = ' '.join(str(error.get('detail') or '').split())[:400]
        self._write('tpf2_sync_lua.txt', fields)

    def _lua(self):
        data = self._read('tpf2_sync_lua_ack.txt')
        if any(data.get(k) != str(self.state[k]) for k in ('operation', 'epoch', 'revision', 'phase')):
            return {}
        return data

    def _native(self, cmd, step, name=''):
        identity = f"{self.state['epoch']}:{self.state['revision']}:{cmd}"
        if cmd not in self.commands:
            self._write('tpf2_native_request.txt', dict(id=identity, cmd=cmd, name=name))
            self.commands[cmd] = False
            return False
        if self.commands[cmd]:
            return True
        result = self._read('tpf2_native_event.txt')
        if result.get('id') != identity:
            return False
        if result.get('success') == '0':
            raise RuntimeError(result.get('detail') or 'Native engine operation failed')
        if result.get('step') == step and result.get('success') == '1':
            self.commands[cmd] = True
        return self.commands[cmd]

    def _ack(self, **fields):
        self.ack = {k: self.state[k] for k in ('operation', 'epoch', 'revision', 'phase')}
        self.ack.update(success=True, **fields)
        return self.ack

    def receive_snapshot(self, blob):
        if not self.state or self.state['phase'] != 'transferring':
            return False
        metadata = self.state['snapshot']
        self.snapshot = PreparedSnapshot.receive(blob, metadata['files'], metadata['digest'])
        return True

    def tick(self):
        if not self.state or self.ack or self.finished:
            return self.ack
        try:
            return self._tick()
        except (OSError, ValueError, RuntimeError) as exc:
            return self._failure(exc)

    def _tick(self):
        phase = self.state['phase']
        status = self._read('tpf2_native_status.txt')
        if not status:
            return None  # host timeout covers unavailable engine control
        if status.get('supported') != '1':
            raise RuntimeError('Native save/load adapter does not support this game build')
        basename = 'mp_' + self.state['epoch'][:12]
        if phase == 'holding':
            if not self._native('hold', 'held'):
                return None
            if status.get('has_world') != '1':
                return self._ack(paused=True, drained=True, speed=1)
            lua = self._lua()
            if lua.get('held') != '1' or not lua.get('world'):
                return None
            if self._native('pause', 'paused'):
                return self._ack(paused=True, drained=True, speed=int(float(lua['speed'])))
        elif phase == 'saving' and self.player == self.state['host']:
            if self.state['mode'] == 'start':
                if not self.selected_save:
                    raise ValueError('Host has not selected a save')
                self.snapshot = PreparedSnapshot.read(self.selected_save)
            elif self._native('save', 'saved', basename):
                self.snapshot = PreparedSnapshot.read(self.save_directory / (basename + '.sav'))
            if self.snapshot:
                return self._ack(files=self.snapshot.files)
        elif phase == 'transferring' and self.snapshot:
            if self.snapshot.digest != self.state['snapshot']['digest']:
                raise ValueError('Snapshot does not match this operation')
            self.snapshot.install(self.save_directory, basename)
            return self._ack(digest=self.snapshot.digest)
        elif phase == 'loading':
            if not self.snapshot or self.snapshot.digest != self.state['snapshot']['digest']:
                raise ValueError('Verified local snapshot is missing')
            if 'epoch' not in self.commands:
                fields = self._read('tpf2_bridge_ctl.txt')
                if not fields.get('instance') or not fields.get('peer'):
                    raise ValueError('Live lobby bridge configuration is missing')
                self._write('tpf2_epoch_request.txt', dict(epoch=self.state['epoch']))
                self.commands['epoch'] = True
            ready = self._read('tpf2_epoch_ready.txt')
            if ready.get('epoch') != self.state['epoch']:
                return None
            if ready.get('ok') != '1':
                raise RuntimeError('Bridge could not clear the previous world')
            if not self._native('load', 'world_ready', basename):
                return None
            lua = self._lua()
            if not lua.get('world') or lua['world'] == self.old_world or lua.get('held') != '1':
                return None
            if self._native('pause', 'paused'):
                return self._ack(paused=True, digest=self.snapshot.digest)
        elif phase in ('checking', 'releasing'):
            lua = self._lua()
            if lua.get('paused') != '1':
                return None
            fields = dict(paused=True, digest=self.state['snapshot']['digest'])
            if phase == 'checking':
                if not lua.get('fingerprint'):
                    return None
                fields['fingerprint'] = lua['fingerprint']
            return self._ack(**fields)
        elif phase == 'complete':
            if self._native('release', 'held'):
                self.finished = True
        # waiting/error/aborted leave both the Lua hold and native gate intact.
        return None
