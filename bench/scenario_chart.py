#!/usr/bin/env python3
"""Typical-scenario chart: qpng vs Rust png crate / image-rs, from bench/results/scenarios/{decode,encode}.md
(produced by bench/run.py). Usage: python3 bench/scenario_chart.py  -> bench/results/scenarios/scenarios.png"""
import os, re, matplotlib, matplotlib.ticker
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results", "scenarios") + "/"

def table(path):
    rows = [l for l in open(path) if l.startswith("|")]
    hdr = [c.strip() for c in rows[0].strip("|\n").split("|")]
    return {r.strip("|\n").split("|")[0].strip(): dict(zip(hdr[1:], [c.strip() for c in r.strip("|\n").split("|")[1:]]))
            for r in rows[2:]}

def sr(cell):
    s, r = (cell.split("@") + ["0"])[:2]
    return float(s), float(r)

Dd, E = table(D + "decode.md"), table(D + "encode.md")
SC = [("photo/rgb8", "Photo\nRGB8"), ("graphics/rgba8", "Screenshot/UI\nRGBA8"), ("photo/g8", "Photo\ngray 8"),
      ("graphics/p8", "Screenshot\npalette 8"), ("photo/16bit", "Photo\n16-bit"), ("ALL", "All 160\nfiles")]
QC, PC, ZC, IC = "#1f5fbf", "#e07b39", "#b8860b", "#9a9a9a"
LEV = [("Fastest", "png-fastest", None, "qpng-fastest", "FASTEST"), ("Fast", "png-fast", None, "qpng-fast", "FAST"),
       ("Balanced", "png-balanced", "pngz-balanced", "qpng-default", "DEFAULT"),
       ("High", "png-high", "pngz-high", "qpng-best", "BEST")]

fig = plt.figure(figsize=(17, 13))
gs = fig.add_gridspec(3, 4, height_ratios=[1.15, 1, 0.8], hspace=0.55, wspace=0.18)
x = np.arange(len(SC))

ax = fig.add_subplot(gs[0, :])
ser = [("image-rs", "imagers", IC), ("png crate (raw)", "png-raw", PC),
       ("qpng (image-rs layout)", "qpng-imagers", "#6f9fe0"), ("qpng (raw)", "qpng-raw", QC)]
w = 0.2
for i, (lab, k, c) in enumerate(ser):
    v = [float(Dd[g][k]) for g, _ in SC]
    b = ax.bar(x + (i - 1.5) * w, v, w, color=c, label=lab)
    ax.bar_label(b, fmt="%.0f", fontsize=7.5, padding=1)
for j, (g, _) in enumerate(SC):
    sp = float(Dd[g]["qpng-imagers"]) / float(Dd[g]["imagers"])
    ax.text(j, max(float(Dd[g][k]) for _, k, _ in ser) + 450, f"×{sp:.2f} vs image-rs", ha="center", fontsize=9, color=QC, weight="bold")
ax.set_xticks(x); ax.set_xticklabels([s for _, s in SC]); ax.set_ylabel("decode MB/s (raw bytes)")
ax.set_ylim(0, ax.get_ylim()[1] * 1.12)
ax.set_title("Decode throughput, single core (higher is better)", weight="bold")
ax.legend(ncol=4, fontsize=9, loc="upper left", bbox_to_anchor=(0, -0.22)); ax.grid(axis="y", alpha=0.3)

for li, (L, p, z, q, qn) in enumerate(LEV):
    ser = [(f"png {L}", p, PC)] + ([(f"png (zlib-rs) {L}", z, ZC)] if z else []) + [(f"qpng {qn}", q, QC)]
    w = 0.8 / len(ser)
    a1 = fig.add_subplot(gs[1, li]); a2 = fig.add_subplot(gs[2, li])
    for i, (lab, k, c) in enumerate(ser):
        off = (i - (len(ser) - 1) / 2) * w
        sp = [sr(E[g][k])[0] for g, _ in SC]; ra = [sr(E[g][k])[1] for g, _ in SC]
        b1 = a1.bar(x + off, sp, w, color=c, label=lab)
        rel = [100 * sr(E[g][p])[1] / r for g, r in zip([g for g, _ in SC], ra)]   # size vs png crate at this level
        b2 = a2.bar(x + off, rel, w, color=c, label=lab)
        if k == q:
            a2.bar_label(b2, fmt="%.0f%%", fontsize=7, padding=1, color=QC, weight="bold")
            a1.bar_label(b1, fmt="%.0f", fontsize=7, padding=1, color=QC, weight="bold")
    a1.set_yscale("log"); a1.set_ylim(3, 15000)
    a1.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
    a1.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter()); a1.set_title(f"Encode: qpng {qn} vs png {L}", weight="bold", fontsize=10)
    a2.set_title(f"PNG size vs png {L} = 100% (lower = better)", fontsize=8.5); a2.set_ylim(0, 125)
    a2.axhline(100, color=PC, lw=0.8, ls="--")
    for a in (a1, a2):
        a.set_xticks(x); a.set_xticklabels([s.replace("\n", " ").replace("Screenshot/UI", "UI").replace("Screenshot", "Scr.")
                                            .replace("Photo ", "").replace("All 160 files", "ALL") for _, s in SC],
                                           rotation=35, ha="right", fontsize=8)
        a.grid(axis="y", alpha=0.3)
    a1.legend(fontsize=7.5, loc="upper right", framealpha=0.9)
    if li == 0: a1.set_ylabel("encode MB/s (log)"); a2.set_ylabel("PNG size, % of png crate")
fig.suptitle("qpng vs Rust png crate / image-rs — typical scenarios (single core, fresh run "
             + open(D + "DATE").read().strip() + ")", fontsize=14, weight="bold", y=0.94)
fig.savefig(D + "scenarios.png", dpi=110, bbox_inches="tight")
print("wrote", D + "scenarios.png")
