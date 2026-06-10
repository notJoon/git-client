// Blobㅇ른 파일의 바이너리 콘텐츠를 저장하는 객체.
// 파일 이름이나 권한 등의 메타데이터 없이 순수 바이너리만 다룸.
//
// 전체 바이트를 SHA-1 hex 변환 후 `git/objects/XX/YYYY...`
// 경로에 zlib 압축해서 저장하는 방식으로 동작함.
#ifndef OBJECT_H
#define OBJECT_H

#include <stddef.h>
#include <stdint.h>

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

#endif
