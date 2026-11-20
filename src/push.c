#define _DEFAULT_SOURCE
#include "mgit.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * push over ssh, as git does it: the pre-push check first, on what would go by the remote-tracking refs, before
 * anything connects; then ssh HOST git-receive-pack 'PATH'; read the refs the far side advertises (pkt-lines); check
 * again only what the far side turns out to lack beyond that; send the updates and one pack of the objects the far
 * side lacks, with deltas (on objects in the pack and on what the far side has); read its report-status, over
 * side-band-64k when it offers that, with its messages shown as "remote: " lines. Fast-forwards and new refs only; an
 * existing tag is never moved. A local path still goes through push_local, as before.
 */

static const unsigned char zero_id[20];

struct update {
	char *src, *dst;	/* full ref names, here and there */
	unsigned char old[20], new[20];
	int have_old;
	const char *status;	/* NULL while it is to be sent */
};


/* A refspec SRC[:DST]: SRC a branch, a tag or HEAD here; DST defaults to SRC's full name, short names keep SRC's kind. */
static void parse_refspec(const char *spec, struct update *u)
{
	const char *colon = strchr(spec, ':');
	char *src = colon ? xstrndup(spec, (size_t)(colon - spec)) : xstrdup(spec), *full = NULL;
	buf b = { 0 };
	unsigned char id[20];

	if (spec[0] == '+')
		die("refusing %s: mgit pushes fast-forwards only (no force)", spec);
	if (!*src)
		die("refusing %s: mgit does not delete remote refs", spec);
	if (!strcmp(src, "HEAD")) {
		full = head_symref();
		if (!full)
			die("HEAD is detached; name the branch to push");
	} else if (!strncmp(src, "refs/", 5)) {
		full = xstrdup(src);
	} else {
		int heads, tags;

		buf_printf(&b, "refs/heads/%s", src);
		heads = refname_ok((char *)b.p) && !ref_read((char *)b.p, id);
		b.len = 0;
		buf_printf(&b, "refs/tags/%s", src);
		tags = refname_ok((char *)b.p) && !ref_read((char *)b.p, id);
		if (heads && tags)
			die("%s is both a branch and a tag here; push refs/heads/%s or refs/tags/%s", src, src, src);
		b.len = 0;
		buf_printf(&b, "refs/%s/%s", tags ? "tags" : "heads", src);
		full = xstrdup((char *)b.p);
	}
	if (!refname_ok(full) || ref_read(full, u->new))
		die("src refspec %s does not match any branch or tag here", src);
	u->src = full;
	if (!colon || !colon[1]) {
		u->dst = xstrdup(full);
	} else if (!strncmp(colon + 1, "refs/", 5)) {
		u->dst = xstrdup(colon + 1);
	} else {
		b.len = 0;
		buf_printf(&b, "%s%s", !strncmp(full, "refs/tags/", 10) ? "refs/tags/" : "refs/heads/", colon + 1);
		u->dst = xstrdup((char *)b.p);
	}
	if (!refname_ok(u->dst) || (strncmp(u->dst, "refs/heads/", 11) && strncmp(u->dst, "refs/tags/", 10)))
		die("cannot push to %s: mgit pushes branches (refs/heads/) and tags (refs/tags/)", u->dst);
	buf_free(&b);
	free(src);
}

static const char *short_name(const char *ref)
{
	return !strncmp(ref, "refs/heads/", 11) ? ref + 11 : !strncmp(ref, "refs/tags/", 10) ? ref + 10 : ref;
}

/* why may come from the far side (a report-status "ng" reason): its control characters are shown, not sent */
static void show(const struct update *u, const char *flag, const char *what, const char *why)
{
	buf w = { 0 };

	if (why)
		buf_add_masked(&w, why);
	printf(" %s %-17s %s -> %s%s%s%s\n", flag, what, short_name(u->src), short_name(u->dst), why ? " (" : "",
	       why ? (char *)w.p : "", why ? ")" : "");
	buf_free(&w);
}

static void show_sent(const struct update *u, int tag)
{
	char a[41], b[41], range[40];

	if (!u->have_old) {
		show(u, "*", tag ? "[new tag]" : "[new branch]", NULL);
		return;
	}
	id_to_hex(u->old, a);
	id_to_hex(u->new, b);
	snprintf(range, sizeof range, "%.7s..%.7s", a, b);
	show(u, " ", range, NULL);
}

static const char push_usage[] = "usage: mgit push [--dry-run] [<remote> [<refspec>...]]";

/* What a side-band packet on band 2 says, as git shows it: each line (ended by LF or CR) after "remote: ". */
static void remote_says(const unsigned char *p, size_t n, int *at_start)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (*at_start) {
			fputs("remote: ", stderr);
			*at_start = 0;
		}
		remote_putc(p[i]);
		if (p[i] == '\n' || p[i] == '\r')
			*at_start = 1;
	}
}

/*
 * The far side's answer after the pack: over side-band, the report-status pkt-lines arrive inside band 1 (gathered
 * into rs), band 2 is shown, band 3 is an error; without it the report comes straight.
 */
static void read_report(int from, buf *rs)
{
	buf b = { 0 };
	int at_start = 1;

	while (pkt_read_raw(from, &b)) {
		if (!b.len)
			continue;
		if (b.p[0] == 1) {
			buf_add(rs, b.p + 1, b.len - 1);
		} else if (b.p[0] == 2) {
			remote_says(b.p + 1, b.len - 1, &at_start);
		} else if (b.p[0] == 3) {
			fflush(stdout);
			die("the remote reported an error: %.*s", (int)(b.len - 1 - (b.p[b.len - 1] == '\n')), (char *)b.p + 1);
		} else {
			die("bad side-band packet from the remote (band %d)", b.p[0]);
		}
	}
	if (!at_start)
		fputc('\n', stderr);
	buf_free(&b);
}

/* One pkt-line from rs at *at into line (its newline dropped); 0 for a flush or the end. */
static int rs_line(const buf *rs, size_t *at, buf *line)
{
	char h[5], *e;
	unsigned long n;

	line->len = 0;
	if (rs->len - *at < 4)
		return 0;
	memcpy(h, rs->p + *at, 4);
	h[4] = 0;
	n = strtoul(h, &e, 16);
	if (*e || !is_hex(h, 4) || (n && n < 4) || (n && n - 4 > rs->len - *at - 4))
		die("bad pkt-line length in the remote's report: '%s'", h);
	*at += 4;
	if (!n)
		return 0;
	buf_add(line, rs->p + *at, n - 4);
	*at += n - 4;
	if (line->len && line->p[line->len - 1] == '\n')
		line->p[--line->len] = 0;
	return 1;
}

/* The refs the remote-tracking refs say a named remote has: refs/remotes/NAME/X as refs/heads/X. */
static size_t tracked_tips(const char *remote, unsigned char (**tips)[20])
{
	char **names;
	size_t n, i, k = 0;
	buf b = { 0 };

	if (!remote) {
		*tips = xmalloc(sizeof **tips);
		return 0;
	}
	buf_printf(&b, "refs/remotes/%s/", remote);
	names = ref_list((char *)b.p, &n);
	*tips = xmalloc((n + 1) * sizeof **tips);
	for (i = 0; i < n; i++) {
		buf name = { 0 };

		buf_printf(&name, "%s%s", (char *)b.p, names[i]);
		if (!ref_read((char *)name.p, (*tips)[k]))
			k++;
		buf_free(&name);
		free(names[i]);
	}
	free(names);
	buf_free(&b);
	return k;
}

int cmd_push(int argc, char **argv)
{
	struct update *ups;
	struct advert adv;
	struct target t;
	struct objlist objs = { 0 }, pre_objs = { 0 };
	struct idset seen = { 0 }, pre_seen = { 0 }, pre_ids = { 0 }, sent = { 0 };
	struct check_ref *refs, *pre_refs;
	unsigned char (*tips)[20], (*tracked)[20], (*edges)[20];
	const char *remote = NULL, *caps;
	char *url;
	char **specs = NULL;
	size_t nspecs = 0, nups, nsend = 0, i, ntips = 0, ntracked, npre = 0, nedges = 0, at = 0;
	int dry = 0, to, from, rejected = 0, failed = 0, first = 1, status, band = 0, covered;
	buf line = { 0 }, out = { 0 }, rs = { 0 };
	pid_t pid;

	for (i = 1; i < (size_t)argc; i++) {
		if (!strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "-n"))
			dry = 1;
		else if (argv[i][0] == '-')
			die("%s", push_usage);
		else if (!remote)
			remote = argv[i];
		else {
			specs = xrealloc(specs, (nspecs + 1) * sizeof *specs);
			specs[nspecs++] = argv[i];
		}
	}
	repo_find();
	if (!remote) {
		remote = "origin";
		if (!(url = remote_url(remote, 1)))
			die("no remote named origin: name one (mgit push <remote> [<branch>]) or add it (mgit remote add origin <url>)");
	} else if (!(url = remote_url(remote, 1))) {
		url = xstrdup(remote);
		remote = NULL;
	}
	if (parse_url(url, &t, "push") == URL_LOCAL) {
		if (dry || nspecs > 1 || (nspecs == 1 && strchr(specs[0], ':')))
			die("a push to a path on this machine takes one branch name and no --dry-run");
		return push_local(t.path, nspecs ? specs[0] : NULL);
	}

	nups = nspecs ? nspecs : 1;
	ups = calloc(nups, sizeof *ups);
	if (!ups)
		die("out of memory");
	for (i = 0; i < nups; i++)
		parse_refspec(nspecs ? specs[i] : "HEAD", &ups[i]);
	for (i = 0; i < nups; i++) {
		size_t k;

		for (k = 0; k < i; k++)
			if (!strcmp(ups[k].dst, ups[i].dst))
				die("%s is named twice", ups[i].dst);
	}

	/*
	 * The check, before ssh starts: what the push sends if the far side has what the remote-tracking refs say (the
	 * refs whose tracking ref is at the new commit already are taken as up to date and not checked).
	 */
	ntracked = tracked_tips(remote, &tracked);
	walk((const unsigned char (*)[20])tracked, ntracked, &pre_seen, NULL, 1);
	tips = xmalloc((nups + 1) * sizeof *tips);
	pre_refs = xmalloc((nups + 1) * sizeof *pre_refs);
	for (i = 0; i < nups; i++) {
		char *tr = remote && !strncmp(ups[i].dst, "refs/heads/", 11) ? fetch_track(remote, ups[i].dst) : NULL;
		unsigned char cur[20];

		if (!(tr && !ref_read(tr, cur) && !memcmp(cur, ups[i].new, 20))) {
			pre_refs[npre].name = ups[i].dst;
			memcpy(pre_refs[npre].id, ups[i].new, 20);
			memcpy(tips[npre++], ups[i].new, 20);
		}
		free(tr);
	}
	walk((const unsigned char (*)[20])tips, npre, &pre_seen, &pre_objs, 0);
	for (i = 0; i < pre_objs.n; i++)
		idset_add(&pre_ids, pre_objs.v[i].id);
	if (npre && check_run(&pre_objs, pre_refs, npre, "push"))
		die("push refused by the check: nothing was sent (nothing connected)");
	free(tips);

	signal(SIGPIPE, SIG_IGN);
	pid = ssh_start(&t, "git-receive-pack", &to, &from);
	advert_read(from, &adv);
	caps = adv.caps;

	/* what each update is: up to date, new, a fast-forward, or refused */
	for (i = 0; i < nups; i++) {
		struct update *u = &ups[i];
		const struct remote_ref *r = advert_find(&adv, u->dst);
		int tag = !strncmp(u->dst, "refs/tags/", 10);

		if (!r)
			continue;
		u->have_old = 1;
		memcpy(u->old, r->id, 20);
		if (!memcmp(u->old, u->new, 20))
			u->status = "up to date";
		else if (tag)
			u->status = "already exists";
		else if (!object_exists(u->old))
			u->status = "fetch first";
		else if (!descends(u->new, u->old))
			u->status = "non-fast-forward";
	}
	for (i = 0; i < nups; i++)
		if (!ups[i].status)
			nsend++;
		else if (strcmp(ups[i].status, "up to date"))
			rejected = 1;

	/* the objects to send: reachable from the new tips, not from anything the remote has that this repository holds */
	tips = xmalloc((adv.n + nups + 1) * sizeof *tips);
	for (i = 0; i < adv.n; i++)
		memcpy(tips[ntips++], adv.v[i].id, 20);
	walk((const unsigned char (*)[20])tips, ntips, &seen, NULL, 1);
	ntips = 0;
	refs = xmalloc((nups + 1) * sizeof *refs);
	for (i = 0; i < nups; i++)
		if (!ups[i].status) {
			memcpy(tips[ntips++], ups[i].new, 20);
			refs[ntips - 1].name = ups[i].dst;
			memcpy(refs[ntips - 1].id, ups[i].new, 20);
		}
	walk((const unsigned char (*)[20])tips, ntips, &seen, &objs, 0);

	printf("To %s\n", url);
	/* checked already unless the far side lacks more than the remote-tracking refs said, or a ref was taken as up to date */
	covered = 1;
	for (i = 0; i < objs.n && covered; i++)
		covered = idset_has(&pre_ids, objs.v[i].id);
	for (i = 0; i < ntips && covered; i++) {
		size_t k;

		for (k = 0; k < npre && strcmp(pre_refs[k].name, refs[i].name); k++)
			;
		covered = k < npre && !memcmp(pre_refs[k].id, refs[i].id, 20);
	}
	if (nsend && !covered)
		puts("push: the remote lacks more than its remote-tracking refs here say; checking that too");
	if (nsend && !covered && check_run(&objs, refs, ntips, "push")) {
		buf_adds(&out, "0000");
		write_all(to, out.p, out.len, "the remote");
		close(to);
		waitpid(pid, &status, 0);
		die("push refused by the check: nothing was sent");
	}
	for (i = 0; i < nups; i++) {
		const struct update *u = &ups[i];

		if (u->status && !strcmp(u->status, "up to date"))
			show(u, "=", "[up to date]", NULL);
		else if (u->status)
			show(u, "!", "[rejected]", u->status);
		else if (dry)
			show_sent(u, !strncmp(u->dst, "refs/tags/", 10));
	}
	if (dry || !nsend) {
		if (dry && nsend)
			printf("would send %zu object%s; --dry-run sent nothing\n", objs.n, objs.n == 1 ? "" : "s");
		else if (!rejected)
			puts("Everything up to date");
		buf_adds(&out, "0000");
		write_all(to, out.p, out.len, "the remote");
		close(to);
		waitpid(pid, &status, 0);
		return rejected;
	}

	/* the commands, then the pack */
	first = 1;
	for (i = 0; i < nups; i++) {
		const struct update *u = &ups[i];
		char a[41], b[41];
		buf cmd = { 0 };

		if (u->status)
			continue;
		id_to_hex(u->have_old ? u->old : zero_id, a);
		id_to_hex(u->new, b);
		buf_printf(&cmd, "%s %s %s", a, b, u->dst);
		if (first) {
			band = has_cap(caps, "report-status") && has_cap(caps, "side-band-64k");
			buf_addc(&cmd, 0);
			buf_adds(&cmd, has_cap(caps, "report-status") ? "report-status " : "");
			buf_adds(&cmd, band ? "side-band-64k " : "");
			/* as git: no progress from the far side unless stderr is a terminal (its hooks are still heard) */
			buf_adds(&cmd, band && !isatty(2) && has_cap(caps, "quiet") ? "quiet " : "");
			buf_adds(&cmd, "agent=mgit/" MGIT_VERSION);
		}
		buf_addc(&cmd, '\n');
		pkt_add(&out, (char *)cmd.p, cmd.len);
		buf_free(&cmd);
		first = 0;
	}
	buf_adds(&out, "0000");
	write_all(to, out.p, out.len, "the remote");
	/* deltas on what the far side has: the parents of the commits sent that it holds, and the old tips of the refs */
	edges = xmalloc((objs.n + nups + 1) * sizeof *edges);
	for (i = 0; i < objs.n; i++)
		idset_add(&sent, objs.v[i].id);
	for (i = 0; i < nups; i++)
		if (!ups[i].status && ups[i].have_old && object_exists(ups[i].old))
			memcpy(edges[nedges++], ups[i].old, 20);
	for (i = 0; i < objs.n; i++) {
		struct commit c;
		buf b = { 0 };
		int type, k;

		if (objs.v[i].type != OBJ_COMMIT)
			continue;
		object_read_or_die(objs.v[i].id, &type, &b);
		commit_parse(&b, &c);
		for (k = 0; k < c.nparent; k++)
			if (!idset_has(&sent, c.parent[k]) && object_exists(c.parent[k])) {
				size_t e;

				for (e = 0; e < nedges && memcmp(edges[e], c.parent[k], 20); e++)
					;
				if (e == nedges)
					memcpy(edges[nedges++], c.parent[k], 20);
			}
		free(c.parent);
		buf_free(&b);
	}
	pack_send_thin(to, "the remote", &objs, (const unsigned char (*)[20])edges, nedges, has_cap(caps, "ofs-delta"));
	close(to);
	free(edges);
	idset_free(&sent);

	if (has_cap(caps, "report-status")) {
		if (band) {
			read_report(from, &rs);
			if (!rs_line(&rs, &at, &line) || strncmp((char *)line.p, "unpack ", 7))
				die("the remote sent no unpack status");
		} else if (!pkt_read(from, &line) || strncmp((char *)line.p, "unpack ", 7)) {
			die("the remote sent no unpack status");
		}
		if (strcmp((char *)line.p, "unpack ok")) {
			warning("the remote could not unpack the pack: %s", (char *)line.p + 7);
			failed = 1;
		}
		while (band ? rs_line(&rs, &at, &line) : pkt_read(from, &line)) {
			int ok = !strncmp((char *)line.p, "ok ", 3);
			char *name = (char *)line.p + 3, *why = NULL;

			if (!ok && strncmp((char *)line.p, "ng ", 3))
				continue;
			if (!ok && (why = strchr(name, ' ')))
				*why++ = 0;
			for (i = 0; i < nups; i++) {
				struct update *u = &ups[i];

				if (u->status || strcmp(u->dst, name))
					continue;
				if (!ok) {
					show(u, "!", "[remote rejected]", why ? why : "no reason given");
					u->status = "remote rejected";
					failed = 1;
				} else {
					show_sent(u, !strncmp(u->dst, "refs/tags/", 10));
					u->status = "ok";
				}
			}
		}
	} else {
		for (i = 0; i < nups; i++)
			if (!ups[i].status)
				show_sent(&ups[i], !strncmp(ups[i].dst, "refs/tags/", 10));
		puts("(the remote gave no report)");
	}
	close(from);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
		warning("ssh ended badly (status %d)", status);
		failed = 1;
	}
	/* remote-tracking refs for a named remote, as git keeps them */
	for (i = 0; remote && i < nups; i++) {
		struct update *u = &ups[i];
		unsigned char cur[20];
		buf tr = { 0 };

		if (!u->status || strcmp(u->status, "ok") || strncmp(u->dst, "refs/heads/", 11))
			continue;
		buf_printf(&tr, "refs/remotes/%s/%s", remote, u->dst + 11);
		if (refname_ok((char *)tr.p)) {
			int have = !ref_read((char *)tr.p, cur);

			ref_update((char *)tr.p, u->new, have ? cur : NULL);
		}
		buf_free(&tr);
	}
	return rejected || failed;
}
