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
		block(c->h, c->blk);
	}
	for (; n >= 64; p += 64, n -= 64)
		block(c->h, p);
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
