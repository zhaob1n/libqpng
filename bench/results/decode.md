# load average 1.62 at start (2026-10-09 21:59 CST), cpu 3
# round 1/3 load 1.57
# round 2/3 load 1.37
# round 3/3 load 1.25

decode: MB/s of raw bytes (sum raw / sum per-file median)
| group | imagers | png-raw | qpng-imagers | qpng-raw |
|---|---|---|---|---|
| graphics/16bit | 777 | 1157 | 1183 | 2034 |
| graphics/g8 | 266 | 935 | 901 | 1257 |
| graphics/ga8 | 513 | 545 | 835 | 811 |
| graphics/p8 | 432 | 1753 | 1140 | 2286 |
| graphics/rgb8 | 563 | 935 | 1353 | 1930 |
| graphics/rgba8 | 760 | 725 | 1429 | 1409 |
| graphics/sub8 | 174 | 399 | 363 | 392 |
| photo/16bit | 223 | 253 | 261 | 295 |
| photo/g8 | 123 | 182 | 196 | 190 |
| photo/ga8 | 243 | 238 | 277 | 275 |
| photo/p8 | 272 | 366 | 358 | 501 |
| photo/rgb8 | 182 | 208 | 294 | 305 |
| photo/rgba8 | 253 | 240 | 320 | 361 |
| photo/sub8 | 150 | 321 | 229 | 432 |
| ALL | 250 | 301 | 330 | 378 |
