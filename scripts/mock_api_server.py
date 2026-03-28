#!/usr/bin/env python3
# Copyright (C) 2025  Ryan Patton
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Minimal HTTP mock for `surfcam_test_http` (check-streaming + HLS presign/PUT)."""

from __future__ import annotations

import json
import socketserver
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer


class ThreadedHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True


class Handler(BaseHTTPRequestHandler):
    server_version = "SurfCamMock/1.0"

    def log_message(self, _format: str, *_args: object) -> None:
        return

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/health":
            self.send_response(200)
            self.end_headers()
            return
        if parsed.path != "/check-streaming-requested":
            self.send_error(404)
            return

        qs = urllib.parse.parse_qs(parsed.query)
        spot = qs.get("spot_id", [""])[0]
        if spot == "badjson_spot":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"not json {")
            return
        if spot == "slow_spot":
            time.sleep(12)

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"stream_requested":false}')

    def do_POST(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path.rstrip("/") != "/hls/presign":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        try:
            data = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            self.send_error(400)
            return
        key = data.get("key", "")
        if "PRESIGN_FAIL_MARKER" in key:
            self.send_response(500)
            self.end_headers()
            return

        port = self.server.server_address[1]
        url = f"http://127.0.0.1:{port}/mock-put"
        body = json.dumps({"url": url, "headers": {}})
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body.encode("utf-8"))

    def do_PUT(self) -> None:
        if not self.path.startswith("/mock-put"):
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            remaining = length
            while remaining > 0:
                chunk = self.rfile.read(min(65536, remaining))
                if not chunk:
                    break
                remaining -= len(chunk)
        self.send_response(204)
        self.end_headers()


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    httpd = ThreadedHTTPServer(("127.0.0.1", port), Handler)
    print(f"mock_api_server listening on 127.0.0.1:{port}", flush=True)
    httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
