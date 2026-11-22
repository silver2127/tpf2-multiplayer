# Retained-host hot-join lab: prepare + start the isolated native pair.
# Based on prepare_retained.py (2026-09-22 trial) with the roster-hold fix and a
# forced 4-unit hash cadence applied BEFORE launch. Production is not touched.
from pathlib import Path
import subprocess, shutil, time, sys, re
r = Path('/opt/tpf2mp-linux-parity-20260921')
out = r / 'hj-lab'; out.mkdir(exist_ok=True)

# 1. the earlier prepare, minus its start loop
src = (r / 'prepare_retained.py').read_text()
cut = src.index("for name,display in [('lab',':10'),('peer',':11')]:")
exec(compile(src[:cut], 'prepare_retained', 'exec'), {'__name__': 'prep'})
# a player (non-dedicated) writes nothing to tpf2_menu.log while in game: the
# watchdog's default 180 s stall rule would kill the peer mid-run
lr = r / 'launch_retained.py'
lr.write_text(lr.read_text().replace("STALL_SECONDS='180'", "STALL_SECONDS='86400'"))
if '--save' in sys.argv:       # a different world for the retained host
    name = sys.argv[sys.argv.index('--save') + 1]
    fl = r / 'lab/share/tpf2mp/tpf2_menu_flags.txt'
    fl.write_text(re.sub(r'dedicated_save=\S+', 'dedicated_save=' + name, fl.read_text()))
    print('lab world:', name, flush=True)
if '--live' in sys.argv:       # the production path: sync_operation `retain` + tpf2mp_live_join.txt
    for n in ('lab', 'peer'):
        nd = r / n / 'share/tpf2mp/netpunch'
        for f in (r / 'hj-lab/netpunch-live').glob('*.py'):
            shutil.copy2(f, nd / f.name)
        (nd / 'tpf2mp_live_join.txt').write_text('1\n' if n == 'lab' else '0\n')
    print('live join: production sync files installed, host io dir says 1', flush=True)
if '--control' in sys.argv:   # the host reloads like everyone else
    shutil.copy2(r / 'retain-host-experiment/sync_runtime.original.py', r / 'lab/share/tpf2mp/netpunch/sync_runtime.py')
    print('control: host reload restored', flush=True)

# 2. roster fix + extra lua (probe hooks live in EVAL, not here)
for n, i in (('lab', 'a'), ('peer', 'b')):
    p = r / n / 'game/mods/mp_lockstep_1/res/scripts/mp/resync.lua'
    s = p.read_text()
    old = '\tlocal roster = tonumber(CM.rosterPlayers)\n'
    if old in s:
        s = s.replace(old, '\tlocal ctl = CM.syncRead("tpf2_bridge_ctl.txt")\n'
                           '\tlocal roster = ctl and ctl.pid == K.PROCESS_ID and tonumber(ctl.players)\n')
        p.write_text(s)
    d = r / n / 'share/tpf2mp/data'
    (d / 'tpf2mp_hash_every.txt').write_text('4\n')
    for f in d.glob('hjprobe_*'):
        f.unlink()
    subprocess.run(['chown', '-R', 'tpf2server:tpf2server', str(r / n / 'game/mods/mp_lockstep_1'), str(d)], check=True)

if '--no-start' in sys.argv:
    raise SystemExit(0)
# 3. start (same as prepare_retained.py)
for name, display in [('lab', ':10'), ('peer', ':11')]:
    subprocess.run(['systemctl', 'reset-failed', 'tpf2mp-retain-' + name + '-xvfb', 'tpf2mp-retain-' + name], stderr=subprocess.DEVNULL)
    subprocess.run(['systemd-run', '--unit=tpf2mp-retain-' + name + '-xvfb', '--uid=tpf2server', '/usr/bin/Xvfb', display,
                    '-screen', '0', '1280x720x24', '-nolisten', 'tcp'], check=True)
    for _ in range(30):
        if subprocess.run(['runuser', '-u', 'tpf2server', '--', 'env', 'DISPLAY=' + display, 'xdpyinfo'],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            break
        time.sleep(.2)
    subprocess.run(['systemd-run', '--unit=tpf2mp-retain-' + name, '/usr/bin/python3', str(r / 'launch_retained.py'), name], check=True)
print('started lab pair', flush=True)
