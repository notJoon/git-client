#include "object.h"
#include "sha1.h"
#include "zlib_wrap.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static char original_cwd[PATH_MAX];
static char tmpdir[] = "/tmp/git-client-object-test.XXXXXX";

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

static void expect_string(const char *name, const char *got, const char *want)
{
	if (strcmp(got, want) == 0)
		return;

	fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", name, got, want);
	failures++;
}

static void expect_bytes(const char *name, const uint8_t *got, const uint8_t *want, size_t len)
{
	if (memcmp(got, want, len) == 0)
		return;

	fprintf(stderr, "FAIL %s\n", name);
	failures++;
}

static void expect_path_exists(const char *name, const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
		return;

	fprintf(stderr, "FAIL %s\n  missing path: %s\n", name, path);
	failures++;
}

static int read_file(const char *path, uint8_t **out, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	long file_size;

	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	file_size = ftell(f);
	if (file_size < 0) {
		fclose(f);
		return -1;
	}
	rewind(f);

	*out = malloc((size_t)file_size);
	if (!*out) {
		fclose(f);
		return -1;
	}
	if (fread(*out, 1, (size_t)file_size, f) != (size_t)file_size) {
		free(*out);
		*out = NULL;
		fclose(f);
		return -1;
	}
	fclose(f);
	*out_len = (size_t)file_size;
	return 0;
}

static int mkdir_if_missing(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static void setup_repo(void)
{
	if (!mkdtemp(tmpdir)) {
		perror("mkdtemp");
		exit(EXIT_FAILURE);
	}

	if (!getcwd(original_cwd, sizeof(original_cwd))) {
		perror("getcwd");
		exit(EXIT_FAILURE);
	}

	if (chdir(tmpdir) != 0 || mkdir_if_missing(".git") != 0 ||
	    mkdir_if_missing(".git/objects") != 0) {
		perror("setup repo");
		exit(EXIT_FAILURE);
	}
}

static void cleanup_repo(void)
{
	if (original_cwd[0] != '\0' && chdir(original_cwd) != 0)
		perror("chdir original cwd");
}

static void test_object_path(void)
{
	char path[128];

	object_path("e69de29bb2d1d6434b8b29ae775ad8c2e48c5391", path, sizeof(path));
	expect_string("object_path uses Git loose-object layout", path,
	              ".git/objects/e6/9de29bb2d1d6434b8b29ae775ad8c2e48c5391");
}

static void test_object_write_empty_blob(void)
{
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	char path[128];

	expect_int("object_write stores empty blob",
	           object_write(OBJ_BLOB, (const uint8_t *)"", 0, hash), 0);
	sha1_to_hex(hash, hex);
	expect_string("empty blob hash matches Git", hex,
	              "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391");

	object_path(hex, path, sizeof(path));
	expect_path_exists("object_write writes loose object file", path);
}

static void test_object_write_read_text_blob(void)
{
	const uint8_t content[] = "Hello, Git!\n";
	const uint8_t raw[] = "blob 12\0Hello, Git!\n";
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	char path[128];
	char type[8] = {0};
	uint8_t *stored = NULL;
	uint8_t *inflated = NULL;
	size_t stored_len = 0;
	size_t inflated_len = 0;
	uint8_t *out = NULL;
	size_t out_len = 0;

	expect_int("object_write stores text blob",
	           object_write(OBJ_BLOB, content, sizeof(content) - 1, hash), 0);
	sha1_to_hex(hash, hex);
	expect_string("text blob hash includes Git object header", hex,
	              "670a245535fe6316eb2316c1103b1a88bb519334");

	object_path(hex, path, sizeof(path));
	expect_int("test can read written loose object", read_file(path, &stored, &stored_len), 0);
	expect_int("written loose object is zlib-compressed Git object data",
	           zlib_inflate(stored, stored_len, &inflated, &inflated_len, sizeof(raw) - 1), 0);
	expect_size("inflated loose object has header plus content length", inflated_len,
	            sizeof(raw) - 1);
	expect_bytes("inflated loose object stores blob header and content", inflated, raw,
	             sizeof(raw) - 1);

	expect_int("object_read reads text blob", object_read(hex, type, &out, &out_len), 0);
	expect_string("object_read reports blob type", type, OBJ_BLOB);
	expect_size("object_read reports text content length", out_len, sizeof(content) - 1);
	expect_bytes("object_read restores text content", out, content, sizeof(content) - 1);
	if (out != NULL && out[out_len] != '\0') {
		fprintf(stderr, "FAIL object_read NUL-terminates returned content buffer\n");
		failures++;
	}

	free(stored);
	free(inflated);
	free(out);
}

static void test_object_write_read_binary_blob(void)
{
	const uint8_t content[] = {0x00, 0xff, 'g', 'i', 't', 0x00, 0x80, '\n'};
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	char type[8] = {0};
	uint8_t *out = NULL;
	size_t out_len = 0;

	expect_int("object_write stores binary blob",
	           object_write(OBJ_BLOB, content, sizeof(content), hash), 0);
	sha1_to_hex(hash, hex);

	expect_int("object_read reads binary blob", object_read(hex, type, &out, &out_len), 0);
	expect_string("object_read reports binary blob type", type, OBJ_BLOB);
	expect_size("object_read preserves binary length", out_len, sizeof(content));
	expect_bytes("object_read preserves binary bytes", out, content, sizeof(content));

	free(out);
}

static void test_object_read_rejects_missing_object(void)
{
	uint8_t *out = NULL;
	size_t out_len = 0;
	char type[8] = {0};

	expect_int("object_read rejects missing object",
	           object_read("0000000000000000000000000000000000000000", type, &out, &out_len),
	           -1);
	if (out != NULL) {
		fprintf(stderr, "FAIL object_read leaves output pointer NULL for missing object\n");
		failures++;
		free(out);
	}
}

int main(void)
{
	setup_repo();

	test_object_path();
	test_object_write_empty_blob();
	test_object_write_read_text_blob();
	test_object_write_read_binary_blob();
	test_object_read_rejects_missing_object();

	cleanup_repo();

	if (failures != 0) {
		fprintf(stderr, "%d object test(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	puts("object tests passed");
	return EXIT_SUCCESS;
}
