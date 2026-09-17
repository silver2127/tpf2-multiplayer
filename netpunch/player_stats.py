"""Player statistics for the dedicated relay (2026-09-17).

The relay is the one long-lived process in a session, so it is where "who
plays, how much, how many at once" can be counted. Everything here is derived
from what the relay already sees -- joins, leaves, starts, the frames it
forwards -- keyed by the player's profile code when the client sends one (a
stable per-install id) and by name otherwise. Nothing is looked up anywhere.

Persisted as ONE JSON object (``player_stats.json`` in the relay's io dir),
written atomically, at most once a minute and on shutdown; a corrupt or
missing file starts the counts from zero. A summary line goes to the log
every STATS_LOG_EVERY seconds while somebody is connected.

    python3 player_stats.py /var/lib/tpf2mp/relay/player_stats.json   prints the summary
"""
import json
import os
import sys
import time

STATS_FLUSH_EVERY = 60.0
STATS_LOG_EVERY = 600.0
MAX_PLAYERS_KEPT = 5000      # the per-player table never grows without bound


def _iso(t):
    return time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(t)) if t else '-'


class PlayerStats:
    def __init__(self, path, log=lambda _: None, clock=time.time):
        self.path = path
        self.log = log
        self.clock = clock
        self.players = {}        # key -> record
        self.total = dict(joins=0, starts=0, unique=0, peak=0, peak_at=0.0, sessions=0,
                          connected_seconds=0.0, frames=0, bytes=0, first_seen=0.0)
        self.online = {}         # key -> connected-since (this run only)
        self._dirty = False
        self._load()
        self._flushed = self.clock()      # the first flush is a minute in (or the shutdown's forced one)
        self._logged = 0.0

    # ---- persistence -----------------------------------------------------
    def _load(self):
        try:
            with open(self.path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            players = data.get('players') or {}
            total = data.get('total') or {}
            if not isinstance(players, dict) or not isinstance(total, dict):
                raise ValueError('shape')
            self.players = {str(k): dict(v) for k, v in players.items() if isinstance(v, dict)}
            for k, v in total.items():
                if k in self.total and type(v) in (int, float):
                    self.total[k] = v
            self.total['unique'] = len(self.players)
        except (OSError, ValueError, TypeError):
            self.players, self.online = {}, {}
        if not self.total['first_seen']:
            self.total['first_seen'] = self.clock()

    def flush(self, force=False):
        now = self.clock()
        if not force and (not self._dirty or now - self._flushed < STATS_FLUSH_EVERY):
            return False
        # live time counts while connected, so a crash loses at most a minute
        snapshot = self.snapshot()
        tmp = self.path + '.tmp'
        try:
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(snapshot, f, indent=1, sort_keys=True)
            os.replace(tmp, self.path)
        except OSError as e:
            self.log(f'[stats] could not write {self.path}: {e}')
            return False
        self._dirty, self._flushed = False, now
        return True

    def snapshot(self):
        """The persisted shape, with the time of the players still connected
        folded in as if they had left now (they are counted again from now)."""
        now = self.clock()
        players = {k: dict(v) for k, v in self.players.items()}
        total = dict(self.total)
        for key, since in self.online.items():
            extra = max(0.0, now - since)
            if key in players:
                players[key]['connected_seconds'] = players[key].get('connected_seconds', 0.0) + extra
                players[key]['last_seen'] = now
            total['connected_seconds'] += extra
        total['unique'] = len(players)
        total['online'] = len(self.online)
        total['written'] = now
        return {'version': 1, 'total': total, 'players': players}

    # ---- events ------------------------------------------------------------
    @staticmethod
    def key_for(name, profile=None):
        p = str(profile or '').strip()
        return ('p:' + p) if p else ('n:' + str(name))

    def join(self, name, profile=None):
        now = self.clock()
        key = self.key_for(name, profile)
        rec = self.players.get(key)
        if rec is None:
            if len(self.players) >= MAX_PLAYERS_KEPT:
                # forget the player least recently seen
                oldest = min(self.players, key=lambda k: self.players[k].get('last_seen', 0))
                del self.players[oldest]
            rec = self.players[key] = dict(name=str(name), first_seen=now, last_seen=now, joins=0, starts=0,
                                           connected_seconds=0.0, frames=0, bytes=0, names=[])
            self.total['unique'] = len(self.players)
        rec['name'] = str(name)
        if str(name) not in rec.setdefault('names', []):
            rec['names'] = (rec['names'] + [str(name)])[-8:]
        rec['joins'] += 1
        rec['last_seen'] = now
        self.total['joins'] += 1
        if not self.online:
            self.total['sessions'] += 1          # 0 -> 1 connected: a session begins
        self.online[key] = now
        if len(self.online) > self.total['peak']:
            self.total['peak'], self.total['peak_at'] = len(self.online), now
        self._dirty = True
        return key

    def leave(self, name, profile=None):
        now = self.clock()
        key = self.key_for(name, profile)
        since = self.online.pop(key, None)
        rec = self.players.get(key)
        if rec is not None:
            rec['last_seen'] = now
            if since is not None:
                rec['connected_seconds'] = rec.get('connected_seconds', 0.0) + max(0.0, now - since)
        if since is not None:
            self.total['connected_seconds'] += max(0.0, now - since)
        self._dirty = True

    def started(self, name, profile=None):
        """The player was started into a world (a save push or a frozen join)."""
        rec = self.players.get(self.key_for(name, profile))
        if rec is not None:
            rec['starts'] = rec.get('starts', 0) + 1
            self.total['starts'] += 1
            self._dirty = True

    def frame(self, name, nbytes, profile=None):
        """One game frame relayed FROM this player (the traffic it generates)."""
        rec = self.players.get(self.key_for(name, profile))
        if rec is not None:
            rec['frames'] = rec.get('frames', 0) + 1
            rec['bytes'] = rec.get('bytes', 0) + int(nbytes)
        self.total['frames'] += 1
        self.total['bytes'] += int(nbytes)
        # frames arrive many times a second: the minute flush picks them up
        self._dirty = True

    # ---- reporting ---------------------------------------------------------
    def summary(self):
        s = self.snapshot()
        t = s['total']
        hours = t['connected_seconds'] / 3600.0
        return (f"online {t['online']}, peak {t['peak']} ({_iso(t['peak_at'])}), unique {t['unique']}, "
                f"joins {t['joins']}, starts {t['starts']}, sessions {t['sessions']}, "
                f"{hours:.1f} player-hours, {t['frames']} frames / {t['bytes'] / 1e6:.1f} MB relayed "
                f"since {_iso(t['first_seen'])}")

    def top(self, n=10):
        s = self.snapshot()
        rows = sorted(s['players'].values(), key=lambda r: -r.get('connected_seconds', 0))[:n]
        return [(r.get('name'), r.get('connected_seconds', 0) / 3600.0, r.get('joins', 0), _iso(r.get('last_seen', 0))) for r in rows]

    def tick(self):
        now = self.clock()
        self.flush()
        if self.online and now - self._logged >= STATS_LOG_EVERY:
            self._logged = now
            self.log('[stats] ' + self.summary())


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'player_stats.json'
    st = PlayerStats(path)
    print(st.summary())
    for name, hours, joins, last in st.top(20):
        print(f'  {name:<32} {hours:6.1f} h  {joins:4d} join(s)  last {last}')
