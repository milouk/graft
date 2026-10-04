/*
 * p9.h — a 9P2000.L file server for one directory tree. See p9.c.
 */
#ifndef _GRAFT_P9_H_
#define _GRAFT_P9_H_

#include <stddef.h>
#include <stdint.h>

struct p9;

/* The largest message it will exchange, whatever the client asks for. */
#define P9_MSIZE_MAX	(256 * 1024 + 24)

/* Serve the tree at 'root'. NULL if it is not a directory. */
struct p9 *p9_new(const char *root);
void	p9_free(struct p9 *);

/*
 * Answer one request. 'req' holds a complete T-message; the R-message is
 * written to 'rep', which has room for 'cap' bytes. Returns the length of
 * the reply, or 0 if the request was too malformed to answer at all.
 */
size_t	p9_request(struct p9 *, const uint8_t *req, size_t len, uint8_t *rep,
	    size_t cap);

#endif /* _GRAFT_P9_H_ */
