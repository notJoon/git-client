#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

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

static void hex_to_bin(const char *hex, uint8_t *out, size_t out_len)
{
	for (size_t i = 0; i < out_len; i++) {
		unsigned int byte;

		sscanf(hex + i * 2, "%2x", &byte);
		out[i] = (uint8_t)byte;
	}
}

static void set_entry(TreeEntry *e, const char *mode, const char *name, const char *sha_hex)
{
	FileMode fm;
	if (mode_parse(mode, &fm) != 0) {
		fprintf(stderr, "Invalid mode: %s\n", mode);
		exit(EXIT_FAILURE);
	}
	e->mode = fm;
	snprintf(e->name, sizeof(e->name), "%s", name);
	hex_to_bin(sha_hex, e->sha1, SHA1_DIGEST_SIZE);
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

/* hello.txt blob ("Hello, Git!\n") shared by the tree fixtures below. */
#define BLOB_HELLO "670a245535fe6316eb2316c1103b1a88bb519334"

static void test_tree_write_single_file(void)
{
	Tree t = {0};
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	char type[8] = {0};
	uint8_t *out = NULL;
	size_t out_len = 0;
	uint8_t want[6 + 1 + 9 + 1 + SHA1_DIGEST_SIZE];
	size_t pos = 0;

	set_entry(&t.entries[0], "100644", "hello.txt", BLOB_HELLO);
	t.count = 1;

	expect_int("tree_write stores single-file tree", tree_write(&t, hash), 0);
	sha1_to_hex(hash, hex);
	expect_string("single-file tree hash matches Git", hex,
	              "d3ec8a0f5950fb1f73ce0d1ed55cd6fa7afcdeb9");

	/* Verify the on-disk serialization: "mode SP name NUL <20-byte raw sha>". */
	expect_int("object_read reads tree object", object_read(hex, type, &out, &out_len), 0);
	expect_string("tree object reports tree type", type, OBJ_TREE);

	memcpy(want + pos, "100644 hello.txt", 16);
	pos += 16;
	want[pos++] = '\0';
	hex_to_bin(BLOB_HELLO, want + pos, SHA1_DIGEST_SIZE);
	pos += SHA1_DIGEST_SIZE;

	expect_size("tree entry uses mode SP name NUL sha layout", out_len, pos);
	expect_bytes("tree entry bytes match Git format", out, want, pos);

	free(out);
}

static void test_tree_sort_orders_entries(void)
{
	Tree t = {0};
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];

	/* Entries supplied out of order; tree_write must sort before hashing. */
	set_entry(&t.entries[0], "100755", "run.sh", BLOB_HELLO);
	set_entry(&t.entries[1], "100644", "hello.txt", BLOB_HELLO);
	t.count = 2;

	expect_int("tree_write stores multi-entry tree", tree_write(&t, hash), 0);
	sha1_to_hex(hash, hex);
	expect_string("tree_write sorts entries by name to match Git", hex,
	              "c14ff4e3c9ff75d15bb997ad80eab92481aa9068");
}

static void test_tree_sort_directory_vs_file(void)
{
	Tree t = {0};
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];

	/*
	 * Git compares a directory entry as if its name ended in '/', so file
	 * "hi.txt" sorts before directory "hi". A plain strcmp would reverse them.
	 */
	set_entry(&t.entries[0], "40000", "hi", "f115c6d5cfb15ca1a72429900dcaca0fd1057951");
	set_entry(&t.entries[1], "100644", "hi.txt", BLOB_HELLO);
	t.count = 2;

	expect_int("tree_write stores dir-and-file tree", tree_write(&t, hash), 0);
	sha1_to_hex(hash, hex);
	expect_string("tree_write compares directory name with trailing slash", hex,
	              "b8bfd21c7237c033e69bc59ada4c9bba1b989a63");
}

static void test_tree_read_round_trip(void)
{
	Tree wt = {0};
	Tree rt = {0};
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	uint8_t want_sha[SHA1_DIGEST_SIZE];

	/* Supplied out of order on purpose; tree_write sorts before storing. */
	set_entry(&wt.entries[0], "100755", "run.sh", BLOB_HELLO);
	set_entry(&wt.entries[1], "100644", "hello.txt", BLOB_HELLO);
	wt.count = 2;

	expect_int("tree_write stores tree for round-trip", tree_write(&wt, hash), 0);
	sha1_to_hex(hash, hex);

	expect_int("tree_read reads tree object", tree_read(hex, &rt), 0);
	expect_int("tree_read restores entry count", rt.count, 2);

	hex_to_bin(BLOB_HELLO, want_sha, SHA1_DIGEST_SIZE);

	/* Entries come back in stored (sorted) order: hello.txt before run.sh. */
	expect_int("tree_read parses first entry mode", (int)rt.entries[0].mode, MODE_REG);
	expect_string("tree_read restores first entry name", rt.entries[0].name, "hello.txt");
	expect_bytes("tree_read restores first entry sha", rt.entries[0].sha1, want_sha,
	             SHA1_DIGEST_SIZE);

	expect_int("tree_read parses second entry mode", (int)rt.entries[1].mode, MODE_EXEC);
	expect_string("tree_read restores second entry name", rt.entries[1].name, "run.sh");
	expect_bytes("tree_read restores second entry sha", rt.entries[1].sha1, want_sha,
	             SHA1_DIGEST_SIZE);
}

static void test_tree_read_rejects_non_tree(void)
{
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	Tree t = {0};

	expect_int("object_write stores blob for type check",
	           object_write(OBJ_BLOB, (const uint8_t *)"hi\n", 3, hash), 0);
	sha1_to_hex(hash, hex);

	expect_int("tree_read rejects non-tree object", tree_read(hex, &t), -1);
}

static void test_tree_read_rejects_missing(void)
{
	Tree t = {0};

	expect_int("tree_read rejects missing object",
	           tree_read("0000000000000000000000000000000000000000", &t), -1);
}

static void test_tree_read_rejects_invalid_mode(void)
{
	uint8_t payload[6 + 1 + 3 + 1 + SHA1_DIGEST_SIZE];
	size_t pos = 0;
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	Tree t = {0};

	/*
	 * Craft a tree payload whose mode "040000" has an invalid leading zero.
	 * tree_read must reject it via mode_parse rather than accept a bad mode.
	 */
	memcpy(payload + pos, "040000 dir", 10);
	pos += 10;
	payload[pos++] = '\0';
	hex_to_bin(BLOB_HELLO, payload + pos, SHA1_DIGEST_SIZE);
	pos += SHA1_DIGEST_SIZE;

	expect_int("object_write stores malformed tree payload",
	           object_write(OBJ_TREE, payload, pos, hash), 0);
	sha1_to_hex(hash, hex);

	expect_int("tree_read rejects entry with invalid mode", tree_read(hex, &t), -1);
}

static void set_sig(Signature *s, const char *name, const char *email, long time, const char *tz)
{
	snprintf(s->name, sizeof(s->name), "%s", name);
	snprintf(s->email, sizeof(s->email), "%s", email);
	s->time = time;
	snprintf(s->tz, sizeof(s->tz), "%s", tz);
}

static void test_commit_write_golden(void)
{
	Commit c = {0};
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	char type[8] = {0};
	uint8_t *out = NULL;
	size_t out_len = 0;

	snprintf(c.tree_hex, sizeof(c.tree_hex), "d3ec8a0f5950fb1f73ce0d1ed55cd6fa7afcdeb9");
	set_sig(&c.author, "Alice", "alice@example.com", 1700000000L, "+0900");
	set_sig(&c.committer, "Alice", "alice@example.com", 1700000000L, "+0900");
	/* Trailing newline must be normalized away so the SHA is stable. */
	snprintf(c.message, sizeof(c.message), "Initial commit\n");

	expect_int("commit_write stores commit", commit_write(&c, hash), 0);
	sha1_to_hex(hash, hex);
	expect_string("commit hash matches Git for identical bytes", hex,
	              "91f1c015a3b15c882926f4702999631339769c25");

	expect_int("object_read reads commit", object_read(hex, type, &out, &out_len), 0);
	expect_string("commit object reports commit type", type, OBJ_COMMIT);

	free(out);
}

static void test_commit_round_trip(void)
{
	Commit wc = {0};
	Commit rc = {0};
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];

	snprintf(wc.tree_hex, sizeof(wc.tree_hex), "d3ec8a0f5950fb1f73ce0d1ed55cd6fa7afcdeb9");
	snprintf(wc.parent_hex[0], sizeof(wc.parent_hex[0]),
	         "1234567890123456789012345678901234567890");
	wc.parent_count = 1;
	set_sig(&wc.author, "Bob", "bob@example.com", 1700001234L, "-0500");
	set_sig(&wc.committer, "Carol", "carol@example.com", 1700005678L, "+0000");
	snprintf(wc.message, sizeof(wc.message), "Second commit");

	expect_int("commit_write stores commit for round-trip", commit_write(&wc, hash), 0);
	sha1_to_hex(hash, hex);

	expect_int("commit_read reads commit", commit_read(hex, &rc), 0);
	expect_string("commit_read restores tree", rc.tree_hex, wc.tree_hex);
	expect_int("commit_read restores parent count", rc.parent_count, 1);
	expect_string("commit_read restores parent", rc.parent_hex[0], wc.parent_hex[0]);
	expect_string("commit_read restores author name", rc.author.name, "Bob");
	expect_string("commit_read restores author email", rc.author.email, "bob@example.com");
	expect_int("commit_read restores author time", (int)rc.author.time, 1700001234);
	expect_string("commit_read restores author tz", rc.author.tz, "-0500");
	expect_string("commit_read restores committer name", rc.committer.name, "Carol");
	expect_string("commit_read restores committer tz", rc.committer.tz, "+0000");
	expect_string("commit_read restores message", rc.message, "Second commit");
}

static void test_commit_write_rejects_null(void)
{
	Commit c = {0};
	uint8_t hash[SHA1_DIGEST_SIZE];

	expect_int("commit_write rejects NULL commit", commit_write(NULL, hash), -1);
	expect_int("commit_write rejects NULL hash_out", commit_write(&c, NULL), -1);
}

static void test_commit_read_rejects_missing(void)
{
	Commit c = {0};

	expect_int("commit_read rejects missing object",
	           commit_read("0000000000000000000000000000000000000000", &c), -1);
}

static void test_commit_read_rejects_non_commit(void)
{
	uint8_t hash[SHA1_DIGEST_SIZE];
	char hex[41];
	Commit c = {0};

	expect_int("object_write stores blob for commit type check",
	           object_write(OBJ_BLOB, (const uint8_t *)"hi\n", 3, hash), 0);
	sha1_to_hex(hash, hex);

	expect_int("commit_read rejects non-commit object", commit_read(hex, &c), -1);
}

int main(void)
{
	setup_repo();

	test_object_path();
	test_object_write_empty_blob();
	test_object_write_read_text_blob();
	test_object_write_read_binary_blob();
	test_object_read_rejects_missing_object();
	test_tree_write_single_file();
	test_tree_sort_orders_entries();
	test_tree_sort_directory_vs_file();
	test_tree_read_round_trip();
	test_tree_read_rejects_non_tree();
	test_tree_read_rejects_missing();
	test_tree_read_rejects_invalid_mode();
	test_commit_write_golden();
	test_commit_round_trip();
	test_commit_write_rejects_null();
	test_commit_read_rejects_missing();
	test_commit_read_rejects_non_commit();

	cleanup_repo();

	if (failures != 0) {
		fprintf(stderr, "%d object test(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	puts("object tests passed");
	return EXIT_SUCCESS;
}
