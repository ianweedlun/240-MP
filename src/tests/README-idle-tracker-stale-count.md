# IDLE-01 stale idle-count regression gate (two-sided, checked-in)

Permanent CTest gate for the IDLE-01 ambush: `IdleTracker::setMpvActive(false)`
/ `setScriptActive(false)` used to unblock the 1 s tick **without resetting the
frozen pre-playback idle count**. Pre-idle to threshold−1, start playback,
return with no keypress (natural EOF, script exit) → the saver fired on the
FIRST tick (~1 s at the production 60 s threshold). The class fix
(`feature/idle01-unblock-reset`, shared `resetIdleState()` on the
blocking→unblocked edge) makes any caller safe by construction; this gate
fails by design if that guard ever regresses.

Promoted from the torture lab harness (snapshot `e201e7d`, cards
`t_ada6fb9b` / `t_878e15cd` / `t_7ce5b803` / `t_146a1fd9`). Durable evidence:
`~/.hermes/cache/evidence/idle01-e201e7d/` (pre-fix torture) and
`~/.hermes/cache/evidence/idle01-fix-t_7ce5b803/` (fix verification).

## Files

- `IdleTrackerStaleCountProbe.cpp` — instrument. Compiles the **real**
  `src/input/IdleTracker.cpp` (Qt6 Core+Gui only — no QML, mpv, audio or
  network; runs on every CI lane including arm64 Qt 6.4). One arm per process,
  fresh tracker. Always exits 0 when it ran; writes the measurement JSON and
  prints a `RESULT` line. Assertions live in the orchestrator.
- `run_idle_tracker_stale_count_case.py` — orchestrator (stdlib python3).
  Owns env, timeout, evidence and the `INVARIANT … VIOLATED` assertions.
- CMake (root `CMakeLists.txt`, inside `BUILD_TESTS`): target
  `idle-tracker-stale-count-probe` + four ctest entries.

## Run

```sh
cmake -B build-test -DBUILD_TESTS=ON
cmake --build build-test --target idle-tracker-stale-count-probe -j
ctest --test-dir build-test -R idle-tracker-stale-count --output-on-failure
```

Full suite: `ctest --test-dir build-test --output-on-failure` (build ALL
targets first — a stale sibling binary can fake a failure).

Sanitized (matches the torture evidence; ASan+UBSan ride global flags, the
orchestrator sets `ASAN_OPTIONS=detect_leaks=1` and asserts sanitizer-clean):

```sh
cmake -B build-asan -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-g -fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --target idle-tracker-stale-count-probe -j
ctest --test-dir build-asan -R idle-tracker-stale-count --output-on-failure
```

## Cases and arms

| ctest case | arm | sequence | pre-fix | post-fix |
|---|---|---|---|---|
| `idle-tracker-stale-count-mpv` | D | pre-idle to t−1 → `setMpvActive(true)` → hold 3.5 s → keyless return | **FAIL** (DEFECT ~0.5 s / 1 tick) | PASS ~3.4–4 s / 4 ticks |
| `idle-tracker-stale-count-script` | S | same via `setScriptActive` | **FAIL** (DEFECT ~0.5 s / 1 tick) | PASS ~3.4–4 s / 4 ticks |
| `idle-tracker-stale-count-keypress` | H1 | same pre-idle + block, ONE real non-auto-repeat KeyPress at return | PASS | PASS |
| `idle-tracker-stale-count-fresh` | H2 | `resetActivity()` baseline (no pre-idle), block, keyless return | PASS | PASS |

Every arm asserts the **freeze invariant**: ZERO `idleSecondsChanged`
emissions during the 3.5 s block, count held at t−1 (H2: 0).

## Timing bands (threshold t, default 4 s)

The 1 s tick is phase-locked to construction, not to return: a healthy count
starting at 0 fires on the t-th tick AFTER return, and the first of those
ticks lands 0..1000 ms after return.

- healthy: latency ≥ (t−1)·1000·0.95 (2850 ms at t=4 — Qt CoarseTimer fires up
  to 5% early) AND ≤ (t+2)·1000 (load slack for busy CI runners) AND ≥ t real
  ticks after return (zero-value reset emissions excluded from the count).
- defect: latency ≤ 1100 ms AND ≤ 2 real ticks — the first-tick fire from the
  stale count. The 1100..2850 ms gap is unambiguous; anything in it (or a rig
  sanity failure) is INCONCLUSIVE and fails the gate.

Measured anchors (ASan/UBSan, quiet lab host): pre-fix D/S 494/497 ms, 1 tick;
post-fix D/S 3498/3496 ms, H1 3494 ms, H2 3466 ms — all 4 ticks.

`activityDetected` counts: the post-fix unblock edge emits one itself, so the
defect arms assert **exactly 1** (edge only, keyless; pre-fix emits 0 → fails
by design). The controls legitimately differ by side — H1: 1 pre-fix
(keypress) / 2 post-fix (edge+keypress); H2: 1 pre-fix (baseline) / 2 post-fix
(baseline+edge) — and assert the documented 1..2 band. The timing assertions
are the regression gate and are identical on both sides.

## Threshold override

`IDLE01_THRESHOLD_S` (orchestrator env, default **4**, min 3; passed to the
probe as argv[3]). Real 1000 ms ticks always — no timer scaling. 4 keeps each
arm ~11 s; `IDLE01_THRESHOLD_S=60` reproduces production fidelity at ~135 s
per arm (manual runs; ctest TIMEOUT is 300 s). The block hold stays 3.5 s.

## Two-sided control run (pre-fix proof)

`IDLE01_SOURCE_ROOT` (CMake cache PATH, default = the configure tree) selects
which checkout's `src/input/IdleTracker.cpp` the probe compiles. Point it at a
pre-fix checkout — the control tree is never edited:

```sh
cmake -B build-ctl -DBUILD_TESTS=ON \
  -DIDLE01_SOURCE_ROOT=/path/to/pre-fix-checkout   # e.g. the fix's direct parent
cmake --build build-ctl --target idle-tracker-stale-count-probe -j
ctest --test-dir build-ctl -R idle-tracker-stale-count --output-on-failure
# expect: mpv + script FAIL (first-tick ambush), keypress + fresh PASS
```

The probe embeds the compiled tree's git HEAD (`IDLE01_SOURCE_REV`); the
orchestrator cross-checks it against `--source-root` so a mis-aimed control
build fails loudly instead of silently testing the wrong class.

`--baseline` (orchestrator flag, manual proof mode, not a ctest entry) asserts
the recorded pre-fix RED shape: defect arms DEFECT / 1 tick / ≤1100 ms / 0
detections, controls PASS with pre-fix counts. It proves the plain gate fails
for the right reason. On the post-fix class `--baseline` fails by design.

## Notes

- `RUN_SERIAL TRUE`: timing gate — keeps the 1 s ticks quiet under `ctest -j4`.
- Evidence per run: `<build-dir>/idle-tracker-stale-count-evidence/<case>/`
  (probe stdout/stderr, `arm.json`, `env.txt`, `verdict.txt`).
- QML paired `resetActivity()` calls are deliberately KEPT (defense in depth);
  this gate tests the class guard, which must hold with or without them.
- App-level mpv/script paths were MASKED even pre-fix (QML paired resets);
  the offscreen-app integration driver lives in the durable evidence roots
  above, not in ctest — the two-sided signal is at class level.