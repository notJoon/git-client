#include "object.h"
#include "sha1.h"
#include "zlib_wrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int object_write(const char *type, const uint8_t *content, size_t content_len, uint8_t *hash_out)
{
	// 헤더 생성
	char header[64];

	int hlen = snprintf(header, sizeof(header), "%s %zu", type, content_len);
	if (hlen < 0 || (size_t)hlen >= sizeof(header)) {
		// 절단 방지
		fprintf(stderr, "object_write: header snprintf failed\n");
		return -1;
	}
	size_t header_size = (size_t)hlen + 1; // NUL 포함

	// SHA-1 계산
	sha1_compute2((uint8_t *)header, header_size, content, content_len, hash_out);

	char hex[41];
	sha1_to_hex(hash_out, hex);

	// 경로 생성
	char dir[256], path[512];
	snprintf(dir, sizeof(dir), ".git/objects/%.2s", hex);
	snprintf(path, sizeof(path), "%s/%s", dir, hex + 2);

	// 이미 존재하면 스킵 처리
	struct stat st;
	if (stat(path, &st) == 0) {
		return 0;
	}

	// 전체 데이터 조합
	size_t total = header_size + content_len;
	uint8_t *raw = malloc(total);
	if (!raw) {
		return -1;
	}

	memcpy(raw, header, header_size);
	memcpy(raw + header_size, content, content_len);

	// zlib 압축
	uint8_t *compressed = NULL;
	size_t comp_len = 0;

	if (zlib_deflate(raw, total, &compressed, &comp_len) < 0) {
		free(raw);
		return -1;
	}
	free(raw);

	// 디렉터리 생성 및 파일 쓰기
	mkdir(dir, 0755);
	FILE *f = fopen(path, "wb");
	if (!f) {
		free(compressed);
		return -1;
	}

	size_t written = fwrite(compressed, 1, comp_len, f);
	if (fclose(f) != 0 || written != comp_len) {
		free(compressed);
		return -1;
	}
	free(compressed);
	return 0;
}

int object_read(const char *hex_hash, char *type_out, uint8_t **out, size_t *out_len)
{
	char path[512];
	object_path(hex_hash, path, sizeof(path));

	FILE *f = fopen(path, "rb");
	if (!f) {
		return -1;
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}

	long fsize = ftell(f);
	rewind(f);
	if (fsize <= 0) {
		fclose(f);
		return -1; // 빈 파일 방지
	}

	uint8_t *raw = malloc((size_t)fsize);
	if (!raw) {
		fclose(f);
		return -1;
	}

	if (fread(raw, 1, (size_t)fsize, f) != (size_t)fsize) {
		free(raw);
		fclose(f);
		return -1;
	}

	// zlib 압축 해제
	// 실패 시 데이터 미초기화 방지
	uint8_t *data = NULL;
	size_t data_len = 0;
	if (zlib_inflate(raw, (size_t)fsize, &data, &data_len, 0) < 0) {
		free(raw);
		return -1;
	}

	// 헤더 파싱: "type size\0content"
	char *nul = memchr(data, '\0', data_len);
	if (!nul) {
		free(data);
		return -1;
	}

	if (type_out) {
		// "blob", "tree", "commit"
		sscanf((char *)data, "%7s", type_out);
	}

	size_t header_size = (size_t)(nul - (char *)data) + 1;
	*out_len = data_len - header_size;

	// commit_read 같은 파서가 본문을 안전하게 다룰 수 있게
	// 끝에 NUL 1바이트를 추가해서 반환. 단, out_len에는
	// NUL 바이트를를 포함하지 않음.
	*out = malloc(*out_len + 1);
	if (!*out) {
		free(data);
		return -1;
	}

	memcpy(*out, data + header_size, *out_len);
	(*out)[*out_len] = '\0';
	free(data);

	return 0;
}

void object_path(const char *hex, char *buf, size_t sz)
{
	snprintf(buf, sz, ".git/objects/%.2s/%s", hex, hex + 2);
}

static int tree_is_dir(const TreeEntry *e)
{
	return e->mode == MODE_DIR;
}

static int tree_entry_cmp(const void *a, const void *b)
{
	const TreeEntry *ea = a, *eb = b;
	size_t la = strlen(ea->name), lb = strlen(eb->name);
	size_t n = la < lb ? la : lb;

	int cmp = memcmp(ea->name, eb->name, n); // unsigned 바이트 비교
	if (cmp) {
		return cmp;
	}

	unsigned char c1 = (unsigned char)ea->name[n]; // 짧은 쪽은 NUL
	unsigned char c2 = (unsigned char)eb->name[n];

	if (!c1 && tree_is_dir(ea)) {
		c1 = '/';
	}
	if (!c2 && tree_is_dir(eb)) {
		c2 = '/';
	}

	return (c1 < c2) ? -1 : (c1 > c2) ? 1 : 0;
}

void tree_sort(Tree *t)
{
	qsort(t->entries, (size_t)t->count, sizeof(TreeEntry), tree_entry_cmp);
}

static int add_size(size_t *out, size_t value);

int tree_write(Tree *t, uint8_t *hash_out)
{
	if (!t || !hash_out)
		return -1;

	tree_sort(t);

	size_t total = 0;

	for (int i = 0; i < t->count; i++) {
		TreeEntry *e = &t->entries[i];
		const char *mode_str = mode_to_str(e->mode);

		// NOTE: 유효한 Mode 집합이 아님. 원본 데이터를 그대로 보존해야 하는 경우
		// 별도의 처리가 필요하지만 잘못된 값은 처리하지 않을 예정.
		if (!mode_str) {
			return -1;
		}

		if (add_size(&total, strlen(mode_str)) < 0 || add_size(&total, 1) < 0 ||
		    add_size(&total, strlen(e->name)) < 0 || add_size(&total, 1) < 0 ||
		    add_size(&total, SHA1_DIGEST_SIZE) < 0) {
			return -1;
		}
	}

	size_t alloc_size = total;
	if (alloc_size == 0)
		alloc_size = 1;

	uint8_t *buf = malloc(alloc_size);
	if (!buf)
		return -1;

	size_t pos = 0;

	for (int i = 0; i < t->count; i++) {
		TreeEntry *e = &t->entries[i];
		const char *mode_str = mode_to_str(e->mode);
		size_t mlen = strlen(mode_str);
		size_t nlen = strlen(e->name);

		memcpy(buf + pos, mode_str, mlen);
		pos += mlen;

		buf[pos++] = ' ';

		memcpy(buf + pos, e->name, nlen);
		pos += nlen;

		buf[pos++] = '\0';

		memcpy(buf + pos, e->sha1, SHA1_DIGEST_SIZE);
		pos += SHA1_DIGEST_SIZE;
	}

	int rc = object_write(OBJ_TREE, buf, pos, hash_out);
	free(buf);

	return rc;
}

int tree_read(const char *hex, Tree *t)
{
	char type[8];
	uint8_t *data = NULL;
	size_t data_len = 0;
	int rc = -1;

	if (!hex || !t) {
		return -1;
	}

	if (object_read(hex, type, &data, &data_len) < 0) {
		return -1;
	}

	if (strcmp(type, OBJ_TREE) != 0) {
		goto cleanup;
	}

	// TODO: 지금 당장은 엔트리 정렬이나 중복 여부는 검증하지 않음. 신뢰 가능한 오브젝트만
	// 처리하고, 엄격한 검사는 git처럼 fsck 단계로 분리하는 것이 좋아보임.
	t->count = 0;

	const uint8_t *p = data;
	const uint8_t *end = data + data_len;

	while (p < end) {
		char mode_buf[8];

		// Tree가 고정 크기 배열이라 MAX_TREE_ENTRIES를 넘으면 조용히 자르지 않고
		// 전체 실패로 처리.
		if (t->count >= MAX_TREE_ENTRIES) {
			goto cleanup;
		}

		const uint8_t *sp = memchr(p, ' ', (size_t)(end - p));
		if (!sp) {
			goto cleanup;
		}

		size_t mode_len = (size_t)(sp - p);
		if (mode_len == 0 || mode_len >= sizeof(mode_buf)) {
			goto cleanup;
		}

		const uint8_t *name = sp + 1;
		const uint8_t *nul = memchr(name, '\0', (size_t)(end - name));
		if (!nul) {
			goto cleanup;
		}

		size_t name_len = (size_t)(nul - name);
		if (name_len == 0 || name_len >= sizeof(t->entries[0].name)) {
			goto cleanup;
		}

		const uint8_t *sha1 = nul + 1;
		if ((size_t)(end - sha1) < SHA1_DIGEST_SIZE) {
			goto cleanup;
		}

		FileMode mode;

		memcpy(mode_buf, p, mode_len);
		mode_buf[mode_len] = '\0';

		if (mode_parse(mode_buf, &mode) != 0) {
			goto cleanup;
		}

		TreeEntry *e = &t->entries[t->count++];

		e->mode = mode;

		memcpy(e->name, name, name_len);
		e->name[name_len] = '\0';

		memcpy(e->sha1, sha1, SHA1_DIGEST_SIZE);

		p = sha1 + SHA1_DIGEST_SIZE;
	}

	rc = 0;

cleanup:
	// 실패 시 부분 파싱 결과를 노출하지 않도록 카운트를 초기화
	if (rc != 0) {
		t->count = 0;
	}
	free(data);
	return rc;
}

static int add_size(size_t *out, size_t value)
{
	if (*out > SIZE_MAX - value)
		return -1;

	*out += value;
	return 0;
}
