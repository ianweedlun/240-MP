# Ambient companion mpv teardown gate (AMBIENT-GHOST-AUDIO-01)

Checked-in two-sided regression for `modules/ambient_mode/views/Player.qml`.
The view starts a second, looping mpv for companion audio in
`Component.onCompleted`. Before the fix its only `stopAudio()` call was in
`onPlaybackEnded`, so any teardown that did not emit `playbackEnded` (shell
Loader source reset, top-level back) left that child playing as ghost audio.
The fix adds `Component.onDestruction: ambientModeBackend.stopAudio()`.

The gate asserts **child-PID liveness and view-driven stopAudio call count**,
never RSS. It compiles the shipped `AmbientModeBackend.cpp`, `MpvLocator.cpp`
and `ConfigFile.cpp` unchanged and loads the actual Player.qml offscreen through
a real `Loader` with a compiled fake mpv on PATH, so it follows product code.

## Arms

| ctest case | view | expectation | why |
|---|---|---|---|
| `ambient-companion-teardown-product` | product | FIXED | the regression gate |
| `ambient-companion-teardown-prefix` | pinned pre-fix | DEFECT | negative control: green only when the orphan reproduces, so the gate fails if it loses its discriminating power |
| `ambient-companion-teardown-repeat-teardown-product` | product | FIXED | six load/teardown cycles, zero live children |
| `ambient-companion-teardown-repeat-teardown-prefix` | pinned pre-fix | DEFECT | same cycles leave an orphan |
| `ambient-companion-teardown-control-ended` | product | PASS | normal `playbackEnded` still reaps the child |
| `ambient-companion-teardown-control-direct-stop` | product | PASS | cleanup path is sound |
| `ambient-companion-teardown-control-empty-audio` | product | PASS | backend empty-path guard, no spawn |
| `ambient-companion-teardown-control-video-only` | product | PASS | no audio row, no spawn |
| `ambient-companion-teardown-control-repeat` | product | REPEAT | six normal-flow cycles, N stops, no orphans |

The pre-fix fixture `ambient-teardown/prefix/Player.qml` is the verbatim view at
RC tip `1045ee56` (sha256 `aa319d811ce1ebb016b8b41bc32f2908d1a7fe4a5702067d499d8988917e9f89`).
The runner verifies that hash on every prefix arm; if the fixture drifts, the
arm fails instead of quietly testing the fixed view.

## Build and run

```sh
cmake -B build-test -DBUILD_TESTS=ON
cmake --build build-test -j4
ctest --test-dir build-test -R ambient-companion-teardown --output-on-failure
```

Linux and macOS both run: the liveness check uses `kill(pid,0)` everywhere and
adds a `/proc` zombie exclusion on Linux. `RUN_SERIAL` keeps the fake mpv PID
registry and PATH stub isolated between arms. Each arm is a separate probe
process, and the probe reaps any orphaned fixture child before exiting.

Evidence per arm: `build-test/ambient-companion-teardown-evidence/<case>/`
holds `result.json`, `stdout.txt`, `stderr.txt` and `verdict.json`.
