#!/usr/bin/env python3
"""Generate the format corpus used by bench/run.py.

Reads the six source images in bench/corpus/src/ (four photos plus screenshot
and graphics) and writes 20 color-type/depth variants of each to
bench/corpus/formats/, plus Adam7 copies of kodim03 and screenshot (160 PNGs).

Encoder mimics libpng defaults: zlib level 6; filter NONE for palette and <8-bit,
otherwise libpng's adaptive heuristic (per row, the filter with the minimum sum
of |signed residual|). Adam7 variants filter each pass separately.

16-bit photo noise uses numpy Generator seed 7 (one stream for the whole run, so generate all six sources in a single invocation). Palette and low-bit photo
variants use Pillow's median-cut / Floyd-Steinberg; recorded against
Pillow 13.0.0 and numpy 2.5.4.

Source images (see bench/corpus/src/SHA256SUMS):
  kodim01/03/05 — Kodak True Color suite (768x512)
  cathedral     — imagecompression.info RGB 8-bit test set (2000x3008)
  screenshot    — synthetic UI, 1280x800 RGB
  graphics      — synthetic RGBA, 800x600
"""
import numpy as np, struct, zlib, os, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.environ.get("QPNG_CORPUS_SRC", os.path.join(ROOT, "bench", "corpus", "src"))
O = os.environ.get("OUT", os.path.join(ROOT, "bench", "corpus", "formats"))
os.makedirs(O, exist_ok=True)
rng = np.random.default_rng(7)
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
def pack(a, depth):            # a: h x (w*ch) uint array -> h x stride bytes (big-endian 16, MSB-first packing <8)
    if depth == 16: return a.astype(">u2").view(np.uint8).reshape(a.shape[0], -1)
    if depth == 8: return a.astype(np.uint8)
    per = 8 // depth; h, n = a.shape; pad = (-n) % per
    a = np.concatenate([a, np.zeros((h, pad), a.dtype)], 1).reshape(h, -1, per).astype(np.uint16)
    out = np.zeros(a.shape[:2], np.uint16)
    for i in range(per): out |= a[:, :, i] << (8 - depth * (i + 1))
    return out.astype(np.uint8)
def filt(b, bpp, adaptive):
    h, n = b.shape; x = b.astype(np.int16)
    if not adaptive: return np.concatenate([np.zeros((h, 1), np.uint8), b], 1)
    a = np.zeros_like(x); a[:, bpp:] = x[:, :-bpp]
    u = np.zeros_like(x); u[1:] = x[:-1]
    c = np.zeros_like(x); c[1:, bpp:] = x[:-1, :-bpp]
    p = a + u - c; pa, pb, pc = abs(p - a), abs(p - u), abs(p - c)
    pr = np.where((pa <= pb) & (pa <= pc), a, np.where(pb <= pc, u, c))
    cand = np.stack([x, x - a, x - u, x - ((a + u) >> 1), x - pr]) & 0xFF        # 5 x h x n
    s = cand.astype(np.int8).astype(np.int32); cost = np.abs(s).sum(2)           # libpng: sum of |signed byte|
    cost[0] = b.astype(np.int32).sum(1) * 0 + np.abs(x.astype(np.int8).astype(np.int32)).sum(1)
    best = cost.argmin(0)
    return np.concatenate([best[:, None].astype(np.uint8), cand[best, np.arange(h)].astype(np.uint8)], 1)
A7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
def write_png(path, arr, ct, depth, interlace=0, plte=None, trns=None):
    h, w = arr.shape[:2]; a = arr.reshape(h, w, -1); ch = a.shape[2]
    bpp = max(1, ch * depth // 8); adaptive = ct != 3 and depth >= 8
    subs = [a] if not interlace else [a[y0::dy, x0::dx] for x0, y0, dx, dy in A7]
    raw = b"".join(filt(pack(s.reshape(s.shape[0], -1), depth), bpp, adaptive).tobytes() for s in subs if s.size)
    d = chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, interlace))
    if plte is not None: d += chunk(b"PLTE", bytes(plte))
    if trns is not None: d += chunk(b"tRNS", bytes(trns))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + d + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
def load(n):
    names = {
        "screenshot": ("screenshot.png", "screenshot_rgb.png", "screenshot_rgb.ppm"),
        "graphics": ("graphics.png", "graphics_rgba.png"),
    }.get(n, (f"{n}.png", f"{n}.ppm"))
    for name in names:
        p = os.path.join(SRC, name)
        if os.path.exists(p):
            im = Image.open(p)
            return np.asarray(im.convert("RGBA" if n == "graphics" or im.mode == "RGBA" else "RGB"))
    raise FileNotFoundError(
        f"missing source image for {n!r} in {SRC}; expected {names[0]} "
        f"(see bench/corpus/src/SHA256SUMS)"
    )
def alpha_for(rgb):            # photos get a soft radial vignette alpha + a hard-cut corner (realistic cutout-ish)
    h, w = rgb.shape[:2]; yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot((xx - w / 2) / (w / 2), (yy - h / 2) / (h / 2))
    al = np.clip((1.15 - r) * 400, 0, 255); al[: h // 6, : w // 6] = 0; return al.astype(np.uint8)
def to16(x, noisy):            # photos: 8-bit value * 257 plus noise in the low byte (like a 16-bit capture); graphics: exact *257
    y = x.astype(np.uint32) * 257
    if noisy: y = np.clip(y.astype(np.int64) + rng.integers(-128, 128, x.shape), 0, 65535)
    return y.astype(np.uint16)
DEFAULT_NAMES = ["kodim01", "kodim03", "kodim05", "cathedral", "screenshot", "graphics"]
NAMES = sys.argv[1:] or DEFAULT_NAMES
if sys.argv[1:] and NAMES != DEFAULT_NAMES:
    print(
        "warning: 16-bit photo noise uses one RNG stream (seed 7) across the whole run;\n"
        "generating a subset will not match the published corpus.\n"
        "Run `python3 bench/gen_corpus.py` with no arguments to reproduce it.",
        file=sys.stderr,
    )
print(f"sources: {SRC}\noutput:  {O}", flush=True)
for n in NAMES:
    im = load(n); photo = n not in ("screenshot", "graphics")
    if im.shape[2] == 4: rgb, al = im[:, :, :3], im[:, :, 3]
    else: rgb, al = im, alpha_for(im)
    pim = Image.fromarray(rgb); gray = np.asarray(pim.convert("L"))
    out = {}
    out["g8"] = (gray, 0, 8, {}); out["g16"] = (to16(gray, photo), 0, 16, {})
    for d in (1, 2, 4):        # photos: Floyd-Steinberg dither to 2^d levels; graphics: plain quantise
        lv = (1 << d) - 1
        if photo:
            q = pim.convert("L").quantize(colors=lv + 1, palette=Image.new("P", (1, 1)), dither=Image.Dither.FLOYDSTEINBERG) if False else None
            g = gray.astype(np.float32).copy(); h, w = g.shape; res = np.zeros_like(gray)
            pal = Image.new("P", (1, 1)); pal.putpalette(sum([[round(i * 255 / lv)] * 3 for i in range(lv + 1)], []) + [0] * 3 * (256 - lv - 1))
            q = pim.convert("L").convert("RGB").quantize(colors=lv + 1, palette=pal, dither=Image.Dither.FLOYDSTEINBERG)
            v = np.asarray(q).astype(np.uint8)
        else: v = np.round(gray.astype(np.float32) * lv / 255).astype(np.uint8)
        out[f"g{d}"] = (np.minimum(v, lv), 0, d, {})
    vals, cnt = np.unique(gray, return_counts=True); tg = int(vals[cnt.argmax()])
    out["g8t"] = (gray, 0, 8, dict(trns=struct.pack(">H", tg)))
    g16 = to16(gray, False); out["g16t"] = (g16, 0, 16, dict(trns=struct.pack(">H", tg * 257)))
    out["ga8"] = (np.dstack([gray, al]), 4, 8, {}); out["ga16"] = (np.dstack([to16(gray, photo), to16(al, False)]), 4, 16, {})
    out["rgb8"] = (rgb, 2, 8, {}); out["rgb16"] = (to16(rgb, photo), 2, 16, {})
    flat = rgb.reshape(-1, 3); key = (flat[:, 0].astype(np.uint32) << 16) | (flat[:, 1].astype(np.uint32) << 8) | flat[:, 2]
    k, c = np.unique(key, return_counts=True); tk = int(k[c.argmax()]); tc = [(tk >> 16) & 255, (tk >> 8) & 255, tk & 255]
    out["rgb8t"] = (rgb, 2, 8, dict(trns=struct.pack(">HHH", *tc)))
    out["rgb16t"] = (to16(rgb, False), 2, 16, dict(trns=struct.pack(">HHH", *[v * 257 for v in tc])))
    out["rgba8"] = (np.dstack([rgb, al]), 6, 8, {}); out["rgba16"] = (np.dstack([to16(rgb, photo), to16(al, False)]), 6, 16, {})
    for d in (1, 2, 4, 8):
        q = pim.quantize(colors=1 << d, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG if photo else Image.Dither.NONE)
        idx = np.asarray(q); pl = q.getpalette()[: 3 * (int(idx.max()) + 1)]
        out[f"p{d}"] = (idx, 3, d, dict(plte=pl))
    rgba = Image.fromarray(np.dstack([rgb, al]), "RGBA"); q = rgba.quantize(256, method=Image.Quantize.FASTOCTREE)
    idx = np.asarray(q); npal = int(idx.max()) + 1; pa = q.getpalette(rawmode="RGBA")[: 4 * npal]
    pl = [pa[i] for i in range(len(pa)) if i % 4 != 3]; ta = [pa[i] for i in range(3, len(pa), 4)]
    while ta and ta[-1] == 255: ta.pop()
    out["p8t"] = (idx, 3, 8, dict(plte=pl, trns=bytes(ta)))
    for fmt, (arr, ct, d, kw) in out.items():
        write_png(f"{O}/{n}.{fmt}.png", arr, ct, d, **kw)
        if n in ("kodim03", "screenshot") or n.endswith("_384"): write_png(f"{O}/{n}.{fmt}.i.png", arr, ct, d, interlace=1, **kw)
    print(n, rgb.shape, "done", flush=True)
