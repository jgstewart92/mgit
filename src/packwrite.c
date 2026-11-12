#define _DEFAULT_SOURCE
#include "mgit.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Writing packs: pack v2 with every object whole (no deltas, which git reads as any other pack), and its idx v2. */

uint32_t crc32_update(uint32_t crc, const void *d, size_t n)
{
	static uint32_t table[256];
	const unsigned char *p = d;

	if (!table[1]) {
		uint32_t i, k, c;

		for (i = 0; i < 256; i++) {
			for (c = i, k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	}
	crc = ~crc;
	while (n--)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static void put32(buf *b, uint32_t v)
{
	unsigned char c[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };

	buf_add(b, c, 4);
}

struct sink {
	int fd;
	const char *what;
	buf b;
	sha1_ctx sha;
	uint64_t off;
};

static void sink_add(struct sink *s, const void *d, size_t n)
{
	sha1_update(&s->sha, d, n);
	buf_add(&s->b, d, n);
	s->off += n;
	if (s->b.len >= 1 << 20) {
		write_all(s->fd, s->b.p, s->b.len, s->what);
		s->b.len = 0;
	}
}

/*
 * Streams a pack of the objects to fd: "PACK", version 2, the count, each object as its type-and-size header and its
 * deflated content, then the SHA-1 of all of it, which is also returned in sum. offs and crcs, when set, receive each
 * entry's offset and the CRC-32 of its bytes, for the idx.
 */
void pack_send(int fd, const char *what, const struct objlist *objs, uint64_t *offs, uint32_t *crcs, unsigned char sum[20])
{
	struct sink s = { fd, what, { 0 }, { { 0 }, 0, { 0 } }, 0 };
	buf hdr = { 0 }, data = { 0 }, z = { 0 };
	size_t i;

	if (objs->n > UINT32_MAX)
		die("too many objects for one pack");
	sha1_init(&s.sha);
	buf_adds(&hdr, "PACK");
	put32(&hdr, 2);
	put32(&hdr, (uint32_t)objs->n);
	sink_add(&s, hdr.p, hdr.len);
	for (i = 0; i < objs->n; i++) {
		unsigned char h[16];
		size_t size, k = 0;
		int type;

		object_read_or_die(objs->v[i].id, &type, &data);
		if (type != objs->v[i].type && objs->v[i].type) {
			char hex[41];

			id_to_hex(objs->v[i].id, hex);
			die("object %s is a %s, not a %s", hex, type_name(type), type_name(objs->v[i].type));
		}
		size = data.len;
		h[k++] = (unsigned char)(type << 4 | (size & 15) | (size > 15 ? 0x80 : 0));
		for (size >>= 4; size; size >>= 7)
			h[k++] = (unsigned char)((size & 0x7f) | (size > 0x7f ? 0x80 : 0));
		z.len = 0;
		zlib_compress(&z, data.p, data.len);
		if (offs)
			offs[i] = s.off;
		if (crcs)
			crcs[i] = crc32_update(crc32_update(0, h, k), z.p, z.len);
		sink_add(&s, h, k);
		sink_add(&s, z.p, z.len);
		buf_free(&data);
	}
	sha1_final(&s.sha, sum);
	buf_add(&s.b, sum, 20);
	write_all(fd, s.b.p, s.b.len, what);
	buf_free(&s.b);
	buf_free(&hdr);
	buf_free(&z);
}

static const struct objlist *sort_objs;

static int by_id(const void *a, const void *b)
{
	return memcmp(sort_objs->v[*(const size_t *)a].id, sort_objs->v[*(const size_t *)b].id, 20);
}

/* idx v2: fan-out, sorted ids, CRC-32s, 31-bit offsets with a table of 64-bit ones past 2 GiB, the pack's SHA-1, its own. */
void idx_build(buf *out, const struct objlist *objs, const uint64_t *offs, const uint32_t *crcs, const unsigned char sum[20])
{
	size_t *ord = xmalloc(objs->n * sizeof *ord), i, nlarge = 0;
	uint32_t fan[256] = { 0 };
	unsigned char check[20];
	sha1_ctx c;

	for (i = 0; i < objs->n; i++)
		ord[i] = i;
	sort_objs = objs;
	qsort(ord, objs->n, sizeof *ord, by_id);
	buf_add(out, "\377tOc", 4);
	put32(out, 2);
	for (i = 0; i < objs->n; i++)
		fan[objs->v[ord[i]].id[0]]++;
	for (i = 1; i < 256; i++)
		fan[i] += fan[i - 1];
	for (i = 0; i < 256; i++)
		put32(out, fan[i]);
	for (i = 0; i < objs->n; i++)
		buf_add(out, objs->v[ord[i]].id, 20);
	for (i = 0; i < objs->n; i++)
		put32(out, crcs[ord[i]]);
	for (i = 0; i < objs->n; i++)
		put32(out, offs[ord[i]] < 0x80000000u ? (uint32_t)offs[ord[i]] : 0x80000000u | (uint32_t)nlarge++);
	for (i = 0; i < objs->n; i++)
		if (offs[ord[i]] >= 0x80000000u) {
			put32(out, (uint32_t)(offs[ord[i]] >> 32));
			put32(out, (uint32_t)offs[ord[i]]);
		}
	buf_add(out, sum, 20);
	sha1_init(&c);
	sha1_update(&c, out->p, out->len);
	sha1_final(&c, check);
	buf_add(out, check, 20);
	free(ord);
}

static char *tmp_file(const char *dir, const char *stem, int *fd)
{
	char *t = path_join(dir, stem);

	if ((*fd = mkstemp(t)) < 0)
		die("cannot create %s: %s", t, strerror(errno));
	return t;
}

static void finish(int fd, const char *path)
{
	if (fchmod(fd, 0444) || fsync(fd) || close(fd))
		die("cannot write %s: %s", path, strerror(errno));
}

static int loose(const unsigned char id[20])
{
	char hex[41];
	buf p = { 0 };
	int r;

	id_to_hex(id, hex);
	buf_printf(&p, "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
	r = access((char *)p.p, F_OK) == 0;
	buf_free(&p);
	return r;
}

/*
 * gc: one new pack holding every loose object reachable from HEAD and the refs (objects already packed stay where
 * they are). The loose copies go only once the new pack and its idx read back right; loose objects nothing reaches
 * are left alone.
 */
int cmd_gc(int argc, char **argv)
{
	struct objlist all = { 0 }, todo = { 0 };
	struct idset seen = { 0 };
	unsigned char (*tips)[20] = NULL, sum[20], id[20];
	uint64_t *offs;
	uint32_t *crcs;
	char **names, *dir, *tpack, *tidx, hex[41];
	size_t n, i, ntips = 0, removed = 0;
	buf idx = { 0 }, name = { 0 };
	int fd, quiet = argc == 2 && (!strcmp(argv[1], "-q") || !strcmp(argv[1], "--quiet"));

	if (argc > 1 && !quiet)
		die("usage: mgit gc [-q]");
	repo_find();
	names = ref_list("refs/", &n);
	tips = xmalloc((n + 1) * sizeof *tips);
	if (!ref_read("HEAD", id))
		memcpy(tips[ntips++], id, 20);
	for (i = 0; i < n; i++) {
		name.len = 0;
		buf_printf(&name, "refs/%s", names[i]);
		if (!ref_read((char *)name.p, id))
			memcpy(tips[ntips++], id, 20);
		free(names[i]);
	}
	free(names);
	walk((const unsigned char (*)[20])tips, ntips, &seen, &all, 0);
	for (i = 0; i < all.n; i++)
		if (loose(all.v[i].id) && !pack_has(all.v[i].id))
			objlist_add(&todo, all.v[i].id, all.v[i].type);
	if (!todo.n) {
		if (!quiet)
			puts("nothing to pack: no loose object is reachable");
		return 0;
	}
	dir = path_join(git_dir, "objects/pack");
	if (mkdir(dir, 0777) && errno != EEXIST)
		die("cannot create %s: %s", dir, strerror(errno));
	offs = xmalloc(todo.n * sizeof *offs);
	crcs = xmalloc(todo.n * sizeof *crcs);
	tpack = tmp_file(dir, "tmp_pack_XXXXXX", &fd);
	pack_send(fd, tpack, &todo, offs, crcs, sum);
	finish(fd, tpack);
	idx_build(&idx, &todo, offs, crcs, sum);
	tidx = tmp_file(dir, "tmp_idx_XXXXXX", &fd);
	write_all(fd, idx.p, idx.len, tidx);
	finish(fd, tidx);
	id_to_hex(sum, hex);
	name.len = 0;
	buf_printf(&name, "%s/pack-%s.pack", dir, hex);
	if (rename(tpack, (char *)name.p))
		die("cannot rename %s: %s", tpack, strerror(errno));
	name.len -= 5;
	buf_adds(&name, ".idx");
	if (rename(tidx, (char *)name.p)) {
		int e = errno;

		unlink(tidx);
		name.len -= 4;
		buf_adds(&name, ".pack");
		unlink((char *)name.p);
		die("cannot rename %s: %s; the new pack is removed and every loose object is kept", tidx, strerror(e));
	}
	if (pack_verify((char *)name.p, &todo)) {
		unlink((char *)name.p);
		name.len -= 4;
		buf_adds(&name, ".pack");
		unlink((char *)name.p);
		die("the new pack did not read back right; it is removed and every loose object is kept");
	}
	for (i = 0; i < todo.n; i++) {
		buf p = { 0 };

		id_to_hex(todo.v[i].id, hex);
		buf_printf(&p, "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
		if (unlink((char *)p.p) && errno != ENOENT)
			die("cannot remove %s: %s", (char *)p.p, strerror(errno));
		removed++;
		p.len -= 39;
		p.p[p.len] = 0;
		rmdir((char *)p.p); /* only once empty, as git prune-packed does */
		buf_free(&p);
	}
	if (!quiet) {
		id_to_hex(sum, hex);
		printf("packed %zu objects into objects/pack/pack-%s.pack; removed %zu loose copies\n", todo.n, hex, removed);
	}
	idset_free(&seen);
	free(all.v);
	free(todo.v);
	free(offs);
	free(crcs);
	free(tips);
	buf_free(&idx);
	buf_free(&name);
	free(tpack);
	free(tidx);
	free(dir);
	return 0;
}
