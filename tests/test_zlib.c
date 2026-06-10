#include "zlib_wrap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_int(const char *name, int got, int want)
{
	if (got == want)
		return;

	fprintf(stderr, "FAIL %s\n  got:  %d\n  want: %d\n", name, got, want);
	failures++;
}

static void expect_size(const char *name, size_t got, size_t want)
{
	if (got == want)
		return;

	fprintf(stderr, "FAIL %s\n  got:  %zu\n  want: %zu\n", name, got, want);
	failures++;
}

static void expect_bytes(const char *name, const uint8_t *got, const uint8_t *want, size_t len)
{
	if (memcmp(got, want, len) == 0)
		return;

	fprintf(stderr, "FAIL %s\n", name);
	failures++;
}

static void test_zlib_round_trip_text(void)
{
	const uint8_t input[] = "blob 62\0The quick brown fox jumps over the lazy dog\n"
	                        "The quick brown fox jumps over the lazy dog\n";
	uint8_t *compressed = NULL;
	uint8_t *inflated = NULL;
	size_t compressed_len = 0;
	size_t inflated_len = 0;

	expect_int("zlib_deflate compresses text",
	           zlib_deflate(input, sizeof(input) - 1, &compressed, &compressed_len), 0);
	if (compressed == NULL || compressed_len == 0) {
		fprintf(stderr, "FAIL zlib_deflate returns compressed bytes\n");
		failures++;
		goto done;
	}

	expect_int("zlib_inflate decompresses text",
	           zlib_inflate(compressed, compressed_len, &inflated, &inflated_len, 0), 0);
	expect_size("zlib_inflate reports original text length", inflated_len, sizeof(input) - 1);
	expect_bytes("zlib_inflate restores original text", inflated, input, sizeof(input) - 1);

done:
	free(compressed);
	free(inflated);
}

static void test_zlib_round_trip_binary(void)
{
	const uint8_t input[] = {0x00, 0xff, 0x01, 0x02, 0x00, 0x7f, 0x80, 0x81, 0x00, 0x42};
	uint8_t *compressed = NULL;
	uint8_t *inflated = NULL;
	size_t compressed_len = 0;
	size_t inflated_len = 0;

	expect_int("zlib_deflate compresses binary input",
	           zlib_deflate(input, sizeof(input), &compressed, &compressed_len), 0);
	expect_int(
	    "zlib_inflate decompresses binary input",
	    zlib_inflate(compressed, compressed_len, &inflated, &inflated_len, sizeof(input)), 0);
	expect_size("zlib_inflate reports original binary length", inflated_len, sizeof(input));
	expect_bytes("zlib_inflate restores binary input", inflated, input, sizeof(input));

	free(compressed);
	free(inflated);
}

static void test_zlib_inflate_grows_past_hint(void)
{
	uint8_t input[4096];
	uint8_t *compressed = NULL;
	uint8_t *inflated = NULL;
	size_t compressed_len = 0;
	size_t inflated_len = 0;

	for (size_t i = 0; i < sizeof(input); i++)
		input[i] = (uint8_t)('a' + (i % 26));

	expect_int("zlib_deflate compresses data larger than hint",
	           zlib_deflate(input, sizeof(input), &compressed, &compressed_len), 0);
	expect_int("zlib_inflate grows output buffer",
	           zlib_inflate(compressed, compressed_len, &inflated, &inflated_len, 8), 0);
	expect_size("zlib_inflate reports grown output length", inflated_len, sizeof(input));
	expect_bytes("zlib_inflate restores data larger than hint", inflated, input, sizeof(input));

	free(compressed);
	free(inflated);
}

static void test_zlib_inflate_rejects_invalid_data(void)
{
	const uint8_t input[] = {'n', 'o', 't', ' ', 'z', 'l', 'i', 'b'};
	uint8_t *inflated = NULL;
	size_t inflated_len = 0;

	expect_int("zlib_inflate rejects invalid data",
	           zlib_inflate(input, sizeof(input), &inflated, &inflated_len, 0), -1);
	if (inflated != NULL) {
		fprintf(stderr, "FAIL zlib_inflate clears output pointer on failure\n");
		failures++;
		free(inflated);
	}
}

int main(void)
{
	test_zlib_round_trip_text();
	test_zlib_round_trip_binary();
	test_zlib_inflate_grows_past_hint();
	test_zlib_inflate_rejects_invalid_data();

	if (failures != 0) {
		fprintf(stderr, "%d zlib test(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	puts("zlib tests passed");
	return EXIT_SUCCESS;
}
