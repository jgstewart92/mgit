#define _DEFAULT_SOURCE
#include "mgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Next "<mode> <name>\0<20-byte id>" entry of a tree object (the mode read in octal, as git reads it), or 0 at the end. */
int tree_next(const unsigned char **pp, const unsigned char *end, unsigned *mode, const char **name,
		     const unsigned char **id)
{
	const unsigned char *p = *pp, *sp, *nul, *q;

	if (p >= end)
		return 0;
	sp = memchr(p, ' ', (size_t)(end - p));
	nul = sp ? memchr(sp, 0, (size_t)(end - sp)) : NULL;
	if (!nul || end - nul < 21 || sp == p || nul == sp + 1)
		die("corrupt tree object");
	*mode = 0;
	for (q = p; q < sp; q++) {
		if (*q < '0' || *q > '7')
			die("corrupt tree object");
		*mode = *mode * 8 + (unsigned)(*q - '0');
	}
	*name = (const char *)sp + 1;
	*id = nul + 1;
	*pp = nul + 21;
	return 1;
}

/* f is sorted by path; each run sharing a "dir/" prefix becomes a subtree, which is exactly git's tree order. */
static void build(const struct files *f, size_t lo, size_t hi, size_t plen, unsigned char id[20])
{
	buf t = { 0 };
	size_t i = lo;

	while (i < hi) {
		const char *name = f->v[i].path + plen, *slash = strchr(name, '/');

		if (!slash) {
			buf_printf(&t, "%o %s", f->v[i].mode, name);
			buf_add(&t, "", 1);
			buf_add(&t, f->v[i].id, 20);
			i++;
		} else {
			size_t nl = (size_t)(slash - name), j = i + 1;
			unsigned char sub[20];

			while (j < hi && !strncmp(f->v[j].path + plen, name, nl + 1))
				j++;
			build(f, i, j, plen + nl + 1, sub);
			buf_printf(&t, "40000 %.*s", (int)nl, name);
			buf_add(&t, "", 1);
			buf_add(&t, sub, 20);
			i = j;
		}
	}
	object_write(OBJ_TREE, t.p, t.len, id);
	buf_free(&t);
}

void tree_build(const struct files *f, unsigned char id[20])
{
	build(f, 0, f->n, 0, id);
}

/* .git or git~1 in any case, then only dots and spaces up to the end, a ':' or a '\': git's is_ntfs_dotgit. */
static int dotgit_at(const char *p)
{
	if (p[0] == '.' && !strncasecmp(p + 1, "git", 3))
		p += 4;
	else if (!strncasecmp(p, "git~1", 5))
		p += 5;
	else
		return 0;
	p += strspn(p, ". ");
	return !*p || *p == ':' || *p == '\\';
}

/*
 * Whether git refuses name as a path component of an entry with this mode (verify_path, with core.protectNTFS on as by
 * default): a .git as NTFS reads one, at the start or after any '\', and a symlink named .gitmodules.
 */
int name_refused(const char *name, unsigned mode)
{
	const char *p = name;

	for (;;) {
		if (dotgit_at(p))
			return 1;
		if (!(p = strchr(p, '\\')))
			break;
		p++;
	}
	return mode == 0120000 && !strncasecmp(name, ".gitmodules", 11) && !name[11 + strspn(name + 11, ". ")];
}

static void flatten(const unsigned char id[20], const char *prefix, struct files *out, int depth)
{
	const unsigned char *p, *end, *eid;
	const char *name;
	unsigned mode;
	buf b = { 0 }, path = { 0 };
	int type;

	if (depth > TREE_DEPTH_MAX)
		die("trees are nested more than %d deep", TREE_DEPTH_MAX);
	object_read_or_die(id, &type, &b);
	if (type != OBJ_TREE)
		die("expected a tree, found a %s", type_name(type));
	p = b.p;
	end = b.p + b.len;
	while (tree_next(&p, end, &mode, &name, &eid)) {
		if (!strcmp(name, ".") || !strcmp(name, "..") || name_refused(name, mode) || strchr(name, '/'))
			die("tree contains an unsafe path: %s%s", prefix, name);
		path.len = 0;
		buf_adds(&path, prefix);
		buf_adds(&path, name);
		if (mode == 040000) {
			buf_addc(&path, '/');
			flatten(eid, (char *)path.p, out, depth + 1);
		} else if (mode == 0160000) {
			files_add(out, (char *)path.p, mode, eid); /* a submodule's commit: kept as it is, never fetched */
		} else if ((mode & 0170000) == 0100000) {
			files_add(out, (char *)path.p, mode & 0111 ? 0100755 : 0100644, eid);
		} else if (mode == 0120000) {
			files_add(out, (char *)path.p, mode, eid);
		} else {
			die("tree entry %s has unknown mode %o", (char *)path.p, mode);
		}
	}
	buf_free(&path);
	buf_free(&b);
}

void tree_flatten(const unsigned char tree[20], struct files *out)
{
	flatten(tree, "", out, 0);
	files_sort(out);
}

void tree_print(const buf *b)
{
	const unsigned char *p = b->p, *end = b->p + b->len, *id;
	const char *name;
	unsigned mode;
	buf line = { 0 };

	while (tree_next(&p, end, &mode, &name, &id)) {
		char hex[41];

		id_to_hex(id, hex);
		line.len = 0;
		buf_printf(&line, "%06o %s %s\t", mode, mode == 040000 ? "tree" : mode == 0160000 ? "commit" : "blob", hex);
		buf_quote(&line, "", name);
		buf_addc(&line, '\n');
		fwrite(line.p, 1, line.len, stdout);
	}
	buf_free(&line);
}
