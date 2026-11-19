# Volume-stall verification report — <16 ms volume keys on both backends

Task: t_7621e31a (torture T1 acceptance gate). Verified tree: `343c508`
(PR #120, `feature/t1-volume-decouple`, stacked on #118) **plus this card's
harness-only edge-case additions** — product code is byte-identical to
`343c508`. Date: 2026-11-19, lab host (Ubuntu, 16 cores, loadavg ≈ 1.0–1.2
during runs, recorded per-run in `env.txt`).

Method: the checked-in regression harness (`README-volume-stall.md`) — real
`InputManager` file-channel injection (`/tmp/240mp_key.in`), real
`VolumeManager` + real ALSA/PipeWire mixer classes, offscreen Qt, compiled
PATH-stub `amixer`/`wpctl` carrying the delay. Fresh ASan/UBSan build
(`-fsanitize=address,undefined -fno-omit-frame-pointer`, `BUILD_TESTS=ON`,
`TELEMETRY=ON`), `ASAN_OPTIONS=detect_leaks=1`, per-run scratch
`XDG_CONFIG_HOME`/`XDG_DATA_HOME`/`HOME`/`PATH`. **All mixer delay lives in
scratch PATH stubs; no system files were changed.** The only shared path is
the fixed `/tmp/240mp_key.in` debug channel (removed before/after every run;
cases run serially).

## Before / after (200 ms stub delay unless noted)

Before = recorded `3970174` lab baseline (README table). After = this run,
strict-semantics orchestrator verdicts, all PASS.

| Case | Before mean / max / hb peak (ms) | After mean / max / hb peak (ms) | After ctor (ms) |
|---|---|---|---|
| ALSA delay200 | 1466.33 / 1470 / 1505 | **0.00 / 0 / 7** | 0 (was 207) |
| PipeWire delay200 | 211.0 / 214 / 219 | **0.00 / 0 / 11** | 0 |
| ALSA delay0 | 23.67 / 25 / 45 | **0.00 / 0 / 8** | 0 |
| PipeWire delay0 | 3.33 / 4 / 16 | **0.00 / 0 / 6** | 0 |
| noop control | — | 0.00 / 0 / 6 | 0 |

Baseline (no-injection) heartbeat floor: 6 ms in every after-run — same as
the recorded pre-fix floor; the rig itself never stalls. Every 200 ms run is
<16 ms on constructor, direct mean/max and peak UI heartbeat gap, on **both**
backends. Zero-delay controls do not regress (they improve: the remaining
per-press spawns are gone).

Edge cases (new, this card; 200 ms delay, real input path):

| Case | direct mean / max | edge hb peak | burst children (25 presses) | clamp no-op signals / children | OSD signals (exact) | final / persisted |
|---|---|---|---|---|---|---|
| alsa-edge | 0.00 / 0 | 10 ms | 2 | 0 / 0 | 12 (= 10 effective + mute + unmute) | 0 / unmuted, persisted identical |
| pipewire-edge | 0.00 / 0 | 13 ms | 3 | 0 / 0 | 22 (= 20 effective + mute + unmute) | 0 / unmuted, persisted identical |

The edge heartbeat gate covers the whole phase including the debounced
persist flush window. Mute was injected through the real file channel
(`volmute`), delivered, then unmuted by direct `toggleMute()` — both
backends.

## Mixer invocation counts (exact argv logs)

ALSA is **no longer 7 spawns per press**. `alsa-delay200/mixer.log`:

```
scontrols          ← constructor probe (async, off the key path)
sget PCM           ← initial hardware read
sset PCM 45%       ← burst press 1
sset PCM 35%       ← burst presses 2–3 coalesced (latest-wins)
sset PCM 30%       ← isolated injected press: exactly 1 child
```

3-press burst = 2 children total (was 21); isolated press = 1 (was 7).
`alsa-edge`: 25+3 presses + mute/unmute = `scontrols, sget PCM, sset PCM 45%,
sset PCM 0%, set PCM mute, set PCM unmute` — the whole repeat burst collapses
to 2 `sset` children; the 3 clamp presses spawn nothing.

PipeWire `pipewire-delay200`: `set-volume 0.95, set-volume 0.85, get-volume,
set-volume 0.80, get-volume` — burst 3 (mutation + refresh), isolated press
2 (set + confirm). `pipewire-edge`: `set-volume 0.95, set-volume 0.00,
get-volume, set-mute true, set-mute false` — 20 effective presses collapse
to 2 `set-volume` children.

## Failing-executable cases (both backends, all PASS, sanitizer-clean)

| Run | Backend selected | Children | Final (cached) | volume.json after exit (destructor flush) |
|---|---|---|---|---|
| alsa exit1 / garbage | alsa | 8 / 8 | 80 / muted | `{"level":80,"muted":true}` — matches |
| alsa missing | none (noop fallback) | 0 | 80 / muted | `{"level":80,"muted":true}` — matches |
| pipewire exit1 / garbage | pipewire | 4 / 4 | 100 / unmuted (rollback to last confirmed) | `{"level":100,"muted":false}` — matches |
| pipewire missing | none (noop fallback) | 0 | 80 / muted | `{"level":80,"muted":true}` — matches |

No hang (bounded waits throughout), no crash, no ASan/UBSan/LSan report.
Adjudication notes:

- ALSA keeps the requested (optimistic) value when the mixer fails — the
  deliberate post-fix semantic (README, t_82bed393 section); cache and
  persistence stay coherent.
- PipeWire rolls a failed mutation back to the last confirmed value (100).
  At probe-read time `persisted_level` can still show the pre-rollback
  optimistic value (85) because the 250 ms debounce has not fired — the
  kept scratch dirs prove convergence: after the destructor flush every
  `volume.json` equals the final cached state. The snapshot's poisoned-cache
  defect (cached/persisted divergence that survived exit) is gone.

## Two-sided proof (baseline inversion)

`--case baseline` on this tree **FAILS as required**: 10 violations —
`baseline-red` on alsa-delay200, alsa-delay0 and pipewire-delay200 ("run
unexpectedly green — fix landed"), plus band/amplification violations
(0.00 ms means outside every red band; ALSA per-press 0/1 ≠ 7/7).
pipewire-delay0 is baseline-exempt by design. The gate still detects the
original defect shape; it is not vacuously green.

## Semantics preservation (explicit)

- Exact level ladders under 200 ms delay: ALSA 50→35→30, PipeWire 100→85→80
  (`semantic-ladder`, strict mode); injected press delivers exactly one more
  step; `volumeChanged` count = 4 per timing run (3 direct + 1 injected —
  one coherent OSD update per effective press, no double paints).
- `persisted == final` on every timing/edge run (`persist-matches`).
- `volume.json` format byte-compatible with `3970174`: compact
  `{"level":N,"muted":B}`, same keys; restore still level-first-then-mute
  with clamping (restore no longer rewrites the file — the only behavioral
  delta, covered by `volume-manager` unit tests incl.
  `testPersistsAcrossInstances`, `testRestoreKeepsMuteWhenLevelUnchanged`).
- Amplifier semantics intact: raise-while-muted unmutes atomically in ONE
  OSD update (`testRaiseWhileMutedIsOneOsdUpdate`); lower-while-muted stays
  muted; mute does not move the level.
- Failure rollback reconciles UI + persist (`testFailureRollbackReconcilesUiAndPersist`).
- Unit suites `volume-mixer`, `volume-manager`, `mixer-process-runner`,
  `pipewire-volume-mixer`: **3× repeat, all green** under ASan/UBSan/LSan.

## No blocking I/O left on the volume path

`grep waitForFinished|waitForStarted|waitForReadyRead|waitForBytesWritten|
QProcess::execute|system(|popen|sleep(|msleep(` over
`src/audio/ALSAVolumeMixer.cpp`, `src/audio/PipeWireVolumeMixer.cpp`,
`src/audio/VolumeManager.cpp`, `src/input/InputManager.cpp`: **zero matches**.
The only `waitForFinished(1000)` in the audio stack is
`MixerProcessRunner.cpp:91`, inside the worker moved to its own `QThread`
(:179) — off the UI thread by design. `InputManager.cpp:1122` calls
`applyVolumeKey` inline in the event filter, which is now instant (optimistic
cached mutation + async submit), as the 0 ms direct timings prove.

## Suite results

- `ctest -R volume-stall` (ASan/UBSan): **9/9 PASS** (4 timing + noop +
  2 fault trios + 2 new edge cases), RUN_SERIAL.
- Strict-semantics orchestrator: **9/9 PASS** (exact ladders + persistence).
- Full ctest, sanitizer build: **34/35** — sole failure
  `local-files-player-smoke`, the pre-existing LSan `QQmlContext` fixture
  leak tracked by t_ccf076ee (same failure at `902bde3` in t_2ecfe298's
  sanitizer log; zero diff of that test's sources vs dev; this card's diff
  is harness-only). Plain (non-sanitizer) build of the same smoke passes —
  the parent's 33/33 full-ctest claim was the plain build.
- Full ctest, plain build at `343c508` (parent, PR #120 CI): 33/33; all CI
  lanes green at `343c508` (x86, arm64, macOS intel, CodeQL).

## Reproduction

```sh
cd <240-MP checkout at this report's commit>
cmake -S . -B build-sanitize -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-sanitize -j16          # build ALL targets first
ctest --test-dir build-sanitize -R volume-stall --output-on-failure   # 9/9

# strict semantics (exact ladder + persistence), per case:
python3 src/tests/run_volume_stall_case.py --case alsa-delay200 \
  --probe build-sanitize/volume-stall-probe \
  --stub-bin build-sanitize/mixer-stub --repo-root . --strict-semantics
# … alsa-delay0 pipewire-delay200 pipewire-delay0 noop-control
# … alsa-faults pipewire-faults alsa-edge pipewire-edge

# two-sided proof — MUST fail with baseline-red on the fixed tree:
python3 src/tests/run_volume_stall_case.py --case baseline \
  --probe build-sanitize/volume-stall-probe \
  --stub-bin build-sanitize/mixer-stub --repo-root .

# semantics units, repeated:
ctest --test-dir build-sanitize \
  -R 'volume-mixer$|volume-manager$|mixer-process-runner$|pipewire-volume-mixer$' \
  --output-on-failure --repeat until-fail:3
```

Evidence per run: `<cwd>/volume-stall-evidence/<case>/<run>/` (RESULT JSON,
exact mixer argv log, env + loadavg, probe stdout/stderr, kept scratch dir).
Run the 16 ms-gated cases on a quiet host; `noop-control` is the canary.

## Scope limits — explicitly unverified

- **Raspberry Pi behavior is UNVERIFIED.** All measurements are lab-host
  x86-64 with stub mixers; no Pi hardware, no real `amixer`/`wpctl` timing.
- **Owner CRT validation remains UNVERIFIED** (golden composite-out setup).
- All delay lives in scratch PATH stubs; **no system files were changed**.
- PR #120 (and #118 beneath it) are **not merged** — Ian merges by hand.