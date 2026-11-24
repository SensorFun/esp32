#!/usr/bin/env python3
"""Run the firmware motion detector over MJPEG AVIs on a virtual clock."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
import re
from pathlib import Path
from functools import partial
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "projects/webrtc/main"
DEFAULT_CLIP = ROOT / "tests/data/motion/camera-2026-11-23_14-59-34.avi"
# Motion settings returned by motion_get. Recording/UI fields do not affect the detector.
RANGES = {
    "blur": (0, 2, 1), "min_speed": (0.25, 40, 0.25),
    "min_texture": (0, 50, 0.5), "min_area": (1, 30, 0.5), "trigger_seconds": (0.1, 10.9, 0.1),
}
# Settings from older firmware (analysis_fps, pre_seconds) are accepted and ignored.
IGNORED = {"overlay", "record", "record_fps", "analysis_fps", "pre_seconds", "post_seconds",
           "max_clip_seconds", "overwrite_oldest"}


def profile_args(values):
    args = []
    for key, value in values.items():
        if key in IGNORED:
            continue
        if key == "method" and value == "optical_flow":
            continue
        if key == "enabled":
            valid = type(value) is bool
            value = int(value) if valid else value
        elif key == "cell":
            valid = type(value) in (int, float) and value in (5, 8, 10, 20)
        elif key in RANGES and type(value) in (int, float) and math.isfinite(value):
            low, high, step = RANGES[key]
            ticks = (value - low) / step
            valid = low <= value <= high and abs(ticks - round(ticks)) < 1e-6
        else:
            valid = False
        if not valid:
            raise ValueError(f"Unsupported motion setting {key}={value!r}")
        args.extend((key, str(value)))
    return args


def chunks(data, start, end):
    while start + 8 <= end:
        tag, size = struct.unpack_from("<4sI", data, start)
        payload = start + 8
        if payload + size > end:
            raise ValueError(f"Truncated AVI chunk {tag!r}")
        yield tag, payload, payload + size
        start = payload + size + size % 2


def read_clip(path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"AVI ":
        raise ValueError(f"{path}: expected RIFF AVI")
    frame_us = 0
    frames = []
    for tag, start, end in chunks(data, 12, len(data)):
        if tag == b"LIST" and data[start:start + 4] == b"hdrl":
            for kind, pos, stop in chunks(data, start + 4, end):
                if kind == b"avih" and stop - pos >= 56:
                    frame_us = struct.unpack_from("<I", data, pos)[0]
        if tag == b"LIST" and data[start:start + 4] == b"movi":
            frames.extend(data[pos:stop] for kind, pos, stop in chunks(data, start + 4, end)
                          if kind == b"00dc")
    if not frame_us or len(frames) < 2 or any(not frame.startswith(b"\xff\xd8") for frame in frames):
        raise ValueError(f"{path}: expected a timed MJPEG clip with at least two frames")
    return frame_us, frames


def build_runner(output):
    # The shared decoder header supplies its own configuration on both targets.
    (output / "sdkconfig.h").write_text("#pragma once\n")
    obj = output / "motion_jpeg.o"
    includes = [f"-I{folder}" for folder in (output, MAIN)]
    subprocess.run(["cc", "-std=c11", "-O2", *includes, "-c", MAIN / "motion_jpeg.c", "-o", obj], check=True)
    executable = output / "replay_motion"
    subprocess.run(["c++", "-std=c++17", "-O2", *includes,
                    ROOT / "tests/webrtc/native/replay_motion.cpp", MAIN / "motion_detector.cpp",
                    MAIN / "optical_flow.cpp", obj, "-o", executable], check=True)
    return executable


def run_case(case, *, executable, video_output, video_deps):
    index, (clip, profile, scenario) = case
    file, (frame_us, frames) = clip
    name, values = profile
    expected = scenario.get("expect_motion")
    stride = scenario.get("frame_stride", 1)
    static = scenario.get("freeze", False)
    work_us = scenario.get("work_us", 0)
    frames_in = [(i * frame_us, frames[0] if static else frames[i]) for i in range(0, len(frames), stride)]
    stream = b"".join(f"{timestamp} {len(jpeg)}\n".encode() + jpeg for timestamp, jpeg in frames_in)
    output = subprocess.run([executable, str(work_us), *profile_args(values)], input=stream,
                            stdout=subprocess.PIPE, check=True)
    trace = [json.loads(line) for line in output.stdout.splitlines()]
    detected = [row for row in trace if row["motion"]]
    triggers = [row for row in trace if row["trigger"]]
    invalid = sum(row["result"] == "invalid" for row in trace)
    analyzed = sum(row["result"] == "analyzed" for row in trace)
    sizes = sorted({(row["width"], row["height"]) for row in trace if row["result"] == "analyzed"})
    expected_size = scenario.get("expect_analysis_size")
    correct_size = expected_size is None or sizes == [tuple(expected_size)]
    passed = correct_size and not invalid and (analyzed > 0 or values.get("enabled") is False) and (
        expected is None or bool(detected) == expected)
    result = {"clip": str(file), "profile": name, "settings_overrides": values, "scenario": scenario,
            "frame_us": frame_us, "input_frames": len(frames_in), "admitted": len(trace),
            "analyzed": analyzed, "analysis_sizes": sizes, "invalid": invalid, "detected": len(detected),
            "first_motion_us": detected[0]["complete_us"] if detected else None,
            "first_trigger_us": triggers[0]["complete_us"] if triggers else None,
            "passed": passed, "trace": trace}
    if video_output:
        from motion_video import export_video
        label = re.sub(r"[^a-zA-Z0-9._-]", "-", f"{file.stem}-{name}-{scenario['name']}")
        path = video_output / f"{index:03d}-{label[:140]}.mp4"
        export_video(path, frames_in, frame_us * stride, trace, video_deps)
        result["video"] = str(path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("clips", nargs="*", type=Path, help=f"Defaults to {DEFAULT_CLIP.relative_to(ROOT)}")
    parser.add_argument("--matrix", type=Path, help="JSON profiles and scenarios to cross with the clips")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--report", type=Path, default=ROOT / "build/motion-replay/report.json")
    parser.add_argument("--video-output", type=Path, help="Write annotated MP4 videos into this directory")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        files = args.clips or [DEFAULT_CLIP]
        original = {"name": "original", "expect_motion": True}
        if files == [DEFAULT_CLIP]:
            original["expect_analysis_size"] = [160, 120]
        matrix = json.loads(args.matrix.read_text()) if args.matrix else {
            "profiles": {"firmware-defaults-enabled": {}},
            "scenarios": [original],
        }
        profiles = [(name, entry.get("values", entry)) for name, entry in matrix["profiles"].items()]
        scenarios = matrix["scenarios"]
        if not profiles or not scenarios:
            raise ValueError("Matrix must contain at least one profile and scenario")
        for _, values in profiles:
            profile_args(values)
        for scenario in scenarios:
            size = scenario.get("expect_analysis_size")
            if size is not None and (not isinstance(size, list) or len(size) != 2 or
                                     any(type(value) is not int or value < 1 for value in size)):
                raise ValueError(f"Invalid expected analysis size {size!r}")
            if (not isinstance(scenario["name"], str) or
                type(scenario.get("frame_stride", 1)) is not int or scenario.get("frame_stride", 1) < 1 or
                type(scenario.get("work_us", 0)) is not int or scenario.get("work_us", 0) < 0 or
                type(scenario.get("freeze", False)) is not bool or
                scenario.get("expect_motion") is not None and type(scenario["expect_motion"]) is not bool):
                raise ValueError(f"Invalid replay scenario {scenario!r}")
        video_deps = None
        if args.video_output:
            from motion_video import video_dependencies
            video_deps = video_dependencies()
        clips = [(file, read_clip(file)) for file in files]
        cases = [(clip, profile, scenario) for clip in clips for profile in profiles for scenario in scenarios]
        with tempfile.TemporaryDirectory() as tmp:
            executable = build_runner(Path(tmp))
            with ThreadPoolExecutor(max_workers=min(args.jobs, len(cases))) as workers:
                run = partial(run_case, executable=executable, video_output=args.video_output, video_deps=video_deps)
                results = list(workers.map(run, enumerate(cases, 1)))
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"{error}\n")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(results, indent=2) + "\n")
    for row in results:
        print(f"{'PASS' if row['passed'] else 'FAIL'} {Path(row['clip']).name} / "
              f"{row['profile']} / {row['scenario']['name']}: "
              f"{row['detected']} detections / {row['analyzed']} analyzed, "
              f"{row['invalid']} invalid, first trigger: {row['first_trigger_us']} us")
    print(f"Frame traces: {args.report}")
    return 0 if all(row["passed"] for row in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
