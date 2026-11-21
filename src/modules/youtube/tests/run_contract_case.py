#!/usr/bin/env python3
"""One case of the YtInnertubeResolver one-terminal-result contract (ROB-07).

Contract under test (src/modules/youtube/YtInnertubeResolver.h, resolve()):
exactly ONE finished() callback per accepted resolve() — success, timeout, or
exception — and an attempt abandoned by the timeout must never emit again nor
chain further requests.

Cases:
  delayed-abandoned  visitorData GET replies AFTER the timeout (default 5500 ms
                     vs kTimeoutMs=4000). Expect exactly one ok=0 reason=timeout
                     callback, nothing more within the observation window
                     (default 2x the fixture delay), and ZERO Player requests.
  near-deadline      visitorData GET replies just BEFORE the deadline (default
                     3500 ms), Player prompt. Expect exactly one ok=1 callback,
                     no timeout result, exactly one Player request carrying the
                     fixture visitorData.
  fast-healthy       sub-100 ms fixture (delay 0). Expect exactly one ok=1 —
                     the over-suppression guard: a fix must not silence the
                     healthy path.

Owns the whole fixture lifecycle: generates an ephemeral self-signed cert
(openssl CLI), starts fixture_proxy.py on a free loopback port, runs the C++
probe (YtResolverContractProbe) for the full observation window, then asserts
against BOTH the per-key callback counts and the proxy request log. Delays are
fixture-side sleeps, not wall-clock races. No live network anywhere.

Knobs (CLI wins over environment, environment wins over case defaults):
  --visitor-delay-ms / YT_CONTRACT_VISITOR_DELAY_MS
  --observe-ms       / YT_CONTRACT_OBSERVE_MS
  --port             (default: pick a free loopback port)
  --evidence-dir     (default: ./yt-contract-evidence/<case>/ under the CWD;
                     ctest runs from the build dir, so evidence lands there)

Dependencies: python3 (stdlib only), the openssl CLI, and the built
yt-resolver-contract-probe binary (passed via --probe).
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import NoReturn

HERE = Path(__file__).resolve().parent
HEADER = HERE.parent / "YtInnertubeResolver.h"

# Case defaults. observe_ms=None means "derive from the delay" below.
CASES = {
    "delayed-abandoned": {"delay_ms": 5500, "observe_ms": None, "expect": "timeout"},
    "near-deadline":     {"delay_ms": 3500, "observe_ms": None, "expect": "success"},
    "fast-healthy":      {"delay_ms": 0,    "observe_ms": 1500, "expect": "success"},
    # #128 live classification, same loopback fixture, canned Player shapes:
    "live-waiting":      {"delay_ms": 0, "observe_ms": 1500, "expect": "live-waiting",
                          "player_mode": "live-waiting"},
    "live-hls":          {"delay_ms": 0, "observe_ms": 1500, "expect": "live-hls",
                          "player_mode": "live-hls"},
    # The CRT defect shape: LIVE response that also carries progressive URLs.
    # Pre-fix code serves the pair (vod=1) and mpv fails to demux it.
    "live-hls-with-pair": {"delay_ms": 0, "observe_ms": 1500, "expect": "live-hls",
                           "player_mode": "live-hls-with-pair"},
}


def die(msg) -> NoReturn:
    print(f"SETUP ERROR: {msg}", file=sys.stderr)
    sys.exit(2)


def resolver_timeout_ms():
    """Read kTimeoutMs from the header so the suite tracks the real deadline."""
    try:
        m = re.search(r"kTimeoutMs\s*=\s*(\d+)", HEADER.read_text(encoding="utf-8"))
        if m:
            return int(m.group(1))
    except OSError:
        pass
    print(f"WARNING: could not parse kTimeoutMs from {HEADER}; assuming 4000",
          file=sys.stderr)
    return 4000


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def make_cert(evidence):
    openssl = shutil.which("openssl")
    if not openssl:
        die("the openssl CLI is required to generate the ephemeral fixture cert")
    cert, key = evidence / "cert.pem", evidence / "key.pem"
    # Same shape as the T0 lab fixture cert: CN + SAN for the two hosts the
    # resolver/vendored library may dial through the CONNECT proxy.
    subprocess.run(
        [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
         "-subj", "/CN=www.youtube.com",
         "-addext", "subjectAltName=DNS:www.youtube.com,DNS:sponsor.ajay.app",
         "-keyout", str(key), "-out", str(cert)],
        check=True, capture_output=True)
    return cert, key


def start_proxy(fixture, port, cert, key, logpath, delay_ms, player_mode="vod"):
    """Start the fixture proxy; return the Popen once ready, or None on failure
    (early exit / never ready) so the caller can retry on another port."""
    proc = subprocess.Popen(
        [sys.executable, str(fixture), "--port", str(port), "--cert", str(cert),
         "--key", str(key), "--log", str(logpath),
         "--visitor-delay-ms", str(delay_ms), "--player-mode", player_mode],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            assert proc.stderr is not None
            err = proc.stderr.read().decode("utf-8", "replace")
            print(f"WARNING: fixture proxy exited early (code {proc.returncode}): {err}",
                  file=sys.stderr)
            return None
        if logpath.exists():
            for line in logpath.read_text(encoding="utf-8").splitlines():
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if row.get("event") == "ready" and row.get("port") == port:
                    return proc
        time.sleep(0.05)
    proc.kill()
    return None


def parse_probe(text):
    meta, events, accepted, refused, summary = {}, [], False, False, False
    for line in text.splitlines():
        if line.startswith("meta,"):
            for tok in line[5:].split(","):
                k, _, v = tok.partition("=")
                meta[k] = v
        elif line.startswith("accepted,"):
            accepted = True
        elif line.startswith("refused,"):
            refused = True
        elif line.startswith("summary,"):
            summary = True
        elif line.startswith("event,"):
            ev = {}
            for tok in line[6:].split(","):
                k, _, v = tok.partition("=")
                if k in ("wall_ms", "resolve_ms", "ok", "hls", "vod"):
                    ev[k] = int(v)
                elif k == "key":
                    ev[k] = v
            # reason is emitted last: rejoin anything after 'reason=' so a
            # reason containing commas cannot corrupt the other fields.
            idx = line.find(",reason=")
            ev["reason"] = line[idx + 8:] if idx >= 0 else ""
            events.append(ev)
    return meta, events, accepted, refused, summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--case", required=True, choices=sorted(CASES))
    p.add_argument("--probe", required=True, help="path to yt-resolver-contract-probe")
    p.add_argument("--fixture", required=True, help="path to fixture_proxy.py")
    p.add_argument("--visitor-delay-ms", type=int, default=None)
    p.add_argument("--observe-ms", type=int, default=None)
    p.add_argument("--port", type=int, default=None)
    p.add_argument("--evidence-dir", default=None)
    a = p.parse_args()

    case = CASES[a.case]
    timeout_ms = resolver_timeout_ms()

    def knob(cli, env, default):
        if cli is not None:
            return cli
        if os.environ.get(env):
            return int(os.environ[env])
        return default

    delay_ms = knob(a.visitor_delay_ms, "YT_CONTRACT_VISITOR_DELAY_MS", case["delay_ms"])
    if case["expect"] == "timeout":
        default_observe = 2 * delay_ms          # >= 2x the fixture delay
    else:
        default_observe = delay_ms + 3000 if case["observe_ms"] is None else case["observe_ms"]
    observe_ms = knob(a.observe_ms, "YT_CONTRACT_OBSERVE_MS", default_observe)
    if delay_ms < 0 or observe_ms <= 0:
        die("delay must be >= 0 and observe window > 0")
    if case["expect"] == "timeout" and delay_ms <= timeout_ms:
        die(f"delayed-abandoned needs --visitor-delay-ms > {timeout_ms} "
            f"(the resolver timeout); got {delay_ms}")
    if case["expect"] == "success" and a.case == "near-deadline" and not (0 < delay_ms < timeout_ms):
        die(f"near-deadline needs 0 < --visitor-delay-ms < {timeout_ms}; got {delay_ms}")

    evidence = Path(a.evidence_dir) if a.evidence_dir else Path.cwd() / "yt-contract-evidence" / a.case
    evidence.mkdir(parents=True, exist_ok=True)
    probe_out_path = evidence / "probe-stdout.txt"
    proxy_log = evidence / "proxy.jsonl"
    key = f"contract-{a.case}"

    print(f"case={a.case} visitor_delay_ms={delay_ms} observe_ms={observe_ms} "
          f"resolver_timeout_ms={timeout_ms} key={key}")
    print(f"evidence: {evidence}")

    cert, cert_key = make_cert(evidence)
    # Truncate BEFORE starting the proxy: a reused evidence dir still holds the
    # previous run's ready row, and observing it would race the new proxy's bind
    # (probe connects to a port nobody listens on yet -> instant exception path).
    proxy_log.write_text("", encoding="utf-8")
    proxy = None
    probe_rc = None
    probe_text = ""
    try:
        port = a.port
        for attempt in range(5):
            if port is None:
                port = free_port()
            proxy = start_proxy(Path(a.fixture), port, cert, cert_key, proxy_log,
                                delay_ms, case.get("player_mode", "vod"))
            if proxy is not None:
                break
            if a.port is not None:
                die(f"could not start fixture proxy on fixed port {a.port}")
            port = None  # next attempt picks a fresh free port
        if proxy is None:
            die("could not start fixture proxy after 5 port attempts")

        try:
            run = subprocess.run(
                [a.probe, str(port), str(cert), key, str(observe_ms)],
                capture_output=True, text=True, timeout=observe_ms / 1000.0 + 30.0)
            probe_rc, probe_text = run.returncode, run.stdout
            if run.stderr:
                probe_text += "\n[stderr]\n" + run.stderr
        except subprocess.TimeoutExpired:
            probe_rc = -1
            probe_text = "probe timed out (observation window + 30 s margin)"
        probe_out_path.write_text(probe_text, encoding="utf-8")
        time.sleep(0.3)  # let the proxy flush its final log rows
    finally:
        if proxy is not None:
            proxy.terminate()
            try:
                proxy.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proxy.kill()

    requests, responses = [], []
    if proxy_log.exists():
        for line in proxy_log.read_text(encoding="utf-8").splitlines():
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if row.get("event") == "request":
                requests.append(row)
            elif row.get("event") == "response":
                responses.append(row)

    meta, events, accepted, refused, summary = parse_probe(probe_text)
    mine = [e for e in events if e.get("key") == key]
    gets = [r for r in requests if r["host"] == "www.youtube.com"
            and r["path"] == "/" and r["method"] == "GET"]
    players = [r for r in requests if r["host"] == "www.youtube.com"
               and r["path"].startswith("/youtubei/v1/player")]
    expected = gets + players
    unexpected = [r for r in requests if r not in expected]

    print("--- callbacks (probe) ---")
    print(probe_text.strip() or "(no output)")
    print("--- proxy requests ---")
    for r in requests:
        print(f"  {r['method']} {r['host']}{r['path']} visitor={r.get('visitor', '')!r}")
    if not requests:
        print("  (none)")

    violations = []

    def violate(name, msg):
        violations.append(f"INVARIANT {name} VIOLATED: {msg}")

    if probe_rc != 0:
        violate("probe-run", f"probe exited with code {probe_rc} (expected 0); "
                f"see {probe_out_path}")
    if refused or not accepted:
        violate("resolve-accepted", "resolve() was refused or never reported acceptance")
    if not summary:
        violate("observation-window", "probe did not run the full observation window "
                "(no summary line); late phantom callbacks may have been missed")
    if meta.get("timeout_ms") and int(meta["timeout_ms"]) != timeout_ms:
        violate("tooling-timeout", f"probe reports kTimeoutMs={meta['timeout_ms']} but the "
                f"header says {timeout_ms}; suite defaults are stale")

    # The core contract: count EVERY callback for the accepted key, not just
    # the first result.
    if len(mine) != 1:
        detail = "; ".join(f"#{i + 1} wall_ms={e['wall_ms']} ok={e['ok']} "
                           f"reason={e['reason']!r}" for i, e in enumerate(mine)) or "(none)"
        violate("one-terminal-result",
                f"key {key!r} produced {len(mine)} terminal callbacks, expected exactly 1 "
                f"per accepted resolve() (YtInnertubeResolver.h contract). Callbacks: {detail}")
        if len(mine) >= 2 and mine[0]["ok"] == 0 and mine[0]["reason"] == "timeout":
            phantom = mine[1]
            violate("no-phantom-after-timeout",
                    f"callback #2 (ok={phantom['ok']} reason={phantom['reason']!r}) arrived "
                    f"at wall_ms={phantom['wall_ms']}, {phantom['wall_ms'] - mine[0]['wall_ms']} ms "
                    f"after the terminal timeout — an attempt abandoned by the timeout must "
                    f"never emit a result again")

    if case["expect"] == "timeout":
        if len(mine) == 1 and not (mine[0]["ok"] == 0 and mine[0]["reason"] == "timeout"):
            violate("timeout-is-terminal",
                    f"expected the single result ok=0 reason=timeout, got "
                    f"ok={mine[0]['ok']} reason={mine[0]['reason']!r}")
        if len(mine) == 0:
            violate("timeout-is-terminal",
                    f"no timeout result arrived within the {observe_ms} ms window")
        if players:
            detail = "; ".join(f"{r['method']} {r['path']} t={r['t']}" for r in players)
            violate("abandoned-attempt-no-player",
                    f"{len(players)} Player request(s) issued for an attempt already "
                    f"abandoned by the timeout (expected 0): {detail}")
    elif case["expect"] == "live-waiting":
        # #128: LIVE_STREAM_OFFLINE must classify as live-waiting — not
        # no-playable-pair (which would pay a yt-dlp spawn and negative-cache
        # an event that simply has not started).
        if len(mine) == 1 and not (mine[0]["ok"] == 0
                                   and mine[0]["reason"] == "live-waiting"):
            violate("live-waiting-class",
                    f"expected the single result ok=0 reason=live-waiting, got "
                    f"ok={mine[0]['ok']} reason={mine[0]['reason']!r}")
        if len(mine) == 0:
            violate("live-waiting-class",
                    f"no result arrived within the {observe_ms} ms window")
        if len(players) != 1:
            violate("player-request-issued",
                    f"expected exactly 1 Player request, got {len(players)}")
    elif case["expect"] == "live-hls":
        # #128: a LIVE event must take the HLS master route (hls=1, vod=0)
        # even when the response also carries progressive-looking URLs —
        # those launched mpv into "Failed to recognize file format" on the CRT.
        if len(mine) == 1 and not (mine[0]["ok"] == 1 and mine[0]["reason"] == ""
                                   and mine[0].get("hls") == 1
                                   and mine[0].get("vod") == 0):
            violate("live-hls-route",
                    f"expected the single result ok=1 hls=1 vod=0 empty reason, got "
                    f"ok={mine[0]['ok']} hls={mine[0].get('hls')} "
                    f"vod={mine[0].get('vod')} reason={mine[0]['reason']!r}")
        if len(mine) == 0:
            violate("live-hls-route",
                    f"no result arrived within the {observe_ms} ms window")
        if len(players) != 1:
            violate("player-request-issued",
                    f"expected exactly 1 Player request, got {len(players)}")
    else:
        if len(mine) == 1 and not (mine[0]["ok"] == 1 and mine[0]["reason"] == ""
                                   and mine[0].get("hls") == 0
                                   and mine[0].get("vod") == 1):
            violate("healthy-single-success",
                    f"expected the single result ok=1 hls=0 vod=1 with empty reason, got "
                    f"ok={mine[0]['ok']} hls={mine[0].get('hls')} "
                    f"vod={mine[0].get('vod')} reason={mine[0]['reason']!r}")
        if any(e["reason"] == "timeout" for e in mine):
            violate("no-timeout-on-healthy",
                    "a timeout result was emitted for an attempt whose visitorData "
                    "replied before the deadline")
        if len(mine) == 1 and mine[0]["resolve_ms"] >= timeout_ms:
            violate("healthy-beats-deadline",
                    f"healthy resolve took {mine[0]['resolve_ms']} ms, at/after the "
                    f"{timeout_ms} ms deadline")
        if len(players) != 1:
            violate("player-request-issued",
                    f"expected exactly 1 Player request for the healthy attempt, "
                    f"got {len(players)}")
        elif players[0].get("visitor") != "fixture-vd":
            violate("visitordata-chained",
                    f"Player request carried visitor={players[0].get('visitor')!r}, "
                    f"expected the page-derived 'fixture-vd'")

    if len(gets) != 1:
        violate("visitordata-fetch-issued",
                f"expected exactly 1 visitorData GET /, got {len(gets)} — the attempt "
                f"must actually start for this case to mean anything")
    if unexpected:
        detail = "; ".join(f"{r['method']} {r['host']}{r['path']}" for r in unexpected)
        violate("no-unexpected-requests",
                f"requests outside the fixture contract were issued: {detail}")

    verdict = "FAIL" if violations else "PASS"
    report = [f"case={a.case} verdict={verdict}",
              f"visitor_delay_ms={delay_ms} observe_ms={observe_ms} "
              f"resolver_timeout_ms={timeout_ms}",
              f"callbacks={len(mine)} gets={len(gets)} player_posts={len(players)}",
              ""]
    report += violations if violations else ["all contract invariants held"]
    (evidence / "verdict.txt").write_text("\n".join(report) + "\n", encoding="utf-8")

    print(f"--- verdict: {verdict} ---")
    for v in violations:
        print(v, file=sys.stderr)
    if not violations:
        print("all contract invariants held")
    print(f"evidence: {evidence}")
    sys.exit(1 if violations else 0)


if __name__ == "__main__":
    main()
