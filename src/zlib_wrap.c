#include "zlib_wrap.h"

#include <stdlib.h>
#include <zlib.h>

int zlib_deflate(const uint8_t *src, size_t src_len, uint8_t **dst, size_t *dst_len)
{
	uLongf bound = compressBound((uLong)src_len);

	*dst = malloc(bound);
	if (!*dst) {
		*dst_len = 0;
		return -1;
	}

	if (compress2(*dst, &bound, src, (uLong)src_len, Z_BEST_COMPRESSION) != Z_OK) {
		free(*dst);
		*dst = NULL;
		*dst_len = 0;
		return -1;
	}

	*dst_len = (size_t)bound;
	return 0;
}

int zlib_inflate(const uint8_t *src, size_t src_len, uint8_t **dst, size_t *dst_len,
                 size_t hint_size)
{
	size_t out_size = hint_size > 0 ? hint_size : src_len * 2;
	int ret;

	if (out_size == 0)
		out_size = 1;

	*dst = malloc(out_size);
	if (!*dst) {
		*dst_len = 0;
		return -1;
	}

	for (;;) {
		uLongf current_len = (uLongf)out_size;

		ret = uncompress(*dst, &current_len, src, (uLong)src_len);
		if (ret != Z_BUF_ERROR) {
			*dst_len = (size_t)current_len;
			break;
		}

		out_size *= 2;
        // realloc 실패 시 원본 보존
		uint8_t *new_dst = realloc(*dst, out_size);
		if (!new_dst) {
			free(*dst);
			*dst = NULL;
			*dst_len = 0;
			return -1;
		}
		*dst = new_dst;
	}

	if (ret != Z_OK) {
		free(*dst);
		*dst = NULL;
		*dst_len = 0;
		return -1;
	}

	return 0;
}
