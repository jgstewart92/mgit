#define _DEFAULT_SOURCE
#include "mgit.h"

#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The pre-push check: what a push would send, against the rules in a settings file read again on every run
 * ($XDG_CONFIG_HOME/mgit/check.conf, else the shipped MGIT_CHECK_DEFAULT). Every hit is printed as FAIL or WARN with
 * where it is (path:line, or the commit); any FAIL refuses the push.
 */

#ifndef MGIT_CHECK_DEFAULT
#define MGIT_CHECK_DEFAULT "/usr/local/share/mgit/check.conf"
#endif

enum { R_SECRET, R_PRIVATE, R_ALLOW, R_BRANCH, R_TAG, R_KINDS };
static const char *const kind_name[R_KINDS] = { "secret", "private", "allow", "branch", "tag" };

struct rule {
	int kind, line;
	int empty;	/* it matches the empty string */
	regex_t re;
	regex_t re_nl;	/* secret and private rules: the same, with REG_NEWLINE, for scan_text's search over many lines */
};

struct ck {
	struct rule *r;
	size_t nr;
	int any_empty;	/* a secret or private rule matches the empty string */
	uint64_t warn_size, max_size;
	size_t subject;
	const char *file;
	int fail, warn;
	/* blob id -> first path it was seen at */
	const struct objlist *objs;
	size_t *by_id;
	char **path;
	struct idset trees_done, paths_done, ignored_done;
	struct ignore ig;
};

static const struct objlist *sort_objs;

static int cmp_by_id(const void *a, const void *b)
{
	return memcmp(sort_objs->v[*(const size_t *)a].id, sort_objs->v[*(const size_t *)b].id, 20);
}

static long obj_index(const struct ck *ck, const unsigned char id[20])
{
	size_t lo = 0, hi = ck->objs->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = memcmp(ck->objs->v[ck->by_id[mid]].id, id, 20);

		if (!c)
			return (long)ck->by_id[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return -1;
}

static void report(struct ck *ck, int fail, const char *rule, const char *key, const char *fmt, ...)
	__attribute__((format(printf, 5, 6)));

static int allowed(const struct ck *ck, const char *text)
{
	size_t i;

	for (i = 0; i < ck->nr; i++)
		if (ck->r[i].kind == R_ALLOW && !regexec(&ck->r[i].re, text, 0, NULL, 0))
			return 1;
	return 0;
}

static void emit(struct ck *ck, int fail, const char *rule, const char *text)
{
	buf t = { 0 };

	buf_add_masked(&t, text);	/* paths and idents come from fetched objects too */
	printf("%s %-11s %s\n", fail ? "FAIL" : "WARN", rule, (char *)t.p);
	buf_free(&t);
	if (fail)
		ck->fail++;
	else
		ck->warn++;
}

/* A hit, unless an allow rule matches key (the printed text when key is NULL). */
static void report(struct ck *ck, int fail, const char *rule, const char *key, const char *fmt, ...)
{
	buf b = { 0 };
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	buf_grow(&b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf((char *)b.p, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b.len = (size_t)n;
	if (!allowed(ck, key ? key : (char *)b.p))
		emit(ck, fail, rule, (char *)b.p);
	buf_free(&b);
}

/* ---- the settings file ---- */

static int load_rules(struct ck *ck)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char *user = xdg && *xdg ? path_join(xdg, "mgit/check.conf") : home ? path_join(home, ".config/mgit/check.conf") : NULL;
	buf b = { 0 };
	char *line, *next;
	int ln = 0, bad = 0;

	ck->warn_size = 52428800;
	ck->max_size = 104857600;
	ck->subject = 72;
	if (user && !read_file(user, &b)) {
		ck->file = user;
	} else {
		if (user && errno != ENOENT)
			die("cannot read %s: %s", user, strerror(errno));
		b.len = 0;
		if (read_file(MGIT_CHECK_DEFAULT, &b)) {
			printf("FAIL %-11s no settings file: neither %s nor %s\n", "settings",
			       user ? user : "$XDG_CONFIG_HOME/mgit/check.conf", MGIT_CHECK_DEFAULT);
			free(user);
			ck->fail++;
			buf_free(&b);
			return -1;
		}
		free(user);
		ck->file = MGIT_CHECK_DEFAULT;
	}
	for (line = (char *)b.p; line; line = next) {
		char *v, *e;
		size_t kl;
		int k, flags = REG_EXTENDED, err;

		ln++;
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		while (isspace((unsigned char)*line))
			line++;
		e = line + strlen(line);
		while (e > line && isspace((unsigned char)e[-1]))
			*--e = 0;
		if (!*line || *line == '#')
			continue;
		kl = strcspn(line, " \t");
		for (v = line + kl; *v == ' ' || *v == '\t'; v++)
			;
		if (!*v) {
			printf("FAIL %-11s %s:%d: a rule needs a value: %s\n", "settings", ck->file, ln, line);
			bad = 1;
			continue;
		}
		if ((kl == 9 && !strncmp(line, "warn-size", 9)) || (kl == 8 && !strncmp(line, "max-size", 8)) ||
		    (kl == 7 && !strncmp(line, "subject", 7))) {
			char *end;
			unsigned long long n = strtoull(v, &end, 10);

			if (*end || !n) {
				printf("FAIL %-11s %s:%d: not a positive number: %s\n", "settings", ck->file, ln, v);
				bad = 1;
			} else if (line[0] == 'w')
				ck->warn_size = n;
			else if (line[0] == 'm')
				ck->max_size = n;
			else
				ck->subject = (size_t)n;
			continue;
		}
		for (k = 0; k < R_KINDS; k++)
			if (strlen(kind_name[k]) == kl && !strncmp(line, kind_name[k], kl))
				break;
		if (k == R_KINDS) {
			printf("FAIL %-11s %s:%d: unknown rule '%.*s'\n", "settings", ck->file, ln, (int)kl, line);
			bad = 1;
			continue;
		}
		if (!strncmp(v, "(?i)", 4)) {
			flags |= REG_ICASE;
			v += 4;
		}
		ck->r = xrealloc(ck->r, (ck->nr + 1) * sizeof *ck->r);
		if ((err = regcomp(&ck->r[ck->nr].re, v, flags))) {
			char msg[200];

			regerror(err, &ck->r[ck->nr].re, msg, sizeof msg);
			printf("FAIL %-11s %s:%d: bad pattern (%s): %s\n", "settings", ck->file, ln, msg, v);
			bad = 1;
			continue;
		}
		if ((k == R_SECRET || k == R_PRIVATE) && regcomp(&ck->r[ck->nr].re_nl, v, flags | REG_NEWLINE))
			die("cannot compile %s:%d a second time", ck->file, ln);
		ck->r[ck->nr].kind = k;
		ck->r[ck->nr].empty = !regexec(&ck->r[ck->nr].re, "", 0, NULL, 0);
		if (ck->r[ck->nr].empty && (k == R_SECRET || k == R_PRIVATE))
			ck->any_empty = 1;
		ck->r[ck->nr++].line = ln;
	}
	buf_free(&b);
	if (bad)
		ck->fail++;
	return bad ? -1 : 0;
}

/* ---- text ---- */

static void put_clipped(buf *out, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n && i < 160; i++)
		buf_addc(out, (unsigned char)s[i] < 0x20 && s[i] != '\t' ? '?' : s[i]);
	if (n > 160)
		buf_adds(out, "...");
}

/* Whether rule r has a match in s[0, n) (which holds no NUL) that no allow rule covers, judged on "WHERE: MATCH". */
static int seg_hit(const struct ck *ck, const struct rule *r, const char *where, size_t lineno, const char *s, size_t n,
		   buf *key)
{
	size_t off = 0;
	regmatch_t m;
	int eflags = 0;

	if (!n && !r->empty)
		return 0;
	for (;;) {
		m.rm_so = 0;
		m.rm_eo = (regoff_t)(n - off);
		if (regexec(&r->re, s + off, 1, &m, eflags | REG_STARTEND))
			return 0;
		key->len = 0;
		if (lineno)
			buf_printf(key, "%s:%zu: ", where, lineno);
		else
			buf_printf(key, "%s: ", where);
		buf_add(key, s + off + m.rm_so, (size_t)(m.rm_eo - m.rm_so));
		if (!allowed(ck, (char *)key->p))
			return 1;
		off += (size_t)(m.rm_eo > m.rm_so ? m.rm_eo : m.rm_so + 1);
		eflags = REG_NOTBOL;
		if (off > n)
			return 0;
	}
}

/*
 * The secret and private rules over one line (cut at NULs, as a binary file's strings are). A rule hits when it has a
 * match no allow rule covers, judged on "WHERE: MATCH"; the line is printed once, with every rule that hit it. Each
 * piece between NULs is searched where it lies (REG_STARTEND), and the empty pieces of a run of NULs only by the
 * rules that match an empty string, so a line of millions of NULs costs no more than reading it.
 */
static void scan_line(struct ck *ck, const char *where, size_t lineno, const char *s, size_t n)
{
	buf key = { 0 }, rules = { 0 };
	size_t a = 0, i, left = 0;
	char *hit = calloc(ck->nr + 1, 1);

	if (!hit)
		die("out of memory");
	for (i = 0; i < ck->nr; i++)
		left += ck->r[i].kind == R_SECRET || ck->r[i].kind == R_PRIVATE;
	while (a <= n && left) {
		const char *nul = a < n ? memchr(s + a, 0, n - a) : NULL;
		size_t b = nul ? (size_t)(nul - s) : n;

		if (b > a || ck->any_empty)
			for (i = 0; i < ck->nr; i++) {
				const struct rule *r = &ck->r[i];

				if ((r->kind != R_SECRET && r->kind != R_PRIVATE) || hit[i])
					continue;
				if (seg_hit(ck, r, where, lineno, s + a, b - a, &key)) {
					hit[i] = 1;
					left--;
				}
			}
		a = b + 1;
		if (!ck->any_empty)
			while (a < n && !s[a])
				a++;
	}
	for (i = 0; i < ck->nr; i++)
		if (hit[i])
			buf_printf(&rules, "%s%s:%d", rules.len ? "," : "", kind_name[ck->r[i].kind], ck->r[i].line);
	free(hit);
	if (rules.len) {
		buf text = { 0 };

		if (lineno)
			buf_printf(&text, "%s:%zu: ", where, lineno);
		else
			buf_printf(&text, "%s: ", where);
		put_clipped(&text, s, n);
		emit(ck, 1, (char *)rules.p, (char *)text.p);
		buf_free(&text);
	}
	buf_free(&key);
	buf_free(&rules);
}

/*
 * Every line through scan_line, except lines no rule can hit. Over a run of lines with no NUL in it, each rule is
 * searched once with REG_NEWLINE (^ and $ at every line end, . never across one) for its next match; a line before
 * every rule's next match has no match of its own, since a match within one line is also a match in the run. Only
 * the lines a next match starts on, and lines holding a NUL, are scanned line by line, so the result is the same as
 * scanning each line, at one search per rule and hit instead of one per rule and line.
 */
static void scan_text(struct ck *ck, const char *where, const unsigned char *p, size_t n, int crlf)
{
	size_t lineno = 1, a = 0, cr = 0, first_cr = 0, run_end = 0, i;
	size_t *next = xmalloc((ck->nr + 1) * sizeof *next);
	char *known = xmalloc(ck->nr + 1);

	while (a < n) {
		const unsigned char *nl = memchr(p + a, '\n', n - a);
		size_t b = nl ? (size_t)(nl - p) : n;
		int dirty = 0;

		if (crlf && b > a && p[b - 1] == '\r' && !cr++)
			first_cr = lineno;
		if (a >= run_end) {
			const unsigned char *z = memchr(p + a, 0, n - a);

			run_end = n;
			if (z) {
				while (z > p + a && z[-1] != '\n')
					z--;
				run_end = (size_t)(z - p);
			}
			memset(known, 0, ck->nr + 1);
		}
		if (a >= run_end) {
			dirty = 1;
		} else {
			for (i = 0; i < ck->nr && !dirty; i++) {
				const struct rule *r = &ck->r[i];
				regmatch_t m;

				if (r->kind != R_SECRET && r->kind != R_PRIVATE)
					continue;
				if (!known[i] || (next[i] != SIZE_MAX && next[i] < a)) {
					m.rm_so = 0;
					m.rm_eo = (regoff_t)(run_end - a);
					next[i] = regexec(&r->re_nl, (const char *)p + a, 1, &m, REG_STARTEND) ? SIZE_MAX :
						  a + (size_t)m.rm_so;
					known[i] = 1;
				}
				dirty = next[i] <= b;
			}
		}
		if (dirty)
			scan_line(ck, where, lineno, (const char *)p + a, b - a);
		a = b + 1;
		lineno++;
	}
	free(next);
	free(known);
	if (cr)
		report(ck, 1, "crlf", NULL, "%s:%zu: CR LF line ending%s (%zu line%s)", where, first_cr, cr > 1 ? "s" : "", cr,
		       cr > 1 ? "s" : "");
}

/* ---- commits and tags ---- */

static void short_hex(const unsigned char id[20], char out[8])
{
	char hex[41];

	id_to_hex(id, hex);
	memcpy(out, hex, 7);
	out[7] = 0;
}

/* "Name <email> secs tz": a name, and an email with an @ in it. */
static void check_ident(struct ck *ck, const char *what, const char *role, const char *s, size_t n)
{
	const char *lt = memchr(s, '<', n), *gt = lt ? memchr(lt, '>', n - (size_t)(lt - s)) : NULL;
	const char *e = lt;
	buf w = { 0 };

	while (e && e > s && e[-1] == ' ')
		e--;
	if (!lt || !gt || e == s)
		report(ck, 1, "identity", NULL, "%s: the %s has no name", what, role);
	else if (gt == lt + 1 || !memchr(lt, '@', (size_t)(gt - lt)))
		report(ck, 1, "identity", NULL, "%s: the %s has no email (%.*s)", what, role, (int)(gt + 1 - s), s);
	buf_printf(&w, "%s %s", what, role);
	if (gt)
		scan_line(ck, (char *)w.p, 0, s, (size_t)(gt + 1 - s));
	buf_free(&w);
}

/* subject, blank line, body: a subject, nothing on line 2, the patterns over every line */
static void check_message(struct ck *ck, const char *what, const char *m, size_t n)
{
	const char *nl = memchr(m, '\n', n);
	size_t sub = nl ? (size_t)(nl - m) : n, chars = 0, i;
	buf w = { 0 };

	if (!sub)
		report(ck, 1, "message", NULL, "%s: no subject line", what);
	for (i = 0; i < sub; i++)
		chars += ((unsigned char)m[i] & 0xc0) != 0x80;
	if (chars > ck->subject)
		report(ck, 0, "message", NULL, "%s: the subject is %zu characters (over %zu): %.*s", what, chars, ck->subject,
		       (int)(sub > 80 ? 80 : sub), m);
	if (nl && (size_t)(nl - m) + 1 < n && m[sub + 1] != '\n')
		report(ck, 1, "message", NULL, "%s: line 2 is not blank (subject, blank line, body)", what);
	buf_printf(&w, "%s message", what);
	scan_text(ck, (char *)w.p, (const unsigned char *)m, n, 0);
	buf_free(&w);
}

/* The header line starting with key ("author "), or NULL. */
static const char *header(const buf *b, const char *key, size_t *len)
{
	const char *p = (const char *)b->p, *end = p + b->len;
	size_t k = strlen(key);

	while (p < end && *p != '\n') {
		const char *nl = memchr(p, '\n', (size_t)(end - p));

		if (!nl)
			nl = end;
		if ((size_t)(nl - p) > k && !memcmp(p, key, k)) {
			*len = (size_t)(nl - p) - k;
			return p + k;
		}
		p = nl + 1;
	}
	return NULL;
}

static const char *body(const buf *b, size_t *len)
{
	const char *s = strstr((const char *)b->p, "\n\n");

	if (!s) {
		*len = 0;
		return (const char *)b->p + b->len;
	}
	*len = b->len - (size_t)(s + 2 - (const char *)b->p);
	return s + 2;
}

/* ---- trees: paths, the ignore rules ---- */

static void check_tree(struct ck *ck, const unsigned char tree[20], const char *prefix, int ign_dir, const unsigned char ctx[20],
		       const char *commit, int depth)
{
	unsigned char key[20], sub_ctx[20];
	const unsigned char *p, *end, *eid;
	const char *name;
	buf b = { 0 }, path = { 0 };
	size_t mark = ck->ig.n;
	unsigned mode;
	sha1_ctx c;
	int type;

	if (depth > TREE_DEPTH_MAX)
		die("trees are nested more than %d deep", TREE_DEPTH_MAX);
	sha1_init(&c);
	sha1_update(&c, tree, 20);
	sha1_update(&c, ctx, 20);
	sha1_update(&c, &ign_dir, sizeof ign_dir);
	sha1_update(&c, prefix, strlen(prefix));
	sha1_final(&c, key);
	if (!idset_add(&ck->trees_done, key))
		return;
	object_read_or_die(tree, &type, &b);
	if (type != OBJ_TREE)
		die("expected a tree, found a %s", type_name(type));
	memcpy(sub_ctx, ctx, 20);
	for (p = b.p, end = b.p + b.len; tree_next(&p, end, &mode, &name, &eid);) {
		if (!ign_dir && !strcmp(name, ".gitignore") && (mode & 0170000) == 0100000) {
			buf gi = { 0 };
			int t;

			object_read_or_die(eid, &t, &gi);
			ignore_push_text(&ck->ig, (char *)gi.p, prefix);
			buf_free(&gi);
			sha1_init(&c);
			sha1_update(&c, sub_ctx, 20);
			sha1_update(&c, eid, 20);
			sha1_final(&c, sub_ctx);
		}
	}
	for (p = b.p; tree_next(&p, end, &mode, &name, &eid);) {
		int dir = mode == 040000, link = mode == 0160000;

		path.len = 0;
		buf_adds(&path, prefix);
		buf_adds(&path, name);
		if (link) {
			unsigned char pk[20];
			char hex[41];

			object_hash(OBJ_BLOB, path.p, path.len, pk);
			if (idset_add(&ck->paths_done, pk)) {
				scan_line(ck, (char *)path.p, 0, (char *)path.p, path.len);
				id_to_hex(eid, hex);
				report(ck, 0, "submodule", NULL, "%s: a submodule entry (commit %.7s); its own commits are not in this "
				       "push and are not checked", (char *)path.p, hex);
			}
			continue;
		}
		if (dir) {
			int ign = ign_dir || ignore_match(&ck->ig, (char *)path.p, 1);

			buf_addc(&path, '/');
			check_tree(ck, eid, (char *)path.p, ign, sub_ctx, commit, depth + 1);
			continue;
		}
		{
			long i = obj_index(ck, eid);
			unsigned char pk[20];

			if (i >= 0 && !ck->path[i])
				ck->path[i] = xstrdup((char *)path.p);
			object_hash(OBJ_BLOB, path.p, path.len, pk);
			if (idset_add(&ck->paths_done, pk))
				scan_line(ck, (char *)path.p, 0, (char *)path.p, path.len);
			if ((ign_dir || ignore_match(&ck->ig, (char *)path.p, 0)) && idset_add(&ck->ignored_done, pk))
				report(ck, 1, "ignored", NULL, "%s: a .gitignore says to ignore it (commit %s)", (char *)path.p, commit);
		}
	}
	ignore_pop(&ck->ig, mark);
	buf_free(&b);
	buf_free(&path);
}

static int ref_allowed(const struct ck *ck, int kind, const char *name)
{
	size_t i;

	for (i = 0; i < ck->nr; i++)
		if (ck->r[i].kind == kind && !regexec(&ck->r[i].re, name, 0, NULL, 0))
			return 1;
	return 0;
}

/* Prints every hit and a summary; returns 1 on any FAIL, else 0. */
int check_run(const struct objlist *objs, const struct check_ref *refs, size_t nrefs, const char *what)
{
	struct ck ck;
	static const unsigned char zero[20];
	size_t i, ncommits = 0, ntags = 0, nblobs = 0;
	uint64_t bytes = 0;

	memset(&ck, 0, sizeof ck);
	if (load_rules(&ck)) {
		printf("%s: FAIL  the settings are broken; nothing else was checked\n", what);
		return 1;
	}
	ck.objs = objs;
	ck.by_id = xmalloc((objs->n + 1) * sizeof *ck.by_id);
	ck.path = calloc(objs->n + 1, sizeof *ck.path);
	if (!ck.path)
		die("out of memory");
	for (i = 0; i < objs->n; i++)
		ck.by_id[i] = i;
	sort_objs = objs;
	qsort(ck.by_id, objs->n, sizeof *ck.by_id, cmp_by_id);
	ignore_init(&ck.ig);

	for (i = 0; i < nrefs; i++) {
		int heads = !strncmp(refs[i].name, "refs/heads/", 11);

		if (heads && !ref_allowed(&ck, R_BRANCH, refs[i].name + 11))
			report(&ck, 1, "branch", NULL, "%s: not a branch name the rules allow (%s)", refs[i].name, ck.file);
		else if (!strncmp(refs[i].name, "refs/tags/", 10) && !ref_allowed(&ck, R_TAG, refs[i].name + 10))
			report(&ck, 1, "tag", NULL, "%s: not a tag name the rules allow (%s)", refs[i].name, ck.file);
	}
	for (i = 0; i < objs->n; i++) {
		const struct obj *o = &objs->v[i];
		char sh[8], label[40];
		buf b = { 0 };
		const char *h, *m;
		size_t hl, ml;
		int type;

		if (o->type != OBJ_COMMIT && o->type != OBJ_TAG)
			continue;
		object_read_or_die(o->id, &type, &b);
		short_hex(o->id, sh);
		snprintf(label, sizeof label, "%s %s", type == OBJ_COMMIT ? "commit" : "tag", sh);
		if (type == OBJ_COMMIT) {
			struct commit c;

			ncommits++;
			commit_parse(&b, &c);
			free(c.parent);
			if ((h = header(&b, "author ", &hl)))
				check_ident(&ck, label, "author", h, hl);
			if ((h = header(&b, "committer ", &hl)))
				check_ident(&ck, label, "committer", h, hl);
			else
				report(&ck, 1, "identity", NULL, "%s: no committer", label);
			check_tree(&ck, c.tree, "", 0, zero, sh, 0);
		} else {
			ntags++;
			if ((h = header(&b, "tagger ", &hl)))
				check_ident(&ck, label, "tagger", h, hl);
			else
				report(&ck, 1, "identity", NULL, "%s: no tagger", label);
		}
		m = body(&b, &ml);
		check_message(&ck, label, m, ml);
		buf_free(&b);
	}
	for (i = 0; i < objs->n; i++) {
		const struct obj *o = &objs->v[i];
		char sh[8];
		buf b = { 0 }, where = { 0 };
		int type, binary;

		if (o->type != OBJ_BLOB)
			continue;
		nblobs++;
		object_read_or_die(o->id, &type, &b);
		bytes += b.len;
		short_hex(o->id, sh);
		if (ck.path[i])
			buf_adds(&where, ck.path[i]);
		else
			buf_printf(&where, "(blob %s)", sh);
		/* binary as git decides it: a NUL in the first 8000 bytes; such a file gets the size and name rules only */
		binary = memchr(b.p, 0, b.len < 8000 ? b.len : 8000) != NULL;
		if (b.len > ck.max_size) {
			/* refused for its size; its text is not read */
			report(&ck, 1, "size", NULL, "%s: %zu bytes, over the limit of %llu (max-size)", (char *)where.p, b.len,
			       (unsigned long long)ck.max_size);
		} else {
			if (b.len > ck.warn_size)
				report(&ck, 0, "size", NULL, "%s: %zu bytes, over %llu (warn-size)", (char *)where.p, b.len,
				       (unsigned long long)ck.warn_size);
			if (!binary)
				scan_text(&ck, (char *)where.p, b.p, b.len, 1);
			if (!binary && is_lfs_pointer(&b))
				report(&ck, 0, "lfs", NULL, "%s: a Git LFS pointer; the content it names is not in this push "
				       "(mgit does not push LFS objects)", (char *)where.p);
		}
		buf_free(&b);
		buf_free(&where);
	}
	printf("%s: %s  %d fail, %d warn; %zu commit%s, %zu tag%s, %zu ref%s, %zu file%s (%llu bytes); rules %s\n", what,
	       ck.fail ? "FAIL" : "PASS", ck.fail, ck.warn, ncommits, ncommits == 1 ? "" : "s", ntags, ntags == 1 ? "" : "s",
	       nrefs, nrefs == 1 ? "" : "s", nblobs, nblobs == 1 ? "" : "s", (unsigned long long)bytes, ck.file);
	for (i = 0; i < objs->n; i++)
		free(ck.path[i]);
	free(ck.path);
	free(ck.by_id);
	for (i = 0; i < ck.nr; i++) {
		regfree(&ck.r[i].re);
		if (ck.r[i].kind == R_SECRET || ck.r[i].kind == R_PRIVATE)
			regfree(&ck.r[i].re_nl);
	}
	free(ck.r);
	idset_free(&ck.trees_done);
	idset_free(&ck.paths_done);
	idset_free(&ck.ignored_done);
	ignore_pop(&ck.ig, 0);
	free(ck.ig.v);
	return ck.fail > 0;
}

/*
 * mgit check [REV]: what a first push would send. With no REV: the current branch and every tag; with one, that
 * branch, tag or revision alone.
 */
int cmd_check(int argc, char **argv)
{
	struct objlist objs = { 0 };
	struct idset seen = { 0 };
	struct check_ref *refs;
	unsigned char (*tips)[20];
	char **tags;
	size_t ntags = 0, nrefs = 0, ntips = 0, i;
	buf name = { 0 };
	int r;

	if (argc > 2 || (argc == 2 && argv[1][0] == '-'))
		die("usage: mgit check [<rev>]");
	repo_find();
	tags = argc == 1 ? ref_list("refs/tags/", &ntags) : NULL;
	tips = xmalloc((ntags + 1) * sizeof *tips);
	refs = xmalloc((ntags + 1) * sizeof *refs);
	if (argc == 1) {
		char *head = head_symref();

		if (head && !ref_read(head, tips[ntips])) {
			refs[nrefs].name = head;
			memcpy(refs[nrefs++].id, tips[ntips++], 20);
		} else if (!head && !ref_read("HEAD", tips[ntips])) {
			ntips++;
		}
		for (i = 0; i < ntags; i++) {
			name.len = 0;
			buf_printf(&name, "refs/tags/%s", tags[i]);
			if (ref_read((char *)name.p, tips[ntips]))
				continue;
			refs[nrefs].name = xstrdup((char *)name.p);
			memcpy(refs[nrefs++].id, tips[ntips++], 20);
		}
	} else {
		static const char *const kinds[] = { "%s", "refs/heads/%s", "refs/tags/%s" };

		for (i = 0; i < 3 && !ntips; i++) {
			name.len = 0;
			buf_printf(&name, kinds[i], argv[1]);
			if ((i || !strncmp(argv[1], "refs/", 5)) && refname_ok((char *)name.p) &&
			    !ref_read((char *)name.p, tips[0])) {
				refs[nrefs].name = xstrdup((char *)name.p);
				memcpy(refs[nrefs++].id, tips[0], 20);
				ntips = 1;
			}
		}
		if (!ntips) {
			if (rev_parse(argv[1], tips[0]))
				die("not a valid revision: %s", argv[1]);
			ntips = 1;
		}
	}
	if (!ntips)
		die("nothing to check: no commits yet");
	walk((const unsigned char (*)[20])tips, ntips, &seen, &objs, 0);
	r = check_run(&objs, refs, nrefs, "check");
	idset_free(&seen);
	free(objs.v);
	buf_free(&name);
	return r;
}
