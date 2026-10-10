# load average 1.42 at start (2026-10-10 21:50 CST), cpu 3
# round 1/3 load 1.42
# round 2/3 load 1.38
# round 3/3 load 1.29

inflate: MB/s of raw bytes (sum raw / sum per-file median), ratio = raw/encoded
| group | libdeflate | pf-pairs | zlib-ng | fdeflate |
|---|---|---|---|---|
| stream from fdeflate-1 | 383 | 664 | 367 | 517 |
| stream from fdeflate-ultrafast | 391 | 640 | 379 | 515 |
| stream from libdeflate-pf | 387 | 643 | 378 | 573 |
| stream from orig | 389 | 553 | 362 | 459 |
| stream from zng6 | 383 | 426 | 334 | 331 |
| ALL | 387 | 570 | 363 | 462 |
