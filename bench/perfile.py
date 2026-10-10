#!/usr/bin/env python3
"""Per-file median ns and size from a run.py --out CSV: perfile.py CSV [regex]"""
import csv, collections, re, statistics, sys, os
d = collections.defaultdict(list); sz = {}
for r in csv.reader(open(sys.argv[1])):
    if len(r) < 10 or r[0] == "config": continue
    cfg, f = r[0], os.path.basename(r[4]); d[(f, cfg)].append(int(r[8])); sz[(f, cfg)] = int(r[9])
cfgs = sorted({c for _, c in d}); files = sorted({f for f, _ in d})
pat = re.compile(sys.argv[2] if len(sys.argv) > 2 else "")
print("file," + ",".join(f"{c} us,{c} bytes" for c in cfgs))
for f in files:
    if pat.search(f):
        print(f + "," + ",".join(f"{statistics.median(d[(f,c)])/1e3:.0f},{sz[(f,c)]}" for c in cfgs))
