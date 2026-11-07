#!/usr/bin/env python3
"""Tiny path-style S3 stand-in for AdaptNote's sync tests.

It deliberately does not validate SigV4 math (that is pinned by the AWS
get-vanilla vector in vocabtest); it does require the signature headers to be
present, honours If-Match on PUT, and returns ETags so the client's conditional
logic is exercised end to end.

Layout: <root>/<bucket>/<key...>
Prints "PORT <n>" once listening.
"""
import hashlib
import os
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.environ.get("FAKE_S3_ROOT", "s3data")
os.makedirs(ROOT, exist_ok=True)


def etag_for(data: bytes) -> str:
    return '"%s"' % hashlib.md5(data).hexdigest()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # keep test output clean
        return

    def _target(self):
        path = urllib.parse.urlparse(self.path).path
        parts = path.lstrip("/").split("/", 1)
        bucket = parts[0]
        key = urllib.parse.unquote(parts[1]) if len(parts) > 1 else ""
        return bucket, key

    def _file(self):
        bucket, key = self._target()
        return os.path.join(ROOT, bucket, key)

    def _authorized(self):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("AWS4-HMAC-SHA256 "):
            self._error(403, "missing SigV4 authorization")
            return False
        if not self.headers.get("x-amz-content-sha256") or not self.headers.get("x-amz-date"):
            self._error(403, "missing signing headers")
            return False
        return True

    def _error(self, status, message):
        body = ("<Error><Code>%d</Code><Message>%s</Message></Error>" %
                (status, message)).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/xml")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        if not self._authorized():
            return
        path = self._file()
        if not os.path.isfile(path):
            self._error(404, "no such key")
            return
        with open(path, "rb") as fh:
            data = fh.read()
        self.send_response(200)
        self.send_header("ETag", etag_for(data))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_HEAD(self):
        if not self._authorized():
            return
        path = self._file()
        if not os.path.isfile(path):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        with open(path, "rb") as fh:
            data = fh.read()
        self.send_response(200)
        self.send_header("ETag", etag_for(data))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()

    def do_PUT(self):
        if not self._authorized():
            return
        length = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(length) if length else b""
        path = self._file()
        match = self.headers.get("If-Match")
        if match:
            if not os.path.isfile(path):
                self._error(412, "precondition failed")
                return
            with open(path, "rb") as fh:
                current = fh.read()
            if etag_for(current) != match:
                self._error(412, "precondition failed")
                return
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
        self.send_response(200)
        self.send_header("ETag", etag_for(data))
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_DELETE(self):
        if not self._authorized():
            return
        path = self._file()
        if os.path.isfile(path):
            os.remove(path)
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print("PORT %d" % server.server_port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
