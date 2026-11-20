#include "mgit.h"

#include <stdlib.h>
#include <string.h>

static uint32_t adler32(const unsigned char *p, size_t n)
{
	uint32_t a = 1, b = 0;

	while (n) {
		size_t chunk = n < 5552 ? n : 5552;

		n -= chunk;
		while (chunk--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

/* Stored (uncompressed) deflate blocks are a valid zlib stream, so git reads these like any other object. */
void zlib_store(buf *out, const unsigned char *in, size_t n)
{
	uint32_t ad = adler32(in, n);
	unsigned char t[4];

	buf_addc(out, 0x78);
	buf_addc(out, 0x01);
	do {
		size_t len = n < 65535 ? n : 65535;
		unsigned char h[5];

		h[0] = len == n;
		h[1] = (unsigned char)(len & 0xff);
		h[2] = (unsigned char)(len >> 8);
		h[3] = (unsigned char)(~len & 0xff);
		h[4] = (unsigned char)((~len >> 8) & 0xff);
		buf_add(out, h, 5);
		buf_add(out, in, len);
		in += len;
		n -= len;
	} while (n);
	t[0] = (unsigned char)(ad >> 24);
	t[1] = (unsigned char)(ad >> 16);
	t[2] = (unsigned char)(ad >> 8);
	t[3] = (unsigned char)ad;
	buf_add(out, t, 4);
}

/*
 * Inflate (RFC 1951). A Huffman code of up to FAST bits is decoded in one step through a table indexed by the next
 * FAST bits of input; a longer one (rare: up to 15 bits) one bit at a time through the canonical counts.
 */

enum { FAST = 10 };

struct huff {
	short count[16];
	short symbol[288];
	uint16_t fast[1 << FAST];	/* by the next FAST bits: length << 9 | symbol, 0 for a longer code */
};

struct state {
	const unsigned char *in;
	size_t inlen, pos;
	uint32_t bitbuf;
	int bitcnt;
	buf *out;
	size_t start, max;	/* out held start bytes before this stream, which may add at most max */
	int err;	/* set once the stream is found bad; sticky, and every loop that decodes checks it */
};

static int bits(struct state *s, int need)
{
	uint32_t v = s->bitbuf;

	while (s->bitcnt < need) {
		if (__builtin_expect(s->pos >= s->inlen, 0)) {	/* out of input: read zeros on, and mark the stream bad */
			s->err = 1;
			s->bitcnt += 8;
			continue;
		}
		v |= (uint32_t)s->in[s->pos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = v >> need;
	s->bitcnt -= need;
	return (int)(v & ((1u << need) - 1));
}

static int construct(struct huff *h, const short *length, int n)
{
	short offs[16];
	int sym, len, left = 1;

	memset(h->fast, 0, sizeof h->fast);
	for (len = 0; len < 16; len++)
		h->count[len] = 0;
	for (sym = 0; sym < n; sym++)
		h->count[length[sym]]++;
	if (h->count[0] == n)
		return 0;
	for (len = 1; len < 16; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (len = 1; len < 15; len++)
		offs[len + 1] = (short)(offs[len] + h->count[len]);
	for (sym = 0; sym < n; sym++)
		if (length[sym])
			h->symbol[offs[length[sym]]++] = (short)sym;
	{
		int code = 0, idx = 0, k;

		for (len = 1; len <= FAST; len++) {
			for (k = 0; k < h->count[len]; k++, code++) {
				int rev = 0, b, j;

				for (b = 0; b < len; b++)
					rev |= ((code >> b) & 1) << (len - 1 - b);
				for (j = rev; j < 1 << FAST; j += 1 << len)
					h->fast[j] = (uint16_t)(len << 9 | h->symbol[idx]);
				idx++;
			}
			code <<= 1;
		}
	}
	return left;
}

static int decode_slow(struct state *s, const struct huff *h)
{
	int code = 0, first = 0, index = 0, len, count;

	for (len = 1; len < 16; len++) {
		code |= bits(s, 1);
		count = h->count[len];
		if (code - count < first)
			return s->err ? -1 : h->symbol[index + (code - first)];	/* bits ran past the input: no symbol */
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* Reads ahead up to FAST bits, whole bytes, which stored blocks and the end of the stream give back. */
static inline int decode(struct state *s, const struct huff *h)
{
	unsigned e;

	while (s->bitcnt < FAST && s->pos < s->inlen) {
		s->bitbuf |= (uint32_t)s->in[s->pos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	e = h->fast[s->bitbuf & ((1u << FAST) - 1)];
	if (e && (int)(e >> 9) <= s->bitcnt) {
		s->bitbuf >>= e >> 9;
		s->bitcnt -= (int)(e >> 9);
		return (int)(e & 511);
	}
	return decode_slow(s, h);
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void codes(struct state *s, const struct huff *lencode, const struct huff *distcode)
{
	buf *out = s->out;

	for (;;) {
		int sym = decode(s, lencode);
		size_t len, dist;

		if (sym < 0)
			{ s->err = 1; return; }
		if (sym < 256) {
			if (out->len - s->start >= s->max)
				{ s->err = 1; return; }
			if (out->len + 2 > out->cap)
				buf_grow(out, 1);
			out->p[out->len++] = (unsigned char)sym;
			continue;
		}
		if (sym == 256) {
			if (out->p)
				out->p[out->len] = 0;
			return;
		}
		sym -= 257;
		if (sym >= 29)
			{ s->err = 1; return; }
		len = (size_t)(lbase[sym] + bits(s, lext[sym]));
		sym = decode(s, distcode);
		if (sym < 0 || sym >= 30)
			{ s->err = 1; return; }
		dist = (size_t)(dbase[sym] + bits(s, dext[sym]));
		if (s->err)
			return;
		if (dist > out->len - s->start || len > s->max - (out->len - s->start))
			{ s->err = 1; return; }
		buf_grow(out, len);
		{
			unsigned char *d = out->p + out->len;
			const unsigned char *from = d - dist;

			out->len += len;
			if (dist >= len)
				memcpy(d, from, len);
			else
				while (len--)
					*d++ = *from++;
		}
	}
}

static void stored(struct state *s)
{
	size_t len;

	s->pos -= (size_t)(s->bitcnt / 8);	/* whole bytes decode read ahead go back */
	s->bitbuf = 0;
	s->bitcnt = 0;
	if (s->inlen - s->pos < 4)
		{ s->err = 1; return; }
	len = (size_t)s->in[s->pos] | (size_t)s->in[s->pos + 1] << 8;
	if (s->in[s->pos + 2] != (~len & 0xff) || s->in[s->pos + 3] != ((~len >> 8) & 0xff))
		{ s->err = 1; return; }
	s->pos += 4;
	if (s->inlen - s->pos < len || len > s->max - (s->out->len - s->start))
		{ s->err = 1; return; }
	buf_add(s->out, s->in + s->pos, len);
	s->pos += len;
}

static void fixed(struct state *s)
{
	static int built;
	static struct huff lencode, distcode;

	if (!built) {
		short lengths[288];
		int i;

		for (i = 0; i < 144; i++)
			lengths[i] = 8;
		for (; i < 256; i++)
			lengths[i] = 9;
		for (; i < 280; i++)
			lengths[i] = 7;
		for (; i < 288; i++)
			lengths[i] = 8;
		construct(&lencode, lengths, 288);
		for (i = 0; i < 30; i++)
			lengths[i] = 5;
		construct(&distcode, lengths, 30);
		built = 1;
	}
	codes(s, &lencode, &distcode);
}

static void dynamic(struct state *s)
{
	static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	short lengths[320];
	struct huff lencode, distcode;
	int nlen, ndist, ncode, index, err;

	nlen = bits(s, 5) + 257;
	ndist = bits(s, 5) + 1;
	ncode = bits(s, 4) + 4;
	if (nlen > 286 || ndist > 30)
		{ s->err = 1; return; }
	for (index = 0; index < ncode; index++)
		lengths[order[index]] = (short)bits(s, 3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (construct(&lencode, lengths, 19) != 0)
		{ s->err = 1; return; }
	index = 0;
	while (index < nlen + ndist) {
		int sym = decode(s, &lencode), len = 0, rep;

		if (sym < 0)
			{ s->err = 1; return; }
		if (sym < 16) {
			lengths[index++] = (short)sym;
			continue;
		}
		if (sym == 16) {
			if (index == 0)
				{ s->err = 1; return; }
			len = lengths[index - 1];
			rep = 3 + bits(s, 2);
		} else if (sym == 17) {
			rep = 3 + bits(s, 3);
		} else {
			rep = 11 + bits(s, 7);
		}
		if (index + rep > nlen + ndist)
			{ s->err = 1; return; }
		while (rep--)
			lengths[index++] = (short)len;
	}
	if (s->err)
		return;
	if (lengths[256] == 0)
		{ s->err = 1; return; }
	err = construct(&lencode, lengths, nlen);
	if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
		{ s->err = 1; return; }
	err = construct(&distcode, lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
		{ s->err = 1; return; }
	codes(s, &lencode, &distcode);
}

/* Appends the decompressed stream to out. Rejects bad headers, bad data, trailing bytes and checksum mismatches. */
int zlib_inflate(buf *out, const unsigned char *in, size_t n)
{
	size_t used;

	return zlib_inflate_prefix(out, in, n, SIZE_MAX, &used) || used != n ? -1 : 0;
}

/* The same for a stream that may be followed by other data, as in a pack, and that may give at most max bytes (it
 * fails as soon as it would give more); used is the stream's length. */
int zlib_inflate_prefix(buf *out, const unsigned char *in, size_t n, size_t max, size_t *used)
{
	struct state s;
	size_t start = out->len;
	uint32_t want;
	int last, type;

	if (n < 7 || (in[0] & 0x0f) != 8 || (in[0] >> 4) > 7 || (in[1] & 0x20) || ((in[0] << 8) | in[1]) % 31)
		return -1;
	s.in = in;
	s.inlen = n;
	s.pos = 2;
	s.bitbuf = 0;
	s.bitcnt = 0;
	s.out = out;
	s.start = start;
	s.max = max;
	s.err = 0;
	do {
		last = bits(&s, 1);
		type = bits(&s, 2);
		if (s.err)
			return -1;
		if (type == 0)
			stored(&s);
		else if (type == 1)
			fixed(&s);
		else if (type == 2)
			dynamic(&s);
		else
			return -1;
		if (s.err)
			return -1;
	} while (!last);
	s.pos -= (size_t)(s.bitcnt / 8);	/* the bytes decode read ahead are the trailer's */
	if (n - s.pos < 4)
		return -1;
	want = (uint32_t)in[s.pos] << 24 | (uint32_t)in[s.pos + 1] << 16 | (uint32_t)in[s.pos + 2] << 8 | in[s.pos + 3];
	*used = s.pos + 4;
	return adler32(out->p ? out->p + start : NULL, out->len - start) == want ? 0 : -1;
}

/*
 * Deflate (RFC 1951) in a zlib wrapper: LZ77 over the 32 KiB window, found through hash chains with one step of lazy
 * matching (as zlib's default level does), then each block of up to 16384 symbols sent as whichever of stored, fixed
 * or dynamic Huffman is the shortest.
 */

enum { WSIZE = 32768, HBITS = 15, MAXCHAIN = 128, LAZY = 32, MAXMATCH = 258, TOOFAR = 4096, BLOCK = 16384 };

struct bitw {
	buf *out;
	uint64_t v;
	int n;
};

static void put(struct bitw *w, uint32_t v, int n)
{
	w->v |= (uint64_t)v << w->n;
	w->n += n;
	while (w->n >= 8) {
		buf_addc(w->out, (int)(w->v & 0xff));
		w->v >>= 8;
		w->n -= 8;
	}
}

static unsigned char lsym[MAXMATCH + 1], dsym[WSIZE + 1];

static void sym_tables(void)
{
	static int built;
	int c, i;

	if (built)
		return;
	for (c = 0; c < 29; c++)
		for (i = lbase[c]; i <= MAXMATCH && (c == 28 || i < lbase[c + 1]); i++)
			lsym[i] = (unsigned char)c;
	for (c = 0; c < 30; c++)
		for (i = dbase[c]; i <= WSIZE && (c == 29 || i < dbase[c + 1]); i++)
			dsym[i] = (unsigned char)c;
	built = 1;
}

struct hnode {
	uint32_t w;
	int sym, parent;
};

static int hnode_cmp(const void *a, const void *b)
{
	const struct hnode *x = a, *y = b;

	return x->w != y->w ? (x->w < y->w ? -1 : 1) : x->sym - y->sym;
}

/*
 * Huffman code lengths of at most limit bits for n symbols. At least two symbols get a code (zlib does the same), so
 * every code is complete; when the tree is too deep, the frequencies are halved and it is built again.
 */
static void huff_lengths(const uint32_t *freq, int n, int limit, unsigned char *len)
{
	struct hnode node[2 * 288];
	uint32_t f[288];
	int i, m, used = 0;

	for (i = 0; i < n; i++)
		used += (f[i] = freq[i]) != 0;
	for (i = 0; used < 2 && i < n; i++)
		if (!f[i]) {
			f[i] = 1;
			used++;
		}
	for (;;) {
		int a = 0, b, next, k, deepest = 0;

		m = 0;
		for (i = 0; i < n; i++)
			if (f[i]) {
				node[m].w = f[i];
				node[m].sym = i;
				m++;
			}
		qsort(node, (size_t)m, sizeof *node, hnode_cmp);
		b = next = m;
		/* two queues: the sorted leaves, and the internal nodes, which come out in order of weight */
		for (k = 0; k < m - 1; k++) {
			int pick[2], j;

			for (j = 0; j < 2; j++)
				pick[j] = a < m && (b == next || node[a].w <= node[b].w) ? a++ : b++;
			node[next].w = node[pick[0]].w + node[pick[1]].w;
			node[next].sym = -1;
			node[pick[0]].parent = node[pick[1]].parent = next;
			next++;
		}
		node[next - 1].parent = -1;
		{
			unsigned char depth[2 * 288];

			depth[next - 1] = 0;
			for (k = next - 2; k >= 0; k--) {
				depth[k] = (unsigned char)(depth[node[k].parent] + 1);
				if (k < m && depth[k] > deepest)
					deepest = depth[k];
			}
			if (deepest <= limit) {
				memset(len, 0, (size_t)n);
				for (k = 0; k < m; k++)
					len[node[k].sym] = depth[k];
				return;
			}
		}
		for (i = 0; i < n; i++)
			if (f[i])
				f[i] = (f[i] + 1) / 2;
	}
}

/* Canonical codes from lengths (RFC 1951 3.2.2), bit-reversed because deflate sends a code's top bit first. */
static void huff_codes(const unsigned char *len, int n, uint16_t *code)
{
	int count[16] = { 0 }, next[16], bits, i, c = 0;

	for (i = 0; i < n; i++)
		count[len[i]]++;
	count[0] = 0;
	for (bits = 1; bits < 16; bits++) {
		c = (c + count[bits - 1]) << 1;
		next[bits] = c;
	}
	for (i = 0; i < n; i++) {
		int v = len[i] ? next[len[i]]++ : 0, r = 0, k;

		for (k = 0; k < len[i]; k++)
			r |= ((v >> k) & 1) << (len[i] - 1 - k);
		code[i] = (uint16_t)r;
	}
}

struct block {
	uint16_t len[BLOCK], dist[BLOCK]; /* dist 0: len is a literal byte */
	size_t n;
};

/* Bits the symbols take under the given code lengths, extra bits included. */
static uint64_t data_bits(const uint32_t *lf, const uint32_t *df, const unsigned char *ll, const unsigned char *dl)
{
	uint64_t t = 0;
	int i;

	for (i = 0; i < 286; i++)
		t += (uint64_t)lf[i] * (ll[i] + (i > 256 ? lext[i - 257] : 0));
	for (i = 0; i < 30; i++)
		t += (uint64_t)df[i] * (dl[i] + dext[i]);
	return t;
}

static void send_data(struct bitw *w, const struct block *b, const uint16_t *lc, const unsigned char *ll,
		      const uint16_t *dc, const unsigned char *dl)
{
	size_t i;

	for (i = 0; i < b->n; i++) {
		unsigned l = b->len[i], d = b->dist[i];

		if (!d) {
			put(w, lc[l], ll[l]);
		} else {
			int s = lsym[l], t = dsym[d];

			put(w, lc[257 + s], ll[257 + s]);
			put(w, l - (unsigned)lbase[s], lext[s]);
			put(w, dc[t], dl[t]);
			put(w, d - (unsigned)dbase[t], dext[t]);
		}
	}
	put(w, lc[256], ll[256]);
}

static void send_stored(struct bitw *w, const unsigned char *in, size_t n, int last)
{
	do {
		size_t k = n < 65535 ? n : 65535;
		unsigned char h[4] = { (unsigned char)k, (unsigned char)(k >> 8), (unsigned char)~k, (unsigned char)(~k >> 8) };

		put(w, last && k == n, 1);
		put(w, 0, 2);
		if (w->n)
			put(w, 0, 8 - w->n);
		buf_add(w->out, h, 4);
		buf_add(w->out, in, k);
		in += k;
		n -= k;
	} while (n);
}

/* The block for the raw bytes in[0..n), as stored, fixed or dynamic Huffman, whichever is shortest. */
static void send_block(struct bitw *w, const struct block *b, const unsigned char *in, size_t n, int last)
{
	static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	uint32_t lf[286] = { 0 }, df[30] = { 0 }, cf[19] = { 0 };
	unsigned char ll[286], dl[30], fl[288], fd[30], cl[19], all[316], rs[316], rx[316];
	uint16_t lc[288], dc[30], cc[19];
	uint64_t fixed_bits, dyn_bits, stored_bits;
	int hlit = 286, hdist = 30, hclen = 19, total, nr = 0, i;
	size_t k;

	for (k = 0; k < b->n; k++) {
		if (!b->dist[k]) {
			lf[b->len[k]]++;
		} else {
			lf[257 + lsym[b->len[k]]]++;
			df[dsym[b->dist[k]]]++;
		}
	}
	lf[256] = 1;
	huff_lengths(lf, 286, 15, ll);
	huff_lengths(df, 30, 15, dl);
	while (hlit > 257 && !ll[hlit - 1])
		hlit--;
	while (hdist > 1 && !dl[hdist - 1])
		hdist--;
	/* the code lengths, run-length coded: 16 repeats the last 3-6 times, 17 and 18 are 3-10 and 11-138 zeros */
	memcpy(all, ll, (size_t)hlit);
	memcpy(all + hlit, dl, (size_t)hdist);
	total = hlit + hdist;
	for (i = 0; i < total;) {
		int v = all[i], run = 1;

		while (i + run < total && all[i + run] == v)
			run++;
		if (v == 0 && run >= 3) {
			int r = run > 138 ? 138 : run;

			rs[nr] = r >= 11 ? 18 : 17;
			rx[nr++] = (unsigned char)(r - (r >= 11 ? 11 : 3));
			i += r;
		} else if (v != 0 && run >= 4) {
			int r = run - 1 > 6 ? 6 : run - 1;

			rs[nr] = (unsigned char)v;
			rx[nr++] = 0;
			rs[nr] = 16;
			rx[nr++] = (unsigned char)(r - 3);
			i += 1 + r;
		} else {
			rs[nr] = (unsigned char)v;
			rx[nr++] = 0;
			i++;
		}
	}
	for (i = 0; i < nr; i++)
		cf[rs[i]]++;
	huff_lengths(cf, 19, 7, cl);
	while (hclen > 4 && !cl[order[hclen - 1]])
		hclen--;
	dyn_bits = 3 + 5 + 5 + 4 + 3 * (uint64_t)hclen + data_bits(lf, df, ll, dl);
	for (i = 0; i < nr; i++)
		dyn_bits += cl[rs[i]] + (rs[i] == 16 ? 2 : rs[i] == 17 ? 3 : rs[i] == 18 ? 7 : 0);
	for (i = 0; i < 288; i++)
		fl[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
	memset(fd, 5, sizeof fd);
	fixed_bits = 3 + data_bits(lf, df, fl, fd);
	stored_bits = ((n + 65534) / 65535 + !n) * (3 + 7 + 32) + 8 * (uint64_t)n;

	if (stored_bits <= fixed_bits && stored_bits <= dyn_bits) {
		send_stored(w, in, n, last);
	} else if (fixed_bits <= dyn_bits) {
		put(w, (uint32_t)last, 1);
		put(w, 1, 2);
		huff_codes(fl, 288, lc);
		huff_codes(fd, 30, dc);
		send_data(w, b, lc, fl, dc, fd);
	} else {
		put(w, (uint32_t)last, 1);
		put(w, 2, 2);
		put(w, (uint32_t)(hlit - 257), 5);
		put(w, (uint32_t)(hdist - 1), 5);
		put(w, (uint32_t)(hclen - 4), 4);
		for (i = 0; i < hclen; i++)
			put(w, cl[order[i]], 3);
		huff_codes(cl, 19, cc);
		for (i = 0; i < nr; i++) {
			put(w, cc[rs[i]], cl[rs[i]]);
			if (rs[i] >= 16)
				put(w, rx[i], rs[i] == 16 ? 2 : rs[i] == 17 ? 3 : 7);
		}
		huff_codes(ll, 286, lc);
		huff_codes(dl, 30, dc);
		send_data(w, b, lc, ll, dc, dl);
	}
}

static uint32_t hash3(const unsigned char *p)
{
	return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & ((1u << HBITS) - 1);
}

/* Longest match for in[p..] among earlier positions on its hash chain, within the window; 0 if under 3 bytes. */
static size_t longest(const unsigned char *in, size_t n, size_t p, const uint32_t *head, const uint32_t *prev,
		      size_t *dist)
{
	size_t best = 0, max = n - p < MAXMATCH ? n - p : MAXMATCH;
	uint32_t c;
	int chain = MAXCHAIN;

	if (max < 3)
		return 0;
	for (c = head[hash3(in + p)]; c && chain--; c = prev[(c - 1) & (WSIZE - 1)]) {
		size_t q = c - 1, l = 0;

		if (p - q > WSIZE)
			break;
		if (in[q + best] != in[p + best])
			continue;
		while (l < max && in[q + l] == in[p + l])
			l++;
		if (l > best) {
			best = l;
			*dist = p - q;
			if (l == max)
				break;
		}
	}
	if (best < 3 || (best == 3 && *dist > TOOFAR))
		return 0;
	return best;
}

void zlib_compress(buf *out, const unsigned char *in, size_t n)
{
	struct bitw w = { out, 0, 0 };
	struct block *b;
	uint32_t *head, *prev, ad = adler32(in, n);
	size_t p = 0, start = 0, len = 0, dist = 0, next_len = 0, next_dist = 0;
	int have_next = 0;
	unsigned char t[4];

	if (n >= UINT32_MAX - 1) {
		zlib_store(out, in, n);
		return;
	}
	sym_tables();
	b = xmalloc(sizeof *b);
	head = calloc((size_t)1 << HBITS, sizeof *head);
	prev = calloc(WSIZE, sizeof *prev);
	if (!head || !prev)
		die("out of memory");
	b->n = 0;
	buf_addc(out, 0x78);
	buf_addc(out, 0x9c);
	while (p < n) {
		if (have_next) {
			len = next_len;
			dist = next_dist;
			have_next = 0;
		} else {
			len = longest(in, n, p, head, prev, &dist);
		}
		if (n - p >= 3) {
			uint32_t h = hash3(in + p);

			prev[p & (WSIZE - 1)] = head[h];
			head[h] = (uint32_t)(p + 1);
		}
		if (len && len < LAZY && p + 1 < n) {
			next_len = longest(in, n, p + 1, head, prev, &next_dist);
			have_next = 1;
			if (next_len > len)
				len = 0;
		}
		if (len) {
			size_t q;

			b->len[b->n] = (uint16_t)len;
			b->dist[b->n++] = (uint16_t)dist;
			for (q = p + 1; q < p + len; q++)
				if (n - q >= 3) {
					uint32_t h = hash3(in + q);

					prev[q & (WSIZE - 1)] = head[h];
					head[h] = (uint32_t)(q + 1);
				}
			p += len;
			have_next = 0;
		} else {
			b->len[b->n] = in[p];
			b->dist[b->n++] = 0;
			p++;
		}
		if (b->n == BLOCK) {
			send_block(&w, b, in + start, p - start, p == n);
			start = p;
			b->n = 0;
			if (p == n)
				goto done;
		}
	}
	send_block(&w, b, in + start, p - start, 1);
done:
	if (w.n)
		put(&w, 0, 8 - w.n);
	t[0] = (unsigned char)(ad >> 24);
	t[1] = (unsigned char)(ad >> 16);
	t[2] = (unsigned char)(ad >> 8);
	t[3] = (unsigned char)ad;
	buf_add(out, t, 4);
	free(b);
	free(head);
	free(prev);
}
