#define _DEFAULT_SOURCE
#include "mgit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const wday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int git_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* An ident line's parts: "Name <email> <seconds> <+hhmm>". */
struct ident_parts {
	const char *name, *zone;
	size_t name_len, upto_gt;	/* upto_gt: "Name <email>" */
	long long secs;
	int have_date;
};

static void ident_split(const char *s, size_t n, struct ident_parts *ip)
{
	const char *gt = NULL, *lt = memchr(s, '<', n), *p, *end = s + n;

	memset(ip, 0, sizeof *ip);
	for (p = s; p < end; p++)
		if (*p == '>')
			gt = p;
	ip->name = s;
	ip->name_len = lt ? (size_t)(lt - s) : n;
	while (ip->name_len && s[ip->name_len - 1] == ' ')
		ip->name_len--;
	if (!gt) {
		ip->upto_gt = n;
		return;
	}
	ip->upto_gt = (size_t)(gt + 1 - s);
	for (p = gt + 1; p < end && *p == ' '; p++)
		;
	if (p == end || *p < '0' || *p > '9')
		return;
	for (; p < end && *p >= '0' && *p <= '9'; p++)
		ip->secs = ip->secs * 10 + (*p - '0');
	if (end - p != 6 || p[0] != ' ' || (p[1] != '+' && p[1] != '-'))
		return;
	ip->zone = p + 1;
	ip->have_date = 1;
}

/* git's default date format, "Thu Nov 12 22:21:18 2026 -0500", in the zone the ident names. */
static void put_date(buf *out, const struct ident_parts *ip)
{
	const char *z = ip->zone;
	long off;
	time_t t;
	struct tm tm;

	if (!ip->have_date)
		return;
	off = ((z[1] - '0') * 10 + (z[2] - '0')) * 3600L + ((z[3] - '0') * 10 + (z[4] - '0')) * 60L;
	t = (time_t)(ip->secs + (z[0] == '-' ? -off : off));
	if (!gmtime_r(&t, &tm))
		return;
	buf_printf(out, "%s %s %d %02d:%02d:%02d %d %.5s", wday[tm.tm_wday], mon[tm.tm_mon], tm.tm_mday,
		   tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900, z);
}

/* "Author: Name <email>" and git's default date line. */
static void put_author(buf *out, const char *s, size_t n)
{
	struct ident_parts ip;

	ident_split(s, n, &ip);
	buf_printf(out, "Author: %.*s\n", (int)ip.upto_gt, s);
	if (ip.have_date) {
		buf_adds(out, "Date:   ");
		put_date(out, &ip);
		buf_addc(out, '\n');
	}
}

/* Message body as git log shows it: leading blank lines dropped, 4-space indent, tabs expanded to 8 columns. */
static void put_message(buf *out, const char *s, size_t n)
{
	const char *p = s, *end = s + n;
	int first = 1;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t len, i, col = 0;

		if (!nl)
			nl = end;
		len = (size_t)(nl - p);
		while (len && git_space(p[len - 1]))
			len--;
		if (len || !first) {
			first = 0;
			buf_adds(out, "    ");
			for (i = 0; i < len; i++) {
				if (p[i] == '\t') {
					do
						buf_addc(out, ' ');
					while (++col % 8);
				} else {
					buf_addc(out, p[i]);
					if (((unsigned char)p[i] & 0xc0) != 0x80)
						col++;
				}
			}
			buf_addc(out, '\n');
		}
		if (nl == end)
			break;
		p = nl + 1;
	}
}

/* git's %s: the first paragraph (after leading blank lines), each line's trailing blanks dropped, joined by spaces. */
static void put_subject(buf *out, const char *s, size_t n)
{
	const char *p = s, *end = s + n;
	int started = 0;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t len;

		if (!nl)
			nl = end;
		len = (size_t)(nl - p);
		while (len && isspace((unsigned char)p[len - 1]))
			len--;
		if (!len) {
			if (started)
				break;
		} else {
			if (started)
				buf_addc(out, ' ');
			buf_add(out, p, len);
			started = 1;
		}
		if (nl == end)
			break;
		p = nl + 1;
	}
}

/* --format: %H %h %an %ad %s, %n and %%; anything else is copied as it stands, as git does with what it does not know. */
static void put_format(buf *out, const char *fmt, const unsigned char id[20], const struct commit *c)
{
	struct ident_parts ip;
	const char *f;
	char hex[41];

	ident_split(c->author, c->author_len, &ip);
	id_to_hex(id, hex);
	for (f = fmt; *f; f++) {
		if (*f != '%') {
			buf_addc(out, *f);
			continue;
		}
		switch (f[1]) {
		case 'H':
			buf_adds(out, hex);
			break;
		case 'h':
			buf_add(out, hex, (size_t)abbrev_len(id, -1));
			break;
		case 's':
			put_subject(out, c->msg, c->msg_len);
			break;
		case 'n':
			buf_addc(out, '\n');
			break;
		case '%':
			buf_addc(out, '%');
			break;
		case 'a':
			if (f[2] == 'n') {
				buf_add(out, ip.name, ip.name_len);
				f++;
				break;
			}
			if (f[2] == 'd') {
				put_date(out, &ip);
				f++;
				break;
			}
			buf_addc(out, '%');
			continue;
		default:
			buf_addc(out, '%');
			continue;
		}
		f++;
	}
	buf_addc(out, '\n');
}

/*
 * The dates --since and --until take, as git reads them (approxidate) for these forms: @<seconds> or a bare number
 * of 9 digits or more; now, yesterday; "<n> <unit>[s] ago" with second, minute, hour, day or week (blanks or dots
 * between the words); YYYY-MM-DD, with [ T]H:MM[:SS] and then an optional zone (Z, +hhmm, -hhmm). A local time
 * goes through mktime; a date with no time takes the current time of day, as git's does. Anything else is refused,
 * where git would read it as now.
 */
int parse_date(const char *s, long long *out)
{
	static const struct {
		const char *name;
		long secs;
	} units[] = { { "second", 1 }, { "minute", 60 }, { "hour", 3600 }, { "day", 86400 }, { "week", 604800 } };
	time_t now = time(NULL);
	const char *p = s;
	struct tm tm, today;
	size_t i, n;
	long long v = 0;
	char *e;

	while (*p == ' ')
		p++;
	n = strlen(p);
	while (n && p[n - 1] == ' ')
		n--;
	if (n == 3 && !memcmp(p, "now", 3)) {
		*out = now;
		return 0;
	}
	if (n == 9 && !memcmp(p, "yesterday", 9)) {
		*out = (long long)now - 86400;
		return 0;
	}
	if ((*p == '@' && strspn(p + 1, "0123456789") == n - 1 && n > 1) || (strspn(p, "0123456789") == n && n >= 9)) {
		for (i = *p == '@'; i < n; i++)
			v = v * 10 + (p[i] - '0');
		*out = v;
		return 0;
	}
	if (isdigit((unsigned char)*p) && (e = (char *)p + strspn(p, "0123456789")) && (*e == ' ' || *e == '.')) {
		const char *u = e, *w;

		v = strtoll(p, NULL, 10);
		while (*u == ' ' || *u == '.')
			u++;
		for (i = 0; i < sizeof units / sizeof *units; i++) {
			size_t ul = strlen(units[i].name);

			if (strncmp(u, units[i].name, ul))
				continue;
			w = u + ul + (u[ul] == 's');
			if (*w != ' ' && *w != '.')
				break;
			while (*w == ' ' || *w == '.')
				w++;
			if ((size_t)(w - p) + 3 == n && !strncmp(w, "ago", 3)) {
				*out = (long long)now - v * units[i].secs;
				return 0;
			}
			break;
		}
		return -1;
	}
	/* YYYY-MM-DD[( |T)H:MM[:SS][ ][Z|+hhmm|-hhmm]] */
	memset(&tm, 0, sizeof tm);
	if (n < 10 || strspn(p, "0123456789") != 4 || p[4] != '-' || !isdigit((unsigned char)p[5]) ||
	    !isdigit((unsigned char)p[6]) || p[7] != '-' || !isdigit((unsigned char)p[8]) || !isdigit((unsigned char)p[9]))
		return -1;
	tm.tm_year = atoi(p) - 1900;
	tm.tm_mon = (p[5] - '0') * 10 + (p[6] - '0') - 1;
	tm.tm_mday = (p[8] - '0') * 10 + (p[9] - '0');
	if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31)
		return -1;
	p += 10;
	n -= 10;
	if (!n) {
		localtime_r(&now, &today);
		tm.tm_hour = today.tm_hour;
		tm.tm_min = today.tm_min;
		tm.tm_sec = today.tm_sec;
		tm.tm_isdst = today.tm_isdst;
		*out = (long long)mktime(&tm);
		return 0;
	}
	{
		char rest[64];
		const char *q;
		int hh, mm, ss = 0, sign = 0, zh = 0, zm = 0;

		if (n >= sizeof rest || (*p != ' ' && *p != 'T'))
			return -1;
		memcpy(rest, p + 1, n - 1);
		rest[n - 1] = 0;
		q = rest;
		if (!isdigit((unsigned char)*q))
			return -1;
		hh = (int)strtol(q, &e, 10);
		if (e - q > 2 || *e != ':' || !isdigit((unsigned char)e[1]) || !isdigit((unsigned char)e[2]))
			return -1;
		mm = (e[1] - '0') * 10 + (e[2] - '0');
		q = e + 3;
		if (*q == ':') {
			if (!isdigit((unsigned char)q[1]) || !isdigit((unsigned char)q[2]))
				return -1;
			ss = (q[1] - '0') * 10 + (q[2] - '0');
			q += 3;
		}
		if (hh > 23 || mm > 59 || ss > 60)
			return -1;
		while (*q == ' ')
			q++;
		if (*q == 'Z' && !q[1]) {
			sign = 1;
		} else if ((*q == '+' || *q == '-') && strspn(q + 1, "0123456789") == 4 && !q[5]) {
			sign = *q == '-' ? -1 : 1;
			zh = (q[1] - '0') * 10 + (q[2] - '0');
			zm = (q[3] - '0') * 10 + (q[4] - '0');
		} else if (*q) {
			return -1;
		}
		tm.tm_hour = hh;
		tm.tm_min = mm;
		tm.tm_sec = ss;
		if (sign) {
			*out = (long long)timegm(&tm) - sign * (zh * 3600LL + zm * 60LL);
		} else {
			tm.tm_isdst = -1;
			*out = (long long)mktime(&tm);
		}
		return 0;
	}
}

/* The seconds of an ident line (0 when it has none, as git takes a commit with no date). */
static long long ident_secs(const char *s, size_t n)
{
	struct ident_parts ip;

	if (!s)
		return 0;
	ident_split(s, n, &ip);
	return ip.have_date ? ip.secs : 0;
}

/* The commits still to show, newest committer date first, and in the order they were met when dates tie (as git). */
struct qent {
	long long date;
	unsigned long seq;
	unsigned char id[20];
};
struct queue {
	struct qent *v;
	size_t n, cap;
	unsigned long seq;
};

static int qless(const struct qent *a, const struct qent *b)
{
	return a->date != b->date ? a->date > b->date : a->seq < b->seq;
}

static void qpush(struct queue *q, const unsigned char id[20], long long date)
{
	size_t i;

	if (q->n == q->cap) {
		q->cap = q->cap ? q->cap * 2 : 64;
		q->v = xrealloc(q->v, q->cap * sizeof *q->v);
	}
	i = q->n++;
	q->v[i].date = date;
	q->v[i].seq = q->seq++;
	memcpy(q->v[i].id, id, 20);
	while (i && qless(&q->v[i], &q->v[(i - 1) / 2])) {
		struct qent t = q->v[i];

		q->v[i] = q->v[(i - 1) / 2];
		q->v[(i - 1) / 2] = t;
		i = (i - 1) / 2;
	}
}

static struct qent qpop(struct queue *q)
{
	struct qent top = q->v[0];
	size_t i = 0;

	q->v[0] = q->v[--q->n];
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i;
		struct qent t;

		if (l < q->n && qless(&q->v[l], &q->v[m]))
			m = l;
		if (r < q->n && qless(&q->v[r], &q->v[m]))
			m = r;
		if (m == i)
			break;
		t = q->v[i];
		q->v[i] = q->v[m];
		q->v[m] = t;
		i = m;
	}
	return top;
}

/* A commit read and parsed; its buffer stays with it. */
struct cobj {
	buf b;
	struct commit c;
};

static void cobj_read(const unsigned char id[20], struct cobj *o)
{
	int type;

	memset(o, 0, sizeof *o);
	object_read_or_die(id, &type, &o->b);
	if (type != OBJ_COMMIT)
		die("expected a commit");
	commit_parse(&o->b, &o->c);
}

static void cobj_free(struct cobj *o)
{
	free(o->c.parent);
	buf_free(&o->b);
}

static long long commit_date(const unsigned char id[20])
{
	struct cobj o;
	long long d;

	cobj_read(id, &o);
	d = ident_secs(o.c.committer, o.c.committer_len);
	cobj_free(&o);
	return d;
}

/*
 * Trees' files, kept to the pathspec, in a few cached slots, since a walk compares each commit's tree with its
 * parent's and then the parent's with its own. tc_slot never evicts the slot avoid (-1: none), so two can be held.
 */
struct tree_cache {
	unsigned char id[4][20];
	struct files f[4];
	int used[4], next;
};

static int tc_slot(struct tree_cache *tc, const unsigned char tree[20], const struct pathspec *ps, int avoid)
{
	struct files all = { 0 };
	int i;

	for (i = 0; i < 4; i++)
		if (tc->used[i] && !memcmp(tc->id[i], tree, 20))
			return i;
	i = tc->next;
	if (i == avoid)
		i = (i + 1) % 4;
	tc->next = (i + 1) % 4;
	if (tc->used[i])
		files_free(&tc->f[i]);
	memset(&tc->f[i], 0, sizeof tc->f[i]);
	tree_flatten(tree, &all);
	if (ps->n) {
		files_keep(&all, ps, 1, &tc->f[i]);
		files_free(&all);
	} else {
		tc->f[i] = all;
	}
	memcpy(tc->id[i], tree, 20);
	tc->used[i] = 1;
	return i;
}

static int same_files(const struct files *a, const struct files *b)
{
	size_t i;

	if (a->n != b->n)
		return 0;
	for (i = 0; i < a->n; i++)
		if (a->v[i].mode != b->v[i].mode || memcmp(a->v[i].id, b->v[i].id, 20) || strcmp(a->v[i].path, b->v[i].path))
			return 0;
	return 1;
}

/* A tip as a commit: tags peeled; 0 when it names no commit (a ref to a tree or a blob, which git log --all passes over). */
static int tip_commit(unsigned char id[20])
{
	for (;;) {
		buf b = { 0 };
		int type;

		if (object_read(id, &type, &b))
			return 0;
		if (type == OBJ_COMMIT) {
			buf_free(&b);
			return 1;
		}
		if (type != OBJ_TAG || b.len < 47 || strncmp((char *)b.p, "object ", 7) || hex_to_id((char *)b.p + 7, id)) {
			buf_free(&b);
			return 0;
		}
		buf_free(&b);
	}
}

static const char log_usage[] =
	"usage: mgit log [--all] [--first-parent] [-<n> | -n <n>] [--since=<date>] [--until=<date>] [--format=<format>]\n"
	"                [--stat] [<rev>...] [-- <path>...]";

/*
 * History in git log's order: the newest committer date first over every parent (or the first parents only), from
 * the revisions named, HEAD, or with --all every ref and HEAD. With paths, git's default simplification: a commit
 * whose tree is the same as a parent's in those paths is not shown and only that parent is followed. --since stops
 * the walk at older commits; --until hides newer ones. --stat diffs each commit against its first parent (a root
 * against nothing; a merge shows none), without rename detection, as git's --no-renames.
 */
int cmd_log(int argc, char **argv)
{
	struct pathspec ps = { NULL, 0 };
	struct queue q = { 0 };
	struct idset seen = { 0 };
	struct tree_cache tc = { 0 };
	const char *fmt = NULL;
	char **revs = xmalloc((size_t)argc * sizeof *revs);
	long long since = 0, until = 0;
	long max = -1, shown = 0;
	int i, nrevs = 0, all = 0, first_parent = 0, stat = 0, have_since = 0, have_until = 0, first = 1, k;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		if (!strcmp(a, "--all")) {
			all = 1;
		} else if (!strcmp(a, "--no-renames")) {
			/* mgit never detects renames: what git shows with this */
		} else if (!strcmp(a, "--first-parent")) {
			first_parent = 1;
		} else if (!strcmp(a, "--stat")) {
			stat = 1;
		} else if (!strncmp(a, "--since=", 8) || !strncmp(a, "--after=", 8)) {
			if (parse_date(a + 8, &since))
				die("cannot read the date '%s' (mgit takes @<seconds>, '<n> <unit>s ago', YYYY-MM-DD [HH:MM[:SS]] [zone])", a + 8);
			have_since = 1;
		} else if (!strncmp(a, "--until=", 8) || !strncmp(a, "--before=", 9)) {
			const char *d = a + (a[2] == 'u' ? 8 : 9);

			if (parse_date(d, &until))
				die("cannot read the date '%s' (mgit takes @<seconds>, '<n> <unit>s ago', YYYY-MM-DD [HH:MM[:SS]] [zone])", d);
			have_until = 1;
		} else if (!strncmp(a, "--format=", 9) || !strncmp(a, "--pretty=", 9)) {
			const char *f = a + 9;

			if (!strcmp(f, "medium"))
				fmt = NULL;
			else if (!strncmp(f, "tformat:", 8))
				fmt = f + 8;
			else if (strchr(f, '%'))
				fmt = f;
			else
				die("mgit log takes --format=<string with %%-placeholders>, tformat:<...> or medium, not '%s'", f);
		} else if (!strncmp(a, "--max-count=", 12) || (!strcmp(a, "-n") && i + 1 < argc) ||
			   (a[0] == '-' && isdigit((unsigned char)a[1]))) {
			const char *v = a[1] == '-' ? a + 12 : a[1] == 'n' ? argv[++i] : a + 1;
			char *e;

			max = strtol(v, &e, 10);
			if (*e || e == v || max < 0)
				die("%s", log_usage);
		} else if (a[0] == '-') {
			die("%s", log_usage);
		} else {
			revs[nrevs++] = argv[i];
		}
	}
	repo_find();
	if (i < argc)
		pathspec_parse(&ps, argc - i, argv + i);

	if (all) {
		size_t n, j;
		char **names = ref_list("refs/", &n);

		for (j = 0; j < n; j++) {
			unsigned char id[20];
			buf name = { 0 };

			buf_printf(&name, "refs/%s", names[j]);
			if (!ref_read((char *)name.p, id) && tip_commit(id) && idset_add(&seen, id))
				qpush(&q, id, commit_date(id));
			buf_free(&name);
			free(names[j]);
		}
		free(names);
	}
	if (!nrevs && !all)
		revs[nrevs++] = "HEAD";
	for (k = 0; k < nrevs; k++) {
		unsigned char id[20];

		if (rev_parse(revs[k], id)) {
			char *head = head_symref();

			if (!strcmp(revs[k], "HEAD") && head)
				die("your current branch '%s' does not have any commits yet",
				    strncmp(head, "refs/heads/", 11) ? head : head + 11);
			die("unknown revision %s", revs[k]);
		}
		peel_to_commit(id);
		if (idset_add(&seen, id))
			qpush(&q, id, commit_date(id));
	}
	if (all) {
		unsigned char id[20];

		if (!ref_read("HEAD", id) && tip_commit(id) && idset_add(&seen, id))
			qpush(&q, id, commit_date(id));
	}
	free(revs);

	while (q.n && (max < 0 || shown < max)) {
		struct qent e = qpop(&q);
		struct cobj o;
		struct commit *c = &o.c;
		int np, show = 1;
		buf out = { 0 };

		if (have_since && e.date < since)
			continue;
		cobj_read(e.id, &o);
		np = first_parent && c->nparent > 1 ? 1 : c->nparent;
		if (ps.n) {
			int sc = tc_slot(&tc, c->tree, &ps, -1);
			struct files none = { 0 };

			if (!np) {
				show = !same_files(&tc.f[sc], &none);
			} else {
				for (k = 0; k < np; k++) {
					unsigned char pt[20];
					int sp;

					commit_tree_id(c->parent[k], pt);
					sc = tc_slot(&tc, c->tree, &ps, -1);
					sp = tc_slot(&tc, pt, &ps, sc);
					if (same_files(&tc.f[sc], &tc.f[sp])) {
						memmove(c->parent[0], c->parent[k], 20);
						np = 1;
						show = 0;
						break;
					}
				}
			}
		}
		for (k = 0; k < np; k++)
			if (idset_add(&seen, c->parent[k]))
				qpush(&q, c->parent[k], commit_date(c->parent[k]));
		if (have_until && e.date > until)
			show = 0;
		if (!show) {
			cobj_free(&o);
			continue;
		}
		shown++;
		if (fmt) {
			put_format(&out, fmt, e.id, c);
		} else {
			char hex[41];

			if (!first)
				buf_addc(&out, '\n');
			id_to_hex(e.id, hex);
			buf_printf(&out, "commit %s\n", hex);
			if (c->nparent > 1) {
				buf_adds(&out, "Merge:");
				for (k = 0; k < c->nparent; k++) {
					id_to_hex(c->parent[k], hex);
					buf_printf(&out, " %.*s", abbrev_len(c->parent[k], -1), hex);
				}
				buf_addc(&out, '\n');
			}
			put_author(&out, c->author, c->author_len);
			buf_addc(&out, '\n');
			put_message(&out, c->msg, c->msg_len);
			while (out.len && git_space((char)out.p[out.len - 1]))
				out.len--;
			buf_addc(&out, '\n');
		}
		first = 0;
		fwrite(out.p, 1, out.len, stdout);
		buf_free(&out);
		if (stat && c->nparent < 2) {
			struct files none = { 0 };
			int sb = tc_slot(&tc, c->tree, &ps, -1);

			if (c->nparent) {
				unsigned char pt[20];
				int sa;

				commit_tree_id(c->parent[0], pt);
				sa = tc_slot(&tc, pt, &ps, sb);
				diff_files_show(&tc.f[sa], &tc.f[sb], 's', "\n");
			} else {
				diff_files_show(&none, &tc.f[sb], 's', "\n");
			}
		}
		cobj_free(&o);
	}
	free(q.v);
	idset_free(&seen);
	return 0;
}
