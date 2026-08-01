#ifndef INDEX_H
#define INDEX_H

#include <stdint.h>
#include <time.h>
#include "sha1.h"

#define INDEX_MAX_ENTRIES 65536

typedef struct {
    uint32_t ctime_sec, ctime_nsec;
    uint32_t mtime_sec, mtime_nsec;
    uint32_t dev, ino;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t size;
    uint8_t sha1[SHA1_DIGEST_SIZE];
    uint16_t flags; /* 하위 12비트 (경로 길이) */

    /*
     * 정상 0
     * ancestoe  1
     * ours 2
     * theirs 3
     */
    uint8_t stage;
    char path[4096];
} IndexEntry;

typedef struct {
    IndexEntry entries[INDEX_MAX_ENTRIES];
    int count;
} Index;

/* .git/index 읽기/쓰기 */
int index_read(Index *idx);
int index_write(const Index *idx);

/* 파일을 스테이징 (stat + blob 저장 + 엔트리 추가나 갱신) */
int index_add(Index *idx, const char *path);

/* 경로 기반 엔트리 탐색 (stage=0) */
IndexEntry *index_find(Index *idx, const char *path, int stage);

/* 엔트리 제거 */
int index_remove(Index *idx, const char *path);

/* 충돌 엔트리 (stage 1, 2, 3) 추가. merge 용도 */
int index_add_conflict(Index *idx, const char *path, int stage, const uint8_t *sha1, uint32_t mode);

/* 트리 오브젝트로 부터 인덱스 재구성 (checkout이나 merge 후 사용) */
int index_from_tree(Index *idx, const char *tree_hex, const char *prefix);

/* 인덱스로부터 중첩 트리를 재귀 생성하고 루트 디렉토리의 SHA-1을 out_sha에 입력 */
int build_tree(const Index *idx, const char *prefix, uint8_t *out_sha);

#endif
