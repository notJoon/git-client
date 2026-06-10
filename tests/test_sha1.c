#include "sha1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static int failures;

static void expect_string(const char *name, const char *got, const char *want)
{
	if (strcmp(got, want) == 0)
		return;

	fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", name, got, want);
	failures++;
}

static void expect_size(const char *name, size_t got, size_t want)
{
	if (got == want)
		return;

	fprintf(stderr, "FAIL %s\n  got:  %zu\n  want: %zu\n", name, got, want);
	failures++;
}

static void digest_hex(const uint8_t *data, size_t len, char *hex)
{
	uint8_t digest[SHA1_DIGEST_SIZE];

	sha1_compute(data, len, digest);
	sha1_to_hex(digest, hex);
}

static void digest2_hex(const uint8_t *a, size_t alen, const uint8_t *b,
			size_t blen, char *hex)
{
	uint8_t digest[SHA1_DIGEST_SIZE];

	sha1_compute2(a, alen, b, blen, digest);
	sha1_to_hex(digest, hex);
}

static void test_sha1_to_hex(void)
{
	const uint8_t digest[SHA1_DIGEST_SIZE] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
		0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
		0x0e, 0x0f, 0x10, 0xab, 0xcd, 0xef,
	};
	char hex[41];

	memset(hex, 'x', sizeof(hex));
	sha1_to_hex(digest, hex);

	expect_string("sha1_to_hex encodes lowercase hex", hex,
		      "000102030405060708090a0b0c0d0e0f10abcdef");
	expect_size("sha1_to_hex writes a 40-char string", strlen(hex), 40);
	if (hex[40] != '\0') {
		fprintf(stderr, "FAIL sha1_to_hex NUL terminates hex_out\n");
		failures++;
	}
}

static void test_sha1_compute_vectors(void)
{
	const struct {
		const char *name;
		const uint8_t *data;
		size_t len;
		const char *want;
	} cases[] = {
		{
			.name = "empty input",
			.data = (const uint8_t *)"",
			.len = 0,
			.want = "da39a3ee5e6b4b0d3255bfef95601890afd80709",
		},
		{
			.name = "abc",
			.data = (const uint8_t *)"abc",
			.len = 3,
			.want = "a9993e364706816aba3e25717850c26c9cd0d89d",
		},
		{
			.name = "quick brown fox",
			.data = (const uint8_t *)"The quick brown fox jumps over the lazy dog",
			.len = 43,
			.want = "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
		},
	};

	for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
		char hex[41];

		digest_hex(cases[i].data, cases[i].len, hex);
		expect_string(cases[i].name, hex, cases[i].want);
	}
}

static void test_sha1_compute_binary_input(void)
{
	const uint8_t data[] = { 'a', '\0', 'b', '\0', 'c' };
	char hex[41];

	digest_hex(data, sizeof(data), hex);
	expect_string("binary input containing NUL bytes", hex,
		      "52aa71588488269464589bd81be624861498ca7b");
}

static void test_sha1_compute2(void)
{
	char hex[41];

	digest2_hex((const uint8_t *)"abc", 3, (const uint8_t *)"def", 3, hex);
	expect_string("sha1_compute2 hashes concatenated buffers", hex,
		      "1f8ac10f23c5b5bc1167bda84b833e5c057a77d2");

	digest2_hex((const uint8_t *)"blob 0", 7, (const uint8_t *)"", 0, hex);
	expect_string("sha1_compute2 supports empty second buffer", hex,
		      "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391");

	digest2_hex((const uint8_t *)"blob 5\0", 7, (const uint8_t *)"hello", 5,
		    hex);
	expect_string("sha1_compute2 computes Git blob object IDs", hex,
		      "b6fc4c620b67d95f953a5c1c1230aaab5db5a1b0");
}

int main(void)
{
	test_sha1_to_hex();
	test_sha1_compute_vectors();
	test_sha1_compute_binary_input();
	test_sha1_compute2();

	if (failures != 0) {
		fprintf(stderr, "%d SHA-1 test(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	puts("SHA-1 tests passed");
	return EXIT_SUCCESS;
}
