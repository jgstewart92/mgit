#define _DEFAULT_SOURCE
#include "mgit.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

char *git_dir, *work_tree;

/* Finds the repository holding the current directory: 0, or -1 when there is none up to /. */
int repo_try_find(void)
{
	char cwd[PATH_MAX], *slash;
	struct stat st;

	if (!getcwd(cwd, sizeof cwd))
		die("cannot get current directory: %s", strerror(errno));
	for (;;) {
		char *g = path_join(strcmp(cwd, "/") ? cwd : "", ".git");
		char *head = path_join(g, "HEAD");
		int found = stat(head, &st) == 0;

		free(head);
		if (found) {
			git_dir = g;
			work_tree = xstrdup(cwd);
			return 0;
		}
		if (lstat(g, &st) == 0 && !S_ISDIR(st.st_mode))
			die("%s is a file; linked worktrees and submodules are not supported", g);
		free(g);
		if (!strcmp(cwd, "/"))
			return -1;
		slash = strrchr(cwd, '/');
		slash[slash == cwd] = 0;
	}
}

void repo_find(void)
{
	if (repo_try_find())
		die("not a git repository (or any parent up to /)");
}

/*
 * A config file as git writes it: [section] and [section "subsection"] headers (the section's case ignored, the
 * subsection's kept), key = value with values in any mix of plain and quoted parts, the escapes \" \\ \n \t \b,
 * comments after # or ; outside quotes. Calls fn for each value. No includes, and a key with no '=' is skipped.
 */
typedef void config_fn(const char *section, const char *sub, const char *key, const char *value, void *ctx);

static void config_parse(const char *path, config_fn *fn, void *ctx)
{
	buf b = { 0 }, sec = { 0 }, sub = { 0 }, val = { 0 };
	char *line, *next;
	int have_sub = 0;

	if (read_file(path, &b)) {
		buf_free(&b);
		return;
	}
	buf_adds(&sec, "");
	for (line = (char *)b.p; line; line = next) {
		char *e, *v, key[128];
		int quoted = 0;
		size_t trim;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		while (isspace((unsigned char)*line))
			line++;
		if (*line == '[') {
			sec.len = sub.len = 0;
			have_sub = 0;
			for (e = line + 1; isalnum((unsigned char)*e) || *e == '-' || *e == '.'; e++)
				buf_addc(&sec, tolower((unsigned char)*e));
			buf_adds(&sec, "");
			if (*e == ' ') {
				while (*e == ' ')
					e++;
				if (*e++ != '"')
					goto bad;
				for (; *e && *e != '"'; e++) {
					if (*e == '\\' && e[1])
						e++;
					buf_addc(&sub, *e);
				}
				if (*e++ != '"')
					goto bad;
				buf_adds(&sub, "");
				have_sub = 1;
			}
			if (*e != ']')
				goto bad;
			continue;
		bad:
			sec.len = 0;
			buf_adds(&sec, "");
			continue;
		}
		if (!sec.len)
			continue;
		for (e = line; (isalnum((unsigned char)*e) || *e == '-') && e - line < 127; e++)
			key[e - line] = (char)tolower((unsigned char)*e);
		key[e - line] = 0;
		if (e == line)
			continue;
		while (*e == ' ' || *e == '\t')
			e++;
		if (*e != '=')
			continue;
		val.len = 0;
		buf_adds(&val, "");
		trim = 0;
		for (v = e + 1; *v == ' ' || *v == '\t'; v++)
			;
		for (; *v; v++) {
			if (!quoted && (*v == '#' || *v == ';'))
				break;
			if (*v == '"') {
				quoted = !quoted;
				trim = val.len;
				continue;
			}
			if (*v == '\\' && v[1]) {
				v++;
				buf_addc(&val, *v == 'n' ? '\n' : *v == 't' ? '\t' : *v == 'b' ? '\b' : *v);
				trim = val.len;
				continue;
			}
			buf_addc(&val, *v);
			if (quoted || !isspace((unsigned char)*v))
				trim = val.len;
		}
		val.len = trim;
		val.p[val.len] = 0;
		fn((char *)sec.p, have_sub ? (char *)sub.p : NULL, key, (char *)val.p, ctx);
	}
	buf_free(&b);
	buf_free(&sec);
	buf_free(&sub);
	buf_free(&val);
}

struct config_query {
	const char *section, *sub, *key;
	char *res;
};

static void config_match(const char *section, const char *sub, const char *key, const char *value, void *ctx)
{
	struct config_query *q = ctx;

	if (strcasecmp(section, q->section) || strcasecmp(key, q->key) || (q->sub ? !sub || strcmp(sub, q->sub) : !!sub))
		return;
	free(q->res);
	q->res = xstrdup(value);
}

/* The config files in git's order of precedence: the repository's, then .gitconfig in the home directory, then $XDG_CONFIG_HOME/git/config. */
static void config_paths(char *paths[3])
{
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");

	paths[0] = git_dir ? path_join(git_dir, "config") : NULL;
	paths[1] = home ? path_join(home, ".gitconfig") : NULL;
	paths[2] = xdg && *xdg ? path_join(xdg, "git/config") : home ? path_join(home, ".config/git/config") : NULL;
}

/*
 * section.sub.key (sub NULL for none) from the first file that sets it, the last value there winning, over the files
 * the mask names: 1 the repository's, 2 the home directory's .gitconfig, 4 $XDG_CONFIG_HOME/git/config.
 */
char *config_get_in(const char *section, const char *sub, const char *key, int files)
{
	struct config_query q = { section, sub, key, NULL };
	char *paths[3];
	int i;

	config_paths(paths);
	for (i = 0; i < 3; i++) {
		if (!q.res && paths[i] && files & (1 << i))
			config_parse(paths[i], config_match, &q);
		free(paths[i]);
	}
	return q.res;
}

char *config_get_sub(const char *section, const char *sub, const char *key)
{
	return config_get_in(section, sub, key, 7);
}

char *config_get(const char *section, const char *key)
{
	return config_get_sub(section, NULL, key);
}

struct config_all {
	const char *section, *sub, *key;
	char **v;
	size_t n;
};

static void config_collect(const char *section, const char *sub, const char *key, const char *value, void *ctx)
{
	struct config_all *q = ctx;

	if (strcasecmp(section, q->section) || strcasecmp(key, q->key) || (q->sub ? !sub || strcmp(sub, q->sub) : !!sub))
		return;
	q->v = xrealloc(q->v, (q->n + 1) * sizeof *q->v);
	q->v[q->n++] = xstrdup(value);
}

/* Every value of a multi-valued key (remote.NAME.fetch), in the order git reads them: the global files, then the repository's. */
char **config_get_all(const char *section, const char *sub, const char *key, size_t *count)
{
	struct config_all q = { section, sub, key, NULL, 0 };
	char *paths[3];
	int i;

	config_paths(paths);
	for (i = 2; i >= 0; i--) {
		if (paths[i])
			config_parse(paths[i], config_collect, &q);
		free(paths[i]);
	}
	*count = q.n;
	return q.v;
}

struct config_subs {
	const char *section;
	char **v;
	size_t n;
};

static void config_sub(const char *section, const char *sub, const char *key, const char *value, void *ctx)
{
	struct config_subs *s = ctx;
	size_t i;

	(void)key;
	(void)value;
	if (!sub || strcasecmp(section, s->section))
		return;
	for (i = 0; i < s->n; i++)
		if (!strcmp(s->v[i], sub))
			return;
	s->v = xrealloc(s->v, (s->n + 1) * sizeof *s->v);
	s->v[s->n++] = xstrdup(sub);
}

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The subsections of section that hold a value in any config file, sorted (git remote lists remotes this way). */
char **config_subsections(const char *section, size_t *count)
{
	struct config_subs s = { section, NULL, 0 };
	char *paths[3];
	int i;

	config_paths(paths);
	for (i = 0; i < 3; i++) {
		if (paths[i])
			config_parse(paths[i], config_sub, &s);
		free(paths[i]);
	}
	if (s.n)
		qsort(s.v, s.n, sizeof *s.v, cmp_name);
	*count = s.n;
	return s.v;
}

/* What SIGINT, SIGTERM or SIGHUP removes before the process ends (DOOM_LOCK, DOOM_PACK, DOOM_IDX in mgit.h). */
static char doomed[3][PATH_MAX];

static void doomed_signal(int sig)
{
	size_t i;

	for (i = 0; i < 3; i++)
		if (doomed[i][0])
			unlink(doomed[i]);
	signal(sig, SIG_DFL);
	raise(sig);
}

/* Sets what slot names for removal on a signal (path NULL clears it). */
void remove_if_killed(int slot, const char *path)
{
	static const int sigs[] = { SIGINT, SIGTERM, SIGHUP };
	static int armed;
	size_t i;

	if (!armed) {
		for (i = 0; i < sizeof sigs / sizeof *sigs; i++) {
			struct sigaction old;

			if (!sigaction(sigs[i], NULL, &old) && old.sa_handler != SIG_IGN)
				signal(sigs[i], doomed_signal);
		}
		armed = 1;
	}
	if (path && strlen(path) < sizeof doomed[slot])
		strcpy(doomed[slot], path);
	else
		doomed[slot][0] = 0;
}

/* Removes the lock this process holds, if any. */
void lock_drop(void)
{
	if (doomed[DOOM_LOCK][0])
		unlink(doomed[DOOM_LOCK]);
	doomed[DOOM_LOCK][0] = 0;
}

/* Creates path as a lock file (O_EXCL), held until lock_done: an exit, a die, or SIGINT, SIGTERM or SIGHUP before then removes it, as git does its lock files. */
int lock_take(const char *path)
{
	static int armed;
	int fd;

	if (!armed) {
		atexit(lock_drop);
		armed = 1;
	}
	remove_if_killed(DOOM_LOCK, NULL);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (fd >= 0)
		remove_if_killed(DOOM_LOCK, path);
	return fd;
}

/* The lock was renamed into place: no longer this process's to remove. */
void lock_done(void)
{
	doomed[DOOM_LOCK][0] = 0;
}

/* Appends text to the repository's config, through config.lock as git does. */
void config_append(const char *text)
{
	char *path = path_join(git_dir, "config"), *lock = path_join(git_dir, "config.lock");
	buf b = { 0 };
	int fd = lock_take(lock);

	if (fd < 0)
		die("cannot lock %s: %s", path, strerror(errno));
	if (read_file(path, &b) && errno != ENOENT) {
		lock_drop();
		die("cannot read %s: %s", path, strerror(errno));
	}
	if (b.len && b.p[b.len - 1] != '\n')
		buf_addc(&b, '\n');
	buf_adds(&b, text);
	write_all(fd, b.p, b.len, lock);
	if (fsync(fd) || close(fd) || rename(lock, path))
		die("cannot write %s: %s", path, strerror(errno));
	lock_done();
	buf_free(&b);
	free(lock);
	free(path);
}

/* The rules of git check-ref-format, which also keep ref lookups inside the git directory. */
int refname_ok(const char *name)
{
	const char *p, *comp = name;
	size_t n = strlen(name);

	if (!n || name[0] == '/' || name[n - 1] == '/' || name[n - 1] == '.' || !strcmp(name, "@") ||
	    strstr(name, "..") || strstr(name, "@{") || strstr(name, "//"))
		return 0;
	for (p = name;; p++) {
		if (*p == '/' || !*p) {
			if (*comp == '.' || (p - comp >= 5 && !memcmp(p - 5, ".lock", 5)))
				return 0;
			if (!*p)
				return 1;
			comp = p + 1;
		} else if ((unsigned char)*p < 0x20 || *p == 0x7f || strchr(" ~^:?*[\\", *p)) {
			return 0;
		}
	}
}

static int read_loose(const char *name, buf *b)
{
	char *path = path_join(git_dir, name);
	int r = read_file(path, b);

	if (r && errno != ENOENT && errno != EISDIR && errno != ENOTDIR)
		die("cannot read %s: %s", path, strerror(errno));
	free(path);
	if (!r)
		while (b->len && isspace(b->p[b->len - 1]))
			b->p[--b->len] = 0;
	return r;
}

static int packed_lookup(const char *name, unsigned char id[20])
{
	char *path = path_join(git_dir, "packed-refs"), *line, *next;
	size_t n = strlen(name);
	buf b = { 0 };
	int r = -1;

	if (read_file(path, &b)) {
		if (errno != ENOENT)
			die("cannot read %s: %s", path, strerror(errno));
	} else {
		for (line = (char *)b.p; line && r; line = next) {
			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			if (strlen(line) == 41 + n && line[40] == ' ' && !strcmp(line + 41, name) && !hex_to_id(line, id))
				r = 0;
		}
	}
	buf_free(&b);
	free(path);
	return r;
}

/* Resolves a ref, following symbolic refs, loose file first and then packed-refs. -1 if it does not exist. */
int ref_read(const char *name, unsigned char id[20])
{
	char *cur = xstrdup(name);
	int depth, r;

	for (depth = 0; depth < 5; depth++) {
		buf b = { 0 };

		if (read_loose(cur, &b)) {
			buf_free(&b);
			r = packed_lookup(cur, id);
			free(cur);
			return r;
		}
		if (!strncmp((char *)b.p, "ref: ", 5)) {
			free(cur);
			cur = xstrdup((char *)b.p + 5);
			buf_free(&b);
			if (!refname_ok(cur))
				die("bad symbolic ref in %s", name);
			continue;
		}
		if (b.len != 40 || hex_to_id((char *)b.p, id))
			die("bad ref %s", cur);
		buf_free(&b);
		free(cur);
		return 0;
	}
	die("symbolic ref loop at %s", name);
}

/* The ref HEAD points at, or NULL when HEAD is detached. */
char *head_symref(void)
{
	buf b = { 0 };
	char *r = NULL;

	if (read_loose("HEAD", &b))
		die("cannot read HEAD");
	if (!strncmp((char *)b.p, "ref: ", 5))
		r = xstrdup((char *)b.p + 5);
	buf_free(&b);
	if (r && !refname_ok(r))
		die("bad symbolic ref in HEAD");
	return r;
}

/* name (HEAD, or a ref such as refs/remotes/origin/HEAD) as a symbolic ref to target, through name.lock. */
void symref_set(const char *name, const char *target)
{
	char *path = path_join(git_dir, name), *lock = xmalloc(strlen(path) + 6), *p;
	int fd;

	sprintf(lock, "%s.lock", path);
	for (p = path + strlen(git_dir) + 1; (p = strchr(p, '/')); p++) {
		*p = 0;
		if (mkdir(path, 0777) && errno != EEXIST)
			die("cannot create %s: %s", path, strerror(errno));
		*p = '/';
	}
	fd = lock_take(lock);
	if (fd < 0)
		die("cannot create %s: %s", lock, strerror(errno));
	write_all(fd, "ref: ", 5, lock);
	write_all(fd, target, strlen(target), lock);
	write_all(fd, "\n", 1, lock);
	if (fsync(fd) || close(fd) || rename(lock, path))
		die("cannot update %s: %s", name, strerror(errno));
	lock_done();
	free(lock);
	free(path);
}

void head_set_symref(const char *ref)
{
	symref_set("HEAD", ref);
}

/* Compare-and-swap: old_id NULL means the ref must not exist yet. */
void ref_update(const char *name, const unsigned char new_id[20], const unsigned char *old_id)
{
	char *path = path_join(git_dir, name), *lock, *p, hex[41];
	unsigned char cur[20];
	int fd, have;

	lock = xmalloc(strlen(path) + 6);
	sprintf(lock, "%s.lock", path);
	for (p = path + strlen(git_dir) + 1; (p = strchr(p, '/')); p++) {
		*p = 0;
		if (mkdir(path, 0777) && errno != EEXIST)
			die("cannot create %s: %s", path, strerror(errno));
		*p = '/';
	}
	fd = lock_take(lock);
	if (fd < 0 && errno == EEXIST)
		die("%s exists: another process is updating %s, or the lock is stale", lock, name);
	if (fd < 0)
		die("cannot create %s: %s", lock, strerror(errno));
	have = ref_read(name, cur) == 0;
	if (old_id ? !have || memcmp(cur, old_id, 20) : have) {
		lock_drop();
		if (old_id)
			die("%s moved while this command ran; nothing updated", name);
		die("%s already exists", name);
	}
	id_to_hex(new_id, hex);
	write_all(fd, hex, 40, lock);
	write_all(fd, "\n", 1, lock);
	if (fsync(fd) || close(fd))
		die("cannot write %s: %s", lock, strerror(errno));
	if (rename(lock, path))
		die("cannot rename %s: %s", lock, strerror(errno));
	lock_done();
	free(lock);
	free(path);
}

/*
 * Appends git's reflog line "<old> <new> <ident>\t<msg>\n" to logs/NAME, so HEAD@{n} and HEAD@{<time>} keep working.
 * As core.logAllRefUpdates says: unset or true creates the log (a repository with a working tree), false only appends
 * to a log that exists. A failed write is reported and does not undo the update.
 */
void reflog_append(const char *name, const unsigned char *old_id, const unsigned char new_id[20], const char *ident,
		   size_t ident_len, const char *msg)
{
	static const unsigned char zero[20];
	char *v = config_get("core", "logallrefupdates"), oh[41], nh[41], *p;
	int create = !v || !strcasecmp(v, "true") || !strcasecmp(v, "always") || !strcasecmp(v, "yes") ||
		     !strcasecmp(v, "on") || !strcmp(v, "1");
	buf path = { 0 }, line = { 0 };
	int fd;

	free(v);
	buf_printf(&path, "%s/logs/%s", git_dir, name);
	if (!create && access((char *)path.p, F_OK))
		goto out;
	for (p = (char *)path.p + strlen(git_dir) + 1; (p = strchr(p, '/')); p++) {
		int r;

		*p = 0;
		r = mkdir((char *)path.p, 0777);
		*p = '/';
		if (r && errno != EEXIST)
			break;
	}
	id_to_hex(old_id ? old_id : zero, oh);
	id_to_hex(new_id, nh);
	buf_printf(&line, "%s %s %.*s\t%s\n", oh, nh, (int)ident_len, ident, msg);
	fd = open((char *)path.p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
	if (fd < 0) {
		warning("cannot append to %s: %s", (char *)path.p, strerror(errno));
		goto out;
	}
	if (write(fd, line.p, line.len) != (ssize_t)line.len)
		warning("cannot append to %s: %s", (char *)path.p, strerror(errno));
	if (close(fd))
		warning("cannot write %s: %s", (char *)path.p, strerror(errno));
out:
	buf_free(&path);
	buf_free(&line);
}

void peel_to_commit(unsigned char id[20])
{
	for (;;) {
		buf b = { 0 };
		char hex[41];
		int type;

		object_read_or_die(id, &type, &b);
		id_to_hex(id, hex);
		if (type == OBJ_COMMIT) {
			buf_free(&b);
			return;
		}
		if (type != OBJ_TAG || b.len < 47 || strncmp((char *)b.p, "object ", 7) || hex_to_id((char *)b.p + 7, id))
			die("%s is a %s, not a commit", hex, type_name(type));
		buf_free(&b);
	}
}

static int nth_parent(unsigned char id[20], unsigned long k)
{
	struct commit c;
	buf b = { 0 };
	int type, r = -1;

	peel_to_commit(id);
	object_read_or_die(id, &type, &b);
	commit_parse(&b, &c);
	if (k >= 1 && k <= (unsigned long)c.nparent) {
		memcpy(id, c.parent[k - 1], 20);
		r = 0;
	}
	free(c.parent);
	buf_free(&b);
	return r;
}

/* Full hex, then git's ref search order, then an abbreviated hex id. */
static int resolve_base(const char *s, unsigned char id[20])
{
	static const char *const rules[] = { "%s", "refs/%s", "refs/tags/%s", "refs/heads/%s",
		"refs/remotes/%s", "refs/remotes/%s/HEAD" };
	size_t n = strlen(s), i;
	char lower[41];
	int r;

	if (n == 40 && is_hex(s, 40))
		return hex_to_id(s, id);
	for (i = 0; i < sizeof rules / sizeof *rules; i++) {
		buf name = { 0 };

		if (i == 0 && strncmp(s, "refs/", 5) && strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_") != n)
			continue;
		buf_printf(&name, rules[i], s);
		r = refname_ok((char *)name.p) ? ref_read((char *)name.p, id) : -1;
		buf_free(&name);
		if (!r)
			return 0;
	}
	if (n >= 4 && n < 40 && is_hex(s, n)) {
		for (i = 0; i < n; i++)
			lower[i] = (char)tolower((unsigned char)s[i]);
		lower[n] = 0;
		r = object_by_prefix(lower, n, id);
		if (r > 1)
			die("short object id %s is ambiguous", s);
		return r == 1 ? 0 : -1;
	}
	return -1;
}

/* <base> followed by any of ~N and ^N (N defaults to 1; ^0 peels to the commit). */
int rev_parse(const char *spec, unsigned char id[20])
{
	size_t n = strcspn(spec, "~^");
	char *base = xstrndup(spec, n);
	const char *p = spec + n;
	int r = resolve_base(base, id);

	free(base);
	while (!r && *p) {
		char op = *p++;
		unsigned long k = 1;

		if (op != '~' && op != '^')
			return -1;
		if (isdigit((unsigned char)*p)) {
			char *e;

			k = strtoul(p, &e, 10);
			p = e;
		}
		if (op == '^' && k == 0)
			peel_to_commit(id);
		else if (op == '^')
			r = nth_parent(id, k);
		else
			while (!r && k--)
				r = nth_parent(id, 1);
	}
	return r;
}

static void add_name(char ***names, size_t *n, char *name)
{
	*names = xrealloc(*names, (*n + 1) * sizeof **names);
	(*names)[(*n)++] = name;
}

static void collect(const char *dir, const char *prefix, char ***names, size_t *n)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d))) {
		size_t len = strlen(de->d_name);
		struct stat st;
		char *full, *name;

		if (de->d_name[0] == '.' || (len > 5 && !strcmp(de->d_name + len - 5, ".lock")))
			continue;
		full = path_join(dir, de->d_name);
		name = xmalloc(strlen(prefix) + len + 2);
		sprintf(name, "%s%s", prefix, de->d_name);
		if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
			strcat(name, "/");
			collect(full, name, names, n);
			free(name);
		} else {
			add_name(names, n, name);
		}
		free(full);
	}
	closedir(d);
}

/* Whether a linked worktree (git worktree add) has branch name checked out; git branch marks those with '+'. */
int checked_out_elsewhere(const char *name)
{
	char *dir = path_join(git_dir, "worktrees");
	DIR *d = opendir(dir);
	struct dirent *de;
	int found = 0;

	while (d && !found && (de = readdir(d))) {
		buf head = { 0 }, path = { 0 };

		if (de->d_name[0] == '.')
			continue;
		buf_printf(&path, "%s/%s/HEAD", dir, de->d_name);
		if (!read_file((char *)path.p, &head)) {
			while (head.len && isspace(head.p[head.len - 1]))
				head.p[--head.len] = 0;
			found = !strncmp((char *)head.p, "ref: refs/heads/", 16) && !strcmp((char *)head.p + 16, name);
		}
		buf_free(&head);
		buf_free(&path);
	}
	if (d)
		closedir(d);
	free(dir);
	return found;
}

/* The refs under prefix ("refs/heads/", or "refs/" for all), named without it, loose and packed, sorted and unique. */
char **ref_list(const char *prefix, size_t *count)
{
	char **names = NULL, *dir = path_join(git_dir, prefix), *path = path_join(git_dir, "packed-refs");
	char *line, *next;
	size_t n = 0, i, j, np = strlen(prefix);
	buf b = { 0 };

	collect(dir, "", &names, &n);
	if (!read_file(path, &b)) {
		for (line = (char *)b.p; line; line = next) {
			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			if (strlen(line) > 41 + np && line[40] == ' ' && !strncmp(line + 41, prefix, np))
				add_name(&names, &n, xstrdup(line + 41 + np));
		}
	}
	buf_free(&b);
	free(dir);
	free(path);
	if (n)
		qsort(names, n, sizeof *names, cmp_name);
	for (i = j = 0; i < n; i++) {
		if (j && !strcmp(names[j - 1], names[i]))
			free(names[i]);
		else
			names[j++] = names[i];
	}
	*count = j;
	return names;
}

/* Branch names (without refs/heads/), loose and packed, sorted and unique. */
char **branch_list(size_t *count)
{
	return ref_list("refs/heads/", count);
}
