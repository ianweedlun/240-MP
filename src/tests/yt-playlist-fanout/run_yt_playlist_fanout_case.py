#!/usr/bin/env python3
"""Orchestrator for the YT playlist fanout two-sided CTest gate.

The RED arm runs the sha256-pinned pre-fix product text; the GREEN arm runs
product text extracted verbatim from the tree at build time. Both arms run an
N sweep with identical inputs, and the gate passes only when RED still shows
the defect shape AND GREEN shows the fixed shape. That makes the negative
control part of the test: weakening the product fix breaks GREEN, while
tampering with the pinned RED text breaks the control check.

Cases: dedup, retention, controls, sanitizers, two-sided (all).
Standard library only.
"""
import argparse
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile

PRE_FIX_HASHES = {
    "playlistIdFromToken.pre_fix.inc":
        "898f48fed1c3a8bc6cdf2c9a65fe3affbc3b3c2e9ee4afe1b7d362881c507a90",
    "readPlaylistEntries.pre_fix.inc":
        "332340ae019e5bd45f2058d4b78208120855d69eca4b568e1db4fb82ebf10ee3",
}
DIAG_RE = re.compile(r"ERROR: AddressSanitizer|runtime error:|LeakSanitizer|"
                     r"SUMMARY: (Address|Undefined)Sanitizer")

# Thresholds inherited from the confirmed torture harness (analysis.json).
RED_EXPONENT_MIN = 1.6      # >=1.6 = super-linear/quadratic defect still present
GREEN_EXPONENT_MAX = 1.35   # <=1.35 = near-linear fix
RSS_FLAT_KB = 4000          # last-three RSS spread on the fixed side

SAN_ENV = dict(os.environ)
SAN_ENV["ASAN_OPTIONS"] = "detect_leaks=1:quarantine_size_mb=0:halt_on_error=1"
SAN_ENV["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"


class Check:
    def __init__(self):
        self.failures = []
        self.checks = 0

    def that(self, ok, message):
        self.checks += 1
        if not ok:
            self.failures.append(message)
        return bool(ok)


def fit_exponent(points):
    xs = [math.log(n) for n, t in points if t > 0]
    ys = [math.log(t) for n, t in points if t > 0]
    if len(xs) < 2:
        return None
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den if den else None


def run_probe(binary, mode, n, workdir, tag, extra=None, repeats=7, asan=False):
    data = os.path.join(workdir, "data-%s-%s-n%d" % (tag, mode, n))
    cmd = [binary, "--mode", mode, "--n", str(n), "--data", data,
           "--repeats", str(repeats)]
    if extra:
        cmd += extra
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          env=SAN_ENV if asan else dict(os.environ), timeout=180)
    lines = [line for line in proc.stdout.splitlines() if line.startswith("{")]
    if not lines:
        raise RuntimeError("no JSON from %r (rc=%d): %s / %s" %
                           (cmd, proc.returncode, proc.stdout[-400:], proc.stderr[-400:]))
    obj = json.loads(lines[-1])
    obj["tag"] = tag
    obj["rc"] = proc.returncode
    obj["diagnostics"] = DIAG_RE.findall(proc.stderr)
    return obj


def verify_provenance(check, pre_fix_dir):
    if not pre_fix_dir:
        check.that(False, "--pre-fix-dir is required (cannot verify RED control text)")
        return {}
    hashes = {}
    for name, expected in PRE_FIX_HASHES.items():
        path = os.path.join(pre_fix_dir, name)
        if not os.path.isfile(path):
            check.that(False, "missing pinned RED control text: " + path)
            hashes[name] = None
            continue
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        hashes[name] = digest
        check.that(digest == expected,
                   "RED control %s changed: sha256=%s expected=%s" %
                   (name, digest, expected))
    return hashes


def dedup_case(check, probe, workdir, evidence):
    ns = [100, 200, 400, 800, 1600]
    runs = []
    for n in ns:
        for mode in ("red", "green"):
            runs.append(run_probe(probe, mode, n, workdir, "timing",
                                  ["--no-retain"], repeats=11))
            runs.append(run_probe(probe, mode, n, workdir, "timing-same",
                                  ["--no-retain", "--all-same"], repeats=11))
    evidence["dedup_runs"] = runs

    def pick(mode, n, tag):
        return next(r for r in runs if r["mode"] == mode and r["n"] == n and r["tag"] == tag)

    red_d = [(n, max(0.0, pick("red", n, "timing")["dedup_us"] -
                            pick("red", n, "timing-same")["dedup_us"])) for n in ns]
    grn_d = [(n, max(0.0, pick("green", n, "timing")["dedup_us"] -
                            pick("green", n, "timing-same")["dedup_us"])) for n in ns]
    red_b = fit_exponent(red_d)
    grn_b = fit_exponent(grn_d)
    if grn_b is None and max(t for _, t in grn_d) <= 200:
        grn_b = 0.0
    evidence["dedup"] = {"red_exponent": red_b, "green_exponent": grn_b,
                         "red_delta_us": red_d, "green_delta_us": grn_d}

    check.that(red_b is not None and red_b >= RED_EXPONENT_MIN,
               "RED dedup exponent %.3f is below %.2f: control no longer shows O(N^2)" %
               (red_b or -1, RED_EXPONENT_MIN))
    check.that(grn_b is not None and grn_b <= GREEN_EXPONENT_MAX,
               "GREEN dedup exponent %.3f exceeds %.2f (quadratic regression)" %
               (grn_b if grn_b is not None else -1, GREEN_EXPONENT_MAX))

    constants = pick("green", ns[0], "timing")
    id_cap, row_budget = constants["id_cap"], constants["row_budget"]
    evidence["constants"] = constants
    # Structural sanity bounds: a cap set to INT_MAX or a budget set to zero
    # would make the plateau assertions vacuous, so reject them explicitly.
    check.that(1 <= id_cap <= 1000, "extracted id cap %d outside [1,1000]" % id_cap)
    check.that(500 <= row_budget <= 10000,
               "extracted row budget %d outside [500,10000]" % row_budget)
    check.that(1 <= constants["concurrency_cap"] <= 2,
               "concurrency cap %d outside [1,2]" % constants["concurrency_cap"])

    for n in ns:
        for mode in ("red", "green"):
            r = pick(mode, n, "timing")
            check.that(r["rc"] == 0 and not r["diagnostics"],
                       "dedup %s n=%d: rc=%d diagnostics=%s" %
                       (mode, n, r["rc"], r["diagnostics"]))
            check.that(r["max_active"] <= constants["concurrency_cap"],
                       "dedup %s n=%d: concurrency %d > %d" %
                       (mode, n, r["max_active"], constants["concurrency_cap"]))
            check.that(r["accepted"] == (n if mode == "red" else min(n, id_cap)),
                       "dedup %s n=%d: accepted=%d (expected %d)" %
                       (mode, n, r["accepted"],
                        n if mode == "red" else min(n, id_cap)))
    return id_cap, row_budget


def retention_case(check, probe, workdir, evidence, id_cap, row_budget):
    ns = [25, 100, 200, 400, 600]
    runs = []
    for n in ns:
        for mode in ("red", "green"):
            runs.append(run_probe(probe, mode, n, workdir, "retention", repeats=5))
    evidence["retention_runs"] = runs

    def pick(mode, n):
        return next(r for r in runs if r["mode"] == mode and r["n"] == n)

    per_fetch = pick("red", ns[0])["rows_per_fetch"]
    red_rows = [pick("red", n)["retained"] for n in ns]
    grn_rows = [pick("green", n)["retained"] for n in ns]
    red_rss = [pick("red", n)["rss_after_kb"] for n in ns]
    grn_rss = [pick("green", n)["rss_after_kb"] for n in ns]
    evidence["retention"] = {"red_rows": red_rows, "green_rows": grn_rows,
                             "red_rss_kb": red_rss, "green_rss_kb": grn_rss}

    check.that(all(rows == n * per_fetch for n, rows in zip(ns, red_rows)),
               "RED rows not N*per_fetch: %s" % red_rows)
    check.that(all(red_rows[i] < red_rows[i + 1] for i in range(len(red_rows) - 1)),
               "RED retained rows not strictly monotonic: %s" % red_rows)
    check.that(all(rows <= row_budget for rows in grn_rows),
               "GREEN rows exceed budget %d: %s" % (row_budget, grn_rows))
    check.that(len(set(grn_rows[-3:])) == 1 and grn_rows[-1] == row_budget,
               "GREEN rows do not plateau at %d: %s" % (row_budget, grn_rows))
    check.that(all(r["fetches"] <= id_cap for r in runs if r["mode"] == "green"),
               "GREEN fetch fanout exceeds id cap %d" % id_cap)
    check.that(all(r["accepted"] == r["n"] for r in runs if r["mode"] == "red"),
               "RED accepted ids are capped (control must stay unbounded)")

    if all(r >= 0 for r in red_rss + grn_rss):
        check.that(all(red_rss[i] < red_rss[i + 1] for i in range(len(red_rss) - 1)),
                   "RED RSS not strictly monotonic: %s" % red_rss)
        check.that(max(grn_rss[-3:]) - min(grn_rss[-3:]) < RSS_FLAT_KB,
                   "GREEN RSS spread %d KB >= %d" %
                   (max(grn_rss[-3:]) - min(grn_rss[-3:]), RSS_FLAT_KB))
    for r in runs:
        check.that(r["rc"] == 0 and not r["diagnostics"],
                   "retention %s n=%d: rc=%d diagnostics=%s" %
                   (r["mode"], r["n"], r["rc"], r["diagnostics"]))
        check.that(r["max_active"] <= r["concurrency_cap"],
                   "retention %s n=%d: concurrency %d > %d" %
                   (r["mode"], r["n"], r["max_active"], r["concurrency_cap"]))
    return per_fetch


def controls_case(check, probe, workdir, evidence, per_fetch, id_cap):
    specs = [
        ("zero", 1, ["--zero"], "red", 0),
        ("one", 1, [], "red", 1),
        ("dups-one", 1, ["--dups", "10"], "red", 1),
        ("dups-ten", 10, ["--dups", "10"], "red", 10),
        ("comments-malformed", 10, ["--comments", "--malformed"], "red", 10),
        ("green-dups", 10, ["--dups", "10"], "green", min(10, id_cap)),
    ]
    runs = [run_probe(probe, mode, n, workdir, tag, extra, repeats=3)
            for tag, n, extra, mode, _ in specs]
    evidence["controls"] = runs
    for (tag, n, extra, mode, expected), r in zip(specs, runs):
        check.that(r["accepted"] == expected,
                   "control %s: accepted=%d expected=%d" % (tag, r["accepted"], expected))
        check.that(r["fetches"] <= min(expected, id_cap) if mode == "green"
                   else r["fetches"] == expected,
                   "control %s: fetches=%d unexpected" % (tag, r["fetches"]))
        if tag == "zero":
            check.that(r["retained"] == 0, "control zero: retained=%d" % r["retained"])
        check.that(r["rc"] == 0 and not r["diagnostics"],
                   "control %s: rc=%d diagnostics=%s" % (tag, r["rc"], r["diagnostics"]))
        check.that(r["max_active"] <= r["concurrency_cap"],
                   "control %s: concurrency %d > %d" %
                   (tag, r["max_active"], r["concurrency_cap"]))


def sanitizer_case(check, asan_probe, workdir, evidence, case_name="sanitizers"):
    if not asan_probe or not os.path.isfile(asan_probe):
        evidence[case_name] = {"skipped": "no sanitizer probe on this platform"}
        return
    runs = []
    for mode in ("red", "green"):
        for n in (100, 400):
            runs.append(run_probe(asan_probe, mode, n, workdir, "asan", repeats=5, asan=True))
    runs.append(run_probe(asan_probe, "red", 10, workdir, "asan-dups",
                          ["--dups", "10"], repeats=3, asan=True))
    runs.append(run_probe(asan_probe, "green", 10, workdir, "asan-controls",
                          ["--comments", "--malformed"], repeats=3, asan=True))
    evidence[case_name] = runs
    for r in runs:
        check.that(r["rc"] == 0,
                   "sanitizer %s n=%d: rc=%d" % (r["mode"], r["n"], r["rc"]))
        check.that(not r["diagnostics"],
                   "sanitizer %s n=%d: %s" % (r["mode"], r["n"], r["diagnostics"]))


def real_retention_case(check, retention_tests, evidence):
    if not retention_tests or not os.path.isfile(retention_tests):
        check.that(False, "real retention test binary missing: %s" % retention_tests)
        return
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    proc = subprocess.run([retention_tests], capture_output=True, text=True,
                          env=env, timeout=180)
    evidence["real_retention"] = {"rc": proc.returncode,
                                  "stdout": proc.stdout[-2000:],
                                  "stderr": proc.stderr[-2000:]}
    check.that(proc.returncode == 0,
               "real YouTubeBackend retention test failed (rc=%d): %s %s" %
               (proc.returncode, proc.stdout[-600:], proc.stderr[-600:]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="two-sided",
                    choices=["dedup", "retention", "controls", "sanitizers",
                             "two-sided"])
    ap.add_argument("--probe", required=True)
    ap.add_argument("--probe-asan")
    ap.add_argument("--retention-tests")
    ap.add_argument("--pre-fix-dir")
    ap.add_argument("--out")
    args = ap.parse_args()

    check = Check()
    id_cap = row_budget = per_fetch = 0
    evidence = {"scenario": "YT-PLAYLIST-FETCH-FANOUT-UNBOUNDED-01",
                "case": args.case, "probe": os.path.basename(args.probe)}
    with tempfile.TemporaryDirectory(prefix="ytfp-ctest-") as workdir:
        if args.case in ("two-sided", "controls", "dedup", "retention"):
            verify_provenance(check, args.pre_fix_dir)
        if args.case in ("two-sided", "dedup"):
            id_cap, row_budget = dedup_case(check, args.probe, workdir, evidence)
        if args.case in ("two-sided", "retention"):
            if "id_cap" not in evidence:
                base = run_probe(args.probe, "green", 100, workdir, "constants", repeats=1)
                id_cap, row_budget = base["id_cap"], base["row_budget"]
            evidence.setdefault("constants", {"id_cap": id_cap, "row_budget": row_budget})
            per_fetch = retention_case(check, args.probe, workdir, evidence, id_cap, row_budget)
        if args.case in ("two-sided", "controls"):
            if "constants" not in evidence:
                base = run_probe(args.probe, "green", 100, workdir, "constants", repeats=1)
                id_cap = base["id_cap"]
                evidence.setdefault("constants", base)
            if "rows_per_fetch" not in evidence:
                base = run_probe(args.probe, "red", 1, workdir, "constants", repeats=1)
                per_fetch = base["rows_per_fetch"]
            controls_case(check, args.probe, workdir, evidence, per_fetch, id_cap)
        if args.case in ("two-sided", "sanitizers"):
            sanitizer_case(check, args.probe_asan, workdir, evidence)
        if args.case == "two-sided":
            real_retention_case(check, args.retention_tests, evidence)

    evidence["checks_run"] = check.checks
    evidence["failures"] = check.failures
    evidence["verdict"] = "PASS" if not check.failures else "FAIL"
    if args.out:
        parent = os.path.dirname(os.path.abspath(args.out))
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(evidence, f, indent=2)
    print("yt-playlist-fanout %s: %d checks, %d failures" %
          (args.case, check.checks, len(check.failures)))
    for failure in check.failures:
        print("FAIL: " + failure)
    return 0 if not check.failures else 1


if __name__ == "__main__":
    sys.exit(main())
