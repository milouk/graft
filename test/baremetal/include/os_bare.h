/*
 * os_bare.h — what the bare-metal test kernel provides in place of a host
 * kernel's headers. Included from port/nvmm_port.h when not building for macOS.
 */
#ifndef _OS_BARE_H_
#define _OS_BARE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef int		pid_t;
typedef int64_t		time_t;
typedef unsigned int	u_int;
typedef unsigned long	u_long;
typedef unsigned char	u_char;

#define EPERM		1
#define ENOENT		2
#define ENXIO		6
#define ENOMEM		12
#define EFAULT		14
#define EBUSY		16
#define EEXIST		17
#define EINVAL		22
#define ENOTSUP		45
#define ENOBUFS		55
#define ERESTART	(-1)

#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define PROT_EXEC	0x4

void *	memcpy(void *, const void *, size_t);
void *	memmove(void *, const void *, size_t);
void *	memset(void *, int, size_t);
int	memcmp(const void *, const void *, size_t);
size_t	strlen(const char *);

#ifndef NULL
#define NULL		((void *)0)
#endif
#ifndef offsetof
#define offsetof(t, m)	__builtin_offsetof(t, m)
#endif

#endif /* _OS_BARE_H_ */
