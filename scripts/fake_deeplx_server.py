#!/usr/bin/env python3
"""Minimal DeepLX-compatible server for the integration test.

Usage: fake_deeplx_server.py <port> [token]
POST /translate  {"text","source_lang","target_lang"} -> {"code":200,"data":...}
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18089
TOKEN = sys.argv[2] if len(sys.argv) > 2 else ""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        path = self.path.split("?")[0]
        if path not in ("/translate", "/v1/translate"):
            self.send_error(404)
            return
        if path == "/v1/translate":
            length = int(self.headers.get("Content-Length", "0"))
            try:
                req = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError:
                self.send_error(400)
                return
            texts = req.get("text") or [""]
            target = str(req.get("target_lang", "zh-Hans"))
            body = json.dumps(
                {
                    "translations": [
                        {
                            "detected_source_language": "en",
                            "text": f"FREE[{target}] {texts[0]}",
                        }
                    ]
                },
                ensure_ascii=False,
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if TOKEN and self.headers.get("Authorization", "") != "Bearer " + TOKEN:
            body = json.dumps({"code": 401, "message": "invalid token"}).encode()
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            req = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self.send_error(400)
            return
        text = str(req.get("text", ""))
        target = str(req.get("target_lang", "ZH"))
        body = json.dumps(
            {
                "code": 200,
                "message": "success",
                "data": f"DLX[{target}] {text}",
                "source_lang": "auto",
                "target_lang": target,
            },
            ensure_ascii=False,
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
