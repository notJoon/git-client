// Blob은 파일의 바이너리 콘텐츠를 저장하는 객체.
// 파일 이름이나 권한 등의 메타데이터 없이 순수 바이너리만 다룸.
//
// 전체 바이트를 SHA-1 hex 변환 후 `git/objects/XX/YYYY...`
// 경로에 zlib 압축해서 저장하는 방식으로 동작함.

#ifndef OBJECT_H
#define OBJECT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sha1.h"

// TODO: enum 타입으로 재정의? 근데 안 예쁜듯.
#define OBJ_BLOB "blob"
#define OBJ_TREE "tree"
#define OBJ_COMMIT "commit"

// Git 오브젝트를 .git/objects에 저장하고 SHA-1을 hash_out에 씀.
int object_write(const char *type, const uint8_t *content, size_t content_len, uint8_t *hash_out);

// SHA-1 hex로 오브젝트를 읽어오는 기능.
int object_read(const char *hex_hash, char *type_out, uint8_t **out, size_t *out_len);

// 오브젝트 경로 반환. .git/objects/ab/edef... 같은 형태를 던져줘야 함.
void object_path(const char *hex_hash, char *path_out, size_t path_size);

// 트리 오브젝트를 워킹 트리에 재귀적으로 기록.
void checkout_tree(const char *tree_hex, const char *prefix);

// -- 트리 구조 관련 정의

#define MAX_TREE_ENTRIES 1024

// enum으로 처리하는 경우 닫힌 집합의 mode만 처리할 수 있기 때문에
// 유효하지 않은 값은 버려질 수 있어 원본 바이트를 보존해야 하는 경우
// 처리가 복잡해질 수 있지만, 애초에 잘못된 값은 고려하지 않을 예정.
typedef enum {
	MODE_REG = 0100644,     /* 일반 파일 */
	MODE_EXEC = 0100755,    /* 실행 파일 */
	MODE_SYMLINK = 0120000, /* 심볼릭 링크 */
	MODE_DIR = 0040000,     /* 디렉토리 */
} FileMode;

static inline const char *mode_to_str(FileMode m)
{
	switch (m) {
	// 앞자리 0 없음.
	case MODE_DIR:
		return "40000";
	case MODE_REG:
		return "100644";
	case MODE_EXEC:
		return "100755";
	case MODE_SYMLINK:
		return "120000";
	}
	return NULL; // 도달 불가
}

static inline int mode_parse(const char *s, FileMode *out)
{
	static const struct {
		const char *text;
		FileMode mode;
	} modes[] = {
	    {"40000", MODE_DIR},
	    {"100644", MODE_REG},
	    {"100755", MODE_EXEC},
	    {"120000", MODE_SYMLINK},
	};

	if (!s || !out)
		return -1;

	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (strcmp(s, modes[i].text) == 0) {
			*out = modes[i].mode;
			return 0;
		}
	}

	return -1;
}

typedef struct {
	FileMode mode;
	char name[256];                 /* 파일이나 디렉토리 이름 */
	uint8_t sha1[SHA1_DIGEST_SIZE]; /* raw 20바이트 */
} TreeEntry;

typedef struct {
	TreeEntry entries[MAX_TREE_ENTRIES];
	int count;
} Tree;

// Tree 객체 직렬화 및 저장
int tree_write(Tree *t, uint8_t *hash_out);

// SHA-1 hex로 Tree 읽기
int tree_read(const char *hex, Tree *t);

// 엔트리 이름 기준 정렬
// 저장 전 필수로 호출해야 함.
void tree_sort(Tree *t);

// -- Commit 오브젝트 관련 정의

#define MAX_PARENTS 2
#define GIT_HEX_SIZE 40
#define GIT_HEX_STR_SIZE (GIT_HEX_SIZE + 1)

typedef struct {
	char name[128];
	char email[128];
	long time;
	char tz[6];
} Signature;

typedef struct {
	char tree_hex[GIT_HEX_STR_SIZE];
	char parent_hex[MAX_PARENTS][GIT_HEX_STR_SIZE]; // 빈 문자열이면 없음.
	int parent_count;

	Signature author;
	Signature committer;

	char message[4096];
} Commit;

int commit_write(const Commit *c, uint8_t *hash_out);
int commit_read(const char *hex, Commit *c);

// -- 디버그/시각화 유틸리티
//
// git 오브젝트가 실제로 어떤 형태로 .git/objects에 저장되고 서로 어떻게 연결되는지 시각화.

// 단일 오브젝트의 내부 저장 형태(경로/타입/크기/본문)를 사람이 읽기 좋게 출력.
// 저장되는 raw 형식 "<type> <size>\0<content>"를 그대로 풀어서 보여준다.
void object_debug_print(const char *hex_hash);

// hex_hash를 루트로 오브젝트 그래프(commit -> tree -> blob)를
// 트리 다이어그램 형태로 재귀 출력한다.
void object_print_graph(const char *hex_hash);

#endif
