#!/usr/bin/env python3
"""Exercise native two-track playback with a clocked, disposable sink."""
import functools
import http.server
import tempfile
import threading
import time
import wave
from pathlib import Path
from upnp_smoke import Renderer, check
from http_pcm_smoke import set_uri, play, wait_state, AVT, INSTANCE
import sys

def run(renderer):
    with tempfile.TemporaryDirectory() as directory:
        for name in ("first.wav", "second.wav"):
            with wave.open(str(Path(directory) / name), "wb") as wav:
                wav.setnchannels(2); wav.setsampwidth(2); wav.setframerate(44100)
                wav.writeframes(bytes(44100 * 4 * 3))
        handler = functools.partial(http.server.SimpleHTTPRequestHandler,directory=directory)
        with http.server.ThreadingHTTPServer(("127.0.0.1",0),handler) as server:
            thread = threading.Thread(target=server.serve_forever,daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            first, second = base + "/first.wav", base + "/second.wav"
            try:
                set_uri(renderer, first)
                renderer.call(AVT,"SetNextAVTransportURI",{**INSTANCE,"NextURI":second,"NextURIMetaData":""})
                play(renderer)
                seen = []
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    transport = renderer.call(AVT,"GetTransportInfo",INSTANCE)
                    media = renderer.call(AVT,"GetMediaInfo",INSTANCE)
                    uri = media["CurrentURI"]
                    if not seen or seen[-1] != uri:
                        seen.append(uri)
                    if transport["CurrentTransportStatus"] != "OK":
                        raise AssertionError(transport)
                    if transport["CurrentTransportState"] == "STOPPED":
                        break
                    time.sleep(.05)
                assert first in seen and second in seen, seen
                assert seen.index(first) < seen.index(second), seen
                wait_state(renderer,"STOPPED","OK")
                print("PASS: queued track becomes current and both PCM tracks complete")
            finally:
                renderer.call(AVT,"Stop",INSTANCE)
                set_uri(renderer,"")
                server.shutdown()
                thread.join(timeout=5)

if __name__ == "__main__":
    assert len(sys.argv) == 3 and sys.argv[2] == "--test-instance"
    run(Renderer(sys.argv[1]))
