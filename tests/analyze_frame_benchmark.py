"""Validate archived benchmark phases; submission GPU scopes are not game GPU time."""
import argparse
import csv
from datetime import datetime
import json
import math
from pathlib import Path
import re
import struct


def fields(text):
    return dict(re.findall(r"(\w+)=([^\s]+)", text))


def metadata(path):
    return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8-sig").splitlines() if "=" in line)


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def validate_capture(log, events, index, mode, data, scale, cropped, reduced):
    requests = [fields(line) for line in events.splitlines() if "capture_requested " in line and fields(line).get("segment") == str(index)]
    finishes = [fields(line) for line in events.splitlines() if "capture_finished " in line and fields(line).get("segment") == str(index)]
    if len(requests) != 1 or len(finishes) != 1:
        raise ValueError("capture requires exactly one request and finish event")
    begin, end = float(requests[0]["epoch"]), float(finishes[0]["epoch"])
    if not begin <= end <= float(data["start_epoch"]) or end >= float(data["measure_start_epoch"]):
        raise ValueError("capture window overlaps measurement or follows restarted warmup")
    # Lua event epochs have one-second precision. Native completion can occur
    # anywhere in the second in which Lua observes request reset.
    rows = [(stamp, line, item) for stamp, line, item in log if begin <= stamp < end + 1 and "[Frame Capture] " in line]
    queued = [row for row in rows if "[Frame Capture] queued " in row[1]]
    complete = [row for row in rows if "[Frame Capture] complete " in row[1]]
    if len(queued) != 1 or len(complete) != 1 or any("[Frame Capture] failed " in line for _, line, _ in rows):
        raise ValueError("capture request/reset lacks exactly one successful native queue and completion")
    first, last = queued[0], complete[0]
    if not first[0] <= last[0] < float(data["measure_start_epoch"]):
        raise ValueError("native capture completion not before measurement")
    dimensions = f"{2 * int(data['width'])}x{int(data['height'])}"
    for _, _, item in (first, last):
        if any(item.get(key) != value for key, value in {
            "cropped": str(cropped), "reduced": str(reduced), "size": dimensions, "mask_drawn": "3"}.items()) or \
                item.get("suppressed") not in ("0", "false") or \
                item.get("resolved") not in (("0", "false") if mode == "baseline" else ("1", "true")) or \
                abs(float(item["fixed_scale"]) - scale) > 1e-5:
            raise ValueError("capture source mode/masks/dimensions mismatch")
    if any(first[2].get(key) != last[2].get(key) for key in ("frame", "pose", "generation")):
        raise ValueError("capture queue/completion identity mismatch")
    name = re.search(r'file="([^"]+)"', last[1])
    if not name:
        raise ValueError("capture completion has no file")
    path = Path(name[1])
    with path.open("rb") as file:
        header = file.read(54)
    if len(header) != 54:
        raise ValueError("truncated capture BMP header")
    signature, size, reserved1, reserved2, offset = struct.unpack_from("<2sIHHI", header)
    info_size, width, height, planes, bits, compression, image_size = struct.unpack_from("<IiiHHII", header, 14)
    expected_width, expected_height = 2 * int(data["width"]), int(data["height"])
    if (signature, reserved1, reserved2, offset, info_size, width, height, planes, bits, compression, image_size) != \
            (b"BM", 0, 0, 54, 40, expected_width, -expected_height, 1, 32, 0, expected_width * expected_height * 4) or \
            size != 54 + image_size or path.stat().st_size != size:
        raise ValueError("capture BMP header/dimensions/file length mismatch")
    return {"file": str(path), "frame": first[2]["frame"], "pose": first[2]["pose"],
            "generation": first[2]["generation"], "requested_epoch": begin, "finished_epoch": end,
            "native_complete_epoch": last[0], "bmp_valid": True, "image_correctness_verified": False}


def analyze(directory, allow_no_gpu=False):
    directory = Path(directory)
    result = {"passed": False, "errors": [], "phases": [], "submit_samples": 0, "gpu_samples": 0,
              "gpu_requirement_waived": allow_no_gpu,
              "native_log_timezone": str(datetime.now().astimezone().tzinfo),
              "limitations": ["Native timestamps use this host's local timezone.",
                  "GPU statistics are weighted report means and maxima, not per-frame percentiles.",
                  "Only reports wholly within the phase, with frame ranges bracketed by measured submits, are included.",
                  "Engine deltas may include engine smoothing/clamping; they are not GPU timings.",
                  "Sparse probes cannot verify every frame or image quality."]}
    run = json.loads((directory / "summary.json").read_text(encoding="utf-8-sig"))
    log = []
    visibility = []
    for line in (directory / "log.txt").read_text(encoding="utf-8-sig", errors="replace").splitlines():
        stamp = re.match(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]", line)
        if not stamp:
            continue
        epoch = datetime.fromisoformat(stamp[1]).timestamp()
        log.append((epoch, line, fields(line)))
        state = re.search(r"XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED (\d+)", line)
        if state:
            visibility.append((epoch, int(state[1])))
    events = (directory / "codex-frame-benchmark-events.txt").read_text(encoding="utf-8-sig")
    capture_required = run.get("capture_frames", False)
    if capture_required:
        if any("[Frame Capture] failed " in line for _, line, _ in log):
            result["errors"].append("native capture failure recorded")
        if sum("[Frame Capture] complete " in line for _, line, _ in log) != len(run["segments"]):
            result["errors"].append("native capture completion count differs from phase count")
    for index, mode in enumerate(run["segments"], 1):
        phase = {"segment": index, "mode": mode, "errors": []}
        result["phases"].append(phase)
        errors = phase["errors"]
        stem = directory / f"codex-frame-benchmark-{index:02d}-{mode}"
        try:
            data = metadata(stem.with_suffix(".txt"))
            start, end = float(data["measure_start_epoch"]), float(data["end_epoch"])
            phase.update(measure_start_epoch=start, end_epoch=end)
            if data.get("state") != "complete" or int(data["segment"]) != index or data["mode"] != mode:
                errors.append("phase identity/state mismatch")
            measure_seconds = data.get("measure_seconds", run.get("measure_seconds"))
            if end <= start or end - start < float(measure_seconds):
                errors.append("incomplete measurement interval")
            if start < float(data["start_epoch"]) + float(data["warmup_seconds"]):
                errors.append("measurement overlaps warmup")
            if not re.search(rf"segment={index}\s+phase={mode}\s+state=warming", events):
                errors.append("missing phase start event")
            with stem.with_suffix(".csv").open(encoding="utf-8-sig", newline="") as file:
                deltas = [float(row["delta_seconds"]) for row in csv.DictReader(file)]
            if not deltas or any(not math.isfinite(value) or value <= 0 for value in deltas):
                errors.append("missing/invalid engine delta samples")
            elif len(deltas) != int(data["samples"]):
                errors.append("CSV sample count differs from phase summary")
            else:
                phase["engine_delta_seconds"] = {"n": len(deltas), "median": percentile(deltas, .5),
                    "p95": percentile(deltas, .95), "max": max(deltas)}
            before = [state for stamp, state in visibility if stamp <= start]
            if not before or before[-1] not in (5, 6):
                errors.append("XR session not visible at measurement start")
            if any(state not in (5, 6) for stamp, state in visibility if start < stamp < end):
                errors.append("XR session became nonvisible during measurement")
            rows = [(stamp, line, item) for stamp, line, item in log if start <= stamp < end]
            submits = [item for _, line, item in rows if "[Frame Perf] submit " in line]
            phase["submit_samples"] = len(submits)
            result["submit_samples"] += len(submits)
            if not submits:
                errors.append("no measured submission probes")
            width, height = int(data["width"]), int(data["height"])
            dimensions = f"{2 * width}x{height}"
            crop = int(mode != "baseline")
            reduce = int(mode in ("reduced", "fixed"))
            scale = float(run["fixed_scale"]) if mode == "fixed" else 0.0
            expected_cropped = 3 if crop else 0
            expected_reduced = 3 if reduce and scale != 1 else 0
            if capture_required:
                try:
                    phase["capture"] = validate_capture(log, events, index, mode, data, scale, expected_cropped, expected_reduced)
                except (OSError, ValueError, KeyError, struct.error) as error:
                    errors.append(f"capture evidence invalid: {error}")
            phase["full_eye_dimensions"] = [width, height]
            for item in submits:
                if any(item.get(key) != value for key, value in {
                    "prepared": "true", "matched": "true", "projections": "3", "rects": "3",
                    "native_fix": "false", "sceneview": "false", "splitscreen": "false",
                    "cropped": str(expected_cropped), "reduced": str(expected_reduced),
                    "scene": dimensions, "output": dimensions}.items()):
                    errors.append(f"submission mode/association/dimensions mismatch at frame {item.get('frame')}")
            if any("[Frame Perf] resolve rejected " in line or
                   ("[Frame Perf] state " in line and item.get("result") in ("failed", "suppressed"))
                   for _, line, item in rows):
                errors.append("resolve rejection/suppression during measurement")
            for _, line, item in rows:
                if "[Frame Perf] state " in line and any(item.get(key) != value for key, value in {
                    "result": "resolved" if crop else "baseline", "cropped": str(expected_cropped),
                    "reduced": str(expected_reduced), "crop_setting": "true" if crop else "false",
                    "reduce_setting": "true" if reduce else "false", "resource_failed": "false"}.items()):
                    errors.append("applied state changed during measurement")
            # The source allocations stay full size; active rectangles must obey the chosen policy.
            candidates = {(item.get("frame"), item.get("eye")): item for _, line, item in rows
                          if "[Frame Perf] candidate " in line}
            active = set()
            float_scale = struct.unpack("f", struct.pack("f", scale))[0]
            for item in submits:
                for eye in range(2):
                    candidate = candidates.get((item["frame"], str(eye)))
                    if candidate is None:
                        errors.append(f"missing eye {eye} rectangle for frame {item['frame']}")
                        continue
                    view = tuple(map(int, candidate["view"].split(",")))
                    rect = tuple(map(int, candidate["crop"].split(",")))
                    active.add((eye, *view))
                    extent = (width, height)
                    if mode == "reduced":
                        extent = rect[2:]
                    elif mode == "fixed":
                        extent = (math.ceil(width * float_scale), math.ceil(height * float_scale))
                    if view != (eye * width, 0, *extent) or (crop and candidate.get("fallback") != "0"):
                        errors.append(f"active rectangle/fallback mismatch at frame {item['frame']} eye {eye}")
            phase["observed_active_rectangles"] = sorted(active)
            timing = []
            frame_numbers = [int(item["frame"]) for item in submits]
            if any(current <= previous for previous, current in zip(frame_numbers, frame_numbers[1:])):
                errors.append("submission frame counter reset/repeated during measurement")
            for stamp, line, item in rows:
                if "[Frame Timing] " not in line:
                    continue
                # Exclude boundary reports and delayed samples whose source-frame span
                # is not safely inside the observed measurement frame range.
                if stamp - float(item["interval_s"]) < start or not frame_numbers:
                    continue
                if not (min(frame_numbers) <= int(item["frame_first"]) <= int(item["frame_last"]) <= max(frame_numbers)):
                    continue
                expected = {"scope": "uevr_openxr_submission", "native_fix": "0", "mask_enabled": "1",
                    "crop_setting": str(crop), "reduce_setting": str(reduce), "cropped": str(expected_cropped),
                    "reduced": str(expected_reduced), "lost": "0", "resolved": str(crop), "suppressed": "0", "mask_drawn": "3"}
                if any(item.get(key) != value for key, value in expected.items()) or any(
                    abs(float(item[key]) - scale) > 1e-5 for key in ("fixed_scale", "source_fixed_scale")):
                    errors.append("timing report contains incompatible mode/source state")
                    continue
                if int(item["gpu_n"]) > 0:
                    timing.append(item)
            gpu_n = sum(int(item["gpu_n"]) for item in timing)
            phase["gpu_samples"] = gpu_n
            phase["gpu_reports"] = len(timing)
            phase["timing_report_evidence"] = [{key: item.get(key) for key in (
                "epoch", "interval_s", "frame_first", "frame_last", "gpu_n", "invalid",
                "stale_skips", "pending_skips", "gpu_unavailable_images")} for item in timing]
            result["gpu_samples"] += gpu_n
            if not gpu_n and not allow_no_gpu:
                errors.append("no fully contained, correctly keyed GPU timing reports")
            if gpu_n:
                phase["submission_gpu_ms"] = {
                    scope: {"mean": sum(float(item[f"gpu_{scope}_avg"]) * int(item["gpu_n"]) for item in timing) / gpu_n,
                            "max": max(float(item[f"gpu_{scope}_max"]) for item in timing)}
                    for scope in ("pre", "source_copy", "reconstruct", "additional", "mask")}
        except (OSError, ValueError, KeyError, TypeError) as error:
            errors.append(f"incomplete/malformed phase evidence: {error}")
        result["errors"].extend(f"segment {index} ({mode}): {error}" for error in errors)
    result["requested_resolution_scale"] = run.get("scale")
    result["capture_files_validated"] = sum("capture" in phase for phase in result["phases"])
    result["image_correctness_verified"] = False
    dimensions = sorted({tuple(phase["full_eye_dimensions"]) for phase in result["phases"] if "full_eye_dimensions" in phase})
    result["observed_full_eye_dimensions"] = dimensions
    if len(dimensions) > 1:
        result["errors"].append("full-eye dimensions changed between phases")
    result["resolution_scale_verification"] = "API full-eye dimensions cross-checked with scene/output; XR recommended base not logged"
    result["passed"] = not result["errors"] and bool(result["phases"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    parser.add_argument("--allow-no-gpu", action="store_true", help="Explicitly waive missing GPU report evidence")
    args = parser.parse_args()
    try:
        result = analyze(args.run_directory, args.allow_no_gpu)
    except (OSError, ValueError, KeyError, TypeError) as error:
        result = {"passed": False, "errors": [f"incomplete/malformed run evidence: {error}"]}
    (args.run_directory / "analysis.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Benchmark evidence: passed={result['passed']}; errors={len(result['errors'])}; see analysis.json")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
