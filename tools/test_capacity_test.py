#!/usr/bin/env python3
"""Tests of capacity_test.py with synthetic BMS data where the answer is known: python3 tools/test_capacity_test.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capacity_test as ct  # noqa: E402

T0 = 1_760_000_000


def seg(rows, minutes, soc, cell, amps, temp=18.0):
    """Append @p minutes of one-minute rows; amps in A (+ charge)."""
    t = rows[-1][0] + 60 if rows else T0
    for _ in range(minutes):
        rows.append((t, soc, cell, int(round(amps * 100)), temp))
        t += 60


def charge_run(extra_rest_h=0, gap_min=0):
    rows = []
    seg(rows, 30, 1, 3.00, 0.0)                    # low rest anchor
    if extra_rest_h:
        seg(rows, extra_rest_h * 60, 1, 3.00, 0.0)  # a dark night: nothing seen by the BMS
    seg(rows, 600, 50, 3.30, 20.0)                  # 10 h at 20 A = 200 Ah
    if gap_min:
        rows = rows[:-300] + [(r[0] + gap_min * 60, *r[1:]) for r in rows[-300:]]
    seg(rows, 1, 100, 3.45, 0.0)
    return rows


class CapacityTest(unittest.TestCase):
    def test_charge_episode_counts_the_ah(self):
        res = ct.find_episode(charge_run(), 3.08, "charge", 0.09)
        self.assertEqual(res["status"], "done")
        self.assertAlmostEqual(res["capacity_ah"], 200.0, delta=0.5)
        self.assertEqual(res["gaps"], [])
        self.assertAlmostEqual(res["temp_c"], 18.0, delta=0.1)

    def test_start_is_the_last_rest_sample_of_the_low_phase(self):
        rows = charge_run()
        res = ct.find_episode(rows, 3.08, "charge", 0.09)
        self.assertEqual(res["start"], rows[29][0])

    def test_idle_draw_is_subtracted(self):
        # 12 h of rest inside the window: the BMS shows 0 A but 0.09 A really went out
        rows = charge_run(extra_rest_h=0)
        rows2 = []
        seg(rows2, 30, 1, 3.00, 0.0)
        seg(rows2, 100, 10, 3.10, 15.0)             # charging starts, still above the anchor
        seg(rows2, 720, 10, 3.20, 0.0)              # a night at rest above the anchor level
        seg(rows2, 400, 60, 3.30, 20.0)
        seg(rows2, 1, 100, 3.45, 0.0)
        res = ct.find_episode(rows2, 3.08, "charge", 0.09)
        self.assertAlmostEqual(res["idle_ah"], 0.09 * 12, delta=0.05)
        self.assertAlmostEqual(res["capacity_ah"], res["counted_ah"] - res["idle_ah"], places=6)

    def test_gap_is_reported_as_unreliable(self):
        res = ct.find_episode(charge_run(gap_min=45), 3.08, "charge", 0.09)
        self.assertEqual(len(res["gaps"]), 1)
        self.assertIn("unreliable", ct.describe(res))

    def test_no_anchor(self):
        rows = []
        seg(rows, 600, 50, 3.20, 0.0)
        res = ct.find_episode(rows, 3.08, "charge", 0.09)
        self.assertIn("no low anchor", res["status"])
        self.assertIn("No result", ct.describe(res))

    def test_anchor_needs_15_minutes_at_rest(self):
        rows = []
        seg(rows, 10, 1, 3.00, 0.0)
        seg(rows, 600, 50, 3.30, 20.0)
        seg(rows, 1, 100, 3.45, 0.0)
        self.assertEqual(ct.low_anchors(rows, 3.08), [])

    def test_a_load_is_no_rest(self):
        rows = []
        seg(rows, 60, 1, 3.00, -2.0)                  # low voltage, but 2 A discharge: sag, not a rest value
        self.assertEqual(ct.low_anchors(rows, 3.08), [])

    def test_running_episode_before_full(self):
        rows = []
        seg(rows, 30, 1, 3.00, 0.0)
        seg(rows, 300, 20, 3.25, 10.0)                # 50 Ah so far
        res = ct.find_episode(rows, 3.08, "charge", 0.09)
        self.assertEqual(res["status"], "running")
        self.assertAlmostEqual(res["capacity_ah"], 50.0, delta=0.5)
        self.assertIn("Running charge test", ct.describe(res))

    def test_newer_running_episode_is_not_hidden_by_an_old_one(self):
        rows = charge_run()
        seg(rows, 120, 90, 3.40, -5.0)
        seg(rows, 40, 2, 3.01, 0.0)                    # a new low anchor, no full charge yet
        res = ct.find_episode(rows, 3.08, "charge", 0.09)
        self.assertEqual(res["status"], "running")
        self.assertEqual(len(ct.episodes(rows, 3.08, "charge", 0.09)), 2)

    def test_two_anchors_before_one_full_charge_use_the_last(self):
        rows = []
        seg(rows, 30, 1, 3.00, 0.0)
        seg(rows, 60, 3, 3.10, 5.0)                    # 5 Ah in, above the anchor
        seg(rows, 30, 2, 3.05, 0.0)                    # low again
        seg(rows, 600, 50, 3.30, 20.0)
        seg(rows, 1, 100, 3.45, 0.0)
        eps = ct.episodes(rows, 3.08, "charge", 0.09)
        self.assertEqual(len(eps), 1)
        self.assertAlmostEqual(eps[0]["capacity_ah"], 200.0, delta=1.0)

    def test_discharge_mode(self):
        rows = []
        seg(rows, 5, 100, 3.45, 0.0)
        seg(rows, 600, 50, 3.25, -15.0)                # 10 h at 15 A = 150 Ah out
        seg(rows, 30, 3, 3.05, 0.0)
        res = ct.find_episode(rows, 3.08, "discharge", 0.09)
        self.assertEqual(res["status"], "done")
        self.assertAlmostEqual(res["capacity_ah"], 150.0, delta=0.5)

    def test_influx_line(self):
        res = ct.find_episode(charge_run(), 3.08, "charge", 0.09)
        line = ct.influx_line(res)
        self.assertTrue(line.startswith("CapacityTest,Mode=charge CapacityAh=200"))
        self.assertTrue(line.endswith(" %d" % int(res["end"])))
        self.assertNotIn("\n", line)


if __name__ == "__main__":
    unittest.main(verbosity=1)
