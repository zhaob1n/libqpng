/*
 * pngfast - fast PNG decoder/encoder (C11, x86-64 with runtime SIMD dispatch).
 *
 * Decoding: every valid PNG color type / bit depth, interlaced or not, three output layouts (raw, png-crate EXPAND,
 * image-rs). Encoding: every color type / bit depth, presets tuned against the Rust png crate.
 * Deflate: a vendored, modified libdeflate (third_party/libdeflate, symbols prefixed pf_), see README.
 *
 * Thread safety: all functions are reentrant; no global mutable state except one-time CPU feature detection.
 * Memory: buffers returned to the caller are allocated with the allocator in the options (default malloc/free) and
 * must be released with pf_image_free() / pf_buffer_free().
 */
#ifndef PNGFAST_H
#define PNGFAST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PNGFAST_VERSION_MAJOR 1
#define PNGFAST_VERSION_MINOR 0
#define PNGFAST_VERSION_PATCH 0
#define PNGFAST_VERSION_STRING "1.0.0"

#if defined(_WIN32) && defined(PNGFAST_SHARED)
#  define PF_API __declspec(dllexport)
#elif defined(__GNUC__)
#  define PF_API __attribute__((visibility("default")))
#else
#  define PF_API
#endif

/* ---- status codes ------------------------------------------------------------------------------------------- */
typedef enum {
    PF_OK = 0,
    PF_ERR_ARGUMENT = -1,     /* invalid argument (NULL pointer, bad enum, inconsistent sizes) */
    PF_ERR_SIGNATURE = -2,    /* not a PNG */
    PF_ERR_HEADER = -3,       /* invalid IHDR (dimensions, color type / depth combination, methods) */
    PF_ERR_CHUNK = -4,        /* malformed chunk layout / order, unknown critical chunk, bad PLTE / tRNS */
    PF_ERR_CRC = -5,          /* chunk CRC mismatch */
    PF_ERR_ZLIB = -6,         /* invalid zlib header */
    PF_ERR_DEFLATE = -7,      /* corrupt / truncated deflate stream or wrong decompressed size */
    PF_ERR_FILTER = -8,       /* invalid scanline filter type */
    PF_ERR_ADLER = -9,        /* Adler-32 mismatch (only with verify_adler) */
    PF_ERR_NOMEM = -10,       /* allocation failed */
    PF_ERR_LIMITS = -11,      /* image exceeds the configured limits */
    PF_ERR_UNSUPPORTED = -12, /* valid PNG feature not supported */
    PF_ERR_ENCODE = -13       /* internal encoder failure (should not happen) */
} pf_status;

PF_API const char *pf_status_string(pf_status status);

/* ---- common types ----------------------------------------------------------------------------------------------- */
typedef enum {
    PF_COLOR_GRAY = 0,
    PF_COLOR_RGB = 2,
    PF_COLOR_PALETTE = 3,
    PF_COLOR_GRAY_ALPHA = 4,
    PF_COLOR_RGBA = 6
} pf_color;

/* Optional allocator hooks for the large buffers (pixels, filtered data, compressed output). */
typedef struct {
    void *(*alloc)(void *user, size_t size);   /* must return memory aligned for any type, or NULL */
    void (*free)(void *user, void *ptr);
    void *user;
} pf_allocator;

/* Header information (pf_read_info). */
typedef struct {
    uint32_t width, height;
    uint8_t bit_depth;        /* 1, 2, 4, 8, 16 */
    uint8_t color_type;       /* pf_color */
    uint8_t interlaced;       /* 0 or 1 (Adam7) */
    uint8_t channels;         /* samples per pixel in the file (palette: 1) */
    uint16_t palette_entries; /* 0 if no PLTE */
    uint16_t trns_bytes;      /* length of tRNS, 0 if absent */
} pf_info;

/* ---- decoding ----------------------------------------------------------------------------------------------- */
typedef enum {
    /* Samples exactly as stored: packed MSB-first for depth < 8, 16-bit big-endian, palette indices.
     * == png crate Transformations::IDENTITY == libpng without transforms. */
    PF_OUTPUT_RAW = 0,
    /* png crate Transformations::EXPAND: palette -> RGB8/RGBA8, gray < 8 bit -> 8 bit (scaled), tRNS -> alpha.
     * 16-bit samples stay big-endian. */
    PF_OUTPUT_EXPAND = 1,
    /* image-rs PngDecoder::read_image: EXPAND + 16-bit samples in native (little) endian. */
    PF_OUTPUT_IMAGERS = 2
} pf_output;

typedef struct {
    pf_output output;           /* default PF_OUTPUT_RAW */
    int verify_crc;             /* default 1 */
    int verify_adler;           /* default 0 (like the png crate) */
    uint32_t max_width;         /* 0 = default (1 << 24) */
    uint32_t max_height;        /* 0 = default (1 << 24) */
    uint64_t max_output_bytes;  /* decompression-bomb limit for the output image; 0 = default (1 GiB) */
    const pf_allocator *allocator; /* NULL = malloc/free */
} pf_decode_options;

typedef struct {
    uint8_t *pixels;          /* height rows of `stride` bytes, rows tightly packed */
    size_t size;              /* height * stride */
    size_t stride;            /* bytes per row */
    uint32_t width, height;
    uint8_t channels;         /* of the OUTPUT (after expansion) */
    uint8_t bit_depth;        /* of the OUTPUT */
    uint8_t color_type;       /* of the OUTPUT (pf_color) */
    pf_allocator allocator;   /* used to free `pixels` */
} pf_image;

/* Fill *opts with the defaults. */
PF_API void pf_decode_options_init(pf_decode_options *opts);
/* Parse the signature + IHDR (+ PLTE/tRNS presence). Does not validate CRCs beyond IHDR. */
PF_API pf_status pf_read_info(const void *png, size_t png_size, pf_info *info);
/* Decode a whole PNG held in memory. opts may be NULL (defaults). On error *img is zeroed. */
PF_API pf_status pf_decode(const void *png, size_t png_size, const pf_decode_options *opts, pf_image *img);
PF_API void pf_image_free(pf_image *img);

/* ---- encoding ----------------------------------------------------------------------------------------------- */
typedef enum {
    PF_PRESET_FASTEST = 0,   /* Up filter + run-only png-fast deflate: maximum speed */
    PF_PRESET_FAST = 1,      /* adaptive filter + png-fast deflate (fdeflate-style parser, Huffman-optimal blocks) */
    PF_PRESET_DEFAULT = 2,   /* content-adaptive: png-fast for photos (+ level 6 for palette/16-bit),
                                filter trial + libdeflate level 6/7 for graphics */
    PF_PRESET_BEST = 3       /* libdeflate level 10 for photos, filter trial + level 9 for graphics */
} pf_preset;

typedef enum {
    PF_FILTER_AUTO = -1,     /* preset decides */
    PF_FILTER_NONE = 0, PF_FILTER_SUB = 1, PF_FILTER_UP = 2, PF_FILTER_AVG = 3, PF_FILTER_PAETH = 4,
    PF_FILTER_ADAPTIVE = 5   /* per row: minimum sum of |signed residual| over the 5 filters (libpng heuristic) */
} pf_filter;

/* deflate_level values beyond libdeflate's 1..12: the png-fast compressor of the vendored libdeflate
 * (fdeflate-style: offset-1 runs, >= 8-byte hashed matches, literal skip-ahead, exact block-type choice). */
#define PF_DEFLATE_PNGFAST      100  /* default png-fast */
#define PF_DEFLATE_PNGFAST_RLE  101  /* runs only (no hash probes): fastest */
#define PF_DEFLATE_PNGFAST_HIGH 102  /* denser probing (skip shift 9) */

typedef struct {
    pf_preset preset;          /* default PF_PRESET_DEFAULT */
    int filter;                /* pf_filter, default PF_FILTER_AUTO */
    int deflate_level;         /* 0 = preset decides; 1..12 = libdeflate level; PF_DEFLATE_* below */
    const uint8_t *palette;    /* PLTE: palette_entries * 3 bytes (required for PF_COLOR_PALETTE) */
    int palette_entries;
    const uint8_t *trns;       /* tRNS chunk payload (optional) */
    int trns_bytes;
    const pf_allocator *allocator; /* NULL = malloc/free */
} pf_encode_options;

PF_API void pf_encode_options_init(pf_encode_options *opts);
/* Encode width x height pixels. `pixels` rows use the RAW layout above (16-bit big-endian, depth < 8 packed MSB
 * first); `stride` = bytes between rows (0 = tightly packed). The PNG is returned in *out (free with
 * pf_buffer_free(*out, opts->allocator)). */
PF_API pf_status pf_encode(const void *pixels, uint32_t width, uint32_t height, size_t stride, pf_color color,
                           int bit_depth, const pf_encode_options *opts, uint8_t **out, size_t *out_size);
PF_API void pf_buffer_free(void *buf, const pf_allocator *allocator);

#ifdef __cplusplus
}
#endif
#endif /* PNGFAST_H */
