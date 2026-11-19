#!/usr/bin/env python3
"""Loopback CONNECT+TLS fixture proxy for the resolver contract suite (ROB-07).

Promoted from the T0 torture-lab fixture (scratch/torture-t0/fixture_proxy.py)
into a checked-in, self-contained test dependency. Standard library only, binds
127.0.0.1 only, and NEVER talks to an upstream network: every request is
answered from the fixtures below.

Routes (after CONNECT + TLS termination with the caller-supplied cert):
  GET  www.youtube.com/                  -> HTML page carrying visitorData,
                                            delayed by --visitor-delay-ms
                                            (fixture-side sleep; the knob that
                                            places the reply before/after the
                                            resolver's kTimeoutMs deadline)
  POST www.youtube.com/youtubei/v1/player* -> healthy 480p video + itag-140
                                            audio pair with a far-future expiry
  ANY  sponsor.ajay.app                  -> [] (empty EDL)
  anything else                          -> 404 {"error": "unfixtured"}

Request log (--log, JSONL): one {"event": "request", host, method, path, bytes,
visitor, t} row per request AS RECEIVED (before any delay), one
{"event": "response", host, path, status, delay_ms, t} row per reply sent, plus
a {"event": "ready", port, visitor_delay_ms, t} row at startup. The orchestrator
asserts against the request rows; response rows carry reply timing evidence.
"""
import argparse
import json
import socketserver
import ssl
import threading
import time
from pathlib import Path

args = argparse.Namespace()  # populated in __main__ before any handler runs
log_lock = threading.Lock()


def log(row):
    row["t"] = round(time.monotonic(), 6)
    with log_lock:
        with open(args.log, "a", encoding="utf-8") as f:
            f.write(json.dumps(row, sort_keys=True) + "\n")


def player_body():
    # Healthy pair: 480p H.264 video + itag-140 AAC audio (default-language
    # track), expire=4102444800 (2100-01-01) so the expiry gate always passes.
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


def response(conn, status, body, host, path, delay_ms=0, content_type="application/json"):
    if isinstance(body, (dict, list)):
        body = json.dumps(body, separators=(",", ":")).encode()
    elif isinstance(body, str):
        body = body.encode()
    reason = {200: "OK", 403: "Forbidden", 404: "Not Found"}.get(status, "OK")
    head = (f"HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode()
    conn.sendall(head + body)
    log({"event": "response", "host": host, "path": path, "status": status,
         "delay_ms": delay_ms})


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        raw = self.request
        delay_s = args.visitor_delay_ms / 1000.0
        raw.settimeout(max(20.0, delay_s + 20.0))
        try:
            head, _ = recv_head(raw)
            if not head or not head.startswith("CONNECT "):
                log({"event": "bad-connect", "head": head})
                return
            authority = head.split()[1]
            host = authority.split(":", 1)[0]
            raw.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
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
                chunk = conn.recv(65536)
                if not chunk:
                    break
                body += chunk
            log({"event": "request", "host": host, "method": method, "path": path,
                 "bytes": length, "visitor": headers.get("x-goog-visitor-id", "")})
            if host == "sponsor.ajay.app":
                response(conn, 200, [], host, path)
            elif host == "www.youtube.com" and path == "/" and method == "GET":
                if delay_s > 0:
                    time.sleep(delay_s)
                page = '<html><script>"visitorData":"fixture-vd"</script></html>'
                response(conn, 200, page, host, path,
                         delay_ms=args.visitor_delay_ms, content_type="text/html")
            elif host == "www.youtube.com" and path.startswith("/youtubei/v1/player"):
                response(conn, 200, player_body(), host, path)
            else:
                response(conn, 404, {"error": "unfixtured", "host": host, "path": path},
                         host, path)
            conn.close()
        except Exception as e:
            log({"event": "handler-error", "kind": type(e).__name__, "detail": str(e)})


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--cert", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--log", required=True)
    p.add_argument("--visitor-delay-ms", type=int, default=0,
                   help="fixture-side sleep before the visitorData GET reply")
    args = p.parse_args()
    if args.visitor_delay_ms < 0:
        p.error("--visitor-delay-ms must be >= 0")
    Path(args.log).write_text("", encoding="utf-8")
    with Server(("127.0.0.1", args.port), Handler) as srv:
        log({"event": "ready", "port": args.port,
             "visitor_delay_ms": args.visitor_delay_ms})
        srv.serve_forever()
