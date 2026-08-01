#define _POSIX_C_SOURCE 200809L

// ref: https://git-scm.com/docs/index-format#_the_git_index_file_has_the_following_format
#include "index.h"
#include "object.h"
#include "sha1.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 엔트리를 이름과 스테이지 기준으로 정렬 */
static int entry_cmp(const void *a, const void *b)
{
	const IndexEntry *ea = a, *eb = b;
	int r = strcmp(ea->path, eb->path);
	return r ? r : (ea->stage - eb->stage);
}

static uint32_t rd_be32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, 4); // *(uint32_t*)(p) 캐스트는 UB임
	return ntohl(v);
}

static uint16_t rd_be16(const uint8_t *p)
{
	uint16_t v;
	memcpy(&v, p, sizeof(v));
	return ntohs(v);
}

static int mode_valid(uint32_t mode)
{
	return mode == 0100644 || mode == 0100755 || mode == 0120000 || mode == 0160000;
}

static int path_valid(const char *path, size_t len)
{
	if (!path || len == 0 || len >= sizeof(((IndexEntry *)0)->path) || path[0] == '/' ||
	    path[len - 1] == '/') {
		return 0;
	}

	const char *part = path;
	const char *end = path + len;
	while (part < end) {
		const char *slash = memchr(part, '/', (size_t)(end - part));
		const char *part_end = slash ? slash : end;
		size_t part_len = (size_t)(part_end - part);

		if (part_len == 0 || (part_len == 1 && part[0] == '.') ||
		    (part_len == 2 && memcmp(part, "..", 2) == 0) ||
		    (part_len == 4 && memcmp(part, ".git", 4) == 0)) {
			return 0;
		}
		part = slash ? slash + 1 : end;
	}
	return 1;
}

static int path_length(const char path[4096], size_t *len)
{
	const char *nul = memchr(path, '\0', 4096);
	if (!nul) {
		return -1;
	}
	*len = (size_t)(nul - path);
	return path_valid(path, *len) ? 0 : -1;
}

int index_read(Index *idx)
{
	uint8_t *buf = NULL;
	FILE *f;
	long file_size;
	size_t len;
	size_t content_len;
	size_t pos;
	uint8_t checksum[SHA1_DIGEST_SIZE];

	if (!idx) {
		return -1;
	}
	f = fopen(".git/index", "rb");
	if (!f) {
		idx->count = 0;
		return 0; // 새 레포
	}

	if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0) {
		goto fail;
	}
	if (fseek(f, 0, SEEK_SET) != 0 || (uintmax_t)file_size > SIZE_MAX) {
		goto fail;
	}
	len = (size_t)file_size;
	if (len < 12 + SHA1_DIGEST_SIZE) {
		goto fail;
	}

	buf = malloc(len);
	if (!buf || fread(buf, 1, len, f) != len) {
		goto fail;
	}
	if (fclose(f) != 0) {
		f = NULL;
		goto fail;
	}
	f = NULL;

	content_len = len - SHA1_DIGEST_SIZE;
	sha1_compute(buf, content_len, checksum);
	if (memcmp(checksum, buf + content_len, SHA1_DIGEST_SIZE) != 0 ||
	    memcmp(buf, "DIRC", 4) != 0 || rd_be32(buf + 4) != 2) {
		goto fail;
	}

	uint32_t count = rd_be32(buf + 8);
	if (count > INDEX_MAX_ENTRIES) {
		goto fail;
	}

	idx->count = 0;
	pos = 12;
	for (uint32_t i = 0; i < count; i++) {
		IndexEntry *e = &idx->entries[i];
		const uint8_t *fixed;
		size_t path_len;

		if (content_len - pos < 62) {
			goto fail;
		}
		fixed = buf + pos;
		pos += 62;

		/* big-endian 필드 파싱 (비정렬 안전 읽기) */
		e->ctime_sec = rd_be32(fixed + 0);
		e->ctime_nsec = rd_be32(fixed + 4);
		e->mtime_sec = rd_be32(fixed + 8);
		e->mtime_nsec = rd_be32(fixed + 12);
		e->dev = rd_be32(fixed + 16);
		e->ino = rd_be32(fixed + 20);
		e->mode = rd_be32(fixed + 24);
		e->uid = rd_be32(fixed + 28);
		e->gid = rd_be32(fixed + 32);
		e->size = rd_be32(fixed + 36);
		memcpy(e->sha1, fixed + 40, 20);
		uint16_t flags = rd_be16(fixed + 60);
		if ((flags & 0x4000) != 0 || !mode_valid(e->mode)) {
			goto fail;
		}
		e->flags = flags;
		e->stage = (flags >> 12) & 0x3; /* stage는 플래그 비트 12, 13 */

		/* 경로 읽기 (NUL 종료) */
		path_len = flags & 0x0fff;
		if (path_len == 0xFFF) {
			const uint8_t *nul = memchr(buf + pos, '\0', content_len - pos);
			if (!nul) {
				goto fail;
			}
			path_len = (size_t)(nul - (buf + pos));
			if (path_len < 0x0fff || path_len >= sizeof(e->path)) {
				goto fail;
			}
		} else {
			if (path_len + 1 > content_len - pos ||
			    memchr(buf + pos, '\0', path_len) != NULL ||
			    buf[pos + path_len] != '\0') {
				goto fail;
			}
		}
		memcpy(e->path, buf + pos, path_len);
		e->path[path_len] = '\0';
		if (!path_valid(e->path, path_len)) {
			goto fail;
		}
		pos += path_len + 1;

		// pathname의 NUL을 포함한 엔트리 크기를 8바이트 경계에 맞춘다.
		size_t pad = (8 - ((62 + path_len + 1) % 8)) % 8;
		if (pad > content_len - pos) {
			goto fail;
		}
		for (size_t j = 0; j < pad; j++) {
			if (buf[pos + j] != '\0') {
				goto fail;
			}
		}
		pos += pad;

		idx->count++;
		if (i > 0 && entry_cmp(&idx->entries[i - 1], e) >= 0) {
			goto fail;
		}
	}

	// v2의 optional extension은 내용을 해석하지 않고 경계만 검증한다.
	while (pos < content_len) {
		if (content_len - pos < 8 || buf[pos] < 'A' || buf[pos] > 'Z') {
			goto fail;
		}
		uint32_t extension_size = rd_be32(buf + pos + 4);
		pos += 8;
		if (extension_size > content_len - pos) {
			goto fail;
		}
		pos += extension_size;
	}

	free(buf);
	return 0;

fail:
	if (f) {
		fclose(f);
	}
	free(buf);
	idx->count = 0;
	return -1;
}

/*
 * IndexEntry가 거대해지는 경우 해당 객체를 복사하는건 비용이 큼.
 * 비용을 줄이기 위래 포인터 배열용 비교자를 구현 함
 */
static int entry_ptr_cmp(const void *a, const void *b)
{
	return entry_cmp(*(const IndexEntry *const *)a, *(const IndexEntry *const *)b);
}

int index_write(const Index *idx)
{
	// 동작 흐름
	// 1. 엔트리 포인터 배열을 정렬
	// 2. 필요한 버퍼 크기를 계산
	// 3. 헤더 처리
	// 4. SHA-1 checksum
	if (!idx || idx->count < 0 || idx->count > INDEX_MAX_ENTRIES) {
		return -1;
	}

	int n = idx->count;
	const IndexEntry **order = NULL;
	if (n > 0) {
		order = malloc((size_t)n * sizeof(*order));
		if (!order) {
			return -1;
		}
		for (int i = 0; i < n; i++) {
			size_t path_len;
			if (idx->entries[i].stage > 3 || (idx->entries[i].flags & 0x4000) != 0 ||
			    !mode_valid(idx->entries[i].mode) ||
			    path_length(idx->entries[i].path, &path_len) != 0) {
				free(order);
				return -1;
			}
			order[i] = &idx->entries[i];
		}
		qsort(order, (size_t)n, sizeof(*order), entry_ptr_cmp);
		for (int i = 1; i < n; i++) {
			if (entry_cmp(order[i - 1], order[i]) == 0) {
				free(order);
				return -1;
			}
		}
	}

	size_t total = 12; // HEADER
	for (int i = 0; i < n; i++) {
		size_t path_len;
		path_length(order[i]->path, &path_len);
		size_t entry_size = 62 + path_len + 1;
		size_t padded_size = entry_size + ((8 - (entry_size % 8)) % 8);
		if (total > SIZE_MAX - padded_size) {
			free(order);
			return -1;
		}
		total += padded_size;
	}
	if (total > SIZE_MAX - SHA1_DIGEST_SIZE) {
		free(order);
		return -1;
	}
	total += SHA1_DIGEST_SIZE;

	uint8_t *buf = malloc(total);
	if (!buf) {
		free(order);
		return -1;
	}
	size_t pos = 0;

	// HEADER
	memcpy(buf + pos, "DIRC", 4);
	pos += 4;
	uint32_t ver = htonl(2);
	uint32_t count = htonl((uint32_t)n);
	memcpy(buf + pos, &ver, 4);
	pos += 4;
	memcpy(buf + pos, &count, 4);
	pos += 4;

	for (int i = 0; i < n; i++) {
		const IndexEntry *e = order[i];
		uint8_t fixed[62];
		memset(fixed, 0, 62);

// 호스트 바이트 순서를 네트워크 바이트 순서(bif endian)로 변경
#define WR32(off, v)                    \
	do {                                \
		uint32_t _v = htonl(v);         \
		memcpy(fixed + (off), &_v, 4);  \
	} while (0)

		// 오프셋  크기    내용
		//   0     4    ctime 초
		//   4     4    ctime 나노초
		//   8     4    mtime 초
		//  12     4    mtime 나노초
		//  16     4    dev
		//  20     4    ino
		//  24     4    mode
		//  28     4    uid
		//  32     4    gid
		//  36     4    파일 크기
		//  40    20    SHA-1
		//  60     2    flags
		WR32(0, e->ctime_sec);
		WR32(4, e->ctime_nsec);
		WR32(8, e->mtime_sec);
		WR32(12, e->mtime_nsec);
		WR32(16, e->dev);
		WR32(20, e->ino);
		WR32(24, e->mode);
		WR32(28, e->uid);
		WR32(32, e->gid);
		WR32(36, e->size);
#undef WR32
		memcpy(fixed + 40, e->sha1, 20);

		size_t path_len;
		path_length(e->path, &path_len);
		// 비트 15    => assume-valid 보존
		// 비트 14    => extended (0)
		// 비트 13-12 => stage 번호, 머지 충돌 시 1/2/3
		// 비트 11-0  => 경로 길이
		// 경로 길이가 12비트에 담기지 않으면 0xFFF로 포화한다.
		uint16_t name_len = (uint16_t)(path_len < 0x0fff ? path_len : 0x0fff);
		uint16_t host_flags = (uint16_t)((e->flags & UINT16_C(0x8000)) |
		                                 ((uint16_t)e->stage << 12) | name_len);
		uint16_t flags = htons(host_flags);
		memcpy(fixed + 60, &flags, 2);

		memcpy(buf + pos, fixed, 62);
		pos += 62;
		memcpy(buf + pos, e->path, path_len);
		pos += path_len;
		buf[pos++] = '\0'; /* NUL 종료자. 패딩에 포함 됨 */

		size_t entry_size = 62 + path_len + 1;
		// 전체 엔트리를 8의 배수가 되도록 패딩 처리.
		// 바깥쪽 `... % 8` 연산은 `entry_size`가 이미 8의 배수일 때
		// 불필요한 8바이트를 추가하는 것을 막기 위해 추가 함.
		//
		// 결과적으로 경로 뒤에는 항상 1~8개의 NUL이 붙게 되고, 파서가 엔트리를
		// `memcpy` 없이 구조체로 직접 매핑할 수 있게 됨.
		size_t pad = (8 - (entry_size % 8)) % 8;
		memset(buf + pos, 0, pad);
		pos += pad;
	}
	free(order);

	/* SHA-1 체크섬 */
	uint8_t cksum[20];
	sha1_compute(buf, pos, cksum);
	memcpy(buf + pos, cksum, 20);
	pos += 20;

	// 완성된 파일만 index로 교체해 중간 쓰기 실패로 인한 손상을 막는다.
	FILE *f = fopen(".git/index.lock", "wbx");
	if (!f) {
		free(buf);
		return -1;
	}
	size_t written = fwrite(buf, 1, pos, f);
	int rc = (fclose(f) == 0 && written == pos) ? 0 : -1;
	if (rc == 0 && rename(".git/index.lock", ".git/index") != 0) {
		rc = -1;
	}
	if (rc != 0) {
		remove(".git/index.lock");
	}
	free(buf);
	return rc;
}

int index_add(Index *idx, const char *path)
{
	struct stat st;
	uint8_t *content = NULL;
	size_t content_len;
	size_t path_len;

	if (!idx || !path || (path_len = strlen(path)) >= sizeof(idx->entries[0].path) ||
	    !path_valid(path, path_len) || lstat(path, &st) != 0) {
		return -1;
	}

	if (S_ISLNK(st.st_mode)) {
		if (st.st_size > 0 && (uintmax_t)st.st_size >= SIZE_MAX) {
			return -1;
		}
		size_t capacity = st.st_size > 0 ? (size_t)st.st_size + 1 : 256;
		for (;;) {
			content = malloc(capacity);
			if (!content) {
				return -1;
			}
			ssize_t read_len = readlink(path, (char *)content, capacity);
			if (read_len < 0) {
				free(content);
				return -1;
			}
			if ((size_t)read_len < capacity) {
				content_len = (size_t)read_len;
				break;
			}
			free(content);
			if (capacity > SIZE_MAX / 2) {
				return -1;
			}
			capacity *= 2;
		}
	} else if (S_ISREG(st.st_mode)) {
		FILE *f = fopen(path, "rb");
		if (!f || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
		    (uintmax_t)st.st_size > SIZE_MAX) {
			if (f) {
				fclose(f);
			}
			return -1;
		}
		content_len = (size_t)st.st_size;
		content = malloc(content_len ? content_len : 1);
		if (!content || fread(content, 1, content_len, f) != content_len ||
		    fgetc(f) != EOF || ferror(f)) {
			free(content);
			fclose(f);
			return -1;
		}
		if (fclose(f) != 0) {
			free(content);
			return -1;
		}
	} else {
		return -1;
	}

	/* blob 저장 */
	uint8_t sha1[20];
	if (object_write(OBJ_BLOB, content, content_len, sha1) < 0) {
		free(content);
		return -1;
	}
	free(content);

	// stage 0으로 해결된 경로에서는 기존 충돌 stage를 제거한다.
	for (int i = 0; i < idx->count;) {
		if (idx->entries[i].stage != 0 && strcmp(idx->entries[i].path, path) == 0) {
			memmove(&idx->entries[i], &idx->entries[i + 1],
			        (size_t)(idx->count - i - 1) * sizeof(idx->entries[0]));
			idx->count--;
		} else {
			i++;
		}
	}

	/* 기존 엔트리 정보 업데이트 아니면 새로 추가 */
	IndexEntry *e = index_find(idx, path, 0);
	if (!e) {
		// 배열 오버플로우 방지
		if (idx->count >= INDEX_MAX_ENTRIES) {
			return -1;
		}
		e = &idx->entries[idx->count++];
		memset(e, 0, sizeof(*e));
	}

	e->ctime_sec = (uint32_t)st.st_ctime;
	e->mtime_sec = (uint32_t)st.st_mtime;
#if defined(__APPLE__)
	e->ctime_nsec = (uint32_t)st.st_ctimensec;
	e->mtime_nsec = (uint32_t)st.st_mtimensec;
#else
	e->ctime_nsec = (uint32_t)st.st_ctim.tv_nsec;
	e->mtime_nsec = (uint32_t)st.st_mtim.tv_nsec;
#endif
	e->dev = (uint32_t)st.st_dev;
	e->ino = (uint32_t)st.st_ino;
	e->mode = S_ISLNK(st.st_mode) ? 0120000 : ((st.st_mode & 0111) ? 0100755 : 0100644);
	e->uid = (uint32_t)st.st_uid;
	e->gid = (uint32_t)st.st_gid;
	e->size = (uint32_t)st.st_size;
	e->flags = 0;
	e->stage = 0;
	memcpy(e->sha1, sha1, 20);
	memcpy(e->path, path, path_len + 1);

	return 0;
}

IndexEntry *index_find(Index *idx, const char *path, int stage)
{
	if (!idx || !path || stage < 0 || stage > 3) {
		return NULL;
	}
	for (int i = 0; i < idx->count; i++) {
		if (idx->entries[i].stage == stage && strcmp(idx->entries[i].path, path) == 0) {
			return &idx->entries[i];
		}
	}
	return NULL;
}

int index_remove(Index *idx, const char *path)
{
	int removed = 0;

	if (!idx || !path) {
		return -1;
	}
	for (int i = 0; i < idx->count;) {
		if (strcmp(idx->entries[i].path, path) == 0) {
			memmove(&idx->entries[i], &idx->entries[i + 1],
			        (size_t)(idx->count - i - 1) * sizeof(idx->entries[0]));
			idx->count--;
			removed = 1;
		} else {
			i++;
		}
	}
	return removed ? 0 : -1;
}

int index_add_conflict(Index *idx, const char *path, int stage, const uint8_t *sha1, uint32_t mode)
{
	size_t path_len;
	IndexEntry *e;

	if (!idx || !path || !sha1 || stage < 1 || stage > 3 || !mode_valid(mode) ||
	    (path_len = strlen(path)) >= sizeof(idx->entries[0].path) ||
	    !path_valid(path, path_len)) {
		return -1;
	}

	for (int i = 0; i < idx->count;) {
		if (idx->entries[i].stage == 0 && strcmp(idx->entries[i].path, path) == 0) {
			memmove(&idx->entries[i], &idx->entries[i + 1],
			        (size_t)(idx->count - i - 1) * sizeof(idx->entries[0]));
			idx->count--;
		} else {
			i++;
		}
	}

	e = index_find(idx, path, stage);
	if (!e) {
		if (idx->count >= INDEX_MAX_ENTRIES) {
			return -1;
		}
		e = &idx->entries[idx->count++];
	}
	memset(e, 0, sizeof(*e));
	e->mode = mode;
	e->stage = (uint8_t)stage;
	memcpy(e->sha1, sha1, SHA1_DIGEST_SIZE);
	memcpy(e->path, path, path_len + 1);
	return 0;
}
