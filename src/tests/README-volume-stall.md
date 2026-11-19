# P-5A volume-stall regression harness (torture T1)

A slow system mixer must not stall the UI thread for more than **16 ms**.
This harness reproduces and measures that stall with **no audio hardware**:
it drives the real `InputManager` file channel (`/tmp/240mp_key.in`), the
real `VolumeManager`, and the real `ALSAVolumeMixer` / `PipeWireVolumeMixer`
classes, offscreen Qt, against PATH-stub `amixer` / `wpctl` executables
whose delay and failure mode are controlled per run.

On the unfixed snapshot `3970174` the timing cases are **red**:

| run | direct mean | direct max | UI heartbeat peak | mixer calls/press |
|---|---|---|---|---|
| ALSA, 200 ms stub delay | ~1466 ms | ~1470 ms | ~1505 ms | **7** |
| PipeWire, 200 ms stub delay | ~211 ms | ~214 ms | ~219 ms | 1 |
| ALSA, zero delay | ~23.7 ms | ~25 ms | ~30 ms | 7 |
| PipeWire, zero delay | ~3.3 ms | ~4 ms | ~16 ms (borderline) | 1 |

ALSA's amplification: one Volume Down press spawns **7** `amixer` processes —
`level()` for the `changeVolume` argument, `level()` again for `setLevel`'s
no-op check, `sset`, then `level()`+`muted()` inside `persist()` and
`level()`+`muted()` again as the `volumeChanged(...)` signal arguments
(`VolumeManager.cpp:37-47,49-60,93-105`; every call is a synchronous
`QProcess::waitForFinished` in `ALSAVolumeMixer.cpp:42-47`). PipeWire spawns
one `wpctl` per press but still blocks ~211 ms at 200 ms mixer latency.

## Quick start

```sh
cmake -B build-test -DBUILD_TESTS=ON
cmake --build build-test --target volume-stall-probe mixer-stub
ctest --test-dir build-test -R volume-stall --output-on-failure
```

Sanitizer build (how the baseline was recorded):

```sh
cmake -B build-test -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
```

One-line reproduction of the recorded red baseline (asserts the snapshot's
red *shape*: stall present, timing bands, ALSA 7-calls/press, level ladder):

```sh
python3 src/tests/run_volume_stall_case.py --case baseline \
  --probe build-test/volume-stall-probe --stub-bin build-test/mixer-stub \
  --repo-root .
```

`--case baseline` **passes when the defect is present** (two-sided test).
After the fix lands it fails with `baseline-red VIOLATED` — that is the
signal to switch the gate to the normal cases, which must then be green.

## Pieces

- `VolumeStallProbe.cpp` — the instrument. Real `AppCore` + `InputManager` +
  `VolumeManager` + mixer classes, `QQuickWindow` offscreen. Scenarios:
  `volume-alsa`, `volume-pipewire`, `volume-noop`, `fault-alsa`,
  `fault-pipewire`. Always exits 0 when it ran; prints one `RESULT` JSON
  line. Assertions live in the orchestrator (ROB-07 convention).
- `run_volume_stall_case.py` — the orchestrator (stdlib-only python3). Owns
  scratch dirs, stub lifecycle, env, timeouts, evidence, and the verdict.
- `mixer-stubs/mixer_stub.cpp` — one compiled binary, copied into the
  scratch bin dir as `amixer` and/or `wpctl` (dispatch on `argv[0]`).
  Compiled on purpose: an interpreter's ~40 ms startup would dwarf the
  ~3 ms process-spawn cost the baseline was measured with and inflate the
  zero-delay control ~10x. `usleep()` gives exact delay on Linux and macOS.
  Built **without** sanitizers even in sanitizer builds (fixture, not
  product code).

What is real vs stubbed: the key path (`InputManager`'s 200 ms-poll debug
file channel → `eventFilter` → `audio::applyVolumeKey`), `VolumeManager`
(clamp, mute rules, persistence, signals), and both mixer classes including
their `QProcess` spawns are real. Only the mixer *executables* are stubs.
The `noop-control` case injects the repo's `NoopVolumeMixer` through
`VolumeManager`'s two-arg constructor instead — everything else identical.

**Volume Down, never Up**: PipeWire starts at cached level 100, so Volume Up
is a clamp no-op that never reaches the mixer.

## Cases and expected verdicts

| ctest case | snapshot 3970174 | after fix |
|---|---|---|
| `volume-stall-alsa-delay200` | **FAIL** (ui-stall) | PASS |
| `volume-stall-alsa-delay0` | **FAIL** (ui-stall: 7 spawns > 16 ms) | PASS |
| `volume-stall-pipewire-delay200` | **FAIL** (ui-stall) | PASS |
| `volume-stall-pipewire-delay0` | borderline (heartbeat sits on the 16 ms boundary) | PASS |
| `volume-stall-noop-control` | PASS (rig self-check) | PASS |
| `volume-stall-alsa-faults` | PASS (bounded, sane, sanitizer-clean) | PASS |
| `volume-stall-pipewire-faults` | PASS (bounded, sane, sanitizer-clean) | PASS |

Fault cases run three sub-runs each: `exit1` (nonzero exit, no output),
`garbage` (unparseable output), `missing` (no executable on PATH → factory
falls back to the no-op mixer). All at 200 ms delay except `missing`.

If `noop-control` ever fails, the host is too noisy for a 16 ms wall-clock
gate (or the rig broke) — re-run on a quieter machine before trusting a red
timing case. It is the canary that separates rig noise from product defect.

**Async phase boundaries.** Backends built on `audio::MixerProcessRunner`
never block the caller, so phase counts cannot be read immediately after a
call: the probe first waits for `VolumeManager::mixerReady()` (startup
probe + initial read; skipped for fault runs), then settles the argv log
(`waitLogQuiescent`: no new line for stub-delay + 150 ms) before each count
and before reading final state. All settling happens OUTSIDE the timed
windows — the 16 ms gate still measures only the caller-visible call.
Latest-wins coalescing means the 3-press direct burst is asserted as a
TOTAL of 1..6 children (async ALSA: 1..2), while the isolated injected
press must cost 1..2 (pre-fix ALSA: 7). `--baseline` keeps asserting the
recorded pre-fix shape (7/press, red bands), so once a fix lands the
baseline case flips to FAIL with `baseline-red` — the two-sided proof.

## Env vars (orchestrator → probe + stubs)

| var | meaning |
|---|---|
| `PATH` | scratch bin dir **only** — no system `amixer`/`wpctl` can leak in; `missing` mode = empty dir |
| `XDG_CONFIG_HOME` / `XDG_DATA_HOME` | scratch overrides |
| `TORTURE_DATA_ROOT` | scratch app data dir (`config.json`, `volume.json`) |
| `TORTURE_MIXER_LOG` | argv capture: one space-joined line per child invocation, written **before** the delay so failed/slow calls count |
| `TORTURE_STUB_STATE` | stub state file (`level muted` text); initial `50 0` |
| `TORTURE_DELAY_MS` | stub sleep before responding (0 or 200 in the suite) |
| `TORTURE_STUB_MODE` | `normal` \| `exit1` \| `garbage` |
| `TORTURE_STUB_CARD` | ALSA stub card shape: `combined` (default — Pi-style `PCM` with playback+capture in one control) \| `split` (desktop-style `PCM Playback Volume` + `PCM Playback Switch`, exercises control validation on `sset`) |
| `TORTURE_FORCE_NOOP` | `1` only for the noop control (probe refuses `volume-noop` without it) |
| `TORTURE_SNAPSHOT` | git sha of the tree under test (RESULT provenance) |
| `ASAN_OPTIONS` | `detect_leaks=1` (sanitizer-clean is asserted from stderr) |

All delay and state live in scratch dirs (`tempfile.mkdtemp` under
`TMPDIR`); no system file or real audio state is touched. The only shared
path is the app's own `/tmp/240mp_key.in` debug channel, which the probe
removes before/after each run — ctest runs these cases `RUN_SERIAL` for
that reason. The key channel is compiled in only with `TELEMETRY=ON` (the
default); with `-DTELEMETRY=OFF` the `injection-delivered` invariant fails
loudly instead of silently measuring nothing.

## Reading a RESULT line

One JSON object per run (also saved to `result.json` in the evidence dir):

- `backend_requested` / `backend_selected` — `alsa` / `pipewire` / `none`;
  must match (a stray `wpctl` on PATH would silently switch the backend).
- `stub_delay_ms`, `stub_mode`, `snapshot` — run conditions.
- `constructor_ms` — `VolumeManager` construction. Pre-fix, ALSA scanned
  `scontrols` synchronously here (a slow mixer stalled app startup); the
  async backend returns immediately and probes in the background.
- `backend_ready`, `ready_wait_ms` — async startup contract: whether the
  backend reported a hardware observation, and how long the probe waited
  for it (outside every timed window). Synchronous backends are ready at
  once (`ready_wait_ms` 0). Fault runs skip the wait by design.
- `direct_key_times_ms[]`, `direct_key_mean_ms`, `direct_key_max_ms` —
  wall time of 3 direct `applyVolumeKey(Down)` calls (the exact call the
  `InputManager` eventFilter makes for a hardware key).
- `baseline_peak_heartbeat_gap_ms` — worst 5 ms-heartbeat gap before
  injection (rig noise floor).
- `injection_peak_heartbeat_gap_ms` — worst heartbeat gap while a real
  `voldown` line travels the file channel (includes ≤200 ms poll alignment).
- `injected_key_delivered`, `volume_changed_signals` — proof the injected
  key reached `VolumeManager` (4 signals = 3 direct + 1 injected).
- `post_direct_level`, `final_level`, `final_muted`, `persisted_level`,
  `persisted_muted` — settled semantics. With the stateful stubs the ladder
  is ALSA 50→35→30, PipeWire/noop 100→85→80.
- `mixer_calls_after_constructor` / `_after_direct` / `_after_presses` /
  `_total` — phase-boundary counts parsed from the argv log, each taken
  after an async settle (see below). `mixer_calls_direct_total` = the
  3-press burst's total children; `per_press_direct` = that total / 3
  (integer division — with latest-wins coalescing a 3-press burst costs
  1..2 children TOTAL, so this floors to 0; the burst total is the
  meaningful number). `per_press_extra` = children for the isolated
  injected press (pre-fix ALSA: **7**; async target: **≤2**, normally 1).
- `target_ms` (16), `pass` (probe-local hint; the orchestrator is
  authoritative), `anchor` (defect source locations).

Verdicts are named invariants: `probe-ran`, `no-hang`, `sanitizer-clean`,
`backend-selected`, `sane-state`, `injection-delivered`,
`mixer-reached/amplification` (burst total 1..6, isolated press 1..2),
`backend-ready` (non-fault, non-baseline runs must observe hardware),
`ui-stall` (constructor, direct max and heartbeat peak all < 16 ms),
`missing-fallback`, plus `baseline-red` / `baseline-band` /
`baseline-amplification` in `--baseline` mode and `semantic-ladder` /
`persist-matches` with `--strict-semantics` (for the post-fix verification
card: exact ladder + persistence, opt-in so a legitimate cache redesign is
adjudicated there, not stranded here).

Evidence per run: `<build-dir>/volume-stall-evidence/<case>/<run>/` —
probe stdout/stderr, `mixer.log` (exact argv), `result.json`, `env.txt`
(incl. loadavg), `verdict.txt`. Scratch dirs are kept and their paths
printed; they are KB-sized.

## Known snapshot behaviors surfaced by this harness

- **Cache poisoning on failure**: both mixers assign `m_level = v` *before*
  checking the command succeeded (`PipeWireVolumeMixer.cpp:22`,
  `ALSAVolumeMixer.cpp:143`). In `exit1`/`garbage` runs the cached level
  advances while persistence stays behind (e.g. PipeWire final 80 vs
  persisted 85). Recorded in RESULT, not gated here — it is a separate
  defect from the stall, for the fix/verification cards to adjudicate.
- **1 s process budget**: `runAmixer`/`run` use `waitForFinished(1000)`
  with no kill on timeout. The suite stays at 200 ms delay; a mixer slower
  than ~1 s per call hits the timeout path (partial output, orphaned
  children) — out of scope for the 16 ms gate.
- **Load sensitivity**: process-spawn overhead grows with host load
  (~3 ms/spawn quiet, ~10-25 ms busy). The delay200 runs are dominated by
  the stub sleep and stay tight; zero-delay numbers drift. Baseline bands
  are wide for this reason; loadavg is recorded in the evidence.