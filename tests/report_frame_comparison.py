"""Describe same-session benchmark outcomes without treating validation as a speedup."""
import argparse
import csv
from datetime import datetime
import json
import math
from pathlib import Path
import re

from analyze_frame_benchmark import fields, metadata, percentile


def relation(value, controls):
    if value < min(controls):
        return "below_all"
    if value > max(controls):
        return "above_all"
    return "within_control_range"


def compare_phase(phase, phases, control, eligible):
    if phase["mode"] == control:
        return {"outcome": "control_observation"}
    controls = [row for row in phases if row["mode"] == control]
    before = [row for row in controls if row["segment"] < phase["segment"]]
    after = [row for row in controls if row["segment"] > phase["segment"]]
    references = before[-1:] + after[:1]
    result = {"control_segments": [row["segment"] for row in references], "bracketed": bool(before and after)}
    if not references or not eligible:
        return dict(result, outcome="descriptive_only_missing_controls_or_failed_run_evidence")
    for key in ("median_ms", "p95_ms", "over_two_intervals_fraction"):
        result[key] = relation(phase[key], [row[key] for row in references])
    # These are observed comparisons, not significance tests or causal attribution.
    if result["p95_ms"] == "above_all" or result["over_two_intervals_fraction"] == "above_all":
        result["outcome"] = "observed_tail_regression"
    elif result["median_ms"] == result["p95_ms"] == "below_all":
        result["outcome"] = "observed_lower_median_and_p95_quality_unverified"
    else:
        result["outcome"] = "mixed_or_within_observed_control_range"
    return result


def geometry_summary(log, phase, width, height):
    pairs = {}
    density = [[], []]
    for stamp, item in log:
        if not phase["measure_start_epoch"] <= stamp < phase["end_epoch"]:
            continue
        eye = int(item["eye"])
        crop = tuple(map(int, item["crop"].split(",")))
        view = tuple(map(int, item["view"].split(",")))
        pairs.setdefault(item["frame"], {})[eye] = view[2] * view[3]
        if phase["mode"] != "baseline" and crop[2] > 0 and crop[3] > 0:
            density[eye].append((view[2] / crop[2], view[3] / crop[3]))
    areas = [sum(pair.values()) / (2 * width * height) for pair in pairs.values() if len(pair) == 2]
    return {"paired_probes": len(areas),
            "active_allocation_area_fraction": {"min": min(areas), "median": percentile(areas, .5), "max": max(areas)} if areas else None,
            "source_pixels_per_output_crop_pixel": [
                {"eye": eye, "horizontal_min": min(x for x, _ in values), "horizontal_max": max(x for x, _ in values),
                 "vertical_min": min(y for _, y in values), "vertical_max": max(y for _, y in values)}
                for eye, values in enumerate(density) if values]}


def pacing_summary(log, phase, fixed_scale):
    result = {"epochs": [], "excluded_boundary_reports": 0, "excluded_setting_reports": 0,
              "settings_match": "requested_only_not_rendered_source", "slow_count_threshold": "2x_runtime_predicted_period_or_20ms",
              "limitations": "CPU wall time inside xrWaitFrame/xrBeginFrame/xrEndFrame only; excludes UEVR locks, layer preparation, "
                  "swapchain calls and GPU work. Blocking can reflect runtime/GPU waits. No p95 is recoverable from aggregates; "
                  "slow_n includes unlogged slow calls. Different settings epochs remain separate."}
    expected = {"frame_enabled": "1", "crop_setting": str(int(phase["mode"] != "baseline")),
                "reduce_setting": str(int(phase["mode"] in ("fixed", "reduced"))),
                "native_fix": "0", "capture": "0", "afr": "0", "settings": "requested"}
    scale = fixed_scale if phase["mode"] == "fixed" else 0
    groups = {}
    start, end = phase["measure_start_epoch"], phase["end_epoch"]
    for stamp, item in log:
        interval_start = int(item["wall_start_us"]) / 1e6
        interval_end = int(item["wall_end_us"]) / 1e6
        if interval_end <= start or interval_start >= end:
            continue
        max_start = int(item["max_wall_start_us"]) / 1e6
        max_end = int(item["max_wall_end_us"]) / 1e6
        if not start <= interval_start <= interval_end <= end or not start <= max_start <= max_end <= end:
            result["excluded_boundary_reports"] += 1
            continue
        if any(item.get(key) != value for key, value in expected.items()) or abs(float(item["fixed_scale"]) - scale) > 1e-5:
            result["excluded_setting_reports"] += 1
            continue
        count = int(item["n"])
        if count <= 0 or item["call"] not in ("xrWaitFrame", "xrBeginFrame", "xrEndFrame"):
            raise ValueError("Invalid runtime pacing call/count")
        # Preserve every requested settings epoch, even when the same mode
        # appears twice or changes away and back during a measurement window.
        key = (int(item["epoch"]), item["vd_dummy"], item["sync_stage"])
        group = groups.setdefault(key, {"epoch": key[0], "vd_dummy": key[1], "sync_stage": key[2], "calls": {}})
        call = group["calls"].setdefault(item["call"], {"reports": 0, "n": 0, "weighted_ms": 0,
            "max_ms": 0, "slow_n": 0, "slow_logged": 0, "failures": 0, "retries": 0,
            "transition_skips_reported": [], "report_intervals": []})
        call["reports"] += 1
        call["n"] += count
        call["weighted_ms"] += float(item["avg_ms"]) * count
        if not call["max_ms"] or float(item["max_ms"]) > call["max_ms"]:
            call["max_ms"] = float(item["max_ms"])
            call["max_event"] = {key: item[key] for key in ("max_frame", "max_frame_kind", "max_retry", "max_result",
                "max_wall_start_us", "max_wall_end_us", "max_predicted_period_ms")}
        for counter in ("slow_n", "slow_logged", "failures", "retries"):
            call[counter] += int(item[counter])
        call["transition_skips_reported"].append(int(item["transition_skips"]))
        call["report_intervals"].append([interval_start, interval_end])
    for group in groups.values():
        for call in group["calls"].values():
            call["mean_ms"] = call.pop("weighted_ms") / call["n"]
        group["missing_calls"] = sorted({"xrWaitFrame", "xrBeginFrame", "xrEndFrame"} - group["calls"].keys())
        result["epochs"].append(group)
    result["available"] = bool(groups)
    return result


def report_run(directory, refresh_hz, control="baseline"):
    directory = Path(directory)
    run = json.loads((directory / "summary.json").read_text(encoding="utf-8-sig"))
    evidence = json.loads((directory / "analysis.json").read_text(encoding="utf-8-sig"))
    result = {"archive": str(directory.resolve()), "game": run["game"], "backend_sha256": run["backend_sha256"],
              "resolution_scale": run["scale"], "fixed_scale": run["fixed_scale"], "refresh_hz_assumed": refresh_hz,
              "run_passed": run["passed"], "run_failure": run.get("failure"), "evidence_passed": evidence["passed"],
              "evidence_errors": evidence.get("errors", []), "control": control,
              "image_correctness_verified": False, "phases": [],
              "limitations": ["Refresh rate is an explicit caller input, not a measured headset cadence.",
                  "Engine delta values can include engine pacing/smoothing; they are not GPU durations or displayed frame times.",
                  "Observed metric ordering is not a significance test, causal attribution, or universal optimization claim.",
                  "Sequential controls bound some drift, but scene/runtime changes can still confound comparisons.",
                  "Equal active pixel area does not imply equal horizontal/vertical sampling or equal visual quality.",
                  "Geometry samples are sparse; captures validate files, not moving stereo image quality.",
                  "Archives are evaluated separately; different runs are not ranked as controlled scale comparisons."]}
    log = []
    pacing_log = []
    for line in (directory / "log.txt").read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if "[Frame Perf] candidate " in line:
            stamp = re.match(r"\[([^]]+)\]", line)
            log.append((datetime.fromisoformat(stamp[1]).timestamp(), fields(line)))
        elif "[Frame Pacing] kind=summary scope=openxr_cpu_call " in line:
            stamp = re.match(r"\[([^]]+)\]", line)
            pacing_log.append((datetime.fromisoformat(stamp[1]).timestamp(), fields(line)))
    for index, mode in enumerate(run["segments"], 1):
        stem = directory / f"codex-frame-benchmark-{index:02d}-{mode}"
        data = metadata(stem.with_suffix(".txt"))
        with stem.with_suffix(".csv").open(encoding="utf-8-sig", newline="") as file:
            samples = [float(row["delta_seconds"]) for row in csv.DictReader(file)]
        start, end = float(data["measure_start_epoch"]), float(data["end_epoch"])
        if not samples or end <= start or len(samples) != int(data["samples"]) or any(not math.isfinite(x) or x <= 0 for x in samples):
            raise ValueError(f"Malformed samples/duration for {stem.name}")
        stutters = sum(value > 2 / refresh_hz for value in samples)
        phase = {"segment": index, "mode": mode, "measure_start_epoch": start, "end_epoch": end,
                 "wall_duration_seconds": end - start, "engine_delta_sum_seconds": sum(samples), "samples": len(samples),
                 "ticks_per_wall_second": len(samples) / (end - start), "mean_ms": sum(samples) / len(samples) * 1000,
                 "median_ms": percentile(samples, .5) * 1000, "p95_ms": percentile(samples, .95) * 1000,
                 "max_ms": max(samples) * 1000, "over_two_intervals_threshold_ms": 2000 / refresh_hz,
                 "over_two_intervals_count": stutters, "over_two_intervals_fraction": stutters / len(samples)}
        phase["geometry"] = geometry_summary(log, phase, int(data["width"]), int(data["height"]))
        phase["runtime_pacing"] = pacing_summary(pacing_log, phase, float(run["fixed_scale"]))
        result["phases"].append(phase)
    for phase in result["phases"]:
        phase["same_session_comparison"] = compare_phase(phase, result["phases"], control, run["passed"] and evidence["passed"])
    return result


def markdown(report):
    lines = [f"Archive: {report['archive']}",
             f"Run passed: {report['run_passed']}; evidence passed: {report['evidence_passed']}; control: {report['control']}; assumed refresh: {report['refresh_hz_assumed']:g} Hz.",
             "", "| Phase | Mode | Seconds / samples | Median ms | p95 ms | >2 intervals | Same-session outcome |",
             "|---|---|---:|---:|---:|---:|---|"]
    for row in report["phases"]:
        lines.append(f"| {row['segment']} | {row['mode']} | {row['wall_duration_seconds']:g} / {row['samples']} | "
                     f"{row['median_ms']:.3f} | {row['p95_ms']:.3f} | {row['over_two_intervals_count']} ({row['over_two_intervals_fraction']:.1%}) | "
                     f"{row['same_session_comparison']['outcome']} |")
    if any(row["runtime_pacing"]["available"] for row in report["phases"]):
        lines.extend(["", "OpenXR CPU call wall times (requested settings; separate epochs; fully contained reports):", "",
                      "| Phase / epoch | Call | Calls | Mean ms | Max ms | Slow calls | Failures / retries |",
                      "|---|---|---:|---:|---:|---:|---:|"])
        for row in report["phases"]:
            for group in row["runtime_pacing"]["epochs"]:
                for name, call in group["calls"].items():
                    lines.append(f"| {row['segment']} / {group['epoch']} | {name} | {call['n']} | {call['mean_ms']:.3f} | "
                                 f"{call['max_ms']:.3f} | {call['slow_n']} | {call['failures']} / {call['retries']} |")
        lines.extend(["", "Slow calls use twice the runtime-predicted period, or 20 ms if unavailable. These are CPU call durations, "
                      "not whole-frame costs or GPU work. Requested settings do not prove rendered-source matching. "
                      "Boundary/setting exclusions and per-call coverage are recorded in JSON."])
    else:
        lines.extend(["", "OpenXR CPU pacing: no wholly contained reports with matching requested settings are available."])
    lines.extend(["", *[f"- {limitation}" for limitation in report["limitations"]]])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", type=Path, nargs="+")
    parser.add_argument("--refresh-hz", type=float, required=True)
    parser.add_argument("--control", choices=("baseline", "crop", "reduced", "fixed"), default="baseline")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if not math.isfinite(args.refresh_hz) or args.refresh_hz <= 0:
        parser.error("--refresh-hz must be finite and positive")
    reports = [report_run(path, args.refresh_hz, args.control) for path in args.archives]
    print(json.dumps(reports, indent=2) if args.json else "\n\n".join(map(markdown, reports)))


if __name__ == "__main__":
    main()
