#define _DEFAULT_SOURCE
#include "mgit.h"

#include <stdlib.h>
#include <string.h>

static size_t idset_slot(const struct idset *s, const unsigned char id[20])
{
	size_t i;

	memcpy(&i, id, sizeof i);
	for (i &= s->cap - 1; s->used[i] && memcmp(s->v[i], id, 20); i = (i + 1) & (s->cap - 1))
		;
	return i;
}

int idset_has(const struct idset *s, const unsigned char id[20])
{
	return s->cap && s->used[idset_slot(s, id)];
}

/* Adds id; returns 0 when it was there already. */
int idset_add(struct idset *s, const unsigned char id[20])
{
	size_t i;

	if (2 * (s->n + 1) > s->cap) {
		struct idset g = { NULL, NULL, s->cap ? s->cap * 2 : 256, 0 };

		g.v = xmalloc(g.cap * sizeof *g.v);
		g.used = calloc(g.cap, 1);
		if (!g.used)
			die("out of memory");
		for (i = 0; i < s->cap; i++)
			if (s->used[i])
				idset_add(&g, s->v[i]);
		free(s->v);
		free(s->used);
		*s = g;
	}
	i = idset_slot(s, id);
	if (s->used[i])
		return 0;
	s->used[i] = 1;
	memcpy(s->v[i], id, 20);
	s->n++;
	return 1;
}

void idset_free(struct idset *s)
{
	free(s->v);
	free(s->used);
	memset(s, 0, sizeof *s);
}

void objlist_add(struct objlist *l, const unsigned char id[20], int type)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->v = xrealloc(l->v, l->cap * sizeof *l->v);
	}
	memcpy(l->v[l->n].id, id, 20);
	l->v[l->n++].type = type;
}

/*
 * Every object reachable from the tips that seen does not hold yet: each is added to seen and, when out is set, to
 * out (commits and tags as met, then trees, then blobs, the order git writes packs in). Blobs are named by their
 * trees and not read. A tip this repository does not have is skipped when missing_ok (a remote's tips may be
 * unknown here); anything else missing is an error.
 */
void walk(const unsigned char (*tips)[20], size_t ntips, struct idset *seen, struct objlist *out, int missing_ok)
{
	struct objlist todo = { 0 }, trees = { 0 }, blobs = { 0 };
	size_t i;

	for (i = 0; i < ntips; i++)
		if (!missing_ok || object_exists(tips[i]))
			objlist_add(&todo, tips[i], 0);
	while (todo.n) {
		struct obj o = todo.v[--todo.n];
		buf b = { 0 };
		int type;

		if (o.type == OBJ_BLOB) {
			if (idset_add(seen, o.id) && out)
				objlist_add(&blobs, o.id, OBJ_BLOB);
			continue;
		}
		if (idset_has(seen, o.id))
			continue;
		object_read_or_die(o.id, &type, &b);
		idset_add(seen, o.id);
		if (type == OBJ_COMMIT) {
			struct commit c;
			int k;

			commit_parse(&b, &c);
			if (out)
				objlist_add(out, o.id, type);
			for (k = c.nparent - 1; k >= 0; k--)
				objlist_add(&todo, c.parent[k], 0);
			objlist_add(&todo, c.tree, OBJ_TREE);
			free(c.parent);
		} else if (type == OBJ_TAG) {
			unsigned char target[20];

			if (b.len < 47 || memcmp(b.p, "object ", 7) || hex_to_id((char *)b.p + 7, target))
				die("corrupt tag object");
			if (out)
				objlist_add(out, o.id, type);
			objlist_add(&todo, target, 0);
		} else if (type == OBJ_TREE) {
			const unsigned char *p = b.p, *end = b.p + b.len;

			if (out)
				objlist_add(&trees, o.id, type);
			while (p < end) {
				const unsigned char *nul = memchr(p, 0, (size_t)(end - p));

				if (!nul || end - nul < 21)
					die("corrupt tree object");
				if (!strncmp((const char *)p, "40000 ", 6))
					objlist_add(&todo, nul + 1, OBJ_TREE);
				else if (strncmp((const char *)p, "160000 ", 7))
					objlist_add(&todo, nul + 1, OBJ_BLOB);
				p = nul + 21;
			}
		} else if (out) {
			objlist_add(&blobs, o.id, type);
		}
		buf_free(&b);
	}
	for (i = 0; out && i < trees.n; i++)
		objlist_add(out, trees.v[i].id, OBJ_TREE);
	for (i = 0; out && i < blobs.n; i++)
		objlist_add(out, blobs.v[i].id, OBJ_BLOB);
	free(todo.v);
	free(trees.v);
	free(blobs.v);
}

/* Whether anc is tip or one of its ancestors here. */
int descends(const unsigned char tip[20], const unsigned char anc[20])
{
	struct objlist todo = { 0 };
	struct idset seen = { 0 };
	int found = 0;

	objlist_add(&todo, tip, OBJ_COMMIT);
	while (todo.n && !found) {
		struct obj o = todo.v[--todo.n];
		struct commit c;
		buf b = { 0 };
		int type, k;

		if (!memcmp(o.id, anc, 20)) {
			found = 1;
			break;
		}
		if (!idset_add(&seen, o.id))
			continue;
		object_read_or_die(o.id, &type, &b);
		if (type == OBJ_TAG) {
			unsigned char t[20];

			if (!hex_to_id((char *)b.p + 7, t))
				objlist_add(&todo, t, 0);
		} else if (type == OBJ_COMMIT) {
			commit_parse(&b, &c);
			for (k = 0; k < c.nparent; k++)
				objlist_add(&todo, c.parent[k], OBJ_COMMIT);
			free(c.parent);
		}
		buf_free(&b);
	}
	free(todo.v);
	idset_free(&seen);
	return found;
}

