/*
 * p9.c — a 9P2000.L file server for one directory tree.
 *
 * This is how a directory of the host appears inside the guest: Linux mounts
 * it with its 9p filesystem, and every operation on it arrives here as a
 * message. The transport is not this file's business; p9_request() takes a
 * request and produces a reply.
 *
 * It serves the files as the user it runs as, and tells each user of the
 * guest that the files are theirs, with the mode they have on the host
 * (directories are open to all; see op_getattr). A
 * container, whatever user it runs as, can then do with a shared file what
 * the person who shared it can, and no more, and a program that insists on
 * owning its files, or on their being private, is satisfied. Requests to
 * change ownership are accepted and ignored.
 *
 * A file is known by its path, and the guest is not trusted with paths: it
 * can make symbolic links in the tree, and replace a directory by one at any
 * moment. So no path is ever handed to the host whole. Each is walked from
 * the root, one directory at a time, refusing links, and the operation is
 * then done on a name in the directory that was reached (dir_open). The
 * guest resolves links itself, by reading them, so the server never needs
 * to, and a link that points out of the tree leads nowhere here.
 *
 * Error numbers on the wire are Linux's, whatever the host's are.
 */

#include <sys/param.h>
#if defined(__linux__)
#include <sys/vfs.h>		/* statfs, when built there for tests */
#else
#include <sys/mount.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p9.h"

/* Message types. An R-message's type is its T-message's plus one. */
enum {
	P9_RLERROR = 7,
	P9_TSTATFS = 8, P9_TLOPEN = 12, P9_TLCREATE = 14, P9_TSYMLINK = 16,
	P9_TMKNOD = 18, P9_TRENAME = 20, P9_TREADLINK = 22, P9_TGETATTR = 24,
	P9_TSETATTR = 26, P9_TXATTRWALK = 30, P9_TXATTRCREATE = 32,
	P9_TREADDIR = 40, P9_TFSYNC = 50, P9_TLOCK = 52, P9_TGETLOCK = 54,
	P9_TLINK = 70, P9_TMKDIR = 72, P9_TRENAMEAT = 74, P9_TUNLINKAT = 76,
	P9_TVERSION = 100, P9_TATTACH = 104, P9_TFLUSH = 108, P9_TWALK = 110,
	P9_TREAD = 116, P9_TWRITE = 118, P9_TCLUNK = 120, P9_TREMOVE = 122,
};

#define P9_HDR		7		/* size[4] type[1] tag[2] */
#define P9_NOFID	0xFFFFFFFFu
#define P9_QTDIR	0x80
#define P9_QTSYMLINK	0x02
#define P9_MAXWELEM	16
#define P9_AT_REMOVEDIR	0x200

/* Tsetattr's "valid" bits. */
#define P9_ATTR_MODE		0x001
#define P9_ATTR_SIZE		0x008
#define P9_ATTR_ATIME		0x010
#define P9_ATTR_MTIME		0x020
#define P9_ATTR_ATIME_SET	0x080
#define P9_ATTR_MTIME_SET	0x100

/* Linux's error numbers, where the host's differ or might. */
#define L_EPERM		1
#define L_ENOENT	2
#define L_EIO		5
#define L_EBADF		9
#define L_EAGAIN	11
#define L_ENOMEM	12
#define L_EACCES	13
#define L_EEXIST	17
#define L_ENOTDIR	20
#define L_EISDIR	21
#define L_EINVAL	22
#define L_ENOSPC	28
#define L_EROFS		30
#define L_ENAMETOOLONG	36
#define L_ENOSYS	38
#define L_ENOTEMPTY	39
#define L_ELOOP		40
#define L_EDEADLK	35
#define L_EOVERFLOW	75
#define L_EILSEQ	84
#define L_EOPNOTSUPP	95
#define L_ESTALE	116
#define L_EDQUOT	122

static uint32_t
errno_to_linux(int e)
{
	switch (e) {
	case EAGAIN:		return L_EAGAIN;
	case ENAMETOOLONG:	return L_ENAMETOOLONG;
	case ENOSYS:		return L_ENOSYS;
	case ENOTEMPTY:		return L_ENOTEMPTY;
	case ELOOP:		return L_ELOOP;
	case EOVERFLOW:		return L_EOVERFLOW;
	case ENOTSUP:		return L_EOPNOTSUPP;
#if EOPNOTSUPP != ENOTSUP
	case EOPNOTSUPP:	return L_EOPNOTSUPP;
#endif
	case ESTALE:		return L_ESTALE;
	case EDQUOT:		return L_EDQUOT;
	case EDEADLK:		return L_EDEADLK;
	case EILSEQ:		return L_EILSEQ;
	default:
		/* Up to here the two agree. Anything else: an I/O error. */
		return (e > 0 && e <= 34) ? (uint32_t)e : L_EIO;
	}
}

/* Linux's open(2) flags, which the client sends as they are. */
static int
flags_from_linux(uint32_t f)
{
	int o = (int)(f & 3);		/* O_RDONLY, O_WRONLY, O_RDWR agree */

	if (f & 00000100) o |= O_CREAT;
	if (f & 00000200) o |= O_EXCL;
	if (f & 00001000) o |= O_TRUNC;
	if (f & 00002000) o |= O_APPEND;
	if (f & 00010000) o |= O_DSYNC;
	if (f & 04000000) o |= O_SYNC;
	return o;
}

/* -------------------------------------------------------------------------- */
/* Fids: the client's handles on files. */

#define P9_FID_BUCKETS	1024		/* 2^10: see fid_bucket() */
#define P9_FID_MAX	(1u << 18)	/* far more than a client holds */

struct fid {
	struct fid *next;	/* in its bucket */
	uint32_t id;
	char *path;		/* on the host; empty if it was lost */
	uint32_t uid;		/* who in the guest it was attached for */
	int fd;			/* -1 until opened */
	DIR *dir;		/* a directory opened for reading */
};

struct p9 {
	char root[PATH_MAX];
	size_t rootlen;
	int rootfd;
	uint32_t msize;
	struct fid *fids[P9_FID_BUCKETS];
	size_t nfids;
};

/* Spread, so that a client cannot choose ids that all land together. */
static size_t
fid_bucket(uint32_t id)
{
	return (id * 2654435761u) >> 22;	/* the top ten bits */
}

static struct fid *
fid_get(struct p9 *s, uint32_t id)
{
	struct fid *f;

	for (f = s->fids[fid_bucket(id)]; f != NULL; f = f->next) {
		if (f->id == id)
			return f;
	}
	return NULL;
}

static struct fid *
fid_new(struct p9 *s, uint32_t id, const char *path, uint32_t uid)
{
	struct fid *f;

	if (id == P9_NOFID || s->nfids >= P9_FID_MAX || fid_get(s, id) != NULL)
		return NULL;
	f = calloc(1, sizeof(*f));
	if (f == NULL)
		return NULL;
	f->path = strdup(path);
	if (f->path == NULL) {
		free(f);
		return NULL;
	}
	f->id = id;
	f->uid = uid;
	f->fd = -1;
	f->next = s->fids[fid_bucket(id)];
	s->fids[fid_bucket(id)] = f;
	s->nfids++;
	return f;
}

static void
fid_close(struct fid *f)
{
	if (f->dir != NULL)
		closedir(f->dir);	/* closes its descriptor too */
	else if (f->fd != -1)
		close(f->fd);
	f->dir = NULL;
	f->fd = -1;
}

static void
fid_drop(struct p9 *s, struct fid *f)
{
	struct fid **link = &s->fids[fid_bucket(f->id)];

	while (*link != f)
		link = &(*link)->next;
	*link = f->next;
	s->nfids--;
	fid_close(f);
	free(f->path);
	free(f);
}

static void
fids_clear(struct p9 *s)
{
	size_t i;

	for (i = 0; i < P9_FID_BUCKETS; i++) {
		while (s->fids[i] != NULL)
			fid_drop(s, s->fids[i]);
	}
}

/*
 * A file was renamed: every fid on it, or under it, follows. One that
 * cannot, for want of room, is left with no path, and fails from then on.
 */
static void
fids_renamed(struct p9 *s, const char *from, const char *to)
{
	const size_t n = strlen(from);
	char buf[PATH_MAX];
	struct fid *f;
	char *copy;
	size_t i;

	for (i = 0; i < P9_FID_BUCKETS; i++) {
		for (f = s->fids[i]; f != NULL; f = f->next) {
			if (strncmp(f->path, from, n) != 0 ||
			    (f->path[n] != '\0' && f->path[n] != '/'))
				continue;
			copy = NULL;
			if (snprintf(buf, sizeof(buf), "%s%s", to, f->path + n) <
			    (int)sizeof(buf))
				copy = strdup(buf);
			if (copy == NULL) {
				f->path[0] = '\0';
				continue;
			}
			free(f->path);
			f->path = copy;
		}
	}
}

/*
 * The path of 'name' in the directory 'dir'. A name is one component: it
 * cannot hold a slash. ".." goes up, but never above the root.
 */
static int
path_join(const struct p9 *s, const char *dir, const char *name, char *out,
    size_t cap)
{
	const char *slash;

	if (dir[0] == '\0')
		return ENOENT;
	if (name[0] == '\0' || strchr(name, '/') != NULL)
		return EINVAL;
	if (strcmp(name, ".") == 0)
		return snprintf(out, cap, "%s", dir) < (int)cap ? 0 :
		    ENAMETOOLONG;
	if (strcmp(name, "..") == 0) {
		if (strlen(dir) <= s->rootlen)
			return snprintf(out, cap, "%s", s->root) < (int)cap ?
			    0 : ENAMETOOLONG;
		slash = strrchr(dir, '/');
		if (slash == NULL || (size_t)(slash - dir) < s->rootlen)
			return EINVAL;
		if ((size_t)(slash - dir) >= cap)
			return ENAMETOOLONG;
		memcpy(out, dir, (size_t)(slash - dir));
		out[slash - dir] = '\0';
		return 0;
	}
	return snprintf(out, cap, "%s/%s", dir, name) < (int)cap ? 0 :
	    ENAMETOOLONG;
}

/* A name something can be created, renamed or removed under. */
static bool
name_ok(const char *name)
{
	return name[0] != '\0' && strchr(name, '/') == NULL &&
	    strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

#define P9_O_DIR	(O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)

/*
 * Open a directory of the tree, walking to it from the root and refusing
 * to pass through, or end on, a symbolic link. With 'leaf' NULL, it is
 * 'path' itself; otherwise it is the directory that holds 'path', and *leaf
 * is the name 'path' has there ("." for the root). Returns a descriptor to
 * close, or -1 with errno set.
 */
static int
dir_open(const struct p9 *s, const char *path, const char **leaf)
{
	char name[NAME_MAX + 1];
	const char *p, *end, *rest;
	int fd, next, error;

	if (strncmp(path, s->root, s->rootlen) != 0 || (s->rootlen > 1 &&
	    path[s->rootlen] != '\0' && path[s->rootlen] != '/')) {
		errno = ENOENT;
		return -1;
	}
	fd = dup(s->rootfd);
	if (fd == -1)
		return -1;
	if (leaf != NULL)
		*leaf = ".";
	p = path + s->rootlen;
	for (;;) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		end = strchr(p, '/');
		if (end == NULL)
			end = p + strlen(p);
		for (rest = end; *rest == '/'; rest++)
			;
		if (leaf != NULL && *rest == '\0') {
			*leaf = p;
			break;
		}
		if ((size_t)(end - p) > NAME_MAX) {
			close(fd);
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(name, p, (size_t)(end - p));
		name[end - p] = '\0';
		next = openat(fd, name, P9_O_DIR);
		error = errno;
		close(fd);
		if (next == -1) {
			errno = error;
			return -1;
		}
		fd = next;
		p = end;
	}
	return fd;
}

/* lstat(), by way of dir_open(). Returns 0 or an errno. */
static int
path_stat(const struct p9 *s, const char *path, struct stat *st)
{
	const char *leaf;
	const int fd = dir_open(s, path, &leaf);
	int error = 0;

	if (fd == -1)
		return errno;
	if (fstatat(fd, leaf, st, AT_SYMLINK_NOFOLLOW) == -1)
		error = errno;
	close(fd);
	return error;
}

/* A fid's file: the one it has open if it has, else the one at its path. */
static int
fid_stat(const struct p9 *s, const struct fid *f, struct stat *st)
{
	if (f->dir != NULL)
		return fstat(dirfd(f->dir), st) == -1 ? errno : 0;
	if (f->fd != -1)
		return fstat(f->fd, st) == -1 ? errno : 0;
	return path_stat(s, f->path, st);
}

/* -------------------------------------------------------------------------- */
/* Reading requests and writing replies. All integers are little-endian. */

struct rd {
	const uint8_t *p;
	size_t left;
	bool bad;
};

struct wr {
	uint8_t *start, *p;
	size_t left;
	bool full;
};

static uint64_t
rd_n(struct rd *r, unsigned int n)
{
	uint64_t v = 0;
	unsigned int i;

	if (r->left < n) {
		r->bad = true;
		r->left = 0;
		return 0;
	}
	for (i = 0; i < n; i++)
		v |= (uint64_t)r->p[i] << (8 * i);
	r->p += n;
	r->left -= n;
	return v;
}
#define rd8(r)	((uint8_t)rd_n((r), 1))
#define rd16(r)	((uint16_t)rd_n((r), 2))
#define rd32(r)	((uint32_t)rd_n((r), 4))
#define rd64(r)	rd_n((r), 8)

/* A string: its length, then its bytes. Copied out and terminated. */
static void
rd_str(struct rd *r, char *out, size_t cap)
{
	const uint16_t n = rd16(r);

	out[0] = '\0';
	if (r->bad || r->left < n || n >= cap || memchr(r->p, '\0', n) != NULL) {
		r->bad = true;
		return;
	}
	memcpy(out, r->p, n);
	out[n] = '\0';
	r->p += n;
	r->left -= n;
}

static void
wr_n(struct wr *w, uint64_t v, unsigned int n)
{
	unsigned int i;

	if (w->left < n) {
		w->full = true;
		return;
	}
	for (i = 0; i < n; i++)
		w->p[i] = (uint8_t)(v >> (8 * i));
	w->p += n;
	w->left -= n;
}
#define wr8(w, v)	wr_n((w), (v), 1)
#define wr16(w, v)	wr_n((w), (v), 2)
#define wr32(w, v)	wr_n((w), (v), 4)
#define wr64(w, v)	wr_n((w), (v), 8)

static void
wr_str(struct wr *w, const char *str)
{
	const size_t n = strlen(str);

	if (n > 0xFFFF || w->left < 2 + n) {
		w->full = true;
		return;
	}
	wr16(w, n);
	memcpy(w->p, str, n);
	w->p += n;
	w->left -= n;
}

static void
wr_qid(struct wr *w, const struct stat *st)
{
	uint8_t type = 0;

	if (S_ISDIR(st->st_mode))
		type = P9_QTDIR;
	else if (S_ISLNK(st->st_mode))
		type = P9_QTSYMLINK;
	wr8(w, type);
	wr32(w, (uint32_t)st->st_mtime);	/* changes when the file does */
	wr64(w, st->st_ino);
}

/* -------------------------------------------------------------------------- */
/* The operations. Each returns 0, or an errno of the host's. */

static int
op_version(struct p9 *s, struct rd *r, struct wr *w)
{
	uint32_t msize = rd32(r);
	char version[32];

	rd_str(r, version, sizeof(version));
	if (r->bad)
		return EINVAL;
	if (msize > P9_MSIZE_MAX)
		msize = P9_MSIZE_MAX;
	if (msize < 4096)
		return EINVAL;
	s->msize = msize;
	/* A new session: whatever the last one held is let go. */
	fids_clear(s);
	wr32(w, msize);
	wr_str(w, strcmp(version, "9P2000.L") == 0 ? "9P2000.L" : "unknown");
	return 0;
}

static int
op_attach(struct p9 *s, struct rd *r, struct wr *w)
{
	const uint32_t id = rd32(r);
	char name[NAME_MAX + 1];
	struct stat st;
	uint32_t uid;

	(void)rd32(r);				/* afid: no authentication */
	rd_str(r, name, sizeof(name));		/* uname */
	rd_str(r, name, sizeof(name));		/* aname */
	uid = rd32(r);
	if (r->bad)
		return EINVAL;
	if (uid == P9_NOFID)			/* nobody in particular */
		uid = 0;
	if (fstat(s->rootfd, &st) == -1)
		return errno;
	if (fid_new(s, id, s->root, uid) == NULL)
		return EBADF;
	wr_qid(w, &st);
	return 0;
}

static int
op_walk(struct p9 *s, struct rd *r, struct wr *w)
{
	const uint32_t id = rd32(r), newid = rd32(r);
	const uint16_t n = rd16(r);
	struct fid *f = fid_get(s, id);
	struct stat st[P9_MAXWELEM];
	char path[PATH_MAX], next[PATH_MAX], name[NAME_MAX + 1];
	uint16_t i, done = 0;
	uint32_t uid;
	int error = 0;

	if (r->bad || n > P9_MAXWELEM)
		return EINVAL;
	if (f == NULL)
		return EBADF;
	if (id != newid && fid_get(s, newid) != NULL)
		return EBADF;
	snprintf(path, sizeof(path), "%s", f->path);
	uid = f->uid;

	for (i = 0; i < n; i++) {
		rd_str(r, name, sizeof(name));
		if (r->bad)
			return EINVAL;
		/* Only a real directory is walked through, never a link. */
		error = path_join(s, path, name, next, sizeof(next));
		if (error == 0)
			error = path_stat(s, next, &st[i]);
		if (error != 0)
			break;
		memcpy(path, next, sizeof(path));
		done++;
	}

	/* Failing on the first name is an error; later, a partial walk. */
	if (n > 0 && done == 0)
		return error;
	if (done == n) {
		if (id == newid && n > 0) {
			char *copy = strdup(path);

			if (copy == NULL)
				return ENOMEM;
			fid_close(f);
			free(f->path);
			f->path = copy;
		} else if (id == newid) {
			/* Nowhere, and onto itself: nothing to do. */
		} else if (fid_new(s, newid, path, uid) == NULL) {
			return ENOMEM;
		}
	}
	wr16(w, done);
	for (i = 0; i < done; i++)
		wr_qid(w, &st[i]);
	return 0;
}

static int
op_getattr(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	struct stat st;
	uint32_t mode;
	int error;

	(void)rd64(r);				/* which fields are wanted */
	if (r->bad)
		return EINVAL;
	if (f == NULL)
		return EBADF;
	error = fid_stat(s, f, &st);
	if (error != 0)
		return error;

	wr64(w, 0x7FF);				/* the basic fields */
	wr_qid(w, &st);
	/*
	 * Whoever asks owns it. Users in the guest are not users of the host,
	 * and all of them act here as the one this runs as; telling each
	 * that the files are theirs lets the guest's own permission check
	 * agree with that, and leaves the mode as it is on the host.
	 *
	 * Except for directories. The guest checks permission against the
	 * owner it last heard of, which may be another user's answer, and a
	 * directory it holds on to (a mount point, a working directory) is
	 * not asked about again. So a directory gives everyone what its owner
	 * has, and the check passes whoever that is taken to be.
	 */
	mode = (uint32_t)st.st_mode;
	if (S_ISDIR(st.st_mode))
		mode |= (mode & 0700) >> 3 | (mode & 0700) >> 6;
	wr32(w, mode);
	wr32(w, f->uid);
	wr32(w, f->uid);
	wr64(w, st.st_nlink);
	wr64(w, (uint64_t)st.st_rdev);
	wr64(w, (uint64_t)st.st_size);
	wr64(w, (uint64_t)st.st_blksize);
	wr64(w, (uint64_t)st.st_blocks);
#if defined(__APPLE__)
	wr64(w, (uint64_t)st.st_atimespec.tv_sec);
	wr64(w, (uint64_t)st.st_atimespec.tv_nsec);
	wr64(w, (uint64_t)st.st_mtimespec.tv_sec);
	wr64(w, (uint64_t)st.st_mtimespec.tv_nsec);
	wr64(w, (uint64_t)st.st_ctimespec.tv_sec);
	wr64(w, (uint64_t)st.st_ctimespec.tv_nsec);
#else
	wr64(w, (uint64_t)st.st_atim.tv_sec);
	wr64(w, (uint64_t)st.st_atim.tv_nsec);
	wr64(w, (uint64_t)st.st_mtim.tv_sec);
	wr64(w, (uint64_t)st.st_mtim.tv_nsec);
	wr64(w, (uint64_t)st.st_ctim.tv_sec);
	wr64(w, (uint64_t)st.st_ctim.tv_nsec);
#endif
	wr64(w, 0);				/* birth time, generation, */
	wr64(w, 0);				/* data version: not given */
	wr64(w, 0);
	wr64(w, 0);
	return 0;
}

static int
op_setattr(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	const uint32_t valid = rd32(r), mode = rd32(r);
	uint64_t size, asec, ansec, msec, mnsec;
	struct timespec ts[2];
	const char *leaf = NULL;
	int dfd = -1, fd, rc, error;

	(void)w;
	(void)rd32(r);				/* uid: ignored */
	(void)rd32(r);				/* gid: ignored */
	size = rd64(r);
	asec = rd64(r);
	ansec = rd64(r);
	msec = rd64(r);
	mnsec = rd64(r);
	if (r->bad)
		return EINVAL;
	if (f == NULL)
		return EBADF;

	/*
	 * On the file the fid has open, if it has: that is the one meant,
	 * even after its name was given to another or taken away.
	 */
	if (f->fd == -1) {
		dfd = dir_open(s, f->path, &leaf);
		if (dfd == -1)
			return errno;
	}
	if (valid & P9_ATTR_MODE) {
		/*
		 * The permission bits, and for a directory the sticky one;
		 * nothing set-user-ID.
		 */
		const mode_t m = mode & (((mode & 0170000) == 0040000) ?
		    01777 : 0777);

		rc = f->fd != -1 ? fchmod(f->fd, m) :
		    fchmodat(dfd, leaf, m, AT_SYMLINK_NOFOLLOW);
		if (rc == -1 && errno != ENOTSUP && errno != EOPNOTSUPP)
			goto fail;
	}
	if ((valid & P9_ATTR_SIZE) && f->fd != -1) {
		if (ftruncate(f->fd, (off_t)size) == -1)
			goto fail;
	} else if (valid & P9_ATTR_SIZE) {
		fd = openat(dfd, leaf, O_WRONLY | O_NOFOLLOW | O_NONBLOCK |
		    O_CLOEXEC);
		if (fd == -1)
			goto fail;
		rc = ftruncate(fd, (off_t)size);
		error = errno;
		close(fd);
		errno = error;
		if (rc == -1)
			goto fail;
	}
	if (valid & (P9_ATTR_ATIME | P9_ATTR_MTIME)) {
		ts[0].tv_nsec = ts[1].tv_nsec = UTIME_OMIT;
		ts[0].tv_sec = ts[1].tv_sec = 0;
		if (valid & P9_ATTR_ATIME) {
			ts[0].tv_nsec = UTIME_NOW;
			if (valid & P9_ATTR_ATIME_SET) {
				ts[0].tv_sec = (time_t)asec;
				ts[0].tv_nsec = (long)ansec;
			}
		}
		if (valid & P9_ATTR_MTIME) {
			ts[1].tv_nsec = UTIME_NOW;
			if (valid & P9_ATTR_MTIME_SET) {
				ts[1].tv_sec = (time_t)msec;
				ts[1].tv_nsec = (long)mnsec;
			}
		}
		rc = f->fd != -1 ? futimens(f->fd, ts) :
		    utimensat(dfd, leaf, ts, AT_SYMLINK_NOFOLLOW);
		if (rc == -1)
			goto fail;
	}
	if (dfd != -1)
		close(dfd);
	return 0;
fail:
	error = errno;
	if (dfd != -1)
		close(dfd);
	return error;
}

static int
op_lopen(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	const uint32_t flags = rd32(r);
	const char *leaf;
	struct stat st;
	int dfd, fd = -1, error = 0;

	if (r->bad)
		return EINVAL;
	if (f == NULL || f->fd != -1 || f->dir != NULL)
		return EBADF;
	dfd = dir_open(s, f->path, &leaf);
	if (dfd == -1)
		return errno;

	if (fstatat(dfd, leaf, &st, AT_SYMLINK_NOFOLLOW) == -1) {
		error = errno;
	} else if (S_ISDIR(st.st_mode)) {
		fd = openat(dfd, leaf, P9_O_DIR);
		if (fd == -1 || (f->dir = fdopendir(fd)) == NULL) {
			error = errno;
			if (fd != -1)
				close(fd);
		}
	} else if (!S_ISREG(st.st_mode)) {
		/* The guest keeps pipes and devices to itself. */
		error = S_ISLNK(st.st_mode) ? ELOOP : ENXIO;
	} else {
		/*
		 * Never create here: lcreate does that. And never wait: what
		 * is a file now might be a pipe by the time it is opened.
		 */
		fd = openat(dfd, leaf, (flags_from_linux(flags) &
		    ~(O_CREAT | O_EXCL)) | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (fd == -1) {
			error = errno;
		} else if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
			error = ENXIO;
			close(fd);
		} else {
			f->fd = fd;
		}
	}
	close(dfd);
	if (error != 0)
		return error;
	wr_qid(w, &st);
	wr32(w, 0);				/* iounit: the client decides */
	return 0;
}

static int
op_lcreate(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	char name[NAME_MAX + 1], path[PATH_MAX], *copy;
	uint32_t flags, mode;
	struct stat st;
	int dfd, fd, error;

	rd_str(r, name, sizeof(name));
	flags = rd32(r);
	mode = rd32(r);
	(void)rd32(r);				/* gid */
	if (r->bad || !name_ok(name))
		return EINVAL;
	if (f == NULL || f->fd != -1 || f->dir != NULL)
		return EBADF;
	error = path_join(s, f->path, name, path, sizeof(path));
	if (error != 0)
		return error;

	dfd = dir_open(s, f->path, NULL);
	if (dfd == -1)
		return errno;
	fd = openat(dfd, name, flags_from_linux(flags) | O_CREAT | O_NOFOLLOW |
	    O_NONBLOCK | O_CLOEXEC, mode & 0777);
	error = errno;
	close(dfd);
	if (fd == -1)
		return error;
	if (fstat(fd, &st) == -1) {
		close(fd);
		return EIO;
	}
	if (!S_ISREG(st.st_mode) || (copy = strdup(path)) == NULL) {
		close(fd);
		return S_ISDIR(st.st_mode) ? EISDIR : EIO;
	}
	/* The fid, which was the directory, becomes the new file. */
	free(f->path);
	f->path = copy;
	f->fd = fd;
	wr_qid(w, &st);
	wr32(w, 0);
	return 0;
}

static int
op_read(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	const uint64_t off = rd64(r);
	uint32_t count = rd32(r);
	ssize_t n;

	if (r->bad)
		return EINVAL;
	if (f == NULL || f->fd == -1)
		return EBADF;
	if (w->left < 4)
		return EIO;
	if (count > w->left - 4)
		count = (uint32_t)(w->left - 4);
	n = pread(f->fd, w->p + 4, count, (off_t)off);
	if (n == -1)
		return errno;
	wr32(w, (uint32_t)n);
	w->p += n;
	w->left -= (size_t)n;
	return 0;
}

static int
op_write(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	const uint64_t off = rd64(r);
	const uint32_t count = rd32(r);
	ssize_t n;

	if (r->bad || r->left < count)
		return EINVAL;
	if (f == NULL || f->fd == -1)
		return EBADF;
	n = pwrite(f->fd, r->p, count, (off_t)off);
	if (n == -1)
		return errno;
	wr32(w, (uint32_t)n);
	return 0;
}

static int
op_readdir(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	const uint64_t off = rd64(r);
	uint32_t count = rd32(r);
	uint8_t *countp;
	struct dirent *de;
	struct wr body;

	if (r->bad)
		return EINVAL;
	if (f == NULL || f->dir == NULL)
		return EBADF;
	if (w->left < 4)
		return EIO;
	if (count > w->left - 4)
		count = (uint32_t)(w->left - 4);

	if (off == 0)
		rewinddir(f->dir);
	else
		seekdir(f->dir, (long)off);

	countp = w->p;
	body.start = body.p = w->p + 4;
	body.left = count;
	body.full = false;
	for (;;) {
		const long before = telldir(f->dir);
		uint8_t *entry = body.p;
		const size_t room = body.left;
		struct stat st;

		de = readdir(f->dir);
		if (de == NULL)
			break;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = (de->d_type == DT_DIR) ? S_IFDIR :
		    (de->d_type == DT_LNK) ? S_IFLNK : S_IFREG;
		wr_qid(&body, &st);
		wr64(&body, (uint64_t)telldir(f->dir));	/* where the next is */
		wr8(&body, de->d_type);
		wr_str(&body, de->d_name);
		if (body.full) {
			/* It did not fit: leave it for the next request. */
			seekdir(f->dir, before);
			if (entry == body.start)
				return EINVAL;	/* and never will */
			body.p = entry;
			body.left = room;
			break;
		}
	}
	w->p = countp;
	wr32(w, (uint32_t)(body.p - body.start));
	w->left -= (size_t)(body.p - body.start);
	w->p = body.p;
	return 0;
}

static int
op_statfs(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	const char *leaf;
	struct statfs sf;
	int dfd, rc, error;

	if (r->bad)
		return EINVAL;
	if (f == NULL)
		return EBADF;
	/* Of the directory it is in: the same filesystem, bar a mount. */
	dfd = dir_open(s, f->path, &leaf);
	if (dfd == -1)
		return errno;
	rc = fstatfs(dfd, &sf);
	error = errno;
	close(dfd);
	if (rc == -1)
		return error;
	wr32(w, 0x01021997);			/* V9FS_MAGIC */
	wr32(w, (uint32_t)sf.f_bsize);
	wr64(w, sf.f_blocks);
	wr64(w, sf.f_bfree);
	wr64(w, sf.f_bavail);
	wr64(w, sf.f_files);
	wr64(w, sf.f_ffree);
	wr64(w, 0);				/* fsid */
	wr32(w, NAME_MAX);
	return 0;
}

static int
op_mkdir(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	char name[NAME_MAX + 1];
	struct stat st;
	uint32_t mode;
	int dfd, error = 0;

	rd_str(r, name, sizeof(name));
	mode = rd32(r);
	(void)rd32(r);				/* gid */
	if (r->bad || !name_ok(name))
		return EINVAL;
	if (f == NULL)
		return EBADF;
	dfd = dir_open(s, f->path, NULL);
	if (dfd == -1)
		return errno;
	if (mkdirat(dfd, name, mode & 01777) == -1 ||
	    fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) == -1)
		error = errno;
	close(dfd);
	if (error != 0)
		return error;
	wr_qid(w, &st);
	return 0;
}

static int
op_symlink(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	char name[NAME_MAX + 1], target[PATH_MAX];
	struct stat st;
	int dfd, error = 0;

	rd_str(r, name, sizeof(name));
	rd_str(r, target, sizeof(target));
	(void)rd32(r);				/* gid */
	if (r->bad || !name_ok(name))
		return EINVAL;
	if (f == NULL)
		return EBADF;
	dfd = dir_open(s, f->path, NULL);
	if (dfd == -1)
		return errno;
	if (symlinkat(target, dfd, name) == -1 ||
	    fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) == -1)
		error = errno;
	close(dfd);
	if (error != 0)
		return error;
	wr_qid(w, &st);
	return 0;
}

static int
op_readlink(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	char target[PATH_MAX];
	const char *leaf;
	ssize_t n;
	int dfd, error;

	if (r->bad)
		return EINVAL;
	if (f == NULL)
		return EBADF;
	dfd = dir_open(s, f->path, &leaf);
	if (dfd == -1)
		return errno;
	n = readlinkat(dfd, leaf, target, sizeof(target) - 1);
	error = errno;
	close(dfd);
	if (n == -1)
		return error;
	target[n] = '\0';
	wr_str(w, target);
	return 0;
}

static int
op_link(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *dir = fid_get(s, rd32(r)), *f = fid_get(s, rd32(r));
	char name[NAME_MAX + 1];
	const char *leaf;
	struct stat st;
	int sfd, dfd, error = 0;

	(void)w;
	rd_str(r, name, sizeof(name));
	if (r->bad || !name_ok(name))
		return EINVAL;
	if (dir == NULL || f == NULL)
		return EBADF;
	sfd = dir_open(s, f->path, &leaf);
	if (sfd == -1)
		return errno;
	dfd = dir_open(s, dir->path, NULL);
	if (dfd == -1) {
		error = errno;
	} else if (fstatat(sfd, leaf, &st, AT_SYMLINK_NOFOLLOW) == -1) {
		error = errno;
	} else if (!S_ISREG(st.st_mode)) {
		/* A second name for a link would be one for what it names. */
		error = EPERM;
	} else if (linkat(sfd, leaf, dfd, name, 0) == -1) {
		error = errno;
	}
	if (dfd != -1)
		close(dfd);
	close(sfd);
	return error;
}

static int
do_rename(struct p9 *s, const char *from, const char *to)
{
	const char *oleaf, *nleaf;
	int ofd, nfd, error = 0;

	if (strcmp(from, s->root) == 0 || strcmp(to, s->root) == 0)
		return EINVAL;
	ofd = dir_open(s, from, &oleaf);
	if (ofd == -1)
		return errno;
	nfd = dir_open(s, to, &nleaf);
	if (nfd == -1 || renameat(ofd, oleaf, nfd, nleaf) == -1)
		error = errno;
	if (nfd != -1)
		close(nfd);
	close(ofd);
	if (error == 0)
		fids_renamed(s, from, to);
	return error;
}

static int
op_renameat(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *odir = fid_get(s, rd32(r)), *ndir;
	char oname[NAME_MAX + 1], nname[NAME_MAX + 1];
	char from[PATH_MAX], to[PATH_MAX];
	int error;

	(void)w;
	rd_str(r, oname, sizeof(oname));
	ndir = fid_get(s, rd32(r));
	rd_str(r, nname, sizeof(nname));
	if (r->bad || !name_ok(oname) || !name_ok(nname))
		return EINVAL;
	if (odir == NULL || ndir == NULL)
		return EBADF;
	error = path_join(s, odir->path, oname, from, sizeof(from));
	if (error == 0)
		error = path_join(s, ndir->path, nname, to, sizeof(to));
	return error ? error : do_rename(s, from, to);
}

static int
op_rename(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r)), *dir = fid_get(s, rd32(r));
	char name[NAME_MAX + 1], from[PATH_MAX], to[PATH_MAX];
	int error;

	(void)w;
	rd_str(r, name, sizeof(name));
	if (r->bad || !name_ok(name))
		return EINVAL;
	if (f == NULL || dir == NULL)
		return EBADF;
	snprintf(from, sizeof(from), "%s", f->path);
	error = path_join(s, dir->path, name, to, sizeof(to));
	return error ? error : do_rename(s, from, to);
}

static int
op_unlinkat(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *dir = fid_get(s, rd32(r));
	char name[NAME_MAX + 1];
	uint32_t flags;
	int dfd, error = 0;

	(void)w;
	rd_str(r, name, sizeof(name));
	flags = rd32(r);
	if (r->bad || !name_ok(name))
		return EINVAL;
	if (dir == NULL)
		return EBADF;
	dfd = dir_open(s, dir->path, NULL);
	if (dfd == -1)
		return errno;
	if (unlinkat(dfd, name, (flags & P9_AT_REMOVEDIR) ? AT_REMOVEDIR : 0))
		error = errno;
	close(dfd);
	return error;
}

static int
op_remove(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));
	const char *leaf;
	struct stat st;
	int dfd, error = 0;

	(void)w;
	if (r->bad)
		return EINVAL;
	if (f == NULL)
		return EBADF;
	if (strcmp(f->path, s->root) == 0) {
		error = EINVAL;
	} else if ((dfd = dir_open(s, f->path, &leaf)) == -1) {
		error = errno;
	} else {
		if (fstatat(dfd, leaf, &st, AT_SYMLINK_NOFOLLOW) == -1 ||
		    unlinkat(dfd, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0))
			error = errno;
		close(dfd);
	}
	fid_drop(s, f);				/* gone whatever happened */
	return error;
}

static int
op_fsync(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));

	(void)w;
	if (r->bad)
		return EINVAL;
	if (f == NULL)
		return EBADF;
	if (f->fd != -1 && fsync(f->fd) == -1)
		return errno;
	return 0;
}

static int
op_clunk(struct p9 *s, struct rd *r, struct wr *w)
{
	struct fid *f = fid_get(s, rd32(r));

	(void)w;
	if (r->bad)
		return EINVAL;
	if (f == NULL)
		return EBADF;
	fid_drop(s, f);
	return 0;
}

/* Locks are granted: there is one client, and it keeps its own. */
static int
op_lock(struct p9 *s, struct rd *r, struct wr *w)
{
	if (fid_get(s, rd32(r)) == NULL)
		return EBADF;
	wr8(w, 0);				/* success */
	return 0;
}

static int
op_getlock(struct p9 *s, struct rd *r, struct wr *w)
{
	char client[256];
	uint64_t start, length;
	uint32_t proc;

	if (fid_get(s, rd32(r)) == NULL)
		return EBADF;
	(void)rd8(r);
	start = rd64(r);
	length = rd64(r);
	proc = rd32(r);
	rd_str(r, client, sizeof(client));
	if (r->bad)
		return EINVAL;
	wr8(w, 2);				/* F_UNLCK: nothing in the way */
	wr64(w, start);
	wr64(w, length);
	wr32(w, proc);
	wr_str(w, client);
	return 0;
}

/* -------------------------------------------------------------------------- */

struct p9 *
p9_new(const char *root)
{
	struct stat st;
	struct p9 *s;

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	if (realpath(root, s->root) == NULL || stat(s->root, &st) == -1 ||
	    !S_ISDIR(st.st_mode)) {
		free(s);
		return NULL;
	}
	s->rootfd = open(s->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (s->rootfd == -1) {
		free(s);
		return NULL;
	}
	s->rootlen = strlen(s->root);
	s->msize = 8192;
	return s;
}

void
p9_free(struct p9 *s)
{
	fids_clear(s);
	close(s->rootfd);
	free(s);
}

size_t
p9_request(struct p9 *s, const uint8_t *req, size_t len, uint8_t *rep,
    size_t cap)
{
	struct rd r;
	struct wr w;
	uint32_t size;
	uint16_t tag;
	uint8_t type;
	int error;

	if (len < P9_HDR || cap < P9_HDR + 4)
		return 0;
	r.p = req;
	r.left = len;
	r.bad = false;
	size = rd32(&r);
	type = rd8(&r);
	tag = rd16(&r);
	if (size != len)
		return 0;

	if (cap > s->msize && type != P9_TVERSION)
		cap = s->msize;
	w.start = rep;
	w.p = rep + P9_HDR;
	w.left = cap - P9_HDR;
	w.full = false;

	switch (type) {
	case P9_TVERSION:	error = op_version(s, &r, &w); break;
	case P9_TATTACH:	error = op_attach(s, &r, &w); break;
	case P9_TWALK:		error = op_walk(s, &r, &w); break;
	case P9_TGETATTR:	error = op_getattr(s, &r, &w); break;
	case P9_TSETATTR:	error = op_setattr(s, &r, &w); break;
	case P9_TLOPEN:		error = op_lopen(s, &r, &w); break;
	case P9_TLCREATE:	error = op_lcreate(s, &r, &w); break;
	case P9_TREAD:		error = op_read(s, &r, &w); break;
	case P9_TWRITE:		error = op_write(s, &r, &w); break;
	case P9_TREADDIR:	error = op_readdir(s, &r, &w); break;
	case P9_TSTATFS:	error = op_statfs(s, &r, &w); break;
	case P9_TMKDIR:		error = op_mkdir(s, &r, &w); break;
	case P9_TSYMLINK:	error = op_symlink(s, &r, &w); break;
	case P9_TREADLINK:	error = op_readlink(s, &r, &w); break;
	case P9_TLINK:		error = op_link(s, &r, &w); break;
	case P9_TRENAMEAT:	error = op_renameat(s, &r, &w); break;
	case P9_TRENAME:	error = op_rename(s, &r, &w); break;
	case P9_TUNLINKAT:	error = op_unlinkat(s, &r, &w); break;
	case P9_TREMOVE:	error = op_remove(s, &r, &w); break;
	case P9_TFSYNC:		error = op_fsync(s, &r, &w); break;
	case P9_TCLUNK:		error = op_clunk(s, &r, &w); break;
	case P9_TLOCK:		error = op_lock(s, &r, &w); break;
	case P9_TGETLOCK:	error = op_getlock(s, &r, &w); break;
	case P9_TFLUSH:		error = 0; break;	/* nothing is pending */
	default:		error = ENOTSUP; break;	/* xattrs, mknod, ... */
	}
	if (error == 0 && w.full)
		error = EIO;

	if (error != 0) {
		w.p = rep + P9_HDR;
		w.left = cap - P9_HDR;
		wr32(&w, errno_to_linux(error));
		type = P9_RLERROR - 1;
	}
	size = (uint32_t)(w.p - rep);
	w.p = rep;
	w.left = P9_HDR;
	wr32(&w, size);
	wr8(&w, type + 1);
	wr16(&w, tag);
	return size;
}
