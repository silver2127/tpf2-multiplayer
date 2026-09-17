"""Relay player statistics: counts, peak, persistence, corrupt file, bounded table."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'netpunch'))
import player_stats
from player_stats import PlayerStats


class StatsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = str(Path(self.temp.name) / 'player_stats.json')
        self.now = 1000.0
        self.lines = []
        self.st = PlayerStats(self.path, log=self.lines.append, clock=lambda: self.now)

    def test_joins_peak_sessions_and_time(self):
        self.st.join('alice', 'PROFA')
        self.now += 60
        self.st.join('bob')
        self.assertEqual(self.st.total['peak'], 2)
        self.assertEqual(self.st.total['peak_at'], 1060.0)
        self.assertEqual(self.st.total['sessions'], 1)
        self.now += 600
        self.st.leave('alice', 'PROFA')
        self.st.leave('bob')
        self.assertEqual(self.st.total['sessions'], 1)
        self.now += 10
        self.st.join('alice', 'PROFA')                       # a second session begins at 0 -> 1
        self.assertEqual(self.st.total['sessions'], 2)
        self.assertEqual(self.st.total['joins'], 3)
        self.assertEqual(self.st.total['unique'], 2)
        a = self.st.players['p:PROFA']
        self.assertEqual(a['joins'], 2)
        self.assertEqual(a['connected_seconds'], 660.0)
        self.assertEqual(self.st.players['n:bob']['connected_seconds'], 600.0)
        self.assertAlmostEqual(self.st.total['connected_seconds'], 1260.0)

    def test_profile_is_the_identity_and_names_are_remembered(self):
        self.st.join('alice', 'PROFA')
        self.st.leave('alice', 'PROFA')
        self.st.join('alicia', 'PROFA')                      # renamed, same install
        self.assertEqual(self.st.total['unique'], 1)
        self.assertEqual(self.st.players['p:PROFA']['names'], ['alice', 'alicia'])
        self.assertEqual(self.st.players['p:PROFA']['name'], 'alicia')

    def test_starts_and_frames(self):
        self.st.join('alice')
        self.st.started('alice')
        self.st.frame('alice', 500)
        self.st.frame('alice', 700)
        self.st.frame('nobody', 9)                          # unknown player: total only
        a = self.st.players['n:alice']
        self.assertEqual((a['starts'], a['frames'], a['bytes']), (1, 2, 1200))
        self.assertEqual((self.st.total['starts'], self.st.total['frames'], self.st.total['bytes']), (1, 3, 1209))

    def test_snapshot_folds_live_time_and_persists_atomically(self):
        self.st.join('alice')
        self.now += 30
        s = self.st.snapshot()
        self.assertEqual(s['players']['n:alice']['connected_seconds'], 30.0)
        self.assertEqual(s['total']['online'], 1)
        self.assertEqual(self.st.players['n:alice']['connected_seconds'], 0.0)   # not double counted
        self.assertFalse(self.st.flush())                    # not a minute since the load
        self.now += 60
        self.assertTrue(self.st.flush())
        self.assertFalse(Path(self.path + '.tmp').exists())
        data = json.loads(Path(self.path).read_text(encoding='utf-8'))
        self.assertEqual(data['total']['joins'], 1)
        # a second run continues the counts; the live player of the old run is not "online"
        again = PlayerStats(self.path, clock=lambda: self.now)
        self.assertEqual(again.total['joins'], 1)
        self.assertEqual(again.total['unique'], 1)
        self.assertEqual(again.online, {})
        self.assertEqual(again.players['n:alice']['connected_seconds'], 90.0)

    def test_corrupt_file_starts_from_zero(self):
        Path(self.path).write_text('{not json', encoding='utf-8')
        st = PlayerStats(self.path, clock=lambda: self.now)
        self.assertEqual(st.total['joins'], 0)
        Path(self.path).write_text(json.dumps({'players': [], 'total': 3}), encoding='utf-8')
        st = PlayerStats(self.path, clock=lambda: self.now)
        self.assertEqual(st.players, {})

    def test_table_is_bounded_and_summary_logs_while_online(self):
        old = player_stats.MAX_PLAYERS_KEPT
        player_stats.MAX_PLAYERS_KEPT = 3
        try:
            for i in range(5):
                self.now += 1
                self.st.join(f'p{i}')
                self.st.leave(f'p{i}')
            self.assertEqual(len(self.st.players), 3)
            self.assertNotIn('n:p0', self.st.players)
            self.assertIn('n:p4', self.st.players)
        finally:
            player_stats.MAX_PLAYERS_KEPT = old
        self.st.join('alice')
        self.now += player_stats.STATS_LOG_EVERY + 1
        self.st.tick()
        self.assertTrue(any(l.startswith('[stats] online 1, peak') for l in self.lines), self.lines)
        self.assertIn('unique', self.st.summary())
        self.assertEqual(self.st.top(1)[0][0], 'alice')


if __name__ == '__main__':
    unittest.main()
