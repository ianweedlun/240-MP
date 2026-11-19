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
