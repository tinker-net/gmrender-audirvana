#!/usr/bin/env python3
"""Validate exact HTTP byte recovery after the first response is truncated."""
import http.server
import socket
import subprocess
import tempfile
import threading
from pathlib import Path

PAYLOAD = bytes(range(256)) * 8192
CUT = 65536
ranges = []

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    sent_truncated = False
    def do_GET(self):
        header = self.headers.get("Range")
        if header:
            start = int(header.removeprefix("bytes=").split("-")[0])
            ranges.append(start)
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(PAYLOAD)-1}/{len(PAYLOAD)}")
            self.send_header("Content-Length", str(len(PAYLOAD)-start))
            self.send_header("Accept-Ranges","bytes")
            self.end_headers()
            self.wfile.write(PAYLOAD[start:])
        else:
            self.send_response(200)
            self.send_header("Content-Length",str(len(PAYLOAD)))
            self.send_header("Accept-Ranges","bytes")
            self.end_headers()
            if not Handler.sent_truncated:
                Handler.sent_truncated = True
                self.wfile.write(PAYLOAD[:CUT])
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_WR)
                self.close_connection = True
            else:
                self.wfile.write(PAYLOAD)

with tempfile.TemporaryDirectory() as temp:
    out = Path(temp)/"download"
    with http.server.ThreadingHTTPServer(("127.0.0.1",0),Handler) as server:
        thread = threading.Thread(target=server.serve_forever,daemon=True)
        thread.start()
        try:
            subprocess.run(["gst-launch-1.0","-q","souphttpsrc",f"location=http://127.0.0.1:{server.server_port}/stream",
                            "retries=3","timeout=15","!","filesink",f"location={out}"],check=True,timeout=30)
            assert out.read_bytes() == PAYLOAD, "HTTP recovery duplicated or lost bytes"
            assert ranges and ranges[0] > 0, f"No byte-range resume observed: {ranges}"
            print("PASS: HTTP interruption resumed by byte range with exact payload")
        finally:
            server.shutdown()
            thread.join(timeout=5)
