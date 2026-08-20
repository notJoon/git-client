#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "index.h"
#include "object.h"
#include "sha1.h"

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
static char tmpdir[] = "/tmp/git-client-index-test.XXXXXX";
static Index *idx;

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

static void expect_bytes(const char *name, const void *got, const void *want, size_t len)
{
	if (memcmp(got, want, len) == 0)
		return;
	fprintf(stderr, "FAIL %s\n", name);
	failures++;
}

static void put_be16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static void put_be32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_raw(const uint8_t *data, size_t len)
{
	FILE *f = fopen(".git/index", "wb");

	if (!f || fwrite(data, 1, len, f) != len || fclose(f) != 0) {
		perror("write .git/index");
		exit(EXIT_FAILURE);
	}
}

static void write_with_checksum(const uint8_t *body, size_t body_len)
{
	uint8_t *file = malloc(body_len + SHA1_DIGEST_SIZE);

	if (!file) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	memcpy(file, body, body_len);
	sha1_compute(body, body_len, file + body_len);
	write_raw(file, body_len + SHA1_DIGEST_SIZE);
	free(file);
}

static size_t make_header(uint8_t *body, uint32_t count)
{
	memcpy(body, "DIRC", 4);
	put_be32(body + 4, 2);
	put_be32(body + 8, count);
	return 12;
}

static size_t append_entry(uint8_t *body, size_t pos, const uint8_t *path, size_t path_len,
                           uint16_t flags)
{
	size_t nul_count = 8 - ((62 + path_len) % 8);

	memset(body + pos, 0, 62);
	put_be32(body + pos + 24, 0100644);
	memset(body + pos + 40, 0x5a, SHA1_DIGEST_SIZE);
	put_be16(body + pos + 60, flags);
	pos += 62;
	memcpy(body + pos, path, path_len);
	pos += path_len;
	memset(body + pos, 0, nul_count);
	return pos + nul_count;
}

static uint8_t *read_file(size_t *len)
{
	FILE *f = fopen(".git/index", "rb");
	uint8_t *data;
	long size;

	if (!f || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
		perror("read .git/index");
		exit(EXIT_FAILURE);
	}
	rewind(f);
	data = malloc((size_t)size);
	if (!data || fread(data, 1, (size_t)size, f) != (size_t)size || fclose(f) != 0) {
		perror("read .git/index");
		exit(EXIT_FAILURE);
	}
	*len = (size_t)size;
	return data;
}

static void test_checksum(void)
{
	uint8_t empty[12];
	uint8_t one[128];
	uint8_t *file;
	size_t file_len;
	size_t len;

	make_header(empty, 0);
	write_with_checksum(empty, sizeof(empty));
	expect_int("index_read accepts a valid checksum", index_read(idx), 0);

	len = make_header(one, 1);
	len = append_entry(one, len, (const uint8_t *)"a", 1, 1);
	write_with_checksum(one, len);
	file = read_file(&file_len);
	file[12] ^= 1;
	write_raw(file, file_len);
	free(file);
	expect_int("index_read rejects a checksum mismatch", index_read(idx), -1);

	write_raw(empty, sizeof(empty));
	expect_int("index_read rejects a missing checksum", index_read(idx), -1);
}

static void test_exact_eof(void)
{
	uint8_t body[128];
	size_t len;

	make_header(body, 0);
	write_raw(body, 11);
	expect_int("index_read rejects a truncated header", index_read(idx), -1);

	len = make_header(body, 1);
	memset(body + len, 0, 61);
	write_raw(body, len + 61);
	expect_int("index_read rejects a truncated fixed entry", index_read(idx), -1);

	len = make_header(body, 1);
	memset(body + len, 0, 62);
	put_be16(body + len + 60, 3);
	memcpy(body + len + 62, "abc", 3);
	write_raw(body, len + 65);
	expect_int("index_read rejects a missing pathname NUL", index_read(idx), -1);

	len = make_header(body, 1);
	len = append_entry(body, len, (const uint8_t *)"abc", 3, 3);
	write_raw(body, len - 1);
	expect_int("index_read rejects truncated padding", index_read(idx), -1);
}

static void test_flags(void)
{
	uint8_t body[128];
	size_t len = make_header(body, 1);

	len = append_entry(body, len, (const uint8_t *)"abc", 3, 0xa003);
	write_with_checksum(body, len);
	expect_int("index_read parses flags", index_read(idx), 0);
	expect_int("index_read parses stage", idx->entries[0].stage, 2);
	expect_int("index_read preserves assume-valid and name length", idx->entries[0].flags,
	           0xa003);
	expect_bytes("index_read uses the flagged pathname length", idx->entries[0].path, "abc\0",
	             4);

	len = make_header(body, 1);
	len = append_entry(body, len, (const uint8_t *)"abc", 3, 0x4003);
	write_with_checksum(body, len);
	expect_int("index_read rejects the extended bit in v2", index_read(idx), -1);
}

static void test_header_and_extensions(void)
{
	uint8_t body[64];
	size_t len;

	len = make_header(body, 0);
	put_be32(body + 4, 3);
	write_with_checksum(body, len);
	expect_int("index_read rejects a version other than v2", index_read(idx), -1);

	len = make_header(body, INDEX_MAX_ENTRIES + 1U);
	write_with_checksum(body, len);
	expect_int("index_read rejects an unsupported entry count", index_read(idx), -1);

	len = make_header(body, 0);
	memcpy(body + len, "TREE", 4);
	put_be32(body + len + 4, 3);
	memcpy(body + len + 8, "abc", 3);
	write_with_checksum(body, len + 11);
	expect_int("index_read skips an optional extension", index_read(idx), 0);

	len = make_header(body, 0);
	memcpy(body + len, "link", 4);
	put_be32(body + len + 4, 0);
	write_with_checksum(body, len + 8);
	expect_int("index_read rejects an unsupported required extension", index_read(idx), -1);

	len = make_header(body, 0);
	memcpy(body + len, "TREE", 4);
	put_be32(body + len + 4, 4);
	memcpy(body + len + 8, "ab", 2);
	write_with_checksum(body, len + 10);
	expect_int("index_read rejects truncated extension data", index_read(idx), -1);
}

static void test_reader_rejects_unsorted_entries(void)
{
	uint8_t body[256];
	size_t len = make_header(body, 2);

	len = append_entry(body, len, (const uint8_t *)"b", 1, 1);
	len = append_entry(body, len, (const uint8_t *)"a", 1, 1);
	write_with_checksum(body, len);
	expect_int("index_read rejects unsorted entries", index_read(idx), -1);
}

static void test_lengths_and_padding(void)
{
	uint8_t *file;
	size_t file_len;
	size_t pos = 12;

	idx->count = 9;
	for (size_t i = 0; i < 8; i++) {
		memset(&idx->entries[i], 0, sizeof(idx->entries[i]));
		idx->entries[i].mode = 0100644;
		memset(idx->entries[i].path, 'a', i + 1);
		idx->entries[i].path[i + 1] = '\0';
	}
	idx->entries[0].flags = 0x8000;
	memset(&idx->entries[8], 0, sizeof(idx->entries[8]));
	idx->entries[8].mode = 0100644;
	memset(idx->entries[8].path, 'a', 4095);
	idx->entries[8].path[4095] = '\0';
	expect_int("index_write writes pathname boundaries", index_write(idx), 0);

	file = read_file(&file_len);
	expect_int("index_write preserves assume-valid", get_be16(file + 12 + 60) & 0x8000, 0x8000);
	for (size_t path_len = 1; path_len <= 8; path_len++) {
		size_t nul_count = 8 - ((62 + path_len) % 8);
		uint8_t zeroes[8] = {0};

		expect_int("short pathname length is stored in flags",
		           get_be16(file + pos + 60) & 0x0fff, (int)path_len);
		expect_bytes("pathname bytes are written verbatim", file + pos + 62,
		             idx->entries[path_len - 1].path, path_len);
		expect_bytes("entry padding contains only NUL bytes", file + pos + 62 + path_len,
		             zeroes, nul_count);
		pos += 62 + path_len + nul_count;
		expect_size("entry ends on an eight-byte boundary", (pos - 12) % 8, 0);
	}
	expect_int("4095-byte pathname length saturates at 0xfff",
	           get_be16(file + pos + 60) & 0x0fff, 0x0fff);
	pos += 62 + 4095 + (8 - ((62 + 4095) % 8));
	expect_size("index contains exactly the entries and checksum", file_len,
	            pos + SHA1_DIGEST_SIZE);
	free(file);
}

static void write_worktree_file(const char *path, const char *contents)
{
	FILE *f = fopen(path, "wb");
	size_t len = strlen(contents);

	if (!f || fwrite(contents, 1, len, f) != len || fclose(f) != 0) {
		perror("write worktree file");
		exit(EXIT_FAILURE);
	}
}

static void test_add_and_conflicts(void)
{
	static const uint8_t oid[SHA1_DIGEST_SIZE] = {1};
	uint8_t *content = NULL;
	size_t content_len = 0;
	char type[8] = {0};
	char hex[41];

	idx->count = 0;
	if (symlink("target", "link") != 0) {
		perror("symlink");
		exit(EXIT_FAILURE);
	}
	expect_int("index_add stages a symlink", index_add(idx, "link"), 0);
	expect_int("index_add stores symlink mode", (int)idx->entries[0].mode, 0120000);
	sha1_to_hex(idx->entries[0].sha1, hex);
	expect_int("symlink blob can be read", object_read(hex, type, &content, &content_len), 0);
	expect_size("symlink blob stores target length", content_len, 6);
	expect_bytes("symlink blob stores the target", content, "target", 6);
	free(content);

	write_worktree_file("resolved", "done\n");
	expect_int("index_add_conflict adds stage 1",
	           index_add_conflict(idx, "resolved", 1, oid, 0100644), 0);
	expect_int("index_add_conflict adds stage 2",
	           index_add_conflict(idx, "resolved", 2, oid, 0100644), 0);
	expect_int("index_add resolves conflict with stage 0", index_add(idx, "resolved"), 0);
	expect_int("resolved path removes stage 1", index_find(idx, "resolved", 1) == NULL, 1);
	expect_int("resolved path removes stage 2", index_find(idx, "resolved", 2) == NULL, 1);
}

static void set_path(IndexEntry *entry, const uint8_t *path, size_t len, uint8_t stage)
{
	memset(entry, 0, sizeof(*entry));
	entry->mode = 0100644;
	memcpy(entry->path, path, len);
	entry->path[len] = '\0';
	entry->stage = stage;
}

static void test_sorting(void)
{
	static const uint8_t high[] = {0x80};
	static const uint8_t *paths[] = {
	    (const uint8_t *)"a",    (const uint8_t *)"same", (const uint8_t *)"same",
	    (const uint8_t *)"same", (const uint8_t *)"z",    high,
	};
	static const size_t lengths[] = {1, 4, 4, 4, 1, 1};
	static const int stages[] = {0, 1, 2, 3, 0, 0};
	uint8_t *file;
	size_t file_len;
	size_t pos = 12;

	idx->count = 6;
	set_path(&idx->entries[0], (const uint8_t *)"same", 4, 3);
	set_path(&idx->entries[1], high, 1, 0);
	set_path(&idx->entries[2], (const uint8_t *)"z", 1, 0);
	set_path(&idx->entries[3], (const uint8_t *)"same", 4, 1);
	set_path(&idx->entries[4], (const uint8_t *)"a", 1, 0);
	set_path(&idx->entries[5], (const uint8_t *)"same", 4, 2);
	expect_int("index_write accepts unsorted entries", index_write(idx), 0);

	file = read_file(&file_len);
	for (size_t i = 0; i < 6; i++) {
		size_t nul_count = 8 - ((62 + lengths[i]) % 8);
		uint16_t flags = get_be16(file + pos + 60);

		expect_bytes("index_write sorts pathnames as unsigned bytes", file + pos + 62,
		             paths[i], lengths[i]);
		expect_int("index_write sorts equal pathnames by stage", (flags >> 12) & 3,
		           stages[i]);
		pos += 62 + lengths[i] + nul_count;
	}
	expect_size("sorted index ends before its checksum", file_len, pos + SHA1_DIGEST_SIZE);
	free(file);
}

static void test_writer_rejects_invalid_entries(void)
{
	idx->count = 1;
	set_path(&idx->entries[0], (const uint8_t *)"a", 1, 4);
	expect_int("index_write rejects an invalid stage", index_write(idx), -1);

	set_path(&idx->entries[0], (const uint8_t *)".git/config", 11, 0);
	expect_int("index_write rejects a forbidden pathname", index_write(idx), -1);

	set_path(&idx->entries[0], (const uint8_t *)"a", 1, 0);
	idx->entries[0].mode = 0;
	expect_int("index_write rejects an invalid mode", index_write(idx), -1);

	idx->count = -1;
	expect_int("index_write rejects a negative entry count", index_write(idx), -1);
	idx->count = 0;
}

static void setup_repo(void)
{
	if (!mkdtemp(tmpdir) || !getcwd(original_cwd, sizeof(original_cwd)) || chdir(tmpdir) != 0 ||
	    mkdir(".git", 0755) != 0 || mkdir(".git/objects", 0755) != 0) {
		perror("setup repo");
		exit(EXIT_FAILURE);
	}
	idx = calloc(1, sizeof(*idx));
	if (!idx) {
		perror("calloc index");
		exit(EXIT_FAILURE);
	}
}

int main(void)
{
	setup_repo();
	test_checksum();
	test_exact_eof();
	test_flags();
	test_header_and_extensions();
	test_reader_rejects_unsorted_entries();
	test_lengths_and_padding();
	test_sorting();
	test_writer_rejects_invalid_entries();
	test_add_and_conflicts();
	free(idx);
	if (chdir(original_cwd) != 0)
		perror("chdir original cwd");

	if (failures != 0) {
		fprintf(stderr, "%d index test(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("index tests passed");
	return EXIT_SUCCESS;
}
