#define _DEFAULT_SOURCE
#include "mgit.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static void status_header(const struct repo_state *s)
{
	char hex[41];

	if (s->head_ref && !strncmp(s->head_ref, "refs/heads/", 11)) {
		printf("On branch %s%s\n", s->head_ref + 11, s->have_head ? "" : " (no commits yet)");
	} else if (s->head_ref) {
		printf("On %s\n", s->head_ref);
	} else {
		id_to_hex(s->head, hex);
		printf("HEAD detached at %.7s\n", hex);
	}
}

/* Changes since HEAD in git diff --name-status form (A, D, M, T for file/symlink swaps), as git add -A would stage them. */
int cmd_status(int argc, char **argv)
{
	struct repo_state s;
	struct files w = { 0 };
	struct pair *ch;
	size_t n, i;
	buf out = { 0 };

	(void)argv;
	if (argc != 1)
		die("usage: mgit status");
	repo_find();
	state_load(&s);
	worktree_scan(&s, 0, NULL, &w);
	status_header(&s);
	n = files_diff(&s.head_files, &w, &ch);
	for (i = 0; i < n; i++) {
		const struct file *a = ch[i].a, *b = ch[i].b;

		buf_addc(&out, !a ? 'A' : !b ? 'D' : (a->mode ^ b->mode) & 0170000 ? 'T' : 'M');
		buf_addc(&out, '\t');
		buf_quote(&out, "", a ? a->path : b->path);
		buf_addc(&out, '\n');
	}
	if (!n)
		buf_adds(&out, "nothing to commit, working tree clean\n");
	fwrite(out.p, 1, out.len, stdout);
	buf_free(&out);
	free(ch);
	return 0;
}

struct line {
	const char *p;
	size_t len; /* including the '\n', if there is one */
};

static size_t split_lines(const buf *b, struct line **out)
{
	const char *p = (const char *)b->p, *end = p + b->len;
	struct line *v = NULL;
	size_t n = 0;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t len = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);

		v = xrealloc(v, (n + 1) * sizeof *v);
		v[n].p = p;
		v[n].len = len;
		n++;
		p += len;
	}
	*out = v;
	return n;
}

/* Gives equal lines equal small integers, so the diff compares ints. */
static void number_lines(const struct line *a, size_t na, const struct line *b, size_t nb, long *ia, long *ib)
{
	size_t cap = 64, i, k, nuniq = 0;
	long *slot;
	const struct line **rep;

	while (cap < 2 * (na + nb))
		cap *= 2;
	slot = xmalloc(cap * sizeof *slot);
	rep = xmalloc((na + nb + 1) * sizeof *rep);
	for (i = 0; i < cap; i++)
		slot[i] = -1;
	for (k = 0; k < na + nb; k++) {
		const struct line *l = k < na ? &a[k] : &b[k - na];
		uint32_t h = 2166136261u;

		for (i = 0; i < l->len; i++)
			h = (h ^ (unsigned char)l->p[i]) * 16777619u;
		for (i = h & (cap - 1); slot[i] >= 0; i = (i + 1) & (cap - 1)) {
			const struct line *r = rep[slot[i]];

			if (r->len == l->len && !memcmp(r->p, l->p, l->len))
				break;
		}
		if (slot[i] < 0) {
			slot[i] = (long)nuniq;
			rep[nuniq++] = l;
		}
		if (k < na)
			ia[k] = slot[i];
		else
			ib[k - na] = slot[i];
	}
	free(slot);
	free(rep);
}

/*
 * Myers' O(ND) difference with the linear-space middle-snake split, in the form libxdiff uses.
 * Diagonals are indexed by i1 - i2 in absolute coordinates. Always minimal: no cost cutoffs.
 */
struct myers {
	const long *a, *b;
	char *dela, *insb;
	long *kvdf, *kvdb;
};

static void split(struct myers *m, long off1, long lim1, long off2, long lim2, long *s1, long *s2)
{
	const long *a = m->a, *b = m->b;
	long *kvdf = m->kvdf, *kvdb = m->kvdb;
	long dmin = off1 - lim2, dmax = lim1 - off2, fmid = off1 - off2, bmid = lim1 - lim2;
	long odd = (fmid - bmid) & 1, fmin = fmid, fmax = fmid, bmin = bmid, bmax = bmid, d, i1, i2;

	kvdf[fmid] = off1;
	kvdb[bmid] = lim1;
	for (;;) {
		if (fmin > dmin)
			kvdf[--fmin - 1] = -1;
		else
			++fmin;
		if (fmax < dmax)
			kvdf[++fmax + 1] = -1;
		else
			--fmax;
		for (d = fmax; d >= fmin; d -= 2) {
			i1 = kvdf[d - 1] >= kvdf[d + 1] ? kvdf[d - 1] + 1 : kvdf[d + 1];
			i2 = i1 - d;
			while (i1 < lim1 && i2 < lim2 && a[i1] == b[i2])
				i1++, i2++;
			kvdf[d] = i1;
			if (odd && bmin <= d && d <= bmax && kvdb[d] <= i1) {
				*s1 = i1;
				*s2 = i2;
				return;
			}
		}
		if (bmin > dmin)
			kvdb[--bmin - 1] = LONG_MAX;
		else
			++bmin;
		if (bmax < dmax)
			kvdb[++bmax + 1] = LONG_MAX;
		else
			--bmax;
		for (d = bmax; d >= bmin; d -= 2) {
			i1 = kvdb[d - 1] < kvdb[d + 1] ? kvdb[d - 1] : kvdb[d + 1] - 1;
			i2 = i1 - d;
			while (i1 > off1 && i2 > off2 && a[i1 - 1] == b[i2 - 1])
				i1--, i2--;
			kvdb[d] = i1;
			if (!odd && fmin <= d && d <= fmax && i1 <= kvdf[d]) {
				*s1 = i1;
				*s2 = i2;
				return;
			}
		}
	}
}

static void compare(struct myers *m, long off1, long lim1, long off2, long lim2)
{
	while (off1 < lim1 && off2 < lim2 && m->a[off1] == m->b[off2])
		off1++, off2++;
	while (off1 < lim1 && off2 < lim2 && m->a[lim1 - 1] == m->b[lim2 - 1])
		lim1--, lim2--;
	if (off1 == lim1) {
		while (off2 < lim2)
			m->insb[off2++] = 1;
	} else if (off2 == lim2) {
		while (off1 < lim1)
			m->dela[off1++] = 1;
	} else {
		long s1, s2;

		split(m, off1, lim1, off2, lim2, &s1, &s2);
		compare(m, off1, s1, off2, s2);
		compare(m, s1, lim1, s2, lim2);
	}
}

struct change {
	long i1, n1, i2, n2;
};

static size_t diff_lines(const struct line *a, long na, const struct line *b, long nb, struct change **out)
{
	long *ia = xmalloc((size_t)(na + 1) * sizeof *ia), *ib = xmalloc((size_t)(nb + 1) * sizeof *ib);
	long *kv = xmalloc((size_t)(2 * (na + nb + 3)) * sizeof *kv), i = 0, j = 0;
	struct myers m;
	struct change *v = NULL;
	size_t n = 0;

	number_lines(a, (size_t)na, b, (size_t)nb, ia, ib);
	m.a = ia;
	m.b = ib;
	m.dela = calloc((size_t)na + 1, 1);
	m.insb = calloc((size_t)nb + 1, 1);
	if (!m.dela || !m.insb)
		die("out of memory");
	m.kvdf = kv + nb + 1;
	m.kvdb = kv + (na + nb + 3) + nb + 1;
	compare(&m, 0, na, 0, nb);
	while (i < na || j < nb) {
		struct change c;

		if (i < na && j < nb && !m.dela[i] && !m.insb[j]) {
			i++, j++;
			continue;
		}
		c.i1 = i;
		c.i2 = j;
		while (i < na && m.dela[i])
			i++;
		while (j < nb && m.insb[j])
			j++;
		c.n1 = i - c.i1;
		c.n2 = j - c.i2;
		if (!c.n1 && !c.n2)
			die("diff: inconsistent edit script");
		v = xrealloc(v, (n + 1) * sizeof *v);
		v[n++] = c;
	}
	free(ia);
	free(ib);
	free(kv);
	free(m.dela);
	free(m.insb);
	*out = v;
	return n;
}

static void emit_line(buf *out, int c, const struct line *l)
{
	buf_addc(out, c);
	buf_add(out, l->p, l->len);
	if (!l->len || l->p[l->len - 1] != '\n')
		buf_adds(out, "\n\\ No newline at end of file\n");
}

static void emit_range(buf *out, long start, long count)
{
	buf_printf(out, "%ld", count ? start + 1 : start);
	if (count != 1)
		buf_printf(out, ",%ld", count);
}

/* libxdiff's default function-name rule: the nearest line above the hunk that starts with a letter, '_' or '$'. */
static void emit_funcname(buf *out, const struct line *a, long before)
{
	while (before-- > 0) {
		const struct line *l = &a[before];
		size_t len = l->len < 80 ? l->len : 80;

		if (!len || !(isalpha((unsigned char)l->p[0]) || l->p[0] == '_' || l->p[0] == '$'))
			continue;
		while (len && isspace((unsigned char)l->p[len - 1]))
			len--;
		buf_addc(out, ' ');
		buf_add(out, l->p, len);
		return;
	}
}

/* Unified diff with 3 lines of context; changes up to 6 lines apart share a hunk, as in git. */
static void unified(buf *out, const buf *old, const buf *new, const char *la, const char *lb)
{
	struct line *a, *b;
	long na = (long)split_lines(old, &a), nb = (long)split_lines(new, &b);
	struct change *ch;
	size_t n = diff_lines(a, na, b, nb, &ch), k = 0;

	if (n) {
		buf_printf(out, "--- %s%s\n", la, strchr(la, ' ') ? "\t" : "");
		buf_printf(out, "+++ %s%s\n", lb, strchr(lb, ' ') ? "\t" : "");
	}
	while (k < n) {
		size_t first = k, last = k;
		long s1, s2, e1, e2, i, j;

		while (last + 1 < n && ch[last + 1].i1 - (ch[last].i1 + ch[last].n1) <= 6)
			last++;
		s1 = ch[first].i1 - 3 > 0 ? ch[first].i1 - 3 : 0;
		s2 = ch[first].i2 - 3 > 0 ? ch[first].i2 - 3 : 0;
		e1 = ch[last].i1 + ch[last].n1 + 3 < na ? ch[last].i1 + ch[last].n1 + 3 : na;
		e2 = ch[last].i2 + ch[last].n2 + 3 < nb ? ch[last].i2 + ch[last].n2 + 3 : nb;
		buf_adds(out, "@@ -");
		emit_range(out, s1, e1 - s1);
		buf_adds(out, " +");
		emit_range(out, s2, e2 - s2);
		buf_adds(out, " @@");
		emit_funcname(out, a, s1);
		buf_addc(out, '\n');
		for (i = s1, j = s2; k <= last; k++) {
			for (; i < ch[k].i1; i++, j++)
				emit_line(out, ' ', &b[j]);
			for (; i < ch[k].i1 + ch[k].n1; i++)
				emit_line(out, '-', &a[i]);
			for (; j < ch[k].i2 + ch[k].n2; j++)
				emit_line(out, '+', &b[j]);
		}
		for (; i < e1; i++, j++)
			emit_line(out, ' ', &b[j]);
	}
	free(a);
	free(b);
	free(ch);
}

static int is_binary(const buf *b)
{
	return memchr(b->p, 0, b->len < 8000 ? b->len : 8000) != NULL;
}

/* What git diffs for a submodule: one line naming its commit. */
static void subproject(const struct file *f, buf *out)
{
	char hex[41];

	id_to_hex(f->id, hex);
	buf_printf(out, "Subproject commit %s\n", hex);
}

static void load_head(const struct file *f, buf *out)
{
	int type;

	if (f->mode == 0160000)
		subproject(f, out);
	else
		object_read_or_die(f->id, &type, out);
}

static void load_worktree(const struct file *f, buf *out)
{
	char *full;

	if (f->mode == 0160000) {
		subproject(f, out);
		return;
	}
	full = path_join(work_tree, f->path);

	if (f->mode == 0120000 ? read_link(full, out) : read_file(full, out))
		die("cannot read %s: %s", full, strerror(errno));
	free(full);
}

/* One file's patch: a is the HEAD entry, b the working-tree entry; either may be NULL. */
static void emit_patch(buf *out, const struct file *a, const struct file *b)
{
	const char *path = a ? a->path : b->path;
	buf la = { 0 }, lb = { 0 }, old = { 0 }, new = { 0 };
	char ha[41] = "0000000", hb[41] = "0000000";

	if (a)
		id_to_hex(a->id, ha);
	if (b)
		id_to_hex(b->id, hb);
	buf_quote(&la, "a/", path);
	buf_quote(&lb, "b/", path);
	buf_printf(out, "diff --git %s %s\n", (char *)la.p, (char *)lb.p);
	if (!a)
		buf_printf(out, "new file mode %06o\n", b->mode);
	else if (!b)
		buf_printf(out, "deleted file mode %06o\n", a->mode);
	else if (a->mode != b->mode)
		buf_printf(out, "old mode %06o\nnew mode %06o\n", a->mode, b->mode);
	if (!a || !b || memcmp(a->id, b->id, 20)) {
		buf_printf(out, "index %.7s..%.7s", ha, hb);
		if (a && b && a->mode == b->mode)
			buf_printf(out, " %06o", a->mode);
		buf_addc(out, '\n');
		if (a)
			load_head(a, &old);
		if (b)
			load_worktree(b, &new);
		if (!a) {
			la.len = 0;
			buf_adds(&la, "/dev/null");
		}
		if (!b) {
			lb.len = 0;
			buf_adds(&lb, "/dev/null");
		}
		if ((old.len && is_binary(&old)) || (new.len && is_binary(&new)))
			buf_printf(out, "Binary files %s and %s differ\n", (char *)la.p, (char *)lb.p);
		else
			unified(out, &old, &new, (char *)la.p, (char *)lb.p);
	}
	buf_free(&la);
	buf_free(&lb);
	buf_free(&old);
	buf_free(&new);
}

/* One file's line of a diffstat. added and deleted are lines, or for a binary file the new and old sizes in bytes. */
struct stat_row {
	char *name;
	long added, deleted;
	int binary;
};

static void stat_row(struct stat_row *r, const struct file *a, const struct file *b)
{
	buf old = { 0 }, new = { 0 }, name = { 0 };

	buf_quote(&name, "", a ? a->path : b->path);
	r->name = (char *)name.p;
	r->added = r->deleted = 0;
	if (a)
		load_head(a, &old);
	if (a && b && !memcmp(a->id, b->id, 20)) {
		r->binary = is_binary(&old); /* a mode change: no lines, "Bin" alone for a binary file */
	} else {
		if (b)
			load_worktree(b, &new);
		r->binary = (old.len && is_binary(&old)) || (new.len && is_binary(&new));
		if (r->binary) {
			r->added = (long)new.len;
			r->deleted = (long)old.len;
		} else {
			struct line *la, *lb;
			long na = (long)split_lines(&old, &la), nb = (long)split_lines(&new, &lb);
			struct change *ch;
			size_t n = diff_lines(la, na, lb, nb, &ch), k;

			for (k = 0; k < n; k++) {
				r->deleted += ch[k].n1;
				r->added += ch[k].n2;
			}
			free(la);
			free(lb);
			free(ch);
		}
	}
	buf_free(&old);
	buf_free(&new);
}

static long decimal_width(long n)
{
	long w = 1;

	while (n >= 10) {
		n /= 10;
		w++;
	}
	return w;
}

static long scale_linear(long it, long width, long max_change)
{
	return it ? 1 + it * (width - 1) / max_change : 0;
}

/* git's term_columns(): $COLUMNS, else the terminal on standard output, else 80. */
static long stat_columns(void)
{
	const char *c = getenv("COLUMNS");
	struct winsize ws;

	if (c && atol(c) > 0)
		return atol(c);
	if (!ioctl(1, TIOCGWINSZ, &ws) && ws.ws_col)
		return ws.ws_col;
	return 80;
}

/* git diff --stat's layout (show_stats in git's diff.c): names cut from the left behind "...", bars scaled to fit. */
static void show_stats(buf *out, const struct stat_row *v, size_t n)
{
	long max_change = 0, max_len = 0, number_width = 0, bin_width = 0, width, name_width, graph_width, adds = 0, dels = 0;
	size_t i;

	if (!n)
		return;
	for (i = 0; i < n; i++) {
		long len = (long)strlen(v[i].name);

		if (max_len < len)
			max_len = len;
		if (v[i].binary) {
			long w = 14 + decimal_width(v[i].added) + decimal_width(v[i].deleted);

			if (bin_width < w)
				bin_width = w;
			number_width = 3;
		} else if (max_change < v[i].added + v[i].deleted) {
			max_change = v[i].added + v[i].deleted;
		}
	}
	width = stat_columns();
	if (number_width < decimal_width(max_change))
		number_width = decimal_width(max_change);
	if (width < 16 + 6 + number_width)
		width = 16 + 6 + number_width;
	graph_width = max_change + 4 > bin_width ? max_change : bin_width - 4;
	name_width = max_len;
	if (name_width + number_width + 6 + graph_width > width) {
		if (graph_width > width * 3 / 8 - number_width - 6) {
			graph_width = width * 3 / 8 - number_width - 6;
			if (graph_width < 6)
				graph_width = 6;
		}
		if (name_width > width - number_width - 6 - graph_width)
			name_width = width - number_width - 6 - graph_width;
		else
			graph_width = width - number_width - 6 - name_width;
	}
	for (i = 0; i < n; i++) {
		const char *prefix = "", *name = v[i].name, *slash;
		long len = name_width, padding, add = v[i].added, del = v[i].deleted;

		if (name_width < (long)strlen(name)) {
			prefix = "...";
			len = len > 3 ? len - 3 : 0;
			name += strlen(name) - (size_t)len; /* quoted names are ASCII: one column a byte */
			if ((slash = strchr(name, '/')))
				name = slash;
		}
		padding = len - (long)strlen(name);
		if (padding < 0)
			padding = 0;
		if (v[i].binary) {
			buf_printf(out, " %s%s%*s | %*s", prefix, name, (int)padding, "", (int)number_width, "Bin");
			if (add || del)
				buf_printf(out, " %ld -> %ld bytes", del, add);
			buf_addc(out, '\n');
			continue;
		}
		adds += add;
		dels += del;
		if (graph_width <= max_change) {
			long total = scale_linear(add + del, graph_width, max_change);

			if (total < 2 && add && del)
				total = 2;
			if (add < del) {
				add = scale_linear(add, graph_width, max_change);
				del = total - add;
			} else {
				del = scale_linear(del, graph_width, max_change);
				add = total - del;
			}
		}
		buf_printf(out, " %s%s%*s | %*ld%s", prefix, name, (int)padding, "", (int)number_width,
			   v[i].added + v[i].deleted, v[i].added + v[i].deleted ? " " : "");
		while (add-- > 0)
			buf_addc(out, '+');
		while (del-- > 0)
			buf_addc(out, '-');
		buf_addc(out, '\n');
	}
	buf_printf(out, " %zu file%s changed", n, n == 1 ? "" : "s");
	if (adds || !dels)
		buf_printf(out, ", %ld insertion%s(+)", adds, adds == 1 ? "" : "s");
	if (dels || !adds)
		buf_printf(out, ", %ld deletion%s(-)", dels, dels == 1 ? "" : "s");
	buf_addc(out, '\n');
}

static const char diff_usage[] = "usage: mgit diff [--quiet | --name-only | --stat] [-- <path>...]";

/*
 * HEAD to the working tree, covering what status lists, as a patch, the names, a diffstat, or only the exit status
 * (--quiet: 1 when anything differs, as git's --exit-code). No rename detection.
 */
int cmd_diff(int argc, char **argv)
{
	struct repo_state s;
	struct files w = { 0 }, hf = { 0 };
	struct pathspec ps = { NULL, 0 };
	struct pair *ch;
	struct stat_row *rows;
	size_t n, i;
	buf out = { 0 };
	int mode = 0;

	for (i = 1; i < (size_t)argc; i++) {
		int m = !strcmp(argv[i], "--quiet") ? 'q' : !strcmp(argv[i], "--name-only") ? 'n' : !strcmp(argv[i], "--stat") ? 's' : 0;

		if (!strcmp(argv[i], "--"))
			break;
		if (!m || mode)
			die("%s", diff_usage);
		mode = m;
	}
	repo_find();
	state_load(&s);
	if (i + 1 < (size_t)argc) {
		pathspec_parse(&ps, argc - (int)i - 1, argv + i + 1);
		files_keep(&s.head_files, &ps, 1, &hf);
	} else {
		hf = s.head_files;
	}
	worktree_scan(&s, 0, &ps, &w);
	n = files_diff(&hf, &w, &ch);
	if (mode == 'q')
		return n > 0;
	rows = xmalloc((n ? n : 1) * sizeof *rows);
	for (i = 0; i < n; i++) {
		const struct file *a = ch[i].a, *b = ch[i].b;

		if (mode == 'n') {
			buf_quote(&out, "", a ? a->path : b->path);
			buf_addc(&out, '\n');
		} else if (mode == 's') {
			stat_row(&rows[i], a, b);
		} else if (a && b && (a->mode ^ b->mode) & 0170000) {
			/* git shows a file/symlink swap as a deletion followed by a creation */
			emit_patch(&out, a, NULL);
			emit_patch(&out, NULL, b);
		} else {
			emit_patch(&out, a, b);
		}
		fwrite(out.p, 1, out.len, stdout);
		out.len = 0;
	}
	if (mode == 's') {
		show_stats(&out, rows, n);
		fwrite(out.p, 1, out.len, stdout);
		for (i = 0; i < n; i++)
			free(rows[i].name);
	}
	free(rows);
	buf_free(&out);
	free(ch);
	return 0;
}
