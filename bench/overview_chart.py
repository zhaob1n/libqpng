import os, re, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager as fm
import numpy as np
for f in fm.findSystemFonts():
    if "NotoSansCJK" in f or "NotoSerifCJK-Regular" in f: fm.fontManager.addfont(f)
plt.rcParams["font.family"] = ["Noto Sans CJK SC", "Noto Serif CJK SC", "DejaVu Sans"]
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results", "phase4") + "/"
def table(path):
    rows = [l for l in open(path) if l.startswith("|")]
    hdr = [c.strip() for c in rows[0].strip("|\n").split("|")]
    out = {}
    for l in rows[2:]:
        c = [x.strip() for x in l.strip("|\n").split("|")]
        out[c[0]] = dict(zip(hdr[1:], c[1:]))
    return out
E = table(D + "encode_matrix.md"); Dd = table(D + "decode.md")
def sr(s): a, b = s.split("@"); return float(a), float(b)
groups = [g for g in E if g != "ALL"]
names = {"graphics":"截图","photo":"照片"}
lab = [names[g.split("/")[0]] + " " + g.split("/")[1] for g in groups]
pairs = [("Fastest", ["png-fastest"], "pf-fastest"), ("Fast", ["png-fast"], "pf-fast"),
         ("Balanced", ["png-balanced","pngz-balanced"], "pf-default"), ("High", ["png-high","pngz-high"], "pf-best")]
fig = plt.figure(figsize=(17, 10))
gs = fig.add_gridspec(2, 3, width_ratios=[1.25, 1, 1], height_ratios=[1, 1], wspace=0.35, hspace=0.35)
# heatmap
ax = fig.add_subplot(gs[:, 0])
M = np.zeros((len(groups), 4)); T = [[""]*4 for _ in groups]
for i, g in enumerate(groups):
    for j, (n, rs, pf) in enumerate(pairs):
        ps, pr = sr(E[g][pf])
        cands = [sr(E[g][r]) for r in rs]
        bs = max(c[0] for c in cands); br = max(c[1] for c in cands)
        sp = ps / bs; sz = (pr / br - 1) * 100
        win = (sp >= 0.97) + (sz >= -0.5)
        M[i, j] = win
        T[i][j] = f"×{sp:.2f}\n{'+' if sz>=0 else ''}{sz:.1f}%"
cmap = matplotlib.colors.ListedColormap(["#f4a6a6", "#ffe08a", "#9fd89f"])
ax.imshow(M, cmap=cmap, vmin=0, vmax=2, aspect="auto")
for i in range(len(groups)):
    for j in range(4):
        ax.text(j, i, T[i][j], ha="center", va="center", fontsize=9)
ax.set_xticks(range(4)); ax.set_xticklabels([f"{p[0]}\n→ {p[2].replace('pf-','').upper()}" for p in pairs])
ax.set_yticks(range(len(groups))); ax.set_yticklabels(lab)
ax.set_title("编码：qpng 对 png crate 各档的最强配置\n上：速度倍数　下：压缩比提升　绿=又快又小　黄=只赢一项", fontsize=11)
# scatter ALL
ax2 = fig.add_subplot(gs[0, 1:])
cols = {"png":"#e07b39", "pf":"#2f6db5"}
for n, rs, pf in pairs:
    for r in rs:
        s, q = sr(E["ALL"][r]); ax2.scatter(s, q, color=cols["png"], s=60, zorder=3)
        ax2.annotate(r.replace("pngz-","png+zlib-rs ").replace("png-","png "), (s, q), textcoords="offset points", xytext=(5, -12), fontsize=8, color=cols["png"])
    s, q = sr(E["ALL"][pf]); ax2.scatter(s, q, color=cols["pf"], s=80, marker="D", zorder=3)
    ax2.annotate(pf.replace("pf-","qpng ").upper().replace("QPNG","qpng"), (s, q), textcoords="offset points", xytext=(5, 5), fontsize=9, color=cols["pf"], weight="bold")
ax2.set_xscale("log"); ax2.set_xlabel("编码速度 MB/s（对数）"); ax2.set_ylabel("压缩比（越高越小）")
ax2.set_title("编码总体（全部图片）：右上更好", fontsize=11); ax2.grid(alpha=.3)
# decode bars
ax3 = fig.add_subplot(gs[1, 1:])
x = np.arange(len(groups)); w = 0.4
a = [float(Dd[g]["imagers"]) for g in groups]; b = [float(Dd[g]["pf-imagers"]) for g in groups]
ax3.bar(x - w/2, a, w, color=cols["png"], label="image-rs")
ax3.bar(x + w/2, b, w, color=cols["pf"], label="qpng（image-rs 兼容输出）")
for i in range(len(groups)): ax3.text(x[i] + w/2, b[i], f"×{b[i]/a[i]:.1f}", ha="center", va="bottom", fontsize=8)
ax3.set_yscale("log"); ax3.set_xticks(x); ax3.set_xticklabels(lab, rotation=45, ha="right", fontsize=8)
ax3.set_ylabel("解码 MB/s（对数）"); ax3.legend(fontsize=9)
al = float(Dd["ALL"]["imagers"]); bl = float(Dd["ALL"]["pf-imagers"])
ax3.set_title(f"解码：所有格式都更快（总体 {bl:.0f} 对 {al:.0f} MB/s）", fontsize=11)
fig.suptitle("qpng（af6c9f5）对 Rust png crate / image-rs　单核，2026-10-10", fontsize=14, weight="bold")
fig.savefig(D + "overview.png", dpi=130, bbox_inches="tight")
