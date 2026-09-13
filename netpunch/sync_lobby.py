"""One-click recovery over the existing authenticated lobby and save transport.

The archived barrier/runtime own engine side effects. This adapter keeps recovery
transfers separate from ordinary start/hot-join transfers and repeats control
messages until acknowledged. Only a player-hosted lobby can coordinate recovery.
"""
import collections
import time

from sync_operation import SyncOperation, SyncReplica
from sync_runtime import SyncParticipant


def make_runtime(args):
    if not getattr(args, 'sync_runtime_dir', None):
        return None
    if not args.save_dir or not args.game_pid:
        raise ValueError('Recovery needs a save directory and owning game PID')
    return SyncParticipant(args.sync_runtime_dir, args.save_dir, args.game_pid, args.name)


class HostRecovery:
    def __init__(self, runtime, host, io, send, members, targets, transfer_factory, available=lambda: True):
        self.runtime, self.io, self.send = runtime, io, send
        self.members, self.targets, self.transfer_factory = members, targets, transfer_factory
        self.is_available = available
        self.barrier = SyncOperation(host)
        self.barrier.revision = int(time.time() * 1000)
        self.transfer = None
        self.transfer_epoch = None
        self.seen = collections.deque(maxlen=512)
        self.local_seen = runtime._read('tpf2_sync_request.txt').get('id')
        self.sent = self.available = 0

    @property
    def held(self):
        return self.barrier.operation is not None and self.barrier.phase != 'complete'

    def command(self, sender, message):
        kind = message.get('cmd', message.get('t'))
        if kind not in ('sync_request', 'sync_retry', 'sync_abort', 'sync_ack'):
            return False
        if sender not in self.members():
            return True
        if kind == 'sync_ack':
            self.barrier.acknowledge(sender, message)
            return True
        request = message.get('id')
        if not isinstance(request, str) or not 1 <= len(request) <= 128:
            return True
        self.send(sender, {'t': 'sync_command_ack', 'id': request})
        key = (sender, request)
        if key in self.seen:
            return True
        self.seen.append(key)
        if kind == 'sync_request':
            if self.is_available() or self.held:
                self.barrier.request(sender, self.members(), 'resync')
        elif kind == 'sync_retry':
            self.barrier.retry(sender, message.get('operation'), self.members())
        else:
            self.barrier.abort(sender, message.get('operation'))
        return True

    def feedback(self, address, message):
        if self.transfer is None or message.get('sid') != self.transfer.sid:
            return False
        handler = {'fbegin_ack': 'on_begin_ack', 'fack': 'on_fack', 'fdone': 'on_fdone'}.get(message.get('t'))
        if not handler:
            return False
        getattr(self.transfer, handler)(address, message)
        return True

    def tick(self, now):
        if (self.is_available() or self.held) and now - self.available >= 1:
            self.available = now
            try:
                self.runtime._write('tpf2_sync_available.txt', dict(protocol=4, wall=int(now)))
            except OSError:
                pass  # availability expires; an IPC sharing conflict must not kill the lobby
        request = self.runtime._read('tpf2_sync_request.txt')
        if request.get('id') and request['id'] != self.local_seen:
            self.local_seen = request['id']
            self.command(self.barrier.host, request)
        self.barrier.tick(self.members())
        self.barrier.take_effects()
        if not self.barrier.operation:
            return
        state = self.barrier.view()
        if self.runtime.accept(state):
            self.io.emit(dict(state, type='sync_state'))
        ack = self.runtime.tick()
        if ack:
            self.barrier.acknowledge(self.barrier.host, ack)
        if self.barrier.phase == 'transferring':
            try:
                if self.transfer_epoch != self.barrier.epoch:
                    blob, files = self.runtime.snapshot.transfer()
                    self.transfer = self.transfer_factory(int(self.barrier.epoch[:8], 16), blob, files, self.targets())
                    self.transfer.begin_msg.update(operation=self.barrier.operation, epoch=self.barrier.epoch)
                    self.transfer_epoch = self.barrier.epoch
                self.transfer.pump(now)
                if self.transfer.failed_names():
                    self.barrier.fail('Snapshot transfer failed')
            except (OSError, ValueError, RuntimeError, AttributeError) as error:
                self.barrier.fail(str(error))
        else:
            self.transfer = None
        if now - self.sent >= .25:
            self.sent = now
            for member in self.members():
                if member != self.barrier.host:
                    self.send(member, dict(self.barrier.view(), t='sync_state'))


class ClientRecovery:
    def __init__(self, runtime, io, send, receiver):
        self.runtime, self.io, self.send, self.receiver = runtime, io, send, receiver
        self.replica = None
        self.pending = {}
        self.local_seen = runtime._read('tpf2_sync_request.txt').get('id')
        self.received_epoch = None
        self.sent = self.available = 0
        self.supported = False

    @property
    def held(self):
        return self.runtime.state is not None and self.runtime.state['phase'] != 'complete'

    def identify(self, player, host, supported):
        self.supported = supported
        if not self.replica or (not self.held and (self.replica.player != player or self.replica.host != host)):
            self.runtime.player = player
            self.replica = SyncReplica(player, host)

    def command(self, message):
        kind = message.get('cmd', message.get('t'))
        if kind not in ('sync_request', 'sync_retry', 'sync_abort'):
            return False
        request = message.get('id')
        if self.supported and isinstance(request, str) and 1 <= len(request) <= 128 and len(self.pending) < 16:
            self.pending[request] = dict(message, t=kind)
        return True

    def message(self, message):
        kind = message.get('t')
        if kind == 'sync_command_ack':
            self.pending.pop(message.get('id'), None)
            return True
        if kind != 'sync_state':
            return False
        if self.replica and (self.supported or self.held) and self.replica.receive(self.replica.host, message):
            if self.runtime.accept(message):
                self.io.emit(dict(message, type='sync_state'))
        return True

    def begin(self, message):
        if 'operation' not in message:
            return False
        state = self.runtime.state
        if state and state['phase'] == 'transferring' and all(message.get(k) == state[k] for k in ('operation', 'epoch')):
            if message.get('sid') == int(state['epoch'][:8], 16):
                self.receiver.on_begin(message)
        return True  # stale recovery transfers must never become normal starts

    def tick(self, now):
        if self.supported and now - self.available >= 1:
            self.available = now
            try:
                self.runtime._write('tpf2_sync_available.txt', dict(protocol=4, wall=int(now)))
            except OSError:
                pass
        request = self.runtime._read('tpf2_sync_request.txt')
        if request.get('id') and request['id'] != self.local_seen:
            self.local_seen = request['id']
            self.command(request)
        state = self.runtime.state
        if state and state['phase'] == 'transferring' and self.receiver.complete and self.receiver.sid == int(state['epoch'][:8], 16):
            if self.received_epoch != state['epoch']:
                try:
                    self.runtime.receive_snapshot(self.receiver.buf)
                    self.received_epoch = state['epoch']
                except (ValueError, OSError) as error:
                    self.runtime._failure(error)
        ack = self.runtime.tick()
        if now - self.sent >= .25:
            self.sent = now
            for command in list(self.pending.values()):
                self.send(command)
            if ack:
                self.send(dict(ack, t='sync_ack'))
