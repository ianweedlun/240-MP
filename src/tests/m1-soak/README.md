# M-1 soak gate — Owner Decision D3(b)

Issue #135, D3 option **(b)**: `quarantine_size_mb=0` + post-warmup slope with
plateau evidence. The 320 B LSan leak is fixed at `7b57aee`; the old `+1.5–1.8 MB`
plain-build RSS number was one-time QtQml/QV4 warmup, not per-transition retention.
No suppression and no threshold relaxation is used.

## Two legs

| Leg | Build | What it gates |
|-----|-------|---------------|
| plain | regular `240mp` | RSS delta from a fully warmed baseline (< 500 KB), post-warmup OLS slope (< 1 KB/transition), late-window growth (< 128 KB), flat QObject/root counts |
| asan-q0 | `-fsanitize=address,undefined -fsanitize=leak` | zero LSan reports, with `ASAN_OPTIONS=...:quarantine_size_mb=0:thread_local_quarantine_size_kb=0` |

`quarantine_size_mb=0` removes the allocator quarantine that produced the
`~278 MB` ASan artifact (it saturated at its 256 MB cap). ASan's own allocator
still drifts a few KB/transition even at q0, so RSS-shape checks are reported
but not gated on that leg — that is the measurement split D3(b) specifies, not
a relaxed threshold. The RSS gate lives entirely on the plain leg.

The harness warms five full passes over all 11 module roots before taking the
baseline, so the measured window is steady state rather than the warmup ramp.
The slope window starts at transition 150.

## Run

```
cd <worktree with 7b57aee applied>
python3 src/tests/m1-soak/run_m1_soak.py \
  --build-dir build-m1fix \
  --asan-build-dir build-asan-lsan \
  --transitions 500 \
  --out <evidence-dir>
```

Exit 0 means both legs passed. Evidence: `gate.json`, `stdout.log`, `stderr.log`
per leg plus `d3b-verdict.json` at the top level. The runner compiles
`t2_app_main.cpp` against each build dir's own flags, so the same source is
measured identically in both configurations.

## Last verified (2026-11-23)

- plain: pass, delta 164 KB, slope 0.154 KB/transition, late window −4 KB, 0 load errors
- asan-q0: pass, exit 0, zero LSan leak markers, `quarantine_size_mb=0` in effect
- combined verdict: pass
