#!/usr/bin/env python3
"""One case of the MEM-02 visitorData size-cap regression (two-sided).

Contract under test (src/modules/youtube/YtInnertubeResolver.{h,cpp},
fetchVisitorData/drainVisitorData): the homepage scan is bounded by an
inclusive kMaxHomepageBytes cap on cumulative delivered body bytes, a token
straddling a chunk boundary is still found via the carry window, a healthy
page resolves ok=1 with the EXACT token chained into the Player request, and
an oversize page fails cleanly with ok=false reason="visitordata-size-cap"
through the same continuation the backend's unconditional yt-dlp ladder
consumes (YouTubeBackend.cpp ctor: every !ok result -> launchYtdlpResolve,
no reason filtering — so the resolver-level ok=false + reason IS the
observable ladder-reach signal; the cap path must NOT chain a Player request).

Cases (positive side passes pre- AND post-fix; negative side FAILS pre-fix):
  healthy-control        56 B page, fixture-vd token. ok=1, exact token.
  realistic-token        496 B page, real 450-char logged-out homepage token.
  straddle-key-64k       key literal spans the 64 KiB chunk boundary (T7).
  straddle-value-64k     450-char value spans the 64 KiB boundary (T8).
  straddle-value-256k    value centered on the 256 KiB boundary (T0 harness).
  exact-cap              page == cap exactly, token at tail: ok=1 (inclusive).
  cap-plus-one           tokenless page == cap+1: ok=0 visitordata-size-cap,
                         0 Player (deterministic inclusive-boundary negative;
                         a tail-token cap+1 page could legitimately early-exit
                         ok=1 within the first cap bytes — see CASES comment).
  oversize-no-token      8 MiB filler, NO token: cap fires before EOF (pre-fix
                         this buffered everything and resolved ok=1).
  oversize-token         8 MiB filler, token at tail: ok=0 size-cap + HWM
                         delta <= 1024 KiB vs a zero-filler control run
                         (pre-fix: ok=1 and ~5.2x amplification, +42,908 KiB).
  no-token-under-cap     small tokenless page: preserved semantics — Player
                         request with synthetic visitor, bot wall,
                         exception:LOGIN_REQUIRED (passes pre- and post-fix).
  real-size-early-exit   873,632 B page at the live capture geometry, token at
                         byte 60,651, 16 KiB drip: ok=1, exact token, homepage
                         bytes_sent <= 262,144 + connection reset (early exit;
                         pre-fix sent the whole page).

Memory assertions are CI-robust accounting, not absolute RSS: the inclusive
cap/cap+1 boundary pair, wire-side bytes_sent (early exit + cap abort), and
ONE generous relative HWM bound (delta <= 1024 KiB vs the zero-filler control,
tolerance covers ASan redzones + up to two in-flight 64 KiB chunks + Qt/TLS
transport; quarantine disabled per the T0 baseline). Large pages are drip-fed
in chunks because single-sendall loopback delivery can buffer ~cap bytes in
the TRANSPORT even post-fix (implementation report t_a39550af §4) — chunked
drip bounds the in-flight burst and makes abort-side resets deterministic.
On platforms without /proc (macOS) the HWM delta is skipped; every behavioral
and bytes-consumed assertion still runs.

Owns the whole fixture lifecycle: ephemeral self-signed cert (openssl CLI,
config-file form so LibreSSL works too), free loopback port with retry,
ready-row port match, probe run for the full observation window, assertions
against probe events AND the proxy request/response log. Failure output names
the violated invariant: INVARIANT <name> VIOLATED: ...

Knobs (CLI wins over environment):
  --observe-ms / YT_VD_CAP_OBSERVE_MS   (default 1500; all fixtures deliver in
                                         < 500 ms, well inside kTimeoutMs)
  --port                                (default: pick a free loopback port)
  --evidence-dir                        (default ./yt-vd-cap-evidence/<case>/
                                         under the CWD; ctest runs from the
                                         build dir, so evidence lands there)

Dependencies: python3 (stdlib only), the openssl CLI, and the built
yt-visitordata-cap-probe binary (passed via --probe). No live network.
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

# Real 450-char visitorData token from an anonymous logged-out YouTube
# homepage captured 2026-11-19 (torture-t0/homepage-live-sample.html, bytes
# [60666, 61116); first key at 60651; mem02-policy.md §3.1). Public page
# content, not a credential; embedded so the suite never needs the network.
LIVE_TOKEN_450 = (
    "Cgtja0RQdC1rZVprMCiZ3vrXBjIKCgJVUxIEGgAgE0gBYqsCCqgCQ3RzQkNBVVNqUUVCREI2UERB"
    "dFNINTIzNk1BWGpPSEFXYUx1QWxxamUycHZlUURGMEtscEdZSHlJM0hhcDA1RlpJeTYwamVWZUhp"
    "SU9mUlpyZm9ab1lhaWtjdTREcWhPWHkzSnJDWFJiNDVoZjBPdDZ4TGh4aTFkTk5qaFBRWVFEc2xD"
    "c2dVelV2UHJ3WldIeFVVR2M5TUVlYzdSaWswWEtyOHdpUXlodk9CdXlHZHZhaDZtbXAzanp2VEoz"
    "Z0k5RFNkODl2UW9CVEpGQVJ4WTlXcWlrOTZONFJfZ3BHM3cyZnhaOTVMU2JIdG9oZ29FME1RMDQx"
    "ZlJEOUpWZi1DbjMzbVBzMm9NZnFNQWF1N3Y3dnd2YkFzVFpTb1gySG1DYUJsN2tSUVA%3D"
)
assert len(LIVE_TOKEN_450) == 450

FIXTURE_TOKEN = "fixture-vd"
# Page overhead: <html>(6) + </html>(7) + <script>(8) + </script>(9) +
# "visitorData":"(15) + token + closing quote(1).
OVERHEAD = {FIXTURE_TOKEN: 56, LIVE_TOKEN_450: 496}
MIB = 1024 * 1024
# HWM tolerance (KiB) vs the zero-filler control — see module docstring.
HWM_DELTA_BOUND_KB = 1024
# Reset-class connection errors are expected abort evidence (the resolver
# closing the socket mid-body on early exit / cap). The proxy logs the CONCRETE
# exception class name (type(e).__name__), so the SSL base name "SSLError" alone
# never matches its reset subclasses — enumerate them, or an arm64 TLS EOF is
# falsely flagged as a hygiene violation. Cert-verification and want-read/write
# are NOT connection resets and still fail proxy-hygiene.
RESET_KINDS = {"BrokenPipeError", "ConnectionResetError", "SSLError",
               "SSLEOFError", "SSLZeroReturnError", "SSLSyscallError",
               "ConnectionAbortedError", "TimeoutError"}

# expect: success | size-cap | bot-wall
# reset:  required (drip-fed with a large unsent remainder at abort) |
#         optional (abort may land after the sender finished) | none
# bytes:  full (bytes_sent == page total) | early (bound + < total) | None
CASES = {
    "healthy-control": dict(
        filler=0, token=FIXTURE_TOKEN, offset=None, chunk=0, delay_ms=0,
        require_visitor=True, expect="success", reset="optional", bytes=None),
    "realistic-token": dict(
        filler=0, token=LIVE_TOKEN_450, offset=None, chunk=0, delay_ms=0,
        require_visitor=True, expect="success", reset="optional", bytes=None),
    # T7: key literal spans 65,530..65,544 across the 65,536 boundary, so the
    # <script> block starts at 65,522. T8: key at 65,500..65,514, value
    # 65,515..65,964 crosses the boundary; <script> at 65,492.
    "straddle-key-64k": dict(
        filler=1 * MIB, token=LIVE_TOKEN_450, offset=65522, chunk=65536,
        delay_ms=15, require_visitor=True, expect="success",
        reset="optional", bytes=None),
    "straddle-value-64k": dict(
        filler=1 * MIB, token=LIVE_TOKEN_450, offset=65492, chunk=65536,
        delay_ms=15, require_visitor=True, expect="success",
        reset="optional", bytes=None),
    # T0 hardened-harness geometry: value centered on the 256 KiB boundary.
    "straddle-value-256k": dict(
        filler=1 * MIB, token=LIVE_TOKEN_450, offset=262144 - 23 - 225,
        chunk=262144, delay_ms=15, require_visitor=True, expect="success",
        reset="optional", bytes=None),
    "exact-cap": dict(
        filler="cap-56", token=FIXTURE_TOKEN, offset=None, chunk=65536,
        delay_ms=2, require_visitor=True, expect="success",
        reset="optional", bytes="full"),
    # Tokenless on purpose: a tail token on a cap+1 page completes 16 bytes
    # before EOF (within the first cap bytes), so early exit can legitimately
    # win at exactly-cap consumption depending on read granularity — the
    # policy T5 geometry is delivery-dependent. A tokenless cap+1 page cannot
    # early-exit: the read delivering byte cap+1 always crosses the inclusive
    # cap first, deterministically, at any chunk/read granularity. With-token
    # oversize behavior is pinned by oversize-token (8 MiB, token at tail).
    "cap-plus-one": dict(
        filler="cap+1-13", token=None, offset=None, chunk=65536,
        delay_ms=2, require_visitor=False, expect="size-cap",
        reset="optional", bytes="full"),
    "oversize-no-token": dict(
        filler=8 * MIB, token=None, offset=None, chunk=65536, delay_ms=2,
        require_visitor=False, expect="size-cap", reset="required",
        bytes="early"),
    "oversize-token": dict(
        filler=8 * MIB, token=FIXTURE_TOKEN, offset=None, chunk=65536,
        delay_ms=2, require_visitor=True, expect="size-cap",
        reset="required", bytes="early", hwm_delta=True),
    "no-token-under-cap": dict(
        filler=0, token=None, offset=None, chunk=0, delay_ms=0,
        require_visitor=True, expect="bot-wall", reset="none", bytes=None),
    # Live capture geometry: 873,632 B total, first key at 60,651, 16 KiB drip.
    "real-size-early-exit": dict(
        filler=873632 - 496, token=LIVE_TOKEN_450, offset=60651, chunk=16384,
        delay_ms=2, require_visitor=True, expect="success",
        reset="required", bytes="early", early_bound=262144),
}


def die(msg) -> NoReturn:
    print(f"SETUP ERROR: {msg}", file=sys.stderr)
    sys.exit(2)


def header_constants():
    """Track the real cap + deadline from the header (suite follows the code)."""
    try:
        text = HEADER.read_text(encoding="utf-8")
    except OSError:
        die(f"cannot read {HEADER}")
    m = re.search(r"kMaxHomepageBytes\s*=\s*(\d+)\s*\*\s*1024\s*\*\s*1024", text)
    if m:
        cap = int(m.group(1)) * MIB
    else:
        m = re.search(r"kMaxHomepageBytes\s*=\s*(\d+)\s*;", text)
        if not m:
            die(f"cannot parse kMaxHomepageBytes from {HEADER}")
        cap = int(m.group(1))
    m = re.search(r"kTimeoutMs\s*=\s*(\d+)", text)
    timeout = int(m.group(1)) if m else 4000
    return cap, timeout


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def make_cert(evidence):
    openssl = shutil.which("openssl")
    if not openssl:
        die("the openssl CLI is required to generate the ephemeral fixture cert")
    cert, key = evidence / "cert.pem", evidence / "key.pem"
    # Config-file form (not -addext): works on OpenSSL and LibreSSL alike.
    # Same shape as the proven T0 lab cert: self-signed CA:TRUE + SANs for the
    # two hosts the resolver/vendored library may dial through the proxy.
    cnf = evidence / "openssl.cnf"
    cnf.write_text(
        "[req]\ndistinguished_name = dn\nx509_extensions = v3_ext\nprompt = no\n"
        "[dn]\nCN = www.youtube.com\n"
        "[v3_ext]\nbasicConstraints = critical,CA:TRUE\n"
        "subjectAltName = DNS:www.youtube.com,DNS:sponsor.ajay.app\n",
        encoding="utf-8")
    subprocess.run(
        [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
         "-config", str(cnf), "-keyout", str(key), "-out", str(cert)],
        check=True, capture_output=True)
    return cert, key


def start_proxy(fixture, port, cert, key, logpath, spec, token):
    """Start the fixture; return the Popen once ready, else None (retryable)."""
    cmd = [sys.executable, str(fixture), "--port", str(port),
           "--cert", str(cert), "--key", str(key), "--log", str(logpath),
           "--page-bytes", str(spec["filler"]),
           "--send-chunk-bytes", str(spec["chunk"]),
           "--send-chunk-delay-ms", str(spec["delay_ms"])]
    if token is None:
        cmd.append("--no-token")
    else:
        cmd += ["--visitor-token", token]
    if spec["offset"] is not None:
        cmd += ["--token-offset", str(spec["offset"])]
    if spec["require_visitor"]:
        cmd.append("--require-visitor")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            err = proc.stderr.read().decode("utf-8", "replace") if proc.stderr else ""
            print(f"WARNING: fixture proxy exited early (code {proc.returncode}): {err}",
                  file=sys.stderr)
            return None
        if logpath.exists():
            for line in logpath.read_text(encoding="utf-8").splitlines():
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                # Match the ready row's PORT: a stale row from a previous run
                # (or a different proxy) must not release the probe early.
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
            for tok in line[8:].split(","):
                k, _, v = tok.partition("=")
                meta["summary_" + k] = v
        elif line.startswith("event,"):
            ev = {}
            for tok in line[6:].split(","):
                k, _, v = tok.partition("=")
                if k in ("wall_ms", "resolve_ms", "ok", "rss_kb", "hwm_kb"):
                    ev[k] = int(v)
                elif k == "key":
                    ev[k] = v
            # reason is emitted LAST: rejoin everything after ',reason=' so a
            # reason containing commas cannot corrupt the other fields.
            idx = line.find(",reason=")
            ev["reason"] = line[idx + 8:] if idx >= 0 else ""
            events.append(ev)
    return meta, events, accepted, refused, summary


def run_round(name, spec, token, cap, fixture, probe, evidence, observe_ms, port):
    """One proxy+probe round. Returns a dict of parsed evidence."""
    evidence.mkdir(parents=True, exist_ok=True)
    proxy_log = evidence / "proxy.jsonl"
    cert, cert_key = make_cert(evidence)
    # Truncate BEFORE starting the proxy: a reused evidence dir still holds
    # the previous run's ready row, which would race the new proxy's bind.
    proxy_log.write_text("", encoding="utf-8")
    proxy = None
    probe_rc, probe_text = None, ""
    try:
        for _ in range(5):
            if port is None:
                port = free_port()
            proxy = start_proxy(fixture, port, cert, cert_key, proxy_log, spec, token)
            if proxy is not None:
                break
            port = None
        if proxy is None:
            die("could not start fixture proxy after 5 port attempts")
        key = f"vd-cap-{name}"
        env = dict(os.environ,
                   ASAN_OPTIONS=("detect_leaks=1:abort_on_error=1:quarantine_size_mb=0:"
                                 "thread_local_quarantine_size_kb=0"),
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        try:
            run = subprocess.run(
                [probe, str(port), str(cert), key, str(observe_ms)],
                capture_output=True, text=True, env=env,
                timeout=observe_ms / 1000.0 + 30.0)
            probe_rc, probe_text = run.returncode, run.stdout
            if run.stderr:
                probe_text += "\n[stderr]\n" + run.stderr
        except subprocess.TimeoutExpired:
            probe_rc = -1
            probe_text = "probe timed out (observation window + 30 s margin)"
        (evidence / "probe-stdout.txt").write_text(probe_text, encoding="utf-8")
        time.sleep(0.3)  # let the proxy flush its final log rows
    finally:
        if proxy is not None:
            proxy.terminate()
            try:
                proxy.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proxy.kill()

    rows = []
    if proxy_log.exists():
        for line in proxy_log.read_text(encoding="utf-8").splitlines():
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    meta, events, accepted, refused, summary = parse_probe(probe_text)
    return dict(name=name, key=f"vd-cap-{name}", spec=spec, token=token,
                probe_rc=probe_rc, probe_text=probe_text, meta=meta,
                events=events, accepted=accepted, refused=refused,
                summary=summary, rows=rows)


def check_round(rd, cap, timeout_ms, violations):
    """Behavioral + hygiene invariants for one round (shared by all cases)."""
    name, spec, key = rd["name"], rd["spec"], rd["key"]
    events = [e for e in rd["events"] if e.get("key") == key]
    requests = [r for r in rd["rows"] if r.get("event") == "request"]
    responses = [r for r in rd["rows"] if r.get("event") == "response"]
    errors = [r for r in rd["rows"] if r.get("event") == "handler-error"]
    walls = [r for r in rd["rows"] if r.get("event") == "visitor-wall"]
    gets = [r for r in requests if r["host"] == "www.youtube.com"
            and r["path"] == "/" and r["method"] == "GET"]
    players = [r for r in requests if r["host"] == "www.youtube.com"
               and r["path"].startswith("/youtubei/v1/player")]
    home_resp = [r for r in responses if r["host"] == "www.youtube.com"
                 and r["path"] == "/"]
    unexpected = [r for r in requests if r not in gets and r not in players]

    def violate(inv, msg):
        violations.append(f"[{name}] INVARIANT {inv} VIOLATED: {msg}")

    if rd["probe_rc"] != 0:
        violate("probe-run", f"probe exited with code {rd['probe_rc']} (expected 0)")
    if rd["refused"] or not rd["accepted"]:
        violate("resolve-accepted", "resolve() was refused or never reported acceptance")
    if not rd["summary"]:
        violate("observation-window", "no summary line — the full observation "
                "window did not run; late phantom callbacks may have been missed")
    cap_meta = rd["meta"].get("cap_bytes", "")
    if cap_meta.isdigit() and int(cap_meta) != cap:
        violate("tooling-cap", f"probe reports kMaxHomepageBytes="
                f"{cap_meta} but the header says {cap}")
    # cap_bytes=none means the binary under test predates the cap constant
    # (fail-before side); the behavioral invariants carry that gate.
    if len(events) != 1:
        detail = "; ".join(f"#{i + 1} wall_ms={e['wall_ms']} ok={e['ok']} "
                           f"reason={e['reason']!r}" for i, e in enumerate(events)) or "(none)"
        violate("one-terminal-result", f"{len(events)} terminal callbacks for key "
                f"{key!r}, expected exactly 1. Callbacks: {detail}")
    if unexpected:
        violate("no-unexpected-requests",
                f"requests beyond homepage GET / Player POST: {unexpected}")
    if len(gets) != 1:
        violate("one-homepage-get", f"expected exactly 1 homepage GET, got {len(gets)}")

    # Sanitizer hygiene (LSan rides ASan on linux-x86_64; the leak summary is
    # branded 'SUMMARY: AddressSanitizer: N byte(s) leaked').
    text = rd["probe_text"]
    if "detected memory leaks" in text or re.search(
            r"SUMMARY: AddressSanitizer:.*leaked", text):
        violate("lsan-clean", "LeakSanitizer reported leaked bytes")
    san_errors = [x for x in text.splitlines()
                  if "ERROR: AddressSanitizer" in x or "runtime error:" in x]
    if san_errors:
        violate("sanitizer-clean", f"ASan/UBSan reports: {san_errors[:3]}")

    # Reset-class handler errors are expected abort evidence; other kinds fail.
    bad_errors = [e for e in errors if e.get("kind") not in RESET_KINDS]
    if bad_errors:
        violate("proxy-hygiene", f"unexpected handler errors: {bad_errors[:3]}")
    if spec["reset"] == "required" and not errors and not any(
            r.get("reset") for r in home_resp):
        violate("early-exit-reset", "drip-fed body with a large unsent remainder "
                "at abort, but no connection reset was observed — the resolver "
                "did not abort the download (no early exit / cap abort)")

    # Wire-side bytes-consumed accounting for the homepage response.
    page_total = spec["filler"] + (OVERHEAD[spec["token"]] if spec["token"] else 13)
    if len(home_resp) != 1:
        violate("one-homepage-response", f"expected 1 homepage response row, "
                f"got {len(home_resp)}")
    else:
        hr = home_resp[0]
        if hr["bytes_total"] != page_total:
            violate("fixture-geometry", f"homepage bytes_total={hr['bytes_total']} "
                    f"but the case geometry says {page_total}")
        if spec["bytes"] == "full":
            if hr["bytes_sent"] != page_total:
                violate("full-consumption", f"bytes_sent={hr['bytes_sent']} != "
                        f"page total {page_total} (this case is fully consumed "
                        f"before the terminal decision)")
        elif spec["bytes"] == "early":
            bound = spec.get("early_bound", cap + 2 * max(spec["chunk"], 65536))
            if hr["bytes_sent"] > bound:
                violate("bounded-bytes-consumed", f"homepage bytes_sent="
                        f"{hr['bytes_sent']} exceeds the early-exit/cap bound "
                        f"{bound} (page total {page_total})")
            if hr["bytes_sent"] >= page_total:
                violate("early-exit", f"the whole {page_total}-byte page was sent "
                        f"(bytes_sent={hr['bytes_sent']}) — no early exit/cap abort")

    # Outcome-specific invariants.
    ev = events[0] if len(events) == 1 else None
    if spec["expect"] == "success":
        if ev and not (ev["ok"] == 1 and ev["reason"] == ""):
            violate("healthy-single-success", f"expected ok=1 with empty reason, "
                    f"got ok={ev['ok']} reason={ev['reason']!r}")
        if ev and ev["resolve_ms"] >= timeout_ms:
            violate("healthy-beats-deadline", f"healthy resolve took "
                    f"{ev['resolve_ms']} ms, at/after the {timeout_ms} ms deadline")
        if len(players) != 1:
            violate("player-request-issued", f"expected exactly 1 Player request "
                    f"for the healthy attempt, got {len(players)}")
        elif players[0].get("visitor") != spec["token"]:
            violate("exact-token-chained", f"Player request carried visitor="
                    f"{players[0].get('visitor')!r}, expected the exact homepage "
                    f"token ({len(spec['token'])} chars)")
        if walls:
            violate("no-bot-wall-on-healthy", f"{len(walls)} visitor-wall hits "
                    "although the exact token was available")
    elif spec["expect"] == "size-cap":
        if ev and not (ev["ok"] == 0 and ev["reason"] == "visitordata-size-cap"):
            violate("clean-cap-failure", f"expected ok=0 "
                    f"reason='visitordata-size-cap', got ok={ev['ok'] if ev else '?'} "
                    f"reason={(ev['reason'] if ev else '(no event)')!r} — an oversize "
                    "homepage must fail cleanly into the backend's unconditional "
                    "yt-dlp ladder (YouTubeBackend.cpp: every !ok -> launchYtdlpResolve)")
        if players:
            violate("cap-failure-no-player", f"{len(players)} Player request(s) "
                    "although the cap failed the resolve — the cap path must emit "
                    "the failure instead of chaining startPlayer()")
        if f"visitorData homepage exceeded size cap" not in rd["probe_text"] \
                or f"cap={cap}" not in rd["probe_text"]:
            violate("cap-diagnostic", "missing the distinguishable cap diagnostic "
                    f"'visitorData homepage exceeded size cap ... cap={cap}' in "
                    "probe stderr")
    elif spec["expect"] == "bot-wall":
        if ev and not (ev["ok"] == 0 and ev["reason"].startswith("exception:")
                       and "LOGIN_REQUIRED" in ev["reason"]):
            violate("preserved-bot-wall", f"expected ok=0 reason=exception:"
                    f"LOGIN_REQUIRED (today's no-token semantics), got "
                    f"ok={ev['ok'] if ev else '?'} reason={(ev['reason'] if ev else '?')!r}")
        if len(players) != 1:
            violate("no-token-still-reaches-player", f"expected exactly 1 Player "
                    f"request (synthetic-visitor continuation), got {len(players)}")
        elif not players[0].get("visitor") or players[0]["visitor"] == spec["token"]:
            violate("synthetic-visitor", "Player request must carry a synthetic "
                    "visitor id when the homepage had no token")
        if not walls:
            violate("bot-wall-hit", "expected the fixture bot wall to reject the "
                    "synthetic visitor")
    return dict(events=events, gets=gets, players=players, errors=errors,
                home_resp=home_resp, walls=walls)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--case", required=True, choices=sorted(CASES))
    p.add_argument("--probe", required=True, help="path to yt-visitordata-cap-probe")
    p.add_argument("--fixture", default=str(HERE / "vd_cap_fixture_proxy.py"))
    p.add_argument("--observe-ms", type=int, default=None)
    p.add_argument("--port", type=int, default=None)
    p.add_argument("--evidence-dir", default=None)
    a = p.parse_args()

    cap, timeout_ms = header_constants()
    observe_ms = a.observe_ms
    if observe_ms is None:
        observe_ms = int(os.environ.get("YT_VD_CAP_OBSERVE_MS", "1500"))
    if observe_ms <= 0:
        die("observe window must be > 0")

    spec = dict(CASES[a.case])
    if isinstance(spec["filler"], str):
        expr = spec["filler"]
        if expr == "cap-56":
            spec["filler"] = cap - OVERHEAD[FIXTURE_TOKEN]
        elif expr == "cap+1-13":
            spec["filler"] = cap + 1 - 13  # tokenless page overhead: <html></html>
        else:
            die(f"unknown filler expression {expr!r}")
    if spec["token"] is not None and spec["offset"] is not None:
        # Sanity: the token block must fit inside the filler region.
        if not (6 <= spec["offset"] <= 6 + spec["filler"]):
            die(f"token offset {spec['offset']} outside filler {spec['filler']}")

    evidence = (Path(a.evidence_dir) if a.evidence_dir
                else Path.cwd() / "yt-vd-cap-evidence" / a.case)
    print(f"case={a.case} cap={cap} timeout_ms={timeout_ms} observe_ms={observe_ms}")
    print(f"evidence: {evidence}")

    violations = []
    rounds = []
    if spec.get("hwm_delta"):
        # Control round first: zero-filler healthy page, same delivery class.
        control = dict(spec, filler=0, offset=None, chunk=0, delay_ms=0,
                       bytes=None, reset="optional", expect="success")
        ctl_rd = run_round(a.case + "-control", control, spec["token"], cap,
                           Path(a.fixture), a.probe, evidence / "control",
                           observe_ms, a.port)
        rounds.append(ctl_rd)
        check_round(ctl_rd, cap, timeout_ms, violations)
    rd = run_round(a.case, spec, spec["token"], cap, Path(a.fixture), a.probe,
                   evidence if not spec.get("hwm_delta") else evidence / "oversize",
                   observe_ms, a.port)
    rounds.append(rd)
    parsed = check_round(rd, cap, timeout_ms, violations)

    if spec.get("hwm_delta"):
        ctl_hwm = int(rounds[0]["meta"].get("summary_hwm_kb", "-1"))
        big_hwm = int(rd["meta"].get("summary_hwm_kb", "-1"))
        if ctl_hwm >= 0 and big_hwm >= 0:
            delta = big_hwm - ctl_hwm
            print(f"hwm: control={ctl_hwm} KiB oversize={big_hwm} KiB "
                  f"delta={delta} KiB bound={HWM_DELTA_BOUND_KB} KiB")
            if delta > HWM_DELTA_BOUND_KB:
                violations.append(
                    f"[{a.case}] INVARIANT bounded-memory VIOLATED: HWM delta "
                    f"{delta} KiB vs zero-filler control exceeds the "
                    f"{HWM_DELTA_BOUND_KB} KiB tolerance (pre-fix amplification "
                    f"was ~5.2x filler = +42,908 KiB at 8 MiB)")
        else:
            print("hwm: /proc unavailable on this platform — HWM delta skipped; "
                  "behavioral + bytes-consumed assertions still enforced")

    print("--- probe output ---")
    print(rd["probe_text"].strip() or "(no output)")
    print("--- proxy requests ---")
    for r in [x for x in rd["rows"] if x.get("event") == "request"]:
        v = r.get("visitor", "")
        shown = repr(v if len(v) <= 24 else v[:24] + "...")
        print(f"  {r['method']} {r['host']}{r['path']} visitor={shown}")
    for r in [x for x in rd["rows"] if x.get("event") == "response"]:
        print(f"  response {r['host']}{r['path']} status={r['status']} "
              f"bytes_sent={r['bytes_sent']}/{r['bytes_total']} reset={r['reset']}")
    if not any(x.get("event") == "request" for x in rd["rows"]):
        print("  (none)")

    verdict = "FAIL" if violations else "PASS"
    (evidence / "verdict.txt").write_text(
        verdict + "\n" + "\n".join(violations) + "\n", encoding="utf-8")
    if violations:
        for v in violations:
            print(v)
        print(f"GATE FAIL: {a.case} ({len(violations)} violation(s)); evidence: {evidence}")
        return 1
    print(f"GATE PASS: {a.case}; evidence: {evidence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())