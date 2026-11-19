#!/usr/bin/env python3
"""P-5A volume-stall case orchestrator (torture T1, checked-in).

Owns everything the probe must not: scratch dirs, PATH-stub lifecycle, env,
timeouts, evidence, and the pass/fail assertions. The probe binary
(VolumeStallProbe.cpp) always exits 0 when it ran and prints one RESULT JSON
line; this script turns that into a verdict. Failure output names the
violated invariant: `INVARIANT <name> VIOLATED: ...` (ROB-07 convention).

Cases (one per ctest entry):
  alsa-delay200 / pipewire-delay200   slow-mixer stall gate (red on 3970174)
  alsa-delay0 / pipewire-delay0       zero-delay controls
  noop-control                        instant-mixer green control (rig self-check)
  alsa-faults / pipewire-faults       exit1 + garbage + missing executable
  baseline                            all four timing runs with the recorded
                                      red-baseline shape asserted (--baseline)

Env contract (set here, consumed by probe + stubs):
  PATH                     scratch bin dir ONLY (the compiled mixer-stub
                           binary copied in as amixer/wpctl). Missing-binary
                           mode = empty bin dir, so the factory falls back to
                           the no-op mixer. No system amixer/wpctl can leak in.
  XDG_CONFIG_HOME / XDG_DATA_HOME   scratch overrides
  TORTURE_DATA_ROOT        scratch app data dir (config.json/volume.json)
  TORTURE_MIXER_LOG        scratch argv capture, fresh per run
  TORTURE_STUB_STATE       scratch stub state file (level/muted)
  TORTURE_DELAY_MS         stub sleep before responding (0 | 200)
  TORTURE_STUB_MODE        normal | exit1 | garbage
  TORTURE_FORCE_NOOP       1 for the noop control only
  TORTURE_SNAPSHOT         git sha of the tree under test (RESULT provenance)
  ASAN_OPTIONS             detect_leaks=1 (sanitizer-clean is asserted)

All delay and state live in scratch dirs (tempfile.mkdtemp under TMPDIR);
no system file or real audio state is touched. The only shared path is the
app's own debug key channel /tmp/240mp_key.in, which the probe removes
before/after each run — ctest runs these cases RUN_SERIAL for that reason.

Evidence per run: <cwd>/volume-stall-evidence/<case>/<run>/ holds probe
stdout/stderr, the mixer argv log, RESULT json, env and verdict. Scratch
dirs are kept (path printed) for inspection; they are KB-sized.

Usage:
  run_volume_stall_case.py --case alsa-delay200 --probe ./build/volume-stall-probe \
      --stub-bin ./build/mixer-stub --repo-root . [--baseline] [--strict-semantics]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

TARGET_MS = 16
PROBE_TIMEOUT_S = 120

TIMING_CASES = {
    "alsa-delay200": {"backend": "alsa", "delay": 200},
    "alsa-delay0": {"backend": "alsa", "delay": 0},
    "pipewire-delay200": {"backend": "pipewire", "delay": 200},
    "pipewire-delay0": {"backend": "pipewire", "delay": 0},
}
FAULT_MODES = [
    # label, stub mode, stub present on PATH, delay
    ("exit1", "exit1", True, 200),
    ("garbage", "garbage", True, 200),
    ("missing", "normal", False, 0),
]
# Recorded 3970174 red baseline (lab torture-t1, ASan/UBSan, 200 ms stub,
# quiet host):
#   ALSA     delay200: mean 1466.33 / max 1470 / heartbeat 1505, 7 calls/press
#   PipeWire delay200: mean 211.0   / max 214  / heartbeat 219,  1 call/press
#   ALSA     delay0:   mean 23.67   (still red: 7 process spawns > 16 ms)
#   PipeWire delay0:   mean 3.33    (heartbeat 16 ms — borderline red)
# Bands are deliberately wide: process-spawn overhead is load-sensitive
# (a busy dev host adds ~10-25 ms per spawn; the delay200 runs are dominated
# by the stub sleep and stay tight). loadavg is recorded in the evidence.
BASELINE_BANDS = {
    "alsa-delay200": {"mean": (1200.0, 2000.0), "peak_min": 1000},
    "pipewire-delay200": {"mean": (150.0, 320.0), "peak_min": 150},
    "alsa-delay0": {"mean": (15.0, 150.0), "peak_min": 0},
    "pipewire-delay0": {"mean": (0.0, 20.0), "peak_min": 0},
}
# Semantic ladder with the stateful stubs (initial hardware level 50;
# PipeWire/noop start from cached 100): 3 direct Downs + 1 injected Down.
LADDER = {
    "alsa": {"post_direct": 35, "final": 30},
    "pipewire": {"post_direct": 85, "final": 80},
    "none": {"post_direct": 85, "final": 80},
}
SANITIZER_MARKERS = (
    "ERROR: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "SUMMARY: AddressSanitizer",
    "runtime error:",
)


def git_snapshot(repo_root):
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--short=7", "HEAD"],
            cwd=repo_root, capture_output=True, text=True, timeout=15)
        if out.returncode == 0:
            return out.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        pass
    return "unknown"


def make_scratch(label, stub_bin, backend, stub_present, stub_mode, delay,
                 force_noop, snapshot):
    scratch = tempfile.mkdtemp(prefix="volume-stall-%s-" % label)
    bin_dir = os.path.join(scratch, "bin")
    os.makedirs(bin_dir)
    if stub_present:
        # Only the requested backend's identity: the factory prefers wpctl
        # over amixer, so a stray wpctl would silently switch the backend.
        names = {"alsa": ["amixer"], "pipewire": ["wpctl"], "none": []}[backend]
        for name in names:
            dst = os.path.join(bin_dir, name)
            shutil.copy(stub_bin, dst)
            os.chmod(dst, 0o755)
    data_root = os.path.join(scratch, "data")
    os.makedirs(data_root)
    mixer_log = os.path.join(scratch, "mixer.log")
    env = os.environ.copy()
    env.update({
        "PATH": bin_dir,
        "XDG_CONFIG_HOME": os.path.join(scratch, "xdg-config"),
        "XDG_DATA_HOME": os.path.join(scratch, "xdg-data"),
        "TORTURE_DATA_ROOT": data_root,
        "TORTURE_MIXER_LOG": mixer_log,
        "TORTURE_STUB_STATE": os.path.join(scratch, "stub-state.txt"),
        "TORTURE_DELAY_MS": str(delay),
        "TORTURE_STUB_MODE": stub_mode,
        "TORTURE_SNAPSHOT": snapshot,
        "QT_QPA_PLATFORM": "offscreen",
        "SDL_VIDEODRIVER": "dummy",
        "UBSAN_OPTIONS": "print_stacktrace=1",
    })
    if "ASAN_OPTIONS" not in env or "detect_leaks" not in env["ASAN_OPTIONS"]:
        env["ASAN_OPTIONS"] = (env.get("ASAN_OPTIONS", "") + ":detect_leaks=1").lstrip(":")
    if force_noop:
        env["TORTURE_FORCE_NOOP"] = "1"
    return scratch, env, mixer_log


def run_probe(label, scenario, probe, stub_bin, backend, stub_present,
              stub_mode, delay, force_noop, snapshot, evidence_root):
    scratch, env, mixer_log = make_scratch(
        label, stub_bin, backend, stub_present, stub_mode, delay,
        force_noop, snapshot)
    timed_out = False
    try:
        proc = subprocess.run([probe, scenario], env=env, cwd=scratch,
                              capture_output=True, text=True,
                              timeout=PROBE_TIMEOUT_S)
        rc, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        rc = -1
        stdout = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = (exc.stderr or b"").decode("utf-8", "replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
    result = None
    for line in stdout.splitlines():
        if line.startswith("RESULT "):
            try:
                result = json.loads(line[len("RESULT "):])
            except ValueError:
                pass
    ev_dir = os.path.join(evidence_root, label)
    os.makedirs(ev_dir, exist_ok=True)
    with open(os.path.join(ev_dir, "probe.stdout"), "w", encoding="utf-8") as f:
        f.write(stdout)
    with open(os.path.join(ev_dir, "probe.stderr"), "w", encoding="utf-8") as f:
        f.write(stderr)
    if os.path.exists(mixer_log):
        shutil.copy(mixer_log, os.path.join(ev_dir, "mixer.log"))
    if result is not None:
        with open(os.path.join(ev_dir, "result.json"), "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2, sort_keys=True)
    with open(os.path.join(ev_dir, "env.txt"), "w", encoding="utf-8") as f:
        try:
            f.write("# loadavg: %s\n" % (os.getloadavg(),))
        except OSError:
            pass
        for k in sorted(env):
            if k.startswith("TORTURE_") or k in ("PATH", "XDG_CONFIG_HOME",
                                                 "XDG_DATA_HOME", "ASAN_OPTIONS",
                                                 "UBSAN_OPTIONS"):
                f.write("%s=%s\n" % (k, env[k]))
    return {"label": label, "rc": rc, "result": result, "stderr": stderr,
            "timed_out": timed_out, "scratch": scratch, "ev_dir": ev_dir}


def check_common(run, violations, expect_backend):
    label = run["label"]
    res = run["result"]
    if run["timed_out"]:
        violations.append("INVARIANT no-hang VIOLATED [%s]: probe exceeded %ds"
                          % (label, PROBE_TIMEOUT_S))
    if run["rc"] != 0 or res is None:
        violations.append("INVARIANT probe-ran VIOLATED [%s]: rc=%s, RESULT present=%s"
                          % (label, run["rc"], res is not None))
        return
    for marker in SANITIZER_MARKERS:
        if marker in run["stderr"]:
            violations.append("INVARIANT sanitizer-clean VIOLATED [%s]: %r in stderr"
                              % (label, marker))
            break
    if res.get("backend_selected") != expect_backend:
        violations.append("INVARIANT backend-selected VIOLATED [%s]: expected %r, got %r"
                          % (label, expect_backend, res.get("backend_selected")))
    level = res.get("final_level")
    if not isinstance(level, int) or not (0 <= level <= 100):
        violations.append("INVARIANT sane-state VIOLATED [%s]: final_level=%r"
                          % (label, level))
    if not isinstance(res.get("final_muted"), bool):
        violations.append("INVARIANT sane-state VIOLATED [%s]: final_muted=%r"
                          % (label, res.get("final_muted")))


def check_timing(run, violations, case, backend, baseline, strict_semantics):
    label = run["label"]
    res = run["result"]
    if res is None:
        return
    delay = res.get("stub_delay_ms", -1)
    if res.get("stub_mode") != "normal":
        violations.append("INVARIANT stub-mode VIOLATED [%s]: %r" % (label, res.get("stub_mode")))
    if not res.get("injected_key_delivered"):
        violations.append(
            "INVARIANT injection-delivered VIOLATED [%s]: file-channel voldown never "
            "reached VolumeManager (signals=%s, post_direct=%s, final=%s). Is "
            "TELEMETRY=ON? The key channel is compiled out with -DTELEMETRY=OFF."
            % (label, res.get("volume_changed_signals"),
               res.get("post_direct_level"), res.get("final_level")))
    if backend == "none":
        if res.get("mixer_calls_total") != 0:
            violations.append("INVARIANT mixer-reached VIOLATED [%s]: noop control "
                              "spawned %s mixer processes" % (label, res.get("mixer_calls_total")))
    else:
        if not (res.get("per_press_direct", 0) >= 1 and res.get("per_press_extra", 0) >= 1):
            violations.append("INVARIANT mixer-reached VIOLATED [%s]: per_press_direct=%s "
                              "per_press_extra=%s — volume keys never reached the mixer"
                              % (label, res.get("per_press_direct"), res.get("per_press_extra")))
    ctor = res.get("constructor_ms", 0)
    dmax = res.get("direct_key_max_ms", 0)
    peak = res.get("injection_peak_heartbeat_gap_ms", 0)
    stall_details = []
    if ctor >= TARGET_MS:
        stall_details.append("constructor %sms" % ctor)
    if dmax >= TARGET_MS:
        stall_details.append("direct max %sms" % dmax)
    if peak >= TARGET_MS:
        stall_details.append("heartbeat peak %sms" % peak)
    if baseline:
        # Two-sided: on the unfixed snapshot the stall MUST be present.
        # pipewire-delay0 is exempt: its only red on 3970174 is a heartbeat
        # gap sitting exactly on the 16 ms boundary (lab: 16 ms), which is
        # scheduler-jitter borderline. The decisive reds are the delay200
        # runs and alsa-delay0; this control is band-checked only.
        if not stall_details and case != "pipewire-delay0":
            violations.append("INVARIANT baseline-red VIOLATED [%s]: run unexpectedly "
                              "green — fix landed, or stub delay not applied" % label)
        band = BASELINE_BANDS[case]
        mean = res.get("direct_key_mean_ms", 0)
        if not (band["mean"][0] <= mean <= band["mean"][1]):
            violations.append("INVARIANT baseline-band VIOLATED [%s]: direct mean %.2fms "
                              "outside %s" % (label, mean, band["mean"]))
        if peak < band["peak_min"]:
            violations.append("INVARIANT baseline-band VIOLATED [%s]: heartbeat peak %sms "
                              "< %sms" % (label, peak, band["peak_min"]))
        if backend == "alsa":
            if res.get("per_press_direct") != 7 or res.get("per_press_extra") != 7:
                violations.append("INVARIANT baseline-amplification VIOLATED [%s]: ALSA "
                                  "per-press calls direct=%s injected=%s, expected 7/7"
                                  % (label, res.get("per_press_direct"), res.get("per_press_extra")))
    elif stall_details:
        violations.append("INVARIANT ui-stall VIOLATED [%s]: %s (target <%dms)"
                          % (label, ", ".join(stall_details), TARGET_MS))
    if baseline or strict_semantics:
        ladder = LADDER[backend]
        if res.get("post_direct_level") != ladder["post_direct"] or \
                res.get("final_level") != ladder["final"]:
            violations.append("INVARIANT semantic-ladder VIOLATED [%s]: post_direct=%s "
                              "final=%s, expected %s/%s (stateful stub semantics)"
                              % (label, res.get("post_direct_level"),
                                 res.get("final_level"),
                                 ladder["post_direct"], ladder["final"]))
        if res.get("persisted_level") != res.get("final_level") or \
                res.get("persisted_muted") != res.get("final_muted"):
            violations.append("INVARIANT persist-matches VIOLATED [%s]: persisted=%s/%s "
                              "final=%s/%s" % (label, res.get("persisted_level"),
                                               res.get("persisted_muted"),
                                               res.get("final_level"), res.get("final_muted")))


def check_fault(run, violations, backend, mode, present):
    label = run["label"]
    res = run["result"]
    if res is None:
        return
    expect_backend = backend if present else "none"
    if res.get("backend_selected") != expect_backend:
        violations.append("INVARIANT backend-selected VIOLATED [%s]: expected %r, got %r"
                          % (label, expect_backend, res.get("backend_selected")))
    if present:
        if res.get("per_press_direct", 0) < 1:
            violations.append("INVARIANT mixer-reached VIOLATED [%s]: failing mixer was "
                              "never invoked (per_press_direct=%s)"
                              % (label, res.get("per_press_direct")))
    else:
        if res.get("mixer_calls_total") != 0:
            violations.append("INVARIANT missing-fallback VIOLATED [%s]: %s mixer calls "
                              "with no executable on PATH" % (label, res.get("mixer_calls_total")))
    persisted = res.get("persisted_level")
    if persisted is not None and not (isinstance(persisted, int) and 0 <= persisted <= 100):
        violations.append("INVARIANT sane-state VIOLATED [%s]: persisted_level=%r"
                          % (label, persisted))


def summarize(run, backend, delay, mode):
    res = run["result"] or {}
    return ("%-22s backend=%-8s delay=%3sms mode=%-7s direct mean=%8.2fms max=%6sms "
            "hb peak=%6sms calls/press=%s/%s final=%s/%s"
            % (run["label"], res.get("backend_selected", "?"), delay, mode,
               res.get("direct_key_mean_ms", -1), res.get("direct_key_max_ms", "?"),
               res.get("injection_peak_heartbeat_gap_ms", "-"),
               res.get("per_press_direct", "?"),
               res.get("per_press_extra", res.get("mixer_calls_total", "?")),
               res.get("final_level", "?"), res.get("final_muted", "?")))


def main() -> int:
    ap = argparse.ArgumentParser(description="P-5A volume-stall case orchestrator (torture T1)")
    ap.add_argument("--case", required=True,
                    choices=sorted(list(TIMING_CASES) + ["noop-control", "alsa-faults",
                                                         "pipewire-faults", "baseline"]))
    ap.add_argument("--probe", required=True, help="volume-stall-probe binary")
    ap.add_argument("--stub-bin", required=True, help="compiled mixer-stub binary")
    ap.add_argument("--repo-root", required=True, help="source tree under test (git sha)")
    ap.add_argument("--baseline", action="store_true",
                    help="assert the recorded 3970174 red-baseline shape (implies red expected)")
    ap.add_argument("--strict-semantics", action="store_true",
                    help="also assert the exact level ladder + persistence (post-fix verification)")
    args = ap.parse_args()
    # The probe runs with cwd=scratch, so relative tool paths must be
    # resolved against the CALLER's cwd before launch.
    args.probe = os.path.abspath(args.probe)
    args.stub_bin = os.path.abspath(args.stub_bin)
    args.repo_root = os.path.abspath(args.repo_root)
    if not os.path.isfile(args.probe) or not os.access(args.probe, os.X_OK):
        print("probe not executable: %s" % args.probe)
        return 2
    if not os.path.isfile(args.stub_bin) or not os.access(args.stub_bin, os.X_OK):
        print("mixer-stub not executable: %s" % args.stub_bin)
        return 2
    snapshot = git_snapshot(args.repo_root)
    evidence_root = os.path.join(os.getcwd(), "volume-stall-evidence", args.case)
    os.makedirs(evidence_root, exist_ok=True)
    violations = []
    runs = []
    print("P-5A volume-stall case=%s snapshot=%s target=<%dms" % (args.case, snapshot, TARGET_MS))

    if args.case == "baseline":
        for case, cfg in TIMING_CASES.items():
            run = run_probe(case, "volume-" + cfg["backend"], args.probe, args.stub_bin,
                            cfg["backend"], True, "normal", cfg["delay"], False,
                            snapshot, evidence_root)
            runs.append(run)
            print(summarize(run, cfg["backend"], cfg["delay"], "normal"))
            check_common(run, violations, cfg["backend"])
            check_timing(run, violations, case, cfg["backend"], True, args.strict_semantics)
    elif args.case in TIMING_CASES:
        cfg = TIMING_CASES[args.case]
        run = run_probe(args.case, "volume-" + cfg["backend"], args.probe, args.stub_bin,
                        cfg["backend"], True, "normal", cfg["delay"], False,
                        snapshot, evidence_root)
        runs.append(run)
        print(summarize(run, cfg["backend"], cfg["delay"], "normal"))
        check_common(run, violations, cfg["backend"])
        check_timing(run, violations, args.case, cfg["backend"],
                     args.baseline, args.strict_semantics)
    elif args.case == "noop-control":
        run = run_probe("noop-control", "volume-noop", args.probe, args.stub_bin,
                        "none", False, "normal", 0, True, snapshot, evidence_root)
        runs.append(run)
        print(summarize(run, "none", 0, "normal"))
        check_common(run, violations, "none")
        check_timing(run, violations, "noop-control", "none", False, args.strict_semantics)
    else:
        backend = args.case.split("-")[0]
        for mode_label, stub_mode, present, delay in FAULT_MODES:
            label = "%s-%s" % (args.case, mode_label)
            run = run_probe(label, "fault-" + backend, args.probe, args.stub_bin,
                            backend, present, stub_mode, delay, False,
                            snapshot, evidence_root)
            runs.append(run)
            print(summarize(run, backend, delay, stub_mode if present else "missing"))
            check_common(run, violations, backend if present else "none")
            check_fault(run, violations, backend, stub_mode, present)

    for run in runs:
        if run["result"] is not None:
            print("RESULT " + json.dumps(run["result"], sort_keys=True))
        print("evidence: %s  scratch: %s" % (run["ev_dir"], run["scratch"]))
    for v in violations:
        print(v)
    verdict = "PASS" if not violations else "FAIL"
    with open(os.path.join(evidence_root, "verdict.txt"), "w", encoding="utf-8") as f:
        f.write("case=%s snapshot=%s verdict=%s\n" % (args.case, snapshot, verdict))
        for v in violations:
            f.write(v + "\n")
    print("CASE %s: %s (%d invariant violation(s), %d run(s))"
          % (args.case, verdict, len(violations), len(runs)))
    return 0 if not violations else 1


if __name__ == "__main__":
    sys.exit(main())