/*
 * p9-serve.c — vmm/p9.c behind a TCP socket, for testing it against the
 * real client: Linux mounts it with "-t 9p -o trans=tcp". In graft-run the
 * same server sits behind a virtio device instead.
 *
 *   p9-serve <port> <directory>
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p9.h"

static int
read_all(int fd, uint8_t *buf, size_t len)
{
	size_t done = 0;
	ssize_t n;

	while (done < len) {
		n = read(fd, buf + done, len - done);
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	static uint8_t req[P9_MSIZE_MAX], rep[P9_MSIZE_MAX];
	struct sockaddr_in sin;
	struct p9 *srv;
	int ls, fd, one = 1;

	if (argc != 3) {
		fprintf(stderr, "usage: p9-serve <port> <directory>\n");
		return 2;
	}
	srv = p9_new(argv[2]);
	if (srv == NULL) {
		fprintf(stderr, "%s: not a directory\n", argv[2]);
		return 1;
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)atoi(argv[1]));
	ls = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (ls == -1 || bind(ls, (struct sockaddr *)&sin, sizeof(sin)) == -1 ||
	    listen(ls, 1) == -1) {
		perror("listen");
		return 1;
	}

	while ((fd = accept(ls, NULL, NULL)) != -1) {
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		for (;;) {
			uint32_t size;
			size_t n;

			if (read_all(fd, req, 4) == -1)
				break;
			size = req[0] | req[1] << 8 | req[2] << 16 |
			    (uint32_t)req[3] << 24;
			if (size < 7 || size > sizeof(req) ||
			    read_all(fd, req + 4, size - 4) == -1)
				break;
			n = p9_request(srv, req, size, rep, sizeof(rep));
			if (n == 0 || write(fd, rep, n) != (ssize_t)n)
				break;
		}
		close(fd);
	}
	return 0;
}
