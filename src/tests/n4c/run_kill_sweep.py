#!/usr/bin/env python3
"""N4c two-sided kill-sweep driver.

For both writers the parent seeds a live OLD document, launches a child that
writes a NEW document, waits for a ready marker, then SIGKILLs at staggered
points across the write window. The live target must ALWAYS be OLD or NEW and
COMPLETE for the atomic writer; the legacy in-place writer (required-red
control) MUST be caught leaving a partial/target-corrupt document somewhere in
the grid. If the legacy arm never shows corruption the gate has no defect-
detection power and the run fails.

Exit 0 = both arms meet expectation, 1 = rig or expectation failure.
"""
import argparse
import os
import signal
import subprocess
import sys
import time

OLD = (b'OLD-DOCUMENT-LIVE-CONTENT ' * 60)          # ~1.5 KB
NEW = bytes((i % 251 for i in range(4 * 1024 * 1024)))  # 4 MB, deterministic


def classify(data):
    if data == OLD:
        return "OLD"
    if data == NEW:
        return "NEW"
    return "CORRUPT"


def kill_once(saver, mode, workdir, delay_us):
    target = os.path.join(workdir, "live.bin")
    ready = os.path.join(workdir, "ready")
    payload = os.path.join(workdir, "payload.bin")
    for p in (target, ready, payload):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    with open(target, "wb") as f:
        f.write(OLD)
    with open(payload, "wb") as f:
        f.write(NEW)

    proc = subprocess.Popen([saver, mode, target, payload, ready],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 5.0
    while not os.path.exists(ready):
        if proc.poll() is not None or time.monotonic() > deadline:
            break
        time.sleep(0.0005)
    if not os.path.exists(ready):
        # Child died before arming — rig fault, not a product verdict.
        try:
            proc.kill()
        except ProcessLookupError:
            pass
        proc.wait()
        raise RuntimeError("ready marker never appeared (mode=%s)" % mode)

    if delay_us:
        time.sleep(delay_us / 1_000_000.0)
    try:
        os.kill(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    proc.wait()

    with open(target, "rb") as f:
        return classify(f.read())


def run_arm(saver, mode, evdir, grid, iters):
    armdir = os.path.join(evdir, mode)
    os.makedirs(armdir, exist_ok=True)
    counts = {"OLD": 0, "NEW": 0, "CORRUPT": 0}
    samples = []
    for d in grid:
        for _ in range(iters):
            c = kill_once(saver, mode, armdir, d)
            counts[c] += 1
            samples.append((d, c))
    return counts, samples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--saver", required=True)
    ap.add_argument("--evdir", required=True)
    ap.add_argument("--iters", type=int, default=2)
    ap.add_argument("--max-us", type=int, default=8000)
    ap.add_argument("--step-us", type=int, default=400)
    args = ap.parse_args()

    os.makedirs(args.evdir, exist_ok=True)
    grid = list(range(0, args.max_us + 1, args.step_us))

    atomic, atomic_samples = run_arm(args.saver, "saver-atomic",
                                    args.evdir, grid, args.iters)
    legacy, legacy_samples = run_arm(args.saver, "saver-legacy",
                                     args.evdir, grid, args.iters)

    print("atomic arm :", atomic)
    print("legacy arm :", legacy)

    ok = True
    if atomic["CORRUPT"] != 0:
        print("FAIL: atomic writer left %d corrupt live files" % atomic["CORRUPT"])
        ok = False
    else:
        print("PASS: atomic writer always left OLD-or-NEW complete")
    if legacy["CORRUPT"] == 0:
        print("FAIL: legacy control never corrupted — gate has no detection power")
        ok = False
    else:
        print("PASS: required-red legacy control corrupted %d/%d kills "
              "(gate detects the defect)" % (legacy["CORRUPT"],
                                             sum(legacy.values())))

    with open(os.path.join(args.evdir, "killsweep-summary.txt"), "w") as f:
        f.write("atomic=%r\nlegacy=%r\n" % (atomic, legacy))
        f.write("atomic_samples=%r\nlegacy_samples=%r\n"
                % (atomic_samples, legacy_samples))

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
