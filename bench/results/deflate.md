# load average 1.23 at start (2026-10-09 22:00 CST), cpu 3
# round 1/3 load 1.23
# round 2/3 load 1.43
# round 3/3 load 1.29

deflate: MB/s of raw bytes (sum raw / sum per-file median), ratio = raw/encoded
| group | fdeflate-ultrafast | fdeflate-rle | fdeflate-1 | fdeflate-6 | qpng-rle | qpng-fast | qpng-6 |
|---|---|---|---|---|---|---|---|
| graphics | 883 @ 1.805 | 686 @ 2.721 | 294 @ 2.947 | 78 @ 3.561 | 868 @ 2.894 | 599 @ 3.125 | 115 @ 3.638 |
| photo | 506 @ 1.549 | 444 @ 1.665 | 427 @ 1.665 | 34 @ 1.640 | 729 @ 1.678 | 665 @ 1.678 | 57 @ 1.637 |
| ALL | 553 @ 1.594 | 478 @ 1.805 | 392 @ 1.824 | 39 @ 1.839 | 753 @ 1.832 | 651 @ 1.849 | 64 @ 1.840 |
