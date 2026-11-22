#!/usr/bin/env python3
"""Generate a short, synthetic stereo DFF fixture with deterministic payload."""
import struct
import sys

def chunk(tag, payload):
    return tag + struct.pack(">Q", len(payload)) + payload + b"\0" * (len(payload) % 2)

def fixture(path):
    prop = b"SND " + chunk(b"FS  ", struct.pack(">I", 2822400))
    prop += chunk(b"CHNL", struct.pack(">H", 2) + b"SLFTSRGT")
    prop += chunk(b"CMPR", b"DSD " + bytes([3]) + b"DSD")
    # 100 ms; known bit-pattern, only use with a simulated output.
    data = bytes([0x10, 0x20, 0x11, 0x21, 0x12, 0x22, 0x13, 0x23]) * 8820
    dff = chunk(b"FRM8", b"DSD " + chunk(b"FVER", b"\x01\x05\x00\x00") + chunk(b"PROP", prop) + chunk(b"DSD ", data))
    with open(path, "wb") as file:
        file.write(dff)

if __name__ == "__main__":
    fixture(sys.argv[1])
