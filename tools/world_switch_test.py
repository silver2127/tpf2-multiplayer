"""A host that loads another world mid-session pushes it to every client.

Real ``lobby.run_host`` / ``lobby.run_client`` over loopback UDP, real save
transfers, no game. What a WORLD SWITCH has to do, and what this checks:

  a) a ``start`` carrying ``switch`` after the session started puts every peer
     back to unstarted, pushes the new save to ALL of them (a plain start would
     push to nobody -- they all have a save already) and broadcasts a start
     flagged as a switch;
  b) a client takes that start although it had started, emits
     ``{"type":"start","save":true,"switch":true}`` for its menu and tells the
     roster it is loading the host's new world;
  c) a switch that arrives while a transfer is running is queued and is still a
     switch when it runs;
  d) a plain start after a started session is unchanged: it serves the peers
     that are waiting for a first save, and nobody else is started again.

    python tools/world_switch_test.py
"""
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import lobby


def make_world(directory, name, filler):
    """A .sav with a .sav.lua beside it that names lockstep.lua -- what the
    host's mod check looks for before it shares anything."""
    save = directory / (name + '.sav')
    save.write_bytes(filler * 40000)
    save.with_suffix('.sav.lua').write_text('{["lockstep.lua"] = {}}\n', encoding='utf-8')
    return str(save)


def events(io, kind):
    return [e for e in lobby._read_events(io.out_path) if e.get('type') == kind]


fails = []


def check(name, condition, extra=''):
    print(('ok   ' if condition else 'FAIL ') + name + (('  (%s)' % extra) if extra else ''))
    if not condition:
        fails.append(name)


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    saves = root / 'saves'
    saves.mkdir()
    worlds = {n: make_world(saves, n, bytes([0x40 + i]))
              for i, n in enumerate(('world1', 'world2', 'world3', 'world4', 'world5'))}
    contents = {n: (saves / (n + '.sav')).read_bytes() for n in worlds}

    names = ('host', 'client1', 'client2', 'client3')
    ios = {name: lobby.LobbyIO(str(root / name)) for name in names}
    host_log = []
    stop = threading.Event()
    sock = lobby.open_socket(0, socket.AF_INET)
    workers = [threading.Thread(target=lobby.run_host, args=(sock, 'host', ios['host']),
                                kwargs=dict(stop=stop, log=host_log.append))]
    connections = []
    workers[0].start()

    def command(who, **fields):
        with open(ios[who].in_path, 'a', encoding='utf-8') as stream:
            stream.write(json.dumps(fields) + '\n')

    def commands(who, *rows):
        """Several commands in ONE write: the host's loop reads them all in the
        same pass, so the second is guaranteed to meet the first's transfer."""
        with open(ios[who].in_path, 'a', encoding='utf-8') as stream:
            stream.write(''.join(json.dumps(row) + '\n' for row in rows))

    def wait_for(predicate, what):
        ok = lobby._wait_until(predicate, timeout=30)
        if not ok:
            print('   timed out waiting for ' + what)
        return ok

    def join(name):
        conn = lobby._dial_loopback(0, sock.getsockname()[1], 5)
        assert conn is not None
        connections.append(conn)
        worker = threading.Thread(target=lobby.run_client, args=(conn, name, ios[name]),
                                  kwargs=dict(stop=stop, log=lambda _: None))
        worker.start()
        workers.append(worker)

    def starts(name):
        return events(ios[name], 'start')

    def incoming(name):
        path = Path(ios[name].dir) / (lobby.INCOMING_BASENAME + '.sav')
        return path.read_bytes() if path.is_file() else b''

    def host_said(fragment):
        return any(fragment in line for line in host_log)

    try:
        join('client1')
        join('client2')
        assert wait_for(lambda: len((lobby._latest_roster(ios['host'].out_path) or {}).get('players', [])) == 3,
                        'the roster of three'), 'lobby never formed'

        # -- the ordinary start everyone knows ------------------------------- #
        command('host', cmd='start', save=worlds['world1'])
        assert wait_for(lambda: all(starts(n) for n in ('client1', 'client2')), 'the first start')
        check('a plain start is not a switch',
              all(e.get('save') is True and 'switch' not in e for n in ('client1', 'client2') for e in starts(n)))
        check('everyone loaded the first world',
              all(incoming(n) == contents['world1'] for n in ('client1', 'client2')))

        # -- a) the host loads another world while everyone is playing ------- #
        command('host', cmd='start', save=worlds['world2'], switch=True)
        assert wait_for(lambda: all(len(starts(n)) >= 2 for n in ('client1', 'client2')), 'the switch start')
        check('the host unstarted every peer and pushed the new world to all of them',
              host_said('world switch: pushing world2.sav to 2 player(s) (2 of them already playing)'),
              next((l for l in host_log if 'world switch' in l), 'no world-switch line'))
        check('the save reached every client although they had all started',
              all(incoming(n) == contents['world2'] for n in ('client1', 'client2')))
        check('the host broadcast the start as a switch', host_said('START broadcast (save=True, world switch)'))
        check('the host menu is told its own start is a switch',
              any(e.get('switch') is True for e in events(ios['host'], 'start')))

        # -- b) the client takes it although it had started ------------------ #
        check('every client emitted a switch start for its menu',
              all(starts(n)[-1].get('switch') is True and starts(n)[-1].get('save') is True
                  for n in ('client1', 'client2')))
        check('the roster is told what the clients are doing',
              wait_for(lambda: any('loading the host\'s new world' in (lobby._latest_roster(ios['host'].out_path)
                                                                      or {}).get('stages', {}).values()
                                   for _ in (0,)), 'the loading stage'))

        # -- c) a switch that meets a running transfer ----------------------- #
        before = {n: len(starts(n)) for n in ('client1', 'client2')}
        commands('host',
                 dict(cmd='start', save=worlds['world3'], switch=True),
                 dict(cmd='start', save=worlds['world4'], switch=True))
        check('the second switch waited for the running transfer',
              wait_for(lambda: host_said('start queued until the running save transfer ends (world switch)'),
                       'the queued switch'))
        assert wait_for(lambda: all(len(starts(n)) >= before[n] + 2 for n in ('client1', 'client2')),
                        'both queued switches')
        check('a queued switch is still a switch',
              all(e.get('switch') is True for n in ('client1', 'client2') for e in starts(n)[before[n]:]))
        check('every client ended in the last world the host loaded',
              wait_for(lambda: all(incoming(n) == contents['world4'] for n in ('client1', 'client2')),
                       'world4 everywhere'),
              {n: incoming(n)[:1] for n in ('client1', 'client2')})

        # -- d) a plain start after a started session is unchanged ----------- #
        settled = {n: len(starts(n)) for n in ('client1', 'client2')}
        join('client3')
        assert wait_for(lambda: len((lobby._latest_roster(ios['host'].out_path) or {}).get('players', [])) == 4,
                        'the roster of four')
        command('host', cmd='start', save=worlds['world5'])
        assert wait_for(lambda: bool(starts('client3')), 'the newcomer start')
        check('the newcomer gets an ordinary start, not a switch',
              all('switch' not in e for e in starts('client3')))
        check('a plain start does not restart the players who already have a world',
              all(len(starts(n)) == settled[n] for n in ('client1', 'client2')),
              {n: len(starts(n)) for n in ('client1', 'client2')})
        check('the players who already have a world keep it',
              all(incoming(n) == contents['world4'] for n in ('client1', 'client2')))

    finally:
        stop.set()
        for worker in workers:
            worker.join(10)
        for connection in connections:
            connection.close()
        try:
            sock.close()
        except OSError:
            pass
        assert all(not worker.is_alive() for worker in workers)

if fails:
    print('FAILED: ' + ', '.join(fails))
    sys.exit(1)
print('PASS: a mid-session world switch unstarts every peer, pushes the new save to all of them, '
      'is taken by clients that had already started, survives a running transfer, and leaves the '
      'plain hot-join start alone')
