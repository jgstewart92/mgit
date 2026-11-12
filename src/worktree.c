#define _DEFAULT_SOURCE
#include "mgit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void state_load(struct repo_state *s)
{
	memset(s, 0, sizeof *s);
	s->head_ref = head_symref();
	s->have_head = ref_read(s->head_ref ? s->head_ref : "HEAD", s->head) == 0;
	if (!s->head_ref && !s->have_head)
		die("HEAD is invalid");
	if (s->have_head) {
		unsigned char tree[20];

		commit_tree_id(s->head, tree);
		tree_flatten(tree, &s->head_files);
	}
	s->have_index = index_read(&s->index, s->stamp) == 0;
}

/* Blob id of the file or symlink at full (st is its lstat), trusting the index stat cache when it can. */
static void file_id(const struct repo_state *s, const char *full, const char *path, const struct stat *st, int store,
		    unsigned char id[20], struct stat_data *sd)
{
	const struct file *c = files_find(&s->index, path);
	buf b = { 0 };

	if (c && stat_matches(c, st, s->stamp) && (!store || object_exists(c->id))) {
		memcpy(id, c->id, 20);
		stat_data_from(sd, st);
		return;
	}
	if (S_ISLNK(st->st_mode)) {
		if (read_link(full, &b))
			die("cannot read link %s: %s", full, strerror(errno));
		stat_data_from(sd, st);
	} else {
		struct stat fst;
		int fd = open(full, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

		if (fd < 0 || fstat(fd, &fst) || read_fd(fd, &b))
			die("cannot read %s: %s", full, strerror(errno));
		close(fd);
		stat_data_from(sd, &fst);
	}
	if (store)
		object_write(OBJ_BLOB, b.p, b.len, id);
	else
		object_hash(OBJ_BLOB, b.p, b.len, id);
	buf_free(&b);
}

/*
 * The PATHs after "--", read as git reads literal paths: relative to the current directory, "." and ".." resolved,
 * never outside the working tree. Wildcards and pathspec magic are refused rather than taken literally.
 */
void pathspec_parse(struct pathspec *ps, int argc, char **argv)
{
	char cwd[PATH_MAX];
	size_t wl = strcmp(work_tree, "/") ? strlen(work_tree) : 0;
	const char *here;
	int i;

	ps->v = xmalloc((size_t)(argc ? argc : 1) * sizeof *ps->v);
	ps->n = 0;
	if (!getcwd(cwd, sizeof cwd))
		die("cannot get current directory: %s", strerror(errno));
	here = cwd[wl] == '/' ? cwd + wl + 1 : "";
	for (i = 0; i < argc; i++) {
		const char *a = argv[i], *p;
		buf b = { 0 };

		if (!*a || a[0] == ':' || strpbrk(a, "*?[\\"))
			die("path '%s': mgit takes plain paths (no wildcards or pathspec magic)", a);
		if (a[0] == '/') {
			if (strncmp(a, work_tree, wl) || (a[wl] && a[wl] != '/'))
				die("path '%s' is outside the working tree %s", a, work_tree);
			a += wl;
		} else {
			buf_adds(&b, here);
		}
		for (p = a; *p;) {
			size_t n = strcspn(p, "/");

			if (n == 2 && !memcmp(p, "..", 2)) {
				char *slash;

				if (!b.len)
					die("path '%s' is outside the working tree %s", argv[i], work_tree);
				slash = strrchr((char *)b.p, '/');
				b.len = slash ? (size_t)(slash - (char *)b.p) : 0;
				b.p[b.len] = 0;
			} else if (n && !(n == 1 && *p == '.')) {
				if (b.len)
					buf_addc(&b, '/');
				buf_add(&b, p, n);
			}
			p += n + (p[n] == '/');
		}
		ps->v[ps->n++] = b.len ? (char *)b.p : xstrdup("");
		if (!b.len)
			buf_free(&b);
	}
}

static int under(const char *path, const char *spec)
{
	size_t n = strlen(spec);

	return !n || (!strncmp(path, spec, n) && (!path[n] || path[n] == '/'));
}

/* Whether path is one of the named paths or inside one. No pathspec covers everything. */
int pathspec_covers(const struct pathspec *ps, const char *path)
{
	size_t i;

	if (!ps || !ps->n)
		return 1;
	for (i = 0; i < ps->n; i++)
		if (under(path, ps->v[i]))
			return 1;
	return 0;
}

/* Whether a scan must enter directory dir: it is covered, or a named path lies inside it. */
static int pathspec_reaches(const struct pathspec *ps, const char *dir)
{
	size_t i, n = strlen(dir);

	if (pathspec_covers(ps, dir))
		return 1;
	for (i = 0; i < ps->n; i++)
		if (!strncmp(ps->v[i], dir, n) && ps->v[i][n] == '/')
			return 1;
	return 0;
}

/* Copies the entries of from that the pathspec covers (covered = 1) or does not (covered = 0), stat data included. */
void files_keep(const struct files *from, const struct pathspec *ps, int covered, struct files *to)
{
	size_t i;

	for (i = 0; i < from->n; i++) {
		const struct file *f = &from->v[i];

		if (pathspec_covers(ps, f->path) == covered) {
			struct file *e = files_add(to, f->path, f->mode, f->id);

			e->have_stat = f->have_stat;
			e->sd = f->sd;
		}
	}
}

static int has_git(const char *dir)
{
	char *g = path_join(dir, ".git");
	struct stat st;
	int r = lstat(g, &st) == 0;

	free(g);
	return r;
}

struct scan {
	const struct repo_state *s;
	const struct files *tracked;
	struct ignore ig;
	int store;
	const struct pathspec *ps;
	struct files *out;
};

/* Inside an ignored directory only tracked paths are kept, and no .gitignore is read, as with git add -A. */
static void scan_dir(struct scan *sc, const char *dir, const char *rel, int ignored)
{
	size_t mark = ignored ? sc->ig.n : ignore_push(&sc->ig, dir, rel);
	DIR *d = opendir(dir);
	struct dirent *de;
	buf path = { 0 };

	if (!d)
		die("cannot open %s: %s", dir, strerror(errno));
	while ((errno = 0, de = readdir(d))) {
		char *full;
		struct stat st;
		int ign;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, ".git"))
			continue;
		full = path_join(dir, de->d_name);
		if (lstat(full, &st))
			die("cannot stat %s: %s", full, strerror(errno));
		path.len = 0;
		buf_adds(&path, rel);
		buf_adds(&path, de->d_name);
		if (!(S_ISDIR(st.st_mode) ? pathspec_reaches(sc->ps, (char *)path.p) : pathspec_covers(sc->ps, (char *)path.p))) {
			free(full);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			const struct file *sub = files_find(sc->tracked, (char *)path.p);

			if (sub && sub->mode == 0160000) {
				/* a submodule: its commit stays what is tracked, as git sees one it has not cloned */
				files_add(sc->out, sub->path, sub->mode, sub->id);
				free(full);
				continue;
			}
			ign = ignored || ignore_match(&sc->ig, (char *)path.p, 1);
			buf_addc(&path, '/');
			if (has_git(full))
				warning("skipping %s: it has its own .git (submodules are not supported)", (char *)path.p);
			else if (!ign || files_have_prefix(sc->tracked, (char *)path.p))
				scan_dir(sc, full, (char *)path.p, ign);
		} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			ign = ignored || ignore_match(&sc->ig, (char *)path.p, 0);
			if (!ign || files_find(sc->tracked, (char *)path.p)) {
				unsigned char id[20];
				struct file *e;
				struct stat_data sd;

				file_id(sc->s, full, (char *)path.p, &st, sc->store, id, &sd);
				e = files_add(sc->out, (char *)path.p, mode_from_stat(&st), id);
				e->have_stat = 1;
				e->sd = sd;
			}
		} else {
			warning("skipping %s: not a file, symlink or directory", (char *)path.p);
		}
		free(full);
	}
	if (errno)
		die("cannot read directory %s: %s", dir, strerror(errno));
	closedir(d);
	buf_free(&path);
	ignore_pop(&sc->ig, mark);
}

/*
 * The snapshot git add -A would make: every file not ignored, plus every tracked file even if ignored.
 * Tracked means in the index, or in HEAD when there is no index yet. store also writes the blobs. With a pathspec,
 * only what it covers, but read with the .gitignore files of every directory above it.
 */
void worktree_scan(const struct repo_state *s, int store, const struct pathspec *ps, struct files *out)
{
	struct scan sc;

	sc.s = s;
	sc.tracked = s->have_index ? &s->index : &s->head_files;
	sc.store = store;
	sc.ps = ps;
	sc.out = out;
	ignore_init(&sc.ig);
	scan_dir(&sc, work_tree, "", 0);
	ignore_pop(&sc.ig, 0);
	free(sc.ig.v);
	files_sort(out);
}

enum { W_ABSENT, W_FILE, W_DIR, W_OTHER, W_DONE };

/* What the working tree holds at path. A path behind a non-directory ancestor counts as absent, so nothing is read or written through a symlink. */
static int path_state(const struct repo_state *s, const char *path, unsigned *mode, unsigned char id[20],
		      struct stat_data *sd)
{
	char *full = path_join(work_tree, path), *p;
	struct stat st;
	int r = W_ABSENT;

	for (p = full + strlen(work_tree) + 1; (p = strchr(p, '/')); p++) {
		int ok;

		*p = 0;
		ok = !lstat(full, &st) && S_ISDIR(st.st_mode);
		*p = '/';
		if (!ok)
			goto out;
	}
	if (lstat(full, &st)) {
		if (errno != ENOENT && errno != ENOTDIR)
			die("cannot stat %s: %s", full, strerror(errno));
	} else if (S_ISDIR(st.st_mode)) {
		r = W_DIR;
	} else if ((*mode = mode_from_stat(&st))) {
		file_id(s, full, path, &st, 0, id, sd);
		r = W_FILE;
	} else {
		r = W_OTHER;
	}
out:
	free(full);
	return r;
}

static int same_entry(unsigned mode, const unsigned char id[20], const struct file *e)
{
	return e && e->mode == mode && !memcmp(e->id, id, 20);
}

/* A directory standing where a file must go may be removed only if it holds nothing but clean files this switch deletes. */
static int dir_expendable(const struct repo_state *s, const char *rel, const struct files *from, const struct files *to)
{
	char *dir = path_join(work_tree, rel);
	DIR *d = opendir(dir);
	struct dirent *de;
	int ok = d != NULL;

	while (ok && (de = readdir(d))) {
		char *sub;
		unsigned mode;
		unsigned char id[20];
		struct stat_data sd;
		int w;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		sub = path_join(rel, de->d_name);
		w = path_state(s, sub, &mode, id, &sd);
		if (w == W_DIR)
			ok = dir_expendable(s, sub, from, to);
		else
			ok = w == W_FILE && same_entry(mode, id, files_find(from, sub)) && !files_find(to, sub);
		free(sub);
	}
	if (d)
		closedir(d);
	free(dir);
	return ok;
}

static void prune_parents(const char *path)
{
	char *p = xstrdup(path), *slash;

	while ((slash = strrchr(p, '/'))) {
		char *full;
		int r;

		*slash = 0;
		full = path_join(work_tree, p);
		r = rmdir(full);
		free(full);
		if (r)
			break;
	}
	free(p);
}

/* Only reached for a directory dir_expendable() cleared, whose files are already deleted. */
static void remove_empty_dirs(const char *rel)
{
	char *dir = path_join(work_tree, rel);
	DIR *d = opendir(dir);
	struct dirent *de;

	while (d && (de = readdir(d))) {
		char *sub;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		sub = path_join(rel, de->d_name);
		remove_empty_dirs(sub);
		free(sub);
	}
	if (d)
		closedir(d);
	if (rmdir(dir) && errno != ENOENT)
		die("cannot remove directory %s: %s", dir, strerror(errno));
	free(dir);
}

static void make_parents(const char *path)
{
	char *full = path_join(work_tree, path), *p;
	struct stat st;

	for (p = full + strlen(work_tree) + 1; (p = strchr(p, '/')); p++) {
		*p = 0;
		if (mkdir(full, 0777) && (errno != EEXIST || lstat(full, &st) || !S_ISDIR(st.st_mode)))
			die("cannot create directory %s", full);
		*p = '/';
	}
	free(full);
}

static mode_t current_umask(void)
{
	mode_t m = umask(0);

	umask(m);
	return m;
}

/* Replaces whatever is at t->path atomically with the blob, and records the stat of what it wrote. */
static void checkout_file(struct file *t)
{
	char *full = path_join(work_tree, t->path), *tmp;
	struct stat st;
	buf b = { 0 };
	int type, fd;

	if (t->mode == 0160000) {
		make_parents(t->path);
		if (mkdir(full, 0777) && errno != EEXIST)
			die("cannot create directory %s: %s", full, strerror(errno));
		free(full);
		return;
	}
	object_read_or_die(t->id, &type, &b);
	if (type != OBJ_BLOB)
		die("%s: expected a blob", t->path);
	make_parents(t->path);
	tmp = xmalloc(strlen(full) + 16);
	strcpy(tmp, full);
	strcpy(strrchr(tmp, '/') + 1, ".mgit-XXXXXX");
	fd = mkstemp(tmp);
	if (fd < 0)
		die("cannot create %s: %s", tmp, strerror(errno));
	if (t->mode == 0120000) {
		close(fd);
		unlink(tmp);
		if (symlink((char *)b.p, tmp))
			die("cannot create symlink %s: %s", tmp, strerror(errno));
	} else {
		write_all(fd, b.p, b.len, tmp);
		if (fchmod(fd, (t->mode == 0100755 ? 0777 : 0666) & ~current_umask()) || close(fd))
			die("cannot write %s: %s", tmp, strerror(errno));
	}
	if (rename(tmp, full))
		die("cannot rename %s to %s: %s", tmp, full, strerror(errno));
	if (lstat(full, &st))
		die("cannot stat %s: %s", full, strerror(errno));
	stat_data_from(&t->sd, &st);
	t->have_stat = 1;
	free(tmp);
	free(full);
	buf_free(&b);
}

/*
 * Moves the working tree from one snapshot to another, keeping local changes to paths the two do not
 * touch. Refuses, changing nothing, if a path it must write or delete holds anything but the old or
 * the new content. Then writes the index for `to`.
 */
void worktree_switch(const struct files *from, struct files *to, const struct repo_state *s)
{
	struct pair *ch;
	size_t n = files_diff(from, to, &ch), k, i;
	int *state = xmalloc((n ? n : 1) * sizeof *state);
	buf bad = { 0 };

	for (k = 0; k < n; k++) {
		const struct file *f = ch[k].a, *t = ch[k].b;
		const char *path = f ? f->path : t->path;
		unsigned mode = 0;
		unsigned char id[20];
		struct stat_data sd;
		int ok;

		state[k] = path_state(s, path, &mode, id, &sd);
		if (state[k] == W_ABSENT)
			ok = 1;
		else if (state[k] == W_FILE)
			ok = same_entry(mode, id, f) || same_entry(mode, id, t);
		else if (state[k] == W_DIR)
			ok = dir_expendable(s, path, from, to);
		else
			ok = 0;
		if (ok && state[k] == W_FILE && same_entry(mode, id, t))
			state[k] = W_DONE;
		else if (ok && t) {
			char *anc = xstrdup(path), *p;
			struct stat st;

			for (p = anc; ok && (p = strchr(p, '/')); p++) {
				char *full;

				*p = 0;
				full = path_join(work_tree, anc);
				if (!lstat(full, &st) && !S_ISDIR(st.st_mode))
					ok = files_find(from, anc) && !files_find(to, anc);
				free(full);
				*p = '/';
			}
			free(anc);
		}
		if (!ok) {
			buf_adds(&bad, "\t");
			buf_adds(&bad, path);
			buf_adds(&bad, "\n");
		}
	}
	if (bad.len) {
		fprintf(stderr, "mgit: these paths have local changes or untracked files that would be overwritten:\n%s",
			(char *)bad.p);
		die("nothing changed; commit, move or remove them first");
	}

	for (k = 0; k < n; k++) {
		const char *path = ch[k].a ? ch[k].a->path : ch[k].b->path;

		if (!ch[k].b && state[k] == W_FILE) {
			char *full = path_join(work_tree, path);

			if (unlink(full))
				die("cannot remove %s: %s", full, strerror(errno));
			free(full);
			prune_parents(path);
		}
	}
	for (k = 0; k < n; k++) {
		if (ch[k].b && state[k] != W_DONE) {
			struct file *t = files_find(to, ch[k].b->path);

			if (state[k] == W_DIR)
				remove_empty_dirs(t->path);
			checkout_file(t);
		}
	}

	/* Index stat data only where the file is known to hold the entry's content. */
	for (i = 0; i < to->n; i++) {
		struct file *t = &to->v[i];
		unsigned mode = 0;
		unsigned char id[20];
		struct stat_data sd;

		if (t->have_stat)
			continue;
		if (path_state(s, t->path, &mode, id, &sd) == W_FILE && same_entry(mode, id, t)) {
			t->sd = sd;
			t->have_stat = 1;
		}
	}
	index_write(to);
	free(state);
	free(ch);
	buf_free(&bad);
}

int cmd_checkout(int argc, char **argv)
{
	struct repo_state s;
	struct files to = { 0 };
	unsigned char id[20], tree[20];
	char hex[41];
	buf ref = { 0 };
	int branch;

	if (argc != 2)
		die("usage: mgit checkout (<branch> | <commit>)");
	repo_find();
	state_load(&s);
	buf_printf(&ref, "refs/heads/%s", argv[1]);
	branch = argv[1][0] != '-' && refname_ok((char *)ref.p) && !ref_read((char *)ref.p, id);
	if (!branch && rev_parse(argv[1], id))
		die("not a branch or revision: %s", argv[1]);
	peel_to_commit(id);
	commit_tree_id(id, tree);
	tree_flatten(tree, &to);
	worktree_switch(&s.head_files, &to, &s);
	id_to_hex(id, hex);
	if (branch) {
		head_set_symref((char *)ref.p);
		printf("Switched to branch '%s'\n", argv[1]);
	} else {
		ref_update("HEAD", id, s.have_head ? s.head : NULL);
		printf("HEAD is now at %.7s (detached)\n", hex);
	}
	files_free(&to);
	buf_free(&ref);
	return 0;
}
