# load average 1.20 at start (2026-10-10 21:03 CST), cpu 3
# round 1/3 load 1.20
# round 2/3 load 1.17
# round 3/3 load 1.15

encode: MB/s of raw bytes (sum raw / sum per-file median), ratio = raw/encoded
| group | pngz-high | png-high | qpng=best@2.9.1024.258 | qpng=best@2.9.1536.258 | qpng=best@2.9.2048.258 | qpng=best@2.9.1024.128 | qpng=best@2.8.2048.258 | qpng=best@2.8.4096.258 |
|---|---|---|---|---|---|---|---|---|
| graphics/ga8 | 15 @ 20.689 | 28 @ 19.145 | 30 @ 20.553 | 26 @ 20.831 | 22 @ 21.051 | 36 @ 20.521 | 22 @ 21.323 | 15 @ 21.668 |
| graphics/rgba8 | 21 @ 27.964 | 40 @ 25.895 | 43 @ 28.031 | 34 @ 28.317 | 30 @ 28.526 | 52 @ 27.834 | 32 @ 27.816 | 20 @ 28.383 |
| ALL | 18 @ 25.030 | 35 @ 23.172 | 38 @ 24.999 | 30 @ 25.288 | 27 @ 25.507 | 45 @ 24.879 | 28 @ 25.253 | 18 @ 25.725 |
