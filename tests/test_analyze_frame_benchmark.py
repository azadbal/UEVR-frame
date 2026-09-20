import json
from datetime import datetime, timedelta
from pathlib import Path
import tempfile
import unittest
import struct

from analyze_frame_benchmark import analyze


class BenchmarkEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.base = datetime(2026, 9, 18, 21, 0, 0)
        self.lines = []
        modes = ["baseline", "crop", "reduced", "fixed", "crop", "native_scaled"]
        (self.path / "summary.json").write_text(json.dumps({"segments": modes, "fixed_scale": .5, "scale": 3}))
        events = []
        self.line(0, "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 5")
        for segment, mode in enumerate(modes, 1):
            begin = segment * 40
            start = (self.base + timedelta(seconds=begin + 10)).timestamp()
            stem = self.path / f"codex-frame-benchmark-{segment:02d}-{mode}"
            stem.with_suffix(".txt").write_text(
                f"state=complete\nsegment={segment}\nmode={mode}\nstart_epoch={start-10}\n"
                f"warmup_seconds=10\nmeasure_seconds=20\nmeasure_start_epoch={start}\nend_epoch={start+20}\n"
                "samples=3\nwidth=100\nheight=100\n")
            stem.with_suffix(".csv").write_text("sample,delta_seconds\n1,0.010\n2,0.020\n3,0.030\n")
            events.append(f"epoch={start-10} segment={segment} phase={mode} state=warming")
            cropped = 0 if mode == "baseline" else 3
            reduced = 3 if mode in ("reduced", "fixed") else 0
            source_size = 50 if mode == "fixed" else 60 if mode == "reduced" else 100
            scale = .5 if mode == "fixed" else 0
            for offset in range(1, 20, 2):
                frame = segment * 100 + offset
                second = begin + 10 + offset
                self.line(second, f"[Frame Perf] submit frame={frame} pose={frame} prepared=true matched=true projections=3 rects=3 "
                          f"scene=200x100 output=200x100 native_fix=false sceneview=false splitscreen=false cropped={cropped} reduced={reduced}")
                for eye in range(2):
                    self.line(second, f"[Frame Perf] candidate frame={frame} eye={eye} crop=20,20,60,60 area_pct=36.00 fallback=0 "
                              f"view={eye*100},0,{source_size},{source_size} P=[1,0,0,0]")
            timing = (f"[Frame Timing] scope=uevr_openxr_submission unit_ms=1 epoch={segment} interval_s=2.001 "
                      f"native_fix=0 mask_enabled=1 crop_setting={int(cropped>0)} reduce_setting={int(reduced>0)} "
                      f"fixed_scale={scale} source_fixed_scale={scale} cropped={cropped} reduced={reduced} "
                      f"lost=0 resolved={int(cropped>0)} suppressed=0 mask_drawn=3 gpu_n=120 "
                      f"frame_first={segment*100+7} frame_last={segment*100+9}")
            for scope in ("pre", "source_copy", "reconstruct", "additional", "mask"):
                timing += f" gpu_{scope}_avg=0.1250 gpu_{scope}_max=0.2500"
            self.line(begin + 20, timing)
        (self.path / "codex-frame-benchmark-events.txt").write_text("\n".join(events))
        self.save_log()

    def line(self, seconds, message):
        stamp = (self.base + timedelta(seconds=seconds)).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        self.lines.append(f"[{stamp}] [UnrealVR] [info] {message}")

    def save_log(self):
        (self.path / "log.txt").write_text("\n".join(sorted(self.lines)))

    def enable_captures(self):
        path = self.path / "summary.json"
        run = json.loads(path.read_text())
        run["capture_frames"] = True
        path.write_text(json.dumps(run))
        events = []
        for segment, mode in enumerate(run["segments"], 1):
            begin = segment * 40
            epoch = (self.base + timedelta(seconds=begin)).timestamp()
            events.extend([f"epoch={epoch-5} segment={segment} phase={mode} state=warming",
                           f"epoch={epoch-2} capture_requested segment={segment}",
                           f"epoch={epoch-1} capture_finished segment={segment}"])
            cropped = 0 if mode == "baseline" else 3
            reduced = 3 if mode in ("reduced", "fixed") else 0
            scale = .5 if mode == "fixed" else 0
            state = (f"frame={segment*100} pose={segment*100} generation=7 cropped={cropped} reduced={reduced} "
                     f"fixed_scale={scale} resolved={'false' if mode == 'baseline' else 'true'} suppressed=false mask_drawn=3 size=200x100")
            bmp = self.path / f"capture-{segment}.bmp"
            header = struct.pack("<2sIHHI", b"BM", 80054, 0, 0, 54)
            header += struct.pack("<IiiHHIIiiII", 40, 200, -100, 1, 32, 0, 80000, 0, 0, 0, 0)
            bmp.write_bytes(header + bytes(80000))
            self.line(begin - 1.8, "[Frame Capture] queued " + state + " bytes=80000")
            self.line(begin - 1.5, "[Frame Capture] complete " + state + f' write_stall_ms=5.000 file="{bmp}"')
        (self.path / "codex-frame-benchmark-events.txt").write_text("\n".join(events))
        self.save_log()

    def test_complete_repeated_phases_and_scope_statistics(self):
        result = analyze(self.path)
        self.assertTrue(result["passed"], result["errors"])
        self.assertEqual(result["gpu_samples"], 720)
        self.assertEqual(len(result["phases"]), 6)
        self.assertEqual(result["phases"][3]["observed_active_rectangles"], [(0, 0, 0, 50, 50), (1, 100, 0, 50, 50)])
        self.assertEqual(result["phases"][0]["submission_gpu_ms"]["source_copy"], {"mean": .125, "max": .25})

    def test_reduction_fallback_fails_even_with_other_good_phases(self):
        self.lines = [line.replace("cropped=3 reduced=3", "cropped=3 reduced=0") if "submit frame=3" in line else line for line in self.lines]
        self.save_log()
        self.assertFalse(analyze(self.path)["passed"])

    def test_visibility_loss_during_measurement_fails(self):
        self.line(132, "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 4")
        self.line(139, "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 5")
        self.save_log()
        self.assertIn("XR session became nonvisible", " ".join(analyze(self.path)["errors"]))

    def test_delayed_visibility_does_not_validate_earlier_phase(self):
        self.lines = [line for line in self.lines if "SESSION_STATE_CHANGED" not in line]
        self.line(75, "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 5")
        self.save_log()
        self.assertIn("XR session not visible at measurement start", " ".join(analyze(self.path)["errors"]))

    def test_gpu_source_mode_mismatch_fails(self):
        self.lines = [line.replace("source_fixed_scale=0.5", "source_fixed_scale=0") for line in self.lines]
        self.save_log()
        self.assertIn("incompatible mode/source state", " ".join(analyze(self.path)["errors"]))

    def test_transient_fallback_between_probe_samples_fails(self):
        self.line(134, "[Frame Perf] state frame=304 frame_enabled=true crop_setting=true reduce_setting=true "
                  "native_fix=false sceneview=false splitscreen=false result=baseline cropped=0 reduced=0 reason=full-view-fallback resource_failed=false")
        self.save_log()
        self.assertIn("applied state changed", " ".join(analyze(self.path)["errors"]))

    def test_boundary_reports_do_not_satisfy_gpu_requirement(self):
        self.lines = [line.replace("interval_s=2.001", "interval_s=20.001") for line in self.lines]
        self.save_log()
        self.assertFalse(analyze(self.path)["passed"])
        self.assertTrue(analyze(self.path, allow_no_gpu=True)["passed"])

    def test_missing_repeated_phase_is_not_replaced_by_first_occurrence(self):
        (self.path / "codex-frame-benchmark-05-crop.txt").unlink()
        self.assertFalse(analyze(self.path)["passed"])

    def test_capture_file_validation_is_not_visual_acceptance(self):
        self.enable_captures()
        result = analyze(self.path)
        self.assertTrue(result["passed"], result["errors"])
        self.assertEqual(result["capture_files_validated"], 6)
        self.assertFalse(result["image_correctness_verified"])

    def test_capture_request_reset_on_error_is_not_success(self):
        self.enable_captures()
        self.lines = [line for line in self.lines if "[Frame Capture]" not in line]
        self.line(38.5, "[Frame Capture] failed reason=unsupported-output")
        self.save_log()
        result = analyze(self.path)
        self.assertFalse(result["passed"])
        self.assertEqual(result["capture_files_validated"], 0)

    def test_capture_wrong_mode_rejected(self):
        self.enable_captures()
        self.lines = [line.replace("reduced=3", "reduced=0") if "[Frame Capture]" in line else line for line in self.lines]
        self.save_log()
        self.assertIn("capture source mode/masks/dimensions mismatch", " ".join(analyze(self.path)["errors"]))

    def test_truncated_capture_bmp_rejected(self):
        self.enable_captures()
        (self.path / "capture-1.bmp").write_bytes(b"BM")
        self.assertIn("truncated capture BMP header", " ".join(analyze(self.path)["errors"]))

    def test_capture_event_during_measurement_rejected(self):
        self.enable_captures()
        path = self.path / "codex-frame-benchmark-events.txt"
        events = path.read_text()
        epoch = (self.base + timedelta(seconds=39)).timestamp()
        path.write_text(events.replace(f"epoch={epoch} capture_finished", f"epoch={epoch+15} capture_finished"))
        self.assertIn("capture window overlaps measurement", " ".join(analyze(self.path)["errors"]))

    def _native_fixture(self):
        run_path = self.path / "summary.json"
        run = json.loads(run_path.read_text())
        run["segments"] = ["baseline", "crop"]
        run["native_stereo_fix"] = True
        run_path.write_text(json.dumps(run))
        for segment, mode in ((1, "baseline"), (2, "crop")):
            path = self.path / f"codex-frame-benchmark-{segment:02d}-{mode}.txt"
            path.write_text(path.read_text() + "native_stereo_fix=true\n")
        for segment, mode in enumerate(("reduced", "fixed", "crop", "native_scaled"), 3):
            for suffix in (".txt", ".csv"):
                (self.path / f"codex-frame-benchmark-{segment:02d}-{mode}{suffix}").unlink()
        self.lines = [line.replace("native_fix=false", "native_fix=true").replace("native_fix=0", "native_fix=1")
                      .replace("view=100,0,", "view=0,0,") for line in self.lines]
        events = self.path.joinpath("codex-frame-benchmark-events.txt").read_text()
        events = "\n".join(line + " native_stereo_fix=true" if "state=warming" in line and "segment=" in line else line
                              for line in events.splitlines())
        self.path.joinpath("codex-frame-benchmark-events.txt").write_text(events)
        self.save_log()

    def test_native_fix_rejects_reduced_fixed_and_native_scaled(self):
        path = self.path / "summary.json"
        run = json.loads(path.read_text())
        run["native_stereo_fix"] = True
        path.write_text(json.dumps(run))
        result = analyze(self.path)
        self.assertFalse(result["passed"])
        self.assertIn("only baseline and crop", " ".join(result["errors"]))

    def test_native_fix_baseline_and_crop_are_accepted(self):
        self._native_fixture()
        result = analyze(self.path)
        self.assertTrue(result["passed"], result["errors"])

    def test_native_fix_rejects_off_state_and_timing_evidence(self):
        self._native_fixture()
        self.lines = [line.replace("native_fix=true", "native_fix=false").replace("native_fix=1", "native_fix=0")
                      for line in self.lines]
        self.save_log()
        result = analyze(self.path)
        self.assertFalse(result["passed"])
        self.assertIn("submission mode/association/dimensions mismatch", " ".join(result["errors"]))
        self.assertIn("incompatible mode/source state", " ".join(result["errors"]))

    def test_native_fix_rejects_unknown_scene_dimensions(self):
        self._native_fixture()
        self.lines = [line.replace("scene=200x100 output=200x100", "scene=0x0 output=200x100") for line in self.lines]
        self.save_log()
        result = analyze(self.path)
        self.assertFalse(result["passed"])
        self.assertIn("submission mode/association/dimensions mismatch", " ".join(result["errors"]))


if __name__ == "__main__":
    unittest.main()
