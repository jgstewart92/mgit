#define _DEFAULT_SOURCE
#include "mgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *src_dir, *dst_dir;

static void read_from(const char *dir, const unsigned char id[20], int want, buf *out)
{
	char hex[41];
	int type;

	git_dir = (char *)dir;
	object_read_or_die(id, &type, out);
	id_to_hex(id, hex);
	if (type != want)
		die("object %s is a %s, expected a %s", hex, type_name(type), type_name(want));
}

static int in_dst(const unsigned char id[20])
{
	git_dir = dst_dir;
	return object_exists(id);
}

static void write_dst(int type, const buf *b, const unsigned char id[20])
{
	unsigned char got[20];

	git_dir = dst_dir;
	object_write(type, b->p, b->len, got);
	if (memcmp(got, id, 20))
		die("object copy produced a different id");
}

/* Children are written before their tree, so a tree present in the destination is complete. */
static void copy_tree(const unsigned char id[20], int depth)
{
	const unsigned char *p, *end, *eid;
	const char *name;
	unsigned mode;
	buf b = { 0 };

	if (depth > TREE_DEPTH_MAX)
		die("trees are nested more than %d deep", TREE_DEPTH_MAX);
	if (in_dst(id))
		return;
	read_from(src_dir, id, OBJ_TREE, &b);
	p = b.p;
	end = b.p + b.len;
	while (tree_next(&p, end, &mode, &name, &eid)) {
		if (mode == 040000) {
			copy_tree(eid, depth + 1);
		} else if (mode == 0160000) {
			; /* a submodule's commit lives in its own repository */
		} else if (!in_dst(eid)) {
			buf blob = { 0 };

			read_from(src_dir, eid, OBJ_BLOB, &blob);
			write_dst(OBJ_BLOB, &blob, eid);
			buf_free(&blob);
		}
	}
	write_dst(OBJ_TREE, &b, id);
	buf_free(&b);
}

/*
 * Copies every commit reachable from tip that the destination lacks, parents first. A commit already
 * in the destination is taken to have its whole history there, as git assumes for a complete repository.
 */
static void copy_history(const unsigned char tip[20])
{
	struct entry {
		unsigned char id[20];
		int expanded;
	} *stack = NULL;
	struct idset done = { 0 };
	size_t n = 0, cap = 0;

	if (in_dst(tip))
		return;
	stack = xmalloc(sizeof *stack);
	cap = 1;
	memcpy(stack[0].id, tip, 20);
	stack[0].expanded = 0;
	n = 1;
	while (n) {
		struct entry *e = &stack[n - 1];
		struct commit c;
		buf b = { 0 };
		int i;

		if (idset_has(&done, e->id) || in_dst(e->id)) {
			n--;
			continue;
		}
		read_from(src_dir, e->id, OBJ_COMMIT, &b);
		commit_parse(&b, &c);
		if (!e->expanded) {
			e->expanded = 1;
			for (i = 0; i < c.nparent; i++) {
				if (idset_has(&done, c.parent[i]) || in_dst(c.parent[i]))
					continue;
				if (n == cap) {
					cap *= 2;
					stack = xrealloc(stack, cap * sizeof *stack);
				}
				memcpy(stack[n].id, c.parent[i], 20);
				stack[n].expanded = 0;
				n++;
			}
		} else {
			copy_tree(c.tree, 0);
			write_dst(OBJ_COMMIT, &b, e->id);
			idset_add(&done, e->id);
			n--;
		}
		free(c.parent);
		buf_free(&b);
	}
	free(stack);
	free(done.v);
	free(done.used);
}

/* Whether anc is reachable from tip, walking parents in the destination repository. */
static int is_ancestor(const unsigned char anc[20], const unsigned char tip[20])
{
	unsigned char (*queue)[20] = xmalloc(20);
	struct idset seen = { 0 };
	size_t head = 0, n = 1, cap = 1;
	int found = 0;

	memcpy(queue[0], tip, 20);
	idset_add(&seen, tip);
	while (head < n && !found) {
		struct commit c;
		buf b = { 0 };
		int i;

		if (!memcmp(queue[head], anc, 20)) {
			found = 1;
			break;
		}
		read_from(dst_dir, queue[head++], OBJ_COMMIT, &b);
		commit_parse(&b, &c);
		for (i = 0; i < c.nparent; i++) {
			if (idset_has(&seen, c.parent[i]))
				continue;
			idset_add(&seen, c.parent[i]);
			if (n == cap) {
				cap *= 2;
				queue = xrealloc(queue, cap * 20);
			}
			memcpy(queue[n++], c.parent[i], 20);
		}
		free(c.parent);
		buf_free(&b);
	}
	free(queue);
	free(seen.v);
	free(seen.used);
	return found;
}

/* dir's git directory: dir/.git, or dir itself when it is a bare repository. */
char *other_repo(const char *dir, int *bare)
{
	char *g = path_join(dir, ".git"), *h = path_join(g, "HEAD");
	struct stat st;

	if (!stat(h, &st)) {
		free(h);
		*bare = 0;
		return g;
	}
	free(h);
	free(g);
	h = path_join(dir, "HEAD");
	g = path_join(dir, "objects");
	if (!stat(h, &st) && !stat(g, &st) && S_ISDIR(st.st_mode)) {
		free(h);
		free(g);
		*bare = 1;
		return xstrdup(dir);
	}
	die("%s is not a repository", dir);
}

static const char *short_branch(const char *ref)
{
	return strncmp(ref, "refs/heads/", 11) ? ref : ref + 11;
}

static char *branch_ref(const char *name)
{
	buf ref = { 0 };

	buf_printf(&ref, "refs/heads/%s", name);
	if (name[0] == '-' || !refname_ok((char *)ref.p))
		die("invalid branch name: %s", name);
	return (char *)ref.p;
}

/* push to a repository on this machine: copies the objects across and fast-forwards the branch there */
int push_local(const char *dir, const char *branch)
{
	unsigned char tip[20], old[20];
	char *ref, *head, h1[41], h2[41];
	int bare, have_old;

	src_dir = git_dir;
	if (branch) {
		ref = branch_ref(branch);
	} else {
		ref = head_symref();
		if (!ref || strncmp(ref, "refs/heads/", 11))
			die("HEAD is detached; name the branch to push");
	}
	if (ref_read(ref, tip))
		die("no branch %s here", ref + 11);
	dst_dir = other_repo(dir, &bare);
	git_dir = dst_dir;
	head = bare ? NULL : head_symref();
	if ((head && !strcmp(head, ref)) || checked_out_elsewhere(ref + 11))
		die("refusing to update %s in %s: it is checked out there (push to a bare repository, or pull from there)",
		    ref + 11, dir);
	have_old = ref_read(ref, old) == 0;
	if (have_old && !memcmp(old, tip, 20)) {
		puts("Everything up to date");
		return 0;
	}
	copy_history(tip);
	if (have_old && !is_ancestor(old, tip))
		die("rejected: %s in %s has commits this branch lacks (not a fast-forward)", ref + 11, dir);
	git_dir = dst_dir;
	ref_update(ref, tip, have_old ? old : NULL);
	id_to_hex(tip, h2);
	if (have_old) {
		id_to_hex(old, h1);
		printf("%.7s..%.7s  %s -> %s\n", h1, h2, ref + 11, ref + 11);
	} else {
		printf("* [new branch]  %s -> %s\n", ref + 11, ref + 11);
	}
	git_dir = src_dir;
	return 0;
}

/* Moves the checked-out branch to tip when that is a fast-forward (or the branch is unborn); refuses otherwise. */
static int fast_forward(struct repo_state *s, const unsigned char tip[20])
{
	struct files to = { 0 };
	unsigned char tree[20];
	char h1[41], h2[41];

	git_dir = dst_dir;
	if (s->have_head && (!memcmp(s->head, tip, 20) || is_ancestor(tip, s->head))) {
		puts("Already up to date.");
		return 0;
	}
	if (s->have_head && !is_ancestor(s->head, tip))
		die("not a fast-forward: the branches have diverged, and mgit cannot merge yet");
	git_dir = dst_dir;
	commit_tree_id(tip, tree);
	tree_flatten(tree, &to);
	worktree_switch(&s->head_files, &to, s);
	ref_update(s->head_ref, tip, s->have_head ? s->head : NULL);
	id_to_hex(tip, h2);
	if (s->have_head) {
		id_to_hex(s->head, h1);
		printf("Fast-forward %.7s..%.7s\n", h1, h2);
	} else {
		printf("Checked out %.7s\n", h2);
	}
	checkout_notices(&s->head_files, &to);
	files_free(&to);
	return 0;
}

static const char pull_usage[] = "usage: mgit pull [<remote> [<branch>]], or mgit pull <dir> [<branch>]";

/*
 * pull [REMOTE [BRANCH]]: fetch REMOTE (the branch's remote, else origin), then fast-forward the current branch to
 * the remote-tracking ref of BRANCH (the branch's merge ref, else its own name). pull DIR [BRANCH], where DIR is no
 * remote's name: the objects copied straight from the repository at DIR, then the same fast-forward, as before.
 */
int cmd_pull(int argc, char **argv)
{
	struct repo_state s;
	unsigned char tip[20];
	char *ref, *url;
	int bare;

	if (argc > 3 || (argc > 1 && argv[1][0] == '-'))
		die("%s", pull_usage);
	repo_find();
	dst_dir = git_dir;
	state_load(&s);
	if (!s.head_ref || strncmp(s.head_ref, "refs/heads/", 11))
		die("HEAD is detached; check out the branch to pull into");
	if (argc == 1 || (url = remote_url(argv[1], 0))) {
		const char *branch = s.head_ref + 11;
		char *remote = argc > 1 ? xstrdup(argv[1]) : config_get_sub("branch", branch, "remote"), *merge, *track;
		int r;

		if (!remote)
			remote = xstrdup("origin");
		merge = argc == 3 ? branch_ref(argv[2]) : config_get_sub("branch", branch, "merge");
		if (!merge)
			merge = xstrdup(s.head_ref);
		r = fetch_remote(remote);
		git_dir = dst_dir;
		if (!(track = fetch_track(remote, merge)))
			die("the fetch lines of remote %s do not take %s", remote, merge);
		if (ref_read(track, tip))
			die("%s has no branch %s", remote, short_branch(merge));
		return fast_forward(&s, tip) || r;
	}
	ref = argc == 3 ? branch_ref(argv[2]) : s.head_ref;
	src_dir = other_repo(argv[1], &bare);
	git_dir = src_dir;
	if (ref_read(ref, tip))
		die("%s has no branch %s", argv[1], ref + 11);
	copy_history(tip);
	return fast_forward(&s, tip);
}
