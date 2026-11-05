/* Shared test helpers (not part of libqpng). File I/O / chunk scan reuse tools/util.h. */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include "../tools/util.h"

uint64_t fnv1a(const uint8_t *d, size_t n);

#define slurp qpng_tool_slurp
#define find_chunk qpng_tool_find_chunk

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#endif
