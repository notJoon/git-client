#ifndef SHA1_H
#define SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_SIZE 20

/* raw 20바이트 해시를 40자리 16진수 문자열로 변환 */
void sha1_to_hex(const uint8_t *hash, char *hex_out); /* hex_out은 41바이트 이상 */

/* 버퍼에 대해 SHA-1 계산 */
void sha1_compute(const uint8_t *hash, size_t len, uint8_t *out);

/* 두 파일을 연결해서 SHA-1 계산 */
void sha1_compute2(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t *out);

#endif
