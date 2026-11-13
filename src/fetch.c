#define _DEFAULT_SOURCE
#include "mgit.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * clone and fetch, and the fetch inside pull. Three sources: a path on this machine (the objects copied across as
 * one pack; no git needed), ssh (git-upload-pack 'PATH' on the far side) and https through curl (smart HTTP), both in
 * protocol v0. The want lines, then have lines in rounds of 32, newest commit first, until the far side ACKs one it
 * has (it then sends only what that commit lacks); one pack back over side-band-64k, indexed here (pack_index), and
 * every object it brings followed to its links before any ref moves. No thin packs, no shallow clones, no
 * credentials.
 */

#define HAVE_ROUND 32
#define RESULT_TYPE "application/x-git-upload-pack-result"

struct source {
	struct target t;
	struct advert adv;
	int to, from;		/* ssh: the pipes to and from it */
	pid_t pid;
	char *dir;		/* a path: the other repository's git directory */
};

static char *tmp_pack;		/* a pack being received, removed at exit unless pack_index took it */
static int tmp_fd = -1;		/* open while it is being written */

static void tmp_cleanup(void)
{
	if (tmp_fd >= 0)
		close(tmp_fd);
	tmp_fd = -1;
	if (tmp_pack) {
		pack_unmap_all();	/* first: on NFS a removed file still open or mapped stays as .nfsXXXX */
		unlink(tmp_pack);
	}
}

static void local_open(struct source *s)
{
	char *save = git_dir, **names, *fmt;
	unsigned char id[20];
	size_t n, i;
	int bare;

	s->dir = other_repo(s->t.path, &bare);
	git_dir = s->dir;
	fmt = config_get("extensions", "objectformat");
	if (fmt && strcasecmp(fmt, "sha1"))
		die("%s uses %s object names; mgit works with SHA-1 repositories only", s->t.path, fmt);
	free(fmt);
	s->adv.caps = xstrdup("");
	s->adv.head = head_symref();
	if (!ref_read("HEAD", id))
		advert_add(&s->adv, "HEAD", id);
	names = ref_list("refs/", &n);
	for (i = 0; i < n; i++) {
		buf name = { 0 };

		buf_printf(&name, "refs/%s", names[i]);
		if (!ref_read((char *)name.p, id))
			advert_add(&s->adv, (char *)name.p, id);
		buf_free(&name);
		free(names[i]);
	}
	free(names);
	git_dir = save;
}

static void http_open(struct source *s)
{
	buf url = { 0 }, line = { 0 };
	int from;
	pid_t pid;

	buf_printf(&url, "%s/info/refs?service=git-upload-pack", s->t.path);
	pid = http_start((char *)url.p, NULL, "application/x-git-upload-pack-advertisement", &from);
	if (!pkt_read(from, &line) || strcmp((char *)line.p, "# service=git-upload-pack"))
		die("%s did not begin with # service=git-upload-pack", (char *)url.p);
	if (pkt_read(from, &line))
		die("%s sent no flush after its service line", (char *)url.p);
	advert_read(from, &s->adv);
	child_finish(pid, from, "curl");
	buf_free(&url);
	buf_free(&line);
}

/* Connects and reads the refs; nothing is written here yet, so a refusal leaves no trace. */
static void source_open(struct source *s, const char *url)
{
	memset(s, 0, sizeof *s);
	s->to = s->from = -1;
	signal(SIGPIPE, SIG_IGN);
	switch (parse_url(url, &s->t, "fetch")) {
	case URL_LOCAL:
		local_open(s);
		break;
	case URL_SSH:
		s->pid = ssh_start(&s->t, "git-upload-pack", &s->to, &s->from);
		advert_read(s->from, &s->adv);
		break;
	default:
		http_open(s);
		break;
	}
}

/* ---- what this side has, newest commit first ---- */

struct haves {
	struct hv {
		long long when;
		unsigned char id[20];
	} *v;
	size_t n, cap;
	struct idset seen;
};

static long long commit_when(const buf *b)
{
	const char *p = (const char *)b->p, *end = p + b->len;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p)), *gt;

		if (!nl || nl == p)
			break;
		if (!strncmp(p, "committer ", 10)) {
			for (gt = nl; gt > p && *gt != '>'; gt--)
				;
			return strtoll(gt + 1, NULL, 10);
		}
		p = nl + 1;
	}
	return 0;
}

static void hv_swap(struct haves *h, size_t a, size_t b)
{
	struct hv t = h->v[a];

	h->v[a] = h->v[b];
	h->v[b] = t;
}

/* Queues the commit id names (a tag is peeled to it); anything missing or not a commit is passed over. */
static void haves_add(struct haves *h, const unsigned char id[20])
{
	unsigned char cur[20];
	buf b = { 0 };
	size_t i;
	int type;

	memcpy(cur, id, 20);
	for (;;) {
		if (!idset_add(&h->seen, cur) || object_read(cur, &type, &b)) {
			buf_free(&b);
			return;
		}
		if (type != OBJ_TAG || b.len < 47 || strncmp((char *)b.p, "object ", 7) || hex_to_id((char *)b.p + 7, cur))
			break;
	}
	if (type == OBJ_COMMIT) {
		if (h->n == h->cap) {
			h->cap = h->cap ? 2 * h->cap : 64;
			h->v = xrealloc(h->v, h->cap * sizeof *h->v);
		}
		i = h->n++;
		h->v[i].when = commit_when(&b);
		memcpy(h->v[i].id, cur, 20);
		for (; i && h->v[(i - 1) / 2].when < h->v[i].when; i = (i - 1) / 2)
			hv_swap(h, i, (i - 1) / 2);
	}
	buf_free(&b);
}

static void haves_init(struct haves *h)
{
	unsigned char id[20];
	char **names;
	size_t n, i;

	memset(h, 0, sizeof *h);
	if (!ref_read("HEAD", id))
		haves_add(h, id);
	names = ref_list("refs/", &n);
	for (i = 0; i < n; i++) {
		buf name = { 0 };

		buf_printf(&name, "refs/%s", names[i]);
		if (!ref_read((char *)name.p, id))
			haves_add(h, id);
		buf_free(&name);
		free(names[i]);
	}
	free(names);
}

/* The newest commit not given yet, its parents queued in its place; 0 when none is left. */
static int haves_next(struct haves *h, unsigned char id[20])
{
	struct commit c;
	buf b = { 0 };
	size_t i = 0;
	int type, k;

	if (!h->n)
		return 0;
	memcpy(id, h->v[0].id, 20);
	h->v[0] = h->v[--h->n];
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i;

		if (l < h->n && h->v[l].when > h->v[m].when)
			m = l;
		if (r < h->n && h->v[r].when > h->v[m].when)
			m = r;
		if (m == i)
			break;
		hv_swap(h, i, m);
		i = m;
	}
	object_read_or_die(id, &type, &b);
	commit_parse(&b, &c);
	for (k = 0; k < c.nparent; k++)
		haves_add(h, c.parent[k]);
	free(c.parent);
	buf_free(&b);
	return 1;
}

static size_t have_lines(buf *req, struct haves *h, size_t max)
{
	unsigned char id[20];
	char hex[41], line[48];
	size_t k = 0;

	while (k < max && haves_next(h, id)) {
		id_to_hex(id, hex);
		snprintf(line, sizeof line, "have %s\n", hex);
		pkt_add(req, line, 46);
		k++;
	}
	return k;
}

/* ---- the exchange ---- */

/*
 * The want lines and their flush; the first carries the capabilities asked for. *band is 2 for side-band-64k.
 * multi_ack_detailed is asked for when the far side has it: each round of haves is then answered by an ACK <id> common
 * for every have it holds and a NAK that ends the round, which is how this side knows when to stop.
 */
static void want_lines(buf *req, const struct source *s, const unsigned char (*want)[20], size_t n, int *band, int *detailed)
{
	const char *caps = s->adv.caps;
	buf line = { 0 };
	size_t i;

	*band = has_cap(caps, "side-band-64k") ? 2 : has_cap(caps, "side-band") ? 1 : 0;
	*detailed = has_cap(caps, "multi_ack_detailed");
	for (i = 0; i < n; i++) {
		char hex[41];

		id_to_hex(want[i], hex);
		line.len = 0;
		buf_printf(&line, "want %s", hex);
		if (!i) {
			if (*detailed)
				buf_adds(&line, " multi_ack_detailed");
			buf_adds(&line, *band == 2 ? " side-band-64k" : *band ? " side-band" : "");
			if (has_cap(caps, "ofs-delta"))
				buf_adds(&line, " ofs-delta");
			if (!isatty(2) && has_cap(caps, "no-progress"))
				buf_adds(&line, " no-progress");
			buf_adds(&line, " agent=mgit/" MGIT_VERSION);
		}
		buf_addc(&line, '\n');
		pkt_add(req, (char *)line.p, line.len);
	}
	buf_adds(req, "0000");
	buf_free(&line);
}

/*
 * The far side's answer to a round of haves, up to the NAK that ends it. Each ACK <id> common (a have it holds) is kept
 * in common as a have line, for the request that ends in done; ACK <id> ready says the same. Returns the count.
 */
static size_t round_answer(int fd, buf *common)
{
	unsigned char id[20];
	buf line = { 0 };
	size_t n = 0;

	for (;;) {
		if (!pkt_read(fd, &line))
			die("the remote sent a flush where an ACK or NAK belongs");
		if (!strcmp((char *)line.p, "NAK"))
			break;
		if (line.len > 44 && !strncmp((char *)line.p, "ACK ", 4) && !hex_to_id((char *)line.p + 4, id) &&
		    (!strcmp((char *)line.p + 44, " common") || !strcmp((char *)line.p + 44, " ready"))) {
			if (line.p[45] == 'c') {
				char l[48];

				snprintf(l, sizeof l, "have %.40s\n", (char *)line.p + 4);
				pkt_add(common, l, 46);
				n++;
			}
			continue;
		}
		die("the remote answered '%.60s' where an ACK or NAK belongs", (char *)line.p);
	}
	buf_free(&line);
	return n;
}

/* After done: past any ACK <id> common, the last answer, ACK <id> (a common commit) or NAK (none); the pack follows. */
static void final_answer(int fd)
{
	unsigned char id[20];
	buf line = { 0 };

	for (;;) {
		if (!pkt_read(fd, &line))
			die("the remote sent a flush where an ACK or NAK belongs");
		if (!strcmp((char *)line.p, "NAK") || (line.len == 44 && !strncmp((char *)line.p, "ACK ", 4) &&
							!hex_to_id((char *)line.p + 4, id)))
			break;
		if (line.len > 44 && !strncmp((char *)line.p, "ACK ", 4) && !hex_to_id((char *)line.p + 4, id) &&
		    (!strcmp((char *)line.p + 44, " common") || !strcmp((char *)line.p + 44, " ready")))
			continue;
		die("the remote answered '%.60s' where an ACK or NAK belongs", (char *)line.p);
	}
	buf_free(&line);
}

static char *tmp_pack_open(int *fd)
{
	static int registered;
	char *dir = path_join(git_dir, "objects/pack");

	if (mkdir(dir, 0777) && errno != EEXIST)
		die("cannot create %s: %s", dir, strerror(errno));
	if (!registered) {
		atexit(tmp_cleanup);
		registered = 1;
	}
	tmp_pack = path_join(dir, "tmp_pack_XXXXXX");
	if ((*fd = mkstemp(tmp_pack)) < 0)
		die("cannot create %s: %s", tmp_pack, strerror(errno));
	tmp_fd = *fd;
	free(dir);
	return tmp_pack;
}

static void tmp_pack_done(int fd, struct objlist *fresh)
{
	int bad = fsync(fd) != 0, e = errno;

	if (close(fd) && !bad) {
		bad = 1;
		e = errno;
	}
	tmp_fd = -1;
	if (bad)
		die("cannot write %s: %s", tmp_pack, strerror(e));
	pack_index(tmp_pack, fresh);
	free(tmp_pack);
	tmp_pack = NULL;
}

/* The pack that follows the last ACK or NAK: in side-band packets (band 1 the pack, 2 progress, 3 an error) or raw. */
static void recv_pack(int fd, int band, struct objlist *fresh)
{
	unsigned char chunk[65536];
	buf b = { 0 };
	int out;

	tmp_pack_open(&out);
	if (band) {
		while (pkt_read_raw(fd, &b)) {
			if (!b.len)
				continue;
			if (b.p[0] == 1)
				write_all(out, b.p + 1, b.len - 1, tmp_pack);
			else if (b.p[0] == 2)
				fwrite(b.p + 1, 1, b.len - 1, stderr);
			else if (b.p[0] == 3)
				die("the remote reported an error: %.*s", (int)(b.len - 1 - (b.p[b.len - 1] == '\n')), (char *)b.p + 1);
			else
				die("bad side-band packet from the remote (band %d)", b.p[0]);
		}
	} else {
		for (;;) {
			ssize_t r = read(fd, chunk, sizeof chunk);

			if (r < 0 && errno == EINTR)
				continue;
			if (r < 0)
				die("cannot read the pack from the remote: %s", strerror(errno));
			if (!r)
				break;
			write_all(out, chunk, (size_t)r, tmp_pack);
		}
	}
	buf_free(&b);
	tmp_pack_done(out, fresh);
}

/*
 * Rounds of up to 32 haves, newest first, each ended by a flush and answered up to its NAK, until a round finds a
 * commit the far side holds; then done. A far side without multi_ack_detailed gets no haves (and sends everything).
 */
static void ssh_fetch(struct source *s, const unsigned char (*want)[20], size_t n, struct objlist *fresh)
{
	struct haves h;
	buf req = { 0 }, common = { 0 };
	int band, detailed;

	want_lines(&req, s, want, n, &band, &detailed);
	write_all(s->to, req.p, req.len, "the remote");
	haves_init(&h);
	while (detailed) {
		req.len = 0;
		if (!have_lines(&req, &h, HAVE_ROUND))
			break;
		buf_adds(&req, "0000");
		write_all(s->to, req.p, req.len, "the remote");
		if (round_answer(s->from, &common))
			break;
	}
	write_all(s->to, "0009done\n", 9, "the remote");
	close(s->to);
	s->to = -1;
	final_answer(s->from);
	recv_pack(s->from, band, fresh);
	child_finish(s->pid, s->from, "ssh");
	s->from = -1;
	idset_free(&h.seen);
	free(h.v);
	buf_free(&req);
	buf_free(&common);
}

/*
 * The same rounds, stateless: each is a POST of the wants and that round's haves; the last POST carries the wants, the
 * common haves the last round found, and done.
 */
static void http_fetch(struct source *s, const unsigned char (*want)[20], size_t n, struct objlist *fresh)
{
	struct haves h;
	buf wants = { 0 }, req = { 0 }, url = { 0 }, common = { 0 };
	int band, detailed, from;
	pid_t pid;

	want_lines(&wants, s, want, n, &band, &detailed);
	buf_printf(&url, "%s/git-upload-pack", s->t.path);
	haves_init(&h);
	while (detailed) {
		size_t found;

		req.len = 0;
		buf_add(&req, wants.p, wants.len);
		if (!have_lines(&req, &h, HAVE_ROUND))
			break;
		buf_adds(&req, "0000");
		pid = http_start((char *)url.p, &req, RESULT_TYPE, &from);
		found = round_answer(from, &common);
		child_finish(pid, from, "curl");
		if (found)
			break;
	}
	req.len = 0;
	buf_add(&req, wants.p, wants.len);
	buf_add(&req, common.p, common.len);
	buf_adds(&req, "0009done\n");
	pid = http_start((char *)url.p, &req, RESULT_TYPE, &from);
	final_answer(from);
	recv_pack(from, band, fresh);
	child_finish(pid, from, "curl");
	idset_free(&h.seen);
	free(h.v);
	buf_free(&wants);
	buf_free(&req);
	buf_free(&url);
	buf_free(&common);
}

/* The objects an object names: a commit's tree and parents, a tag's target, a tree's entries but submodules. */
static void push_links(const buf *b, int type, struct objlist *todo)
{
	if (type == OBJ_COMMIT) {
		struct commit c;
		int k;

		commit_parse(b, &c);
		objlist_add(todo, c.tree, OBJ_TREE);
		for (k = 0; k < c.nparent; k++)
			objlist_add(todo, c.parent[k], OBJ_COMMIT);
		free(c.parent);
	} else if (type == OBJ_TAG) {
		unsigned char target[20];

		if (b->len < 47 || memcmp(b->p, "object ", 7) || hex_to_id((char *)b->p + 7, target))
			die("corrupt tag object");
		objlist_add(todo, target, 0);
	} else if (type == OBJ_TREE) {
		const unsigned char *p = b->p, *end = b->p + b->len;

		while (p < end) {
			const unsigned char *nul = memchr(p, 0, (size_t)(end - p));

			if (!nul || end - nul < 21)
				die("corrupt tree object");
			if (!strncmp((const char *)p, "40000 ", 6))
				objlist_add(todo, nul + 1, OBJ_TREE);
			else if (strncmp((const char *)p, "160000 ", 7))
				objlist_add(todo, nul + 1, OBJ_BLOB);
			p = nul + 21;
		}
	}
}

/* From a path: every object the wants reach that this repository lacks, read there and written here as one pack. */
static void local_fetch(struct source *s, const unsigned char (*want)[20], size_t n, struct objlist *fresh)
{
	struct objlist todo = { 0 }, objs = { 0 };
	struct idset seen = { 0 };
	char *here = git_dir;
	unsigned char sum[20];
	size_t i;
	int fd;

	for (i = 0; i < n; i++)
		objlist_add(&todo, want[i], 0);
	while (todo.n) {
		struct obj o = todo.v[--todo.n];
		buf b = { 0 };
		int type;

		if (!idset_add(&seen, o.id))
			continue;
		git_dir = here;
		if (object_exists(o.id))
			continue; /* here already, and so (as git assumes) is all it reaches */
		git_dir = s->dir;
		if (o.type == OBJ_BLOB) {
			objlist_add(&objs, o.id, OBJ_BLOB);
			continue;
		}
		object_read_or_die(o.id, &type, &b);
		if (o.type && type != o.type)
			die("corrupt repository %s: an object is a %s where a %s belongs", s->t.path, type_name(type), type_name(o.type));
		objlist_add(&objs, o.id, type);
		push_links(&b, type, &todo);
		buf_free(&b);
	}
	git_dir = here;
	if (objs.n) {
		tmp_pack_open(&fd);
		git_dir = s->dir;
		pack_send(fd, tmp_pack, &objs, NULL, NULL, sum);
		git_dir = here;
		tmp_pack_done(fd, fresh);
	}
	idset_free(&seen);
	free(todo.v);
	free(objs.v);
}

static void source_fetch(struct source *s, const unsigned char (*want)[20], size_t n, struct objlist *fresh)
{
	if (s->t.kind == URL_LOCAL) {
		local_fetch(s, want, n, fresh);
	} else if (s->t.kind == URL_HTTP) {
		if (n)
			http_fetch(s, want, n, fresh);
	} else if (n) {
		ssh_fetch(s, want, n, fresh);
	} else {
		write_all(s->to, "0000", 4, "the remote");
		close(s->to);
		child_finish(s->pid, s->from, "ssh");
	}
}

/*
 * Before any ref moves: every object the new tips reach is here. What came in the pack is read and followed; anything
 * else must have been here already (and so, as git assumes of a complete repository, everything it reaches).
 */
static void check_connected(const struct objlist *fresh, const unsigned char (*tips)[20], size_t n)
{
	struct idset in = { 0 }, seen = { 0 };
	struct objlist todo = { 0 };
	size_t i;

	for (i = 0; i < fresh->n; i++)
		idset_add(&in, fresh->v[i].id);
	for (i = 0; i < n; i++)
		objlist_add(&todo, tips[i], 0);
	while (todo.n) {
		struct obj o = todo.v[--todo.n];
		buf b = { 0 };
		char hex[41];
		int type;

		if (!idset_add(&seen, o.id))
			continue;
		if (!idset_has(&in, o.id)) {
			if (object_exists(o.id))
				continue;
			id_to_hex(o.id, hex);
			die("the pack that came lacks object %s, which the new refs need; no ref was changed", hex);
		}
		if (o.type == OBJ_BLOB)
			continue;
		object_read_or_die(o.id, &type, &b);
		if (o.type && type != o.type) {
			id_to_hex(o.id, hex);
			die("object %s is a %s where a %s belongs; no ref was changed", hex, type_name(type), type_name(o.type));
		}
		push_links(&b, type, &todo);
		buf_free(&b);
	}
	idset_free(&in);
	idset_free(&seen);
	free(todo.v);
}

/* ---- refs ---- */

struct spec {
	char *src, *dst;	/* for a glob, the parts before the '*' */
	int force, glob;
};

/* remote.NAME.fetch, each "[+]SRC/<star>:DST/<star>" or "[+]SRC:DST"; with none, the one git writes for NAME. */
static size_t specs_load(const char *remote, struct spec **out)
{
	size_t n, i;
	char **v = config_get_all("remote", remote, "fetch", &n);
	struct spec *s;

	if (!n) {
		buf d = { 0 };

		buf_printf(&d, "+refs/heads/*:refs/remotes/%s/*", remote);
		v = xmalloc(sizeof *v);
		v[0] = (char *)d.p;
		n = 1;
	}
	s = xmalloc(n * sizeof *s);
	for (i = 0; i < n; i++) {
		char *p = v[i] + (v[i][0] == '+'), *colon = strchr(p, ':');
		size_t ls, ld;

		s[i].force = v[i][0] == '+';
		if (!colon || colon == p || !colon[1])
			die("remote.%s.fetch '%s': mgit takes [+]<src>:<dst>", remote, v[i]);
		s[i].src = xstrndup(p, (size_t)(colon - p));
		s[i].dst = xstrdup(colon + 1);
		ls = strlen(s[i].src);
		ld = strlen(s[i].dst);
		s[i].glob = ls >= 2 && !strcmp(s[i].src + ls - 2, "/*");
		if (s[i].glob != (ld >= 2 && !strcmp(s[i].dst + ld - 2, "/*")) ||
		    strchr(s[i].src, '*') != (s[i].glob ? s[i].src + ls - 1 : NULL) ||
		    strchr(s[i].dst, '*') != (s[i].glob ? s[i].dst + ld - 1 : NULL))
			die("remote.%s.fetch '%s': mgit takes refs/<x>/*:refs/<y>/* or two full ref names", remote, v[i]);
		if (s[i].glob) {
			s[i].src[ls - 1] = 0;
			s[i].dst[ld - 1] = 0;
		}
		free(v[i]);
	}
	free(v);
	*out = s;
	return n;
}

/* The local ref s maps the remote's ref to, or NULL. */
static char *spec_map(const struct spec *s, const char *ref)
{
	size_t n = strlen(s->src);
	buf b = { 0 };

	if (s->glob ? strncmp(ref, s->src, n) || !ref[n] : strcmp(ref, s->src))
		return NULL;
	buf_adds(&b, s->dst);
	if (s->glob)
		buf_adds(&b, ref + n);
	if (!refname_ok((char *)b.p)) {
		buf_free(&b);
		return NULL;
	}
	return (char *)b.p;
}

/* The remote-tracking ref the remote's fetch lines give its ref (refs/heads/main -> refs/remotes/origin/main), or NULL. */
char *fetch_track(const char *remote, const char *ref)
{
	struct spec *s;
	size_t n = specs_load(remote, &s), i;
	char *r = NULL;

	for (i = 0; i < n && !r; i++)
		r = spec_map(&s[i], ref);
	return r;
}

static const char *short_ref(const char *ref)
{
	static const char *const pre[] = { "refs/heads/", "refs/tags/", "refs/remotes/" };
	size_t i;

	for (i = 0; i < sizeof pre / sizeof *pre; i++)
		if (!strncmp(ref, pre[i], strlen(pre[i])))
			return ref + strlen(pre[i]);
	return ref;
}

struct fupd {
	const struct remote_ref *r;
	char *dst;
	int force, tag;
};

/* Adds id to the list unless it is there already or this repository has the object. */
static void want_add(unsigned char (*want)[20], size_t *n, const unsigned char id[20])
{
	size_t k;

	if (object_exists(id))
		return;
	for (k = 0; k < *n; k++)
		if (!memcmp(want[k], id, 20))
			return;
	memcpy(want[(*n)++], id, 20);
}

/*
 * git fetch REMOTE, with its tags as --tags takes them: the branches through the remote's fetch lines (forced where a
 * line starts with +, else fast-forwards only), every tag this side lacks; a tag that differs here is left alone
 * (would clobber). Prints git's lines on stderr; returns 1 if any ref was refused.
 */
int fetch_remote(const char *name)
{
	struct source s;
	struct spec *sp;
	struct fupd *up;
	struct objlist fresh = { 0 };
	unsigned char (*want)[20], (*tips)[20];
	char *url = remote_url(name, 0);
	size_t nsp, i, k, nup = 0, nwant = 0;
	int rejected = 0;

	if (!url)
		die("no remote named %s (add one: mgit remote add %s <url>)", name, name);
	nsp = specs_load(name, &sp);
	source_open(&s, url);
	up = xmalloc((s.adv.n + 1) * sizeof *up);
	for (i = 0; i < s.adv.n; i++) {
		const struct remote_ref *r = &s.adv.v[i];
		size_t len = strlen(r->name);
		unsigned char cur[20];
		char *dst = NULL;
		int force = 0;

		if (len > 3 && !strcmp(r->name + len - 3, "^{}"))
			continue;
		for (k = 0; k < nsp && !dst; k++)
			if ((dst = spec_map(&sp[k], r->name)))
				force = sp[k].force;
		if (!dst && !strncmp(r->name, "refs/tags/", 10) && refname_ok(r->name))
			dst = xstrdup(r->name);
		if (!dst)
			continue;
		if (!ref_read(dst, cur) && !memcmp(cur, r->id, 20)) {
			free(dst);
			continue;
		}
		up[nup].r = r;
		up[nup].dst = dst;
		up[nup].force = force;
		up[nup++].tag = !strncmp(dst, "refs/tags/", 10);
	}
	want = xmalloc((nup + 1) * sizeof *want);
	tips = xmalloc((nup + 1) * sizeof *tips);
	for (i = 0; i < nup; i++) {
		memcpy(tips[i], up[i].r->id, 20);
		want_add(want, &nwant, up[i].r->id);
	}
	source_fetch(&s, (const unsigned char (*)[20])want, nwant, &fresh);
	check_connected(&fresh, (const unsigned char (*)[20])tips, nup);
	for (i = 0; i < nup; i++) {
		struct fupd *u = &up[i];
		unsigned char old[20];
		int have = !ref_read(u->dst, old);
		const char *flag = "*", *what, *why = NULL;
		char range[48], a[41], b[41];

		if (!i)
			fprintf(stderr, "From %s\n", url);
		if (!have) {
			what = u->tag ? "[new tag]" : !strncmp(u->r->name, "refs/heads/", 11) ? "[new branch]" : "[new ref]";
		} else if (u->tag) {
			flag = "!";
			what = "[rejected]";
			why = "would clobber existing tag";
		} else {
			id_to_hex(old, a);
			id_to_hex(u->r->id, b);
			what = range;
			if (descends(u->r->id, old)) {
				flag = " ";
				snprintf(range, sizeof range, "%.7s..%.7s", a, b);
			} else if (u->force) {
				flag = "+";
				snprintf(range, sizeof range, "%.7s...%.7s", a, b);
				why = "forced update";
			} else {
				flag = "!";
				what = "[rejected]";
				why = "non-fast-forward";
			}
		}
		if (*flag == '!')
			rejected = 1;
		else
			ref_update(u->dst, u->r->id, have ? old : NULL);
		fprintf(stderr, " %s %-17s %s -> %s%s%s%s\n", flag, what, short_ref(u->r->name), short_ref(u->dst),
			why ? "  (" : "", why ? why : "", why ? ")" : "");
	}
	free(want);
	free(tips);
	free(fresh.v);
	return rejected;
}

int cmd_fetch(int argc, char **argv)
{
	char *remote = NULL, *head;

	if (argc > 2 || (argc == 2 && argv[1][0] == '-'))
		die("usage: mgit fetch [<remote>]");
	repo_find();
	if (argc == 2) {
		remote = xstrdup(argv[1]);
	} else if ((head = head_symref())) {
		if (!strncmp(head, "refs/heads/", 11))
			remote = config_get_sub("branch", head + 11, "remote");
		free(head);
	}
	return fetch_remote(remote ? remote : "origin");
}

/* ---- notices: what a checkout leaves as git (set up as usual) would not ---- */

int is_lfs_pointer(const buf *b)
{
	static const char sig[] = "version https://git-lfs.github.com/spec/v1\n";

	return b->len < 1024 && b->len > sizeof sig && !memcmp(b->p, sig, sizeof sig - 1) && strstr((char *)b->p, "\noid sha256:");
}

/* Whether list ("a (p:1), b (p:2)") names the token already. */
static int token_listed(const buf *list, const char *tok, size_t n)
{
	const char *p = list->len ? (const char *)list->p : "";

	for (; *p; p++)
		if ((p == (const char *)list->p || (p[-1] == ' ' && p[-2] == ',')) && !strncmp(p, tok, n) && !strncmp(p + n, " (", 2))
			return 1;
	return 0;
}

/*
 * The attributes in a .gitattributes that change what a checkout writes: filter (smudge), eol, ident and
 * working-tree-encoding. Each distinct one is added to list with where it first appears; returns how many were set.
 */
static size_t attr_scan(const buf *b, const char *path, buf *list)
{
	static const char *const names[] = { "filter", "eol", "ident", "working-tree-encoding" };
	const char *p = (const char *)b->p, *end = p + b->len;
	size_t hits = 0, line = 0, i;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p)), *q;

		if (!nl)
			nl = end;
		line++;
		for (q = p; q < nl && (*q == ' ' || *q == '\t'); q++)
			;
		if (q < nl && *q != '#') {
			q += strcspn(q, " \t\n"); /* the pattern */
			while (q < nl) {
				size_t tl;

				while (q < nl && (*q == ' ' || *q == '\t' || *q == '\r'))
					q++;
				tl = strcspn(q, " \t\r\n");
				if (q + tl > nl)
					tl = (size_t)(nl - q);
				for (i = 0; tl && *q != '-' && *q != '!' && i < sizeof names / sizeof *names; i++) {
					size_t n = strlen(names[i]);

					if (tl >= n && !strncmp(q, names[i], n) && (tl == n || q[n] == '=')) {
						if (!token_listed(list, q, tl))
							buf_printf(list, "%s%.*s (%s:%zu)", list->len ? ", " : "", (int)tl, q, path, line);
						hits++;
						break;
					}
				}
				q += tl;
			}
		}
		p = nl + 1;
	}
	return hits;
}

static void notice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void notice(const char *fmt, ...)
{
	va_list ap;

	fflush(stdout);
	fputs("mgit: notice: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* For the entries of to that from does not hold as they are: submodules, LFS pointers and attributes, each said once. */
void checkout_notices(const struct files *from, const struct files *to)
{
	const char *sub = NULL, *lfs = NULL;
	size_t i, nsub = 0, nlfs = 0, nattr = 0;
	buf attr = { 0 };

	for (i = 0; i < to->n; i++) {
		const struct file *t = &to->v[i], *f = files_find(from, t->path);
		const char *base = strrchr(t->path, '/');
		buf b = { 0 };
		int type;

		if (f && f->mode == t->mode && !memcmp(f->id, t->id, 20))
			continue;
		base = base ? base + 1 : t->path;
		if (t->mode == 0160000) {
			if (!nsub++)
				sub = t->path;
		} else if (t->mode == 0120000) {
			continue;
		} else if (!strcmp(base, ".gitattributes")) {
			object_read_or_die(t->id, &type, &b);
			nattr += attr_scan(&b, t->path, &attr);
		} else if (!t->have_stat || t->sd.size < 1024) {
			object_read_or_die(t->id, &type, &b);
			if (is_lfs_pointer(&b) && !nlfs++)
				lfs = t->path;
		}
		buf_free(&b);
	}
	if (nsub)
		notice("%zu submodule%s not fetched (mgit does not fetch submodules; each is an empty directory): %s%s", nsub,
		       nsub == 1 ? "" : "s", sub, nsub > 1 ? " ..." : "");
	if (nlfs)
		notice("%zu file%s Git LFS pointer%s, left as %s (mgit does not fetch LFS content): %s%s", nlfs,
		       nlfs == 1 ? " is a" : "s are", nlfs == 1 ? "" : "s", nlfs == 1 ? "it is" : "they are", lfs, nlfs > 1 ? " ..." : "");
	if (nattr)
		notice(".gitattributes asks for %s; mgit checks files out as stored, with no filter, eol or encoding change",
		       (char *)attr.p);
	buf_free(&attr);
}

/* ---- clone ---- */

static char *clone_dir;		/* what a clone is writing into, emptied at exit unless it finished */
static int clone_made, clone_done;

/* Removes path and all under it (keep_top: only what is under it), never following a symlink. */
static void remove_tree(const char *path, int keep_top)
{
	struct stat st;
	struct dirent *de;
	DIR *d;

	if (lstat(path, &st))
		return;
	if (S_ISDIR(st.st_mode) && (d = opendir(path))) {
		while ((de = readdir(d))) {
			char *sub;

			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			sub = path_join(path, de->d_name);
			remove_tree(sub, 0);
			free(sub);
		}
		closedir(d);
		if (!keep_top)
			rmdir(path);
	} else if (!keep_top) {
		unlink(path);
	}
}

static void clone_cleanup(void)
{
	if (clone_dir && !clone_done) {
		pack_unmap_all();	/* first: on NFS a removed pack still mapped stays as .nfsXXXX, and its directories with it */
		remove_tree(clone_dir, !clone_made);
	}
}

/* git's guess: the last part of the url, without a trailing /.git or .git. */
static char *guess_dir(const char *url)
{
	size_t n = strlen(url), start;

	while (n && url[n - 1] == '/')
		n--;
	if (n >= 5 && !strncmp(url + n - 5, "/.git", 5)) {
		n -= 5;
		while (n && url[n - 1] == '/')
			n--;
	} else if (n >= 4 && !strncmp(url + n - 4, ".git", 4)) {
		n -= 4;
	}
	for (start = n; start && url[start - 1] != '/' && url[start - 1] != ':'; start--)
		;
	if (start == n)
		die("cannot tell a directory name from %s; name one: mgit clone <url> <dir>", url);
	return xstrndup(url + start, n - start);
}

static int dir_empty(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	int empty = d != NULL;

	while (empty && (de = readdir(d)))
		empty = !strcmp(de->d_name, ".") || !strcmp(de->d_name, "..");
	if (d)
		closedir(d);
	return empty;
}

static void make_dirs(const char *dir)
{
	char *p = xstrdup(dir), *s;

	for (s = p + 1; (s = strchr(s, '/')); s++) {
		*s = 0;
		if (mkdir(p, 0777) && errno != EEXIST)
			die("cannot create %s: %s", p, strerror(errno));
		*s = '/';
	}
	free(p);
}

static void clone_checkout(const unsigned char commit[20])
{
	struct repo_state st;
	struct files none = { 0 }, to = { 0 };
	unsigned char id[20], tree[20];

	memset(&st, 0, sizeof st);
	memcpy(id, commit, 20);
	peel_to_commit(id);
	commit_tree_id(id, tree);
	tree_flatten(tree, &to);
	worktree_switch(&none, &to, &st);
	checkout_notices(&none, &to);
	files_free(&to);
}

/* The branch HEAD names (symref=HEAD:...), or, from a server that does not say, one at HEAD's commit (main, master first). */
static const struct remote_ref *default_branch(const struct advert *a)
{
	static const char *const pref[] = { "refs/heads/main", "refs/heads/master" };
	const struct remote_ref *head = advert_find(a, "HEAD"), *r;
	size_t i;

	if (a->head)
		return strncmp(a->head, "refs/heads/", 11) ? NULL : advert_find(a, a->head);
	if (!head)
		return NULL;
	for (i = 0; i < 2; i++)
		if ((r = advert_find(a, pref[i])) && !memcmp(r->id, head->id, 20))
			return r;
	for (i = 0; i < a->n; i++)
		if (!strncmp(a->v[i].name, "refs/heads/", 11) && !memcmp(a->v[i].id, head->id, 20))
			return &a->v[i];
	return NULL;
}

int cmd_clone(int argc, char **argv)
{
	struct source s;
	struct objlist fresh = { 0 };
	const struct remote_ref *branch, *head;
	unsigned char (*want)[20];
	char *dir, abs[PATH_MAX];
	const char *url = argc > 1 ? argv[1] : "";
	size_t i, nwant = 0;
	struct stat st;
	buf b = { 0 };

	if (argc < 2 || argc > 3 || argv[1][0] == '-' || (argc == 3 && argv[2][0] == '-'))
		die("usage: mgit clone <url> [<dir>]");
	dir = argc == 3 ? xstrdup(argv[2]) : guess_dir(url);
	if (!stat(dir, &st) && !(S_ISDIR(st.st_mode) && dir_empty(dir)))
		die("destination path '%s' already exists and is not an empty directory", dir);
	clone_made = stat(dir, &st) != 0;
	source_open(&s, url);
	if (s.t.kind == URL_LOCAL) {
		if (!realpath(s.t.path, abs))
			die("cannot resolve %s: %s", s.t.path, strerror(errno));
		url = xstrdup(abs);
	}
	fprintf(stderr, "Cloning into '%s'...\n", dir);
	make_dirs(dir);
	clone_dir = dir;
	atexit(clone_cleanup);
	git_dir = repo_init(dir);
	if (!realpath(dir, abs))
		die("cannot resolve %s: %s", dir, strerror(errno));
	work_tree = clone_dir = xstrdup(abs);
	buf_adds(&b, "[remote \"origin\"]\n\turl = ");
	config_value(&b, url);
	buf_adds(&b, "\n\tfetch = +refs/heads/*:refs/remotes/origin/*\n");
	config_append((char *)b.p);

	want = xmalloc((s.adv.n + 1) * sizeof *want);
	for (i = 0; i < s.adv.n; i++) {
		const char *n = s.adv.v[i].name;
		size_t len = strlen(n);

		if ((!strncmp(n, "refs/heads/", 11) || !strncmp(n, "refs/tags/", 10)) && strcmp(n + len - 3, "^{}") &&
		    refname_ok(n))
			want_add(want, &nwant, s.adv.v[i].id);
	}
	source_fetch(&s, (const unsigned char (*)[20])want, nwant, &fresh);
	check_connected(&fresh, (const unsigned char (*)[20])want, nwant);

	for (i = 0; i < s.adv.n; i++) {
		const char *n = s.adv.v[i].name;
		size_t len = strlen(n);

		if (!refname_ok(n) || (len > 3 && !strcmp(n + len - 3, "^{}")))
			continue;
		b.len = 0;
		if (!strncmp(n, "refs/heads/", 11))
			buf_printf(&b, "refs/remotes/origin/%s", n + 11);
		else if (!strncmp(n, "refs/tags/", 10))
			buf_adds(&b, n);
		else
			continue;
		if (refname_ok((char *)b.p))
			ref_update((char *)b.p, s.adv.v[i].id, NULL);
	}
	branch = default_branch(&s.adv);
	head = advert_find(&s.adv, "HEAD");
	if (branch) {
		const char *name = branch->name + 11;

		b.len = 0;
		buf_printf(&b, "refs/remotes/origin/%s", name);
		symref_set("refs/remotes/origin/HEAD", (char *)b.p);
		ref_update(branch->name, branch->id, NULL);
		head_set_symref(branch->name);
		b.len = 0;
		buf_adds(&b, "[branch \"");
		buf_adds(&b, name);
		buf_adds(&b, "\"]\n\tremote = origin\n\tmerge = ");
		config_value(&b, branch->name);
		buf_adds(&b, "\n");
		config_append((char *)b.p);
		clone_checkout(branch->id);
	} else if (head && !s.adv.head) {
		ref_update("HEAD", head->id, NULL);
		warning("the remote's HEAD is no branch; HEAD is detached at its commit");
		clone_checkout(head->id);
	} else {
		if (s.adv.head && strcmp(s.adv.head, "refs/heads/main") && refname_ok(s.adv.head))
			head_set_symref(s.adv.head);
		if (!s.adv.n)
			warning("you appear to have cloned an empty repository");
		else
			warning("the remote's HEAD names %s, which it does not have; nothing is checked out",
				s.adv.head ? s.adv.head : "nothing");
	}
	clone_done = 1;
	free(want);
	free(fresh.v);
	buf_free(&b);
	return 0;
}
