#!/usr/bin/env python3
"""IDLE-01 stale idle-count case orchestrator (checked-in two-sided gate).

Owns everything the probe must not: env, timeout, evidence, and the pass/fail
assertions. The probe binary (IdleTrackerStaleCountProbe.cpp) always exits 0
when it ran and writes the measurement JSON; this script turns that into a
verdict. Failure output names the violated invariant:
`INVARIANT <name> VIOLATED: ...` (ROB-07 / volume-stall convention).

Cases (one per ctest entry), arm letters kept from the torture evidence:
  mpv      (D)  pre-idle to threshold-1, mpv block, KEYLESS return.
                FAILS BY DESIGN on the pre-fix class (first-tick fire).
  script   (S)  same via setScriptActive. FAILS BY DESIGN pre-fix.
  keypress (H1) same pre-idle + block, one real non-auto-repeat KeyPress at
                return. Healthy control — PASS on BOTH sides.
  fresh    (H2) resetActivity() baseline (no pre-idle), block, keyless return.
                Healthy control — PASS on BOTH sides.

Env contract:
  IDLE01_THRESHOLD_S   accelerated threshold in seconds (default 4, min 3).
                       Real 1000 ms ticks always — no timer scaling. 4 keeps
                       each arm ~11 s; 60 reproduces production fidelity at
                       ~135 s/arm (manual runs only, ctest TIMEOUT is 300).
  QT_QPA_PLATFORM      set to offscreen here.
  ASAN_OPTIONS         detect_leaks=1 appended here (sanitizer-clean asserted).
  UBSAN_OPTIONS        print_stacktrace=1 set here.

Timing bands (threshold t, default 4):
  healthy  latency >= (t-1)*1000*0.95 (2850 ms at t=4; Qt CoarseTimer fires up
           to 5% early) AND <= (t+2)*1000 (load slack for parallel CI) AND
           >= t real ticks after return (reset emissions excluded).
  defect   latency <= 1100 ms AND <= 2 real ticks (first-tick fire from the
           stale pre-playback count). The 1100..2850 ms gap is unambiguous.
  The freeze invariant (ZERO idleSecondsChanged emissions during the 3.5 s
  block, count held at threshold-1 / 0) is asserted on EVERY arm, both sides.

activityDetected counts: the post-fix blocking->unblocked edge emits one
activityDetected itself, so the defect arms see exactly 1 (edge only,
keyless) while the controls legitimately differ by side — keypress 1 pre-fix
(keypress only) / 2 post-fix (edge + keypress), fresh 1 pre-fix (baseline) /
2 post-fix (baseline + edge). The controls therefore assert the documented
1..2 band; the defect arms assert the exact post-fix 1. The timing assertions
are the regression gate and are identical on both sides — never weakened.

--baseline (manual two-sided proof mode, NOT registered in ctest): asserts
the recorded PRE-FIX red shape instead — defect arms DEFECT (1 tick,
<=1100 ms, 0 detections), controls PASS with pre-fix counts (1). Run it
against a pre-fix control build to prove the plain gate fails for the RIGHT
reason. On the post-fix class --baseline fails by design (arms are green).

Evidence per run: <cwd>/idle-tracker-stale-count-evidence/<case>/ holds
probe stdout/stderr, arm.json, env.txt and verdict.txt.

Usage:
  run_idle_tracker_stale_count_case.py --case mpv \
      --probe ./build-test/idle-tracker-stale-count-probe \
      --source-root . [--baseline]
"""
import argparse
import json
import os
import subprocess
import sys

CASE_TO_ARM = {"mpv": "D", "script": "S", "keypress": "H1", "fresh": "H2"}
DEFECT_ARMS = ("D", "S")
DEFAULT_THRESHOLD_S = 4
MIN_THRESHOLD_S = 3
DEFECT_MAX_MS = 1100.0
SANITIZER_MARKERS = (
    "ERROR: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "SUMMARY: AddressSanitizer",
    "runtime error:",
)


def contract_min_ms(threshold):
    return (threshold - 1) * 1000.0 * 0.95


def latency_max_ms(threshold):
    # Load slack only — the discriminating assertion is the LOWER bound plus
    # the tick count. (t+2)*1000 tolerates coarse-timer drift on busy CI runners.
    return (threshold + 2) * 1000.0


def git_rev(source_root):
    try:
        out = subprocess.run(["git", "-C", source_root, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=15)
        if out.returncode == 0:
            return out.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        pass
    return "unknown"


def check_rig(run, res, violations, threshold):
    """Instrument sanity — identical on both sides, every arm."""
    label = run["label"]
    if run["timed_out"]:
        violations.append("INVARIANT no-hang VIOLATED [%s]: probe exceeded %ds"
                          % (label, run["timeout_s"]))
    if run["rc"] != 0 or res is None:
        violations.append("INVARIANT probe-ran VIOLATED [%s]: rc=%s, JSON present=%s"
                          % (label, run["rc"], res is not None))
        return
    for marker in SANITIZER_MARKERS:
        if marker in run["stderr"]:
            violations.append("INVARIANT sanitizer-clean VIOLATED [%s]: %r in stderr"
                              % (label, marker))
            break
    if res.get("threshold_s") != threshold:
        violations.append("INVARIANT threshold-applied VIOLATED [%s]: probe ran %r, want %d"
                          % (label, res.get("threshold_s"), threshold))
    if res.get("pre_idle_reached") is not True:
        violations.append("INVARIANT pre-idle-reached VIOLATED [%s]: count never reached "
                          "threshold-1 before the block" % label)
    # The freeze proof: zero count emissions while blocked, count held.
    if res.get("idle_emissions_during_block") != 0:
        violations.append("INVARIANT freeze-zero-emissions VIOLATED [%s]: %s "
                          "idleSecondsChanged emissions during the 3.5 s block (want 0)"
                          % (label, res.get("idle_emissions_during_block")))
    expect_count = 0 if res.get("arm") == "H2" else threshold - 1
    if res.get("freeze_observed") is not True or \
            res.get("count_at_block") != expect_count or \
            res.get("count_after_block") != expect_count:
        violations.append("INVARIANT freeze-count-held VIOLATED [%s]: at_block=%s "
                          "after_block=%s frozen=%s (want %d/%d/True)"
                          % (label, res.get("count_at_block"), res.get("count_after_block"),
                             res.get("freeze_observed"), expect_count, expect_count))
    if res.get("fired") is not True:
        violations.append("INVARIANT fired VIOLATED [%s]: saver never fired in the "
                          "threshold+4 s window" % label)


def check_green(run, res, violations, threshold):
    """Post-fix contract — the checked-in gate. Defect arms fail here pre-fix."""
    label = run["label"]
    if res is None:
        return
    arm = res.get("arm")
    latency = res.get("fire_latency_ms", -1.0)
    ticks = res.get("real_ticks_after_return", -1)
    lo, hi = contract_min_ms(threshold), latency_max_ms(threshold)
    if not (lo <= latency <= hi):
        violations.append("INVARIANT full-threshold-latency VIOLATED [%s]: keyless-return "
                          "fire latency %.1f ms outside [%.0f, %.0f] — a first-tick fire "
                          "(~1 tick, <=1100 ms) is the IDLE-01 stale-count ambush"
                          % (label, latency, lo, hi))
    if ticks < threshold:
        violations.append("INVARIANT full-threshold-ticks VIOLATED [%s]: %d real ticks "
                          "after return, want >= %d (a full threshold of post-return idle)"
                          % (label, ticks, threshold))
    if res.get("verdict") != "PASS":
        violations.append("INVARIANT verdict-pass VIOLATED [%s]: probe timing verdict %r"
                          % (label, res.get("verdict")))
    detections = res.get("activity_detections", -1)
    if arm in DEFECT_ARMS:
        if res.get("key_sent") is not False:
            violations.append("INVARIANT keyless VIOLATED [%s]: defect arm must return "
                              "with NO key events" % label)
        if detections != 1:
            violations.append("INVARIANT edge-reset-detection VIOLATED [%s]: "
                              "activity_detections=%s, want exactly 1 (the post-fix "
                              "blocking->unblocked edge emits one reset; pre-fix emits 0)"
                              % (label, detections))
    elif arm == "H1":
        if res.get("key_sent") is not True:
            violations.append("INVARIANT key-delivered VIOLATED [%s]: control must send "
                              "one real KeyPress at return" % label)
        if not (1 <= detections <= 2):
            violations.append("INVARIANT activity-count-band VIOLATED [%s]: "
                              "activity_detections=%s, want 1 (pre-fix: keypress) or "
                              "2 (post-fix: edge + keypress)" % (label, detections))
    elif arm == "H2":
        if res.get("key_sent") is not False:
            violations.append("INVARIANT keyless VIOLATED [%s]: fresh-idle control sends "
                              "no keys" % label)
        if not (1 <= detections <= 2):
            violations.append("INVARIANT activity-count-band VIOLATED [%s]: "
                              "activity_detections=%s, want 1 (pre-fix: baseline) or "
                              "2 (post-fix: baseline + edge)" % (label, detections))


def check_baseline(run, res, violations, threshold):
    """Pre-fix red shape (manual --baseline proof mode, both sides' rig checks)."""
    label = run["label"]
    if res is None:
        return
    arm = res.get("arm")
    latency = res.get("fire_latency_ms", -1.0)
    ticks = res.get("real_ticks_after_return", -1)
    detections = res.get("activity_detections", -1)
    if arm in DEFECT_ARMS:
        if res.get("verdict") != "DEFECT" or ticks != 1 or latency > DEFECT_MAX_MS:
            violations.append("INVARIANT baseline-red VIOLATED [%s]: expected the pre-fix "
                              "first-tick ambush (DEFECT, 1 tick, <=%.0f ms), got verdict=%r "
                              "ticks=%s latency=%.1f — fix landed, or wrong source tree?"
                              % (label, DEFECT_MAX_MS, res.get("verdict"), ticks, latency))
        if detections != 0:
            violations.append("INVARIANT baseline-no-edge-reset VIOLATED [%s]: pre-fix "
                              "unblock emits no activityDetected, got %s" % (label, detections))
        if res.get("key_sent") is not False:
            violations.append("INVARIANT keyless VIOLATED [%s]: defect arm must return "
                              "with NO key events" % label)
    else:
        lo, hi = contract_min_ms(threshold), latency_max_ms(threshold)
        if res.get("verdict") != "PASS" or not (lo <= latency <= hi) or ticks < threshold:
            violations.append("INVARIANT baseline-control-green VIOLATED [%s]: healthy "
                              "control must PASS on the pre-fix class too (verdict=%r "
                              "latency=%.1f ticks=%s)" % (label, res.get("verdict"), latency, ticks))
        want = 1  # pre-fix: keypress only (H1) / baseline only (H2)
        if detections != want:
            violations.append("INVARIANT baseline-control-count VIOLATED [%s]: pre-fix "
                              "activity_detections=%s, want %d" % (label, detections, want))
        if arm == "H1" and res.get("key_sent") is not True:
            violations.append("INVARIANT key-delivered VIOLATED [%s]: control must send "
                              "one real KeyPress at return" % label)


def main() -> int:
    ap = argparse.ArgumentParser(description="IDLE-01 stale idle-count case orchestrator")
    ap.add_argument("--case", required=True, choices=sorted(CASE_TO_ARM))
    ap.add_argument("--probe", required=True, help="idle-tracker-stale-count-probe binary")
    ap.add_argument("--source-root", required=True,
                    help="tree whose src/input/IdleTracker.cpp was compiled (git provenance)")
    ap.add_argument("--baseline", action="store_true",
                    help="assert the recorded PRE-FIX red shape (manual two-sided proof)")
    args = ap.parse_args()
    args.probe = os.path.abspath(args.probe)
    args.source_root = os.path.abspath(args.source_root)
    if not os.path.isfile(args.probe) or not os.access(args.probe, os.X_OK):
        print("probe not executable: %s" % args.probe)
        return 2
    try:
        threshold = int(os.environ.get("IDLE01_THRESHOLD_S", DEFAULT_THRESHOLD_S))
    except ValueError:
        print("IDLE01_THRESHOLD_S must be an integer")
        return 2
    if threshold < MIN_THRESHOLD_S:
        print("IDLE01_THRESHOLD_S must be >= %d (timing bands need a gap)" % MIN_THRESHOLD_S)
        return 2

    arm = CASE_TO_ARM[args.case]
    rev = git_rev(args.source_root)
    evidence_root = os.path.join(os.getcwd(), "idle-tracker-stale-count-evidence", args.case)
    os.makedirs(evidence_root, exist_ok=True)
    out_json = os.path.join(evidence_root, "arm.json")
    if os.path.exists(out_json):
        os.remove(out_json)  # never assert a stale JSON from an earlier run

    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["UBSAN_OPTIONS"] = "print_stacktrace=1"
    if "ASAN_OPTIONS" not in env or "detect_leaks" not in env["ASAN_OPTIONS"]:
        env["ASAN_OPTIONS"] = (env.get("ASAN_OPTIONS", "") + ":detect_leaks=1").lstrip(":")

    timeout_s = 2 * threshold + 30
    timed_out = False
    try:
        proc = subprocess.run([args.probe, arm, out_json, str(threshold)],
                              env=env, cwd=evidence_root, capture_output=True,
                              text=True, timeout=timeout_s)
        rc, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        rc = -1
        stdout = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = (exc.stderr or b"").decode("utf-8", "replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")

    res = None
    if os.path.exists(out_json):
        try:
            with open(out_json, encoding="utf-8") as f:
                res = json.load(f)
        except ValueError:
            res = None
    with open(os.path.join(evidence_root, "probe.stdout"), "w", encoding="utf-8") as f:
        f.write(stdout)
    with open(os.path.join(evidence_root, "probe.stderr"), "w", encoding="utf-8") as f:
        f.write(stderr)
    with open(os.path.join(evidence_root, "env.txt"), "w", encoding="utf-8") as f:
        try:
            f.write("# loadavg: %s\n" % (os.getloadavg(),))
        except OSError:
            pass
        f.write("IDLE01_THRESHOLD_S=%d\n" % threshold)
        for k in ("QT_QPA_PLATFORM", "ASAN_OPTIONS", "UBSAN_OPTIONS"):
            f.write("%s=%s\n" % (k, env[k]))
        f.write("source_root=%s\nsource_rev=%s\nbaseline=%s\n"
                % (args.source_root, rev, args.baseline))

    run = {"label": args.case, "rc": rc, "stderr": stderr,
           "timed_out": timed_out, "timeout_s": timeout_s}
    violations = []
    print("IDLE-01 stale-count case=%s arm=%s threshold=%ds rev=%s mode=%s"
          % (args.case, arm, threshold, rev[:12], "baseline" if args.baseline else "gate"))
    check_rig(run, res, violations, threshold)
    if res is not None:
        # Provenance cross-check: the compiled-in rev must match the source
        # tree the orchestrator was pointed at (catches mis-aimed control builds).
        compiled = res.get("source_rev", "unknown")
        if compiled != rev and "unknown" not in (compiled, rev):
            violations.append("INVARIANT provenance VIOLATED [%s]: probe compiled from %s "
                              "but --source-root is at %s" % (args.case, compiled[:12], rev[:12]))
        if args.baseline:
            check_baseline(run, res, violations, threshold)
        else:
            check_green(run, res, violations, threshold)
        print("arm %s: verdict=%s latency_ms=%.1f real_ticks=%d detections=%d freeze_emissions=%d"
              % (arm, res.get("verdict"), res.get("fire_latency_ms", -1.0),
                 res.get("real_ticks_after_return", -1), res.get("activity_detections", -1),
                 res.get("idle_emissions_during_block", -1)))
        print("RESULT " + json.dumps(res, sort_keys=True))
    print("evidence: %s" % evidence_root)
    for v in violations:
        print(v)
    verdict = "PASS" if not violations else "FAIL"
    with open(os.path.join(evidence_root, "verdict.txt"), "w", encoding="utf-8") as f:
        f.write("case=%s arm=%s threshold=%ds rev=%s mode=%s verdict=%s\n"
                % (args.case, arm, threshold, rev, "baseline" if args.baseline else "gate", verdict))
        for v in violations:
            f.write(v + "\n")
    print("CASE %s: %s (%d invariant violation(s))" % (args.case, verdict, len(violations)))
    return 0 if not violations else 1


if __name__ == "__main__":
    sys.exit(main())