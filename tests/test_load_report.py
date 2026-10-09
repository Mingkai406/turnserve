import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from load_benchmark import summarize


class ReportTests(unittest.TestCase):
    def test_failures_remain_in_denominator_and_arrival_time(self):
        events = [
            dict(type=t, session=s, ms=ms)
            for s, t, ms in [
                ("a", "offered", 0),
                ("a", "dispatched", 20),
                ("a", "token", 40),
                ("a", "completed", 80),
                ("b", "offered", 0),
                ("b", "dispatched", 3),
                ("b", "queue_rejected", 3),
            ]
        ]
        result = summarize(events, 100)
        self.assertEqual(result["deadline_success_rate"], 0.5)
        self.assertEqual(result["completed_ttft_p95_ms"], 40)
        self.assertEqual(result["dispatch_lag_p95_ms"], 20)
        with self.assertRaises(ValueError):
            summarize(events[:-1])


if __name__ == "__main__":
    unittest.main()
