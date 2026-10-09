# load average 1.23 at start (2026-10-09 22:31 CST), cpu 3
# round 1/3 load 1.23
# round 2/3 load 1.35
# round 3/3 load 1.25

inflate: MB/s of raw bytes (sum raw / sum per-file median), ratio = raw/encoded
| group | fdeflate | libdeflate | pf-pairs |
|---|---|---|---|
| stream from fdeflate-1 | 474 | 350 | 618 |
| stream from fdeflate-ultrafast | 470 | 357 | 528 |
| stream from libdeflate-pf | 501 | 370 | 558 |
| stream from orig | 403 | 361 | 477 |
| stream from zng6 | 299 | 362 | 391 |
| ALL | 415 | 360 | 502 |
