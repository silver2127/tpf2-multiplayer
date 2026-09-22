# One retained-host hot-join experiment, end to end, on the private lab pair.
# usage: hj_run.py [--dumps N] [--no-restart]
#   restarts the pair (hj_prepare.py), waits for the host world, opens the
#   peer's Multiplayer panel, joins, installs the person probe on both sides
#   as soon as the peer's world is up, collects N paired dumps, compares.
# Production (tpf2mp-game) is never touched.
import json, subprocess, sys, time
from pathlib import Path
r = Path('/opt/tpf2mp-linux-parity-20260921')
lab = r / 'hj-lab'
dumps = int(sys.argv[sys.argv.index('--dumps') + 1]) if '--dumps' in sys.argv else 20
t0 = time.time()


def say(*a):
    print(f'[{time.time() - t0:6.0f}s]', *a, flush=True)


def sh(*cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def xclick(display, x, y):
    sh('xdotool', 'mousemove', str(x), str(y), 'mousedown', '1', 'sleep', '0.3', 'mouseup', '1', env={'DISPLAY': display})


def sync_phase(name):
    p = r / name / 'share/tpf2mp/netpunch/lobby_out.jsonl'
    try:
        ev = [json.loads(s) for s in p.read_text().splitlines() if s.strip()]
    except Exception:
        return None, None
    e = next((e for e in reversed(ev) if e.get('type') == 'sync_state'), {})
    return e.get('operation'), e.get('phase')


def native(name):
    p = r / name / 'share/tpf2mp/data/tpf2_native_status.txt'
    try:
        return dict(l.split('=', 1) for l in p.read_text().splitlines() if '=' in l)
    except Exception:
        return {}


def lua_world(name):
    """the Lua world token the game last acknowledged (a new one after a load)"""
    try:
        f = dict(l.split('=', 1) for l in (r / name / 'share/tpf2mp/data/tpf2_sync_lua_ack.txt').read_text().splitlines() if '=' in l)
        return f.get('world')
    except Exception:
        return None


def wait(cond, what, timeout):
    end = time.time() + timeout
    while time.time() < end:
        v = cond()
        if v:
            return v
        for u in ('tpf2mp-retain-lab', 'tpf2mp-retain-peer'):
            if subprocess.run(['systemctl', 'is-active', '-q', u]).returncode:
                raise SystemExit(f'{u} died while waiting for {what}')
        time.sleep(2)
    raise SystemExit(f'timeout waiting for {what}')


def peer_menu_ready():
    log = (r / 'peer/share/tpf2mp/data/tpf2_menu.log')
    return log.exists() and 'main page built' in log.read_text(errors='replace')


if '--no-restart' not in sys.argv:
    subprocess.run(['systemctl', 'stop', 'tpf2mp-retain-peer', 'tpf2mp-retain-lab'])
    subprocess.run(['systemctl', 'stop', 'tpf2mp-retain-peer-xvfb', 'tpf2mp-retain-lab-xvfb'])
    for n in ('lab', 'peer'):
        for f in ('tpf2_menu.log', 'tpf2_bridge.log'):
            (r / n / 'share/tpf2mp/data' / f).unlink(missing_ok=True)
        st = r / n / 'share/tpf2mp/data/tpf2_native_status.txt'
        st.unlink(missing_ok=True)
    extra = [a for a in sys.argv[1:] if a in ('--control', '--live')]
    if '--save' in sys.argv: extra += ['--save', sys.argv[sys.argv.index('--save') + 1]]
    sh('python3', str(r / 'hj_prepare.py'), *extra, stdout=subprocess.DEVNULL)
    say('pair started')
wait(lambda: native('lab').get('has_world') == '1', 'host world', 900)
say('host world up')
time.sleep(20)
sh('python3', str(lab / 'hj_eval.py'), str(lab / 'probe.lua'), 'lab', stdout=subprocess.DEVNULL)
prejoin = int(sys.argv[sys.argv.index('--prejoin') + 1]) if '--prejoin' in sys.argv else 0
if prejoin:
    say(f'host runs alone for {prejoin} s before the join')
    time.sleep(prejoin)
wait(peer_menu_ready, 'peer title menu', 600)
time.sleep(8)
xclick(':11', 120, 321)   # MULTIPLAYER
time.sleep(3)
op0 = sync_phase('peer')[0]
sh('python3', str(r / 'join_retained.py'), stdout=subprocess.DEVNULL)
say('join clicked')
wait(lambda: sync_phase('peer')[0] not in (None, op0) or sync_phase('lab')[1] in ('holding', 'waiting', 'saving', 'transferring'), 'join round start', 180)
say('round started')
wait(lambda: sync_phase('lab')[1] not in (None, 'holding') or None, 'host past holding', 600)
world0 = lua_world('lab')
wait(lambda: native('peer').get('has_world') == '1' and native('lab').get('has_world') == '1' and sync_phase('peer')[1] in ('checking', 'releasing', 'complete'), 'round worlds', 900)
sh('python3', str(lab / 'hj_eval.py'), str(lab / 'probe.lua'), 'both', stdout=subprocess.DEVNULL)
say('peer world up, probe queued; phases', sync_phase('lab'), sync_phase('peer'))
wait(lambda: sync_phase('peer')[1] in ('complete', 'error', 'aborted') and sync_phase('peer')[1], 'round end', 600)
say('round', sync_phase('lab'), sync_phase('peer'))
world1 = lua_world('lab')
say(f'host world token before the join {world0}, after {world1}: '
    + ('KEPT (retained host)' if world0 and world0 == world1 else 'the host loaded a new world'))
sh('python3', str(r / 'force_hash_grid.py'), stdout=subprocess.DEVNULL)   # the 4-unit hash cadence
wait(lambda: len(list((r / 'peer/share/tpf2mp/data').glob('hjprobe_b_*.txt'))) >= dumps, f'{dumps} peer dumps', 1800)
say('collected')
sh('python3', str(lab / 'hj_compare.py'))
