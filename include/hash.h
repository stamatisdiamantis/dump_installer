#ifndef HASH_H
#define HASH_H

#include <stdint.h>

static inline uint32_t fnv1a32(const char *s) {
    uint32_t h = 2166136261u;
    while (s && *s) {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

#endif
