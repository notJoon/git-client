#ifndef REFS_H
#define REFS_H

#include <stddef.h>

/*
 * HEAD에서 현재 커밋 해시(SHA-1)를 읽어 옴.
 * 이때 symbolic ref 추적도 포함해야 함.
 */
int head_read(char *hex_out);

/* HEAD가 가리키는 브랜치 이름 반환 ("refs/heads/main" -> "main") */
int head_branch(char *branch_out, size_t branch_out_size);

/* 브랜치가 가리키는 커밋 해시(SHA-1)를 읽어 옴 */
int ref_read(const char *ref_name, char *hex_out);

/* 커밋 후 브랜치 업데이트 */
int ref_write(const char *ref_name, const char *hex);

/* HEAD를 특정 브랜치로 설정 */
int head_set_branch(const char *branch);

/* HEAD를 detached 상태로 설정 */
int head_set_detached(const char *hex);

/* 브랜치 생성 */
int branch_create(const char *name, const char *start_hex);

/* 브랜치 목록 (refs/heads/) */
int branch_list(char names[][128], int max_count);

#endif // REFS_H
