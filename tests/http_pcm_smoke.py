#!/usr/bin/env python3
"""HTTP/PCM smoke test. Only use a disposable renderer with a fake audio sink."""
import argparse
import functools
import http.server
import tempfile
import threading
import time
import wave
from pathlib import Path
from upnp_smoke import Renderer, check

AVT = "AVTransport"
INSTANCE = {"InstanceID": "0"}


def wait_state(renderer, state, status, seconds=15):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        last = renderer.call(AVT, "GetTransportInfo", INSTANCE)
        if last["CurrentTransportState"] == state and last["CurrentTransportStatus"] == status:
            return
        time.sleep(0.1)
    raise AssertionError(f"Expected {state}/{status}, last transport response: {last}")


def set_uri(renderer, uri):
    renderer.call(AVT, "SetAVTransportURI",
                  {**INSTANCE, "CurrentURI": uri, "CurrentURIMetaData": ""})


def play(renderer):
    renderer.call(AVT, "Play", {**INSTANCE, "Speed": "1"})


def run(renderer):
    with tempfile.TemporaryDirectory() as directory:
        # Silent stereo PCM, 16-bit, 44.1 kHz. The runner has no real DAC.
        with wave.open(str(Path(directory) / "test.wav"), "wb") as wav:
            wav.setnchannels(2)
            wav.setsampwidth(2)
            wav.setframerate(44100)
            wav.writeframes(bytes(44100 * 4 * 3))
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=directory)
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as http:
            thread = threading.Thread(target=http.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{http.server_port}"
            try:
                set_uri(renderer, base + "/missing.wav")
                play(renderer)
                wait_state(renderer, "STOPPED", "ERROR_OCCURRED")
                check(renderer.call(AVT, "GetMediaInfo", INSTANCE)["CurrentURI"],
                      base + "/missing.wav", "Failed URI retained")
                set_uri(renderer, base + "/test.wav")
                check(renderer.call(AVT, "GetTransportInfo", INSTANCE)["CurrentTransportStatus"],
                      "OK", "New URI resets error status")
                play(renderer)
                wait_state(renderer, "STOPPED", "OK", seconds=15)
                print("PASS: HTTP error reported, subsequent PCM stream completed normally")
            finally:
                renderer.call(AVT, "Stop", INSTANCE)
                set_uri(renderer, "")
                http.shutdown()
                thread.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_url")
    parser.add_argument("--test-instance", action="store_true")
    args = parser.parse_args()
    if not args.test_instance:
        parser.error("--test-instance is required")
    run(Renderer(args.base_url))
