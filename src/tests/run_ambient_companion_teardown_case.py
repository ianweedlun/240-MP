#!/usr/bin/env python3
"""AMBIENT-GHOST-AUDIO-01 two-sided gate orchestrator (checked in).

The probe (AmbientCompanionTeardownProbe.cpp) measures one arm and always exits
0 when it ran. This script owns the verdict: it runs the probe for a named
ctest case, checks rig hygiene, then asserts the expected behavioral shape.
Failure output names the violated invariant (volume-stall / ROB-07 convention).

Two sides, one assertion layer:
  teardown-product       product Player.qml -> FIXED (stop called, child reaped)
  teardown-prefix        pinned pre-fix view -> DEFECT (stop never called,
                         child alive); this ctest case is GREEN only when the
                         defect is reproduced, so the gate provably catches
                         a regression of the fix.
  repeat-teardown-*      six load -> Loader-source-reset cycles without
                         playbackEnded: product ends with zero live children,
                         prefix ends with an orphan.

Controls (product view) prove the cleanup path itself is sound and that the
no-spawn paths stay that way: control-ended, control-direct-stop,
control-empty-audio, control-video-only, control-repeat.

The pre-fix fixture is pinned verbatim (sha256 below). If it changes, the
rig-sanity check fails loudly instead of silently weakening the negative arm.

Usage:
  run_ambient_companion_teardown_case.py --ctest-case teardown-product \
      --probe ./build-test/ambient-companion-teardown-probe \
      --fake-mpv ./build-test/ambient-companion-fake-mpv \
      --product-view ./modules/ambient_mode/views/Player.qml \
      --prefix-view ./src/tests/ambient-teardown/prefix/Player.qml \
      --repo-root . [--cycles 6]
"""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys

EXPECTED_PREFIX_SHA = "aa319d811ce1ebb016b8b41bc32f2908d1a7fe4a5702067d499d8988917e9f89"

# ctest case -> (probe case, view, expectation, companion audio spawned?)
CASES = {
    "teardown-product":         ("teardown",         "product", "FIXED",  True),
    "teardown-prefix":          ("teardown",         "prefix",  "DEFECT", True),
    "control-ended":            ("control-ended",    "product", "PASS",   True),
    "control-direct-stop":      ("control-direct-stop", "product", "PASS", True),
    "control-empty-audio":      ("control-empty-audio", "product", "PASS", False),
    "control-video-only":       ("control-video-only", "product", "PASS", False),
    "control-repeat":           ("control-repeat",   "product", "REPEAT", True),
    "repeat-teardown-product":  ("repeat-teardown",  "product", "FIXED",  True),
    "repeat-teardown-prefix":   ("repeat-teardown",  "prefix",  "DEFECT", True),
}

SANITIZER_MARKERS = (
    "ERROR: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "SUMMARY: AddressSanitizer",
    "runtime error:",
)


def fail(violations, name, detail):
    violations.append("INVARIANT %s VIOLATED: %s" % (name, detail))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ctest-case", required=True, choices=sorted(CASES))
    ap.add_argument("--probe", required=True)
    ap.add_argument("--fake-mpv", required=True)
    ap.add_argument("--product-view", required=True)
    ap.add_argument("--prefix-view", required=True)
    ap.add_argument("--repo-root", required=True)
    ap.add_argument("--cycles", type=int, default=6)
    args = ap.parse_args(argv)

    probe_case, view, expect, with_audio = CASES[args.ctest_case]
    view_file = args.product_view if view == "product" else args.prefix_view
    violations = []

    if view == "prefix":
        actual = sha256(args.prefix_view)
        if actual != EXPECTED_PREFIX_SHA:
            fail(violations, "prefix-fixture-pinned",
                 "pre-fix view sha256 %s, want %s" % (actual, EXPECTED_PREFIX_SHA))

    evidence = pathlib.Path(os.getcwd()) / "ambient-companion-teardown-evidence" / args.ctest_case
    evidence.mkdir(parents=True, exist_ok=True)
    result_path = evidence / "result.json"
    if result_path.exists():
        result_path.unlink()

    cmd = [
        args.probe,
        "--case=" + probe_case,
        "--view-file=" + str(pathlib.Path(view_file).resolve()),
        "--fake-mpv=" + str(pathlib.Path(args.fake_mpv).resolve()),
        "--result=" + str(result_path.resolve()),
        "--view-label=" + view,
        "--cycles=" + str(args.cycles),
    ]
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=0"
    env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=0"

    try:
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=90, env=env)
        stdout_text = run.stdout if isinstance(run.stdout, str) else (run.stdout or b"").decode("utf-8", "replace")
        stderr_text = run.stderr if isinstance(run.stderr, str) else (run.stderr or b"").decode("utf-8", "replace")
        timed_out = False
    except subprocess.TimeoutExpired as exc:
        run = subprocess.CompletedProcess(cmd, -1, "", "")
        stdout_text = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout or b"").decode("utf-8", "replace")
        stderr_text = exc.stderr if isinstance(exc.stderr, str) else (exc.stderr or b"").decode("utf-8", "replace")
        timed_out = True

    stdout_text = str(stdout_text)
    stderr_text = str(stderr_text)

    (evidence / "stdout.txt").write_text(stdout_text, encoding="utf-8")
    (evidence / "stderr.txt").write_text(stderr_text, encoding="utf-8")

    if timed_out:
        fail(violations, "probe-ran", "probe exceeded 90 s")
    if run.returncode != 0:
        fail(violations, "probe-ran", "rc=%s" % run.returncode)
    for marker in SANITIZER_MARKERS:
        if marker in stderr_text:
            fail(violations, "sanitizer-clean", "%r in stderr" % marker)
            break

    res = None
    if result_path.exists():
        try:
            res = json.loads(result_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            fail(violations, "result-json", str(exc))

    if res is not None:
        if res.get("case") != probe_case or res.get("view") != view:
            fail(violations, "arm-identity", "result is case=%r view=%r" %
                 (res.get("case"), res.get("view")))
        if not res.get("wrapperOk") or not res.get("hostOk"):
            fail(violations, "qml-loaded", "wrapperOk=%r hostOk=%r notes=%r" %
                 (res.get("wrapperOk"), res.get("hostOk"), res.get("notes")))
        if res.get("loadCalls") != 1:
            fail(violations, "view-loaded", "loadCalls=%r" % res.get("loadCalls"))

        # Spawn proof: companion audio paths must actually create a child;
        # no-audio controls must not. This keeps a broken view from passing
        # an arm simply by never launching the process under test.
        if with_audio:
            if res.get("startCalls", 0) < 1 or res.get("pidAfterLoad", -1) <= 0:
                fail(violations, "companion-spawned", "startCalls=%r pid=%r" %
                     (res.get("startCalls"), res.get("pidAfterLoad")))
            if not res.get("aliveAfterLoad") or not res.get("aliveRegistryAfterLoad"):
                fail(violations, "companion-alive-before-teardown",
                     "aliveAfterLoad=%r registry=%r" %
                     (res.get("aliveAfterLoad"), res.get("aliveRegistryAfterLoad")))
        else:
            if res.get("startCalls", 0) != 0 or res.get("aliveAfterLoad"):
                fail(violations, "no-companion-spawn",
                     "startCalls=%r aliveAfterLoad=%r" %
                     (res.get("startCalls"), res.get("aliveAfterLoad")))

        stop = res.get("stopCallsFinal", 0)
        alive = res.get("aliveFinal")
        reg = res.get("aliveRegistryFinal")
        reg_size = len(reg) if isinstance(reg, list) else -1
        fixed_contract = (stop >= 1 and alive is False and reg_size == 0)

        if expect == "FIXED":
            if stop < 1:
                fail(violations, "stopAudio-called", "stopCallsFinal=%d" % stop)
            if alive is not False or reg_size != 0:
                fail(violations, "no-live-child", "aliveFinal=%r registry=%r" % (alive, reg))
        elif expect == "DEFECT":
            # The negative control: the fixed contract MUST fail on the
            # pre-fix view. Green here means the gate catches the regression.
            if stop != 0:
                fail(violations, "defect-stop-absent", "stopCallsFinal=%d, want 0" % stop)
            is_repeat = probe_case == "repeat-teardown"
            # Multi-cycle arms judge liveness from the registry: a later
            # startAudio() replaces (and stops) the previous child, so the
            # first PID can be dead while the latest one is still orphaned.
            if reg_size < 1 or (not is_repeat and alive is not True):
                fail(violations, "defect-child-orphaned", "aliveFinal=%r registry=%r" % (alive, reg))
            if fixed_contract:
                fail(violations, "regression-visible",
                     "fixed contract unexpectedly holds on the pre-fix view")
        elif expect == "PASS":
            if alive is not False or reg_size != 0:
                fail(violations, "no-live-child", "aliveFinal=%r registry=%r" % (alive, reg))
        elif expect == "REPEAT":
            if res.get("spawnedTotal") != args.cycles:
                fail(violations, "repeat-spawn-count", "spawnedTotal=%r want %d" %
                     (res.get("spawnedTotal"), args.cycles))
            if stop < args.cycles:
                fail(violations, "repeat-stop-count", "stopCallsFinal=%d want >=%d" %
                     (stop, args.cycles))
            if alive is not False or reg_size != 0:
                fail(violations, "repeat-no-orphans", "aliveFinal=%r registry=%r" % (alive, reg))

        if probe_case == "repeat-teardown" and res.get("spawnedTotal") != args.cycles:
            fail(violations, "repeat-spawn-count", "spawnedTotal=%r want %d" %
                 (res.get("spawnedTotal"), args.cycles))

        verdict = {
            "ctest_case": args.ctest_case,
            "expected": expect,
            "view": view,
            "case": probe_case,
            "stopCallsFinal": stop,
            "aliveFinal": alive,
            "aliveRegistryFinal": reg,
            "spawnedTotal": res.get("spawnedTotal"),
            "fixedContractHolds": fixed_contract,
        }
    else:
        verdict = {"ctest_case": args.ctest_case, "expected": expect, "view": view}
    (evidence / "verdict.json").write_text(
        json.dumps(verdict, indent=2) + "\n", encoding="utf-8")

    if violations:
        for v in violations:
            print(v)
        print("VERDICT %s FAIL" % args.ctest_case)
        print("PROBE_STDOUT %s" % stdout_text.strip())
        return 1

    print("RESULT %s" % json.dumps(verdict, sort_keys=True))
    print("VERDICT %s PASS" % args.ctest_case)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
