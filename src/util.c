#define _DEFAULT_SOURCE
#include "mgit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Whether c is shown as ^X, as git shows a control character a far side sends (newline, CR and tab are not). */
static int shown_masked(unsigned char c)
{
	return (c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f;
}

/* One byte to stderr, a control character as ^X. */
void remote_putc(unsigned char c)
{
	if (shown_masked(c)) {
		fputc('^', stderr);
		fputc(c ^ 0x40, stderr);
	} else {
		fputc(c, stderr);
	}
}

/* s appended to b, each control character as ^X: for text from elsewhere printed on stdout. */
void buf_add_masked(buf *b, const char *s)
{
	buf_adds(b, "");
	for (; *s; s++) {
		if (shown_masked((unsigned char)*s)) {
			buf_addc(b, '^');
			buf_addc(b, *s ^ 0x40);
		} else {
			buf_addc(b, *s);
		}
	}
}

/* A message on stderr after pre; what it quotes from a far side may hold any byte, so control characters are shown. */
static void say(const char *pre, const char *fmt, va_list ap)
{
	va_list aq;
	char *s;
	int n, i;

	fflush(stdout);
	fputs(pre, stderr);
	va_copy(aq, ap);
	n = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (n >= 0 && (s = malloc((size_t)n + 1))) {
		vsnprintf(s, (size_t)n + 1, fmt, ap);
		for (i = 0; i < n; i++)
			remote_putc((unsigned char)s[i]);
		free(s);
	} else {
		vfprintf(stderr, fmt, ap);
	}
	fputc('\n', stderr);
}

void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	say("mgit: ", fmt, ap);
	va_end(ap);
	exit(128);
}

void warning(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	say("mgit: warning: ", fmt, ap);
	va_end(ap);
}

void notice(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	say("mgit: notice: ", fmt, ap);
	va_end(ap);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		die("out of memory");
	return p;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);

	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

char *path_join(const char *a, const char *b)
{
	size_t na = strlen(a), nb = strlen(b);
	char *p = xmalloc(na + nb + 2);

	memcpy(p, a, na);
	p[na] = '/';
	memcpy(p + na + 1, b, nb + 1);
	return p;
}

void buf_grow(buf *b, size_t extra)
{
	size_t need, cap;

	if (extra > SIZE_MAX - b->len - 1)
		die("out of memory");
	need = b->len + extra + 1;
	if (need <= b->cap)
		return;
	cap = b->cap ? b->cap : 64;
	while (cap < need)
		cap = cap > SIZE_MAX / 2 ? need : cap * 2;
	b->p = xrealloc(b->p, cap);
	b->cap = cap;
}

void buf_add(buf *b, const void *d, size_t n)
{
	buf_grow(b, n);
	if (n)
		memcpy(b->p + b->len, d, n);
	b->len += n;
	b->p[b->len] = 0;
}

void buf_addc(buf *b, int c)
{
	unsigned char ch = (unsigned char)c;

	buf_add(b, &ch, 1);
}

void buf_adds(buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

void buf_printf(buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		die("formatting failed");
	buf_grow(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf((char *)b->p + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

static int must_quote(unsigned char c)
{
	return c < 0x20 || c >= 0x7f || c == '"' || c == '\\';
}

/* prefix + path, in git's C-style quotes when path has control, non-ASCII, '"' or '\' bytes (core.quotePath). */
void buf_quote(buf *b, const char *prefix, const char *path)
{
	static const char esc[] = "\a\b\t\n\v\f\r\"\\", rep[] = "abtnvfr\"\\";
	const char *p, *q;

	for (p = path; *p && !must_quote((unsigned char)*p); p++)
		;
	if (!*p) {
		buf_adds(b, prefix);
		buf_adds(b, path);
		return;
	}
	buf_addc(b, '"');
	buf_adds(b, prefix);
	for (p = path; *p; p++) {
		unsigned char c = (unsigned char)*p;

		if ((q = strchr(esc, c)))
			buf_printf(b, "\\%c", rep[q - esc]);
		else if (must_quote(c))
			buf_printf(b, "\\%03o", c);
		else
			buf_addc(b, c);
	}
	buf_addc(b, '"');
}

void buf_free(buf *b)
{
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

int read_fd(int fd, buf *out)
{
	for (;;) {
		ssize_t r;

		buf_grow(out, 65536);
		r = read(fd, out->p + out->len, out->cap - out->len - 1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			break;
		out->len += (size_t)r;
	}
	out->p[out->len] = 0;
	return 0;
}

int read_file(const char *path, buf *out)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), r, e;

	if (fd < 0)
		return -1;
	r = read_fd(fd, out);
	e = errno;
	close(fd);
	errno = e;
	return r;
}

int read_link(const char *path, buf *out)
{
	size_t cap = 256;

	for (;;) {
		ssize_t r;

		buf_grow(out, cap);
		r = readlink(path, (char *)out->p + out->len, cap);
		if (r < 0)
			return -1;
		if ((size_t)r < cap) {
			out->len += (size_t)r;
			out->p[out->len] = 0;
			return 0;
		}
		cap *= 2;
	}
}

void write_all(int fd, const void *d, size_t n, const char *what)
{
	const unsigned char *p = d;

	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			die("cannot write %s: %s", what, strerror(errno));
		}
		p += w;
		n -= (size_t)w;
	}
}

static const char hexdigits[] = "0123456789abcdef";

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

void id_to_hex(const unsigned char id[20], char hex[41])
{
	int i;

	for (i = 0; i < 20; i++) {
		hex[2 * i] = hexdigits[id[i] >> 4];
		hex[2 * i + 1] = hexdigits[id[i] & 15];
	}
	hex[40] = 0;
}

int hex_to_id(const char *hex, unsigned char id[20])
{
	int i, hi, lo;

	for (i = 0; i < 20; i++) {
		if ((hi = hexval(hex[2 * i])) < 0 || (lo = hexval(hex[2 * i + 1])) < 0)
			return -1;
		id[i] = (unsigned char)(hi << 4 | lo);
	}
	return 0;
}

int is_hex(const char *s, size_t n)
{
	while (n--)
		if (hexval(*s++) < 0)
			return 0;
	return 1;
}
