#ifndef ZLIB_WRAP_H
#define ZLIB_WRAP_H

#include <stddef.h>
#include <stdint.h>

int zlib_deflate(const uint8_t *src, size_t src_len, uint8_t **dst, size_t *dst_len);

int zlib_inflate(const uint8_t *src, size_t src_len, uint8_t **dst, size_t *dst_len,
                 size_t hint_size);

#endif
