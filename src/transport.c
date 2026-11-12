#define _DEFAULT_SOURCE
#include "mgit.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * What push and fetch share: pkt-lines, the url forms git takes, the refs a far side advertises, and the two ways to
 * reach one, ssh (git-receive-pack or git-upload-pack) and smart HTTP through curl (git-upload-pack only). Both are
 * subprocesses, so mgit itself speaks neither TCP nor TLS.
 */

void read_exact(int fd, void *d, size_t n)
{
	unsigned char *p = d;

	while (n) {
		ssize_t r = read(fd, p, n);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			die("the remote end hung up%s", r < 0 ? " (read error)" : "");
		p += r;
		n -= (size_t)r;
	}
}

/* One pkt-line's payload into data, as sent; returns 0 for a flush packet. */
int pkt_read_raw(int fd, buf *data)
{
	char h[5];
	unsigned long n;
	char *e;

	read_exact(fd, h, 4);
	h[4] = 0;
	n = strtoul(h, &e, 16);
	if (*e || !is_hex(h, 4) || (n && n < 4) || n > 65520)
		die("bad pkt-line length from the remote: '%s'", h);
	data->len = 0;
	if (!n)
		return 0;
	buf_grow(data, n - 4);
	read_exact(fd, data->p, n - 4);
	data->len = n - 4;
	data->p[data->len] = 0;
	return 1;
}

/* One pkt-line into line (its trailing newline dropped); returns 0 for a flush packet. */
int pkt_read(int fd, buf *line)
{
	if (!pkt_read_raw(fd, line))
		return 0;
	if (line->len && line->p[line->len - 1] == '\n')
		line->len--;
	line->p[line->len] = 0;
	if (line->len >= 4 && !memcmp(line->p, "ERR ", 4))
		die("the remote refused: %s", (char *)line->p + 4);
	return 1;
}

void pkt_add(buf *out, const char *d, size_t n)
{
	buf_printf(out, "%04zx", n + 4);
	buf_add(out, d, n);
}

/*
 * git's rules: scheme://... ; [user@]host:path when a colon comes before any slash; otherwise a local path. verb is
 * "push" or "fetch": push takes ssh and paths, fetch https too. https is anonymous only, so a url with a user name in
 * it is refused.
 */
int parse_url(const char *url, struct target *t, const char *verb)
{
	const char *p = strstr(url, "://"), *c;
	int push = !strcmp(verb, "push");

	memset(t, 0, sizeof *t);
	if (p) {
		size_t sl = (size_t)(p - url);
		const char *h = p + 3, *slash = strchr(h, '/'), *at, *colon;

		if (sl == 4 && !strncmp(url, "file", 4)) {
			t->path = xstrdup(p + 3);
			return t->kind = URL_LOCAL;
		}
		if (!push && ((sl == 5 && !strncmp(url, "https", 5)) || (sl == 4 && !strncmp(url, "http", 4)))) {
			size_t n = strlen(url);

			if (!slash || !slash[1])
				die("no path in %s", url);
			if (memchr(h, '@', (size_t)(slash - h)))
				die("mgit fetches over https anonymously only: take any user name (and password) out of the url");
			while (n && url[n - 1] == '/')
				n--;
			t->host = xstrndup(h, (size_t)(slash - h));
			t->path = xstrndup(url, n);
			return t->kind = URL_HTTP;
		}
		if (!(sl == 3 && !strncmp(url, "ssh", 3)) && !(sl == 7 && !strncmp(url, "git+ssh", 7)) &&
		    !(sl == 7 && !strncmp(url, "ssh+git", 7))) {
			if (push)
				die("mgit pushes over ssh or to a path on this machine; not %.*s://", (int)sl, url);
			die("mgit fetches over ssh, https or from a path on this machine; not %.*s://", (int)sl, url);
		}
		if (!slash)
			die("no path in %s", url);
		at = memchr(h, '@', (size_t)(slash - h));
		colon = memchr(at ? at : h, ':', (size_t)(slash - (at ? at : h)));
		t->host = xstrndup(h, (size_t)((colon ? colon : slash) - h));
		if (colon)
			t->port = xstrndup(colon + 1, (size_t)(slash - colon - 1));
		t->path = xstrdup(slash[1] == '~' ? slash + 1 : slash);
		if (t->port && (!*t->port || strspn(t->port, "0123456789") != strlen(t->port)))
			die("bad port in %s", url);
	} else if ((c = strchr(url, ':')) && !memchr(url, '/', (size_t)(c - url)) && c > url) {
		t->host = xstrndup(url, (size_t)(c - url));
		t->path = xstrdup(c + 1);
	} else {
		t->path = xstrdup(url);
		return t->kind = URL_LOCAL;
	}
	if (!*t->host || t->host[0] == '-' || !*t->path)
		die("bad ssh url %s", url);
	return t->kind = URL_SSH;
}

int has_cap(const char *caps, const char *name)
{
	size_t n = strlen(name);
	const char *p;

	for (p = caps; (p = strstr(p, name)); p += n)
		if ((p == caps || p[-1] == ' ') && (!p[n] || p[n] == ' ' || p[n] == '='))
			return 1;
	return 0;
}

void advert_add(struct advert *a, const char *name, const unsigned char id[20])
{
	a->v = xrealloc(a->v, (a->n + 1) * sizeof *a->v);
	a->v[a->n].name = xstrdup(name);
	memcpy(a->v[a->n++].id, id, 20);
}

const struct remote_ref *advert_find(const struct advert *a, const char *name)
{
	size_t i;

	for (i = 0; i < a->n; i++)
		if (!strcmp(a->v[i].name, name))
			return &a->v[i];
	return NULL;
}

/* The value of the capability name=VALUE in caps whose value starts with prefix (symref=HEAD:...), or NULL. */
static char *cap_value(const char *caps, const char *name, const char *prefix)
{
	size_t n = strlen(name), pn = strlen(prefix);
	const char *p;

	for (p = caps; (p = strstr(p, name)); p += n)
		if ((p == caps || p[-1] == ' ') && p[n] == '=' && !strncmp(p + n + 1, prefix, pn))
			return xstrndup(p + n + 1 + pn, strcspn(p + n + 1 + pn, " "));
	return NULL;
}

/*
 * The refs a far side advertises (protocol v0): "<id> <name>", capabilities after a NUL on the first line, a flush at
 * the end. A repository with SHA-256 object names is refused here, before its 64-digit ids are read. An empty
 * repository sends one line of zeros named capabilities^{}, which is not a ref.
 */
void advert_read(int fd, struct advert *a)
{
	static const unsigned char zero[20];
	buf line = { 0 };
	int first = 1;

	memset(a, 0, sizeof *a);
	a->caps = xstrdup("");
	while (pkt_read(fd, &line)) {
		char *sp, *nul = memchr(line.p, 0, line.len), *fmt;
		unsigned char id[20];

		if (first && line.len >= 9 && !memcmp(line.p, "version 1", 9))
			continue;
		if (nul) {
			free(a->caps);
			a->caps = xstrdup(nul + 1);
			if ((fmt = cap_value(a->caps, "object-format", "")) && strcmp(fmt, "sha1"))
				die("the remote repository uses %s object names; mgit works with SHA-1 repositories only", fmt);
			free(fmt);
			if (!a->head)
				a->head = cap_value(a->caps, "symref", "HEAD:");
		}
		first = 0;
		if (line.len >= 8 && !memcmp(line.p, "shallow ", 8))
			continue;
		sp = memchr(line.p, ' ', line.len);
		if (!sp || sp - (char *)line.p != 40 || hex_to_id((char *)line.p, id))
			die("bad ref line from the remote: %.60s", (char *)line.p);
		if (!memcmp(id, zero, 20))
			continue;
		advert_add(a, sp + 1, id);
	}
	buf_free(&line);
}

/* git-receive-pack 'PATH' (or git-upload-pack), quoted for the far side's shell as git's sq_quote does. */
static char *remote_command(const char *service, const char *path)
{
	buf b = { 0 };

	buf_printf(&b, "%s '", service);
	for (; *path; path++) {
		if (*path == '\'' || *path == '!')
			buf_printf(&b, "'\\%c'", *path);
		else
			buf_addc(&b, *path);
	}
	buf_addc(&b, '\'');
	return (char *)b.p;
}

pid_t ssh_start(const struct target *t, const char *service, int *to, int *from)
{
	int in[2], out[2];
	char *cmd = remote_command(service, t->path);
	const char *args[6];
	int n = 0;
	pid_t pid;

	args[n++] = "ssh";
	if (t->port) {
		args[n++] = "-p";
		args[n++] = t->port;
	}
	args[n++] = t->host;
	args[n++] = cmd;
	args[n] = NULL;
	if (pipe(in) || pipe(out))
		die("cannot make a pipe: %s", strerror(errno));
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		die("cannot fork: %s", strerror(errno));
	if (!pid) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		execvp("ssh", (char *const *)args);
		fprintf(stderr, "mgit: cannot run ssh: %s\n", strerror(errno));
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	*to = in[1];
	*from = out[0];
	free(cmd);
	return pid;
}

/* Reads what is left on fd, then waits for pid; dies unless it exited 0. */
void child_finish(pid_t pid, int fd, const char *what)
{
	char sink[4096];
	int status;

	while (fd >= 0) {
		ssize_t r = read(fd, sink, sizeof sink);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
	}
	if (fd >= 0)
		close(fd);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
		die("%s ended badly (%s %d)", what, WIFEXITED(status) ? "exit" : "signal",
		    WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
}

/* One header line, CR LF or LF ended, into line; 0 at the end of the stream. */
static int header_line(int fd, buf *line)
{
	char c;

	line->len = 0;
	for (;;) {
		ssize_t r = read(fd, &c, 1);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return line->len > 0;
		if (c == '\n')
			break;
		buf_addc(line, c);
	}
	if (line->len && line->p[line->len - 1] == '\r')
		line->len--;
	buf_add(line, "", 0);
	return 1;
}

/* The header block of one response: its status (0 at the end of the stream), and the value of each header asked for. */
static int header_block(int fd, char **ctype, char **location)
{
	buf line = { 0 };
	int status = 0;

	if (!header_line(fd, &line)) {
		buf_free(&line);
		return 0;
	}
	if (strncmp((char *)line.p, "HTTP/", 5) || !strchr((char *)line.p, ' '))
		die("curl gave no HTTP status line: %.60s", (char *)line.p);
	status = atoi(strchr((char *)line.p, ' ') + 1);
	while (header_line(fd, &line) && line.len) {
		char *v = strchr((char *)line.p, ':'), **slot = NULL;

		if (!v)
			continue;
		*v++ = 0;
		if (!strcasecmp((char *)line.p, "content-type"))
			slot = ctype;
		else if (!strcasecmp((char *)line.p, "location"))
			slot = location;
		if (!slot)
			continue;
		while (*v == ' ' || *v == '\t')
			v++;
		free(*slot);
		*slot = xstrndup(v, strcspn(v, slot == ctype ? "; \t" : " \t"));
	}
	buf_free(&line);
	return status;
}

/*
 * One smart-HTTP request through curl: a GET of url, or a POST of body. curl runs with -q (no .curlrc), sends no
 * credentials and follows no redirect; its headers come first on its output (--dump-header -). Returns curl's pid
 * with *from just past the headers, once the status is 200 and the content type is type; dies otherwise.
 */
pid_t http_start(const char *url, const buf *body, const char *type, int *from)
{
	const char *args[24];
	char *ctype = NULL, *location = NULL;
	int out[2], in[2] = { -1, -1 }, n = 0, status;
	pid_t pid;

	args[n++] = "curl";
	args[n++] = "-q";
	args[n++] = "--silent";
	args[n++] = "--show-error";
	args[n++] = "--proto";
	args[n++] = "=http,https";
	args[n++] = "--dump-header";
	args[n++] = "-";
	args[n++] = "--user-agent";
	args[n++] = "mgit/" MGIT_VERSION;
	if (body) {
		args[n++] = "--data-binary";
		args[n++] = "@-";
		args[n++] = "--header";
		args[n++] = "Content-Type: application/x-git-upload-pack-request";
		args[n++] = "--header";
		args[n++] = "Accept: application/x-git-upload-pack-result";
		args[n++] = "--header";
		args[n++] = "Expect:";
	}
	args[n++] = url;
	args[n] = NULL;
	if (pipe(out) || (body && pipe(in)))
		die("cannot make a pipe: %s", strerror(errno));
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		die("cannot fork: %s", strerror(errno));
	if (!pid) {
		int nul = body ? in[0] : open("/dev/null", O_RDONLY);

		dup2(nul, 0);
		dup2(out[1], 1);
		close(out[0]);
		close(out[1]);
		if (body)
			close(in[1]);
		close(nul);
		execvp("curl", (char *const *)args);
		fprintf(stderr, "mgit: cannot run curl: %s\n", strerror(errno));
		_exit(127);
	}
	close(out[1]);
	if (body) {
		close(in[0]);
		write_all(in[1], body->p, body->len, "curl");
		close(in[1]);
	}
	while ((status = header_block(out[0], &ctype, &location)) >= 100 && status < 200)
		;
	if (status != 200) {
		child_finish(pid, out[0], "curl");
		if (!status)
			die("no answer from %s", url);
		if (status == 401 || status == 403)
			die("%s answered HTTP %d: it asks for credentials, and mgit fetches anonymously only "
			    "(a private or missing repository)", url, status);
		if (status >= 300 && status < 400)
			die("%s answered HTTP %d: it moved to %s; clone that url instead", url, status, location ? location : "?");
		die("%s answered HTTP %d", url, status);
	}
	if (!ctype || strcmp(ctype, type))
		die("%s is not a smart git http server (content type %s, not %s)", url, ctype ? ctype : "none", type);
	free(ctype);
	free(location);
	*from = out[0];
	return pid;
}
