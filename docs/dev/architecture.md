# Architecture

The code is authoritative; this page is the map. Language and ownership rules:
[modern-c.md](modern-c.md).

```mermaid
flowchart LR
  subgraph API["include/qpng.h"]
    D[qpng_decode] ; E[qpng_encode] ; SD[qpng_decoder_*] ; SE[qpng_encoder_*] ; A[qpng_apng_*] ; M[qpng_read_metadata]
  end
  D --> DEC[decode.c]
  SD --> ST[stream.c] --> DEC
  A --> AP[apng.c] --> DEC
  DEC --> UNF[unfilter.c] --> EXP[expand.c]
  E --> ENC[encode.c] --> FE[filter_enc.c]
  ENC --> DB[deflate_backend.c]
  ST --> DB
  M --> META[metadata.c]
  ENC --> META
  ST --> META
  AP --> META
```

| path | contents |
|---|---|
| `include/qpng.h` | public API |
| `src/internal.h`, `src/common.c` | shared PNG primitives (signature, IHDR, chunk writers, zlib header, predictors), checked sizes, `qpng__grow`, CPU dispatch, allocators, status strings |
| `src/decode.c` | chunks, limits, fused inflate → unfilter, Adam7 |
| `src/unfilter.c` | scalar / SSE2 / AVX2 unfilter |
| `src/expand.c` | output transforms |
| `src/filter_enc.c` | encoder filters + fused adaptive cost |
| `src/encode.c` | presets, content probe, policy |
| `src/deflate_backend.{c,h}` | libdeflate + zlib-ng |
| `src/metadata.{c,h}` | ancillary chunks |
| `src/stream.c` | streaming decoder / encoder |
| `src/apng.c` | APNG |
| `third_party/*` | vendored; not modernized |
| `tests/`, `fuzz/`, `bench/`, `tools/`, `scripts/` | see README |

Release builds use `-O2` on purpose: CMake's default `-O3` measured 20–30%
slower on the decoder's 2-row wavefront kernels.
