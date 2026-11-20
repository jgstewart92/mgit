#define _DEFAULT_SOURCE
#include "mgit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void commit_parse(const buf *b, struct commit *c)
{
	const char *p = (const char *)b->p, *end = p + b->len;
	int have_tree = 0, line = 0, parents = 1;

	memset(c, 0, sizeof *c);
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t len;

		if (!nl)
			nl = end;
		len = (size_t)(nl - p);
		if (!len) {
			c->msg = nl + 1;
			c->msg_len = (size_t)(end - c->msg);
			break;
		}
		/* as git reads a commit: the tree on the first line only, the parents only on the lines right after it */
		if (!line++) {
			if (len != 45 || memcmp(p, "tree ", 5) || hex_to_id(p + 5, c->tree))
				break;
			have_tree = 1;
		} else if (parents && len == 47 && !memcmp(p, "parent ", 7)) {
			c->parent = xrealloc(c->parent, (size_t)(c->nparent + 1) * sizeof *c->parent);
			if (hex_to_id(p + 7, c->parent[c->nparent++]))
				die("corrupt commit: bad parent line");
		} else {
			parents = 0;
		}
		if (len > 7 && !memcmp(p, "author ", 7)) {
			c->author = p + 7;
			c->author_len = len - 7;
		} else if (len > 10 && !memcmp(p, "committer ", 10)) {
			c->committer = p + 10;
			c->committer_len = len - 10;
		}
		if (nl == end)
			break;
		p = nl + 1;
	}
	if (!have_tree || !c->author)
		die("corrupt commit: missing tree or author");
	if (!c->msg)
		c->msg = end;
}

void commit_tree_id(const unsigned char commit[20], unsigned char tree[20])
{
	struct commit c;
	buf b = { 0 };
	int type;

	object_read_or_die(commit, &type, &b);
	if (type != OBJ_COMMIT)
		die("expected a commit, found a %s", type_name(type));
	commit_parse(&b, &c);
	memcpy(tree, c.tree, 20);
	free(c.parent);
	buf_free(&b);
}

/* As git: a variable that is set wins even when empty, then <role>.<key>, user.<key>, and for the email $EMAIL. */
static char *ident_part(const char *env, const char *role, const char *key)
{
	const char *v = getenv(env);
	char *r;

	if (v)
		return xstrdup(v);
	r = config_get(role, key);
	if (!r)
		r = config_get("user", key);
	if (!r && !strcmp(key, "email") && (v = getenv("EMAIL")) && *v)
		r = xstrdup(v);
	return r;
}

/* git's crud (ident.c): what it trims from both ends of a name or an email. '.' is not crud. */
static int crud(unsigned char c)
{
	return c <= 32 || c == ',' || c == ':' || c == ';' || c == '<' || c == '>' || c == '"' || c == '\\' || c == '\'';
}

/* git's strbuf_addstr_without_crud: crud trimmed from both ends, '\n', '<' and '>' dropped from inside. */
static size_t add_without_crud(buf *out, const char *s)
{
	size_t n = strlen(s), i, before = out->len;

	while (n && crud((unsigned char)*s))
		s++, n--;
	while (n && crud((unsigned char)s[n - 1]))
		n--;
	for (i = 0; i < n; i++)
		if (s[i] != '\n' && s[i] != '<' && s[i] != '>')
			buf_addc(out, s[i]);
	return out->len - before;
}

/*
 * "<label> Name <email> <seconds> <+hhmm>\n" for role author or committer (label is the role, or "tagger" for the
 * committer of a tag), from GIT_<ROLE>_* variables or config, as git does; force, from --author, is "Name <email>"
 * and wins over both.
 */
void ident(buf *out, const char *label, const char *role, const char *force)
{
	int author = !strcmp(role, "author");
	const char *up = author ? "AUTHOR" : "COMMITTER";
	const char *date = getenv(author ? "GIT_AUTHOR_DATE" : "GIT_COMMITTER_DATE");
	char *name, *email;

	if (force) {
		/* git's split_ident_line: the name before '<' (trailing blanks dropped), the email up to the next '>' */
		const char *lt = strchr(force, '<'), *gt = lt ? strchr(lt + 1, '>') : NULL, *e = lt;

		if (!gt)
			die("--author takes 'Name <email>' (mgit does not look an author up by name, as git does without '>')");
		while (e > force && isspace((unsigned char)e[-1]))
			e--;
		name = xstrndup(force, (size_t)(e - force));
		email = xstrndup(lt + 1, (size_t)(gt - lt - 1));
	} else {
		name = ident_part(author ? "GIT_AUTHOR_NAME" : "GIT_COMMITTER_NAME", role, "name");
		email = ident_part(author ? "GIT_AUTHOR_EMAIL" : "GIT_COMMITTER_EMAIL", role, "email");
	}

	if (!name || !email)
		die("%s identity unknown: set user.name and user.email in the global .gitconfig, or GIT_%s_NAME and GIT_%s_EMAIL",
		    role, up, up);
	if (!*name)
		die("empty ident name (for <%s>) not allowed", email);
	/* git's fmt_ident: crud trimmed and delimiters dropped from both; a name of nothing else is refused */
	buf_printf(out, "%s ", label);
	if (!add_without_crud(out, name))
		die("name consists only of disallowed characters: %s", name);
	buf_adds(out, " <");
	add_without_crud(out, email);
	buf_adds(out, "> ");
	if (date && *date) {
		const char *p = date + (*date == '@');
		size_t nd = strspn(p, "0123456789");

		if (!nd || p[nd] != ' ' || (p[nd + 1] != '+' && p[nd + 1] != '-') ||
		    strspn(p + nd + 2, "0123456789") != 4 || p[nd + 6])
			die("GIT_%s_DATE must be '<unix-seconds> <+hhmm>'", up);
		buf_adds(out, p);
	} else {
		time_t now = time(NULL);
		struct tm tm;
		long off;

		localtime_r(&now, &tm);
		off = tm.tm_gmtoff;
		buf_printf(out, "%lld %c%02ld%02ld", (long long)now, off < 0 ? '-' : '+',
			   labs(off) / 3600, labs(off) % 3600 / 60);
	}
	buf_addc(out, '\n');
	free(name);
	free(email);
}

static int git_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/*
 * git's cleanup of a -m message: trailing whitespace stripped, leading and trailing blank lines dropped, runs of them
 * collapsed; comments also drops lines starting with '#', as git tag does.
 */
void cleanup_message(buf *out, const char *in, size_t n, int comments)
{
	const char *p = in, *end = in + n;
	int blank = 0;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t len;

		if (!nl)
			nl = end;
		len = (size_t)(nl - p);
		while (len && git_space(p[len - 1]))
			len--;
		if (comments && len && *p == '#') {
			/* dropped without a trace: it neither ends nor starts a run of blank lines */
		} else if (!len) {
			blank = out->len > 0;
		} else {
			if (blank)
				buf_addc(out, '\n');
			blank = 0;
			buf_add(out, p, len);
			buf_addc(out, '\n');
		}
		if (nl == end)
			break;
		p = nl + 1;
	}
}

/* Adds every entry of add to f, keeping f sorted. git add replaces a file standing where a new path needs a directory: so does this. */
static void files_merge(struct files *f, const struct files *add)
{
	size_t i, j;

	for (i = 0; i < add->n; i++) {
		struct file *e = files_add(f, add->v[i].path, add->v[i].mode, add->v[i].id);

		e->have_stat = add->v[i].have_stat;
		e->sd = add->v[i].sd;
	}
	files_sort(f);
	for (i = 0; i < add->n; i++) {
		char *p = xstrdup(add->v[i].path), *slash;
		struct file *e;

		while ((slash = strrchr(p, '/'))) {
			*slash = 0;
			if ((e = files_find(f, p)))
				e->mode = 0;
		}
		free(p);
	}
	for (i = j = 0; i < f->n; i++) {
		if (f->v[i].mode)
			f->v[j++] = f->v[i];
		else
			free(f->v[i].path);
	}
	f->n = j;
}

static int any_covered(const struct files *f, const struct pathspec *one)
{
	size_t i;

	for (i = 0; i < f->n; i++)
		if (pathspec_covers(one, f->v[i].path))
			return 1;
	return 0;
}

static const char commit_usage[] = "usage: mgit commit -m <message> [-m <message>...] [--author 'Name <email>'] [-q] [-- <path>...]";

int cmd_commit(int argc, char **argv)
{
	unsigned char tree[20], parent[20], id[20];
	buf raw = { 0 }, msg = { 0 }, who = { 0 }, body = { 0 };
	struct repo_state s;
	struct files snap = { 0 }, next = { 0 }, idx = { 0 }, *tree_files = &snap, *index_files = &snap;
	struct pathspec ps = { NULL, 0 };
	const char *branch, *author = NULL;
	char *head, hex[41], **paths = NULL;
	size_t sublen, k;
	int i, have_msg = 0, have_parent, quiet = 0, npaths = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--")) {
			paths = argv + i + 1;
			npaths = argc - i - 1;
			break;
		}
		if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
			quiet = 1;
			continue;
		}
		if (!strncmp(argv[i], "--author=", 9) || (!strcmp(argv[i], "--author") && i + 1 < argc)) {
			author = argv[i][8] ? argv[i] + 9 : argv[++i];
			continue;
		}
		if (strcmp(argv[i], "-m") || i + 1 == argc)
			die("%s", commit_usage);
		if (raw.len)
			buf_addc(&raw, '\n');
		buf_adds(&raw, argv[++i]);
		if (raw.len && raw.p[raw.len - 1] != '\n')
			buf_addc(&raw, '\n');
		have_msg = 1;
	}
	if (!have_msg)
		die("%s", commit_usage);
	cleanup_message(&msg, (char *)raw.p, raw.len, 0);
	if (!msg.len) {
		fputs("Aborting commit due to empty commit message.\n", stderr);
		return 1;
	}

	repo_find();
	for (k = 0; k < 5; k++) {
		static const char *const busy[] = { "MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply" };
		char *f = path_join(git_dir, busy[k]);

		if (!access(f, F_OK))
			die("%s exists: git is in the middle of a merge, cherry-pick, revert or rebase; finish it with git", f);
		free(f);
	}
	ident(&who, "author", "author", author);
	ident(&who, "committer", "committer", NULL);
	state_load(&s);
	if (npaths) {
		/* HEAD's tree with each named path as the working tree has it; the index the same over what it holds. */
		const struct files *tracked = s.have_index ? &s.index : &s.head_files;

		pathspec_parse(&ps, npaths, paths);
		worktree_scan(&s, 1, &ps, &snap);
		for (k = 0; k < ps.n; k++) {
			struct pathspec one = { &ps.v[k], 1 };

			if (!any_covered(&snap, &one) && !any_covered(&s.head_files, &one) && !any_covered(tracked, &one))
				die("pathspec '%s' did not match any file(s) known to git", paths[k]);
		}
		files_keep(&s.head_files, &ps, 0, &next);
		files_merge(&next, &snap);
		files_keep(tracked, &ps, 0, &idx);
		/* stat data the old index could not vouch for (racily clean) must not look valid under the new stamp */
		for (k = 0; k < idx.n; k++) {
			struct stat_data *sd = &idx.v[k].sd;

			if (s.stamp[0] < sd->mtime_s || (s.stamp[0] == sd->mtime_s && s.stamp[1] <= sd->mtime_ns))
				idx.v[k].have_stat = 0;
		}
		files_merge(&idx, &snap);
		tree_files = &next;
		index_files = &idx;
	} else {
		worktree_scan(&s, 1, NULL, &snap);
	}
	tree_build(tree_files, tree);
	head = s.head_ref;
	have_parent = s.have_head;
	memcpy(parent, s.head, 20);
	if (have_parent) {
		unsigned char ptree[20];

		commit_tree_id(parent, ptree);
		if (!memcmp(ptree, tree, 20)) {
			puts(npaths ? "nothing to commit in the named paths" : "nothing to commit, working tree clean");
			return 1;
		}
	} else if (!tree_files->n) {
		puts("nothing to commit");
		return 1;
	}

	id_to_hex(tree, hex);
	buf_printf(&body, "tree %s\n", hex);
	if (have_parent) {
		id_to_hex(parent, hex);
		buf_printf(&body, "parent %s\n", hex);
	}
	buf_add(&body, who.p, who.len);
	buf_addc(&body, '\n');
	buf_add(&body, msg.p, msg.len);
	object_write(OBJ_COMMIT, body.p, body.len, id);
	ref_update(head ? head : "HEAD", id, have_parent ? parent : NULL);
	sublen = strcspn((char *)msg.p, "\n");
	{
		/* git's reflog lines: the committer, then "commit: <subject>" */
		const char *ci = strstr((char *)who.p, "\ncommitter ") + 11;
		buf rmsg = { 0 };

		buf_printf(&rmsg, "commit%s: %.*s", have_parent ? "" : " (initial)", (int)sublen, (char *)msg.p);
		if (head)
			reflog_append(head, have_parent ? parent : NULL, id, ci, strcspn(ci, "\n"), (char *)rmsg.p);
		reflog_append("HEAD", have_parent ? parent : NULL, id, ci, strcspn(ci, "\n"), (char *)rmsg.p);
		buf_free(&rmsg);
	}
	index_write(index_files);
	if (quiet)
		return 0;

	id_to_hex(id, hex);
	branch = !head ? "detached HEAD" : strncmp(head, "refs/heads/", 11) ? head : head + 11;
	printf("[%s%s %.7s] %.*s\n", branch, have_parent ? "" : " (root-commit)", hex, (int)sublen, (char *)msg.p);
	return 0;
}

