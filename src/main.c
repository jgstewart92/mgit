#define _DEFAULT_SOURCE
#include "mgit.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static const char usage_text[] =
	"mgit " MGIT_VERSION ", a git subset in git's own format\n"
	"usage: mgit <command> [<args>]\n"
	"\n"
	"  init [<dir>]                       create an empty repository (branch main)\n"
	"  status                             changes since HEAD (what commit would record)\n"
	"  diff [--quiet | --name-only | --stat] [-- <path>...]\n"
	"                                     those changes as a patch, names, a diffstat, or exit 1 if any\n"
	"  commit -m <msg> [-m <msg>...] [--author 'Name <email>'] [-q] [-- <path>...]\n"
	"                                     snapshot every file not ignored, plus tracked files; with paths,\n"
	"                                     only those (deletions in them included), the rest as in HEAD\n"
	"  log [<rev>]                        first-parent history\n"
	"  branch [<name> [<start>]]          list or create branches\n"
	"  checkout (<branch> | <commit>)     switch, keeping local changes the switch does not touch\n"
	"  tag [-a] [-m <msg>...] <name> [<rev>]\n"
	"                                     a tag (annotated with -a or -m); with no name, list them\n"
	"  remote [-v] | remote add <name> <url>\n"
	"                                     list the remotes, or add one\n"
	"  push [--dry-run] [<remote> [<refspec>...]]\n"
	"                                     over ssh (git-receive-pack), after the pre-push check: fast-forward\n"
	"                                     branches and add tags there; to a local path, one branch as before\n"
	"  check [<rev>]                      the pre-push check alone (rules: $XDG_CONFIG_HOME/mgit/check.conf)\n"
	"  clone <url> [<dir>]                a repository over ssh, https (anonymously) or from a path, with\n"
	"                                     its remote origin, its tags and its default branch checked out\n"
	"  fetch [<remote>]                   new commits and tags from a remote (origin by default)\n"
	"  pull [<remote> [<branch>]]         fetch, then fast-forward the current branch to its upstream\n"
	"  pull <dir> [<branch>]              fast-forward the current branch from another repository\n"
	"  gc [-q]                            pack the loose objects the refs reach into one pack\n"
	"  rev-parse [--short[=<n>]] <rev>... print object ids (<ref>, <hex>, ~N, ^N), or one abbreviated\n"
	"  write-tree                         store the working tree, print its tree id\n"
	"  hash-object [-w] (--stdin | <file>...)\n"
	"  cat-file (-t | -s | -e | -p | <type>) <object>\n"
	"  --version\n";

static int usage(void)
{
	fputs(usage_text, stderr);
	return 129;
}

static void create_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);

	if (fd < 0)
		die("cannot create %s: %s", path, strerror(errno));
	write_all(fd, text, strlen(text), path);
	if (close(fd))
		die("cannot write %s: %s", path, strerror(errno));
}

/* An empty repository in dir (made if need be), on branch main; returns its git directory's absolute path. */
char *repo_init(const char *dir)
{
	static const char *const dirs[] = { "objects", "refs", "refs/heads", "refs/tags" };
	char *g, *p, abs[PATH_MAX];
	size_t i;

	if (mkdir(dir, 0777) && errno != EEXIST)
		die("cannot create %s: %s", dir, strerror(errno));
	g = path_join(dir, ".git");
	if (mkdir(g, 0777))
		die("cannot create %s: %s", g, strerror(errno));
	for (i = 0; i < sizeof dirs / sizeof *dirs; i++) {
		p = path_join(g, dirs[i]);
		if (mkdir(p, 0777))
			die("cannot create %s: %s", p, strerror(errno));
		free(p);
	}
	p = path_join(g, "HEAD");
	create_file(p, "ref: refs/heads/main\n");
	free(p);
	p = path_join(g, "config");
	create_file(p, "[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n");
	free(p);
	if (!realpath(g, abs))
		die("cannot resolve %s: %s", g, strerror(errno));
	free(g);
	return xstrdup(abs);
}

static int cmd_init(int argc, char **argv)
{
	char *g;

	if (argc > 2)
		return usage();
	g = repo_init(argc > 1 ? argv[1] : ".");
	printf("Initialized empty repository in %s/\n", g);
	free(g);
	return 0;
}

static void hash_one(const buf *b, int store)
{
	unsigned char id[20];
	char hex[41];

	if (store)
		object_write(OBJ_BLOB, b->p, b->len, id);
	else
		object_hash(OBJ_BLOB, b->p, b->len, id);
	id_to_hex(id, hex);
	puts(hex);
}

static int cmd_hash_object(int argc, char **argv)
{
	int i, store = 0, from_stdin = 0, files = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w"))
			store = 1;
		else if (!strcmp(argv[i], "--stdin"))
			from_stdin = 1;
		else if (argv[i][0] == '-')
			return usage();
		else
			files++;
	}
	if (from_stdin ? files > 0 : files == 0)
		return usage();
	if (store)
		repo_find();
	if (from_stdin) {
		buf b = { 0 };

		if (read_fd(0, &b))
			die("cannot read standard input: %s", strerror(errno));
		hash_one(&b, store);
		buf_free(&b);
	}
	for (i = 1; i < argc; i++) {
		buf b = { 0 };

		if (argv[i][0] == '-')
			continue;
		if (read_file(argv[i], &b))
			die("cannot read %s: %s", argv[i], strerror(errno));
		hash_one(&b, store);
		buf_free(&b);
	}
	return 0;
}

static int cmd_cat_file(int argc, char **argv)
{
	unsigned char id[20];
	buf b = { 0 };
	const char *opt;
	int type, want = 0;

	if (argc != 3)
		return usage();
	opt = argv[1];
	if (opt[0] != '-' && !(want = type_from_name(opt, strlen(opt))))
		return usage();
	if (opt[0] == '-' && strcmp(opt, "-t") && strcmp(opt, "-s") && strcmp(opt, "-e") && strcmp(opt, "-p"))
		return usage();
	repo_find();
	if (rev_parse(argv[2], id)) {
		if (!strcmp(opt, "-e"))
			return 1;
		die("not a valid object name: %s", argv[2]);
	}
	if (!strcmp(opt, "-e"))
		return object_exists(id) ? 0 : 1;
	object_read_or_die(id, &type, &b);
	if (!strcmp(opt, "-t"))
		puts(type_name(type));
	else if (!strcmp(opt, "-s"))
		printf("%zu\n", b.len);
	else if (!strcmp(opt, "-p") && type == OBJ_TREE)
		tree_print(&b);
	else if (!strcmp(opt, "-p") || want == type)
		fwrite(b.p, 1, b.len, stdout);
	else
		die("%s is a %s, not a %s", argv[2], type_name(type), opt);
	buf_free(&b);
	return 0;
}

static int cmd_write_tree(int argc, char **argv)
{
	struct repo_state s;
	struct files w = { 0 };
	unsigned char id[20];
	char hex[41];

	(void)argv;
	if (argc != 1)
		return usage();
	repo_find();
	state_load(&s);
	worktree_scan(&s, 1, NULL, &w);
	tree_build(&w, id);
	id_to_hex(id, hex);
	puts(hex);
	return 0;
}

static int cmd_branch(int argc, char **argv)
{
	const char *start = argc == 3 ? argv[2] : "HEAD";
	unsigned char id[20];
	char *head, **names, hex[41];
	size_t i, n;
	buf ref = { 0 };

	if (argc > 3)
		return usage();
	repo_find();
	if (argc == 1) {
		head = head_symref();
		if (!head && !ref_read("HEAD", id)) {
			id_to_hex(id, hex);
			printf("* (HEAD detached at %.7s)\n", hex);
		}
		names = branch_list(&n);
		for (i = 0; i < n; i++) {
			int cur = head && !strncmp(head, "refs/heads/", 11) && !strcmp(head + 11, names[i]);

			printf("%c %s\n", cur ? '*' : checked_out_elsewhere(names[i]) ? '+' : ' ', names[i]);
			free(names[i]);
		}
		free(names);
		free(head);
		return 0;
	}
	buf_printf(&ref, "refs/heads/%s", argv[1]);
	if (argv[1][0] == '-' || !strcmp(argv[1], "HEAD") || !refname_ok((char *)ref.p))
		die("invalid branch name: %s", argv[1]);
	if (!ref_read((char *)ref.p, id))
		die("a branch named '%s' already exists", argv[1]);
	if (rev_parse(start, id))
		die("not a valid revision: %s", start);
	peel_to_commit(id);
	ref_update((char *)ref.p, id, NULL);
	buf_free(&ref);
	return 0;
}

/*
 * git's abbreviation: at least len digits (len < 0: 7, or more once the packs hold 2^14 objects or more, from the
 * same count git uses), extended until no other object shares the prefix.
 */
static int abbrev_len(const unsigned char id[20], int len)
{
	unsigned char other[20];
	char hex[41];

	if (len < 0) {
		unsigned long count = pack_object_count();
		int bits = 0;

		while (count >> bits > 1)
			bits++;
		len = (bits + 2) / 2;
		if (len < 7)
			len = 7;
	}
	if (len < 4)
		len = 4;
	id_to_hex(id, hex);
	while (len < 40 && object_by_prefix(hex, (size_t)len, other) > 1)
		len++;
	return len;
}

static int cmd_rev_parse(int argc, char **argv)
{
	unsigned char id[20];
	char hex[41];
	int i, abbrev = 0, len = -1;

	if (argc < 2)
		return usage();
	if (!strcmp(argv[1], "--short") || !strncmp(argv[1], "--short=", 8)) {
		/* as in git, --short is --verify: one revision */
		if (argc != 3)
			die("usage: mgit rev-parse --short[=<n>] <rev>");
		if (argv[1][7]) {
			char *e;
			unsigned long n = strtoul(argv[1] + 8, &e, 10);

			if (*e || e == argv[1] + 8)
				die("--short takes a number: %s", argv[1]);
			len = n > 40 ? 40 : (int)n;
		}
		abbrev = 1;
		argv++;
		argc--;
	}
	repo_find();
	for (i = 1; i < argc; i++) {
		if (rev_parse(argv[i], id))
			die("not a valid revision: %s", argv[i]);
		id_to_hex(id, hex);
		if (abbrev)
			hex[abbrev_len(id, len)] = 0;
		puts(hex);
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		int (*fn)(int, char **);
	} cmds[] = {
		{ "init", cmd_init },
		{ "status", cmd_status },
		{ "diff", cmd_diff },
		{ "commit", cmd_commit },
		{ "log", cmd_log },
		{ "branch", cmd_branch },
		{ "checkout", cmd_checkout },
		{ "push", cmd_push },
		{ "tag", cmd_tag },
		{ "remote", cmd_remote },
		{ "check", cmd_check },
		{ "gc", cmd_gc },
		{ "pull", cmd_pull },
		{ "clone", cmd_clone },
		{ "fetch", cmd_fetch },
		{ "rev-parse", cmd_rev_parse },
		{ "write-tree", cmd_write_tree },
		{ "hash-object", cmd_hash_object },
		{ "cat-file", cmd_cat_file },
	};
	size_t i;

	if (argc < 2)
		return usage();
	if (argc == 2 && !strcmp(argv[1], "--version")) {
		puts("mgit " MGIT_VERSION);
		return 0;
	}
	for (i = 0; i < sizeof cmds / sizeof *cmds; i++) {
		if (!strcmp(argv[1], cmds[i].name)) {
			int r = cmds[i].fn(argc - 1, argv + 1);

			if (fflush(stdout) || ferror(stdout))
				die("cannot write output: %s", strerror(errno));
			return r;
		}
	}
	return usage();
}
