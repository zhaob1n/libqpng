# load average 0.78 at start (2026-10-09 22:04 CST), cpu 3
# round 1/3 load 0.80
# round 2/3 load 0.86
# round 3/3 load 0.90

decode: MB/s of raw bytes (sum raw / sum per-file median)
| group | imagers | v3-imagers | qpng-imagers |
|---|---|---|---|
| graphics/16bit | 747 | 1176 | 1126 |
| graphics/g8 | 259 | 723 | 855 |
| graphics/ga8 | 524 | 782 | 810 |
| graphics/p8 | 438 | 1039 | 1122 |
| graphics/rgb8 | 538 | 1337 | 1350 |
| graphics/rgba8 | 737 | 1339 | 1349 |
| graphics/sub8 | 144 | 366 | 319 |
| photo/16bit | 208 | 267 | 248 |
| photo/g8 | 115 | 226 | 187 |
| photo/ga8 | 227 | 381 | 269 |
| photo/p8 | 247 | 363 | 344 |
| photo/rgb8 | 173 | 252 | 263 |
| photo/rgba8 | 229 | 309 | 324 |
| photo/sub8 | 128 | 237 | 238 |
| ALL | 233 | 333 | 314 |
