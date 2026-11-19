#!/usr/bin/env python3
"""CFG-01 G1/G3/G6 kill-sweep orchestrator (test plan §6 G1, §4, §8).

Runs: classifier selftest (rig-validity) -> genbig template -> POSIX killprobe
sweep -> Qt classify batch -> G1/G3/G6 verdicts + manifest.

Variant expectations (two-sided, §6 G5):
  FIX: G1 invariants hold (ZEROED==0, TRUNCATED==0, MISSING==0, COMPLETE>=1,
       valid>=160) -> exit 0. G3: live intact + saver exit!=0 + warning.
  P2:  G1 invariants hold (atomic writer alone survives kills) -> exit 0.
       G3: live intact (exit code not asserted — P2 surface is void).
  PRE: required-RED. Defect signature ZEROED+TRUNCATED>=1 AND COMPLETE>=1
       -> exit 2 (gate FAILS by design on the defect snapshot). Canary miss
       -> calibrate grid (3000/6000/12000/24000 us, cap 4 extensions);
       still no window -> exit 3 RIG-FAIL (never a product verdict).

Exit taxonomy: 0 pass / 2 product fail (or PRE required-red confirmed) /
1 rig error / 3 rig-validity.
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time

F2_SIZE = 68410
F2_SHA = "c7bf857d97a895151d73c13eadd35a184998f8bbb12d549651934b03cd1888e1"
ALLOWED = {"config.json", "config.json.bak"}
TEMP_HINTS = ("qt_temp", ".tmp", "config.json.")


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], **kw)


def is_temp(name):
    if name in ALLOWED or name.startswith("config.json.corrupt-"):
        return False
    return name.startswith("config.json.") or ".tmp" in name or "qt_temp" in name


def litter_check(evdir, qt_version):
    """G6: per-trial dir listings. Graceful-failure trials (rlimit): zero temp
    entries on ALL Qt versions. Kill trials: zero temps required on Qt>=6.11
    (O_TMPFILE unnamed inode); older Qt leaves orphaned named temps on process
    death — recorded, not a product defect (live-file invariant carries G1)."""
    violations, orphaned, quarantined = [], 0, 0
    major, minor = (int(x) for x in qt_version.split(".")[:2])
    tmpfile_qt = (major, minor) >= (6, 11)
    tdir = os.path.join(evdir, "trials")
    for t in sorted(os.listdir(tdir)):
        lp = os.path.join(tdir, t, "listing.txt")
        if not os.path.exists(lp):
            continue
        names = [n.strip() for n in open(lp) if n.strip()]
        kind = "kill"
        raw = os.path.join(evdir, "raw-rows.jsonl")
        for line in open(raw):
            row = json.loads(line)
            if f"t{row['trial']:03d}" == t:
                kind = row["kind"]
                break
        temps = [n for n in names if is_temp(n)]
        quarantined += sum(1 for n in names if n.startswith("config.json.corrupt-"))
        if temps:
            if kind == "rlimit" or tmpfile_qt:
                violations.append({"trial": t, "kind": kind, "temps": temps})
            else:
                orphaned += len(temps)
    return violations, orphaned, quarantined


def g3_check(evdir, variant):
    """G3 RLIMIT rows: FIX live intact + exit!=0 + warning; PRE 1024B cut +
    exit 0 (silent); P2 live intact (void surface, exit not asserted)."""
    rows = [json.loads(l) for l in open(os.path.join(evdir, "rows.jsonl"))]
    rl = [r for r in rows if r["kind"] == "rlimit"]
    results, ok = [], True
    for r in rl:
        t = f"t{r['trial']:03d}"
        err = open(os.path.join(evdir, "trials", t, "stderr.log"),
                   errors="replace").read()
        warned = "[cfgfile]" in err and ("WARN" in err or "failed" in err)
        if variant == "PRE":
            good = r["size"] == 1024 and r["exit_code"] == 0 and r["class"] == "TRUNCATED"
            sig = "1024B-cut+silent-exit0"
        else:
            intact = r["size"] == F2_SIZE and r["sha256"] == F2_SHA
            if variant == "FIX":
                good = intact and r["exit_code"] != 0 and warned
            else:  # P2
                good = intact
            sig = "live-intact" + ("+checked-failure" if variant == "FIX" else "")
        ok = ok and good
        results.append({"trial": r["trial"], "size": r["size"],
                        "exit_code": r["exit_code"], "warned": warned,
                        "class": r["class"], "pass": good, "signature": sig})
    return ok, results


def sweep_once(args, evdir, max_us):
    os.makedirs(evdir, exist_ok=True)
    # 1. classifier selftest — rig-validity precondition.
    p = run([args.classify, "selftest", evdir, args.golden_dir])
    if p.returncode != 0:
        return {"verdict": "RIG-VALIDITY", "exit": 3, "stage": "selftest"}
    # 2. template generation + pin assertion.
    tmpldir = os.path.join(evdir, "tmpl")
    p = run([args.saver, "genbig", tmpldir])
    if p.returncode != 0:
        return {"verdict": "RIG-ERROR", "exit": 1, "stage": "genbig"}
    src = os.path.join(tmpldir, "config.json")
    b = open(src, "rb").read()
    if len(b) != F2_SIZE or sha256(b) != F2_SHA:
        print(f"RIG ERROR: template pin mismatch size={len(b)} sha={sha256(b)}")
        return {"verdict": "RIG-ERROR", "exit": 1, "stage": "template-pin"}
    shutil.copyfile(src, os.path.join(evdir, "template.json"))
    # 3. POSIX sweep.
    p = run([args.killprobe, args.saver, evdir, max_us, args.step, args.repeats])
    if p.returncode == 3:
        return {"verdict": "RIG-VALIDITY", "exit": 3, "stage": "ready-timeout"}
    if p.returncode != 0:
        return {"verdict": "RIG-ERROR", "exit": 1, "stage": "killprobe"}
    # 4. classify batch.
    p = run([args.classify, "batch", evdir])
    if p.returncode != 0:
        return {"verdict": "RIG-ERROR", "exit": 1, "stage": "classify"}
    summary = json.load(open(os.path.join(evdir, "summary.json")))
    return {"verdict": "OK", "exit": 0, "summary": summary, "max_us": max_us}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--saver", required=True)
    ap.add_argument("--killprobe", required=True)
    ap.add_argument("--classify", required=True)
    ap.add_argument("--evdir", required=True)
    ap.add_argument("--golden-dir", required=True)
    ap.add_argument("--variant", required=True, choices=["PRE", "P2", "FIX"])
    ap.add_argument("--qt-version", default="6.11")
    ap.add_argument("--max-us", type=int, default=1550)
    ap.add_argument("--step", type=int, default=50)
    ap.add_argument("--repeats", type=int, default=5)
    args = ap.parse_args()

    started = time.time()
    extensions = []
    max_us = args.max_us
    result = sweep_once(args, args.evdir, max_us)
    # PRE canary calibration (§6 G1.7): the sweep must bracket the write window.
    if args.variant == "PRE" and result["verdict"] == "OK":
        for ext_max in (3000, 6000, 12000, 24000):
            c = result["summary"]["counts"]
            hit = c["ZEROED"] + c["TRUNCATED"] >= 1
            spans = c["COMPLETE-POSTWRITE"] >= 1
            if hit and spans:
                break
            extensions.append({"from_max_us": max_us, "to_max_us": ext_max,
                               "counts_before": dict(c)})
            max_us = ext_max
            evdir = f"{args.evdir}-ext{len(extensions)}"
            result = sweep_once(args, evdir, max_us)
            if result["verdict"] != "OK":
                break
        else:
            c = result.get("summary", {}).get("counts", {})
            if not (c.get("ZEROED", 0) + c.get("TRUNCATED", 0) >= 1
                    and c.get("COMPLETE-POSTWRITE", 0) >= 1):
                result = {"verdict": "RIG-FAIL", "exit": 3,
                          "stage": "calibration-exhausted"}

    manifest = {
        "gate": "G1+G3+G6", "variant": args.variant,
        "host": platform.node(), "platform": platform.platform(),
        "qt_version": args.qt_version,
        "grid": {"max_us": max_us, "step_us": args.step, "repeats": args.repeats},
        "extensions": extensions,
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(started)),
        "runtime_s": round(time.time() - started, 1),
    }

    if result["verdict"] != "OK":
        manifest.update(result)
        json.dump(manifest, open(os.path.join(args.evdir, "manifest.json"), "w"), indent=2)
        print(f"KILL-SWEEP {args.variant}: {result['verdict']} (stage={result.get('stage')})")
        return result["exit"]

    summary = result["summary"]
    c = summary["counts"]
    evdir = args.evdir if not extensions else f"{args.evdir}-ext{len(extensions)}"
    litter, orphaned, quarantined = litter_check(evdir, args.qt_version)
    g3ok, g3rows = g3_check(evdir, args.variant)
    manifest["summary"] = summary
    manifest["g6"] = {"litter_violations": litter, "orphaned_temps_old_qt": orphaned,
                      "quarantine_files": quarantined,
                      "pass": not litter and quarantined == 0}
    manifest["g3"] = {"pass": g3ok, "rows": g3rows}

    if args.variant == "PRE":
        signature = (c["ZEROED"] + c["TRUNCATED"] >= 1 and c["COMPLETE-POSTWRITE"] >= 1)
        manifest["g1"] = {"pass": False, "required_red": True,
                          "defect_signature_confirmed": signature,
                          "expectation": "ZEROED+TRUNCATED>=1 AND COMPLETE>=1, exit 2"}
        manifest["verdict"] = "DEFECT-REPRODUCED" if signature else "SIGNATURE-MISS"
        manifest["exit"] = 2 if signature else 3
        json.dump(manifest, open(os.path.join(args.evdir, "manifest.json"), "w"), indent=2)
        print(f"KILL-SWEEP PRE: {manifest['verdict']} counts={c} -> exit {manifest['exit']}")
        return manifest["exit"]

    # FIX / P2: the invariants are the contract.
    g1ok = (c["ZEROED"] == 0 and c["TRUNCATED"] == 0 and c["MISSING"] == 0
            and c["COMPLETE-POSTWRITE"] >= 1 and summary["valid"] >= 160
            and summary["kills"] >= 160)
    manifest["g1"] = {"pass": g1ok,
                      "invariants": "ZEROED==0 TRUNCATED==0 MISSING==0 COMPLETE>=1 valid>=160"}
    all_ok = g1ok and g3ok and manifest["g6"]["pass"]
    manifest["verdict"] = "PASS" if all_ok else "FAIL"
    manifest["exit"] = 0 if all_ok else 2
    json.dump(manifest, open(os.path.join(args.evdir, "manifest.json"), "w"), indent=2)
    print(f"KILL-SWEEP {args.variant}: {manifest['verdict']} counts={c} "
          f"g3={g3ok} g6={manifest['g6']['pass']} -> exit {manifest['exit']}")
    return manifest["exit"]


if __name__ == "__main__":
    sys.exit(main())