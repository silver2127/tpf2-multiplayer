"""Transfer UI rate, stalls, recovery and transport changes."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "netpunch"))
from transfer_status import TransferMeter


class TransferStatus(unittest.TestCase):
    def test_progress_stall_and_retry(self):
        meter = TransferMeter()
        event = meter.event(10, 0, 113_000_000, "Steam", "connecting", "send", "Player")
        self.assertEqual(event["bytes_total"], 113_000_000)
        self.assertIn("113.0 MB", event["detail"])
        self.assertIsNone(meter.event(10.2, 500_000, 113_000_000, "Steam", "connecting", "send", "Player"))
        event = meter.event(11, 2_000_000, 113_000_000, "TCP", "connected", "send", "Player")
        self.assertEqual(event["bytes_per_second"], 2_000_000)
        self.assertIn("2.00 MB/s", event["detail"])
        event = meter.event(12, 2_000_000, 113_000_000, "Steam", "interrupted", "send", "Player")
        self.assertEqual(event["bytes_per_second"], 0)
        self.assertIn("TCP interrupted", event["detail"])
        event = meter.event(13, 0, 113_000_000, "Steam", "failed", "send", "Player")
        self.assertEqual(event["bytes_per_second"], 0)
        self.assertIn("TCP failed", event["detail"])

    def test_peers_have_separate_rates_and_new_save_resets(self):
        a, b = TransferMeter(), TransferMeter()
        for meter in (a, b):
            meter.event(0, 0, 4_000_000, "TCP", "connected", "recv")
        self.assertEqual(a.event(1, 3_000_000, 4_000_000, "TCP", "connected", "recv")["bytes_per_second"], 3_000_000)
        self.assertEqual(b.event(1, 1_000_000, 4_000_000, "TCP", "connected", "recv")["bytes_per_second"], 1_000_000)
        self.assertEqual(TransferMeter().event(2, 0, 5_000_000, "UDP", "off", "recv")["bytes_per_second"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
