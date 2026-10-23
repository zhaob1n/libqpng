# load average 0.20 at start (2026-10-09 22:35 CST), cpu 3
# round 1/3 load 0.20
# round 2/3 load 0.52
# round 3/3 load 1.22

decode: MB/s of raw bytes (sum raw / sum per-file median)
| group | imagers | png-raw | v3-imagers | qpng-imagers | qpng-raw |
|---|---|---|---|---|---|
| graphics/16bit | 778 | 1152 | 948 | 951 | 1279 |
| graphics/g8 | 264 | 947 | 694 | 733 | 1019 |
| graphics/ga8 | 544 | 514 | 762 | 807 | 839 |
| graphics/p8 | 442 | 1615 | 1126 | 1156 | 2668 |
| graphics/rgb8 | 554 | 971 | 1294 | 1393 | 1899 |
| graphics/rgba8 | 730 | 737 | 1343 | 1437 | 1396 |
| graphics/sub8 | 170 | 382 | 325 | 384 | 397 |
| ALL | 583 | 958 | 919 | 953 | 1223 |
