#define _DEFAULT_SOURCE
#include "mgit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The pack push sends, with deltas as git's pack-objects makes them: every object to send and every "preferred base"
 * (what the far side already has at the same paths: the trees and files of the commits the new ones grow from) sorted
 * by type, then by a hash of the path's last characters, bases first, then by size, largest first; each object to
 * send then tried as a delta against the 10 before it of its type, kept as one when the delta is smaller than half the
 * object and than any delta found before. A delta on an object in the pack is written as OFS_DELTA (REF_DELTA when the
 * far side does not take offsets), after its base; one on a preferred base as REF_DELTA, which the far side resolves
 * against what it has (a thin pack, as git push sends). Chains stop at 50; objects over 32 MiB are sent whole.
 */

enum { WINDOW = 10, MAX_DEPTH = 50, MAX_DELTA_OBJECT = 32 << 20, MAX_BASES = 20000, MAX_EDGES = 16, BLK = 16,
       BUCKET_MAX = 64 };

/* ---- one delta: copy and insert instructions that rebuild trg from src, as git's patch-delta reads them ---- */

struct dindex {
	const unsigned char *src;
	size_t n;
	uint32_t *head, *next, *pos;
	unsigned shift;
};

static const uint64_t HM = 0x9e3779b97f4a7c15ull;

static uint64_t hm15(void)
{
	static uint64_t v;

	if (!v) {
		int i;

		v = 1;
		for (i = 0; i < BLK - 1; i++)
			v *= HM;
	}
	return v;
}

static uint64_t block_hash(const unsigned char *p)
{
	uint64_t h = 0;
	int i;

	for (i = 0; i < BLK; i++)
		h = h * HM + p[i];
	return h;
}

static void dindex_build(struct dindex *x, const unsigned char *src, size_t n)
{
	size_t nb = n / BLK, nbits = 1, i;
	unsigned char *count;

	memset(x, 0, sizeof *x);
	x->src = src;
	x->n = n;
	if (!nb)
		return;
	while (((size_t)1 << nbits) < nb && nbits < 30)
		nbits++;
	x->shift = 64 - (unsigned)nbits;
	x->head = calloc((size_t)1 << nbits, sizeof *x->head);
	count = calloc((size_t)1 << nbits, 1);
	x->next = xmalloc(nb * sizeof *x->next);
	x->pos = xmalloc(nb * sizeof *x->pos);
	if (!x->head || !count)
		die("out of memory");
	for (i = 0; i < nb; i++) {
		uint64_t b = block_hash(src + i * BLK) >> x->shift;

		x->pos[i] = (uint32_t)(i * BLK);
		x->next[i] = 0;
		if (count[b] >= BUCKET_MAX)
			continue;
		count[b]++;
		x->next[i] = x->head[b];
		x->head[b] = (uint32_t)i + 1;
	}
	free(count);
}

static void dindex_free(struct dindex *x)
{
	free(x->head);
	free(x->next);
	free(x->pos);
	memset(x, 0, sizeof *x);
}

static size_t same_run(const unsigned char *a, const unsigned char *b, size_t max)
{
	size_t k = 0;

	while (k + 8 <= max) {
		uint64_t x, y;

		memcpy(&x, a + k, 8);
		memcpy(&y, b + k, 8);
		if (x != y)
			break;
		k += 8;
	}
	while (k < max && a[k] == b[k])
		k++;
	return k;
}

static void put_size(buf *out, uint64_t v)
{
	do {
		unsigned char c = (unsigned char)(v & 0x7f);

		v >>= 7;
		buf_addc(out, c | (v ? 0x80 : 0));
	} while (v);
}

static void put_insert(buf *out, const unsigned char *p, size_t n)
{
	while (n) {
		size_t k = n < 127 ? n : 127;

		buf_addc(out, (int)k);
		buf_add(out, p, k);
		p += k;
		n -= k;
	}
}

static void put_copy(buf *out, size_t off, size_t len)
{
	while (len) {
		size_t k = len < 0x10000 ? len : 0x10000;
		unsigned char op[8];
		int n = 1, i;

		op[0] = 0x80;
		for (i = 0; i < 4; i++)
			if ((off >> (8 * i)) & 0xff) {
				op[0] |= (unsigned char)(1 << i);
				op[n++] = (unsigned char)(off >> (8 * i));
			}
		if (k != 0x10000)
			for (i = 0; i < 3; i++)
				if ((k >> (8 * i)) & 0xff) {
					op[0] |= (unsigned char)(0x10 << i);
					op[n++] = (unsigned char)(k >> (8 * i));
				}
		buf_add(out, op, (size_t)n);
		off += k;
		len -= k;
	}
}

/*
 * The delta from x's source to trg into out; returns -1 (out undefined) once it would be longer than max. Matches
 * are found through the source's 16-byte blocks, extended forward and back over bytes not yet sent.
 */
static int delta_make(const struct dindex *x, const unsigned char *trg, size_t tn, size_t max, buf *out)
{
	size_t i = 0, lit = 0;
	uint64_t h = 0, m15 = hm15();

	/* a copy carries 4 offset bytes: past 4 GiB of source the delta would rebuild the wrong object (a proved bound) */
	if (x->n > 0xffffffffu)
		return -1;
	out->len = 0;
	put_size(out, x->n);
	put_size(out, tn);
	if (x->head && tn >= BLK)
		h = block_hash(trg);
	while (x->head && i + BLK <= tn) {
		size_t best = 0, bpos = 0;
		uint32_t e;

		for (e = x->head[h >> x->shift]; e; e = x->next[e - 1]) {
			size_t p = x->pos[e - 1], len;

			if (memcmp(x->src + p, trg + i, BLK))
				continue;
			len = BLK + same_run(x->src + p + BLK, trg + i + BLK, (x->n - p < tn - i ? x->n - p : tn - i) - BLK);
			if (len > best) {
				best = len;
				bpos = p;
				if (len >= 4096)
					break;
			}
		}
		if (best) {
			while (lit && bpos && x->src[bpos - 1] == trg[i - 1]) {
				bpos--;
				i--;
				lit--;
				best++;
			}
			put_insert(out, trg + i - lit, lit);
			lit = 0;
			put_copy(out, bpos, best);
			i += best;
			if (out->len > max)
				return -1;
			if (i + BLK <= tn)
				h = block_hash(trg + i);
			continue;
		}
		if (i + BLK < tn)
			h = (h - m15 * trg[i]) * HM + trg[i + BLK];
		i++;
		lit++;
		if (out->len + lit + lit / 127 > max)
			return -1;
	}
	lit += tn - i;
	put_insert(out, trg + tn - lit, lit);
	return out->len > max ? -1 : 0;
}

/* ---- names: the path each object is at, for git's name hash ---- */

struct names {
	struct idset ids;		/* ids named so far */
	unsigned char (*id)[20];
	uint32_t *hash;
	char **path;
	size_t n, cap;
};

static uint32_t name_hash(const char *name)
{
	uint32_t c, h = 0;

	while ((c = (unsigned char)*name++))
		if (!isspace((int)c))
			h = (h >> 2) + (c << 24);
	return h;
}

static void name_add(struct names *nm, const unsigned char id[20], const char *path)
{
	if (!idset_add(&nm->ids, id))
		return;
	if (nm->n == nm->cap) {
		nm->cap = nm->cap ? 2 * nm->cap : 256;
		nm->id = xrealloc(nm->id, nm->cap * sizeof *nm->id);
		nm->hash = xrealloc(nm->hash, nm->cap * sizeof *nm->hash);
		nm->path = xrealloc(nm->path, nm->cap * sizeof *nm->path);
	}
	memcpy(nm->id[nm->n], id, 20);
	nm->hash[nm->n] = name_hash(path);
	nm->path[nm->n++] = xstrdup(path);
}

/*
 * Names what tree holds, at prefix: every entry in want gets its path; a subtree is gone into when want holds it (the
 * objects to send: a tree the far side has holds nothing new), or, for the bases (want NULL), when the push sends a
 * tree at the same path (dirs: its paths' hashes).
 */
static void name_tree(struct names *nm, const unsigned char tree[20], const char *prefix, const struct idset *want,
		      const struct idset *dirs, size_t *budget)
{
	const unsigned char *p, *end;
	buf b = { 0 }, path = { 0 };
	int type;

	if (object_read(tree, &type, &b) || type != OBJ_TREE) {
		buf_free(&b);
		return;
	}
	for (p = b.p, end = b.p + b.len; p < end;) {
		const unsigned char *nul = memchr(p, 0, (size_t)(end - p));
		const char *name = strchr((const char *)p, ' ');
		int dir = !strncmp((const char *)p, "40000 ", 6), link = !strncmp((const char *)p, "160000 ", 7);

		if (!nul || !name || end - nul < 21)
			break;
		path.len = 0;
		buf_adds(&path, prefix);
		buf_adds(&path, name + 1);
		if (!link) {
			if (want ? idset_has(want, nul + 1) : *budget > 0) {
				if (!want)
					(*budget)--;
				name_add(nm, nul + 1, (char *)path.p);
			}
			if (dir) {
				unsigned char key[20];

				object_hash(OBJ_BLOB, path.p, path.len, key);
				if (want ? idset_has(want, nul + 1) : idset_has(dirs, key)) {
					buf_addc(&path, '/');
					name_tree(nm, nul + 1, (char *)path.p, want, dirs, budget);
				}
			}
		}
		p = nul + 21;
	}
	buf_free(&b);
	buf_free(&path);
}

static const struct names *sort_names;

static int by_name_id(const void *a, const void *b)
{
	return memcmp(sort_names->id[*(const size_t *)a], sort_names->id[*(const size_t *)b], 20);
}

/* The name hash of id (0 when it has no path: a commit or a tag), through ord, nm's entries sorted by id. */
static uint32_t name_of(const struct names *nm, const size_t *ord, const unsigned char id[20])
{
	size_t lo = 0, hi = nm->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int r = memcmp(nm->id[ord[mid]], id, 20);

		if (!r)
			return nm->hash[ord[mid]];
		if (r < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

/* ---- the search ---- */

struct cand {
	unsigned char id[20];
	int type, base;		/* base: a preferred base, not sent */
	uint32_t hash;
	size_t size, order;
	long delta_of;		/* index of the base this is a delta on, or -1 */
	int depth;
	buf delta;
};

static int cand_cmp(const void *pa, const void *pb)
{
	const struct cand *a = pa, *b = pb;

	if (a->type != b->type)
		return a->type > b->type ? -1 : 1;
	if (a->hash != b->hash)
		return a->hash > b->hash ? -1 : 1;
	if (a->base != b->base)
		return a->base > b->base ? -1 : 1;
	if (a->size != b->size)
		return a->size > b->size ? -1 : 1;
	return a->order < b->order ? -1 : a->order > b->order;
}

struct slot {
	long who;		/* the candidate held, or -1 */
	buf data;
	struct dindex ix;
	int indexed;
};

static void find_deltas(struct cand *c, size_t n)
{
	struct slot win[WINDOW];
	size_t i, k;
	buf d = { 0 };

	for (k = 0; k < WINDOW; k++) {
		memset(&win[k], 0, sizeof win[k]);
		win[k].who = -1;
	}
	for (i = 0; i < n; i++) {
		struct slot *s = &win[i % WINDOW];
		long best = -1;
		int type;

		if (s->who >= 0) {
			if (s->indexed)
				dindex_free(&s->ix);
			s->indexed = 0;
			s->who = -1;
		}
		if (c[i].size > MAX_DELTA_OBJECT || object_read(c[i].id, &type, &s->data))
			continue;
		s->who = (long)i;
		if (c[i].base || c[i].size < 64)
			continue;
		for (k = 1; k <= WINDOW && k <= i; k++) {
			struct slot *bs = &win[(i - k) % WINDOW];
			const struct cand *b = &c[i - k];
			size_t max, diff;

			if (bs->who != (long)(i - k) || b->type != c[i].type || b->depth >= MAX_DEPTH)
				continue;
			if (c[i].size < b->size / 32)
				continue;
			max = best >= 0 ? c[i].delta.len - 1 : c[i].size / 2 - 20;
			diff = b->size > c[i].size ? 0 : c[i].size - b->size;
			if (diff >= max)
				continue;
			if (!bs->indexed) {
				dindex_build(&bs->ix, bs->data.p, bs->data.len);
				bs->indexed = 1;
			}
			if (delta_make(&bs->ix, s->data.p, s->data.len, max, &d))
				continue;
			best = (long)(i - k);
			c[i].delta.len = 0;
			buf_add(&c[i].delta, d.p, d.len);
		}
		if (best >= 0) {
			c[i].delta_of = best;
			c[i].depth = c[best].depth + 1;
		} else {
			buf_free(&c[i].delta);
		}
	}
	for (k = 0; k < WINDOW; k++) {
		if (win[k].indexed)
			dindex_free(&win[k].ix);
		buf_free(&win[k].data);
	}
	buf_free(&d);
}

/* ---- the pack ---- */

struct out {
	int fd;
	const char *what;
	buf b;
	sha1_ctx sha;
	uint64_t off;
};

static void out_add(struct out *o, const void *d, size_t n)
{
	sha1_update(&o->sha, d, n);
	buf_add(&o->b, d, n);
	o->off += n;
	if (o->b.len >= 1 << 20) {
		write_all(o->fd, o->b.p, o->b.len, o->what);
		o->b.len = 0;
	}
}

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

struct writer {
	struct out o;
	struct cand *c;
	size_t *at;		/* by sorted index: 1 + the position in the send list, 0 for a base */
	uint64_t *off;		/* by sorted index: where it was written, once it was */
	char *done;
	int ofs;
	buf z, data;
};

static void write_one(struct writer *w, size_t i)
{
	struct cand *c = &w->c[i];
	unsigned char h[32];
	size_t k = 0, size;
	int type, t;

	if (w->done[i])
		return;
	w->done[i] = 1;
	if (c->delta_of >= 0 && !w->c[c->delta_of].base)
		write_one(w, (size_t)c->delta_of);
	if (c->delta_of >= 0) {
		const struct cand *b = &w->c[c->delta_of];

		t = !b->base && w->ofs ? 6 : 7;
		size = c->delta.len;
		h[k++] = (unsigned char)(t << 4 | (size & 15) | (size > 15 ? 0x80 : 0));
		for (size >>= 4; size; size >>= 7)
			h[k++] = (unsigned char)((size & 0x7f) | (size > 0x7f ? 0x80 : 0));
		if (t == 6) {
			uint64_t rel = w->o.off - w->off[c->delta_of];
			unsigned char d[10];
			int pos = 9;

			d[pos] = (unsigned char)(rel & 127);
			while (rel >>= 7)
				d[--pos] = (unsigned char)(128 | (--rel & 127));
			memcpy(h + k, d + pos, (size_t)(10 - pos));
			k += (size_t)(10 - pos);
		} else {
			memcpy(h + k, b->id, 20);
			k += 20;
		}
		w->z.len = 0;
		zlib_compress(&w->z, c->delta.p, c->delta.len);
		buf_free(&c->delta);
	} else {
		object_read_or_die(c->id, &type, &w->data);
		size = w->data.len;
		h[k++] = (unsigned char)(type << 4 | (size & 15) | (size > 15 ? 0x80 : 0));
		for (size >>= 4; size; size >>= 7)
			h[k++] = (unsigned char)((size & 0x7f) | (size > 0x7f ? 0x80 : 0));
		w->z.len = 0;
		zlib_compress(&w->z, w->data.p, w->data.len);
		buf_free(&w->data);
	}
	w->off[i] = w->o.off;
	out_add(&w->o, h, k);
	out_add(&w->o, w->z.p, w->z.len);
}

/*
 * Streams to fd one pack of objs, with deltas against each other and against the trees and files of edges (commits
 * the far side has); ofs: it takes OFS_DELTA. Says on stderr what it sent.
 */
void pack_send_thin(int fd, const char *what, const struct objlist *objs, const unsigned char (*edges)[20], size_t nedges,
		    int ofs)
{
	struct names nm = { 0 }, bn = { 0 };
	struct idset want = { 0 }, dirs = { 0 };
	struct cand *c;
	struct writer w;
	size_t i, n = 0, ndelta = 0, nthin = 0, budget = MAX_BASES, *by_send, *ord;
	unsigned char hdr[12], sum[20];

	if (objs->n > UINT32_MAX)
		die("too many objects for one pack");
	for (i = 0; i < objs->n; i++)
		idset_add(&want, objs->v[i].id);
	for (i = 0; i < objs->n; i++) {
		struct commit cm;
		buf b = { 0 };
		int type;

		if (objs->v[i].type != OBJ_COMMIT)
			continue;
		object_read_or_die(objs->v[i].id, &type, &b);
		commit_parse(&b, &cm);
		if (idset_has(&want, cm.tree))
			name_add(&nm, cm.tree, "");
		name_tree(&nm, cm.tree, "", &want, NULL, &budget);
		free(cm.parent);
		buf_free(&b);
	}
	for (i = 0; i < nm.n; i++) {
		unsigned char key[20];
		size_t len = strlen(nm.path[i]);

		object_hash(OBJ_BLOB, nm.path[i], len, key);
		idset_add(&dirs, key);
	}
	for (i = 0; i < nedges && i < MAX_EDGES; i++) {
		unsigned char tree[20];
		buf b = { 0 };
		struct commit cm;
		int type;

		if (object_read(edges[i], &type, &b) || type != OBJ_COMMIT) {
			buf_free(&b);
			continue;
		}
		commit_parse(&b, &cm);
		memcpy(tree, cm.tree, 20);
		free(cm.parent);
		buf_free(&b);
		if (!idset_has(&want, tree) && budget) {
			budget--;
			name_add(&bn, tree, "");
		}
		name_tree(&bn, tree, "", NULL, &dirs, &budget);
	}

	c = calloc(objs->n + bn.n + 1, sizeof *c);
	ord = xmalloc((nm.n + 1) * sizeof *ord);
	if (!c)
		die("out of memory");
	for (i = 0; i < nm.n; i++)
		ord[i] = i;
	sort_names = &nm;
	qsort(ord, nm.n, sizeof *ord, by_name_id);
	for (i = 0; i < objs->n; i++, n++) {
		buf b = { 0 };
		int type;

		memcpy(c[n].id, objs->v[i].id, 20);
		c[n].order = i;
		c[n].delta_of = -1;
		object_read_or_die(c[n].id, &type, &b);
		c[n].type = type;
		c[n].size = b.len;
		buf_free(&b);
		c[n].hash = name_of(&nm, ord, c[n].id);
	}
	for (i = 0; i < bn.n; i++) {
		buf b = { 0 };
		int type;

		if (idset_has(&want, bn.id[i]) || object_read(bn.id[i], &type, &b)) {
			buf_free(&b);
			continue;
		}
		memcpy(c[n].id, bn.id[i], 20);
		c[n].base = 1;
		c[n].type = type;
		c[n].size = b.len;
		c[n].hash = bn.hash[i];
		c[n].order = objs->n + i;
		c[n++].delta_of = -1;
		buf_free(&b);
	}
	qsort(c, n, sizeof *c, cand_cmp);
	find_deltas(c, n);

	memset(&w, 0, sizeof w);
	w.o.fd = fd;
	w.o.what = what;
	sha1_init(&w.o.sha);
	w.c = c;
	w.ofs = ofs;
	w.off = calloc(n + 1, sizeof *w.off);
	w.done = calloc(n + 1, 1);
	by_send = calloc(objs->n + 1, sizeof *by_send);
	if (!w.off || !w.done || !by_send)
		die("out of memory");
	for (i = 0; i < n; i++)
		if (!c[i].base)
			by_send[c[i].order] = i;
	memcpy(hdr, "PACK", 4);
	put32(hdr + 4, 2);
	put32(hdr + 8, (uint32_t)objs->n);
	out_add(&w.o, hdr, 12);
	for (i = 0; i < objs->n; i++) {
		const struct cand *x = &c[by_send[i]];

		if (x->delta_of >= 0) {
			ndelta++;
			nthin += c[x->delta_of].base;
		}
		write_one(&w, by_send[i]);
	}
	sha1_final(&w.o.sha, sum);
	buf_add(&w.o.b, sum, 20);
	w.o.off += 20;
	write_all(fd, w.o.b.p, w.o.b.len, what);
	fprintf(stderr, "Total %zu (delta %zu, %zu of them on objects the remote has), %llu bytes\n", objs->n, ndelta, nthin,
		(unsigned long long)w.o.off);

	buf_free(&w.o.b);
	buf_free(&w.z);
	free(w.off);
	free(w.done);
	free(by_send);
	free(ord);
	for (i = 0; i < n; i++)
		buf_free(&c[i].delta);
	free(c);
	for (i = 0; i < nm.n; i++)
		free(nm.path[i]);
	for (i = 0; i < bn.n; i++)
		free(bn.path[i]);
	free(nm.id);
	free(nm.hash);
	free(nm.path);
	free(bn.id);
	free(bn.hash);
	free(bn.path);
	idset_free(&nm.ids);
	idset_free(&bn.ids);
	idset_free(&want);
	idset_free(&dirs);
}
