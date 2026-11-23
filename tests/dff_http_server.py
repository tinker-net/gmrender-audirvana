#!/usr/bin/env python3
"""Small byte-range-capable HTTP test server for DFF fixtures.

GStreamer's DFF parser probes the container using HTTP byte ranges.
Python's SimpleHTTPRequestHandler does not implement Range/206 on
all supported versions, so it cannot validate DFF over HTTP.
"""
import argparse
import http.server
from pathlib import Path

class RangeHandler(http.server.BaseHTTPRequestHandler):
    file: Path = None

    def respond(self, head=False):
        if self.path != "/gmrender-fixture.dff":
            self.send_error(404)
            return
        length = self.file.stat().st_size
        low, high = 0, length - 1
        requested = self.headers.get("Range")
        if requested:
            if not requested.startswith("bytes=") or "," in requested:
                self.send_error(400)
                return
            pair = requested[6:].split("-", 1)
            if len(pair) != 2:
                self.send_error(400)
                return
            try:
                if pair[0]:
                    low = int(pair[0])
                    if pair[1]:
                        high = min(high, int(pair[1]))
                else:
                    suffix = int(pair[1])
                    if suffix <= 0:
                        raise ValueError
                    low = max(0, length - suffix)
            except ValueError:
                self.send_error(400)
                return
            if low >= length or high < low or low < 0:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{length}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        self.send_response(206 if requested else 200)
        self.send_header("Content-Type", "audio/dff")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(high - low + 1))
        if requested:
            self.send_header("Content-Range", f"bytes {low}-{high}/{length}")
        self.end_headers()
        if not head:
            with self.file.open("rb") as stream:
                stream.seek(low)
                remaining = high - low + 1
                while remaining:
                    data = stream.read(min(65536, remaining))
                    if not data:
                        break
                    self.wfile.write(data)
                    remaining -= len(data)

    def do_HEAD(self):
        self.respond(head=True)

    def do_GET(self):
        self.respond()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("filename")
    ap.add_argument("--port", type=int, default=49801)
    args = ap.parse_args()
    RangeHandler.file = Path(args.filename)
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), RangeHandler) as server:
        server.serve_forever()
