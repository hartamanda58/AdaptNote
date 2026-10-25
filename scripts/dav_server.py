#!/usr/bin/env python3
"""Minimal WebDAV server (stdlib only) used by synctest.

Supports: PROPFIND (Depth 0/1), GET, PUT (with If-Match), MKCOL, DELETE,
Basic auth, ETag generation. Not a general purpose server: it exists so the
sync tests exercise real HTTP instead of mocks.
"""
import argparse
import base64
import hashlib
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, quote
from xml.sax.saxutils import escape

ROOT = "."
USER = "bxn"
PASSWORD = "test"


def etag_for(path):
    st = os.stat(path)
    seed = "%d:%d" % (st.st_mtime_ns, st.st_size)
    return '"%s"' % hashlib.sha1(seed.encode()).hexdigest()[:16]


def fs_path(url_path):
    rel = unquote(url_path).lstrip("/")
    full = os.path.normpath(os.path.join(ROOT, rel))
    if not full.startswith(os.path.abspath(ROOT)):
        raise ValueError("path traversal")
    return full


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if os.environ.get("DAV_VERBOSE"):
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # ---------------- auth

    def authorized(self):
        header = self.headers.get("Authorization", "")
        if not header.startswith("Basic "):
            return False
        try:
            raw = base64.b64decode(header[6:]).decode()
        except Exception:
            return False
        return raw == "%s:%s" % (USER, PASSWORD)

    def deny(self):
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="dav"')
        self.send_header("Content-Length", "0")
        self.end_headers()

    def respond(self, code, body=b"", headers=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    # ---------------- verbs

    def do_PROPFIND(self):
        if not self.authorized():
            return self.deny()
        try:
            path = fs_path(self.path)
        except ValueError:
            return self.respond(403)
        if not os.path.exists(path):
            return self.respond(404)
        depth = self.headers.get("Depth", "1")
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)

        entries = [path]
        if depth != "0" and os.path.isdir(path):
            entries += [os.path.join(path, n) for n in sorted(os.listdir(path))]

        parts = ['<?xml version="1.0" encoding="utf-8"?>', '<D:multistatus xmlns:D="DAV:">']
        base = self.path.rstrip("/")
        for e in entries:
            rel = os.path.relpath(e, ROOT)
            href = base if rel == "." else base + "/" + quote(rel.replace(os.sep, "/"))
            is_dir = os.path.isdir(e)
            parts.append("<D:response><D:href>%s</D:href><D:propstat><D:prop>" % escape(href))
            if is_dir:
                parts.append("<D:resourcetype><D:collection/></D:resourcetype>")
            else:
                st = os.stat(e)
                parts.append("<D:resourcetype/>")
                parts.append("<D:getcontentlength>%d</D:getcontentlength>" % st.st_size)
                parts.append("<D:getetag>%s</D:getetag>" % escape(etag_for(e)))
                parts.append("<D:getlastmodified>%s</D:getlastmodified>"
                             % self.date_time_string(st.st_mtime))
            parts.append("<D:status>HTTP/1.1 200 OK</D:status></D:prop></D:propstat></D:response>")
        parts.append("</D:multistatus>")
        self.respond(207, "".join(parts).encode(), {"Content-Type": 'application/xml; charset="utf-8"'})

    def do_GET(self):
        if not self.authorized():
            return self.deny()
        try:
            path = fs_path(self.path)
        except ValueError:
            return self.respond(403)
        if not os.path.isfile(path):
            return self.respond(404)
        with open(path, "rb") as f:
            data = f.read()
        self.respond(200, data, {"ETag": etag_for(path), "Content-Type": "application/json"})

    def do_PUT(self):
        if not self.authorized():
            return self.deny()
        try:
            path = fs_path(self.path)
        except ValueError:
            return self.respond(403)
        length = int(self.headers.get("Content-Length") or 0)
        payload = self.rfile.read(length) if length else b""

        if_match = self.headers.get("If-Match")
        self.log_message("PUT %s If-Match=%r etag=%r len=%d",
                         self.path, if_match,
                         etag_for(path) if os.path.isfile(path) else None, len(payload))
        if if_match:
            if not os.path.isfile(path) or etag_for(path) != if_match:
                # the body was already consumed above, so the connection stays in sync
                return self.respond(412)
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        existed = os.path.isfile(path)
        with open(path, "wb") as f:
            f.write(payload)
        self.respond(204 if existed else 201, b"", {"ETag": etag_for(path)})

    def do_MKCOL(self):
        if not self.authorized():
            return self.deny()
        try:
            path = fs_path(self.path)
        except ValueError:
            return self.respond(403)
        if os.path.isdir(path):
            return self.respond(405)
        if os.path.exists(path):
            return self.respond(405)
        os.makedirs(path, exist_ok=True)
        self.respond(201)

    def do_DELETE(self):
        if not self.authorized():
            return self.deny()
        try:
            path = fs_path(self.path)
        except ValueError:
            return self.respond(403)
        if os.path.isfile(path):
            os.unlink(path)
            return self.respond(204)
        return self.respond(404)


def main():
    global ROOT, USER, PASSWORD
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--user", default="bxn")
    ap.add_argument("--password", default="test")
    args = ap.parse_args()

    ROOT = os.path.abspath(args.root)
    os.makedirs(ROOT, exist_ok=True)
    USER = args.user
    PASSWORD = args.password

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    port = httpd.server_address[1]
    print("READY %d" % port, flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
