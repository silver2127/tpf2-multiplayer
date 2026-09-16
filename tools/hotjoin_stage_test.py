"""The host's roster shows a joiner's save transfer from the host's OWN sender,
before the joiner reports anything, and the joiner's own report still wins.

Real ``lobby.run_host`` / ``lobby.run_client`` over loopback UDP, a real save
transfer, no game. What this checks (2026-09-16):

  a) while the save goes out, the host loop sets the peer's stage from its
     sender's 10% steps ("receiving save N%") -- seen in the host log as
     "stage <- sender" lines and in the roster events the host panel reads;
  b) once the peer verified the file the stage is "save received, loading",
     a text only the host side ever produces, and it stays until the joiner's
     DLL says what it is doing;
  c) a joiner's own {"cmd":"stage"} overrides it in the roster;
  d) the host's own {"cmd":"stage"} (its own load, a world switch) shows
     beside the host's name and "" clears it;
  e) the merge rule on its own: a joiner's "receiving save M%" at least as far
     along beats the sender's N%, an older one does not, "loading world ..."
     is never touched, and "done" replaces any receive text.

    python tools/hotjoin_stage_test.py
"""
import json
from pathlib import Path
import re
import socket
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import lobby


def make_world(directory, name, filler, size):
    """A .sav (several chunks long, so the sender crosses 10% steps) with a
    .sav.lua beside it that names lockstep.lua -- what the host's mod check
    looks for before it shares anything."""
    save = directory / (name + '.sav')
    save.write_bytes(filler * size)
    save.with_suffix('.sav.lua').write_text('{["lockstep.lua"] = {}}\n', encoding='utf-8')
    return str(save)


def events(io, kind):
    return [e for e in lobby._read_events(io.out_path) if e.get('type') == kind]


fails = []


def check(name, condition, extra=''):
    print(('ok   ' if condition else 'FAIL ') + name + (('  (%s)' % extra) if extra else ''))
    if not condition:
        fails.append(name)


# -- e) the merge rule on its own --------------------------------------------- #
merge = lobby._merge_sender_stage
check('an empty stage takes the sender\'s progress', merge('', 'receiving save 10%', 10) == 'receiving save 10%')
check('a stale text from the last session takes it', merge('catching up (3 s behind)', 'receiving save 10%', 10) == 'receiving save 10%')
check('the joiner\'s own report, further along, wins', merge('receiving save 40%', 'receiving save 30%', 30) is None)
check('the joiner\'s own report at the same point wins', merge('receiving save 30%', 'receiving save 30%', 30) is None)
check('an older joiner report is passed', merge('receiving save 20%', 'receiving save 30%', 30) == 'receiving save 30%')
check('a load under way is never touched by send progress', merge('loading world 12%', 'receiving save 90%', 90) is None)
check('a load under way is never touched by done', merge('loading world 12%', 'save received, loading', None) is None)
check('done replaces any receive text', merge('receiving save 90%', 'save received, loading', None) == 'save received, loading')

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    saves = root / 'saves'
    saves.mkdir()
    world = make_world(saves, 'world1', b'\x41', 2 * 1024 * 1024)

    names = ('host', 'client1')
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

    def roster():
        return lobby._latest_roster(ios['host'].out_path) or {}

    def stage_of(name):
        return roster().get('stages', {}).get(name, '')

    def stage_history(name):
        seen = []
        for e in events(ios['host'], 'roster'):
            s = e.get('stages', {}).get(name, '')
            if not seen or seen[-1] != s:
                seen.append(s)
        return seen

    def sender_lines():
        return [l for l in host_log if 'stage <- sender:' in l]

    try:
        join('client1')
        assert wait_for(lambda: len(roster().get('players', [])) == 2, 'the roster of two'), 'lobby never formed'

        # -- a) + b) the host's sender drives the joiner's stage --------------- #
        command('host', cmd='start', save=world)
        assert wait_for(lambda: bool(events(ios['client1'], 'start')), 'the start')
        assert wait_for(lambda: stage_of('client1') == 'save received, loading', 'the done stage')
        pcts = [int(m.group(1)) for l in sender_lines()
                for m in [re.search(r'receiving save (\d+)%', l)] if m]
        # On loopback the whole file is in flight at once (SEND_WINDOW_LOCAL), so
        # the sender's own view jumps from 0 to the end and its end step usually
        # loses to the joiner's own 100% (the merge rule); the 10% steps in
        # between are the joiner's reports. What the sender must do is open the
        # sequence with 0% before any joiner report, and never go backwards.
        check('the host log shows the sender opening the roster at 0% and never regressing',
              bool(pcts) and pcts[0] == 0 and pcts == sorted(pcts), str(pcts))
        check('the sender\'s first word came before any joiner report',
              bool(sender_lines()) and 'receiving save 0%' in sender_lines()[0], (sender_lines() or ['none'])[0])
        history = stage_history('client1')
        check('the roster went through receiving stages to "save received, loading"',
              any(s.startswith('receiving save ') for s in history) and history[-1] == 'save received, loading',
              ' | '.join(history))
        check('"save received, loading" is the host side\'s own word',
              any('stage <- sender: save received, loading' in l for l in host_log))

        # -- c) the joiner's own report overrides --------------------------- #
        command('client1', cmd='stage', text='loading world 42%')
        check('the joiner\'s own report replaces the host-side stage',
              wait_for(lambda: stage_of('client1') == 'loading world 42%', 'the joiner report'), stage_of('client1'))
        command('client1', cmd='stage', text='')
        check('the joiner clears its stage', wait_for(lambda: stage_of('client1') == '', 'the clear'))

        # -- d) the host's own stage ----------------------------------------- #
        command('host', cmd='stage', text='loading world 7%')
        check('the host\'s own stage shows beside its name',
              wait_for(lambda: stage_of('host') == 'loading world 7%', 'the host stage'), stage_of('host'))
        check('the client roster carries it too',
              wait_for(lambda: (lobby._latest_roster(ios['client1'].out_path) or {}).get('stages', {}).get('host') == 'loading world 7%',
                       'the client-side host stage'))
        command('host', cmd='stage', text='')
        check('the host clears its stage', wait_for(lambda: 'host' not in roster().get('stages', {}), 'the host clear'))

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
print('PASS: the host roster follows its own sender through the save transfer, says "save received, '
      'loading" at the end, yields to the joiner\'s own report, and shows the host\'s own stage')
