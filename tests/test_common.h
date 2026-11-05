/* Shared test helpers (not part of libqpng). */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

unsigned char *slurp(const char *path, size_t *n);
uint64_t fnv1a(const unsigned char *d, size_t n);
const unsigned char *find_chunk(const unsigned char *b, size_t n, const char *type, int *len);

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#endif
