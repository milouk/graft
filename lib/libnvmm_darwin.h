/*
 * libnvmm_darwin.h — the handful of BSD libc and <sys/cdefs.h> conveniences
 * libnvmm uses that macOS does not have.
 */
#ifndef _LIBNVMM_DARWIN_H_
#define _LIBNVMM_DARWIN_H_

#ifndef __predict_true
#define __predict_true(x)	__builtin_expect(!!(x), 1)
#endif
#ifndef __predict_false
#define __predict_false(x)	__builtin_expect(!!(x), 0)
#endif
#ifndef __arraycount
#define __arraycount(a)		(sizeof(a) / sizeof((a)[0]))
#endif
#ifndef __unreachable
#define __unreachable()		__builtin_unreachable()
#endif
#ifndef __dead
#define __dead			__attribute__((__noreturn__))
#endif

#endif /* _LIBNVMM_DARWIN_H_ */
