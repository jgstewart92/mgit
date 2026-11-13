#include "mgit.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(uint32_t h[5], const unsigned char *p)
{
	uint32_t w[80], a, b, c, d, e, f, k, t;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		       (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20)
			f = (b & c) | (~b & d), k = 0x5a827999;
		else if (i < 40)
			f = b ^ c ^ d, k = 0x6ed9eba1;
		else if (i < 60)
			f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
		else
			f = b ^ c ^ d, k = 0xca62c1d6;
		t = ROL(a, 5) + f + e + k + w[i];
		e = d, d = c, c = ROL(b, 30), b = a, a = t;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

#if defined(__x86_64__) && !defined(MGIT_SHA1_PORTABLE)
#include <immintrin.h>
/*
 * x86's SHA instructions (every Zen, Intel since Ice Lake and Goldmont), as in Intel's reference: the rounds four at
 * a time with sha1rnds4, E carried by sha1nexte, the message schedule by sha1msg1, sha1msg2 and xor. About 6x the
 * portable rounds; a clone hashes every object it receives, and its pack, so this is most of a clone's CPU time.
 * MGIT_SHA1_PORTABLE at build time leaves them out (test.sh checks the portable rounds that way).
 */
#define R4(e_in, e_out, msg, f)                                    \
	e_in = _mm_sha1nexte_epu32(e_in, msg);                     \
	e_out = abcd;                                              \
	abcd = _mm_sha1rnds4_epu32(abcd, e_in, f)

__attribute__((target("sha,sse4.1"))) static void blocks_ni(uint32_t h[5], const unsigned char *p, size_t n)
{
	const __m128i mask = _mm_set_epi64x(0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
	__m128i abcd, e0, e1, m0, m1, m2, m3, abcd_save, e0_save;

	abcd = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)h), 0x1B);
	e0 = _mm_set_epi32((int)h[4], 0, 0, 0);
	for (; n--; p += 64) {
		abcd_save = abcd;
		e0_save = e0;
		m0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)p), mask);
		m1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16)), mask);
		m2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 32)), mask);
		m3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 48)), mask);

		/* rounds 0-3 */
		e0 = _mm_add_epi32(e0, m0);
		e1 = abcd;
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 0);
		/* 4-7 */
		R4(e1, e0, m1, 0);
		m0 = _mm_sha1msg1_epu32(m0, m1);
		/* 8-11 */
		R4(e0, e1, m2, 0);
		m1 = _mm_sha1msg1_epu32(m1, m2);
		m0 = _mm_xor_si128(m0, m2);
		/* 12-15 */
		e1 = _mm_sha1nexte_epu32(e1, m3);
		e0 = abcd;
		m0 = _mm_sha1msg2_epu32(m0, m3);
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 0);
		m2 = _mm_sha1msg1_epu32(m2, m3);
		m1 = _mm_xor_si128(m1, m3);
		/* 16-19 */
		e0 = _mm_sha1nexte_epu32(e0, m0);
		e1 = abcd;
		m1 = _mm_sha1msg2_epu32(m1, m0);
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 0);
		m3 = _mm_sha1msg1_epu32(m3, m0);
		m2 = _mm_xor_si128(m2, m0);
		/* 20-23 */
		e1 = _mm_sha1nexte_epu32(e1, m1);
		e0 = abcd;
		m2 = _mm_sha1msg2_epu32(m2, m1);
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 1);
		m0 = _mm_sha1msg1_epu32(m0, m1);
		m3 = _mm_xor_si128(m3, m1);
		/* 24-27 */
		e0 = _mm_sha1nexte_epu32(e0, m2);
		e1 = abcd;
		m3 = _mm_sha1msg2_epu32(m3, m2);
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 1);
		m1 = _mm_sha1msg1_epu32(m1, m2);
		m0 = _mm_xor_si128(m0, m2);
		/* 28-31 */
		e1 = _mm_sha1nexte_epu32(e1, m3);
		e0 = abcd;
		m0 = _mm_sha1msg2_epu32(m0, m3);
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 1);
		m2 = _mm_sha1msg1_epu32(m2, m3);
		m1 = _mm_xor_si128(m1, m3);
		/* 32-35 */
		e0 = _mm_sha1nexte_epu32(e0, m0);
		e1 = abcd;
		m1 = _mm_sha1msg2_epu32(m1, m0);
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 1);
		m3 = _mm_sha1msg1_epu32(m3, m0);
		m2 = _mm_xor_si128(m2, m0);
		/* 36-39 */
		e1 = _mm_sha1nexte_epu32(e1, m1);
		e0 = abcd;
		m2 = _mm_sha1msg2_epu32(m2, m1);
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 1);
		m0 = _mm_sha1msg1_epu32(m0, m1);
		m3 = _mm_xor_si128(m3, m1);
		/* 40-43 */
		e0 = _mm_sha1nexte_epu32(e0, m2);
		e1 = abcd;
		m3 = _mm_sha1msg2_epu32(m3, m2);
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 2);
		m1 = _mm_sha1msg1_epu32(m1, m2);
		m0 = _mm_xor_si128(m0, m2);
		/* 44-47 */
		e1 = _mm_sha1nexte_epu32(e1, m3);
		e0 = abcd;
		m0 = _mm_sha1msg2_epu32(m0, m3);
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 2);
		m2 = _mm_sha1msg1_epu32(m2, m3);
		m1 = _mm_xor_si128(m1, m3);
		/* 48-51 */
		e0 = _mm_sha1nexte_epu32(e0, m0);
		e1 = abcd;
		m1 = _mm_sha1msg2_epu32(m1, m0);
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 2);
		m3 = _mm_sha1msg1_epu32(m3, m0);
		m2 = _mm_xor_si128(m2, m0);
		/* 52-55 */
		e1 = _mm_sha1nexte_epu32(e1, m1);
		e0 = abcd;
		m2 = _mm_sha1msg2_epu32(m2, m1);
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 2);
		m0 = _mm_sha1msg1_epu32(m0, m1);
		m3 = _mm_xor_si128(m3, m1);
		/* 56-59 */
		e0 = _mm_sha1nexte_epu32(e0, m2);
		e1 = abcd;
		m3 = _mm_sha1msg2_epu32(m3, m2);
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 2);
		m1 = _mm_sha1msg1_epu32(m1, m2);
		m0 = _mm_xor_si128(m0, m2);
		/* 60-63 */
		e1 = _mm_sha1nexte_epu32(e1, m3);
		e0 = abcd;
		m0 = _mm_sha1msg2_epu32(m0, m3);
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 3);
		m2 = _mm_sha1msg1_epu32(m2, m3);
		m1 = _mm_xor_si128(m1, m3);
		/* 64-67 */
		e0 = _mm_sha1nexte_epu32(e0, m0);
		e1 = abcd;
		m1 = _mm_sha1msg2_epu32(m1, m0);
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 3);
		m3 = _mm_sha1msg1_epu32(m3, m0);
		m2 = _mm_xor_si128(m2, m0);
		/* 68-71 */
		e1 = _mm_sha1nexte_epu32(e1, m1);
		e0 = abcd;
		m2 = _mm_sha1msg2_epu32(m2, m1);
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 3);
		m3 = _mm_xor_si128(m3, m1);
		/* 72-75 */
		e0 = _mm_sha1nexte_epu32(e0, m2);
		e1 = abcd;
		m3 = _mm_sha1msg2_epu32(m3, m2);
		abcd = _mm_sha1rnds4_epu32(abcd, e0, 3);
		/* 76-79 */
		e1 = _mm_sha1nexte_epu32(e1, m3);
		e0 = abcd;
		abcd = _mm_sha1rnds4_epu32(abcd, e1, 3);

		e0 = _mm_sha1nexte_epu32(e0, e0_save);
		abcd = _mm_add_epi32(abcd, abcd_save);
	}
	abcd = _mm_shuffle_epi32(abcd, 0x1B);
	_mm_storeu_si128((__m128i *)h, abcd);
	h[4] = (uint32_t)_mm_extract_epi32(e0, 3);
}

static int have_ni(void)
{
	static int v = -1;

	if (v < 0)
		v = __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1");
	return v;
}
#endif

static void blocks(uint32_t h[5], const unsigned char *p, size_t n)
{
#if defined(__x86_64__) && !defined(MGIT_SHA1_PORTABLE)
	if (have_ni()) {
		blocks_ni(h, p, n);
		return;
	}
#endif
	for (; n--; p += 64)
		block(h, p);
}

void sha1_init(sha1_ctx *c)
{
	c->h[0] = 0x67452301;
	c->h[1] = 0xefcdab89;
	c->h[2] = 0x98badcfe;
	c->h[3] = 0x10325476;
	c->h[4] = 0xc3d2e1f0;
	c->len = 0;
}

void sha1_update(sha1_ctx *c, const void *data, size_t n)
{
	const unsigned char *p = data;
	size_t used = c->len % 64;

	if (!n)
		return;
	c->len += n;
	if (used) {
		size_t take = 64 - used < n ? 64 - used : n;

		memcpy(c->blk + used, p, take);
		p += take;
		n -= take;
		if (used + take < 64)
			return;
		blocks(c->h, c->blk, 1);
	}
	if (n >= 64) {
		blocks(c->h, p, n / 64);
		p += n / 64 * 64;
		n %= 64;
	}
	memcpy(c->blk, p, n);
}

void sha1_final(sha1_ctx *c, unsigned char out[20])
{
	uint64_t bits = c->len * 8;
	size_t used = c->len % 64, padlen = (used < 56 ? 56 : 120) - used;
	unsigned char pad[72] = { 0x80 };
	int i;

	for (i = 0; i < 8; i++)
		pad[padlen + i] = (unsigned char)(bits >> (56 - 8 * i));
	sha1_update(c, pad, padlen + 8);
	for (i = 0; i < 20; i++)
		out[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}
