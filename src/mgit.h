#ifndef MGIT_H
#define MGIT_H

#define MGIT_VERSION "6.0.0"

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

enum { OBJ_COMMIT = 1, OBJ_TREE = 2, OBJ_BLOB = 3, OBJ_TAG = 4 };

/* Growable byte buffer; zero-initialise before use. Kept NUL-terminated once allocated. */
typedef struct {
	unsigned char *p;
	size_t len, cap;
} buf;

struct commit {
	unsigned char tree[20];
	unsigned char (*parent)[20];
	int nparent;
	const char *author, *msg;
	size_t author_len, msg_len;
};

/* The stat fields git keeps in its index, truncated to 32 bits as git does. */
struct stat_data {
	uint32_t ctime_s, ctime_ns, mtime_s, mtime_ns, dev, ino, uid, gid, size;
};

/* A tracked path: a tree entry, an index entry or a working-tree file. */
struct file {
	char *path;
	unsigned mode;
	unsigned char id[20];
	int have_stat;
	struct stat_data sd;
};

/* Sorted by path (byte order), which is also git's tree and index order. */
struct files {
	struct file *v;
	size_t n, cap;
};

struct pair {
	const struct file *a, *b;
};

/* Paths named after "--", relative to the top of the working tree ("" is all of it); n == 0 means everything. */
struct pathspec {
	char **v;
	size_t n;
};

struct repo_state {
	char *head_ref;
	int have_head;
	unsigned char head[20];
	struct files head_files;
	struct files index;
	int have_index;
	uint32_t stamp[2];
};

/* util.c */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warning(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *path_join(const char *a, const char *b);
void buf_grow(buf *b, size_t extra);
void buf_add(buf *b, const void *d, size_t n);
void buf_addc(buf *b, int c);
void buf_adds(buf *b, const char *s);
void buf_printf(buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_quote(buf *b, const char *prefix, const char *path);
void buf_free(buf *b);
int read_fd(int fd, buf *out);
int read_file(const char *path, buf *out);
int read_link(const char *path, buf *out);
void write_all(int fd, const void *d, size_t n, const char *what);
void id_to_hex(const unsigned char id[20], char hex[41]);
int hex_to_id(const char *hex, unsigned char id[20]);
int is_hex(const char *s, size_t n);

/* sha1.c */
typedef struct {
	uint32_t h[5];
	uint64_t len;
	unsigned char blk[64];
} sha1_ctx;
void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *d, size_t n);
void sha1_final(sha1_ctx *c, unsigned char out[20]);

/* zlib.c */
void zlib_store(buf *out, const unsigned char *in, size_t n);
void zlib_compress(buf *out, const unsigned char *in, size_t n);
int zlib_inflate(buf *out, const unsigned char *in, size_t n);
int zlib_inflate_prefix(buf *out, const unsigned char *in, size_t n, size_t *used);

/* pack.c */
struct objlist;
int pack_verify(const char *idx_path, const struct objlist *want);
void pack_index(const char *tmp, struct objlist *out);
int pack_read(const unsigned char id[20], int *type, buf *out);
int pack_has(const unsigned char id[20]);
unsigned long pack_object_count(void);
void pack_prefix(const char *hex, size_t n, void (*found)(const unsigned char id[20], void *ctx), void *ctx);

/* object.c */
const char *type_name(int type);
int type_from_name(const char *s, size_t n);
void object_hash(int type, const void *d, size_t n, unsigned char id[20]);
void object_write(int type, const void *d, size_t n, unsigned char id[20]);
int object_read(const unsigned char id[20], int *type, buf *out);
void object_read_or_die(const unsigned char id[20], int *type, buf *out);
int object_exists(const unsigned char id[20]);
int object_by_prefix(const char *hex, size_t n, unsigned char id[20]);

/* main.c */
char *repo_init(const char *dir);

/* repo.c */
extern char *git_dir, *work_tree;
void repo_find(void);
char *config_get(const char *section, const char *key);
char *config_get_sub(const char *section, const char *sub, const char *key);
char **config_subsections(const char *section, size_t *count);
void config_append(const char *text);
int refname_ok(const char *name);
int ref_read(const char *name, unsigned char id[20]);
char *head_symref(void);
void head_set_symref(const char *ref);
void symref_set(const char *name, const char *target);
char **config_get_all(const char *section, const char *sub, const char *key, size_t *count);
void ref_update(const char *name, const unsigned char new_id[20], const unsigned char *old_id);
void reflog_append(const char *name, const unsigned char *old_id, const unsigned char new_id[20], const char *ident,
		   size_t ident_len, const char *msg);
void peel_to_commit(unsigned char id[20]);
int rev_parse(const char *spec, unsigned char id[20]);
char **ref_list(const char *prefix, size_t *count);
char **branch_list(size_t *count);
int checked_out_elsewhere(const char *name);

/* index.c */
struct file *files_add(struct files *f, const char *path, unsigned mode, const unsigned char id[20]);
void files_sort(struct files *f);
struct file *files_find(const struct files *f, const char *path);
int files_have_prefix(const struct files *f, const char *prefix);
size_t files_diff(const struct files *a, const struct files *b, struct pair **out);
void files_free(struct files *f);
unsigned mode_from_stat(const struct stat *st);
void stat_data_from(struct stat_data *sd, const struct stat *st);
int stat_matches(const struct file *e, const struct stat *st, const uint32_t stamp[2]);
int index_read(struct files *out, uint32_t stamp[2]);
void index_write(const struct files *f);

/* ignore.c */
struct pattern;
struct ignore {
	struct pattern *v;
	size_t n, cap;
};
void ignore_init(struct ignore *ig);
size_t ignore_push(struct ignore *ig, const char *dir, const char *rel);
size_t ignore_push_text(struct ignore *ig, const char *text, const char *rel);
void ignore_pop(struct ignore *ig, size_t mark);
int ignore_match(const struct ignore *ig, const char *path, int isdir);
int wildmatch(const char *pat, const char *text, int pathname);

/* tree.c */
void tree_build(const struct files *f, unsigned char id[20]);
void tree_flatten(const unsigned char tree[20], struct files *out);
void tree_print(const buf *b);

/* worktree.c */
void state_load(struct repo_state *s);
void pathspec_parse(struct pathspec *ps, int argc, char **argv);
int pathspec_covers(const struct pathspec *ps, const char *path);
void files_keep(const struct files *from, const struct pathspec *ps, int covered, struct files *to);
void worktree_scan(const struct repo_state *s, int store, const struct pathspec *ps, struct files *out);
void worktree_switch(const struct files *from, struct files *to, const struct repo_state *s);
int cmd_checkout(int argc, char **argv);

/* commit.c */
void commit_parse(const buf *b, struct commit *c);
void commit_tree_id(const unsigned char commit[20], unsigned char tree[20]);
void ident(buf *out, const char *label, const char *role, const char *force);
void cleanup_message(buf *out, const char *in, size_t n, int comments);
int cmd_commit(int argc, char **argv);
int cmd_log(int argc, char **argv);

/* diff.c */
int cmd_status(int argc, char **argv);
int cmd_diff(int argc, char **argv);

/* sync.c: between local repositories, and pull */
char *other_repo(const char *dir, int *bare);
int push_local(const char *dir, const char *branch);
int cmd_pull(int argc, char **argv);

/* walk.c */
struct idset {
	unsigned char (*v)[20];
	char *used;
	size_t cap, n;
};
struct obj {
	unsigned char id[20];
	int type;
};
struct objlist {
	struct obj *v;
	size_t n, cap;
};
int idset_has(const struct idset *s, const unsigned char id[20]);
int idset_add(struct idset *s, const unsigned char id[20]);
void idset_free(struct idset *s);
void objlist_add(struct objlist *l, const unsigned char id[20], int type);
void walk(const unsigned char (*tips)[20], size_t ntips, struct idset *seen, struct objlist *out, int missing_ok);
int descends(const unsigned char tip[20], const unsigned char anc[20]);

/* packwrite.c */
uint32_t crc32_update(uint32_t crc, const void *d, size_t n);
void idx_build(buf *out, const struct objlist *objs, const uint64_t *offs, const uint32_t *crcs, const unsigned char sum[20]);
void pack_send(int fd, const char *what, const struct objlist *objs, uint64_t *offs, uint32_t *crcs, unsigned char sum[20]);
int cmd_gc(int argc, char **argv);

/* tag.c */
int cmd_tag(int argc, char **argv);

/* remote.c */
int cmd_remote(int argc, char **argv);
char *remote_url(const char *name, int push);
void config_value(buf *b, const char *v);

/* transport.c: pkt-lines, urls, advertised refs, ssh and curl as subprocesses */
enum { URL_LOCAL, URL_SSH, URL_HTTP };
struct target {
	int kind;
	char *host, *port, *path;	/* URL_HTTP: path is the whole url, trailing slashes dropped */
};
struct remote_ref {
	char *name;
	unsigned char id[20];
};
struct advert {
	struct remote_ref *v;
	size_t n;
	char *caps;	/* the first line's capabilities, "" for none */
	char *head;	/* the ref HEAD points at (symref=HEAD:...), or NULL when not said */
};
void read_exact(int fd, void *d, size_t n);
int pkt_read_raw(int fd, buf *data);
int pkt_read(int fd, buf *line);
void pkt_add(buf *out, const char *d, size_t n);
int parse_url(const char *url, struct target *t, const char *verb);
int has_cap(const char *caps, const char *name);
void advert_add(struct advert *a, const char *name, const unsigned char id[20]);
const struct remote_ref *advert_find(const struct advert *a, const char *name);
void advert_read(int fd, struct advert *a);
pid_t ssh_start(const struct target *t, const char *service, int *to, int *from);
pid_t http_start(const char *url, const buf *body, const char *type, int *from);
void child_finish(pid_t pid, int fd, const char *what);

/* fetch.c: clone and fetch (protocol v0 upload-pack over ssh or https, or from a path) */
int fetch_remote(const char *remote);
char *fetch_track(const char *remote, const char *ref);
void checkout_notices(const struct files *from, const struct files *to);
int cmd_clone(int argc, char **argv);
int cmd_fetch(int argc, char **argv);

/* push.c */
int cmd_push(int argc, char **argv);

/* check.c: the pre-push check */
struct check_ref {
	const char *name;	/* the full ref name the push would update, e.g. refs/heads/main */
	unsigned char id[20];
};
int check_run(const struct objlist *objs, const struct check_ref *refs, size_t nrefs, const char *what);
int cmd_check(int argc, char **argv);

#endif
