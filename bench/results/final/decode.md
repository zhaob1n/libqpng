# load average 1.40 at start (2026-10-09 22:28 CST), cpu 3
# round 1/3 load 1.37
# round 2/3 load 1.60
# round 3/3 load 1.33

decode: MB/s of raw bytes (sum raw / sum per-file median)
| group | imagers | png-raw | v3-imagers | pf-imagers | pf-raw |
|---|---|---|---|---|---|
| graphics/16bit | 720 | 1045 | 1210 | 938 | 1575 |
| graphics/g8 | 265 | 874 | 679 | 658 | 727 |
| graphics/ga8 | 532 | 453 | 820 | 690 | 545 |
| graphics/p8 | 377 | 1417 | 1116 | 745 | 2168 |
| graphics/rgb8 | 536 | 848 | 1375 | 1009 | 1431 |
| graphics/rgba8 | 661 | 685 | 1407 | 1119 | 1240 |
| graphics/sub8 | 148 | 350 | 353 | 235 | 286 |
| photo/16bit | 198 | 236 | 277 | 271 | 272 |
| photo/g8 | 108 | 179 | 241 | 251 | 222 |
| photo/ga8 | 221 | 230 | 383 | 353 | 277 |
| photo/p8 | 255 | 347 | 336 | 365 | 430 |
| photo/rgb8 | 176 | 203 | 287 | 282 | 293 |
| photo/rgba8 | 231 | 212 | 326 | 338 | 367 |
| photo/sub8 | 127 | 310 | 232 | 235 | 339 |
| ALL | 226 | 283 | 348 | 336 | 352 |
