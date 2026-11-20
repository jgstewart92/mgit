#define _DEFAULT_SOURCE
#include "mgit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Packs are mapped read-only and found through their idx v2 files; idx v1 (git before 1.5.2) is refused. */

struct pack {
	char *name;
	const unsigned char *idx, *data;
	size_t idx_len, data_len;
	uint32_t n;
};

struct repo_packs {
	char *dir;
	struct pack *v;
	size_t n;
};

/* sync.c switches git_dir between two repositories, so packs are kept per repository. */
static struct repo_packs *repos;
static size_t nrepos;

static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static const unsigned char *map_file(const char *path, size_t *len)
{
	struct stat st;
	void *p;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0 || fstat(fd, &st))
		die("cannot open %s: %s", path, strerror(errno));
	*len = (size_t)st.st_size;
	p = *len ? mmap(NULL, *len, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	if (p == MAP_FAILED)
		die("cannot map %s: %s", path, *len ? strerror(errno) : "empty file");
	close(fd);
	return p;
}

static void load_pack(struct repo_packs *r, const char *dir, const char *idxname)
{
	char *ipath = path_join(dir, idxname), *ppath;
	size_t stem = strlen(ipath) - 4;
	struct pack p;
	uint32_t i, prev = 0;

	ppath = xmalloc(stem + 6);
	memcpy(ppath, ipath, stem);
	strcpy(ppath + stem, ".pack");
	if (access(ppath, F_OK)) {
		free(ipath);
		free(ppath);
		return;
	}
	p.name = ppath;
	p.idx = map_file(ipath, &p.idx_len);
	p.data = map_file(ppath, &p.data_len);
	if (p.idx_len < 8 + 1024 + 40 || memcmp(p.idx, "\377tOc", 4) || get32(p.idx + 4) != 2)
		die("%s: not an index version 2 (mgit reads only that)", ipath);
	for (i = 0; i < 256; i++) {
		uint32_t f = get32(p.idx + 8 + 4 * i);

		if (f < prev)
			die("%s is corrupt (fan-out table)", ipath);
		prev = f;
	}
	p.n = prev;
	if ((p.idx_len - 8 - 1024 - 40) / 28 < p.n)
		die("%s is corrupt (too short)", ipath);
	if (p.data_len < 32 || memcmp(p.data, "PACK", 4) || (get32(p.data + 4) != 2 && get32(p.data + 4) != 3) ||
	    get32(p.data + 8) != p.n)
		die("%s does not match its index", ppath);
	r->v = xrealloc(r->v, (r->n + 1) * sizeof *r->v);
	r->v[r->n++] = p;
	free(ipath);
}

static struct repo_packs *packs(void)
{
	char *dir;
	DIR *d;
	struct dirent *de;
	struct repo_packs *r;
	size_t i;

	for (i = 0; i < nrepos; i++)
		if (!strcmp(repos[i].dir, git_dir))
			return &repos[i];
	repos = xrealloc(repos, (nrepos + 1) * sizeof *repos);
	r = &repos[nrepos++];
	r->dir = xstrdup(git_dir);
	r->v = NULL;
	r->n = 0;
	dir = path_join(git_dir, "objects/pack");
	if ((d = opendir(dir))) {
		while ((de = readdir(d))) {
			size_t n = strlen(de->d_name);

			if (n > 4 && !strcmp(de->d_name + n - 4, ".idx"))
				load_pack(r, dir, de->d_name);
		}
		closedir(d);
	}
	free(dir);
	return r;
}

static uint64_t entry_offset(const struct pack *p, uint32_t i)
{
	const unsigned char *offs = p->idx + 8 + 1024 + (size_t)p->n * 24, *large = offs + (size_t)p->n * 4;
	uint32_t o = get32(offs + 4 * (size_t)i);

	if (!(o & 0x80000000u))
		return o;
	o &= 0x7fffffffu;
	if ((size_t)(p->idx + p->idx_len - 40 - large) / 8 <= o)
		die("%s.idx is corrupt (large offset)", p->name);
	return (uint64_t)get32(large + 8 * (size_t)o) << 32 | get32(large + 8 * (size_t)o + 4);
}

/* Range [*lo, *hi) of index entries whose ids start with the first byte of key. */
static void bucket(const struct pack *p, unsigned char first, uint32_t *lo, uint32_t *hi)
{
	*lo = first ? get32(p->idx + 8 + 4 * (first - 1)) : 0;
	*hi = get32(p->idx + 8 + 4 * first);
}

static const unsigned char *entry_id(const struct pack *p, uint32_t i)
{
	return p->idx + 8 + 1024 + 20 * (size_t)i;
}

static int find(const struct pack *p, const unsigned char id[20], uint64_t *off)
{
	uint32_t lo, hi;

	bucket(p, id[0], &lo, &hi);
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		int c = memcmp(entry_id(p, mid), id, 20);

		if (!c) {
			*off = entry_offset(p, mid);
			return 1;
		}
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

/* Delta bases are cached by (pack, offset), since many objects share the start of a chain. */
#define CACHE_SLOTS 256
#define CACHE_MAX_OBJECT (1u << 20)
static struct {
	const struct pack *p;
	uint64_t off;
	int type;
	buf b;
} cache[CACHE_SLOTS];

static int cache_get(const struct pack *p, uint64_t off, int *type, buf *out)
{
	size_t s = (size_t)(off * 2654435761u) % CACHE_SLOTS;

	if (cache[s].p != p || cache[s].off != off)
		return 0;
	*type = cache[s].type;
	out->len = 0;
	buf_add(out, cache[s].b.p, cache[s].b.len);
	return 1;
}

static void cache_put(const struct pack *p, uint64_t off, int type, const buf *b)
{
	size_t s = (size_t)(off * 2654435761u) % CACHE_SLOTS;

	if (b->len > CACHE_MAX_OBJECT)
		return;
	cache[s].p = p;
	cache[s].off = off;
	cache[s].type = type;
	cache[s].b.len = 0;
	buf_add(&cache[s].b, b->p, b->len);
}

static uint64_t delta_size(const unsigned char **pp, const unsigned char *end)
{
	const unsigned char *p = *pp;
	uint64_t v = 0;
	int shift = 0;
	unsigned char c;

	do {
		if (p >= end || shift > 63)
			return UINT64_MAX;
		c = *p++;
		v |= (uint64_t)(c & 0x7f) << shift;
		shift += 7;
	} while (c & 0x80);
	*pp = p;
	return v;
}

/* Rebuilds an object from its base and a git delta: copy-from-base and insert-literal instructions. */
static int apply_delta(const buf *base, const buf *delta, buf *out)
{
	const unsigned char *p = delta->p, *end = delta->p + delta->len;
	uint64_t src = delta_size(&p, end), dst = delta_size(&p, end);

	/* each instruction is at least one byte and gives at most 0xFFFFFF (a copy's three size bytes), so a larger dst cannot be met: refuse it before allocating */
	if (src != base->len || dst == UINT64_MAX || dst > SIZE_MAX - 1 || dst / 0xFFFFFF > (uint64_t)(end - p))
		return -1;
	out->len = 0;
	buf_grow(out, (size_t)dst);
	out->p[0] = 0;	/* NUL-terminated even when nothing is added (dst 0) */
	while (p < end) {
		unsigned char c = *p++;

		if (c & 0x80) {
			uint64_t off = 0, size = 0;
			int i;

			for (i = 0; i < 4; i++)
				if (c & (1 << i)) {
					if (p >= end)
						return -1;
					off |= (uint64_t)*p++ << (8 * i);
				}
			for (i = 0; i < 3; i++)
				if (c & (0x10 << i)) {
					if (p >= end)
						return -1;
					size |= (uint64_t)*p++ << (8 * i);
				}
			if (!size)
				size = 0x10000;
			if (off > base->len || size > base->len - off || size > dst - out->len)
				return -1;
			buf_add(out, base->p + off, (size_t)size);
		} else if (c) {
			if ((size_t)(end - p) < c || c > dst - out->len)
				return -1;
			buf_add(out, p, c);
			p += c;
		} else {
			return -1;
		}
	}
	return out->len == dst ? 0 : -1;
}

/*
 * A pack being indexed (pack_index) has no idx yet: its REF_DELTA bases are found among its own entries whose ids are
 * known so far, sorted by id in ix.by_id.
 */
struct entry {
	uint64_t off, end;	/* the entry's bytes in the pack: [off, end) */
	unsigned char id[20];
	int type;		/* 0 until the object is rebuilt */
	uint32_t crc;
};

static struct {
	const struct pack *p;
	struct entry *v;
	size_t *by_id, nby;
} ix;

static int by_entry_id(const void *a, const void *b)
{
	return memcmp(ix.v[*(const size_t *)a].id, ix.v[*(const size_t *)b].id, 20);
}

static int ix_find(const unsigned char id[20], uint64_t *off)
{
	size_t lo = 0, hi = ix.nby;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = memcmp(ix.v[ix.by_id[mid]].id, id, 20);

		if (!c) {
			*off = ix.v[ix.by_id[mid]].off;
			return 1;
		}
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

/* The inflated payload of the entry starting at q, which must be exactly size bytes. */
static int inflate_entry(const struct pack *p, const unsigned char *q, uint64_t size, buf *out)
{
	size_t used;

	out->len = 0;
	return zlib_inflate_prefix(out, q, (size_t)(p->data + p->data_len - 20 - q), (size_t)size, &used) ||
	       out->len != size ? -1 : 0;
}

/* The pack that holds id, first in the list, and where; NULL when none does. */
static const struct pack *pack_find(const unsigned char id[20], uint64_t *off)
{
	struct repo_packs *r = packs();
	size_t i;

	for (i = 0; i < r->n; i++)
		if (find(&r->v[i], id, off))
			return &r->v[i];
	return NULL;
}

/* Git writes chains of at most 4095 deltas. A longer one is refused, and so ends a cycle of REF_DELTAs naming each other. */
#define CHAIN_MAX 10000

/* A delta met on the way down its chain, applied on the way back up. */
struct link {
	const struct pack *p;
	const unsigned char *q;		/* its instructions, deflated */
	uint64_t size;			/* their inflated size */
	uint64_t base_off;		/* its base, when that is in p at a known offset */
	unsigned char id[20];		/* its base, when named by id through the object store */
	int by_id;
};

/*
 * Rebuilds the object at off in a loop, so a chain costs heap and never stack, however it runs: OFS_DELTA, REF_DELTA,
 * from pack to pack. A base named by id is found as object_read finds it, loose first, and is held to its id.
 */
static int unpack(const struct pack *p, uint64_t off, int *type, buf *out)
{
	struct link *v = NULL;
	size_t n = 0, cap = 0;
	buf delta = { 0 }, next = { 0 };
	int r = -1, checked = 0;	/* checked: what out holds came from the cache or a loose object */

	for (;;) {
		const unsigned char *q, *end = p->data + p->data_len - 20;
		struct link *l;
		uint64_t size;
		unsigned char c;
		int t, shift = 4;

		if (off < 12 || off >= p->data_len - 20)
			goto out;
		q = p->data + off;
		c = *q++;
		t = (c >> 4) & 7;
		size = c & 15;
		while (c & 0x80) {
			if (q >= end || shift > 57)
				goto out;
			c = *q++;
			size |= (uint64_t)(c & 0x7f) << shift;
			shift += 7;
		}
		if (t >= OBJ_COMMIT && t <= OBJ_TAG) {
			*type = t;
			if (inflate_entry(p, q, size, out))
				goto out;
			break;
		}
		if ((t != 6 && t != 7) || n == CHAIN_MAX)
			goto out;
		if (n == cap) {
			cap = cap ? 2 * cap : 16;
			v = xrealloc(v, cap * sizeof *v);
		}
		l = &v[n++];
		l->p = p;
		l->size = size;
		l->by_id = 0;
		if (t == 6) {
			/* OFS_DELTA: base offset backwards, base-128 with an extra 1 added per continuation byte */
			uint64_t back;

			if (q >= end)
				goto out;
			c = *q++;
			back = c & 0x7f;
			while (c & 0x80) {
				if (q >= end || back >= (UINT64_MAX >> 8))
					goto out;
				c = *q++;
				back = ((back + 1) << 7) | (c & 0x7f);
			}
			if (!back || back > off)
				goto out;
			off -= back;
		} else {
			/* REF_DELTA: base named by id, possibly loose or in another pack */
			if (end - q < 20)
				goto out;
			memcpy(l->id, q, 20);
			q += 20;
			if (p == ix.p) {
				if (!ix_find(l->id, &off))
					goto out;
			} else {
				l->by_id = 1;
				l->q = q;
				if (!loose_read(l->id, type, out)) {
					checked = 1;
					break;
				}
				if (!(p = pack_find(l->id, &off)))
					goto out;
				continue;
			}
		}
		l->q = q;
		l->base_off = off;
		if (cache_get(p, off, type, out)) {
			checked = 1;
			break;
		}
	}
	for (; n; n--) {
		const struct link *l = &v[n - 1];
		buf t;

		if (!checked) {
			if (l->by_id)
				object_check(l->id, *type, out);
			else
				cache_put(l->p, l->base_off, *type, out);
		}
		checked = 0;
		if (inflate_entry(l->p, l->q, l->size, &delta) || apply_delta(out, &delta, &next))
			goto out;
		t = *out;
		*out = next;
		next = t;
	}
	r = 0;
out:
	free(v);
	buf_free(&delta);
	buf_free(&next);
	return r;
}

/* -1 when no pack holds id; dies when one does but the entry cannot be rebuilt. */
int pack_read(const unsigned char id[20], int *type, buf *out)
{
	uint64_t off;
	const struct pack *p = pack_find(id, &off);
	char hex[41];

	if (!p)
		return -1;
	if (unpack(p, off, type, out)) {
		id_to_hex(id, hex);
		die("corrupt object %s in %s", hex, p->name);
	}
	return 0;
}

int pack_has(const unsigned char id[20])
{
	struct repo_packs *r = packs();
	uint64_t off;
	size_t i;

	for (i = 0; i < r->n; i++)
		if (find(&r->v[i], id, &off))
			return 1;
	return 0;
}

/* Objects in all packs, duplicates counted again: the count git sizes its default abbreviation from. */
unsigned long pack_object_count(void)
{
	struct repo_packs *r = packs();
	unsigned long n = 0;
	size_t i;

	for (i = 0; i < r->n; i++)
		n += r->v[i].n;
	return n;
}

/* Calls found for each packed id that starts with the n lowercase hex digits (n >= 2). */
void pack_prefix(const char *hex, size_t n, void (*found)(const unsigned char id[20], void *ctx), void *ctx)
{
	struct repo_packs *r = packs();
	unsigned char first[20];
	char key[41];
	size_t i;

	memcpy(key, hex, 2);
	memset(key + 2, '0', 38);
	key[40] = 0;
	if (hex_to_id(key, first))
		return;
	for (i = 0; i < r->n; i++) {
		uint32_t lo, hi, j;

		bucket(&r->v[i], first[0], &lo, &hi);
		for (j = lo; j < hi; j++) {
			char h[41];

			id_to_hex(entry_id(&r->v[i], j), h);
			if (!memcmp(h, hex, n))
				found(entry_id(&r->v[i], j), ctx);
		}
	}
}

static int sum_ok(const unsigned char *d, size_t n, const unsigned char want[20])
{
	unsigned char got[20];
	sha1_ctx c;

	sha1_init(&c);
	sha1_update(&c, d, n);
	sha1_final(&c, got);
	return !memcmp(got, want, 20);
}

/*
 * Reads a pack just written back through its idx: the pack's and the idx's SHA-1s, the idx naming the pack's, the
 * count, and each object in want found, rebuilt and hash-checked. Returns -1 (saying why) on any failure.
 */
int pack_verify(const char *idx_path, const struct objlist *want)
{
	struct repo_packs *r = xmalloc(sizeof *r);
	const char *slash = strrchr(idx_path, '/');
	char *dir = xstrndup(idx_path, (size_t)(slash - idx_path));
	const struct pack *p;
	size_t i;

	r->dir = dir;
	r->v = NULL;
	r->n = 0;
	load_pack(r, dir, slash + 1);
	if (r->n != 1) {
		warning("%s: its pack is missing", idx_path);
		return -1;
	}
	p = &r->v[0];
	if (!sum_ok(p->data, p->data_len - 20, p->data + p->data_len - 20) ||
	    !sum_ok(p->idx, p->idx_len - 20, p->idx + p->idx_len - 20) ||
	    memcmp(p->idx + p->idx_len - 40, p->data + p->data_len - 20, 20)) {
		warning("%s: a checksum does not match", idx_path);
		return -1;
	}
	if (p->n != want->n) {
		warning("%s: %u objects, not %zu", idx_path, p->n, want->n);
		return -1;
	}
	for (i = 0; i < want->n; i++) {
		unsigned char check[20];
		uint64_t off;
		buf b = { 0 };
		int type;
		char hex[41];

		id_to_hex(want->v[i].id, hex);
		if (!find(p, want->v[i].id, &off) || unpack(p, off, &type, &b)) {
			warning("%s: object %s is missing or unreadable", idx_path, hex);
			return -1;
		}
		object_hash(type, b.p, b.len, check);
		buf_free(&b);
		if (memcmp(check, want->v[i].id, 20) || type != want->v[i].type) {
			warning("%s: object %s reads back as something else", idx_path, hex);
			return -1;
		}
	}
	return 0;
}

/*
 * For a pack pack_index has just checked object by object: the idx read back as a reader will use it, its own
 * SHA-1, the pack's SHA-1 in it, the count, and each object found through it at the offset it was found at. The
 * objects are not rebuilt and hashed a second time.
 */
static int idx_readback(const char *idx_path, const struct objlist *want, const uint64_t *offs)
{
	struct repo_packs r = { 0 };
	const char *slash = strrchr(idx_path, '/');
	char *dir = xstrndup(idx_path, (size_t)(slash - idx_path));
	const struct pack *p;
	size_t i;
	int bad = 0;

	load_pack(&r, dir, slash + 1);
	free(dir);
	if (r.n != 1) {
		warning("%s: its pack is missing", idx_path);
		return -1;
	}
	p = &r.v[0];
	if (!sum_ok(p->idx, p->idx_len - 20, p->idx + p->idx_len - 20) ||
	    memcmp(p->idx + p->idx_len - 40, p->data + p->data_len - 20, 20) || p->n != want->n) {
		warning("%s: its checksum, its pack's or its count is wrong", idx_path);
		bad = 1;
	}
	for (i = 0; i < want->n && !bad; i++) {
		uint64_t off;

		if (!find(p, want->v[i].id, &off) || off != offs[i]) {
			warning("%s: an object is not where the pack has it", idx_path);
			bad = 1;
		}
	}
	munmap((void *)p->idx, p->idx_len);
	munmap((void *)p->data, p->data_len);
	free(p->name);
	free(r.v);
	return bad ? -1 : 0;
}

static void cache_forget(const struct pack *p)
{
	size_t s;

	for (s = 0; s < CACHE_SLOTS; s++)
		if (cache[s].p == p)
			cache[s].p = NULL;
}

static const struct pack *indexing;	/* the pack pack_index has mapped, until it returns */

/*
 * Unmaps every pack this process has mapped, each repository's and one being indexed. On NFS a file removed while it
 * is still mapped stays behind as .nfsXXXX, and so does the directory holding it, so whatever removes pack files that
 * may be mapped (a refused pack, a clone that failed) calls this first.
 */
void pack_unmap_all(void)
{
	size_t i, k;

	for (i = 0; i < nrepos; i++) {
		for (k = 0; k < repos[i].n; k++) {
			munmap((void *)repos[i].v[k].idx, repos[i].v[k].idx_len);
			munmap((void *)repos[i].v[k].data, repos[i].v[k].data_len);
			free(repos[i].v[k].name);
		}
		free(repos[i].v);
		free(repos[i].dir);
	}
	free(repos);
	repos = NULL;
	nrepos = 0;
	if (indexing)
		munmap((void *)indexing->data, indexing->data_len);
	indexing = NULL;
	ix.p = NULL;
	for (i = 0; i < CACHE_SLOTS; i++)
		cache[i].p = NULL;
}

[[noreturn]] static void index_fail(const char *pack, const char *idx, const char *why)
{
	pack_unmap_all();
	unlink(pack);
	if (idx)
		unlink(idx);
	die("the pack that came is refused (%s); nothing of it is kept", why);
}

/*
 * A pack that came from elsewhere, written to tmp in objects/pack: its SHA-1 trailer checked, every entry inflated to
 * its stated size, every delta rebuilt from a base in the same pack (OFS_DELTA by offset, REF_DELTA by id; a thin
 * pack is refused), every object hashed. Then its idx v2, both renamed to pack-<sum>, the pair read back through the
 * idx (pack_verify) and the pack added to this repository's list. out receives every object in the pack. Dies,
 * removing the files, on anything wrong. Returns the kept pack's path (NULL for an empty pack, which is removed).
 */
char *pack_index(const char *tmp, struct objlist *out)
{
	char *kept = NULL;
	struct pack p = { 0 };
	struct entry *v;
	struct repo_packs *r;
	uint64_t off = 12, *offs;
	uint32_t i, n, *crcs, left, done;
	char *dir = path_join(git_dir, "objects/pack"), *tidx, hex[41], why[96];
	buf z = { 0 }, idx = { 0 }, name = { 0 };
	size_t k;
	struct stat st;
	int fd;

	r = packs();
	p.name = xstrdup(tmp);
	if (stat(tmp, &st) || st.st_size < 32)
		index_fail(tmp, NULL, "it is not a pack");
	p.data = map_file(tmp, &p.data_len);
	indexing = &p;
	if (p.data_len < 32 || memcmp(p.data, "PACK", 4) || (get32(p.data + 4) != 2 && get32(p.data + 4) != 3))
		index_fail(tmp, NULL, "it is not a pack");
	if (!sum_ok(p.data, p.data_len - 20, p.data + p.data_len - 20))
		index_fail(tmp, NULL, "its SHA-1 trailer does not match its content");
	n = p.n = get32(p.data + 8);
	if (n > p.data_len / 8)
		index_fail(tmp, NULL, "it claims more objects than it can hold");
	v = calloc(n ? n : 1, sizeof *v);
	if (!v)
		die("out of memory");
	for (i = 0; i < n; i++) {
		const unsigned char *q = p.data + off, *end = p.data + p.data_len - 20;
		uint64_t size;
		size_t used;
		unsigned char c;
		int t, shift = 4;

		if (q >= end)
			index_fail(tmp, NULL, "it is cut short");
		v[i].off = off;
		c = *q++;
		t = (c >> 4) & 7;
		size = c & 15;
		while (c & 0x80) {
			if (q >= end || shift > 57)
				index_fail(tmp, NULL, "an entry header is corrupt");
			c = *q++;
			size |= (uint64_t)(c & 0x7f) << shift;
			shift += 7;
		}
		if (t == 6) {
			uint64_t back;
			uint32_t lo = 0, hi = i;

			if (q >= end)
				index_fail(tmp, NULL, "an entry header is corrupt");
			c = *q++;
			back = c & 0x7f;
			while (c & 0x80) {
				if (q >= end || back >= (UINT64_MAX >> 8))
					index_fail(tmp, NULL, "an entry header is corrupt");
				c = *q++;
				back = ((back + 1) << 7) | (c & 0x7f);
			}
			if (!back || back > off)
				index_fail(tmp, NULL, "a delta's base lies outside the pack");
			while (lo < hi) {
				uint32_t mid = lo + (hi - lo) / 2;

				if (v[mid].off < off - back)
					lo = mid + 1;
				else
					hi = mid;
			}
			if (lo == i || v[lo].off != off - back)
				index_fail(tmp, NULL, "a delta's base offset is not the start of an entry");
		} else if (t == 7) {
			if (end - q < 20)
				index_fail(tmp, NULL, "it is cut short");
			q += 20;
		} else if (t < OBJ_COMMIT || t > OBJ_TAG) {
			index_fail(tmp, NULL, "an entry has an unknown type");
		}
		z.len = 0;
		if (zlib_inflate_prefix(&z, q, (size_t)(end - q), (size_t)size, &used) || z.len != size)
			index_fail(tmp, NULL, "an entry does not inflate to its stated size");
		v[i].end = (uint64_t)(q - p.data) + used;
		v[i].crc = crc32_update(0, p.data + off, (size_t)(v[i].end - off));
		if (t <= OBJ_TAG) {
			v[i].type = t;
			object_hash(t, z.p, z.len, v[i].id);
		}
		off = v[i].end;
	}
	if (off != p.data_len - 20)
		index_fail(tmp, NULL, "bytes follow its last object");

	/* deltas, in passes: a REF_DELTA's base is known once a pass has rebuilt it */
	ix.p = &p;
	ix.v = v;
	ix.by_id = xmalloc((n ? n : 1) * sizeof *ix.by_id);
	do {
		ix.nby = 0;
		for (i = 0; i < n; i++)
			if (v[i].type)
				ix.by_id[ix.nby++] = i;
		qsort(ix.by_id, ix.nby, sizeof *ix.by_id, by_entry_id);
		left = done = 0;
		for (i = 0; i < n; i++) {
			int type;

			if (v[i].type)
				continue;
			if (unpack(&p, v[i].off, &type, &z)) {
				left++;
				continue;
			}
			v[i].type = type;
			object_hash(type, z.p, z.len, v[i].id);
			done++;
		}
	} while (left && done);
	ix.p = NULL;
	cache_forget(&p);
	free(ix.by_id);
	if (left) {
		snprintf(why, sizeof why, "%u deltas have no base in it that rebuilds", left);
		index_fail(tmp, NULL, why);
	}
	if (!n)
		goto out;	/* an empty pack: nothing to keep, removed below once unmapped */

	offs = xmalloc(n * sizeof *offs);
	crcs = xmalloc(n * sizeof *crcs);
	for (i = 0; i < n; i++) {
		objlist_add(out, v[i].id, v[i].type);
		offs[i] = v[i].off;
		crcs[i] = v[i].crc;
	}
	idx_build(&idx, out, offs, crcs, p.data + p.data_len - 20);
	tidx = path_join(dir, "tmp_idx_XXXXXX");
	if ((fd = mkstemp(tidx)) < 0)
		index_fail(tmp, NULL, "no idx file could be made");
	write_all(fd, idx.p, idx.len, tidx);
	if (fchmod(fd, 0444) || fsync(fd) || close(fd))
		index_fail(tmp, tidx, "the idx could not be written");
	id_to_hex(p.data + p.data_len - 20, hex);
	buf_printf(&name, "%s/pack-%s.pack", dir, hex);
	{
		buf in = { 0 };
		int here;

		buf_printf(&in, "%s/pack-%s.idx", dir, hex);
		here = !access((char *)name.p, F_OK) && !access((char *)in.p, F_OK);
		if (!here) {
			/* until the caller knows what it brings connects, a signal removes the pair too */
			remove_if_killed(DOOM_PACK, (char *)name.p);
			remove_if_killed(DOOM_IDX, (char *)in.p);
		}
		buf_free(&in);
		if (here) {
			/* this very pack is here already (a pack is named by its checksum): that one stays as it is */
			unlink(tidx);
			unlink(tmp);
			free(offs);
			free(crcs);
			free(tidx);
			goto out;
		}
	}
	if (rename(tmp, (char *)name.p))
		index_fail(tmp, tidx, "the pack could not be renamed");
	name.len -= 5;
	buf_adds(&name, ".idx");
	if (rename(tidx, (char *)name.p)) {
		name.len -= 4;
		buf_adds(&name, ".pack");
		index_fail((char *)name.p, tidx, "the idx could not be renamed");
	}
	/* listed for this repository first, as every later read finds the pack through it */
	{
		buf pk = { 0 }, in = { 0 };

		buf_add(&pk, name.p, name.len - 4);
		buf_adds(&pk, ".pack");
		for (k = 0; k < r->n && strcmp(r->v[k].name, (char *)pk.p); k++)
			;
		if (k == r->n) {
			buf_printf(&in, "pack-%s.idx", hex);
			load_pack(r, dir, (char *)in.p);
		}
		if (idx_readback((char *)name.p, out, offs))
			index_fail((char *)pk.p, (char *)name.p, "it does not read back through its idx");
		kept = xstrdup((char *)pk.p);
		buf_free(&pk);
		buf_free(&in);
	}
	free(offs);
	free(crcs);
	free(tidx);
out:
	indexing = NULL;
	munmap((void *)p.data, p.data_len);
	if (!n)
		unlink(tmp);
	free(p.name);
	free(v);
	free(dir);
	buf_free(&z);
	buf_free(&idx);
	buf_free(&name);
	return kept;
}
