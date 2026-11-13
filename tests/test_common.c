#include "test_common.h"

uint64_t fnv1a(size_t n, const uint8_t d[n]) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 0x100000001b3ull;
    return h;
}
