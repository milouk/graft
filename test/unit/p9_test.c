/*
 * p9_test.c — the 9P file server, driven the way the guest's kernel drives
 * it: message by message, against a real directory on this machine.
 */

#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p9.h"

static int checks;
#define CHECK(cond)							\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,	\
			    __LINE__, #cond);				\
			exit(1);					\
		}							\
	} while (0)

static struct p9 *srv;
static uint8_t req[P9_MSIZE_MAX], rep[P9_MSIZE_MAX];
static size_t reqlen, replen, rpos;

/*
 * Build and send one request. The format string says what follows the
 * header: 1, 2, 4, 8 for integers of that many bytes, s for a string, and
 * d for raw bytes (a pointer, then a length).
 */
static uint8_t
call(uint8_t type, const char *fmt, ...)
{
	va_list ap;
	size_t n = 7, len;
	uint64_t v;
	const char *str;
	int i, bytes;

	va_start(ap, fmt);
	for (; *fmt != '\0'; fmt++) {
		switch (*fmt) {
		case '1': case '2': case '4': case '8':
			bytes = *fmt - '0';
			v = (bytes == 8) ? va_arg(ap, uint64_t) :
			    (uint64_t)va_arg(ap, unsigned int);
			for (i = 0; i < bytes; i++)
				req[n++] = (uint8_t)(v >> (8 * i));
			break;
		case 's':
			str = va_arg(ap, const char *);
			len = strlen(str);
			req[n++] = (uint8_t)len;
			req[n++] = (uint8_t)(len >> 8);
			memcpy(req + n, str, len);
			n += len;
			break;
		case 'd':
			str = va_arg(ap, const char *);
			len = va_arg(ap, size_t);
			memcpy(req + n, str, len);
			n += len;
			break;
		}
	}
	va_end(ap);

	req[0] = (uint8_t)n;
	req[1] = (uint8_t)(n >> 8);
	req[2] = (uint8_t)(n >> 16);
	req[3] = (uint8_t)(n >> 24);
	req[4] = type;
	req[5] = 0x34;
	req[6] = 0x12;
	reqlen = n;

	replen = p9_request(srv, req, reqlen, rep, sizeof(rep));
	CHECK(replen >= 7);
	CHECK((size_t)(rep[0] | rep[1] << 8 | rep[2] << 16 |
	    (uint32_t)rep[3] << 24) == replen);
	CHECK(rep[5] == 0x34 && rep[6] == 0x12);	/* the tag comes back */
	rpos = 7;
	if (getenv("P9_TRACE") != NULL)
		fprintf(stderr, "T%u -> R%u%s%u\n", type, rep[4],
		    rep[4] == 7 ? " error " : " len ", rep[4] == 7 ?
		    (unsigned)(rep[7] | rep[8] << 8) : (unsigned)replen);
	return rep[4];
}

static uint64_t
get(int bytes)
{
	uint64_t v = 0;
	int i;

	CHECK(rpos + (size_t)bytes <= replen);
	for (i = 0; i < bytes; i++)
		v |= (uint64_t)rep[rpos++] << (8 * i);
	return v;
}

/* The error number of an Rlerror, or 0 if the reply is not one. */
static uint32_t
err(uint8_t type)
{
	return type == 7 ? (uint32_t)get(4) : 0;
}

#define T_STATFS 8
#define T_LOPEN 12
#define T_LCREATE 14
#define T_SYMLINK 16
#define T_READLINK 22
#define T_GETATTR 24
#define T_SETATTR 26
#define T_XATTRWALK 30
#define T_READDIR 40
#define T_LINK 70
#define T_MKDIR 72
#define T_RENAMEAT 74
#define T_UNLINKAT 76
#define T_VERSION 100
#define T_ATTACH 104
#define T_WALK 110
#define T_READ 116
#define T_WRITE 118
#define T_CLUNK 120

int
main(void)
{
	char root[] = "/tmp/p9test.XXXXXX", outside[] = "/tmp/p9out.XXXXXX";
	char path[1024], buf[256];
	uint32_t n;
	uint64_t mode;
	int fd, seen;
	struct stat sb;

	CHECK(mkdtemp(root) != NULL);
	CHECK(mkdtemp(outside) != NULL);
	snprintf(path, sizeof(path), "%s/secret", outside);
	fd = open(path, O_CREAT | O_WRONLY, 0600);
	CHECK(fd != -1 && write(fd, "secret", 6) == 6 && close(fd) == 0);

	srv = p9_new(root);
	CHECK(srv != NULL);
	CHECK(p9_new("/nonexistent/place") == NULL);

	/* --- the session ---------------------------------------------- */
	CHECK(call(T_VERSION, "4s", 65536u, "9P2000.L") == T_VERSION + 1);
	CHECK(get(4) == 65536);
	CHECK(call(T_ATTACH, "44ss4", 0u, ~0u, "root", "", 0u) == T_ATTACH + 1);
	CHECK(get(1) == 0x80);				/* a directory */
	CHECK(err(call(T_ATTACH, "44ss4", 0u, ~0u, "root", "", 0u)) != 0);

	/* --- create, write, read back --------------------------------- */
	CHECK(call(T_WALK, "442", 0u, 1u, 0u) == T_WALK + 1);	/* clone */
	CHECK(call(T_LCREATE, "4s444", 1u, "hello.txt", 0102u, 0640u, 0u) ==
	    T_LCREATE + 1);
	CHECK(call(T_WRITE, "484d", 1u, (uint64_t)0, 11u, "hello world",
	    (size_t)11) == T_WRITE + 1);
	CHECK(get(4) == 11);
	CHECK(call(T_READ, "484", 1u, (uint64_t)6, 100u) == T_READ + 1);
	CHECK(get(4) == 5 && memcmp(rep + rpos, "world", 5) == 0);
	CHECK(call(T_CLUNK, "4", 1u) == T_CLUNK + 1);
	CHECK(err(call(T_CLUNK, "4", 1u)) == 9);	/* EBADF: gone */

	snprintf(path, sizeof(path), "%s/hello.txt", root);
	fd = open(path, O_RDONLY);
	CHECK(fd != -1 && read(fd, buf, sizeof(buf)) == 11 &&
	    memcmp(buf, "hello world", 11) == 0);
	close(fd);

	/* --- attributes: the host's mode, owned by whoever attached ----- */
	CHECK(call(T_WALK, "442s", 0u, 2u, 1u, "hello.txt") == T_WALK + 1);
	CHECK(get(2) == 1);
	CHECK(call(T_GETATTR, "48", 2u, (uint64_t)0x7FF) == T_GETATTR + 1);
	(void)get(8);
	rpos += 13;
	mode = get(4);
	CHECK((mode & 0777) == 0640);		/* as on the host */
	CHECK(get(4) == 0 && get(4) == 0);	/* uid, gid */
	(void)get(8);
	(void)get(8);
	CHECK(get(8) == 11);			/* size */

	/* Another user is told the same file is theirs. */
	CHECK(call(T_ATTACH, "44ss4", 50u, ~0u, "user", "", 1000u) ==
	    T_ATTACH + 1);
	CHECK(call(T_WALK, "442s", 50u, 51u, 1u, "hello.txt") == T_WALK + 1);
	CHECK(call(T_GETATTR, "48", 51u, (uint64_t)0x7FF) == T_GETATTR + 1);
	(void)get(8);
	rpos += 13;
	CHECK((get(4) & 0777) == 0640);
	CHECK(get(4) == 1000 && get(4) == 1000);
	CHECK(call(T_CLUNK, "4", 51u) == T_CLUNK + 1);
	CHECK(call(T_CLUNK, "4", 50u) == T_CLUNK + 1);

	/* Truncate through setattr. */
	CHECK(call(T_SETATTR, "4444488888", 2u, 0x8u, 0u, 0u, 0u, (uint64_t)5,
	    (uint64_t)0, (uint64_t)0, (uint64_t)0, (uint64_t)0) ==
	    T_SETATTR + 1);
	CHECK(call(T_GETATTR, "48", 2u, (uint64_t)0x7FF) == T_GETATTR + 1);
	rpos += 8 + 13 + 4 + 4 + 4 + 8 + 8;
	CHECK(get(8) == 5);

	/* --- directories ---------------------------------------------- */
	CHECK(call(T_MKDIR, "4s44", 0u, "sub", 0755u, 0u) == T_MKDIR + 1);
	CHECK(err(call(T_MKDIR, "4s44", 0u, "sub", 0755u, 0u)) == 17);
	CHECK(call(T_WALK, "442", 0u, 3u, 0u) == T_WALK + 1);
	CHECK(call(T_LOPEN, "44", 3u, 0u) == T_LOPEN + 1);
	CHECK(call(T_READDIR, "484", 3u, (uint64_t)0, 4096u) == T_READDIR + 1);
	n = (uint32_t)get(4);
	seen = 0;
	while (rpos < 11 + n) {
		uint16_t len;

		rpos += 13 + 8 + 1;		/* qid, offset, type */
		len = (uint16_t)get(2);
		if (len == 9 && memcmp(rep + rpos, "hello.txt", 9) == 0)
			seen |= 1;
		if (len == 3 && memcmp(rep + rpos, "sub", 3) == 0)
			seen |= 2;
		rpos += len;
	}
	CHECK(seen == 3);
	CHECK(call(T_CLUNK, "4", 3u) == T_CLUNK + 1);

	/* A partial walk reports how far it got; a failed first step fails. */
	CHECK(call(T_WALK, "442ss", 0u, 4u, 2u, "sub", "nope") == T_WALK + 1);
	CHECK(get(2) == 1);
	CHECK(err(call(T_WALK, "442s", 0u, 4u, 1u, "nope")) == 2);

	/* --- rename, and a fid that follows its file ------------------ */
	CHECK(call(T_RENAMEAT, "4s4s", 0u, "hello.txt", 0u, "moved.txt") ==
	    T_RENAMEAT + 1);
	CHECK(call(T_GETATTR, "48", 2u, (uint64_t)0x7FF) == T_GETATTR + 1);
	snprintf(path, sizeof(path), "%s/moved.txt", root);
	CHECK(access(path, F_OK) == 0);

	/* --- nothing gets out of the tree ----------------------------- */
	/* ".." from the root stays at the root. */
	CHECK(call(T_WALK, "442s", 0u, 5u, 1u, "..") == T_WALK + 1);
	CHECK(call(T_WALK, "442s", 5u, 6u, 1u, "moved.txt") == T_WALK + 1);
	CHECK(call(T_CLUNK, "4", 5u) == T_CLUNK + 1);
	CHECK(call(T_CLUNK, "4", 6u) == T_CLUNK + 1);
	/* A name is one component. */
	CHECK(err(call(T_WALK, "442s", 0u, 5u, 1u, "sub/../..")) != 0);
	CHECK(err(call(T_MKDIR, "4s44", 0u, "../escaped", 0755u, 0u)) != 0);
	/* A link out of the tree can be made and read, but not followed. */
	CHECK(call(T_SYMLINK, "4ss4", 0u, "out", outside, 0u) == T_SYMLINK + 1);
	CHECK(call(T_WALK, "442s", 0u, 5u, 1u, "out") == T_WALK + 1);
	CHECK(call(T_READLINK, "4", 5u) == T_READLINK + 1);
	CHECK(get(2) == strlen(outside));
	CHECK(err(call(T_LOPEN, "44", 5u, 0u)) == 40);		/* ELOOP */
	CHECK(err(call(T_WALK, "442s", 5u, 6u, 1u, "secret")) == 20);
	CHECK(call(T_WALK, "442ss", 0u, 6u, 2u, "out", "secret") == T_WALK + 1);
	CHECK(get(2) == 1);			/* stopped at the link */
	CHECK(call(T_CLUNK, "4", 5u) == T_CLUNK + 1);

	/* --- removal, and Linux's error numbers ----------------------- */
	CHECK(call(T_WALK, "442s", 0u, 4u, 1u, "sub") == T_WALK + 1);
	CHECK(call(T_LCREATE, "4s444", 4u, "inner", 0102u, 0644u, 0u) ==
	    T_LCREATE + 1);
	CHECK(err(call(T_UNLINKAT, "4s4", 0u, "sub", 0x200u)) == 39);
	CHECK(call(T_UNLINKAT, "4s4", 0u, "moved.txt", 0u) == T_UNLINKAT + 1);
	snprintf(path, sizeof(path), "%s/moved.txt", root);
	CHECK(access(path, F_OK) == -1);
	CHECK(call(T_STATFS, "4", 0u) == T_STATFS + 1);
	CHECK(get(4) == 0x01021997);
	CHECK(err(call(T_XATTRWALK, "44s", 0u, 9u, "user.x")) == 95);

	/* --- a guest that plays tricks with links --------------------- */
	/* A directory replaced by a link, under fids that were on it. */
	CHECK(call(T_MKDIR, "4s44", 0u, "d", 0755u, 0u) == T_MKDIR + 1);
	CHECK(call(T_WALK, "442s", 0u, 20u, 1u, "d") == T_WALK + 1);
	CHECK(call(T_WALK, "442s", 0u, 21u, 1u, "d") == T_WALK + 1);
	CHECK(call(T_UNLINKAT, "4s4", 0u, "d", 0x200u) == T_UNLINKAT + 1);
	CHECK(call(T_SYMLINK, "4ss4", 0u, "d", outside, 0u) == T_SYMLINK + 1);
	CHECK(err(call(T_LCREATE, "4s444", 20u, "planted", 0102u, 0644u,
	    0u)) != 0);
	CHECK(err(call(T_MKDIR, "4s44", 21u, "planted", 0755u, 0u)) != 0);
	CHECK(err(call(T_UNLINKAT, "4s4", 21u, "secret", 0u)) != 0);
	CHECK(err(call(T_WALK, "442s", 21u, 22u, 1u, "secret")) != 0);
	CHECK(err(call(T_LOPEN, "44", 21u, 0u)) != 0);
	CHECK(err(call(T_RENAMEAT, "4s4s", 21u, "secret", 0u, "mine")) != 0);
	snprintf(path, sizeof(path), "%s/planted", outside);
	CHECK(access(path, F_OK) == -1);
	snprintf(path, sizeof(path), "%s/secret", outside);
	CHECK(access(path, F_OK) == 0);

	/* A link to a file outside: not truncated, not given a second name. */
	CHECK(call(T_SYMLINK, "4ss4", 0u, "l", path, 0u) == T_SYMLINK + 1);
	CHECK(call(T_WALK, "442s", 0u, 23u, 1u, "l") == T_WALK + 1);
	CHECK(err(call(T_SETATTR, "4444488888", 23u, 0x8u, 0u, 0u, 0u,
	    (uint64_t)0, (uint64_t)0, (uint64_t)0, (uint64_t)0,
	    (uint64_t)0)) != 0);
	CHECK(stat(path, &sb) == 0 && sb.st_size == 6);
	CHECK(err(call(T_LINK, "44s", 0u, 23u, "hard")) == 1);	/* EPERM */
	snprintf(path, sizeof(path), "%s/hard", root);
	CHECK(access(path, F_OK) == -1);

	/* No set-user-ID bit, whatever is asked for. */
	CHECK(call(T_WALK, "442s", 0u, 24u, 1u, "sub") == T_WALK + 1);
	CHECK(call(T_LCREATE, "4s444", 24u, "tool", 0102u, 04755u, 0u) ==
	    T_LCREATE + 1);
	CHECK(call(T_SETATTR, "4444488888", 24u, 0x1u, 06755u, 0u, 0u,
	    (uint64_t)0, (uint64_t)0, (uint64_t)0, (uint64_t)0,
	    (uint64_t)0) == T_SETATTR + 1);
	snprintf(path, sizeof(path), "%s/sub/tool", root);
	CHECK(stat(path, &sb) == 0 && (sb.st_mode & 07777) == 0755);

	/* An open file is still that file once its name is gone. */
	CHECK(call(T_UNLINKAT, "4s4", 0u, "sub", 0u) != T_UNLINKAT + 1);
	CHECK(call(T_WALK, "442s", 0u, 25u, 1u, "sub") == T_WALK + 1);
	CHECK(call(T_UNLINKAT, "4s4", 25u, "tool", 0u) == T_UNLINKAT + 1);
	CHECK(call(T_GETATTR, "48", 24u, (uint64_t)0x7FF) == T_GETATTR + 1);
	CHECK(call(T_SETATTR, "4444488888", 24u, 0x8u, 0u, 0u, 0u,
	    (uint64_t)100, (uint64_t)0, (uint64_t)0, (uint64_t)0,
	    (uint64_t)0) == T_SETATTR + 1);
	/* And walking a fid onto itself leaves it open. */
	CHECK(call(T_WALK, "442", 24u, 24u, 0u) == T_WALK + 1);
	CHECK(call(T_READ, "484", 24u, (uint64_t)0, 10u) == T_READ + 1);
	CHECK(get(4) == 10);

	/* A pipe is refused, not waited on. */
	snprintf(path, sizeof(path), "%s/pipe", root);
	CHECK(mkfifo(path, 0644) == 0);
	CHECK(call(T_WALK, "442s", 0u, 26u, 1u, "pipe") == T_WALK + 1);
	CHECK(err(call(T_LOPEN, "44", 26u, 0u)) != 0);

	/* A directory read with no room for one entry says so. */
	CHECK(call(T_WALK, "442", 0u, 27u, 0u) == T_WALK + 1);
	CHECK(call(T_LOPEN, "44", 27u, 0u) == T_LOPEN + 1);
	CHECK(err(call(T_READDIR, "484", 27u, (uint64_t)0, 20u)) == 22);

	/* Fids cannot be made without end. */
	for (n = 0; n < 300000; n++) {
		if (call(T_WALK, "442", 0u, 1000u + n, 0u) != T_WALK + 1)
			break;
	}
	CHECK(n > 1000 && n < 300000);

	/* --- garbage does not crash it -------------------------------- */
	memset(req, 0xFF, 64);
	CHECK(p9_request(srv, req, 3, rep, sizeof(rep)) == 0);
	req[0] = 64; req[1] = req[2] = req[3] = 0;
	req[4] = T_WALK;
	CHECK(p9_request(srv, req, 64, rep, sizeof(rep)) >= 7);
	CHECK(rep[4] == 7);
	CHECK(err(call(T_READ, "484", 77u, (uint64_t)0, 10u)) == 9);

	p9_free(srv);
	snprintf(buf, sizeof(buf), "rm -rf '%s' '%s'", root, outside);
	(void)system(buf);
	printf("p9_test: %d checks passed\n", checks);
	return 0;
}
