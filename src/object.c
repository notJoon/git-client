#include "object.h"
#include "zlib_wrap.h"
#include "sha1.h"
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
