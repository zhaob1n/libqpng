#!/usr/bin/env python3
"""Regenerate tests/data/refs.txt: FNV-1a-64 of the png crate / image-rs decode output for every test PNG in the
three output modes (raw = png IDENTITY, expand = png EXPAND, imagers = image-rs PngDecoder::read_image).
'ERR' = the Rust decoder rejects the file. Requires tools/rsref built (cargo build --release)."""
import subprocess, sys, os, glob, tempfile
here = os.path.dirname(os.path.abspath(__file__))
rsref = os.path.join(here, "..", "tools", "rsref", "target", "release", "rsref")
def fnv(b):
    h = 0xcbf29ce484222325
    for x in b: h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h
files = sorted(glob.glob(os.path.join(here, "data", "*", "*.png")))
with tempfile.TemporaryDirectory() as td, open(os.path.join(here, "data", "refs.txt"), "w") as out:
    for f in files:
        rel = os.path.relpath(f, os.path.join(here, "data"))
        row = [rel]
        for mode in ("raw", "expand", "imagers"):
            o = os.path.join(td, "o.raw")
            r = subprocess.run([rsref, "decode", mode, f, o], capture_output=True)
            if r.returncode: row.append("ERR")
            else:
                data = open(o, "rb").read(); w, h = r.stdout.decode().split()[:2]
                row.append(f"{w}x{h}:{len(data)}:{fnv(data):016x}")
        out.write(" ".join(row) + "\n")
print(len(files), "files")
