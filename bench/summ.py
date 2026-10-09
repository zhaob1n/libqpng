#!/usr/bin/env python3
"""summ.py < CSV : sum of per-file median ns and sum of output bytes (quick A/B helper)."""
import sys, collections, statistics as st
t = collections.defaultdict(list); b = {}
for l in sys.stdin:
    f = l.strip().split(",")
    if len(f) < 9: continue
    t[f[3]].append(int(f[7])); b[f[3]] = int(f[8])
print(f"{sum(st.median(v) for v in t.values())/1e6:.1f} ms  {sum(b.values())} bytes  ({len(t)} files)")
