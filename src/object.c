#define _DEFAULT_SOURCE
#include "mgit.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const names[] = { NULL, "commit", "tree", "blob", "tag" };

const char *type_name(int type)
{
	return type >= OBJ_COMMIT && type <= OBJ_TAG ? names[type] : "unknown";
}

int type_from_name(const char *s, size_t n)
{
	int t;

	for (t = OBJ_COMMIT; t <= OBJ_TAG; t++)
		if (strlen(names[t]) == n && !memcmp(names[t], s, n))
			return t;
	return 0;
}

/* "<type> <size>\0", returning its length including the NUL. */
static size_t header(char hdr[32], int type, size_t n)
{
	return (size_t)snprintf(hdr, 32, "%s %zu", type_name(type), n) + 1;
}

void object_hash(int type, const void *data, size_t n, unsigned char id[20])
{
	char hdr[32];
	size_t hl = header(hdr, type, n);
	sha1_ctx c;

	sha1_init(&c);
	sha1_update(&c, hdr, hl);
	sha1_update(&c, data, n);
	sha1_final(&c, id);
}

static char *object_path(const unsigned char id[20])
{
	char hex[41];
	buf b = { 0 };

	id_to_hex(id, hex);
	buf_printf(&b, "%s/objects/%.2s/%s", git_dir, hex, hex + 2);
	return (char *)b.p;
}

void object_write(int type, const void *data, size_t n, unsigned char id[20])
{
	char hdr[32], *path, *dir, *tmp;
	buf raw = { 0 }, z = { 0 };
	int fd;

	object_hash(type, data, n, id);
	path = object_path(id);
	if (access(path, F_OK) == 0 || pack_has(id)) {
		free(path);
		return;
	}
	buf_add(&raw, hdr, header(hdr, type, n));
	buf_add(&raw, data, n);
	zlib_compress(&z, raw.p, raw.len);
	buf_free(&raw);

	dir = xstrndup(path, strlen(path) - 39);
	if (mkdir(dir, 0777) && errno != EEXIST)
		die("cannot create %s: %s", dir, strerror(errno));
	tmp = path_join(dir, "tmp_obj_XXXXXX");
	fd = mkstemp(tmp);
	if (fd < 0)
		die("cannot create %s: %s", tmp, strerror(errno));
	write_all(fd, z.p, z.len, tmp);
	if (fchmod(fd, 0444) || fsync(fd) || close(fd))
		die("cannot write %s: %s", tmp, strerror(errno));
	if (rename(tmp, path))
		die("cannot rename %s to %s: %s", tmp, path, strerror(errno));
	buf_free(&z);
	free(tmp);
	free(dir);
	free(path);
}

/* Dies unless the content hashes to id. */
void object_check(const unsigned char id[20], int type, const buf *b)
{
	unsigned char check[20];
	char hex[41];

	object_hash(type, b->p, b->len, check);
	if (memcmp(check, id, 20)) {
		id_to_hex(id, hex);
		die("corrupt packed object %s: hash mismatch", hex);
	}
}

/* Returns -1 if no loose object has this id; dies if one does but is corrupt. out receives the content. */
int loose_read(const unsigned char id[20], int *type, buf *out)
{
	char *path = object_path(id);
	buf z = { 0 }, raw = { 0 };
	unsigned char *nul, *p, check[20];
	size_t size = 0, hl;
	sha1_ctx c;
	int t;

	if (read_file(path, &z)) {
		if (errno != ENOENT)
			die("cannot read %s: %s", path, strerror(errno));
		free(path);
		buf_free(&z);
		return -1;
	}
	if (zlib_inflate(&raw, z.p, z.len))
		die("corrupt object %s: bad zlib stream", path);
	buf_free(&z);
	nul = raw.len ? memchr(raw.p, 0, raw.len) : NULL;
	p = nul ? memchr(raw.p, ' ', (size_t)(nul - raw.p)) : NULL;
	t = p ? type_from_name((char *)raw.p, (size_t)(p - raw.p)) : 0;
	if (!t || p + 1 == nul)
		die("corrupt object %s: bad header", path);
	for (p++; p < nul; p++) {
		if (*p < '0' || *p > '9' || size > (SIZE_MAX - 9) / 10)
			die("corrupt object %s: bad header", path);
		size = size * 10 + (size_t)(*p - '0');
	}
	hl = (size_t)(nul - raw.p) + 1;
	if (raw.len - hl != size)
		die("corrupt object %s: size mismatch", path);
	sha1_init(&c);
	sha1_update(&c, raw.p, raw.len);
	sha1_final(&c, check);
	if (memcmp(check, id, 20))
		die("corrupt object %s: hash mismatch", path);
	memmove(raw.p, raw.p + hl, size);
	raw.len = size;
	raw.p[size] = 0;
	buf_free(out);
	*out = raw;
	*type = t;
	free(path);
	return 0;
}

/* Returns -1 if the object is absent; dies if it is present but corrupt. out receives the content. */
int object_read(const unsigned char id[20], int *type, buf *out)
{
	if (!loose_read(id, type, out))
		return 0;
	if (pack_read(id, type, out))
		return -1;
	object_check(id, *type, out);
	return 0;
}

void object_read_or_die(const unsigned char id[20], int *type, buf *out)
{
	char hex[41];

	if (!object_read(id, type, out))
		return;
	id_to_hex(id, hex);
	die("object %s not found", hex);
}

int object_exists(const unsigned char id[20])
{
	char *path = object_path(id);
	int r = access(path, F_OK) == 0;

	free(path);
	return r || pack_has(id);
}

struct hits {
	unsigned char first[20];
	int count;
};

/* An object can be both loose and packed, or in two packs; only a second distinct id makes a prefix ambiguous. */
static void hit(const unsigned char id[20], void *ctx)
{
	struct hits *h = ctx;

	if (h->count && !memcmp(h->first, id, 20))
		return;
	if (!h->count)
		memcpy(h->first, id, 20);
	h->count++;
}

/* hex is lowercase, 4 to 39 digits. Returns the number of distinct matching objects (2 means two or more); id is the first. */
int object_by_prefix(const char *hex, size_t n, unsigned char id[20])
{
	struct hits h = { { 0 }, 0 };
	buf path = { 0 };
	DIR *d;
	struct dirent *de;
	char full[41];
	unsigned char lid[20];

	buf_printf(&path, "%s/objects/%.2s", git_dir, hex);
	d = opendir((char *)path.p);
	buf_free(&path);
	while (d && (de = readdir(d))) {
		if (strlen(de->d_name) != 38 || memcmp(de->d_name, hex + 2, n - 2) || !is_hex(de->d_name, 38))
			continue;
		memcpy(full, hex, 2);
		memcpy(full + 2, de->d_name, 39);
		if (!hex_to_id(full, lid))
			hit(lid, &h);
	}
	if (d)
		closedir(d);
	pack_prefix(hex, n, hit, &h);
	memcpy(id, h.first, 20);
	return h.count > 1 ? 2 : h.count;
}
