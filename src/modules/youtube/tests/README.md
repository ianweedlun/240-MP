# YtInnertubeResolver one-terminal-result contract suite (ROB-07)

Regression tests for the contract on `resolve()` in `../YtInnertubeResolver.h`:
**exactly one `finished()` callback per accepted `resolve()`** — success,
timeout, or exception — and an attempt abandoned by the timeout must never emit
a result again nor chain further requests.

ROB-07: a visitorData GET replying *after* the 4 s timeout produced a phantom
second result (`ok=0 reason=timeout`, then `ok=1`) because the late completion
handler only checked `m_pendingStart` and `startPlayer()` captured the *current*
generation. These tests fail on that snapshot and pass once abandoned attempts
are guarded by the generation captured when their request was issued.

## Cases

| ctest name | fixture delay | expectation |
|---|---|---|
| `yt-resolver-contract-delayed-abandoned` | 5500 ms (> 4000 ms timeout) | exactly one `ok=0 reason=timeout`; no further callback within 2× the delay; **zero** Player requests |
| `yt-resolver-contract-near-deadline` | 3500 ms (< timeout) | exactly one `ok=1`; no timeout result; one Player request carrying `fixture-vd` |
| `yt-resolver-contract-fast-healthy` | 0 ms | exactly one `ok=1` (over-suppression guard) |
| `yt-resolver-contract-live-waiting` | 0 ms | `ok=0 reason=live-waiting` (LIVE_STREAM_OFFLINE) |
| `yt-resolver-contract-live-hls` | 0 ms | `ok=1 hls=1 vod=0` (running event, master only) |
| `yt-resolver-contract-live-hls-with-pair` | 0 ms | `ok=1 hls=1 vod=0` — LIVE response that also carries progressive URLs still takes HLS |
| `yt-resolver-contract-post-live-hls` | 0 ms | `ok=1 hls=1 vod=0` — FINISHED event (post_live: isLive=0, `source/yt_live_broadcast` manifest, live-ingest pair) takes HLS; pre-fix code serves the dead pair |
| `yt-resolver-contract-premiered-vod-pair` | 0 ms | `ok=1 hls=0 vod=1` — premiered-live VOD (isLiveContent=1, `source/youtube` manifest, working pair) stays on the pair route; catches isLiveContent-based misclassification |

Delays are fixture-side sleeps in `fixture_proxy.py` (loopback CONNECT+TLS
fixture, stdlib-only Python, **no live network**). The C++ probe
(`YtResolverContractProbe.cpp`) drives the real resolver + vendored
innertube-qt library, counts callbacks **per accepted resolve() key** over the
full observation window, and the orchestrator (`run_contract_case.py`) asserts
against both the callback counts and the proxy request log. Failure output
names the violated invariant (`INVARIANT <name> VIOLATED: ...`).

## Run

Single command from a fresh checkout (configure + build + run):

```sh
cmake -B build-test -DBUILD_TESTS=ON && cmake --build build-test --target yt-resolver-contract-probe -j && ctest --test-dir build-test -R yt-resolver-contract --output-on-failure
```

Or step by step:

```sh
cmake -B build-test -DBUILD_TESTS=ON
cmake --build build-test --target yt-resolver-contract-probe
ctest --test-dir build-test -R yt-resolver-contract --output-on-failure
```

Dependencies: Qt6 Core+Network, python3 (stdlib only), the `openssl` CLI
(ephemeral self-signed fixture cert, generated per run — no key material is
checked in). Runs headless; no mpv, no display, no Raspberry Pi.

## Knobs

CLI flags on `run_contract_case.py` win over environment variables, which win
over case defaults:

- `--visitor-delay-ms` / `YT_CONTRACT_VISITOR_DELAY_MS` — fixture sleep before
  the visitorData reply.
- `--observe-ms` / `YT_CONTRACT_OBSERVE_MS` — observation window from resolve()
  acceptance (defaults: 2× delay for delayed-abandoned, delay+3000 for
  near-deadline, 1500 for fast-healthy).
- `--port`, `--evidence-dir` — free loopback port by default; evidence
  (probe stdout, proxy JSONL, verdict, ephemeral cert) lands in
  `<build-dir>/yt-contract-evidence/<case>/`.

The deadline itself is read from `kTimeoutMs` in the header, so the suite
tracks the real constant. `delayed-abandoned` requires delay > timeout;
`near-deadline` requires 0 < delay < timeout (500 ms default margin).

## Expected result on the unfixed snapshot (3970174)

`delayed-abandoned` FAILS with `one-terminal-result` / `no-phantom-after-timeout`
(two callbacks: timeout at ~3.9 s, phantom success at ~5.5 s) and
`abandoned-attempt-no-player` (a Player POST is issued for the abandoned
attempt). `near-deadline` and `fast-healthy` PASS.

---

# YouTube resolver test instruments

## MEM-02 visitorData size-cap regression (`yt-visitordata-cap-*`)

Two-sided CTest regression for the bounded streaming visitorData scan in
`YtInnertubeResolver.{h,cpp}` (MEM-02: the homepage fetch used to buffer the
whole page — `readAll()` + whole-page `QString::fromUtf8` — with ~5.2x peak
memory amplification; the fix streams 64 KiB reads through a ≤69,632-byte
carry window under an inclusive 4 MiB cap).

Files:
- `YtVdCapProbe.cpp` — C++ instrument: real resolver + vendored innertube-qt,
  records EVERY `finished()` callback for the accepted key over the full
  observation window (never quits on the first result) plus per-event RSS/HWM.
  Always exits 0 when it ran; assertions live in the orchestrator.
- `vd_cap_fixture_proxy.py` — loopback CONNECT+TLS fixture (stdlib only, no
  upstream network). Page shaping: filler size, token value/placement/absence,
  chunked drip delivery, visitor-id bot wall. JSONL log: request rows,
  response rows with `bytes_sent`/`bytes_total`/`reset` accounting,
  handler-error rows, ready row.
- `run_vd_cap_case.py` — owns cert generation (openssl CLI, config-file form
  for OpenSSL/LibreSSL portability), proxy lifecycle (free port + retry,
  ready-row port match), probe run, and assertions on probe events AND the
  proxy log. Failure output names the violated invariant:
  `INVARIANT <name> VIOLATED: ...`.

Run:
```sh
cmake -B build-test -DBUILD_TESTS=ON
cmake --build build-test --target yt-visitordata-cap-probe -j
ctest --test-dir build-test -R yt-visitordata-cap --output-on-failure
```
Deps: Qt6 Core+Network, python3 (stdlib), openssl CLI. Headless; no mpv/Pi/
live network. Evidence (probe stdout+stderr, proxy.jsonl, verdict.txt,
ephemeral cert) in `<build-dir>/yt-vd-cap-evidence/<case>/`.

Cases and two-sided expectations (cap value is parsed from
`kMaxHomepageBytes` in the header, so boundary cases track the constant):

| case | geometry | post-fix | pre-fix |
|---|---|---|---|
| healthy-control | 56 B, `fixture-vd` | PASS ok=1 exact token | PASS |
| realistic-token | 496 B, real 450-char logged-out token | PASS ok=1 exact token | PASS |
| straddle-key-64k | key literal spans 65,536; 64 KiB drip | PASS (carry window) | PASS |
| straddle-value-64k | 450-char value spans 65,536; drip | PASS | PASS |
| straddle-value-256k | value centered on 262,144; drip | PASS | PASS |
| exact-cap | page == cap, token at tail | PASS ok=1 (inclusive) | PASS |
| cap-plus-one | tokenless page == cap+1 | PASS ok=0 `visitordata-size-cap`, 0 Player | **FAIL** (ok=1, full buffer) |
| oversize-no-token | 8 MiB filler, no token, drip | PASS ok=0 size-cap before EOF | **FAIL** (ok=1, full buffer) |
| oversize-token | 8 MiB filler, token at tail, drip | PASS ok=0 size-cap + HWM delta ≤ 1024 KiB | **FAIL** (ok=1, ~+42,908 KiB) |
| no-token-under-cap | 13 B, no token, bot wall | PASS `exception:LOGIN_REQUIRED` via Player | PASS (preserved semantics) |
| real-size-early-exit | 873,632 B live geometry, token @60,651, 16 KiB drip | PASS ok=1, bytes_sent ≤ 262,144 + reset | **FAIL** (whole page sent) |

Ladder reach: the backend's `finished` handler classifies every `!ok` result
as transient (surfaces `playbackResolveFailed`; no subprocess ladder exists
since NEG-01), so the resolver-level `ok=false reason=visitordata-size-cap` +
zero Player requests + the cap diagnostic IS the observable clean-failure
signal; the suite asserts all three rather than "no crash".

Boundary-note: `cap-plus-one` is tokenless on purpose. A tail token on a
cap+1 page completes 16 bytes before EOF — inside the first cap bytes — so
early exit can legitimately win at exactly-cap consumption depending on
read granularity (the policy's T5 with-token geometry is delivery-dependent;
observed both ways during suite development). A tokenless cap+1 page cannot
early-exit, so the inclusive-cap crossing is deterministic at any chunk/read
granularity. With-token oversize behavior is pinned by `oversize-token`.

Memory assertions are CI-robust accounting, not absolute RSS: the inclusive
cap/cap+1 boundary pair, wire-side `bytes_sent` (early exit / cap abort), and
one generous relative HWM bound (delta ≤ 1024 KiB vs a zero-filler control run
in the same case; tolerance covers ASan redzones + up to two in-flight 64 KiB
chunks + Qt/TLS transport; ASan quarantine disabled per the T0 baseline).
Large pages are drip-fed: single-sendall loopback delivery can buffer ~cap
bytes in the TRANSPORT even post-fix, and reset evidence is timing-dependent
without chunking. On platforms without `/proc` (macOS) the HWM delta is
skipped; all behavioral and bytes-consumed assertions still run.

Timeout/race semantics are deliberately untouched here (owned by the ROB-07
contract suite / t_af4c8524): every fixture delivers well inside `kTimeoutMs`,
and the observation window only guards against duplicate/phantom emissions.

## Lab gate probe (`yt-resolver-probe`, NOT a ctest test)

`YtResolverProbe.cpp` resolves through the real resolver and requires mpv to
decode BOTH streams — it needs the live network and mpv, so it is built but
never registered as a test. Run manually: `./build-test/yt-resolver-probe
<videoId> [required audio xtags substring]`.
