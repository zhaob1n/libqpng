# load average 1.29 at start (2026-10-09 22:02 CST), cpu 3
# round 1/3 load 1.29
# round 2/3 load 1.21
# round 3/3 load 1.24

inflate: MB/s of raw bytes (sum raw / sum per-file median), ratio = raw/encoded
| group | fdeflate | libdeflate | pf-pairs |
|---|---|---|---|
| stream from fdeflate-1 | 545 | 363 | 609 |
| stream from fdeflate-ultrafast | 517 | 363 | 564 |
| stream from libdeflate-pf | 535 | 368 | 588 |
| stream from orig | 436 | 356 | 523 |
| stream from zng6 | 329 | 341 | 432 |
| ALL | 456 | 358 | 535 |
