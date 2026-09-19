"""Summarize Frame Perf diagnostics without estimating GPU time or FPS."""

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path


FIELD = re.compile(r"(?P<key>[A-Za-z_][\w-]*)=(?P<value>[^\s]+)")
FRAME_POSE = re.compile(r"\bframe=(?P<frame>-?\d+)\s+pose=(?P<pose>-?\d+)")


def _records(path):
    records = []
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            marker = line.find("[Frame Perf]")
            if marker < 0:
                continue
            text = line[marker + len("[Frame Perf]"):].strip()
            kind = text.split(maxsplit=1)[0] if text else ""
            values = {m.group("key"): m.group("value").rstrip(",;")
                      for m in FIELD.finditer(text)}
            records.append((kind, values, text))
    return records


def _timing_records(path):
    records = []
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            marker = line.find("[Frame Timing]")
            if marker < 0:
                continue
            text = line[marker + len("[Frame Timing]"):].strip()
            values = {m.group("key"): m.group("value").rstrip(",;")
                      for m in FIELD.finditer(text)}
            if values.get("scope") == "uevr_openxr_native_submission" and values.get("unit_ms") == "1":
                records.append(values)
    return records


def _enabled(values, key):
    value = values.get(key, "").lower()
    return value == "true" or (value.isdigit() and int(value) != 0)


def _matched_submit(values):
    try:
        return (_enabled(values, "matched") and int(values.get("projections", 0)) == 3
                and int(values.get("rects", 0)) == 3)
    except ValueError:
        return False


TIMING_FIELDS = {
    "gpu_copy_avg": "gpu_n", "gpu_copy_max": "gpu_n",
    "gpu_mask_avg": "gpu_n", "gpu_mask_max": "gpu_n",
    "cpu_image_fence_wait_avg": "cpu_n", "cpu_image_fence_wait_max": "cpu_n",
    "cpu_record_execute_avg": "cpu_n", "cpu_record_execute_max": "cpu_n",
}


def _timing_summary(path):
    groups = {}
    for values in _timing_records(path):
        group = "true" if _enabled(values, "mask_enabled") else "false"
        result = groups.setdefault(group, {"records": 0, "gpu_n": 0, "cpu_n": 0,
                                           "max_unavailable_images": 0, "pending_skips": 0,
                                           "invalid": 0, "weighted": {}, "maxima": {}})
        result["records"] += 1
        for counter in ("gpu_n", "cpu_n", "pending_skips", "invalid"):
            try:
                result[counter] += int(values.get(counter, 0))
            except ValueError:
                pass
        try:
            result["max_unavailable_images"] = max(
                result["max_unavailable_images"], int(values.get("gpu_unavailable_images", 0)))
        except ValueError:
            pass
        for field, sample_field in TIMING_FIELDS.items():
            try:
                samples = int(values.get(sample_field, 0))
                value = float(values[field])
            except (KeyError, ValueError):
                continue
            if samples > 0:
                if field.endswith("_max"):
                    result["maxima"][field] = max(result["maxima"].get(field, value), value)
                else:
                    total, count = result["weighted"].get(field, (0.0, 0))
                    result["weighted"][field] = (total + value * samples, count + samples)
    for result in groups.values():
        result["weighted"] = {
            field: (total / count if count else None)
            for field, (total, count) in result["weighted"].items()
        }
    return groups


def summarize(path):
    records = _records(path)
    submits = [(v, t) for k, v, t in records if k == "submit"]
    geometry = [(v, t) for k, v, t in records if k == "geometry"]

    groups = Counter(
        ("native" if _enabled(v, "native_fix") else "non-native",
         "cropped" if _enabled(v, "cropped") else "full",
         "reduced" if _enabled(v, "reduced") else "full")
        for v, _ in submits
    )
    deltas = []
    for values, _ in submits:
        match = FRAME_POSE.search(" ".join(f"{k}={v}" for k, v in values.items()))
        if match:
            deltas.append(int(match.group("frame")) - int(match.group("pose")))

    matched_geometry = sum(_matched_submit(v) for v, _ in submits)
    paired = matched_geometry

    losses = 0
    rejects = 0
    for _, _, text in records:
        lowered = text.lower()
        if re.search(r"\b(?:loss|lost|dropped|drop)=?true\b|\bframe\s+loss\b", lowered):
            losses += 1
        if re.search(r"\b(?:reject|rejected)=?true\b|\breject(?:ed|ion)?\b", lowered):
            rejects += 1

    return {
        "name": str(path),
        "submits": len(submits),
        "native": sum(_enabled(v, "native_fix") for v, _ in submits),
        "cropped": sum(_enabled(v, "cropped") for v, _ in submits),
        "reduced": sum(_enabled(v, "reduced") for v, _ in submits),
        "groups": groups,
        "geometry": len(geometry),
        "matched_geometry": matched_geometry,
        "paired_both_eye_matched": paired,
        "deltas": deltas,
        "losses": losses,
        "rejects": rejects,
        "timing": _timing_summary(path),
    }


def report(summary):
    deltas = summary["deltas"]
    delta_text = "none" if not deltas else ",".join(map(str, sorted(set(deltas))))
    groups = ";".join(
        f"{native}/{crop}/{reduce_}={count}"
        for (native, crop, reduce_), count in sorted(summary["groups"].items())
    ) or "none"
    timing = ";".join(
        f"mask={mask} gpu_n={data['gpu_n']} cpu_n={data['cpu_n']} "
        f"gpu_copy_avg={data['weighted'].get('gpu_copy_avg')} "
        f"gpu_mask_avg={data['weighted'].get('gpu_mask_avg')} "
        f"gpu_copy_max={data['maxima'].get('gpu_copy_max')} "
        f"gpu_mask_max={data['maxima'].get('gpu_mask_max')} "
        f"max_unavailable_images={data['max_unavailable_images']}"
        for mask, data in sorted(summary["timing"].items())
    ) or "none"
    return (f"{summary['name']}: submits={summary['submits']} "
            f"native={summary['native']} cropped={summary['cropped']} "
            f"reduced={summary['reduced']} groups={groups} "
            f"geometry={summary['geometry']} matched_geometry={summary['matched_geometry']} "
            f"paired_both_eye_matched={summary['paired_both_eye_matched']} "
            f"frame_pose_delta={delta_text} losses={summary['losses']} rejects={summary['rejects']} "
            f"timing={timing}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--require-matched", action="store_true",
                        help="fail unless at least one frame has matched eye 0 and eye 1 records")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args(argv)
    summaries = [summarize(path) for path in args.logs]
    if args.json:
        json_summaries = []
        for summary in summaries:
            item = dict(summary)
            item["groups"] = {"/".join(key): value for key, value in summary["groups"].items()}
            json_summaries.append(item)
        print(json.dumps(json_summaries, sort_keys=True))
    else:
        for summary in summaries:
            print(report(summary))
    if args.require_matched and any(not s["paired_both_eye_matched"] for s in summaries):
        missing = ", ".join(s["name"] for s in summaries if not s["paired_both_eye_matched"])
        print(f"ASSERTION FAILED: no paired both-eye matched records in {missing}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
