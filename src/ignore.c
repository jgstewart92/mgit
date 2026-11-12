#define _DEFAULT_SOURCE
#include "mgit.h"

#include <stdlib.h>
#include <string.h>

struct pattern {
	char *pat, *base;
	int negate, dironly, pathname;
};

/*
 * Git's wildmatch for the forms .gitignore uses: '*', '?', '[...]' (with '!' or '^', ranges and '\'
 * escapes; no [:class:] names) and, when pathname is set, "**" as a whole path component.
 */
static int wm(const char *pat, const char *p, const char *t, int pathname)
{
	for (; *p; p++, t++) {
		const char *q;

		switch (*p) {
		case '\\':
			if (!*++p || *t != *p)
				return 0;
			break;
		case '?':
			if (!*t || (pathname && *t == '/'))
				return 0;
			break;
		case '*':
			if (p[1] == '*') {
				q = p;
				while (p[1] == '*')
					p++;
				if (pathname && (q == pat || q[-1] == '/') && (!p[1] || p[1] == '/')) {
					if (!p[1])
						return 1;
					if (wm(pat, p + 2, t, pathname))
						return 1;
					for (; *t; t++)
						if (*t == '/' && wm(pat, p + 2, t + 1, pathname))
							return 1;
					return 0;
				}
			}
			if (!p[1])
				return !pathname || !strchr(t, '/');
			for (;; t++) {
				if (wm(pat, p + 1, t, pathname))
					return 1;
				if (!*t || (pathname && *t == '/'))
					return 0;
			}
		case '[': {
			unsigned char c = (unsigned char)*t;
			int neg = 0, hit = 0;

			if (!c || (pathname && c == '/'))
				return 0;
			q = p + 1;
			if (*q == '!' || *q == '^') {
				neg = 1;
				q++;
			}
			do {
				unsigned char lo, hi;

				if (*q == '\\')
					q++;
				if (!*q)
					return 0;
				lo = hi = (unsigned char)*q++;
				if (*q == '-' && q[1] && q[1] != ']') {
					if (*++q == '\\')
						q++;
					if (!*q)
						return 0;
					hi = (unsigned char)*q++;
				}
				if (lo <= c && c <= hi)
					hit = 1;
			} while (*q != ']');
			if (hit == neg)
				return 0;
			p = q;
			break;
		}
		default:
			if (*t != *p)
				return 0;
		}
	}
	return !*t;
}

int wildmatch(const char *pat, const char *text, int pathname)
{
	return wm(pat, pat, text, pathname);
}

static void add_patterns(struct ignore *ig, const char *text, const char *base)
{
	const char *line, *next;

	for (line = text; line && *line; line = next) {
		const char *s = line;
		size_t len;
		struct pattern *p;
		int negate = 0, dironly = 0;

		next = strchr(line, '\n');
		len = next ? (size_t)(next - line) : strlen(line);
		if (next)
			next++;
		if (len && s[len - 1] == '\r')
			len--;
		while (len && s[len - 1] == ' ' && !(len >= 2 && s[len - 2] == '\\'))
			len--;
		if (!len || *s == '#')
			continue;
		if (*s == '!') {
			negate = 1;
			s++;
			len--;
		}
		if (len && s[len - 1] == '/') {
			dironly = 1;
			len--;
		}
		if (!len)
			continue;
		if (ig->n == ig->cap) {
			ig->cap = ig->cap ? ig->cap * 2 : 32;
			ig->v = xrealloc(ig->v, ig->cap * sizeof *ig->v);
		}
		p = &ig->v[ig->n++];
		p->negate = negate;
		p->dironly = dironly;
		p->pathname = memchr(s, '/', len) != NULL;
		if (*s == '/') {
			s++;
			len--;
		}
		p->pat = xstrndup(s, len);
		p->base = xstrdup(base);
	}
}

static void add_file(struct ignore *ig, const char *path, const char *base)
{
	buf b = { 0 };

	if (!read_file(path, &b))
		add_patterns(ig, (char *)b.p, base);
	buf_free(&b);
}

/* Global excludes, then .git/info/exclude; per-directory .gitignore files are pushed during the walk. */
void ignore_init(struct ignore *ig)
{
	char *file = config_get("core", "excludesfile"), *path;
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");

	memset(ig, 0, sizeof *ig);
	if (file && file[0] == '~' && file[1] == '/' && home) {
		path = path_join(home, file + 2);
		free(file);
		file = path;
	}
	if (!file && xdg && *xdg)
		file = path_join(xdg, "git/ignore");
	else if (!file && home)
		file = path_join(home, ".config/git/ignore");
	if (file)
		add_file(ig, file, "");
	free(file);
	path = path_join(git_dir, "info/exclude");
	add_file(ig, path, "");
	free(path);
}

/* Loads dir/.gitignore for paths under rel ("" or "a/b/"); returns the mark to pop back to. */
size_t ignore_push(struct ignore *ig, const char *dir, const char *rel)
{
	char *path = path_join(dir, ".gitignore");
	size_t mark = ig->n;
	struct stat st;

	if (!lstat(path, &st) && S_ISREG(st.st_mode))
		add_file(ig, path, rel);
	free(path);
	return mark;
}

void ignore_pop(struct ignore *ig, size_t mark)
{
	while (ig->n > mark) {
		ig->n--;
		free(ig->v[ig->n].pat);
		free(ig->v[ig->n].base);
	}
}

/* The last matching pattern decides; deeper .gitignore files come later, so they win. */
int ignore_match(const struct ignore *ig, const char *path, int isdir)
{
	const char *slash = strrchr(path, '/'), *name = slash ? slash + 1 : path;
	size_t i = ig->n;

	while (i--) {
		const struct pattern *p = &ig->v[i];
		size_t bl = strlen(p->base);

		if (p->dironly && !isdir)
			continue;
		if (p->pathname ? !strncmp(path, p->base, bl) && wildmatch(p->pat, path + bl, 1) : wildmatch(p->pat, name, 0))
			return !p->negate;
	}
	return 0;
}

/* The same for a .gitignore held in text (a committed one, read from its tree). */
size_t ignore_push_text(struct ignore *ig, const char *text, const char *rel)
{
	size_t mark = ig->n;

	add_patterns(ig, text, rel);
	return mark;
}
