# load average 1.09 at start (2026-10-10 21:49 CST), cpu 3
# round 1/3 load 1.09
# round 2/3 load 1.06
# round 3/3 load 1.04

decode: MB/s of raw bytes (sum raw / sum per-file median)
| group | pf-imagers | pf-raw | imagers | png-raw |
|---|---|---|---|---|
| graphics/16bit | 1403 | 2519 | 972 | 1546 |
| graphics/g8 | 831 | 1151 | 291 | 1000 |
| graphics/ga8 | 961 | 948 | 618 | 603 |
| graphics/p8 | 1824 | 5248 | 1047 | 2867 |
| graphics/rgb8 | 1658 | 2371 | 693 | 1541 |
| graphics/rgba8 | 1877 | 1834 | 1065 | 1083 |
| graphics/sub8 | 560 | 5108 | 286 | 2680 |
| photo/16bit | 302 | 346 | 233 | 265 |
| photo/g8 | 270 | 300 | 126 | 193 |
| photo/ga8 | 419 | 433 | 251 | 253 |
| photo/p8 | 413 | 548 | 318 | 398 |
| photo/rgb8 | 310 | 341 | 190 | 225 |
| photo/rgba8 | 351 | 402 | 261 | 263 |
| photo/sub8 | 283 | 553 | 170 | 380 |
| ALL | 359 | 423 | 252 | 304 |
