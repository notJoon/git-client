#include "sha1.h"

#include <stdio.h>
#include <string.h>

/*
 * RFC 3174 기반 자체 SHA-1 구현.
 * OpenSSL의 디펜던시를 피하기 위해 구현했고 Git 오브젝트 식별용 해시를 계산하는데 사용.
 */

typedef struct {
	uint32_t state[5];     /* 누적 해시 상태 (h0..h4) */
	uint64_t bit_count;    /* 처리한 총 비트 수 */
	uint8_t  block[64];    /* 진행 중인 64바이트 블록 버퍼 */
	size_t   block_len;    /* 버퍼에 쌓인 바이트 수 */
} sha1_ctx;

static uint32_t rotl32(uint32_t v, unsigned bits)
{
	return (v << bits) | (v >> (32 - bits));
}

static void sha1_init(sha1_ctx *ctx)
{
	ctx->state[0] = 0x67452301u;
	ctx->state[1] = 0xEFCDAB89u;
	ctx->state[2] = 0x98BADCFEu;
	ctx->state[3] = 0x10325476u;
	ctx->state[4] = 0xC3D2E1F0u;
	ctx->bit_count = 0;
	ctx->block_len = 0;
}

/* 한 개의 64바이트 블록을 처리해 상태를 갱신한다 */
static void sha1_process_block(sha1_ctx *ctx, const uint8_t *block)
{
	uint32_t w[80];

	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)block[i * 4] << 24 |
		       (uint32_t)block[i * 4 + 1] << 16 |
		       (uint32_t)block[i * 4 + 2] << 8 |
		       (uint32_t)block[i * 4 + 3];

	for (int i = 16; i < 80; i++)
		w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2];
	uint32_t d = ctx->state[3], e = ctx->state[4];

	for (int i = 0; i < 80; i++) {
		uint32_t f, k;

		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999u;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1u;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDCu;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6u;
		}

		uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rotl32(b, 30);
		b = a;
		a = tmp;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
}

static void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t len)
{
	ctx->bit_count += (uint64_t)len * 8;

	while (len > 0) {
		size_t take = 64 - ctx->block_len;
		if (take > len)
			take = len;

		memcpy(ctx->block + ctx->block_len, data, take);
		ctx->block_len += take;
		data += take;
		len -= take;

		if (ctx->block_len == 64) {
			sha1_process_block(ctx, ctx->block);
			ctx->block_len = 0;
		}
	}
}

static void sha1_final(sha1_ctx *ctx, uint8_t *out)
{
	uint64_t total_bits = ctx->bit_count;

	/* 0x80 패딩 후, 길이(8바이트)를 위한 공간을 남기고 0으로 채운다 */
	uint8_t pad = 0x80;
	sha1_update(ctx, &pad, 1);

	uint8_t zero = 0;
	while (ctx->block_len != 56)
		sha1_update(ctx, &zero, 1);

	/* 메시지 길이를 big-endian 64비트로 추가 (sha1_update가 bit_count를
	 * 건드리므로 직접 버퍼에 쓰지 않고 별도 바이트 배열로 넘긴다) */
	uint8_t len_be[8];
	for (int i = 0; i < 8; i++)
		len_be[i] = (uint8_t)(total_bits >> (56 - i * 8));
	sha1_update(ctx, len_be, 8);

	for (int i = 0; i < 5; i++) {
		out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

void sha1_to_hex(const uint8_t *hash, char *hex_out)
{
	for (int i = 0; i < SHA1_DIGEST_SIZE; i++)
		sprintf(hex_out + i * 2, "%02x", hash[i]);
	hex_out[40] = '\0';
}

void sha1_compute(const uint8_t *data, size_t len, uint8_t *out)
{
	sha1_ctx ctx;

	sha1_init(&ctx);
	sha1_update(&ctx, data, len);
	sha1_final(&ctx, out);
}

void sha1_compute2(const uint8_t *a, size_t a_len, const uint8_t *b,
		   size_t b_len, uint8_t *out)
{
	sha1_ctx ctx;

	sha1_init(&ctx);
	sha1_update(&ctx, a, a_len);
	sha1_update(&ctx, b, b_len);
	sha1_final(&ctx, out);
}
