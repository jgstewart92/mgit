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

/* Gives equal lines equal small integers (their class), so the diff compares ints. Returns the number of classes. */
static long number_lines(const struct line *a, size_t na, const struct line *b, size_t nb, long *ia, long *ib)
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
	return (long)nuniq;
}

/*
 * The diff git diff makes by default, ported from git's xdiff (xprepare.c and xdiffi.c): the common ends trimmed,
 * lines with no match in the other file marked changed and lines with many matches set aside when they sit among
 * those, Myers' divide and conquer with xdiff's cost heuristics (so a large diff may be a little longer than
 * minimal, as git's is), then every change slid as git slides it, with its indent heuristic.
 */
enum { X_MAX_COST_MIN = 256, X_HEUR_MIN_COST = 256, X_SNAKE_CNT = 20, X_K_HEUR = 4, X_MAX_EQLIMIT = 1024,
       X_SIMSCAN_WINDOW = 100, X_KPDIS_RUN = 4 };

struct xfile {
	const struct line *rec;
	const long *cls;	/* each line's class */
	long nrec;
	char *rchg;		/* changed lines; rchg[-1] and rchg[nrec] exist and stay 0 */
	long *rindex, *ha, nreff;	/* the lines left for the comparison: their index and class */
	long dstart, dend;
};

struct xsplit {
	long i1, i2;
	int min_lo, min_hi;
};

struct xenv {
	long mxcost, snake_cnt, heur_min;
};

static long bogosqrt(long n)
{
	long i;

	for (i = 1; n > 0; n >>= 2)
		i <<= 1;
	return i;
}

static void xsplit(const long *ha1, long off1, long lim1, const long *ha2, long off2, long lim2, long *kvdf, long *kvdb,
		   int need_min, struct xsplit *spl, const struct xenv *xenv)
{
	long dmin = off1 - lim2, dmax = lim1 - off2, fmid = off1 - off2, bmid = lim1 - lim2;
	long odd = (fmid - bmid) & 1, fmin = fmid, fmax = fmid, bmin = bmid, bmax = bmid;
	long ec, d, i1, i2, prev1, best, dd, v, k;

	kvdf[fmid] = off1;
	kvdb[bmid] = lim1;
	for (ec = 1;; ec++) {
		int got_snake = 0;

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
			prev1 = i1;
			i2 = i1 - d;
			for (; i1 < lim1 && i2 < lim2 && ha1[i1] == ha2[i2]; i1++, i2++)
				;
			if (i1 - prev1 > xenv->snake_cnt)
				got_snake = 1;
			kvdf[d] = i1;
			if (odd && bmin <= d && d <= bmax && kvdb[d] <= i1) {
				spl->i1 = i1;
				spl->i2 = i2;
				spl->min_lo = spl->min_hi = 1;
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
			prev1 = i1;
			i2 = i1 - d;
			for (; i1 > off1 && i2 > off2 && ha1[i1 - 1] == ha2[i2 - 1]; i1--, i2--)
				;
			if (prev1 - i1 > xenv->snake_cnt)
				got_snake = 1;
			kvdb[d] = i1;
			if (!odd && fmin <= d && d <= fmax && i1 <= kvdf[d]) {
				spl->i1 = i1;
				spl->i2 = i2;
				spl->min_lo = spl->min_hi = 1;
				return;
			}
		}
		if (need_min)
			continue;
		if (got_snake && ec > xenv->heur_min) {
			for (best = 0, d = fmax; d >= fmin; d -= 2) {
				dd = d > fmid ? d - fmid : fmid - d;
				i1 = kvdf[d];
				i2 = i1 - d;
				v = (i1 - off1) + (i2 - off2) - dd;
				if (v > X_K_HEUR * ec && v > best && off1 + xenv->snake_cnt <= i1 && i1 < lim1 &&
				    off2 + xenv->snake_cnt <= i2 && i2 < lim2) {
					for (k = 1; ha1[i1 - k] == ha2[i2 - k]; k++)
						if (k == xenv->snake_cnt) {
							best = v;
							spl->i1 = i1;
							spl->i2 = i2;
							break;
						}
				}
			}
			if (best > 0) {
				spl->min_lo = 1;
				spl->min_hi = 0;
				return;
			}
			for (best = 0, d = bmax; d >= bmin; d -= 2) {
				dd = d > bmid ? d - bmid : bmid - d;
				i1 = kvdb[d];
				i2 = i1 - d;
				v = (lim1 - i1) + (lim2 - i2) - dd;
				if (v > X_K_HEUR * ec && v > best && off1 < i1 && i1 <= lim1 - xenv->snake_cnt && off2 < i2 &&
				    i2 <= lim2 - xenv->snake_cnt) {
					for (k = 0; ha1[i1 + k] == ha2[i2 + k]; k++)
						if (k == xenv->snake_cnt - 1) {
							best = v;
							spl->i1 = i1;
							spl->i2 = i2;
							break;
						}
				}
			}
			if (best > 0) {
				spl->min_lo = 0;
				spl->min_hi = 1;
				return;
			}
		}
		if (ec >= xenv->mxcost) {
			long fbest = -1, fbest1 = -1, bbest = LONG_MAX, bbest1 = LONG_MAX;

			for (d = fmax; d >= fmin; d -= 2) {
				i1 = kvdf[d] < lim1 ? kvdf[d] : lim1;
				i2 = i1 - d;
				if (lim2 < i2) {
					i1 = lim2 + d;
					i2 = lim2;
				}
				if (fbest < i1 + i2) {
					fbest = i1 + i2;
					fbest1 = i1;
				}
			}
			for (d = bmax; d >= bmin; d -= 2) {
				i1 = kvdb[d] > off1 ? kvdb[d] : off1;
				i2 = i1 - d;
				if (i2 < off2) {
					i1 = off2 + d;
					i2 = off2;
				}
				if (i1 + i2 < bbest) {
					bbest = i1 + i2;
					bbest1 = i1;
				}
			}
			if ((lim1 + lim2) - bbest < fbest - (off1 + off2)) {
				spl->i1 = fbest1;
				spl->i2 = fbest - fbest1;
				spl->min_lo = 1;
				spl->min_hi = 0;
			} else {
				spl->i1 = bbest1;
				spl->i2 = bbest - bbest1;
				spl->min_lo = 0;
				spl->min_hi = 1;
			}
			return;
		}
	}
}

static void xrecs_cmp(struct xfile *x1, long off1, long lim1, struct xfile *x2, long off2, long lim2, long *kvdf,
		      long *kvdb, int need_min, const struct xenv *xenv)
{
	const long *ha1 = x1->ha, *ha2 = x2->ha;

	for (; off1 < lim1 && off2 < lim2 && ha1[off1] == ha2[off2]; off1++, off2++)
		;
	for (; off1 < lim1 && off2 < lim2 && ha1[lim1 - 1] == ha2[lim2 - 1]; lim1--, lim2--)
		;
	if (off1 == lim1) {
		for (; off2 < lim2; off2++)
			x2->rchg[x2->rindex[off2]] = 1;
	} else if (off2 == lim2) {
		for (; off1 < lim1; off1++)
			x1->rchg[x1->rindex[off1]] = 1;
	} else {
		struct xsplit spl = { 0, 0, 0, 0 };

		xsplit(ha1, off1, lim1, ha2, off2, lim2, kvdf, kvdb, need_min, &spl, xenv);
		xrecs_cmp(x1, off1, spl.i1, x2, off2, spl.i2, kvdf, kvdb, spl.min_lo, xenv);
		xrecs_cmp(x1, spl.i1, lim1, x2, spl.i2, lim2, kvdf, kvdb, spl.min_hi, xenv);
	}
}

/* xdl_clean_mmatch: a line with many matches is set aside only inside a run of lines that have none. */
static int clean_mmatch(const char *dis, long i, long s, long e)
{
	long r, rdis0, rpdis0, rdis1, rpdis1;

	if (i - s > X_SIMSCAN_WINDOW)
		s = i - X_SIMSCAN_WINDOW;
	if (e - i > X_SIMSCAN_WINDOW)
		e = i + X_SIMSCAN_WINDOW;
	for (r = 1, rdis0 = 0, rpdis0 = 1; (i - r) >= s; r++) {
		if (!dis[i - r])
			rdis0++;
		else if (dis[i - r] == 2)
			rpdis0++;
		else
			break;
	}
	if (rdis0 == 0)
		return 0;
	for (r = 1, rdis1 = 0, rpdis1 = 1; (i + r) <= e; r++) {
		if (!dis[i + r])
			rdis1++;
		else if (dis[i + r] == 2)
			rpdis1++;
		else
			break;
	}
	if (rdis1 == 0)
		return 0;
	rdis1 += rdis0;
	rpdis1 += rpdis0;
	return rpdis1 * X_KPDIS_RUN < (rpdis1 + rdis1);
}

/* xdl_cleanup_records for one file: other_count[c] is how often class c occurs in the other file. */
static void cleanup(struct xfile *x, const long *other_count)
{
	long i, mlim = bogosqrt(x->nrec);
	char *dis = calloc((size_t)x->nrec + 1, 1);

	if (!dis)
		die("out of memory");
	if (mlim > X_MAX_EQLIMIT)
		mlim = X_MAX_EQLIMIT;
	for (i = x->dstart; i <= x->dend; i++) {
		long nm = other_count[x->cls[i]];

		dis[i] = nm == 0 ? 0 : nm >= mlim ? 2 : 1;
	}
	x->nreff = 0;
	for (i = x->dstart; i <= x->dend; i++) {
		if (dis[i] == 1 || (dis[i] == 2 && !clean_mmatch(dis, i, x->dstart, x->dend))) {
			x->rindex[x->nreff] = i;
			x->ha[x->nreff] = x->cls[i];
			x->nreff++;
		} else {
			x->rchg[i] = 1;
		}
	}
	free(dis);
}

/* Sliding a group of changed lines (xdiffi.c's group_*), and the indent heuristic that chooses where it goes. */
struct xgroup {
	long start, end;
};

static void group_init(const struct xfile *x, struct xgroup *g)
{
	g->start = g->end = 0;
	while (x->rchg[g->end])
		g->end++;
}

static int group_next(const struct xfile *x, struct xgroup *g)
{
	if (g->end == x->nrec)
		return -1;
	g->start = g->end + 1;
	for (g->end = g->start; x->rchg[g->end]; g->end++)
		;
	return 0;
}

static int group_previous(const struct xfile *x, struct xgroup *g)
{
	if (g->start == 0)
		return -1;
	g->end = g->start - 1;
	for (g->start = g->end; x->rchg[g->start - 1]; g->start--)
		;
	return 0;
}

static int group_slide_down(struct xfile *x, struct xgroup *g)
{
	if (g->end < x->nrec && x->cls[g->start] == x->cls[g->end]) {
		x->rchg[g->start++] = 0;
		x->rchg[g->end++] = 1;
		while (x->rchg[g->end])
			g->end++;
		return 0;
	}
	return -1;
}

static int group_slide_up(struct xfile *x, struct xgroup *g)
{
	if (g->start > 0 && x->cls[g->start - 1] == x->cls[g->end - 1]) {
		x->rchg[--g->start] = 1;
		x->rchg[--g->end] = 0;
		while (x->rchg[g->start - 1])
			g->start--;
		return 0;
	}
	return -1;
}

enum { MAX_INDENT = 200, MAX_BLANKS = 20, INDENT_HEURISTIC_MAX_SLIDING = 100, START_OF_FILE_PENALTY = 1,
       END_OF_FILE_PENALTY = 21, TOTAL_BLANK_WEIGHT = -30, POST_BLANK_WEIGHT = 6, RELATIVE_INDENT_PENALTY = -4,
       RELATIVE_INDENT_WITH_BLANK_PENALTY = 10, RELATIVE_OUTDENT_PENALTY = 24, RELATIVE_OUTDENT_WITH_BLANK_PENALTY = 17,
       RELATIVE_DEDENT_PENALTY = 23, RELATIVE_DEDENT_WITH_BLANK_PENALTY = 17, INDENT_WEIGHT = 60 };

struct split_measurement {
	int end_of_file, indent, pre_blank, pre_indent, post_blank, post_indent;
};

struct split_score {
	int effective_indent, penalty;
};

/* A line's indent with tabs to 8 columns, -1 for a line of only whitespace (git's isspace: space, \t, \n, \r). */
static int get_indent(const struct line *l)
{
	size_t i;
	int ret = 0;

	for (i = 0; i < l->len; i++) {
		char c = l->p[i];

		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			return ret;
		if (c == ' ')
			ret += 1;
		else if (c == '\t')
			ret += 8 - ret % 8;
		if (ret >= MAX_INDENT)
			return MAX_INDENT;
	}
	return -1;
}

static void measure_split(const struct xfile *x, long split, struct split_measurement *m)
{
	long i;

	if (split >= x->nrec) {
		m->end_of_file = 1;
		m->indent = -1;
	} else {
		m->end_of_file = 0;
		m->indent = get_indent(&x->rec[split]);
	}
	m->pre_blank = 0;
	m->pre_indent = -1;
	for (i = split - 1; i >= 0; i--) {
		m->pre_indent = get_indent(&x->rec[i]);
		if (m->pre_indent != -1)
			break;
		m->pre_blank += 1;
		if (m->pre_blank == MAX_BLANKS) {
			m->pre_indent = 0;
			break;
		}
	}
	m->post_blank = 0;
	m->post_indent = -1;
	for (i = split + 1; i < x->nrec; i++) {
		m->post_indent = get_indent(&x->rec[i]);
		if (m->post_indent != -1)
			break;
		m->post_blank += 1;
		if (m->post_blank == MAX_BLANKS) {
			m->post_indent = 0;
			break;
		}
	}
}

static void score_add_split(const struct split_measurement *m, struct split_score *s)
{
	int post_blank, total_blank, indent, any_blanks;

	if (m->pre_indent == -1 && m->pre_blank == 0)
		s->penalty += START_OF_FILE_PENALTY;
	if (m->end_of_file)
		s->penalty += END_OF_FILE_PENALTY;
	post_blank = m->indent == -1 ? 1 + m->post_blank : 0;
	total_blank = m->pre_blank + post_blank;
	s->penalty += TOTAL_BLANK_WEIGHT * total_blank;
	s->penalty += POST_BLANK_WEIGHT * post_blank;
	indent = m->indent != -1 ? m->indent : m->post_indent;
	any_blanks = total_blank != 0;
	s->effective_indent += indent;
	if (indent == -1 || m->pre_indent == -1) {
		/* nothing more */
	} else if (indent > m->pre_indent) {
		s->penalty += any_blanks ? RELATIVE_INDENT_WITH_BLANK_PENALTY : RELATIVE_INDENT_PENALTY;
	} else if (indent == m->pre_indent) {
		/* nothing more */
	} else if (m->post_indent != -1 && m->post_indent > indent) {
		s->penalty += any_blanks ? RELATIVE_OUTDENT_WITH_BLANK_PENALTY : RELATIVE_OUTDENT_PENALTY;
	} else {
		s->penalty += any_blanks ? RELATIVE_DEDENT_WITH_BLANK_PENALTY : RELATIVE_DEDENT_PENALTY;
	}
}

static int score_cmp(const struct split_score *s1, const struct split_score *s2)
{
	int cmp_indents = (s1->effective_indent > s2->effective_indent) - (s1->effective_indent < s2->effective_indent);

	return INDENT_WEIGHT * cmp_indents + (s1->penalty - s2->penalty);
}

/* xdl_change_compact: x's groups moved as far as they can go, merged when they meet, then placed as git places them. */
static void change_compact(struct xfile *x, struct xfile *xo)
{
	struct xgroup g, go;
	long earliest_end, end_matching_other, groupsize;

	group_init(x, &g);
	group_init(xo, &go);
	for (;;) {
		if (g.end != g.start) {
			do {
				groupsize = g.end - g.start;
				end_matching_other = -1;
				while (!group_slide_up(x, &g))
					if (group_previous(xo, &go))
						die("diff: group sync broken sliding up");
				earliest_end = g.end;
				if (go.end > go.start)
					end_matching_other = g.end;
				for (;;) {
					if (group_slide_down(x, &g))
						break;
					if (group_next(xo, &go))
						die("diff: group sync broken sliding down");
					if (go.end > go.start)
						end_matching_other = g.end;
				}
			} while (groupsize != g.end - g.start);
			if (g.end == earliest_end) {
				/* it cannot move */
			} else if (end_matching_other != -1) {
				while (go.end == go.start) {
					if (group_slide_up(x, &g))
						die("diff: match disappeared");
					if (group_previous(xo, &go))
						die("diff: group sync broken sliding to match");
				}
			} else {
				long shift, best_shift = -1;
				struct split_score best_score = { 0, 0 };

				shift = earliest_end;
				if (g.end - groupsize - 1 > shift)
					shift = g.end - groupsize - 1;
				if (g.end - INDENT_HEURISTIC_MAX_SLIDING > shift)
					shift = g.end - INDENT_HEURISTIC_MAX_SLIDING;
				for (; shift <= g.end; shift++) {
					struct split_measurement m;
					struct split_score score = { 0, 0 };

					measure_split(x, shift, &m);
					score_add_split(&m, &score);
					measure_split(x, shift - groupsize, &m);
					score_add_split(&m, &score);
					if (best_shift == -1 || score_cmp(&score, &best_score) <= 0) {
						best_score = score;
						best_shift = shift;
					}
				}
				while (g.end > best_shift) {
					if (group_slide_up(x, &g))
						die("diff: best shift unreached");
					if (group_previous(xo, &go))
						die("diff: group sync broken sliding to blank line");
				}
			}
		}
		if (group_next(x, &g))
			break;
		if (group_next(xo, &go))
			die("diff: group sync broken moving to next group");
	}
	if (!group_next(xo, &go))
		die("diff: group sync broken at end of file");
}

static void xfile_init(struct xfile *x, const struct line *rec, const long *cls, long nrec)
{
	x->rec = rec;
	x->cls = cls;
	x->nrec = nrec;
	x->rchg = calloc((size_t)nrec + 2, 1);
	if (!x->rchg)
		die("out of memory");
	x->rchg++;
	x->rindex = xmalloc((size_t)(nrec + 1) * sizeof *x->rindex);
	x->ha = xmalloc((size_t)(nrec + 1) * sizeof *x->ha);
	x->nreff = 0;
}

static void xfile_free(struct xfile *x)
{
	free(x->rchg - 1);
	free(x->rindex);
	free(x->ha);
}

struct change {
	long i1, n1, i2, n2;
};

static size_t diff_lines(const struct line *a, long na, const struct line *b, long nb, struct change **out)
{
	long *ia = xmalloc((size_t)(na + 1) * sizeof *ia), *ib = xmalloc((size_t)(nb + 1) * sizeof *ib);
	long ncls = number_lines(a, (size_t)na, b, (size_t)nb, ia, ib), *c1, *c2, i, j, lim, ndiags, *kv;
	struct xfile x1, x2;
	struct xenv xenv;
	struct change *v = NULL;
	size_t n = 0;

	c1 = calloc((size_t)ncls + 1, sizeof *c1);
	c2 = calloc((size_t)ncls + 1, sizeof *c2);
	if (!c1 || !c2)
		die("out of memory");
	for (i = 0; i < na; i++)
		c1[ia[i]]++;
	for (i = 0; i < nb; i++)
		c2[ib[i]]++;
	xfile_init(&x1, a, ia, na);
	xfile_init(&x2, b, ib, nb);
	/* xdl_trim_ends */
	lim = na < nb ? na : nb;
	for (i = 0; i < lim && ia[i] == ib[i]; i++)
		;
	x1.dstart = x2.dstart = i;
	for (lim -= i, i = 0; i < lim && ia[na - 1 - i] == ib[nb - 1 - i]; i++)
		;
	x1.dend = na - i - 1;
	x2.dend = nb - i - 1;
	cleanup(&x1, c2);
	cleanup(&x2, c1);
	ndiags = x1.nreff + x2.nreff + 3;
	kv = xmalloc((size_t)(2 * ndiags + 2) * sizeof *kv);
	xenv.mxcost = bogosqrt(ndiags);
	if (xenv.mxcost < X_MAX_COST_MIN)
		xenv.mxcost = X_MAX_COST_MIN;
	xenv.snake_cnt = X_SNAKE_CNT;
	xenv.heur_min = X_HEUR_MIN_COST;
	xrecs_cmp(&x1, 0, x1.nreff, &x2, 0, x2.nreff, kv + x2.nreff + 1, kv + ndiags + x2.nreff + 1, 0, &xenv);
	change_compact(&x1, &x2);
	change_compact(&x2, &x1);
	for (i = j = 0; i < na || j < nb;) {
		struct change c;

		if (i < na && j < nb && !x1.rchg[i] && !x2.rchg[j]) {
			i++, j++;
			continue;
		}
		c.i1 = i;
		c.i2 = j;
		while (i < na && x1.rchg[i])
			i++;
		while (j < nb && x2.rchg[j])
			j++;
		c.n1 = i - c.i1;
		c.n2 = j - c.i2;
		if (!c.n1 && !c.n2)
			die("diff: inconsistent edit script");
		v = xrealloc(v, (n + 1) * sizeof *v);
		v[n++] = c;
	}
	xfile_free(&x1);
	xfile_free(&x2);
	free(kv);
	free(c1);
	free(c2);
	free(ia);
	free(ib);
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

/* Set while diffing two revisions: the "b" side is then read from the object store, not the working tree. */
static int b_from_objects;

static void load_worktree(const struct file *f, buf *out)
{
	char *full;

	if (b_from_objects) {
		load_head(f, out);
		return;
	}
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
	static const unsigned char zero[20];
	const char *path = a ? a->path : b->path;
	buf la = { 0 }, lb = { 0 }, old = { 0 }, new = { 0 };
	char ha[41], hb[41];

	/* git abbreviates both ids as rev-parse --short does: 7 digits or more as the repository grows */
	id_to_hex(a ? a->id : zero, ha);
	id_to_hex(b ? b->id : zero, hb);
	ha[abbrev_len(a ? a->id : zero, -1)] = 0;
	hb[abbrev_len(b ? b->id : zero, -1)] = 0;
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
		buf_printf(out, "index %s..%s", ha, hb);
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

/* The changes between a and b, sorted lists of entries, as a patch, the names or a diffstat; lead goes first when there are any. */
static size_t show_changes(const struct files *a, const struct files *b, int mode, const char *lead)
{
	struct pair *ch;
	struct stat_row *rows;
	size_t n = files_diff(a, b, &ch), i;
	buf out = { 0 };

	if (n)
		fputs(lead, stdout);
	rows = xmalloc((n ? n : 1) * sizeof *rows);
	for (i = 0; i < n; i++) {
		const struct file *x = ch[i].a, *y = ch[i].b;

		if (mode == 'n') {
			buf_quote(&out, "", x ? x->path : y->path);
			buf_addc(&out, '\n');
		} else if (mode == 's') {
			stat_row(&rows[i], x, y);
		} else if (x && y && (x->mode ^ y->mode) & 0170000) {
			/* git shows a file/symlink swap as a deletion followed by a creation */
			emit_patch(&out, x, NULL);
			emit_patch(&out, NULL, y);
		} else {
			emit_patch(&out, x, y);
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
	return n;
}

/* Two trees' files (both in the object store), as git diff <rev> <rev> shows them; mode 'n', 's' or 0 for a patch. */
size_t diff_files_show(const struct files *a, const struct files *b, int mode, const char *lead)
{
	size_t n;

	b_from_objects = 1;
	n = show_changes(a, b, mode, lead);
	b_from_objects = 0;
	return n;
}

/* A revision's tree (a commit's, a tag's target's, or a tree named directly), as git diff takes a tree-ish. */
static void rev_tree(const char *spec, unsigned char tree[20])
{
	unsigned char id[20];

	if (rev_parse(spec, id))
		die("bad revision '%s'", spec);
	for (;;) {
		buf b = { 0 };
		int type;

		object_read_or_die(id, &type, &b);
		if (type == OBJ_TREE) {
			memcpy(tree, id, 20);
		} else if (type == OBJ_COMMIT) {
			struct commit c;

			commit_parse(&b, &c);
			memcpy(tree, c.tree, 20);
			free(c.parent);
		} else if (type == OBJ_TAG && b.len >= 47 && !strncmp((char *)b.p, "object ", 7) && !hex_to_id((char *)b.p + 7, id)) {
			buf_free(&b);
			continue;
		} else {
			die("'%s' names a %s, not a commit or a tree", spec, type_name(type));
		}
		buf_free(&b);
		return;
	}
}

static const char diff_usage[] = "usage: mgit diff [--quiet | --name-only | --stat] [<rev> <rev>] [-- <path>...]";

/*
 * HEAD to the working tree, covering what status lists, or one revision to another, as a patch, the names, a diffstat,
 * or only the exit status (--quiet: 1 when anything differs, as git's --exit-code). No rename detection, as git's
 * --no-renames.
 */
int cmd_diff(int argc, char **argv)
{
	struct repo_state s;
	struct files w = { 0 }, hf = { 0 };
	struct pathspec ps = { NULL, 0 };
	const char *revs[2];
	size_t i;
	int mode = 0, nrevs = 0;

	for (i = 1; i < (size_t)argc; i++) {
		int m = !strcmp(argv[i], "--quiet") ? 'q' : !strcmp(argv[i], "--name-only") ? 'n' : !strcmp(argv[i], "--stat") ? 's' : 0;

		if (!strcmp(argv[i], "--"))
			break;
		if (!strcmp(argv[i], "--no-renames"))
			continue;	/* mgit never detects renames */
		if (argv[i][0] != '-') {
			if (nrevs == 2)
				die("%s", diff_usage);
			revs[nrevs++] = argv[i];
			continue;
		}
		if (!m || mode)
			die("%s", diff_usage);
		mode = m;
	}
	if (nrevs == 1)
		die("mgit diff takes two revisions, or none for HEAD to the working tree\n%s", diff_usage);
	repo_find();
	if (i + 1 < (size_t)argc)
		pathspec_parse(&ps, argc - (int)i - 1, argv + i + 1);
	if (nrevs == 2) {
		unsigned char ta[20], tb[20];
		struct files fa = { 0 }, fb = { 0 }, ka = { 0 }, kb = { 0 };
		struct pair *ch;
		size_t n;

		rev_tree(revs[0], ta);
		rev_tree(revs[1], tb);
		tree_flatten(ta, &fa);
		tree_flatten(tb, &fb);
		if (ps.n) {
			files_keep(&fa, &ps, 1, &ka);
			files_keep(&fb, &ps, 1, &kb);
		} else {
			ka = fa;
			kb = fb;
		}
		if (mode == 'q') {
			n = files_diff(&ka, &kb, &ch);
			free(ch);
			return n > 0;
		}
		diff_files_show(&ka, &kb, mode, "");
		return 0;
	}
	state_load(&s);
	if (ps.n)
		files_keep(&s.head_files, &ps, 1, &hf);
	else
		hf = s.head_files;
	worktree_scan(&s, 0, &ps, &w);
	if (mode == 'q') {
		struct pair *ch;
		size_t n = files_diff(&hf, &w, &ch);

		free(ch);
		return n > 0;
	}
	show_changes(&hf, &w, mode, "");
	return 0;
}
