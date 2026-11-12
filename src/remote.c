#define _DEFAULT_SOURCE
#include "mgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* remote.NAME.url, or for a push remote.NAME.pushurl when set; NULL when the remote has no url. */
char *remote_url(const char *name, int push)
{
	char *u = push ? config_get_sub("remote", name, "pushurl") : NULL;

	return u ? u : config_get_sub("remote", name, "url");
}

/* A config value as git writes one: quoted when it starts or ends with a space or holds # or ;, with \ " tab and newline escaped. */
void config_value(buf *b, const char *v)
{
	size_t n = strlen(v);
	int q = n && (v[0] == ' ' || v[n - 1] == ' ' || strpbrk(v, "#;"));

	if (q)
		buf_addc(b, '"');
	for (; *v; v++) {
		if (*v == '\n')
			buf_adds(b, "\\n");
		else if (*v == '\t')
			buf_adds(b, "\\t");
		else {
			if (*v == '"' || *v == '\\')
				buf_addc(b, '\\');
			buf_addc(b, *v);
		}
	}
	if (q)
		buf_addc(b, '"');
}

static const char remote_usage[] = "usage: mgit remote [-v], or mgit remote add <name> <url>";

int cmd_remote(int argc, char **argv)
{
	char **names;
	size_t n, i;
	int verbose = argc == 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--verbose"));

	repo_find();
	if (argc == 4 && !strcmp(argv[1], "add")) {
		buf test = { 0 }, text = { 0 };
		char *have;

		buf_printf(&test, "refs/remotes/%s/test", argv[2]);
		if (!*argv[2] || argv[2][0] == '-' || strchr(argv[2], '"') || !refname_ok((char *)test.p))
			die("'%s' is not a valid remote name", argv[2]);
		if ((have = remote_url(argv[2], 0))) {
			fprintf(stderr, "error: remote %s already exists.\n", argv[2]);
			return 3;
		}
		buf_printf(&text, "[remote \"%s\"]\n\turl = ", argv[2]);
		config_value(&text, argv[3]);
		buf_printf(&text, "\n\tfetch = +refs/heads/*:refs/remotes/%s/*\n", argv[2]);
		config_append((char *)text.p);
		buf_free(&test);
		buf_free(&text);
		return 0;
	}
	if (argc > 1 && !verbose)
		die("%s", remote_usage);
	names = config_subsections("remote", &n);
	for (i = 0; i < n; i++) {
		char *url = remote_url(names[i], 0), *push = remote_url(names[i], 1);

		if (url || push) {
			if (!verbose)
				puts(names[i]);
			else {
				if (url)
					printf("%s\t%s (fetch)\n", names[i], url);
				printf("%s\t%s (push)\n", names[i], push ? push : url);
			}
		}
		free(url);
		free(push);
		free(names[i]);
	}
	free(names);
	return 0;
}
