#!/usr/bin/env python3
"""Fake OpenAI-compatible streaming endpoint for aitest.

POST /v1/chat/completions  -> text/event-stream with reasoning_content + content deltas.
GET  /last-request         -> JSON {"headers": {...}, "body": {...}} of the last POST.
Model name containing "slow" makes the stream pause so a test can cancel it.
Model name containing "fail" returns HTTP 401 with an OpenAI-style error body.
"""
import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"last": None}
LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/last-request"):
            with LOCK:
                return self._json(200, STATE["last"] or {})
        self._json(404, {"error": "not found"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw.decode())
        except Exception:
            body = {}
        headers = {k.lower(): v for k, v in self.headers.items()}
        with LOCK:
            STATE["last"] = {"headers": headers, "body": body}

        model = str(body.get("model", ""))
        if "fail" in model:
            return self._json(401, {"error": {"message": "invalid api key", "type": "auth"}})

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        def send_frame(obj):
            data = ("data: " + json.dumps(obj) + "\n\n").encode()
            self.wfile.write(("%x\r\n" % len(data)).encode() + data + b"\r\n")
            self.wfile.flush()

        def delta(**kw):
            send_frame({"choices": [{"index": 0, "delta": kw, "finish_reason": None}]})

        try:
            delta(reasoning_content="thinking")
            delta(reasoning_content=" about it…")
            delta(content="Hello")
            if "slow" in model:
                for _ in range(40):
                    time.sleep(0.05)
                    delta(content=".")
            delta(content=" from fake")
            delta(content=" server")
            send_frame({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
            end = b"data: [DONE]\n\n"
            self.wfile.write(("%x\r\n" % len(end)).encode() + end + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    args = ap.parse_args()
    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("READY %d" % httpd.server_address[1], flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
