#!/usr/bin/env python3
"""
Reproducible qpng-vs-Rust benchmark driver.

  bench/run.py encode  [--rounds N] [--reps N] [--cpu C] [--configs a,b,..] [--out results.csv]
  bench/run.py decode  ...
  bench/run.py deflate ...   (raw zlib streams: qpng's libdeflate vs fdeflate)
  bench/run.py inflate ...   (decompress PNG IDAT zlib streams made by several encoders)

Method: checks box load first (refuses to start if the 1-min load average is
above --max-load unless --force), pins every process to one core (taskset),
runs all configurations in rotating order per round (A B C, B C A, ...),
keeps every timed rep, and reports per-file medians aggregated per group as
MB/s of *raw image bytes* (decoded size) plus compression ratio
(raw / encoded).  1 untimed warm-up rep per file inside each tool.
"""
import argparse, collections, csv, io, os, re, statistics as st, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PFBENCH = os.environ.get("PFBENCH", os.path.join(ROOT, "build", "qpngbench"))
RSREF = os.environ.get("RSREF", os.path.join(ROOT, "tools/rsref/target/release/rsref"))
RSREF_Z = os.environ.get("RSREF_ZLIBRS", os.path.join(ROOT, "tools/rsref/target-zlibrs/release/rsref"))
CORPUS = os.environ.get("QPNG_CORPUS", "/workspace/image-bench/png-next/images/formats")
RAWDIR = os.environ.get("QPNG_RAW", "/workspace/deflate-bench/data")

# label -> argv prefix (files appended).  {R} = reps.
ENCODE = {
    "png-fastest":      [RSREF, "bench-encode", "fastest", "{R}"],
    "png-fast":         [RSREF, "bench-encode", "fast", "{R}"],
    "png-balanced":     [RSREF, "bench-encode", "balanced", "{R}"],
    "png-high":         [RSREF, "bench-encode", "high", "{R}"],
    "pngz-balanced":    [RSREF_Z, "bench-encode", "balanced", "{R}"],
    "pngz-high":        [RSREF_Z, "bench-encode", "high", "{R}"],
    "pf-fastest":       [PFBENCH, "encode", "fastest", "{R}"],
    "pf-fast":          [PFBENCH, "encode", "fast", "{R}"],
    "pf-default":       [PFBENCH, "encode", "default", "{R}"],
    "pf-best":          [PFBENCH, "encode", "best", "{R}"],
    # backend forced (qpng_encode_options.backend)
    **{f"pf-{p}-{b}": [PFBENCH, "encode", f"{p}#{b}", "{R}"] for p in ("fastest", "fast", "default", "best")
       for b in ("ld", "zng")},
    # exploration
    **{f"pf-u{f}": [PFBENCH, "encode", f"fast:{f}:ultra", "{R}"] for f in (0, 1, 2, 4, 5)},
    "pf-fast-pf":       [PFBENCH, "encode", "fast:5:pf", "{R}"],
    **{f"pf-n{n}": [PFBENCH, "encode", f"fast:0:{n}", "{R}"] for n in (1, 2, 3, 4, 5, 6, 7, 8, 9)},
    "pf-l2":            [PFBENCH, "encode", "fast:5:2", "{R}"],
    "pf-l4":            [PFBENCH, "encode", "fast:5:4", "{R}"],
    # exploration: adaptive filter + libdeflate level N
    **{f"pf-l{n}": [PFBENCH, "encode", f"fast:5:{n}", "{R}"] for n in (1, 3, 5, 6, 7, 8, 9, 10, 11, 12)},
}
DECODE = {
    "imagers":     [RSREF, "bench-decode", "imagers", "{R}"],
    "png-raw":     [RSREF, "bench-decode", "raw", "{R}"],
    "pf-imagers":  [PFBENCH, "decode", "imagers", "{R}"],
    "pf-raw":      [PFBENCH, "decode", "raw", "{R}"],
}
DEFLATE = {
    "fdeflate-ultrafast": [RSREF, "bench-deflate", "ultrafast", "{R}"],
    "fdeflate-rle":       [RSREF, "bench-deflate", "rle", "{R}"],
    "fdeflate-1":         [RSREF, "bench-deflate", "1", "{R}"],
    "fdeflate-6":         [RSREF, "bench-deflate", "6", "{R}"],
    "pf-rle":             [PFBENCH, "deflate", "pfrle", "{R}"],
    "pf-fast":            [PFBENCH, "deflate", "pf", "{R}"],
    "pf-6":               [PFBENCH, "deflate", "6", "{R}"],
}

# optional (only via --configs): previous prototype, to check the cleaned-up decoder for regressions
V3 = os.environ.get("PNGNEXT3", "/workspace/image-bench/png-next/bin/pngnext3")
EXTRA_DECODE = {"v3-imagers": [V3, "v3", "decode-qpng3", str(3 << 20), "{R}"]}

INFLATE = {
    "fdeflate":        [RSREF, "bench-inflate", "-", "{R}"],
    "libdeflate":      [PFBENCH, "inflate", "stock", "{R}"],
    "pf-pairs":        [PFBENCH, "inflate", "pairs", "{R}"],
    "zlib-ng":         [PFBENCH, "inflate", "zng", "{R}"],
}
STREAMS = os.environ.get("QPNG_STREAMS", "/workspace/deflate-bench/streams/png")

FMT_GROUPS = [("rgb8", r"\.rgb8t?\."), ("rgba8", r"\.rgba8\."), ("ga8", r"\.ga8\."),
              ("g8", r"\.g8t?\."), ("p8", r"\.p8t?\."), ("sub8", r"\.(g|p)[124]\."),
              ("16bit", r"\.(g16t?|ga16|rgb16t?|rgba16)\.")]

def content(f):
    b = os.path.basename(f)
    return "graphics" if b.startswith(("screenshot", "graphics")) else "photo"

def group_of(f, mode):
    if mode == "deflate":
        return "graphics" if "screen" in f else "photo"
    if mode == "inflate":
        return "stream from " + f.split(".idat.")[1].replace(".zlib", "")
    for g, rx in FMT_GROUPS:
        if re.search(rx, os.path.basename(f)):
            return f"{content(f)}/{g}"
    return None

def load_ok(max_load):
    la = os.getloadavg()[0]
    return la, la <= max_load

def run(argv, reps, files, cpu):
    argv = [a.replace("{R}", str(reps)) for a in argv]
    out = subprocess.run(["taskset", "-c", str(cpu)] + argv + files,
                         check=True, capture_output=True, text=True).stdout
    return list(csv.reader(io.StringIO(out)))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["encode", "decode", "deflate", "inflate"])
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--cpu", type=int, default=3)
    ap.add_argument("--configs", default="")
    ap.add_argument("--files", default="", help="regex filter on file names")
    ap.add_argument("--interlaced", action="store_true", help="decode: include .i.png")
    ap.add_argument("--max-load", type=float, default=2.5)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--out", default="")
    ap.add_argument("--merge", default="", help="comma-separated CSVs from earlier runs (--out) whose configs "
                    "are added to the table (e.g. slow reference encoders measured once)")
    ap.add_argument("--order", default="", help="column order for the table (default: run configs, then merged)")
    a = ap.parse_args()

    table = {"encode": ENCODE, "decode": DECODE, "deflate": DEFLATE, "inflate": INFLATE}[a.mode]
    if a.mode == "decode":
        table = {**table, **EXTRA_DECODE}
    cfgs = [c for c in (a.configs.split(",") if a.configs else table if a.mode != "decode" else DECODE)]
    # ad-hoc qpngbench configs: "pfx=PARAM" (e.g. pfx=fast:5:9/4096/258)
    table = {**table, **{c: [PFBENCH, a.mode, c[4:], "{R}"] for c in cfgs if c.startswith("pfx=")}}
    if a.mode == "inflate":
        files = sorted(f"{RAWDIR}/png/{f.split('.idat.')[0]}.idat:{STREAMS}/{f}" for f in os.listdir(STREAMS)
                       if f.endswith(".zlib"))
    elif a.mode == "deflate":
        files = sorted([f"{RAWDIR}/png/{f}" for f in os.listdir(f"{RAWDIR}/png") if f.endswith(".idat")] +
                       [f"{RAWDIR}/png-screen/{f}" for f in os.listdir(f"{RAWDIR}/png-screen") if f.endswith(".filt")])
    else:
        files = sorted(os.path.join(CORPUS, f) for f in os.listdir(CORPUS) if f.endswith(".png")
                       and (a.interlaced or a.mode == "decode" and a.interlaced or ".i." not in f))
    if a.files:
        files = [f for f in files if re.search(a.files, f)]
    files = [f for f in files if group_of(f, a.mode)]

    la, ok = load_ok(a.max_load)
    print(f"# load average {la:.2f} at start ({time.strftime('%Y-%m-%d %H:%M %Z')}), cpu {a.cpu}", file=sys.stderr)
    if not ok and not a.force:
        sys.exit(f"box busy (load {la:.2f} > {a.max_load}); retry later or --force")

    # raw (decoded) size per file
    raw = {}
    if a.mode == "inflate":
        raw = {f: os.path.getsize(f.split(":")[0]) for f in files}
    elif a.mode == "deflate":
        raw = {f: os.path.getsize(f) for f in files}
    else:
        for r in run([PFBENCH, "decode", "raw", "1"], 1, files, a.cpu):
            raw[r[3]] = int(r[8])

    table_merged = set()
    ns = collections.defaultdict(list)   # (cfg, file) -> [ns]
    size = {}                            # (cfg, file) -> encoded bytes
    rows = []
    for rd in range(a.rounds):
        order = cfgs[rd % len(cfgs):] + cfgs[:rd % len(cfgs)]
        la, _ = load_ok(a.max_load)
        print(f"# round {rd+1}/{a.rounds} load {la:.2f}", file=sys.stderr)
        for c in order:
            if a.mode == "inflate":   # tools report the original file; key results by stream instead
                res = [r[:3] + [f] + r[4:] for f in files for r in run(table[c], a.reps, [f], a.cpu)]
            else:
                res = run(table[c], a.reps, files, a.cpu)
            for r in res:
                ns[(c, r[3])].append(int(r[7]))
                size[(c, r[3])] = int(r[8])
                rows.append([c] + r)
    if a.out:
        with open(a.out, "w", newline="") as fh:
            csv.writer(fh).writerows(rows)
    for m in filter(None, a.merge.split(",")):
        for r in csv.reader(open(m)):
            c = r[0]
            if c in cfgs and c not in table_merged:
                continue
            if c not in cfgs:
                cfgs.append(c); table_merged.add(c)
            if r[4] in raw:
                ns[(c, r[4])].append(int(r[8])); size[(c, r[4])] = int(r[9])
    if a.order:
        cfgs = [c for c in a.order.split(",") if c in cfgs]

    groups = sorted({group_of(f, a.mode) for f in files})
    print(f"\n{a.mode}: MB/s of raw bytes (sum raw / sum per-file median)" +
          (", ratio = raw/encoded" if a.mode != "decode" else ""))
    hdr = "| group | " + " | ".join(cfgs) + " |"
    print(hdr); print("|" + "---|" * (len(cfgs) + 1))
    for g in groups + ["ALL"]:
        fs = [f for f in files if g == "ALL" or group_of(f, a.mode) == g]
        cells = []
        for c in cfgs:
            t = sum(st.median(ns[(c, f)]) for f in fs)
            rb = sum(raw[f] for f in fs)
            cell = f"{rb / t * 1e3:.0f}"
            if a.mode in ("encode", "deflate"):
                cell += f" @ {rb / sum(size[(c, f)] for f in fs):.3f}"
            cells.append(cell)
        print(f"| {g} | " + " | ".join(cells) + " |")

if __name__ == "__main__":
    main()
