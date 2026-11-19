#!/usr/bin/env python3
"""Loopback CONNECT+TLS fixture proxy for the MEM-02 visitorData size-cap
regression suite (src/modules/youtube/tests/run_vd_cap_case.py).

Stdlib only; NO upstream networking — every request is answered locally or
404'd. Evolved from the T0 lab fixture (torture-t0/fixture_proxy.py, hardened
by t_24042026): same page-shaping flags and request/handler-error logging,
plus per-response bytes-consumed accounting so the orchestrator can assert
early exit and cap enforcement from the wire side.

Page shaping (homepage = <html> + filler + optional token block + </html>):
  --visitor-token VALUE     token embedded in the homepage and expected by the
                            player endpoint (default: fixture-vd)
  --no-token                homepage carries no visitorData script at all
  --token-offset N          place the <script> token block at body byte offset
                            N (filler is split around it; --page-bytes filler
                            total is preserved). Default: token at the tail.
  --page-bytes N            filler byte count (default 0)
  --require-visitor         player endpoint answers LOGIN_REQUIRED unless the
                            X-Goog-Visitor-Id header equals --visitor-token
                            (mirrors real YouTube's bot wall for synthetic ids)
  --send-chunk-bytes N      deliver the response body in N-byte chunks
                            (0 = single sendall)
  --send-chunk-delay-ms N   sleep between chunks so each arrives as a distinct
                            read event (deterministic stream boundaries; also
                            makes abort-side connection resets deterministic)

JSONL log rows:
  ready          once, with the bound port and all shaping parameters
  request        per request: host/method/path/content-length/visitor header
  response       per response: status, bytes_total, bytes_sent, reset flag.
                 bytes_sent counts COMPLETED sendall calls (body bytes only,
                 headers excluded) — a lower bound on wire bytes when a send
                 fails mid-chunk; conservative for the <= assertions.
  handler-error  per handler exception (kind + detail). Reset-class kinds
                 (BrokenPipeError/ConnectionResetError/SSLError/EOF) are
                 EXPECTED evidence of the resolver's abort() on early exit /
                 cap failure when the body is drip-fed; the orchestrator
                 classifies them, this fixture only records.
"""
import argparse
import json
import socketserver
import ssl
import threading
import time
from pathlib import Path

args = argparse.Namespace()  # populated in __main__ before the server starts
log_lock = threading.Lock()


def log(row):
    row["t"] = round(time.monotonic(), 6)
    with log_lock:
        with open(args.log, "a", encoding="utf-8") as f:
            f.write(json.dumps(row, sort_keys=True) + "\n")


def player_body():
    """Healthy 480p video + default-language 140 audio pair, expire year 2100."""
    return {
        "playabilityStatus": {"status": "OK"},
        "videoDetails": {"videoId": "fixture"},
        "streamingData": {
            "expiresInSeconds": 21599,
            "formats": [],
            "adaptiveFormats": [
                {"itag": 135, "url": "http://127.0.0.1:9/video?expire=4102444800",
                 "mimeType": "video/mp4; codecs=\"avc1.4d401e\"",
                 "width": 640, "height": 480, "qualityLabel": "480p"},
                {"itag": 140, "url": "http://127.0.0.1:9/audio?expire=4102444800",
                 "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"",
                 "audioQuality": "AUDIO_QUALITY_MEDIUM",
                 "audioTrack": {"id": "en.4", "displayName": "English original",
                                "audioIsDefault": True}}
            ]
        }
    }


def recv_head(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(65536)
        if not chunk:
            return None, b""
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)
    return head.decode("iso-8859-1", "replace"), rest


def response(conn, host, path, status, body, content_type="application/json"):
    if isinstance(body, (dict, list)):
        body = json.dumps(body, separators=(",", ":")).encode()
    elif isinstance(body, str):
        body = body.encode()
    reason = {200: "OK", 403: "Forbidden", 404: "Not Found"}.get(status, "OK")
    head = (f"HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode()
    chunk = args.send_chunk_bytes
    row = {"event": "response", "host": host, "path": path, "status": status,
           "bytes_total": len(body), "bytes_sent": 0, "reset": False,
           "chunk_bytes": chunk, "delay_ms": args.send_chunk_delay_ms}
    try:
        conn.sendall(head)
        if chunk > 0 and len(body) > chunk:
            delay = args.send_chunk_delay_ms / 1000.0
            for i in range(0, len(body), chunk):
                part = body[i:i + chunk]
                conn.sendall(part)
                row["bytes_sent"] += len(part)
                if delay:
                    time.sleep(delay)
        else:
            conn.sendall(body)
            row["bytes_sent"] = len(body)
    except OSError:
        # Peer aborted (resolver early exit / cap failure). Record the partial
        # accounting, then let handle() log the handler-error row.
        row["reset"] = True
        raise
    finally:
        log(row)


def homepage_body():
    """<html> + filler (with optional token placement) + </html>.

    Overhead with the default fixture-vd token is 56 bytes; with a 450-char
    token it is 496 bytes (6 + 7 html tags + 8 + 9 script tags + 15 key literal
    + token + 1 closing quote). The orchestrator derives page totals from the
    same arithmetic.
    """
    token_block = f'<script>"visitorData":"{args.visitor_token}"</script>'
    filler = "x" * args.page_bytes
    prefix = "<html>"
    if args.no_token:
        return prefix + filler + "</html>"
    if args.token_offset >= 0:
        off = args.token_offset
        if off < len(prefix) or off - len(prefix) > args.page_bytes:
            raise ValueError(f"token-offset {off} outside page (prefix {len(prefix)}, "
                             f"filler {args.page_bytes})")
        cut = off - len(prefix)
        return prefix + filler[:cut] + token_block + filler[cut:] + "</html>"
    return prefix + filler + token_block + "</html>"


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        raw = self.request
        raw.settimeout(30)
        try:
            head, _ = recv_head(raw)
            if not head or not head.startswith("CONNECT "):
                log({"event": "bad-connect", "head": head})
                return
            authority = head.split()[1]
            host = authority.split(":", 1)[0]
            raw.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(args.cert, args.key)
            conn = context.wrap_socket(raw, server_side=True)
            request_head, body = recv_head(conn)
            if request_head is None:
                return
            lines = request_head.split("\r\n")
            method, path = lines[0].split(" ", 2)[:2]
            headers = {}
            for line in lines[1:]:
                if ":" in line:
                    k, v = line.split(":", 1)
                    headers[k.lower()] = v.strip()
            length = int(headers.get("content-length", "0"))
            while len(body) < length:
                more = conn.recv(65536)
                if not more:
                    break
                body += more
            log({"event": "request", "host": host, "method": method, "path": path,
                 "bytes": length, "visitor": headers.get("x-goog-visitor-id", "")})
            if host == "sponsor.ajay.app":
                response(conn, host, path, 200, [])
            elif host == "www.youtube.com" and path == "/":
                response(conn, host, path, 200, homepage_body(), "text/html")
            elif host == "www.youtube.com" and path.startswith("/youtubei/v1/player"):
                visitor = headers.get("x-goog-visitor-id", "")
                if args.require_visitor and visitor != args.visitor_token:
                    log({"event": "visitor-wall", "visitor_len": len(visitor),
                         "visitor_prefix": visitor[:16]})
                    response(conn, host, path, 200, {"playabilityStatus": {
                        "status": "LOGIN_REQUIRED",
                        "reason": "Sign in to confirm you're not a bot"}})
                else:
                    response(conn, host, path, 200, player_body())
            else:
                response(conn, host, path, 404,
                         {"error": "unfixtured", "host": host, "path": path})
            conn.close()
        except Exception as e:
            log({"event": "handler-error", "kind": type(e).__name__, "detail": str(e)})


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--cert", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--log", required=True)
    p.add_argument("--page-bytes", type=int, default=0)
    p.add_argument("--visitor-token", default="fixture-vd")
    p.add_argument("--no-token", action="store_true")
    p.add_argument("--token-offset", type=int, default=-1)
    p.add_argument("--require-visitor", action="store_true")
    p.add_argument("--send-chunk-bytes", type=int, default=0)
    p.add_argument("--send-chunk-delay-ms", type=int, default=0)
    args = p.parse_args()
    Path(args.log).write_text("", encoding="utf-8")
    with Server(("127.0.0.1", args.port), Handler) as srv:
        log({"event": "ready", "port": srv.server_address[1],
             "page_bytes": args.page_bytes, "no_token": args.no_token,
             "token_offset": args.token_offset,
             "visitor_token_len": len(args.visitor_token),
             "require_visitor": args.require_visitor,
             "send_chunk_bytes": args.send_chunk_bytes,
             "send_chunk_delay_ms": args.send_chunk_delay_ms})
        srv.serve_forever()