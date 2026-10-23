/*
 * qpng - fast PNG decoder/encoder (C11, x86-64 with runtime SIMD dispatch).
 *
 * Decoding: every valid PNG color type / bit depth, interlaced or not, three output layouts (raw, png-crate EXPAND,
 * image-rs). Encoding: every color type / bit depth, presets tuned against the Rust png crate.
 * Deflate: a vendored, modified libdeflate (third_party/libdeflate, symbols prefixed qpng_), see README.
 *
 * Thread safety: all functions are reentrant; no global mutable state except one-time CPU feature detection.
 * Memory: buffers returned to the caller are allocated with the allocator in the options (default malloc/free) and
 * must be released with qpng_image_free() / qpng_buffer_free().
 */
#ifndef QPNG_H
#define QPNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QPNG_VERSION_MAJOR 1
#define QPNG_VERSION_MINOR 2
#define QPNG_VERSION_PATCH 0
#define QPNG_VERSION_STRING "1.2.0"

#if defined(_WIN32) && defined(QPNG_SHARED)
#  define QPNG_API __declspec(dllexport)
#elif defined(__GNUC__)
#  define QPNG_API __attribute__((visibility("default")))
#else
#  define QPNG_API
#endif

/* ---- status codes ------------------------------------------------------------------------------------------- */
typedef enum {
    QPNG_OK = 0,
    QPNG_ERR_ARGUMENT = -1,     /* invalid argument (NULL pointer, bad enum, inconsistent sizes) */
    QPNG_ERR_SIGNATURE = -2,    /* not a PNG */
    QPNG_ERR_HEADER = -3,       /* invalid IHDR (dimensions, color type / depth combination, methods) */
    QPNG_ERR_CHUNK = -4,        /* malformed chunk layout / order, unknown critical chunk, bad PLTE / tRNS */
    QPNG_ERR_CRC = -5,          /* chunk CRC mismatch */
    QPNG_ERR_ZLIB = -6,         /* invalid zlib header */
    QPNG_ERR_DEFLATE = -7,      /* corrupt / truncated deflate stream or wrong decompressed size */
    QPNG_ERR_FILTER = -8,       /* invalid scanline filter type */
    QPNG_ERR_ADLER = -9,        /* Adler-32 mismatch (only with verify_adler) */
    QPNG_ERR_NOMEM = -10,       /* allocation failed */
    QPNG_ERR_LIMITS = -11,      /* image exceeds the configured limits */
    QPNG_ERR_UNSUPPORTED = -12, /* valid PNG feature not supported */
    QPNG_ERR_ENCODE = -13,      /* internal encoder failure (should not happen) */
    QPNG_ERR_ABORTED = -14,     /* a user callback (rows, read, write) returned nonzero */
    QPNG_ERR_STATE = -15,       /* streaming call in the wrong state (e.g. finish before IEND, too many rows) */
    QPNG_ERR_APNG = -16,        /* invalid APNG animation structure */
    QPNG_MORE_DATA = 1          /* streaming decode: not an error, more input is needed */
} qpng_status;

QPNG_API const char *qpng_status_string(qpng_status status);

/* ---- common types ----------------------------------------------------------------------------------------------- */
typedef enum {
    QPNG_COLOR_GRAY = 0,
    QPNG_COLOR_RGB = 2,
    QPNG_COLOR_PALETTE = 3,
    QPNG_COLOR_GRAY_ALPHA = 4,
    QPNG_COLOR_RGBA = 6
} qpng_color;

/* Optional allocator hooks for the large buffers (pixels, filtered data, compressed output). */
typedef struct {
    void *(*alloc)(void *user, size_t size);   /* must return memory aligned for any type, or NULL */
    void (*free)(void *user, void *ptr);
    void *user;
} qpng_allocator;

/* Header information (qpng_read_info). */
typedef struct {
    uint32_t width, height;
    uint8_t bit_depth;        /* 1, 2, 4, 8, 16 */
    uint8_t color_type;       /* qpng_color */
    uint8_t interlaced;       /* 0 or 1 (Adam7) */
    uint8_t channels;         /* samples per pixel in the file (palette: 1) */
    uint16_t palette_entries; /* 0 if no PLTE */
    uint16_t trns_bytes;      /* length of tRNS, 0 if absent */
} qpng_info;

/* ---- decoding ----------------------------------------------------------------------------------------------- */
typedef enum {
    /* Samples exactly as stored: packed MSB-first for depth < 8, 16-bit big-endian, palette indices.
     * == png crate Transformations::IDENTITY == libpng without transforms. */
    QPNG_OUTPUT_RAW = 0,
    /* png crate Transformations::EXPAND: palette -> RGB8/RGBA8, gray < 8 bit -> 8 bit (scaled), tRNS -> alpha.
     * 16-bit samples stay big-endian. */
    QPNG_OUTPUT_EXPAND = 1,
    /* image-rs PngDecoder::read_image: EXPAND + 16-bit samples in native (little) endian. */
    QPNG_OUTPUT_IMAGERS = 2
} qpng_output;

typedef struct {
    qpng_output output;           /* default QPNG_OUTPUT_RAW */
    int verify_crc;             /* default 1 */
    int verify_adler;           /* default 0 (like the png crate) */
    uint32_t max_width;         /* 0 = default (1 << 24) */
    uint32_t max_height;        /* 0 = default (1 << 24) */
    uint64_t max_output_bytes;  /* decompression-bomb limit for the output image; 0 = default (1 GiB) */
    const qpng_allocator *allocator; /* NULL = malloc/free */
} qpng_decode_options;

typedef struct {
    uint8_t *pixels;          /* height rows of `stride` bytes, rows tightly packed */
    size_t size;              /* height * stride */
    size_t stride;            /* bytes per row */
    uint32_t width, height;
    uint8_t channels;         /* of the OUTPUT (after expansion) */
    uint8_t bit_depth;        /* of the OUTPUT */
    uint8_t color_type;       /* of the OUTPUT (qpng_color) */
    qpng_allocator allocator;   /* used to free `pixels` */
} qpng_image;

/* Fill *opts with the defaults. */
QPNG_API void qpng_decode_options_init(qpng_decode_options *opts);
/* Parse the signature + IHDR (+ PLTE/tRNS presence). Does not validate CRCs beyond IHDR. */
QPNG_API qpng_status qpng_read_info(const void *png, size_t png_size, qpng_info *info);
/* Decode a whole PNG held in memory. opts may be NULL (defaults). On error *img is zeroed. */
QPNG_API qpng_status qpng_decode(const void *png, size_t png_size, const qpng_decode_options *opts, qpng_image *img);
QPNG_API void qpng_image_free(qpng_image *img);

/* ---- metadata (ancillary chunks) ------------------------------------------------------------------------------
 * qpng_read_metadata() parses every ancillary chunk of a PNG held in memory into a qpng_metadata (one allocation tree
 * owned by the struct, free with qpng_metadata_free). qpng_encode_options.metadata writes the same struct back.
 * Values are reported as stored: qpng applies NO color management (gAMA, cHRM, iCCP, sRGB, cICP, mDCV, cLLI,
 * sBIT and bKGD are informational; convert yourself, e.g. with lcms2, if needed).
 * Compressed chunks (zTXt, compressed iTXt, iCCP) are inflated with per-chunk and total limits (decompression
 * bombs). Malformed, duplicate or over-limit ancillary chunks are skipped and counted in `skipped` (like browsers /
 * the png crate); an unknown critical chunk or a broken chunk layout is an error as in qpng_decode. */
typedef enum { QPNG_TEXT_TEXT = 0, QPNG_TEXT_ZTXT = 1, QPNG_TEXT_ITXT = 2 } qpng_text_kind;
typedef struct {
    qpng_text_kind kind;           /* chunk it came from / is written as */
    const char *keyword;         /* 1..79 bytes Latin-1, NUL-terminated */
    const char *text;            /* tEXt/zTXt: Latin-1, iTXt: UTF-8; NUL-terminated (decompressed) */
    size_t text_size;            /* bytes, without the terminator (encode: 0 = strlen(text)) */
    const char *language;        /* iTXt only (may be ""), NULL otherwise */
    const char *translated_keyword; /* iTXt only (UTF-8, may be ""), NULL otherwise */
    int compressed;              /* iTXt: compression flag (zTXt is always compressed, tEXt never) */
} qpng_text;

/* where an unknown chunk sits; the encoder writes it back at the same place. Decoding reports BEFORE_PLTE only for
 * chunks in front of an actual PLTE chunk (without PLTE every chunk ahead of the image data is BEFORE_IDAT). */
typedef enum { QPNG_CHUNK_BEFORE_PLTE = 0, QPNG_CHUNK_BEFORE_IDAT = 1, QPNG_CHUNK_AFTER_IDAT = 2 } qpng_chunk_location;
typedef struct {
    char type[5];                /* 4 ASCII letters + NUL; must be ancillary (lowercase first letter) */
    qpng_chunk_location location;
    const uint8_t *data;
    uint32_t size;
} qpng_chunk;

typedef struct {
    /* has_* = 1 if the chunk was present (decode) / should be written (encode) */
    int has_gama;  uint32_t gamma;            /* gAMA: gamma * 100000 */
    int has_chrm;  uint32_t chrm[8];          /* cHRM: white x,y, red x,y, green x,y, blue x,y (* 100000) */
    int has_srgb;  uint8_t srgb_intent;       /* sRGB: rendering intent 0..3 */
    int has_cicp;  uint8_t cicp[4];           /* cICP: primaries, transfer, matrix coefficients, full range flag */
    int has_mdcv;  uint16_t mdcv_primaries[6]; uint16_t mdcv_white[2]; uint32_t mdcv_max_lum, mdcv_min_lum; /* mDCV */
    int has_clli;  uint32_t clli_max_cll, clli_max_fall;   /* cLLI (* 10000 cd/m2) */
    int has_phys;  uint32_t phys_x, phys_y; uint8_t phys_unit;   /* pHYs: pixels per unit, unit 0 = unknown, 1 = metre */
    int has_time;  uint16_t time_year; uint8_t time_month, time_day, time_hour, time_minute, time_second;  /* tIME */
    int has_bkgd;  uint8_t bkgd[6]; uint8_t bkgd_size;     /* bKGD as stored (1: palette index, 2: gray, 6: RGB; BE) */
    int has_sbit;  uint8_t sbit[4]; uint8_t sbit_size;     /* sBIT as stored (1..4 bytes) */
    const char *iccp_name;       /* iCCP profile name (NULL if no iCCP) */
    const uint8_t *iccp;         /* decompressed ICC profile */
    size_t iccp_size;
    const uint8_t *exif;         /* eXIf payload (NULL if absent) */
    size_t exif_size;
    const qpng_text *texts;  size_t text_count;
    const qpng_chunk *chunks; size_t chunk_count;   /* unknown ancillary chunks (passthrough), in file order */
    uint32_t skipped;            /* decode: malformed / duplicate / over-limit ancillary chunks that were ignored */
    void *internal_;             /* decode: owned storage (do not touch); encode: ignored */
} qpng_metadata;

typedef struct {
    int verify_crc;                    /* default 1: a bad ancillary CRC -> chunk skipped (counted) */
    size_t max_chunk_decompressed;     /* per zTXt/iTXt/iCCP inflated size; 0 = default (16 MiB) */
    size_t max_total;                  /* all metadata (incl. decompressed); 0 = default (64 MiB) */
    uint32_t max_chunks;               /* text + unknown chunks kept; 0 = default (4096) */
    int keep_unknown;                  /* default 1: collect unknown ancillary chunks for passthrough */
    const qpng_allocator *allocator;     /* NULL = malloc/free */
} qpng_metadata_options;

QPNG_API void qpng_metadata_options_init(qpng_metadata_options *opts);
/* Parse all ancillary chunks (the image data is not decoded). opts may be NULL. On error *meta is zeroed. */
QPNG_API qpng_status qpng_read_metadata(const void *png, size_t png_size, const qpng_metadata_options *opts, qpng_metadata *meta);
QPNG_API void qpng_metadata_free(qpng_metadata *meta);

/* ---- encoding ----------------------------------------------------------------------------------------------- */
typedef enum {
    QPNG_PRESET_FASTEST = 0,   /* Up filter + run-only png-fast deflate: maximum speed */
    QPNG_PRESET_FAST = 1,      /* adaptive filter + png-fast deflate (fdeflate-style parser, Huffman-optimal blocks) */
    QPNG_PRESET_DEFAULT = 2,   /* content-adaptive: png-fast for photos (+ Paeth trial for palette, level 6 for 16-bit),
                                filter trial + zlib-ng level 6 for graphics (libdeflate L4-8 without zlib-ng) */
    QPNG_PRESET_BEST = 3       /* libdeflate level 8 for photos, filter trial + zlib-ng level 9 for graphics
                                (libdeflate L9/L10 without zlib-ng) */
} qpng_preset;

typedef enum {
    QPNG_FILTER_AUTO = -1,     /* preset decides */
    QPNG_FILTER_NONE = 0, QPNG_FILTER_SUB = 1, QPNG_FILTER_UP = 2, QPNG_FILTER_AVG = 3, QPNG_FILTER_PAETH = 4,
    QPNG_FILTER_ADAPTIVE = 5   /* per row: minimum sum of |signed residual| over the 5 filters (libpng heuristic) */
} qpng_filter;

/* deflate_level values beyond libdeflate's 1..12: the png-fast compressor of the vendored libdeflate
 * (fdeflate-style: offset-1 runs, >= 8-byte hashed matches, literal skip-ahead, exact block-type choice). */
#define QPNG_DEFLATE_QPNG      100  /* default png-fast */
#define QPNG_DEFLATE_QPNG_RLE  101  /* runs only (no hash probes): fastest */
#define QPNG_DEFLATE_QPNG_HIGH 102  /* denser probing (skip shift 9) */
#define QPNG_DEFLATE_PNGULTRA     103  /* single pass, runs only, sampled Huffman codes: fastest */

/* Deflate backend (qpng_encode_options.backend). AUTO picks per preset and content (see README "Backends").
 * LIBDEFLATE: the vendored, modified libdeflate only (qpng 1.0 behaviour). ZLIBNG: the optional vendored zlib-ng
 * (built with QPNG_WITH_ZLIBNG; qpng_backend_available() tells) for every stream the preset would otherwise give
 * to libdeflate's levels, png-fast / png-ultra included. */
typedef enum {
    QPNG_BACKEND_AUTO = 0,
    QPNG_BACKEND_LIBDEFLATE = 1,
    QPNG_BACKEND_ZLIBNG = 2
} qpng_backend;

/* zlib strategies for QPNG_BACKEND_ZLIBNG (same values as zlib's Z_*) */
typedef enum {
    QPNG_ZSTRATEGY_DEFAULT = 0, QPNG_ZSTRATEGY_FILTERED = 1, QPNG_ZSTRATEGY_HUFFMAN_ONLY = 2, QPNG_ZSTRATEGY_RLE = 3
} qpng_zstrategy;

/* 1 if the backend is compiled into this library (AUTO and LIBDEFLATE: always) */
QPNG_API int qpng_backend_available(qpng_backend backend);

typedef struct {
    qpng_preset preset;          /* default QPNG_PRESET_DEFAULT */
    int filter;                /* qpng_filter, default QPNG_FILTER_AUTO */
    int deflate_level;         /* 0 = preset decides; 1..12 = libdeflate level; QPNG_DEFLATE_* below */
    const uint8_t *palette;    /* PLTE: palette_entries * 3 bytes (required for QPNG_COLOR_PALETTE) */
    int palette_entries;
    const uint8_t *trns;       /* tRNS chunk payload (optional) */
    int trns_bytes;
    const qpng_allocator *allocator; /* NULL = malloc/free */
    /* advanced, with an explicit deflate_level 2..12: match finder search depth (1..65535) and nice match length
     * (3..258); 0 = the level's default */
    int search_depth;
    int nice_length;
    /* since 1.1. backend: qpng_backend (default AUTO). With QPNG_BACKEND_ZLIBNG, deflate_level is a zlib-ng level 1..9
     * (0 = the preset's level: FASTEST 1, FAST 2, DEFAULT 6, BEST 9; the filter probing of DEFAULT/BEST stays on)
     * and zlib_strategy a qpng_zstrategy. search_depth / nice_length map to zlib's max_chain / nice_length.
     * QPNG_ERR_UNSUPPORTED if the backend is not compiled in. */
    int backend;
    int zlib_strategy;
    /* since 1.2: ancillary chunks to write (NULL = none). zTXt / compressed iTXt texts and the ICC profile are
     * compressed here. Invalid entries (keywords, sizes for the color type, values out of range, critical or
     * reserved unknown chunk types) -> QPNG_ERR_ARGUMENT. Unknown chunks are written unchanged. */
    const qpng_metadata *metadata;
} qpng_encode_options;

QPNG_API void qpng_encode_options_init(qpng_encode_options *opts);
/* Encode width x height pixels. `pixels` rows use the RAW layout above (16-bit big-endian, depth < 8 packed MSB
 * first); `stride` = bytes between rows (0 = tightly packed). The PNG is returned in *out (free with
 * qpng_buffer_free(*out, opts->allocator)). */
QPNG_API qpng_status qpng_encode(const void *pixels, uint32_t width, uint32_t height, size_t stride, qpng_color color,
                           int bit_depth, const qpng_encode_options *opts, uint8_t **out, size_t *out_size);
QPNG_API void qpng_buffer_free(void *buf, const qpng_allocator *allocator);

/* ---- streaming ------------------------------------------------------------------------------------------------
 * Incremental decode: create a qpng_decoder, push bytes as they arrive (any split), query the header as soon as IHDR
 * is in, finish after IEND.
 *  - row mode (row_cb != NULL): every output row (qpng_decode_options.output layout, `row_bytes` bytes) is handed to
 *    row_cb in order. Non-interlaced images with the zlib-ng backend built in are decoded truly incrementally with
 *    O(row) memory (zlib-ng streaming inflate: ~35% slower inflate than the one-shot libdeflate path). Interlaced
 *    images, and every image in libdeflate-only builds, are buffered (compressed bytes) and decoded with the
 *    one-shot path at IEND, then emitted row by row (memory: file + image).
 *  - image mode (row_cb == NULL): compressed data is buffered and qpng_decoder_finish() returns the whole image via
 *    the fast one-shot path (same speed as qpng_decode plus one copy of the input).
 * Ancillary chunks are not interpreted (use qpng_read_metadata on the header bytes if needed); APNG data is ignored
 * as in qpng_decode. CRCs are checked incrementally (verify_crc). */
typedef struct qpng_decoder qpng_decoder;
/* return nonzero to abort (the push / finish then fails with QPNG_ERR_ABORTED) */
typedef int (*qpng_row_fn)(void *user, uint32_t y, const uint8_t *row, size_t row_bytes);
QPNG_API qpng_status qpng_decoder_new(const qpng_decode_options *opts, qpng_row_fn row_cb, void *user, qpng_decoder **dec);
/* returns QPNG_OK when the stream is complete (IEND seen), QPNG_MORE_DATA when more input is expected, or an error
 * (sticky: later calls return it again) */
QPNG_API qpng_status qpng_decoder_push(qpng_decoder *dec, const void *data, size_t size);
/* QPNG_OK once IHDR has been parsed (info filled), QPNG_MORE_DATA before */
QPNG_API qpng_status qpng_decoder_info(const qpng_decoder *dec, qpng_info *info);
/* after IEND: image mode returns the image in *img; row mode emits any rows still pending (img may be NULL) */
QPNG_API qpng_status qpng_decoder_finish(qpng_decoder *dec, qpng_image *img);
QPNG_API void qpng_decoder_free(qpng_decoder *dec);
/* convenience: pull input from read (returns bytes read, 0 = end of input, (size_t)-1 = error -> QPNG_ERR_ABORTED)
 * and decode with qpng_decoder_* (row_cb / img as above) */
typedef size_t (*qpng_read_fn)(void *user, uint8_t *buf, size_t size);
QPNG_API qpng_status qpng_decode_stream(qpng_read_fn read, void *read_user, const qpng_decode_options *opts, qpng_row_fn row_cb,
                                  void *row_user, qpng_image *img);

/* Incremental encode with bounded memory: rows are pushed in order (any batch size) and the PNG is written through
 * write() as it is produced (IDAT chunks of <= 256 KiB). Filtering per preset (FASTEST: Up, others: adaptive; no
 * whole-image probing), deflate: FASTEST/FAST png-ultra (as one-shot), DEFAULT/BEST zlib-ng level 6 / 9 when built
 * in, otherwise png-ultra with the adaptive filter. Explicit filter / deflate_level / backend work as in
 * qpng_encode where streaming is possible (libdeflate levels 1-12 and png-fast are not streamable -> QPNG_ERR_UNSUPPORTED).
 * opts->metadata is written. Memory: a few rows + compressor state (zlib-ng ~300 KiB). */
typedef struct qpng_encoder qpng_encoder;
typedef int (*qpng_write_fn)(void *user, const uint8_t *data, size_t size);   /* nonzero = abort */
QPNG_API qpng_status qpng_encoder_new(uint32_t width, uint32_t height, qpng_color color, int bit_depth,
                                const qpng_encode_options *opts, qpng_write_fn write, void *write_user, qpng_encoder **enc);
/* rows in qpng_encode's layout; stride 0 = packed. Pushing more than `height` rows -> QPNG_ERR_STATE. */
QPNG_API qpng_status qpng_encoder_push_rows(qpng_encoder *enc, const void *rows, size_t stride, uint32_t nrows);
/* all rows must have been pushed; writes the remaining data and IEND */
QPNG_API qpng_status qpng_encoder_finish(qpng_encoder *enc);
QPNG_API void qpng_encoder_free(qpng_encoder *enc);

/* ---- APNG -----------------------------------------------------------------------------------------------------
 * Decode: qpng_apng_open() parses and validates the animation structure (acTL / fcTL / fdAT sequence numbers, frame
 * count, regions) of a PNG in memory (the buffer must stay valid until qpng_apng_free). A file without acTL (or with
 * acTL after IDAT) opens as a still image with one frame. Invalid animation structure -> QPNG_ERR_APNG (browsers
 * then show the default image: use qpng_decode, which ignores APNG chunks).
 *  - qpng_apng_next_frame(): iterator over the composed frames: RGBA8 canvas (width*4 stride, owned by the decoder,
 *    valid until the next call) after applying the previous frame's dispose op and this frame's blend op
 *    (APNG spec / browser semantics: canvas starts transparent black, first frame DISPOSE_PREVIOUS acts as
 *    BACKGROUND, BLEND_OVER is 8-bit "source over" with rounding, 16-bit samples are rounded to 8 bits).
 *    qpng_apng_rewind() restarts (for loops). Returns QPNG_ERR_STATE after the last frame.
 *  - qpng_apng_raw_frame(): one frame's own pixels (its fcTL size) in any qpng_decode output layout, no composition.
 * When the default image (IDAT) is not part of the animation it is not a frame (default_image_is_frame = 0). */
typedef enum { QPNG_DISPOSE_NONE = 0, QPNG_DISPOSE_BACKGROUND = 1, QPNG_DISPOSE_PREVIOUS = 2 } qpng_dispose_op;
typedef enum { QPNG_BLEND_SOURCE = 0, QPNG_BLEND_OVER = 1 } qpng_blend_op;
typedef struct {
    uint32_t width, height, x_offset, y_offset;
    uint16_t delay_num, delay_den;   /* delay = num / den seconds (den 0 means 100) */
    uint8_t dispose_op, blend_op;
} qpng_frame_info;
typedef struct {
    qpng_info image;                   /* IHDR (canvas) */
    uint32_t num_frames;             /* 1 for a still image */
    uint32_t num_plays;              /* 0 = infinite */
    int animated;                    /* acTL present and valid */
    int default_image_is_frame;      /* IDAT is frame 0 */
} qpng_apng_info;
typedef struct qpng_apng qpng_apng;
QPNG_API qpng_status qpng_apng_open(const void *png, size_t size, const qpng_decode_options *opts, qpng_apng **apng);
QPNG_API void qpng_apng_get_info(const qpng_apng *apng, qpng_apng_info *info);
QPNG_API qpng_status qpng_apng_next_frame(qpng_apng *apng, const uint8_t **canvas_rgba8, qpng_frame_info *frame);
QPNG_API void qpng_apng_rewind(qpng_apng *apng);
/* opts NULL = the options given to qpng_apng_open */
QPNG_API qpng_status qpng_apng_raw_frame(qpng_apng *apng, uint32_t index, const qpng_decode_options *opts, qpng_image *img,
                                   qpng_frame_info *frame);
QPNG_API void qpng_apng_free(qpng_apng *apng);

/* Encode an APNG: every frame is pixels of its own fcTL size in qpng_encode's layout (same color type / depth /
 * palette for all), compressed with opts (preset, backend, ...); opts->metadata is written once. default_image
 * NULL: frame 0 is the default image (must cover the canvas at offset 0); otherwise default_image (canvas size,
 * default_stride) is a separate still image that is not part of the animation. */
typedef struct { const void *pixels; size_t stride; qpng_frame_info info; } qpng_apng_frame;
QPNG_API qpng_status qpng_encode_apng(uint32_t width, uint32_t height, qpng_color color, int bit_depth, const qpng_apng_frame *frames,
                                uint32_t frame_count, uint32_t num_plays, const void *default_image, size_t default_stride,
                                const qpng_encode_options *opts, uint8_t **out, size_t *out_size);

#ifdef __cplusplus
}
#endif
#endif /* QPNG_H */
