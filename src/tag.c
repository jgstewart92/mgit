#define _DEFAULT_SOURCE
#include "mgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char tag_usage[] = "usage: mgit tag [-a] [-m <message>...] <name> [<rev>], or mgit tag to list";

/*
 * git tag: no arguments lists the tags; NAME [REV] makes a lightweight tag (a ref to REV's object, HEAD by default);
 * with -a or -m it writes a tag object, its tagger the committer's identity, its message cleaned as git tag cleans
 * one (comment lines dropped too).
 */
int cmd_tag(int argc, char **argv)
{
	unsigned char target[20], id[20];
	const char *name = NULL, *rev = "HEAD";
	buf raw = { 0 }, msg = { 0 }, ref = { 0 }, body = { 0 };
	int i, annotate = 0, nargs = 0, type;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-a")) {
			annotate = 1;
		} else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
			if (annotate == 2)
				buf_addc(&raw, '\n');
			buf_adds(&raw, argv[++i]);
			if (raw.len && raw.p[raw.len - 1] != '\n')
				buf_addc(&raw, '\n');
			annotate = 2;
		} else if (argv[i][0] == '-') {
			die("%s", tag_usage);
		} else if (nargs++ == 0) {
			name = argv[i];
		} else if (nargs == 2) {
			rev = argv[i];
		} else {
			die("%s", tag_usage);
		}
	}
	repo_find();
	if (!name) {
		char **names;
		size_t n, k;

		if (annotate)
			die("%s", tag_usage);
		names = ref_list("refs/tags/", &n);
		for (k = 0; k < n; k++) {
			puts(names[k]);
			free(names[k]);
		}
		free(names);
		return 0;
	}
	if (annotate == 1)
		die("an annotated tag needs a message: -m <message>");
	buf_printf(&ref, "refs/tags/%s", name);
	if (name[0] == '-' || !refname_ok((char *)ref.p))
		die("'%s' is not a valid tag name", name);
	if (!ref_read((char *)ref.p, id))
		die("tag '%s' already exists", name);
	if (rev_parse(rev, target))
		die("not a valid revision: %s", rev);
	if (annotate) {
		char hex[41];

		object_read_or_die(target, &type, &body);
		buf_free(&body);
		cleanup_message(&msg, (char *)raw.p, raw.len, 1);
		id_to_hex(target, hex);
		buf_printf(&body, "object %s\ntype %s\ntag %s\n", hex, type_name(type), name);
		ident(&body, "tagger", "committer", NULL);
		buf_addc(&body, '\n');
		buf_add(&body, msg.p, msg.len);
		object_write(OBJ_TAG, body.p, body.len, id);
	} else {
		memcpy(id, target, 20);
	}
	ref_update((char *)ref.p, id, NULL);
	buf_free(&raw);
	buf_free(&msg);
	buf_free(&ref);
	buf_free(&body);
	return 0;
}
