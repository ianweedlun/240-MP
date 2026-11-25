#!/usr/bin/env python3
"""M-1 soak gate — Owner Decision D3(b) (issue #135).

Builds the T2AppSoak observer into a copy of the product main(), runs the
module-list <-> module-view transition sweep headless, and applies the
D3(b) gate:

  * ASAN_OPTIONS=...:quarantine_size_mb=0  -> allocator quarantine cannot
    masquerade as per-transition retention (the rest of the ~278 MB ASan
    delta was exactly this quarantine, saturated at its 256 MB cap).
  * post-warmup RSS slope + late-window plateau (computed in t2_app_soak.h)
    -> the bounded ~1.5-1.8 MB QtQml/QV4 warmup is not a leak.
  * zero LSan reports, flat QObject/root counts, no load errors.

The C++ harness emits the pass decision in T2_RESULT; this runner adds the
sanitizer-report check (LSan prints to stderr after main returns) and writes
a compact evidence bundle. It never relaxes a threshold or adds a suppression.

Usage:
    python3 src/tests/m1-soak/run_m1_soak.py \
        --build-dir build-asan-lsan --transitions 500 --out <evidence-dir>
"""

import argparse
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys

LEAK_MARKERS = (
    "ERROR: LeakSanitizer",
    "SUMMARY: AddressSanitizer",
    "ERROR: AddressSanitizer",
    "detected memory leaks",
)


def parse_make_var(path: pathlib.Path, name: str) -> str:
    for line in path.read_text().splitlines():
        if line.startswith(name + " = "):
            return line[len(name) + 3 :]
    return ""


def build_soak(build_dir: pathlib.Path) -> pathlib.Path:
    tgt = build_dir / "CMakeFiles" / "240mp.dir"
    flags = parse_make_var(tgt / "flags.make", "CXX_DEFINES")
    includes = parse_make_var(tgt / "flags.make", "CXX_INCLUDES")
    cxxflags = parse_make_var(tgt / "flags.make", "CXX_FLAGS")
    obj = build_dir / "t2_app_main.o"
    src = build_dir.parent / "src" / "t2_app_main.cpp"
    compile_cmd = (
        ["/usr/bin/c++"]
        + shlex.split(flags)
        + ['-DAPP_VERSION="dev"']
        + shlex.split(includes)
        + shlex.split(cxxflags)
        + ["-c", str(src), "-o", str(obj)]
    )
    subprocess.run(compile_cmd, cwd=build_dir, check=True)

    link = shlex.split((tgt / "link.txt").read_text())
    out = build_dir / "t2_app_main"
    new_link = []
    skip_next = False
    for token in link:
        if skip_next:
            new_link.append(str(out))
            skip_next = False
            continue
        if token == "-o":
            new_link.append(token)
            skip_next = True
            continue
        if token.endswith("src/main.cpp.o"):
            new_link.append(str(obj))
        else:
            new_link.append(token)
    subprocess.run(new_link, cwd=build_dir, check=True)
    return out


def run_soak(binary: pathlib.Path, args) -> dict:
    env = os.environ.copy()
    env.update(
        {
            "QT_QPA_PLATFORM": "offscreen",
            "APP_ROOT": str(args.app_root),
            "DATA_ROOT": str(args.data_root),
            "TORTURE_APP_SCENARIO": args.scenario,
            "TORTURE_M1_TRANSITIONS": str(args.transitions),
            "TORTURE_M1_GATE_MODE": "asan-q0" if (args.asan or args.quarantine0) else "plain",
        }
    )
    if args.interval_ms > 0:
        env["TORTURE_M1_INTERVAL_MS"] = str(args.interval_ms)
    if args.quarantine0 or args.asan:
        # D3(b): the quarantine is the artifact being removed, not suppressed.
        env["ASAN_OPTIONS"] = (
            "detect_leaks=1:abort_on_error=1:"
            "quarantine_size_mb=0:thread_local_quarantine_size_kb=0"
        )
    proc = subprocess.run(
        [str(binary)],
        env=env,
        cwd=str(args.app_root),
        capture_output=True,
        text=True,
        timeout=args.timeout,
    )
    result = None
    samples = []
    for line in proc.stdout.splitlines():
        if line.startswith("T2_RESULT "):
            result = json.loads(line[len("T2_RESULT ") :])
        elif line.startswith("T2_SAMPLE "):
            samples.append(json.loads(line[len("T2_SAMPLE ") :]))
    leak_hits = [m for m in LEAK_MARKERS if m in proc.stderr or m in proc.stdout]
    passed = bool(result and result.get("pass")) and not leak_hits and proc.returncode == 0
    return {
        "pass": passed,
        "scenario": args.scenario,
        "transitions": args.transitions,
        "asan_options": env.get("ASAN_OPTIONS", ""),
        "exit_code": proc.returncode,
        "sanitizer_leak_markers": leak_hits,
        "result": result,
        "sample_count": len(samples),
        "samples": samples,
        "stdout_full": proc.stdout,
        "stderr_full": proc.stderr,
        "stdout_tail": proc.stdout[-4000:],
        "stderr_tail": proc.stderr[-4000:],
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", required=True, type=pathlib.Path)
    p.add_argument("--app-root", type=pathlib.Path, default=None)
    p.add_argument("--data-root", type=pathlib.Path, default=pathlib.Path.home() / ".local/share/240-MP")
    p.add_argument("--scenario", default="M1")
    p.add_argument("--transitions", type=int, default=500)
    p.add_argument("--out", type=pathlib.Path, default=None)
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--asan", action="store_true", help="build dir is an ASan/LSan build")
    p.add_argument("--quarantine0", action="store_true", help="force ASAN_OPTIONS quarantine_size_mb=0")
    p.add_argument("--interval-ms", type=int, default=0,
                   help="transition timer interval in ms (0 = harness default 4 ms); use 1 for the fast stress leg")
    p.add_argument("--asan-build-dir", type=pathlib.Path, default=None,
                   help="also run the ASan/LSan quarantine_size_mb=0 leg here and emit a combined D3(b) verdict")
    args = p.parse_args()

    args.build_dir = args.build_dir.resolve()
    if args.app_root is None:
        args.app_root = args.build_dir.parent.resolve()
    else:
        args.app_root = args.app_root.resolve()
    if args.out is None:
        args.out = args.build_dir / "m1-soak-evidence"
    args.out.mkdir(parents=True, exist_ok=True)

    def persist(leg, out_dir):
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "gate.json").write_text(json.dumps(leg, indent=2) + "\n")
        (out_dir / "stdout.log").write_text(leg.get("stdout_full", ""))
        (out_dir / "stderr.log").write_text(leg.get("stderr_full", ""))

    def leg_summary(leg):
        r = leg["result"] or {}
        return {
            "pass": leg["pass"], "exit_code": leg["exit_code"],
            "sanitizer_leak_markers": leg["sanitizer_leak_markers"],
            "asan_options": leg["asan_options"],
            "gate_basis": r.get("gate_basis"),
            "slope_kb_per_transition": r.get("post_warmup_slope_kb_per_transition"),
            "late_window_growth_kb": r.get("late_window_growth_kb"),
        }

    plain = run_soak(build_soak(args.build_dir), args)
    persist(plain, args.out)
    verdict = {"d3b": "Owner Decision D3(b): plain RSS slope/plateau + ASan quarantine_size_mb=0 LSan", "plain": plain}

    if args.asan_build_dir is not None:
        asan_args = argparse.Namespace(**vars(args))
        asan_args.build_dir = args.asan_build_dir.resolve()
        asan_args.asan = True
        asan_args.quarantine0 = True
        asan_args.out = args.out / "asan-q0"
        asan = run_soak(build_soak(asan_args.build_dir), asan_args)
        persist(asan, asan_args.out)
        verdict["asan_q0"] = asan
        verdict["pass"] = bool(plain["pass"] and asan["pass"])
        (args.out / "d3b-verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")
    else:
        verdict["pass"] = plain["pass"]

    print(json.dumps({
        "pass": verdict["pass"],
        "legs": {name: leg_summary(leg) for name, leg in verdict.items() if isinstance(leg, dict) and "pass" in leg},
        "evidence": str(args.out / ("d3b-verdict.json" if args.asan_build_dir else "gate.json")),
    }, indent=2))
    return 0 if verdict["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
