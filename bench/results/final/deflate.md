# load average 1.20 at start (2026-10-09 22:29 CST), cpu 3
# round 1/3 load 1.20
# round 2/3 load 1.14
# round 3/3 load 1.29

deflate: MB/s of raw bytes (sum raw / sum per-file median), ratio = raw/encoded
| group | fdeflate-ultrafast | fdeflate-rle | fdeflate-1 | fdeflate-6 | qpng-rle | qpng-fast | qpng-6 |
|---|---|---|---|---|---|---|---|
| graphics | 1041 @ 1.805 | 544 @ 2.721 | 281 @ 2.947 | 74 @ 3.561 | 796 @ 2.894 | 618 @ 3.125 | 87 @ 3.638 |
| photo | 460 @ 1.549 | 399 @ 1.665 | 399 @ 1.665 | 34 @ 1.640 | 672 @ 1.678 | 599 @ 1.678 | 55 @ 1.637 |
| ALL | 518 @ 1.594 | 422 @ 1.805 | 368 @ 1.824 | 38 @ 1.839 | 693 @ 1.832 | 603 @ 1.849 | 59 @ 1.840 |
