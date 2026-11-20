#define _DEFAULT_SOURCE
#include "mgit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

struct file *files_add(struct files *f, const char *path, unsigned mode, const unsigned char id[20])
{
	struct file *e;

	if (f->n == f->cap) {
		f->cap = f->cap ? f->cap * 2 : 64;
		f->v = xrealloc(f->v, f->cap * sizeof *f->v);
	}
	e = &f->v[f->n++];
	memset(e, 0, sizeof *e);
	e->path = xstrdup(path);
	e->mode = mode;
	memcpy(e->id, id, 20);
	return e;
}

static int cmp_file(const void *a, const void *b)
{
	return strcmp(((const struct file *)a)->path, ((const struct file *)b)->path);
}

void files_sort(struct files *f)
{
	if (f->n)
		qsort(f->v, f->n, sizeof *f->v, cmp_file);
}

static size_t lower_bound(const struct files *f, const char *key)
{
	size_t lo = 0, hi = f->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (strcmp(f->v[mid].path, key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

struct file *files_find(const struct files *f, const char *path)
{
	size_t i = lower_bound(f, path);

	return i < f->n && !strcmp(f->v[i].path, path) ? &f->v[i] : NULL;
}

int files_have_prefix(const struct files *f, const char *prefix)
{
	size_t i = lower_bound(f, prefix);

	return i < f->n && !strncmp(f->v[i].path, prefix, strlen(prefix));
}

/* Pairs up the paths whose entries differ; a side is NULL where the path is absent. */
size_t files_diff(const struct files *a, const struct files *b, struct pair **out)
{
	size_t i = 0, j = 0, n = 0;
	struct pair *v = NULL;

	while (i < a->n || j < b->n) {
		int c = i == a->n ? 1 : j == b->n ? -1 : strcmp(a->v[i].path, b->v[j].path);
		struct pair p = { NULL, NULL };

		if (c <= 0)
			p.a = &a->v[i++];
		if (c >= 0)
			p.b = &b->v[j++];
		if (p.a && p.b && p.a->mode == p.b->mode && !memcmp(p.a->id, p.b->id, 20))
			continue;
		v = xrealloc(v, (n + 1) * sizeof *v);
		v[n++] = p;
	}
	*out = v;
	return n;
}

void files_free(struct files *f)
{
	size_t i;

	for (i = 0; i < f->n; i++)
		free(f->v[i].path);
	free(f->v);
	memset(f, 0, sizeof *f);
}

unsigned mode_from_stat(const struct stat *st)
{
	if (S_ISREG(st->st_mode))
		return st->st_mode & S_IXUSR ? 0100755 : 0100644;
	if (S_ISLNK(st->st_mode))
		return 0120000;
	return 0;
}

void stat_data_from(struct stat_data *sd, const struct stat *st)
{
	sd->ctime_s = (uint32_t)st->st_ctim.tv_sec;
	sd->ctime_ns = (uint32_t)st->st_ctim.tv_nsec;
	sd->mtime_s = (uint32_t)st->st_mtim.tv_sec;
	sd->mtime_ns = (uint32_t)st->st_mtim.tv_nsec;
	sd->dev = (uint32_t)st->st_dev;
	sd->ino = (uint32_t)st->st_ino;
	sd->uid = (uint32_t)st->st_uid;
	sd->gid = (uint32_t)st->st_gid;
	sd->size = (uint32_t)st->st_size;
}

/* True when st vouches for e->id without reading the file. stamp is the index file's mtime. */
int stat_matches(const struct file *e, const struct stat *st, const uint32_t stamp[2])
{
	struct stat_data sd;

	if (!e->have_stat || e->mode != mode_from_stat(st))
		return 0;
	stat_data_from(&sd, st);
	if (sd.mtime_s != e->sd.mtime_s || sd.mtime_ns != e->sd.mtime_ns || sd.ctime_s != e->sd.ctime_s ||
	    sd.ctime_ns != e->sd.ctime_ns || sd.ino != e->sd.ino || sd.uid != e->sd.uid || sd.gid != e->sd.gid ||
	    sd.size != e->sd.size)
		return 0;
	/* Racily clean: changed no earlier than the index was written, so equal stat data proves nothing. */
	return stamp[0] > sd.mtime_s || (stamp[0] == sd.mtime_s && stamp[1] > sd.mtime_ns);
}

static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(buf *b, uint32_t v)
{
	unsigned char t[4];

	t[0] = (unsigned char)(v >> 24);
	t[1] = (unsigned char)(v >> 16);
	t[2] = (unsigned char)(v >> 8);
	t[3] = (unsigned char)v;
	buf_add(b, t, 4);
}

/* Reads index versions 2 and 3. Returns -1 when there is no index. */
int index_read(struct files *out, uint32_t stamp[2])
{
	static const unsigned char zero[20];
	char *path = path_join(git_dir, "index"), *name;
	const unsigned char *p, *end;
	unsigned char check[20];
	uint32_t version, count, i;
	struct stat st;
	sha1_ctx c;
	buf b = { 0 };

	/* The stamp is taken before reading, so a concurrent rewrite can only make entries look racy. */
	if (stat(path, &st)) {
		if (errno != ENOENT)
			die("cannot stat %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	stamp[0] = (uint32_t)st.st_mtim.tv_sec;
	stamp[1] = (uint32_t)st.st_mtim.tv_nsec;
	if (read_file(path, &b))
		die("cannot read %s: %s", path, strerror(errno));
	if (b.len < 32 || memcmp(b.p, "DIRC", 4))
		die("%s is not an index file", path);
	version = get32(b.p + 4);
	count = get32(b.p + 8);
	if (version != 2 && version != 3)
		die("%s has index version %u; mgit reads versions 2 and 3", path, version);
	sha1_init(&c);
	sha1_update(&c, b.p, b.len - 20);
	sha1_final(&c, check);
	if (memcmp(b.p + b.len - 20, zero, 20) && memcmp(b.p + b.len - 20, check, 20))
		die("%s is corrupt (checksum mismatch)", path);
	p = b.p + 12;
	end = b.p + b.len - 20;
	for (i = 0; i < count; i++) {
		size_t fixed = 62, len, size;
		unsigned flags, mode;
		struct file *e;

		if (end - p < 62)
			die("%s is corrupt (truncated entry)", path);
		flags = (unsigned)p[60] << 8 | p[61];
		if (flags & 0x4000)
			fixed = 64;
		len = flags & 0xfff;
		if (len == 0xfff) {
			const unsigned char *z = (size_t)(end - p) > fixed ? memchr(p + fixed, 0, (size_t)(end - p) - fixed) : NULL;

			if (!z)
				die("%s is corrupt (unterminated path)", path);
			len = (size_t)(z - p) - fixed;
		}
		size = (fixed + len + 8) & ~(size_t)7;
		if ((size_t)(end - p) < size)
			die("%s is corrupt (truncated entry)", path);
		if (flags & 0x3000)
			die("the index has unmerged entries; resolve them with git first");
		if ((flags & 0x4000) && (p[62] & 0x40))
			die("the index has skip-worktree entries (a sparse checkout), which mgit does not support");
		mode = get32(p + 24);
		name = xstrndup((const char *)p + fixed, len);
		if (mode != 0100644 && mode != 0100755 && mode != 0120000 && mode != 0160000)
			die("%s has an entry with unknown mode %o (%s)", path, mode, name);
		if (out->n && strcmp(out->v[out->n - 1].path, name) >= 0)
			die("%s is corrupt (entries out of order at %s)", path, name);
		e = files_add(out, name, mode, p + 40);
		free(name);
		e->have_stat = 1;
		e->sd.ctime_s = get32(p);
		e->sd.ctime_ns = get32(p + 4);
		e->sd.mtime_s = get32(p + 8);
		e->sd.mtime_ns = get32(p + 12);
		e->sd.dev = get32(p + 16);
		e->sd.ino = get32(p + 20);
		e->sd.uid = get32(p + 28);
		e->sd.gid = get32(p + 32);
		e->sd.size = get32(p + 36);
		p += size;
	}
	/* Extensions starting with an uppercase letter are optional caches; anything else changes the meaning. */
	while (p < end) {
		uint32_t size;

		if (end - p < 8)
			die("%s is corrupt (truncated extension)", path);
		size = get32(p + 4);
		if (*p < 'A' || *p > 'Z')
			die("%s uses index extension '%.4s', which mgit does not support", path, (const char *)p);
		if ((size_t)(end - p) - 8 < size)
			die("%s is corrupt (truncated extension)", path);
		p += 8 + size;
	}
	buf_free(&b);
	free(path);
	return 0;
}

/* Writes a version 2 index with no extensions; entries without stat data get zeros, which forces a re-check. */
void index_write(const struct files *f)
{
	char *path = path_join(git_dir, "index"), *lock = xmalloc(strlen(path) + 6);
	unsigned char sum[20];
	sha1_ctx c;
	buf b = { 0 };
	size_t i;
	int fd;

	sprintf(lock, "%s.lock", path);
	fd = lock_take(lock);
	if (fd < 0 && errno == EEXIST)
		die("%s exists: another git or mgit is running, or the lock is stale", lock);
	if (fd < 0)
		die("cannot create %s: %s", lock, strerror(errno));
	buf_add(&b, "DIRC", 4);
	put32(&b, 2);
	put32(&b, (uint32_t)f->n);
	for (i = 0; i < f->n; i++) {
		const struct file *e = &f->v[i];
		struct stat_data sd = { 0 };
		size_t len = strlen(e->path);
		unsigned char flags[2];

		if (e->have_stat)
			sd = e->sd;
		put32(&b, sd.ctime_s);
		put32(&b, sd.ctime_ns);
		put32(&b, sd.mtime_s);
		put32(&b, sd.mtime_ns);
		put32(&b, sd.dev);
		put32(&b, sd.ino);
		put32(&b, e->mode);
		put32(&b, sd.uid);
		put32(&b, sd.gid);
		put32(&b, sd.size);
		buf_add(&b, e->id, 20);
		flags[0] = (unsigned char)((len < 0xfff ? len : 0xfff) >> 8);
		flags[1] = (unsigned char)(len < 0xfff ? len : 0xfff);
		buf_add(&b, flags, 2);
		buf_add(&b, e->path, len);
		buf_add(&b, "\0\0\0\0\0\0\0\0", 8 - (62 + len) % 8);
	}
	sha1_init(&c);
	sha1_update(&c, b.p, b.len);
	sha1_final(&c, sum);
	buf_add(&b, sum, 20);
	write_all(fd, b.p, b.len, lock);
	if (fsync(fd) || close(fd))
		die("cannot write %s: %s", lock, strerror(errno));
	if (rename(lock, path))
		die("cannot rename %s: %s", lock, strerror(errno));
	lock_done();
	buf_free(&b);
	free(lock);
	free(path);
}
