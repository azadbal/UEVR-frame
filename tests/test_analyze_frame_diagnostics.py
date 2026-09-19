"""Focused parser checks for the archived Frame Perf field layouts."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import analyze_frame_diagnostics as analyzer


NEGATIVE = "[Frame Perf] submit frame=340 pose=339 prepared=false matched=false projections=0 rects=0 native_fix=true cropped=0 reduced=0\n"
POSITIVE = "[Frame Perf] submit frame=8761 pose=8761 prepared=true matched=true projections=3 rects=3 native_fix=false cropped=3 reduced=3\n"
TIMING = ("[Frame Timing] scope=uevr_openxr_native_submission unit_ms=1 mask_enabled=true "
          "gpu_n=2 gpu_copy_avg=2 gpu_copy_max=3 gpu_mask_avg=4 gpu_mask_max=5 "
          "cpu_n=0 cpu_image_fence_wait_avg=99 cpu_image_fence_wait_max=99 "
          "cpu_record_execute_avg=99 cpu_record_execute_max=99 gpu_unavailable_images=1 pending_skips=2 invalid=0\n"
          "[Frame Timing] scope=uevr_openxr_native_submission unit_ms=1 mask_enabled=true "
          "gpu_n=1 gpu_copy_avg=8 gpu_copy_max=9 gpu_mask_avg=10 gpu_mask_max=11 "
          "cpu_n=2 cpu_image_fence_wait_avg=3 cpu_image_fence_wait_max=5 "
          "cpu_record_execute_avg=7 cpu_record_execute_max=9 gpu_unavailable_images=0 pending_skips=1 invalid=1\n")


class FrameDiagnosticsTests(unittest.TestCase):
    def _log(self, text):
        handle = tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".log", delete=False)
        handle.write(text)
        handle.close()
        self.addCleanup(lambda: Path(handle.name).unlink(missing_ok=True))
        return Path(handle.name)

    def test_numeric_masks_and_matched_submit(self):
        summary = analyzer.summarize(self._log(NEGATIVE + POSITIVE))
        self.assertEqual(summary["submits"], 2)
        self.assertEqual(summary["cropped"], 1)
        self.assertEqual(summary["reduced"], 1)
        self.assertEqual(summary["matched_geometry"], 1)
        self.assertEqual(summary["paired_both_eye_matched"], 1)
        invalid = analyzer.summarize(self._log(POSITIVE.replace("projections=3", "projections=4")))
        self.assertEqual(invalid["paired_both_eye_matched"], 0)

    def test_require_matched_checks_every_log(self):
        negative = self._log(NEGATIVE)
        positive = self._log(POSITIVE)
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            status = analyzer.main(["--require-matched", str(positive), str(negative)])
        self.assertEqual(status, 1)
        self.assertIn("ASSERTION FAILED", output.getvalue())

    def test_timing_uses_sample_weighted_means_and_excludes_zero_samples(self):
        summary = analyzer.summarize(self._log(TIMING))
        timing = summary["timing"]["true"]
        self.assertEqual(timing["gpu_n"], 3)
        self.assertAlmostEqual(timing["weighted"]["gpu_copy_avg"], 4.0)
        self.assertEqual(timing["weighted"].get("cpu_image_fence_wait_avg"), 3.0)
        self.assertEqual(timing["max_unavailable_images"], 1)
        self.assertEqual(timing["maxima"]["gpu_copy_max"], 9.0)
        self.assertEqual(timing["pending_skips"], 3)
        self.assertEqual(timing["invalid"], 1)


if __name__ == "__main__":
    unittest.main()
