import unittest

from report_frame_comparison import compare_phase, geometry_summary, pacing_summary, percentile


def phase(segment, mode, median, p95, stutters):
    return {"segment": segment, "mode": mode, "median_ms": median, "p95_ms": p95,
            "over_two_intervals_fraction": stutters}


class ComparisonTests(unittest.TestCase):
    def test_better_median_does_not_hide_tail_regression(self):
        rows = [phase(1, "baseline", 24.7, 27.7, 0), phase(2, "reduced", 19.4, 30.2, .1),
                phase(3, "baseline", 24.8, 27.9, .001)]
        result = compare_phase(rows[1], rows, "baseline", True)
        self.assertEqual(result["outcome"], "observed_tail_regression")
        self.assertEqual(result["median_ms"], "below_all")
        self.assertTrue(result["bracketed"])

    def test_same_median_repeated_mode_can_have_different_outcomes(self):
        rows = [phase(1, "crop", 14, 15, .002), phase(2, "reduced", 14, 14.8, .002),
                phase(3, "reduced", 14, 45.5, .15), phase(4, "crop", 14, 15, .002)]
        self.assertNotEqual(compare_phase(rows[1], rows, "crop", True)["outcome"],
                            compare_phase(rows[2], rows, "crop", True)["outcome"])

    def test_failed_evidence_cannot_promote_improvement(self):
        rows = [phase(1, "baseline", 25, 28, .1), phase(2, "fixed", 19, 22, 0)]
        result = compare_phase(rows[1], rows, "baseline", False)
        self.assertFalse(result["bracketed"])
        self.assertIn("descriptive_only", result["outcome"])

    def test_equal_area_does_not_hide_anisotropic_sampling(self):
        log = [(10, {"eye": "0", "frame": "7", "crop": "0,0,100,40", "view": "0,0,50,80"}),
               (10, {"eye": "1", "frame": "7", "crop": "0,0,100,40", "view": "100,0,50,80"})]
        result = geometry_summary(log, {"mode": "fixed", "measure_start_epoch": 9, "end_epoch": 11}, 100, 100)
        self.assertEqual(result["active_allocation_area_fraction"]["median"], .4)
        density = result["source_pixels_per_output_crop_pixel"][0]
        self.assertEqual(density["horizontal_min"], .5)
        self.assertEqual(density["vertical_min"], 2)

    def test_p95_is_engine_delta_tail_not_reciprocal_fps(self):
        self.assertEqual(percentile([.014] * 90 + [.045] * 10, .95), .045)

    def pacing(self, start=10, end=12, epoch=1, count=10, mean=2, maximum=6, slow=1, call="xrWaitFrame", **changes):
        row = {"epoch": str(epoch), "interval_s": str(end-start), "wall_start_us": str(int(start*1e6)),
               "wall_end_us": str(int(end*1e6)), "call": call, "n": str(count), "avg_ms": str(mean),
               "max_ms": str(maximum), "slow_n": str(slow), "slow_logged": str(min(slow, 4)), "failures": "0",
               "retries": "0", "transition_skips": "0", "max_wall_start_us": str(int((end-.1)*1e6)),
               "max_wall_end_us": str(int(end*1e6)), "max_frame": "7", "max_frame_kind": "requested",
               "max_retry": "0", "max_result": "0", "max_predicted_period_ms": "13.8889",
               "frame_enabled": "1", "crop_setting": "1", "reduce_setting": "1", "native_fix": "0",
               "capture": "0", "afr": "0", "vd_dummy": "1", "sync_stage": "2", "fixed_scale": "0.74", "settings": "requested"}
        row.update(changes)
        return (end, row)

    def test_pacing_weighted_means_slow_counts_and_separate_epochs(self):
        log = [self.pacing(count=10, mean=2, slow=8), self.pacing(12, 14, count=30, mean=4, maximum=12, slow=5),
               self.pacing(14, 16, epoch=2, mean=9)]
        result = pacing_summary(log, {"mode": "fixed", "measure_start_epoch": 10, "end_epoch": 20}, .74)
        first = result["epochs"][0]["calls"]["xrWaitFrame"]
        self.assertEqual(first["mean_ms"], 3.5)
        self.assertEqual(first["max_ms"], 12)
        self.assertEqual(first["slow_n"], 13)
        self.assertEqual(first["slow_logged"], 8)
        self.assertEqual(len(result["epochs"]), 2)
        self.assertEqual(result["epochs"][1]["calls"]["xrWaitFrame"]["mean_ms"], 9)

    def test_pacing_requires_whole_interval_and_matching_requested_settings(self):
        log = [self.pacing(9, 11), self.pacing(19, 21), self.pacing(12, 14, fixed_scale="0"),
               self.pacing(14, 16, capture="1"), self.pacing(16, 18)]
        result = pacing_summary(log, {"mode": "fixed", "measure_start_epoch": 10, "end_epoch": 20}, .74)
        self.assertEqual(result["excluded_boundary_reports"], 2)
        self.assertEqual(result["excluded_setting_reports"], 2)
        self.assertEqual(result["epochs"][0]["calls"]["xrWaitFrame"]["n"], 10)

    def test_pacing_each_call_has_own_count_and_late_log_time_does_not_shift_interval(self):
        log = [(99, self.pacing(call=name)[1]) for name in ("xrWaitFrame", "xrBeginFrame", "xrEndFrame")]
        result = pacing_summary(log, {"mode": "fixed", "measure_start_epoch": 10, "end_epoch": 20}, .74)
        self.assertEqual(result["epochs"][0]["missing_calls"], [])
        self.assertEqual(len(result["epochs"][0]["calls"]), 3)

    def test_old_archives_without_pacing_are_explicitly_unavailable(self):
        result = pacing_summary([], {"mode": "fixed", "measure_start_epoch": 10, "end_epoch": 20}, .74)
        self.assertFalse(result["available"])

    def test_native_flag_selects_native_reports_and_excludes_off_reports(self):
        native = pacing_summary([self.pacing(native_fix="1", reduce_setting="0", fixed_scale="0")], {"mode": "crop", "native_stereo_fix": True,
                                               "measure_start_epoch": 10, "end_epoch": 20}, .74)
        self.assertTrue(native["available"])
        off = pacing_summary([self.pacing(native_fix="0", reduce_setting="0", fixed_scale="0")], {"mode": "crop", "native_stereo_fix": True,
                                         "measure_start_epoch": 10, "end_epoch": 20}, .74)
        self.assertFalse(off["available"])


if __name__ == "__main__":
    unittest.main()
